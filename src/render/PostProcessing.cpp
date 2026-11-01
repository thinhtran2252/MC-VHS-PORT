#include "render/PostProcessing.h"

#include <android/log.h>
#include <dlfcn.h>
#include <pl/memory/Hook.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>

namespace mcvhs {
namespace {

constexpr char kLogTag[] = "MCVHS-PORT";
using EglSwapBuffersFn = EGLBoolean (*)(EGLDisplay, EGLSurface);
const auto kStartTime = std::chrono::steady_clock::now();

void logMessage(int priority, const char* message) {
    __android_log_print(priority, kLogTag, "%s", message);
}

GLuint compileShader(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    if (!shader) {
        logMessage(ANDROID_LOG_ERROR, "glCreateShader failed");
        return 0;
    }
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
    glDeleteShader(shader);
    return 0;
}

constexpr char kVertexShader[] = R"GLSL(#version 300 es
precision highp float;
out vec2 vTexCoord;
void main() {
    // Oversized fullscreen triangle. vTexCoord must be p, not p * 0.5.
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vTexCoord = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

constexpr char kFragmentShader[] = R"GLSL(#version 300 es
precision highp float;
precision highp int;
in vec2 vTexCoord;
out vec4 fragColor;

uniform sampler2D uScene;
uniform vec2 uResolution;
uniform float uTime;
uniform float uMaxMip;
uniform int uStage; // 0 composite2, 1 composite4, 2 composite5, 3 composite6

uniform int uUseStatic;
uniform float uStaticStrength;
uniform int uUseLensDistortion;
uniform float uLensDistortionStrength;
uniform int uAspectRatio;
uniform int uAutoExposure;
uniform float uManualExposure;
uniform int uTonemapMethod;
uniform int uCompressionMethod;
uniform float uLumaCompression;
uniform float uChromaCompression;
uniform float uChromaBoost;
uniform float uExtraZoom;
uniform int uUseSharpness;
uniform float uSharpnessStrength;
uniform float uSharpnessRadius;
uniform int uSideJitter;
uniform float uSideJitterAmount;

float randomValue(inout uint state) {
    state = state * 747796405u + 2891336453u;
    uint result = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    result = (result >> 22u) ^ result;
    state *= result;
    return float(result) / 4294967295.0;
}

float randomNormal(inout uint state) {
    float theta = 6.28318530718 * randomValue(state);
    float rho = sqrt(-2.0 * log(max(randomValue(state), 1.0e-7)));
    state += uint(rho * cos(theta));
    return rho * cos(theta);
}

vec3 randomDirection(inout uint state) {
    return normalize(vec3(randomNormal(state), randomNormal(state), randomNormal(state)));
}

vec3 rgbToYuv(vec3 rgb) {
    float y = 0.299 * rgb.r + 0.587 * rgb.g + 0.114 * rgb.b;
    return vec3(y, 0.493 * (rgb.b - y), 0.877 * (rgb.r - y));
}

vec3 yuvToRgb(vec3 yuv) {
    return vec3(
        yuv.x + (1.0 / 0.877) * yuv.z,
        yuv.x - 0.39393 * yuv.y - 0.58081 * yuv.z,
        yuv.x + (1.0 / 0.493) * yuv.y
    );
}

vec3 rgbToYcbcr(vec3 c) {
    float y = 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
    return vec3(y, (c.b - y) * 0.565, (c.r - y) * 0.713);
}

vec3 ycbcrToRgb(vec3 c) {
    return vec3(c.x + 1.403 * c.z,
                c.x - 0.344 * c.y - 0.714 * c.z,
                c.x + 1.770 * c.y);
}

vec3 rgbToYiq(vec3 rgb) {
    // Same GLSL matrix/vector multiplication as composite5.fsh.
    const mat3 transform = mat3(
        0.299, 0.596, 0.211,
        0.587, -0.274, -0.523,
        0.114, -0.322, 0.312
    );
    return rgb * transform;
}

vec3 yiqToRgb(vec3 yiq) {
    const mat3 transform = mat3(
        1.0, 1.0, 1.0,
        0.956, -0.272, -1.106,
        0.621, -0.647, 1.703
    );
    return yiq * transform;
}

vec4 composite2(vec2 texCoord) {
    vec2 uv = texCoord;
    if (uAspectRatio != 0)
        uv.x = (uv.x - 0.5) * 1.2 + 0.5;

    if (uUseLensDistortion != 0) {
        uv = uv * 2.0 - 1.0;
        uv *= 1.0 + uLensDistortionStrength * dot(uv, uv);
        uv = uv * 0.5 + 0.5;
    }

    vec4 color = texture(uScene, uv);
    if (uUseStatic != 0) {
        uvec2 pixel = uvec2(texCoord * uResolution);
        pixel = (pixel / 4u) * 4u;
        uint pixelIndex = pixel.y * uint(uResolution.x) + pixel.x;
        uint seed = pixelIndex + uint(fract(uTime) * uResolution.y * uResolution.y);
        vec3 noise = randomDirection(seed);
        vec3 yuv = rgbToYuv(color.rgb);
        yuv.yz += noise.xy * uStaticStrength;
        color.rgb = yuvToRgb(yuv);
    }

    // Source composite2 applies this curve unconditionally.
    color.rgb = (color.rgb + color.rgb) / max(vec3(0.0001), vec3(2.0) - color.rgb);
    if (uAutoExposure != 0) {
        float averageLuma = rgbToYuv(textureLod(uScene, vec2(0.5), min(8.0, uMaxMip)).rgb).x + 0.5;
        color.rgb /= max(averageLuma, 0.0001);
    }
    color.rgb *= uManualExposure;
    color.rgb = clamp(color.rgb, vec3(0.0), vec3(10.0));

    // Java composite2 explicitly blacks pixels outside the horizontal crop.
    if (uv.x < 0.0 || uv.x > 1.0) color.rgb = vec3(0.0);
    return color;
}

vec4 composite4(vec4 color) {
    if (uTonemapMethod == 1) {
        vec3 x = clamp(color.rgb, vec3(-10.0), vec3(10.0));
        color.rgb = 2.0 / (1.0 + exp(-2.0 * x)) - 1.0;
    } else if (uTonemapMethod == 2) {
        color.rgb = 1.0 - exp(-color.rgb);
    } else if (uTonemapMethod == 3) {
        color.rgb = color.rgb / (1.0 + color.rgb);
    }
    return color;
}

vec4 composite5(vec2 uv) {
    vec4 base = texture(uScene, uv);
    float lumaLod = min(uLumaCompression, uMaxMip);
    float chromaLod = min(uChromaCompression, uMaxMip);
    vec3 lumaSource = textureLod(uScene, uv, lumaLod).rgb;
    vec3 chromaSource = textureLod(uScene, uv, chromaLod).rgb;
    vec3 result;

    if (uCompressionMethod == 0) {
        float y = rgbToYuv(lumaSource).x;
        vec2 uvChroma = rgbToYuv(chromaSource).yz * (1.0 + uChromaBoost);
        result = yuvToRgb(vec3(y, uvChroma));
    } else if (uCompressionMethod == 1) {
        float y = rgbToYiq(lumaSource).x;
        vec2 iq = rgbToYiq(chromaSource).yz * (1.0 + uChromaBoost);
        result = yiqToRgb(vec3(y, iq));
    } else {
        float y = rgbToYcbcr(lumaSource).x;
        vec2 cbcr = rgbToYcbcr(chromaSource).yz * (1.0 + uChromaBoost);
        result = ycbcrToRgb(vec3(y, cbcr));
    }
    base.rgb = result;
    return base;
}

vec4 composite6(vec2 texCoord) {
    vec2 coord = texCoord * 2.0 - 1.0;
    coord /= 1.0 + uExtraZoom;
    coord = coord * 0.5 + 0.5;

    if (uSideJitter != 0) {
        uint seed = uint(fract(uTime) * 10000.0);
        float jitter = (pow(randomValue(seed), 2.0) * 2.0 - 1.0) * uSideJitterAmount;
        coord.x += jitter;
    }

    vec4 color = texture(uScene, coord);
    if (uUseSharpness != 0) {
        vec3 blurred = textureLod(uScene, coord, min(uSharpnessRadius, uMaxMip)).rgb;
        color.rgb = mix(color.rgb, blurred, -uSharpnessStrength);
    }
    return color;
}

void main() {
    if (uStage == 0) fragColor = composite2(vTexCoord);
    else if (uStage == 1) fragColor = composite4(texture(uScene, vTexCoord));
    else if (uStage == 2) fragColor = composite5(vTexCoord);
    else fragColor = composite6(vTexCoord);
}
)GLSL";

struct SavedGlState {
    GLint drawFramebuffer{};
    GLint readFramebuffer{};
    GLint viewport[4]{};
    GLint program{};
    GLint vertexArray{};
    GLint activeTexture{};
    GLint texture0{};
    GLint sampler0{};
    GLboolean colorMask[4]{GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLboolean blend{};
    GLboolean depth{};
    GLboolean stencil{};
    GLboolean scissor{};
    GLboolean cull{};

    void capture() {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertexArray);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture0);
        glGetIntegerv(GL_SAMPLER_BINDING, &sampler0);
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
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glUseProgram(static_cast<GLuint>(program));
        glBindVertexArray(static_cast<GLuint>(vertexArray));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture0));
        glBindSampler(0, static_cast<GLuint>(sampler0));
        glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
        blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
        depth ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
        stencil ? glEnable(GL_STENCIL_TEST) : glDisable(GL_STENCIL_TEST);
        scissor ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
        cull ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
        glActiveTexture(static_cast<GLenum>(activeTexture));
    }
};

