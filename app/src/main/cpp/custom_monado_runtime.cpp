#include <android/log.h>
#include "android_native_app_glue.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <vector>
#include <string>
#include <cmath>
#include <cstring>
#include <unistd.h>
#include <algorithm> // For std::min

// OpenXR Headers
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define TAG "OpenXROverlayApp"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

const uint32_t LAYER_COUNT = 4;

// Main application state
struct OpenXrApp {
    struct android_app* app;
    bool resumed = false;
    bool sessionRunning = false;

    // --- Animation State ---
    // 0=background, 1=blue, 2=magenta, 3=green, 4=done
    int animation_stage = 0;
    float stage_timer = 0.0f;

    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    XrSpace appSpace = XR_NULL_HANDLE;

    XrSwapchain swapchains[LAYER_COUNT] = {XR_NULL_HANDLE};
    std::vector<GLuint> framebuffers[LAYER_COUNT];
    bool swapchainsCreated = false;

    uint32_t swapchainWidths[LAYER_COUNT] = {1024, 512, 512, 512};
    uint32_t swapchainHeights[LAYER_COUNT] = {1024, 256, 256, 256};

    XrViewConfigurationType viewConfigType;
    XrEnvironmentBlendMode blendMode;
};

// ... (CompileShader and CreateProgram helpers are unchanged) ...
GLuint CompileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint is_compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &is_compiled);
    if (is_compiled == GL_FALSE) {
        GLint max_length = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &max_length);
        std::vector<char> log(max_length);
        glGetShaderInfoLog(shader, max_length, &max_length, log.data());
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}
GLuint CreateProgram(const char* vs_source, const char* fs_source) {
    const char* vertex_shader_source = R"_(#version 300 es
    precision highp float;
    layout(location = 0) in vec2 aPos;
    void main() {
        gl_Position = vec4(aPos, 0.0, 1.0);
    })_";

    const char* fragment_shader_source = R"_(#version 300 es
    precision highp float;
    out vec4 FragColor;
    void main() {
        FragColor = vec4(1.0);
    })_";
    GLuint vs = CompileShader(GL_VERTEX_SHADER, vertex_shader_source);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fragment_shader_source);
    if (vs == 0 || fs == 0) return 0;
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    return program;
}

