package com.shiinobu.re4;

import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RadialGradient;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;

/**
 * The touch controls' faces, in three styles (Modern default, Classic, and optional Item Liquid):
 * <ul>
 * <li>Modern: round buttons with a layered metal ring, a silhouette with a vertical gradient
 * (TouchGlyphs), a caption inside and the controller letter in a badge.</li>
 * <li>Classic: no pictures. A large gradient letter with a small caption, a seal dot toward the
 * outside of the diamond, pill shoulders and a cross D-pad.</li>
 * <li>Item Liquid: glossy black liquid gamepad style with controller X/A/B/Y & analog sticks,
 * curved ergonomic L/R triggers, and thin rounded rectangle utility buttons.</li>
 * </ul>
 * Every control is drawn in an 80x80 box centred on 40,40 and scaled onto the screen, so the shaders
 * are made once in box space. Alphas follow the opacity setting; held controls stay clear.
 */
final class TouchIcons {
    /** One set of colours and the shaders made from it. */
    private static final class Palette {
        final int ring, ringHi, ringLo, fillB, ic2, label;
        final Shader base, hot, glyph, glyphHeld, letter, letterSmall, knob, knobGlow, knobGlowHeld;
        Palette(int ring, int ringHi, int ringLo, int fillA, int fillB, int ic1, int ic2, int ic3, int glow, int label) {
            this.ring = ring;
            this.ringHi = ringHi;
            this.ringLo = ringLo;
            this.fillB = fillB;
            this.ic2 = ic2;
            this.label = label;
            base = new RadialGradient(40f, 31f, 52f, rgb(fillA), rgb(fillB), Shader.TileMode.CLAMP);
            hot = new RadialGradient(40f, 32f, 27f, argb(0.45f, glow), 0, Shader.TileMode.CLAMP);
            final int[] stops = {rgb(ic1), rgb(ic2), rgb(ic3)};
            final float[] at = {0f, 0.55f, 1f};
            glyph = new LinearGradient(0f, 0f, 0f, TouchGlyphs.BOX, stops, at, Shader.TileMode.CLAMP);
            glyphHeld = new LinearGradient(0f, 0f, 0f, TouchGlyphs.BOX,
                    new int[] {rgb(0xFFFFFF), rgb(0xFFFFFF), rgb(ic2)}, new float[] {0f, 0.6f, 1f}, Shader.TileMode.CLAMP);
            letter = new LinearGradient(0f, 21f, 0f, 52f, stops, at, Shader.TileMode.CLAMP);
            letterSmall = new LinearGradient(0f, 27f, 0f, 53f, stops, at, Shader.TileMode.CLAMP);
            // Knob shaders are in the knob's own space (centred on 0,0).
            knob = new RadialGradient(-0.1f * KNOB, -0.2f * KNOB, 1.4f * KNOB, rgb(ic1), rgb(ic2), Shader.TileMode.CLAMP);
            knobGlow = new RadialGradient(0f, 0f, 2.2f * KNOB, argb(0.4f, glow), 0, Shader.TileMode.CLAMP);
            knobGlowHeld = new RadialGradient(0f, 0f, 2.4f * KNOB, argb(0.7f, glow), 0, Shader.TileMode.CLAMP);
        }
    }

    private static final Palette BLUE = new Palette(0x2F7FE0, 0x8CC4FF, 0x0D2C5C, 0x0F1D33, 0x05090F,
            0xE6F3FF, 0x4EA4FF, 0x10408A, 0x3C96FF, 0xDCECFF);
    private static final Palette GREEN = new Palette(0x2FAE5F, 0x8BE3A8, 0x0D4224, 0x10291A, 0x050C08,
            0xE3FBE9, 0x48D17C, 0x0F6A33, 0x3CD278, 0xD8F7E3);
    private static final Palette RED = new Palette(0xD8343E, 0xFF8A7A, 0x5E1016, 0x2A1013, 0x0C0607,
            0xFFE1D8, 0xFF5A4A, 0x8E1620, 0xFF4632, 0xFFE3DC);
    private static final Palette AMBER = new Palette(0xE0A92A, 0xFFE08A, 0x5A420C, 0x2A2210, 0x0D0A04,
            0xFFF6D6, 0xF2BE3C, 0x7A5510, 0xFFBE3C, 0xFFF2CC);
    private static final Palette WHITE = new Palette(0xD9DEE6, 0xFFFFFF, 0x4A4F58, 0x24272C, 0x0A0B0D,
            0xFFFFFF, 0xE3E7EE, 0x8A93A3, 0xFFFFFF, 0xFFFFFF);

    /** The controls' colour; A, B, X and Y keep their own. */
    static final int COLOR_GOLD = 0, COLOR_RED = 1, COLOR_BLUE = 2, COLOR_WHITE = 3;
    private static final Palette[] THEMES = {AMBER, RED, BLUE, WHITE};
    private Palette theme = BLUE;

    void setColor(int color) { theme = color >= 0 && color < THEMES.length ? THEMES[color] : BLUE; }

    /** The colours of the sticks, D-pad and every button but A, B, X and Y. */
    private Palette chrome() { return theme; }

    /** A face button's colours (A green, B red, X blue, Y amber); everything else is the theme's. */
    private Palette paletteOf(int bit) {
        switch (bit) {
            case TouchControlsView.A: return GREEN;
            case TouchControlsView.B: return RED;
            case TouchControlsView.Y: return AMBER;
            case TouchControlsView.X: return BLUE;
            default: return theme;
        }
    }

    /** Button radius in the box: the ring's outer edge. */
    private static final float R = 38f, D = 2f * R;
    private static final float WELL = 38f, KNOB = 0.18f * 2f * WELL;
    /** Item Liquid physical gamepad thumbstick cap radius (fuller than minimal knob). */
    private static final float STICK_KNOB = 0.23f * 2f * WELL;
    /** Classic pills: width over height. */
    static final float PILL_RATIO = 1.62f;

    // Classic: the D-pad's cross, its arms, chevrons, and the Home outline (24 box).
    private static final Path CROSS = svg("M 33 6 H 47 A 4 4 0 0 1 51 10 V 29 H 70 A 4 4 0 0 1 74 33 V 47 "
            + "A 4 4 0 0 1 70 51 H 51 V 70 A 4 4 0 0 1 47 74 H 33 A 4 4 0 0 1 29 70 V 51 H 10 "
            + "A 4 4 0 0 1 6 47 V 33 A 4 4 0 0 1 10 29 H 29 V 10 A 4 4 0 0 1 33 6 Z");
    private static final Path ARM_UP = svg("M 33 7 H 47 A 3 3 0 0 1 50 10 V 30 H 30 V 10 A 3 3 0 0 1 33 7 Z");
    private static final Path ARM_DOWN = svg("M 30 50 H 50 V 70 A 3 3 0 0 1 47 73 H 33 A 3 3 0 0 1 30 70 Z");
    private static final Path ARM_LEFT = svg("M 10 30 H 30 V 50 H 10 A 3 3 0 0 1 7 47 V 33 A 3 3 0 0 1 10 30 Z");
    private static final Path ARM_RIGHT = svg("M 50 30 H 70 A 3 3 0 0 1 73 33 V 47 A 3 3 0 0 1 70 50 H 50 Z");
    private static final Path CHEV_UP = svg("M 35.5 19 L 40 13.5 L 44.5 19");
    private static final Path CHEV_DOWN = svg("M 35.5 61 L 40 66.5 L 44.5 61");
    private static final Path CHEV_LEFT = svg("M 19 35.5 L 13.5 40 L 19 44.5");
    private static final Path CHEV_RIGHT = svg("M 61 35.5 L 66.5 40 L 61 44.5");
    private static final Path HOUSE = svg("M 5 11.5 L 12 5 L 19 11.5 M 7.5 10 V 18.5 H 16.5 V 10");

