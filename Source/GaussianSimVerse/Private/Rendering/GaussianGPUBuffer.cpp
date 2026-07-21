// Copyright Epic Games, Inc. All Rights Reserved.

#include "Rendering/GaussianGPUBuffer.h"
#include "Rendering/GaussianRenderSettings.h"
#include "GaussianSimVerse.h"
#include "GaussianTypes.h"
#include "RenderGraphUtils.h"
#include "RHIResources.h"

void FGaussianGPUBuffer::SetCPUData(TArray<FGaussianSplatGPU>&& InSplatData)
{
	FScopeLock Lock(&DataLock);
	SplatCPUData = MoveTemp(InSplatData);
	GaussianGPU::BuildPositionBuffer(SplatCPUData, PositionCPUData);
	NumGaussians = SplatCPUData.Num();
	bHasShCpuData = (ShCoefficientCPUData.Num() == static_cast<int32>(NumGaussians * GaussianShCoefficientsPerSplat)) ? 1u : 0u;
	bDirty = true;
}

void FGaussianGPUBuffer::SetCPUDataFromStaging(const TArray<FGaussianSplatData>& StagingData)
{
	SetCPUDataFromStaging(StagingData, TArray<float>(), 0);
}

void FGaussianGPUBuffer::SetCPUDataFromStaging(
	const TArray<FGaussianSplatData>& StagingData,
	const TArray<float>& ShCoefficients,
	int32 InImportedShDegree)
{
	// Convert outside the lock (read-only source). All mutation of this buffer is under DataLock
	// so game-thread re-init cannot race render-thread Commit/Upload (was AV when dragging UI).
	TArray<FGaussianSplatGPU> Converted;
	GaussianGPU::ConvertSplatDataArray(StagingData, Converted);

	TArray<float> ShCopy;
	const int32 ExpectedSh = StagingData.Num() * GaussianShCoefficientsPerSplat;
	uint32 NewImportedShDegree = static_cast<uint32>(FMath::Clamp(InImportedShDegree, 0, 3));
	if (StagingData.Num() > 0 && ShCoefficients.Num() == ExpectedSh)
	{
		ShCopy = ShCoefficients;
	}
	else
	{
		NewImportedShDegree = 0;
	}

	TArray<FVector4f> Positions;
	GaussianGPU::BuildPositionBuffer(Converted, Positions);

	FScopeLock Lock(&DataLock);
	SplatCPUData = MoveTemp(Converted);
	PositionCPUData = MoveTemp(Positions);
	ShCoefficientCPUData = MoveTemp(ShCopy);
	ImportedShDegree = NewImportedShDegree;
	NumGaussians = static_cast<uint32>(SplatCPUData.Num());
	bHasShCpuData = (ShCoefficientCPUData.Num() == static_cast<int32>(NumGaussians * GaussianShCoefficientsPerSplat)) ? 1u : 0u;
	bDirty = true;
}

void FGaussianGPUBuffer::SetCPUDataPrepared(
	TArray<FGaussianSplatGPU>&& InSplatData,
	TArray<FVector4f>&& InPositions,
	TArray<float>&& InShCoefficients,
	int32 InImportedShDegree)
{
	FScopeLock Lock(&DataLock);
	// Steal storage (no realloc copy). Clear any previous staging first to free peak RAM.
	SplatCPUData.Empty();
	PositionCPUData.Empty();
	ShCoefficientCPUData.Empty();

	SplatCPUData = MoveTemp(InSplatData);
	if (InPositions.Num() == SplatCPUData.Num())
	{
		PositionCPUData = MoveTemp(InPositions);
	}
	else if (SplatCPUData.Num() > 0)
	{
		GaussianGPU::BuildPositionBuffer(SplatCPUData, PositionCPUData);
	}
	ImportedShDegree = static_cast<uint32>(FMath::Clamp(InImportedShDegree, 0, 3));
	ShCoefficientCPUData = MoveTemp(InShCoefficients);
	if (SplatCPUData.Num() > 0
		&& ShCoefficientCPUData.Num() != SplatCPUData.Num() * static_cast<int32>(GaussianShCoefficientsPerSplat))
	{
		ShCoefficientCPUData.Reset();
		ImportedShDegree = 0;
	}
	NumGaussians = static_cast<uint32>(SplatCPUData.Num());
	bHasShCpuData = (ShCoefficientCPUData.Num() == static_cast<int32>(NumGaussians * GaussianShCoefficientsPerSplat)) ? 1u : 0u;
	bDirty = true;
}

