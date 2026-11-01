// The runner as a PC game (include/ps2_app.h). Its own file, outside the unity batches: it includes <windows.h>.
#include "ps2_app.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <tlhelp32.h>
#else
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#endif

#ifndef PS2X_APP_TITLE
#define PS2X_APP_TITLE ""
#endif
#ifndef PS2X_APP_FOLDER
#define PS2X_APP_FOLDER ""
#endif
#ifndef PS2X_APP_XDG_FOLDER
#define PS2X_APP_XDG_FOLDER PS2X_APP_FOLDER
#endif
#ifndef PS2X_APP_BOOT_ELF
#define PS2X_APP_BOOT_ELF ""
#endif
#ifndef PS2X_APP_DATA_FOLDER
#define PS2X_APP_DATA_FOLDER "game"
#endif

namespace fs = std::filesystem;

namespace ps2x::app
{
    namespace
    {
        constexpr size_t kTailLines = 200;
        constexpr int kLogsKept = 3;

        std::string displayName()
        {
            return *PS2X_APP_TITLE ? PS2X_APP_TITLE : (*PS2X_APP_FOLDER ? PS2X_APP_FOLDER : "ps2EntryRunner");
        }

        std::string logBaseName()
        {
            return *PS2X_APP_FOLDER ? PS2X_APP_FOLDER : "ps2EntryRunner";
        }

        std::string timeStamp(const char *format)
        {
            const std::time_t now = std::time(nullptr);
            std::tm local{};
#if defined(_WIN32)
            localtime_s(&local, &now);
#else
            localtime_r(&now, &local);
#endif
            char text[64];
            std::strftime(text, sizeof(text), format, &local);
            return text;
        }

        // The log: what the two pipes carry, written to the file and to the original stream, its last lines kept.
        struct Log
        {
            std::timed_mutex mutex;
            FILE *file = nullptr;
            fs::path path;
            std::deque<std::string> tail;
            std::string partial[2];
            int readFd[2]{-1, -1};
            int originalFd[2]{-1, -1};
            std::atomic<int> busy{0};
            std::atomic<int64_t> lastActivity{0}; // steady clock, ns: when a reader last finished a chunk
            std::thread readers[2];
            bool started = false;
        };

        Log &theLog()
        {
            static Log *log = new Log; // never destroyed: the readers outlive static destruction
            return *log;
        }

        void keepLines(Log &log, int stream, const char *data, size_t size)
        {
            std::string &line = log.partial[stream];
            for (size_t i = 0; i < size; ++i)
            {
                const char c = data[i];
                if (c == '\n')
                {
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                    log.tail.push_back(std::move(line));
                    line.clear();
                    if (log.tail.size() > kTailLines)
                        log.tail.pop_front();
                }
                else if (line.size() < 4096)
                    line.push_back(c);
            }
        }

#if defined(_WIN32)
        // On the OS handles, not _read / _write: those hold the CRT's lock on the descriptor while they wait, and a
        // reader killed by ExitProcess holding it hangs the CRT's own exit.
        int rawRead(int fd, char *buffer, unsigned size)
        {
            DWORD got = 0;
            const HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
            return ReadFile(handle, buffer, size, &got, nullptr) ? static_cast<int>(got) : -1;
        }
        void rawWrite(int fd, const char *data, size_t size)
        {
            DWORD written = 0;
            WriteFile(reinterpret_cast<HANDLE>(_get_osfhandle(fd)), data, static_cast<DWORD>(size), &written, nullptr);
        }
#else
        int rawRead(int fd, char *buffer, unsigned size) { return static_cast<int>(read(fd, buffer, size)); }
        void rawWrite(int fd, const char *data, size_t size)
        {
            while (size > 0)
            {
                const ssize_t written = write(fd, data, size);
                if (written <= 0)
                    return;
                data += written;
                size -= static_cast<size_t>(written);
            }
        }
#endif

