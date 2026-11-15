#include "DomainAdapters/EditorProject/UeAgentSettingsAuthoring.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "DomainAdapters/EditorProject/UeAgentSettingsConfig.h"
#include "HAL/IConsoleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "Utilities/AuthoringExport/UeAgentExportWorkspaceWriter.h"

namespace UE::UAI::DomainAdapters::EditorProject
{
namespace
{
using namespace HjsonWorkspace;
using namespace HjsonWorkspace::DomainAuthoring;
const FString ProviderId = TEXT("project-settings-hjson-provider");
const FString Schema = TEXT("uai.project_settings.edit/1");
const FString InitContract = TEXT("uai.init.project_settings.root/1");

FString ConfigValue(const UObject& Object, FProperty& Property)
{
    FString Value;
    Property.ExportText_InContainer(0, Value, &Object, &Object, const_cast<UObject*>(&Object), PPF_ConsoleVariable);
    return Value;
}

bool MatchesConsoleValue(const UObject& Object, FProperty& Property, const IConsoleVariable& CVar)
{
    if (const auto* Bool = CastField<FBoolProperty>(&Property))
        return Bool->GetPropertyValue_InContainer(&Object) == (CVar.GetInt() != 0);
    if (const auto* Text = CastField<FStrProperty>(&Property))
        return Text->GetPropertyValue_InContainer(&Object) == CVar.GetString();
    if (const auto* Name = CastField<FNameProperty>(&Property))
        return Name->GetPropertyValue_InContainer(&Object).ToString() == CVar.GetString();
    if (const auto* Enum = CastField<FEnumProperty>(&Property))
        return Enum->GetUnderlyingProperty()->GetSignedIntPropertyValue(Enum->ContainerPtrToValuePtr<void>(&Object)) == CVar.GetInt();
    if (const auto* Number = CastField<FNumericProperty>(&Property))
    {
        const void* Value = Number->ContainerPtrToValuePtr<void>(&Object);
        return Number->IsFloatingPoint() ? Number->GetFloatingPointPropertyValue(Value) == static_cast<double>(CVar.GetFloat())
            : Number->GetSignedIntPropertyValue(Value) == CVar.GetInt();
    }
    return ConfigValue(Object, Property) == CVar.GetString();
}

class FProvider final : public IImportInitializationDomainProvider
{
public:
    const FString& GetProviderId() const override { return ProviderId; }
    bool Matches(const FImportInitializationDomainRequest& Request) const override
    {
        UObject* Object = StaticFindObject(UObject::StaticClass(), nullptr, *Request.GetTargetIdentity());
        return Request.GetRole() == EInitializationNodeRole::RootTarget && Object
            && Object->GetClass()->GetPathName() == Request.GetRootTypeIdentity()
            && IsRegisteredSettingsObject(*Object) && MatchesDomainImportSchema(Request, Schema);
    }
    bool TryResolveInitializationContract(const FImportInitializationDomainRequest& Request,
        FString& Contract, FWorkspaceError&) const override
    { if (!Matches(Request)) return false; Contract = InitContract; return true; }