void uniform1i(GLuint program, const char* name, GLint value) {
    const GLint location = glGetUniformLocation(program, name);
    if (location >= 0) glUniform1i(location, value);
}
void uniform1f(GLuint program, const char* name, GLfloat value) {
    const GLint location = glGetUniformLocation(program, name);
    if (location >= 0) glUniform1f(location, value);
}

} // namespace

PostProcessing& PostProcessing::instance() {
    static PostProcessing instance;
    return instance;
}

void PostProcessing::setConfig(const ModConfig& config) {
    std::lock_guard<std::mutex> lock(mConfigMutex);
    mConfig = config;
}

bool PostProcessing::start() {
    std::lock_guard<std::mutex> lock(mHookMutex);
    if (mHookInstalled) return true;

    if (!mEglLibrary) mEglLibrary = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
    if (!mEglLibrary) {
        logMessage(ANDROID_LOG_ERROR, "Could not open libEGL.so; post-processing hook not installed");
        return false;
    }
    mSwapTarget = dlsym(mEglLibrary, "eglSwapBuffers");
    if (!mSwapTarget) {
        logMessage(ANDROID_LOG_ERROR, "eglSwapBuffers symbol not found");
        return false;
    }

    void* original = nullptr;
    const int result = pl::memory::hook(
        reinterpret_cast<pl::memory::FuncPtr>(mSwapTarget),
        reinterpret_cast<pl::memory::FuncPtr>(&PostProcessing::swapBuffersHook),
        &original,
        pl::memory::HookPriority::Low);
    if (result != 0 || !original) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Failed to install eglSwapBuffers hook: %d", result);
        return false;
    }
    mOriginalSwap.store(original, std::memory_order_release);
    mHookInstalled = true;
    logMessage(ANDROID_LOG_INFO, "eglSwapBuffers hook installed");
    return true;
}

