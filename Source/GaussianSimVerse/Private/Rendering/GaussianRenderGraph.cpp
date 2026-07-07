// Copyright Epic Games, Inc. All Rights Reserved.

#include "Rendering/GaussianRenderGraph.h"
#include "Rendering/GaussianShaderTypes.h"
#include "Rendering/GaussianRenderSettings.h"
#include "Rendering/GaussianGPUResources.h"
#include "GaussianSimVerse.h"
#include "SceneView.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"
#include "GlobalShader.h"
#include "PipelineStateCache.h"
#include "RHIStaticStates.h"
#include "CommonRenderResources.h"

namespace GaussianRenderGraphPrivate
{
	static constexpr uint32 SplatsPerRasterBatch = 2048;
	static constexpr uint32 SplatsPerDrawBatch = 65535;

	static void FillGaussianColorGradeUniforms(
		FVector3f& OutClrOffset,
		FVector3f& OutClrScaleRGB,
		float& OutSaturation,
		float& OutTransparencyMultiplier,
		const FGaussianColorGradeGPU& ColorGrade)
	{
		OutClrOffset = ColorGrade.ClrOffset;
		OutClrScaleRGB = ColorGrade.ClrScaleRGB;
		OutSaturation = ColorGrade.Saturation;
		OutTransparencyMultiplier = ColorGrade.TransparencyMultiplier;
	}

	static void FillGaussianSplatDrawParameters(
		FGaussianSplatDrawSharedParameters& OutParameters,
		const FMatrix44f& LocalToWorldMatrix,
		const FVector3f& PreViewTranslation,
		const FMatrix44f& TranslatedViewMatrix,
		const FMatrix44f& TranslatedWorldToClip,
		const FVector4f& ViewportRect,
		const FVector4f& ViewRect,
		uint32 BatchStart,
		uint32 BatchCount,
		uint32 SortedCount,
		uint32 GaussianCount,
		float SplatScale,
		float GaussianAlphaCutoff,
		float GaussianAlphaCullThreshold,
		float GaussianCutoffK,
		float GaussianCovarianceDilation,
		float GaussianMinSigmaPixels,
		uint32 MaxRasterRadius,
		bool bDebugOverlay,
		uint32 RenderShDegree,
		uint32 ImportedShDegree,
		uint32 bHasShCoefficients,
		const FVector3f& CameraWorldPosition,
		const FGaussianColorGradeGPU& ColorGrade,
		FRDGBufferSRVRef SortedIndicesSRV,
		FRDGBufferSRVRef VisibleCountSRV,
		FRDGBufferSRVRef GaussianSplatsSRV,
		FRDGBufferSRVRef GaussianShCoeffsSRV)
	{
		OutParameters.LocalToWorldMatrix = LocalToWorldMatrix;
		OutParameters.PreViewTranslation = PreViewTranslation;
		OutParameters.TranslatedViewMatrix = TranslatedViewMatrix;
		OutParameters.TranslatedWorldToClip = TranslatedWorldToClip;
		OutParameters.ViewportRect = ViewportRect;
		OutParameters.ViewRect = ViewRect;
		OutParameters.BatchStart = BatchStart;
		OutParameters.BatchCount = BatchCount;
		OutParameters.SortedCount = SortedCount;
		OutParameters.GaussianCount = GaussianCount;
		OutParameters.SplatScale = SplatScale;
		OutParameters.GaussianAlphaCutoff = GaussianAlphaCutoff;
		OutParameters.GaussianAlphaCullThreshold = GaussianAlphaCullThreshold;
		OutParameters.GaussianCutoffK = GaussianCutoffK;
		OutParameters.GaussianCovarianceDilation = GaussianCovarianceDilation;
		OutParameters.GaussianMinSigmaPixels = GaussianMinSigmaPixels;
		OutParameters.MaxRasterRadius = MaxRasterRadius;
		OutParameters.bDebugOverlay = bDebugOverlay ? 1u : 0u;
		OutParameters.RenderShDegree = RenderShDegree;
		OutParameters.ImportedShDegree = ImportedShDegree;
		OutParameters.bHasShCoefficients = bHasShCoefficients;
		OutParameters.CameraWorldPosition = CameraWorldPosition;
		FillGaussianColorGradeUniforms(
			OutParameters.GaussianClrOffset,
			OutParameters.GaussianClrScaleRGB,
			OutParameters.GaussianSaturation,
			OutParameters.GaussianTransparencyMultiplier,
			ColorGrade);
		OutParameters.SortedIndices = SortedIndicesSRV;
		OutParameters.VisibleCountBuffer = VisibleCountSRV;
		OutParameters.GaussianSplatsVec4 = GaussianSplatsSRV;
		OutParameters.GaussianShCoeffs = GaussianShCoeffsSRV;
	}

	static void AddGaussianSplatDrawPass(
		FRDGBuilder& GraphBuilder,
		FRDGEventName&& EventName,
		const FGlobalShaderMap* GlobalShaderMap,
		FGaussianSplatDrawPS::FParameters* PassParameters,
		const FIntPoint& ViewportSize,
		uint32 NumInstances)
	{
		if (NumInstances == 0)
		{
			return;
		}

		TShaderMapRef<FGaussianSplatDrawVS> VertexShader(GlobalShaderMap);
		TShaderMapRef<FGaussianSplatDrawPS> PixelShader(GlobalShaderMap);
		if (!VertexShader.IsValid() || !PixelShader.IsValid())
		{
			return;
		}

		GraphBuilder.AddPass(
			Forward<FRDGEventName>(EventName),
			PassParameters,
			ERDGPassFlags::Raster,
			[PassParameters, VertexShader, PixelShader, ViewportSize, NumInstances](FRHICommandList& RHICmdList)
			{
				FGraphicsPipelineStateInitializer GraphicsPSOInit;
				RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
				GraphicsPSOInit.PrimitiveType = PT_TriangleList;
				GraphicsPSOInit.BlendState = TStaticBlendState<
					CW_RGBA,
					BO_Add, BF_One, BF_InverseSourceAlpha,
					BO_Add, BF_One, BF_InverseSourceAlpha>::GetRHI();
				GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
				GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
				GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
				GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
				GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();

				SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
				SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), PassParameters->Shared);
				SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *PassParameters);

				RHICmdList.SetViewport(
					0.0f, 0.0f, 0.0f,
					static_cast<float>(ViewportSize.X), static_cast<float>(ViewportSize.Y), 1.0f);
				RHICmdList.DrawPrimitive(0, 2, NumInstances);
			});
	}

	static uint32 GetPaddedSortCount(uint32 VisibleCount)
	{
		const int32 MaxSort = FMath::Max(2, GaussianSimVerse::RenderSettings::CVarMaxSortElements.GetValueOnRenderThread());
		const uint32 ClampedCount = FMath::Min(VisibleCount, static_cast<uint32>(MaxSort));
		return FMath::RoundUpToPowerOfTwo(ClampedCount);
	}
}

FGaussianRDGTransientResources FGaussianRenderGraph::AllocateTransientResources(FRDGBuilder& GraphBuilder)
{
	FGaussianRDGTransientResources Resources;

	FRDGBufferDesc CounterDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1);
	CounterDesc.Usage = EBufferUsageFlags::Static | EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::ShaderResource;
	Resources.FrameworkCounterBuffer = GraphBuilder.CreateBuffer(CounterDesc, TEXT("Gaussian.FrameworkCounter"));
	Resources.FrameworkCounterUAV = GraphBuilder.CreateUAV(Resources.FrameworkCounterBuffer);

	return Resources;
}

