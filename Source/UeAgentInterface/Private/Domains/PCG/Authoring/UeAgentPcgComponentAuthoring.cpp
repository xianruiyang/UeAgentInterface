#include "Domains/PCG/Authoring/UeAgentPcgComponentAuthoring.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Class.h"
#include "Domains/PCG/UeAgentPCGDomainBackend.h"
#include "PCGComponent.h"
#include "PCGGraph.h"
#include "PCGSubsystem.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeExit.h"
#include "StructUtils/PropertyBag.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

namespace UeAgentPcgComponentAuthoring
{
namespace
{
using UE::UAI::Contract::FDiagnostic;

FDiagnostic InputFailure(const TCHAR* Code, const TCHAR* Reason, const FJsonObject& Input,
    const FString& Field, const TSharedPtr<FJsonValue>& Actual, const TCHAR* Expected,
    const FString& Pointer = FString())
{
    auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Field,
        MakeShared<FJsonValueString>(Expected), Actual, TEXT("pcg_component_apply"), Reason);
    Error.Source.InternalJsonPath = Pointer.IsEmpty() ? TEXT("/") + Field : Pointer;
    UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, Input, Field);
    return Error;
}

void BindTarget(FDiagnostic& Error, const UActorComponent& Component, const FString& LogicalTarget)
{
    if (!Error.IsValid()) return;
    const FString NativeTarget = Component.GetPathName();
    const FString Target = LogicalTarget.IsEmpty() ? NativeTarget : LogicalTarget;
    TFunction<void(FDiagnostic&)> Bind = [&](FDiagnostic& Issue)
    {
        if (Issue.TargetPath.IsEmpty()) Issue.TargetPath = Target;
        else if (Issue.TargetPath == NativeTarget || Issue.TargetPath.StartsWith(NativeTarget + TEXT("."))
            || Issue.TargetPath.StartsWith(NativeTarget + TEXT(":")))
        {
            if (Issue.TargetPath != Target)
            {
                if (!Issue.InternalContext.IsEmpty()) Issue.InternalContext += TEXT("; ");
                Issue.InternalContext += Issue.TargetPath;
                Issue.TargetPath = Target;
            }
        }
        for (auto& Child : Issue.Diagnostics) Bind(Child);
        if (Issue.PublicCause)
        {
            auto Cause = MakeShared<FDiagnostic>(*Issue.PublicCause);
            Bind(*Cause);
            Issue.PublicCause = MoveTemp(Cause);
        }
    };
    Bind(Error);
}
}

bool SettleOwnedInputs(AActor& Actor, UE::UAI::Contract::FDiagnostic& Error)
{
    TArray<UPCGComponent*> Components;
    Actor.GetComponents(Components);
    for (UPCGComponent* Component : Components)
    {
        if (!Component->bActivated || !Component->GetGraph()
            || !Component->bRegenerateInEditor || Component->IsManagedByRuntimeGenSystem()
            || (!Component->bGenerated && !Component->IsGenerating()
                && !Component->ShouldGenerateBPPCGAddedToWorld())) continue;
        UPCGSubsystem* Subsystem = Component->GetSubsystem();
        if (!Subsystem) continue;
        Component->DirtyGenerated(EPCGComponentDirtyFlag::Input);
        Component->Refresh(EPCGChangeType::Input);
        const double Started = FPlatformTime::Seconds();
        while (Component->IsRefreshInProgress() || Component->IsGenerating() || Component->IsCleaningUp())
        {
            // Advance PCG work only, not a reentrant Editor/world tick.
            Subsystem->Tick(0.0f);
            if (FPlatformTime::Seconds() - Started > 30.0)
            {
                auto State = MakeShared<FJsonObject>();
                State->SetNumberField(TEXT("elapsed_seconds"), FPlatformTime::Seconds() - Started);
                State->SetBoolField(TEXT("refresh_in_progress"), Component->IsRefreshInProgress());
                State->SetBoolField(TEXT("generating"), Component->IsGenerating());
                State->SetBoolField(TEXT("cleaning_up"), Component->IsCleaningUp());
                Subsystem->CancelGeneration(Component);
                Error = FDiagnostic::Failure(TEXT("pcg_native_refresh_did_not_settle"),
                    TEXT("Native PCG refresh did not settle within 30 seconds and generation was cancelled."),
                    TEXT("pcg_native_refresh"));
                Error.SourceKind = TEXT("native_result");
                Error.TargetPath = Component->GetPathName();
                Error.PropertyPath = TEXT("refresh_state");
                Error.ActualValue = MakeShared<FJsonValueObject>(State);
                Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("refresh, generation and cleanup complete within 30 seconds"));
                return false;
            }
            FPlatformProcess::SleepNoStats(0.001f);
        }
        if (!Component->bGenerated)
        {
            Error = FDiagnostic::Failure(TEXT("pcg_native_refresh_failed"),
                TEXT("Native PCG refresh finished without generating the component."), TEXT("pcg_native_refresh"));
            Error.SourceKind = TEXT("native_result");
            Error.TargetPath = Component->GetPathName();
            Error.PropertyPath = TEXT("generated");
            Error.ActualValue = MakeShared<FJsonValueBoolean>(Component->bGenerated);
            Error.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
            return false;
        }
    }
    return true;
}

