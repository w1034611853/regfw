#pragma once
#include "../dsl.hpp"
#include <initializer_list>

namespace reg {
class AddressView;
class AddressSpace;
struct RepeatSpec {
    std::uint64_t count{}, stride_bytes{};
    constexpr auto stride(std::uint64_t bytes) const {
        auto out = *this;
        out.stride_bytes = bytes;
        return out;
    }
};
constexpr RepeatSpec repeat(std::uint64_t count) {
    return {count, 0};
}

namespace detail {
struct WindowTarget {
    const RegisterMap* owner{};
    std::weak_ptr<void> lifetime;
    std::vector<EntityState*> nodes;
    std::string name; // Canonical storage group name, independent of address entrances.
    std::uint64_t base{};
    std::size_t first{}, count{};
    bool slice{};
};
std::uint64_t window_target_span(const WindowTarget& target);
class ViewCompiler;

// Owned construction data. Neither a window nor a copy owns register values.
class ViewDescription {
    friend class ViewCompiler;
    friend class ::reg::AddressView;
    friend class ::reg::AddressSpace;
    enum class Use { Container, Mapping, Reserved };
    std::source_location location_;
    std::string name_;
    std::uint64_t offset_{}, size_{};
    std::vector<ViewDescription> children_;
    Use use_ = Use::Container;
    detail::WindowTarget target_;
    AliasAccessPolicy policy_;
    AccessRouteKind route_ = AccessRouteKind::Direct;
    std::optional<std::uint32_t> domain_;
    const void* resolver_key_{};
    std::string resolver_name_;
    const void* pre_access_key_{};
    std::string pre_access_name_;
    std::shared_ptr<PreAccessSlot> pre_access_inline_;
    std::uint64_t repeat_count_{1}, repeat_stride_{}, repeat_index_{}, template_offset_{};
    std::shared_ptr<const int> repeat_group_;
    std::int32_t priority_{};
    ViewDescription with_target(detail::WindowTarget target) const;

  public:
    explicit ViewDescription(std::string name,
                             std::source_location at = std::source_location::current())
        : location_(at), name_(std::move(name)) {
    }
    auto offset(std::uint64_t bytes) const {
        auto out = *this;
        out.offset_ = bytes;
        return out;
    }
    auto size(std::uint64_t bytes) const {
        auto out = *this;
        out.size_ = bytes;
        return out;
    }
    auto size_bytes(std::uint64_t bytes) const {
        return size(bytes);
    }
    auto sub_windows(std::initializer_list<ViewDescription> children) const {
        auto out = *this;
        out.children_.insert(out.children_.end(), children.begin(), children.end());
        return out;
    }
    template <class F>
    ViewDescription sub_windows(RepeatSpec spec, F&& factory) const {
        if (!size_ || !spec.count || spec.count > UINT32_MAX || !spec.stride_bytes ||
            spec.stride_bytes % 4 || (spec.count - 1) > (size_ - 1) / spec.stride_bytes)
            throw Error("invalid repeated sub_windows count/stride/span: " +
                            detail::declaration(name_, offset_, size_, location_) +
                            " count=" + std::to_string(spec.count) +
                            " stride=" + std::to_string(spec.stride_bytes),
                        ErrorKind::Configuration,
                        location_);
        auto out = *this;
        auto group = std::make_shared<const int>(0);
        for (std::uint64_t i = 0; i < spec.count; ++i) {
            ViewDescription child = std::invoke(factory, static_cast<std::size_t>(i));
            const auto step = i * spec.stride_bytes;
            if (!child.size_ || child.offset_ > size_ - step ||
                child.size_ > size_ - step - child.offset_)
                throw Error(
                    "repeated sub_window outside its parent: " +
                        detail::declaration(
                            child.name_, child.offset_ + step, child.size_, child.location_) +
                        "\nparent: " + detail::declaration(name_, 0, size_, location_),
                    ErrorKind::Configuration,
                    child.location_);
            child.template_offset_ = child.offset_;
            child.offset_ += step;
            child.repeat_count_ = spec.count;
            child.repeat_stride_ = spec.stride_bytes;
            child.repeat_index_ = i;
            child.repeat_group_ = group;
            out.children_.push_back(std::move(child));
        }
        return out;
    }
    template <class B>
    ViewDescription map(const B& value) const
        requires(std::derived_from<B, Bundle>)
    {
        return with_target(detail::Access::window_target(value));
    }
    template <class T, std::size_t R, bool V>
    ViewDescription map(const RegView<T, R, V>& value) const {
        auto* node = detail::Access::node(value);
        detail::WindowTarget target;
        target.owner = detail::Access::owner(node);
        target.lifetime = detail::Access::lifetime(value);
        target.nodes.push_back(node);
        target.first = detail::Access::first(value);
        target.count = value.size();
        target.slice = true;
        return with_target(std::move(target));
    }
    ViewDescription reserved() const;
    auto identity(std::uint32_t domain) const {
        auto out = *this;
        out.domain_ = domain;
        return out;
    }
    template <class Tag, class V>
    auto using_resolver(FieldKey<Tag, V> key) const {
        static_assert(std::same_as<V, std::uint32_t>,
                      "window identity resolver key must have uint32_t value type");
        auto out = *this;
        out.resolver_key_ = key.id();
        out.resolver_name_ = key.name;
        return out;
    }
    auto read_only_ignore_writes() const {
        auto out = *this;
        out.policy_ = AliasAccessPolicy::READ_ONLY_IGNORE_WRITES;
        return out;
    }
    auto read_only_error_writes() const {
        auto out = *this;
        out.policy_ = AliasAccessPolicy::READ_ONLY_ERROR_WRITES;
        return out;
    }
    auto vf_to_pf() const {
        auto out = *this;
        out.route_ = AccessRouteKind::VfToPf;
        return out;
    }
    auto pf_to_vf() const {
        auto out = *this;
        out.route_ = AccessRouteKind::PfToVf;
        return out;
    }
};
} // namespace detail
} // namespace reg
