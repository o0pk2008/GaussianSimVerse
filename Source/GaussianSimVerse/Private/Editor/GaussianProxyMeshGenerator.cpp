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
#include "Containers/Queue.h"

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

	static bool InGrid(const FInt3& V, int32 DimX, int32 DimY, int32 DimZ)
	{
		return V.X >= 0 && V.Y >= 0 && V.Z >= 0 && V.X < DimX && V.Y < DimY && V.Z < DimZ;
	}

	/**
	 * Dense bit grid for PlayCanvas-faithful fill/carve (CPU port of sparse+GPU logic).
	 * Index: x + y*NX + z*NX*NY. UE: Z-up (PlayCanvas carve height is Y-up → map to Z).
	 */
	struct FDenseVoxelBits
	{
		int32 NX = 0;
		int32 NY = 0;
		int32 NZ = 0;
		TBitArray<> Bits;

		int64 Count() const { return static_cast<int64>(NX) * NY * NZ; }

		int64 Index(int32 X, int32 Y, int32 Z) const
		{
			return static_cast<int64>(Z) * NX * NY + static_cast<int64>(Y) * NX + X;
		}

		void Init(int32 InX, int32 InY, int32 InZ, bool bValue)
		{
			NX = InX;
			NY = InY;
			NZ = InZ;
			Bits.Init(bValue, static_cast<int32>(Count()));
		}

		bool InBounds(int32 X, int32 Y, int32 Z) const
		{
			return X >= 0 && Y >= 0 && Z >= 0 && X < NX && Y < NY && Z < NZ;
		}

		bool Get(int32 X, int32 Y, int32 Z) const
		{
			return Bits[static_cast<int32>(Index(X, Y, Z))];
		}

		void Set(int32 X, int32 Y, int32 Z, bool bValue)
		{
			Bits[static_cast<int32>(Index(X, Y, Z))] = bValue;
		}

		void FromSolidSet(const TSet<FInt3>& Solid)
		{
			Bits.Init(false, static_cast<int32>(Count()));
			for (const FInt3& V : Solid)
			{
				if (InBounds(V.X, V.Y, V.Z))
				{
					Set(V.X, V.Y, V.Z, true);
				}
			}
		}

		void ToSolidSet(TSet<FInt3>& OutSolid) const
		{
			OutSolid.Reset();
			OutSolid.Reserve(static_cast<int32>(Count() / 8));
			for (int32 Z = 0; Z < NZ; ++Z)
			{
				for (int32 Y = 0; Y < NY; ++Y)
				{
					for (int32 X = 0; X < NX; ++X)
					{
						if (Get(X, Y, Z))
						{
							OutSolid.Add(FInt3{X, Y, Z});
						}
					}
				}
			}
		}
	};

	/** Isotropic 6-neighbor dilate, Rings times. */
	static void DilateDenseIso(FDenseVoxelBits& Grid, int32 Rings)
	{
		if (Rings <= 0)
		{
			return;
		}
		const FInt3* Off = FaceNeighborOffsets();
		for (int32 R = 0; R < Rings; ++R)
		{
			TArray<FInt3> ToAdd;
			for (int32 Z = 0; Z < Grid.NZ; ++Z)
			{
				for (int32 Y = 0; Y < Grid.NY; ++Y)
				{
					for (int32 X = 0; X < Grid.NX; ++X)
					{
						if (!Grid.Get(X, Y, Z))
						{
							continue;
						}
						for (int32 i = 0; i < 6; ++i)
						{
							const int32 NX = X + Off[i].X;
							const int32 NY = Y + Off[i].Y;
							const int32 NZ = Z + Off[i].Z;
							if (Grid.InBounds(NX, NY, NZ) && !Grid.Get(NX, NY, NZ))
							{
								ToAdd.Add(FInt3{NX, NY, NZ});
							}
						}
					}
				}
			}
			for (const FInt3& C : ToAdd)
			{
				Grid.Set(C.X, C.Y, C.Z, true);
			}
		}
	}

	/** Anisotropic dilate: Rxz in X/Y, Rz in Z (capsule approx; UE Z-up). */
	static void DilateDenseAniso(FDenseVoxelBits& Grid, int32 Rxz, int32 Rz)
	{
		// Separable: expand X, then Y, then Z.
		auto ExpandAxis = [&](int32 DX, int32 DY, int32 DZ, int32 Rings)
		{
			for (int32 R = 0; R < Rings; ++R)
			{
				TArray<FInt3> ToAdd;
				for (int32 Z = 0; Z < Grid.NZ; ++Z)
				{
					for (int32 Y = 0; Y < Grid.NY; ++Y)
					{
						for (int32 X = 0; X < Grid.NX; ++X)
						{
							if (!Grid.Get(X, Y, Z))
							{
								continue;
							}
							const int32 NX = X + DX;
							const int32 NY = Y + DY;
							const int32 NZ = Z + DZ;
							if (Grid.InBounds(NX, NY, NZ) && !Grid.Get(NX, NY, NZ))
							{
								ToAdd.Add(FInt3{NX, NY, NZ});
							}
							const int32 PX = X - DX;
							const int32 PY = Y - DY;
							const int32 PZ = Z - DZ;
							if (Grid.InBounds(PX, PY, PZ) && !Grid.Get(PX, PY, PZ))
							{
								ToAdd.Add(FInt3{PX, PY, PZ});
							}
						}
					}
				}
				for (const FInt3& C : ToAdd)
				{
					Grid.Set(C.X, C.Y, C.Z, true);
				}
			}
		};
		if (Rxz > 0)
		{
			ExpandAxis(1, 0, 0, Rxz);
			ExpandAxis(0, 1, 0, Rxz);
		}
		if (Rz > 0)
		{
			ExpandAxis(0, 0, 1, Rz);
		}
	}

	static void OrDense(FDenseVoxelBits& Dest, const FDenseVoxelBits& Src)
	{
		const int32 N = Dest.Bits.Num();
		for (int32 i = 0; i < N; ++i)
		{
			if (Src.Bits[i])
			{
				Dest.Bits[i] = true;
			}
		}
	}

	/**
	 * PlayCanvas fillExterior (CPU):
	 * dilate solid → BFS exterior free from AABB → if seed free from outside, skip;
	 * else dilate exterior free and OR into solid (seal shell from outside).
	 */
	static bool ApplyExternalFillDense(
		FDenseVoxelBits& Solid,
		int32 SealRings,
		const FInt3& SeedCell)
	{
		if (Solid.Count() == 0)
		{
			return false;
		}

		FDenseVoxelBits Dilated = Solid;
		DilateDenseIso(Dilated, SealRings);

		FDenseVoxelBits ExteriorFree;
		ExteriorFree.Init(Solid.NX, Solid.NY, Solid.NZ, false);
		TQueue<FInt3> Queue;
		const FInt3* Off = FaceNeighborOffsets();

		auto TrySeed = [&](int32 X, int32 Y, int32 Z)
		{
			if (!Dilated.InBounds(X, Y, Z) || Dilated.Get(X, Y, Z) || ExteriorFree.Get(X, Y, Z))
			{
				return;
			}
			ExteriorFree.Set(X, Y, Z, true);
			Queue.Enqueue(FInt3{X, Y, Z});
		};

		for (int32 Y = 0; Y < Solid.NY; ++Y)
		{
			for (int32 Z = 0; Z < Solid.NZ; ++Z)
			{
				TrySeed(0, Y, Z);
				TrySeed(Solid.NX - 1, Y, Z);
			}
		}
		for (int32 X = 0; X < Solid.NX; ++X)
		{
			for (int32 Z = 0; Z < Solid.NZ; ++Z)
			{
				TrySeed(X, 0, Z);
				TrySeed(X, Solid.NY - 1, Z);
			}
		}
		for (int32 X = 0; X < Solid.NX; ++X)
		{
			for (int32 Y = 0; Y < Solid.NY; ++Y)
			{
				TrySeed(X, Y, 0);
				TrySeed(X, Y, Solid.NZ - 1);
			}
		}

		while (!Queue.IsEmpty())
		{
			FInt3 Cur;
			Queue.Dequeue(Cur);
			for (int32 i = 0; i < 6; ++i)
			{
				const int32 NX = Cur.X + Off[i].X;
				const int32 NY = Cur.Y + Off[i].Y;
				const int32 NZ = Cur.Z + Off[i].Z;
				if (!Dilated.InBounds(NX, NY, NZ) || Dilated.Get(NX, NY, NZ) || ExteriorFree.Get(NX, NY, NZ))
				{
					continue;
				}
				ExteriorFree.Set(NX, NY, NZ, true);
				Queue.Enqueue(FInt3{NX, NY, NZ});
			}
		}

		if (Solid.InBounds(SeedCell.X, SeedCell.Y, SeedCell.Z) && ExteriorFree.Get(SeedCell.X, SeedCell.Y, SeedCell.Z))
		{
			UE_LOG(LogTemp, Warning,
				TEXT("GaussianProxy Room external-fill skipped: seed reachable from outside. Move Fill Seed inside or raise Fill Seal Size."));
			return false;
		}

		// Official: dilate exterior free, then solid |= dilatedExterior.
		DilateDenseIso(ExteriorFree, SealRings);
		OrDense(Solid, ExteriorFree);
		UE_LOG(LogTemp, Log, TEXT("GaussianProxy Room external-fill applied (seed enclosed)."));
		return true;
	}

	/**
	 * PlayCanvas fillFloor (CPU, UE Z-up):
	 * walk each XY column up Z, fill empty under first solid; optional XZ neighborhood support.
	 */
	static void ApplyFloorFillDense(FDenseVoxelBits& Solid, int32 NeighborRadiusCells)
	{
		FDenseVoxelBits Found;
		Found.Init(Solid.NX, Solid.NY, Solid.NZ, false);

		TArray<int32> MinSolidZ;
		MinSolidZ.Init(INDEX_NONE, Solid.NX * Solid.NY);
		auto IdxXY = [&](int32 X, int32 Y) { return X * Solid.NY + Y; };

		for (int32 Z = 0; Z < Solid.NZ; ++Z)
		{
			for (int32 Y = 0; Y < Solid.NY; ++Y)
			{
				for (int32 X = 0; X < Solid.NX; ++X)
				{
					if (Solid.Get(X, Y, Z))
					{
						const int32 I = IdxXY(X, Y);
						if (MinSolidZ[I] == INDEX_NONE)
						{
							MinSolidZ[I] = Z;
						}
					}
				}
			}
		}

		const int32 R = FMath::Max(NeighborRadiusCells, 0);
		for (int32 X = 0; X < Solid.NX; ++X)
		{
			for (int32 Y = 0; Y < Solid.NY; ++Y)
			{
				const int32 SurfZ = MinSolidZ[IdxXY(X, Y)];
				if (SurfZ == INDEX_NONE)
				{
					continue;
				}
				bool bOk = (R == 0);
				if (!bOk)
				{
					for (int32 DX = -R; DX <= R && !bOk; ++DX)
					{
						for (int32 DY = -R; DY <= R && !bOk; ++DY)
						{
							if (DX == 0 && DY == 0)
							{
								continue;
							}
							const int32 NX = X + DX;
							const int32 NY = Y + DY;
							if (NX < 0 || NY < 0 || NX >= Solid.NX || NY >= Solid.NY)
							{
								continue;
							}
							if (MinSolidZ[IdxXY(NX, NY)] != INDEX_NONE)
							{
								bOk = true;
							}
						}
					}
				}
				if (!bOk)
				{
					continue;
				}
				for (int32 Z = 0; Z < SurfZ; ++Z)
				{
					Found.Set(X, Y, Z, true);
				}
			}
		}

		// Mild XZ dilate of found (approx official second dilate).
		if (R > 0)
		{
			DilateDenseAniso(Found, FMath::Min(R, 2), 0);
		}
		OrDense(Solid, Found);
		UE_LOG(LogTemp, Log, TEXT("GaussianProxy Outdoor floor-fill applied."));
	}

	/**
	 * PlayCanvas carve (CPU):
	 * blocked = dilate(solid, r, h/2); BFS free; nav = dilate(free);
	 * result solid = NOT nav (collision shell around navigable space).
	 */
	static bool ApplyCarveDense(
		FDenseVoxelBits& Solid,
		int32 KernelR,
		int32 ZHalf,
		const FInt3& SeedCell)
	{
		if (!Solid.InBounds(SeedCell.X, SeedCell.Y, SeedCell.Z))
		{
			UE_LOG(LogTemp, Warning, TEXT("GaussianProxy carve: seed outside grid, skipped."));
			return false;
		}

		FDenseVoxelBits Blocked = Solid;
		DilateDenseAniso(Blocked, KernelR, ZHalf);

		FInt3 Seed = SeedCell;
		if (Blocked.Get(Seed.X, Seed.Y, Seed.Z))
		{
			// Search nearest free cell (official findNearestFreeCell simplified).
			const int32 MaxR = FMath::Max(KernelR, ZHalf) * 2 + 4;
			bool bFound = false;
			for (int32 Rad = 1; Rad <= MaxR && !bFound; ++Rad)
			{
				for (int32 DZ = -Rad; DZ <= Rad && !bFound; ++DZ)
				{
					for (int32 DY = -Rad; DY <= Rad && !bFound; ++DY)
					{
						for (int32 DX = -Rad; DX <= Rad && !bFound; ++DX)
						{
							const int32 X = SeedCell.X + DX;
							const int32 Y = SeedCell.Y + DY;
							const int32 Z = SeedCell.Z + DZ;
							if (Blocked.InBounds(X, Y, Z) && !Blocked.Get(X, Y, Z))
							{
								Seed = FInt3{X, Y, Z};
								bFound = true;
							}
						}
					}
				}
			}
			if (!bFound)
			{
				UE_LOG(LogTemp, Warning, TEXT("GaussianProxy carve: seed blocked, no free cell, skipped."));
				return false;
			}
		}

		FDenseVoxelBits Visited;
		Visited.Init(Solid.NX, Solid.NY, Solid.NZ, false);
		TQueue<FInt3> Queue;
		Visited.Set(Seed.X, Seed.Y, Seed.Z, true);
		Queue.Enqueue(Seed);
		const FInt3* Off = FaceNeighborOffsets();
		int32 FreeCount = 1;

		while (!Queue.IsEmpty())
		{
			FInt3 Cur;
			Queue.Dequeue(Cur);
			for (int32 i = 0; i < 6; ++i)
			{
				const int32 NX = Cur.X + Off[i].X;
				const int32 NY = Cur.Y + Off[i].Y;
				const int32 NZ = Cur.Z + Off[i].Z;
				if (!Blocked.InBounds(NX, NY, NZ) || Blocked.Get(NX, NY, NZ) || Visited.Get(NX, NY, NZ))
				{
					continue;
				}
				Visited.Set(NX, NY, NZ, true);
				Queue.Enqueue(FInt3{NX, NY, NZ});
				++FreeCount;
			}
		}

		if (FreeCount < 8)
		{
			UE_LOG(LogTemp, Warning, TEXT("GaussianProxy carve: too few free cells (%d), skipped."), FreeCount);
			return false;
		}

		// Navigable region expanded by capsule kernel (official).
		FDenseVoxelBits Nav = Visited;
		DilateDenseAniso(Nav, KernelR, ZHalf);

		// Invert: collision solid = not navigable (within grid).
		for (int32 Z = 0; Z < Solid.NZ; ++Z)
		{
			for (int32 Y = 0; Y < Solid.NY; ++Y)
			{
				for (int32 X = 0; X < Solid.NX; ++X)
				{
					Solid.Set(X, Y, Z, !Nav.Get(X, Y, Z));
				}
			}
		}

		// Drop pure exterior bulk: keep solid only if adjacent to navigable (inner shell).
		// This matches usable collision mesh without AABB outer box faces dominating.
		TArray<FInt3> Keep;
		for (int32 Z = 0; Z < Solid.NZ; ++Z)
		{
			for (int32 Y = 0; Y < Solid.NY; ++Y)
			{
				for (int32 X = 0; X < Solid.NX; ++X)
				{
					if (!Solid.Get(X, Y, Z))
					{
						continue;
					}
					bool bAdjNav = false;
					for (int32 i = 0; i < 6; ++i)
					{
						const int32 NX = X + Off[i].X;
						const int32 NY = Y + Off[i].Y;
						const int32 NZ = Z + Off[i].Z;
						if (Nav.InBounds(NX, NY, NZ) && Nav.Get(NX, NY, NZ))
						{
							bAdjNav = true;
							break;
						}
					}
					if (bAdjNav)
					{
						Keep.Add(FInt3{X, Y, Z});
					}
				}
			}
		}
		Solid.Bits.Init(false, Solid.Bits.Num());
		for (const FInt3& C : Keep)
		{
			Solid.Set(C.X, C.Y, C.Z, true);
		}

		UE_LOG(LogTemp, Log, TEXT("GaussianProxy carve applied (free=%d, shell=%d)."), FreeCount, Keep.Num());
		return Keep.Num() > 0;
	}

	/**
	 * BFS connected components (6-neighbor). Drops islands smaller than MinVoxels.
	 * Call before Outdoor floor-fill so sparse floaters do not become tall solid pillars.
	 */
	static void RemoveSmallSolidIslands(TSet<FInt3>& Solid, int32 MinVoxels)
	{
		if (MinVoxels <= 1 || Solid.Num() == 0)
		{
			return;
		}

		const FInt3* Off = FaceNeighborOffsets();
		TSet<FInt3> Visited;
		Visited.Reserve(Solid.Num());
		TArray<FInt3> Stack;
		TArray<FInt3> Component;
		int32 DroppedComponents = 0;
		int32 DroppedVoxels = 0;

		for (const FInt3& Start : Solid)
		{
			if (Visited.Contains(Start))
			{
				continue;
			}

			Component.Reset();
			Stack.Reset();
			Stack.Add(Start);
			Visited.Add(Start);

			while (Stack.Num() > 0)
			{
				const FInt3 Cur = Stack.Pop(EAllowShrinking::No);
				Component.Add(Cur);
				for (int32 i = 0; i < 6; ++i)
				{
					const FInt3 N{Cur.X + Off[i].X, Cur.Y + Off[i].Y, Cur.Z + Off[i].Z};
					if (Solid.Contains(N) && !Visited.Contains(N))
					{
						Visited.Add(N);
						Stack.Add(N);
					}
				}
			}

			if (Component.Num() < MinVoxels)
			{
				++DroppedComponents;
				DroppedVoxels += Component.Num();
				for (const FInt3& V : Component)
				{
					Solid.Remove(V);
				}
			}
		}

		if (DroppedComponents > 0)
		{
			UE_LOG(LogTemp, Log,
				TEXT("GaussianProxy: removed %d small solid islands (%d voxels, min=%d)."),
				DroppedComponents, DroppedVoxels, MinVoxels);
		}
	}

	/**
	 * Keep a single solid component: the one containing PreferredSeed (if solid / nearest),
	 * otherwise the largest by voxel count. Removes distant clutter clouds after cleanup/fill.
	 */
	static void KeepPrimarySolidComponent(TSet<FInt3>& Solid, const FInt3* PreferredSeed)
	{
		if (Solid.Num() == 0)
		{
			return;
		}

		const FInt3* Off = FaceNeighborOffsets();
		TSet<FInt3> Visited;
		Visited.Reserve(Solid.Num());
		TArray<FInt3> Stack;
		TArray<TArray<FInt3>> Components;
		Components.Reserve(32);

		for (const FInt3& Start : Solid)
		{
			if (Visited.Contains(Start))
			{
				continue;
			}

			TArray<FInt3> Component;
			Stack.Reset();
			Stack.Add(Start);
			Visited.Add(Start);
			while (Stack.Num() > 0)
			{
				const FInt3 Cur = Stack.Pop(EAllowShrinking::No);
				Component.Add(Cur);
				for (int32 i = 0; i < 6; ++i)
				{
					const FInt3 N{Cur.X + Off[i].X, Cur.Y + Off[i].Y, Cur.Z + Off[i].Z};
					if (Solid.Contains(N) && !Visited.Contains(N))
					{
						Visited.Add(N);
						Stack.Add(N);
					}
				}
			}
			Components.Add(MoveTemp(Component));
		}

		if (Components.Num() <= 1)
		{
			return;
		}

		int32 BestIndex = 0;
		int32 BestScore = Components[0].Num();
		for (int32 i = 1; i < Components.Num(); ++i)
		{
			if (Components[i].Num() > BestScore)
			{
				BestScore = Components[i].Num();
				BestIndex = i;
			}
		}

		// Prefer the component that contains (or is nearest to) the fill/carve seed.
		if (PreferredSeed)
		{
			const FInt3 Seed = *PreferredSeed;
			int32 SeedComponent = INDEX_NONE;
			int64 BestDistSq = TNumericLimits<int64>::Max();
			for (int32 Ci = 0; Ci < Components.Num(); ++Ci)
			{
				for (const FInt3& V : Components[Ci])
				{
					if (V == Seed)
					{
						SeedComponent = Ci;
						BestDistSq = 0;
						break;
					}
					const int64 DX = static_cast<int64>(V.X) - Seed.X;
					const int64 DY = static_cast<int64>(V.Y) - Seed.Y;
					const int64 DZ = static_cast<int64>(V.Z) - Seed.Z;
					const int64 DistSq = DX * DX + DY * DY + DZ * DZ;
					if (DistSq < BestDistSq)
					{
						BestDistSq = DistSq;
						SeedComponent = Ci;
					}
				}
				if (BestDistSq == 0)
				{
					break;
				}
			}
			// Only switch away from largest if seed is reasonably close (within ~32 voxels).
			if (SeedComponent != INDEX_NONE && BestDistSq <= (32ll * 32ll))
			{
				BestIndex = SeedComponent;
			}
		}

		Solid.Reset();
		Solid.Reserve(Components[BestIndex].Num());
		for (const FInt3& V : Components[BestIndex])
		{
			Solid.Add(V);
		}

		UE_LOG(LogTemp, Log,
			TEXT("GaussianProxy: kept primary solid component %d/%d voxels (was %d components)."),
			Solid.Num(), BestScore, Components.Num());
	}

	/** Keep only solid cells that touch empty — thins exterior bulk after external-fill without carve. */
	static void CompactToEmptyAdjacentShell(FDenseVoxelBits& Solid)
	{
		const FInt3* Off = FaceNeighborOffsets();
		TArray<FInt3> Keep;
		for (int32 Z = 0; Z < Solid.NZ; ++Z)
		{
			for (int32 Y = 0; Y < Solid.NY; ++Y)
			{
				for (int32 X = 0; X < Solid.NX; ++X)
				{
					if (!Solid.Get(X, Y, Z))
					{
						continue;
					}
					bool bTouchEmpty = false;
					for (int32 i = 0; i < 6; ++i)
					{
						const int32 NX = X + Off[i].X;
						const int32 NY = Y + Off[i].Y;
						const int32 NZ = Z + Off[i].Z;
						if (!Solid.InBounds(NX, NY, NZ) || !Solid.Get(NX, NY, NZ))
						{
							// Interior empty only: OOB counts as exterior, still touch empty.
							if (Solid.InBounds(NX, NY, NZ) && !Solid.Get(NX, NY, NZ))
							{
								bTouchEmpty = true;
								break;
							}
						}
					}
					if (bTouchEmpty)
					{
						Keep.Add(FInt3{X, Y, Z});
					}
				}
			}
		}
		Solid.Bits.Init(false, Solid.Bits.Num());
		for (const FInt3& C : Keep)
		{
			Solid.Set(C.X, C.Y, C.Z, true);
		}
	}

	static void ApplySceneTypeFill(
		TSet<FInt3>& Solid,
		const FGaussianProxyMeshBuildSettings& Settings,
		float VoxelSize,
		const FVector& GridOrigin,
		int32 DimX,
		int32 DimY,
		int32 DimZ)
	{
		if (Settings.SceneType == EGaussianProxySceneType::SingleObject)
		{
			return;
		}

		const int64 Cells = static_cast<int64>(DimX) * DimY * DimZ;
		// Dense path cap (~48M cells ≈ 6MB bitset). Larger scenes rely on auto-grown voxels.
		constexpr int64 MaxDenseCells = 48ll * 1024 * 1024;
		if (Cells <= 0 || Cells > MaxDenseCells)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("GaussianProxy fill/carve skipped: dense grid too large (%lld cells). Increase Voxel Size."),
				Cells);
			return;
		}

		FDenseVoxelBits Dense;
		Dense.Init(DimX, DimY, DimZ, false);
		Dense.FromSolidSet(Solid);

		const int32 SealRings = FMath::Max(1, FMath::CeilToInt(Settings.FillSealSizeCm / FMath::Max(VoxelSize, 1.0f)));
		const FVector Seed = Settings.FillSeedPosition;
		const FInt3 SeedCell{
			FMath::FloorToInt((Seed.X - GridOrigin.X) / VoxelSize),
			FMath::FloorToInt((Seed.Y - GridOrigin.Y) / VoxelSize),
			FMath::FloorToInt((Seed.Z - GridOrigin.Z) / VoxelSize)
		};

		if (Settings.SceneType == EGaussianProxySceneType::Room)
		{
			const bool bFilled = ApplyExternalFillDense(Dense, SealRings, SeedCell);
			if (Settings.bEnableCarve && Settings.CarveHeightCm > 0.0f)
			{
				const int32 KernelR = FMath::Max(0, FMath::CeilToInt(Settings.CarveRadiusCm / VoxelSize));
				const int32 ZHalf = FMath::Max(1, FMath::CeilToInt(Settings.CarveHeightCm / (2.0f * VoxelSize)));
				if (!ApplyCarveDense(Dense, KernelR, ZHalf, SeedCell) && bFilled)
				{
					// Fall back to interior-facing shell after external fill.
					CompactToEmptyAdjacentShell(Dense);
				}
			}
			else if (bFilled)
			{
				CompactToEmptyAdjacentShell(Dense);
			}
		}
		else if (Settings.SceneType == EGaussianProxySceneType::Outdoor)
		{
			const int32 RadiusCells = FMath::Max(1, FMath::CeilToInt((2.0f * Settings.FillSealSizeCm) / FMath::Max(VoxelSize, 1.0f)));
			ApplyFloorFillDense(Dense, RadiusCells);
			if (Settings.bEnableCarve && Settings.CarveHeightCm > 0.0f)
			{
				const int32 KernelR = FMath::Max(0, FMath::CeilToInt(Settings.CarveRadiusCm / VoxelSize));
				const int32 ZHalf = FMath::Max(1, FMath::CeilToInt(Settings.CarveHeightCm / (2.0f * VoxelSize)));
				ApplyCarveDense(Dense, KernelR, ZHalf, SeedCell);
			}
		}

		Dense.ToSolidSet(Solid);
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
	 * Intermediate welded surface mesh (shared corners across voxel faces).
	 * Required for Laplacian — duplicate per-face verts have no neighborhood.
	 */
	struct FProxySimpleMesh
	{
		TArray<FVector3f> Positions;
		/** Triangle indices, 3 per tri, UE CW front when viewed from outside. */
		TArray<int32> Indices;
		/** Geometric outward normal per triangle (pre-smooth). */
		TArray<FVector3f> TriNormals;
	};

	/** Lattice corner key: world = GridOrigin + Lattice * VoxelSize. */
	static FInt3 CornerLatticeKey(const FInt3& Cell, float OffX, float OffY, float OffZ)
	{
		return FInt3{
			Cell.X + (OffX > 0.0f ? 1 : 0),
			Cell.Y + (OffY > 0.0f ? 1 : 0),
			Cell.Z + (OffZ > 0.0f ? 1 : 0)
		};
	}

	static void AppendQuadWelded(
		FProxySimpleMesh& Mesh,
		TMap<FInt3, int32>& LatticeToIndex,
		const FVector& GridOrigin,
		float VoxelSize,
		const FInt3& LatticeA,
		const FInt3& LatticeB,
		const FInt3& LatticeC,
		const FInt3& LatticeD,
		const FVector3f& OutwardNormal)
	{
		auto GetOrAdd = [&](const FInt3& L) -> int32
		{
			if (const int32* Found = LatticeToIndex.Find(L))
			{
				return *Found;
			}
			const FVector3f P(
				GridOrigin.X + static_cast<float>(L.X) * VoxelSize,
				GridOrigin.Y + static_cast<float>(L.Y) * VoxelSize,
				GridOrigin.Z + static_cast<float>(L.Z) * VoxelSize);
			const int32 Idx = Mesh.Positions.Add(P);
			LatticeToIndex.Add(L, Idx);
			return Idx;
		};

		int32 IA = GetOrAdd(LatticeA);
		int32 IB = GetOrAdd(LatticeB);
		int32 IC = GetOrAdd(LatticeC);
		int32 ID = GetOrAdd(LatticeD);

		const FVector3f PA = Mesh.Positions[IA];
		const FVector3f PB = Mesh.Positions[IB];
		const FVector3f PC = Mesh.Positions[IC];
		// RH Cross matching outward ⇒ CCW from outside; UE wants CW front ⇒ swap B/D.
		const FVector3f GeoN = FVector3f::CrossProduct(PB - PA, PC - PA);
		if (FVector3f::DotProduct(GeoN, OutwardNormal) > 0.0f)
		{
			Swap(IB, ID);
		}

		const FVector3f N = OutwardNormal.GetSafeNormal();
		// Tris: A-B-C and A-C-D
		Mesh.Indices.Add(IA);
		Mesh.Indices.Add(IB);
		Mesh.Indices.Add(IC);
		Mesh.TriNormals.Add(N);
		Mesh.Indices.Add(IA);
		Mesh.Indices.Add(IC);
		Mesh.Indices.Add(ID);
		Mesh.TriNormals.Add(N);
	}

	static void BuildExposedFaceSimpleMesh(
		const TSet<FInt3>& Solid,
		float VoxelSize,
		const FVector& GridOrigin,
		FProxySimpleMesh& OutMesh)
	{
		OutMesh.Positions.Reset();
		OutMesh.Indices.Reset();
		OutMesh.TriNormals.Reset();
		OutMesh.Positions.Reserve(Solid.Num() * 4);
		OutMesh.Indices.Reserve(Solid.Num() * 12);

		TMap<FInt3, int32> LatticeToIndex;
		LatticeToIndex.Reserve(Solid.Num() * 4);

		const float H = VoxelSize * 0.5f;
		const FInt3 Neigh[6] = {
			{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}
		};
		const FVector3f Outward[6] = {
			FVector3f(1, 0, 0), FVector3f(-1, 0, 0),
			FVector3f(0, 1, 0), FVector3f(0, -1, 0),
			FVector3f(0, 0, 1), FVector3f(0, 0, -1)
		};
		// Corner offsets from cell center (same as previous FaceQuads).
		const FVector3f FaceOff[6][4] = {
			{ FVector3f(H, -H, -H), FVector3f(H, H, -H), FVector3f(H, H, H), FVector3f(H, -H, H) },
			{ FVector3f(-H, -H, -H), FVector3f(-H, -H, H), FVector3f(-H, H, H), FVector3f(-H, H, -H) },
			{ FVector3f(-H, H, -H), FVector3f(-H, H, H), FVector3f(H, H, H), FVector3f(H, H, -H) },
			{ FVector3f(-H, -H, -H), FVector3f(H, -H, -H), FVector3f(H, -H, H), FVector3f(-H, -H, H) },
			{ FVector3f(-H, -H, H), FVector3f(H, -H, H), FVector3f(H, H, H), FVector3f(-H, H, H) },
			{ FVector3f(-H, -H, -H), FVector3f(-H, H, -H), FVector3f(H, H, -H), FVector3f(H, -H, -H) },
		};

		for (const FInt3& V : Solid)
		{
			for (int32 F = 0; F < 6; ++F)
			{
				const FInt3 Nbr{V.X + Neigh[F].X, V.Y + Neigh[F].Y, V.Z + Neigh[F].Z};
				if (Solid.Contains(Nbr))
				{
					continue;
				}
				const FInt3 LA = CornerLatticeKey(V, FaceOff[F][0].X, FaceOff[F][0].Y, FaceOff[F][0].Z);
				const FInt3 LB = CornerLatticeKey(V, FaceOff[F][1].X, FaceOff[F][1].Y, FaceOff[F][1].Z);
				const FInt3 LC = CornerLatticeKey(V, FaceOff[F][2].X, FaceOff[F][2].Y, FaceOff[F][2].Z);
				const FInt3 LD = CornerLatticeKey(V, FaceOff[F][3].X, FaceOff[F][3].Y, FaceOff[F][3].Z);
				AppendQuadWelded(OutMesh, LatticeToIndex, GridOrigin, VoxelSize, LA, LB, LC, LD, Outward[F]);
			}
		}
	}

	/**
	 * Taubin smooth: alternate λ / μ Laplacian steps to reduce shrink while rounding steps.
	 * Needs welded topology (shared verts across faces).
	 */
	static void TaubinSmoothSimpleMesh(
		FProxySimpleMesh& Mesh,
		int32 Iterations,
		float Lambda)
	{
		const int32 NumV = Mesh.Positions.Num();
		const int32 NumIdx = Mesh.Indices.Num();
		if (NumV < 3 || NumIdx < 3 || Iterations <= 0)
		{
			return;
		}

		const float L = FMath::Clamp(Lambda, 0.05f, 0.9f);
		// Classic Taubin μ slightly more negative than -λ to limit volume loss.
		const float Mu = -FMath::Clamp(L + 0.02f, 0.1f, 0.95f);

		TArray<TArray<int32>> Adj;
		Adj.SetNum(NumV);
		for (int32 i = 0; i < NumIdx; i += 3)
		{
			const int32 A = Mesh.Indices[i];
			const int32 B = Mesh.Indices[i + 1];
			const int32 C = Mesh.Indices[i + 2];
			if (A < 0 || B < 0 || C < 0 || A >= NumV || B >= NumV || C >= NumV)
			{
				continue;
			}
			Adj[A].AddUnique(B);
			Adj[A].AddUnique(C);
			Adj[B].AddUnique(A);
			Adj[B].AddUnique(C);
			Adj[C].AddUnique(A);
			Adj[C].AddUnique(B);
		}

		auto LaplacianPass = [&](float Weight)
		{
			TArray<FVector3f> Next = Mesh.Positions;
			for (int32 Vi = 0; Vi < NumV; ++Vi)
			{
				const TArray<int32>& Nbrs = Adj[Vi];
				if (Nbrs.Num() == 0)
				{
					continue;
				}
				FVector3f Avg = FVector3f::ZeroVector;
				for (const int32 N : Nbrs)
				{
					Avg += Mesh.Positions[N];
				}
				Avg /= static_cast<float>(Nbrs.Num());
				Next[Vi] = Mesh.Positions[Vi] + (Avg - Mesh.Positions[Vi]) * Weight;
			}
			Mesh.Positions = MoveTemp(Next);
		};

		for (int32 It = 0; It < Iterations; ++It)
		{
			LaplacianPass(L);
			LaplacianPass(Mu);
		}

		// Refresh triangle normals from smoothed positions.
		const int32 NumTris = NumIdx / 3;
		Mesh.TriNormals.SetNum(NumTris);
		for (int32 T = 0; T < NumTris; ++T)
		{
			const int32 I0 = Mesh.Indices[T * 3];
			const int32 I1 = Mesh.Indices[T * 3 + 1];
			const int32 I2 = Mesh.Indices[T * 3 + 2];
			const FVector3f P0 = Mesh.Positions[I0];
			const FVector3f P1 = Mesh.Positions[I1];
			const FVector3f P2 = Mesh.Positions[I2];
			// Submitted winding is UE CW-from-outside; geometric RH cross points inward.
			// Store outward normal for lighting = -RH.
			Mesh.TriNormals[T] = (-FVector3f::CrossProduct(P1 - P0, P2 - P0)).GetSafeNormal();
		}

		UE_LOG(LogTemp, Log,
			TEXT("GaussianProxy: Taubin smooth %d iters (lambda=%.2f) on %d verts / %d tris."),
			Iterations, L, NumV, NumTris);
	}

	static void WriteSimpleMeshToDescription(
		const FProxySimpleMesh& Mesh,
		FMeshDescription& MeshDescription)
	{
		FStaticMeshAttributes Attributes(MeshDescription);
		Attributes.Register();
		const FPolygonGroupID PolyGroup = MeshDescription.CreatePolygonGroup();
		TVertexAttributesRef<FVector3f> VertexPositions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector3f> VertexNormals = Attributes.GetVertexInstanceNormals();

		const int32 NumV = Mesh.Positions.Num();
		TArray<FVertexID> VertIDs;
		VertIDs.SetNum(NumV);
		for (int32 i = 0; i < NumV; ++i)
		{
			VertIDs[i] = MeshDescription.CreateVertex();
			VertexPositions[VertIDs[i]] = Mesh.Positions[i];
		}

		// Average triangle normals onto vertices for smoother lighting.
		TArray<FVector3f> AccNormal;
		TArray<int32> AccCount;
		AccNormal.Init(FVector3f::ZeroVector, NumV);
		AccCount.Init(0, NumV);
		const int32 NumTris = Mesh.Indices.Num() / 3;
		for (int32 T = 0; T < NumTris; ++T)
		{
			const FVector3f N = (T < Mesh.TriNormals.Num())
				? Mesh.TriNormals[T]
				: FVector3f::UpVector;
			for (int32 K = 0; K < 3; ++K)
			{
				const int32 Vi = Mesh.Indices[T * 3 + K];
				if (Vi >= 0 && Vi < NumV)
				{
					AccNormal[Vi] += N;
					AccCount[Vi]++;
				}
			}
		}
		TArray<FVector3f> VertNormal;
		VertNormal.SetNum(NumV);
		for (int32 i = 0; i < NumV; ++i)
		{
			VertNormal[i] = (AccCount[i] > 0)
				? (AccNormal[i] / static_cast<float>(AccCount[i])).GetSafeNormal()
				: FVector3f::UpVector;
		}

		for (int32 T = 0; T < NumTris; ++T)
		{
			const int32 I0 = Mesh.Indices[T * 3];
			const int32 I1 = Mesh.Indices[T * 3 + 1];
			const int32 I2 = Mesh.Indices[T * 3 + 2];
			if (I0 < 0 || I1 < 0 || I2 < 0 || I0 >= NumV || I1 >= NumV || I2 >= NumV)
			{
				continue;
			}
			const FVertexInstanceID VI0 = MeshDescription.CreateVertexInstance(VertIDs[I0]);
			const FVertexInstanceID VI1 = MeshDescription.CreateVertexInstance(VertIDs[I1]);
			const FVertexInstanceID VI2 = MeshDescription.CreateVertexInstance(VertIDs[I2]);
			VertexNormals[VI0] = VertNormal[I0];
			VertexNormals[VI1] = VertNormal[I1];
			VertexNormals[VI2] = VertNormal[I2];
			TArray<FVertexInstanceID, TFixedAllocator<3>> Tri;
			Tri.Add(VI0);
			Tri.Add(VI1);
			Tri.Add(VI2);
			MeshDescription.CreatePolygon(PolyGroup, Tri);
		}
	}

	static void ExtractExposedFaces(
		const TSet<FInt3>& Solid,
		float VoxelSize,
		const FVector& GridOrigin,
		FMeshDescription& MeshDescription,
		bool bSurfaceSmooth,
		int32 SmoothIterations,
		float SmoothLambda)
	{
		FProxySimpleMesh Simple;
		BuildExposedFaceSimpleMesh(Solid, VoxelSize, GridOrigin, Simple);
		if (bSurfaceSmooth && SmoothIterations > 0)
		{
			TaubinSmoothSimpleMesh(Simple, SmoothIterations, SmoothLambda);
		}
		WriteSimpleMeshToDescription(Simple, MeshDescription);
	}

	// --- Marching Cubes (classic edge/tri tables, binary occupancy isolevel 0.5) ---
	// Paul Bourke / public-domain MC tables (trimmed formatting).
	static const int32 GMcEdgeTable[256] = {
		0x0  , 0x109, 0x203, 0x30a, 0x406, 0x50f, 0x605, 0x70c,
		0x80c, 0x905, 0xa0f, 0xb06, 0xc0a, 0xd03, 0xe09, 0xf00,
		0x190, 0x99 , 0x393, 0x29a, 0x596, 0x49f, 0x795, 0x69c,
		0x99c, 0x895, 0xb9f, 0xa96, 0xd9a, 0xc93, 0xf99, 0xe90,
		0x230, 0x339, 0x33 , 0x13a, 0x636, 0x73f, 0x435, 0x53c,
		0xa3c, 0xb35, 0x83f, 0x936, 0xe3a, 0xf33, 0xc39, 0xd30,
		0x3a0, 0x2a9, 0x1a3, 0xaa , 0x7a6, 0x6af, 0x5a5, 0x4ac,
		0xbac, 0xaa5, 0x9af, 0x8a6, 0xfaa, 0xea3, 0xda9, 0xca0,
		0x460, 0x569, 0x663, 0x76a, 0x66 , 0x16f, 0x265, 0x36c,
		0xc6c, 0xd65, 0xe6f, 0xf66, 0x86a, 0x963, 0xa69, 0xb60,
		0x5f0, 0x4f9, 0x7f3, 0x6fa, 0x1f6, 0xff , 0x3f5, 0x2fc,
		0xdfc, 0xcf5, 0xfff, 0xef6, 0x9fa, 0x8f3, 0xbf9, 0xaf0,
		0x650, 0x759, 0x453, 0x55a, 0x256, 0x35f, 0x55 , 0x15c,
		0xe5c, 0xf55, 0xc5f, 0xd56, 0xa5a, 0xb53, 0x859, 0x950,
		0x7c0, 0x6c9, 0x5c3, 0x4ca, 0x3c6, 0x2cf, 0x1c5, 0xcc ,
		0xfcc, 0xec5, 0xdcf, 0xcc6, 0xbca, 0xac3, 0x9c9, 0x8c0,
		0x8c0, 0x9c9, 0xac3, 0xbca, 0xcc6, 0xdcf, 0xec5, 0xfcc,
		0xcc , 0x1c5, 0x2cf, 0x3c6, 0x4ca, 0x5c3, 0x6c9, 0x7c0,
		0x950, 0x859, 0xb53, 0xa5a, 0xd56, 0xc5f, 0xf55, 0xe5c,
		0x15c, 0x55 , 0x35f, 0x256, 0x55a, 0x453, 0x759, 0x650,
		0xaf0, 0xbf9, 0x8f3, 0x9fa, 0xef6, 0xfff, 0xcf5, 0xdfc,
		0x2fc, 0x3f5, 0xff , 0x1f6, 0x6fa, 0x7f3, 0x4f9, 0x5f0,
		0xb60, 0xa69, 0x963, 0x86a, 0xf66, 0xe6f, 0xd65, 0xc6c,
		0x36c, 0x265, 0x16f, 0x66 , 0x76a, 0x663, 0x569, 0x460,
		0xca0, 0xda9, 0xea3, 0xfaa, 0x8a6, 0x9af, 0xaa5, 0xbac,
		0x4ac, 0x5a5, 0x6af, 0x7a6, 0xaa , 0x1a3, 0x2a9, 0x3a0,
		0xd30, 0xc39, 0xf33, 0xe3a, 0x936, 0x83f, 0xb35, 0xa3c,
		0x53c, 0x435, 0x73f, 0x636, 0x13a, 0x33 , 0x339, 0x230,
		0xe90, 0xf99, 0xc93, 0xd9a, 0xa96, 0xb9f, 0x895, 0x99c,
		0x69c, 0x795, 0x49f, 0x596, 0x29a, 0x393, 0x99 , 0x190,
		0xf00, 0xe09, 0xd03, 0xc0a, 0xb06, 0xa0f, 0x905, 0x80c,
		0x70c, 0x605, 0x50f, 0x406, 0x30a, 0x203, 0x109, 0x0
	};

	static const int8 GMcTriTable[256][16] = {
		{-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{0,8,3,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{0,1,9,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{1,8,3,9,8,1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{1,2,10,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{0,8,3,1,2,10,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{9,2,10,0,2,9,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{2,8,3,2,10,8,10,9,8,-1,-1,-1,-1,-1,-1,-1},
		{3,11,2,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{0,11,2,8,11,0,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{1,9,0,2,3,11,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{1,11,2,1,9,11,9,8,11,-1,-1,-1,-1,-1,-1,-1},
		{3,10,1,11,10,3,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{0,10,1,0,8,10,8,11,10,-1,-1,-1,-1,-1,-1,-1},
		{3,9,0,3,11,9,11,10,9,-1,-1,-1,-1,-1,-1,-1},
		{9,8,10,10,8,11,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{4,7,8,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{4,3,0,7,3,4,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{0,1,9,8,4,7,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{4,1,9,4,7,1,7,3,1,-1,-1,-1,-1,-1,-1,-1},
		{1,2,10,8,4,7,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{3,4,7,3,0,4,1,2,10,-1,-1,-1,-1,-1,-1,-1},
		{9,2,10,9,0,2,8,4,7,-1,-1,-1,-1,-1,-1,-1},
		{2,10,9,2,9,7,2,7,3,7,9,4,-1,-1,-1,-1},
		{8,4,7,3,11,2,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{11,4,7,11,2,4,2,0,4,-1,-1,-1,-1,-1,-1,-1},
		{9,0,1,8,4,7,2,3,11,-1,-1,-1,-1,-1,-1,-1},
		{4,7,11,9,4,11,9,11,2,9,2,1,-1,-1,-1,-1},
		{3,10,1,3,11,10,7,8,4,-1,-1,-1,-1,-1,-1,-1},
		{1,11,10,1,4,11,1,0,4,7,11,4,-1,-1,-1,-1},
		{4,7,8,9,0,11,9,11,10,11,0,3,-1,-1,-1,-1},
		{4,7,11,4,11,9,9,11,10,-1,-1,-1,-1,-1,-1,-1},
		{9,5,4,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{9,5,4,0,8,3,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{0,5,4,1,5,0,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{8,5,4,8,3,5,3,1,5,-1,-1,-1,-1,-1,-1,-1},
		{1,2,10,9,5,4,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{3,0,8,1,2,10,4,9,5,-1,-1,-1,-1,-1,-1,-1},
		{5,2,10,5,4,2,4,0,2,-1,-1,-1,-1,-1,-1,-1},
		{2,10,5,3,2,5,3,5,4,3,4,8,-1,-1,-1,-1},
		{9,5,4,2,3,11,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{0,11,2,0,8,11,4,9,5,-1,-1,-1,-1,-1,-1,-1},
		{0,5,4,0,1,5,2,3,11,-1,-1,-1,-1,-1,-1,-1},
		{2,1,5,2,5,8,2,8,11,4,8,5,-1,-1,-1,-1},
		{10,3,11,10,1,3,9,5,4,-1,-1,-1,-1,-1,-1,-1},
		{4,9,5,0,8,1,8,10,1,8,11,10,-1,-1,-1,-1},
		{5,4,0,5,0,11,5,11,10,11,0,3,-1,-1,-1,-1},
		{5,4,8,5,8,10,10,8,11,-1,-1,-1,-1,-1,-1,-1},
		{9,7,8,5,7,9,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{9,3,0,9,5,3,5,7,3,-1,-1,-1,-1,-1,-1,-1},
		{0,7,8,0,1,7,1,5,7,-1,-1,-1,-1,-1,-1,-1},
		{1,5,3,3,5,7,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{9,7,8,9,5,7,10,1,2,-1,-1,-1,-1,-1,-1,-1},
		{10,1,2,9,5,0,5,3,0,5,7,3,-1,-1,-1,-1},
		{8,0,2,8,2,5,8,5,7,10,5,2,-1,-1,-1,-1},
		{2,10,5,2,5,3,3,5,7,-1,-1,-1,-1,-1,-1,-1},
		{7,9,5,7,8,9,3,11,2,-1,-1,-1,-1,-1,-1,-1},
		{9,5,7,9,7,2,9,2,0,2,7,11,-1,-1,-1,-1},
		{2,3,11,0,1,8,1,7,8,1,5,7,-1,-1,-1,-1},
		{11,2,1,11,1,7,7,1,5,-1,-1,-1,-1,-1,-1,-1},
		{9,5,8,8,5,7,10,1,3,10,3,11,-1,-1,-1,-1},
		{5,7,0,5,0,9,7,11,0,1,0,10,11,10,0,-1},
		{11,10,0,11,0,3,10,5,0,8,0,7,5,7,0,-1},
		{11,10,5,7,11,5,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{10,6,5,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{0,8,3,5,10,6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{9,0,1,5,10,6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{1,8,3,1,9,8,5,10,6,-1,-1,-1,-1,-1,-1,-1},
		{1,6,5,2,6,1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{1,6,5,1,2,6,3,0,8,-1,-1,-1,-1,-1,-1,-1},
		{9,6,5,9,0,6,0,2,6,-1,-1,-1,-1,-1,-1,-1},
		{5,9,8,5,8,2,5,2,6,3,2,8,-1,-1,-1,-1},
		{2,3,11,10,6,5,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{11,0,8,11,2,0,10,6,5,-1,-1,-1,-1,-1,-1,-1},
		{0,1,9,2,3,11,5,10,6,-1,-1,-1,-1,-1,-1,-1},
		{5,10,6,1,9,2,9,11,2,9,8,11,-1,-1,-1,-1},
		{6,3,11,6,5,3,5,1,3,-1,-1,-1,-1,-1,-1,-1},
		{0,8,11,0,11,5,0,5,1,5,11,6,-1,-1,-1,-1},
		{3,11,6,0,3,6,0,6,5,0,5,9,-1,-1,-1,-1},
		{6,5,9,6,9,11,11,9,8,-1,-1,-1,-1,-1,-1,-1},
		{5,10,6,4,7,8,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{4,3,0,4,7,3,6,5,10,-1,-1,-1,-1,-1,-1,-1},
		{1,9,0,5,10,6,8,4,7,-1,-1,-1,-1,-1,-1,-1},
		{10,6,5,1,9,7,1,7,3,7,9,4,-1,-1,-1,-1},
		{6,1,2,6,5,1,4,7,8,-1,-1,-1,-1,-1,-1,-1},
		{1,2,5,5,2,6,3,0,4,3,4,7,-1,-1,-1,-1},
		{8,4,7,9,0,5,0,6,5,0,2,6,-1,-1,-1,-1},
		{7,3,9,7,9,4,3,2,9,5,9,6,2,6,9,-1},
		{3,11,2,7,8,4,10,6,5,-1,-1,-1,-1,-1,-1,-1},
		{5,10,6,4,7,2,4,2,0,2,7,11,-1,-1,-1,-1},
		{0,1,9,4,7,8,2,3,11,5,10,6,-1,-1,-1,-1},
		{9,2,1,9,11,2,9,4,11,7,11,4,5,10,6,-1},
		{8,4,7,3,11,5,3,5,1,5,11,6,-1,-1,-1,-1},
		{5,1,11,5,11,6,1,0,11,7,11,4,0,4,11,-1},
		{0,5,9,0,6,5,0,3,6,11,6,3,8,4,7,-1},
		{6,5,9,6,9,11,4,7,9,7,11,9,-1,-1,-1,-1},
		{10,4,9,6,4,10,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{4,10,6,4,9,10,0,8,3,-1,-1,-1,-1,-1,-1,-1},
		{10,0,1,10,6,0,6,4,0,-1,-1,-1,-1,-1,-1,-1},
		{8,3,1,8,1,6,8,6,4,6,1,10,-1,-1,-1,-1},
		{1,4,9,1,2,4,2,6,4,-1,-1,-1,-1,-1,-1,-1},
		{3,0,8,1,2,9,2,4,9,2,6,4,-1,-1,-1,-1},
		{0,2,4,4,2,6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{8,3,2,8,2,4,4,2,6,-1,-1,-1,-1,-1,-1,-1},
		{10,4,9,10,6,4,11,2,3,-1,-1,-1,-1,-1,-1,-1},
		{0,8,2,2,8,11,4,9,10,4,10,6,-1,-1,-1,-1},
		{3,11,2,0,1,6,0,6,4,6,1,10,-1,-1,-1,-1},
		{6,4,1,6,1,10,4,8,1,2,1,11,8,11,1,-1},
		{9,6,4,9,3,6,9,1,3,11,6,3,-1,-1,-1,-1},
		{8,11,1,8,1,0,11,6,1,9,1,4,6,4,1,-1},
		{3,11,6,3,6,0,0,6,4,-1,-1,-1,-1,-1,-1,-1},
		{6,4,8,11,6,8,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{7,10,6,7,8,10,8,9,10,-1,-1,-1,-1,-1,-1,-1},
		{0,7,3,0,10,7,0,9,10,6,7,10,-1,-1,-1,-1},
		{10,6,7,1,10,7,1,7,8,1,8,0,-1,-1,-1,-1},
		{10,6,7,10,7,1,1,7,3,-1,-1,-1,-1,-1,-1,-1},
		{1,2,6,1,6,8,1,8,9,8,6,7,-1,-1,-1,-1},
		{2,6,9,2,9,1,6,7,9,0,9,3,7,3,9,-1},
		{7,8,0,7,0,6,6,0,2,-1,-1,-1,-1,-1,-1,-1},
		{7,3,2,6,7,2,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{2,3,11,10,6,8,10,8,9,8,6,7,-1,-1,-1,-1},
		{2,0,7,2,7,11,0,9,7,6,7,10,9,10,7,-1},
		{1,8,0,1,7,8,1,10,7,6,7,10,2,3,11,-1},
		{11,2,1,11,1,7,10,6,1,6,7,1,-1,-1,-1,-1},
		{8,9,6,8,6,7,9,1,6,11,6,3,1,3,6,-1},
		{0,9,1,11,6,7,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{7,8,0,7,0,6,3,11,0,11,6,0,-1,-1,-1,-1},
		{7,11,6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{7,6,11,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{3,0,8,11,7,6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{0,1,9,11,7,6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{8,1,9,8,3,1,11,7,6,-1,-1,-1,-1,-1,-1,-1},
		{10,1,2,6,11,7,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{1,2,10,3,0,8,6,11,7,-1,-1,-1,-1,-1,-1,-1},
		{2,9,0,2,10,9,6,11,7,-1,-1,-1,-1,-1,-1,-1},
		{6,11,7,2,10,3,10,8,3,10,9,8,-1,-1,-1,-1},
		{7,2,3,6,2,7,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{7,0,8,7,6,0,6,2,0,-1,-1,-1,-1,-1,-1,-1},
		{2,7,6,2,3,7,0,1,9,-1,-1,-1,-1,-1,-1,-1},
		{1,6,2,1,8,6,1,9,8,8,7,6,-1,-1,-1,-1},
		{10,7,6,10,1,7,1,3,7,-1,-1,-1,-1,-1,-1,-1},
		{10,7,6,1,7,10,1,8,7,1,0,8,-1,-1,-1,-1},
		{0,3,7,0,7,10,0,10,9,6,10,7,-1,-1,-1,-1},
		{7,6,10,7,10,8,8,10,9,-1,-1,-1,-1,-1,-1,-1},
		{6,8,4,11,8,6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{3,6,11,3,0,6,0,4,6,-1,-1,-1,-1,-1,-1,-1},
		{8,6,11,8,4,6,9,0,1,-1,-1,-1,-1,-1,-1,-1},
		{9,4,6,9,6,3,9,3,1,11,3,6,-1,-1,-1,-1},
		{6,8,4,6,11,8,2,10,1,-1,-1,-1,-1,-1,-1,-1},
		{1,2,10,3,0,11,0,6,11,0,4,6,-1,-1,-1,-1},
		{4,11,8,4,6,11,0,2,9,2,10,9,-1,-1,-1,-1},
		{10,9,3,10,3,2,9,4,3,11,3,6,4,6,3,-1},
		{8,2,3,8,4,2,4,6,2,-1,-1,-1,-1,-1,-1,-1},
		{0,4,2,4,6,2,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{1,9,0,2,3,4,2,4,6,4,3,8,-1,-1,-1,-1},
		{1,9,4,1,4,2,2,4,6,-1,-1,-1,-1,-1,-1,-1},
		{8,1,3,8,6,1,8,4,6,6,10,1,-1,-1,-1,-1},
		{10,1,0,10,0,6,6,0,4,-1,-1,-1,-1,-1,-1,-1},
		{4,6,3,4,3,8,6,10,3,0,3,9,10,9,3,-1},
		{10,9,4,6,10,4,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{4,9,5,7,6,11,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{0,8,3,4,9,5,11,7,6,-1,-1,-1,-1,-1,-1,-1},
		{5,0,1,5,4,0,7,6,11,-1,-1,-1,-1,-1,-1,-1},
		{11,7,6,8,3,4,3,5,4,3,1,5,-1,-1,-1,-1},
		{9,5,4,10,1,2,7,6,11,-1,-1,-1,-1,-1,-1,-1},
		{6,11,7,1,2,10,0,8,3,4,9,5,-1,-1,-1,-1},
		{7,6,11,5,4,10,4,2,10,4,0,2,-1,-1,-1,-1},
		{3,4,8,3,5,4,3,2,5,10,5,2,11,7,6,-1},
		{7,2,3,7,6,2,5,4,9,-1,-1,-1,-1,-1,-1,-1},
		{9,5,4,0,8,6,0,6,2,6,8,7,-1,-1,-1,-1},
		{3,6,2,3,7,6,1,5,0,5,4,0,-1,-1,-1,-1},
		{6,2,8,6,8,7,2,1,8,4,8,5,1,5,8,-1},
		{9,5,4,10,1,6,1,7,6,1,3,7,-1,-1,-1,-1},
		{1,6,10,1,7,6,1,0,7,8,7,0,9,5,4,-1},
		{4,0,10,4,10,5,0,3,10,6,10,7,3,7,10,-1},
		{7,6,10,7,10,8,5,4,10,4,8,10,-1,-1,-1,-1},
		{6,9,5,6,11,9,11,8,9,-1,-1,-1,-1,-1,-1,-1},
		{3,6,11,0,6,3,0,5,6,0,9,5,-1,-1,-1,-1},
		{0,11,8,0,5,11,0,1,5,5,6,11,-1,-1,-1,-1},
		{6,11,3,6,3,5,5,3,1,-1,-1,-1,-1,-1,-1,-1},
		{1,2,10,9,5,11,9,11,8,11,5,6,-1,-1,-1,-1},
		{0,11,3,0,6,11,0,9,6,5,6,9,1,2,10,-1},
		{11,8,5,11,5,6,8,0,5,10,5,2,0,2,5,-1},
		{6,11,3,6,3,5,2,10,3,10,5,3,-1,-1,-1,-1},
		{5,8,9,5,2,8,5,6,2,3,8,2,-1,-1,-1,-1},
		{9,5,6,9,6,0,0,6,2,-1,-1,-1,-1,-1,-1,-1},
		{1,5,8,1,8,0,5,6,8,3,8,2,6,2,8,-1},
		{1,5,6,2,1,6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{1,3,6,1,6,10,3,8,6,5,6,9,8,9,6,-1},
		{10,1,0,10,0,6,9,5,0,5,6,0,-1,-1,-1,-1},
		{0,3,8,5,6,10,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{10,5,6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{11,5,10,7,5,11,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{11,5,10,11,7,5,8,3,0,-1,-1,-1,-1,-1,-1,-1},
		{5,11,7,5,10,11,1,9,0,-1,-1,-1,-1,-1,-1,-1},
		{10,7,5,10,11,7,9,8,1,8,3,1,-1,-1,-1,-1},
		{11,1,2,11,7,1,7,5,1,-1,-1,-1,-1,-1,-1,-1},
		{0,8,3,1,2,7,1,7,5,7,2,11,-1,-1,-1,-1},
		{9,7,5,9,2,7,9,0,2,2,11,7,-1,-1,-1,-1},
		{7,5,2,7,2,11,5,9,2,3,2,8,9,8,2,-1},
		{2,5,10,2,3,5,3,7,5,-1,-1,-1,-1,-1,-1,-1},
		{8,2,0,8,5,2,8,7,5,10,2,5,-1,-1,-1,-1},
		{9,0,1,5,10,3,5,3,7,3,10,2,-1,-1,-1,-1},
		{9,8,2,9,2,1,8,7,2,10,2,5,7,5,2,-1},
		{1,3,5,3,7,5,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{0,8,7,0,7,1,1,7,5,-1,-1,-1,-1,-1,-1,-1},
		{9,0,3,9,3,5,5,3,7,-1,-1,-1,-1,-1,-1,-1},
		{9,8,7,5,9,7,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{5,8,4,5,10,8,10,11,8,-1,-1,-1,-1,-1,-1,-1},
		{5,0,4,5,11,0,5,10,11,11,3,0,-1,-1,-1,-1},
		{0,1,9,8,4,10,8,10,11,10,4,5,-1,-1,-1,-1},
		{10,11,4,10,4,5,11,3,4,9,4,1,3,1,4,-1},
		{2,5,1,2,8,5,2,11,8,4,5,8,-1,-1,-1,-1},
		{0,4,11,0,11,3,4,5,11,2,11,1,5,1,11,-1},
		{0,2,5,0,5,9,2,11,5,4,5,8,11,8,5,-1},
		{9,4,5,2,11,3,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{2,5,10,3,5,2,3,4,5,3,8,4,-1,-1,-1,-1},
		{5,10,2,5,2,4,4,2,0,-1,-1,-1,-1,-1,-1,-1},
		{3,10,2,3,5,10,3,8,5,4,5,8,0,1,9,-1},
		{5,10,2,5,2,4,1,9,2,9,4,2,-1,-1,-1,-1},
		{8,4,5,8,5,3,3,5,1,-1,-1,-1,-1,-1,-1,-1},
		{0,4,5,1,0,5,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{8,4,5,8,5,3,9,0,5,0,3,5,-1,-1,-1,-1},
		{9,4,5,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{4,11,7,4,9,11,9,10,11,-1,-1,-1,-1,-1,-1,-1},
		{0,8,3,4,9,7,9,11,7,9,10,11,-1,-1,-1,-1},
		{1,10,11,1,11,4,1,4,0,7,4,11,-1,-1,-1,-1},
		{3,1,4,3,4,8,1,10,4,7,4,11,10,11,4,-1},
		{4,11,7,9,11,4,9,2,11,9,1,2,-1,-1,-1,-1},
		{9,7,4,9,11,7,9,1,11,2,11,1,0,8,3,-1},
		{11,7,4,11,4,2,2,4,0,-1,-1,-1,-1,-1,-1,-1},
		{11,7,4,11,4,2,8,3,4,3,2,4,-1,-1,-1,-1},
		{2,9,10,2,7,9,2,3,7,7,4,9,-1,-1,-1,-1},
		{9,10,7,9,7,4,10,2,7,8,7,0,2,0,7,-1},
		{3,7,10,3,10,2,7,4,10,1,10,0,4,0,10,-1},
		{1,10,2,8,7,4,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{4,9,1,4,1,7,7,1,3,-1,-1,-1,-1,-1,-1,-1},
		{4,9,1,4,1,7,0,8,1,8,7,1,-1,-1,-1,-1},
		{4,0,3,7,4,3,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{4,8,7,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{9,10,8,10,11,8,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{3,0,9,3,9,11,11,9,10,-1,-1,-1,-1,-1,-1,-1},
		{0,1,10,0,10,8,8,10,11,-1,-1,-1,-1,-1,-1,-1},
		{3,1,10,11,3,10,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{1,2,11,1,11,9,9,11,8,-1,-1,-1,-1,-1,-1,-1},
		{3,0,9,3,9,11,1,2,9,2,11,9,-1,-1,-1,-1},
		{0,2,11,8,0,11,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{3,2,11,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{2,3,8,2,8,10,10,8,9,-1,-1,-1,-1,-1,-1,-1},
		{9,10,2,0,9,2,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{2,3,8,2,8,10,0,1,8,1,10,8,-1,-1,-1,-1},
		{1,10,2,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{1,3,8,9,1,8,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{0,9,1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{0,3,8,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
		{-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}
	};

	static FVector3f McCornerWorld(const FInt3& Cube, int32 Corner, float VoxelSize, const FVector& GridOrigin)
	{
		// Cube corners: 0:(0,0,0) 1:(1,0,0) 2:(1,1,0) 3:(0,1,0) 4:(0,0,1) 5:(1,0,1) 6:(1,1,1) 7:(0,1,1)
		static const int32 OX[8] = {0,1,1,0, 0,1,1,0};
		static const int32 OY[8] = {0,0,1,1, 0,0,1,1};
		static const int32 OZ[8] = {0,0,0,0, 1,1,1,1};
		const float X = GridOrigin.X + static_cast<float>(Cube.X + OX[Corner]) * VoxelSize;
		const float Y = GridOrigin.Y + static_cast<float>(Cube.Y + OY[Corner]) * VoxelSize;
		const float Z = GridOrigin.Z + static_cast<float>(Cube.Z + OZ[Corner]) * VoxelSize;
		return FVector3f(X, Y, Z);
	}

	static bool McCornerSolid(const TSet<FInt3>& Solid, const FInt3& Cube, int32 Corner)
	{
		static const int32 OX[8] = {0,1,1,0, 0,1,1,0};
		static const int32 OY[8] = {0,0,1,1, 0,0,1,1};
		static const int32 OZ[8] = {0,0,0,0, 1,1,1,1};
		return Solid.Contains(FInt3{Cube.X + OX[Corner], Cube.Y + OY[Corner], Cube.Z + OZ[Corner]});
	}

	/**
	 * Unique key for an undirected unit lattice edge (MC always spans adjacent cells).
	 * Packs min endpoint (20 bits/axis, bias for negatives) + axis (X=0,Y=1,Z=2).
	 */
	static uint64 McEdgeKey(int32 X0, int32 Y0, int32 Z0, int32 X1, int32 Y1, int32 Z1)
	{
		if (X1 < X0 || (X1 == X0 && Y1 < Y0) || (X1 == X0 && Y1 == Y0 && Z1 < Z0))
		{
			Swap(X0, X1);
			Swap(Y0, Y1);
			Swap(Z0, Z1);
		}
		const int32 Axis = (X1 != X0) ? 0 : ((Y1 != Y0) ? 1 : 2);
		const uint32 BX = static_cast<uint32>(X0 + 0x80000) & 0xFFFFF;
		const uint32 BY = static_cast<uint32>(Y0 + 0x80000) & 0xFFFFF;
		const uint32 BZ = static_cast<uint32>(Z0 + 0x80000) & 0xFFFFF;
		return (static_cast<uint64>(Axis & 3) << 60)
			| (static_cast<uint64>(BX) << 40)
			| (static_cast<uint64>(BY) << 20)
			| static_cast<uint64>(BZ);
	}

	static void ExtractMarchingCubes(
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

		// Edge endpoint corner pairs (standard MC edge index 0..11).
		static const int32 EdgeCorners[12][2] = {
			{0,1},{1,2},{2,3},{3,0},
			{4,5},{5,6},{6,7},{7,4},
			{0,4},{1,5},{2,6},{3,7}
		};
		static const int32 CornerOX[8] = {0,1,1,0, 0,1,1,0};
		static const int32 CornerOY[8] = {0,0,1,1, 0,0,1,1};
		static const int32 CornerOZ[8] = {0,0,0,0, 1,1,1,1};

		// Process each cube that has at least one solid corner (unique origins).
		TSet<FInt3> Cubes;
		Cubes.Reserve(Solid.Num() * 2);
		for (const FInt3& V : Solid)
		{
			for (int32 DX = -1; DX <= 0; ++DX)
			{
				for (int32 DY = -1; DY <= 0; ++DY)
				{
					for (int32 DZ = -1; DZ <= 0; ++DZ)
					{
						Cubes.Add(FInt3{V.X + DX, V.Y + DY, V.Z + DZ});
					}
				}
			}
		}

		TMap<uint64, FVertexID> EdgeVertices;
		EdgeVertices.Reserve(Cubes.Num() * 2);

		auto GetOrCreateEdgeVertex = [&](const FInt3& Cube, int32 EdgeIndex) -> FVertexID
		{
			const int32 C0 = EdgeCorners[EdgeIndex][0];
			const int32 C1 = EdgeCorners[EdgeIndex][1];
			const int32 X0 = Cube.X + CornerOX[C0];
			const int32 Y0 = Cube.Y + CornerOY[C0];
			const int32 Z0 = Cube.Z + CornerOZ[C0];
			const int32 X1 = Cube.X + CornerOX[C1];
			const int32 Y1 = Cube.Y + CornerOY[C1];
			const int32 Z1 = Cube.Z + CornerOZ[C1];
			const uint64 Key = McEdgeKey(X0, Y0, Z0, X1, Y1, Z1);
			if (const FVertexID* Existing = EdgeVertices.Find(Key))
			{
				return *Existing;
			}
			// Binary field: mid-edge (isolevel 0.5).
			const FVector3f P0 = McCornerWorld(Cube, C0, VoxelSize, GridOrigin);
			const FVector3f P1 = McCornerWorld(Cube, C1, VoxelSize, GridOrigin);
			const FVertexID VID = MeshDescription.CreateVertex();
			VertexPositions[VID] = (P0 + P1) * 0.5f;
			EdgeVertices.Add(Key, VID);
			return VID;
		};

		int32 TriCount = 0;
		for (const FInt3& Cube : Cubes)
		{
			int32 CubeIndex = 0;
			for (int32 C = 0; C < 8; ++C)
			{
				if (McCornerSolid(Solid, Cube, C))
				{
					CubeIndex |= (1 << C);
				}
			}
			const int32 Edges = GMcEdgeTable[CubeIndex];
			if (Edges == 0)
			{
				continue;
			}

			FVertexID EdgeVert[12];
			for (int32 E = 0; E < 12; ++E)
			{
				if (Edges & (1 << E))
				{
					EdgeVert[E] = GetOrCreateEdgeVertex(Cube, E);
				}
			}

			for (int32 i = 0; GMcTriTable[CubeIndex][i] != -1; i += 3)
			{
				const int32 E0 = GMcTriTable[CubeIndex][i];
				const int32 E1 = GMcTriTable[CubeIndex][i + 1];
				const int32 E2 = GMcTriTable[CubeIndex][i + 2];
				FVertexID V0 = EdgeVert[E0];
				FVertexID V1 = EdgeVert[E1];
				FVertexID V2 = EdgeVert[E2];
				const FVector3f P0 = VertexPositions[V0];
				const FVector3f P1 = VertexPositions[V1];
				const FVector3f P2 = VertexPositions[V2];
				FVector3f GeoN = FVector3f::CrossProduct(P1 - P0, P2 - P0).GetSafeNormal();
				// Prefer geometric normal toward empty (outward from solid).
				const FVector3f Centroid = (P0 + P1 + P2) / 3.0f;
				const FVector Probe = FVector(Centroid) + FVector(GeoN) * (VoxelSize * 0.25f);
				const FInt3 ProbeCell{
					FMath::FloorToInt((Probe.X - GridOrigin.X) / VoxelSize),
					FMath::FloorToInt((Probe.Y - GridOrigin.Y) / VoxelSize),
					FMath::FloorToInt((Probe.Z - GridOrigin.Z) / VoxelSize)
				};
				if (Solid.Contains(ProbeCell))
				{
					GeoN = -GeoN;
					Swap(V1, V2); // RH winding now matches outward
				}
				// UE front face = CW from outside. RH-outward is CCW from outside → flip once.
				Swap(V1, V2);
				const FVector3f N = GeoN; // keep lighting normals outward

				const FVertexInstanceID I0 = MeshDescription.CreateVertexInstance(V0);
				const FVertexInstanceID I1 = MeshDescription.CreateVertexInstance(V1);
				const FVertexInstanceID I2 = MeshDescription.CreateVertexInstance(V2);
				VertexNormals[I0] = N;
				VertexNormals[I1] = N;
				VertexNormals[I2] = N;
				TArray<FVertexInstanceID, TFixedAllocator<3>> Tri;
				Tri.Add(I0);
				Tri.Add(I1);
				Tri.Add(I2);
				MeshDescription.CreatePolygon(PolyGroup, Tri);
				++TriCount;
			}
		}

		UE_LOG(LogTemp, Log,
			TEXT("GaussianProxy: Marching Cubes produced %d triangles from %d solid voxels (%d cubes)."),
			TriCount, Solid.Num(), Cubes.Num());
	}
}

UStaticMesh* FGaussianProxyMeshGenerator::BuildMeshFromPoints(
	const TArray<FVector>& Points,
	const FGaussianProxyMeshBuildSettings& Settings,
	const FString& AssetName,
	FString& OutError,
	FVector* OutLocalOffset,
	float* OutActualVoxelSizeCm)
{
	using namespace GaussianProxyMeshPrivate;

	if (Points.Num() < 8)
	{
		OutError = TEXT("Not enough sample points to build a proxy mesh (need at least 8).");
		return nullptr;
	}

	// Work in optional recentered frame.
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

	// Scene size in cm (before voxel pad). Large outdoor / forest sets span hundreds of meters.
	const FVector SampleExtent = (Max - Min).ComponentMax(FVector(1.0f));
	const bool bSingleObject = (Settings.SceneType == EGaussianProxySceneType::SingleObject);
	// Single Object is sparse (surface TSet only): allow finer grids (up to 2048/axis).
	// Room/Outdoor need dense bitsets for fill/carve → keep tighter caps.
	const int32 MaxDim = bSingleObject
		? FMath::Clamp(FMath::Max(Settings.MaxGridDim, 2048), 64, 4096)
		: FMath::Clamp(Settings.MaxGridDim, 64, 2048);
	// Hard build cap; Room/Outdoor dense fill/carve needs a tighter budget (~48M cells).
	// Single Object does not allocate the full AABB bitset — only solid surface cells —
	// so skip volume-based cell budget (it was forcing ~38cm on medium scenes).
	const int64 MaxTotalCellsHard = 1024ll * 1024ll * 256ll;
	const int64 MaxTotalCells = bSingleObject
		? MaxTotalCellsHard
		: (48ll * 1024 * 1024);

	// Fit each axis under MaxDim; Room/Outdoor also fit total AABB cells under MaxTotalCells:
	// (Ex/V)*(Ey/V)*(Ez/V) ≈ Ex*Ey*Ez / V^3  ≤  MaxTotalCells
	float VoxelSize = FMath::Max(Settings.VoxelSizeCm, 1.0f);
	const float NeededForAxis = FMath::Max3(
		SampleExtent.X / static_cast<float>(MaxDim - 4),
		SampleExtent.Y / static_cast<float>(MaxDim - 4),
		SampleExtent.Z / static_cast<float>(MaxDim - 4));
	float NeededForVolume = 0.0f;
	if (!bSingleObject)
	{
		const double ExtentVolume =
			static_cast<double>(SampleExtent.X) * SampleExtent.Y * SampleExtent.Z;
		NeededForVolume = static_cast<float>(FMath::Pow(
			ExtentVolume / static_cast<double>(FMath::Max<int64>(MaxTotalCells, 1)),
			1.0 / 3.0)) * 1.15f; // pad for +1 cells / padding voxels
	}
	const float NeededVoxel = FMath::Max(NeededForAxis, NeededForVolume);
	const float RequestedVoxel = VoxelSize;
	if (NeededVoxel > VoxelSize)
	{
		if (Settings.bAutoGrowVoxelSize)
		{
			VoxelSize = FMath::Max(FMath::CeilToFloat(NeededVoxel), RequestedVoxel);
			const TCHAR* Reason = (NeededForVolume > NeededForAxis)
				? TEXT("total cell budget")
				: TEXT("per-axis grid cap");
			UE_LOG(LogTemp, Warning,
				TEXT("GaussianProxy: auto-raised Voxel Size %.1f → %.1f cm (%s; scene ~%.0fx%.0fx%.0f cm, maxDim=%d, maxCells=%lld)."),
				RequestedVoxel, VoxelSize, Reason,
				SampleExtent.X, SampleExtent.Y, SampleExtent.Z, MaxDim, MaxTotalCells);
		}
		else
		{
			OutError = FString::Printf(
				TEXT("Voxel grid too large for Voxel Size %.1f cm (scene ~%.0f x %.0f x %.0f cm).\n")
				TEXT("Increase Voxel Size to at least ~%.0f cm, or enable Auto Grow Voxel Size."),
				RequestedVoxel, SampleExtent.X, SampleExtent.Y, SampleExtent.Z, NeededVoxel);
			return nullptr;
		}
	}

	if (OutActualVoxelSizeCm)
	{
		*OutActualVoxelSizeCm = VoxelSize;
	}

	// Pad one voxel so edge samples are never clamped into a wrong cell.
	Min -= FVector(VoxelSize);
	Max += FVector(VoxelSize);

	auto ComputeDims = [&](int32& OutX, int32& OutY, int32& OutZ)
	{
		const FVector E = Max - Min;
		OutX = FMath::Max(1, FMath::CeilToInt(E.X / VoxelSize) + 1);
		OutY = FMath::Max(1, FMath::CeilToInt(E.Y / VoxelSize) + 1);
		OutZ = FMath::Max(1, FMath::CeilToInt(E.Z / VoxelSize) + 1);
	};

	int32 DimX = 1, DimY = 1, DimZ = 1;
	ComputeDims(DimX, DimY, DimZ);
	int64 VoxelCount = static_cast<int64>(DimX) * DimY * DimZ;

	// Second grow: Room/Outdoor dense path if AABB cell count still exceeds budget.
	// Single Object skips this — sparse surface does not allocate VoxelCount bits.
	if (!bSingleObject && VoxelCount > MaxTotalCells && Settings.bAutoGrowVoxelSize)
	{
		const float Grow = static_cast<float>(FMath::Pow(
			static_cast<double>(VoxelCount) / static_cast<double>(FMath::Max<int64>(MaxTotalCells, 1)),
			1.0 / 3.0));
		VoxelSize = FMath::CeilToFloat(VoxelSize * Grow * 1.05f);
		if (OutActualVoxelSizeCm)
		{
			*OutActualVoxelSizeCm = VoxelSize;
		}
		Min = LocalPoints[0];
		Max = LocalPoints[0];
		for (const FVector& P : LocalPoints)
		{
			Min = Min.ComponentMin(P);
			Max = Max.ComponentMax(P);
		}
		Min -= FVector(VoxelSize);
		Max += FVector(VoxelSize);
		ComputeDims(DimX, DimY, DimZ);
		VoxelCount = static_cast<int64>(DimX) * DimY * DimZ;
		UE_LOG(LogTemp, Warning,
			TEXT("GaussianProxy: second auto-raise Voxel Size → %.1f cm (grid %dx%dx%d = %lld cells)."),
			VoxelSize, DimX, DimY, DimZ, VoxelCount);
	}

	const bool bGridTooLarge = bSingleObject
		? (DimX > MaxDim || DimY > MaxDim || DimZ > MaxDim)
		: (VoxelCount > MaxTotalCellsHard || DimX > MaxDim || DimY > MaxDim || DimZ > MaxDim);
	if (bGridTooLarge)
	{
		const float Suggest = FMath::Max(NeededVoxel, VoxelSize * 1.25f);
		OutError = FString::Printf(
			TEXT("Voxel grid still too large (%dx%dx%d = %lld index cells) at %.1f cm.\n")
			TEXT("Raise Voxel Size to at least ~%.0f cm (scene ~%.0f x %.0f x %.0f cm, maxDim=%d)."),
			DimX, DimY, DimZ, VoxelCount, VoxelSize, Suggest,
			SampleExtent.X, SampleExtent.Y, SampleExtent.Z, MaxDim);
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

	// Drop sparse floaters BEFORE fill so Outdoor floor-fill does not erect pillars under noise.
	RemoveSmallSolidIslands(Solid, Settings.MinSolidIslandVoxels);

	// PlayCanvas-style fill: Single skips; Room external-fill; Outdoor floor-fill.
	ApplySceneTypeFill(Solid, Settings, VoxelSize, Min, DimX, DimY, DimZ);

	// After fill/carve, drop residual tiny shells and optionally keep only the main body.
	RemoveSmallSolidIslands(Solid, Settings.MinSolidIslandVoxels);
	if (Settings.bKeepPrimarySolidComponent && Solid.Num() > 0)
	{
		const FVector Seed = Settings.FillSeedPosition - MeshOrigin;
		const FInt3 SeedCell{
			FMath::FloorToInt((Seed.X - Min.X) / VoxelSize),
			FMath::FloorToInt((Seed.Y - Min.Y) / VoxelSize),
			FMath::FloorToInt((Seed.Z - Min.Z) / VoxelSize)
		};
		const FInt3* SeedPtr =
			(Settings.SceneType != EGaussianProxySceneType::SingleObject) ? &SeedCell : nullptr;
		KeepPrimarySolidComponent(Solid, SeedPtr);
	}

	if (Solid.Num() == 0)
	{
		OutError = TEXT("Voxelization produced an empty solid set. Lower Min Hits / Shrink / Min Island, or Min Opacity.");
		return nullptr;
	}

	FMeshDescription MeshDescription;
	// Smooth = welded Faces + Taubin/Laplacian (not binary marching cubes on thin shells).
	const bool bSurfaceSmooth = (Settings.MeshMode == EGaussianProxyMeshMode::Smooth);
	ExtractExposedFaces(
		Solid,
		VoxelSize,
		Min,
		MeshDescription,
		bSurfaceSmooth,
		Settings.SmoothIterations,
		Settings.SmoothLambda);
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
