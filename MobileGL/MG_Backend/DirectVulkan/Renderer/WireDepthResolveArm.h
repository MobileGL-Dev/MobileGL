// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/WireDepthResolveArm.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
#pragma once

#include <Includes.h>

// P7 gate 5 (g5-msrbo, g5-msprobe): WHICH ARM RESOLVES A MULTISAMPLE DEPTH/STENCIL ASPECT FIRST.
//
// The wire arm has two (WireFramebuffer.inc, ResolveWireDepthStencil): a render pass that carries
// a VK_KHR_depth_stencil_resolve attachment and no draw, and the baked shader pass of
// WireMultisampleResolve.inc, which draws. P7 wave 2-B2 took the render pass wherever the
// extension exists. On the Redmi (Adreno 830, Vulkan 1.3.284, driver 512.800.71) that pass left
// its resolve target unwritten - KHR-GL46.direct_state_access.renderbuffers_storage_multisample
// read every resolved depth and stencil value back as 0 on the inproc arm while the monolith arm
// passed, and the same case passed on the device with the shader arm forced
// (MGITEST_MAGMA_FORCE_SHADER_DEPTH_RESOLVE=1).
//
// A DRIVER THAT CLAIMS WHAT IT CANNOT DO IS A POST PROBE, NOT A VENDOR QUIRK (standing rule,
// 2026-08-22). The first fix keyed the order on Qualcomm's vendor id; it is replaced by a probe
// that reproduces the production resolve on the actual VkDevice (WireDepthResolveProbe.cpp) and
// asks the one question the order depends on: does the no-draw render pass write the resolve
// target? A driver that fixes the defect gets the fast arm back, and a driver of any vendor that
// has it gets the shader arm - neither needs a table edit.
//
// EVERY BUG PROBE CARRIES A CONTROL (MG_Util/SelfTest/DriverBugProbes.h). The control is the
// production SHADER arm fed the same inputs: when it does not resolve them either, the probe has
// measured its own setup (or a device that cannot resolve depth at all) rather than the render
// pass, the verdict is INCONCLUSIVE, and the default order stands.
//
// This header is the pure half - the measurement, the verdict, the test knob and the choice - so
// a unit test pins every mapping with a fake result source (MG_Test/Pipeline/PipelineQuirkTest.cpp).
// The Vulkan half that fills the measurement is WireDepthResolveProbe.{h,cpp}.
namespace MobileGL::MG_Backend::DirectVulkan {
    // One aspect of one probed format, both arms. Values are the RAW aspect words the
    // buffer copy returns (D16: 16 bits; D24: the low 24 bits of the 32-bit word; D32F: the float's
    // bits; stencil: 8 bits), compared within `tolerance` (1 LSB for a UNORM depth, 0 otherwise).
    struct WireDepthResolveAspectReading {
        // The format has this aspect and the render-pass arm's copy of it was read back.
        Bool measured = false;
        Uint32 texels = 0;
        Uint32 expected = 0;  // the multisample source's cleared value
        Uint32 sentinel = 0;  // what the resolve target was pre-filled with
        Uint32 tolerance = 0;
        // THE SUBJECT: VK_KHR_depth_stencil_resolve render pass, no draw.
        Uint32 renderPassMatches = 0;   // texels within tolerance of `expected`
        Uint32 renderPassSentinels = 0; // texels still holding the sentinel
        Uint32 renderPassFirst = 0;     // texel (0,0)
        // THE CONTROL: the production shader pass, same inputs. False where it cannot run for this
        // aspect (a stencil aspect without VK_EXT_shader_stencil_export, a failed pipeline).
        Bool shaderRan = false;
        Uint32 shaderMatches = 0;
        Uint32 shaderFirst = 0;
    };

    struct WireDepthResolveFormatReading {
        String name;          // e.g. "D24_UNORM_S8_UINT"
        Int32 format = 0;     // the VkFormat
        Uint32 samples = 0;
        // False when the format was not probed; `skipReason` says why (unsupported, the render-pass
        // arm would not be taken for it, an object the probe needed could not be created).
        Bool ran = false;
        String skipReason;
        WireDepthResolveAspectReading depth, stencil;
    };

    struct WireDepthResolveProbeMeasurement {
        // At least one format was recorded, submitted and read back.
        Bool ran = false;
        // The bounded fence wait expired: every object the probe created was deliberately leaked
        // (the queue may still be executing it), and nothing was read back.
        Bool fenceWaitTimedOut = false;
        String failureReason;
        // Came from MGITEST_MAGMA_DEPTH_RESOLVE_PROBE, not from the device.
        Bool fromKnob = false;
        Vector<WireDepthResolveFormatReading> formats;
    };

