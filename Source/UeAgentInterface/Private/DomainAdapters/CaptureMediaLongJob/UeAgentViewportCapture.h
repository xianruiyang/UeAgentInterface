#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentMechanicsRequestContext.h"

class FJsonObject;
class FEditorViewportClient;
class FViewport;

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	/** Owns active Level Viewport image and debug-buffer capture mechanics. */
	class UEAGENTINTERFACE_API FViewportCapture final
	{
	public:
		static bool Capture(
			const FUeAgentRequestContext& Context,
			TSharedPtr<FJsonObject>& OutData,
			UE::UAI::Contract::FDiagnostic& OutError);

		static bool CaptureBuffer(
			const FUeAgentRequestContext& Context,
			TSharedPtr<FJsonObject>& OutData,
			UE::UAI::Contract::FDiagnostic& OutError);

		static bool TakeActiveViewportScreenshot(
			TArray<FColor>& OutPixels,
			FIntPoint& OutSize,
			UE::UAI::Contract::FDiagnostic& OutError);

		static bool TakeActiveViewportScreenshotWithWarmup(
			int32 WarmupFrames,
			TArray<FColor>& OutPixels,
			FIntPoint& OutSize,
			TSharedPtr<FJsonObject>& OutCaptureInfo,
			UE::UAI::Contract::FDiagnostic& OutError);

		static FString MakeShotPath(const FString& Extension);
		static bool TakeViewportScreenshot(FEditorViewportClient& Client, int32 WarmupFrames,
			TArray<FColor>& OutPixels, FIntPoint& OutSize,
			TSharedPtr<FJsonObject>& OutCaptureInfo, UE::UAI::Contract::FDiagnostic& OutError);
		static bool TakeViewportScreenshot(FViewport& Viewport, int32 WarmupFrames,
			TArray<FColor>& OutPixels, FIntPoint& OutSize,
			TSharedPtr<FJsonObject>& OutCaptureInfo, UE::UAI::Contract::FDiagnostic& OutError);
	};

	UEAGENTINTERFACE_API bool TryCaptureViewportImage(
		const TSharedPtr<FJsonObject>& Parameters,
		const FString& AssetNotFoundError,
		TSharedPtr<FJsonObject>& OutData,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryCaptureViewportBufferImage(
		const TSharedPtr<FJsonObject>& Parameters,
		const FString& AssetNotFoundError,
		TSharedPtr<FJsonObject>& OutData,
		UE::UAI::Contract::FDiagnostic& OutError);
}
