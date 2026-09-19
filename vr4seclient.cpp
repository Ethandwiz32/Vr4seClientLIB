/*
 * vr4seclient.cpp — v3 FULL REWRITE
 * ─────────────────────────────────────────────────────────────────────────────
 * [Context: AArch64 | Android API 29 | Unity VR | com.bloodrex.robolab]
 * [Build: NDK r25c, clang++17, libil2cpp.so as target, Dobby for hooking]
 *
 * Root cause of the Y-button not opening:
 *   vr4seclient.cpp v2 reads:
 *       bool cur = (*(volatile uint8_t*)(g_base + offsets::left_y_button)) != 0;
 *   …where g_base was resolved against "libRoboLab.so" which does NOT exist.
 *   /proc/self/maps on this APK shows: libil2cpp.so, libunity.so, libmain.so.
 *   g_base therefore always stays 0. The pointer deref is 0x00E8F714 → SIGSEGV
 *   caught by the OS and the thread silently dies before the GUI ever opens.
 *
 * v3 approach:
 *   1. Resolve g_il2cpp_base from "libil2cpp.so" (correct lib name).
 *   2. dlsym every il2cpp_* function we need from libil2cpp.so handle.
 *   3. Hook il2cpp_init (exported from libil2cpp.so) with Dobby to know
 *      exactly when the runtime is ready before we walk domain metadata.
 *   4. After il2cpp_init fires, walk assembly images to cache MethodInfo*
 *      for FlyCam.Update, ConnectAndJoin.Start, GrabHelper methods etc.
 *   5. Hook FlyCam.Update with Dobby → our per-frame callback. Inside it
 *      we read OVRInput state via il2cpp_runtime_invoke on OVRInput.Get().
 *   6. Y-button toggle → flip g_gui.visible → call Java overlay start/stop.
 *   7. Mod application via il2cpp_runtime_invoke on cached MethodInfo ptrs
 *      or direct field writes using il2cpp_field_set_value.
 *
 * Prefab list strategy:
 *   PrefabSpawner holds a NetworkPrefabRef[] or List<> on the Runner.
 *   We enumerate them at OnPlayerJoined time (hook that method) and stash
 *   the names in g_prefab_names[]. The Prefabs GUI tab renders this list.
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include "vr4seclient.h"

/* ── Globals ─────────────────────────────────────────────────────────────── */
ModConfig         g_mods           = {};
GuiState          g_gui            = {};
JavaVM*           g_jvm            = nullptr;
pthread_mutex_t   g_mtx            = PTHREAD_MUTEX_INITIALIZER;
uintptr_t         g_il2cpp_base    = 0;
struct Il2cppFn   g_il2            = {};
bool              g_il2_ready      = false;

char  g_prefab_names[MAX_PREFABS][64] = {};
int   g_prefab_count                  = 0;

/* ── Cached game object/method pointers ──────────────────────────────────── */
static Il2CppClass*  cls_FlyCam           = nullptr; /* FlyCam in Assembly-CSharp   */
static Il2CppClass*  cls_ConnectAndJoin   = nullptr;
static Il2CppClass*  cls_PrefabSpawner    = nullptr;
static Il2CppClass*  cls_OVRInput         = nullptr;
static Il2CppClass*  cls_Rigidbody        = nullptr;

static MethodInfo*   meth_FlyCam_Update         = nullptr;
static MethodInfo*   meth_CAJ_OnPlayerJoined     = nullptr;
static MethodInfo*   meth_CAJ_ConnectNow         = nullptr;
static MethodInfo*   meth_OVRInput_Get_bool       = nullptr;  /* Get(Button, Controller) */
static MethodInfo*   meth_Rigidbody_AddForce      = nullptr;
static MethodInfo*   meth_Rigidbody_set_velocity  = nullptr;
static MethodInfo*   meth_Rigidbody_set_useGravity= nullptr;

static FieldInfo*    field_FlyCam_enabled        = nullptr;  /* Component.enabled      */
static FieldInfo*    field_Rigidbody_velocity     = nullptr;

/* singleton FlyCam instance, set inside the hooked Update() */
static Il2CppObject* g_fly_cam_instance           = nullptr;

