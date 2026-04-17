// MCBE_MobileKeystrokes_CrashProof.cpp

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
#include <sys/mman.h>

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

#define KNOWN_MIC_OFFSET 0

// ============================================================================
// STRUCTURES
// ============================================================================

struct Vec2 { float x, y; };

struct MoveInputComponent {
    char pad[0x24];
    Vec2 mMove;
    char pad2[0x34];
    unsigned short mFlagValues;
};

// ============================================================================
// GLOBAL STATE
// ============================================================================

struct KeyState {
    bool w = false, a = false, s = false, d = false, space = false;
};

struct KeySettings {
    bool show_w = true, show_a = true, show_s = true, show_d = true, show_space = true;
};

static KeyState g_keys;
static KeySettings g_settings;
static std::mutex g_keyMutex;
static bool g_initialized = false;
static bool g_showSettings = false;
static int g_micOffset = 0;
static void* g_playerPtr = nullptr;
static int g_tickCount = 0;
static bool g_scanning = false;

// ============================================================================
// FUNCTION HOOKS
// ============================================================================

typedef EGLBoolean (*EGLSwapBuffers_t)(EGLDisplay, EGLSurface);
static EGLSwapBuffers_t orig_eglSwapBuffers = nullptr;

// ============================================================================
// SAFE MEMORY SCANNER
// ============================================================================

bool isValidPointer(void* ptr) {
    if (!ptr) return false;
    
    // Check if pointer is readable
    int fd[2];
    if (pipe(fd) == -1) return false;
    
    bool valid = (write(fd[1], ptr, 1) == 1);
    close(fd[0]);
    close(fd[1]);
    
    return valid;
}

void scanMemoryForPlayer() {
    if (g_playerPtr || g_scanning) return;
    g_scanning = true;
    
    LOGI("=== STARTING MEMORY SCAN ===");
    
    // Get MC base
    FILE* maps = fopen("/proc/self/maps", "r");
    if (!maps) {
        g_scanning = false;
        return;
    }
    
    char line[512];
    uintptr_t mcBase = 0;
    while (fgets(line, sizeof(line), maps)) {
        if (strstr(line, "libminecraftpe.so") && strstr(line, "r-xp")) {
            sscanf(line, "%lx", &mcBase);
            break;
        }
    }
    fclose(maps);
    
    if (!mcBase) {
        LOGI("Could not find MC base");
        g_scanning = false;
        return;
    }
    
    LOGI("MC base: 0x%lx", mcBase);
    
    // Re-open maps for heap scanning
    maps = fopen("/proc/self/maps", "r");
    if (!maps) {
        g_scanning = false;
        return;
    }
    
    int foundCount = 0;
    
    while (fgets(line, sizeof(line), maps) && !g_playerPtr) {
        uintptr_t start, end;
        char perms[5];
        
        if (sscanf(line, "%lx-%lx %4s", &start, &end, perms) != 3) continue;
        
        // Look for readable heap regions
        if (perms[0] != 'r') continue; // Not readable
        if (!strstr(line, "[heap]") && !strstr(line, "[anon")) continue;
        
        // Limit scan size to prevent crashes
        size_t regionSize = end - start;
        if (regionSize > 100 * 1024 * 1024) continue; // Skip huge regions (>100MB)
        
        // Scan with bounds checking
        uint64_t* mem = (uint64_t*)start;
        size_t numPtrs = regionSize / 8;
        
        for (size_t i = 0; i < numPtrs && !g_playerPtr; i++) {
            // Check if this looks like a vtable pointer to MC code
            uint64_t val = mem[i];
            
            // Vtable heuristic: pointer to .text section of libminecraftpe.so
            if (val < mcBase || val > mcBase + 0x2000000) continue;
            
            void* potentialObj = (void*)&mem[i];
            
            // Test MIC offsets safely
            const int testOffsets[] = {0x10A8, 0xF80, 0xFA0, 0xFB0, 0xFC0, 0xFD0, 0xFE0, 0xFF0,
                                       0x1000, 0x1008, 0x1010, 0x1018, 0x1020, 0x1028, 0x1030, 0x1038,
                                       0x1040, 0x1048, 0x1050, 0x1058, 0x1060, 0x1068, 0x1070, 0x1078,
                                       0x1080, 0x1088, 0x1090, 0x1098, 0x10A0, 0x10B0, 0x10C0, 0x10D0, 0x10E0, 0x10F0};
            
            for (int off : testOffsets) {
                void** micPtr = (void**)((uintptr_t)potentialObj + off);
                
                // Safe read check
                if (!micPtr) continue;
                
                // Use mincore to check if memory is valid (Linux specific)
                unsigned char vec;
                if (mincore((void*)((uintptr_t)micPtr & ~0xFFF), 4096, &vec) != 0) continue;
                
                void* mic = *micPtr;
                if (!mic) continue;
                
                // Check MIC pointer validity
                if (mincore((void*)((uintptr_t)mic & ~0xFFF), 4096, &vec) != 0) continue;
                
                // Read mMove values safely
                float mx, my;
                
                // Use try-catch equivalent by checking memory first
                volatile float* mMoveX = (volatile float*)((uintptr_t)mic + 0x24);
                volatile float* mMoveY = (volatile float*)((uintptr_t)mic + 0x28);
                
                // Quick validity check - read twice to ensure stable
                float mx1 = *mMoveX;
                float my1 = *mMoveY;
                usleep(1000); // 1ms
                float mx2 = *mMoveX;
                float my2 = *mMoveY;
                
                // Values should be stable (not changing during read)
                if (abs(mx1 - mx2) > 0.01f || abs(my1 - my2) > 0.01f) continue;
                
                mx = mx1;
                my = my1;
                
                // Check if values look like movement (-1 to 1 typically)
                if (mx >= -2.0f && mx <= 2.0f && my >= -2.0f && my <= 2.0f) {
                    foundCount++;
                    LOGI("Candidate #%d: obj=%p off=0x%X mMove=(%.3f, %.3f)",
                         foundCount, potentialObj, off, mx, my);
                    
                    // Use first good candidate
                    g_playerPtr = potentialObj;
                    g_micOffset = off;
                    break;
                }
            }
        }
    }
    
    fclose(maps);
    g_scanning = false;
    
    if (g_playerPtr) {
        LOGI("=== PLAYER FOUND ===");
        LOGI("Player: %p", g_playerPtr);
        LOGI("MIC Offset: 0x%X", g_micOffset);
        LOGI("Move around to verify!");
    } else {
        LOGI("=== PLAYER NOT FOUND, WILL RETRY ===");
    }
}

