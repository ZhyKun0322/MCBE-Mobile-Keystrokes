#include <jni.h>
#include <android/log.h>
#include <android/input.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <mutex>

#include "pl/Gloss.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_android.h"

#define LOG_TAG "MobileKeystrokes"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ─────────────────────────────────────────────────────────────────────────────
//  1. Taco's MoveInputComponent layout (from screenshot, 26.13.1)
// ─────────────────────────────────────────────────────────────────────────────
struct Vec2 { float x, y; };
struct Vec3 { float x, y, z; };

// MoveInputState occupies 0x10 bytes (two of them back-to-back)
struct MoveInputState {
    char data[0x10];
};

struct MoveInputComponent {
    MoveInputState  mInputState;            // [0x00] processed input state
    MoveInputState  mRawInputState;         // [0x10] RAW input state (D-pad/touch)
    uint8_t         mHoldAutoJumpInWaterTicks; // [0x20]
    char            pad[0x3];              // align to 0x24
    Vec2            mMove;                 // [0x24] movement vector
    Vec2            mLookDelta;            // [0x2C]
    Vec2            mInteractDir;          // [0x34]
    char            pad2[0x4];             // align to 0x3C
    Vec3            mDisplacement;         // [0x3C]
    Vec3            mDisplacementDelta;    // [0x48]
    Vec3            mCameraOrientation;    // [0x54]
    unsigned short  mFlagValues;           // [0x60] brstd::bitset<11,ushort> — bit 0 = jump
    bool            mIsPaddling[2];        // [0x61]
};

// ─────────────────────────────────────────────────────────────────────────────
//  2. Key state  +  per-key visibility settings
// ─────────────────────────────────────────────────────────────────────────────
struct KeyState {
    bool w=false, a=false, s=false, d=false;
    bool space=false, lmb=false, rmb=false;
};

struct KeySettings {
    bool show_w     = true;
    bool show_a     = true;
    bool show_s     = true;
    bool show_d     = true;
    bool show_space = true;
    bool show_lmb   = true;
    bool show_rmb   = true;
};

static KeyState    g_keys;
static KeySettings g_settings;
static std::mutex  g_keymutex;
static bool        g_initialized   = false;
static bool        g_show_settings = false;

// ─────────────────────────────────────────────────────────────────────────────
//  3. NormalTick hook — WASD + SPACE
//     mMove at [0x24] reflects the FINAL movement vector (works for joystick).
//     For D-pad/touch, mRawInputState at [0x10] holds the unprocessed state.
//     We read mMove for direction and mFlagValues for jump — both confirmed
//     by Taco's struct dump for 26.13.1.
// ─────────────────────────────────────────────────────────────────────────────
typedef void (*NormalTick_t)(void*);
static NormalTick_t orig_NormalTick = nullptr;

static int g_tick_count = 0;

void hook_NormalTick(void* player) {
    if (player) {
        MoveInputComponent* mic =
            *(MoveInputComponent**)((uintptr_t)player + 0x10A8);

        if (mic) {
            std::lock_guard<std::mutex> lock(g_keymutex);

            // mMove [0x24]: x = strafe, y = forward
            // Positive y = forward (W), negative y = backward (S)
            // Positive x = right (D),  negative x = left (A)
            g_keys.w     = (mic->mMove.y >  0.1f);
            g_keys.s     = (mic->mMove.y < -0.1f);
            g_keys.d     = (mic->mMove.x >  0.1f);
            g_keys.a     = (mic->mMove.x < -0.1f);

            // mFlagValues [0x60]: bit 0 = jump
            g_keys.space = (mic->mFlagValues & 0x1) != 0;

            // Debug: log every 60 ticks so we can verify values in logcat
            if (++g_tick_count % 60 == 0) {
                LOGI("mMove=(%.2f,%.2f) flags=0x%X w=%d a=%d s=%d d=%d space=%d",
                     mic->mMove.x, mic->mMove.y, mic->mFlagValues,
                     g_keys.w, g_keys.a, g_keys.s, g_keys.d, g_keys.space);
            }
        }
    }
    if (orig_NormalTick) orig_NormalTick(player);
}

