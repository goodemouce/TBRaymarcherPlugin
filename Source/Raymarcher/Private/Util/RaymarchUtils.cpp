// Copyright 2021 Tomas Bartipan and Technical University of Munich.
// Licensed under MIT license - See License.txt for details.
// Special credits go to : Temaran (compute shader tutorial), TheHugeManatee (original concept, supervision) and Ryan Brucks
// (original raymarching code).

#include "Util/RaymarchUtils.h"

#include "Containers/UnrealString.h"
#include "GlobalShader.h"
#include "Logging/MessageLog.h"
#include "PipelineStateCache.h"
#include "RHICommandList.h"
#include "RHIDefinitions.h"
#include "RHIStaticStates.h"
#include "Rendering/LightingShaders.h"
#include "Rendering/RaymarchTypes.h"
#include "SceneInterface.h"
#include "SceneUtils.h"
#include "ShaderParameterUtils.h"
#include "Rendering/OctreeShaders.h"
#include "VolumeTextureToolkit/Public/TextureUtilities.h"

#include <Engine/TextureRenderTargetVolume.h>

#include <cstdio>

#define LOCTEXT_NAMESPACE "RaymarchPlugin"

// void URaymarchUtils::InitLightVolume(UVolumeTexture*& LightVolume, FIntVector Dimensions)
// {
// 	// FMemory::Memset(InitMemory, 1, TotalSize);
// 	CreateVolumeTextureTransient(LightVolume, PF_R32_FLOAT, Dimensions, nullptr);
// }

void URaymarchUtils::AddDirLightToSingleVolume(const FBasicRaymarchRenderingResources& Resources,
	const FDirLightParameters& LightParameters, const bool Added, const FRaymarchWorldParameters WorldParameters, bool& LightAdded,
	bool bGPUSync)
{
	if (!Resources.DataVolumeTextureRef || !Resources.DataVolumeTextureRef->GetResource() || !Resources.TFTextureRef->GetResource() ||
		!Resources.LightVolumeRenderTarget->GetResource() || !Resources.DataVolumeTextureRef->GetResource()->TextureRHI ||
		!Resources.TFTextureRef->GetResource()->TextureRHI || !Resources.LightVolumeRenderTarget->GetResource()->TextureRHI)
	{
		LightAdded = false;
		return;
	}
	else
	{
		LightAdded = true;
	}

	if (bGPUSync)
	{
		// Call the actual rendering code on RenderThread.
		// #todo fix GPUSync'd version of shader.
		//ENQUEUE_RENDER_COMMAND(CaptureCommand)
		//([=](FRHICommandListImmediate& RHICmdList) {
		//	AddDirLightToSingleLightVolume_GPUSync_RenderThread(RHICmdList, Resources, LightParameters, Added, WorldParameters);
		//});
	}
	else
	{
		// Call the actual rendering code on RenderThread.
		ENQUEUE_RENDER_COMMAND(CaptureCommand)
		([=](FRHICommandListImmediate& RHICmdList) {
			AddDirLightToSingleLightVolume_RenderThread(RHICmdList, Resources, LightParameters, Added, WorldParameters);
		});
	}
}

void URaymarchUtils::ChangeDirLightInSingleVolume(FBasicRaymarchRenderingResources& Resources,
	const FDirLightParameters OldLightParameters, const FDirLightParameters NewLightParameters,
	const FRaymarchWorldParameters WorldParameters, bool& LightAdded, bool bGpuSync)
{
	if (!Resources.DataVolumeTextureRef || !Resources.DataVolumeTextureRef->GetResource() || !Resources.TFTextureRef->GetResource() ||
		!Resources.LightVolumeRenderTarget->GetResource() || !Resources.DataVolumeTextureRef->GetResource()->TextureRHI ||
		!Resources.TFTextureRef->GetResource()->TextureRHI || !Resources.LightVolumeRenderTarget->GetResource()->TextureRHI)
	{
		LightAdded = false;
		return;
	}
	else
	{
		LightAdded = true;
	}

	// Call the actual rendering code on RenderThread. We capture by value so that if
	ENQUEUE_RENDER_COMMAND(CaptureCommand)
	([=](FRHICommandListImmediate& RHICmdList) {
		ChangeDirLightInSingleLightVolume_RenderThread(
			RHICmdList, Resources, OldLightParameters, NewLightParameters, WorldParameters);
	});
}

