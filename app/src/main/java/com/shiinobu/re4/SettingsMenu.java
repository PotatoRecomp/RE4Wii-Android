package com.shiinobu.re4;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Typeface;
import android.util.DisplayMetrics;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * The settings, one section at a time, drawn into a column the caller owns: the launcher's page (under
 * its category tabs) or the in-game menu. Each change is saved to the app's preferences and written to
 * Config.toml at once, then `onChanged` runs (the launcher redraws its summary, the game applies what
 * it can while playing).
 *
 * In the game, settings that are only read when the game starts carry a red "restart" warning: they
 * are saved, and apply the next time the game starts.
 */
final class SettingsMenu {
    private static final String TAG = "RE4";

    static final int DISPLAY = 0, GRAPHICS = 1, PERFORMANCE = 2, CONTROLS = 3, SAVE_DATA = 4, CHEATS = 5,
            TEXTURES = 6, SYSTEM = 7, DEVELOPER = 8;
    private static final String[] NAMES = {"Display", "Graphics", "Performance", "Controls", "Save Data",
        "Cheats", "Textures", "System", "Developer"};
    private static final int[] ICONS = {R.drawable.ic_display, R.drawable.ic_graphics, R.drawable.ic_performance,
        R.drawable.ic_controls, R.drawable.ic_save, R.drawable.ic_cheats, R.drawable.ic_textures,
        R.drawable.ic_system, R.drawable.ic_developer};

    static String name(int section) { return Tr.t(NAMES[section]); }
    static int icon(int section) { return ICONS[section]; }
    /** Sections a player should know are risky (warning colour on their tab). */
    static boolean risky(int section) { return section == CHEATS; }
    /** A tab shown greyed out that cannot be opened: Cheats in a build without them (-Pre4Cheats). */
    static boolean disabled(int section) { return section == CHEATS && !BuildConfig.CHEATS; }

    /**
     * The game's menu holds every setting, System last; the tabs shrink their text to fit one row.
     * Cheats is listed in every build, greyed out without BuildConfig.CHEATS.
     */
    static int[] gameSections() {
        return new int[] {DISPLAY, GRAPHICS, PERFORMANCE, CONTROLS, SAVE_DATA, CHEATS, TEXTURES, SYSTEM};
    }

    /** The game's own rows for System (game file, updates). */
    interface SystemRows { void fill(SettingsMenu menu); }

    private final Activity activity;
    private final LinearLayout rows;
    private final boolean inGame;
    private final Runnable onChanged;
    private AppSettings settings;
    private int section = DISPLAY;
    SystemRows systemRows;
    /** The owner draws its own frame (tabs, title, buttons) again in the new language. */
    Runnable languageChanged;

    SettingsMenu(Activity activity, LinearLayout rows, boolean inGame, Runnable onChanged) {
        this.activity = activity;
        this.rows = rows;
        this.inGame = inGame;
        this.onChanged = onChanged;
        this.settings = new AppSettings(activity);
    }

    int section() { return section; }

    private void save() {
        try {
            AppFiles.writeConfig(activity, settings);
        } catch (Exception e) {
            Log.e(TAG, "could not save the settings", e);
        }
    }

    /** After a change: save, tell the owner, and draw the section again with the new values. */
    void changed() {
        save();
        if (onChanged != null) onChanged.run();
        show(section);
    }

    /** Draws `index` again in place (the views swap before the next frame, so nothing jumps). */
    void show(int index) {
        if (index == TEXTURES && section != index && !hdBusy) hdCacheBytes = -1;  // count again on entry
        section = index;
        settings = new AppSettings(activity);  // the other process may have written it
        rows.removeAllViews();
        switch (index) {
            case DISPLAY: showDisplay(); break;
            case GRAPHICS: showGraphics(); break;
            case PERFORMANCE: showPerformance(); break;
            case CONTROLS: showControls(); break;
            case SAVE_DATA: showSaveData(); break;
            case CHEATS: showCheats(); break;
            case TEXTURES: showTextures(); break;
            default:
                if (systemRows != null) systemRows.fill(this);
                break;
        }
        if (pendingChanged != null) pendingChanged.run();
    }

    /** Runs after each draw: the in-game footer shows the restart-only settings that wait (pendingRestarts). */
    Runnable pendingChanged;
    /** In game: closes the pause menu and opens the F10 bar (GameActivity). */
    Runnable openControllerBar;

    /**
     * A setting read once when the game starts: the card already says so in red. It is saved now and
     * applies after Restart (beside Resume) or the next time the app opens.
     */
    private void confirmRestart(String name, String now, String before, Runnable revert) {
        changed();
    }

    // --- sections --------------------------------------------------------------------------------

    private void showDisplay() {
        group(Tr.t("Screen"));
        choiceRow(Tr.t("Language"), Tr.t("The menus' language. Auto follows the phone's region: Indonesian "
                        + "in Indonesia, English elsewhere."),
                new String[] {Tr.t("Auto"), "English", "Bahasa Indonesia"}, settings.language(), true, which -> {
                    settings.setLanguage(which);
                    Tr.init(activity);
                    START_VALUES.clear();  // keyed by the translated names
                    PENDING.clear();
                    changed();  // drawn again in the new language
                    if (languageChanged != null) languageChanged.run();  // the tabs, title and buttons too
                });
        choiceRow(Tr.t("Aspect Ratio"), Tr.t("The shape of the picture.") + " "
                        + Tr.t("Fill Screen covers the whole screen; Fill Screen + Bars keeps the game's black "
                        + "bars at the top and bottom.") + " "
                        + Tr.t("4:3 Full Screen stretches the original 4:3 picture to the whole screen."),
                new String[] {Tr.t("Original 4:3"), "16:9", Tr.t("Fill Screen"), Tr.t("Fill Screen + Bars"),
                        Tr.t("4:3 Full Screen")},
                settings.aspectMode(), true,
                which -> settings.setAspectMode(which));
        resolutionRow();
        displayResolutionRow();
        brightnessRow();

        group(Tr.t("Gameplay"));
        choiceRow(Tr.t("Laser Color"), Tr.t("The color of the laser sight and its dot."),
                new String[] {Tr.t("Red"), Tr.t("Blue"), Tr.t("Yellow"), Tr.t("Green")}, settings.laserColor(), true,
                which -> settings.setLaserColor(which));
        choiceRow(Tr.t("Crosshair"), Tr.t("A sight where the laser hits while aiming, shown even when the laser "
                        + "line is hard to see or turned off by Disable Effects."),
                new String[] {Tr.t("Off"), "+", Tr.t("Dot")}, settings.crosshair(), true,
                which -> settings.setCrosshair(which));
        final int[] crosshairSizes = {50, 75, 100, 125, 150, 200};
        final String[] crosshairLabels = new String[crosshairSizes.length];
        int crosshairSelected = 2;
        for (int i = 0; i < crosshairSizes.length; ++i) {
            crosshairLabels[i] = crosshairSizes[i] + "%";
            if (crosshairSizes[i] == settings.crosshairSize()) crosshairSelected = i;
        }
        sliderRow(Tr.t("Crosshair Size"), crosshairLabels, crosshairSelected, settings.crosshair() != 0,
                which -> settings.setCrosshairSize(crosshairSizes[which]));
        choiceRow(Tr.t("Camera Distance"), Tr.t("A wider view behind the player, as if the camera stood farther back. "
                        + "Cutscenes, the scope, menus and Separate Ways keep the normal view."),
                new String[] {Tr.t("Default"), "1.5x", "2x"}, settings.cameraDistance(), true,
                which -> settings.setCameraDistance(which));
        toggleRow(Tr.t("Speedrun Mode"), Tr.t("A timer in the top right: the game's own play time (IGT), real "
                        + "time since New Game (RTA) and the time of each chapter, also saved to speedrun.csv. "
                        + "Cheats and Stage Select are off while it is on."),
                settings.speedrunMode(), on -> settings.setSpeedrunMode(on));

        group(Tr.t("Minimap"));
        minimapRows();

        group(Tr.t("Performance Overlay"));
        heldToggle("show_fps", Tr.t("Performance Overlay"), Tr.t("A see-through bar at the top of the game with FPS and phone stats."),
                settings.showFps(), on -> settings.setShowFps(on));
        choiceRow(Tr.t("Overlay Style"), settings.showFps()
                        ? Tr.t("Bar: one see-through bar at the top centre. Minimal: no bar, the FPS large in the "
                                + "top left with the other items under it.")
                        : Tr.t("Turn Performance Overlay on to choose."),
                new String[] {Tr.t("Bar"), Tr.t("Minimal")}, settings.overlayStyle(), settings.showFps(),
                which -> settings.setOverlayStyle(which));
        final String[] items = {Tr.t("FPS"), Tr.t("CPU %"), Tr.t("GPU %"), Tr.t("Battery"), Tr.t("Temperature °C"), Tr.t("Details (ms, draws, RAM)"),
                Tr.t("Frametime Graph")};
        final int mask = settings.overlayItems();
        final boolean[] lit = new boolean[items.length];
        for (int i = 0; i < 6; ++i) lit[i] = (mask & (1 << i)) != 0;
        lit[6] = settings.frameGraph();
        multiRow(Tr.t("Overlay Items"), settings.showFps()
                        ? Tr.t("Tap to show or hide each one. FPS counts the frames the game really draws a second. "
                                + "CPU is how busy the game's thread is: near 100% means the phone's CPU is the limit.")
                        : Tr.t("Turn Performance Overlay on to choose."),
                items, lit, settings.showFps(), which -> {
                    if (which == 6) settings.setFrameGraph(!settings.frameGraph());
                    else settings.setOverlayItems(settings.overlayItems() ^ (1 << which));
                });
    }

    /**
     * Render Resolution, shared with the launcher's quick settings: fixed steps of the Wii's 480 lines
     * (AppSettings.RESOLUTION_SCALES, as in the Basara port), 540p among them. A value saved by the old
     * percentage slider shows as the nearest step.
     */
    void resolutionRow() {
        final float[] scales = AppSettings.RESOLUTION_SCALES;
        final String[] labels = new String[scales.length];
        final float scale = settings.resolutionScale();
        int selected = 0;
        for (int i = 0; i < scales.length; ++i) {
            labels[i] = AppSettings.resolutionLabel(scales[i]);
            if (Math.abs(scales[i] - scale) < Math.abs(scales[selected] - scale)) selected = i;
        }
        sliderRow(Tr.t("Render Resolution"), labels, selected, true,
                which -> settings.setResolutionScale(scales[which]));
    }

