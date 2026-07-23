// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianStreamedSceneActor.h"
#include "GaussianStreamedSceneAsset.h"
#include "GaussianScene.h"
#include "GaussianSimVerse.h"
#include "Rendering/GaussianRenderer.h"
#include "Rendering/GaussianRenderSettings.h"
#include "Rendering/GaussianProxyDofMitigation.h"
#include "Rendering/GaussianRelighting.h"
#include "Components/BillboardComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/TextureCube.h"
#include "Engine/Engine.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"

#if WITH_EDITOR
#include "Editor.h"
#include "LevelEditorViewport.h"
#include "Editor/GaussianProxyMeshGenerator.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#endif

AGaussianStreamedSceneActor::AGaussianStreamedSceneActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	// Editor pick target: streamed Gaussians have no standard mesh hit proxies.
	// Wireframe bounds box is used for click selection (no billboard icon).
	EditorSprite = CreateDefaultSubobject<UBillboardComponent>(TEXT("EditorSprite"));
	EditorSprite->SetupAttachment(SceneRoot);
	EditorSprite->SetHiddenInGame(true);
	EditorSprite->SetVisibility(false);
	EditorSprite->SetHiddenInSceneCapture(true);

	BoundsVisual = CreateDefaultSubobject<UBoxComponent>(TEXT("BoundsVisual"));
	BoundsVisual->SetupAttachment(SceneRoot);
	BoundsVisual->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	BoundsVisual->SetCollisionObjectType(ECC_WorldDynamic);
	BoundsVisual->SetCollisionResponseToAllChannels(ECR_Ignore);
	BoundsVisual->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
	BoundsVisual->SetGenerateOverlapEvents(false);
	BoundsVisual->SetCanEverAffectNavigation(false);
	BoundsVisual->SetHiddenInGame(true);
	BoundsVisual->SetVisibility(true);
	BoundsVisual->ShapeColor = FColor(80, 200, 255);
	BoundsVisual->SetLineThickness(1.5f);
	// Wireframe only when selected; collision stays on for click-picking.
	BoundsVisual->bDrawOnlyIfSelected = true;
	BoundsVisual->SetBoxExtent(FVector(100.0f), false);

	static ConstructorHelpers::FObjectFinder<UTexture2D> SpriteFinder(TEXT("/Engine/EditorResources/S_Note"));
	if (SpriteFinder.Succeeded())
	{
		EditorSprite->SetSprite(SpriteFinder.Object);
	}

	ProxyMeshComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ProxyMeshComponent"));
	ProxyMeshComponent->SetupAttachment(SceneRoot);
	ProxyMeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ProxyMeshComponent->SetCastShadow(false);
	ProxyMeshComponent->SetHiddenInGame(true);
	ProxyMeshComponent->SetVisibility(false);
	ProxyMeshComponent->SetGenerateOverlapEvents(false);
}

void AGaussianStreamedSceneActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	// Property edits can remount components �?do not force a full stream restart.
	InitializeStreaming(false);
	RefreshRenderRegistration();
	ApplyStreamingCVarOverrides();
	StreamingManager.UpdateStreaming(GetStreamingViewOrigin(), GetStreamingViewDirection());

#if WITH_EDITOR
	if (!EditorCameraMovedHandle.IsValid())
	{
		EditorCameraMovedHandle = FEditorDelegates::OnEditorCameraMoved.AddUObject(this, &AGaussianStreamedSceneActor::HandleEditorCameraMoved);
	}
	if (!EditorStreamingTickHandle.IsValid())
	{
		// Bootstrap: near every-frame commits; steady: slower idle ticks.
		EditorStreamingTickInterval = 0.0f;
		EditorStreamingTickHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateUObject(this, &AGaussianStreamedSceneActor::HandleEditorStreamingTick),
			EditorStreamingTickInterval);
	}
#endif
}

void AGaussianStreamedSceneActor::PostUnregisterAllComponents()
{
#if WITH_EDITOR
	if (EditorCameraMovedHandle.IsValid())
	{
		FEditorDelegates::OnEditorCameraMoved.Remove(EditorCameraMovedHandle);
		EditorCameraMovedHandle.Reset();
	}
	if (EditorStreamingTickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(EditorStreamingTickHandle);
		EditorStreamingTickHandle.Reset();
	}
#endif

	// Detach from renderer only. Do NOT ShutdownStreaming here: editor property edits
	// (Debug Render, etc.) remount components and would wipe / reload every chunk.
	UnregisterScene();
	Super::PostUnregisterAllComponents();
}

void AGaussianStreamedSceneActor::SetStreamedSceneAsset(UGaussianStreamedSceneAsset* InAsset)
{
	StreamedSceneAsset = InAsset;
	SnapActorToSceneOrigin();
	InitializeStreaming(true);
	RefreshRenderRegistration();
	ApplyStreamingCVarOverrides();
	StreamingManager.UpdateStreaming(GetStreamingViewOrigin(), GetStreamingViewDirection());
}

