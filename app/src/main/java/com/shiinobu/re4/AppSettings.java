package com.shiinobu.re4;

import android.content.Context;
import android.content.SharedPreferences;

/** Player settings, kept in one SharedPreferences file shared by the launcher and the game. */
final class AppSettings {
    static final int ASPECT_4_3 = 0;
    static final int ASPECT_16_9 = 1;
    static final int ASPECT_FILL = 2;
    /** Fill Screen with the game's black bars at the top and bottom (2 removes them). */
    static final int ASPECT_FILL_BARS = 3;
    /** The original 4:3 picture stretched to the whole screen (the runtime's RE4_STRETCH_43). */
    static final int ASPECT_4_3_FULL = 4;

    /** GPU driver workarounds: picked from the phone's GPU, or forced (AuroraGpuCompat). */
    static final int GPU_COMPAT_AUTO = 0;
    static final int GPU_COMPAT_ON = 1;
    static final int GPU_COMPAT_OFF = 2;

    /** Render scales offered in the menu; 1x renders 480 lines, whatever the aspect; 0 the screen's own. */
    static final float[] RESOLUTION_SCALES = {0f, 0.5f, 0.75f, 1f, 1.125f, 1.5f, 2f, 3f, 4f};

    private static final String FILE = "settings";
    private final SharedPreferences prefs;
    private final Context app;

    AppSettings(Context context) {
        // Both processes (launcher and :game) read it. Writes use commit(): the game process is
        // ended right after its menu closes, which would drop a pending apply().
        // MODE_MULTI_PROCESS: each new AppSettings rereads the file when the other process changed it
        // (the :game process otherwise keeps its first copy, and Continue missed launcher changes).
        //noinspection deprecation
        app = context.getApplicationContext();
        prefs = context.getSharedPreferences(FILE, Context.MODE_PRIVATE | Context.MODE_MULTI_PROCESS);
        // Once: the FPS / performance overlay costs frame time on slow phones, so installs that had
        // it on start with it off again (it can still be turned back on in Display settings).
        if (!prefs.getBoolean("show_fps_reset_1", false)) {
            edit().putBoolean("show_fps", false).putBoolean("show_fps_reset_1", true).commit();
        }
    }

    /**
     * Every write rereads the file first. commit() writes this copy's whole map, so a long-lived
     * AppSettings (the launcher keeps one from onCreate) wrote back the keys it had read at start over
     * what the :game process saved since: Keyboard Keys set in the game were gone at the next start
     * (1.1 test report, 2026-11-22). getSharedPreferences with MODE_MULTI_PROCESS reloads a changed file.
     */
    private SharedPreferences.Editor edit() {
        //noinspection deprecation
        app.getSharedPreferences(FILE, Context.MODE_PRIVATE | Context.MODE_MULTI_PROCESS);
        return prefs.edit();
    }

    /** DeviceProfile has not run yet on this install. */
    boolean needsDeviceProfile() { return !prefs.getBoolean("device_profile_1", false); }

    /** Thermal Control was on by default and still misjudges: turn it off once on every install. */
    void resetThermalOnce() {
        if (prefs.getBoolean("thermal_off_1", false)) return;
        edit().putBoolean("thermal_off_1", true).putBoolean("thermal_control", false).commit();
    }

    // --- cheats (runtime/src/cheats.cpp, [cheats] in Config.toml) ---------------------------------
    boolean cheatUnlockAll() { return BuildConfig.CHEATS && prefs.getBoolean("cheat_unlock_all", false); }
    void setCheatUnlockAll(boolean on) { edit().putBoolean("cheat_unlock_all", on).commit(); }
    boolean cheatInfiniteHealth() { return BuildConfig.CHEATS && prefs.getBoolean("cheat_infinite_health", false); }
    void setCheatInfiniteHealth(boolean on) { edit().putBoolean("cheat_infinite_health", on).commit(); }
    boolean cheatInfiniteAmmo() { return BuildConfig.CHEATS && prefs.getBoolean("cheat_infinite_ammo", false); }
    void setCheatInfiniteAmmo(boolean on) { edit().putBoolean("cheat_infinite_ammo", on).commit(); }
    boolean cheatSpecialWeapons() { return BuildConfig.CHEATS && prefs.getBoolean("cheat_special_weapons", false); }
    void setCheatSpecialWeapons(boolean on) { edit().putBoolean("cheat_special_weapons", on).commit(); }
    boolean cheatMaxMoney() { return BuildConfig.CHEATS && prefs.getBoolean("cheat_max_money", false); }
    void setCheatMaxMoney(boolean on) { edit().putBoolean("cheat_max_money", on).commit(); }
    boolean cheatAutoAim() { return BuildConfig.CHEATS && prefs.getBoolean("cheat_auto_aim", false); }
    void setCheatAutoAim(boolean on) { edit().putBoolean("cheat_auto_aim", on).commit(); }
    boolean cheatAutoQte() { return BuildConfig.CHEATS && prefs.getBoolean("cheat_auto_qte", false); }
    void setCheatAutoQte(boolean on) { edit().putBoolean("cheat_auto_qte", on).commit(); }
    boolean cheatItemStack() { return BuildConfig.CHEATS && prefs.getBoolean("cheat_item_stack", false); }
    void setCheatItemStack(boolean on) { edit().putBoolean("cheat_item_stack", on).commit(); }
    /** Play As (main story): pl_type 0 Leon (off), 2 Ada, 3 HUNK, 4 Krauser, 5 Wesker. */
    int cheatPlayAs() {
        final int v = BuildConfig.CHEATS ? prefs.getInt("cheat_play_as", 0) : 0;
        return v >= 2 && v <= 5 ? v : 0;
    }
    void setCheatPlayAs(int type) { edit().putInt("cheat_play_as", type).commit(); }
    boolean cheatBiggestCase() { return BuildConfig.CHEATS && prefs.getBoolean("cheat_biggest_case", false); }
    void setCheatBiggestCase(boolean on) { edit().putBoolean("cheat_biggest_case", on).commit(); }
    boolean cheatOneShotKills() { return BuildConfig.CHEATS && prefs.getBoolean("cheat_one_shot", false); }
    void setCheatOneShotKills(boolean on) { edit().putBoolean("cheat_one_shot", on).commit(); }
    boolean cheatSteadyAim() { return BuildConfig.CHEATS && prefs.getBoolean("cheat_steady_aim", false); }
    void setCheatSteadyAim(boolean on) { edit().putBoolean("cheat_steady_aim", on).commit(); }
    /** Any cheat on (the launcher's IN USE panel). */
    boolean anyCheat() {
        return cheatUnlockAll() || cheatInfiniteHealth() || cheatInfiniteAmmo() || cheatSpecialWeapons()
                || cheatMaxMoney() || cheatAutoAim() || cheatAutoQte() || cheatItemStack() || cheatPlayAs() != 0
                || cheatBiggestCase() || cheatOneShotKills() || cheatSteadyAim();
    }

