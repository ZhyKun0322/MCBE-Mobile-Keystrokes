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

#define VERSION "1.7.0-probe"

// ══════════════════════════════════════════════════════════════════════════
//  Offsets  (ADJUST THESE)
// ══════════════════════════════════════════════════════════════════════════

// TRY THESE ONE BY ONE:
// 0xa88e170 - most likely normalTick
// 0xa890bd4 - candidate 2  
// 0xa8934ec - candidate 3
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
static uint32_t            g_rgns_gen = 0;
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
//  LocalPlayer pointer
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

// ══════════════════════════════════════════════════════════════════════════
//  MoveInput access
// ══════════════════════════════════════════════════════════════════════════
static MoveInputComponent* getMoveInput_entt(void* lp) {
    uintptr_t base = reinterpret_cast<uintptr_t>(lp);
    if (!sane(base + ENTITY_CTX_OFF)) return nullptr;

    auto* ctx = reinterpret_cast<McEntityCtx*>(base + ENTITY_CTX_OFF);
    if (!sane(reinterpret_cast<uintptr_t>(ctx))) return nullptr;

    McRegistry* reg = ctx->registry;
    if (!reg || !sane(reinterpret_cast<uintptr_t>(reg))) return nullptr;
    if (!readable(reinterpret_cast<uintptr_t>(reg), 64)) return nullptr;

    McEntity ent = ctx->entity;
    if (ent == entt::null) return nullptr;
    if (!reg->valid(ent)) return nullptr;

    return reg->try_get<MoveInputComponent>(ent);
}

static int      g_mic_off = -1;
static bool     g_mic_found = false;
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
    void* lp;
    { std::lock_guard<std::mutex> lk(g_playerMtx); lp = g_localPlayer; }
    if (!lp) return;

    MoveInputComponent* mic = getMoveInput(lp);
    if (!mic) {
        if (++g_lpFailCount >= LP_FAIL_LIMIT) {
            std::lock_guard<std::mutex> lk(g_playerMtx);
            g_localPlayer = nullptr;
            g_lpFailCount = 0;
            g_mic_found = false;
            g_mic_off = -1;
            LOGW("LocalPlayer cleared after %d failures", LP_FAIL_LIMIT);
        }
        return;
    }
    g_lpFailCount = 0;

    float mx = mic->mMove_x;
    float my = mic->mMove_y;
    bool jump = (mic->mInputState[4] != 0) || ((mic->mFlagValues & 0x1) != 0);

    std::lock_guard<std::mutex> lk(g_keymutex);
    g_keys.w = (my > 0.1f);
    g_keys.s = (my < -0.1f);
    g_keys.a = (mx < -0.1f);
    g_keys.d = (mx > 0.1f);
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
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,bg);
    ImGui::PushStyleColor(ImGuiCol_Text,fg);
    ImGui::Button(lbl,sz);ImGui::PopStyleColor(4);
}

static void drawkeycps(const char* lbl,bool on,ImVec2 sz,int cps){
    float a=g_opacity;
    ImVec4 bg=on?ImVec4(.85f,.85f,.85f,.95f*a):ImVec4(.18f,.20f,.22f,.88f*a);
    ImVec4 fg=on?ImVec4(.05f,.05f,.05f,a)     :ImVec4(.90f,.90f,.90f,a);
    ImGui::PushStyleColor(ImGuiCol_Button,bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,bg);
    ImVec2 pos=ImGui::GetCursorScreenPos();
    ImGui::Button("##ck",sz);
    ImDrawList* dl=ImGui::GetWindowDrawList();
    ImFont* fn=ImGui::GetFont();
    float fs=ImGui::GetFontSize(),sfs=fs*0.75f;
    ImVec2 lsz=fn->CalcTextSizeA(fs,FLT_MAX,0,lbl);
    char cb[16];snprintf(cb,16,"%d CPS",cps);
    ImVec2 csz=fn->CalcTextSizeA(sfs,FLT_MAX,0,cb);
    float bh=lsz.y+3+csz.y,bt=pos.y+(sz.y-bh)*0.5f;
    dl->AddText(fn,fs, ImVec2(pos.x+(sz.x-lsz.x)*0.5f,bt),
                ImGui::ColorConvertFloat4ToU32(fg),lbl);
    ImVec4 dim={fg.x,fg.y,fg.z,fg.w*0.7f};
    dl->AddText(fn,sfs,ImVec2(pos.x+(sz.x-csz.x)*0.5f,bt+lsz.y+3),
                ImGui::ColorConvertFloat4ToU32(dim),cb);
    ImGui::PopStyleColor(3);
}

