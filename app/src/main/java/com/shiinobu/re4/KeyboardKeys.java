package com.shiinobu.re4;

import android.app.Activity;
import android.view.InputDevice;
import android.view.KeyEvent;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Settings > Controls > Keyboard Keys: one or two keys of a physical keyboard for each Classic
 * Controller control. Saved as "classic_a=Space,J|classic_b=Left Shift|..." (AppSettings.keyboardKeys),
 * read by the game in Keybinds::SetFromApp: the Config.toml key, then SDL scancode names. "" is the
 * default layout (keybinds.cpp), and an action left out keeps its default.
 */
final class KeyboardKeys {
    /** Actions in the list's order: native key, what it does in RE4, its default keys (SDL names). */
    private static final String[][] ACTIONS = {
        {"lstick_up", "Move Forward", "W"}, {"lstick_down", "Move Back", "S"},
        {"lstick_left", "Move Left", "A"}, {"lstick_right", "Move Right", "D"},
        {"classic_a", "A · Action / Fire", "Space,J"}, {"classic_b", "B · Run / Cancel", "Left Shift,Backspace"},
        {"classic_x", "X · Ashley", "Q"}, {"classic_y", "Y · Inventory", "Tab,I"},
        {"classic_r", "R · Ready Weapon", "K,Right Shift"}, {"classic_l", "L · Ready Knife", "L,C"},
        {"reload", "Reload (R + B)", "R"},
        {"classic_zl", "ZL · Map", "N"}, {"classic_zr", "ZR · Map", "M"},
        {"plus", "+ · Options / Skip", "Return,Escape"}, {"minus", "− · Inventory", "-"},
        {"classic_home", "HOME", "H"},
        {"rstick_up", "Camera Up", "Up"}, {"rstick_down", "Camera Down", "Down"},
        {"rstick_left", "Camera Left", "Left"}, {"rstick_right", "Camera Right", "Right"},
        {"up", "D-pad Up", "Keypad 8"}, {"down", "D-pad Down", "Keypad 2"},
        {"left", "D-pad Left", "Keypad 4"}, {"right", "D-pad Right", "Keypad 6"},
    };

    private final Activity activity;
    private final AppSettings settings;
    private final Runnable changed;

    KeyboardKeys(Activity activity, AppSettings settings, Runnable changed) {
        this.activity = activity;
        this.settings = settings;
        this.changed = changed;
    }

    /** One line for the settings card. */
    static String summary(AppSettings settings) {
        return settings.keyboardKeys().isEmpty()
                ? Tr.t("Default: WASD move, arrows camera, Space A, Shift B, K aim, L or C knife, R reload.")
                : Tr.t("Set by you.");
    }

    void show() {
        final Map<String, List<String>> map = parse(settings.keyboardKeys());
        final Sheet sheet = new Sheet(activity, Tr.t("Keyboard Keys"),
                Tr.t("Tap an action, then press the key you want for it. Each action takes up to two keys."));
        for (String[] action : ACTIONS) {
            final List<String> keys = map.get(action[0]);
            sheet.action(Tr.t(action[1]), keys.isEmpty() ? Tr.t("No key") : String.join("  /  ", keys),
                    false, false, () -> ask(action));
        }
        sheet.action(Tr.t("Reset to Default"), null, false, true, () -> {
            settings.setKeyboardKeys("");
            changed.run();
            show();
        });
        sheet.action(Tr.t("Done"), null, true, false, null);
        sheet.show();
    }

    /** Waits for a key for `action`: it replaces the action's keys, or with `add` joins the first one. */
    private void ask(String[] action) { ask(action, false); }

    private void ask(String[] action, boolean add) {
        final Sheet sheet = new Sheet(activity, Tr.t("Press a key"), Tr.t(action[1]));
        final boolean[] got = {false};
        final boolean[] chained = {false};  // the second-key sheet took over and shows the list itself
        sheet.onGamepad((dialog, keyCode, event) -> {
            if (!fromKeyboard(event)) return false;
            final String name = sdlName(keyCode);
            if (name == null) return false;
            if (event.getAction() == KeyEvent.ACTION_DOWN && !got[0]) {
                got[0] = true;
                assign(action[0], name, add);
                sheet.dismiss();
            }
            return true;
        }, null);
        // Mouse buttons (keybinds.cpp "Mouse Left" ...; they count while the game has the mouse captured).
        // Chosen here: a left click on this sheet is what presses its buttons.
        for (String[] click : new String[][] {{"Left Click", "Mouse Left"}, {"Right Click", "Mouse Right"},
                {"Middle Click", "Mouse Middle"}}) {
            sheet.action(Tr.t(click[0]), null, false, false, () -> assign(action[0], click[1], add));
        }
        if (!add) {
            sheet.action(Tr.t("Add as a second key"), null, false, false, () -> {
                chained[0] = true;
                ask(action, true);
            });
            sheet.action(Tr.t("No key"), null, false, false, () -> assign(action[0], null, false));
        }
        sheet.action(Tr.t("Cancel"), null, false, false, null);
        // Dialog dismiss listeners run after the button's action, so `chained` is set by then.
        sheet.onDismiss(() -> {
            if (!chained[0]) show();
        });
        sheet.show();
    }

