#pragma once

#include <lexec/core/completion_signatures.hpp>
#include <lexec/core/completion_tags.hpp>
#include <lexec/core/env.hpp>
#include <lexec/core/operation_state.hpp>
#include <lexec/core/queries.hpp>
#include <lexec/core/receiver.hpp>
#include <lexec/core/scheduler.hpp>
#include <lexec/core/sender.hpp>
#include <lexec/detail/config.hpp>
#include <lexec/detail/meta.hpp>
#include <lexec/stop_token.hpp>

#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

// Type-erased senders, receivers, and schedulers, after stdexec's exec::any_sender:
//
//   using int_sender = lexec::any_sender_of<lexec::set_value_t(int), lexec::set_stopped_t()>;
//   int_sender s = lexec::just(1);                        // any sender with those completions
//
//   using env_receiver = lexec::any_receiver<lexec::completion_signatures<lexec::set_value_t(int)>,
//                                            lexec::queries<int(answer_t) noexcept>>;
//   lexec::any_sender<env_receiver> t = lexec::read_env(answer);
//
//   lexec::any_scheduler<lexec::any_sender_of<lexec::set_value_t(), lexec::set_stopped_t()>> sch
//       = pool.get_scheduler();
//
// The erased sender sees an environment with the listed queries and get_stop_token, which
// is an inplace_stop_token. A small sender is stored inline; connecting always allocates the
// erased operation, whose type only the erased sender knows.

namespace lexec {

// The queries a type-erased environment answers, as signatures R(Query) noexcept.
template <class... Sigs>
struct queries {};

namespace detail {

template <class Sig>
struct query_signature {
    static_assert(dependent_false<Sig>, "lexec: a type-erased query must be spelled R(Query) noexcept");
};

template <class R, class Query>
struct query_signature<R(Query) noexcept> {
    using result = R;
    using query = Query;
};

template <class Query, class... Sigs>
struct find_query_signature {};

template <class Query, class Sig, class... Sigs>
struct find_query_signature<Query, Sig, Sigs...>
    : std::conditional_t<std::is_same_v<typename query_signature<Sig>::query, Query>, type_identity<Sig>,
                         find_query_signature<Query, Sigs...>> {};

// ---- the erased receiver ----

template <class Sig>
struct completion_entry;

template <class Tag, class... Args>
struct completion_entry<Tag(Args...)> {
    void (*fn)(void *, Args &&...) noexcept;

    template <class Target>
    static void call(void *const target, Args &&...args) noexcept {
        static_cast<Target *>(target)->template complete<Tag>(static_cast<Args &&>(args)...);
    }
};

template <class Sig>
struct query_entry {
    typename query_signature<Sig>::result (*fn)(void const *) noexcept;

    template <class Target>
    static typename query_signature<Sig>::result call(void const *const target) noexcept {
        using query = typename query_signature<Sig>::query;
        auto const &env = static_cast<Target const *>(target)->environment();
        static_assert(noexcept(query{}(env)), "lexec: a type-erased query must be answered without throwing");
        return query{}(env);
    }
};

template <class Sigs, class Queries>
struct receiver_vtable;

template <class... Sigs, class... Qs>
struct receiver_vtable<completion_signatures<Sigs...>, queries<Qs...>> : completion_entry<Sigs>..., query_entry<Qs>... {
    inplace_stop_token (*stop_token)(void const *) noexcept;

    template <class Target>
    static inplace_stop_token stop_token_of(void const *const target) noexcept {
        return static_cast<Target const *>(target)->stop_token();
    }
};

template <class Sigs, class Queries, class Target>
inline constexpr receiver_vtable<Sigs, Queries> receiver_vtable_for{};

template <class... Sigs, class... Qs, class Target>
inline constexpr receiver_vtable<completion_signatures<Sigs...>, queries<Qs...>>
    receiver_vtable_for<completion_signatures<Sigs...>, queries<Qs...>, Target>{
        {&completion_entry<Sigs>::template call<Target>}..., {&query_entry<Qs>::template call<Target>}...,
        &receiver_vtable<completion_signatures<Sigs...>, queries<Qs...>>::template stop_token_of<Target>};

template <class Sigs, class Queries>
struct erased_env;

template <class Sigs, class... Qs>
struct erased_env<Sigs, queries<Qs...>> {
    template <class Query, class Sig = typename find_query_signature<Query, Qs...>::type>
    typename query_signature<Sig>::result query(Query) const noexcept {
        return static_cast<query_entry<Sig> const &>(*vtable).fn(target);
    }

