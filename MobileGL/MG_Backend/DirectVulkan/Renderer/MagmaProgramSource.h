// MobileGL - server-owned program input for Magma (P5f fm).
#pragma once

#include <MG_State/GLState/ProgramState/ProgramObject.h>

#if MOBILEGL_BUILD_DISAGGREGATED
#include <MG_Pipe/PipeApply.h>
#include <MG_State/GLState/ProgramState/ProgramArtifactsCodec.h>
#include "../DirectVulkanResourceState.h"
// P7 wave 2 package C, OQ-8: spirv_reflect.h is GONE from this header, and its absence is the
// gate. Everything this class needed it for - the storage-block index space - now travels in
// the archive (LinkArtifacts::storageBlocks), so the SPIR-V reflector is off the Magma wire
// draw path entirely. A future edit that re-adds the include is re-adding a per-draw reflect,
// because a MagmaProgramSource is constructed once per draw and can memoise nothing.
#endif

namespace MobileGL::MG_Backend::DirectVulkan {
#if MOBILEGL_BUILD_DISAGGREGATED
    // A borrowed view, valid only while its frontend object or server record lives.
    // It never constructs a ProgramObject on the server. Post-link mutable values
    // come from the record tails, not from the archive's link-time defaults.
    class MagmaProgramSource {
    public:
        using Program = MG_State::GLState::ProgramObject;
        MagmaProgramSource(const Program& program) : m_frontend(&program) {}
        MagmaProgramSource(MG_Pipe::MGPipeHandle handle, const MG_Pipe::MGPipeShaderCsoRecord& record)
            : m_handle(handle), m_record(&record) {
            MOBILEGL_ASSERT(record.Archive != nullptr, "Magma program record has no archive");
        }

        Bool IsWire() const { return m_record != nullptr; }
        MG_Pipe::MGPipeHandle Handle() const { return m_handle; }
        const Program* Frontend() const { return m_frontend; }
        Uint GetExternalIndex() const { return IsWire() ? m_handle.Slot : m_frontend->GetExternalIndex(); }
        Uint64 GetLifetimeId() const {
            return IsWire() ? (Uint64(m_handle.Gen) << 32) | m_handle.Slot : m_frontend->GetLifetimeId();
        }
        Bool GetLinkStatus() const { return IsWire() ? m_record->Desc.LinkStatus != 0 : m_frontend->GetLinkStatus(); }
        Bool GetSpirvStatus() const { return IsWire() ? m_record->Desc.SpirvStatus != 0 : m_frontend->GetSpirvStatus(); }
        Bool PointSizeDemoted() const { return IsWire() ? m_record->Desc.PointSizeDemoted != 0 : m_frontend->PointSizeDemoted(); }
        Bool GetSpirvValidationEnabled() const {
            return IsWire() ? m_record->Desc.EnableSpirvValidation != 0 : m_frontend->GetSpirvValidationEnabled();
        }
        const Vector<Vector<Uint>>& GetGeneratedSpirv() const {
            return IsWire() ? m_record->Archive->Spirv.generatedSpirv : m_frontend->GetGeneratedSpirv();
        }
        Vector<ShaderStage> GetLinkedShaderStages() const {
            if (!IsWire()) return m_frontend->GetLinkedShaderStages();
            Vector<ShaderStage> result;
            for (const auto stage : m_record->Archive->LinkedStages) result.push_back(static_cast<ShaderStage>(stage));
            return result;
        }
        Bool HasLinkedShaderStage(ShaderStage stage) const {
            if (!IsWire()) return m_frontend->HasLinkedShaderStage(stage);
            const auto& stages = m_record->Archive->LinkedStages;
            return std::find(stages.begin(), stages.end(), static_cast<Uint32>(stage)) != stages.end();
        }
        Bool GetBackendHashMemo(Uint flags, Uint64& hash) const {
            // A view has no lifetime in which a memo can safely persist. The server
            // factory's content cache still owns compiled programs; hash from current
            // archive + binding tails rather than borrowing the client's mutable memo.
            return !IsWire() && m_frontend->GetBackendHashMemo(flags, hash);
        }
        void SetBackendHashMemo(Uint flags, Uint64 hash) const {
            if (!IsWire()) m_frontend->SetBackendHashMemo(flags, hash);
        }
        Uint64 GetBackendStateVersion() const { return IsWire() ? m_record->Serial : m_frontend->GetBackendStateVersion(); }
        Uint64 GetBlockBindingVersion() const { return IsWire() ? m_record->BindingsSerial : m_frontend->GetBlockBindingVersion(); }
        Uint64 GetImageUnitVersion() const { return IsWire() ? m_record->BindingsSerial : m_frontend->GetImageUnitVersion(); }
        Uint64 GetLinkVersion() const { return IsWire() ? m_record->Serial : m_frontend->GetLinkVersion(); }