    /**
     * RE4 is CPU-bound on every phone: GX Thread took the Redmi's (SD732G) game thread from 43 to
     * 25-30 ms a frame, and the SD870 (3.2 GHz) still spent 6-9 ms of its 15-21 ms frame in GX at
     * 60 fps (2026-11-12). On once for every phone with a spare core; the player can still turn it off.
     */
    void gxThreadForCpuOnce(boolean cpuBound) {
        if (prefs.getBoolean("gx_thread_re4_2", false)) return;
        edit().putBoolean("gx_thread_re4_2", true).putBoolean("gx_thread", cpuBound).commit();
    }

    /** The device profile's picks, for the settings the player never set; runs once. */
    void applyDeviceProfile(float scale, boolean gxThread) {
        final SharedPreferences.Editor e = edit().putBoolean("device_profile_1", true)
                .putBoolean("gx_thread_re4_2", true).putBoolean("thermal_off_1", true);
        if (!prefs.contains("resolution_scale")) e.putFloat("resolution_scale", scale);
        if (!prefs.contains("gx_thread")) e.putBoolean("gx_thread", gxThread);
        e.commit();
    }

    /** The second device profile (Low-End preset on a weak phone) has not run yet on this install. */
    boolean needsLowEndProfile() { return !prefs.getBoolean("device_profile_2", false); }
    void markLowEndProfile() { edit().putBoolean("device_profile_2", true).commit(); }
    boolean has(String key) { return prefs.contains(key); }

    /** GL_RENDERER seen by the launcher's device profile, for the Low-End preset in either process. */
    String gpuName() { return prefs.getString("gpu_name", ""); }
    void setGpuName(String name) { edit().putString("gpu_name", name == null ? "" : name).commit(); }

    // --- Low-End preset -----------------------------------------------------------------------------
    /** Every setting the Low-End preset may change; what they were before is kept for Undo. */
    static final String[] LOW_END_KEYS = {"display_lines", "resolution_scale", "texture_filter", "fsr_on",
        "dynamic_resolution", "hd_textures", "show_fps", "overlay_items", "frame_graph", "reduce_screen_filters",
        "reduce_shadows", "reduce_effects", "draw_distance"};
    /** Preset keys the player may still change while it is on. */
    private static final java.util.Set<String> LOW_END_FREE = new java.util.HashSet<>(java.util.Arrays.asList(
            "resolution_scale", "show_fps", "overlay_items", "frame_graph"));

    boolean lowEndApplied() { return prefs.contains("low_end_backup"); }

    /** 0 off, 1 Low-End, 2 Ultra Low (DeviceProfile.lowEndValues). */
    int lowEndLevel() { return lowEndApplied() ? Math.max(1, Math.min(2, prefs.getInt("low_end_level", 1))) : 0; }

    /**
     * A setting the Low-End preset holds while it is on: it still has the preset's value, so it cannot be
     * changed by hand (a key the preset left alone, like V-Sync the player had set, stays free). Render
     * Resolution and the performance overlay are never held (LOW_END_FREE).
     */
    boolean lowEndHolds(String key) {
        if (!lowEndApplied() || LOW_END_FREE.contains(key)) return false;
        final Object want = DeviceProfile.lowEndValues(gpuName(), lowEndLevel()).get(key);
        return want != null && want.equals(prefs.getAll().get(key));
    }

    /** Saves what the LOW_END_KEYS hold now (a missing key stays missing on Undo), then writes `values`. */
    void applyLowEnd(java.util.Map<String, Object> values) { applyLowEnd(values, 1); }

    void applyLowEnd(java.util.Map<String, Object> values, int level) {
        final java.util.Map<String, ?> all = prefs.getAll();
        final StringBuilder backup = new StringBuilder();
        for (String key : LOW_END_KEYS) {
            final Object v = all.get(key);
            final String type = v instanceof Boolean ? "b" : v instanceof Integer ? "i" : v instanceof Float ? "f" : "-";
            backup.append(key).append('=').append(type).append(':').append(v == null ? "" : v).append('\n');
        }
        final SharedPreferences.Editor e = edit().putString("low_end_backup", backup.toString())
                .putInt("low_end_level", level);
        for (java.util.Map.Entry<String, Object> entry : values.entrySet()) {
            final Object v = entry.getValue();
            if (v instanceof Boolean) e.putBoolean(entry.getKey(), (Boolean) v);
            else if (v instanceof Integer) e.putInt(entry.getKey(), (Integer) v);
            else if (v instanceof Float) e.putFloat(entry.getKey(), (Float) v);
        }
        e.commit();
    }

