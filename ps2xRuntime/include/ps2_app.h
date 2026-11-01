#pragma once

// The runner as a PC game, for a port that names itself (CMake PS2X_APP_TITLE, PS2X_APP_FOLDER, PS2X_APP_XDG_FOLDER,
// PS2X_APP_BOOT_ELF, PS2X_APP_DATA_FOLDER): a per-user folder for the memory cards, the settings and the log, the
// game data found without an argument, the console's output also in a log file, a crash report and message boxes
// instead of a silent exit. With no PS2X_APP_FOLDER the runner behaves as before (configured() false, userDirectory()
// empty).

#include <filesystem>
#include <string>

namespace ps2x::app
{
    // Whether the build names an application (PS2X_APP_FOLDER).
    bool configured();

    // The window title: PS2X_APP_TITLE, or empty (the runner then builds its own).
    const char *title();

    // The per-user folder, created on first use: PS2X_USER_DIR, else %APPDATA%\<PS2X_APP_FOLDER> on Windows,
    // $XDG_DATA_HOME/<PS2X_APP_XDG_FOLDER> (~/.local/share/...) elsewhere. Empty when not configured.
    const std::filesystem::path &userDirectory();

    // The folder of the running executable.
    std::filesystem::path executableDirectory();

    // The game's boot ELF with no argument: <exe>/<PS2X_APP_DATA_FOLDER>/<PS2X_APP_BOOT_ELF>, then the same under the
    // user folder. Empty when neither exists; searched lists the places looked at.
    std::filesystem::path findGameData(std::string *searched = nullptr);

    // Copies the memory cards (mc0, mc1) and the settings (input.ini, video.ini) from the folder they were kept in
    // before (the ELF's) to the user folder, once per old folder, never overwriting and never deleting.
    void migrateFrom(const std::filesystem::path &oldDirectory);

    // Everything written to stdout and stderr also goes to <user folder>/logs/<name>.log (the last 3 runs kept:
    // <name>.log, <name>.1.log, <name>.2.log), the terminal still getting it when there is one. On Windows, a
    // GUI-subsystem runner started from cmd or PowerShell attaches to their console. PS2X_LOG_FILE=<path> writes
    // there instead, PS2X_LOG_FILE=0 writes no file.
    void startLogging();
    // The log file in use (empty if none).
    const std::filesystem::path &logPath();
    // Writes out what is still in the pipes to the log (before the process exits).
    void flushLog();

    // Windows: an unhandled-exception filter writing <user folder>/crashes/crash-<time>.txt (exception, address and
    // module, the stack, the last 200 log lines) and showing a message box naming it. PS2X_TEST_CRASH=<seconds>
    // crashes a thread on purpose after that long, to test it.
    void installCrashHandler();
    // The same report for a fatal error that is not a CPU exception (std::terminate, a fatal exception in main).
    void reportFatal(const std::string &reason);

    // A message box on Windows (stderr elsewhere), the text in the log either way; PS2X_NO_DIALOGS=1: the log only
    // (unattended runs).
    void showMessage(const std::string &text, bool error);
}
