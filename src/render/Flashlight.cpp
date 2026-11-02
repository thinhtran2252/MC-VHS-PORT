#include "render/Flashlight.h"

#include <android/log.h>
#include <dlfcn.h>
#include <pl/memory/Hook.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <vector>
#include <utility>

namespace mcvhs {
namespace {

constexpr char kLogTag[] = "MCVHS-FLASHLIGHT";
constexpr int kLogTextureWidth = 640;
constexpr int kLogTextureHeight = 160;
using EglSwapBuffersFn = EGLBoolean (*)(EGLDisplay, EGLSurface);
using ClearFn = void (*)(GLbitfield);
using ClearBufferFvFn = void (*)(GLenum, GLint, const GLfloat*);
using ClearBufferFiFn = void (*)(GLenum, GLint, GLfloat, GLint);

void androidLog(int priority, const char* message) {
    __android_log_print(priority, kLogTag, "%s", message ? message : "");
}

const char* glErrorName(GLenum error) {
    switch (error) {
        case GL_INVALID_ENUM: return "INVALID_ENUM";
        case GL_INVALID_VALUE: return "INVALID_VALUE";
        case GL_INVALID_OPERATION: return "INVALID_OPERATION";
        case GL_OUT_OF_MEMORY: return "OUT_OF_MEMORY";
#ifdef GL_INVALID_FRAMEBUFFER_OPERATION
        case GL_INVALID_FRAMEBUFFER_OPERATION: return "INVALID_FRAMEBUFFER_OPERATION";
#endif
        default: return "UNKNOWN";
    }
}

std::array<std::uint8_t, 5> glyphColumns(char input) {
    const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(input)));
    switch (c) {
        case 'A': return {0x7e,0x11,0x11,0x11,0x7e};
        case 'B': return {0x7f,0x49,0x49,0x49,0x36};
        case 'C': return {0x3e,0x41,0x41,0x41,0x22};
        case 'D': return {0x7f,0x41,0x41,0x22,0x1c};
        case 'E': return {0x7f,0x49,0x49,0x49,0x41};
        case 'F': return {0x7f,0x09,0x09,0x09,0x01};
        case 'G': return {0x3e,0x41,0x49,0x49,0x7a};
        case 'H': return {0x7f,0x08,0x08,0x08,0x7f};
        case 'I': return {0x00,0x41,0x7f,0x41,0x00};
        case 'J': return {0x20,0x40,0x41,0x3f,0x01};
        case 'K': return {0x7f,0x08,0x14,0x22,0x41};
        case 'L': return {0x7f,0x40,0x40,0x40,0x40};
        case 'M': return {0x7f,0x02,0x0c,0x02,0x7f};
        case 'N': return {0x7f,0x04,0x08,0x10,0x7f};
        case 'O': return {0x3e,0x41,0x41,0x41,0x3e};
        case 'P': return {0x7f,0x09,0x09,0x09,0x06};
        case 'Q': return {0x3e,0x41,0x51,0x21,0x5e};
        case 'R': return {0x7f,0x09,0x19,0x29,0x46};
        case 'S': return {0x46,0x49,0x49,0x49,0x31};
        case 'T': return {0x01,0x01,0x7f,0x01,0x01};
        case 'U': return {0x3f,0x40,0x40,0x40,0x3f};
        case 'V': return {0x1f,0x20,0x40,0x20,0x1f};
        case 'W': return {0x3f,0x40,0x30,0x40,0x3f};
        case 'X': return {0x63,0x14,0x08,0x14,0x63};
        case 'Y': return {0x07,0x08,0x70,0x08,0x07};
        case 'Z': return {0x61,0x51,0x49,0x45,0x43};
        case '0': return {0x3e,0x51,0x49,0x45,0x3e};
        case '1': return {0x00,0x42,0x7f,0x40,0x00};
        case '2': return {0x42,0x61,0x51,0x49,0x46};
        case '3': return {0x21,0x41,0x45,0x4b,0x31};
        case '4': return {0x18,0x14,0x12,0x7f,0x10};
        case '5': return {0x27,0x45,0x45,0x45,0x39};
        case '6': return {0x3c,0x4a,0x49,0x49,0x30};
        case '7': return {0x01,0x71,0x09,0x05,0x03};
        case '8': return {0x36,0x49,0x49,0x49,0x36};
        case '9': return {0x06,0x49,0x49,0x29,0x1e};
        case ':': return {0x00,0x36,0x36,0x00,0x00};
        case '.': return {0x00,0x60,0x60,0x00,0x00};
        case ',': return {0x00,0x40,0x30,0x00,0x00};
        case '-': return {0x08,0x08,0x08,0x08,0x08};
        case '+': return {0x08,0x08,0x3e,0x08,0x08};
        case '/': return {0x60,0x18,0x06,0x01,0x00};
        case '_': return {0x40,0x40,0x40,0x40,0x40};
        case '!': return {0x00,0x00,0x5f,0x00,0x00};
        case '?': return {0x02,0x01,0x51,0x09,0x06};
        case '=': return {0x14,0x14,0x14,0x14,0x14};
        case '(': return {0x00,0x1c,0x22,0x41,0x00};
        case ')': return {0x00,0x41,0x22,0x1c,0x00};
        case ' ': default: return {0,0,0,0,0};
    }
}

