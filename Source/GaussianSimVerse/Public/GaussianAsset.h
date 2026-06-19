// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GaussianTypes.h"
#include "GaussianAsset.generated.h"

class FGaussianGPUBuffer;

/**
 * Persistent Gaussian splat data asset.
 * Large splat payloads are stored as a compact binary blob (not exposed in the editor).
 */
UCLASS(BlueprintType)
class GAUSSIANSIMVERSE_API UGaussianAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	UGaussianAsset();
	virtual void BeginDestroy() override;
	virtual void Serialize(FArchive& Ar) override;

	/** Original disk import path (serialized, hidden from details). */
	FString ImportSourcePath;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian")
	EGaussianSourceFormat SourceFormat = EGaussianSourceFormat::Unknown;

	/** World-space placement center. Splat positions are stored relative to this point. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian")
	FGaussianBounds Bounds;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian|SOG")
	TArray<TObjectPtr<class UTexture2D>> SourceTextures;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian")
	int32 GaussianCount = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian")
	TArray<FGaussianLODInfo> LODLevels;

	bool IsValidForRendering() const;

	/** Initialize or refresh the GPU buffer from stored bulk data. Game thread only. */
	void InitGPUResources();

	/** Release GPU resources. Game thread only. */
	void ReleaseGPUResources();

	/** Replace stored splat payload and bounds. Game thread only. */
	void SetStagingData(const TArray<FGaussianSplatData>& InStagingData);

	void SetSourceTextures(const TArray<class UTexture2D*>& InTextures);

	/** Approximate on-disk / in-memory payload size in megabytes. */
	float GetPayloadSizeMB() const;

	TSharedPtr<FGaussianGPUBuffer, ESPMode::ThreadSafe> GetGPUBufferShared() const { return GPUBuffer; }
	FGaussianGPUBuffer* GetGPUBuffer() const { return GPUBuffer.Get(); }

private:
	void EnsureStagingLoaded() const;
	void EncodeStagingToBulk(const TArray<FGaussianSplatData>& InStagingData);
	static FGaussianBounds ComputeBounds(const TArray<FGaussianSplatData>& Splats);

	/** Serialized splat payload. Not a UPROPERTY to avoid editor details freeze. */
	TArray<uint8> BulkSplatData;

	mutable TArray<FGaussianSplatData> StagingCache;
	mutable bool bStagingCacheLoaded = false;

	TSharedPtr<FGaussianGPUBuffer, ESPMode::ThreadSafe> GPUBuffer;
};
