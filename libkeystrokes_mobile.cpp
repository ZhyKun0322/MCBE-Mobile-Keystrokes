#include <jni.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <mutex>

// Correct includes based on your folder structure
#include "pl/Gloss.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_android.h"

#define LOG_TAG "MobileKeystrokes"

struct KeyState { bool w=0, a=0, s=0, d=0, space=0; };
static KeyState g_keys;
static std::mutex g_keymutex;
static bool g_initialized = false;

// --- MOVEMENT BRAIN ---
typedef void (*MoveInputTick_t)(void* self, void* player);
static MoveInputTick_t orig_MoveInputTick = nullptr;

void hook_MoveInputTick(void* self, void* player) {
    if (self) {
        std::lock_guard<std::mutex> lock(g_keymutex);
        // Offsets for MCBE 1.21.13.1
        g_keys.w = (*(float*)((uintptr_t)self + 0x20) > 0.1f);
        g_keys.s = (*(float*)((uintptr_t)self + 0x20) < -0.1f);
        g_keys.d = (*(float*)((uintptr_t)self + 0x24) > 0.1f);
        g_keys.a = (*(float*)((uintptr_t)self + 0x24) < -0.1f);
        g_keys.space = *(bool*)((uintptr_t)self + 0x28);
    }
    if (orig_MoveInputTick) orig_MoveInputTick(self, player);
}

void render_hud() {
    ImGui::SetNextWindowPos(ImVec2(100, 100), ImGuiCond_FirstUseEver);
    ImGui::Begin("##HUD", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_AlwaysAutoResize);
    float ks = 60.0f;
    auto DrawKey = [&](const char* lbl, bool act, ImVec2 pos) {
        ImGui::SetCursorPos(pos);
        ImVec4 col = act ? ImVec4(1,1,1,0.8f) : ImVec4(0,0,0,0.4f);
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

static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;

EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!g_initialized) {
        ImGui::CreateContext();
        ImGui_ImplAndroid_Init();
        ImGui_ImplOpenGL3_Init("#version 300 es");
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

void* main_thread(void*) {
    sleep(15);
    GlossInit(true);

    // 1. Hook Graphics
    GHandle hegl = GlossOpen("libEGL.so");
    void* swap = (void*)GlossSymbol(hegl, "eglSwapBuffers", nullptr);
    if (swap) GlossHook(swap, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);

    // 2. Hook Movement - FIXED: Changed GlossBase to GlossGetLibBias
    uintptr_t mc_base = GlossGetLibBias("libminecraftpe.so");
    if (mc_base) {
        void* tick_addr = (void*)(mc_base + 0x3CB3740);
        GlossHook(tick_addr, (void*)hook_MoveInputTick, (void**)&orig_MoveInputTick);
    }
    return nullptr;
}

__attribute__((constructor)) void init() {
    pthread_t t;
    pthread_create(&t, nullptr, main_thread, nullptr);
}
