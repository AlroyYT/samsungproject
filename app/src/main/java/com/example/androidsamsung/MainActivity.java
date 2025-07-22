package com.example.androidsamsung;

import android.app.NativeActivity;

public class MainActivity extends NativeActivity {
    static {
        // This name MUST match the name in CMakeLists.txt
        System.loadLibrary("main");
    }
}