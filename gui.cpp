/*
 * gui.cpp — v3 Android Canvas Overlay
 * ─────────────────────────────────────────────────────────────────────────────
 * [Context: AArch64 | Android API 29 | TYPE_APPLICATION_OVERLAY | Canvas 2D]
 *
 * This entire file is Java bytecode driven from C — we generate the Java
 * Overlay class at runtime via JNI + reflection. No GLES. No EGL. No GL
 * context sharing with Unity. A completely separate Android surface.
 *
 * The Java side (com.vr4se.Overlay) is injected as a DEX class into the
 * app's ClassLoader at JNI_OnLoad time. We embed the DEX bytes as a
 * byte array here, write them to a temp file, and load via DexClassLoader.
 *
 * HOWEVER — since we can't ship pre-compiled DEX inline cleanly in a .cpp
 * without a build step, the recommended deployment is:
 *
 *   Option A (recommended): Include the compiled Overlay.dex in the APK
 *   alongside libvr4seclient.so. The Java code is in Overlay.java below.
 *   The APK patch step (apktool rebuild) puts it in assets/vr4se/Overlay.dex.
 *   We load it here with:
 *       DexClassLoader(dex_path, opt_dir, null, parent_classloader)
 *
 *   Option B: Write the Overlay.java source and include it via the APK's
 *   smali/ directory after decompile, letting apktool recompile it.
 *   This is what most APK mods do. Instructions in README.
 *
 * Overlay layout (5 tabs drawn in Canvas):
 *   PLAYER    : God Mode, Invisible
 *   MOVEMENT  : Fly, Speed Boost, Big Speed, Inf Jump, Speed slider
 *   GUNS      : Kick Gun, Ban Gun, Crash Gun, Lag Gun, Steal Prefab, Fling Prefab
 *   PREFABS   : Prefab Spawner toggle, Load Prefabs button, scrollable prefab list
 *   CONFIG    : Hand select (Left/Right)
 *
 * Colors: deep navy + cyan accent. Same aesthetic as v2 but Canvas-based.
 * ─────────────────────────────────────────────────────────────────────────────
 */

