// MCBE_MobileKeystrokes_Runtime.cpp
// Works on stripped 26.13.1 by finding player pointer at runtime

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
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ============================================================================
// CONFIGURATION
// ============================================================================

// If you find the correct MIC offset, set it here and recompile
// Example: 0x10A8, 0xF80, etc.
#define KNOWN_MIC_OFFSET 0

// ============================================================================
// STRUCTURES
// ============================================================================

struct Vec2 { float x, y; };

struct MoveInputComponent {
    char pad[0x24];
    Vec2 mMove;                  // 0x24 - movement
    char pad2[0x34];
    unsigned short mFlagValues;  // 0x60 - bit 0 = jump
};

// ============================================================================
// GLOBAL STATE
// ============================================================================

struct KeyState {
    bool w=false, a=false, s=false, d=false, space=false;
};

static KeyState g_keys;
static std::mutex g_keyMutex;
static bool g_initialized=false;
static bool g_showSettings=false;
static int g_micOffset=0;
static void* g_playerPtr=nullptr;
static int g_tickCount=0;

// ============================================================================
// FUNCTION HOOKS
// ============================================================================

typedef void (*EGLSwapBuffers_t)(EGLDisplay, EGLSurface);
static EGLSwapBuffers_t orig_eglSwapBuffers=nullptr;

// ============================================================================
// CRITICAL: This scans memory to find player and MIC
// ============================================================================

void scanMemoryForPlayer() {
    // Read /proc/self/maps to find heap regions
    FILE* maps = fopen("/proc/self/maps", "r");
    if (!maps) return;
    
    char line[512];
    LOGI("=== SCANNING MEMORY FOR PLAYER ===");
    
    while (fgets(line, sizeof(line), maps)) {
        // Look for heap or anon regions that are readable/writable
        if (strstr(line, "rw-p") && (strstr(line, "[heap]") || strstr(line, "anon"))) {
            uintptr_t start, end;
            sscanf(line, "%lx-%lx", &start, &end);
            
            // Scan this region for potential player objects
            // Look for patterns that match LocalPlayer vtable or structure
            uint64_t* mem = (uint64_t*)start;
            size_t size = (end - start) / 8;
            
            for (size_t i = 0; i < size; i++) {
                // Heuristic: Look for pointer to libminecraftpe.so code section
                // vtable pointers typically point to .text section
                if (mem[i] > 0x6c00000000 && mem[i] < 0x6d00000000) {
                    // Potential object, check if it has MIC at common offsets
                    void* potentialPlayer = (void*)&mem[i];
                    
                    // Try common MIC offsets
                    const int testOffsets[] = {0x10A8, 0xF80, 0xFA0, 0x10B0, 0x10C0};
                    
                    for (int off : testOffsets) {
                        void** micPtr = (void**)((uintptr_t)potentialPlayer + off);
                        if (!micPtr || !*micPtr) continue;
                        
                        uintptr_t mic = (uintptr_t)*micPtr;
                        if (mic < start || mic > end) continue; // MIC should be in heap
                        
                        float mx = *(float*)(mic + 0x24);
                        float my = *(float*)(mic + 0x28);
                        
                        // Check if values look like movement (-1 to 1 range typically)
                        if (mx > -2 && mx < 2 && my > -2 && my < 2) {
                            LOGI("Potential player at %p, MIC offset 0x%X, mMove=(%.2f, %.2f)",
                                 potentialPlayer, off, mx, my);
                            
                            // If this is the first valid one, use it
                            if (!g_playerPtr) {
                                g_playerPtr = potentialPlayer;
                                g_micOffset = off;
                                LOGI("*** USING: player=%p, offset=0x%X ***", 
                                     g_playerPtr, g_micOffset);
                            }
                        }
                    }
                }
            }
        }
    }
    
    fclose(maps);
}

// ============================================================================
// UPDATE KEYS - Called every frame from EGL hook
// ============================================================================

