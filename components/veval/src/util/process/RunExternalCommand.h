#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace veval::util::command_runner {
std::string read(const std::filesystem::path&);
std::string env(const char* key, const std::string& fallback = {});
void installCancellationHandlers();
int cancellationSignal();
int command(const std::vector<std::string>&, const std::filesystem::path& log,
            double seconds = 10, bool progress = false, bool cancellable = false,
            const std::filesystem::path& directory = {});
std::string query(const std::vector<std::string>&, const std::filesystem::path& log,
                  double seconds = 10, bool cancellable = false);
} // namespace veval::util::command_runner
