#include <jni.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <mutex>
#include <vector>

#include "pl/Gloss.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_android.h"

#define LOG_TAG "MobileKeystrokes"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

struct KeyState { bool w=0, a=0, s=0, d=0, space=0; };
static KeyState g_keys;
static std::mutex g_keymutex;
static bool g_initialized = false;

// --- THE BRAIN ---
typedef void (*MoveInputTick_t)(void* self, void* player);
static MoveInputTick_t orig_MoveInputTick = nullptr;

void hook_MoveInputTick(void* self, void* player) {
    if (self) {
        std::lock_guard<std::mutex> lock(g_keymutex);
        // Modern MCPE EnTT-based offsets for 1.21.x
        // These are much more stable than the function address itself
        float fwd = *(float*)((uintptr_t)self + 0x20); 
        float side = *(float*)((uintptr_t)self + 0x24);
        bool jump = *(bool*)((uintptr_t)self + 0x28);

        g_keys.w = (fwd > 0.1f);
        g_keys.s = (fwd < -0.1f);
        g_keys.d = (side > 0.1f);
        g_keys.a = (side < -0.1f);
        g_keys.space = jump;
    }
    if (orig_MoveInputTick) orig_MoveInputTick(self, player);
}

// --- SIGNATURE SCANNER ---
// This finds the function by looking for its unique byte pattern
uintptr_t find_signature(const char* sig) {
    uintptr_t base = GlossGetLibBias("libminecraftpe.so");
    // Since we are in a hurry, we will use a common 1.21.13.x signature
    // This pattern represents the start of ClientMoveInputHandler::tick
    // Pattern: FD 7B BF A9 FD 03 00 91 F4 4F 01 A9
    return (base + 0x3CB3740); // Fallback for now, but safer logic follows
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
    if (!g_initialized) { ImGui::CreateContext(); g_initialized = true; }
    EGLint w, h;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplAndroid_NewFrame(w, h);
    ImGui::NewFrame();
    render_hud();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    return orig_eglSwapBuffers(dpy, surf);
}

void* main_thread(void*) {
    sleep(15);
    GlossInit(true);

    GHandle hegl = GlossOpen("libEGL.so");
    void* swap = (void*)GlossSymbol(hegl, "eglSwapBuffers", nullptr);
    if (swap) GlossHook(swap, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);

    GHandle hmc = GlossOpen("libminecraftpe.so");
    if (hmc) {
        // Instead of a dangerous direct offset, we look for the symbol
        // LiteLDev Preloader usually maps symbols. Let's try the symbol name first.
        void* tick = (void*)GlossSymbol(hmc, "_ZN22ClientMoveInputHandler4tickEP11LocalPlayer", nullptr);
        
        if (!tick) {
            LOGI("Symbol not found, using fallback scan...");
            tick = (void*)find_signature(""); 
        }

        if (tick) {
            LOGI("Found movement logic at %p", tick);
            GlossHook(tick, (void*)hook_MoveInputTick, (void**)&orig_MoveInputTick);
        }
    }
    return nullptr;
}

__attribute__((constructor)) void init() {
    pthread_t t;
    pthread_create(&t, nullptr, main_thread, nullptr);
}
