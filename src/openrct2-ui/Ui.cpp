/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "Ui.h"

#include "SDLException.h"
#include "UiContext.h"
#include "audio/AudioContext.h"
#include "drawing/BitmapReader.h"

#include <memory>
// OPENRCT2MINI cut 41: backtrace + signal handlers for crash localisation.
#if defined(__linux__)
#include <csignal>
#include <execinfo.h>
#include <unistd.h>
#endif
#include <openrct2/Context.h>
#include <openrct2/Diagnostic.h>
#include <openrct2/Game.h>  // OPENRCT2MINI: gPowerOffSaveRequested
#include <openrct2/MiniDebug.h>  // OPENRCT2MINI revision 64 — gated debug logging
#include <openrct2/OpenRCT2.h>
#include <openrct2/PlatformEnvironment.h>
#include <openrct2/audio/AudioContext.h>
#include <openrct2/command_line/CommandLine.hpp>
#include <openrct2/platform/Platform.h>
#include <openrct2/ui/UiContext.h>
// OPENRCT2MINI defaults-export: includes for the --dump-defaults
// handler at the top of main. Pulls in Config / ShortcutManager /
// Haptic so we can bootstrap each system's defaults without booting
// the full game context.
#include <openrct2/config/Config.h>
#include <openrct2/core/Path.hpp>
#include <openrct2/haptic/HapticEvent.h>
#include "input/ShortcutManager.h"
#include <filesystem>

#include <cstdio>
#include <cstdlib>
#include <new>
#if defined(__GLIBC__)
    #include <malloc.h>
    #include <mcheck.h>
#endif

#ifdef __EMSCRIPTEN__
    #include <emscripten.h>
#endif

using namespace OpenRCT2;
using namespace OpenRCT2::Audio;
using namespace OpenRCT2::Ui;

template<typename T>
static std::shared_ptr<T> ToShared(std::unique_ptr<T>&& src)
{
    return std::shared_ptr<T>(std::move(src));
}

// OPENRCT2MINI: out-of-memory diagnostic. Default behaviour is std::terminate / SIGKILL,
// which on the device gives the user no idea what happened. Print a clear message
// from /proc/self/status and exit cleanly so the user (and crash logs) can attribute
// the failure to OOM rather than a generic crash.
[[noreturn]] static void OpenRCT2miniOnOutOfMemory()
{
    // Avoid any further allocations — read /proc/self/status directly.
    char buf[2048] = {};
    if (auto* f = std::fopen("/proc/self/status", "r"))
    {
        // Capture-then-discard pattern: GCC's warn_unused_result is not
        // suppressed by (void)-cast (long-standing PR 66425), so we have
        // to assign to a [[maybe_unused]] variable. Short reads are fine —
        // buffer is zero-initialised and we _Exit immediately afterwards.
        [[maybe_unused]] auto got = std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
    }
    std::fputs("\n[OPENRCT2MINI] Out of memory!\n", stderr);
    std::fputs("[OPENRCT2MINI] /proc/self/status:\n", stderr);
    std::fputs(buf, stderr);
    std::fputs("\n[OPENRCT2MINI] Aborting cleanly. Save your work and reduce park complexity if you can.\n", stderr);
    std::_Exit(2);
}

/**
 * Main entry point for non-Windows systems. Windows instead uses its own DLL proxy.
 */
