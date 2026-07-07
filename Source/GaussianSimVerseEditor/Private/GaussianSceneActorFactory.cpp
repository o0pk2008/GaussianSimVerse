// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSceneActorFactory.h"
#include "GaussianAsset.h"
#include "GaussianSceneActor.h"

UGaussianSceneActorFactory::UGaussianSceneActorFactory()
{
	DisplayName = NSLOCTEXT("GaussianSimVerse", "GaussianSceneActorFactoryDisplayName", "Gaussian Scene");
	NewActorClass = AGaussianSceneActor::StaticClass();
}

bool UGaussianSceneActorFactory::CanCreateActorFrom(const FAssetData& AssetData, FText& OutErrorMsg)
{
	if (!AssetData.IsValid())
	{
		OutErrorMsg = NSLOCTEXT("GaussianSimVerse", "GaussianSceneActorFactory_NoGaussianAsset", "A valid Gaussian Asset must be specified.");
		return false;
	}

	const UGaussianAsset* GaussianAsset = Cast<UGaussianAsset>(AssetData.GetAsset());
	if (!GaussianAsset)
	{
		OutErrorMsg = NSLOCTEXT("GaussianSimVerse", "GaussianSceneActorFactory_NoGaussianAsset", "A valid Gaussian Asset must be specified.");
		return false;
	}

	return true;
}

void UGaussianSceneActorFactory::PostSpawnActor(UObject* Asset, AActor* NewActor)
{
	UGaussianAsset* GaussianAsset = Cast<UGaussianAsset>(Asset);
	AGaussianSceneActor* GaussianActor = Cast<AGaussianSceneActor>(NewActor);
	if (!GaussianAsset || !GaussianActor)
	{
		return;
	}

	GaussianActor->SetGaussianAsset(GaussianAsset);
}

UObject* UGaussianSceneActorFactory::GetAssetFromActorInstance(AActor* ActorInstance)
{
	if (const AGaussianSceneActor* GaussianActor = Cast<AGaussianSceneActor>(ActorInstance))
	{
		return GaussianActor->GaussianAsset.Get();
	}

	return nullptr;
}

