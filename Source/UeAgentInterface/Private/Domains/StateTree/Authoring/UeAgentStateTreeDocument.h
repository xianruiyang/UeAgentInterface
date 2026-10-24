#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"

class FJsonValue;
class UStateTree;
class UStruct;
class UPackage;
namespace UE::UAI::HjsonWorkspace { class FWorkspaceError; class FHjsonNode; }
namespace UE::UAI::Composition::HjsonImport { class FPreparedAuthoringImport; struct FSemanticWorkspaceChangeProjection; }

namespace UeAgentStateTreeAuthoring
{
	// Captures native defaults for an already resolved candidate; does not mutate the target.
	bool BuildTreeItemDefaults(UStateTree& Target, const FString& Name, const FString& Kind,
		const UStruct* Type, FString Placement, TSharedPtr<FJsonObject>& Item,
		TSharedPtr<FJsonObject>& Parameters, UE::UAI::HjsonWorkspace::FWorkspaceError& Error,
		const TSharedPtr<FJsonObject>& InitialCategories = nullptr,
		const UE::UAI::HjsonWorkspace::FHjsonNode* InitialCategorySource = nullptr);
	UEAGENTINTERFACE_API bool ValidateTreeDocument(const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& Error);
	// Resolve only unchanged context removed by native ownership/binding semantics.
	bool NormalizeTreeRemovalContext(const TSharedPtr<FJsonObject>& BeforeItems,
		const TSharedPtr<FJsonObject>& DesiredItems, UE::UAI::Contract::FDiagnostic& Error);
	// Called inside the existing Reflection transaction, which owns recovery and persistence.
	UEAGENTINTERFACE_API bool ApplyTreeDocument(UStateTree& Tree, const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& Error,
		UPackage* TextIdentityPackage = nullptr);
	// Initializes an already duplicated preview without discarding its native parameter bags.
	bool InitializeTreeAuthoringPreview(UStateTree& Source, UStateTree& Preview, UE::UAI::Contract::FDiagnostic& Error);
	// Forward authoring only: applies native semantic edit intents before
	// the common transaction captures its expected state. Exact replay remains separate.
	bool ApplyTreeDocumentForAuthoring(UStateTree& Tree, const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& Error,
		UPackage* TextIdentityPackage = nullptr);
	bool BuildTreeDocumentFromSemanticModel(UStateTree& Tree,
		const UE::UAI::HjsonWorkspace::FAuthoringModelRef& Model,
		const UE::UAI::Composition::HjsonImport::FPreparedAuthoringImport& Prepared,
		const UE::UAI::Composition::HjsonImport::FSemanticWorkspaceChangeProjection& Projection,
		TSharedPtr<FJsonObject>& OutDocument, UE::UAI::HjsonWorkspace::FWorkspaceError& Error);
}
