package com.shiinobu.re4;

import android.app.Activity;
import android.app.Dialog;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.WindowInsets;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/**
 * What changed in each build: the versions in a strip across the top, the
 * chosen version's changes below. Opens once after an install or update (on the newest
 * version) and from the launcher's What's New tile. The newest entry is first and describes the
 * build installed; add one for every release, listing only what that build changed.
 */
final class ReleaseNotes {
    private static final class Entry {
        final String version, title, date;
        final String[] added, fixed, known;
        Entry(String version, String title, String date, String[] added, String[] fixed, String[] known) {
            this.version = version;
            this.title = title;
            this.date = date;
            this.added = added;
            this.fixed = fixed;
            this.known = known;
        }
    }

    private static final Entry[] ALL = {
        new Entry("Final Beta", "Play As fixes, smoother room loads, herb stacks", "2026-11-18",
                new String[] {},
                new String[] {
                    "Play As: Ada, HUNK, Krauser and Wesker can tell Ashley to wait or follow (X), as Leon does. With Ashley following, Krauser's X gives the command instead of the arm attack.",
                    "Camera Distance no longer raises and widens the view through the binoculars and the rifle scope.",
                    "Play As Krauser: the bow's arrows are endless, as in The Mercenaries.",
                    "Play As Wesker: his handgun silencer now comes with a New Game and with a switch to Wesker during the story.",
                    "Play As: Ada and Krauser crouch behind cover with Leon's moves (Ada sank half into the floor), and Krauser jumps over walls the same way.",
                    "Item Stack: dragging a herb onto a stack of herbs mixes them as the Combine command does (the dropped herb vanished and no mixed herb appeared).",
                    "Short stutters after a room loads are gone (the game's light checks read video memory the port had not mapped, and each first read stalled the game)."},
                new String[] {
                    "Inventory items can still appear and disappear on some Mali phones with 60 FPS Full. Please say whether it also happens with 60 FPS Off.",
                    "On an Adreno 610 phone the GPU stopped (device lost) on a castle room change at Render Resolution 1.5×. Use 1× or lower there.",
                    "With 60 FPS on, smoke, fire, blood and the menus still move at 30.",
                    "Going back from the main menu to the title movie can freeze on some phones (seen on a moto g57). If it happens, please send the log."}),
        new Entry("Beta 14", "New cheats, Item Stack works, scope zoom, castle crash fix", "2026-11-17",
                new String[] {
                    "Cheats tab sorted into Unlocks, Player, Weapons and Items, and Assist.",
                    "New cheat Play As (main story, experimental): Ada, HUNK, Krauser or Wesker plays Leon's story from the next room on, with Leon's case.",
                    "Play As: story cutscenes and the radio calls show the character you play (its own body, head and hands; Krauser's arm glow too), moved by Leon's motions and voice.",
                    "Play As: every character can finish off stunned Ganados on every stage (Wesker's and HUNK's finishers were missing outside the Mercenaries).",
                    "Cheats > Play As > Replay Cutscene: watch the last cutscene again without closing the game.",
                    "New cheat One-Shot Kills: any hit that hurts an enemy kills it.",
                    "Play As: a New Game started with Play As on gives the character its own Mercenaries start set (weapons tuned and loaded; Wesker's Handgun with his silencer). Switching in the middle of the story keeps your case and adds that character's weapons with some ammo where there is room.",
                    "New cheat Largest Attache Case: your case becomes the XL one at the next room.",
                    "Cheats > Item Stack 31,999 now also stacks on pickup: a picked-up item joins a stack of the same item (two green herbs make one stack of 2).",
                    "Touch controls: while aiming with a rifle scope, D-pad up and down zoom in and out. The same now works with the binoculars (touch and gamepad).",
                    "Fill Screen: the black bars no longer stay after loading a save.",
                    "New cheat, Steady Aim: the aiming pointer no longer shakes.",
                    "Auto Aim locks on the enemy's body for the whole aim, also when it falls or is stunned, and moves on to the next enemy when it dies.",
                    "More shaders are ready from the first start (from testers' shared caches), so fewer stutters in the castle.",
                    "Slightly faster game code: whole-number to decimal conversions and character skinning take fewer steps, and the speed profile is recorded again on the current code.",
                    "New defaults: Cheats > Item Stack is on, Display Resolution is 480p. A value you already picked stays."},
                new String[] {
                    "The first Beta 14 build closed right after starting the game on every phone. Fixed in this build: install it over the old one.",
                    "Performance > Disable Effects hid the harpoon laser in the Del Lago fight. Effects now stay on while you ride the boat.",
                    "Cheats > Item Stack did nothing in Beta 13: it now works.",
                    "Fixed a crash on entering the castle (room 300) seen on an OPPO phone with an Adreno 610.",
                    "The game's own Options > Brightness works again. Display > Brightness only overrides it when set away from 0.",
                    "Rifle scopes could not zoom with touch controls.",
                    "Play As: Ada no longer crashes at the first radio call, and Ada, Krauser and Wesker take out the radio before it, as Leon does."},
                new String[] {
                    "Inventory items can still appear and disappear on some Mali phones with 60 FPS Full. Please say whether it also happens with 60 FPS Off.",
                    "On an Adreno 610 phone the GPU stopped (device lost) on a castle room change at Render Resolution 1.5×. Use 1× or lower there.",
                    "With 60 FPS on, smoke, fire, blood and the menus still move at 30.",
                    "Going back from the main menu to the title movie can freeze on some phones (seen on a moto g57). If it happens, please send the log."}),
        new Entry("Beta 13", "Infinite grenades, Auto Aim without the scope", "2026-11-15",
                new String[] {
                    "Cheats > Item Stack 31,999: every item in the case stacks up to 31,999 (ammo, herbs, sprays, grenades). Two of the same item stack when combined, so two green herbs stack instead of mixing.",
                    "Performance: the Reduce options are now Disable Effects, Disable Screen Filters and Disable Shadows, and each turns its whole category off. Disable Fog also removes the drifting fog sheets (the ones that turned black).",
                    "Performance > Draw Distance (Full, Medium, Short): the game stops drawing far trees, houses and enemies sooner. Short is faster but things pop in.",
                    "Low-End and Ultra Low presets redone for RE4, and they now lock the settings they set (Render Resolution and the overlay stay free).",
                    "Display Resolution: new 540p. Render Resolution is back to fixed steps (Match screen, 0.5×, 0.75×, 1×, 540p, 1.5×, 2×, 3×, 4×) and applies at once; Display Resolution applies after Restart.",
                    "Touch camera: new Camera Mode (Controls > Camera). Locked, the default, keeps the camera behind Leon: a sideways drag turns him and the camera together, even standing still, and up or down looks up or down; the left stick still walks. Default is the game's own camera, the old drag that only looks aside.",
                    "Controls are sorted into Gamepad, Touch Controls, Camera and Gyro Aim.",
                    "Display > Crosshair (Off, + or Dot) and Crosshair Size: a sight where the laser hits while aiming, shown even when the laser line is hard to see.",
                    "Skip Boot is now on by default (Performance): the game starts past the strap screen and the title intro.",
                    "Cheats > Auto Aim no longer turns the rifle scope: scopes aim by hand again. The option's name says so."},
                new String[] {
                    "Cheats > Infinite Ammo now also keeps grenades (hand, incendiary, flash). Before, they ran out from the second throw on.",
                    "Cheats > Infinite Ammo now also works for a weapon with its stock attached (TMP, Red9): before, it lost rounds.",
                    "Frameskip Always with 60 FPS off no longer halves the picture rate to 15 fps: it now skips only frames that ran late, like Auto. With 60 FPS Lite it still leaves out every other inserted picture.",
                    "60 FPS Full: items in the inventory no longer blink (menus are shown without inserted pictures), and far scenery no longer leaves a light jagged band along the horizon while the camera turns.",
                    "Fixed a crash after a room change seen on an OPPO phone with a PowerVR GPU (a file closed twice).",
                    "The Load screen no longer stretches across the whole screen as it fades out after a load.",
                    "Touch controls: while aiming, the left stick moves the sight too (it did nothing before).",
                    "Disable Effects (also turned on by Ultra Low) no longer hides the laser sight.",
                    "Frameskip Auto at 30 fps no longer stutters: it leaves a picture out only when the GPU is what holds the game back, not when the game's own work runs late."},
                new String[] {
                    "Going back from the main menu to the title movie can freeze on some phones (seen on a moto g57). If it happens, please send the log, and say whether it still happens with GX Thread off.",
                    "With 60 FPS on, smoke, fire, blood and the menus still move at 30.",
                    "With 60 FPS Full, enemies can judder when the phone falls behind. Use 60 FPS Lite or Off if you see it.",
                    "On some PowerVR phones (Samsung A04) the inventory background shows only in the top-right quarter.",
                    "Some Mali phones can freeze while screen recording or streaming.",
                    "Phones that lower their CPU clock when warm (many Xiaomi/Redmi) can still slow down in the village after a while."}),
        new Entry("Beta 12", "Crash fix 12 s after start, less CPU work", "2026-11-15",
                new String[] {
                    "The game does less CPU work per frame in number conversions and floating-point math, so slow and warm phones keep a higher frame rate."},
                new String[] {
                    "On some phones (seen on Infinix with Android 14 and 16) the game closed about 12 seconds after starting. A bug in a diagnostic log line caused it. Fixed.",
                    "If the game freezes, the log now also records what the graphics thread was doing, to find the freeze when going back from the main menu to the title movie."},
                new String[] {
                    "Going back from the main menu to the title movie can freeze on some phones (seen on a moto g57). If it happens, please send the log, and say whether it still happens with GX Thread off.",
                    "With 60 FPS on, smoke, fire, blood and the menus still move at 30.",
                    "On some PowerVR phones (Samsung A04) the inventory background shows only in the top-right quarter.",
                    "Some Mali phones can freeze while screen recording or streaming.",
                    "Phones that lower their CPU clock when warm (many Xiaomi/Redmi) can still slow down in the village after a while."}),
        new Entry("Beta 11", "Faster shader loading, Frameskip with 60 FPS Lite, steadier village", "2026-11-15",
                new String[] {
                    "Shader loading is much faster: about 4 s instead of 45 s on a Snapdragon 730, because identical shaders are now made once and shared. Later starts show \"Loading shaders\" while they load.",
                    "Frameskip now also works with 60 FPS Lite: it leaves out only the inserted pictures (Auto: when the phone falls behind; Always: every other one, 45 fps). It stays off with 60 FPS Full."},
                new String[] {
                    "Phones with two fast cores and slower others (Snapdragon 730/732G, e.g. Redmi Note 10 Pro) dropped to 10-15 FPS in the village: the picture was prepared on the slow cores. It now runs on the fast cores next to the game, so 30 and 60 FPS hold much steadier there.",
                    "The touch buttons were redrawn in full every frame while a stick moved, taking CPU and GPU time from the game. Each button is now drawn once and reused.",
                    "With 60 FPS on, a crash could happen while running and turning the camera fast in the village (the GPU stopped). Fixed.",
                    "The game needs less CPU work per frame (fewer hand-overs to the graphics thread) and the download is smaller again (about 31 MB)."},
                new String[] {
                    "With 60 FPS on, smoke, fire, blood and the menus still move at 30.",
                    "On some PowerVR phones (Samsung A04) the inventory background shows only in the top-right quarter.",
                    "Some Mali phones can freeze while screen recording or streaming.",
                    "Phones that lower their CPU clock when warm (many Xiaomi/Redmi) can still slow down in the village after a while."}),
        new Entry("Beta 10", "60 FPS Lite, shaders ready at start, Frameskip, smaller download", "2026-11-15",
                new String[] {
                    "Settings > Performance > 60 FPS is now Off, Lite or Full. Lite redraws only what moves (Leon, Ashley, enemies, doors) and moves the rest of the picture with the camera: much lighter on the GPU than Full, for phones that could not hold 60 before. Full is the 60 FPS from Beta 8. Those who had 60 FPS on get Full.",
                    "Every switch in the settings is now an On / Off choice like the others, and Display and Performance are sorted into groups (Screen, Gameplay, Minimap, Overlay; Preset, Frame Rate, Resolution and Heat, Effects, System).",
                    "The first start after installing, updating or loading the game file waits on a loading screen until the shaders are ready, so the game no longer stutters while they compile in the first minutes. Later starts go straight in.",
                    "Cheats > Auto QTE: the button prompts in cutscenes and fights press themselves, dodges (A+B, L+R) and button mashing alike.",
                    "Settings > Performance > Frameskip: Off, Auto (skips a picture only when the phone falls behind) or Always (draws every other picture). For slow phones; the game itself keeps its full speed. It is off while 60 FPS is on. It sits right under Preset.",
                    "Skip Boot is now off by default: tick it in Settings > Performance to skip the strap screen.",
                    "The APK is less than half the size (about 48 MB instead of 115 MB). The first start after installing unpacks the game for a few seconds (\"Preparing the game...\"); later starts are as quick as before.",
                    "Cheats > Auto Aim also works with the rifle scope: the sight starts on the nearest enemy's head.",
                    "Share Logs also sends very large logs (the start and the end, where a crash is written) instead of leaving them out."},
                new String[] {
                    "After restarting the game, the touch buttons sometimes did nothing for up to three minutes (until the shaders were rebuilt in the background). They now work from the strap screen on.",
                    "Leaving the Load menu a second time (back to the title) gave a black screen: the game's sound shutdown waited forever.",
                    "Separate Ways still had black bars in some places (the village) with Fill Screen on.",
                    "Aiming with the touch controls went up when dragging down (and down when dragging up). The sight now follows the finger again.",
                    "Separate Ways went black and crashed with Camera Distance on: its scenes were too heavy for the GPU with the wider view. Camera Distance now keeps the normal view in Separate Ways.",
                    "More shaders are prepared ahead, now also from Mali phones, so fewer stutters the first time an area loads."},
                new String[] {
                    "With 60 FPS on, smoke, fire, blood and the menus still move at 30.",
                    "On some PowerVR phones (Samsung A04) the inventory background shows only in the top-right quarter.",
                    "Some Mali phones can freeze while screen recording or streaming."}),
        new Entry("Beta 9", "Shader cache, laser color, camera distance, speedrun timer", "2026-11-13",
                new String[] {
                    "Settings > Display > Laser Color: red (the game's), blue, yellow or green for the laser sight and its dot.",
                    "Settings > Display > Camera Distance: Default, 1.5x or 2x, a wider view behind Leon as if the camera stood farther back. Cutscenes, the scope and menus keep the normal view.",
                    "Settings > Display > Speedrun Mode (test): a timer in the top right with the game's play time (IGT), real time since New Game (RTA) and each chapter's time, saved to speedrun.csv and sent with Share Logs. Cheats and Stage Select are off while it is on.",
                    "Cheats > Auto Aim: aiming starts on the nearest enemy and follows it for a moment (the game's own lock-on option).",
                    "With 60 FPS on, the in-between picture also smooths moving textures (water, lava), fades and the light from torches and lamps, which used to step at 30.",
                    "Settings > System > Share Shader Cache: sends the list of shaders your phone has used. Testers, please send it: a later version can prepare them ahead and stutter less the first time an area loads.",
                    "Shaders are prepared ahead from the testers' shader caches, and the prepared shaders are kept: the first start after installing takes longer, later starts are much quicker and new areas stutter less."},
                new String[] {
                    "With 60 FPS on, a quick tap on the touch buttons could be lost, so the title menu cursor sometimes would not move.",
                    "Aiming with a touch drag or the gyro went up and down the wrong way: the game's Aim option Normal aims down with the stick up. The sight now follows the finger and the phone, whatever that option says.",
                    "Closing the HOME Menu could crash the game (its sound thread fell behind and the shutdown failed).",
                    "Separate Ways kept its black bars and a smaller picture with Fill Screen on."},
                new String[] {
                    "With 60 FPS on, smoke, fire, blood and the menus still move at 30.",
                    "On some PowerVR phones (Samsung A04) the inventory background shows only in the top-right quarter.",
                    "Some Mali phones can freeze while screen recording or streaming."}),
        new Entry("Beta 8", "Smooth 60 FPS", "2026-11-12",
                new String[] {
                    "60 FPS is rebuilt: the game keeps its own 30 fps and the phone draws a picture between two of its frames. Ashley, events, doors and timers behave exactly as at 30. It is off after this update; turn it on in Performance. It needs about twice the GPU work and drops back to 30 by itself when the phone cannot keep up.",
                    "Settings > Controls > Check Gamepad: lists the connected gamepads and shows each button press, to see whether the phone gets the gamepad at all."},
                new String[] {
                    "A second, see-through copy of Leon and the scene above the picture where the game blurs the screen (Del Lago, explosions, low health): the blur's frame copy was squeezed to 86% of its height.",
                    "A Bluetooth gamepad that did nothing in the game (its media-key half, such as \"GamePadPlus V3 Consumer Control\", took player 1): every connected gamepad now controls Leon."},
                new String[] {
                    "With 60 FPS on, smoke, fire, blood and the menus still move at 30.",
                    "On some PowerVR phones (Samsung A04) the inventory background shows only in the top-right quarter.",
                    "Some Mali phones can freeze while screen recording or streaming."}),
        new Entry("Beta 7", "Touch camera and Separate Ways merchant", "2026-11-12",
                new String[] {},
                new String[] {
                    "Touch camera works like a thumb on the stick: hold the finger off where it landed to keep turning, lift it to let go. It always turns the way the finger is held (Invert Camera no longer reverses it), swings further the further the finger goes, and Camera Sensitivity sets how far a drag swings it. It no longer swings back when the finger stops or looks at the ceiling during a sideways drag.",
                    "Crash when buying weapons in Separate Ways with a save from Beta 4: Leon's Special Weapons are taken out of Ada's merchant even with the cheat off."},
                new String[] {
                    "On some PowerVR phones (Samsung A04) the inventory background shows only in the top-right quarter.",
                    "With 60 FPS on, Ashley floats up and some events can get stuck (the Camp Key door).",
                    "Some Mali phones can freeze while screen recording or streaming."}),
        new Entry("Beta 6", "Puzzles, character shine and a safer 60 FPS", "2026-11-12",
                new String[] {
                    "The Ashley button shows two people.",
                    "60 FPS (Experimental) asks first: a warning lists its known problems, and the game restarts to turn it on."},
                new String[] {
                    "Reduce Effects no longer hides puzzle lights, switches and lasers: it pauses in every puzzle, terminal and laser room (the church symbols first).",
                    "Leon's, Ashley's and the Ganados' hair is no longer see-through or dark on phones: the graphics buffers were too small for RE4 and split every frame.",
                    "Fewer GPU submits a frame on every phone, from the same buffer fix.",
                    "Reduce Effects keeps the lit parts of models.",
                    "60 FPS is off after this update, even if it was on before."},
                new String[] {
                    "With 60 FPS on, Ashley floats up and some events can get stuck (the Camp Key door).",
                    "Some Mali phones can freeze while screen recording or streaming.",
                    "Item glows may still be hidden with Reduce Effects on."}),
        new Entry("Beta 5", "Fill Screen without Real Widescreen", "2026-11-12",
                new String[] {"While R or L is held, dragging anywhere aims."},
                new String[] {
                    "Real Widescreen is gone: the picture is stretched as a whole, so HUD and world line up.",
                    "Crash at El Gigante (chapter 1-3).",
                    "Freeze when buying weapons in Separate Ways: Special Weapons now stocks only Leon's merchant.",
                    "Fill Screen works again; the typewriter save screen and the merchant highlight show correctly."},
                new String[] {}),
        new Entry("Beta 3", "Minimap, auto save and cheats", "2026-11-12",
                new String[] {
                    "Minimap overlay.",
                    "Auto save of each room to FILE 20, with a warning on screen.",
                    "Cheats: infinite ammo and grenades, Special Weapons at the merchant, Max Money.",
                    "Gyro aim."},
                new String[] {
                    "Crashes in chapters 1-2 and 1-4.",
                    "Less CPU work per frame; GX Thread is on by default on phones with 4 or more cores.",
                    "The GPU % in the overlay is measured on the GPU."},
                new String[] {}),
        new Entry("Beta 1", "First beta", "2026-11-11",
                new String[] {
                    "Reduce Shadows, Reduce Screen Filters and Reduce Effects, in the Low-End presets.",
                    "Fill Screen without the black bars.",
                    "60 FPS (Experimental), back to 30 in cutscenes.",
                    "Classic Controller layout on the touch screen, so the game aims with the laser."},
                new String[] {"Soft reset no longer hangs."},
                new String[] {}),
        new Entry("0.0.1", "Resident Evil 4 Wii: first build", "2026-11-11",
                new String[] {"Boots RB4E08 and plays the main game on Windows."},
                new String[] {},
                new String[] {"The Android launcher still carries parts of the port it was copied from (save backup)."}),
    };

