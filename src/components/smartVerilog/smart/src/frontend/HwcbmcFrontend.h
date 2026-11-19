// SMART's input metadata from the shared EBMC Verilog frontend.

#ifndef SMART_FRONTEND_HWCBMCFRONTEND_H
#define SMART_FRONTEND_HWCBMCFRONTEND_H

#include <string>
#include <vector>

#include "SVModule.h"

namespace smart {
namespace frontend {

struct HwcbmcOptions {
    // Additional design files to elaborate together with the main one (the
    // plan's multi-file case). The directory of the main file is scanned for
    // sibling `.sv`/`.v` files when `autoDiscoverSiblings` is set.
    std::vector<std::string> extraFiles;
    bool autoDiscoverSiblings = true;

    // `-I` paths for the Verilog preprocessor. The directory holding the
    // design is always searched, so `` `include "ibex_pkg.sv" `` resolves.
    std::vector<std::string> includePaths;
    // Extra `-D` defines, "NAME" or "NAME=VALUE".
    std::vector<std::string> defines;

};

struct InstanceFreeReg {
    std::string instance;  // Dotted path below the top.
    FreeReg reg;
};

// Parse and elaborate the design, reporting errors to the caller.
ModuleInfo parseModuleFile(const std::string& path, const std::string& top,
                           const HwcbmcOptions& options = {},
                           std::vector<InstanceFreeReg>* instanceRegs = nullptr);

// Does `path` declare a module called `name`? A cheap textual scan, used to
// keep a file that redeclares the top module — such as this tool's own
// previous output — out of a design's file list.
bool declaresModule(const std::string& path, const std::string& name);

}  // namespace frontend
}  // namespace smart

#endif  // SMART_FRONTEND_HWCBMCFRONTEND_H
