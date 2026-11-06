#ifndef REGFW_QEMU_BRIDGE_H
#define REGFW_QEMU_BRIDGE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Compatible C boundary; return QEMU MemTxResult bits (0,1,2,4).
 * Install read/write in a device's read_with_attrs/write_with_attrs wrappers.
 * No dependency on QEMU headers, enums or a specific QEMU version. */
typedef uint32_t (*regfw_read_fn)(void*, uint64_t, uint64_t*, unsigned);
typedef uint32_t (*regfw_write_fn)(void*, uint64_t, uint64_t, unsigned);
typedef struct regfw_mmio_bridge {
    void* opaque;
    regfw_read_fn read;
    regfw_write_fn write;
} regfw_mmio_bridge;
#ifdef __cplusplus
}
#include "regfw.hpp"
#include "async.hpp"
namespace reg {
// A non-owning view: C opaque points at the map, not this temporary wrapper.
struct QemuBridge {
    RegisterMap* map;
    explicit QemuBridge(RegisterMap& value) noexcept : map(&value) {
    }
    regfw_mmio_bridge c_api() const noexcept {
        return {map,
                [](void* p, uint64_t a, uint64_t* data, unsigned s) noexcept -> uint32_t {
                    return static_cast<RegisterMap*>(p)->qemu_mmio_read_sized(a, data, s);
                },
                [](void* p, uint64_t a, uint64_t data, unsigned s) noexcept -> uint32_t {
                    return static_cast<RegisterMap*>(p)->qemu_mmio_write_sized(a, data, s);
                }};
    }
};
inline regfw_mmio_bridge make_qemu_bridge(RegisterMap& map) noexcept {
    return QemuBridge(map).c_api();
}
template <ExecutionMode Mode>
inline regfw_mmio_bridge make_qemu_bridge(RegisterEndpoint<Mode>& endpoint) noexcept {
    return {&endpoint,
            [](void* p, uint64_t a, uint64_t* value, unsigned size) noexcept -> uint32_t {
                auto result = static_cast<RegisterEndpoint<Mode>*>(p)->read(a, size);
                *value = result.value;
                return result.qemu_bits();
            },
            [](void* p, uint64_t a, uint64_t value, unsigned size) noexcept -> uint32_t {
                return static_cast<RegisterEndpoint<Mode>*>(p)->write(a, value, size).qemu_bits();
            }};
}
} // namespace reg
#endif
#endif
