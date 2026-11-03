// The native single-pass layout builder reports geometry violations when a
// layout is installed rather than instantiating a large consteval plan.
#include <regfw/regfw.hpp>
#include <iostream>
#include <stdexcept>
#include <string_view>

struct R : reg::RegEntityBundle {
    REG_BUNDLE(R);
    REG(X, std::uint32_t, reg::at(0));
    REG_SCHEMA(&R::X);
};
struct Range : reg::Bundle {
    REG_BUNDLE(Range);
    REG_ENTITY(r, R);
    REG_LAYOUT([](auto l) {
        return l.address_space("S").map(l.view("V", 64).map(l.r), 0xfffffffffffffffcULL);
    });
};
struct Nested : reg::Bundle {
    REG_BUNDLE(Nested);
    REG_ENTITY(r, R);
    REG_LAYOUT([](auto l) {
        return l.address_space("S").map(
            l.view("A", 64).identity(1).map(l.view("B", 64).identity(2).map(l.r)), 0x1000);
    });
};
struct OutOfRangeIndex : reg::Bundle {
    REG_BUNDLE(OutOfRangeIndex);
    REG_REPEAT_ENTITY(r, R, 4);
    REG_LAYOUT([](auto l) {
        return l.address_space("S").map(l.view("V", 64).map(l.r[4]), 0);
    });
};
struct DynamicReg : reg::RegEntityBundle {
    REG_BUNDLE(DynamicReg);
    REG_DYNAMIC(X, std::uint32_t, reg::at(0));
};
struct DynamicInStatic : reg::Bundle {
    REG_BUNDLE(DynamicInStatic);
    REG_ENTITY(r, DynamicReg);
    REG_LAYOUT([](auto l) {
        return l.address_space("S").map(l.view("V", 64).map(l.r), 0);
    });
};
template <class B>
void expects_error(std::string_view expected) {
    try {
        reg::RegisterBank<B> bank(reg::space());
    } catch (const reg::Error& e) {
        if (std::string_view(e.what()).find(expected) != std::string_view::npos)
            return;
        throw;
    } catch (std::string_view e) {
        if (e.find(expected) != std::string_view::npos)
            return;
        throw;
    } catch (const char* e) {
        if (std::string_view(e).find(expected) != std::string_view::npos)
            return;
        throw;
    }
    throw std::runtime_error("native layout failed to reject an invalid declaration");
}
int main() {
    try {
        expects_error<Range>("static view outside parent/unaligned");
        expects_error<Nested>("nested static identity domains unsupported");
        expects_error<OutOfRangeIndex>("symbolic entity index out of range");
        expects_error<DynamicInStatic>("REG_DYNAMIC requires dynamic REG_LAYOUT");
        std::cout << "native layout validation: 4/4 rejected\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    } catch (const char* e) {
        std::cerr << e << "\n";
        return 1;
    } catch (std::string_view e) {
        std::cerr << e << "\n";
        return 1;
    }
}