void URaymarchUtils::GenerateOctree(FBasicRaymarchRenderingResources& Resources)
{
	// Call the actual rendering code on RenderThread. We capture by value so that if
	ENQUEUE_RENDER_COMMAND(CaptureCommand)
	([=](FRHICommandListImmediate& RHICmdList)
	{
		GenerateOctreeForVolume_RenderThread(RHICmdList, Resources);
	});
}


// Function to sample from the Texture2D
FFloat16Color SampleFromTexture(float U, float V, UTexture2D* TF)
{
	// Ensure the Texture2D is valid
	if (TF == nullptr)
	{
		return {};
	}
	
	int32 TextureWidth = TF->GetSizeX();
	int32 TextureHeight = TF->GetSizeY();
	int32 X = FMath::Clamp(FMath::RoundToInt(U * TextureWidth), 0, TextureWidth - 1);
	int32 Y = FMath::Clamp(FMath::RoundToInt(V * TextureHeight), 0, TextureHeight - 1);

	
	// Get the texture data
	FTexture2DMipMap* MipMap = &TF->GetPlatformData()->Mips[0];
	const void* TextureData = MipMap->BulkData.LockReadOnly();

	// Calculate the index in the texture data
	int32 TextureDataIndex = (Y * MipMap->SizeX) + X;

	// Sample the color
	const FFloat16Color* ColorData = static_cast<const FFloat16Color*>(TextureData);
	FFloat16Color SampleColor = ColorData[TextureDataIndex];

	// Unlock the texture data
	MipMap->BulkData.Unlock();

	return SampleColor;
}

FVector4 URaymarchUtils::GetWindowingParamsBitNumber(FWindowingParameters WindowingParams, int EdgeBites, UTexture2D* TF)
{
	// TFPos == 1 => Value = WindowWidth - WindowWidth/2 + WindowCenter
	// TFPos == 0 => Value = WindowCenter - (WindowWidth/2);
	using uint = unsigned int;
	
	float Value0 = WindowingParams.Center - (WindowingParams.Width / 2.0);
	float Value1 = WindowingParams.Center + (WindowingParams.Width / 2.0);
    
	const float Factor = 1.0/31.0;
	uint Value0Bit = uint(Value0/Factor);
	uint Value1Bit = uint(Value1/Factor);

	//check(Value0Bit > Value1Bit);
	if(Value0Bit > Value1Bit)
	{
		Swap(Value0Bit, Value1Bit);
	}

	if (!WindowingParams.LowCutoff)
	{
		Value0Bit = 0;
	}

	if(!WindowingParams.HighCutoff)
	{
		Value1Bit = 31;
	}
	
	
	
	uint Result = 0;
	for(uint i = Value0Bit; i <= Value1Bit; i++)
	{
		FFloat16Color TFColor = SampleFromTexture(Factor * i, 0.5, TF);
		if (TFColor.A != 0)
		{
			uint n = (1 << i);
			Result |= n;
		}
	}

	for(int k = 0; k < EdgeBites; k++)
	{
		Result |= (Result << 1);
		Result |= (Result >> 1);
	}
	GEngine->AddOnScreenDebugMessage(54,10,FColor::Orange, FString::Printf(TEXT("%u"),Result));
	return FVector4(static_cast<float>(Result),0,0,0 );
}


void URaymarchUtils::ClearResourceLightVolumes(const FBasicRaymarchRenderingResources Resources, float ClearValue)
{
	if (!Resources.LightVolumeRenderTarget)
	{
		return;
	}
	UVolumeTextureToolkit::ClearVolumeTexture(Resources.LightVolumeRenderTarget, ClearValue);
}

RAYMARCHER_API void URaymarchUtils::MakeDefaultTFTexture(UTexture2D*& OutTexture)
{
	const unsigned SampleCount = 256;

	// Give the texture some height, so it can be inspected in the asset editor.
	const unsigned TextureHeight = 1;

	FFloat16* Samples = new FFloat16[SampleCount * 4 * TextureHeight];
	for (unsigned i = 0; i < SampleCount; i++)
	{
		float Whiteness = (float) i / (float) (SampleCount - 1);
		Samples[i * 4] = Whiteness;
		Samples[i * 4 + 1] = Whiteness;
		Samples[i * 4 + 2] = Whiteness;
		Samples[i * 4 + 3] = 1;
	}

	for (unsigned i = 1; i < TextureHeight; i++)
	{
		FMemory::Memcpy(Samples + (i * SampleCount * 4), Samples, SampleCount * 4 * 2);
	}

	UVolumeTextureToolkit::Create2DTextureTransient(
		OutTexture, PF_FloatRGBA, FIntPoint(SampleCount, TextureHeight), (uint8*) Samples);

	// Don't forget to free the memory here
	delete[] Samples;
	return;
}