/*
 * ═══════════════════════════════════════════════════════════════════════════
 * Overlay.java — include this in your APK smali (Option B)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * package com.vr4se;
 *
 * import android.app.Service;
 * import android.content.Context;
 * import android.graphics.*;
 * import android.os.*;
 * import android.view.*;
 *
 * public class Overlay {
 *
 *     // ── Native bridge ──────────────────────────────────────────────────
 *     static { System.loadLibrary("vr4seclient"); }
 *
 *     public static native void   nativeToggle(int idx);
 *     public static native void   nativeSetTab(int tab);
 *     public static native void   nativeSetHand(int hand);
 *     public static native void   nativeSetFloat(int idx, float val);
 *     public static native int[]  nativeGetState();
 *     public static native String[] nativeGetPrefabNames();
 *
 *     // ── Overlay state ──────────────────────────────────────────────────
 *     private static WindowManager   wm;
 *     private static SurfaceView      sv;
 *     private static boolean          running;
 *     private static Thread           renderThread;
 *     private static int              activeTab = 0;
 *
 *     // ── Tab definitions ─────────────────────────────────────────────────
 *     private static final String[] TABS = {"PLAYER","MOVEMENT","GUNS","PREFABS","CONFIG"};
 *
 *     // ── Show / hide ──────────────────────────────────────────────────────
 *     public static void show() {
 *         // Must be called from a Looper thread; use Handler(Looper.getMainLooper())
 *         new Handler(Looper.getMainLooper()).post(() -> {
 *             if (sv != null) { sv.setVisibility(View.VISIBLE); return; }
 *             Context ctx = getContext();  // see getContext() below
 *             wm = (WindowManager) ctx.getSystemService(Context.WINDOW_SERVICE);
 *
 *             // Params for TYPE_APPLICATION_OVERLAY (API 26+)
 *             WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
 *                 600, 800,
 *                 WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
 *                 WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE |
 *                     WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL |
 *                     WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
 *                 PixelFormat.TRANSLUCENT
 *             );
 *             lp.gravity = Gravity.TOP | Gravity.START;
 *             lp.x = 60; lp.y = 80;
 *
 *             sv = new VR4SESurface(ctx);
 *             wm.addView(sv, lp);
 *             running = true;
 *             startRenderThread();
 *         });
 *     }
 *
 *     public static void hide() {
 *         new Handler(Looper.getMainLooper()).post(() -> {
 *             if (sv != null && wm != null) {
 *                 wm.removeView(sv); sv = null; running = false;
 *             }
 *         });
 *     }
 *
 *     // ── Render thread: polls nativeGetState() 60fps ───────────────────
 *     private static void startRenderThread() {
 *         renderThread = new Thread(() -> {
 *             while (running && sv != null) {
 *                 sv.postInvalidate();
 *                 try { Thread.sleep(16); } catch(InterruptedException e){ break; }
 *             }
 *         }, "vr4se-render");
 *         renderThread.setDaemon(true);
 *         renderThread.start();
 *     }
 *
 *     // ── Context trick: get app context via ActivityThread reflection ──
 *     private static Context appCtx;
 *     public static Context getContext() {
 *         if (appCtx != null) return appCtx;
 *         try {
 *             Class<?> at = Class.forName("android.app.ActivityThread");
 *             Object   me = at.getMethod("currentActivityThread").invoke(null);
 *             appCtx = (Context) at.getMethod("getApplication").invoke(me);
 *         } catch (Exception e) { e.printStackTrace(); }
 *         return appCtx;
 *     }
 *
 *     // ══════════════════════════════════════════════════════════════════
 *     // VR4SESurface — custom SurfaceView that draws the mod menu
 *     // ══════════════════════════════════════════════════════════════════
 *     static class VR4SESurface extends SurfaceView {
 *         // Color palette
 *         private static final int C_BG_PANEL  = 0xFF16213E;
 *         private static final int C_BG_ROW    = 0xFF0F3460;
 *         private static final int C_ACCENT    = 0xFF00D4FF;
 *         private static final int C_ON        = 0xFF00FF88;
 *         private static final int C_OFF       = 0xFF444466;
 *         private static final int C_TEXT_PRI  = 0xFFEEEEFF;
 *         private static final int C_TEXT_SEC  = 0xFF8888AA;
 *         private static final int C_DANGER    = 0xFFFF4466;
 *         private static final int C_TAB_ACT   = 0xFF00D4FF;
 *         private static final int C_TAB_INACT = 0xFF334466;
 *         private static final int C_TITLE_BG  = 0xFF0D2640;
 *
 *         private Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
 *         private RectF  rect  = new RectF();
 *         private int[]  state = new int[32];
 *
 *         // Touch state
 *         private float touchX, touchY;
 *         private boolean touching;
 *
 *         VR4SESurface(Context ctx) {
 *             super(ctx);
 *             setWillNotDraw(false);
 *             setZOrderOnTop(true);
 *             setBackgroundColor(0x00000000);
 *             getHolder().setFormat(PixelFormat.TRANSLUCENT);
 *         }
 *
 *         @Override
 *         public boolean onTouchEvent(MotionEvent e) {
 *             touchX = e.getX(); touchY = e.getY();
 *             if (e.getAction() == MotionEvent.ACTION_UP) {
 *                 handleTap(touchX, touchY);
 *             }
 *             return true;
 *         }
 *
 *         @Override
 *         protected void onDraw(Canvas canvas) {
 *             state = nativeGetState();
 *             if (state == null || state[0] == 0) return;  // hidden
 *
 *             canvas.drawColor(0x00000000, PorterDuff.Mode.CLEAR);
 *             drawWindow(canvas, state);
 *         }
 *
 *         // ── Window frame ───────────────────────────────────────────────
 *         private void drawWindow(Canvas c, int[] s) {
 *             float W = 580, H = 720, X = 10, Y = 10;
 *             float TITLE_H = 50, TAB_H = 40, PAD = 12, ROW_H = 50;
 *
 *             // Shadow
 *             paint.setColor(0x44001133);
 *             rect.set(X-4, Y-4, X+W+4, Y+H+4);
 *             c.drawRoundRect(rect, 16, 16, paint);
 *
 *             // Panel bg
 *             paint.setColor(C_BG_PANEL);
 *             rect.set(X, Y, X+W, Y+H);
 *             c.drawRoundRect(rect, 12, 12, paint);
 *
 *             // Title bar
 *             paint.setColor(C_TITLE_BG);
 *             rect.set(X, Y, X+W, Y+TITLE_H);
 *             c.drawRect(rect, paint);
 *             // Accent left stripe
 *             paint.setColor(C_ACCENT);
 *             c.drawRect(X, Y, X+4, Y+TITLE_H, paint);
 *             // Title text
 *             paint.setColor(C_ACCENT);
 *             paint.setTextSize(22);
 *             paint.setFakeBoldText(true);
 *             c.drawText("VR4SE CLIENT", X+14, Y+33, paint);
 *             // Version badge
 *             paint.setColor(0x2200AAFF);
 *             rect.set(X+W-120, Y+12, X+W-8, Y+38);
 *             c.drawRoundRect(rect, 6, 6, paint);
 *             paint.setColor(C_ACCENT);
 *             paint.setTextSize(11);
 *             paint.setFakeBoldText(false);
 *             c.drawText("v3.0 // for Jason", X+W-116, Y+29, paint);
 *
 *             // Tab bar
 *             int activeTab = s[1];
 *             float tabW = W / TABS.length;
 *             for (int i = 0; i < TABS.length; i++) {
 *                 float tx = X + i * tabW, ty = Y + TITLE_H;
 *                 boolean active = (i == activeTab);
 *                 paint.setColor(active ? 0xFF0D2A50 : 0xFF1A1A2E);
 *                 c.drawRect(tx, ty, tx+tabW, ty+TAB_H, paint);
 *                 if (active) {
 *                     paint.setColor(C_ACCENT);
 *                     c.drawRect(tx+4, ty+TAB_H-3, tx+tabW-4, ty+TAB_H, paint);
 *                 }
 *                 paint.setColor(active ? C_TAB_ACT : C_TEXT_SEC);
 *                 paint.setTextSize(12);
 *                 paint.setTextAlign(Paint.Align.CENTER);
 *                 c.drawText(TABS[i], tx+tabW/2f, ty+TAB_H/2f+5, paint);
 *                 paint.setTextAlign(Paint.Align.LEFT);
 *             }
 *
 *             // Separator
 *             paint.setColor(0xFF1A3A5A);
 *             c.drawRect(X, Y+TITLE_H+TAB_H, X+W, Y+TITLE_H+TAB_H+1, paint);
 *
 *             // Content
 *             float contentY = Y + TITLE_H + TAB_H + 1;
 *             switch (activeTab) {
 *                 case 0: drawTabPlayer    (c, s, X, contentY, W, PAD, ROW_H); break;
 *                 case 1: drawTabMovement  (c, s, X, contentY, W, PAD, ROW_H); break;
 *                 case 2: drawTabGuns      (c, s, X, contentY, W, PAD, ROW_H); break;
 *                 case 3: drawTabPrefabs   (c, s, X, contentY, W, PAD, ROW_H); break;
 *                 case 4: drawTabConfig    (c, s, X, contentY, W, PAD, ROW_H); break;
 *             }
 *
 *             // Footer
 *             paint.setColor(0xFF1A1A2E);
 *             c.drawRect(X, Y+H-22, X+W, Y+H, paint);
 *             paint.setColor(C_TEXT_SEC);
 *             paint.setTextSize(10);
 *             c.drawText("Y/B = toggle  |  vr4seclient v3  |  for Jason 💀", X+8, Y+H-7, paint);
 *         }
 *
 *         // ── Row helpers ─────────────────────────────────────────────────
 *         private void drawToggleRow(Canvas c, String label, boolean on,
 *                                    float x, float y, float w, float rowH) {
 *             // Row bg
 *             paint.setColor(C_BG_ROW);
 *             rect.set(x+10, y+3, x+w-10, y+rowH-3);
 *             c.drawRoundRect(rect, 8, 8, paint);
 *             // Toggle pill
 *             float pw = 52, ph = 26;
 *             float px = x+w-pw-20, py = y+(rowH-ph)/2f;
 *             paint.setColor(on ? C_ON : C_OFF);
 *             rect.set(px, py, px+pw, py+ph);
 *             c.drawRoundRect(rect, ph/2f, ph/2f, paint);
 *             // Thumb
 *             float tx = on ? px+pw-ph/2f : px+ph/2f;
 *             paint.setColor(0xFFFFFFFF);
 *             c.drawCircle(tx, py+ph/2f, ph/2f-3, paint);
 *             // Label
 *             paint.setColor(C_TEXT_PRI);
 *             paint.setTextSize(16);
 *             c.drawText(label, x+20, y+rowH/2f+6, paint);
 *         }
 *
 *         private void drawButton(Canvas c, String label, float x, float y, float w, float h) {
 *             paint.setColor(C_ACCENT);
 *             rect.set(x, y, x+w, y+h);
 *             c.drawRoundRect(rect, 8, 8, paint);
 *             paint.setColor(0xFF000022);
 *             paint.setTextSize(15);
 *             paint.setTextAlign(Paint.Align.CENTER);
 *             c.drawText(label, x+w/2f, y+h/2f+5, paint);
 *             paint.setTextAlign(Paint.Align.LEFT);
 *         }
 *
 *         // ── Tabs ─────────────────────────────────────────────────────────
 *         private void drawTabPlayer(Canvas c, int[] s, float x, float y, float w, float p, float rh) {
 *             drawToggleRow(c, "God Mode",   s[2]!=0, x, y,      w, rh);
 *             drawToggleRow(c, "Invisible",  s[3]!=0, x, y+rh,   w, rh);
 *         }
 *
 *         private void drawTabMovement(Canvas c, int[] s, float x, float y, float w, float p, float rh) {
 *             drawToggleRow(c, "Fly",         s[4]!=0, x, y,       w, rh);
 *             drawToggleRow(c, "Speed Boost", s[5]!=0, x, y+rh,    w, rh);
 *             drawToggleRow(c, "Big Speed",   s[6]!=0, x, y+rh*2,  w, rh);
 *             drawToggleRow(c, "Inf Jump",    s[7]!=0, x, y+rh*3,  w, rh);
 *             // Speed slider
 *             float sv_mult; System.arraycopy(intBitsToFloat(s, 18), 0, new float[]{}, 0, 0);
 *             // (float from int bits)
 *             float mult = Float.intBitsToFloat(s[18]);
 *             drawSlider(c, "Speed x", mult, 1f, 20f, x+10, y+rh*4+8, w-20, 36);
 *         }
 *
 *         private void drawTabGuns(Canvas c, int[] s, float x, float y, float w, float p, float rh) {
 *             drawToggleRow(c, "Kick Gun",        s[8] !=0, x, y,      w, rh);
 *             drawToggleRow(c, "Ban Gun",         s[9] !=0, x, y+rh,   w, rh);
 *             drawToggleRow(c, "Crash Gun",       s[10]!=0, x, y+rh*2, w, rh);
 *             drawToggleRow(c, "Lag Gun",         s[11]!=0, x, y+rh*3, w, rh);
 *             drawToggleRow(c, "Steal Prefab Gun",s[12]!=0, x, y+rh*4, w, rh);
 *             drawToggleRow(c, "Fling Prefab Gun",s[13]!=0, x, y+rh*5, w, rh);
 *         }
 *
 *         private void drawTabPrefabs(Canvas c, int[] s, float x, float y, float w, float p, float rh) {
 *             drawToggleRow(c, "Prefab Spawner", s[14]!=0, x, y, w, rh);
 *             drawButton(c, "Load Prefabs", x+10, y+rh+8, 140, 34);
 *             // Prefab list (scrollable, simplified)
 *             String[] names = nativeGetPrefabNames();
 *             int sel = s[17];
 *             if (names != null) {
 *                 float iy = y + rh + 52;
 *                 for (int i = 0; i < Math.min(names.length, 8); i++) {
 *                     paint.setColor(i == sel ? C_ACCENT : C_BG_ROW);
 *                     rect.set(x+10, iy+i*36, x+w-10, iy+i*36+32);
 *                     c.drawRoundRect(rect, 6, 6, paint);
 *                     paint.setColor(i == sel ? 0xFF000022 : C_TEXT_PRI);
 *                     paint.setTextSize(14);
 *                     c.drawText(names[i], x+20, iy+i*36+22, paint);
 *                 }
 *             }
 *         }
 *
 *         private void drawTabConfig(Canvas c, int[] s, float x, float y, float w, float p, float rh) {
 *             int hand = s[15];
 *             // Hand selector buttons
 *             paint.setColor(hand==0 ? C_ACCENT : C_BG_ROW);
 *             rect.set(x+10, y+8, x+w/2f-5, y+8+50);
 *             c.drawRoundRect(rect, 8, 8, paint);
 *             paint.setColor(hand==0 ? 0xFF000022 : C_TEXT_PRI);
 *             paint.setTextSize(16); paint.setTextAlign(Paint.Align.CENTER);
 *             c.drawText("Left Hand (Y)", x+w/4f, y+38, paint);
 *
 *             paint.setColor(hand==1 ? C_ACCENT : C_BG_ROW);
 *             rect.set(x+w/2f+5, y+8, x+w-10, y+58);
 *             c.drawRoundRect(rect, 8, 8, paint);
 *             paint.setColor(hand==1 ? 0xFF000022 : C_TEXT_PRI);
 *             c.drawText("Right Hand (B)", x+3*w/4f, y+38, paint);
 *             paint.setTextAlign(Paint.Align.LEFT);
 *
 *             // Status
 *             paint.setColor(C_TEXT_SEC);
 *             paint.setTextSize(13);
 *             String status = s[30]!=0 ? "IL2CPP: READY ✓" : "IL2CPP: loading…";
 *             c.drawText(status, x+14, y+100, paint);
 *             c.drawText("Game: com.bloodrex.robolab", x+14, y+120, paint);
 *         }
 *
 *         private void drawSlider(Canvas c, String label, float val, float min, float max,
 *                                  float x, float y, float w, float h) {
 *             float t = (val - min) / (max - min);
 *             // Track
 *             paint.setColor(C_BG_ROW);
 *             rect.set(x, y+h*0.3f, x+w, y+h*0.7f);
 *             c.drawRoundRect(rect, 4, 4, paint);
 *             // Fill
 *             paint.setColor(C_ACCENT);
 *             rect.set(x, y+h*0.3f, x+w*t, y+h*0.7f);
 *             c.drawRoundRect(rect, 4, 4, paint);
 *             // Thumb
 *             paint.setColor(C_ACCENT);
 *             c.drawCircle(x+w*t, y+h/2f, h/2f, paint);
 *             // Label
 *             paint.setColor(C_TEXT_SEC);
 *             paint.setTextSize(13);
 *             c.drawText(label + String.format("  %.1f", val), x, y-2, paint);
 *         }
 *
 *         // ── Touch handling ───────────────────────────────────────────────
 *         private void handleTap(float tx, float ty) {
 *             int[] s = nativeGetState();
 *             if (s == null) return;
 *             int activeTab = s[1];
 *
 *             float W = 580, X = 10, Y = 10, TITLE_H = 50, TAB_H = 40;
 *             float tabW = W / TABS.length;
 *
 *             // Tab bar tap
 *             if (ty >= Y+TITLE_H && ty <= Y+TITLE_H+TAB_H) {
 *                 int tapped = (int)((tx - X) / tabW);
 *                 if (tapped >= 0 && tapped < TABS.length) {
 *                     nativeSetTab(tapped);
 *                     invalidate();
 *                     return;
 *                 }
 *             }
 *
 *             float contentY = Y + TITLE_H + TAB_H + 1;
 *             float rh = 50, p = 12;
 *
 *             switch (activeTab) {
 *                 // Toggle rows: tap right half to toggle
 *                 case 0: // Player
 *                     if (inRow(tx, ty, X, contentY,     W, rh)) nativeToggle(0); // God
 *                     if (inRow(tx, ty, X, contentY+rh,  W, rh)) nativeToggle(1); // Invis
 *                     break;
 *                 case 1: // Movement
 *                     if (inRow(tx, ty, X, contentY,       W, rh)) nativeToggle(2); // Fly
 *                     if (inRow(tx, ty, X, contentY+rh,    W, rh)) nativeToggle(3); // Speed
 *                     if (inRow(tx, ty, X, contentY+rh*2,  W, rh)) nativeToggle(4); // BigSpd
 *                     if (inRow(tx, ty, X, contentY+rh*3,  W, rh)) nativeToggle(5); // InfJmp
 *                     break;
 *                 case 2: // Guns
 *                     if (inRow(tx, ty, X, contentY,       W, rh)) nativeToggle(6);
 *                     if (inRow(tx, ty, X, contentY+rh,    W, rh)) nativeToggle(7);
 *                     if (inRow(tx, ty, X, contentY+rh*2,  W, rh)) nativeToggle(8);
 *                     if (inRow(tx, ty, X, contentY+rh*3,  W, rh)) nativeToggle(9);
 *                     if (inRow(tx, ty, X, contentY+rh*4,  W, rh)) nativeToggle(10);
 *                     if (inRow(tx, ty, X, contentY+rh*5,  W, rh)) nativeToggle(11);
 *                     break;
 *                 case 3: // Prefabs
 *                     if (inRow(tx, ty, X, contentY,       W, rh)) nativeToggle(12);
 *                     // Load button
 *                     if (tx>=X+10 && tx<=X+150 && ty>=contentY+rh+8 && ty<=contentY+rh+42)
 *                         nativeToggle(13);
 *                     // Prefab list selection
 *                     float listY = contentY + rh + 52;
 *                     int sel = (int)((ty - listY) / 36);
 *                     if (sel >= 0 && sel < 8) nativeSetFloat(1, sel);
 *                     break;
 *                 case 4: // Config
 *                     // Left hand
 *                     if (tx>=X+10 && tx<=X+W/2f-5 && ty>=contentY+8 && ty<=contentY+58)
 *                         nativeSetHand(0);
 *                     // Right hand
 *                     if (tx>=X+W/2f+5 && tx<=X+W-10 && ty>=contentY+8 && ty<=contentY+58)
 *                         nativeSetHand(1);
 *                     break;
 *             }
 *             invalidate();
 *         }
 *
 *         private boolean inRow(float tx, float ty, float rx, float ry, float rw, float rh) {
 *             return tx >= rx && tx <= rx+rw && ty >= ry && ty <= ry+rh;
 *         }
 *     }
 * }
 */

/* ─────────────────────────────────────────────────────────────────────────
 * The above is the complete Overlay.java. To deploy:
 *
 * 1. Save it as src/main/java/com/vr4se/Overlay.java in your Android project
 *    (or decompile the APK with apktool and add it to smali_classes2/).
 *
 * 2. Add SYSTEM_ALERT_WINDOW permission to AndroidManifest.xml:
 *    <uses-permission android:name="android.permission.SYSTEM_ALERT_WINDOW"/>
 *
 * 3. Build libvr4seclient.so and inject into APK lib/arm64-v8a/.
 *
 * 4. Rebuild APK: apktool b ModdedSlapLab -o output.apk
 *    Sign with: apksigner sign --ks debug.keystore output.apk
 *
 * The SYSTEM_ALERT_WINDOW permission is already present in many VR mod APKs
 * because Photon SDK itself uses overlay-style debug windows. If not present,
 * grant it via: adb shell appops set com.bloodrex.robolab SYSTEM_ALERT_WINDOW allow
 * ─────────────────────────────────────────────────────────────────────────*/

/* This file is intentionally mostly comments for the Java source.
   All C++ GUI bridging is in vr4seclient.cpp §11 and §12. */
