#include <lexec/execution.hpp>

#include <type_traits>

namespace ex = lexec;
using thread_pool = lexec::static_thread_pool;

#include "bulk_bench.hpp"

int main() { bulk_bench::run_on_pools(); }
