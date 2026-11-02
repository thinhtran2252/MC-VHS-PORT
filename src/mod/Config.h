#pragma once

#include <string_view>
#include <pl/Config.hpp>

namespace mcvhs {

// Values follow the active Java shader options in mcvhs/shaders.properties.
// Interlacing and USE_MORE_STATIC are intentionally omitted until their source
// inputs can be supplied faithfully to a final-frame native pass.
struct ModConfig {
    int version = 5;
    bool enabled = true;
    int PROFILE = 0; // 0 = Default, 1 = DeepFried
    bool Optimize = false; // Additional shader-side approximations and fast paths

    // Flashlight uses a GLES depth-clear hook and a late final-frame pass.
    // Dev Logs renders a compact diagnostics overlay in the top-left corner.
    bool USE_FLASHLIGHT = false;
    float FLASHLIGHT_BRIGHTNESS = 2.0f;
    float FLASHLIGHT_DISTANCE = 4.0f;
    bool DEV_LOGS = false;

    bool USE_STATIC = true;
    float STATIC_STRENGTH = 0.2f;
    bool USE_LENS_DISTORTION = true;
    float LENS_DISTORTION_STRENGTH = 0.01f;
    bool ASPECT_RATIO = true;
    bool AUTO_EXPOSURE = true;
    float MANUAL_EXPOSURE = 0.7f;

    int TONEMAPPING_METHOD = 1; // 0 none, 1 tanh, 2 Beer/exponential, 3 Reinhard
    int COMPRESSION_METHOD = 0; // 0 YUV, 1 YIQ, 2 YCbCr
    int LUMA_COMPRESSION = 2;
    int CHROMA_COMPRESSION = 5;
    float CHROMA_BOOST = 0.0f;

    float EXTRA_ZOOM = 0.0f;
    bool USE_SHARPNESS = true;
    int SHARPNESS_STRENGTH = 2;
    float SHARPNESS_RADIUS = 2.0f;
    bool SIDE_JITTER = false;
    float SIDE_JITTER_AMMOUNT = 0.001f;
};

ModConfig applyProfile(ModConfig config, int profile);
void normalizeConfig(ModConfig& config);
nlohmann::json makeDefaultConfigJson();
nlohmann::json makeConfigSchemaJson();

} // namespace mcvhs

template <> struct pl::config::Schema<mcvhs::ModConfig> {
    static constexpr std::string_view title = "MC VHS PORT";
    static constexpr std::string_view description = "Bedrock native port of screen-space effects from the MC VHS Java shader.";
    static constexpr FieldSchema field(std::string_view name) {
        if (name == "version") return {.title = "Config version", .readOnly = true};
        if (name == "enabled") return {.title = "Enable post-processing"};
        if (name == "PROFILE") return {.title = "Profile", .description = "0 = Default, 1 = DeepFried", .minimum = 0.0, .maximum = 1.0};
        if (name == "Optimize") return {.title = "Optimize", .description = "Fuse compatible full-screen passes and use faster shader approximations."};
        if (name == "USE_FLASHLIGHT") return {.title = "Flashlight", .description = "Depth-based screen-space spotlight. Requires a readable world depth buffer."};
        if (name == "FLASHLIGHT_BRIGHTNESS") return {.title = "Flashlight Brightness", .minimum = 0.1, .maximum = 6.0};
        if (name == "FLASHLIGHT_DISTANCE") return {.title = "Flashlight Falloff", .description = "Depth attenuation distance in the flashlight pass.", .minimum = 0.5, .maximum = 16.0};
        if (name == "DEV_LOGS") return {.title = "Dev Logs", .description = "Show render, depth-capture, shader and GL diagnostics at the top-left of the screen."};
        if (name == "USE_STATIC") return {.title = "Static Noise"};
        if (name == "STATIC_STRENGTH") return {.title = "Static Strength", .minimum = 0.1, .maximum = 1.0};
        if (name == "USE_LENS_DISTORTION") return {.title = "Lens Distortion"};
        if (name == "LENS_DISTORTION_STRENGTH") return {.title = "Lens Distortion Strength", .minimum = 0.01, .maximum = 0.05};
        if (name == "ASPECT_RATIO") return {.title = "4:3 Aspect Ratio"};
        if (name == "AUTO_EXPOSURE") return {.title = "Auto Exposure"};
        if (name == "MANUAL_EXPOSURE") return {.title = "Manual Exposure", .minimum = 0.1, .maximum = 1.0};
        if (name == "TONEMAPPING_METHOD") return {.title = "Tonemapping Method", .description = "0 None, 1 Tanh, 2 Beer, 3 Reinhard", .minimum = 0.0, .maximum = 3.0};
        if (name == "COMPRESSION_METHOD") return {.title = "Compression Method", .description = "0 YUV, 1 YIQ, 2 YCbCr", .minimum = 0.0, .maximum = 2.0};
        if (name == "LUMA_COMPRESSION") return {.title = "Luma Compression LOD", .minimum = 1.0, .maximum = 8.0};
        if (name == "CHROMA_COMPRESSION") return {.title = "Chroma Compression LOD", .minimum = 1.0, .maximum = 8.0};
        if (name == "CHROMA_BOOST") return {.title = "Chroma Boost", .minimum = 0.0, .maximum = 2.0};
        if (name == "EXTRA_ZOOM") return {.title = "Extra Zoom", .minimum = 0.0, .maximum = 2.0};
        if (name == "USE_SHARPNESS") return {.title = "Sharpness"};
        if (name == "SHARPNESS_STRENGTH") return {.title = "Sharpness Strength", .minimum = 1.0, .maximum = 10.0};
        if (name == "SHARPNESS_RADIUS") return {.title = "Sharpness Radius / Mip LOD", .minimum = 1.0, .maximum = 8.0};
        if (name == "SIDE_JITTER") return {.title = "Side Jitter"};
        if (name == "SIDE_JITTER_AMMOUNT") return {.title = "Side Jitter Amount", .minimum = 0.001, .maximum = 0.01};
        return {};
    }
};
