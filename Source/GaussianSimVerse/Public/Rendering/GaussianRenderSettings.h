// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"

namespace GaussianSimVerse::RenderSettings
{
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarEnableRendering;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarEnableCull;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarEnableSort;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarEnableRaster;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarDebugDrawFramework;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarDebugGPUBuffer;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarDebugCull;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<float> CVarFrustumMargin;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<float> CVarSplatScale;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarMaxRasterRadius;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarMaxSortElements;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarSortMethod;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarMaxScenesPerView;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarMaxRasterSplats;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarDebugRender;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarDebugOverlay;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarUseTileRaster;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<float> CVarAdaptiveFarViewRatio;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<float> CVarAdaptiveFarViewHysteresis;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarMaxSplatsPerTile;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<float> CVarAlphaCutoff;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<float> CVarAlphaCullThreshold;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<float> CVarCutoffK;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<float> CVarCovarianceDilation;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<float> CVarMinSigmaPixels;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarUseResolvedSceneColor;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarPostProcessPass;

	FORCEINLINE bool IsRenderingEnabled()
	{
		return CVarEnableRendering.GetValueOnAnyThread() != 0;
	}

	FORCEINLINE bool IsCullEnabled()
	{
		return CVarEnableCull.GetValueOnAnyThread() != 0;
	}

	FORCEINLINE bool IsSortEnabled()
	{
		return CVarEnableSort.GetValueOnAnyThread() != 0;
	}

	/** 0 = bitonic, 1 = radix (default). */
	FORCEINLINE int32 GetSortMethod()
	{
		return FMath::Clamp(CVarSortMethod.GetValueOnAnyThread(), 0, 1);
	}

	FORCEINLINE bool IsRasterEnabled()
	{
		return CVarEnableRaster.GetValueOnAnyThread() != 0;
	}

	FORCEINLINE bool IsFrameworkDebugEnabled()
	{
		return CVarDebugDrawFramework.GetValueOnAnyThread() != 0;
	}

	FORCEINLINE bool IsGPUBufferDebugEnabled()
	{
		return CVarDebugGPUBuffer.GetValueOnAnyThread() != 0;
	}

	FORCEINLINE bool IsCullDebugEnabled()
	{
		return CVarDebugCull.GetValueOnAnyThread() != 0;
	}

	FORCEINLINE bool IsRenderDebugEnabled()
	{
		return CVarDebugRender.GetValueOnAnyThread() != 0;
	}

	FORCEINLINE bool IsDebugOverlayEnabled()
	{
		return CVarDebugOverlay.GetValueOnAnyThread() != 0;
	}

	/** Shader per-tile splat storage cap (TileSortCS / TileBlendCS). */
	constexpr int32 ShaderMaxSplatsPerTile = 16384;

	FORCEINLINE int32 GetTileRasterMode()
	{
		return FMath::Clamp(CVarUseTileRaster.GetValueOnAnyThread(), 0, 2);
	}

	/** @deprecated Use GetTileRasterMode(); 1 = force tile, 0 = force global. */
	FORCEINLINE bool UseTileRaster()
	{
		return GetTileRasterMode() == 1;
	}

	FORCEINLINE int32 GetMaxSplatsPerTile()
	{
		return FMath::Clamp(CVarMaxSplatsPerTile.GetValueOnAnyThread(), 16, ShaderMaxSplatsPerTile);
	}

	/**
	 * Global per-splat raster loops over each splat's screen footprint (up to MaxRasterRadius^2
	 * pixels) with RMW UAV blends. Million-scale PLYs TDR the GPU — keep this independent of
	 * MaxSortElements (sort can be large; global pixel loops cannot).
	 */
	FORCEINLINE uint32 GetMaxSafeGlobalRasterSplats()
	{
		return 262144u;
	}

