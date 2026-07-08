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
#include "Engine/Engine.h"
#include "GameFramework/PlayerController.h"
#include "UObject/ConstructorHelpers.h"

#if WITH_EDITOR
#include "Editor.h"
#include "LevelEditorViewport.h"
#endif

AGaussianStreamedSceneActor::AGaussianStreamedSceneActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	EditorSprite = CreateDefaultSubobject<UBillboardComponent>(TEXT("EditorSprite"));
	EditorSprite->SetupAttachment(SceneRoot);
	EditorSprite->SetHiddenInGame(true);
	EditorSprite->SetVisibility(false);

	BoundsVisual = CreateDefaultSubobject<UBoxComponent>(TEXT("BoundsVisual"));
	BoundsVisual->SetupAttachment(SceneRoot);
	BoundsVisual->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	BoundsVisual->SetCollisionResponseToAllChannels(ECR_Ignore);
	BoundsVisual->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
	BoundsVisual->SetGenerateOverlapEvents(false);
	BoundsVisual->SetHiddenInGame(true);
	BoundsVisual->ShapeColor = FColor(80, 200, 255);
	BoundsVisual->SetLineThickness(2.0f);
	BoundsVisual->bDrawOnlyIfSelected = false;

	static ConstructorHelpers::FObjectFinder<UTexture2D> SpriteFinder(TEXT("/Engine/EditorResources/S_Note"));
	if (SpriteFinder.Succeeded())
	{
		EditorSprite->SetSprite(SpriteFinder.Object);
	}
}

void AGaussianStreamedSceneActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	InitializeStreaming();
	RefreshRenderRegistration();

#if WITH_EDITOR
	if (!EditorCameraMovedHandle.IsValid())
	{
		EditorCameraMovedHandle = FEditorDelegates::OnEditorCameraMoved.AddUObject(this, &AGaussianStreamedSceneActor::HandleEditorCameraMoved);
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
#endif

	ShutdownStreaming();
	UnregisterScene();
	Super::PostUnregisterAllComponents();
}

void AGaussianStreamedSceneActor::SetStreamedSceneAsset(UGaussianStreamedSceneAsset* InAsset)
{
	StreamedSceneAsset = InAsset;
	SnapActorToSceneOrigin();
	InitializeStreaming();
	RefreshRenderRegistration();
}

void AGaussianStreamedSceneActor::NotifyStreamingChunkLoaded()
{
	TryRegisterScene();
	RefreshRenderRegistration();
}

void AGaussianStreamedSceneActor::DrawStreamingDebugOverlay(const FGaussianStreamingManager& Manager) const
{
	if (!GEngine)
	{
		return;
	}

	const FString Message = FString::Printf(
		TEXT("Gaussian Streaming | LoadedChunks=%d Desired=%d Pending=%d Splats=%d"),
		Manager.GetLoadedChunkCount(),
		Manager.GetDesiredChunkCount(),
		Manager.GetPendingLoadCount(),
		Manager.GetLoadedSplatCount());

	GEngine->AddOnScreenDebugMessage(INDEX_NONE, 0.0f, FColor::Cyan, Message);
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
	GaussianSimVerse::RenderSettings::CVarStreamingDebugDraw->Set(bStreamingDebugDraw ? 1 : 0, ECVF_SetByCode);
	GaussianSimVerse::RenderSettings::CVarStreamingDebugOverlay->Set(bStreamingDebugOverlay ? 1 : 0, ECVF_SetByCode);
	GaussianSimVerse::RenderSettings::CVarStreamingDebugRenderMode->Set(static_cast<int32>(DebugRenderMode), ECVF_SetByCode);
	GaussianSimVerse::RenderSettings::CVarDebugOverlay->Set(DebugRenderMode == EGaussianStreamingDebugRenderMode::LOD ? 1 : 0, ECVF_SetByCode);
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
	InitializeStreaming();
	RefreshRenderRegistration();
}

void AGaussianStreamedSceneActor::BeginPlay()
{
	Super::BeginPlay();
	ApplyStreamingCVarOverrides();
	InitializeStreaming();
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

	if (!GaussianSimVerse::RenderSettings::IsStreamingEnabled()
		|| !StreamedSceneAsset
		|| !GaussianScene
		|| !ShouldRenderGaussian())
	{
		return;
	}

	ApplyStreamingCVarOverrides();
	StreamingManager.UpdateStreaming(GetStreamingViewOrigin());
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
		InitializeStreaming();
		RefreshRenderRegistration();
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, bEnableRendering))
	{
		RefreshRenderRegistration();
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, bApplyStreamingCVarOverrides)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, bStreamingEnable)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, StreamingLoadRadius)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, StreamingLodBaseDistance)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, StreamingMaxLoadedSplats)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, StreamingMaxLoadsPerFrame)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, bStreamingDebugDraw)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, bStreamingDebugOverlay)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, DebugRenderMode))
	{
		ApplyStreamingCVarOverrides();
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

	if (!bFinished || !GaussianScene || !bEnableRendering)
	{
		return;
	}

	GaussianScene->WorldTransform = GetActorTransform();
	FGaussianRenderer::Get().MarkSceneDirty(GaussianScene);
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

void AGaussianStreamedSceneActor::InitializeStreaming()
{
	if (!GaussianScene)
	{
		GaussianScene = NewObject<UGaussianScene>(this, TEXT("GaussianScene"));
	}

	if (!StreamedSceneAsset)
	{
		StreamingManager.Shutdown();
		return;
	}

	GaussianScene->WorldTransform = GetActorTransform();
	SyncSceneSettings();
	StreamingManager.Initialize(this, StreamedSceneAsset, GaussianScene);
	UpdateBoundsVisual();
}

void AGaussianStreamedSceneActor::ShutdownStreaming()
{
	StreamingManager.Shutdown();
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

	const FVector TargetLocation = FVector(StreamedSceneAsset->LodMeta.SceneBounds.Origin);
	if (!GetActorLocation().Equals(TargetLocation, 1.0f))
	{
		SetActorLocation(TargetLocation, false, nullptr, ETeleportType::TeleportPhysics);
	}
}

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

	if (StreamedSceneAsset)
	{
		const FVector Extent = FVector(StreamedSceneAsset->LodMeta.SceneBounds.Extent);
		BoundsVisual->SetBoxExtent(Extent, false);
	}

	BoundsVisual->SetVisibility(false);
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
	if (const UWorld* World = GetWorld())
	{
		if (const APlayerController* PC = World->GetFirstPlayerController())
		{
			FVector ViewLocation;
			FRotator ViewRotation;
			PC->GetPlayerViewPoint(ViewLocation, ViewRotation);
			return ViewLocation;
		}

#if WITH_EDITOR
		if (World->WorldType == EWorldType::Editor && GCurrentLevelEditingViewportClient)
		{
			return GCurrentLevelEditingViewportClient->GetViewLocation();
		}
#endif
	}

	return GetActorLocation();
}

#if WITH_EDITOR
void AGaussianStreamedSceneActor::HandleEditorCameraMoved(
	const FVector& Location,
	const FRotator& Rotation,
	ELevelViewportType ViewportType,
	int32 ViewIndex)
{
	if (!GaussianSimVerse::RenderSettings::IsStreamingEnabled()
		|| !StreamedSceneAsset
		|| !GaussianScene
		|| !ShouldRenderGaussian())
	{
		return;
	}

	StreamingManager.UpdateStreaming(Location);
}
#endif
