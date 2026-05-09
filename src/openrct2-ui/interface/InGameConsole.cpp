/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "InGameConsole.h"

#include "../UiStringIds.h"
#include "../windows/Windows.h"
#include "Theme.h"

#include <algorithm>
#include <cstring>
#include <openrct2/Context.h>
#include <openrct2/Input.h>
#include <openrct2/Version.h>
#include <openrct2/config/Config.h>
#include <openrct2/core/UTF8.h>
#include <openrct2/drawing/ColourMap.h>
#include <openrct2/drawing/Drawing.String.h>
#include <openrct2/drawing/Drawing.h>
#include <openrct2/drawing/Rectangle.h>
#include <openrct2/drawing/Text.h>
#include <openrct2/interface/ColourWithFlags.h>
#include <openrct2/interface/Viewport.h>
#include <openrct2/interface/Window.h>
#include <openrct2/localisation/Language.h>
#include <openrct2/localisation/LocalisationService.h>
#include <openrct2/profiling/Bench.h>

using namespace OpenRCT2;
using namespace OpenRCT2::Drawing;
using namespace OpenRCT2::Ui;

static InGameConsole _inGameConsole;

static FontStyle InGameConsoleGetFontStyle()
{
    return (Config::Get().interface.consoleSmallFont ? FontStyle::small : FontStyle::medium);
}

static int32_t InGameConsoleGetLineHeight()
{
    return FontGetLineHeight(InGameConsoleGetFontStyle());
}

void InGameConsole::WriteInitial()
{
    InteractiveConsole::WriteLine(OPENRCT2_NAME " " kOpenRCT2Version);
    InteractiveConsole::WriteLine(LanguageGetString(STR_CONSOLE_HELPER_TEXT));
    InteractiveConsole::WriteLine("");
    WritePrompt();
}

void InGameConsole::WritePrompt()
{
    InteractiveConsole::WriteLine("> ");
}

void InGameConsole::Input(ConsoleInput input)
{
    if (_isCommandAwaitingCompletion)
    {
        // Do not process input while a command is running
        return;
    }

    switch (input)
    {
        case ConsoleInput::LineClear:
            ClearInput();
            RefreshCaret();
            break;
        case ConsoleInput::LineExecute:
            if (_consoleCurrentLine[0] != '\0')
            {
                HistoryAdd(_consoleCurrentLine);

                // Append text we are executing to prompt line
                _consoleLines.back().first.append(_consoleCurrentLine);

                Execute(_consoleCurrentLine);
                if (IsExecuting())
                {
                    _isCommandAwaitingCompletion = true;
                }
                else
                {
                    WritePrompt();
                }
                ClearInput();
                RefreshCaret();
            }
            ScrollToEnd();
            break;
        case ConsoleInput::HistoryPrevious:
            if (_consoleHistoryIndex > 0)
            {
                _consoleHistoryIndex--;
                _consoleCurrentLine = _consoleHistory[_consoleHistoryIndex];
            }
            _consoleTextInputSession->Length = UTF8Length(_consoleCurrentLine.c_str());
            _consoleTextInputSession->SelectionStart = _consoleCurrentLine.size();
            RefreshCaret(_consoleTextInputSession->SelectionStart);
            break;
        case ConsoleInput::HistoryNext:
            if (_consoleHistoryIndex + 1 < _consoleHistory.size())
            {
                _consoleHistoryIndex++;
                _consoleCurrentLine = _consoleHistory[_consoleHistoryIndex];
                _consoleTextInputSession->Length = UTF8Length(_consoleCurrentLine.c_str());
                _consoleTextInputSession->SelectionStart = _consoleCurrentLine.size();
            }
            else
            {
                _consoleHistoryIndex = _consoleHistory.size();
                ClearInput();
            }
            RefreshCaret(_consoleTextInputSession->SelectionStart);
            break;
        case ConsoleInput::ScrollPrevious:
        {
            int32_t scrollAmt = GetNumVisibleLines() - 1;
            Scroll(scrollAmt);
            break;
        }
        case ConsoleInput::ScrollNext:
        {
            int32_t scrollAmt = GetNumVisibleLines() - 1;
            Scroll(-scrollAmt);
            break;
        }
        default:
            break;
    }
}

void InGameConsole::ClearInput()
{
    _consoleCurrentLine.clear();
    if (_isOpen)
    {
        _consoleTextInputSession = ContextStartTextInput(_consoleCurrentLine, kConsoleInputSize);
    }
}

