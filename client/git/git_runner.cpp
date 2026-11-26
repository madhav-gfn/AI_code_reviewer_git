#include "git/git_runner.h"

#include <cstdlib>
#include <vector>
#include <cstdio>
#include <memory>
#include <array>

#if defined(_WIN32)
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

namespace mygit::git {

namespace {
std::vector<std::string> parse_args(const std::string& args_str) {
    std::vector<std::string> args;
    std::string current_arg;
    bool in_double_quotes = false;
    bool in_single_quotes = false;
    bool escaped = false;

    for (char c : args_str) {
        if (escaped) {
            current_arg += c;
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"' && !in_single_quotes) {
            in_double_quotes = !in_double_quotes;
        } else if (c == '\'' && !in_double_quotes) {
            in_single_quotes = !in_single_quotes;
        } else if (c == ' ' && !in_double_quotes && !in_single_quotes) {
            if (!current_arg.empty()) {
                args.push_back(current_arg);
                current_arg.clear();
            }
        } else {
            current_arg += c;
        }
    }
    if (!current_arg.empty()) {
        args.push_back(current_arg);
    }
    return args;
}

#if !defined(_WIN32)
std::string run_capture_posix(const std::vector<char*>& args) {
    int link[2];
    if (pipe(link) == -1) return {};

    pid_t pid = fork();
    if (pid == -1) {
        close(link[0]);
        close(link[1]);
        return {};
    } else if (pid == 0) {
        dup2(link[1], STDOUT_FILENO);
        close(link[0]);
        close(link[1]);
        execvp("git", args.data());
        std::exit(127);
    } else {
        close(link[1]);
        std::string result;
        char buffer[256];
        int count;
        while ((count = read(link[0], buffer, sizeof(buffer))) > 0) {
            result.append(buffer, count);
        }
        close(link[0]);
        int status;
        waitpid(pid, &status, 0);
        return result;
    }
}
#endif

#if defined(_WIN32)
std::string run_capture_win32(const std::vector<std::string>& args) {
    // Escape arguments for Windows command line
    std::string cmd = "git";
    for (const auto& arg : args) {
        cmd += " \"";
        for (size_t i = 0; i < arg.size(); ++i) {
            char c = arg[i];
            if (c == '"') {
                cmd += "\\\"";
            } else if (c == '\\') {
                size_t j = i;
                while (j < arg.size() && arg[j] == '\\') j++;
                if (j == arg.size() || arg[j] == '"') {
                    // Double the backslashes if followed by a quote or at end of string
                    for (size_t k = i; k < j; ++k) cmd += "\\\\";
                } else {
                    for (size_t k = i; k < j; ++k) cmd += "\\";
                }
                i = j - 1;
            } else {
                cmd += c;
            }
        }
        cmd += "\"";
    }

    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;

    HANDLE hChildStd_OUT_Rd = NULL;
    HANDLE hChildStd_OUT_Wr = NULL;
    if (!CreatePipe(&hChildStd_OUT_Rd, &hChildStd_OUT_Wr, &saAttr, 0)) {
        return {};
    }
    if (!SetHandleInformation(hChildStd_OUT_Rd, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(hChildStd_OUT_Rd);
        CloseHandle(hChildStd_OUT_Wr);
        return {};
    }

    PROCESS_INFORMATION piProcInfo;
    ZeroMemory(&piProcInfo, sizeof(PROCESS_INFORMATION));
    STARTUPINFO siStartInfo;
    ZeroMemory(&siStartInfo, sizeof(STARTUPINFO));
    siStartInfo.cb = sizeof(STARTUPINFO);
    siStartInfo.hStdError = hChildStd_OUT_Wr;
    siStartInfo.hStdOutput = hChildStd_OUT_Wr;
    siStartInfo.dwFlags |= STARTF_USESTDHANDLES;

    BOOL bSuccess = CreateProcessA(NULL,
        const_cast<LPSTR>(cmd.c_str()),
        NULL,
        NULL,
        TRUE,
        0,
        NULL,
        NULL,
        &siStartInfo,
        &piProcInfo);

    if (!bSuccess) {
        CloseHandle(hChildStd_OUT_Rd);
        CloseHandle(hChildStd_OUT_Wr);
        return {};
    }

    CloseHandle(hChildStd_OUT_Wr);

    std::string result;
    DWORD dwRead;
    CHAR chBuf[256];
    while (ReadFile(hChildStd_OUT_Rd, chBuf, sizeof(chBuf), &dwRead, NULL) && dwRead != 0) {
        result.append(chBuf, dwRead);
    }

    CloseHandle(hChildStd_OUT_Rd);
    WaitForSingleObject(piProcInfo.hProcess, INFINITE);
    CloseHandle(piProcInfo.hProcess);
    CloseHandle(piProcInfo.hThread);
    return result;
}
#endif

}

int run_git(const std::string& args) {
    std::vector<std::string> parsed_args = parse_args(args);
    std::vector<char*> c_args;
    c_args.push_back(const_cast<char*>("git"));
    for (const auto& arg : parsed_args) {
        c_args.push_back(const_cast<char*>(arg.c_str()));
    }
    c_args.push_back(nullptr);

#if defined(_WIN32)
    return _spawnvp(_P_WAIT, "git", c_args.data());
#else
    pid_t pid = fork();
    if (pid == -1) {
        return -1;
    } else if (pid == 0) {
        execvp("git", c_args.data());
        std::exit(127);
    } else {
        int status;
        if (waitpid(pid, &status, 0) == -1) {
            return -1;
        }
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }
        return -1;
    }
#endif
}

std::string run_git_capture(const std::string& args) {
    std::vector<std::string> parsed_args = parse_args(args);

#if defined(_WIN32)
    std::string result = run_capture_win32(parsed_args);
#else
    std::vector<char*> c_args;
    c_args.push_back(const_cast<char*>("git"));
    for (const auto& arg : parsed_args) {
        c_args.push_back(const_cast<char*>(arg.c_str()));
    }
    c_args.push_back(nullptr);

    std::string result = run_capture_posix(c_args);
#endif

    // Trim trailing whitespace / newlines.
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' ')) {
        result.pop_back();
    }
    return result;
}

}  // namespace mygit::git