    inplace_stop_token query(get_stop_token_t) const noexcept { return vtable->stop_token(target); }

    receiver_vtable<Sigs, queries<Qs...>> const *vtable;
    void const *target;
};

// A non-owning receiver for the erased operation: a vtable and the operation it completes.
template <class Sigs, class Queries>
struct receiver_ref;

template <class... Sigs, class Queries>
struct receiver_ref<completion_signatures<Sigs...>, Queries> {
    using receiver_concept = receiver_t;
    using vtable_type = receiver_vtable<completion_signatures<Sigs...>, Queries>;

    template <class... Vs, std::enable_if_t<is_one_of_v<set_value_t(Vs...), Sigs...>, int> = 0>
    void set_value(Vs &&...vs) && noexcept {
        static_cast<completion_entry<set_value_t(Vs...)> const &>(*vtable).fn(target, static_cast<Vs &&>(vs)...);
    }

    template <class E, std::enable_if_t<is_one_of_v<set_error_t(E), Sigs...>, int> = 0>
    void set_error(E &&e) && noexcept {
        static_cast<completion_entry<set_error_t(E)> const &>(*vtable).fn(target, static_cast<E &&>(e));
    }

    template <class Sig = set_stopped_t(), std::enable_if_t<is_one_of_v<Sig, Sigs...>, int> = 0>
    void set_stopped() && noexcept {
        static_cast<completion_entry<set_stopped_t()> const &>(*vtable).fn(target);
    }

    erased_env<completion_signatures<Sigs...>, Queries> get_env() const noexcept { return {vtable, target}; }

    vtable_type const *vtable;
    void *target;
};

// Whether every completion of Sndr, in the erased environment, is one of Sigs.
template <class Sndr, class Sigs, class Queries, class = void>
inline constexpr bool erasable_sender_v = false;

template <class Sndr, class Sigs, class Queries>
inline constexpr bool erasable_sender_v<
    Sndr, Sigs, Queries, std::void_t<completion_signatures_of_t<Sndr, erased_env<Sigs, Queries>>>> =
    is_receiver_of_v<receiver_ref<Sigs, Queries>, completion_signatures_of_t<Sndr, erased_env<Sigs, Queries>>>;

// ---- the erased operation ----

struct erased_operation {
    virtual void start() noexcept = 0;
    virtual ~erased_operation() = default;
};

template <class Sndr, class Ref>
struct concrete_operation final : erased_operation {
    concrete_operation(Sndr &&sndr, Ref const ref) : op(lexec::connect(static_cast<Sndr &&>(sndr), ref)) {}

    void start() noexcept override { lexec::start(op); }

    connect_result_t<Sndr, Ref> op;
};

// The receiver's stop token as an inplace_stop_token: itself, none, or one of a source
// that the receiver's token requests stop on.
template <class Token, bool = std::is_same_v<Token, inplace_stop_token> or is_unstoppable_token_v<Token>>
struct stop_adapter {
    template <class Env>
    static inplace_stop_token token(Env const &env) noexcept {
        if constexpr (std::is_same_v<Token, inplace_stop_token>) {
            return lexec::get_stop_token(env);
        } else {
            static_cast<void>(env);
            return {};
        }
    }

    void attach(Token const &) noexcept {}
    void detach() noexcept {}
    inplace_stop_token token() const noexcept { return {}; }
    static constexpr bool passes_through = true;
};

template <class Token>
struct stop_adapter<Token, false> {
    struct forward {
        void operator()() noexcept { source->request_stop(); }
        inplace_stop_source *source;
    };

    using callback = stop_callback_for_t<Token, forward>;

    stop_adapter() noexcept {}
    stop_adapter(stop_adapter &&) = delete;
    ~stop_adapter() { detach(); }

    void attach(Token const &token_) noexcept {
        ::new (static_cast<void *>(storage)) callback(token_, forward{&source});
        attached = true;
    }

