#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace UeAgentLevelFolderDiff
{
	struct UEAGENTINTERFACE_API FDiffOptions
	{
		FDiffOptions();

		FString BaseFolder;
		FString TargetFolder;
		int32 MaxChanges;
	};

	UEAGENTINTERFACE_API bool DiffLevelFolders(
		const FDiffOptions& Options,
		TSharedPtr<FJsonObject>& OutSummary,
		FString& OutError);
}
