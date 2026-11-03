#pragma once
#include "runtime.hpp"
#include "detail/static_metadata.hpp"
#include <utility>
#include <new>
#include <map>
namespace reg {
struct AccessGroupSpec;
template <unsigned I>
struct IndexDimension {
    static constexpr unsigned value = I;
};
template <unsigned I>
inline constexpr IndexDimension<I> index{};
struct IdentityDomainCondition {
    std::uint32_t domain;
    bool operator()(const RegAccessInfo& i) const {
        return i.is_identity_domain(domain);
    }
};
constexpr auto identity_domain(std::uint32_t domain) {
    return IdentityDomainCondition{domain};
}
template <class K>
struct Passthrough {
    K field;
};
template <class K>
constexpr auto passthrough(K field) {
    return Passthrough<K>{field};
}
class Groups {
    struct Coordinate {
        const void* id;
        std::string_view name;
        unsigned dimension;
        RegData (*convert)(std::uint32_t);
        bool identity{};
    };
    struct Derivation {
        const void* output{};
        std::string_view output_name;
        const void* source{};
        std::string_view source_name;
        std::function<bool(const RegAccessInfo&)> condition;
        std::function<RegData(const RegAccessInfo&)> resolver;
        std::shared_ptr<detail::AccessResolverSlot> resolver_slot;
        std::function<void(RegisterMap&)> bind;
        const void* fallback{};
        bool metadata_only{};
        std::optional<std::uint32_t> condition_domain;
        detail::CallbackDescription condition_description, resolver_description;
        const void* resolver_key{};
        std::string_view resolver_name;
    };
    struct Plan {
        std::string name;
        struct Member {
            detail::EntityState* node;
            std::weak_ptr<void> lifetime;
        };
        std::vector<Member> members;
        std::vector<Coordinate> coordinates;
        std::vector<std::shared_ptr<Derivation>> derivations;
        void run(RegAccessInfo& info) const;
    };
    std::vector<std::shared_ptr<Plan>> plans_;

  public:
    template <const auto& Spec, unsigned Dimension, class S, class O>
    static consteval detail::StaticIndexRule index_plan(S source, O output, std::string_view name) {
        static_assert(Dimension < Spec.rank, "static index dimension outside register rank");
        static_assert(std::same_as<typename S::Value, std::uint32_t> &&
                          std::same_as<typename O::Value, std::uint32_t>,
                      "static index rule requires uint32 fields");
        std::uint64_t coefficient = 1;
        for (std::size_t k = Dimension + 1; k < Spec.rank; ++k) {
            if (!Spec.dimensions[k] || coefficient > UINT32_MAX / Spec.dimensions[k])
                throw "static index coefficient overflow";
            coefficient *= Spec.dimensions[k];
        }
        if (!Spec.dimensions[Dimension] || Spec.dimensions[Dimension] > UINT32_MAX)
            throw "invalid static index extent";
        return {source.id(),
                output.id(),
                name,
                source.name,
                output.name,
                Dimension,
                Spec.rank,
                static_cast<std::uint32_t>(Spec.dimensions[Dimension]),
                static_cast<std::uint32_t>(coefficient)};
    }
    static void install_index(detail::EntityState* node, detail::StaticIndexRule rule);
    static void apply_index(const detail::StaticIndexRule& rule,
                            const detail::AccessResolverSlot& slot,
                            RegAccessInfo& info);
    template <class Output, class Source>
    class DeriveBuilder;
    struct NoSource {};
    class GroupBuilder {
        std::shared_ptr<Plan> plan_;
        void add_member(detail::EntityState* node);

      public:
        explicit GroupBuilder(std::shared_ptr<Plan> p) : plan_(std::move(p)) {
        }
        template <class... R>
        GroupBuilder& members(const R&... r) {
            (add_member(detail::Access::node(r)), ...);
            return *this;
        }
        template <class K, unsigned I>
        GroupBuilder& coordinate(K key, IndexDimension<I>) {
            static_assert(I < MAX_REG_DIMS);
            plan_->coordinates.push_back({key.id(),
                                          key.name,
                                          I,
                                          [](auto v) {
                                              return detail::to_bits(
                                                  detail::value_from_data<typename K::Value>(v));
                                          },
                                          std::same_as<typename K::Value, std::uint32_t>});
            return *this;
        }
        template <class O>
        auto derive(O output) {
            auto d = std::make_shared<Derivation>();
            d->output = output.id();
            d->output_name = output.name;
            plan_->derivations.push_back(d);
            return DeriveBuilder<O, NoSource>(plan_, d);
        }
    };
    template <class Output, class Source>
    class DeriveBuilder {
        std::shared_ptr<Plan> plan_;
        std::shared_ptr<Derivation> rule_;

