// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VertexInputStateFactory.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "VertexInputStateFactory.h"
#include <MG_Backend/Record/ApplyRoleBackend.h>
#include "MagmaPipeArms.h"
#include "MG_Util/Converters/MGToStr/DataTypeConverter.h"
#include <MG_Backend/BackendObjects.h>
#if MOBILEGL_BUILD_DISAGGREGATED
#include <Config.h>
#include <MG_Remote/Server/ServerLoop.h>
#endif
#include <utility>

namespace MobileGL::MG_Backend::DirectVulkan {
    Bool VertexInputStateFactory::BuildWireVertexInput(const MG_Pipe::MGPipeVertexElementsRecord& elements,
            const MG_Pipe::MGPipeApplierState& state, Uint32 activeMask, BackendVertexInputState& out) const {
        out = BackendVertexInputState{};
        for (Uint32 location = 0; location < elements.AttributeCount; ++location) {
            const auto& attr = elements.Attributes[location];
            if (!attr.Enabled || !(activeMask & (1u << location))) continue;
            // P7 wave 2 package C, CONTRACT-P7 §3.2 `vertex-layout`: THE ONE STRING BECAME TWO
            // VERDICTS, and the split is by what the reason is ABOUT rather than by severity.
            //
            // Every reason here used to return false, and the single caller answered that with
            // one P7-marked MagmaWireFatal - an abort, bypassing Session::Fail, invisible
            // to the census gate and with no equivalent on the monolith arm at all. But the
            // reasons are not one kind of thing:
            //
            //   * `buffer-window` is a statement about the RECORD: an enabled attribute the
            //     program reads names a slot outside the window set_vertex_buffers published.
            //     Nothing about the device or the format is involved; the two halves of the
            //     protocol disagree about what crossed. It stays a named Fatal through the hook
            //     (see the caller), and it is load-bearing rather than defensive - it is what
            //     caught the Redmi VAO hash collision fixed in 376c04be.
            //
            //   * every other reason is a statement about what VULKAN CAN EXPRESS on this
            //     device: a GL type with no VkFormat, a format without
            //     VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT, a component size we cannot compute, an
            //     offset sum that does not fit. The monolith arm has had an answer for these
            //     since it was written, and it is not an abort: mask the attribute out of the
            //     vertex input state and carry on (the `unsupportedAttribMask |= ...; continue;`
            //     sites in GetOrCreateVertexInputState above). The draw path then decides,
            //     loudly and once, whether the masked attribute was one the program actually
            //     reads. This arm now does exactly that, which is what "same observable as the
            //     monolith lane" means for this row.
            //
            // THE COUNT: the contract says "the other six"; there are SEVEN return sites below
            // it, because `offset-overflow` was added after the audit's count and is an
            // arithmetic guard rather than a shape one. It is masked with the rest: an
            // attribute whose base offsets cannot be added is one Vulkan cannot fetch, the
            // monolith arm never computes that sum at all, and masking is strictly safer than
            // a Fatal for a number no application can reach on purpose. Recorded in
            // notes/p7/magma-c.md rather than silently reconciled.
            const auto describe = [&](const char* reason, VkFormat format) {
                MGLOG_E_ONCE("Magma wire vertex layout: %s location=%u type=%u size=%u normalized=%u integer=%u long=%u bgra=%u stride=%d offset=%llu format=%d bufferWindow=%u+%u activeMask=0x%x",
                    reason, location, attr.Type, static_cast<Uint32>(attr.Size), static_cast<Uint32>(attr.Normalized),
                    static_cast<Uint32>(attr.IsInteger), static_cast<Uint32>(attr.IsLong), static_cast<Uint32>(attr.IsBgra),
                    attr.Stride, static_cast<unsigned long long>(attr.Offset), static_cast<Int>(format),
                    state.VertexBufferStart, state.VertexBufferCount, activeMask);
            };
            // The protocol verdict: the caller turns a false into the named Fatal.
            const auto reject = [&](const char* reason, VkFormat format = VK_FORMAT_UNDEFINED) {
                describe(reason, format);
                return false;
            };
            // The device verdict: MGLOG_E_ONCE + mask + continue, the monolith arm's shape.
            // MGLOG_E_ONCE and not MGLOG_E, also the monolith arm's: an unmappable attribute is
            // a property of the VAO and the device, so it repeats every draw, and the per-draw
            // line is what made the old abort look preferable to whoever wrote it.
            const auto maskOut = [&](const char* reason, VkFormat format = VK_FORMAT_UNDEFINED) {
                describe(reason, format);
                out.unsupportedAttribMask |= 1u << location;
            };
            // set_vertex_buffers is flattened PER ATTRIBUTE (VertexInputEmit.h), not
            // indexed by the original ARB binding point in attr.BindingIndex.
            if (location < state.VertexBufferStart ||
                location - state.VertexBufferStart >= state.VertexBufferCount) return reject("buffer-window");
            const auto& buffer = state.VertexBuffers[location];
            const auto type = static_cast<DataType>(attr.Type);
            if (attr.Stride < 0 || attr.Size < 1 || attr.Size > 4) { maskOut("attribute-shape"); continue; }
            VkFormat format = ToVkVertexFormat(type, attr.Size, attr.Normalized, attr.IsInteger,
                                               attr.IsBgra, attr.IsLong);
            auto conversion = VertexStreamConversion::None;
            if (format == VK_FORMAT_UNDEFINED && type == DataType::Float64) {
                const auto* backend = MG_Backend::ApplyRoleBackend();
                if (backend && backend->GetDynamicParameters().SupportsFloat64VertexAttributes) {
                    // Exactly the monolith arm's answer for the same state: with native fp64
                    // the module KEPT its 64-bit inputs (DemoteFloat64Pass did not run), so
                    // narrowing the stream would feed float32 to a Float64 input - and the
                    // monolith build therefore leaves sourceVkFormat UNDEFINED and falls into
                    // its own mask-out below. Same place, same mask.
                    maskOut("native-fp64-format");
                    continue;
                }
                format = ToFloat32VertexFormat(attr.Size);
                conversion = VertexStreamConversion::Float64ToFloat32;
            }
            if (format == VK_FORMAT_UNDEFINED) { maskOut("format-map"); continue; }
            if (!SupportsVertexBufferFormat(format)) {
                if (!IsScaledIntegerVertexFormat(format)) { maskOut("native-format-feature", format); continue; }
                format = ToFloat32VertexFormat(attr.Size);
                conversion = VertexStreamConversion::ScaledIntegerToFloat32;
                if (!SupportsVertexBufferFormat(format)) { maskOut("converted-format-feature", format); continue; }
            }
            const SizeT elementSize = GetAttributeByteSize(type, attr.Size, attr.IsBgra);
            if (!elementSize) { maskOut("element-size", format); continue; }
            const Uint64 offset = attr.Offset + buffer.Offset;
            if (offset < attr.Offset) { maskOut("offset-overflow", format); continue; }
            const SizeT alignment = (type == DataType::Int2101010Rev || type == DataType::Uint2101010Rev)
                ? elementSize : GetComponentSize(type);
            if (conversion == VertexStreamConversion::None && alignment > 1 &&
                (offset % alignment || static_cast<Uint32>(attr.Stride) % alignment))
                conversion = VertexStreamConversion::Repack;
            Uint32 stride = static_cast<Uint32>(attr.Stride);
            if (stride && conversion != VertexStreamConversion::None)
                stride = conversion == VertexStreamConversion::Repack ? static_cast<Uint32>(elementSize)
                    : static_cast<Uint32>(attr.Size) * sizeof(Float);
            const Uint32 binding = static_cast<Uint32>(out.bindings.size());
            out.bindings.push_back({binding, stride,
                buffer.Divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX});
            out.attributes.push_back({location, binding, format, 0});
            out.bindingAttributeLocations.push_back(location);
            out.bindingBaseOffsets.push_back(static_cast<SizeT>(offset));
            out.bindingConversions.push_back(conversion);
            if (buffer.Divisor > 1) out.bindingDivisors.push_back({binding, buffer.Divisor});
            out.attributeLocationMask |= 1u << location;
        }
        // Native handles/offsets do not decide the pipeline layout. Include only the
        // resolved binding/attribute/divisor values, including a legal zero stride.
        Uint64 hash = XXH64(out.bindings.data(), out.bindings.size() * sizeof(out.bindings[0]), 0);
        hash = XXH64(out.attributes.data(), out.attributes.size() * sizeof(out.attributes[0]), hash);
        hash = XXH64(out.bindingDivisors.data(),
            out.bindingDivisors.size() * sizeof(out.bindingDivisors[0]), hash);
        // THE MASK IS PART OF THE LAYOUT NOW, for the same reason the monolith entry's hash
        // carries it (the XXH64_update over unsupportedAttribMask in GetOrCreateVertexInputState
        // above). Before this package the mask was always zero here, so leaving it out of the
        // hash was free; now two VAOs can produce the SAME bindings, attributes and divisors
        // and differ only in which enabled attribute was masked out, and a hash blind to that
        // would serve one of them the other's pipeline.
        out.layoutHash = XXH64(&out.unsupportedAttribMask, sizeof(out.unsupportedAttribMask), hash);
        out.state.vertexBindingDescriptionCount = static_cast<Uint32>(out.bindings.size());
        out.state.pVertexBindingDescriptions = out.bindings.data();
        out.state.vertexAttributeDescriptionCount = static_cast<Uint32>(out.attributes.size());
        out.state.pVertexAttributeDescriptions = out.attributes.data();
        if (!out.bindingDivisors.empty()) {
            out.divisorState.vertexBindingDivisorCount = static_cast<Uint32>(out.bindingDivisors.size());
            out.divisorState.pVertexBindingDivisors = out.bindingDivisors.data();
            out.state.pNext = &out.divisorState;
        }
        return true;
    }

