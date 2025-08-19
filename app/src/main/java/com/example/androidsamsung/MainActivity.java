package com.example.androidsamsung;

import android.app.NativeActivity;
import android.os.Bundle;

public class MainActivity extends NativeActivity {
    static {
        /**
         * This loads the C++ library.
         * The name 'samsungproject' must match the library name defined in your
         * CMakeLists.txt file (the first argument to `add_library`).
         */
        System.loadLibrary("samsungproject");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
    }
}