// ── Settings panel ────────────────────────────────────────────────────────
static void drawsettings(ImVec2 hp){
    float sw=std::max(g_width*0.26f,220.f),sh=std::max(g_height*0.62f,340.f);
    float ks=g_keysize,sp=ks*0.04f,hw=ks*3+sp*2;
    float px=hp.x+hw+8,py=hp.y;
    if(px+sw>g_width) px=hp.x-sw-8;
    if(py+sh>g_height)py=g_height-sh-8;
    if(px<0)px=8;if(py<0)py=8;
    ImGui::SetNextWindowPos(ImVec2(px,py),ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(sw,sh),ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg,            ImVec4(.10f,.12f,.16f,.97f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg,             ImVec4(.16f,.20f,.28f,1));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,      ImVec4(.22f,.28f,.38f,1));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,          ImVec4(.35f,.65f,1,1));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive,    ImVec4(.50f,.80f,1,1));
    ImGui::PushStyleColor(ImGuiCol_CheckMark,           ImVec4(.35f,.65f,1,1));
    ImGui::PushStyleColor(ImGuiCol_Separator,           ImVec4(.25f,.32f,.45f,1));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg,         ImVec4(.08f,.10f,.14f,1));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab,       ImVec4(.30f,.50f,.80f,1));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered,ImVec4(.40f,.60f,.90f,1));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImVec4(.50f,.75f,1,1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,10);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(12,12));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  ImVec2(6,11));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6,4));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize,6);
    ImGui::Begin("##cfg",nullptr,
        ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove);
    float cw=sw-24;
    ImGui::TextColored(ImVec4(.85f,.92f,1,1),"KEYSTROKES  v" VERSION);
    ImGui::Spacing();ImGui::Separator();ImGui::Spacing();

    if(g_consume_variant>=0)
        ImGui::TextColored(ImVec4(.4f,.8f,.4f,1),"Input hook: variant %d",g_consume_variant);
    else
        ImGui::TextColored(ImVec4(1,.35f,.35f,1),"Input hook: NOT FOUND");

    if(orig_normalTick)
        ImGui::TextColored(ImVec4(.4f,.8f,.4f,1),"normalTick: hooked (0x%x)", (unsigned)g_normalTickOff);
    else if(g_normalTickOff==0)
        ImGui::TextColored(ImVec4(1,.7f,.1f,1),"normalTick: offset not set");
    else
        ImGui::TextColored(ImVec4(1,.35f,.35f,1),"normalTick: hook failed");

    {
        void* lp; {std::lock_guard<std::mutex> lk(g_playerMtx);lp=g_localPlayer;}
        if(lp) ImGui::TextColored(ImVec4(.4f,.8f,.4f,1),"Player: active");
        else   ImGui::TextColored(ImVec4(1,.7f,.1f,1),"Player: waiting...");
    }

    if(g_mic_found)
        ImGui::TextColored(ImVec4(.4f,.8f,.4f,1),"Scanner: LP+0x%x (fallback)",g_mic_off);
    else
        ImGui::TextColored(ImVec4(.55f,.62f,.75f,1),"Scanner: standby");

    ImGui::Spacing();ImGui::Separator();ImGui::Spacing();
    ImGui::TextColored(ImVec4(.5f,.75f,1,1),"KEY SIZE");
    ImGui::TextColored(ImVec4(.9f,.93f,1,1),"%.0f dp",g_keysize);
    ImGui::SetNextItemWidth(cw);
    if(ImGui::SliderFloat("##sz",&g_keysize,30,120,""))savecfg();
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(.5f,.75f,1,1),"OPACITY");
    float op=g_opacity*100;
    ImGui::TextColored(ImVec4(.9f,.93f,1,1),"%.0f%%",op);
    ImGui::SetNextItemWidth(cw);
    if(ImGui::SliderFloat("##op",&op,10,100,"")){ g_opacity=op/100;savecfg();}
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(.5f,.75f,1,1),"CORNER RADIUS");
    ImGui::TextColored(ImVec4(.9f,.93f,1,1),"%.0f dp",g_rounding);
    ImGui::SetNextItemWidth(cw);
    if(ImGui::SliderFloat("##rnd",&g_rounding,0,50,""))savecfg();
    ImGui::Spacing();ImGui::Separator();ImGui::Spacing();
    ImGui::TextColored(ImVec4(.5f,.75f,1,1),"LOCK POSITION");
    bool lk=g_locked;
    if(ImGui::Checkbox("##lk",&lk)){g_locked=lk;savecfg();}
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(.55f,.62f,.75f,1),"Prevent drag");
    ImGui::Spacing();ImGui::Separator();ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button,       ImVec4(.18f,.30f,.55f,1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(.25f,.42f,.72f,1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.35f,.55f,.90f,1));
    ImGui::PushStyleColor(ImGuiCol_Text,         ImVec4(.88f,.93f,1,1));
    if(ImGui::Button("Reset to Default",ImVec2(cw,0))){
        g_keysize=50;g_opacity=1;g_rounding=8;g_locked=false;
        g_hudpos=ImVec2(100,100);savecfg();
    }
    ImGui::PopStyleColor(4);
    float rem=ImGui::GetContentRegionAvail().y-ImGui::GetTextLineHeightWithSpacing()*2.5f;
    if(rem>0)ImGui::Dummy(ImVec2(0,rem));
    ImGui::Separator();ImGui::Spacing();
    ImGui::TextColored(ImVec4(.4f,.5f,.65f,1),"Made by");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(.5f,.75f,1,1),"ZhyKun");
    ImGui::End();
    ImGui::PopStyleVar(5);ImGui::PopStyleColor(11);
    ImGuiIO& io=ImGui::GetIO();
    if(ImGui::IsMouseClicked(0)&&
       (io.MousePos.x<px||io.MousePos.x>px+sw||
        io.MousePos.y<py||io.MousePos.y>py+sh))
        g_showsettings=false;
}

