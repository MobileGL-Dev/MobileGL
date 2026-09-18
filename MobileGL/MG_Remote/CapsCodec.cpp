// MobileGL - MobileGL/MG_Remote/CapsCodec.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P5 package w1: MGPCaps's two blob serializers, the pair MGPipeTypes.h:134-136 defers to
// this phase by name ("Their serializers land with the transport (P5)").
//
// THE FORMAT, and every part of it is a refusal rather than a guess. It is
// ProgramArtifactsCodec's shape on purpose - that codec is the tree's one worked example of a
// wire format that has survived a real transport, so copying it is cheaper than being
// original and much cheaper than being wrong:
//
//   * a VERSION word first, and the two TABLE DIMENSIONS second, so a peer whose
//     TextureInternalFormat enum grew is a mismatch AT READ TIME rather than a silent shear
//     that reads one format's capabilities as another's;
//   * length-prefixed everything, with the count checked against the bytes that REMAIN before
//     a single element is reserved, so a corrupt count cannot become a four-billion-element
//     resize;
//   * SPARSE for all three capability tables. The dense form is
//     2 * targets * formats * 8 bytes plus the sample-count lists - with 13 targets and 77
//     internal formats that is ~16 KiB of mostly zeroes, PER CONTEXT AND PER CAPS
//     INVALIDATION, because R-12 makes a re-arriving snapshot the invalidation rather than a
//     once-per-process cost. The tables are overwhelmingly empty, so what crosses is
//     (index, value) pairs and the decoder Clear()s first;
//   * little-endian by memcpy of fixed-width scalars, which is what every other MobileGL wire
//     struct already assumes and what the ABI fingerprint below makes checkable;
//   * every decoder returns FALSE on truncation, a bad version, a dimension mismatch, an
//     out-of-range index or TRAILING BYTES THE FORMAT DOES NOT ACCOUNT FOR. These bytes
//     arrive over a wire and the outputs are left in a defined, default state on a refusal.

#include "CapsCodec.h"

#include "Protocol/mg_protocol_base.h"

#include <MGGitHash.h>
#include <MG_Util/Debug/Log.h>

#include <cstdlib>
#include <cstring>

namespace MobileGL::MG_Remote {

    // The consumer mask may not collide with the MGPCapBits below it. The HIGHEST allocated
    // feature bit is kCapBackendOwnsXfbCapture, 1<<9 (P5b t2 raised it from
    // kCapNeedsHostUboBytes' 1<<8); this asserts the gap stays a gap rather than trusting the
    // comment, so it has to name whichever bit is currently the top one.
    static_assert((static_cast<Uint64>(MG_Pipe::kCapBackendOwnsXfbCapture) & kMGCapsConsumerMask) == 0,
                  "an MGPCapBit has grown into CallMask's consumer block (bits 32..47)");
    static_assert(MGCapsServerConsumes(MGCapsConsumerBits(MG_Pipe::kMGPipeSubsystemResources),
                                       MG_Pipe::kMGPipeSubsystemResources),
                  "the consumer encoding does not round-trip");
    static_assert(!MGCapsServerConsumes(MGCapsConsumerBits(MG_Pipe::kMGPipeSubsystemResources),
                                        MG_Pipe::kMGPipeSubsystemPrograms),
                  "the consumer encoding answers yes for a family it was not given");
    // P4a's highest allocated subsystem bit must fit the sixteen-bit block. This is the
    // assertion that turns "room to P8" from a comment into a build break.
    static_assert(MG_Pipe::kMGPipeSubsystemsMigratedAtP4a <= 0xFFFFull,
                  "the subsystem mask no longer fits CallMask's sixteen consumer bits");
    // P5e (sb): and the phase constant that is actually published now, so the assertion tracks
    // the highest allocated bit rather than the highest bit at the time it was written.
    static_assert(MG_Pipe::kMGPipeSubsystemsMigratedAtP5e <= 0xFFFFull,
                  "the subsystem mask no longer fits CallMask's sixteen consumer bits");

    namespace {

        // Bumped whenever the bytes change in a way a previous reader would misread. A reader
        // that sees a different word REFUSES; it never tries to guess a layout.
        constexpr Uint32 kFormatCapabilitiesCodecVersion = 1;
        constexpr Uint32 kRendererInfoCodecVersion = 1;

