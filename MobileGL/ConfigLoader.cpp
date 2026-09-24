// MobileGL - MobileGL/ConfigLoader.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "Config.h"
#if MOBILEGL_PIPE_PUSH
// For kMGPipeSubsystemsMigratedAtP5e, the push build's PipePush default (the P2, P3a and P4a
// constants beside it are the phase-by-phase controls, not the default). Push-only, so the
// pull build's translation unit is unchanged.
#include <MG_Pipe/MGPipe.h>
#endif

#include <cerrno>
#include <cstdlib>
#if MOBILEGL_BUILD_DISAGGREGATED
// std::strcmp, for InitIpc's server-role check (P12). Split-only, so the pull build's TU is
// unchanged (G1).
#include <cstring>
#endif

#ifndef _WIN32
extern char** environ;
#endif

namespace MobileGL::MG_Config {
    // Zero/default-initialized at static-init time (all fields have constexpr-friendly
    // defaults), so it is safe to read even if MG_ConfigLoader::Init has not run yet.
    FeaturesTable Features;
#if MOBILEGL_BUILD_DISAGGREGATED
    // Same contract, and for the same reason: MG_Backend::Init() reads Transport, and a
    // build order that put it before MG_ConfigLoader::Init() must see Monolith rather than
    // a torn enum. Defined only here - in a pull build Config.h makes Transport a constexpr
    // and there is nothing to define.
    TransportMode Transport = TransportMode::Monolith;
    String TransportEndpoint;
    Bool SplitTransportRequestedByConfig = false;
    IpcTable Ipc;
#endif
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

    // Quirk overrides are tri-state: an unset variable keeps device auto-detection, a truthy
    // value forces the quirk on, anything else set ("0", "false", "") forces it off.
    inline MG_Config::QuirkOverride QueryEnvQuirkOverride(const String& key) {
        auto it = acceptedEnvVariablesMap->find(key);
        if (it == acceptedEnvVariablesMap->end()) {
            return MG_Config::QuirkOverride::Auto;
        }
        return IsTruthyValue(it->second) ? MG_Config::QuirkOverride::ForceOn
                                         : MG_Config::QuirkOverride::ForceOff;
    }

