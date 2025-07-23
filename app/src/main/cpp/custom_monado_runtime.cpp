// custom_monado_runtime.cpp
// Custom Monado Runtime Implementation with OpenGL Drawing

#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <algorithm>

#define LOG_TAG "CustomMonadoRuntime"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// --- GLSL Shaders for drawing colored quads ---
const char* vertexShaderSource = R"(#version 300 es
layout (location = 0) in vec2 aPos;
uniform mat4 model;
void main() {
    gl_Position = model * vec4(aPos, 0.0, 1.0);
}
)";

const char* fragmentShaderSource = R"(#version 300 es
precision mediump float;
out vec4 FragColor;
uniform vec3 color;
void main() {
    FragColor = vec4(color, 1.0);
}
)";


// Custom OpenXR Types (bypassing standard loader)
typedef uint64_t XrFlags64;
typedef uint32_t XrBool32;
typedef uint64_t XrTime;
typedef uint64_t XrDuration;
typedef int64_t XrSystemId;

#define XR_SUCCESS 0
#define XR_ERROR_VALIDATION_FAILURE 1
#define XR_NULL_HANDLE nullptr

struct XrInstance_T { int dummy; };
struct XrSession_T { int dummy; };
struct XrSpace_T { int dummy; };
struct XrSwapchain_T { int dummy; };

typedef XrInstance_T* XrInstance;
typedef XrSession_T* XrSession;
typedef XrSpace_T* XrSpace;
typedef XrSwapchain_T* XrSwapchain;

// OverlayLayer struct now includes color
struct OverlayLayer {
    int id;
    XrSession session;
    float x, y, scale;
    float color[3];

    OverlayLayer() : id(0), session(nullptr), x(0), y(0), scale(1.0f), color{1,1,1} {}
};

// --- Matrix Math Helpers ---
void matrix_identity(float* m) {
    m[0] = 1; m[4] = 0; m[8] = 0;  m[12] = 0;
    m[1] = 0; m[5] = 1; m[9] = 0;  m[13] = 0;
    m[2] = 0; m[6] = 0; m[10] = 1; m[14] = 0;
    m[3] = 0; m[7] = 0; m[11] = 0; m[15] = 1;
}

void matrix_translate(float x, float y, float z, float* m) {
    matrix_identity(m);
    m[12] = x; m[13] = y; m[14] = z;
}

void matrix_scale(float sx, float sy, float sz, float* m) {
    matrix_identity(m);
    m[0] = sx; m[5] = sy; m[10] = sz;
}

void matrix_multiply(const float* a, const float* b, float* r) {
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            r[i * 4 + j] = 0;
            for (int k = 0; k < 4; ++k) {
                r[i * 4 + j] += a[k * 4 + j] * b[i * 4 + k];
            }
        }
    }
}


// Custom Monado Runtime State
class CustomMonadoRuntime {
public:
    static CustomMonadoRuntime& getInstance() {
        static CustomMonadoRuntime instance;
        return instance;
    }

    bool initialize(ANativeWindow* window) {
        LOGI("Initializing Custom Monado Runtime");
        if (!window) {
            LOGE("Invalid window provided");
            return false;
        }

        if (!initializeEGL(window)) {
            LOGE("Failed to initialize EGL");
            return false;
        }

        // --- NEW: Setup shaders and geometry ---
        if(!initializeGraphics()) {
            LOGE("Failed to initialize graphics resources");
            return false;
        }

        m_initialized = true;
        LOGI("Custom Monado Runtime initialized successfully");
        return true;
    }
    bool updateOverlayPosition(int layerId, float x, float y) {
        auto it = m_overlayLayers.find(layerId);
        if (it == m_overlayLayers.end()) return false;

        it->second.x = x;
        it->second.y = y;
        return true;
    }

    bool updateOverlayScale(int layerId, float scale) {
        auto it = m_overlayLayers.find(layerId);
        if (it == m_overlayLayers.end()) return false;

        it->second.scale = scale;
        return true;
    }