    /** Minimap: the room's outline with Leon, Ashley and the enemies, over the game. */
    private void minimapRows() {
        final boolean on = settings.minimap();
        toggleRow(Tr.t("Minimap"), Tr.t("A small map of the room in a corner: its walls, Leon's arrow and the "
                        + "characters around him. Hidden in cutscenes and menus."),
                on, value -> {
                    settings.setMinimap(value);
                    changed();
                });
        final String[] markers = {Tr.t("Enemies"), Tr.t("Ashley / Luis"), Tr.t("Animals"), Tr.t("Items")};
        final int bits = settings.minimapMarkers();
        final boolean[] lit = new boolean[markers.length];
        for (int i = 0; i < markers.length; ++i) lit[i] = (bits & (1 << i)) != 0;
        multiRow(Tr.t("Minimap Shows"), Tr.t("Tap to show or hide each kind. Enemies are dark red, bright red with a "
                        + "pulsing ring once they have spotted Leon, turning orange as they are hurt; bosses purple; "
                        + "Ashley yellow, Luis blue; animals grey. Items are diamonds: green healing, amber ammo, "
                        + "blue the rest."),
                markers, lit, on, which -> settings.setMinimapMarkers(settings.minimapMarkers() ^ (1 << which)));
        choiceRow(Tr.t("Minimap Turns"), Tr.t("With the camera: up is where the camera looks. North up: fixed."),
                new String[] {Tr.t("With the camera"), Tr.t("North up")}, settings.minimapOrientation(), on,
                which -> settings.setMinimapOrientation(which));
        choiceRow(Tr.t("Minimap Corner"), Tr.t("Where on the screen it sits."),
                new String[] {Tr.t("Top left"), Tr.t("Top right"), Tr.t("Bottom left"), Tr.t("Bottom right")},
                settings.minimapCorner(), on, which -> settings.setMinimapCorner(which));
        choiceRow(Tr.t("Minimap Zoom"), Tr.t("How much of the room it shows."),
                new String[] {Tr.t("Near"), Tr.t("Medium"), Tr.t("Far")}, settings.minimapZoom(), on,
                which -> settings.setMinimapZoom(which));
        final int[] sizes = {15, 20, 25, 30, 35, 40, 45, 50, 55, 60};
        final String[] sizeLabels = new String[sizes.length];
        int sizeAt = 3;
        for (int i = 0; i < sizes.length; ++i) {
            sizeLabels[i] = sizes[i] + "%";
            if (sizes[i] == settings.minimapSize()) sizeAt = i;
        }
        sliderRow(Tr.t("Minimap Size"), sizeLabels, sizeAt, on, which -> settings.setMinimapSize(sizes[which]));
        final int[] opacities = {20, 30, 40, 50, 60, 70, 80, 90, 100};
        final String[] opacityLabels = new String[opacities.length];
        int opacityAt = 6;
        for (int i = 0; i < opacities.length; ++i) {
            opacityLabels[i] = opacities[i] + "%";
            if (opacities[i] == settings.minimapOpacity()) opacityAt = i;
        }
        sliderRow(Tr.t("Minimap Opacity"), opacityLabels, opacityAt, on,
                which -> settings.setMinimapOpacity(opacities[which]));
    }

    /** Brightness: the game's own brightness filter, -50..+50 in steps of 10 (0 = the game's default, centred). */
    private void brightnessRow() {
        final int[] levels = {-50, -40, -30, -20, -10, 0, 10, 20, 30, 40, 50};
        final String[] labels = new String[levels.length];
        int selected = 5;
        for (int i = 0; i < levels.length; ++i) {
            labels[i] = levels[i] > 0 ? "+" + levels[i] : String.valueOf(levels[i]);
            if (levels[i] == settings.brightness()) selected = i;
        }
        sliderRow(Tr.t("Brightness"), labels, selected, true, which -> settings.setBrightness(levels[which]));
    }

    /** Not held by the Low-End preset: the player may change it. */
    private boolean free(String key) {
        return !settings.lowEndHolds(key);
    }

    /** `about`, plus why it cannot be changed while the Low-End preset holds it. */
    private String held(String key, String about) {
        return free(key) ? about : about + "\n" + Tr.t("Set by the Preset: turn the preset off to change it.");
    }

    /** A toggle the Low-End preset may hold: then it shows its value, dimmed, and cannot be tapped. */
    private void heldToggle(String key, String title, String about, boolean on, Toggle toggle) {
        if (free(key)) toggleRow(title, about, on, toggle);
        else choiceRow(title, held(key, about), new String[] {Tr.t("Off"), Tr.t("On")}, on ? 1 : 0, false,
                which -> {});
    }

    /** The real screen size (the app's own metrics leave out the system bars). */
    static DisplayMetrics screenMetrics(Activity activity) {
        final DisplayMetrics m = new DisplayMetrics();
        activity.getWindowManager().getDefaultDisplay().getRealMetrics(m);
        return m;
    }

    /** Lines of the picture sent to the screen: Display Resolution when it is below the screen's. */
    static int outputLines(Activity activity, AppSettings settings) {
        final DisplayMetrics m = screenMetrics(activity);
        final int shortSide = Math.min(m.widthPixels, m.heightPixels);
        final int lines = settings.displayLines();
        return lines > 0 && lines < shortSide * 0.95f ? lines : shortSide;
    }



    static final String[] UPSCALERS = {"Off", "FSR Light", "FSR Full", "SGSR"};

    /** Display Resolution: the size of the picture the game sends to the screen (GameActivity). */
    private void displayResolutionRow() {
        final int[] lines = AppSettings.DISPLAY_LINES;
        final String[] labels = new String[lines.length];
        int selected = 0;
        for (int i = 0; i < lines.length; ++i) {
            labels[i] = lines[i] == 0 ? Tr.t("Screen") : lines[i] + "p";
            if (lines[i] == settings.displayLines()) selected = i;
        }
        final DisplayMetrics m = screenMetrics(activity);
        final int shortSide = Math.min(m.widthPixels, m.heightPixels);
        restart(null);
        choiceRow(Tr.t("Display Resolution"), held("display_lines", Tr.t("The picture the game sends to the screen, which stretches it "
                        + "for free. Lower takes GPU work off every frame, menus too: steadier fps on a slow "
                        + "phone, blurrier picture. This screen: ") + Math.max(m.widthPixels, m.heightPixels)
                        + "×" + shortSide + Tr.t(" (a size at or above it changes nothing).")),
                labels, selected, free("display_lines"), which -> settings.setDisplayLines(lines[which]));
    }

    /** Off / FSR Light / FSR Full / SGSR, shared with the launcher's quick settings. */
    void upscalerRow() {
        final String[] labels = new String[UPSCALERS.length];
        for (int i = 0; i < labels.length; ++i) labels[i] = Tr.t(UPSCALERS[i]);
        choiceRow(Tr.t("Upscaler"), held("fsr_on", Tr.t("Keeps a picture rendered below the screen's resolution sharp when it is "
                        + "stretched. FSR Light sharpens it and costs almost nothing. FSR Full adds FSR1's edge-aware "
                        + "upscale: sharpest, but several ms on a small phone GPU. SGSR: Qualcomm's edge-aware "
                        + "upscale in one pass, lighter than FSR Full.")),
                labels, settings.fsr(), free("fsr_on"), which -> settings.setFsr(which));
    }

    private void showGraphics() {
        upscalerRow();
        choiceRow(Tr.t("Texture Detail"), held("texture_filter", Tr.t("Keeps the ground, walls and characters sharp at low Render "
                        + "Resolution and when seen at an angle. Sharpest costs a little more on a small phone GPU.")),
                new String[] {Tr.t("Original"), Tr.t("Sharp"), Tr.t("Sharpest")}, settings.textureFilter(),
                free("texture_filter"),
                which -> settings.setTextureFilter(which));
        restart(null);
        choiceRow(Tr.t("Graphics Compatibility"), Tr.t("Fixes for phone GPUs whose drivers break the 3D models "
                        + "(scrambled textures, stretched characters). Auto uses them where needed."),
                new String[] {Tr.t("Auto"), Tr.t("On"), Tr.t("Off")}, settings.gpuCompat(), true,
                which -> settings.setGpuCompat(which));
        restart(null);
        // Only the renderers that run on the GPU driver chosen below (GpuDrivers.rendererRuns): with a
        // driver the player installed, the game works on that driver alone.
        final java.util.List<Integer> renderers = new java.util.ArrayList<>();
        final java.util.List<String> rendererNames = new java.util.ArrayList<>();
        int rendererIndex = 0;
        for (int which : AppSettings.RENDERER_ORDER) {
            if (!GpuDrivers.rendererRuns(activity, settings, which)) continue;
            if (which == GpuDrivers.effectiveRenderer(activity, settings)) rendererIndex = renderers.size();
            renderers.add(which);
            rendererNames.add(Tr.t(AppSettings.rendererName(which)));
        }
        final GpuDrivers.Driver activeDriver = GpuDrivers.find(activity, settings.gpuDriver());
        choiceRow(Tr.t("Renderer"), Tr.t("Auto picks one for this phone (the Vulkan renderer on most). Vulkan: the game talks to the "
                        + "Vulkan driver directly. WebGPU: the WebGPU (Dawn) renderer, for phones whose Vulkan "
                        + "renderer shows broken graphics. OpenGL ES (Experimental): WebGPU on OpenGL ES, for phones "
                        + "whose Vulkan driver shows a black screen or closes the game.")
                        + (activeDriver == null ? "" : " " + Tr.t("With an installed GPU driver, only the renderers "
                        + "that run on it are listed (never OpenGL ES: the driver is Vulkan only).")),
                rendererNames.toArray(new String[0]), rendererIndex,
                true, index -> {
                    final int which = renderers.get(index);
                    final int before = settings.renderer();
                    settings.setRenderer(which);
                    confirmRestart(Tr.t("Renderer"), AppSettings.rendererName(which), AppSettings.rendererName(before),
                            () -> settings.setRenderer(before));
                }, false);
        showGpuDrivers();
    }

