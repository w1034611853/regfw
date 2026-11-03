#pragma once

/**
 * @file vmstate_fields.hpp
 * @brief C++20 字段式快照，使用 module_var_save/module_var_load 作为后端。
 *
 * 类的 public 区域声明 VMSTATE_FIELDS(成员...)，自动提供 Save(void*) / Load(void*)。
 * 无需继承，无需登记成员类型或偏移。流中按清单顺序保存，不按成员内存位置保存。
 * 所有自定义值类型都需登记字段，或已有公开的 Save/Load；不默认转储未知对象。
 * 已验证布局的寄存器 union、固定 POD 块可显式用 hac::snapshot::Raw(member) 登记。
 * 单个位域不能绑定引用；应登记其对应的整数存储或明确选用 Raw 包含它的寄存器。
 *
 * 支持：算术类型、enum、原生数组、array、vector（含 bool）、deque、list、string、
 * map、unordered_map、set、unordered_set、queue、stack、pair、tuple、optional、variant。
 * 容器的元素递归处理。动态容器元素、optional 新值、variant 新分支须可默认构造。
 * map/set 的键在保存时复制，因此键须可复制；queue/stack 须可复制。
 * stack 按栈底到栈顶保存，恢复后栈顶和弹出顺序保持一致。
 * 比较器、哈希器、相等谓词、分配器不写入流，沿用接收对象的现有配置。
 * unordered 容器不保证不同运行间输出顺序相同。variant 分支序号依赖声明顺序。
 *
 * 原始对象指针和 unique_ptr<T> 表示已初始化的子对象：保存空标记及对象状态，
 * 不保存地址，不 new/delete，不改变指向；Load 前空/非空拓扑必须与快照一致。
 * unique_ptr<T[]>、void*、函数指针、shared_ptr、span 不自动支持。
 * 父指针、共享引用和临时视图不要登记；每个子对象沿归属关系登记一次。
 * 检测当前指针递归路径上的环，不提供通用对象图和别名重建。
 * 容器恢复会重建元素，外部指向旧元素的引用须在全部加载后重新建立。
 *
 * 特殊类型可直接实现 template<class Archive> void VmstateFields(Archive& ar)，
 * 使用 ar(成员...) 递归处理；Archive::is_loading 用于区分方向。
 * 可选：定义 void AfterSnapshotLoad()，本对象登记字段全部恢复后自动调用。
 * 依赖兄弟模块的重连，应在最外层对象的 AfterSnapshotLoad 中完成。
 *
 * 数值使用 host 原生表示；适合同一架构、相同字段定义的快照。
 * 不兼容旧 VmstateBlock 文件格式；增加/删除/重排字段须由上层快照版本处理。
 * Raw 不能包含指针、容器或运行时资源；trivially_copyable 只是一项必要检查。
 *
 * Load 非事务式：失败时部分字段可能已修改，不能直接继续运行半恢复设备。
 * 后端 module_var_load 必须完整读取或报告失败；本层无法从 void 返回值检测短读。
 * Save/Load 时对象须静止或由调用方同步；本层不加锁，不重建 QEMU 注册关系。
 * 异常须在 C++/C 回调边界内处理。
 */

#include "qemu_interface.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <stack>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace hac::snapshot {

// 单次顶层 Save/Load 的限制。需要更大快照时用 Save/Load 的第三个参数调整。
struct Limits {
    std::uint64_t max_container_elements = 1'048'576;
    std::uint64_t max_total_bytes = 256ull * 1024 * 1024;
    std::size_t max_depth = 128;
};

// 显式登记固定内存块；调用者负责保证块内没有指针/容器/运行时资源。
template<class T>
struct RawRef { T& value; };

template<class T>
[[nodiscard]] RawRef<T> Raw(T& value) noexcept
{
    static_assert(std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>,
                  "Raw requires trivially copyable non-pointer storage");
    return {value};
}

