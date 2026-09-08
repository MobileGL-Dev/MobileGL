// MobileGL - MobileGL/MG_Test/Pipe/VertexInputEmitTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P3a's vertex-input family: create/bind/delete_vertex_elements, set_vertex_buffers and
// set_index_buffer, on both sides of the call.
//
// THIS SUITE IS A NAMED GATE. The phase's G6 is "for every VAO configuration the emitted
// MGPVertexElements blob + MGPVertexBuffers set + MGPIndexBuffer reproduce exactly the values
// the backend's VAO twin reads from the frontend today, field by field, for all 32 attribute
// slots", and it is spelled `ctest -R 'VertexInputEmit\.'`; G7 is its negative control, a
// script that stops the wire conversion copying ONE field and expects this suite to go red
// NAMING that field. So a case here must fail by field name, never by a bare count, or the
// control cannot answer.
//
// THE SUITE IS `VertexInputEmit`, not `VertexInputEmitTest`: the file is XTest.cpp and the
// suite is X, this directory's convention (RenderStateSpansTest.cpp -> RenderStateSpans), and
// it is what both gates grep for.
//
// THE TARGET AND ITS ctest REGISTRATION ARE THE CONTRACT COMMIT'S; THE CONTENTS ARE NOT - the
// client package writes the conversion cases, the base-instance suppression pair and the
// create/bind ping-pong pair into this file without touching MG_Test/Pipe/CMakeLists.txt.
//
// IT HAS ITS OWN main() for the same reason ResourceEmitTest does: the applier refuses a
// vertex-elements record whose declared counts do not describe its own blob, and that verdict
// is a log line in a shipped push build and std::abort() in a poison or verify one.
//
// Every case is a visible SKIP in a pull build rather than a vanishing test, so `ctest -N`
// stays name-for-name identical between the pull and the push trees.

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "Includes.h"
#include <MG_Pipe/MGPipe.h>
#if MOBILEGL_PIPE_PUSH
#include <Config.h>
#include <MG_Impl/Pipe/SetHashSuppressor.h>
#include <MG_Impl/Pipe/SlotAllocator.h>
#include <MG_Impl/Pipe/VertexInputEmit.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_State/GLState/Core.h>
#endif

using namespace MobileGL;
using namespace MobileGL::MG_Pipe;

namespace {
    String g_logPath;

    int ProcessId() {
#if defined(_WIN32)
        return _getpid();
#else
        return static_cast<int>(getpid());
#endif
    }

