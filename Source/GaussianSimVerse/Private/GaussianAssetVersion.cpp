// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianAssetVersion.h"
#include "Serialization/CustomVersion.h"

const FGuid FGaussianAssetSerializationVersion::GUID(
	0x8F4A2B1C, 0x6D3E4F50, 0x9A8B7C6D, 0xE5F40312);

// Register before any UGaussianAsset::Serialize call (including editor delete/save).
FCustomVersionRegistration GRegisterGaussianAssetSerializationVersion(
	FGaussianAssetSerializationVersion::GUID,
	FGaussianAssetSerializationVersion::Latest,
	TEXT("GaussianSimVerse GaussianAsset SH coefficient payload"));