// ─────────────────────────────────────────────────────────────────────────────
//  4. Input hook — LMB / RMB via AInputEvent
//     Hooks the same libinput.so symbols as ZhyKun's original mod.
// ─────────────────────────────────────────────────────────────────────────────
static void       (*orig_input1)(void*, void*, void*)              = nullptr;
static int32_t    (*orig_input2)(void*, void*, bool, long,
                                  uint32_t*, AInputEvent**)        = nullptr;

static void handle_mouse_event(AInputEvent* event) {
    if (!event) return;
    if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION) return;

    int32_t btn = AMotionEvent_getButtonState(event);
    std::lock_guard<std::mutex> lock(g_keymutex);
    g_keys.lmb = (btn & AMOTION_EVENT_BUTTON_PRIMARY)   != 0;
    g_keys.rmb = (btn & AMOTION_EVENT_BUTTON_SECONDARY) != 0;
}

static void hook_input1(void* thiz, void* a1, void* a2) {
    if (orig_input1) orig_input1(thiz, a1, a2);
    if (thiz && g_initialized)
        handle_mouse_event((AInputEvent*)thiz);
}

static int32_t hook_input2(void* thiz, void* a1, bool a2, long a3,
                            uint32_t* a4, AInputEvent** event) {
    int32_t r = orig_input2 ? orig_input2(thiz, a1, a2, a3, a4, event) : 0;
    if (r == 0 && event && *event && g_initialized)
        handle_mouse_event(*event);
    return r;
}

static void hook_input() {
    void* sym1 = (void*)GlossSymbol(GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE",
        nullptr);
    if (sym1) {
        if (GlossHook(sym1, (void*)hook_input1, (void**)&orig_input1)) {
            LOGI("Hooked input (sym1) for LMB/RMB");
            return;
        }
    }
    void* sym2 = (void*)GlossSymbol(GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE",
        nullptr);
    if (sym2) {
        if (GlossHook(sym2, (void*)hook_input2, (void**)&orig_input2)) {
            LOGI("Hooked input (sym2) for LMB/RMB");
            return;
        }
    }
    LOGE("Could not hook input — LMB/RMB will stay dark (Bluetooth mouse only)");
}

// ─────────────────────────────────────────────────────────────────────────────
//  5. GL state save / restore  (prevents corrupting Minecraft's render state)
// ─────────────────────────────────────────────────────────────────────────────
struct GLState {
    GLint  prog, tex, atex, abuf, ebuf, vao, fbo, vp[4], sc[4], bsrc, bdst;
    GLboolean blend, cull, depth, scissor;
};

static void gl_save(GLState& s) {
    glGetIntegerv(GL_CURRENT_PROGRAM,              &s.prog);
    glGetIntegerv(GL_TEXTURE_BINDING_2D,           &s.tex);
    glGetIntegerv(GL_ACTIVE_TEXTURE,               &s.atex);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING,         &s.abuf);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s.ebuf);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING,         &s.vao);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING,          &s.fbo);
    glGetIntegerv(GL_VIEWPORT,                     s.vp);
    glGetIntegerv(GL_SCISSOR_BOX,                  s.sc);
    glGetIntegerv(GL_BLEND_SRC_ALPHA,              &s.bsrc);
    glGetIntegerv(GL_BLEND_DST_ALPHA,              &s.bdst);
    s.blend   = glIsEnabled(GL_BLEND);
    s.cull    = glIsEnabled(GL_CULL_FACE);
    s.depth   = glIsEnabled(GL_DEPTH_TEST);
    s.scissor = glIsEnabled(GL_SCISSOR_TEST);
}

