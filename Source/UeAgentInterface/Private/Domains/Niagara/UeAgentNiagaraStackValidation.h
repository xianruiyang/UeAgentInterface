// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UObject;

namespace UeAgentNiagaraStackValidation
{
	struct FResult
	{
		bool bInspectionComplete = false;
		int32 ErrorCount = 0;
		int32 WarningCount = 0;
		FString FirstError;

		int32 GetBlockingIssueCount() const
		{
			return ErrorCount + WarningCount;
		}
	};

	UEAGENTINTERFACE_API FResult InspectAsset(UObject* Asset);
}
