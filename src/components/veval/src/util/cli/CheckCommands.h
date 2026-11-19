#pragma once
#include <util/irep.h>

namespace veval::workflow {
int main(int argc, char **argv);
int prepareSources(int argc, char **argv);
int executeOwned(int argc, char **argv);
int superviseCommand(int argc, char **argv);
bool containsFormalStatement(const irept &);
}
