#pragma once
#include "dsl.hpp"

namespace reg {
// Owned initialization options. Constructing a spec does not modify a map.
struct RegisterSpaceSpec {
    std::uint64_t size_bytes{};
    std::string map_name;
    std::optional<std::string> root_name;
    HLogLogger* log{};
    ResetScope scope;
    bool finalize_on_build{};
    auto name(std::string value) const {
        auto out = *this;
        out.map_name = std::move(value);
        return out;
    }
    auto root(std::string value) const {
        auto out = *this;
        out.root_name = std::move(value);
        return out;
    }
    auto logger(HLogLogger* value) const {
        auto out = *this;
        out.log = value;
        return out;
    }
    template <class L>
        requires std::is_same_v<L, hlog::Logger>
    auto logger(const L& value) const {
        return logger(value.raw_handle());
    }
    auto reset_scope(ResetScope value) const {
        auto out = *this;
        out.scope = value;
        return out;
    }
    auto finalize() const {
        auto out = *this;
        out.finalize_on_build = true;
        return out;
    }
};
inline RegisterSpaceSpec space(std::uint64_t size = UINT64_MAX & ~std::uint64_t{3}) {
    RegisterSpaceSpec out;
    out.size_bytes = size;
    return out;
}

// Initialization entry: infer Bundle from existing unique_ptr members.
// Publish both pointers only after construction and optional finalization succeed.
template <class B>
void init_bank(std::unique_ptr<RegisterMap>& map,
               std::unique_ptr<B>& regs,
               RegisterSpaceSpec spec) {

    if (map || regs)
        throw Error("init_bank requires empty map and register pointers", ErrorKind::Configuration);
    auto next_map =
        std::make_unique<RegisterMap>(spec.size_bytes, spec.map_name, std::move(spec.log));
    auto next_regs = std::make_unique<B>(build<B>(reg::scope(
        *next_map, 0, spec.size_bytes, spec.root_name.value_or(spec.map_name), spec.scope)));
    if (spec.finalize_on_build)
        next_map->finalize();
    map = std::move(next_map);
    regs = std::move(next_regs);
}

// One owner for map + typed bundle. The heap state never moves, so handles,
// captured register views, and existing C MMIO bridge views remain stable when
// the owning bank is moved. Bundle destruction always precedes map destruction.
template <class B>
class RegisterBank {
  public:
    using Registers = B;

  private:
    struct State {
        RegisterMap map;
        B regs;
        explicit State(RegisterSpaceSpec spec)
            : map(spec.size_bytes, spec.map_name, std::move(spec.log)),
              regs(build<B>(reg::scope(
                  map, 0, spec.size_bytes, spec.root_name.value_or(spec.map_name), spec.scope))) {
            if (spec.finalize_on_build)
                map.finalize();
        }
    };
    std::unique_ptr<State> state_;
    State& state() {
        if (!state_)
            throw Error("register bank is not initialized", ErrorKind::Configuration);
        return *state_;
    }
    const State& state() const {
        if (!state_)
            throw Error("register bank is not initialized", ErrorKind::Configuration);
        return *state_;
    }

  public:
    RegisterBank() = default;
    explicit RegisterBank(RegisterSpaceSpec spec) {
        init(std::move(spec));
    }
    RegisterBank(const RegisterBank&) = delete;
    RegisterBank& operator=(const RegisterBank&) = delete;
    RegisterBank(RegisterBank&&) noexcept = default;
    RegisterBank& operator=(RegisterBank&&) noexcept = default;
    explicit operator bool() const noexcept {
        return bool(state_);
    }
    RegisterBank& init(RegisterSpaceSpec spec) {
        if (state_)
            throw Error("register bank is already initialized", ErrorKind::Configuration);
        auto next = std::make_unique<State>(std::move(spec));
        state_ = std::move(next);
        return *this;
    }
    B* operator->() {
        return &state().regs;
    }
    const B* operator->() const {
        return &state().regs;
    }
    B& operator*() & {
        return state().regs;
    }
    const B& operator*() const& {
        return state().regs;
    }
    B& operator*() && = delete;
    const B& operator*() const&& = delete;
    auto& map() & {
        return state().map;
    }
    const auto& map() const& {
        return state().map;
    }
    auto& map() && = delete;
    const auto& map() const&& = delete;
    // Complete register checkpoint: values plus in-flight read/write state.
    VersionedSnapshot save() const {
        return state().map.dump_state_versioned();
    }
    void load(const VersionedSnapshot& snapshot) {
        state().map.restore_state_versioned(snapshot);
    }
    template <class Archive>
        requires requires { Archive::is_loading; }
    void save(Archive& ar) const {
        static_assert(!Archive::is_loading, "save requires a writer archive");
        auto snapshot = save();
        ar(snapshot);
    }
    template <class Archive>
        requires requires { Archive::is_loading; }
    void load(Archive& ar) {
        static_assert(Archive::is_loading, "load requires a reader archive");
        VersionedSnapshot snapshot;
        ar(snapshot);
        load(snapshot);
    }
    // Enables VMSTATE_FIELDS(reg_bank, ...), without saving callbacks/pointers.
    template <class Archive>
    void VmstateFields(Archive& ar) {
        if constexpr (Archive::is_loading)
            load(ar);
        else
            save(ar);
    }
    // Definitions are in the optional <regfw/vmstate.hpp> adapter, which uses
    // the user's existing vmstate_fields.hpp / module_var_save/load backend.
    void save(void* file) const;
    void load(void* file);
    void Save(void* file) const {
        save(file);
    }
    void Load(void* file) {
        load(file);
    }
    void finish() {
        state().map.finalize();
    }
    auto reset() const {
        return state().regs.reset();
    }
    void clear() noexcept {
        state_.reset();
    }
};
template <class B>
auto make_bank(RegisterSpaceSpec spec) {
    return RegisterBank<B>(std::move(spec));
}
} // namespace reg