static bool   g_pressing=false;
static double g_pstart=0.0;
static const double LP_SEC=0.5;

static void drawmenu(){
    KeyState k;{std::lock_guard<std::mutex> lk(g_keymutex);k=g_keys;}
    int lc=g_lc.get(),rc=g_rc.get();
    float ks=g_keysize,sp=ks*0.04f,hw=ks*3+sp*2;
    float hh=ks*3+sp*2+ks*1.5f+sp+ks*0.7f+sp;
    ImGuiIO& io=ImGui::GetIO();
    bool inside=(io.MousePos.x>=g_hudpos.x&&io.MousePos.x<=g_hudpos.x+hw&&
                 io.MousePos.y>=g_hudpos.y&&io.MousePos.y<=g_hudpos.y+hh);
    if(inside&&io.MouseDown[0]&&!g_pressing&&!g_showsettings)
        {g_pressing=true;g_pstart=nowsec();}
    if(!io.MouseDown[0])g_pressing=false;
    if(g_pressing&&(nowsec()-g_pstart)>=LP_SEC){g_showsettings=true;g_pressing=false;}
    if(!g_locked&&!g_showsettings&&inside&&ImGui::IsMouseDragging(0)){
        g_hudpos.x+=io.MouseDelta.x;g_hudpos.y+=io.MouseDelta.y;
        g_hudpos.x=std::max(0.f,std::min(g_hudpos.x,(float)g_width-hw));
        g_hudpos.y=std::max(0.f,std::min(g_hudpos.y,(float)g_height-hh));
        savecfg();
    }
    if(g_showsettings)drawsettings(g_hudpos);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0,0));
    ImGui::SetNextWindowPos(g_hudpos,ImGuiCond_Always);
    ImGui::Begin("##ks",nullptr,
        ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoBackground|
        ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoInputs);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  ImVec2(sp,sp));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,g_rounding);
    ImGui::SetCursorPosX(ks+sp);
    drawkey("W",k.w,ImVec2(ks,ks));
    drawkey("A",k.a,ImVec2(ks,ks));ImGui::SameLine();
    drawkey("S",k.s,ImVec2(ks,ks));ImGui::SameLine();
    drawkey("D",k.d,ImVec2(ks,ks));
    float half=(hw-sp)*0.5f;
    drawkeycps("LMB",k.lmb,ImVec2(half,ks*1.5f),lc);ImGui::SameLine();
    drawkeycps("RMB",k.rmb,ImVec2(half,ks*1.5f),rc);
    drawkey("_____",k.space,ImVec2(hw,ks*0.7f));
    ImGui::PopStyleVar(3);ImGui::End();
}

