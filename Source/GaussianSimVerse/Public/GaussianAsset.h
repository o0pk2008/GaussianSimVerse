// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GaussianTypes.h"
#include "Rendering/GaussianGPUResources.h"
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

	/**
	 * Dataset AABB (origin = center, extent = half-size).
	 * When bUsesDatasetCoordinates is false (legacy), splat positions in bulk are relative to Origin
	 * and the scene actor snaps to Origin.
	 * When true (default for new imports), positions are absolute SuperSplat/dataset coordinates
	 * (same frame as streamed LOD with DatasetPivot=0) and the actor stays at the world origin.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian")
	FGaussianBounds Bounds;

	/**
	 * True: bulk positions are in dataset/UE absolute frame (match streamed LOD / SuperSplat).
	 * False: legacy centered storage (positions relative to Bounds.Origin).
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian")
	bool bUsesDatasetCoordinates = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian|SOG")
	TArray<TObjectPtr<class UTexture2D>> SourceTextures;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian")
	int32 GaussianCount = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian")
	TArray<FGaussianLODInfo> LODLevels;

	/** Highest SH band present in imported coefficient data (0..3). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian|Rendering")
	int32 ImportedShDegree = 0;

	bool IsValidForRendering() const;

	/** Initialize or refresh the GPU buffer from stored bulk data. Game thread only. */
	void InitGPUResources();

	/** Release GPU resources. Game thread only. */
	void ReleaseGPUResources();

	/** Replace stored splat payload and bounds. Game thread only. */
	void SetStagingData(const TArray<FGaussianSplatData>& InStagingData);
	void SetStagingData(
		const TArray<FGaussianSplatData>& InStagingData,
		TArray<float>&& InShCoefficients,
		int32 InImportedShDegree);

	/**
	 * Streamed commit path: staging already centered, GPU layout already converted on a worker.
	 * Avoids ConvertSplatDataArray / BuildPositionBuffer on the game thread.
	 */
	void SetPreparedStreamingData(
		TArray<FGaussianSplatData>&& InCenteredStaging,
		TArray<float>&& InShCoefficients,
		int32 InImportedShDegree,
		const FGaussianBounds& InBounds,
		TArray<FGaussianSplatGPU>&& InGpuSplats,
		TArray<FVector4f>&& InPositions);

	void SetSourceTextures(const TArray<class UTexture2D*>& InTextures);

	/** Approximate on-disk / in-memory payload size in megabytes. */
	float GetPayloadSizeMB() const;

	/**
	 * Ensure CPU staging is available and sample splat centers for proxy-mesh generation.
	 * Positions match rendering frame (dataset absolute if bUsesDatasetCoordinates, else relative to Bounds.Origin).
	 * @param VoxelSizeCm When > 0, only large splats (vs voxel size) get scale shells so centers stay dense.
	 * @return Number of points appended.
	 */
	int32 CollectProxySamplePoints(TArray<FVector>& OutPoints, float MinOpacity, int32 MaxPoints, float VoxelSizeCm = 0.0f) const;

	TSharedPtr<FGaussianGPUBuffer, ESPMode::ThreadSafe> GetGPUBufferShared() const { return GPUBuffer; }
	FGaussianGPUBuffer* GetGPUBuffer() const { return GPUBuffer.Get(); }

private:
	void EnsureStagingLoaded() const;
	void EnsureShCoefficientsLoaded() const;
	void EncodeStagingToBulk(const TArray<FGaussianSplatData>& InStagingData);
	void EncodeShCoefficientsToBulk(const TArray<float>& InShCoefficients);
	static FGaussianBounds ComputeBounds(const TArray<FGaussianSplatData>& Splats);

	/** Serialized splat payload. Not a UPROPERTY to avoid editor details freeze. */
	TArray<uint8> BulkSplatData;

	/** Per-splat SH coefficients: f_dc(3) + f_rest(45). Empty for legacy imports. */
	TArray<uint8> BulkShCoefficientData;

	mutable TArray<FGaussianSplatData> StagingCache;
	mutable bool bStagingCacheLoaded = false;

	mutable TArray<float> ShCoefficientCache;
	mutable bool bShCoefficientCacheLoaded = false;

	TSharedPtr<FGaussianGPUBuffer, ESPMode::ThreadSafe> GPUBuffer;
};
