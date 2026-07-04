// Copyright Epic Games, Inc. All Rights Reserved.

#include "Import/GaussianImportUtils.h"

namespace GaussianImport
{
	namespace Private
	{
		/**
		 * Authoring (Transform.PLY / SuperSplat) -> UE linear map:
		 *   (x, y, z) -> (-x, -z, -y)
		 * UE.Z = -Y maps authoring -Y-up to UE +Z-up.
		 * Position and rotation MUST use this same basis or ellipsoids shear wrong.
		 */
		FVector3f AuthoringToUeVector(const FVector3f& V)
		{
			return FVector3f(-V.X, -V.Z, -V.Y);
		}

		const FQuat& GetAuthoringToUeQuat()
		{
			// Images of authoring basis axes under AuthoringToUeVector.
			static const FQuat Quat = []()
			{
				const FVector XAxis(-1.0, 0.0, 0.0); // +X -> -X
				const FVector YAxis(0.0, 0.0, -1.0); // +Y -> -Z
				const FVector ZAxis(0.0, -1.0, 0.0); // +Z -> -Y
				FMatrix Basis = FMatrix::Identity;
				Basis.SetAxes(&XAxis, &YAxis, &ZAxis);
				return FQuat(Basis).GetNormalized();
			}();
			return Quat;
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
		// Local scale axes are carried by the converted quaternion; only convert units.
		return Scale * MetersToCentimeters;
	}

	FVector4f PlayCanvasToUERotation(float W, float X, float Y, float Z)
	{
		// Same left-multiply as splat-transform: Q_ue = Q_basis * Q_src.
		const FQuat QSrc(X, Y, Z, W);
		const FQuat QUe = (Private::GetAuthoringToUeQuat() * QSrc).GetNormalized();
		return FVector4f(QUe.X, QUe.Y, QUe.Z, QUe.W);
	}

	FVector3f PlyToUEPosition(const FVector3f& Position)
	{
		// PLY and SOG share Transform.PLY in SuperSplat. The editor's entity Z-180 is
		// only for PlayCanvas +Y-up display — do not bake it into vertex data.
		// Use the same PlayCanvas->UE map as SOG.
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
		// Local scale axes are carried by the converted quaternion; only convert units.
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
}