    enum class WireDepthResolveProbeVerdict : Uint8 {
        // Nothing was measured: the render-pass arm is not available (nothing to choose between),
        // or the probe could not set itself up. The default order stands.
        NotRun,
        // Measured, but no format's CONTROL resolved: the probe cannot tell the render pass's
        // defect from its own. The default order stands, and the report must never call it a bug.
        Inconclusive,
        // Every format whose control resolved was resolved by the render pass too.
        Clean,
        // The defect: on at least one format the shader control resolved the inputs and the
        // render pass did not (wrote nothing - the sentinel survived - or wrote wrong values).
        RenderPassResolveBroken,
    };

    inline const char* WireDepthResolveProbeVerdictName(WireDepthResolveProbeVerdict verdict) {
        switch (verdict) {
            case WireDepthResolveProbeVerdict::NotRun: return "not-run";
            case WireDepthResolveProbeVerdict::Inconclusive: return "inconclusive";
            case WireDepthResolveProbeVerdict::Clean: return "clean";
            case WireDepthResolveProbeVerdict::RenderPassResolveBroken: return "render-pass-resolve-broken";
        }
        return "?";
    }

    inline Bool WireDepthResolveControlHolds(const WireDepthResolveAspectReading& aspect) {
        return aspect.measured && aspect.shaderRan && aspect.texels > 0 && aspect.shaderMatches == aspect.texels;
    }

    inline Bool WireDepthResolveSubjectHolds(const WireDepthResolveAspectReading& aspect) {
        return aspect.texels > 0 && aspect.renderPassMatches == aspect.texels;
    }

    // An aspect only counts where its control holds; a format is conclusive when any of its
    // aspects is. The render pass resolves every aspect of the format in one pass, so one aspect
    // it fails is the defect for the whole arm.
    inline WireDepthResolveProbeVerdict EvaluateWireDepthResolveProbe(const WireDepthResolveProbeMeasurement& measurement) {
        if (!measurement.ran || measurement.fenceWaitTimedOut) return WireDepthResolveProbeVerdict::NotRun;
        Bool conclusive = false, broken = false;
        for (const WireDepthResolveFormatReading& reading : measurement.formats) {
            if (!reading.ran) continue;
            for (const WireDepthResolveAspectReading* aspect : {&reading.depth, &reading.stencil}) {
                if (!WireDepthResolveControlHolds(*aspect)) continue;
                conclusive = true;
                if (!WireDepthResolveSubjectHolds(*aspect)) broken = true;
            }
        }
        if (broken) return WireDepthResolveProbeVerdict::RenderPassResolveBroken;
        return conclusive ? WireDepthResolveProbeVerdict::Clean : WireDepthResolveProbeVerdict::Inconclusive;
    }

    // THE TEST KNOB, read by the SERVER (split and spawn arms only; the tcp server is a lane fixture
    // whose environment no entry reaches - spawn_lane_parity.py's MAGMA_SERVER_ENV_KNOB_NO_TCP).
    // Lavapipe resolves correctly through both arms, so no host lane can reach the verdict that
    // matters on the Redmi. MGITEST_MAGMA_DEPTH_RESOLVE_PROBE=bug|clean replaces the MEASUREMENT
    // with a canned one that says exactly that, and nothing else: the canned measurement still goes
    // through EvaluateWireDepthResolveProbe and the same member, so an entry that forces `bug` is red
    // when the evaluation stops detecting the defect.
    enum class WireDepthResolveProbeKnob : Uint8 { Measure, ForceBug, ForceClean, Unrecognised };

    inline WireDepthResolveProbeKnob ParseWireDepthResolveProbeKnob(const char* value) {
        if (value == nullptr || *value == '\0') return WireDepthResolveProbeKnob::Measure;
        if (std::strcmp(value, "bug") == 0) return WireDepthResolveProbeKnob::ForceBug;
        if (std::strcmp(value, "clean") == 0) return WireDepthResolveProbeKnob::ForceClean;
        return WireDepthResolveProbeKnob::Unrecognised;
    }