      public:
        DeriveBuilder(std::shared_ptr<Plan> p, std::shared_ptr<Derivation> r)
            : plan_(std::move(p)), rule_(std::move(r)) {
        }
        template <class S>
        auto from(S source) {
            rule_->source = source.id();
            rule_->source_name = source.name;
            return DeriveBuilder<Output, S>(plan_, rule_);
        }
        template <class C>
        DeriveBuilder& when(C condition,
                            std::source_location at = std::source_location::current()) {
            rule_->condition_domain.reset();
            if constexpr (std::same_as<C, IdentityDomainCondition>)
                rule_->condition_domain = condition.domain;
            rule_->condition_description = detail::callback_description<C>({}, at);
            rule_->condition = std::move(condition);
            return *this;
        }
        template <class F>
        DeriveBuilder& using_resolver(F f,
                                      std::source_location at = std::source_location::current()) {
            return using_resolver(std::string_view{}, std::move(f), at);
        }
        template <class F>
        DeriveBuilder& using_resolver(std::string_view name,
                                      F f,
                                      std::source_location at = std::source_location::current()) {
            static_assert(!std::same_as<Source, NoSource>, "derive().from(coordinate) is required");
            rule_->bind = {};
            rule_->resolver_slot.reset();
            rule_->resolver_key = nullptr;
            rule_->resolver_name = {};
            rule_->resolver_description = detail::callback_description<F>(name, at);
            auto source_id = rule_->source;
            rule_->resolver = [f = std::move(f),
                               source_id](const RegAccessInfo& info) mutable -> RegData {
                auto p = info.index_fields.get(source_id);
                if (!p)
                    throw RegAccessDeriveError::device_error("missing resolver source");
                auto v = detail::value_from_data<typename Source::Value>(p->value);
                auto call = [&]() {
                    if constexpr (std::is_invocable_v<F&, const RegAccessInfo&>)
                        return std::invoke(f, info);
                    else if constexpr (std::is_invocable_v<F&, std::uint32_t, decltype(v)>)
                        return std::invoke(f, info.resolved_id(), v);
                    else
                        return std::invoke(f, v);
                };
                return detail::checked_access_result<typename Output::Value>(call());
            };
            return *this;
        }
        template <class Tag, class V>
        DeriveBuilder& using_resolver(FieldKey<Tag, V> key) {
            static_assert(!std::same_as<Source, NoSource>, "derive().from(coordinate) is required");
            static_assert(std::same_as<V, typename Output::Value>,
                          "resolver field value type must match derivation output");
            rule_->resolver = {};
            rule_->resolver_key = key.id();
            rule_->resolver_name = key.name;
            rule_->resolver_description = {};
            rule_->bind = [rule = rule_.get(), key](RegisterMap& map) {
                auto slot = map.access_resolver_slot(key);
                rule->resolver_slot = std::move(slot);
            };
            return *this;
        }
        template <class K>
        DeriveBuilder& otherwise(Passthrough<K> p) {
            rule_->fallback = p.field.id();
            return *this;
        }
        DeriveBuilder& metadata_only() {
            rule_->metadata_only = true;
            return *this;
        }
        template <class O>
        auto derive(O output) {
            return GroupBuilder(plan_).derive(output);
        }
        template <class K, unsigned I>
        auto coordinate(K key, IndexDimension<I> dim) {
            auto g = GroupBuilder(plan_);
            g.coordinate(key, dim);
            return g;
        }
    };
    auto group(std::string name) {
        for (auto& p : plans_)
            if (p->name == name)
                throw Error("duplicate access group name", ErrorKind::Configuration);
        auto p = std::make_shared<Plan>();
        p->name = std::move(name);
        plans_.push_back(p);
        return GroupBuilder(p);
    }
    void group(std::string name, const AccessGroupSpec& spec);
    void install();
};
class RegRegion {
    std::uint64_t base_{}, size_{};
    std::string name_;
    std::source_location location_;

  public:
    auto location() const {
        return location_;
    }
    RegRegion() = default;
    RegRegion(std::uint64_t base,
              std::uint64_t size,
              std::string name = {},
              std::source_location at = std::source_location::current())
        : base_(base), size_(size), name_(std::move(name)), location_(at) {
    }
    auto base() const {
        return base_;
    }
    auto size() const {
        return size_;
    }
    const auto& name() const {
        return name_;
    }
    auto end_exclusive() const {
        return base_ + size_;
    }
    auto subregion(std::uint64_t offset,
                   std::uint64_t size,
                   std::string name = {},
                   std::source_location at = std::source_location::current()) const {
        if (!size || offset % 4 || size % 4 || offset > size_ || size > size_ - offset)
            throw Error(
                "subregion outside parent: " + detail::declaration(name, base_ + offset, size, at) +
                    "\nparent: " + detail::declaration(name_, base_, size_, location_),
                ErrorKind::Configuration,
                at);
        return RegRegion(base_ + offset, size, std::move(name), at);
    }
};
namespace detail {
struct ResetSet {
    std::vector<detail::EntityState*> nodes;
    std::shared_ptr<detail::ResetSet> parent;
    std::weak_ptr<void> lifetime;
    std::size_t layout_scope = ~std::size_t{};
    std::size_t reset() const;
};
} // namespace detail
class RegScope;
template <class B>
B build(RegScope scope);
template <class B, std::size_t N>
class RepeatBundle {
    static_assert(N > 0, "repeat count must be positive");
    using Array = std::array<B, N>;
    B* values_{};
    std::size_t constructed_{};
    friend class RegScope;
    friend class RegLayout;
    REGFW_DETAIL_NOINLINE void release() noexcept {
        if (values_) {
            std::destroy_n(values_, constructed_);
            std::allocator<B>{}.deallocate(values_, N);
            values_ = nullptr;
            constructed_ = 0;
        }
    }
    B* values() {
        if (!values_)
            throw Error("repeat register bundle has not been initialized",
                        ErrorKind::Configuration);
        return values_;
    }
    const B* values() const {
        if (!values_)
            throw Error("repeat register bundle has not been initialized",
                        ErrorKind::Configuration);
        return values_;
    }
    template <class Make>
    static RepeatBundle generate(Make make) {
        RepeatBundle out;
        out.values_ = std::allocator<B>{}.allocate(N);
        for (std::size_t i = 0; i < N; ++i) {
            // Direct prvalue construction avoids moving a whole B or an
            // expanded array of B objects through intermediate temporaries.
            ::new (static_cast<void*>(out.values_ + i)) B(make(i));
            ++out.constructed_;
        }
        return out;
    }

