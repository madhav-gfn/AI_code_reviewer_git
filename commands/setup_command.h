#pragma once

namespace mygit::commands {

// Runs `mygit setup`: interactive first-time configuration wizard.
// Writes model path and GPU layer count to ~/.mygit/config.json.
// Returns 0 on success, 1 on failure.
int run_setup();

}  // namespace mygit::commands
