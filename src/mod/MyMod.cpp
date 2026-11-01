#include "mod/MyMod.h"

#include "render/PostProcessing.h"

#include <pl/ModMenu.hpp>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace mcvhs {
namespace {

bool parseBool(std::string_view value, bool& out) {
    if (value == "true" || value == "1" || value == "on") { out = true; return true; }
    if (value == "false" || value == "0" || value == "off") { out = false; return true; }
    return false;
}

bool parseInt(std::string_view value, int& out) {
    const std::string text(value);
    char* end = nullptr;
    const long parsed = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') return false;
    out = static_cast<int>(parsed);
    return true;
}

bool parseFloat(std::string_view value, float& out) {
    const std::string text(value);
    char* end = nullptr;
    const float parsed = std::strtof(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || !std::isfinite(parsed)) return false;
    out = parsed;
    return true;
}

std::string boolText(bool value) { return value ? "true" : "false"; }
std::string intText(int value) { return std::to_string(value); }
std::string floatText(float value) { return std::to_string(value); }

} // namespace

McVhsMod& McVhsMod::instance() {
    static McVhsMod instance;
    return instance;
}

McVhsMod::McVhsMod() : mSelf(*ll::mod::NativeMod::current()) {}

bool McVhsMod::load() {
    auto& self = getSelf();
    self.getLogger().info("Loading MC VHS PORT v0.2.0");

    std::error_code ec;
    std::filesystem::create_directories(self.getDataDir(), ec);
    if (ec) {
        self.getLogger().error("Could not create mod data directory: {}", ec.message());
        return false;
    }
    std::filesystem::create_directories(self.getConfigDir(), ec);
    if (ec) {
        self.getLogger().error("Could not create mod config directory: {}", ec.message());
        return false;
    }

    mConfigFile.emplace();
    if (!mConfigFile->load()) {
        self.getLogger().error("Could not load config.json");
        mConfigFile.reset();
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mConfigMutex);
        mConfig = mConfigFile->value();
        normalizeConfig(mConfig);
        mConfigFile->value() = mConfig;
        if (!mConfigFile->save()) self.getLogger().warn("Could not normalize config.json");
    }
    PostProcessing::instance().setConfig(mConfig);
    self.getLogger().info("MC VHS PORT config loaded");
    return true;
}

bool McVhsMod::enable() {
    if (!registerMenu()) {
        getSelf().getLogger().error("Levi Mod Menu registration failed");
        return false;
    }

    ModConfig snapshot;
    {
        std::lock_guard<std::mutex> lock(mConfigMutex);
        snapshot = mConfig;
    }
    if (snapshot.enabled && !PostProcessing::instance().start()) {
        unregisterMenu();
        getSelf().getLogger().error("Could not install the final-frame EGL hook");
        return false;
    }
    getSelf().getLogger().info("MC VHS PORT enabled");
    return true;
}

bool McVhsMod::disable() {
    PostProcessing::instance().stop();
    unregisterMenu();
    getSelf().getLogger().info("MC VHS PORT disabled");
    return true;
}

bool McVhsMod::unload() {
    PostProcessing::instance().stop();
    unregisterMenu();
    std::lock_guard<std::mutex> lock(mConfigMutex);
    mConfigFile.reset();
    getSelf().getLogger().info("MC VHS PORT unloaded");
    return true;
}