        void readerLoop(int stream)
        {
            Log &log = theLog();
            std::vector<char> buffer(64 * 1024);
            for (;;)
            {
                const int got = rawRead(log.readFd[stream], buffer.data(), static_cast<unsigned>(buffer.size()));
                if (got <= 0)
                    return;
                ++log.busy;
                if (log.originalFd[stream] >= 0)
                    rawWrite(log.originalFd[stream], buffer.data(), static_cast<size_t>(got));
                {
                    std::lock_guard<std::timed_mutex> lock(log.mutex);
                    if (log.file)
                    {
                        rawWrite(fileno(log.file), buffer.data(), static_cast<size_t>(got)); // unbuffered, no stream lock
                    }
                    keepLines(log, stream, buffer.data(), static_cast<size_t>(got));
                }
                log.lastActivity = std::chrono::steady_clock::now().time_since_epoch().count();
                --log.busy;
            }
        }

        // <base>.log -> <base>.1.log -> <base>.2.log, the oldest dropped.
        void rotate(const fs::path &directory, const std::string &base)
        {
            std::error_code ec;
            const auto name = [&](int index)
            {
                return directory / (index == 0 ? base + ".log" : base + "." + std::to_string(index) + ".log");
            };
            fs::remove(name(kLogsKept - 1), ec);
            for (int index = kLogsKept - 2; index >= 0; --index)
                if (fs::exists(name(index), ec))
                    fs::rename(name(index), name(index + 1), ec);
        }

        FILE *openFile(const fs::path &path, const char *mode)
        {
#if defined(_WIN32)
            const std::wstring wideMode(mode, mode + std::strlen(mode));
            return _wfsopen(path.wstring().c_str(), wideMode.c_str(), _SH_DENYNO);
#else
            return std::fopen(path.string().c_str(), mode);
#endif
        }

        std::vector<std::string> lastLines()
        {
            Log &log = theLog();
            std::vector<std::string> lines;
            // The crashed thread may hold the lock (or the heap's): wait a little, then go without the lines.
            if (!log.mutex.try_lock_for(std::chrono::milliseconds(500)))
                return lines;
            lines.assign(log.tail.begin(), log.tail.end());
            for (const std::string &partial : log.partial)
                if (!partial.empty())
                    lines.push_back(partial);
            log.mutex.unlock();
            return lines;
        }

        void waitForPipes(std::chrono::milliseconds limit)
        {
            Log &log = theLog();
            if (!log.started)
                return;
            // Not by asking the pipes what they hold (PeekNamedPipe waits behind the reader's blocking ReadFile): the
            // readers get what was just written within a few ms; done once they have been idle for 40 ms.
            const auto end = std::chrono::steady_clock::now() + limit;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            while (std::chrono::steady_clock::now() < end)
            {
                const auto idle = std::chrono::steady_clock::now().time_since_epoch().count() - log.lastActivity.load();
                if (log.busy.load() == 0 && idle > std::chrono::nanoseconds(std::chrono::milliseconds(40)).count())
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }

        std::string crashFolderText(const fs::path &report)
        {
            std::string text = report.string();
            if (!logPath().empty())
                text += "\n\nThe log of this run: " + logPath().string();
            return text;
        }
    }

    bool configured()
    {
        return *PS2X_APP_FOLDER != '\0';
    }

    const char *title()
    {
        return PS2X_APP_TITLE;
    }

    const fs::path &userDirectory()
    {
        static const fs::path directory = []
        {
            fs::path result;
            if (const char *forced = std::getenv("PS2X_USER_DIR"); forced && *forced)
                result = fs::path(forced);
            else if (!configured())
                return result;
            else
            {
#if defined(_WIN32)
                if (const wchar_t *appData = _wgetenv(L"APPDATA"); appData && *appData)
                    result = fs::path(appData) / PS2X_APP_FOLDER;
                else
                    result = executableDirectory() / "user";
#else
                if (const char *data = std::getenv("XDG_DATA_HOME"); data && *data)
                    result = fs::path(data) / PS2X_APP_XDG_FOLDER;
                else if (const char *home = std::getenv("HOME"); home && *home)
                    result = fs::path(home) / ".local" / "share" / PS2X_APP_XDG_FOLDER;
                else
                    result = executableDirectory() / "user";
#endif
            }
            std::error_code ec;
            fs::create_directories(result, ec);
            const fs::path absolute = fs::absolute(result, ec);
            return ec ? result.lexically_normal() : absolute.lexically_normal();
        }();
        return directory;
    }

