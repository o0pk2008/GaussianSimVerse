// Copyright Epic Games, Inc. All Rights Reserved.

#include "Editor/GaussianProxyMeshGenerator.h"

#if WITH_EDITOR

#include "GaussianAsset.h"
#include "GaussianStreamedSceneAsset.h"
#include "Streaming/GaussianLodTypes.h"
#include "Import/GaussianSogChunkLoader.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "Misc/ScopedSlowTask.h"
#include "ObjectTools.h"
#include "HAL/FileManager.h"
#include "Materials/Material.h"
#include "RenderingThread.h"

namespace GaussianProxyMeshPrivate
{
	struct FInt3
	{
		int32 X = 0;
		int32 Y = 0;
		int32 Z = 0;

		bool operator==(const FInt3& O) const { return X == O.X && Y == O.Y && Z == O.Z; }
		friend uint32 GetTypeHash(const FInt3& V)
		{
			return HashCombine(HashCombine(::GetTypeHash(V.X), ::GetTypeHash(V.Y)), ::GetTypeHash(V.Z));
		}
	};

	/**
	 * Centers-only sampling. Scale shells and dilate made the proxy noticeably fatter than the
	 * rendered Gaussian (ellipsoids fall off; hard shells/voxels do not).
	 */
	static void AppendSplatProxySamples(
		const FGaussianSplatData& Splat,
		float MinOpacity,
		int32 MaxPoints,
		TArray<FVector>& OutPoints,
		int32& InOutAdded)
	{
		if (InOutAdded >= MaxPoints || Splat.Color.W < MinOpacity)
		{
			return;
		}

		OutPoints.Add(FVector(Splat.Position));
		++InOutAdded;
	}

	static const FInt3* FaceNeighborOffsets()
	{
		static const FInt3 Offsets[6] = {
			{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}
		};
		return Offsets;
	}

	static void DilateSolid(TSet<FInt3>& Solid, int32 Rings)
	{
		if (Rings <= 0 || Solid.Num() == 0)
		{
			return;
		}

		const FInt3* Offsets = FaceNeighborOffsets();
		TSet<FInt3> Current = Solid;
		for (int32 Ring = 0; Ring < Rings; ++Ring)
		{
			TSet<FInt3> Added;
			for (const FInt3& V : Current)
			{
				for (int32 i = 0; i < 6; ++i)
				{
					const FInt3 N{V.X + Offsets[i].X, V.Y + Offsets[i].Y, V.Z + Offsets[i].Z};
					if (!Solid.Contains(N))
					{
						Added.Add(N);
					}
				}
			}
			if (Added.Num() == 0)
			{
				break;
			}
			Solid.Append(Added);
			Current = MoveTemp(Added);
		}
	}

	/** Peel outer rings (reduces fat fringe from sparse occupancy). */
	static void ErodeSolid(TSet<FInt3>& Solid, int32 Rings)
	{
		if (Rings <= 0 || Solid.Num() == 0)
		{
			return;
		}

		const FInt3* Offsets = FaceNeighborOffsets();
		for (int32 Ring = 0; Ring < Rings; ++Ring)
		{
			TSet<FInt3> ToRemove;
			for (const FInt3& V : Solid)
			{
				for (int32 i = 0; i < 6; ++i)
				{
					const FInt3 N{V.X + Offsets[i].X, V.Y + Offsets[i].Y, V.Z + Offsets[i].Z};
					if (!Solid.Contains(N))
					{
						ToRemove.Add(V);
						break;
					}
				}
			}
			if (ToRemove.Num() == 0)
			{
				break;
			}
			for (const FInt3& V : ToRemove)
			{
				Solid.Remove(V);
			}
			if (Solid.Num() == 0)
			{
				break;
			}
		}
	}

