// MobileGL - MobileGL/MG_State/GLState/ProgramState/ProgramObject.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ProgramObject.h"
#include "ProgramLinkTask.h"
#include <atomic>
#include <MG_Util/Async/ShaderCompilePool.h>
#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>
#include <MG_Util/ShaderTranspiler/CompileEnv.h>

const char* kDefaultFragmentShaderSource = R"(#version 460 core
layout(location = 0) out vec4 FragColor;
void main() {}
)";


namespace MobileGL::MG_State::GLState {
    static std::atomic<Uint64> s_nextProgramLifetimeId = 1;

    Uint64 ProgramObject::AllocateLifetimeId() {
        return s_nextProgramLifetimeId.fetch_add(1, std::memory_order_relaxed);
    }

    ProgramObject::~ProgramObject() { CancelLink(); }

    // EnsureLinkJoined() is defined inline in ProgramObject.h (see the comment there for
    // why: ~1200 call sites, no LTO). Only its blocking half lives here.

    void ProgramObject::JoinPendingLink() const {
        MOBILEGL_ASSERT(!MG_Util::Async::ShaderCompilePool::IsPoolThread(),
                        "ProgramObject::EnsureLinkJoined() reached from a pool thread; a job body must never read "
                        "GL-thread-owned objects");

        // Move the node out FIRST. The publish below runs GL-thread-only code that reads
        // link output through Artifacts() (ApplyDeferredDiagnostics can reach
        // pGLContext->RecordError, and a future reader might not be so careful), and with
        // m_pendingLink still set that would re-enter this function.
        const SharedPtr<ProgramLinkTask> pending = Move(m_pendingLink);
        m_pendingLink.reset();

        pending->Wait();
        if (pending->IsComplete()) {
            // ONE move, not thirty cross-thread field assignments: the artifacts block is
            // exactly what a link produces, so moving it IS the publish.
            m_artifacts = Move(pending->artifacts);
            // The second bump. The first one happened at ENQUEUE so every backend memo read
            // "stale" for the whole pending window; this one invalidates anything a backend
            // may have cached DURING that window, when m_artifacts still held the previous
            // link's output. Without it a memo taken mid-window would survive the publish
            // and describe a program that no longer exists.
            BumpLinkObservableVersions();
        }
        // A node that settled as Cancelled published nothing, and m_artifacts still holds
        // what Link()'s prologue left there: cleared, LINK_STATUS false, no info log. That is
        // the correct answer for a link that was superseded or abandoned, and it is why no
        // caller of CancelLink() has to repair anything afterwards.

        // Worker-side log lines and any deferred GL error are raised HERE, on the GL thread,
        // at the first join of the job that produced them - which is where a serial
        // implementation would have produced them.
        MG_Util::Async::ApplyDeferredDiagnostics(*pending);
    }

    Bool ProgramObject::IsPendingLinkTerminal() const { return m_pendingLink->IsTerminal(); }

    void ProgramObject::CancelLink() {
        if (!m_pendingLink) return;
        // Cooperative and non-blocking. A node that no worker has picked up settles
        // immediately; one that is running is flagged and settles when its body returns,
        // writing only into itself the whole time. Either way nothing waits, and the node
        // keeps its own inputs alive for as long as it needs them.
        m_pendingLink->Cancel();
        m_pendingLink.reset();
    }

    void ProgramObject::BumpLinkObservableVersions() const {
        // Relinking regenerates the SPIR-V, so any backend-cached state keyed on
        // m_backendStateVersion (e.g. the content-hash memo) must be invalidated,
        // along with every link-derived backend cache (m_linkVersion) and the
        // last-uploaded-UBO gate (a relink resets uniforms to their initial values,
        // and that reset must reach the GPU). GL-THREAD ONLY: bumped once per link
        // in Link()'s prologue and at the publish, and by glProgramBinary's mandated
        // failure - never from the link body, which runs on a pool worker: a
        // non-atomic ++ there against the draw path's reads would be exactly the
        // lost-invalidation memo hazard.
        ++m_backendStateVersion;
        ++m_linkVersion;
        MarkUBOContentDirty();
    }