    // Item Liquid: rounded cross D-pad (6px rounded tips + 3px concave inner fillets).
    private static final Path LIQUID_CROSS = svg("M 35 6 H 45 A 6 6 0 0 1 51 12 V 26 Q 51 29 54 29 H 68 "
            + "A 6 6 0 0 1 74 35 V 45 A 6 6 0 0 1 68 51 H 54 Q 51 51 51 54 V 68 "
            + "A 6 6 0 0 1 45 74 H 35 A 6 6 0 0 1 29 68 V 54 Q 29 51 26 51 H 12 "
            + "A 6 6 0 0 1 6 45 V 35 A 6 6 0 0 1 12 29 H 26 Q 29 29 29 26 V 12 A 6 6 0 0 1 35 6 Z");
    private static final Path LIQUID_ARM_UP = svg("M 35 7 H 45 A 5 5 0 0 1 50 12 V 30 H 30 V 12 A 5 5 0 0 1 35 7 Z");
    private static final Path LIQUID_ARM_DOWN = svg("M 30 50 H 50 V 68 A 5 5 0 0 1 45 73 H 35 A 5 5 0 0 1 30 68 Z");
    private static final Path LIQUID_ARM_LEFT = svg("M 12 30 H 30 V 50 H 12 A 5 5 0 0 1 7 45 V 35 A 5 5 0 0 1 12 30 Z");
    private static final Path LIQUID_ARM_RIGHT = svg("M 50 30 H 68 A 5 5 0 0 1 73 35 V 45 A 5 5 0 0 1 68 50 H 50 Z");

    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint line = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Typeface fontCondensed = Typeface.create("sans-serif-condensed", Typeface.BOLD);
    private final Typeface fontLiquid = Typeface.create("sans-serif", Typeface.BOLD);
    private final Path tri = new Path();
    private final RectF cell = new RectF();
    private final RectF rect = new RectF();
    private float opacity = 0.6f;

    private final Shader gloss = new LinearGradient(40f, 2f, 40f, 40f, argb(0.15f, 0xFFFFFF), 0, Shader.TileMode.CLAMP);
    private final Shader wellShade = new RadialGradient(40f, 40f, WELL, argb(0.05f, 0), argb(0.32f, 0),
            Shader.TileMode.CLAMP);

    // Item Liquid shaders (pre-allocated in 80x80 box space; knob shaders in 0,0 space).
    private final Shader liquidBase = new RadialGradient(40f, 26f, 56f,
            new int[] {rgb(0x282E38), rgb(0x0E1117), rgb(0x05070A)},
            new float[] {0f, 0.55f, 1f}, Shader.TileMode.CLAMP);
    private final Shader liquidTopSheen = new LinearGradient(40f, 3f, 40f, 38f,
            new int[] {argb(0.42f, 0xFFFFFF), argb(0.11f, 0xE8F1FF), 0},
            new float[] {0f, 0.52f, 1f}, Shader.TileMode.CLAMP);
    private final Shader liquidBottomBounce = new LinearGradient(40f, 52f, 40f, 77f,
            new int[] {0, argb(0.18f, 0xD8E8FF)},
            new float[] {0f, 1f}, Shader.TileMode.CLAMP);
    private final Shader liquidPressGlow = new RadialGradient(40f, 40f, 36f,
            argb(0.32f, 0xFFFFFF), argb(0.08f, 0xDCEAFF), Shader.TileMode.CLAMP);
    private final Shader liquidRim = new LinearGradient(40f, 2f, 40f, 78f,
            new int[] {argb(0.82f, 0xFFFFFF), argb(0.22f, 0xC8D4E6), argb(0.18f, 0x90A0B8), argb(0.50f, 0xDCEAFF)},
            new float[] {0f, 0.36f, 0.68f, 1f}, Shader.TileMode.CLAMP);
    private final Shader liquidLetter = new LinearGradient(0f, 21f, 0f, 58f,
            new int[] {rgb(0xFFFFFF), rgb(0xE8EEF7), rgb(0xBAC4D4)},
            new float[] {0f, 0.58f, 1f}, Shader.TileMode.CLAMP);
    private final Shader liquidGlyph = new LinearGradient(0f, 36f, 0f, TouchGlyphs.BOX - 36f,
            new int[] {rgb(0xFFFFFF), rgb(0xEDF3FC), rgb(0xB8C7DC)},
            new float[] {0f, 0.58f, 1f}, Shader.TileMode.CLAMP);
    private final Shader liquidWellBase = new RadialGradient(40f, 40f, WELL,
            new int[] {argb(0.54f, 0x030508), argb(0.68f, 0x0B0E14), argb(0.76f, 0x161B24)},
            new float[] {0f, 0.72f, 1f}, Shader.TileMode.CLAMP);
    private final Shader liquidKnobBase = new RadialGradient(-0.20f * STICK_KNOB, -0.26f * STICK_KNOB, 1.45f * STICK_KNOB,
            new int[] {rgb(0x343A46), rgb(0x11151C), rgb(0x040608)},
            new float[] {0f, 0.54f, 1f}, Shader.TileMode.CLAMP);
    private final Shader liquidKnobDish = new RadialGradient(0f, 0.24f * STICK_KNOB, 0.92f * STICK_KNOB,
            new int[] {rgb(0x252B36), rgb(0x0D1016), rgb(0x05070A)},
            new float[] {0f, 0.55f, 1f}, Shader.TileMode.CLAMP);
    private final Shader liquidKnobSheen = new LinearGradient(0f, -STICK_KNOB, 0f, 0.12f * STICK_KNOB,
            new int[] {argb(0.46f, 0xFFFFFF), argb(0.10f, 0xE6F0FF), 0},
            new float[] {0f, 0.55f, 1f}, Shader.TileMode.CLAMP);
    private final Shader liquidKnobRim = new LinearGradient(0f, -STICK_KNOB, 0f, STICK_KNOB,
            new int[] {argb(0.86f, 0xFFFFFF), argb(0.24f, 0xB8C6D9), argb(0.52f, 0xDCEAFF)},
            new float[] {0f, 0.55f, 1f}, Shader.TileMode.CLAMP);
    private final Path triggerPath = new Path();

    TouchIcons() {
        fill.setStyle(Paint.Style.FILL);
        line.setStyle(Paint.Style.STROKE);
        line.setStrokeCap(Paint.Cap.ROUND);
        line.setStrokeJoin(Paint.Join.ROUND);
        text.setTextAlign(Paint.Align.CENTER);
        text.setTypeface(fontCondensed);
    }

    void setOpacity(float opacity) { this.opacity = Math.max(0f, Math.min(1f, opacity)); }

