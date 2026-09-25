#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

namespace ex = stdexec;
using thread_pool = exec::static_thread_pool;

#include "pool_bench.hpp"

int main() { pool_bench::run_all(); }
