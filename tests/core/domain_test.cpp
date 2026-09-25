#include <lexec/execution.hpp>

#include <doctest/doctest.h>

#include <type_traits>

namespace {

using lexec::default_domain;
using lexec::get_completion_domain_t;
using lexec::set_error_t;
using lexec::set_value_t;

struct test_domain {};

// A scheduler that names its own domain; nothing here schedules on it.
struct domain_scheduler {
    using scheduler_concept = lexec::scheduler_t;

    test_domain query(get_completion_domain_t<set_value_t>) const noexcept { return {}; }
};

[[maybe_unused]] auto const domain_env = lexec::prop{lexec::get_domain, test_domain{}};
using domain_env_t = decltype(domain_env);

static_assert(std::is_same_v<decltype(lexec::get_domain(lexec::env<>{})), default_domain>);
static_assert(std::is_same_v<decltype(lexec::get_domain(domain_env)), test_domain>);
static_assert(std::is_same_v<decltype(lexec::get_domain(lexec::prop{lexec::get_scheduler, domain_scheduler{}})),
                             test_domain>);

// just completes wherever it is started, so its completion domain is the start's.
using just_attrs = lexec::env_of_t<decltype(lexec::just(1))>;
static_assert(std::is_same_v<decltype(lexec::get_completion_domain<set_value_t>(just_attrs{}, domain_env)), test_domain>);
static_assert(std::is_same_v<decltype(lexec::get_completion_domain<>(just_attrs{}, domain_env)), test_domain>);
static_assert(not std::is_invocable_v<get_completion_domain_t<set_value_t>, just_attrs>);
static_assert(not std::is_invocable_v<get_completion_domain_t<set_error_t>, just_attrs, domain_env_t>);

[[maybe_unused]] auto const identity = [](int v) noexcept { return v; };
[[maybe_unused]] auto const just_again = [](int &v) noexcept { return lexec::just(v); };

// then completes where its predecessor does.
using then_attrs = lexec::env_of_t<decltype(lexec::just(1) | lexec::then(identity))>;
static_assert(std::is_same_v<decltype(lexec::get_completion_domain<>(then_attrs{}, domain_env)), test_domain>);

// let completes where its second sender does, which starts in the predecessor's domain.
using let_attrs = lexec::env_of_t<decltype(lexec::just(1) | lexec::let_value(just_again))>;
static_assert(std::is_same_v<decltype(lexec::get_completion_domain<>(let_attrs{}, domain_env)), test_domain>);

// when_all completes where its children do.
using when_all_attrs = lexec::env_of_t<decltype(lexec::when_all(lexec::just(1), lexec::just(2)))>;
static_assert(std::is_same_v<decltype(lexec::get_completion_domain<>(when_all_attrs{}, domain_env)), test_domain>);

// COMMON-DOMAIN: the one domain all are, or an indeterminate domain listing each once;
// indeterminate_domain<> means no information and joins with anything.
struct other_domain {};
static_assert(std::is_same_v<lexec::common_domain_t<test_domain, test_domain>, test_domain>);
static_assert(std::is_same_v<lexec::common_domain_t<test_domain, other_domain, test_domain>,
                             lexec::indeterminate_domain<test_domain, other_domain>>);
static_assert(std::is_same_v<lexec::common_domain_t<lexec::indeterminate_domain<>, test_domain>, test_domain>);
static_assert(std::is_same_v<lexec::common_domain_t<lexec::indeterminate_domain<test_domain, other_domain>, other_domain>,
                             lexec::indeterminate_domain<test_domain, other_domain>>);

// Without the environment it will be started in, an inline sender cannot tell where it completes.
static_assert(not std::is_invocable_v<lexec::get_completion_scheduler_t<set_value_t>, just_attrs>);

} // namespace

TEST_CASE("get_completion_scheduler of an inline sender is the scheduler it is started on") {
    auto loop = lexec::run_loop{};
    auto const scheduler = loop.get_scheduler();
    auto const env = lexec::prop{lexec::get_scheduler, scheduler};
    auto const reported = lexec::get_completion_scheduler<set_value_t>(lexec::get_env(lexec::just(1)), env);
    auto const is_started_scheduler = reported == scheduler;
    CHECK(is_started_scheduler);
}

TEST_CASE("a scheduler asked with an environment reports itself as its completion scheduler") {
    auto loop = lexec::run_loop{};
    auto const scheduler = loop.get_scheduler();
    auto const reports_itself = lexec::get_completion_scheduler<set_value_t>(scheduler, lexec::env<>{}) == scheduler;
    auto const schedule_reports_it =
        lexec::get_completion_scheduler<set_value_t>(lexec::get_env(lexec::schedule(scheduler))) == scheduler;
    CHECK(reports_itself);
    CHECK(schedule_reports_it);
}
