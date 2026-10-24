// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UObject;
class UEdGraph;
class FJsonValue;
namespace UeAgentAuthoringObjectApply { struct FOptions; struct FFailure; }

namespace UeAgentBlueprintAuthoring
{
// The native owner regenerates this graph's children; their GUIDs are not
// persistent authored identities. The graph itself keeps its ordinary identity.
bool IsNativeGeneratedGraph(const UEdGraph& Graph);

// Native node languages can store a canonical representation of authored text.
// Resolve it in an isolated native context, before the ordinary property apply.
bool NormalizeNativeNodeProperties(UObject& Object,
    TArray<TSharedPtr<FJsonValue>>& Descriptors,
    const UeAgentAuthoringObjectApply::FOptions& Options,
    UeAgentAuthoringObjectApply::FFailure& OutFailure);
}
