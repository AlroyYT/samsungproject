package com.example.androidsamsung;

import androidx.appcompat.app.AppCompatActivity;
import android.os.Bundle;
import android.util.Log;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.MotionEvent;
import android.widget.TextView;
import android.widget.LinearLayout;
import android.graphics.Color;
import android.os.Handler;
import android.os.Looper;

public class MainActivity extends AppCompatActivity {

    private static final String TAG = "OpenXR_MainActivity";

    static {
        System.loadLibrary("openxroverlay");
    }

    // Native method declarations (existing ones)
    public native boolean initializeCustomRuntime(Surface surface);
    public native long createXRInstance();
    public native long createXRSession(long instance, Surface surface);
    public native boolean beginXRFrame(long session);
    public native boolean endXRFrame(long session);
    public native boolean createOverlay(long session, int layerId);
    public native int getFrameCount();
    public native String getRuntimeInfo();
    public native String[] getSupportedExtensions();
    public native void destroyCustomRuntime();

    // NEW: Animation and interaction methods
    public native void updateOverlayPosition(int layerId, float x, float y);
    public native void updateOverlayScale(int layerId, float scale);
    public native void updateOverlayColor(int layerId, float r, float g, float b);

    private Thread renderThread = null;
    private volatile boolean renderingActive = false;

    // UI Elements
    private TextView statusText;
    private TextView frameCountText;
    private TextView runtimeInfoText;
    private TextView interactionText;
    private Handler uiHandler;

