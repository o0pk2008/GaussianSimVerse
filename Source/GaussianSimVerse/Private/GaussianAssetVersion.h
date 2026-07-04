// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** Custom UObject serialization version for UGaussianAsset. */
struct FGaussianAssetSerializationVersion
{
	enum Type : int32
	{
		Initial = 0,
		AddedShCoefficientData = 1,
		Latest = AddedShCoefficientData
	};

	static const FGuid GUID;
};
