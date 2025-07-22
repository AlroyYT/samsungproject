# Sets the minimum version of CMake required to build the native
# library. You should always be explicit with this.
cmake_minimum_required(VERSION 3.22.1)

# Declares the project name.
project("androidsamsung")

# Adds the native_app_glue source file to a static library.
# This glue code helps manage Android application lifecycle events.
add_library(native_app_glue STATIC
        ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)

# Adds your main C++ source file to a shared library, which will be
# loaded by your Android app.
add_library(androidsamsung SHARED
        main.cpp)

# Specifies include directories for the compiler to find header files.
target_include_directories(androidsamsung PRIVATE
        # Path for android_native_app_glue.h
        ${ANDROID_NDK}/sources/android/native_app_glue
        # Path for your own headers (e.g., openxr/)
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# --- Find Standard Android Libraries ---
# Finds the system log library.
find_library(log-lib log)
# Finds the core Android library.
find_library(android-lib android)
# Finds the EGL graphics library.
find_library(egl-lib EGL)
# Finds the OpenGL ES 3.x library.
find_library(glesv3-lib GLESv3)


# --- IMPORTANT: Find the OpenXR Library ---
# This is the key fix. We need to tell CMake where the pre-compiled
# OpenXR loader library (.so file) is located.

# 1. DEFINE THE PATH to the directory containing your ABI subfolders (arm64-v8a, etc.)
set(OPENXR_LIBS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/openxr/libs)

# 2. CREATE AN IMPORTED LIBRARY TARGET for OpenXR.
add_library(openxr_loader SHARED IMPORTED)

# 3. SET THE LOCATION of the library file for the target.
set_target_properties(openxr_loader PROPERTIES
        IMPORTED_LOCATION ${OPENXR_LIBS_DIR}/android.${ANDROID_ABI}/libopenxr_loader.so
)


# --- Link All Libraries to Your Application ---
# This command connects all the required libraries to your main 'androidsamsung' library.
target_link_libraries(androidsamsung
        # Link the OpenXR loader library that we just imported.
        openxr_loader

        # Link the native app glue code.
        native_app_glue

        # Link the standard Android system libraries.
        ${log-lib}
        ${android-lib}
        ${egl-lib}
        ${glesv3-lib}
)
