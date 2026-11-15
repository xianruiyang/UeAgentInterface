// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FProperty;

namespace UeAgentPropertyValueCodec
{
// A narrow proof for the support predicate only, never for encoded values.
// Reuse is limited to the same synchronous read-only scope and exact property.
// Dynamic/container/generic/custom shapes return false and use the normal reader.
bool TryGetValueIndependentAuthoringSupport(const FProperty* Property, bool& OutSupported);
}