  public:
    RepeatBundle() noexcept = default;
    explicit RepeatBundle(Array values)
        : RepeatBundle(generate([&](std::size_t i) {
              return std::move(values[i]);
          })) {
    }
    RepeatBundle(const RepeatBundle& other)
        requires(std::is_copy_constructible_v<B>)
    {
        if (other.values_) {
            auto next = generate([&](std::size_t i) {
                return other.values_[i];
            });
            *this = std::move(next);
        }
    }
    RepeatBundle& operator=(const RepeatBundle& other)
        requires(std::is_copy_constructible_v<B>)
    {
        if (this != &other) {
            RepeatBundle next(other);
            *this = std::move(next);
        }
        return *this;
    }
    RepeatBundle(RepeatBundle&& other) noexcept
        : values_(std::exchange(other.values_, nullptr)),
          constructed_(std::exchange(other.constructed_, 0)) {
    }
    RepeatBundle& operator=(RepeatBundle&& other) noexcept {
        if (this != &other) {
            release();
            values_ = std::exchange(other.values_, nullptr);
            constructed_ = std::exchange(other.constructed_, 0);
        }
        return *this;
    }
    ~RepeatBundle() {
        release();
    }
    explicit operator bool() const noexcept {
        return values_ != nullptr;
    }
    B& operator[](std::size_t i) {
        auto p = values();
        if (i >= N)
            throw std::out_of_range("repeat index");
        return p[i];
    }
    const B& operator[](std::size_t i) const {
        auto p = values();
        if (i >= N)
            throw std::out_of_range("repeat index");
        return p[i];
    }
    B* begin() {
        return values();
    }
    B* end() {
        return values() + N;
    }
    const B* begin() const {
        return values();
    }
    const B* end() const {
        return values() + N;
    }
    static constexpr std::size_t size() {
        return N;
    }
    void reset() const {
        for (auto& b : *this)
            b.reset();
    }
};
template <class B>
struct DynamicRepeatBundle {
    std::vector<B> values;
    B& operator[](std::size_t i) {
        return values.at(i);
    }
    const B& operator[](std::size_t i) const {
        return values.at(i);
    }
    auto begin() {
        return values.begin();
    }
    auto end() {
        return values.end();
    }
    auto begin() const {
        return values.begin();
    }
    auto end() const {
        return values.end();
    }
    auto size() const {
        return values.size();
    }
    void reset() const {
        for (auto& value : values)
            value.reset();
    }
};
struct ApertureSpec {
    std::uint64_t offset{}, size{}, stride_bytes{};
    std::optional<ResetScope> scope;
    std::optional<ResetScope::Kind> instance_scope;
    bool storage_only{};
    std::source_location location = std::source_location::current();
    constexpr auto at(std::uint64_t value) const {
        auto out = *this;
        out.offset = value;
        return out;
    }
    constexpr auto size_bytes(std::uint64_t value) const {
        auto out = *this;
        if (out.stride_bytes == out.size)
            out.stride_bytes = value;
        out.size = value;
        return out;
    }
    constexpr auto stride(std::uint64_t value) const {
        auto out = *this;
        out.stride_bytes = value;
        return out;
    }
    constexpr auto reset_scope(ResetScope value) const {
        auto out = *this;
        out.scope = value;
        out.instance_scope.reset();
        return out;
    }
    constexpr auto reset_per_instance(ResetScope::Kind kind) const {
        auto out = *this;
        out.instance_scope = kind;
        out.scope.reset();
        return out;
    }
};
constexpr ApertureSpec aperture(std::uint64_t offset = 0,
                                std::uint64_t size = 0,
                                std::source_location at = std::source_location::current()) {
    return {offset, size, size, {}, {}, false, at};
}
namespace detail {
constexpr ApertureSpec entity_aperture() {
    ApertureSpec out;
    out.storage_only = true;
    return out;
}
constexpr ApertureSpec entity_aperture(ApertureSpec spec) {
    return spec;
}
} // namespace detail
constexpr ApertureSpec entity() {
    return detail::entity_aperture();
}
class RegScope {
    friend class Bundle;
    friend class RegLayout;
    friend struct detail::Access;
    template <class B>
    friend B build(RegScope scope);

  private:
    RegisterMap* map{};
    RegRegion region_;
    std::optional<std::uint32_t> instance;
    ResetScope default_reset_scope;
    std::shared_ptr<detail::ResetSet> reset_set;
    std::vector<RegRegion> exports;
    // Validated, non-overlapping intervals; construction must not scan all
    // earlier array elements for every new declaration.
    std::map<std::uint64_t, RegRegion> claims;
    bool view_only_{};
    bool storage_only_{};

  public:
    RegScope(RegisterMap& m,
             std::uint64_t base,
             std::uint64_t size,
             std::string name = {},
             ResetScope rs = {},
             std::optional<std::uint32_t> inst = {},
             std::shared_ptr<detail::ResetSet> parent = {},
             std::uint64_t repeat_count = 1,
             std::uint64_t repeat_stride = 0,
             std::uint64_t repeat_index = 0,
             bool view_only = false,
             bool storage_only = false);
    auto region() const {
        return region_;
    }
    auto base() const {
        return region_.base();
    }
    auto size() const {
        return region_.size();
    }
    void set_default_reset_scope(ResetScope scope) {
        default_reset_scope = scope;
    }

  private:
    void check_claim(std::uint64_t offset,
                     std::uint64_t size,
                     std::string_view name,
                     std::source_location at) const;

  private:
    void claim(std::uint64_t offset,
               std::uint64_t size,
               std::string name = {},
               std::source_location at = std::source_location::current());

  private:
    std::vector<std::uint64_t>
    prepare_reg(std::string_view name, const detail::RegisterConfig& spec, unsigned value_bits);
    void finish_reg(std::string_view name,
                    std::span<const std::uint64_t> addresses,
                    unsigned occupied,
                    detail::EntityState* node);
    detail::EntityState* reg_node(std::string_view name,
                                  const detail::RegisterConfig& spec,
                                  const detail::StorageType& type);
    template <bool Wide, std::size_t R, class B, bool A>
    REGFW_DETAIL_NOINLINE detail::EntityState* reg_spec(std::string_view name,
                                                        const RegisterSpec<R, B, A>& spec,
                                                        const detail::StorageType& type) {
        return reg_node(name, detail::register_config<Wide>(spec), type);
    }
    template <class T, std::size_t R, class B, bool A>
    REGFW_DETAIL_INIT_CTOR auto reg(std::string_view name, const RegisterSpec<R, B, A>& spec) {
        auto* node = reg_spec<detail::wide_value_v<T>>(name, spec, detail::storage_type<T>);
        return RegView<T, R, !A>(node, 0, map->life_);
    }

