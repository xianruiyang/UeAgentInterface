#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace UeAgentLevelFolderValidate
{
	struct UEAGENTINTERFACE_API FValidateOptions
	{
		FValidateOptions();

		FString InputFolder;
	};

	UEAGENTINTERFACE_API bool ValidateLevelFolder(
		const FValidateOptions& Options,
		TSharedPtr<FJsonObject>& OutSummary,
		FString& OutError);
}