struct SavedGlState {
    GLint drawFramebuffer{};
    GLint readFramebuffer{};
    GLint readBuffer{};
    GLint viewport[4]{};
    GLint program{};
    GLint vertexArray{};
    GLint activeTexture{};
    GLint textures[3]{};
    GLint samplers[3]{};
    GLint pixelUnpackBuffer{};
    GLint unpackAlignment{};
    GLboolean colorMask[4]{GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLboolean blend{};
    GLboolean depth{};
    GLboolean stencil{};
    GLboolean scissor{};
    GLboolean cull{};

    void capture() {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
        glGetIntegerv(GL_READ_BUFFER, &readBuffer);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertexArray);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &pixelUnpackBuffer);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackAlignment);
        for (int i = 0; i < 3; ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &textures[i]);
            glGetIntegerv(GL_SAMPLER_BINDING, &samplers[i]);
        }
        glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
        blend = glIsEnabled(GL_BLEND);
        depth = glIsEnabled(GL_DEPTH_TEST);
        stencil = glIsEnabled(GL_STENCIL_TEST);
        scissor = glIsEnabled(GL_SCISSOR_TEST);
        cull = glIsEnabled(GL_CULL_FACE);
        glActiveTexture(static_cast<GLenum>(activeTexture));
    }

    void restore() const {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFramebuffer));
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFramebuffer));
        glReadBuffer(static_cast<GLenum>(readBuffer));
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glUseProgram(static_cast<GLuint>(program));
        glBindVertexArray(static_cast<GLuint>(vertexArray));
        for (int i = 0; i < 3; ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(textures[i]));
            glBindSampler(i, static_cast<GLuint>(samplers[i]));
        }
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(pixelUnpackBuffer));
        glPixelStorei(GL_UNPACK_ALIGNMENT, unpackAlignment);
        glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
        blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
        depth ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
        stencil ? glEnable(GL_STENCIL_TEST) : glDisable(GL_STENCIL_TEST);
        scissor ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
        cull ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
        glActiveTexture(static_cast<GLenum>(activeTexture));
    }
};

struct ScopedGlState {
    SavedGlState state;
    ScopedGlState() { state.capture(); }
    ~ScopedGlState() { state.restore(); }
};

constexpr char kVertexShader[] = R"GLSL(#version 300 es
precision highp float;
out vec2 vTexCoord;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vTexCoord = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

constexpr char kFragmentShader[] = R"GLSL(#version 300 es
precision highp float;
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D uScene;
uniform sampler2D uDepth;
uniform sampler2D uLogs;
uniform vec2 uResolution;
uniform int uFlashlightEnabled;
uniform int uHasDepth;
uniform int uReverseDepth;
uniform int uShowLogs;
uniform float uBrightness;
uniform float uDistance;

float beamProfile(vec2 uv) {
    vec2 p = (uv - vec2(0.5)) * vec2(uResolution.x / max(uResolution.y, 1.0), 1.0);
    float radius = length(p);
    float hotspot = 1.0 - smoothstep(0.015, 0.22, radius);
    float halo = smoothstep(0.07, 0.13, radius) * (1.0 - smoothstep(0.14, 0.40, radius)) * 0.52;
    float spill = (1.0 - smoothstep(0.16, 0.82, radius)) * 0.18;
    return clamp(max(hotspot, max(halo, spill)), 0.0, 1.0);
}

vec3 flashlight(vec3 scene, vec2 uv) {
    float depthSample = texture(uDepth, uv).r;
    if (uReverseDepth != 0) depthSample = 1.0 - depthSample;
    const float nearPlane = 0.05;
    const float farPlane = 512.0;
    float denominator = depthSample * (nearPlane - farPlane) + farPlane;
    float linearDepth = (nearPlane * farPlane) / max(denominator, 0.000001);
    float fade = clamp(1.0 - sqrt(max(linearDepth, 0.0)) / max(uDistance, 0.1), 0.0, 1.0);

    vec2 ndc = uv * 2.0 - 1.0;
    float aspect = uResolution.x / max(uResolution.y, 1.0);
    vec3 viewPosition = vec3(ndc.x * aspect * linearDepth, ndc.y * linearDepth, -linearDepth);
    vec3 dx = dFdx(viewPosition);
    vec3 dy = dFdy(viewPosition);
    vec3 normalVector = cross(dx, dy);
    float normalLength = length(normalVector);
    float facing = 0.65;
    if (normalLength > 0.000001) {
        vec3 normal = normalVector / normalLength;
        facing = 0.20 + 0.80 * abs(dot(normal, normalize(-viewPosition)));
    }
    float geometry = step(depthSample, 0.999999);

    float amount = beamProfile(uv) * fade * facing * geometry * uBrightness;
    const vec3 warmTint = vec3(1.0, 0.88, 0.70);
    vec3 reflectedScene = scene * amount * 0.82;
    vec3 lowLightFill = warmTint * amount * 0.18;
    return clamp(scene + reflectedScene + lowLightFill, vec3(0.0), vec3(1.0));
}