    // A MAKE-CURRENT CLEARS THE WORKING STATE AND ADVANCES THE SERIALS, and for this family
    // the difference between those two verbs is the whole of the rule. The backend's VAO twin
    // decides "have I already synced this?" by comparing its own memo against the serials, and
    // the twin does NOT die with a make-current - it is destroyed with the context, and D-G4
    // deletes the wrapping-version-plus-identity patch that used to cover the gap. So there
    // are three things a reset could do to a serial whose state it has just cleared and only
    // one of them is right: carrying the count over lets a twin read clean over a cleared
    // window immediately; RESTARTING AT 0 walks the counter back up through every value it has
    // already stamped into a surviving twin, which is worse because it is silent and reliable;
    // advancing announces the clearing and can never hand out a stamped value again.
    //
    // So: the bound handle is null rather than "whatever was bound", the window is empty
    // rather than 32 stale entries, the fetch shift is 0 rather than the last draw's - and the
    // two serials have MOVED FORWARD. The applier's OBJECT records are a different scope
    // entirely and are deliberately not touched here; ResourceEmit's
    // TheObjectRecordsSurviveAMakeCurrentAndOnlyTheWorkingStateIsReset is where that is driven
    // with live records in the table.
    TEST(VertexInputEmit, AResetApplierCarriesNoVertexInputStateOver) {
#if !MOBILEGL_PIPE_PUSH
        GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
        MGPipeApplierState& applier = MGPipeApplier();
        applier.BoundVertexElements = MGPipeHandle{7, 3};
        applier.VertexBufferStart = 1;
        applier.VertexBufferCount = 5;
        applier.VertexFetchBaseInstance = 9;
        applier.VertexBuffersSerial = 42;
        applier.IndexBufferSerial = 43;
        applier.MapPersistentRoundtrips = 44;

        MGPipeApplierReset();

        EXPECT_TRUE(MGPipeHandleIsNull(MGPipeApplier().BoundVertexElements));
        EXPECT_EQ(MGPipeApplier().VertexBufferStart, 0u);
        EXPECT_EQ(MGPipeApplier().VertexBufferCount, 0u);
        EXPECT_EQ(MGPipeApplier().VertexFetchBaseInstance, 0u);
        EXPECT_EQ(MGPipeApplier().MapPersistentRoundtrips, 0u);
        // MOVED FORWARD, not zeroed. 43 and 44 are the successors of the 42 and 43 above, and
        // the property that matters is the strict inequality: no value this counter has
        // already handed to a twin may ever come back.
        EXPECT_EQ(MGPipeApplier().VertexBuffersSerial, 43u);
        EXPECT_EQ(MGPipeApplier().IndexBufferSerial, 44u);
        EXPECT_GT(MGPipeApplier().VertexBuffersSerial, 42u);
        EXPECT_GT(MGPipeApplier().IndexBufferSerial, 43u);
#endif
    }

#if !MOBILEGL_PIPE_PUSH
    // G2 requires the pull and push ctest name sets to be identical, name for name, so a
    // push-only case is present and SKIPS rather than being absent.
#define MGL_VERTEX_INPUT_EMIT_TEST_LIST(X)                                                         \
    X(VertexInputEmit, EveryAttributeFieldSurvivesTheWireConversion)                                \
    X(VertexInputEmit, ABindingModelStrideOfZeroSurvivesAsZero)                                     \
    X(VertexInputEmit, IsLongAndFloat64TravelSeparately)                                            \
    X(VertexInputEmit, ABaseInstanceChangeAloneStillEmitsTheVertexBufferSet)                        \
    X(VertexInputEmit, AnUnchangedSetWithAnUnchangedBaseInstanceEmitsNothing)                       \
    X(VertexInputEmit, RebindingTheSameVaoEmitsABindAndNoCreate)                                    \
    X(VertexInputEmit, PingPongingBetweenTwoVaosNeverRecreatesEither)

#define MGL_DECLARE_PULL_SKIP(Suite, Name)                                                         \
    TEST(Suite, Name) { GTEST_SKIP() << "compiled only under MOBILEGL_PIPE_PUSH"; }
    MGL_VERTEX_INPUT_EMIT_TEST_LIST(MGL_DECLARE_PULL_SKIP)
#undef MGL_DECLARE_PULL_SKIP
#else
    using GLContext = MG_State::GLState::GLContext;
    using MG_State::GLState::BufferObject;
    using MG_State::GLState::VertexArrayObject;

    // The emitters are driven DIRECTLY rather than through MGPipeValidateForVerb, and that
    // is the point: G6 is a statement about the conversion, and a case that went through the
    // validate point would also be testing the tracker's shutters, which have their own
    // suite. What is asserted is what the emitter handed the applier - on this tree the
    // applier's entry points are stubs, so the emitter's own staging buffers ARE the
    // emitted record, at no copy.
    //
    // AN RAII SCOPE RATHER THAN A gtest FIXTURE: both gates grep `ctest -R
    // 'VertexInputEmit\.'`, a TEST_F files its cases under the FIXTURE's name, and gtest
    // refuses to mix TEST and TEST_F under one suite name - so a fixture would rename every
    // case out of the gate's reach.
    struct EmitterScope {
        EmitterScope() {
            m_previousContext = Move(MG_State::pGLContext);
            MG_State::pGLContext = MakeUnique<GLContext>();
            MGPipeVertexInputEmitterInstance().Reset();
            MGPipeVertexInputEmitterInstance().ResetCounters();
            MGPipeSetHashSuppressorInstance().InvalidateAll();
        }
        ~EmitterScope() {
            MG_State::pGLContext.reset();
            MG_State::pGLContext = Move(m_previousContext);
            MGPipeVertexInputEmitterInstance().Reset();
            MGPipeVertexInputEmitterInstance().ResetCounters();
            MGPipeSetHashSuppressorInstance().InvalidateAll();
        }
        EmitterScope(const EmitterScope&) = delete;
        EmitterScope& operator=(const EmitterScope&) = delete;

