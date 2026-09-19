/*
 * vr4seclient.cpp
 * ─────────────────────────────────────────────────────────────────────────────
 * Core logic: JNI_OnLoad, /proc/self/maps base resolution, mod applier,
 * controller Y-button polling, memory patch engine.
 *
 * [Context: AArch64 | Android API 29 | RoboLab.apk | APKToolkit injection]
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include "vr4seclient.h"
#include <stdio.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>

// ─── Global Singletons ───────────────────────────────────────────────────────
GuiState        g_gui     = {0};
ModConfig       g_mods    = {0};
ControllerState g_ctrl    = {0};
uintptr_t       g_base_addr = 0;
pthread_mutex_t g_gui_mutex = PTHREAD_MUTEX_INITIALIZER;

// ─── Known memory offsets from RoboLab (placeholder — swap after RE) ─────────
// Pattern-matched via arm64 disassembly of libRoboLab.so
// These are relative to g_base_addr
namespace offsets {
    // Player
    constexpr uintptr_t player_health       = 0x00A1B2C0; // float
    constexpr uintptr_t player_armor        = 0x00A1B2C8; // float
    constexpr uintptr_t god_mode_flag       = 0x00A1B300; // bool (1 byte)

    // Movement
    constexpr uintptr_t move_speed          = 0x00B3C410; // float
    constexpr uintptr_t jump_height         = 0x00B3C418; // float
    constexpr uintptr_t noclip_flag         = 0x00B3C480; // bool
    constexpr uintptr_t fly_flag            = 0x00B3C484; // bool
    constexpr uintptr_t gravity_scale       = 0x00B3C490; // float (0 = no gravity)

    // Guns
    constexpr uintptr_t fire_rate           = 0x00C5D500; // float
    constexpr uintptr_t damage_mult         = 0x00C5D508; // float
    constexpr uintptr_t ammo_count          = 0x00C5D510; // int32
    constexpr uintptr_t no_reload_flag      = 0x00C5D520; // bool
    constexpr uintptr_t spread_mult         = 0x00C5D530; // float (0 = no spread)
    constexpr uintptr_t recoil_mult         = 0x00C5D540; // float

    // Time
    constexpr uintptr_t time_scale          = 0x00D7E600; // float
}

// ─── VR Controller Y-button offset (OVR SDK / OpenXR binding) ────────────────
// RoboLab uses OpenXR; Y-button maps to XR_LEFT_HAND_Y_BUTTON
// Controller input struct offset in game's input manager
constexpr uintptr_t CONTROLLER_INPUT_BASE = 0x00E8F700;
constexpr uintptr_t Y_BUTTON_BYTE_OFFSET  = 0x00000014; // 1 = pressed

// ─── Base Address Resolution ─────────────────────────────────────────────────
// Walks /proc/self/maps looking for the RoboLab native lib
// Returns the lowest mapped address of libRoboLab.so (the .text base)
uintptr_t vr4se_get_base(void) {
    FILE* maps = fopen("/proc/self/maps", "r");
    if (!maps) {
        LOGE("vr4se: failed to open /proc/self/maps: %s", strerror(errno));
        return 0;
    }

    char line[512];
    uintptr_t base = 0;

    while (fgets(line, sizeof(line), maps)) {
        // We're looking for a line like:
        // 7a1234000-7a9000000 r-xp 00000000 ... /data/app/com.robolab.vr/libRoboLab.so
        if (strstr(line, "libRoboLab.so") && strstr(line, "r-xp")) {
            // parse start address from "start-end"
            base = (uintptr_t)strtoull(line, NULL, 16);
            LOGI("vr4se: libRoboLab.so base = 0x%lx", base);
            break;
        }
    }

    fclose(maps);

    if (!base) {
        LOGE("vr4se: libRoboLab.so not found in maps — injection timing issue?");
    }

    return base;
}

// ─── Patch Engine ─────────────────────────────────────────────────────────────
// mprotects the page PROT_READ|PROT_WRITE|PROT_EXEC, writes bytes, restores.
bool vr4se_patch_bytes(uintptr_t addr, uint8_t* patch, size_t len) {
    if (!addr || !patch || !len) return false;

    // Align to page boundary
    uintptr_t page     = addr & ~(uintptr_t)(getpagesize() - 1);
    size_t    page_len = len + (addr - page);

    if (mprotect((void*)page, page_len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        LOGE("vr4se: mprotect RWX failed at 0x%lx: %s", addr, strerror(errno));
        return false;
    }

    memcpy((void*)addr, patch, len);

    // Flush instruction cache — mandatory on AArch64 after patching executable pages
    __builtin___clear_cache((char*)addr, (char*)(addr + len));

    // Restore to RX
    mprotect((void*)page, page_len, PROT_READ | PROT_EXEC);
    LOGD("vr4se: patched %zu bytes at 0x%lx", len, addr);
    return true;
}

bool vr4se_patch_nop(uintptr_t addr, size_t count) {
    // AArch64 NOP = 0xD503201F (little-endian: 1F 20 03 D5)
    uint8_t nop4[4] = {0x1F, 0x20, 0x03, 0xD5};
    bool ok = true;
    for (size_t i = 0; i < count; i++) {
        ok &= vr4se_patch_bytes(addr + (i * 4), nop4, 4);
    }
    return ok;
}

bool vr4se_write_float(uintptr_t addr, float val) {
    return vr4se_patch_bytes(addr, (uint8_t*)&val, sizeof(float));
}

bool vr4se_write_int(uintptr_t addr, int val) {
    return vr4se_patch_bytes(addr, (uint8_t*)&val, sizeof(int));
}

// ─── Controller Polling ───────────────────────────────────────────────────────
// Called from the render loop thread each frame.
// Reads the Y-button state from the game's OpenXR input struct in memory.
void vr4se_poll_controller(void) {
    if (!g_base_addr) return;

    uintptr_t input_base = g_base_addr + CONTROLLER_INPUT_BASE;
    uintptr_t y_btn_addr = input_base + Y_BUTTON_BYTE_OFFSET;

    // Read current Y button state (1 byte)
    uint8_t y_state = *(volatile uint8_t*)y_btn_addr;

    g_ctrl.y_pressed_prev = g_ctrl.y_pressed_cur;
    g_ctrl.y_pressed_cur  = (y_state != 0);

    // Rising edge — just pressed
    if (g_ctrl.y_pressed_cur && !g_ctrl.y_pressed_prev) {
        LOGI("vr4se: Y button pressed — toggling GUI");
        gui_toggle();
    }
}

// ─── Mod Applier ─────────────────────────────────────────────────────────────
// Called each frame after controller poll. Applies enabled mod values
// directly into game memory via the patch engine.
void vr4se_apply_mods(void) {
    if (!g_base_addr) return;

    pthread_mutex_lock(&g_gui_mutex);
    ModConfig cfg = g_mods;  // snapshot to minimize lock time
    pthread_mutex_unlock(&g_gui_mutex);

    // ── Player ────────────────────────────────────────────────────────────────
    if (cfg.god_mode) {
        // Write 9999.0f to health each frame so it can't drain
        vr4se_write_float(g_base_addr + offsets::player_health, 9999.0f);
        vr4se_write_float(g_base_addr + offsets::player_armor,  9999.0f);
        uint8_t one = 1;
        vr4se_patch_bytes(g_base_addr + offsets::god_mode_flag, &one, 1);
    } else {
        // Write user-configured values
        vr4se_write_float(g_base_addr + offsets::player_health, cfg.player_health);
        vr4se_write_float(g_base_addr + offsets::player_armor,  cfg.player_armor);
    }

    // ── Movement ──────────────────────────────────────────────────────────────
    if (cfg.super_speed) {
        vr4se_write_float(g_base_addr + offsets::move_speed,
                          cfg.speed_multiplier * 5.0f);
    }

    if (cfg.no_clip) {
        uint8_t one = 1;
        vr4se_patch_bytes(g_base_addr + offsets::noclip_flag, &one, 1);
    } else {
        uint8_t zero = 0;
        vr4se_patch_bytes(g_base_addr + offsets::noclip_flag, &zero, 1);
    }

    if (cfg.fly_mode) {
        uint8_t one = 1;
        vr4se_patch_bytes(g_base_addr + offsets::fly_flag, &one, 1);
    } else {
        uint8_t zero = 0;
        vr4se_patch_bytes(g_base_addr + offsets::fly_flag, &zero, 1);
    }

    if (cfg.no_gravity) {
        vr4se_write_float(g_base_addr + offsets::gravity_scale, 0.0f);
    } else {
        vr4se_write_float(g_base_addr + offsets::gravity_scale, 1.0f);
    }

    vr4se_write_float(g_base_addr + offsets::jump_height, cfg.jump_height);

    // ── Time ──────────────────────────────────────────────────────────────────
    if (cfg.time_scale_enabled) {
        vr4se_write_float(g_base_addr + offsets::time_scale, cfg.time_scale);
    } else {
        vr4se_write_float(g_base_addr + offsets::time_scale, 1.0f);
    }

    // ── Guns ──────────────────────────────────────────────────────────────────
    if (cfg.rapid_fire) {
        vr4se_write_float(g_base_addr + offsets::fire_rate, cfg.fire_rate);
    }

    vr4se_write_float(g_base_addr + offsets::damage_mult, cfg.damage_multiplier);

    if (cfg.infinite_ammo || cfg.no_reload) {
        vr4se_write_int(g_base_addr + offsets::ammo_count, 9999);
        uint8_t one = 1;
        vr4se_patch_bytes(g_base_addr + offsets::no_reload_flag, &one, 1);
    }

    if (cfg.no_spread) {
        vr4se_write_float(g_base_addr + offsets::spread_mult, 0.0f);
    } else {
        vr4se_write_float(g_base_addr + offsets::spread_mult, 1.0f);
    }

    if (cfg.no_recoil) {
        vr4se_write_float(g_base_addr + offsets::recoil_mult, 0.0f);
    } else {
        vr4se_write_float(g_base_addr + offsets::recoil_mult, 1.0f);
    }

    // ── Rage Mode — all maxed ─────────────────────────────────────────────────
    if (cfg.rage_mode) {
        vr4se_write_float(g_base_addr + offsets::player_health,  99999.0f);
        vr4se_write_float(g_base_addr + offsets::damage_mult,    100.0f);
        vr4se_write_float(g_base_addr + offsets::fire_rate,      50.0f);
        vr4se_write_float(g_base_addr + offsets::move_speed,     100.0f);
        vr4se_write_float(g_base_addr + offsets::gravity_scale,  0.0f);
        vr4se_write_int  (g_base_addr + offsets::ammo_count,     99999);
        vr4se_write_float(g_base_addr + offsets::spread_mult,    0.0f);
        vr4se_write_float(g_base_addr + offsets::recoil_mult,    0.0f);
        vr4se_write_float(g_base_addr + offsets::time_scale,     2.0f);
    }
}

// ─── Main Thread ─────────────────────────────────────────────────────────────
// Spawned by JNI_OnLoad. Runs the render/poll/apply loop forever.
static void* vr4se_main_thread(void* arg) {
    (void)arg;

    LOGI("vr4se: main thread started");

    // Wait for the game's native lib to map before resolving base
    for (int retry = 0; retry < 60 && !g_base_addr; retry++) {
        g_base_addr = vr4se_get_base();
        if (!g_base_addr) usleep(500000); // 500ms between retries
    }

    if (!g_base_addr) {
        LOGE("vr4se: could not resolve base after 30s — aborting thread");
        return NULL;
    }

    // Initialize defaults
    g_mods.player_health    = 100.0f;
    g_mods.player_armor     = 100.0f;
    g_mods.speed_multiplier = 3.0f;
    g_mods.jump_height      = 5.0f;
    g_mods.damage_multiplier= 1.5f;
    g_mods.fire_rate        = 10.0f;
    g_mods.time_scale       = 1.0f;

    // Init GUI renderer
    gui_init();

    struct timespec ts_prev, ts_cur;
    clock_gettime(CLOCK_MONOTONIC, &ts_prev);

    // Main loop — ~60fps cadence
    while (1) {
        clock_gettime(CLOCK_MONOTONIC, &ts_cur);
        float dt = (float)(ts_cur.tv_sec  - ts_prev.tv_sec) +
                   (float)(ts_cur.tv_nsec - ts_prev.tv_nsec) * 1e-9f;
        ts_prev = ts_cur;

        // Poll VR controller Y button
        vr4se_poll_controller();

        // Apply all enabled mods to game memory
        vr4se_apply_mods();

        // Update GUI animation state
        gui_update_animation(dt);

        // Render overlay if visible (or still animating out)
        if (g_gui.visible || g_gui.animating_out || g_gui.anim_alpha > 0.01f) {
            gui_render();
        }

        // ~60fps
        usleep(16666);
    }

    gui_shutdown();
    return NULL;
}

// ─── JNI Entry Point ─────────────────────────────────────────────────────────
// Called by the Android runtime when libvr4seclient.so is loaded.
// APKToolkit injection patches the APK's lib loading path to include us.
JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved) {
    (void)vm;
    (void)reserved;

    LOGI("──────────────────────────────────────────────────");
    LOGI("  vr4seclient %s", VR4SE_VERSION_STR);
    LOGI("  RoboLab VR Mod Menu | AArch64");
    LOGI("  Y button → toggle GUI");
    LOGI("──────────────────────────────────────────────────");

    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    if (pthread_create(&thread, &attr, vr4se_main_thread, NULL) != 0) {
        LOGE("vr4se: failed to spawn main thread: %s", strerror(errno));
        return JNI_VERSION_1_6;
    }

    pthread_attr_destroy(&attr);
    LOGI("vr4se: main thread spawned");
    return JNI_VERSION_1_6;
}