void main() {
    vec4 color = texture(uScene, vTexCoord);
    if (uFlashlightEnabled != 0 && uHasDepth != 0)
        color.rgb = flashlight(color.rgb, vTexCoord);

    if (uShowLogs != 0) {
        vec2 pixel = vTexCoord * uResolution;
        float fromTop = uResolution.y - pixel.y;
        vec2 logCoord = vec2(pixel.x / 640.0, fromTop / 160.0);
        if (pixel.x >= 0.0 && pixel.x < 640.0 && fromTop >= 0.0 && fromTop < 160.0) {
            vec4 logPixel = texture(uLogs, logCoord);
            color.rgb = mix(color.rgb, logPixel.rgb, logPixel.a);
        }
    }
    fragColor = color;
}
)GLSL";

} // namespace

FlashlightRenderer& FlashlightRenderer::instance() {
    static FlashlightRenderer instance;
    return instance;
}

void FlashlightRenderer::appendLog(const char* message) {
    if (!message) return;
    std::string line;
    line.reserve(40);
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(message); *p && line.size() < 34; ++p) {
        const char c = static_cast<char>(std::toupper(*p));
        line.push_back((c >= 32 && c <= 126) ? c : ' ');
    }
    if (line.empty()) return;
    {
        std::lock_guard<std::mutex> lock(mLogMutex);
        if (mLogCount < mLogLines.size()) {
            const std::size_t index = (mLogStart + mLogCount) % mLogLines.size();
            mLogLines[index] = std::move(line);
            ++mLogCount;
        } else {
            mLogLines[mLogStart] = std::move(line);
            mLogStart = (mLogStart + 1) % mLogLines.size();
        }
        ++mLogRevision;
    }
}

void FlashlightRenderer::setConfig(const ModConfig& config) {
    ModConfig previous;
    {
        std::lock_guard<std::mutex> lock(mConfigMutex);
        previous = mConfig;
        mConfig = config;
    }
    if (previous.Optimize != config.Optimize)
        appendLog(config.Optimize ? "OPTIMIZE ON" : "OPTIMIZE OFF");
    if (previous.USE_FLASHLIGHT != config.USE_FLASHLIGHT) {
        mMissingDepthLogged.store(false, std::memory_order_release);
        if (!config.USE_FLASHLIGHT) {
            mDepthReady.store(false, std::memory_order_release);
        }
        appendLog(config.USE_FLASHLIGHT ? "FLASHLIGHT ENABLED" : "FLASHLIGHT DISABLED");
    }
    if (previous.DEV_LOGS != config.DEV_LOGS)
        appendLog(config.DEV_LOGS ? "DEV LOGS ENABLED" : "DEV LOGS DISABLED");
}

bool FlashlightRenderer::start() {
    std::lock_guard<std::mutex> lock(mHookMutex);
    if (mSwapHookInstalled) return true;

    if (!mEglLibrary) mEglLibrary = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
    if (!mGlLibrary) mGlLibrary = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL);
    if (!mEglLibrary || !mGlLibrary) {
        appendLog("HOOK LIBRARY OPEN FAILED");
        androidLog(ANDROID_LOG_ERROR, "Could not open libEGL.so or libGLESv2.so");
        return false;
    }

    mSwapTarget = dlsym(mEglLibrary, "eglSwapBuffers");
    mClearTarget = dlsym(mGlLibrary, "glClear");
    mClearBufferFvTarget = dlsym(mGlLibrary, "glClearBufferfv");
    mClearBufferFiTarget = dlsym(mGlLibrary, "glClearBufferfi");
    if (!mSwapTarget) {
        appendLog("EGL SWAP SYMBOL MISSING");
        return false;
    }

    void* originalSwap = nullptr;
    const int swapResult = pl::memory::hook(
        reinterpret_cast<pl::memory::FuncPtr>(mSwapTarget),
        reinterpret_cast<pl::memory::FuncPtr>(&FlashlightRenderer::swapBuffersHook),
        &originalSwap,
        pl::memory::HookPriority::Lowest);
    if (swapResult != 0 || !originalSwap) {
        char msg[48]{};
        std::snprintf(msg, sizeof(msg), "EGL SWAP HOOK FAIL %d", swapResult);
        appendLog(msg);
        return false;
    }
    mOriginalSwap.store(originalSwap, std::memory_order_release);
    mSwapHookInstalled = true;

    if (mClearTarget) {
        void* original = nullptr;
        const int result = pl::memory::hook(
            reinterpret_cast<pl::memory::FuncPtr>(mClearTarget),
            reinterpret_cast<pl::memory::FuncPtr>(&FlashlightRenderer::clearHook),
            &original, pl::memory::HookPriority::Normal);
        if (result == 0 && original) {
            mOriginalClear.store(original, std::memory_order_release);
            mClearHookInstalled = true;
        } else {
            appendLog("GLCLEAR HOOK FAILED");
        }
    } else {
        appendLog("GLCLEAR SYMBOL MISSING");
    }

    if (mClearBufferFvTarget) {
        void* original = nullptr;
        const int result = pl::memory::hook(
            reinterpret_cast<pl::memory::FuncPtr>(mClearBufferFvTarget),
            reinterpret_cast<pl::memory::FuncPtr>(&FlashlightRenderer::clearBufferFvHook),
            &original, pl::memory::HookPriority::Normal);
        if (result == 0 && original) {
            mOriginalClearBufferFv.store(original, std::memory_order_release);
            mClearBufferFvHookInstalled = true;
        } else {
            appendLog("CLEARBUFFERFV HOOK FAIL");
        }
    }

    if (mClearBufferFiTarget) {
        void* original = nullptr;
        const int result = pl::memory::hook(
            reinterpret_cast<pl::memory::FuncPtr>(mClearBufferFiTarget),
            reinterpret_cast<pl::memory::FuncPtr>(&FlashlightRenderer::clearBufferFiHook),
            &original, pl::memory::HookPriority::Normal);
        if (result == 0 && original) {
            mOriginalClearBufferFi.store(original, std::memory_order_release);
            mClearBufferFiHookInstalled = true;
        } else {
            appendLog("CLEARBUFFERFI HOOK FAIL");
        }
    }

    appendLog("FLASHLIGHT SWAP HOOK READY");
    appendLog(mClearHookInstalled || mClearBufferFvHookInstalled || mClearBufferFiHookInstalled
        ? "DEPTH CLEAR HOOK READY" : "DEPTH CLEAR HOOK MISSING");
    androidLog(ANDROID_LOG_INFO, "Flashlight swap and depth-clear hooks installed");
    return true;
}

