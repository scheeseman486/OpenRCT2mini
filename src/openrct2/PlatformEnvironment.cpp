/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "PlatformEnvironment.h"

#include "Diagnostic.h"
#include "OpenRCT2.h"
#include "config/Config.h"
#include "core/EnumUtils.hpp"
#include "core/File.h"
#include "core/Path.hpp"
#include "core/String.hpp"
#include "platform/Platform.h"
#include "rct1/Csg.h"  // OPENRCT2MINI revision 62 — RCT1-specific data probe

using namespace OpenRCT2;

static constexpr const char* kDirectoryNamesRCT2[] = {
    "Data",        // DATA
    "Landscapes",  // LANDSCAPE
    nullptr,       // LANGUAGE
    nullptr,       // LOG_CHAT
    nullptr,       // LOG_SERVER
    nullptr,       // NETWORK_KEY
    "ObjData",     // OBJECT
    nullptr,       // PLUGIN
    "Saved Games", // SAVE
    "Scenarios",   // SCENARIO
    nullptr,       // SCREENSHOT
    nullptr,       // SEQUENCE
    nullptr,       // SHADER
    nullptr,       // THEME
    "Tracks",      // TRACK
};

static constexpr u8string_view kDirectoryNamesOpenRCT2[] = {
    u8"data",             // DATA
    u8"landscape",        // LANDSCAPE
    u8"language",         // LANGUAGE
    u8"chatlogs",         // LOG_CHAT
    u8"serverlogs",       // LOG_SERVER
    u8"keys",             // NETWORK_KEY
    u8"object",           // OBJECT
    u8"plugin",           // PLUGIN
    u8"save",             // SAVE
    u8"scenario",         // SCENARIO
    u8"screenshot",       // SCREENSHOT
    u8"sequence",         // SEQUENCE
    u8"shaders",          // SHADER
    u8"themes",           // THEME
    u8"track",            // TRACK
    u8"heightmap",        // HEIGHTMAP
    u8"replay",           // REPLAY
    u8"desyncs",          // DESYNCS
    u8"crash",            // CRASH
    u8"assetpack",        // ASSET_PACK
    u8"scenario_patches", // SCENARIO_PATCHES
};

static constexpr u8string_view kFileNames[] = {
    u8"config.ini",                              // CONFIG
    u8"hotkeys.dat",                             // CONFIG_SHORTCUTS_LEGACY
    u8"shortcuts.json",                          // CONFIG_SHORTCUTS
    u8"objects.idx",                             // CACHE_OBJECTS
    u8"tracks.idx",                              // CACHE_TRACKS
    u8"scenarios.idx",                           // CACHE_SCENARIOS
    u8"groups.json",                             // NETWORK_GROUPS
    u8"servers.cfg",                             // NETWORK_SERVERS
    u8"users.json",                              // NETWORK_USERS
    u8"highscores.dat",                          // SCORES
    u8"scores.dat",                              // SCORES (LEGACY)
    u8"Saved Games" PATH_SEPARATOR "scores.dat", // SCORES (RCT2)
    u8"changelog.txt",                           // CHANGELOG
    u8"plugin.store.json",                       // PLUGIN_STORE
    u8"contributors.md",                         // CONTRIBUTORS
};

class PlatformEnvironment final : public IPlatformEnvironment
{
private:
    u8string _basePath[kDirBaseCount];
    RCT2Variant _rct2Variant = RCT2Variant::rct2Original;

public:
    explicit PlatformEnvironment(DirBaseValues basePaths)
    {
        for (size_t i = 0; i < kDirBaseCount; i++)
        {
            _basePath[i] = basePaths[i];
        }
    }

    u8string GetDirectoryPath(DirBase base) const override
    {
        return _basePath[EnumValue(base)];
    }

    u8string GetDirectoryPath(DirBase base, DirId did) const override
    {
        auto basePath = GetDirectoryPath(base);
        u8string_view directoryName;
        switch (base)
        {
            default:
            case DirBase::rct1:
                if (basePath.empty())
                    return {};

                directoryName = kDirectoryNamesRCT2[EnumValue(did)];
                break;
            case DirBase::rct2:
                switch (_rct2Variant)
                {
                    case RCT2Variant::rct2Original:
                        directoryName = kDirectoryNamesRCT2[EnumValue(did)];
                        break;
                    case RCT2Variant::rctClassicWindows:
                        directoryName = Platform::kRCTClassicWindowsDataFolder;
                        break;
                    case RCT2Variant::rctClassicMac:
                        directoryName = Platform::kRCTClassicMacOSDataFolder;
                        break;
                    case RCT2Variant::rctClassicPlusMac:
                        directoryName = Platform::kRCTClassicPlusMacOSDataFolder;
                        break;
                }
                break;
            case DirBase::openrct2:
            case DirBase::user:
            case DirBase::config:
                directoryName = kDirectoryNamesOpenRCT2[EnumValue(did)];
                break;
        }

        return Path::Combine(basePath, directoryName);
    }

