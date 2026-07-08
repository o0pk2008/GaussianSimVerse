// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Rendering/GaussianRenderResources.h"
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
	FGaussianFrameResources BuildFrameResources_RenderThread(const FSceneView& View) const;
	void RenderGaussiansForView(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		FRDGTextureRef SceneColorTexture,
		const FIntRect& SceneColorViewRect) const;

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
};