        // A count is never believed before it is weighed against the bytes that are left. The
        // largest legal element count in either blob is bounded by the tables' own dimensions,
        // but a String's length is not, so the reader checks bytes rather than a constant.
        class Writer {
        public:
            explicit Writer(Vector<Uint8>& out) : m_out(out) {}

            void Raw(const void* bytes, SizeT size) {
                if (size == 0) {
                    return;
                }
                const auto* p = static_cast<const Uint8*>(bytes);
                m_out.insert(m_out.end(), p, p + size);
            }
            void U8(Uint8 v) { Raw(&v, sizeof(v)); }
            void U32(Uint32 v) { Raw(&v, sizeof(v)); }
            void U64(Uint64 v) { Raw(&v, sizeof(v)); }
            void I32(Int32 v) { Raw(&v, sizeof(v)); }
            void Str(const String& s) {
                U32(static_cast<Uint32>(s.size()));
                Raw(s.data(), s.size());
            }
            void OptStr(const Optional<String>& s) {
                U8(s.has_value() ? 1u : 0u);
                if (s.has_value()) {
                    Str(*s);
                }
            }

        private:
            Vector<Uint8>& m_out;
        };

        class Reader {
        public:
            Reader(const void* bytes, Uint64 size)
                : m_p(static_cast<const Uint8*>(bytes)), m_left(bytes != nullptr ? size : 0) {}

            Bool Raw(void* out, Uint64 size) {
                if (!m_ok || size > m_left) {
                    m_ok = false;
                    return false;
                }
                std::memcpy(out, m_p, static_cast<SizeT>(size));
                m_p += size;
                m_left -= size;
                return true;
            }
            Bool U8(Uint8& v) { return Raw(&v, sizeof(v)); }
            Bool U32(Uint32& v) { return Raw(&v, sizeof(v)); }
            Bool U64(Uint64& v) { return Raw(&v, sizeof(v)); }
            Bool I32(Int32& v) { return Raw(&v, sizeof(v)); }
            Bool Str(String& s) {
                Uint32 length = 0;
                if (!U32(length)) {
                    return false;
                }
                // THE CHECK THAT MATTERS: the count is weighed against the bytes that remain
                // BEFORE the string is sized, so a corrupt length is a refusal rather than a
                // four-gigabyte allocation.
                if (length > m_left) {
                    m_ok = false;
                    return false;
                }
                s.assign(reinterpret_cast<const char*>(m_p), static_cast<SizeT>(length));
                m_p += length;
                m_left -= length;
                return true;
            }
            Bool OptStr(Optional<String>& s) {
                Uint8 present = 0;
                if (!U8(present)) {
                    return false;
                }
                if (present == 0) {
                    s.reset();
                    return true;
                }
                String value;
                if (!Str(value)) {
                    return false;
                }
                s = Move(value);
                return true;
            }
            // How many elements of `elementBytes` could still possibly be there. The gate a
            // length-prefixed array is held to before it reserves anything.
            Uint64 RoomFor(Uint64 elementBytes) const {
                return elementBytes == 0 ? 0 : m_left / elementBytes;
            }
            Bool Ok() const { return m_ok; }
            Uint64 Left() const { return m_left; }
            // Trailing bytes the format does not account for are a REFUSAL: they mean the
            // writer and the reader disagree about the shape, and the half that was read is
            // not trustworthy just because it parsed.
            Bool Finished() const { return m_ok && m_left == 0; }

        private:
            const Uint8* m_p = nullptr;
            Uint64 m_left = 0;
            Bool m_ok = true;
        };

        using MG_Backend::FormatCapabilityCache;
        using MG_Backend::FormatCapabilityFlags;

        constexpr Uint64 kFormatCells = static_cast<Uint64>(MG_Backend::kFormatCapabilityTargetCount) *
                                        static_cast<Uint64>(MG_Backend::kFormatCapabilityFormatCount);

    } // namespace

    // ---------------------------------------------------------------------------------
    // FormatCapabilityCache
    // ---------------------------------------------------------------------------------

