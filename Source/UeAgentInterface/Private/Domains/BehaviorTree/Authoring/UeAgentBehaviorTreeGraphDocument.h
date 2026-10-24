#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"

class UBehaviorTree;
class UBehaviorTreeGraphNode;
class UEdGraph;
class FJsonObject;
class FJsonValue;
namespace UE::UAI::HjsonWorkspace { class FWorkspaceError; struct FGraphNodeReferenceBinding; }
namespace UE::UAI::Composition::HjsonImport { class FPreparedAuthoringImport; struct FSemanticWorkspaceChangeProjection; }
namespace UeAgentDetailsAuthoringService { struct FScopedPropertyIntent; }

	namespace UeAgentBehaviorTreeAuthoring
{
	struct FMutation;
	bool TryResolveCapturedNodeReference(UBehaviorTree& Tree, const FString& Reference,
		UBehaviorTreeGraphNode*& OutNode, UE::UAI::HjsonWorkspace::FWorkspaceError& OutError);
	bool TryResolveCapturedGraphOwnerReference(UBehaviorTree& Tree, const FString& Reference,
		UEdGraph*& OutGraph, UE::UAI::HjsonWorkspace::FWorkspaceError& OutError);
	bool BuildDefaultOwnedGraphDocument(const FString& OwnerId, TSharedPtr<FJsonObject>& OutDocument, UE::UAI::Contract::FDiagnostic& OutError);
	// Server-side desired state. Workspace identity and aliases are resolved by the common coordinator.
	bool ValidateGraphDocument(const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool ReadGraphDocument(UBehaviorTree& Tree, TSharedPtr<FJsonValue>& OutValue, UE::UAI::Contract::FDiagnostic& OutError);
	bool ApplyGraphDocument(UBehaviorTree& Tree, const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& OutError);
	bool ApplyGraphDocument(UBehaviorTree& Tree, const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& OutError,
		TArray<UeAgentDetailsAuthoringService::FScopedPropertyIntent>* OutPropertyIntents);
	// Adapt prepared native additions to the same desired state used by ordinary workspaces.
	bool BuildGraphDocumentFromMutations(UBehaviorTree& Tree, const TArray<FMutation>& Mutations,
		TSharedPtr<FJsonObject>& OutDocument, UE::UAI::Contract::FDiagnostic& OutError);
	bool BuildGraphDocumentFromSemanticModel(UBehaviorTree& Tree,
		const UE::UAI::HjsonWorkspace::FAuthoringModelRef& Model,
		const UE::UAI::Composition::HjsonImport::FPreparedAuthoringImport& Prepared,
		const UE::UAI::Composition::HjsonImport::FSemanticWorkspaceChangeProjection& Projection,
		TSharedPtr<FJsonObject>& OutDocument, UE::UAI::HjsonWorkspace::FWorkspaceError& OutError,
		TMap<FString, UE::UAI::Contract::FSourceLocation>* OutSourceLocations = nullptr,
		TArray<UE::UAI::HjsonWorkspace::FGraphNodeReferenceBinding>* OutReferenceBindings = nullptr);
}
