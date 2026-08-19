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
	};

	UEAGENTINTERFACE_API bool ApplyLevelFolder(
		UWorld* World,
		const FApplyOptions& Options,
		TSharedPtr<FJsonObject>& OutSummary,
		FString& OutError,
		const FString& RequestId = FString());
}