bool McVhsMod::registerMenu() {
    if (mMenuRegistered) return true;

    ModConfig config;
    {
        std::lock_guard<std::mutex> lock(mConfigMutex);
        config = mConfig;
    }

    auto module = pl::modmenu::ModuleBuilder("mc_vhs_port", "MC VHS PORT")
        .description("Screen-space effects ported from the MC VHS Java shader. Unsupported world/material stages are not exposed.")
        .modId(getSelf().getId())
        .defaultEnabled(config.enabled)
        .config("PROFILE", "Profile (0 Default, 1 DeepFried)", pl::modmenu::ConfigType::SliderInt,
                intText(config.PROFILE), "0", "1")
        .config("Optimize", "Optimize", pl::modmenu::ConfigType::Toggle, boolText(config.Optimize))
        .config("ASPECT_RATIO", "4:3 Aspect Ratio", pl::modmenu::ConfigType::Toggle, boolText(config.ASPECT_RATIO))
        .config("USE_LENS_DISTORTION", "Lens Distortion", pl::modmenu::ConfigType::Toggle, boolText(config.USE_LENS_DISTORTION))
        .config("LENS_DISTORTION_STRENGTH", "Lens Distortion Strength", pl::modmenu::ConfigType::SliderFloat,
                floatText(config.LENS_DISTORTION_STRENGTH), "0.01", "0.05", "USE_LENS_DISTORTION")
        .config("USE_STATIC", "Static Noise", pl::modmenu::ConfigType::Toggle, boolText(config.USE_STATIC))
        .config("STATIC_STRENGTH", "Static Strength", pl::modmenu::ConfigType::SliderFloat,
                floatText(config.STATIC_STRENGTH), "0.1", "1.0", "USE_STATIC")
        .config("AUTO_EXPOSURE", "Auto Exposure", pl::modmenu::ConfigType::Toggle, boolText(config.AUTO_EXPOSURE))
        .config("MANUAL_EXPOSURE", "Manual Exposure", pl::modmenu::ConfigType::SliderFloat,
                floatText(config.MANUAL_EXPOSURE), "0.1", "1.0")
        .config("TONEMAPPING_METHOD", "Tonemapping (0 None, 1 Tanh, 2 Beer, 3 Reinhard)", pl::modmenu::ConfigType::SliderInt,
                intText(config.TONEMAPPING_METHOD), "0", "3")
        .config("COMPRESSION_METHOD", "Compression (0 YUV, 1 YIQ, 2 YCbCr)", pl::modmenu::ConfigType::SliderInt,
                intText(config.COMPRESSION_METHOD), "0", "2")
        .config("LUMA_COMPRESSION", "Luma Compression LOD", pl::modmenu::ConfigType::SliderInt,
                intText(config.LUMA_COMPRESSION), "1", "8")
        .config("CHROMA_COMPRESSION", "Chroma Compression LOD", pl::modmenu::ConfigType::SliderInt,
                intText(config.CHROMA_COMPRESSION), "1", "8")
        .config("CHROMA_BOOST", "Chroma Boost", pl::modmenu::ConfigType::SliderFloat,
                floatText(config.CHROMA_BOOST), "0.0", "2.0")
        .config("EXTRA_ZOOM", "Extra Zoom", pl::modmenu::ConfigType::SliderFloat,
                floatText(config.EXTRA_ZOOM), "0.0", "2.0")
        .config("USE_SHARPNESS", "Sharpness", pl::modmenu::ConfigType::Toggle, boolText(config.USE_SHARPNESS))
        .config("SHARPNESS_STRENGTH", "Sharpness Strength", pl::modmenu::ConfigType::SliderInt,
                intText(config.SHARPNESS_STRENGTH), "1", "10", "USE_SHARPNESS")
        .config("SHARPNESS_RADIUS", "Sharpness Radius / Mip LOD", pl::modmenu::ConfigType::SliderFloat,
                floatText(config.SHARPNESS_RADIUS), "1.0", "8.0", "USE_SHARPNESS")
        .config("SIDE_JITTER", "Side Jitter", pl::modmenu::ConfigType::Toggle, boolText(config.SIDE_JITTER))
        .config("SIDE_JITTER_AMMOUNT", "Side Jitter Amount", pl::modmenu::ConfigType::SliderFloat,
                floatText(config.SIDE_JITTER_AMMOUNT), "0.001", "0.01", "SIDE_JITTER")
        .onToggle([this](std::string_view, bool enabled) { handleToggle(enabled); })
        .onConfigChanged([this](std::string_view, std::string_view key, std::string_view value) {
            handleConfigChanged(key, value);
        });

    if (!module.registerModule()) return false;
    mMenuRegistered = true;
    return true;
}

void McVhsMod::unregisterMenu() {
    if (!mMenuRegistered) return;
    pl::modmenu::unregisterModule("mc_vhs_port");
    mMenuRegistered = false;
}

