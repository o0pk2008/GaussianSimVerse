// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

/** Thread group size shared by all GaussianSimVerse compute shaders. */
static constexpr uint32 GaussianThreadGroupSize = 64;

class GAUSSIANSIMVERSE_API FGaussianFrameworkCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGaussianFrameworkCS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianFrameworkCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, SceneCount)
		SHADER_PARAMETER(uint32, GaussianCount)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWFrameworkCounter)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};

class GAUSSIANSIMVERSE_API FGaussianCullCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGaussianCullCS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianCullCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FMatrix44f, LocalToWorldMatrix)
		SHADER_PARAMETER(FVector3f, PreViewTranslation)
		SHADER_PARAMETER(FMatrix44f, TranslatedWorldToClip)
		SHADER_PARAMETER(uint32, GaussianCount)
		SHADER_PARAMETER(float, FrustumMargin)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, GaussianPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWVisibleIndices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWVisibleCount)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};

class GAUSSIANSIMVERSE_API FGaussianSortKeysCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGaussianSortKeysCS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianSortKeysCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FMatrix44f, LocalToWorldMatrix)
		SHADER_PARAMETER(FVector3f, PreViewTranslation)
		SHADER_PARAMETER(FMatrix44f, TranslatedWorldToClip)
	SHADER_PARAMETER(uint32, MaxVisibleCount)
	SHADER_PARAMETER(uint32, PaddedCount)
	SHADER_PARAMETER(uint32, ChunkIndex)
	SHADER_PARAMETER(uint32, bUseGlobalIndexPayload)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ChunkWriteBases)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, VisibleIndices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, VisibleCountBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, GaussianPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint2>, RWSortKeys)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};

class GAUSSIANSIMVERSE_API FGaussianBitonicSortCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGaussianBitonicSortCS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianBitonicSortCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, K)
		SHADER_PARAMETER(uint32, J)
		SHADER_PARAMETER(uint32, PaddedCount)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint2>, RWSortKeys)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};

class GAUSSIANSIMVERSE_API FGaussianSortExtractCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGaussianSortExtractCS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianSortExtractCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, MaxVisibleCount)
		SHADER_PARAMETER(uint32, bReverseSortedIndices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, SortKeys)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, VisibleCountBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWSortedIndices)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};

class GAUSSIANSIMVERSE_API FGaussianConcatVisibleCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGaussianConcatVisibleCS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianConcatVisibleCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, ChunkIndex)
		SHADER_PARAMETER(uint32, MaxVisibleCount)
		SHADER_PARAMETER(uint32, BindingIndex)
		SHADER_PARAMETER(uint32, bCopyShCoefficients)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ChunkWriteBases)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, VisibleIndices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, VisibleCountBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, GaussianSplatsVec4)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, GaussianShCoeffs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RWUnifiedSplatsVec4)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWBindingIds)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, RWUnifiedShCoeffs)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};

class GAUSSIANSIMVERSE_API FGaussianGlobalUnifiedSortKeysCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGaussianGlobalUnifiedSortKeysCS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianGlobalUnifiedSortKeysCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, PaddedCount)
		SHADER_PARAMETER(FVector3f, CameraViewDirection)
		SHADER_PARAMETER(float, ViewDepthMin)
		SHADER_PARAMETER(float, ViewDepthMax)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, TotalVisibleCount)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, UnifiedSplatsVec4)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BindingIds)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, ChunkMatrixRows)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint2>, RWSortKeys)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};

class GAUSSIANSIMVERSE_API FGaussianClaimChunkWriteBaseCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGaussianClaimChunkWriteBaseCS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianClaimChunkWriteBaseCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, ChunkIndex)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, VisibleCountBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWChunkWriteBases)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWWriteCursor)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};

class GAUSSIANSIMVERSE_API FGaussianCopyChunkVisibleCountCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGaussianCopyChunkVisibleCountCS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianCopyChunkVisibleCountCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, ChunkIndex)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, VisibleCountBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWChunkVisibleCounts)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};

class GAUSSIANSIMVERSE_API FGaussianChunkVisiblePrefixSumCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGaussianChunkVisiblePrefixSumCS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianChunkVisiblePrefixSumCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, NumChunks)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ChunkVisibleCounts)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWChunkWriteBases)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWTotalVisibleCount)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};

class GAUSSIANSIMVERSE_API FGaussianClearSortKeysCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGaussianClearSortKeysCS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianClearSortKeysCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, KeyCount)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint2>, RWSortKeys)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};

