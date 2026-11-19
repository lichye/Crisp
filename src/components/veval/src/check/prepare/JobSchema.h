#pragma once
#include "../../util/task/CommandArgumentsAndFiles.h"
#include "check/prepare/ReadLocalSpec.h"
namespace veval::verification {
void validateShape(const J &, const std::string &schema);
void validateJob(const J &);
J jobPremises(const Product &, const std::string &label);
J verifyProduct(const J &, const Product &);
}
