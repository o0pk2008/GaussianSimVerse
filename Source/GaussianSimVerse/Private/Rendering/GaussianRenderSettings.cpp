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
		1.5f,
		TEXT("Screen-space scale factor for Gaussian splat footprints."),
		ECVF_RenderThreadSafe | ECVF_Scalability);

	TAutoConsoleVariable<int32> CVarMaxRasterRadius(
		TEXT("r.GaussianSimVerse.MaxRasterRadius"),
		256,
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

	TAutoConsoleVariable<int32> CVarUseTileRaster(
		TEXT("r.GaussianSimVerse.UseTileRaster"),
		1,
		TEXT("Rasterization strategy:\n")
		TEXT("0: Always global per-splat raster (no tile seams; slow when close)\n")
		TEXT("1: Tile raster + TileSort + multi-batch blend (default)\n")
		TEXT("2: Adaptive — tile when close/medium, global when far"),
		ECVF_RenderThreadSafe | ECVF_Scalability);

	TAutoConsoleVariable<float> CVarAdaptiveFarViewRatio(
		TEXT("r.GaussianSimVerse.AdaptiveFarViewRatio"),
		52.0f,
		TEXT("Adaptive mode (UseTileRaster=2): begin switching to global when\n")
		TEXT("(camera distance / scene bounds radius) exceeds this value.\n")
		TEXT("Default 52 keeps tile through medium distance (fast). Global only when far.\n")
		TEXT("Lower values (e.g. 28) switch to global sooner — seam-free but very slow at medium range."),
		ECVF_RenderThreadSafe | ECVF_Scalability);

	TAutoConsoleVariable<float> CVarAdaptiveFarViewHysteresis(
		TEXT("r.GaussianSimVerse.AdaptiveFarViewHysteresis"),
		6.0f,
		TEXT("Adaptive mode hysteresis band on dist/radius around AdaptiveFarViewRatio.\n")
		TEXT("Reduces tile/global flicker and pop at the transition distance."),
		ECVF_RenderThreadSafe | ECVF_Scalability);

	TAutoConsoleVariable<int32> CVarMaxSplatsPerTile(
		TEXT("r.GaussianSimVerse.MaxSplatsPerTile"),
		8192,
		TEXT("Maximum splats stored per 16x16 tile (tile raster).\n")
		TEXT("TileSort + multi-batch TileBlend process the full list up to this cap.\n")
		TEXT("Values above the cap are clamped. Lower values cause 16px tile dropout (blocks).\n")
		TEXT("Valid range: 16..16384"),
		ECVF_RenderThreadSafe | ECVF_Scalability);

	TAutoConsoleVariable<float> CVarAlphaCutoff(
		TEXT("r.GaussianSimVerse.AlphaCutoff"),
		1.0f / 255.0f,
		TEXT("Minimum per-pixel alpha contribution kept during Gaussian blending.\n")
		TEXT("Lower values keep more tail energy but can increase halos/noise."),
		ECVF_RenderThreadSafe | ECVF_Scalability);

	TAutoConsoleVariable<float> CVarAlphaCullThreshold(
		TEXT("r.GaussianSimVerse.AlphaCullThreshold"),
		2.0f / 255.0f,
		TEXT("Cull entire splats whose base opacity is below this threshold before rasterization.\n")
		TEXT("Helps remove fuzzy low-density outliers around the model silhouette."),
		ECVF_RenderThreadSafe | ECVF_Scalability);

	TAutoConsoleVariable<float> CVarCutoffK(
		TEXT("r.GaussianSimVerse.CutoffK"),
		7.0f,
		TEXT("Ellipse cutoff in conic space.\n")
		TEXT("Lower values tighten splat tails and reduce halos; higher values fill more aggressively."),
		ECVF_RenderThreadSafe | ECVF_Scalability);

	TAutoConsoleVariable<float> CVarCovarianceDilation(
		TEXT("r.GaussianSimVerse.CovarianceDilation"),
		0.3f,
		TEXT("Extra screen-space covariance added for anti-aliasing.\n")
		TEXT("Higher values reduce distant tile-edge sparkles but can soften detail."),
		ECVF_RenderThreadSafe | ECVF_Scalability);

	TAutoConsoleVariable<float> CVarMinSigmaPixels(
		TEXT("r.GaussianSimVerse.MinSigmaPixels"),
		0.35f,
		TEXT("Minimum Gaussian sigma in pixels after projection.\n")
		TEXT("Helps fill distant sub-pixel splats and reduces tile-boundary holes."),
		ECVF_RenderThreadSafe | ECVF_Scalability);

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
