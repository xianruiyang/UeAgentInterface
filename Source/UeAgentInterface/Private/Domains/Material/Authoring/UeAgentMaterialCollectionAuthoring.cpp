#include "Domains/Material/Authoring/UeAgentMaterialCollectionAuthoring.h"

#include "Core/HjsonWorkspace/UeAgentKeyedItemAuthoringProjection.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Materials/MaterialParameterCollection.h"
#include "Misc/ScopeExit.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringEncodedTypes.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"

namespace UeAgentMaterialAuthoring::Collection
{
namespace
{
template <typename T>
bool ReadParameters(UMaterialParameterCollection& Owner, const TArray<T>& Parameters,
    TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& Error)
{
    auto Items = MakeShared<FJsonObject>();
    for (const auto& Parameter : Parameters)
    {
        UeAgentAuthoringState::FStateSnapshot Snapshot;
        TSharedPtr<FJsonObject> Properties;
        if (!UeAgentDetailsAuthoringService::CaptureStructProperties(T::StaticStruct(), &Parameter,
            Owner.GetPathName(), &Owner, {}, Snapshot, Properties, Error)
            || !UeAgentDetailsAuthoringService::ProjectReadableProperties(Snapshot, Properties, Error)) return false;
        auto Record = MakeShared<FJsonObject>();
        Record->SetObjectField(TEXT("settings"), Properties);
        Items->SetObjectField(Parameter.ParameterName.ToString(), Record);
    }
    UeAgentAuthoringEncodedTypes::FScope::RecordNameKeyedObject(Items);
    Value = MakeShared<FJsonValueObject>(Items);
    return true;
}

template <typename T>
bool PrepareParameters(UMaterialParameterCollection& Owner, const TArray<T>& Before,
    const TSharedPtr<FJsonValue>& Desired, const FString& Field, TArray<T>& After,
    TArray<FParameterIntent>& Intents, UE::UAI::Contract::FDiagnostic& Error)
{
    using namespace UE::UAI;
    TSharedPtr<FJsonValue> Captured;
    if (!ReadParameters(Owner, Before, Captured, Error)) return false;
    auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
    StateSemantic::FUeAgentCanonicalValuePtr Current, Proposed, Resolved;
    if (!StateSemantic::CanonicalJson::TryFromJsonValue(Captured, Resources, Current, Error)
        || !StateSemantic::CanonicalJson::TryFromJsonValue(Desired, Resources, Proposed, Error)) return false;
    HjsonWorkspace::FKeyedItemProjectionContract Contract;
    Contract.RootField = Field;
    Contract.bIdentityFromMapKey = true;
    Contract.bMapKeyUsesNameIdentity = true;
    Contract.bRequiresCreationCandidate = false;
    Contract.VisibleFieldPolicy.VisibleTopLevelFields = {TEXT("settings")};
    HjsonWorkspace::FWorkspaceError WorkspaceError;
    bool Changed = false;
    if (!HjsonWorkspace::TryResolveKeyedItemDesiredState(*Proposed, *Current, Field,
        Contract, nullptr, Resources, Resolved, Changed, WorkspaceError))
    { Error = WorkspaceError.GetDiagnostic(); return false; }
    TSharedPtr<FJsonValue> ResolvedJson;
    if (!StateSemantic::CanonicalJson::TryToJsonValue(*Resolved, ResolvedJson, Error)) return false;
    const auto Items = ResolvedJson->AsObject();
    TArray<FString> Names;
    // Existing native IDs and order survive edits and deletion of other entries.
    for (const auto& Parameter : Before)
        if (Items->HasField(Parameter.ParameterName.ToString())) Names.Add(Parameter.ParameterName.ToString());
    TArray<FString> Added;
    for (const auto& Item : Items->Values) if (!Names.Contains(Item.Key)) Added.Add(Item.Key);
    Added.Sort(); Names.Append(Added);
    for (const auto& Name : Names)
    {
        const auto* Existing = Before.FindByPredicate([&](const T& P) { return P.ParameterName == FName(*Name); });
        T Parameter = Existing ? *Existing : T();
        if (!Existing) Parameter.ParameterName = FName(*Name);
        const auto Record = Items->GetObjectField(Name);
        const auto SourceValue = Desired && Desired->Type == EJson::Object ? Desired->AsObject()->TryGetField(Name) : nullptr;
        const auto SourceRecord = SourceValue && SourceValue->Type == EJson::Object ? SourceValue->AsObject() : nullptr;
        const auto SourceLocations = SourceRecord
            ? UeAgentAuthoringDiagnostics::FScopedSourceLocations::CaptureProjection(*SourceRecord)
            : TMap<FString, UE::UAI::Contract::FSourceLocation>();
        UeAgentAuthoringDiagnostics::FScopedSourceLocations Sources(Record.Get(),
            [&SourceLocations](const FString& Pointer) {
                return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(SourceLocations, Pointer);
            });
        ON_SCOPE_EXIT {
            if (!Error.IsEmpty() && SourceRecord)
                UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *SourceRecord);
        };
        FParameterIntent Intent;
        Intent.SourceKey = Name;
        if (const auto Settings = Record->TryGetField(TEXT("settings")))
        {
            UeAgentAuthoringState::FStateSnapshot Snapshot;
            TSharedPtr<FJsonObject> Ignored;
            TArray<TSharedPtr<FJsonValue>> Descriptors;
            if (!UeAgentDetailsAuthoringService::CaptureStructProperties(T::StaticStruct(), &Parameter,
                Owner.GetPathName(), &Owner, {}, Snapshot, Ignored, Error)
                || !UeAgentDetailsAuthoringService::DecodeReadableProperties(Snapshot, Settings, Descriptors, Error)) return false;
            UeAgentAuthoringObjectApply::FOptions Options;
            Options.bCompactValues = true;
            Options.bEmitEditorNotifications = false;
            Options.bMarkPackageDirty = false;
            UeAgentAuthoringObjectApply::FPreparedApply Prepared;
            UeAgentAuthoringObjectApply::FFailure Failure;
            UeAgentAuthoringObjectApply::FApplyResult Applied;
            if (!UeAgentAuthoringObjectApply::PrepareStruct(T::StaticStruct(), &Parameter, &Owner,
                Descriptors, Options, Prepared, Failure)) {
                Error = Failure.Diagnostic;
                UeAgentDetailsAuthoringService::LocateReadablePropertyFailure(Error, Settings, TEXT("/") + Field + TEXT("/")
                    + Name.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")) + TEXT("/settings"));
                return false;
            }
            if (!UeAgentAuthoringObjectApply::ApplyPrepared(Prepared, Applied)) {
                Error = Applied.Diagnostic; return false;
            }
            if (!Prepared.CaptureIntent(Intent.Properties, Failure)) {
                Error = Failure.Diagnostic; return false;
            }
            Intent.Settings = SourceRecord ? SourceRecord->TryGetField(TEXT("settings")) : nullptr;
        }
        Intent.Name = Parameter.ParameterName;
        Intents.Add(MoveTemp(Intent));
        After.Add(MoveTemp(Parameter));
    }
    return true;
}

template <typename T>
bool VerifyParameters(UMaterialParameterCollection& Owner, TArray<T>& Parameters,
    const TArray<FParameterIntent>& Intents, const FString& Field, UE::UAI::Contract::FDiagnostic& Error)
{
    if (Parameters.Num() != Intents.Num()) {
        Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("material_collection_intent_count_mismatch"),
            FString::Printf(TEXT("Material collection '%s' has %d %s parameters after applying %d entries."),
                *Owner.GetName(), Parameters.Num(), *Field, Intents.Num()), TEXT("authoring"));
        Error.TargetPath = Owner.GetPathName();
        Error.PropertyPath = Field;
        Error.ActualValue = MakeShared<FJsonValueNumber>(Parameters.Num());
        Error.ExpectedValue = MakeShared<FJsonValueNumber>(Intents.Num());
        return false;
    }
    for (int32 Index = 0; Index < Intents.Num(); ++Index) {
        const auto& Intent = Intents[Index];
        auto& Parameter = Parameters[Index];
        if (Parameter.ParameterName != Intent.Name) {
            Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("material_collection_intent_name_mismatch"),
                FString::Printf(TEXT("Material collection %s parameter at index %d did not retain the requested name '%s'."),
                    *Field, Index, *Intent.Name.ToString()), TEXT("authoring"));
            Error.TargetPath = Owner.GetPathName();
            Error.PropertyPath = Field + TEXT("/") + Intent.SourceKey;
            Error.ActualValue = MakeShared<FJsonValueString>(Parameter.ParameterName.ToString());
            Error.ExpectedValue = MakeShared<FJsonValueString>(Intent.Name.ToString());
            return false;
        }
        UeAgentAuthoringObjectApply::FApplyResult Result;
        if (!Intent.Properties.IsEmpty() && !Intent.Properties.VerifyStruct(T::StaticStruct(), &Parameter, &Owner, Result)) {
            Error = Result.Diagnostic;
            UeAgentDetailsAuthoringService::LocateReadablePropertyFailure(Error, Intent.Settings, TEXT("/") + Field + TEXT("/")
                + Intent.SourceKey.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")) + TEXT("/settings"));
            return false;
        }
    }
    return true;
}
}

