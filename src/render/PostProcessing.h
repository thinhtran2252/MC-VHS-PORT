#pragma once

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <atomic>
#include <mutex>

#include "mod/Config.h"

namespace mcvhs {

class PostProcessing {
  public:
    static PostProcessing &instance();

    bool start();
    void stop();
    void setConfig(const ModConfig &config);

  private:
    PostProcessing() = default;
    PostProcessing(const PostProcessing &) = delete;
    PostProcessing &operator=(const PostProcessing &) = delete;

    bool renderCurrentFrame();
    bool ensureGlResources(EGLContext context);
    static EGLBoolean swapBuffersHook(EGLDisplay display, EGLSurface surface);

    std::mutex mConfigMutex;
    std::mutex mHookMutex;
    ModConfig mConfig{};

    void *mEglHandle{};
    void *mSwapTarget{};
    std::atomic<void *> mOriginalSwap{nullptr};
    bool mHookInstalled{};

    EGLContext mGlContext{EGL_NO_CONTEXT};
    GLuint mProgram{};
    GLuint mVertexArray{};
    GLuint mSceneTexture{};
    GLuint mResolveFramebuffer{};
    GLint mTextureWidth{};
    GLint mTextureHeight{};
    bool mShaderFailed{};
};

} // namespace mcvhs
