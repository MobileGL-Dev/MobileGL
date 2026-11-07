// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/EmulateClipDistancePass.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include "source/opt/pass.h"
#include "spirv-tools/optimizer.hpp"

#include <Includes.h>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            // Emulates gl_ClipDistance on a backend that cannot host the built-in at all: an ES
            // driver without GL_EXT_clip_cull_distance, or a Vulkan device with
            // shaderClipDistance == VK_FALSE (every Mali GPU measured so far is both). GL 3.0+
            // requires GL_MAX_CLIP_DISTANCES >= 8, so a frontend that reports GL 4.6 cannot
            // honestly answer 0 there, and answering 8 without this pass makes every clipping
            // draw either fail to compile or render unclipped.
            //
            // WHAT THE EMULATION IS. Clip distance d is linear in clip space, so its
            // perspective-correct interpolant at a fragment is exactly the distance the
            // rasterizer's clipper would have evaluated there; discarding the fragments where it
            // is negative removes the same pixels hardware clipping removes. So:
            //   - every access to the ClipDistance built-in, in every stage, is redirected onto
            //     a Private float[N] shadow (mg_ClipDistanceShadow) and the ClipDistance
            //     capability is stripped - the gl_PerVertex member stays declared but unaccessed,
            //     which is the shape glslang emits for every program that never clips;
            //   - the LAST pre-rasterization stage (the PRODUCER) writes
            //     `enabled(i) ? shadow[i] : 1.0` into ceil(N/4) ordinary vec4 varyings
            //     (mg_ClipDistanceEmu0/1) at every return of its entry function - before every
            //     EmitVertex in a geometry stage, which is when one vertex's outputs latch.
            //     enabled(i) is bit i of the reserved default-block uniform the source pipeline
            //     declared (CLIP_DISTANCE_ENABLES_UNIFORM_NAME), which the draw path refreshes
            //     from GL_CLIP_DISTANCE0..7 before every draw. Applying the enables HERE keeps
            //     the varyings free of pipeline-key state and makes a disabled distance - even
            //     one the shader writes negative - clip nothing, as GL requires;
            //   - the fragment stage (the CONSUMER) reads the carriers before anything else and
            //     executes OpKill when any carried distance is negative. Its own gl_ClipDistance
            //     reads are served from the same carriers (GLSL leaves the disabled elements
            //     undefined there, so the 1.0 they carry is a legal value).
            //   - every other stage that touches the built-in (a vertex stage ahead of a
            //     geometry stage, say) only has its accesses redirected: in GL those writes are
            //     dead unless a later stage reads gl_in[].gl_ClipDistance.
            //
            // WHAT IT COSTS: two varying locations past every location the program uses, and a
            // discard in the fragment stage of every clipping program (early depth/stencil is
            // lost for those programs). Cull distance is NOT emulated: culling is per primitive,
            // which a per-fragment discard cannot reproduce.
            //
            // WHAT DECLINES (the module is left byte-identical and the report says why):
            // gl_in[].gl_ClipDistance reads and gl_out[].gl_ClipDistance writes (arrayed
            // per-vertex access in tessellation/geometry stages), a non-constant gl_PerVertex
            // member index, whole-gl_PerVertex loads/stores/copies, and a pointer into the
            // built-in that escapes into an opcode the pass cannot retype (a function argument).
            //
            // NATIVE MASKING (EmulateClipDistanceOptions::nativeBuiltin) is the same producer
            // rewrite for a backend that hosts the built-in but cannot switch distances off: the
            // masked values go back into gl_ClipDistance instead of into carriers.
            //
            // One module per run; the program-wide contract (one shared location, which stage
            // is the producer, every stage rewritten or none) is owned by
            // ShaderCompiler::EmulateClipDistanceForProgram, the only caller.
            struct EmulateClipDistanceOptions {
                enum class Role : Uint8 {
                    // Redirect the accesses and strip the capability; no carriers.
                    Redirect,
                    // The last pre-rasterization stage: redirect, then write the carriers.
                    Producer,
                    // The fragment stage: read the carriers, discard, serve gl_ClipDistance reads.
                    Consumer,
                };
                Role role = Role::Redirect;
                // The first carrier location; carrier k sits at location + k.
                Uint32 location = 0;
                // Consumer only: how many distances the producer carries (0 = none, in which
                // case the stage only has its own reads redirected).
                Uint32 distanceCount = 0;
                // Producer only: the default-block uniform member holding the enable bits. When
                // the module has no such member every distance counts as enabled.
                String enablesMemberName;
                // Producer only: the backend HOSTS the built-in (a Vulkan device with
                // shaderClipDistance) but has no per-distance enable state - Vulkan clips by
                // every distance a shader writes. Instead of carriers, the producer writes
                // `enabled(i) ? d[i] : 1.0` back into gl_ClipDistance itself at the same flush
                // points, so hardware clipping stays and a disabled distance clips nothing. The
                // capability is kept, nothing else in the program is touched, and gl_in /
                // fragment-stage reads stay native.
                Bool nativeBuiltin = false;
            };

            struct EmulateClipDistanceReport {
                Bool declined = false;
                String declineReason;
                Bool rewrote = false;
                // Producer: how many distances it now carries.
                Uint32 distanceCount = 0;
                // Producer: whether the enables uniform was found (false = all-enabled fallback).
                Bool enablesFound = false;
            };

            // What one module looks like to the program-wide driver, from a single parse.
            struct ClipDistanceModuleProbe {
                Bool parsed = false;
                // glslang declares the capability on first ACCESS, so this is the trigger.
                Bool declaresClipDistanceCapability = false;
                // The length of the module's non-arrayed ClipDistance OUTPUT (block member or
                // standalone variable), or 0 when it declares none.
                Uint32 outputDistanceCount = 0;
            };

            class EmulateClipDistancePass : public spvtools::opt::Pass {
            public:
                static constexpr const char* kShadowName = "mg_ClipDistanceShadow";
                static constexpr Uint32 kMaxCarriers = 2;
                static const char* CarrierName(Uint32 index) {
                    return index == 0 ? "mg_ClipDistanceEmu0" : "mg_ClipDistanceEmu1";
                }

                EmulateClipDistancePass(EmulateClipDistanceOptions options, EmulateClipDistanceReport* report)
                    : m_options(Move(options)), m_report(report) {}
                const char* name() const override { return "mobilegl-emulate-clip-distance"; }
                Status Process() override;

                static ClipDistanceModuleProbe ProbeModule(const Vector<Uint32>& spirv);

                static spvtools::Optimizer::PassToken CreateEmulateClipDistancePass(EmulateClipDistanceOptions options,
                                                                                    EmulateClipDistanceReport* report);

            private:
                EmulateClipDistanceOptions m_options;
                EmulateClipDistanceReport* m_report;
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