/** 4-bit digit histogram for GPU radix sort (3DGS.cpp / VkRadixSort style). */
class GAUSSIANSIMVERSE_API FGaussianRadixCountCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGaussianRadixCountCS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianRadixCountCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, ElementCount)
		SHADER_PARAMETER(uint32, DigitShift)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, SortKeys)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWHistogram)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};

class GAUSSIANSIMVERSE_API FGaussianRadixPrefixSumCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGaussianRadixPrefixSumCS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianRadixPrefixSumCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, Histogram)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWScatterBase)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};

class GAUSSIANSIMVERSE_API FGaussianRadixScatterCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGaussianRadixScatterCS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianRadixScatterCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, ElementCount)
		SHADER_PARAMETER(uint32, DigitShift)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, SortKeysIn)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWScatterBase)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint2>, RWSortKeysOut)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};

class GAUSSIANSIMVERSE_API FGaussianBinSplatsCountCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGaussianBinSplatsCountCS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianBinSplatsCountCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FMatrix44f, LocalToWorldMatrix)
		SHADER_PARAMETER(FVector3f, PreViewTranslation)
		SHADER_PARAMETER(FMatrix44f, TranslatedViewMatrix)
		SHADER_PARAMETER(FMatrix44f, TranslatedWorldToClip)
		SHADER_PARAMETER(FVector4f, ViewportRect)
		SHADER_PARAMETER(FVector4f, ViewRect)
		SHADER_PARAMETER(uint32, SortedCount)
		SHADER_PARAMETER(uint32, GaussianCount)
		SHADER_PARAMETER(uint32, NumTilesX)
		SHADER_PARAMETER(uint32, NumTilesY)
		SHADER_PARAMETER(float, SplatScale)
		SHADER_PARAMETER(float, GaussianAlphaCullThreshold)
		SHADER_PARAMETER(float, GaussianCutoffK)
		SHADER_PARAMETER(float, GaussianCovarianceDilation)
		SHADER_PARAMETER(float, GaussianMinSigmaPixels)
		SHADER_PARAMETER(uint32, MaxRasterRadius)
		SHADER_PARAMETER(uint32, bDebugOverlay)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, SortedIndices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, VisibleCountBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, GaussianSplatsVec4)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWTileCounts)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};

class GAUSSIANSIMVERSE_API FGaussianTilePrefixSumCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGaussianTilePrefixSumCS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianTilePrefixSumCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, NumTiles)
		SHADER_PARAMETER(uint32, MaxTileSplats)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, TileCounts)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWTileOffsets)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWTotalTileSplats)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};

class GAUSSIANSIMVERSE_API FGaussianBinSplatsFillCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGaussianBinSplatsFillCS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianBinSplatsFillCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FMatrix44f, LocalToWorldMatrix)
		SHADER_PARAMETER(FVector3f, PreViewTranslation)
		SHADER_PARAMETER(FMatrix44f, TranslatedViewMatrix)
		SHADER_PARAMETER(FMatrix44f, TranslatedWorldToClip)
		SHADER_PARAMETER(FVector4f, ViewportRect)
		SHADER_PARAMETER(FVector4f, ViewRect)
		SHADER_PARAMETER(uint32, SortedCount)
		SHADER_PARAMETER(uint32, GaussianCount)
		SHADER_PARAMETER(uint32, NumTilesX)
		SHADER_PARAMETER(uint32, NumTilesY)
		SHADER_PARAMETER(uint32, MaxTileSplats)
		SHADER_PARAMETER(float, SplatScale)
		SHADER_PARAMETER(float, GaussianAlphaCutoff)
		SHADER_PARAMETER(float, GaussianAlphaCullThreshold)
		SHADER_PARAMETER(float, GaussianCutoffK)
		SHADER_PARAMETER(float, GaussianCovarianceDilation)
		SHADER_PARAMETER(float, GaussianMinSigmaPixels)
		SHADER_PARAMETER(uint32, MaxRasterRadius)
		SHADER_PARAMETER(uint32, bDebugOverlay)
		SHADER_PARAMETER(FVector3f, CameraViewDirection)
		SHADER_PARAMETER(float, ViewDepthMin)
		SHADER_PARAMETER(float, ViewDepthMax)
		SHADER_PARAMETER(uint32, bUseGlobalChunkLookup)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, SortedIndices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, VisibleCountBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BindingIds)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, ChunkMatrixRows)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, ChunkBindingParams)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, GaussianSplatsVec4)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, TileOffsets)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWTileFillCounters)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint2>, RWTileSplats)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};

