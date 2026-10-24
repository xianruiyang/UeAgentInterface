// Copyright Epic Games, Inc. All Rights Reserved.
#include "Utilities/UeAgentSkeletonVirtualBones.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Animation/Skeleton.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

bool UeAgentSkeletonVirtualBones::TryAdd(USkeleton& Skeleton, FName Source, FName Target,
    FName& Name, UE::UAI::Contract::FDiagnostic& OutError, FString& OutField,
    const FDiagnosticSource& DiagnosticSource)
{
    OutError.Reset();
    OutField.Reset();
    if (Name.IsNone() ? Skeleton.AddNewVirtualBone(Source, Target, Name)
                      : Skeleton.AddNewNamedVirtualBone(Source, Target, Name)) return true;

    FString Reason;
    if (!Name.IsNone() && !VirtualBoneNameHelpers::CheckVirtualBonePrefix(Name.ToString()))
    {
        OutField = TEXT("name");
        Reason = FString::Printf(TEXT("Virtual bone name '%s' must start with the native prefix '%s'. Omit name to use the native generated name."),
            *Name.ToString(), *VirtualBoneNameHelpers::VirtualBonePrefix);
    }
    else
    {
        for (const FVirtualBone& Bone : Skeleton.GetVirtualBones())
        {
            if (Bone.SourceBoneName == Source && Bone.TargetBoneName == Target)
            {
                OutField = TEXT("target");
                Reason = FString::Printf(TEXT("Virtual bone '%s' already connects '%s' to '%s'; the native skeleton permits one virtual bone per source/target pair."),
                    *Bone.VirtualBoneName.ToString(), *Source.ToString(), *Target.ToString());
                break;
            }
            if (Bone.VirtualBoneName == Name)
            {
                OutField = TEXT("name");
                Reason = FString::Printf(TEXT("Virtual bone name '%s' already exists."), *Name.ToString());
                break;
            }
        }
    }
    if (Reason.IsEmpty()) Reason = FString::Printf(TEXT("The native skeleton rejected creation of virtual bone '%s' from '%s' to '%s'."),
        *Name.ToString(), *Source.ToString(), *Target.ToString());
    UeAgentAuthoringDiagnostics::FDiagnostic Diagnostic;
    Diagnostic.Code = TEXT("animation_virtual_bone_add_failed");
    Diagnostic.Stage = TEXT("native_apply");
    Diagnostic.TargetPath = DiagnosticSource.TargetPath.IsEmpty() ? Skeleton.GetPathName() : DiagnosticSource.TargetPath;
    Diagnostic.Source = OutField == TEXT("name") ? DiagnosticSource.Name
        : OutField == TEXT("target") ? DiagnosticSource.TargetBone : DiagnosticSource.Operation;
    Diagnostic.SourceKind = Diagnostic.Source.SourceFile.IsEmpty() && Diagnostic.Source.JsonPath.IsEmpty()
        && Diagnostic.Source.InternalJsonPath.IsEmpty() ? TEXT("native_result") : TEXT("related_input");
    const auto Arguments = MakeShared<FJsonObject>();
    Arguments->SetStringField(TEXT("name"), Name.ToString());
    Arguments->SetStringField(TEXT("source"), Source.ToString());
    Arguments->SetStringField(TEXT("target"), Target.ToString());
    Diagnostic.ActualValue = MakeShared<FJsonValueObject>(Arguments);
    Diagnostic.PublicMessage = MoveTemp(Reason);
    OutError = Diagnostic;
    return false;
}
