#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace tt2::launcher
{
    std::filesystem::path executable_dir();

    class ChildProcess
    {
    public:
        static std::unique_ptr<ChildProcess> start(const std::filesystem::path& exe, const std::vector<std::wstring>& args,
                                                   const std::filesystem::path& working_dir, bool new_console);
        ~ChildProcess();
        bool running() const;
        int exit_code() const;

    private:
        void* process_ = nullptr;
    };
}