        UniquePtr<GLContext> m_previousContext;
    };

    GLContext& Ctx() { return *MG_State::pGLContext; }
    MGPipeVertexInputEmitter& Emitter() { return MGPipeVertexInputEmitterInstance(); }

    const SharedPtr<VertexArrayObject>& MakeVao(Uint name) {
        Ctx().CreateVertexArrayObject(name);
        Ctx().BindVertexArray(name);
        return Ctx().GetBoundVertexArray();
    }

    // ============================ G6 ============================
    //
    // "For every VAO configuration the emitted MGPVertexElements blob reproduces EXACTLY the
    // values the backend's VAO twin reads from the frontend today, field by field, for all 32
    // attribute slots."
    //
    // The oracle is the frontend attribute itself, read back through the same getter the twin
    // uses, so this cannot drift into asserting what the emitter happens to do. Every field is
    // its own EXPECT naming that field, which is what G7's scripted control needs: it stops
    // the conversion copying ONE member and expects this case to go red NAMING it.
    //
    // All three configuration families are driven, because they resolve differently and a
    // conversion that works for one is not evidence about the others: the legacy pointer
    // entry points (which resolve a 0 stride to the element size before it ever reaches the
    // wire), the ARB_vertex_attrib_binding entry points (where a 0 stride means the opposite
    // and must survive), and the enable/disable switch.
    TEST(VertexInputEmit, EveryAttributeFieldSurvivesTheWireConversion) {
        EmitterScope scope;
        const SharedPtr<VertexArrayObject> vao = MakeVao(1);
        const SharedPtr<BufferObject> buffer = Ctx().CreateBufferObject(1);
        buffer->Respecify(4096, nullptr);

        constexpr int kAttribs = VertexArrayObject::MAX_VERTEX_ATTRIBS;
        const DataType kTypes[] = {DataType::Float32, DataType::Int16,   DataType::Uint8,
                                   DataType::Int32,   DataType::Float64, DataType::Uint2101010Rev};
        for (int i = 0; i < kAttribs; ++i) {
            const auto index = static_cast<Uint>(i);
            const DataType type = kTypes[i % 6];
            const int size = 1 + (i % 4);
            const Bool normalized = (i % 3) == 0;
            const Bool isInteger = (i % 5) == 0;
            if (i < 12) {
                // The legacy pointer family: a raw stride, an effective stride and a pointer
                // offset, all three distinct so a conversion that took the wrong one fails.
                vao->SetAttributeFormat(index, size, type, normalized, 16 + i, static_cast<SizeT>(64 + i * 4),
                                        isInteger, false, 32 + i);
                vao->MirrorPointerIntoBinding(index, buffer, static_cast<SizeT>(64 + i * 4), 32 + i);
                vao->BindAttributeBuffer(index, buffer);
                vao->SetAttributeDivisor(index, static_cast<Uint>(i % 3));
            } else if (i < 24) {
                // The binding-model family, with the attribute deliberately fed by a DIFFERENT
                // binding index than its own - which is the one thing MGPVertexAttribWire::
                // BindingIndex exists to carry and the one an identity mapping would hide.
                const Uint binding = static_cast<Uint>((i + 5) % kAttribs);
                vao->SetAttributeFormatSeparate(index, size, type, normalized, isInteger,
                                                static_cast<Uint>(8 * (i % 4)), false, type == DataType::Float64);
                vao->SetAttributeBinding(index, binding);
                vao->SetBindingBuffer(binding, buffer, static_cast<SizeT>(128 + i), 48 + i);
                vao->SetBindingDivisor(binding, static_cast<Uint>(i % 2));
            } else {
                // GL_BGRA keeps size 4 and is its own flag; the disabled tail proves Enabled
                // travels rather than being implied by "has a format".
                vao->SetAttributeFormat(index, 4, DataType::Uint8, true, 0, static_cast<SizeT>(i), false, true, -1);
            }
            if ((i % 2) == 0) {
                vao->EnableAttribute(index);
            } else {
                vao->DisableAttribute(index);
            }
        }

        ASSERT_GT(Emitter().EmitVertexElements(Ctx()), 0u) << "a fresh VAO must publish a create";
        ASSERT_EQ(Emitter().CreateCount(), 1u);
        EXPECT_EQ(Emitter().LastElements().AttributeCount, static_cast<Uint32>(kAttribs));
        EXPECT_EQ(Emitter().LastElements().BindingPointCount,
                  static_cast<Uint32>(VertexArrayObject::MAX_VERTEX_ATTRIB_BINDINGS));
        EXPECT_EQ(Emitter().LastElements().Blob.Size,
                  static_cast<Uint64>(kAttribs) * sizeof(MGPVertexAttribWire) +
                      static_cast<Uint64>(VertexArrayObject::MAX_VERTEX_ATTRIB_BINDINGS) *
                          sizeof(MGPVertexBindingPointWire))
            << "the declared counts must describe the blob's declared size, or the applier refuses it";

        for (int i = 0; i < kAttribs; ++i) {
            const auto index = static_cast<Uint>(i);
            const auto& attrib = vao->GetAttribute(index);
            const MGPVertexAttribWire& wire = Emitter().LastAttributes()[static_cast<SizeT>(i)];
            SCOPED_TRACE(::testing::Message() << "attribute " << i);
            EXPECT_EQ(wire.Offset, static_cast<Uint64>(attrib.Offset));
            EXPECT_EQ(wire.Stride, static_cast<Int32>(attrib.Stride));
            EXPECT_EQ(wire.Type, static_cast<Uint32>(attrib.Type));
            EXPECT_EQ(wire.Size, static_cast<Uint8>(attrib.Size));
            EXPECT_EQ(wire.Enabled, attrib.Enabled ? 1 : 0);
            EXPECT_EQ(wire.Normalized, attrib.Normalized ? 1 : 0);
            EXPECT_EQ(wire.IsInteger, attrib.IsInteger ? 1 : 0);
            EXPECT_EQ(wire.IsLong, attrib.IsLong ? 1 : 0);
            EXPECT_EQ(wire.IsBgra, attrib.IsBgra ? 1 : 0);
            EXPECT_EQ(wire.BindingIndex, static_cast<Uint8>(vao->GetAttributeBindingIndex(index)));
            EXPECT_EQ(wire.Pad0, 0u) << "padding must stay padding";
        }

        for (int b = 0; b < VertexArrayObject::MAX_VERTEX_ATTRIB_BINDINGS; ++b) {
            const auto& point = vao->GetBindingPoint(static_cast<Uint>(b));
            const MGPVertexBindingPointWire& wire = Emitter().LastBindingPoints()[static_cast<SizeT>(b)];
            SCOPED_TRACE(::testing::Message() << "binding point " << b);
            EXPECT_EQ(wire.Offset, static_cast<Uint64>(point.Offset));
            EXPECT_EQ(wire.Stride, static_cast<Int32>(point.Stride));
            EXPECT_EQ(wire.Divisor, static_cast<Uint32>(point.Divisor));
        }

        // The divisor is NOT in the attribute view - it is resolved per binding point and
        // travels in MGPVertexBuffer::Divisor, which is where the backend reads it. Asserted
        // here rather than left to a reader of the struct, because carrying it twice is
        // exactly how a malformed record comes to disagree with itself.
        Emitter().EmitVertexBuffers(Ctx(), 0);
        for (Uint32 i = 0; i < Emitter().LastVertexBuffers().Count; ++i) {
            const auto& attrib = vao->GetAttribute(i);
            const MGPVertexBuffer& entry = Emitter().LastEntries()[i];
            SCOPED_TRACE(::testing::Message() << "vertex buffer entry " << i);
            EXPECT_EQ(entry.Divisor, static_cast<Uint32>(attrib.Divisor));
            EXPECT_EQ(entry.Stride, static_cast<Uint32>(attrib.Stride));
            EXPECT_EQ(entry.BindingIndex, i);
            EXPECT_EQ(entry.Offset, 0u) << "the attribute's own byte offset lives in the wire attribute";
        }
    }

