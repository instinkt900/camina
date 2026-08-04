#include "platform/process.h"

#include "core/log.h"

#if defined(_WIN32)
#include <cstddef>
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <spawn.h>
#include <sys/wait.h>
// This is what declares `environ`, which is the environment the child inherits.
#include <unistd.h>
#endif

namespace engine::platform {

    namespace {

#if defined(_WIN32)

        /**
         * Turns a UTF-8 string into what the wide Windows calls take.
         *
         * A content path can hold any character a person can type, and the
         * narrow calls read one of those through the local code page. That
         * turns a path the file system accepts into one it does not.
         */
        [[nodiscard]] std::wstring widen(const std::string& text) {
            if (text.empty()) {
                return {};
            }
            const int size = static_cast<int>(text.size());
            const int needed =
                MultiByteToWideChar(CP_UTF8, 0, text.data(), size, nullptr, 0);
            if (needed <= 0) {
                return {};
            }
            std::wstring wide(static_cast<std::size_t>(needed), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.data(), size, wide.data(), needed);
            return wide;
        }

        /**
         * Adds one argument to a command line, quoted the way Windows reads it.
         *
         * Windows hands the child a single string and the child splits it. So
         * an argument holding a space has to arrive quoted, and a backslash
         * before a quote has to be doubled. This is the rule
         * CommandLineToArgvW documents, and the C runtime of the child uses the
         * same one.
         */
        void append_argument(const std::wstring& argument, std::wstring& out) {
            if (!out.empty()) {
                out.push_back(L' ');
            }
            if (!argument.empty() &&
                argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
                out.append(argument);
                return;
            }

            out.push_back(L'"');
            for (std::size_t at = 0; at < argument.size(); ++at) {
                std::size_t slashes = 0;
                while (at < argument.size() && argument[at] == L'\\') {
                    ++at;
                    ++slashes;
                }
                if (at == argument.size()) {
                    // The run ends the argument, and the quote that closes the
                    // argument comes next. Each backslash therefore has to be
                    // doubled so none of them escapes that quote.
                    out.append(slashes * 2, L'\\');
                    break;
                }
                if (argument[at] == L'"') {
                    out.append((slashes * 2) + 1, L'\\');
                } else {
                    out.append(slashes, L'\\');
                }
                out.push_back(argument[at]);
            }
            out.push_back(L'"');
        }

#endif

    } // namespace

#if defined(_WIN32)

    ProcessResult run_process(const std::filesystem::path& program,
                              const std::vector<std::string>& arguments) {
        // The first word of a command line is the program name by convention,
        // and a child that reads its own argv expects it there.
        std::wstring command;
        append_argument(program.wstring(), command);
        for (const std::string& argument : arguments) {
            append_argument(widen(argument), command);
        }

        // CreateProcessW may write into this buffer, so it cannot be the
        // string's own storage.
        std::vector<wchar_t> writable(command.begin(), command.end());
        writable.push_back(L'\0');

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION child{};

        if (CreateProcessW(program.c_str(), writable.data(), nullptr, nullptr, FALSE, 0,
                           nullptr, nullptr, &startup, &child) == 0) {
            ENGINE_LOG_ERROR("{}: it would not start. Windows error {}.", program.string(),
                             GetLastError());
            return {};
        }

        WaitForSingleObject(child.hProcess, INFINITE);
        DWORD code = 0;
        const bool read = GetExitCodeProcess(child.hProcess, &code) != 0;
        CloseHandle(child.hProcess);
        CloseHandle(child.hThread);

        if (!read) {
            ENGINE_LOG_ERROR("{}: it ran, and Windows will not say what it returned.",
                             program.string());
            return {};
        }
        return ProcessResult{ .ran = true, .exit_code = static_cast<int>(code) };
    }

#else

    ProcessResult run_process(const std::filesystem::path& program,
                              const std::vector<std::string>& arguments) {
        // posix_spawn takes a writable argument array, so every string here is
        // a copy this function owns. Building it before the spawn also keeps
        // the child free of any work, which matters because only a short list
        // of calls is legal between a fork and an exec.
        std::string program_text = program.string();
        std::vector<std::string> owned = arguments;

        std::vector<char*> argv;
        argv.reserve(owned.size() + 2);
        argv.push_back(program_text.data());
        for (std::string& argument : owned) {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);

        // posix_spawn rather than fork and exec. It reports a program that
        // will not start as an error code, where fork reports it in the child
        // that cannot log any more.
        pid_t child = 0;
        const int started =
            posix_spawn(&child, program_text.c_str(), nullptr, nullptr, argv.data(), environ);
        if (started != 0) {
            ENGINE_LOG_ERROR("{}: it would not start. {}", program_text,
                             std::strerror(started));
            return {};
        }

        int status = 0;
        while (waitpid(child, &status, 0) < 0) {
            if (errno != EINTR) {
                ENGINE_LOG_ERROR("{}: it started and the wait failed. {}", program_text,
                                 std::strerror(errno));
                return {};
            }
        }

        if (WIFSIGNALED(status)) {
            ENGINE_LOG_ERROR("{}: signal {} stopped it.", program_text, WTERMSIG(status));
            return {};
        }
        if (!WIFEXITED(status)) {
            ENGINE_LOG_ERROR("{}: it stopped without exiting.", program_text);
            return {};
        }
        return ProcessResult{ .ran = true, .exit_code = WEXITSTATUS(status) };
    }

#endif

} // namespace engine::platform
