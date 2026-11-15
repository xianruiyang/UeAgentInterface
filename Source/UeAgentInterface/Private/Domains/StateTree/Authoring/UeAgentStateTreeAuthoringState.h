#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

class UStateTreeEditorData;
class UStateTree;
class UPackage;

namespace UeAgentStateTreeAuthoring
{
	inline constexpr const TCHAR* TreeSemanticId = TEXT("uai.semantic.state_tree.tree");
	inline constexpr const TCHAR* ItemSemanticId = TEXT("uai.semantic.state_tree.item");
	inline constexpr const TCHAR* ItemPropertySemanticId = TEXT("uai.semantic.state_tree.item_property");

	UEAGENTINTERFACE_API bool RegisterTreeState(UE::UAI::Contract::FDiagnostic& OutError);
	// Persistent editor structure and native binding sources. Context/event descriptors
	// are captured without acquiring runtime values or preparing temporary event payloads.
	UEAGENTINTERFACE_API bool AppendStructuralReadRequests(const UStateTreeEditorData& EditorData,
		const FString& AssetPath, TArray<UeAgentAuthoringState::FReadRequest>& OutRequests, UE::UAI::Contract::FDiagnostic& OutError);
	// Frozen domain document built from the same structural/property capture as export.
	UEAGENTINTERFACE_API bool ReadTreeDocument(UStateTree& Tree, TSharedPtr<FJsonValue>& OutValue, UE::UAI::Contract::FDiagnostic& OutError);
	bool ReadTreeDocument(UStateTree& Tree, TSharedPtr<FJsonValue>& OutValue, UE::UAI::Contract::FDiagnostic& OutError,
		TOptional<UeAgentAuthoringState::FStateSnapshot>* OutStructure);
	// Applies only the document's property payloads to the already-resolved structure.
	// The caller owns the enclosing structural transaction and its recovery.
	// A transaction identity package resolves text sources into its private document
	// for reuse by actual application. Scratch-only callers leave it null.
	UEAGENTINTERFACE_API bool ApplyTreeDocumentProperties(UStateTree& Tree,
		const TSharedPtr<FJsonValue>& Document, UE::UAI::Contract::FDiagnostic& OutError, UPackage* TextIdentityPackage = nullptr);
}