    Bool EncodeFormatCapabilities(const FormatCapabilityCache& cache, Vector<Uint8>& out) {
        Writer w(out);
        w.U32(kFormatCapabilitiesCodecVersion);
        w.U32(static_cast<Uint32>(MG_Backend::kFormatCapabilityTargetCount));
        w.U32(static_cast<Uint32>(MG_Backend::kFormatCapabilityFormatCount));
        w.U32(0); // reserved, keeps the header 16 bytes and 8-aligned for the pairs below

        // The two flag tables, sparse. A cell is written only when it is non-zero, so what
        // crosses is proportional to what the driver actually supports rather than to the
        // square of two enum spaces.
        const auto writeTable = [&](const MG_Backend::FormatCapabilityTable& table) {
            Uint32 populated = 0;
            for (SizeT t = 0; t < MG_Backend::kFormatCapabilityTargetCount; ++t) {
                for (SizeT f = 0; f < MG_Backend::kFormatCapabilityFormatCount; ++f) {
                    if (table[t][f].GetRaw() != 0) {
                        ++populated;
                    }
                }
            }
            w.U32(populated);
            for (SizeT t = 0; t < MG_Backend::kFormatCapabilityTargetCount; ++t) {
                for (SizeT f = 0; f < MG_Backend::kFormatCapabilityFormatCount; ++f) {
                    const Uint64 raw = static_cast<Uint64>(table[t][f].GetRaw());
                    if (raw == 0) {
                        continue;
                    }
                    w.U32(static_cast<Uint32>(t * MG_Backend::kFormatCapabilityFormatCount + f));
                    w.U64(raw);
                }
            }
        };
        writeTable(cache.FullCaps);
        writeTable(cache.CaveatCaps);

        // The sample-count lists: the Vector<Int> that is the reason this cannot be a memcpy.
        Uint32 populated = 0;
        for (SizeT t = 0; t < MG_Backend::kFormatCapabilityTargetCount; ++t) {
            for (SizeT f = 0; f < MG_Backend::kFormatCapabilityFormatCount; ++f) {
                if (!cache.SampleCounts[t][f].empty()) {
                    ++populated;
                }
            }
        }
        w.U32(populated);
        for (SizeT t = 0; t < MG_Backend::kFormatCapabilityTargetCount; ++t) {
            for (SizeT f = 0; f < MG_Backend::kFormatCapabilityFormatCount; ++f) {
                const Vector<Int>& counts = cache.SampleCounts[t][f];
                if (counts.empty()) {
                    continue;
                }
                w.U32(static_cast<Uint32>(t * MG_Backend::kFormatCapabilityFormatCount + f));
                w.U32(static_cast<Uint32>(counts.size()));
                for (const Int value : counts) {
                    w.I32(static_cast<Int32>(value));
                }
            }
        }
        return true;
    }

    Bool DecodeFormatCapabilities(const void* bytes, Uint64 size, FormatCapabilityCache& out) {
        out.Clear();
        Reader r(bytes, size);

        Uint32 version = 0;
        Uint32 targets = 0;
        Uint32 formats = 0;
        Uint32 reserved = 0;
        if (!r.U32(version) || !r.U32(targets) || !r.U32(formats) || !r.U32(reserved)) {
            return false;
        }
        if (version != kFormatCapabilitiesCodecVersion) {
            MGLOG_E("MG_Remote caps: format-capability blob is version %u, this build reads %u",
                    version, kFormatCapabilitiesCodecVersion);
            return false;
        }
        if (targets != MG_Backend::kFormatCapabilityTargetCount ||
            formats != MG_Backend::kFormatCapabilityFormatCount) {
            // Not a corrupt stream: a peer whose TextureTarget or TextureInternalFormat enum
            // is a different size. Reading it anyway shears every cell onto a neighbouring
            // format, which is exactly the failure the ABI fingerprint exists to make loud.
            MGLOG_E("MG_Remote caps: format-capability blob is %ux%u, this build is %llux%llu",
                    targets, formats,
                    static_cast<unsigned long long>(MG_Backend::kFormatCapabilityTargetCount),
                    static_cast<unsigned long long>(MG_Backend::kFormatCapabilityFormatCount));
            return false;
        }

        const auto readTable = [&](MG_Backend::FormatCapabilityTable& table) -> Bool {
            Uint32 populated = 0;
            if (!r.U32(populated)) {
                return false;
            }
            if (populated > r.RoomFor(sizeof(Uint32) + sizeof(Uint64))) {
                return false;
            }
            for (Uint32 i = 0; i < populated; ++i) {
                Uint32 index = 0;
                Uint64 raw = 0;
                if (!r.U32(index) || !r.U64(raw)) {
                    return false;
                }
                if (static_cast<Uint64>(index) >= kFormatCells) {
                    return false;
                }
                table[index / MG_Backend::kFormatCapabilityFormatCount]
                     [index % MG_Backend::kFormatCapabilityFormatCount] =
                         FormatCapabilityFlags(raw);
            }
            return true;
        };
        if (!readTable(out.FullCaps) || !readTable(out.CaveatCaps)) {
            out.Clear();
            return false;
        }

        Uint32 populated = 0;
        if (!r.U32(populated) || populated > r.RoomFor(2 * sizeof(Uint32))) {
            out.Clear();
            return false;
        }
        for (Uint32 i = 0; i < populated; ++i) {
            Uint32 index = 0;
            Uint32 count = 0;
            if (!r.U32(index) || !r.U32(count)) {
                out.Clear();
                return false;
            }
            if (static_cast<Uint64>(index) >= kFormatCells || count > r.RoomFor(sizeof(Int32))) {
                out.Clear();
                return false;
            }
            Vector<Int>& counts = out.SampleCounts[index / MG_Backend::kFormatCapabilityFormatCount]
                                                  [index % MG_Backend::kFormatCapabilityFormatCount];
            counts.resize(static_cast<SizeT>(count));
            for (Uint32 j = 0; j < count; ++j) {
                Int32 value = 0;
                if (!r.I32(value)) {
                    out.Clear();
                    return false;
                }
                counts[j] = static_cast<Int>(value);
            }
        }

        if (!r.Finished()) {
            MGLOG_E("MG_Remote caps: format-capability blob has %llu trailing bytes the format "
                    "does not account for",
                    static_cast<unsigned long long>(r.Left()));
            out.Clear();
            return false;
        }
        return true;
    }