void FlashlightRenderer::stop() {
    std::lock_guard<std::mutex> lock(mHookMutex);
    if (mClearHookInstalled) {
        pl::memory::unhook(reinterpret_cast<pl::memory::FuncPtr>(mClearTarget),
                           reinterpret_cast<pl::memory::FuncPtr>(&FlashlightRenderer::clearHook));
        mClearHookInstalled = false;
    }
    if (mClearBufferFvHookInstalled) {
        pl::memory::unhook(reinterpret_cast<pl::memory::FuncPtr>(mClearBufferFvTarget),
                           reinterpret_cast<pl::memory::FuncPtr>(&FlashlightRenderer::clearBufferFvHook));
        mClearBufferFvHookInstalled = false;
    }
    if (mClearBufferFiHookInstalled) {
        pl::memory::unhook(reinterpret_cast<pl::memory::FuncPtr>(mClearBufferFiTarget),
                           reinterpret_cast<pl::memory::FuncPtr>(&FlashlightRenderer::clearBufferFiHook));
        mClearBufferFiHookInstalled = false;
    }
    if (mSwapHookInstalled) {
        pl::memory::unhook(reinterpret_cast<pl::memory::FuncPtr>(mSwapTarget),
                           reinterpret_cast<pl::memory::FuncPtr>(&FlashlightRenderer::swapBuffersHook));
        mSwapHookInstalled = false;
    }
    appendLog("FLASHLIGHT HOOKS STOPPED");
}

void FlashlightRenderer::clearHook(GLbitfield mask) {
    auto& self = instance();
    self.beforeClear(mask);
    auto original = reinterpret_cast<ClearFn>(self.mOriginalClear.load(std::memory_order_acquire));
    if (original) original(mask);
}

void FlashlightRenderer::clearBufferFvHook(GLenum buffer, GLint drawBuffer, const GLfloat* values) {
    auto& self = instance();
    if (drawBuffer == 0) {
        if (buffer == GL_COLOR) self.beforeClear(GL_COLOR_BUFFER_BIT);
        else if (buffer == GL_DEPTH) self.beforeClear(GL_DEPTH_BUFFER_BIT);
    }
    auto original = reinterpret_cast<ClearBufferFvFn>(self.mOriginalClearBufferFv.load(std::memory_order_acquire));
    if (original) original(buffer, drawBuffer, values);
}

void FlashlightRenderer::clearBufferFiHook(GLenum buffer, GLint drawBuffer, GLfloat depth, GLint stencil) {
    auto& self = instance();
    if (buffer == GL_DEPTH_STENCIL) self.beforeClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    auto original = reinterpret_cast<ClearBufferFiFn>(self.mOriginalClearBufferFi.load(std::memory_order_acquire));
    if (original) original(buffer, drawBuffer, depth, stencil);
}