void PostProcessing::stop() {
    std::lock_guard<std::mutex> lock(mHookMutex);
    if (!mHookInstalled) return;
    pl::memory::unhook(
        reinterpret_cast<pl::memory::FuncPtr>(mSwapTarget),
        reinterpret_cast<pl::memory::FuncPtr>(&PostProcessing::swapBuffersHook));
    mHookInstalled = false;
    // Keep EGL and the original trampoline alive for any swap callback already in flight.
    logMessage(ANDROID_LOG_INFO, "eglSwapBuffers hook removed");
}

EGLBoolean PostProcessing::swapBuffersHook(EGLDisplay display, EGLSurface surface) {
    auto& self = PostProcessing::instance();
    self.renderCurrentFrame(display, surface);
    auto original = reinterpret_cast<EglSwapBuffersFn>(self.mOriginalSwap.load(std::memory_order_acquire));
    return original ? original(display, surface) : EGL_FALSE;
}

bool PostProcessing::ensureProgram() {
    if (mProgram) return true;
    const GLuint vertex = compileShader(GL_VERTEX_SHADER, kVertexShader);
    const GLuint fragment = compileShader(GL_FRAGMENT_SHADER, kFragmentShader);
    if (!vertex || !fragment) {
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        return false;
    }
    const GLuint program = glCreateProgram();
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
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "GLSL link failed: %s", info.c_str());
        glDeleteProgram(program);
        return false;
    }
    mProgram = program;
    return true;
}

