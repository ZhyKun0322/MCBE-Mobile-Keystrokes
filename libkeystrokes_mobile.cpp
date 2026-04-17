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
#include <algorithm>

#include "pl/Hook.h"
#include "pl/Gloss.h"
#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

#define LOG_TAG "KeystrokesMobile"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)

#define VERSION "2.0.0" // Updated for mobile memory release

struct KeyState {
    bool w = false, a = false, s = false, d = false;
    bool space = false;
};

static KeyState g_keys;
static std::mutex g_keymutex;

static bool g_initialized = false;
static int g_width = 0, g_height = 0;
static float g_uiscale = 1.0f;
static EGLContext g_targetcontext = EGL_NO_CONTEXT;
static EGLSurface g_targetsurface = EGL_NO_SURFACE;
static uintptr_t g_mcBase = 0;

static float g_keysize      = 50.0f;
static float g_opacity      = 1.0f;
static float g_rounding     = 8.0f;
static bool  g_locked       = false;
static bool  g_showsettings = false;
static ImVec2 g_hudpos      = ImVec2(100, 100);
static bool  g_posloaded    = false;

static const char* SAVE_PATHS[] = {
    "/data/data/com.mojang.minecraftpe/files/keystrokes_mobile.cfg",
    "/data/data/com.mojang.minecraftpe.preview/files/keystrokes_mobile.cfg",
    nullptr
};

static const char* consume_syms[] = {
    "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventEb",
    "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE",
    "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEbxPjPPNS_10InputEventE",
    "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEbxPPNS_10InputEventEPj",
    "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEbxPPNS_10InputEventEPjb",
    nullptr
};

static int g_consume_variant = -1;

typedef int32_t (*consume_fn_0)(void*, void*, bool, long,      uint32_t*, AInputEvent**, bool);
typedef int32_t (*consume_fn_1)(void*, void*, bool, long,      uint32_t*, AInputEvent**);
typedef int32_t (*consume_fn_2)(void*, void*, bool, long long, uint32_t*, AInputEvent**);
typedef int32_t (*consume_fn_3)(void*, void*, bool, long long, AInputEvent**, uint32_t*);
typedef int32_t (*consume_fn_4)(void*, void*, bool, long long, AInputEvent**, uint32_t*, bool);

static consume_fn_0 orig_consume_0 = nullptr;
static consume_fn_1 orig_consume_1 = nullptr;
static consume_fn_2 orig_consume_2 = nullptr;
static consume_fn_3 orig_consume_3 = nullptr;
static consume_fn_4 orig_consume_4 = nullptr;

static EGLBoolean (*orig_eglswapbuffers)(EGLDisplay, EGLSurface) = nullptr;

static double nowsec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static float g_lasttouchy = 0.0f;
static bool  g_touchdown  = false;

static const char* getsavepath() {
    for (int i = 0; SAVE_PATHS[i]; i++) {
        FILE* f = fopen(SAVE_PATHS[i], "r");
        if (f) { fclose(f); return SAVE_PATHS[i]; }
        f = fopen(SAVE_PATHS[i], "a");
        if (f) { fclose(f); return SAVE_PATHS[i]; }
    }
    return SAVE_PATHS[0];
}

static void savecfg() {
    const char* path = getsavepath();
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%f %f %f %f %d %f\n",
        g_hudpos.x, g_hudpos.y, g_keysize, g_opacity, (int)g_locked, g_rounding);
    fclose(f);
}

static void loadcfg() {
    if (g_posloaded) return;
    g_posloaded = true;
    for (int i = 0; SAVE_PATHS[i]; i++) {
        FILE* f = fopen(SAVE_PATHS[i], "r");
        if (!f) continue;
        int locked = 0;
        int read = fscanf(f, "%f %f %f %f %d %f",
            &g_hudpos.x, &g_hudpos.y, &g_keysize, &g_opacity, &locked, &g_rounding);
        fclose(f);
        if (read >= 5) {
            g_locked   = (locked != 0);
            g_keysize  = std::max(30.0f,  std::min(g_keysize,  120.0f));
            g_opacity  = std::max(0.1f,   std::min(g_opacity,  1.0f));
            g_rounding = std::max(0.0f,   std::min(g_rounding, 50.0f));
            return;
        }
    }
}

static bool   g_pressing   = false;
static double g_pressstart = 0.0;
static const double LONGPRESS_SEC = 0.5;

