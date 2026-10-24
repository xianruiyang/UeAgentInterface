#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

namespace UeAgentAuthoringFolderIO
{
	UEAGENTINTERFACE_API bool ResetExportFolder(
		const FString& FolderPathInput,
		FString& OutResolvedFolderPath,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool ResetExportFolder(
		const FString& FolderPathInput,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool CopyAuthoringTreeIfExists(
		const FString& SourceFolderPath,
		const FString& DestinationFolderPath,
		UE::UAI::Contract::FDiagnostic& OutError);
}
