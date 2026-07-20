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

	// Scene size in cm (before voxel pad). Large LOD outdoor sets can be hundreds of meters.
	const FVector SampleExtent = (Max - Min).ComponentMax(FVector(1.0f));
	const int32 MaxDim = FMath::Clamp(Settings.MaxGridDim, 64, 2048);

	// Requested size, then optionally grow so each axis fits MaxDim (plus pad cells).
	float VoxelSize = FMath::Max(Settings.VoxelSizeCm, 1.0f);
	const float NeededVoxel = FMath::Max3(
		SampleExtent.X / static_cast<float>(MaxDim - 4),
		SampleExtent.Y / static_cast<float>(MaxDim - 4),
		SampleExtent.Z / static_cast<float>(MaxDim - 4));
	const float RequestedVoxel = VoxelSize;
	if (NeededVoxel > VoxelSize)
	{
		if (Settings.bAutoGrowVoxelSize)
		{
			// Ceil to a readable cm value so large scenes always succeed.
			VoxelSize = FMath::Max(FMath::CeilToFloat(NeededVoxel), RequestedVoxel);
			UE_LOG(LogTemp, Warning,
				TEXT("GaussianProxy: auto-raised Voxel Size %.1f → %.1f cm (scene extent ~%.0fx%.0fx%.0f cm, max dim %d)."),
				RequestedVoxel, VoxelSize, SampleExtent.X, SampleExtent.Y, SampleExtent.Z, MaxDim);
		}
		else
		{
			OutError = FString::Printf(
				TEXT("Voxel grid too large for Voxel Size %.1f cm (scene ~%.0f x %.0f x %.0f cm).\n")
				TEXT("Increase Voxel Size to at least %.1f cm (recommended for this LOD/scene),\n")
				TEXT("or enable auto-grow (default)."),
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

	const FVector Extent = Max - Min;
	const int32 DimX = FMath::Max(1, FMath::CeilToInt(Extent.X / VoxelSize) + 1);
	const int32 DimY = FMath::Max(1, FMath::CeilToInt(Extent.Y / VoxelSize) + 1);
	const int32 DimZ = FMath::Max(1, FMath::CeilToInt(Extent.Z / VoxelSize) + 1);
	const int64 VoxelCount = static_cast<int64>(DimX) * DimY * DimZ;
	if (VoxelCount > 1024ll * 1024ll * 256ll || DimX > MaxDim || DimY > MaxDim || DimZ > MaxDim)
	{
		OutError = FString::Printf(
			TEXT("Voxel grid still too large (%dx%dx%d) at %.1f cm.\nIncrease Voxel Size further (scene ~%.0fx%.0fx%.0f cm)."),
			DimX, DimY, DimZ, VoxelSize, SampleExtent.X, SampleExtent.Y, SampleExtent.Z);
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

	// PlayCanvas-style fill: Single skips; Room external-fill; Outdoor floor-fill.
	ApplySceneTypeFill(Solid, Settings, VoxelSize, Min, DimX, DimY, DimZ);

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
