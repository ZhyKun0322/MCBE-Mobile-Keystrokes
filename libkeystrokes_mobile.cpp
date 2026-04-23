#include <jni.h>
#include <android/log.h>
#include <android/input.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <inttypes.h>
#include <cstddef>
#include <vector>

#include "pl/Hook.h"
#include "pl/Gloss.h"
#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"
#include "entt/entt.hpp"

#define LOG_TAG "Keystrokes"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)

#define VERSION "1.8.0"

// ══════════════════════════════════════════════════════════════════════════
//  Offsets — normalTick is fallback only now; preloader path is primary
// ══════════════════════════════════════════════════════════════════════════
static const uintptr_t g_normalTickOff = 0xa88e170;
static const uintptr_t ENTITY_CTX_OFF  = 0x10;

// ══════════════════════════════════════════════════════════════════════════
//  MoveInputComponent
// ══════════════════════════════════════════════════════════════════════════
struct MoveInputComponent {
    uint8_t  mInputState[0x10];
    uint8_t  mRawInputState[0x10];
    uint8_t  mHoldAutoJumpInWaterTicks;
    uint8_t  _pad[3];
    float    mMove_x;
    float    mMove_y;
    float    mLookDelta_x;
    float    mLookDelta_y;
    float    mInteractDir_x;
    float    mInteractDir_y;
    float    mDisplacement_x;
    float    mDisplacement_y;
    float    mDisplacement_z;
    float    mDisplacementDelta_x;
    float    mDisplacementDelta_y;
    float    mDisplacementDelta_z;
    float    mCameraOrientation_x;
    float    mCameraOrientation_y;
    float    mCameraOrientation_z;
    uint16_t mFlagValues;
    bool     mIsPaddling[2];
};

// ══════════════════════════════════════════════════════════════════════════
//  entt types
// ══════════════════════════════════════════════════════════════════════════
using McEntity   = entt::entity;
using McRegistry = entt::basic_registry<McEntity>;

struct McEntityCtx {
    McRegistry* registry;
    McEntity    entity;
};

// ══════════════════════════════════════════════════════════════════════════
//  Memory validation
// ══════════════════════════════════════════════════════════════════════════
struct MemRgn { uintptr_t s, e; };
static std::vector<MemRgn> g_rgns;
static std::mutex          g_rgns_mtx;
static int                 g_rgns_frame = 0;
static uint32_t            g_rgns_gen   = 0;
static const int           RGNS_REFRESH = 120;

static void refresh_rgns() {
    std::lock_guard<std::mutex> lk(g_rgns_mtx);
    g_rgns.clear();
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        uintptr_t s = 0, e = 0; char p[8] = {};
        if (sscanf(line, "%" PRIxPTR "-%" PRIxPTR " %4s", &s, &e, p) == 3 && p[0] == 'r')
            g_rgns.push_back({s, e});
    }
    fclose(f);
    ++g_rgns_gen;
}

static void maybe_refresh_rgns() {
    if (++g_rgns_frame < RGNS_REFRESH) return;
    g_rgns_frame = 0;
    refresh_rgns();
}

static bool readable(uintptr_t addr, size_t sz = 8) {
    for (auto& r : g_rgns)
        if (addr >= r.s && addr + sz <= r.e) return true;
    return false;
}

static inline bool sane(uintptr_t p) {
    return p > 0x10000ULL && p < 0x800000000000ULL;
}

// ══════════════════════════════════════════════════════════════════════════
//  PATH A — GetPreloaderInput (libpreloader.so)
//  This is the PRIMARY path. It reads key state directly from the preloader
//  without needing any normalTick offset or MoveInputComponent scanning.
//  The preloader input struct layout (standard across MC BE preloader builds):
//    bool forward  [0x00]  — W
//    bool backward [0x01]  — S
//    bool left     [0x02]  — A
//    bool right    [0x03]  — D
//    bool jump     [0x04]  — Space
//    bool sneak    [0x05]
//    bool sprint   [0x06]
// ══════════════════════════════════════════════════════════════════════════
struct PreloaderInput {
    bool forward;   // W
    bool backward;  // S
    bool left;      // A
    bool right;     // D
    bool jump;      // Space
    bool sneak;
    bool sprint;
};