    /** System's report tools (launcher only): share a run's log, the device info, reset and clear. */
    void reportRows(boolean gameRunning) {
        toggleRow(Tr.t("Recording-Friendly Audio"), Tr.t("For screen recordings: some phones play the game's sound "
                        + "on a fast path that recordings miss, so the video comes out silent. On keeps the sound on "
                        + "the regular path, with a little more delay. Applies when the game starts again."),
                settings.recordingAudio(), on -> settings.setRecordingAudio(on));
        final boolean files = AppFiles.hasFileAccess(activity);
        actionRow(Tr.t("Share Log"), files
                        ? Tr.t("Sends the log of the game runs you pick (the one that crashed first) with the device "
                                + "info, as one zip: to Discord, Telegram, WhatsApp or anywhere else.")
                        : Tr.t("Needs \"All files access\" for this app, to read the logs."),
                files ? Tr.t("Share") : null, () -> LogShare.show(activity));
        actionRow(Tr.t("Share Shader Cache"), Tr.t("Sends the list of shaders this phone has used, as one zip. "
                        + "Testers: send it to the developer so a later version can prepare them ahead and stutter less."),
                Tr.t("Share"), () -> LogShare.shareShaderCache(activity));
        actionRow(Tr.t("Device Info"), LogShare.deviceInfo(activity).trim(), Tr.t("Copy"),
                () -> LogShare.copyDeviceInfo(activity));
        actionRow(Tr.t("Reset and Clear"), Tr.t("Reset all settings, clear the HD texture and shader caches, "
                        + "the testing options or the logs, in one place."),
                Tr.t("Choose"), () -> DevTools.showReset(activity, gameRunning, () -> show(SYSTEM)));
    }

    /** Everything about the game's textures: packs, dump, the key report and HD Textures. */
    private void showTextures() {
        showCustomTextures();
        group(Tr.t("Mod Tools"));
        showTextureTools();
        group(Tr.t("HD Textures"));
        showHdTextures();
    }

    /**
     * The HD cache size, counted (or the cache deleted) off the UI thread: it holds thousands of files on
     * shared storage, and walking them on every redraw made the section stutter. -1: not counted yet.
     */
    private static long hdCacheBytes = -1;
    private static boolean hdBusy;

    private void hdCacheWork(java.io.File dir, boolean clear) {
        if (hdBusy) return;
        hdBusy = true;
        new Thread(() -> {
            if (clear) AppFiles.deleteRecursively(dir);
            final long bytes = clear ? 0 : AppFiles.folderSize(dir);
            activity.runOnUiThread(() -> {
                hdBusy = false;
                hdCacheBytes = bytes;
                if (section == TEXTURES && !activity.isFinishing()) show(section);
            });
        }, "hd-cache-size").start();
    }

    /** HD Textures: upscaled while playing, with their storage and cache. */
    private void showHdTextures() {
        restart(null);
        choiceRow(Tr.t("HD Textures"), held("hd_textures", Tr.t("Makes the game's textures twice as sharp: each one is upscaled in the "
                        + "background the first time it appears (blurry, then sharp) and kept for next time. Uses "
                        + "more RAM and storage.")
                        + (weakPhone() ? Tr.t(" This phone has little RAM or renders below 1x: the gain is small.") : "")),
                new String[] {Tr.t("Off"), Tr.t("Live")}, settings.hdTextures(), free("hd_textures"), which -> {
                    final int before = settings.hdTextures();
                    settings.setHdTextures(which);
                    confirmRestart(Tr.t("HD Textures"), which == 1 ? Tr.t("Live") : "off", before == 1 ? Tr.t("Live") : "off",
                            () -> settings.setHdTextures(before));
                }, false);
        final int[] sizes = AppSettings.HD_CACHE_SIZES;
        int size = 1;
        for (int i = 0; i < sizes.length; ++i) if (sizes[i] == settings.hdCacheMb()) size = i;
        restart(null);
        choiceRow(Tr.t("HD Texture Storage"), settings.hdTextures() != 0
                        ? Tr.t("The most storage the sharper textures may take; once full, no new ones are made.")
                        : Tr.t("Turn HD Textures on to choose."),
                new String[] {Tr.t("512 MB"), Tr.t("1 GB"), Tr.t("2 GB")}, size, settings.hdTextures() != 0,
                which -> settings.setHdCacheMb(sizes[which]));
        final java.io.File hdCache = AppFiles.hdCacheDir();
        if (hdCache != null && hdCacheBytes < 0) hdCacheWork(hdCache, false);
        final long used = Math.max(0, hdCacheBytes);
        valueRow(Tr.t("HD Texture Cache"), hdCache == null ? "–"
                        : hdBusy || hdCacheBytes < 0 ? "…"
                        : String.format(java.util.Locale.ROOT, "%.0f MB", used / 1048576.0),
                used > 0 && !hdBusy && !inGame ? Tr.t("Clear") : null, this::confirmClearHdCache);
    }

    /** What the game loaded per mod (Textures/Loaded.txt), read each time the Textures tab is shown. */
    private java.util.Map<String, TexturePacks.Used> used = new java.util.HashMap<>();

    /** Texture mods from the Textures folder: on or off, what was found, then each mod ticked and put in order. */
    private void showCustomTextures() {
        final String folder = folderName("Textures");
        final boolean files = AppFiles.hasFileAccess(activity);
        restart(null);
        toggleRow(Tr.t("Custom Textures"), null, settings.customTextures(), files, on -> settings.setCustomTextures(on));
        if (!files) {
            note(Tr.t("Needs \"All files access\" for this app, to read ") + folder + ".", Ui.WARNING);
            return;
        }
        final java.io.File root = AppFiles.textureDir();
        final java.util.List<TexturePacks.Pack> packs = TexturePacks.load(settings, root);
        final boolean enabled = settings.customTextures();
        used = TexturePacks.used(root);
        TexturePacks.countMissing(activity, root, packs, () -> {
            if (section == TEXTURES) show(section);
        });
        final java.util.Map<String, Integer> overridden = TexturePacks.overridden(packs);
        showTextureStatus(packs, overridden, enabled, folder);

        group(Tr.t("Mods") + (inGame ? "  ⟳ Restart" : ""));
        if (packs.isEmpty()) {
            note(Tr.t("No mods yet. Put each mod in its own folder in ") + folder + Tr.t(" (how: README.txt there)."),
                    Ui.SECONDARY);
        } else {
            note(Tr.t("Tap a mod to tick it, i for what was found in it. The higher mod wins when two change the "
                    + "same texture."), Ui.MUTED);
        }
        for (int i = 0; i < packs.size(); ++i) packRow(packs, i, enabled, overridden);
        if (packs.size() > 1) {
            final LinearLayout bulk = new LinearLayout(activity);
            for (int on = 1; on >= 0; --on) {
                final boolean tick = on == 1;
                final TextView chip = Ui.chip(activity, (tick ? Tr.t("Tick all") : Tr.t("Untick all"))
                        .toUpperCase(java.util.Locale.ROOT), false, enabled, v -> {
                    for (TexturePacks.Pack p : packs) p.on = tick;
                    TexturePacks.save(settings, packs);
                    changed();
                });
                final LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(0, dp(28), 1f);
                if (!tick) params.leftMargin = dp(4);
                bulk.addView(chip, params);
            }
            rows.addView(bulk, Ui.matchWidth(dp(4)));
        }
        valueRow(Tr.t("Mods Folder"), folder, Tr.t("Rescan"), () -> TexturePacks.recount());
    }

    /** A wrapped line of text between rows (cards do not show their descriptions in this menu). */
    private void note(String text, int color) {
        final TextView view = Ui.text(activity, text, 12, color);
        view.setPadding(0, dp(2), 0, dp(4));
        rows.addView(view, Ui.matchWidth(0));
    }

    /** One line of what the ticked mods add up to, then each problem found, in the warning colour. */
    private void showTextureStatus(java.util.List<TexturePacks.Pack> packs, java.util.Map<String, Integer> overridden,
                                   boolean enabled, String folder) {
        int on = 0, textures = 0, conflicts = 0, empty = 0, notSeen = 0, unreadable = 0, loaded = 0, failed = 0;
        String failedSample = null;
        long bytes = 0;
        boolean scanned = true;
        for (TexturePacks.Pack p : packs) {
            final TexturePacks.Info i = TexturePacks.info(p.name);
            if (i == null) {
                scanned = false;
                continue;
            }
            if (!p.on) continue;
            ++on;
            textures += i.valid - i.duplicates;
            bytes += i.bytes;
            if (i.valid == 0) ++empty;
            if (i.seen == 0 && i.valid > 0) ++notSeen;
            unreadable += i.unreadable;
            final TexturePacks.Used u = used.get(p.name);
            if (u != null) {
                loaded += u.ok;
                failed += u.failed;
                if (failedSample == null) failedSample = u.failedSample;
            }
            final Integer n = overridden.get(p.name);
            if (n != null) conflicts += n;
        }
        final String summary;
        if (!scanned) summary = Tr.t("Checking the mods…");
        else if (!enabled) summary = Tr.t("Off: the game uses its own textures.");
        else if (on == 0) summary = Tr.t("No mod ticked: the game uses its own textures.");
        else summary = String.format(java.util.Locale.ROOT, Tr.t("%d mods on, %d textures, %s"), on,
                    textures - conflicts, megabytes(bytes));
        note(summary, Ui.SECONDARY);
        if (!scanned) return;
        if (enabled && on > 0) {
            if (loaded > 0) {
                note(String.format(java.util.Locale.ROOT, inGame ? Tr.t("✓ In game now: %d textures replaced so far.")
                        : Tr.t("✓ Last time in game: %d textures replaced."), loaded), Ui.SUCCESS);
            } else {
                note(inGame ? Tr.t("In game now: no texture replaced yet (they load when the game shows them).")
                        : Tr.t("Not used in game yet: start the game, and the mods' textures load as they show."),
                        Ui.MUTED);
            }
        }
        for (String zip : TexturePacks.archives()) {
            note("⚠ " + zip + Tr.t(" is still packed: unpack it into its own folder in ") + folder
                    + Tr.t(", then Rescan."), Ui.WARNING);
        }
        if (!enabled) return;
        if (failed > 0) note(String.format(java.util.Locale.ROOT, Tr.t("⚠ %d textures could not be read by the game, "
                + "for example %s: tap i on its mod."), failed, failedSample), Ui.WARNING);
        if (unreadable > 0) note(String.format(java.util.Locale.ROOT, Tr.t("⚠ %d files are neither PNG nor DDS inside "
                + "(often a JPEG renamed): the game skips them."), unreadable), Ui.WARNING);
        if (empty > 0) note(Tr.t("⚠ A ticked mod has no texture the game can use: tap i on it."), Ui.WARNING);
        if (notSeen > 0) note(Tr.t("⚠ A ticked mod matches none of the textures in Dump: it may be for another "
                + "game or version (this one is RB4E08)."), Ui.WARNING);
        if (conflicts > 0) note(String.format(java.util.Locale.ROOT, Tr.t("%d textures are in more than one ticked "
                + "mod: the higher mod's are used."), conflicts), Ui.MUTED);
        final long ram = DeviceProfile.ramMb(activity);
        if (ram > 0 && bytes / 1048576 > ram / 4) {
            note(Tr.t("⚠ The ticked mods are large for this phone's RAM: loading can stutter, or the system can "
                    + "close the game. Untick some, or use a smaller pack."), Ui.WARNING);
        }
    }

