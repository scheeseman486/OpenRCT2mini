/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#pragma once

// OPENRCT2MINI focus-mode-plan / Phase F.5: bridge between libopenrct2's
// shared window-paint code and the openrct2-ui WidgetFocus module.
//
// `libopenrct2` (core, contains Window.cpp) can't directly include
// openrct2-ui headers — that'd be a circular library dependency. The
// existing escape hatch is to declare a function in a core-side header
// and let openrct2-ui provide the implementation; the final link
// resolves the symbol. `GetWindowManager()` is the canonical example.
//
// This header declares the one bridge function `WindowDrawSingle` needs:
// "draw the focus ring on this window if the focus state thinks it
// belongs here, otherwise no-op." The UI side knows the focus state
// (InputManager owns it) and the WidgetFocus draw recipe; the core
// side just calls into the bridge at the right moment in the paint
// pipeline.

namespace OpenRCT2
{
    struct WindowBase;
    namespace Drawing
    {
        struct RenderTarget;
    }
} // namespace OpenRCT2

namespace OpenRCT2::Ui
{
    // Draw the focus ring on `window` if it matches the InputManager's
    // currently-focused window AND the resolved active context is
    // widgetFocus. Called from WindowDrawSingle (Window.cpp) AFTER
    // the window's own onDraw, so the ring sits on top of whatever
    // the window renders. Safe to call unconditionally — it's a
    // single-windowMgr + InputManager state check + (maybe)
    // fillInset, all cheap.
    void drawFocusOutlineIfActive(OpenRCT2::Drawing::RenderTarget& rt, const OpenRCT2::WindowBase& window);
} // namespace OpenRCT2::Ui