typedef PreloaderInput* (*fn_GetPreloaderInput_t)();
static fn_GetPreloaderInput_t g_GetPreloaderInput = nullptr;
static bool                   g_preloaderTried    = false;
static bool                   g_preloaderOK       = false;

static bool initPreloaderPath() {
    if (g_preloaderTried) return g_preloaderOK;
    g_preloaderTried = true;
    GHandle h = GlossOpen("libpreloader.so");
    if (!h) { LOGW("preloader: libpreloader.so not found"); return false; }
    void* sym = (void*)GlossSymbol(h, "GetPreloaderInput", nullptr);
    if (!sym) { LOGW("preloader: GetPreloaderInput not found"); return false; }
    g_GetPreloaderInput = reinterpret_cast<fn_GetPreloaderInput_t>(sym);
    g_preloaderOK = true;
    LOGI("preloader: GetPreloaderInput found OK");
    return true;
}

// ══════════════════════════════════════════════════════════════════════════
//  PATH B — normalTick hook + MoveInputComponent (fallback)
// ══════════════════════════════════════════════════════════════════════════
static void*      g_localPlayer = nullptr;
static int        g_lpFailCount = 0;
static const int  LP_FAIL_LIMIT = 60;
static std::mutex g_playerMtx;

using fn_normalTick_t = void(*)(void*);
static fn_normalTick_t orig_normalTick = nullptr;

static void hook_normalTick(void* thiz) {
    { std::lock_guard<std::mutex> lk(g_playerMtx); g_localPlayer = thiz; }
    orig_normalTick(thiz);
}

static MoveInputComponent* getMoveInput_entt(void* lp) {
    uintptr_t base = reinterpret_cast<uintptr_t>(lp);
    if (!sane(base + ENTITY_CTX_OFF)) return nullptr;
    auto* ctx = reinterpret_cast<McEntityCtx*>(base + ENTITY_CTX_OFF);
    McRegistry* reg = ctx->registry;
    if (!reg || !sane(reinterpret_cast<uintptr_t>(reg))) return nullptr;
    if (!readable(reinterpret_cast<uintptr_t>(reg), 64)) return nullptr;
    McEntity ent = ctx->entity;
    if (ent == entt::null) return nullptr;
    if (!reg->valid(ent)) return nullptr;
    return reg->try_get<MoveInputComponent>(ent);
}

static int      g_mic_off     = -1;
static bool     g_mic_found   = false;
static uint32_t g_mic_refresh = 0;

static bool mic_valid(uintptr_t ptr) {
    if (!readable(ptr, sizeof(MoveInputComponent))) return false;
    auto* m = reinterpret_cast<MoveInputComponent*>(ptr);
    if (!std::isfinite(m->mMove_x) || fabsf(m->mMove_x) > 2.0f) return false;
    if (!std::isfinite(m->mMove_y) || fabsf(m->mMove_y) > 2.0f) return false;
    if (!std::isfinite(m->mLookDelta_x) || fabsf(m->mLookDelta_x) > 20.f) return false;
    if (!std::isfinite(m->mLookDelta_y) || fabsf(m->mLookDelta_y) > 20.f) return false;
    if (m->mFlagValues > 0x7FF) return false;
    return true;
}