bool PostProcessing::allocateTargets(int width, int height, GLint internalFormat, GLenum type) {
    glActiveTexture(GL_TEXTURE0);

    // The swapchain image is copied into an RGBA8 texture, matching the
    // screen-copy path used by LeviPostFX. Only subsequent processing targets
    // use RGBA16F when the device supports renderable half-float textures.
    glBindTexture(GL_TEXTURE_2D, mCaptureTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    for (GLuint texture : {mLensTexture, mToneTexture, mCompressionTexture}) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0,
                     GL_RGBA, type, nullptr);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, mCaptureFramebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mCaptureTexture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return false;

    glBindFramebuffer(GL_FRAMEBUFFER, mWorkFramebuffer);
    for (GLuint texture : {mLensTexture, mToneTexture, mCompressionTexture}) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return false;
    }
    mTextureFormat = internalFormat;
    mTextureType = type;
    return true;
}

bool PostProcessing::ensureTargets(int width, int height) {
    if (width == mTextureWidth && height == mTextureHeight && mCaptureTexture && mLensTexture && mToneTexture && mCompressionTexture)
        return true;

    if (!mCaptureTexture) glGenTextures(1, &mCaptureTexture);
    if (!mLensTexture) glGenTextures(1, &mLensTexture);
    if (!mToneTexture) glGenTextures(1, &mToneTexture);
    if (!mCompressionTexture) glGenTextures(1, &mCompressionTexture);
    if (!mCaptureFramebuffer) glGenFramebuffers(1, &mCaptureFramebuffer);
    if (!mWorkFramebuffer) glGenFramebuffers(1, &mWorkFramebuffer);
    if (!mCaptureTexture || !mLensTexture || !mToneTexture || !mCompressionTexture || !mCaptureFramebuffer || !mWorkFramebuffer)
        return false;

    // Prefer floating-point intermediates so the Java exposure/tonemapping stages
    // do not clamp HDR values between passes. Fall back for devices without a
    // renderable RGBA16F framebuffer.
    bool allocated = allocateTargets(width, height, GL_RGBA16F, GL_HALF_FLOAT);
    if (!allocated) allocated = allocateTargets(width, height, GL_RGBA8, GL_UNSIGNED_BYTE);
    if (!allocated) {
        logMessage(ANDROID_LOG_ERROR, "Could not create complete post-processing framebuffers");
        mTextureWidth = 0;
        mTextureHeight = 0;
        return false;
    }

    const GLint maxLevel = static_cast<GLint>(std::floor(std::log2(static_cast<float>(std::max(width, height)))));
    for (GLuint texture : {mCaptureTexture, mLensTexture, mToneTexture, mCompressionTexture}) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, maxLevel);
    }
    mTextureWidth = width;
    mTextureHeight = height;
    logMessage(ANDROID_LOG_INFO, mTextureFormat == GL_RGBA16F
        ? "Allocated RGBA16F intermediate targets" : "Allocated RGBA8 fallback targets; HDR precision reduced");
    return true;
}

