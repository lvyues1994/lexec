#pragma once

#include <cstddef>

namespace lexec_test {

// Number of global operator new calls so far in this process.
std::size_t allocation_count() noexcept;

} // namespace lexec_test
