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

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Streaming", meta = (DisplayPriority = 0))
	TObjectPtr<UGaussianStreamedSceneAsset> StreamedSceneAsset;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian", meta = (DisplayPriority = 1))
	bool bEnableRendering = true;

	// --- Proxy (under Gaussian, near the top) ---

	/** Optional proxy mesh for depth / collision / future DOF (dataset/actor-local space). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Proxy", meta = (DisplayPriority = 2))
	TObjectPtr<class UStaticMesh> ProxyMesh;

#if WITH_EDITOR
	/**
	 * Sample the streamed SOG dataset from disk, voxelize, and build a StaticMesh proxy
	 * (saved next to the streamed scene asset).
	 */
	UFUNCTION(CallInEditor, Category = "Gaussian|Proxy", meta = (DisplayName = "Generate Proxy Mesh", DisplayPriority = 3))
	void GenerateProxyMeshFromDataset();
#endif

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Proxy", meta = (ClampMin = "1.0", UIMin = "1.0", DisplayName = "Voxel Size (cm)", DisplayPriority = 4))
	float ProxyVoxelSizeCm = 2.0f;

	/** Ignore very faint splats. Keep low (~0.05) for denser silhouette matching the Gaussian. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Proxy", meta = (ClampMin = "0.0", ClampMax = "1.0", DisplayName = "Min Opacity", DisplayPriority = 5))
	float ProxyMinOpacity = 0.05f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Proxy", meta = (ClampMin = "1000", DisplayName = "Max Sample Points", DisplayPriority = 6))
	int32 ProxyMaxSamplePoints = 400000;

	/** Extra solid rings (0 = tight). Only raise if the surface has holes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Proxy", meta = (ClampMin = "0", ClampMax = "3", DisplayName = "Dilate Rings", DisplayPriority = 7))
	int32 ProxyDilateRings = 0;

	/** Peel outer solid rings (1 removes about one voxel of fringe fat). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Proxy", meta = (ClampMin = "0", ClampMax = "3", DisplayName = "Shrink Rings", DisplayPriority = 8))
	int32 ProxyShrinkRings = 0;

	/** Cells need this many sample hits to become solid (1 = densest silhouette). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Proxy", meta = (ClampMin = "1", ClampMax = "8", DisplayName = "Min Hits Per Voxel", DisplayPriority = 9))
	int32 ProxyMinHitsPerVoxel = 1;

	/**
	 * Draw proxy color in the main pass. Independent of collision / depth writes.
	 * Turn off to keep collision + depth without a visible mesh in Lit view.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Proxy", meta = (DisplayName = "Show Proxy Mesh", DisplayPriority = 10))
	bool bShowProxyMesh = false;

	/** Write Custom Depth even when Show is off (preferred for beauty-pass masks). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Proxy", meta = (DisplayName = "Write Custom Depth", DisplayPriority = 11))
	bool bProxyWriteCustomDepth = true;

	/**
	 * Write Scene Depth even when Show is off.
	 * Can look like a black mesh (SSAO/occlusion) over Gaussians — use for sensors / hard depth.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Proxy", meta = (DisplayName = "Write Scene Depth", DisplayPriority = 12))
	bool bProxyWriteSceneDepth = false;

	/** Runtime collision on the proxy. Independent of Show Proxy Mesh. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Proxy", meta = (DisplayName = "Enable Collision", DisplayPriority = 13))
	bool bProxyEnableCollision = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian|Proxy", meta = (DisplayName = "Mesh Local Offset", DisplayPriority = 14))
	FVector ProxyMeshLocalOffset = FVector::ZeroVector;

	UFUNCTION(BlueprintCallable, Category = "Gaussian|Proxy")
	void ApplyProxyMeshSettings();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Rendering", meta = (DisplayName = "SH Band", DisplayPriority = 20))
	EGaussianSHBand ShBandOverride = EGaussianSHBand::SH3;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Colors", meta = (ShowOnlyInnerProperties, DisplayPriority = 21))
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
	int32 StreamingMaxLoadedSplats = 2000000;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Streaming|CVar Overrides", meta = (DisplayName = "Max Loads Per Frame", ClampMin = "1", ClampMax = "16", UIMin = "1", UIMax = "16", EditCondition = "bApplyStreamingCVarOverrides"))
	int32 StreamingMaxLoadsPerFrame = 12;

	/**
	 * Soft budget for splats committed per update (smooths LOD-switch hitches).
	 * 0 = unlimited. Always commits at least one finished chunk.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Streaming|CVar Overrides", meta = (DisplayName = "Max Commit Splats Per Frame", ClampMin = "0", UIMin = "0", EditCondition = "bApplyStreamingCVarOverrides"))
	int32 StreamingMaxCommitSplatsPerFrame = 800000;

	/** Extra coarser LOD levels while the camera is moving (0 = off). Detail recovers after settle. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Streaming|CVar Overrides", meta = (DisplayName = "Motion LOD Bias", ClampMin = "0", ClampMax = "8", UIMin = "0", UIMax = "8", EditCondition = "bApplyStreamingCVarOverrides"))
	int32 StreamingMotionLodBias = 0;

	/**
	 * PlayCanvas-style underfill: max coarser LOD steps when optimal is not resident.
	 * 0 = always request optimal LOD; higher = show/load coarser first, then promote step-by-step.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Streaming|CVar Overrides", meta = (DisplayName = "LOD Underfill Limit", ClampMin = "0", ClampMax = "8", UIMin = "0", UIMax = "8", EditCondition = "bApplyStreamingCVarOverrides"))
	int32 StreamingLodUnderfillLimit = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Streaming|CVar Overrides", meta = (DisplayName = "Debug Draw Octree", EditCondition = "bApplyStreamingCVarOverrides"))
	bool bStreamingDebugDraw = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Streaming|CVar Overrides", meta = (DisplayName = "Debug Overlay", EditCondition = "bApplyStreamingCVarOverrides"))
	bool bStreamingDebugOverlay = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Streaming|Debug", meta = (DisplayName = "Debug Render", EditCondition = "bApplyStreamingCVarOverrides"))
	EGaussianStreamingDebugRenderMode DebugRenderMode = EGaussianStreamingDebugRenderMode::None;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian|Advanced", meta = (DisplayPriority = 90))
	TObjectPtr<UGaussianScene> GaussianScene;

	/** Hidden from category clutter — still visible once in the Components tree. */
	UPROPERTY()
	TObjectPtr<class USceneComponent> SceneRoot;

	UPROPERTY()
	TObjectPtr<class UBillboardComponent> EditorSprite;

	UPROPERTY()
	TObjectPtr<class UBoxComponent> BoundsVisual;

	UPROPERTY()
	TObjectPtr<class UStaticMeshComponent> ProxyMeshComponent;

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
	/** @param bForceRestart When false, keep resident chunks if already streaming the same asset. */
	void InitializeStreaming(bool bForceRestart = false);
	void ShutdownStreaming();
	bool IsStreamingInitialized() const;
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
	/** True after a successful InitializeStreaming for the current StreamedSceneAsset. */
	bool bStreamingInitialized = false;
};
