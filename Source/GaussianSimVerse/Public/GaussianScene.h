// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GaussianChunk.h"
#include "GaussianScene.generated.h"

class UGaussianAsset;

/**
 * Runtime container for one or more Gaussian chunks in a world.
 * Registered with FGaussianRenderer for RDG submission.
 */
UCLASS(BlueprintType)
class GAUSSIANSIMVERSE_API UGaussianScene : public UObject
{
	GENERATED_BODY()

public:
	UGaussianScene();

	void RegisterWithRenderer();
	void UnregisterFromRenderer();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian")
	FTransform WorldTransform = FTransform::Identity;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian")
	TArray<TObjectPtr<UGaussianChunk>> Chunks;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian")
	bool bEnableRendering = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian|Rendering")
	float SplatScale = 1.5f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian|Rendering")
	float AlphaCullThreshold = 0.007843137f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian|Rendering")
	float CutoffK = 7.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian|Rendering")
	float CovarianceDilation = 0.3f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Rendering", meta = (DisplayName = "SH Band"))
	EGaussianSHBand ShBand = EGaussianSHBand::SH3;

	uint32 GetTotalGaussianCount() const;
	FGaussianBounds GetCombinedBounds() const;
	bool IsRegisteredWithRenderer() const { return bRegisteredWithRenderer; }

private:
	bool bRegisteredWithRenderer = false;
};
