#pragma once

#include "front.hpp"

namespace crisp::front::detail {

// One set of explicit values per scope. Lists retain their physical line order.
struct Document {
    Task common;
    std::vector<Task> named;
};

struct Diagnostic {
    std::optional<std::size_t> line;
    std::string message;
};

Document parse(const std::filesystem::path& input, const std::filesystem::path& absolute);

} // namespace crisp::front::detail
