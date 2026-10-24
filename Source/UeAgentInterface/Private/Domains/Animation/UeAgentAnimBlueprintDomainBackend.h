#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Animation/AnimBlueprint.h"
#include "CoreMinimal.h"
#include "EdGraph/EdGraph.h"
#include "Kismet2/CompilerResultsLog.h"

class IAnimClassInterface;
class FJsonObject;
class FJsonValue;
class UAnimBlueprint;
class UAnimGraphNode_StateMachineBase;
class UAnimStateAliasNode;
class UAnimStateNodeBase;
class UAnimStateTransitionNode;
class UAnimationStateMachineGraph;
class UEdGraph;
class UEdGraphNode;
class USkeletalMesh;
class USkeleton;

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
	bool AssetExists(const FString& InPath);
	UAnimBlueprint* LoadAnimBlueprint(const FString& InPath);
	USkeleton* LoadSkeleton(const FString& InPath);
	USkeletalMesh* LoadSkeletalMesh(const FString& InPath);
	UClass* ResolveClassByPath(const FString& InClassPath);
	FString BlueprintStatusToString(EBlueprintStatus InStatus);
	bool ParseCompilerSeverityFilter(const FString& InFilterText, ECompilerMessageSeverityFilter& OutFilter);
	bool SaveAnimBlueprintPackage(UAnimBlueprint* AnimBlueprint, UE::UAI::Contract::FDiagnostic& OutError);
	FString PreviewApplicationMethodToString(EPreviewAnimationBlueprintApplicationMethod InMethod);
	bool ParsePreviewApplicationMethod(const FString& InText, EPreviewAnimationBlueprintApplicationMethod& OutMethod);
	void AppendAnimBlueprintGraphItems(
		UAnimBlueprint* AnimBlueprint,
		TArray<TSharedPtr<FJsonValue>>& OutGraphs,
		int32& OutAnimGraphCount,
		int32& OutLogicGraphCount);
	const IAnimClassInterface* ResolveAnimClassInterface(const UAnimBlueprint* AnimBlueprint);
	TArray<UEdGraph*> GatherAnimGraphs(UAnimBlueprint* AnimBlueprint);
	FString GuidToString(const FGuid& InGuid);
	TArray<TSharedPtr<FJsonValue>> MakeStringArray(const TArray<FString>& InValues);
	FString NodeGuidToString(const UEdGraphNode* Node);
	bool ParseNodeGuid(const FString& InNodeGuid, FGuid& OutGuid);
	UClass* ResolveAnimLayerInterfaceClass(const FString& InClassPath);
	bool IsLayerInterfaceBlueprint(const UAnimBlueprint* AnimBlueprint);
	bool IsAnimLayerGraph(const UEdGraph* Graph);
	UEdGraph* ResolveAnyAnimBlueprintGraph(UAnimBlueprint* AnimBlueprint, const FString& InGraphName);
	bool FinalizeAnimBlueprintStructureMutation(UAnimBlueprint* AnimBlueprint, bool bCompile, bool bSave, UE::UAI::Contract::FDiagnostic& OutError);

	template<typename NodeType>
	NodeType* AddNodeToGraph(UEdGraph* Graph, const int32 PosX, const int32 PosY)
	{
		if (!Graph) return nullptr;
		NodeType* NewNode = NewObject<NodeType>(Graph);
		if (!NewNode) return nullptr;
		Graph->AddNode(NewNode, true, false);
		NewNode->CreateNewGuid();
		NewNode->PostPlacedNewNode();
		NewNode->AllocateDefaultPins();
		NewNode->NodePosX = PosX;
		NewNode->NodePosY = PosY;
		return NewNode;
	}

	FString StateNodeTypeToString(const UAnimStateNodeBase* StateNode);
	TArray<FString> CollectAliasTargetNames(const UAnimStateAliasNode* AliasNode);
	TSharedPtr<FJsonObject> BuildStateNodeJson(const UAnimStateNodeBase* StateNode);
	TSharedPtr<FJsonObject> BuildTransitionJson(const UAnimStateTransitionNode* TransitionNode);
	UAnimGraphNode_StateMachineBase* FindStateMachineNode(UAnimBlueprint* AnimBlueprint, const FString& StateMachineName);
	UAnimStateNodeBase* FindStateNodeByName(UAnimationStateMachineGraph* StateMachineGraph, const FString& StateName);
	UAnimStateNodeBase* FindStateNodeByGuid(UAnimationStateMachineGraph* StateMachineGraph, const FGuid& NodeGuid);
	UAnimStateTransitionNode* FindTransitionNode(UAnimationStateMachineGraph* StateMachineGraph, const FString& SourceStateName, const FString& TargetStateName);
	UAnimStateTransitionNode* FindTransitionNodeByGuid(UAnimationStateMachineGraph* StateMachineGraph, const FGuid& NodeGuid);
	UAnimStateNodeBase* GetEntryConnectedState(UAnimationStateMachineGraph* StateMachineGraph);
	bool ClearEntryState(UAnimationStateMachineGraph* StateMachineGraph, int32& OutBrokenLinkCount, UE::UAI::Contract::FDiagnostic& OutError);
	bool SetEntryState(UAnimationStateMachineGraph* StateMachineGraph, UAnimStateNodeBase* TargetStateNode, UE::UAI::Contract::FDiagnostic& OutError);
}
