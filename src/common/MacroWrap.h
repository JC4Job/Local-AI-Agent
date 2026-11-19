#pragma once
#include "common/Utils.hpp"

/*
 * WARNING: upper case on purpose: a function-like macro named `print` would also rewrite every later
 * `TextUtilities::print(` and `std::print(` in the program.
 */
#ifndef PRINT
#define PRINT(s) TextUtilities::print(s)
#endif
