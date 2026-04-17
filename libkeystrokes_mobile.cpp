#include <jni.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <mutex>

#include "pl/Gloss.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_android.h"

static uintptr_t g_playerAddr = 0; 
static bool g_initialized = false;

typedef void (*NormalTick)(void* self);
static NormalTick orig_NormalTick = nullptr;

// This only saves the Player address, it DOES NOT look for movement
// This makes it impossible to crash.
void hook_NormalTick(void* player) {
    if (player) {
        g_playerAddr = (uintptr_t)player; 
    }
    if (orig_NormalTick) orig_NormalTick(player);
}

void render_hud() {
    ImGui::SetNextWindowPos(ImVec2(100, 100), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(300, 100), ImGuiCond_Always);
    ImGui::Begin("DEBUG HUD", nullptr, ImGuiWindowFlags_NoDecoration);
    
    if (g_playerAddr == 0) {
        ImGui::Text("Waiting for player...");
    } else {
        ImGui::Text("Player Address: 0x%lX", g_playerAddr);
        ImGui::Text("W A S D logic is OFF for now.");
    }
    
    ImGui::End();
}

static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;
EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    EGLint w, h;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w); eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w < 500 || h < 500) return orig_eglSwapBuffers(dpy, surf);
    if (!g_initialized) { ImGui::CreateContext(); g_initialized = true; }
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
        void* tick = (void*)GlossSymbol(hmc, "_ZN11LocalPlayer10normalTickEv", nullptr);
        if (tick) GlossHook(tick, (void*)hook_NormalTick, (void**)&orig_NormalTick);
    }
    return nullptr;
}
__attribute__((constructor)) void init() { pthread_t t; pthread_create(&t, nullptr, main_thread, nullptr); }
