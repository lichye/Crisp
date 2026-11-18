#pragma once

#include "../cli/ParseArguments.h"
#include <filesystem>
#include <map>

namespace veval::cli {
std::string jsonEscape(const std::string&);
std::string readText(const std::filesystem::path&);
std::string sha256(const std::string&);
void writeEffectiveConfig(const CommandLineOptions&);
void writeUnifiedResultMetadata(const CommandLineOptions&, const std::string& boundaryFile,
                                const std::map<std::string, std::size_t>& counts);
} // namespace veval::cli
