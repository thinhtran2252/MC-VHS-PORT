#pragma once

#include <string_view>

#include <pl/Config.hpp>

namespace mcvhs {

// These names intentionally mirror the corresponding OptiFine shader options.
// The config file is the Bedrock-side equivalent of shaders.properties.
struct ModConfig {
    int version = 1;
    bool enabled = true;

    int PROFILE = 0; // 0 = Default, 1 = DeepFried

    bool LENS_MODULE_ENABLED = true;
    bool COMPRESSION_MODULE_ENABLED = true;
    bool POSTPROCESSING_MODULE_ENABLED = true;
    bool USER_SUGGESTIONS_MODULE_ENABLED = true;

    bool ASPECT_RATIO = true;
    bool USE_LENS_DISTORTION = true;
    float LENS_DISTORTION_STRENGTH = 0.01f;

    bool USE_INTERLACING = true;
    float INTERLACING_STRENGTH = 10.0f;
    bool USE_STATIC = true;
    float STATIC_STRENGTH = 0.2f;
    bool USE_MORE_STATIC = true;
    bool SIDE_JITTER = false;
    float SIDE_JITTER_AMMOUNT = 0.001f;

    bool AUTO_EXPOSURE = true;
    float MANUAL_EXPOSURE = 0.7f;
    int TONEMAPPING_METHOD = 1; // 0 = none, 1 = tanh, 2 = exponential, 3 = Reinhard
    float EXTRA_ZOOM = 0.0f;
    bool USE_SHARPNESS = true;
    int SHARPNESS_STRENGTH = 2;
    float SHARPNESS_RADIUS = 2.0f;

    int COMPRESSION_METHOD = 0; // 0 = YUV, 1 = YIQ, 2 = YCbCr
    int LUMA_COMPRESSION = 2;
    int CHROMA_COMPRESSION = 5;
    float CHROMA_BOOST = 0.0f;
};

ModConfig applyProfile(ModConfig config, int profile);
void normalizeConfig(ModConfig &config);

nlohmann::json makeDefaultConfigJson();
nlohmann::json makeConfigSchemaJson();

} // namespace mcvhs

template <> struct pl::config::Schema<mcvhs::ModConfig> {
    static constexpr std::string_view title = "MC VHS PORT Configuration";
    static constexpr std::string_view description =
        "Bedrock port of the screen-space post-processing controls from MC VHS.";

    static constexpr FieldSchema field(std::string_view name) {
        if (name == "version")
            return {.title = "Config version", .readOnly = true};
        if (name == "enabled")
            return {.title = "Enable MC VHS PORT", .description = "Master switch for the post-processing pipeline."};
        if (name == "PROFILE")
            return {.title = "Profile", .description = "0 = Default, 1 = DeepFried.", .minimum = 0.0, .maximum = 1.0};

        if (name == "LENS_MODULE_ENABLED") return {.title = "Lens module"};
        if (name == "COMPRESSION_MODULE_ENABLED") return {.title = "Compression module"};
        if (name == "POSTPROCESSING_MODULE_ENABLED") return {.title = "Post Processing module"};
        if (name == "USER_SUGGESTIONS_MODULE_ENABLED") return {.title = "User Suggestions module"};

        if (name == "ASPECT_RATIO") return {.title = "4:3 Aspect Ratio"};
        if (name == "USE_LENS_DISTORTION") return {.title = "Lens Distortion"};
        if (name == "LENS_DISTORTION_STRENGTH") return {.title = "Lens Distortion Strength", .minimum = 0.0, .maximum = 0.05};

        if (name == "USE_INTERLACING") return {.title = "Interlacing"};
        if (name == "INTERLACING_STRENGTH") return {.title = "Interlacing Strength", .minimum = 5.0, .maximum = 40.0};
        if (name == "USE_STATIC") return {.title = "Static Noise"};
        if (name == "STATIC_STRENGTH") return {.title = "Static Strength", .minimum = 0.1, .maximum = 1.0};
        if (name == "USE_MORE_STATIC") return {.title = "White Noise Bursts"};
        if (name == "SIDE_JITTER") return {.title = "Side Jitter"};
        if (name == "SIDE_JITTER_AMMOUNT") return {.title = "Side Jitter Amount", .minimum = 0.001, .maximum = 0.01};

        if (name == "AUTO_EXPOSURE") return {.title = "Auto Exposure"};
        if (name == "MANUAL_EXPOSURE") return {.title = "Manual Exposure", .minimum = 0.1, .maximum = 1.0};
        if (name == "TONEMAPPING_METHOD") return {.title = "Tonemapping Method", .description = "0 = None, 1 = Tanh, 2 = Beer, 3 = Reinhard.", .minimum = 0.0, .maximum = 3.0};
        if (name == "EXTRA_ZOOM") return {.title = "Extra Zoom", .minimum = 0.0, .maximum = 2.0};
        if (name == "USE_SHARPNESS") return {.title = "Sharpness"};
        if (name == "SHARPNESS_STRENGTH") return {.title = "Sharpness Strength", .minimum = 1.0, .maximum = 10.0};
        if (name == "SHARPNESS_RADIUS") return {.title = "Sharpness Radius", .minimum = 1.0, .maximum = 8.0};

        if (name == "COMPRESSION_METHOD") return {.title = "Compression Method", .description = "0 = YUV, 1 = YIQ, 2 = YCbCr.", .minimum = 0.0, .maximum = 2.0};
        if (name == "LUMA_COMPRESSION") return {.title = "Luma Compression", .minimum = 1.0, .maximum = 8.0};
        if (name == "CHROMA_COMPRESSION") return {.title = "Chroma Compression", .minimum = 1.0, .maximum = 8.0};
        if (name == "CHROMA_BOOST") return {.title = "Chroma Boost", .minimum = 0.0, .maximum = 2.0};
        return {};
    }
};