// OPENRCT2MINI: live mirror from OSK. Called every frame while the OSK
// is up over a console session. Updates _consoleCurrentLine and the
// caret position so the prompt line in the console region renders
// what the user is typing on the OSK.
void InGameConsole::OskMirrorBuffer(std::string_view text, size_t caret)
{
    _consoleCurrentLine.assign(text);
    if (caret > _consoleCurrentLine.size())
        caret = _consoleCurrentLine.size();
    RefreshCaret(caret);
    if (_consoleTextInputSession != nullptr)
    {
        _consoleTextInputSession->SelectionStart = caret;
        _consoleTextInputSession->Length = UTF8Length(_consoleCurrentLine.c_str());
    }
}

// OPENRCT2MINI: OSK Start submitted a line. Behave as if the user hit
// Enter on a real keyboard: install the typed text and run
// LineExecute, which appends to history, runs the command, scrolls
// the output, and clears the input via ClearInput().
void InGameConsole::OskSubmitLine(std::string_view text)
{
    _consoleCurrentLine.assign(text);
    RefreshCaret(_consoleCurrentLine.size());
    Input(ConsoleInput::LineExecute);
    // ClearInput inside LineExecute re-calls ContextStartTextInput so
    // the keyboard-driven path can keep typing. The OSK is driving
    // input here, not the keyboard, and the OSK already disables SDL
    // text-input on open — so re-arm the disable, otherwise SDL
    // resumes feeding TEXTINPUT events into _consoleCurrentLine and
    // every subsequent device-button press briefly flashes raw chars
    // in the prompt before the OSK frame mirror overwrites them.
    ContextStopTextInput();
}

void InGameConsole::HistoryAdd(const u8string& src)
{
    if (_consoleHistory.size() >= kConsoleHistorySize)
    {
        _consoleHistory.pop_front();
    }
    _consoleHistory.push_back(src);
    _consoleHistoryIndex = _consoleHistory.size();
}

void InGameConsole::ScrollToEnd()
{
    const int32_t maxLines = GetNumVisibleLines();
    if (maxLines == 0)
        _consoleScrollPos = 0;
    else
        _consoleScrollPos = std::max<int32_t>(0, static_cast<int32_t>(_consoleLines.size()) - maxLines);
}

void InGameConsole::RefreshCaret(size_t position)
{
    _consoleCaretTicks = 0;
    _selectionStart = position;

    auto text = u8string_view{ _consoleCurrentLine }.substr(0, _selectionStart);
    _caretScreenPosX = getStringWidth(text, InGameConsoleGetFontStyle(), true);
}

void InGameConsole::Scroll(int32_t linesToScroll)
{
    const int32_t maxVisibleLines = GetNumVisibleLines();
    const int32_t numLines = static_cast<int32_t>(_consoleLines.size());
    if (numLines > maxVisibleLines)
    {
        int32_t maxScrollValue = numLines - maxVisibleLines;
        _consoleScrollPos = std::clamp<int32_t>(_consoleScrollPos - linesToScroll, 0, maxScrollValue);
    }
}

void InGameConsole::Clear()
{
    _consoleLines.clear();
    ScrollToEnd();
}

void InGameConsole::ClearLine()
{
    _consoleCurrentLine[0] = 0;
    RefreshCaret();
}

void InGameConsole::Open()
{
    if (!_isInitialised)
    {
        WriteInitial();
        _isInitialised = true;
    }

    _isOpen = true;
    ScrollToEnd();
    RefreshCaret();
    _consoleTextInputSession = ContextStartTextInput(_consoleCurrentLine, kConsoleInputSize);
    // OPENRCT2MINI: spawn the on-screen keyboard so the user can type
    // commands without a physical keyboard. The OSK calls back into
    // OskMirrorBuffer / OskSubmitLine for the live mirror and commit.
    OpenRCT2::Ui::Windows::OskOpenForConsole();
}

void InGameConsole::Close()
{
    _consoleTextInputSession = nullptr;
    _isOpen = false;
    Invalidate();
    ContextStopTextInput();
    // OPENRCT2MINI: tear down the OSK along with the console.
    // CloseByClass is idempotent if the OSK is already closing.
    OpenRCT2::Ui::Windows::OskClose();
}

void InGameConsole::Hide()
{
    Close();
}

void InGameConsole::Toggle()
{
    if (_isOpen)
    {
        Close();
    }
    else
    {
        Open();
    }
}