	/** Adaptive mode with hysteresis: tile close/medium, global only when clearly far (splats tiny). */
	FORCEINLINE bool ShouldUseTileRasterPath(
		int32 RasterMode,
		bool bTileShadersValid,
		uint32 EstimatedSplatsPerTile,
		int32 MaxSplatsPerTile,
		float ViewDistanceToScene,
		float SceneBoundsRadius,
		bool& bInOutLastUsedTilePath,
		uint32 TotalSplatCount = 0)
	{
		if (!bTileShadersValid)
		{
			return false;
		}
		if (RasterMode == 0)
		{
			// Explicit global request — caller must still guard huge clouds.
			return false;
		}

		const uint32 MaxSafeGlobal = GetMaxSafeGlobalRasterSplats();
		const bool bGlobalSafe = TotalSplatCount == 0 || TotalSplatCount <= MaxSafeGlobal;

		const float SafeRadius = FMath::Max(SceneBoundsRadius, 1.0f);
		const float DistanceToRadius = ViewDistanceToScene / SafeRadius;
		const float FarViewRatio = FMath::Max(2.0f, CVarAdaptiveFarViewRatio.GetValueOnAnyThread());

		const uint32 TileDenseThreshold = static_cast<uint32>(FMath::Max(16, (MaxSplatsPerTile * 3) / 4));
		// Dense tiles overflow → 16px block seams. Prefer global only when the cloud is small
		// enough that per-splat screen loops will not TDR the device.
		if (bGlobalSafe && EstimatedSplatsPerTile > TileDenseThreshold)
		{
			return false;
		}

		if (RasterMode == 1)
		{
			// Tile-first mode still falls back to global when clearly far; otherwise all splats
			// cluster into a few tiles and overflow the per-tile cap (empty bbox at distance).
			if (bGlobalSafe && DistanceToRadius > FarViewRatio)
			{
				return false;
			}
			return true;
		}

		if (!bGlobalSafe)
		{
			// Large PLYs must stay on the tile path (global raster TDRs).
			bInOutLastUsedTilePath = true;
			return true;
		}

		const float Hysteresis = FMath::Max(0.0f, CVarAdaptiveFarViewHysteresis.GetValueOnAnyThread());

		if (bInOutLastUsedTilePath)
		{
			if (DistanceToRadius > FarViewRatio + Hysteresis)
			{
				bInOutLastUsedTilePath = false;
			}
		}
		else if (DistanceToRadius < FarViewRatio - Hysteresis)
		{
			bInOutLastUsedTilePath = true;
		}

		return bInOutLastUsedTilePath;
	}

	FORCEINLINE float GetAlphaCutoff()
	{
		return FMath::Clamp(CVarAlphaCutoff.GetValueOnAnyThread(), 0.0f, 1.0f);
	}

	FORCEINLINE float GetAlphaCullThreshold()
	{
		return FMath::Clamp(CVarAlphaCullThreshold.GetValueOnAnyThread(), 0.0f, 1.0f);
	}

	FORCEINLINE float GetCutoffK()
	{
		return FMath::Max(0.25f, CVarCutoffK.GetValueOnAnyThread());
	}

	FORCEINLINE float GetCovarianceDilation()
	{
		return FMath::Max(0.0f, CVarCovarianceDilation.GetValueOnAnyThread());
	}

	FORCEINLINE float GetMinSigmaPixels()
	{
		return FMath::Max(0.0f, CVarMinSigmaPixels.GetValueOnAnyThread());
	}

	FORCEINLINE float GetFrustumMargin()
	{
		return FMath::Max(0.0f, CVarFrustumMargin.GetValueOnAnyThread());
	}

	FORCEINLINE float GetSplatScale()
	{
		return FMath::Max(0.01f, CVarSplatScale.GetValueOnAnyThread());
	}

	/** 0 = BeforeDOF, 1 = AfterDOF */
	FORCEINLINE int32 GetPostProcessPass()
	{
		return FMath::Clamp(CVarPostProcessPass.GetValueOnAnyThread(), 0, 1);
	}

	FORCEINLINE bool UseResolvedSceneColorPath()
	{
		return CVarUseResolvedSceneColor.GetValueOnAnyThread() != 0;
	}
}