    bool updateOverlayColor(int layerId, float r, float g, float b) {
        auto it = m_overlayLayers.find(layerId);
        if (it == m_overlayLayers.end()) return false;

        it->second.color[0] = r;
        it->second.color[1] = g;
        it->second.color[2] = b;
        return true;
    }

    void cleanup() {
        if (m_initialized) {
            m_overlayLayers.clear();

            // --- NEW: Cleanup graphics resources ---
            if (m_shaderProgram != 0) glDeleteProgram(m_shaderProgram);
            if (m_quadVAO != 0) glDeleteVertexArrays(1, &m_quadVAO);
            if (m_quadVBO != 0) glDeleteBuffers(1, &m_quadVBO);

            if (m_eglDisplay != EGL_NO_DISPLAY) {
                eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                if (m_eglContext != EGL_NO_CONTEXT) eglDestroyContext(m_eglDisplay, m_eglContext);
                if (m_eglSurface != EGL_NO_SURFACE) eglDestroySurface(m_eglDisplay, m_eglSurface);
                eglTerminate(m_eglDisplay);
            }
            m_initialized = false;
            LOGI("Custom Monado Runtime cleaned up");
        }
    }

    XrInstance createInstance() {
        if (!m_initialized) return XR_NULL_HANDLE;
        static uint64_t instanceCounter = 0x12345678;
        XrInstance instance = reinterpret_cast<XrInstance>(instanceCounter++);
        return instance;
    }

    XrSession createSession(XrInstance instance, ANativeWindow* window) {
        if (!window) return XR_NULL_HANDLE;
        static uint64_t sessionCounter = 0x87654321;
        XrSession session = reinterpret_cast<XrSession>(sessionCounter++);
        m_sessionWindows[session] = window;
        return session;
    }

    bool beginFrame(XrSession session) {
        if (m_sessionWindows.find(session) == m_sessionWindows.end()) return false;
        m_frameCount++;

        // Clear frame buffer to CYAN to match the problem statement
        glClearColor(0.0f, 1.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        return true;
    }

    bool endFrame(XrSession session) {
        if (m_sessionWindows.find(session) == m_sessionWindows.end()) return false;

        // Render overlays before swapping buffers
        renderOverlays();

        if (!eglSwapBuffers(m_eglDisplay, m_eglSurface)) {
            LOGE("Failed to swap EGL buffers");
            return false;
        }
        return true;
    }

    bool createOverlay(XrSession session, int layerId) {
        if (m_overlayLayers.find(layerId) != m_overlayLayers.end()) return false;

        OverlayLayer layer;
        layer.id = layerId;
        layer.session = session;
        layer.scale = 0.25f; // Set a uniform scale

        // Set positions and colors based on the problem statement
        switch (layerId) {
            case 0: // Blue
                layer.x = -0.5f; layer.y = 0.5f;
                layer.color[0] = 0.0f; layer.color[1] = 0.0f; layer.color[2] = 1.0f;
                break;
            case 1: // Magenta
                layer.x = 0.0f; layer.y = -0.25f;
                layer.color[0] = 1.0f; layer.color[1] = 0.0f; layer.color[2] = 1.0f;
                break;
            case 2: // Green
                layer.x = 0.5f; layer.y = 0.25f;
                layer.color[0] = 0.0f; layer.color[1] = 1.0f; layer.color[2] = 0.0f;
                break;
            default: // Don't create more than 3 visible overlays
                return true;
        }

        m_overlayLayers[layerId] = layer;
        LOGI("Created overlay layer %d", layerId);
        return true;
    }

    int getFrameCount() const { return m_frameCount; }
    std::string getRuntimeInfo() const { return "Custom Monado Runtime v2.0 - With GL"; }
    std::vector<std::string> getSupportedExtensions() const { return {"XR_EXTX_overlay"}; }

private:
    bool m_initialized = false;
    int m_frameCount = 0;

    EGLDisplay m_eglDisplay = EGL_NO_DISPLAY;
    EGLContext m_eglContext = EGL_NO_CONTEXT;
    EGLSurface m_eglSurface = EGL_NO_SURFACE;

    // --- NEW: Graphics resources ---
    GLuint m_shaderProgram = 0;
    GLuint m_quadVAO = 0;
    GLuint m_quadVBO = 0;
    GLint m_modelMatrixLocation = -1;
    GLint m_colorLocation = -1;

    std::map<XrSession, ANativeWindow*> m_sessionWindows;
    std::map<int, OverlayLayer> m_overlayLayers;

    bool initializeEGL(ANativeWindow* window) {
        m_eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (!eglInitialize(m_eglDisplay, nullptr, nullptr)) return false;

        EGLint configAttribs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
        EGLConfig config;
        EGLint numConfigs;
        if (!eglChooseConfig(m_eglDisplay, configAttribs, &config, 1, &numConfigs) || numConfigs == 0) return false;

        m_eglSurface = eglCreateWindowSurface(m_eglDisplay, config, window, nullptr);
        if (m_eglSurface == EGL_NO_SURFACE) return false;

        EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
        m_eglContext = eglCreateContext(m_eglDisplay, config, EGL_NO_CONTEXT, contextAttribs);
        if (m_eglContext == EGL_NO_CONTEXT) return false;

        if (!eglMakeCurrent(m_eglDisplay, m_eglSurface, m_eglSurface, m_eglContext)) return false;

        EGLint width, height;
        eglQuerySurface(m_eglDisplay, m_eglSurface, EGL_WIDTH, &width);
        eglQuerySurface(m_eglDisplay, m_eglSurface, EGL_HEIGHT, &height);
        glViewport(0, 0, width, height);

        LOGI("EGL initialized successfully");
        return true;
    }

    // --- NEW: Shader/Geometry Initialization ---
    bool initializeGraphics() {
        // Compile shaders
        GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vertexShader, 1, &vertexShaderSource, nullptr);
        glCompileShader(vertexShader);
        // (Error checking omitted for brevity)

        GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fragmentShader, 1, &fragmentShaderSource, nullptr);
        glCompileShader(fragmentShader);
        // (Error checking omitted for brevity)

