#include "mod/Config.h"
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
bool writeJson(const std::filesystem::path& path, const nlohmann::json& value) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) { std::cerr << "Cannot create config directory: " << ec.message() << '\n'; return false; }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) { std::cerr << "Cannot write " << path << '\n'; return false; }
    out << value.dump(2) << '\n';
    return out.good();
}
}
int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "Usage: levi_config_generator <output-dir>\n"; return 2; }
    const std::filesystem::path dir(argv[1]);
    if (!writeJson(dir / "config.json", mcvhs::makeDefaultConfigJson())) return 1;
    if (!writeJson(dir / "config.schema.json", mcvhs::makeConfigSchemaJson())) return 1;
    return 0;
}
