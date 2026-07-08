// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GaussianTypes.h"
#include "Streaming/GaussianStreamingManager.h"

#if WITH_EDITOR
#include "EditorViewportClient.h"
#include "Containers/Ticker.h"
#endif

#include "GaussianStreamedSceneActor.generated.h"

class UGaussianStreamedSceneAsset;
class UGaussianScene;

UENUM(BlueprintType)
enum class EGaussianStreamingDebugRenderMode : uint8
{
	None UMETA(DisplayName = "None"),
	LOD UMETA(DisplayName = "LOD"),
};

/**
 * Places a streamed Gaussian LOD dataset in the level and loads chunks at runtime.
 */
UCLASS(Blueprintable, meta = (DisplayName = "Gaussian Streamed Scene"))
class GAUSSIANSIMVERSE_API AGaussianStreamedSceneActor : public AActor
{
	GENERATED_BODY()

public:
	AGaussianStreamedSceneActor();

	UFUNCTION(BlueprintCallable, Category = "Gaussian|Streaming")
	void SetStreamedSceneAsset(UGaussianStreamedSceneAsset* InAsset);

	void NotifyStreamingChunkLoaded();
	void DrawStreamingDebugOverlay(const FGaussianStreamingManager& Manager) const;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Streaming")
	TObjectPtr<UGaussianStreamedSceneAsset> StreamedSceneAsset;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian")
	bool bEnableRendering = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Rendering", meta = (DisplayName = "SH Band"))
	EGaussianSHBand ShBandOverride = EGaussianSHBand::SH3;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Colors", meta = (ShowOnlyInnerProperties))
	FGaussianColorAdjustment Colors;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Streaming|CVar Overrides", meta = (DisplayName = "Apply Streaming CVar Overrides"))
	bool bApplyStreamingCVarOverrides = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Streaming|CVar Overrides", meta = (DisplayName = "Streaming Enable", EditCondition = "bApplyStreamingCVarOverrides"))
	bool bStreamingEnable = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Streaming|CVar Overrides", meta = (DisplayName = "Load Radius", ClampMin = "100.0", UIMin = "100.0", EditCondition = "bApplyStreamingCVarOverrides"))
	float StreamingLoadRadius = 50000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Streaming|CVar Overrides", meta = (DisplayName = "LOD Base Distance", ClampMin = "0.01", UIMin = "0.01", EditCondition = "bApplyStreamingCVarOverrides"))
	float StreamingLodBaseDistance = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Streaming|CVar Overrides", meta = (DisplayName = "Max Loaded Splats", ClampMin = "0", UIMin = "0", EditCondition = "bApplyStreamingCVarOverrides"))
	int32 StreamingMaxLoadedSplats = 4000000;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Streaming|CVar Overrides", meta = (DisplayName = "Max Loads Per Frame", ClampMin = "1", ClampMax = "16", UIMin = "1", UIMax = "16", EditCondition = "bApplyStreamingCVarOverrides"))
	int32 StreamingMaxLoadsPerFrame = 8;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Streaming|CVar Overrides", meta = (DisplayName = "Debug Draw Octree", EditCondition = "bApplyStreamingCVarOverrides"))
	bool bStreamingDebugDraw = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Streaming|CVar Overrides", meta = (DisplayName = "Debug Overlay", EditCondition = "bApplyStreamingCVarOverrides"))
	bool bStreamingDebugOverlay = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Streaming|Debug", meta = (DisplayName = "Debug Render", EditCondition = "bApplyStreamingCVarOverrides"))
	EGaussianStreamingDebugRenderMode DebugRenderMode = EGaussianStreamingDebugRenderMode::None;

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
	virtual void Tick(float DeltaSeconds) override;
	virtual void BeginDestroy() override;
	virtual void SetActorHiddenInGame(bool bNewHidden) override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void PostEditMove(bool bFinished) override;
	virtual void Destroyed() override;
	virtual void SetIsTemporarilyHiddenInEditor(bool bIsHidden) override;
#endif

private:
	bool ShouldRenderGaussian() const;
	void RefreshRenderRegistration();
	void InitializeStreaming();
	void ShutdownStreaming();
	void SyncSceneSettings();
	void TryRegisterScene();
	void RegisterScene();
	void UnregisterScene();
	void SnapActorToSceneOrigin();
	void UpdateBoundsVisual();
	FVector GetStreamingViewOrigin() const;
	FVector GetStreamingViewDirection() const;
	void ApplyStreamingCVarOverrides() const;
	void UpdateStreamingFromView();
#if WITH_EDITOR
	void HandleEditorCameraMoved(const FVector& Location, const FRotator& Rotation, ELevelViewportType ViewportType, int32 ViewIndex);
	bool HandleEditorStreamingTick(float DeltaTime);
	void EnsureEditorStreamingTickInterval();
	FDelegateHandle EditorCameraMovedHandle;
	FTSTicker::FDelegateHandle EditorStreamingTickHandle;
	double LastEditorStreamingUpdateSeconds = 0.0;
	float EditorStreamingTickInterval = 0.0f;
#endif

	FGaussianStreamingManager StreamingManager;
};