    u8string GetFilePath(PathId pathid) const override
    {
        auto dirbase = GetDefaultBaseDirectory(pathid);
        auto basePath = GetDirectoryPath(dirbase);
        auto fileName = kFileNames[EnumValue(pathid)];

        auto assetPath = Platform::GetAssetPath();
        if (!assetPath.empty())
        {
            auto combinedAssetPath = Path::Combine(assetPath, basePath, fileName);
            if (File::Exists(combinedAssetPath))
            {
                return combinedAssetPath;
            }
        }

        return Path::Combine(basePath, fileName);
    }

    u8string FindFile(DirBase base, DirId did, u8string_view fileName) const override
    {
        auto dataPath = GetDirectoryPath(base, did);

        std::string alternativeFilename;
        if (_rct2Variant != RCT2Variant::rct2Original && base == DirBase::rct2 && did == DirId::data)
        {
            // Special case, handle RCT Classic css ogg files
            if (String::startsWith(fileName, "css", true) && String::endsWith(fileName, ".dat", true))
            {
                alternativeFilename = fileName.substr(0, fileName.size() - 3);
                alternativeFilename.append("ogg");
                fileName = alternativeFilename;
            }
        }

        auto path = Path::ResolveCasing(Path::Combine(dataPath, fileName));
        if (base == DirBase::rct1 && did == DirId::data && !File::Exists(path))
        {
            // Special case, handle RCT1 steam layout where some data files are under a CD root
            auto basePath = GetDirectoryPath(base);
            auto alternativePath = Path::ResolveCasing(Path::Combine(basePath, "RCTdeluxe_install", "Data", fileName));
            if (File::Exists(alternativePath))
            {
                path = alternativePath;
            }
        }

        return path;
    }

    void SetBasePath(DirBase base, u8string_view path) override
    {
        _basePath[EnumValue(base)] = path;

        if (base == DirBase::rct2)
        {
            // This value being empty can be valid on headless.
            auto variant = Platform::classifyGamePath(path);
            if (variant.has_value())
                _rct2Variant = variant.value();
        }
    }

    bool IsUsingClassic() const override
    {
        return _rct2Variant != RCT2Variant::rct2Original;
    }

private:
    static DirBase GetDefaultBaseDirectory(PathId pathid)
    {
        switch (pathid)
        {
            case PathId::config:
            case PathId::configShortcutsLegacy:
            case PathId::configShortcuts:
                return DirBase::config;
            case PathId::cacheObjects:
            case PathId::cacheTracks:
            case PathId::cacheScenarios:
                return DirBase::cache;
            case PathId::scoresRCT2:
                return DirBase::rct2;
            case PathId::changelog:
            case PathId::contributors:
                return DirBase::documentation;
            case PathId::networkGroups:
            case PathId::networkServers:
            case PathId::networkUsers:
            case PathId::scores:
            case PathId::scoresLegacy:
            default:
                return DirBase::user;
        }
    }
};

std::unique_ptr<IPlatformEnvironment> OpenRCT2::CreatePlatformEnvironment(DirBaseValues basePaths)
{
    return std::make_unique<PlatformEnvironment>(basePaths);
}

// OPENRCT2MINI: only Android still uses a per-user subdirectory under
// the system app-data root (see CreatePlatformEnvironment). Other
// platforms point user/config/cache straight at <exeDir>/save with no
// subdirectory, so this helper compiles to no callers there.
#if defined(__ANDROID__)
static u8string GetOpenRCT2DirectoryName()
{
    return u8"openrct2-user";
}
#endif

