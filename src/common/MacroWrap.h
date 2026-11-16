#pragma once
#include "common/Utils.hpp"

/*
    Upper case on purpose: a function-like macro named `PRINT` would also rewrite every later
    `TextUtilities::PRINT(` and `std::PRINT(` in the program.
*/
#ifndef PRINT
#define PRINT(s) TextUtilities::print(s)
#endif
