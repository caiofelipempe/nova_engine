#include "nova/platform/FileSystem.h"

#include <fstream>

#if defined(_WIN32)
    #include <windows.h>
#elif defined(__linux__)
    #include <unistd.h>
#endif

namespace nova::fs {

std::filesystem::path executableDir() {
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(std::wstring(buffer, len)).parent_path();
#elif defined(__linux__)
    char buffer[4096];
    ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len <= 0) return std::filesystem::current_path();
    return std::filesystem::path(std::string(buffer, static_cast<size_t>(len))).parent_path();
#else
    return std::filesystem::current_path();
#endif
}

std::optional<std::vector<u8>> readBinary(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return std::nullopt;
    auto size = static_cast<size_t>(file.tellg());
    std::vector<u8> data(size);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    return data;
}

std::optional<std::string> readText(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

} // namespace nova::fs
