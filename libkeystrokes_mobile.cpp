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

#define LOG_TAG "MobileKeystrokes"

// --- TACO'S STRUCT (ALIGNED TO YOUR DISCOVERY) ---
struct Vec2 { 
    float x; // [0x24] Side (A/D) - YOU FOUND THIS AT ...3F0
    float y; // [0x28] Forward (W/S) - THIS IS AT ...3F4
};

struct MoveInputComponent {
    char filler0[0x24];       // Padding
    Vec2 mMove;               // The movement vector
    char filler1[0x34];       // Padding
    unsigned short mFlags;    // [0x60] Jump flag
};

struct KeyState { bool w=0, a=0, s=0, d=0, space=0; };
static KeyState g_keys;
static std::mutex g_keymutex;
static bool g_initialized = false;

// --- THE BRAIN (FIXED MAPPING) ---
typedef void (*NormalTick)(void* self);
static NormalTick orig_NormalTick = nullptr;

void hook_NormalTick(void* player) {
    if (player) {
        // Offset for MCBE 1.21.13.1
        MoveInputComponent* mic = *(MoveInputComponent**)((uintptr_t)player + 0x10A8); 
        
        if (mic) {
            std::lock_guard<std::mutex> lock(g_keymutex);

            // Based on your discovery: X-axis is Side
            // -1.0 is Left (A), +1.0 is Right (D)
            g_keys.a = (mic->mMove.x < -0.1f);
            g_keys.d = (mic->mMove.x > 0.1f);

            // Y-axis is Forward/Back
            // +1.0 is Forward (W), -1.0 is Backward (S)
            g_keys.w = (mic->mMove.y > 0.1f);
            g_keys.s = (mic->mMove.y < -0.1f);

            // Jump detection (Flag 0x60)
            g_keys.space = (mic->mFlags & 0x1); 
        }
    }
    if (orig_NormalTick) orig_NormalTick(player);
}

// --- UI RENDERING ---
void drawkey(const char* lbl, bool act, ImVec2 size) {
    ImVec4 col = act ? ImVec4(1,1,1,0.8f) : ImVec4(0,0,0,0.4f);
    ImGui::PushStyleColor(ImGuiCol_Button, col);
    ImGui::Button(lbl, size);
    ImGui::PopStyleColor();
}

void render_hud() {
    ImGui::SetNextWindowPos(ImVec2(100, 100), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(210, 220), ImGuiCond_Always);
    ImGui::Begin("##HUD", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground);
    
    float ks = 60.0f;
    float sp = 5.0f;

    // W
    ImGui::SetCursorPosX(ks + sp + 10);
    drawkey("W", g_keys.w, ImVec2(ks, ks));
    
    // A S D
    ImGui::SetCursorPosX(10);
    drawkey("A", g_keys.a, ImVec2(ks, ks)); ImGui::SameLine();
    drawkey("S", g_keys.s, ImVec2(ks, ks)); ImGui::SameLine();
    drawkey("D", g_keys.d, ImVec2(ks, ks));
    
    // SPACE
    ImGui::SetCursorPosX(10);
    drawkey("SPACE", g_keys.space, ImVec2(ks * 3 + (sp * 2), 35));

    ImGui::End();
}

// --- STANDARD HOOKS ---
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

__attribute__((constructor)) void init() {
    pthread_t t;
    pthread_create(&t, nullptr, main_thread, nullptr);
}
