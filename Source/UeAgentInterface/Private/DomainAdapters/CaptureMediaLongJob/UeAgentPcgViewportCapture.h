#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FJsonObject;

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	/**
	 * Captures the active viewport for a PCG subject, optionally framing the
	 * requested Actor first. Viewport framing and image capture remain owned by
	 * their existing shared mechanics; this adapter owns only the PCG policy
	 * projection and result envelope.
	 */
	UEAGENTINTERFACE_API bool TryCapturePcgViewportImage(
		const TSharedPtr<FJsonObject>& Parameters,
		const FString& AssetNotFoundError,
		TSharedPtr<FJsonObject>& OutData,
		UE::UAI::Contract::FDiagnostic& OutError);
}
