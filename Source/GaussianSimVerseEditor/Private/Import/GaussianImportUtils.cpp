// Copyright Epic Games, Inc. All Rights Reserved.

#include "Import/GaussianImportUtils.h"

namespace GaussianImport
{
	float Sigmoid(float X)
	{
		return 1.0f / (1.0f + FMath::Exp(-X));
	}

	float UnlogPosition(float N)
	{
		return FMath::Sign(N) * (FMath::Exp(FMath::Abs(N)) - 1.0f);
	}

	FVector3f LerpVec3(const FVector3f& Min, const FVector3f& Max, const FVector3f& T)
	{
		return FVector3f(
			FMath::Lerp(Min.X, Max.X, T.X),
			FMath::Lerp(Min.Y, Max.Y, T.Y),
			FMath::Lerp(Min.Z, Max.Z, T.Z));
	}

	FVector3f PlayCanvasToUEPosition(const FVector3f& Position)
	{
		// PlayCanvas (X right, Y up, Z backward) -> UE (X forward, Y right, Z up).
		// Negate PlayCanvas Y so the splat cloud is not vertically flipped in UE.
		return FVector3f(Position.X, -Position.Z, -Position.Y) * MetersToCentimeters;
	}

	FVector3f MetersToUEScale(const FVector3f& Scale)
	{
		// PlayCanvas / SOG: rotation is converted via PlayCanvasToUERotation; only convert units.
		return Scale * MetersToCentimeters;
	}

	FVector4f PlayCanvasToUERotation(float W, float X, float Y, float Z)
	{
		const FVector4f PlayCanvasQuat(X, Y, Z, W);
		const FQuat4f QPlayCanvas(PlayCanvasQuat.X, PlayCanvasQuat.Y, PlayCanvasQuat.Z, PlayCanvasQuat.W);

		const FQuat4f YUpToZUp = FQuat4f(FVector3f(1.0f, 0.0f, 0.0f), -PI * 0.5f);
		const FQuat4f FlipVertical = FQuat4f(FVector3f(0.0f, 1.0f, 0.0f), PI);
		const FQuat4f QUnreal = FlipVertical * YUpToZUp * QPlayCanvas;
		return FVector4f(QUnreal.X, QUnreal.Y, QUnreal.Z, QUnreal.W);
	}

	FVector3f PlyToUEPosition(const FVector3f& Position)
	{
		// Standard 3DGS PLY exports typically follow a COLMAP-like basis.
		// Map source (x, y, z) -> UE (forward, right, up) as (z, x, -y).
		return FVector3f(Position.Z, Position.X, -Position.Y) * MetersToCentimeters;
	}

	FVector3f PlyToUEDirection(const FVector3f& Direction)
	{
		return FVector3f(Direction.Z, Direction.X, -Direction.Y);
	}

	namespace GaussianImportPrivate
	{
		// Right-handed basis for PLY RDF -> UE (X forward, Y right, Z up).
		// Maps COLMAP +X -> UE +Y, +Y -> UE -Z, +Z -> UE +X.
		const FQuat PlyBasisToUeQuat = []() -> FQuat
		{
			FVector XAxis(0.0, 1.0, 0.0);
			FVector YAxis(0.0, 0.0, -1.0);
			FVector ZAxis(-1.0, 0.0, 0.0);
			FMatrix Basis = FMatrix::Identity;
			Basis.SetAxes(&XAxis, &YAxis, &ZAxis, nullptr);
			return FQuat(Basis);
		}();
	}

	FVector4f PlyToUERotation(float W, float X, float Y, float Z)
	{
		const FQuat PlyQuat(X, Y, Z, W);
		const FQuat UeQuat = (GaussianImportPrivate::PlyBasisToUeQuat * PlyQuat).GetNormalized();
		return FVector4f(UeQuat.X, UeQuat.Y, UeQuat.Z, UeQuat.W);
	}

	FVector3f PlyMetersToUEScale(const FVector3f& Scale)
	{
		// Rotation already maps PLY principal axes into UE space; only convert units.
		return Scale * MetersToCentimeters;
	}

	FVector4f SH0ToLinearColor(float Fdc0, float Fdc1, float Fdc2, float Opacity)
	{
		const float R = FMath::Clamp(0.5f + SH_C0 * Fdc0, 0.0f, 1.0f);
		const float G = FMath::Clamp(0.5f + SH_C0 * Fdc1, 0.0f, 1.0f);
		const float B = FMath::Clamp(0.5f + SH_C0 * Fdc2, 0.0f, 1.0f);
		return FVector4f(R, G, B, Opacity);
	}

	void ComputeBounds(const TArray<FGaussianSplatData>& Splats, FGaussianBounds& OutBounds)
	{
		if (Splats.Num() == 0)
		{
			return;
		}

		FBox Box(ForceInit);
		for (const FGaussianSplatData& Splat : Splats)
		{
			Box += FVector(Splat.Position);
		}

		if (Box.IsValid)
		{
			OutBounds.Origin = FVector3f(Box.GetCenter());
			OutBounds.Extent = FVector3f(Box.GetExtent());
		}
	}
}