        const void* GetUBOData() const { return IsWire() ? m_record->GlobalConstants.data() : m_frontend->GetUBOData(); }
        Uint GetUBOSize() const { return IsWire() ? static_cast<Uint>(m_record->GlobalConstants.size()) : m_frontend->GetUBOSize(); }
        Uint32 GetUBOContentVersion() const { return IsWire() ? m_record->GlobalConstantsVersion : m_frontend->GetUBOContentVersion(); }
        Int GetActiveUniformBlocksCount() const {
            return IsWire() ? static_cast<Int>(Link().glBlockIndexToTProgram.size()) : m_frontend->GetActiveUniformBlocksCount();
        }
        Uint GetUniformBlockIndex(const char* name) const {
            if (!IsWire()) return m_frontend->GetUniformBlockIndex(name);
            auto found = Link().uniformBlockIndexByName.find(name);
            if (found == Link().uniformBlockIndexByName.end()) found = Link().uniformBlockIndexByName.find(String(name) + "[0]");
            return found == Link().uniformBlockIndexByName.end() ? GL_INVALID_INDEX : found->second;
        }
        Uint GetUniformBlockBinding(Uint index) const {
            if (!IsWire()) return m_frontend->GetUniformBlockBinding(index);
            return index < m_record->BlockBindings.size() ? static_cast<Uint>(m_record->BlockBindings[index]) : 0;
        }
        const String& GetUniformBlockName(Uint index) const {
            if (!IsWire()) return m_frontend->GetUniformBlockName(index);
            const auto* block = Block(index);
            static const String empty;
            return block ? block->name : empty;
        }
        Uint GetUBOSizeAt(Uint index) const {
            if (!IsWire()) return m_frontend->GetUBOSizeAt(index);
            const auto* block = Block(index);
            return block ? (static_cast<Uint>(block->size) + 15u) & ~15u : 0;
        }
        Bool IsValidUniformLocation(Int location) const {
            return IsWire() ? Program::IsValidUniformLocation(Link(), location) : m_frontend->IsValidUniformLocation(location);
        }
        Bool UniformLocationsAliasSameUniform(Int a, Int b) const {
            if (!IsWire()) return m_frontend->UniformLocationsAliasSameUniform(a, b);
            return IsValidUniformLocation(a) && IsValidUniformLocation(b) &&
                Link().uniformIndexInTProgram[a] == Link().uniformIndexInTProgram[b];
        }
        GLenum GetUniformType(Uint location) const {
            if (!IsWire()) return m_frontend->GetUniformType(location);
            return IsValidUniformLocation(static_cast<Int>(location))
                ? Program::UniformAtIn(Link(), Link().uniformIndexInTProgram[location]).glDefineType : 0;
        }
        Int GetUniformSamplerOrImageUnitIndex(Uint location) const {
            if (!IsWire()) return m_frontend->GetUniformSamplerOrImageUnitIndex(location);
            const auto& units = m_record->SamplerUnits;
            const auto found = std::lower_bound(units.begin(), units.end(), location,
                [](const MG_Pipe::MGPProgramSamplerUnit& unit, Uint value) { return unit.Location < value; });
            return found != units.end() && found->Location == location ? found->Unit : -1;
        }
        Int GetUniformLocation(const String& name) const {
            if (!IsWire()) return m_frontend->GetUniformLocation(name);
            const auto& locations = Link().uniformLocations;
            auto found = locations.find(name);
            if (found != locations.end()) return static_cast<Int>(found->second);
            if (name.empty()) return -1;
            found = locations.find(name + "[0]");
            if (found != locations.end()) return static_cast<Int>(found->second);
            if (name.back() != ']' || name.size() < 4) return -1;
            const SizeT bracket = name.rfind('[');
            if (bracket == String::npos || bracket + 1 >= name.size() - 1) return -1;
            Uint element = 0;
            for (SizeT i = bracket + 1; i < name.size() - 1; ++i) {
                if (name[i] < '0' || name[i] > '9') return -1;
                element = element * 10 + static_cast<Uint>(name[i] - '0');
                if (element > 0x0fffffffu) return -1;
            }
            found = locations.find(name.substr(0, bracket) + "[0]");
            if (found == locations.end()) found = locations.find(name.substr(0, bracket));
            if (found == locations.end()) return -1;
            const Int base = static_cast<Int>(found->second);
            if (!IsValidUniformLocation(base)) return -1;
            const Int tIndex = Link().uniformIndexInTProgram[base];
            if (!Program::UniformAtIn(Link(), tIndex).type.isArray ||
                static_cast<GLint>(element) >= Program::GetUniformArraySizeByTIndex(Link(), tIndex)) return -1;
            const Int location = base + static_cast<Int>(element);
            return UniformLocationsAliasSameUniform(base, location) ? location : -1;
        }

