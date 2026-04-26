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

#include "pl/Hook.h"
#include "pl/Gloss.h"
#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

#define LOG_TAG "Keystrokes"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define VERSION "2.0.0"

// ══════════════════════════════════════════════════════════════════════════
// Key state  — derived from touch events, no MC internals needed
// ══════════════════════════════════════════════════════════════════════════
struct KeyState { bool w, a, s, d, space; };
static KeyState   g_keys   = {};
static std::mutex g_keymtx;

// ══════════════════════════════════════════════════════════════════════════
// Joystick touch tracking
//
// MCBE mobile uses a virtual joystick on the LEFT side of the screen.
// We track the first pointer that touches down in the left 45% of width.
// The displacement from touch-down origin → WASD direction.
//
// Jump button is on the RIGHT side, lower portion of screen.
// We track any pointer that touches down in right 30%, lower 35%.
// ══════════════════════════════════════════════════════════════════════════
static const float JOY_DEADZONE  = 18.0f;  // px — below this = no movement
static const float JOY_ZONE_X    = 0.45f;  // left 45% of screen = joystick
static const float JUMP_ZONE_X   = 0.70f;  // right 30% = jump zone
static const float JUMP_ZONE_Y   = 0.65f;  // lower 35% = jump zone

// One slot per Android pointer (max 10 simultaneous fingers)
struct Ptr {
    int32_t id   = -1;
    float   ox = 0, oy = 0;  // origin at touch-down
    float   cx = 0, cy = 0;  // current position
    bool    isJoy  = false;
    bool    isJump = false;
};
static const int MAX_PTRS = 10;
static Ptr       g_ptrs[MAX_PTRS];
static std::mutex g_ptrmtx;

static int findPtr(int32_t id) {
    for (int i = 0; i < MAX_PTRS; i++)
        if (g_ptrs[i].id == id) return i;
    return -1;
}
static int allocPtr(int32_t id) {
    for (int i = 0; i < MAX_PTRS; i++)
        if (g_ptrs[i].id < 0) { g_ptrs[i].id = id; return i; }
    return -1; // all slots full
}
static void freePtr(int32_t id) {
    int i = findPtr(id);
    if (i >= 0) g_ptrs[i] = Ptr{};
}

// Recompute g_keys from current pointer state — called after every touch event
static void recomputeKeys(int screenW, int screenH) {
    float dx = 0, dy = 0;
    bool  jump = false;
    for (int i = 0; i < MAX_PTRS; i++) {
        if (g_ptrs[i].id < 0) continue;
        if (g_ptrs[i].isJoy) {
            dx = g_ptrs[i].cx - g_ptrs[i].ox;
            dy = g_ptrs[i].cy - g_ptrs[i].oy;
        }
        if (g_ptrs[i].isJump) jump = true;
    }
    float mag = sqrtf(dx*dx + dy*dy);
    KeyState k{};
    if (mag > JOY_DEADZONE) {
        k.w = (dy < -JOY_DEADZONE);
        k.s = (dy >  JOY_DEADZONE);
        k.a = (dx < -JOY_DEADZONE);
        k.d = (dx >  JOY_DEADZONE);
    }
    k.space = jump;
    std::lock_guard<std::mutex> lk(g_keymtx);
    g_keys = k;
}

// ══════════════════════════════════════════════════════════════════════════
// Touch queue — feeds ImGui for HUD drag + settings window interaction
// ══════════════════════════════════════════════════════════════════════════
struct TouchEvt { float x, y, dy; int32_t action; };
static const int TQSZ = 16;
static TouchEvt  g_tq[TQSZ];
static int       g_tqh = 0, g_tqt = 0;
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
// Screen size (set by eglSwapBuffers hook, read by processinput)
// ══════════════════════════════════════════════════════════════════════════
static int g_width = 0, g_height = 0;

// ══════════════════════════════════════════════════════════════════════════
// processinput — called from InputConsumer::consume on the input thread
// 1. Tracks joystick + jump pointer state → g_keys
// 2. Forwards to ImGui queue for HUD drag / settings interaction
// ══════════════════════════════════════════════════════════════════════════
static float  g_lasty = 0.f;
static bool   g_td    = false;

