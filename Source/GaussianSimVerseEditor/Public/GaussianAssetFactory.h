// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "GaussianAssetFactory.generated.h"

UCLASS()
class GAUSSIANSIMVERSEEDITOR_API UGaussianAssetFactory : public UFactory
{
	GENERATED_BODY()

public:
	UGaussianAssetFactory();

	virtual UObject* FactoryCreateFile(
		UClass* InClass,
		UObject* InParent,
		FName InName,
		EObjectFlags Flags,
		const FString& Filename,
		const TCHAR* Parms,
		FFeedbackContext* Warn,
		bool& bOutOperationCanceled) override;

	virtual bool FactoryCanImport(const FString& Filename) override;
};
