#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include <type_traits>

namespace ex = stdexec;
using thread_pool = exec::static_thread_pool;

#include "bulk_bench.hpp"

int main() { bulk_bench::run_on_pools(); }