bool PostProcessing::drawPass(GLuint inputTexture, GLuint outputFramebuffer, GLuint outputTexture,
                              int stage, int width, int height, const ModConfig& config,
                              float timeSeconds, float maxMip) {
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, outputFramebuffer);
    if (outputTexture != 0) {
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, 0);
        if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return false;
    }
    glViewport(0, 0, width, height);
    glUseProgram(mProgram);
    glBindVertexArray(mVertexArray);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, inputTexture);
    glBindSampler(0, 0);

    uniform1i(mProgram, "uScene", 0);
    uniform1i(mProgram, "uStage", stage);
    GLint resolution = glGetUniformLocation(mProgram, "uResolution");
    if (resolution >= 0) glUniform2f(resolution, static_cast<float>(width), static_cast<float>(height));
    uniform1f(mProgram, "uTime", timeSeconds);
    uniform1f(mProgram, "uMaxMip", maxMip);

    uniform1i(mProgram, "uUseStatic", config.USE_STATIC ? 1 : 0);
    uniform1f(mProgram, "uStaticStrength", config.STATIC_STRENGTH);
    uniform1i(mProgram, "uUseLensDistortion", config.USE_LENS_DISTORTION ? 1 : 0);
    uniform1f(mProgram, "uLensDistortionStrength", config.LENS_DISTORTION_STRENGTH);
    uniform1i(mProgram, "uAspectRatio", config.ASPECT_RATIO ? 1 : 0);
    uniform1i(mProgram, "uAutoExposure", config.AUTO_EXPOSURE ? 1 : 0);
    uniform1f(mProgram, "uManualExposure", config.MANUAL_EXPOSURE);
    uniform1i(mProgram, "uTonemapMethod", config.TONEMAPPING_METHOD);
    uniform1i(mProgram, "uCompressionMethod", config.COMPRESSION_METHOD);
    uniform1f(mProgram, "uLumaCompression", static_cast<float>(config.LUMA_COMPRESSION));
    uniform1f(mProgram, "uChromaCompression", static_cast<float>(config.CHROMA_COMPRESSION));
    uniform1f(mProgram, "uChromaBoost", config.CHROMA_BOOST);
    uniform1f(mProgram, "uExtraZoom", config.EXTRA_ZOOM);
    uniform1i(mProgram, "uUseSharpness", config.USE_SHARPNESS ? 1 : 0);
    uniform1f(mProgram, "uSharpnessStrength", static_cast<float>(config.SHARPNESS_STRENGTH));
    uniform1f(mProgram, "uSharpnessRadius", config.SHARPNESS_RADIUS);
    uniform1i(mProgram, "uSideJitter", config.SIDE_JITTER ? 1 : 0);
    uniform1f(mProgram, "uSideJitterAmount", config.SIDE_JITTER_AMMOUNT);

    glDrawArrays(GL_TRIANGLES, 0, 3);
    // Do not gate the pass on glGetError(): the game or another mod may have
    // left a pending global GL error before this hook was entered.
    return true;
}