    // NEW: Animation state
    private long animationStartTime = 0;
    private boolean animationsEnabled = true;
    private float touchX = 0.5f, touchY = 0.5f;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Log.d(TAG, "Activity created.");
        uiHandler = new Handler(Looper.getMainLooper());
        createStatusUI();
    }

    private void createStatusUI() {
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setBackgroundColor(Color.BLACK);
        layout.setPadding(20, 20, 20, 20);

        statusText = new TextView(this);
        statusText.setTextColor(Color.GREEN);
        statusText.setTextSize(16);
        layout.addView(statusText);

        runtimeInfoText = new TextView(this);
        runtimeInfoText.setTextColor(Color.CYAN);
        runtimeInfoText.setTextSize(14);
        layout.addView(runtimeInfoText);

        frameCountText = new TextView(this);
        frameCountText.setTextColor(Color.YELLOW);
        frameCountText.setTextSize(14);
        layout.addView(frameCountText);

        // NEW: Interaction info display
        interactionText = new TextView(this);
        interactionText.setTextColor(Color.MAGENTA);
        interactionText.setTextSize(12);
        interactionText.setText("Touch screen to move overlays • Tap to change animation");
        layout.addView(interactionText);

        SurfaceView surfaceView = new SurfaceView(this);
        surfaceView.setLayoutParams(new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.MATCH_PARENT, 1.0f));

        surfaceView.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                Log.i(TAG, "Surface created. Starting render thread.");
                startRenderThread(holder.getSurface());
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                Log.i(TAG, "Surface destroyed. Stopping render thread.");
                stopRenderThread();
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                // Not used in this model
            }
        });

        layout.addView(surfaceView);
        setContentView(layout);
    }

    // NEW: Touch handling for interaction
    @Override
    public boolean onTouchEvent(MotionEvent event) {
        switch (event.getAction()) {
            case MotionEvent.ACTION_DOWN:
                // Toggle animation on tap
                animationsEnabled = !animationsEnabled;
                updateInteractionInfo("Animation " + (animationsEnabled ? "ON" : "OFF"));
                break;

            case MotionEvent.ACTION_MOVE:
            case MotionEvent.ACTION_UP:
                // Convert screen coordinates to normalized coordinates (-1 to 1)
                touchX = (event.getX() / getResources().getDisplayMetrics().widthPixels) * 2.0f - 1.0f;
                touchY = -((event.getY() / getResources().getDisplayMetrics().heightPixels) * 2.0f - 1.0f);

                // Update overlay positions based on touch
                updateOverlayPosition(0, touchX - 0.3f, touchY + 0.3f);  // Blue offset
                updateOverlayPosition(1, touchX, touchY);                 // Magenta follows touch
                updateOverlayPosition(2, touchX + 0.3f, touchY - 0.3f);  // Green offset
                break;
        }
        return true;
    }

    private void startRenderThread(Surface surface) {
        if (renderThread != null) {
            return;
        }
        renderingActive = true;
        animationStartTime = System.currentTimeMillis();

        renderThread = new Thread(() -> {
            long xrInstance = 0;
            long xrSession = 0;

            // 1. INITIALIZE
            updateStatus("Initializing Custom Runtime...");
            if (!initializeCustomRuntime(surface)) {
                updateStatus("ERROR: Runtime initialization failed");
                return;
            }

            // 2. CREATE XR OBJECTS
            updateStatus("Creating XR Instance...");
            xrInstance = createXRInstance();
            if (xrInstance == 0) {
                updateStatus("ERROR: Failed to create XR Instance");
                destroyCustomRuntime();
                return;
            }

            updateStatus("Creating XR Session...");
            xrSession = createXRSession(xrInstance, surface);
            if (xrSession == 0) {
                updateStatus("ERROR: Failed to create XR Session");
                destroyCustomRuntime();
                return;
            }

            // Create overlays
            updateStatus("Creating Overlay Layers...");
            for (int i = 0; i < 3; i++) createOverlay(xrSession, i);
            updateRuntimeInfo(getRuntimeInfo(), getSupportedExtensions());
            updateStatus("Runtime Active - Interactive Rendering Started");

            // 3. RENDER LOOP WITH ANIMATIONS
            while (renderingActive) {
                if (beginXRFrame(xrSession)) {
                    // NEW: Update animations if enabled
                    if (animationsEnabled) {
                        updateAnimations();
                    }
                    endXRFrame(xrSession);
                }

                // Update UI periodically
                if (getFrameCount() % 30 == 0) {
                    updateFrameCount();
                }

                try {
                    Thread.sleep(14); // ~72 FPS
                } catch (InterruptedException e) {
                    break;
                }
            }

            // 4. CLEANUP
            Log.i(TAG, "Render loop finished. Cleaning up.");
            destroyCustomRuntime();

        }, "MonadoRenderThread");
        renderThread.start();
    }

    // NEW: Animation logic
    private void updateAnimations() {
        long currentTime = System.currentTimeMillis();
        float timeSeconds = (currentTime - animationStartTime) / 1000.0f;

        // Circular motion for overlay 0 (Blue)
        float radius = 0.3f;
        float blueX = (float) Math.cos(timeSeconds * 0.5) * radius;
        float blueY = (float) Math.sin(timeSeconds * 0.5) * radius;
        updateOverlayPosition(0, blueX, blueY);

        // Pulsing scale for overlay 1 (Magenta)
        float pulseFactor = 1.0f + 0.3f * (float) Math.sin(timeSeconds * 2.0);
        updateOverlayScale(1, 0.25f * pulseFactor);

        // Color cycling for overlay 2 (Green)
        float colorCycle = (float) Math.sin(timeSeconds) * 0.5f + 0.5f;
        updateOverlayColor(2, colorCycle, 1.0f - colorCycle, 0.5f);

        // Figure-8 motion for overlay 2
        float fig8X = (float) Math.sin(timeSeconds * 0.8) * 0.4f;
        float fig8Y = (float) Math.sin(timeSeconds * 1.6) * 0.2f;
        updateOverlayPosition(2, fig8X, fig8Y);
    }

    private void stopRenderThread() {
        renderingActive = false;
        if (renderThread != null) {
            try {
                renderThread.join();
            } catch (InterruptedException e) {
                Log.w(TAG, "Interrupted while joining render thread.");
            }
            renderThread = null;
        }
        updateStatus("Runtime Stopped");
    }

    // UI update methods
    private void updateStatus(String message) {
        uiHandler.post(() -> {
            if (statusText != null) {
                statusText.setText("Status: " + message);
            }
            Log.i(TAG, "Status: " + message);
        });
    }

    // NEW: Update interaction info
    private void updateInteractionInfo(String message) {
        uiHandler.post(() -> {
            if (interactionText != null) {
                interactionText.setText(message + " • Touch: (" +
                        String.format("%.2f", touchX) + ", " + String.format("%.2f", touchY) + ")");
            }
        });
    }

    private void updateRuntimeInfo(String runtimeInfo, String[] extensions) {
        uiHandler.post(() -> {
            if (runtimeInfoText != null) {
                StringBuilder info = new StringBuilder();
                info.append("Runtime: ").append(runtimeInfo).append("\n");
                info.append("Extensions: ");
                if (extensions != null) {
                    for (int i = 0; i < extensions.length; i++) {
                        if (i > 0) info.append(", ");
                        info.append(extensions[i]);
                    }
                }
                runtimeInfoText.setText(info.toString());
            }
        });
    }

    private void updateFrameCount() {
        uiHandler.post(() -> {
            if (frameCountText != null) {
                frameCountText.setText("Frame: " + getFrameCount() +
                        (animationsEnabled ? " [ANIMATED]" : " [STATIC]"));
            }
        });
    }

    @Override
    protected void onPause() {
        super.onPause();
        Log.d(TAG, "Activity paused, stopping renderer.");
        stopRenderThread();
    }

    @Override
    protected void onResume() {
        super.onResume();
        Log.d(TAG, "Activity resumed.");
    }
}