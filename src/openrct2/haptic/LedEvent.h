#pragma once
#include <cstdint>

// OPENRCT2MINI v0.5.2 merge: News::Severity now lives in its
// canonical home (management/NewsItem.h) — the safe-apply that
// clobbered NewsItem.h had us forward-declare it here as a temporary
// stub; phase 5d restored the enum properly. The full GetSeverity
// classifier impl + per-Item severity cache field on News::Item are
// still pending re-merge (see plans/docs/upstream-merge-v0.5.2-status.md).
// LED engine itself remains stubbed (setActive / forceClear / Update
// are no-ops) so the LED Options UI shell compiles + links; the actual
// SDL2 controller-LED writes will be re-wired in a follow-up.
#include "../management/NewsItem.h"

namespace OpenRCT2::Led
{
    void OnNewsItemAdded();
    void OnNewsItemDismissed(int32_t);
    void Update();
    void Clear();
    // OPENRCT2MINI v0.5.2 merge: stubs to keep LED Options window + Test
    // button compiling while the underlying engine path is feature-disabled.
    void setActive(News::Severity);
    void forceClear();
}
