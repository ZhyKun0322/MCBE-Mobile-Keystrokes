#include <jni.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <mutex>
#include <dlfcn.h>

// Matches your folder structure: include/pl/
#include "pl/Gloss.h"
#include "pl/Hook.h"
#include "imgui.h"

#define LOG_TAG "MobileKeystrokes"

// --- DATA STRUCTURE ---
struct KeyState { 
    bool w = false, a = false, s = false, d = false, space = false; 
};
static KeyState g_keys;
static std::mutex g_keymutex;
static bool g_initialized = false;

// --- THE BRAIN (Movement Hook) ---
// This replaces physical keys with Joystick/D-pad logic
typedef void (*MoveInputTick_t)(void* self, void* player);
static MoveInputTick_t orig_MoveInputTick = nullptr;

void hook_MoveInputTick(void* self, void* player) {
    if (self) {
        std::lock_guard<std::mutex> lock(g_keymutex);
        
        // Offsets for Minecraft 1.21.13.1 (Mobile arm64-v8a)
        float fwd = *(float*)((uintptr_t)self + 0x20);  // moveForward
        float side = *(float*)((uintptr_t)self + 0x24); // moveSide
        bool jump = *(bool*)((uintptr_t)self + 0x28);   // isJumping

        // Lighting up the keys based on movement logic
        g_keys.w = (fwd > 0.1f);
        g_keys.s = (fwd < -0.1f);
        g_keys.d = (side > 0.1f);
        g_keys.a = (side < -0.1f);
        g_keys.space = jump;
    }
    if (orig_MoveInputTick) orig_MoveInputTick(self, player);
}

// --- THE DISPLAY (ImGui HUD) ---
void render_hud() {
    ImGui::SetNextWindowPos(ImVec2(100, 100), ImGuiCond_FirstUseEver);
    ImGui::Begin("##HUD", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_AlwaysAutoResize);
    
    float ks = 60.0f; // Key Size
    auto DrawKey = [&](const char* lbl, bool active, ImVec2 pos) {
        ImGui::SetCursorPos(pos);
        // Active = White, Idle = Transparent Black
        ImVec4 col = active ? ImVec4(1.0f, 1.0f, 1.0f, 0.8f) : ImVec4(0.0f, 0.0f, 0.0f, 0.4f);
        ImGui::PushStyleColor(ImGuiCol_Button, col);
        ImGui::Button(lbl, ImVec2(ks, ks));
        ImGui::PopStyleColor();
    };

    DrawKey("W", g_keys.w, ImVec2(ks + 5, 0));
    DrawKey("A", g_keys.a, ImVec2(0, ks + 5));
    DrawKey("S", g_keys.s, ImVec2(ks + 5, ks + 5));
    DrawKey("D", g_keys.d, ImVec2((ks + 5) * 2, ks + 5));
    
    ImGui::End();
}

// --- GRAPHICS HOOK ---
static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;

EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!g_initialized) {
        ImGui::CreateContext();
        // Setup ImGui here if needed (Style, Fonts)
        g_initialized = true;
    }

    EGLint w, h;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(w, h);
    ImGui::NewFrame();
    
    render_hud();
    
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    return orig_eglSwapBuffers(dpy, surf);
}

// --- INITIALIZATION ---
void* main_thread(void*) {
    // Wait for the game to load its libraries
    sleep(15);
    
    GlossInit(true);

    // 1. Hook the Screen (eglSwapBuffers)
    GHandle hegl = GlossOpen("libEGL.so");
    void* swap = (void*)GlossSymbol(hegl, "eglSwapBuffers", nullptr);
    if (swap) GlossHook(swap, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);

    // 2. Hook the Movement (MoveInputHandler)
    GHandle hmc = GlossOpen("libminecraftpe.so");
    if (hmc) {
        // This is the address for 1.21.13.1
        void* tick_addr = (void*)((uintptr_t)GlossBase(hmc) + 0x3CB3740);
        GlossHook(tick_addr, (void*)hook_MoveInputTick, (void**)&orig_MoveInputTick);
    }

    return nullptr;
}

__attribute__((constructor)) void init() {
    pthread_t t;
    pthread_create(&t, nullptr, main_thread, nullptr);
}