void McVhsMod::saveConfigLocked() {
    if (!mConfigFile) return;
    mConfigFile->value() = mConfig;
    if (!mConfigFile->save()) getSelf().getLogger().warn("Failed to save MC VHS PORT config");
}

void McVhsMod::handleToggle(bool enabled) {
    ModConfig snapshot;
    {
        std::lock_guard<std::mutex> lock(mConfigMutex);
        mConfig.enabled = enabled;
        saveConfigLocked();
        snapshot = mConfig;
    }
    PostProcessing::instance().setConfig(snapshot);
    if (enabled) {
        if (!PostProcessing::instance().start()) getSelf().getLogger().error("Could not install EGL hook after enabling effects");
    } else {
        PostProcessing::instance().stop();
    }
}

void McVhsMod::handleConfigChanged(std::string_view key, std::string_view value) {
    bool valid = true;
    bool enabledChanged = false;
    bool enabledValue = false;
    ModConfig snapshot;
    {
        std::lock_guard<std::mutex> lock(mConfigMutex);
        if (key == "PROFILE") {
            int profile = mConfig.PROFILE;
            valid = parseInt(value, profile);
            if (valid) mConfig = applyProfile(mConfig, profile);
        } else if (key == "Optimize") valid = parseBool(value, mConfig.Optimize);
        else if (key == "USE_STATIC") valid = parseBool(value, mConfig.USE_STATIC);
        else if (key == "STATIC_STRENGTH") valid = parseFloat(value, mConfig.STATIC_STRENGTH);
        else if (key == "USE_LENS_DISTORTION") valid = parseBool(value, mConfig.USE_LENS_DISTORTION);
        else if (key == "LENS_DISTORTION_STRENGTH") valid = parseFloat(value, mConfig.LENS_DISTORTION_STRENGTH);
        else if (key == "ASPECT_RATIO") valid = parseBool(value, mConfig.ASPECT_RATIO);
        else if (key == "AUTO_EXPOSURE") valid = parseBool(value, mConfig.AUTO_EXPOSURE);
        else if (key == "MANUAL_EXPOSURE") valid = parseFloat(value, mConfig.MANUAL_EXPOSURE);
        else if (key == "TONEMAPPING_METHOD") valid = parseInt(value, mConfig.TONEMAPPING_METHOD);
        else if (key == "COMPRESSION_METHOD") valid = parseInt(value, mConfig.COMPRESSION_METHOD);
        else if (key == "LUMA_COMPRESSION") valid = parseInt(value, mConfig.LUMA_COMPRESSION);
        else if (key == "CHROMA_COMPRESSION") valid = parseInt(value, mConfig.CHROMA_COMPRESSION);
        else if (key == "CHROMA_BOOST") valid = parseFloat(value, mConfig.CHROMA_BOOST);
        else if (key == "EXTRA_ZOOM") valid = parseFloat(value, mConfig.EXTRA_ZOOM);
        else if (key == "USE_SHARPNESS") valid = parseBool(value, mConfig.USE_SHARPNESS);
        else if (key == "SHARPNESS_STRENGTH") valid = parseInt(value, mConfig.SHARPNESS_STRENGTH);
        else if (key == "SHARPNESS_RADIUS") valid = parseFloat(value, mConfig.SHARPNESS_RADIUS);
        else if (key == "SIDE_JITTER") valid = parseBool(value, mConfig.SIDE_JITTER);
        else if (key == "SIDE_JITTER_AMMOUNT") valid = parseFloat(value, mConfig.SIDE_JITTER_AMMOUNT);
        else if (key == "enabled") {
            valid = parseBool(value, mConfig.enabled);
            enabledChanged = valid;
            enabledValue = mConfig.enabled;
        } else valid = false;

        if (valid) {
            normalizeConfig(mConfig);
            saveConfigLocked();
            snapshot = mConfig;
        }
    }

    if (!valid) {
        getSelf().getLogger().warn("Ignored invalid MC VHS setting: {}", std::string(key));
        return;
    }
    PostProcessing::instance().setConfig(snapshot);
    if (enabledChanged) {
        if (enabledValue) PostProcessing::instance().start();
        else PostProcessing::instance().stop();
    }
}

} // namespace mcvhs