        SizeT GetTransformFeedbackVaryingCount() const { return IsWire() ? Link().xfbVaryings.size() : m_frontend->GetTransformFeedbackVaryingCount(); }
        const Vector<MG_State::GLState::XfbVarying>& GetTransformFeedbackVaryings() const {
            return IsWire() ? Link().xfbVaryings : m_frontend->GetTransformFeedbackVaryings();
        }
        SizeT GetTransformFeedbackBufferCount() const { return IsWire() ? Link().xfbStrides.size() : m_frontend->GetTransformFeedbackBufferCount(); }
        Uint32 GetTransformFeedbackStride(Uint index) const {
            if (!IsWire()) return m_frontend->GetTransformFeedbackStride(index);
            return index < Link().xfbStrides.size() ? Link().xfbStrides[index] : 0;
        }
        GLenum GetTransformFeedbackBufferMode() const {
            return IsWire() ? Link().xfbBufferMode : m_frontend->GetTransformFeedbackBufferMode();
        }
        // P7 wave 2 package C, OQ-8 (CONTRACT-P7 §5.3): READ OUT OF THE ARCHIVE.
        //
        // What stood here re-ran spvReflectCreateShaderModule over EVERY stage module of the
        // program, and it did so PER DRAW: a MagmaProgramSource is a borrowed view that
        // WireDraw.inc's WireProgramSource constructs afresh for each wire draw, so the lazy
        // `m_storageBlocksReady` latch could never outlive one. The list it rebuilt is a pure
        // function of the linked program, so it is now built once at link time and travels in
        // the archive (LinkArtifacts::storageBlocks), which takes the SPIR-V reflector off the
        // wire draw path entirely - this header no longer includes spirv_reflect.h at all.
        Uint GetShaderStorageBlockIndex(const String& name) const {
            if (!IsWire()) return DirectVulkan::GetShaderStorageBlockIndex(*m_frontend, name);
            const auto& blocks = Link().storageBlocks;
            // The archive carries NORMALISED names (the array subscript stripped), because
            // that is the spelling SPIRV-Reflect's type_name gives and the spelling
            // ProgramFactory::ReflectLayout looks up. A caller that still holds a GL-style
            // "Foo[2]" gets the same second chance it always had.
            const auto find = [&blocks](const String& key) {
                return std::find_if(blocks.begin(), blocks.end(),
                    [&](const MG_State::GLState::StorageBlockReflection& block) {
                        return block.name == key;
                    });
            };
            auto found = find(name);
            if (found == blocks.end() && !name.empty() && name.back() == ']') {
                const auto bracket = name.rfind('[');
                if (bracket != String::npos) found = find(name.substr(0, bracket));
            }
            return found == blocks.end() ? GL_INVALID_INDEX
                                         : static_cast<Uint>(found - blocks.begin());
        }
        Uint GetShaderStorageBlockBinding(Uint index) const {
            if (!IsWire()) return DirectVulkan::GetShaderStorageBlockBinding(*m_frontend, index);
            const auto& blocks = Link().storageBlocks;
            if (index >= blocks.size()) return 0;
            const auto& block = blocks[index];
            // THE OVERRIDE STILL WINS, and it still comes off the RECORD rather than the
            // archive: glShaderStorageBlockBinding is a post-link mutable and the archive
            // carries link-time defaults only (rule on MagmaProgramSource, and the reason
            // StorageBlockReflection::binding is documented as the DECLARED binding).
            for (const auto& override : m_record->StorageOverrides) {
                if (override.Name == block.name) return override.Binding;
            }
            return block.binding;
        }

    private:
        const MG_State::GLState::LinkArtifacts& Link() const { return m_record->Archive->Link; }
        const MG_State::GLState::BlockReflection* Block(Uint index) const {
            if (index >= Link().glBlockIndexToTProgram.size()) return nullptr;
            const Int tIndex = Link().glBlockIndexToTProgram[index];
            return tIndex >= 0 && static_cast<SizeT>(tIndex) < Link().blockReflection.size()
                ? &Link().blockReflection[tIndex] : nullptr;
        }
        const Program* m_frontend = nullptr;
        MG_Pipe::MGPipeHandle m_handle = MG_Pipe::kMGPipeNullHandle;
        const MG_Pipe::MGPipeShaderCsoRecord* m_record = nullptr;
        // P7 OQ-8: the two mutables that memoised the per-draw reflect are gone with it. They
        // were `mutable` precisely because this class is a borrowed const view - and that is
        // also why they never helped: a view lives for one draw, so the latch was cold every
        // time it was read.
    };
#else
    using MagmaProgramSource = MG_State::GLState::ProgramObject;
#endif
}
