// Copyright Epic Games, Inc. All Rights Reserved.

#include "Rendering/GaussianRelighting.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/TextureCube.h"
#include "Engine/World.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/UObjectIterator.h"
#include "Engine/Engine.h"

#if WITH_EDITOR
#include "Editor.h"
#include "LevelEditorViewport.h"
#endif

namespace GaussianRelighting
{
	static TWeakObjectPtr<UMaterialInterface> GProxyLitMaterial;

	UMaterialInterface* GetOrCreateProxyLitMaterial()
	{
		if (UMaterialInterface* Existing = GProxyLitMaterial.Get())
		{
			return Existing;
		}

		// Prefer a real DefaultLit surface (BasicShape is often poorly lit / wrong params in capture).
		UMaterialInterface* Loaded = LoadObject<UMaterialInterface>(
			nullptr,
			TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));
		if (!Loaded)
		{
			Loaded = LoadObject<UMaterialInterface>(
				nullptr,
				TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
		}
		if (!Loaded)
		{
			Loaded = UMaterial::GetDefaultMaterial(MD_Surface);
		}
		GProxyLitMaterial = Loaded;
		return Loaded;
	}

	UMaterialInstanceDynamic* CreateProxyLitMID(UObject* Outer)
	{
		UMaterialInterface* Base = GetOrCreateProxyLitMaterial();
		if (!Base || !Outer)
		{
			return nullptr;
		}
		UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Base, Outer);
		if (MID)
		{
			// PlayCanvas-style ~0.5 gray albedo (Brightness≈2 compensates).
			const FLinearColor Gray(0.5f, 0.5f, 0.5f, 1.0f);
			MID->SetVectorParameterValue(TEXT("BaseColor"), Gray);
			MID->SetVectorParameterValue(TEXT("Color"), Gray);
			MID->SetScalarParameterValue(TEXT("Metallic"), 0.0f);
			MID->SetScalarParameterValue(TEXT("Specular"), 0.5f);
			MID->SetScalarParameterValue(TEXT("Roughness"), 0.7f);
		}
		return MID;
	}

	void EnsureComponents(
		AActor* Owner,
		USceneCaptureComponent2D*& InOutCapture,
		UTextureRenderTarget2D*& InOutRT,
		USkyLightComponent*& InOutSkyLight)
	{
		if (!Owner)
		{
			return;
		}

		if (!InOutCapture)
		{
			InOutCapture = NewObject<USceneCaptureComponent2D>(Owner, TEXT("GaussianRelightCapture"));
			InOutCapture->SetupAttachment(Owner->GetRootComponent());
			InOutCapture->RegisterComponent();
			InOutCapture->bCaptureEveryFrame = true;
			InOutCapture->bCaptureOnMovement = true;
			InOutCapture->bAlwaysPersistRenderingState = true;
			InOutCapture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
			// SceneColorHDR keeps linear lighting without full post stack; better for modulate factors.
			InOutCapture->CaptureSource = ESceneCaptureSource::SCS_SceneColorHDR;
			InOutCapture->bUseRayTracingIfEnabled = false;
		}
		// Always refresh flags (existing captures need shadow fixes too).
		ApplyCaptureShowFlags(InOutCapture);

		if (!InOutSkyLight)
		{
			InOutSkyLight = NewObject<USkyLightComponent>(Owner, TEXT("GaussianRelightSkyLight"));
			InOutSkyLight->SetupAttachment(Owner->GetRootComponent());
			InOutSkyLight->RegisterComponent();
			InOutSkyLight->SetMobility(EComponentMobility::Movable);
			InOutSkyLight->bRealTimeCapture = false;
			InOutSkyLight->SourceType = ESkyLightSourceType::SLS_SpecifiedCubemap;
			InOutSkyLight->SetVisibility(false);
			InOutSkyLight->SetIntensity(0.0f);
		}

		if (!InOutRT)
		{
			InOutRT = EnsureRenderTarget(InOutRT, Owner, 512, 512);
		}
	}

	UTextureRenderTarget2D* EnsureRenderTarget(
		UTextureRenderTarget2D*& InOutRT,
		UObject* Outer,
		int32 Width,
		int32 Height)
	{
		Width = FMath::Clamp(Width, 32, 4096);
		Height = FMath::Clamp(Height, 32, 4096);

		if (!InOutRT)
		{
			InOutRT = NewObject<UTextureRenderTarget2D>(Outer, NAME_None, RF_Transient);
			InOutRT->RenderTargetFormat = RTF_RGBA16f;
			InOutRT->ClearColor = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
			InOutRT->bAutoGenerateMips = false;
			InOutRT->InitAutoFormat(Width, Height);
			InOutRT->UpdateResourceImmediate(true);
			return InOutRT;
		}

		if (InOutRT->SizeX != Width || InOutRT->SizeY != Height)
		{
			InOutRT->InitAutoFormat(Width, Height);
			InOutRT->UpdateResourceImmediate(true);
		}
		return InOutRT;
	}

	void ConfigureCapture(
		USceneCaptureComponent2D* Capture,
		UStaticMeshComponent* ProxyMeshComponent,
		UTextureRenderTarget2D* RT)
	{
		if (!Capture)
		{
			return;
		}

		Capture->TextureTarget = RT;
		// Full scene: box/wall/character cast real shadows onto the gray proxy (not an isolated capture).
		Capture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;
		Capture->ShowOnlyComponents.Reset();
		Capture->ShowOnlyActors.Reset();
		if (Capture->IsRegistered())
		{
			Capture->ClearShowOnlyComponents();
		}
		Capture->bCaptureEveryFrame = true;
		ApplyCaptureShowFlags(Capture);

		// Ensure proxy is drawn in this capture (lit gray) even if hidden from main color.
		if (ProxyMeshComponent)
		{
			ProxyMeshComponent->SetHiddenInSceneCapture(false);
			ProxyMeshComponent->SetVisibility(true);
		}
	}

	void ConfigureMainViewShadowCaster(
		UStaticMeshComponent* ShadowOnlyProxy,
		UStaticMesh* ProxyMesh,
		const FVector& LocalOffset,
		bool bEnable)
	{
		if (!ShadowOnlyProxy)
		{
			return;
		}

		if (!bEnable || !ProxyMesh)
		{
			ShadowOnlyProxy->SetStaticMesh(nullptr);
			ShadowOnlyProxy->SetVisibility(false);
			ShadowOnlyProxy->SetCastShadow(false);
			ShadowOnlyProxy->bCastHiddenShadow = false;
			return;
		}

		ShadowOnlyProxy->SetStaticMesh(ProxyMesh);
		ShadowOnlyProxy->SetRelativeLocation(LocalOffset);
		ShadowOnlyProxy->SetRelativeRotation(FRotator::ZeroRotator);
		ShadowOnlyProxy->SetRelativeScale3D(FVector::OneVector);
		ShadowOnlyProxy->SetVisibility(true);
		ShadowOnlyProxy->SetHiddenInGame(false);
		ShadowOnlyProxy->SetHiddenInSceneCapture(true); // lighting RT uses the other proxy component
		ShadowOnlyProxy->SetRenderInMainPass(false);    // no gray mesh in beauty
		ShadowOnlyProxy->bRenderInDepthPass = false;
		ShadowOnlyProxy->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		ShadowOnlyProxy->SetCastShadow(true);
		ShadowOnlyProxy->bCastDynamicShadow = true;
		ShadowOnlyProxy->bCastStaticShadow = true;
		ShadowOnlyProxy->bCastContactShadow = true;
		ShadowOnlyProxy->bCastFarShadow = true;
		// Cast into the main view even though not drawn in the main color pass.
		ShadowOnlyProxy->bCastHiddenShadow = true;
		ShadowOnlyProxy->SetRenderCustomDepth(false);
		ShadowOnlyProxy->SetReceivesDecals(false);
		ShadowOnlyProxy->SetLightingChannels(true, false, false);
		ShadowOnlyProxy->MarkRenderStateDirty();
	}

	static bool GetActiveViewPose(UWorld* World, FVector& OutLoc, FRotator& OutRot, float& OutHFovDeg, FIntPoint& OutViewSize)
	{
		OutLoc = FVector::ZeroVector;
		OutRot = FRotator::ZeroRotator;
		OutHFovDeg = 90.0f;
		OutViewSize = FIntPoint(1280, 720);

		if (!World)
		{
			return false;
		}

		if (World->IsGameWorld())
		{
			if (APlayerController* PC = World->GetFirstPlayerController())
			{
				FVector Loc;
				FRotator Rot;
				PC->GetPlayerViewPoint(Loc, Rot);
				OutLoc = Loc;
				OutRot = Rot;
				if (PC->PlayerCameraManager)
				{
					OutHFovDeg = PC->PlayerCameraManager->GetFOVAngle();
				}
				if (GEngine && GEngine->GameViewport)
				{
					FVector2D Size;
					GEngine->GameViewport->GetViewportSize(Size);
					if (Size.X > 1.0 && Size.Y > 1.0)
					{
						OutViewSize = FIntPoint(FMath::RoundToInt(Size.X), FMath::RoundToInt(Size.Y));
					}
				}
				return true;
			}
		}

#if WITH_EDITOR
		if (GEditor)
		{
			FViewport* Active = GEditor->GetActiveViewport();
			if (FLevelEditorViewportClient* VC = static_cast<FLevelEditorViewportClient*>(
					Active ? Active->GetClient() : nullptr))
			{
				OutLoc = VC->GetViewLocation();
				OutRot = VC->GetViewRotation();
				OutHFovDeg = VC->FOVAngle;
				if (Active)
				{
					OutViewSize = FIntPoint(Active->GetSizeXY().X, Active->GetSizeXY().Y);
				}
				return true;
			}

			// Fallback: first perspective level viewport.
			for (FLevelEditorViewportClient* VC : GEditor->GetLevelViewportClients())
			{
				if (VC && VC->IsPerspective())
				{
					OutLoc = VC->GetViewLocation();
					OutRot = VC->GetViewRotation();
					OutHFovDeg = VC->FOVAngle;
					if (VC->Viewport)
					{
						OutViewSize = FIntPoint(VC->Viewport->GetSizeXY().X, VC->Viewport->GetSizeXY().Y);
					}
					return true;
				}
			}
		}
#endif
		return false;
	}

	bool SyncCaptureToActiveView(
		USceneCaptureComponent2D* Capture,
		UWorld* World,
		float TextureScale,
		UTextureRenderTarget2D*& InOutRT,
		UObject* RTOuter)
	{
		if (!Capture)
		{
			return false;
		}

		FVector Loc;
		FRotator Rot;
		float HFov = 90.0f;
		FIntPoint ViewSize;
		if (!GetActiveViewPose(World, Loc, Rot, HFov, ViewSize))
		{
			return false;
		}

		const float Scale = FMath::Clamp(TextureScale, 0.25f, 1.0f);
		const int32 W = FMath::Max(32, FMath::RoundToInt(ViewSize.X * Scale));
		const int32 H = FMath::Max(32, FMath::RoundToInt(ViewSize.Y * Scale));
		EnsureRenderTarget(InOutRT, RTOuter, W, H);
		Capture->TextureTarget = InOutRT;

		Capture->SetWorldLocationAndRotation(Loc, Rot);
		Capture->FOVAngle = HFov;
		// Force an immediate capture so the first frame after enabling is not a cleared black RT.
		Capture->CaptureScene();
		return true;
	}

	void ApplyEnvironment(
		USkyLightComponent* SkyLight,
		const FGaussianRelightSettings& Settings)
	{
		if (!SkyLight)
		{
			return;
		}

		if (!Settings.bEnabled || !Settings.Skydome)
		{
			SkyLight->SetVisibility(false);
			SkyLight->SetIntensity(0.0f);
			SkyLight->SetCubemap(nullptr);
			return;
		}

		SkyLight->SetVisibility(true);
		SkyLight->SourceType = ESkyLightSourceType::SLS_SpecifiedCubemap;
		SkyLight->SetCubemap(Settings.Skydome);
		// Map UI exposure to sky intensity (1.0 ≈ natural).
		SkyLight->SetIntensity(FMath::Max(0.0f, Settings.EnvExposure));
		SkyLight->SourceCubemapAngle = Settings.EnvRotationDegrees;
		SkyLight->MarkRenderStateDirty();
		SkyLight->SetCaptureIsDirty();
		SkyLight->UpdateSkyCaptureContents(SkyLight->GetWorld());
	}

	void ApplyCaptureShowFlags(USceneCaptureComponent2D* Capture)
	{
		if (!Capture)
		{
			return;
		}

		Capture->ShowFlags.SetAtmosphere(false);
		Capture->ShowFlags.SetFog(false);
		Capture->ShowFlags.SetParticles(false);
		Capture->ShowFlags.SetLighting(true);
		Capture->ShowFlags.SetDeferredLighting(true);
		Capture->ShowFlags.SetDirectLighting(true);
		Capture->ShowFlags.SetPointLights(true);
		Capture->ShowFlags.SetSpotLights(true);
		Capture->ShowFlags.SetRectLights(true);
		Capture->ShowFlags.SetSkyLighting(true);
		Capture->ShowFlags.SetStaticMeshes(true);
		Capture->ShowFlags.SetSkeletalMeshes(true);
		// Critical for sun/character shadows in the capture (CSM / VSM path).
		Capture->ShowFlags.SetDynamicShadows(true);
		// Lumen in SceneCapture is incomplete and often yields unshadowed / black-ish lighting.
		Capture->ShowFlags.SetLumenGlobalIllumination(false);
		Capture->ShowFlags.SetLumenReflections(false);
		Capture->ShowFlags.SetGlobalIllumination(false);
		Capture->ShowFlags.SetInstancedFoliage(true);
		Capture->ShowFlags.SetInstancedGrass(true);
		Capture->ShowFlags.SetLandscape(true);
		Capture->ShowFlags.SetPostProcessing(false);
		Capture->ShowFlags.SetTemporalAA(false);
		Capture->ShowFlags.SetMotionBlur(false);
		Capture->ShowFlags.SetBloom(false);
		Capture->ShowFlags.SetEyeAdaptation(false);
		Capture->ShowFlags.SetToneCurve(false);
		Capture->ShowFlags.SetTonemapper(false);
	}

	void ConfigureProxyVisibilityForRelight(
		UStaticMeshComponent* Proxy,
		bool bRelightEnabled,
		bool bShowProxyMesh,
		UMaterialInterface* RelightMaterial)
	{
		if (!Proxy)
		{
			return;
		}

		if (bRelightEnabled)
		{
			// Drawn in the full-scene lighting capture (receive sun + box shadows).
			// Main-view color only if Show Proxy; casting onto ground is a separate shadow-only mesh.
			Proxy->SetVisibility(true);
			Proxy->SetHiddenInGame(false);
			Proxy->SetHiddenInSceneCapture(false);
			Proxy->SetVisibleInSceneCaptureOnly(false);
			// Always in base pass so SceneCapture (full scene) includes this gray lit proxy.
			Proxy->SetRenderInMainPass(true);
			Proxy->bRenderInDepthPass = true;
			// Avoid double shadows with the dedicated shadow-only component.
			Proxy->SetCastShadow(false);
			Proxy->bCastDynamicShadow = false;
			Proxy->bCastStaticShadow = false;
			Proxy->bCastHiddenShadow = false;
			Proxy->SetReceivesDecals(false);
			Proxy->bAffectDynamicIndirectLighting = true;
			Proxy->SetLightingChannels(true, false, false);
			// If not showing proxy in beauty, hide main-view color via "only owner see" is wrong;
			// use VisibleInSceneCaptureOnly=false + hide with bRenderInMainPass only when showing.
			// When !Show: still need main pass for capture → use Capture-only visibility trick:
			//  VisibleInSceneCaptureOnly would kill main shadows (other component handles that).
			if (!bShowProxyMesh)
			{
				// Keep RenderInMainPass true for capture; hide from player by owner-no-see on non-capture.
				// SceneCapture ignores OwnerNoSee by default in many versions — use:
				Proxy->SetVisibleInSceneCaptureOnly(true);
			}
			if (RelightMaterial)
			{
				const int32 Num = FMath::Max(Proxy->GetNumMaterials(), 1);
				for (int32 i = 0; i < Num; ++i)
				{
					Proxy->SetMaterial(i, RelightMaterial);
				}
			}
			Proxy->MarkRenderStateDirty();
		}
		else
		{
			Proxy->SetVisibleInSceneCaptureOnly(false);
			Proxy->SetCastShadow(false);
			Proxy->bCastHiddenShadow = false;
		}
	}

	void EnableHiddenShadowCasters(
		UWorld* World,
		UPrimitiveComponent* ExcludeProxy,
		TArray<TWeakObjectPtr<UPrimitiveComponent>>& InOutModified)
	{
		// Full-scene capture no longer needs CastHiddenShadow hacks; keep API for restore on disable.
		(void)World;
		(void)ExcludeProxy;
		(void)InOutModified;
	}

	void RestoreHiddenShadowCasters(
		TArray<TWeakObjectPtr<UPrimitiveComponent>>& InOutModified)
	{
		for (const TWeakObjectPtr<UPrimitiveComponent>& Weak : InOutModified)
		{
			if (UPrimitiveComponent* Prim = Weak.Get())
			{
				Prim->bCastHiddenShadow = false;
				Prim->MarkRenderStateDirty();
			}
		}
		InOutModified.Reset();
	}

	FGaussianRelightFrameState MakeFrameState(
		const FGaussianRelightSettings& Settings,
		UTextureRenderTarget2D* RT)
	{
		FGaussianRelightFrameState State;
		State.bEnabled = Settings.bEnabled && RT != nullptr;
		State.Blend = FMath::Clamp(Settings.Blend, 0.0f, 1.0f);
		State.Exposure = FMath::Max(0.0f, Settings.Exposure);
		State.Brightness = FMath::Max(0.0f, Settings.Brightness);
		State.Background = FMath::Max(0.0f, Settings.Background);
		State.bDebug = Settings.bDebug;
		State.RenderTarget = RT;
		return State;
	}
}
