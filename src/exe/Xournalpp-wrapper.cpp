/*
 * Xournal++
 *
 * The main application
 *
 * @author Xournal++ Team
 * https://github.com/xournalpp/xournalpp
 *
 * @license GNU GPLv2
 */

#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <thread>

#include <gio/gio.h>
#include <gtk/gtk.h>

#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <shellapi.h>
// clang-format on
#endif

#include "util/PathUtil.h"
#include "util/VersionInfo.h"
#include "util/XojMsgBox.h"
#include "util/glib_casts.h"
#include "util/gtk4_helper.h"
#include "util/raii/CStringWrapper.h"
#include "util/raii/GObjectSPtr.h"

#include "config-dev.h"
#include "config.h"
#include "filesystem.h"

static constexpr const char* UI_RESOURCE = "/org/xournalpp/wrapper/ui/crashDialog.glade";

// See CrashHandler::emergencySave()
static constexpr const char* EMERGENCY_SAVE_MSG_REGEX = "Successfully saved document to \"(.*)\"";

static auto escape(const std::string& str) {
    return xoj::util::OwnedCString::assumeOwnership(g_uri_escape_string(str.c_str(), nullptr, true));
}

/// Create a URL which opens a crash report on github, pre-filled with the data we can get
static std::string makeCrashReportURL() {
    // See .github/ISSUE_TEMPLATE/crash_report.yml for the keywords (id)
    std::stringstream str;
    str.imbue(std::locale::classic());

    str << PROJECT_CRASHREPORT;
    str << "&os=" << escape(xoj::util::getOsInfo()).get();

#ifdef __linux__
    auto getEnvTruncated = [](const char* var) {
        if (auto* value = std::getenv(var); value) {
            auto d = std::string(value);
            if (d.length() > 20) {
                // No control over the environment variable: limit the length
                d.resize(20);
            }
            return d;
        } else {
            return std::string("unknown");
        }
    };
    str << "&desktop=" << escape(getEnvTruncated("DESKTOP_SESSION") + " " + getEnvTruncated("XDG_SESSION_TYPE")).get();
    str << "&displayserver=" << escape(xoj::util::getGdkBackend()).get();
#endif

    str << "&version=" << escape(xoj::util::getXournalppVersion()).get();
    str << "&gtk=" << gtk_major_version << "." << gtk_minor_version << "." << gtk_micro_version;

    return str.str();
}

static void activate(GtkApplication* app, std::string* str) {
    xoj::util::GObjectSPtr<GtkBuilder> builder(gtk_builder_new_from_resource(UI_RESOURCE), xoj::util::adopt);
    auto* window = GTK_WIDGET(gtk_builder_get_object(builder.get(), "crashDialog"));
    gtk_application_add_window(app, GTK_WINDOW(window));
    auto* view = GTK_TEXT_VIEW(gtk_builder_get_object(builder.get(), "textViewLog"));
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(view), str->data(), -1);

    auto* entry = GTK_ENTRY(gtk_builder_get_object(builder.get(), "entryPathEmergencySave"));
    std::smatch res;
    std::regex_search(*str, res, std::regex(EMERGENCY_SAVE_MSG_REGEX));
    if (res.empty()) {
        gtk_editable_set_text(GTK_EDITABLE(entry), "FAILED");
        gtk_widget_add_css_class(GTK_WIDGET(entry), "error");
    } else {
        gtk_editable_set_text(GTK_EDITABLE(entry), res[1].str().c_str());
    }

    g_signal_connect_object(gtk_builder_get_object(builder.get(), "closeBtn"), "clicked",
                            G_CALLBACK(+[](GtkButton* btn, gpointer w) { gtk_window_close(GTK_WINDOW(w)); }), window,
                            GConnectFlags(0));
    g_signal_connect_object(gtk_builder_get_object(builder.get(), "reportBtn"), "clicked",
                            G_CALLBACK(+[](GtkButton* btn, gpointer w) {
                                XojMsgBox::openURL(GTK_WINDOW(w), makeCrashReportURL().c_str());
                            }),
                            window, GConnectFlags(0));
    gtk_window_present(GTK_WINDOW(window));
}

