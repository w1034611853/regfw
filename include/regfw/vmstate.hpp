#pragma once
// Optional adapter. Supply the existing vmstate_fields.hpp and its
// qemu_interface.h on the include path. No VMStateDescription is involved.
#include <vmstate_fields.hpp>
#include "regfw.hpp"

namespace reg {
template <class B>
void RegisterBank<B>::save(void* file) const {
    hac::snapshot::Writer archive(file);
    save(archive);
}
template <class B>
void RegisterBank<B>::load(void* file) {
    hac::snapshot::Reader archive(file);
    load(archive);
}
} // namespace reg
