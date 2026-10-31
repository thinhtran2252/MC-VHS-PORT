#include "render/PostProcessing.h"

#include <GLES3/gl3.h>
#include <android/log.h>
#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>

#include <pl/memory/Hook.hpp>

namespace mcvhs {
namespace {

using EglSwapBuffersFn = EGLBoolean (*)(EGLDisplay, EGLSurface);
constexpr const char *kLogTag = "MCVHS-PORT";
const auto kProcessStart = std::chrono::steady_clock::now();

const char *kVertexShader = R"GLSL(
#version 300 es
precision highp float;
out vec2 vUv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUv = p * 0.5;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

const char *kFragmentShader = R"GLSL(
#version 300 es
precision highp float;
precision highp int;
in vec2 vUv;
out vec4 fragColor;

uniform sampler2D uScene;
uniform vec2 uResolution;
uniform float uTime;

uniform int uLensGroup;
uniform int uCompressionGroup;
uniform int uPostGroup;
uniform int uSignalGroup;
uniform int uUserSuggestionsGroup;

uniform int uAspectRatio;
uniform int uLensDistortion;
uniform float uLensDistortionStrength;
uniform int uInterlacing;
uniform float uInterlacingStrength;
uniform int uStaticNoise;
uniform float uStaticStrength;
uniform int uMoreStatic;
uniform int uSideJitter;
uniform float uSideJitterAmount;
uniform int uAutoExposure;
uniform float uManualExposure;
uniform int uTonemapMethod;
uniform float uExtraZoom;
uniform int uSharpness;
uniform float uSharpnessStrength;
uniform float uSharpnessRadius;
uniform int uCompressionMethod;
uniform float uLumaCompression;
uniform float uChromaCompression;
uniform float uChromaBoost;

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

vec3 rgb2yuv(vec3 rgb) {
    float y = dot(rgb, vec3(0.299, 0.587, 0.114));
    return vec3(y, 0.493 * (rgb.b - y), 0.877 * (rgb.r - y));
}

vec3 yuv2rgb(vec3 yuv) {
    return vec3(
        yuv.x + (1.0 / 0.877) * yuv.z,
        yuv.x - 0.39393 * yuv.y - 0.58081 * yuv.z,
        yuv.x + (1.0 / 0.493) * yuv.y
    );
}

vec3 rgb2yiq(vec3 c) {
    return vec3(
        dot(c, vec3(0.299, 0.587, 0.114)),
        dot(c, vec3(0.596, -0.274, -0.322)),
        dot(c, vec3(0.211, -0.523, 0.312))
    );
}

vec3 yiq2rgb(vec3 c) {
    return vec3(
        c.x + 0.956 * c.y + 0.621 * c.z,
        c.x - 0.272 * c.y - 0.647 * c.z,
        c.x - 1.106 * c.y + 1.703 * c.z
    );
}

vec3 rgb2ycbcr(vec3 c) {
    float y = dot(c, vec3(0.299, 0.587, 0.114));
    return vec3(y, (c.b - y) * 0.565, (c.r - y) * 0.713);
}

vec3 ycbcr2rgb(vec3 c) {
    return vec3(
        c.x + 1.403 * c.z,
        c.x - 0.344 * c.y - 0.714 * c.z,
        c.x + 1.770 * c.y
    );
}

vec3 applyTonemap(vec3 color) {
    if (uPostGroup == 0)
        return color;
    if (uTonemapMethod == 1) {
        vec3 x = clamp(color, vec3(-10.0), vec3(10.0));
        return 2.0 / (1.0 + exp(-2.0 * x)) - 1.0;
    }
    if (uTonemapMethod == 2)
        return 1.0 - exp(-max(color, vec3(0.0)));
    if (uTonemapMethod == 3)
        return color / (1.0 + max(color, vec3(0.0)));
    return color;
}

vec3 processComposite2(vec2 uv, float lod) {
    vec3 color = textureLod(uScene, clamp(uv, vec2(0.0), vec2(1.0)), lod).rgb;

    if (uSignalGroup != 0 && uStaticNoise != 0) {
        vec2 block = floor(uv * uResolution / 4.0);
        float frameSeed = floor(uTime * 24.0);
        vec2 noise = vec2(hash12(block + frameSeed), hash12(block.yx + frameSeed + 13.0)) - 0.5;
        noise *= uStaticStrength / exp2(max(lod, 0.0) * 0.5);
        vec3 yuv = rgb2yuv(color);
        yuv.yz += noise;
        color = yuv2rgb(yuv);
    }

    // This contrast curve is always active in the original composite2 pass.
    color = (color + color) / max(vec3(0.001), vec3(2.0) - color);
    if (uLensGroup != 0 && uAutoExposure != 0) {
        float averageLuma = rgb2yuv(textureLod(uScene, vec2(0.5), 8.0).rgb).x + 0.5;
        color /= max(averageLuma, 0.001);
    }
    if (uLensGroup != 0)
        color *= uManualExposure;

    if (uPostGroup != 0 && uMoreStatic != 0) {
        vec2 pixel = floor(uv * uResolution);
        float frameSeed = floor(uTime * 36.0);
        float whiteBitA = step(0.992, hash12(pixel + frameSeed));
        float whiteBitB = step(0.60, hash12(pixel.yx - frameSeed + 41.0));
        float whiteBits = whiteBitA * whiteBitB / exp2(max(lod, 0.0));
        color = mix(color, vec3(1.0), whiteBits);
    }
    return clamp(color, vec3(0.0), vec3(10.0));
}

vec3 processComposite5(vec2 uv, float outputLod) {
    if (uCompressionGroup == 0)
        return applyTonemap(processComposite2(uv, outputLod));

    float lumaLod = clamp(outputLod + uLumaCompression, 0.0, 12.0);
    float chromaLod = clamp(outputLod + uChromaCompression, 0.0, 12.0);
    vec3 lumaSample = applyTonemap(processComposite2(uv, lumaLod));
    vec3 chromaSample = applyTonemap(processComposite2(uv, chromaLod));

    if (uCompressionMethod == 0) {
        float y = rgb2yuv(lumaSample).x;
        vec2 chroma = rgb2yuv(chromaSample).yz * (1.0 + uChromaBoost);
        return yuv2rgb(vec3(y, chroma));
    }
    if (uCompressionMethod == 1) {
        float y = rgb2yiq(lumaSample).x;
        vec2 chroma = rgb2yiq(chromaSample).yz * (1.0 + uChromaBoost);
        return yiq2rgb(vec3(y, chroma));
    }

    float y = rgb2ycbcr(lumaSample).x;
    vec2 chroma = rgb2ycbcr(chromaSample).yz * (1.0 + uChromaBoost);
    return ycbcr2rgb(vec3(y, chroma));
}

void main() {
    vec2 uv = vUv;

    // composite6: zoom and horizontal jitter. Applied before the single
    // framebuffer sample because this Bedrock hook currently uses one pass.
    if (uPostGroup != 0 && uExtraZoom > 0.0) {
        uv = (uv * 2.0 - 1.0) / (1.0 + uExtraZoom);
        uv = uv * 0.5 + 0.5;
    }
    if (uUserSuggestionsGroup != 0 && uSideJitter != 0) {
        float frameSeed = floor(uTime * 30.0);
        float jitter = (hash12(vec2(frameSeed, 17.0)) * 2.0 - 1.0) * uSideJitterAmount;
        uv.x += jitter;
    }

    // composite2: display shaping and lens warp.
    if (uSignalGroup != 0 && uAspectRatio != 0)
        uv.x = (uv.x - 0.5) * 1.2 + 0.5;
    if (uLensGroup != 0 && uLensDistortion != 0) {
        vec2 centered = uv * 2.0 - 1.0;
        centered *= 1.0 + uLensDistortionStrength * dot(centered, centered);
        uv = centered * 0.5 + 0.5;
    }
    if (uSignalGroup != 0 && uInterlacing != 0) {
        // Java scales this offset by camera displacement. The final EGL hook
        // has no camera transform, so use a time-varying scanline phase.
        float band = fract(uv.y * (uResolution.y / 10.0)) > 0.5 ? 1.0 : 0.0;
        uv.x += band * (2.0 / uResolution.x) * (uInterlacingStrength / 20.0) * sin(uTime * 12.0);
    }

    vec3 processed = processComposite2(uv, 0.0);
    processed = applyTonemap(processed);

    // composite5: chroma channels are sampled from the already processed
    // composite2 + tonemapping result, including its generated screen mips.
    if (uCompressionGroup != 0)
        processed = processComposite5(uv, 0.0);

    // composite6: approximate the blurred mip of the prior processed passes.
    if (uPostGroup != 0 && uSharpness != 0) {
        vec3 blurred = processComposite5(uv, uSharpnessRadius);
        processed = mix(processed, blurred, -uSharpnessStrength);
    }

    vec4 originalPixel = texture(uScene, clamp(uv, vec2(0.0), vec2(1.0)));
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        processed = vec3(0.0);

    fragColor = vec4(clamp(processed, vec3(0.0), vec3(10.0)), originalPixel.a);
}
)GLSL";

void logMessage(int priority, const char *message) {
    __android_log_print(priority, kLogTag, "%s", message);
}

GLuint compileShader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    if (!shader)
        return 0;
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE)
        return shader;

    GLint logLength = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
    std::string log(static_cast<size_t>(std::max(logLength, 1)), '\0');
    glGetShaderInfoLog(shader, logLength, nullptr, log.data());
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Shader compile failed: %s", log.c_str());
    glDeleteShader(shader);
    return 0;
}

