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
#include <openrct2/MiniDebug.h>  // OPENRCT2MINI revision 64 — gated debug logging
#include <openrct2/OpenRCT2.h>
#include <openrct2/PlatformEnvironment.h>
#include <openrct2/audio/AudioContext.h>
#include <openrct2/command_line/CommandLine.hpp>
#include <openrct2/platform/Platform.h>
#include <openrct2/ui/UiContext.h>

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
        std::fread(buf, 1, sizeof(buf) - 1, f);
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
                static const char prefix[] = "\n[OPENRCT2MINI] *** signal ";
                ::write(STDERR_FILENO, prefix, sizeof(prefix) - 1);
                char num[8] = {0};
                int n = sig;
                int len = 0;
                if (n == 0) num[len++] = '0';
                else {
                    char tmp[8]; int t = 0;
                    while (n > 0 && t < 7) { tmp[t++] = '0' + (n % 10); n /= 10; }
                    while (t > 0) num[len++] = tmp[--t];
                }
                ::write(STDERR_FILENO, num, len);
                ::write(STDERR_FILENO, " — backtrace:\n", 14);
                void* frames[40];
                int got = ::backtrace(frames, 40);
                ::backtrace_symbols_fd(frames, got, STDERR_FILENO);
                ::write(STDERR_FILENO, "[OPENRCT2MINI] *** end backtrace\n", 32);
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
#if defined(__GLIBC__)
    mallopt(M_ARENA_MAX, 2);
    kpt("mallopt arena_max=2 ok");
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