// ============================================================================
// UPDATE KEYS
// ============================================================================

void updateKeys() {
    if (!g_playerPtr) {
        if (g_tickCount % 180 == 0) { // Try every 3 seconds
            scanMemoryForPlayer();
        }
        g_tickCount++;
        return;
    }
    
    // Safe read with null checks
    void** micPtr = (void**)((uintptr_t)g_playerPtr + g_micOffset);
    if (!micPtr) {
        g_playerPtr = nullptr;
        return;
    }
    
    void* mic = *micPtr;
    if (!mic) {
        g_playerPtr = nullptr;
        return;
    }
    
    // Check if MIC memory is still valid
    unsigned char vec;
    if (mincore((void*)((uintptr_t)mic & ~0xFFF), 4096, &vec) != 0) {
        g_playerPtr = nullptr;
        return;
    }
    
    MoveInputComponent* micComp = (MoveInputComponent*)mic;
    
    std::lock_guard<std::mutex> lock(g_keyMutex);
    
    const float DEADZONE = 0.15f;
    g_keys.w = (micComp->mMove.y < -DEADZONE);
    g_keys.s = (micComp->mMove.y > DEADZONE);
    g_keys.a = (micComp->mMove.x < -DEADZONE);
    g_keys.d = (micComp->mMove.x > DEADZONE);
    g_keys.space = (micComp->mFlagValues & 0x1) != 0;
    
    if (g_tickCount % 120 == 0) {
        LOGI("Keys: W=%d A=%d S=%d D=%d J=%d | mMove=(%.2f,%.2f)",
             g_keys.w, g_keys.a, g_keys.s, g_keys.d, g_keys.space,
             micComp->mMove.x, micComp->mMove.y);
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
                ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "Status: SCANNING...");
                if (g_scanning) {
                    ImGui::Text("Scanning memory...");
                } else {
                    ImGui::Text("Waiting to retry...");
                }
            }
            
            if (ImGui::Button("Force Rescan")) {
                g_playerPtr = nullptr;
                g_micOffset = 0;
            }
        }
        ImGui::End();
    }
}

// ============================================================================
// EGL HOOK
// ============================================================================

EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!orig_eglSwapBuffers) {
        LOGE("orig_eglSwapBuffers is NULL!");
        return EGL_FALSE;
    }
    
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
        LOGI("ImGui initialized, starting scan...");
        
        // First scan
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
    LOGI("MobileKeystrokes 26.13.1 (Crash-Proof)");
    LOGI("========================================");
    
    // Hook EGL
    GHandle hegl = GlossOpen("libEGL.so");
    void* swap = (void*)GlossSymbol(hegl, "eglSwapBuffers", nullptr);
    if (swap) {
        GlossHook(swap, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);
        LOGI("[OK] EGL hooked at %p", swap);
    } else {
        LOGE("[FAIL] EGL hook failed");
    }
    
    LOGI("Enter a world - scanning will begin automatically");
    LOGI("========================================");
    
    return nullptr;
}

__attribute__((constructor)) void init() {
    pthread_t t;
    pthread_create(&t, nullptr, main_thread, nullptr);
}