void FlashlightRenderer::beforeClear(GLbitfield mask) {
    ModConfig config;
    {
        std::lock_guard<std::mutex> lock(mConfigMutex);
        config = mConfig;
    }
    if (!config.enabled || !config.USE_FLASHLIGHT) return;
    if (!(mask & (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT))) return;

    const EGLContext context = eglGetCurrentContext();
    const EGLSurface surface = eglGetCurrentSurface(EGL_DRAW);
    const EGLDisplay display = eglGetCurrentDisplay();
    if (context == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE || display == EGL_NO_DISPLAY) return;

    GLint major = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    if (major < 3) return;

    EGLint width = 0, height = 0;
    if (eglQuerySurface(display, surface, EGL_WIDTH, &width) != EGL_TRUE ||
        eglQuerySurface(display, surface, EGL_HEIGHT, &height) != EGL_TRUE || width < 64 || height < 64) return;

    GLint viewport[4]{};
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (viewport[0] != 0 || viewport[1] != 0 || viewport[2] != width || viewport[3] != height) {
        if (!mMissingDepthLogged.load(std::memory_order_acquire)) appendLog("DEPTH VIEWPORT MISMATCH");
        mMissingDepthLogged.store(true, std::memory_order_release);
        return;
    }

    if (glIsEnabled(GL_SCISSOR_TEST)) {
        GLint scissor[4]{};
        glGetIntegerv(GL_SCISSOR_BOX, scissor);
        if (scissor[0] > 0 || scissor[1] > 0 ||
            scissor[0] + scissor[2] < width || scissor[1] + scissor[3] < height) {
            if (!mMissingDepthLogged.load(std::memory_order_acquire)) appendLog("DEPTH SCISSOR NOT FULLSCREEN");
            mMissingDepthLogged.store(true, std::memory_order_release);
            return;
        }
    }

    resetGlObjectsForNewContext(context);

    if (mTrackContext != context || mTrackSurface != surface) {
        mTrackContext = context;
        mTrackSurface = surface;
        mSceneStarted = false;
        mCaptureProcessed = false;
        mWorldFramebuffer = -1;
    }

    GLint framebuffer = 0;
    GLint currentProgram = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
    glGetIntegerv(GL_CURRENT_PROGRAM, &currentProgram);

    if (mask & GL_COLOR_BUFFER_BIT) {
        if (!mCaptureProcessed) {
            mSceneStarted = true;
            mWorldFramebuffer = framebuffer;
            mSceneStartProgram = currentProgram;
            mDepthReady.store(false, std::memory_order_release);
        }
        // A combined color+depth clear happens before the scene is drawn.
        return;
    }

    if (!mSceneStarted || mCaptureProcessed || framebuffer != mWorldFramebuffer) return;
    if (!currentProgram || currentProgram == mSceneStartProgram) return;

    mCaptureProcessed = true;
    GLint depthBits = 0, stencilBits = 0, sampleBuffers = 0, samples = 0;
    glGetIntegerv(GL_DEPTH_BITS, &depthBits);
    glGetIntegerv(GL_STENCIL_BITS, &stencilBits);
    glGetIntegerv(GL_SAMPLE_BUFFERS, &sampleBuffers);
    glGetIntegerv(GL_SAMPLES, &samples);
    GLint depthFunc = GL_LESS;
    glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
    const bool reverseDepth = depthFunc == GL_GREATER || depthFunc == GL_GEQUAL;
    mReverseDepth.store(reverseDepth, std::memory_order_release);

    if (depthBits <= 0) {
        if (!mMissingDepthLogged.load(std::memory_order_acquire)) appendLog("DEPTH BUFFER NOT EXPOSED");
        mMissingDepthLogged.store(true, std::memory_order_release);
        return;
    }
    if (sampleBuffers > 0 || samples > 0) {
        appendLog("DEPTH MSAA UNSUPPORTED");
        mMissingDepthLogged.store(true, std::memory_order_release);
        return;
    }

    if (!captureDepth(static_cast<GLuint>(framebuffer), display, surface,
                      width, height, depthBits, stencilBits)) {
        mDepthReady.store(false, std::memory_order_release);
        mMissingDepthLogged.store(true, std::memory_order_release);
    }
}