    /** Gives `key` the key `name` (null: none). A key belongs to one action, so it leaves any other. */
    private void assign(String key, String name, boolean add) {
        final Map<String, List<String>> map = parse(settings.keyboardKeys());
        if (name != null) {
            for (Map.Entry<String, List<String>> entry : map.entrySet()) {
                if (!entry.getKey().equals(key)) entry.getValue().removeIf(name::equalsIgnoreCase);
            }
        }
        final List<String> keys = map.get(key);
        if (name == null) {
            keys.clear();
        } else if (add) {
            keys.removeIf(name::equalsIgnoreCase);
            if (keys.size() >= 2) keys.remove(1);
            keys.add(name);
        } else {
            keys.clear();
            keys.add(name);
        }
        final StringBuilder text = new StringBuilder();
        for (String[] action : ACTIONS) {
            if (text.length() > 0) text.append('|');
            final List<String> k = map.get(action[0]);
            text.append(action[0]).append('=').append(k.isEmpty() ? "None" : String.join(",", k));
        }
        settings.setKeyboardKeys(text.toString());
        changed.run();
    }

    /** Every action with its keys: the saved ones, else the default. */
    private static Map<String, List<String>> parse(String text) {
        final Map<String, String> saved = new LinkedHashMap<>();
        for (String entry : text.split("\\|")) {
            final int eq = entry.indexOf('=');
            if (eq > 0) saved.put(entry.substring(0, eq).trim(), entry.substring(eq + 1).trim());
        }
        final Map<String, List<String>> map = new LinkedHashMap<>();
        final List<String> taken = new ArrayList<>();  // keys the player gave an action
        for (String[] action : ACTIONS) {
            final String value = saved.containsKey(action[0]) ? saved.get(action[0]) : action[2];
            final List<String> keys = new ArrayList<>();
            if (!value.equalsIgnoreCase("None")) {
                for (String k : splitKeys(value)) if (!k.isEmpty()) keys.add(k);
            }
            if (saved.containsKey(action[0])) taken.addAll(keys);
            map.put(action[0], keys);
        }
        // An action newer than the saved keys (Reload, 1.2) gets its default only where the player's own
        // keys leave it free: R set to Ashley in 1.1 must not reload as well.
        if (!saved.isEmpty()) {
            for (String[] action : ACTIONS) {
                if (saved.containsKey(action[0])) continue;
                map.get(action[0]).removeIf(k -> taken.stream().anyMatch(k::equalsIgnoreCase));
            }
        }
        return map;
    }

    /** What the game gets (GameActivity, nativeSetKeyboardKeys): "" for the default layout, else every
     *  action spelled out as parse resolves it, so keybinds.cpp never adds a default key taken elsewhere. */
    static String forGame(String text) {
        if (text == null || text.isEmpty()) return "";
        final Map<String, List<String>> map = parse(text);
        final StringBuilder out = new StringBuilder();
        for (String[] action : ACTIONS) {
            if (out.length() > 0) out.append('|');
            final List<String> k = map.get(action[0]);
            out.append(action[0]).append('=').append(k.isEmpty() ? "None" : String.join(",", k));
        }
        return out.toString();
    }

    /** "A,Keypad ,,B": a key named "," (or ending in one) keeps its comma, as keybinds.cpp reads it. */
    private static List<String> splitKeys(String value) {
        final List<String> out = new ArrayList<>();
        int start = 0;
        while (start < value.length()) {
            int comma = value.indexOf(',', start);
            if (comma < 0) {
                out.add(value.substring(start).trim());
                break;
            }
            if (comma == start || (comma + 1 < value.length() && value.charAt(comma + 1) == ',')) {
                out.add(value.substring(start, comma + 1).trim());
                start = comma + 1;
                if (start < value.length() && value.charAt(start) == ',') ++start;
            } else {
                out.add(value.substring(start, comma).trim());
                start = comma + 1;
            }
        }
        return out;
    }