static void processinput(AInputEvent* ev) {
    if (AInputEvent_getType(ev) != AINPUT_EVENT_TYPE_MOTION) return;

    int sw = g_width, sh = g_height;
    if (sw <= 0 || sh <= 0) return;

    int32_t actionMasked = AMotionEvent_getAction(ev) & AMOTION_EVENT_ACTION_MASK;
    int32_t ptrIdx = (AMotionEvent_getAction(ev) & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK)
                     >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    int32_t ptrId  = AMotionEvent_getPointerId(ev, ptrIdx);
    float   px     = AMotionEvent_getX(ev, ptrIdx);
    float   py     = AMotionEvent_getY(ev, ptrIdx);

    {
        std::lock_guard<std::mutex> lk(g_ptrmtx);

        if (actionMasked == AMOTION_EVENT_ACTION_DOWN ||
            actionMasked == AMOTION_EVENT_ACTION_POINTER_DOWN) {
            int slot = allocPtr(ptrId);
            if (slot >= 0) {
                g_ptrs[slot].ox = g_ptrs[slot].cx = px;
                g_ptrs[slot].oy = g_ptrs[slot].cy = py;
                // Classify pointer
                bool leftSide  = (px / sw) < JOY_ZONE_X;
                bool rightSide = (px / sw) > JUMP_ZONE_X;
                bool lowerPart = (py / sh) > JUMP_ZONE_Y;
                // Joystick: left side — pick only one joystick pointer at a time
                bool joyFree = true;
                for (int i = 0; i < MAX_PTRS; i++)
                    if (g_ptrs[i].id >= 0 && g_ptrs[i].isJoy) { joyFree = false; break; }
                g_ptrs[slot].isJoy  = leftSide && joyFree;
                g_ptrs[slot].isJump = rightSide && lowerPart;
            }
            recomputeKeys(sw, sh);
        }
        else if (actionMasked == AMOTION_EVENT_ACTION_MOVE) {
            // MOVE events carry ALL active pointers — update all of them
            int32_t cnt = AMotionEvent_getPointerCount(ev);
            for (int32_t pi = 0; pi < cnt; pi++) {
                int32_t pid = AMotionEvent_getPointerId(ev, pi);
                int slot = findPtr(pid);
                if (slot < 0) continue;
                g_ptrs[slot].cx = AMotionEvent_getX(ev, pi);
                g_ptrs[slot].cy = AMotionEvent_getY(ev, pi);
            }
            recomputeKeys(sw, sh);
        }
        else if (actionMasked == AMOTION_EVENT_ACTION_UP   ||
                 actionMasked == AMOTION_EVENT_ACTION_POINTER_UP ||
                 actionMasked == AMOTION_EVENT_ACTION_CANCEL) {
            freePtr(ptrId);
            recomputeKeys(sw, sh);
        }
    }

    // Forward to ImGui queue (for settings window / HUD drag)
    if (actionMasked == AMOTION_EVENT_ACTION_DOWN)
        { g_lasty = py; g_td = true; tq_push(px, py, AMOTION_EVENT_ACTION_DOWN); }
    else if (actionMasked == AMOTION_EVENT_ACTION_MOVE && g_td)
        { float dy = py - g_lasty; g_lasty = py; tq_push(px, py, AMOTION_EVENT_ACTION_MOVE, dy*-0.06f); }
    else if (actionMasked == AMOTION_EVENT_ACTION_UP || actionMasked == AMOTION_EVENT_ACTION_CANCEL)
        { g_td = false; tq_push(px, py, AMOTION_EVENT_ACTION_UP); }
}

// ══════════════════════════════════════════════════════════════════════════
// InputConsumer::consume hook variants (unchanged — just routes to processinput)
// ══════════════════════════════════════════════════════════════════════════
static const char* consume_syms[] = {
    "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventEb",
    "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE",
    "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEbxPjPPNS_10InputEventE",
    "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEbxPPNS_10InputEventEPj",
    "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEbxPPNS_10InputEventEPjb",
    nullptr
};
static int g_consume_variant = -1;

