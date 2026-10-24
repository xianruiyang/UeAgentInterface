#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "EdGraph/EdGraphPin.h"
#include "SubobjectDataHandle.h"

class FJsonObject;
class FJsonValue;
class AActor;
class UActorComponent;
class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
class UEdGraphSchema;
class USimpleConstructionScript;
class USCS_Node;
class USceneComponent;

namespace UeAgentAuthoringTypeDescriptors
{
	struct FDescriptor;
}

namespace UeAgentBlueprintAuthoringOps
{
	using UE::UAI::Contract::FDiagnostic;
	UObject* LoadAssetObject(const FString& InPath);
	UBlueprint* LoadBlueprintAsset(const FString& InPath);
	UClass* ResolveClassByPath(const FString& InClassPath);
	UEdGraph* ResolveBlueprintGraph(UBlueprint* Blueprint, const FString& InGraphName);
	bool SaveBlueprintPackage(UBlueprint* Blueprint, FDiagnostic& OutError);
	USCS_Node* FindSCSNodeByAnyName(USimpleConstructionScript* SCS, const FString& InComponentName);
	struct FInheritedComponent
	{
		UActorComponent* Component = nullptr;
		USCS_Node* SourceNode = nullptr;
		FString Name;
		FString ParentName;
		TArray<FString> Aliases;
	};
	// Reads current overrides without creating templates or changing the Blueprint.
	void GatherInheritedComponents(UBlueprint& Blueprint, TArray<FInheritedComponent>& OutComponents);
	UActorComponent* ResolveInheritedComponent(UBlueprint& Blueprint, const FString& InComponentName);
	UActorComponent* ResolveEditableInheritedComponent(UBlueprint& Blueprint,
		const FString& InComponentName, FDiagnostic& OutError);
	bool TryChangeInheritedComponentClass(UBlueprint& Blueprint, const FString& Name,
		UClass& NewClass, FDiagnostic& OutError);
	bool TryRemoveBlueprintComponentNode(UBlueprint& Blueprint, USCS_Node& Node, FDiagnostic& OutError);
	void CollectComponentRemovalDependents(UBlueprint& Blueprint, const USCS_Node& Node,
		TArray<UBlueprint*>& OutBlueprints, TArray<AActor*>& OutActors);
	bool TryRenameBlueprintComponentNode(UBlueprint& Blueprint, USCS_Node& Node,
		const FString& RequestedName, FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool TryCreateBlueprintComponentNode(
		UBlueprint& Blueprint,
		USCS_Node* Parent,
		UClass& ComponentClass,
		const FString& RequestedName,
		UObject* AssetOverride,
		const TFunction<void(const FSubobjectDataHandle&)>& OnCreated,
		USCS_Node*& OutNode,
		FDiagnostic& OutError,
		USceneComponent* InheritedParent = nullptr);
	bool TrySetBlueprintComponentParent(UBlueprint& Blueprint, USCS_Node& Node,
		USCS_Node* Parent, USceneComponent* InheritedParent, FDiagnostic& OutError);
	UEdGraphPin* FindNodePinByName(UEdGraphNode* Node, const FString& InPinName);
	/** Connect through the native schema and complete the editor's node notification lifecycle. */
	UEAGENTINTERFACE_API bool TryCreateGraphConnection(
		const UEdGraphSchema& Schema, UEdGraphPin* From, UEdGraphPin* To, FDiagnostic& OutError);
	bool IsUserConstructionScriptGraph(const UEdGraph* Graph);
	bool IsUnsafeConstructionScriptPcgCall(const FString& FunctionOwnerClass, const FString& FunctionName);
	bool RequiresExplicitParentOrEventNode(const FString& FunctionName);
	FString PinContainerTypeToString(EPinContainerType InContainerType);
	UEAGENTINTERFACE_API TSharedRef<FJsonObject> BuildPinTypeJsonObject(const FEdGraphPinType& PinType,
		const UBlueprint* Blueprint = nullptr, const FString& CanonicalAssetPath = FString());
	UEAGENTINTERFACE_API bool BuildPinTypeFromJsonObject(const TSharedPtr<FJsonObject>& TypeObj, FEdGraphPinType& OutPinType,
		FDiagnostic& OutError, UBlueprint* Blueprint = nullptr);
	UEAGENTINTERFACE_API bool TryDescribePinDefault(
		const FEdGraphPinType& PinType,
		const FString& DefaultValue,
		UeAgentAuthoringTypeDescriptors::FDescriptor& OutType,
		TSharedPtr<FJsonValue>& OutValue,
		FDiagnostic& OutError,
		TSharedPtr<FJsonObject>* OutValueDefinition = nullptr);
	bool TryEncodeStructPinDefaultText(
		const FString& StructPath,
		const TSharedPtr<FJsonValue>& Value,
		FString& OutText,
		FDiagnostic& OutError);
}
