#include "mod/Config.h"

#include <algorithm>
#include <cmath>

namespace mcvhs {

ModConfig applyProfile(ModConfig config, int profile) {
    config.PROFILE = std::clamp(profile, 0, 1);

    // Values are taken from the Default and DeepFried profiles in the Java
    // source shaders.properties file, for the effects implemented in this port.
    config.USE_LENS_DISTORTION = true;
    config.LENS_DISTORTION_STRENGTH = 0.01f;
    config.AUTO_EXPOSURE = true;
    config.MANUAL_EXPOSURE = 0.7f;
    config.USE_STATIC = true;
    config.STATIC_STRENGTH = 0.2f;
    config.ASPECT_RATIO = true;
    config.USE_INTERLACING = true;
    config.COMPRESSION_METHOD = 0;
    config.CHROMA_BOOST = 0.0f;
    config.USE_SHARPNESS = true;
    config.EXTRA_ZOOM = 0.0f;
    config.SIDE_JITTER = false;
    config.SIDE_JITTER_AMMOUNT = 0.001f;

    if (config.PROFILE == 0) {
        config.INTERLACING_STRENGTH = 10.0f;
        config.LUMA_COMPRESSION = 2;
        config.CHROMA_COMPRESSION = 5;
        config.SHARPNESS_STRENGTH = 2;
        config.SHARPNESS_RADIUS = 2.0f;
        config.TONEMAPPING_METHOD = 1;
        config.USE_MORE_STATIC = true;
    } else {
        config.INTERLACING_STRENGTH = 20.0f;
        config.LUMA_COMPRESSION = 1;
        config.CHROMA_COMPRESSION = 1;
        config.SHARPNESS_STRENGTH = 10;
        config.SHARPNESS_RADIUS = 4.0f;
        config.TONEMAPPING_METHOD = 0;
        config.USE_MORE_STATIC = false;
    }

    return config;
}

void normalizeConfig(ModConfig &c) {
    c.PROFILE = std::clamp(c.PROFILE, 0, 1);
    c.LENS_DISTORTION_STRENGTH = std::clamp(std::isfinite(c.LENS_DISTORTION_STRENGTH) ? c.LENS_DISTORTION_STRENGTH : 0.01f, 0.0f, 0.05f);
    c.INTERLACING_STRENGTH = std::clamp(std::isfinite(c.INTERLACING_STRENGTH) ? c.INTERLACING_STRENGTH : 10.0f, 5.0f, 40.0f);
    c.STATIC_STRENGTH = std::clamp(std::isfinite(c.STATIC_STRENGTH) ? c.STATIC_STRENGTH : 0.2f, 0.1f, 1.0f);
    c.SIDE_JITTER_AMMOUNT = std::clamp(std::isfinite(c.SIDE_JITTER_AMMOUNT) ? c.SIDE_JITTER_AMMOUNT : 0.001f, 0.001f, 0.01f);
    c.MANUAL_EXPOSURE = std::clamp(std::isfinite(c.MANUAL_EXPOSURE) ? c.MANUAL_EXPOSURE : 0.7f, 0.1f, 1.0f);
    c.TONEMAPPING_METHOD = std::clamp(c.TONEMAPPING_METHOD, 0, 3);
    c.EXTRA_ZOOM = std::clamp(std::isfinite(c.EXTRA_ZOOM) ? c.EXTRA_ZOOM : 0.0f, 0.0f, 2.0f);
    c.SHARPNESS_STRENGTH = std::clamp(c.SHARPNESS_STRENGTH, 1, 10);
    c.SHARPNESS_RADIUS = std::clamp(std::isfinite(c.SHARPNESS_RADIUS) ? c.SHARPNESS_RADIUS : 2.0f, 1.0f, 8.0f);
    c.COMPRESSION_METHOD = std::clamp(c.COMPRESSION_METHOD, 0, 2);
    c.LUMA_COMPRESSION = std::clamp(c.LUMA_COMPRESSION, 1, 8);
    c.CHROMA_COMPRESSION = std::clamp(c.CHROMA_COMPRESSION, 1, 8);
    c.CHROMA_BOOST = std::clamp(std::isfinite(c.CHROMA_BOOST) ? c.CHROMA_BOOST : 0.0f, 0.0f, 2.0f);
}

nlohmann::json makeDefaultConfigJson() {
    return pl::config::defaultJson(ModConfig{});
}

nlohmann::json makeConfigSchemaJson() {
    return pl::config::schema(ModConfig{});
}

} // namespace mcvhs