#ifdef _WIN32
namespace {
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
        quoted.append(*it == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
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

struct ChildResult {
    bool started = false;
    DWORD pid = 0;
    DWORD exitCode = 1;
    std::string output;
    std::string error;
};

/**
 * Fork: runs xournalpp directly with CreateProcessW. GSubprocess goes through an extra helper process
 * (gspawn-win64-helper) on Windows, which delays every start from the Start menu.
 */
auto runDirectly(const fs::path& exe) -> ChildResult {
    ChildResult result;
    std::wstring commandLine = quoteArgument(exe.wstring());
    int nArgs = 0;
    if (LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &nArgs)) {
        for (int i = 1; i < nArgs; i++) {
            commandLine += L" " + quoteArgument(args[i]);
        }
        LocalFree(args);
    }
    SetEnvironmentVariableW(L"G_MESSAGES_DEBUG", L"" G_LOG_DOMAIN);  // Print xopp debug messages

    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr;
    if (!CreatePipe(&outRead, &outWrite, &attributes, 0) || !CreatePipe(&errRead, &errWrite, &attributes, 0)) {
        return result;
    }
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = outWrite;
    startup.hStdError = errWrite;
    PROCESS_INFORMATION process{};
    result.started = CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup,
                                    &process);
    CloseHandle(outWrite);
    CloseHandle(errWrite);
    if (result.started) {
        result.pid = process.dwProcessId;
        std::cout << "Xournal++ started with PID: " << result.pid << std::endl;
        std::thread errorReader([&] { result.error = readAll(errRead); });
        result.output = readAll(outRead);
        errorReader.join();
        WaitForSingleObject(process.hProcess, INFINITE);
        GetExitCodeProcess(process.hProcess, &result.exitCode);
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
    }
    CloseHandle(outRead);
    CloseHandle(errRead);
    return result;
}
}  // namespace
#endif

