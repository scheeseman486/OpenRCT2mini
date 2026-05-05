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
 * OPENRCT2MINI: cut 33. C++17 backport of std::span. The Miyoo Mini's
 * OnionUI runs glibc 2.28; the only Linux cross-toolchain matching that
 * glibc with C++20 support is a multi-day Buildroot fork. Backporting the
 * project to C++17 against the existing community toolchain (GCC 8.3) is
 * cheaper. The dominant C++20 feature in the source is std::span (87 uses
 * across 49 files).
 *
 * This header is the project-wide shim: when the host compiler has
 * std::span (C++20+), we just include <span>. When it doesn't (GCC 8.3
 * with -std=gnu++17), we provide a drop-in implementation under the same
 * name via a using-alias. Source code carries on writing `std::span<T>`
 * unchanged.
 *
 * The shim covers only the subset of std::span this project uses:
 * dynamic-extent, pointer + size, iterators, subspan/first/last, [],
 * front/back, size/empty/data. No static-extent template specialization,
 * no static_assert on contiguous-iterator concepts, no SFINAE corner
 * cases. Keeps the implementation small and the diff narrow.
 *
 *****************************************************************************/

#pragma once

// OPENRCT2MINI: cut 36. <version> is C++20 (it's where __cpp_lib_span lives).
// On GCC 8.3 the header doesn't exist. Probe for it via __has_include first;
// on toolchains where it's missing we know we don't have std::span either.
#if defined(__has_include) && __has_include(<version>)
    #include <version>
#endif

#if defined(__cpp_lib_span) && __cpp_lib_span >= 202002L

    #include <span>

#else

    #include <array>
    #include <cstddef>
    #include <iterator>
    #include <type_traits>
    #include <vector>

namespace OpenRCT2::Compat
{
    template<typename T>
    class span
    {
    private:
        T* _data{ nullptr };
        std::size_t _size{ 0 };

    public:
        using element_type = T;
        using value_type = std::remove_cv_t<T>;
        using size_type = std::size_t;
        using difference_type = std::ptrdiff_t;
        using pointer = T*;
        using const_pointer = const T*;
        using reference = T&;
        using const_reference = const T&;
        using iterator = T*;
        using reverse_iterator = std::reverse_iterator<iterator>;

        constexpr span() noexcept = default;

        constexpr span(T* ptr, size_type count) noexcept
            : _data(ptr)
            , _size(count)
        {
        }

        constexpr span(T* first, T* last) noexcept
            : _data(first)
            , _size(static_cast<size_type>(last - first))
        {
        }

        template<std::size_t N>
        constexpr span(T (&arr)[N]) noexcept
            : _data(arr)
            , _size(N)
        {
        }

        template<typename U, std::size_t N, std::enable_if_t<std::is_convertible_v<U (*)[], T (*)[]>, int> = 0>
        constexpr span(std::array<U, N>& arr) noexcept
            : _data(arr.data())
            , _size(N)
        {
        }

        template<typename U, std::size_t N, std::enable_if_t<std::is_convertible_v<const U (*)[], T (*)[]>, int> = 0>
        constexpr span(const std::array<U, N>& arr) noexcept
            : _data(arr.data())
            , _size(N)
        {
        }

        template<typename U, typename Alloc, std::enable_if_t<std::is_convertible_v<U (*)[], T (*)[]>, int> = 0>
        constexpr span(std::vector<U, Alloc>& vec) noexcept
            : _data(vec.data())
            , _size(vec.size())
        {
        }

        template<typename U, typename Alloc, std::enable_if_t<std::is_convertible_v<const U (*)[], T (*)[]>, int> = 0>
        constexpr span(const std::vector<U, Alloc>& vec) noexcept
            : _data(vec.data())
            , _size(vec.size())
        {
        }

        // Allow span<const T> to be constructed from span<T> (covariance for const).
        template<typename U, std::enable_if_t<std::is_convertible_v<U (*)[], T (*)[]>, int> = 0>
        constexpr span(const span<U>& other) noexcept
            : _data(other.data())
            , _size(other.size())
        {
        }

        // OPENRCT2MINI: generic contiguous-container constructor — covers
        // sfl::static_vector and other vendor containers exposing data()/size().
        // SFINAE'd so it doesn't shadow the std::vector / std::array constructors
        // and only matches when no other constructor would.
        template<
            typename Container,
            typename = std::enable_if_t<
                !std::is_same_v<std::remove_cv_t<std::remove_reference_t<Container>>, span>
                && !std::is_array_v<std::remove_reference_t<Container>>>,
            typename = decltype(std::declval<Container&>().data()),
            typename = decltype(std::declval<Container&>().size())>
        constexpr span(Container& c) noexcept
            : _data(c.data())
            , _size(c.size())
        {
        }

        template<
            typename Container,
            typename = std::enable_if_t<
                !std::is_same_v<std::remove_cv_t<std::remove_reference_t<Container>>, span>
                && !std::is_array_v<std::remove_reference_t<Container>>>,
            typename = decltype(std::declval<const Container&>().data()),
            typename = decltype(std::declval<const Container&>().size())>
        constexpr span(const Container& c) noexcept
            : _data(c.data())
            , _size(c.size())
        {
        }

        constexpr span(const span&) noexcept = default;
        constexpr span& operator=(const span&) noexcept = default;

        constexpr T* data() const noexcept { return _data; }
        constexpr size_type size() const noexcept { return _size; }
        constexpr size_type size_bytes() const noexcept { return _size * sizeof(T); }
        constexpr bool empty() const noexcept { return _size == 0; }

        constexpr T& operator[](size_type idx) const noexcept { return _data[idx]; }
        constexpr T& front() const noexcept { return _data[0]; }
        constexpr T& back() const noexcept { return _data[_size - 1]; }

        constexpr iterator begin() const noexcept { return _data; }
        constexpr iterator end() const noexcept { return _data + _size; }
        constexpr reverse_iterator rbegin() const noexcept { return reverse_iterator(end()); }
        constexpr reverse_iterator rend() const noexcept { return reverse_iterator(begin()); }

        constexpr span<T> subspan(size_type offset) const noexcept
        {
            return span<T>(_data + offset, _size - offset);
        }
        constexpr span<T> subspan(size_type offset, size_type count) const noexcept
        {
            return span<T>(_data + offset, count);
        }

        constexpr span<T> first(size_type count) const noexcept { return span<T>(_data, count); }
        constexpr span<T> last(size_type count) const noexcept
        {
            return span<T>(_data + (_size - count), count);
        }
    };

    template<typename T> span(T*, std::size_t) -> span<T>;
    template<typename T> span(T*, T*) -> span<T>;
    template<typename T, std::size_t N> span(T (&)[N]) -> span<T>;
    template<typename T, std::size_t N> span(std::array<T, N>&) -> span<T>;
    template<typename T, std::size_t N> span(const std::array<T, N>&) -> span<const T>;
    template<typename T, typename A> span(std::vector<T, A>&) -> span<T>;
    template<typename T, typename A> span(const std::vector<T, A>&) -> span<const T>;
} // namespace OpenRCT2::Compat

// Inject into std so existing source can keep writing `std::span<T>`. Adding
// declarations to namespace std is technically UB per the C++ standard, but
// using-aliases for missing types is a long-tested pattern (Boost, ranges-v3,
// many vendor codebases). It works on every C++ compiler and stays inert when
// std::span is the real thing (the #if guard above).
namespace std
{
    template<typename T> using span = ::OpenRCT2::Compat::span<T>;
} // namespace std

#endif // __cpp_lib_span
