#pragma once
#include "behaviors.hpp"
#include "storage.hpp"
#include "entity.hpp"
#include "detail/static_plan.hpp"
#include <hlog.h>
#include <cassert>
#include <cstddef>
#include <new>

namespace hlog {
class Logger;
}

namespace reg {
class RegisterMap;
namespace detail {
class ViewCompiler;
struct WindowTarget;
class FormulaInstaller;
class FormulaDecoder;
} // namespace detail
class Groups;
class RegScope;
class RegLayout;
class Bundle;
template <class T, std::size_t Rank, bool ValueAccess>
class RegView;
namespace detail {
struct Access;

struct ScalarAccessSite {
    std::uint64_t target{};
    AccessRoute route;
    AliasAccessPolicy policy;
    std::optional<std::uint32_t> instance, callback_instance;
    std::string_view view_name;
    const RegElementIndex* callback_index{};
    bool used{};
};
struct ScalarAccessFrame;
struct MappedIndexSite {
    const void* source{};
    const void* output{};
    std::string_view source_name, output_name;
    unsigned dimension{};
};
struct MappedAccessFrame {
    const ScalarAccessSite* source{};
    const ScalarAccessSite* target{};
    const MappedIndexSite* mapping{};
    const AccessIdentity* identity{};
    const RegElementIndex* request{};
    RegData mapped{}, logical{};
    std::uint32_t linear{};
    bool has_mapped{}, index_valid{};
    std::string_view group;
    AccessRoute route;
    AliasAccessPolicy policy;
    std::optional<std::uint32_t> instance;
    bool used{};
    std::array<StoredField, MAX_REG_ACCESS_FIELDS>* index_scratch{};
    std::array<StoredField, MAX_REG_ACCESS_FIELDS>* mapped_scratch{};
    void fill(RegAccessInfo&,
              const ScalarAccessFrame&,
              StoredField* logical = nullptr,
              StoredField* physical = nullptr) const;
};
struct ScalarAccessFrame {
    const ScalarAccessSite* site;
    std::uint64_t address;
    RegData raw, before, after;
    RegTransactionKind kind;
    const MappedAccessFrame* mapped{};
    template <class F>
    REGFW_DETAIL_ALWAYS_INLINE decltype(auto) with_info(F&& callback) const {
        RegAccessInfo info;
        StoredField logical, physical;
        if (mapped)
            mapped->fill(info, *this, &logical, &physical);
        else
            fill(info);
        return std::forward<F>(callback)(info);
    }
    REGFW_DETAIL_ALWAYS_INLINE void fill(RegAccessInfo& i) const {
        if (mapped) {
            mapped->fill(i, *this);
            return;
        }
        i.kind = kind;
        i.address = address;
        i.alias_resolved_address = i.resolved_address = site->target;
        i.size = 4;
        i.register_width_bits = 32;
        i.bus_mask = 0xffffffff;
        i.request_index = i.index = *site->callback_index;
        i.raw_data = i.bus_data = raw;
        i.stored_before = before;
        i.stored_after = after;
        i.route = site->route;
        i.alias_access_policy = site->policy;
        i.source_instance = site->callback_instance;
        i.view_path = site->view_name;
        i.is_alias = site->used;
    }
};

REGFW_DETAIL_ALWAYS_INLINE inline void MappedAccessFrame::fill(RegAccessInfo& i,
                                                               const ScalarAccessFrame& f,
                                                               StoredField* logical,
                                                               StoredField* physical) const {
    i.kind = f.kind;
    i.address = f.address;
    i.alias_resolved_address = source->target;
    i.resolved_address = target->target;
    i.size = 4;
    i.internal_byte_offset = 0;
    i.register_width_bits = 32;
    i.wide_values = nullptr;
    i.bus_mask = 0xffffffff;
    i.request_index = *request;
    i.index.linear_value = mapping && has_mapped && index_valid ? linear : request->linear_value;
    i.index.rank_value = request->rank_value;
    [&]<std::size_t... D>(std::index_sequence<D...>) {
        ((i.index.values[D] = mapping && has_mapped && index_valid && mapping->dimension == D
                                  ? static_cast<std::uint32_t>(mapped)
                                  : request->values[D]),
         ...);
    }(std::make_index_sequence<MAX_REG_DIMS>{});
    i.raw_data = i.bus_data = f.raw;
    i.stored_before = f.before;
    i.stored_after = f.after;
    i.route = route;
    i.alias_access_policy = policy;
    i.source_instance = instance;
    i.view_path = target->view_name;
    i.is_alias = used;
    i.access_group = group;
    if (identity) {
        i.request_identity = identity->request_id;
        i.resolved_identity = identity->resolved_id;
        i.identity_domain = identity->domain;
        i.identity_mode = identity->mode;
        i.identity_source = identity->source;
    } else {
        i.request_identity.reset();
        i.resolved_identity.reset();
        i.identity_domain.reset();
        i.identity_mode.reset();
        i.identity_source.reset();
    }
    i.index_fields.entries.owned.reset();
    i.mapped_fields.entries.owned.reset();
    i.index_fields.entries.values = logical ? logical : index_scratch->data();
    i.mapped_fields.entries.values = physical ? physical : mapped_scratch->data();
    i.index_fields.size = i.mapped_fields.size = 0;
    if (mapping) {
        i.index_fields.entries[0] = {
            mapping->source, mapping->source_name, this->logical, mapping->dimension};
        i.index_fields.size = 1;
        if (has_mapped) {
            i.mapped_fields.entries[0] = {
                mapping->output, mapping->output_name, mapped, mapping->dimension};
            i.mapped_fields.size = 1;
        }
    }
}

// One compiled owner for every event callable. Small captures are built
// directly in aligned inline storage; only invocation depends on F.
class EventCallback final {
  public:
    using Invoke = void (*)(void*, const void*, const RegAccessInfo&);
    using Destroy = void (*)(void*) noexcept;
    using FrameInvoke = void (*)(void*, RegData, const ScalarAccessFrame&);

  private:
    static constexpr std::size_t INLINE_BYTES = 2 * sizeof(void*);
    alignas(std::max_align_t) std::byte inline_storage_[INLINE_BYTES];
    void* target_{};
    std::size_t alignment_{};
    Invoke invoke_{};
    FrameInvoke frame_invoke_{};
    Destroy destroy_{};
    bool constructed_{};

