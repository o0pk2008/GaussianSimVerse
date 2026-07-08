// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianStreamedSceneActorFactory.h"
#include "GaussianStreamedSceneAsset.h"
#include "GaussianStreamedSceneActor.h"

UGaussianStreamedSceneActorFactory::UGaussianStreamedSceneActorFactory()
{
	DisplayName = NSLOCTEXT("GaussianSimVerse", "GaussianStreamedSceneActorFactoryDisplayName", "Gaussian Streamed Scene");
	NewActorClass = AGaussianStreamedSceneActor::StaticClass();
}

bool UGaussianStreamedSceneActorFactory::CanCreateActorFrom(const FAssetData& AssetData, FText& OutErrorMsg)
{
	if (!AssetData.IsValid())
	{
		OutErrorMsg = NSLOCTEXT("GaussianSimVerse", "GaussianStreamedSceneActorFactory_NoAsset", "A valid Gaussian Streamed Scene Asset must be specified.");
		return false;
	}

	if (!Cast<UGaussianStreamedSceneAsset>(AssetData.GetAsset()))
	{
		OutErrorMsg = NSLOCTEXT("GaussianSimVerse", "GaussianStreamedSceneActorFactory_NoAsset", "A valid Gaussian Streamed Scene Asset must be specified.");
		return false;
	}

	return true;
}

void UGaussianStreamedSceneActorFactory::PostSpawnActor(UObject* Asset, AActor* NewActor)
{
	UGaussianStreamedSceneAsset* StreamedAsset = Cast<UGaussianStreamedSceneAsset>(Asset);
	AGaussianStreamedSceneActor* StreamedActor = Cast<AGaussianStreamedSceneActor>(NewActor);
	if (!StreamedAsset || !StreamedActor)
	{
		return;
	}

	StreamedActor->SetStreamedSceneAsset(StreamedAsset);
}

UObject* UGaussianStreamedSceneActorFactory::GetAssetFromActorInstance(AActor* ActorInstance)
{
	if (const AGaussianStreamedSceneActor* StreamedActor = Cast<AGaussianStreamedSceneActor>(ActorInstance))
	{
		return StreamedActor->StreamedSceneAsset.Get();
	}

	return nullptr;
}
