#pragma once

#include "nova/core/Base.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace nova::fs {

// Directory that contains the running executable.
std::filesystem::path executableDir();

std::optional<std::vector<u8>> readBinary(const std::filesystem::path& path);
std::optional<std::string> readText(const std::filesystem::path& path);

} // namespace nova::fs
