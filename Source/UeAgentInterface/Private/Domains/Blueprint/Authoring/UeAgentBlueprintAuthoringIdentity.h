// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;

namespace UeAgentBlueprintAuthoring
{
	/** Stable public identity shared by Blueprint graph export, search and mutation. */
	UEAGENTINTERFACE_API FString GraphIdentity(const UEdGraph& Graph);

	/** Captured native node reference shared by graph state and scoped queries. */
	UEAGENTINTERFACE_API FString NodeReference(const UEdGraphNode* Node);
	/** Signature-owned entry/result identity, independent of whether it has a GUID. */
	UEAGENTINTERFACE_API FString GetBuiltinNodeId(const UEdGraphNode* Node);

	/** Human-readable node identity shared by state export and scoped queries. */
	UEAGENTINTERFACE_API FString NodeSemanticName(const UEdGraphNode& Node);
	/** Native lookup for the same node-local pin aliases used by HJSON. */
	UEAGENTINTERFACE_API bool BuildPinAliasLookup(
		const UEdGraphNode& Node, TMap<FString, UEdGraphPin*>& OutPins, UE::UAI::Contract::FDiagnostic& OutError);
}