    bool TryExecute(const FImportInitializationDomainRequest& Request, const FString& Contract,
        FImportInitializationDomainResponse& Response, FWorkspaceError& Error) const override
    {
        if (Contract != InitContract || !Matches(Request))
        {
            auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("settings_target_invalid"),
                Contract != InitContract ? TEXT("The initialization contract does not match the registered settings provider.")
                    : TEXT("The request must identify an existing registered settings object, its concrete type, and a root-target initialization role."), TEXT("domain_execute"));
            Detail.TargetPath = Request.GetTargetIdentity();
            Detail.InternalContext = Contract;
            Detail.SourceKind = TEXT("native_result");
            Error.SetFailure(MoveTemp(Detail));
            return false;
        }
        UObject* Object = StaticFindObject(UObject::StaticClass(), nullptr, *Request.GetTargetIdentity());
        TArray<UeAgentAuthoringState::FReadRequest> Requests;
        UE::UAI::Contract::FDiagnostic Detail;
        UeAgentAuthoringState::FCaptureResult Capture;
        FString Payload;
        if (Request.GetCommandId() == TEXT("content_graph.export_workspace"))
        {
            if (!BuildSettingsReadRequests(*Object, Requests, Detail) || !UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
            { Error.SetFailure(Detail.IsEmpty() ? Capture.Diagnostic : Detail, Request.GetTargetIdentity()); return false; }
            TSharedPtr<FJsonObject> Params;
            FString Path;
            UE::UAI::Contract::FDiagnostic ParseError;
            if (!CommandContract::Json::TryParseObject(Request.GetCanonicalParamsJson(), Params, ParseError))
            { Error.SetFailure(MoveTemp(ParseError)); return false; }
            if (!Params->TryGetStringField(TEXT("workspace_path"), Path))
            {
                auto Failure = UE::UAI::Contract::FDiagnostic::Failure(TEXT("settings_workspace_path_missing"),
                    TEXT("Settings export requires a workspace_path string."), TEXT("workspace_export"));
                Failure.SourceKind = TEXT("command_input");
                Failure.Source.JsonPath = TEXT("/workspace_path");
                Failure.ActualValue = Params->TryGetField(TEXT("workspace_path"));
                Failure.ActualValuePresent = Failure.ActualValue.IsValid();
                Failure.ExpectedValue = MakeShared<FJsonValueString>(TEXT("workspace path string"));
                Error.SetFailure(MoveTemp(Failure));
                return false;
            }
            FString Profile = TEXT("llm_authoring");
            Params->TryGetStringField(TEXT("profile_id"), Profile);
            UeAgentExportWorkspaceWriter::FWorkspaceResult Result;
            if (!TryExportCompositionWorkspace(TEXT("ue.project_settings.config"), Profile, Capture.Snapshot, Path, Result,
                Request.GetWorkspaceExportPreparation().Get()))
            { Error.SetFailure(Result.Diagnostic, Request.GetTargetIdentity()); return false; }
            if (!TryCanonicalizeObject(Result.ToPublicJsonObject(), Payload, Error)) return false;
            Response = FImportInitializationDomainResponse(MoveTemp(Payload), true);
            return true;
        }
        if (Request.GetCommandId() != TEXT("content_graph.import_workspace"))
        {
            auto Failure = UE::UAI::Contract::FDiagnostic::Failure(TEXT("settings_command_unsupported"),
                TEXT("The registered settings provider supports only workspace export and import."), TEXT("domain_execute"));
            Failure.CandidateName = Request.GetCommandId();
            Failure.TargetPath = Request.GetTargetIdentity();
            Error.SetFailure(MoveTemp(Failure));
            return false;
        }
        FResolvedDomainHjsonDocument Document;
        if (!TryDecodeResolvedDocument(Request.GetTargetIdentity(), Request.GetCanonicalParamsJson(),
            {ProviderId, Schema, Request.GetRootTypeIdentity()}, Document, Error)) return false;
        FScopedDomainHjsonDiagnosticSource DiagnosticSource(Document, Error);
        if (!Document.bSemanticWorkspaceDirectory)
        {
            auto Failure = UE::UAI::Contract::FDiagnostic::Failure(TEXT("settings_semantic_workspace_required"),
                TEXT("Settings import requires an exported semantic workspace directory."), TEXT("document_restore"));
            Failure.TargetPath = Request.GetTargetIdentity();
            Failure.SourceKind = TEXT("command_input");
            Failure.Source.JsonPath = TEXT("/source");
            Error.SetFailure(MoveTemp(Failure));
            return false;
        }
        auto Resources = MakeSemanticWorkspaceResourceTracker();
        if (!BuildSettingsReadRequests(*Object, Requests, Detail))
        { Error.SetFailure(Detail, Request.GetTargetIdentity()); return false; }
        SelectPropertyImportReadRequests(*Document.SemanticWorkspace, Requests);
        if (!CapturePropertyImportState(*Document.SemanticWorkspace, Requests, Capture))
        { Error.SetFailure(Capture.Diagnostic, Request.GetTargetIdentity()); return false; }
        StateSemantic::FUeAgentCanonicalSnapshotPtr Snapshot;
        FAuthoringModelPtr Model;
        if (!TryBuildPropertyImportCurrentCapture(Document.SemanticWorkspace.Get(), TEXT("ue.project_settings.config"), TEXT("llm_authoring"),
            Capture.Snapshot, Resources, Snapshot, Model, Error)) return false;
        Capture.Snapshot.ReleaseNativeValues();
        TMap<FName, FString> Before;
        for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
            if (It->HasAnyPropertyFlags(CPF_Config)) Before.Add(It->GetFName(), ConfigValue(*Object, **It));
        const FString Target = Request.GetTargetIdentity();
        const FString RootType = Request.GetRootTypeIdentity();
        if (!TryCoordinateSemanticWorkspaceImport(Document.SemanticWorkspace.ToSharedRef(),
            Model.ToSharedRef(), Snapshot.ToSharedRef(),
            BuildSemanticWorkspaceImportDescriptor(TEXT("project_settings.semantic_workspace"), *Model),
            Target, Resources,
            {ProviderId, Target, Target, TEXT("project-settings"), TEXT("settings_prepare_failed"), TEXT("settings_apply_failed"),
                TEXT("settings_readback_invalid"), TEXT("settings_schema_invalid"),
                [Target](TArray<UeAgentAuthoringState::FReadRequest>& Requests, UE::UAI::Contract::FDiagnostic& Detail) {
                    UObject* Native = StaticFindObject(UObject::StaticClass(), nullptr, *Target);
                    if (!Native)
                    {
                        Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("settings_target_invalid"),
                            TEXT("The registered settings object is no longer available for native readback."), TEXT("readback"));
                        Detail.TargetPath = Target;
                        Detail.SourceKind = TEXT("native_result");
                        return false;
                    }
                    return BuildSettingsReadRequests(*Native, Requests, Detail);
                }},
            [Object](const FString& RequestId, const Composition::HjsonImport::FPreparedAuthoringImport&,
                Composition::HjsonImport::FSemanticWorkspaceChangeProjection&& Projection, FRootPropertyApplyOutcome& Outcome)
            {
                if (!Projection.StructuralEdits.IsEmpty())
                {
                    Outcome.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("settings_change_surface_unsupported"),
                        TEXT("Settings import supports property changes, but no structural edits."), TEXT("prepare"));
                    Outcome.Diagnostic.TargetPath = Object->GetPathName();
                    Outcome.Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(Projection.StructuralEdits.Num());
                    Outcome.Diagnostic.ExpectedValue = MakeShared<FJsonValueNumber>(0);
                    return false;
                }
                return ApplySettingsProperties(*Object, RequestId, Projection.RootObjectProperties, Outcome);
            },
            [&Target, &RootType](FString& OutPayload, FString& Revision, FWorkspaceError& ReadError)
            {
                UObject* Owner = nullptr;
                ContentGraphRead::FUeAgentContentGraphCapture ReadCapture;
                TSharedPtr<FJsonObject> ReadSnapshot;
                if (!TryCaptureObject(Target, RootType, Owner, ReadCapture, ReadError)
                    || !TryBuildCaptureSnapshot(Target, RootType, ReadCapture, ReadSnapshot, ReadError)
                    || !TryCanonicalizeObject(ReadSnapshot, OutPayload, ReadError)) return false;
                Revision = ReadCapture.Revision;
                return true;
            }, Payload, Error)) return false;

