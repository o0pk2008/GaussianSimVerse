// Copyright Epic Games, Inc. All Rights Reserved.

#include "Rendering/GaussianRenderSettings.h"

namespace GaussianSimVerse::RenderSettings
{
	TAutoConsoleVariable<int32> CVarEnableRendering(
		TEXT("r.GaussianSimVerse.Enable"),
		1,
		TEXT("Enable GaussianSimVerse RDG rendering pipeline.\n")
		TEXT("0: Disabled\n")
		TEXT("1: Enabled (default)"),
		ECVF_RenderThreadSafe | ECVF_Scalability);

	TAutoConsoleVariable<int32> CVarEnableCull(
		TEXT("r.GaussianSimVerse.EnableCull"),
		0,
		TEXT("Enable GPU frustum culling for Gaussian splats.\n")
		TEXT("0: Disabled (default, more reliable at close range / steep angles)\n")
		TEXT("1: Enabled"),
		ECVF_RenderThreadSafe | ECVF_Scalability);

	TAutoConsoleVariable<int32> CVarEnableSort(
		TEXT("r.GaussianSimVerse.EnableSort"),
		1,
		TEXT("Enable GPU depth sort before rasterization.\n")
		TEXT("0: Disabled\n")
		TEXT("1: Enabled (default)"),
		ECVF_RenderThreadSafe | ECVF_Scalability);

	TAutoConsoleVariable<int32> CVarEnableRaster(
		TEXT("r.GaussianSimVerse.EnableRaster"),
		1,
		TEXT("Enable GPU Gaussian rasterization into scene color.\n")
		TEXT("0: Disabled\n")
		TEXT("1: Enabled (default)"),
		ECVF_RenderThreadSafe | ECVF_Scalability);

	TAutoConsoleVariable<int32> CVarDebugDrawFramework(
		TEXT("r.GaussianSimVerse.DebugFramework"),
		0,
		TEXT("Run the RDG framework validation compute pass.\n")
		TEXT("0: Off (default)\n")
		TEXT("1: On"),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarDebugGPUBuffer(
		TEXT("r.GaussianSimVerse.DebugGPUBuffer"),
		0,
		TEXT("Log GPU buffer upload events.\n")
		TEXT("0: Off (default)\n")
		TEXT("1: On"),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarDebugCull(
		TEXT("r.GaussianSimVerse.DebugCull"),
		0,
		TEXT("Reserved for cull debug readback.\n")
		TEXT("0: Off (default)\n")
		TEXT("1: On"),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<float> CVarFrustumMargin(
		TEXT("r.GaussianSimVerse.FrustumMargin"),
		3.0f,
		TEXT("Expand frustum planes by this fraction of W when culling is enabled."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<float> CVarSplatScale(
		TEXT("r.GaussianSimVerse.SplatScale"),
		6.0f,
		TEXT("Screen-space scale factor for Gaussian splat footprints."),
		ECVF_RenderThreadSafe | ECVF_Scalability);

	TAutoConsoleVariable<int32> CVarMaxRasterRadius(
		TEXT("r.GaussianSimVerse.MaxRasterRadius"),
		64,
		TEXT("Maximum screen-space splat radius in pixels."),
		ECVF_RenderThreadSafe | ECVF_Scalability);

	TAutoConsoleVariable<int32> CVarMaxSortElements(
		TEXT("r.GaussianSimVerse.MaxSortElements"),
		262144,
		TEXT("Maximum Gaussians per chunk for GPU bitonic sort (padded to power of two)."),
		ECVF_RenderThreadSafe | ECVF_Scalability);

	TAutoConsoleVariable<int32> CVarMaxScenesPerView(
		TEXT("r.GaussianSimVerse.MaxScenesPerView"),
		256,
		TEXT("Maximum number of Gaussian scenes submitted per view."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarMaxRasterSplats(
		TEXT("r.GaussianSimVerse.MaxRasterSplats"),
		0,
		TEXT("Maximum Gaussians rasterized per chunk per frame. 0 = no cap (render all visible splats)."),
		ECVF_RenderThreadSafe | ECVF_Scalability);

	TAutoConsoleVariable<int32> CVarDebugRender(
		TEXT("r.GaussianSimVerse.DebugRender"),
		0,
		TEXT("Log per-frame Gaussian raster stats.\n")
		TEXT("0: Off (default)\n")
		TEXT("1: On"),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarDebugOverlay(
		TEXT("r.GaussianSimVerse.DebugOverlay"),
		0,
		TEXT("Draw bright debug disks at projected splat centers (bypasses opacity).\n")
		TEXT("0: Off (default)\n")
		TEXT("1: On"),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarUseResolvedSceneColor(
		TEXT("r.GaussianSimVerse.UseResolvedSceneColor"),
		0,
		TEXT("Inject splats via ResolvedSceneColor callback (legacy path).\n")
		TEXT("0: Off — use post-process injection (default, recommended for editor)\n")
		TEXT("1: On"),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarPostProcessPass(
		TEXT("r.GaussianSimVerse.PostProcessPass"),
		1,
		TEXT("Which post-process pass injects Gaussian splats.\n")
		TEXT("0: BeforeDOF\n")
		TEXT("1: AfterDOF (default)"),
		ECVF_RenderThreadSafe);
}
