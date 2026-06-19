// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianAssetTypeActions.h"
#include "GaussianAsset.h"
#include "GaussianSceneActor.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "LevelEditorViewport.h"
#include "Subsystems/EditorActorSubsystem.h"

#define LOCTEXT_NAMESPACE "GaussianAssetTypeActions"

FText FAssetTypeActions_GaussianAsset::GetName() const
{
	return LOCTEXT("AssetName", "Gaussian Splat");
}

FColor FAssetTypeActions_GaussianAsset::GetTypeColor() const
{
	return FColor(120, 180, 255);
}

UClass* FAssetTypeActions_GaussianAsset::GetSupportedClass() const
{
	return UGaussianAsset::StaticClass();
}

uint32 FAssetTypeActions_GaussianAsset::GetCategories()
{
	return EAssetTypeCategories::Misc;
}

void FAssetTypeActions_GaussianAsset::GetActions(const TArray<UObject*>& InObjects, FMenuBuilder& MenuBuilder)
{
	const TArray<TWeakObjectPtr<UGaussianAsset>> Assets = GetTypedWeakObjectPtrs<UGaussianAsset>(InObjects);

	MenuBuilder.AddMenuEntry(
		LOCTEXT("PlaceInLevel", "Place Gaussian Scene in Level"),
		LOCTEXT("PlaceInLevelTooltip", "Spawn a GaussianSceneActor in the current level and assign this asset."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.NewLevel"),
		FUIAction(
			FExecuteAction::CreateLambda([Assets]()
			{
				FAssetTypeActions_GaussianAsset::PlaceActorsInLevel(Assets);
			}),
			FCanExecuteAction::CreateLambda([Assets]()
			{
				return Assets.Num() > 0 && GEditor != nullptr;
			})));

	FAssetTypeActions_Base::GetActions(InObjects, MenuBuilder);
}

void FAssetTypeActions_GaussianAsset::PlaceActorsInLevel(const TArray<TWeakObjectPtr<UGaussianAsset>>& Assets)
{
	if (!GEditor)
	{
		return;
	}

	UEditorActorSubsystem* const ActorSubsystem = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
	if (!ActorSubsystem)
	{
		return;
	}

	int32 OffsetIndex = 0;

	for (const TWeakObjectPtr<UGaussianAsset>& WeakAsset : Assets)
	{
		UGaussianAsset* Asset = WeakAsset.Get();
		if (!Asset || !Asset->IsValidForRendering())
		{
			continue;
		}

		const FVector SpawnLocation = FVector(Asset->Bounds.Origin) + FVector(OffsetIndex * 200.0, 0.0, 0.0);
		++OffsetIndex;

		AActor* SpawnedActor = ActorSubsystem->SpawnActorFromClass(
			AGaussianSceneActor::StaticClass(),
			SpawnLocation,
			FRotator::ZeroRotator);

		AGaussianSceneActor* GaussianActor = Cast<AGaussianSceneActor>(SpawnedActor);
		if (!GaussianActor)
		{
			continue;
		}

		GaussianActor->SetGaussianAsset(Asset);
		ActorSubsystem->SetActorSelectionState(GaussianActor, true);

		if (GCurrentLevelEditingViewportClient && Asset->Bounds.Extent.GetMax() > KINDA_SMALL_NUMBER)
		{
			const FBox SceneBox = Asset->Bounds.GetBox();
			if (SceneBox.IsValid)
			{
				GCurrentLevelEditingViewportClient->FocusViewportOnBox(SceneBox, true);
			}
		}
	}

	if (OffsetIndex > 0)
	{
		GEditor->NoteSelectionChange();
	}
}

#undef LOCTEXT_NAMESPACE
