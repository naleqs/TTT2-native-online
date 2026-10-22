#include "update_check.h"

#include <nlohmann/json.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace tt2::launcher
{
    namespace
    {
        constexpr wchar_t kHost[] = L"api.github.com";

        // TT2_UPDATE_REPO=owner/name (testing): check another repository's releases.
        std::wstring release_path()
        {
            std::string repo = "naleqs/TTT2-native-online";
            if (const char* v = std::getenv("TT2_UPDATE_REPO"); v && *v)
                repo = v;
            return L"/repos/" + std::wstring(repo.begin(), repo.end()) + L"/releases/latest";
        }

        std::mutex g_mutex;
        std::optional<UpdateInfo> g_update;
        std::atomic<bool> g_started{ false };

        // GET https://api.github.com/... (the API needs a User-Agent). Empty on any failure.
        std::string fetch_latest_release()
        {
            std::string body;
            HINTERNET session = WinHttpOpen(L"TTT2-Native-Online-launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                            WINHTTP_NO_PROXY_BYPASS, 0);
            if (!session)
                return body;
            WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);
            if (HINTERNET connect = WinHttpConnect(session, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0))
            {
                if (HINTERNET request = WinHttpOpenRequest(connect, L"GET", release_path().c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                           WINHTTP_FLAG_SECURE))
                {
                    const wchar_t* headers = L"Accept: application/vnd.github+json\r\n";
                    DWORD status = 0, size = sizeof(status);
                    if (WinHttpSendRequest(request, headers, DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                        WinHttpReceiveResponse(request, nullptr) &&
                        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                                            &size, WINHTTP_NO_HEADER_INDEX) &&
                        status == 200)
                    {
                        for (;;)
                        {
                            DWORD available = 0;
                            if (!WinHttpQueryDataAvailable(request, &available) || available == 0 || body.size() > (1u << 20))
                                break;
                            std::vector<char> chunk(available);
                            DWORD read = 0;
                            if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0)
                                break;
                            body.append(chunk.data(), read);
                        }
                    }
                    WinHttpCloseHandle(request);
                }
                WinHttpCloseHandle(connect);
            }
            WinHttpCloseHandle(session);
            return body;
        }

        // "v0.2.0" or "release-0.2.0" -> "0.2.0"
        std::string strip_prefix(std::string v)
        {
            const size_t digit = v.find_first_of("0123456789");
            return digit == std::string::npos ? std::string() : v.substr(digit);
        }

        std::vector<int> version_parts(const std::string& tag)
        {
            const std::string v = strip_prefix(tag);
            std::vector<int> parts;
            std::stringstream ss(v);
            for (std::string part; std::getline(ss, part, '.');)
                parts.push_back(std::atoi(part.c_str())); // "1-beta" counts as 1
            return parts;
        }
    }

    bool is_newer_version(const std::string& latest, const std::string& current)
    {
        const auto a = version_parts(latest), b = version_parts(current);
        for (size_t i = 0; i < std::max(a.size(), b.size()); i++)
        {
            const int x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
            if (x != y)
                return x > y;
        }
        return false;
    }

    void start_update_check(const std::string& current_version)
    {
        if (g_started.exchange(true))
            return;
        std::thread([current_version] {
            const auto j = nlohmann::json::parse(fetch_latest_release(), nullptr, false);
            if (j.is_discarded() || !j.is_object() || j.value("draft", false) || j.value("prerelease", false))
                return;
            std::string tag = j.value("tag_name", "");
            if (tag.empty() || !is_newer_version(tag, current_version))
                return;
            tag = strip_prefix(tag);
            std::lock_guard lock(g_mutex);
            g_update = UpdateInfo{ tag, j.value("html_url", "https://github.com/naleqs/TTT2-native-online/releases") };
        }).detach();
    }

    std::optional<UpdateInfo> available_update()
    {
        std::lock_guard lock(g_mutex);
        return g_update;
    }
}