void FGaussianRenderGraph::AddPasses(
	FRDGBuilder& GraphBuilder,
	const FPassInputs& Inputs,
	FGaussianRDGTransientResources& TransientResources)
{
	if (!GaussianSimVerse::RenderSettings::IsRenderingEnabled())
	{
		return;
	}

	AddGPUBufferUploadPasses(GraphBuilder, Inputs, TransientResources);

	if (GaussianSimVerse::RenderSettings::IsCullEnabled())
	{
		AddGPUFrustumCullPasses(GraphBuilder, Inputs, TransientResources);
	}
	else
	{
		AddPassthroughCullResults(GraphBuilder, TransientResources);
	}

	if (GaussianSimVerse::RenderSettings::IsSortEnabled())
	{
		AddGPUDepthSortPasses(GraphBuilder, Inputs, TransientResources);
	}

	if (GaussianSimVerse::RenderSettings::IsRasterEnabled() && Inputs.SceneColorTexture)
	{
		AddGPURasterPasses(GraphBuilder, Inputs, TransientResources);
	}

	AddFrameworkValidationPass(GraphBuilder, Inputs, TransientResources);
}

void FGaussianRenderGraph::AddGPUBufferUploadPasses(
	FRDGBuilder& GraphBuilder,
	const FPassInputs& Inputs,
	FGaussianRDGTransientResources& TransientResources)
{
	TransientResources.GPUBuffers.Reset();
	TransientResources.CullResults.Reset();
	TransientResources.SortResults.Reset();
	TransientResources.UploadedGaussianCount = 0;
	TransientResources.TotalCulledVisibleCount = 0;

	for (const FGaussianSceneProxy& SceneProxy : Inputs.FrameResources.SceneProxies)
	{
		for (const FGaussianChunkRenderData& Chunk : SceneProxy.Chunks)
		{
			if (!Chunk.GPUBuffer || !Chunk.GPUBuffer->HasValidData())
			{
				continue;
			}

			FGaussianRDGBufferBinding Binding;
			Binding.SourceBuffer = Chunk.GPUBuffer;
			Binding.SceneId = SceneProxy.SceneId;
			Binding.ChunkIndex = Chunk.ChunkIndex;
			Binding.LocalToWorld = Chunk.LocalToWorld;
			// Actor scale * global CVar (default 1.0 matches SuperSplat footprint).
			Binding.SplatScale = SceneProxy.SplatScale * GaussianSimVerse::RenderSettings::GetSplatScale();
			Binding.AlphaCullThreshold = SceneProxy.AlphaCullThreshold;
			Binding.CutoffK = SceneProxy.CutoffK;
			Binding.CovarianceDilation = SceneProxy.CovarianceDilation;
			Binding.RenderShDegree = static_cast<uint32>(SceneProxy.ShBand);
			Binding.ColorGrade = SceneProxy.ColorGrade;

			Chunk.GPUBuffer->CommitToGPU(GraphBuilder, Binding);

			if (Binding.SplatBuffer && Binding.PositionSRV)
			{
				TransientResources.UploadedGaussianCount += Binding.NumGaussians;
				TransientResources.GPUBuffers.Add(MoveTemp(Binding));
			}
		}
	}

	if (GaussianSimVerse::RenderSettings::IsRenderDebugEnabled()
		&& Inputs.FrameResources.SceneProxies.Num() > 0
		&& TransientResources.GPUBuffers.Num() == 0)
	{
		UE_LOG(LogGaussianSimVerse, Warning, TEXT("GaussianSimVerse: scene proxies present but no GPU buffers were uploaded."));
	}
}

void FGaussianRenderGraph::AddPassthroughCullResults(
	FRDGBuilder& GraphBuilder,
	FGaussianRDGTransientResources& TransientResources)
{
	TransientResources.CullResults.Reserve(TransientResources.GPUBuffers.Num());

	for (int32 BindingIndex = 0; BindingIndex < TransientResources.GPUBuffers.Num(); ++BindingIndex)
	{
		const FGaussianRDGBufferBinding& Binding = TransientResources.GPUBuffers[BindingIndex];
		if (!Binding.PositionSRV || Binding.NumGaussians == 0)
		{
			continue;
		}

		TArray<uint32> Indices;
		Indices.SetNumUninitialized(Binding.NumGaussians);
		for (uint32 Index = 0; Index < Binding.NumGaussians; ++Index)
		{
			Indices[Index] = Index;
		}

		const TArray<uint32> VisibleCountArray = { Binding.NumGaussians };

		FRDGBufferRef VisibleIndicesBuffer = CreateStructuredUploadBuffer(
			GraphBuilder,
			*FString::Printf(TEXT("Gaussian.PassthroughIndices.S%d.C%d"), Binding.SceneId, Binding.ChunkIndex),
			Indices);

		FRDGBufferRef VisibleCountBuffer = CreateStructuredUploadBuffer(
			GraphBuilder,
			*FString::Printf(TEXT("Gaussian.PassthroughCount.S%d.C%d"), Binding.SceneId, Binding.ChunkIndex),
			VisibleCountArray);

		FGaussianRDGCullResult CullResult;
		CullResult.GPUBindingIndex = BindingIndex;
		CullResult.VisibleIndicesBuffer = VisibleIndicesBuffer;
		CullResult.VisibleIndicesSRV = GraphBuilder.CreateSRV(VisibleIndicesBuffer);
		CullResult.VisibleCountBuffer = VisibleCountBuffer;
		CullResult.VisibleCountSRV = GraphBuilder.CreateSRV(VisibleCountBuffer);
		CullResult.MaxVisibleCount = Binding.NumGaussians;
		TransientResources.CullResults.Add(MoveTemp(CullResult));
	}
}

void FGaussianRenderGraph::AddGPUFrustumCullPasses(
	FRDGBuilder& GraphBuilder,
	const FPassInputs& Inputs,
	FGaussianRDGTransientResources& TransientResources)
{
	if (TransientResources.GPUBuffers.Num() == 0)
	{
		return;
	}

	const FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	const TShaderMapRef<FGaussianCullCS> ComputeShader(GlobalShaderMap);
	if (!ComputeShader.IsValid())
	{
		return;
	}

	const float FrustumMargin = GaussianSimVerse::RenderSettings::GetFrustumMargin();
	const FVector3f PreViewTranslation(Inputs.ViewData.PreViewTranslation);
	const FMatrix44f TranslatedWorldToClip(Inputs.ViewData.TranslatedWorldToClip);

	TransientResources.CullResults.Reserve(TransientResources.GPUBuffers.Num());

	for (int32 BindingIndex = 0; BindingIndex < TransientResources.GPUBuffers.Num(); ++BindingIndex)
	{
		const FGaussianRDGBufferBinding& Binding = TransientResources.GPUBuffers[BindingIndex];
		if (!Binding.PositionSRV || Binding.NumGaussians == 0)
		{
			continue;
		}

		const uint32 NumGaussians = Binding.NumGaussians;
		const uint32 NumGroups = FMath::DivideAndRoundUp(NumGaussians, GaussianThreadGroupSize);

		FRDGBufferRef VisibleIndicesBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumGaussians),
			*FString::Printf(TEXT("Gaussian.VisibleIndices.S%d.C%d"), Binding.SceneId, Binding.ChunkIndex));

		FRDGBufferRef VisibleCountBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1),
			*FString::Printf(TEXT("Gaussian.VisibleCount.S%d.C%d"), Binding.SceneId, Binding.ChunkIndex));

		FRDGBufferUAVRef VisibleIndicesUAV = GraphBuilder.CreateUAV(VisibleIndicesBuffer);
		FRDGBufferUAVRef VisibleCountUAV = GraphBuilder.CreateUAV(VisibleCountBuffer);

		AddClearUAVPass(GraphBuilder, VisibleCountUAV, 0u);

		FGaussianCullCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGaussianCullCS::FParameters>();
		PassParameters->LocalToWorldMatrix = FMatrix44f(Binding.LocalToWorld);
		PassParameters->PreViewTranslation = PreViewTranslation;
		PassParameters->TranslatedWorldToClip = TranslatedWorldToClip;
		PassParameters->GaussianCount = NumGaussians;
		PassParameters->FrustumMargin = FrustumMargin;
		PassParameters->GaussianPositions = Binding.PositionSRV;
		PassParameters->RWVisibleIndices = VisibleIndicesUAV;
		PassParameters->RWVisibleCount = VisibleCountUAV;

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GaussianSimVerse::Cull S%u C%u (%u)", Binding.SceneId, Binding.ChunkIndex, NumGaussians),
			ERDGPassFlags::Compute,
			ComputeShader,
			PassParameters,
			FIntVector(NumGroups, 1, 1));

		FGaussianRDGCullResult CullResult;
		CullResult.GPUBindingIndex = BindingIndex;
		CullResult.VisibleIndicesBuffer = VisibleIndicesBuffer;
		CullResult.VisibleIndicesSRV = GraphBuilder.CreateSRV(VisibleIndicesBuffer);
		CullResult.VisibleCountBuffer = VisibleCountBuffer;
		CullResult.VisibleCountSRV = GraphBuilder.CreateSRV(VisibleCountBuffer);
		CullResult.MaxVisibleCount = NumGaussians;
		TransientResources.CullResults.Add(MoveTemp(CullResult));
	}
}