typedef int32_t (*cfn0)(void*,void*,bool,long,      uint32_t*,AInputEvent**,bool);
typedef int32_t (*cfn1)(void*,void*,bool,long,      uint32_t*,AInputEvent**);
typedef int32_t (*cfn2)(void*,void*,bool,long long, uint32_t*,AInputEvent**);
typedef int32_t (*cfn3)(void*,void*,bool,long long, AInputEvent**,uint32_t*);
typedef int32_t (*cfn4)(void*,void*,bool,long long, AInputEvent**,uint32_t*,bool);

static cfn0 orig0=nullptr; static cfn1 orig1=nullptr;
static cfn2 orig2=nullptr; static cfn3 orig3=nullptr; static cfn4 orig4=nullptr;

static int32_t hc0(void*t,void*a,bool b,long c,uint32_t*d,AInputEvent**e,bool f){
    int32_t r=orig0?orig0(t,a,b,c,d,e,f):0;if(r==0&&e&&*e)processinput(*e);return r;}
static int32_t hc1(void*t,void*a,bool b,long c,uint32_t*d,AInputEvent**e){
    int32_t r=orig1?orig1(t,a,b,c,d,e):0;  if(r==0&&e&&*e)processinput(*e);return r;}
static int32_t hc2(void*t,void*a,bool b,long long c,uint32_t*d,AInputEvent**e){
    int32_t r=orig2?orig2(t,a,b,c,d,e):0;  if(r==0&&e&&*e)processinput(*e);return r;}
static int32_t hc3(void*t,void*a,bool b,long long c,AInputEvent**e,uint32_t*d){
    int32_t r=orig3?orig3(t,a,b,c,e,d):0;  if(r==0&&e&&*e)processinput(*e);return r;}
static int32_t hc4(void*t,void*a,bool b,long long c,AInputEvent**e,uint32_t*d,bool f){
    int32_t r=orig4?orig4(t,a,b,c,e,d,f):0;if(r==0&&e&&*e)processinput(*e);return r;}

// ══════════════════════════════════════════════════════════════════════════
// GL state save/restore
// ══════════════════════════════════════════════════════════════════════════
struct glst {
    GLint prog,tex,atex,abuf,ebuf,vao,fbo,vp[4],sc[4],bsrc,bdst;
    GLboolean blend,cull,depth,scissor;
};
static void sgl(glst& s){
    glGetIntegerv(GL_CURRENT_PROGRAM,&s.prog);
    glGetIntegerv(GL_TEXTURE_BINDING_2D,&s.tex);
    glGetIntegerv(GL_ACTIVE_TEXTURE,&s.atex);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&s.abuf);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING,&s.ebuf);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&s.vao);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING,&s.fbo);
    glGetIntegerv(GL_VIEWPORT,s.vp);
    glGetIntegerv(GL_SCISSOR_BOX,s.sc);
    glGetIntegerv(GL_BLEND_SRC_ALPHA,&s.bsrc);
    glGetIntegerv(GL_BLEND_DST_ALPHA,&s.bdst);
    s.blend=glIsEnabled(GL_BLEND);s.cull=glIsEnabled(GL_CULL_FACE);
    s.depth=glIsEnabled(GL_DEPTH_TEST);s.scissor=glIsEnabled(GL_SCISSOR_TEST);
}
static void rgl(const glst& s){
    glUseProgram(s.prog);
    glActiveTexture(s.atex);glBindTexture(GL_TEXTURE_2D,s.tex);
    glBindBuffer(GL_ARRAY_BUFFER,s.abuf);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,s.ebuf);
    glBindVertexArray(s.vao);
    glBindFramebuffer(GL_FRAMEBUFFER,s.fbo);
    glViewport(s.vp[0],s.vp[1],s.vp[2],s.vp[3]);
    glScissor(s.sc[0],s.sc[1],s.sc[2],s.sc[3]);
    glBlendFunc(s.bsrc,s.bdst);
    s.blend?glEnable(GL_BLEND):glDisable(GL_BLEND);
    s.cull?glEnable(GL_CULL_FACE):glDisable(GL_CULL_FACE);
    s.depth?glEnable(GL_DEPTH_TEST):glDisable(GL_DEPTH_TEST);
    s.scissor?glEnable(GL_SCISSOR_TEST):glDisable(GL_SCISSOR_TEST);
}

