// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Rendering/GaussianGPUResources.h"
#include "RenderGraphResources.h"

class FGaussianGPUBuffer;

/** Per-chunk RDG handles produced each frame after upload. */
struct GAUSSIANSIMVERSE_API FGaussianRDGBufferBinding
{
	FGaussianGPUBuffer* SourceBuffer = nullptr;
	FRDGBufferRef SplatBuffer = nullptr;
	FRDGBufferRef PositionBuffer = nullptr;
	FRDGBufferSRVRef SplatSRV = nullptr;
	FRDGBufferSRVRef PositionSRV = nullptr;
	uint32 NumGaussians = 0;
	uint32 SceneId = INDEX_NONE;
	uint32 ChunkIndex = INDEX_NONE;
	FMatrix LocalToWorld = FMatrix::Identity;
	float SplatScale = 1.0f;
	float AlphaCullThreshold = 0.007843137f;
	float CutoffK = 7.0f;
	float CovarianceDilation = 0.3f;
	uint32 ImportedShDegree = 0;
	uint32 RenderShDegree = 3;
	uint32 bHasShCoefficients = 0;
	FRDGBufferSRVRef ShCoefficientsSRV = nullptr;
};

/** Per-chunk GPU frustum cull output (Phase 3+). */
struct GAUSSIANSIMVERSE_API FGaussianRDGCullResult
{
	int32 GPUBindingIndex = INDEX_NONE;
	FRDGBufferRef VisibleIndicesBuffer = nullptr;
	FRDGBufferSRVRef VisibleIndicesSRV = nullptr;
	FRDGBufferRef VisibleCountBuffer = nullptr;
	FRDGBufferSRVRef VisibleCountSRV = nullptr;
	uint32 MaxVisibleCount = 0;
};

/** Per-chunk depth-sorted indices (Phase 4+). */
struct GAUSSIANSIMVERSE_API FGaussianRDGSortResult
{
	int32 CullResultIndex = INDEX_NONE;
	FRDGBufferRef SortedIndicesBuffer = nullptr;
	FRDGBufferSRVRef SortedIndicesSRV = nullptr;
	uint32 VisibleCount = 0;
};

/**
 * CPU staging + render-thread pooled buffers for one UGaussianAsset.
 * Not an FRenderResource: lifetime is owned by UGaussianAsset on the game thread.
 */
class GAUSSIANSIMVERSE_API FGaussianGPUBuffer
{
public:
	void SetCPUData(TArray<FGaussianSplatGPU>&& InSplatData);
	void SetCPUDataFromStaging(const TArray<FGaussianSplatData>& StagingData);
	void SetCPUDataFromStaging(
		const TArray<FGaussianSplatData>& StagingData,
		const TArray<float>& ShCoefficients,
		int32 InImportedShDegree);
	void MarkDirty();

	uint32 GetNumGaussians() const { return NumGaussians; }
	bool IsDirty() const { return bDirty; }
	bool HasValidData() const { return NumGaussians > 0; }

	/** Upload dirty data and register pooled buffers with the current RDG graph. Render thread only. */
	void CommitToGPU(FRDGBuilder& GraphBuilder, FGaussianRDGBufferBinding& OutBinding);

	/** Release pooled GPU buffers. Render thread only. */
	void ReleaseRenderResources();

private:
	void ReleasePooledBuffers();
	void EnsurePooledBuffers(uint32 InNumGaussians);
	void UploadToPooledBuffers(FRDGBuilder& GraphBuilder);

	TArray<FGaussianSplatGPU> SplatCPUData;
	TArray<FVector4f> PositionCPUData;
	TArray<float> ShCoefficientCPUData;
	uint32 NumGaussians = 0;
	uint32 ImportedShDegree = 0;
	bool bDirty = true;

	TRefCountPtr<FRDGPooledBuffer> SplatPooledBuffer;
	TRefCountPtr<FRDGPooledBuffer> PositionPooledBuffer;
	TRefCountPtr<FRDGPooledBuffer> ShCoefficientPooledBuffer;
};
