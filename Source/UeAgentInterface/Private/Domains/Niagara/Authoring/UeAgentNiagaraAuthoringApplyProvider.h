// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Utilities/AuthoringOperationApply/UeAgentApplyInputContract.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"

class FJsonValue;
class FJsonObject;
class UObject;

namespace UeAgentAuthoringOperationApply
{
	class IApplyProvider;
	class IDurablePackageStager;
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
		TSharedPtr<FJsonObject> Stages;
		TSharedPtr<FJsonObject> Emitters;
		TFunction<bool(UeAgentAuthoringWorkspaceTransaction::FOperationFailure&)> FinalReadback;
		TSharedPtr<UeAgentAuthoringOperationApply::IDurablePackageStager> PackageStager;
		bool bCreateDefaultSystemNodes = true;
		bool bAddDefaultEmitterModulesAndRenderers = true;
		bool bCompile = true;
		bool bSave = true;
		bool bCompactValues = false;
		bool bForceDelete = false;
		bool bUseUncheckedDelete = false;
		TSharedPtr<FJsonObject> UserParameters;
	};

	struct UEAGENTINTERFACE_API FAssetApplyResult
	{
		FString Status;
		FString Phase;
		UE::UAI::Contract::FDiagnostic Diagnostic;
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

	UEAGENTINTERFACE_API bool RegisterAssetApplyContracts(UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API FString GetApplyFailureMessage(const FAssetApplyResult& Result);
	UEAGENTINTERFACE_API TSharedRef<UeAgentAuthoringOperationApply::IApplyProvider> MakeAssetApplyProvider(bool bCompactValues = false);
	UEAGENTINTERFACE_API bool CompileAsset(UObject* Asset, UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool SaveAsset(UObject* Asset, UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool SaveAsset(UObject* Asset, UE::UAI::Contract::FDiagnostic& OutError, FString* OutDetail);
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
	UEAGENTINTERFACE_API bool ApplyAssetInlineDocument(
		const TSharedPtr<const FJsonObject>& Document,
		const FString& ExpectedAssetKind,
		const FString& ExpectedAssetPath,
		FAssetApplyResult& OutResult,
		const FString& RequestId = FString(),
		bool bCompactValues = false,
		TFunction<bool(UeAgentAuthoringWorkspaceTransaction::FOperationFailure&)> FinalReadback = {},
		const TSharedPtr<UeAgentAuthoringOperationApply::IDurablePackageStager>& PackageStager = nullptr);
}
