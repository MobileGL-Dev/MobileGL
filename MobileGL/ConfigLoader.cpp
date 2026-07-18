// MobileGL - MobileGL/ConfigLoader.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "Config.h"

#include <cerrno>
#include <cstdlib>

#ifndef _WIN32
extern char** environ;
#endif

namespace MobileGL::MG_Config {
    // Zero/default-initialized at static-init time (all fields have constexpr-friendly
    // defaults), so it is safe to read even if MG_ConfigLoader::Init has not run yet.
    FeaturesTable Features;
} // namespace MobileGL::MG_Config

namespace MobileGL::MG_ConfigLoader {
    static UniquePtr<UnorderedMap<String, String>> acceptedEnvVariablesMap;

    static Bool IsAcceptedPrefix(const String& key) {
        return (key.compare(0, 6, "LIBGL_") == 0 || key.compare(0, 9, "MOBILEGL_") == 0);
    }

    inline void InitializeAcceptedEnvVariables() {
        if (!acceptedEnvVariablesMap) {
            acceptedEnvVariablesMap = MakeUnique<UnorderedMap<String, String>>();
        } else {
            acceptedEnvVariablesMap->clear();
        }

        char** envPtr = nullptr;

#ifdef _WIN32
        envPtr = _environ;
#else // POSIX
        envPtr = ::environ;
#endif

        if (envPtr == nullptr) return;

        for (char** env = envPtr; *env != nullptr; ++env) {
            String entry(*env);
            SizeT pos = entry.find('=');
            if (pos != String::npos) {
                String key = entry.substr(0, pos);
                String value = entry.substr(pos + 1);

                if (IsAcceptedPrefix(key)) {
                    (*acceptedEnvVariablesMap)[key] = value;
                    MGLOG_D("Config: Accepted env variable: %s=%s", key.c_str(), value.c_str());
                }
            }
        }
    }

    inline void QueryEnvVariable(const String& key, String& outValue, const String& defaultValue) {
        auto it = acceptedEnvVariablesMap->find(key);
        if (it != acceptedEnvVariablesMap->end()) {
            outValue = it->second;
        } else {
            outValue = defaultValue;
        }
    }

    // Unified truthy rule for boolean feature env variables: set, non-empty, not "0",
    // and not "false" (case-insensitive).
    static Bool IsTruthyValue(const String& value) {
        if (value.empty() || value == "0") {
            return false;
        }
        String lowered = value;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lowered != "false";
    }

    inline Bool QueryEnvFlag(const String& key) {
        auto it = acceptedEnvVariablesMap->find(key);
        return it != acceptedEnvVariablesMap->end() && IsTruthyValue(it->second);
    }

    inline Uint32 QueryEnvUint32(const String& key, Uint32 defaultValue, Uint32 minValue, Uint32 maxValue) {
        auto it = acceptedEnvVariablesMap->find(key);
        if (it == acceptedEnvVariablesMap->end()) {
            return defaultValue;
        }

        const String& value = it->second;
        char* parseEnd = nullptr;
        errno = 0;
        const unsigned long parsedValue = std::strtoul(value.c_str(), &parseEnd, 10);
        if (parseEnd == value.c_str() || *parseEnd != '\0' || errno == ERANGE || parsedValue < minValue ||
            parsedValue > maxValue) {
            MGLOG_W("Config: Ignoring invalid env variable %s='%s'; expected an integer in range [%u, %u], "
                    "using default %u",
                    key.c_str(), value.c_str(), minValue, maxValue, defaultValue);
            return defaultValue;
        }

        return static_cast<Uint32>(parsedValue);
    }

    inline void InitFeatures() {
        auto& features = MG_Config::Features;
        features.DisableTimerQuery = QueryEnvFlag("MOBILEGL_DISABLE_TIMERQUERY");
        features.UseAngle = QueryEnvFlag("MOBILEGL_USE_ANGLE");
#if defined(MOBILEGL_TRACE_ANGLE_VARIANTS)
        QueryEnvVariable("MOBILEGL_TRACE_ANGLE_VARIANT", features.TraceAngleVariant, "");
#endif
        features.DisableSubgroup = QueryEnvFlag("MOBILEGL_DISABLE_SUBGROUP");
        features.MagmaR11G11B10FFallback = QueryEnvFlag("MOBILEGL_MAGMA_R11G11B10F_FALLBACK");
        features.MagmaFramesInFlight = QueryEnvUint32("MOBILEGL_MAGMA_FRAMESINFLIGHT", 3, 1, 64);
        features.AvoidSamplerMipmapMinFilter =
            QueryEnvFlag("MOBILEGL_AVOID_SAMPLER_MIPMAP_MIN_FILTER");
        features.CoherentAsFlush = QueryEnvFlag("MOBILEGL_COHERENT_AS_FLUSH");
        features.TraceSkipAutodestroy = QueryEnvFlag("MOBILEGL_TRACE_SKIP_AUTODESTROY");
        features.DisableUboRing = QueryEnvFlag("MOBILEGL_DISABLE_UBO_RING");
        features.RelaxedSemantics = QueryEnvFlag("MOBILEGL_RELAXED_SEMANTICS");
    }

    inline void InitBackendType() {
        String backendTypeStr;
        QueryEnvVariable("MOBILEGL_BACKEND_TYPE", backendTypeStr, "DirectGLES");
#define ENTRY(backendType)                                                                                             \
    if (backendTypeStr == #backendType) {                                                                              \
        MG_Config::ActiveBackendType = BackendType::backendType;                                                       \
        MGLOG_I("Config: Active backend type set to " #backendType);                                                   \
        return;                                                                                                        \
    }
        ENTRY(DirectGLES)
        ENTRY(DirectVulkan)
        ENTRY(Unknown)
        MG_Config::ActiveBackendType = BackendType::Unknown;
#undef ENTRY
    }

    void Init() {
        MGLOG_D("Loading configuration from environment variables...");
        InitializeAcceptedEnvVariables();

        InitBackendType();
        InitFeatures();

        // Destroy the map since we won't need it anymore
        acceptedEnvVariablesMap.reset();
    }
} // namespace MobileGL::MG_ConfigLoader
