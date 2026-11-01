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

// Lazily allocated depth texture. It is populated only when the EGL backbuffer
// exposes a single-sample depth buffer that can be copied without CPU readback.
GLuint gDepthTexture = 0;
GLenum gDepthInternalFormat = 0;
GLenum gDepthUploadFormat = GL_DEPTH_COMPONENT;
GLenum gDepthPixelType = GL_UNSIGNED_SHORT;
GLenum gDepthAttachment = GL_DEPTH_ATTACHMENT;
int gDepthWidth = 0;
int gDepthHeight = 0;
bool gDepthReady = false;
bool gDepthWarningLogged = false;

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
uniform sampler2D uDepth;
uniform vec2 uResolution;
uniform float uTime;
uniform float uMaxMip;
uniform int uStage; // 0 composite2, 1 composite4, 2 composite5, 3 composite6
uniform int uOptimize;
uniform int uUseFlashlight;
uniform int uHasDepth;
uniform float uFlashlightBrightness;
uniform float uFlashlightDistance;

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

// A uniform spherical direction has the same probability distribution as a
// normalized 3D Gaussian vector, but needs only two random values and avoids
// the Gaussian log/sqrt/cos sequence plus normalize().
vec3 randomDirection(inout uint state) {
    float z = randomValue(state) * 2.0 - 1.0;
    float angle = 6.28318530718 * randomValue(state);
    float radius = sqrt(max(0.0, 1.0 - z * z));
    return vec3(radius * cos(angle), radius * sin(angle), z);
}