std::unique_ptr<IPlatformEnvironment> OpenRCT2::CreatePlatformEnvironment()
{
    // OPENRCT2MINI: host build matches the device layout — user data,
    // config, and cache all live in <exeDir>/save (or whatever each
    // platform's GetFolderPath returns). The historical "OpenRCT2"
    // subdirectory append (kept on Android via GetOpenRCT2DirectoryName)
    // is dropped on host so the project tree is fully self-contained:
    // no files in ~/.config, ~/Documents, or ~/Library/Application
    // Support; everything sits beside the binary just like the Mini's
    // $APPDIR/save layout. Mini itself isn't affected — it overrides
    // user/config/cache via gCustomUserDataPath below.
#if defined(__ANDROID__)
    auto subDirectory = GetOpenRCT2DirectoryName();
#else
    const std::string subDirectory{};
#endif

    // Set default paths
    std::string basePaths[kDirBaseCount];
    basePaths[EnumValue(DirBase::openrct2)] = Platform::GetInstallPath();
    basePaths[EnumValue(DirBase::user)] = Path::Combine(Platform::GetFolderPath(SpecialFolder::userData), subDirectory);
    basePaths[EnumValue(DirBase::config)] = Path::Combine(Platform::GetFolderPath(SpecialFolder::userConfig), subDirectory);
    basePaths[EnumValue(DirBase::cache)] = Path::Combine(Platform::GetFolderPath(SpecialFolder::userCache), subDirectory);
    basePaths[EnumValue(DirBase::documentation)] = Platform::GetDocsPath();

    // Override paths that have been specified via the command line
    if (!gCustomRCT1DataPath.empty())
    {
        basePaths[EnumValue(DirBase::rct1)] = gCustomRCT1DataPath;
    }
    if (!gCustomRCT2DataPath.empty())
    {
        basePaths[EnumValue(DirBase::rct2)] = gCustomRCT2DataPath;
    }
    if (!gCustomOpenRCT2DataPath.empty())
    {
        basePaths[EnumValue(DirBase::openrct2)] = gCustomOpenRCT2DataPath;
    }
    if (!gCustomUserDataPath.empty())
    {
        basePaths[EnumValue(DirBase::user)] = gCustomUserDataPath;
        basePaths[EnumValue(DirBase::config)] = gCustomUserDataPath;
        basePaths[EnumValue(DirBase::cache)] = gCustomUserDataPath;
    }

    if (basePaths[EnumValue(DirBase::documentation)].empty())
    {
        basePaths[EnumValue(DirBase::documentation)] = basePaths[EnumValue(DirBase::openrct2)];
    }

    auto env = CreatePlatformEnvironment(basePaths);

    // Now load the config so we can get the RCT1 and RCT2 paths
    auto configPath = env->GetFilePath(PathId::config);
    Config::SetDefaults();
    // OPENRCT2MINI: capture first-run state so we can apply auto-detection
    // policies (e.g. enable RCT1 if found) only on the very first run, and
    // respect the user's later choices (e.g. clearing the rct1 path to
    // disable RCT1) on subsequent runs.
    const bool firstRun = !Config::OpenFromPath(configPath);
    if (firstRun)
    {
        Config::SaveToPath(configPath);
    }
    // OPENRCT2MINI cut 46 / 53c / revision 62: auto-resolve install paths
    // in this order:
    //   1. configured path (if it actually contains a game install)
    //   2. <exe_dir>/<subdir>          (binary dropped alongside install)
    //   3. <rct2_dir>/../<subdir>      (RCT1-only: sibling of RCT2 install,
    //                                   the typical disc-install layout)
    // Each step does case-insensitive matching for "rct1" / "RCT1" /
    // "Rct1" because Windows installs ship in mixed case and our targets
    // (Linux + ext4 SD card) preserve that case.
    //
    // Revision 62: RCT1 vs RCT2 use different on-disk layouts —
    // Platform::OriginalGameDataExists only knows the RCT2 layout
    // (Data/g1.dat); RCT1 needs Csg1datPresentAtLocation (Data/CSG1.DAT).
    // The earlier code used OriginalGameDataExists for both, so a valid
    // RCT1 install never matched and the first-run auto-enable below
    // silently no-op'd — user had to set the path manually in Options.
    // OPENRCT2MINI / appimage-plan: when launched from an AppImage,
    // GetCurrentExecutablePath() returns the inside-the-mount path
    // (/tmp/.mount_xxx/usr/bin/openrct2), which is read-only squashfs and
    // disappears at exit. The user's actual filesystem location is the
    // directory holding the .AppImage file, exposed by the AppImageKit
    // runtime via $APPIMAGE. Mirror the same lookup as
    // Platform::GetFolderPath() for saves: prefer $APPIMAGE-dir when set,
    // otherwise the binary's directory. Mini and bare-binary host builds
    // (no $APPIMAGE) keep the existing exe-dir behaviour.
    auto exeDir = [&]() -> std::string {
        if (const char* appImagePath = getenv("APPIMAGE");
            appImagePath != nullptr && appImagePath[0] != '\0')
        {
            return Path::GetDirectory(appImagePath);
        }
        return Path::GetDirectory(Platform::GetCurrentExecutablePath());
    }();
    auto isValidInstall = [](const std::string& p, bool isRct1) {
        if (p.empty())
            return false;
        return isRct1 ? Csg1datPresentAtLocation(p) : Platform::OriginalGameDataExists(p);
    };
    auto resolveDataPath = [&exeDir, &isValidInstall](
                               const std::string& configured, std::initializer_list<const char*> names, bool isRct1,
                               const std::string& siblingOf = {}) {
        if (isValidInstall(configured, isRct1))
            return configured;
        for (auto* name : names)
        {
            if (!exeDir.empty())
            {
                auto candidate = Path::Combine(exeDir, name);
                if (isValidInstall(candidate, isRct1))
                    return candidate;
            }
            if (!siblingOf.empty())
            {
                auto parent = Path::GetDirectory(siblingOf);
                if (!parent.empty())
                {
                    auto candidate = Path::Combine(parent, name);
                    if (isValidInstall(candidate, isRct1))
                        return candidate;
                }
            }
        }
        return configured;
    };
    // Resolve rct2 first so we can use it as the sibling reference for rct1.
    if (gCustomRCT2DataPath.empty())
    {
        env->SetBasePath(
            DirBase::rct2,
            resolveDataPath(Config::Get().general.rct2Path, { u8"rct2", u8"RCT2", u8"Rct2" }, /*isRct1=*/false));
    }
    if (gCustomRCT1DataPath.empty())
    {
        const auto& rct2Resolved = env->GetDirectoryPath(DirBase::rct2);
        env->SetBasePath(
            DirBase::rct1,
            resolveDataPath(
                Config::Get().general.rct1Path, { u8"rct1", u8"RCT1", u8"Rct1" }, /*isRct1=*/true, rct2Resolved));
    }

    // OPENRCT2MINI: RCT1 auto-enable. The path-resolution above sets
    // DirBase::rct1 on the runtime env, but the rest of the engine
    // (Drawing.Sprite.cpp's CSG load, the title-sequence picker, etc.)
    // gates RCT1 features on Config::Get().general.rct1Path being non-empty.
    // Mirror the resolved env path back into the config so the user
    // doesn't have to dig into Options > Advanced and click "Browse"
    // before RCT1-themed objects load.
    //
    // Revision 62: dropped the firstRun gate. The earlier code only
    // populated on the *very first boot*, which (a) silently no-op'd if
    // the original probe was broken and (b) left users stuck if they'd
    // ever booted with that bug. Policy is now: if the resolved
    // DirBase::rct1 is a real RCT1 install AND the user hasn't set a
    // path yet, populate it. Users who want to disable RCT1 can either
    // remove the rct1/ folder (no detection → no auto-populate) or set
    // a deliberately bogus rct1Path in Options (we only override when
    // the configured value is empty, so any non-empty value sticks).
    //
    // Also revision 62: use the RCT1-specific data probe
    // (Csg1datPresentAtLocation) instead of OriginalGameDataExists.
    // OriginalGameDataExists only knows the RCT2 layout (Data/g1.dat)
    // and silently fails for valid RCT1 installs (Data/CSG1.DAT).
    if (Config::Get().general.rct1Path.empty())
    {
        const auto& rct1Resolved = env->GetDirectoryPath(DirBase::rct1);
        if (!rct1Resolved.empty() && Csg1datPresentAtLocation(rct1Resolved))
        {
            Config::Get().general.rct1Path = rct1Resolved;
            Config::SaveToPath(configPath);
            LOG_VERBOSE("[OPENRCT2MINI] RCT1 auto-enable: %s", rct1Resolved.c_str());
        }
    }

    // Log base paths
    LOG_VERBOSE("DirBase::rct1    : %s", env->GetDirectoryPath(DirBase::rct1).c_str());
    LOG_VERBOSE("DirBase::rct2    : %s", env->GetDirectoryPath(DirBase::rct2).c_str());
    LOG_VERBOSE("DirBase::openrct2: %s", env->GetDirectoryPath(DirBase::openrct2).c_str());
    LOG_VERBOSE("DirBase::user    : %s", env->GetDirectoryPath(DirBase::user).c_str());
    LOG_VERBOSE("DirBase::config  : %s", env->GetDirectoryPath(DirBase::config).c_str());
    LOG_VERBOSE("DirBase::cache   : %s", env->GetDirectoryPath(DirBase::cache).c_str());

    return env;
}
