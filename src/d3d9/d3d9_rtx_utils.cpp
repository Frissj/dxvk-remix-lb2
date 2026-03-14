#pragma once

#include "d3d9_include.h"
#include "d3d9_state.h"

#include "d3d9_util.h"
#include "d3d9_buffer.h"

#include "d3d9_rtx_utils.h"
#include "d3d9_device.h"
#include "d3d9_shader.h"

#include <algorithm>
#include <cmath>

#include "../util/util_fastops.h"
#include "../util/util_math.h"

namespace dxvk {
  bool getMinMaxBoneIndices(const uint8_t* pBoneIndices, uint32_t stride, uint32_t vertexCount, uint32_t numBonesPerVertex, int& minBoneIndex, int& maxBoneIndex) {
    ScopedCpuProfileZone();
    if (vertexCount == 0)
      return false;

    minBoneIndex = 256;
    maxBoneIndex = -1;

    for (uint32_t i = 0; i < vertexCount; ++i) {
      for (uint32_t j = 0; j < numBonesPerVertex; ++j) {
        minBoneIndex = std::min(minBoneIndex, (int) pBoneIndices[j]);
        maxBoneIndex = std::max(maxBoneIndex, (int) pBoneIndices[j]);
      }
      pBoneIndices += stride;
    }

    return true;
  }

  bool isRenderTargetPrimary(const D3DPRESENT_PARAMETERS& presenterParams, const D3D9_COMMON_TEXTURE_DESC* renderTargetDesc) {
    return presenterParams.BackBufferWidth == renderTargetDesc->Width &&
           presenterParams.BackBufferHeight == renderTargetDesc->Height;
  }

  DxvkRtTextureOperation convertTextureOp(uint32_t op) {
    // TODO: support more D3DTEXTUREOP member when necessary
    switch (op) {
    default:
    case D3DTOP_MODULATE: return DxvkRtTextureOperation::Modulate;
    case D3DTOP_DISABLE: return DxvkRtTextureOperation::Disable;
    case D3DTOP_SELECTARG1: return DxvkRtTextureOperation::SelectArg1;
    case D3DTOP_SELECTARG2: return DxvkRtTextureOperation::SelectArg2;
    case D3DTOP_MODULATE2X: return DxvkRtTextureOperation::Modulate2x;
    case D3DTOP_MODULATE4X: return DxvkRtTextureOperation::Modulate4x;
    case D3DTOP_ADD: return DxvkRtTextureOperation::Add;
    }
  }

  RtTextureArgSource convertColorSource(uint32_t source) {
    switch (source) {
    default:
    case D3DMCS_COLOR2: // TODO: support 2nd vertex color array
    case D3DMCS_MATERIAL: return RtTextureArgSource::None;
    case D3DMCS_COLOR1: return RtTextureArgSource::VertexColor0;
    }
  }

  RtTextureArgSource convertTextureArg(uint32_t arg, RtTextureArgSource color0, RtTextureArgSource color1) {
    // TODO: support more D3DTA_* macro when necessary
    switch (arg) {
    default: return RtTextureArgSource::None;
    case D3DTA_CURRENT:
    case D3DTA_DIFFUSE: return color0;
    case D3DTA_SPECULAR: return color1;
    case D3DTA_TEXTURE: return RtTextureArgSource::Texture;
    case D3DTA_TFACTOR: return RtTextureArgSource::TFactor;
    }
  }