/* ── Dobby function declarations (linked at link time) ───────────────────── */
extern "C" {
    int DobbyHook(void* addr, void* hook, void** orig);
    int DobbyInstrument(void* addr, void* hook);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  BASE RESOLVER
 * ═══════════════════════════════════════════════════════════════════════════*/
uintptr_t vr4se_get_lib_base(const char* libname) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char   line[512];
    uintptr_t base = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, libname) && strstr(line, "r-xp")) {
            base = (uintptr_t)strtoull(line, nullptr, 16);
            break;
        }
    }
    fclose(f);
    return base;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  RAW PATCH ENGINE  (used only where il2cpp field API isn't available)
 * ═══════════════════════════════════════════════════════════════════════════*/
bool vr4se_patch_bytes(uintptr_t addr, uint8_t* data, size_t len) {
    if (!addr || !data || !len) return false;
    uintptr_t page     = addr & ~(uintptr_t)(getpagesize() - 1);
    size_t    span     = len + (addr - page);
    if (mprotect((void*)page, span, PROT_READ|PROT_WRITE|PROT_EXEC) != 0) return false;
    memcpy((void*)addr, data, len);
    __builtin___clear_cache((char*)addr, (char*)(addr + len));
    mprotect((void*)page, span, PROT_READ|PROT_EXEC);
    return true;
}
bool vr4se_write_float(uintptr_t addr, float v)    { return vr4se_patch_bytes(addr, (uint8_t*)&v, 4); }
bool vr4se_write_int32(uintptr_t addr, int32_t v)  { return vr4se_patch_bytes(addr, (uint8_t*)&v, 4); }
bool vr4se_write_byte (uintptr_t addr, uint8_t v)  { return vr4se_patch_bytes(addr, &v, 1); }

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  IL2CPP BOOTSTRAP — dlsym all needed exports from libil2cpp.so
 * ═══════════════════════════════════════════════════════════════════════════*/
bool vr4se_il2cpp_init(void) {
    /* Open libil2cpp.so — it is already mapped by the time JNI_OnLoad fires */
    void* h = dlopen("libil2cpp.so", RTLD_LAZY | RTLD_NOLOAD);
    if (!h) {
        /* RTLD_NOLOAD failed means it was opened without GLOBAL flag.
           Try the full path via /proc/self/maps. */
        char path[256] = {};
        FILE* f = fopen("/proc/self/maps", "r");
        if (f) {
            char line[512];
            while (fgets(line, sizeof(line), f)) {
                if (strstr(line, "libil2cpp.so")) {
                    /* Extract path (last token) */
                    char* p = strchr(line, '/');
                    if (p) {
                        size_t len = strlen(p);
                        if (len > 0 && p[len-1] == '\n') p[len-1] = 0;
                        strncpy(path, p, sizeof(path)-1);
                        break;
                    }
                }
            }
            fclose(f);
        }
        h = dlopen(path[0] ? path : "libil2cpp.so", RTLD_LAZY | RTLD_GLOBAL);
    }
    if (!h) {
        LOGE("vr4se: dlopen libil2cpp.so failed: %s", dlerror());
        return false;
    }

#define DLSYM(fn)  do { \
    g_il2.fn = (decltype(g_il2.fn))dlsym(h, "il2cpp_" #fn); \
    if (!g_il2.fn) { LOGE("vr4se: missing il2cpp_" #fn); return false; } \
} while(0)

    DLSYM(domain_get);
    DLSYM(domain_get_assemblies);
    DLSYM(assembly_get_image);
    DLSYM(image_get_class_count);
    DLSYM(image_get_class);
    DLSYM(class_get_name);
    DLSYM(class_get_namespace);
    DLSYM(class_get_method_from_name);
    DLSYM(class_get_field_from_name);
    DLSYM(runtime_invoke);
    DLSYM(string_new);
    DLSYM(object_new);
    DLSYM(object_get_class);
    DLSYM(field_get_value);
    DLSYM(field_set_value);
    DLSYM(field_static_get_value);
    DLSYM(field_static_set_value);
#undef DLSYM

    g_il2_ready = true;
    LOGI("vr4se: il2cpp functions resolved OK");
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  CLASS / METHOD / FIELD LOOKUP — walks domain at runtime
 * ═══════════════════════════════════════════════════════════════════════════*/
Il2CppClass* vr4se_find_class(const char* ns, const char* name) {
    if (!g_il2_ready) return nullptr;

    void*  domain    = g_il2.domain_get();
    size_t asm_count = 0;
    void** assemblies = g_il2.domain_get_assemblies(domain, &asm_count);

    for (size_t ai = 0; ai < asm_count; ai++) {
        void*    image      = g_il2.assembly_get_image(assemblies[ai]);
        uint32_t cls_count  = g_il2.image_get_class_count(image);
        for (uint32_t ci = 0; ci < cls_count; ci++) {
            Il2CppClass* klass = g_il2.image_get_class(image, ci);
            const char*  kname = g_il2.class_get_name(klass);
            const char*  kns   = g_il2.class_get_namespace(klass);
            if (!kname) continue;
            if (strcmp(kname, name) != 0) continue;
            if (ns && ns[0] && kns && strcmp(kns, ns) != 0) continue;
            return klass;
        }
    }
    LOGE("vr4se: class not found: %s.%s", ns ? ns : "", name);
    return nullptr;
}

MethodInfo* vr4se_find_method(const char* ns, const char* klass,
                               const char* method, int argc) {
    Il2CppClass* c = vr4se_find_class(ns, klass);
    if (!c) return nullptr;
    MethodInfo* m = g_il2.class_get_method_from_name(c, method, argc);
    if (!m) LOGE("vr4se: method not found: %s.%s(%d)", klass, method, argc);
    return m;
}

FieldInfo* vr4se_find_field(const char* ns, const char* klass,
                             const char* field) {
    Il2CppClass* c = vr4se_find_class(ns, klass);
    if (!c) return nullptr;
    FieldInfo* f = g_il2.class_get_field_from_name(c, field);
    if (!f) LOGE("vr4se: field not found: %s.%s", klass, field);
    return f;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  CACHE GAME POINTERS — called once after il2cpp domain is ready
 * ═══════════════════════════════════════════════════════════════════════════*/
static void cache_game_pointers(void) {
    /* FlyCam — handles in-game spectate/fly camera on Quest. Its Update()
       is the ideal injection point because it runs every Unity frame and
       has direct access to XR controller state.                            */
    cls_FlyCam  = vr4se_find_class("", "FlyCam");
    if (cls_FlyCam) {
        meth_FlyCam_Update = g_il2.class_get_method_from_name(cls_FlyCam, "Update", 0);
        LOGI("vr4se: FlyCam.Update @ %p", (void*)meth_FlyCam_Update);
    }

    /* ConnectAndJoin — Photon Fusion session manager (verified in libil2cpp) */
    cls_ConnectAndJoin = vr4se_find_class("", "ConnectAndJoin");
    if (cls_ConnectAndJoin) {
        meth_CAJ_OnPlayerJoined = g_il2.class_get_method_from_name(
            cls_ConnectAndJoin, "OnPlayerJoined", 2);
        meth_CAJ_ConnectNow = g_il2.class_get_method_from_name(
            cls_ConnectAndJoin, "ConnectNow", 0);
    }

    /* PrefabSpawner — Fusion INetworkRunnerCallbacks implementor.
       SpawnPlayer(NetworkRunner, PlayerRef) is the spawn entry point.      */
    cls_PrefabSpawner = vr4se_find_class("", "PrefabSpawner");

    /* UnityEngine.Rigidbody — used for velocity/gravity writes on player RB */
    cls_Rigidbody = vr4se_find_class("UnityEngine", "Rigidbody");
    if (cls_Rigidbody) {
        meth_Rigidbody_set_velocity   = g_il2.class_get_method_from_name(
            cls_Rigidbody, "set_velocity", 1);
        meth_Rigidbody_set_useGravity = g_il2.class_get_method_from_name(
            cls_Rigidbody, "set_useGravity", 1);
        meth_Rigidbody_AddForce       = g_il2.class_get_method_from_name(
            cls_Rigidbody, "AddForce", 2);
        field_Rigidbody_velocity = g_il2.class_get_field_from_name(
            cls_Rigidbody, "m_Velocity");
    }

    /* OVRInput — Oculus SDK input class for Quest controller buttons.
       OVRInput.Get(Button, Controller) → bool                              */
    cls_OVRInput = vr4se_find_class("", "OVRInput");
    if (cls_OVRInput) {
        /* arg count 2: Button bitmask + Controller bitmask */
        meth_OVRInput_Get_bool = g_il2.class_get_method_from_name(
            cls_OVRInput, "Get", 2);
        LOGI("vr4se: OVRInput.Get @ %p", (void*)meth_OVRInput_Get_bool);
    }

    LOGI("vr4se: game pointers cached");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  Y-BUTTON POLL via OVRInput.Get  (called every Update frame)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * OVRInput.Button enum values (from IL2CPP metadata int values):
 *   One   = 0x00000001  ← X button left controller
 *   Two   = 0x00000002  ← Y button left controller  ← WE WANT THIS
 *   Three = 0x00000004  ← A button right controller
 *   Four  = 0x00000008  ← B button right controller ← or this for right hand
 *
 * OVRInput.Controller enum:
 *   LTouch = 0x00000001  (left  Touch controller)
 *   RTouch = 0x00000002  (right Touch controller)
 *   All    = 0x000000FF
 */
static bool s_prev_y = false;

static bool poll_y_button(void) {
    if (!meth_OVRInput_Get_bool) return false;

    Il2CppException* exc = nullptr;

    /* Button: Two (Y) = 0x00000002, Controller: LTouch = 0x00000001
       Both are il2cpp enums passed as boxed int32 args.                    */
    int32_t btn  = (g_mods.hand == 0) ? 0x00000002 : 0x00000008; /* Y or B   */
    int32_t ctrl = (g_mods.hand == 0) ? 0x00000001 : 0x00000002; /* L or R   */

    void* args[2] = { &btn, &ctrl };
    /* OVRInput.Get is static → obj = nullptr */
    Il2CppObject* result = (Il2CppObject*)g_il2.runtime_invoke(
        meth_OVRInput_Get_bool, nullptr, args, &exc);

    if (exc || !result) return false;
    /* Boolean is returned as Il2CppObject wrapping a bool field at offset 8 */
    return *((bool*)((uint8_t*)result + sizeof(Il2CppObject)));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §7  MOD APPLICATION — runs every frame from hooked FlyCam.Update
 * ═══════════════════════════════════════════════════════════════════════════*/

/* Photon Fusion gun spam helpers — these call into the Photon SDK via
   il2cpp_runtime_invoke, not raw RPC packets. The Grabber/GrabHelper classes
   in libil2cpp match exactly the MonkiiGUI naming. We replicate the call
   patterns from MonkiiGUI's Photon namespace methods.                        */

/* Helper: invoke a static void method with no args */
static void invoke_static_void(MethodInfo* m) {
    if (!m) return;
    Il2CppException* exc = nullptr;
    g_il2.runtime_invoke(m, nullptr, nullptr, &exc);
    if (exc) LOGE("vr4se: invoke exception");
}

/* Helper: find and invoke by class + method name (one-shot, no cache) */
static void spam_photon(const char* klass, const char* method) {
    MethodInfo* m = vr4se_find_method("", klass, method, 0);
    invoke_static_void(m);
}

static uint32_t s_frame = 0;

void vr4se_apply_mods(Il2CppObject* /* unused for now */) {
    if (!g_il2_ready) return;
    pthread_mutex_lock(&g_mtx);
    ModConfig cfg = g_mods;
    pthread_mutex_unlock(&g_mtx);

    s_frame++;

    /* ── Y-button toggle ───────────────────────────────────────────────── */
    bool cur_y = poll_y_button();
    if (cur_y && !s_prev_y) {
        /* Toggle GUI — must happen on a thread with JNI env */
        JNIEnv* env = nullptr;
        if (g_jvm && g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK) {
            pthread_mutex_lock(&g_mtx);
            g_gui.visible = !g_gui.visible;
            bool show = g_gui.visible;
            pthread_mutex_unlock(&g_mtx);
            LOGI("vr4se: GUI %s (Y button)", show ? "OPEN" : "CLOSED");
            if (show) vr4se_gui_show(env);
            else      vr4se_gui_hide(env);
        }
    }
    s_prev_y = cur_y;

    /* ── Fly mode ──────────────────────────────────────────────────────── */
    /* FlyCam is Unity's built-in free-camera component. Enabling it on the
       local player's camera object lets them fly. We write the `enabled`
       bool field directly on the FlyCam Il2CppObject instance.             */
    if (g_fly_cam_instance && field_FlyCam_enabled) {
        bool val = cfg.fly;
        g_il2.field_set_value(g_fly_cam_instance, field_FlyCam_enabled, &val);
    }

    /* ── Speed boost — write Rigidbody velocity via field every 3 frames ─ */
    if ((s_frame % 3 == 0) && (cfg.speed_boost || cfg.big_speed)) {
        /* We don't have the player Rigidbody ptr yet without a player hook.
           Speed is instead applied via FlyCam's own speed field.
           FlyCam exposes: float sensitivity, float moveSpeed
           We write moveSpeed directly. MonkiiGUI does the same with arm scale.
           Field name discovered via metadata walk at init.                   */
        FieldInfo* fspeed = vr4se_find_field("", "FlyCam", "moveSpeed");
        if (fspeed && g_fly_cam_instance) {
            float spd = cfg.big_speed ? 10.0f : (cfg.speed_boost ? 3.0f : 1.0f);
            spd *= cfg.speed_mult;
            g_il2.field_set_value(g_fly_cam_instance, fspeed, &spd);
        }
    }

    /* ── Inf jump — zero gravity scale on Rigidbody every few frames ───── */
    /* Handled via player Rigidbody; we need a player instance from GrabHelper.
       Left as a hook stub — implementation fills in when GrabHelper hook fires.*/

    /* ── Gun spam (rate-limited to 1 call per 6 frames per gun) ─────────── */
    if (s_frame % 6 == 0) {
        if (cfg.kick_gun)         vr4se_kick_gun_tick();
        if (cfg.ban_gun)          vr4se_ban_gun_tick();
        if (cfg.crash_gun)        vr4se_crash_gun_tick();
        if (cfg.lag_gun)          vr4se_lag_gun_tick();
        if (cfg.steal_prefab_gun) vr4se_steal_prefab_tick();
        if (cfg.fling_prefab_gun) vr4se_fling_prefab_tick();
        if (cfg.prefab_spawner)   vr4se_prefab_spawner_tick();
    }

    /* ── Load prefab list (one-shot on flag) ──────────────────────────── */
    if (cfg.load_prefabs) {
        vr4se_load_prefabs();
        pthread_mutex_lock(&g_mtx);
        g_mods.load_prefabs = false;  /* clear the trigger              */
        pthread_mutex_unlock(&g_mtx);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §8  GUN SPAM IMPLEMENTATIONS
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * MonkiiGUI proves these class names exist (from strings analysis):
 *   Photon::KickGunBullet2Spam  → kicks player out with gun bullet
 *   Photon::BanCage2Spam        → bans target (spawns ban cage)
 *   Photon::CrashAll            → crashes all clients in room
 *   Photon::GrenadeSpam         → grenade projectile spam (lag gun)
 *   Photon::MagnetGunSpam       → steal/attract prefabs
 *   Photon::RagdollSpam         → fling ragdolls (fling prefab)
 *
 * All are static void methods on the Photon class with no args.
 * Their IL2CPP symbol names end in _m<40hexchars> (seen in libil2cpp.so).
 */
void vr4se_kick_gun_tick()         { spam_photon("Photon", "KickGunBullet2Spam"); }
void vr4se_ban_gun_tick()          { spam_photon("Photon", "BanCage2Spam"); }
void vr4se_crash_gun_tick()        { spam_photon("Photon", "CrashAll"); }
void vr4se_lag_gun_tick()          { spam_photon("Photon", "GrenadeSpam"); }
void vr4se_steal_prefab_tick()     { spam_photon("Photon", "MagnetGunSpam"); }
void vr4se_fling_prefab_tick()     { spam_photon("Photon", "RagdollSpam"); }

/* ── PrefabSpawner gun: calls SpawnPlayer with selected prefab index ─────── */
void vr4se_prefab_spawner_tick() {
    if (!cls_PrefabSpawner) return;
    /* SpawnPlayer(NetworkRunner runner, PlayerRef playerRef) — 2 args.
       For a simple local-spawn approach we call with null runner to trigger
       the default prefab at our position.                                    */
    MethodInfo* m = g_il2.class_get_method_from_name(
        cls_PrefabSpawner, "SpawnPlayer", 2);
    if (!m) return;
    void* args[2] = { nullptr, nullptr };
    Il2CppException* exc = nullptr;
    g_il2.runtime_invoke(m, nullptr, args, &exc);
}

/* ── Prefab enumeration ──────────────────────────────────────────────────── */
void vr4se_load_prefabs() {
    /* In Photon Fusion the NetworkPrefabTable lives on NetworkProjectConfig.
       We grab it as a static field:
         NetworkProjectConfig.GetGlobal() → NetworkProjectConfig instance
         instance.PrefabTable → NetworkPrefabTable
         table.PrefabCount → int
         table.GetPrefab(i) → NetworkPrefabData
       All via il2cpp_runtime_invoke. */
    LOGI("vr4se: loading prefab list…");
    g_prefab_count = 0;

    Il2CppClass* cls_cfg = vr4se_find_class("Fusion", "NetworkProjectConfig");
    if (!cls_cfg) {
        /* Try without namespace */
        cls_cfg = vr4se_find_class("", "NetworkProjectConfig");
    }
    if (!cls_cfg) {
        LOGE("vr4se: NetworkProjectConfig not found");
        /* Populate with known prefabs from MonkiiGUI string analysis */
        const char* known[] = {
            "Grenade","Molotov","Ragdoll","ZomBear","ZomBunny","Boy",
            "Hellephant","KickGunBullet2","BanCage2","MagnetGun","SMGShot2",
            "ShotgunBullet2","PlasmaBulletBattle2","GlockBullet2",
            "DoubleBarrelBullet2","PistolBulletBattle2","FlintKnockV2",
            "CrossBowArrow","Pumpkin","DragonFireBulet","EggNadeV2Z2",
            "CatNadeV2Z","VIPBullet2","SniperBulletBattle2","GrenadeLaunched",
            "WaterBullet","BulletV2BattleZ","TommyShot2","Flintlock","Harpon",
            "JmanHeadV3Z","Supressed2","Burber","ParticlesLight","Flintlock"
        };
        g_prefab_count = (int)(sizeof(known)/sizeof(known[0]));
        if (g_prefab_count > MAX_PREFABS) g_prefab_count = MAX_PREFABS;
        for (int i = 0; i < g_prefab_count; i++) {
            strncpy(g_prefab_names[i], known[i], 63);
            g_prefab_names[i][63] = 0;
        }
        LOGI("vr4se: loaded %d fallback prefab names", g_prefab_count);
        return;
    }

    /* If we found the class, invoke GetGlobal() */
    MethodInfo* m_get = g_il2.class_get_method_from_name(cls_cfg, "GetGlobal", 0);
    if (!m_get) { LOGE("vr4se: GetGlobal not found"); return; }

    Il2CppException* exc = nullptr;
    Il2CppObject* cfg_inst = (Il2CppObject*)g_il2.runtime_invoke(m_get, nullptr, nullptr, &exc);
    if (!cfg_inst || exc) { LOGE("vr4se: GetGlobal failed"); return; }

    /* Try to enumerate from PrefabTable — specific traversal depends on Fusion
       version. We stub with a log for now; real offset determined at runtime. */
    LOGI("vr4se: NetworkProjectConfig instance @ %p", (void*)cfg_inst);
    /* …field traversal left for runtime RE with offset dump                    */
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §9  DOBBY HOOK — FlyCam.Update
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * FlyCam.Update is a MonoBehaviour Update() with signature:
 *   void Update()  →  IL2CPP compiled to: void FlyCam_Update_m<hash>(FlyCam* this, MethodInfo*)
 * AArch64 calling convention: x0 = this (FlyCam Il2CppObject*), x1 = MethodInfo*
 *
 * We hook this to:
 *   a) Capture the FlyCam instance (x0) → g_fly_cam_instance
 *   b) Run vr4se_apply_mods() every frame
 *   c) Call original to not break normal camera behavior
 */
typedef void (*FlyCam_Update_t)(Il2CppObject* self, MethodInfo* method);
static FlyCam_Update_t orig_FlyCam_Update = nullptr;

static void hook_FlyCam_Update(Il2CppObject* self, MethodInfo* method) {
    /* Capture the singleton FlyCam instance on first call */
    if (!g_fly_cam_instance && self) {
        g_fly_cam_instance = self;
        LOGI("vr4se: FlyCam instance captured @ %p", (void*)self);

        /* Now we can cache the 'enabled' field on this instance's class */
        if (!field_FlyCam_enabled && g_il2_ready) {
            Il2CppClass* klass = g_il2.object_get_class(self);
            /* Walk parent chain: FlyCam → Behaviour → Component
               'enabled' lives on Behaviour */
            if (klass) {
                field_FlyCam_enabled = g_il2.class_get_field_from_name(klass, "m_Enabled");
                if (!field_FlyCam_enabled)
                    field_FlyCam_enabled = g_il2.class_get_field_from_name(klass, "enabled");
            }
        }
    }

    /* Run mod applier */
    vr4se_apply_mods(self);

    /* Call original FlyCam.Update */
    if (orig_FlyCam_Update) orig_FlyCam_Update(self, method);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §10  IL2CPP INIT HOOK — know exactly when runtime is ready
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * il2cpp_init(const char* domain_name) is the first export called in
 * libil2cpp.so right after the Unity engine loads it. We hook it to
 * defer our class-cache walk until the runtime is fully initialized.
 */
typedef int (*il2cpp_init_t)(const char*);
static il2cpp_init_t orig_il2cpp_init = nullptr;

static int hook_il2cpp_init(const char* domain_name) {
    int ret = orig_il2cpp_init ? orig_il2cpp_init(domain_name) : 0;
    LOGI("vr4se: il2cpp_init(\"%s\") fired — caching game pointers", domain_name);
    cache_game_pointers();

    /* Now hook FlyCam.Update if we found it */
    if (meth_FlyCam_Update) {
        /* MethodInfo* contains function pointer at offset 0 on AArch64/ARM64
           il2cpp compiled code. The actual native function is at methodPointer. */
        void** methodPointer = (void**)meth_FlyCam_Update; /* methodPointer is field[0] */
        void*  native_fn     = *methodPointer;
        if (native_fn) {
            int err = DobbyHook(native_fn,
                                (void*)hook_FlyCam_Update,
                                (void**)&orig_FlyCam_Update);
            if (err == 0) LOGI("vr4se: FlyCam.Update hooked @ %p", native_fn);
            else          LOGE("vr4se: FlyCam.Update hook failed: %d", err);
        }
    }

    return ret;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §11  GUI JNI BRIDGE — show/hide Android Canvas overlay
 * ═══════════════════════════════════════════════════════════════════════════*/
static jclass    g_overlay_cls   = nullptr;
static jmethodID g_overlay_show  = nullptr;
static jmethodID g_overlay_hide  = nullptr;

static void ensure_overlay_cls(JNIEnv* env) {
    if (g_overlay_cls) return;
    jclass cls = env->FindClass("com/vr4se/Overlay");
    if (!cls) { LOGE("vr4se: com/vr4se/Overlay not found — is the APK patched?"); return; }
    g_overlay_cls  = (jclass)env->NewGlobalRef(cls);
    g_overlay_show = env->GetStaticMethodID(g_overlay_cls, "show", "()V");
    g_overlay_hide = env->GetStaticMethodID(g_overlay_cls, "hide", "()V");
    env->DeleteLocalRef(cls);
}

void vr4se_gui_show(JNIEnv* env) {
    ensure_overlay_cls(env);
    if (g_overlay_cls && g_overlay_show)
        env->CallStaticVoidMethod(g_overlay_cls, g_overlay_show);
}

void vr4se_gui_hide(JNIEnv* env) {
    ensure_overlay_cls(env);
    if (g_overlay_cls && g_overlay_hide)
        env->CallStaticVoidMethod(g_overlay_cls, g_overlay_hide);
}

void vr4se_gui_toggle(JNIEnv* env) {
    pthread_mutex_lock(&g_mtx);
    g_gui.visible = !g_gui.visible;
    bool show = g_gui.visible;
    pthread_mutex_unlock(&g_mtx);
    if (show) vr4se_gui_show(env);
    else      vr4se_gui_hide(env);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §12  JNI EXPORTS — called FROM Java Overlay to toggle mods
 * ═══════════════════════════════════════════════════════════════════════════*/
extern "C" {

JNIEXPORT void JNICALL
Java_com_vr4se_Overlay_nativeToggle(JNIEnv*, jclass, jint idx) {
    pthread_mutex_lock(&g_mtx);
    switch (idx) {
        /* Player */
        case  0: g_mods.god_mode          ^= 1; break;
        case  1: g_mods.invisible         ^= 1; break;
        /* Movement */
        case  2: g_mods.fly               ^= 1; break;
        case  3: g_mods.speed_boost       ^= 1; if (g_mods.speed_boost) g_mods.big_speed = false; break;
        case  4: g_mods.big_speed         ^= 1; if (g_mods.big_speed)   g_mods.speed_boost = false; break;
        case  5: g_mods.inf_jump          ^= 1; break;
        /* Guns */
        case  6: g_mods.kick_gun          ^= 1; break;
        case  7: g_mods.ban_gun           ^= 1; break;
        case  8: g_mods.crash_gun         ^= 1; break;
        case  9: g_mods.lag_gun           ^= 1; break;
        case 10: g_mods.steal_prefab_gun  ^= 1; break;
        case 11: g_mods.fling_prefab_gun  ^= 1; break;
        /* Prefabs */
        case 12: g_mods.prefab_spawner    ^= 1; break;
        case 13: g_mods.load_prefabs       = 1; break;
        default: break;
    }
    pthread_mutex_unlock(&g_mtx);
}

JNIEXPORT void JNICALL
Java_com_vr4se_Overlay_nativeSetTab(JNIEnv*, jclass, jint tab) {
    pthread_mutex_lock(&g_mtx);
    g_gui.active_tab = tab;
    pthread_mutex_unlock(&g_mtx);
}

JNIEXPORT void JNICALL
Java_com_vr4se_Overlay_nativeSetHand(JNIEnv*, jclass, jint hand) {
    pthread_mutex_lock(&g_mtx);
    g_mods.hand = hand;
    pthread_mutex_unlock(&g_mtx);
}

JNIEXPORT void JNICALL
Java_com_vr4se_Overlay_nativeSetFloat(JNIEnv*, jclass, jint idx, jfloat val) {
    pthread_mutex_lock(&g_mtx);
    switch (idx) {
        case 0: g_mods.speed_mult   = val; break;
        case 1: g_mods.prefab_index = (int)val; break;
        default: break;
    }
    pthread_mutex_unlock(&g_mtx);
}

JNIEXPORT jintArray JNICALL
Java_com_vr4se_Overlay_nativeGetState(JNIEnv* env, jclass) {
    pthread_mutex_lock(&g_mtx);
    int state[32] = {};
    state[0]  = g_gui.visible     ? 1 : 0;
    state[1]  = g_gui.active_tab;
    state[2]  = g_mods.god_mode   ? 1 : 0;
    state[3]  = g_mods.invisible  ? 1 : 0;
    state[4]  = g_mods.fly        ? 1 : 0;
    state[5]  = g_mods.speed_boost? 1 : 0;
    state[6]  = g_mods.big_speed  ? 1 : 0;
    state[7]  = g_mods.inf_jump   ? 1 : 0;
    state[8]  = g_mods.kick_gun   ? 1 : 0;
    state[9]  = g_mods.ban_gun    ? 1 : 0;
    state[10] = g_mods.crash_gun  ? 1 : 0;
    state[11] = g_mods.lag_gun    ? 1 : 0;
    state[12] = g_mods.steal_prefab_gun ? 1 : 0;
    state[13] = g_mods.fling_prefab_gun ? 1 : 0;
    state[14] = g_mods.prefab_spawner   ? 1 : 0;
    state[15] = g_mods.hand;
    state[16] = g_prefab_count;
    state[17] = g_mods.prefab_index;
    memcpy(&state[18], &g_mods.speed_mult, 4);
    state[30] = g_il2_ready ? 1 : 0;
    pthread_mutex_unlock(&g_mtx);

    jintArray arr = env->NewIntArray(32);
    env->SetIntArrayRegion(arr, 0, 32, state);
    return arr;
}

/* Prefab names exposed to Java as String array */
JNIEXPORT jobjectArray JNICALL
Java_com_vr4se_Overlay_nativeGetPrefabNames(JNIEnv* env, jclass) {
    pthread_mutex_lock(&g_mtx);
    int count = g_prefab_count;
    pthread_mutex_unlock(&g_mtx);

    jclass  str_cls = env->FindClass("java/lang/String");
    jobjectArray arr = env->NewObjectArray(count, str_cls, nullptr);
    for (int i = 0; i < count; i++) {
        jstring s = env->NewStringUTF(g_prefab_names[i]);
        env->SetObjectArrayElement(arr, i, s);
        env->DeleteLocalRef(s);
    }
    env->DeleteLocalRef(str_cls);
    return arr;
}

} /* extern "C" */

/* ═══════════════════════════════════════════════════════════════════════════
 * §13  MAIN INIT THREAD — waits for libil2cpp then hooks il2cpp_init
 * ═══════════════════════════════════════════════════════════════════════════*/
static void* main_thread(void*) {
    LOGI("vr4se: ─────────────────────────────────────");
    LOGI("vr4se:   %s", VR4SE_VERSION);
    LOGI("vr4se:   Target: com.bloodrex.robolab");
    LOGI("vr4se:   Y (left) or B (right) = toggle GUI");
    LOGI("vr4se: ─────────────────────────────────────");

    /* 1. Wait for libil2cpp.so to be mapped */
    for (int i = 0; i < 120 && !g_il2cpp_base; i++) {
        g_il2cpp_base = vr4se_get_lib_base("libil2cpp.so");
        if (!g_il2cpp_base) usleep(500000);
    }
    if (!g_il2cpp_base) {
        LOGE("vr4se: libil2cpp.so base not found — aborting");
        return nullptr;
    }
    LOGI("vr4se: libil2cpp.so base = 0x%lx", g_il2cpp_base);

    /* 2. dlsym all il2cpp function pointers */
    if (!vr4se_il2cpp_init()) {
        LOGE("vr4se: il2cpp fn init failed — aborting");
        return nullptr;
    }

    /* 3. Hook il2cpp_init export. If the runtime is already up (JNI_OnLoad
       fires after il2cpp_init on some builds), call cache directly instead. */
    void* h = dlopen("libil2cpp.so", RTLD_LAZY | RTLD_NOLOAD);
    if (!h) h = dlopen("libil2cpp.so", RTLD_LAZY | RTLD_GLOBAL);
    if (h) {
        void* il2cpp_init_fn = dlsym(h, "il2cpp_init");
        if (il2cpp_init_fn) {
            int err = DobbyHook(il2cpp_init_fn,
                                (void*)hook_il2cpp_init,
                                (void**)&orig_il2cpp_init);
            if (err == 0) LOGI("vr4se: il2cpp_init hooked");
            else {
                /* Hook failed — runtime already ran; call cache now */
                LOGD("vr4se: il2cpp_init hook skipped (already ran), caching now");
                cache_game_pointers();
            }
        }
    }

    /* 4. Init GUI defaults */
    pthread_mutex_lock(&g_mtx);
    g_mods.hand       = 0;   /* left hand */
    g_mods.speed_mult = 1.0f;
    g_gui.active_tab  = 0;
    pthread_mutex_unlock(&g_mtx);

    /* 5. Kick off Java overlay via JNI */
    JNIEnv* env = nullptr;
    if (g_jvm) {
        g_jvm->AttachCurrentThread(&env, nullptr);
        if (env) {
            ensure_overlay_cls(env);
            LOGI("vr4se: Java overlay class %s", g_overlay_cls ? "ready" : "MISSING");
        }
    }

    /* 6. Load fallback prefab list immediately */
    vr4se_load_prefabs();

    LOGI("vr4se: init complete — waiting for FlyCam.Update hook to fire");

    if (g_jvm && env) g_jvm->DetachCurrentThread();
    return nullptr;
}

void vr4se_main_init(JavaVM* jvm) {
    g_jvm = jvm;
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, main_thread, nullptr);
    pthread_attr_destroy(&attr);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §14  JNI_OnLoad — Android entry point
 * ═══════════════════════════════════════════════════════════════════════════*/
JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    vr4se_main_init(vm);
    return JNI_VERSION_1_6;
}
