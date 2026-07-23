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

	GAUSSIANSIMVERSE_API float Sigmoid(float X);
	GAUSSIANSIMVERSE_API float UnlogPosition(float N);
	GAUSSIANSIMVERSE_API FVector3f LerpVec3(const FVector3f& Min, const FVector3f& Max, const FVector3f& T);

	/** SuperSplat / 3DGS authoring → UE cm: (x,y,z)→(x,-z,-y)*100 (matches SuperSplat; Scale.Y=-1 baked in). */
	GAUSSIANSIMVERSE_API FVector3f PlayCanvasToUEPosition(const FVector3f& Position);

	/** Quaternion (w,x,y,z) in authoring frame to UE (x,y,z,w). */
	GAUSSIANSIMVERSE_API FVector4f PlayCanvasToUERotation(float W, float X, float Y, float Z);

	GAUSSIANSIMVERSE_API FVector3f PlyToUEPosition(const FVector3f& Position);
	GAUSSIANSIMVERSE_API FVector3f PlyToUEDirection(const FVector3f& Direction);
	GAUSSIANSIMVERSE_API FVector3f MetersToUEScale(const FVector3f& Scale);
	GAUSSIANSIMVERSE_API FVector3f PlyMetersToUEScale(const FVector3f& Scale);
	GAUSSIANSIMVERSE_API FVector4f PlyToUERotation(float W, float X, float Y, float Z);

	GAUSSIANSIMVERSE_API FVector4f SH0ToLinearColor(float Fdc0, float Fdc1, float Fdc2, float Opacity);

	GAUSSIANSIMVERSE_API void ComputeBounds(const TArray<FGaussianSplatData>& Splats, FGaussianBounds& OutBounds);

	GAUSSIANSIMVERSE_API FGaussianBounds PlayCanvasBoundsToUE(const FVector3f& Min, const FVector3f& Max);
}