void updateKeys() {
    if (!g_playerPtr) {
        // Try to find player every 5 seconds if not found
        if (g_tickCount % 300 == 0) {
            scanMemoryForPlayer();
        }
        g_tickCount++;
        return;
    }
    
    // Read MIC
    void** micPtr = (void**)((uintptr_t)g_playerPtr + g_micOffset);
    if (!micPtr || !*micPtr) {
        g_playerPtr = nullptr; // Reset if invalid
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
    
    // Debug output
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

static void drawKey(const char* lbl, bool pressed, ImVec2 size = ImVec2(60,60)) {
    ImVec4 bg = pressed ? ImVec4(1,1,1,0.95f) : ImVec4(0.15f,0.15f,0.15f,0.80f);
    ImVec4 text = pressed ? ImVec4(0,0,0,1) : ImVec4(0.9f,0.9f,0.9f,1);
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
    
    const float ks = 60.0f, sp = 5.0f;
    float tw = ks * 3 + sp * 2;
    
    ImGui::SetNextWindowPos(ImVec2(10,90), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##HUD", nullptr,
                 ImGuiWindowFlags_NoDecoration |
                 ImGuiWindowFlags_NoBackground |
                 ImGuiWindowFlags_AlwaysAutoResize);
    
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(sp,sp));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    
    // Settings button
    ImGui::SameLine(tw - 26);
    if (ImGui::Button("S", ImVec2(24,24))) g_showSettings = !g_showSettings;
    
    // W
    float wPos = (tw - ks) / 2.0f;
    ImGui::SetCursorPosX(wPos);
    drawKey("W", k.w, ImVec2(ks,ks));
    
    // A S D
    drawKey("A", k.a, ImVec2(ks,ks));
    ImGui::SameLine();
    drawKey("S", k.s, ImVec2(ks,ks));
    ImGui::SameLine();
    drawKey("D", k.d, ImVec2(ks,ks));
    
    // SPACE
    drawKey("SPACE", k.space, ImVec2(tw,40));
    
    ImGui::PopStyleVar(2);
    ImGui::End();
    
    // Settings window
    if (g_showSettings) {
        ImGui::Begin("Settings", &g_showSettings, ImGuiWindowFlags_AlwaysAutoResize);
        
        if (g_playerPtr) {
            ImGui::TextColored(ImVec4(0,1,0,1), "Status: ACTIVE");
            ImGui::Text("Player: %p", g_playerPtr);
            ImGui::Text("MIC Offset: 0x%X", g_micOffset);
            ImGui::Text("Keys: W=%d A=%d S=%d D=%d J=%d",
                        g_keys.w, g_keys.a, g_keys.s, g_keys.d, g_keys.space);
        } else {
            ImGui::TextColored(ImVec4(1,0.5f,0,1), "Status: SEARCHING...");
            ImGui::Text("Scanning memory for player...");
        }
        
        if (ImGui::Button("Force Rescan")) {
            g_playerPtr = nullptr;
            g_micOffset = 0;
            scanMemoryForPlayer();
        }
        
        ImGui::End();
    }
}

// ============================================================================
// EGL HOOK - Main entry point every frame
// ============================================================================

EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!orig_eglSwapBuffers) return EGL_FALSE;
    
    EGLint w=0,h=0;
    eglQuerySurface(dpy,surf,EGL_WIDTH,&w);
    eglQuerySurface(dpy,surf,EGL_HEIGHT,&h);
    if (w<500 || h<500) return orig_eglSwapBuffers(dpy,surf);
    
    // Init ImGui once
    if (!g_initialized) {
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename=nullptr;
        float scale = (float)h / 720.0f;
        if (scale<1.5f) scale=1.5f;
        if (scale>4.0f) scale=4.0f;
        ImFontConfig cfg; cfg.SizePixels=28.0f*scale;
        ImGui::GetIO().Fonts->AddFontDefault(&cfg);
        ImGui_ImplAndroid_Init();
        ImGui_ImplOpenGL3_Init("#version 300 es");
        ImGui::GetStyle().ScaleAllSizes(scale);
        g_initialized=true;
        LOGI("ImGui initialized");
        
        // Start scanning for player
        scanMemoryForPlayer();
    }
    
    // Update keystrokes every frame
    updateKeys();
    
    // Render UI
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(w,h);
    ImGui::NewFrame();
    renderHUD();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    
    return orig_eglSwapBuffers(dpy,surf);
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
        LOGI("[OK] EGL hooked - scanning will start on first frame");
    } else {
        LOGE("[FAIL] EGL hook failed");
    }
    
    LOGI("Enter a world and the mod will auto-scan for player");
    LOGI("========================================");
    
    return nullptr;
}

__attribute__((constructor)) void init() {
    pthread_t t;
    pthread_create(&t, nullptr, main_thread, nullptr);
}
