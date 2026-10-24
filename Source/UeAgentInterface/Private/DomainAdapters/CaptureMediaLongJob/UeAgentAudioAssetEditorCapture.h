#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FJsonObject;

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	/** Captures a MetaSound asset editor while preserving the audio policy envelope. */
	UEAGENTINTERFACE_API bool TryCaptureMetaSoundEditorImage(
		const TSharedPtr<FJsonObject>& Parameters,
		const FString& AssetNotFoundError,
		TSharedPtr<FJsonObject>& OutData,
		UE::UAI::Contract::FDiagnostic& OutError);

	/** Captures a Sound Cue asset editor while preserving the audio policy envelope. */
	UEAGENTINTERFACE_API bool TryCaptureSoundCueEditorImage(
		const TSharedPtr<FJsonObject>& Parameters,
		const FString& AssetNotFoundError,
		TSharedPtr<FJsonObject>& OutData,
		UE::UAI::Contract::FDiagnostic& OutError);
}