	static void CollectFinestLodSlices(const FGaussianLodTreeNode& Node, TArray<FGaussianLodSlice>& OutSlices)
	{
		if (Node.IsLeaf())
		{
			const FGaussianLodSlice* Best = nullptr;
			for (const FGaussianLodSlice& Slice : Node.LodSlices)
			{
				if (Slice.Count <= 0 || Slice.FileIndex == INDEX_NONE)
				{
					continue;
				}
				// Lower LodLevel = finer detail in this streamed SOG convention.
				if (!Best || Slice.LodLevel < Best->LodLevel)
				{
					Best = &Slice;
				}
			}
			if (Best)
			{
				OutSlices.Add(*Best);
			}
			return;
		}

		for (const FGaussianLodTreeNode& Child : Node.Children)
		{
			CollectFinestLodSlices(Child, OutSlices);
		}
	}

	/**
	 * Emit a quad with triangles whose geometric normal matches OutwardNormal.
	 * Previous hard-coded CW winding was only correct for some axes, so mixed faces flipped.
	 */
	static void AppendQuadOutward(
		FMeshDescription& MeshDescription,
		FPolygonGroupID PolyGroup,
		TVertexAttributesRef<FVector3f> VertexPositions,
		TVertexInstanceAttributesRef<FVector3f> VertexNormals,
		FVector3f A,
		FVector3f B,
		FVector3f C,
		FVector3f D,
		const FVector3f& OutwardNormal)
	{
		// Right-hand cross: if A-B-C points inward, reverse to A-D-C / A-C-B by swapping B and D.
		const FVector3f GeoNormal = FVector3f::CrossProduct(B - A, C - A);
		if (FVector3f::DotProduct(GeoNormal, OutwardNormal) < 0.0f)
		{
			Swap(B, D);
		}

		const FVertexID V0 = MeshDescription.CreateVertex();
		const FVertexID V1 = MeshDescription.CreateVertex();
		const FVertexID V2 = MeshDescription.CreateVertex();
		const FVertexID V3 = MeshDescription.CreateVertex();
		VertexPositions[V0] = A;
		VertexPositions[V1] = B;
		VertexPositions[V2] = C;
		VertexPositions[V3] = D;

		const FVertexInstanceID I0 = MeshDescription.CreateVertexInstance(V0);
		const FVertexInstanceID I1 = MeshDescription.CreateVertexInstance(V1);
		const FVertexInstanceID I2 = MeshDescription.CreateVertexInstance(V2);
		const FVertexInstanceID I3 = MeshDescription.CreateVertexInstance(V3);

		const FVector3f N = OutwardNormal.GetSafeNormal();
		VertexNormals[I0] = N;
		VertexNormals[I1] = N;
		VertexNormals[I2] = N;
		VertexNormals[I3] = N;

		// Front face: A-B-C and A-C-D (consistent outward after optional B/D swap).
		{
			TArray<FVertexInstanceID, TFixedAllocator<3>> Tri;
			Tri.Add(I0);
			Tri.Add(I1);
			Tri.Add(I2);
			MeshDescription.CreatePolygon(PolyGroup, Tri);
		}
		{
			TArray<FVertexInstanceID, TFixedAllocator<3>> Tri;
			Tri.Add(I0);
			Tri.Add(I2);
			Tri.Add(I3);
			MeshDescription.CreatePolygon(PolyGroup, Tri);
		}
	}

