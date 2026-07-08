#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UWorld;

namespace UeAgentLevelFolderApply
{
	struct UEAGENTINTERFACE_API FApplyOptions
	{
		FApplyOptions();

		FString InputFolder;
		bool bCreateMissingActors;
	};

	UEAGENTINTERFACE_API bool ApplyLevelFolder(
		UWorld* World,
		const FApplyOptions& Options,
		TSharedPtr<FJsonObject>& OutSummary,
		FString& OutError);
}