  private:
    template <class B>
    auto group_at(std::uint64_t offset,
                  std::uint64_t size,
                  std::string name = {},
                  std::source_location at = std::source_location::current()) {
        auto child = region_.subregion(offset, size, detail::join_name(region_.name(), name), at);
        claim(offset, size, name, at);
        return build<B>(RegScope(*map,
                                 child.base(),
                                 child.size(),
                                 child.name(),
                                 default_reset_scope,
                                 instance,
                                 reset_set,
                                 1,
                                 0,
                                 0,
                                 view_only_,
                                 storage_only_));
    }

  private:
    template <class B, std::size_t N>
    auto repeat_group_at(std::uint64_t offset,
                         std::uint64_t stride,
                         std::uint64_t size,
                         std::string name = {},
                         std::source_location at = std::source_location::current()) {
        static_assert(N > 0);
        if (!stride || stride % 4 || size > stride || offset > this->size() ||
            (N - 1) > (UINT64_MAX - offset) / stride || (N - 1) * stride + offset > this->size() ||
            size > this->size() - offset - (N - 1) * stride)
            throw Error("invalid repeated group", ErrorKind::Configuration);
        for (std::size_t i = 0; i < N; ++i)
            claim(offset + i * stride, size, name + "[" + std::to_string(i) + "]", at);
        auto make = [&](std::size_t i) {
            auto start = base() + offset + i * stride;
            auto child = build<B>(
                RegScope(*map,
                         start,
                         size,
                         detail::join_name(region_.name(), name) + "[" + std::to_string(i) + "]",
                         default_reset_scope,
                         static_cast<std::uint32_t>(i),
                         reset_set,
                         N,
                         stride,
                         i,
                         view_only_,
                         storage_only_));
            if (!storage_only_)
                map->register_source_instance(start, size, static_cast<std::uint32_t>(i));
            return child;
        };
        return RepeatBundle<B, N>::generate(make);
    }

  private:
    template <class B>
    auto repeat_group_at(std::uint64_t offset,
                         std::size_t count,
                         std::uint64_t stride,
                         std::uint64_t size,
                         std::string name = {}) {
        DynamicRepeatBundle<B> out;
        if (!stride || stride % 4 || size > stride)
            throw Error("invalid dynamic repeat", ErrorKind::Configuration);
        out.values.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            if (i > (UINT64_MAX - offset) / stride || i > UINT32_MAX)
                throw Error("repeat overflow", ErrorKind::Configuration);
            auto rel = offset + i * stride;
            claim(rel, size, name);
            out.values.push_back(build<B>(RegScope(*map,
                                                   base() + rel,
                                                   size,
                                                   name + "[" + std::to_string(i) + "]",
                                                   default_reset_scope,
                                                   static_cast<std::uint32_t>(i),
                                                   reset_set,
                                                   count,
                                                   stride,
                                                   i,
                                                   view_only_,
                                                   storage_only_)));
            if (!storage_only_)
                map->register_source_instance(base() + rel, size, static_cast<std::uint32_t>(i));
        }
        return out;
    }

  private:
    template <class B>
    auto declare_aperture(std::string_view name, const ApertureSpec& spec) {
        if (spec.storage_only) {
            if (spec.offset || spec.size || spec.stride_bytes)
                throw Error("entity declaration cannot specify an address aperture",
                            ErrorKind::Configuration);
            if (view_only_)
                throw Error("RegViewBundle cannot create entity groups", ErrorKind::Configuration);
            if (spec.instance_scope)
                throw Error("single entity cannot use per-instance reset scope",
                            ErrorKind::Configuration);
            return build<B>(RegScope(*map,
                                     0,
                                     map->size_bytes(),
                                     detail::join_name(region_.name(), name),
                                     spec.scope.value_or(default_reset_scope),
                                     instance,
                                     reset_set,
                                     1,
                                     0,
                                     0,
                                     false,
                                     true));
        }
        if (spec.instance_scope)
            throw Error("single aperture cannot use per-instance reset scope",
                        ErrorKind::Configuration);
        auto child = region_.subregion(
            spec.offset, spec.size, detail::join_name(region_.name(), name), spec.location);
        claim(spec.offset, spec.size, std::string(name), spec.location);
        return build<B>(RegScope(*map,
                                 child.base(),
                                 child.size(),
                                 child.name(),
                                 spec.scope.value_or(default_reset_scope),
                                 instance,
                                 reset_set,
                                 1,
                                 0,
                                 0,
                                 view_only_,
                                 storage_only_));
    }