  public:
    EventCallback(std::size_t bytes,
                  std::size_t alignment,
                  Invoke invoke,
                  Destroy destroy,
                  FrameInvoke frame = nullptr);
    ~EventCallback();
    EventCallback(const EventCallback&) = delete;
    EventCallback& operator=(const EventCallback&) = delete;
    void* target_memory() noexcept {
        return target_;
    }
    void mark_constructed() noexcept {
        constructed_ = true;
    }
    void from_frame(RegData value, const ScalarAccessFrame& frame) const {
        if (frame_invoke_) {
            frame_invoke_(target_, value, frame);
            return;
        }
        RegAccessInfo i;
        frame.fill(i);
        invoke_(target_, &value, i);
    }
    void operator()(RegData value, const RegAccessInfo& info) const {
        invoke_(target_, &value, info);
    }
    void operator()(const WideRegData& value, const RegAccessInfo& info) const {
        invoke_(target_, &value, info);
    }
};
std::shared_ptr<EventCallback> allocate_event(std::size_t bytes,
                                              std::size_t alignment,
                                              EventCallback::Invoke invoke,
                                              EventCallback::Destroy destroy,
                                              EventCallback::FrameInvoke frame = nullptr);
template <class F>
std::shared_ptr<const EventCallback>
bind_event(F&& f, EventCallback::Invoke invoke, EventCallback::FrameInvoke frame = nullptr) {
    using Callable = std::remove_cvref_t<F>;
    EventCallback::Destroy destroy = nullptr;
    if constexpr (!std::is_trivially_destructible_v<Callable>)
        destroy = [](void* p) noexcept {
            static_cast<Callable*>(p)->~Callable();
        };
    auto event = allocate_event(sizeof(Callable), alignof(Callable), invoke, destroy, frame);
    ::new (event->target_memory()) Callable(std::forward<F>(f));
    event->mark_constructed();
    return event;
}
// Inspect only a non-generic callable's exact signature. Probing a generic
// lambda with raw info can make its typed index.get() body ill-formed.
template <class Signature>
struct CommonInfoMethod : std::false_type {};
#define REGFW_DETAIL_COMMON_INFO_METHOD(QUALIFIERS)                                                \
    template <class C, class R>                                                                    \
    struct CommonInfoMethod<R (C::*)(const RegAccessInfo&) QUALIFIERS> : std::true_type {          \
        using Result = R;                                                                          \
    };                                                                                             \
    template <class C, class R>                                                                    \
    struct CommonInfoMethod<R (C::*)(RegAccessInfo) QUALIFIERS> : std::true_type {                 \
        using Result = R;                                                                          \
    }
REGFW_DETAIL_COMMON_INFO_METHOD();
REGFW_DETAIL_COMMON_INFO_METHOD(const);
REGFW_DETAIL_COMMON_INFO_METHOD(noexcept);
REGFW_DETAIL_COMMON_INFO_METHOD(const noexcept);
REGFW_DETAIL_COMMON_INFO_METHOD(&);
REGFW_DETAIL_COMMON_INFO_METHOD(const&);
REGFW_DETAIL_COMMON_INFO_METHOD(& noexcept);
REGFW_DETAIL_COMMON_INFO_METHOD(const& noexcept);
#undef REGFW_DETAIL_COMMON_INFO_METHOD
template <class F, class = void>
struct CommonInfoCallable : std::false_type {};
template <class F>
struct CommonInfoCallable<F, std::void_t<decltype(&F::operator())>>
    : CommonInfoMethod<decltype(&F::operator())> {};
template <class R>
struct CommonInfoCallable<R (*)(const RegAccessInfo&), void> : std::true_type {
    using Result = R;
};
template <class R>
struct CommonInfoCallable<R (*)(RegAccessInfo), void> : std::true_type {
    using Result = R;
};
template <class R>
struct CommonInfoCallable<R (*)(const RegAccessInfo&) noexcept, void> : std::true_type {
    using Result = R;
};
template <class R>
struct CommonInfoCallable<R (*)(RegAccessInfo) noexcept, void> : std::true_type {
    using Result = R;
};
template <class F>
void invoke_common_event(void* target, const void*, const RegAccessInfo& info) {
    (*static_cast<F*>(target))(info);
}
template <class F>
std::shared_ptr<const EventCallback> bind_pre_access(F f) {
    // Full and scalar-frame entry points share one callable, including its
    // mutable state. Materialize only the info fields used by this hook.
    if constexpr (std::is_pointer_v<F> || std::same_as<F, std::nullptr_t>) {
        // Preserve std::function's empty/null-target behavior: invocation
        // throws and follows the normal hook-failure path.
        return bind_pre_access(std::function<void(const RegAccessInfo&)>(std::move(f)));
    } else {
        return bind_event(std::move(f),
                          invoke_common_event<F>,
                          [](void* target, RegData, const ScalarAccessFrame& frame) {
                              frame.with_info([&](const RegAccessInfo& info) {
                                  std::invoke(*static_cast<F*>(target), info);
                              });
                          });
    }
}
template <class Info, class F>
void invoke_info_event(void* target, const void*, const RegAccessInfo& info) {
    std::invoke(*static_cast<F*>(target), Info(info));
}
template <class F>
void invoke_empty_event(void* target, const void*, const RegAccessInfo&) {
    std::invoke(*static_cast<F*>(target));
}
template <class T, class Info, class F>
void invoke_value_event(void* target, const void* value, const RegAccessInfo& info) {
    using Data = std::conditional_t<wide_value_v<T>, WideRegData, RegData>;
    std::invoke(
        *static_cast<F*>(target), value_from_data<T>(*static_cast<const Data*>(value)), Info(info));
}

struct EntityState;
struct RegViewLayout;
enum class EventKind { ReadComplete, Write };

template <class T, class Info, class F>
void invoke_frame_event(void* target, RegData value, const ScalarAccessFrame& frame) {
    frame.with_info([&](const RegAccessInfo& i) {
        auto& f = *static_cast<F*>(target);
        if constexpr (CommonInfoCallable<F>::value)
            f(i);
        else if constexpr (std::is_invocable_v<F&, T, const Info&>)
            std::invoke(f, value_from_data<T>(value), Info(i, TrustedAccessTag{}));
        else if constexpr (std::is_invocable_v<F&, const Info&>)
            std::invoke(f, Info(i, TrustedAccessTag{}));
        else
            std::invoke(f);
    });
}
class ScalarReadSource {
    using Full = std::function<RegData(RegData, const RegAccessInfo&)>;
    using Frame = std::function<RegData(RegData, const ScalarAccessFrame&)>;
    Full full_;
    Frame frame_;