class GAUSSIANSIMVERSE_API FGaussianTileSortCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGaussianTileSortCS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianTileSortCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, NumTilesX)
		SHADER_PARAMETER(uint32, NumTilesY)
		SHADER_PARAMETER(uint32, MaxTileSplats)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, TileOffsets)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, TileCounts)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint2>, RWTileSplats)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};

class GAUSSIANSIMVERSE_API FGaussianTileBlendCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGaussianTileBlendCS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianTileBlendCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FMatrix44f, LocalToWorldMatrix)
		SHADER_PARAMETER(FVector3f, PreViewTranslation)
		SHADER_PARAMETER(FMatrix44f, TranslatedViewMatrix)
		SHADER_PARAMETER(FMatrix44f, TranslatedWorldToClip)
		SHADER_PARAMETER(FVector4f, ViewportRect)
		SHADER_PARAMETER(FVector4f, ViewRect)
		SHADER_PARAMETER(uint32, NumTilesX)
		SHADER_PARAMETER(uint32, NumTilesY)
		SHADER_PARAMETER(uint32, NumTiles)
		SHADER_PARAMETER(uint32, GaussianCount)
		SHADER_PARAMETER(uint32, MaxTileSplats)
		SHADER_PARAMETER(float, SplatScale)
		SHADER_PARAMETER(float, GaussianAlphaCutoff)
		SHADER_PARAMETER(float, GaussianAlphaCullThreshold)
		SHADER_PARAMETER(float, GaussianCutoffK)
		SHADER_PARAMETER(float, GaussianCovarianceDilation)
		SHADER_PARAMETER(float, GaussianMinSigmaPixels)
		SHADER_PARAMETER(uint32, MaxRasterRadius)
		SHADER_PARAMETER(uint32, bDebugOverlay)
		SHADER_PARAMETER(uint32, RenderShDegree)
		SHADER_PARAMETER(uint32, ImportedShDegree)
		SHADER_PARAMETER(uint32, bHasShCoefficients)
		SHADER_PARAMETER(FVector3f, CameraWorldPosition)
		SHADER_PARAMETER(uint32, bUseGlobalChunkLookup)
		SHADER_PARAMETER(FVector3f, GaussianClrOffset)
		SHADER_PARAMETER(FVector3f, GaussianClrScaleRGB)
		SHADER_PARAMETER(float, GaussianSaturation)
		SHADER_PARAMETER(float, GaussianTransparencyMultiplier)
		SHADER_PARAMETER(uint32, StreamingDebugRenderMode)
		SHADER_PARAMETER(uint32, bAfterTonemap)
		SHADER_PARAMETER(uint32, bDepthOcclusion)
		SHADER_PARAMETER(float, DepthOcclusionBias)
		SHADER_PARAMETER(FVector4f, InvDeviceZToWorldZTransform)
		SHADER_PARAMETER(FIntPoint, SceneColorOffset)
		SHADER_PARAMETER(FIntPoint, SceneDepthOffset)
		SHADER_PARAMETER(uint32, bUseProxyStencilExclude)
		SHADER_PARAMETER(uint32, ProxyStencilExclude)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, TileOffsets)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, TileCounts)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, TileSplats)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, SortedIndices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BindingIds)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, ChunkMatrixRows)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, ChunkBindingParams)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, GaussianSplatsVec4)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, GaussianShCoeffs)
		SHADER_PARAMETER(uint32, bWriteSoftDepth)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, CustomStencilTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWOverlay)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, RWSoftDepthBits)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};