bool FIntent::Verify(UMaterialParameterCollection& Asset, UE::UAI::Contract::FDiagnostic& Error) const
{
    UeAgentAuthoringDiagnostics::FScopedSourceLocations Sources(SourceDocument.Get(),
        [this](const FString& Pointer) {
            return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(SourceLocations, Pointer);
        });
    ON_SCOPE_EXIT {
        if (!Error.IsEmpty() && SourceDocument)
            UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *SourceDocument);
    };
    return (!bScalars || VerifyParameters(Asset, Asset.ScalarParameters, Scalars, TEXT("scalars"), Error))
        && (!bVectors || VerifyParameters(Asset, Asset.VectorParameters, Vectors, TEXT("vectors"), Error));
}

bool Read(UMaterialParameterCollection& Asset, TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& Error)
{
    auto Result = MakeShared<FJsonObject>();
    TSharedPtr<FJsonValue> Scalars, Vectors;
    if (!ReadParameters(Asset, Asset.ScalarParameters, Scalars, Error)
        || !ReadParameters(Asset, Asset.VectorParameters, Vectors, Error)) return false;
    Result->SetField(TEXT("scalars"), Scalars);
    Result->SetField(TEXT("vectors"), Vectors);
    Value = MakeShared<FJsonValueObject>(Result);
    return true;
}

