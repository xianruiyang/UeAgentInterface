// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraStageModuleAuthoring.h"
#include "Utilities/AuthoringOperationApply/UeAgentApplyInputContract.h"

class FJsonObject;

namespace UeAgentAuthoringOperationApply
{
	class IApplyProvider;
}

namespace UeAgentNiagaraAuthoring
{
	enum class EStackTargetKind : uint8
	{
		Module,
		DynamicInput,
	};

	struct UEAGENTINTERFACE_API FStackApplyRequest
	{
		UeAgentApplyInputContract::EOperation Operation = UeAgentApplyInputContract::EOperation::Update;
		EStackTargetKind TargetKind = EStackTargetKind::Module;
		UeAgentNiagaraStageModuleAuthoring::FStageTarget Stage;
		UeAgentNiagaraStageModuleAuthoring::FModuleSelector Module;
		UeAgentNiagaraStageModuleAuthoring::FModuleSelector SourceModule;
		FString InputName;
		TSharedPtr<FJsonObject> ScriptReference;
		int32 TargetIndex = INDEX_NONE;
		FString SuggestedName;
		bool bCompile = false;
		bool bSave = false;
	};

	struct UEAGENTINTERFACE_API FStackApplyResult
	{
		FString Status;
		FString Phase;
		FString ErrorCode;
		FString ErrorDetail;
		FString ResolvedOperation;
		FString AssetPath;
		FString StageKey;
		FString TargetGuid;
		FString ResolvedInputName;
		int32 ModuleIndex = INDEX_NONE;
		bool bRollbackAttempted = false;
		bool bRollbackSucceeded = false;

		bool IsApplied() const { return Status == TEXT("applied"); }
	};

	UEAGENTINTERFACE_API bool RegisterStackApplyContracts(FString& OutError);
	UEAGENTINTERFACE_API TSharedRef<UeAgentAuthoringOperationApply::IApplyProvider> MakeStackApplyProvider();
	UEAGENTINTERFACE_API bool ApplyStack(
		const FStackApplyRequest& Request,
		FStackApplyResult& OutResult,
		const FString& RequestId = FString());
}