void AGaussianStreamedSceneActor::NotifyStreamingChunkLoaded()
{
	// Avoid RefreshRenderRegistration here: MarkSceneDirty already rebuilds proxies; re-initing triggers extra hitches.
	if (GaussianScene && !GaussianScene->IsRegisteredWithRenderer())
	{
		TryRegisterScene();
	}
}

void AGaussianStreamedSceneActor::DrawStreamingDebugOverlay(const FGaussianStreamingManager& Manager) const
{
	if (!GEngine)
	{
		return;
	}

	const FString Message = FString::Printf(
		TEXT("Gaussian Streaming | Chunks=%d Desired=%d Pending=%d Splats=%d | Motion=%s Bootstrap=%s"),
		Manager.GetLoadedChunkCount(),
		Manager.GetDesiredChunkCount(),
		Manager.GetPendingLoadCount(),
		Manager.GetLoadedSplatCount(),
		Manager.IsCameraInMotion() ? TEXT("Y") : TEXT("N"),
		Manager.IsBootstrapActive() ? TEXT("Y") : TEXT("N"));

	const int32 MessageKey = static_cast<int32>(GetUniqueID());
	GEngine->AddOnScreenDebugMessage(MessageKey, 0.2f, FColor::Cyan, Message);
}

void AGaussianStreamedSceneActor::SyncSceneSettings()
{
	if (!GaussianScene)
	{
		return;
	}

	GaussianScene->ShBand = ShBandOverride;
	GaussianScene->Colors = Colors;
	GaussianScene->DofMode = ProxyMesh ? ProxyDofMode : EGaussianProxyDofMode::Off;
	GaussianScene->bUseProxyDepthOfField = (GaussianScene->DofMode != EGaussianProxyDofMode::Off);
	GaussianScene->DofFocalDistanceCm = ProxyDofFocalDistanceCm;
	GaussianScene->DofCocScale = ProxyDofCocScale;
	GaussianScene->DofMaxBlurRadiusPx = ProxyDofMaxBlurRadiusPx;
	GaussianScene->DofProxyStencil = static_cast<uint32>(FMath::Clamp(ProxyCustomDepthStencilValue, 0, 255));
}

void AGaussianStreamedSceneActor::ApplyStreamingCVarOverrides() const
{
	if (!bApplyStreamingCVarOverrides)
	{
		return;
	}

	GaussianSimVerse::RenderSettings::CVarStreamingEnable->Set(bStreamingEnable ? 1 : 0, ECVF_SetByCode);
	GaussianSimVerse::RenderSettings::CVarStreamingLoadRadius->Set(StreamingLoadRadius, ECVF_SetByCode);
	GaussianSimVerse::RenderSettings::CVarStreamingLodBaseDistance->Set(StreamingLodBaseDistance, ECVF_SetByCode);
	GaussianSimVerse::RenderSettings::CVarStreamingMaxLoadedSplats->Set(StreamingMaxLoadedSplats, ECVF_SetByCode);
	GaussianSimVerse::RenderSettings::CVarStreamingMaxLoadsPerFrame->Set(StreamingMaxLoadsPerFrame, ECVF_SetByCode);
	GaussianSimVerse::RenderSettings::CVarStreamingMaxCommitSplatsPerFrame->Set(StreamingMaxCommitSplatsPerFrame, ECVF_SetByCode);
	GaussianSimVerse::RenderSettings::CVarStreamingMotionLodBias->Set(StreamingMotionLodBias, ECVF_SetByCode);
	GaussianSimVerse::RenderSettings::CVarStreamingLodUnderfillLimit->Set(StreamingLodUnderfillLimit, ECVF_SetByCode);
	GaussianSimVerse::RenderSettings::CVarStreamingDebugDraw->Set(bStreamingDebugDraw ? 1 : 0, ECVF_SetByCode);
	GaussianSimVerse::RenderSettings::CVarStreamingDebugOverlay->Set(bStreamingDebugOverlay ? 1 : 0, ECVF_SetByCode);
	GaussianSimVerse::RenderSettings::CVarStreamingDebugRenderMode->Set(static_cast<int32>(DebugRenderMode), ECVF_SetByCode);
}

void AGaussianStreamedSceneActor::UpdateStreamingFromView()
{
	if (!GaussianSimVerse::RenderSettings::IsStreamingEnabled()
		|| !StreamedSceneAsset
		|| !GaussianScene
		|| !ShouldRenderGaussian())
	{
		return;
	}

	StreamingManager.UpdateStreaming(GetStreamingViewOrigin(), GetStreamingViewDirection());
}

void AGaussianStreamedSceneActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	if (!GaussianScene)
	{
		GaussianScene = NewObject<UGaussianScene>(this, TEXT("GaussianScene"));
	}

	GaussianScene->WorldTransform = GetActorTransform();
	SyncSceneSettings();
	ApplyStreamingCVarOverrides();
	UpdateBoundsVisual();
	ApplyProxyMeshSettings();
	// Property edits rerun construction scripts �?never wipe resident streaming here.
	InitializeStreaming(false);
	RefreshRenderRegistration();
}

