#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FJsonValue;
class FJsonObject;
class UStruct;
class USmartObjectDefinition;
class UPackage;
class UObject;
struct FWorldConditionQueryDefinition;
namespace UE::UAI::HjsonWorkspace { class FHjsonNode; }

namespace UeAgentSmartObjectAuthoring
{
	// Root, slot and new-item defaults share the native condition compilation boundary.
	bool InitializeConditionQuery(FWorldConditionQueryDefinition& Query, const UObject& Owner,
		const FString& PropertyPath, UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool BuildDefinitionItemDefaults(USmartObjectDefinition& Context,
		const FString& Label, const FString& Kind, UStruct* Type, TSharedPtr<FJsonObject>& OutItem,
		UE::UAI::Contract::FDiagnostic& OutError, const TSharedPtr<FJsonObject>& InitialCategories = nullptr,
		const UE::UAI::HjsonWorkspace::FHjsonNode* InitialCategorySource = nullptr);
	UEAGENTINTERFACE_API bool ValidateDefinitionDocument(const TSharedPtr<FJsonValue>& Document, UE::UAI::Contract::FDiagnostic& OutError);
	// Reconcile native slot containment and retained unresolved links against the original
	// document. Explicit edits to a removed owner remain an error rather than being lost.
	UEAGENTINTERFACE_API bool ResolveDefinitionRemovalContext(const TSharedPtr<FJsonObject>& BeforeItems,
		const TSharedPtr<FJsonObject>& DesiredItems, UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool ValidateDefinitionBindingsForAuthoring(USmartObjectDefinition& Definition,
		const TSharedPtr<FJsonValue>& Document, UE::UAI::Contract::FDiagnostic& OutError);
	// Exact reconstruction. Reflection owns checkpoints, persistence and failure recovery.
	UEAGENTINTERFACE_API bool ApplyDefinitionDocument(USmartObjectDefinition& Definition,
		const TSharedPtr<FJsonValue>& Document, UE::UAI::Contract::FDiagnostic& OutError, UPackage* TextIdentityPackage = nullptr);
	// Forward authoring uses the same native property notifications for preview and target.
	UEAGENTINTERFACE_API bool ApplyDefinitionDocumentForAuthoring(USmartObjectDefinition& Definition,
		const TSharedPtr<FJsonValue>& Document, UE::UAI::Contract::FDiagnostic& OutError, UPackage* TextIdentityPackage = nullptr);
}
