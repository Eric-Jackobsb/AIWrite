// string_util.cpp —— 字符串通用工具单一来源实现（CLN-05 收敛）
#include "string_util.h"

#include <windows.h> // ::WideCharToMultiByte

#include <cctype>
#include <ctime>

namespace aiwrite::utils {

std::size_t findCi(const std::string& haystack, const std::string& needle)
{
    if (needle.empty()) {
        return 0;
    }
    if (needle.size() > haystack.size()) {
        return std::string::npos;
    }
    const auto fold = [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool ok = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (fold(haystack[i + j]) != fold(needle[j])) {
                ok = false;
                break;
            }
        }
        if (ok) {
            return i;
        }
    }
    return std::string::npos;
}

std::string trim(const std::string& text)
{
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

std::string nowString()
{
    const std::time_t now = std::time(nullptr);
    std::tm           local{};
    localtime_s(&local, &now);
    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local);
    return buffer;
}

std::string toLower(std::string text)
{
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

std::string narrow(const std::wstring& wide)
{
    if (wide.empty()) {
        return {};
    }
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (need <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), need,
                          nullptr, nullptr);
    return out;
}

} // namespace aiwrite::utils
