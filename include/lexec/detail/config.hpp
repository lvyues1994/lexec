#pragma once

// MSVC reports the language version in _MSVC_LANG unless /Zc:__cplusplus is given.
#if defined(_MSVC_LANG)
#define LEXEC_CPLUSPLUS _MSVC_LANG
#else
#define LEXEC_CPLUSPLUS __cplusplus
#endif

#if LEXEC_CPLUSPLUS < 201703L
#error "lexec requires C++17 or later"
#endif

// Without /permissive-, MSVC treats `and`, `or`, and `not` as keywords only through this header.
#if defined(_MSC_VER) && !defined(__clang__)
#include <iso646.h>
#endif

#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
#define LEXEC_HAS_EXCEPTIONS 1
#else
#define LEXEC_HAS_EXCEPTIONS 0
#endif

// GCC and Clang accept the C++20 attribute in C++17 mode as an extension; MSVC ignores it
// and has its own spelling.
#if defined(_MSC_VER) && !defined(__clang__)
#define LEXEC_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#elif defined(__has_cpp_attribute)
#if __has_cpp_attribute(no_unique_address)
#define LEXEC_NO_UNIQUE_ADDRESS [[no_unique_address]]
#endif
#endif
#ifndef LEXEC_NO_UNIQUE_ADDRESS
#define LEXEC_NO_UNIQUE_ADDRESS
#endif

// For members holding operation states constructed in place from connect's prvalue.
// At /O2, MSVC writes past the enclosing object when such a member is
// [[msvc::no_unique_address]] (C4789).
#if defined(_MSC_VER) && !defined(__clang__)
#define LEXEC_IMMOVABLE_NO_UNIQUE_ADDRESS
#else
#define LEXEC_IMMOVABLE_NO_UNIQUE_ADDRESS LEXEC_NO_UNIQUE_ADDRESS
#endif
