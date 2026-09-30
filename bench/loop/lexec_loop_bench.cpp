#include <lexec/execution.hpp>

namespace ex = lexec;
namespace loops = lexec;
using thread_pool = lexec::static_thread_pool;

#include "loop_bench.hpp"

int main() { loop_bench::run_all(); }
