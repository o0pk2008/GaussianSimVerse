// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ActorFactories/ActorFactory.h"
#include "GaussianStreamedSceneActorFactory.generated.h"

UCLASS()
class GAUSSIANSIMVERSEEDITOR_API UGaussianStreamedSceneActorFactory : public UActorFactory
{
	GENERATED_BODY()

public:
	UGaussianStreamedSceneActorFactory();

	virtual bool CanCreateActorFrom(const FAssetData& AssetData, FText& OutErrorMsg) override;
	virtual void PostSpawnActor(UObject* Asset, AActor* NewActor) override;
	virtual UObject* GetAssetFromActorInstance(AActor* ActorInstance) override;
};
