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
 * Supports direct drag-drop of GaussianAsset into the level viewport.
 */
UCLASS(Blueprintable, meta = (DisplayName = "Gaussian Scene"))
class GAUSSIANSIMVERSE_API AGaussianSceneActor : public AActor
{
	GENERATED_BODY()

public:
	AGaussianSceneActor();

	UFUNCTION(BlueprintCallable, Category = "Gaussian")
	void SetGaussianAsset(UGaussianAsset* InAsset);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian", meta = (DisplayPriority = 0))
	TObjectPtr<UGaussianAsset> GaussianAsset;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian", meta = (DisplayPriority = 1))
	bool bEnableRendering = true;

	// --- Proxy (under Gaussian, high in the panel) ---

	/** Optional proxy mesh for depth / collision / future DOF (actor-local space). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Proxy", meta = (DisplayPriority = 2))
	TObjectPtr<class UStaticMesh> ProxyMesh;

#if WITH_EDITOR
	/** Generate a voxel surface mesh from the Gaussian asset (saved next to the asset). */
	UFUNCTION(CallInEditor, Category = "Gaussian|Proxy", meta = (DisplayName = "Generate Proxy Mesh", DisplayPriority = 3))
	void GenerateProxyMeshFromAsset();
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
	 * Draw proxy color in the main pass (what you see in Lit view).
	 * Independent of collision / Custom Depth / Scene Depth — turn off to keep those without a visible mesh.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Proxy", meta = (DisplayName = "Show Proxy Mesh", DisplayPriority = 10))
	bool bShowProxyMesh = false;

	/**
	 * Write Custom Depth (Buffer Visualization → Custom Depth). Works with Show off.
	 * Preferred for beauty-pass masks / many post effects (does not darken Lit like Scene Depth).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Proxy", meta = (DisplayName = "Write Custom Depth", DisplayPriority = 11))
	bool bProxyWriteCustomDepth = true;

	/**
	 * Write Scene Depth (true scene depth buffer). Works with Show off.
	 * WARNING: solid proxy depth is used by SSAO/contact shadows and can look like a black mesh
	 * over the Gaussian even when Show is off. Use for depth sensors / hard occlusion; prefer
	 * Custom Depth for non-destructive beauty-pass DOF masks.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Proxy", meta = (DisplayName = "Write Scene Depth", DisplayPriority = 12))
	bool bProxyWriteSceneDepth = false;

	/** Runtime collision on the proxy mesh. Independent of Show Proxy Mesh. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Proxy", meta = (DisplayName = "Enable Collision", DisplayPriority = 13))
	bool bProxyEnableCollision = false;

	/**
	 * Mesh asset is built centered at origin for clean Static Mesh previews.
	 * This offset restores actor-local alignment with the Gaussian in the scene.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian|Proxy", meta = (DisplayName = "Mesh Local Offset", DisplayPriority = 14))
	FVector ProxyMeshLocalOffset = FVector::ZeroVector;

	/** Apply current ProxyMesh + flags to the component. */
	UFUNCTION(BlueprintCallable, Category = "Gaussian|Proxy")
	void ApplyProxyMeshSettings();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Rendering", meta = (DisplayName = "SH Band", DisplayPriority = 20))
	EGaussianSHBand ShBandOverride = EGaussianSHBand::SH3;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian|Colors", meta = (ShowOnlyInnerProperties, DisplayPriority = 21))
	FGaussianColorAdjustment Colors;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian|Advanced", meta = (DisplayPriority = 90))
	TObjectPtr<UGaussianScene> GaussianScene;

	/** Hidden from the main category list — still appears once in the Components tree. */
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
