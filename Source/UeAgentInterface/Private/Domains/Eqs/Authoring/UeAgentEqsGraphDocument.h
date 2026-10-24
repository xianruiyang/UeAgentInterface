#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"

class UEnvQuery;
class UAIGraphNode;
class FJsonObject;
class FJsonValue;
namespace UE::UAI::HjsonWorkspace { class FWorkspaceError; struct FGraphNodeReferenceBinding; }
namespace UE::UAI::Composition::HjsonImport { class FPreparedAuthoringImport; struct FSemanticWorkspaceChangeProjection; }
namespace UeAgentDetailsAuthoringService { struct FScopedPropertyIntent; }

namespace UeAgentEqsAuthoring
{
	bool TryResolveCapturedNodeReference(UEnvQuery& Query, const FString& Reference,
		UAIGraphNode*& OutNode, UE::UAI::HjsonWorkspace::FWorkspaceError& OutError);
	bool ValidateGraphDocument(const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool ReadGraphDocument(UEnvQuery& Query, TSharedPtr<FJsonValue>& OutValue, UE::UAI::Contract::FDiagnostic& OutError);
	bool ApplyGraphDocument(UEnvQuery& Query, const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& OutError);
	bool ApplyGraphDocument(UEnvQuery& Query, const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& OutError,
		TArray<UeAgentDetailsAuthoringService::FScopedPropertyIntent>* OutPropertyIntents);
	bool AppendDefaultGraphNodes(const TSharedPtr<FJsonObject>& Nodes, UE::UAI::Contract::FDiagnostic& OutError);
	// Project the specialized option-create/layout contract into the complete graph state.
	bool BuildGraphDocumentFromOptionChanges(UEnvQuery& Query,
		const TMap<FString, UClass*>& NewOptions, const TMap<FString, int32>& Positions,
		TSharedPtr<FJsonObject>& OutDocument, UE::UAI::Contract::FDiagnostic& OutError);
	bool BuildGraphDocumentFromSemanticModel(UEnvQuery& Query,
		const UE::UAI::HjsonWorkspace::FAuthoringModelRef& Model,
		const UE::UAI::Composition::HjsonImport::FPreparedAuthoringImport& Prepared,
		const UE::UAI::Composition::HjsonImport::FSemanticWorkspaceChangeProjection& Projection,
		TSharedPtr<FJsonObject>& OutDocument, UE::UAI::HjsonWorkspace::FWorkspaceError& OutError,
		TMap<FString, UE::UAI::Contract::FSourceLocation>* OutSourceLocations = nullptr,
		TArray<UE::UAI::HjsonWorkspace::FGraphNodeReferenceBinding>* OutReferenceBindings = nullptr);
}