    void detach() noexcept {
        if (attached) {
            attached = false;
            std::launder(reinterpret_cast<callback *>(storage))->~callback();
        }
    }

    inplace_stop_token token() const noexcept { return source.get_token(); }

    static constexpr bool passes_through = false;

    inplace_stop_source source;
    alignas(callback) unsigned char storage[sizeof(callback)];
    bool attached = false;
};

// ---- the erased sender ----

inline constexpr std::size_t inline_sender_size = 4 * sizeof(void *);

template <class T>
inline constexpr bool stored_inline_v = sizeof(T) <= inline_sender_size and alignof(T) <= alignof(std::max_align_t) and
                                        std::is_nothrow_move_constructible_v<T>;

// Holds one object of an erased type, inline when small and cheaply movable, else on the
// heap.
struct erased_storage {
    template <class T>
    static T *get(erased_storage &storage) noexcept {
        if constexpr (stored_inline_v<T>) {
            return std::launder(reinterpret_cast<T *>(storage.buffer));
        } else {
            return *std::launder(reinterpret_cast<T **>(storage.buffer));
        }
    }

    template <class T>
    static T const *get(erased_storage const &storage) noexcept {
        if constexpr (stored_inline_v<T>) {
            return std::launder(reinterpret_cast<T const *>(storage.buffer));
        } else {
            return *std::launder(reinterpret_cast<T *const *>(storage.buffer));
        }
    }

    template <class T, class... Args>
    static void construct(erased_storage &storage, Args &&...args) {
        if constexpr (stored_inline_v<T>) {
            ::new (static_cast<void *>(storage.buffer)) T(static_cast<Args &&>(args)...);
        } else {
            ::new (static_cast<void *>(storage.buffer)) T *(new T(static_cast<Args &&>(args)...));
        }
    }

    template <class T>
    static void move(erased_storage &to, erased_storage &from) noexcept {
        if constexpr (stored_inline_v<T>) {
            ::new (static_cast<void *>(to.buffer)) T(static_cast<T &&>(*get<T>(from)));
            get<T>(from)->~T();
        } else {
            ::new (static_cast<void *>(to.buffer)) T *(get<T>(from));
        }
    }

    template <class T>
    static void destroy(erased_storage &storage) noexcept {
        if constexpr (stored_inline_v<T>) {
            get<T>(storage)->~T();
        } else {
            delete get<T>(storage);
        }
    }

    alignas(std::max_align_t) unsigned char buffer[inline_sender_size];
};

template <class Sigs, class Queries, class SenderQueries>
struct sender_vtable;

template <class Sigs, class Queries, class... SQs>
struct sender_vtable<Sigs, Queries, queries<SQs...>> : query_entry<SQs>... {
    void (*move)(erased_storage &, erased_storage &) noexcept;
    void (*destroy)(erased_storage &) noexcept;
    erased_operation *(*connect)(erased_storage &, receiver_ref<Sigs, Queries>);
};

// Answers the sender's own queries through the erased sender's vtable.
template <class Sndr>
struct sender_attrs_target {
    decltype(auto) environment() const noexcept { return lexec::get_env(*sndr); }

    Sndr const *sndr;
};

template <class Sigs, class Queries, class SenderQueries, class Sndr>
struct sender_vtable_for;

template <class Sigs, class Queries, class... SQs, class Sndr>
struct sender_vtable_for<Sigs, Queries, queries<SQs...>, Sndr> {
    template <class Sig>
    static typename query_signature<Sig>::result query(void const *const storage) noexcept {
        auto const target = sender_attrs_target<Sndr>{erased_storage::get<Sndr>(*static_cast<erased_storage const *>(storage))};
        return query_entry<Sig>::template call<sender_attrs_target<Sndr>>(&target);
    }

    static erased_operation *connect(erased_storage &storage, receiver_ref<Sigs, Queries> const ref) {
        using operation = concrete_operation<Sndr, receiver_ref<Sigs, Queries>>;
        return new operation(static_cast<Sndr &&>(*erased_storage::get<Sndr>(storage)), ref);
    }

