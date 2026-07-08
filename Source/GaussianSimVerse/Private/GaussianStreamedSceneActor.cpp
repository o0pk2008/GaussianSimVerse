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
		TEXT("Gaussian Streaming | LoadedChunks=%d Desired=%d Pending=%d Splats=%d"),
		Manager.GetLoadedChunkCount(),
		Manager.GetDesiredChunkCount(),
		Manager.GetPendingLoadCount(),
		Manager.GetLoadedSplatCount());

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
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianStreamedSceneActor, StreamingMaxLoadsPerFrame))
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

FVector AGaussianStreamedSceneActor::GetStreamingViewDirection() const
{
	if (const UWorld* World = GetWorld())
	{
		if (const APlayerController* PC = World->GetFirstPlayerController())
		{
			FVector ViewLocation;
			FRotator ViewRotation;
			PC->GetPlayerViewPoint(ViewLocation, ViewRotation);
			return ViewRotation.Vector();
		}

#if WITH_EDITOR
		if (World->WorldType == EWorldType::Editor && GCurrentLevelEditingViewportClient)
		{
			return GCurrentLevelEditingViewportClient->GetViewRotation().Vector();
		}
#endif
	}

	return GetActorForwardVector();
}

#if WITH_EDITOR
namespace
{
	constexpr float EditorStreamingIdleIntervalSeconds = 0.05f;
	constexpr float EditorStreamingMotionIntervalSeconds = 0.016f;
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
