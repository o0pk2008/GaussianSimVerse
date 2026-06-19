// Copyright Epic Games, Inc. All Rights Reserved.

#include "Rendering/GaussianGPUBuffer.h"
#include "Rendering/GaussianRenderSettings.h"
#include "GaussianSimVerse.h"
#include "RenderGraphUtils.h"
#include "RHIResources.h"

void FGaussianGPUBuffer::SetCPUData(TArray<FGaussianSplatGPU>&& InSplatData)
{
	SplatCPUData = MoveTemp(InSplatData);
	GaussianGPU::BuildPositionBuffer(SplatCPUData, PositionCPUData);
	NumGaussians = SplatCPUData.Num();
	bDirty = true;
}

void FGaussianGPUBuffer::SetCPUDataFromStaging(const TArray<FGaussianSplatData>& StagingData)
{
	TArray<FGaussianSplatGPU> Converted;
	GaussianGPU::ConvertSplatDataArray(StagingData, Converted);
	SetCPUData(MoveTemp(Converted));
}

void FGaussianGPUBuffer::MarkDirty()
{
	bDirty = true;
}

void FGaussianGPUBuffer::ReleaseRenderResources()
{
	ReleasePooledBuffers();
}

void FGaussianGPUBuffer::ReleasePooledBuffers()
{
	SplatPooledBuffer.SafeRelease();
	PositionPooledBuffer.SafeRelease();
}

void FGaussianGPUBuffer::EnsurePooledBuffers(uint32 InNumGaussians)
{
	if (InNumGaussians == 0)
	{
		return;
	}

	const FRDGBufferDesc SplatDesc = FRDGBufferDesc::CreateStructuredDesc(
		sizeof(FVector4f),
		InNumGaussians * (sizeof(FGaussianSplatGPU) / sizeof(FVector4f)));
	const FRDGBufferDesc PositionDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), InNumGaussians);

	const uint32 SplatFloat4Count = InNumGaussians * (sizeof(FGaussianSplatGPU) / sizeof(FVector4f));

	if (!SplatPooledBuffer.IsValid() || SplatPooledBuffer->Desc.NumElements != SplatFloat4Count)
	{
		SplatPooledBuffer = AllocatePooledBuffer(SplatDesc, TEXT("Gaussian.SplatBuffer"));
		bDirty = true;
	}

	if (!PositionPooledBuffer.IsValid() || PositionPooledBuffer->Desc.NumElements != InNumGaussians)
	{
		PositionPooledBuffer = AllocatePooledBuffer(PositionDesc, TEXT("Gaussian.PositionBuffer"));
		bDirty = true;
	}
}

void FGaussianGPUBuffer::UploadToPooledBuffers(FRDGBuilder& GraphBuilder)
{
	if (!bDirty || NumGaussians == 0 || !SplatPooledBuffer.IsValid() || !PositionPooledBuffer.IsValid())
	{
		return;
	}

	FRDGBufferRef SplatUploadBuffer = CreateStructuredUploadBuffer(
		GraphBuilder,
		TEXT("Gaussian.SplatUpload"),
		SplatCPUData);

	FRDGBufferRef PositionUploadBuffer = CreateStructuredUploadBuffer(
		GraphBuilder,
		TEXT("Gaussian.PositionUpload"),
		PositionCPUData);

	FRDGBufferRef SplatDestBuffer = GraphBuilder.RegisterExternalBuffer(SplatPooledBuffer);
	FRDGBufferRef PositionDestBuffer = GraphBuilder.RegisterExternalBuffer(PositionPooledBuffer);

	AddCopyBufferPass(GraphBuilder, SplatDestBuffer, SplatUploadBuffer);
	AddCopyBufferPass(GraphBuilder, PositionDestBuffer, PositionUploadBuffer);

	bDirty = false;

	if (GaussianSimVerse::RenderSettings::IsGPUBufferDebugEnabled())
	{
		UE_LOG(LogGaussianSimVerse, Log, TEXT("Uploaded %u Gaussians to GPU buffer"), NumGaussians);
	}
}

void FGaussianGPUBuffer::CommitToGPU(FRDGBuilder& GraphBuilder, FGaussianRDGBufferBinding& OutBinding)
{
	OutBinding.SourceBuffer = this;
	OutBinding.SplatBuffer = nullptr;
	OutBinding.PositionBuffer = nullptr;
	OutBinding.SplatSRV = nullptr;
	OutBinding.PositionSRV = nullptr;
	OutBinding.NumGaussians = NumGaussians;

	if (NumGaussians == 0)
	{
		return;
	}

	if (!SplatPooledBuffer.IsValid() || !PositionPooledBuffer.IsValid())
	{
		EnsurePooledBuffers(NumGaussians);
	}

	UploadToPooledBuffers(GraphBuilder);

	OutBinding.SplatBuffer = GraphBuilder.RegisterExternalBuffer(SplatPooledBuffer);
	OutBinding.PositionBuffer = GraphBuilder.RegisterExternalBuffer(PositionPooledBuffer);
	OutBinding.SplatSRV = GraphBuilder.CreateSRV(OutBinding.SplatBuffer);
	OutBinding.PositionSRV = GraphBuilder.CreateSRV(OutBinding.PositionBuffer);
}