namespace detail {
enum class Kind { Other, Array, Sequence, String, Map, Set, Queue, Stack, Optional,
                  Variant, Tuple, Unique, Raw };
template<class T> struct Shape { static constexpr Kind kind = Kind::Other; };
template<class T, std::size_t N> struct Shape<std::array<T, N>> { static constexpr Kind kind = Kind::Array; };
template<class T, class A> struct Shape<std::vector<T, A>> { static constexpr Kind kind = Kind::Sequence; };
template<class T, class A> struct Shape<std::deque<T, A>> { static constexpr Kind kind = Kind::Sequence; };
template<class T, class A> struct Shape<std::list<T, A>> { static constexpr Kind kind = Kind::Sequence; };
template<class C, class Tr, class A> struct Shape<std::basic_string<C, Tr, A>> { static constexpr Kind kind = Kind::String; };
template<class K, class V, class C, class A> struct Shape<std::map<K, V, C, A>> { static constexpr Kind kind = Kind::Map; };
template<class K, class V, class H, class E, class A> struct Shape<std::unordered_map<K, V, H, E, A>> { static constexpr Kind kind = Kind::Map; };
template<class K, class C, class A> struct Shape<std::set<K, C, A>> { static constexpr Kind kind = Kind::Set; };
template<class K, class H, class E, class A> struct Shape<std::unordered_set<K, H, E, A>> { static constexpr Kind kind = Kind::Set; };
template<class T, class C> struct Shape<std::queue<T, C>> { static constexpr Kind kind = Kind::Queue; };
template<class T, class C> struct Shape<std::stack<T, C>> { static constexpr Kind kind = Kind::Stack; };
template<class T> struct Shape<std::optional<T>> { static constexpr Kind kind = Kind::Optional; };
template<class... T> struct Shape<std::variant<T...>> { static constexpr Kind kind = Kind::Variant; };
template<class... T> struct Shape<std::tuple<T...>> { static constexpr Kind kind = Kind::Tuple; };
template<class A, class B> struct Shape<std::pair<A, B>> { static constexpr Kind kind = Kind::Tuple; };
template<class T, class D> struct Shape<std::unique_ptr<T, D>> { static constexpr Kind kind = Kind::Unique; };
template<class T> struct Shape<RawRef<T>> { static constexpr Kind kind = Kind::Raw; };
template<class> inline constexpr bool unsupported = false;

// 保存原比较器/哈希配置和分配器，创建一个空的关联容器。
template<class T>
T EmptyAssociative(const T& value)
{
    if constexpr (requires { value.hash_function(); }) {
        T result(0, value.hash_function(), value.key_eq(), value.get_allocator());
        result.max_load_factor(value.max_load_factor());
        return result;
    } else {
        return T(value.key_comp(), value.get_allocator());
    }
}

class ArchiveBase {
protected:
    void* file_;
    Limits limits_;
    std::uint64_t bytes_ = 0;
    std::size_t depth_ = 0;
    std::vector<const void*> pointer_path_;

    ArchiveBase(void* file, Limits limits) : file_(file), limits_(limits) {}

    void Account(std::size_t size)
    {
        if (size > limits_.max_total_bytes - bytes_) {
            throw std::length_error("snapshot byte limit exceeded");
        }
        bytes_ += size;
    }

    void CheckCount(std::uint64_t count) const
    {
        if (count > limits_.max_container_elements ||
            count > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error("snapshot container count exceeds limit");
        }
    }

    struct DepthGuard {
        ArchiveBase& owner;
        explicit DepthGuard(ArchiveBase& value) : owner(value)
        {
            if (owner.depth_ >= owner.limits_.max_depth) {
                throw std::length_error("snapshot nesting limit exceeded");
            }
            ++owner.depth_;
        }
        ~DepthGuard() { --owner.depth_; }
    };

    struct PointerGuard {
        ArchiveBase& owner;
        PointerGuard(ArchiveBase& value, const void* address) : owner(value)
        {
            for (const auto existing : owner.pointer_path_) {
                if (existing == address) {
                    throw std::logic_error("snapshot pointer cycle; exclude back references");
                }
            }
            owner.pointer_path_.push_back(address);
        }
        ~PointerGuard() { owner.pointer_path_.pop_back(); }
    };
};
} // namespace detail

class Writer : private detail::ArchiveBase {
public:
    static constexpr bool is_loading = false;
    explicit Writer(void* file, Limits limits = {}) : ArchiveBase(file, limits) {}