    VkFormat VertexInputStateFactory::ToVkVertexFormat(DataType type, Int size, Bool normalized, Bool isInteger,
                                                       Bool isBgra, Bool isLong) {
        if (isBgra) {
            // GL_BGRA: four reversed-order components, always normalized (enforced at validation), only
            // legal with GL_UNSIGNED_BYTE or a 2_10_10_10 type. The reversed VkFormats put the
            // components back into R,G,B,A order for the shader.
            switch (type) {
            case DataType::Uint8:
                return VK_FORMAT_B8G8R8A8_UNORM;
            case DataType::Uint2101010Rev:
                return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
            case DataType::Int2101010Rev:
                return VK_FORMAT_A2R10G10B10_SNORM_PACK32;
            default:
                return VK_FORMAT_UNDEFINED;
            }
        }
        switch (type) {
        case DataType::Uint2101010Rev:
            // Packed 2_10_10_10 travels the float-normalizing path only; size is always 4. SNORM/UNORM
            // normalize, SSCALED/USCALED cast the packed field to float.
            if (isInteger || size != 4) return VK_FORMAT_UNDEFINED;
            return normalized ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : VK_FORMAT_A2B10G10R10_USCALED_PACK32;
        case DataType::Int2101010Rev:
            if (isInteger || size != 4) return VK_FORMAT_UNDEFINED;
            return normalized ? VK_FORMAT_A2B10G10R10_SNORM_PACK32 : VK_FORMAT_A2B10G10R10_SSCALED_PACK32;
        case DataType::Float64:
            // A 64-bit attribute is fetched as its 32-bit word pair and bitcast back to double in the
            // shader (PackDoubleVertexInputsPass does the shader half). That is bit-exact and, unlike
            // VK_FORMAT_R64*_SFLOAT, needs no format capability: lavapipe reports bufferFeatures = 0
            // for every R64 float format, so a native 64-bit vertex fetch is simply unavailable there
            // while shaderFloat64 is not. Both halves key off nothing but the attribute being long,
            // so they always agree without extra plumbing.
            //
            // ... as long as the shader half still runs. It does not when the backend has declared
            // no 64-bit vertex attribute support: DemoteFloat64Pass has already narrowed every
            // `dvec` input to a `vec` by then, so PackDoubleVertexInputsPass finds nothing to pack
            // and a UINT-formatted attribute would be fed to a float input - garbage with no
            // diagnostic anywhere. Declining here hands the attribute to the caller's
            // Float64ToFloat32 fallback instead, which narrows the source doubles to match the
            // demoted `vec` input - the same thing DirectGLES does for the same state. The
            // frontend RECORDS the format either way, so this gate is the only thing standing
            // between a legal glVertexAttribLFormat and a mismatched pipeline.
            if (
#if MOBILEGL_BUILD_DISAGGREGATED
                // P5c (hd, CONTRACT-P5C §3.7): see the narrowFloat64Arrays site above.
                MG_Config::Transport != MG_Config::TransportMode::Monolith
                    ? (MG_Remote::Server::ServerLoopInstance().Backend() == nullptr ||
                       !MG_Remote::Server::ServerLoopInstance().Backend()
                            ->GetDynamicParameters()
                            .SupportsFloat64VertexAttributes)
                    :
#endif
                MG_Backend::pActiveBackendObject == nullptr ||
                !MG_Backend::pActiveBackendObject->GetDynamicParameters().SupportsFloat64VertexAttributes) {
                return VK_FORMAT_UNDEFINED;
            }
            if (!isLong || isInteger || normalized) return VK_FORMAT_UNDEFINED;
            switch (size) {
            case 1: return VK_FORMAT_R32G32_UINT;
            case 2: return VK_FORMAT_R32G32B32A32_UINT;
            // A dvec3/dvec4 input is 6/8 uint32 components: no single VkFormat, and GL spreads it
            // over two attribute locations, which the location-per-VAO-index model here does not
            // express. Declined rather than fetched wrong.
            default: return VK_FORMAT_UNDEFINED;
            }
        case DataType::Float32:
            switch (size) {
            case 1: return VK_FORMAT_R32_SFLOAT;
            case 2: return VK_FORMAT_R32G32_SFLOAT;
            case 3: return VK_FORMAT_R32G32B32_SFLOAT;
            case 4: return VK_FORMAT_R32G32B32A32_SFLOAT;
            default: return VK_FORMAT_UNDEFINED;
            }
        case DataType::Float16:
            // GL_HALF_FLOAT is a floating-point array type: it is never an integer attribute, and
            // GL_TRUE for `normalized` is ignored for float types rather than selecting a *NORM format.
            if (isInteger) return VK_FORMAT_UNDEFINED;
            switch (size) {
            case 1: return VK_FORMAT_R16_SFLOAT;
            case 2: return VK_FORMAT_R16G16_SFLOAT;
            case 3: return VK_FORMAT_R16G16B16_SFLOAT;
            case 4: return VK_FORMAT_R16G16B16A16_SFLOAT;
            default: return VK_FORMAT_UNDEFINED;
            }
        case DataType::Int32:
            if (!isInteger || normalized) return VK_FORMAT_UNDEFINED;
            switch (size) {
            case 1: return VK_FORMAT_R32_SINT;
            case 2: return VK_FORMAT_R32G32_SINT;
            case 3: return VK_FORMAT_R32G32B32_SINT;
            case 4: return VK_FORMAT_R32G32B32A32_SINT;
            default: return VK_FORMAT_UNDEFINED;
            }
        case DataType::Uint32:
            if (!isInteger || normalized) return VK_FORMAT_UNDEFINED;
            switch (size) {
            case 1: return VK_FORMAT_R32_UINT;
            case 2: return VK_FORMAT_R32G32_UINT;
            case 3: return VK_FORMAT_R32G32B32_UINT;
            case 4: return VK_FORMAT_R32G32B32A32_UINT;
            default: return VK_FORMAT_UNDEFINED;
            }
        case DataType::Int16:
            switch (size) {
            case 1:
                return isInteger ? VK_FORMAT_R16_SINT : (normalized ? VK_FORMAT_R16_SNORM : VK_FORMAT_R16_SSCALED);
            case 2:
                return isInteger ? VK_FORMAT_R16G16_SINT
                                 : (normalized ? VK_FORMAT_R16G16_SNORM : VK_FORMAT_R16G16_SSCALED);
            case 3:
                return isInteger ? VK_FORMAT_R16G16B16_SINT
                                 : (normalized ? VK_FORMAT_R16G16B16_SNORM : VK_FORMAT_R16G16B16_SSCALED);
            case 4:
                return isInteger ? VK_FORMAT_R16G16B16A16_SINT
                                 : (normalized ? VK_FORMAT_R16G16B16A16_SNORM : VK_FORMAT_R16G16B16A16_SSCALED);
            default: return VK_FORMAT_UNDEFINED;
            }
        case DataType::Uint16:
            switch (size) {
            case 1:
                return isInteger ? VK_FORMAT_R16_UINT : (normalized ? VK_FORMAT_R16_UNORM : VK_FORMAT_R16_USCALED);
            case 2:
                return isInteger ? VK_FORMAT_R16G16_UINT
                                 : (normalized ? VK_FORMAT_R16G16_UNORM : VK_FORMAT_R16G16_USCALED);
            case 3:
                return isInteger ? VK_FORMAT_R16G16B16_UINT
                                 : (normalized ? VK_FORMAT_R16G16B16_UNORM : VK_FORMAT_R16G16B16_USCALED);
            case 4:
                return isInteger ? VK_FORMAT_R16G16B16A16_UINT
                                 : (normalized ? VK_FORMAT_R16G16B16A16_UNORM : VK_FORMAT_R16G16B16A16_USCALED);
            default: return VK_FORMAT_UNDEFINED;
            }
        case DataType::Int8:
            switch (size) {
            case 1:
                return isInteger ? VK_FORMAT_R8_SINT : (normalized ? VK_FORMAT_R8_SNORM : VK_FORMAT_R8_SSCALED);
            case 2:
                return isInteger ? VK_FORMAT_R8G8_SINT
                                 : (normalized ? VK_FORMAT_R8G8_SNORM : VK_FORMAT_R8G8_SSCALED);
            case 3:
                return isInteger ? VK_FORMAT_R8G8B8_SINT
                                 : (normalized ? VK_FORMAT_R8G8B8_SNORM : VK_FORMAT_R8G8B8_SSCALED);
            case 4:
                return isInteger ? VK_FORMAT_R8G8B8A8_SINT
                                 : (normalized ? VK_FORMAT_R8G8B8A8_SNORM : VK_FORMAT_R8G8B8A8_SSCALED);
            default: return VK_FORMAT_UNDEFINED;
            }
        case DataType::Uint8:
            switch (size) {
            case 1:
                return isInteger ? VK_FORMAT_R8_UINT : (normalized ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8_USCALED);
            case 2:
                return isInteger ? VK_FORMAT_R8G8_UINT
                                 : (normalized ? VK_FORMAT_R8G8_UNORM : VK_FORMAT_R8G8_USCALED);
            case 3:
                return isInteger ? VK_FORMAT_R8G8B8_UINT
                                 : (normalized ? VK_FORMAT_R8G8B8_UNORM : VK_FORMAT_R8G8B8_USCALED);
            case 4:
                return isInteger ? VK_FORMAT_R8G8B8A8_UINT
                                 : (normalized ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R8G8B8A8_USCALED);
            default: return VK_FORMAT_UNDEFINED;
            }
        default:
            return VK_FORMAT_UNDEFINED;
        }
    }

