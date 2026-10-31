#include "mod/MyMod.h"

#include <filesystem>
#include <string>
#include <cstdlib>
#include <cmath>

#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>

#include "render/PostProcessing.h"

namespace mcvhs {
namespace {

bool parseBool(std::string_view value, bool &out) {
    if (value == "true" || value == "1" || value == "on") {
        out = true;
        return true;
    }
    if (value == "false" || value == "0" || value == "off") {
        out = false;
        return true;
    }
    return false;
}

bool parseInt(std::string_view value, int &out) {
    const std::string text(value);
    char *end = nullptr;
    const long parsed = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0')
        return false;
    out = static_cast<int>(parsed);
    return true;
}

bool parseFloat(std::string_view value, float &out) {
    const std::string text(value);
    char *end = nullptr;
    const float parsed = std::strtof(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || !std::isfinite(parsed))
        return false;
    out = parsed;
    return true;
}

std::string boolText(bool value) { return value ? "true" : "false"; }
std::string intText(int value) { return std::to_string(value); }
std::string floatText(float value) { return std::to_string(value); }

} // namespace

McVhsMod &McVhsMod::instance() {
    static McVhsMod instance;
    return instance;
}

McVhsMod::McVhsMod() : mSelf(*ll::mod::NativeMod::current()) {}

bool McVhsMod::load() {
    auto &self = getSelf();
    self.getLogger().info("Loading MC VHS PORT...");

    std::error_code ec;
    std::filesystem::create_directories(self.getDataDir(), ec);
    if (ec) {
        self.getLogger().error("Failed to create data directory: {}", ec.message());
        return false;
    }
    std::filesystem::create_directories(self.getConfigDir(), ec);
    if (ec) {
        self.getLogger().error("Failed to create config directory: {}", ec.message());
        return false;
    }

    mConfigFile.emplace();
    if (!mConfigFile->load()) {
        self.getLogger().error("Failed to load MC VHS PORT config.");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mConfigMutex);
        mConfig = mConfigFile->value();
        normalizeConfig(mConfig);
        mConfigFile->value() = mConfig;
        if (!mConfigFile->save())
            self.getLogger().warn("Could not normalize config.json on disk.");
    }

    refreshRuntime();
    registerMenuModules();
    self.getLogger().info("MC VHS PORT loaded. Config: {}", mConfigFile->configPath().string());
    return true;
}

bool McVhsMod::enable() {
    getSelf().getLogger().info("Enabling MC VHS PORT...");
    mLifecycleActive.store(true, std::memory_order_relaxed);
    refreshRuntime();
    return true;
}

bool McVhsMod::disable() {
    getSelf().getLogger().info("Disabling MC VHS PORT...");
    mLifecycleActive.store(false, std::memory_order_relaxed);
    PostProcessing::instance().stop();
    return true;
}

bool McVhsMod::unload() {
    getSelf().getLogger().info("Unloading MC VHS PORT...");
    mLifecycleActive.store(false, std::memory_order_relaxed);
    PostProcessing::instance().stop();
    unregisterMenuModules();
    std::lock_guard<std::mutex> lock(mConfigMutex);
    mConfigFile.reset();
    return true;
}