    // KHR-GL43.vertex_attrib_binding.basic-input-case7/8: a pointer call's stride 0 means
    // "tightly packed" and the frontend already resolved it to the element size, so a zero
    // that reaches the wire can only have come from the binding model - where it means every
    // vertex reads the SAME element and the fetch address never advances. Collapsing it back
    // into the element size is what made those two cases read past the buffer.
    TEST(VertexInputEmit, ABindingModelStrideOfZeroSurvivesAsZero) {
        EmitterScope scope;
        const SharedPtr<VertexArrayObject> vao = MakeVao(1);
        const SharedPtr<BufferObject> buffer = Ctx().CreateBufferObject(1);
        buffer->Respecify(256, nullptr);

        vao->SetAttributeFormatSeparate(0, 4, DataType::Float32, false, false, 0);
        vao->SetAttributeBinding(0, 0);
        vao->SetBindingBuffer(0, buffer, 0, 0); // the binding model's zero
        vao->EnableAttribute(0);

        // The control, on the SAME emission: a pointer-style zero was already resolved to the
        // tightly packed element size by the GL entry point (which is what the effective
        // stride argument carries), so it must NOT reach the wire as a zero. The raw argument
        // stays 0 and is reported verbatim by glGetVertexAttribiv - which is exactly why the
        // two are stored apart and only the resolved one travels.
        vao->SetAttributeFormat(1, 4, DataType::Float32, false, 0, 0, false, false, 16);
        vao->MirrorPointerIntoBinding(1, buffer, 0, 16);
        vao->BindAttributeBuffer(1, buffer);
        vao->EnableAttribute(1);

        ASSERT_GT(Emitter().EmitVertexElements(Ctx()), 0u);
        ASSERT_EQ(vao->GetAttribute(0).Stride, 0) << "the frontend itself no longer resolves this to zero";
        EXPECT_EQ(Emitter().LastAttributes()[0].Stride, 0)
            << "a binding-model stride of 0 was collapsed into the element size";
        EXPECT_EQ(Emitter().LastBindingPoints()[0].Stride, 0);
        EXPECT_NE(Emitter().LastAttributes()[1].Stride, 0) << "a resolved pointer stride reached the wire as 0";
        EXPECT_EQ(Emitter().LastAttributes()[1].Stride, static_cast<Int32>(vao->GetAttribute(1).Stride));
        EXPECT_EQ(vao->GetAttribute(1).LegacyStride, 0) << "the raw query answer is not the resolved one";

        Emitter().EmitVertexBuffers(Ctx(), 0);
        EXPECT_EQ(Emitter().LastEntries()[0].Stride, 0u) << "and the set has to agree with the format";
    }

