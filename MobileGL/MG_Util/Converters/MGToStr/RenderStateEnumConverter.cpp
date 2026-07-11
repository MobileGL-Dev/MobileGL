// MobileGL - MobileGL/MG_Util/Converters/MGToStr/RenderStateEnumConverter.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "RenderStateEnumConverter.h"

namespace MobileGL {
    namespace MG_Util {
        String ConvertBlendFactorToString(BlendFactor v) {
            switch (v) {
            case BlendFactor::Zero:
                return "Zero";
            case BlendFactor::One:
                return "One";
            case BlendFactor::SrcColor:
                return "SrcColor";
            case BlendFactor::OneMinusSrcColor:
                return "OneMinusSrcColor";
            case BlendFactor::DstColor:
                return "DstColor";
            case BlendFactor::OneMinusDstColor:
                return "OneMinusDstColor";
            case BlendFactor::SrcAlpha:
                return "SrcAlpha";
            case BlendFactor::OneMinusSrcAlpha:
                return "OneMinusSrcAlpha";
            case BlendFactor::DstAlpha:
                return "DstAlpha";
            case BlendFactor::OneMinusDstAlpha:
                return "OneMinusDstAlpha";
            case BlendFactor::ConstantColor:
                return "ConstantColor";
            case BlendFactor::OneMinusConstantColor:
                return "OneMinusConstantColor";
            case BlendFactor::ConstantAlpha:
                return "ConstantAlpha";
            case BlendFactor::OneMinusConstantAlpha:
                return "OneMinusConstantAlpha";
            case BlendFactor::Src1Color:
                return "Src1Color";
            case BlendFactor::OneMinusSrc1Color:
                return "OneMinusSrc1Color";
            case BlendFactor::Src1Alpha:
                return "Src1Alpha";
            case BlendFactor::OneMinusSrc1Alpha:
                return "OneMinusSrc1Alpha";
            default:
                return "Unknown";
            }
        }

        String ConvertLogicOperationToString(LogicOperation v) {
            switch (v) {
            case LogicOperation::Clear:
                return "Clear";
            case LogicOperation::And:
                return "And";
            case LogicOperation::AndReverse:
                return "AndReverse";
            case LogicOperation::Copy:
                return "Copy";
            case LogicOperation::AndInverted:
                return "AndInverted";
            case LogicOperation::Noop:
                return "Noop";
            case LogicOperation::Xor:
                return "Xor";
            case LogicOperation::Or:
                return "Or";
            case LogicOperation::Nor:
                return "Nor";
            case LogicOperation::Equiv:
                return "Equiv";
            case LogicOperation::Invert:
                return "Invert";
            case LogicOperation::OrReverse:
                return "OrReverse";
            case LogicOperation::CopyInverted:
                return "CopyInverted";
            case LogicOperation::OrInverted:
                return "OrInverted";
            case LogicOperation::Nand:
                return "Nand";
            case LogicOperation::Set:
                return "Set";
            default:
                return "Unknown";
            }
        }

        String ConvertDepthTestFuncToString(DepthTestFunc v) {
            switch (v) {
            case DepthTestFunc::Never:
                return "Never";
            case DepthTestFunc::Less:
                return "Less";
            case DepthTestFunc::Equal:
                return "Equal";
            case DepthTestFunc::LessEqual:
                return "LessEqual";
            case DepthTestFunc::Greater:
                return "Greater";
            case DepthTestFunc::NotEqual:
                return "NotEqual";
            case DepthTestFunc::GreaterEqual:
                return "GreaterEqual";
            case DepthTestFunc::Always:
                return "Always";
            default:
                return "Unknown";
            }
        }

        String ConvertStencilOperationToString(StencilOperation v) {
            switch (v) {
            case StencilOperation::Keep:
                return "Keep";
            case StencilOperation::Zero:
                return "Zero";
            case StencilOperation::Replace:
                return "Replace";
            case StencilOperation::IncrementClamp:
                return "IncrementClamp";
            case StencilOperation::DecrementClamp:
                return "DecrementClamp";
            case StencilOperation::Invert:
                return "Invert";
            case StencilOperation::IncrementWrap:
                return "IncrementWrap";
            case StencilOperation::DecrementWrap:
                return "DecrementWrap";
            default:
                return "Unknown";
            }
        }