  private:
    template <class B, std::size_t N>
    auto declare_repeat_aperture(std::string_view name, const ApertureSpec& spec) {
        static_assert(N > 0 && N <= UINT32_MAX, "repeat aperture count must be 1..UINT32_MAX");
        if (spec.storage_only) {
            if (spec.offset || spec.size || spec.stride_bytes)
                throw Error("entity declaration cannot specify an address aperture",
                            ErrorKind::Configuration);
            if (view_only_)
                throw Error("RegViewBundle cannot create entity groups", ErrorKind::Configuration);
            return RepeatBundle<B, N>::generate([&](std::size_t i) {
                auto rs = spec.scope.value_or(default_reset_scope);
                if (spec.instance_scope)
                    rs = {*spec.instance_scope,
                          *spec.instance_scope == ResetScope::Vf ? static_cast<std::uint32_t>(i)
                                                                 : 0};
                return build<B>(RegScope(*map,
                                         0,
                                         map->size_bytes(),
                                         detail::join_name(region_.name(), name) + "[" +
                                             std::to_string(i) + "]",
                                         rs,
                                         static_cast<std::uint32_t>(i),
                                         reset_set,
                                         N,
                                         0,
                                         i,
                                         false,
                                         true));
            });
        }
        auto offset = spec.offset, stride = spec.stride_bytes, size = spec.size;
        if (!stride || stride % 4 || !size || size % 4 || size > stride || offset > this->size() ||
            (N - 1) > (UINT64_MAX - offset) / stride || (N - 1) * stride + offset > this->size() ||
            size > this->size() - offset - (N - 1) * stride)
            throw Error("invalid declared repeated aperture: " +
                            detail::declaration(detail::join_name(region_.name(), name),
                                                base() + offset,
                                                size,
                                                spec.location) +
                            " count=" + std::to_string(N) + " stride=" + std::to_string(stride) +
                            "; each aperture must fit its stride and parent",
                        ErrorKind::Configuration,
                        spec.location);
        for (std::size_t i = 0; i < N; ++i)
            claim(offset + i * stride,
                  size,
                  std::string(name) + "[" + std::to_string(i) + "]",
                  spec.location);
        auto make = [&](std::size_t i) {
            auto start = base() + offset + i * stride;
            auto rs = spec.scope.value_or(default_reset_scope);
            if (spec.instance_scope)
                rs = {*spec.instance_scope,
                      *spec.instance_scope == ResetScope::Vf ? static_cast<std::uint32_t>(i) : 0};
            auto child = build<B>(
                RegScope(*map,
                         start,
                         size,
                         detail::join_name(region_.name(), name) + "[" + std::to_string(i) + "]",
                         rs,
                         static_cast<std::uint32_t>(i),
                         reset_set,
                         N,
                         stride,
                         i,
                         view_only_,
                         storage_only_));
            if (!storage_only_)
                map->register_source_instance(start, size, static_cast<std::uint32_t>(i));
            return child;
        };
        return RepeatBundle<B, N>::generate(make);
    }

  public:
    void reserve(std::uint64_t offset,
                 std::uint64_t size,
                 std::string name = {},
                 std::source_location at = std::source_location::current()) {
        auto r = region_.subregion(offset, size, name, at);
        claim(offset, size, name, at);
        map->reserve_region(r.base(), r.size(), r.name(), 1, 0, 0, at);
    }
    void alias(std::uint64_t offset,
               const RegRegion& target,
               std::string name = {},
               AliasAccessPolicy policy = AliasAccessPolicy::READ_WRITE,
               AccessRoute route = AccessRoute::ALIAS,
               std::source_location at = std::source_location::current()) {
        auto r = region_.subregion(offset, target.size(), name, at);
        claim(offset, target.size(), name, at);
        map->map_routed_alias_window_with_policy(
            r.base(), target.base(), target.size(), route, policy, r.name(), at);
    }
    void export_region(std::uint64_t offset, std::uint64_t size, std::string name) {
        exports.push_back(region_.subregion(offset, size, std::move(name)));
    }
    auto reset() const {
        return reset_set->reset();
    }
};
class Bundle {
    friend struct detail::Access;

  protected:
    RegScope scope_;
    void mark_view_only() {
        scope_.view_only_ = true;
    }
    bool storage_only_scope() const noexcept {
        return scope_.storage_only_;
    }
    template <class T, std::size_t R, class B, bool A>
    REGFW_DETAIL_ALWAYS_INLINE auto declare(std::string_view name,
                                            const RegisterSpec<R, B, A>& spec) {
        return scope_.template reg<T>(name, spec);
    }
    template <class B>
    auto declare_group(std::string_view name,
                       std::uint64_t offset,
                       std::uint64_t size,
                       std::source_location at = std::source_location::current()) {
        return scope_.template group_at<B>(offset, size, std::string(name), at);
    }
    template <class B, std::size_t N>
    auto declare_repeat_group(std::string_view name,
                              std::uint64_t offset,
                              std::uint64_t stride,
                              std::uint64_t size,
                              std::source_location at = std::source_location::current()) {
        return scope_.template repeat_group_at<B, N>(offset, stride, size, std::string(name), at);
    }
    template <class B>
    auto declare_aperture(std::string_view name, const ApertureSpec& spec) {
        return scope_.template declare_aperture<B>(name, spec);
    }
    template <class B, std::size_t N>
    auto declare_repeat_aperture(std::string_view name, const ApertureSpec& spec) {
        return scope_.template declare_repeat_aperture<B, N>(name, spec);
    }

  public:
    using RegBundleBase = Bundle;
    static auto _regfw_auto_probe(detail::DeclarationIndex<0>) -> std::true_type;
    explicit Bundle(RegScope scope) : scope_(std::move(scope)) {
    }
    Bundle(Bundle&&) = default;
    Bundle& operator=(Bundle&&) = default;
    Bundle(const Bundle&) = delete;
    Bundle& operator=(const Bundle&) = delete;
    auto reset() const {
        return scope_.reset();
    }
    auto region() const {
        return scope_.region();
    }
    const auto& exports() const {
        return scope_.exports;
    }
    void declare_export(std::uint64_t offset, std::uint64_t size, std::string name) {
        scope_.export_region(offset, size, std::move(name));
    }
    RegRegion export_region(std::string_view name) const {
        for (auto& r : scope_.exports)
            if (r.name() == name)
                return r;
        throw Error("unknown exported region: " + std::string(name), ErrorKind::Configuration);
    }
    std::vector<RegisterMetadata> metadata() const;
};
class RegEntityBundle : public Bundle {
  public:
    using RegBundleBase = RegEntityBundle;
    explicit RegEntityBundle(RegScope scope) : Bundle(std::move(scope)) {
    }
};
// A composite address view may contain aliases and subviews, but
// cannot instantiate REG storage, including through nested legacy groups.
class RegViewBundle : public Bundle {
  public:
    using RegBundleBase = RegViewBundle;
    explicit RegViewBundle(RegScope scope) : Bundle(std::move(scope)) {
        mark_view_only();
    }
};
template <class B>
REGFW_DETAIL_NOINLINE B build(RegScope scope) {
    auto* map = scope.map;
    B out(std::move(scope));
    if constexpr (requires(B& b, RegisterMap& m) { b.configure_layout(m); })
        out.configure_layout(*map);
    if constexpr (requires(B& b, Groups& g) { b.access_groups(g); }) {
        Groups groups;
        out.access_groups(groups);
        groups.install();
    }
    return out;
}
inline auto scope(RegisterMap& map,
                  std::uint64_t base,
                  std::uint64_t size,
                  std::string name = {},
                  ResetScope reset = {}) {
    return RegScope(map, base, size, std::move(name), reset);
}
class RegLayout {
    RegisterMap& map_;
    std::uint64_t cursor_{};
    std::shared_ptr<detail::ResetSet> reset_set_ = std::make_shared<detail::ResetSet>();
    std::vector<RegRegion> claims_;