    // 按参数顺序递归保存。参数表达式只求值一次。
    template<class... Ts>
    void operator()(Ts&&... fields) { (One(fields), ...); }

private:
    void Bytes(const void* data, std::size_t size)
    {
        Account(size);
        auto ptr = static_cast<const std::uint8_t*>(data);
        while (size != 0) {
            const auto chunk = static_cast<std::uint32_t>(
                size > UINT32_MAX ? UINT32_MAX : size);
            module_var_save(file_, ptr, chunk);
            ptr += chunk;
            size -= chunk;
        }
    }

    void Count(std::size_t count)
    {
        CheckCount(count);
        auto n = static_cast<std::uint32_t>(count);
        One(n);
    }

    template<class P>
    void Pointer(P& pointer)
    {
        bool present = static_cast<bool>(pointer);
        One(present);
        if (present) {
            detail::ArchiveBase::PointerGuard guard(*this, std::addressof(*pointer));
            One(*pointer);
        }
    }

    template<class T>
    void One(T& value)
    {
        using U = std::remove_cv_t<T>;
        constexpr auto kind = detail::Shape<U>::kind;
        DepthGuard guard(*this);
        static_assert(!std::is_volatile_v<T>, "snapshot does not read volatile storage");

        if constexpr (std::is_same_v<U, bool>) {
            std::uint8_t b = value ? 1 : 0;
            Bytes(&b, sizeof b);
        } else if constexpr (std::is_arithmetic_v<U> || std::is_enum_v<U>) {
            Bytes(&value, sizeof value);
        } else if constexpr (kind == detail::Kind::Raw) {
            Bytes(&value.value, sizeof value.value);
        } else if constexpr (std::is_pointer_v<U>) {
            static_assert(std::is_object_v<std::remove_pointer_t<U>>,
                          "only initialized object pointers can be snapshotted");
            Pointer(value);
        } else if constexpr (kind == detail::Kind::Unique) {
            static_assert(!std::is_array_v<typename U::element_type> &&
                          requires { *value; }, "unique_ptr arrays are not supported");
            Pointer(value);
        } else if constexpr (std::is_array_v<U> || kind == detail::Kind::Array) {
            for (auto& element : value) One(element);
        } else if constexpr (kind == detail::Kind::Sequence || kind == detail::Kind::String) {
            Count(value.size());
            // vector<bool> 元素是代理，须先读出 bool。
            if constexpr (std::is_same_v<typename U::value_type, bool>) {
                for (bool element : value) One(element);
            } else {
                for (auto& element : value) One(element);
            }
        } else if constexpr (kind == detail::Kind::Map) {
            Count(value.size());
            for (auto& entry : value) {
                typename U::key_type key = entry.first;
                One(key);
                One(entry.second);
            }
        } else if constexpr (kind == detail::Kind::Set) {
            Count(value.size());
            for (const auto& entry : value) {
                typename U::key_type key = entry;
                One(key);
            }
        } else if constexpr (kind == detail::Kind::Queue) {
            Count(value.size());
            U copy = value;
            while (!copy.empty()) {
                One(copy.front());
                copy.pop();
            }
        } else if constexpr (kind == detail::Kind::Stack) {
            static_assert(std::is_copy_constructible_v<typename U::value_type>,
                          "snapshot stack elements must be copy constructible");
            Count(value.size());
            U copy = value;
            // stack 只能访问栈顶；先取出副本，再反向保存，避免改变原栈。
            // deque<bool> 没有 vector<bool> 的代理引用问题。
            std::deque<typename U::value_type> elements;
            while (!copy.empty()) {
                elements.emplace_back(copy.top());
                copy.pop();
            }
            for (auto it = elements.rbegin(); it != elements.rend(); ++it) {
                One(*it);
            }
        } else if constexpr (kind == detail::Kind::Optional) {
            bool present = value.has_value();
            One(present);
            if (present) One(*value);
        } else if constexpr (kind == detail::Kind::Variant) {
            if (value.valueless_by_exception()) {
                throw std::logic_error("cannot save valueless variant");
            }
            auto index = static_cast<std::uint32_t>(value.index());
            One(index);
            std::visit([this](auto& element) { One(element); }, value);
        } else if constexpr (kind == detail::Kind::Tuple) {
            std::apply([this](auto&... fields) { (*this)(fields...); }, value);
        } else if constexpr (std::is_same_v<U, std::monostate>) {
            // 无字段。
        } else if constexpr (requires { value.VmstateFields(*this); }) {
            value.VmstateFields(*this);
        } else if constexpr (requires { value.Save(file_); }) {
            // 渐进迁移：允许调用既有的 Save/Load。旧实现自行负责 I/O 检查及格式。
            value.Save(file_);
        } else {
            static_assert(detail::unsupported<U>,
                "unsupported snapshot type: add VMSTATE_FIELDS or audited Raw(field)");
        }
    }
};

