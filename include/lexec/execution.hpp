#pragma once

#include <lexec/detail/config.hpp>

#include <lexec/execution_policy.hpp>
#include <lexec/stop_token.hpp>

#include <lexec/core/completion_signatures.hpp>
#include <lexec/core/completion_tags.hpp>
#include <lexec/core/domain.hpp>
#include <lexec/core/env.hpp>
#include <lexec/core/operation_state.hpp>
#include <lexec/core/queries.hpp>
#include <lexec/core/receiver.hpp>
#include <lexec/core/scheduler.hpp>
#include <lexec/core/sender.hpp>
#include <lexec/core/sender_traits.hpp>
#include <lexec/core/transform_sender.hpp>

#include <lexec/framework/basic_sender.hpp>
#include <lexec/framework/sender_adaptor_closure.hpp>

#include <lexec/algorithms/bulk.hpp>
#include <lexec/algorithms/continues_on.hpp>
#include <lexec/algorithms/into_variant.hpp>
#include <lexec/algorithms/just.hpp>
#include <lexec/algorithms/let.hpp>
#include <lexec/algorithms/read_env.hpp>
#include <lexec/algorithms/starts_on.hpp>
#include <lexec/algorithms/stopped_as.hpp>
#include <lexec/algorithms/sync_wait.hpp>
#include <lexec/algorithms/then.hpp>
#include <lexec/algorithms/when_all.hpp>
#include <lexec/algorithms/write_env.hpp>

#include <lexec/schedulers/inline_scheduler.hpp>
#include <lexec/schedulers/run_loop.hpp>
#include <lexec/schedulers/static_thread_pool.hpp>