	static void ExtractExposedFaces(
		const TSet<FInt3>& Solid,
		float VoxelSize,
		const FVector& GridOrigin,
		FMeshDescription& MeshDescription)
	{
		FStaticMeshAttributes Attributes(MeshDescription);
		Attributes.Register();
		const FPolygonGroupID PolyGroup = MeshDescription.CreatePolygonGroup();
		TVertexAttributesRef<FVector3f> VertexPositions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector3f> VertexNormals = Attributes.GetVertexInstanceNormals();

		// MUST recompute every call — do NOT use static storage for H-dependent geometry.
		// A previous bug kept FaceQuads as static, so the first generation permanently froze H
		// (e.g. 25cm from VoxelSize=50). Later size=1 runs still extruded 50cm cubes → mesh
		// half-extent ≈ sample + 25 and looked fat / stacked (see object_YiZi logs).
		const float H = VoxelSize * 0.5f;
		const FInt3 Neigh[6] = {
			{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}
		};
		const FVector3f Outward[6] = {
			FVector3f(1, 0, 0), FVector3f(-1, 0, 0),
			FVector3f(0, 1, 0), FVector3f(0, -1, 0),
			FVector3f(0, 0, 1), FVector3f(0, 0, -1)
		};
		// Corner order is arbitrary; AppendQuadOutward fixes winding against Outward[i].
		const FVector3f FaceQuads[6][4] = {
			// +X  (Y,Z plane)
			{ FVector3f(H, -H, -H), FVector3f(H, H, -H), FVector3f(H, H, H), FVector3f(H, -H, H) },
			// -X
			{ FVector3f(-H, -H, -H), FVector3f(-H, -H, H), FVector3f(-H, H, H), FVector3f(-H, H, -H) },
			// +Y  (X,Z plane)
			{ FVector3f(-H, H, -H), FVector3f(-H, H, H), FVector3f(H, H, H), FVector3f(H, H, -H) },
			// -Y
			{ FVector3f(-H, -H, -H), FVector3f(H, -H, -H), FVector3f(H, -H, H), FVector3f(-H, -H, H) },
			// +Z  (X,Y plane)
			{ FVector3f(-H, -H, H), FVector3f(H, -H, H), FVector3f(H, H, H), FVector3f(-H, H, H) },
			// -Z
			{ FVector3f(-H, -H, -H), FVector3f(-H, H, -H), FVector3f(H, H, -H), FVector3f(H, -H, -H) },
		};

		for (const FInt3& V : Solid)
		{
			const FVector Center = GridOrigin + FVector(
				(static_cast<float>(V.X) + 0.5f) * VoxelSize,
				(static_cast<float>(V.Y) + 0.5f) * VoxelSize,
				(static_cast<float>(V.Z) + 0.5f) * VoxelSize);

			for (int32 F = 0; F < 6; ++F)
			{
				const FInt3 N{V.X + Neigh[F].X, V.Y + Neigh[F].Y, V.Z + Neigh[F].Z};
				if (Solid.Contains(N))
				{
					continue;
				}

				const FVector3f C(Center);
				AppendQuadOutward(
					MeshDescription,
					PolyGroup,
					VertexPositions,
					VertexNormals,
					C + FaceQuads[F][0],
					C + FaceQuads[F][1],
					C + FaceQuads[F][2],
					C + FaceQuads[F][3],
					Outward[F]);
			}
		}
	}
}