    // VertexAttribFormat(GL_DOUBLE) reads doubles from memory and asks for them CONVERTED to
    // float; VertexAttribLFormat keeps all 64 bits. The backend's fp64 narrowing and its
    // Adreno disabled-attribute workaround both key on telling the two apart, so IsLong may
    // never be inferred from Type == Float64.
    TEST(VertexInputEmit, IsLongAndFloat64TravelSeparately) {
        EmitterScope scope;
        const SharedPtr<VertexArrayObject> vao = MakeVao(1);
        // Attribute 0: GL_DOUBLE, converted to float. Attribute 1: the same type, kept long.
        vao->SetAttributeFormatSeparate(0, 4, DataType::Float64, false, false, 0, false, false);
        vao->SetAttributeFormatSeparate(1, 4, DataType::Float64, false, false, 0, false, true);
        // Attribute 2: NOT a double, and not long either - so "IsLong implies Float64" is
        // asserted in both directions.
        vao->SetAttributeFormatSeparate(2, 4, DataType::Float32, false, false, 0, false, false);
        vao->EnableAttribute(0);
        vao->EnableAttribute(1);
        vao->EnableAttribute(2);

        ASSERT_GT(Emitter().EmitVertexElements(Ctx()), 0u);
        const auto& wires = Emitter().LastAttributes();
        EXPECT_EQ(wires[0].Type, static_cast<Uint32>(DataType::Float64));
        EXPECT_EQ(wires[0].IsLong, 0) << "a converted double must not travel as long";
        EXPECT_EQ(wires[1].Type, static_cast<Uint32>(DataType::Float64));
        EXPECT_EQ(wires[1].IsLong, 1) << "an L-format double lost its long flag";
        EXPECT_EQ(wires[2].Type, static_cast<Uint32>(DataType::Float32));
        EXPECT_EQ(wires[2].IsLong, 0);
        // And the frontend agrees, so this is not the emitter asserting its own answer.
        EXPECT_EQ(vao->GetAttribute(0).IsLong, false);
        EXPECT_EQ(vao->GetAttribute(1).IsLong, true);
    }

