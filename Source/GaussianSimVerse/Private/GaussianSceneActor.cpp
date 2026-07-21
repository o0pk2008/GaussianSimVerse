// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSceneActor.h"
#include "GaussianAsset.h"
#include "GaussianChunk.h"
#include "GaussianScene.h"
#include "GaussianSimVerse.h"
#include "Rendering/GaussianRenderer.h"
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
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"
#if WITH_EDITOR
#include "Editor/GaussianProxyMeshGenerator.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#endif

AGaussianSceneActor::AGaussianSceneActor()
{
	// Tick only needed when relighting is enabled (enabled in UpdateRelighting).
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;

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
	GaussianScene->DofMode = ProxyMesh ? ProxyDofMode : EGaussianProxyDofMode::Off;
	GaussianScene->bUseProxyDepthOfField = (GaussianScene->DofMode != EGaussianProxyDofMode::Off);
	GaussianScene->DofFocalDistanceCm = ProxyDofFocalDistanceCm;
	GaussianScene->DofCocScale = ProxyDofCocScale;
	GaussianScene->DofMaxBlurRadiusPx = ProxyDofMaxBlurRadiusPx;
	GaussianScene->DofProxyStencil = static_cast<uint32>(FMath::Clamp(ProxyCustomDepthStencilValue, 0, 255));
}

void AGaussianSceneActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	// Property edits (incl. relight Blend slider) re-run construction scripts. Never tear down /
	// re-InitGPU multi-million splat buffers here — only soft-sync transform + settings.
	if (GaussianScene && GaussianAsset && GaussianAsset->IsValidForRendering())
	{
		GaussianScene->WorldTransform = GetActorTransform();
		SyncSceneSettings();
		UpdateBoundsVisual();

		if (GaussianScene->IsRegisteredWithRenderer())
		{
			FGaussianRenderer::Get().MarkSceneDirty(GaussianScene, /*bFlushImmediately=*/false);
		}
		else if (GaussianScene->Chunks.Num() > 0)
		{
			RefreshRenderRegistration();
		}
		else
		{
			// Scene object exists but empty (first construct) — build chunks without Unregister churn.
			RebuildGaussianScene();
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
	if (bProxyDofMitigationHeld)
	{
		GaussianProxyDofMitigation::SetActive(false);
		bProxyDofMitigationHeld = false;
	}
	bEnableRelighting = false;
	UpdateRelighting();
	UnregisterScene();
	Super::EndPlay(EndPlayReason);
}

void AGaussianSceneActor::BeginDestroy()
{
	if (bProxyDofMitigationHeld)
	{
		GaussianProxyDofMitigation::SetActive(false);
		bProxyDofMitigationHeld = false;
	}
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

	// Relight sliders: capture params only — never Rebuild/ApplyProxy/MarkSceneDirty (crash path).
	const bool bRelightParamOnly =
		PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianSceneActor, RelightBlend)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianSceneActor, RelightExposure)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianSceneActor, RelightBrightness)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianSceneActor, RelightBackground)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianSceneActor, RelightTextureScale)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianSceneActor, bRelightDebug)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianSceneActor, RelightEnvExposure)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianSceneActor, RelightEnvRotation);
	if (bRelightParamOnly)
	{
		UpdateRelighting();
		return;
	}

	if (PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianSceneActor, bEnableRelighting)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianSceneActor, RelightSkydome))
	{
		// Enable / skydome may need proxy capture + material; still no full GPU re-init.
		if (bEnableRelighting)
		{
			ApplyProxyMeshSettings(); // ends with UpdateRelighting
		}
		else
		{
			UpdateRelighting();
		}
		return;
	}

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
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianSceneActor, bProxyEnableCollision)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianSceneActor, ProxyDofMode)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianSceneActor, bProxyDofSuppressScreenSpaceAO)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianSceneActor, ProxyCustomDepthStencilValue))
	{
		if (PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianSceneActor, ProxyDofMode)
			&& ProxyDofMode != EGaussianProxyDofMode::Off)
		{
			bProxyWriteCustomDepth = true;
			bProxyWriteSceneDepth = false; // never early SceneDepth for DOF beauty
		}
		ApplyProxyMeshSettings();
		SyncDepthOfFieldToScene();
		SyncSceneSettings();
		if (GaussianScene && GaussianScene->IsRegisteredWithRenderer())
		{
			FGaussianRenderer::Get().MarkSceneDirty(GaussianScene);
		}
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianSceneActor, ProxyDofFocalDistanceCm)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianSceneActor, ProxyDofCocScale)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AGaussianSceneActor, ProxyDofMaxBlurRadiusPx))
	{
		SyncSceneSettings();
		if (GaussianScene && GaussianScene->IsRegisteredWithRenderer())
		{
			FGaussianRenderer::Get().MarkSceneDirty(GaussianScene);
		}
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

	// Dataset-coordinate assets match streamed LOD: actor at world origin, splats absolute.
	// Legacy centered assets: actor at Bounds.Origin (positions stored relative to that point).
	const FVector TargetLocation = GaussianAsset->bUsesDatasetCoordinates
		? FVector::ZeroVector
		: FVector(GaussianAsset->Bounds.Origin);
	if (!GetActorLocation().Equals(TargetLocation, 1.0f))
	{
		SetActorLocation(TargetLocation, false, nullptr, ETeleportType::TeleportPhysics);
	}
}