void InGameConsole::WriteLine(const std::string& input, FormatToken colourFormat)
{
    std::string line;
    std::size_t splitPos = 0;
    std::size_t stringOffset = 0;
    while (splitPos != std::string::npos)
    {
        splitPos = input.find('\n', stringOffset);
        line = input.substr(stringOffset, splitPos - stringOffset);
        _consoleLines.emplace_back(line, colourFormat);
        stringOffset = splitPos + 1;
    }

    if (_consoleLines.size() > kConsoleMaxLines)
    {
        const std::size_t linesToErase = _consoleLines.size() - kConsoleMaxLines;
        _consoleLines.erase(_consoleLines.begin(), _consoleLines.begin() + linesToErase);
    }
}

void InGameConsole::Invalidate() const
{
    GfxSetDirtyBlocks({ _consoleTopLeft, _consoleBottomRight });
}

void InGameConsole::Update()
{
    _consoleTopLeft = { 0, 0 };
    // OPENRCT2MINI: clamp console region to fit above the OSK when
    // it's up. The OSK uses a compact 214 px layout in console mode
    // (no edit strip — the console renders its own prompt) so we
    // get the saved 26 px back as console real estate.
    int32_t bottomY = 322;
    if (OpenRCT2::Ui::Windows::OskIsActive())
    {
        const int32_t oskH = OpenRCT2::Ui::Windows::OskGetActiveHeight();
        bottomY = ContextGetHeight() - oskH;
    }
    _consoleBottomRight = { ContextGetWidth(), bottomY };

    if (_isOpen)
    {
        // When scrolling the map, the console pixels get copied... therefore invalidate the screen
        WindowBase* mainWindow = WindowGetMain();
        if (mainWindow != nullptr)
        {
            Viewport* mainViewport = WindowGetViewport(mainWindow);
            if (mainViewport != nullptr)
            {
                if (_lastMainViewport != mainViewport->viewPos)
                {
                    _lastMainViewport = mainViewport->viewPos;

                    GfxInvalidateScreen();
                }
            }
        }
    }

    if (_isCommandAwaitingCompletion && !IsExecuting())
    {
        WritePrompt();
        _isCommandAwaitingCompletion = false;
    }

    // OPENRCT2MINI rev 95b: bench finish handshake. The `bench` command
    // hides the console at start; when the run completes, Bench latches
    // _reportPending and we pop the console back open with the summary
    // line + log path so the user sees the result immediately.
    if (Profiling::Bench::hasPendingReport())
    {
        Open();
        InteractiveConsole::WriteLine(Profiling::Bench::getLastReport());
        const auto& logPath = Profiling::Bench::getLastLogPath();
        if (!logPath.empty())
        {
            InteractiveConsole::WriteLine("Detailed log: " + logPath);
        }
        WritePrompt();
        Profiling::Bench::markReportConsumed();
    }

    // Flash the caret
    _consoleCaretTicks = (_consoleCaretTicks + 1) % 30;
}

