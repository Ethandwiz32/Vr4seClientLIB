#pragma once
/*
 * vr4seclient.h — v3 FULL REWRITE
 * ─────────────────────────────────────────────────────────────────────────────
 * [Context: AArch64 | Android API 29 | Unity VR | com.bloodrex.robolab]
 * [Lib target: libil2cpp.so  (NOT libRoboLab.so — that doesn't exist)]
 * [Networking: Photon Fusion (NOT PUN) — ConnectAndJoin class]
 * [Game classes: FlyCam, Grabber, GrabHelper, PrefabSpawner, WelderGraphics]
 * [Input: Unity InputSystem / OpenXR — polled via XR runtime, NOT raw memory]
 *
 * v1/v2 fatal bugs fixed:
 *   1. Wrong lib name: searched "libRoboLab.so" — doesn't exist in APK.
 *      Correct lib is "libil2cpp.so". g_base resolves against that.
 *
 *   2. All offsets were placeholder zeroes. We do NOT use static offsets on
 *      IL2CPP — Unity encodes IL into libil2cpp at compile time and the field
 *      layout shifts every build. Instead we use BNM-style runtime IL2CPP
 *      class/method/field lookup by name string → pointer resolved at init.
 *      This is exactly how MonkiiGUI (the working reference mod) operates.
 *
 *   3. Controller polling read from a fabricated OpenXR struct offset.
 *      On Meta Quest the Y-button state lives in Unity's InputSystem, not
 *      in a raw pointer offset. We hook Update() on a MonoBehaviour that
 *      Unity calls each frame, then call OVRInput.Get() via IL2CPP invoke.
 *
 *   4. GUI: v2 still referenced GLES shaders in vr4seclient.h but used
 *      the Java Canvas path in vr4seclient.cpp. Header is now consistent:
 *      we ship one overlay strategy — Android Canvas TYPE_APPLICATION_OVERLAY
 *      drawn from a Java thread. No EGL, no glCreateShader, no GL at all.
 *
 * Architecture (v3):
 *   • BNM IL2CPP bootstrap (custom-rolled, no BNM library dependency):
 *       - Wait for il2cpp_init (hooked via Dobby on libil2cpp.so export)
 *       - Walk il2cpp domain → assemblies → images → type definitions
 *       - Cache MethodInfo* pointers by class name + method name
 *   • Dobby inline hook on FlyCam.Update() for per-frame mod application
 *   • Dobby hook on ConnectAndJoin.Start() for Photon Fusion room hooks
 *   • Y-button toggle: hook on any MonoBehaviour.Update that has access to
 *     OVRInput, or poll via Unity InputSystem action "PrimaryButton"
 *   • Mods applied by invoking cached MethodInfo* with il2cpp_runtime_invoke
 *   • GUI: TYPE_APPLICATION_OVERLAY Java Canvas, JNI-bridged from hook thread
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
#include <stdio.h>
#include <sys/mman.h>
#include <errno.h>
#include <math.h>
#include <dirent.h>

/* ── Logging ─────────────────────────────────────────────────────────────── */
#define LOG_TAG  "vr4se"
#define LOGI(...)  __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...)  __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...)  __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

/* ── Version ─────────────────────────────────────────────────────────────── */
#define VR4SE_VERSION "v3.0 // vr4seclient // for Jason"

/* ── IL2CPP minimal type stubs ───────────────────────────────────────────── */
/* We never link against il2cpp directly — we dlsym every function pointer.  */
struct Il2CppObject { void* klass; void* monitor; };
struct Il2CppString { Il2CppObject obj; int32_t length; uint16_t chars[1]; };
struct Il2CppException { Il2CppObject base; };
struct MethodInfo;   /* opaque */
struct FieldInfo;    /* opaque */
struct Il2CppClass;  /* opaque */

/* ── IL2CPP exported function pointer types ──────────────────────────────── */
typedef void*       (*t_il2cpp_domain_get)(void);
typedef void**      (*t_il2cpp_domain_get_assemblies)(void* domain, size_t* size);
typedef void*       (*t_il2cpp_assembly_get_image)(void* assembly);
typedef uint32_t    (*t_il2cpp_image_get_class_count)(void* image);
typedef Il2CppClass*(*t_il2cpp_image_get_class)(void* image, uint32_t index);
typedef const char* (*t_il2cpp_class_get_name)(Il2CppClass* klass);
typedef const char* (*t_il2cpp_class_get_namespace)(Il2CppClass* klass);
typedef MethodInfo* (*t_il2cpp_class_get_method_from_name)(Il2CppClass*, const char*, int);
typedef FieldInfo*  (*t_il2cpp_class_get_field_from_name)(Il2CppClass*, const char*);
typedef void*       (*t_il2cpp_runtime_invoke)(MethodInfo*, void* obj, void** args, Il2CppException**);
typedef Il2CppString* (*t_il2cpp_string_new)(const char*);
typedef void*       (*t_il2cpp_object_new)(Il2CppClass*);
typedef Il2CppClass*(*t_il2cpp_object_get_class)(Il2CppObject*);
typedef void        (*t_il2cpp_field_get_value)(Il2CppObject*, FieldInfo*, void* value);
typedef void        (*t_il2cpp_field_set_value)(Il2CppObject*, FieldInfo*, void* value);
typedef void        (*t_il2cpp_field_static_get_value)(FieldInfo*, void* value);
typedef void        (*t_il2cpp_field_static_set_value)(FieldInfo*, void* value);