  void setTextureStageState(const Direct3DState9& d3d9State, const uint32_t stageIdx, bool useStageTextureFactorBlending, bool useMultipleStageTextureFactorBlending, LegacyMaterialData& materialData, DrawCallTransforms& transformData) {
    materialData.textureColorOperation = convertTextureOp(d3d9State.textureStages[stageIdx][DXVK_TSS_COLOROP]);
    materialData.textureColorArg1Source = convertTextureArg(d3d9State.textureStages[stageIdx][DXVK_TSS_COLORARG1], materialData.diffuseColorSource, materialData.specularColorSource);
    materialData.textureColorArg2Source = convertTextureArg(d3d9State.textureStages[stageIdx][DXVK_TSS_COLORARG2], materialData.diffuseColorSource, materialData.specularColorSource);
    if (!useStageTextureFactorBlending) {
      if (materialData.textureColorArg1Source == RtTextureArgSource::TFactor) {
        materialData.textureColorArg1Source = RtTextureArgSource::None;
      }
      if (materialData.textureColorArg2Source == RtTextureArgSource::TFactor) {
        materialData.textureColorArg2Source = RtTextureArgSource::None;
      }
    }

    materialData.textureAlphaOperation = convertTextureOp(d3d9State.textureStages[stageIdx][DXVK_TSS_ALPHAOP]);
    materialData.textureAlphaArg1Source = convertTextureArg(d3d9State.textureStages[stageIdx][DXVK_TSS_ALPHAARG1], materialData.diffuseColorSource, materialData.specularColorSource);
    materialData.textureAlphaArg2Source = convertTextureArg(d3d9State.textureStages[stageIdx][DXVK_TSS_ALPHAARG2], materialData.diffuseColorSource, materialData.specularColorSource);
    if (!useStageTextureFactorBlending) {
      if (materialData.textureAlphaArg1Source == RtTextureArgSource::TFactor) {
        materialData.textureAlphaArg1Source = RtTextureArgSource::None;
      }
      if (materialData.textureAlphaArg2Source == RtTextureArgSource::TFactor) {
        materialData.textureAlphaArg2Source = RtTextureArgSource::None;
      }
    }

    materialData.isTextureFactorBlend = useMultipleStageTextureFactorBlending;

    const DWORD texcoordIndex = d3d9State.textureStages[stageIdx][DXVK_TSS_TEXCOORDINDEX];
    const DWORD transformFlags = d3d9State.textureStages[stageIdx][DXVK_TSS_TEXTURETRANSFORMFLAGS];

    const auto textureTransformCount = transformFlags & 0x3;

    if (textureTransformCount != D3DTTFF_DISABLE) {
      transformData.textureTransform = d3d9State.transforms[GetTransformIndex(D3DTS_TEXTURE0) + stageIdx];

      if (textureTransformCount > 2) {
        ONCE(Logger::info(str::format("[RTX-Compatibility-Info] Use of texture transform element counts beyond 2 is not supported in Remix yet (and thus will be clamped to 2 elements).")));
      }

      // Todo: Store texture transform element count (1-4) in the future.
    } else {
      transformData.textureTransform = Matrix4();
    }

    if (transformFlags & D3DTTFF_PROJECTED) {
      ONCE(Logger::info(str::format("[RTX-Compatibility-Info] Use of projected texture transform detected, but it's not supported in Remix yet.")));

      // Todo: Store texture transform projection flag in the future.
    }

    switch (texcoordIndex) {
    default:
    case D3DTSS_TCI_PASSTHRU:
      transformData.texgenMode = TexGenMode::None;

      break;
    case D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR:
    case D3DTSS_TCI_SPHEREMAP:
      transformData.texgenMode = TexGenMode::None;

      ONCE(Logger::info(str::format("[RTX-Compatibility-Info] Use of special TCI flags detected (spheremap or camera space reflection vector), but they're not supported in Remix yet.")));

      break;
    case D3DTSS_TCI_CAMERASPACEPOSITION:
      transformData.texgenMode = TexGenMode::ViewPositions;

      break;
    case D3DTSS_TCI_CAMERASPACENORMAL:
      transformData.texgenMode = TexGenMode::ViewNormals;

      break;
    }
  }