static MoveInputComponent* getMoveInput_scan(void* lp) {
    uintptr_t base = reinterpret_cast<uintptr_t>(lp);
    if (g_mic_found && g_mic_refresh != g_rgns_gen) {
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(base + g_mic_off);
        if (!mic_valid(ptr)) { g_mic_found = false; g_mic_off = -1; }
        else g_mic_refresh = g_rgns_gen;
    }
    if (g_mic_found) {
        if (!readable(base + g_mic_off, 8)) { g_mic_found = false; return nullptr; }
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(base + g_mic_off);
        if (mic_valid(ptr)) return reinterpret_cast<MoveInputComponent*>(ptr);
        g_mic_found = false; g_mic_off = -1;
    }
    if (!readable(base, 0x800)) return nullptr;
    for (int off = 0; off < 0x800; off += 8) {
        if (!readable(base + off, 8)) continue;
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(base + off);
        if (!sane(ptr)) continue;
        if (mic_valid(ptr)) {
            g_mic_off = off; g_mic_found = true; g_mic_refresh = g_rgns_gen;
            LOGI("MIC scanner LP+0x%x move=(%f,%f)", off,
                 reinterpret_cast<MoveInputComponent*>(ptr)->mMove_x,
                 reinterpret_cast<MoveInputComponent*>(ptr)->mMove_y);
            return reinterpret_cast<MoveInputComponent*>(ptr);
        }
    }
    return nullptr;
}

static MoveInputComponent* getMoveInput(void* lp) {
    if (!lp) return nullptr;
    MoveInputComponent* mic = getMoveInput_entt(lp);
    if (mic) return mic;
    return getMoveInput_scan(lp);
}

// ══════════════════════════════════════════════════════════════════════════
//  Key state
// ══════════════════════════════════════════════════════════════════════════
struct KeyState { bool w, a, s, d, space, lmb, rmb; };
static KeyState   g_keys = {};
static std::mutex g_keymutex;

static void updateKeysFromPlayer() {
    // ── PATH A: GetPreloaderInput (no offset needed, works on any MC version) ──
    if (g_preloaderOK && g_GetPreloaderInput) {
        PreloaderInput* pi = g_GetPreloaderInput();
        if (pi && sane(reinterpret_cast<uintptr_t>(pi))) {
            std::lock_guard<std::mutex> lk(g_keymutex);
            g_keys.w     = pi->forward;
            g_keys.s     = pi->backward;
            g_keys.a     = pi->left;
            g_keys.d     = pi->right;
            g_keys.space = pi->jump;
            return;
        }
    }

    // ── PATH B: normalTick hook + MoveInputComponent ──
    void* lp;
    { std::lock_guard<std::mutex> lk(g_playerMtx); lp = g_localPlayer; }
    if (!lp) return;

    MoveInputComponent* mic = getMoveInput(lp);
    if (!mic) {
        if (++g_lpFailCount >= LP_FAIL_LIMIT) {
            std::lock_guard<std::mutex> lk(g_playerMtx);
            g_localPlayer = nullptr;
            g_lpFailCount = 0;
            g_mic_found   = false;
            g_mic_off     = -1;
            LOGW("LocalPlayer cleared after %d failures", LP_FAIL_LIMIT);
        }
        return;
    }
    g_lpFailCount = 0;

    float mx   = mic->mMove_x;
    float my   = mic->mMove_y;
    bool  jump = (mic->mInputState[4] != 0) || ((mic->mFlagValues & 0x1) != 0);

    std::lock_guard<std::mutex> lk(g_keymutex);
    g_keys.w     = (my >  0.1f);
    g_keys.s     = (my < -0.1f);
    g_keys.a     = (mx < -0.1f);
    g_keys.d     = (mx >  0.1f);
    g_keys.space = jump;
}

// ══════════════════════════════════════════════════════════════════════════
//  Touch queue
// ══════════════════════════════════════════════════════════════════════════
struct TouchEvt { float x, y, dy; int32_t action; };
static constexpr int TQSZ = 16;
static TouchEvt   g_tq[TQSZ];
static int        g_tqh = 0, g_tqt = 0;
static std::mutex g_tqmtx;