  public:
    ScalarReadSource(Full full, Frame frame = {})
        : full_(std::move(full)), frame_(std::move(frame)) {
    }
    RegData operator()(RegData value, const RegAccessInfo& info) const {
        return full_(value, info);
    }
    RegData from_frame(RegData value, const ScalarAccessFrame& frame) const {
        if (frame_)
            return frame_(value, frame);
        RegAccessInfo i;
        frame.fill(i);
        return full_(value, i);
    }
};
using WideReadSource = std::function<WideRegData(WideRegData, const RegAccessInfo&)>;
} // namespace detail
namespace detail {
void* checked_value_pointer(EntityState* node,
                            std::size_t index,
                            const void* token,
                            const std::weak_ptr<void>& life,
                            std::size_t rank,
                            bool empty = false);
void* checked_value_pointer(EntityState* node,
                            std::size_t index,
                            const void* token,
                            const std::shared_ptr<void>& life,
                            std::size_t rank,
                            bool empty = false);
} // namespace detail
namespace detail {
// Common ownership bookkeeping is compiled once, including copy/move/dtor.
class HandleState {
  protected:
    EntityState* node_{};
    std::size_t index_{};
    std::size_t count_{};
    std::weak_ptr<void> lifetime_;
    void check() const;
    EntityState* checked_node() const;
    void* value_pointer(const void* token) const;
    std::size_t checked_element(std::size_t index) const;
    std::size_t checked_element(RegElementIndex index) const;
    std::size_t element_index(std::span<const std::size_t> coordinates) const;
    void ensure_source_available() const;
    void ensure_event_available(EventKind kind) const;
    void set_source(std::shared_ptr<const ScalarReadSource> callback) const;
    void set_source(std::shared_ptr<const WideReadSource> callback) const;
    void set_event(EventKind kind, std::shared_ptr<const EventCallback> callback) const;