        // Link program
        m_shaderProgram = glCreateProgram();
        glAttachShader(m_shaderProgram, vertexShader);
        glAttachShader(m_shaderProgram, fragmentShader);
        glLinkProgram(m_shaderProgram);
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
        // (Error checking omitted for brevity)

        m_modelMatrixLocation = glGetUniformLocation(m_shaderProgram, "model");
        m_colorLocation = glGetUniformLocation(m_shaderProgram, "color");

        // Create quad geometry (a simple square)
        float vertices[] = {
                -0.5f, -0.5f,
                0.5f, -0.5f,
                0.5f,  0.5f,
                -0.5f,  0.5f
        };
        unsigned int indices[] = { 0, 1, 2, 2, 3, 0 };

        glGenVertexArrays(1, &m_quadVAO);
        glGenBuffers(1, &m_quadVBO);
        GLuint EBO;
        glGenBuffers(1, &EBO);

        glBindVertexArray(m_quadVAO);
        glBindBuffer(GL_ARRAY_BUFFER, m_quadVBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glBindVertexArray(0);

        return true;
    }

    // --- IMPLEMENTED: Actual overlay drawing ---
    void renderOverlayLayer(const OverlayLayer& layer) {
        float scaleMatrix[16], transMatrix[16], modelMatrix[16];
        matrix_scale(layer.scale, layer.scale, 1.0f, scaleMatrix);
        matrix_translate(layer.x, layer.y, 0.0f, transMatrix);
        matrix_multiply(transMatrix, scaleMatrix, modelMatrix);

        glUniformMatrix4fv(m_modelMatrixLocation, 1, GL_FALSE, modelMatrix);
        glUniform3fv(m_colorLocation, 1, layer.color);

        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    }

    // --- Calls renderOverlayLayer for each overlay ---
    void renderOverlays() {
        glUseProgram(m_shaderProgram);
        glBindVertexArray(m_quadVAO);
        for (auto const& [id, layer] : m_overlayLayers) {
            renderOverlayLayer(layer);
        }
        glBindVertexArray(0);
    }
};