static void tq_push(float x, float y, int32_t action, float dy = 0.f) {
    std::lock_guard<std::mutex> lk(g_tqmtx);
    int nxt = (g_tqh + 1) % TQSZ;
    if (nxt != g_tqt) { g_tq[g_tqh] = {x, y, dy, action}; g_tqh = nxt; }
}

static void tq_drain() {
    std::lock_guard<std::mutex> lk(g_tqmtx);
    ImGuiIO& io = ImGui::GetIO();
    while (g_tqt != g_tqh) {
        const auto& e = g_tq[g_tqt]; g_tqt = (g_tqt + 1) % TQSZ;
        switch (e.action) {
            case AMOTION_EVENT_ACTION_DOWN:
                io.MousePos = ImVec2(e.x, e.y); io.MouseDown[0] = true; break;
            case AMOTION_EVENT_ACTION_MOVE:
                io.MousePos = ImVec2(e.x, e.y); io.MouseWheel += e.dy; break;
            default:
                io.MouseDown[0] = false; break;
        }
    }
}

// ══════════════════════════════════════════════════════════════════════════
//  Globals
// ══════════════════════════════════════════════════════════════════════════
static bool       g_initialized = false;
static int        g_width = 0, g_height = 0;
static float      g_uiscale = 1.0f;
static EGLContext g_targetcontext = EGL_NO_CONTEXT;
static EGLSurface g_targetsurface = EGL_NO_SURFACE;
static float      g_keysize = 50.0f;
static float      g_opacity = 1.0f;
static float      g_rounding = 8.0f;
static bool       g_locked = false;
static bool       g_showsettings = false;
static ImVec2     g_hudpos = ImVec2(100, 100);
static bool       g_posloaded = false;
static uintptr_t  g_mcBase = 0;
static bool       g_baseFound = false;

static void findMcBase() {
    if (g_baseFound) return;
    FILE* f = fopen("/proc/self/maps", "r"); if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "libminecraftpe.so")) {
            uintptr_t s = 0, e = 0;
            sscanf(line, "%" PRIxPTR "-%" PRIxPTR, &s, &e);
            if (!g_mcBase) { g_mcBase = s; g_baseFound = true; LOGI("mc base=0x%" PRIxPTR, g_mcBase); }
        }
    }
    fclose(f);
}

// ── Save / load ───────────────────────────────────────────────────────────
static const char* SAVE_PATHS[] = {
    "/data/data/com.mojang.minecraftpe/files/keystrokes.cfg",
    "/data/data/com.mojang.minecraftpe.preview/files/keystrokes.cfg",
    nullptr
};

static const char* getsavepath() {
    for (int i = 0; SAVE_PATHS[i]; i++) {
        FILE* f = fopen(SAVE_PATHS[i], "r"); if (f) { fclose(f); return SAVE_PATHS[i]; }
        f = fopen(SAVE_PATHS[i], "a"); if (f) { fclose(f); return SAVE_PATHS[i]; }
    }
    return SAVE_PATHS[0];
}

static void savecfg() {
    FILE* f = fopen(getsavepath(), "w"); if (!f) return;
    fprintf(f, "%f %f %f %f %d %f\n", g_hudpos.x, g_hudpos.y, g_keysize, g_opacity, (int)g_locked, g_rounding);
    fclose(f);
}

static void loadcfg() {
    if (g_posloaded) return; g_posloaded = true;
    for (int i = 0; SAVE_PATHS[i]; i++) {
        FILE* f = fopen(SAVE_PATHS[i], "r"); if (!f) continue;
        int locked = 0;
        int n = fscanf(f, "%f %f %f %f %d %f", &g_hudpos.x, &g_hudpos.y, &g_keysize, &g_opacity, &locked, &g_rounding);
        fclose(f);
        if (n >= 5) {
            g_locked = locked != 0;
            g_keysize = std::max(30.f, std::min(g_keysize, 120.f));
            g_opacity = std::max(0.1f, std::min(g_opacity, 1.0f));
            g_rounding = std::max(0.0f, std::min(g_rounding, 50.f));
            return;
        }
    }
}