/** Shared uniforms for instanced splat quad draw (VS + PS). */
BEGIN_SHADER_PARAMETER_STRUCT(FGaussianSplatDrawSharedParameters, )
	SHADER_PARAMETER(FMatrix44f, LocalToWorldMatrix)
	SHADER_PARAMETER(FVector3f, PreViewTranslation)
	SHADER_PARAMETER(FMatrix44f, TranslatedViewMatrix)
	SHADER_PARAMETER(FMatrix44f, TranslatedWorldToClip)
	SHADER_PARAMETER(FVector4f, ViewportRect)
	SHADER_PARAMETER(FVector4f, ViewRect)
	SHADER_PARAMETER(uint32, BatchStart)
	SHADER_PARAMETER(uint32, BatchCount)
	SHADER_PARAMETER(uint32, SortedCount)
	SHADER_PARAMETER(uint32, GaussianCount)
	SHADER_PARAMETER(float, SplatScale)
	SHADER_PARAMETER(float, GaussianAlphaCutoff)
	SHADER_PARAMETER(float, GaussianAlphaCullThreshold)
	SHADER_PARAMETER(float, GaussianCutoffK)
	SHADER_PARAMETER(float, GaussianCovarianceDilation)
	SHADER_PARAMETER(float, GaussianMinSigmaPixels)
	SHADER_PARAMETER(uint32, MaxRasterRadius)
	SHADER_PARAMETER(uint32, bDebugOverlay)
	SHADER_PARAMETER(uint32, RenderShDegree)
	SHADER_PARAMETER(uint32, ImportedShDegree)
	SHADER_PARAMETER(uint32, bHasShCoefficients)
	SHADER_PARAMETER(FVector3f, CameraWorldPosition)
	SHADER_PARAMETER(FVector3f, GaussianClrOffset)
	SHADER_PARAMETER(FVector3f, GaussianClrScaleRGB)
	SHADER_PARAMETER(float, GaussianSaturation)
	SHADER_PARAMETER(float, GaussianTransparencyMultiplier)
	SHADER_PARAMETER(uint32, StreamingDebugRenderMode)
	SHADER_PARAMETER(uint32, bAfterTonemap)
	SHADER_PARAMETER(uint32, bUseGlobalChunkLookup)
	SHADER_PARAMETER(uint32, bDepthOcclusion)
	SHADER_PARAMETER(float, DepthOcclusionBias)
	SHADER_PARAMETER(FVector4f, InvDeviceZToWorldZTransform)
	SHADER_PARAMETER(FIntPoint, SceneColorOffset)
	SHADER_PARAMETER(FIntPoint, SceneDepthOffset)
	SHADER_PARAMETER(uint32, bUseProxyStencilExclude)
	SHADER_PARAMETER(uint32, ProxyStencilExclude)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, SortedIndices)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, VisibleCountBuffer)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BindingIds)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, ChunkMatrixRows)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, ChunkBindingParams)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, GaussianSplatsVec4)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, GaussianShCoeffs)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, CustomStencilTexture)
END_SHADER_PARAMETER_STRUCT()

/** Instanced screen-space splat quads — hardware raster (SuperSplat-style global draw). */
class GAUSSIANSIMVERSE_API FGaussianSplatDrawVS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGaussianSplatDrawVS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianSplatDrawVS, FGlobalShader);

	using FParameters = FGaussianSplatDrawSharedParameters;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};

class GAUSSIANSIMVERSE_API FGaussianSplatDrawPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGaussianSplatDrawPS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianSplatDrawPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FGaussianSplatDrawSharedParameters, Shared)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};

class GAUSSIANSIMVERSE_API FGaussianRasterCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGaussianRasterCS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianRasterCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FMatrix44f, LocalToWorldMatrix)
		SHADER_PARAMETER(FVector3f, PreViewTranslation)
		SHADER_PARAMETER(FMatrix44f, TranslatedViewMatrix)
		SHADER_PARAMETER(FMatrix44f, TranslatedWorldToClip)
		SHADER_PARAMETER(FVector4f, ViewportRect)
		SHADER_PARAMETER(FVector4f, ViewRect)
		SHADER_PARAMETER(uint32, BatchStart)
		SHADER_PARAMETER(uint32, BatchCount)
		SHADER_PARAMETER(uint32, SortedCount)
		SHADER_PARAMETER(uint32, GaussianCount)
		SHADER_PARAMETER(float, SplatScale)
		SHADER_PARAMETER(float, GaussianAlphaCutoff)
		SHADER_PARAMETER(float, GaussianAlphaCullThreshold)
		SHADER_PARAMETER(float, GaussianCutoffK)
		SHADER_PARAMETER(float, GaussianCovarianceDilation)
		SHADER_PARAMETER(float, GaussianMinSigmaPixels)
		SHADER_PARAMETER(uint32, MaxRasterRadius)
		SHADER_PARAMETER(uint32, bDebugOverlay)
		SHADER_PARAMETER(uint32, RenderShDegree)
		SHADER_PARAMETER(uint32, ImportedShDegree)
		SHADER_PARAMETER(uint32, bHasShCoefficients)
		SHADER_PARAMETER(FVector3f, CameraWorldPosition)
		SHADER_PARAMETER(FVector3f, GaussianClrOffset)
		SHADER_PARAMETER(FVector3f, GaussianClrScaleRGB)
		SHADER_PARAMETER(float, GaussianSaturation)
		SHADER_PARAMETER(float, GaussianTransparencyMultiplier)
		SHADER_PARAMETER(uint32, StreamingDebugRenderMode)
		SHADER_PARAMETER(uint32, bAfterTonemap)
		SHADER_PARAMETER(uint32, bDepthOcclusion)
		SHADER_PARAMETER(float, DepthOcclusionBias)
		SHADER_PARAMETER(FVector4f, InvDeviceZToWorldZTransform)
		SHADER_PARAMETER(FIntPoint, SceneColorOffset)
		SHADER_PARAMETER(FIntPoint, SceneDepthOffset)
		SHADER_PARAMETER(uint32, bUseProxyStencilExclude)
		SHADER_PARAMETER(uint32, ProxyStencilExclude)
		SHADER_PARAMETER(uint32, bWriteSoftDepth)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, SortedIndices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, VisibleCountBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, GaussianSplatsVec4)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, GaussianShCoeffs)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, CustomStencilTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWOverlay)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, RWSoftDepthBits)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};