/* ── Cached IL2CPP fn pointers (resolved in vr4se_il2cpp_init) ───────────── */
struct Il2cppFn {
    t_il2cpp_domain_get                  domain_get;
    t_il2cpp_domain_get_assemblies       domain_get_assemblies;
    t_il2cpp_assembly_get_image          assembly_get_image;
    t_il2cpp_image_get_class_count       image_get_class_count;
    t_il2cpp_image_get_class             image_get_class;
    t_il2cpp_class_get_name              class_get_name;
    t_il2cpp_class_get_namespace         class_get_namespace;
    t_il2cpp_class_get_method_from_name  class_get_method_from_name;
    t_il2cpp_class_get_field_from_name   class_get_field_from_name;
    t_il2cpp_runtime_invoke              runtime_invoke;
    t_il2cpp_string_new                  string_new;
    t_il2cpp_object_new                  object_new;
    t_il2cpp_object_get_class            object_get_class;
    t_il2cpp_field_get_value             field_get_value;
    t_il2cpp_field_set_value             field_set_value;
    t_il2cpp_field_static_get_value      field_static_get_value;
    t_il2cpp_field_static_set_value      field_static_set_value;
};
extern struct Il2cppFn g_il2;
extern bool            g_il2_ready;

/* ── Unity Vector3 / Quaternion (raw struct layout) ─────────────────────── */
typedef struct { float x, y, z; }        Vec3;
typedef struct { float x, y, z, w; }     Quat;

/* ── Mod config ──────────────────────────────────────────────────────────── */
typedef struct {
    /* ── Player ───────────────────────────────────────────────── */
    bool   god_mode;
    bool   invisible;

    /* ── Movement ─────────────────────────────────────────────── */
    bool   fly;              /* FlyCam toggle via field patch          */
    bool   speed_boost;      /* small (3×)                             */
    bool   big_speed;        /* large (10×)                            */
    bool   inf_jump;         /* zero-out jump gravity component        */
    float  speed_mult;       /* raw multiplier slider 1.0–20.0         */

    /* ── Guns (Photon Fusion RPC spam) ───────────────────────── */
    bool   kick_gun;         /* KickGun bullet spam                    */
    bool   ban_gun;          /* BanCage2 (ban cage) spam               */
    bool   crash_gun;        /* crash all (CrashAll)                   */
    bool   lag_gun;          /* projectile spam to cause lag           */
    bool   steal_prefab_gun; /* steal object and refling               */
    bool   fling_prefab_gun; /* FlingPrefab spam                       */

    /* ── Prefabs ──────────────────────────────────────────────── */
    bool   prefab_spawner;   /* PrefabSpawner gun mode                 */
    int    prefab_index;     /* which prefab (0..MAX_PREFABS-1)        */
    bool   load_prefabs;     /* trigger prefab list refresh            */

    /* ── Config ───────────────────────────────────────────────── */
    int    hand;             /* 0=left, 1=right                        */
} ModConfig;

/* ── GUI state ───────────────────────────────────────────────────────────── */
/* 5 tabs: 0=Player 1=Movement 2=Guns 3=Prefabs 4=Config                     */
#define TAB_COUNT 5
static const char* const TAB_NAMES[TAB_COUNT] =
    {"PLAYER","MOVEMENT","GUNS","PREFABS","CONFIG"};

typedef struct {
    bool  visible;
    int   active_tab;
    float anim_alpha;   /* 0→1 for fade, driven from Java thread      */
} GuiState;

/* ── Globals (defined in vr4seclient.cpp) ────────────────────────────────── */
extern ModConfig        g_mods;
extern GuiState         g_gui;
extern JavaVM*          g_jvm;
extern pthread_mutex_t  g_mtx;
extern uintptr_t        g_il2cpp_base;  /* libil2cpp.so load addr             */

/* ── Prefab list (filled from PrefabSpawner at runtime) ─────────────────── */
#define MAX_PREFABS 64
extern char  g_prefab_names[MAX_PREFABS][64];
extern int   g_prefab_count;

/* ── Forward decls ───────────────────────────────────────────────────────── */
/* il2cpp bootstrap */
bool        vr4se_il2cpp_init(void);
Il2CppClass* vr4se_find_class(const char* ns, const char* name);
MethodInfo* vr4se_find_method(const char* ns, const char* klass,
                               const char* method, int argc);
FieldInfo*  vr4se_find_field(const char* ns, const char* klass,
                              const char* field);

/* base resolver */
uintptr_t   vr4se_get_lib_base(const char* libname);

/* raw patch (for non-IL2CPP fields when we know the offset) */
bool        vr4se_patch_bytes(uintptr_t addr, uint8_t* data, size_t len);
bool        vr4se_write_float(uintptr_t addr, float v);
bool        vr4se_write_int32(uintptr_t addr, int32_t v);
bool        vr4se_write_byte(uintptr_t addr, uint8_t v);

/* per-frame work (called from hooked Update) */
void        vr4se_apply_mods(Il2CppObject* local_player);

/* Photon RPC spammers (called when gun flags are set) */
void        vr4se_kick_gun_tick(void);
void        vr4se_ban_gun_tick(void);
void        vr4se_crash_gun_tick(void);
void        vr4se_lag_gun_tick(void);
void        vr4se_steal_prefab_tick(void);
void        vr4se_fling_prefab_tick(void);
void        vr4se_prefab_spawner_tick(void);

/* prefab enumeration */
void        vr4se_load_prefabs(void);

/* GUI JNI bridge */
void        vr4se_gui_show(JNIEnv* env);
void        vr4se_gui_hide(JNIEnv* env);
void        vr4se_gui_toggle(JNIEnv* env);

/* entry */
void        vr4se_main_init(JavaVM* jvm);

#endif /* VR4SECLIENT_H */