    private static String megabytes(long bytes) {
        return bytes >= 1024L * 1048576
                ? String.format(java.util.Locale.ROOT, "%.1f GB", bytes / 1073741824.0)
                : String.format(java.util.Locale.ROOT, "%.0f MB", Math.ceil(bytes / 1048576.0));
    }

    /** One mod: its tick, name, what was found, i for details, and arrows to move it up or down the order. */
    private void packRow(java.util.List<TexturePacks.Pack> packs, int index, boolean enabled,
                         java.util.Map<String, Integer> overridden) {
        final TexturePacks.Pack pack = packs.get(index);
        final LinearLayout row = new LinearLayout(activity);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(0, dp(3), 0, dp(3));
        row.addView(Ui.toggle(activity, pack.on && enabled));
        final boolean loose = pack.name.equals(TexturePacks.LOOSE);
        final TextView name = Ui.text(activity, loose ? Tr.t("(Loose files)") : pack.name, 13, Ui.TEXT);
        name.setSingleLine(true);
        name.setEllipsize(android.text.TextUtils.TruncateAt.MIDDLE);
        final LinearLayout.LayoutParams nameParams = new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        nameParams.leftMargin = dp(8);
        row.addView(name, nameParams);
        final TexturePacks.Info info = TexturePacks.info(pack.name);
        final Integer lost = overridden.get(pack.name);
        final String status;
        int color = Ui.SECONDARY;
        if (info == null) {
            status = "…";
        } else if (info.valid == 0) {
            status = info.unreadable > 0 ? Tr.t("⚠ unreadable files") : Tr.t("⚠ no valid names");
            color = Ui.WARNING;
        } else if (pack.on && enabled && used.containsKey(pack.name) && used.get(pack.name).failed > 0) {
            status = String.format(java.util.Locale.ROOT, Tr.t("⚠ %d failed"), used.get(pack.name).failed);
            color = Ui.WARNING;
        } else if (pack.on && enabled && used.containsKey(pack.name) && used.get(pack.name).ok > 0) {
            status = String.format(java.util.Locale.ROOT, Tr.t("✓ %d in use"), used.get(pack.name).ok);
            color = Ui.SUCCESS;
        } else if (pack.on && enabled && info.seen == 0) {
            status = Tr.t("⚠ not this game?");
            color = Ui.WARNING;
        } else if (pack.on && enabled && lost != null && lost > 0) {
            status = String.format(java.util.Locale.ROOT, Tr.t("%d files, %d covered"), info.valid, lost);
        } else {
            status = String.format(java.util.Locale.ROOT, Tr.t("%d files"), info.valid);
        }
        final TextView files = Ui.text(activity, status, 12, color);
        files.setSingleLine(true);
        row.addView(files);
        final TextView about = Ui.chip(activity, "i", false, info != null, v -> showPackInfo(pack, lost));
        about.setPadding(0, 0, 0, 0);
        final LinearLayout.LayoutParams aboutParams = new LinearLayout.LayoutParams(dp(30), dp(28));
        aboutParams.leftMargin = dp(8);
        row.addView(about, aboutParams);
        for (int dir = -1; dir <= 1; dir += 2) {
            final int to = index + dir;
            final boolean can = enabled && to >= 0 && to < packs.size();
            final TextView arrow = Ui.chip(activity, dir < 0 ? "▲" : "▼", false, can, v -> {
                java.util.Collections.swap(packs, index, to);
                TexturePacks.save(settings, packs);
                changed();
            });
            arrow.setPadding(0, 0, 0, 0);
            final LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(dp(30), dp(28));
            params.leftMargin = dp(2);
            row.addView(arrow, params);
        }
        if (enabled) {
            row.setClickable(true);
            row.setOnClickListener(v -> {
                pack.on = !pack.on;
                TexturePacks.save(settings, packs);
                changed();
            });
        } else {
            name.setAlpha(0.4f);
            files.setAlpha(0.4f);
        }
        rows.addView(row, Ui.matchWidth(0));
    }

    /** What the scan found in one mod, and what each problem means. */
    private void showPackInfo(TexturePacks.Pack pack, Integer lost) {
        final TexturePacks.Info i = TexturePacks.info(pack.name);
        if (i == null) return;
        final boolean loose = pack.name.equals(TexturePacks.LOOSE);
        final Sheet sheet = new Sheet(activity, loose ? Tr.t("(Loose files)") : pack.name, null);
        sheet.paragraph(String.format(java.util.Locale.ROOT, Tr.t("%d textures the game can use (%s), %d of them .dds."),
                i.valid, megabytes(i.bytes), i.dds));
        final TexturePacks.Used u = used.get(pack.name);
        if (u != null && u.ok > 0) {
            sheet.paragraph(String.format(java.util.Locale.ROOT, inGame ? Tr.t("%d of them are in use in the game now.")
                    : Tr.t("%d of them were used the last time in game."), u.ok));
        } else if (pack.on && settings.customTextures()) {
            sheet.paragraph(Tr.t("None used in game yet: a texture loads when the game shows it."));
        } else {
            sheet.paragraph(Tr.t("Not ticked, so the game does not use it."));
        }
        if (u != null && u.failed > 0) {
            sheet.warning(String.format(java.util.Locale.ROOT, Tr.t("%d could not be read by the game"), u.failed),
                    Tr.t("The file is damaged or not a real PNG or DDS. For example:") + " " + u.failedSample);
        }
        if (i.unreadable > 0) {
            sheet.warning(String.format(java.util.Locale.ROOT, Tr.t("%d files are neither PNG nor DDS inside"),
                    i.unreadable), Tr.t("Often a JPEG renamed to .png or .dds. Save it again as PNG. For example:")
                    + " " + i.unreadableSample);
        }
        if (i.renamed > 0) {
            sheet.note(String.format(java.util.Locale.ROOT, Tr.t("%d .dds files are PNG images inside (renamed): "
                    + "they are read as PNG, which is fine."), i.renamed));
        }
        if (i.seen >= 0) {
            sheet.paragraph(String.format(java.util.Locale.ROOT, Tr.t("%d of them were seen in Dump (textures the game "
                    + "has shown on this phone)."), i.seen));
        }
        if (lost != null && lost > 0) {
            sheet.paragraph(String.format(java.util.Locale.ROOT, Tr.t("%d are also in a mod above this one, so "
                    + "that mod's are used. Move this mod up to use its own."), lost));
        }
        if (i.duplicates > 0) {
            sheet.paragraph(String.format(java.util.Locale.ROOT, Tr.t("%d textures are in this mod twice (a .png and "
                    + "a .dds, or two folders): only one of each is used."), i.duplicates));
        }
        if (i.bad > 0) {
            sheet.warning(String.format(java.util.Locale.ROOT, Tr.t("%d images with names the game cannot match"),
                    i.bad), Tr.t("Names must look like tex1_64x64_1a2b3c4d5e6f7a8b_5.png (Dolphin style, made by "
                    + "Dump Textures). For example:") + " " + i.badSample);
        }
        if (i.valid > 0 && i.seen == 0) {
            sheet.warning(Tr.t("Not seen in this game"), Tr.t("None of its names are in Dump. Maybe it is made for "
                    + "another game or version (this one is Wii NTSC-U RB4E08), or Dump only has a few textures yet."));
        }
        if (i.dds > 0) {
            sheet.note(Tr.t("On a GPU without BC textures (most Mali) the .dds files are unpacked while loading: "
                    + "they work, with more loading time."));
        }
        sheet.action(Tr.t("Close"), null, true, false, null).show();
    }

    /** Tools for pack makers and testers: texture dump and the texture key report. */
    private void showTextureTools() {
        final String folder = folderName("Textures");
        restart(null);
        toggleRow(Tr.t("Dump Textures"), settings.customTextures()
                        ? Tr.t("Saves each texture the game uses to ") + folder + Tr.t("/dump, named the way its "
                                + "replacement must be: edit one and put it in ") + folder + Tr.t(". Slows loading and fills "
                                + "storage: turn it off when done.")
                        : Tr.t("Turn Custom Textures on to choose."),
                settings.dumpTextures(), settings.customTextures(), on -> settings.setDumpTextures(on));
        restart(null);
        toggleRow(Tr.t("Report Texture Keys"), Tr.t("Lists the name of every texture the game uses in the log (only "
                        + "the names, no pictures), so testers can check that texture packs match the game."),
                settings.textureKeyReport(), on -> settings.setTextureKeyReport(on));
    }

    /** Player-installed Vulkan drivers (GpuDrivers): choose one, install a .zip, remove one. */
    private void showGpuDrivers() {
        if (!GpuDrivers.supported()) return;
        final java.util.List<GpuDrivers.Driver> drivers = GpuDrivers.installed(activity);
        final String[] options = new String[drivers.size() + 1];
        options[0] = Tr.t("System");
        int selected = 0;
        for (int i = 0; i < drivers.size(); ++i) {
            options[i + 1] = drivers.get(i).label();
            if (drivers.get(i).id.equals(settings.gpuDriver())) selected = i + 1;
        }
        final String before = settings.gpuDriver();
        final String beforeName = options[selected];
        restart(null);
        choiceRow(Tr.t("GPU Driver"), Tr.t("System is the phone's own driver. A driver you install (a Turnip .zip "
                        + "made for Dolphin or Yuzu, for Snapdragon / Adreno) can be faster or fix broken graphics. "
                        + "Mali drivers (malisx2) run on the Vulkan renderer, which starts by itself for them. "
                        + "If it fails or the game closes while starting, it goes back to System by itself."),
                options, selected, true, which -> {
                    settings.setGpuDriver(which == 0 ? "" : drivers.get(which - 1).id);
                    confirmRestart(Tr.t("GPU Driver"), options[which], beforeName,
                            () -> settings.setGpuDriver(before));
                }, false);
        // 1.2 has no launcher settings: installing and removing live here too, used from the next start.
        actionRow(Tr.t("Install GPU Driver"), Tr.t("Choose a driver .zip (it has a meta.json inside), for example "
                        + "from Download."),
                Tr.t("Install"), this::installGpuDriver);
        for (GpuDrivers.Driver driver : drivers) {
            final String about = (driver.description.isEmpty() ? "" : driver.description + " ")
                    + (driver.author.isEmpty() ? "" : Tr.t("By ") + driver.author + ".");
            actionRow(driver.label(), about.trim(), Tr.t("Remove"), () -> {
                if (driver.id.equals(settings.gpuDriver())) settings.setGpuDriver("");
                GpuDrivers.remove(activity, driver);
            });
        }
    }