void FGaussianGPUBuffer::MarkDirty()
{
	bDirty = true;
}

void FGaussianGPUBuffer::ReleaseRenderResources()
{
	FScopeLock Lock(&DataLock);
	ReleasePooledBuffers();
	// Make the buffer inert: a stale render-thread scene proxy may still hold a shared ref to
	// this buffer for a frame after the asset is unloaded. Zeroing NumGaussians makes HasValidData()
	// return false so the stale proxy skips it, and prevents a re-commit from reallocating pooled
	// buffers and then trying to upload already-moved (empty) CPU staging (which crashes RDG).
	SplatCPUData.Empty();
	PositionCPUData.Empty();
	ShCoefficientCPUData.Empty();
	NumGaussians = 0;
	bHasShCpuData = 0;
	bDirty = false;
}

void FGaussianGPUBuffer::ReleasePooledBuffers()
{
	SplatPooledBuffer.SafeRelease();
	PositionPooledBuffer.SafeRelease();
	ShCoefficientPooledBuffer.SafeRelease();
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
	const uint32 ShCoefficientCount = InNumGaussians * GaussianShCoefficientsPerSplat;

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

	const bool bNeedShBuffer = bHasShCpuData != 0u;
	if (bNeedShBuffer)
	{
		const FRDGBufferDesc ShDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(float), ShCoefficientCount);
		if (!ShCoefficientPooledBuffer.IsValid() || ShCoefficientPooledBuffer->Desc.NumElements != ShCoefficientCount)
		{
			ShCoefficientPooledBuffer = AllocatePooledBuffer(ShDesc, TEXT("Gaussian.ShCoefficientBuffer"));
			bDirty = true;
		}
	}
	else
	{
		ShCoefficientPooledBuffer.SafeRelease();
	}
}

