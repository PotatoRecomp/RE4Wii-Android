package com.shiinobu.re4;

import android.view.InputDevice;
import android.view.MotionEvent;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * Settings > Controls > Right Stick Axes as SDL's joystick axis numbers. Generic pads report the right
 * stick on Z/RZ or on RX/RY, and SDL's guessed mapping can take the wrong pair. SDL numbers a pad's
 * axes in the order of SDLControllerManager.RangeComparator (hats left out), so the same pair has
 * different numbers on different pads: they are worked out here for the connected gamepad.
 */
final class StickAxes {
    static final int AUTO = 0;
    static final int Z_RZ = 1;
    static final int RX_RY = 2;

    private StickAxes() {}

    /** The sort key of SDLControllerManager.RangeComparator. */
    private static int sdlOrder(int axis) {
        if (axis == MotionEvent.AXIS_GAS) return MotionEvent.AXIS_BRAKE;
        if (axis == MotionEvent.AXIS_BRAKE) return MotionEvent.AXIS_GAS;
        if (axis == MotionEvent.AXIS_Z) return MotionEvent.AXIS_RZ - 1;
        if (axis > MotionEvent.AXIS_Z && axis < MotionEvent.AXIS_RZ) return axis - 1;
        return axis;
    }

    /** {x, y} on the first connected gamepad that has both axes; {-1, -1} for Auto or when none has them. */
    static int[] right(int mode) {
        final int[] none = {-1, -1};
        if (mode != Z_RZ && mode != RX_RY) return none;
        final int wantX = mode == Z_RZ ? MotionEvent.AXIS_Z : MotionEvent.AXIS_RX;
        final int wantY = mode == Z_RZ ? MotionEvent.AXIS_RZ : MotionEvent.AXIS_RY;
        for (int id : InputDevice.getDeviceIds()) {
            final InputDevice device = InputDevice.getDevice(id);
            if (device == null || (device.getSources() & InputDevice.SOURCE_CLASS_JOYSTICK) == 0) continue;
            final List<Integer> axes = new ArrayList<>();
            for (InputDevice.MotionRange range : device.getMotionRanges()) {
                if ((range.getSource() & InputDevice.SOURCE_CLASS_JOYSTICK) == 0) continue;
                final int axis = range.getAxis();
                if (axis == MotionEvent.AXIS_HAT_X || axis == MotionEvent.AXIS_HAT_Y) continue;
                axes.add(axis);
            }
            Collections.sort(axes, (a, b) -> Integer.compare(sdlOrder(a), sdlOrder(b)));
            final int x = axes.indexOf(wantX);
            final int y = axes.indexOf(wantY);
            if (x >= 0 && y >= 0) return new int[] {x, y};
        }
        return none;
    }
}
