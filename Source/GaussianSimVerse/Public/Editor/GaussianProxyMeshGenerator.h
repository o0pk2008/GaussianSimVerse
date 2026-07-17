// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_EDITOR

class UStaticMesh;
class UGaussianAsset;
class UGaussianStreamedSceneAsset;
class UObject;

/** Settings for editor-time proxy mesh generation (splat-transform collision inspired MVP). */
struct FGaussianProxyMeshBuildSettings
{
	/** Voxel edge length in Unreal units (cm). Smaller = denser mesh. */
	float VoxelSizeCm = 2.0f;

	/** Ignore splats with opacity below this. Low (~0.05) keeps a denser silhouette. */
	float MinOpacity = 0.05f;

	/** Cap sampled points to keep generation interactive. */
	int32 MaxSamplePoints = 400000;

	/** Expand solid voxels by this many rings (0 = tight fit). Helps seal thin surfaces only. */
	int32 DilateRings = 0;

	/**
	 * Erode solid by this many rings after dilate (pulls silhouette inward).
	 * 1 often removes one voxel of "fat" from fringe occupancy.
	 */
	int32 ShrinkRings = 0;

	/**
	 * Require this many sample hits in a cell before it becomes solid.
	 * 1 keeps every hit (best silhouette); 2+ drops sparse fringe.
	 */
	int32 MinHitsPerVoxel = 1;

	/**
	 * When true, mesh vertices are recentered to AABB center (needs component offset in the scene).
	 * Default false: keep Gaussian actor-local / dataset coordinates so asset preview matches splats.
	 */
	bool bCenterMeshAtOrigin = false;

	/** Package path without asset name, e.g. /Game/MyScene (defaults next to source asset). */
	FString PackagePath = TEXT("/Game/GaussianProxies");
};

/**
 * Builds a simple solid-surface StaticMesh from Gaussian sample points
 * (voxelize → exposed-face mesh), suitable as a depth/collision proxy.
 */
class GAUSSIANSIMVERSE_API FGaussianProxyMeshGenerator
{
public:
	/**
	 * Voxelize points and create a UStaticMesh asset.
	 * @param OutLocalOffset If bCenterMeshAtOrigin, the translation to apply on the mesh component
	 *        so world/actor-local placement still matches the Gaussian.
	 */
	static UStaticMesh* BuildMeshFromPoints(
		const TArray<FVector>& Points,
		const FGaussianProxyMeshBuildSettings& Settings,
		const FString& AssetName,
		FString& OutError,
		FVector* OutLocalOffset = nullptr);

	/** Sample a non-streamed Gaussian asset (positions relative to Bounds.Origin). */
	static bool CollectPointsFromAsset(
		const UGaussianAsset* Asset,
		const FGaussianProxyMeshBuildSettings& Settings,
		TArray<FVector>& OutPoints,
		FString& OutError);

	/**
	 * Sample a streamed SOG dataset from disk (absolute dataset/UE coordinates, pivot 0).
	 * Prefers finest LOD slices so coarse LODs do not fatten the occupancy.
	 */
	static bool CollectPointsFromStreamedAsset(
		const UGaussianStreamedSceneAsset* Asset,
		const FGaussianProxyMeshBuildSettings& Settings,
		TArray<FVector>& OutPoints,
		FString& OutError,
		TFunction<void(const FString&)> Progress = nullptr);
};

#endif // WITH_EDITOR
