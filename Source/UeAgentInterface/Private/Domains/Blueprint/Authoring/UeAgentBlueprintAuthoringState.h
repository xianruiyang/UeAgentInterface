// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

class UClass;
class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UObject;
class FJsonObject;

namespace UeAgentBlueprintAuthoring
{
	using UE::UAI::Contract::FDiagnostic;
	inline constexpr const TCHAR* ObservedReparentHierarchy = TEXT("observed_reparent_hierarchy");
	inline constexpr const TCHAR* ObservedGraphGuidsContext = TEXT("observed_graph_guids");
	inline constexpr const TCHAR* ObservedGraphNamePrefix = TEXT("observed_graph_name.");
	inline constexpr const TCHAR* ObservedAbsentGraphPrefix = TEXT("observed_absent_graph.");
	inline constexpr const TCHAR* ObservedRootGraphPrefix = TEXT("observed_root_graph.");
	inline constexpr const TCHAR* ObservedMemberCapture = TEXT("observed_member_capture");
	inline constexpr const TCHAR* ObservedInterfacePrefix = TEXT("observed_interface.");
	inline constexpr const TCHAR* ObservedDefaultMemberPrefix = TEXT("observed_member_default.");
	inline constexpr const TCHAR* ObservedMemberPrefix = TEXT("observed_member.");
	inline constexpr const TCHAR* ObservedMissingMemberPrefix = TEXT("observed_absent_member.");
	inline constexpr const TCHAR* ObservedDelegatePrefix = TEXT("observed_delegate.");
	inline constexpr const TCHAR* ObservedMissingDelegatePrefix = TEXT("observed_absent_delegate.");
	inline constexpr const TCHAR* ObservedComponentPrefix = TEXT("observed_scs_component.");
	bool CaptureObservedGraphBodies(UBlueprint& Blueprint, const FString& AssetPath, const FString& GraphGuids,
		const FString& WorkspaceTargetKind, TArray<UeAgentAuthoringState::FReadRequest> Requests,
		UeAgentAuthoringState::FStateValue& OutValue, FDiagnostic& Error,
		const TMap<FString, FString>* GraphContext = nullptr);
	UEAGENTINTERFACE_API bool ProjectObservedNodePinDefaults(FJsonObject& DesiredNode, UeAgentAuthoringState::FStateEntry& NativeNode,
		const UeAgentAuthoringState::FStateSnapshot& FinalCapture, const TArray<UeAgentAuthoringState::FReadRequest>* Requests,
		FDiagnostic& Error);
	void GatherExportGraphs(const UBlueprint& Blueprint, TArray<UEdGraph*>& OutGraphs);
	FString ExportGraphKind(const UBlueprint& Blueprint, const UEdGraph* Graph);
	void AppendGraphDisplayState(const UBlueprint& Blueprint, const UEdGraph& Graph, FJsonObject& State,
		const FString& CanonicalAssetPath = FString());

	UEAGENTINTERFACE_API void AppendGraphNodeSemanticState(
		const UEdGraphNode& Node,
		FJsonObject& State);
	UEAGENTINTERFACE_API void AppendGraphNodeSemanticState(
		const UEdGraphNode& Node,
		FJsonObject& State,
		const FString& CanonicalAssetPath);
	UEAGENTINTERFACE_API UObject* CreateFactorySeed(
		const FString& AssetKind,
		UObject* Outer,
		FName Name,
		EObjectFlags Flags,
		FDiagnostic& OutError);
	UEAGENTINTERFACE_API UObject* CreateFactorySeed(
		const FString& AssetKind,
		UClass* ParentClass,
		UObject* Outer,
		FName Name,
		EObjectFlags Flags,
		FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool BuildAssetSnapshotReadRequest(
		UObject* Asset,
		const FString& CanonicalAssetPath,
		const FString& AssetKind,
		bool bExpectedAbsent,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool BuildAssetExportSnapshotReadRequests(
		UObject& Asset,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool AppendReusableBlueprintExportSnapshotReadRequests(
		UBlueprint& Blueprint,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FDiagnostic& OutError,
		const FString& CanonicalAssetPath = FString());
	// Operation comparisons retain generated program semantics without native child GUIDs.
	// Public export continues to use the complete node/property/pin/edge requests above.
	UEAGENTINTERFACE_API bool AppendReusableBlueprintVerificationReadRequests(
		UBlueprint& Blueprint,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FDiagnostic& OutError,
		const FString& CanonicalAssetPath = FString());
	UEAGENTINTERFACE_API bool AppendBlueprintRootPropertyReadRequests(
		UBlueprint& Blueprint,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool RegisterStateAccessor(UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool RegisterNativeDefaultProvider(UE::UAI::Contract::FDiagnostic& OutError);
}
