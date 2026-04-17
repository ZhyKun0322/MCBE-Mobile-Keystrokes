#include <jni.h>
#include <android/log.h>
#include <android/input.h>
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
// CONFIGURATION - MODIFY THESE AFTER FINDING THE OFFSET
// ============================================================================

// Set this to 0 to auto-scan, or set to the found offset (e.g., 0x10A8)
#define KNOWN_MIC_OFFSET 0

// Enable detailed scanning logs
#define DEBUG_SCAN 1

// ============================================================================
// GAME STRUCTURES
// ============================================================================

struct Vec2 { float x, y; };
struct Vec3 { float x, y, z; };

struct MoveInputState {
    char data[0x10];
};

struct MoveInputComponent {
    MoveInputState  mInputState;            // 0x00
    MoveInputState  mRawInputState;         // 0x10
    uint8_t         mHoldAutoJumpInWaterTicks; // 0x20
    char            pad[0x3];               // 0x21
    Vec2            mMove;                  // 0x24 - MOVEMENT VECTOR
    Vec2            mLookDelta;             // 0x2C
    Vec2            mInteractDir;           // 0x34
    char            pad2[0x4];              // 0x3C
    Vec3            mDisplacement;          // 0x40
    Vec3            mDisplacementDelta;       // 0x4C
    Vec3            mCameraOrientation;     // 0x58
    unsigned short  mFlagValues;            // 0x60 - bit 0 = jump
    bool            mIsPaddling[2];         // 0x62
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

static KeyState         g_keys;
static KeySettings      g_settings;
static std::mutex       g_keyMutex;
static bool             g_initialized = false;
static bool             g_showSettings = false;
static bool             g_scanComplete = false;
static int              g_micOffset = 0;
static void*            g_playerPtr = nullptr;
static int              g_tickCount = 0;
static int              g_foundOffset = 0;

// ============================================================================
// FUNCTION HOOKS
// ============================================================================

typedef void (*NormalTick_t)(void*);
static NormalTick_t orig_NormalTick = nullptr;

// ============================================================================
// CRITICAL: OFFSET SCANNER - Run this to find the correct offset
// ============================================================================

void scanForMIC(void* player) {
    if (g_scanComplete) return;
    
    uintptr_t playerAddr = (uintptr_t)player;
    
    LOGI("========================================");
    LOGI("SCANNING FOR MOVEINPUTCOMPONENT OFFSET");
    LOGI("Player base: %p", player);
    LOGI("========================================");
    LOGI("Stand still, then move around to see which offset changes!");
    LOGI("========================================");
    
    // Extended offset list covering all likely locations
    const int offsets[] = {
        // Low offsets (older versions)
        0xF00, 0xF08, 0xF10, 0xF18, 0xF20, 0xF28, 0xF30, 0xF38,
        0xF40, 0xF48, 0xF50, 0xF58, 0xF60, 0xF68, 0xF70, 0xF78,
        0xF80, 0xF88, 0xF90, 0xF98, 0xFA0, 0xFA8, 0xFB0, 0xFB8,
        0xFC0, 0xFC8, 0xFD0, 0xFD8, 0xFE0, 0xFE8, 0xFF0, 0xFF8,
        
        // Mid offsets (common in recent versions)
        0x1000, 0x1008, 0x1010, 0x1018, 0x1020, 0x1028, 0x1030, 0x1038,
        0x1040, 0x1048, 0x1050, 0x1058, 0x1060, 0x1068, 0x1070, 0x1078,
        0x1080, 0x1088, 0x1090, 0x1098, 0x10A0, 0x10A8, 0x10B0, 0x10B8,
        0x10C0, 0x10C8, 0x10D0, 0x10D8, 0x10E0, 0x10E8, 0x10F0, 0x10F8,
        
        // High offsets (if structure grew)
        0x1100, 0x1108, 0x1110, 0x1118, 0x1120, 0x1128, 0x1130, 0x1138,
        0x1140, 0x1148, 0x1150, 0x1158, 0x1160, 0x1168, 0x1170, 0x1178,
        0x1180, 0x1188, 0x1190, 0x1198, 0x11A0, 0x11A8, 0x11B0, 0x11B8,
        0x11C0, 0x11C8, 0x11D0, 0x11D8, 0x11E0, 0x11E8, 0x11F0, 0x11F8,
        0x1200, 0x1208, 0x1210, 0x1218, 0x1220, 0x1228, 0x1230, 0x1238,
    };
    
    int candidateCount = 0;
    
    for (size_t i = 0; i < sizeof(offsets)/sizeof(offsets[0]); i++) {
        int offset = offsets[i];
        void** ptr = (void**)(playerAddr + offset);
        
        if (!ptr || !*ptr) continue;
        
        uintptr_t micAddr = (uintptr_t)*ptr;
        
        // Validate pointer is in heap range
        if (micAddr < 0x1000000000 || micAddr > 0x700000000000) continue;
        
        // Read potential mMove values
        float* mMoveX = (float*)(micAddr + 0x24);
        float* mMoveY = (float*)(micAddr + 0x28);
        
        // Check for valid float values
        if (*mMoveX < -1000 || *mMoveX > 1000) continue;
        if (*mMoveY < -1000 || *mMoveY > 1000) continue;
        
        // Read flags
        unsigned short* flags = (unsigned short*)(micAddr + 0x60);
        
        candidateCount++;
        
        #if DEBUG_SCAN
        LOGI("[0x%04X] mMove=(%+.3f, %+.3f) flags=0x%04X", 
             offset, *mMoveX, *mMoveY, *flags);
        #endif
        
        // Auto-select first reasonable candidate
        if (g_micOffset == 0) {
            g_micOffset = offset;
            g_foundOffset = offset;
            LOGI("*** AUTO-SELECTED offset 0x%X ***", offset);
        }
    }
    
    LOGI("========================================");
    LOGI("Found %d candidates", candidateCount);
    LOGI("Selected offset: 0x%X", g_micOffset);
    LOGI("Now MOVE in-game and check if values change!");
    LOGI("If not, restart and set KNOWN_MIC_OFFSET manually");
    LOGI("========================================");
    
    g_scanComplete = true;
}

// ============================================================================
// NORMALTICK HOOK
// ============================================================================

void hook_NormalTick(void* player) {
    // Call original first
    if (orig_NormalTick) {
        orig_NormalTick(player);
    }
    
    if (!player) return;
    g_playerPtr = player;
    
    // Use hardcoded offset if set
    if (KNOWN_MIC_OFFSET != 0 && g_micOffset == 0) {
        g_micOffset = KNOWN_MIC_OFFSET;
        LOGI("Using hardcoded offset: 0x%X", g_micOffset);
    }
    
    // Scan for offset if not found
    if (g_micOffset == 0) {
        scanForMIC(player);
        return; // Skip reading until we have offset
    }
    
    // Read MoveInputComponent
    void** micPtr = (void**)((uintptr_t)player + g_micOffset);
    if (!micPtr || !*micPtr) {
        g_micOffset = 0; // Reset if invalid
        return;
    }
    
    MoveInputComponent* mic = (MoveInputComponent*)*micPtr;
    
    std::lock_guard<std::mutex> lock(g_keyMutex);
    
    // Read movement with deadzone
    const float DEADZONE = 0.15f;
    g_keys.w = (mic->mMove.y < -DEADZONE);
    g_keys.s = (mic->mMove.y > DEADZONE);
    g_keys.a = (mic->mMove.x < -DEADZONE);
    g_keys.d = (mic->mMove.x > DEADZONE);
    g_keys.space = (mic->mFlagValues & 0x1) != 0;
    
    // Debug output every 2 seconds
    if (++g_tickCount % 120 == 0) {
        LOGI("Keys: W=%d A=%d S=%d D=%d JMP=%d | mMove=(%.2f, %.2f) @ 0x%X",
             g_keys.w, g_keys.a, g_keys.s, g_keys.d, g_keys.space,
             mic->mMove.x, mic->mMove.y, g_micOffset);
    }
}

// ============================================================================
// UI RENDERING
// ============================================================================

struct GLState {
    GLint prog, tex, atex, abuf, ebuf, vao, fbo, vp[4];
    GLboolean blend, cull, depth, scissor;
};

static void glSave(GLState& s) {
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.prog);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s.atex);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.abuf);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s.ebuf);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.vao);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.fbo);
    glGetIntegerv(GL_VIEWPORT, s.vp);
    s.blend = glIsEnabled(GL_BLEND);
    s.cull = glIsEnabled(GL_CULL_FACE);
    s.depth = glIsEnabled(GL_DEPTH_TEST);
    s.scissor = glIsEnabled(GL_SCISSOR_TEST);
}