    private void installGpuDriver() {
        if (!AppFiles.hasFileAccess(activity)) {
            askFileAccess(() -> android.widget.Toast.makeText(activity, Tr.t("Allow file access to choose a "
                    + "driver .zip."), android.widget.Toast.LENGTH_LONG).show());
            return;
        }
        FileBrowser.showFiles(activity, FileBrowser.downloads(), Tr.t("Choose a GPU driver"),
                Tr.t("A driver .zip with a meta.json inside (Turnip for Adreno)."),
                f -> f.getName().toLowerCase(java.util.Locale.ROOT).endsWith(".zip"),
                Tr.t("No folders or .zip files here."), file -> {
                    try {
                        final GpuDrivers.Driver driver = GpuDrivers.install(activity, file);
                        android.widget.Toast.makeText(activity, Tr.t("Installed ") + driver.label()
                                + Tr.t(". Choose it under GPU Driver."), android.widget.Toast.LENGTH_LONG).show();
                    } catch (java.io.IOException e) {
                        new Sheet(activity, Tr.t("Not a driver"), e.getMessage())
                                .action(Tr.t("OK"), null, true, false, null).show();
                    }
                    changed();
                });
    }

    private void showPerformance() {
        // Every phone; a low or mid-low one has it on from its first start.
        group(Tr.t("Preset"));
        lowEndRow();

        group(Tr.t("Frame Rate"));
        restart(null);
        final String[] sixtyNames = {Tr.t("Off"), Tr.t("Lite"), Tr.t("Adaptive"), Tr.t("Full")};
        choiceRow(Tr.t("60 FPS"), Tr.t("The game keeps its own 30 fps and the phone draws an extra picture "
                        + "between two of its frames, so motion is smoother and the game plays exactly the same. "
                        + "Lite redraws only what moves (characters, doors) and moves the rest with the camera: "
                        + "much lighter on the GPU. Full redraws everything: about twice the GPU work, always 60. "
                        + "Adaptive is Full that drops back to a steady 30 where the phone cannot keep up. "
                        + "Smoke, fire and the menus still move at 30."),
                sixtyNames, settings.sixtyFpsMode(), true, which -> {
                    final int before = settings.sixtyFpsMode();
                    settings.setSixtyFpsMode(which);
                    confirmRestart(Tr.t("60 FPS"), sixtyNames[which], sixtyNames[before],
                            () -> settings.setSixtyFpsMode(before));
                }, false);
        choiceRow(Tr.t("Frameskip"), Tr.t("Leaves out drawing some frames so a slow GPU keeps the game at full "
                        + "speed. The game itself still runs every frame. Auto: only after a frame that ran late. "
                        + "Always: the same as Auto while 60 FPS is off. With 60 FPS Lite "
                        + "it leaves out only inserted pictures: Auto when the phone falls behind, Always every "
                        + "other one (45 fps). Off with 60 FPS Full and Adaptive."),
                new String[] {Tr.t("Off"), Tr.t("Auto"), Tr.t("Always")}, settings.frameskip(), true,
                which -> settings.setFrameskip(which));
        heldToggle("vsync", Tr.t("V-Sync"), Tr.t("Frames go out in step with the display: smoother motion."), settings.vsync(),
                on -> settings.setVSync(on));
        toggleRow(Tr.t("Even Frame Pacing"), Tr.t("With V-Sync, a picture that is ready early waits one refresh so every "
                        + "picture stays on screen for the same time: smoother camera, but controls can feel a little "
                        + "slower."),
                settings.pictureHold(), on -> settings.setPictureHold(on));

        group(Tr.t("Resolution and Heat"));
        heldToggle("dynamic_resolution", Tr.t("Dynamic Resolution"), Tr.t("Lowers the render resolution while the GPU holds the game back "
                        + "(never below 480 lines) and raises it again when there is room. Render Resolution stays the "
                        + "highest it goes."),
                settings.dynamicResolution(), on -> settings.setDynamicResolution(on));
        final boolean thermal = AppSettings.thermalSupported(activity);
        toggleRow(Tr.t("Thermal Control"), thermal
                        ? Tr.t("When the phone warms up, lowers the resolution if the GPU is the one working "
                        + "hard. Back to normal once it cools down.")
                        : Tr.t("Not available: needs Android 12 or newer with a thermal sensor report."),
                settings.thermalControl(), thermal, on -> settings.setThermalControl(on));
        final boolean sustained = AppSettings.sustainedSupported(activity);
        toggleRow(Tr.t("Sustained Performance"), sustained
                        ? Tr.t("Keeps the CPU and GPU at speeds the phone can hold for hours: a little slower at the "
                        + "start, but no sudden drop once it gets hot in a long session. The game no longer asks for "
                        + "the fastest cores.")
                        : Tr.t("Not available: this phone does not offer it."),
                settings.sustainedPerformance(), sustained, on -> settings.setSustainedPerformance(on));

        group(Tr.t("Effects"));
        toggleRow(Tr.t("Disable Fog"), Tr.t("Removes the distance haze and the drifting fog, dust and rain, the heaviest "
                        + "blending in foggy places such as the village road. Faraway scenery stays clear."),
                settings.disableFog(), on -> settings.setDisableFog(on));
        heldToggle("reduce_effects", Tr.t("Disable Effects"), Tr.t("Turns off every effect: sparks, blood, smoke, fire, fog and "
                        + "muzzle flashes. Menus and puzzle rooms keep theirs, as some puzzles are made of effects."),
                settings.reduceEffects(), on -> settings.setReduceEffects(on));
        heldToggle("reduce_screen_filters", Tr.t("Disable Screen Filters"), Tr.t("Turns off the full-screen filters: blur, depth of field, glows "
                        + "and the dust, snow and rain layer. Each copies the whole frame, which is heavy on a phone "
                        + "GPU. The scopes, thermal vision and fades stay, as the game needs them."),
                settings.reduceScreenFilters(), on -> settings.setReduceScreenFilters(on));
        heldToggle("reduce_shadows", Tr.t("Disable Shadows"), Tr.t("Turns off the shadows cast by lamps and torches, which draw "
                        + "the characters a second time. The small shadows under the feet stay."),
                settings.reduceShadows(), on -> settings.setReduceShadows(on));
        choiceRow(Tr.t("Draw Distance"), held("draw_distance", Tr.t("How far away the game still draws trees, houses and enemies. "
                        + "Shorter skips the far ones before they are drawn, the biggest saving in open areas such "
                        + "as the village (Short: about half the draws). Far things then appear closer to you.")),
                new String[] {Tr.t("Full"), Tr.t("Medium"), Tr.t("Short")}, settings.drawDistance(), free("draw_distance"),
                which -> settings.setDrawDistance(which));

        group(Tr.t("System"));
        restart(null);
        toggleRow(Tr.t("GX Thread"), Tr.t("Runs the graphics work on its own CPU core, so busy scenes run faster on "
                        + "slower phones."),
                settings.gxThread(), true, on -> {
                    settings.setGxThread(on);
                    confirmRestart(Tr.t("GX Thread"), on ? "on" : "off", on ? "off" : "on",
                            () -> settings.setGxThread(!on));
                }, false);
        restart(null);
        choiceRow(Tr.t("Upload Memory"), Tr.t("Vulkan renderer: the memory the game writes its graphics data into. "
                        + "Auto measures at start which one is faster on this phone. Standard suits most phones, "
                        + "Cached helps some slow ones."),
                new String[] {Tr.t("Auto"), Tr.t("Standard"), Tr.t("Cached")}, settings.uploadMemory(), true,
                which -> settings.setUploadMemory(which));
        restart(null);
        toggleRow(Tr.t("Skip Boot"), Tr.t("Skips the Wii Remote strap screen at start."),
                settings.skipBoot(), on -> settings.setSkipBoot(on));
    }

    /**
     * The Low-End preset on any phone (DeviceProfile.lowEndValues); a low or mid-low phone has it on
     * from its first start (DeviceProfile.applyOnce). Off puts back the settings from before it.
     */
    private void lowEndRow() {
        final String gpu = settings.gpuName();
        final String render = AppSettings.resolutionLabel((Float) DeviceProfile.lowEndValues(gpu, 1).get("resolution_scale"));
        final String ultraRender = AppSettings.resolutionLabel((Float) DeviceProfile.lowEndValues(gpu, 2).get("resolution_scale"));
        final String[] options = {Tr.t("Off"), Tr.t("Low-End"), Tr.t("Ultra Low")};
        choiceRow(Tr.t("Preset"), Tr.t("The lightest settings in one tap, for a phone that cannot hold "
                        + "30 fps: steadier, blurrier.") + "\n• " + Tr.t("Display Resolution") + " 480p\n• "
                        + Tr.t("Render Resolution") + " " + render + "\n• " + Tr.t("Texture Detail") + " "
                        + Tr.t("Original") + "\n• " + Tr.t("Upscaler") + " " + Tr.t("Off") + "\n• "
                        + Tr.t("Dynamic Resolution") + " "
                        + Tr.t("On") + "\n• " + Tr.t("HD Textures") + " " + Tr.t("Off") + "\n• "
                        + Tr.t("Disable Screen Filters") + " " + Tr.t("On") + "\n• " + Tr.t("Disable Shadows") + " "
                        + Tr.t("On") + "\n• " + Tr.t("Draw Distance") + " " + Tr.t("Medium") + "\n"
                        + Tr.t("Ultra Low: the same, with Display Resolution 360p, Render Resolution ") + ultraRender
                        + Tr.t(", for a phone that drops in the heaviest effects.") + " "
                        + Tr.t("It also turns on Disable Effects and sets Draw Distance to Short.") + "\n"
                        + Tr.t("Off puts back the settings from "
                        + "before it. Applies when the game starts again."),
                options, settings.lowEndLevel(), true, which -> {
                    if (which == settings.lowEndLevel()) return;
                    settings.undoLowEnd();  // a switch between levels keeps the backup from before either
                    if (which > 0) settings.applyLowEnd(DeviceProfile.lowEndValues(gpu, which), which);
                    changed();
                });
    }

