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
	};

	UEAGENTINTERFACE_API bool ExportLevelFolder(
		UWorld* World,
		const FExportOptions& Options,
		TSharedPtr<FJsonObject>& OutSummary,
		FString& OutError);
}