    private static int rgb(int rgb) { return 0xFF000000 | rgb; }
    private static int argb(float alpha, int rgb) { return (Math.round(alpha * 255) << 24) | (rgb & 0xFFFFFF); }

    /** Sets a paint's colour at `alpha` of the current opacity. */
    private void tint(Paint p, int rgb, float alpha, boolean held) {
        p.setShader(null);
        p.setColor(rgb(rgb));
        p.setAlpha(Math.round(255 * Math.max(0f, Math.min(1f, alpha * level(held)))));
    }

    private void shade(Paint p, Shader s, float alpha, boolean held) {
        p.setColor(0xFFFFFFFF);
        p.setShader(s);
        p.setAlpha(Math.round(255 * Math.max(0f, Math.min(1f, alpha * level(held)))));
    }

    private float level(boolean held) { return held ? Math.max(opacity, 0.95f) : opacity; }

    /** Moves the canvas so the 80x80 box sits centred on cx,cy at `unit` pixels per step. */
    private static void enter(Canvas c, float cx, float cy, float unit, boolean down) {
        c.save();
        c.translate(cx, cy);
        c.scale(unit, unit);
        if (down) c.scale(0.94f, 0.94f);
        c.translate(-40f, -40f);
    }

    /**
     * The disc under every round button in Feral Interactive style:
     * - Subtle translucent dark circular backing
     * - Soft press highlight fill when touched
     * - Single crisp circular outline (circle line, monochrome)
     */
    private void disc(Canvas c, Palette p, boolean down, boolean hot) {
        // Translucent dark circular backing
        tint(fill, 0x050810, down ? 0.35f : 0.22f, down);
        c.drawCircle(40f, 40f, R, fill);

        // Soft highlight fill when pressed or hot
        if (down) {
            tint(fill, 0xFFFFFF, 0.28f, true);
            c.drawCircle(40f, 40f, R - 1f, fill);
        } else if (hot) {
            tint(fill, 0xFFFFFF, 0.10f, false);
            c.drawCircle(40f, 40f, R - 1f, fill);
        }

        // Crisp circle line outline (clean monochrome white)
        tint(line, 0xFFFFFF, down ? 1.0f : 0.85f, down);
        line.setStrokeWidth(down ? 2.8f : 2.0f);
        c.drawCircle(40f, 40f, R - 1.2f, line);
    }

    private void caption(Canvas c, Palette p, String caption, float size, float spacing, boolean down) {
        text.setTextSize(size);
        text.setLetterSpacing(spacing);
        final float fit = 0.74f * D / Math.max(1f, text.measureText(caption));
        if (fit < 1f) text.setTextSize(size * fit);
        tint(text, p.label, 1f, down);
        text.setShadowLayer(1.6f, 0f, 0.8f, argb(0.9f * level(down), 0));
        c.drawText(caption, 40f, 2f + 0.85f * D - text.descent() * 0.4f, text);
        text.clearShadowLayer();
        text.setLetterSpacing(0f);
    }

    /**
     * Modern style: a clean circular line button with a detailed icon centered inside
     * and a Wii Classic Controller button indicator badge seamlessly set into the top rim.
     */
    void button(Canvas c, int bit, Path glyph, String caption, String key, float cx, float cy, float r,
            boolean down, boolean hot, boolean keyBelow) {
        enter(c, cx, cy, r / R, down);

        final float C = 40f;
        final float mainR = 30.5f;
        final float by = C - mainR; // 9.5f, sits right on top rim
        final boolean hasKey = key != null;
        final boolean wide = hasKey && key.length() > 1; // "ZL", "ZR"
        final float rx = wide ? 10.5f : 8.0f;
        final float ry = 8.0f;
        final float gap = (wide ? 10.5f : 8.0f) * 1.15f;

        // 1. Backing (Dark translucent glass fill)
        tint(fill, 0x050810, down ? 0.35f : 0.22f, down);
        c.drawCircle(C, C, mainR, fill);
        if (hasKey) {
            cell.set(C - rx, by - ry, C + rx, by + ry);
            c.drawRoundRect(cell, ry, ry, fill);
        }

        // 2. Press highlight fill
        if (down) {
            tint(fill, 0xFFFFFF, 0.28f, true);
            c.drawCircle(C, C, mainR - 1f, fill);
            if (hasKey) {
                tint(fill, 0xFFFFFF, 1.0f, true);
                c.drawRoundRect(cell, ry, ry, fill);
            }
        } else if (hot) {
            tint(fill, 0xFFFFFF, 0.10f, false);
            c.drawCircle(C, C, mainR - 1f, fill);
        }

        // 3. Main Circle Rim (with gap at top where badge sits)
        tint(line, 0xFFFFFF, down ? 1.0f : 0.85f, down);
        line.setStrokeWidth(down ? 2.8f : 2.0f);
        if (hasKey) {
            rect.set(C - mainR + 1f, C - mainR + 1f, C + mainR - 1f, C + mainR - 1f);
            final float halfDeg = (float) Math.toDegrees(Math.asin(Math.min(gap / mainR, 1f)));
            c.drawArc(rect, -90f + halfDeg, 360f - 2f * halfDeg, false, line);
        } else {
            c.drawCircle(C, C, mainR - 1f, line);
        }

        // 4. Badge on top rim
        if (hasKey) {
            cell.set(C - rx, by - ry, C + rx, by + ry);
            tint(line, down ? 0xFFFFFF : 0xE6EDF5, down ? 1.0f : 0.85f, down);
            line.setStrokeWidth(down ? 2.2f : 1.6f);
            c.drawRoundRect(cell, ry, ry, line);

            text.setTextSize(wide ? 9.5f : 11.5f);
            text.setTypeface(Typeface.create("sans-serif-condensed", Typeface.BOLD));
            if (down) {
                tint(text, 0x080B12, 1.0f, true);
            } else {
                tint(text, 0xFFFFFF, 1.0f, false);
            }
            c.drawText(key, C, by - (text.descent() + text.ascent()) / 2f + 0.5f, text);
        }

        // 5. Action Icon centered inside the main circle
        if (glyph != null) {
            final float size = 0.46f * D;
            c.save();
            c.translate(C - size / 2f, C + mainR * 0.04f - size / 2f);
            final float k = size / TouchGlyphs.BOX;
            c.scale(k, k);
            tint(fill, down ? 0xFFFFFF : 0xF2F5F8, down ? 1.0f : 0.90f, down);
            c.drawPath(glyph, fill);
            c.restore();
        }

        c.restore();
    }


    /**
     * Classic: a round button with its letter, a small caption under it (null for none) and, for
     * the face buttons, a seal dot on the side facing out of the diamond (`seal`: 0 none, 1 right,
     * 2 bottom, 3 top, 4 left).
     */
    void classic(Canvas c, int bit, String letter, String caption, float cx, float cy, float r,
            boolean down, boolean hot, int seal) {
        final Palette p = paletteOf(bit);
        enter(c, cx, cy, r / R, down);
        disc(c, p, down, hot);
        tint(line, p.ringHi, 0.28f, down);
        line.setStrokeWidth(0.8f);
        c.drawCircle(40f, 40f, 0.7f * R, line);
        letter(c, p, letter, caption != null ? 37f : 40f, caption != null ? 0.46f * D : 0.5f * D, down);
        if (caption != null) caption(c, p, caption, 0.11f * D, 0.12f, down);
        if (seal != 0) {
            final float dot = 0.035f * D, edge = 2f + 0.085f * D + dot;
            final float sx = seal == 1 ? 80f - edge : seal == 4 ? edge : 40f;
            final float sy = seal == 2 ? 80f - 2f - 0.05f * D - dot : seal == 3 ? edge : 40f;
            tint(fill, p.ringHi, 1f, down);
            c.drawCircle(sx, sy, dot, fill);
        }
        c.restore();
    }

