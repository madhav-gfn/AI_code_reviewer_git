#include "commands/setup_command.h"

#include <filesystem>
#include <iostream>
#include <string>

#include "config/mygit_config.h"

namespace mygit::commands {

namespace fs = std::filesystem;

int run_setup() {
    std::cout << "\n  mygit setup\n"
              << "  " << std::string(36, '-') << "\n\n";

    // Load existing config to show current values as defaults.
    config::MygitConfig current;
    try { current = config::load_config(); } catch (...) {}

    const fs::path config_path = config::get_config_path();
    std::cout << "  Config: " << config_path << "\n\n";

    // --- Model path ---
    std::string model_path;
    while (true) {
        if (!current.model_path.empty()) {
            std::cout << "  Model path [" << current.model_path << "]: ";
        } else {
            std::cout << "  Model path (full path to your .gguf file): ";
        }
        std::getline(std::cin, model_path);
        if (model_path.empty()) model_path = current.model_path;

        if (model_path.empty()) {
            std::cout << "  Path cannot be empty.\n";
            continue;
        }

        if (!fs::exists(model_path)) {
            std::cout << "  Warning: file not found at that path.\n"
                      << "  Save anyway? [y/N]: ";
            std::string confirm;
            std::getline(std::cin, confirm);
            if (confirm != "y" && confirm != "Y") continue;
        }
        break;
    }

    // --- GPU layers ---
    std::cout << "  GPU layers (0 = CPU only) [" << current.gpu_layers << "]: ";
    std::string gpu_input;
    std::getline(std::cin, gpu_input);
    int gpu_layers = current.gpu_layers;
    if (!gpu_input.empty()) {
        try { gpu_layers = std::stoi(gpu_input); } catch (...) {}
    }

    // --- Save ---
    config::MygitConfig cfg;
    cfg.model_path = model_path;
    cfg.gpu_layers  = gpu_layers;
    try {
        config::save_config(cfg);
    } catch (const std::exception& e) {
        std::cerr << "\n  Error saving config: " << e.what() << "\n";
        return 1;
    }

    std::cout << "\n  Saved. Run 'mygit review' to test your setup.\n\n";
    return 0;
}

}  // namespace mygit::commands