bool FlashlightRenderer::ensureDepthTargets(int width, int height, GLint depthBits, GLint stencilBits) {
    GLenum internalFormat = 0;
    GLenum uploadFormat = GL_DEPTH_COMPONENT;
    GLenum pixelType = GL_UNSIGNED_INT;
    GLenum attachment = GL_DEPTH_ATTACHMENT;

    if (stencilBits > 0) {
        if (depthBits != 24 || stencilBits != 8) {
            appendLog("DEPTH STENCIL FORMAT UNSUPPORTED");
            return false;
        }
        internalFormat = GL_DEPTH24_STENCIL8;
        uploadFormat = GL_DEPTH_STENCIL;
        pixelType = GL_UNSIGNED_INT_24_8;
        attachment = GL_DEPTH_STENCIL_ATTACHMENT;
    } else if (depthBits == 16) {
        internalFormat = GL_DEPTH_COMPONENT16;
        uploadFormat = GL_DEPTH_COMPONENT;
        pixelType = GL_UNSIGNED_SHORT;
    } else if (depthBits == 24) {
        internalFormat = GL_DEPTH_COMPONENT24;
        uploadFormat = GL_DEPTH_COMPONENT;
        pixelType = GL_UNSIGNED_INT;
    } else if (depthBits >= 32) {
        internalFormat = GL_DEPTH_COMPONENT32F;
        uploadFormat = GL_DEPTH_COMPONENT;
        pixelType = GL_FLOAT;
    } else {
        appendLog("DEPTH FORMAT UNSUPPORTED");
        return false;
    }

    const bool recreate = !mDepthTexture || !mDepthScratchTexture || !mDepthFramebuffer ||
        mDepthWidth != width || mDepthHeight != height || mDepthBits != depthBits ||
        mStencilBits != stencilBits || mDepthInternalFormat != internalFormat;
    if (!recreate) return true;

    if (!mDepthTexture) glGenTextures(1, &mDepthTexture);
    if (!mDepthScratchTexture) glGenTextures(1, &mDepthScratchTexture);
    if (!mDepthFramebuffer) glGenFramebuffers(1, &mDepthFramebuffer);
    if (!mDepthTexture || !mDepthScratchTexture || !mDepthFramebuffer) {
        appendLog("DEPTH TARGET ALLOCATION FAILED");
        return false;
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mDepthTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internalFormat), width, height, 0,
                 uploadFormat, pixelType, nullptr);

    glBindTexture(GL_TEXTURE_2D, mDepthScratchTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    glBindFramebuffer(GL_FRAMEBUFFER, mDepthFramebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mDepthScratchTexture, 0);
    if (attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, mDepthTexture, 0);
    } else {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, mDepthTexture, 0);
    }
    const GLenum colorAttachment = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &colorAttachment);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        char msg[48]{};
        std::snprintf(msg, sizeof(msg), "DEPTH FBO INCOMPLETE 0x%04X", status);
        appendLog(msg);
        return false;
    }

    mDepthWidth = width;
    mDepthHeight = height;
    mDepthBits = depthBits;
    mStencilBits = stencilBits;
    mDepthInternalFormat = internalFormat;
    mDepthUploadFormat = uploadFormat;
    mDepthPixelType = pixelType;
    mDepthAttachment = attachment;
    return true;
}

bool FlashlightRenderer::captureDepth(GLuint sourceFramebuffer, EGLDisplay, EGLSurface,
                                      int width, int height, GLint depthBits, GLint stencilBits) {
    ScopedGlState saved;
    if (!ensureDepthTargets(width, height, depthBits, stencilBits)) return false;

    glBindFramebuffer(GL_READ_FRAMEBUFFER, sourceFramebuffer);
    glReadBuffer(sourceFramebuffer == 0 ? GL_BACK : GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mDepthFramebuffer);
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height,
                      GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        char msg[64]{};
        std::snprintf(msg, sizeof(msg), "DEPTH BLIT %s 0x%04X", glErrorName(error), error);
        appendLog(msg);
        androidLog(ANDROID_LOG_WARN, msg);
        return false;
    }

    mDepthReady.store(true, std::memory_order_release);
    mMissingDepthLogged.store(false, std::memory_order_release);
    char msg[48]{};
    const bool reverseDepth = mReverseDepth.load(std::memory_order_acquire);
    std::snprintf(msg, sizeof(msg), reverseDepth ? "DEPTH CAPTURE OK %d BIT REV" : "DEPTH CAPTURE OK %d BIT", depthBits);
    appendLog(msg);
    androidLog(ANDROID_LOG_INFO, msg);
    return true;
}

bool FlashlightRenderer::ensureProgram() {
    if (mProgram) return true;
    auto compile = [](GLenum type, const char* source) -> GLuint {
        GLuint shader = glCreateShader(type);
        if (!shader) return 0;
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint compiled = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (compiled == GL_TRUE) return shader;
        GLint length = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        std::string info(static_cast<std::size_t>(std::max(length, 1)), '\0');
        glGetShaderInfoLog(shader, length, nullptr, info.data());
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "GLSL compile failed: %s", info.c_str());
        androidLog(ANDROID_LOG_ERROR, "Flashlight GLSL compile failed");
        glDeleteShader(shader);
        return 0;
    };

    const GLuint vertex = compile(GL_VERTEX_SHADER, kVertexShader);
    const GLuint fragment = compile(GL_FRAGMENT_SHADER, kFragmentShader);
    if (!vertex || !fragment) {
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        appendLog("FLASHLIGHT GLSL COMPILE FAIL");
        return false;
    }

    const GLuint program = glCreateProgram();
    if (!program) {
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        appendLog("FLASHLIGHT PROGRAM CREATE FAIL");
        return false;
    }
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        GLint length = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
        std::string info(static_cast<std::size_t>(std::max(length, 1)), '\0');
        glGetProgramInfoLog(program, length, nullptr, info.data());
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Flashlight GLSL link failed: %s", info.c_str());
        glDeleteProgram(program);
        appendLog("FLASHLIGHT GLSL LINK FAIL");
        return false;
    }

    mProgram = program;
    mUniformScene = glGetUniformLocation(mProgram, "uScene");
    mUniformDepth = glGetUniformLocation(mProgram, "uDepth");
    mUniformLogs = glGetUniformLocation(mProgram, "uLogs");
    mUniformResolution = glGetUniformLocation(mProgram, "uResolution");
    mUniformFlashlightEnabled = glGetUniformLocation(mProgram, "uFlashlightEnabled");
    mUniformHasDepth = glGetUniformLocation(mProgram, "uHasDepth");
    mUniformReverseDepth = glGetUniformLocation(mProgram, "uReverseDepth");
    mUniformShowLogs = glGetUniformLocation(mProgram, "uShowLogs");
    mUniformBrightness = glGetUniformLocation(mProgram, "uBrightness");
    mUniformDistance = glGetUniformLocation(mProgram, "uDistance");
    appendLog("FLASHLIGHT GLSL LINK OK");
    return true;
}