// ONLY handles touch input for ImGui. Keyboard tracking removed.
static void processinput(AInputEvent* event) {
    if (g_initialized) ImGui_ImplAndroid_HandleInputEvent(event);
    int32_t type = AInputEvent_getType(event);

    if (type == AINPUT_EVENT_TYPE_MOTION) {
        int32_t action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
        
        if (g_initialized && g_showsettings) {
            float tx = AMotionEvent_getX(event, 0);
            float ty = AMotionEvent_getY(event, 0);
            ImGuiIO& io = ImGui::GetIO();

            if (action == AMOTION_EVENT_ACTION_DOWN) {
                g_lasttouchy    = ty;
                g_touchdown     = true;
                io.MousePos     = ImVec2(tx, ty);
                io.MouseDown[0] = true;
            } else if (action == AMOTION_EVENT_ACTION_MOVE && g_touchdown) {
                float dy     = ty - g_lasttouchy;
                g_lasttouchy = ty;
                io.MousePos  = ImVec2(tx, ty);
                io.MouseWheel += dy * -0.06f;
            } else if (action == AMOTION_EVENT_ACTION_UP ||
                       action == AMOTION_EVENT_ACTION_CANCEL) {
                g_touchdown     = false;
                io.MouseDown[0] = false;
            }
        }
    }
}

static int32_t hook_consume_0(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** outEvent, bool a6) {
    int32_t result = orig_consume_0 ? orig_consume_0(thiz, a1, a2, a3, a4, outEvent, a6) : 0;
    if (result == 0 && outEvent && *outEvent) processinput(*outEvent);
    return result;
}
static int32_t hook_consume_1(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** outEvent) {
    int32_t result = orig_consume_1 ? orig_consume_1(thiz, a1, a2, a3, a4, outEvent) : 0;
    if (result == 0 && outEvent && *outEvent) processinput(*outEvent);
    return result;
}
static int32_t hook_consume_2(void* thiz, void* a1, bool a2, long long a3, uint32_t* a4, AInputEvent** outEvent) {
    int32_t result = orig_consume_2 ? orig_consume_2(thiz, a1, a2, a3, a4, outEvent) : 0;
    if (result == 0 && outEvent && *outEvent) processinput(*outEvent);
    return result;
}
static int32_t hook_consume_3(void* thiz, void* a1, bool a2, long long a3, AInputEvent** outEvent, uint32_t* a4) {
    int32_t result = orig_consume_3 ? orig_consume_3(thiz, a1, a2, a3, outEvent, a4) : 0;
    if (result == 0 && outEvent && *outEvent) processinput(*outEvent);
    return result;
}
static int32_t hook_consume_4(void* thiz, void* a1, bool a2, long long a3, AInputEvent** outEvent, uint32_t* a4, bool a6) {
    int32_t result = orig_consume_4 ? orig_consume_4(thiz, a1, a2, a3, outEvent, a4, a6) : 0;
    if (result == 0 && outEvent && *outEvent) processinput(*outEvent);
    return result;
}

struct glstate {
    GLint prog, tex, atex, abuf, ebuf, vao, fbo, vp[4], sc[4], bsrc, bdst;
    GLboolean blend, cull, depth, scissor;
};

static void savegl(glstate& s) {
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.prog); glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s.atex);  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.abuf);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s.ebuf); glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.vao);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.fbo); glGetIntegerv(GL_VIEWPORT, s.vp);
    glGetIntegerv(GL_SCISSOR_BOX, s.sc); glGetIntegerv(GL_BLEND_SRC_ALPHA, &s.bsrc);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &s.bdst);
    s.blend = glIsEnabled(GL_BLEND); s.cull = glIsEnabled(GL_CULL_FACE);
    s.depth = glIsEnabled(GL_DEPTH_TEST); s.scissor = glIsEnabled(GL_SCISSOR_TEST);
}

static void restoregl(const glstate& s) {
    glUseProgram(s.prog); glActiveTexture(s.atex); glBindTexture(GL_TEXTURE_2D, s.tex);
    glBindBuffer(GL_ARRAY_BUFFER, s.abuf); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s.ebuf);
    glBindVertexArray(s.vao); glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    glViewport(s.vp[0], s.vp[1], s.vp[2], s.vp[3]); glScissor(s.sc[0], s.sc[1], s.sc[2], s.sc[3]);
    glBlendFunc(s.bsrc, s.bdst);
    s.blend   ? glEnable(GL_BLEND)        : glDisable(GL_BLEND);
    s.cull    ? glEnable(GL_CULL_FACE)    : glDisable(GL_CULL_FACE);
    s.depth   ? glEnable(GL_DEPTH_TEST)   : glDisable(GL_DEPTH_TEST);
    s.scissor ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
}