// Small integer avalanche hash. Unlike the source Gaussian generator, this
// avoids log/sqrt/cos/normalize for every fragment when Optimize is enabled.
uint hash32(uint value) {
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

vec2 fastNoise2D(uint seed) {
    uint x = hash32(seed + 0x9e3779b9u);
    uint y = hash32(seed + 0x85ebca6bu);
    const float scale = 1.0 / 16777215.0;
    return vec2(float(x >> 8u), float(y >> 8u)) * scale * 2.0 - 1.0;
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

// Pade [5/4] approximation for tanh(x). Its maximum absolute error is about
// 0.00136 over the [0, 10] range used by this pass, before saturation.
vec3 fastTanh(vec3 x) {
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    vec3 numerator = x * (945.0 + x2 * (105.0 + x2));
    vec3 denominator = 945.0 + x2 * (420.0 + 15.0 * x2);
    return clamp(numerator / denominator, vec3(-1.0), vec3(1.0));
}

// Pade [2/2] approximation to exp(-x), evaluated at x/16 and squared four
// times. composite2 clamps input RGB to [0, 10], so the reduced argument stays
// in [0, 0.625] and the approximation remains close to the original Beer curve.
vec3 fastBeer(vec3 x) {
    vec3 t = max(x, vec3(0.0)) * 0.0625;
    vec3 t2 = t * t;
    vec3 e = (1.0 - 0.5 * t + t2 * 0.0833333333333) /
             (1.0 + 0.5 * t + t2 * 0.0833333333333);
    e *= e;
    e *= e;
    e *= e;
    e *= e;
    return 1.0 - e;
}

// Soft spot profile inspired by the source Spotlight.png. This is only the
// beam shape: the visible light is still multiplied by each pixel's real depth
// so distant geometry fades out instead of receiving a flat camera overlay.
float flashlightProfile(vec2 uv) {
    vec2 p = (uv - vec2(0.5)) * vec2(uResolution.x / max(uResolution.y, 1.0), 1.0);
    float radius = length(p);
    float hotspot = 1.0 - smoothstep(0.015, 0.22, radius);
    float halo = smoothstep(0.07, 0.13, radius) *
                 (1.0 - smoothstep(0.14, 0.40, radius)) * 0.52;
    float spill = (1.0 - smoothstep(0.16, 0.82, radius)) * 0.18;
    return clamp(max(hotspot, max(halo, spill)), 0.0, 1.0);
}

vec3 applyDepthFlashlight(vec3 sceneColor, vec2 uv) {
    if (uUseFlashlight == 0 || uHasDepth == 0 ||
        uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        return sceneColor;

    float depth = texture(uDepth, uv).r;

    // The Android preloader SDK used by this project does not expose Bedrock's
    // mce::Camera clip planes. These defaults approximate the normal Bedrock
    // perspective range; the attenuation equation mirrors MC VHS's
    // 1 - sqrt(linearDepth) / TORCH_DISTANCE calculation.
    const float nearPlane = 0.05;
    const float farPlane = 512.0;
    float denominator = depth * (nearPlane - farPlane) + farPlane;
    float linearDepth = (nearPlane * farPlane) / max(denominator, 0.000001);
    float distanceFade = clamp(
        1.0 - sqrt(max(linearDepth, 0.0)) / max(uFlashlightDistance, 0.1),
        0.0, 1.0
    );

    float amount = flashlightProfile(uv) * distanceFade * uFlashlightBrightness;
    // A small warm fill term lets genuinely dark pixels become visible; the
    // scene-colour term preserves texture/detail instead of painting a flat disk.
    const vec3 flashlightTint = vec3(1.0, 0.88, 0.70);
    vec3 reflectedScene = sceneColor * amount * 0.82;
    vec3 lowLightFill = flashlightTint * amount * 0.18;
    return clamp(sceneColor + reflectedScene + lowLightFill, vec3(0.0), vec3(1.0));
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
        vec3 yuv = rgbToYuv(color.rgb);
        if (uOptimize != 0)
            yuv.yz += fastNoise2D(seed) * uStaticStrength;
        else
            yuv.yz += randomDirection(seed).xy * uStaticStrength;
        color.rgb = yuvToRgb(yuv);
    }

    // Algebraically equivalent to (2*x)/(2-x), but with fewer vector ops.
    if (uOptimize != 0)
        color.rgb = color.rgb / max(vec3(0.00005), vec3(1.0) - color.rgb * 0.5);
    else
        color.rgb = (color.rgb + color.rgb) / max(vec3(0.0001), vec3(2.0) - color.rgb);

    if (uAutoExposure != 0) {
        vec3 exposureSample = textureLod(uScene, vec2(0.5), min(8.0, uMaxMip)).rgb;
        // Only luma is consumed here. Avoid calculating the unused U/V channels.
        float averageLuma = dot(exposureSample, vec3(0.299, 0.587, 0.114)) + 0.5;
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
        if (uOptimize != 0) {
            color.rgb = fastTanh(clamp(color.rgb, vec3(-10.0), vec3(10.0)));
        } else {
            vec3 x = clamp(color.rgb, vec3(-10.0), vec3(10.0));
            color.rgb = 2.0 / (1.0 + exp(-2.0 * x)) - 1.0;
        }
    } else if (uTonemapMethod == 2) {
        color.rgb = (uOptimize != 0)
            ? fastBeer(color.rgb)
            : 1.0 - exp(-color.rgb);
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
    vec3 chromaSource;

    if (uOptimize != 0 && lumaLod == chromaLod)
        chromaSource = lumaSource;
    else
        chromaSource = textureLod(uScene, uv, chromaLod).rgb;

    vec3 result;
    if (uOptimize != 0) {
        // Calculate only the channels consumed by each color-space transform.
        // The coefficients and matrix layout match the original Java shader.
        if (uCompressionMethod == 0) {
            float y = dot(lumaSource, vec3(0.299, 0.587, 0.114));
            float chromaY = dot(chromaSource, vec3(0.299, 0.587, 0.114));
            vec2 uvChroma = vec2(
                0.493 * (chromaSource.b - chromaY),
                0.877 * (chromaSource.r - chromaY)
            ) * (1.0 + uChromaBoost);
            result = yuvToRgb(vec3(y, uvChroma));
        } else if (uCompressionMethod == 1) {
            float y = dot(lumaSource, vec3(0.299, 0.596, 0.211));
            vec2 iq = vec2(
                dot(chromaSource, vec3(0.587, -0.274, -0.523)),
                dot(chromaSource, vec3(0.114, -0.322, 0.312))
            ) * (1.0 + uChromaBoost);
            result = yiqToRgb(vec3(y, iq));
        } else {
            float y = dot(lumaSource, vec3(0.299, 0.587, 0.114));
            float chromaY = dot(chromaSource, vec3(0.299, 0.587, 0.114));
            vec2 cbcr = vec2(
                (chromaSource.b - chromaY) * 0.565,
                (chromaSource.r - chromaY) * 0.713
            ) * (1.0 + uChromaBoost);
            result = ycbcrToRgb(vec3(y, cbcr));
        }
    } else if (uCompressionMethod == 0) {
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
        float random = (uOptimize != 0)
            ? float(hash32(seed) >> 8u) * (1.0 / 16777215.0)
            : randomValue(seed);
        float jitter = ((random * random) * 2.0 - 1.0) * uSideJitterAmount;
        coord.x += jitter;
    }

    vec4 color = texture(uScene, coord);
    if (uUseSharpness != 0) {
        vec3 blurred = textureLod(uScene, coord, min(uSharpnessRadius, uMaxMip)).rgb;
        // Equivalent to mix(color, blurred, -strength), written explicitly.
        color.rgb = color.rgb + (color.rgb - blurred) * uSharpnessStrength;
    }
    return color;
}

void main() {
    if (uStage == 0) {
        vec4 color = composite2(vTexCoord);
        // composite2 and composite4 have no texture dependency between them.
        // Fuse these full-screen stages only under Optimize to save one draw
        // and one full-resolution intermediate write/read pair.
        if (uOptimize != 0) color = composite4(color);
        fragColor = color;
    } else if (uStage == 1) {
        fragColor = composite4(texture(uScene, vTexCoord));
    } else if (uStage == 2) {
        fragColor = composite5(vTexCoord);
    } else {
        vec4 color = composite6(vTexCoord);
        color.rgb = applyDepthFlashlight(color.rgb, vTexCoord);
        fragColor = color;
    }
}
)GLSL";

struct UniformLocations {
    GLint scene{-1};
    GLint resolution{-1};
    GLint time{-1};
    GLint maxMip{-1};
    GLint stage{-1};
    GLint optimize{-1};
    GLint depth{-1};
    GLint useFlashlight{-1};
    GLint hasDepth{-1};
    GLint flashlightBrightness{-1};
    GLint flashlightDistance{-1};
    GLint useStatic{-1};
    GLint staticStrength{-1};
    GLint useLensDistortion{-1};
    GLint lensDistortionStrength{-1};
    GLint aspectRatio{-1};
    GLint autoExposure{-1};
    GLint manualExposure{-1};
    GLint tonemapMethod{-1};
    GLint compressionMethod{-1};
    GLint lumaCompression{-1};
    GLint chromaCompression{-1};
    GLint chromaBoost{-1};
    GLint extraZoom{-1};
    GLint useSharpness{-1};
    GLint sharpnessStrength{-1};
    GLint sharpnessRadius{-1};
    GLint sideJitter{-1};
    GLint sideJitterAmount{-1};

    void load(GLuint program) {
        scene = glGetUniformLocation(program, "uScene");
        resolution = glGetUniformLocation(program, "uResolution");
        time = glGetUniformLocation(program, "uTime");
        maxMip = glGetUniformLocation(program, "uMaxMip");
        stage = glGetUniformLocation(program, "uStage");
        optimize = glGetUniformLocation(program, "uOptimize");
        depth = glGetUniformLocation(program, "uDepth");
        useFlashlight = glGetUniformLocation(program, "uUseFlashlight");
        hasDepth = glGetUniformLocation(program, "uHasDepth");
        flashlightBrightness = glGetUniformLocation(program, "uFlashlightBrightness");
        flashlightDistance = glGetUniformLocation(program, "uFlashlightDistance");
        useStatic = glGetUniformLocation(program, "uUseStatic");
        staticStrength = glGetUniformLocation(program, "uStaticStrength");
        useLensDistortion = glGetUniformLocation(program, "uUseLensDistortion");
        lensDistortionStrength = glGetUniformLocation(program, "uLensDistortionStrength");
        aspectRatio = glGetUniformLocation(program, "uAspectRatio");
        autoExposure = glGetUniformLocation(program, "uAutoExposure");
        manualExposure = glGetUniformLocation(program, "uManualExposure");
        tonemapMethod = glGetUniformLocation(program, "uTonemapMethod");
        compressionMethod = glGetUniformLocation(program, "uCompressionMethod");
        lumaCompression = glGetUniformLocation(program, "uLumaCompression");
        chromaCompression = glGetUniformLocation(program, "uChromaCompression");
        chromaBoost = glGetUniformLocation(program, "uChromaBoost");
        extraZoom = glGetUniformLocation(program, "uExtraZoom");
        useSharpness = glGetUniformLocation(program, "uUseSharpness");
        sharpnessStrength = glGetUniformLocation(program, "uSharpnessStrength");
        sharpnessRadius = glGetUniformLocation(program, "uSharpnessRadius");
        sideJitter = glGetUniformLocation(program, "uSideJitter");
        sideJitterAmount = glGetUniformLocation(program, "uSideJitterAmount");
    }
};

UniformLocations gUniforms;

struct SavedGlState {
    GLint drawFramebuffer{};
    GLint readFramebuffer{};
    GLint viewport[4]{};
    GLint program{};
    GLint vertexArray{};
    GLint activeTexture{};
    GLint texture0{};
    GLint sampler0{};
    GLint texture1{};
    GLint sampler1{};
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
        glActiveTexture(GL_TEXTURE1);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture1);
        glGetIntegerv(GL_SAMPLER_BINDING, &sampler1);
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
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture1));
        glBindSampler(1, static_cast<GLuint>(sampler1));
        glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
        blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
        depth ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
        stencil ? glEnable(GL_STENCIL_TEST) : glDisable(GL_STENCIL_TEST);
        scissor ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
        cull ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
        glActiveTexture(static_cast<GLenum>(activeTexture));
    }
};