    private static boolean fromKeyboard(KeyEvent event) {
        if (event.getKeyCode() == KeyEvent.KEYCODE_BACK) return false;  // the phone's Back closes the sheet
        final InputDevice device = event.getDevice();
        if (device == null || device.isVirtual()) return false;
        return (event.getSource() & InputDevice.SOURCE_KEYBOARD) == InputDevice.SOURCE_KEYBOARD
                && device.getKeyboardType() == InputDevice.KEYBOARD_TYPE_ALPHABETIC;
    }

    /** An Android key code as SDL names its scancode (SDL_GetScancodeName), null for keys it has none for. */
    static String sdlName(int code) {
        if (code >= KeyEvent.KEYCODE_A && code <= KeyEvent.KEYCODE_Z) {
            return String.valueOf((char) ('A' + code - KeyEvent.KEYCODE_A));
        }
        if (code >= KeyEvent.KEYCODE_0 && code <= KeyEvent.KEYCODE_9) {
            return String.valueOf((char) ('0' + code - KeyEvent.KEYCODE_0));
        }
        if (code >= KeyEvent.KEYCODE_NUMPAD_0 && code <= KeyEvent.KEYCODE_NUMPAD_9) {
            return "Keypad " + (code - KeyEvent.KEYCODE_NUMPAD_0);
        }
        if (code >= KeyEvent.KEYCODE_F1 && code <= KeyEvent.KEYCODE_F12) {
            return "F" + (1 + code - KeyEvent.KEYCODE_F1);
        }
        switch (code) {
            case KeyEvent.KEYCODE_SPACE: return "Space";
            case KeyEvent.KEYCODE_ENTER: return "Return";
            case KeyEvent.KEYCODE_ESCAPE: return "Escape";
            case KeyEvent.KEYCODE_DEL: return "Backspace";
            case KeyEvent.KEYCODE_FORWARD_DEL: return "Delete";
            case KeyEvent.KEYCODE_TAB: return "Tab";
            case KeyEvent.KEYCODE_SHIFT_LEFT: return "Left Shift";
            case KeyEvent.KEYCODE_SHIFT_RIGHT: return "Right Shift";
            case KeyEvent.KEYCODE_CTRL_LEFT: return "Left Ctrl";
            case KeyEvent.KEYCODE_CTRL_RIGHT: return "Right Ctrl";
            case KeyEvent.KEYCODE_ALT_LEFT: return "Left Alt";
            case KeyEvent.KEYCODE_ALT_RIGHT: return "Right Alt";
            case KeyEvent.KEYCODE_CAPS_LOCK: return "CapsLock";
            case KeyEvent.KEYCODE_DPAD_UP: return "Up";
            case KeyEvent.KEYCODE_DPAD_DOWN: return "Down";
            case KeyEvent.KEYCODE_DPAD_LEFT: return "Left";
            case KeyEvent.KEYCODE_DPAD_RIGHT: return "Right";
            case KeyEvent.KEYCODE_INSERT: return "Insert";
            case KeyEvent.KEYCODE_MOVE_HOME: return "Home";
            case KeyEvent.KEYCODE_MOVE_END: return "End";
            case KeyEvent.KEYCODE_PAGE_UP: return "PageUp";
            case KeyEvent.KEYCODE_PAGE_DOWN: return "PageDown";
            case KeyEvent.KEYCODE_MINUS: return "-";
            case KeyEvent.KEYCODE_EQUALS: return "=";
            case KeyEvent.KEYCODE_LEFT_BRACKET: return "[";
            case KeyEvent.KEYCODE_RIGHT_BRACKET: return "]";
            case KeyEvent.KEYCODE_BACKSLASH: return "\\";
            case KeyEvent.KEYCODE_SEMICOLON: return ";";
            case KeyEvent.KEYCODE_APOSTROPHE: return "'";
            case KeyEvent.KEYCODE_GRAVE: return "`";
            case KeyEvent.KEYCODE_COMMA: return ",";
            case KeyEvent.KEYCODE_PERIOD: return ".";
            case KeyEvent.KEYCODE_SLASH: return "/";
            case KeyEvent.KEYCODE_NUMPAD_ENTER: return "Keypad Enter";
            case KeyEvent.KEYCODE_NUMPAD_ADD: return "Keypad +";
            case KeyEvent.KEYCODE_NUMPAD_SUBTRACT: return "Keypad -";
            case KeyEvent.KEYCODE_NUMPAD_MULTIPLY: return "Keypad *";
            case KeyEvent.KEYCODE_NUMPAD_DIVIDE: return "Keypad /";
            case KeyEvent.KEYCODE_NUMPAD_DOT: return "Keypad .";
            default: return null;
        }
    }
}