void URaymarchUtils::ColorCurveToTexture(UCurveLinearColor* Curve, UTexture2D*& OutTexture)
{
	const unsigned sampleCount = 256;

	// Give the texture some height, so it can be inspected in the asset editor. Possibly breaks cache consistency
	const unsigned TextureHeight = 16;

	// Using float16 format because RGBA8 wouldn't be persistent for some reason.
	FFloat16* samples = new FFloat16[sampleCount * 4 * TextureHeight];

	for (unsigned i = 0; i < sampleCount; i++)
	{
		float index = ((float) i) / ((float) sampleCount - 1);
		FLinearColor picked = Curve->GetLinearColorValue(index);

		samples[i * 4] = picked.R;
		samples[i * 4 + 1] = picked.G;
		samples[i * 4 + 2] = picked.B;
		samples[i * 4 + 3] = picked.A;
	}

	for (unsigned i = 1; i < TextureHeight; i++)
	{
		FMemory::Memcpy(samples + (i * sampleCount * 4), samples, sampleCount * 4 * 2);
	}

	UVolumeTextureToolkit::Create2DTextureTransient(
		OutTexture, PF_FloatRGBA, FIntPoint(sampleCount, TextureHeight), (uint8*) samples);

	delete[] samples;	 // Don't forget to free the memory here
	return;
}

void URaymarchUtils::CreateBufferTextures(FIntPoint Size, EPixelFormat PixelFormat, OneAxisReadWriteBufferResources& RWBuffers)
{
	if (Size.X == 0 || Size.Y == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("Warning: Creating Buffer Textures: Size is Zero!"), 3);
		return;
	}

	FRHITextureCreateDesc Desc =
		FRHITextureCreateDesc::Create2D(TEXT("Illumination Buffer"), Size.X, Size.Y, PixelFormat);
	Desc.Flags |= TexCreate_ShaderResource | TexCreate_UAV;
	Desc.NumMips = 1;
	Desc.NumSamples = 1;
	
	for (int i = 0; i < 4; i++)
	{
		RWBuffers.Buffers[i] =
			RHICreateTexture(Desc);
		RWBuffers.UAVs[i] = GetCmdList().CreateUnorderedAccessView(RWBuffers.Buffers[i]);
	}
}

void URaymarchUtils::ReleaseOneAxisReadWriteBufferResources(OneAxisReadWriteBufferResources& Buffer)
{
	for (FUnorderedAccessViewRHIRef& UAV : Buffer.UAVs)
	{
		if (UAV)
		{
			UAV.SafeRelease();
		}
		UAV = nullptr;
	}

	for (FTexture2DRHIRef& TextureRef : Buffer.Buffers)
	{
		if (TextureRef)
		{
			TextureRef.SafeRelease();
		}
		TextureRef = nullptr;
	}
}

void URaymarchUtils::GetVolumeTextureDimensions(UVolumeTexture* Texture, FIntVector& Dimensions)
{
	if (Texture)
	{
		// This is slightly retarded...
		Dimensions = FIntVector(Texture->GetSizeX(), Texture->GetSizeY(), Texture->GetSizeZ());
	}
	else
	{
		Dimensions = FIntVector(0, 0, 0);
	}
}

void URaymarchUtils::TransformToMatrix(const FTransform Transform, FMatrix& OutMatrix, bool WithScaling)
{
	if (WithScaling)
	{
		OutMatrix = Transform.ToMatrixWithScale();
	}
	else
	{
		OutMatrix = Transform.ToMatrixNoScale();
	}
}

void URaymarchUtils::LocalToTextureCoords(FVector LocalCoords, FVector& TextureCoords)
{
	TextureCoords = (LocalCoords / 2.0f) + 0.5f;
}

void URaymarchUtils::TextureToLocalCoords(FVector TextureCoors, FVector& LocalCoords)
{
	LocalCoords = (TextureCoors - 0.5f) * 2.0f;
}

#undef LOCTEXT_NAMESPACE