#pragma once
#include <string_view>
namespace reg::layout_titles {
inline constexpr std::string_view bundle = "BUNDLE / ADDRESS LAYOUT";
inline constexpr std::string_view aliases = "STATIC ALIAS MAP";
inline constexpr std::string_view shared = "SHARED STORAGE / STATIC ADDRESS TARGETS";
inline constexpr std::string_view canonical = "CANONICAL ADDRESS REGIONS / SHARED ACCESS";
inline constexpr std::string_view paths = "ACCESS PATHS → SAME CANONICAL RANGE";
inline constexpr std::string_view identity = "IDENTITY WINDOWS";
inline constexpr std::string_view index = "INDEX MAPPING RULES (one representative per repeat)";
inline constexpr std::string_view callbacks = "REGISTERED MAPPING FUNCTIONS";
inline constexpr std::string_view effective =
    "EFFECTIVE INSTANCE ROUTING (symbolic; no resolver is executed)";
inline constexpr std::string_view warnings = "LAYOUT WARNINGS";
} // namespace reg::layout_titles