static void setup(){
    if(g_initialized||g_width<=0||g_height<=0)return;
    loadcfg();
    ImGui::CreateContext();
    ImGuiIO& io=ImGui::GetIO();io.IniFilename=nullptr;
    int mn=std::min(g_width,g_height),mx=std::max(g_width,g_height);
    float dp=std::max(0.85f,std::min((float)mn/480.f,2.8f));
    if(mx>2400)dp=std::min(dp*1.15f,2.8f);
    g_uiscale=dp;
    ImFontConfig fc;fc.SizePixels=std::max(14.f,15.f*dp);
    io.Fonts->AddFontDefault(&fc);
    ImGui_ImplAndroid_Init();
    ImGui_ImplOpenGL3_Init("#version 300 es");
    ImGuiStyle& st=ImGui::GetStyle();
    st.ScaleAllSizes(dp);st.WindowBorderSize=0;
    float ks=g_keysize,sp=ks*0.04f,hw=ks*3+sp*2,hh=ks*4+sp*3;
    g_hudpos.x=std::max(0.f,std::min(g_hudpos.x,(float)g_width-hw));
    g_hudpos.y=std::max(0.f,std::min(g_hudpos.y,(float)g_height-hh));
    refresh_rgns();
    g_initialized=true;
}

static void render(){
    if(!g_initialized)return;
    glst s;sgl(s);
    maybe_refresh_rgns();
    ImGuiIO& io=ImGui::GetIO();
    io.DisplaySize=ImVec2((float)g_width,(float)g_height);
    tq_drain();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_width,g_height);
    ImGui::NewFrame();
    updateKeysFromPlayer();
    drawmenu();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    rgl(s);
}

static EGLBoolean hook_swap(EGLDisplay dpy,EGLSurface surf){
    EGLContext ctx=eglGetCurrentContext();
    if(ctx==EGL_NO_CONTEXT)return orig_swap(dpy,surf);
    EGLint w=0,h=0;
    eglQuerySurface(dpy,surf,EGL_WIDTH,&w);
    eglQuerySurface(dpy,surf,EGL_HEIGHT,&h);
    if(w<500||h<500)return orig_swap(dpy,surf);
    if(g_targetcontext==EGL_NO_CONTEXT){g_targetcontext=ctx;g_targetsurface=surf;}
    if(ctx==g_targetcontext&&surf==g_targetsurface){g_width=w;g_height=h;setup();render();}
    return orig_swap(dpy,surf);
}

static int g_currentProbe = -1;
static uintptr_t g_probeAddrs[] = {
    0xa88e170,
    0xa890bd4,
    0xa8934ec,
    0
};

