package com.shiinobu.re4;

import android.graphics.Matrix;
import android.graphics.Path;

/**
 * Silhouettes for the touch controls, in a 512x512 box (fill, non-zero winding: an outline runs
 * clockwise on screen, a hole the other way). One picture per RE4 Classic Controller action.
 *
 * <p>AIM, MENU and HOME are from game-icons.net by Lorc, Delapouite and contributors, licensed CC BY
 * 3.0 (https://creativecommons.org/licenses/by/3.0/): crosshair, cog, house, converted to absolute
 * path data with the shapes unchanged. The others are plain shapes drawn for this app.
 */
final class TouchGlyphs {
    private TouchGlyphs() {}

    static final float BOX = 512f;

    /** A: action / fire. A muzzle-flash burst. */
    static final Path FIRE = star(8, 230f, 92f);

    /** B: run (hold while moving). Two forward chevrons. */
    static final Path RUN = TouchIcons.svg("M 52 104 L 246 256 L 52 408 L 52 330 L 146 256 L 52 182 Z "
            + "M 252 104 L 446 256 L 252 408 L 252 330 L 346 256 L 252 182 Z");

    /** X: Ashley's commands (wait / follow). A head and shoulders. */
    static final Path ASHLEY = person();

    /** Y and −: the inventory. The attaché case, with its handle, lid line and two latches. */
    static final Path CASE = TouchIcons.svg("M 82 172 L 430 172 Q 456 172 456 198 L 456 414 Q 456 440 430 440 "
            + "L 82 440 Q 56 440 56 414 L 56 198 Q 56 172 82 172 Z "
            + "M 56 266 L 456 266 L 456 248 L 56 248 Z "
            + "M 146 230 L 190 230 L 190 284 L 146 284 Z "
            + "M 322 230 L 366 230 L 366 284 L 322 284 Z "
            + "M 194 172 L 194 120 Q 194 98 216 98 L 296 98 Q 318 98 318 120 L 318 172 Z "
            + "M 222 172 L 290 172 L 290 126 L 222 126 Z");

    /** L: the knife, the point up and to the right. */
    static final Path KNIFE = rotated(TouchIcons.svg("M 256 26 C 302 92 292 160 292 326 L 220 326 L 220 140 "
            + "C 220 98 234 60 256 26 Z "
            + "M 180 326 L 332 326 L 332 356 L 180 356 Z "
            + "M 226 356 L 286 356 L 286 472 Q 286 486 272 486 L 240 486 Q 226 486 226 472 Z"), 45f);

    /** ZL / ZR: the map. A map folded in three. */
    static final Path MAP = TouchIcons.svg("M 36 124 L 180 72 L 180 392 L 36 444 Z "
            + "M 196 72 L 316 124 L 316 444 L 196 392 Z "
            + "M 332 124 L 476 72 L 476 392 L 332 444 Z");

    /** +: the options screen. Three sliders. */
    static final Path OPTIONS = sliders();

    /** R: aim (ready the weapon). A crosshair. */
    static final Path AIM = TouchIcons.svg("M 247 32 L 247 55.2 C 143.2 59.8 59.8 143.2 55.2 247 L 32 247 L 32 265 L 55.2 265 C 59.8 368.8 143.2"
            + " 452.2 247 456.8 L 247 480 L 265 480 L 265 456.8 C 368.8 452.2 452.2 368.8 456.8 265 L 480 265 L 480"
            + " 247 L 456.8 247 C 452.2 143.2 368.8 59.8 265 55.2 L 265 32 Z M 247 73.2 L 247 128 L 265 128 L 265 7"
            + "3.2 C 359 77.8 434.2 153 438.8 247 L 384 247 L 384 265 L 438.8 265 C 434.2 359 359 434.2 265 438.8 L"
            + " 265 384 L 247 384 L 247 438.8 C 153 434.2 77.8 359 73.2 265 L 128 265 L 128 247 L 73.2 247 C 77.8 1"
            + "53 153 77.8 247 73.2 M 247 224 L 247 247 L 224 247 L 224 265 L 247 265 L 247 288 L 265 288 L 265 265"
            + " L 288 265 L 288 247 L 265 247 L 265 224 Z");