    /** The release shows its own notes from 1.0 on; the Beta keeps the Beta history too. */
    private static final Entry[] ENTRIES = BuildConfig.BETA ? betaEntries() : releaseEntries();

    /** The Beta ends at the Final Beta: the release's own notes are not its. */
    private static Entry[] betaEntries() {
        for (int i = 0; i < ALL.length; ++i) {
            if (ALL[i].version.equals("Final Beta")) return java.util.Arrays.copyOfRange(ALL, i, ALL.length);
        }
        return ALL;
    }

    private static Entry[] releaseEntries() {
        for (int i = 0; i < ALL.length; ++i) {
            if (ALL[i].version.equals(BetaGate.RELEASE_VERSION)) return java.util.Arrays.copyOf(ALL, i + 1);
        }
        return ALL;
    }

    /** The build installed: its notes are shown once. */
    private static String build(Activity activity) {
        try {
            return activity.getPackageManager().getPackageInfo(activity.getPackageName(), 0).getLongVersionCode()
                    + ":" + ENTRIES[0].title;
        } catch (Exception e) {
            return ENTRIES[0].title;
        }
    }

    /** After an install or update: the screen, on the newest version, once. */
    static void showIfNew(Activity activity, AppSettings settings) {
        final String build = build(activity);
        if (build.equals(settings.notesSeen())) return;
        settings.setNotesSeen(build);
        showAll(activity);
    }

