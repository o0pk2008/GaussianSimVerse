// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "GaussianStreamedSceneAssetFactory.generated.h"

UCLASS()
class GAUSSIANSIMVERSEEDITOR_API UGaussianStreamedSceneAssetFactory : public UFactory
{
	GENERATED_BODY()

public:
	UGaussianStreamedSceneAssetFactory();

	virtual bool FactoryCanImport(const FString& Filename) override;
	virtual UObject* FactoryCreateFile(
		UClass* InClass,
		UObject* InParent,
		FName InName,
		EObjectFlags Flags,
		const FString& Filename,
		const TCHAR* Parms,
		FFeedbackContext* Warn,
		bool& bOutOperationCanceled) override;
};
