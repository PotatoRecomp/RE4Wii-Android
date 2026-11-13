package com.shiinobu.re4;

import android.app.Activity;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.charset.Charset;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;

/**
 * Developer > Stage Select (builds with -Pre4Dev): jumps to any room's entry point from the disc's own
 * debug table, files/debug/roominfo.dat (Capcom's AREA JUMP list, room_jmp.cpp on the GameCube): u32
 * stage count, a u32 offset per stage, each stage block a u32 count and 0x20-byte records (u16 flag,
 * u16 stage << 8 | room, pos x y z, angle, then name / author string offsets), big-endian.
 */
final class StageSelect {
    private StageSelect() {}

    static final class Point {
        final int stage, room, point;
        final float x, y, z, angle;
        final String name;

        Point(int stage, int room, int point, float x, float y, float z, float angle, String name) {
            this.stage = stage;
            this.room = room;
            this.point = point;
            this.x = x;
            this.y = y;
            this.z = z;
            this.angle = angle;
            this.name = name;
        }
    }

    static List<Point> read(File file) throws IOException {
        final ByteBuffer d = ByteBuffer.wrap(Files.readAllBytes(file.toPath()));  // big-endian by default
        final List<Point> points = new ArrayList<>();
        final int stages = d.getInt(0);
        for (int s = 0; s < stages; s++) {
            final int off = d.getInt(4 + 4 * s);
            if (off == 0) continue;
            final int count = d.getInt(off);
            int lastRoom = -1, point = 0;
            for (int i = 0; i < count; i++) {
                final int r = off + 4 + i * 0x20;
                final boolean hasPos = (d.getShort(r) & 1) != 0;
                final int room = d.getShort(r + 2) & 0xFFFF;
                point = room == lastRoom ? point + 1 : 0;  // a room's points follow each other
                lastRoom = room;
                points.add(new Point(s, room, point, hasPos ? d.getFloat(r + 4) : 0f, hasPos ? d.getFloat(r + 8) : 0f,
                        hasPos ? d.getFloat(r + 12) : 0f, hasPos ? d.getFloat(r + 16) : 0f, string(d, d.getInt(r + 0x14))));
            }
        }
        return points;
    }

    private static String string(ByteBuffer d, int off) {
        if (off <= 0 || off >= d.limit()) return "";
        int end = off;
        while (end < d.limit() && d.get(end) != 0) end++;
        final byte[] bytes = new byte[end - off];
        for (int i = 0; i < bytes.length; i++) bytes[i] = d.get(off + i);
        return new String(bytes, Charset.forName("Shift_JIS")).trim();
    }

    /** Lists the stages' points; a tap asks for the jump, which the game makes at its next free-play frame. */
    static void show(Activity activity, File gameDir, Jump jump) {
        final List<Point> points;
        try {
            points = read(new File(gameDir, "files/debug/roominfo.dat"));
        } catch (IOException | RuntimeException e) {
            new Sheet(activity, Tr.t("Stage Select"), Tr.t("debug/roominfo.dat could not be read: ") + e.getMessage())
                    .action(Tr.t("Close"), null, true, false, null).show();
            return;
        }
        final Sheet sheet = new Sheet(activity, Tr.t("Stage Select"),
                Tr.t("Developer only. Jumps from free play; the story flags stay as they are."));
        final LinearLayout list = new LinearLayout(activity);
        list.setOrientation(LinearLayout.VERTICAL);
        int lastStage = -1;
        for (Point p : points) {
            if (p.stage != lastStage) {
                lastStage = p.stage;
                list.addView(Ui.text(activity, "Stage " + p.stage, 13, Ui.SECONDARY), Ui.matchWidth(Ui.dp(activity, 8)));
            }
            final TextView row = Ui.text(activity, String.format("%03X.%d  %s", p.room, p.point,
                    p.name.isEmpty() ? "-" : p.name), 13, Ui.TEXT);
            row.setPadding(Ui.dp(activity, 10), Ui.dp(activity, 8), Ui.dp(activity, 10), Ui.dp(activity, 8));
            row.setBackground(Ui.touchBox(activity, Ui.BUTTON, 0, 0));
            row.setClickable(true);
            row.setOnClickListener(v -> {
                sheet.dismiss();
                jump.go(p);
            });
            list.addView(row, Ui.matchWidth(Ui.dp(activity, 2)));
        }
        final ScrollView scroll = new ScrollView(activity);
        scroll.addView(list);
        sheet.view(scroll).action(Tr.t("Cancel"), null, false, false, null).show();
    }

    interface Jump { void go(Point point); }
}