// Function to find libminecraftpe.so base address
static uintptr_t GetMinecraftBase() {
    uintptr_t base = 0;
    FILE* fp = fopen("/proc/self/maps", "r");
    if (fp) {
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, "libminecraftpe.so") && strstr(line, "r-xp")) {
                sscanf(line, "%lx", &base);
                break;
            }
        }
        fclose(fp);
    }
    return base;
}

// Memory reader function updating g_keys based on your offsets
static void UpdateKeysFromMemory() {
    if (g_mcBase == 0) {
        g_mcBase = GetMinecraftBase();
        return; // Try again next frame if not found
    }

    // Safely cast the calculated addresses to float pointers and read
    float vertical   = *(float*)(g_mcBase + 0x1B98304);
    float horizontal = *(float*)(g_mcBase + 0x2C09D3F0);
    float jumpState  = *(float*)(g_mcBase - 0x2309F70C);

    std::lock_guard<std::mutex> lock(g_keymutex);
    // Using 0.5f threshold to account for slight analog inputs
    g_keys.w = (vertical >= 0.5f);
    g_keys.s = (vertical <= -0.5f);
    g_keys.a = (horizontal >= 0.5f);
    g_keys.d = (horizontal <= -0.5f);
    g_keys.space = (jumpState >= 0.5f);
}

static void drawkey(const char* label, bool pressed, ImVec2 size) {
    float a = g_opacity;
    ImVec4 color     = pressed ? ImVec4(0.85f, 0.85f, 0.85f, 0.95f*a)
                               : ImVec4(0.18f, 0.20f, 0.22f, 0.88f*a);
    ImVec4 textcolor = pressed ? ImVec4(0.05f, 0.05f, 0.05f, a)
                               : ImVec4(0.90f, 0.90f, 0.90f, a);
    ImGui::PushStyleColor(ImGuiCol_Button,        color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  color);
    ImGui::PushStyleColor(ImGuiCol_Text,          textcolor);
    ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
}