bool initializeOpenXR(OpenXrApp* oxr) {
    // Initialize loader first
    PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR;
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction*)&xrInitializeLoaderKHR);
    XrLoaderInitInfoAndroidKHR loaderInitInfo = {XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    loaderInitInfo.applicationVM = oxr->app->activity->vm;
    loaderInitInfo.applicationContext = oxr->app->activity->clazz;
    xrInitializeLoaderKHR((const XrLoaderInitInfoBaseHeaderKHR*)&loaderInitInfo);

    // Required extensions for XR_EXTX_overlay
    std::vector<const char*> extensions = {
            XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
            XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
            XR_EXTX_OVERLAY_EXTENSION_NAME  // This is the correct extension name
    };

    XrApplicationInfo appInfo = {};
    strncpy(appInfo.applicationName, "MultiOverlayTest", XR_MAX_APPLICATION_NAME_SIZE - 1);
    appInfo.apiVersion = XR_CURRENT_API_VERSION;

    XrInstanceCreateInfoAndroidKHR instanceCreateInfoAndroid = {XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    instanceCreateInfoAndroid.applicationVM = oxr->app->activity->vm;
    instanceCreateInfoAndroid.applicationActivity = oxr->app->activity->clazz;

    XrInstanceCreateInfo instanceCreateInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    instanceCreateInfo.next = &instanceCreateInfoAndroid;
    instanceCreateInfo.applicationInfo = appInfo;
    instanceCreateInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    instanceCreateInfo.enabledExtensionNames = extensions.data();

    XrResult result = xrCreateInstance(&instanceCreateInfo, &oxr->instance);
    if (XR_FAILED(result)) {
        LOGE("Failed to create OpenXR instance: %d", result);
        return false;
    }

    XrSystemGetInfo systemGetInfo = {XR_TYPE_SYSTEM_GET_INFO};
    systemGetInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    result = xrGetSystem(oxr->instance, &systemGetInfo, &oxr->systemId);
    if (XR_FAILED(result)) {
        LOGE("Failed to get OpenXR system: %d", result);
        return false;
    }

    LOGI("Successfully initialized OpenXR with XR_EXTX_overlay extension");
    return true;
}

bool createSession(OpenXrApp* oxr) {
    uint32_t viewConfigCount;
    xrEnumerateViewConfigurations(oxr->instance, oxr->systemId, 0, &viewConfigCount, nullptr);
    std::vector<XrViewConfigurationType> viewConfigs(viewConfigCount);
    xrEnumerateViewConfigurations(oxr->instance, oxr->systemId, viewConfigCount, &viewConfigCount, viewConfigs.data());
    oxr->viewConfigType = viewConfigs[0];
    LOGI("Using view configuration type: %d", oxr->viewConfigType);

    uint32_t blendModeCount;
    xrEnumerateEnvironmentBlendModes(oxr->instance, oxr->systemId, oxr->viewConfigType, 0, &blendModeCount, nullptr);
    std::vector<XrEnvironmentBlendMode> blendModes(blendModeCount);
    xrEnumerateEnvironmentBlendModes(oxr->instance, oxr->systemId, oxr->viewConfigType, blendModeCount, &blendModeCount, blendModes.data());
    oxr->blendMode = blendModes[0];
    LOGI("Using environment blend mode: %d", oxr->blendMode);

    PFN_xrGetOpenGLESGraphicsRequirementsKHR xrGetOpenGLESGraphicsRequirementsKHR = nullptr;
    xrGetInstanceProcAddr(oxr->instance, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&xrGetOpenGLESGraphicsRequirementsKHR);
    XrGraphicsRequirementsOpenGLESKHR graphicsRequirements = {XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    xrGetOpenGLESGraphicsRequirementsKHR(oxr->instance, oxr->systemId, &graphicsRequirements);

    XrGraphicsBindingOpenGLESAndroidKHR graphicsBinding = {XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    graphicsBinding.display = eglGetCurrentDisplay();
    graphicsBinding.context = eglGetCurrentContext();

    // Create overlay session using XR_EXTX_overlay extension
    XrSessionCreateInfoOverlayEXTX overlayCreateInfo = {XR_TYPE_SESSION_CREATE_INFO_OVERLAY_EXTX};
    overlayCreateInfo.next = &graphicsBinding;  // Graphics binding goes in overlay struct's next
    overlayCreateInfo.createFlags = 0;
    overlayCreateInfo.sessionLayersPlacement = 1;  // Place overlay on top

    XrSessionCreateInfo sessionCreateInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionCreateInfo.next = &overlayCreateInfo;  // Overlay info goes in session create next
    sessionCreateInfo.systemId = oxr->systemId;

    XrResult result = xrCreateSession(oxr->instance, &sessionCreateInfo, &oxr->session);
    if (XR_FAILED(result)) {
        LOGE("Failed to create OpenXR overlay session: %d", result);
        return false;
    }

    XrReferenceSpaceCreateInfo spaceCreateInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceCreateInfo.poseInReferenceSpace = {{0,0,0,1}, {0,0,0}};
    result = xrCreateReferenceSpace(oxr->session, &spaceCreateInfo, &oxr->appSpace);
    if (XR_FAILED(result)) {
        LOGE("Failed to create reference space: %d", result);
        return false;
    }

    return true;
}

bool createSwapchains(OpenXrApp* oxr) {
    if (oxr->swapchainsCreated) return true;
    LOGI("Creating %d swapchains...", LAYER_COUNT);

    for (uint32_t i = 0; i < LAYER_COUNT; ++i) {
        XrSwapchainCreateInfo swapchainCreateInfo = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
        swapchainCreateInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        swapchainCreateInfo.format = GL_RGBA8;
        swapchainCreateInfo.width = oxr->swapchainWidths[i];
        swapchainCreateInfo.height = oxr->swapchainHeights[i];
        swapchainCreateInfo.sampleCount = 1;
        swapchainCreateInfo.faceCount = 1;
        swapchainCreateInfo.arraySize = 1;
        swapchainCreateInfo.mipCount = 1;

        xrCreateSwapchain(oxr->session, &swapchainCreateInfo, &oxr->swapchains[i]);

        uint32_t image_count;
        xrEnumerateSwapchainImages(oxr->swapchains[i], 0, &image_count, nullptr);
        std::vector<XrSwapchainImageOpenGLESKHR> swapchain_images(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        xrEnumerateSwapchainImages(oxr->swapchains[i], image_count, &image_count, (XrSwapchainImageBaseHeader*)swapchain_images.data());

        oxr->framebuffers[i].resize(image_count);
        glGenFramebuffers(image_count, oxr->framebuffers[i].data());
        for (uint32_t j = 0; j < image_count; ++j) {
            glBindFramebuffer(GL_FRAMEBUFFER, oxr->framebuffers[i][j]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, swapchain_images[j].image, 0);
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    oxr->swapchainsCreated = true;
    LOGI("All swapchains created successfully.");
    return true;
}

void pollEvents(OpenXrApp* oxr) {
    XrEventDataBuffer eventData = {XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(oxr->instance, &eventData) == XR_SUCCESS) {
        if (eventData.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto stateEvent = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&eventData);
            oxr->sessionState = stateEvent.state;
            switch (oxr->sessionState) {
                case XR_SESSION_STATE_READY: {
                    XrSessionBeginInfo beginInfo = {XR_TYPE_SESSION_BEGIN_INFO};
                    beginInfo.primaryViewConfigurationType = oxr->viewConfigType;
                    if (XR_SUCCEEDED(xrBeginSession(oxr->session, &beginInfo))) {
                        oxr->sessionRunning = true;
                        createSwapchains(oxr);
                    }
                } break;
                case XR_SESSION_STATE_STOPPING:
                    oxr->sessionRunning = false;
                    xrEndSession(oxr->session);
                    break;
                case XR_SESSION_STATE_EXITING:
                    ANativeActivity_finish(oxr->app->activity);
                    break;
                default: break;
            }
        }
        eventData = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

void renderFrame(OpenXrApp* oxr) {
    if (!oxr->sessionRunning || !oxr->swapchainsCreated || !oxr->resumed) return;

    // --- Animation Logic ---
    // Advance the timer by a fixed amount, assuming ~60fps for simplicity
    oxr->stage_timer += 0.0166f;
    // After 1.2 seconds, advance to the next stage of the animation
    if (oxr->stage_timer > 1.2f && oxr->animation_stage < 4) {
        oxr->animation_stage++;
        oxr->stage_timer = 0.0f; // Reset timer for the next stage
    }

    XrFrameState frameState = {XR_TYPE_FRAME_STATE};
    xrWaitFrame(oxr->session, nullptr, &frameState);
    xrBeginFrame(oxr->session, nullptr);

    std::vector<XrCompositionLayerBaseHeader*> layers;

    if (frameState.shouldRender) {
        // --- Render content to each swapchain ---
        float colors[LAYER_COUNT][4] = {
                {0.0f, 1.0f, 1.0f, 1.0f}, // Cyan
                {0.0f, 0.0f, 0.8f, 1.0f}, // Blue
                {1.0f, 0.0f, 1.0f, 1.0f}, // Magenta
                {0.0f, 1.0f, 0.0f, 1.0f}  // Green
        };

        for (uint32_t i = 0; i < LAYER_COUNT; ++i) {
            uint32_t imageIndex;
            xrAcquireSwapchainImage(oxr->swapchains[i], nullptr, &imageIndex);
            XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
                                                 reinterpret_cast<const void *>(XR_INFINITE_DURATION)};
            xrWaitSwapchainImage(oxr->swapchains[i], &waitInfo);
            glBindFramebuffer(GL_FRAMEBUFFER, oxr->framebuffers[i][imageIndex]);
            glViewport(0, 0, oxr->swapchainWidths[i], oxr->swapchainHeights[i]);
            glClearColor(colors[i][0], colors[i][1], colors[i][2], colors[i][3]);
            glClear(GL_COLOR_BUFFER_BIT);
            xrReleaseSwapchainImage(oxr->swapchains[i], nullptr);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // --- Define Layers and Animate Them ---
        static XrCompositionLayerQuad quadLayers[LAYER_COUNT];

        // Layer 0: Cyan Background (Always visible)
        quadLayers[0] = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        quadLayers[0].space = oxr->appSpace;
        quadLayers[0].subImage = {{oxr->swapchains[0]}, {{0,0}, {(int32_t)oxr->swapchainWidths[0], (int32_t)oxr->swapchainHeights[0]}}};
        quadLayers[0].pose = {{0,0,0,1}, {0, 0, -2.0f}};
        quadLayers[0].size = {2.0f, 2.0f};
        layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&quadLayers[0]));

        // Layer 1: Blue
        if (oxr->animation_stage >= 1) {
            quadLayers[1] = {XR_TYPE_COMPOSITION_LAYER_QUAD};
            quadLayers[1].space = oxr->appSpace;
            quadLayers[1].subImage = {{oxr->swapchains[1]}, {{0,0}, {(int32_t)oxr->swapchainWidths[1], (int32_t)oxr->swapchainHeights[1]}}};
            quadLayers[1].pose = {{0,0,0,1}, {-0.4f, 0.5f, -1.5f}};
            float scale = (oxr->animation_stage == 1) ? std::min(1.0f, oxr->stage_timer / 0.5f) : 1.0f;
            quadLayers[1].size = {0.8f * scale, 0.4f * scale}; // Animate scale-in
            layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&quadLayers[1]));
        }

        // Layer 2: Magenta
        if (oxr->animation_stage >= 2) {
            quadLayers[2] = {XR_TYPE_COMPOSITION_LAYER_QUAD};
            quadLayers[2].space = oxr->appSpace;
            quadLayers[2].subImage = {{oxr->swapchains[2]}, {{0,0}, {(int32_t)oxr->swapchainWidths[2], (int32_t)oxr->swapchainHeights[2]}}};
            quadLayers[2].pose = {{0,0,0,1}, {-0.2f, -0.2f, -1.0f}};
            float scale = (oxr->animation_stage == 2) ? std::min(1.0f, oxr->stage_timer / 0.5f) : 1.0f;
            quadLayers[2].size = {0.4f * scale, 0.4f * scale}; // Animate scale-in
            layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&quadLayers[2]));
        }

        // Layer 3: Green
        if (oxr->animation_stage >= 3) {
            quadLayers[3] = {XR_TYPE_COMPOSITION_LAYER_QUAD};
            quadLayers[3].space = oxr->appSpace;
            quadLayers[3].subImage = {{oxr->swapchains[3]}, {{0,0}, {(int32_t)oxr->swapchainWidths[3], (int32_t)oxr->swapchainHeights[3]}}};
            quadLayers[3].pose = {{0,0,0,1}, {0.4f, 0.3f, -1.2f}};
            float scale = (oxr->animation_stage == 3) ? std::min(1.0f, oxr->stage_timer / 0.5f) : 1.0f;
            quadLayers[3].size = {0.5f * scale, 0.5f * scale}; // Animate scale-in
            layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&quadLayers[3]));
        }
    }

    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = oxr->blendMode;
    endInfo.layerCount = static_cast<uint32_t>(layers.size());
    endInfo.layers = layers.data();
    xrEndFrame(oxr->session, &endInfo);
}

