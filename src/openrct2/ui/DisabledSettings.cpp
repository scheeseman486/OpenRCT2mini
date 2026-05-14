/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "DisabledSettings.h"

#include "../core/Console.hpp"
#include "../core/Json.hpp"
// Per-build manifest embedded by cmake/EmbedFileAsArray.cmake; see
// src/openrct2/CMakeLists.txt for the embed wiring and
// config/<build>/disabled_settings.json for the source file.
#include "DisabledSettingsDefault.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace OpenRCT2;

namespace OpenRCT2::Ui::DisabledSettings
{
    namespace
    {
        // Lazy-loaded singletons. Linear-scan lookups against these
        // are cheap because the manifests are tiny — a couple dozen
        // entries across both. Loaded once on first query and held
        // for the process lifetime.
        std::vector<std::string> _disabled;
        std::vector<std::string> _removed;
        bool _loaded = false;

        void ensureLoaded()
        {
            if (_loaded)
                return;
            // Set the flag before parsing so a recursive failure path
            // (theoretical — Console::Error doesn't re-enter us, but
            // be defensive) doesn't infinite-loop. On parse failure
            // we leave both vectors empty, which means every query
            // returns false; the engine falls back to "nothing gated"
            // rather than crashing.
            _loaded = true;
            try
            {
                const auto* data = reinterpret_cast<const char*>(kDisabledSettingsDefault);
                auto root = json_t::parse(data, data + kDisabledSettingsDefaultSize);
                if (!root.is_object())
                    return;

                if (auto it = root.find("disabled"); it != root.end() && it->is_array())
                {
                    for (const auto& entry : *it)
                    {
                        if (entry.is_string())
                            _disabled.push_back(entry.get<std::string>());
                    }
                }
                if (auto it = root.find("removed"); it != root.end() && it->is_array())
                {
                    for (const auto& entry : *it)
                    {
                        if (entry.is_string())
                            _removed.push_back(entry.get<std::string>());
                    }
                }
            }
            catch (...)
            {
                // Embedded blob is part of the binary — a parse
                // failure means the build pipeline produced something
                // broken. Leave the vectors empty and log so the
                // problem surfaces if anyone scans stderr; the engine
                // continues with no gates applied.
                Console::Error::WriteLine(
                    "DisabledSettings: embedded disabled_settings.json failed to parse");
            }
        }

        bool contains(const std::vector<std::string>& haystack, std::string_view needle)
        {
            return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
        }
    } // namespace

    bool isDisabled(std::string_view key)
    {
        ensureLoaded();
        return contains(_disabled, key);
    }

    bool isRemoved(std::string_view key)
    {
        ensureLoaded();
        return contains(_removed, key);
    }

    bool isLanguageRemoved(std::string_view localeTag)
    {
        // Compose "Language.<tag>" without an allocation per query —
        // the SSO threshold covers our locale tag lengths.
        std::string composed;
        composed.reserve(9 + localeTag.size());
        composed.append("Language.");
        composed.append(localeTag);
        return isRemoved(composed);
    }
} // namespace OpenRCT2::Ui::DisabledSettings
