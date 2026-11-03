#pragma once
// Declaration payload shared by the light and full device headers. Keep this
// file free of direct framework includes so the entry cost is measured in the
// wrapper, not here.
#ifndef ELEMENTS
#define ELEMENTS 64
#endif
namespace declare_device {
template <unsigned N, class T = std::uint32_t>
struct Arrays : reg::RegEntityBundle {
    REG_BUNDLE(Arrays);
    static constexpr auto gap = std::uint64_t(N) * sizeof(T) + 64;
#define ITEM(K) REG(A##K, T, reg::at(K* gap).array(reg::dims(N), reg::strides(sizeof(T))));
    ITEM(0) ITEM(1) ITEM(2) ITEM(3) ITEM(4) ITEM(5) ITEM(6) ITEM(7) ITEM(8) ITEM(9)
#undef ITEM
};
struct Device : reg::Bundle {
    REG_BUNDLE_DECL(Device);
    using Business = Arrays<ELEMENTS>;
    REG_REPEAT_ENTITY(vf, Business, 9);
    REG_LAYOUT_DECL();
};
} // namespace declare_device