    // D-H2.3, THE SUPPRESSOR TRAP. set_vertex_buffers is suppressed on an unchanged content
    // hash. The base instance is DRAW state and moves without the buffer set moving, so a
    // hash that did not include it would suppress the one record whose changed field is the
    // fetch shift, and the server would keep the previous one - silently wrong geometry on
    // instanced draws, and no desktop SSIM case need exercise it.
    TEST(VertexInputEmit, ABaseInstanceChangeAloneStillEmitsTheVertexBufferSet) {
        EmitterScope scope;
        const SharedPtr<VertexArrayObject> vao = MakeVao(1);
        const SharedPtr<BufferObject> buffer = Ctx().CreateBufferObject(1);
        buffer->Respecify(256, nullptr);
        vao->SetAttributeFormat(0, 4, DataType::Float32, false, 16, 0, false);
        vao->BindAttributeBuffer(0, buffer);
        vao->SetAttributeDivisor(0, 1);
        vao->EnableAttribute(0);

        ASSERT_GT(Emitter().EmitVertexBuffers(Ctx(), 0), 0u) << "the first set always goes out";
        ASSERT_EQ(Emitter().VertexBufferSetCount(), 1u);
        const Uint64 firstHash = Emitter().LastVertexBuffers().ContentHash;
        EXPECT_EQ(Emitter().LastVertexBuffers().BaseInstance, 0u);

        // NOTHING about the buffer set changed; only the draw's base instance.
        EXPECT_GT(Emitter().EmitVertexBuffers(Ctx(), 7), 0u)
            << "a base-instance-only change was suppressed - it is not in the content hash";
        EXPECT_EQ(Emitter().VertexBufferSetCount(), 2u);
        EXPECT_EQ(Emitter().LastVertexBuffers().BaseInstance, 7u)
            << "the RAW value the draw carried, never a pre-shifted offset";
        EXPECT_NE(Emitter().LastVertexBuffers().ContentHash, firstHash);

        // And back to zero is a change too - which is what makes a plain draw after a
        // base-instanced one undo the shift.
        EXPECT_GT(Emitter().EmitVertexBuffers(Ctx(), 0), 0u);
        EXPECT_EQ(Emitter().LastVertexBuffers().BaseInstance, 0u);
        EXPECT_EQ(Emitter().LastVertexBuffers().ContentHash, firstHash)
            << "the hash is a function of the set and the base instance, so it has to come back";
    }

