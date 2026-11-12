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

#include "control/CrashHandler.h"  // for installCrashHandlers
#include "control/XournalMain.h"   // for run

#ifdef __APPLE__
#include "osx/setup-env.h"
#endif

#ifdef _WIN32
#include <cstdlib>

#include "win32/console.h"
#endif

auto main(int argc, char* argv[]) -> int {
#ifdef _WIN32
    // Attach to the console here. Otherwise, gspawn-win32-helper will create annoying console popups,
    // e.g. in the LaTeX tool after every change in the formula
    attachConsole();
#endif

    // init crash handler
    installCrashHandlers();

#ifdef DEV_CALL_LOG
    Log::initlog();
#endif

#ifdef _WIN32
    // Switch to the FontConfig backend for Pango - See #3371
    _putenv_s("PANGOCAIRO_BACKEND", "fc");
    // Fork: with client-side decorations (a header bar) GDK makes a window a layered window, which hands the
    // whole window surface to the compositor after every paint, also for a few pixels of new ink. Without it only
    // the changed area is presented, which shortens the delay between pen and ink.
    if (!std::getenv("GDK_WIN32_LAYERED")) {
        _putenv_s("GDK_WIN32_LAYERED", "0");
    }
#endif

#ifdef __APPLE__
    // Setup the environment variables, in particular so that the pixbuf loaders are found
    setupEnvironment();
#endif

    // Use this two line to test the crash handler...
    // int* crash = nullptr;
    // *crash = 0;

    int result = XournalMain::run(argc, argv);

#ifdef DEV_CALL_LOG
    Log::closelog();
#endif

    return result;
}
