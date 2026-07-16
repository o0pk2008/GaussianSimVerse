// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianAsset.h"
#include "GaussianAssetVersion.h"
#include "Engine/Texture2D.h"
#include "Rendering/GaussianGPUBuffer.h"
#include "RenderingThread.h"

UGaussianAsset::UGaussianAsset()
{
}

void UGaussianAsset::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);
	Ar.UsingCustomVersion(FGaussianAssetSerializationVersion::GUID);

	Ar << BulkSplatData;
	Ar << ImportSourcePath;

	if (Ar.CustomVer(FGaussianAssetSerializationVersion::GUID) >= FGaussianAssetSerializationVersion::AddedShCoefficientData)
	{
		Ar << BulkShCoefficientData;
		Ar << ImportedShDegree;
	}
	else if (Ar.IsLoading())
	{
		BulkShCoefficientData.Reset();
		ImportedShDegree = 0;
	}

	if (Ar.IsLoading())
	{
		bStagingCacheLoaded = false;
		StagingCache.Empty();
		ShCoefficientCache.Empty();
		bShCoefficientCacheLoaded = false;
		GaussianCount = BulkSplatData.Num() / static_cast<int32>(sizeof(FGaussianSplatData));
	}
}

void UGaussianAsset::BeginDestroy()
{
	ReleaseGPUResources();
	FlushRenderingCommands();
	StagingCache.Empty();
	ShCoefficientCache.Empty();
	bStagingCacheLoaded = false;
	bShCoefficientCacheLoaded = false;
	Super::BeginDestroy();
}

bool UGaussianAsset::IsValidForRendering() const
{
	if (GaussianCount <= 0)
	{
		return false;
	}

	// Imported / serialized assets keep payload in BulkSplatData.
	if (BulkSplatData.Num() > 0)
	{
		return true;
	}

	// Streamed path: SetPreparedStreamingData intentionally skips Bulk to save RAM.
	// Validity comes from the GPU buffer (NumGaussians stays set after CPU staging is uploaded).
	return GPUBuffer.IsValid() && GPUBuffer->HasValidData();
}

float UGaussianAsset::GetPayloadSizeMB() const
{
	return static_cast<float>(BulkSplatData.Num()) / (1024.0f * 1024.0f);
}

FGaussianBounds UGaussianAsset::ComputeBounds(const TArray<FGaussianSplatData>& Splats)
{
	FGaussianBounds Result;
	if (Splats.Num() == 0)
	{
		return Result;
	}

	FBox Box(ForceInit);
	for (const FGaussianSplatData& Splat : Splats)
	{
		Box += FVector(Splat.Position);
	}

	if (Box.IsValid)
	{
		Result.Origin = FVector3f(Box.GetCenter());
		Result.Extent = FVector3f(Box.GetExtent());
	}

	return Result;
}

void UGaussianAsset::EncodeStagingToBulk(const TArray<FGaussianSplatData>& InStagingData)
{
	const int32 ByteCount = InStagingData.Num() * static_cast<int32>(sizeof(FGaussianSplatData));
	BulkSplatData.SetNumUninitialized(ByteCount);
	if (ByteCount > 0)
	{
		FMemory::Memcpy(BulkSplatData.GetData(), InStagingData.GetData(), ByteCount);
	}
	else
	{
		BulkSplatData.Reset();
	}
}

void UGaussianAsset::EnsureShCoefficientsLoaded() const
{
	if (bShCoefficientCacheLoaded)
	{
		return;
	}

	const int32 FloatCount = BulkShCoefficientData.Num() / static_cast<int32>(sizeof(float));
	ShCoefficientCache.SetNumUninitialized(FloatCount);
	if (FloatCount > 0)
	{
		FMemory::Memcpy(ShCoefficientCache.GetData(), BulkShCoefficientData.GetData(), BulkShCoefficientData.Num());
	}

	bShCoefficientCacheLoaded = true;
}

void UGaussianAsset::EncodeShCoefficientsToBulk(const TArray<float>& InShCoefficients)
{
	const int32 ByteCount = InShCoefficients.Num() * static_cast<int32>(sizeof(float));
	BulkShCoefficientData.SetNumUninitialized(ByteCount);
	if (ByteCount > 0)
	{
		FMemory::Memcpy(BulkShCoefficientData.GetData(), InShCoefficients.GetData(), ByteCount);
	}
	else
	{
		BulkShCoefficientData.Reset();
	}
}