  public:
    HandleState();
    HandleState(EntityState* node,
                std::size_t index,
                const std::weak_ptr<void>& life,
                std::size_t count = std::dynamic_extent);
    HandleState(EntityState* node,
                std::size_t index,
                const std::shared_ptr<void>& life,
                std::size_t count = std::dynamic_extent);
    HandleState(const HandleState&);
    HandleState(HandleState&&) noexcept;
    ~HandleState();
    bool is_alive() const noexcept;
    std::size_t size() const;
    bool empty() const {
        return size() == 0;
    }
    const RegEntity& entity() const;
    std::uint64_t address() const;
    std::uint64_t address_span_bytes() const;
    void reset() const;
    std::string diagnostic_name() const;
    RegisterMetadata metadata() const;
    bool has_on_write() const;
    bool has_on_read() const;
    bool has_on_read_complete() const;
};
} // namespace detail
template <class T, std::size_t Rank = 1, bool ValueAccess = true>
class RegView final
    : public std::conditional_t<ValueAccess, detail::DirectValueMembers<T>, detail::ArrayRoot>,
      public detail::HandleState {
    using Members =
        std::conditional_t<ValueAccess, detail::DirectValueMembers<T>, detail::ArrayRoot>;
    template <class, std::size_t, bool>
    friend class RegView;

    static_assert(Rank >= 1 && Rank <= MAX_REG_DIMS, "register rank must be 1..MAX_REG_DIMS");
    friend struct detail::Access;
    friend class RegisterMap;
    friend class RegScope;
    using DataType = std::conditional_t<detail::wide_value_v<T>, WideRegData, RegData>;
    using InfoType = std::conditional_t<detail::wide_value_v<T>,
                                        WideTypedRegAccessInfo<Rank>,
                                        TypedRegAccessInfo<Rank>>;
    template <class F>
    static decltype(auto) invoke_user(F& callback, T value, const InfoType& info) {
        if constexpr (std::is_invocable_v<F&, T, const InfoType&>)
            return std::invoke(callback, value, info);
        else if constexpr (std::is_invocable_v<F&, const InfoType&>)
            return std::invoke(callback, info);
        else if constexpr (std::is_invocable_v<F&>)
            return std::invoke(callback);
        else
            static_assert(sizeof(F) == 0, "callback must accept (value,info), (info), or ()");
    }

    template <class R>
    struct OptionalResult : std::false_type {};
    template <class V>
    struct OptionalResult<std::optional<V>> : std::true_type {};
    template <class F, class... A>
    static DataType read_result(DataType original, F& call, A&&... args) {
        using R = std::remove_cvref_t<std::invoke_result_t<F&, A...>>;
        if constexpr (std::is_void_v<R>) {
            std::invoke(call, std::forward<A>(args)...);
            return original;
        } else if constexpr (std::same_as<R, std::nullopt_t>) {
            std::invoke(call, std::forward<A>(args)...);
            return original;
        } else if constexpr (OptionalResult<R>::value) {
            auto result = std::invoke(call, std::forward<A>(args)...);
            return result ? DataType(detail::to_bits(*result)) : original;
        } else
            return DataType(detail::to_bits(std::invoke(call, std::forward<A>(args)...)));
    }
    template <bool Trusted, class F>
    static DataType source_call(F& f, DataType value, const RegAccessInfo& info) {
        if constexpr (detail::CommonInfoCallable<F>::value)
            return read_result(value, f, info);
        else if constexpr (std::is_invocable_v<F&, T, const InfoType&>) {
            if constexpr (Trusted)
                return read_result(value,
                                   f,
                                   detail::value_from_data<T>(value),
                                   InfoType(info, detail::TrustedAccessTag{}));
            else
                return read_result(value, f, detail::value_from_data<T>(value), InfoType(info));
        } else if constexpr (std::is_invocable_v<F&, const InfoType&>) {
            if constexpr (Trusted)
                return read_result(value, f, InfoType(info, detail::TrustedAccessTag{}));
            else
                return read_result(value, f, InfoType(info));
        } else if constexpr (std::is_invocable_v<F&>)
            return read_result(value, f);
        else
            static_assert(sizeof(F) == 0, "callback must accept (value,info), (info), or ()");
    }
    template <class F>
    static auto source_adapter(F f) {
        if constexpr (detail::wide_value_v<T>) {
            using Fn = detail::WideReadSource;
            return std::make_shared<const Fn>(
                [f = std::move(f)](DataType value, const RegAccessInfo& info) mutable {
                    return source_call<false>(f, value, info);
                });
        } else {
            auto callable = std::make_shared<F>(std::move(f));
            return std::make_shared<const detail::ScalarReadSource>(
                [callable](RegData value, const RegAccessInfo& info) {
                    return source_call<false>(*callable, value, info);
                },
                [callable](RegData value, const detail::ScalarAccessFrame& frame) {
                    return frame.with_info([&](const RegAccessInfo& i) {
                        return source_call<true>(*callable, value, i);
                    });
                });
        }
    }
    template <class F>
    static std::shared_ptr<const detail::EventCallback> event_adapter(F f) {
        detail::EventCallback::FrameInvoke frame = nullptr;
        if constexpr (!detail::wide_value_v<T>)
            frame = &detail::invoke_frame_event<T, InfoType, F>;
        if constexpr (detail::CommonInfoCallable<F>::value)
            return detail::bind_event(std::move(f), &detail::invoke_common_event<F>, frame);
        else if constexpr (std::is_invocable_v<F&, T, const InfoType&>)
            return detail::bind_event(
                std::move(f), &detail::invoke_value_event<T, InfoType, F>, frame);
        else if constexpr (std::is_invocable_v<F&, const InfoType&>)
            return detail::bind_event(std::move(f), &detail::invoke_info_event<InfoType, F>, frame);
        else if constexpr (std::is_invocable_v<F&>)
            return detail::bind_event(std::move(f), &detail::invoke_empty_event<F>, frame);
        else
            static_assert(sizeof(F) == 0, "callback must accept (value,info), (info), or ()");
    }

  public:
    using Value = T;
    using Data = DataType;
    using Raw = detail::raw_type_t<T>;

    using AccessInfo = InfoType;
    static constexpr std::size_t rank = Rank;
    RegView()
        requires(!ValueAccess || detail::direct_member_mask<T> == 0)
    = default;
    REGFW_DETAIL_ALWAYS_INLINE RegView(const RegView&) = default;
    REGFW_DETAIL_ALWAYS_INLINE RegView(RegView&&) noexcept = default;
    REGFW_DETAIL_ALWAYS_INLINE ~RegView() = default;
    RegView& operator=(const RegView& other) noexcept {
        if (this != &other) {
            std::destroy_at(this);
            std::construct_at(this, other);
        }
        return *this;
    }
    RegView& operator=(RegView&& other) noexcept {
        if (this != &other) {
            std::destroy_at(this);
            std::construct_at(this, std::move(other));
        }
        return *this;
    }
    RegView& operator=(T value)
        requires(ValueAccess)
    {
        ref() = value;
        return *this;
    }
    T& ref() const
        requires(ValueAccess)
    {
        return *static_cast<T*>(value_pointer(&detail::value_type_token<T>));
    }
    operator T&() const
        requires(ValueAccess)
    {
        return ref();
    }
    T* operator->() const
        requires(ValueAccess)
    {
        return &ref();
    }
    T& operator*() const
        requires(ValueAccess)
    {
        return ref();
    }

  private:
    REGFW_DETAIL_ALWAYS_INLINE RegView(detail::EntityState* node,
                                       std::size_t index,
                                       const std::weak_ptr<void>& life,
                                       std::size_t count = std::dynamic_extent)
        : Members(static_cast<T*>(detail::checked_value_pointer(
              node, index, &detail::value_type_token<T>, life, Rank, !ValueAccess && count == 0))),
          detail::HandleState(node, index, life, count) {
    }
    REGFW_DETAIL_ALWAYS_INLINE RegView(detail::EntityState* node,
                                       std::size_t index,
                                       const std::shared_ptr<void>& life,
                                       std::size_t count = std::dynamic_extent)
        : Members(static_cast<T*>(detail::checked_value_pointer(
              node, index, &detail::value_type_token<T>, life, Rank, !ValueAccess && count == 0))),
          detail::HandleState(node, index, life, count) {
    }
    detail::EntityState* node() const {
        check();
        return node_;
    }

  public:
    auto at_element(std::size_t i) const {
        return RegView<T, Rank, true>(node_, checked_element(i), lifetime_, 1);
    }
    auto at_element(TypedRegElementIndex<Rank> i) const {
        return RegView<T, Rank, true>(node_, checked_element(i.raw()), lifetime_, 1);
    }
    template <class... I>
    auto at(I... indices) const
        requires((std::integral<I> && ...) && (sizeof...(I) == Rank))
    {
        std::array<std::size_t, sizeof...(I)> coordinates{static_cast<std::size_t>(indices)...};
        return RegView<T, Rank, true>(node_, element_index(coordinates), lifetime_, 1);
    }
    auto subspan(std::size_t offset, std::size_t count = std::dynamic_extent) const
        requires(Rank == 1)
    {
        if (!node_) {
            if (offset || (count != 0 && count != std::dynamic_extent))
                throw Error("register subspan out of range", ErrorKind::Access);
            return RegView<T, 1, false>{};
        }
        check();
        if (offset > count_ || (count != std::dynamic_extent && count > count_ - offset))
            throw Error("register subspan out of range", ErrorKind::Access);
        return RegView<T, 1, false>(node_,
                                    index_ + offset,
                                    lifetime_,
                                    count == std::dynamic_extent ? count_ - offset : count);
    }
    T* data() const {
        if (!node_)
            return nullptr;
        check();
        return count_ ? static_cast<T*>(value_pointer(&detail::value_type_token<T>)) : nullptr;
    }
    std::size_t size_bytes() const {
        return size() * sizeof(T);
    }
    // Callbacks/reset belong to RegEntity, including through a sliced view.

    static constexpr unsigned width_bits() {
        return detail::value_bits<T>;
    }


    template <class F>
    REGFW_DETAIL_INIT_CTOR const RegView& on_read(F f) const {
        ensure_source_available();
        set_source(source_adapter(std::move(f)));
        return *this;
    }
    template <class F>
    REGFW_DETAIL_INIT_CTOR const RegView& on_read_complete(F f) const {
        ensure_event_available(detail::EventKind::ReadComplete);
        if constexpr (detail::CommonInfoCallable<F>::value)
            static_assert(std::is_void_v<typename detail::CommonInfoCallable<F>::Result>,
                          "on_read_complete must return void; use on_read to return data");
        else
            static_assert(
                std::is_void_v<decltype(invoke_user(
                    std::declval<F&>(), std::declval<T>(), std::declval<const InfoType&>()))>,
                "on_read_complete must return void; use on_read to return data");
        set_event(detail::EventKind::ReadComplete, event_adapter(std::move(f)));
        return *this;
    }
    template <class F>
    REGFW_DETAIL_INIT_CTOR const RegView& on_write(F f) const {
        ensure_event_available(detail::EventKind::Write);
        set_event(detail::EventKind::Write, event_adapter(std::move(f)));
        return *this;
    }
};
// Source compatibility: REG now returns the actual span-like RegView type.
template <class T, std::size_t Rank = 1, bool ValueAccess = true>
using TypedReg = RegView<T, Rank, ValueAccess>;
namespace detail {
struct Access {
    template <class T, std::size_t R, bool V>
    static EntityState* node(const RegView<T, R, V>& handle) {
        return handle.node();
    }
    template <class T, std::size_t R, bool V>
    static std::weak_ptr<void> lifetime(const RegView<T, R, V>& handle) {
        return handle.lifetime_;
    }
    static std::size_t storage_bytes(const EntityState* node);
    static WideRegData read_wide(const EntityState* node, std::size_t index);
    static const RegisterMap* owner(const EntityState* node);
    static const RegisterMap* owner(const Bundle& bundle);
    static std::weak_ptr<void> lifetime(const Bundle& bundle);
    static WindowTarget window_target(const Bundle& bundle);
    static std::span<EntityState* const> entity_nodes(const Bundle& bundle);
    template <class T, std::size_t R, bool V>
    static std::size_t first(const RegView<T, R, V>& handle) {
        return handle.index_;
    }
    template <class T, std::size_t R, bool V>
    static const RegisterMap* owner(const RegView<T, R, V>& handle) {
        return owner(node(handle));
    }
    template <class T, std::size_t R, bool V>
    static std::size_t storage_bytes(const RegView<T, R, V>& handle) {
        return storage_bytes(node(handle));
    }
};
} // namespace detail
template <class Handle>
using access_info_t = typename std::remove_cvref_t<Handle>::AccessInfo;

namespace detail {
// Address -> entry page table used by the manual (non-view) mapping path
// and the alias cache. The static precomputed-index binding of the old
// per-word backend was removed with that backend.
template <class E>
class SparseLookup {
    static constexpr std::uint32_t empty = UINT32_MAX;
    struct Page;
    struct SparsePages;
    std::uint64_t bytes_{};
    std::unique_ptr<SparsePages> sparse_;
    template <class F>
    void for_each_page(F f) const;
    std::vector<std::unique_ptr<Page>> pages_;
    std::vector<E> entries_;