static void gl_restore(const GLState& s) {
    glUseProgram(s.prog);
    glActiveTexture(s.atex);
    glBindTexture(GL_TEXTURE_2D, s.tex);
    glBindBuffer(GL_ARRAY_BUFFER, s.abuf);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s.ebuf);
    glBindVertexArray(s.vao);
    glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    glViewport(s.vp[0], s.vp[1], s.vp[2], s.vp[3]);
    glScissor(s.sc[0], s.sc[1], s.sc[2], s.sc[3]);
    glBlendFunc(s.bsrc, s.bdst);
    s.blend   ? glEnable(GL_BLEND)        : glDisable(GL_BLEND);
    s.cull    ? glEnable(GL_CULL_FACE)    : glDisable(GL_CULL_FACE);
    s.depth   ? glEnable(GL_DEPTH_TEST)   : glDisable(GL_DEPTH_TEST);
    s.scissor ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
}

// ─────────────────────────────────────────────────────────────────────────────
//  6. UI — drawkey  (white bg + black text when pressed, like ZhyKun's original)
// ─────────────────────────────────────────────────────────────────────────────
static void drawkey(const char* lbl, bool pressed, ImVec2 size = ImVec2(60, 60)) {
    ImVec4 bg   = pressed ? ImVec4(1.0f, 1.0f, 1.0f, 0.95f)
                           : ImVec4(0.15f, 0.15f, 0.15f, 0.80f);
    ImVec4 text = pressed ? ImVec4(0.0f, 0.0f, 0.0f, 1.0f)
                           : ImVec4(0.9f, 0.9f, 0.9f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button,        bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  bg);
    ImGui::PushStyleColor(ImGuiCol_Text,          text);
    ImGui::Button(lbl, size);
    ImGui::PopStyleColor(4);
}

// ─────────────────────────────────────────────────────────────────────────────
//  7. Settings window  (separate ImGui window, opened via gear button on HUD)
// ─────────────────────────────────────────────────────────────────────────────
static void draw_settings() {
    if (!g_show_settings) return;

    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(220, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.85f);

    if (ImGui::Begin("Keystroke Settings", &g_show_settings,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar)) {
        ImGui::TextDisabled("Toggle key visibility:");
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Checkbox("W  (Forward)",  &g_settings.show_w);
        ImGui::Checkbox("A  (Left)",     &g_settings.show_a);
        ImGui::Checkbox("S  (Backward)", &g_settings.show_s);
        ImGui::Checkbox("D  (Right)",    &g_settings.show_d);
        ImGui::Spacing();
        ImGui::Checkbox("SPACE (Jump)",  &g_settings.show_space);
        ImGui::Spacing();
        ImGui::Checkbox("LMB  (Attack)", &g_settings.show_lmb);
        ImGui::Checkbox("RMB  (Use)",    &g_settings.show_rmb);
    }
    ImGui::End();
}