bool FlashlightRenderer::ensureRenderTargets(int width, int height) {
    if (mTextureWidth == width && mTextureHeight == height && mSceneTexture && mLogTexture) return true;
    if (!mSceneTexture) glGenTextures(1, &mSceneTexture);
    if (!mLogTexture) glGenTextures(1, &mLogTexture);
    if (!mSceneTexture || !mLogTexture) return false;

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mSceneTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    glBindTexture(GL_TEXTURE_2D, mLogTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kLogTextureWidth, kLogTextureHeight, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    mTextureWidth = width;
    mTextureHeight = height;
    mUploadedLogRevision = ~std::uint64_t{0};
    appendLog("FLASHLIGHT TARGETS READY");
    return true;
}

void FlashlightRenderer::updateLogTexture() {
    std::array<std::string, 6> lines{};
    std::uint64_t revision = 0;
    {
        std::lock_guard<std::mutex> lock(mLogMutex);
        revision = mLogRevision;
        const std::size_t visible = std::min<std::size_t>(mLogCount, lines.size());
        const std::size_t first = (mLogStart + mLogCount - visible) % mLogLines.size();
        for (std::size_t i = 0; i < visible; ++i)
            lines[i] = mLogLines[(first + i) % mLogLines.size()];
    }
    if (revision == mUploadedLogRevision) return;

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kLogTextureWidth) * kLogTextureHeight * 4, 0);
    for (int y = 0; y < kLogTextureHeight; ++y) {
        for (int x = 0; x < kLogTextureWidth; ++x) {
            const std::size_t index = (static_cast<std::size_t>(y) * kLogTextureWidth + x) * 4;
            pixels[index + 0] = 7;
            pixels[index + 1] = 12;
            pixels[index + 2] = 20;
            pixels[index + 3] = 172;
        }
    }

    constexpr float scale = 2.5f;
    constexpr int charAdvance = 15;
    constexpr int lineAdvance = 24;
    for (std::size_t line = 0; line < lines.size(); ++line) {
        const int originY = 9 + static_cast<int>(line) * lineAdvance;
        for (std::size_t c = 0; c < lines[line].size() && c < 34; ++c) {
            const auto columns = glyphColumns(lines[line][c]);
            const int originX = 10 + static_cast<int>(c) * charAdvance;
            for (int column = 0; column < 5; ++column) {
                for (int row = 0; row < 7; ++row) {
                    if ((columns[column] & (1u << row)) == 0) continue;
                    const int xBegin = static_cast<int>(column * scale);
                    const int xEnd = static_cast<int>((column + 1) * scale);
                    const int yBegin = static_cast<int>(row * scale);
                    const int yEnd = static_cast<int>((row + 1) * scale);
                    for (int sy = yBegin; sy < yEnd; ++sy) {
                        for (int sx = xBegin; sx < xEnd; ++sx) {
                            const int x = originX + sx;
                            const int y = originY + sy;
                            if (x < 0 || x >= kLogTextureWidth || y < 0 || y >= kLogTextureHeight) continue;
                            const std::size_t index = (static_cast<std::size_t>(y) * kLogTextureWidth + x) * 4;
                            pixels[index + 0] = 238;
                            pixels[index + 1] = 244;
                            pixels[index + 2] = 252;
                            pixels[index + 3] = 250;
                        }
                    }
                }
            }
        }
    }

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, mLogTexture);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kLogTextureWidth, kLogTextureHeight,
                    GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    mUploadedLogRevision = revision;
}

void FlashlightRenderer::resetGlObjectsForNewContext(EGLContext context) {
    if (mGlContext == context) return;
    mGlContext = context;
    // Names from another EGL context are not valid in this context. Do not
    // delete them here because the old context is no longer current.
    mProgram = mVertexArray = 0;
    mUniformScene = mUniformDepth = mUniformLogs = mUniformResolution = -1;
    mUniformFlashlightEnabled = mUniformHasDepth = mUniformReverseDepth = mUniformShowLogs = -1;
    mUniformBrightness = mUniformDistance = -1;
    mSceneTexture = mLogTexture = 0;
    mDepthTexture = mDepthScratchTexture = mDepthFramebuffer = 0;
    mTextureWidth = mTextureHeight = 0;
    mDepthWidth = mDepthHeight = 0;
    mDepthBits = mStencilBits = 0;
    mDepthInternalFormat = 0;
    mDepthUploadFormat = GL_DEPTH_COMPONENT;
    mDepthPixelType = GL_UNSIGNED_SHORT;
    mDepthAttachment = GL_DEPTH_ATTACHMENT;
    mDepthReady.store(false, std::memory_order_release);
    mReverseDepth.store(false, std::memory_order_release);
    mSceneStarted = false;
    mCaptureProcessed = false;
    mWorldFramebuffer = -1;
    mUploadedLogRevision = ~std::uint64_t{0};
    mRenderPassLogged = false;
}

