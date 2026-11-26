#include "config/mygit_config.h"

#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace mygit::config {

namespace fs = std::filesystem;

fs::path get_config_dir() {
#if defined(_WIN32)
    if (const char* up = std::getenv("USERPROFILE")) return fs::path(up) / ".mygit";
#endif
    if (const char* home = std::getenv("HOME")) return fs::path(home) / ".mygit";
    return fs::current_path() / ".mygit";  // last resort
}

fs::path get_config_path() {
    return get_config_dir() / "config.json";
}

MygitConfig load_config() {
    const fs::path path = get_config_path();
    if (!fs::exists(path)) {
        throw std::runtime_error(
            "No config found. Run 'mygit setup' to configure your model.");
    }

    std::ifstream f(path);
    if (!f) throw std::runtime_error("Could not read config: " + path.string());

    nlohmann::json j;
    try { f >> j; }
    catch (const nlohmann::json::exception& e) {
        throw std::runtime_error(std::string("Corrupt config JSON: ") + e.what());
    }

    MygitConfig cfg;
    cfg.model_path = j.value("model_path", "");
    cfg.gpu_layers  = j.value("gpu_layers", 0);

    if (cfg.model_path.empty()) {
        throw std::runtime_error(
            "Config is missing model_path. Run 'mygit setup' to fix it.");
    }
    return cfg;
}

void save_config(const MygitConfig& cfg) {
    const fs::path dir  = get_config_dir();
    const fs::path path = dir / "config.json";

    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) throw std::runtime_error("Could not create config dir: " + ec.message());

    nlohmann::json j;
    j["model_path"] = cfg.model_path;
    j["gpu_layers"] = cfg.gpu_layers;

    std::ofstream f(path);
    if (!f) throw std::runtime_error("Could not write config: " + path.string());
    f << j.dump(2) << "\n";
}

}  // namespace mygit::config