bool ensureDepthTexture(GLuint captureFramebuffer, int width, int height,
                        GLint depthBits, GLint stencilBits) {
    GLenum internalFormat = 0;
    GLenum uploadFormat = GL_DEPTH_COMPONENT;
    GLenum pixelType = GL_UNSIGNED_SHORT;
    GLenum attachment = GL_DEPTH_ATTACHMENT;

    if (stencilBits > 0) {
        // The common EGL packed format is D24S8. Matching both aspects is
        // required by GLES depth blits, even when only the depth bit is copied.
        if (depthBits != 24 || stencilBits != 8) return false;
        internalFormat = GL_DEPTH24_STENCIL8;
        uploadFormat = GL_DEPTH_STENCIL;
        pixelType = GL_UNSIGNED_INT_24_8;
        attachment = GL_DEPTH_STENCIL_ATTACHMENT;
    } else if (depthBits == 16) {
        internalFormat = GL_DEPTH_COMPONENT16;
        uploadFormat = GL_DEPTH_COMPONENT;
        pixelType = GL_UNSIGNED_SHORT;
        attachment = GL_DEPTH_ATTACHMENT;
    } else if (depthBits == 24) {
        internalFormat = GL_DEPTH_COMPONENT24;
        uploadFormat = GL_DEPTH_COMPONENT;
        pixelType = GL_UNSIGNED_INT;
        attachment = GL_DEPTH_ATTACHMENT;
    } else if (depthBits >= 32) {
        internalFormat = GL_DEPTH_COMPONENT32F;
        uploadFormat = GL_DEPTH_COMPONENT;
        pixelType = GL_FLOAT;
        attachment = GL_DEPTH_ATTACHMENT;
    } else {
        return false;
    }

    if (!gDepthTexture) glGenTextures(1, &gDepthTexture);
    if (!gDepthTexture) return false;

    const bool storageChanged = gDepthWidth != width || gDepthHeight != height ||
                                gDepthInternalFormat != internalFormat ||
                                gDepthUploadFormat != uploadFormat ||
                                gDepthPixelType != pixelType ||
                                gDepthAttachment != attachment;
    if (!storageChanged) return true;

    glBindTexture(GL_TEXTURE_2D, gDepthTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internalFormat), width, height, 0,
                 uploadFormat, pixelType, nullptr);

    glBindFramebuffer(GL_FRAMEBUFFER, captureFramebuffer);
    if (gDepthInternalFormat != 0 && gDepthAttachment != attachment)
        glFramebufferTexture2D(GL_FRAMEBUFFER, gDepthAttachment, GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D, gDepthTexture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D, 0, 0);
        glDeleteTextures(1, &gDepthTexture);
        gDepthTexture = 0;
        gDepthInternalFormat = 0;
        gDepthUploadFormat = GL_DEPTH_COMPONENT;
        gDepthPixelType = GL_UNSIGNED_SHORT;
        gDepthAttachment = GL_DEPTH_ATTACHMENT;
        gDepthWidth = 0;
        gDepthHeight = 0;
        return false;
    }

    gDepthInternalFormat = internalFormat;
    gDepthUploadFormat = uploadFormat;
    gDepthPixelType = pixelType;
    gDepthAttachment = attachment;
    gDepthWidth = width;
    gDepthHeight = height;
    return true;
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
    // Uniform lookups are fixed for the linked program; do them once, not once
    // per setting per pass on every frame.
    gUniforms.load(mProgram);
    return true;
}