void McVhsMod::registerMenuModules() {
    ModConfig config;
    {
        std::lock_guard<std::mutex> lock(mConfigMutex);
        config = mConfig;
    }

    auto master = pl::modmenu::ModuleBuilder("mcvhs_master", "MC VHS PORT")
        .description("Master switch and Java-style shader profiles.")
        .modId(getSelf().getId())
        .defaultEnabled(config.enabled)
        .config("PROFILE", "Profile (0 Default, 1 DeepFried)", pl::modmenu::ConfigType::SliderInt,
                intText(config.PROFILE), "0", "1")
        .onToggle([this](std::string_view id, bool enabled) { handleModuleToggle(id, enabled); })
        .onConfigChanged([this](std::string_view id, std::string_view key, std::string_view value) {
            handleConfigChanged(id, key, value);
        });

    auto lens = pl::modmenu::ModuleBuilder("mcvhs_lens", "Lens")
        .description("Lens distortion and exposure controls, matching the Java Lens page.")
        .modId(getSelf().getId())
        .defaultEnabled(config.LENS_MODULE_ENABLED)
        .config("USE_LENS_DISTORTION", "Lens Distortion", pl::modmenu::ConfigType::Toggle, boolText(config.USE_LENS_DISTORTION))
        .config("LENS_DISTORTION_STRENGTH", "Distortion Strength", pl::modmenu::ConfigType::SliderFloat,
                floatText(config.LENS_DISTORTION_STRENGTH), "0.0", "0.05", "USE_LENS_DISTORTION")
        .config("AUTO_EXPOSURE", "Auto Exposure", pl::modmenu::ConfigType::Toggle, boolText(config.AUTO_EXPOSURE))
        .config("MANUAL_EXPOSURE", "Manual Exposure", pl::modmenu::ConfigType::SliderFloat,
                floatText(config.MANUAL_EXPOSURE), "0.1", "1.0")
        .onToggle([this](std::string_view id, bool enabled) { handleModuleToggle(id, enabled); })
        .onConfigChanged([this](std::string_view id, std::string_view key, std::string_view value) {
            handleConfigChanged(id, key, value);
        });

    auto compression = pl::modmenu::ModuleBuilder("mcvhs_compression", "Compression")
        .description("Aspect ratio, VHS signal artifacts and YUV / YIQ / YCbCr compression.")
        .modId(getSelf().getId())
        .defaultEnabled(config.COMPRESSION_MODULE_ENABLED)
        .config("ASPECT_RATIO", "Aspect Ratio (Java crop)", pl::modmenu::ConfigType::Toggle, boolText(config.ASPECT_RATIO))
        .config("USE_STATIC", "Static Noise", pl::modmenu::ConfigType::Toggle, boolText(config.USE_STATIC))
        .config("STATIC_STRENGTH", "Static Strength", pl::modmenu::ConfigType::SliderFloat,
                floatText(config.STATIC_STRENGTH), "0.1", "1.0", "USE_STATIC")
        .config("USE_INTERLACING", "Interlacing", pl::modmenu::ConfigType::Toggle, boolText(config.USE_INTERLACING))
        .config("INTERLACING_STRENGTH", "Interlacing Strength", pl::modmenu::ConfigType::SliderFloat,
                floatText(config.INTERLACING_STRENGTH), "5.0", "40.0", "USE_INTERLACING")
        .config("COMPRESSION_METHOD", "Method (0 YUV, 1 YIQ, 2 YCbCr)", pl::modmenu::ConfigType::SliderInt,
                intText(config.COMPRESSION_METHOD), "0", "2")
        .config("LUMA_COMPRESSION", "Luma Compression", pl::modmenu::ConfigType::SliderInt,
                intText(config.LUMA_COMPRESSION), "1", "8")
        .config("CHROMA_COMPRESSION", "Chroma Compression", pl::modmenu::ConfigType::SliderInt,
                intText(config.CHROMA_COMPRESSION), "1", "8")
        .config("CHROMA_BOOST", "Chroma Boost", pl::modmenu::ConfigType::SliderFloat,
                floatText(config.CHROMA_BOOST), "0.0", "2.0")
        .onToggle([this](std::string_view id, bool enabled) { handleModuleToggle(id, enabled); })
        .onConfigChanged([this](std::string_view id, std::string_view key, std::string_view value) {
            handleConfigChanged(id, key, value);
        });

    auto post = pl::modmenu::ModuleBuilder("mcvhs_postprocessing", "Post Processing")
        .description("Tonemapping, white-noise bursts, zoom and sharpening controls.")
        .modId(getSelf().getId())
        .defaultEnabled(config.POSTPROCESSING_MODULE_ENABLED)
        .config("TONEMAPPING_METHOD", "Tonemapping (0 None, 1 Tanh, 2 Beer, 3 Reinhard)", pl::modmenu::ConfigType::SliderInt,
                intText(config.TONEMAPPING_METHOD), "0", "3")
        .config("USE_MORE_STATIC", "White Noise Bursts", pl::modmenu::ConfigType::Toggle, boolText(config.USE_MORE_STATIC))
        .config("EXTRA_ZOOM", "Extra Zoom", pl::modmenu::ConfigType::SliderFloat,
                floatText(config.EXTRA_ZOOM), "0.0", "2.0")
        .config("USE_SHARPNESS", "Sharpness", pl::modmenu::ConfigType::Toggle, boolText(config.USE_SHARPNESS))
        .config("SHARPNESS_STRENGTH", "Sharpness Strength", pl::modmenu::ConfigType::SliderInt,
                intText(config.SHARPNESS_STRENGTH), "1", "10", "USE_SHARPNESS")
        .config("SHARPNESS_RADIUS", "Sharpness Radius", pl::modmenu::ConfigType::SliderFloat,
                floatText(config.SHARPNESS_RADIUS), "1.0", "8.0", "USE_SHARPNESS")
        .onToggle([this](std::string_view id, bool enabled) { handleModuleToggle(id, enabled); })
        .onConfigChanged([this](std::string_view id, std::string_view key, std::string_view value) {
            handleConfigChanged(id, key, value);
        });

    auto suggestions = pl::modmenu::ModuleBuilder("mcvhs_user_suggestions", "User Suggestions")
        .description("Optional side-jitter controls from the Java shader's UserSuggestions page.")
        .modId(getSelf().getId())
        .defaultEnabled(config.USER_SUGGESTIONS_MODULE_ENABLED)
        .config("SIDE_JITTER", "Side Jitter", pl::modmenu::ConfigType::Toggle, boolText(config.SIDE_JITTER))
        .config("SIDE_JITTER_AMMOUNT", "Side Jitter Amount", pl::modmenu::ConfigType::SliderFloat,
                floatText(config.SIDE_JITTER_AMMOUNT), "0.001", "0.01", "SIDE_JITTER")
        .onToggle([this](std::string_view id, bool enabled) { handleModuleToggle(id, enabled); })
        .onConfigChanged([this](std::string_view id, std::string_view key, std::string_view value) {
            handleConfigChanged(id, key, value);
        });

    bool ok = true;
    ok &= master.registerModule();
    ok &= lens.registerModule();
    ok &= compression.registerModule();
    ok &= post.registerModule();
    ok &= suggestions.registerModule();
    // Keep this true even on partial registration so unload can safely
    // unregister every module ID that may have succeeded.
    mMenuRegistered = true;

    if (!ok)
        getSelf().getLogger().warn("One or more MC VHS Mod Menu sections could not be registered.");
}

