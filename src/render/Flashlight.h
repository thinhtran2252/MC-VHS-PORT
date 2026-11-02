#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include "mod/Config.h"

namespace mcvhs {

class FlashlightRenderer {
public:
    static FlashlightRenderer& instance();

    void setConfig(const ModConfig& config);
    bool start();
    void stop();

private:
    FlashlightRenderer() = default;
    FlashlightRenderer(const FlashlightRenderer&) = delete;
    FlashlightRenderer& operator=(const FlashlightRenderer&) = delete;

    static EGLBoolean swapBuffersHook(EGLDisplay display, EGLSurface surface);
    static void clearHook(GLbitfield mask);
    static void clearBufferFvHook(GLenum buffer, GLint drawBuffer, const GLfloat* values);
    static void clearBufferFiHook(GLenum buffer, GLint drawBuffer, GLfloat depth, GLint stencil);

    void beforeClear(GLbitfield mask);
    bool captureDepth(GLuint sourceFramebuffer, EGLDisplay display, EGLSurface surface,
                      int width, int height, GLint depthBits, GLint stencilBits);
    bool ensureDepthTargets(int width, int height, GLint depthBits, GLint stencilBits);
    bool renderCurrentFrame(EGLDisplay display, EGLSurface surface);
    bool ensureProgram();
    bool ensureRenderTargets(int width, int height);
    void updateLogTexture();
    void appendLog(const char* message);
    void finishFrame();
    void resetGlObjectsForNewContext(EGLContext context);

    mutable std::mutex mConfigMutex;
    ModConfig mConfig{};

    std::mutex mHookMutex;
    void* mEglLibrary{};
    void* mGlLibrary{};
    void* mSwapTarget{};
    void* mClearTarget{};
    void* mClearBufferFvTarget{};
    void* mClearBufferFiTarget{};
    std::atomic<void*> mOriginalSwap{nullptr};
    std::atomic<void*> mOriginalClear{nullptr};
    std::atomic<void*> mOriginalClearBufferFv{nullptr};
    std::atomic<void*> mOriginalClearBufferFi{nullptr};
    bool mSwapHookInstalled{};
    bool mClearHookInstalled{};
    bool mClearBufferFvHookInstalled{};
    bool mClearBufferFiHookInstalled{};

    std::atomic_flag mRendering = ATOMIC_FLAG_INIT;
    EGLContext mGlContext{EGL_NO_CONTEXT};

    GLuint mProgram{};
    GLuint mVertexArray{};
    GLuint mSceneTexture{};
    GLuint mLogTexture{};
    GLuint mDepthTexture{};
    GLuint mDepthScratchTexture{};
    GLuint mDepthFramebuffer{};
    GLint mUniformScene{-1};
    GLint mUniformDepth{-1};
    GLint mUniformLogs{-1};
    GLint mUniformResolution{-1};
    GLint mUniformFlashlightEnabled{-1};
    GLint mUniformHasDepth{-1};
    GLint mUniformReverseDepth{-1};
    GLint mUniformShowLogs{-1};
    GLint mUniformBrightness{-1};
    GLint mUniformDistance{-1};
    int mTextureWidth{};
    int mTextureHeight{};
    int mDepthWidth{};
    int mDepthHeight{};
    GLint mDepthBits{};
    GLint mStencilBits{};
    GLenum mDepthInternalFormat{};
    GLenum mDepthUploadFormat{GL_DEPTH_COMPONENT};
    GLenum mDepthPixelType{GL_UNSIGNED_SHORT};
    GLenum mDepthAttachment{GL_DEPTH_ATTACHMENT};

    EGLContext mTrackContext{EGL_NO_CONTEXT};
    EGLSurface mTrackSurface{EGL_NO_SURFACE};
    GLint mWorldFramebuffer{-1};
    GLint mSceneStartProgram{};
    bool mSceneStarted{};
    bool mCaptureProcessed{};
    std::atomic<bool> mDepthReady{false};
    std::atomic<bool> mReverseDepth{false};
    std::atomic<bool> mMissingDepthLogged{false};
    bool mRenderPassLogged{};

    std::mutex mLogMutex;
    std::array<std::string, 8> mLogLines{};
    std::size_t mLogStart{};
    std::size_t mLogCount{};
    std::uint64_t mLogRevision{};
    std::uint64_t mUploadedLogRevision{~std::uint64_t{0}};
};

} // namespace mcvhs