void AGaussianStreamedSceneActor::BeginPlay()
{
	Super::BeginPlay();
	ApplyStreamingCVarOverrides();
	InitializeStreaming(false);
	RefreshRenderRegistration();
}

void AGaussianStreamedSceneActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (bProxyDofMitigationHeld)
	{
		GaussianProxyDofMitigation::SetActive(false);
		bProxyDofMitigationHeld = false;
	}
	ShutdownStreaming();
	UnregisterScene();
	Super::EndPlay(EndPlayReason);
}

void AGaussianStreamedSceneActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UpdateStreamingFromView();
	if (bEnableRelighting)
	{
		UpdateRelighting();
	}
}

bool AGaussianStreamedSceneActor::ShouldTickIfViewportsOnly() const
{
	// Streamed actor already ticks for LOD; always allow viewport-only tick when placed in editor.
	return true;
}

void AGaussianStreamedSceneActor::BeginDestroy()
{
	if (bProxyDofMitigationHeld)
	{
		GaussianProxyDofMitigation::SetActive(false);
		bProxyDofMitigationHeld = false;
	}
	ShutdownStreaming();
	UnregisterScene();
	Super::BeginDestroy();
}

void AGaussianStreamedSceneActor::SetActorHiddenInGame(bool bNewHidden)
{
	const bool bWasHidden = IsHidden();
	Super::SetActorHiddenInGame(bNewHidden);
	if (bWasHidden != IsHidden())
	{
		RefreshRenderRegistration();
	}
}

#if WITH_EDITOR
void AGaussianStreamedSceneActor::SetIsTemporarilyHiddenInEditor(bool bIsHidden)
{
	const bool bWasHidden = IsTemporarilyHiddenInEditor();
	Super::SetIsTemporarilyHiddenInEditor(bIsHidden);
	if (bWasHidden != IsTemporarilyHiddenInEditor())
	{
		RefreshRenderRegistration();
	}
}

void AGaussianStreamedSceneActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropertyName = PropertyChangedEvent.GetPropertyName();

	// Relight sliders must not re-enter ApplyProxyMesh / streaming rebuilds.
	const bool bRelightParamOnly =
		PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, RelightBlend)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, RelightExposure)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, RelightBrightness)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, RelightBackground)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, RelightTextureScale)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, bRelightDebug)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, RelightEnvExposure)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, RelightEnvRotation);
	if (bRelightParamOnly)
	{
		UpdateRelighting();
		return;
	}

	if (PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, bEnableRelighting)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, RelightSkydome))
	{
		if (bEnableRelighting)
		{
			ApplyProxyMeshSettings();
		}
		else
		{
			UpdateRelighting();
		}
		return;
	}

	if (PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, StreamedSceneAsset))
	{
		SnapActorToSceneOrigin();
		InitializeStreaming(true);
		RefreshRenderRegistration();
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, bEnableRendering))
	{
		RefreshRenderRegistration();
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, ProxyMesh)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, bShowProxyMesh)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, bProxyWriteCustomDepth)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, bProxyWriteSceneDepth)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, bProxyEnableCollision)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, ProxyDofMode)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, bProxyDofSuppressScreenSpaceAO)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, ProxyCustomDepthStencilValue))
	{
		if (PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, ProxyDofMode)
			&& ProxyDofMode != EGaussianProxyDofMode::Off)
		{
			bProxyWriteCustomDepth = true;
			bProxyWriteSceneDepth = false;
		}
		ApplyProxyMeshSettings();
		SyncDepthOfFieldToScene();
		SyncSceneSettings();
		if (GaussianScene && GaussianScene->IsRegisteredWithRenderer())
		{
			FGaussianRenderer::Get().MarkSceneDirty(GaussianScene);
		}
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, ProxyDofFocalDistanceCm)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, ProxyDofCocScale)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, ProxyDofMaxBlurRadiusPx))
	{
		SyncSceneSettings();
		if (GaussianScene && GaussianScene->IsRegisteredWithRenderer())
		{
			FGaussianRenderer::Get().MarkSceneDirty(GaussianScene);
		}
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, bStreamingDebugDraw)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, bStreamingDebugOverlay)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, DebugRenderMode))
	{
		// Debug Render is a per-frame CVar tint �?never restart streaming or rebuild proxies.
		ApplyStreamingCVarOverrides();
		if (bStreamingDebugOverlay)
		{
			DrawStreamingDebugOverlay(StreamingManager);
		}
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, bApplyStreamingCVarOverrides)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, bStreamingEnable)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, StreamingLoadRadius)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, StreamingLodBaseDistance)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, StreamingMaxLoadedSplats)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, StreamingMaxLoadsPerFrame)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, StreamingMaxCommitSplatsPerFrame)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, StreamingMotionLodBias)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, StreamingLodUnderfillLimit))
	{
		ApplyStreamingCVarOverrides();
		if (bStreamingEnable)
		{
			UpdateStreamingFromView();
		}
		else
		{
			ShutdownStreaming();
			RefreshRenderRegistration();
		}
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, ShBandOverride)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, Colors)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(FGaussianColorAdjustment, Temperature)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(FGaussianColorAdjustment, Saturation)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(FGaussianColorAdjustment, Brightness)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(FGaussianColorAdjustment, BlackPoint)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(FGaussianColorAdjustment, WhitePoint)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(FGaussianColorAdjustment, Transparency))
	{
		SyncSceneSettings();
		if (GaussianScene && GaussianScene->IsRegisteredWithRenderer())
		{
			FGaussianRenderer::Get().MarkSceneDirty(GaussianScene);
		}
	}
}

