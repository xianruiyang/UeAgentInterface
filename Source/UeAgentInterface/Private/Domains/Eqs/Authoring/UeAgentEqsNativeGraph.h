#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class UEnvQuery;
class UEdGraphNode;
class FJsonObject;

namespace UeAgentEqsAuthoring
{
	struct FNodeCapture
	{
		FString Id;
		FString ParentId;
		int32 Order = 0;
		const UEdGraphNode* NativeNode = nullptr;
	};

	struct FConnectionCapture
	{
		FString Id;
		FString SourceId;
		FString TargetId;
		int32 SourcePin = INDEX_NONE;
		int32 TargetPin = INDEX_NONE;
	};

	struct FGraphCapture
	{
		TArray<FNodeCapture> Nodes;
		TArray<FConnectionCapture> Connections;
	};

	// Capture persisted editor nodes, including disabled tests, without refreshing the asset.
	bool CaptureGraph(const UEnvQuery& Query, FGraphCapture& OutCapture, UE::UAI::Contract::FDiagnostic& OutError);
	bool CaptureNodeState(const UEdGraphNode& Node, TSharedPtr<FJsonObject>& OutState, UE::UAI::Contract::FDiagnostic& OutError);
	const UObject* GetNodePropertyObject(const UEdGraphNode& Node);
}