auto main(int argc, char* argv[]) -> int {
    GError* err = nullptr;
    std::stringstream errorlog;
    errorlog.imbue(std::locale::classic());

#ifdef _WIN32
    const auto exe = Util::getExePath() / "xournalpp.exe";
    errorlog << "Executing \"" << char_cast(exe.u8string().c_str()) << "\"" << std::endl;
    auto child = runDirectly(exe);
    int exitstatus = -1;
    if (!child.started) {
        printf("Failed to start\n");
        errorlog << "Failed to start: error " << GetLastError() << std::endl;
    } else {
        errorlog << "Xournal++ started with PID: " << child.pid << std::endl;
        exitstatus = static_cast<int>(child.exitCode);
        if (exitstatus == 0) [[likely]] {
            std::cout << "Execution completed normally" << std::endl;
            return 0;
        }
        std::cout << "Exited with error status: " << exitstatus << std::endl;
        errorlog << "Exited with error status: " << exitstatus << std::endl;
        time_t lt = time(nullptr);
        errorlog << "    Date: " << ctime(&lt) << std::endl;
        errorlog << xoj::util::getVersionInfo();
        errorlog << "  (The GDK backend is probably printed out below)\n";
        errorlog << "\n*** Output: ***\n\n" << child.output;
        errorlog << "\n\n*** Cerr: ***\n\n" << child.error;
    }
    (void)argc;
    (void)argv;
    (void)err;
#else

    auto p = [&]() {
        static constexpr auto FLAGS = GSubprocessFlags(G_SUBPROCESS_FLAGS_STDOUT_PIPE
#ifdef _WIN32  // On Windows, STDERR_MERGE does not work. See https://gitlab.gnome.org/GNOME/glib/-/issues/3723
                                                       | G_SUBPROCESS_FLAGS_STDERR_PIPE
#else
                                                       | G_SUBPROCESS_FLAGS_STDERR_MERGE
#endif
        );

        std::vector<const char*> subargv;
        std::cout << Util::getExePath() << std::endl;

        const std::u8string path = (Util::getExePath() / "xournalpp").u8string();
        subargv.emplace_back(char_cast(path.c_str()));  // Data is owned by `path` - Do not delete it
        errorlog << "Executing \"" << char_cast(path);

#ifdef _WIN32
        std::vector<std::string> utf8_args;

        LPWSTR* szArglist;
        int nArgs;
        szArglist = CommandLineToArgvW(GetCommandLineW(), &nArgs);

        if (szArglist != nullptr) {
            // Start from 1 to skip program name
            for (int i = 1; i < nArgs; i++) {
                // Calculate required buffer size for UTF-8
                int size_needed = WideCharToMultiByte(CP_UTF8, 0, szArglist[i], -1, nullptr, 0, nullptr, nullptr);
                if (size_needed > 0) {
                    std::string utf8_arg(size_needed - 1, 0);  // -1 to exclude null terminator
                    WideCharToMultiByte(CP_UTF8, 0, szArglist[i], -1, &utf8_arg[0], size_needed, nullptr, nullptr);
                    utf8_args.push_back(std::move(utf8_arg));
                }
            }
            LocalFree(szArglist);

            // Add UTF-8 encoded arguments
            for (const auto& arg: utf8_args) {
                subargv.emplace_back(arg.c_str());
                errorlog << " " << arg;
            }
        } else {
            // Fallback to original argv if CommandLineToArgvW fails
            for (int i = 1; i < argc; i++) {
                subargv.emplace_back(argv[i]);
                errorlog << " " << argv[i];
            }
        }
#else
        for (int i = 1; i < argc; i++) {
            subargv.emplace_back(argv[i]);
            errorlog << " " << argv[i];
        }
#endif
        subargv.emplace_back(nullptr);
        errorlog << "\"" << std::endl;

        xoj::util::GObjectSPtr<GSubprocessLauncher> launcher(g_subprocess_launcher_new(FLAGS), xoj::util::adopt);
        g_subprocess_launcher_set_environ(launcher.get(), nullptr);  // Copies the host's environment
        g_subprocess_launcher_setenv(launcher.get(), "G_MESSAGES_DEBUG", G_LOG_DOMAIN, false);  // Print xopp debug

        return xoj::util::GObjectSPtr<GSubprocess>(g_subprocess_launcher_spawnv(launcher.get(), subargv.data(), &err),
                                                   xoj::util::adopt);
    }();

    int exitstatus = -1;

    if (err) {
        printf("Failed to start\n");
        errorlog << "Failed to start: " << err->message << std::endl;
        g_error_free(err);
        err = nullptr;
    } else {
        xoj::util::OwnedCString stdoutBuffer;
#ifdef _WIN32  // On Windows, STDERR_MERGE does not work. See https://gitlab.gnome.org/GNOME/glib/-/issues/3723
        xoj::util::OwnedCString stderrBuffer;
#endif

        std::cout << "Xournal++ started with PID: " << g_subprocess_get_identifier(p.get()) << std::endl;
        errorlog << "Xournal++ started with PID: " << g_subprocess_get_identifier(p.get()) << std::endl;

        g_subprocess_communicate_utf8(p.get(), nullptr, nullptr, stdoutBuffer.contentReplacer(),
#ifdef _WIN32  // On Windows, STDERR_MERGE does not work. See https://gitlab.gnome.org/GNOME/glib/-/issues/3723
                                      stderrBuffer.contentReplacer(),
#else
                                      nullptr,
#endif
                                      &err);

        if (g_subprocess_get_if_exited(p.get())) {
            exitstatus = g_subprocess_get_exit_status(p.get());
            if (exitstatus == 0) [[likely]] {
                std::cout << "Execution completed normally" << std::endl;
                return 0;
            } else {
                std::cout << "Exited with error status: " << exitstatus << std::endl;
                errorlog << "Exited with error status: " << exitstatus << std::endl;
            }
        }
        if (g_subprocess_get_if_signaled(p.get())) {
            auto sig = g_subprocess_get_term_sig(p.get());
            std::cout << "Crashed with signal: " << sig << std::endl;
            errorlog << "Crashed with signal: " << sig << std::endl;
        }

        if (err) {
            errorlog << "    Error: " << err->message << std::endl;
            g_error_free(err);
            err = nullptr;
        }

        time_t lt = time(nullptr);
        errorlog << "    Date: " << ctime(&lt) << std::endl;

        errorlog << xoj::util::getVersionInfo();
        errorlog << "  (The GDK backend is probably printed out below)\n";

        errorlog << "\n*** Output: ***\n\n" << stdoutBuffer.get();

#ifdef _WIN32
        errorlog << "\n\n*** Cerr: ***\n\n" << stderrBuffer.get();
#endif
    }

#endif

    GtkApplication* app = gtk_application_new(APPLICATION_ID, G_APPLICATION_FLAGS_NONE);
    g_signal_connect_data(app, "activate", xoj::util::wrap_for_g_callback_v<activate>, new std::string(errorlog.str()),
                          xoj::util::closure_notify_cb<std::string>, GConnectFlags(0));

    g_application_run(G_APPLICATION(app), 0, nullptr);
    g_object_unref(app);

    return exitstatus;
}