    /** Classic shoulder: a pill `hw` x `hh` (half sizes) on screen with its letters and caption. */
    void classicPill(Canvas c, int bit, String letter, String caption, float cx, float cy, float hw, float hh,
            boolean down, boolean hot) {
        final Palette p = paletteOf(bit);
        enter(c, cx, cy, hh / R, down);
        final float half = R * hw / hh;
        cell.set(40f - half, 2f, 40f + half, 78f);
        shade(fill, p.base, 1f, down);
        c.drawRoundRect(cell, R, R, fill);
        if (hot || down) {
            shade(fill, p.hot, down ? 1.5f : 1f, down);
            c.drawRoundRect(cell, R, R, fill);
        }
        shade(fill, gloss, 1f, down);
        c.drawRoundRect(cell, R, R, fill);
        tint(line, down ? 0xFFFFFF : p.ring, 1f, down);
        line.setStrokeWidth(2.3f);
        cell.inset(1.15f, 1.15f);
        c.drawRoundRect(cell, R, R, line);
        tint(line, p.ringLo, 1f, down);
        line.setStrokeWidth(1.7f);
        cell.inset(2f, 2f);
        c.drawRoundRect(cell, R, R, line);
        tint(line, p.ringHi, 0.28f, down);
        line.setStrokeWidth(0.8f);
        cell.inset(8f, 8f);
        c.drawRoundRect(cell, R, R, line);
        letter(c, p, letter, caption != null ? 37f : 40f, 0.42f * D, down);
        if (caption != null) caption(c, p, caption, 0.11f * D, 0.12f, down);
        c.restore();
    }

    /** Classic −, + (a letter), Home (an outline) and the gear (its glyph), without captions. */
    void classicSystem(Canvas c, int bit, String letter, Path glyph, float cx, float cy, float r, boolean down) {
        final Palette p = paletteOf(bit);
        enter(c, cx, cy, r / R, down);
        disc(c, p, down, false);
        if (bit == TouchControlsView.HOME) {
            c.save();
            c.translate(40f - 0.26f * D, 40f - 0.26f * D);
            c.scale(0.52f * D / 24f, 0.52f * D / 24f);
            line.setShader(null);
            line.setColor(0xFFFFFFFF);
            line.setShader(p.letterSmall);
            line.setAlpha(Math.round(255 * level(down)));
            line.setStrokeWidth(2.2f);
            c.drawPath(HOUSE, line);
            line.setShader(null);
            c.restore();
        } else if (glyph != null) {
            final float size = 0.52f * D, k = size / TouchGlyphs.BOX;
            c.save();
            c.translate(40f - size / 2f, 40f - size / 2f);
            c.scale(k, k);
            shade(fill, down ? p.glyphHeld : p.glyph, 1f, down);
            c.drawPath(glyph, fill);
            c.restore();
        } else {
            letter(c, p, letter, 40f, 0.56f * D, down);
        }
        c.restore();
    }

    private void letter(Canvas c, Palette p, String letter, float centreY, float size, boolean down) {
        text.setTextSize(size);
        final float y = centreY - (text.descent() + text.ascent()) / 2f;
        tint(text, 0, 0.75f, down);
        c.drawText(letter, 40f, y + 1.8f, text);
        if (down) tint(text, 0xFFFFFF, 1f, true);
        else shade(text, p.letter, 1f, false);
        c.drawText(letter, 40f, y, text);
        text.setShader(null);
    }

    /** A stick ("minimal"): a clean circle line well with a sleek lit knob moved by vx, vy (-1..1). */
    void stick(Canvas c, float cx, float cy, float r, float vx, float vy, boolean active) {
        stickWell(c, cx, cy, r, active);
        final float[] at = knobAt(cx, cy, r, vx, vy);
        stickKnob(c, at[0], at[1], r, active);
    }

    /** Where the knob of a stick at cx, cy (radius r) sits for vx, vy. */
    static float[] knobAt(float cx, float cy, float r, float vx, float vy) {
        return knobAt(cx, cy, r, vx, vy, false);
    }

    static float[] knobAt(float cx, float cy, float r, float vx, float vy, boolean stickCap) {
        final float k = stickCap ? STICK_KNOB : KNOB;
        final float travel = (WELL - k) * r / WELL;
        return new float[] {cx + vx * travel, cy + vy * travel};
    }

    /** Half the size of a stick's knob on screen, its outline included. */
    static float knobHalf(float r) { return knobHalf(r, false); }

    static float knobHalf(float r, boolean stickCap) {
        final float k = stickCap ? STICK_KNOB : KNOB;
        return (k + 2.5f) * r / WELL;
    }

    void stickWell(Canvas c, float cx, float cy, float r, boolean active) {
        enter(c, cx, cy, r / WELL, false);

        // Clean dark translucent well backing
        tint(fill, 0x050810, active ? 0.30f : 0.20f, active);
        c.drawCircle(40f, 40f, WELL, fill);

        // Well boundary circle line
        tint(line, active ? 0xFFFFFF : 0xE6EDF5, active ? 1.0f : 0.85f, active);
        line.setStrokeWidth(active ? 2.5f : 1.8f);
        c.drawCircle(40f, 40f, WELL - 1.2f, line);
        c.restore();
    }

    /** The knob centred on cx, cy, for a stick of radius r. */
    void stickKnob(Canvas c, float cx, float cy, float r, boolean active) {
        c.save();
        c.translate(cx, cy);
        c.scale(r / WELL, r / WELL);

        // Knob backing
        tint(fill, 0x050810, active ? 0.50f : 0.35f, active);
        c.drawCircle(0f, 0f, KNOB, fill);
        if (active) {
            tint(fill, 0xFFFFFF, 0.25f, true);
            c.drawCircle(0f, 0f, KNOB, fill);
        }

        // Knob circle line
        tint(line, active ? 0xFFFFFF : 0xE6EDF5, 1f, active);
        line.setStrokeWidth(active ? 2.6f : 2.0f);
        c.drawCircle(0f, 0f, KNOB - 1f, line);

        // Knob center dot
        tint(fill, 0xFFFFFF, active ? 1.0f : 0.85f, active);
        c.drawCircle(0f, 0f, 3.2f, fill);

        c.restore();
    }