    /** As above, with `after` run once it is closed. False (and `after` not run) when nothing is new. */
    static boolean showIfNew(Activity activity, AppSettings settings, Runnable after) {
        final String build = build(activity);
        if (build.equals(settings.notesSeen())) return false;
        settings.setNotesSeen(build);
        final ReleaseNotes notes = new ReleaseNotes(activity);
        notes.dialog.setOnDismissListener(d -> after.run());
        notes.dialog.show();
        return true;
    }

    /** The full screen, on the newest version. */
    static void showAll(Activity activity) {
        new ReleaseNotes(activity).dialog.show();
    }

    // --- the screen (same layout and look as SettingsMenu) ---------------------------------------

    private final Activity activity;
    private final Dialog dialog;
    private final LinearLayout nav;
    private final LinearLayout rows;
    private final ScrollView rowsScroll;

    private ReleaseNotes(Activity activity) {
        this.activity = activity;

        // Upright page: the title, a sideways strip of versions, the chosen version's notes, Done in view.
        final TextView title = text(Tr.t("What's New"), 24, Color.WHITE);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        final TextView installed = text(Tr.t("Installed: RE4 ") + Updater.displayVersion(activity), 12,
                LauncherActivity.MUTED);
        installed.setPadding(0, dp(2), 0, dp(10));
        nav = new LinearLayout(activity);
        nav.setOrientation(LinearLayout.HORIZONTAL);
        for (int i = 0; i < ENTRIES.length; ++i) {
            final int index = i;
            final LinearLayout item = new LinearLayout(activity);
            item.setOrientation(LinearLayout.VERTICAL);
            item.setPadding(dp(14), dp(8), dp(14), dp(8));
            item.setClickable(true);
            item.setOnClickListener(v -> showEntry(index));
            final TextView name = text(ENTRIES[i].version, 15, Color.WHITE);
            name.setTypeface(LauncherActivity.MEDIUM);
            item.addView(name);
            item.addView(text(i == 0 ? Tr.t("this version") : ENTRIES[i].date, 11, LauncherActivity.MUTED));
            final LinearLayout.LayoutParams itemParams = new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
            if (i > 0) itemParams.leftMargin = dp(6);
            nav.addView(item, itemParams);
        }
        final HorizontalScrollView navScroll = new HorizontalScrollView(activity);
        navScroll.setHorizontalScrollBarEnabled(false);
        navScroll.addView(nav);

        rows = new LinearLayout(activity);
        rows.setOrientation(LinearLayout.VERTICAL);
        rows.setPadding(0, dp(14), 0, 0);
        rowsScroll = new ScrollView(activity);
        rowsScroll.setVerticalScrollBarEnabled(false);
        rowsScroll.addView(rows);

        final TextView done = text(Tr.t("Done"), 17, Color.WHITE);
        done.setTypeface(Typeface.DEFAULT_BOLD);
        done.setGravity(Gravity.CENTER);
        done.setPadding(0, dp(12), 0, dp(12));
        done.setBackground(rounded(LauncherActivity.ACCENT, dp(24)));

        final LinearLayout page = new LinearLayout(activity);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setBackgroundColor(LauncherActivity.BACKGROUND);
        page.addView(title);
        page.addView(installed);
        page.addView(navScroll, matchWidth(0));
        page.addView(rowsScroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));
        page.addView(done, matchWidth(dp(10)));

