// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Proxy Scene Depth for Cinematic DOF makes SSAO/contact-shadows darken the proxy volume
 * (looks like a black mesh over gaussians). Optional ref-counted CVar suppress while DOF is on.
 */
namespace GaussianProxyDofMitigation
{
	/** Enable or disable visual mitigations for one consumer (actor). Ref-counted. */
	void SetActive(bool bActive);

	/** Apply depth-only component flags so the proxy contributes depth without lit color. */
	void ConfigureDepthOnlyComponent(class UStaticMeshComponent* Component, bool bShowColor, bool bWriteSceneDepth);
}
