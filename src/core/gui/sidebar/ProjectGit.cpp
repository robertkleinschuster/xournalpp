#include "ProjectGit.h"

#include <cstdlib>
#include <optional>
#include <thread>
#include <utility>

#include <glib.h>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace fs = std::filesystem;

namespace {

auto utf8(const fs::path& path) -> std::string {
    auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

auto pathFromUtf8(const std::string& value) -> fs::path {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}

auto findGit() -> const std::optional<std::string>& {
    static const std::optional<std::string> git = []() -> std::optional<std::string> {
        // Never block on a terminal credential prompt; there is no terminal
        g_setenv("GIT_TERMINAL_PROMPT", "0", true);
        if (gchar* found = g_find_program_in_path("git")) {
            std::string result(found);
            g_free(found);
            return result;
        }
#ifdef _WIN32
        for (const char* variable: {"ProgramFiles", "ProgramW6432", "LOCALAPPDATA"}) {
            if (const char* base = g_getenv(variable)) {
                const auto candidate = pathFromUtf8(base) /
                                       (std::string(variable) == "LOCALAPPDATA" ? "Programs/Git/cmd/git.exe" :
                                                                                  "Git/cmd/git.exe");
                std::error_code error;
                if (fs::is_regular_file(candidate, error)) {
                    return utf8(candidate);
                }
            }
        }
#endif
        return std::nullopt;
    }();
    return git;
}

#ifdef _WIN32
auto widen(const std::string& value) -> std::wstring {
    gunichar2* converted = g_utf8_to_utf16(value.c_str(), -1, nullptr, nullptr, nullptr);
    std::wstring result(converted ? reinterpret_cast<wchar_t*>(converted) : L"");
    g_free(converted);
    return result;
}

/// Quotes an argument following the rules of CommandLineToArgvW
auto quoteArgument(const std::wstring& arg) -> std::wstring {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring quoted = L"\"";
    for (auto it = arg.begin();; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            quoted.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
        } else {
            quoted.append(backslashes, L'\\');
        }
        quoted.push_back(*it);
    }
    quoted.push_back(L'"');
    return quoted;
}

auto readAll(HANDLE handle) -> std::string {
    std::string result;
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(handle, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        result.append(buffer, read);
    }
    return result;
}

auto runSync(const std::string& git, const fs::path& dir, const std::vector<std::string>& args)
        -> ProjectGit::CommandResult {
    ProjectGit::CommandResult result;
    std::wstring commandLine = quoteArgument(widen(git));
    for (const auto& arg: args) {
        commandLine += L" " + quoteArgument(widen(arg));
    }

    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr;
    if (!CreatePipe(&outRead, &outWrite, &attributes, 0) || !CreatePipe(&errRead, &errWrite, &attributes, 0)) {
        result.error = "Could not create pipes for git";
        return result;
    }
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input;
    startup.hStdOutput = outWrite;
    startup.hStdError = errWrite;
    PROCESS_INFORMATION process{};
    const auto workingDirectory = dir.wstring();
    const bool started = CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                        nullptr, workingDirectory.c_str(), &startup, &process);
    CloseHandle(outWrite);
    CloseHandle(errWrite);
    if (input != INVALID_HANDLE_VALUE) {
        CloseHandle(input);
    }
    if (!started) {
        CloseHandle(outRead);
        CloseHandle(errRead);
        result.error = "Could not start git";
        return result;
    }