void AGaussianSceneActor::SyncDepthOfFieldToScene()
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

void AGaussianSceneActor::ApplyProxyMeshSettings()
{
	if (!ProxyMeshComponent)
	{
		return;
	}

	// Plugin DOF uses Custom Depth only. NEVER enable early Write Scene Depth for DOF
	// (that blocks the skydome �?black voxel grid + engine sky warning).
	if (ProxyDofMode != EGaussianProxyDofMode::Off)
	{
		bProxyWriteCustomDepth = true;
		bProxyWriteSceneDepth = false; // forced off for beauty DOF
	}

	ProxyMeshComponent->SetStaticMesh(ProxyMesh);

	const bool bWantsCustomDepth = bProxyWriteCustomDepth || (ProxyDofMode != EGaussianProxyDofMode::Off);
	// Scene Depth early write: sensors only — never for DOF beauty.
	const bool bEarlySceneDepth = bProxyWriteSceneDepth && ProxyDofMode == EGaussianProxyDofMode::Off;
	const bool bWantsAnyDepthPass = bEarlySceneDepth || bWantsCustomDepth;
	const bool bComponentActive = ProxyMesh != nullptr
		&& (bShowProxyMesh || bWantsAnyDepthPass || bProxyEnableCollision
			|| (ProxyDofMode != EGaussianProxyDofMode::Off) || bEnableRelighting);

	ProxyMeshComponent->SetVisibility(bComponentActive);
	ProxyMeshComponent->SetHiddenInGame(!bComponentActive);

	ProxyMeshComponent->SetRenderCustomDepth(bWantsCustomDepth);
	// Stencil 0 is reserved "no proxy"; keep [1,255] to avoid clearing exclude mask.
	ProxyMeshComponent->SetCustomDepthStencilValue(FMath::Clamp(ProxyCustomDepthStencilValue, 1, 255));
	// Depth-only path: no lit main pass; no early SceneDepth when Proxy DOF is on.
	// When relighting, keep lighting channels / base pass usable for SceneCapture.
	GaussianProxyDofMitigation::ConfigureDepthOnlyComponent(
		ProxyMeshComponent,
		bShowProxyMesh || bEnableRelighting,
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

	// Opaque material for depth / custom depth. Relighting installs its own lit MID afterward.
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
					// Keep a stable opaque material for depth-only draws.
					ProxyMeshComponent->SetMaterial(Index, DepthMat);
				}
			}
		}
	}

	ProxyMeshComponent->MarkRenderStateDirty();
	SyncDepthOfFieldToScene();
	UpdateRelighting();
}

void AGaussianSceneActor::UpdateRelighting()
{
	const bool bWant = bEnableRelighting && ProxyMesh != nullptr && ProxyMeshComponent != nullptr;
	PrimaryActorTick.SetTickFunctionEnable(bWant);

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
		FGaussianRenderer::Get().SetRelightFrameState_GameThread(FGaussianRelightFrameState{});
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

	// Invisible mesh in the main view → sun shadows on the ground / other actors.
	GaussianRelighting::ConfigureMainViewShadowCaster(
		RelightShadowProxy,
		ProxyMesh,
		ProxyMeshLocalOffset,
		true);

	// Full-scene lighting capture: box/walls occlude light and cast onto the gray proxy.
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

void AGaussianSceneActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bEnableRelighting)
	{
		UpdateRelighting();
	}
}

bool AGaussianSceneActor::ShouldTickIfViewportsOnly() const
{
	return bEnableRelighting;
}

#if WITH_EDITOR
void AGaussianSceneActor::SetupDepthOfField()
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
	bProxyWriteSceneDepth = false; // critical: never early SceneDepth for DOF
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

void AGaussianSceneActor::RebuildGaussianScene()
{
	// Only unregister when tearing down rendering for a real asset/content change.
	// Avoid Unregister+Register+InitGPU on every construction-script pass (relight UI).
	const bool bWasRegistered = GaussianScene && GaussianScene->IsRegisteredWithRenderer();
	if (bWasRegistered)
	{
		UnregisterScene();
	}

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

	// Absolute dataset coords: box around Bounds.Origin. Legacy centered: box around actor (0).
	FVector Extent(50.0f);
	FVector BoxLocalOrigin = FVector::ZeroVector;
	if (GaussianAsset && !GaussianAsset->Bounds.Extent.IsNearlyZero())
	{
		Extent = FVector(GaussianAsset->Bounds.Extent).ComponentMax(FVector(10.0f));
		if (GaussianAsset->bUsesDatasetCoordinates)
		{
			BoxLocalOrigin = FVector(GaussianAsset->Bounds.Origin);
		}
	}

	BoundsVisual->SetRelativeLocation(BoxLocalOrigin);
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