  public:
    explicit SparseLookup(std::uint64_t bytes);
    ~SparseLookup();
    const E* get(std::uint64_t address) const;
    void insert(std::uint64_t address, E value);
    void clear();
    void freeze();
    void swap(SparseLookup& other) noexcept;
    template <class F>
    void for_each(F f) const;
    template <class F>
    void for_each_mut(F f);
};
struct AddressWindow;
struct IdentityWindow;
struct LayoutScope;
// Initialization-only diagnostics; never consulted by MMIO dispatch.
struct CallbackDescription {
    std::string name;
    std::string_view type;
    std::source_location location;
};
template <class F>
constexpr std::string_view callback_type_name() {
#if defined(__clang__) || defined(__GNUC__)
    return __PRETTY_FUNCTION__;
#elif defined(_MSC_VER)
    return __FUNCSIG__;
#else
    return "callable";
#endif
}
template <class F>
CallbackDescription callback_description(std::string_view name, std::source_location at) {
    return {std::string(name), callback_type_name<F>(), at};
}
struct ReservedRegion;
bool overlaps(std::uint64_t a, std::uint64_t n, std::uint64_t b, std::uint64_t m);
std::string join_name(std::string_view prefix, std::string_view name);
} // namespace detail
namespace detail {
template <class V, class R>
RegData checked_access_result(R result) {
    if constexpr (std::signed_integral<R>)
        if (result < 0)
            throw RegAccessDeriveError::access_error("negative mapped element");
    if constexpr (std::integral<V>)
        if (detail::to_bits(result) > std::numeric_limits<V>::max())
            throw RegAccessDeriveError::access_error("mapped value exceeds declared field type");
    return detail::to_bits(detail::value_from_data<V>(detail::to_bits(result)));
}
} // namespace detail
namespace detail {
struct ViewPreAccessChain;
struct PreAccessSlot {
    using Function = EventCallback;
    std::string name;
    std::shared_ptr<const Function> callback;
    CallbackDescription description;
    bool deferred{}, required{};
};
struct AccessResolverSlot {
    struct Function {
        using Full = std::function<RegData(const RegAccessInfo&)>;
        using Frame = std::function<RegData(const ScalarAccessFrame&)>;
        Full full;
        Frame frame;
        Function(Full f, Frame p = {}) : full(std::move(f)), frame(std::move(p)) {
        }
        RegData operator()(const RegAccessInfo& i) const {
            return full(i);
        }
        RegData from_frame(const ScalarAccessFrame& f) const {
            if (frame)
                return frame(f);
            RegAccessInfo i;
            f.fill(i);
            return full(i);
        }
    };
    std::string name;
    std::shared_ptr<const Function> callback;
    CallbackDescription description;
    bool deferred{};
    bool required{};
    RegData from_frame(const ScalarAccessFrame& frame) const {
        auto fn = callback;
        if (!fn)
            throw RegAccessDeriveError::device_error("access resolver is not bound: " + name);
        return fn->from_frame(frame);
    }
    RegData operator()(const RegAccessInfo& info) const {
        auto fn = callback; // May be cleared/replaced from inside the callback.
        if (!fn)
            throw RegAccessDeriveError::device_error("access resolver is not bound: " + name);
        return (*fn)(info);
    }
};
} // namespace detail
class RegisterMap {
    friend class detail::AsyncBackend;
    friend class Groups;
    friend class RegScope;
    friend class RegLayout;
    friend struct detail::EntityState;
    friend class detail::ViewCompiler;

    friend class detail::FormulaInstaller;
    friend class detail::FormulaDecoder;

    using Node = detail::EntityState;
    struct Entry;
    struct AliasEntry;
    struct CompiledAccess;
    struct AliasHop;
    struct Resolution;
    struct AddressResolution;
    struct DispatchGuard;
    struct PreAccessGuard;
    struct AccessScratch;
    std::unique_ptr<AccessScratch> access_scratch_;
    struct InstanceWindow;
    std::uint64_t size_{};
    detail::SparseLookup<Entry> lookup_;
    detail::SparseLookup<AliasEntry> aliases_;