    SizeT VertexInputStateFactory::GetComponentSize(DataType type) {
        switch (type) {
        case DataType::Int8:
        case DataType::Uint8:
            return 1;
        case DataType::Int16:
        case DataType::Uint16:
        case DataType::Float16:
            return 2;
        case DataType::Int32:
        case DataType::Uint32:
        case DataType::Float32:
        case DataType::Fixed32:
            return 4;
        case DataType::Float64:
            return 8;
        default:
            return 0;
        }
    }

    SizeT VertexInputStateFactory::GetAttributeByteSize(DataType type, Int size, Bool isBgra) {
        // The packed 2_10_10_10 types are a single 32-bit word for all 4 components; GL_BGRA is always
        // 4 components (GL_UNSIGNED_BYTE x4 = 4 bytes, or a packed word = 4 bytes) -- both are 4 bytes.
        if (type == DataType::Int2101010Rev || type == DataType::Uint2101010Rev || isBgra) {
            return 4;
        }
        const SizeT componentSize = GetComponentSize(type);
        return componentSize == 0 ? 0 : componentSize * static_cast<SizeT>(size);
    }

    Bool VertexInputStateFactory::IsScaledIntegerVertexFormat(VkFormat format) {
        switch (format) {
        case VK_FORMAT_R8_USCALED:
        case VK_FORMAT_R8_SSCALED:
        case VK_FORMAT_R8G8_USCALED:
        case VK_FORMAT_R8G8_SSCALED:
        case VK_FORMAT_R8G8B8_USCALED:
        case VK_FORMAT_R8G8B8_SSCALED:
        case VK_FORMAT_R8G8B8A8_USCALED:
        case VK_FORMAT_R8G8B8A8_SSCALED:
        case VK_FORMAT_R16_USCALED:
        case VK_FORMAT_R16_SSCALED:
        case VK_FORMAT_R16G16_USCALED:
        case VK_FORMAT_R16G16_SSCALED:
        case VK_FORMAT_R16G16B16_USCALED:
        case VK_FORMAT_R16G16B16_SSCALED:
        case VK_FORMAT_R16G16B16A16_USCALED:
        case VK_FORMAT_R16G16B16A16_SSCALED:
            return true;
        default:
            return false;
        }
    }

    VkFormat VertexInputStateFactory::ToFloat32VertexFormat(Int componentCount) {
        switch (componentCount) {
        case 1: return VK_FORMAT_R32_SFLOAT;
        case 2: return VK_FORMAT_R32G32_SFLOAT;
        case 3: return VK_FORMAT_R32G32B32_SFLOAT;
        case 4: return VK_FORMAT_R32G32B32A32_SFLOAT;
        default: return VK_FORMAT_UNDEFINED;
        }
    }

    Bool VertexInputStateFactory::SupportsVertexBufferFormat(VkFormat format) const {
        if (m_physicalDevice == VK_NULL_HANDLE || format == VK_FORMAT_UNDEFINED) {
            return false;
        }
        const auto found = m_wireVertexFormatSupport.find(format);
        if (found != m_wireVertexFormatSupport.end()) return found->second;
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(m_physicalDevice, format, &properties);
        const Bool supported = (properties.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0;
        m_wireVertexFormatSupport.emplace(format, supported);
        return supported;
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