// ══════════════════════════════════════════════════════════════════════════
// Config
// ══════════════════════════════════════════════════════════════════════════
static float  g_keysize  = 50.f;
static float  g_opacity  = 1.f;
static float  g_rounding = 8.f;
static bool   g_locked   = false;
static ImVec2 g_hudpos   = ImVec2(100,100);
static bool   g_posloaded= false;
static float  g_uiscale  = 1.f;
static bool   g_initialized = false;
static bool   g_showsettings = false;
static EGLContext g_targetcontext = EGL_NO_CONTEXT;
static EGLSurface g_targetsurface = EGL_NO_SURFACE;

static EGLBoolean (*orig_swap)(EGLDisplay,EGLSurface) = nullptr;

static const char* SAVE_PATHS[]={
    "/data/data/com.mojang.minecraftpe/files/keystrokes.cfg",
    "/data/data/com.mojang.minecraftpe.preview/files/keystrokes.cfg",
    nullptr
};
static const char* getsavepath(){
    for(int i=0;SAVE_PATHS[i];i++){
        FILE*f=fopen(SAVE_PATHS[i],"r");if(f){fclose(f);return SAVE_PATHS[i];}
        f=fopen(SAVE_PATHS[i],"a");if(f){fclose(f);return SAVE_PATHS[i];}
    }return SAVE_PATHS[0];
}
static void savecfg(){
    FILE*f=fopen(getsavepath(),"w");if(!f)return;
    fprintf(f,"%f %f %f %f %d %f\n",
        g_hudpos.x,g_hudpos.y,g_keysize,g_opacity,(int)g_locked,g_rounding);
    fclose(f);
}
static void loadcfg(){
    if(g_posloaded)return;g_posloaded=true;
    for(int i=0;SAVE_PATHS[i];i++){
        FILE*f=fopen(SAVE_PATHS[i],"r");if(!f)continue;
        int lk=0;
        int n=fscanf(f,"%f %f %f %f %d %f",
            &g_hudpos.x,&g_hudpos.y,&g_keysize,&g_opacity,&lk,&g_rounding);
        fclose(f);
        if(n>=5){
            g_locked=(lk!=0);
            g_keysize=std::max(30.f,std::min(g_keysize,120.f));
            g_opacity=std::max(0.1f,std::min(g_opacity,1.f));
            g_rounding=std::max(0.f,std::min(g_rounding,50.f));
            return;
        }
    }
}

