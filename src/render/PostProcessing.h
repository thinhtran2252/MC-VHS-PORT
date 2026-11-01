#pragma once

#include <atomic>
#include <mutex>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include "mod/Config.h"

namespace mcvhs {

class PostProcessing {
public:
    static PostProcessing& instance();

    void setConfig(const ModConfig& config);
    bool start();
    void stop();

private:
    PostProcessing() = default;
    PostProcessing(const PostProcessing&) = delete;
    PostProcessing& operator=(const PostProcessing&) = delete;

    static EGLBoolean swapBuffersHook(EGLDisplay display, EGLSurface surface);
    bool renderCurrentFrame();
    bool ensureProgram();
    bool ensureTargets(int width, int height);
    bool allocateTargets(int width, int height, GLint internalFormat, GLenum type);
    bool drawPass(GLuint inputTexture, GLuint outputFramebuffer, GLuint outputTexture,
                  int stage, int width, int height, const ModConfig& config,
                  float timeSeconds, float maxMip);

    mutable std::mutex mConfigMutex;
    ModConfig mConfig{};

    std::mutex mHookMutex;
    void* mEglLibrary{};
    void* mSwapTarget{};
    std::atomic<void*> mOriginalSwap{nullptr};
    bool mHookInstalled{};

    std::atomic_flag mRendering = ATOMIC_FLAG_INIT;
    EGLContext mGlContext{EGL_NO_CONTEXT};
    GLuint mProgram{};
    GLuint mVertexArray{};
    GLuint mCaptureTexture{};
    GLuint mLensTexture{};
    GLuint mToneTexture{};
    GLuint mCompressionTexture{};
    GLuint mCaptureFramebuffer{};
    GLuint mWorkFramebuffer{};
    int mTextureWidth{};
    int mTextureHeight{};
    GLint mTextureFormat{GL_RGBA8};
    GLenum mTextureType{GL_UNSIGNED_BYTE};
};

} // namespace mcvhs
