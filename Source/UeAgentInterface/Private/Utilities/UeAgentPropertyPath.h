#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/UeAgentPropertyValueCodec.h"

class FProperty;
class FSetProperty;
class FArrayProperty;
class FJsonValue;

namespace UeAgentPropertyPath
{
using UE::UAI::Contract::FDiagnostic;
    struct FResolved
    {
        struct FSetSelection
        {
            int32 PathIndex = INDEX_NONE;
            FProperty* ElementProperty = nullptr;
            void* ElementAddress = nullptr;
        };
        FProperty* Property = nullptr;
        void* Address = nullptr;
        bool bSingleFixedElement = false;
        bool bReadOnly = false;
        TArray<TPair<FSetProperty*, void*>> Sets;
        TArray<TPair<FArrayProperty*, void*>> ArrayDependencies;
        TArray<FSetSelection> SetSelections;
    };

    // Caller owns the memory and mutation policy. For writes resolve a temporary
    // root copy, decode the leaf, then FinishWrite before committing the root.
    UEAGENTINTERFACE_API bool Resolve(FProperty* Root, void* Address, const TArray<TSharedPtr<FJsonValue>>& Path,
        UObject* Owner, const UeAgentPropertyValueCodec::FOptions& Options,
        FResolved& Out, FDiagnostic& Error, bool bTypeOnly = false);
    UEAGENTINTERFACE_API bool FinishWrite(const FResolved& Target, FDiagnostic& Error);
    // Freeze selectors before releasing temporary write storage. A set element's
    // value is its identity, so a successful edit can change its readback path.
    UEAGENTINTERFACE_API bool CaptureWritePath(const FResolved& Target,
        const TArray<TSharedPtr<FJsonValue>>& Path, UObject* Owner,
        const UeAgentPropertyValueCodec::FOptions& Options,
        TArray<TSharedPtr<FJsonValue>>& OutPath, FDiagnostic& Error);
}