    void ProgramObject::ResetLinkArtifacts(LinkArtifacts& artifacts) {
        // Worker-safe pure clear: touches LinkArtifacts only, which is why the link body can
        // call it on its own block. The link-observable version bumps live in
        // BumpLinkObservableVersions() on the GL thread.

        // Deliberately NOT `artifacts = {}`: infoLog, linkedFragDataLocation/Index and the
        // geometry strip-capture pair live in LinkArtifacts but are not part of what this
        // function has ever cleared, and its callers depend on that (they write infoLog
        // immediately AFTER calling here). Link()'s prologue does not use this - it assigns a
        // whole default-constructed block, where the ordering is explicit.
        artifacts.program.reset();
        artifacts.generatedSpirv.clear();
        artifacts.uniformLocations.clear();
        artifacts.glUniformIndexToTProgram.clear();
        artifacts.tProgramUniformIndexToGl.clear();
        artifacts.glBlockIndexToTProgram.clear();
        artifacts.tProgramBlockIndexToGl.clear();
        artifacts.linkedExplicitUniformLocations.clear();
        artifacts.uniformIndexInTProgram.clear();
        artifacts.uniformSamplerOrImageUnitIndex.clear();
        artifacts.explicitOpaqueUniformBindings.clear();
        artifacts.uniformBlockIndexByName.clear();
        artifacts.uniformBlockBinding.clear();
        artifacts.shaderStorageBlockBinding.clear();
        artifacts.uniformOffsets.clear();
        artifacts.uniformSizesInBytes.clear();
        artifacts.globalUboScratch.clear();
        artifacts.attribs.clear();
        artifacts.attribTypes.clear();
        artifacts.activeUniformCount = 0;
        artifacts.maxUniformLocation = 0;
        artifacts.uniformNameMaxLength = 0;
        artifacts.attribInNameMaxLength = 0;
        artifacts.uniformBlockNameMaxLength = 0;
        artifacts.xfbVaryings.clear();
        artifacts.xfbInterfaceNames.clear();
        artifacts.xfbStrides.clear();
        artifacts.xfbBufferMode = GL_INTERLEAVED_ATTRIBS;
        artifacts.xfbVaryingNameMaxLength = 0;
        artifacts.xfbNeedsScatteredCapture = false;
        artifacts.xfbPackedStride = 0;
        artifacts.gsInputPrimitive = GL_NONE;
        artifacts.linkStatus = false;
    }




    bool ProgramObject::ShaderIsAttached(const SharedPtr<ShaderObject>& shader) {
        MGLOG_D("ProgramObject %u: ShaderIsAttached check for shader %p", m_externalIndex, shader.get());
        auto it = std::find_if(m_shaders.begin(), m_shaders.end(),
                               [shader](const SharedPtr<ShaderObject>& s) { return s.get() == shader.get(); });
        bool attached = it != m_shaders.end();
        MGLOG_D("ProgramObject %u: ShaderIsAttached -> %s", m_externalIndex, attached ? "true" : "false");
        return attached;
    }

    // NO CancelLink here, nor in DetachShader below. Both only edit the attach lists, which
    // a pending link does not read - it snapshotted (stage, source, compile node) per shader
    // at enqueue and is isolated from every later mutation. GL agrees: attaching or detaching
    // takes effect at the NEXT link and leaves the current LINK_STATUS alone, so cancelling
    // would make `glLinkProgram; glAttachShader; glGetProgramiv(LINK_STATUS)` report FALSE
    // for a link that succeeded - and would break glCreateShaderProgramv outright, since that
    // is specified as link-then-detach and would discard its own link before anyone read it.
    bool ProgramObject::AttachShader(const SharedPtr<ShaderObject>& shader) {
        MGLOG_D("ProgramObject %u: AttachShader called for shader %p", m_externalIndex, shader.get());
        if (ShaderIsAttached(shader)) {
            MGLOG_D("ProgramObject %u: AttachShader - shader already attached, skipping", m_externalIndex);
            return false;
        }
        m_shaders.emplace_back(shader);
        MGLOG_D("ProgramObject %u: AttachShader - attached successfully, total shaders now %zu", m_externalIndex,
                m_shaders.size());
        return true;
    }

    SizeT ProgramObject::DetachShader(const SharedPtr<ShaderObject>& shader) {
        MGLOG_D("DetachShader called for shader %p from ProgramObject %u", shader.get(), m_externalIndex);
        if (!ShaderIsAttached(shader)) {
            MGLOG_D("Shader %p is not attached to ProgramObject %u, cannot detach.", shader.get(), m_externalIndex);
            return 0;
        }
        m_detachedShaders.push_back(shader);
        MGLOG_D("Shader %p marked for detachment from ProgramObject %u", shader.get(), m_externalIndex);
        return 1;
    }

    SizeT ProgramObject::RemoveShader(const SharedPtr<ShaderObject>& shader) {
        MGLOG_D("ProgramObject %u: RemoveShader called for shader %p", m_externalIndex, shader.get());
        auto count =
            std::erase_if(m_shaders, [shader](const SharedPtr<ShaderObject>& s) { return s.get() == shader.get(); });

        MGLOG_D("ProgramObject %u: RemoveShader - removed %zu shader(s), remaining %zu", m_externalIndex, count,
                m_shaders.size());
        return count;
    }

