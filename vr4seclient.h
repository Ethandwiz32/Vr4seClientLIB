#pragma once
/*
 * vr4seclient.h
 * ─────────────────────────────────────────────────────────────────────────────
 * VR4SE Client — Injectable mod menu for RoboLab (AArch64 / Android VR)
 * Toggle: Y button on VR controller
 * Renderer: Custom immediate-mode overlay via EGL hook
 * Build: NDK r25c, API 29+, AArch64
 * ─────────────────────────────────────────────────────────────────────────────
 */

#ifndef VR4SECLIENT_H
#define VR4SECLIENT_H

#include <jni.h>
#include <android/log.h>
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <math.h>

#define LOG_TAG "vr4seclient"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

// ─── Version ─────────────────────────────────────────────────────────────────
#define VR4SE_VERSION_MAJOR 1
#define VR4SE_VERSION_MINOR 0
#define VR4SE_VERSION_STR   "v1.0 // vr4seclient"

// ─── Memory Utility ──────────────────────────────────────────────────────────
#define ROBOLAB_BASE        0x0000000000000000UL   // resolved at runtime via /proc/self/maps
#define PATCH_NOP_ARM64     0xD503201F             // NOP opcode AArch64

// ─── GUI State ───────────────────────────────────────────────────────────────
typedef struct {
    bool   visible;           // is the menu open?
    int    active_tab;        // 0=Player 1=Movement 2=Overpowered 3=Guns
    float  anim_alpha;        // 0.0 → 1.0 fade animation
    float  anim_slide;        // pixel offset for slide-in from left
    bool   animating_in;
    bool   animating_out;
    float  scroll_y;          // per-tab scroll position
} GuiState;

// ─── Tab IDs ─────────────────────────────────────────────────────────────────
#define TAB_PLAYER      0
#define TAB_MOVEMENT    1
#define TAB_OVERPOWERED 2
#define TAB_GUNS        3
#define TAB_COUNT       4

static const char* TAB_NAMES[TAB_COUNT] = {
    "PLAYER", "MOVEMENT", "OVERPOWERED", "GUNS"
};

// ─── Mod Toggles & Values ────────────────────────────────────────────────────
typedef struct {
    // ── Player Tab ───────────────────────────────────────────────────────────
    bool   god_mode;
    bool   infinite_ammo;
    bool   no_reload;
    bool   esp_boxes;
    bool   esp_names;
    float  player_health;       // 0 – 9999
    float  player_armor;        // 0 – 9999

    // ── Movement Tab ─────────────────────────────────────────────────────────
    bool   super_speed;
    bool   no_clip;
    bool   fly_mode;
    bool   teleport_to_enemy;
    float  speed_multiplier;    // 1.0 – 20.0
    float  jump_height;         // 1.0 – 50.0

    // ── Overpowered Tab ──────────────────────────────────────────────────────
    bool   one_shot_kill;
    bool   no_gravity;
    bool   infinite_stamina;
    bool   rage_mode;           // all hacks at max
    bool   time_scale_enabled;
    float  time_scale;          // 0.1 – 5.0

    // ── Guns Tab ─────────────────────────────────────────────────────────────
    bool   rapid_fire;
    bool   no_spread;
    bool   no_recoil;
    bool   bullet_penetration;
    bool   explosive_bullets;
    float  damage_multiplier;   // 1.0 – 100.0
    float  fire_rate;           // 1.0 – 50.0
    int    selected_gun;        // gun selector index
} ModConfig;

// ─── Controller State ────────────────────────────────────────────────────────
typedef struct {
    bool y_pressed_prev;
    bool y_pressed_cur;
} ControllerState;

// ─── Global Singletons (defined in vr4seclient.cpp) ─────────────────────────
extern GuiState       g_gui;
extern ModConfig      g_mods;
extern ControllerState g_ctrl;
extern uintptr_t      g_base_addr;
extern pthread_mutex_t g_gui_mutex;

// ─── Forward Declarations ────────────────────────────────────────────────────
void        vr4se_init(void);
void        vr4se_shutdown(void);
void        vr4se_render_frame(void);
void        vr4se_poll_controller(void);
void        vr4se_apply_mods(void);
uintptr_t   vr4se_get_base(void);
bool        vr4se_patch_bytes(uintptr_t addr, uint8_t* patch, size_t len);
bool        vr4se_patch_nop(uintptr_t addr, size_t count);
bool        vr4se_write_float(uintptr_t addr, float val);
bool        vr4se_write_int(uintptr_t addr, int val);

// ─── GUI Forward Declarations ─────────────────────────────────────────────────
void        gui_init(void);
void        gui_shutdown(void);
void        gui_render(void);
void        gui_toggle(void);
void        gui_update_animation(float dt);
void        gui_draw_tab_player(void);
void        gui_draw_tab_movement(void);
void        gui_draw_tab_overpowered(void);
void        gui_draw_tab_guns(void);

// ─── Renderer Forward Declarations ───────────────────────────────────────────
bool        renderer_init(void);
void        renderer_shutdown(void);
void        renderer_draw_rect(float x, float y, float w, float h,
                                uint32_t color, float radius);
void        renderer_draw_text(float x, float y, const char* text,
                                uint32_t color, float scale);
void        renderer_draw_toggle(float x, float y, float w, float h,
                                  bool enabled, const char* label);
void        renderer_draw_slider(float x, float y, float w, float h,
                                  float* value, float min_val, float max_val,
                                  const char* label);
void        renderer_begin_frame(void);
void        renderer_end_frame(void);
int         renderer_get_screen_width(void);
int         renderer_get_screen_height(void);

#endif // VR4SECLIENT_H
