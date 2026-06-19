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
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarMaxScenesPerView;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarMaxRasterSplats;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarDebugRender;
	extern GAUSSIANSIMVERSE_API TAutoConsoleVariable<int32> CVarDebugOverlay;
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
