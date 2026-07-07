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

/** Spherical harmonics band used at render time (SH0 = diffuse only). */
UENUM(BlueprintType)
enum class EGaussianSHBand : uint8
{
	SH0 UMETA(DisplayName = "SH0"),
	SH1 UMETA(DisplayName = "SH1"),
	SH2 UMETA(DisplayName = "SH2"),
	SH3 UMETA(DisplayName = "SH3"),
};

constexpr int32 GaussianShRestCoefficientCount = 45;
constexpr int32 GaussianShDcCoefficientCount = 3;
constexpr int32 GaussianShCoefficientsPerSplat = GaussianShDcCoefficientCount + GaussianShRestCoefficientCount;

/** SuperSplat-style per-scene color grading (UI transparency is log-space; shader uses exp). */
USTRUCT(BlueprintType)
struct GAUSSIANSIMVERSE_API FGaussianColorAdjustment
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Colors", meta = (ClampMin = "-0.5", ClampMax = "0.5", UIMin = "-0.5", UIMax = "0.5"))
	float Temperature = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Colors", meta = (ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "2.0"))
	float Saturation = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Colors", meta = (ClampMin = "-1.0", ClampMax = "1.0", UIMin = "-1.0", UIMax = "1.0"))
	float Brightness = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Colors", meta = (ClampMin = "0.0", ClampMax = "0.5", UIMin = "0.0", UIMax = "0.5"))
	float BlackPoint = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Colors", meta = (ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	float WhitePoint = 1.0f;

	/** Log-space transparency slider (0 = neutral; shader multiplier = exp(value)). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Colors", meta = (ClampMin = "-6.0", ClampMax = "6.0", UIMin = "-6.0", UIMax = "6.0"))
	float Transparency = 0.0f;
};

/** GPU uniforms derived from FGaussianColorAdjustment (SuperSplat clrOffset/clrScale/saturation). */
struct FGaussianColorGradeGPU
{
	FVector3f ClrOffset = FVector3f::ZeroVector;
	FVector3f ClrScaleRGB = FVector3f::OneVector;
	float Saturation = 1.0f;
	float TransparencyMultiplier = 1.0f;

	static FGaussianColorGradeGPU FromAdjustment(const FGaussianColorAdjustment& Adjustment)
	{
		const float Range = FMath::Max(Adjustment.WhitePoint - Adjustment.BlackPoint, KINDA_SMALL_NUMBER);
		const float Scale = 1.0f / Range;

		FGaussianColorGradeGPU Out;
		Out.ClrOffset = FVector3f(-Adjustment.BlackPoint + Adjustment.Brightness);
		Out.ClrScaleRGB = FVector3f(
			Scale * (1.0f + Adjustment.Temperature),
			Scale,
			Scale * (1.0f - Adjustment.Temperature));
		Out.Saturation = Adjustment.Saturation;
		Out.TransparencyMultiplier = FMath::Exp(Adjustment.Transparency);
		return Out;
	}
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