void FGaussianRenderGraph::AddGPUDepthSortPasses(
	FRDGBuilder& GraphBuilder,
	const FPassInputs& Inputs,
	FGaussianRDGTransientResources& TransientResources)
{
	if (TransientResources.CullResults.Num() == 0)
	{
		return;
	}

	const FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	const TShaderMapRef<FGaussianSortKeysCS> SortKeysShader(GlobalShaderMap);
	const TShaderMapRef<FGaussianBitonicSortCS> BitonicShader(GlobalShaderMap);
	const TShaderMapRef<FGaussianSortExtractCS> ExtractShader(GlobalShaderMap);
	const TShaderMapRef<FGaussianRadixCountCS> RadixCountShader(GlobalShaderMap);
	const TShaderMapRef<FGaussianRadixPrefixSumCS> RadixPrefixShader(GlobalShaderMap);
	const TShaderMapRef<FGaussianRadixScatterCS> RadixScatterShader(GlobalShaderMap);
	if (!SortKeysShader.IsValid() || !ExtractShader.IsValid())
	{
		return;
	}

	const bool bRadixAvailable = RadixCountShader.IsValid() && RadixPrefixShader.IsValid() && RadixScatterShader.IsValid();
	const bool bBitonicAvailable = BitonicShader.IsValid();
	const int32 SortMethod = GaussianSimVerse::RenderSettings::GetSortMethod();
	const bool bUseRadix = (SortMethod == 1) && bRadixAvailable;
	if (!bUseRadix && !bBitonicAvailable)
	{
		return;
	}

	TransientResources.SortResults.Reserve(TransientResources.CullResults.Num());

	for (int32 CullIndex = 0; CullIndex < TransientResources.CullResults.Num(); ++CullIndex)
	{
		const FGaussianRDGCullResult& CullResult = TransientResources.CullResults[CullIndex];
		if (!CullResult.VisibleIndicesSRV || !CullResult.VisibleCountSRV || CullResult.MaxVisibleCount == 0)
		{
			continue;
		}

		const FGaussianRDGBufferBinding& Binding = TransientResources.GPUBuffers[CullResult.GPUBindingIndex];
		const uint32 MaxVisibleCount = CullResult.MaxVisibleCount;
		const int32 MaxSortElements = FMath::Max(2, GaussianSimVerse::RenderSettings::CVarMaxSortElements.GetValueOnRenderThread());
		if (MaxVisibleCount > static_cast<uint32>(MaxSortElements))
		{
			static bool bLoggedSortSkipOnce = false;
			if (!bLoggedSortSkipOnce)
			{
				bLoggedSortSkipOnce = true;
				UE_LOG(LogGaussianSimVerse, Warning,
					TEXT("GaussianSimVerse: skipping depth sort for %u splats (limit %d). Raise r.GaussianSimVerse.MaxSortElements."),
					MaxVisibleCount, MaxSortElements);
			}
			continue;
		}

		// Radix sorts exactly N elements (no power-of-two pad). Bitonic still needs pot padding.
		const uint32 ElementCount = MaxVisibleCount;
		const uint32 KeyBufferCount = bUseRadix
			? ElementCount
			: GaussianRenderGraphPrivate::GetPaddedSortCount(ElementCount);
		if (KeyBufferCount < 1)
		{
			continue;
		}

		const FMatrix44f LocalToWorldMatrix(Binding.LocalToWorld);
		const FVector3f PreViewTranslation(Inputs.ViewData.PreViewTranslation);
		const FMatrix44f TranslatedViewMatrix(Inputs.ViewData.TranslatedViewMatrix);
		const FMatrix44f TranslatedWorldToClip(Inputs.ViewData.TranslatedWorldToClip);

		FRDGBufferRef SortKeysA = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FUintVector2), KeyBufferCount),
			*FString::Printf(TEXT("Gaussian.SortKeysA.S%d.C%d"), Binding.SceneId, Binding.ChunkIndex));
		FRDGBufferRef SortKeysB = bUseRadix
			? GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(FUintVector2), KeyBufferCount),
				*FString::Printf(TEXT("Gaussian.SortKeysB.S%d.C%d"), Binding.SceneId, Binding.ChunkIndex))
			: nullptr;

		FRDGBufferRef SortedIndicesBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), MaxVisibleCount),
			*FString::Printf(TEXT("Gaussian.SortedIndices.S%d.C%d"), Binding.SceneId, Binding.ChunkIndex));

		FRDGBufferUAVRef SortKeysAUAV = GraphBuilder.CreateUAV(SortKeysA);
		{
			FGaussianSortKeysCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGaussianSortKeysCS::FParameters>();
			PassParameters->LocalToWorldMatrix = LocalToWorldMatrix;
			PassParameters->PreViewTranslation = PreViewTranslation;
			PassParameters->TranslatedViewMatrix = TranslatedViewMatrix;
			PassParameters->TranslatedWorldToClip = TranslatedWorldToClip;
			PassParameters->MaxVisibleCount = MaxVisibleCount;
			PassParameters->PaddedCount = KeyBufferCount;
			PassParameters->VisibleIndices = CullResult.VisibleIndicesSRV;
			PassParameters->VisibleCountBuffer = CullResult.VisibleCountSRV;
			PassParameters->GaussianPositions = Binding.PositionSRV;
			PassParameters->RWSortKeys = SortKeysAUAV;

			const uint32 NumGroups = FMath::DivideAndRoundUp(KeyBufferCount, GaussianThreadGroupSize);
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("GaussianSimVerse::SortKeys S%u C%u", Binding.SceneId, Binding.ChunkIndex),
				ERDGPassFlags::Compute,
				SortKeysShader,
				PassParameters,
				FIntVector(NumGroups, 1, 1));
		}

		FRDGBufferRef SortedKeysBuffer = SortKeysA;

		if (bUseRadix)
		{
			// 16 passes x 4-bit digits over 64-bit (depth, GaussianIndex) for stable far-to-near order.
			FRDGBufferRef HistogramBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 16),
				*FString::Printf(TEXT("Gaussian.RadixHist.S%d.C%d"), Binding.SceneId, Binding.ChunkIndex));
			FRDGBufferRef ScatterBaseBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 16),
				*FString::Printf(TEXT("Gaussian.RadixBase.S%d.C%d"), Binding.SceneId, Binding.ChunkIndex));

			FRDGBufferRef InputKeys = SortKeysA;
			FRDGBufferRef OutputKeys = SortKeysB;

			for (uint32 Digit = 0; Digit < 16u; ++Digit)
			{
				const uint32 DigitShift = Digit * 4u;
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(HistogramBuffer), 0u);

				{
					FGaussianRadixCountCS::FParameters* CountParams = GraphBuilder.AllocParameters<FGaussianRadixCountCS::FParameters>();
					CountParams->ElementCount = ElementCount;
					CountParams->DigitShift = DigitShift;
					CountParams->SortKeys = GraphBuilder.CreateSRV(InputKeys);
					CountParams->RWHistogram = GraphBuilder.CreateUAV(HistogramBuffer);
					const uint32 NumGroups = FMath::DivideAndRoundUp(ElementCount, GaussianThreadGroupSize);
					FComputeShaderUtils::AddPass(
						GraphBuilder,
						RDG_EVENT_NAME("GaussianSimVerse::RadixCount d%u", Digit),
						ERDGPassFlags::Compute,
						RadixCountShader,
						CountParams,
						FIntVector(NumGroups, 1, 1));
				}

				{
					FGaussianRadixPrefixSumCS::FParameters* PrefixParams = GraphBuilder.AllocParameters<FGaussianRadixPrefixSumCS::FParameters>();
					PrefixParams->Histogram = GraphBuilder.CreateSRV(HistogramBuffer);
					PrefixParams->RWScatterBase = GraphBuilder.CreateUAV(ScatterBaseBuffer);
					FComputeShaderUtils::AddPass(
						GraphBuilder,
						RDG_EVENT_NAME("GaussianSimVerse::RadixPrefix d%u", Digit),
						ERDGPassFlags::Compute,
						RadixPrefixShader,
						PrefixParams,
						FIntVector(1, 1, 1));
				}

				{
					FGaussianRadixScatterCS::FParameters* ScatterParams = GraphBuilder.AllocParameters<FGaussianRadixScatterCS::FParameters>();
					ScatterParams->ElementCount = ElementCount;
					ScatterParams->DigitShift = DigitShift;
					ScatterParams->SortKeysIn = GraphBuilder.CreateSRV(InputKeys);
					ScatterParams->RWScatterBase = GraphBuilder.CreateUAV(ScatterBaseBuffer);
					ScatterParams->RWSortKeysOut = GraphBuilder.CreateUAV(OutputKeys);
					const uint32 NumGroups = FMath::DivideAndRoundUp(ElementCount, GaussianThreadGroupSize);
					FComputeShaderUtils::AddPass(
						GraphBuilder,
						RDG_EVENT_NAME("GaussianSimVerse::RadixScatter d%u", Digit),
						ERDGPassFlags::Compute,
						RadixScatterShader,
						ScatterParams,
						FIntVector(NumGroups, 1, 1));
				}

				Swap(InputKeys, OutputKeys);
			}

			// After even number of swaps, result is in SortKeysA.
			SortedKeysBuffer = SortKeysA;
		}
		else
		{
			const uint32 PaddedCount = KeyBufferCount;
			for (uint32 K = 2; K <= PaddedCount; K <<= 1)
			{
				for (uint32 J = K >> 1; J > 0; J >>= 1)
				{
					FGaussianBitonicSortCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGaussianBitonicSortCS::FParameters>();
					PassParameters->K = K;
					PassParameters->J = J;
					PassParameters->PaddedCount = PaddedCount;
					PassParameters->RWSortKeys = SortKeysAUAV;

					const uint32 NumGroups = FMath::DivideAndRoundUp(PaddedCount, GaussianThreadGroupSize);
					FComputeShaderUtils::AddPass(
						GraphBuilder,
						RDG_EVENT_NAME("GaussianSimVerse::BitonicSort K%u J%u", K, J),
						ERDGPassFlags::Compute,
						BitonicShader,
						PassParameters,
						FIntVector(NumGroups, 1, 1));
				}
			}
			SortedKeysBuffer = SortKeysA;
		}

		FRDGBufferUAVRef SortedIndicesUAV = GraphBuilder.CreateUAV(SortedIndicesBuffer);
		{
			FGaussianSortExtractCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGaussianSortExtractCS::FParameters>();
			PassParameters->MaxVisibleCount = MaxVisibleCount;
			PassParameters->SortKeys = GraphBuilder.CreateSRV(SortedKeysBuffer);
			PassParameters->VisibleCountBuffer = CullResult.VisibleCountSRV;
			PassParameters->RWSortedIndices = SortedIndicesUAV;

			const uint32 NumGroups = FMath::DivideAndRoundUp(ElementCount, GaussianThreadGroupSize);
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("GaussianSimVerse::SortExtract S%u C%u (%s)", Binding.SceneId, Binding.ChunkIndex, bUseRadix ? TEXT("radix") : TEXT("bitonic")),
				ERDGPassFlags::Compute,
				ExtractShader,
				PassParameters,
				FIntVector(NumGroups, 1, 1));
		}

		FGaussianRDGSortResult SortResult;
		SortResult.CullResultIndex = CullIndex;
		SortResult.SortedIndicesBuffer = SortedIndicesBuffer;
		SortResult.SortedIndicesSRV = GraphBuilder.CreateSRV(SortedIndicesBuffer);
		SortResult.VisibleCount = MaxVisibleCount;
		TransientResources.SortResults.Add(MoveTemp(SortResult));
	}
}

