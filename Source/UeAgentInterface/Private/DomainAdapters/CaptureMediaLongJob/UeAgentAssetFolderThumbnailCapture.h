#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/Contract/UeAgentMechanicsRequestContext.h"

class FJsonObject;

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	FString GetAssetFolderPreviewOptionsSchema();

	bool TryCaptureAssetFolderThumbnails(
		const FUeAgentRequestContext& Context,
		TSharedPtr<FJsonObject>& OutResult,
		Contract::FDiagnostic& OutError);
}
