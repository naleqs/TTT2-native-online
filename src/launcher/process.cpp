#include "process.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace tt2::launcher
{
    std::filesystem::path executable_dir()
    {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        return std::filesystem::path(path).parent_path();
    }

    namespace
    {
        // Windows command-line quoting (CommandLineToArgvW rules).
        std::wstring quote(const std::wstring& arg)
        {
            if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos)
                return arg;
            std::wstring out = L"\"";
            size_t backslashes = 0;
            for (wchar_t c : arg)
            {
                if (c == L'\\')
                {
                    backslashes++;
                    continue;
                }
                if (c == L'"')
                    out.append(backslashes * 2 + 1, L'\\');
                else
                    out.append(backslashes, L'\\');
                backslashes = 0;
                out.push_back(c);
            }
            out.append(backslashes * 2, L'\\');
            out.push_back(L'"');
            return out;
        }
    }

    std::unique_ptr<ChildProcess> ChildProcess::start(const std::filesystem::path& exe, const std::vector<std::wstring>& args,
                                                      const std::filesystem::path& working_dir, bool new_console)
    {
        std::wstring cmd = quote(exe.wstring());
        for (const auto& a : args)
            cmd += L" " + quote(a);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, new_console ? CREATE_NEW_CONSOLE : 0, nullptr,
                            working_dir.c_str(), &si, &pi))
            return nullptr;
        CloseHandle(pi.hThread);
        auto child = std::unique_ptr<ChildProcess>(new ChildProcess());
        child->process_ = pi.hProcess;
        return child;
    }

    ChildProcess::~ChildProcess()
    {
        if (process_)
            CloseHandle(process_);
    }

    bool ChildProcess::running() const
    {
        return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
    }

    int ChildProcess::exit_code() const
    {
        DWORD code = 1;
        if (process_)
            GetExitCodeProcess(process_, &code);
        return int(code);
    }
}
