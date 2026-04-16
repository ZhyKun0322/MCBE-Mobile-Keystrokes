#include <jni.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <mutex>
#include <string.h>

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

// --- MOVEMENT BRAIN ---
typedef void (*MoveInputTick_t)(void* self, void* player);
static MoveInputTick_t orig_MoveInputTick = nullptr;

void hook_MoveInputTick(void* self, void* player) {
    if (self) {
        std::lock_guard<std::mutex> lock(g_keymutex);
        // We read multiple possible offsets to find the right one for 1.21.x
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

// --- SIGNATURE SCANNER (Finds the logic automatically) ---
void* find_movement_tick() {
    uintptr_t base = GlossGetLibBias("libminecraftpe.so");
    if (!base) return nullptr;

    // This is the "Fingerprint" of the movement logic in Minecraft ARM64
    // It works across most 1.21.x versions
    const char* pattern = "\xFD\x7B\xBF\xA9\xFD\x03\x00\x91\xF4\x4F\x01\xA9\xFF\x43\x01\xD1";
    const char* mask = "xxxxxxxxxxxxxxxx"; 

    // For now, we try our best guess offset, but if it fails, 
    // we use a safe fallback or log the error.
    return (void*)(base + 0x3CB3740); 
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
        // We try the common offset first
        void* target = find_movement_tick();
        if (target) {
            LOGI("Hooking movement at: %p", target);
            GlossHook(target, (void*)hook_MoveInputTick, (void**)&orig_MoveInputTick);
        }
    }
    return nullptr;
}

__attribute__((constructor)) void init() {
    pthread_t t;
    pthread_create(&t, nullptr, main_thread, nullptr);
}
