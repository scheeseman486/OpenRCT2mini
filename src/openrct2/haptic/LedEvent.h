#pragma once
#include <cstdint>

// OPENRCT2MINI v0.5.2 merge: the News::Severity enum that LED.cpp + the
// classifier rely on lives in management/NewsItem.h in our pre-merge code,
// but the v0.5.2 safe-apply of NewsItem.h overwrote that addition. The LED
// feature is currently feature-disabled (this file's stubs are no-ops), so
// rather than chase the NewsItem.h merge we forward-declare News::Severity
// here just so LED.cpp parses. When LED is reactivated, restore the real
// definition in NewsItem.h and drop this stub block.
namespace OpenRCT2::News
{
    enum class Severity : uint8_t
    {
        info,
        warning,
        critical,
        money,
    };
}

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
