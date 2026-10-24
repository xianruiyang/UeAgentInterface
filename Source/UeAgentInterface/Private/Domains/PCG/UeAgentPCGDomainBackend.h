// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class AActor;
class FJsonObject;
class FJsonValue;
class UAssetEditorSubsystem;
class UClass;
class UObject;
class UPCGComponent;
class UPCGGraph;
class UPCGGraphInstance;
class UPCGNode;
class UPCGSettings;
class UWorld;
struct FInstancedPropertyBag;
struct FPCGDataCollection;
enum class EPCGComponentGenerationTrigger : uint8;
namespace UE::UAI::HjsonWorkspace { struct FGraphAuthoringConnectionContext; }

namespace UeAgentPCG
{
	enum class EGraphNodeCreationOverrideDisposition : uint8
	{
		Unhandled,
		Created,
		Failed,
	};

	struct FGraphNodeCreationOverrideResult
	{
		EGraphNodeCreationOverrideDisposition Disposition =
			EGraphNodeCreationOverrideDisposition::Unhandled;
		UPCGNode* Node = nullptr;
		UE::UAI::Contract::FDiagnostic Diagnostic;
	};

	using FGraphNodeCreationOverride = TFunction<
		FGraphNodeCreationOverrideResult(
			UPCGGraph& Graph,
			const TSharedPtr<FJsonObject>& NodeJson)>;
	using FGraphNodePropertiesApply = TFunction<bool(const TMap<FString, UPCGNode*>&)>;

	inline constexpr const TCHAR* SchemaCapabilities = TEXT("ue_agent_interface.pcg.capabilities.v1");
	inline constexpr const TCHAR* SchemaCatalog = TEXT("ue_agent_interface.pcg.node_catalog.v1");
	inline constexpr const TCHAR* SchemaFolder = TEXT("ue_agent_interface.pcg.graph_folder.v1");
	inline constexpr const TCHAR* SchemaComponent = TEXT("ue_agent_interface.pcg.component.v1");
	inline constexpr const TCHAR* SchemaData = TEXT("ue_agent_interface.pcg.data.v1");
	inline constexpr const TCHAR* InputNodeId = TEXT("__input__");
	inline constexpr const TCHAR* OutputNodeId = TEXT("__output__");

	FString NormalizeAssetPath(const FString& InPath);
	FString ResolveFolderPath(const FString& InPath);
	UObject* LoadAssetObject(const FString& InPath);

	template<typename TObjectType>
	TObjectType* LoadAsset(const FString& InPath)
	{
		return Cast<TObjectType>(LoadAssetObject(InPath));
	}

	bool SavePackagesForObjects(const TArray<UObject*>& Objects, UE::UAI::Contract::FDiagnostic& OutError);
	bool SaveAuthoringFile(
		const FString& OutputPathInput,
		const TSharedPtr<FJsonObject>& Root,
		FString& OutOutputPath,
		UE::UAI::Contract::FDiagnostic& OutError);
	int32 CountErrors(const TArray<UE::UAI::Contract::FDiagnostic>& Issues);
	int32 ApplyPropertyArray(
		UObject* Object,
		const TArray<TSharedPtr<FJsonValue>>* PropertyValues,
		TArray<UE::UAI::Contract::FDiagnostic>& Issues,
		const FString& PathPrefix);
	UClass* ResolvePCGSettingsClass(const FString& ClassPathInput);
	TSharedPtr<FJsonObject> SettingsClassToCatalogJson(UClass* Class);
	FString MakeStableNodeId(UPCGGraph* Graph, UPCGNode* Node, TSet<FString>& UsedIds);
	UEAGENTINTERFACE_API TMap<FString, UPCGNode*> BuildExportNodeMapById(UPCGGraph* Graph);
	UEAGENTINTERFACE_API TSharedPtr<FJsonObject> NodeToJson(
		UPCGGraph* Graph,
		UPCGNode* Node,
		const FString& NodeId);
	TSharedPtr<FJsonObject> ExportParameterBagToJson(const FInstancedPropertyBag* Bag);
	int32 GetParameterBagPropertyCount(const FInstancedPropertyBag* Bag);
	int32 ApplyCanonicalParameterBag(
		FInstancedPropertyBag& Bag,
		const TSharedPtr<FJsonObject>& BagJson,
		TArray<UE::UAI::Contract::FDiagnostic>& Issues,
		TArray<FName>* OutAppliedNames = nullptr,
		bool bAllowLayoutChanges = true);
	int32 PreflightCanonicalParameterBag(
		const FInstancedPropertyBag* SourceBag,
		const TSharedPtr<FJsonObject>& BagJson,
		TArray<UE::UAI::Contract::FDiagnostic>& Issues,
		bool bAllowLayoutChanges);
	void RefreshGraphInstanceParameters(UPCGGraphInstance* Instance);
	void NotifyGraphInstanceParameterValuesChanged(
		UPCGGraphInstance* Instance,
		const TArray<FName>& ParameterNames);
	UPCGGraph* CreateOrLoadGraphAsset(
		const FString& AssetPathInput,
		bool bCreateIfMissing,
		bool& bOutCreated,
		UE::UAI::Contract::FDiagnostic& OutError);
	bool ApplyGraphJson(
		UPCGGraph* Graph,
		const TSharedPtr<FJsonObject>& GraphJson,
		bool bReplaceNodes,
		TArray<UE::UAI::Contract::FDiagnostic>& Issues,
		int32& OutAppliedNodes,
		int32& OutAppliedEdges,
		const FGraphNodeCreationOverride* NodeCreationOverride = nullptr,
		bool bPreserveExistingNodes = false,
		TMap<FString, UPCGNode*>* OutAppliedNodeBindings = nullptr,
		const FGraphNodePropertiesApply* ApplyNodeProperties = nullptr,
		const UE::UAI::HjsonWorkspace::FGraphAuthoringConnectionContext* Connections = nullptr,
		const FString& LogicalTarget = FString());
	bool PreflightApplyGraphJson(
		UPCGGraph* ExistingGraph,
		const TSharedPtr<FJsonObject>& GraphJson,
		bool bReplaceNodes,
		TArray<UE::UAI::Contract::FDiagnostic>& Issues,
		int32& OutAppliedNodes,
		int32& OutAppliedEdges,
		const FGraphNodeCreationOverride* NodeCreationOverride = nullptr);
	AActor* FindActorByNameOrLabelDirect(UWorld* World, const FString& NameOrLabel);
	bool GetStringAny(
		const TSharedPtr<FJsonObject>& Params,
		const TArray<const TCHAR*>& Keys,
		FString& OutValue);
	UPCGComponent* FindPCGComponent(
		UWorld* World,
		const TSharedPtr<FJsonObject>& Params,
		UE::UAI::Contract::FDiagnostic& OutError);
	bool TryParseGenerationTrigger(
		const FString& TriggerText,
		EPCGComponentGenerationTrigger& OutTrigger);
	TSharedPtr<FJsonObject> ComponentToJson(UPCGComponent* Component);
	TSharedPtr<FJsonObject> ComponentToJson(UPCGComponent* Component, bool bIncludeProperties);
	TSharedPtr<FJsonObject> PCGDataCollectionToJson(
		const FPCGDataCollection& Collection,
		int32 MaxPointsPerData);
	void SetActionStatus(TSharedPtr<FJsonObject>& OutData, UPCGComponent* Component);
	void BuildAuthoringGraphAndLayout(
		const TSharedPtr<FJsonObject>& SourceGraph,
		TSharedPtr<FJsonObject>& OutGraph,
		TSharedPtr<FJsonObject>& OutLayout);
}
