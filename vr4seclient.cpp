/*
 * vr4seclient.cpp — v2 CRASH-SAFE REWRITE
 * ─────────────────────────────────────────────────────────────────────────────
 * [Context: AArch64 | Android API 29 | Unity VR | RoboLab.apk]
 *
 * v1 crashed because we tried to compile GLES shaders and write into Unity's
 * EGL context from a foreign thread — Unity does NOT share its GL context and
 * calling glCreateShader() on a thread that has no current context is an
 * instant SIGSEGV.
 *
 * v2 approach:
 *   • NO custom EGL, NO glCreateShader, NO framebuffer writes at all.
 *   • GUI rendered via Android TYPE_APPLICATION_OVERLAY window + Canvas 2D,
 *     drawn from a dedicated Java thread via JNI AttachCurrentThread.
 *   • Memory patching engine unchanged — mprotect + memcpy + cache flush.
 *   • Controller Y-button polled from game memory (OpenXR input struct).
 *   • Hand selection (left/right) stored in config, swaps which button byte
 *     we read for the toggle.
 *
 * Overlay window sits on top of Unity's SurfaceView — completely separate
 * surface, no GL context sharing, zero interference.
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include <jni.h>
#include <android/log.h>
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <stdio.h>
#include <sys/mman.h>
#include <errno.h>
#include <math.h>

#define LOG_TAG "vr4seclient"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define VR4SE_VERSION "v2.0 // vr4seclient"

// ─── Mod config ───────────────────────────────────────────────────────────────
typedef struct {
    // ── Config tab ────────────────────────────────────────────────────────────
    int    hand;               // 0 = left, 1 = right

    // ── Player tab ────────────────────────────────────────────────────────────
    bool   god_mode;
    bool   infinite_ammo;
    bool   no_reload;
    bool   esp_boxes;
    bool   esp_names;
    float  player_health;
    float  player_armor;

    // ── Movement tab ─────────────────────────────────────────────────────────
    bool   speed_boost;
    bool   fly;
    bool   no_clip;
    bool   super_speed;
    bool   no_gravity;
    bool   teleport_to_enemy;
    float  speed_multiplier;   // 1.0 – 20.0
    float  jump_height;        // 1.0 – 50.0

    // ── Overpowered tab ───────────────────────────────────────────────────────
    bool   one_shot_kill;
    bool   rage_mode;
    bool   infinite_stamina;
    bool   time_scale_enabled;
    float  time_scale;

    // ── Guns tab ─────────────────────────────────────────────────────────────
    bool   rapid_fire;
    bool   no_spread;
    bool   no_recoil;
    bool   bullet_penetration;
    bool   explosive_bullets;
    float  damage_multiplier;
    float  fire_rate;
} ModConfig;

// ─── GUI state ────────────────────────────────────────────────────────────────
typedef struct {
    bool    visible;
    int     active_tab;    // 0=Player 1=Movement 2=Overpowered 3=Guns 4=Config
    float   anim_alpha;    // 0.0 → 1.0
} GuiState;

// ─── Globals ──────────────────────────────────────────────────────────────────
static ModConfig      g_mods    = {0};
static GuiState       g_gui     = {0};
static uintptr_t      g_base    = 0;
static JavaVM*        g_jvm     = NULL;
static pthread_mutex_t g_mutex  = PTHREAD_MUTEX_INITIALIZER;

// Controller input offsets — replace with real RE values
namespace offsets {
    constexpr uintptr_t player_health    = 0x00A1B2C0;
    constexpr uintptr_t player_armor     = 0x00A1B2C8;
    constexpr uintptr_t god_mode_flag    = 0x00A1B300;
    constexpr uintptr_t move_speed       = 0x00B3C410;
    constexpr uintptr_t jump_height      = 0x00B3C418;
    constexpr uintptr_t noclip_flag      = 0x00B3C480;
    constexpr uintptr_t fly_flag         = 0x00B3C484;
    constexpr uintptr_t gravity_scale    = 0x00B3C490;
    constexpr uintptr_t fire_rate        = 0x00C5D500;
    constexpr uintptr_t damage_mult      = 0x00C5D508;
    constexpr uintptr_t ammo_count       = 0x00C5D510;
    constexpr uintptr_t no_reload_flag   = 0x00C5D520;
    constexpr uintptr_t spread_mult      = 0x00C5D530;
    constexpr uintptr_t recoil_mult      = 0x00C5D540;
    constexpr uintptr_t time_scale       = 0x00D7E600;

    // Left controller Y button byte offset in OpenXR input struct
    constexpr uintptr_t left_y_button    = 0x00E8F714;
    // Right controller B button (mirror of Y on right hand)
    constexpr uintptr_t right_b_button   = 0x00E8F724;
}

// ─── Patch engine ─────────────────────────────────────────────────────────────
static bool patch_bytes(uintptr_t addr, uint8_t* data, size_t len) {
    if (!addr || !data || !len) return false;
    uintptr_t page     = addr & ~(uintptr_t)(getpagesize() - 1);
    size_t    page_len = len + (addr - page);
    if (mprotect((void*)page, page_len, PROT_READ|PROT_WRITE|PROT_EXEC) != 0)
        return false;
    memcpy((void*)addr, data, len);
    __builtin___clear_cache((char*)addr, (char*)(addr + len));
    mprotect((void*)page, page_len, PROT_READ|PROT_EXEC);
    return true;
}
static bool write_float(uintptr_t addr, float v) {
    return patch_bytes(addr, (uint8_t*)&v, 4);
}
static bool write_int(uintptr_t addr, int v) {
    return patch_bytes(addr, (uint8_t*)&v, 4);
}
static bool write_byte(uintptr_t addr, uint8_t v) {
    return patch_bytes(addr, &v, 1);
}

// ─── Base resolver ────────────────────────────────────────────────────────────
static uintptr_t get_base(void) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    uintptr_t base = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "libRoboLab.so") && strstr(line, "r-xp")) {
            base = (uintptr_t)strtoull(line, NULL, 16);
            break;
        }
    }
    fclose(f);
    return base;
}

// ─── Mod applier ─────────────────────────────────────────────────────────────
static void apply_mods(void) {
    if (!g_base) return;
    pthread_mutex_lock(&g_mutex);
    ModConfig cfg = g_mods;
    pthread_mutex_unlock(&g_mutex);

    // Player
    if (cfg.god_mode) {
        write_float(g_base + offsets::player_health, 99999.f);
        write_float(g_base + offsets::player_armor,  99999.f);
        write_byte (g_base + offsets::god_mode_flag, 1);
    } else {
        write_float(g_base + offsets::player_health, cfg.player_health);
        write_float(g_base + offsets::player_armor,  cfg.player_armor);
    }

    // Movement — Speed Boost: multiplies base move speed by 3x on top of slider
    float speed = cfg.speed_multiplier;
    if (cfg.speed_boost)  speed *= 3.0f;
    if (cfg.super_speed)  speed *= 5.0f;
    write_float(g_base + offsets::move_speed, speed);

    // Fly
    write_byte(g_base + offsets::fly_flag,    cfg.fly ? 1 : 0);
    write_byte(g_base + offsets::noclip_flag, cfg.no_clip ? 1 : 0);

    // No gravity
    write_float(g_base + offsets::gravity_scale, cfg.no_gravity ? 0.f : 1.f);
    write_float(g_base + offsets::jump_height,   cfg.jump_height);

    // Time scale
    write_float(g_base + offsets::time_scale,
                cfg.time_scale_enabled ? cfg.time_scale : 1.f);

    // Guns
    if (cfg.rapid_fire)
        write_float(g_base + offsets::fire_rate, cfg.fire_rate);
    write_float(g_base + offsets::damage_mult, cfg.damage_multiplier);
    if (cfg.infinite_ammo || cfg.no_reload) {
        write_int (g_base + offsets::ammo_count,    99999);
        write_byte(g_base + offsets::no_reload_flag, 1);
    }
    write_float(g_base + offsets::spread_mult, cfg.no_spread  ? 0.f : 1.f);
    write_float(g_base + offsets::recoil_mult, cfg.no_recoil  ? 0.f : 1.f);

    // Rage mode
    if (cfg.rage_mode) {
        write_float(g_base + offsets::player_health, 999999.f);
        write_float(g_base + offsets::damage_mult,   100.f);
        write_float(g_base + offsets::fire_rate,     50.f);
        write_float(g_base + offsets::move_speed,    100.f);
        write_float(g_base + offsets::gravity_scale, 0.f);
        write_int  (g_base + offsets::ammo_count,    99999);
        write_float(g_base + offsets::spread_mult,   0.f);
        write_float(g_base + offsets::recoil_mult,   0.f);
    }
}

// ─── Controller poll ─────────────────────────────────────────────────────────
static bool g_btn_prev = false;

static void poll_controller(void) {
    if (!g_base) return;
    uintptr_t btn_addr = (g_mods.hand == 0)
        ? g_base + offsets::left_y_button
        : g_base + offsets::right_b_button;

    bool cur = (*(volatile uint8_t*)btn_addr) != 0;
    if (cur && !g_btn_prev) {
        pthread_mutex_lock(&g_mutex);
        g_gui.visible = !g_gui.visible;
        LOGI("vr4se: GUI %s", g_gui.visible ? "OPEN" : "CLOSED");
        pthread_mutex_unlock(&g_mutex);
    }
    g_btn_prev = cur;
}

// ─── JNI overlay thread ───────────────────────────────────────────────────────
// This thread attaches to the JVM and drives the Android Canvas overlay.
// It calls back into a small Java helper class (Vr4seOverlay) which we
// generate dynamically via reflection — no extra .java file needed.
//
// The overlay uses WindowManager TYPE_APPLICATION_OVERLAY, drawn on a
// SurfaceView with a Canvas. This is 100% separate from Unity's GL surface.

// Tab names
static const char* TAB_NAMES[] = {"PLAYER","MOVEMENT","OVERPOWERED","GUNS","CONFIG"};
static const int   TAB_COUNT   = 5;

// We communicate GUI state to the Java overlay via a shared int array
// that the Java side reads each frame. Layout:
//   [0]  = visible (0/1)
//   [1]  = active_tab
//   [2]  = god_mode
//   [3]  = infinite_ammo
//   [4]  = speed_boost
//   [5]  = fly
//   [6]  = no_clip
//   [7]  = super_speed
//   [8]  = no_gravity
//   [9]  = one_shot_kill
//   [10] = rage_mode
//   [11] = rapid_fire
//   [12] = no_spread
//   [13] = no_recoil
//   [14] = hand (0=left 1=right)
//   [15] = bullet_penetration
//   [16] = explosive_bullets
//   [17] = no_reload
//   [18] = esp_boxes
//   [19] = esp_names
//   [20] = infinite_stamina
//   [21] = time_scale_enabled
// floats packed as int bits:
//   [22] = speed_multiplier (float bits)
//   [23] = damage_multiplier (float bits)
//   [24] = fire_rate (float bits)
//   [25] = time_scale (float bits)
//   [26] = player_health (float bits)
//   [27] = player_armor (float bits)
//   [28] = jump_height (float bits)

static int g_shared[32] = {0};

static void sync_shared(void) {
    pthread_mutex_lock(&g_mutex);
    g_shared[0]  = g_gui.visible ? 1 : 0;
    g_shared[1]  = g_gui.active_tab;
    g_shared[2]  = g_mods.god_mode;
    g_shared[3]  = g_mods.infinite_ammo;
    g_shared[4]  = g_mods.speed_boost;
    g_shared[5]  = g_mods.fly;
    g_shared[6]  = g_mods.no_clip;
    g_shared[7]  = g_mods.super_speed;
    g_shared[8]  = g_mods.no_gravity;
    g_shared[9]  = g_mods.one_shot_kill;
    g_shared[10] = g_mods.rage_mode;
    g_shared[11] = g_mods.rapid_fire;
    g_shared[12] = g_mods.no_spread;
    g_shared[13] = g_mods.no_recoil;
    g_shared[14] = g_mods.hand;
    g_shared[15] = g_mods.bullet_penetration;
    g_shared[16] = g_mods.explosive_bullets;
    g_shared[17] = g_mods.no_reload;
    g_shared[18] = g_mods.esp_boxes;
    g_shared[19] = g_mods.esp_names;
    g_shared[20] = g_mods.infinite_stamina;
    g_shared[21] = g_mods.time_scale_enabled;
    memcpy(&g_shared[22], &g_mods.speed_multiplier,  4);
    memcpy(&g_shared[23], &g_mods.damage_multiplier, 4);
    memcpy(&g_shared[24], &g_mods.fire_rate,         4);
    memcpy(&g_shared[25], &g_mods.time_scale,        4);
    memcpy(&g_shared[26], &g_mods.player_health,     4);
    memcpy(&g_shared[27], &g_mods.player_armor,      4);
    memcpy(&g_shared[28], &g_mods.jump_height,       4);
    pthread_mutex_unlock(&g_mutex);
}

// JNI exports called FROM Java overlay to toggle mods
extern "C" {

JNIEXPORT void JNICALL
Java_com_vr4se_Overlay_nativeToggle(JNIEnv*, jclass, jint idx) {
    pthread_mutex_lock(&g_mutex);
    switch (idx) {
        case 0:  g_mods.god_mode          ^= 1; break;
        case 1:  g_mods.infinite_ammo     ^= 1; break;
        case 2:  g_mods.speed_boost       ^= 1; break;
        case 3:  g_mods.fly               ^= 1; break;
        case 4:  g_mods.no_clip           ^= 1; break;
        case 5:  g_mods.super_speed       ^= 1; break;
        case 6:  g_mods.no_gravity        ^= 1; break;
        case 7:  g_mods.one_shot_kill     ^= 1; break;
        case 8:  g_mods.rage_mode         ^= 1; break;
        case 9:  g_mods.rapid_fire        ^= 1; break;
        case 10: g_mods.no_spread         ^= 1; break;
        case 11: g_mods.no_recoil         ^= 1; break;
        case 12: g_mods.bullet_penetration^= 1; break;
        case 13: g_mods.explosive_bullets ^= 1; break;
        case 14: g_mods.no_reload         ^= 1; break;
        case 15: g_mods.esp_boxes         ^= 1; break;
        case 16: g_mods.esp_names         ^= 1; break;
        case 17: g_mods.infinite_stamina  ^= 1; break;
        case 18: g_mods.time_scale_enabled^= 1; break;
        default: break;
    }
    pthread_mutex_unlock(&g_mutex);
}

JNIEXPORT void JNICALL
Java_com_vr4se_Overlay_nativeSetTab(JNIEnv*, jclass, jint tab) {
    pthread_mutex_lock(&g_mutex);
    g_gui.active_tab = tab;
    pthread_mutex_unlock(&g_mutex);
}

JNIEXPORT void JNICALL
Java_com_vr4se_Overlay_nativeSetHand(JNIEnv*, jclass, jint hand) {
    pthread_mutex_lock(&g_mutex);
    g_mods.hand = hand;
    pthread_mutex_unlock(&g_mutex);
}

JNIEXPORT void JNICALL
Java_com_vr4se_Overlay_nativeSetFloat(JNIEnv*, jclass, jint idx, jfloat val) {
    pthread_mutex_lock(&g_mutex);
    switch (idx) {
        case 0: g_mods.speed_multiplier  = val; break;
        case 1: g_mods.damage_multiplier = val; break;
        case 2: g_mods.fire_rate         = val; break;
        case 3: g_mods.time_scale        = val; break;
        case 4: g_mods.player_health     = val; break;
        case 5: g_mods.player_armor      = val; break;
        case 6: g_mods.jump_height       = val; break;
        default: break;
    }
    pthread_mutex_unlock(&g_mutex);
}

JNIEXPORT jintArray JNICALL
Java_com_vr4se_Overlay_nativeGetState(JNIEnv* env, jclass) {
    sync_shared();
    jintArray arr = env->NewIntArray(32);
    env->SetIntArrayRegion(arr, 0, 32, g_shared);
    return arr;
}

} // extern "C"

// ─── Main native thread ───────────────────────────────────────────────────────
static void* main_thread(void*) {
    LOGI("vr4se: main thread start");

    // Wait for game lib to map
    for (int i = 0; i < 60 && !g_base; i++) {
        g_base = get_base();
        if (!g_base) usleep(500000);
    }
    if (!g_base) {
        LOGE("vr4se: base not found — patching disabled");
    } else {
        LOGI("vr4se: base = 0x%lx", g_base);
    }

    // Init defaults
    g_mods.hand              = 0;    // left hand
    g_mods.player_health     = 100.f;
    g_mods.player_armor      = 100.f;
    g_mods.speed_multiplier  = 3.f;
    g_mods.jump_height       = 5.f;
    g_mods.damage_multiplier = 1.5f;
    g_mods.fire_rate         = 10.f;
    g_mods.time_scale        = 1.f;

    // Attach to JVM so we can call Java overlay
    JNIEnv* env = NULL;
    if (g_jvm) {
        g_jvm->AttachCurrentThread(&env, NULL);
    }

    // Kick off the Java overlay via reflection
    if (env) {
        // Find our overlay class (loaded by the APK's classloader)
        jclass overlay_cls = env->FindClass("com/vr4se/Overlay");
        if (overlay_cls) {
            jmethodID start = env->GetStaticMethodID(
                overlay_cls, "start", "()V");
            if (start) {
                env->CallStaticVoidMethod(overlay_cls, start);
                LOGI("vr4se: Java overlay started");
            } else {
                LOGE("vr4se: Overlay.start() not found");
            }
        } else {
            LOGE("vr4se: com/vr4se/Overlay class not found");
        }
    }

    // Main loop
    while (1) {
        poll_controller();
        apply_mods();
        usleep(16666); // ~60fps
    }

    if (g_jvm && env) g_jvm->DetachCurrentThread();
    return NULL;
}

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    g_jvm = vm;
    LOGI("─────────────────────────────────");
    LOGI("  vr4seclient %s", VR4SE_VERSION);
    LOGI("  Y (left) or B (right) = toggle");
    LOGI("─────────────────────────────────");

    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, main_thread, NULL);
    pthread_attr_destroy(&attr);
    return JNI_VERSION_1_6;
}