    // Formula address decoding backend, the only view dispatcher since
    // the per-word tables were removed. Installed by install_formula_layout
    // and the dynamic AddressSpace compiler alike.
    std::unique_ptr<detail::FormulaDecoder> formula_;
    bool compiled_view_mode_{};
    std::vector<LayoutWarning> mapping_warnings_;
    std::vector<std::unique_ptr<std::string>> compiled_names_;
    std::vector<std::unique_ptr<detail::ViewPreAccessChain>> view_pre_access_chains_;
    std::vector<std::unique_ptr<RegEntity>> entities_;
    std::vector<Node*> nodes_;
    std::vector<std::unique_ptr<detail::RegViewLayout>> primary_views_;
    std::vector<detail::AddressWindow> windows_;
    std::vector<detail::ReservedRegion> reserved_;
    std::vector<InstanceWindow> instances_;
    std::vector<detail::IdentityWindow> identities_;
    std::vector<detail::LayoutScope> layout_scopes_;
    std::shared_ptr<void> life_;
    HLogLogger* logger_{};
    bool log_enabled(HLogLevel level) const noexcept;
    void emit_log(const detail::LogRecord& record) const noexcept;
    bool finalized_{}, dispatching_{}, pre_access_active_{};
    // Endpoint admission runs after resolution/validation, before behavior.
    void* endpoint_owner_{};
    std::function<RegMemTxResult(const Node&, const RegAccessInfo&)> access_admission_;
    struct ViewPolicy {
        std::uint64_t base, size;
        AliasAccessPolicy policy;
    };
    std::vector<ViewPolicy> view_policies_;
    AliasAccessPolicy view_policy(std::uint64_t address, unsigned size) const;
    static std::uint64_t checked_map_size(std::uint64_t size);
    void require_initialized() const;
    using IdentityFn = std::function<std::uint32_t(const IdentityResolveRequest&)>;
    using PreFn = detail::PreAccessSlot::Function;
    std::shared_ptr<const IdentityFn> identity_resolver_;
    detail::CallbackDescription identity_description_;
    bool identity_deferred_{};
    IdentityRejectionPolicy identity_rejection_policy_ = IdentityRejectionPolicy::AccessError;
    std::shared_ptr<const PreFn> pre_access_;
    std::vector<std::shared_ptr<const PreFn>> retired_pre_access_;
    // Slots are sparse, but resolver lookup is outside the ordinary MMIO path.
    struct ResolverRegistry;
    std::unique_ptr<ResolverRegistry> access_resolvers_;
    void bind_identity_resolver(std::shared_ptr<const IdentityFn> callback,
                                detail::CallbackDescription description);
    void bind_window_resolver(const void* key,
                              std::string_view name,
                              std::shared_ptr<const IdentityFn> callback,
                              detail::CallbackDescription description);
    void clear_window_resolver(const void* key);
    bool has_window_resolver(const void* key) const;
    void defer_window_resolver(const void* key, std::string_view name);
    void bind_access_resolver(const void* key,
                              std::string_view name,
                              std::shared_ptr<const detail::AccessResolverSlot::Function> callback,
                              detail::CallbackDescription description);
    std::size_t register_layout_scope(std::uint64_t base,
                                      std::uint64_t size,
                                      std::string name,
                                      ResetScope reset,
                                      std::optional<std::uint32_t> instance,
                                      std::size_t parent,
                                      std::uint64_t repeat_count,
                                      std::uint64_t repeat_stride,
                                      std::uint64_t repeat_index);
    void emit_layout() const noexcept;
    void emit_resolver_update(std::string_view key,
                              const detail::CallbackDescription& description,
                              bool bound) const noexcept;
    void release_retired_pre_access() noexcept;
    void retire_pre_access(const std::shared_ptr<const PreFn>& callback);
    void bind_pre_access(std::shared_ptr<const PreFn> callback);
    std::shared_ptr<detail::PreAccessSlot> view_pre_access_slot(const void* key,
                                                                std::string_view name);
    void bind_view_pre_access(const void* key,
                              std::string_view name,
                              std::shared_ptr<const PreFn> callback,
                              detail::CallbackDescription description);
    void clear_view_pre_access(const void* key);
    bool has_view_pre_access(const void* key) const;
    void defer_view_pre_access(const void* key, std::string_view name);
    std::shared_ptr<detail::AccessResolverSlot> access_resolver_slot(const void* id,
                                                                     std::string_view name);
    void clear_access_resolver(const void* id);
    bool has_access_resolver(const void* id) const;
    std::optional<std::uint32_t> source_instance(std::uint64_t address) const;
    const detail::AddressWindow* window_at(std::uint64_t address) const;
    void mutable_layout() const;
    void region_check(std::uint64_t base, std::uint64_t n) const;
    AliasEntry
    walk_alias(std::uint64_t original, unsigned size, std::vector<AliasHop>* hops = nullptr) const;
    AliasEntry resolve_static(std::uint64_t address, unsigned size) const;
    // Selected view entry for an address: formula decode or the word table.
    const AliasEntry* view_entry(std::uint64_t address, AliasEntry& scratch) const;
    AddressResolution resolve_metadata(std::uint64_t address, unsigned size);
    Resolution& resolve(std::uint64_t address,
                        unsigned size,
                        RegTransactionKind kind,
                        RegData value,
                        const AliasEntry* selected = nullptr);
    template <unsigned Mode>
    Resolution& resolve_plan(std::uint64_t address,
                             unsigned size,
                             RegTransactionKind kind,
                             RegData value,
                             const AliasEntry* selected);
    template <bool Writing>
    std::optional<RegData>
    fast_scalar(std::uint64_t address, RegData value, const CompiledAccess* selected);
    std::optional<RegData> fast_scalar_resolved(Resolution& r, bool writing);
    template <bool Writing>
    std::optional<RegMmioReadResult>
    mapped_scalar(std::uint64_t address, RegData value, const CompiledAccess* selected);
    RegData scalar32_events(const Entry& e,
                            RegAccessInfo& info,
                            const AccessIdentity* identity,
                            bool writing);
    void validate_access(const Resolution& r) const;
    void derive(Resolution& r);
    std::optional<RegMemTxResult> prepare(Resolution& r);
    bool pre(Resolution& r);
    template <class Context>
    bool pre(const Node& node, const detail::ViewPreAccessChain* chain, const Context& context);
    bool access_log_enabled(const Node&) const noexcept;
    void
    access_log(const Resolution& r, RegData value, std::string_view action = {}) const noexcept;
    void log_error(std::string_view phase,
                   std::string_view name,
                   const RegAccessInfo* info = nullptr) const noexcept;
    void log_request_error(RegTransactionKind kind,
                           std::uint64_t address,
                           RegData value,
                           unsigned size) const noexcept;
    void validate_resolver_bindings() const;
    void warn_truncated_write(std::uint64_t address, RegData value, unsigned size) const noexcept;
    void complete_read_event(Resolution& r, WideRegData value, bool source_ok = true);
    void refresh_wide(Resolution& r, WideAccessValues& values);
    RegMmioWriteResult write_wide(Resolution& r);
    RegMmioReadResult read_wide(Resolution& r);
    WideRegData read_storage(const Node& n, std::size_t index) const;
    void write_storage(Node& n, std::size_t index, WideRegData value);
    void transaction_values(Resolution& r, WideAccessValues& values);
    RegMmioWriteResult write_transaction(Resolution& r);
    RegMmioReadResult read_transaction(Resolution& r);

