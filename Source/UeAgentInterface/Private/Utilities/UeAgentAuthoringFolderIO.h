#pragma once

#include "CoreMinimal.h"

namespace UeAgentAuthoringFolderIO
{
	UEAGENTINTERFACE_API bool ResetExportFolder(
		const FString& FolderPathInput,
		FString& OutResolvedFolderPath,
		FString& OutError);

	UEAGENTINTERFACE_API bool ResetExportFolder(
		const FString& FolderPathInput,
		FString& OutError);

	UEAGENTINTERFACE_API bool CopyAuthoringTreeIfExists(
		const FString& SourceFolderPath,
		const FString& DestinationFolderPath,
		FString& OutError);
}