    std::thread errorReader([&] { result.error = readAll(errRead); });
    result.output = readAll(outRead);
    errorReader.join();
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    result.success = exitCode == 0;
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    CloseHandle(outRead);
    CloseHandle(errRead);
    return result;
}
#else
auto runSync(const std::string& git, const fs::path& dir, const std::vector<std::string>& args)
        -> ProjectGit::CommandResult {
    ProjectGit::CommandResult result;
    std::vector<gchar*> argv{const_cast<gchar*>(git.c_str())};
    for (const auto& arg: args) {
        argv.push_back(const_cast<gchar*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    gchar* out = nullptr;
    gchar* err = nullptr;
    gint status = 1;
    GError* error = nullptr;
    const auto workingDirectory = dir.string();
    if (g_spawn_sync(workingDirectory.c_str(), argv.data(), nullptr, G_SPAWN_DEFAULT, nullptr, nullptr, &out, &err,
                     &status, &error)) {
        result.success = status == 0;
        result.output = out ? out : "";
        result.error = err ? err : "";
    } else {
        result.error = error->message;
        g_error_free(error);
    }
    g_free(out);
    g_free(err);
    return result;
}
#endif

struct PendingResult {
    std::weak_ptr<void> owner;
    std::function<void(ProjectGit::CommandResult)> callback;
    ProjectGit::CommandResult result;
};

auto parseCount(const std::string& text, const std::string& label) -> size_t {
    const auto pos = text.find(label);
    if (pos == std::string::npos) {
        return 0;
    }
    return static_cast<size_t>(std::strtoul(text.c_str() + pos + label.size(), nullptr, 10));
}

}  // namespace

bool ProjectGit::isAvailable() { return findGit().has_value(); }

void ProjectGit::run(const fs::path& dir, std::vector<std::string> args, std::weak_ptr<void> owner,
                     std::function<void(CommandResult)> callback) {
    auto* pending = new PendingResult{std::move(owner), std::move(callback), {}};
    const auto& git = findGit();
    if (!git) {
        pending->result.error = "Git was not found";
        g_idle_add(+[](gpointer data) -> gboolean {
            std::unique_ptr<PendingResult> p(static_cast<PendingResult*>(data));
            if (!p->owner.expired()) {
                p->callback(std::move(p->result));
            }
            return G_SOURCE_REMOVE;
        }, pending);
        return;
    }
    std::thread([pending, git = *git, dir, args = std::move(args)] {
        pending->result = runSync(git, dir, args);
        g_idle_add(+[](gpointer data) -> gboolean {
            std::unique_ptr<PendingResult> p(static_cast<PendingResult*>(data));
            if (!p->owner.expired()) {
                p->callback(std::move(p->result));
            }
            return G_SOURCE_REMOVE;
        }, pending);
    }).detach();
}

auto ProjectGit::parseStatus(const std::string& output, const fs::path& toplevel) -> Status {
    Status status;
    size_t pos = 0;
    auto next = [&]() -> std::optional<std::string> {
        if (pos >= output.size()) {
            return std::nullopt;
        }
        auto end = output.find('\0', pos);
        if (end == std::string::npos) {
            end = output.size();
        }
        auto entry = output.substr(pos, end - pos);
        pos = end + 1;
        return entry;
    };
    while (auto entry = next()) {
        if (entry->rfind("## ", 0) == 0) {
            // e.g. "main...origin/main [ahead 1, behind 2]"
            status.hasUpstream = entry->find("...") != std::string::npos;
            status.ahead = parseCount(*entry, "ahead ");
            status.behind = parseCount(*entry, "behind ");
            continue;
        }
        if (entry->size() < 4) {
            continue;
        }
        const char index = (*entry)[0];
        const char worktree = (*entry)[1];
        if (index == 'R' || index == 'C') {
            next();  // skip the original path of a rename or copy
        }
        status.changedFiles++;
        if (index == 'D' || worktree == 'D') {
            continue;  // deleted files have no row in the tree
        }
        const auto state = index == '?' || index == 'A' ? FileState::Added : FileState::Modified;
        status.files[pathKey(toplevel / pathFromUtf8(entry->substr(3)))] = state;
    }
    return status;
}

auto ProjectGit::pathKey(const fs::path& path) -> std::string {
    auto normalized = path.lexically_normal();
    normalized.make_preferred();
    auto key = utf8(normalized);
    while (key.size() > 1 && (key.back() == '/' || key.back() == '\\')) {
        key.pop_back();
    }
#ifdef _WIN32
    gchar* folded = g_utf8_casefold(key.c_str(), -1);
    key = folded;
    g_free(folded);
#endif
    return key;
}
