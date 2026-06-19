// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GaussianTypes.generated.h"

/** Supported Gaussian splat file formats (import in Editor, stream at Runtime). */
UENUM(BlueprintType)
enum class EGaussianSourceFormat : uint8
{
	Unknown UMETA(Hidden),
	PLY     UMETA(DisplayName = "PLY"),
	Splat   UMETA(DisplayName = ".splat"),
	SOG     UMETA(DisplayName = "SOG"),
	SPZ     UMETA(DisplayName = "SPZ"),
};

/** Per-Gaussian attributes stored on GPU (Phase 2+). Layout matches shader GaussianCommon.ush. */
USTRUCT(BlueprintType)
struct GAUSSIANSIMVERSE_API FGaussianSplatData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian")
	FVector3f Position = FVector3f::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian")
	FVector4f Rotation = FVector4f(0.f, 0.f, 0.f, 1.f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian")
	FVector3f Scale = FVector3f(0.01f, 0.01f, 0.01f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian")
	FVector4f Color = FVector4f(1.f, 1.f, 1.f, 1.f);
};

/** Spatial bounds for streaming and culling. */
USTRUCT(BlueprintType)
struct GAUSSIANSIMVERSE_API FGaussianBounds
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian")
	FVector3f Origin = FVector3f::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian")
	FVector3f Extent = FVector3f(100.f, 100.f, 100.f);

	FBox GetBox() const
	{
		return FBox(FVector(Origin - Extent), FVector(Origin + Extent));
	}
};

/** LOD metadata for a streamed chunk (Phase 5+). */
USTRUCT(BlueprintType)
struct GAUSSIANSIMVERSE_API FGaussianLODInfo
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian")
	int32 LODIndex = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian")
	int32 GaussianCount = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian")
	float ScreenSizeThreshold = 0.0f;
};
