#pragma once

#if __cplusplus < 201703L
#error "lexec requires C++17 or later"
#endif

#if defined(__cpp_exceptions)
#define LEXEC_HAS_EXCEPTIONS 1
#else
#define LEXEC_HAS_EXCEPTIONS 0
#endif

// GCC and Clang accept the C++20 attribute in C++17 mode as an extension.
#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(no_unique_address)
#define LEXEC_NO_UNIQUE_ADDRESS [[no_unique_address]]
#endif
#endif
#ifndef LEXEC_NO_UNIQUE_ADDRESS
#define LEXEC_NO_UNIQUE_ADDRESS
#endif