bool FlashlightRenderer::renderCurrentFrame(EGLDisplay display, EGLSurface surface) {
    if (mRendering.test_and_set(std::memory_order_acquire)) return false;
    struct RenderingGuard {
        std::atomic_flag& flag;
        ~RenderingGuard() { flag.clear(std::memory_order_release); }
    } renderingGuard{mRendering};

    ModConfig config;
    {
        std::lock_guard<std::mutex> lock(mConfigMutex);
        config = mConfig;
    }
    if (!config.enabled || (!config.USE_FLASHLIGHT && !config.DEV_LOGS) ||
        eglGetCurrentContext() == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE ||
        eglGetCurrentSurface(EGL_DRAW) != surface) return false;

    EGLint width = 0, height = 0;
    if (eglQuerySurface(display, surface, EGL_WIDTH, &width) != EGL_TRUE ||
        eglQuerySurface(display, surface, EGL_HEIGHT, &height) != EGL_TRUE ||
        width <= 0 || height <= 0 || width > 8192 || height > 8192) return false;

    ScopedGlState saved;
    resetGlObjectsForNewContext(eglGetCurrentContext());
    if (!ensureProgram()) return false;
    if (!mVertexArray) glGenVertexArrays(1, &mVertexArray);
    if (!mVertexArray || !ensureRenderTargets(width, height)) {
        appendLog("FLASHLIGHT RENDER TARGET FAIL");
        return false;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mSceneTexture);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);

    if (config.DEV_LOGS) updateLogTexture();

    const bool hasDepth = config.USE_FLASHLIGHT && mDepthReady.load(std::memory_order_acquire) &&
                          mDepthTexture && mDepthWidth == width && mDepthHeight == height;
    if (config.USE_FLASHLIGHT && !hasDepth && !mMissingDepthLogged.load(std::memory_order_acquire)) {
        appendLog("FLASHLIGHT WAITING FOR DEPTH");
        mMissingDepthLogged.store(true, std::memory_order_release);
    }

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(mProgram);
    glBindVertexArray(mVertexArray);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mSceneTexture);
    glBindSampler(0, 0);
    glUniform1i(mUniformScene, 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, hasDepth ? mDepthTexture : 0);
    glBindSampler(1, 0);
    glUniform1i(mUniformDepth, 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, mLogTexture);
    glBindSampler(2, 0);
    glUniform1i(mUniformLogs, 2);

    glUniform2f(mUniformResolution, static_cast<float>(width), static_cast<float>(height));
    glUniform1i(mUniformFlashlightEnabled, hasDepth ? 1 : 0);
    glUniform1i(mUniformHasDepth, hasDepth ? 1 : 0);
    glUniform1i(mUniformReverseDepth, mReverseDepth.load(std::memory_order_acquire) ? 1 : 0);
    glUniform1i(mUniformShowLogs, config.DEV_LOGS ? 1 : 0);
    glUniform1f(mUniformBrightness, config.FLASHLIGHT_BRIGHTNESS);
    glUniform1f(mUniformDistance, config.FLASHLIGHT_DISTANCE);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        char msg[64]{};
        std::snprintf(msg, sizeof(msg), "FLASHLIGHT GL %s 0x%04X", glErrorName(error), error);
        appendLog(msg);
        androidLog(ANDROID_LOG_WARN, msg);
    } else if (!mRenderPassLogged) {
        appendLog("FLASHLIGHT FINAL PASS OK");
        mRenderPassLogged = true;
    }

    if (hasDepth) mDepthReady.store(false, std::memory_order_release);
    return true;
}

void FlashlightRenderer::finishFrame() {
    ModConfig config;
    {
        std::lock_guard<std::mutex> lock(mConfigMutex);
        config = mConfig;
    }
    if (config.USE_FLASHLIGHT && mSceneStarted && !mCaptureProcessed &&
        !mMissingDepthLogged.exchange(true, std::memory_order_acq_rel)) {
        appendLog("DEPTH CLEAR PATTERN NOT FOUND");
    }
    mSceneStarted = false;
    mCaptureProcessed = false;
    mWorldFramebuffer = -1;
}

EGLBoolean FlashlightRenderer::swapBuffersHook(EGLDisplay display, EGLSurface surface) {
    auto& self = instance();
    self.renderCurrentFrame(display, surface);
    self.finishFrame();
    auto original = reinterpret_cast<EglSwapBuffersFn>(self.mOriginalSwap.load(std::memory_order_acquire));
    return original ? original(display, surface) : EGL_FALSE;
}

} // namespace mcvhs