// ── InputConsumer variants ────────────────────────────────────────────────
static const char* consume_syms[] = {
    "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventEb",
    "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE",
    "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEbxPjPPNS_10InputEventE",
    "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEbxPPNS_10InputEventEPj",
    "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEbxPPNS_10InputEventEPjb",
    nullptr
};

static int g_consume_variant = -1;
typedef int32_t (*cfn0)(void*,void*,bool,long,     uint32_t*,AInputEvent**,bool);
typedef int32_t (*cfn1)(void*,void*,bool,long,     uint32_t*,AInputEvent**);
typedef int32_t (*cfn2)(void*,void*,bool,long long,uint32_t*,AInputEvent**);
typedef int32_t (*cfn3)(void*,void*,bool,long long,AInputEvent**,uint32_t*);
typedef int32_t (*cfn4)(void*,void*,bool,long long,AInputEvent**,uint32_t*,bool);
static cfn0 orig0=nullptr; static cfn1 orig1=nullptr;
static cfn2 orig2=nullptr; static cfn3 orig3=nullptr; static cfn4 orig4=nullptr;
static EGLBoolean (*orig_swap)(EGLDisplay,EGLSurface) = nullptr;

// ── CPS / time ────────────────────────────────────────────────────────────
static double nowsec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct CpsTracker {
    static const int N=64; double t[N]={}; int h=0,c=0;
    void click(){t[h]=nowsec();h=(h+1)%N;if(c<N)c++;}
    int get(){double cut=nowsec()-1.0;int n=0;
        for(int i=0;i<c;i++){if(t[(h-1-i+N)%N]>=cut)n++;else break;}return n;}
};

static CpsTracker g_lc, g_rc;
static bool  g_pl=false, g_pr=false;
static float g_lasty=0.0f;
static bool  g_td=false;

// ── processinput ──────────────────────────────────────────────────────────
static void processinput(AInputEvent* ev) {
    int32_t type = AInputEvent_getType(ev);
    if (type == AINPUT_EVENT_TYPE_MOTION) {
        int32_t act = AMotionEvent_getAction(ev)&AMOTION_EVENT_ACTION_MASK;
        int32_t btn = AMotionEvent_getButtonState(ev);
        bool nl=(btn&AMOTION_EVENT_BUTTON_PRIMARY)!=0;
        bool nr=(btn&AMOTION_EVENT_BUTTON_SECONDARY)!=0;

        {
            std::lock_guard<std::mutex> lk(g_keymutex);
            if(nl&&!g_pl)g_lc.click(); if(nr&&!g_pr)g_rc.click();
            g_pl=nl; g_pr=nr; g_keys.lmb=nl; g_keys.rmb=nr;
        }

        if(g_initialized&&g_showsettings){
            float tx=AMotionEvent_getX(ev,0),ty=AMotionEvent_getY(ev,0);
            if(act==AMOTION_EVENT_ACTION_DOWN){
                g_lasty=ty;g_td=true;tq_push(tx,ty,AMOTION_EVENT_ACTION_DOWN);
            }else if(act==AMOTION_EVENT_ACTION_MOVE&&g_td){
                float dy=ty-g_lasty;g_lasty=ty;
                tq_push(tx,ty,AMOTION_EVENT_ACTION_MOVE,dy*-0.06f);
            }else if(act==AMOTION_EVENT_ACTION_UP||act==AMOTION_EVENT_ACTION_CANCEL){
                g_td=false;tq_push(tx,ty,AMOTION_EVENT_ACTION_UP);
            }
        }
    }
}

static int32_t hc0(void*t,void*a,bool b,long c,uint32_t*d,AInputEvent**e,bool f){
    int32_t r=orig0?orig0(t,a,b,c,d,e,f):0;if(r==0&&e&&*e)processinput(*e);return r;}
