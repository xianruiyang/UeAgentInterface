#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace UeAgentAuthoringFolderStats
{
	struct UEAGENTINTERFACE_API FCollectOptions
	{
		bool bIncludeHjsonFileCount = true;
		bool bIncludeJsonFileCount = true;
	};

	struct UEAGENTINTERFACE_API FStats
	{
		bool bFolderExists = false;
		int32 HjsonFileCount = 0;
		int32 JsonFileCount = 0;
	};

	UEAGENTINTERFACE_API FString NormalizeFolderPath(const FString& FolderPathInput);

	UEAGENTINTERFACE_API FStats CollectStats(
		const FString& FolderPathInput,
		const FCollectOptions& Options = FCollectOptions());

	UEAGENTINTERFACE_API int32 CountHjsonFiles(const FString& FolderPathInput);

	UEAGENTINTERFACE_API void SetExportStatsFields(
		const TSharedPtr<FJsonObject>& TargetObject,
		const FStats& Stats,
		const FCollectOptions& Options = FCollectOptions());

	UEAGENTINTERFACE_API void SetExportStatsFields(
		const TSharedPtr<FJsonObject>& TargetObject,
		const FString& FolderPathInput,
		const FCollectOptions& Options = FCollectOptions());
}