    fs::path executableDirectory()
    {
#if defined(_WIN32)
        std::wstring buffer(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0)
                return fs::current_path();
            if (length < buffer.size())
            {
                buffer.resize(length);
                break;
            }
            buffer.resize(buffer.size() * 2);
        }
        return fs::path(buffer).parent_path();
#else
        std::error_code ec;
        const fs::path self = fs::read_symlink("/proc/self/exe", ec);
        return ec ? fs::current_path(ec) : self.parent_path();
#endif
    }

    fs::path findGameData(std::string *searched)
    {
        if (!*PS2X_APP_BOOT_ELF)
            return {};
        std::vector<fs::path> candidates{executableDirectory() / PS2X_APP_DATA_FOLDER / PS2X_APP_BOOT_ELF};
        if (!userDirectory().empty())
            candidates.push_back(userDirectory() / PS2X_APP_DATA_FOLDER / PS2X_APP_BOOT_ELF);
        std::error_code ec;
        for (const fs::path &candidate : candidates)
        {
            if (searched)
                *searched += "  " + candidate.string() + "\n";
            if (fs::is_regular_file(candidate, ec))
                return candidate.lexically_normal();
        }
        return {};
    }

    void migrateFrom(const fs::path &oldDirectory)
    {
        const fs::path &user = userDirectory();
        std::error_code ec;
        if (user.empty() || oldDirectory.empty() || fs::equivalent(user, oldDirectory, ec))
            return;
        const fs::path source = fs::absolute(oldDirectory, ec).lexically_normal();
        // One line per old folder already migrated: each is copied from once.
        const fs::path marker = user / "migrated.txt";
        {
            std::ifstream in(marker);
            for (std::string line; std::getline(in, line);)
                if (line == source.string())
                    return;
        }
        int copied = 0;
        for (const char *card : {"mc0", "mc1"})
        {
            const fs::path from = source / card;
            if (!fs::is_directory(from, ec))
                continue;
            const fs::path to = user / card;
            fs::create_directories(to, ec);
            // A save (one folder on the card) is copied whole, or not at all when the card here already has it.
            for (const fs::directory_entry &entry : fs::directory_iterator(from, ec))
            {
                const fs::path target = to / entry.path().filename();
                if (fs::exists(target, ec))
                    continue;
                fs::copy(entry.path(), target, fs::copy_options::recursive, ec);
                if (!ec)
                    ++copied;
            }
        }
        for (const char *settings : {"input.ini", "video.ini"})
        {
            const fs::path from = source / settings, to = user / settings;
            if (fs::is_regular_file(from, ec) && !fs::exists(to, ec) && fs::copy_file(from, to, ec))
                ++copied;
        }
        std::ofstream(marker, std::ios::app) << source.string() << "\n";
        std::fprintf(stderr, "[app] %d save(s) and settings file(s) copied from %s to %s (the old ones left in place)\n",
                     copied, source.string().c_str(), user.string().c_str());
    }

    const fs::path &logPath()
    {
        return theLog().path;
    }