  public:
    std::string name_prefix;
    RegisterMap() noexcept;
    explicit RegisterMap(std::uint64_t size, std::string name = {}, HLogLogger* logger = nullptr);
    template <class L>
        requires std::is_same_v<L, hlog::Logger>
    RegisterMap(std::uint64_t size, std::string name, const L& logger)
        : RegisterMap(size, std::move(name), logger.raw_handle()) {
    }
    // Initialize an empty map once. Failure leaves it empty and retryable.
    void init(std::uint64_t size, std::string name = {}, HLogLogger* logger = nullptr);
    template <class L>
        requires std::is_same_v<L, hlog::Logger>
    void init(std::uint64_t size, std::string name, const L& logger) {
        init(size, std::move(name), logger.raw_handle());
    }
    // Keep the native handle, never a copy of its level or a cached filter.
    void set_logger(HLogLogger* logger) noexcept {
        logger_ = logger;
    }
    template <class L>
        requires std::is_same_v<L, hlog::Logger>
    void set_logger(const L& logger) noexcept {
        set_logger(logger.raw_handle());
    }
    explicit operator bool() const noexcept {
        return size_ != 0;
    }
    ~RegisterMap();
    RegisterMap(const RegisterMap&) = delete;
    RegisterMap& operator=(const RegisterMap&) = delete;
    RegisterMap(RegisterMap&&) = delete;
    RegisterMap& operator=(RegisterMap&&) = delete;
    std::uint64_t size_bytes() const;
    std::size_t entity_count() const noexcept {
        return entities_.size();
    }
    std::size_t mapped_view_count() const noexcept;
    bool is_finalized() const;

  private:
    std::weak_ptr<void> lifetime() const;
    std::unique_ptr<Node> configure_node(std::string name,
                                         const detail::RegisterConfig& spec,
                                         unsigned value_size,
                                         std::optional<std::uint32_t> instance,
                                         std::optional<ResetScope> default_scope);
    REGFW_DETAIL_NOINLINE Node* install_node(std::uint64_t address,
                                             std::unique_ptr<Node> n,
                                             std::span<const std::uint64_t> strides,
                                             bool storage_only = false);
    Node* create_node(std::uint64_t address,
                      std::string name,
                      const detail::RegisterConfig& spec,
                      const detail::StorageType& type,
                      std::optional<std::uint32_t> instance,
                      std::optional<ResetScope> default_scope = {},
                      bool storage_only = false);
    template <bool Wide, std::size_t R, class B, bool A>
    REGFW_DETAIL_NOINLINE Node* create_spec(std::uint64_t address,
                                            std::string name,
                                            const RegisterSpec<R, B, A>& spec,
                                            const detail::StorageType& type,
                                            std::optional<std::uint32_t> instance) {
        return create_node(
            address, std::move(name), detail::register_config<Wide>(spec), type, instance);
    }

  public:
    template <class T, std::size_t R, class B, bool A>
    REGFW_DETAIL_INIT_CTOR auto create_reg(std::uint64_t address,
                                           std::string name,
                                           const RegisterSpec<R, B, A>& spec,
                                           std::optional<std::uint32_t> instance = {}) {
        auto* node = create_spec<detail::wide_value_v<T>>(
            address, std::move(name), spec, detail::storage_type<T>, instance);
        return RegView<T, R, !A>(node, 0, life_);
    }
    template <class T>
    auto create_reg(std::uint64_t address,
                    std::string name,
                    std::source_location location = std::source_location::current()) {
        return create_reg<T>(address, std::move(name), at(0, location));
    }
    void reserve_region(std::uint64_t base,
                        std::uint64_t size,
                        std::string name = {},
                        std::uint64_t repeat_count = 1,
                        std::uint64_t repeat_stride = 0,
                        std::uint64_t repeat_index = 0,
                        std::source_location at = std::source_location::current());
    void
    map_routed_alias_window_with_policy(std::uint64_t source,
                                        std::uint64_t target,
                                        std::uint64_t size,
                                        AccessRoute route,
                                        AliasAccessPolicy policy,
                                        std::string name = {},
                                        std::source_location at = std::source_location::current());
    void map_alias_window(std::uint64_t source,
                          std::uint64_t target,
                          std::uint64_t size,
                          std::string name = {},
                          std::source_location at = std::source_location::current());
    void map_alias_window_with_policy(std::uint64_t source,
                                      std::uint64_t target,
                                      std::uint64_t size,
                                      AliasAccessPolicy policy,
                                      std::string name = {},
                                      std::source_location at = std::source_location::current());
    void map_routed_alias_window(std::uint64_t source,
                                 std::uint64_t target,
                                 std::uint64_t size,
                                 AccessRoute route,
                                 std::string name = {},
                                 std::source_location at = std::source_location::current());

  private:
    void register_source_instance(std::uint64_t base, std::uint64_t size, std::uint32_t instance);

  private:
    void register_identity_repeat_window(PhysicalRepeatAddressWindow window,
                                         std::uint64_t offset,
                                         std::uint64_t size,
                                         std::uint32_t domain,
                                         IdentityRemapMode mode,
                                         IdentityTraceLabels labels,
                                         std::string name,
                                         std::source_location at = std::source_location::current());

  public:
    template <class F>
    void set_identity_resolver(F f, std::source_location at = std::source_location::current()) {
        bind_identity_resolver(std::make_shared<const IdentityFn>(std::move(f)),
                               detail::callback_description<F>({}, at));
    }
    template <class F>
    void set_identity_resolver(std::string_view name,
                               F f,
                               std::source_location at = std::source_location::current()) {
        bind_identity_resolver(std::make_shared<const IdentityFn>(std::move(f)),
                               detail::callback_description<F>(name, at));
    }
    template <class Tag, class V, class F>
    void set_identity_resolver(FieldKey<Tag, V> key,
                               F f,
                               std::source_location at = std::source_location::current()) {
        set_identity_resolver(key, std::string_view{}, std::move(f), at);
    }
    template <class Tag, class V, class F>
    void set_identity_resolver(FieldKey<Tag, V> key,
                               std::string_view name,
                               F f,
                               std::source_location at = std::source_location::current()) {
        static_assert(std::same_as<V, std::uint32_t>,
                      "window identity resolver key must have uint32_t value type");
        auto callback =
            [f = std::move(f)](const IdentityResolveRequest& request) mutable -> std::uint32_t {
            const auto result = std::invoke(f, request);
            try {
                return detail::value_from_data<std::uint32_t>(
                    detail::checked_access_result<std::uint32_t>(result));
            } catch (const RegAccessDeriveError& e) {
                if (e.result == RegMemTxResult::AccessError)
                    throw Error("identity resolver returned an invalid unsigned ID",
                                ErrorKind::Identity);
                throw;
            }
        };
        bind_window_resolver(key.id(),
                             key.name,
                             std::make_shared<const IdentityFn>(std::move(callback)),
                             detail::callback_description<F>(name, at));
    }
    template <class Tag, class V>
    void clear_identity_resolver(FieldKey<Tag, V> key) {
        clear_window_resolver(key.id());
    }
    template <class Tag, class V>
    bool has_identity_resolver(FieldKey<Tag, V> key) const {
        return has_window_resolver(key.id());
    }
    template <class Tag, class V>
    void defer_identity_resolver(FieldKey<Tag, V> key) {
        defer_window_resolver(key.id(), key.name);
    }
    void clear_identity_resolver();
    bool has_identity_resolver() const;
    // Explicit opt-in for protocols that treat invalid identities as ignored.
    // An exception from a broken resolver still returns DeviceError.
    void set_identity_rejection_policy(IdentityRejectionPolicy policy) {
        require_initialized();
        identity_rejection_policy_ = policy;
    }
    void defer_identity_resolver() {
        mutable_layout();
        identity_deferred_ = true;
    }
    // Binding is independent of layout finalization; existing plans retain this stable slot.
  private:
    template <class Tag, class V>
    std::shared_ptr<detail::AccessResolverSlot> access_resolver_slot(FieldKey<Tag, V> key) {
        return access_resolver_slot(key.id(), key.name);
    }

