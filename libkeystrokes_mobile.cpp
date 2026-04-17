// MCBE_MobileKeystrokes_26.13.1_Fixed.cpp
// Complete working version for stripped MCBE 26.13.1

#include <jni.h>
#include <android/log.h>
#include <android/input.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <mutex>
#include <fstream>
#include <vector>

#include "pl/Gloss.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_android.h"

#define LOG_TAG "MobileKeystrokes"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ============================================================================
// CONFIGURATION
// ============================================================================

// If you find the correct MIC offset, set it here
#define KNOWN_MIC_OFFSET 0

// ============================================================================
// STRUCTURES
// ============================================================================

struct Vec2 { float x, y; };

struct MoveInputComponent {
    char pad[0x24];
    Vec2 mMove;                  // 0x24 - movement vector
    char pad2[0x34];
    unsigned short mFlagValues;  // 0x60 - bit 0 = jump
};

// ============================================================================
// GLOBAL STATE
// ============================================================================

struct KeyState {
    bool w = false;
    bool a = false;
    bool s = false;
    bool d = false;
    bool space = false;
};

struct KeySettings {
    bool show_w = true;
    bool show_a = true;
    bool show_s = true;
    bool show_d = true;
    bool show_space = true;
};

static KeyState g_keys;
static KeySettings g_settings;
static std::mutex g_keyMutex;
static bool g_initialized = false;
static bool g_showSettings = false;
static bool g_scanDone = false;
static int g_micOffset = 0;
static void* g_playerPtr = nullptr;
static int g_tickCount = 0;

// ============================================================================
// FUNCTION HOOKS
// ============================================================================

typedef EGLBoolean (*EGLSwapBuffers_t)(EGLDisplay, EGLSurface);
static EGLSwapBuffers_t orig_eglSwapBuffers = nullptr;

// ============================================================================
// MEMORY SCANNER - Find player in heap
// ============================================================================

void scanMemoryForPlayer() {
    if (g_playerPtr) return;
    
    FILE* maps = fopen("/proc/self/maps", "r");
    if (!maps) return;
    
    char line[512];
    LOGI("=== SCANNING MEMORY FOR PLAYER ===");
    
    // Get libminecraftpe.so base for reference
    uintptr_t mcBase = 0;
    FILE* maps2 = fopen("/proc/self/maps", "r");
    while (fgets(line, sizeof(line), maps2)) {
        if (strstr(line, "libminecraftpe.so") && strstr(line, "r-xp")) {
            sscanf(line, "%lx", &mcBase);
            break;
        }
    }
    fclose(maps2);
    
    LOGI("MC base: 0x%lx", mcBase);
    
    // Reset to start of maps
    fseek(maps, 0, SEEK_SET);
    
    while (fgets(line, sizeof(line), maps)) {
        // Look for heap or anon regions
        if (strstr(line, "rw-p") && (strstr(line, "[heap]") || strstr(line, "anon"))) {
            uintptr_t start, end;
            sscanf(line, "%lx-%lx", &start, &end);
            
            // Scan this region
            uint64_t* mem = (uint64_t*)start;
            size_t size = (end - start) / 8;
            
            for (size_t i = 0; i < size; i++) {
                // Look for pointers to libminecraftpe.so (potential vtable)
                if (mem[i] > mcBase && mem[i] < mcBase + 0x10000000) {
                    void* potentialPlayer = (void*)&mem[i];
                    
                    // Test common MIC offsets
                    const int testOffsets[] = {0x10A8, 0xF80, 0xFA0, 0x10B0, 0x10C0, 0x10D0, 0x10E0};
                    
                    for (int off : testOffsets) {
                        void** micPtr = (void**)((uintptr_t)potentialPlayer + off);
                        if (!micPtr || !*micPtr) continue;
                        
                        uintptr_t mic = (uintptr_t)*micPtr;
                        
                        // MIC should be in readable heap memory
                        if (mic < 0x1000000000 || mic > 0x800000000000) continue;
                        
                        float mx = *(float*)(mic + 0x24);
                        float my = *(float*)(mic + 0x28);
                        
                        // Check if values look like movement
                        if (mx > -2 && mx < 2 && my > -2 && my < 2) {
                            LOGI("Found: player=%p offset=0x%X mMove=(%.2f, %.2f)",
                                 potentialPlayer, off, mx, my);
                            
                            g_playerPtr = potentialPlayer;
                            g_micOffset = off;
                            fclose(maps);
                            return;
                        }
                    }
                }
            }
        }
    }
    
    fclose(maps);
    LOGI("Player not found yet, will retry...");
}

