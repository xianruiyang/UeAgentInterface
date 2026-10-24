#include "Misc/AutomationTest.h"
#include "Utilities/AuthoringObjectApply/UeAgentObjectApplyNotificationTestTypes.h"
#include "Composition/HjsonImport/UeAgentReflectedObjectRecord.h"
#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceResolver.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentReadOnlyHashIdentityTest,
    "UAI.Core.Authoring.ReadOnlyHashIdentityRoundTrip", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentReadOnlyHashIdentityTest::RunTest(const FString&)
{
    using namespace UE::UAI::Composition::HjsonImport;
    using namespace UeAgentAuthoringObjectApply;
    TStrongObjectPtr<UUeAgentReadOnlyHashIdentityTarget> Target(NewObject<UUeAgentReadOnlyHashIdentityTarget>());
    auto Key = [](int32 Identity, int32 Payload) { FUeAgentReadOnlyHashIdentity Value; Value.Identity = Identity; Value.Payload = Payload; return Value; };
    Target->Records.Add(Key(811, 10)); Target->Records.Add(Key(922, 20));
    Target->Lookup.Add(Key(811, 10), 101); Target->Lookup.Add(Key(922, 20), 202);
    Target->Names = {TEXT("Alpha"), TEXT("Beta")};
    const auto BeforeSet = Target->Records;
    const auto BeforeMap = Target->Lookup;
    auto SameNative = [&](const TSet<FUeAgentReadOnlyHashIdentity>& ExpectedSet, const TMap<FUeAgentReadOnlyHashIdentity, int32>& ExpectedMap) {
        bool Same = TestEqual(TEXT("full native Set count"), Target->Records.Num(), ExpectedSet.Num());
        Same &= TestEqual(TEXT("full native Map count"), Target->Lookup.Num(), ExpectedMap.Num());
        for (const auto& Item : ExpectedSet) {
            const auto* Actual = Target->Records.Find(Item);
            if (!TestNotNull(*FString::Printf(TEXT("native Set identity %d retained"), Item.Identity), Actual)) { Same = false; continue; }
            Same &= TestEqual(TEXT("native Set payload"), Actual->Payload, Item.Payload);
        }
        for (const auto& Pair : ExpectedMap) {
            const auto* ActualValue = Target->Lookup.Find(Pair.Key);
            if (!TestNotNull(*FString::Printf(TEXT("native Map identity %d retained"), Pair.Key.Identity), ActualValue)) { Same = false; continue; }
            Same &= TestEqual(TEXT("native mapped value"), *ActualValue, Pair.Value);
            for (const auto& ActualPair : Target->Lookup)
                if (ActualPair.Key.Identity == Pair.Key.Identity) Same &= TestEqual(TEXT("native Map key payload"), ActualPair.Key.Payload, Pair.Key.Payload);
        }
        return Same;
    };
    UeAgentDetailsAuthoringService::FCapturePolicy Policy;
    TSharedPtr<FJsonObject> Properties, Definitions;
    FString Error;
    if (!CaptureReflectedObjectRecord(*Target.Get(), Policy, Properties, Definitions, Error)) { AddError(Error); return false; }
    FPreparedApply NoOp;
    const bool NoOpPrepared = PrepareReflectedObjectRecord(*Target.Get(), Policy, Properties, NoOp, Error);
    TestTrue(TEXT("ordinary full export prepares"), NoOpPrepared);
    if (NoOpPrepared) TestEqual(TEXT("ordinary unchanged export is no-op"), NoOp.NumProperties(), 0);
    else AddError(TEXT("no-op Prepare: ") + Error);
    SameNative(BeforeSet, BeforeMap);

    auto RunStage = [&](const FString& Stage, bool IsSet, bool Edit, bool SingleSurvivor) {
        AddInfo(TEXT("READONLY_HASH_STAGE ") + Stage);
        if (!CaptureReflectedObjectRecord(*Target.Get(), Policy, Properties, Definitions, Error)) { AddError(Stage + TEXT(" Capture: ") + Error); return; }
        const auto Category = Properties->GetObjectField(TEXT("Test"));
        Category->RemoveField(IsSet ? TEXT("Lookup") : TEXT("Records"));
        const auto Row = Category->GetObjectField(IsSet ? TEXT("Records") : TEXT("Lookup"));
        const TArray<TSharedPtr<FJsonValue>> Items = IsSet ? Row->GetArrayField(TEXT("value")) : Row->GetObjectField(TEXT("value"))->GetArrayField(TEXT("entries"));
        TMap<int32, TSharedPtr<FJsonValue>> ByPayload;
        for (const auto& Item : Items) {
            const auto Fields = (IsSet ? Item->AsObject() : Item->AsObject()->GetObjectField(TEXT("key")))->GetObjectField(TEXT("fields"));
            ByPayload.Add(static_cast<int32>(Fields->GetNumberField(TEXT("Payload"))), Item);
            TestTrue(TEXT("qualified hash value carries from facts"), Item->AsObject()->HasTypedField<EJson::Object>(TEXT("from")));
            AddInfo(FString::Printf(TEXT("%s ordinary identity field present=%s"), *Stage, Fields->HasField(TEXT("Identity")) ? TEXT("true") : TEXT("false")));
        }
        if (!TestTrue(TEXT("ordinary fixture has both editable payloads"), ByPayload.Contains(10) && ByPayload.Contains(20))) return;
        auto ExpectedSet = BeforeSet; auto ExpectedMap = BeforeMap;
        if (Edit) {
            if (IsSet) {
                ByPayload.FindChecked(10)->AsObject()->GetObjectField(TEXT("fields"))->SetNumberField(TEXT("Payload"), 57);
                ExpectedSet.Find(Key(811, 0))->Payload = 57;
            } else {
                ByPayload.FindChecked(10)->AsObject()->SetNumberField(TEXT("value"), 303);
                ExpectedMap.FindChecked(Key(811, 0)) = 303;
            }
        }
        TArray<TSharedPtr<FJsonValue>> DesiredItems;
        if (!SingleSurvivor) DesiredItems.Add(ByPayload.FindChecked(20));
        DesiredItems.Add(ByPayload.FindChecked(10));
        if (IsSet) {
            Row->SetArrayField(TEXT("value"), DesiredItems);
            if (SingleSurvivor) ExpectedSet.Remove(Key(922, 0));
        } else {
            Row->GetObjectField(TEXT("value"))->SetArrayField(TEXT("entries"), DesiredItems);
            if (SingleSurvivor) ExpectedMap.Remove(Key(922, 0));
        }
        FPreparedApply Prepared; FApplyResult Result;
        Error.Reset();
        const bool PreparedOk = PrepareReflectedObjectRecord(*Target.Get(), Policy, Properties, Prepared, Error);
        AddInfo(Stage + TEXT(" Prepare=") + (PreparedOk ? TEXT("true") : TEXT("false")) + TEXT(" error=") + Error);
        TestTrue(*(Stage + TEXT(" Prepare accepts legal ordinary edit")), PreparedOk);
        SameNative(BeforeSet, BeforeMap);
        if (!PreparedOk) return;
        const bool Applied = ApplyPrepared(Prepared, Result);
        TestTrue(*(Stage + TEXT(" Apply")), Applied);
        if (Applied) {
            if (IsSet) for (const auto& Item : Target->Records)
                AddInfo(FString::Printf(TEXT("%s actual identity=%d payload=%d"), *Stage, Item.Identity, Item.Payload));
            else for (const auto& Pair : Target->Lookup)
                AddInfo(FString::Printf(TEXT("%s actual identity=%d key_payload=%d mapped=%d"), *Stage, Pair.Key.Identity, Pair.Key.Payload, Pair.Value));
            SameNative(ExpectedSet, ExpectedMap);
            const bool Verified = VerifyPrepared(Prepared, Result);
            AddInfo(Stage + TEXT(" Verify=") + (Verified ? TEXT("true") : TEXT("false")));
            TestTrue(*(Stage + TEXT(" Verify")), Verified);
        }
        TestTrue(*(Stage + TEXT(" Rollback")), RollbackPrepared(Prepared, Result));
        SameNative(BeforeSet, BeforeMap);
    };
    RunStage(TEXT("Set.Reorder"), true, false, false);
    RunStage(TEXT("Set.PayloadEdit"), true, true, false);
    RunStage(TEXT("Map.Reorder"), false, false, false);
    RunStage(TEXT("Map.ValueEdit"), false, true, false);
    RunStage(TEXT("Set.OneSurvivorEdit"), true, true, true);
    RunStage(TEXT("Map.OneSurvivorEdit"), false, true, true);
    auto RejectSource = [&](bool IsSet, const FString& ExpectedCode, TFunction<void(TSharedPtr<FJsonObject>)> Change) {
        if (!CaptureReflectedObjectRecord(*Target.Get(), Policy, Properties, Definitions, Error)) { AddError(Error); return; }
        auto Category = Properties->GetObjectField(TEXT("Test")); Category->RemoveField(IsSet ? TEXT("Lookup") : TEXT("Records"));
        auto Row = Category->GetObjectField(IsSet ? TEXT("Records") : TEXT("Lookup"));
        auto Items = IsSet ? Row->GetArrayField(TEXT("value")) : Row->GetObjectField(TEXT("value"))->GetArrayField(TEXT("entries"));
        Change(Items[0]->AsObject());
        FPreparedApply Rejected;
        TestFalse(*ExpectedCode, PrepareReflectedObjectRecord(*Target.Get(), Policy, Properties, Rejected, Error));
        TestTrue(TEXT("source rejection is explicit"), Error.Contains(ExpectedCode));
        SameNative(BeforeSet, BeforeMap);
    };
    for (bool IsSet : {true, false}) {
        RejectSource(IsSet, TEXT("hash_source_required"), [](auto Item) { Item->RemoveField(TEXT("from")); });
        RejectSource(IsSet, TEXT("hash_source_not_found"), [](auto Item) {
            Item->GetObjectField(TEXT("from"))->GetObjectField(TEXT("fields"))->GetObjectField(TEXT("Identity"))->SetNumberField(TEXT("literal"), 999);
        });
        RejectSource(IsSet, TEXT("hash_source_facts_mismatch"), [](auto Item) {
            Item->GetObjectField(TEXT("from"))->GetObjectField(TEXT("fields"))->GetObjectField(TEXT("Payload"))->SetNumberField(TEXT("literal"), 999);
        });
        RejectSource(IsSet, TEXT("read_only_or_non_editable"), [IsSet](auto Item) {
            (IsSet ? Item : Item->GetObjectField(TEXT("key")))->GetObjectField(TEXT("fields"))->SetNumberField(TEXT("Identity"), 999);
        });
    }
    for (bool IsSet : {true, false}) {
        if (!CaptureReflectedObjectRecord(*Target.Get(), Policy, Properties, Definitions, Error)) { AddError(Error); return false; }
        auto Category = Properties->GetObjectField(TEXT("Test")); Category->RemoveField(IsSet ? TEXT("Lookup") : TEXT("Records"));
        auto Row = Category->GetObjectField(IsSet ? TEXT("Records") : TEXT("Lookup"));
        auto Items = IsSet ? Row->GetArrayField(TEXT("value")) : Row->GetObjectField(TEXT("value"))->GetArrayField(TEXT("entries"));
        auto NewFields = MakeShared<FJsonObject>(); NewFields->SetNumberField(TEXT("Payload"), 68);
        auto NewKey = MakeShared<FJsonObject>(); NewKey->SetObjectField(TEXT("fields"), NewFields);
        auto NewItem = IsSet ? NewKey : MakeShared<FJsonObject>();
        if (!IsSet) { NewItem->SetObjectField(TEXT("key"), NewKey); NewItem->SetNumberField(TEXT("value"), 404); }
        NewItem->SetField(TEXT("from"), MakeShared<FJsonValueNull>());
        Items.Add(MakeShared<FJsonValueObject>(NewItem));
        auto SetItems = [&]() { if (IsSet) Row->SetArrayField(TEXT("value"), Items); else Row->GetObjectField(TEXT("value"))->SetArrayField(TEXT("entries"), Items); };
        SetItems();
        FPreparedApply Created; FApplyResult Result;
        if (!PrepareReflectedObjectRecord(*Target.Get(), Policy, Properties, Created, Error)) { AddError(Error); return false; }
        SameNative(BeforeSet, BeforeMap);
        if (!ApplyPrepared(Created, Result)) { AddError(Result.Diagnostic.Code); return false; }
        auto ExpectedSet = BeforeSet; auto ExpectedMap = BeforeMap;
        if (IsSet) ExpectedSet.Add(Key(7, 68)); else ExpectedMap.Add(Key(7, 68), 404);
        SameNative(ExpectedSet, ExpectedMap);
        TestTrue(TEXT("explicit native-default creation verifies"), VerifyPrepared(Created, Result));
        TestTrue(TEXT("explicit creation rolls back"), RollbackPrepared(Created, Result));
        SameNative(BeforeSet, BeforeMap);
        Items.Add(MakeShared<FJsonValueObject>(NewItem)); SetItems();
        FPreparedApply Duplicate;
        TestFalse(TEXT("duplicate native default identity rejects"), PrepareReflectedObjectRecord(*Target.Get(), Policy, Properties, Duplicate, Error));
        TestTrue(TEXT("existing duplicate policy remains"), Error.Contains(IsSet ? TEXT("duplicate_set_item") : TEXT("duplicate_map_key")));
        SameNative(BeforeSet, BeforeMap);
    }
    if (!CaptureReflectedObjectRecord(*Target.Get(), Policy, Properties, Definitions, Error)) { AddError(Error); return false; }
    auto Names = Properties->GetObjectField(TEXT("Test"))->GetObjectField(TEXT("Names"));
    auto NameValues = Names->GetArrayField(TEXT("value"));
    for (const auto& Value : NameValues) TestTrue(TEXT("complete scalar Set keeps its string representation"), Value->Type == EJson::String);
    NameValues.Add(MakeShared<FJsonValueString>(TEXT("New"))); Names->SetArrayField(TEXT("value"), NameValues);
    FPreparedApply Scalar; FApplyResult ScalarResult;
    if (!PrepareReflectedObjectRecord(*Target.Get(), Policy, Properties, Scalar, Error)) { AddError(Error); return false; }
    TestTrue(TEXT("ordinary scalar creation applies without from"), ApplyPrepared(Scalar, ScalarResult));
    TestTrue(TEXT("ordinary scalar new value exists"), Target->Names.Contains(TEXT("New")) && Target->Names.Num() == 3);
    TestTrue(TEXT("ordinary scalar result verifies"), VerifyPrepared(Scalar, ScalarResult));
    SameNative(BeforeSet, BeforeMap);
    TestTrue(TEXT("ordinary scalar rollback"), RollbackPrepared(Scalar, ScalarResult));
    TestTrue(TEXT("ordinary scalar native values restored"), !Target->Names.Contains(TEXT("New")) && Target->Names.Num() == 2);
    // The same native capture must survive persisted workspace projection and
    // the existing revision gate; no element registry or alternate importer.
    using namespace UE::UAI::HjsonWorkspace;
    auto Resources = MakeSemanticWorkspaceResourceTracker();
    UeAgentAuthoringState::FStateSnapshot NativeSnapshot;
    TSharedPtr<FJsonObject> CaptureDocument;
    UE::UAI::StateSemantic::FUeAgentCanonicalSnapshotPtr Canonical;
    FAuthoringModelPtr Model;
    FWorkspaceError WorkspaceError;
    auto CaptureWorkspace = [&]() {
        return UeAgentDetailsAuthoringService::CaptureObjectProperties(Target.Get(), Policy, NativeSnapshot, CaptureDocument, Error)
            && TryBuildCanonicalAuthoringCapture(TEXT("ue.generic.object"), TEXT("llm_authoring"), NativeSnapshot,
                Resources, Canonical, Model, WorkspaceError);
    };
    if (!CaptureWorkspace()) { AddError(Error + WorkspaceError.GetCode()); return false; }
    FExportWorkspacePlanPtr Plan;
    if (!TryBuildCompositionExportPlan(Model.ToSharedRef(),
        {Canonical->GetSnapshotId(), Canonical->GetRevision(), TEXT("readonly-hash-source-test")}, {}, Resources, Plan, WorkspaceError))
    { AddError(WorkspaceError.GetCode()); return false; }
    const FString WorkspaceRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UAI/Tests/HashSource"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
    for (const auto& File : Plan->GetFiles()) {
        const FString Path = FPaths::Combine(WorkspaceRoot, File.GetRelativePath());
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
        if (!FFileHelper::SaveArrayToFile(File.GetCanonicalUtf8(), *Path)) { AddError(TEXT("workspace write failed")); return false; }
    }
    FResolvedSemanticWorkspacePtr Resolved;
    if (!TryResolveEditedSemanticWorkspace(FWorkspaceSource::Directory(WorkspaceRoot), ESemanticWorkspaceMode::State,
        Resources, Resolved, WorkspaceError)) { AddError(WorkspaceError.GetCode() + TEXT(":") + WorkspaceError.GetSourceUnit()); return false; }
    TArray<TSharedPtr<FJsonValue>> Changes;
    FSemanticWorkspaceImportError ImportError;
    auto PrepareWorkspace = [&]() {
        return TryPrepareRootObjectPropertyChanges(Resolved.ToSharedRef(), Model.ToSharedRef(), Canonical.ToSharedRef(),
            UE::UAI::Contract::FUeAgentDescriptorIdentity(TEXT("uai.test.hash-source"), 1,
                TEXT("sha256:5555555555555555555555555555555555555555555555555555555555555555")),
            TEXT("hash-source-workspace"), TEXT("hash-source-workspace:1"),
            UE::UAI::Contract::FUeAgentCallerContext(TEXT("hash-source-test"),
                {TEXT("uai:query"), TEXT("uai:operate"), TEXT("authoring.write")}, TEXT("hash-source-workspace")),
            Target->GetPathName(), Resources, Changes, ImportError);
    };
    TestFalse(TEXT("unchanged workspace has no explicit changes"), PrepareWorkspace());
    TestEqual(TEXT("ordinary workspace reports its established no-op result"), ImportError.Code, FString(TEXT("hjson_import_no_explicit_changes")));
    TestEqual(TEXT("persisted hash-source workspace is no-op"), Changes.Num(), 0);
    SameNative(BeforeSet, BeforeMap);
    int32 WorkspaceEdits = 0;
    for (const auto& File : Plan->GetFiles()) {
        if (File.GetRelativePath().StartsWith(TEXT(".uai/"))) continue;
        const FString Path = FPaths::Combine(WorkspaceRoot, File.GetRelativePath());
        FString Text;
        if (!FFileHelper::LoadFileToString(Text, *Path)) { AddError(TEXT("workspace read failed")); return false; }
        // 101 is unique to the mapped value in this fixture's ordinary file.
        const int32 Replacements = Text.ReplaceInline(TEXT("101"), TEXT("303"));
        WorkspaceEdits += Replacements;
        if (Replacements && !FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        { AddError(TEXT("workspace edit failed")); return false; }
    }
    if (!TestEqual(TEXT("one ordinary mapped value is edited"), WorkspaceEdits, 1)) return false;
    if (!TryResolveEditedSemanticWorkspace(FWorkspaceSource::Directory(WorkspaceRoot), ESemanticWorkspaceMode::State,
        Resources, Resolved, WorkspaceError)) { AddError(WorkspaceError.GetCode()); return false; }
    if (!PrepareWorkspace()) { AddError(ImportError.Code + ImportError.Detail); return false; }
    FPreparedApply WorkspaceApply; FFailure WorkspaceFailure; FOptions WorkspaceOptions; FApplyResult WorkspaceResult;
    WorkspaceOptions.bAllowDomainAuthorizedOwners = true;
    if (!PrepareObject(Target.Get(), Changes, WorkspaceOptions, WorkspaceApply, WorkspaceFailure))
    { AddError(WorkspaceFailure.Diagnostic.Code + WorkspaceFailure.Diagnostic.PublicMessage); return false; }
    SameNative(BeforeSet, BeforeMap);
    if (!ApplyPrepared(WorkspaceApply, WorkspaceResult)) { AddError(WorkspaceResult.Diagnostic.Code); return false; }
    auto WorkspaceExpected = BeforeMap; WorkspaceExpected.FindChecked(Key(811, 0)) = 303;
    SameNative(BeforeSet, WorkspaceExpected);
    TestTrue(TEXT("persisted source facts verify native edit"), VerifyPrepared(WorkspaceApply, WorkspaceResult));
    TestTrue(TEXT("persisted source facts roll back complete state"), RollbackPrepared(WorkspaceApply, WorkspaceResult));
    SameNative(BeforeSet, BeforeMap);
    Target->Records.Find(Key(811, 0))->Payload = 71;
    if (!CaptureWorkspace()) { Target->Records = BeforeSet; AddError(Error + WorkspaceError.GetCode()); return false; }
    TestFalse(TEXT("recaptured changed native payload rejects stale workspace"), PrepareWorkspace());
    TestEqual(TEXT("existing workspace revision gate rejects stale"), ImportError.Code, FString(TEXT("hjson_import_stale_base_revision")));
    TestEqual(TEXT("stale workspace produces no changes"), Changes.Num(), 0);
    Target->Records = BeforeSet;
    SameNative(BeforeSet, BeforeMap);
    TestTrue(TEXT("workspace fixture cleanup"), IFileManager::Get().DeleteDirectory(*WorkspaceRoot, false, true));
    return true;
}