        String ConvertPixelStoreParamToString(PixelStoreParam v) {
            switch (v) {
            case PixelStoreParam::PackAlignment:
                return "PackAlignment";
            case PixelStoreParam::PackRowLength:
                return "PackRowLength";
            case PixelStoreParam::PackImageHeight:
                return "PackImageHeight";
            case PixelStoreParam::PackSkipRows:
                return "PackSkipRows";
            case PixelStoreParam::PackSkipPixels:
                return "PackSkipPixels";
            case PixelStoreParam::PackSkipImages:
                return "PackSkipImages";
            case PixelStoreParam::PackSwapBytes:
                return "PackSwapBytes";
            case PixelStoreParam::PackLSBFirst:
                return "PackLSBFirst";
            case PixelStoreParam::UnpackAlignment:
                return "UnpackAlignment";
            case PixelStoreParam::UnpackRowLength:
                return "UnpackRowLength";
            case PixelStoreParam::UnpackImageHeight:
                return "UnpackImageHeight";
            case PixelStoreParam::UnpackSkipRows:
                return "UnpackSkipRows";
            case PixelStoreParam::UnpackSkipPixels:
                return "UnpackSkipPixels";
            case PixelStoreParam::UnpackSkipImages:
                return "UnpackSkipImages";
            case PixelStoreParam::UnpackSwapBytes:
                return "UnpackSwapBytes";
            case PixelStoreParam::UnpackLSBFirst:
                return "UnpackLSBFirst";
            default:
                return "Unknown";
            }
        }

        String ConvertCullFaceModeToString(CullFaceMode v) {
            switch (v) {
            case CullFaceMode::Front:
                return "Front";
            case CullFaceMode::Back:
                return "Back";
            case CullFaceMode::FrontAndBack:
                return "FrontAndBack";
            default:
                return "Unknown";
            }
        }

        String ConvertFrontFaceModeToString(FrontFaceMode v) {
            switch (v) {
            case FrontFaceMode::CounterClockwise:
                return "CounterClockwise";
            case FrontFaceMode::Clockwise:
                return "Clockwise";
            default:
                return "Unknown";
            }
        }

        String ConvertProvokingVertexModeToString(ProvokingVertexMode v) {
            switch (v) {
            case ProvokingVertexMode::FirstVertex:
                return "FirstVertex";
            case ProvokingVertexMode::LastVertex:
                return "LastVertex";
            default:
                return "Unknown";
            }
        }

        String ConvertCapabilityInputToString(CapabilityInput v) {
            switch (v) {
            case CapabilityInput::Blend:
                return "Blend";
            case CapabilityInput::ClipDistance0:
                return "ClipDistance0";
            case CapabilityInput::ClipDistance1:
                return "ClipDistance1";
            case CapabilityInput::ClipDistance2:
                return "ClipDistance2";
            case CapabilityInput::ClipDistance3:
                return "ClipDistance3";
            case CapabilityInput::ClipDistance4:
                return "ClipDistance4";
            case CapabilityInput::ClipDistance5:
                return "ClipDistance5";
            case CapabilityInput::ClipDistance6:
                return "ClipDistance6";
            case CapabilityInput::ClipDistance7:
                return "ClipDistance7";
            case CapabilityInput::ColorLogicOp:
                return "ColorLogicOp";
            case CapabilityInput::CullFace:
                return "CullFace";
            case CapabilityInput::DebugOutput:
                return "DebugOutput";
            case CapabilityInput::DebugOutputSynchronous:
                return "DebugOutputSynchronous";
            case CapabilityInput::DepthClamp:
                return "DepthClamp";
            case CapabilityInput::DepthTest:
                return "DepthTest";
            case CapabilityInput::Dither:
                return "Dither";
            case CapabilityInput::FramebufferSrgb:
                return "FramebufferSrgb";
            case CapabilityInput::LineSmooth:
                return "LineSmooth";
            case CapabilityInput::Multisample:
                return "Multisample";
            case CapabilityInput::PolygonOffsetFill:
                return "PolygonOffsetFill";
            case CapabilityInput::PolygonOffsetLine:
                return "PolygonOffsetLine";
            case CapabilityInput::PolygonOffsetPoint:
                return "PolygonOffsetPoint";
            case CapabilityInput::PolygonSmooth:
                return "PolygonSmooth";
            case CapabilityInput::PrimitiveRestart:
                return "PrimitiveRestart";
            case CapabilityInput::PrimitiveRestartFixedIndex:
                return "PrimitiveRestartFixedIndex";
            case CapabilityInput::RasterizerDiscard:
                return "RasterizerDiscard";
            case CapabilityInput::SampleAlphaToCoverage:
                return "SampleAlphaToCoverage";
            case CapabilityInput::SampleAlphaToOne:
                return "SampleAlphaToOne";
            case CapabilityInput::SampleCoverage:
                return "SampleCoverage";
            case CapabilityInput::SampleShading:
                return "SampleShading";
            case CapabilityInput::SampleMask:
                return "SampleMask";
            case CapabilityInput::ScissorTest:
                return "ScissorTest";
            case CapabilityInput::StencilTest:
                return "StencilTest";
            case CapabilityInput::TextureCubeMapSeamless:
                return "TextureCubeMapSeamless";
            case CapabilityInput::ProgramPointSize:
                return "ProgramPointSize";
            default:
                return "Unknown";
            }
        }
    } // namespace MG_Util
} // namespace MobileGL
