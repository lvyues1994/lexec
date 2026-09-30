#include <exec/repeat_n.hpp>
#include <exec/repeat_until.hpp>
#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

namespace ex = stdexec;
namespace loops = exec;
using thread_pool = exec::static_thread_pool;

#include "loop_bench.hpp"

int main() { loop_bench::run_all(); }