    /** Modern D-pad: 4 separate cross arrow buttons with clean thin circle lines and clear spacing. */
    void pad(Canvas c, float cx, float cy, float r, int heldBits) {
        enter(c, cx, cy, r / R, false);

        final float C = 40f;
        final float dist = 24.5f;
        final float btnR = 11.5f;

        final int[] bits = {TouchControlsView.UP, TouchControlsView.DOWN, TouchControlsView.LEFT, TouchControlsView.RIGHT};
        final float[][] dirs = {{0f, -1f}, {0f, 1f}, {-1f, 0f}, {1f, 0f}};

        for (int i = 0; i < 4; i++) {
            final float bx = C + dirs[i][0] * dist;
            final float by = C + dirs[i][1] * dist;
            final boolean lit = (heldBits & bits[i]) != 0;

            // Backing
            tint(fill, 0x050810, lit ? 0.35f : 0.22f, lit);
            c.drawCircle(bx, by, btnR, fill);
            if (lit) {
                tint(fill, 0xFFFFFF, 0.28f, true);
                c.drawCircle(bx, by, btnR - 0.7f, fill);
            }

            // Button rim (thin, clean circle line)
            tint(line, lit ? 0xFFFFFF : 0xE6EDF5, lit ? 1.0f : 0.85f, lit);
            line.setStrokeWidth(lit ? 1.8f : 1.3f);
            c.drawCircle(bx, by, btnR - 0.7f, line);

            // Chevron arrow
            drawChevron(c, bx, by, dirs[i][0], dirs[i][1], lit);
        }

        c.restore();
    }

    private void drawChevron(Canvas c, float cx, float cy, float dx, float dy, boolean lit) {
        tint(line, lit ? 0xFFFFFF : 0xE6EDF5, 1f, lit);
        line.setStrokeWidth(lit ? 1.9f : 1.5f);
        tri.reset();
        final float t = 4.6f;
        final float tx = cx + dx * t, ty = cy + dy * t;
        final float b1x = cx - dx * t * 0.4f - dy * t, b1y = cy - dy * t * 0.4f + dx * t;
        final float b2x = cx - dx * t * 0.4f + dy * t, b2y = cy - dy * t * 0.4f - dx * t;
        tri.moveTo(b1x, b1y);
        tri.lineTo(tx, ty);
        tri.lineTo(b2x, b2y);
        c.drawPath(tri, line);
    }

    /** Classic D-pad: one cross, the held arms lit, chevrons and a centre dot. */
    void classicPad(Canvas c, float cx, float cy, float r, int heldBits) {
        final Palette p = chrome();
        enter(c, cx, cy, r / 34f, false);
        shade(fill, p.base, 1f, false);
        c.drawPath(CROSS, fill);
        final boolean up = (heldBits & TouchControlsView.UP) != 0, down = (heldBits & TouchControlsView.DOWN) != 0;
        final boolean left = (heldBits & TouchControlsView.LEFT) != 0;
        final boolean right = (heldBits & TouchControlsView.RIGHT) != 0;
        tint(fill, 0xFFFFFF, 0.22f, true);
        if (up) c.drawPath(ARM_UP, fill);
        if (down) c.drawPath(ARM_DOWN, fill);
        if (left) c.drawPath(ARM_LEFT, fill);
        if (right) c.drawPath(ARM_RIGHT, fill);
        tint(line, p.ring, 1f, false);
        line.setStrokeWidth(2f);
        c.drawPath(CROSS, line);
        c.save();
        c.translate(40f, 40f);
        c.scale(0.9f, 0.9f);
        c.translate(-40f, -40f);
        tint(line, p.ringHi, 0.35f, false);
        line.setStrokeWidth(0.7f);
        c.drawPath(CROSS, line);
        c.restore();
        chevron(c, p, CHEV_UP, up);
        chevron(c, p, CHEV_DOWN, down);
        chevron(c, p, CHEV_LEFT, left);
        chevron(c, p, CHEV_RIGHT, right);
        tint(fill, p.ringHi, 1f, false);
        c.drawCircle(40f, 40f, 3f, fill);
        c.restore();
    }

    private void chevron(Canvas c, Palette p, Path path, boolean lit) {
        tint(line, lit ? 0xFFFFFF : p.ringHi, 1f, lit);
        line.setStrokeWidth(lit ? 3.0f : 2.2f);
        c.drawPath(path, line);
    }

    // --- Item Liquid (Jet-black glossy gamepad style: controller X/A/B/Y & sticks, curved L/R,
    //                  thin rounded rectangle utility buttons) -----------------------------------

    /** Classic controller color accent for X/A/B/Y in Item Liquid (0 = neutral jet-black liquid). */
    private static int itemLiquidFaceAccent(int bit) {
        switch (bit) {
            case TouchControlsView.A: return 0x4CD97B;  // A: gamepad green
            case TouchControlsView.B: return 0xFF5A5F;  // B: gamepad red
            case TouchControlsView.X: return 0x4DA3FF;  // X: gamepad blue
            case TouchControlsView.Y: return 0xFFC43D;  // Y: gamepad amber
            default: return 0;
        }
    }

    /** Builds the ergonomic curved L/R trigger silhouette: wider/fuller rounded bottom curving upward. */
    private void buildCurvedTriggerPath(Path p, float half) {
        final float left = 40f - half;
        final float right = 40f + half;
        final float top = 7f;
        final float bot = 73f;
        final float midY = 40f;
        p.reset();
        // Start along wide plush rounded bottom edge.
        p.moveTo(left + 18f, bot);
        p.lineTo(right - 16f, bot);
        // Rounded bottom-right corner rising gently into inner-right side.
        p.cubicTo(right - 2.5f, bot, right, bot - 11f, right - 1.5f, bot - 25f);
        p.cubicTo(right - 3f, top + 14f, right - 9f, top + 5f, right - 23f, top + 5f);
        // Smooth upward-curving upper shoulder ridge.
        p.cubicTo(40f + half * 0.22f, top + 1f, 40f - half * 0.10f, top - 1f, left + 29f, top + 6f);
        // Outer side sweeps outward & downward into the fuller, wider rounded bottom.
        p.cubicTo(left + 9f, top + 13f, left - 1f, midY + 5f, left + 1f, bot - 19f);
        p.cubicTo(left + 2f, bot - 6f, left + 7f, bot, left + 18f, bot);
        p.close();
    }

    /** Draws a physical gamepad-style black liquid disc for X, A, B, Y. */
    private void itemLiquidFaceDisc(Canvas c, int accentRgb, boolean down) {
        // Outer socket depth shadow.
        tint(fill, 0x000000, down ? 0.50f : 0.34f, down);
        c.drawCircle(40f, 41.4f, R + 1.0f, fill);

        // Recessed controller button bezel ring.
        tint(fill, 0x05070A, down ? 0.90f : 0.78f, down);
        c.drawCircle(40f, 40f, R, fill);

        // Raised piano-black glossy liquid cap.
        shade(fill, liquidBase, down ? 0.94f : 0.84f, down);
        c.drawCircle(40f, 40f, R - 2.0f, fill);

        // Upper liquid dome highlight.
        rect.set(40f - R * 0.74f, 40f - R + 3.6f, 40f + R * 0.74f, 40f + R * 0.04f);
        shade(fill, liquidTopSheen, down ? 0.88f : 0.78f, down);
        c.drawOval(rect, fill);

        // Delicate bottom caustic reflection clipped inside cap.
        c.save();
        rect.set(40f - R + 2.2f, 40f - R + 2.2f, 40f + R - 2.2f, 40f + R - 2.2f);
        tri.reset();
        tri.addOval(rect, Path.Direction.CW);
        c.clipPath(tri);
        shade(fill, liquidBottomBounce, down ? 0.95f : 0.76f, down);
        c.drawCircle(40f, 40f, R - 2.2f, fill);
        c.restore();

        // Press glow.
        if (down) {
            if (accentRgb != 0) {
                tint(fill, accentRgb, 0.32f, true);
                c.drawCircle(40f, 40f, R - 2.2f, fill);
            } else {
                shade(fill, liquidPressGlow, 1f, true);
                c.drawCircle(40f, 40f, R - 2.2f, fill);
            }
        }

        // Outer metallic/glass refractive bezel rim.
        if (down) {
            tint(line, accentRgb != 0 ? accentRgb : 0xFFFFFF, 0.98f, true);
            line.setStrokeWidth(2.6f);
        } else {
            shade(line, liquidRim, 0.95f, false);
            line.setStrokeWidth(1.8f);
        }
        c.drawCircle(40f, 40f, R - 1.1f, line);
        line.setShader(null);

        // Inner controller cap ring with subtle face-button accent tint.
        if (accentRgb != 0) {
            tint(line, accentRgb, down ? 0.85f : 0.42f, down);
        } else {
            tint(line, 0xFFFFFF, down ? 0.40f : 0.18f, down);
        }
        line.setStrokeWidth(1.15f);
        c.drawCircle(40f, 40f, R - 4.2f, line);
    }