    void startLogging()
    {
        Log &log = theLog();
        if (log.started)
            return;
        const char *setting = std::getenv("PS2X_LOG_FILE");
        const bool noFile = setting && std::strcmp(setting, "0") == 0;
        if (!noFile)
        {
            if (setting && *setting)
                log.path = fs::path(setting);
            else if (!userDirectory().empty())
            {
                const fs::path directory = userDirectory() / "logs";
                std::error_code ec;
                fs::create_directories(directory, ec);
                rotate(directory, logBaseName());
                log.path = directory / (logBaseName() + ".log");
            }
            if (!log.path.empty())
            {
                log.file = openFile(log.path, "wb");
                if (!log.file && !(setting && *setting))
                {
                    // Another instance writing it: a file of this run's own.
                    log.path.replace_filename(logBaseName() + "." + timeStamp("%Y%m%d-%H%M%S") + ".log");
                    log.file = openFile(log.path, "wb");
                }
                if (!log.file)
                    log.path.clear();
            }
        }

        FILE *streams[2] = {stdout, stderr};
#if defined(_WIN32)
        const DWORD stdIds[2] = {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
        const auto valid = [](HANDLE handle) { return handle != nullptr && handle != INVALID_HANDLE_VALUE; };
        bool terminal = valid(GetStdHandle(STD_OUTPUT_HANDLE)) || valid(GetStdHandle(STD_ERROR_HANDLE));
        // A console of our own (a console-subsystem runner opened from Explorer): let it go, the log has it all.
        if (terminal && GetConsoleWindow() && GetFileType(GetStdHandle(STD_ERROR_HANDLE)) == FILE_TYPE_CHAR)
        {
            DWORD processes[2];
            if (GetConsoleProcessList(processes, 2) == 1)
            {
                FreeConsole();
                terminal = false;
                for (FILE *stream : streams)
                    std::freopen("NUL", "w", stream);
            }
        }
        // A GUI-subsystem runner started from cmd or PowerShell: their console.
        if (!terminal && AttachConsole(ATTACH_PARENT_PROCESS))
        {
            for (FILE *stream : streams)
                std::freopen("CONOUT$", "w", stream);
            terminal = true;
        }
        if (_fileno(stdin) < 0)
            std::freopen("NUL", "r", stdin);
        for (int i = 0; i < 2; ++i)
        {
            if (_fileno(streams[i]) < 0 || _get_osfhandle(_fileno(streams[i])) < 0)
                std::freopen("NUL", "w", streams[i]);
            const bool real = valid(GetStdHandle(stdIds[i])) || terminal;
            log.originalFd[i] = real && _fileno(streams[i]) >= 0 ? _dup(_fileno(streams[i])) : -1;
        }
#else
        for (int i = 0; i < 2; ++i)
            log.originalFd[i] = fcntl(fileno(streams[i]), F_GETFD) != -1 ? dup(fileno(streams[i])) : -1;
#endif
        if (!log.file && log.originalFd[0] < 0 && log.originalFd[1] < 0)
            return;

        for (int i = 0; i < 2; ++i)
        {
            int fds[2];
#if defined(_WIN32)
            if (_pipe(fds, 1 << 20, _O_BINARY | _O_NOINHERIT) != 0)
                return;
#else
            if (pipe(fds) != 0)
                return;
            fcntl(fds[0], F_SETFD, FD_CLOEXEC);
            fcntl(fds[1], F_SETFD, FD_CLOEXEC);
#endif
            std::fflush(streams[i]);
#if defined(_WIN32)
            _dup2(fds[1], _fileno(streams[i]));
            SetStdHandle(stdIds[i], reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(streams[i]))));
            _close(fds[1]);
#else
            dup2(fds[1], fileno(streams[i]));
            close(fds[1]);
#endif
            log.readFd[i] = fds[0];
            log.readers[i] = std::thread(readerLoop, i);
            log.readers[i].detach();
        }
        std::setvbuf(stderr, nullptr, _IONBF, 0);
        log.started = true;
        if (log.file)
        {
            const std::string header = displayName() + " log, " + timeStamp("%Y-%m-%d %H:%M:%S") + ", " +
                                       executableDirectory().string() + "\n";
            std::lock_guard<std::timed_mutex> lock(log.mutex);
            rawWrite(fileno(log.file), header.data(), header.size());
        }
    }

    void flushLog()
    {
        std::fflush(stdout);
        std::fflush(stderr);
        waitForPipes(std::chrono::milliseconds(1000));
    }

    namespace
    {
        fs::path writeReport(const std::string &body)
        {
            fs::path directory = userDirectory().empty() ? executableDirectory() : userDirectory();
            directory /= "crashes";
            std::error_code ec;
            fs::create_directories(directory, ec);
            const fs::path path = directory / ("crash-" + timeStamp("%Y%m%d-%H%M%S") + ".txt");
            if (FILE *file = openFile(path, "wb"))
            {
                std::fwrite(body.data(), 1, body.size(), file);
                std::fclose(file);
            }
            return path;
        }

        std::string reportHeader(const std::string &what)
        {
            std::string text = displayName() + " crash report\n";
            text += "Time: " + timeStamp("%Y-%m-%d %H:%M:%S") + "\n";
            text += "Executable folder: " + executableDirectory().string() + "\n";
            if (!logPath().empty())
                text += "Log: " + logPath().string() + "\n";
            text += what;
            return text;
        }

        std::string reportTail()
        {
            std::string text = "\nLast log lines:\n";
            for (const std::string &line : lastLines())
                text += "  " + line + "\n";
            return text;
        }
    }

    void showMessage(const std::string &text, bool error)
    {
        std::fprintf(stderr, "[app] %s\n", text.c_str());
        if (const char *quiet = std::getenv("PS2X_NO_DIALOGS"); quiet && *quiet == '1')
            return;
#if defined(_WIN32)
        const auto widen = [](const std::string &utf8)
        {
            const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
            std::wstring wide(size > 0 ? size : 1, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), size);
            return wide;
        };
        MessageBoxW(nullptr, widen(text).c_str(), widen(displayName()).c_str(),
                    (error ? MB_ICONERROR : MB_ICONINFORMATION) | MB_OK | MB_TOPMOST | MB_SETFOREGROUND);