void AGaussianStreamedSceneActor::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);

	// Update every drag tick (bFinished==false) so streamed clouds follow the actor while moving,
	// not only on mouse release.
	if (!GaussianScene || !bEnableRendering)
	{
		return;
	}

	GaussianScene->WorldTransform = GetActorTransform();
	if (GaussianScene->IsRegisteredWithRenderer())
	{
		FGaussianRenderer::Get().MarkSceneDirty(GaussianScene);
	}
}

void AGaussianStreamedSceneActor::Destroyed()
{
	ShutdownStreaming();
	UnregisterScene();
	Super::Destroyed();
}
#endif

bool AGaussianStreamedSceneActor::ShouldRenderGaussian() const
{
	if (!bEnableRendering || !StreamedSceneAsset)
	{
		return false;
	}

	if (IsHidden())
	{
		return false;
	}

#if WITH_EDITOR
	if (IsTemporarilyHiddenInEditor())
	{
		return false;
	}
#endif

	return true;
}

void AGaussianStreamedSceneActor::RefreshRenderRegistration()
{
	if (!GaussianScene)
	{
		return;
	}

	GaussianScene->bEnableRendering = ShouldRenderGaussian();
	if (!GaussianScene->bEnableRendering)
	{
		UnregisterScene();
		return;
	}

	if (!GaussianScene->IsRegisteredWithRenderer())
	{
		TryRegisterScene();
		return;
	}

	GaussianScene->WorldTransform = GetActorTransform();
	SyncSceneSettings();
	FGaussianRenderer::Get().MarkSceneDirty(GaussianScene);
}

void AGaussianStreamedSceneActor::InitializeStreaming(bool bForceRestart)
{
	if (!GaussianScene)
	{
		GaussianScene = NewObject<UGaussianScene>(this, TEXT("GaussianScene"));
	}

	if (!StreamedSceneAsset)
	{
		ShutdownStreaming();
		return;
	}

	// Placement uses SOG splat coordinates (UE cm after import), NOT lod-meta tree.bound.
	// Some exports (e.g. large interiors) ship tree.bound in a different scale/frame than SOG means;
	// SuperSplat still looks centered because it frames actual splat positions. We keep pivot at 0
	// so ChunkOffset = Asset.Bounds.Origin and the actor at origin matches SuperSplat framing.
	GaussianScene->DatasetPivot = FVector::ZeroVector;
	GaussianScene->bHasDatasetPivot = true;

	if (!bForceRestart
		&& bStreamingInitialized
		&& StreamingManager.GetLoadedChunkCount() > 0)
	{
		GaussianScene->WorldTransform = GetActorTransform();
		SyncSceneSettings();
		UpdateBoundsVisual();
		return;
	}

	GaussianScene->WorldTransform = GetActorTransform();
	SyncSceneSettings();
	StreamingManager.Initialize(this, StreamedSceneAsset, GaussianScene);
	bStreamingInitialized = true;
	UpdateBoundsVisual();
}

bool AGaussianStreamedSceneActor::IsStreamingInitialized() const
{
	return bStreamingInitialized;
}

void AGaussianStreamedSceneActor::ShutdownStreaming()
{
	StreamingManager.Shutdown();
	bStreamingInitialized = false;
	if (GaussianScene)
	{
		GaussianScene->Chunks.Reset();
	}
}

void AGaussianStreamedSceneActor::SnapActorToSceneOrigin()
{
	if (!StreamedSceneAsset)
	{
		return;
	}

	// Do NOT snap to lod-meta SceneBounds.Origin: on some datasets that AABB is inconsistent with
	// SOG position ranges (model near 0, tree.bound hundreds of meters away). SuperSplat centers
	// the view on splat data, which is near the authoring origin �?place the actor there too.
	const FVector TargetLocation = FVector::ZeroVector;
	if (!GetActorLocation().Equals(TargetLocation, 1.0f))
	{
		SetActorLocation(TargetLocation, false, nullptr, ETeleportType::TeleportPhysics);
	}
}

void AGaussianStreamedSceneActor::SyncDepthOfFieldToScene()
{
	const bool bWantDof = (ProxyDofMode != EGaussianProxyDofMode::Off) && ProxyMesh != nullptr;
	const bool bWantMitigation = bWantDof && bProxyDofSuppressScreenSpaceAO;
	if (bWantMitigation != bProxyDofMitigationHeld)
	{
		GaussianProxyDofMitigation::SetActive(bWantMitigation);
		bProxyDofMitigationHeld = bWantMitigation;
	}

	if (!GaussianScene)
	{
		return;
	}

	if (GaussianScene->bUseProxyDepthOfField != bWantDof)
	{
		GaussianScene->bUseProxyDepthOfField = bWantDof;
		if (GaussianScene->IsRegisteredWithRenderer())
		{
			FGaussianRenderer::Get().MarkSceneDirty(GaussianScene);
		}
	}
}