    // The counterpart, and the reason the suppressor exists at all: an unchanged set with an
    // unchanged base instance is not a record worth sending, and the slot must say so.
    TEST(VertexInputEmit, AnUnchangedSetWithAnUnchangedBaseInstanceEmitsNothing) {
        EmitterScope scope;
        const SharedPtr<VertexArrayObject> vao = MakeVao(1);
        const SharedPtr<BufferObject> buffer = Ctx().CreateBufferObject(1);
        buffer->Respecify(256, nullptr);
        vao->SetAttributeFormat(0, 4, DataType::Float32, false, 16, 0, false);
        vao->BindAttributeBuffer(0, buffer);
        vao->EnableAttribute(0);

        ASSERT_GT(Emitter().EmitVertexBuffers(Ctx(), 3), 0u);
        ASSERT_EQ(Emitter().VertexBufferSetCount(), 1u);
        const Uint64 latched =
            MGPipeSetHashSuppressorInstance().LastEmitted(MGPipeSuppressorSlot::SetVertexBuffers);
        EXPECT_NE(latched, 0u) << "0 is reserved for 'never emitted'";

        EXPECT_EQ(Emitter().EmitVertexBuffers(Ctx(), 3), 0u) << "an unchanged set went out again";
        EXPECT_EQ(Emitter().VertexBufferSetCount(), 1u);
        EXPECT_EQ(MGPipeSetHashSuppressorInstance().LastEmitted(MGPipeSuppressorSlot::SetVertexBuffers),
                  latched);

        // A real change to the SET still goes out with the same base instance, so the
        // suppression above is not simply "this slot is stuck".
        vao->SetAttributeDivisor(0, 4);
        EXPECT_GT(Emitter().EmitVertexBuffers(Ctx(), 3), 0u);
        EXPECT_EQ(Emitter().VertexBufferSetCount(), 2u);
    }

    // D-G3's per-handle latch. create_vertex_elements is re-issued on the SAME handle when a
    // configuration moves, and the latch is stored per handle rather than globally so that
    // rebinding cannot look like a configuration change.
    TEST(VertexInputEmit, RebindingTheSameVaoEmitsABindAndNoCreate) {
        EmitterScope scope;
        const SharedPtr<VertexArrayObject> a = MakeVao(1);
        a->SetAttributeFormat(0, 4, DataType::Float32, false, 16, 0, false);
        a->EnableAttribute(0);

        ASSERT_GT(Emitter().EmitVertexElements(Ctx()), 0u);
        EXPECT_EQ(Emitter().CreateCount(), 1u);
        EXPECT_EQ(Emitter().BindCount(), 1u);

        // Same VAO, same configuration: nothing at all.
        EXPECT_EQ(Emitter().EmitVertexElements(Ctx()), 0u);
        EXPECT_EQ(Emitter().CreateCount(), 1u);
        EXPECT_EQ(Emitter().BindCount(), 1u);

        // Away and back. The bind is re-emitted because the server's bound handle moved; the
        // create is not, because this handle already published this configuration.
        MakeVao(2);
        ASSERT_GT(Emitter().EmitVertexElements(Ctx()), 0u);
        EXPECT_EQ(Emitter().CreateCount(), 2u);
        EXPECT_EQ(Emitter().BindCount(), 2u);

        Ctx().BindVertexArray(1);
        EXPECT_GT(Emitter().EmitVertexElements(Ctx()), 0u);
        EXPECT_EQ(Emitter().CreateCount(), 2u) << "a rebind re-created a configuration that had not moved";
        EXPECT_EQ(Emitter().BindCount(), 3u);

        // A configuration change on the BOUND VAO re-creates on the same handle and does NOT
        // rebind: the server's bound handle did not move.
        const MGPipeHandle bound = Emitter().BoundHandle();
        a->SetAttributeFormat(1, 2, DataType::Int16, true, 8, 4, true);
        a->EnableAttribute(1);
        EXPECT_GT(Emitter().EmitVertexElements(Ctx()), 0u);
        EXPECT_EQ(Emitter().CreateCount(), 3u);
        EXPECT_EQ(Emitter().BindCount(), 3u) << "a re-create must not rebind";
        EXPECT_EQ(Emitter().LastElements().Cso, bound) << "and it must land on the SAME handle";
    }

