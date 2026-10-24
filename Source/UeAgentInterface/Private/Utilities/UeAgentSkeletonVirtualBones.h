// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"
#include "CoreMinimal.h"
class USkeleton;

namespace UeAgentSkeletonVirtualBones
{
    struct FDiagnosticSource
    {
        FString TargetPath;
        UE::UAI::Contract::FSourceLocation Operation;
        UE::UAI::Contract::FSourceLocation Name;
        UE::UAI::Contract::FSourceLocation TargetBone;
    };
    // Uses native naming and duplicate rules; preserves the reason for native rejection.
    bool TryAdd(USkeleton& Skeleton, FName Source, FName Target, FName& Name,
        UE::UAI::Contract::FDiagnostic& OutError, FString& OutField,
        const FDiagnosticSource& DiagnosticSource = FDiagnosticSource());
}
