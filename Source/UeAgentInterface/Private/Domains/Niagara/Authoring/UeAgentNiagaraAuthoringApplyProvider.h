// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringOperationApply/UeAgentApplyInputContract.h"

class FJsonValue;
class UObject;

namespace UeAgentAuthoringOperationApply
{
	class IApplyProvider;
}

namespace UeAgentNiagaraAuthoring
{
	struct UEAGENTINTERFACE_API FAssetApplyRequest
	{
		UeAgentApplyInputContract::EOperation Operation = UeAgentApplyInputContract::EOperation::Update;
		FString AssetPath;
		FString AssetKind;
		FString SourceAssetPath;
		TArray<TSharedPtr<FJsonValue>> Properties;
		bool bCreateDefaultSystemNodes = true;
		bool bAddDefaultEmitterModulesAndRenderers = true;
		bool bCompile = true;
		bool bSave = true;
		bool bForceDelete = false;
		bool bUseUncheckedDelete = false;
	};

	struct UEAGENTINTERFACE_API FAssetApplyResult
	{
		FString Status;
		FString Phase;
		FString ErrorCode;
		FString ErrorDetail;
		FString ResolvedOperation;
		FString AssetPath;
		FString ObjectPath;
		FString AssetKind;
		FString CompileStatus;
		UObject* Asset = nullptr;
		TArray<FString> CompiledTargets;
		int32 CompileErrorCount = 0;
		int32 CompileWarningCount = 0;
		int32 StackIssueCount = 0;
		int32 StackWarningCount = 0;
		int32 RepairedEmitterVersionCount = 0;
		int32 ClosedEditorCount = 0;
		int32 UnitsChecked = 0;
		int32 MutationsApplied = 0;
		bool bCompilationComplete = false;
		bool bSaved = false;
		bool bEditorIsolationPerformed = false;
		bool bRollbackAttempted = false;
		bool bRollbackSucceeded = false;

		bool IsApplied() const
		{
			return Status == TEXT("complete")
				|| Status == TEXT("applied_in_memory");
		}

		bool CanReportComplete() const
		{
			if (!bSaved)
			{
				return false;
			}
			if (ResolvedOperation == TEXT("remove"))
			{
				return true;
			}
			return CompileStatus == TEXT("complete")
				&& bCompilationComplete
				&& CompileErrorCount == 0
				&& StackIssueCount == 0
				&& !CompiledTargets.IsEmpty();
		}
	};

	UEAGENTINTERFACE_API bool RegisterAssetApplyContracts(FString& OutError);
	UEAGENTINTERFACE_API FString GetApplyFailureMessage(const FAssetApplyResult& Result);
	UEAGENTINTERFACE_API TSharedRef<UeAgentAuthoringOperationApply::IApplyProvider> MakeAssetApplyProvider();
	UEAGENTINTERFACE_API bool CompileAsset(UObject* Asset, FString& OutError);
	UEAGENTINTERFACE_API bool SaveAsset(UObject* Asset, FString& OutError);
	UEAGENTINTERFACE_API bool ApplyAsset(
		const FAssetApplyRequest& Request,
		FAssetApplyResult& OutResult,
		const FString& RequestId = FString());
	UEAGENTINTERFACE_API bool ApplyAssetWorkspace(
		const FString& WorkspaceRoot,
		const FString& ExpectedAssetKind,
		const FString& ExpectedAssetPath,
		FAssetApplyResult& OutResult,
		const FString& RequestId = FString());
}