    void ProgramObject::AddDefaultFragmentShaderIfMissing() {
        Bool needsDefaultFS = false;
        for (const auto& shader : m_shaders) {
            auto stage = shader->GetShaderStage();
            if (stage == ShaderStage::Vertex) {
                needsDefaultFS = true;
                continue;
            }
            if (stage == ShaderStage::Fragment) {
                needsDefaultFS = false;
                return;
            }
        }

        if (!needsDefaultFS) return;

        MGLOG_D("ProgramObject %u: No fragment shader attached, adding default fragment shader.", m_externalIndex);
        SharedPtr<ShaderObject> defaultFS = MakeShared<ShaderObject>(ShaderStage::Fragment, 0);
        defaultFS->SetShaderSource(kDefaultFragmentShaderSource);
        defaultFS->Compile(); // TODO: use a global default FS object.
        auto status = defaultFS->GetCompileStatus();
        if (!status) {
            MGLOG_E("ProgramObject %u: Failed to compile default fragment shader. InfoLog:\n%s", m_externalIndex,
                    defaultFS->GetInfoLog().c_str());
            return;
        }
        m_shaders.push_back(defaultFS);
        MGLOG_D("ProgramObject %u: Default fragment shader added.", m_externalIndex);
    }

    void ProgramObject::Link(Bool addDefaultFSIfMissingForRenderingPipelineProgram) {
        MGLOG_D("ProgramObject %u: Link start, shaders to link: %zu", m_externalIndex, m_shaders.size());
        // The last link wins. A link still in flight is computing an answer this call is
        // about to replace, and nothing has observed it yet (an observation would have
        // joined), so it is dropped where it stands - no wait.
        CancelLink();

        // Bumped at ENQUEUE, not at publish, and that ordering is the whole invalidation
        // story: from this instant every backend memo keyed on m_backendStateVersion /
        // m_linkVersion reads "stale", so nothing can keep using the PREVIOUS link's
        // reflection while the new one is still being computed. (The publish bumps a second
        // time, for anything cached during the pending window itself.)
        ++m_backendStateVersion;
        BumpLinkObservableVersions();
        // A whole-struct reset, unlike ResetLinkArtifacts(): during the pending window this
        // is what every gated reader sees, so it has to be the complete "not linked" state -
        // including the fields ResetLinkArtifacts deliberately preserves for its own callers.
        m_artifacts = {};

        // ---- GL-thread-owned mutations ----
        // Remove detached shaders first
        for (const auto& detachedShader : m_detachedShaders) {
            RemoveShader(detachedShader);
        }
        m_detachedShaders.clear();

        if (addDefaultFSIfMissingForRenderingPipelineProgram) {
            AddDefaultFragmentShaderIfMissing();
        }
        if (m_shaders.empty()) {
            m_artifacts.infoLog = "No shader objects are attached to program.";
            MGLOG_E("ProgramObject %u: Link failed - no shader objects attached.", m_externalIndex);
            return;
        }

        std::sort(m_shaders.begin(), m_shaders.end(),
                  [](const SharedPtr<ShaderObject>& a, const SharedPtr<ShaderObject>& b) {
                      return a->GetShaderStage() < b->GetShaderStage();
                  });

        // ---- end of the GL-thread prologue: everything below is the snapshot ----
        // Everything above mutates GL-thread-owned state (the attach lists, the version
        // counters, the default-FS fixup) and must stay on the calling thread. Everything
        // below is a pure function of what is copied into `in`, which is what lets the body
        // run on a worker. Nothing here reads compile OUTPUT - taking the nodes without
        // joining them is exactly what makes glLinkProgram not block on glCompileShader.
        auto task = MakeShared<ProgramLinkTask>();
        task->in.externalIndex = m_externalIndex;
        task->in.env = MG_Util::ShaderTranspiler::GetCurrentCompileEnv();
        task->in.explicitAttribLocations = m_explicitAttribLocations;
        task->in.explicitFragDataLocation = m_explicitFragDataLocation;
        task->in.explicitFragDataIndex = m_explicitFragDataIndex;
        task->in.requestedXfbVaryings = m_requestedXfbVaryings;
        task->in.requestedXfbBufferMode = m_requestedXfbBufferMode;
        task->in.maxFragmentOutputColorNumber = m_maxFragmentOutputColorNumber;

        Vector<SharedPtr<ShaderCompileTask>> deps;
        deps.reserve(m_shaders.size());
        task->in.shaders.reserve(m_shaders.size());
        for (const auto& shader : m_shaders) {
            const SharedPtr<ShaderCompileTask>& node = shader->CompiledNodeForLink();
            if (node) {
                // This link is now an observer of that node's result, and the ShaderObject is
                // no longer the only route to it: without the marker, the ordinary
                // link-then-detach-then-delete teardown would cancel a compile this link is
                // waiting on and turn a successful link into GL_FALSE.
                node->MarkLinkReferenced();
                if (!node->IsTerminal()) deps.push_back(node);
            }
            task->in.shaders.push_back({shader->GetShaderStage(), shader->GetShaderSourcePtr(), node});
        }

        m_pendingLink = task;

        // Flag off - or glMaxShaderCompilerThreadsKHR(0), see AsyncShaderCompileActive():
        // byte-identical to the synchronous implementation. RunInline() executes the same
        // body on this thread and the join below publishes through the same code, so the two
        // modes differ only in WHICH thread ran RunBody().
        if (!MG_Util::Async::AsyncShaderCompileActive()) {
            task->RunInline();
            EnsureLinkJoined();
            return;
        }
        task->SubmitAfter(deps);
    }