    /** The launcher's menu (Back while playing). A cog. */
    static final Path MENU = TouchIcons.svg("M 234.9 18.8 C 208.8 21.1 183.3 27.3 160 37.6 C 175.4 70.3 174.2 104.3 153.3 120 C 132.4 135.6 99.2 "
            + "127.9 72.2 104.3 C 55.9 123.7 42.8 145.6 33.3 169.9 C 67.1 182.2 89.9 207.6 86.2 233.4 C 82.5 259.2 "
            + "53.5 277.1 17.6 279.5 C 19.9 305.3 26.5 330.3 36.5 353.4 C 54.3 345.2 72.3 342.3 87.6 344.4 C 100.7 "
            + "346.3 111.6 351.5 118.8 361.1 C 134.6 382.2 126.4 415.8 102 443.1 C 121.5 459.4 144.2 472.5 168.6 48"
            + "2.1 C 180.9 448.3 206.3 425.5 232.1 429.2 C 257.9 432.9 275.8 461.9 278.2 497.8 C 304.3 495.4 329.7 "
            + "489.2 353.1 479 C 337.3 446 338.8 411.4 359.9 395.6 C 369.5 388.4 382.5 386.6 395.5 388.5 C 410.6 39"
            + "0.6 426.7 398.7 441.1 411.2 C 457.2 391.9 470.5 369.7 479.9 345.7 C 446.1 333.3 423.3 308 427 282.2 "
            + "C 430.7 256.3 459.7 238.4 495.6 236 C 493.2 210.1 486.8 185.4 476.7 162.2 C 444.1 177.3 410 175.3 39"
            + "4.4 154.5 C 378.7 133.5 387.2 100.6 411.1 73.5 C 391.6 57.1 368.8 44 344.5 34.4 C 332.1 68.3 306.8 9"
            + "1.1 281 87.4 C 255.1 83.7 237.2 54.7 234.9 18.8 Z M 256.5 113.9 C 336.2 113.9 400.9 178.7 400.9 258."
            + "3 C 400.9 337.9 336.2 402.7 256.5 402.7 C 176.9 402.7 112.2 337.9 112.2 258.3 C 112.2 178.7 176.9 11"
            + "3.9 256.5 113.9 Z M 256.5 132.6 C 187 132.6 130.8 188.8 130.8 258.3 C 130.8 327.8 187 384 256.5 384 "
            + "C 326.1 384 382.2 327.8 382.2 258.3 C 382.2 188.8 326.1 132.6 256.5 132.6 Z M 256.6 147.7 C 317.7 14"
            + "7.7 367.2 197.2 367.2 258.3 C 367.2 319.4 317.7 369 256.6 369 C 195.5 369 145.9 319.4 145.9 258.3 C "
            + "145.9 197.2 195.5 147.7 256.6 147.7 Z");
    /** Home: the Wii's HOME button. A house. */
    static final Path HOME = TouchIcons.svg("M 256 19.3 L 25.6 249.6 L 19.3 256 L 32 268.7 L 38.4 262.4 L 256 44.7 L 473.6 262.4 L 480 268.7 L 49"
            + "2.7 256 L 486.4 249.6 Z M 96 48 L 96 155.3 L 160 91.3 L 160 48 Z M 256 68.7 L 64 260.7 L 64 486 L 12"
            + "8 486 L 128 320 L 224 320 L 224 486 L 448 486 L 448 260.7 Z M 288 320 L 384 320 L 384 400 L 288 400 "
            + "Z");

    /** A `points`-pointed star around the box centre, clockwise on screen. */
    private static Path star(int points, float outer, float inner) {
        final Path p = new Path();
        for (int i = 0; i < points * 2; ++i) {
            final double a = Math.PI * i / points - Math.PI / 2;
            final float r = (i % 2 == 0) ? outer : inner;
            final float x = 256f + (float) (Math.cos(a) * r), y = 256f + (float) (Math.sin(a) * r);
            if (i == 0) p.moveTo(x, y);
            else p.lineTo(x, y);
        }
        p.close();
        return p;
    }

    private static Path person() {
        final Path p = new Path();
        p.addCircle(256f, 146f, 86f, Path.Direction.CW);
        p.moveTo(84f, 468f);
        p.cubicTo(84f, 336f, 164f, 254f, 256f, 254f);
        p.cubicTo(348f, 254f, 428f, 336f, 428f, 468f);
        p.close();
        return p;
    }

    private static Path sliders() {
        final Path p = new Path();
        final float[] rows = {128f, 256f, 384f};
        final float[] knobs = {330f, 178f, 372f};
        for (int i = 0; i < rows.length; ++i) {
            p.addRoundRect(56f, rows[i] - 14f, 456f, rows[i] + 14f, 14f, 14f, Path.Direction.CW);
            p.addCircle(knobs[i], rows[i], 46f, Path.Direction.CW);
        }
        return p;
    }

    private static Path rotated(Path p, float degrees) {
        final Matrix m = new Matrix();
        m.setRotate(degrees, 256f, 256f);
        p.transform(m);
        return p;
    }
}