static void drawsettings(ImVec2 hudpos) {
    float sw = g_width  * 0.26f;
    float sh = g_height * 0.62f;
    sw = std::max(sw, 220.0f);
    sh = std::max(sh, 340.0f);

    float ks      = g_keysize;
    float spacing = ks * 0.04f;
    float hudW    = ks * 3 + spacing * 2;

    float px = hudpos.x + hudW + 8.0f;
    float py = hudpos.y;
    if (px + sw > g_width)  px = hudpos.x - sw - 8.0f;
    if (py + sh > g_height) py = g_height - sh - 8.0f;
    if (px < 0) px = 8.0f;
    if (py < 0) py = 8.0f;

    ImGui::SetNextWindowPos(ImVec2(px, py), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(sw, sh), ImGuiCond_Always);

    ImGui::PushStyleColor(ImGuiCol_WindowBg,             ImVec4(0.10f, 0.12f, 0.16f, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg,              ImVec4(0.16f, 0.20f, 0.28f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,       ImVec4(0.22f, 0.28f, 0.38f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,           ImVec4(0.35f, 0.65f, 1.00f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive,     ImVec4(0.50f, 0.80f, 1.00f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark,            ImVec4(0.35f, 0.65f, 1.00f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Separator,            ImVec4(0.25f, 0.32f, 0.45f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg,          ImVec4(0.08f, 0.10f, 0.14f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab,        ImVec4(0.30f, 0.50f, 0.80f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(0.40f, 0.60f, 0.90f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive,  ImVec4(0.50f, 0.75f, 1.00f, 1.00f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,  ImVec2(12.0f, 12.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,    ImVec2(6.0f, 11.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,   ImVec2(6.0f, 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize,  6.0f);

    ImGui::Begin("##cfg", nullptr,
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize   |
        ImGuiWindowFlags_NoMove);

    float ctrlw = sw - 24.0f;

    ImGui::TextColored(ImVec4(0.85f, 0.92f, 1.00f, 1.0f), "KEYSTROKES  v" VERSION);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (g_mcBase != 0)
        ImGui::TextColored(ImVec4(0.40f, 0.80f, 0.40f, 1.0f), "Memory Reader: Active");
    else
        ImGui::TextColored(ImVec4(1.00f, 0.35f, 0.35f, 1.0f), "Memory Reader: Locating...");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(0.50f, 0.75f, 1.00f, 1.0f), "KEY SIZE");
    ImGui::TextColored(ImVec4(0.90f, 0.93f, 1.00f, 1.0f), "%.0f dp", g_keysize);
    ImGui::SetNextItemWidth(ctrlw);
    if (ImGui::SliderFloat("##sz", &g_keysize, 30.0f, 120.0f, "")) savecfg();

    ImGui::Spacing();

    ImGui::TextColored(ImVec4(0.50f, 0.75f, 1.00f, 1.0f), "OPACITY");
    float op = g_opacity * 100.0f;
    ImGui::TextColored(ImVec4(0.90f, 0.93f, 1.00f, 1.0f), "%.0f%%", op);
    ImGui::SetNextItemWidth(ctrlw);
    if (ImGui::SliderFloat("##op", &op, 10.0f, 100.0f, "")) {
        g_opacity = op / 100.0f;
        savecfg();
    }

    ImGui::Spacing();

    ImGui::TextColored(ImVec4(0.50f, 0.75f, 1.00f, 1.0f), "CORNER RADIUS");
    ImGui::TextColored(ImVec4(0.90f, 0.93f, 1.00f, 1.0f), "%.0f dp", g_rounding);
    ImGui::SetNextItemWidth(ctrlw);
    if (ImGui::SliderFloat("##rnd", &g_rounding, 0.0f, 50.0f, "")) savecfg();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(0.50f, 0.75f, 1.00f, 1.0f), "LOCK POSITION");
    bool locked = g_locked;
    if (ImGui::Checkbox("##lk", &locked)) { g_locked = locked; savecfg(); }
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.55f, 0.62f, 0.75f, 1.0f), "Prevent drag & move");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(0.50f, 0.75f, 1.00f, 1.0f), "RESET");
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.30f, 0.55f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.42f, 0.72f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.35f, 0.55f, 0.90f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.88f, 0.93f, 1.00f, 1.00f));
    if (ImGui::Button("Reset to Default", ImVec2(ctrlw, 0))) {
        g_keysize  = 50.0f;
        g_opacity  = 1.0f;
        g_rounding = 8.0f;
        g_locked   = false;
        g_hudpos   = ImVec2(100, 100);
        savecfg();
    }
    ImGui::PopStyleColor(4);

    float remaining = ImGui::GetContentRegionAvail().y
                    - ImGui::GetTextLineHeightWithSpacing() * 2.5f;
    if (remaining > 0) ImGui::Dummy(ImVec2(0, remaining));

    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.40f, 0.50f, 0.65f, 1.0f), "Made by");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.50f, 0.75f, 1.00f, 1.0f), "ZhyKun");

    ImGui::End();
    ImGui::PopStyleVar(5);
    ImGui::PopStyleColor(11);

    ImGuiIO& io = ImGui::GetIO();
    bool outsideclick = ImGui::IsMouseClicked(0) &&
        (io.MousePos.x < px || io.MousePos.x > px + sw ||
         io.MousePos.y < py || io.MousePos.y > py + sh);
    if (outsideclick) g_showsettings = false;
}

static void drawmenu() {
    UpdateKeysFromMemory(); // Read from memory before drawing!
    
    KeyState k;
    { std::lock_guard<std::mutex> lock(g_keymutex); k = g_keys; }

    float ks      = g_keysize;
    float spacing = ks * 0.04f;
    float hudW    = ks * 3 + spacing * 2;
    // Adjusted height since LMB/RMB were removed
    float hudH    = ks * 2.7f + spacing * 2;

    ImGuiIO& io = ImGui::GetIO();
    bool isInside = (io.MousePos.x >= g_hudpos.x && io.MousePos.x <= g_hudpos.x + hudW &&
                     io.MousePos.y >= g_hudpos.y && io.MousePos.y <= g_hudpos.y + hudH);

    if (isInside && io.MouseDown[0] && !g_pressing && !g_showsettings) {
        g_pressing   = true;
        g_pressstart = nowsec();
    }
    if (!io.MouseDown[0]) g_pressing = false;

    if (g_pressing && (nowsec() - g_pressstart) >= LONGPRESS_SEC) {
        g_showsettings = true;
        g_pressing     = false;
    }

    if (!g_locked && !g_showsettings && isInside && ImGui::IsMouseDragging(0)) {
        g_hudpos.x += io.MouseDelta.x;
        g_hudpos.y += io.MouseDelta.y;
        g_hudpos.x = std::max(0.0f, std::min(g_hudpos.x, (float)g_width  - hudW));
        g_hudpos.y = std::max(0.0f, std::min(g_hudpos.y, (float)g_height - hudH));
        savecfg();
    }

    if (g_showsettings) drawsettings(g_hudpos);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowPos(g_hudpos, ImGuiCond_Always);
    ImGui::Begin("##ks", nullptr,
        ImGuiWindowFlags_NoTitleBar       |
        ImGuiWindowFlags_NoBackground     |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoMove           |
        ImGuiWindowFlags_NoInputs);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(spacing, spacing));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, g_rounding);

    ImGui::SetCursorPosX(ks + spacing);
    drawkey("W", k.w, ImVec2(ks, ks));

    drawkey("A", k.a, ImVec2(ks, ks)); ImGui::SameLine();
    drawkey("S", k.s, ImVec2(ks, ks)); ImGui::SameLine();
    drawkey("D", k.d, ImVec2(ks, ks));

    drawkey("_____", k.space, ImVec2(hudW, ks * 0.7f));

    ImGui::PopStyleVar(3);
    ImGui::End();
}

static void setup() {
    if (g_initialized || g_width <= 0 || g_height <= 0) return;
    loadcfg();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    int minside = std::min(g_width, g_height);
    int maxside = std::max(g_width, g_height);

    float dpscale = (float)minside / 480.0f;
    dpscale = std::max(0.85f, std::min(dpscale, 2.8f));
    if (maxside > 2400) dpscale = std::min(dpscale * 1.15f, 2.8f);

    g_uiscale = dpscale;

    float fontsize = std::max(14.0f, 15.0f * dpscale);
    ImFontConfig cfg;
    cfg.SizePixels = fontsize;
    io.Fonts->AddFontDefault(&cfg);

    ImGui_ImplAndroid_Init();
    ImGui_ImplOpenGL3_Init("#version 300 es");

    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(dpscale);
    style.WindowBorderSize = 0.0f;

    g_initialized = true;
}

static void render() {
    if (!g_initialized) return;
    glstate s; savegl(s);
    ImGuiIO& io    = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_width, (float)g_height);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_width, g_height);
    ImGui::NewFrame();
    drawmenu();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    restoregl(s);
}

