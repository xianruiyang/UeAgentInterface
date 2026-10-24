// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class UObject;

namespace UeAgentNiagaraStackValidation
{
	struct FFix
	{
		FString Id;
		FString Description;
	};

	struct FIssue
	{
		FString Id;
		UE::UAI::Contract::FDiagnostic Diagnostic;
		FString EntryKey;
		FString EntryName;
		FString EmitterHandleId;
		FString EmitterName;
		TArray<FFix> Fixes;
	};

	struct FResult
	{
		bool bInspectionComplete = false;
		int32 ErrorCount = 0;
		int32 WarningCount = 0;
		UE::UAI::Contract::FDiagnostic InspectionDiagnostic;
		TArray<FIssue> Issues;
		UEAGENTINTERFACE_API UE::UAI::Contract::FDiagnostic GetFailure(bool bIncludeWarnings = false) const;

		int32 GetBlockingIssueCount() const
		{
			return ErrorCount + WarningCount;
		}
	};

	UEAGENTINTERFACE_API FResult InspectAsset(UObject* Asset, bool bIncludeIssues = false);
}
