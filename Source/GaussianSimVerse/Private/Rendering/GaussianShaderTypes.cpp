// Copyright Epic Games, Inc. All Rights Reserved.

#include "Rendering/GaussianShaderTypes.h"
#include "ShaderParameterUtils.h"
#include "DataDrivenShaderPlatformInfo.h"

#define IMPLEMENT_GAUSSIAN_CS(ShaderClass, ShaderPath) \
	IMPLEMENT_GLOBAL_SHADER(ShaderClass, ShaderPath, "MainCS", SF_Compute); \
	bool ShaderClass::ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) \
	{ \
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5); \
	} \
	void ShaderClass::ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment) \
	{ \
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment); \
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), GaussianThreadGroupSize); \
		OutEnvironment.CompilerFlags.Add(CFLAG_StandardOptimization); \
	}

IMPLEMENT_GAUSSIAN_CS(FGaussianFrameworkCS, "/Plugin/GaussianSimVerse/Private/FrameworkCS.usf")
IMPLEMENT_GAUSSIAN_CS(FGaussianCullCS, "/Plugin/GaussianSimVerse/Private/CullCS.usf")
IMPLEMENT_GAUSSIAN_CS(FGaussianSortKeysCS, "/Plugin/GaussianSimVerse/Private/SortKeysCS.usf")
IMPLEMENT_GAUSSIAN_CS(FGaussianBitonicSortCS, "/Plugin/GaussianSimVerse/Private/BitonicSortCS.usf")
IMPLEMENT_GAUSSIAN_CS(FGaussianSortExtractCS, "/Plugin/GaussianSimVerse/Private/SortExtractCS.usf")
IMPLEMENT_GAUSSIAN_CS(FGaussianRadixCountCS, "/Plugin/GaussianSimVerse/Private/RadixCountCS.usf")
IMPLEMENT_GAUSSIAN_CS(FGaussianRadixPrefixSumCS, "/Plugin/GaussianSimVerse/Private/RadixPrefixSumCS.usf")
IMPLEMENT_GAUSSIAN_CS(FGaussianRadixScatterCS, "/Plugin/GaussianSimVerse/Private/RadixScatterCS.usf")
IMPLEMENT_GAUSSIAN_CS(FGaussianRasterCS, "/Plugin/GaussianSimVerse/Private/RasterCS.usf")
IMPLEMENT_GAUSSIAN_CS(FGaussianResolveCS, "/Plugin/GaussianSimVerse/Private/ResolveCS.usf")

IMPLEMENT_GLOBAL_SHADER(FGaussianBinSplatsCountCS, "/Plugin/GaussianSimVerse/Private/BinSplatsCountCS.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGaussianBinSplatsFillCS, "/Plugin/GaussianSimVerse/Private/BinSplatsFillCS.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGaussianTilePrefixSumCS, "/Plugin/GaussianSimVerse/Private/TilePrefixSumCS.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGaussianTileSortCS, "/Plugin/GaussianSimVerse/Private/TileSortCS.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGaussianTileBlendCS, "/Plugin/GaussianSimVerse/Private/TileBlendCS.usf", "MainCS", SF_Compute);

bool FGaussianBinSplatsCountCS::ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
{
	return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
}

void FGaussianBinSplatsCountCS::ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), GaussianThreadGroupSize);
	OutEnvironment.SetDefine(TEXT("GAUSSIAN_TILE_SIZE"), 16);
	OutEnvironment.SetDefine(TEXT("GAUSSIAN_MAX_SPLATS_PER_TILE"), 16384);
	OutEnvironment.CompilerFlags.Add(CFLAG_StandardOptimization);
}

bool FGaussianBinSplatsFillCS::ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
{
	return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
}

void FGaussianBinSplatsFillCS::ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), GaussianThreadGroupSize);
	OutEnvironment.SetDefine(TEXT("GAUSSIAN_TILE_SIZE"), 16);
	OutEnvironment.SetDefine(TEXT("GAUSSIAN_MAX_SPLATS_PER_TILE"), 16384);
	OutEnvironment.CompilerFlags.Add(CFLAG_StandardOptimization);
}

bool FGaussianTilePrefixSumCS::ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
{
	return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
}

void FGaussianTilePrefixSumCS::ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	OutEnvironment.CompilerFlags.Add(CFLAG_StandardOptimization);
}

bool FGaussianTileSortCS::ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
{
	return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
}

void FGaussianTileSortCS::ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	OutEnvironment.SetDefine(TEXT("GAUSSIAN_TILE_SIZE"), 16);
	OutEnvironment.SetDefine(TEXT("GAUSSIAN_MAX_SPLATS_PER_TILE"), 16384);
	OutEnvironment.CompilerFlags.Add(CFLAG_StandardOptimization);
}

bool FGaussianTileBlendCS::ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
{
	return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
}

void FGaussianTileBlendCS::ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	OutEnvironment.SetDefine(TEXT("GAUSSIAN_TILE_SIZE"), 16);
	OutEnvironment.SetDefine(TEXT("GAUSSIAN_MAX_SPLATS_PER_TILE"), 16384);
	OutEnvironment.SetDefine(TEXT("GAUSSIAN_TILE_BLEND_BATCH"), 1024);
	OutEnvironment.CompilerFlags.Add(CFLAG_StandardOptimization);
}

IMPLEMENT_GLOBAL_SHADER(FGaussianCompositeCS, "/Plugin/GaussianSimVerse/Private/CompositeCS.usf", "MainCS", SF_Compute);

bool FGaussianCompositeCS::ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
{
	return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
}

void FGaussianCompositeCS::ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	OutEnvironment.CompilerFlags.Add(CFLAG_StandardOptimization);
}
