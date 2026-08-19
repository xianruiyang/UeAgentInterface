// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "NiagaraTypes.h"

class UEdGraphPin;
class UNiagaraNode;
class UNiagaraNodeAssignment;
class UNiagaraNodeOutput;

namespace UeAgentNiagaraStageModuleAuthoring
{
	struct FResolvedStage;
}

namespace UeAgentNiagaraStandaloneModuleAuthoring
{
	UEAGENTINTERFACE_API bool IsStandaloneModuleStage(
		const UeAgentNiagaraStageModuleAuthoring::FResolvedStage& Stage);

	UEAGENTINTERFACE_API void GetAssignmentNodes(
		UNiagaraNodeOutput& OutputNode,
		TArray<UNiagaraNode*>& OutNodes);

	UEAGENTINTERFACE_API bool ResolveAssignmentNode(
		UNiagaraNodeOutput& OutputNode,
		const FGuid& NodeGuid,
		UNiagaraNode*& OutNode,
		int32& OutIndex,
		FString& OutError);

	UEAGENTINTERFACE_API bool ResolveLegacyAssignmentNodeByTargets(
		UNiagaraNodeOutput& OutputNode,
		const TArray<FNiagaraVariable>& RequestedTargets,
		UNiagaraNodeAssignment*& OutNode,
		int32& OutIndex,
		FString& OutError);

	UEAGENTINTERFACE_API UNiagaraNode* AddAssignmentNode(
		UNiagaraNodeOutput& OutputNode,
		const TArray<FNiagaraVariable>& AssignmentTargets,
		int32 TargetIndex,
		FString& OutError);

	UEAGENTINTERFACE_API bool MoveAssignmentNode(
		UNiagaraNodeOutput& OutputNode,
		UNiagaraNode& Node,
		int32 TargetIndex,
		int32& OutNewIndex,
		FString& OutError);

	UEAGENTINTERFACE_API bool RemoveAssignmentNode(
		UNiagaraNodeOutput& OutputNode,
		UNiagaraNode& Node,
		FString& OutError);

	UEAGENTINTERFACE_API void GetAssignmentTargets(
		const UNiagaraNode& Node,
		TArray<FNiagaraVariable>& OutTargets);

	UEAGENTINTERFACE_API bool EnsureAssignmentTargetsRegistered(
		UNiagaraNode& Node,
		FString& OutError);

	UEAGENTINTERFACE_API bool AssignmentTargetsMatch(
		const UNiagaraNode& Node,
		const TArray<FNiagaraVariable>& RequestedTargets);

	UEAGENTINTERFACE_API UEdGraphPin* FindAssignmentTargetPin(
		UNiagaraNode& Node,
		const FString& InputName,
		FNiagaraVariable& OutVariable);
}
