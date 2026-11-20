#pragma once

/// @file
/// @brief Loading shared libraries (DLL / .so / .dylib) at runtime.

#include "nova/core/Base.h"

#include <filesystem>
#include <string>

namespace nova::platform {

/// @brief An open shared library; closed on destruction. Move-only.
class DynamicLibrary {
public:
    DynamicLibrary() = default;
    ~DynamicLibrary() { close(); }
    DynamicLibrary(DynamicLibrary&& o) noexcept : m_handle(std::exchange(o.m_handle, nullptr)) {}
    DynamicLibrary& operator=(DynamicLibrary&& o) noexcept {
        if (this != &o) {
            close();
            m_handle = std::exchange(o.m_handle, nullptr);
        }
        return *this;
    }
    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;

    /// @brief Opens a library (closing the current one). @param[out] error Reason on failure.
    bool open(const std::filesystem::path& path, std::string* error = nullptr);
    /// @brief Unloads the library. Everything it created must be destroyed first.
    void close();
    /// @brief True if a library is open.
    bool isOpen() const { return m_handle != nullptr; }
    /// @brief Address of an exported symbol, or `nullptr`.
    void* symbol(const char* name) const;
    /// @brief Typed symbol lookup, e.g. `lib.function<int(*)()>("version")`.
    template <typename F>
    F function(const char* name) const { return reinterpret_cast<F>(symbol(name)); }

private:
    void* m_handle = nullptr;
};

} // namespace nova::platform