    /**
     * A Low-End preset applied by an older build (its backup has no overlay_items) gets the overlay keys the
     * preset now sets, once; their old values join the backup so Undo still puts them back.
     */
    void upgradeLowEnd() {
        upgradeLowEndDrawDistance();
        final String backup = prefs.getString("low_end_backup", null);
        if (backup == null || backup.contains("overlay_items=")) return;
        final java.util.Map<String, ?> all = prefs.getAll();
        final StringBuilder more = new StringBuilder(backup);
        if (more.length() > 0 && more.charAt(more.length() - 1) != '\n') more.append('\n');
        for (String key : new String[] {"overlay_items", "frame_graph"}) {
            final Object v = all.get(key);
            final String type = v instanceof Boolean ? "b" : v instanceof Integer ? "i" : "-";
            more.append(key).append('=').append(type).append(':').append(v == null ? "" : v).append('\n');
        }
        edit().putString("low_end_backup", more.toString())
                .putBoolean("show_fps", true).putInt("overlay_items", OVERLAY_FPS).putBoolean("frame_graph", false)
                .commit();
    }

    /**
     * A Low-End preset applied before Draw Distance existed gets the level's draw distance, once; the old
     * value joins the backup so Undo still puts it back.
     */
    private void upgradeLowEndDrawDistance() {
        final String backup = prefs.getString("low_end_backup", null);
        if (backup == null || backup.contains("draw_distance=")) return;
        final Object v = prefs.getAll().get("draw_distance");
        final StringBuilder more = new StringBuilder(backup);
        if (more.length() > 0 && more.charAt(more.length() - 1) != '\n') more.append('\n');
        more.append("draw_distance=").append(v instanceof Integer ? "i" : "-").append(':')
                .append(v == null ? "" : v).append('\n');
        edit().putString("low_end_backup", more.toString())
                .putInt("draw_distance", lowEndLevel() >= 2 ? 2 : 1).commit();
    }

    /** Puts back the settings from before the Low-End preset. */
    void undoLowEnd() {
        final String backup = prefs.getString("low_end_backup", null);
        if (backup == null) return;
        final SharedPreferences.Editor e = edit().remove("low_end_backup").remove("low_end_level");
        for (String line : backup.split("\n")) {
            final int eq = line.indexOf('='), colon = line.indexOf(':', eq + 1);
            if (eq <= 0 || colon < 0) continue;
            final String key = line.substring(0, eq), type = line.substring(eq + 1, colon), value = line.substring(colon + 1);
            try {
                switch (type) {
                    case "b": e.putBoolean(key, Boolean.parseBoolean(value)); break;
                    case "i": e.putInt(key, Integer.parseInt(value)); break;
                    case "f": e.putFloat(key, Float.parseFloat(value)); break;
                    default: e.remove(key); break;
                }
            } catch (NumberFormatException ignored) {
                e.remove(key);
            }
        }
        e.commit();
    }

    /**
     * Display Resolution: the short side of the surface the game draws into (0: the screen's own). The
     * compositor stretches it to the screen for free, so every full-screen pass gets cheaper.
     */
    static final int[] DISPLAY_LINES = {0, 960, 720, 540, 480, 360};
    int displayLines() { return Math.max(0, prefs.getInt("display_lines", 480)); }
    void setDisplayLines(int lines) { edit().putInt("display_lines", lines).commit(); }

    boolean disableFog() { return prefs.getBoolean("disable_fog", false); }
    void setDisableFog(boolean on) { edit().putBoolean("disable_fog", on).commit(); }
    boolean reduceEffects() { return prefs.getBoolean("reduce_effects", false); }
    // 60 FPS by frame interpolation (Beta 8): the game logic stays at 30, so the old switch's bugs are
    // gone. A new key, off for everyone: it needs GPU headroom, so the player picks it.
    // Beta 10: 0 Off, 1 Lite (RE4_60FPS=2: the moving things redrawn, the rest moved with the camera),
    // 2 Full (RE4_60FPS=1: everything redrawn). The Beta 8 switch's "on" was Full.
    // 1.2: 0 Off, 1 Lite, 2 Adaptive (Full that holds a steady 30 where the phone cannot keep 60;
    // RE4_60FPS=3, display_settings.cpp AdaptiveSixtyTick), 3 Full (always 60). In 1.1 Full and Lite
    // both held to 30 by themselves; a 1.1 Full ("sixty_fps_mode" 2) becomes Adaptive, as it played.
    static final int SIXTY_OFF = 0, SIXTY_LITE = 1, SIXTY_ADAPTIVE = 2, SIXTY_FULL = 3;
    int sixtyFpsMode() {
        if (prefs.contains("sixty_fps_mode_12")) return Math.max(0, Math.min(3, prefs.getInt("sixty_fps_mode_12", 0)));
        if (prefs.contains("sixty_fps_mode")) {
            final int old = Math.max(0, Math.min(2, prefs.getInt("sixty_fps_mode", 0)));
            return old == 2 ? SIXTY_ADAPTIVE : old;
        }
        return prefs.getBoolean("sixty_fps_interp", false) ? SIXTY_ADAPTIVE : SIXTY_OFF;
    }
    void setSixtyFpsMode(int mode) {
        edit().putInt("sixty_fps_mode_12", mode).remove("sixty_fps_mode").remove("sixty_fps_interp")
                .remove("sixty_fps_warned").remove("sixty_fps").commit();
    }
    /** RE4_60FPS for the runtime (settings_overlay.cpp): "0", "2" Lite, "3" Adaptive or "1" Full. */
    String sixtyFpsEnv() { return new String[] {"0", "2", "3", "1"}[sixtyFpsMode()]; }
    void setReduceEffects(boolean on) { edit().putBoolean("reduce_effects", on).commit(); }
    /** Reduce Screen Filters (runtime game_tweaks.cpp FilterTrans): blur, depth of field, glows, dust. */
    boolean reduceScreenFilters() { return prefs.getBoolean("reduce_screen_filters", false); }
    void setReduceScreenFilters(boolean on) { edit().putBoolean("reduce_screen_filters", on).commit(); }
    boolean reduceShadows() { return prefs.getBoolean("reduce_shadows", false); }
    void setReduceShadows(boolean on) { edit().putBoolean("reduce_shadows", on).commit(); }
    /** Draw Distance (runtime game_tweaks.cpp): 0 Full, 1 Medium, 2 Short. */
    int drawDistance() { return Math.max(0, Math.min(2, prefs.getInt("draw_distance", 0))); }
    void setDrawDistance(int level) { edit().putInt("draw_distance", level).commit(); }
    /** The far plane scale for a Draw Distance level. */
    static float drawDistanceScale(int level) { return level == 2 ? 0.5f : level == 1 ? 0.75f : 1.0f; }
    /** Settings > Display > Laser Color: 0 red (the game's), 1 blue, 2 yellow, 3 green (game_tweaks.cpp). */
    /** 0 off, 1 auto, 2 every other frame (aurora_set_frameskip). */
    int frameskip() { return Math.max(0, Math.min(2, prefs.getInt("frameskip", 0))); }
    void setFrameskip(int mode) { edit().putInt("frameskip", mode).commit(); }

