#include <jni.h>
#include <android/log.h>
#include <pthread.h>
#include <unistd.h>
#include <mutex>

#include "pl/Gloss.h"

#define LOG_TAG "MobileKeystrokes"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

// NO IMGUI, NO GRAPHICS
typedef void (*NormalTick)(void* self);
static NormalTick orig_NormalTick = nullptr;

void hook_NormalTick(void* player) {
    if (player) {
        // We only print to the logcat, we don't touch ANY memory offsets
        // This is the safest test possible.
        static int frameCount = 0;
        if (frameCount++ % 100 == 0) {
            LOGI("The mod is ALIVE. Player address is: %p", player);
        }
    }
    if (orig_NormalTick) orig_NormalTick(player);
}

void* main_thread(void*) {
    // Wait for the game to fully load
    sleep(15);
    
    GlossInit(true);
    LOGI("Gloss Initialized...");

    GHandle hmc = GlossOpen("libminecraftpe.so");
    if (hmc) {
        // We try to find the player heartbeat
        void* tick = (void*)GlossSymbol(hmc, "_ZN11LocalPlayer10normalTickEv", nullptr);
        if (tick) {
            LOGI("Found NormalTick at %p. Hooking now...", tick);
            GlossHook(tick, (void*)hook_NormalTick, (void**)&orig_NormalTick);
            LOGI("Hook success!");
        } else {
            LOGI("Could not find the symbol!");
        }
    }
    return nullptr;
}

__attribute__((constructor)) void init() {
    pthread_t t;
    pthread_create(&t, nullptr, main_thread, nullptr);
}
