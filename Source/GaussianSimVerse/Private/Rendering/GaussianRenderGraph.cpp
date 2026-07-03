// Copyright Epic Games, Inc. All Rights Reserved.

#include "Rendering/GaussianRenderGraph.h"
#include "Rendering/GaussianShaderTypes.h"
#include "Rendering/GaussianRenderSettings.h"
#include "Rendering/GaussianGPUResources.h"
#include "GaussianSimVerse.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"
#include "GlobalShader.h"

namespace GaussianRenderGraphPrivate
{
	static constexpr uint32 SplatsPerRasterBatch = 2048;

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
			Binding.SplatScale = SceneProxy.SplatScale;
			Binding.AlphaCullThreshold = SceneProxy.AlphaCullThreshold;
			Binding.CutoffK = SceneProxy.CutoffK;
			Binding.CovarianceDilation = SceneProxy.CovarianceDilation;

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
	if (!SortKeysShader.IsValid() || !BitonicShader.IsValid() || !ExtractShader.IsValid())
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
		const uint32 PaddedCount = GaussianRenderGraphPrivate::GetPaddedSortCount(MaxVisibleCount);
		if (PaddedCount < 2)
		{
			continue;
		}

		const FMatrix44f LocalToWorldMatrix(Binding.LocalToWorld);
		const FVector3f PreViewTranslation(Inputs.ViewData.PreViewTranslation);
		const FMatrix44f TranslatedViewMatrix(Inputs.ViewData.TranslatedViewMatrix);
		const FMatrix44f TranslatedWorldToClip(Inputs.ViewData.TranslatedWorldToClip);

