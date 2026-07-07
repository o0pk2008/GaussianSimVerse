// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ActorFactories/ActorFactory.h"
#include "GaussianSceneActorFactory.generated.h"

UCLASS()
class GAUSSIANSIMVERSEEDITOR_API UGaussianSceneActorFactory : public UActorFactory
{
	GENERATED_BODY()

public:
	UGaussianSceneActorFactory();

	virtual bool CanCreateActorFrom(const FAssetData& AssetData, FText& OutErrorMsg) override;
	virtual void PostSpawnActor(UObject* Asset, AActor* NewActor) override;
	virtual UObject* GetAssetFromActorInstance(AActor* ActorInstance) override;
};