        // Native live effects and saved settings are separate facts. Restart metadata
        // and higher-priority CVars must not be reported as already active.
        TSharedPtr<FJsonObject> Result;
        UE::UAI::Contract::FDiagnostic ResultError;
        if (!CommandContract::Json::TryParseObject(Payload, Result, ResultError))
        {
            ResultError.SourceKind = TEXT("native_result");
            ResultError.TargetPath = Target;
            Error.SetFailure(MoveTemp(ResultError));
            return false;
        }
        TArray<TSharedPtr<FJsonValue>> Effects;
        for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
        {
            const FString* Old = Before.Find(It->GetFName());
            if (!Old || *Old == ConfigValue(*Object, **It)) continue;
            auto Effect = MakeShared<FJsonObject>();
            Effect->SetStringField(TEXT("property"), It->GetName());
            const bool bRestart = It->GetBoolMetaData(TEXT("ConfigRestartRequired"));
            Effect->SetBoolField(TEXT("restart_required"), bRestart);
            Effect->SetStringField(TEXT("status"), bRestart ? TEXT("saved_restart_required") : TEXT("native_change_notified"));
            const FString CVarName = It->GetMetaData(TEXT("ConsoleVariable"));
            if (!CVarName.IsEmpty())
            {
                Effect->SetStringField(TEXT("console_variable"), CVarName);
                Effect->SetStringField(TEXT("configured_value"), ConfigValue(*Object, **It));
                if (const IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*CVarName))
                {
                    const FString Effective = CVar->GetString();
                    Effect->SetStringField(TEXT("effective_value"), Effective);
                    const bool bEqual = MatchesConsoleValue(*Object, **It, *CVar);
                    Effect->SetStringField(TEXT("status"), bRestart ? TEXT("saved_restart_required")
                        : bEqual ? TEXT("applied_live") : TEXT("saved_live_value_differs"));
                    if ((CVar->GetFlags() & ECVF_ReadOnly) && !bEqual) Effect->SetBoolField(TEXT("restart_required"), true);
                }
            }
            Effects.Add(MakeShared<FJsonValueObject>(Effect));
        }
        Result->SetArrayField(TEXT("settings_effects"), Effects);
        if (!TryCanonicalizeObject(Result, Payload, Error)) return false;
        Response = FImportInitializationDomainResponse(MoveTemp(Payload), false);
        return true;
    }
};
}
TSharedRef<HjsonWorkspace::IImportInitializationDomainProvider> CreateSettingsHjsonProvider()
{ return MakeShared<FProvider>(); }
}