void McVhsMod::unregisterMenuModules() {
    if (!mMenuRegistered)
        return;
    pl::modmenu::unregisterModule("mcvhs_master");
    pl::modmenu::unregisterModule("mcvhs_lens");
    pl::modmenu::unregisterModule("mcvhs_compression");
    pl::modmenu::unregisterModule("mcvhs_postprocessing");
    pl::modmenu::unregisterModule("mcvhs_user_suggestions");
    mMenuRegistered = false;
}

void McVhsMod::handleModuleToggle(std::string_view moduleId, bool enabled) {
    std::lock_guard<std::mutex> lock(mConfigMutex);
    if (moduleId == "mcvhs_master") mConfig.enabled = enabled;
    else if (moduleId == "mcvhs_lens") mConfig.LENS_MODULE_ENABLED = enabled;
    else if (moduleId == "mcvhs_compression") mConfig.COMPRESSION_MODULE_ENABLED = enabled;
    else if (moduleId == "mcvhs_postprocessing") mConfig.POSTPROCESSING_MODULE_ENABLED = enabled;
    else if (moduleId == "mcvhs_user_suggestions") mConfig.USER_SUGGESTIONS_MODULE_ENABLED = enabled;
    normalizeConfig(mConfig);
    if (mConfigFile) {
        mConfigFile->value() = mConfig;
        if (!mConfigFile->save()) getSelf().getLogger().warn("Failed to save config after module toggle.");
    }
    PostProcessing::instance().setConfig(mConfig);
    if (mLifecycleActive.load(std::memory_order_relaxed)) {
        if (mConfig.enabled) PostProcessing::instance().start();
        else PostProcessing::instance().stop();
    }
}

void McVhsMod::handleConfigChanged(std::string_view, std::string_view key,
                                  std::string_view value) {
    mutateConfig(key, value);
}