void AGaussianStreamedSceneActor::ApplyProxyMeshSettings()
{
	if (!ProxyMeshComponent)
	{
		return;
	}

	// NEVER early-write SceneDepth for beauty (blocks skydome → black sky).
	if (ProxyDofMode != EGaussianProxyDofMode::Off)
	{
		bProxyWriteCustomDepth = true;
	}
	bProxyWriteSceneDepth = false;

	ProxyMeshComponent->SetStaticMesh(ProxyMesh);

	const bool bWantsCustomDepth = bProxyWriteCustomDepth || (ProxyDofMode != EGaussianProxyDofMode::Off);
	const bool bEarlySceneDepth = false; // hard-disable: large proxy hull blacks out sky
	const bool bWantsAnyDepthPass = bWantsCustomDepth;
	const bool bComponentActive = ProxyMesh != nullptr
		&& (bShowProxyMesh || bWantsAnyDepthPass || bProxyEnableCollision
			|| (ProxyDofMode != EGaussianProxyDofMode::Off) || bEnableRelighting);

	ProxyMeshComponent->SetVisibility(bComponentActive);
	ProxyMeshComponent->SetHiddenInGame(!bComponentActive);
	ProxyMeshComponent->SetRenderCustomDepth(bWantsCustomDepth);
	ProxyMeshComponent->SetCustomDepthStencilValue(FMath::Clamp(ProxyCustomDepthStencilValue, 1, 255));
	// Do NOT pass bEnableRelighting as show-color (that forced main DepthPass → black sky).
	GaussianProxyDofMitigation::ConfigureDepthOnlyComponent(
		ProxyMeshComponent,
		bShowProxyMesh,
		bEarlySceneDepth);

	if (bProxyEnableCollision)
	{
		ProxyMeshComponent->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		ProxyMeshComponent->SetCollisionResponseToAllChannels(ECR_Block);
		ProxyMeshComponent->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
	}
	else if (bShowProxyMesh)
	{
		ProxyMeshComponent->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		ProxyMeshComponent->SetCollisionResponseToAllChannels(ECR_Ignore);
		ProxyMeshComponent->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
	}
	else
	{
		ProxyMeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}

	ProxyMeshComponent->SetRelativeLocation(ProxyMeshLocalOffset);
	ProxyMeshComponent->SetRelativeRotation(FRotator::ZeroRotator);
	ProxyMeshComponent->SetRelativeScale3D(FVector::OneVector);

	if (ProxyMesh && bComponentActive && !bEnableRelighting)
	{
		if (UMaterialInterface* DepthMat = LoadObject<UMaterialInterface>(
			nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")))
		{
			const int32 NumMaterials = FMath::Max(ProxyMeshComponent->GetNumMaterials(), 1);
			for (int32 Index = 0; Index < NumMaterials; ++Index)
			{
				if (!ProxyMeshComponent->GetMaterial(Index) || !bShowProxyMesh)
				{
					ProxyMeshComponent->SetMaterial(Index, DepthMat);
				}
			}
		}
	}

	ProxyMeshComponent->MarkRenderStateDirty();
	SyncDepthOfFieldToScene();
	UpdateRelighting();
}

void AGaussianStreamedSceneActor::UpdateRelighting()
{
	const bool bWant = bEnableRelighting && ProxyMesh != nullptr && ProxyMeshComponent != nullptr;

	if (!bWant)
	{
		if (RelightCapture)
		{
			RelightCapture->bCaptureEveryFrame = false;
			RelightCapture->TextureTarget = nullptr;
			RelightCapture->ShowOnlyComponents.Reset();
		}
		if (RelightSkyLight)
		{
			FGaussianRelightSettings Off;
			GaussianRelighting::ApplyEnvironment(RelightSkyLight, Off);
		}
		if (ProxyMeshComponent)
		{
			ProxyMeshComponent->SetVisibleInSceneCaptureOnly(false);
		}
		GaussianRelighting::ConfigureMainViewShadowCaster(RelightShadowProxy, nullptr, FVector::ZeroVector, false);
		GaussianRelighting::RestoreHiddenShadowCasters(RelightHiddenShadowCasters);
		// Do not clear global relight if another actor owns it — only clear if we were the source.
		FGaussianRelightFrameState Empty;
		if (RelightRenderTarget)
		{
			const FGaussianRelightFrameState Cur = FGaussianRenderer::Get().GetRelightFrameState();
			if (Cur.RenderTarget.Get() == RelightRenderTarget)
			{
				FGaussianRenderer::Get().SetRelightFrameState_GameThread(Empty);
			}
		}
		return;
	}

	USceneCaptureComponent2D* Capture = RelightCapture;
	UTextureRenderTarget2D* RT = RelightRenderTarget;
	USkyLightComponent* Sky = RelightSkyLight;
	GaussianRelighting::EnsureComponents(this, Capture, RT, Sky);
	RelightCapture = Capture;
	RelightRenderTarget = RT;
	RelightSkyLight = Sky;

	if (!RelightShadowProxy)
	{
		RelightShadowProxy = NewObject<UStaticMeshComponent>(this, TEXT("RelightShadowProxy"));
		RelightShadowProxy->SetupAttachment(GetRootComponent());
		RelightShadowProxy->RegisterComponent();
	}

	if (!RelightProxyMaterial)
	{
		RelightProxyMaterial = GaussianRelighting::CreateProxyLitMID(this);
	}

	GaussianRelighting::ConfigureProxyVisibilityForRelight(
		ProxyMeshComponent,
		true,
		bShowProxyMesh,
		RelightProxyMaterial);

	GaussianRelighting::ConfigureMainViewShadowCaster(
		RelightShadowProxy,
		ProxyMesh,
		ProxyMeshLocalOffset,
		true);

	GaussianRelighting::ConfigureCapture(RelightCapture, ProxyMeshComponent, RelightRenderTarget);
	GaussianRelighting::SyncCaptureToActiveView(
		RelightCapture,
		GetWorld(),
		RelightTextureScale,
		RT,
		this);
	RelightRenderTarget = RT;
	if (RelightCapture)
	{
		RelightCapture->TextureTarget = RelightRenderTarget;
	}

	FGaussianRelightSettings Settings;
	Settings.bEnabled = true;
	Settings.Blend = RelightBlend;
	Settings.Exposure = RelightExposure;
	Settings.Brightness = RelightBrightness;
	Settings.Background = RelightBackground;
	Settings.TextureScale = RelightTextureScale;
	Settings.bDebug = bRelightDebug;
	Settings.EnvExposure = RelightEnvExposure;
	Settings.EnvRotationDegrees = RelightEnvRotation;
	Settings.Skydome = RelightSkydome;
	GaussianRelighting::ApplyEnvironment(RelightSkyLight, Settings);

	FGaussianRenderer::Get().SetRelightFrameState_GameThread(
		GaussianRelighting::MakeFrameState(Settings, RelightRenderTarget));
}

#if WITH_EDITOR
void AGaussianStreamedSceneActor::SetupDepthOfField()
{
	if (!ProxyMesh)
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(
			TEXT("Generate or assign a Proxy Mesh first.\n")
			TEXT("DOF uses the proxy as Scene Depth geometry for Unreal Cinematic DOF.")));
		return;
	}

	ProxyDofMode = EGaussianProxyDofMode::CineCamera;
	bProxyWriteCustomDepth = true;
	bProxyWriteSceneDepth = false;
	bShowProxyMesh = false;
	bProxyDofSuppressScreenSpaceAO = false;
	ApplyProxyMeshSettings();
	SyncDepthOfFieldToScene();

	FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(
		TEXT("DOF Mode = CineCamera (Engine Diaphragm DOF).\n\n")
		TEXT("Configured:\n")
		TEXT("- Write Custom Depth ON\n")
		TEXT("- Write Scene Depth OFF (keeps sky clean)\n")
		TEXT("- Gaussians inject BeforeDOF\n")
		TEXT("- CustomDepth merged into SceneDepth just before engine DOF\n\n")
		TEXT("On CineCamera:\n")
		TEXT("- Enable Depth of Field (Cinematic)\n")
		TEXT("- Set Focus Distance / Focus Target + Aperture\n\n")
		TEXT("Console: r.CustomDepth 3 , r.DepthOfFieldQuality 2\n")
		TEXT("Do NOT enable early Write Scene Depth.")));
}

