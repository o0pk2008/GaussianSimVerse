// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GaussianTypes.h"

namespace GaussianImport
{
	/** PlayCanvas / SOG SH DC constant. */
	constexpr float SH_C0 = 0.28209479177387814f;

	constexpr float Sqrt2 = 1.41421356237f;

	/** 3DGS / SOG data is in meters; UE world units are centimeters. */
	constexpr float MetersToCentimeters = 100.0f;

	GAUSSIANSIMVERSEEDITOR_API float Sigmoid(float X);
	GAUSSIANSIMVERSEEDITOR_API float UnlogPosition(float N);
	GAUSSIANSIMVERSEEDITOR_API FVector3f LerpVec3(const FVector3f& Min, const FVector3f& Max, const FVector3f& T);

	/** SuperSplat / 3DGS authoring → UE cm: (x,y,z)→(x,-z,-y)*100 (matches SuperSplat; Scale.Y=-1 baked in). */
	GAUSSIANSIMVERSEEDITOR_API FVector3f PlayCanvasToUEPosition(const FVector3f& Position);

	/** Quaternion (w,x,y,z) in authoring frame to UE (x,y,z,w). */
	GAUSSIANSIMVERSEEDITOR_API FVector4f PlayCanvasToUERotation(float W, float X, float Y, float Z);

	/** PLY → UE; same basis as SOG / PlayCanvasToUEPosition. */
	GAUSSIANSIMVERSEEDITOR_API FVector3f PlyToUEPosition(const FVector3f& Position);

	/** PLY direction to UE without unit conversion (same basis as PlyToUEPosition). */
	GAUSSIANSIMVERSEEDITOR_API FVector3f PlyToUEDirection(const FVector3f& Direction);

	/** Convert meter-based Gaussian scale to UE centimeters (PlayCanvas / SOG). */
	GAUSSIANSIMVERSEEDITOR_API FVector3f MetersToUEScale(const FVector3f& Scale);

	/** PLY scale to UE centimeters (no axis permutation; rotation carries orientation). */
	GAUSSIANSIMVERSEEDITOR_API FVector3f PlyMetersToUEScale(const FVector3f& Scale);

	/** PLY quaternion (w,x,y,z) to UE (x,y,z,w) — same path as SOG. */
	GAUSSIANSIMVERSEEDITOR_API FVector4f PlyToUERotation(float W, float X, float Y, float Z);

	GAUSSIANSIMVERSEEDITOR_API FVector4f SH0ToLinearColor(float Fdc0, float Fdc1, float Fdc2, float Opacity);

	GAUSSIANSIMVERSEEDITOR_API void ComputeBounds(const TArray<FGaussianSplatData>& Splats, FGaussianBounds& OutBounds);
}
