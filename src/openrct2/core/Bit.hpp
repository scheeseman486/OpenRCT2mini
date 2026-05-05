/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *
 *****************************************************************************
 *
 * OPENRCT2MINI: cut 33. C++17 backport companion to Span.hpp. Source uses
 * std::popcount (5 sites) and std::has_single_bit (1 site) — both C++20,
 * unavailable in GCC 8.3. When the compiler exposes <bit>'s C++20 ops
 * (__cpp_lib_bitops), include the real header; otherwise provide the
 * narrow subset the project actually needs, backed by GCC builtins.
 *
 *****************************************************************************/

#pragma once

// OPENRCT2MINI: cut 36. <version> is C++20-only — probe with __has_include.
#if defined(__has_include) && __has_include(<version>)
    #include <version>
#endif

#if defined(__cpp_lib_bitops) && __cpp_lib_bitops >= 201907L

    #include <bit>

#else

    #include <climits>
    #include <cstdint>
    #include <type_traits>

namespace OpenRCT2::Compat
{
    template<typename T, std::enable_if_t<std::is_unsigned_v<T>, int> = 0>
    constexpr int popcount(T x) noexcept
    {
        if constexpr (sizeof(T) <= sizeof(unsigned int))
            return __builtin_popcount(static_cast<unsigned int>(x));
        else if constexpr (sizeof(T) == sizeof(unsigned long))
            return __builtin_popcountl(static_cast<unsigned long>(x));
        else
            return __builtin_popcountll(static_cast<unsigned long long>(x));
    }

    template<typename T, std::enable_if_t<std::is_unsigned_v<T>, int> = 0>
    constexpr bool has_single_bit(T x) noexcept
    {
        return x != 0 && (x & (x - 1)) == 0;
    }
} // namespace OpenRCT2::Compat

namespace std
{
    template<typename T> constexpr int popcount(T x) noexcept
    {
        return ::OpenRCT2::Compat::popcount(x);
    }
    template<typename T> constexpr bool has_single_bit(T x) noexcept
    {
        return ::OpenRCT2::Compat::has_single_bit(x);
    }
} // namespace std

#endif // __cpp_lib_bitops