    // The latch is per handle, so alternating between two VAOs re-binds and never re-creates.
    // A global latch would re-create both on every swap - strictly more work than the tree
    // does today, which is the trade D-G1's identity-addressed CSO exists to avoid.
    TEST(VertexInputEmit, PingPongingBetweenTwoVaosNeverRecreatesEither) {
        EmitterScope scope;
        const SharedPtr<VertexArrayObject> a = MakeVao(1);
        a->SetAttributeFormat(0, 4, DataType::Float32, false, 16, 0, false);
        a->EnableAttribute(0);
        ASSERT_GT(Emitter().EmitVertexElements(Ctx()), 0u);

        const SharedPtr<VertexArrayObject> b = MakeVao(2);
        b->SetAttributeFormat(0, 2, DataType::Int16, true, 8, 4, true);
        b->EnableAttribute(0);
        ASSERT_GT(Emitter().EmitVertexElements(Ctx()), 0u);
        ASSERT_EQ(Emitter().CreateCount(), 2u);
        const MGPipeHandle handleB = Emitter().BoundHandle();

        for (int i = 0; i < 8; ++i) {
            Ctx().BindVertexArray(1);
            EXPECT_GT(Emitter().EmitVertexElements(Ctx()), 0u);
            Ctx().BindVertexArray(2);
            EXPECT_GT(Emitter().EmitVertexElements(Ctx()), 0u);
        }
        EXPECT_EQ(Emitter().CreateCount(), 2u) << "ping-ponging re-created a VAO's configuration";
        EXPECT_EQ(Emitter().BindCount(), 2u + 16u);
        EXPECT_EQ(Emitter().BoundHandle(), handleB) << "the two VAOs swapped handles";

        // Unbinding entirely publishes the null handle, once.
        Ctx().BindVertexArray(0);
        EXPECT_GT(Emitter().EmitVertexElements(Ctx()), 0u) << "the default VAO is a VAO and has a handle";
        EXPECT_EQ(Emitter().CreateCount(), 3u);
    }
#endif // MOBILEGL_PIPE_PUSH
} // namespace

int main(int argc, char** argv) {
    // Before anything logs: the logger reads this variable once, on its first write, and
    // caches the handle. The name carries this process's pid, and the file is removed on the
    // way out.
    namespace fs = std::filesystem;
    const fs::path path =
        fs::temp_directory_path() / ("mobilegl-vertexinputemit-test-" + std::to_string(ProcessId()) + ".log");
    std::error_code ec;
    fs::remove(path, ec);
    g_logPath = path.string();
#if defined(_WIN32)
    _putenv_s("MOBILEGL_LOG_FILE_PATH", g_logPath.c_str());
#else
    setenv("MOBILEGL_LOG_FILE_PATH", g_logPath.c_str(), 1);
#endif
    ::testing::InitGoogleTest(&argc, argv);
    const int rc = RUN_ALL_TESTS();
    fs::remove(path, ec);
    return rc;
}