    static constexpr sender_vtable<Sigs, Queries, queries<SQs...>> value{
        {&query<SQs>}..., &erased_storage::move<Sndr>, &erased_storage::destroy<Sndr>, &connect};
};

template <class Sigs, class Queries, class Rcvr>
struct any_operation {
    using operation_state_concept = operation_state_t;
    using token_type = stop_token_of_t<env_of_t<Rcvr>>;
    using ref = receiver_ref<Sigs, Queries>;

    template <class Vtable>
    any_operation(Vtable const *const sender, erased_storage &storage, Rcvr &&rcvr_)
        : rcvr(static_cast<Rcvr &&>(rcvr_)),
          inner(sender->connect(storage, ref{&receiver_vtable_for<Sigs, Queries, any_operation>, this})) {}

    any_operation(any_operation &&) = delete;

    void start() & noexcept {
        if constexpr (not stop_adapter<token_type>::passes_through) {
            stop.attach(lexec::get_stop_token(lexec::get_env(rcvr)));
        }
        inner->start();
    }

    template <class Tag, class... Args>
    void complete(Args &&...args) noexcept {
        stop.detach();
        Tag{}(static_cast<Rcvr &&>(rcvr), static_cast<Args &&>(args)...);
    }

    decltype(auto) environment() const noexcept { return lexec::get_env(rcvr); }

    inplace_stop_token stop_token() const noexcept {
        if constexpr (stop_adapter<token_type>::passes_through) {
            return stop_adapter<token_type>::token(lexec::get_env(rcvr));
        } else {
            return stop.token();
        }
    }

    Rcvr rcvr;
    stop_adapter<token_type> stop;
    std::unique_ptr<erased_operation> inner;
};

template <class Sigs, class Queries, class SenderQueries>
struct erased_attrs;

template <class Sigs, class Queries, class... SQs>
struct erased_attrs<Sigs, Queries, queries<SQs...>> {
    template <class Query, class Sig = typename find_query_signature<Query, SQs...>::type>
    typename query_signature<Sig>::result query(Query) const noexcept {
        return static_cast<query_entry<Sig> const &>(*vtable).fn(storage);
    }

    sender_vtable<Sigs, Queries, queries<SQs...>> const *vtable;
    erased_storage const *storage;
};

} // namespace detail

template <class Sigs, class Queries = queries<>>
struct any_receiver {
    using completion_signatures = Sigs;
    using queries_type = Queries;
};

template <class AnyReceiver, class SenderQueries = queries<>>
class any_sender {
    using sigs = typename AnyReceiver::completion_signatures;
    using receiver_queries = typename AnyReceiver::queries_type;
    using vtable = detail::sender_vtable<sigs, receiver_queries, SenderQueries>;

public:
    using sender_concept = sender_t;
    using completion_signatures = sigs;

    template <class Sndr, class S = detail::remove_cvref_t<Sndr>,
              std::enable_if_t<not std::is_same_v<S, any_sender> and is_sender_v<S> and
                                   detail::erasable_sender_v<S, sigs, receiver_queries>,
                               int> = 0>
    any_sender(Sndr &&sndr) : table(&detail::sender_vtable_for<sigs, receiver_queries, SenderQueries, S>::value) {
        detail::erased_storage::construct<S>(storage, static_cast<Sndr &&>(sndr));
    }

    any_sender(any_sender &&other) noexcept : table(std::exchange(other.table, nullptr)) {
        if (table != nullptr) {
            table->move(storage, other.storage);
        }
    }

    any_sender &operator=(any_sender &&other) noexcept {
        if (this != &other) {
            reset();
            table = std::exchange(other.table, nullptr);
            if (table != nullptr) {
                table->move(storage, other.storage);
            }
        }
        return *this;
    }

    ~any_sender() { reset(); }

    // Connecting consumes the erased sender.
    template <class Rcvr>
    detail::any_operation<sigs, receiver_queries, Rcvr> connect(Rcvr rcvr) && {
        static_assert(is_receiver_of_v<Rcvr, sigs>, "lexec::any_sender: the receiver does not accept every completion");
        return {table, storage, static_cast<Rcvr &&>(rcvr)};
    }

    detail::erased_attrs<sigs, receiver_queries, SenderQueries> get_env() const noexcept { return {table, &storage}; }

private:
    void reset() noexcept {
        if (table != nullptr) {
            std::exchange(table, nullptr)->destroy(storage);
        }
    }

