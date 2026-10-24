// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"

class FProperty;
class UStruct;
struct FEnvTraceData;

// FEnvTraceData has one channel represented by an editor enum and a serialized
// collision enum. Keep UE's type semantics independent of the asset owning it.
namespace UeAgentEnvTraceDataValue
{
bool IsType(const UStruct* Struct);
FEnvTraceData Canonical(const FEnvTraceData& Value);
bool ReconcileEdit(FEnvTraceData& Value, const FEnvTraceData& Before,
    bool bTracePresent, bool bSerializedPresent, UE::UAI::Contract::FDiagnostic& Error);
void CopyChannels(const FEnvTraceData& Source, FEnvTraceData& Target);

// Only call with a resolved native member address, never detached scalar storage.
FEnvTraceData* LinkedMemberContainer(const FProperty* Property, void* NativeValue);
void FinalizeLinkedMember(const FProperty* Property, void* NativeValue);
}
