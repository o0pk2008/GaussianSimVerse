// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h"
#include "Rendering/GaussianRenderResources.h"
#include "Rendering/GaussianGPUBuffer.h"

class FSceneView;

/** RDG transient resources allocated per frame. */
struct FGaussianRDGTransientResources
{
	FRDGBufferRef FrameworkCounterBuffer = nullptr;
	FRDGBufferUAVRef FrameworkCounterUAV = nullptr;
	TArray<FGaussianRDGBufferBinding> GPUBuffers;
	TArray<FGaussianRDGCullResult> CullResults;
	TArray<FGaussianRDGSortResult> SortResults;
	uint32 UploadedGaussianCount = 0;
	uint32 TotalCulledVisibleCount = 0;

	float ViewDepthMin = 0.0f;
	float ViewDepthMax = 100000.0f;
	bool bUseGlobalUnifiedSort = false;
	uint32 GlobalUnifiedCount = 0;
	FRDGBufferSRVRef GlobalUnifiedSplatsSRV = nullptr;
	FRDGBufferSRVRef GlobalUnifiedShCoeffsSRV = nullptr;
	FRDGBufferSRVRef GlobalBindingIdsSRV = nullptr;
	FRDGBufferSRVRef GlobalChunkMatrixRowsSRV = nullptr;
	FRDGBufferSRVRef GlobalChunkBindingParamsSRV = nullptr;
	FRDGBufferSRVRef GlobalSortedIndicesSRV = nullptr;
	FRDGBufferSRVRef GlobalVisibleCountSRV = nullptr;
	FGaussianRDGBufferBinding GlobalDrawBinding;
};

class GAUSSIANSIMVERSE_API FGaussianRenderGraph
{
public:
	struct FPassInputs
	{
		const FSceneView* View = nullptr;
		FGaussianViewData ViewData;
		FGaussianFrameResources FrameResources;
		FRDGTextureRef SceneColorTexture = nullptr;
		FIntRect SceneColorViewRect;
		/**
		 * Absolute pixel origin of SceneDepth / CustomStencil in SceneTextures space
		 * (usually ViewInfo.ViewRect.Min). May differ from SceneColorViewRect after late PP resolves.
		 */
		FIntPoint SceneDepthPixelOffset = FIntPoint::ZeroValue;
		/** Engine SceneDepth (resolved) for occlusion vs opaque actors. */
		FRDGTextureRef SceneDepthTexture = nullptr;
		/** Custom-depth stencil SRV (proxy mesh stamps stencil for DOF exclude). */
		FRDGTextureSRVRef CustomStencilSRV = nullptr;
		/** View.InvDeviceZToWorldZTransform for DeviceZ → linear depth. */
		FVector4f InvDeviceZToWorldZTransform = FVector4f(0, 0, 0, 0);
		/** Occlude splats behind opaque SceneDepth (cm bias). */
		bool bDepthOcclusion = true;
		float DepthOcclusionBiasCm = 2.0f;
		/**
		 * When true, pixels whose Custom Stencil == ProxyStencilExclude skip depth occlusion
		 * (proxy Scene Depth for DOF must not erase gaussians).
		 */
		bool bExcludeProxyStencilFromOcclusion = false;
		uint32 ProxyStencilExclude = 1;

		/**
		 * CineCamera DOF: export per-pixel nearest splat DeviceZ (reverse-Z, larger = nearer)
		 * so engine Diaphragm DOF can focus near vs far gaussians (not whole-cloud only).
		 */
		bool bExportSoftDepthForDof = false;
		/** Optional out: R32_FLOAT DeviceZ, 0 = no gaussian. */
		FRDGTextureRef* OutSoftDepthDeviceZ = nullptr;
	};

	static FGaussianRDGTransientResources AllocateTransientResources(FRDGBuilder& GraphBuilder);
	static void AddPasses(FRDGBuilder& GraphBuilder, const FPassInputs& Inputs, FGaussianRDGTransientResources& TransientResources);

private:
	static void AddGPUBufferUploadPasses(
		FRDGBuilder& GraphBuilder,
		const FPassInputs& Inputs,
		FGaussianRDGTransientResources& TransientResources);

	static void AddGPUFrustumCullPasses(
		FRDGBuilder& GraphBuilder,
		const FPassInputs& Inputs,
		FGaussianRDGTransientResources& TransientResources);

	static void AddPassthroughCullResults(
		FRDGBuilder& GraphBuilder,
		FGaussianRDGTransientResources& TransientResources);

	static void AddGPUDepthSortPasses(
		FRDGBuilder& GraphBuilder,
		const FPassInputs& Inputs,
		FGaussianRDGTransientResources& TransientResources);

	static void AddGPURasterPasses(
		FRDGBuilder& GraphBuilder,
		const FPassInputs& Inputs,
		FGaussianRDGTransientResources& TransientResources);

	static void AddFrameworkValidationPass(
		FRDGBuilder& GraphBuilder,
		const FPassInputs& Inputs,
		FGaussianRDGTransientResources& TransientResources);
};
