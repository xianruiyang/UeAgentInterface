#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FJsonObject;

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	/**
	 * Captures an arbitrary asset editor window through Slate's offscreen
	 * widget renderer. This owner is independent of the asset's domain; the
	 * caller supplies only the asset path, output parameters, and the
	 * domain-facing not-found error token.
	 */
	bool TryCaptureAssetEditorImage(
		const TSharedPtr<FJsonObject>& Parameters,
		const FString& AssetNotFoundError,
		TSharedPtr<FJsonObject>& OutData,
		UE::UAI::Contract::FDiagnostic& OutError);
}
