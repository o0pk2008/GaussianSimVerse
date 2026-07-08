// Copyright Epic Games, Inc. All Rights Reserved.

#include "Streaming/GaussianLodMetaReader.h"
#include "Import/GaussianImportUtils.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace GaussianLodMetaPrivate
{
	static bool ParseBound(const TSharedPtr<FJsonObject>& BoundObject, FGaussianBounds& OutBounds)
	{
		const TArray<TSharedPtr<FJsonValue>>* MinArray = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* MaxArray = nullptr;
		if (!BoundObject->TryGetArrayField(TEXT("min"), MinArray)
			|| !BoundObject->TryGetArrayField(TEXT("max"), MaxArray)
			|| !MinArray || MinArray->Num() != 3
			|| !MaxArray || MaxArray->Num() != 3)
		{
			return false;
		}

		const FVector3f Min(
			static_cast<float>((*MinArray)[0]->AsNumber()),
			static_cast<float>((*MinArray)[1]->AsNumber()),
			static_cast<float>((*MinArray)[2]->AsNumber()));
		const FVector3f Max(
			static_cast<float>((*MaxArray)[0]->AsNumber()),
			static_cast<float>((*MaxArray)[1]->AsNumber()),
			static_cast<float>((*MaxArray)[2]->AsNumber()));
		OutBounds = GaussianImport::PlayCanvasBoundsToUE(Min, Max);
		return true;
	}

	static bool ParseLodSlices(const TSharedPtr<FJsonObject>& LodsObject, TArray<FGaussianLodSlice>& OutSlices)
	{
		OutSlices.Reset();
		if (!LodsObject.IsValid())
		{
			return false;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : LodsObject->Values)
		{
			const TSharedPtr<FJsonObject>* SliceObject = nullptr;
			if (!Pair.Value->TryGetObject(SliceObject) || !SliceObject || !(*SliceObject).IsValid())
			{
				continue;
			}

			FGaussianLodSlice Slice;
			Slice.LodLevel = FCString::Atoi(*Pair.Key);
			(*SliceObject)->TryGetNumberField(TEXT("file"), Slice.FileIndex);
			(*SliceObject)->TryGetNumberField(TEXT("offset"), Slice.Offset);
			(*SliceObject)->TryGetNumberField(TEXT("count"), Slice.Count);
			OutSlices.Add(Slice);
		}

		OutSlices.Sort([](const FGaussianLodSlice& A, const FGaussianLodSlice& B)
		{
			return A.LodLevel < B.LodLevel;
		});
		return OutSlices.Num() > 0;
	}

	static bool ParseNode(const TSharedPtr<FJsonObject>& NodeObject, FGaussianLodTreeNode& OutNode, int32& InOutNextLeafId)
	{
		const TSharedPtr<FJsonObject>* BoundObject = nullptr;
		if (!NodeObject->TryGetObjectField(TEXT("bound"), BoundObject) || !BoundObject)
		{
			return false;
		}

		if (!ParseBound(*BoundObject, OutNode.Bounds))
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* LodsObject = nullptr;
		if (NodeObject->TryGetObjectField(TEXT("lods"), LodsObject) && LodsObject)
		{
			ParseLodSlices(*LodsObject, OutNode.LodSlices);
		}

		const TArray<TSharedPtr<FJsonValue>>* ChildrenArray = nullptr;
		if (NodeObject->TryGetArrayField(TEXT("children"), ChildrenArray) && ChildrenArray)
		{
			for (const TSharedPtr<FJsonValue>& ChildValue : *ChildrenArray)
			{
				const TSharedPtr<FJsonObject>* ChildObject = nullptr;
				if (!ChildValue->TryGetObject(ChildObject) || !ChildObject)
				{
					continue;
				}

				FGaussianLodTreeNode ChildNode;
				if (ParseNode(*ChildObject, ChildNode, InOutNextLeafId))
				{
					OutNode.Children.Add(MoveTemp(ChildNode));
				}
			}
		}

		if (OutNode.IsLeaf())
		{
			OutNode.LeafId = InOutNextLeafId++;
		}

		return OutNode.IsLeaf() || OutNode.Children.Num() > 0;
	}
}

bool FGaussianLodMetaReader::ParseFile(const FString& LodMetaPath, FGaussianLodMetaData& OutMeta, FGaussianLodTreeNode& OutTree, FString& OutError)
{
	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *LodMetaPath))
	{
		OutError = FString::Printf(TEXT("Failed to read lod-meta.json: %s"), *LodMetaPath);
		return false;
	}

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		OutError = FString::Printf(TEXT("Failed to parse lod-meta.json: %s"), *LodMetaPath);
		return false;
	}

	RootObject->TryGetNumberField(TEXT("lodLevels"), OutMeta.LodLevels);
	RootObject->TryGetStringField(TEXT("environment"), OutMeta.EnvironmentRelativePath);

	const TArray<TSharedPtr<FJsonValue>>* FilenamesArray = nullptr;
	if (RootObject->TryGetArrayField(TEXT("filenames"), FilenamesArray) && FilenamesArray)
	{
		OutMeta.Filenames.Reserve(FilenamesArray->Num());
		for (const TSharedPtr<FJsonValue>& Value : *FilenamesArray)
		{
			OutMeta.Filenames.Add(Value->AsString());
		}
	}

	const TSharedPtr<FJsonObject>* TreeObject = nullptr;
	if (!RootObject->TryGetObjectField(TEXT("tree"), TreeObject) || !TreeObject)
	{
		OutError = TEXT("lod-meta.json missing tree");
		return false;
	}

	int32 NextLeafId = 0;
	if (!GaussianLodMetaPrivate::ParseNode(*TreeObject, OutTree, NextLeafId))
	{
		OutError = TEXT("lod-meta.json tree parse failed");
		return false;
	}

	OutMeta.SceneBounds = OutTree.Bounds;
	return OutMeta.LodLevels > 0 && OutMeta.Filenames.Num() > 0;
}
