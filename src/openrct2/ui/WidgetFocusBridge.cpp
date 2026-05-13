/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

// OPENRCT2MINI focus-mode-plan / Phase F.5: weak default implementation
// of the bridge function. The openrct2-ui library provides a stronger
// override that consults InputManager state and paints the focus ring;
// the linker picks that strong definition when openrct2-ui is in the
// link line. The CLI build (`openrct2-cli`) links libopenrct2.a alone
// — it has no UI library, no InputManager, no focus state — so it
// falls back to this no-op. Without the weak default, openrct2-cli
// would fail to link with an undefined reference to
// `drawFocusOutlineIfActive`.
//
// Same pattern as a function pointer registration but simpler: no
// startup-order hazard, no extra state, no need to remember to
// register. Pure compile-time linker decision.

#include "WidgetFocusBridge.h"

namespace OpenRCT2::Ui
{
    __attribute__((weak)) void drawFocusOutlineIfActive(
        OpenRCT2::Drawing::RenderTarget& /*rt*/, const OpenRCT2::WindowBase& /*window*/)
    {
        // No-op default. Real implementation lives in
        // src/openrct2-ui/input/WidgetFocus.cpp.
    }
} // namespace OpenRCT2::Ui
