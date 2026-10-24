#include "Misc/AutomationTest.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "Utilities/UeAgentUObjectProperties.h"
#include "Materials/MaterialParameterCollection.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUaiPropertyLifecycleTest,
    "UeAgentInterface.Common.PropertyLifecycle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUaiPropertyLifecycleTest::RunTest(const FString& Parameters)
{
    using namespace UeAgentAuthoringObjectApply;
    TStrongObjectPtr<UMaterialParameterCollection> Root(NewObject<UMaterialParameterCollection>(
        GetTransientPackage(), NAME_None, RF_Transient));
    auto& Original = Root->ScalarParameters.AddDefaulted_GetRef();
    Original.ParameterName = TEXT("Original");
    Original.DefaultValue = 1.0f;
    const FGuid OriginalId = Original.Id;

    auto Descriptor = [](const FString& Path, const TSharedPtr<FJsonValue>& Value) {
        auto Object = MakeShared<FJsonObject>();
        Object->SetStringField(TEXT("property_path"), Path);
        Object->SetField(TEXT("value"), Value);
        return MakeShared<FJsonValueObject>(Object);
    };
    FOptions Options;
    Options.bCompactValues = true;
    Options.bEmitEditorNotifications = false;
    Options.bMarkPackageDirty = false;
    Options.bRecordOwnerTransaction = false;

    TArray<TSharedPtr<FJsonValue>> Items;
    for (int32 Index = 0; Index < 64; ++Index)
    {
        auto Fields = MakeShared<FJsonObject>();
        Fields->SetStringField(TEXT("ParameterName"), Index == 0 ? TEXT("Original") : FString::Printf(TEXT("Added%d"), Index));
        Fields->SetNumberField(TEXT("DefaultValue"), Index == 0 ? 12.5 : Index);
        auto Item = MakeShared<FJsonObject>();
        Item->SetObjectField(TEXT("fields"), Fields);
        Items.Add(MakeShared<FJsonValueObject>(Item));
    }
    const TArray<TSharedPtr<FJsonValue>> Edits = {
        Descriptor(TEXT("ScalarParameters"), MakeShared<FJsonValueArray>(Items)),
        Descriptor(TEXT("ScalarParameters[0].DefaultValue"), MakeShared<FJsonValueNumber>(12.5))};
    FPreparedApply Plan;
    FFailure Failure;
    if (!TestTrue(TEXT("prepare root array and child together"), PrepareObject(Root.Get(), Edits, Options, Plan, Failure)))
    { AddError(Failure.Diagnostic.Code + TEXT(":") + Failure.Diagnostic.PublicMessage); return false; }
    FApplyResult Result;
    if (!TestTrue(TEXT("write child into reallocated current array"), ApplyPrepared(Plan, Result)))
    { AddError(Result.Diagnostic.Code + TEXT(":") + Result.Diagnostic.PublicMessage); return false; }
    if (!TestEqual(TEXT("growth applied"), Root->ScalarParameters.Num(), 64)) return false;
    TestEqual(TEXT("child intent applied"), Root->ScalarParameters[0].DefaultValue, 12.5f);
    TestTrue(TEXT("unchanged native identity preserved"), Root->ScalarParameters[0].Id == OriginalId);
    TestTrue(TEXT("independent prepared intent readback"), VerifyPrepared(Plan, Result));
    Root->ScalarParameters[0].DefaultValue = 77.0f;
    TestFalse(TEXT("late native value loss is rejected"), VerifyPrepared(Plan, Result));
    if (!TestTrue(TEXT("reverse rollback resolves current targets"), RollbackPrepared(Plan, Result))) return false;
    if (!TestEqual(TEXT("original collection restored"), Root->ScalarParameters.Num(), 1)) return false;
    TestEqual(TEXT("original value restored"), Root->ScalarParameters[0].DefaultValue, 1.0f);
    TestTrue(TEXT("original identity restored"), Root->ScalarParameters[0].Id == OriginalId);

    FPreparedApply Missing;
    const TArray<TSharedPtr<FJsonValue>> Removal = {
        Descriptor(TEXT("ScalarParameters"), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{})),
        Descriptor(TEXT("ScalarParameters[0].DefaultValue"), MakeShared<FJsonValueNumber>(13.0))};
    if (!TestTrue(TEXT("prepare existing child before removal"), PrepareObject(Root.Get(), Removal, Options, Missing, Failure)))
    { AddError(Failure.Diagnostic.Code); return false; }
    TestFalse(TEXT("removed target fails before stale write"), ApplyPrepared(Missing, Result));
    TestEqual(TEXT("missing target diagnosed"), Result.Diagnostic.PropertyPath, FString(TEXT("ScalarParameters[0].DefaultValue")));
    TestEqual(TEXT("refresh failure owns its stage"), Result.Diagnostic.Stage, FString(TEXT("property_target_refresh")));
    if (!TestTrue(TEXT("failed batch restores removed collection"), RollbackPrepared(Missing, Result))) return false;
    if (!TestEqual(TEXT("removed collection rollback count"), Root->ScalarParameters.Num(), 1)) return false;

    // Final verification rebinds immutable input intent after native owner replacement.
    FPreparedApply ScopedPlan;
    const TArray<TSharedPtr<FJsonValue>> ScopedEdits = {
        Descriptor(TEXT("ScalarParameters[0].DefaultValue"), MakeShared<FJsonValueNumber>(7.5))};
    if (!TestTrue(TEXT("prepare independent scoped input"), PrepareObject(Root.Get(), ScopedEdits, Options, ScopedPlan, Failure))) return false;
    if (!TestTrue(TEXT("apply before resolving scoped identities"), ApplyPrepared(ScopedPlan, Result))) return false;
    Root->ScalarParameters[0].DefaultValue = 99.0f;
    FPropertyIntent ScopedIntent;
    if (!TestTrue(TEXT("capture decoded input instead of later native value"), ScopedPlan.CaptureIntent(ScopedIntent, Failure))) return false;
    if (!TestTrue(TEXT("restore fixture after independent capture"), RollbackPrepared(ScopedPlan, Result))) return false;
    TArray<UeAgentDetailsAuthoringService::FScopedPropertyIntent> ScopedIntents;
    UeAgentDetailsAuthoringService::AppendScopedObjectPropertyIntents({ScopedIntent},
        TEXT("test.property"), TEXT("node_id"), TEXT("created"), ScopedIntents,
        {{TEXT("graph_name"), TEXT("main")}});
    if (!TestEqual(TEXT("scoped verification retains the original input"), ScopedIntents.Num(), 1)) return false;
    TStrongObjectPtr<UMaterialParameterCollection> Rebound(NewObject<UMaterialParameterCollection>(
        GetTransientPackage(), NAME_None, RF_Transient));
    Rebound->ScalarParameters = Root->ScalarParameters;
    Rebound->ScalarParameters[0].DefaultValue = 7.5f;
    UeAgentAuthoringState::FReadRequest ReboundRequest;
    ReboundRequest.SemanticId = TEXT("test.property");
    ReboundRequest.RootObject = Rebound.Get();
    ReboundRequest.SnapshotIdentityOverride.Emplace();
    ReboundRequest.SnapshotIdentityOverride->Selectors.Add(TEXT("node_id"), TEXT("created"));
    ReboundRequest.SnapshotIdentityOverride->Selectors.Add(TEXT("graph_name"), TEXT("main"));
    ReboundRequest.SnapshotIdentityOverride->Selectors.Add(TEXT("property_path"), TEXT("ScalarParameters"));
    FString ScopedError;
    TestTrue(TEXT("scoped input survives owner replacement"),
        UeAgentDetailsAuthoringService::VerifyScopedPropertyIntents(ScopedIntents, {ReboundRequest}, ScopedError));
    auto OtherGraph = ReboundRequest;
    OtherGraph.RootObject = Root.Get();
    OtherGraph.SnapshotIdentityOverride->Selectors.Add(TEXT("graph_name"), TEXT("other"));
    TestTrue(TEXT("same local id in another graph cannot replace the intended owner"),
        UeAgentDetailsAuthoringService::VerifyScopedPropertyIntents(ScopedIntents, {OtherGraph, ReboundRequest}, ScopedError));
    Rebound->ScalarParameters[0].DefaultValue = 5.0f;
    TestFalse(TEXT("final default cannot hide omitted input"),
        UeAgentDetailsAuthoringService::VerifyScopedPropertyIntents(ScopedIntents, {ReboundRequest}, ScopedError));
    TestFalse(TEXT("missing current owner cannot silently pass"),
        UeAgentDetailsAuthoringService::VerifyScopedPropertyIntents(ScopedIntents, {}, ScopedError));

    auto InvalidValue = MakeShared<FJsonObject>();
    InvalidValue->SetStringField(TEXT("literal"), TEXT("invalid-number"));
    auto Invalid = Descriptor(TEXT("ScalarParameters[0].DefaultValue"), MakeShared<FJsonValueObject>(InvalidValue));
    auto Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("schema"), TEXT("uai.uobject_properties.v2"));
    Payload->SetArrayField(TEXT("properties"), {Invalid});
    UeAgentUObjectProperties::FApplySummaryResult Parsed;
    TestFalse(TEXT("object apply reports failure"), UeAgentUObjectProperties::ApplyObjectProperties(Root.Get(), Payload, {}, Parsed));
    auto Diagnostic = Parsed.Diagnostic;
    TestTrue(TEXT("object apply keeps typed diagnostic"), Diagnostic.IsValid());
    TestEqual(TEXT("object apply preserves failed path"), Diagnostic.PropertyPath, FString(TEXT("ScalarParameters[0].DefaultValue")));
    TestEqual(TEXT("failed decode leaves original value"), Root->ScalarParameters[0].DefaultValue, 1.0f);
    Payload->SetArrayField(TEXT("properties"), {Descriptor(TEXT("DefaultValue"), MakeShared<FJsonValueObject>(InvalidValue))});
    auto* ScalarType = CastFieldChecked<FStructProperty>(FindFProperty<FArrayProperty>(
        Root->GetClass(), TEXT("ScalarParameters"))->Inner)->Struct.Get();
    TestFalse(TEXT("struct apply reports failure"), UeAgentUObjectProperties::ApplyStructProperties(ScalarType,
        &Root->ScalarParameters[0], Root.Get(), Payload, {}, Parsed));
    TestTrue(TEXT("struct apply keeps typed diagnostic"), Parsed.Diagnostic.IsValid());

    Diagnostic.Code = TEXT("value_mismatch");
    Diagnostic.Stage = TEXT("property_intent_verify");
    Diagnostic.Source.SourceFile = TEXT("properties.hjson");
    Diagnostic.Source.Line = 17;
    Diagnostic.ExpectedValue = MakeShared<FJsonValueNumber>(1);
    Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(2);
    const auto Plain = UE::UAI::Contract::FDiagnostic::Failure(TEXT("expected_number"),
        TEXT("A number is required."), TEXT("input_decode"), TEXT("ordinary native detail"));
    {
        auto Document = MakeShared<FJsonObject>();
        auto SourceItems = MakeShared<FJsonObject>();
        auto Item = MakeShared<FJsonObject>();
        SourceItems->SetObjectField(TEXT("layer~/2"), Item);
        Document->SetObjectField(TEXT("items"), SourceItems);
        UeAgentAuthoringDiagnostics::FScopedSourceLocations Sources(&Document.Get(), [](const FString& Pointer) {
            UeAgentAuthoringDiagnostics::FSourceLocation Source;
            Source.SourceFile = TEXT("inline://diagnostic-fixture");
            Source.JsonPath = Pointer; Source.Line = 7; Source.Column = 4;
            return Source;
        });
        UeAgentAuthoringDiagnostics::FDiagnostic Located;
        UeAgentAuthoringDiagnostics::LocateFailure(Located, *Item, TEXT("preflight"), TEXT("asset"), TEXT("target"));
        TestFalse(TEXT("empty provider detail still forms a valid public error"), Located.IsEmpty());
        TestEqual(TEXT("missing diagnostic has an explicit fallback code"), Located.Code, FString(TEXT("authoring_diagnostic_missing")));
        TestEqual(TEXT("raw inline source binds exact escaped object and missing field"),
            Located.Source.JsonPath, FString(TEXT("/items/layer~0~12/target")));
        TestFalse(TEXT("source binding never injects metadata into editable business values"), Item->HasField(TEXT("authoring_source")));
        auto Aggregate = UE::UAI::Contract::FDiagnostic::Failure(TEXT("invalid_value"), TEXT("Wrong value."), TEXT("preflight"));
        Aggregate.Diagnostics.Add(Located);
        Aggregate.Diagnostics.Add(Plain);
        UeAgentAuthoringDiagnostics::LocateAuthoringSource(Aggregate, *Item, TEXT("value"));
        TestEqual(TEXT("specific child location is retained"), Aggregate.Diagnostics[0].Source.JsonPath, Located.Source.JsonPath);
        TestEqual(TEXT("unlocated child receives the current input source"), Aggregate.Diagnostics[1].Source.JsonPath, FString(TEXT("/items/layer~0~12/value")));
    }
    const auto Forwarded = Diagnostic;
    TestEqual(TEXT("source survives generic handler"), Forwarded.Source.SourceFile, FString(TEXT("properties.hjson")));
    if (TestTrue(TEXT("actual fact remains present"), Forwarded.ActualValue.IsValid()))
        TestEqual(TEXT("actual value survives generic handler"), Forwarded.ActualValue->AsNumber(), 2.0);
    AddExpectedError(TEXT("UAI workspace operation:"), EAutomationExpectedErrorFlags::Contains, 3);
    UE::UAI::HjsonWorkspace::FWorkspaceError WorkspaceError;
    auto Candidate = UE::UAI::Contract::FDiagnostic::Failure(TEXT("candidate_ambiguous"), TEXT("Choose one of the matching candidates."), TEXT("candidate_lookup"));
    Candidate.CandidateName = TEXT("Pulse");
    auto Expression = MakeShared<FJsonObject>(); Expression->SetStringField(TEXT("form"), TEXT("expression"));
    auto Function = MakeShared<FJsonObject>(); Function->SetStringField(TEXT("form"), TEXT("function"));
    Candidate.CandidateChoices = MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
        MakeShared<FJsonValueObject>(Expression), MakeShared<FJsonValueObject>(Function)});
    Candidate.RecoveryAction = TEXT("query candidates");
    WorkspaceError.SetFailure(Candidate, TEXT("graph.hjson"), TEXT("/nodes/item/candidate"));
    TestEqual(TEXT("caller candidate location survives missing external source"),
        WorkspaceError.GetJsonPointer(), FString(TEXT("/nodes/item/candidate")));
    const TSharedPtr<FJsonObject> CandidatePublic = WorkspaceError.GetDiagnostic().ToPublicJson();
    if (TestTrue(TEXT("workspace emits public candidate carrier"), CandidatePublic.IsValid()))
    {
        TestEqual(TEXT("candidate name survives workspace"), CandidatePublic->GetStringField(TEXT("name")), FString(TEXT("Pulse")));
        const TArray<TSharedPtr<FJsonValue>>* Choices = nullptr;
        if (TestTrue(TEXT("candidate choices remain present"), CandidatePublic->TryGetArrayField(TEXT("choose"), Choices)))
            TestEqual(TEXT("candidate choices survive workspace"), Choices->Num(), 2);
        TestEqual(TEXT("lookup recovery survives workspace"), CandidatePublic->GetStringField(TEXT("recovery_action")), FString(TEXT("query candidates")));
        TestEqual(TEXT("caller source stays readable"), CandidatePublic->GetStringField(TEXT("source_file")), FString(TEXT("graph.hjson")));
    }
    Diagnostic.Source.JsonPath = TEXT("/original/property");
    WorkspaceError.SetFailure(Diagnostic, TEXT("fallback.hjson"), TEXT("/candidate"));
    TestEqual(TEXT("specific upstream source takes precedence over fallback"), WorkspaceError.GetSourceUnit(), FString(TEXT("properties.hjson")));
    TestEqual(TEXT("specific upstream pointer takes precedence over fallback"), WorkspaceError.GetJsonPointer(), FString(TEXT("/original/property")));
    UE::UAI::HjsonWorkspace::FWorkspaceError SelectorError;
    SelectorError.Set(TEXT("domain_hjson_candidate_selector_invalid"), FString(), TEXT("/nodes/item/candidate/name"));
    const auto& SelectorDiagnostic = SelectorError.GetDiagnostic();
    TestEqual(TEXT("unbound selector keeps the exact semantic field"),
        SelectorDiagnostic.SemanticPointer, FString(TEXT("/nodes/item/candidate/name")));
    TestTrue(TEXT("unbound selector does not invent a source file"), SelectorDiagnostic.Source.SourceFile.IsEmpty());
    auto RecoveryDiagnostic = SelectorError.GetDiagnostic();
    RecoveryDiagnostic.CauseCode = RecoveryDiagnostic.Code;
    RecoveryDiagnostic.Code = TEXT("asset_state_unknown");
    RecoveryDiagnostic.PublicMessage = TEXT("The asset state could not be confirmed after rollback.");
    UeAgentAuthoringDiagnostics::SetRollbackFacts(RecoveryDiagnostic, false);
    TestEqual(TEXT("rollback failure retains its terminal code"), RecoveryDiagnostic.Code, FString(TEXT("asset_state_unknown")));
    TestEqual(TEXT("rollback failure retains original input cause"), RecoveryDiagnostic.CauseCode, SelectorError.GetCode());
    TestEqual(TEXT("rollback facts retain input location"), RecoveryDiagnostic.SemanticPointer, SelectorDiagnostic.SemanticPointer);
    TestTrue(TEXT("rollback attempt is explicit"), RecoveryDiagnostic.RollbackAttempted.IsSet() && RecoveryDiagnostic.RollbackAttempted.GetValue());
    TestTrue(TEXT("unconfirmed rollback is explicit"), RecoveryDiagnostic.RollbackSucceeded.IsSet() && !RecoveryDiagnostic.RollbackSucceeded.GetValue());
    TestFalse(TEXT("unconfirmed rollback is not safe to retry"), RecoveryDiagnostic.bRetrySafe);
    TestEqual(TEXT("shared pointer escaping preserves special business keys"),
        UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(TEXT("part~/leaf")), FString(TEXT("part~0~1leaf")));
    WorkspaceError.SetFailure(SelectorError.GetDiagnostic(), TEXT("graph.hjson"), TEXT("/nodes/item/candidate"));
    TestEqual(TEXT("semantic carrier restores precise location before source binding"),
        WorkspaceError.GetJsonPointer(), FString(TEXT("/nodes/item/candidate/name")));
    SelectorError.Set(TEXT("internal_failure"), TEXT(".uai/manifest.hjson"), TEXT("/private/identity"));
    const TSharedPtr<FJsonObject> PrivateWorkspace = SelectorError.GetDiagnostic().ToPublicJson();
    if (TestTrue(TEXT("private workspace carrier projects"), PrivateWorkspace.IsValid()))
        TestFalse(TEXT("private workspace pointer remains hidden"), PrivateWorkspace->HasField(TEXT("semantic_pointer")));
    return true;
}