// --- JNI Interface (no changes needed here) ---
extern "C" {
JNIEXPORT jboolean JNICALL
Java_com_example_androidsamsung_MainActivity_initializeCustomRuntime(JNIEnv *env, jobject thiz,
                                                                     jobject surface) {
    if (!surface) return JNI_FALSE;
    ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
    if (!window) return JNI_FALSE;
    return CustomMonadoRuntime::getInstance().initialize(window) ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jlong JNICALL
Java_com_example_androidsamsung_MainActivity_createXRInstance(JNIEnv *env, jobject thiz) {
    return reinterpret_cast<jlong>(CustomMonadoRuntime::getInstance().createInstance());
}
JNIEXPORT jlong JNICALL
Java_com_example_androidsamsung_MainActivity_createXRSession(JNIEnv *env, jobject thiz,
                                                             jlong instanceHandle,
                                                             jobject surface) {
    ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
    return reinterpret_cast<jlong>(CustomMonadoRuntime::getInstance().createSession(
            reinterpret_cast<XrInstance>(instanceHandle), window));
}
JNIEXPORT jboolean JNICALL
Java_com_example_androidsamsung_MainActivity_beginXRFrame(JNIEnv *env, jobject thiz,
                                                          jlong sessionHandle) {
    return CustomMonadoRuntime::getInstance().beginFrame(reinterpret_cast<XrSession>(sessionHandle))
           ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jboolean JNICALL
Java_com_example_androidsamsung_MainActivity_endXRFrame(JNIEnv *env, jobject thiz,
                                                        jlong sessionHandle) {
    return CustomMonadoRuntime::getInstance().endFrame(reinterpret_cast<XrSession>(sessionHandle))
           ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jboolean JNICALL
Java_com_example_androidsamsung_MainActivity_createOverlay(JNIEnv *env, jobject thiz,
                                                           jlong sessionHandle, jint layerId) {
    return CustomMonadoRuntime::getInstance().createOverlay(
            reinterpret_cast<XrSession>(sessionHandle), layerId) ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jint JNICALL
Java_com_example_androidsamsung_MainActivity_getFrameCount(JNIEnv *env, jobject thiz) {
    return CustomMonadoRuntime::getInstance().getFrameCount();
}
JNIEXPORT jstring JNICALL
Java_com_example_androidsamsung_MainActivity_getRuntimeInfo(JNIEnv *env, jobject thiz) {
    return env->NewStringUTF(CustomMonadoRuntime::getInstance().getRuntimeInfo().c_str());
}
JNIEXPORT jobjectArray JNICALL
Java_com_example_androidsamsung_MainActivity_getSupportedExtensions(JNIEnv *env, jobject thiz) {
    auto extensions = CustomMonadoRuntime::getInstance().getSupportedExtensions();
    jobjectArray result = env->NewObjectArray(extensions.size(), env->FindClass("java/lang/String"),
                                              nullptr);
    for (size_t i = 0; i < extensions.size(); i++) {
        env->SetObjectArrayElement(result, i, env->NewStringUTF(extensions[i].c_str()));
    }
    return result;
}
JNIEXPORT void JNICALL
Java_com_example_androidsamsung_MainActivity_destroyCustomRuntime(JNIEnv *env, jobject thiz) {
    CustomMonadoRuntime::getInstance().cleanup();
}
JNIEXPORT void JNICALL Java_com_example_androidsamsung_MainActivity_updateOverlayPosition(
        JNIEnv *env, jobject thiz, jint layerId, jfloat x, jfloat y) {
    CustomMonadoRuntime::getInstance().updateOverlayPosition(layerId, x, y);
}

JNIEXPORT void JNICALL Java_com_example_androidsamsung_MainActivity_updateOverlayScale(
        JNIEnv *env, jobject thiz, jint layerId, jfloat scale) {
    CustomMonadoRuntime::getInstance().updateOverlayScale(layerId, scale);
}

JNIEXPORT void JNICALL Java_com_example_androidsamsung_MainActivity_updateOverlayColor(
        JNIEnv *env, jobject thiz, jint layerId, jfloat r, jfloat g, jfloat b) {
    CustomMonadoRuntime::getInstance().updateOverlayColor(layerId, r, g, b);
}
}