    // Multi-draw mode is a named-value preference: unset keeps Auto (best supported tier),
    // a recognized name selects that tier as the ceiling, anything else warns and keeps Auto.
    inline MG_Config::MultiDrawMode QueryEnvMultiDrawMode(const String& key) {
        auto it = acceptedEnvVariablesMap->find(key);
        if (it == acceptedEnvVariablesMap->end()) {
            return MG_Config::MultiDrawMode::Auto;
        }
        String lowered = it->second;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowered == "ext") return MG_Config::MultiDrawMode::Ext;
        if (lowered == "indirect") return MG_Config::MultiDrawMode::Indirect;
        if (lowered == "unroll") return MG_Config::MultiDrawMode::Unroll;
        if (lowered.empty() || lowered == "auto") return MG_Config::MultiDrawMode::Auto;
        MGLOG_W("Config: Ignoring invalid env variable %s='%s'; expected ext|indirect|unroll|auto, using auto",
                key.c_str(), it->second.c_str());
        return MG_Config::MultiDrawMode::Auto;
    }

    // Same contract as QueryEnvMultiDrawMode, over the DirectGLES tier names.
    inline MG_Config::GLESMultiDrawMode QueryEnvGLESMultiDrawMode(const String& key) {
        auto it = acceptedEnvVariablesMap->find(key);
        if (it == acceptedEnvVariablesMap->end()) {
            return MG_Config::GLESMultiDrawMode::Auto;
        }
        String lowered = it->second;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowered == "ext") return MG_Config::GLESMultiDrawMode::Ext;
        if (lowered == "multiindirect") return MG_Config::GLESMultiDrawMode::MultiIndirect;
        if (lowered == "indirect") return MG_Config::GLESMultiDrawMode::Indirect;
        if (lowered == "basevertex") return MG_Config::GLESMultiDrawMode::BaseVertex;
        if (lowered == "drawelements") return MG_Config::GLESMultiDrawMode::DrawElements;
        if (lowered == "compute") return MG_Config::GLESMultiDrawMode::Compute;
        if (lowered.empty() || lowered == "auto") return MG_Config::GLESMultiDrawMode::Auto;
        MGLOG_W("Config: Ignoring invalid env variable %s='%s'; expected "
                "ext|multiindirect|indirect|basevertex|drawelements|compute|auto, using auto",
                key.c_str(), it->second.c_str());
        return MG_Config::GLESMultiDrawMode::Auto;
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

    // Same contract as QueryEnvUint32, over 64 bits and accepting an explicit 0x prefix: the
    // one consumer is a subsystem BITMASK, and a bitmask written in decimal is unreadable.
    // Decimal otherwise - never strtoull's base 0, whose "leading zero means octal" rule
    // silently read MOBILEGL_PIPE_PUSH=010 as 8 - and a '-' anywhere is rejected rather than
    // wrapped, which strtoull would otherwise do without complaint (-1 -> every bit set).
    inline Uint64 QueryEnvUint64(const String& key, Uint64 defaultValue) {
        auto it = acceptedEnvVariablesMap->find(key);
        if (it == acceptedEnvVariablesMap->end()) {
            return defaultValue;
        }

        const String& value = it->second;
        const char* text = value.c_str();
        int base = 10;
        if (value.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
            text += 2;
            base = 16;
        }
        char* parseEnd = nullptr;
        errno = 0;
        const bool negative = value.find('-') != String::npos;
        const unsigned long long parsedValue = negative ? 0 : std::strtoull(text, &parseEnd, base);
        if (negative || parseEnd == text || *parseEnd != '\0' || errno == ERANGE) {
            MGLOG_W("Config: Ignoring invalid env variable %s='%s'; expected a non-negative integer "
                    "(decimal, or 0x-prefixed hexadecimal), using default %llu",
                    key.c_str(), value.c_str(), static_cast<unsigned long long>(defaultValue));
            return defaultValue;
        }

        return static_cast<Uint64>(parsedValue);
    }

    inline void InitFeatures() {
        auto& features = MG_Config::Features;
        features.DisableTimerQuery = QueryEnvFlag("MOBILEGL_DISABLE_TIMERQUERY");
        features.EsprytEnableTextureView = QueryEnvFlag("MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW");
        features.EnableSpirvValidation = QueryEnvFlag("MOBILEGL_ENABLE_SPIRV_VALIDATION");
        features.EsprytUseAngle = QueryEnvFlag("MOBILEGL_ESPRYT_USE_ANGLE");
#if defined(MOBILEGL_TRACE_ANGLE_VARIANTS)
        QueryEnvVariable("MOBILEGL_TRACE_ANGLE_VARIANT", features.TraceAngleVariant, "");
#endif
        features.MagmaDisableSubgroup = QueryEnvFlag("MOBILEGL_MAGMA_DISABLE_SUBGROUP");
        features.MagmaEmulateSubgroup = QueryEnvFlag("MOBILEGL_MAGMA_EMULATE_SUBGROUP");
        features.MagmaFixIterationRPSubgroupScratch =
            QueryEnvQuirkOverride("MOBILEGL_MAGMA_FIX_ITERATIONRP_SUBGROUP_SCRATCH");
        features.MagmaIterationRPFixBarrier = QueryEnvFlag("MOBILEGL_MAGMA_ITERATIONRP_FIX_BARRIER");
        features.MagmaDeriveNumSubgroups = QueryEnvQuirkOverride("MOBILEGL_MAGMA_DERIVE_NUM_SUBGROUPS");
        features.AdvertiseFp64 = QueryEnvFlag("MOBILEGL_ADVERTISE_FP64");
        features.MagmaR11G11B10FFallback = QueryEnvFlag("MOBILEGL_MAGMA_R11G11B10F_FALLBACK");
        features.MagmaFramesInFlight = QueryEnvUint32("MOBILEGL_MAGMA_FRAMESINFLIGHT", 3, 1, 64);
        features.EsprytAvoidSamplerMipmapMinFilter =
            QueryEnvFlag("MOBILEGL_ESPRYT_AVOID_SAMPLER_MIPMAP_MIN_FILTER");
        features.EsprytAvoidExplicitLodBias = QueryEnvFlag("MOBILEGL_ESPRYT_AVOID_EXPLICIT_LOD_BIAS");
        features.EsprytUnlocatedIoBlocks = QueryEnvQuirkOverride("MOBILEGL_ESPRYT_UNLOCATED_IO_BLOCKS");
        features.PointSizeDemotion = QueryEnvQuirkOverride("MOBILEGL_POINT_SIZE_DEMOTION");
        features.CoherentAsFlush = QueryEnvFlag("MOBILEGL_COHERENT_AS_FLUSH");
        features.TraceSkipAutodestroy = QueryEnvFlag("MOBILEGL_TRACE_SKIP_AUTODESTROY");
        features.EsprytDisableUboRing = QueryEnvFlag("MOBILEGL_ESPRYT_DISABLE_UBO_RING");
        features.EsprytDisableUnpackRing = QueryEnvFlag("MOBILEGL_ESPRYT_DISABLE_UNPACK_RING");
        features.EsprytDisableUploadRing = QueryEnvFlag("MOBILEGL_ESPRYT_DISABLE_UPLOAD_RING");
        features.EsprytDisableInvalidateFlush = QueryEnvFlag("MOBILEGL_ESPRYT_DISABLE_INVALIDATE_FLUSH");
        features.DisableLargeBufferAdoption = QueryEnvFlag("MOBILEGL_DISABLE_LARGE_BUFFER_ADOPTION");
        features.EsprytForceDepthStencilReadbackEmulation =
            QueryEnvFlag("MOBILEGL_ESPRYT_FORCE_DS_READBACK_EMULATION");
        features.RelaxedSemantics = QueryEnvFlag("MOBILEGL_RELAXED_SEMANTICS");
        features.MagmaDisableBlendedDepthWriteQuirk =
            QueryEnvQuirkOverride("MOBILEGL_MAGMA_DISABLE_BLENDED_DEPTH_WRITE");
        features.MagmaDisableRobustBufferAccess = QueryEnvFlag("MOBILEGL_MAGMA_DISABLE_ROBUST_BUFFER_ACCESS");
        features.MagmaMultiDrawMode = QueryEnvMultiDrawMode("MOBILEGL_MAGMA_MULTIDRAW_MODE");
        features.EsprytMultiDrawMode = QueryEnvGLESMultiDrawMode("MOBILEGL_ESPRYT_MULTIDRAW_MODE");
        features.AsyncShaderCompile = QueryEnvQuirkOverride("MOBILEGL_ASYNC_SHADER_COMPILE");
        features.AsyncShaderCompileThreads = QueryEnvUint32("MOBILEGL_ASYNC_SHADER_COMPILE_THREADS", 0, 0, 64);
        features.AsyncOptimisticShaderStatus =
            QueryEnvQuirkOverride("MOBILEGL_ASYNC_OPTIMISTIC_SHADER_STATUS");
        features.ShaderTranslationCache = QueryEnvQuirkOverride("MOBILEGL_SHADER_CACHE");
        features.EsprytViewportArrayEmulation =
            QueryEnvQuirkOverride("MOBILEGL_ESPRYT_FORCE_VIEWPORT_ARRAY_EMULATION");
        features.EsprytWidenPacked16Storage =
            QueryEnvQuirkOverride("MOBILEGL_ESPRYT_WIDEN_PACKED16_STORAGE");
        features.MagmaPrimGenQueryReroute = QueryEnvQuirkOverride("MOBILEGL_MAGMA_PRIMGEN_QUERY_REROUTE");
        // MGPipe. Nothing here needs adding to an allow-list: InitializeAcceptedEnvVariables
        // accepts every MOBILEGL_ / LIBGL_ prefixed variable in the environment, so a name
        // that starts with MOBILEGL_ is visible to these queries by construction.
#if MOBILEGL_PIPE_PUSH
        // A push build with the knob unset runs every subsystem migrated so far, so the
        // shipped path is the one the gates measure; MOBILEGL_PIPE_PUSH=0 in the
        // environment is the all-subsystems-pull control that reproduces P1 exactly, and
        // kMGPipeSubsystemsMigratedAtP3a (0x1ff) is the phase-by-phase control - P4a's four
        // subsystems off, everything P3a landed still on.
        features.PipePush = QueryEnvUint64("MOBILEGL_PIPE_PUSH", MG_Pipe::kMGPipeSubsystemsMigratedAtP5e);
#else
        // Meaningless in a pull build: there is nothing to push. Config.h documents 0 as
        // "pull everything" and that stays literally true.
        features.PipePush = QueryEnvUint64("MOBILEGL_PIPE_PUSH", 0);
#endif
        features.PipeVerify = QueryEnvFlag("MOBILEGL_PIPE_VERIFY");
#if MOBILEGL_PIPE_PUSH
        // Defaults ON: read as a tri-state so only an explicitly falsy value turns it off.
        features.PipeVerifyFatal =
            QueryEnvQuirkOverride("MOBILEGL_PIPE_VERIFY_FATAL") != MG_Config::QuirkOverride::ForceOff;
        QueryEnvVariable("MOBILEGL_PIPE_VERIFY_CORRUPT", features.PipeVerifyCorrupt, "");
        QueryEnvVariable("MOBILEGL_PIPE_POISON_OMIT", features.PipePoisonOmit, "");
        features.PipeHandleAbaControl = QueryEnvFlag("MOBILEGL_PIPE_HANDLE_ABA_CONTROL");
#endif
        features.PipeStats = QueryEnvFlag("MOBILEGL_PIPE_STATS");
        // Defaults ON, so the flag has to be read as a tri-state rather than as a plain
        // truthy check: unset must keep the memos, and only an explicitly falsy value may
        // drop them.
        features.PipeLegacyMemos =
            QueryEnvQuirkOverride("MOBILEGL_PIPE_LEGACY_MEMOS") != MG_Config::QuirkOverride::ForceOff;
        features.PipeTexelRetainMb = QueryEnvUint32("MOBILEGL_PIPE_TEXEL_RETAIN_MB", 0, 0, 4096);
        features.PipeIndexMirrorMb = QueryEnvUint32("MOBILEGL_PIPE_INDEX_MIRROR_MB", 64, 0, 4096);
        features.PipeStatsPeriod = QueryEnvUint32("MOBILEGL_PIPE_STATS_PERIOD", 120, 1, 1000000);
        QueryEnvVariable("MOBILEGL_PIPE_STATS_FILE", features.PipeStatsFile, "");
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

#if MOBILEGL_BUILD_DISAGGREGATED
    // MOBILEGL_TRANSPORT = monolith | inproc | spawn | unix:<path> | pipe:<name>
    // (ARCHITECTURE.md:583). Shaped after InitBackendType above: an exact-name table, then
    // one fallback that names what it did instead. The two prefixed forms are the only
    // reason this is not literally that function's ENTRY macro.
    //
    // spawn / unix: / pipe: PARSE AND THEN REFUSE. They are P6's, and the refusal is NAMED
    // rather than silent, because the failure this avoids is a P6 lane that set
    // MOBILEGL_TRANSPORT=spawn, fell back to monolith, and went green on the wrong arm.
    // The mode is left at Monolith so nothing half-initializes.
    inline void InitTransport() {
        String value;
        QueryEnvVariable("MOBILEGL_TRANSPORT", value, "monolith");
        String lowered = value;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        MG_Config::TransportEndpoint.clear();
        if (lowered.empty() || lowered == "monolith") {
            MG_Config::Transport = MG_Config::TransportMode::Monolith;
            return;
        }
        if (lowered == "inproc") {
            MG_Config::Transport = MG_Config::TransportMode::InProcess;
            // P7 F1: see Config.h. Set beside the mode, on the two arms that actually resolve
            // one, and NOT on the two that name a transport and then stay monolith - a process
            // that was refused its transport brings no client half up either.
            MG_Config::SplitTransportRequestedByConfig = true;
            MGLOG_I("Config: MOBILEGL_TRANSPORT=inproc - the MGPipe record stream crosses a real "
                    "ring to an apply thread");
            return;
        }
        // The three P6 forms. Recognised precisely, so the diagnostic can say "not yet"
        // rather than "unknown", which are different bugs on the operator's side.
        // P6 `sm`/`cp`: spawn is IMPLEMENTED. It launches a server process on a
        // rendezvous of its own and connects to it - the two processes are
        // independent, so this is `Dial = Connect` with a launcher attached
        // rather than the fork-coupled shape ARCHITECTURE.md:488 described.
        if (lowered == "spawn") {
            MG_Config::Transport = MG_Config::TransportMode::Spawn;
            MG_Config::SplitTransportRequestedByConfig = true; // P7 F1, as above.
            // THE SAME SHAPE AS THE inproc LINE ABOVE, AND FOR THE SAME REASON.
            // run_trace_case.cmake asserts on a distinctive sentence from this
            // function rather than on `MOBILEGL_TRANSPORT=spawn` alone, because
            // ConfigLoader logs an `Accepted env variable: KEY=VALUE` line for
            // every MOBILEGL_* in EVERY build including the pull one - so the
            // bare KEY=VALUE is satisfied by a monolith library that parsed
            // nothing (review M-5). A spawn arm with no line of its own could
            // only have been asserted with the inproc marker, which spawn never
            // emits: the arm would have been red for a missing sentence rather
            // than green for a working transport.
            MGLOG_I("Config: MOBILEGL_TRANSPORT=spawn - the MGPipe record stream crosses a real "
                    "ring to an apply thread in ANOTHER PROCESS");
            return;
        }
        if (lowered.compare(0, 5, "unix:") == 0 || lowered.compare(0, 5, "pipe:") == 0) {
            // unix: and pipe: remain P6+ - connecting to an endpoint SOMEBODY
            // ELSE is listening on needs the server lifecycle to be somebody
            // else's too, which is P12's. Named, never a silent degrade.
            MGLOG_E("Config: MOBILEGL_TRANSPORT='%s' names a transport P6 does not implement yet; "
                    "staying on monolith. This run is NOT a split run.",
                    value.c_str());
            MG_Config::Transport = MG_Config::TransportMode::Monolith;
            return;
        }
        MGLOG_W("Config: Ignoring invalid env variable MOBILEGL_TRANSPORT='%s'; expected "
                "monolith|inproc|spawn|unix:<path>|pipe:<name>, using monolith",
                value.c_str());
        MG_Config::Transport = MG_Config::TransportMode::Monolith;
    }

    // The MOBILEGL_IPC_* family (Config.h IpcTable). Parsed unconditionally rather than only
    // when Transport != Monolith: a knob that silently means nothing on one arm of an A/B is
    // how an A/B stops being one, and the ranges below are the diagnostics.
    inline void InitIpc() {
        auto& ipc = MG_Config::Ipc;
        QueryEnvVariable("MOBILEGL_IPC_SERVER_PATH", ipc.ServerPath, "");
        QueryEnvVariable("MOBILEGL_IPC_CONTROL", ipc.Control, "fork");
        QueryEnvVariable("MOBILEGL_IPC_DATA", ipc.Data, "auto");
        // Both ring floors are 1 MiB, not 0: a ring caps ONE record at half its size, and
        // the catalogue's largest fixed payload (MGPFramebufferState, 304 bytes) plus a
        // create_shader_state archive already needs far more than a toy ring. The ceilings
        // are sanity, not policy.
        ipc.RingMb = QueryEnvUint32("MOBILEGL_IPC_RING_MB", 8, 1, 1024);
        ipc.StageMb = QueryEnvUint32("MOBILEGL_IPC_STAGE_MB", 32, 1, 4096);
        // P7 wave 4 M2 (Config.h has the semantics). 0 is admitted ON PURPOSE as the negative
        // control: no forced sync, and MagmaWireReclaimScenario's watermark case must go red.
        ipc.WireDeferredMb = QueryEnvUint32("MOBILEGL_IPC_WIRE_DEFERRED_MB", 64, 0, 65536);
        ipc.SpinUs = QueryEnvUint32("MOBILEGL_IPC_SPIN_US", 50, 0, 1000000);
        // PH-6 (ID-P7-2; Config.h has the semantics). 0 is NOT admitted: a server with no
        // patience at all would forfeit a healthy run-ahead client the first time its ring
        // filled between two of the client's drains, which is an ordinary backlog.
        ipc.EventWaitMs = QueryEnvUint32("MOBILEGL_IPC_EVENT_WAIT_MS", 2000, 1, 600000);
        // 0 is admitted ON PURPOSE and is the negative control of exit gate E3(a): it turns
        // the persistent-map push OFF, and PersistentCoherentMapScenario must go red.
        ipc.PersistentBlockKb = QueryEnvUint32("MOBILEGL_IPC_PERSISTENT_BLOCK_KB", 64, 0, 65536);
        // Whole-range push is the 0 arm; with it on (default) only blocks whose
        // xxHash64 changed since the last push are shipped.
        ipc.PersistentHashSuppress = QueryEnvUint32("MOBILEGL_IPC_PERSISTENT_HASH_SUPPRESS", 1, 0, 1);
        ipc.BatchWaits = QueryEnvUint32("MOBILEGL_IPC_BATCH_WAITS", 1, 0, 1);
        // The verify harness compares the pushed block against the applier per verb; a
        // batched queue lets the comparer read a supplied field mid-apply, which is a
        // torn read rather than a divergence. The batch is therefore off whenever the
        // shadow comparer is armed.
        if (MG_Config::Features.PipeVerify) ipc.BatchWaits = 0;
        // 2 is the only tier P5 implements (R-6). 0 and 1 parse here and are refused at the
        // point of use, which is where the "P11" in the message belongs.
        ipc.AdoptTier = QueryEnvUint32("MOBILEGL_IPC_ADOPT_TIER", 2, 0, 2);
        ipc.VerbBarrier = QueryEnvUint32("MOBILEGL_IPC_VERB_BARRIER", 1, 0, 1);
        // P5e (MG_Remote/CONTRACT-P5E.md §1). The wait rule's A/B, parsed here like every
        // other IPC knob and armed only where the server publishes kCapRunAheadApply.
        ipc.RunAhead = QueryEnvUint32("MOBILEGL_IPC_RUN_AHEAD", 1, 0, 1);
        // ... and forced OFF by the verify harness for BatchWaits' reason, one step further:
        // run-ahead's whole point is that the client stops filling gPipeInputs for an
        // unbarriered record, and the comparator has nothing left to compare when it does.
        if (MG_Config::Features.PipeVerify) ipc.RunAhead = 0;
        // The present credit (ruling 4). 1 is one frame of overlap; 8 is the ceiling because a
        // deeper queue buys nothing on a CPU-bound client and pays for it in latency. 0 is NOT
        // admitted: a credit of zero would mean "publish no present at all".
        ipc.PresentCredit = QueryEnvUint32("MOBILEGL_IPC_PRESENT_CREDIT", 1, 1, 8);
        // CONTRACT-P6 D5b's reply bound and P7's cold-start budget (Config.h has the semantics).
        // Both are floored at 100 ms: a zero bound would declare every live server silent.
        ipc.ControlTimeoutMs = QueryEnvUint32("MOBILEGL_IPC_CONTROL_TIMEOUT_MS", 5000, 100, 600000);
        ipc.ColdStartMs = QueryEnvUint32("MOBILEGL_IPC_COLD_START_MS", 20000, 100, 600000);
        ipc.StrictErrors = QueryEnvFlag("MOBILEGL_IPC_STRICT_ERRORS");
        ipc.Audit = QueryEnvFlag("MOBILEGL_IPC_AUDIT");
        QueryEnvVariable("MOBILEGL_IPC_SERVER_AFFINITY", ipc.ServerAffinity, "auto");
        // P5f f1: the dual-block rehearsal (P5F-WIRE-COMPLETENESS.md §4). Forced OFF by the
        // verify harness: the comparator's entry compare and compare-at-read hook are built on
        // there being ONE filled block (the hook pins itself to &gPipeInputs,
        // PipeFill.cpp's MGPipeVerifyReadHook), which is exactly what the rehearsal splits.
        ipc.RoleSplitState = QueryEnvFlag("MOBILEGL_IPC_ROLE_SPLIT_STATE");
        if (MG_Config::Features.PipeVerify && ipc.RoleSplitState) {
            MGLOG_W("Config: MOBILEGL_IPC_ROLE_SPLIT_STATE=1 is incompatible with "
                    "MOBILEGL_PIPE_VERIFY (the comparator owns the single fill block); "
                    "the dual-block rehearsal is OFF for this run");
            ipc.RoleSplitState = false;
        }

        // P6 `dl` (CONTRACT-P6 5.3): MOBILEGL_IPC_RESPAWN IS A NAMED REFUSAL, NOT A NO-OP.
        //
        // The device-lost latch is deliberately one-way - a session whose server died has
        // nothing to recover into, because every handle the client minted names an object in a
        // process that no longer exists. Re-pushing the world onto a fresh server is the work
        // this knob would turn on, and no stage has written it.
        //
        // Refused BY NAME rather than parsed and ignored, for the reason Config.h gives about
        // the whole IPC family: an environment variable nothing consumes is indistinguishable
        // from one that is consumed and does nothing, and an operator who set this one would
        // otherwise conclude that recovery had been tried and had not helped.
        {
            String respawn;
            QueryEnvVariable("MOBILEGL_IPC_RESPAWN", respawn, "");
            if (!respawn.empty() && respawn != "0") {
                MGLOG_E("Config: MOBILEGL_IPC_RESPAWN='%s' names a recovery NO STAGE HAS "
                        "IMPLEMENTED. The device-lost latch is one-way on purpose: every handle "
                        "this client minted names an object inside the server process, so a new "
                        "server would have to be re-pushed the entire world before a single verb "
                        "could land. This run will latch device-lost and stay there.",
                        respawn.c_str());
            }
        }

        // P12 (on-screen server window): MOBILEGL_IPC_SURFACE = offscreen | server (Config.h has
        // the semantics). An unknown value is named and read as the default, like every knob here.
        {
            String surface;
            QueryEnvVariable("MOBILEGL_IPC_SURFACE", surface, "offscreen");
            std::transform(surface.begin(), surface.end(), surface.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (surface.empty() || surface == "offscreen") {
                ipc.Surface = MG_Config::IpcSurface::Offscreen;
            } else if (surface == "server") {
                ipc.Surface = MG_Config::IpcSurface::Server;
            } else {
                MGLOG_W("Config: Ignoring invalid env variable MOBILEGL_IPC_SURFACE='%s'; expected "
                        "offscreen|server, using offscreen",
                        surface.c_str());
                ipc.Surface = MG_Config::IpcSurface::Offscreen;
            }
            // (Not said by a server process: a spawned server inherits the client's environment,
            // parses this before RunSession sets its own transport, and has no client half anyway.)
            const char* role = std::getenv("MOBILEGL_IPC_ROLE");
            const Bool serverProcess = role != nullptr && std::strcmp(role, "server") == 0;
            if (ipc.Surface == MG_Config::IpcSurface::Server && !serverProcess &&
                MG_Config::Transport != MG_Config::TransportMode::Spawn) {
                // Said, not silently honoured: there is no remote server here to own a window.
                MGLOG_W("Config: MOBILEGL_IPC_SURFACE=server is IGNORED - it asks a remote server "
                        "(MOBILEGL_TRANSPORT=spawn, a fork or tcp:// control) to own the window surface, "
                        "and this run has none. Window surfaces stay the client's own");
            }
        }

        if (MG_Config::Transport == MG_Config::TransportMode::Monolith) return;
        // One line, on the arm where these numbers decide behaviour, because every one of
        // them is a number a bug report has to quote.
        MGLOG_I("Config: IPC ring=%uMiB stage=%uMiB wire-deferred=%uMiB spin=%uus event-wait=%ums "
                "persistent-block=%uKiB "
                "adopt-tier=%u verb-barrier=%u run-ahead=%u present-credit=%u control-timeout=%ums "
                "cold-start=%ums strict=%d audit=%d role-split-state=%d affinity='%s' surface=%s",
                ipc.RingMb, ipc.StageMb, ipc.WireDeferredMb, ipc.SpinUs, ipc.EventWaitMs,
                ipc.PersistentBlockKb, ipc.AdoptTier,
                ipc.VerbBarrier, ipc.RunAhead, ipc.PresentCredit, ipc.ControlTimeoutMs, ipc.ColdStartMs,
                static_cast<int>(ipc.StrictErrors), static_cast<int>(ipc.Audit),
                static_cast<int>(ipc.RoleSplitState), ipc.ServerAffinity.c_str(),
                MG_Config::ServerOwnedWindowSurfaces() ? "server" : "offscreen");
        if (ipc.VerbBarrier == 0) {
            MGLOG_W("Config: MOBILEGL_IPC_VERB_BARRIER=0 is the R-1 NEGATIVE CONTROL and is "
                    "expected to fail: the client still pulls 31 of 63 PipeInputs fields from a "
                    "live GLContext, so an unbarriered queue lets the server read future values");
        }
        if (ipc.WireDeferredMb == 0) {
            MGLOG_W("Config: MOBILEGL_IPC_WIRE_DEFERRED_MB=0 is the M2 NEGATIVE CONTROL: the "
                    "server never forces a sync for orphaned wire buffer stores, so a long frame "
                    "that respecifies and draws holds every one of them until the frame ends");
        }
        if (ipc.PersistentBlockKb == 0) {
            MGLOG_W("Config: MOBILEGL_IPC_PERSISTENT_BLOCK_KB=0 is the E3(a) NEGATIVE CONTROL: "
                    "the persistent-map push is OFF and a coherent-map scenario must go red");
        }
    }
#endif

    void Init() {
        MGLOG_D("Loading configuration from environment variables...");
        InitializeAcceptedEnvVariables();

        InitBackendType();
        InitFeatures();
#if MOBILEGL_BUILD_DISAGGREGATED
        // After InitFeatures, so the one line InitIpc logs is the last word on this run's
        // configuration, and before the accepted-env map is destroyed just below.
        InitTransport();
        InitIpc();
#endif

        // Destroy the map since we won't need it anymore
        acceptedEnvVariablesMap.reset();
    }
} // namespace MobileGL::MG_ConfigLoader
