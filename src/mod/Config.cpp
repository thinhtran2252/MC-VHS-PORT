#include "mod/Config.h"
#include <algorithm>
#include <cmath>

namespace mcvhs {

ModConfig applyProfile(ModConfig c, int profile) {
    c.PROFILE = std::clamp(profile, 0, 1);
    c.USE_STATIC = true;
    c.STATIC_STRENGTH = 0.2f;
    c.USE_LENS_DISTORTION = true;
    c.LENS_DISTORTION_STRENGTH = 0.01f;
    c.ASPECT_RATIO = true;
    c.AUTO_EXPOSURE = true;
    c.MANUAL_EXPOSURE = 0.7f;
    c.COMPRESSION_METHOD = 0;
    c.CHROMA_BOOST = 0.0f;
    c.USE_SHARPNESS = true;
    c.EXTRA_ZOOM = 0.0f;
    c.SIDE_JITTER = false;
    c.SIDE_JITTER_AMMOUNT = 0.001f;
    if (c.PROFILE == 0) {
        c.LUMA_COMPRESSION = 2;
        c.CHROMA_COMPRESSION = 5;
        c.SHARPNESS_STRENGTH = 2;
        c.SHARPNESS_RADIUS = 2.0f;
        c.TONEMAPPING_METHOD = 1;
    } else {
        c.LUMA_COMPRESSION = 1;
        c.CHROMA_COMPRESSION = 1;
        c.SHARPNESS_STRENGTH = 10;
        c.SHARPNESS_RADIUS = 4.0f;
        c.TONEMAPPING_METHOD = 0;
    }
    // Preserve independent runtime toggles when changing visual profiles.
    return c;
}

void normalizeConfig(ModConfig& c) {
    c.version = 5;
    c.PROFILE = std::clamp(c.PROFILE, 0, 1);
    auto finiteClamp = [](float v, float fallback, float lo, float hi) {
        return std::clamp(std::isfinite(v) ? v : fallback, lo, hi);
    };
    c.STATIC_STRENGTH = finiteClamp(c.STATIC_STRENGTH, 0.2f, 0.1f, 1.0f);
    c.LENS_DISTORTION_STRENGTH = finiteClamp(c.LENS_DISTORTION_STRENGTH, 0.01f, 0.01f, 0.05f);
    c.MANUAL_EXPOSURE = finiteClamp(c.MANUAL_EXPOSURE, 0.7f, 0.1f, 1.0f);
    c.TONEMAPPING_METHOD = std::clamp(c.TONEMAPPING_METHOD, 0, 3);
    c.COMPRESSION_METHOD = std::clamp(c.COMPRESSION_METHOD, 0, 2);
    c.LUMA_COMPRESSION = std::clamp(c.LUMA_COMPRESSION, 1, 8);
    c.CHROMA_COMPRESSION = std::clamp(c.CHROMA_COMPRESSION, 1, 8);
    c.CHROMA_BOOST = finiteClamp(c.CHROMA_BOOST, 0.0f, 0.0f, 2.0f);
    c.EXTRA_ZOOM = finiteClamp(c.EXTRA_ZOOM, 0.0f, 0.0f, 2.0f);
    c.SHARPNESS_STRENGTH = std::clamp(c.SHARPNESS_STRENGTH, 1, 10);
    c.SHARPNESS_RADIUS = finiteClamp(c.SHARPNESS_RADIUS, 2.0f, 1.0f, 8.0f);
    c.SIDE_JITTER_AMMOUNT = finiteClamp(c.SIDE_JITTER_AMMOUNT, 0.001f, 0.001f, 0.01f);
    c.FLASHLIGHT_BRIGHTNESS = finiteClamp(c.FLASHLIGHT_BRIGHTNESS, 2.0f, 0.1f, 6.0f);
    c.FLASHLIGHT_DISTANCE = finiteClamp(c.FLASHLIGHT_DISTANCE, 4.0f, 0.5f, 16.0f);
}

nlohmann::json makeDefaultConfigJson() { return pl::config::defaultJson(ModConfig{}); }
nlohmann::json makeConfigSchemaJson() { return pl::config::schema(ModConfig{}); }

} // namespace mcvhs