		FRDGBufferRef SortKeysBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FUintVector2), PaddedCount),
			*FString::Printf(TEXT("Gaussian.SortKeys.S%d.C%d"), Binding.SceneId, Binding.ChunkIndex));

		FRDGBufferRef SortedIndicesBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), MaxVisibleCount),
			*FString::Printf(TEXT("Gaussian.SortedIndices.S%d.C%d"), Binding.SceneId, Binding.ChunkIndex));

		FRDGBufferUAVRef SortKeysUAV = GraphBuilder.CreateUAV(SortKeysBuffer);

		{
			FGaussianSortKeysCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGaussianSortKeysCS::FParameters>();
			PassParameters->LocalToWorldMatrix = LocalToWorldMatrix;
			PassParameters->PreViewTranslation = PreViewTranslation;
			PassParameters->TranslatedViewMatrix = TranslatedViewMatrix;
			PassParameters->TranslatedWorldToClip = TranslatedWorldToClip;
			PassParameters->MaxVisibleCount = MaxVisibleCount;
			PassParameters->PaddedCount = PaddedCount;
			PassParameters->VisibleIndices = CullResult.VisibleIndicesSRV;
			PassParameters->VisibleCountBuffer = CullResult.VisibleCountSRV;
			PassParameters->GaussianPositions = Binding.PositionSRV;
			PassParameters->RWSortKeys = SortKeysUAV;

			const uint32 NumGroups = FMath::DivideAndRoundUp(PaddedCount, GaussianThreadGroupSize);
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("GaussianSimVerse::SortKeys S%u C%u", Binding.SceneId, Binding.ChunkIndex),
				ERDGPassFlags::Compute,
				SortKeysShader,
				PassParameters,
				FIntVector(NumGroups, 1, 1));
		}

		for (uint32 K = 2; K <= PaddedCount; K <<= 1)
		{
			for (uint32 J = K >> 1; J > 0; J >>= 1)
			{
				FGaussianBitonicSortCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGaussianBitonicSortCS::FParameters>();
				PassParameters->K = K;
				PassParameters->J = J;
				PassParameters->PaddedCount = PaddedCount;
				PassParameters->RWSortKeys = SortKeysUAV;

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

		FRDGBufferUAVRef SortedIndicesUAV = GraphBuilder.CreateUAV(SortedIndicesBuffer);
		{
			FGaussianSortExtractCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGaussianSortExtractCS::FParameters>();
			PassParameters->MaxVisibleCount = MaxVisibleCount;
			PassParameters->SortKeys = GraphBuilder.CreateSRV(SortKeysBuffer);
			PassParameters->VisibleCountBuffer = CullResult.VisibleCountSRV;
			PassParameters->RWSortedIndices = SortedIndicesUAV;

			const uint32 NumGroups = FMath::DivideAndRoundUp(PaddedCount, GaussianThreadGroupSize);
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("GaussianSimVerse::SortExtract S%u C%u", Binding.SceneId, Binding.ChunkIndex),
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
	const TShaderMapRef<FGaussianBinSplatsFillCS> BinFillShader(GlobalShaderMap);
	const TShaderMapRef<FGaussianTileSortCS> TileSortShader(GlobalShaderMap);
	const TShaderMapRef<FGaussianTileBlendCS> TileBlendShader(GlobalShaderMap);
	const bool bTileShadersValid = BinFillShader.IsValid() && TileSortShader.IsValid() && TileBlendShader.IsValid();
	if (!bTileShadersValid && !RasterShader.IsValid())
	{
		UE_LOG(LogGaussianSimVerse, Warning, TEXT("GaussianSimVerse raster shaders are not compiled for this platform/feature level."));
		return;
	}

	FRDGTextureRef SceneColorTexture = Inputs.SceneColorTexture;
	FRDGTextureDesc OverlayDesc = SceneColorTexture->Desc;
	OverlayDesc.Flags |= ETextureCreateFlags::ShaderResource | ETextureCreateFlags::UAV;
	FRDGTextureRef OverlayTexture = GraphBuilder.CreateTexture(OverlayDesc, TEXT("Gaussian.SplatOverlay"));
	FRDGTextureUAVRef OverlayUAV = GraphBuilder.CreateUAV(OverlayTexture);
	AddClearUAVPass(GraphBuilder, OverlayUAV, FVector4f(0.0f, 0.0f, 0.0f, 0.0f));

	const FVector4f ViewportRect(
		static_cast<float>(Inputs.ViewData.ViewRect.Min.X),
		static_cast<float>(Inputs.ViewData.ViewRect.Min.Y),
		static_cast<float>(Inputs.ViewData.ViewRect.Width()),
		static_cast<float>(Inputs.ViewData.ViewRect.Height()));

	const FVector4f ViewRect(
		static_cast<float>(Inputs.SceneColorViewRect.Min.X),
		static_cast<float>(Inputs.SceneColorViewRect.Min.Y),
		static_cast<float>(Inputs.SceneColorViewRect.Width()),
		static_cast<float>(Inputs.SceneColorViewRect.Height()));

	if (GaussianSimVerse::RenderSettings::IsRenderDebugEnabled()
		&& (Inputs.ViewData.ViewRect.Width() != Inputs.SceneColorViewRect.Width()
			|| Inputs.ViewData.ViewRect.Height() != Inputs.SceneColorViewRect.Height()
			|| Inputs.ViewData.ViewRect.Min != Inputs.SceneColorViewRect.Min))
	{
		static bool bLoggedViewportTextureMismatchOnce = false;
		if (!bLoggedViewportTextureMismatchOnce)
		{
			bLoggedViewportTextureMismatchOnce = true;
			UE_LOG(LogGaussianSimVerse, Log,
				TEXT("GaussianSimVerse viewport/texture rect mismatch (expected with screen %%): viewport (%d,%d %dx%d) texture (%d,%d %dx%d)"),
				Inputs.ViewData.ViewRect.Min.X, Inputs.ViewData.ViewRect.Min.Y,
				Inputs.ViewData.ViewRect.Width(), Inputs.ViewData.ViewRect.Height(),
				Inputs.SceneColorViewRect.Min.X, Inputs.SceneColorViewRect.Min.Y,
				Inputs.SceneColorViewRect.Width(), Inputs.SceneColorViewRect.Height());
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
	const uint32 ViewWidth = static_cast<uint32>(FMath::Max(0, Inputs.SceneColorViewRect.Width()));
	const uint32 ViewHeight = static_cast<uint32>(FMath::Max(0, Inputs.SceneColorViewRect.Height()));
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
						FVector(SceneProxy.Bounds.Origin)));
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
		bAdaptiveUseTilePath = GaussianSimVerse::RenderSettings::ShouldUseTileRasterPath(
			TileRasterMode,
			bTileShadersValid,
			0u,
			static_cast<int32>(MaxSplatsPerTile),
			AdaptiveViewDistance,
			AdaptiveSceneBoundsRadius,
			bAdaptiveLastTilePath);
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
					FVector(SceneProxy.Bounds.Origin)));
				SceneBoundsRadius = FVector(SceneProxy.Bounds.Extent).Size();
				break;
			}
		}

		bool bUseTilePath = false;
		if (TileRasterMode == 2)
		{
			bUseTilePath = bAdaptiveUseTilePath;
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
				bUnusedAdaptiveState);
		}
		if (!bUseTilePath && !RasterShader.IsValid() && bTileShadersValid)
		{
			bUseTilePath = true;
		}

		if (TileRasterMode == 2 && bTileShadersValid && RasterShader.IsValid()
			&& GaussianSimVerse::RenderSettings::IsRenderDebugEnabled())
		{
			const float SafeRadius = FMath::Max(SceneBoundsRadius, 1.0f);
			const float DistanceToRadius = ViewDistanceToScene / SafeRadius;
			UE_LOG(LogGaussianSimVerse, Log,
				TEXT("GaussianSimVerse raster S%u: %s (dist=%.0f radius=%.0f dist/radius=%.1f splats/tile~%u)"),
				Binding.SceneId,
				bUseTilePath ? TEXT("tile") : TEXT("global"),
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
			BlendParameters->TileOffsets = TileOffsetsSRV;
			BlendParameters->TileCounts = TileFillCounterSRV;
			BlendParameters->TileSplats = TileSplatsSRV;
			BlendParameters->SortedIndices = SortedIndicesSRV;
			BlendParameters->GaussianSplatsVec4 = Binding.SplatSRV;
			BlendParameters->RWOverlay = OverlayUAV;

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("GaussianSimVerse::TileBlend S%u C%u (%ux%u tiles)", Binding.SceneId, Binding.ChunkIndex, NumTilesX, NumTilesY),
				ERDGPassFlags::Compute,
				TileBlendShader,
				BlendParameters,
				FIntVector(NumTilesX, NumTilesY, 1));
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
				PassParameters->SortedIndices = SortedIndicesSRV;
				PassParameters->VisibleCountBuffer = CullResult.VisibleCountSRV;
				PassParameters->GaussianSplatsVec4 = Binding.SplatSRV;
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
			FRDGTextureRef MergedTexture = GraphBuilder.CreateTexture(OverlayDesc, TEXT("Gaussian.MergedSceneColor"));
			FRDGTextureUAVRef MergedUAV = GraphBuilder.CreateUAV(MergedTexture);
			AddCopyTexturePass(GraphBuilder, SceneColorTexture, MergedTexture);

			FGaussianCompositeCS::FParameters* CompositeParams = GraphBuilder.AllocParameters<FGaussianCompositeCS::FParameters>();
			CompositeParams->ViewRect = ViewRect;
			CompositeParams->SceneColorTexture = SceneColorTexture;
			CompositeParams->OverlayTexture = OverlayTexture;
			CompositeParams->RWSceneColor = MergedUAV;

			const uint32 GroupsX = FMath::DivideAndRoundUp(static_cast<uint32>(Inputs.SceneColorViewRect.Width()), 8u);
			const uint32 GroupsY = FMath::DivideAndRoundUp(static_cast<uint32>(Inputs.SceneColorViewRect.Height()), 8u);
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