  private:
    void claim(std::uint64_t base,
               std::uint64_t size,
               std::string name,
               std::source_location at = std::source_location::current());

  public:
  public:
    explicit RegLayout(RegisterMap& map) : map_(map) {
        reset_set_->lifetime = map.lifetime();
    }
    auto cursor() const {
        return cursor_;
    }
    RegLayout& set_cursor(std::uint64_t value) {
        if (value > map_.size_bytes() || value % 4 || map_.is_finalized())
            throw Error("invalid layout cursor", ErrorKind::Configuration);
        cursor_ = value;
        return *this;
    }
    template <class B>
    auto aperture_at(std::uint64_t base,
                     std::uint64_t size,
                     std::string name = {},
                     ResetScope rs = {},
                     std::source_location at = std::source_location::current()) {
        claim(base, size, name, at);
        return build<B>(RegScope(map_, base, size, std::move(name), rs, {}, reset_set_));
    }
    template <class B>
    auto push_aperture(std::uint64_t size,
                       std::string name = {},
                       ResetScope rs = {},
                       std::source_location at = std::source_location::current()) {
        auto out = aperture_at<B>(cursor_, size, std::move(name), rs, at);
        cursor_ += size;
        return out;
    }
    template <class B, std::size_t N>
    auto repeat_aperture_at(std::uint64_t base,
                            std::uint64_t stride,
                            std::uint64_t size,
                            std::string name = {},
                            ResetScope::Kind reset_kind = ResetScope::Global,
                            std::source_location at = std::source_location::current()) {
        static_assert(N > 0);
        if (!stride || stride % 4 || size > stride || base > map_.size_bytes() ||
            (N - 1) > (UINT64_MAX - base) / stride || (N - 1) * stride + base > map_.size_bytes() ||
            size > map_.size_bytes() - base - (N - 1) * stride)
            throw Error("invalid repeated aperture", ErrorKind::Configuration);
        for (std::size_t i = 0; i < N; ++i)
            claim(base + i * stride, size, name + "[" + std::to_string(i) + "]", at);
        auto make = [&](std::size_t i) {
            auto start = base + i * stride;
            auto child = build<B>(RegScope(
                map_,
                start,
                size,
                name + "[" + std::to_string(i) + "]",
                ResetScope{reset_kind,
                           reset_kind == ResetScope::Vf ? static_cast<std::uint32_t>(i) : 0},
                static_cast<std::uint32_t>(i),
                reset_set_,
                N,
                stride,
                i));
            map_.register_source_instance(start, size, static_cast<std::uint32_t>(i));
            return child;
        };
        return RepeatBundle<B, N>::generate(make);
    }
    template <class B, std::size_t N>
    auto repeat_aperture(std::uint64_t stride,
                         std::uint64_t size,
                         std::string name = {},
                         std::source_location at = std::source_location::current()) {
        auto out = repeat_aperture_at<B, N>(
            cursor_, stride, size, std::move(name), ResetScope::Global, at);
        cursor_ += (N - 1) * stride + size;
        return out;
    }
    template <class B>
    auto repeat_aperture_at(std::uint64_t base,
                            std::size_t count,
                            std::uint64_t stride,
                            std::uint64_t size,
                            std::string name = {},
                            ResetScope::Kind reset_kind = ResetScope::Global,
                            std::source_location at = std::source_location::current()) {
        DynamicRepeatBundle<B> out;
        if (!stride || stride % 4 || size > stride || count > UINT32_MAX)
            throw Error("invalid dynamic aperture", ErrorKind::Configuration);
        if (count && (base > map_.size_bytes() || (count - 1) > (UINT64_MAX - base) / stride ||
                      (count - 1) * stride + base > map_.size_bytes() ||
                      size > map_.size_bytes() - base - (count - 1) * stride))
            throw Error("dynamic aperture overflow", ErrorKind::Configuration);
        out.values.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            auto start = base + i * stride;
            claim(start, size, name, at);
            out.values.push_back(build<B>(RegScope(
                map_,
                start,
                size,
                name + "[" + std::to_string(i) + "]",
                ResetScope{reset_kind,
                           reset_kind == ResetScope::Vf ? static_cast<std::uint32_t>(i) : 0},
                static_cast<std::uint32_t>(i),
                reset_set_,
                count,
                stride,
                i)));
            map_.register_source_instance(start, size, static_cast<std::uint32_t>(i));
        }
        return out;
    }
    template <class B>
    auto repeat_aperture(std::size_t count,
                         std::uint64_t stride,
                         std::uint64_t size,
                         std::string name = {},
                         std::source_location at = std::source_location::current()) {
        auto out = repeat_aperture_at<B>(
            cursor_, count, stride, size, std::move(name), ResetScope::Global, at);
        if (count)
            cursor_ += (count - 1) * stride + size;
        return out;
    }
    RegLayout& padding(std::uint64_t size,
                       std::string name = {},
                       std::source_location at = std::source_location::current()) {
        (void)reserve_at(cursor_, size, std::move(name), at);
        cursor_ += size;
        return *this;
    }
    RegRegion reserve_at(std::uint64_t base,
                         std::uint64_t size,
                         std::string name = {},
                         std::source_location at = std::source_location::current()) {
        claim(base, size, name, at);
        map_.reserve_region(base, size, name, 1, 0, 0, at);
        return RegRegion(base, size, std::move(name));
    }
    auto alias_region(std::uint64_t source,
                      const RegRegion& target,
                      std::string name = {},
                      AliasAccessPolicy p = AliasAccessPolicy::READ_WRITE,
                      std::source_location at = std::source_location::current()) {
        claim(source, target.size(), name, at);
        map_.map_alias_window_with_policy(source, target.base(), target.size(), p, name, at);
        return RegRegion(source, target.size(), std::move(name));
    }
    auto routed_alias_region(std::uint64_t source,
                             const RegRegion& target,
                             AccessRoute route,
                             std::string name = {},
                             AliasAccessPolicy p = AliasAccessPolicy::READ_WRITE,
                             std::source_location at = std::source_location::current()) {
        claim(source, target.size(), name, at);
        map_.map_routed_alias_window_with_policy(
            source, target.base(), target.size(), route, p, name, at);
        return RegRegion(source, target.size(), std::move(name));
    }
    auto alias_in_aperture(std::uint64_t source,
                           const RegRegion& target,
                           std::string name = {},
                           AliasAccessPolicy p = AliasAccessPolicy::READ_WRITE,
                           AccessRoute route = AccessRoute::ALIAS,
                           std::source_location at = std::source_location::current()) {
        bool contained = false;
        for (auto& c : claims_)
            if (source >= c.base() && source - c.base() <= c.size() &&
                target.size() <= c.size() - (source - c.base()))
                contained = true;
        if (!contained)
            throw Error("alias is not inside an aperture", ErrorKind::Configuration);
        map_.map_routed_alias_window_with_policy(
            source, target.base(), target.size(), route, p, name, at);
        return RegRegion(source, target.size(), std::move(name));
    }
    void identity_repeat_window(PhysicalRepeatAddressWindow w,
                                std::uint32_t domain,
                                IdentityRemapMode mode,
                                std::uint64_t offset = 0,
                                std::uint64_t size = 0,
                                std::string name = {},
                                IdentityTraceLabels labels = {},
                                std::source_location at = std::source_location::current()) {
        map_.register_identity_repeat_window(
            w, offset, size ? size : w.size, domain, mode, labels, std::move(name), at);
    }
    void finish() {
        map_.finalize();
    }
    auto reset() const {
        return reset_set_->reset();
    }
};

} // namespace reg
// namespace reg

