package com.shiinobu.re4;

import android.app.Activity;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.view.Surface;

/**
 * Gyro aiming: the phone's turn rate as a left-stick tilt, which moves RE4's laser sight while a
 * weapon or the knife is readied (Classic R / L). The game's aim is a rate control, so a turn rate
 * maps onto it directly: turn faster, the sight moves faster; hold still, it stops.
 */
final class GyroAim implements SensorEventListener {
    interface Listener {
        /** -1..1 with +y up; 0, 0 when the phone is still. */
        void onGyroAim(float x, float y);
    }

    /** Turn rate (rad/s) that gives a full tilt at sensitivity 1 (about 115 degrees a second). */
    private static final float FULL_RATE = 2.0f;
    /** Below this rate (rad/s) the phone is held still: hand tremor and sensor drift. */
    private static final float NOISE_RATE = 0.08f;
    /** The game ignores a small stick tilt: a real turn starts just above it. */
    private static final float GAME_DEAD_ZONE = 0.1f;
    /** Low-pass weight of a new sample (about 50 a second): steadies a shaking hand. */
    private static final float SMOOTHING = 0.3f;
    private float rateX, rateY;

    private final Activity activity;
    private final Listener listener;
    private final SensorManager sensors;
    private final Sensor gyro;
    private float sensitivity = 1f;
    private boolean invertY;
    private boolean running;

    GyroAim(Activity activity, Listener listener) {
        this.activity = activity;
        this.listener = listener;
        sensors = activity.getSystemService(SensorManager.class);
        gyro = sensors != null ? sensors.getDefaultSensor(Sensor.TYPE_GYROSCOPE) : null;
    }

    static boolean supported(Activity activity) {
        final SensorManager s = activity.getSystemService(SensorManager.class);
        return s != null && s.getDefaultSensor(Sensor.TYPE_GYROSCOPE) != null;
    }

    void start(boolean on, float sensitivity, boolean invertY) {
        this.sensitivity = sensitivity;
        this.invertY = invertY;
        if (!on || gyro == null) {
            stop();
            return;
        }
        if (!running) running = sensors.registerListener(this, gyro, SensorManager.SENSOR_DELAY_GAME);
    }

    void stop() {
        if (running) sensors.unregisterListener(this);
        running = false;
        rateX = rateY = 0f;
        listener.onGyroAim(0f, 0f);
    }

    @Override
    public void onSensorChanged(SensorEvent event) {
        // The rotation rates in the screen's frame (x right, y up), whatever way the phone is turned.
        final float dx = event.values[0], dy = event.values[1];
        final float sx, sy;
        switch (activity.getWindowManager().getDefaultDisplay().getRotation()) {
            case Surface.ROTATION_90: sx = -dy; sy = dx; break;
            case Surface.ROTATION_180: sx = -dx; sy = -dy; break;
            case Surface.ROTATION_270: sx = dy; sy = -dx; break;
            default: sx = dx; sy = dy; break;
        }
        // Turning right is a negative turn about the screen's up axis; tilting the top away is a
        // positive turn about its right axis, and points the back of the phone up.
        rateX += SMOOTHING * (-sy - rateX);
        rateY += SMOOTHING * ((invertY ? -sx : sx) - rateY);
        // One direction for both axes: a turn to the right does not also nudge the sight up or down.
        final float rate = (float) Math.hypot(rateX, rateY);
        if (rate < NOISE_RATE) {
            listener.onGyroAim(0f, 0f);
            return;
        }
        final float tilt = Math.min(1f, (rate - NOISE_RATE) * sensitivity / FULL_RATE);
        final float out = (GAME_DEAD_ZONE + (1f - GAME_DEAD_ZONE) * tilt) / rate;
        listener.onGyroAim(rateX * out, rateY * out);
    }

    @Override
    public void onAccuracyChanged(Sensor sensor, int accuracy) {}
}