    private void showSaveData() {
        toggleRow(Tr.t("Auto Save"), Tr.t("Saves to slot 20 each time a room starts. Never while Play As is on."),
                settings.autoSave(), on -> settings.setAutoSave(on));
        final boolean has = SaveData.hasSave(activity);
        valueRow(Tr.t("Back Up Save"), has
                        ? android.text.format.DateFormat.getMediumDateFormat(activity)
                                        .format(new java.util.Date(SaveData.lastSaved(activity)))
                                + " " + android.text.format.DateFormat.getTimeFormat(activity)
                                        .format(new java.util.Date(SaveData.lastSaved(activity)))
                        : Tr.t("No save yet"),
                Tr.t("Back Up"), has, this::backUp);
        actionRow(Tr.t("Import Save"), Tr.t("Replaces this phone's save with a .zip made by Back Up. A running "
                        + "game is closed without saving."),
                Tr.t("Import"), this::importSave);
        final java.io.File last = SaveData.latestBackup(activity);
        valueRow(Tr.t("Undo Last Import"), last == null ? Tr.t("Nothing to undo")
                        : android.text.format.DateFormat.getMediumDateFormat(activity)
                                        .format(new java.util.Date(last.lastModified()))
                                + " " + android.text.format.DateFormat.getTimeFormat(activity)
                                        .format(new java.util.Date(last.lastModified())),
                Tr.t("Undo"), last != null, this::confirmUndoImport);
    }

    /**
     * A save import from the game: the game's process must end before its save is replaced, so the
     * start screen does it (LauncherActivity ACTION_IMPORT_SAVE) and the game opens again afterwards.
     */
    private void importFromGame(java.io.File file) {
        ((GameActivity) activity).toLauncher(LauncherActivity.ACTION_IMPORT_SAVE, file == null ? null : file.getPath());
    }

    private void backUp() {
        if (!AppFiles.hasFileAccess(activity)) {
            askFileAccess(() -> SaveData.startExport(activity));
            return;
        }
        final java.io.File saves = SaveData.savesDir();
        FileBrowser.showFolder(activity, saves != null ? saves : FileBrowser.downloads(),
                Tr.t("Where to save the backup"),
                Tr.t("Tap Save Here to keep it in ") + AppFiles.HOME + Tr.t("/Saves, or open another folder first."),
                folder -> SaveData.exportToFolder(activity, folder, this::changed));
    }

    private void importSave() {
        if (!AppFiles.hasFileAccess(activity)) {
            askFileAccess(() -> SaveData.startImport(activity));
            return;
        }
        FileBrowser.showFiles(activity, SaveData.importStart(), Tr.t("Choose your save backup"),
                Tr.t("The re4-save-….zip made by Back Up, or a data.bin copied from a Wii or Dolphin. "
                        + "The save on this phone is replaced (and kept for Undo Last Import)."),
                SaveData::isSaveFile, Tr.t("No folders, .zip or .bin files here."),
                this::importFromGame);
    }

    private void confirmUndoImport() {
        new Sheet(activity, Tr.t("Undo the last import?"), Tr.t("The save from before that import comes back. A "
                + "running game is closed without saving."))
                .action(Tr.t("Undo"), null, true, false, () -> importFromGame(null))
                .action(Tr.t("Cancel"), null, false, false, null)
                .show();
    }

    /** Without "All files access" the folders cannot be listed: allow it, or use the phone's own picker. */
    private void askFileAccess(Runnable systemPicker) {
        new Sheet(activity, Tr.t("Allow file access"), Tr.t("To list the folders on this phone."))
                .step("1", Tr.t("Open Settings"), AppFiles.fileAccessStep())
                .step("2", Tr.t("Come back"), Tr.t("Then tap the button again."))
                .action(Tr.t("Open Settings"), null, true, false, () -> {
                    if (android.os.Build.VERSION.SDK_INT < 30) {
                        AppFiles.requestLegacyFileAccess(activity);
                        return;
                    }
                    try {
                        activity.startActivity(new android.content.Intent(
                                android.provider.Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                android.net.Uri.parse("package:" + activity.getPackageName())));
                    } catch (android.content.ActivityNotFoundException e) {
                        systemPicker.run();
                    }
                })
                .action(Tr.t("Use System Picker"), Tr.t("The phone's own file picker, without the permission"), false,
                        false, systemPicker)
                .action(Tr.t("Cancel"), null, false, false, null)
                .show();
    }

    /** Only opened in a build with cheats (BuildConfig.CHEATS); applied at once in the game. */
    private void showCheats() {
        final TextView warning = Ui.text(activity, Tr.t("Cheats change your save: back it up first (Save Data). Off in LAN play."),
                12, Sheet.WARNING_TEXT);
        warning.setPadding(0, dp(2), 0, dp(4));
        rows.addView(warning, Ui.matchWidth(0));
        if (settings.speedrunMode()) {
            final TextView speedrun = Ui.text(activity, Tr.t("Speedrun Mode is on (Display): cheats stay off."),
                    12, Sheet.WARNING_TEXT);
            speedrun.setPadding(0, 0, 0, dp(4));
            rows.addView(speedrun, Ui.matchWidth(0));
        }

        group(Tr.t("Unlocks"));
        toggleRow(Tr.t("Unlock All"), Tr.t("Opens the extras: costumes, Professional, Separate Ways, Assignment Ada "
                        + "and the Mercenaries characters. What the game saves stays open after you turn this off."),
                settings.cheatUnlockAll(), on -> settings.setCheatUnlockAll(on));
        toggleRow(Tr.t("Special Weapons"), Tr.t("The merchant sells the P.R.L. 412, Chicago Typewriter, Handcannon, "
                        + "Infinite Launcher and Matilda from the first playthrough, at the game's prices."),
                settings.cheatSpecialWeapons(), on -> settings.setCheatSpecialWeapons(on));
        toggleRow(Tr.t("Max Money"), Tr.t("Your pesetas stay at 99,999,999."),
                settings.cheatMaxMoney(), on -> settings.setCheatMaxMoney(on));

        group(Tr.t("Player"));
        final String[] who = {"Leon", "Ada", "HUNK", "Krauser", "Wesker"};
        final int[] whoType = {0, 2, 3, 4, 5};
        int whoAt = 0;
        for (int i = 0; i < whoType.length; ++i) {
            if (whoType[i] == settings.cheatPlayAs()) whoAt = i;
        }
        choiceRow(Tr.t("Play As (main story)"), Tr.t("Another character plays Leon's story from the next room on, "
                        + "with Leon's case, life and money, plus its own weapons where the case has room. A New Game "
                        + "starts with its Mercenaries set. Experimental: some events may look wrong. Leon is off."),
                who, whoAt, true, which -> settings.setCheatPlayAs(whoType[which]));
        toggleRow(Tr.t("Infinite Health"), Tr.t("Leon's health (and Ashley's) is refilled as soon as it drops. "
                        + "Instant deaths, such as a chainsaw or a fall, still end the game."),
                settings.cheatInfiniteHealth(), on -> settings.setCheatInfiniteHealth(on));
        toggleRow(Tr.t("One-Shot Kills"), Tr.t("Any hit that hurts an enemy kills it, bosses too."),
                settings.cheatOneShotKills(), on -> settings.setCheatOneShotKills(on));
        toggleRow(Tr.t("Steady Aim"), Tr.t("The aiming pointer no longer shakes."),
                settings.cheatSteadyAim(), on -> settings.setCheatSteadyAim(on));

        group(Tr.t("Weapons and Items"));
        toggleRow(Tr.t("Infinite Ammo"), Tr.t("The readied weapon's magazine stays full, even with no spare ammo "
                        + "in the case, and grenades are not used up."),
                settings.cheatInfiniteAmmo(), on -> settings.setCheatInfiniteAmmo(on));
        toggleRow(Tr.t("Item Stack 31,999"), Tr.t("Every item in the case stacks up to 31,999: ammo, herbs, sprays "
                        + "and grenades. Pick up or combine two of the same item to stack them; two green herbs stack "
                        + "instead of mixing. Stacks stay in your save after you turn this off."),
                settings.cheatItemStack(), on -> settings.setCheatItemStack(on));
        toggleRow(Tr.t("Largest Attache Case"), Tr.t("Your case becomes the XL one at the next room. It stays "
                        + "in your save after you turn this off."),
                settings.cheatBiggestCase(), on -> settings.setCheatBiggestCase(on));

        group(Tr.t("Assist"));
        toggleRow(Tr.t("Auto Aim (not with scopes)"), Tr.t("When you aim, the sight locks on the body of the nearest "
                        + "enemy and stays on it while you aim, also when it falls or is stunned; when it dies the "
                        + "sight moves on to the next one. Holding the aim stick for half a second lets go until the next aim. "
                        + "Rifle scopes aim by hand."),
                settings.cheatAutoAim(), on -> settings.setCheatAutoAim(on));
        toggleRow(Tr.t("Auto QTE"), Tr.t("Button prompts in cutscenes and fights press themselves: dodges "
                        + "(A+B, L+R) and button mashing."),
                settings.cheatAutoQte(), on -> settings.setCheatAutoQte(on));
    }

