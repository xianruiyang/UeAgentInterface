#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class UBehaviorTree;
class UBlackboardData;
class UBehaviorTreeGraph;
class UBehaviorTreeGraphNode;
class UClass;
class UEdGraph;
class UEdGraphNode;
class FJsonObject;

namespace UeAgentBehaviorTreeAuthoring
{
	struct FNodeCapture
	{
		FString Id;
		FString ParentId;
		FString PayloadClass;
		int32 Order = 0;
		TSharedPtr<FJsonObject> State;
		const UEdGraphNode* NativeNode = nullptr;
	};

	struct FConnectionCapture
	{
		FString Id;
		FString SourceId;
		FString TargetId;
		TSharedPtr<FJsonObject> State;
	};

	struct FGraphContents
	{
		TArray<FNodeCapture> Nodes;
		TArray<FConnectionCapture> Connections;
	};

	struct FOwnedGraphCapture : FGraphContents
	{
		FString Id;
		FString OwnerId;
		const UEdGraph* NativeGraph = nullptr;
	};

	struct FGraphCapture : FGraphContents
	{
		TArray<FOwnedGraphCapture> OwnedGraphs;
	};

	// Read the native graph without creating, reconstructing or compiling it.
	UEAGENTINTERFACE_API bool CaptureGraph(const UBehaviorTree& Tree, FGraphCapture& OutCapture, UE::UAI::Contract::FDiagnostic& OutError);
	bool CaptureGraph(const UBehaviorTree& Tree, FGraphCapture& OutCapture, UE::UAI::Contract::FDiagnostic& OutError,
		bool bDeferExportedProperties);
	bool CaptureGraphContents(const UEdGraph& Graph, FGraphContents& OutCapture, UE::UAI::Contract::FDiagnostic& OutError);
	bool CaptureNode(const UEdGraphNode& Node, FNodeCapture& OutCapture, UE::UAI::Contract::FDiagnostic& OutError);
	const UObject* GetNodePropertyObject(const UEdGraphNode& Node);
	const UObject* GetExportedNodePropertyObject(const UEdGraphNode& Node);
	// One authored binding, with the native Root editor projection kept in sync.
	void BindBlackboard(UBehaviorTree& Tree, UBlackboardData* Blackboard);

	struct FNodeCreateRequest
	{
		UClass* GraphNodeClass = nullptr;
		UClass* PayloadClass = nullptr;
		FString CandidateName;
		FString CandidateId;
		TMap<FString, FString> CandidateSelect;
		FVector2f Location = FVector2f::ZeroVector;
		FGuid NodeGuid;
		bool bInjected = false;
	};

	// Native creation only. The calling authoring transaction owns persistence and recovery.
	bool CreateNode(UBehaviorTree& Tree, UBehaviorTreeGraph& Graph,
		const FNodeCreateRequest& Request, UBehaviorTreeGraphNode*& OutNode,
		UE::UAI::Contract::FDiagnostic& OutError);

	enum class EMutationKind : uint8 { Root, Node, Attachment, BlackboardBinding, Connection };
	struct FMutation
	{
		EMutationKind Kind = EMutationKind::Node;
		FString Identity;
		FNodeCreateRequest Node;
		FString ParentId;
		FString ObjectRef;
		FString FromId;
		FString ToId;
		int32 FromIndex = 0;
		int32 ToIndex = 0;
	};

	// Executes prepared native edits and settles the graph. No persistence or recovery here.
	bool ApplyMutations(UBehaviorTree& Tree, const TArray<FMutation>& Mutations,
		FString& OutRevision, UE::UAI::Contract::FDiagnostic& OutError, bool bSettle = true);
	bool SettleGraph(UBehaviorTree& Tree, FString& OutRevision, UE::UAI::Contract::FDiagnostic& OutError);
}
