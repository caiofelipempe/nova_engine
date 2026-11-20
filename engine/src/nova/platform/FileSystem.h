#pragma once

/// @file
/// @brief Small file system helpers.

#include "nova/core/Base.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

/// @brief File system helpers.
namespace nova::fs {

/// @brief Directory that contains the running executable (where `shaders/` is deployed).
std::filesystem::path executableDir();

/// @brief Reads a whole file as bytes.
/// @return The contents, or `std::nullopt` if the file cannot be opened.
std::optional<std::vector<u8>> readBinary(const std::filesystem::path& path);

/// @brief Reads a whole file as text.
/// @return The contents, or `std::nullopt` if the file cannot be opened.
std::optional<std::string> readText(const std::filesystem::path& path);

} // namespace nova::fs