    // ---------------------------------------------------------------------------------
    // RendererInfo
    // ---------------------------------------------------------------------------------

    namespace {

        void WriteVersion(Writer& w, const Version& v) {
            w.I32(static_cast<Int32>(v.Major));
            w.I32(static_cast<Int32>(v.Minor));
            w.I32(static_cast<Int32>(v.Patch));
            w.OptStr(v.Suffix);
            w.U8(v.Type.has_value() ? 1u : 0u);
            w.U8(v.Type.has_value() ? static_cast<Uint8>(*v.Type) : 0u);
        }

        Bool ReadVersion(Reader& r, Version& v) {
            Int32 major = 0;
            Int32 minor = 0;
            Int32 patch = 0;
            if (!r.I32(major) || !r.I32(minor) || !r.I32(patch)) {
                return false;
            }
            v.Major = static_cast<Int>(major);
            v.Minor = static_cast<Int>(minor);
            v.Patch = static_cast<Int>(patch);
            if (!r.OptStr(v.Suffix)) {
                return false;
            }
            Uint8 hasType = 0;
            Uint8 type = 0;
            if (!r.U8(hasType) || !r.U8(type)) {
                return false;
            }
            if (hasType != 0) {
                v.Type = static_cast<VersionType>(type);
            } else {
                v.Type.reset();
            }
            return true;
        }

    } // namespace

    Bool EncodeRendererInfo(const RendererInfo& info, Vector<Uint8>& out) {
        Writer w(out);
        w.U32(kRendererInfoCodecVersion);
        w.Str(info.RendererName);
        w.Str(info.BackendName);
        w.OptStr(info.ExtraVendor);
        WriteVersion(w, info.RendererGLInfo.TargetGLVersion);
        WriteVersion(w, info.RendererGLInfo.TargetGLSLVersion);
        w.U32(static_cast<Uint32>(info.RendererGLInfo.Extensions.size()));
        for (const GLExtension extension : info.RendererGLInfo.Extensions) {
            w.U32(static_cast<Uint32>(extension));
        }
        w.U8(info.RendererGLInfo.IsCompatibilityProfile ? 1u : 0u);
        w.U8(info.StaticBackendCapability.AllowVSOnlyPrograms ? 1u : 0u);
        return true;
    }