    int laserColor() { return Math.max(0, Math.min(3, prefs.getInt("laser_color", 0))); }
    void setLaserColor(int which) { edit().putInt("laser_color", which).commit(); }
    /** Crosshair while aiming (crosshair.cpp): 0 off, 1 "+", 2 dot; its size 50..200%. */
    int crosshair() { return Math.max(0, Math.min(2, prefs.getInt("crosshair", 0))); }
    void setCrosshair(int style) { edit().putInt("crosshair", style).commit(); }
    int crosshairSize() { return Math.max(50, Math.min(200, prefs.getInt("crosshair_size", 100))); }
    void setCrosshairSize(int percent) { edit().putInt("crosshair_size", percent).commit(); }
    /** Settings > Display > Camera Distance: how far back the view is in free play (game_tweaks.cpp). */
    static final float[] CAMERA_DISTANCES = {1f, 1.5f, 2f};
    int cameraDistance() { return Math.max(0, Math.min(CAMERA_DISTANCES.length - 1, prefs.getInt("camera_distance", 0))); }
    void setCameraDistance(int which) { edit().putInt("camera_distance", which).commit(); }
    float cameraDistanceFactor() { return CAMERA_DISTANCES[cameraDistance()]; }
    /** Settings > Display > Speedrun Mode: IGT / RTA timer and chapter splits; cheats off (game_tweaks.cpp). */
    boolean speedrunMode() { return prefs.getBoolean("speedrun_mode", false); }
    void setSpeedrunMode(boolean on) { edit().putBoolean("speedrun_mode", on).commit(); }
    int aspectMode() { return prefs.getInt("aspect_mode", ASPECT_FILL); }
    void setAspectMode(int mode) { edit().putInt("aspect_mode", mode).commit(); }

    /** The nearest RESOLUTION_SCALES step: a value from Beta 12's percentage slider (1.35, ...) snaps. */
    float resolutionScale() {
        final float saved = prefs.getFloat("resolution_scale", 1f);
        float best = RESOLUTION_SCALES[0];
        for (float s : RESOLUTION_SCALES) {
            if (Math.abs(s - saved) < Math.abs(best - saved)) best = s;
        }
        return best;
    }
    void setResolutionScale(float scale) { edit().putFloat("resolution_scale", scale).commit(); }

    int gpuCompat() { return prefs.getInt("gpu_compat", GPU_COMPAT_AUTO); }
    void setGpuCompat(int mode) { edit().putInt("gpu_compat", mode).commit(); }

    /**
     * Upscaling when the game renders below the screen's resolution (on by default): 0 FSR Light
     * (sharpen the stretched picture), 1 FSR Full (FSR1's edge-aware upscale, heavier on the GPU),
     * 2 SGSR (Qualcomm's single-pass edge-aware upscale).
     */
    boolean fsrOn() { return prefs.getBoolean("fsr_on", true); }
    void setFsrOn(boolean on) { edit().putBoolean("fsr_on", on).commit(); }
    boolean fsrFull() { return prefs.getBoolean("fsr_full", false); }
    void setFsrFull(boolean full) { edit().putBoolean("fsr_full", full).commit(); }
    int upscaler() {
        return Math.max(0, Math.min(2, prefs.getInt("upscaler", prefs.getBoolean("fsr_full", false) ? 1 : 0)));
    }
    void setUpscaler(int mode) { edit().putInt("upscaler", Math.max(0, Math.min(2, mode))).commit(); }
    /** The config value: 0 off, 1 FSR light, 2 FSR full, 3 SGSR. */
    int fsr() { return fsrOn() ? (upscaler() + 1) : 0; }
    /** 0 off, 1 FSR Light, 2 FSR Full, 3 SGSR (Upscaling on/off plus the upscaler). */
    void setFsr(int mode) {
        setFsrOn(mode > 0);
        if (mode > 0) setUpscaler(mode - 1);
    }

    /**
     * Texture Detail for the 3D world: 0 original (as the game asks), 1 sharp (default: the Wii's
     * mip level even below its resolution + 4x anisotropic), 2 sharpest (16x anisotropic).
     */
    int textureFilter() { return Math.max(0, Math.min(2, prefs.getInt("texture_filter", 0))); }
    void setTextureFilter(int mode) { edit().putInt("texture_filter", mode).commit(); }

