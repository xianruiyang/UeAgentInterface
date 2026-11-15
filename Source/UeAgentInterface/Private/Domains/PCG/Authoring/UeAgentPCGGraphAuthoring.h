// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"

class FJsonObject;
class UPCGGraph;
class UPCGNode;

namespace UE::UAI::HjsonWorkspace { class FResolvedSemanticWorkspace; struct FGraphAuthoringConnectionContext; }

namespace UeAgentPCGAuthoring
{
	struct FGraphPropertyIntent;
	struct UEAGENTINTERFACE_API FGraphApplyResult
	{
		int32 NodeCount = 0;
		int32 EdgeCount = 0;
		int32 NodePropertiesApplied = 0;
		TSharedPtr<const FGraphPropertyIntent> PropertyIntent;
	};
	UEAGENTINTERFACE_API bool VerifyGraphPropertyIntent(
		UPCGGraph& Graph, const FGraphApplyResult& Result, UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API FString MakeEdgeKey(
		const FString& FromNode,
		const FString& FromPin,
		const FString& ToNode,
		const FString& ToPin);
	TSharedRef<FJsonObject> BuildNodeState(UPCGGraph& Graph, UPCGNode& Node);
	UEAGENTINTERFACE_API bool BuildGraphDocument(
		UPCGGraph& Graph,
		TSharedPtr<FJsonObject>& OutDocument,
		UE::UAI::Contract::FDiagnostic& OutError,
		const FString& LogicalTarget = FString());
	UEAGENTINTERFACE_API bool BuildBackendGraphDocument(
		const TSharedPtr<const FJsonObject>& CanonicalDocument,
		TSharedPtr<FJsonObject>& OutBackendDocument,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool ApplyGraphDocument(
		UPCGGraph& Graph,
		const TSharedPtr<const FJsonObject>& CanonicalDocument,
		bool bReplaceNodes,
		FGraphApplyResult& OutResult,
		UE::UAI::Contract::FDiagnostic& OutError,
		bool bPreserveExistingNodes = false,
		const UE::UAI::HjsonWorkspace::FGraphAuthoringConnectionContext* Connections = nullptr,
		const FString& LogicalTarget = FString());
	UEAGENTINTERFACE_API bool PreflightGraphDocument(
		UPCGGraph* ExistingGraph,
		const TSharedPtr<const FJsonObject>& CanonicalDocument,
		bool bReplaceNodes,
		FGraphApplyResult& OutResult,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryBuildNativeGraphDocumentFromSemanticModel(
		const UE::UAI::HjsonWorkspace::FAuthoringModelRef& Model,
		TSharedPtr<FJsonObject>& OutDocument,
		UE::UAI::Contract::FDiagnostic& OutError,
		const UE::UAI::HjsonWorkspace::FResolvedSemanticWorkspace* Workspace = nullptr,
		UE::UAI::HjsonWorkspace::FGraphAuthoringConnectionContext* OutConnections = nullptr);
}