static EGLBoolean hook_eglswapbuffers(EGLDisplay dpy, EGLSurface surf) {
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return orig_eglswapbuffers(dpy, surf);
    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH,  &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w < 500 || h < 500) return orig_eglswapbuffers(dpy, surf);
    if (g_targetcontext == EGL_NO_CONTEXT) { g_targetcontext = ctx; g_targetsurface = surf; }
    if (ctx == g_targetcontext && surf == g_targetsurface) { 
        g_width = w; 
        g_height = h; 
        setup();   // Initialize ImGui if not already done
        render();  // Draw the keystrokes
    }

    return orig_eglswapbuffers(dpy, surf);
}

static void* mainthread(void*) {
    // Wait for the game to fully load into memory (5 seconds)
    sleep(5);

    // Initialize the Gloss Hooking library
    GlossInit(true);

    LOGI("Keystrokes: Initializing Hooks...");

    // 1. Hook EGL swap (This creates our 'Render Loop')
    GHandle hegl = GlossOpen("libEGL.so");
    void* swap   = (void*)GlossSymbol(hegl, "eglSwapBuffers", nullptr);
    if (!swap) {
        GHandle hgles = GlossOpen("libGLESv2.so");
        swap = (void*)GlossSymbol(hgles, "eglSwapBuffers", nullptr);
    }
    
    if (swap) {
        GlossHook(swap, (void*)hook_eglswapbuffers, (void**)&orig_eglswapbuffers);
        LOGI("Keystrokes: EGL Hooked Successfully");
    }

    // 2. Hook InputConsumer::consume (For Menu Touch Interaction)
    GHandle hlib     = GlossOpen("libinput.so");
    void* symconsume = nullptr;

    for (int i = 0; consume_syms[i]; i++) {
        symconsume = (void*)GlossSymbol(hlib, consume_syms[i], nullptr);
        if (symconsume) {
            g_consume_variant = i;
            break;
        }
    }

    if (symconsume) {
        switch (g_consume_variant) {
            case 0: GlossHook(symconsume, (void*)hook_consume_0, (void**)&orig_consume_0); break;
            case 1: GlossHook(symconsume, (void*)hook_consume_1, (void**)&orig_consume_1); break;
            case 2: GlossHook(symconsume, (void*)hook_consume_2, (void**)&orig_consume_2); break;
            case 3: GlossHook(symconsume, (void*)hook_consume_3, (void**)&orig_consume_3); break;
            case 4: GlossHook(symconsume, (void*)hook_consume_4, (void**)&orig_consume_4); break;
        }
        LOGI("Keystrokes: Input Hooked Successfully (Variant %d)", g_consume_variant);
    }

    return nullptr;
}

// Entry point when the .so is loaded
__attribute__((constructor))
void keystrokes_init() {
    pthread_t t;
    pthread_create(&t, nullptr, mainthread, nullptr);
}