static int32_t hc1(void*t,void*a,bool b,long c,uint32_t*d,AInputEvent**e){
    int32_t r=orig1?orig1(t,a,b,c,d,e):0;if(r==0&&e&&*e)processinput(*e);return r;}
static int32_t hc2(void*t,void*a,bool b,long long c,uint32_t*d,AInputEvent**e){
    int32_t r=orig2?orig2(t,a,b,c,d,e):0;if(r==0&&e&&*e)processinput(*e);return r;}
static int32_t hc3(void*t,void*a,bool b,long long c,AInputEvent**e,uint32_t*d){
    int32_t r=orig3?orig3(t,a,b,c,e,d):0;if(r==0&&e&&*e)processinput(*e);return r;}
static int32_t hc4(void*t,void*a,bool b,long long c,AInputEvent**e,uint32_t*d,bool f){
    int32_t r=orig4?orig4(t,a,b,c,e,d,f):0;if(r==0&&e&&*e)processinput(*e);return r;}

// ── GL state save/restore ─────────────────────────────────────────────────
struct glst{GLint prog,tex,atex,abuf,ebuf,vao,fbo,vp[4],sc[4],bsrc,bdst;
            GLboolean blend,cull,depth,scissor;};
static void sgl(glst&s){
    glGetIntegerv(GL_CURRENT_PROGRAM,&s.prog);glGetIntegerv(GL_TEXTURE_BINDING_2D,&s.tex);
    glGetIntegerv(GL_ACTIVE_TEXTURE,&s.atex); glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&s.abuf);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING,&s.ebuf);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&s.vao);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING,&s.fbo);glGetIntegerv(GL_VIEWPORT,s.vp);
    glGetIntegerv(GL_SCISSOR_BOX,s.sc);
    glGetIntegerv(GL_BLEND_SRC_ALPHA,&s.bsrc);glGetIntegerv(GL_BLEND_DST_ALPHA,&s.bdst);
    s.blend=glIsEnabled(GL_BLEND);s.cull=glIsEnabled(GL_CULL_FACE);
    s.depth=glIsEnabled(GL_DEPTH_TEST);s.scissor=glIsEnabled(GL_SCISSOR_TEST);
}
static void rgl(const glst&s){
    glUseProgram(s.prog);glActiveTexture(s.atex);glBindTexture(GL_TEXTURE_2D,s.tex);
    glBindBuffer(GL_ARRAY_BUFFER,s.abuf);glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,s.ebuf);
    glBindVertexArray(s.vao);glBindFramebuffer(GL_FRAMEBUFFER,s.fbo);
    glViewport(s.vp[0],s.vp[1],s.vp[2],s.vp[3]);
    glScissor(s.sc[0],s.sc[1],s.sc[2],s.sc[3]);glBlendFunc(s.bsrc,s.bdst);
    s.blend  ?glEnable(GL_BLEND)       :glDisable(GL_BLEND);
    s.cull   ?glEnable(GL_CULL_FACE)   :glDisable(GL_CULL_FACE);
    s.depth  ?glEnable(GL_DEPTH_TEST)  :glDisable(GL_DEPTH_TEST);
    s.scissor?glEnable(GL_SCISSOR_TEST):glDisable(GL_SCISSOR_TEST);
}

// ── Key drawing ───────────────────────────────────────────────────────────
static void drawkey(const char* lbl,bool on,ImVec2 sz){
    float a=g_opacity;
    ImVec4 bg=on?ImVec4(.85f,.85f,.85f,.95f*a):ImVec4(.18f,.20f,.22f,.88f*a);
    ImVec4 fg=on?ImVec4(.05f,.05f,.05f,a)     :ImVec4(.90f,.90f,.90f,a);
    ImGui::PushStyleColor(ImGuiCol_Button,bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive
