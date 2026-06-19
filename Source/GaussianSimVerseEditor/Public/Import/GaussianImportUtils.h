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

	/** PlayCanvas RH (Y-up, Z-back) position to UE LH Z-up. */
	GAUSSIANSIMVERSEEDITOR_API FVector3f PlayCanvasToUEPosition(const FVector3f& Position);

	/** Quaternion stored as (w,x,y,z) in PlayCanvas space to UE (x,y,z,w) FVector4f. */
	GAUSSIANSIMVERSEEDITOR_API FVector4f PlayCanvasToUERotation(float W, float X, float Y, float Z);

	/** Standard 3DGS PLY coords (common training export) to UE. */
	GAUSSIANSIMVERSEEDITOR_API FVector3f PlyToUEPosition(const FVector3f& Position);

	/** Convert meter-based Gaussian scale to UE centimeters. */
	GAUSSIANSIMVERSEEDITOR_API FVector3f MetersToUEScale(const FVector3f& Scale);

	GAUSSIANSIMVERSEEDITOR_API FVector4f PlyToUERotation(float W, float X, float Y, float Z);

	GAUSSIANSIMVERSEEDITOR_API FVector4f SH0ToLinearColor(float Fdc0, float Fdc1, float Fdc2, float Opacity);

	GAUSSIANSIMVERSEEDITOR_API void ComputeBounds(const TArray<FGaussianSplatData>& Splats, FGaussianBounds& OutBounds);
}
