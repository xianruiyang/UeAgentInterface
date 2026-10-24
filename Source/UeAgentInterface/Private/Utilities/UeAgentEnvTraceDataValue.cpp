// Copyright Epic Games, Inc. All Rights Reserved.
#include "Utilities/UeAgentEnvTraceDataValue.h"

#include "Engine/EngineTypes.h"
#include "EnvironmentQuery/EnvQueryTypes.h"
#include "UObject/UnrealType.h"

namespace UeAgentEnvTraceDataValue
{
bool IsType(const UStruct* Struct)
{
    return Struct == FEnvTraceData::StaticStruct();
}

FEnvTraceData Canonical(const FEnvTraceData& Value)
{
    auto Result = Value;
    // Use UE's migration on a copy. Reading must not mutate the asset.
    Result.OnPostLoad();
    return Result;
}

bool ReconcileEdit(FEnvTraceData& Value, const FEnvTraceData& Before,
    bool bTracePresent, bool bSerializedPresent, UE::UAI::Contract::FDiagnostic& Error)
{
    const auto Baseline = Canonical(Before);
    const bool bTraceChanged = bTracePresent && Value.TraceChannel != Baseline.TraceChannel;
    const bool bSerializedChanged = bSerializedPresent && Value.SerializedChannel != Baseline.SerializedChannel;
    if (bTraceChanged && bSerializedChanged
        && UEngineTypes::ConvertToCollisionChannel(Value.TraceChannel) != Value.SerializedChannel)
    {
        Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("env_trace_channels_conflict"),
            TEXT("TraceChannel and SerializedChannel select different collision channels."), TEXT("property_value_decode"));
        return false;
    }
    if (bTraceChanged)
        Value.SerializedChannel = UEngineTypes::ConvertToCollisionChannel(Value.TraceChannel);
    else if (!bSerializedChanged)
        Value.SerializedChannel = Baseline.SerializedChannel;
    // The serialized channel is authoritative after migration. This is the same
    // reverse mapping UE performs when the asset is loaded.
    Value.VersionNum = 1;
    Value.OnPostLoad();
    return true;
}

void CopyChannels(const FEnvTraceData& Source, FEnvTraceData& Target)
{
    Target.TraceChannel = Source.TraceChannel;
    Target.SerializedChannel = Source.SerializedChannel;
    Target.VersionNum = Source.VersionNum;
}

FEnvTraceData* LinkedMemberContainer(const FProperty* Property, void* NativeValue)
{
    if (!Property || !NativeValue || !IsType(Property->GetOwnerStruct())
        || (Property->GetFName() != GET_MEMBER_NAME_CHECKED(FEnvTraceData, TraceChannel)
            && Property->GetFName() != GET_MEMBER_NAME_CHECKED(FEnvTraceData, SerializedChannel)))
        return nullptr;
    return reinterpret_cast<FEnvTraceData*>(static_cast<uint8*>(NativeValue) - Property->GetOffset_ForInternal());
}

void FinalizeLinkedMember(const FProperty* Property, void* NativeValue)
{
    auto* Value = LinkedMemberContainer(Property, NativeValue);
    if (!Value) return;
    if (Property->GetFName() == GET_MEMBER_NAME_CHECKED(FEnvTraceData, TraceChannel))
        Value->SerializedChannel = UEngineTypes::ConvertToCollisionChannel(Value->TraceChannel);
    Value->VersionNum = 1;
    Value->OnPostLoad();
}
}