    Bool DecodeRendererInfo(const void* bytes, Uint64 size, RendererInfo& out) {
        out = RendererInfo{};
        Reader r(bytes, size);

        Uint32 version = 0;
        if (!r.U32(version)) {
            return false;
        }
        if (version != kRendererInfoCodecVersion) {
            MGLOG_E("MG_Remote caps: renderer-info blob is version %u, this build reads %u", version,
                    kRendererInfoCodecVersion);
            return false;
        }
        if (!r.Str(out.RendererName) || !r.Str(out.BackendName) || !r.OptStr(out.ExtraVendor) ||
            !ReadVersion(r, out.RendererGLInfo.TargetGLVersion) ||
            !ReadVersion(r, out.RendererGLInfo.TargetGLSLVersion)) {
            out = RendererInfo{};
            return false;
        }

        Uint32 extensionCount = 0;
        if (!r.U32(extensionCount) || extensionCount > r.RoomFor(sizeof(Uint32))) {
            out = RendererInfo{};
            return false;
        }
        out.RendererGLInfo.Extensions.resize(static_cast<SizeT>(extensionCount));
        for (Uint32 i = 0; i < extensionCount; ++i) {
            Uint32 value = 0;
            if (!r.U32(value)) {
                out = RendererInfo{};
                return false;
            }
            out.RendererGLInfo.Extensions[i] = static_cast<GLExtension>(value);
        }

        Uint8 compatibility = 0;
        Uint8 vsOnly = 0;
        if (!r.U8(compatibility) || !r.U8(vsOnly)) {
            out = RendererInfo{};
            return false;
        }
        out.RendererGLInfo.IsCompatibilityProfile = compatibility != 0;
        out.StaticBackendCapability.AllowVSOnlyPrograms = vsOnly != 0;

        if (!r.Finished()) {
            MGLOG_E("MG_Remote caps: renderer-info blob has %llu trailing bytes the format does "
                    "not account for",
                    static_cast<unsigned long long>(r.Left()));
            out = RendererInfo{};
            return false;
        }
        return true;
    }

    // ---------------------------------------------------------------------------------
    // The ABI assertion the handshake carries
    // ---------------------------------------------------------------------------------

    Transport::AbiFingerprintInputs CapsAbiFingerprintInputs() {
        // MGPCaps has only a COMPOSITIONAL size assertion (MGPipeTypes.h:145-146) because
        // DynamicBackendParameters still carries SizeT and GLenum members - P0.5's fixed-width
        // rewrite did not happen and P5 does not do it either (table 0's ABI row; the rewrite
        // is P7's account). So the caps block's literal size IS ABI-dependent, and this
        // fingerprint is what turns that from a latent hazard into a named refusal.
        //
        // The git stamp is in it because two builds of the same sizes can still disagree about
        // a FIELD ORDER, which no sizeof can see; P6's spawn is same-machine and same-binary,
        // so it inherits this unchanged rather than needing a looser rule.
        Transport::AbiFingerprintInputs inputs;
        inputs.DynamicParamsSize = sizeof(MG_Backend::DynamicBackendParameters);
        inputs.CapsSize = sizeof(MG_Pipe::MGPCaps);
        inputs.FunctionTableSize = sizeof(MG_Backend::GLFunctionsTable);
        inputs.FormatCapabilityTargets = static_cast<Uint64>(MG_Backend::kFormatCapabilityTargetCount);
        inputs.FormatCapabilityFormats = static_cast<Uint64>(MG_Backend::kFormatCapabilityFormatCount);
        inputs.FormatCapabilitiesCodecVersion = kFormatCapabilitiesCodecVersion;
        inputs.RendererInfoCodecVersion = kRendererInfoCodecVersion;
        inputs.OpCount = static_cast<Uint64>(MG_Pipe::MGPWireOp::kOpCount);
        // The declared protocol ABI, carried over from s1's version of this function at
        // integration (ID-33). The sizeofs above catch a struct that changed shape; this
        // catches a peer that changed the PROTOCOL while every struct stayed the same size,
        // which is the one break the rest of the mix is blind to.
        inputs.AbiVersion =
            static_cast<Uint32>(MOBILEGL_ABI_VERSION(MOBILEGL_PROTOCOL_ABI_MAJOR, MOBILEGL_PROTOCOL_ABI_MINOR));
        inputs.BuildStamp = GIT_COMMIT_HASH_SHORT;
        return inputs;
    }

    // ONE implementation, deliberately a one-liner: the wave-1 review (ID-46 finding 6) found a
    // second hand-rolled FNV loop here while the mixer under Transport/ had no production caller,
    // so the sensitivity test could not see this function change. Now it starts from here.
    Uint64 CapsAbiFingerprint() { return Transport::MixAbiFingerprint(CapsAbiFingerprintInputs()); }

} // namespace MobileGL::MG_Remote
