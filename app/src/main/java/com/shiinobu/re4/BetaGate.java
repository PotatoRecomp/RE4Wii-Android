package com.shiinobu.re4;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.IOException;
import java.net.HttpURLConnection;

/**
 * The testers' Beta (BuildConfig.BETA) stops once it is switched off or the release is out:
 * <ul>
 * <li>the latest GitHub release is RELEASE_VERSION or later, or</li>
 * <li>beta-channel.json in shiinobu/re4-beta-android says
 * {"enabled": false, "message": "..."}, or {"min_beta": N} with this Beta's number below N.</li>
 * </ul>
 * The status file sits in the release repo; while that repo is private GitHub answers 404
 * without a login (treated as no file). It is checked even without ONLINE_UPDATES.
 * No status file, or no network, leaves the Beta playable until a check sees it end; LauncherActivity
 * keeps that answer (AppSettings.betaEnded), so going offline afterwards doesn't reopen it.
 * A private test track (BuildConfig.BETA_TRACK) reads beta-channel-<track>.json instead and runs
 * only while that file is there and open: no file means closed, and LauncherActivity doesn't start
 * it without an answer (no network = no game).
 * Testing: debug.re4.update_url and debug.re4.beta_url point both checks at a local server.
 */
final class BetaGate {
    static final String RELEASE_VERSION = "1.0";
    private static final String BETA_STATUS =
            "https://raw.githubusercontent.com/shiinobu/re4-beta-android/main/beta-channel"
                    + (BuildConfig.BETA_TRACK.isEmpty() ? "" : "-" + BuildConfig.BETA_TRACK.toLowerCase()) + ".json";

    private static final String TRACK_CLOSED = "This test has been closed. Thanks for testing!";

    private BetaGate() {}

    /** Null while the Beta may run; otherwise why it ended, for the tester. Network: off the UI thread. */
    static String check() throws IOException {
        if (BETA_STATUS.isEmpty()) {
            return null;  // no RE4 beta channel
        }
        if (BuildConfig.ONLINE_UPDATES) {
            final Updater.Release release = Updater.latest();
            if (release != null && !Updater.isNewer(RELEASE_VERSION, release.version)) {
                return "RE4 " + release.version + " is out.";
            }
        }
        final HttpURLConnection connection =
                Updater.open(Updater.debugProperty("debug.re4.beta_url", BETA_STATUS));
        try {
            final int code = connection.getResponseCode();
            if (code == HttpURLConnection.HTTP_NOT_FOUND) {
                return BuildConfig.BETA_TRACK.isEmpty() ? null : TRACK_CLOSED;
            }
            if (code != HttpURLConnection.HTTP_OK) {
                throw new IOException("GitHub answered " + code);
            }
            final JSONObject status = new JSONObject(Updater.readAll(connection.getInputStream()));
            if (!status.optBoolean("enabled", true)) {
                return status.optString("message",
                        BuildConfig.BETA_TRACK.isEmpty() ? "The Beta has been switched off." : TRACK_CLOSED);
            }
            if (BuildConfig.VERSION_CODE < status.optInt("min_beta", 0)) {
                return status.optString("min_beta_message",
                        "This Beta is too old. Ask the developer for the new one.");
            }
            return null;
        } catch (JSONException e) {
            throw new IOException("unreadable beta-channel.json: " + e.getMessage());
        } finally {
            connection.disconnect();
        }
    }
}
