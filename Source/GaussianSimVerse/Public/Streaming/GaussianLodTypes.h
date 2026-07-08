// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GaussianTypes.h"
#include "GaussianLodTypes.generated.h"

/** One LOD slice inside a streamed SOG chunk file. */
USTRUCT(BlueprintType)
struct GAUSSIANSIMVERSE_API FGaussianLodSlice
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian")
	int32 LodLevel = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian")
	int32 FileIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian")
	int32 Offset = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian")
	int32 Count = 0;
};

/** Spatial tree node from lod-meta.json (PlayCanvas Streamed SOG). */
USTRUCT()
struct GAUSSIANSIMVERSE_API FGaussianLodTreeNode
{
	GENERATED_BODY()

	UPROPERTY()
	FGaussianBounds Bounds;

	UPROPERTY()
	TArray<FGaussianLodSlice> LodSlices;

	/** Assigned once at lod-meta parse time; stable across frames for LOD sibling matching. */
	int32 LeafId = INDEX_NONE;

	/** Recursive children are not UPROPERTY (UHT does not support struct recursion). */
	TArray<FGaussianLodTreeNode> Children;

	bool IsLeaf() const { return LodSlices.Num() > 0; }
};

/** Serializable lod-meta.json fields stored on UGaussianStreamedSceneAsset. */
USTRUCT(BlueprintType)
struct GAUSSIANSIMVERSE_API FGaussianLodMetaData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian")
	int32 LodLevels = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian")
	FString EnvironmentRelativePath;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian")
	TArray<FString> Filenames;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian")
	FGaussianBounds SceneBounds;
};

/** Unique key for a resident streamed chunk subset. */
USTRUCT()
struct GAUSSIANSIMVERSE_API FGaussianStreamChunkKey
{
	GENERATED_BODY()

	UPROPERTY()
	FString KeyString;

	UPROPERTY()
	int32 FileIndex = INDEX_NONE;

	UPROPERTY()
	int32 Offset = 0;

	UPROPERTY()
	int32 Count = 0;

	UPROPERTY()
	int32 LodLevel = 0;

	/** Stable id of the octree leaf this slice belongs to (links LOD siblings). */
	UPROPERTY()
	int32 LeafId = INDEX_NONE;

	UPROPERTY()
	bool bEnvironment = false;

	static FGaussianStreamChunkKey MakeEnvironment()
	{
		FGaussianStreamChunkKey Key;
		Key.bEnvironment = true;
		Key.KeyString = TEXT("env");
		return Key;
	}

	static FGaussianStreamChunkKey MakeSlice(int32 InFileIndex, int32 InOffset, int32 InCount, int32 InLodLevel, int32 InLeafId)
	{
		FGaussianStreamChunkKey Key;
		Key.FileIndex = InFileIndex;
		Key.Offset = InOffset;
		Key.Count = InCount;
		Key.LodLevel = InLodLevel;
		Key.LeafId = InLeafId;
		// Include LeafId so LOD siblings share a spatial identity while remaining unique keys.
		Key.KeyString = FString::Printf(TEXT("L%d_%d_%d_%d_%d"), InLeafId, InFileIndex, InOffset, InCount, InLodLevel);
		return Key;
	}

	friend uint32 GetTypeHash(const FGaussianStreamChunkKey& Key)
	{
		return GetTypeHash(Key.KeyString);
	}

	bool operator==(const FGaussianStreamChunkKey& Other) const
	{
		return KeyString == Other.KeyString;
	}
};

UENUM(BlueprintType)
enum class EGaussianChunkLoadState : uint8
{
	Unloaded,
	Loading,
	Loaded,
	Failed
};