    /** Auto Update: opening the app installs the latest GitHub release (off by default). */
    boolean autoUpdate() { return prefs.getBoolean("auto_update", false); }
    /** The newest version offered at start without Auto Update: each one is offered once. */
    String offeredUpdate() { return prefs.getString("offered_update", ""); }
    void setOfferedUpdate(String version) { edit().putString("offered_update", version).apply(); }
    void setAutoUpdate(boolean on) { edit().putBoolean("auto_update", on).commit(); }

    boolean showFps() { return prefs.getBoolean("show_fps", false); }
    void setShowFps(boolean show) { edit().putBoolean("show_fps", show).commit(); }
    boolean spinningCoins() { return prefs.getBoolean("spinning_coins", true); }
    void setSpinningCoins(boolean on) { edit().putBoolean("spinning_coins", on).commit(); }

    /** What the performance overlay shows: OVERLAY_* bits (the runtime's kOverlay* in settings_overlay.h). */
    static final int OVERLAY_FPS = 1, OVERLAY_CPU = 2, OVERLAY_GPU = 4, OVERLAY_BATTERY = 8, OVERLAY_TEMPS = 16,
            OVERLAY_DETAILS = 32, OVERLAY_DEFAULT = 31;
    /** Not an item: the Minimal overlay style, sent to the runtime in the same bits (kOverlayMinimal). */
    static final int OVERLAY_MINIMAL = 64;
    int overlayItems() { return prefs.getInt("overlay_items", OVERLAY_DEFAULT); }
    void setOverlayItems(int items) { edit().putInt("overlay_items", items).commit(); }
    /** Settings > Display > Overlay Style: 0 = Bar (top centre), 1 = Minimal (top left, no bar). */
    int overlayStyle() { return prefs.getInt("overlay_style", 0) == 1 ? 1 : 0; }
    void setOverlayStyle(int style) { edit().putInt("overlay_style", style).commit(); }
    /** The overlay items plus the style bit, as the runtime reads them. */
    int overlayFlags() { return (overlayItems() & ~OVERLAY_MINIMAL) | (overlayStyle() == 1 ? OVERLAY_MINIMAL : 0); }

    boolean frameGraph() { return prefs.getBoolean("frame_graph", false); }
    void setFrameGraph(boolean on) { edit().putBoolean("frame_graph", on).commit(); }

    /**
     * V-Sync: frames go out in step with the display and the game's clock follows its vsync. Off, the
     * game keeps its own 59.94 Hz clock (vi.cpp) and a late frame does not wait for the next refresh.
     */
    boolean vsync() { return prefs.getBoolean("vsync", true); }
    void setVSync(boolean on) { edit().putBoolean("vsync", on).commit(); }

    /** Even Frame Pacing: the presenter's picture hold (aurora_set_picture_hold). Off by default. */
    boolean pictureHold() { return prefs.getBoolean("picture_hold", false); }
    void setPictureHold(boolean on) { edit().putBoolean("picture_hold", on).commit(); }

    boolean touchControls() { return prefs.getBoolean("touch_controls", true); }
    void setTouchControls(boolean on) { edit().putBoolean("touch_controls", on).commit(); }

    float touchOpacity() { return prefs.getFloat("touch_opacity", 0.6f); }
    void setTouchOpacity(float opacity) { edit().putFloat("touch_opacity", opacity).commit(); }

    /** Touch camera drag and drag-to-aim speed, 1 = default. */
    float cameraSensitivity() { return prefs.getFloat("camera_sensitivity", 1f); }
    void setCameraSensitivity(float s) { edit().putFloat("camera_sensitivity", s).commit(); }
    /** Settings > Controls > Aim Sensitivity: the sight's speed (touch drag, touch sticks, gamepad). */
    float aimSensitivity() { return prefs.getFloat("aim_sensitivity", 1f); }
    void setAimSensitivity(float s) { edit().putFloat("aim_sensitivity", s).commit(); }
    /** Gyro Aim: the phone's turn moves the sight while R or L is held (GyroAim). */
    boolean gyroAim() { return prefs.getBoolean("gyro_aim", false); }
    void setGyroAim(boolean on) { edit().putBoolean("gyro_aim", on).commit(); }
    float gyroSensitivity() { return prefs.getFloat("gyro_sensitivity", 1f); }
    void setGyroSensitivity(float s) { edit().putFloat("gyro_sensitivity", s).commit(); }
    boolean gyroInvertY() { return prefs.getBoolean("gyro_invert_y", false); }
    void setGyroInvertY(boolean on) { edit().putBoolean("gyro_invert_y", on).commit(); }

    boolean haptics() { return prefs.getBoolean("haptics", true); }
    void setHaptics(boolean on) { edit().putBoolean("haptics", on).commit(); }
    boolean touchCamera() { return prefs.getBoolean("touch_camera", false); }
    /** TouchControlsView.STYLE_MODERN, STYLE_CLASSIC or STYLE_ITEM_LIQUID. */
    // Defaults to Modern (matching upstream); players can optionally switch to Classic or Item Liquid.
    int touchStyle() {
        final int s = prefs.getInt("touch_style_re4", TouchControlsView.STYLE_MODERN);
        return s >= 0 && s <= TouchControlsView.STYLE_ITEM_LIQUID ? s : TouchControlsView.STYLE_MODERN;
    }
    void setTouchStyle(int style) { edit().putInt("touch_style_re4", style).commit(); }
    /** TouchIcons.COLOR_*: gold, red, blue or white; A, B, X and Y keep their own colours. */
    int touchColor() { return prefs.getInt("touch_color", TouchIcons.COLOR_BLUE); }
    void setTouchColor(int color) { edit().putInt("touch_color", color).commit(); }
    void setTouchCamera(boolean on) { edit().putBoolean("touch_camera", on).commit(); }
    /** Touch camera style: Swipe (turns Leon, the default) or Stick (a floating right stick, Beta 7-13). */
    boolean swipeCamera() { return prefs.getBoolean("swipe_camera", true); }
    void setSwipeCamera(boolean on) { edit().putBoolean("swipe_camera", on).commit(); }
    /** Settings > Controls > Gamepad Buttons (GamepadButtons): "a=b,b=a,...", "" = the default layout. */
    String gamepadMap() { return prefs.getString("gamepad_map", ""); }
    void setGamepadMap(String map) { edit().putString("gamepad_map", map).commit(); }
    /** Off: a drag to the right turns the view to the right. On (default): the scene follows the finger. */
    boolean invertCamera() { return prefs.getBoolean("invert_camera", true); }
    void setInvertCamera(boolean on) { edit().putBoolean("invert_camera", on).commit(); }