#if defined(_MSC_VER) && !defined(__DISABLE_DLL_PROXY__)
int NormalisedMain(int argc, const char** argv)
#else
int main(int argc, const char** argv)
#endif
{
    // OPENRCT2MINI: revision 39g / 64. Per-step early-init checkpoints —
    // covered command-line parsing, mallopt, BitmapReader registration,
    // PlatformEnvironment construction, Context constructor, and
    // Context::Initialise. Gated behind OPENRCT2MINI_DEBUG via MiniDebug.h
    // — release builds compile the lambda body to a no-op and the
    // compiler elides the call sites entirely under -O2/-O3.
    auto kpt = []([[maybe_unused]] const char* tag) {
        MINI_DBG_LOG("checkpoint: %s\n", tag);
    };
    kpt("main entered");

    // OPENRCT2MINI: cut 41. Install a SIGSEGV/SIGABRT/SIGBUS/SIGFPE handler
    // that dumps a stack backtrace via execinfo.h before re-raising. Without
    // this a device crash is just `signal=SIGSEGV` in the launcher footer
    // and we have no idea where in 24 MB of binary it died. The handler
    // uses async-signal-safe APIs (write, backtrace, backtrace_symbols_fd)
    // and re-raises the signal with the default action so core dumps still
    // get written if ulimit -c is non-zero.
#if defined(__linux__)
    {
        struct SegvHandler {
            static void Handle(int sig)
            {
                // Async-signal-safe best-effort writes. Capture into a
                // [[maybe_unused]] variable because (void)-cast does NOT
                // suppress GCC's warn_unused_result on ::write (PR 66425).
                // We're about to re-raise the signal anyway — failure to
                // write a few diagnostic bytes is irrelevant.
                [[maybe_unused]] ssize_t wr;
                static const char prefix[] = "\n[OPENRCT2MINI] *** signal ";
                wr = ::write(STDERR_FILENO, prefix, sizeof(prefix) - 1);
                char num[8] = {0};
                int n = sig;
                int len = 0;
                if (n == 0) num[len++] = '0';
                else {
                    char tmp[8]; int t = 0;
                    while (n > 0 && t < 7) { tmp[t++] = '0' + (n % 10); n /= 10; }
                    while (t > 0) num[len++] = tmp[--t];
                }
                wr = ::write(STDERR_FILENO, num, len);
                wr = ::write(STDERR_FILENO, " — backtrace:\n", 14);
                void* frames[40];
                int got = ::backtrace(frames, 40);
                ::backtrace_symbols_fd(frames, got, STDERR_FILENO);
                wr = ::write(STDERR_FILENO, "[OPENRCT2MINI] *** end backtrace\n", 32);
                (void)wr;
                // Re-raise with default action so the kernel still dumps
                // a core (subject to ulimit) and the parent shell sees the
                // same exit status it would have without the handler.
                ::signal(sig, SIG_DFL);
                ::raise(sig);
            }
        };
        ::signal(SIGSEGV, SegvHandler::Handle);
        ::signal(SIGBUS, SegvHandler::Handle);
        ::signal(SIGFPE, SegvHandler::Handle);
        ::signal(SIGABRT, SegvHandler::Handle);
        ::signal(SIGILL, SegvHandler::Handle);

        // OPENRCT2MINI: poweroff handler. The Miyoo Mini launcher kills
        // running games by sending SIGTERM (and may follow with SIGHUP /
        // SIGINT depending on launcher version) when the user holds the
        // power button. We just flip an atomic flag here — the save runs
        // from the main loop in HandlePowerOffSaveIfRequested() since
        // SaveGameWithName allocates / opens files / takes locks, none
        // of which is async-signal-safe. The handler does not re-raise:
        // letting the loop exit cleanly is the goal, not crashing.
        //
        // host-restoration-plan §1d: Mini-only. On host SIGINT is the
        // developer's Ctrl-C and SIGTERM is normal process shutdown —
        // we don't want to silently coerce a park save out of either.
        // The OOM handler and SIGSEGV/SIGBUS/SIGFPE/SIGABRT/SIGILL
        // backtrace handlers above stay unconditional; they're useful
        // diagnostics on any platform.
#ifdef OPENRCT2MINI
        struct PowerOffHandler {
            static void Handle(int /*sig*/)
            {
                gPowerOffSaveRequested.store(true, std::memory_order_relaxed);
            }
        };
        ::signal(SIGTERM, PowerOffHandler::Handle);
        ::signal(SIGHUP, PowerOffHandler::Handle);
        ::signal(SIGINT, PowerOffHandler::Handle);
#endif
    }
    kpt("signal handlers installed");
#endif

    // OPENRCT2MINI: install OOM handler before any allocation happens.
    std::set_new_handler(&OpenRCT2miniOnOutOfMemory);
    kpt("set_new_handler ok");
    // OPENRCT2MINI: clamp glibc to 2 malloc arenas. Glibc default is 8×ncores, which on
    // Miyoo Mini's dual-core CPU is 16, and on a typical dev host 64+. Each arena has its
    // own free-list and reserves heap pages. Worker threads (audio, network, jobpool) each
    // grab one, so anonymous heap balloons by tens of MB even at idle. 2 arenas matches
    // the device's actual core count and saves ~27 MB of anonymous RSS at the title screen.
#if defined(__GLIBC__) && defined(OPENRCT2MINI)
    // host-restoration-plan §1d: Mini-only. On host the default 8×ncores
    // arenas are fine — desktop has plenty of address space.
    mallopt(M_ARENA_MAX, 2);
    kpt("mallopt arena_max=2 ok");
#endif
#if defined(__GLIBC__)
    // OPENRCT2MINI: cut 24 reverted. Lowering M_MMAP_THRESHOLD made [heap] look smaller
    // by routing medium allocations through mmap, but anonymous total stayed identical
    // — memory just moved between accounting buckets. Keep glibc default (128 KiB).
    // OPENRCT2MINI diagnostic: if MALLOC_TRACE=path is set, glibc writes a malloc/free
    // log to that path. Process with `mtrace ./openrct2 path` to find leaks.
    mtrace();
    kpt("mtrace ok");
#endif
#ifdef __EMSCRIPTEN__
    MAIN_THREAD_EM_ASM({
        specialHTMLTargets["!canvas"] = Module.canvas;
        Module.canvas.addEventListener("contextmenu", function(e) { e.preventDefault(); });
    });
#endif
    int32_t rc = EXIT_SUCCESS;
    kpt("calling CommandLineRun");
    int runGame = CommandLineRun(argv, argc);
    kpt("CommandLineRun returned");
    RegisterBitmapReader();
    kpt("RegisterBitmapReader ok");

    // OPENRCT2MINI defaults-export: --dump-defaults short-circuit.
    // The CLI flag stashes its target directory on gDumpDefaultsPath
    // and returns EXITCODE_CONTINUE so we can intercept here, after
    // basic process bootstrap (signal handlers, malloc tuning,
    // RegisterBitmapReader for any image-touching code paths) but
    // BEFORE the heavyweight context/audio/UI construction below.
    //
    // We bootstrap a minimal PlatformEnvironment, then synthesise
    // each system's built-in defaults to disk:
    //   * Config::SetDefaults populates the global Config from the
    //     in-source DefaultIniReader (post-P4: from an embedded ini
    //     blob; for now this still produces the canonical defaults).
    //     Config::SaveToPath then serialises to <dir>/config.ini.
    //   * ShortcutManager ctor calls registerDefaultShortcuts, which
    //     populates each shortcut's `standard` AND `current` arrays.
    //     saveUserBindings(path) writes `current` to JSON — so the
    //     dump captures the full defaults table.
    //   * Haptic::writeDefaultProfilesTo wraps seedDefaults +
    //     saveProfilesToDisk(path) in one call (we added it for
    //     this purpose — see HapticEvent.h).
    //
    // After the dump we return EXIT_SUCCESS without booting the
    // game. The handler lives here in openrct2-ui (not in the
    // openrct2 lib that owns RootCommands.cpp) because
    // ShortcutManager is openrct2-ui code; the CLI parser sits one
    // library down and can't reach it.
    if (runGame == EXITCODE_CONTINUE && !gDumpDefaultsPath.empty())
    {
        kpt("dump-defaults: starting");
        try
        {
            // Ensure the target directory exists. `gDumpDefaultsPath`
            // was already absolutised in RootCommands.cpp.
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::u8path(gDumpDefaultsPath), ec);
            if (ec)
            {
                std::fprintf(stderr,
                    "[--dump-defaults] could not create '%s': %s\n",
                    gDumpDefaultsPath.c_str(), ec.message().c_str());
                return EXIT_FAILURE;
            }

            // (1) config.ini — populate Config singleton from the
            // canonical defaults, then write it out.
            Config::SetDefaults();
            const auto configPath = Path::Combine(gDumpDefaultsPath, u8"config.ini");
            if (!Config::SaveToPath(configPath))
            {
                std::fprintf(stderr,
                    "[--dump-defaults] failed to write %s\n", configPath.c_str());
                return EXIT_FAILURE;
            }
            kpt("dump-defaults: wrote config.ini");

            // (2) shortcuts.json — construct a ShortcutManager (the
            // ctor runs registerDefaultShortcuts populating each
            // entry's `current` and `standard`), then serialise
            // `current` to JSON via the path-taking overload.
            //
            // ShortcutManager needs an IPlatformEnvironment for its
            // _env reference, but only consults it for the user
            // shortcuts.json path on the disk-load path, which we
            // never trigger here. CreatePlatformEnvironment honours
            // the gCustomUserDataPath / gCustomRCT2DataPath env
            // overrides set by the corresponding CLI flags — fine
            // even though we don't actually use that file location.
            auto env = CreatePlatformEnvironment();
            Ui::ShortcutManager sm(*env);
            const auto shortcutsPath = Path::Combine(gDumpDefaultsPath, u8"shortcuts.json");
            sm.saveUserBindings(std::filesystem::u8path(shortcutsPath));
            kpt("dump-defaults: wrote shortcuts.json");

            // (3) rumble.json — seedDefaults + write via the helper
            // we added in HapticEvent.h.
            const auto rumblePath = Path::Combine(gDumpDefaultsPath, u8"rumble.json");
            Haptic::writeDefaultProfilesTo(rumblePath);
            kpt("dump-defaults: wrote rumble.json");

            std::fprintf(stderr,
                "[--dump-defaults] wrote config.ini, shortcuts.json, rumble.json to %s\n",
                gDumpDefaultsPath.c_str());
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "[--dump-defaults] failed: %s\n", e.what());
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }

    if (runGame == EXITCODE_CONTINUE)
    {
        std::unique_ptr<IContext> context;
        if (gOpenRCT2Headless)
        {
            kpt("CreateContext (headless)");
            // Run OpenRCT2 with a plain context
            context = CreateContext();
        }
        else
        {
            // Run OpenRCT2 with a UI context
            kpt("CreatePlatformEnvironment");
            auto env = CreatePlatformEnvironment();
            std::unique_ptr<IAudioContext> audioContext;
            try
            {
                kpt("CreateAudioContext");
                audioContext = CreateAudioContext();
            }
            catch (const SDLException& e)
            {
                LOG_WARNING("Failed to create audio context. Using dummy audio context. Error message was: %s", e.what());
                audioContext = CreateDummyAudioContext();
            }
            kpt("CreateUiContext");
            auto uiContext = CreateUiContext(*env);
            kpt("CreateContext (full)");
            context = CreateContext(std::move(env), std::move(audioContext), std::move(uiContext));
        }
        kpt("Context::RunOpenRCT2 starting");
        rc = context->RunOpenRCT2(argc, argv);
        kpt("Context::RunOpenRCT2 returned");
    }
    else if (runGame == EXITCODE_FAIL)
    {
        rc = EXIT_FAILURE;
    }
    kpt("main returning");
    return rc;
}

#ifdef __ANDROID__
extern "C" {
int SDL_main(int argc, const char* argv[])
{
    return main(argc, argv);
}
}
#endif
