#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FJsonObject;

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	class UEAGENTINTERFACE_API FCaptureImageProcessing final
	{
	public:
		static bool ResizePixelsMaxSize(const TArray<FColor>& InPixels, const FIntPoint& InSize, int32 MaxSize, TArray<FColor>& OutPixels, FIntPoint& OutSize, UE::UAI::Contract::FDiagnostic& OutError);
		static bool ResizePixelsToSize(const TArray<FColor>& InPixels, const FIntPoint& InSize, const FIntPoint& RequestedSize, TArray<FColor>& OutPixels, FIntPoint& OutSize, UE::UAI::Contract::FDiagnostic& OutError);
		static bool WriteCompressedImage(const TArray<FColor>& Pixels, const FIntPoint& Size, const FString& Format, int32 Quality, const FString& OutFilePath, UE::UAI::Contract::FDiagnostic& OutError, bool bForceOpaqueAlpha = true);
		static bool WriteFrameSequenceSheetImage(const TArray<TArray<FColor>>& FramePixels, const TArray<FString>& FrameLabels, const FIntPoint& FrameSize, const TSharedPtr<FJsonObject>& Params, const FString& Format, int32 Quality, FString& OutFilePath, TSharedPtr<FJsonObject>& OutSheetData, UE::UAI::Contract::FDiagnostic& OutError);
	};
}
