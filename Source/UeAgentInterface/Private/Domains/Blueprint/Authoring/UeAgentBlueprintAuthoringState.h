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
