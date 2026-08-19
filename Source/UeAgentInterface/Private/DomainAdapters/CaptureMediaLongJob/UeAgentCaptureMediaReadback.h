#pragma once

#include "CoreMinimal.h"
#include "Core/Verification/UeAgentIndependentReadback.h"

class UTextureRenderTarget2D;

#define UAI_CAPTURE_MEDIA_READBACK_V1 1

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	UEAGENTINTERFACE_API bool
	TryCreateCaptureImageArtifactReadbackOracle(
		FString OracleId,
		FString SubjectRef,
		TSharedPtr<
			Verification::IIndependentReadbackOracle,
			ESPMode::ThreadSafe>& OutOracle,
		Verification::FVerificationError& OutError);

	UEAGENTINTERFACE_API bool
	TryCreateCaptureWaveArtifactReadbackOracle(
		FString OracleId,
		FString SubjectRef,
		TSharedPtr<
			Verification::IIndependentReadbackOracle,
			ESPMode::ThreadSafe>& OutOracle,
		Verification::FVerificationError& OutError);

	UEAGENTINTERFACE_API bool
	TryCreateCaptureRenderTargetReadbackOracle(
		FString OracleId,
		FString SubjectRef,
		UTextureRenderTarget2D* RenderTarget,
		TSharedPtr<
			Verification::IIndependentReadbackOracle,
			ESPMode::ThreadSafe>& OutOracle,
		Verification::FVerificationError& OutError);
}

