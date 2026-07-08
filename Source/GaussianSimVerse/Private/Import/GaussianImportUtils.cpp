// Copyright Epic Games, Inc. All Rights Reserved.

#include "Import/GaussianImportUtils.h"

namespace GaussianImport
{
	namespace Private
	{
		FVector3f AuthoringToUeVector(const FVector3f& V)
		{
			return FVector3f(-V.X, -V.Z, -V.Y);
		}

		const FQuat& GetAuthoringToUeQuat()
		{
			static const FQuat Quat = []()
			{
				const FVector XAxis(-1.0, 0.0, 0.0);
				const FVector YAxis(0.0, 0.0, -1.0);
				const FVector ZAxis(0.0, -1.0, 0.0);
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
		return Scale * MetersToCentimeters;
	}

	FVector4f PlayCanvasToUERotation(float W, float X, float Y, float Z)
	{
		const FQuat QSrc(X, Y, Z, W);
		const FQuat QUe = (Private::GetAuthoringToUeQuat() * QSrc).GetNormalized();
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