bool PostProcessing::renderCurrentFrame(EGLDisplay display, EGLSurface surface) {
    if (mRendering.test_and_set(std::memory_order_acquire)) return false;
    struct ClearFlag { std::atomic_flag& flag; ~ClearFlag() { flag.clear(std::memory_order_release); } } clear{mRendering};

    ModConfig config;
    {
        std::lock_guard<std::mutex> lock(mConfigMutex);
        config = mConfig;
    }
    if (!config.enabled || eglGetCurrentContext() == EGL_NO_CONTEXT) return false;

    // Follow the presentation surface passed to eglSwapBuffers. World rendering
    // may leave an internal FBO and a reduced viewport bound at this point.
    if (surface == EGL_NO_SURFACE || eglGetCurrentSurface(EGL_DRAW) != surface) {
        return false;
    }

    EGLint surfaceWidth = 0;
    EGLint surfaceHeight = 0;
    if (eglQuerySurface(display, surface, EGL_WIDTH, &surfaceWidth) != EGL_TRUE ||
        eglQuerySurface(display, surface, EGL_HEIGHT, &surfaceHeight) != EGL_TRUE ||
        surfaceWidth <= 0 || surfaceHeight <= 0 ||
        surfaceWidth > 8192 || surfaceHeight > 8192) {
        logMessage(ANDROID_LOG_WARN, "Invalid EGL surface dimensions");
        return false;
    }

    SavedGlState state;
    state.capture();
    auto restore = [&state]() { state.restore(); };

    const EGLContext context = eglGetCurrentContext();
    if (mGlContext != context) {
        // GL object names belong to the previous context. They must not be deleted
        // while another context is current, so initialize fresh names instead.
        mGlContext = context;
        mProgram = mVertexArray = 0;
        mCaptureTexture = mLensTexture = mToneTexture = mCompressionTexture = 0;
        mCaptureFramebuffer = mWorkFramebuffer = 0;
        mTextureWidth = mTextureHeight = 0;
    }

    if (!ensureProgram()) {
        restore();
        return false;
    }
    if (!mVertexArray) glGenVertexArrays(1, &mVertexArray);
    if (!mVertexArray || !ensureTargets(surfaceWidth, surfaceHeight)) {
        restore();
        return false;
    }

    const int width = surfaceWidth;
    const int height = surfaceHeight;
    const float maxMip = std::floor(std::log2(static_cast<float>(std::max(width, height))));
    const float timeSeconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - kStartTime).count();

    // Match LeviPostFX's final-frame capture path: read from framebuffer 0,
    // copy the full EGL surface into a screen texture, then build its mipmaps.
    // This deliberately does not depend on the world's current FBO or viewport.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mCaptureTexture);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
    glGenerateMipmap(GL_TEXTURE_2D);

    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    // Separate targets preserve the original shader's pass/mipmap ordering:
    // composite2 -> composite4 (bloom deferred) -> composite5 -> composite6.
    if (!drawPass(mCaptureTexture, mWorkFramebuffer, mLensTexture, 0,
                  width, height, config, timeSeconds, maxMip)) {
        restore(); return false;
    }
    glBindTexture(GL_TEXTURE_2D, mLensTexture);
    glGenerateMipmap(GL_TEXTURE_2D);

    if (!drawPass(mLensTexture, mWorkFramebuffer, mToneTexture, 1,
                  width, height, config, timeSeconds, maxMip)) {
        restore(); return false;
    }
    glBindTexture(GL_TEXTURE_2D, mToneTexture);
    glGenerateMipmap(GL_TEXTURE_2D);

    if (!drawPass(mToneTexture, mWorkFramebuffer, mCompressionTexture, 2,
                  width, height, config, timeSeconds, maxMip)) {
        restore(); return false;
    }
    glBindTexture(GL_TEXTURE_2D, mCompressionTexture);
    glGenerateMipmap(GL_TEXTURE_2D);

    if (!drawPass(mCompressionTexture, 0, 0, 3,
                  width, height, config, timeSeconds, maxMip)) {
        restore(); return false;
    }

    restore();
    return true;
}

} // namespace mcvhs
