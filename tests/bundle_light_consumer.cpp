// Plain consumer translation unit: registers, handles and callbacks without
// the constexpr layout compiler. This TU must not include <regfw/regfw.hpp>.
#include "bundle_light.hpp"
namespace light {
void arm_callbacks(reg::RegisterBank<Device>& bank, std::uint32_t& observed) {
    for (unsigned v = 0; v < 4; ++v)
        bank->vf[v].CTL.on_write([&](std::uint32_t old, const reg::RegAccessInfo& i) {
            observed = old + i.stored_after;
        });
    bank->pf.CTL.on_read([&](std::uint32_t value, const reg::RegAccessInfo&) {
        return value ^ 0x33u;
    });
}
} // namespace light