    void ProgramObject::MarkAsDeleted() {
        MGLOG_D("ProgramObject %u: MarkAsDeleted called (was %s)", m_externalIndex,
                m_deleteStatus ? "deleted" : "not deleted");
        m_deleteStatus = true;
        MGLOG_D("ProgramObject %u: MarkAsDeleted - now marked deleted", m_externalIndex);
    }

    Vector<SharedPtr<ShaderObject>>& ProgramObject::GetAttachedShaders() {
        MGLOG_D("ProgramObject %u: GetAttachedShaders called, returning %zu shaders", m_externalIndex,
                m_shaders.size());
        return m_shaders;
    }

    const Vector<SharedPtr<ShaderObject>>& ProgramObject::GetAttachedShaders() const {
        return m_shaders;
    }


    void ProgramObject::SetExplicitVertexInLocation(Uint index, const char* name) {
        MGLOG_D("ProgramObject %u: SetExplicitVertexInLocation called name='%s' index=%u", m_externalIndex, name,
                index);
        m_explicitAttribLocations[name] = index;
        MGLOG_D("ProgramObject %u: SetExplicitVertexInLocation - stored explicit location for '%s' -> %u",
                m_externalIndex, name, index);
    }

    void ProgramObject::SetExplicitFragmentOutLocation(Uint index, const char* name) {
        MGLOG_D("ProgramObject %u: SetExplicitFragmentOutLocation called name='%s' index=%u", m_externalIndex, name,
                index);
        m_explicitFragDataLocation[name] = index;
        MGLOG_D("ProgramObject %u: SetExplicitFragmentOutLocation - stored explicit location for '%s' -> %u",
                m_externalIndex, name, index);
    }

    void ProgramObject::SetExplicitFragmentOutIndex(Uint colorIndex, const char* name) {
        m_explicitFragDataIndex[name] = colorIndex;
        MGLOG_D("ProgramObject %u: SetExplicitFragmentOutIndex - stored color index for '%s' -> %u", m_externalIndex,
                name, colorIndex);
    }


    Int ProgramObject::GetFragmentDataLocation(const char* name) {
        if (!Artifacts().program || !name) return -1;

        const auto explicitLocation = Artifacts().linkedFragDataLocation.find(name);
        const Int outputCount = Artifacts().program->getNumPipeOutputs();
        for (Int index = 0; index < outputCount; ++index) {
            const auto& output = Artifacts().program->getPipeOutput(index);
            if (output.name != name) continue;
            if (explicitLocation != Artifacts().linkedFragDataLocation.end()) return static_cast<Int>(explicitLocation->second);
            return static_cast<Int>(output.layoutLocation());
        }
        return -1;
    }

    Int ProgramObject::GetFragmentDataIndex(const char* name) {
        // Only an active user-defined fragment output has an index; reuse the location lookup to test
        // that. The color index defaults to 0 unless glBindFragDataLocationIndexed bound it to 1.
        // (Shader-side layout(index = ...) qualifiers are not reflected here, only API bindings.)
        if (GetFragmentDataLocation(name) < 0) return -1;
        const auto it = Artifacts().linkedFragDataIndex.find(name);
        return it != Artifacts().linkedFragDataIndex.end() ? static_cast<Int>(it->second) : 0;
    }
} // namespace MobileGL::MG_State::GLState
