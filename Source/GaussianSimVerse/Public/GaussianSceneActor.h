// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GaussianTypes.h"
#include "GaussianSceneActor.generated.h"

class UGaussianAsset;
class UGaussianScene;
class UGaussianChunk;

/**
 * Places a Gaussian splat asset in the level and registers it with the GPU renderer.
 * Drag the data asset into the level is NOT supported — use right-click "Place Gaussian Scene in Level"
 * or add this actor from the Place Actors panel (Gaussian SimVerse category).
 */
UCLASS(Blueprintable, meta = (DisplayName = "Gaussian Scene"))
class GAUSSIANSIMVERSE_API AGaussianSceneActor : public AActor
{
	GENERATED_BODY()

public:
	AGaussianSceneActor();

	UFUNCTION(BlueprintCallable, Category = "Gaussian")
	void SetGaussianAsset(UGaussianAsset* InAsset);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian")
	TObjectPtr<UGaussianAsset> GaussianAsset;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian")
	bool bEnableRendering = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Rendering", meta = (DisplayName = "SH Band"))
	EGaussianSHBand ShBandOverride = EGaussianSHBand::SH3;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Colors", meta = (ShowOnlyInnerProperties))
	FGaussianColorAdjustment Colors;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian")
	TObjectPtr<UGaussianScene> GaussianScene;

	UPROPERTY(VisibleAnywhere, Category = "Gaussian")
	TObjectPtr<class USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, Category = "Gaussian")
	TObjectPtr<class UBillboardComponent> EditorSprite;

	UPROPERTY(VisibleAnywhere, Category = "Gaussian")
	TObjectPtr<class UBoxComponent> BoundsVisual;

protected:
	virtual void PostRegisterAllComponents() override;
	virtual void PostUnregisterAllComponents() override;
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void BeginDestroy() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void PostEditMove(bool bFinished) override;
	virtual void Destroyed() override;
#endif

private:
	void RebuildGaussianScene();
	void SyncSceneSettings();
	void TryRegisterScene();
	void RegisterScene();
	void UnregisterScene();
	void SnapActorToAssetOrigin();
	void UpdateBoundsVisual();

	UPROPERTY()
	TObjectPtr<UGaussianChunk> GaussianChunk;
};