// ══════════════════════════════════════════════════════════════════════════
// Time helper
// ══════════════════════════════════════════════════════════════════════════
static double nowsec(){
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ══════════════════════════════════════════════════════════════════════════
// Drawing
// ══════════════════════════════════════════════════════════════════════════
static void drawkey(const char* lbl, bool on, ImVec2 sz){
    float a=g_opacity;
    ImVec4 bg=on?ImVec4(.85f,.85f,.85f,.95f*a):ImVec4(.18f,.20f,.22f,.88f*a);
    ImVec4 fg=on?ImVec4(.05f,.05f,.05f,a):ImVec4(.90f,.90f,.90f,a);
    ImGui::PushStyleColor(ImGuiCol_Button,bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,bg);
    ImGui::PushStyleColor(ImGuiCol_Text,fg);
    ImGui::Button(lbl,sz);
    ImGui::PopStyleColor(4);
}

// ── Settings window ───────────────────────────────────────────────────────
static void drawsettings(ImVec2 anchor){
    ImGuiIO& io=ImGui::GetIO();
    float sw=300.f*g_uiscale,sh=std::min(460.f*g_uiscale,io.DisplaySize.y*0.85f);
    float px=anchor.x,py=anchor.y+g_keysize*3.5f;
    if(px+sw>io.DisplaySize.x)px=io.DisplaySize.x-sw-4;
    if(py+sh>io.DisplaySize.y)py=anchor.y-sh-4;
    if(py<0)py=4;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,14.f*g_uiscale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(16.f*g_uiscale,14.f*g_uiscale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(8.f*g_uiscale,10.f*g_uiscale));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,8.f*g_uiscale);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding,8.f*g_uiscale);
    ImGui::PushStyleColor(ImGuiCol_WindowBg,      ImVec4(.10f,.11f,.15f,.97f));
    ImGui::PushStyleColor(ImGuiCol_TitleBg,       ImVec4(.10f,.11f,.15f,.97f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(.13f,.15f,.20f,.97f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,    ImVec4(.35f,.55f,.90f,1.f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive,ImVec4(.45f,.65f,1.f,1.f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg,       ImVec4(.16f,.18f,.24f,1.f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,ImVec4(.20f,.23f,.30f,1.f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark,     ImVec4(.45f,.70f,1.f,1.f));
    ImGui::PushStyleColor(ImGuiCol_Border,        ImVec4(.25f,.30f,.42f,.6f));
    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(.18f,.25f,.40f,.8f));
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(.88f,.93f,1.f,1.f));
    ImGui::SetNextWindowPos(ImVec2(px,py),ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(sw,sh),ImGuiCond_Always);
    ImGui::Begin("Keystrokes Settings",nullptr,
        ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoCollapse);
    float cw=ImGui::GetContentRegionAvail().x;

    // Status
    ImGui::TextColored(ImVec4(.5f,.75f,1,1),"INPUT METHOD");
    ImGui::TextColored(ImVec4(.4f,.8f,.4f,1),"Touch joystick tracking");
    ImGui::TextColored(ImVec4(.55f,.62f,.75f,1),
        "Joy zone: left %.0f%%  Jump: right %.0f%% / lower %.0f%%",
        JOY_ZONE_X*100, (1.f-JUMP_ZONE_X)*100, (1.f-JUMP_ZONE_Y)*100);
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
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(.18f,.30f,.55f,1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.25f,.42f,.72f,1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(.35f,.55f,.90f,1));
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(.88f,.93f,1,1));
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
    ImGuiIO& io2=ImGui::GetIO();
    if(ImGui::IsMouseClicked(0)&&
       (io2.MousePos.x<px||io2.MousePos.x>px+sw||
        io2.MousePos.y<py||io2.MousePos.y>py+sh))
        g_showsettings=false;
}

// ── HUD ───────────────────────────────────────────────────────────────────
static bool   g_pressing = false;
static double g_pstart   = 0.0;
static const double LP_SEC = 0.5;

static void drawmenu(){
    KeyState k; { std::lock_guard<std::mutex> lk(g_keymtx); k=g_keys; }
    float ks=g_keysize, sp=ks*0.04f, hw=ks*3+sp*2;
    float hh=ks*2+sp*2+sp+ks*0.7f;
    ImGuiIO& io=ImGui::GetIO();
    bool inside=(io.MousePos.x>=g_hudpos.x&&io.MousePos.x<=g_hudpos.x+hw&&
                 io.MousePos.y>=g_hudpos.y&&io.MousePos.y<=g_hudpos.y+hh);
 // Long press → open settings
    if(inside&&io.MouseDown[0]&&!g_pressing&&!g_showsettings)
        {g_pressing=true;g_pstart=nowsec();}
    if(!io.MouseDown[0])g_pressing=false;
    if(g_pressing&&(nowsec()-g_pstart)>=LP_SEC){g_showsettings=true;g_pressing=false;}
    // Drag to reposition
    if(!g_locked&&!g_showsettings&&inside&&ImGui::IsMouseDragging(0)){
        g_hudpos.x+=io.MouseDelta.x; g_hudpos.y+=io.MouseDelta.y;
        g_hudpos.x=std::max(0.f,std::min(g_hudpos.x,(float)g_width-hw));
        g_hudpos.y=std::max(0.f,std::min(g_hudpos.y,(float)g_height-hh));
        savecfg();
    }
    if(g_showsettings) drawsettings(g_hudpos);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0,0));
    ImGui::SetNextWindowPos(g_hudpos,ImGuiCond_Always);
    ImGui::Begin("##ks",nullptr,
        ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoBackground|
        ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoMove|
        ImGuiWindowFlags_NoInputs);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  ImVec2(sp,sp));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,g_rounding);
    // Row 1: [  ] [W] [  ]
    ImVec2 sq(ks,ks);
    ImGui::Dummy(sq); ImGui::SameLine();
    drawkey("W",k.w,sq);
    // Row 2: [A] [S] [D]
    drawkey("A",k.a,sq); ImGui::SameLine();
    drawkey("S",k.s,sq); ImGui::SameLine();
    drawkey("D",k.d,sq);
    // Row 3: [   SPACE   ]
    drawkey("_____",k.space,ImVec2(hw,ks*0.7f));
    ImGui::PopStyleVar(3); ImGui::End();
}