    /** Crisp icon fill for thin rounded utility buttons in Item Liquid style. */
    private void liquidGlyph(Canvas c, Path glyph, float size, boolean down) {
        final float k = size / TouchGlyphs.BOX;
        c.save();
        c.translate(40f - size / 2f, 40f - size / 2f + 1.4f);
        c.scale(k, k);
        tint(fill, 0x000000, down ? 0.62f : 0.52f, down);
        c.drawPath(glyph, fill);
        c.restore();

        c.save();
        c.translate(40f - size / 2f, 40f - size / 2f);
        c.scale(k, k);
        if (down) {
            tint(fill, 0xFFFFFF, 1f, true);
        } else {
            shade(fill, liquidGlyph, 0.96f, false);
        }
        c.drawPath(glyph, fill);
        fill.setShader(null);
        c.restore();
    }

    private void liquidLabel(Canvas c, String label, float centreY, float size, int tintRgb, boolean down) {
        text.setTypeface(fontLiquid);
        text.setTextSize(size);
        final float y = centreY - (text.descent() + text.ascent()) / 2f;
        tint(text, 0x000000, 0.70f, down);
        c.drawText(label, 40f, y + 1.6f, text);
        if (down) {
            tint(text, 0xFFFFFF, 1f, true);
        } else if (tintRgb != 0) {
            tint(text, tintRgb, 0.96f, false);
        } else {
            shade(text, liquidLetter, 0.96f, false);
        }
        c.drawText(label, 40f, y, text);
        text.setShader(null);
        text.setTypeface(fontCondensed);
    }

    /** Item Liquid X, A, B, Y face button: glossy black controller cap with crisp gamepad letter. */
    void itemLiquidFaceButton(Canvas c, int bit, String letter, float cx, float cy, float r, boolean down) {
        enter(c, cx, cy, r / R, down);
        final int accent = itemLiquidFaceAccent(bit);
        itemLiquidFaceDisc(c, accent, down);
        if (letter != null) {
            liquidLabel(c, letter, 40f, 0.46f * D, accent != 0 ? blendTowardWhite(accent, 0.55f) : 0, down);
        }
        c.restore();
    }

    private static int blendTowardWhite(int rgb, float t) {
        final int r = Math.min(255, Math.round(((rgb >> 16) & 0xFF) + (255 - ((rgb >> 16) & 0xFF)) * t));
        final int g = Math.min(255, Math.round(((rgb >> 8) & 0xFF) + (255 - ((rgb >> 8) & 0xFF)) * t));
        final int b = Math.min(255, Math.round((rgb & 0xFF) + (255 - (rgb & 0xFF)) * t));
        return (r << 16) | (g << 8) | b;
    }

    /**
     * Item Liquid L / R curved trigger: ergonomic rounded bumper/trigger shape that is wider and fuller
     * at the bottom and curves smoothly upward (mirrored symmetrically for L vs R).
     */
    void itemLiquidTrigger(Canvas c, String label, float cx, float cy, float hw, float hh,
            boolean down, boolean rightSide) {
        enter(c, cx, cy, hh / R, down);
        final float half = R * hw / hh;
        buildCurvedTriggerPath(triggerPath, half);

        c.save();
        if (rightSide) {
            c.scale(-1f, 1f, 40f, 40f);
        }

        // Outer depth shadow.
        c.save();
        c.translate(0f, 1.8f);
        tint(fill, 0x000000, down ? 0.44f : 0.30f, down);
        c.drawPath(triggerPath, fill);
        c.restore();

        // Glossy jet-black obsidian liquid body.
        shade(fill, liquidBase, down ? 0.92f : 0.78f, down);
        c.drawPath(triggerPath, fill);

        // Clipped upper liquid sheen, bottom bounce reflection, and press glow.
        c.save();
        c.clipPath(triggerPath);
        rect.set(40f - half + 4f, 6f, 40f + half - 4f, 40f);
        shade(fill, liquidTopSheen, down ? 0.90f : 0.78f, down);
        c.drawOval(rect, fill);

        shade(fill, liquidBottomBounce, down ? 1f : 0.80f, down);
        c.drawRect(40f - half, 48f, 40f + half, 75f, fill);

        if (down) {
            shade(fill, liquidPressGlow, 1f, true);
            c.drawPath(triggerPath, fill);
        }
        c.restore();

        // Refractive outer glass rim.
        if (down) {
            tint(line, 0xFFFFFF, 0.98f, true);
            line.setStrokeWidth(2.6f);
        } else {
            shade(line, liquidRim, 0.96f, false);
            line.setStrokeWidth(1.9f);
        }
        c.drawPath(triggerPath, line);
        line.setShader(null);

        // Subtle inner bevel line.
        c.save();
        c.translate(40f, 40f);
        c.scale(0.94f, 0.90f);
        c.translate(-40f, -40f);
        tint(line, 0xFFFFFF, down ? 0.34f : 0.16f, down);
        line.setStrokeWidth(0.9f);
        c.drawPath(triggerPath, line);
        c.restore();

        c.restore();

        // Upright controller label (L or R) optically centered inside the wider lower body.
        if (label != null) {
            liquidLabel(c, label, 43f, 0.42f * D, 0, down);
        }
        c.restore();
    }

