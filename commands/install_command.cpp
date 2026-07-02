#include "commands/install_command.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#ifdef _WIN32
#   define WIN32_LEAN_AND_MEAN
#   define NOMINMAX
#   include <windows.h>
#else
#   include <fstream>
#endif

#include "config/mygit_config.h"

namespace mygit::commands {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Return the directory where we install the binary: ~/.mygit/bin
static fs::path install_dir() {
    return config::get_config_dir() / "bin";
}

/// Return the path of the currently-running executable.
static fs::path self_exe_path() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        throw std::runtime_error("GetModuleFileName failed");
    }
    return fs::path(buf);
#else
    // Linux: /proc/self/exe; macOS: not 100 % portable, but good enough.
    std::error_code ec;
    auto p = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return p;

    // macOS fallback via _NSGetExecutablePath would go here.
    // For now, fall back to argv[0]-style heuristic.
    throw std::runtime_error(
        "Cannot determine executable path on this platform");
#endif
}

// ---------------------------------------------------------------------------
// Windows PATH update via registry
// ---------------------------------------------------------------------------
#ifdef _WIN32

/// Read the current user PATH from the registry (HKCU\Environment).
static std::string read_user_path() {
    HKEY key{};
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Environment", 0,
                      KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return {};
    }

    char buf[8192]{};
    DWORD size = sizeof(buf);
    DWORD type{};
    const LONG rc = RegQueryValueExA(key, "Path", nullptr, &type,
                                     reinterpret_cast<BYTE*>(buf), &size);
    RegCloseKey(key);

    if (rc != ERROR_SUCCESS) return {};
    return std::string(buf, size > 0 ? size - 1 : 0);  // trim trailing NUL
}

/// Returns true if `dir` is already present in the semicolon-separated PATH.
static bool path_contains(const std::string& path_var, const fs::path& dir) {
    const std::string target = dir.string();
    std::string lower_target = target;
    std::string lower_path   = path_var;

    // Case-insensitive comparison for Windows paths.
    for (auto& c : lower_target) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    for (auto& c : lower_path)   c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));

    // Check each semicolon-delimited entry.
    std::string::size_type start = 0;
    while (start < lower_path.size()) {
        auto end = lower_path.find(';', start);
        if (end == std::string::npos) end = lower_path.size();

        std::string entry = lower_path.substr(start, end - start);
        // Trim trailing backslash for comparison.
        while (!entry.empty() && (entry.back() == '\\' || entry.back() == '/'))
            entry.pop_back();

        std::string cmp = lower_target;
        while (!cmp.empty() && (cmp.back() == '\\' || cmp.back() == '/'))
            cmp.pop_back();

        if (entry == cmp) return true;
        start = end + 1;
    }
    return false;
}

/// Append `dir` to the user PATH in the Windows registry.
static bool append_to_user_path(const fs::path& dir) {
    std::string current = read_user_path();

    if (path_contains(current, dir)) {
        std::cout << "  PATH already contains " << dir << "\n";
        return true;
    }

    // Append with semicolon separator.
    std::string updated = current;
    if (!updated.empty() && updated.back() != ';') updated += ';';
    updated += dir.string();

    HKEY key{};
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Environment", 0,
                      KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        std::cerr << "  Error: cannot open HKCU\\Environment for writing.\n";
        return false;
    }

    // Use REG_EXPAND_SZ so %VARIABLES% inside PATH keep working.
    const LONG rc = RegSetValueExA(
        key, "Path", 0, REG_EXPAND_SZ,
        reinterpret_cast<const BYTE*>(updated.c_str()),
        static_cast<DWORD>(updated.size() + 1));  // include NUL
    RegCloseKey(key);

    if (rc != ERROR_SUCCESS) {
        std::cerr << "  Error: RegSetValueEx failed (" << rc << ").\n";
        return false;
    }

    // Broadcast WM_SETTINGCHANGE so Explorer picks up the change immediately.
    DWORD_PTR result{};
    SendMessageTimeoutA(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                        reinterpret_cast<LPARAM>("Environment"),
                        SMTO_ABORTIFHUNG, 5000, &result);

    std::cout << "  Added " << dir << " to user PATH.\n";
    return true;
}

#else
// ---------------------------------------------------------------------------
// POSIX PATH update: append an export line to the user's shell rc file.
// ---------------------------------------------------------------------------