TSharedPtr<FJsonObject> Read(const UActorComponent& Component)
{
    const auto* Pcg = Cast<UPCGComponent>(&Component);
    const auto* Instance = Pcg ? Pcg->GetGraphInstance() : nullptr;
    if (!Instance) return nullptr;
    auto Value = MakeShared<FJsonObject>();
    Value->SetField(TEXT("graph"), UeAgentPropertyValueCodec::EncodeReferencePath(
        Instance->Graph ? Instance->Graph->GetPathName() : FString()));
    Value->SetObjectField(TEXT("parameters"), UeAgentPCG::ExportParameterBagToJson(Instance->GetUserParametersStruct()));
    TArray<TSharedPtr<FJsonValue>> Overrides;
    if (const auto* Struct = Instance->ParametersOverrides.Parameters.GetPropertyBagStruct())
        for (const auto& Desc : Struct->GetPropertyDescs())
            if (Instance->IsPropertyOverridden(Desc.CachedProperty))
                Overrides.Add(MakeShared<FJsonValueString>(Desc.Name.ToString()));
    Value->SetArrayField(TEXT("overrides"), Overrides);
    return Value;
}

bool Apply(UActorComponent& Component, const TSharedPtr<const FJsonObject>& Value,
    UE::UAI::Contract::FDiagnostic& Error, const FString& LogicalTarget)
{
    if (!Value) return true;
    ON_SCOPE_EXIT { BindTarget(Error, Component, LogicalTarget); };
    auto* Pcg = Cast<UPCGComponent>(&Component);
    auto* Instance = Pcg ? Pcg->GetGraphInstance() : nullptr;
    if (!Instance)
    {
        Error = FDiagnostic::Failure(TEXT("pcg_component_instance_required"),
            Pcg ? TEXT("The PCG component has no graph instance to edit.")
                : TEXT("PCG component settings require a PCGComponent target."),
            TEXT("pcg_component_apply"));
        Error.SourceKind = TEXT("native_result");
        Error.PropertyPath = Pcg ? TEXT("graph_instance_present") : TEXT("component_class");
        Error.ActualValue = Pcg ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false))
            : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Component.GetClass()->GetPathName()));
        Error.ExpectedValue = Pcg ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true))
            : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(UPCGComponent::StaticClass()->GetPathName()));
        return false;
    }
    UPCGGraphInterface* Graph = Instance->Graph;
    if (const auto GraphValue = Value->TryGetField(TEXT("graph")))
    {
        UObject* Resolved = nullptr;
        if (!UeAgentPropertyValueCodec::DecodeObjectReference(GraphValue, UPCGGraphInterface::StaticClass(), nullptr, Resolved, Error))
        {
            UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(Error, TEXT("/graph"));
            UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Value, TEXT("graph"));
            return false;
        }
        Graph = Cast<UPCGGraphInterface>(Resolved);
        if (Graph && !Instance->CanGraphInterfaceBeSet(Graph))
        {
            Error = InputFailure(TEXT("pcg_graph_instance_cycle"),
                TEXT("The requested parent graph would create a cycle in the PCG graph instance hierarchy."),
                *Value, TEXT("graph"), GraphValue, TEXT("a parent graph that does not reference this instance"));
            return false;
        }
    }
    const auto Before = Read(Component);
    const bool bGraphChanged = Graph != Instance->Graph;
    const auto Parameters = Value->TryGetField(TEXT("parameters"));
    const bool bKeepInheritedParameters = bGraphChanged && (!Parameters ||
        FJsonValue::CompareEqual(*Parameters, *Before->TryGetField(TEXT("parameters"))));
    // Native SetGraph owns parameter inheritance and component cache refresh.
    // The Level lifecycle invokes this on its isolated proposed component first.
    Component.Modify();
    Instance->Modify();
    if (bGraphChanged) Pcg->SetGraph(Graph);
    // Removing the parent also removes its schema and overrides. The exported
    // dependent fields must not recreate parameters after the native reset.
    if (!Graph) return true;
    TArray<FName> AppliedNames;
    if (Parameters && !bKeepInheritedParameters)
    {
        if (Parameters->Type != EJson::Object)
        {
            Error = InputFailure(TEXT("pcg_parameters_object_required"),
                TEXT("PCG parameters must be a canonical parameter bag object."),
                *Value, TEXT("parameters"), Parameters, TEXT("an object containing schema and fields"));
            return false;
        }
        TArray<UE::UAI::Contract::FDiagnostic> Issues;
        UeAgentPCG::ApplyCanonicalParameterBag(Instance->ParametersOverrides.Parameters,
            Parameters->AsObject(), Issues, &AppliedNames, false);
        if (UeAgentPCG::CountErrors(Issues))
        {
            Error = UeAgentAuthoringDiagnostics::MakeIssuesFailure(
                TEXT("pcg_parameter_values_invalid"), Issues, TEXT("parameter_apply"), Component.GetPathName());
            return false;
        }
    }
    if (const auto Overrides = Value->TryGetField(TEXT("overrides")))
    {
        if (bKeepInheritedParameters && FJsonValue::CompareEqual(
            *Overrides, *Before->TryGetField(TEXT("overrides")))) return true;
        if (Overrides->Type != EJson::Array)
        {
            Error = InputFailure(TEXT("pcg_overrides_array_required"),
                TEXT("PCG parameter overrides must be an array of parameter names."),
                *Value, TEXT("overrides"), Overrides, TEXT("an array of parameter names"));
            return false;
        }
        TSet<FName> Names;
        for (int32 InputIndex = 0; InputIndex < Overrides->AsArray().Num(); ++InputIndex)
        {
            const auto& Name = Overrides->AsArray()[InputIndex];
            FString Text;
            const bool bHasName = Name && Name->TryGetString(Text);
            if (!bHasName || !Instance->ParametersOverrides.Parameters.FindPropertyDescByName(FName(*Text)))
            {
                Error = InputFailure(TEXT("pcg_override_parameter_not_found"),
                    bHasName ? TEXT("The override name does not identify a parameter in the graph schema.")
                        : TEXT("The override entry cannot be read as a parameter name."),
                    *Value, TEXT("overrides"), Name, TEXT("a parameter name present in the graph schema"),
                    FString::Printf(TEXT("/overrides/%d"), InputIndex));
                return false;
            }
            Names.Add(FName(*Text));
        }
        if (const auto* Struct = Instance->ParametersOverrides.Parameters.GetPropertyBagStruct())
            for (const auto& Desc : Struct->GetPropertyDescs())
                Instance->UpdatePropertyOverride(Desc.CachedProperty, Names.Contains(Desc.Name));
    }
    UeAgentPCG::NotifyGraphInstanceParameterValuesChanged(Instance, AppliedNames);
    return true;
}
}
