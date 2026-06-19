// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianAsset.h"
#include "Engine/Texture2D.h"
#include "Rendering/GaussianGPUBuffer.h"
#include "RenderingThread.h"

UGaussianAsset::UGaussianAsset()
{
}

void UGaussianAsset::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);
	Ar << BulkSplatData;
	Ar << ImportSourcePath;

	if (Ar.IsLoading())
	{
		bStagingCacheLoaded = false;
		StagingCache.Empty();
		GaussianCount = BulkSplatData.Num() / static_cast<int32>(sizeof(FGaussianSplatData));
	}
}

void UGaussianAsset::BeginDestroy()
{
	ReleaseGPUResources();
	FlushRenderingCommands();
	StagingCache.Empty();
	bStagingCacheLoaded = false;
	Super::BeginDestroy();
}

bool UGaussianAsset::IsValidForRendering() const
{
	return GaussianCount > 0 && BulkSplatData.Num() > 0;
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

	if (!GPUBuffer.IsValid())
	{
		GPUBuffer = MakeShared<FGaussianGPUBuffer, ESPMode::ThreadSafe>();
	}

	GPUBuffer->SetCPUDataFromStaging(StagingCache);
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
	Bounds = ComputeBounds(InStagingData);

	TArray<FGaussianSplatData> CenteredData = InStagingData;
	const FVector3f WorldCenter = Bounds.Origin;
	for (FGaussianSplatData& Splat : CenteredData)
	{
		Splat.Position -= WorldCenter;
	}

	EncodeStagingToBulk(CenteredData);
	GaussianCount = CenteredData.Num();

	StagingCache = MoveTemp(CenteredData);
	bStagingCacheLoaded = true;

	if (GPUBuffer.IsValid())
	{
		GPUBuffer->SetCPUDataFromStaging(StagingCache);
	}
}

void UGaussianAsset::SetSourceTextures(const TArray<UTexture2D*>& InTextures)
{
	SourceTextures.Reset();
	for (UTexture2D* Texture : InTextures)
	{
		SourceTextures.Add(Texture);
	}
}
