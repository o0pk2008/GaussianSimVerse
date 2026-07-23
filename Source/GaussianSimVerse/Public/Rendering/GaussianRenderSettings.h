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
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarRenderDebug;
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
	/** When 1, CineCamera DOF forces Full BeforeDOF inject (color+soft-depth) even if PostProcessPass is 1/2. */
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarAutoBeforeDofForProxyDof;
	/** When 1, also run plugin CoC blur after inject (optional; CineCamera usually enough). */
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarPluginDofBlur;
	/** Occlude Gaussian pixels behind opaque scene depth (mix with regular actors). */
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarDepthOcclusion;
	/** Extra cm added to scene depth before occlusion test (reduce edge z-fight). */
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<float> CVarDepthOcclusionBias;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarPreferNonTemporalAA;

	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarStreamingEnable;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<float> CVarStreamingLoadRadius;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<float> CVarStreamingLodBaseDistance;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<float> CVarStreamingLodMultiplier;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarStreamingMaxLoadedSplats;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarStreamingMaxLoadsPerFrame;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarStreamingMaxCommitSplatsPerFrame;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarStreamingMotionLodBias;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarStreamingLodUnderfillLimit;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarStreamingDebugDraw;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarStreamingDebugOverlay;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarStreamingDebugRenderMode;

	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarGlobalUnifiedSort;

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
		return CVarDebugRender.GetValueOnAnyThread() != 0
			|| CVarRenderDebug.GetValueOnAnyThread() != 0;
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

	/** Adaptive mode: instanced splat draw by default; tile only when forced (mode 1). */
	FORCEINLINE bool ShouldUseTileRasterPath(
		int32 RasterMode,
		bool bTileShadersValid,
		uint32 EstimatedSplatsPerTile,
		int32 MaxSplatsPerTile,
		float ViewDistanceToScene,
		float SceneBoundsRadius,
		bool& bInOutLastUsedTilePath,
		uint32 TotalSplatCount = 0,
		bool bDrawRasterAvailable = false)
	{
		if (!bTileShadersValid)
		{
			return false;
		}
		if (RasterMode == 0)
		{
			return false;
		}
		if (RasterMode == 1)
		{
			return true;
		}

		// Adaptive (mode 2): prefer hardware splat draw — no per-tile cap, no compute TDR.
		if (bDrawRasterAvailable)
		{
			bInOutLastUsedTilePath = false;
			return false;
		}

		const uint32 MaxSafeGlobal = GetMaxSafeGlobalRasterSplats();
		const bool bGlobalSafe = TotalSplatCount == 0 || TotalSplatCount <= MaxSafeGlobal;

		const float SafeRadius = FMath::Max(SceneBoundsRadius, 1.0f);
		const float DistanceToRadius = ViewDistanceToScene / SafeRadius;
		const float FarViewRatio = FMath::Max(2.0f, CVarAdaptiveFarViewRatio.GetValueOnAnyThread());

		const uint32 TileDenseThreshold = static_cast<uint32>(FMath::Max(16, (MaxSplatsPerTile * 3) / 4));
		if (bGlobalSafe && EstimatedSplatsPerTile > TileDenseThreshold)
		{
			return false;
		}

		if (!bGlobalSafe)
		{
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

	/** 0 = BeforeDOF, 1 = AfterDOF, 2 = AfterMotionBlur (manual CVar only). */
	FORCEINLINE int32 GetPostProcessPass()
	{
		return FMath::Clamp(CVarPostProcessPass.GetValueOnAnyThread(), 0, 3);
	}

	FORCEINLINE bool IsAutoBeforeDofForProxyDofEnabled()
	{
		return CVarAutoBeforeDofForProxyDof.GetValueOnAnyThread() != 0;
	}

	FORCEINLINE bool IsDepthOcclusionEnabled()
	{
		return CVarDepthOcclusion.GetValueOnAnyThread() != 0;
	}

	FORCEINLINE float GetDepthOcclusionBiasCm()
	{
		return FMath::Max(0.0f, CVarDepthOcclusionBias.GetValueOnAnyThread());
	}

	/** 0 off, 1 force FXAA, 2 force None when gaussians active and view is TAA/TSR. */
	FORCEINLINE int32 GetPreferNonTemporalAAMode()
	{
		return FMath::Clamp(CVarPreferNonTemporalAA.GetValueOnAnyThread(), 0, 2);
	}

	FORCEINLINE bool UseResolvedSceneColorPath()
	{
		return CVarUseResolvedSceneColor.GetValueOnAnyThread() != 0;
	}

	FORCEINLINE bool IsStreamingEnabled()
	{
		return CVarStreamingEnable.GetValueOnAnyThread() != 0;
	}

	FORCEINLINE float GetStreamingLoadRadius()
	{
		return FMath::Max(100.0f, CVarStreamingLoadRadius.GetValueOnAnyThread());
	}

	FORCEINLINE float GetStreamingLodBaseDistance()
	{
		return FMath::Max(0.01f, CVarStreamingLodBaseDistance.GetValueOnAnyThread());
	}

	FORCEINLINE float GetStreamingLodMultiplier()
	{
		return FMath::Max(0.01f, CVarStreamingLodMultiplier.GetValueOnAnyThread());
	}

	FORCEINLINE int32 GetStreamingMaxLoadedSplats()
	{
		return FMath::Max(0, CVarStreamingMaxLoadedSplats.GetValueOnAnyThread());
	}

	FORCEINLINE int32 GetStreamingMaxLoadsPerFrame()
	{
		return FMath::Clamp(CVarStreamingMaxLoadsPerFrame.GetValueOnAnyThread(), 1, 16);
	}

	/**
	 * Soft budget for splat count committed to the scene on the game thread per update.
	 * 0 = unlimited (chunk-count limit only). Always allows at least one finished chunk so progress continues.
	 */
	FORCEINLINE int32 GetStreamingMaxCommitSplatsPerFrame()
	{
		return FMath::Max(0, CVarStreamingMaxCommitSplatsPerFrame.GetValueOnAnyThread());
	}

	/** Extra LOD levels (coarser) applied while the camera is actively moving, for smoother motion. */
	FORCEINLINE int32 GetStreamingMotionLodBias()
	{
		return FMath::Clamp(CVarStreamingMotionLodBias.GetValueOnAnyThread(), 0, 8);
	}

	/**
	 * Max coarser LOD steps allowed when the optimal level is not resident yet (PlayCanvas-style underfill).
	 * 0 = always request optimal; higher = show/load coarser first, then promote step-by-step.
	 */
	FORCEINLINE int32 GetStreamingLodUnderfillLimit()
	{
		return FMath::Clamp(CVarStreamingLodUnderfillLimit.GetValueOnAnyThread(), 0, 8);
	}

	FORCEINLINE bool IsStreamingDebugEnabled()
	{
		return CVarStreamingDebugDraw.GetValueOnAnyThread() != 0;
	}

	FORCEINLINE bool IsStreamingDebugOverlayEnabled()
	{
		return CVarStreamingDebugOverlay.GetValueOnAnyThread() != 0;
	}

	FORCEINLINE int32 GetStreamingDebugRenderMode()
	{
		return FMath::Clamp(CVarStreamingDebugRenderMode.GetValueOnAnyThread(), 0, 1);
	}

	FORCEINLINE bool IsGlobalUnifiedSortEnabled()
	{
		return CVarGlobalUnifiedSort.GetValueOnAnyThread() != 0;
	}
}
