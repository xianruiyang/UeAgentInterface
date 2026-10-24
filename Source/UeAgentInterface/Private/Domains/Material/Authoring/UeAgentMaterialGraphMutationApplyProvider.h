// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Domains/Material/Authoring/UeAgentMaterialGraphAuthoring.h"
#include "Utilities/AuthoringOperationApply/UeAgentApplyInputContract.h"

class FJsonValue;

namespace UeAgentAuthoringOperationApply
{
	class IApplyProvider;
}

namespace UeAgentMaterialAuthoring
{
	enum class EGraphMutationKind : uint8
	{
		Expression,
		Connection,
		RootInput,
	};

	struct UEAGENTINTERFACE_API FGraphMutationRequest
	{
		UeAgentApplyInputContract::EOperation Operation = UeAgentApplyInputContract::EOperation::Update;
		EGraphMutationKind Kind = EGraphMutationKind::Expression;
		FString AssetPath;
		FExpressionSelector Expression;
		FExpressionSelector FromExpression;
		FExpressionSelector ToExpression;
		FString ExpressionClassPath;
		FString Path;
		FString CatalogName;
		FString MaterialFunctionPath;
		FGuid NewExpressionGuid;
		FString SuggestedName;
		int32 NodePosX = 0;
		int32 NodePosY = 0;
		TArray<TSharedPtr<FJsonValue>> Properties;
		FString FromOutputName;
		FString ToInputName;
		FString MaterialProperty;
		bool bCompile = true;
		bool bSave = false;
	};

	struct UEAGENTINTERFACE_API FGraphMutationResult
	{
		FString Status;
		FString Phase;
		UE::UAI::Contract::FDiagnostic Diagnostic;
		FString ResolvedOperation;
		FString AssetPath;
		FString ExpressionGuid;
		FString ExpressionName;
		FString FromExpressionGuid;
		FString ToExpressionGuid;
		bool bRollbackAttempted = false;
		bool bRollbackSucceeded = false;

		bool IsApplied() const { return Status == TEXT("applied"); }
	};

	UEAGENTINTERFACE_API bool RegisterGraphMutationApplyContracts(UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API TSharedRef<UeAgentAuthoringOperationApply::IApplyProvider> MakeGraphMutationApplyProvider();
	UEAGENTINTERFACE_API bool ApplyGraphMutation(
		const FGraphMutationRequest& Request,
		FGraphMutationResult& OutResult,
		const FString& RequestId = FString());
}