    private void showControls() {
        group(Tr.t("Gamepad"));
        actionRow(Tr.t("Gamepad Buttons"), GamepadButtons.summary(settings), Tr.t("Change"),
                () -> new GamepadButtons(activity, settings, this::changed).show());
        actionRow(Tr.t("Check Gamepad"), Tr.t("See which gamepads are connected and test their buttons."),
                Tr.t("Open"), () -> GamepadCheck.show(activity));
        toggleRow(Tr.t("Alternate Controls"), Tr.t("LT / L2 aims, RT / R2 fires, LB / L1 readies the knife, RB / R1 "
                        + "opens the map, and the right stick aims while a weapon is ready. Only with the default "
                        + "Gamepad Buttons."),
                settings.altControls(), on -> settings.setAltControls(on));
        toggleRow(Tr.t("Swap A/B and X/Y"), Tr.t("On: the face buttons go by their labels (Xbox A is A). "
                        + "Off: by position, like a Wii Classic Controller (A on the right)."),
                settings.swapFace(), on -> settings.setSwapFace(on));
        toggleRow(Tr.t("Swap Sticks"), Tr.t("The right stick moves Leon, the left stick turns the camera."),
                settings.swapSticks(), on -> settings.setSwapSticks(on));
        toggleRow(Tr.t("Invert Camera Y"), Tr.t("Pushing the camera stick up looks down."),
                settings.invertRightY(), on -> settings.setInvertRightY(on));
        sliderRow(Tr.t("Right Stick Axes"), new String[] {Tr.t("Auto"), "Z / RZ", "RX / RY"},
                settings.rightStickAxes(), true, which -> settings.setRightStickAxes(which));
        final int[] deadZones = {0, 5, 10, 15, 20, 25, 30};
        final String[] deadLabels = {"0%", "5%", "10%", "15%", "20%", "25%", "30%"};
        sliderRow(Tr.t("Stick Dead Zone"), deadLabels, indexOf(deadZones, settings.stickDeadZone()), true,
                which -> settings.setStickDeadZone(deadZones[which]));
        final int[] ranges = {50, 60, 70, 80, 90, 100};
        final String[] rangeLabels = {"50%", "60%", "70%", "80%", "90%", "100%"};
        sliderRow(Tr.t("Stick Range"), rangeLabels, indexOf(ranges, settings.stickRange()), true,
                which -> settings.setStickRange(ranges[which]));

        group(Tr.t("Mouse and Keyboard"));
        toggleRow(Tr.t("Mouse Aim"), Tr.t("Click the game to capture the mouse. Right button aims (R), left button "
                        + "fires (A), middle button readies the knife. While aiming the mouse moves the sight, "
                        + "otherwise it turns Leon."),
                settings.mouseAim(), on -> settings.setMouseAim(on));
        sensitivityRow(Tr.t("Mouse Sensitivity"), settings.mouseSensitivity(), settings.mouseAim(),
                s -> settings.setMouseSensitivity(s));
        actionRow(Tr.t("Keyboard Keys"), KeyboardKeys.summary(settings), Tr.t("Change"),
                () -> new KeyboardKeys(activity, settings, this::changed).show());
        // The F10 bar (runtime/src/settings_overlay.cpp): Controller settings, Keyboard and mouse on a port.
        // Players without an F10 key, or who never heard of it, open it from here (in game only).
        actionRow(Tr.t("Controller Settings (F10)"), null,
                inGame && openControllerBar != null ? Tr.t("Open") : null, () -> {
                    if (openControllerBar != null) openControllerBar.run();
                });
        note(inGame ? Tr.t("Opens the F10 bar over the game: Controller settings, Keyboard and mouse per port. "
                        + "F10 or Escape closes it.")
                : Tr.t("In game, press F10 on the keyboard (or open it here from the pause menu's settings) for "
                        + "Controller settings."), Ui.SECONDARY);

        group(Tr.t("Touch Controls"));
        toggleRow(Tr.t("Touch Controls"), Tr.t("The on-screen Classic Controller."), settings.touchControls(),
                on -> settings.setTouchControls(on));
        choiceRow(Tr.t("Button Style"), Tr.t("Modern: clean monochrome icons. Classic: colored Wii letters. "
                        + "Item Liquid: glossy black liquid gamepad style with controller X/A/B/Y & analog sticks, "
                        + "curved ergonomic L/R triggers, and thin rounded rectangle utility buttons."),
                new String[] {Tr.t("Modern"), Tr.t("Classic"), Tr.t("Item Liquid")},
                settings.touchStyle(), settings.touchControls(),
                which -> settings.setTouchStyle(which));
        final String[] opacities = {"20%", "40%", "60%", "80%", "100%"};
        final int opacity = Math.max(0, Math.min(4, Math.round(settings.touchOpacity() * 5f) - 1));
        sliderRow(Tr.t("Touch Control Opacity"), opacities, opacity,
                settings.touchControls(), which -> settings.setTouchOpacity((which + 1) / 5f));
        choiceRow(Tr.t("Show Touch Controls"), Tr.t("Auto: hidden while a gamepad is used, a touch on the screen "
                        + "shows them again. Always: they stay on screen."),
                new String[] {Tr.t("Auto"), Tr.t("Always")}, settings.touchShow() == 0 ? 0 : 1, settings.touchControls(),
                which -> settings.setTouchShow(which));
        toggleRow(Tr.t("Tap for QTE"), Tr.t("When a button prompt flashes up (dodge, mash, shake off), a tap on "
                        + "the picture presses its buttons. Tap again and again to mash."),
                settings.tapQte(), on -> settings.setTapQte(on));
        toggleRow(Tr.t("Knife by Swipe"), Tr.t("A quick swipe on the picture readies the knife and slashes: "
                        + "swipe up to slash high, down to slash low."),
                settings.knifeSwipe(), on -> settings.setKnifeSwipe(on));
        actionRow(Tr.t("Touch Control Layout"), settings.touchLayout().isEmpty()
                        ? Tr.t("Move and resize the controls while playing: Back > Edit Controls Layout.")
                        : Tr.t("Arranged by you. Change it while playing: Back > Edit Controls Layout."),
                settings.touchLayout().isEmpty() ? null : Tr.t("Reset"), () -> settings.setTouchLayout(""));
        toggleRow(Tr.t("Fixed Left Stick"), Tr.t("On: the stick moves only when touched where it is drawn. "
                        + "Off: a thumb anywhere on the empty left side grabs it."),
                settings.fixedStick(), on -> settings.setFixedStick(on));
        toggleRow(Tr.t("Auto Run"), Tr.t("On: pushing the stick forward runs without holding B. Hold B to walk. "
                        + "Works with touch, gamepad and keyboard."),
                settings.autoRun(), on -> settings.setAutoRun(on));
        toggleRow(Tr.t("Toggle Aim"), Tr.t("On: press R once to aim and again to lower the weapon, no need to hold it. "
                        + "L (knife) works the same way. Menus, events and L / R prompts use the buttons as usual. "
                        + "Works with touch, gamepad and keyboard."),
                settings.toggleAim(), on -> settings.setToggleAim(on));
        toggleRow(Tr.t("Toggle Run"), Tr.t("On: press B once and Leon keeps running, press it again to walk. "
                        + "Menus, events and B prompts use B as usual. Works with touch, gamepad and keyboard."),
                settings.toggleRun(), on -> settings.setToggleRun(on));
        toggleRow(Tr.t("Vibration"), Tr.t("A short buzz when a touch button is pressed."), settings.haptics(),
                on -> settings.setHaptics(on));

        group(Tr.t("Camera"));
        toggleRow(Tr.t("Touch camera"), Tr.t("Drag on the empty right side of the screen to turn the camera. The right stick is hidden while this is on."),
                settings.touchCamera(), on -> settings.setTouchCamera(on));
        choiceRow(Tr.t("Camera Mode"), Tr.t("Locked: the camera stays behind Leon and he faces where it looks. "
                        + "Drag sideways to turn both, up or down to look up or down; the left stick walks. "
                        + "Default: the game's own camera, the drag works like the right stick and only looks "
                        + "aside, then swings back."),
                new String[] {Tr.t("Locked"), Tr.t("Default")}, settings.swipeCamera() ? 0 : 1, settings.touchCamera(),
                which -> settings.setSwipeCamera(which == 0));
        toggleRow(Tr.t("Invert Camera"), Tr.t("On: the right touch stick is reversed both ways. Off: the game's own directions. "
                        + "Touch camera always turns the way the finger is held."),
                settings.invertCamera(), on -> settings.setInvertCamera(on));
        sensitivityRow(Tr.t("Camera Sensitivity"), settings.cameraSensitivity(), true,
                s -> settings.setCameraSensitivity(s));
        sensitivityRow(Tr.t("Aim Sensitivity"), settings.aimSensitivity(), true,
                s -> settings.setAimSensitivity(s));

        group(Tr.t("Gyro Aim"));
        final boolean gyroOk = GyroAim.supported(activity);
        toggleRow(Tr.t("Gyro Aim"), gyroOk
                        ? Tr.t("While R (weapon) or L (knife) is held, turn the phone to move the laser sight.")
                        : Tr.t("Not available: this phone has no gyroscope."),
                settings.gyroAim(), gyroOk, on -> settings.setGyroAim(on));
        sensitivityRow(Tr.t("Gyro Sensitivity"), settings.gyroSensitivity(), gyroOk && settings.gyroAim(),
                s -> settings.setGyroSensitivity(s));
        toggleRow(Tr.t("Invert Gyro Up/Down"), Tr.t("On: tilting the top of the phone toward you aims up."),
                settings.gyroInvertY(), gyroOk && settings.gyroAim(), on -> settings.setGyroInvertY(on));
    }

    static final float[] SENSITIVITIES = {0.25f, 0.5f, 0.75f, 1f, 1.5f, 2f, 3f};

    interface Sensitivity { void set(float value); }

    /** A 25%..300% slider for a sensitivity (1 = 100%, the default). */
    private void sensitivityRow(String title, float value, boolean enabled, Sensitivity apply) {
        final String[] labels = new String[SENSITIVITIES.length];
        int selected = 3;
        for (int i = 0; i < labels.length; ++i) {
            labels[i] = Math.round(SENSITIVITIES[i] * 100) + "%";
            if (Math.abs(SENSITIVITIES[i] - value) < 0.01f) selected = i;
        }
        sliderRow(title, labels, selected, enabled, which -> apply.set(SENSITIVITIES[which]));
    }

    /** RAM under 4.5 GB, or a render scale below 1x: HD Textures costs a lot and shows little. */
    private boolean weakPhone() {
        final android.app.ActivityManager am = activity.getSystemService(android.app.ActivityManager.class);
        final android.app.ActivityManager.MemoryInfo info = new android.app.ActivityManager.MemoryInfo();
        if (am != null) am.getMemoryInfo(info);
        final float scale = settings.resolutionScale();
        return (info.totalMem > 0 && info.totalMem < (4500L << 20)) || (scale > 0f && scale < 1f);
    }

    private static String folderName(String sub) {
        return AppFiles.HOME + "/" + sub;
    }

    private void confirmClearHdCache() {
        new Sheet(activity, Tr.t("Clear HD Texture Cache?"), Tr.t("The sharper textures are made again while you play."))
                .action(Tr.t("Clear"), null, true, true, () -> {
                    final java.io.File dir = AppFiles.hdCacheDir();
                    if (dir != null) hdCacheWork(dir, true);
                    changed();
                })
                .action(Tr.t("Cancel"), null, false, false, null)
                .show();
    }

    /** "WxH" the game renders at for the chosen scale (0: the screen's own size). */
    static String renderSize(Activity activity, AppSettings settings) {
        final float scale = settings.resolutionScale();
        final DisplayMetrics m = screenMetrics(activity);
        final int screenLong = Math.max(m.widthPixels, m.heightPixels), screenShort = Math.min(m.widthPixels, m.heightPixels);
        final int h = outputLines(activity, settings);
        final int w = screenShort > 0 ? Math.round(screenLong * (float) h / screenShort) : screenLong;
        if (scale <= 0f) return w + "×" + h;
        final float aspect;
        switch (settings.aspectMode()) {
            case AppSettings.ASPECT_4_3: aspect = 4f / 3f; break;
            case AppSettings.ASPECT_16_9: aspect = 16f / 9f; break;
            default: aspect = h > 0 ? Math.max((float) w / h, 16f / 9f) : 16f / 9f; break;
        }
        final int lines = Math.round(480 * scale);
        return Math.round(lines * aspect) + "×" + lines;
    }

