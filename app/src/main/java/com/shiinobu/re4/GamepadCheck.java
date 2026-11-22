package com.shiinobu.re4;

import android.app.Activity;
import android.content.Context;
import android.hardware.input.InputManager;
import android.os.Handler;
import android.os.Looper;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.widget.TextView;

/**
 * Settings > Controls > Check Gamepad: the input devices Android sees (refreshed as they connect),
 * and the last button or stick move with the device it came from. Any gamepad drives Leon
 * (VirtualRemote::Sample merges every port into player 1), so this answers "is it connected and
 * does Android get its buttons"; console.log has the game's side ("gamepads: ...").
 */
final class GamepadCheck {
    private GamepadCheck() {}

    static void show(Activity activity) {
        final Sheet sheet = new Sheet(activity, Tr.t("Check Gamepad"),
                Tr.t("Press any button or move a stick. Every connected gamepad controls Leon."));
        final TextView devices = Ui.text(activity, "", 13, Ui.TEXT);
        final TextView last = Ui.text(activity, Tr.t("Waiting for a button…"), 15, Ui.TEXT);
        final TextView hint = Ui.text(activity, Tr.t("Move both sticks. If a stick shows nothing here, the gamepad sends no analog values: switch it to its analog or X-input mode (often a Mode, Analog or Home + X button)."), 13, Ui.TEXT);
        sheet.view(devices).view(last).view(hint);
        devices.setText(listDevices());

        final InputManager input = (InputManager) activity.getSystemService(Context.INPUT_SERVICE);
        final InputManager.InputDeviceListener listener = new InputManager.InputDeviceListener() {
            @Override public void onInputDeviceAdded(int id) { devices.setText(listDevices()); }
            @Override public void onInputDeviceRemoved(int id) { devices.setText(listDevices()); }
            @Override public void onInputDeviceChanged(int id) { devices.setText(listDevices()); }
        };
        if (input != null) input.registerInputDeviceListener(listener, new Handler(Looper.getMainLooper()));

        sheet.onGamepad((dialog, keyCode, event) -> {
            if (keyCode == KeyEvent.KEYCODE_BACK && !isGamepad(event.getDevice())) return false;
            if (event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0) {
                last.setText(String.format(Tr.t("%1$s on %2$s"),
                        KeyEvent.keyCodeToString(keyCode).replace("KEYCODE_", ""), name(event.getDevice())));
            }
            return true;
        }, event -> {
            if (event.getAction() != MotionEvent.ACTION_MOVE || !isGamepad(event.getDevice())) return false;
            final String moved = strongestAxis(event);
            if (moved != null) last.setText(String.format(Tr.t("%1$s on %2$s"), moved, name(event.getDevice())));
            final String tip = rightStickTip(event);
            if (tip != null) hint.setText(tip);
            return true;
        });
        sheet.action(Tr.t("Done"), null, true, false, null);
        sheet.onDismiss(() -> {
            if (input != null) input.unregisterInputDeviceListener(listener);
        });
        sheet.show();
    }

    /** One line a device: the name, its USB ids and what kind of input Android says it gives. */
    private static String listDevices() {
        final StringBuilder text = new StringBuilder();
        for (int id : InputDevice.getDeviceIds()) {
            final InputDevice device = InputDevice.getDevice(id);
            if (device == null || device.isVirtual()) continue;
            final int sources = device.getSources();
            final String kind;
            if (isGamepad(device)) {
                kind = Tr.t("gamepad");
            } else if ((sources & InputDevice.SOURCE_KEYBOARD) == InputDevice.SOURCE_KEYBOARD
                    && device.isExternal()) {
                kind = Tr.t("buttons only");
            } else {
                continue;  // the touchscreen, the phone's own keys
            }
            if (text.length() > 0) text.append('\n');
            text.append("• ").append(device.getName())
                    .append(String.format(" (%04x:%04x) — ", device.getVendorId(), device.getProductId()))
                    .append(kind);
        }
        return text.length() > 0 ? text.toString()
                : Tr.t("No gamepad found. Pair it in Android's Bluetooth settings (or plug it in), then open this again.");
    }

    /** "name (vvvv:pppp, sources 0x...)" for each gamepad Android lists, "; " between; empty when none. */
    static String envValue() {
        final StringBuilder text = new StringBuilder();
        for (int id : InputDevice.getDeviceIds()) {
            final InputDevice device = InputDevice.getDevice(id);
            if (device == null || device.isVirtual() || !isGamepad(device)) continue;
            if (text.length() > 0) text.append("; ");
            text.append(device.getName()).append(String.format(" (%04x:%04x, sources 0x%x)",
                    device.getVendorId(), device.getProductId(), device.getSources()));
        }
        return text.toString();
    }

    private static boolean isGamepad(InputDevice device) {
        if (device == null) return false;
        final int sources = device.getSources();
        return (sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (sources & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    private static String name(InputDevice device) {
        return device != null ? device.getName() : Tr.t("an unknown device");
    }

    /**
     * Which pair carries the right stick, for Settings > Controls > Right Stick Axes (generic pads use
     * Z/RZ or RX/RY, and some put the triggers on the other pair). Null while neither pair is pushed.
     */
    private static String rightStickTip(MotionEvent event) {
        final float z = Math.max(Math.abs(event.getAxisValue(MotionEvent.AXIS_Z)),
                Math.abs(event.getAxisValue(MotionEvent.AXIS_RZ)));
        final float rx = Math.max(Math.abs(event.getAxisValue(MotionEvent.AXIS_RX)),
                Math.abs(event.getAxisValue(MotionEvent.AXIS_RY)));
        if (Math.max(z, rx) < 0.5f) return null;
        return z > rx ? Tr.t("This gamepad's right stick is Z / RZ. If the camera does not turn in the game, set Settings > Controls > Right Stick Axes to Z / RZ.")
                : Tr.t("This gamepad's right stick is RX / RY. If the camera does not turn in the game, set Settings > Controls > Right Stick Axes to RX / RY.");
    }

    /** The axis moved furthest from rest, e.g. "Left stick X +0.82". */
    private static String strongestAxis(MotionEvent event) {
        final int[] axes = {MotionEvent.AXIS_X, MotionEvent.AXIS_Y, MotionEvent.AXIS_Z, MotionEvent.AXIS_RZ,
                MotionEvent.AXIS_RX, MotionEvent.AXIS_RY, MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_RTRIGGER,
                MotionEvent.AXIS_BRAKE, MotionEvent.AXIS_GAS, MotionEvent.AXIS_HAT_X, MotionEvent.AXIS_HAT_Y};
        final String[] names = {"Left stick X", "Left stick Y", "Right stick X (Z)", "Right stick Y (RZ)",
                "Right stick X (RX)", "Right stick Y (RY)", "Left trigger", "Right trigger",
                "Left trigger (brake)", "Right trigger (gas)", "D-pad X", "D-pad Y"};
        int best = -1;
        float bestValue = 0.3f;
        for (int i = 0; i < axes.length; ++i) {
            final float value = Math.abs(event.getAxisValue(axes[i]));
            if (value > bestValue) {
                bestValue = value;
                best = i;
            }
        }
        return best < 0 ? null : String.format("%s %+.2f", names[best], event.getAxisValue(axes[best]));
    }
}
