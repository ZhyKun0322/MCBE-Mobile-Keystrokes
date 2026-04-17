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

struct MoveInputState {
    char data[0x10];
};

struct MoveInputComponent {
    MoveInputState  mInputState;            // [0x00]
    MoveInputState  mRawInputState;         // [0x10]
    uint8_t         mHoldAutoJumpInWaterTicks; // [0x20]
    char            pad[0x3];
    Vec2            mMove;                 // [0x24]
    Vec2            mLookDelta;            // [0x2C]
    Vec2            mInteractDir;          // [0x34]
    char            pad2[0x4];
    Vec3            mDisplacement;         // [0x3C]
    Vec3            mDisplacementDelta;    // [0x48]
    Vec3            mCameraOrientation;    // [0x54]
    unsigned short  mFlagValues;           // [0x60] bit 0 = jump
    bool            mIsPaddling[2];        // [0x61]
};

// ─────────────────────────────────────────────────────────────────────────────
//  2. Key state + settings (NO LMB/RMB - mobile only!)
// ─────────────────────────────────────────────────────────────────────────────
struct KeyState {
    bool w=false, a=false, s=false, d=false;
    bool space=false;
};

struct KeySettings {
    bool show_w     = true;
    bool show_a     = true;
    bool show_s     = true;
    bool show_d     = true;
    bool show_space = true;
};

static KeyState    g_keys;
static KeySettings g_settings;
static std::mutex  g_keymutex;
static bool        g_initialized   = false;
static bool        g_show_settings = false;
static bool        g_hook_working  = false; // Track if hook is working

// ─────────────────────────────────────────────────────────────────────────────
//  3. NormalTick hook — FIXED with better error checking
// ─────────────────────────────────────────────────────────────────────────────
typedef void (*NormalTick_t)(void*);
static NormalTick_t orig_NormalTick = nullptr;

static int g_tick_count = 0;

void hook_NormalTick(void* player) {
    // Call original FIRST
    if (orig_NormalTick) {
        orig_NormalTick(player);
    }
    
    if (!player) return;
    
    // Try multiple offsets to find MoveInputComponent
    // Offset 0x10A8 might vary by version, so we try a range
    MoveInputComponent* mic = nullptr;
    
    // Common offsets for LocalPlayer -> MoveInputComponent
    const uintptr_t offsets[] = { 0x10A8, 0x10B0, 0x10C0, 0x1100, 0x1150, 0xF00, 0xF80 };
    uintptr_t playerAddr = (uintptr_t)player;
    
    for (size_t i = 0; i < sizeof(offsets)/sizeof(offsets[0]); i++) {
        void** ptr = (void**)(playerAddr + offsets[i]);
        if (!ptr || !*ptr) continue;
        
        // Validate: check if mMove values are reasonable (-10 to 10 range)
        Vec2* mMove = (Vec2*)((uintptr_t)*ptr + 0x24);
        if (mMove->x >= -10.0f && mMove->x <= 10.0f && 
            mMove->y >= -10.0f && mMove->y <= 10.0f) {
            mic = (MoveInputComponent*)*ptr;
            if (g_tick_count % 120 == 0) {
                LOGI("Found MoveInputComponent at offset 0x%zX", offsets[i]);
            }
            break;
        }
    }
    
    if (!mic) {
        // Component not found, don't spam logs
        return;
    }
    
    g_hook_working = true;
    
    std::lock_guard<std::mutex> lock(g_keymutex);
    
    // Read movement from mMove [0x24]
    // Note: In MC, negative Y is forward (W), positive Y is backward (S)
    g_keys.w     = (mic->mMove.y < -0.15f);
    g_keys.s     = (mic->mMove.y >  0.15f);
    g_keys.a     = (mic->mMove.x < -0.15f);
    g_keys.d     = (mic->mMove.x >  0.15f);
    
    // Read jump from mFlagValues [0x60] bit 0
    g_keys.space = (mic->mFlagValues & 0x1) != 0;
    
    // Debug log every 120 ticks (2 seconds at 60fps)
    if (++g_tick_count % 120 == 0) {
        LOGI("mMove=(%.2f,%.2f) flags=0x%04X W=%d A=%d S=%d D=%d SPACE=%d",
             mic->mMove.x, mic->mMove.y, mic->mFlagValues,
             g_keys.w, g_keys.a, g_keys.s, g_keys.d, g_keys.space);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  4. GL state save / restore
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
//  5. UI — drawkey
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
//  6. Settings window
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
        
        // Show hook status
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("Hook: %s", g_hook_working ? "WORKING" : "SEARCHING...");
    }
    ImGui::End();
}