GLuint createProgram() {
    GLuint vertex = compileShader(GL_VERTEX_SHADER, kVertexShader);
    GLuint fragment = compileShader(GL_FRAGMENT_SHADER, kFragmentShader);
    if (!vertex || !fragment) {
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        return 0;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked == GL_TRUE)
        return program;

    GLint logLength = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
    std::string log(static_cast<size_t>(std::max(logLength, 1)), '\0');
    glGetProgramInfoLog(program, logLength, nullptr, log.data());
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Program link failed: %s", log.c_str());
    glDeleteProgram(program);
    return 0;
}

GLint uniform(GLuint program, const char *name) {
    return glGetUniformLocation(program, name);
}

void setInt(GLuint program, const char *name, int value) {
    const GLint loc = uniform(program, name);
    if (loc >= 0) glUniform1i(loc, value);
}

void setFloat(GLuint program, const char *name, float value) {
    const GLint loc = uniform(program, name);
    if (loc >= 0) glUniform1f(loc, value);
}

} // namespace

PostProcessing &PostProcessing::instance() {
    static PostProcessing instance;
    return instance;
}

void PostProcessing::setConfig(const ModConfig &config) {
    std::lock_guard<std::mutex> lock(mConfigMutex);
    mConfig = config;
}

bool PostProcessing::start() {
    std::lock_guard<std::mutex> hookLock(mHookMutex);
    if (mHookInstalled)
        return true;

    if (!mEglHandle) {
        mEglHandle = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (!mEglHandle) {
        logMessage(ANDROID_LOG_ERROR, "Could not load libEGL.so; post-processing hook was not installed.");
        return false;
    }

    mSwapTarget = dlsym(mEglHandle, "eglSwapBuffers");
    if (!mSwapTarget) {
        logMessage(ANDROID_LOG_ERROR, "eglSwapBuffers was not found in libEGL.so.");
        return false;
    }

    void *originalSwap = nullptr;
    const int result = pl::memory::hook(
        reinterpret_cast<pl::memory::FuncPtr>(mSwapTarget),
        reinterpret_cast<pl::memory::FuncPtr>(&PostProcessing::swapBuffersHook),
        &originalSwap,
        pl::memory::HookPriority::Low);
    if (result != 0 || !originalSwap) {
        logMessage(ANDROID_LOG_ERROR, "Failed to hook eglSwapBuffers; effects remain disabled.");
        return false;
    }

    mOriginalSwap.store(originalSwap, std::memory_order_release);
    mHookInstalled = true;
    logMessage(ANDROID_LOG_INFO, "eglSwapBuffers hook installed.");
    return true;
}

void PostProcessing::stop() {
    std::lock_guard<std::mutex> hookLock(mHookMutex);
    if (!mHookInstalled)
        return;
    pl::memory::unhook(
        reinterpret_cast<pl::memory::FuncPtr>(mSwapTarget),
        reinterpret_cast<pl::memory::FuncPtr>(&PostProcessing::swapBuffersHook));
    mHookInstalled = false;
    // Keep the original trampoline alive: a swap callback may already be in
    // flight on the render thread while disable() removes the hook.
    logMessage(ANDROID_LOG_INFO, "eglSwapBuffers hook removed.");
}

EGLBoolean PostProcessing::swapBuffersHook(EGLDisplay display, EGLSurface surface) {
    auto &self = PostProcessing::instance();
    self.renderCurrentFrame();

    auto original = reinterpret_cast<EglSwapBuffersFn>(self.mOriginalSwap.load(std::memory_order_acquire));
    if (!original)
        return EGL_FALSE;
    return original(display, surface);
}

bool PostProcessing::ensureGlResources(EGLContext context) {
    if (context == EGL_NO_CONTEXT)
        return false;

    if (mGlContext != context) {
        // GL names belong to the old context. Do not delete them while a
        // different context is current; simply initialize a fresh set.
        mGlContext = context;
        mProgram = 0;
        mVertexArray = 0;
        mSceneTexture = 0;
        mResolveFramebuffer = 0;
        mTextureWidth = 0;
        mTextureHeight = 0;
        mShaderFailed = false;
    }

    if (mShaderFailed)
        return false;

    if (!mProgram) {
        mProgram = createProgram();
        if (!mProgram) {
            mShaderFailed = true;
            logMessage(ANDROID_LOG_ERROR, "Post-processing shader initialization failed for this EGL context.");
            return false;
        }
    }
    if (!mVertexArray)
        glGenVertexArrays(1, &mVertexArray);
    if (!mSceneTexture)
        glGenTextures(1, &mSceneTexture);
    if (!mResolveFramebuffer)
        glGenFramebuffers(1, &mResolveFramebuffer);

    return mProgram != 0 && mVertexArray != 0 && mSceneTexture != 0 && mResolveFramebuffer != 0;
}

bool PostProcessing::renderCurrentFrame() {
    ModConfig config;
    {
        std::lock_guard<std::mutex> lock(mConfigMutex);
        config = mConfig;
    }
    if (!config.enabled || eglGetCurrentContext() == EGL_NO_CONTEXT)
        return false;

    if (!ensureGlResources(eglGetCurrentContext()))
        return false;

    GLint viewport[4]{};
    GLint oldDrawFramebuffer = 0;
    GLint oldReadFramebuffer = 0;
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &oldDrawFramebuffer);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldReadFramebuffer);

    // At eglSwapBuffers the game's final image should be on the EGL surface.
    // Skip non-default FBOs rather than accidentally post-process an internal pass.
    if (oldDrawFramebuffer != 0 || oldReadFramebuffer != 0 || viewport[2] <= 0 || viewport[3] <= 0 || viewport[2] > 4096 || viewport[3] > 4096)
        return false;

    GLint oldProgram = 0;
    GLint oldVao = 0;
    GLint oldArrayBuffer = 0;
    GLint oldActiveTexture = GL_TEXTURE0;
    GLint oldTexture0 = 0;
    GLint oldSampler0 = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &oldVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &oldArrayBuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActiveTexture);

    const GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
    const GLboolean depthWasEnabled = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean scissorWasEnabled = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean cullWasEnabled = glIsEnabled(GL_CULL_FACE);
    const GLboolean stencilWasEnabled = glIsEnabled(GL_STENCIL_TEST);
    GLboolean oldColorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    glGetBooleanv(GL_COLOR_WRITEMASK, oldColorMask);

    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture0);
    glGetIntegerv(GL_SAMPLER_BINDING, &oldSampler0);
    glBindSampler(0, 0);
    glBindTexture(GL_TEXTURE_2D, mSceneTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Resolve the EGL surface into a normal 2D texture. Using blit instead of
    // glCopyTexImage2D also handles multisampled default surfaces on GLES 3.
    if (mTextureWidth != viewport[2] || mTextureHeight != viewport[3]) {
        glBindTexture(GL_TEXTURE_2D, mSceneTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, viewport[2], viewport[3], 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        mTextureWidth = viewport[2];
        mTextureHeight = viewport[3];
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mResolveFramebuffer);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_TEXTURE_2D, mSceneTexture, 0);
    if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(oldDrawFramebuffer));
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(oldReadFramebuffer));
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture0));
        glBindSampler(0, static_cast<GLuint>(oldSampler0));
        glActiveTexture(static_cast<GLenum>(oldActiveTexture));
        mTextureWidth = 0;
        mTextureHeight = 0;
        return false;
    }
    glBlitFramebuffer(viewport[0], viewport[1], viewport[0] + viewport[2], viewport[1] + viewport[3],
                      0, 0, viewport[2], viewport[3], GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glGenerateMipmap(GL_TEXTURE_2D);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glUseProgram(mProgram);
    glBindVertexArray(mVertexArray);
    glUniform1i(uniform(mProgram, "uScene"), 0);
    glUniform2f(uniform(mProgram, "uResolution"), static_cast<float>(viewport[2]), static_cast<float>(viewport[3]));

    const auto now = std::chrono::steady_clock::now();
    const float timeSeconds = std::chrono::duration<float>(now - kProcessStart).count();
    setFloat(mProgram, "uTime", timeSeconds);

    setInt(mProgram, "uLensGroup", config.LENS_MODULE_ENABLED ? 1 : 0);
    setInt(mProgram, "uCompressionGroup", config.COMPRESSION_MODULE_ENABLED ? 1 : 0);
    setInt(mProgram, "uPostGroup", config.POSTPROCESSING_MODULE_ENABLED ? 1 : 0);
    setInt(mProgram, "uSignalGroup", config.COMPRESSION_MODULE_ENABLED ? 1 : 0);
    setInt(mProgram, "uUserSuggestionsGroup", config.USER_SUGGESTIONS_MODULE_ENABLED ? 1 : 0);

    setInt(mProgram, "uAspectRatio", config.ASPECT_RATIO ? 1 : 0);
    setInt(mProgram, "uLensDistortion", config.USE_LENS_DISTORTION ? 1 : 0);
    setFloat(mProgram, "uLensDistortionStrength", config.LENS_DISTORTION_STRENGTH);
    setInt(mProgram, "uInterlacing", config.USE_INTERLACING ? 1 : 0);
    setFloat(mProgram, "uInterlacingStrength", config.INTERLACING_STRENGTH);
    setInt(mProgram, "uStaticNoise", config.USE_STATIC ? 1 : 0);
    setFloat(mProgram, "uStaticStrength", config.STATIC_STRENGTH);
    setInt(mProgram, "uMoreStatic", config.USE_MORE_STATIC ? 1 : 0);
    setInt(mProgram, "uSideJitter", config.SIDE_JITTER ? 1 : 0);
    setFloat(mProgram, "uSideJitterAmount", config.SIDE_JITTER_AMMOUNT);
    setInt(mProgram, "uAutoExposure", config.AUTO_EXPOSURE ? 1 : 0);
    setFloat(mProgram, "uManualExposure", config.MANUAL_EXPOSURE);
    setInt(mProgram, "uTonemapMethod", config.TONEMAPPING_METHOD);
    setFloat(mProgram, "uExtraZoom", config.EXTRA_ZOOM);
    setInt(mProgram, "uSharpness", config.USE_SHARPNESS ? 1 : 0);
    setFloat(mProgram, "uSharpnessStrength", static_cast<float>(config.SHARPNESS_STRENGTH));
    setFloat(mProgram, "uSharpnessRadius", config.SHARPNESS_RADIUS);
    setInt(mProgram, "uCompressionMethod", config.COMPRESSION_METHOD);
    setFloat(mProgram, "uLumaCompression", static_cast<float>(config.LUMA_COMPRESSION));
    setFloat(mProgram, "uChromaCompression", static_cast<float>(config.CHROMA_COMPRESSION));
    setFloat(mProgram, "uChromaBoost", config.CHROMA_BOOST);

    glDrawArrays(GL_TRIANGLES, 0, 3);

    // Restore the state the game had when it called eglSwapBuffers.
    glBindVertexArray(static_cast<GLuint>(oldVao));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(oldArrayBuffer));
    glUseProgram(static_cast<GLuint>(oldProgram));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(oldDrawFramebuffer));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(oldReadFramebuffer));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture0));
    glBindSampler(0, static_cast<GLuint>(oldSampler0));
    glActiveTexture(static_cast<GLenum>(oldActiveTexture));

    if (blendWasEnabled) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (depthWasEnabled) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (scissorWasEnabled) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (cullWasEnabled) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (stencilWasEnabled) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    glColorMask(oldColorMask[0], oldColorMask[1], oldColorMask[2], oldColorMask[3]);

    return true;
}

} // namespace mcvhs