  public:
    template <class Tag, class V, class F>
    void set_access_resolver(FieldKey<Tag, V> key,
                             F f,
                             std::source_location at = std::source_location::current()) {
        set_access_resolver(key, std::string_view{}, std::move(f), at);
    }
    template <class Tag, class V, class F>
    void set_access_resolver(FieldKey<Tag, V> key,
                             std::string_view name,
                             F f,
                             std::source_location at = std::source_location::current()) {
        auto callable = std::make_shared<F>(std::move(f));
        auto callback = std::make_shared<const detail::AccessResolverSlot::Function>(
            [callable](const RegAccessInfo& info) {
                return detail::checked_access_result<V>(std::invoke(*callable, info));
            },
            [callable](const detail::ScalarAccessFrame& frame) {
                return frame.with_info([&](const RegAccessInfo& i) {
                    return detail::checked_access_result<V>(std::invoke(*callable, i));
                });
            });
        bind_access_resolver(
            key.id(), key.name, std::move(callback), detail::callback_description<F>(name, at));
    }
    template <class Tag, class V>
    void clear_access_resolver(FieldKey<Tag, V> key) {
        clear_access_resolver(key.id());
    }
    template <class Tag, class V>
    void defer_access_resolver(FieldKey<Tag, V> key) {
        mutable_layout();
        access_resolver_slot(key)->deferred = true;
    }
    template <class Tag, class V>
    bool has_access_resolver(FieldKey<Tag, V> key) const {
        return has_access_resolver(key.id());
    }
    template <class F>
    void set_pre_access(F f) {
        bind_pre_access(detail::bind_pre_access(std::move(f)));
    }
    template <class Tag, class F>
    void set_pre_access(PreAccessKey<Tag> key,
                        F f,
                        std::source_location at = std::source_location::current()) {
        set_pre_access(key, std::string_view{}, std::move(f), at);
    }
    template <class Tag, class F>
    void set_pre_access(PreAccessKey<Tag> key,
                        std::string_view name,
                        F f,
                        std::source_location at = std::source_location::current()) {
        static_assert(std::same_as<std::invoke_result_t<F&, const RegAccessInfo&>, void>,
                      "pre_access must return void");
        bind_view_pre_access(key.id(),
                             key.name,
                             detail::bind_pre_access(std::move(f)),
                             detail::callback_description<F>(name, at));
    }
    template <class Tag>
    void clear_pre_access(PreAccessKey<Tag> key) {
        clear_view_pre_access(key.id());
    }
    template <class Tag>
    bool has_pre_access(PreAccessKey<Tag> key) const {
        return has_view_pre_access(key.id());
    }
    template <class Tag>
    void defer_pre_access(PreAccessKey<Tag> key) {
        defer_view_pre_access(key.id(), key.name);
    }
    void clear_pre_access();
    bool has_pre_access() const;
    std::uint64_t resolve_address(std::uint64_t address, unsigned size = 1) const;
    std::tuple<std::uint64_t, AccessRoute, std::optional<AccessIdentity>>
    resolve_address_with_identity(std::uint64_t address, unsigned size = 4);
    std::pair<std::uint64_t, AccessRoute> resolve_address_route(std::uint64_t address,
                                                                unsigned size = 1) const;
    void finalize();
    // Summary: bundle roles, array spans, shared storage and symbolic routing.
    // Full also includes static alias, identity, index and callback tables.
    // Repeats expand one representative and retain counts/address strides.
    // Also emitted once at successful finalize through hlog at Debug level.
    // Formatting never invokes user callbacks.
    std::string describe_layout(LayoutDetail detail = LayoutDetail::Summary) const;
    void validate_integrity() const;
    // Inspection never runs identity/index resolvers, behaviors or callbacks.
    // Supply a known identity to inspect that branch without executing it.
    std::string explain_address(std::uint64_t address,
                                unsigned size = 4,
                                RegTransactionKind kind = RegTransactionKind::Read,
                                std::optional<std::uint32_t> resolved_identity = {}) const;
    std::vector<LayoutWarning> layout_warnings() const;
    RegMmioWriteResult try_mmio_write_sized(std::uint64_t address, RegData value, unsigned size);
    RegMmioReadResult try_mmio_read_sized(std::uint64_t address, unsigned size);
    auto mmio_write_sized(std::uint64_t a, RegData v, unsigned s) {
        return try_mmio_write_sized(a, v, s);
    }
    auto mmio_write(std::uint64_t a, RegData v) {
        return try_mmio_write_sized(a, v, 4);
    }
    RegData mmio_read_sized(std::uint64_t a, unsigned s);
    RegData mmio_read(std::uint64_t a);
    std::uint32_t qemu_mmio_write_sized(std::uint64_t a, RegData v, unsigned s) noexcept;
    std::uint32_t qemu_mmio_read_sized(std::uint64_t a, RegData* data, unsigned s) noexcept;
    std::size_t reset();
    std::size_t reset_matching(ResetRequest request);
    void reset_all();

  private:
    std::vector<Node*> snapshot_nodes() const;

  public:
    SnapshotSchema snapshot_schema() const;
    static std::uint64_t snapshot_layout_hash(std::span<const SnapshotLayoutDesc> slots);
    auto describe_snapshot_layout() const {
        return snapshot_schema().slots;
    }
    std::size_t total_snapshot_data_count() const;
    std::size_t total_node_data_count() const;
    std::size_t dump_state(std::span<RegData> data) const;
    void restore_state(std::span<const RegData> data);

    VersionedSnapshot dump_state_versioned() const;
    void restore_state_versioned(const VersionedSnapshot& snapshot);
};


} // namespace reg
// namespace reg