#define REG(NAME, TYPE, ...)                                                                       \
    static inline constexpr auto NAME##_spec = (__VA_ARGS__);                                      \
    [[maybe_unused]] static inline constexpr ::reg::PhysicalRegisterAddress NAME##_address{        \
        NAME##_spec};                                                                              \
    ::reg::RegView<TYPE,                                                                           \
                   std::remove_cvref_t<decltype(NAME##_spec)>::rank,                               \
                   !std::remove_cvref_t<decltype(NAME##_spec)>::is_array>                          \
        NAME = this->template declare<TYPE>(#NAME, NAME##_spec);                                   \
    template <class RegfwOwner>                                                                    \
    static constexpr auto _regfw_static_member(                                                    \
        std::integral_constant<decltype(&RegfwOwner::NAME), &RegfwOwner::NAME>) {                  \
        return ::reg::detail::StaticRegisterMember<&RegfwOwner::NAME, NAME##_spec, TYPE>{#NAME};   \
    }                                                                                              \
    REGFW_DETAIL_AUTO_MEMBER(NAME)
#define REG_DYNAMIC(NAME, TYPE, ...)                                                               \
    static inline const auto NAME##_spec = (__VA_ARGS__);                                          \
    [[maybe_unused]] static inline const ::reg::PhysicalRegisterAddress NAME##_address{            \
        NAME##_spec};                                                                              \
    ::reg::RegView<TYPE,                                                                           \
                   std::remove_cvref_t<decltype(NAME##_spec)>::rank,                               \
                   !std::remove_cvref_t<decltype(NAME##_spec)>::is_array>                          \
        NAME = this->template declare<TYPE>(#NAME, NAME##_spec);                                   \
    template <class RegfwOwner>                                                                    \
    static constexpr auto _regfw_static_member(                                                    \
        std::integral_constant<decltype(&RegfwOwner::NAME), &RegfwOwner::NAME>) {                  \
        return ::reg::detail::StaticDynamicMember{#NAME};                                          \
    }                                                                                              \
    REGFW_DETAIL_AUTO_MEMBER(NAME)
#define REG_GROUP(NAME, TYPE, OFFSET, SIZE)                                                        \
    [[maybe_unused]] static inline constexpr ::reg::PhysicalAddressWindow NAME##_window{OFFSET,    \
                                                                                        SIZE};     \
    TYPE NAME = this->template declare_group<TYPE>(#NAME, OFFSET, SIZE);                           \
    static inline constexpr auto NAME##_group_spec = ::reg::aperture(OFFSET, SIZE);                \
    template <class RegfwOwner>                                                                    \
    static constexpr auto _regfw_static_member(                                                    \
        std::integral_constant<decltype(&RegfwOwner::NAME), &RegfwOwner::NAME>) {                  \
        return ::reg::detail::StaticBundleMember<&RegfwOwner::NAME, NAME##_group_spec>{#NAME};     \
    }                                                                                              \
    REGFW_DETAIL_AUTO_MEMBER(NAME)
#define REG_REPEAT_GROUP(NAME, TYPE, COUNT, OFFSET, STRIDE, SIZE)                                  \
    [[maybe_unused]] static inline constexpr ::reg::PhysicalRepeatAddressWindow NAME##_window{     \
        OFFSET, COUNT, STRIDE, SIZE};                                                              \
    ::reg::RepeatBundle<TYPE, COUNT> NAME =                                                        \
        this->template declare_repeat_group<TYPE, COUNT>(#NAME, OFFSET, STRIDE, SIZE);             \
    static inline constexpr auto NAME##_group_spec = ::reg::aperture(OFFSET, SIZE).stride(STRIDE); \
    template <class RegfwOwner>                                                                    \
    static constexpr auto _regfw_static_member(                                                    \
        std::integral_constant<decltype(&RegfwOwner::NAME), &RegfwOwner::NAME>) {                  \
        return ::reg::detail::StaticBundleMember<&RegfwOwner::NAME, NAME##_group_spec>{#NAME};     \
    }                                                                                              \
    REGFW_DETAIL_AUTO_MEMBER(NAME)

// Declarative top-level address windows; offsets are relative to their containing bundle.
#define REG_APERTURE(NAME, TYPE, ...)                                                              \
    static inline constexpr auto NAME##_aperture_spec = (__VA_ARGS__);                             \
    [[maybe_unused]] static inline constexpr ::reg::PhysicalAddressWindow NAME##_window{           \
        NAME##_aperture_spec.offset, NAME##_aperture_spec.size};                                   \
    TYPE NAME = this->template declare_aperture<TYPE>(#NAME, NAME##_aperture_spec);                \
    template <class RegfwOwner>                                                                    \
    static constexpr auto _regfw_static_member(                                                    \
        std::integral_constant<decltype(&RegfwOwner::NAME), &RegfwOwner::NAME>) {                  \
        return ::reg::detail::StaticBundleMember<&RegfwOwner::NAME, NAME##_aperture_spec>{#NAME};  \
    }                                                                                              \
    REGFW_DETAIL_AUTO_MEMBER(NAME)
#define REG_REPEAT_APERTURE(NAME, TYPE, COUNT, ...)                                                \
    static inline constexpr auto NAME##_aperture_spec = (__VA_ARGS__);                             \
    [[maybe_unused]] static inline constexpr ::reg::PhysicalRepeatAddressWindow NAME##_window{     \
        NAME##_aperture_spec.offset,                                                               \
        COUNT,                                                                                     \
        NAME##_aperture_spec.stride_bytes,                                                         \
        NAME##_aperture_spec.size};                                                                \
    ::reg::RepeatBundle<TYPE, COUNT> NAME =                                                        \
        this->template declare_repeat_aperture<TYPE, COUNT>(#NAME, NAME##_aperture_spec);          \
    template <class RegfwOwner>                                                                    \
    static constexpr auto _regfw_static_member(                                                    \
        std::integral_constant<decltype(&RegfwOwner::NAME), &RegfwOwner::NAME>) {                  \
        return ::reg::detail::StaticBundleMember<&RegfwOwner::NAME, NAME##_aperture_spec>{#NAME};  \
    }                                                                                              \
    REGFW_DETAIL_AUTO_MEMBER(NAME)

#define REG_ENTITY(NAME, TYPE, ...)                                                                \
    static_assert(std::derived_from<TYPE, ::reg::RegEntityBundle>,                                 \
                  "REG_ENTITY requires a RegEntityBundle");                                        \
    REG_APERTURE(NAME, TYPE, ::reg::detail::entity_aperture(__VA_ARGS__))
#define REG_REPEAT_ENTITY(NAME, TYPE, COUNT, ...)                                                  \
    static_assert(std::derived_from<TYPE, ::reg::RegEntityBundle>,                                 \
                  "REG_REPEAT_ENTITY requires a RegEntityBundle");                                 \
    REG_REPEAT_APERTURE(NAME, TYPE, COUNT, ::reg::detail::entity_aperture(__VA_ARGS__))
#define REG_VIEW(NAME, TYPE, ...)                                                                  \
    static_assert(std::derived_from<TYPE, ::reg::RegViewBundle>,                                   \
                  "REG_VIEW requires a RegViewBundle");                                            \
    REG_APERTURE(NAME, TYPE, __VA_ARGS__)
#define REG_REPEAT_VIEW(NAME, TYPE, COUNT, ...)                                                    \
    static_assert(std::derived_from<TYPE, ::reg::RegViewBundle>,                                   \
                  "REG_REPEAT_VIEW requires a RegViewBundle");                                     \
    REG_REPEAT_APERTURE(NAME, TYPE, COUNT, __VA_ARGS__)

// Only bundle construction/movement/destruction use this compile policy.
// MMIO, callbacks, storage and direct references retain the target's options.
#define REG_BUNDLE(TYPE)                                                                           \
    using RegBundleType = TYPE;                                                                    \
    REGFW_DETAIL_INIT_CTOR explicit TYPE(::reg::RegScope scope)                                    \
        : RegBundleBase(std::move(scope)) {                                                        \
    }                                                                                              \
    REGFW_DETAIL_INIT_CTOR TYPE(TYPE&&) = default;                                                 \
    REGFW_DETAIL_INIT_CTOR TYPE& operator=(TYPE&&) = default;                                      \
    REGFW_DETAIL_INIT_CTOR ~TYPE() = default

// For large devices, put declarations in the PCH and definitions in one .cpp.
// Expand both macros in the same namespace as TYPE (use an unqualified name).
#define REG_BUNDLE_DECL(TYPE)                                                                      \
    using RegBundleType = TYPE;                                                                    \
    REGFW_DETAIL_INIT_CTOR explicit TYPE(::reg::RegScope scope);                                   \
    REGFW_DETAIL_INIT_CTOR TYPE(TYPE&&) noexcept = default;                                        \
    REGFW_DETAIL_INIT_CTOR TYPE& operator=(TYPE&&) noexcept = default;                             \
    REGFW_DETAIL_INIT_CTOR ~TYPE()
#define REG_BUNDLE_IMPL(TYPE)                                                                      \
    REGFW_DETAIL_INIT_CTOR TYPE::TYPE(::reg::RegScope scope) : RegBundleBase(std::move(scope)) {   \
    }                                                                                              \
    REGFW_DETAIL_INIT_CTOR TYPE::~TYPE() = default

// End the literal member list of an entity used by REG_STATIC_LAYOUT.
#define REG_SCHEMA(...)                                                                            \
    static constexpr auto _regfw_static_schema() {                                                 \
        return ::reg::detail::static_schema<RegBundleType __VA_OPT__(, ) __VA_ARGS__>();           \
    }