    // --- rows ------------------------------------------------------------------------------------
    // Dear ImGui rows, as hikki's DefJam menu: the name on the left in capitals, the control on the
    // right (a stepper, a checkbox, a slider or a button). The lines about each setting are not shown
    // in the game: the menu stays short and calm.

    interface Toggle { void set(boolean on); }
    interface Choice { void pick(int which); }

    /** The next row is only read at start: its name gets a "⟳ Restart" tag (restartName). */
    private boolean pendingRestart;

    /** Marks the next row as applying at the next start (`text` is not shown any more). */
    private void restart(String text) {
        pendingRestart = true;
    }

    /** A dim heading and a line between groups in one section (ImGui's SeparatorText). */
    private void group(String title) {
        final TextView caption = Ui.caption(activity, title);
        caption.setPadding(0, dp(10), 0, 0);
        rows.addView(caption);
        rows.addView(Ui.line(activity));
    }

    /**
     * The row's name. A setting read only at start (in the game) carries a "⟳ Restart" tag; once it
     * differs from the value the game started with, the row turns amber and the footer counts it
     * (pendingRestarts) until it is set back or the game restarts.
     */
    private View restartName(String title, boolean restartOnly, LinearLayout row) {
        final TextView name = Ui.text(activity, title.toUpperCase(java.util.Locale.ROOT), 13, Ui.TEXT);
        if (!restartOnly) return name;
        final LinearLayout box = new LinearLayout(activity);
        box.setOrientation(LinearLayout.VERTICAL);
        final LinearLayout line = new LinearLayout(activity);
        line.setGravity(Gravity.CENTER_VERTICAL);
        line.addView(name);
        final TextView tag = Ui.text(activity, "⟳ Restart", 10, Ui.PENDING);
        tag.setBackground(Ui.box(activity, 0, Ui.PENDING, 3));
        tag.setPadding(dp(4), 0, dp(4), 0);
        final LinearLayout.LayoutParams tagParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        tagParams.leftMargin = dp(6);
        line.addView(tag, tagParams);
        box.addView(line);
        if (rowValue != null) {
            final String before = START_VALUES.computeIfAbsent(title, k -> rowValue);
            if (!before.equals(rowValue)) {
                PENDING.put(title, before + " → " + rowValue);
                row.setBackgroundColor(Ui.PENDING_BG);
            } else {
                PENDING.remove(title);
            }
        }
        return box;
    }

    /** Values of the restart-only rows when this game started (the process lives as long as the game). */
    private static final java.util.Map<String, String> START_VALUES = new java.util.HashMap<>();
    private static final java.util.Map<String, String> PENDING = new java.util.LinkedHashMap<>();

    /** Restart-only settings changed since the game started: name → "before → now". */
    static java.util.Map<String, String> pendingRestarts() {
        return java.util.Collections.unmodifiableMap(PENDING);
    }

    /** The value the next card shows, for the restart tracking (null: not tracked). */
    private String rowValue;

    /** A row: the name, and `control` on the right (or under it, full width, when `underneath`). */
    LinearLayout card(String title, String about, View control, boolean underneath, boolean enabled) {
        final LinearLayout row = new LinearLayout(activity);
        row.setOrientation(underneath ? LinearLayout.VERTICAL : LinearLayout.HORIZONTAL);
        row.setGravity(underneath ? Gravity.START : Gravity.CENTER_VERTICAL);
        row.setPadding(0, dp(3), 0, dp(3));
        final View name = restartName(title, pendingRestart && inGame, row);
        pendingRestart = false;
        rowValue = null;
        if (underneath) {
            row.addView(name, Ui.matchWidth(0));
            if (control != null) row.addView(control, Ui.matchWidth(dp(4)));
        } else {
            row.addView(name, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
            if (control != null) {
                final android.view.ViewGroup.LayoutParams own = control.getLayoutParams();
                final boolean fixed = own != null && own.width > 0;  // the checkbox keeps its square
                final LinearLayout.LayoutParams params = fixed
                        ? new LinearLayout.LayoutParams(own.width, own.height)
                        : new LinearLayout.LayoutParams(0, dp(28), 1.1f);
                params.leftMargin = dp(10);
                row.addView(control, params);
            }
        }
        if (!enabled) row.setAlpha(0.4f);
        rows.addView(row, Ui.matchWidth(0));
        return row;
    }

    /** An Off / On choice, drawn like every other choice row (< ON >). */
    void toggleRow(String title, String about, boolean on, Toggle toggle) {
        toggleRow(title, about, on, true, toggle);
    }

    /** Disabled: dimmed, unticked, and does nothing when tapped (the phone cannot do it). */
    void toggleRow(String title, String about, boolean on, boolean enabled, Toggle toggle) {
        toggleRow(title, about, on, enabled, toggle, true);
    }

    private void toggleRow(String title, String about, boolean on, boolean enabled, Toggle toggle, boolean redraw) {
        choiceRow(title, about, new String[] {Tr.t("Off"), Tr.t("On")}, on && enabled ? 1 : 0, enabled,
                which -> toggle.set(which == 1), redraw);
    }

    /** ImGui's combo as DefJam draws it: the value in a frame, then < and > to step through the options. */
    void choiceRow(String title, String about, String[] options, int selected, boolean enabled, Choice choice) {
        choiceRow(title, about, options, selected, enabled, choice, true);
    }

    /** `redraw` false: `choice` saves and redraws by itself (it may ask first). */
    private void choiceRow(String title, String about, String[] options, int selected, boolean enabled,
                           Choice choice, boolean redraw) {
        final LinearLayout control = new LinearLayout(activity);
        final TextView value = Ui.text(activity, options[Math.max(0, Math.min(selected, options.length - 1))]
                .toUpperCase(java.util.Locale.ROOT), 13, Ui.TEXT);
        value.setGravity(Gravity.CENTER);
        value.setSingleLine(true);
        value.setEllipsize(android.text.TextUtils.TruncateAt.END);
        value.setPadding(dp(4), 0, dp(4), 0);
        value.setBackgroundColor(Ui.FRAME);
        control.addView(value, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.MATCH_PARENT, 1f));
        for (int dir = -1; dir <= 1; dir += 2) {
            final int step = dir;
            final TextView arrow = Ui.chip(activity, dir < 0 ? "<" : ">", false, enabled, v -> {
                final int next = ((selected + step) % options.length + options.length) % options.length;
                if (next == selected) return;
                choice.pick(next);
                if (redraw) changed();
            });
            arrow.setPadding(0, 0, 0, 0);
            final LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(dp(28),
                    LinearLayout.LayoutParams.MATCH_PARENT);
            params.leftMargin = dp(2);
            control.addView(arrow, params);
        }
        rowValue = options[Math.max(0, Math.min(selected, options.length - 1))];
        card(title, about, control, false, enabled);
    }

    /** ImGui's slider over the options: drag anywhere on it, saved when the finger lets go. */
    /** Where `value` is in `values`, or the nearest one below it (0 when none). */
    private static int indexOf(int[] values, int value) {
        int best = 0;
        for (int i = 0; i < values.length; ++i) {
            if (values[i] <= value) best = i;
        }
        return best;
    }

    private void sliderRow(String title, String[] options, int selected, boolean enabled, Choice choice) {
        final Ui.Slider slider = new Ui.Slider(activity, options.length - 1, selected);
        slider.setLabel(options[selected]);
        slider.setEnabled(enabled);
        slider.setListener(new Ui.Slider.Listener() {
            @Override
            public void changed(int step) {
                slider.setLabel(options[step]);
            }

            @Override
            public void released(int step) {
                if (step == selected) return;
                choice.pick(step);
                SettingsMenu.this.changed();
            }
        });
        rowValue = options[selected];
        card(title, null, slider, false, enabled);
    }

    /** Buttons under the name that each turn on or off by themselves; the lit ones are on. */
    private void multiRow(String title, String about, String[] options, boolean[] lit, boolean enabled,
                          Choice flip) {
        final Ui.FlowLayout chips = new Ui.FlowLayout(activity, dp(2));
        for (int i = 0; i < options.length; ++i) {
            final int which = i;
            chips.addView(Ui.chip(activity, (lit[i] ? "✓ " : "") + options[i], lit[i], enabled, v -> {
                flip.pick(which);
                changed();
            }));
        }
        final StringBuilder value = new StringBuilder();
        for (int i = 0; i < options.length; ++i) {
            if (lit[i]) value.append(value.length() > 0 ? ", " : "").append(options[i]);
        }
        rowValue = value.length() > 0 ? value.toString() : Tr.t("Off");
        card(title, about, chips, true, enabled);
    }

    /** A button filling the control's place (dimmed when `label` is null: nothing to do now). */
    void actionRow(String title, String about, String label, Runnable action) {
        valueRow(title, null, label, action);
    }

    /** `value` in a frame (the amount, the version, the cache size), then the button beside it. */
    void valueRow(String title, String value, String label, Runnable action) {
        valueRow(title, value, label, label != null, action);
    }

    /** `enabled` false: the button is shown dimmed with its name, and does nothing. */
    void valueRow(String title, String value, String label, boolean enabled, Runnable action) {
        final LinearLayout control = new LinearLayout(activity);
        if (value != null) {
            final TextView frame = Ui.text(activity, value, 13, Ui.TEXT);
            frame.setGravity(Gravity.CENTER);
            frame.setSingleLine(true);
            frame.setEllipsize(android.text.TextUtils.TruncateAt.END);
            frame.setPadding(dp(4), 0, dp(4), 0);
            frame.setBackgroundColor(Ui.FRAME);
            control.addView(frame, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.MATCH_PARENT, 1f));
        }
        final TextView button = Ui.chip(activity, label == null ? "–" : label.toUpperCase(java.util.Locale.ROOT),
                false, enabled && label != null, v -> {
                    action.run();
                    changed();
                });
        final LinearLayout.LayoutParams params = value == null
                ? new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.MATCH_PARENT, 1f)
                : new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
                        LinearLayout.LayoutParams.MATCH_PARENT);
        if (value != null) params.leftMargin = dp(2);
        control.addView(button, params);
        card(title, null, control, false, true);
    }

    private int dp(int value) {
        return Ui.dp(activity, value);
    }
}