/// Pick the right shell rc file (~/.bashrc, ~/.zshrc, …).
static fs::path shell_rc_path() {
    const char* home = std::getenv("HOME");
    if (!home) throw std::runtime_error("HOME is not set");

    const fs::path home_dir{home};

    // Prefer the shell the user is actually running.
    if (const char* shell = std::getenv("SHELL")) {
        const std::string sh{shell};
        if (sh.find("zsh") != std::string::npos)  return home_dir / ".zshrc";
        if (sh.find("fish") != std::string::npos)  return home_dir / ".config" / "fish" / "config.fish";
    }
    return home_dir / ".bashrc";
}

/// The export line we append.
static std::string export_line(const fs::path& dir) {
    return "export PATH=\"" + dir.string() + ":$PATH\"";
}

/// Returns true if the export line already exists in the file.
static bool rc_file_contains(const fs::path& rc, const std::string& line) {
    if (!fs::exists(rc)) return false;
    std::ifstream f(rc);
    std::string buf;
    while (std::getline(f, buf)) {
        if (buf.find(line) != std::string::npos) return true;
    }
    return false;
}

static bool append_to_shell_rc(const fs::path& dir) {
    const fs::path rc = shell_rc_path();
    const std::string line = export_line(dir);

    if (rc_file_contains(rc, line)) {
        std::cout << "  PATH export already present in " << rc << "\n";
        return true;
    }

    std::ofstream f(rc, std::ios::app);
    if (!f) {
        std::cerr << "  Error: cannot open " << rc << " for appending.\n";
        return false;
    }
    f << "\n# Added by mygit install\n" << line << "\n";
    std::cout << "  Appended PATH export to " << rc << "\n";
    return true;
}

#endif  // _WIN32

// ---------------------------------------------------------------------------
// Main entry point
// ---------------------------------------------------------------------------

int run_install() {
    std::cout << "\n  mygit install\n"
              << "  " << std::string(36, '-') << "\n\n";

    try {
        // 1. Determine source and destination paths.
        const fs::path src = self_exe_path();
        const fs::path dst_dir = install_dir();

#ifdef _WIN32
        const fs::path dst = dst_dir / "mygit.exe";
#else
        const fs::path dst = dst_dir / "mygit";
#endif

        // 2. Create the destination directory.
        std::error_code ec;
        fs::create_directories(dst_dir, ec);
        if (ec) {
            std::cerr << "  Error creating directory " << dst_dir
                      << ": " << ec.message() << "\n";
            return 1;
        }

        // 3. Copy the executable (overwrite if it already exists).
        //    On Windows the running exe cannot be overwritten directly, but
        //    the destination is a *different* path, so this is fine.
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            std::cerr << "  Error copying executable: " << ec.message() << "\n";
            return 1;
        }
        std::cout << "  Copied " << src.filename() << " -> " << dst << "\n";

        // Also copy any sibling DLL files that the executable depends on
        // (e.g. llama.dll, ggml.dll, ggml-base.dll, ggml-cpu.dll).
        const fs::path src_dir = src.parent_path();
        for (const auto& entry : fs::directory_iterator(src_dir)) {
            if (!entry.is_regular_file()) continue;
            const auto ext = entry.path().extension().string();
            // Case-insensitive check for .dll
            if (ext == ".dll" || ext == ".DLL") {
                const fs::path dll_dst = dst_dir / entry.path().filename();
                fs::copy_file(entry.path(), dll_dst,
                              fs::copy_options::overwrite_existing, ec);
                if (ec) {
                    std::cerr << "  Warning: could not copy "
                              << entry.path().filename() << ": "
                              << ec.message() << "\n";
                    ec.clear();
                } else {
                    std::cout << "  Copied " << entry.path().filename()
                              << " -> " << dll_dst << "\n";
                }
            }
        }

#ifndef _WIN32
        // Make sure the installed binary is executable.
        fs::permissions(dst, fs::perms::owner_exec | fs::perms::group_exec |
                                 fs::perms::others_exec,
                        fs::perm_options::add, ec);
#endif

        // 4. Add the install directory to the user's PATH.
        bool path_ok{};
#ifdef _WIN32
        path_ok = append_to_user_path(dst_dir);
#else
        path_ok = append_to_shell_rc(dst_dir);
#endif

        if (!path_ok) {
            std::cerr << "\n  Warning: could not update PATH automatically.\n"
                      << "  Please add " << dst_dir << " to your PATH manually.\n\n";
        }

        // 5. Print instructions.
        std::cout << "\n  Done!  Please restart your terminal (or open a new one)\n"
                  << "  so the updated PATH takes effect.  Then run:\n\n"
                  << "    mygit review\n\n"
                  << "  from any Git repository.\n\n";
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "  Error: " << e.what() << "\n";
        return 1;
    }
}

}  // namespace mygit::commands