    /** The touch controls as the player arranged them in the layout editor ("" is the default). */
    boolean fixedStick() { return prefs.getBoolean("touch_fixed_stick", false); }
    void setFixedStick(boolean on) { edit().putBoolean("touch_fixed_stick", on).commit(); }
    /** Settings > Controls > Auto Run: the stick pushed forward runs, B held walks (game_tweaks.cpp). */
    boolean autoRun() { return prefs.getBoolean("auto_run", false); }
    void setAutoRun(boolean on) { edit().putBoolean("auto_run", on).commit(); }
    /** Settings > Controls > Toggle Aim (default off): a press of R or L holds it (virtual_remote.cpp). */
    boolean toggleAim() { return prefs.getBoolean("toggle_aim", false); }
    void setToggleAim(boolean on) { edit().putBoolean("toggle_aim", on).commit(); }
    /** Settings > Controls > Toggle Run: a press of B holds it, Leon keeps running (virtual_remote.cpp). */
    boolean toggleRun() { return prefs.getBoolean("toggle_run", false); }
    void setToggleRun(boolean on) { edit().putBoolean("toggle_run", on).commit(); }
    /** A tap on the picture answers a quick-time prompt (default on). */
    boolean tapQte() { return prefs.getBoolean("tap_qte", true); }
    void setTapQte(boolean on) { edit().putBoolean("tap_qte", on).commit(); }
    /** A quick swipe on the picture readies the knife and slashes (up high, down low). */
    boolean knifeSwipe() { return prefs.getBoolean("knife_swipe", false); }
    void setKnifeSwipe(boolean on) { edit().putBoolean("knife_swipe", on).commit(); }
    /** 0 Auto (hidden while a gamepad is used, a touch shows them), 1 Always. Off is touchControls(). */
    int touchShow() { return prefs.getInt("touch_show", 0); }
    void setTouchShow(int mode) { edit().putInt("touch_show", mode).commit(); }
    /** Gamepad: Alternate Controls (LT aim, RT fire, LB knife, right stick aims). */
    boolean altControls() { return prefs.getBoolean("alt_controls", false); }
    void setAltControls(boolean on) { edit().putBoolean("alt_controls", on).commit(); }
    /** Gamepad: face buttons by label (Xbox A = A) instead of by position. */
    boolean swapFace() { return prefs.getBoolean("swap_face", false); }
    void setSwapFace(boolean on) { edit().putBoolean("swap_face", on).commit(); }
    boolean swapSticks() { return prefs.getBoolean("swap_sticks", false); }
    void setSwapSticks(boolean on) { edit().putBoolean("swap_sticks", on).commit(); }
    boolean invertRightY() { return prefs.getBoolean("invert_right_y", false); }
    void setInvertRightY(boolean on) { edit().putBoolean("invert_right_y", on).commit(); }
    int rightStickAxes() { return prefs.getInt("right_stick_axes", StickAxes.AUTO); }
    void setRightStickAxes(int mode) { edit().putInt("right_stick_axes", mode).commit(); }
    /** Off by default since 1.1: saving each room wrote Play As states into the save. */
    boolean autoSave() { return prefs.getBoolean("auto_save_rooms", false); }
    void setAutoSave(boolean on) { edit().putBoolean("auto_save_rooms", on).commit(); }
    /** Gamepad stick dead zone 0..30 % and range 50..100 % (0 and 100: as the pad reports). */
    int stickDeadZone() { return prefs.getInt("stick_dead_zone", 0); }
    void setStickDeadZone(int percent) { edit().putInt("stick_dead_zone", percent).commit(); }
    int stickRange() { return prefs.getInt("stick_range", 100); }
    void setStickRange(int percent) { edit().putInt("stick_range", percent).commit(); }
    /** Mouse: a click captures it; it aims while L / R is held and turns Leon otherwise (default on). */
    boolean mouseAim() { return prefs.getBoolean("mouse_aim", true); }
    void setMouseAim(boolean on) { edit().putBoolean("mouse_aim", on).commit(); }
    float mouseSensitivity() { return prefs.getFloat("mouse_sensitivity", 1f); }
    /** Settings > Controls > Keyboard Keys ("" = the default layout; KeyboardKeys). */
    String keyboardKeys() { return prefs.getString("keyboard_keys", ""); }
    void setKeyboardKeys(String keys) { edit().putString("keyboard_keys", keys).commit(); }
    void setMouseSensitivity(float s) { edit().putFloat("mouse_sensitivity", s).commit(); }

    String touchLayout() { return prefs.getString("touch_layout", ""); }

    /** The installed GPU driver (GpuDrivers id) the game loads; "" = the phone's own. */
    String gpuDriver() { return prefs.getString("gpu_driver", ""); }

