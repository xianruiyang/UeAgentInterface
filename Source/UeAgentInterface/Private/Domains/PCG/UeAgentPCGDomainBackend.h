// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

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

namespace UeAgentPCG
{
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

	bool SavePackagesForObjects(const TArray<UObject*>& Objects, FString& OutError);
	bool SaveAuthoringFile(
		const FString& OutputPathInput,
		const TSharedPtr<FJsonObject>& Root,
		FString& OutOutputPath,
		FString& OutError);
	bool PCGGraphHasMaterializedEditorGraph(const UPCGGraph* Graph);
	void ClosePCGEditorOpenedForGraphInit(
		UAssetEditorSubsystem* AssetEditorSubsystem,
		UPCGGraph* Graph,
		bool bOpenedEditorForGraphInit);
	bool EnsurePCGEditorGraphMaterialized(UPCGGraph* Graph, bool& bOutOpenedEditor);
	void AddIssue(
		TArray<TSharedPtr<FJsonValue>>& Issues,
		const FString& Severity,
		const FString& Code,
		const FString& Path,
		const FString& Message);
	int32 CountErrors(const TArray<TSharedPtr<FJsonValue>>& Issues);
	int32 ApplyPropertyArray(
		UObject* Object,
		const TArray<TSharedPtr<FJsonValue>>* PropertyValues,
		TArray<TSharedPtr<FJsonValue>>& Issues,
		const FString& PathPrefix);
	UClass* ResolvePCGSettingsClass(const FString& ClassPathInput);
	TSharedPtr<FJsonObject> SettingsClassToCatalogJson(UClass* Class);
	FString MakeStableNodeId(UPCGGraph* Graph, UPCGNode* Node, TSet<FString>& UsedIds);
	TMap<FString, UPCGNode*> BuildExportNodeMapById(UPCGGraph* Graph);
	TSharedPtr<FJsonObject> NodeToJson(UPCGGraph* Graph, UPCGNode* Node, const FString& NodeId);
	TSharedPtr<FJsonObject> ExportParameterBagToJson(const FInstancedPropertyBag* Bag);
	TSharedPtr<FJsonObject> ExportGraphToJson(UPCGGraph* Graph);
	int32 GetParameterBagPropertyCount(const FInstancedPropertyBag* Bag);
	int32 ApplyCanonicalParameterBag(
		FInstancedPropertyBag& Bag,
		const TSharedPtr<FJsonObject>& BagJson,
		TArray<TSharedPtr<FJsonValue>>& Issues,
		TArray<FName>* OutAppliedNames = nullptr,
		bool bAllowLayoutChanges = true);
	int32 PreflightCanonicalParameterBag(
		const FInstancedPropertyBag* SourceBag,
		const TSharedPtr<FJsonObject>& BagJson,
		TArray<TSharedPtr<FJsonValue>>& Issues,
		bool bAllowLayoutChanges);
	void RefreshGraphInstanceParameters(UPCGGraphInstance* Instance);
	void NotifyGraphInstanceParameterValuesChanged(
		UPCGGraphInstance* Instance,
		const TArray<FName>& ParameterNames);
	UPCGGraph* CreateOrLoadGraphAsset(
		const FString& AssetPathInput,
		bool bCreateIfMissing,
		bool& bOutCreated,
		FString& OutError);
	bool ApplyGraphJson(
		UPCGGraph* Graph,
		const TSharedPtr<FJsonObject>& GraphJson,
		bool bReplaceNodes,
		TArray<TSharedPtr<FJsonValue>>& Issues,
		int32& OutAppliedNodes,
		int32& OutAppliedEdges);
	bool PreflightApplyGraphJson(
		UPCGGraph* ExistingGraph,
		const TSharedPtr<FJsonObject>& GraphJson,
		bool bReplaceNodes,
		TArray<TSharedPtr<FJsonValue>>& Issues,
		int32& OutAppliedNodes,
		int32& OutAppliedEdges);
	AActor* FindActorByNameOrLabelDirect(UWorld* World, const FString& NameOrLabel);
	bool GetStringAny(
		const TSharedPtr<FJsonObject>& Params,
		const TArray<const TCHAR*>& Keys,
		FString& OutValue);
	UPCGComponent* FindPCGComponent(
		UWorld* World,
		const TSharedPtr<FJsonObject>& Params,
		FString& OutError);
	bool TryParseGenerationTrigger(
		const FString& TriggerText,
		EPCGComponentGenerationTrigger& OutTrigger);
	TSharedPtr<FJsonObject> ComponentToJson(UPCGComponent* Component);
	TSharedPtr<FJsonObject> PCGDataCollectionToJson(
		const FPCGDataCollection& Collection,
		int32 MaxPointsPerData);
	void SetActionStatus(TSharedPtr<FJsonObject>& OutData, UPCGComponent* Component);
	void BuildAuthoringGraphAndLayout(
		const TSharedPtr<FJsonObject>& SourceGraph,
		TSharedPtr<FJsonObject>& OutGraph,
		TSharedPtr<FJsonObject>& OutLayout);
}
