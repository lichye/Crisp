#pragma once

#include <cstddef>
#include <filesystem>

struct witness_lasso_viewt;

namespace crisp::check {

bool lasso_json_enabled();

// Write beside the scratch VCD. The parent publishes only completed artifacts.
void write_lasso_json(const witness_lasso_viewt&, const std::filesystem::path&);

} // namespace crisp::check