    /** 0 = Auto (Dawn; the Vulkan backend only for a driver Dawn cannot use), 1 = Vulkan backend first. */
    /**
     * Renderer: 0 Auto, 1 Vulkan (aurora's own), 2 OpenGL ES (Dawn on OpenGL ES), 3 Dawn, 4 WebGPU 1.0
     * (libmain10.so: the 1.0 renderer, GpuDrivers.useRenderer10).
     */
    int renderer() { return prefs.getInt("renderer", 0); }
    /** The Renderer choice in menu order: Auto, Vulkan, WebGPU (Dawn), WebGPU 1.0 (only with libmain10.so), OpenGL ES. */
    static final int[] RENDERER_ORDER = {0, 1, 3, 4, 2};
    static String rendererName(int which) {
        switch (which) {
            case 1: return "Vulkan";
            case 2: return "OpenGL ES (Experimental)";
            case 3: return "WebGPU";
            case 4: return "WebGPU 1.0";
            default: return "Auto";
        }
    }
    void setRenderer(int which) { edit().putInt("renderer", which).commit(); }
    void setGpuDriver(String id) { edit().putString("gpu_driver", id == null ? "" : id).commit(); }
    void setTouchLayout(String layout) { edit().putString("touch_layout", layout).commit(); }

    /** GX Thread: GX work on its own thread; read when the game starts. */
    boolean gxThread() { return prefs.getBoolean("gx_thread", false); }
    /** Minimap (runtime minimap.cpp): the room's outline with Leon, Ashley and the enemies. */
    boolean minimap() { return prefs.getBoolean("minimap", false); }
    void setMinimap(boolean on) { edit().putBoolean("minimap", on).commit(); }
    /** Minimap markers: bit 0 enemies, 1 Ashley / Luis, 2 animals, 3 items. */
    int minimapMarkers() { return prefs.getInt("minimap_markers", 11) & 15; }
    void setMinimapMarkers(int bits) { edit().putInt("minimap_markers", bits & 15).commit(); }
    /** Minimap orientation: 0 turns with the camera, 1 north up. */
    int minimapOrientation() { return Math.max(0, Math.min(1, prefs.getInt("minimap_orientation", 0))); }
    void setMinimapOrientation(int which) { edit().putInt("minimap_orientation", which).commit(); }
    /** Minimap side, percent of the screen's height (15..60). */
    int minimapSize() { return Math.max(15, Math.min(60, prefs.getInt("minimap_size", 30))); }
    void setMinimapSize(int percent) { edit().putInt("minimap_size", percent).commit(); }
    /** Minimap corner: 0 top left, 1 top right, 2 bottom left, 3 bottom right. */
    int minimapCorner() { return Math.max(0, Math.min(3, prefs.getInt("minimap_corner", 1))); }
    void setMinimapCorner(int which) { edit().putInt("minimap_corner", which).commit(); }
    /** Minimap zoom: 0 near, 1 medium, 2 far. */
    int minimapZoom() { return Math.max(0, Math.min(2, prefs.getInt("minimap_zoom", 1))); }
    void setMinimapZoom(int which) { edit().putInt("minimap_zoom", which).commit(); }
    /** Minimap opacity, percent (20..100). */
    int minimapOpacity() { return Math.max(20, Math.min(100, prefs.getInt("minimap_opacity", 80))); }
    void setMinimapOpacity(int percent) { edit().putInt("minimap_opacity", percent).commit(); }
    /** Minimap::Flags for the runtime. */
    int minimapFlags() {
        final int markers = minimapMarkers();
        return (minimap() ? 1 : 0) | ((markers & 1) != 0 ? 2 : 0) | ((markers & 2) != 0 ? 4 : 0)
                | ((markers & 4) != 0 ? 8 : 0) | (minimapOrientation() == 1 ? 16 : 0) | ((markers & 8) != 0 ? 32 : 0);
    }
    /** Skip Boot: the strap screen and the title intro skipped (runtime game_tweaks.cpp). */
    boolean skipBoot() { return prefs.getBoolean("skip_boot", true); }
    /**
     * Every start waits on a loading screen until every cached shader is rebuilt (main.cpp ShaderWait).
     * Beta 10 waited only on the first start after an install, update or new game file; later starts
     * rebuilt them in the background for about 45 s (Redmi, Adreno 618), and a game entered meanwhile
     * dropped to 2-10 FPS waiting for them, which players took for lag.
     */
    boolean waitShaders() { return true; }
    /**
     * The first wait after a new game file or an app install/update prepares the shaders (compiles
     * them); later ones load them. "shader_wait_done" holds the install stamp (NativeLib.stamp) of the
     * last finished wait.
     */
    boolean prepareShaders(Context context) {
        return !NativeLib.stamp(context).equals(prefs.getString("shader_wait_done", ""));
    }
    void setShaderWaitDone(Context context) {
        edit().putString("shader_wait_done", NativeLib.stamp(context))
                .putBoolean("shader_wait_running", false).commit();
    }
    /** Set as a shader wait starts, cleared when it ends: still set at the next start = it never ended. */
    void setShaderWaitRunning() { edit().putBoolean("shader_wait_running", true).commit(); }
    void clearShaderWaitRunning() { edit().putBoolean("shader_wait_running", false).commit(); }
    /**
     * One pipeline compile at a time (AURORA_VK_SERIAL_PIPELINES). Turned on for good once a shader wait
     * did not finish: Adreno 725 and PowerVR drivers crashed in their shader compilers at random points
     * of "Preparing shaders" with several compile threads (GitHub #17 / #19).
     */
    boolean serialPipelines() {
        if (prefs.getBoolean("shader_wait_running", false) && !prefs.getBoolean("serial_pipelines", false)) {
            edit().putBoolean("serial_pipelines", true).putBoolean("shader_wait_running", false).commit();
        }
        return prefs.getBoolean("serial_pipelines", false);
    }
    void clearShaderWaitDone() { edit().remove("shader_wait_done").commit(); }
    /** Brightness: offset from the game's default, -50..50 (its Options menu allows -30..50). */
    int brightness() { return Math.max(-50, Math.min(50, prefs.getInt("brightness", 0))); }
    void setBrightness(int offset) { edit().putInt("brightness", offset).commit(); }
    void setSkipBoot(boolean on) { edit().putBoolean("skip_boot", on).commit(); }
    void setGxThread(boolean on) { edit().putBoolean("gx_thread", on).commit(); }
    /** Thermal Control (off by default). */
    boolean thermalControl() { return prefs.getBoolean("thermal_control", false); }
    void setThermalControl(boolean on) { edit().putBoolean("thermal_control", on).commit(); }
    /** Sustained Performance (off by default): clocks the phone can hold for a long session. */
    boolean sustainedPerformance() { return prefs.getBoolean("sustained_performance", false); }
    void setSustainedPerformance(boolean on) { edit().putBoolean("sustained_performance", on).commit(); }
    /** Recording-friendly audio (off by default): the sound stays on the path a screen recording captures. */
    boolean recordingAudio() { return prefs.getBoolean("recording_audio", false); }
    void setRecordingAudio(boolean on) { edit().putBoolean("recording_audio", on).commit(); }
    /** Upload Memory (Vulkan renderer): 0 Auto (measured at start), 1 Standard (write-combined), 2 Cached. */
    int uploadMemory() { return prefs.getInt("upload_memory", 0); }
    void setUploadMemory(int mode) { edit().putInt("upload_memory", mode).commit(); }