void McVhsMod::mutateConfig(std::string_view key, std::string_view value) {
    bool handled = true;
    const bool profileChanged = key == "PROFILE";
    {
        std::lock_guard<std::mutex> lock(mConfigMutex);

        if (key == "PROFILE") {
            int profile = mConfig.PROFILE;
            if (parseInt(value, profile)) mConfig = applyProfile(mConfig, profile);
            else handled = false;
        } else if (key == "ASPECT_RATIO") handled = parseBool(value, mConfig.ASPECT_RATIO);
        else if (key == "USE_LENS_DISTORTION") handled = parseBool(value, mConfig.USE_LENS_DISTORTION);
        else if (key == "LENS_DISTORTION_STRENGTH") handled = parseFloat(value, mConfig.LENS_DISTORTION_STRENGTH);
        else if (key == "USE_INTERLACING") handled = parseBool(value, mConfig.USE_INTERLACING);
        else if (key == "INTERLACING_STRENGTH") handled = parseFloat(value, mConfig.INTERLACING_STRENGTH);
        else if (key == "USE_STATIC") handled = parseBool(value, mConfig.USE_STATIC);
        else if (key == "STATIC_STRENGTH") handled = parseFloat(value, mConfig.STATIC_STRENGTH);
        else if (key == "USE_MORE_STATIC") handled = parseBool(value, mConfig.USE_MORE_STATIC);
        else if (key == "SIDE_JITTER") handled = parseBool(value, mConfig.SIDE_JITTER);
        else if (key == "SIDE_JITTER_AMMOUNT") handled = parseFloat(value, mConfig.SIDE_JITTER_AMMOUNT);
        else if (key == "AUTO_EXPOSURE") handled = parseBool(value, mConfig.AUTO_EXPOSURE);
        else if (key == "MANUAL_EXPOSURE") handled = parseFloat(value, mConfig.MANUAL_EXPOSURE);
        else if (key == "TONEMAPPING_METHOD") handled = parseInt(value, mConfig.TONEMAPPING_METHOD);
        else if (key == "EXTRA_ZOOM") handled = parseFloat(value, mConfig.EXTRA_ZOOM);
        else if (key == "USE_SHARPNESS") handled = parseBool(value, mConfig.USE_SHARPNESS);
        else if (key == "SHARPNESS_STRENGTH") handled = parseInt(value, mConfig.SHARPNESS_STRENGTH);
        else if (key == "SHARPNESS_RADIUS") handled = parseFloat(value, mConfig.SHARPNESS_RADIUS);
        else if (key == "COMPRESSION_METHOD") handled = parseInt(value, mConfig.COMPRESSION_METHOD);
        else if (key == "LUMA_COMPRESSION") handled = parseInt(value, mConfig.LUMA_COMPRESSION);
        else if (key == "CHROMA_COMPRESSION") handled = parseInt(value, mConfig.CHROMA_COMPRESSION);
        else if (key == "CHROMA_BOOST") handled = parseFloat(value, mConfig.CHROMA_BOOST);
        else handled = false;

        if (!handled)
            return;

        normalizeConfig(mConfig);
        if (mConfigFile) {
            mConfigFile->value() = mConfig;
            if (!mConfigFile->save()) getSelf().getLogger().warn("Failed to save MC VHS config.");
        }
        PostProcessing::instance().setConfig(mConfig);
    }

    // Re-register the menu after profile application so the visible slider
    // values/defaults match the newly applied preset, not the previous one.
    if (profileChanged && mMenuRegistered) {
        unregisterMenuModules();
        registerMenuModules();
    }
}

void McVhsMod::refreshRuntime() {
    ModConfig snapshot;
    {
        std::lock_guard<std::mutex> lock(mConfigMutex);
        snapshot = mConfig;
    }
    PostProcessing::instance().setConfig(snapshot);

    if (!mLifecycleActive.load(std::memory_order_relaxed))
        return;
    if (snapshot.enabled) {
        if (!PostProcessing::instance().start())
            getSelf().getLogger().error("MC VHS effects are configured but the EGL hook could not be installed.");
    } else {
        PostProcessing::instance().stop();
    }
}

} // namespace mcvhs