    // One packed format's worth of readings: the control resolved both aspects; the render pass
    // either did too (`renderPassWrites`) or left the sentinel everywhere.
    inline WireDepthResolveProbeMeasurement CannedWireDepthResolveProbeMeasurement(Bool renderPassWrites) {
        WireDepthResolveProbeMeasurement measurement;
        measurement.ran = true;
        measurement.fromKnob = true;
        WireDepthResolveFormatReading reading;
        reading.name = "MGITEST_MAGMA_DEPTH_RESOLVE_PROBE";
        reading.samples = 4;
        reading.ran = true;
        const auto fill = [&](WireDepthResolveAspectReading& aspect, Uint32 expected, Uint32 sentinel) {
            aspect.measured = true;
            aspect.texels = 16;
            aspect.expected = expected;
            aspect.sentinel = sentinel;
            aspect.renderPassMatches = renderPassWrites ? 16 : 0;
            aspect.renderPassSentinels = renderPassWrites ? 0 : 16;
            aspect.renderPassFirst = renderPassWrites ? expected : sentinel;
            aspect.shaderRan = true;
            aspect.shaderMatches = 16;
            aspect.shaderFirst = expected;
        };
        fill(reading.depth, 0x400000u, 0xBFFFFFu);
        fill(reading.stencil, 0x5Au, 0xA5u);
        measurement.formats.push_back(Move(reading));
        return measurement;
    }

    struct WireDepthResolveArmChoice {
        WireDepthResolveProbeMeasurement measurement;
        WireDepthResolveProbeVerdict verdict = WireDepthResolveProbeVerdict::NotRun;
        // m_wirePreferShaderDepthResolve: the shader pass goes first, the render pass is the
        // fallback for what the shader cannot write.
        Bool preferShader = false;
    };

    // The whole decision, with the Vulkan half behind `measure` (called at most once, and only when
    // the knob does not decide and the render-pass arm exists at all: without it the shader pass
    // is the only arm and there is no order to choose).
    template <typename MeasureFn>
    WireDepthResolveArmChoice ChooseWireDepthResolveArm(WireDepthResolveProbeKnob knob, Bool renderPassArmAvailable,
                                                        MeasureFn&& measure) {
        WireDepthResolveArmChoice choice;
        if (knob == WireDepthResolveProbeKnob::ForceBug || knob == WireDepthResolveProbeKnob::ForceClean) {
            choice.measurement = CannedWireDepthResolveProbeMeasurement(knob == WireDepthResolveProbeKnob::ForceClean);
        } else if (renderPassArmAvailable) {
            choice.measurement = measure();
        }
        choice.verdict = EvaluateWireDepthResolveProbe(choice.measurement);
        choice.preferShader = choice.verdict == WireDepthResolveProbeVerdict::RenderPassResolveBroken;
        return choice;
    }

    // One format's readings, both arms: "D24_UNORM_S8_UINT x4: depth expected 0x400000 sentinel
    // 0xbfffff: render pass 0/16 (first 0xbfffff, sentinel 16), shader control 16/16 (first
    // 0x400000) / stencil ...".
    inline String DescribeWireDepthResolveFormat(const WireDepthResolveFormatReading& reading) {
        const auto aspectText = [](const char* name, const WireDepthResolveAspectReading& aspect) {
            String text = format("{} expected 0x{:x} sentinel 0x{:x}: render pass {}/{} (first 0x{:x}, sentinel {}), ",
                                 name, aspect.expected, aspect.sentinel, aspect.renderPassMatches, aspect.texels,
                                 aspect.renderPassFirst, aspect.renderPassSentinels);
            if (aspect.shaderRan) {
                text += format("shader control {}/{} (first 0x{:x})", aspect.shaderMatches, aspect.texels,
                               aspect.shaderFirst);
            } else {
                text += "no shader control";
            }
            return text;
        };
        String out = reading.name;
        if (!reading.ran) return out + " skipped (" + reading.skipReason + ")";
        out += format(" x{}: ", reading.samples);
        if (reading.depth.measured) out += aspectText("depth", reading.depth);
        if (reading.depth.measured && reading.stencil.measured) out += " / ";
        if (reading.stencil.measured) out += aspectText("stencil", reading.stencil);
        return out;
    }

    // Every format, "; "-separated, for the POST row.
    inline String DescribeWireDepthResolveProbe(const WireDepthResolveProbeMeasurement& measurement) {
        if (!measurement.ran) {
            return measurement.failureReason.empty() ? String("not run") : "not run (" + measurement.failureReason + ")";
        }
        String out;
        for (const WireDepthResolveFormatReading& reading : measurement.formats) {
            if (!out.empty()) out += "; ";
            out += DescribeWireDepthResolveFormat(reading);
        }
        return out;
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