    /** Whether the phone has Android's sustained performance mode (Window.setSustainedPerformanceMode). */
    static boolean sustainedSupported(android.content.Context context) {
        final android.os.PowerManager pm = context.getSystemService(android.os.PowerManager.class);
        return pm != null && pm.isSustainedPerformanceModeSupported();
    }

    /**
     * Thermal Control works on every phone: the native side reads the system's thermal headroom
     * where there is one and the battery temperature otherwise. Not probed here: Android answers
     * NaN to headroom calls less than 10 s apart, and a probe right before the game spent its call.
     */
    static boolean thermalSupported(android.content.Context context) {
        return true;
    }

    /** Replacement textures (Dolphin-style tex1_*.png) from /sdcard/RE4Wii[-Beta]/Textures. */
    boolean customTextures() { return prefs.getBoolean("custom_textures", false); }
    void setCustomTextures(boolean on) { edit().putBoolean("custom_textures", on).commit(); }
    /** The texture mods in order with their ticks (TexturePacks: "1|name" or "0|name" per line). */
    String texturePacks() { return prefs.getString("texture_packs", ""); }
    void setTexturePacks(String value) { edit().putString("texture_packs", value).commit(); }
    /** With custom textures: every texture the game uses goes to Textures/Dump, named for its replacement. */
    boolean dumpTextures() { return prefs.getBoolean("dump_textures", false); }
    void setDumpTextures(boolean on) { edit().putBoolean("dump_textures", on).commit(); }

    /** HD Textures: 0 off, 1 live (upscaled 2x while playing, kept in HD-Cache). */
    int hdTextures() { return Math.max(0, Math.min(1, prefs.getInt("hd_textures", 0))); }
    void setHdTextures(int mode) { edit().putInt("hd_textures", mode).commit(); }
    /** Disk budget of the HD texture cache, in MB. */
    static final int[] HD_CACHE_SIZES = {512, 1024, 2048};
    int hdCacheMb() { return prefs.getInt("hd_cache_mb", 1024); }
    void setHdCacheMb(int mb) { edit().putInt("hd_cache_mb", mb).commit(); }
    /** Report Texture Keys: the log lists the name of each texture the game uses (no images). */
    boolean textureKeyReport() { return prefs.getBoolean("texture_key_report", false); }
    void setTextureKeyReport(boolean on) { edit().putBoolean("texture_key_report", on).commit(); }

    /** The launcher's language: Tr.AUTO (from the phone's region), Tr.ENGLISH or Tr.INDONESIAN. */
    int language() { return prefs.getInt("language", Tr.AUTO); }
    void setLanguage(int language) { edit().putInt("language", language).commit(); }

    /** The build whose release notes the player has seen ("" before the first). */
    String notesSeen() { return prefs.getString("notes_seen", ""); }
    void setNotesSeen(String build) { edit().putString("notes_seen", build).commit(); }

    /** Lowers the render resolution while the GPU holds the game back (the chosen one is the ceiling). */
    boolean dynamicResolution() { return prefs.getBoolean("dynamic_resolution", false); }
    void setDynamicResolution(boolean on) { edit().putBoolean("dynamic_resolution", on).commit(); }

    /** Beta only: why the Beta ended, once BetaGate has seen it end (kept, so going offline won't reopen it). */
    /**
     * Why this Beta ended, or null. Kept with the versionCode it was seen on: an update installed over
     * an ended Beta starts clean (the next check locks it again if it is still too old).
     */
    String betaEnded() {
        final String reason = prefs.getString("beta_ended", null);
        if (reason == null) return null;
        if (prefs.getInt("beta_ended_code", -1) != BuildConfig.VERSION_CODE) {
            edit().remove("beta_ended").remove("beta_ended_code").commit();
            return null;
        }
        return reason;
    }
    void clearBetaEnded() { edit().remove("beta_ended").remove("beta_ended_code").commit(); }

    void setBetaEnded(String reason) {
        edit().putString("beta_ended", reason).putInt("beta_ended_code", BuildConfig.VERSION_CODE).commit();
    }

    /** "2× (960p)": the scale and the number of lines it renders, as the menu shows it. */
    static String resolutionLabel(float scale) {
        if (scale <= 0f) return Tr.t("Match screen");
        if (Math.abs(scale - 1.125f) < 0.01f) return "540p";
        final String factor = scale == Math.rint(scale)
                ? String.valueOf((int) scale) : String.valueOf(scale);
        return factor + "× (" + Math.round(480 * scale) + "p)";
    }
}
