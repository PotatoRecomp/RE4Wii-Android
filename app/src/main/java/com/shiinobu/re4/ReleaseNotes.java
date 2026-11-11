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