// ============================================================================
// UPDATE KEYS
// ============================================================================

void updateKeys() {
    if (!g_playerPtr) {
        if (g_tickCount % 300 == 0) {
            scanMemoryForPlayer();
        }
        g_tickCount++;
        return;
    }
    
    // Read MIC
    void** micPtr = (void**)((uintptr_t)g_playerPtr + g_micOffset);
    if (!micPtr || !*micPtr) {
        g_playerPtr = nullptr;
        return;
    }
    
    MoveInputComponent* mic = (MoveInputComponent*)*micPtr;
    
    std::lock_guard<std::mutex> lock(g_keyMutex);
    
    const float DEADZONE = 0.15f;
    g_keys.w = (mic->mMove.y < -DEADZONE);
    g_keys.s = (mic->mMove.y > DEADZONE);
    g_keys.a = (mic->mMove.x < -DEADZONE);
    g_keys.d = (mic->mMove.x > DEADZONE);
    g_keys.space = (mic->mFlagValues & 0x1) != 0;
    
    if (g_tickCount % 120 == 0) {
        LOGI("Keys: W=%d A=%d S=%d D=%d J=%d | mMove=(%.2f,%.2f)",
             g_keys.w, g_keys.a, g_keys.s, g_keys.d, g_keys.space,
             mic->mMove.x, mic->mMove.y);
    }
    g_tickCount++;
}

// ============================================================================
// UI RENDERING
// ============================================================================

static void drawKey(const char* lbl, bool pressed, ImVec2 size = ImVec2(60, 60)) {
    ImVec4 bg = pressed ? ImVec4(1.0f, 1.0f, 1.0f, 0.95f) : ImVec4(0.15f, 0.15f, 0.15f, 0.80f);
    ImVec4 text = pressed ? ImVec4(0.0f, 0.0f, 0.0f, 1.0f) : ImVec4(0.9f, 0.9f, 0.9f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, bg);
    ImGui::PushStyleColor(ImGuiCol_Text, text);
    ImGui::Button(lbl, size);
    ImGui::PopStyleColor(4);
}

