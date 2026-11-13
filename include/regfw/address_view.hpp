#pragma once
#include "detail/view_description.hpp"

namespace reg {
// Immutable composition builders. A view owns mapping descriptions, never values.
// Empty parts are transparent. Explicit reserved parts terminate decoding.
class AddressView {
    friend class AddressSpace;
    detail::ViewDescription window_;
    AddressView
    append(detail::ViewDescription child, std::uint64_t offset, std::int32_t priority) const {
        auto out = *this;
        child.offset_ = offset;
        child.priority_ = priority;
        out.window_.children_.push_back(std::move(child));
        return out;
    }

  public:
    explicit AddressView(std::string name,
                         std::uint64_t size,
                         std::source_location at = std::source_location::current())
        : window_(detail::ViewDescription(std::move(name), at).size(size)) {
    }
    AddressView map(const AddressView& target,
                    std::uint64_t offset = 0,
                    std::int32_t priority = 0,
                    std::source_location at = std::source_location::current()) const {
        auto child = target.window_;
        child.location_ = at;
        return append(std::move(child), offset, priority);
    }
    template <class B>
    AddressView map(const B& target,
                    std::uint64_t offset = 0,
                    std::int32_t priority = 0,
                    std::source_location at = std::source_location::current()) const
        requires(std::derived_from<B, Bundle>)
    {
        if (offset >= window_.size_)
            throw Error("view mapping offset outside view", ErrorKind::Configuration);
        auto child = detail::ViewDescription("ENTITY", at).size(window_.size_ - offset).map(target);
        child.name_ = target.region().name();
        if (auto dot = child.name_.rfind('.'); dot != std::string::npos)
            child.name_.erase(0, dot + 1);
        if (auto bracket = child.name_.find('['); bracket != std::string::npos)
            child.name_.erase(bracket);
        return append(std::move(child), offset, priority);
    }
    template <class T, std::size_t R, bool V>
    AddressView map(const RegView<T, R, V>& target,
                    std::uint64_t offset = 0,
                    std::int32_t priority = 0,
                    std::source_location at = std::source_location::current()) const {
        auto child = detail::ViewDescription("REGISTER", at).map(target);
        child.size_ = detail::window_target_span(child.target_);
        return append(std::move(child), offset, priority);
    }
    template <class F>
    AddressView map_repeat(std::string name,
                           std::uint64_t offset,
                           RepeatSpec repeat,
                           F&& factory,
                           std::int32_t priority = 0,
                           std::source_location at = std::source_location::current()) const {
        if (!repeat.count || !repeat.stride_bytes ||
            repeat.count > UINT64_MAX / repeat.stride_bytes)
            throw Error("invalid repeated view span", ErrorKind::Configuration, at);
        const auto span = repeat.count * repeat.stride_bytes;
        if (offset > window_.size_ || span > window_.size_ - offset)
            throw Error("repeated view family outside parent view", ErrorKind::Configuration, at);
        auto family = detail::ViewDescription(std::move(name), at).size(span);
        family = family.sub_windows(repeat, [&](auto i) {
            return std::invoke(factory, i).window_;
        });
        return append(std::move(family), offset, priority);
    }
    AddressView reserve(std::uint64_t offset,
                        std::uint64_t size,
                        std::int32_t priority = 0,
                        std::source_location at = std::source_location::current()) const {
        return append(
            detail::ViewDescription("RESERVED", at).size(size).reserved(), offset, priority);
    }
    template <class Tag>
    AddressView using_pre_access(PreAccessKey<Tag> key,
                                 std::source_location at = std::source_location::current()) const {
        if (window_.pre_access_key_ || window_.pre_access_inline_)
            throw Error("view pre_access already declared", ErrorKind::Configuration, at);
        auto out = *this;
        out.window_.pre_access_key_ = key.id();
        out.window_.pre_access_name_ = key.name;
        return out;
    }
    template <class F>
    AddressView pre_access(F f, std::source_location at = std::source_location::current()) const {
        static_assert(std::same_as<std::invoke_result_t<F&, const RegAccessInfo&>, void>,
                      "pre_access must return void");
        if (window_.pre_access_key_ || window_.pre_access_inline_)
            throw Error("view pre_access already declared", ErrorKind::Configuration, at);
        auto out = *this;
        auto slot = std::make_shared<detail::PreAccessSlot>();
        slot->name = "inline";
        slot->callback = detail::bind_pre_access(std::move(f));
        slot->description = detail::callback_description<F>({}, at);
        out.window_.pre_access_inline_ = std::move(slot);
        return out;
    }
    auto read_only_ignore_writes() const {
        auto out = *this;
        out.window_.policy_ = AliasAccessPolicy::READ_ONLY_IGNORE_WRITES;
        return out;
    }
    auto read_only_error_writes() const {
        auto out = *this;
        out.window_.policy_ = AliasAccessPolicy::READ_ONLY_ERROR_WRITES;
        return out;
    }
    auto vf_to_pf() const {
        auto out = *this;
        out.window_.route_ = AccessRouteKind::VfToPf;
        return out;
    }
    auto pf_to_vf() const {
        auto out = *this;
        out.window_.route_ = AccessRouteKind::PfToVf;
        return out;
    }
    auto identity(std::uint32_t domain) const {
        auto out = *this;
        out.window_.domain_ = domain;
        return out;
    }
    template <class Tag, class V>
    auto using_resolver(FieldKey<Tag, V> key) const {
        auto out = *this;
        out.window_ = out.window_.using_resolver(key);
        return out;
    }
};
inline AddressView view(std::string name,
                        std::uint64_t size,
                        std::source_location at = std::source_location::current()) {
    return AddressView(std::move(name), size, at);
}

class AddressSpace {
    friend void apply_layout(RegisterMap&, const AddressSpace&);
    detail::ViewDescription root_;

  public:
    explicit AddressSpace(std::string name,
                          std::source_location at = std::source_location::current())
        : root_(std::move(name), at) {
    }
    AddressSpace map(const AddressView& view,
                     std::uint64_t base,
                     std::int32_t priority = 0,
                     std::source_location at = std::source_location::current()) const {
        auto out = *this;
        auto child = view.window_;
        child.offset_ = base;
        child.priority_ = priority;
        child.location_ = at;
        out.root_.children_.push_back(std::move(child));
        return out;
    }
    template <class F>
    AddressSpace map_repeat(std::string name,
                            std::uint64_t base,
                            RepeatSpec repeat,
                            F&& factory,
                            std::int32_t priority = 0,
                            std::source_location at = std::source_location::current()) const {
        if (!repeat.count || !repeat.stride_bytes ||
            repeat.count > UINT64_MAX / repeat.stride_bytes)
            throw Error("invalid repeated view span", ErrorKind::Configuration);
        auto family = detail::ViewDescription(std::move(name), at)
                          .offset(base)
                          .size(repeat.count * repeat.stride_bytes);
        family.priority_ = priority;
        family = family.sub_windows(repeat, [&](auto i) {
            return std::invoke(factory, i).window_;
        });
        auto out = *this;
        out.root_.children_.push_back(std::move(family));
        return out;
    }
};
void apply_layout(RegisterMap& map, const AddressSpace& space);
inline AddressSpace address_space(std::string name,
                                  std::source_location at = std::source_location::current()) {
    return AddressSpace(std::move(name), at);
}
} // namespace reg
