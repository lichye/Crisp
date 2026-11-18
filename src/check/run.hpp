#pragma once

#include "front/front.hpp"

namespace crisp::check {

struct Summary {
    std::size_t proved = 0, refuted = 0, unknown = 0;
    bool error = false, incomplete = false;
    int exit_code() const {
        if (error) return 2;
        if (refuted) return 1;
        if (incomplete || unknown || !proved) return 3;
        return 0;
    }
};

void run(const front::Task&, Summary&);

} // namespace crisp::check