static void glRestore(const GLState& s) {
    glUseProgram(s.prog);
    glActiveTexture(s.atex);
    glBindTexture(GL_TEXTURE_2D, s.tex);
    glBindBuffer(GL_ARRAY_BUFFER, s.abuf);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s.ebuf);
    glBindVertexArray(s.vao);
    glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    glViewport(s.vp[0], s.vp[1], s.vp[2], s.vp[3]);
    s.blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
    s.cull ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
    s.depth ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
    s.scissor ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
}

static void drawKey(const char* label, bool pressed, ImVec2 size = ImVec2(60, 60)) {
    ImVec4 bg = pressed ? 
        ImVec4(1.0f, 1.0f, 1.0f, 0.95f) : 
        ImVec4(0.15f, 0.15f, 0.15f, 0.80f);
    ImVec4 text = pressed ? 
        ImVec4(0.0f, 0.0f, 0.0f, 1.0f) : 
        ImVec4(0.9f, 0.9f, 0.9f, 1.0f);
    
    ImGui::PushStyleColor(ImGuiCol_Button, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, bg);
    ImGui::PushStyleColor(ImGuiCol_Text, text);
    ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
}

static void renderHUD() {
    KeyState k;
    { std::lock_guard<std::mutex> lk(g_keyMutex); k = g_keys; }
    
    const float keySize = 60.0f;
    const float spacing = 5.0f;
    float totalWidth = keySize * 3 + spacing * 2;
    
    ImGui::SetNextWindowPos(ImVec2(10, 90), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##HUD", nullptr,
                 ImGuiWindowFlags_NoDecoration |
                 ImGuiWindowFlags_NoBackground |
                 ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);
    
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, spacing));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    
    // Drag handle
    ImGui::InvisibleButton("##drag", ImVec2(totalWidth, 8));
    if (ImGui::IsItemActive()) {
        ImVec2 delta = ImGui::GetIO().MouseDelta;
        ImVec2 pos = ImGui::GetWindowPos();
        ImGui::SetWindowPos(ImVec2(pos.x + delta.x, pos.y + delta.y));
    }
    
    // Settings button
    ImGui::SameLine(totalWidth - 26);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.12f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.22f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.8f, 0.8f, 0.6f));
    if (ImGui::Button("S", ImVec2(24, 24))) g_showSettings = !g_showSettings;
    ImGui::PopStyleColor(4);
    
    // Row 1: W (centered)
    if (g_settings.show_w) {
        float wPos = (totalWidth - keySize) / 2.0f;
        ImGui::SetCursorPosX(wPos);
        drawKey("W", k.w, ImVec2(keySize, keySize));
    }
    
    // Row 2: A S D
    if (g_settings.show_a) {
        drawKey("A", k.a, ImVec2(keySize, keySize));
    }
    if (g_settings.show_s) {
        if (g_settings.show_a) ImGui::SameLine();
        drawKey("S", k.s, ImVec2(keySize, keySize));
    }
    if (g_settings.show_d) {
        if (g_settings.show_a || g_settings.show_s) ImGui::SameLine();
        drawKey("D", k.d, ImVec2(keySize, keySize));
    }
    
    // Row 3: SPACE
    if (g_settings.show_space) {
        drawKey("SPACE", k.space, ImVec2(totalWidth, 40));
    }
    
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
            
            if (g_micOffset) {
                ImGui::TextColored(ImVec4(0, 1, 0, 1), "Status: ACTIVE");
            } else {
                ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "Status: SCANNING...");
            }
            
            ImGui::Text("MIC Offset: 0x%X", g_micOffset);
            ImGui::Text("Player Ptr: %p", g_playerPtr);
            ImGui::Text("Tick Count: %d", g_tickCount);
            
            if (g_micOffset) {
                ImGui::Text("W:%d A:%d S:%d D:%d J:%d",
                           g_keys.w, g_keys.a, g_keys.s, g_keys.d, g_keys.space);
            }
            
            if (ImGui::Button("Rescan Offset")) {
                g_micOffset = 0;
                g_scanComplete = false;
            }
        }
        ImGui::End();
    }
}

