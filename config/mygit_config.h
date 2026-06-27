#pragma once

#include <filesystem>
#include <string>

namespace mygit::config {

struct MygitConfig {
    std::string model_path;
    int gpu_layers = 0;
};

std::filesystem::path get_config_dir();
std::filesystem::path get_config_path();

// Loads ~/.mygit/config.json. Throws std::runtime_error if missing or corrupt.
MygitConfig load_config();

void save_config(const MygitConfig& cfg);

}  // namespace mygit::config
