// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UObject;
class FJsonValue;

namespace UeAgentAuthoringObjectApply {
struct FOptions;
struct FFailure;

/** Normalize component collision intent on an isolated preview. The actual
 * instance/template supplies the edit baseline, never a reconstructed clone. */
bool NormalizePrimitiveComponentProperties(UObject* Proposed, UObject* Baseline,
    TArray<TSharedPtr<FJsonValue>>& Descriptors, const FOptions& Options,
    FFailure& OutFailure);
}