void android_main(struct android_app* app) {
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(display, nullptr, nullptr);
    EGLConfig config;
    const EGLint configAttribs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE };
    EGLint numConfigs;
    eglChooseConfig(display, configAttribs, &config, 1, &numConfigs);
    const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);

    OpenXrApp oxr = {};
    oxr.app = app;
    app->userData = &oxr;

    app->onAppCmd = [](struct android_app* app, int32_t cmd) {
        auto* oxr_ptr = (OpenXrApp*)app->userData;
        if (cmd == APP_CMD_RESUME) oxr_ptr->resumed = true;
        if (cmd == APP_CMD_PAUSE) oxr_ptr->resumed = false;
    };

    // Modified input handler to reset the animation on tap
    app->onInputEvent = [](struct android_app* app, AInputEvent* event) -> int32_t {
        auto* oxr_ptr = (OpenXrApp*)app->userData;
        if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION) {
            if (AMotionEvent_getAction(event) == AMOTION_EVENT_ACTION_DOWN) {
                // Reset animation
                oxr_ptr->animation_stage = 0;
                oxr_ptr->stage_timer = 0.0f;
                LOGI("Animation reset by user.");
            }
            return 1;
        }
        return 0;
    };

    if (!initializeOpenXR(&oxr) || !createSession(&oxr)) {
        if(oxr.instance) xrDestroyInstance(oxr.instance);
        return;
    }

    while (!app->destroyRequested) {
        struct android_poll_source* source;
        while (ALooper_pollOnce(0, nullptr, nullptr, (void**)&source) >= 0) {
            if (source) source->process(app, source);
            if (app->destroyRequested) break;
        }
        pollEvents(&oxr);
        renderFrame(&oxr);
    }

    for (uint32_t i = 0; i < LAYER_COUNT; ++i) {
        glDeleteFramebuffers(oxr.framebuffers[i].size(), oxr.framebuffers[i].data());
        if (oxr.swapchains[i]) xrDestroySwapchain(oxr.swapchains[i]);
    }

    if (oxr.appSpace) xrDestroySpace(oxr.appSpace);
    if (oxr.session) xrDestroySession(oxr.session);
    if (oxr.instance) xrDestroyInstance(oxr.instance);
    eglDestroyContext(display, context);
    eglTerminate(display);
}