void FGaussianRenderGraph::AddGPURasterPasses(
	FRDGBuilder& GraphBuilder,
	const FPassInputs& Inputs,
	FGaussianRDGTransientResources& TransientResources)
{
	if (!Inputs.SceneColorTexture || TransientResources.CullResults.Num() == 0)
	{
		return;
	}

	const FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	const int32 TileRasterMode = GaussianSimVerse::RenderSettings::GetTileRasterMode();
	const TShaderMapRef<FGaussianRasterCS> RasterShader(GlobalShaderMap);
	const TShaderMapRef<FGaussianSplatDrawVS> SplatDrawVertexShader(GlobalShaderMap);
	const TShaderMapRef<FGaussianSplatDrawPS> SplatDrawPixelShader(GlobalShaderMap);
	const TShaderMapRef<FGaussianBinSplatsFillCS> BinFillShader(GlobalShaderMap);
	const TShaderMapRef<FGaussianTileSortCS> TileSortShader(GlobalShaderMap);
	const TShaderMapRef<FGaussianTileBlendCS> TileBlendShader(GlobalShaderMap);
	const bool bDrawShadersValid = SplatDrawVertexShader.IsValid() && SplatDrawPixelShader.IsValid();
	const bool bTileShadersValid = BinFillShader.IsValid() && TileSortShader.IsValid() && TileBlendShader.IsValid();
	if (!bTileShadersValid && !bDrawShadersValid && !RasterShader.IsValid())
	{
		UE_LOG(LogGaussianSimVerse, Warning, TEXT("GaussianSimVerse raster shaders are not compiled for this platform/feature level."));
		return;
	}

	FRDGTextureRef SceneColorTexture = Inputs.SceneColorTexture;
	const FIntPoint OverlayExtent(
		FMath::Max(1, Inputs.SceneColorViewRect.Width()),
		FMath::Max(1, Inputs.SceneColorViewRect.Height()));

	FRDGTextureDesc OverlayDesc = SceneColorTexture->Desc;
	OverlayDesc.Extent = OverlayExtent;
	OverlayDesc.Flags |= ETextureCreateFlags::ShaderResource | ETextureCreateFlags::UAV | ETextureCreateFlags::RenderTargetable;
	FRDGTextureRef OverlayTexture = GraphBuilder.CreateTexture(OverlayDesc, TEXT("Gaussian.SplatOverlay"));
	FRDGTextureUAVRef OverlayUAV = GraphBuilder.CreateUAV(OverlayTexture);
	AddClearUAVPass(GraphBuilder, OverlayUAV, FVector4f(0.0f, 0.0f, 0.0f, 0.0f));

	const TArray<float> DummyShCoefficientsCPU = { 0.0f };
	FRDGBufferRef DummyShCoefficientsBuffer = CreateStructuredUploadBuffer(
		GraphBuilder,
		TEXT("Gaussian.DummyShCoeffs"),
		DummyShCoefficientsCPU);
	FRDGBufferSRVRef DummyShCoefficientsSRV = GraphBuilder.CreateSRV(DummyShCoefficientsBuffer);
	const FVector3f CameraWorldPosition(Inputs.ViewData.ViewOrigin);

	// Local 0-based rect for projection and raster — matches SceneColor slice / base-pass render resolution.
	const FVector4f ViewRect(
		0.0f,
		0.0f,
		static_cast<float>(OverlayExtent.X),
		static_cast<float>(OverlayExtent.Y));

	// ViewportRect == ViewRect here: clip-to-screen stays in overlay space; composite adds SceneColorOffset.
	const FVector4f ViewportRect = ViewRect;

	const FIntPoint SceneColorOffset = Inputs.SceneColorViewRect.Min;

	if (Inputs.View != nullptr && GaussianSimVerse::RenderSettings::IsRenderDebugEnabled())
	{
		const FIntRect UnscaledViewRect = Inputs.View->UnscaledViewRect;
		if (UnscaledViewRect.Size() != Inputs.SceneColorViewRect.Size())
		{
			static bool bLoggedViewportTextureMismatchOnce = false;
			if (!bLoggedViewportTextureMismatchOnce)
			{
				bLoggedViewportTextureMismatchOnce = true;
				UE_LOG(LogGaussianSimVerse, Log,
					TEXT("GaussianSimVerse using SceneColor rect for projection (UnscaledViewRect %dx%d != SceneColor %dx%d; common with screen %% / TSR)"),
					UnscaledViewRect.Width(), UnscaledViewRect.Height(),
					Inputs.SceneColorViewRect.Width(), Inputs.SceneColorViewRect.Height());
			}
		}
	}

	const float GaussianAlphaCutoff = GaussianSimVerse::RenderSettings::GetAlphaCutoff();
	const float GaussianMinSigmaPixels = GaussianSimVerse::RenderSettings::GetMinSigmaPixels();
	const uint32 MaxRasterRadius = static_cast<uint32>(FMath::Max(1, GaussianSimVerse::RenderSettings::CVarMaxRasterRadius.GetValueOnRenderThread()));
	const int32 MaxRasterSplatsCVar = GaussianSimVerse::RenderSettings::CVarMaxRasterSplats.GetValueOnRenderThread();
	const uint32 MaxRasterSplats = MaxRasterSplatsCVar > 0
		? static_cast<uint32>(MaxRasterSplatsCVar)
		: TNumericLimits<uint32>::Max();
	const bool bUseSortResults = GaussianSimVerse::RenderSettings::IsSortEnabled();
	const bool bDebugOverlay = GaussianSimVerse::RenderSettings::IsDebugOverlayEnabled();
	const FVector3f PreViewTranslation(Inputs.ViewData.PreViewTranslation);
	const FMatrix44f TranslatedWorldToClip(Inputs.ViewData.TranslatedWorldToClip);
	const FMatrix44f TranslatedViewMatrix(Inputs.ViewData.TranslatedViewMatrix);
	uint32 TotalRasterSplats = 0;

	const uint32 TileSize = 16u;
	const uint32 ViewWidth = static_cast<uint32>(OverlayExtent.X);
	const uint32 ViewHeight = static_cast<uint32>(OverlayExtent.Y);
	const uint32 NumTilesX = FMath::Max(1u, (ViewWidth + TileSize - 1u) / TileSize);
	const uint32 NumTilesY = FMath::Max(1u, (ViewHeight + TileSize - 1u) / TileSize);
	const uint32 NumTiles = NumTilesX * NumTilesY;

	const uint32 MaxSplatsPerTile = static_cast<uint32>(GaussianSimVerse::RenderSettings::GetMaxSplatsPerTile());

	TArray<uint32> TileOffsetsCPU;
	TileOffsetsCPU.SetNumUninitialized(static_cast<int32>(NumTiles + 1u));
	for (uint32 TileId = 0; TileId <= NumTiles; ++TileId)
	{
		TileOffsetsCPU[static_cast<int32>(TileId)] = TileId * MaxSplatsPerTile;
	}

	FRDGBufferRef TileOffsetsBuffer = CreateStructuredUploadBuffer(
		GraphBuilder,
		TEXT("Gaussian.TileOffsets"),
		TileOffsetsCPU);
	FRDGBufferSRVRef TileOffsetsSRV = GraphBuilder.CreateSRV(TileOffsetsBuffer);

	float AdaptiveViewDistance = 0.0f;
	float AdaptiveSceneBoundsRadius = 100.0f;
	bool bHaveAdaptiveSceneMetrics = false;
	if (TransientResources.CullResults.Num() > 0)
	{
		const FGaussianRDGCullResult& FirstCullResult = TransientResources.CullResults[0];
		if (TransientResources.GPUBuffers.IsValidIndex(FirstCullResult.GPUBindingIndex))
		{
			const FGaussianRDGBufferBinding& FirstBinding = TransientResources.GPUBuffers[FirstCullResult.GPUBindingIndex];
			for (const FGaussianSceneProxy& SceneProxy : Inputs.FrameResources.SceneProxies)
			{
				if (SceneProxy.SceneId == FirstBinding.SceneId)
				{
					AdaptiveViewDistance = static_cast<float>(FVector::Dist(
						Inputs.ViewData.ViewOrigin,
						SceneProxy.LocalToWorld.GetOrigin()));
					AdaptiveSceneBoundsRadius = FVector(SceneProxy.Bounds.Extent).Size();
					bHaveAdaptiveSceneMetrics = true;
					break;
				}
			}
		}
	}

	static TMap<uint32, bool> AdaptiveLastTilePathByView;
	const uint32 ViewKey = (Inputs.View != nullptr)
		? static_cast<uint32>(Inputs.View->GetViewKey())
		: 0u;
	bool& bAdaptiveLastTilePath = AdaptiveLastTilePathByView.FindOrAdd(ViewKey, true);

	bool bAdaptiveUseTilePath = true;
	if (TileRasterMode == 2 && bTileShadersValid && bHaveAdaptiveSceneMetrics)
	{
		const uint32 AdaptiveSplatCount = TransientResources.CullResults.Num() > 0
			? TransientResources.CullResults[0].MaxVisibleCount
			: 0u;
		bAdaptiveUseTilePath = GaussianSimVerse::RenderSettings::ShouldUseTileRasterPath(
			TileRasterMode,
			bTileShadersValid,
			0u,
			static_cast<int32>(MaxSplatsPerTile),
			AdaptiveViewDistance,
			AdaptiveSceneBoundsRadius,
			bAdaptiveLastTilePath,
			AdaptiveSplatCount,
			bDrawShadersValid);
	}

	for (int32 CullIndex = 0; CullIndex < TransientResources.CullResults.Num(); ++CullIndex)
	{
		const FGaussianRDGCullResult& CullResult = TransientResources.CullResults[CullIndex];
		FRDGBufferSRVRef SortedIndicesSRV = CullResult.VisibleIndicesSRV;
		uint32 SortedCount = CullResult.MaxVisibleCount;

		if (bUseSortResults)
		{
			const FGaussianRDGSortResult* SortResult = TransientResources.SortResults.FindByPredicate(
				[CullIndex](const FGaussianRDGSortResult& Result)
				{
					return Result.CullResultIndex == CullIndex;
				});

			if (SortResult && SortResult->SortedIndicesSRV)
			{
				SortedIndicesSRV = SortResult->SortedIndicesSRV;
				SortedCount = SortResult->VisibleCount;
			}
		}

		SortedCount = FMath::Min(SortedCount, MaxRasterSplats);

		if (MaxRasterSplatsCVar > 0 && CullResult.MaxVisibleCount > static_cast<uint32>(MaxRasterSplatsCVar))
		{
			static bool bLoggedRasterCapOnce = false;
			if (!bLoggedRasterCapOnce)
			{
				bLoggedRasterCapOnce = true;
				UE_LOG(LogGaussianSimVerse, Warning,
					TEXT("GaussianSimVerse raster capped at %d/%u splats. Set r.GaussianSimVerse.MaxRasterSplats 0 to render all."),
					MaxRasterSplatsCVar, CullResult.MaxVisibleCount);
			}
		}

		if (!SortedIndicesSRV || SortedCount == 0)
		{
			continue;
		}

		const FGaussianRDGBufferBinding& Binding = TransientResources.GPUBuffers[CullResult.GPUBindingIndex];
		const float SplatScale = Binding.SplatScale;
		const float GaussianAlphaCullThreshold = Binding.AlphaCullThreshold;
		const float GaussianCutoffK = Binding.CutoffK;
		const float GaussianCovarianceDilation = Binding.CovarianceDilation;

		const uint32 EstimatedSplatsPerTile = NumTiles > 0
			? (SortedCount + NumTiles - 1u) / NumTiles
			: SortedCount;

		float ViewDistanceToScene = 0.0f;
		float SceneBoundsRadius = 100.0f;
		for (const FGaussianSceneProxy& SceneProxy : Inputs.FrameResources.SceneProxies)
		{
			if (SceneProxy.SceneId == Binding.SceneId)
			{
				ViewDistanceToScene = static_cast<float>(FVector::Dist(
					Inputs.ViewData.ViewOrigin,
					SceneProxy.LocalToWorld.GetOrigin()));
				SceneBoundsRadius = FVector(SceneProxy.Bounds.Extent).Size();
				break;
			}
		}

		bool bUseTilePath = false;
		if (TileRasterMode == 2)
		{
			bool bAdaptiveState = bAdaptiveUseTilePath;
			bUseTilePath = GaussianSimVerse::RenderSettings::ShouldUseTileRasterPath(
				TileRasterMode,
				bTileShadersValid,
				EstimatedSplatsPerTile,
				static_cast<int32>(MaxSplatsPerTile),
				ViewDistanceToScene,
				SceneBoundsRadius,
				bAdaptiveState,
				SortedCount,
				bDrawShadersValid);
			bAdaptiveLastTilePath = bAdaptiveState;
		}
		else
		{
			bool bUnusedAdaptiveState = true;
			bUseTilePath = GaussianSimVerse::RenderSettings::ShouldUseTileRasterPath(
				TileRasterMode,
				bTileShadersValid,
				EstimatedSplatsPerTile,
				static_cast<int32>(MaxSplatsPerTile),
				ViewDistanceToScene,
				SceneBoundsRadius,
				bUnusedAdaptiveState,
				SortedCount,
				bDrawShadersValid);
		}

		// Legacy compute global raster TDRs on huge clouds — only force tile when draw shaders are unavailable.
		const uint32 MaxSafeGlobal = GaussianSimVerse::RenderSettings::GetMaxSafeGlobalRasterSplats();
		if (!bUseTilePath && !bDrawShadersValid && SortedCount > MaxSafeGlobal && bTileShadersValid)
		{
			bUseTilePath = true;
			static bool bLoggedForceTileOnce = false;
			if (!bLoggedForceTileOnce)
			{
				bLoggedForceTileOnce = true;
				UE_LOG(LogGaussianSimVerse, Warning,
					TEXT("GaussianSimVerse: forcing tile raster for %u splats (global path TDRs above %u). Raise r.GaussianSimVerse.MaxSortElements only if you also raise sort capacity."),
					SortedCount, MaxSafeGlobal);
			}
		}

		if (!bUseTilePath && !bDrawShadersValid && !RasterShader.IsValid() && bTileShadersValid)
		{
			bUseTilePath = true;
		}

		if (TileRasterMode == 2 && (bTileShadersValid || bDrawShadersValid)
			&& GaussianSimVerse::RenderSettings::IsRenderDebugEnabled())
		{
			const float SafeRadius = FMath::Max(SceneBoundsRadius, 1.0f);
			const float DistanceToRadius = ViewDistanceToScene / SafeRadius;
			const TCHAR* RasterPathName = bUseTilePath
				? TEXT("tile")
				: (bDrawShadersValid ? TEXT("draw") : TEXT("compute"));
			UE_LOG(LogGaussianSimVerse, Log,
				TEXT("GaussianSimVerse raster S%u: %s (dist=%.0f radius=%.0f dist/radius=%.1f splats/tile~%u)"),
				Binding.SceneId,
				RasterPathName,
				ViewDistanceToScene,
				SceneBoundsRadius,
				DistanceToRadius,
				EstimatedSplatsPerTile);
		}

		if (bUseTilePath)
		{
			FRDGBufferRef TileFillCounterBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumTiles),
				TEXT("Gaussian.TileFillCounters"));
			FRDGBufferUAVRef TileFillCounterUAV = GraphBuilder.CreateUAV(TileFillCounterBuffer);
			FRDGBufferSRVRef TileFillCounterSRV = GraphBuilder.CreateSRV(TileFillCounterBuffer);
			AddClearUAVPass(GraphBuilder, TileFillCounterUAV, 0u);

			const uint32 TotalTileSlots = NumTiles * MaxSplatsPerTile;
			FRDGBufferRef TileSplatsBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(FUintVector2), TotalTileSlots),
				TEXT("Gaussian.TileSplats"));
			FRDGBufferUAVRef TileSplatsUAV = GraphBuilder.CreateUAV(TileSplatsBuffer);
			FRDGBufferSRVRef TileSplatsSRV = GraphBuilder.CreateSRV(TileSplatsBuffer);

			FGaussianBinSplatsFillCS::FParameters* FillParameters = GraphBuilder.AllocParameters<FGaussianBinSplatsFillCS::FParameters>();
			FillParameters->LocalToWorldMatrix = FMatrix44f(Binding.LocalToWorld);
			FillParameters->PreViewTranslation = PreViewTranslation;
			FillParameters->TranslatedViewMatrix = TranslatedViewMatrix;
			FillParameters->TranslatedWorldToClip = TranslatedWorldToClip;
			FillParameters->ViewportRect = ViewportRect;
			FillParameters->ViewRect = ViewRect;
			FillParameters->SortedCount = SortedCount;
			FillParameters->GaussianCount = Binding.NumGaussians;
			FillParameters->NumTilesX = NumTilesX;
			FillParameters->NumTilesY = NumTilesY;
			FillParameters->MaxTileSplats = MaxSplatsPerTile;
			FillParameters->SplatScale = SplatScale;
			FillParameters->GaussianAlphaCutoff = GaussianAlphaCutoff;
			FillParameters->GaussianAlphaCullThreshold = GaussianAlphaCullThreshold;
			FillParameters->GaussianCutoffK = GaussianCutoffK;
			FillParameters->GaussianCovarianceDilation = GaussianCovarianceDilation;
			FillParameters->GaussianMinSigmaPixels = GaussianMinSigmaPixels;
			FillParameters->MaxRasterRadius = MaxRasterRadius;
			FillParameters->bDebugOverlay = bDebugOverlay ? 1u : 0u;
			FillParameters->SortedIndices = SortedIndicesSRV;
			FillParameters->VisibleCountBuffer = CullResult.VisibleCountSRV;
			FillParameters->GaussianSplatsVec4 = Binding.SplatSRV;
			FillParameters->TileOffsets = TileOffsetsSRV;
			FillParameters->RWTileFillCounters = TileFillCounterUAV;
			FillParameters->RWTileSplats = TileSplatsUAV;

			const uint32 FillGroups = FMath::DivideAndRoundUp(SortedCount, GaussianThreadGroupSize);
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("GaussianSimVerse::TileFill S%u C%u (%u splats)", Binding.SceneId, Binding.ChunkIndex, SortedCount),
				ERDGPassFlags::Compute,
				BinFillShader,
				FillParameters,
				FIntVector(FillGroups, 1, 1));

			FGaussianTileSortCS::FParameters* SortParameters = GraphBuilder.AllocParameters<FGaussianTileSortCS::FParameters>();
			SortParameters->NumTilesX = NumTilesX;
			SortParameters->NumTilesY = NumTilesY;
			SortParameters->MaxTileSplats = MaxSplatsPerTile;
			SortParameters->TileOffsets = TileOffsetsSRV;
			SortParameters->TileCounts = TileFillCounterSRV;
			SortParameters->RWTileSplats = TileSplatsUAV;

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("GaussianSimVerse::TileSort S%u C%u", Binding.SceneId, Binding.ChunkIndex),
				ERDGPassFlags::Compute,
				TileSortShader,
				SortParameters,
				FIntVector(NumTilesX, NumTilesY, 1));

			FGaussianTileBlendCS::FParameters* BlendParameters = GraphBuilder.AllocParameters<FGaussianTileBlendCS::FParameters>();
			BlendParameters->LocalToWorldMatrix = FMatrix44f(Binding.LocalToWorld);
			BlendParameters->PreViewTranslation = PreViewTranslation;
			BlendParameters->TranslatedViewMatrix = TranslatedViewMatrix;
			BlendParameters->TranslatedWorldToClip = TranslatedWorldToClip;
			BlendParameters->ViewportRect = ViewportRect;
			BlendParameters->ViewRect = ViewRect;
			BlendParameters->NumTilesX = NumTilesX;
			BlendParameters->NumTilesY = NumTilesY;
			BlendParameters->NumTiles = NumTiles;
			BlendParameters->GaussianCount = Binding.NumGaussians;
			BlendParameters->MaxTileSplats = MaxSplatsPerTile;
			BlendParameters->SplatScale = SplatScale;
			BlendParameters->GaussianAlphaCutoff = GaussianAlphaCutoff;
			BlendParameters->GaussianAlphaCullThreshold = GaussianAlphaCullThreshold;
			BlendParameters->GaussianCutoffK = GaussianCutoffK;
			BlendParameters->GaussianCovarianceDilation = GaussianCovarianceDilation;
			BlendParameters->GaussianMinSigmaPixels = GaussianMinSigmaPixels;
			BlendParameters->MaxRasterRadius = MaxRasterRadius;
			BlendParameters->bDebugOverlay = bDebugOverlay ? 1u : 0u;
			BlendParameters->RenderShDegree = Binding.RenderShDegree;
			BlendParameters->ImportedShDegree = Binding.ImportedShDegree;
			BlendParameters->bHasShCoefficients = Binding.bHasShCoefficients;
			BlendParameters->CameraWorldPosition = CameraWorldPosition;
			GaussianRenderGraphPrivate::FillGaussianColorGradeUniforms(
				BlendParameters->GaussianClrOffset,
				BlendParameters->GaussianClrScaleRGB,
				BlendParameters->GaussianSaturation,
				BlendParameters->GaussianTransparencyMultiplier,
				Binding.ColorGrade);
			BlendParameters->TileOffsets = TileOffsetsSRV;
			BlendParameters->TileCounts = TileFillCounterSRV;
			BlendParameters->TileSplats = TileSplatsSRV;
			BlendParameters->SortedIndices = SortedIndicesSRV;
			BlendParameters->GaussianSplatsVec4 = Binding.SplatSRV;
			BlendParameters->GaussianShCoeffs = Binding.ShCoefficientsSRV ? Binding.ShCoefficientsSRV : DummyShCoefficientsSRV;
			BlendParameters->RWOverlay = OverlayUAV;

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("GaussianSimVerse::TileBlend S%u C%u (%ux%u tiles)", Binding.SceneId, Binding.ChunkIndex, NumTilesX, NumTilesY),
				ERDGPassFlags::Compute,
				TileBlendShader,
				BlendParameters,
				FIntVector(NumTilesX, NumTilesY, 1));
		}
		else if (bDrawShadersValid)
		{
			for (uint32 BatchStart = 0; BatchStart < SortedCount; BatchStart += GaussianRenderGraphPrivate::SplatsPerDrawBatch)
			{
				const uint32 BatchCount = FMath::Min(
					GaussianRenderGraphPrivate::SplatsPerDrawBatch,
					SortedCount - BatchStart);

				FGaussianSplatDrawPS::FParameters* DrawParameters = GraphBuilder.AllocParameters<FGaussianSplatDrawPS::FParameters>();
				GaussianRenderGraphPrivate::FillGaussianSplatDrawParameters(
					DrawParameters->Shared,
					FMatrix44f(Binding.LocalToWorld),
					PreViewTranslation,
					TranslatedViewMatrix,
					TranslatedWorldToClip,
					ViewportRect,
					ViewRect,
					BatchStart,
					BatchCount,
					SortedCount,
					Binding.NumGaussians,
					SplatScale,
					GaussianAlphaCutoff,
					GaussianAlphaCullThreshold,
					GaussianCutoffK,
					GaussianCovarianceDilation,
					GaussianMinSigmaPixels,
					MaxRasterRadius,
					bDebugOverlay,
					Binding.RenderShDegree,
					Binding.ImportedShDegree,
					Binding.bHasShCoefficients,
					CameraWorldPosition,
					Binding.ColorGrade,
					SortedIndicesSRV,
					CullResult.VisibleCountSRV,
					Binding.SplatSRV,
					Binding.ShCoefficientsSRV ? Binding.ShCoefficientsSRV : DummyShCoefficientsSRV);

				DrawParameters->RenderTargets[0] = FRenderTargetBinding(OverlayTexture, ERenderTargetLoadAction::ELoad);

				GaussianRenderGraphPrivate::AddGaussianSplatDrawPass(
					GraphBuilder,
					RDG_EVENT_NAME("GaussianSimVerse::SplatDraw S%u C%u (%u..%u)", Binding.SceneId, Binding.ChunkIndex, BatchStart, BatchStart + BatchCount),
					GlobalShaderMap,
					DrawParameters,
					OverlayExtent,
					BatchCount);
			}
		}
		else
		{
			for (uint32 BatchStart = 0; BatchStart < SortedCount; BatchStart += GaussianRenderGraphPrivate::SplatsPerRasterBatch)
			{
				const uint32 BatchCount = FMath::Min(
					GaussianRenderGraphPrivate::SplatsPerRasterBatch,
					SortedCount - BatchStart);

				FGaussianRasterCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGaussianRasterCS::FParameters>();
				PassParameters->LocalToWorldMatrix = FMatrix44f(Binding.LocalToWorld);
				PassParameters->PreViewTranslation = PreViewTranslation;
				PassParameters->TranslatedViewMatrix = TranslatedViewMatrix;
				PassParameters->TranslatedWorldToClip = TranslatedWorldToClip;
				PassParameters->ViewportRect = ViewportRect;
				PassParameters->ViewRect = ViewRect;
				PassParameters->BatchStart = BatchStart;
				PassParameters->BatchCount = BatchCount;
				PassParameters->SortedCount = SortedCount;
				PassParameters->GaussianCount = Binding.NumGaussians;
				PassParameters->SplatScale = SplatScale;
				PassParameters->GaussianAlphaCutoff = GaussianAlphaCutoff;
				PassParameters->GaussianAlphaCullThreshold = GaussianAlphaCullThreshold;
				PassParameters->GaussianCutoffK = GaussianCutoffK;
				PassParameters->GaussianCovarianceDilation = GaussianCovarianceDilation;
				PassParameters->GaussianMinSigmaPixels = GaussianMinSigmaPixels;
				PassParameters->MaxRasterRadius = MaxRasterRadius;
				PassParameters->bDebugOverlay = bDebugOverlay ? 1u : 0u;
				PassParameters->RenderShDegree = Binding.RenderShDegree;
				PassParameters->ImportedShDegree = Binding.ImportedShDegree;
				PassParameters->bHasShCoefficients = Binding.bHasShCoefficients;
				PassParameters->CameraWorldPosition = CameraWorldPosition;
				GaussianRenderGraphPrivate::FillGaussianColorGradeUniforms(
					PassParameters->GaussianClrOffset,
					PassParameters->GaussianClrScaleRGB,
					PassParameters->GaussianSaturation,
					PassParameters->GaussianTransparencyMultiplier,
					Binding.ColorGrade);
				PassParameters->SortedIndices = SortedIndicesSRV;
				PassParameters->VisibleCountBuffer = CullResult.VisibleCountSRV;
				PassParameters->GaussianSplatsVec4 = Binding.SplatSRV;
				PassParameters->GaussianShCoeffs = Binding.ShCoefficientsSRV ? Binding.ShCoefficientsSRV : DummyShCoefficientsSRV;
				PassParameters->RWOverlay = OverlayUAV;

				const uint32 NumGroups = FMath::DivideAndRoundUp(BatchCount, GaussianThreadGroupSize);
				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("GaussianSimVerse::Raster S%u C%u (%u..%u)", Binding.SceneId, Binding.ChunkIndex, BatchStart, BatchStart + BatchCount),
					ERDGPassFlags::Compute,
					RasterShader,
					PassParameters,
					FIntVector(NumGroups, 1, 1));
			}
		}

		TotalRasterSplats += SortedCount;
	}

	if (TotalRasterSplats > 0)
	{
		const TShaderMapRef<FGaussianCompositeCS> CompositeShader(GlobalShaderMap);
		if (CompositeShader.IsValid())
		{
			// Scene color resolve textures are not created with UAV; composite to a scratch target then copy back.
			FRDGTextureDesc MergedDesc = SceneColorTexture->Desc;
			MergedDesc.Flags |= ETextureCreateFlags::ShaderResource | ETextureCreateFlags::UAV;
			FRDGTextureRef MergedTexture = GraphBuilder.CreateTexture(MergedDesc, TEXT("Gaussian.MergedSceneColor"));
			FRDGTextureUAVRef MergedUAV = GraphBuilder.CreateUAV(MergedTexture);
			AddCopyTexturePass(GraphBuilder, SceneColorTexture, MergedTexture);

			FGaussianCompositeCS::FParameters* CompositeParams = GraphBuilder.AllocParameters<FGaussianCompositeCS::FParameters>();
			CompositeParams->ViewRect = ViewRect;
			CompositeParams->SceneColorOffset = SceneColorOffset;
			CompositeParams->SceneColorTexture = SceneColorTexture;
			CompositeParams->OverlayTexture = OverlayTexture;
			CompositeParams->RWSceneColor = MergedUAV;

			const uint32 GroupsX = FMath::DivideAndRoundUp(ViewWidth, 8u);
			const uint32 GroupsY = FMath::DivideAndRoundUp(ViewHeight, 8u);
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("GaussianSimVerse::Composite"),
				ERDGPassFlags::Compute,
				CompositeShader,
				CompositeParams,
				FIntVector(GroupsX, GroupsY, 1));

			AddCopyTexturePass(GraphBuilder, MergedTexture, SceneColorTexture);
		}
		else
		{
			UE_LOG(LogGaussianSimVerse, Warning, TEXT("GaussianSimVerse composite shader is not compiled; splats were rasterized but not blended."));
		}
	}

	if (GaussianSimVerse::RenderSettings::IsRenderDebugEnabled())
	{
		UE_LOG(LogGaussianSimVerse, Log, TEXT("GaussianSimVerse raster: %u splats, %u GPU bindings"), TotalRasterSplats, TransientResources.GPUBuffers.Num());
	}
	else if (TotalRasterSplats == 0 && TransientResources.GPUBuffers.Num() > 0)
	{
		static bool bLoggedZeroRasterOnce = false;
		if (!bLoggedZeroRasterOnce)
		{
			bLoggedZeroRasterOnce = true;
			UE_LOG(LogGaussianSimVerse, Warning, TEXT("GaussianSimVerse raster submitted 0 splats despite %u GPU bindings. Try: r.GaussianSimVerse.EnableCull 0, r.GaussianSimVerse.DebugOverlay 1"),
				TransientResources.GPUBuffers.Num());
		}
	}
}

void FGaussianRenderGraph::AddFrameworkValidationPass(
	FRDGBuilder& GraphBuilder,
	const FPassInputs& Inputs,
	FGaussianRDGTransientResources& TransientResources)
{
	if (!GaussianSimVerse::RenderSettings::IsFrameworkDebugEnabled())
	{
		return;
	}

	const FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	const TShaderMapRef<FGaussianFrameworkCS> ComputeShader(GlobalShaderMap);

	if (!ComputeShader.IsValid())
	{
		return;
	}

	FGaussianFrameworkCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGaussianFrameworkCS::FParameters>();
	PassParameters->SceneCount = Inputs.FrameResources.SceneProxies.Num();
	PassParameters->GaussianCount = TransientResources.UploadedGaussianCount > 0
		? TransientResources.UploadedGaussianCount
		: Inputs.FrameResources.TotalGaussianCount;
	PassParameters->RWFrameworkCounter = TransientResources.FrameworkCounterUAV;

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GaussianSimVerse::FrameworkValidation"),
		ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
		ComputeShader,
		PassParameters,
		FIntVector(1, 1, 1));
}