// ══════════════════════════════════════════════════════════════════════════
// Setup / render
// ══════════════════════════════════════════════════════════════════════════
static void setup(){
    if(g_initialized||g_width<=0||g_height<=0)return;
    loadcfg();
    ImGui::CreateContext();
    ImGuiIO& io=ImGui::GetIO(); io.IniFilename=nullptr;
    int mn=std::min(g_width,g_height),mx=std::max(g_width,g_height);
    float dp=std::max(0.85f,std::min((float)mn/480.f,2.8f));
    if(mx>2400)dp=std::min(dp*1.15f,2.8f);
    g_uiscale=dp;
    ImFontConfig fc; fc.SizePixels=std::max(14.f,15.f*dp);
    io.Fonts->AddFontDefault(&fc);
    ImGui_ImplAndroid_Init();
    ImGui_ImplOpenGL3_Init("#version 300 es");
    ImGuiStyle& st=ImGui::GetStyle();
    st.ScaleAllSizes(dp); st.WindowBorderSize=0;
    float ks=g_keysize,sp=ks*0.04f,hw=ks*3+sp*2,hh=ks*3+sp*2;
    g_hudpos.x=std::max(0.f,std::min(g_hudpos.x,(float)g_width-hw));
    g_hudpos.y=std::max(0.f,std::min(g_hudpos.y,(float)g_height-hh));
    g_initialized=true;
    LOGI("Keystrokes v" VERSION " init %dx%d dp=%.2f", g_width, g_height, dp);
}

static void render(){
    if(!g_initialized)return;
    glst s; sgl(s);
    ImGuiIO& io=ImGui::GetIO();
    io.DisplaySize=ImVec2((float)g_width,(float)g_height);
    tq_drain();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_width,g_height);
    ImGui::NewFrame();
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

// ══════════════════════════════════════════════════════════════════════════
// Main thread — hooks eglSwapBuffers + InputConsumer::consume only
// ══════════════════════════════════════════════════════════════════════════
static void* mainthread(void*){
    sleep(5);
    GlossInit(true);
    // eglSwapBuffers
    GHandle hegl=GlossOpen("libEGL.so");
    void* swap=(void*)GlossSymbol(hegl,"eglSwapBuffers",nullptr);
    if(!swap){GHandle hg=GlossOpen("libGLESv2.so");swap=(void*)GlossSymbol(hg,"eglSwapBuffers",nullptr);}
    if(swap) GlossHook(swap,(void*)hook_swap,(void**)&orig_swap);
    else LOGW("eglSwapBuffers not found");
    // InputConsumer::consume
    GHandle hlib=GlossOpen("libinput.so");
    void* sc=nullptr;
    for(int i=0;consume_syms[i]&&!sc;i++){
        sc=(void*)GlossSymbol(hlib,consume_syms[i],nullptr);
        if(sc){g_consume_variant=i;LOGI("consume variant %d",i);}
    }
    if(!sc) LOGW("InputConsumer::consume not found");
    else { switch(g_consume_variant){
        case 0:GlossHook(sc,(void*)hc0,(void**)&orig0);break;
        case 1:GlossHook(sc,(void*)hc1,(void**)&orig1);break;
        case 2:GlossHook(sc,(void*)hc2,(void**)&orig2);break;
        case 3:GlossHook(sc,(void*)hc3,(void**)&orig3);break;
        case 4:GlossHook(sc,(void*)hc4,(void**)&orig4);break;
    }}
    return nullptr;
}

__attribute__((constructor))
void keystrokes_init(){
    pthread_t t;
    pthread_create(&t,nullptr,mainthread,nullptr);
}