#else
        (void)error;
#endif
    }

    void reportFatal(const std::string &reason)
    {
        waitForPipes(std::chrono::milliseconds(300));
        const fs::path report = writeReport(reportHeader("Fatal error: " + reason + "\n") + reportTail());
        showMessage(displayName() + " stopped on an error:\n" + reason + "\n\nA report was written to:\n" +
                        crashFolderText(report),
                    true);
    }

#if defined(_WIN32)
    namespace
    {
        EXCEPTION_RECORD g_record;
        CONTEXT g_context;
        DWORD g_crashedThread = 0;
        std::atomic<DWORD> g_helperThread{0};
        fs::path g_reportPath;

        const char *exceptionName(DWORD code)
        {
            switch (code)
            {
            case EXCEPTION_ACCESS_VIOLATION: return "access violation";
            case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
            case EXCEPTION_DATATYPE_MISALIGNMENT: return "datatype misalignment";
            case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "float divide by zero";
            case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
            case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
            case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
            case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
            case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
            case 0xC0000409: return "stack buffer overrun / fast fail";
            case 0xC0000374: return "heap corruption";
            default: return "exception";
            }
        }

        // module+offset, and the address addr2line takes (the module's preferred base plus the offset).
        std::string describeAddress(DWORD64 address)
        {
            char text[512];
            HMODULE module = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCWSTR>(address), &module) ||
                !module)
            {
                std::snprintf(text, sizeof(text), "0x%016llx", static_cast<unsigned long long>(address));
                return text;
            }
            wchar_t widePath[MAX_PATH] = {};
            GetModuleFileNameW(module, widePath, MAX_PATH);
            const std::string name = fs::path(widePath).filename().string();
            const unsigned long long offset = address - reinterpret_cast<DWORD64>(module);
            // The preferred base from the file on disk: the loaded image's header holds the base it was relocated to.
            unsigned long long preferred = 0;
            const HANDLE file = CreateFileW(widePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                            nullptr, OPEN_EXISTING, 0, nullptr);
            if (file != INVALID_HANDLE_VALUE)
            {
                uint8_t header[4096];
                DWORD got = 0;
                if (ReadFile(file, header, sizeof(header), &got, nullptr) && got >= sizeof(IMAGE_DOS_HEADER))
                {
                    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(header);
                    if (dos->e_lfanew > 0 && static_cast<DWORD>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS) <= got)
                        preferred = reinterpret_cast<const IMAGE_NT_HEADERS *>(header + dos->e_lfanew)->OptionalHeader.ImageBase;
                }
                CloseHandle(file);
            }
            std::snprintf(text, sizeof(text), "%s+0x%llx (0x%llx)", name.c_str(), offset, preferred + offset);
            return text;
        }

        DWORD WINAPI reportThread(LPVOID)
        {
            g_helperThread = GetCurrentThreadId();
            char line[256];
            std::string what;
            std::snprintf(line, sizeof(line), "Exception: 0x%08lx (%s)", static_cast<unsigned long>(g_record.ExceptionCode),
                          exceptionName(g_record.ExceptionCode));
            what += line;
            if (g_record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && g_record.NumberParameters >= 2)
            {
                const ULONG_PTR kind = g_record.ExceptionInformation[0];
                std::snprintf(line, sizeof(line), ", %s at 0x%llx",
                              kind == 0 ? "read" : kind == 1 ? "write" : "execute",
                              static_cast<unsigned long long>(g_record.ExceptionInformation[1]));
                what += line;
            }
            what += "\nAddress: " + describeAddress(reinterpret_cast<DWORD64>(g_record.ExceptionAddress)) + "\n";
            std::snprintf(line, sizeof(line), "Thread: %lu\n", static_cast<unsigned long>(g_crashedThread));
            what += line;
            // Written once without the stack, in case the walk faults on a damaged stack.
            const std::string first = reportHeader(what) + reportTail();
            g_reportPath = writeReport(first);
            std::string stack = "\nStack (addr2line -f -C -e <exe> <address in parentheses>):\n";
            CONTEXT context = g_context;
            for (int frame = 0; frame < 48 && context.Rip; ++frame)
            {
                stack += "  #" + std::to_string(frame) + " " + describeAddress(context.Rip) + "\n";
                DWORD64 imageBase = 0;
                PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
                if (function)
                {
                    void *handlerData = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData,
                                     &establisher, nullptr);
                }
                else
                {
                    // A leaf: the return address is at the stack pointer.
                    DWORD64 returnAddress = 0;
                    SIZE_T read = 0;
                    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(context.Rsp), &returnAddress,
                                           sizeof(returnAddress), &read) ||
                        read != sizeof(returnAddress))
                        break;
                    context.Rip = returnAddress;
                    context.Rsp += 8;
                }
            }
            if (FILE *file = openFile(g_reportPath, "wb"))
            {
                const std::string full = reportHeader(what) + stack + reportTail();
                std::fwrite(full.data(), 1, full.size(), file);
                std::fclose(file);
            }
            return 0;
        }

        DWORD WINAPI messageThread(LPVOID)
        {
            g_helperThread = GetCurrentThreadId();
            // The game's windows out of the way (asynchronously: their threads may be the one that crashed).
            EnumWindows([](HWND window, LPARAM) -> BOOL
                        {
                            DWORD process = 0;
                            GetWindowThreadProcessId(window, &process);
                            if (process == GetCurrentProcessId())
                                ShowWindowAsync(window, SW_HIDE);
                            return TRUE; },
                        0);
            showMessage(displayName() + " has crashed.\n\nA crash report was written to:\n" + crashFolderText(g_reportPath) +
                            "\n\nPlease attach both files when reporting the problem.",
                        true);
            return 0;
        }

        LONG WINAPI crashFilter(EXCEPTION_POINTERS *pointers)
        {
            static std::atomic<bool> s_entered{false};
            if (GetCurrentThreadId() == g_helperThread.load())
                ExitThread(1); // the report's own walk faulted: the first write stands
            if (s_entered.exchange(true))
            {
                Sleep(INFINITE); // a second thread crashing meanwhile: the first one's report ends the process
                return EXCEPTION_CONTINUE_SEARCH;
            }
            g_record = *pointers->ExceptionRecord;
            g_context = *pointers->ContextRecord;
            g_crashedThread = GetCurrentThreadId();
            waitForPipes(std::chrono::milliseconds(300));
            for (LPTHREAD_START_ROUTINE routine : {reportThread, messageThread})
            {
                g_helperThread = 0;
                if (HANDLE thread = CreateThread(nullptr, 1 << 20, routine, nullptr, 0, nullptr))
                {
                    WaitForSingleObject(thread, INFINITE);
                    CloseHandle(thread);
                }
            }
            TerminateProcess(GetCurrentProcess(), g_record.ExceptionCode);
            return EXCEPTION_EXECUTE_HANDLER;
        }
    }
#endif

    void installCrashHandler()
    {
#if defined(_WIN32)
        SetUnhandledExceptionFilter(crashFilter);
        // Some room for the filter on a thread that overflowed its stack.
        ULONG guarantee = 64 * 1024;
        SetThreadStackGuarantee(&guarantee);
#endif
        if (const char *test = std::getenv("PS2X_TEST_CRASH"); test && *test)
        {
            const double seconds = std::atof(test);
            std::thread([seconds]
                        {
                            std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
                            std::fprintf(stderr, "[app] PS2X_TEST_CRASH: crashing on purpose\n");
                            volatile uintptr_t nowhere = 0;
                            *reinterpret_cast<volatile int *>(nowhere) = 0x7E57; })
                .detach();
        }
    }
}
