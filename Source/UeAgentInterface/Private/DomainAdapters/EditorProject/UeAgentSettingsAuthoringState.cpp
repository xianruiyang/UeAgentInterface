#include "DomainAdapters/EditorProject/UeAgentSettingsAuthoring.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonValue.h"

#include "DomainAdapters/EditorProject/UeAgentSettingsDiscovery.h"
#include "UObject/UnrealType.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticRegistry.h"

namespace UE::UAI::DomainAdapters::EditorProject
{
namespace
{
const FString Kind = TEXT("project_settings");
const FString IdentityModel = TEXT("project_settings.authoring.identity");
const FString AccessorId = TEXT("project_settings.authoring.state");
const FString TargetSemantic = TEXT("project_settings.export.target");

UeAgentAuthoringState::EReadOutcome ReadTarget(const UeAgentAuthoringState::FReadRequest& Request,
    UeAgentAuthoringState::FStateValue& Value, UE::UAI::Contract::FDiagnostic& Error)
{
    using namespace UeAgentAuthoringState;
    if (Request.SemanticId != TargetSemantic)
    {
        Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("settings_semantic_invalid"),
            TEXT("The settings state accessor does not support the requested semantic."),
            TEXT("authoring_state"));
        Error.SourceKind = TEXT("native_result");
        Error.PropertyPath = TEXT("/semantic_supported");
        Error.ActualValue = MakeShared<FJsonValueBoolean>(false);
        Error.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
        Error.ActualValuePresent = true;
        Error.ExpectedValuePresent = true;
        Error.ActualEvidence = MakeShared<FJsonValueString>(Request.SemanticId);
        Error.ExpectedEvidence = MakeShared<FJsonValueString>(TargetSemantic);
        Error.SubjectId = Request.RootOwnerPath;
        return EReadOutcome::Failed;
    }
    Value = FStateValue();
    Value.TargetIdentity = Request.TargetIdentity;
    Value.Type.Kind = TEXT("object");
    Value.Provenance = AccessorId;
    auto Target = MakeShared<FJsonObject>();
    Target->SetStringField(TEXT("asset_path"), Request.RootOwnerPath);
    Target->SetStringField(TEXT("asset_kind"), Kind);
    Value.Value = MakeShared<FJsonValueObject>(Target);
    return EReadOutcome::Resolved;
}

bool RegisterState(UE::UAI::Contract::FDiagnostic& Error)
{
    UeAgentAuthoringIdentity::FModelDescriptor Identity;
    Identity.ModelId = IdentityModel;
    Identity.TargetKind = Kind;
    Identity.Selectors = {{TEXT("asset_path"), 100, false}};
    Identity.KeyPatterns = {{TEXT("asset"), 0, {TEXT("asset_path")}}};
    UeAgentAuthoringIdentity::ERegistrationResult IdentityResult;
    if (!UeAgentAuthoringIdentity::FRegistry::Get().RegisterModel(Identity, IdentityResult, Error)) return false;
    using namespace UeAgentAuthoringSemanticRegistry;
    FDomainDescriptor Domain;
    Domain.DomainId = TEXT("project_settings.authoring");
    Domain.TargetKinds = {Kind};
    Domain.MutationProviderId = TEXT("project_settings.authoring.apply");
    Domain.CapabilityTags = {TEXT("config"), TEXT("editor_authoring")};
    ERegistrationResult Result;
    if (!FRegistry::Get().RegisterDomain(Domain, Result, Error)) return false;
    FSemanticDescriptor Semantic;
    Semantic.SemanticId = TargetSemantic;
    Semantic.DomainId = Domain.DomainId;
    Semantic.TargetKind = Kind;
    Semantic.ValueType = TEXT("object");
    Semantic.OwnerModel = TEXT("project_settings_owner");
    Semantic.IdentityModel = IdentityModel;
    Semantic.StateAccessor = AccessorId;
    Semantic.WriteCapability = EWriteCapability::ReadOnly;
    Semantic.ConstraintSource = TEXT("project_settings.native");
    Semantic.AvailabilitySource = AccessorId;
    Semantic.Comparator = TEXT("uai.compare.canonical_value");
    Semantic.CapabilityTags = Domain.CapabilityTags;
    if (!FRegistry::Get().RegisterSemantic(Semantic, Result, Error)) return false;
    UeAgentAuthoringState::FStateAccessorDescriptor Accessor;
    Accessor.AccessorId = AccessorId;
    Accessor.TargetKinds = {Kind};
    Accessor.CapabilityTags = Domain.CapabilityTags;
    UeAgentAuthoringState::ERegistrationResult StateResult;
    return UeAgentAuthoringState::RegisterBuiltInStateAccessors(UeAgentAuthoringState::FStateAccessorRegistry::Get(), Error)
        && UeAgentAuthoringState::FStateAccessorRegistry::Get().RegisterAccessor(Accessor, &ReadTarget, StateResult, Error);
}
}

bool IsRegisteredSettingsObject(const UObject& Object)
{
    // Localization owns its structured target collection and keeps that adapter.
    if (!Object.GetClass()->HasAnyClassFlags(CLASS_Config)
        || Object.GetClass()->GetPathName() == TEXT("/Script/Localization.LocalizationSettings")) return false;
    FSettingsPage Page;
    UE::UAI::Contract::FDiagnostic Error;
    // The registered settings page owns eligibility, including Editor settings.
    // A Config class alone does not make an arbitrary UObject a settings target.
    return FindSettingsPage(Object.GetPathName(), Page, Error);
}

bool BuildSettingsReadRequests(const UObject& Object,
    TArray<UeAgentAuthoringState::FReadRequest>& Requests, UE::UAI::Contract::FDiagnostic& Error)
{
    Requests.Reset();
    if (!RegisterState(Error)) return false;
    UeAgentAuthoringState::FReadRequest Request;
    Request.SemanticId = TargetSemantic;
    Request.MarkExportTarget();
    Request.RootOwnerPath = Object.GetPathName();
    if (!UeAgentAuthoringIdentity::BuildCanonicalIdentity(IdentityModel, Kind,
        {{TEXT("asset_path"), Request.RootOwnerPath}}, Request.TargetIdentity, Error)) return false;
    Request.TargetQuery.ModelId = Request.TargetIdentity.ModelId;
    Request.TargetQuery.TargetKind = Request.TargetIdentity.TargetKind;
    Request.TargetQuery.CanonicalKey = Request.TargetIdentity.CanonicalKey;
    Requests.Add(MoveTemp(Request));
    UeAgentAuthoringState::FEditablePropertyReadOptions Options;
    Options.SemanticId = TEXT("core.property");
    Options.IncludeProperty = [](const FProperty* Property, const void*, const UObject*) { return Property && Property->HasAnyPropertyFlags(CPF_Config); };
    return UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(&Object, Options, Requests, Error);
}
}
