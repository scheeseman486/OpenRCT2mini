/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "OpenRCT2.h"

#include "core/EnumUtils.hpp"

StartupAction gOpenRCT2StartupAction = StartupAction::title;
utf8 gOpenRCT2StartupActionPath[512] = { 0 };
u8string gCustomUserDataPath = {};
u8string gCustomOpenRCT2DataPath = {};
u8string gCustomRCT1DataPath = {};
u8string gCustomRCT2DataPath = {};
u8string gCustomPassword = {};
u8string gSilentRecordingName = {};

// OPENRCT2MINI defaults-export: when non-empty (set by the --dump-defaults
// CLI flag), main() should bootstrap the three defaults-producing systems
// (Config / ShortcutManager / Haptic), serialise each one's built-in
// defaults to <dir>/{config.ini, shortcuts.json, rumble.json}, and exit
// instead of launching the game. Used at packaging time to capture the
// per-build seeds that get embedded into the binary in P3.
u8string gDumpDefaultsPath = {};

bool gOpenRCT2Headless = false;
bool gOpenRCT2NoGraphics = false;

bool gOpenRCT2ShowChangelog;
StringId gOpenRCT2PendingParkLoadError = kStringIdNone;  // OPENRCT2MINI revision 70c
bool gOpenRCT2SilentBreakpad;

uint32_t gCurrentDrawCount = 0;
LegacyScene gLegacyScene;
uint32_t gScreenAge;
PromptMode gSavePromptMode;

bool gSilentReplays = false;

bool isInEditorMode()
{
    return EnumValue(gLegacyScene) >= EnumValue(LegacyScene::scenarioEditor);
}

bool isInTrackDesignerOrManager()
{
    return gLegacyScene == LegacyScene::trackDesigner || gLegacyScene == LegacyScene::trackDesignsManager;
}
