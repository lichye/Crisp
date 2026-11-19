#pragma once
#include "../../util/database/Json.h"
namespace veval::result_validation {
struct Product {
    struct PropertyRange {
        size_t begin, end;
        std::string label;
        bool assertion;
    };
    std::vector<PropertyRange> propertyRanges;
    J head = object(), targets = object(), assumes = object(), reset;
    J derived = list(), order = list();
    fs::path path;
    Product(const fs::path &, Files &);
    J premises(const std::string &label) const;
};
} // namespace veval::result_validation
