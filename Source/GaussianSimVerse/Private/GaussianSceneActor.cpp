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
	EditorSprite->SetRelativeScale3D(FVector(2.0f));

	BoundsVisual = CreateDefaultSubobject<UBoxComponent>(TEXT("BoundsVisual"));
	BoundsVisual->SetupAttachment(SceneRoot);
	BoundsVisual->SetCollisionEnabled(ECollisionEnabled::NoCollision);
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
	TryRegisterScene();
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
	TryRegisterScene();
}

void AGaussianSceneActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	if (GaussianScene && GaussianAsset && GaussianAsset->IsValidForRendering())
	{
		GaussianScene->WorldTransform = GetActorTransform();
		UpdateBoundsVisual();

		if (GaussianScene->IsRegisteredWithRenderer())
		{
			FGaussianRenderer::Get().MarkSceneDirty(GaussianScene);
		}
		else
		{
			TryRegisterScene();
		}
		return;
	}

	RebuildGaussianScene();
	TryRegisterScene();
}

void AGaussianSceneActor::BeginPlay()
{
	Super::BeginPlay();
	RebuildGaussianScene();
	RegisterScene();
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

#if WITH_EDITOR
void AGaussianSceneActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropertyName = PropertyChangedEvent.GetPropertyName();
	if (PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianSceneActor, GaussianAsset))
	{
		SnapActorToAssetOrigin();
		RebuildGaussianScene();
		TryRegisterScene();
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianSceneActor, bEnableRendering))
	{
		RebuildGaussianScene();
		TryRegisterScene();
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
	GaussianScene->bEnableRendering = bEnableRendering;
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
	if (!GetWorld() || !GaussianScene || GaussianScene->Chunks.Num() == 0 || !bEnableRendering)
	{
		return;
	}

	RegisterScene();
}

void AGaussianSceneActor::RegisterScene()
{
	if (!GaussianScene || GaussianScene->Chunks.Num() == 0 || !bEnableRendering || !GaussianAsset)
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
		BoundsVisual->SetVisibility(true);
	}
	else
	{
		BoundsVisual->SetVisibility(false);
	}
}

void AGaussianSceneActor::UnregisterScene()
{
	if (GaussianScene)
	{
		GaussianScene->UnregisterFromRenderer();
	}
}
