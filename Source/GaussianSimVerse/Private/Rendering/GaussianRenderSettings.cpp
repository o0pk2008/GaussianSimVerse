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
		1.0f,
		TEXT("Screen-space scale factor for Gaussian splat footprints (1.0 matches SuperSplat).\n")
		TEXT("Values >1 inflate footprints and wash out color via over-blending."),
		ECVF_RenderThreadSafe | ECVF_Scalability);

	TAutoConsoleVariable<int32> CVarMaxRasterRadius(
		TEXT("r.GaussianSimVerse.MaxRasterRadius"),
		256,
		TEXT("Maximum screen-space splat radius in pixels."),
		ECVF_RenderThreadSafe | ECVF_Scalability);

	TAutoConsoleVariable<int32> CVarMaxSortElements(
		TEXT("r.GaussianSimVerse.MaxSortElements"),
		8388608,
		TEXT("Maximum Gaussians per chunk for GPU depth sort.\n")
		TEXT("Default 8M covers large SuperSplat PLYs. Chunks larger than this skip sorting."),
		ECVF_RenderThreadSafe | ECVF_Scalability);

	TAutoConsoleVariable<int32> CVarSortMethod(
		TEXT("r.GaussianSimVerse.SortMethod"),
		0,
		TEXT("GPU depth-sort algorithm:\n")
		TEXT("0: Bitonic + stable GaussianIndex tie-break (default; flicker-free transparency)\n")
		TEXT("1: Radix 4-bit (faster on huge clouds; 64-bit stable key; may still flicker on ties)"),
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
		TEXT("1: On\n")
		TEXT("Alias: r.GaussianSimVerse.RenderDebug"),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarRenderDebug(
		TEXT("r.GaussianSimVerse.RenderDebug"),
		0,
		TEXT("Alias for r.GaussianSimVerse.DebugRender — log per-frame Gaussian raster stats."),
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
		2,
		TEXT("Rasterization strategy:\n")
		TEXT("0: Instanced splat draw (default global path; SuperSplat-style)\n")
		TEXT("1: Tile compute raster (legacy; 16px blocks when tiles overflow)\n")
		TEXT("2: Adaptive — instanced draw when shaders available (default)\n")
		TEXT("   Falls back to tile/compute only if draw shaders fail to compile."),
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
		16384,
		TEXT("Maximum splats stored per 16x16 tile (tile raster).\n")
		TEXT("TileSort + multi-batch TileBlend process the full list up to this cap.\n")
		TEXT("Lower values cause 16px tile dropout (blocks) on dense PLYs.\n")
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

	TAutoConsoleVariable<int32> CVarStreamingEnable(
		TEXT("r.GaussianSimVerse.Streaming.Enable"),
		1,
		TEXT("Enable runtime LOD streaming for GaussianStreamedSceneActor.\n")
		TEXT("0: Disabled\n")
		TEXT("1: Enabled (default)"),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarStreamingLoadRadius(
		TEXT("r.GaussianSimVerse.Streaming.LoadRadius"),
		50000.0f,
		TEXT("World units around the view origin where streamed chunks may load."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarStreamingLodBaseDistance(
		TEXT("r.GaussianSimVerse.Streaming.LodBaseDistance"),
		1.0f,
		TEXT("Base distance metric for streamed LOD selection (higher = coarser LOD sooner)."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarStreamingLodMultiplier(
		TEXT("r.GaussianSimVerse.Streaming.LodMultiplier"),
		1.0f,
		TEXT("Additional scale on streamed LOD metric."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarStreamingMaxLoadedSplats(
		TEXT("r.GaussianSimVerse.Streaming.MaxLoadedSplats"),
		4000000,
		TEXT("Maximum resident splats across all streamed chunks (0 = unlimited)."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarStreamingMaxLoadsPerFrame(
		TEXT("r.GaussianSimVerse.Streaming.MaxLoadsPerFrame"),
		8,
		TEXT("Maximum async chunk loads started per frame."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarStreamingDebugDraw(
		TEXT("r.GaussianSimVerse.Streaming.DebugDraw"),
		0,
		TEXT("Draw streamed LOD octree bounds in the viewport.\n")
		TEXT("0: Off (default)\n")
		TEXT("1: On"),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarStreamingDebugOverlay(
		TEXT("r.GaussianSimVerse.Streaming.DebugOverlay"),
		0,
		TEXT("Show on-screen streamed chunk statistics.\n")
		TEXT("0: Off (default)\n")
		TEXT("1: On"),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarStreamingDebugRenderMode(
		TEXT("r.GaussianSimVerse.Streaming.DebugRenderMode"),
		0,
		TEXT("Streamed scene debug render mode.\n")
		TEXT("0: None (default)\n")
		TEXT("1: LOD (color-code chunks by LOD level)"),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarGlobalUnifiedSort(
		TEXT("r.GaussianSimVerse.GlobalUnifiedSort"),
		1,
		TEXT("When multiple chunks are visible, merge them into one global depth-sorted draw.\n")
		TEXT("Fixes front/back seams between streamed LOD chunks.\n")
		TEXT("0: Per-chunk sort (legacy)\n")
		TEXT("1: Global unified sort (default)"),
		ECVF_RenderThreadSafe | ECVF_Scalability);
}
