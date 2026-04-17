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
#include <algorithm>

#include "pl/Hook.h"
#include "pl/Gloss.h"
#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

#define LOG_TAG "Keystrokes"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)

// --- MOBILE BRAIN DATA ---
struct KeyState {
    bool w = false, a = false, s = false, d = false, space = false;
};
static KeyState g_keys;
static std::mutex g_keymutex;

static bool g_initialized = false;
static int g_width = 0, g_height = 0;
static EGLContext g_targetcontext = EGL_NO_CONTEXT;
static EGLSurface g_targetsurface = EGL_NO_SURFACE;

// --- MOVEMENT BRAIN (MOBILE LOGIC) ---
typedef void (*MoveInputTick_t)(void* self, void* player);
static MoveInputTick_t orig_MoveInputTick = nullptr;

void hook_MoveInputTick(void* self, void* player) {
    if (self) {
        std::lock_guard<std::mutex> lock(g_keymutex);
        // Reading joystick/touch intent (Verified for 1.21.13.1)
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

// --- UI RENDERING (PC STYLE) ---
static void drawkey(const char* label, bool pressed, ImVec2 size) {
    ImVec4 color = pressed ? ImVec4(0.85f, 0.85f, 0.85f, 0.95f) : ImVec4(0.18f, 0.20f, 0.22f, 0.88f);
    ImVec4 textcolor = pressed ? ImVec4(0.05f, 0.05f, 0.05f, 1.0f) : ImVec4(0.90f, 0.90f, 0.90f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, color);
    ImGui::PushStyleColor(ImGuiCol_Text, textcolor);
    ImGui::Button(label, size);
    ImGui::PopStyleColor(2);
}

static void drawmenu() {
    KeyState k;
    { std::lock_guard<std::mutex> lock(g_keymutex); k = g_keys; }

    ImGui::SetNextWindowPos(ImVec2(100, 100), ImGuiCond_FirstUseEver);
    ImGui::Begin("##ks", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs);

    float ks = 50.0f;
    float spacing = 5.0f;

    ImGui::SetCursorPosX(ks + spacing);
    drawkey("W", k.w, ImVec2(ks, ks));

    drawkey("A", k.a, ImVec2(ks, ks)); ImGui::SameLine();
    drawkey("S", k.s, ImVec2(ks, ks)); ImGui::SameLine();
    drawkey("D", k.d, ImVec2(ks, ks));

    drawkey("_____", k.space, ImVec2(ks * 3 + spacing * 2, ks * 0.6f));

    ImGui::End();
}

static void setup() {
    if (g_initialized) return;
    ImGui::CreateContext();
    ImGui_ImplAndroid_Init();
    ImGui_ImplOpenGL3_Init("#version 300 es");
    g_initialized = true;
}

// --- STABILITY FIX: PC-STYLE GRAPHICS CHECK ---
static EGLBoolean (*orig_eglswapbuffers)(EGLDisplay, EGLSurface) = nullptr;

static EGLBoolean hook_eglswapbuffers(EGLDisplay dpy, EGLSurface surf) {
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return orig_eglswapbuffers(dpy, surf);

    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH,  &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);

    // Safety: If surface is too small, game hasn't fully loaded context yet
    if (w < 500 || h < 500) return orig_eglswapbuffers(dpy, surf);

    if (g_targetcontext == EGL_NO_CONTEXT) { 
        g_targetcontext = ctx; 
        g_targetsurface = surf; 
    }

    if (ctx == g_targetcontext && surf == g_targetsurface) {
        g_width = w; g_height = h;
        setup();

        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2((float)w, (float)h);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplAndroid_NewFrame(w, h);
        ImGui::NewFrame();
        
        drawmenu();

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }

    return orig_eglswapbuffers(dpy, surf);
}

static void* mainthread(void*) {
    // Wait for game to fully initialize (PC used 5, we use 10 for safety)
    sleep(10);

    GlossInit(true);

    // 1. Hook Graphics (Using PC's method)
    GHandle hegl = GlossOpen("libEGL.so");
    void* swap = (void*)GlossSymbol(hegl, "eglSwapBuffers", nullptr);
    if (swap) GlossHook(swap, (void*)hook_eglswapbuffers, (void**)&orig_eglswapbuffers);

    // 2. Hook Mobile Brain (Joystick Logic)
    GHandle hmc = GlossOpen("libminecraftpe.so");
    if (hmc) {
        // Offset for 1.21.13.1
        uintptr_t bias = GlossGetLibBias("libminecraftpe.so");
        void* tick = (void*)(bias + 0x3CB3740);
        
        GlossHook(tick, (void*)hook_MoveInputTick, (void**)&orig_MoveInputTick);
        LOGI("Mobile Movement Brain Connected!");
    }

    return nullptr;
}

__attribute__((constructor))
void keystrokes_init() {
    pthread_t t;
    pthread_create(&t, nullptr, mainthread, nullptr);
}
