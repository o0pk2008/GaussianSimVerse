// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class AActor;
class UStaticMeshComponent;
class USceneCaptureComponent2D;
class UTextureRenderTarget2D;
class USkyLightComponent;
class UTextureCube;
class UMaterialInterface;
class UMaterialInstanceDynamic;

/** PlayCanvas-style proxy-mesh relighting settings (Global + Environment). */
struct GAUSSIANSIMVERSE_API FGaussianRelightSettings
{
	bool bEnabled = false;

	// --- Global (PlayCanvas GsplatRelighting) ---
	/** 0 = original baked splats, 1 = fully modulated by lit proxy. */
	float Blend = 1.0f;
	/** Extra exposure applied to the lighting factor (after brightness). */
	float Exposure = 1.0f;
	/** Compensates proxy gray albedo (~2 for 0.5 albedo). */
	float Brightness = 2.0f;
	/** Multiplier for splat pixels not covered by the proxy (e.g. sky holes). */
	float Background = 1.0f;
	/** Relight capture resolution scale vs view (0.25–1). */
	float TextureScale = 0.5f;
	/** Draw lit-proxy RT as a corner inset on SceneColor. */
	bool bDebug = false;

	// --- Environment ---
	float EnvExposure = 1.0f;
	/** Yaw degrees applied to the skydome / sky light. */
	float EnvRotationDegrees = 0.0f;
	TObjectPtr<UTextureCube> Skydome = nullptr;
};

/** Per-frame state pushed game-thread → render-thread for composite modulation. */
struct GAUSSIANSIMVERSE_API FGaussianRelightFrameState
{
	bool bEnabled = false;
	float Blend = 1.0f;
	float Exposure = 1.0f;
	float Brightness = 2.0f;
	float Background = 1.0f;
	bool bDebug = false;
	/** Game-thread RT; resolved to RHI on the render thread each frame. */
	TWeakObjectPtr<UTextureRenderTarget2D> RenderTarget;
};

/**
 * Helpers shared by GaussianSceneActor / StreamedSceneActor for PlayCanvas-style relighting:
 * lit proxy → offscreen RT → modulate gaussians in CompositeCS.
 */
namespace GaussianRelighting
{
	/** Neutral gray DefaultLit-like material for the capture (brightness≈2 compensates 0.5 albedo). */
	GAUSSIANSIMVERSE_API UMaterialInterface* GetOrCreateProxyLitMaterial();

	GAUSSIANSIMVERSE_API UMaterialInstanceDynamic* CreateProxyLitMID(UObject* Outer);

	/** Create capture + RT + optional sky light on first enable. */
	GAUSSIANSIMVERSE_API void EnsureComponents(
		AActor* Owner,
		USceneCaptureComponent2D*& InOutCapture,
		UTextureRenderTarget2D*& InOutRT,
		USkyLightComponent*& InOutSkyLight);

	/** Resize RT when view size or TextureScale changes. Returns the RT. */
	GAUSSIANSIMVERSE_API UTextureRenderTarget2D* EnsureRenderTarget(
		UTextureRenderTarget2D*& InOutRT,
		UObject* Outer,
		int32 Width,
		int32 Height);

	/**
	 * Lighting capture: full scene primitives so boxes/walls cast real shadows onto the proxy.
	 * Proxy must use the gray lit material; coverage uses RT alpha + optional custom stencil.
	 */
	GAUSSIANSIMVERSE_API void ConfigureCapture(
		USceneCaptureComponent2D* Capture,
		UStaticMeshComponent* ProxyMeshComponent,
		UTextureRenderTarget2D* RT);

	/**
	 * Invisible main-view mesh that only casts shadows onto the ground / other actors
	 * (gaussians themselves never cast engine shadows).
	 */
	GAUSSIANSIMVERSE_API void ConfigureMainViewShadowCaster(
		UStaticMeshComponent* ShadowOnlyProxy,
		UStaticMesh* ProxyMesh,
		const FVector& LocalOffset,
		bool bEnable);

	/**
	 * Match active view camera (PIE player or editor viewport).
	 * Returns false if no valid view.
	 */
	GAUSSIANSIMVERSE_API bool SyncCaptureToActiveView(
		USceneCaptureComponent2D* Capture,
		UWorld* World,
		float TextureScale,
		UTextureRenderTarget2D*& InOutRT,
		UObject* RTOuter);

	/** Apply Skydome cubemap / exposure / rotation to a movable SkyLight (optional). */
	GAUSSIANSIMVERSE_API void ApplyEnvironment(
		USkyLightComponent* SkyLight,
		const FGaussianRelightSettings& Settings);

	/**
	 * When relighting is on: proxy must be capturable (visible to SceneCapture) but not
	 * drawn in the main lit pass unless Show Proxy is on.
	 */
	GAUSSIANSIMVERSE_API void ConfigureProxyVisibilityForRelight(
		UStaticMeshComponent* Proxy,
		bool bRelightEnabled,
		bool bShowProxyMesh,
		UMaterialInterface* RelightMaterial);

	/**
	 * ShowOnly capture only draws the proxy color. Other meshes must CastHiddenShadow so the
	 * sun can still cast onto the proxy (UE does not include non-listed meshes in the shadow map otherwise).
	 * Call once when enabling; Restore when disabling.
	 */
	GAUSSIANSIMVERSE_API void EnableHiddenShadowCasters(
		UWorld* World,
		UPrimitiveComponent* ExcludeProxy,
		TArray<TWeakObjectPtr<UPrimitiveComponent>>& InOutModified);

	GAUSSIANSIMVERSE_API void RestoreHiddenShadowCasters(
		TArray<TWeakObjectPtr<UPrimitiveComponent>>& InOutModified);

	/** Re-apply capture show-flags (shadows on, Lumen off) even if the component already exists. */
	GAUSSIANSIMVERSE_API void ApplyCaptureShowFlags(USceneCaptureComponent2D* Capture);

	GAUSSIANSIMVERSE_API FGaussianRelightFrameState MakeFrameState(
		const FGaussianRelightSettings& Settings,
		UTextureRenderTarget2D* RT);
}