    vtable const *table;
    detail::erased_storage storage;
};

template <class... Sigs>
using any_sender_of = any_sender<any_receiver<completion_signatures<Sigs...>>>;

template <class AnySender, class SchedulerQueries = queries<>>
class any_scheduler;

namespace detail {

template <class AnySender>
struct scheduler_vtable {
    void (*copy)(erased_storage &, erased_storage const &);
    void (*move)(erased_storage &, erased_storage &) noexcept;
    void (*destroy)(erased_storage &) noexcept;
    AnySender (*schedule)(erased_storage const &);
    bool (*equal)(erased_storage const &, erased_storage const &) noexcept;
};

template <class AnySender, class Sch>
struct scheduler_vtable_for {
    static void copy(erased_storage &to, erased_storage const &from) {
        erased_storage::construct<Sch>(to, *erased_storage::get<Sch>(from));
    }

    static AnySender schedule(erased_storage const &storage) { return AnySender(lexec::schedule(*erased_storage::get<Sch>(storage))); }

    static bool equal(erased_storage const &lhs, erased_storage const &rhs) noexcept {
        return *erased_storage::get<Sch>(lhs) == *erased_storage::get<Sch>(rhs);
    }

    static constexpr scheduler_vtable<AnySender> value{&copy, &erased_storage::move<Sch>, &erased_storage::destroy<Sch>,
                                                       &schedule, &equal};
};

template <class Scheduler>
struct schedule_attrs_of {
    Scheduler query(get_completion_scheduler_t<set_value_t>) const noexcept { return sch; }

    Scheduler sch;
};

// schedule() on an any_scheduler: the erased schedule sender, reporting the any_scheduler
// as where it completes.
template <class Scheduler, class AnySender>
struct any_schedule_sender {
    using sender_concept = sender_t;
    using completion_signatures = typename AnySender::completion_signatures;

    template <class Rcvr>
    auto connect(Rcvr rcvr) && {
        return static_cast<AnySender &&>(sndr).connect(static_cast<Rcvr &&>(rcvr));
    }

    auto get_env() const noexcept { return env<schedule_attrs_of<Scheduler>, env_of_t<AnySender const &>>{{sch}, sndr.get_env()}; }

    Scheduler sch;
    AnySender sndr;
};

} // namespace detail

// A scheduler of any type whose schedule sender AnySender can hold. Two of them are
// equal if they hold schedulers of the same type that compare equal.
template <class AnySender, class SchedulerQueries>
class any_scheduler {
    using vtable = detail::scheduler_vtable<AnySender>;

public:
    using scheduler_concept = scheduler_t;

    template <class Sch, class S = detail::remove_cvref_t<Sch>,
              std::enable_if_t<not std::is_same_v<S, any_scheduler> and is_scheduler_v<S>, int> = 0>
    any_scheduler(Sch &&sch) : table(&detail::scheduler_vtable_for<AnySender, S>::value) {
        detail::erased_storage::construct<S>(storage, static_cast<Sch &&>(sch));
    }

    any_scheduler(any_scheduler const &other) : table(other.table) { table->copy(storage, other.storage); }

    any_scheduler(any_scheduler &&other) noexcept : table(std::exchange(other.table, nullptr)) {
        if (table != nullptr) {
            table->move(storage, other.storage);
        }
    }

    any_scheduler &operator=(any_scheduler other) noexcept {
        reset();
        table = std::exchange(other.table, nullptr);
        if (table != nullptr) {
            table->move(storage, other.storage);
        }
        return *this;
    }

    ~any_scheduler() { reset(); }

    detail::any_schedule_sender<any_scheduler, AnySender> schedule() const {
        return {*this, table->schedule(storage)};
    }

    friend bool operator==(any_scheduler const &lhs, any_scheduler const &rhs) noexcept {
        return lhs.table == rhs.table and lhs.table->equal(lhs.storage, rhs.storage);
    }
    friend bool operator!=(any_scheduler const &lhs, any_scheduler const &rhs) noexcept { return not(lhs == rhs); }

private:
    void reset() noexcept {
        if (table != nullptr) {
            std::exchange(table, nullptr)->destroy(storage);
        }
    }

    vtable const *table;
    detail::erased_storage storage;
};

} // namespace lexec