class Reader : private detail::ArchiveBase {
public:
    static constexpr bool is_loading = true;
    explicit Reader(void* file, Limits limits = {}) : ArchiveBase(file, limits) {}

    // 按参数顺序递归加载；字段必须可写，错误时抛异常。
    template<class... Ts>
    void operator()(Ts&&... fields) { (One(fields), ...); }

private:
    void Bytes(void* data, std::size_t size)
    {
        Account(size);
        auto ptr = static_cast<std::uint8_t*>(data);
        while (size != 0) {
            const auto chunk = static_cast<std::uint32_t>(
                size > UINT32_MAX ? UINT32_MAX : size);
            module_var_load(file_, ptr, chunk);
            ptr += chunk;
            size -= chunk;
        }
    }

    std::uint32_t Count()
    {
        std::uint32_t n = 0;
        One(n);
        CheckCount(n);
        return n;
    }

    template<class P>
    void Pointer(P& pointer)
    {
        bool present = false;
        One(present);
        if (present != static_cast<bool>(pointer)) {
            throw std::runtime_error("snapshot pointer topology mismatch: initialize child objects before Load");
        }
        if (present) {
            PointerGuard guard(*this, std::addressof(*pointer));
            One(*pointer);
        }
    }

    template<std::size_t I = 0, class V>
    void Variant(V& value, std::uint32_t index)
    {
        if constexpr (I < std::variant_size_v<V>) {
            if (index == I) {
                if (value.index() != I) value.template emplace<I>();
                One(std::get<I>(value));
            } else {
                Variant<I + 1>(value, index);
            }
        } else {
            throw std::runtime_error("invalid snapshot variant index");
        }
    }

