#pragma once

#include "CoreMinimal.h"

class UWorld;
class FJsonObject;

namespace UeAgentLevelFolderExport
{
	struct UEAGENTINTERFACE_API FExportOptions
	{
		FExportOptions();

		FString OutputFolder;
		FString Profile;
		bool bOverwrite;
		bool bSplitProperties;
	};

	UEAGENTINTERFACE_API bool ExportLevelFolder(
		UWorld* World,
		const FExportOptions& Options,
		TSharedPtr<FJsonObject>& OutSummary,
		FString& OutError);
}
