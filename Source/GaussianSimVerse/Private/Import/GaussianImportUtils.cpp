// Copyright Epic Games, Inc. All Rights Reserved.

#include "Import/GaussianImportUtils.h"

namespace GaussianImport
{
	namespace Private
	{
		/**
		 * SuperSplat / 3DGS authoring → UE linear map (meters, no unit scale here):
		 *   (x, y, z) → (x, -z, -y)
		 *
		 * Matches SuperSplat viewer (equivalent to upright map (x,z,-y) then Scale.Y = -1).
		 * det(M) = -1 (reflection). Do NOT build FQuat directly from M — rebuild rotation
		 * from transformed principal axes instead.
		 */
		FVector3f AuthoringToUeVector(const FVector3f& V)
		{
			return FVector3f(V.X, -V.Z, -V.Y);
		}

		FVector AuthoringToUeVector(const FVector& V)
		{
			return FVector(V.X, -V.Z, -V.Y);
		}

		/**
		 * Map authoring rotation into UE under AuthoringToUeVector.
		 * Axes a_i' = M * R * e_i; force a right-handed orthonormal frame for a valid quat.
		 * (Gaussian ellipsoids are axis-flip symmetric, so fixing handedness is safe.)
		 */
		FQuat AuthoringRotationToUe(const FQuat& QSrc)
		{
			FVector T0 = AuthoringToUeVector(QSrc.RotateVector(FVector(1.0, 0.0, 0.0))).GetSafeNormal();
			FVector T1 = AuthoringToUeVector(QSrc.RotateVector(FVector(0.0, 1.0, 0.0))).GetSafeNormal();
			FVector T2 = AuthoringToUeVector(QSrc.RotateVector(FVector(0.0, 0.0, 1.0))).GetSafeNormal();

			// Re-orthogonalize (numerical safety).
			T1 = (T1 - T0 * FVector::DotProduct(T1, T0)).GetSafeNormal();
			FVector T2Rh = FVector::CrossProduct(T0, T1).GetSafeNormal();
			if (FVector::DotProduct(T2Rh, T2) < 0.0)
			{
				// M was improper; cross product already chose RH — keep T2Rh.
			}
			T2 = T2Rh;
			T1 = FVector::CrossProduct(T2, T0).GetSafeNormal();

			FMatrix Basis = FMatrix::Identity;
			Basis.SetAxes(&T0, &T1, &T2);
			return FQuat(Basis).GetNormalized();
		}
	}

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
		return Private::AuthoringToUeVector(Position) * MetersToCentimeters;
	}

	FVector3f MetersToUEScale(const FVector3f& Scale)
	{
		// Principal-axis lengths are invariant under orthogonal M (incl. reflections).
		return Scale.GetAbs() * MetersToCentimeters;
	}

	FVector4f PlayCanvasToUERotation(float W, float X, float Y, float Z)
	{
		const FQuat QSrc(X, Y, Z, W);
		const FQuat QUe = Private::AuthoringRotationToUe(QSrc.GetNormalized());
		return FVector4f(QUe.X, QUe.Y, QUe.Z, QUe.W);
	}

	FVector3f PlyToUEPosition(const FVector3f& Position)
	{
		return PlayCanvasToUEPosition(Position);
	}

	FVector3f PlyToUEDirection(const FVector3f& Direction)
	{
		return Private::AuthoringToUeVector(Direction);
	}

	FVector4f PlyToUERotation(float W, float X, float Y, float Z)
	{
		return PlayCanvasToUERotation(W, X, Y, Z);
	}

	FVector3f PlyMetersToUEScale(const FVector3f& Scale)
	{
		return MetersToUEScale(Scale);
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

	FGaussianBounds PlayCanvasBoundsToUE(const FVector3f& Min, const FVector3f& Max)
	{
		const FVector3f UeMin = PlayCanvasToUEPosition(Min);
		const FVector3f UeMax = PlayCanvasToUEPosition(Max);
		const FVector3f Center = (UeMin + UeMax) * 0.5f;
		const FVector3f Extent = (UeMax - UeMin).GetAbs() * 0.5f;

		FGaussianBounds Bounds;
		Bounds.Origin = Center;
		Bounds.Extent = Extent;
		return Bounds;
	}
}