void UGaussianAsset::EnsureStagingLoaded() const
{
	if (bStagingCacheLoaded)
	{
		return;
	}

	const int32 SplatSize = static_cast<int32>(sizeof(FGaussianSplatData));
	const int32 Count = BulkSplatData.Num() / SplatSize;
	StagingCache.SetNumUninitialized(Count);
	if (Count > 0)
	{
		FMemory::Memcpy(StagingCache.GetData(), BulkSplatData.GetData(), Count * SplatSize);
	}

	bStagingCacheLoaded = true;
}

void UGaussianAsset::InitGPUResources()
{
	if (!IsValidForRendering())
	{
		return;
	}

	EnsureStagingLoaded();
	EnsureShCoefficientsLoaded();

	if (!GPUBuffer.IsValid())
	{
		GPUBuffer = MakeShared<FGaussianGPUBuffer, ESPMode::ThreadSafe>();
	}

	GPUBuffer->SetCPUDataFromStaging(StagingCache, ShCoefficientCache, ImportedShDegree);
	GaussianCount = StagingCache.Num();
}

void UGaussianAsset::ReleaseGPUResources()
{
	if (!GPUBuffer.IsValid())
	{
		return;
	}

	TSharedPtr<FGaussianGPUBuffer, ESPMode::ThreadSafe> BufferToRelease = MoveTemp(GPUBuffer);

	ENQUEUE_RENDER_COMMAND(GaussianSimVerse_ReleaseAssetGPU)(
		[BufferToRelease = MoveTemp(BufferToRelease)](FRHICommandListImmediate& RHICmdList) mutable
		{
			if (BufferToRelease.IsValid())
			{
				BufferToRelease->ReleaseRenderResources();
			}
		});
}

void UGaussianAsset::SetStagingData(const TArray<FGaussianSplatData>& InStagingData)
{
	SetStagingData(InStagingData, TArray<float>(), 0);
}

void UGaussianAsset::SetStagingData(
	const TArray<FGaussianSplatData>& InStagingData,
	TArray<float>&& InShCoefficients,
	int32 InImportedShDegree)
{
	Bounds = ComputeBounds(InStagingData);
	ImportedShDegree = FMath::Clamp(InImportedShDegree, 0, 3);

	TArray<FGaussianSplatData> CenteredData = InStagingData;
	const FVector3f WorldCenter = Bounds.Origin;
	for (FGaussianSplatData& Splat : CenteredData)
	{
		Splat.Position -= WorldCenter;
	}

	EncodeStagingToBulk(CenteredData);
	EncodeShCoefficientsToBulk(InShCoefficients);
	GaussianCount = CenteredData.Num();

	StagingCache = MoveTemp(CenteredData);
	bStagingCacheLoaded = true;
	ShCoefficientCache = MoveTemp(InShCoefficients);
	bShCoefficientCacheLoaded = true;

	if (GPUBuffer.IsValid())
	{
		GPUBuffer->SetCPUDataFromStaging(StagingCache, ShCoefficientCache, ImportedShDegree);
	}
}

void UGaussianAsset::SetPreparedStreamingData(
	TArray<FGaussianSplatData>&& InCenteredStaging,
	TArray<float>&& InShCoefficients,
	int32 InImportedShDegree,
	const FGaussianBounds& InBounds,
	TArray<FGaussianSplatGPU>&& InGpuSplats,
	TArray<FVector4f>&& InPositions)
{
	// Streamed path: GPU layout is the source of truth. Do NOT keep Bulk + StagingCache + SH
	// duplicates — that tripled RAM and OOM'd during aggressive LOD catch-up.
	Bounds = InBounds;
	ImportedShDegree = FMath::Clamp(InImportedShDegree, 0, 3);
	GaussianCount = InGpuSplats.Num() > 0 ? InGpuSplats.Num() : InCenteredStaging.Num();

	InCenteredStaging.Empty();
	BulkSplatData.Empty();
	BulkShCoefficientData.Empty();
	StagingCache.Empty();
	bStagingCacheLoaded = true;
	ShCoefficientCache.Empty();
	bShCoefficientCacheLoaded = true;

	if (!GPUBuffer.IsValid())
	{
		GPUBuffer = MakeShared<FGaussianGPUBuffer, ESPMode::ThreadSafe>();
	}

	GPUBuffer->SetCPUDataPrepared(
		MoveTemp(InGpuSplats),
		MoveTemp(InPositions),
		MoveTemp(InShCoefficients),
		ImportedShDegree);
}

void UGaussianAsset::SetSourceTextures(const TArray<UTexture2D*>& InTextures)
{
	SourceTextures.Reset();
	for (UTexture2D* Texture : InTextures)
	{
		SourceTextures.Add(Texture);
	}
}