  void setLegacyMaterialState(D3D9DeviceEx* pDevice, const bool alphaSwizzle, LegacyMaterialData& materialData) {
    assert(pDevice != nullptr);
    const Direct3DState9& d3d9State = *pDevice->GetRawState();

    const bool hasPositionT = d3d9State.vertexDecl != nullptr && d3d9State.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasPositionT);
    const bool hasColor0 = d3d9State.vertexDecl != nullptr && d3d9State.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasColor0);
    const bool hasColor1 = d3d9State.vertexDecl != nullptr && d3d9State.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasColor1);
    const bool lighting = d3d9State.renderStates[D3DRS_LIGHTING] != 0 && !hasPositionT; // FFP lighting on only if not positionT

    uint32_t diffuseSource = hasColor0 ? D3DMCS_COLOR1 : D3DMCS_MATERIAL;
    uint32_t specularSource = hasColor1 ? D3DMCS_COLOR2 : D3DMCS_MATERIAL;
    if (lighting) {
      const bool colorVertex = d3d9State.renderStates[D3DRS_COLORVERTEX] != 0;
      const uint32_t mask = (lighting && colorVertex) ? (diffuseSource | specularSource) : 0;

      diffuseSource = d3d9State.renderStates[D3DRS_DIFFUSEMATERIALSOURCE] & mask;
      specularSource = d3d9State.renderStates[D3DRS_SPECULARMATERIALSOURCE] & mask;
    }

    materialData.alphaTestEnabled = pDevice->IsAlphaTestEnabled();
    materialData.alphaTestCompareOp = materialData.alphaTestEnabled ? DecodeCompareOp(D3DCMPFUNC(d3d9State.renderStates[D3DRS_ALPHAFUNC])) : VK_COMPARE_OP_ALWAYS;
    materialData.alphaTestReferenceValue = d3d9State.renderStates[D3DRS_ALPHAREF] & 0xFF; // Note: Only bottom 8 bits should be used as per the standard.

    materialData.diffuseColorSource = convertColorSource(diffuseSource);
    materialData.specularColorSource = convertColorSource(specularSource);

    materialData.tFactor = d3d9State.renderStates[D3DRS_TEXTUREFACTOR];

    DxvkBlendMode& m = materialData.blendMode;
    m.enableBlending = d3d9State.renderStates[D3DRS_ALPHABLENDENABLE] != FALSE;

    D3D9BlendState color;
    color.Src = D3DBLEND(d3d9State.renderStates[D3DRS_SRCBLEND]);
    color.Dst = D3DBLEND(d3d9State.renderStates[D3DRS_DESTBLEND]);
    color.Op = D3DBLENDOP(d3d9State.renderStates[D3DRS_BLENDOP]);
    FixupBlendState(color);

    D3D9BlendState alpha = color;
    if (d3d9State.renderStates[D3DRS_SEPARATEALPHABLENDENABLE]) {
      alpha.Src = D3DBLEND(d3d9State.renderStates[D3DRS_SRCBLENDALPHA]);
      alpha.Dst = D3DBLEND(d3d9State.renderStates[D3DRS_DESTBLENDALPHA]);
      alpha.Op = D3DBLENDOP(d3d9State.renderStates[D3DRS_BLENDOPALPHA]);
      FixupBlendState(alpha);
    }

    m.colorSrcFactor = DecodeBlendFactor(color.Src, false);
    m.colorDstFactor = DecodeBlendFactor(color.Dst, false);
    m.colorBlendOp = DecodeBlendOp(color.Op);

    m.alphaSrcFactor = DecodeBlendFactor(alpha.Src, true);
    m.alphaDstFactor = DecodeBlendFactor(alpha.Dst, true);
    m.alphaBlendOp = DecodeBlendOp(alpha.Op);

    m.writeMask = d3d9State.renderStates[ColorWriteIndex(0)];

    auto NormalizeFactor = [alphaSwizzle](VkBlendFactor f) {
      if (alphaSwizzle) {
        if (f == VK_BLEND_FACTOR_DST_ALPHA) {
          return VK_BLEND_FACTOR_ONE;
        }
        if (f == VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA) {
          return VK_BLEND_FACTOR_ZERO;
        }
      }
      return f;
    };
    m.colorSrcFactor = NormalizeFactor(m.colorSrcFactor);
    m.colorDstFactor = NormalizeFactor(m.colorDstFactor);
    m.alphaSrcFactor = NormalizeFactor(m.alphaSrcFactor);
    m.alphaDstFactor = NormalizeFactor(m.alphaDstFactor);

    materialData.d3dMaterial = d3d9State.material;

    // Allow the users to configure vertex color as baked lighting for legacy draw calls.
    materialData.isVertexColorBakedLighting = RtxOptions::vertexColorIsBakedLighting();
  }


  void setFogState(D3D9DeviceEx* pDevice, FogState& fogState) {
    const Direct3DState9& d3d9State = *pDevice->GetRawState();

    if (d3d9State.renderStates[D3DRS_FOGENABLE]) {
      Vector4 color;
      DecodeD3DCOLOR(D3DCOLOR(d3d9State.renderStates[D3DRS_FOGCOLOR]), color.data);

      float end = bit::cast<float>(d3d9State.renderStates[D3DRS_FOGEND]);
      float start = bit::cast<float>(d3d9State.renderStates[D3DRS_FOGSTART]);

      fogState.mode = d3d9State.renderStates[D3DRS_FOGTABLEMODE] != D3DFOG_NONE ? d3d9State.renderStates[D3DRS_FOGTABLEMODE] : d3d9State.renderStates[D3DRS_FOGVERTEXMODE];
      fogState.color = color.xyz();
      fogState.scale = 1.0f / (end - start);
      fogState.end = end;
      fogState.density = bit::cast<float>(d3d9State.renderStates[D3DRS_FOGDENSITY]);
    } else {
      fogState.mode = D3DFOG_NONE;
    }
  }

  // NV-DXVK start: extract material properties from programmable shader constants (ubershader emulation)
  static int32_t findConstantRegister(const DxsoCtab& ctab, const char* name) {
    for (const auto& c : ctab.m_constantData) {
      if (c.name == name)
        return static_cast<int32_t>(c.registerIndex);
    }
    return -1;
  }

  static uint32_t float4ToD3DCOLOR(const Vector4& v) {
    uint8_t a = static_cast<uint8_t>(std::clamp(v[3], 0.0f, 1.0f) * 255.0f + 0.5f);
    uint8_t r = static_cast<uint8_t>(std::clamp(v[0], 0.0f, 1.0f) * 255.0f + 0.5f);
    uint8_t g = static_cast<uint8_t>(std::clamp(v[1], 0.0f, 1.0f) * 255.0f + 0.5f);
    uint8_t b = static_cast<uint8_t>(std::clamp(v[2], 0.0f, 1.0f) * 255.0f + 0.5f);
    return D3DCOLOR_ARGB(a, r, g, b);
  }

  static Vector4 readPsConstant(const Direct3DState9& d3d9State, const char* name) {
    if (d3d9State.pixelShader == nullptr)
      return Vector4(0.0f);
    const DxsoCtab& ctab = d3d9State.pixelShader->GetCommonShader()->GetCtab();
    int32_t reg = findConstantRegister(ctab, name);
    if (reg >= 0 && reg < caps::MaxFloatConstantsPS)
      return d3d9State.psConsts.fConsts[reg];
    return Vector4(0.0f);
  }

  static Vector4 readVsConstant(const Direct3DState9& d3d9State, const char* name) {
    if (d3d9State.vertexShader == nullptr)
      return Vector4(0.0f);
    const DxsoCtab& ctab = d3d9State.vertexShader->GetCommonShader()->GetCtab();
    int32_t reg = findConstantRegister(ctab, name);
    if (reg >= 0 && reg < caps::MaxFloatConstantsSoftware)
      return d3d9State.vsConsts.fConsts[reg];
    return Vector4(0.0f);
  }

  static bool isNonZero(const Vector4& v) {
    return v[0] != 0.0f || v[1] != 0.0f || v[2] != 0.0f || v[3] != 0.0f;
  }

  static bool isNonWhite(const Vector4& v) {
    return v[0] != 1.0f || v[1] != 1.0f || v[2] != 1.0f;
  }

  void extractShaderConstantMaterial(D3D9DeviceEx* pDevice, LegacyMaterialData& materialData) {
    if (!RtxOptions::enableShaderConstantExtraction())
      return;

    const Direct3DState9& d3d9State = *pDevice->GetRawState();

    // When using shader constant extraction, the game's ubershader handles lighting -
    // vertex colors are actual vertex colors, not baked lighting
    materialData.isVertexColorBakedLighting = false;
    materialData.hasExtractedPBR = true;

    // === ALBEDO COLOR ===
    // Primary diffuse from PS constant fs_layer0_diffuse
    Vector4 diffuseColor = readPsConstant(d3d9State, "fs_layer0_diffuse");
    // Fallback: VS constant vs_layer0_diffuse
    if (!isNonZero(diffuseColor))
      diffuseColor = readVsConstant(d3d9State, "vs_layer0_diffuse");
    // Default to white if nothing found
    if (!isNonZero(diffuseColor))
      diffuseColor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);

    // Multiply by VS tint
    Vector4 tintColor = readVsConstant(d3d9State, "vs_kTint");
    if (isNonZero(tintColor)) {
      diffuseColor[0] *= std::clamp(tintColor[0], 0.0f, 2.0f);
      diffuseColor[1] *= std::clamp(tintColor[1], 0.0f, 2.0f);
      diffuseColor[2] *= std::clamp(tintColor[2], 0.0f, 2.0f);
      diffuseColor[3] *= std::clamp(tintColor[3], 0.0f, 2.0f);
    }

    // Blend in additional diffuse layers (multi-layer materials)
    Vector4 layer1 = readPsConstant(d3d9State, "fs_layer1_diffuse");
    Vector4 layer2 = readPsConstant(d3d9State, "fs_layer2_diffuse");
    Vector4 layer3 = readPsConstant(d3d9State, "fs_layer3_diffuse");
    if (isNonZero(layer1)) {
      float a = std::clamp(layer1[3], 0.0f, 1.0f);
      diffuseColor[0] = diffuseColor[0] * (1.0f - a) + layer1[0] * a;
      diffuseColor[1] = diffuseColor[1] * (1.0f - a) + layer1[1] * a;
      diffuseColor[2] = diffuseColor[2] * (1.0f - a) + layer1[2] * a;
    }
    if (isNonZero(layer2)) {
      float a = std::clamp(layer2[3], 0.0f, 1.0f);
      diffuseColor[0] = diffuseColor[0] * (1.0f - a) + layer2[0] * a;
      diffuseColor[1] = diffuseColor[1] * (1.0f - a) + layer2[1] * a;
      diffuseColor[2] = diffuseColor[2] * (1.0f - a) + layer2[2] * a;
    }
    if (isNonZero(layer3)) {
      float a = std::clamp(layer3[3], 0.0f, 1.0f);
      diffuseColor[0] = diffuseColor[0] * (1.0f - a) + layer3[0] * a;
      diffuseColor[1] = diffuseColor[1] * (1.0f - a) + layer3[1] * a;
      diffuseColor[2] = diffuseColor[2] * (1.0f - a) + layer3[2] * a;
    }

    // Car paint materials: use dominant tint as albedo
    Vector4 carpaint0 = readPsConstant(d3d9State, "fs_carpaint_tints0");
    if (isNonZero(carpaint0)) {
      diffuseColor[0] *= std::clamp(carpaint0[0], 0.0f, 1.0f);
      diffuseColor[1] *= std::clamp(carpaint0[1], 0.0f, 1.0f);
      diffuseColor[2] *= std::clamp(carpaint0[2], 0.0f, 1.0f);
    }

    // Store final albedo
    materialData.extractedAlbedoColor = Vector3(
      std::clamp(diffuseColor[0], 0.0f, 1.0f),
      std::clamp(diffuseColor[1], 0.0f, 1.0f),
      std::clamp(diffuseColor[2], 0.0f, 1.0f)
    );
    materialData.extractedOpacity = std::clamp(diffuseColor[3], 0.0f, 1.0f);

    // Set tFactor for texture modulation pipeline
    materialData.tFactor = float4ToD3DCOLOR(Vector4(
      materialData.extractedAlbedoColor[0],
      materialData.extractedAlbedoColor[1],
      materialData.extractedAlbedoColor[2],
      materialData.extractedOpacity
    ));
    if (materialData.usesTexture()) {
      materialData.textureColorArg1Source = RtTextureArgSource::Texture;
      materialData.textureColorArg2Source = RtTextureArgSource::TFactor;
      materialData.textureColorOperation = DxvkRtTextureOperation::Modulate;
    } else {
      materialData.textureColorArg1Source = RtTextureArgSource::TFactor;
      materialData.textureColorOperation = DxvkRtTextureOperation::SelectArg1;
    }
    materialData.textureAlphaArg1Source = materialData.usesTexture()
      ? RtTextureArgSource::Texture : RtTextureArgSource::TFactor;
    materialData.textureAlphaArg2Source = materialData.usesTexture()
      ? RtTextureArgSource::TFactor : RtTextureArgSource::None;
    materialData.textureAlphaOperation = materialData.usesTexture()
      ? DxvkRtTextureOperation::Modulate : DxvkRtTextureOperation::SelectArg1;

    // === ROUGHNESS ===
    // Try BRDF roughness first (direct value from kBRDFRoughness)
    Vector4 brdfParams = readPsConstant(d3d9State, "fs_brdf_params");
    if (isNonZero(brdfParams) && brdfParams[0] > 0.0f)
      materialData.extractedRoughness = std::clamp(brdfParams[0], 0.01f, 1.0f);
    // Fallback: derive from specular cosine power (roughness = sqrt(2/(power+2)))
    Vector4 specParams = readPsConstant(d3d9State, "fs_specular_params");
    if (materialData.extractedRoughness < 0.0f && isNonZero(specParams) && specParams[0] > 0.0f) {
      materialData.extractedSpecularPower = specParams[0];
      materialData.extractedRoughness = std::clamp(std::sqrt(2.0f / (specParams[0] + 2.0f)), 0.01f, 1.0f);
    }
    // Fallback: fs_lego_params.x (engine-specific material roughness)
    Vector4 legoParams = readPsConstant(d3d9State, "fs_lego_params");
    if (materialData.extractedRoughness < 0.0f && isNonZero(legoParams))
      materialData.extractedRoughness = std::clamp(legoParams[0], 0.01f, 1.0f);

    // === METALLIC ===
    // From fresnel params - kFresnel = reflectance at normal incidence → metallic
    Vector4 fresnelParams = readPsConstant(d3d9State, "fs_fresnel_params");
    if (isNonZero(fresnelParams))
      materialData.extractedMetallic = std::clamp(fresnelParams[0], 0.0f, 1.0f);
    // Car paint → semi-metallic
    Vector4 carpaintParams = readPsConstant(d3d9State, "fs_carpaint_params");
    if (isNonZero(carpaintParams) && materialData.extractedMetallic < 0.0f)
      materialData.extractedMetallic = 0.5f;
    // Specular color luminance → metallic hint
    Vector4 specColor = readPsConstant(d3d9State, "fs_specular_specular");
    if (isNonZero(specColor)) {
      float specLum = specColor[0] * 0.299f + specColor[1] * 0.587f + specColor[2] * 0.114f;
      if (materialData.extractedRoughness < 0.0f)
        materialData.extractedRoughness = std::clamp(1.0f - specLum * 0.5f, 0.01f, 1.0f);
      if (materialData.extractedMetallic < 0.0f && specLum > 0.3f)
        materialData.extractedMetallic = std::clamp(specLum, 0.0f, 1.0f);
    }

    // === EMISSIVE ===
    // Only fs_incandescentGlow makes things glow. If the game set it, it glows. If not, it doesn't.
    // fs_rimLightColour is a shading effect, NOT emissive.
    Vector4 glowColor = readPsConstant(d3d9State, "fs_incandescentGlow");
    if (isNonZero(glowColor)) {
      materialData.extractedEmissiveColor = Vector3(glowColor[0], glowColor[1], glowColor[2]);
      materialData.extractedEmissiveIntensity = 1.0f;
    }

    // Store emissive in D3D material for downstream detection
    materialData.d3dMaterial.Emissive.r = materialData.extractedEmissiveColor[0];
    materialData.d3dMaterial.Emissive.g = materialData.extractedEmissiveColor[1];
    materialData.d3dMaterial.Emissive.b = materialData.extractedEmissiveColor[2];
    materialData.d3dMaterial.Emissive.a = materialData.extractedEmissiveIntensity > 0.0f ? 1.0f : 0.0f;

    // Populate existing tt* fields for backward compatibility with the existing as<OpaqueMaterialData>() path
    if (materialData.extractedRoughness >= 0.0f)
      materialData.ttRoughnessConstant = materialData.extractedRoughness;
    materialData.ttSpecularIntensity = materialData.extractedSpecularPower;
    float glowLum = materialData.extractedEmissiveColor[0] * 0.299f
                  + materialData.extractedEmissiveColor[1] * 0.587f
                  + materialData.extractedEmissiveColor[2] * 0.114f;
    materialData.ttGlowIntensity = glowLum > 0.0f ? materialData.extractedEmissiveIntensity : -1.0f;
  }
  // NV-DXVK end
}