        dialog = new Dialog(activity, android.R.style.Theme_DeviceDefault_NoActionBar_Fullscreen);
        dialog.setContentView(page);
        // Bars stay visible (like the launcher); the page keeps clear of them.
        SystemBars.edgeToEdge(dialog.getWindow());
        page.setOnApplyWindowInsetsListener((v, insets) -> {
            final android.graphics.Insets bars = SystemBars.bars(insets, false);
            v.setPadding(dp(16) + bars.left, dp(16) + bars.top, dp(16) + bars.right, dp(16) + bars.bottom);
            return SystemBars.consumed(insets);
        });
        done.setOnClickListener(v -> dialog.dismiss());
        showEntry(0);
    }


    private void showEntry(int index) {
        for (int i = 0; i < ENTRIES.length; ++i) {
            nav.getChildAt(i).setBackground(i == index ? rounded(LauncherActivity.RAISED, dp(16)) : null);
        }
        rows.removeAllViews();
        final Entry e = ENTRIES[index];
        final TextView heading = text(e.version, 22, Color.WHITE);
        heading.setTypeface(Typeface.DEFAULT_BOLD);
        rows.addView(heading);
        final TextView about = text(Tr.t(e.title) + " · " + e.date, 13, LauncherActivity.MUTED);
        about.setPadding(0, dp(2), 0, 0);
        rows.addView(about);
        group(Tr.t("Added"), "+", LauncherActivity.ACCENT, e.added);
        group(Tr.t("Fixed"), "✓", LauncherActivity.GREEN, e.fixed);
        group(Tr.t("Known issues"), "!", Sheet.WARNING_TEXT, e.known);
        rowsScroll.scrollTo(0, 0);
    }

    /** A label, then one card per change, each with its mark on the left. */
    private void group(String label, String mark, int markColor, String[] lines) {
        if (lines.length == 0) return;
        final TextView title = text(label, 13, LauncherActivity.MUTED);
        title.setTypeface(LauncherActivity.MEDIUM);
        rows.addView(title, matchWidth(dp(18)));
        for (String line : lines) {
            final LinearLayout card = new LinearLayout(activity);
            card.setOrientation(LinearLayout.HORIZONTAL);
            card.setGravity(Gravity.CENTER_VERTICAL);
            card.setPadding(dp(18), dp(14), dp(14), dp(14));
            card.setBackground(rounded(LauncherActivity.SURFACE, dp(18)));
            final TextView markView = text(mark, 16, markColor);
            markView.setTypeface(Typeface.DEFAULT_BOLD);
            markView.setGravity(Gravity.CENTER);
            card.addView(markView, new LinearLayout.LayoutParams(dp(22), LinearLayout.LayoutParams.WRAP_CONTENT));
            final TextView words = text(Tr.t(line), 15, Color.WHITE);
            final LinearLayout.LayoutParams wordsParams =
                    new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
            wordsParams.leftMargin = dp(10);
            card.addView(words, wordsParams);
            rows.addView(card, matchWidth(dp(8)));
        }
    }

    private TextView text(String value, int sp, int color) {
        final TextView view = new TextView(activity);
        view.setText(value);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        view.setTextColor(color);
        return view;
    }

    private static GradientDrawable rounded(int color, int radius) {
        final GradientDrawable shape = new GradientDrawable();
        shape.setCornerRadius(radius);
        shape.setColor(color);
        return shape;
    }

    private static LinearLayout.LayoutParams matchWidth(int topMargin) {
        final LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        params.topMargin = topMargin;
        return params;
    }

    private int dp(int value) {
        return Math.round(value * activity.getResources().getDisplayMetrics().density);
    }
}
