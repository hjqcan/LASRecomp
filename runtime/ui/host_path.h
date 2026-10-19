#pragma once
#include <filesystem>
#include <string>

// Candidate directories add depth. Extended paths let internal Windows I/O
// handle it without changing the registry; external identities stay ordinary.
namespace host_path {
inline std::filesystem::path logical(const std::filesystem::path &path) {
#ifdef _WIN32
    const auto &value = path.native();
    if (value.rfind(L"\\\\?\\UNC\\", 0) == 0) return L"\\\\" + value.substr(8);
    if (value.rfind(L"\\\\?\\", 0) == 0) return value.substr(4);
#endif
    return path;
}
inline std::filesystem::path io(const std::filesystem::path &path) {
    auto absolute = std::filesystem::absolute(logical(path)).lexically_normal();
#ifdef _WIN32
    absolute.make_preferred();
    auto value = absolute.native();
    if (value.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + value.substr(2);
    return L"\\\\?\\" + value;
#else
    return absolute;
#endif
}
inline std::filesystem::path relative(const std::filesystem::path &path, const std::filesystem::path &base) {
    // libstdc++ treats the drive component of \\?\C:\ as a nested root name;
    // lexically_relative then returns an empty path even for a direct child.
    return logical(path).lexically_relative(logical(base));
}
}