bool PostProcessing::allocateTargets(int width, int height, GLint internalFormat, GLenum type) {
    glActiveTexture(GL_TEXTURE0);

    // The swapchain image is copied into an RGBA8 texture, matching the
    // screen-copy path used by LeviPostFX. Only subsequent processing targets
    // use RGBA16F when the device supports renderable half-float textures.
    glBindTexture(GL_TEXTURE_2D, mCaptureTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    if (gDepthTexture) {
        glBindTexture(GL_TEXTURE_2D, gDepthTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(gDepthInternalFormat), width, height, 0,
                     gDepthUploadFormat, gDepthPixelType, nullptr);
        gDepthWidth = width;
        gDepthHeight = height;
    }

    for (GLuint texture : {mLensTexture, mToneTexture, mCompressionTexture}) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0,
                     GL_RGBA, type, nullptr);
    }

    // composite4 samples the lens result only at base LOD. Keeping this texture
    // non-mipmapped makes it complete without paying for a mip chain every frame.
    glBindTexture(GL_TEXTURE_2D, mLensTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    glBindFramebuffer(GL_FRAMEBUFFER, mCaptureFramebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mCaptureTexture, 0);
    if (gDepthTexture)
        glFramebufferTexture2D(GL_FRAMEBUFFER, gDepthAttachment, GL_TEXTURE_2D, gDepthTexture, 0);
    else
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
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
    if (width == mTextureWidth && height == mTextureHeight && mCaptureTexture &&
        mLensTexture && mToneTexture && mCompressionTexture)
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
    // Restore the base-level-only sampler after the general max-level setup.
    glBindTexture(GL_TEXTURE_2D, mLensTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

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
        // Every texture attached here was validated in ensureTargets(). Avoid
        // repeating a driver-side completeness query for each pass every frame.
    }
    glViewport(0, 0, width, height);
    glUseProgram(mProgram);
    glBindVertexArray(mVertexArray);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, inputTexture);
    glBindSampler(0, 0);
    glUniform1i(gUniforms.scene, 0);
    glUniform1i(gUniforms.stage, stage);
    glUniform1i(gUniforms.optimize, config.Optimize ? 1 : 0);

    if (stage == 0) {
        glUniform2f(gUniforms.resolution, static_cast<float>(width), static_cast<float>(height));
        glUniform1f(gUniforms.time, timeSeconds);
        glUniform1f(gUniforms.maxMip, maxMip);
        glUniform1i(gUniforms.useStatic, config.USE_STATIC ? 1 : 0);
        glUniform1f(gUniforms.staticStrength, config.STATIC_STRENGTH);
        glUniform1i(gUniforms.useLensDistortion, config.USE_LENS_DISTORTION ? 1 : 0);
        glUniform1f(gUniforms.lensDistortionStrength, config.LENS_DISTORTION_STRENGTH);
        glUniform1i(gUniforms.aspectRatio, config.ASPECT_RATIO ? 1 : 0);
        glUniform1i(gUniforms.autoExposure, config.AUTO_EXPOSURE ? 1 : 0);
        glUniform1f(gUniforms.manualExposure, config.MANUAL_EXPOSURE);
        if (config.Optimize)
            glUniform1i(gUniforms.tonemapMethod, config.TONEMAPPING_METHOD);
    } else if (stage == 1) {
        glUniform1i(gUniforms.tonemapMethod, config.TONEMAPPING_METHOD);
    } else if (stage == 2) {
        glUniform1f(gUniforms.maxMip, maxMip);
        glUniform1i(gUniforms.compressionMethod, config.COMPRESSION_METHOD);
        glUniform1f(gUniforms.lumaCompression, static_cast<float>(config.LUMA_COMPRESSION));
        glUniform1f(gUniforms.chromaCompression, static_cast<float>(config.CHROMA_COMPRESSION));
        glUniform1f(gUniforms.chromaBoost, config.CHROMA_BOOST);
    } else {
        glUniform2f(gUniforms.resolution, static_cast<float>(width), static_cast<float>(height));
        glUniform1f(gUniforms.time, timeSeconds);
        glUniform1f(gUniforms.maxMip, maxMip);
        glUniform1f(gUniforms.extraZoom, config.EXTRA_ZOOM);
        glUniform1i(gUniforms.useSharpness, config.USE_SHARPNESS ? 1 : 0);
        glUniform1f(gUniforms.sharpnessStrength, static_cast<float>(config.SHARPNESS_STRENGTH));
        glUniform1f(gUniforms.sharpnessRadius, config.SHARPNESS_RADIUS);
        glUniform1i(gUniforms.sideJitter, config.SIDE_JITTER ? 1 : 0);
        glUniform1f(gUniforms.sideJitterAmount, config.SIDE_JITTER_AMMOUNT);
        glUniform1i(gUniforms.depth, 1);
        glUniform1i(gUniforms.useFlashlight, config.USE_FLASHLIGHT ? 1 : 0);
        glUniform1i(gUniforms.hasDepth, gDepthReady ? 1 : 0);
        glUniform1f(gUniforms.flashlightBrightness, config.FLASHLIGHT_BRIGHTNESS);
        glUniform1f(gUniforms.flashlightDistance, config.FLASHLIGHT_DISTANCE);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, gDepthTexture);
        glBindSampler(1, 0);
        glActiveTexture(GL_TEXTURE0);
    }

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
        gDepthTexture = 0;
        gDepthInternalFormat = 0;
        gDepthUploadFormat = GL_DEPTH_COMPONENT;
        gDepthPixelType = GL_UNSIGNED_SHORT;
        gDepthAttachment = GL_DEPTH_ATTACHMENT;
        gDepthWidth = 0;
        gDepthHeight = 0;
        gDepthReady = false;
        gDepthWarningLogged = false;
        mTextureWidth = mTextureHeight = 0;
        gUniforms = UniformLocations{};
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

    // Capture the full EGL surface. Build a mip chain only when auto exposure
    // will actually sample it; the no-exposure path is base-level only.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gDepthReady = false;
    if (config.USE_FLASHLIGHT) {
        GLint defaultDepthBits = 0;
        GLint defaultStencilBits = 0;
        GLint defaultSampleBuffers = 0;
        glGetIntegerv(GL_DEPTH_BITS, &defaultDepthBits);
        glGetIntegerv(GL_STENCIL_BITS, &defaultStencilBits);
        glGetIntegerv(GL_SAMPLE_BUFFERS, &defaultSampleBuffers);
        if (defaultDepthBits > 0 && defaultSampleBuffers == 0 &&
            ensureDepthTexture(mCaptureFramebuffer, width, height, defaultDepthBits, defaultStencilBits)) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mCaptureFramebuffer);
            glBlitFramebuffer(0, 0, width, height, 0, 0, width, height,
                              GL_DEPTH_BUFFER_BIT, GL_NEAREST);
            gDepthReady = true;
            gDepthWarningLogged = false;
        } else if (!gDepthWarningLogged) {
            logMessage(ANDROID_LOG_WARN,
                "Flashlight skipped: EGL backbuffer has no compatible single-sample depth buffer; no screen-space fallback is used");
            gDepthWarningLogged = true;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mCaptureTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    config.AUTO_EXPOSURE ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
    if (config.AUTO_EXPOSURE)
        glGenerateMipmap(GL_TEXTURE_2D);

    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    // Optimize fuses composite2 + composite4 into one fullscreen pass. This
    // removes one full-resolution draw and one intermediate RGBA16F write/read.
    if (!drawPass(mCaptureTexture, mWorkFramebuffer, mLensTexture, 0,
                  width, height, config, timeSeconds, maxMip)) {
        restore(); return false;
    }

    GLuint compressionInput = mLensTexture;
    if (config.Optimize) {
        glBindTexture(GL_TEXTURE_2D, mLensTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glGenerateMipmap(GL_TEXTURE_2D);
    } else {
        // Keep the non-optimized path and its original intermediate unchanged.
        glBindTexture(GL_TEXTURE_2D, mLensTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        if (!drawPass(mLensTexture, mWorkFramebuffer, mToneTexture, 1,
                      width, height, config, timeSeconds, maxMip)) {
            restore(); return false;
        }
        glBindTexture(GL_TEXTURE_2D, mToneTexture);
        glGenerateMipmap(GL_TEXTURE_2D);
        compressionInput = mToneTexture;
    }

    if (!drawPass(compressionInput, mWorkFramebuffer, mCompressionTexture, 2,
                  width, height, config, timeSeconds, maxMip)) {
        restore(); return false;
    }

    // composite6 only reads mip levels when sharpness is enabled.
    glBindTexture(GL_TEXTURE_2D, mCompressionTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    config.USE_SHARPNESS ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    if (config.USE_SHARPNESS)
        glGenerateMipmap(GL_TEXTURE_2D);

    if (!drawPass(mCompressionTexture, 0, 0, 3,
                  width, height, config, timeSeconds, maxMip)) {
        restore(); return false;
    }

    restore();
    return true;
}

} // namespace mcvhs
