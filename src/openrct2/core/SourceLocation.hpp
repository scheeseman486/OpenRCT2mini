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
 * OPENRCT2MINI: cut 33. C++17 backport of std::source_location. Guard.hpp's
 * Assert() and the OpenGL glCall macro both rely on it for file/line
 * capture at call sites. The shim uses GCC builtins (__builtin_FILE,
 * __builtin_LINE, __builtin_FUNCTION) which are available since GCC 4.8 —
 * same mechanism the C++20 stdlib uses internally.
 *
 *****************************************************************************/

#pragma once

// OPENRCT2MINI: cut 36. <version> is C++20. Probe via __has_include — on
// GCC 8.3 the header doesn't exist; we know we lack std::source_location
// in that case and the fallback path (Compat::SourceLocation) is used.
#if defined(__has_include) && __has_include(<version>)
    #include <version>
#endif

#if defined(__cpp_lib_source_location) && __cpp_lib_source_location >= 201907L

    #include <source_location>

#else

    #include <cstdint>

namespace OpenRCT2::Compat
{
    class source_location
    {
    public:
        static constexpr source_location current(
            const char* fileName = __builtin_FILE(),
            const char* functionName = __builtin_FUNCTION(),
            std::uint_least32_t lineNumber = __builtin_LINE(),
            std::uint_least32_t columnOffset = 0) noexcept
        {
            return source_location(fileName, functionName, lineNumber, columnOffset);
        }

        constexpr source_location() noexcept = default;

        constexpr std::uint_least32_t line() const noexcept { return _line; }
        constexpr std::uint_least32_t column() const noexcept { return _column; }
        constexpr const char* file_name() const noexcept { return _file; }
        constexpr const char* function_name() const noexcept { return _function; }

    private:
        constexpr source_location(
            const char* fileName, const char* functionName, std::uint_least32_t lineNumber,
            std::uint_least32_t columnOffset) noexcept
            : _file(fileName)
            , _function(functionName)
            , _line(lineNumber)
            , _column(columnOffset)
        {
        }

        const char* _file{ "" };
        const char* _function{ "" };
        std::uint_least32_t _line{ 0 };
        std::uint_least32_t _column{ 0 };
    };
} // namespace OpenRCT2::Compat

namespace std
{
    using source_location = ::OpenRCT2::Compat::source_location;
} // namespace std

#endif // __cpp_lib_source_location