void AGaussianStreamedSceneActor::GenerateProxyMeshFromDataset()
{
	if (!StreamedSceneAsset)
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("Assign a Streamed Scene Asset first.")));
		return;
	}

	FGaussianProxyMeshBuildSettings Settings;
	Settings.SceneType = ProxySceneType;
	Settings.FillSealSizeCm = FMath::Max(ProxyFillSealSizeCm, 1.0f);
	Settings.FillSeedPosition = ProxyFillSeedLocal;
	Settings.bEnableCarve = bProxyEnableCarve;
	Settings.CarveHeightCm = FMath::Max(ProxyCarveHeightCm, 10.0f);
	Settings.CarveRadiusCm = FMath::Max(ProxyCarveRadiusCm, 1.0f);
	Settings.VoxelSizeCm = FMath::Max(ProxyVoxelSizeCm, 1.0f);
	Settings.MeshMode = ProxyMeshMode;
	Settings.SmoothIterations = ProxySmoothIterations;
	Settings.bAutoGrowVoxelSize = bProxyAutoGrowVoxelSize;
	Settings.MinOpacity = ProxyMinOpacity;
	Settings.MaxSamplePoints = ProxyMaxSamplePoints;
	Settings.DilateRings = ProxyDilateRings;
	Settings.ShrinkRings = ProxyShrinkRings;
	Settings.MinHitsPerVoxel = ProxyMinHitsPerVoxel;
	Settings.MinSolidIslandVoxels = ProxyMinSolidIslandVoxels;
	Settings.bKeepPrimarySolidComponent = bProxyKeepPrimarySolid;
	// Keep dataset/actor-local space so mesh and Gaussian share one frame (asset + level).
	Settings.bCenterMeshAtOrigin = false;
	// Save next to the streamed scene asset package for easier content management.
	if (const UPackage* AssetPackage = StreamedSceneAsset->GetOutermost())
	{
		Settings.PackagePath = FPackageName::GetLongPackagePath(AssetPackage->GetName());
	}

	TArray<FVector> Points;
	FString Error;
	if (!FGaussianProxyMeshGenerator::CollectPointsFromStreamedAsset(StreamedSceneAsset, Settings, Points, Error))
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error));
		return;
	}

	// Streamed: sample points are dataset/absolute UE coords (= actor-local when DatasetPivot=0).
	// Mesh is recentered for asset preview; ProxyMeshLocalOffset restores scene placement.

	FBox SampleBounds(ForceInit);
	for (const FVector& P : Points)
	{
		SampleBounds += P;
	}

	// Detach current proxy before rebuild so StaticMesh GPU resources can be released safely.
	if (ProxyMeshComponent)
	{
		ProxyMeshComponent->SetStaticMesh(nullptr);
	}
	ProxyMesh = nullptr;

	const FString AssetName = FString::Printf(TEXT("%s_Proxy"), *StreamedSceneAsset->GetName());
	FVector LocalOffset = FVector::ZeroVector;
	float ActualVoxelCm = Settings.VoxelSizeCm;
	UStaticMesh* Mesh = FGaussianProxyMeshGenerator::BuildMeshFromPoints(
		Points, Settings, AssetName, Error, &LocalOffset, &ActualVoxelCm);
	if (!Mesh)
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error));
		return;
	}

	// Keep UI in sync if auto-grow raised voxel size for a large LOD scene.
	if (ActualVoxelCm > ProxyVoxelSizeCm + 0.5f)
	{
		ProxyVoxelSizeCm = ActualVoxelCm;
	}

	ProxyMesh = Mesh;
	ProxyMeshLocalOffset = LocalOffset;
	bShowProxyMesh = true;
	ApplyProxyMeshSettings();

	const FBoxSphereBounds MeshBounds = Mesh->GetBounds();
	const FString AutoGrowNote = (ActualVoxelCm > Settings.VoxelSizeCm + 0.5f)
		? FString::Printf(
			TEXT("\n(Auto-raised Voxel Size %.1f �?%.1f cm: scene AABB too large for requested size.\n")
			TEXT(" Single Object max ~2048 cells/axis; Room/Outdoor also has dense cell budget.\n")
			TEXT(" Uncheck Auto Grow Voxel Size to fail instead of growing.)"),
			Settings.VoxelSizeCm, ActualVoxelCm)
		: FString();
	FMessageDialog::Open(
		EAppMsgType::Ok,
		FText::FromString(FString::Printf(
			TEXT("Proxy mesh generated: %s\nPoints sampled: %d\nVoxel size used: %.1f cm%s\nSample extent: %s\nMesh extent: %s\nLocal offset: %s\nTip: Mesh Mode Surface Smooth = Faces + Laplacian; Show=off + Custom Depth for beauty."),
			*Mesh->GetPathName(),
			Points.Num(),
			ActualVoxelCm,
			*AutoGrowNote,
			*SampleBounds.GetExtent().ToCompactString(),
			*FVector(MeshBounds.BoxExtent).ToCompactString(),
			*ProxyMeshLocalOffset.ToCompactString())));
}
#endif