static void renderHUD() {
    KeyState k;
    { std::lock_guard<std::mutex> lk(g_keyMutex); k = g_keys; }
    
    const float ks = 60.0f;
    const float sp = 5.0f;
    float tw = ks * 3 + sp * 2;
    
    ImGui::SetNextWindowPos(ImVec2(10, 90), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##HUD", nullptr,
                 ImGuiWindowFlags_NoDecoration |
                 ImGuiWindowFlags_NoBackground |
                 ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);
    
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(sp, sp));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    
    // Drag handle
    ImGui::InvisibleButton("##drag", ImVec2(tw, 8));
    if (ImGui::IsItemActive()) {
        ImVec2 delta = ImGui::GetIO().MouseDelta;
        ImVec2 pos = ImGui::GetWindowPos();
        ImGui::SetWindowPos(ImVec2(pos.x + delta.x, pos.y + delta.y));
    }
    
    // Settings button
    ImGui::SameLine(tw - 26);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.12f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.22f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.8f, 0.8f, 0.6f));
    if (ImGui::Button("S", ImVec2(24, 24))) g_showSettings = !g_showSettings;
    ImGui::PopStyleColor(4);
    
    // W
    ImGui::SetCursorPosX((tw - ks) / 2.0f);
    drawKey("W", k.w, ImVec2(ks, ks));
    
    // A S D
    drawKey("A", k.a, ImVec2(ks, ks));
    ImGui::SameLine();
    drawKey("S", k.s, ImVec2(ks, ks));
    ImGui::SameLine();
    drawKey("D", k.d, ImVec2(ks, ks));
    
    // SPACE
    drawKey("SPACE", k.space, ImVec2(tw, 40));
    
    ImGui::PopStyleVar(2);
    ImGui::End();
    
    // Settings window
    if (g_showSettings) {
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(250, 0), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.90f);
        
        if (ImGui::Begin("Keystroke Settings", &g_showSettings,
                         ImGuiWindowFlags_AlwaysAutoResize)) {
            
            ImGui::TextDisabled("Toggle Keys:");
            ImGui::Separator();
            ImGui::Checkbox("Show W", &g_settings.show_w);
            ImGui::Checkbox("Show A", &g_settings.show_a);
            ImGui::Checkbox("Show S", &g_settings.show_s);
            ImGui::Checkbox("Show D", &g_settings.show_d);
            ImGui::Spacing();
            ImGui::Checkbox("Show SPACE", &g_settings.show_space);
            
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextDisabled("Debug Info:");
            
            if (g_playerPtr) {
                ImGui::TextColored(ImVec4(0, 1, 0, 1), "Status: ACTIVE");
                ImGui::Text("MIC Offset: 0x%X", g_micOffset);
                ImGui::Text("Player: %p", g_playerPtr);
                ImGui::Text("W:%d A:%d S:%d D:%d J:%d",
                           g_keys.w, g_keys.a, g_keys.s, g_keys.d, g_keys.space);
            } else {
                ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "Status: SEARCHING...");
                ImGui::Text("Scanning memory...");
            }
            
            if (ImGui::Button("Force Rescan")) {
                g_playerPtr = nullptr;
                g_micOffset = 0;
                scanMemoryForPlayer();
            }
        }
        ImGui::End();
    }
}

// ============================================================================
// EGL HOOK - FIXED RETURN TYPE
// ============================================================================

EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!orig_eglSwapBuffers) return EGL_FALSE;
    
    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w < 500 || h < 500) return orig_eglSwapBuffers(dpy, surf);
    
    // Init ImGui once
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
        LOGI("ImGui initialized");
        
        // Start scanning
        scanMemoryForPlayer();
    }
    
    // Save GL state
    GLint last_program, last_texture, last_array_buffer, last_vertex_array;
    glGetIntegerv(GL_CURRENT_PROGRAM, &last_program);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_texture);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &last_array_buffer);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &last_vertex_array);
    
    // Update and render
    updateKeys();
    
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(w, h);
    ImGui::NewFrame();
    renderHUD();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    
    // Restore GL state
    glUseProgram(last_program);
    glBindTexture(GL_TEXTURE_2D, last_texture);
    glBindBuffer(GL_ARRAY_BUFFER, last_array_buffer);
    glBindVertexArray(last_vertex_array);
    
    return orig_eglSwapBuffers(dpy, surf);
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void* main_thread(void*) {
    sleep(15);
    GlossInit(true);
    
    LOGI("========================================");
    LOGI("MobileKeystrokes 26.13.1 (Runtime Scanner)");
    LOGI("========================================");
    
    // Hook EGL
    GHandle hegl = GlossOpen("libEGL.so");
    void* swap = (void*)GlossSymbol(hegl, "eglSwapBuffers", nullptr);
    if (swap) {
        GlossHook(swap, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);
        LOGI("[OK] EGL hooked");
    } else {
        LOGE("[FAIL] EGL hook failed");
    }
    
    LOGI("Enter a world - auto-scan will begin");
    LOGI("========================================");
    
    return nullptr;
}

__attribute__((constructor)) void init() {
    pthread_t t;
    pthread_create(&t, nullptr, main_thread, nullptr);
}
