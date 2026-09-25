#include <lexec/execution.hpp>

namespace ex = lexec;
using thread_pool = lexec::static_thread_pool;

#include "pool_bench.hpp"

int main() { pool_bench::run_all(); }
