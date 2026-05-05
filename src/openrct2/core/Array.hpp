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
 * OPENRCT2MINI: cut 33. C++17 backport of std::to_array. The source uses it
 * in 19+ places to build constexpr arrays from braced init lists where the
 * element count is auto-deduced. C++17 std::array doesn't have to_array;
 * inlining each use would mean hand-counting elements, error-prone for the
 * larger tables (track descriptors, palette tables, etc.).
 *
 * The C++17 implementation is well-known: helper that uses index_sequence
 * to copy elements from a C array (which is what {a, b, c} matches when
 * the element type is fixed by the template argument).
 *
 *****************************************************************************/

#pragma once

// OPENRCT2MINI: cut 36. <version> is C++20-only — probe with __has_include.
#if defined(__has_include) && __has_include(<version>)
    #include <version>
#endif

#if defined(__cpp_lib_to_array) && __cpp_lib_to_array >= 201907L

    #include <array>

#else

    #include <array>
    #include <cstddef>
    #include <type_traits>
    #include <utility>

namespace OpenRCT2::Compat
{
    template<typename T, std::size_t N, std::size_t... I>
    constexpr std::array<std::remove_cv_t<T>, N> to_array_impl(T (&a)[N], std::index_sequence<I...>)
    {
        return { { a[I]... } };
    }

    template<typename T, std::size_t N, std::size_t... I>
    constexpr std::array<std::remove_cv_t<T>, N> to_array_impl(T (&&a)[N], std::index_sequence<I...>)
    {
        return { { std::move(a[I])... } };
    }

    template<typename T, std::size_t N>
    constexpr std::array<std::remove_cv_t<T>, N> to_array(T (&a)[N])
    {
        return to_array_impl(a, std::make_index_sequence<N>{});
    }

    template<typename T, std::size_t N>
    constexpr std::array<std::remove_cv_t<T>, N> to_array(T (&&a)[N])
    {
        return to_array_impl(std::move(a), std::make_index_sequence<N>{});
    }
} // namespace OpenRCT2::Compat

namespace std
{
    template<typename T, std::size_t N>
    constexpr ::std::array<::std::remove_cv_t<T>, N> to_array(T (&a)[N])
    {
        return ::OpenRCT2::Compat::to_array(a);
    }

    template<typename T, std::size_t N>
    constexpr ::std::array<::std::remove_cv_t<T>, N> to_array(T (&&a)[N])
    {
        return ::OpenRCT2::Compat::to_array(std::move(a));
    }
} // namespace std

#endif // __cpp_lib_to_array
