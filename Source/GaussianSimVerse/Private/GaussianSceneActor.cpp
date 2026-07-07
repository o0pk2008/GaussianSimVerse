// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSceneActor.h"
#include "GaussianAsset.h"
#include "GaussianChunk.h"
#include "GaussianScene.h"
#include "GaussianSimVerse.h"
#include "Rendering/GaussianRenderer.h"
#include "Components/BillboardComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "UObject/ConstructorHelpers.h"

AGaussianSceneActor::AGaussianSceneActor()
{
	PrimaryActorTick.bCanEverTick = false;

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

void AGaussianSceneActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	RebuildGaussianScene();
	RefreshRenderRegistration();
}

void AGaussianSceneActor::PostUnregisterAllComponents()
{
	UnregisterScene();
	Super::PostUnregisterAllComponents();
}

void AGaussianSceneActor::SetGaussianAsset(UGaussianAsset* InAsset)
{
	GaussianAsset = InAsset;
	SnapActorToAssetOrigin();
	RebuildGaussianScene();
	RefreshRenderRegistration();
}

void AGaussianSceneActor::SyncSceneSettings()
{
	if (!GaussianScene)
	{
		return;
	}

	GaussianScene->ShBand = ShBandOverride;
	GaussianScene->Colors = Colors;
}

void AGaussianSceneActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	if (GaussianScene && GaussianAsset && GaussianAsset->IsValidForRendering())
	{
		GaussianScene->WorldTransform = GetActorTransform();
		SyncSceneSettings();
		UpdateBoundsVisual();

		if (GaussianScene->IsRegisteredWithRenderer())
		{
			FGaussianRenderer::Get().MarkSceneDirty(GaussianScene);
		}
		else
		{
			RefreshRenderRegistration();
		}
		return;
	}

	RebuildGaussianScene();
	RefreshRenderRegistration();
}

void AGaussianSceneActor::BeginPlay()
{
	Super::BeginPlay();
	RebuildGaussianScene();
	RefreshRenderRegistration();
}

void AGaussianSceneActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnregisterScene();
	Super::EndPlay(EndPlayReason);
}

void AGaussianSceneActor::BeginDestroy()
{
	UnregisterScene();
	Super::BeginDestroy();
}

void AGaussianSceneActor::SetActorHiddenInGame(bool bNewHidden)
{
	const bool bWasHidden = IsHidden();
	Super::SetActorHiddenInGame(bNewHidden);
	if (bWasHidden != IsHidden())
	{
		RefreshRenderRegistration();
	}
}

#if WITH_EDITOR
void AGaussianSceneActor::SetIsTemporarilyHiddenInEditor(bool bIsHidden)
{
	const bool bWasHidden = IsTemporarilyHiddenInEditor();
	Super::SetIsTemporarilyHiddenInEditor(bIsHidden);
	if (bWasHidden != IsTemporarilyHiddenInEditor())
	{
		RefreshRenderRegistration();
	}
}

void AGaussianSceneActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropertyName = PropertyChangedEvent.GetPropertyName();
	if (PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianSceneActor, GaussianAsset))
	{
		SnapActorToAssetOrigin();
		RebuildGaussianScene();
		RefreshRenderRegistration();
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianSceneActor, bEnableRendering))
	{
		RebuildGaussianScene();
		RefreshRenderRegistration();
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianSceneActor, ShBandOverride)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianSceneActor, Colors)
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

void AGaussianSceneActor::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);

	if (!bFinished || !GaussianScene || !bEnableRendering)
	{
		return;
	}

	GaussianScene->WorldTransform = GetActorTransform();
	FGaussianRenderer::Get().MarkSceneDirty(GaussianScene);
}

void AGaussianSceneActor::Destroyed()
{
	UnregisterScene();
	Super::Destroyed();
}
#endif

bool AGaussianSceneActor::ShouldRenderGaussian() const
{
	if (!bEnableRendering || !GaussianAsset || !GaussianAsset->IsValidForRendering())
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

void AGaussianSceneActor::RefreshRenderRegistration()
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

void AGaussianSceneActor::SnapActorToAssetOrigin()
{
	if (!GaussianAsset || !GaussianAsset->IsValidForRendering())
	{
		return;
	}

	const FVector TargetLocation = FVector(GaussianAsset->Bounds.Origin);
	if (!GetActorLocation().Equals(TargetLocation, 1.0f))
	{
		SetActorLocation(TargetLocation, false, nullptr, ETeleportType::TeleportPhysics);
	}
}

void AGaussianSceneActor::RebuildGaussianScene()
{
	UnregisterScene();

	if (!GaussianScene)
	{
		GaussianScene = NewObject<UGaussianScene>(this, TEXT("GaussianScene"));
	}

	if (!GaussianChunk)
	{
		GaussianChunk = NewObject<UGaussianChunk>(GaussianScene, TEXT("GaussianChunk"));
	}

	GaussianScene->WorldTransform = GetActorTransform();
	GaussianScene->bEnableRendering = ShouldRenderGaussian();
	SyncSceneSettings();
	GaussianScene->Chunks.Reset();

	if (GaussianAsset && GaussianAsset->IsValidForRendering())
	{
		GaussianChunk->Asset = GaussianAsset;
		FGaussianBounds LocalBounds;
		LocalBounds.Origin = FVector3f::ZeroVector;
		LocalBounds.Extent = GaussianAsset->Bounds.Extent;
		GaussianChunk->LocalBounds = LocalBounds;
		GaussianScene->Chunks.Add(GaussianChunk);
	}

	UpdateBoundsVisual();
}

void AGaussianSceneActor::TryRegisterScene()
{
	if (!GetWorld() || !GaussianScene || GaussianScene->Chunks.Num() == 0 || !ShouldRenderGaussian())
	{
		return;
	}

	RegisterScene();
}

void AGaussianSceneActor::RegisterScene()
{
	if (!GaussianScene || GaussianScene->Chunks.Num() == 0 || !ShouldRenderGaussian() || !GaussianAsset)
	{
		return;
	}

	UE_LOG(LogGaussianSimVerse, Log, TEXT("Registering Gaussian scene actor '%s' at %s (%d splats, extent %s)"),
		*GetName(),
		*GetActorLocation().ToString(),
		GaussianAsset->GaussianCount,
		*FVector(GaussianAsset->Bounds.Extent).ToString());

	GaussianAsset->InitGPUResources();
	GaussianScene->WorldTransform = GetActorTransform();
	SyncSceneSettings();
	GaussianScene->RegisterWithRenderer();
}

void AGaussianSceneActor::UpdateBoundsVisual()
{
	if (!BoundsVisual)
	{
		return;
	}

	if (GaussianAsset && GaussianAsset->IsValidForRendering())
	{
		const FVector Extent = FVector(GaussianAsset->Bounds.Extent);
		BoundsVisual->SetBoxExtent(Extent, false);
	}

	BoundsVisual->SetVisibility(false);
}

void AGaussianSceneActor::UnregisterScene()
{
	if (GaussianScene)
	{
		GaussianScene->UnregisterFromRenderer();
	}
}