void FGaussianGPUBuffer::UploadToPooledBuffers(FRDGBuilder& GraphBuilder)
{
	TArray<FGaussianSplatGPU> UploadSplatData;
	TArray<FVector4f> UploadPositionData;
	TArray<float> UploadShData;
	uint32 UploadNumGaussians = 0;
	TRefCountPtr<FRDGPooledBuffer> LocalSplatBuffer;
	TRefCountPtr<FRDGPooledBuffer> LocalPositionBuffer;
	TRefCountPtr<FRDGPooledBuffer> LocalShBuffer;

	{
		FScopeLock Lock(&DataLock);
		// SplatCPUData/PositionCPUData are moved out (emptied) after the first successful upload.
		// If bDirty is re-set later (e.g. pooled buffers were reallocated) without new CPU data,
		// skip rather than enqueue a zero-element upload into RDG (which would assert/crash).
		if (!bDirty
			|| NumGaussians == 0
			|| SplatCPUData.Num() == 0
			|| PositionCPUData.Num() == 0
			|| !SplatPooledBuffer.IsValid()
			|| !PositionPooledBuffer.IsValid())
		{
			return;
		}

		// Move out of staging so we do not clone multi-MB arrays on the render thread every upload.
		UploadSplatData = MoveTemp(SplatCPUData);
		UploadPositionData = MoveTemp(PositionCPUData);
		UploadShData = MoveTemp(ShCoefficientCPUData);
		UploadNumGaussians = NumGaussians;
		LocalSplatBuffer = SplatPooledBuffer;
		LocalPositionBuffer = PositionPooledBuffer;
		if (ShCoefficientPooledBuffer.IsValid()
			&& UploadShData.Num() == static_cast<int32>(UploadNumGaussians * GaussianShCoefficientsPerSplat))
		{
			LocalShBuffer = ShCoefficientPooledBuffer;
		}
	}

	FRDGBufferRef SplatUploadBuffer = CreateStructuredUploadBuffer(
		GraphBuilder,
		TEXT("Gaussian.SplatUpload"),
		UploadSplatData);

	FRDGBufferRef PositionUploadBuffer = CreateStructuredUploadBuffer(
		GraphBuilder,
		TEXT("Gaussian.PositionUpload"),
		UploadPositionData);

	FRDGBufferRef SplatDestBuffer = GraphBuilder.RegisterExternalBuffer(LocalSplatBuffer);
	FRDGBufferRef PositionDestBuffer = GraphBuilder.RegisterExternalBuffer(LocalPositionBuffer);

	AddCopyBufferPass(GraphBuilder, SplatDestBuffer, SplatUploadBuffer);
	AddCopyBufferPass(GraphBuilder, PositionDestBuffer, PositionUploadBuffer);

	if (LocalShBuffer.IsValid())
	{
		FRDGBufferRef ShUploadBuffer = CreateStructuredUploadBuffer(
			GraphBuilder,
			TEXT("Gaussian.ShCoefficientUpload"),
			UploadShData);
		FRDGBufferRef ShDestBuffer = GraphBuilder.RegisterExternalBuffer(LocalShBuffer);
		AddCopyBufferPass(GraphBuilder, ShDestBuffer, ShUploadBuffer);
	}

	{
		FScopeLock Lock(&DataLock);
		bDirty = false;
		// CPU staging emptied after successful enqueue; keep NumGaussians / pooled buffers.
		SplatCPUData.Empty();
		PositionCPUData.Empty();
		ShCoefficientCPUData.Empty();
	}

	if (GaussianSimVerse::RenderSettings::IsGPUBufferDebugEnabled())
	{
		UE_LOG(LogGaussianSimVerse, Log, TEXT("Uploaded %u Gaussians to GPU buffer"), UploadNumGaussians);
	}
}

void FGaussianGPUBuffer::CommitToGPU(FRDGBuilder& GraphBuilder, FGaussianRDGBufferBinding& OutBinding)
{
	OutBinding.SourceBuffer = this;
	OutBinding.SplatBuffer = nullptr;
	OutBinding.PositionBuffer = nullptr;
	OutBinding.SplatSRV = nullptr;
	OutBinding.PositionSRV = nullptr;
	OutBinding.ShCoefficientsSRV = nullptr;
	OutBinding.NumGaussians = NumGaussians;
	OutBinding.ImportedShDegree = ImportedShDegree;
	OutBinding.bHasShCoefficients = 0u;

	if (NumGaussians == 0)
	{
		return;
	}

	{
		FScopeLock Lock(&DataLock);
		if (!SplatPooledBuffer.IsValid() || !PositionPooledBuffer.IsValid())
		{
			EnsurePooledBuffers(NumGaussians);
		}
	}

	UploadToPooledBuffers(GraphBuilder);

	OutBinding.SplatBuffer = GraphBuilder.RegisterExternalBuffer(SplatPooledBuffer);
	OutBinding.PositionBuffer = GraphBuilder.RegisterExternalBuffer(PositionPooledBuffer);
	OutBinding.SplatSRV = GraphBuilder.CreateSRV(OutBinding.SplatBuffer);
	OutBinding.PositionSRV = GraphBuilder.CreateSRV(OutBinding.PositionBuffer);
	OutBinding.ImportedShDegree = ImportedShDegree;

	if (ShCoefficientPooledBuffer.IsValid() && bHasShCpuData != 0u)
	{
		FRDGBufferRef ShBuffer = GraphBuilder.RegisterExternalBuffer(ShCoefficientPooledBuffer);
		OutBinding.ShCoefficientsSRV = GraphBuilder.CreateSRV(ShBuffer);
		OutBinding.bHasShCoefficients = 1u;
	}
}