// ─────────────────────────────────────────────────────────────────────────────
//  8. HUD window
// ─────────────────────────────────────────────────────────────────────────────
static void render_hud() {
    KeyState k;
    { std::lock_guard<std::mutex> lk(g_keymutex); k = g_keys; }

    const float ks = 60.0f;
    const float sp =  5.0f;

    ImGui::SetNextWindowPos(ImVec2(10, 90), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##HUD", nullptr,
                 ImGuiWindowFlags_NoDecoration    |
                 ImGuiWindowFlags_NoBackground    |
                 ImGuiWindowFlags_AlwaysAutoResize|
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(sp, sp));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);

    // Drag handle
    ImGui::InvisibleButton("##drag", ImVec2(ks * 3 + sp * 2, 8));
    if (ImGui::IsItemActive()) {
        ImVec2 d = ImGui::GetIO().MouseDelta;
        ImVec2 p = ImGui::GetWindowPos();
        ImGui::SetWindowPos(ImVec2(p.x + d.x, p.y + d.y));
    }

    // Gear / settings toggle (top-right)
    ImGui::SameLine(ks * 3 + sp * 2 - 26);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1,1,1,0.12f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1,1,1,0.22f));
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.8f,0.8f,0.8f,0.6f));
    if (ImGui::Button("##gear", ImVec2(24, 24))) g_show_settings = !g_show_settings;
    ImGui::PopStyleColor(4);

    // ── Row 1: W ──────────────────────────────────────────────────────────
    if (g_settings.show_w) {
        ImGui::SetCursorPosX(ks + sp + 5);
        drawkey("W", k.w, ImVec2(ks, ks));
    }

    // ── Row 2: A S D ──────────────────────────────────────────────────────
    bool any_asd = g_settings.show_a || g_settings.show_s || g_settings.show_d;
    if (any_asd) {
        bool first = true;
        auto nextkey = [&](const char* lbl, bool pressed, bool show) {
            if (!show) return;
            if (!first) ImGui::SameLine();
            drawkey(lbl, pressed, ImVec2(ks, ks));
            first = false;
        };
        nextkey("A", k.a, g_settings.show_a);
        nextkey("S", k.s, g_settings.show_s);
        nextkey("D", k.d, g_settings.show_d);
    }

    // ── Row 3: SPACE ──────────────────────────────────────────────────────
    if (g_settings.show_space) {
        drawkey("SPACE", k.space, ImVec2(ks * 3 + sp * 2, 40));
    }

    // ── Row 4: LMB  RMB ───────────────────────────────────────────────────
    bool any_mouse = g_settings.show_lmb || g_settings.show_rmb;
    if (any_mouse) {
        float half = (ks * 3 + sp * 2 - sp) / 2.0f;
        if (g_settings.show_lmb) {
            drawkey("LMB", k.lmb, ImVec2(half, ks));
            if (g_settings.show_rmb) ImGui::SameLine();
        }
        if (g_settings.show_rmb) {
            drawkey("RMB", k.rmb, ImVec2(half, ks));
        }
    }

    ImGui::PopStyleVar(2);
    ImGui::End();
}

// ─────────────────────────────────────────────────────────────────────────────
//  9. EGL swap hook
// ─────────────────────────────────────────────────────────────────────────────
static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;

EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!orig_eglSwapBuffers) return EGL_FALSE;

    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH,  &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w < 500 || h < 500) return orig_eglSwapBuffers(dpy, surf);

    if (!g_initialized) {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;

        // Scale font to screen — same logic as ZhyKun's original
        float scale = (float)h / 720.0f;
        if (scale < 1.5f) scale = 1.5f;
        if (scale > 4.0f) scale = 4.0f;
        ImFontConfig cfg;
        cfg.SizePixels = 28.0f * scale;
        io.Fonts->AddFontDefault(&cfg);

        ImGui_ImplAndroid_Init();
        ImGui_ImplOpenGL3_Init("#version 300 es");
        ImGui::GetStyle().ScaleAllSizes(scale);

        g_initialized = true;
        LOGI("ImGui initialized (%dx%d, scale=%.2f)", w, h, scale);
    }

    GLState gs;
    gl_save(gs);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(w, h);
    ImGui::NewFrame();

    render_hud();
    draw_settings();

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    gl_restore(gs);

    return orig_eglSwapBuffers(dpy, surf);
}

// ─────────────────────────────────────────────────────────────────────────────
//  10. Entry point
// ─────────────────────────────────────────────────────────────────────────────
void* main_thread(void*) {
    sleep(15);
    GlossInit(true);

    // EGL hook
    GHandle hegl = GlossOpen("libEGL.so");
    void* swap = (void*)GlossSymbol(hegl, "eglSwapBuffers", nullptr);
    if (swap) GlossHook(swap, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);

    // NormalTick hook (WASD + SPACE)
    GHandle hmc = GlossOpen("libminecraftpe.so");
    if (hmc) {
        void* tick = (void*)GlossSymbol(hmc, "_ZN11LocalPlayer10normalTickEv", nullptr);
        if (tick) GlossHook(tick, (void*)hook_NormalTick, (void**)&orig_NormalTick);
    }

    // Input hook (LMB + RMB via Bluetooth mouse / USB OTG)
    hook_input();

    return nullptr;
}

__attribute__((constructor)) void init() {
    pthread_t t;
    pthread_create(&t, nullptr, main_thread, nullptr);
}