void AGaussianStreamedSceneActor::TryRegisterScene()
{
	if (!GetWorld() || !GaussianScene || !ShouldRenderGaussian())
	{
		return;
	}

	RegisterScene();
}

void AGaussianStreamedSceneActor::RegisterScene()
{
	if (!GaussianScene || !ShouldRenderGaussian() || !StreamedSceneAsset)
	{
		return;
	}

	GaussianScene->WorldTransform = GetActorTransform();
	SyncSceneSettings();
	GaussianScene->RegisterWithRenderer();

	UE_LOG(LogGaussianSimVerse, Log, TEXT("Registered streamed Gaussian scene '%s' at %s"),
		*GetName(),
		*GetActorLocation().ToString());
}

void AGaussianStreamedSceneActor::UpdateBoundsVisual()
{
	if (!BoundsVisual)
	{
		return;
	}

	// Prefer lod-meta scene bounds (dataset space; actor pivot is usually 0).
	FVector Origin = FVector::ZeroVector;
	FVector Extent(100.0f);
	if (StreamedSceneAsset && !StreamedSceneAsset->LodMeta.SceneBounds.Extent.IsNearlyZero())
	{
		Origin = FVector(StreamedSceneAsset->LodMeta.SceneBounds.Origin);
		Extent = FVector(StreamedSceneAsset->LodMeta.SceneBounds.Extent).ComponentMax(FVector(50.0f));
	}

	BoundsVisual->SetRelativeLocation(Origin);
	BoundsVisual->SetBoxExtent(Extent, true);
	BoundsVisual->SetHiddenInGame(true);
	BoundsVisual->SetVisibility(true);
	BoundsVisual->bDrawOnlyIfSelected = true;
	BoundsVisual->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	BoundsVisual->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);

	if (EditorSprite)
	{
		EditorSprite->SetRelativeLocation(Origin);
		EditorSprite->SetVisibility(false);
		EditorSprite->SetHiddenInGame(true);
	}
}