UStaticMesh* FGaussianProxyMeshGenerator::BuildMeshFromPoints(
	const TArray<FVector>& Points,
	const FGaussianProxyMeshBuildSettings& Settings,
	const FString& AssetName,
	FString& OutError,
	FVector* OutLocalOffset)
{
	using namespace GaussianProxyMeshPrivate;

	if (Points.Num() < 8)
	{
		OutError = TEXT("Not enough sample points to build a proxy mesh (need at least 8).");
		return nullptr;
	}

	// VoxelSizeCm is UE world units (centimeters). Larger size => larger cubes.
	const float VoxelSize = FMath::Max(Settings.VoxelSizeCm, 1.0f);

	// Work in a recentered frame so Static Mesh Editor preview sits near the origin.
	// Scene alignment is restored by applying OutLocalOffset on the mesh component.
	FVector SampleMin = Points[0];
	FVector SampleMax = Points[0];
	for (const FVector& P : Points)
	{
		SampleMin = SampleMin.ComponentMin(P);
		SampleMax = SampleMax.ComponentMax(P);
	}
	const FVector MeshOrigin = Settings.bCenterMeshAtOrigin
		? (SampleMin + SampleMax) * 0.5
		: FVector::ZeroVector;
	if (OutLocalOffset)
	{
		*OutLocalOffset = MeshOrigin;
	}

	TArray<FVector> LocalPoints;
	LocalPoints.Reserve(Points.Num());
	for (const FVector& P : Points)
	{
		LocalPoints.Add(P - MeshOrigin);
	}

	FVector Min = LocalPoints[0];
	FVector Max = LocalPoints[0];
	for (const FVector& P : LocalPoints)
	{
		Min = Min.ComponentMin(P);
		Max = Max.ComponentMax(P);
	}

	// Pad one voxel so edge samples are never clamped into a wrong cell.
	Min -= FVector(VoxelSize);
	Max += FVector(VoxelSize);

	const FVector Extent = Max - Min;
	const int32 DimX = FMath::Max(1, FMath::CeilToInt(Extent.X / VoxelSize) + 1);
	const int32 DimY = FMath::Max(1, FMath::CeilToInt(Extent.Y / VoxelSize) + 1);
	const int32 DimZ = FMath::Max(1, FMath::CeilToInt(Extent.Z / VoxelSize) + 1);
	const int64 VoxelCount = static_cast<int64>(DimX) * DimY * DimZ;
	if (VoxelCount > 1024ll * 1024ll * 256ll || DimX > 2048 || DimY > 2048 || DimZ > 2048)
	{
		OutError = FString::Printf(
			TEXT("Voxel grid too large (%dx%dx%d). Increase Voxel Size (current %.1f cm)."),
			DimX, DimY, DimZ, VoxelSize);
		return nullptr;
	}

	// Count hits per cell so sparse fringe does not inflate a solid "fat" shell.
	TMap<FInt3, int32> Hits;
	Hits.Reserve(LocalPoints.Num());
	for (const FVector& P : LocalPoints)
	{
		const FInt3 Cell{
			FMath::Clamp(FMath::FloorToInt((P.X - Min.X) / VoxelSize), 0, DimX - 1),
			FMath::Clamp(FMath::FloorToInt((P.Y - Min.Y) / VoxelSize), 0, DimY - 1),
			FMath::Clamp(FMath::FloorToInt((P.Z - Min.Z) / VoxelSize), 0, DimZ - 1)
		};
		Hits.FindOrAdd(Cell)++;
	}

	const int32 MinHits = FMath::Max(Settings.MinHitsPerVoxel, 1);
	TSet<FInt3> Solid;
	Solid.Reserve(Hits.Num());
	for (const TPair<FInt3, int32>& Pair : Hits)
	{
		if (Pair.Value >= MinHits)
		{
			Solid.Add(Pair.Key);
		}
	}

	DilateSolid(Solid, Settings.DilateRings);
	ErodeSolid(Solid, Settings.ShrinkRings);
	if (Solid.Num() == 0)
	{
		OutError = TEXT("Voxelization produced an empty solid set. Lower Min Hits / Shrink, or Min Opacity.");
		return nullptr;
	}

	FMeshDescription MeshDescription;
	ExtractExposedFaces(Solid, VoxelSize, Min, MeshDescription);
	if (MeshDescription.Vertices().Num() == 0)
	{
		OutError = TEXT("No surface faces generated from voxels.");
		return nullptr;
	}

	const FString PackagePath = Settings.PackagePath.IsEmpty()
		? TEXT("/Game/GaussianProxies")
		: Settings.PackagePath;
	const FString SafeName = ObjectTools::SanitizeObjectName(AssetName.IsEmpty() ? TEXT("GaussianProxy") : AssetName);
	const FString PackageName = PackagePath / SafeName;

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		OutError = FString::Printf(TEXT("Failed to create package %s"), *PackageName);
		return nullptr;
	}
	Package->FullyLoad();

	// Reuse existing asset on regenerate so content-browser refs stay valid.
	// IMPORTANT: must release GPU render resources before rebuild, or UE fatals with
	// "FRenderResource was deleted without being released first" (seen on object_*_Proxy).
	UStaticMesh* StaticMesh = FindObject<UStaticMesh>(Package, *SafeName);
	const bool bCreatedNew = (StaticMesh == nullptr);
	if (!StaticMesh)
	{
		StaticMesh = NewObject<UStaticMesh>(Package, *SafeName, RF_Public | RF_Standalone | RF_Transactional);
	}
	if (!StaticMesh)
	{
		OutError = TEXT("Failed to create UStaticMesh object.");
		return nullptr;
	}

	if (!bCreatedNew)
	{
		// Drop RHI resources still held by the previous build / components using this mesh.
		StaticMesh->PreEditChange(nullptr);
		StaticMesh->ReleaseResources();
		StaticMesh->ReleaseResourcesFence.Wait();
		FlushRenderingCommands();
	}

	TArray<const FMeshDescription*> MeshDescriptions;
	MeshDescriptions.Add(&MeshDescription);
	UStaticMesh::FBuildMeshDescriptionsParams BuildParams;
	BuildParams.bBuildSimpleCollision = true;
	BuildParams.bFastBuild = true;
	BuildParams.bAllowCpuAccess = true;
	// Avoid marking the mesh as already initialized with stale render data during rebuild.
	BuildParams.bUseHashAsGuid = false;
	StaticMesh->BuildFromMeshDescriptions(MeshDescriptions, BuildParams);

	// Opaque default material so the mesh writes Scene Depth (translucent/unassigned will not).
	if (UMaterial* DefaultMat = LoadObject<UMaterial>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")))
	{
		if (StaticMesh->GetStaticMaterials().Num() == 0)
		{
			StaticMesh->GetStaticMaterials().Add(FStaticMaterial(DefaultMat));
		}
		else
		{
			StaticMesh->GetStaticMaterials()[0] = FStaticMaterial(DefaultMat);
		}
	}

	if (!bCreatedNew)
	{
		StaticMesh->PostEditChange();
	}
	else
	{
		FAssetRegistryModule::AssetCreated(StaticMesh);
	}

	Package->MarkPackageDirty();

#if WITH_EDITOR
	const FString PackageFilename = FPackageName::LongPackageNameToFilename(
		PackageName,
		FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	UPackage::SavePackage(Package, StaticMesh, *PackageFilename, SaveArgs);
#endif

	return StaticMesh;
}

bool FGaussianProxyMeshGenerator::CollectPointsFromAsset(
	const UGaussianAsset* Asset,
	const FGaussianProxyMeshBuildSettings& Settings,
	TArray<FVector>& OutPoints,
	FString& OutError)
{
	OutPoints.Reset();
	if (!Asset)
	{
		OutError = TEXT("Gaussian asset is null.");
		return false;
	}

	// Prefer local dense sampling with voxel-aware shells over asset helper defaults.
	if (Asset->GaussianCount <= 0)
	{
		OutError = TEXT("Asset has no sampleable splat positions (missing staging/bulk data).");
		return false;
	}

	// CollectProxySamplePoints uses centers-first logic; pass through for bulk staging.
	const int32 Added = Asset->CollectProxySamplePoints(
		OutPoints,
		Settings.MinOpacity,
		Settings.MaxSamplePoints,
		Settings.VoxelSizeCm);
	if (Added <= 0 || OutPoints.Num() < 8)
	{
		OutError = TEXT("Asset has no sampleable splat positions (missing staging/bulk data).");
		return false;
	}
	return true;
}

bool FGaussianProxyMeshGenerator::CollectPointsFromStreamedAsset(
	const UGaussianStreamedSceneAsset* Asset,
	const FGaussianProxyMeshBuildSettings& Settings,
	TArray<FVector>& OutPoints,
	FString& OutError,
	TFunction<void(const FString&)> Progress)
{
	using namespace GaussianProxyMeshPrivate;

	OutPoints.Reset();
	if (!Asset)
	{
		OutError = TEXT("Streamed scene asset is null.");
		return false;
	}

	FString TreeError;
	if (!Asset->EnsureLodTreeLoaded(TreeError))
	{
		OutError = TreeError;
		return false;
	}

	// Prefer finest LOD only — mixing coarse LODs piles large soft blobs into occupancy (fat + stacked).
	TArray<FGaussianLodSlice> FinestSlices;
	CollectFinestLodSlices(Asset->LodTree, FinestSlices);

	// Fallback: whole files if tree has no leaf slices.
	const bool bUseSlices = FinestSlices.Num() > 0;
	const int32 JobCount = bUseSlices ? FinestSlices.Num() : Asset->LodMeta.Filenames.Num();
	if (JobCount <= 0)
	{
		OutError = TEXT("Streamed asset has no chunk filenames / LOD slices.");
		return false;
	}

	const int32 BudgetPerJob = FMath::Max(500, Settings.MaxSamplePoints / FMath::Max(1, JobCount));
	OutPoints.Reserve(Settings.MaxSamplePoints);

	FScopedSlowTask SlowTask(
		static_cast<float>(JobCount),
		NSLOCTEXT("GaussianSimVerse", "ProxyGenStreamed", "Sampling finest SOG LOD for proxy mesh..."));
	SlowTask.MakeDialog(true);

	for (int32 JobIndex = 0; JobIndex < JobCount; ++JobIndex)
	{
		if (SlowTask.ShouldCancel())
		{
			OutError = TEXT("Proxy mesh generation cancelled.");
			return false;
		}
		SlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("Chunk %d / %d"), JobIndex + 1, JobCount)));

		if (Progress)
		{
			Progress(FString::Printf(TEXT("Loading chunk %d/%d"), JobIndex + 1, JobCount));
		}

		int32 FileIndex = JobIndex;
		FGaussianSogChunkLoader::FLoadRange Range;
		const FGaussianSogChunkLoader::FLoadRange* RangePtr = nullptr;
		if (bUseSlices)
		{
			const FGaussianLodSlice& Slice = FinestSlices[JobIndex];
			FileIndex = Slice.FileIndex;
			Range.Offset = Slice.Offset;
			Range.Count = Slice.Count;
			RangePtr = &Range;
		}

		FString Directory;
		if (!Asset->ResolveChunkDirectory(FileIndex, Directory))
		{
			continue;
		}

		TArray<FGaussianSplatData> Splats;
		FString LoadError;
		if (!FGaussianSogChunkLoader::LoadDirectory(Directory, Splats, LoadError, nullptr, nullptr, RangePtr))
		{
			UE_LOG(LogTemp, Warning, TEXT("Proxy sample skip %s: %s"), *Directory, *LoadError);
			continue;
		}

		const int32 Total = Splats.Num();
		if (Total <= 0)
		{
			continue;
		}

		const int32 JobPointCap = FMath::Min(BudgetPerJob, Settings.MaxSamplePoints - OutPoints.Num());
		const int32 Stride = FMath::Max(1, Total / FMath::Max(1, JobPointCap));
		int32 Added = 0;
		for (int32 Index = 0; Index < Total && Added < JobPointCap; Index += Stride)
		{
			// Absolute dataset/UE coordinates (actor-local when DatasetPivot=0).
			AppendSplatProxySamples(Splats[Index], Settings.MinOpacity, JobPointCap, OutPoints, Added);
		}

		if (OutPoints.Num() >= Settings.MaxSamplePoints)
		{
			break;
		}
	}

	if (OutPoints.Num() < 8)
	{
		OutError = TEXT("Failed to sample enough points from streamed SOG dataset.");
		return false;
	}
	return true;
}

#endif // WITH_EDITOR
