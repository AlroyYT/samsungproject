# Sets the minimum version of CMake required.
cmake_minimum_required(VERSION 3.22.1)

# Declares the project name.
project("androidsamsung")

# REMOVED: add_library(native_app_glue ...), it is not needed for a JNI-based app.

# Add ONLY your custom runtime implementation to the shared library.
# The main.cpp file has been removed as it's part of the NativeActivity model.
add_library(openxroverlay SHARED
        custom_monado_runtime.cpp)

# Specifies include directories for the compiler to find header files.
target_include_directories(openxroverlay PRIVATE
        # REMOVED: The native_app_glue include path is no longer needed.
        ${CMAKE_CURRENT_SOURCE_DIR}/openxr/include
)

# --- Find Standard Android Libraries ---
find_library(log-lib log)
find_library(android-lib android)
find_library(egl-lib EGL)
find_library(glesv3-lib GLESv3)

# --- Link Libraries for Custom Monado Runtime ---
target_link_libraries(openxroverlay
        # REMOVED: openxr_loader and native_app_glue are not part of this model.
        ${log-lib}
        ${android-lib}
        ${egl-lib}
        ${glesv3-lib}
)

# --- Add Custom Runtime Definitions ---
target_compile_definitions(openxroverlay PRIVATE
        # This is not strictly needed anymore but doesn't hurt.
        CUSTOM_MONADO_RUNTIME=1
        XR_USE_PLATFORM_ANDROID=1
)