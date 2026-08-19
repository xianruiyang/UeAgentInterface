// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceLoader.h"

class FJsonObject;
class UWorld;

namespace UeAgentAuthoringOperationApply
{
	class IApplyProvider;
}

namespace UeAgentLevelAuthoring
{
	struct UEAGENTINTERFACE_API FWorkspaceApplyOptions
	{
		bool bDryRun = false;
		bool bUseEditorTransaction = true;
	};

	struct UEAGENTINTERFACE_API FWorkspaceApplyResult
	{
		FString Status;
		FString Phase;
		FString ErrorCode;
		FString ErrorDetail;
		int32 UnitsChecked = 0;
		int32 MutationsApplied = 0;
		int32 ActorsCreated = 0;
		int32 ActorsUpdated = 0;
		int32 ActorsRemoved = 0;
		int32 ComponentsCreated = 0;
		int32 ComponentsUpdated = 0;
		int32 ComponentsRemoved = 0;
		bool bRollbackAttempted = false;
		bool bRollbackSucceeded = false;

		bool IsSuccessful() const
		{
			return Status == TEXT("applied") || Status == TEXT("ready");
		}
		TSharedRef<FJsonObject> ToJsonObject() const;
	};

	UEAGENTINTERFACE_API bool RegisterApplyContracts(FString& OutError);
	UEAGENTINTERFACE_API TSharedRef<UeAgentAuthoringOperationApply::IApplyProvider> MakeApplyProvider();
	UEAGENTINTERFACE_API bool ApplyWorkspaceSource(
		UWorld& World,
		const UeAgentAuthoringWorkspaceLoader::FSourceRequest& Source,
		const FWorkspaceApplyOptions& Options,
		FWorkspaceApplyResult& OutResult,
		const FString& RequestId = FString());
	UEAGENTINTERFACE_API bool ApplyWorkspaceDirectory(
		UWorld& World,
		const FString& WorkspaceRoot,
		const FWorkspaceApplyOptions& Options,
		FWorkspaceApplyResult& OutResult,
		const FString& RequestId = FString());
	UEAGENTINTERFACE_API bool ValidateWorkspaceDirectory(
		UWorld& World,
		const FString& WorkspaceRoot,
		FWorkspaceApplyResult& OutResult);
	UEAGENTINTERFACE_API bool DiffWorkspaceDirectories(
		const FString& BaseRoot,
		const FString& TargetRoot,
		int32 MaxChanges,
		TSharedPtr<FJsonObject>& OutSummary,
		FString& OutError);
}
