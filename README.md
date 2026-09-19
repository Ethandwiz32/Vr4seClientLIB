# vr4seclient — RoboLab VR Mod Menu
**AArch64 | Android API 29+ | GLES 3.0 | OpenXR Y-button toggle**

---

## What this is

`libvr4seclient.so` is an injectable mod menu for RoboLab VR.
It attaches to the game process at load time (via APKToolkit lib injection),
resolves the game's native lib base address from `/proc/self/maps`,
and applies memory patches each frame while rendering a custom GLES 3 overlay.

```
Y button (left VR controller)
       ↓
  gui_toggle()
       ↓
  slide + fade animation (0.25s)
       ↓
  [PLAYER | MOVEMENT | OVERPOWERED | GUNS]
```

---

## Project layout

```
vr4seclient/
├── include/
│   └── vr4seclient.h        ← all types, offsets, forward decls
├── src/
│   ├── vr4seclient.cpp      ← JNI_OnLoad, base resolver, mod applier, patch engine
│   └── gui.cpp              ← immediate-mode GLES3 renderer + tab UI
├── jni/
│   ├── Android.mk
│   └── Application.mk
└── README.md
```

---

## Build

Requires Android NDK r25c in your PATH.

```bash
# From the project root
ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=jni/Android.mk

# Output will be at:
# libs/arm64-v8a/libvr4seclient.so
```

---

## Injection via APKToolkit

1. **Decompile the APK**
   ```bash
   apktool d RoboLab.apk -o RoboLab_unpacked
   ```

2. **Drop the .so**
   ```bash
   cp libs/arm64-v8a/libvr4seclient.so \
      RoboLab_unpacked/lib/arm64-v8a/
   ```

3. **Patch the lib loader**

   Open `RoboLab_unpacked/smali/com/robolab/RoboLabActivity.smali`
   (or wherever the game calls `System.loadLibrary`).
   Add this *before* the existing `loadLibrary` call:

   ```smali
   const-string v0, "vr4seclient"
   invoke-static {v0}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V
   ```

   This forces the JVM to load our lib first, so `JNI_OnLoad` fires,
   spawns the main thread, and waits for `libRoboLab.so` to map before
   attaching to it.

4. **Repack and sign**
   ```bash
   apktool b RoboLab_unpacked -o RoboLab_modded.apk
   # Sign with apksigner / uber-apk-signer
   uber-apk-signer -a RoboLab_modded.apk --allowResign -o .
   ```

5. **Install**
   ```bash
   adb install RoboLab_modded_aligned_signed.apk
   ```

---

## Finding the real offsets (RE guide)

The offsets in `vr4seclient.h` under `namespace offsets` are **placeholders**.
You need to reverse `libRoboLab.so` with your tool of choice (IDA, Ghidra,
Binary Ninja) and replace them with real values.

### Step 1 — pull the so

```bash
adb shell pm path com.robolab.vr
# → package:/data/app/com.robolab.vr-1/base.apk
adb pull /data/app/com.robolab.vr-1/base.apk
unzip base.apk lib/arm64-v8a/libRoboLab.so
```

### Step 2 — identify the player struct

In Ghidra / IDA, search for string refs to `"playerHealth"`, `"hp"`, `"health"`.
Trace the xref back to the function that writes to it, then follow the store
instruction to its destination register. That register holds a pointer into
the player struct — the offset from the struct base is your `player_health` offset.

### Step 3 — resolve from game base

The game will ASLR-randomize the lib on each launch.
`vr4se_get_base()` reads `/proc/self/maps` at runtime to find where
`libRoboLab.so` landed. All offsets are **relative to that base**.

```
real_addr = g_base_addr + offsets::player_health
```

### Common patterns to scan for (AArch64 ASM)

**Float write to health:**
```asm
; FSTR S0, [X19, #0x2C0]  ← store float to player+0x2C0
; X19 = player object ptr
; pattern: FSTR S[0-9], \[X[0-9]+, #0x[0-9A-F]+\]
```

**Bool flag:**
```asm
; STRB WZR, [X8, #0x484]  ← zero a byte (disable flag)
; or
; MOV W9, #1
; STRB W9, [X8, #0x484]   ← set flag
```

---

## Y-button binding

RoboLab uses OpenXR. The Y button maps to
`XR_ACTION_TYPE_BOOLEAN_INPUT` on the left controller.

The game's input manager polls this into a struct. The layout at
`CONTROLLER_INPUT_BASE + Y_BUTTON_BYTE_OFFSET` is where we read the pressed
state each frame. If the Y-button read isn't working, dump the input struct:

```bash
# From a root shell on device:
adb shell
su
cat /proc/$(pidof com.robolab.vr)/maps | grep libRoboLab
# Note the base, then:
dd if=/proc/$(pidof com.robolab.vr)/mem \
   bs=1 skip=$((BASE + CONTROLLER_INPUT_BASE)) count=64 2>/dev/null | xxd
```

Find the byte that flips when you press Y — that's your `Y_BUTTON_BYTE_OFFSET`.

---

## GUI tab reference

| Tab | Features |
|---|---|
| **Player** | God Mode, ESP Boxes, ESP Names, Infinite Ammo, Health slider, Armor slider |
| **Movement** | Super Speed, No Clip, Fly Mode, Teleport to Enemy, Speed Mult slider, Jump Height slider |
| **Overpowered** | RAGE MODE, One Shot Kill, No Gravity, Infinite Stamina, Time Scale toggle + slider |
| **Guns** | Rapid Fire, No Spread, No Recoil, Bullet Penetration, Explosive Bullets, Damage Mult slider, Fire Rate slider |

---

## Animation system

```
Open (Y pressed):
  anim_slide: -300px → 0px   @ 6× speed
  anim_alpha:  0.0  → 1.0   @ 6× speed
  Duration: ~0.25s

Close (Y pressed again):
  anim_slide: 0px → -300px  @ 8× speed
  anim_alpha: 1.0 → 0.0     @ 8× speed
  Duration: ~0.20s

Toggles: pill with sliding thumb, color green/gray
Sliders: track + fill + glowing thumb
Rage Mode row: red background, immediate visual diff
```

---

built for Jason 💀
```
vr4se | vr4seclient v1.0
```
