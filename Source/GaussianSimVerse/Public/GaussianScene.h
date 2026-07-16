// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GaussianChunk.h"
#include "GaussianTypes.h"
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

	/**
	 * Streamed datasets: pivot in dataset space (lod-meta scene origin).
	 * Used to convert absolute chunk centers into actor-local offsets so moving the actor works.
	 */
	FVector DatasetPivot = FVector::ZeroVector;
	bool bHasDatasetPivot = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian")
	TArray<TObjectPtr<UGaussianChunk>> Chunks;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian")
	bool bEnableRendering = true;

	/** Driven by AGaussianSceneActor; not exposed in the actor details panel. */
	float SplatScale = 1.0f;
	float AlphaCullThreshold = 0.007843137f;
	float CutoffK = 7.0f;
	float CovarianceDilation = 0.3f;

	EGaussianSHBand ShBand = EGaussianSHBand::SH3;

	FGaussianColorAdjustment Colors;

	uint32 GetTotalGaussianCount() const;
	FGaussianBounds GetCombinedBounds() const;
	bool IsRegisteredWithRenderer() const { return bRegisteredWithRenderer; }

private:
	bool bRegisteredWithRenderer = false;
};