void InGameConsole::Draw(RenderTarget& rt) const
{
    // OPENRCT2MINI rev 95d: render the "Benchmark running..." overlay
    // even when the console is hidden. This is the only on-screen
    // signal that the game is in bench mode (inputs are dropped and
    // the user can't see the console). Drawn first so it falls under
    // the console panel if the user re-opens it manually somehow.
    if (Profiling::Bench::isActive())
    {
        ColourWithFlags benchColour = { OpenRCT2::Drawing::Colour::white, {} };
        if (!LocalisationService_UseTrueTypeFont())
        {
            benchColour.flags.set(ColourFlag::withOutline, true);
        }
        const FontStyle benchStyle = FontStyle::medium;
        drawText(rt, ScreenCoordsXY{ 4, 4 }, "Benchmark running...", { benchColour, benchStyle });
    }

    if (!_isOpen)
        return;

    // Set font
    ColourWithFlags textColour = { ThemeGetColour(WindowClass::console, 1).colour, {} };
    const FontStyle style = InGameConsoleGetFontStyle();
    const int32_t lineHeight = InGameConsoleGetLineHeight();
    const int32_t maxLines = GetNumVisibleLines();

    // TTF looks far better without the outlines
    if (!LocalisationService_UseTrueTypeFont())
    {
        textColour.flags.set(ColourFlag::withOutline, true);
    }

    Invalidate();

    // Give console area a translucent effect.
    Rectangle::filter(rt, { _consoleTopLeft, _consoleBottomRight }, FilterPaletteID::palette51);

    // Make input area more opaque.
    Rectangle::filter(
        rt, { { _consoleTopLeft.x, _consoleBottomRight.y - lineHeight - 10 }, _consoleBottomRight - ScreenCoordsXY{ 0, 1 } },
        FilterPaletteID::palette51);

    // Paint background colour.
    auto backgroundColour = ThemeGetColour(WindowClass::console, 0);
    Rectangle::fillInset(
        rt, { _consoleTopLeft, _consoleBottomRight }, backgroundColour, Rectangle::BorderStyle::outset,
        Rectangle::FillBrightness::light, Rectangle::FillMode::none);
    Rectangle::fillInset(
        rt, { _consoleTopLeft + ScreenCoordsXY{ 1, 1 }, _consoleBottomRight - ScreenCoordsXY{ 1, 1 } }, backgroundColour,
        Rectangle::BorderStyle::inset);

    std::string lineBuffer;
    auto screenCoords = _consoleTopLeft + ScreenCoordsXY{ kConsoleEdgePadding, kConsoleEdgePadding };

    // Draw text inside console
    for (std::size_t i = 0; i < _consoleLines.size() && i < static_cast<size_t>(maxLines); i++)
    {
        const size_t index = i + _consoleScrollPos;
        if (_consoleLines[index].second == FormatToken::colourWindow2)
        {
            // This is something of a hack to ensure the text is actually black
            // as opposed to a desaturated grey
            if (textColour.colour == OpenRCT2::Drawing::Colour::black)
            {
                drawText(rt, screenCoords, "{BLACK}", { textColour, style });
                drawText(rt, screenCoords, _consoleLines[index].first, { kColourNull, style, { TextPaintFlag::noFormatting } });
            }
            else
            {
                drawText(rt, screenCoords, _consoleLines[index].first, { textColour, style, { TextPaintFlag::noFormatting } });
            }
        }
        else
        {
            std::string lineColour = FormatTokenToStringWithBraces(_consoleLines[index].second);
            drawText(rt, screenCoords, lineColour, { textColour, style });
            drawText(rt, screenCoords, _consoleLines[index].first, { kColourNull, style, { TextPaintFlag::noFormatting } });
        }

        screenCoords.y += lineHeight;
    }

    screenCoords.y = _consoleBottomRight.y - lineHeight - kConsoleEdgePadding - 1;

    // Draw current line
    if (textColour.colour == OpenRCT2::Drawing::Colour::black)
    {
        drawText(rt, screenCoords, "{BLACK}", { textColour, style });
        drawText(rt, screenCoords, _consoleCurrentLine, { kColourNull, style, { TextPaintFlag::noFormatting } });
    }
    else
    {
        drawText(rt, screenCoords, _consoleCurrentLine, { textColour, style, { TextPaintFlag::noFormatting } });
    }

    // Draw caret
    if (_consoleCaretTicks < kConsoleCaretFlashThreshold)
    {
        auto caret = screenCoords + ScreenCoordsXY{ _caretScreenPosX, lineHeight };
        auto caretColour = getColourMap(textColour.colour).lightest;
        Rectangle::fill(rt, { caret, caret + ScreenCoordsXY{ kConsoleCaretWidth, 1 } }, caretColour);
    }

    // What about border colours?
    auto borderColour1 = getColourMap(backgroundColour.colour).light;
    auto borderColour2 = getColourMap(backgroundColour.colour).midDark;

    // Input area top border
    Rectangle::fill(
        rt,
        { { _consoleTopLeft.x, _consoleBottomRight.y - lineHeight - 11 },
          { _consoleBottomRight.x, _consoleBottomRight.y - lineHeight - 11 } },
        borderColour1);
    Rectangle::fill(
        rt,
        { { _consoleTopLeft.x, _consoleBottomRight.y - lineHeight - 10 },
          { _consoleBottomRight.x, _consoleBottomRight.y - lineHeight - 10 } },
        borderColour2);

    // Input area bottom border
    Rectangle::fill(
        rt, { { _consoleTopLeft.x, _consoleBottomRight.y - 1 }, { _consoleBottomRight.x, _consoleBottomRight.y - 1 } },
        borderColour1);
    Rectangle::fill(rt, { { _consoleTopLeft.x, _consoleBottomRight.y }, _consoleBottomRight }, borderColour2);
}

// Calculates the amount of visible lines, based on the console size, excluding the input line.
int32_t InGameConsole::GetNumVisibleLines() const
{
    const int32_t lineHeight = InGameConsoleGetLineHeight();
    const int32_t consoleHeight = _consoleBottomRight.y - _consoleTopLeft.y;
    if (consoleHeight == 0)
        return 0;
    const int32_t drawableHeight = consoleHeight - 2 * lineHeight - 4; // input line, separator - padding
    return drawableHeight / lineHeight;
}
