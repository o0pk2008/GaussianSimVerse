// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianStreamedSceneActor.h"
#include "GaussianStreamedSceneAsset.h"
#include "GaussianScene.h"
#include "GaussianSimVerse.h"
#include "Rendering/GaussianRenderer.h"
#include "Rendering/GaussianRenderSettings.h"
#include "Components/BillboardComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Engine.h"
#include "GameFramework/PlayerController.h"
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
	// Property edits can remount components — do not force a full stream restart.
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
	// Property edits rerun construction scripts — never wipe resident streaming here.
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
	ShutdownStreaming();
	UnregisterScene();
	Super::EndPlay(EndPlayReason);
}

void AGaussianStreamedSceneActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UpdateStreamingFromView();
}

void AGaussianStreamedSceneActor::BeginDestroy()
{
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
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, bProxyEnableCollision))
	{
		ApplyProxyMeshSettings();
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, bStreamingDebugDraw)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, bStreamingDebugOverlay)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, DebugRenderMode))
	{
		// Debug Render is a per-frame CVar tint — never restart streaming or rebuild proxies.
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
	// the view on splat data, which is near the authoring origin — place the actor there too.
	const FVector TargetLocation = FVector::ZeroVector;
	if (!GetActorLocation().Equals(TargetLocation, 1.0f))
	{
		SetActorLocation(TargetLocation, false, nullptr, ETeleportType::TeleportPhysics);
	}
}

void AGaussianStreamedSceneActor::ApplyProxyMeshSettings()
{
	if (!ProxyMeshComponent)
	{
		return;
	}

	ProxyMeshComponent->SetStaticMesh(ProxyMesh);

	// Show / collision / depth independent. Scene Depth can look like a black mesh via SSAO
	// even when Show is off — see property tooltip; prefer Custom Depth for beauty pass.
	const bool bWantsSceneDepth = bProxyWriteSceneDepth;
	const bool bWantsCustomDepth = bProxyWriteCustomDepth;
	const bool bWantsAnyDepthPass = bWantsSceneDepth || bWantsCustomDepth;
	const bool bComponentActive = ProxyMesh != nullptr
		&& (bShowProxyMesh || bWantsAnyDepthPass || bProxyEnableCollision);

	ProxyMeshComponent->SetVisibility(bComponentActive);
	ProxyMeshComponent->SetHiddenInGame(!bComponentActive);
	ProxyMeshComponent->SetRenderInMainPass(bShowProxyMesh);
	ProxyMeshComponent->bRenderInDepthPass = bShowProxyMesh || bWantsSceneDepth;
	ProxyMeshComponent->SetRenderCustomDepth(bWantsCustomDepth);
	ProxyMeshComponent->SetCustomDepthStencilValue(1);
	ProxyMeshComponent->SetCastShadow(false);
	ProxyMeshComponent->bCastContactShadow = false;
	ProxyMeshComponent->bAffectDynamicIndirectLighting = bShowProxyMesh;
	ProxyMeshComponent->bAffectDistanceFieldLighting = false;
	ProxyMeshComponent->SetReceivesDecals(false);

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

	if (ProxyMesh && bComponentActive)
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
}

#if WITH_EDITOR
void AGaussianStreamedSceneActor::GenerateProxyMeshFromDataset()
{
	if (!StreamedSceneAsset)
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("Assign a Streamed Scene Asset first.")));
		return;
	}

	FGaussianProxyMeshBuildSettings Settings;
	Settings.VoxelSizeCm = FMath::Max(ProxyVoxelSizeCm, 1.0f);
	Settings.MinOpacity = ProxyMinOpacity;
	Settings.MaxSamplePoints = ProxyMaxSamplePoints;
	Settings.DilateRings = ProxyDilateRings;
	Settings.ShrinkRings = ProxyShrinkRings;
	Settings.MinHitsPerVoxel = ProxyMinHitsPerVoxel;
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
	UStaticMesh* Mesh = FGaussianProxyMeshGenerator::BuildMeshFromPoints(Points, Settings, AssetName, Error, &LocalOffset);
	if (!Mesh)
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error));
		return;
	}

	ProxyMesh = Mesh;
	ProxyMeshLocalOffset = LocalOffset;
	bShowProxyMesh = true;
	ApplyProxyMeshSettings();

	const FBoxSphereBounds MeshBounds = Mesh->GetBounds();
	FMessageDialog::Open(
		EAppMsgType::Ok,
		FText::FromString(FString::Printf(
			TEXT("Proxy mesh generated: %s\nPoints sampled: %d\nVoxel size: %.1f cm\nSample extent: %s\nMesh extent: %s\nLocal offset: %s\nTip: Scene Depth needs Show Proxy Mesh ON (Gaussians never write depth).\nCustom Depth is a different buffer (Write Custom Depth)."),
			*Mesh->GetPathName(),
			Points.Num(),
			Settings.VoxelSizeCm,
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
