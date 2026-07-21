// Copyright Epic Games, Inc. All Rights Reserved.

#include "Rendering/GaussianProxyDofMitigation.h"
#include "Components/StaticMeshComponent.h"
#include "HAL/IConsoleManager.h"

namespace GaussianProxyDofMitigation
{
	static int32 GActiveCount = 0;
	static bool bHaveSavedCVars = false;
	static int32 GSavedAoLevels = 1;
	static float GSavedAoIntensity = 1.0f;
	static int32 GSavedContactShadows = 1;
	static int32 GSavedAoMethod = 0;
	static int32 GSavedAoMaxQuality = 100;
	static int32 GSavedLumenScreenTraces = 1;

	static IConsoleVariable* FindCVar(const TCHAR* Name)
	{
		return IConsoleManager::Get().FindConsoleVariable(Name);
	}

	static void SaveAndSetInt(const TCHAR* Name, int32 NewValue, int32& OutSaved)
	{
		if (IConsoleVariable* CVar = FindCVar(Name))
		{
			OutSaved = CVar->GetInt();
			CVar->Set(NewValue, ECVF_SetByCode);
		}
	}

	static void SaveAndSetFloat(const TCHAR* Name, float NewValue, float& OutSaved)
	{
		if (IConsoleVariable* CVar = FindCVar(Name))
		{
			OutSaved = CVar->GetFloat();
			CVar->Set(NewValue, ECVF_SetByCode);
		}
	}

	static void RestoreInt(const TCHAR* Name, int32 Saved)
	{
		if (IConsoleVariable* CVar = FindCVar(Name))
		{
			CVar->Set(Saved, ECVF_SetByCode);
		}
	}

	static void RestoreFloat(const TCHAR* Name, float Saved)
	{
		if (IConsoleVariable* CVar = FindCVar(Name))
		{
			CVar->Set(Saved, ECVF_SetByCode);
		}
	}

	static void ApplyCVarSuppress()
	{
		if (bHaveSavedCVars)
		{
			return;
		}

		// SSAO / GTAO family (names differ across UE versions — set what exists).
		SaveAndSetInt(TEXT("r.AmbientOcclusionLevels"), 0, GSavedAoLevels);
		SaveAndSetFloat(TEXT("r.AmbientOcclusionIntensity"), 0.0f, GSavedAoIntensity);
		SaveAndSetInt(TEXT("r.AmbientOcclusion.Method"), 0, GSavedAoMethod);
		SaveAndSetInt(TEXT("r.AmbientOcclusionMaxQuality"), 0, GSavedAoMaxQuality);
		SaveAndSetInt(TEXT("r.ContactShadows"), 0, GSavedContactShadows);
		// Lumen can also darken from proxy Scene Depth via screen traces.
		SaveAndSetInt(TEXT("r.Lumen.ScreenProbeGather.ScreenTraces"), 0, GSavedLumenScreenTraces);

		bHaveSavedCVars = true;
		UE_LOG(LogTemp, Log,
			TEXT("GaussianSimVerse: Proxy DOF mitigations on (SSAO/ContactShadows/Lumen screen-traces suppressed)."));
	}

	static void RestoreCVarSuppress()
	{
		if (!bHaveSavedCVars)
		{
			return;
		}

		RestoreInt(TEXT("r.AmbientOcclusionLevels"), GSavedAoLevels);
		RestoreFloat(TEXT("r.AmbientOcclusionIntensity"), GSavedAoIntensity);
		RestoreInt(TEXT("r.AmbientOcclusion.Method"), GSavedAoMethod);
		RestoreInt(TEXT("r.AmbientOcclusionMaxQuality"), GSavedAoMaxQuality);
		RestoreInt(TEXT("r.ContactShadows"), GSavedContactShadows);
		RestoreInt(TEXT("r.Lumen.ScreenProbeGather.ScreenTraces"), GSavedLumenScreenTraces);

		bHaveSavedCVars = false;
		UE_LOG(LogTemp, Log, TEXT("GaussianSimVerse: Proxy DOF mitigations restored previous CVars."));
	}

	void SetActive(bool bActive)
	{
		if (bActive)
		{
			if (GActiveCount == 0)
			{
				ApplyCVarSuppress();
			}
			++GActiveCount;
		}
		else if (GActiveCount > 0)
		{
			--GActiveCount;
			if (GActiveCount == 0)
			{
				RestoreCVarSuppress();
			}
		}
	}

	void ConfigureDepthOnlyComponent(UStaticMeshComponent* Component, bool bShowColor, bool bWriteSceneDepth)
	{
		if (!Component)
		{
			return;
		}

		// Color only when the user wants to see the proxy mesh.
		Component->SetRenderInMainPass(bShowColor);
		Component->bRenderInDepthPass = bShowColor || bWriteSceneDepth;

		// Never light or shadow from a pure depth proxy — reduces "black volume" side channels.
		Component->SetCastShadow(false);
		Component->bCastDynamicShadow = false;
		Component->bCastStaticShadow = false;
		Component->bCastContactShadow = false;
		Component->bCastVolumetricTranslucentShadow = false;
		Component->bCastInsetShadow = false;
		Component->bAffectDynamicIndirectLighting = bShowColor;
		Component->bAffectDistanceFieldLighting = false;
		Component->SetReceivesDecals(false);
		Component->bUseAsOccluder = bWriteSceneDepth;
		Component->bVisibleInReflectionCaptures = bShowColor;
		Component->bVisibleInRealTimeSkyCaptures = bShowColor;
		Component->bVisibleInRayTracing = bShowColor;
		Component->SetCullDistance(0.0f);
		Component->SetBoundsScale(1.0f);

		// No lighting channels contribution when depth-only.
		if (!bShowColor)
		{
			FLightingChannels Channels;
			Channels.bChannel0 = false;
			Channels.bChannel1 = false;
			Channels.bChannel2 = false;
			Component->SetLightingChannels(Channels.bChannel0, Channels.bChannel1, Channels.bChannel2);
		}
	}
}
