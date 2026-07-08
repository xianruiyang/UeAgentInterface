#pragma once

#include "Animation/AnimBlueprint.h"
#include "CoreMinimal.h"
#include "Kismet2/CompilerResultsLog.h"

class UAnimBlueprint;
class UAnimGraphNode_StateMachineBase;
class UAnimStateNodeBase;
class UAnimStateTransitionNode;
class UAnimationStateMachineGraph;
class UEdGraph;
class UEdGraphNode;

namespace UeAgentAnimBlueprintOps
{
	enum class ECompilerMessageSeverityFilter : uint8
	{
		All,
		Error,
		Warning,
		Info,
		WarningOrError
	};

	FString NormalizeAssetPath(const FString& InPath);
	UAnimBlueprint* LoadAnimBlueprint(const FString& InPath);
	FString BlueprintStatusToString(EBlueprintStatus InStatus);
	void BuildCompilerLogJson(
		const FCompilerResultsLog& InCompilerLog,
		ECompilerMessageSeverityFilter InFilter,
		int32 InMaxMessages,
		TArray<TSharedPtr<FJsonValue>>& OutMessages,
		int32& OutFilteredErrorCount,
		int32& OutFilteredWarningCount,
		int32& OutFilteredInfoCount);
	bool SaveAnimBlueprintPackage(UAnimBlueprint* AnimBlueprint, FString& OutError);
	FString PreviewApplicationMethodToString(EPreviewAnimationBlueprintApplicationMethod InMethod);
	TArray<UEdGraph*> GatherAnimGraphs(UAnimBlueprint* AnimBlueprint);
	TArray<TSharedPtr<FJsonValue>> MakeStringArray(const TArray<FString>& InValues);
	bool ParseNodeGuid(const FString& InNodeGuid, FGuid& OutGuid);
	bool IsLayerInterfaceBlueprint(const UAnimBlueprint* AnimBlueprint);
	bool IsAnimLayerGraph(const UEdGraph* Graph);
	UAnimGraphNode_StateMachineBase* FindStateMachineNode(UAnimBlueprint* AnimBlueprint, const FString& StateMachineName);
	UAnimStateNodeBase* FindStateNodeByName(UAnimationStateMachineGraph* StateMachineGraph, const FString& StateName);
	UAnimStateNodeBase* FindStateNodeByGuid(UAnimationStateMachineGraph* StateMachineGraph, const FGuid& NodeGuid);
	UAnimStateTransitionNode* FindTransitionNode(UAnimationStateMachineGraph* StateMachineGraph, const FString& SourceStateName, const FString& TargetStateName);
	UAnimStateTransitionNode* FindTransitionNodeByGuid(UAnimationStateMachineGraph* StateMachineGraph, const FGuid& NodeGuid);
}