// ============================================================================
// EGL HOOK
// ============================================================================

static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;

EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!orig_eglSwapBuffers) return EGL_FALSE;
    
    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    
    if (w < 500 || h < 500) {
        return orig_eglSwapBuffers(dpy, surf);
    }
    
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
        LOGI("ImGui initialized: %dx%d scale=%.2f", w, h, scale);
    }
    
    GLState gs;
    glSave(gs);
    
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(w, h);
    ImGui::NewFrame();
    
    renderHUD();
    
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    
    glRestore(gs);
    
    return orig_eglSwapBuffers(dpy, surf);
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void* main_thread(void*) {
    sleep(15);
    GlossInit(true);
    
    LOGI("========================================");
    LOGI("MobileKeystrokes for LeviLauncher");
    LOGI("MCBE Version: 26.13.1");
    LOGI("Status: Searching for offsets...");
    LOGI("========================================");
    
    // Hook EGL for rendering
    GHandle hegl = GlossOpen("libEGL.so");
    void* swap = (void*)GlossSymbol(hegl, "eglSwapBuffers", nullptr);
    if (swap) {
        GlossHook(swap, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);
        LOGI("[OK] EGL hook installed");
    } else {
        LOGE("[FAIL] EGL hook failed");
    }
    
    // Try to hook NormalTick
    GHandle hmc = GlossOpen("libminecraftpe.so");
    if (hmc) {
        void* tick = nullptr;
        
        // Try symbol names (unlikely in stripped binary)
        const char* symbols[] = {
            "_ZN11LocalPlayer10normalTickEv",
            "_ZN11LocalPlayer10NormalTickEv",
            "_ZN11LocalPlayer4tickEv",
            "_ZN6Player10normalTickEv",
            "_ZN6Player10NormalTickEv",
            nullptr
        };
        
        for (int i = 0; symbols[i]; i++) {
            tick = (void*)GlossSymbol(hmc, symbols[i], nullptr);
            if (tick) {
                LOGI("[OK] Found symbol: %s at %p", symbols[i], tick);
                break;
            }
        }
        
        // If symbols stripped, we need manual offset
        if (!tick) {
            LOGI("[INFO] Symbols stripped - using alternative methods");
            
            // Get library base
            FILE* maps = fopen("/proc/self/maps", "r");
            char line[512];
            uintptr_t base = 0;
            while (fgets(line, sizeof(line), maps)) {
                if (strstr(line, "libminecraftpe.so") && strstr(line, "r-xp")) {
                    sscanf(line, "%lx", &base);
                    break;
                }
            }
            fclose(maps);
            
            if (base) {
                LOGI("[INFO] libminecraftpe.so base: 0x%lx", base);
                LOGI("[INFO] Need to find NormalTick offset manually");
            }
        }
        
        if (tick) {
            if (GlossHook(tick, (void*)hook_NormalTick, (void**)&orig_NormalTick)) {
                LOGI("[OK] NormalTick hook installed at %p", tick);
            } else {
                LOGE("[FAIL] Failed to hook NormalTick");
            }
        } else {
            LOGI("[WARN] NormalTick not found - keystrokes won't work");
            LOGI("[WARN] Use runtime scanner or ask Taco for offset");
        }
    } else {
        LOGE("[FAIL] Could not open libminecraftpe.so");
    }
    
    LOGI("========================================");
    LOGI("Initialization complete");
    LOGI("========================================");
    
    return nullptr;
}

__attribute__((constructor)) void init() {
    pthread_t t;
    pthread_create(&t, nullptr, main_thread, nullptr);
}