    /**
     * Item Liquid thin rounded rectangle for utility buttons (ZL, ZR, −, +, HOME, Menu gear).
     */
    void itemLiquidThinRect(Canvas c, int bit, String label, Path glyph, float cx, float cy,
            float hw, float hh, boolean down) {
        enter(c, cx, cy, hh / R, down);
        final float half = Math.max(R, R * hw / hh);
        final float corner = 18f;

        // Soft outer depth shadow.
        cell.set(40f - half - 0.6f, 4.4f, 40f + half + 0.6f, 77.6f);
        tint(fill, 0x000000, down ? 0.42f : 0.28f, down);
        c.drawRoundRect(cell, corner, corner, fill);

        // Jet-black obsidian liquid rounded rectangle body.
        cell.set(40f - half, 3f, 40f + half, 77f);
        shade(fill, liquidBase, down ? 0.92f : 0.78f, down);
        c.drawRoundRect(cell, corner, corner, fill);

        // Upper liquid glass sheen strip.
        rect.set(40f - half + 4f, 5.5f, 40f + half - 4f, 38f);
        shade(fill, liquidTopSheen, down ? 0.88f : 0.74f, down);
        c.drawRoundRect(rect, corner * 0.65f, corner * 0.65f, fill);

        // Bottom caustic reflection clipped inside box.
        c.save();
        tri.reset();
        rect.set(40f - half + 1.5f, 4.5f, 40f + half - 1.5f, 75.5f);
        tri.addRoundRect(rect, corner - 1.5f, corner - 1.5f, Path.Direction.CW);
        c.clipPath(tri);
        shade(fill, liquidBottomBounce, down ? 0.95f : 0.78f, down);
        c.drawRoundRect(rect, corner - 1.5f, corner - 1.5f, fill);
        if (down) {
            shade(fill, liquidPressGlow, 1f, true);
            c.drawRoundRect(rect, corner - 1.5f, corner - 1.5f, fill);
        }
        c.restore();

        // Crisp thin refractive glass rim.
        cell.set(40f - half + 1.2f, 4.2f, 40f + half - 1.2f, 75.8f);
        if (down) {
            tint(line, 0xFFFFFF, 0.98f, true);
            line.setStrokeWidth(2.6f);
        } else {
            shade(line, liquidRim, 0.94f, false);
            line.setStrokeWidth(1.9f);
        }
        c.drawRoundRect(cell, corner - 1.0f, corner - 1.0f, line);
        line.setShader(null);

        // Inner hairline bevel.
        cell.inset(2.4f, 2.4f);
        tint(line, 0xFFFFFF, down ? 0.34f : 0.14f, down);
        line.setStrokeWidth(0.85f);
        c.drawRoundRect(cell, Math.max(4f, corner - 3.2f), Math.max(4f, corner - 3.2f), line);

        // Content: Home outline, glyph (Menu gear), or crisp label (ZL, ZR, −, +).
        if (bit == TouchControlsView.HOME) {
            c.save();
            c.translate(40f - 0.28f * D, 40f - 0.28f * D);
            c.scale(0.56f * D / 24f, 0.56f * D / 24f);
            tint(line, down ? 0xFFFFFF : 0xEEF3FA, down ? 1f : 0.94f, down);
            line.setStrokeWidth(2.2f);
            c.drawPath(HOUSE, line);
            c.restore();
        } else if (glyph != null) {
            liquidGlyph(c, glyph, 0.58f * D, down);
        } else if (label != null) {
            final float fontSize = label.length() > 1 ? 0.44f * D : 0.56f * D;
            liquidLabel(c, label, 40f, fontSize, 0, down);
        }
        c.restore();
    }

    /** Item Liquid D-pad: physical black glossy controller rounded cross with recessed center pivot. */
    void itemLiquidPad(Canvas c, float cx, float cy, float r, int heldBits) {
        enter(c, cx, cy, r / 34f, false);
        final boolean up = (heldBits & TouchControlsView.UP) != 0;
        final boolean down = (heldBits & TouchControlsView.DOWN) != 0;
        final boolean left = (heldBits & TouchControlsView.LEFT) != 0;
        final boolean right = (heldBits & TouchControlsView.RIGHT) != 0;

        c.save();
        c.translate(0f, 1.4f);
        tint(fill, 0x000000, 0.32f, false);
        c.drawPath(LIQUID_CROSS, fill);
        c.restore();

        shade(fill, liquidBase, 0.78f, false);
        c.drawPath(LIQUID_CROSS, fill);

        c.save();
        c.clipPath(LIQUID_CROSS);
        shade(fill, liquidTopSheen, 0.76f, false);
        c.drawRect(6f, 6f, 74f, 42f, fill);
        shade(fill, liquidBottomBounce, 0.80f, false);
        c.drawRect(6f, 48f, 74f, 74f, fill);

        tint(fill, 0xFFFFFF, 0.28f, true);
        if (up) c.drawPath(LIQUID_ARM_UP, fill);
        if (down) c.drawPath(LIQUID_ARM_DOWN, fill);
        if (left) c.drawPath(LIQUID_ARM_LEFT, fill);
        if (right) c.drawPath(LIQUID_ARM_RIGHT, fill);
        c.restore();

        shade(line, liquidRim, 0.96f, false);
        line.setStrokeWidth(1.8f);
        c.drawPath(LIQUID_CROSS, line);
        line.setShader(null);

        c.save();
        c.translate(40f, 40f);
        c.scale(0.92f, 0.92f);
        c.translate(-40f, -40f);
        tint(line, 0xFFFFFF, 0.14f, false);
        line.setStrokeWidth(0.75f);
        c.drawPath(LIQUID_CROSS, line);
        c.restore();

        liquidChevron(c, CHEV_UP, up);
        liquidChevron(c, CHEV_DOWN, down);
        liquidChevron(c, CHEV_LEFT, left);
        liquidChevron(c, CHEV_RIGHT, right);

        tint(fill, 0x000000, 0.38f, false);
        c.drawCircle(40f, 40f, 6.4f, fill);
        tint(line, 0xFFFFFF, 0.22f, false);
        line.setStrokeWidth(0.9f);
        c.drawCircle(40f, 40f, 6.4f, line);
        c.restore();
    }

    private void liquidChevron(Canvas c, Path path, boolean lit) {
        tint(line, lit ? 0xFFFFFF : 0xD8E2F0, lit ? 1f : 0.82f, lit);
        line.setStrokeWidth(lit ? 2.8f : 2.1f);
        c.drawPath(path, line);
    }

    /** Item Liquid analog stick socket: recessed dark gamepad gate well with subtle cardinal notches. */
    void itemLiquidStickWell(Canvas c, float cx, float cy, float r, boolean active) {
        enter(c, cx, cy, r / WELL, false);

        // Soft outer shadow.
        tint(fill, 0x000000, active ? 0.34f : 0.24f, active);
        c.drawCircle(40f, 41.3f, WELL + 0.7f, fill);

        // Deep recessed controller socket.
        shade(fill, liquidWellBase, active ? 0.90f : 0.76f, active);
        c.drawCircle(40f, 40f, WELL, fill);

        // Subtle inner machined lip shadow.
        tint(line, 0x000000, 0.42f, active);
        line.setStrokeWidth(3.2f);
        c.drawCircle(40f, 40f, WELL - 2.8f, line);

        // Soft top glass sheen along upper socket edge.
        rect.set(40f - WELL * 0.74f, 40f - WELL + 2.8f, 40f + WELL * 0.74f, 40f - WELL * 0.08f);
        shade(fill, liquidTopSheen, active ? 0.46f : 0.34f, active);
        c.drawOval(rect, fill);

        // Refractive outer ring.
        if (active) {
            tint(line, 0xFFFFFF, 0.90f, true);
            line.setStrokeWidth(2.2f);
        } else {
            shade(line, liquidRim, 0.92f, false);
            line.setStrokeWidth(1.75f);
        }
        c.drawCircle(40f, 40f, WELL - 1.1f, line);
        line.setShader(null);

        // Fine inner bezel + 4 subtle controller cardinal ticks.
        tint(line, 0xFFFFFF, active ? 0.28f : 0.14f, active);
        line.setStrokeWidth(0.8f);
        c.drawCircle(40f, 40f, WELL - 4.2f, line);
        tint(line, 0xFFFFFF, active ? 0.50f : 0.26f, active);
        line.setStrokeWidth(1.3f);
        final float t0 = WELL - 7.4f, t1 = WELL - 4.6f;
        c.drawLine(40f, 40f - t0, 40f, 40f - t1, line);
        c.drawLine(40f, 40f + t0, 40f, 40f + t1, line);
        c.drawLine(40f - t0, 40f, 40f - t1, 40f, line);
        c.drawLine(40f + t0, 40f, 40f + t1, 40f, line);

        c.restore();
    }

