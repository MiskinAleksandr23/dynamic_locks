#pragma once

#include <stdexcept>

#define Check(cond) if (!(cond)) throw std::runtime_error(#cond);

#define CheckIfTrue(cond1, cond2) if (cond1) Check(cond2)

#define NotImplemented throw std::runtime_error("not implemented");