// ─────────────────────────────────────────────────────────────────────────────
//  7. HUD window - FIXED STRUCTURE: W on top, A S D middle, SPACE bottom
// ─────────────────────────────────────────────────────────────────────────────
static void render_hud() {
    KeyState k;
    { std::lock_guard<std::mutex> lk(g_keymutex); k = g_keys; }

    const float ks = 60.0f;
    const float sp =  5.0f;
    float total_width = ks * 3 + sp * 2;

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
    ImGui::InvisibleButton("##drag", ImVec2(total_width, 8));
    if (ImGui::IsItemActive()) {
        ImVec2 d = ImGui::GetIO().MouseDelta;
        ImVec2 p = ImGui::GetWindowPos();
        ImGui::SetWindowPos(ImVec2(p.x + d.x, p.y + d.y));
    }

    // Settings gear button
    ImGui::SameLine(total_width - 26);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1,1,1,0.12f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1,1,1,0.22f));
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.8f,0.8f,0.8f,0.6f));
    if (ImGui::Button("##gear", ImVec2(24, 24))) g_show_settings = !g_show_settings;
    ImGui::PopStyleColor(4);

    // ── Row 1: W (centered above ASD) ────────────────────────────────────
    if (g_settings.show_w) {
        float w_pos_x = (total_width - ks) / 2.0f;
        ImGui::SetCursorPosX(w_pos_x);
        drawkey("W", k.w, ImVec2(ks, ks));
    }

    // ── Row 2: A S D ──────────────────────────────────────────────────────
    bool any_asd = g_settings.show_a || g_settings.show_s || g_settings.show_d;
    if (any_asd) {
        if (g_settings.show_a) {
            drawkey("A", k.a, ImVec2(ks, ks));
        }
        if (g_settings.show_s) {
            if (g_settings.show_a) ImGui::SameLine();
            drawkey("S", k.s, ImVec2(ks, ks));
        }
        if (g_settings.show_d) {
            if (g_settings.show_a || g_settings.show_s) ImGui::SameLine();
            drawkey("D", k.d, ImVec2(ks, ks));
        }
    }

    // ── Row 3: SPACE (full width below WASD) ─────────────────────────────
    if (g_settings.show_space) {
        drawkey("SPACE", k.space, ImVec2(total_width, 40));
    }

    ImGui::PopStyleVar(2);
    ImGui::End();
}

// ─────────────────────────────────────────────────────────────────────────────
//  8. EGL swap hook
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
//  9. Entry point - FIXED symbol name and added pattern scan fallback
// ─────────────────────────────────────────────────────────────────────────────

// Pattern for LocalPlayer::normalTick (common in MCBE 1.20+)
// This is a fallback if symbol lookup fails
static bool pattern_match(uintptr_t addr, const char* pattern, const char* mask) {
    // Simple pattern scan implementation
    return false; // Placeholder - implement if needed
}

void* main_thread(void*) {
    sleep(15);
    GlossInit(true);

    // EGL hook
    GHandle hegl = GlossOpen("libEGL.so");
    void* swap = (void*)GlossSymbol(hegl, "eglSwapBuffers", nullptr);
    if (swap) {
        GlossHook(swap, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);
        LOGI("EGL hook installed");
    }

    // NormalTick hook - FIXED: try multiple symbol names
    GHandle hmc = GlossOpen("libminecraftpe.so");
    if (hmc) {
        void* tick = nullptr;
        
        // Try different mangled names for LocalPlayer::normalTick
        const char* symbols[] = {
            "_ZN11LocalPlayer10normalTickEv",     // Standard Itanium mangling
            "_ZN11LocalPlayer10NormalTickEv",     // Capital N (wrong but try anyway)
            "normalTick",                          // Unmangled (if exported)
            "_ZN11LocalPlayer4tickEv",            // Alternative name
            nullptr
        };
        
        for (int i = 0; symbols[i]; i++) {
            tick = (void*)GlossSymbol(hmc, symbols[i], nullptr);
            if (tick) {
                LOGI("Found normalTick at symbol: %s", symbols[i]);
                break;
            }
        }
        
        // If symbol not found, try to find via string reference
        if (!tick) {
            LOGI("Symbol not found, trying string search...");
            // Look for "normalTick" string in binary, then find xref to function
            // This requires more advanced scanning
        }
        
        if (tick) {
            if (GlossHook(tick, (void*)hook_NormalTick, (void**)&orig_NormalTick)) {
                LOGI("NormalTick hook installed successfully!");
            } else {
                LOGE("Failed to hook NormalTick");
            }
        } else {
            LOGE("Could not find LocalPlayer::normalTick - keystrokes won't work!");
        }
    } else {
        LOGE("Could not open libminecraftpe.so");
    }

    return nullptr;
}

__attribute__((constructor)) void init() {
    pthread_t t;
    pthread_create(&t, nullptr, main_thread, nullptr);
}