    template<class T>
    void One(T& value)
    {
        using U = std::remove_cv_t<T>;
        constexpr auto kind = detail::Shape<U>::kind;
        DepthGuard guard(*this);
        static_assert(!std::is_const_v<T> && !std::is_volatile_v<T>,
                      "snapshot destination must be mutable non-volatile storage");

        if constexpr (std::is_same_v<U, bool>) {
            std::uint8_t b = 0;
            Bytes(&b, sizeof b);
            if (b > 1) throw std::runtime_error("invalid snapshot bool");
            value = b != 0;
        } else if constexpr (std::is_arithmetic_v<U> || std::is_enum_v<U>) {
            Bytes(&value, sizeof value);
        } else if constexpr (kind == detail::Kind::Raw) {
            Bytes(&value.value, sizeof value.value);
        } else if constexpr (std::is_pointer_v<U>) {
            static_assert(std::is_object_v<std::remove_pointer_t<U>>,
                          "only initialized object pointers can be snapshotted");
            Pointer(value);
        } else if constexpr (kind == detail::Kind::Unique) {
            static_assert(requires { *value; }, "unique_ptr arrays are not supported");
            Pointer(value);
        } else if constexpr (std::is_array_v<U> || kind == detail::Kind::Array) {
            for (auto& element : value) One(element);
        } else if constexpr (kind == detail::Kind::Sequence || kind == detail::Kind::String) {
            auto n = Count();
            U restored(value.get_allocator());
            if (n > restored.max_size()) throw std::length_error("snapshot container too large");
            restored.resize(n);
            if constexpr (std::is_same_v<typename U::value_type, bool>) {
                for (auto it = restored.begin(); it != restored.end(); ++it) {
                    bool element = false;
                    One(element);
                    *it = element;
                }
            } else {
                for (auto& element : restored) One(element);
            }
            value.swap(restored);
        } else if constexpr (kind == detail::Kind::Map) {
            auto n = Count();
            U restored = detail::EmptyAssociative(value);
            if (n > restored.max_size()) throw std::length_error("snapshot map too large");
            for (std::uint32_t i = 0; i < n; ++i) {
                typename U::key_type key{};
                One(key);
                auto [it, inserted] = restored.try_emplace(std::move(key));
                if (!inserted) throw std::runtime_error("duplicate snapshot map key");
                One(it->second);
            }
            value.swap(restored);
        } else if constexpr (kind == detail::Kind::Set) {
            auto n = Count();
            U restored = detail::EmptyAssociative(value);
            if (n > restored.max_size()) throw std::length_error("snapshot set too large");
            for (std::uint32_t i = 0; i < n; ++i) {
                typename U::key_type key{};
                One(key);
                if (!restored.insert(std::move(key)).second) {
                    throw std::runtime_error("duplicate snapshot set key");
                }
            }
            value.swap(restored);
        } else if constexpr (kind == detail::Kind::Queue) {
            auto n = Count();
            // 拷贝后清空，保留底层容器分配器；要求 queue 可复制。
            U restored = value;
            while (!restored.empty()) restored.pop();
            for (std::uint32_t i = 0; i < n; ++i) {
                restored.emplace();
                One(restored.back());
            }
            value.swap(restored);
        } else if constexpr (kind == detail::Kind::Stack) {
            static_assert(std::is_copy_constructible_v<typename U::value_type>,
                          "snapshot stack elements must be copy constructible");
            static_assert(std::is_default_constructible_v<typename U::value_type>,
                          "snapshot stack elements must be default constructible");
            auto n = Count();
            // 沿用 queue 的拷贝后清空策略，不直接修改目标栈。
            U restored = value;
            while (!restored.empty()) restored.pop();
            for (std::uint32_t i = 0; i < n; ++i) {
                restored.emplace();
                if constexpr (std::is_same_v<typename U::value_type, bool>) {
                    bool element = false;
                    One(element);
                    restored.top() = element;
                } else {
                    One(restored.top());
                }
            }
            value.swap(restored);
        } else if constexpr (kind == detail::Kind::Optional) {
            bool present = false;
            One(present);
            if (!present) value.reset();
            else {
                if (!value) value.emplace();
                One(*value);
            }
        } else if constexpr (kind == detail::Kind::Variant) {
            std::uint32_t index = 0;
            One(index);
            Variant(value, index);
        } else if constexpr (kind == detail::Kind::Tuple) {
            std::apply([this](auto&... fields) { (*this)(fields...); }, value);
        } else if constexpr (std::is_same_v<U, std::monostate>) {
            // 无字段。
        } else if constexpr (requires { value.VmstateFields(*this); }) {
            value.VmstateFields(*this);
            if constexpr (requires { value.AfterSnapshotLoad(); }) {
                value.AfterSnapshotLoad();
            }
        } else if constexpr (requires { value.Load(file_); }) {
            value.Load(file_);
        } else {
            static_assert(detail::unsupported<U>,
                "unsupported snapshot type: add VMSTATE_FIELDS or audited Raw(field)");
        }
    }
};

// 保存/恢复单个根对象或容器。自定义限制只覆盖使用本框架递归处理的字段；
// 调用旧式 Save/Load 后，旧实现的流量和递归不计入本次限制。
template<class T>
void Save(void* file, T& value, Limits limits = {})
{
    Writer archive(file, limits);
    archive(value);
}

template<class T>
void Load(void* file, T& value, Limits limits = {})
{
    Reader archive(file, limits);
    archive(value);
}

} // namespace hac::snapshot

// 放在类的 public 区域。已有 Save/Load 时使用 VMSTATE_FIELDS_ONLY，保留手写入口。
// 为延迟实例化模块指针的序列化，VmstateFields 本身为函数模板。
#ifdef VMSTATE_FIELDS
#error "Remove the old VMSTATE_FIELDS definition before including vmstate_fields.hpp"
#endif
#define VMSTATE_FIELDS_ONLY(...)                                      \
    template<class VmstateArchive>                                    \
    void VmstateFields(VmstateArchive& vmstate_archive)                 \
    {                                                                 \
        vmstate_archive(__VA_ARGS__);                                  \
    }

#define VMSTATE_FIELDS(...)                                           \
    VMSTATE_FIELDS_ONLY(__VA_ARGS__)                                   \
    void Save(void* vmstate_file)                                      \
    {                                                                 \
        ::hac::snapshot::Save(vmstate_file, *this);                     \
    }                                                                 \
    void Load(void* vmstate_file)                                      \
    {                                                                 \
        ::hac::snapshot::Load(vmstate_file, *this);                     \
    }