static void (*probe_origs[4])(void*) = {};

static void probe_hook(void* thiz) {
    uintptr_t base = reinterpret_cast<uintptr_t>(thiz);
    bool isLocalPlayer = false;
    
    if (base > 0x10000000ULL && base < 0x800000000000ULL) {
        if (readable(base + ENTITY_CTX_OFF, 16)) {
            auto* ctx = reinterpret_cast<McEntityCtx*>(base + ENTITY_CTX_OFF);
            if (sane(reinterpret_cast<uintptr_t>(ctx)) && 
                sane(reinterpret_cast<uintptr_t>(ctx->registry)) &&
                readable(reinterpret_cast<uintptr_t>(ctx->registry), 64) &&
                ctx->registry->valid(ctx->entity)) {
                isLocalPlayer = true;
            }
        }
    }
    
    if (isLocalPlayer) {
        LOGI("PROBE[%d] 0x%x: VALID LocalPlayer thiz=%p", 
             g_currentProbe, g_probeAddrs[g_currentProbe], thiz);
    }
    
    probe_origs[g_currentProbe](thiz);
}

static void* probe_thread(void*) {
    sleep(10);
    findMcBase();
    if (!g_baseFound) { LOGW("probe: mc base not found"); return nullptr; }
    
    for (int i = 0; g_probeAddrs[i]; i++) {
        g_currentProbe = i;
        void* sym = reinterpret_cast<void*>(g_mcBase + g_probeAddrs[i]);
        LOGI("Probing %d: 0x%x", i, g_probeAddrs[i]);
        GlossHook(sym, (void*)probe_hook, (void**)&probe_origs[i]);
        sleep(5);
        GlossHook(sym, (void*)probe_origs[i], nullptr);
    }
    LOGI("Probe complete");
    return nullptr;
}

static void* mainthread(void*){
    sleep(5);
    GlossInit(true);
    GHandle hegl=GlossOpen("libEGL.so");
    void* swap=(void*)GlossSymbol(hegl,"eglSwapBuffers",nullptr);
    if(!swap){GHandle hg=GlossOpen("libGLESv2.so");swap=(void*)GlossSymbol(hg,"eglSwapBuffers",nullptr);}
    if(swap)GlossHook(swap,(void*)hook_swap,(void**)&orig_swap);
    GHandle hlib=GlossOpen("libinput.so");
    void* sc=nullptr;
    for(int i=0;consume_syms[i];i++){
        sc=(void*)GlossSymbol(hlib,consume_syms[i],nullptr);
        if(sc){g_consume_variant=i;LOGI("consume variant %d",i);break;}
    }
    if(!sc){LOGW("consume: no variant");}
    else{switch(g_consume_variant){
        case 0:GlossHook(sc,(void*)hc0,(void**)&orig0);break;
        case 1:GlossHook(sc,(void*)hc1,(void**)&orig1);break;
        case 2:GlossHook(sc,(void*)hc2,(void**)&orig2);break;
        case 3:GlossHook(sc,(void*)hc3,(void**)&orig3);break;
        case 4:GlossHook(sc,(void*)hc4,(void**)&orig4);break;
    }}
    findMcBase();
    if(g_baseFound&&g_normalTickOff!=0){
        void* sym=reinterpret_cast<void*>(g_mcBase+g_normalTickOff);
        GlossHook(sym,(void*)hook_normalTick,(void**)&orig_normalTick);
        if(orig_normalTick)LOGI("normalTick hooked @ 0x%" PRIxPTR,(uintptr_t)sym);
        else               LOGW("normalTick hook failed");
    }else LOGW("normalTick: mc base not found or offset=0");
    return nullptr;
}

__attribute__((constructor))
void keystrokes_init(){
    pthread_t t;
    pthread_create(&t,nullptr,mainthread,nullptr);
    pthread_t probe;
    pthread_create(&probe,nullptr,probe_thread,nullptr);
}
