#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

class USmartObjectDefinition;
class UPackage;

namespace UeAgentSmartObjectAuthoring
{
	inline constexpr const TCHAR* DefinitionSemanticId = TEXT("uai.semantic.smart_object.definition");
	inline constexpr const TCHAR* ItemSemanticId = TEXT("uai.semantic.smart_object.item");
	inline constexpr const TCHAR* ItemPropertySemanticId = TEXT("uai.semantic.smart_object.item_property");
	UEAGENTINTERFACE_API bool RegisterDefinitionState(UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool AppendStructuralReadRequests(const USmartObjectDefinition& Definition,
		const FString& AssetPath, TArray<UeAgentAuthoringState::FReadRequest>& OutRequests, UE::UAI::Contract::FDiagnostic& OutError);
	// Frozen native document; public authoring uses the composition projection instead.
	UEAGENTINTERFACE_API bool ReadDefinitionDocument(const USmartObjectDefinition& Definition,
		TSharedPtr<FJsonValue>& OutValue, UE::UAI::Contract::FDiagnostic& OutError);
	bool ReadDefinitionDocument(const USmartObjectDefinition& Definition, TSharedPtr<FJsonValue>& OutValue,
		UE::UAI::Contract::FDiagnostic& OutError, TOptional<UeAgentAuthoringState::FStateSnapshot>* OutStructure);
	// The caller must reconstruct structure before replaying captured properties.
	UEAGENTINTERFACE_API bool ApplyDefinitionDocumentProperties(USmartObjectDefinition& Definition,
		const TSharedPtr<FJsonValue>& Document, UE::UAI::Contract::FDiagnostic& OutError, UPackage* TextIdentityPackage = nullptr);
	UEAGENTINTERFACE_API bool ApplyDefinitionDocumentPropertiesForAuthoring(USmartObjectDefinition& Definition,
		const TSharedPtr<FJsonValue>& Document, UE::UAI::Contract::FDiagnostic& OutError, UPackage* TextIdentityPackage = nullptr);
	// Exact persistent binding storage replay; does not repair paths or apply parameters.
	UEAGENTINTERFACE_API bool ApplyDefinitionBindingState(USmartObjectDefinition& Definition,
		const TSharedPtr<FJsonValue>& Document, UE::UAI::Contract::FDiagnostic& OutError, bool bApplyBindableStructs = true);
	// Forward storage mutation via the native collection; caller settles the complete document.
	UEAGENTINTERFACE_API bool ApplyDefinitionBindingsForAuthoring(USmartObjectDefinition& Definition,
		const TSharedPtr<FJsonValue>& Document, UE::UAI::Contract::FDiagnostic& OutError);
}