void AGaussianStreamedSceneActor::UnregisterScene()
{
	if (GaussianScene)
	{
		GaussianScene->UnregisterFromRenderer();
	}
}

FVector AGaussianStreamedSceneActor::GetStreamingViewOrigin() const
{
	FVector WorldView = GetActorLocation();
	if (const UWorld* World = GetWorld())
	{
		if (const APlayerController* PC = World->GetFirstPlayerController())
		{
			FVector ViewLocation;
			FRotator ViewRotation;
			PC->GetPlayerViewPoint(ViewLocation, ViewRotation);
			WorldView = ViewLocation;
		}
#if WITH_EDITOR
		else if (World->WorldType == EWorldType::Editor && GCurrentLevelEditingViewportClient)
		{
			WorldView = GCurrentLevelEditingViewportClient->GetViewLocation();
		}
#endif
	}

	// SOG / chunk placement use dataset coordinates with pivot 0 (same frame as splat means).
	// Convert world camera into actor/dataset space so LOD still works after moving the actor.
	return GetActorTransform().InverseTransformPosition(WorldView);
}

FVector AGaussianStreamedSceneActor::GetStreamingViewDirection() const
{
	FVector WorldDir = GetActorForwardVector();
	if (const UWorld* World = GetWorld())
	{
		if (const APlayerController* PC = World->GetFirstPlayerController())
		{
			FVector ViewLocation;
			FRotator ViewRotation;
			PC->GetPlayerViewPoint(ViewLocation, ViewRotation);
			WorldDir = ViewRotation.Vector();
		}
#if WITH_EDITOR
		else if (World->WorldType == EWorldType::Editor && GCurrentLevelEditingViewportClient)
		{
			WorldDir = GCurrentLevelEditingViewportClient->GetViewRotation().Vector();
		}
#endif
	}

	return GetActorTransform().InverseTransformVector(WorldDir).GetSafeNormal();
}

#if WITH_EDITOR
namespace
{
	// Keep editor streaming near frame-rate so LOD catch-up feels timely in the viewport.
	constexpr float EditorStreamingIdleIntervalSeconds = 0.016f;
	constexpr float EditorStreamingMotionIntervalSeconds = 0.0f;
}

void AGaussianStreamedSceneActor::EnsureEditorStreamingTickInterval()
{
	const float DesiredInterval = StreamingManager.IsBootstrapActive()
		? 0.0f
		: (StreamingManager.IsCameraInMotion() ? EditorStreamingMotionIntervalSeconds : EditorStreamingIdleIntervalSeconds);
	if (FMath::IsNearlyEqual(EditorStreamingTickInterval, DesiredInterval)
		&& EditorStreamingTickHandle.IsValid())
	{
		return;
	}

	if (EditorStreamingTickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(EditorStreamingTickHandle);
		EditorStreamingTickHandle.Reset();
	}

	EditorStreamingTickInterval = DesiredInterval;
	EditorStreamingTickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &AGaussianStreamedSceneActor::HandleEditorStreamingTick),
		EditorStreamingTickInterval);
}

void AGaussianStreamedSceneActor::HandleEditorCameraMoved(
	const FVector& Location,
	const FRotator& Rotation,
	ELevelViewportType ViewportType,
	int32 ViewIndex)
{
	const double NowSeconds = FPlatformTime::Seconds();
	const double MinInterval = StreamingManager.IsBootstrapActive()
		? 0.0
		: static_cast<double>(StreamingManager.IsCameraInMotion() ? EditorStreamingMotionIntervalSeconds : EditorStreamingIdleIntervalSeconds);
	if ((NowSeconds - LastEditorStreamingUpdateSeconds) < MinInterval)
	{
		return;
	}
	LastEditorStreamingUpdateSeconds = NowSeconds;

	if (!GaussianSimVerse::RenderSettings::IsStreamingEnabled()
		|| !StreamedSceneAsset
		|| !GaussianScene
		|| !ShouldRenderGaussian())
	{
		return;
	}

	StreamingManager.UpdateStreaming(Location, Rotation.Vector());
	EnsureEditorStreamingTickInterval();
}

bool AGaussianStreamedSceneActor::HandleEditorStreamingTick(float DeltaTime)
{
	if (!IsValid(this))
	{
		return false;
	}

	LastEditorStreamingUpdateSeconds = FPlatformTime::Seconds();
	UpdateStreamingFromView();
	EnsureEditorStreamingTickInterval();
	return true;
}
#endif
