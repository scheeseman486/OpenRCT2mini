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
 * OPENRCT2MINI: cut 34. CP1252 → UTF-8 lookup. Used by the legacy RCT1/RCT2
 * save importer to translate Windows-1252-encoded scenario names. Replaces
 * a 30 MB ICU dependency for what's actually a 256-entry table.
 *
 * Bytes 0x00..0x7F are ASCII-identical to UTF-8.
 * Bytes 0x80..0x9F are the Microsoft-extension range (smart quotes, em dash,
 * Euro sign, etc.).
 * Bytes 0xA0..0xFF are the Latin-1 supplement.
 * Five bytes (0x81, 0x8D, 0x8F, 0x90, 0x9D) are unassigned in CP1252.
 *
 *****************************************************************************/

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace OpenRCT2::Compat
{
    // Returns the UTF-8 encoding of CP1252 byte `b`. The output is at most 3
    // bytes (largest entries are codepoints up to U+FFFF, which encode as 3
    // UTF-8 bytes). Unmapped bytes return the U+FFFD replacement character.
    inline void appendCp1252Byte(std::string& out, uint8_t b)
    {
        // ASCII pass-through.
        if (b < 0x80)
        {
            out.push_back(static_cast<char>(b));
            return;
        }

        // Per CP1252:
        // 0x80..0x9F maps to a hand-table of typographic chars (Microsoft extensions).
        // 0xA0..0xFF maps directly to U+00A0..U+00FF (Latin-1 supplement).
        static constexpr uint16_t kCp1252Upper[0x80] = {
            // 0x80..0x9F — Microsoft extensions
            0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, // 80..87
            0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD, // 88..8F
            0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, // 90..97
            0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178, // 98..9F
            // 0xA0..0xFF — Latin-1 supplement (identity to U+00A0..U+00FF)
            0x00A0, 0x00A1, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7, // A0..A7
            0x00A8, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF, // A8..AF
            0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7, // B0..B7
            0x00B8, 0x00B9, 0x00BA, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF, // B8..BF
            0x00C0, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7, // C0..C7
            0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF, // C8..CF
            0x00D0, 0x00D1, 0x00D2, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x00D7, // D0..D7
            0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0x00DE, 0x00DF, // D8..DF
            0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7, // E0..E7
            0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF, // E8..EF
            0x00F0, 0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x00F7, // F0..F7
            0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x00FE, 0x00FF, // F8..FF
        };

        const uint16_t cp = kCp1252Upper[b - 0x80];
        if (cp < 0x800)
        {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    inline std::string cp1252ToUtf8(std::string_view src)
    {
        std::string out;
        out.reserve(src.size());
        for (unsigned char b : src)
            appendCp1252Byte(out, b);
        return out;
    }
} // namespace OpenRCT2::Compat