bool Apply(UMaterialParameterCollection& Asset, const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& Error,
    TSharedPtr<const FIntent>* OutIntent)
{
    if (OutIntent) OutIntent->Reset();
    if (!Value || Value->Type != EJson::Object) {
        Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("material_collection_parameters_object_required"),
            TEXT("Material collection parameters must be an object containing scalar or vector entries."), TEXT("authoring"));
        Error.TargetPath = Asset.GetPathName();
        Error.PropertyPath = TEXT("collection_parameters");
        Error.ActualValue = Value;
        Error.ActualValuePresent = Value.IsValid();
        Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
        return false;
    }
    auto Scalars = Asset.ScalarParameters;
    auto Vectors = Asset.VectorParameters;
    auto Intent = MakeShared<FIntent>();
    Intent->SourceDocument = Value->AsObject();
    Intent->SourceLocations = UeAgentAuthoringDiagnostics::FScopedSourceLocations::CaptureProjection(*Intent->SourceDocument);
    if (const auto Desired = Value->AsObject()->TryGetField(TEXT("scalars")))
    {
        Scalars.Reset();
        Intent->bScalars = true;
        if (!PrepareParameters(Asset, Asset.ScalarParameters, Desired, TEXT("scalars"), Scalars, Intent->Scalars, Error)) return false;
    }
    if (const auto Desired = Value->AsObject()->TryGetField(TEXT("vectors")))
    {
        Vectors.Reset();
        Intent->bVectors = true;
        if (!PrepareParameters(Asset, Asset.VectorParameters, Desired, TEXT("vectors"), Vectors, Intent->Vectors, Error)) return false;
    }
    Asset.Modify();
    Asset.PreEditChange(nullptr);
    Asset.ScalarParameters = MoveTemp(Scalars);
    Asset.VectorParameters = MoveTemp(Vectors);
    Asset.PostEditChange();
    Asset.MarkPackageDirty();
    if (!Intent->Verify(Asset, Error)) return false;
    if (OutIntent) *OutIntent = Intent;
    return true;
}

TSharedPtr<FJsonObject> BuildValueDefinition()
{
    auto Fields = MakeShared<FJsonObject>();
    for (const TCHAR* Name : {TEXT("scalars"), TEXT("vectors")})
    {
        auto Field = MakeShared<FJsonObject>();
        auto Contract = MakeShared<FJsonObject>();
        Contract->SetStringField(TEXT("collection_semantics"), TEXT("keyed_desired_state"));
        Field->SetStringField(TEXT("type"), TEXT("object"));
        Field->SetObjectField(TEXT("contract"), Contract);
        Fields->SetObjectField(Name, Field);
    }
    auto Definition = MakeShared<FJsonObject>();
    Definition->SetObjectField(TEXT("fields"), Fields);
    return Definition;
}
}