class GAUSSIANSIMVERSE_API FGaussianResolveCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGaussianResolveCS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianResolveCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FVector4f, ViewRect)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, AccumAlpha)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, AccumPremulR)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, AccumPremulG)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, AccumPremulB)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWOverlay)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};

/**
 * Fullscreen: write SceneDepth from CustomDepth where CustomStencil matches the proxy stencil.
 * Color RT is a dummy (CW_NONE) so D3D12 accepts the graphics PSO.
 */
class GAUSSIANSIMVERSE_API FGaussianProxyDepthMergePS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGaussianProxyDepthMergePS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianProxyDepthMergePS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CustomDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, CustomStencilTexture)
		SHADER_PARAMETER(uint32, ProxyStencil)
		SHADER_PARAMETER(uint32, bHasCustomStencil)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};

/** Fullscreen: write SceneDepth from gaussian soft DeviceZ bits (reverse-Z, 0 = empty). */
class GAUSSIANSIMVERSE_API FGaussianSoftDepthMergePS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGaussianSoftDepthMergePS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianSoftDepthMergePS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, SoftDepthBitsTexture)
		SHADER_PARAMETER(FIntPoint, SoftDepthOffset)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};

/** Separable CoC blur: CustomDepth (proxy stencil) / SceneDepth hybrid — no early SceneDepth write. */
class GAUSSIANSIMVERSE_API FGaussianDofCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGaussianDofCS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianDofCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FVector4f, ViewRect)
		SHADER_PARAMETER(FIntPoint, TextureSize)
		SHADER_PARAMETER(FVector2f, BlurDirection)
		SHADER_PARAMETER(float, FocalDistanceCm)
		SHADER_PARAMETER(float, CocScale)
		SHADER_PARAMETER(float, MaxBlurRadiusPx)
		SHADER_PARAMETER(FVector4f, InvDeviceZToWorldZTransform)
		SHADER_PARAMETER(uint32, ProxyStencil)
		SHADER_PARAMETER(uint32, bHasCustomStencil)
		SHADER_PARAMETER(uint32, bHasCustomDepth)
		SHADER_PARAMETER(uint32, bHasSceneDepth)
		SHADER_PARAMETER(uint32, bHasSoftDepth)
		SHADER_PARAMETER(uint32, bMaskByOverlayAlpha)
		SHADER_PARAMETER(FIntPoint, SoftDepthOffset)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CustomDepthTexture)
		// Prefer Texture2D (not uint2) so binding PF_X24_G8 / dummy views does not mismatch HLSL type.
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, CustomStencilTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, SoftDepthBitsTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, OverlayTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWOutput)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};

class GAUSSIANSIMVERSE_API FGaussianCompositeCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGaussianCompositeCS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianCompositeCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FVector4f, ViewRect)
		SHADER_PARAMETER(FIntPoint, SceneColorOffset)
		SHADER_PARAMETER(uint32, bRelightEnabled)
		SHADER_PARAMETER(uint32, bRelightDebug)
		SHADER_PARAMETER(uint32, bAfterTonemap)
		SHADER_PARAMETER(float, PreExposure)
		SHADER_PARAMETER(float, RelightBlend)
		SHADER_PARAMETER(float, RelightExposure)
		SHADER_PARAMETER(float, RelightBrightness)
		SHADER_PARAMETER(float, RelightBackground)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, OverlayTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, RelightTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, RelightSampler)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWSceneColor)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};
