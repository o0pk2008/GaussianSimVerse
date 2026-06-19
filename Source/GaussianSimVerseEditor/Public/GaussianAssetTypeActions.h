// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AssetTypeActions_Base.h"

class FAssetTypeActions_GaussianAsset : public FAssetTypeActions_Base
{
public:
	virtual FText GetName() const override;
	virtual FColor GetTypeColor() const override;
	virtual UClass* GetSupportedClass() const override;
	virtual uint32 GetCategories() override;
	virtual void GetActions(const TArray<UObject*>& InObjects, FMenuBuilder& MenuBuilder) override;

private:
	static void PlaceActorsInLevel(const TArray<TWeakObjectPtr<class UGaussianAsset>>& Assets);
};
