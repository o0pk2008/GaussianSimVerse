// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Rendering/GaussianRenderResources.h"
#include "Rendering/GaussianRelighting.h"
#include "RendererInterface.h"

class UGaussianScene;
class FGaussianViewExtension;
class FSceneView;
struct FSceneTextures;

/**
 * Central render coordinator for GaussianSimVerse.
 * Owns scene registration (game thread) and proxy sync (render thread).
 */
class GAUSSIANSIMVERSE_API FGaussianRenderer
{
public:
	static FGaussianRenderer& Get();

	/** FViewInfo::ViewRect when available; falls back to UnscaledViewRect on FSceneView. */
	static FIntRect GetEffectiveViewRect(const FSceneView& View);

	void Initialize();
	void Shutdown();

	// Game thread
	void RegisterScene(UGaussianScene* Scene);
	void UnregisterScene(UGaussianScene* Scene);
	/** Mark a scene dirty. By default rebuilds proxies immediately; pass false to coalesce then FlushDirtySceneProxies. */
	void MarkSceneDirty(UGaussianScene* Scene, bool bFlushImmediately = true);
	/** Rebuild proxies once if any deferred MarkSceneDirty calls are pending. */
	void FlushDirtySceneProxies();

	// Render thread
	void SyncSceneProxies_RenderThread();
	void CacheView_RenderThread(const FSceneView& View, const FIntRect& ViewportRect);
	bool HasActiveScenes() const;

	/** Any proxy DOF mode active (CineCamera or Plugin). */
	bool WantsEngineDepthOfField() const;
	/** CineCamera / Diaphragm DOF path (BeforeDOF + late CustomDepth merge). */
	bool WantsCineCameraDepthOfField() const;
	/** Plugin compute CoC blur path. */
	bool WantsPluginDepthOfField() const;

	FGaussianFrameResources BuildFrameResources_RenderThread(const FSceneView& View) const;

	/**
	 * Inject mode for split CineCamera DOF + exposure-safe color composite.
	 * SoftDepthAndOverlay runs at BeforeDOF (no SceneColor write).
	 * CompositePending runs at AfterTonemap using the stashed overlay.
	 */
	enum class EInjectMode : uint8
	{
		Full = 0,
		SoftDepthAndOverlay,
		CompositePending,
	};

	/**
	 * Raster + composite gaussians for a post-process inject.
	 * @param PreferredOutput If valid, composite into this RT (e.g. OverrideOutput after tonemap).
	 * @param bAfterTonemap When true, splat colors stay display-referred (no sRGB→linear).
	 * @return Scene color texture for the rest of the PP chain.
	 */
	FRDGTextureRef RenderGaussiansForView(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		FRDGTextureRef SceneColorTexture,
		const FIntRect& SceneColorViewRect,
		FRDGTextureRef PreferredOutput = nullptr,
		bool bAfterTonemap = false,
		EInjectMode InjectMode = EInjectMode::Full) const;

	/** Clear RDG-only deferred overlays (call once per frame before PP). */
	void ClearDeferredOverlays_RenderThread() const;

	/**
	 * Plugin CoC DOF after gaussians are composited. Uses proxy CustomDepth (not early SceneDepth).
	 * Optional; CineCamera users should prefer BeforeDOF inject + late SceneDepth merge instead.
	 */
	void ApplyProxyDepthOfField_RenderThread(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		FRDGTextureRef SceneColorTexture,
		const FIntRect& SceneColorViewRect) const;

	/**
	 * Before engine Diaphragm/Cinematic DOF: write proxy CustomDepth (stencil-tagged) into the
	 * SceneDepth texture that DOF samples — without early BasePass SceneDepth (sky stays clean).
	 */
	void MergeProxyCustomDepthIntoSceneDepth_RenderThread(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		uint32 ProxyStencilValue = 1) const;

	/** After gaussian raster: write soft DeviceZ into SceneDepth for near/far CoC falloff. */
	void MergeGaussianSoftDepthIntoSceneDepth_RenderThread(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		FRDGTextureRef SoftDepthBitsTexture,
		const FIntRect& SoftDepthViewRect) const;

	/** Aggregate DOF params from active scenes (first enabled wins / max blur). */
	bool GetActiveDofSettings(
		float& OutFocalDistanceCm,
		float& OutCocScale,
		float& OutMaxBlurRadiusPx,
		uint32& OutProxyStencil) const;

	/**
	 * Game-thread: push PlayCanvas-style relight RT + params for the next composite.
	 * Call each tick from the actor that owns the SceneCapture (last writer wins).
	 */
	void SetRelightFrameState_GameThread(const FGaussianRelightFrameState& State);

	/** Copy of last game-thread relight state (render thread reads under SceneLock). */
	FGaussianRelightFrameState GetRelightFrameState() const;

	bool IsInitialized() const { return bInitialized; }

private:
	FGaussianRenderer() = default;

	void OnPostOpaqueRender(FPostOpaqueRenderParameters& Parameters);
	void OnResolvedSceneColor(FRDGBuilder& GraphBuilder, const FSceneTextures& SceneTextures);
	static FGaussianViewData BuildViewData(const FSceneView& InView);

	struct FGaussianCachedViewState
	{
		const FSceneView* View = nullptr;
		FIntRect ViewportRect;
		uint32 ViewKey = 0;
	};

	FGaussianSceneProxy BuildSceneProxy(const UGaussianScene* Scene, uint32 SceneId) const;
	void RebuildSceneProxies_GameThread();
	void PruneInvalidRegisteredScenes();
	void EnsureViewExtensionRegistered();

	struct FRegisteredScene
	{
		TWeakObjectPtr<UGaussianScene> Scene;
		uint32 SceneId = INDEX_NONE;
		bool bDirty = true;
	};

	mutable FCriticalSection SceneLock;
	TArray<FRegisteredScene> RegisteredScenes;
	TArray<FGaussianSceneProxy> GameThreadSceneProxies;
	TArray<FGaussianSceneProxy> RenderThreadSceneProxies;
	TArray<FGaussianSceneProxy> PendingRenderThreadProxies;
	uint32 NextSceneId = 1;
	bool bProxiesDirty = true;
	bool bHasPendingRenderThreadProxies = false;
	bool bInitialized = false;

	FDelegateHandle PostOpaqueDelegateHandle;
	FDelegateHandle ResolvedSceneColorDelegateHandle;
	mutable TArray<FGaussianCachedViewState> CachedViewsThisFrame;
	mutable uint32 CachedViewsFrameNumber = 0;
	TSharedPtr<FGaussianViewExtension, ESPMode::ThreadSafe> ViewExtension;

	FGaussianRelightFrameState RelightFrameState;

	/** Per-view overlay stashed at BeforeDOF for AfterTonemap composite (same GraphBuilder frame). */
	struct FDeferredOverlay
	{
		uint32 ViewKey = 0;
		FRDGTextureRef Overlay = nullptr;
		FIntRect SceneColorViewRect;
		FIntPoint SceneColorOffset = FIntPoint::ZeroValue;
	};
	mutable TArray<FDeferredOverlay> DeferredOverlays;
};