    /** Item Liquid analog thumbstick cap: sculpted 3D black gamepad stick with concave dish & grip rim. */
    void itemLiquidStickKnob(Canvas c, float cx, float cy, float r, boolean active) {
        c.save();
        c.translate(cx, cy);
        c.scale(r / WELL, r / WELL);

        // Contact shadow cast onto the recessed socket.
        tint(fill, 0x000000, active ? 0.56f : 0.44f, active);
        c.drawCircle(0f, 2.0f, STICK_KNOB + 1.0f, fill);

        // Outer sculpted black controller cap collar.
        shade(fill, liquidKnobBase, active ? 0.96f : 0.88f, active);
        c.drawCircle(0f, 0f, STICK_KNOB, fill);

        // Curved liquid gloss along upper collar edge.
        rect.set(-STICK_KNOB * 0.78f, -STICK_KNOB + 1.2f, STICK_KNOB * 0.78f, STICK_KNOB * 0.05f);
        shade(fill, liquidKnobSheen, active ? 0.96f : 0.84f, active);
        c.drawOval(rect, fill);

        // Recessed concave thumb dish (darker at top, catching light at bottom like a real stick cap).
        shade(fill, liquidKnobDish, active ? 0.98f : 0.92f, active);
        c.drawCircle(0f, 0f, STICK_KNOB * 0.68f, fill);

        if (active) {
            tint(fill, 0xFFFFFF, 0.14f, true);
            c.drawCircle(0f, 0f, STICK_KNOB - 1f, fill);
        }

        // Tactile raised grip ridge separating outer rubber rim and concave center dish.
        tint(line, 0x000000, 0.55f, active);
        line.setStrokeWidth(1.6f);
        c.drawCircle(0f, 0f, STICK_KNOB * 0.68f + 0.6f, line);
        tint(line, 0xFFFFFF, active ? 0.48f : 0.28f, active);
        line.setStrokeWidth(1.0f);
        c.drawCircle(0f, 0f, STICK_KNOB * 0.68f, line);

        // Outer refractive thumbstick cap rim.
        if (active) {
            tint(line, 0xFFFFFF, 0.98f, true);
            line.setStrokeWidth(2.2f);
        } else {
            shade(line, liquidKnobRim, 0.95f, false);
            line.setStrokeWidth(1.65f);
        }
        c.drawCircle(0f, 0f, STICK_KNOB - 0.9f, line);
        line.setShader(null);

        c.restore();
    }

    /** SVG path data, absolute commands only (M L H V C Q A Z), as used by TouchGlyphs. */
    static Path svg(String data) {
        final Path p = new Path();
        final String[] tok = data.trim().replace(",", " ").split("\\s+");
        int i = 0;
        float x = 0, y = 0, startX = 0, startY = 0;
        char cmd = 'M';
        while (i < tok.length) {
            final char first = tok[i].charAt(0);
            if (Character.isLetter(first)) {
                cmd = first;
                ++i;
                if (cmd == 'Z') {
                    p.close();
                    x = startX;
                    y = startY;
                    continue;
                }
            }
            switch (cmd) {
                case 'M':
                    x = f(tok[i++]); y = f(tok[i++]);
                    p.moveTo(x, y);
                    startX = x; startY = y;
                    cmd = 'L';  // further pairs are lines
                    break;
                case 'L':
                    x = f(tok[i++]); y = f(tok[i++]);
                    p.lineTo(x, y);
                    break;
                case 'H':
                    x = f(tok[i++]);
                    p.lineTo(x, y);
                    break;
                case 'V':
                    y = f(tok[i++]);
                    p.lineTo(x, y);
                    break;
                case 'C': {
                    final float x1 = f(tok[i++]), y1 = f(tok[i++]), x2 = f(tok[i++]), y2 = f(tok[i++]);
                    x = f(tok[i++]); y = f(tok[i++]);
                    p.cubicTo(x1, y1, x2, y2, x, y);
                    break;
                }
                case 'Q': {
                    final float x1 = f(tok[i++]), y1 = f(tok[i++]);
                    x = f(tok[i++]); y = f(tok[i++]);
                    p.quadTo(x1, y1, x, y);
                    break;
                }
                case 'A': {
                    final float rx = f(tok[i++]), ry = f(tok[i++]);
                    final float rot = f(tok[i++]);
                    final boolean large = f(tok[i++]) != 0, sweep = f(tok[i++]) != 0;
                    final float ex = f(tok[i++]), ey = f(tok[i++]);
                    arc(p, x, y, rx, ry, rot, large, sweep, ex, ey);
                    x = ex; y = ey;
                    break;
                }
                default:
                    throw new IllegalArgumentException("svg command " + cmd);
            }
        }
        return p;
    }

    private static float f(String s) { return Float.parseFloat(s); }

    /** SVG endpoint arc to Path.arcTo (circles and unrotated ellipses). */
    private static void arc(Path p, float x0, float y0, float rx, float ry, float rotation,
            boolean large, boolean sweep, float x1, float y1) {
        if (rx == 0 || ry == 0) { p.lineTo(x1, y1); return; }
        final double dx = (x0 - x1) / 2.0, dy = (y0 - y1) / 2.0;
        double rxs = rx, rys = ry;
        final double lambda = (dx * dx) / (rxs * rxs) + (dy * dy) / (rys * rys);
        if (lambda > 1) { rxs *= Math.sqrt(lambda); rys *= Math.sqrt(lambda); }
        double num = rxs * rxs * rys * rys - rxs * rxs * dy * dy - rys * rys * dx * dx;
        final double den = rxs * rxs * dy * dy + rys * rys * dx * dx;
        double coef = den == 0 ? 0 : Math.sqrt(Math.max(0, num / den));
        if (large == sweep) coef = -coef;
        final double cxp = coef * rxs * dy / rys, cyp = -coef * rys * dx / rxs;
        final double cx = cxp + (x0 + x1) / 2.0, cy = cyp + (y0 + y1) / 2.0;
        final double start = Math.atan2((dy - cyp) / rys, (dx - cxp) / rxs);
        final double end = Math.atan2((-dy - cyp) / rys, (-dx - cxp) / rxs);
        double delta = end - start;
        if (sweep && delta < 0) delta += 2 * Math.PI;
        if (!sweep && delta > 0) delta -= 2 * Math.PI;
        final RectF oval = new RectF((float) (cx - rxs), (float) (cy - rys), (float) (cx + rxs), (float) (cy + rys));
        p.arcTo(oval, (float) Math.toDegrees(start), (float) Math.toDegrees(delta), false);
    }
}
