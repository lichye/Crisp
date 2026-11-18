#pragma once
#include "../../util/database/Json.h"
#include <set>

namespace veval::result_validation {
// The boundary is the one original compilation input replaced by the product.
// Resolve old relative headers against that recorded input, not the caller's cwd.
struct SourcePaths {
    fs::path boundary, directory;
    SourcePaths(const J &configuration, const J &job, const J &head) {
        std::set<fs::path> design;
        for(const auto &input:items(job["inputs"]))
            if(text(input["role"])=="design"||text(input["role"])=="harness")
                design.insert(fs::absolute(text(input["path"])).lexically_normal());
        size_t count=0;
        for(const auto &file:items(configuration["frontend"]["file_order"])) {
            auto path=fs::absolute(text(file)).lexically_normal();
            if(!design.count(path)){boundary=path;++count;}
        }
        if(count!=1)throw std::runtime_error("original boundary input absent or ambiguous");
        fs::path spelling=text(head["boundary_spec"]);
        if(spelling.empty())throw std::runtime_error("original boundary header absent");
        directory=fs::current_path();
        if(fs::absolute(spelling).lexically_normal()==boundary)return;
        if(spelling.is_absolute())throw std::runtime_error("boundary header differs from original input");
        directory=boundary;
        for(const auto &part:spelling) {
            if(part==".")continue;
            if(part=="..")throw std::runtime_error("relative boundary requires recorded working directory");
            directory=directory.parent_path();
        }
        if((directory/spelling).lexically_normal()!=boundary)
            throw std::runtime_error("relative boundary differs from original input");
    }
    std::string resolve(const std::string &value) const {
        fs::path path=value;
        return (path.is_absolute()?path:directory/path).lexically_normal().string();
    }
};
}
