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
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"
#if WITH_EDITOR
#include "Editor/GaussianProxyMeshGenerator.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#endif

AGaussianSceneActor::AGaussianSceneActor()
{
	PrimaryActorTick.bCanEverTick = false;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	// Editor pick target: Gaussians are post-process and have no standard hit proxies.
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
	// Wireframe only when the actor is selected; collision stays on for click-picking.
	BoundsVisual->bDrawOnlyIfSelected = true;
	BoundsVisual->SetBoxExtent(FVector(50.0f), false);

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
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianSceneActor, ProxyMesh)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianSceneActor, bShowProxyMesh)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianSceneActor, bProxyWriteCustomDepth)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianSceneActor, bProxyWriteSceneDepth)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianSceneActor, bProxyEnableCollision))
	{
		ApplyProxyMeshSettings();
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

void AGaussianSceneActor::ApplyProxyMeshSettings()
{
	if (!ProxyMeshComponent)
	{
		return;
	}

	ProxyMeshComponent->SetStaticMesh(ProxyMesh);

	// Show / collision / depth are independent.
	// NOTE: Write Scene Depth injects a solid depth volume; SSAO/contact shadows then darken
	// that volume so it can look like a "black mesh" even with Show off. Prefer Custom Depth
	// for beauty-pass effects; enable Scene Depth for sensors / true scene occlusion.
	const bool bWantsSceneDepth = bProxyWriteSceneDepth;
	const bool bWantsCustomDepth = bProxyWriteCustomDepth;
	const bool bWantsAnyDepthPass = bWantsSceneDepth || bWantsCustomDepth;
	const bool bComponentActive = ProxyMesh != nullptr
		&& (bShowProxyMesh || bWantsAnyDepthPass || bProxyEnableCollision);

	ProxyMeshComponent->SetVisibility(bComponentActive);
	ProxyMeshComponent->SetHiddenInGame(!bComponentActive);

	// Color only when Show is on. Depth/custom depth can run without a lit color pass.
	ProxyMeshComponent->SetRenderInMainPass(bShowProxyMesh);
	ProxyMeshComponent->bRenderInDepthPass = bShowProxyMesh || bWantsSceneDepth;
	ProxyMeshComponent->SetRenderCustomDepth(bWantsCustomDepth);
	ProxyMeshComponent->SetCustomDepthStencilValue(1);
	ProxyMeshComponent->SetCastShadow(false);
	ProxyMeshComponent->bCastContactShadow = false;
	ProxyMeshComponent->bAffectDynamicIndirectLighting = bShowProxyMesh;
	ProxyMeshComponent->bAffectDistanceFieldLighting = false;
	ProxyMeshComponent->SetReceivesDecals(false);
	ProxyMeshComponent->SetCullDistance(0.0f);
	ProxyMeshComponent->SetBoundsScale(1.0f);

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

	// Opaque material needed for depth writes. When not shown, still assign so depth passes work.
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
					// Keep a stable opaque material for depth-only draws.
					ProxyMeshComponent->SetMaterial(Index, DepthMat);
				}
			}
		}
	}

	ProxyMeshComponent->MarkRenderStateDirty();
}

#if WITH_EDITOR
void AGaussianSceneActor::GenerateProxyMeshFromAsset()
{
	if (!GaussianAsset)
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("Assign a Gaussian Asset first.")));
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
	Settings.MinOpacity = ProxyMinOpacity;
	Settings.MaxSamplePoints = ProxyMaxSamplePoints;
	Settings.DilateRings = ProxyDilateRings;
	Settings.ShrinkRings = ProxyShrinkRings;
	Settings.MinHitsPerVoxel = ProxyMinHitsPerVoxel;
	// Keep Gaussian actor-local space so asset preview matches the splat cloud frame.
	Settings.bCenterMeshAtOrigin = false;
	// Save next to the Gaussian asset package for easier content management.
	if (const UPackage* AssetPackage = GaussianAsset->GetOutermost())
	{
		Settings.PackagePath = FPackageName::GetLongPackagePath(AssetPackage->GetName());
	}

	TArray<FVector> Points;
	FString Error;
	if (!FGaussianProxyMeshGenerator::CollectPointsFromAsset(GaussianAsset, Settings, Points, Error))
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error));
		return;
	}

	// Non-streamed splat positions are relative to Bounds.Origin; rendering uses the same
	// actor-local frame (actor snapped to Bounds.Origin). Keep points as-is; mesh may recenter.
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

	const FString AssetName = FString::Printf(TEXT("%s_Proxy"), *GaussianAsset->GetName());
	FVector LocalOffset = FVector::ZeroVector;
	float ActualVoxelCm = Settings.VoxelSizeCm;
	UStaticMesh* Mesh = FGaussianProxyMeshGenerator::BuildMeshFromPoints(
		Points, Settings, AssetName, Error, &LocalOffset, &ActualVoxelCm);
	if (!Mesh)
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error));
		return;
	}

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
		? FString::Printf(TEXT("\n(Auto-raised Voxel Size %.1f → %.1f cm so the grid fits this large scene.)"),
			Settings.VoxelSizeCm, ActualVoxelCm)
		: FString();
	FMessageDialog::Open(
		EAppMsgType::Ok,
		FText::FromString(FString::Printf(
			TEXT("Proxy mesh generated: %s\nPoints sampled: %d\nVoxel size used: %.1f cm%s\nSample extent: %s\nMesh extent: %s\nLocal offset: %s\nTip: Show=off + Custom Depth for beauty; Scene Depth can darken Lit."),
			*Mesh->GetPathName(),
			Points.Num(),
			ActualVoxelCm,
			*AutoGrowNote,
			*SampleBounds.GetExtent().ToCompactString(),
			*FVector(MeshBounds.BoxExtent).ToCompactString(),
			*ProxyMeshLocalOffset.ToCompactString())));
}
#endif

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
		GaussianChunk->LoadState = EGaussianChunkLoadState::Loaded;
		GaussianScene->Chunks.Add(GaussianChunk);
	}
	else if (GaussianChunk)
	{
		GaussianChunk->LoadState = EGaussianChunkLoadState::Unloaded;
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

	// Non-streamed splats are centered at actor (Bounds.Origin); local AABB is about origin.
	FVector Extent(50.0f);
	if (GaussianAsset && !GaussianAsset->Bounds.Extent.IsNearlyZero())
	{
		Extent = FVector(GaussianAsset->Bounds.Extent);
		// Tiny assets still need a pickable volume.
		Extent = Extent.ComponentMax(FVector(10.0f));
	}

	BoundsVisual->SetRelativeLocation(FVector::ZeroVector);
	BoundsVisual->SetBoxExtent(Extent, true);
	BoundsVisual->SetHiddenInGame(true);
	BoundsVisual->SetVisibility(true);
	BoundsVisual->bDrawOnlyIfSelected = true;
	BoundsVisual->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	BoundsVisual->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);

	if (EditorSprite)
	{
		EditorSprite->SetVisibility(false);
		EditorSprite->SetHiddenInGame(true);
	}
}

void AGaussianSceneActor::UnregisterScene()
{
	if (GaussianScene)
	{
		GaussianScene->UnregisterFromRenderer();
	}
}
