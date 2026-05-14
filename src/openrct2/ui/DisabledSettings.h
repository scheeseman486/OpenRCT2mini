/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#pragma once

#include <string_view>

// OPENRCT2MINI disabled-settings: per-build UI gate manifest.
//
// Replaces the scattered `#ifdef OPENRCT2MINI` blocks that disabled
// or removed UI elements on the Mini build. The manifest itself is a
// JSON file at config/<build>/disabled_settings.json, hex-included
// into the binary at compile time by cmake/EmbedFileAsArray.cmake.
// The runtime parses it once on first use and answers per-widget /
// per-language queries from each gate site.
//
// Two states are supported:
//
//   * disabled — widget exists in the layout but is greyed out
//     (the on-screen affordance is visible, just inactive). Backed
//     by widgetSetEnabled(*this, idx, false) at the call site.
//
//   * removed  — element is not drawn at all (language filtered out
//     of the dropdown; widget hidden; dropdown entry omitted).
//
// Identifiers follow `Namespace.SYMBOL` form, where Namespace is the
// source class or category (`Options`, `TitleMenu`, `Language`) and
// SYMBOL is the literal WIDX_* enum name or, for languages, the
// IETF locale tag (`ar-EG`, `zh-CN`, …).
//
// Lookups are case-sensitive linear scans of small string vectors —
// the manifests are tiny (a couple dozen entries each) and the
// queries fire only from Options pane onPrepare and similar
// infrequent paths. No hash map needed.

namespace OpenRCT2::Ui::DisabledSettings
{
    // Returns true if `key` (e.g. "Options.WIDX_FULLSCREEN") is
    // listed in the embedded manifest's `disabled[]` array. Used by
    // window onPrepare hooks to swap a static widget-enable into a
    // data-driven check.
    bool isDisabled(std::string_view key);

    // Returns true if `key` is listed in the embedded manifest's
    // `removed[]` array. Currently consumed by the language-filter
    // lookup (`Language.<locale>`); other call sites where the
    // widget is physically absent from the layout (TitleMenu's
    // Open-Content-Folder entry) still rely on compile-time #ifdef
    // because the dropdown index enum can't be made runtime-data-
    // driven without a layout refactor — they're listed in the
    // manifest for documentation but not consulted yet.
    bool isRemoved(std::string_view key);

    // Convenience wrapper: `isRemoved("Language." + code)`. Replaces
    // the hand-rolled IsLanguageHiddenFromDropdown() filter in
    // Options.cpp so the language hide-list lives in the manifest
    // instead of a hardcoded switch.
    bool isLanguageRemoved(std::string_view localeTag);

} // namespace OpenRCT2::Ui::DisabledSettings
