#include "Misc/AutomationTest.h"
#include "Utilities/AuthoringObjectApply/UeAgentObjectApplyNotificationTestTypes.h"
#include "Composition/HjsonImport/UeAgentReflectedObjectRecord.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentSetSparseCompletionTest,
    "UAI.Core.Authoring.SetSparseCompletion", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentSetSparseCompletionTest::RunTest(const FString&)
{
    using namespace UE::UAI::Composition::HjsonImport;
    using namespace UeAgentAuthoringObjectApply;
    TStrongObjectPtr<UUeAgentSetCompletionTarget> Target(NewObject<UUeAgentSetCompletionTarget>());
    auto MakeItem = [](FName Key, int32 Value, int32 ReadOnly) {
        FUeAgentKeyedDynamicSettings Item; Item.Key = Key;
        Item.Payload.InitializeAs<FUeAgentAlternateNotificationSettings>();
        auto& Payload = Item.Payload.GetMutable<FUeAgentAlternateNotificationSettings>();
        Payload.Value = Value; Payload.ReadOnly = ReadOnly;
        return Item;
    };
    Target->Records.Add(MakeItem(TEXT("Alpha"), 31, 811));
    Target->Records.Add(MakeItem(TEXT("Beta"), 42, 922));
    const auto Before = Target->Records;
    auto Expected = Before;
    auto SameNative = [&](const TSet<FUeAgentKeyedDynamicSettings>& Desired) {
        bool Same = TestEqual(TEXT("native set count"), Target->Records.Num(), Desired.Num());
        for (const auto& Item : Desired) {
            const auto* Actual = Target->Records.Find(Item);
            if (!TestNotNull(TEXT("native identity exists"), Actual)) { Same = false; continue; }
            if (!TestTrue(TEXT("complete payload type"), Actual->Payload.GetScriptStruct() == Item.Payload.GetScriptStruct())) { Same = false; continue; }
            const auto& A = Actual->Payload.Get<FUeAgentAlternateNotificationSettings>();
            const auto& D = Item.Payload.Get<FUeAgentAlternateNotificationSettings>();
            Same &= TestEqual(TEXT("complete payload Value"), A.Value, D.Value);
            Same &= TestEqual(TEXT("complete payload ReadOnly"), A.ReadOnly, D.ReadOnly);
        }
        return Same;
    };
    UeAgentDetailsAuthoringService::FCapturePolicy Policy;
    TSharedPtr<FJsonObject> Properties, Definitions;
    FString Error;
    if (!CaptureReflectedObjectRecord(*Target.Get(), Policy, Properties, Definitions, Error)) { AddError(Error); return false; }
    auto Set = Properties->GetObjectField(TEXT("Test"))->GetObjectField(TEXT("Records"));
    if (!TestTrue(TEXT("ordinary set value is the direct items array"), Set->HasTypedField<EJson::Array>(TEXT("value")))) return false;
    TMap<FString, TSharedPtr<FJsonValue>> ByKey;
    for (const auto& Item : Set->GetArrayField(TEXT("value"))) {
        const auto Fields = Item->AsObject()->GetObjectField(TEXT("fields"));
        ByKey.Add(Fields->GetStringField(TEXT("Key")), Item);
        TestFalse(TEXT("ordinary export omits readonly payload"),
            Fields->GetObjectField(TEXT("Payload"))->GetObjectField(TEXT("fields"))->HasField(TEXT("ReadOnly")));
    }
    auto CheckNoOp = [&]() {
        FPreparedApply Prepared;
        if (!PrepareReflectedObjectRecord(*Target.Get(), Policy, Properties, Prepared, Error)) { AddError(Error); return false; }
        TestEqual(TEXT("unchanged or reordered set prepares no edits"), Prepared.NumProperties(), 0);
        return SameNative(Before);
    };
    if (!TestTrue(TEXT("ordinary export has the two expected identities"), ByKey.Contains(TEXT("Alpha")) && ByKey.Contains(TEXT("Beta")))) return false;
    if (!CheckNoOp()) return false;
    Set->SetArrayField(TEXT("value"), {ByKey.FindChecked(TEXT("Beta")), ByKey.FindChecked(TEXT("Alpha"))});
    if (!CheckNoOp()) return false;
    ByKey.FindChecked(TEXT("Alpha"))->AsObject()->GetObjectField(TEXT("fields"))->GetObjectField(TEXT("Payload"))->GetObjectField(TEXT("fields"))->SetNumberField(TEXT("Value"), 57);
    ByKey.FindChecked(TEXT("Beta"))->AsObject()->GetObjectField(TEXT("fields"))->RemoveField(TEXT("Payload"));
    Expected.Find(MakeItem(TEXT("Alpha"), 0, 0))->Payload.GetMutable<FUeAgentAlternateNotificationSettings>().Value = 57;
    auto RoundTrip = [&](bool Tamper) {
        FPreparedApply Prepared; FApplyResult Result;
        if (!PrepareReflectedObjectRecord(*Target.Get(), Policy, Properties, Prepared, Error)) { AddError(Error); return false; }
        TestTrue(TEXT("payload edit is retained"), Prepared.NumProperties() > 0);
        SameNative(Before);
        if (!ApplyPrepared(Prepared, Result)) { AddError(Result.ErrorCode); return false; }
        bool Same = SameNative(Expected);
        TestTrue(TEXT("complete set verifies"), VerifyPrepared(Prepared, Result));
        if (Tamper) {
            Target->Records.Find(MakeItem(TEXT("Alpha"), 0, 0))->Payload.GetMutable<FUeAgentAlternateNotificationSettings>().ReadOnly = -1;
            TestFalse(TEXT("Verify rejects nonidentity readonly payload tamper"), VerifyPrepared(Prepared, Result));
        }
        if (!RollbackPrepared(Prepared, Result)) { AddError(Result.ErrorCode); return false; }
        Same &= SameNative(Before);
        return Same;
    };
    if (!RoundTrip(true)) return false;
    auto PayloadFields = MakeShared<FJsonObject>(); PayloadFields->SetNumberField(TEXT("Value"), 68);
    auto Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("struct_path"), FUeAgentAlternateNotificationSettings::StaticStruct()->GetPathName());
    Payload->SetObjectField(TEXT("fields"), PayloadFields);
    auto Fields = MakeShared<FJsonObject>(); Fields->SetStringField(TEXT("Key"), TEXT("NewKey")); Fields->SetObjectField(TEXT("Payload"), Payload);
    auto Item = MakeShared<FJsonObject>(); Item->SetObjectField(TEXT("fields"), Fields);
    Item->SetField(TEXT("from"), MakeShared<FJsonValueNull>());
    Set->SetArrayField(TEXT("value"), {ByKey.FindChecked(TEXT("Beta")), ByKey.FindChecked(TEXT("Alpha")), MakeShared<FJsonValueObject>(Item)});
    Expected.Add(MakeItem(TEXT("NewKey"), 68, 62));
    if (!RoundTrip(false)) return false;
    Set->SetArrayField(TEXT("value"), {ByKey.FindChecked(TEXT("Alpha")), MakeShared<FJsonValueObject>(Item)});
    Expected.Remove(MakeItem(TEXT("Beta"), 0, 0));
    if (!RoundTrip(false)) return false;
    Set->SetArrayField(TEXT("value"), {ByKey.FindChecked(TEXT("Alpha")), ByKey.FindChecked(TEXT("Alpha"))});
    FPreparedApply Rejected;
    TestFalse(TEXT("duplicate identity rejected"), PrepareReflectedObjectRecord(*Target.Get(), Policy, Properties, Rejected, Error));
    TestTrue(TEXT("native duplicate rejection"), Error.Contains(TEXT("duplicate_set_item")));
    SameNative(Before);

    // Exercise the original comparison entry through every enclosing shape that
    // could otherwise hide the Set payload behind an outer native Identical.
    TStrongObjectPtr<UUeAgentPairedDefinitionTarget> Left(NewObject<UUeAgentPairedDefinitionTarget>());
    TStrongObjectPtr<UUeAgentPairedDefinitionTarget> Right(NewObject<UUeAgentPairedDefinitionTarget>());
    Left->Nested.Set = Before; Right->Nested.Set = Before;
    Right->Nested.Set.Find(MakeItem(TEXT("Alpha"), 0, 0))->Payload.GetMutable<FUeAgentAlternateNotificationSettings>().ReadOnly = -1;
    Left->Fixed[0] = Left->Nested; Right->Fixed[0] = Right->Nested;
    Left->Array.Add(Left->Nested); Right->Array.Add(Right->Nested);
    Left->Optional = Left->Nested; Right->Optional = Right->Nested;
    UeAgentPropertyValueCodec::FOptions Options;
    for (const FName Name : {FName(TEXT("Nested")), FName(TEXT("Fixed")), FName(TEXT("Array")), FName(TEXT("Optional"))}) {
        FProperty* Property = FindFProperty<FProperty>(Left->GetClass(), Name);
        TestTrue(TEXT("fixture custom identity hides payload from native Identical"), Property->Identical_InContainer(Left.Get(), Right.Get()));
        TestFalse(TEXT("nested set full payload differs"), UeAgentPropertyValueCodec::AreSemanticallyEquivalent(Property,
            Property->ContainerPtrToValuePtr<void>(Left.Get()), Property->ContainerPtrToValuePtr<void>(Right.Get()), Left.Get(), Options));
    }
    Target->Records.Empty();
    FUeAgentKeyedDynamicSettings UnsetDynamic; UnsetDynamic.Key = TEXT("UnsetDynamic");
    Target->Records.Add(UnsetDynamic);
    if (!CaptureReflectedObjectRecord(*Target.Get(), Policy, Properties, Definitions, Error)) { AddError(Error); return false; }
    auto DynamicRow = Properties->GetObjectField(TEXT("Test"))->GetObjectField(TEXT("Records"));
    TestTrue(TEXT("unset dynamic payload keeps the type-level from protocol"), DynamicRow->GetArrayField(TEXT("value"))[0]->AsObject()->HasField(TEXT("from")));
    Target->Records.Empty();
    if (!CaptureReflectedObjectRecord(*Target.Get(), Policy, Properties, Definitions, Error)) { AddError(Error); return false; }
    DynamicRow = Properties->GetObjectField(TEXT("Test"))->GetObjectField(TEXT("Records"));
    TestEqual(TEXT("empty collection declares source protocol before a dynamic type is chosen"),
        Definitions->GetObjectField(DynamicRow->GetStringField(TEXT("definition")))->GetObjectField(TEXT("type"))->GetStringField(TEXT("hash_source")),
        FString(TEXT("from_required_null_creates")));
    DynamicRow->SetArrayField(TEXT("value"), {MakeShared<FJsonValueObject>(Item)});
    FPreparedApply NewDynamic; FApplyResult DynamicResult;
    if (!PrepareReflectedObjectRecord(*Target.Get(), Policy, Properties, NewDynamic, Error)) { AddError(Error); return false; }
    TestTrue(TEXT("new dynamic choice prepares without side effects"), Target->Records.IsEmpty());
    if (!ApplyPrepared(NewDynamic, DynamicResult)) { AddError(DynamicResult.ErrorCode); return false; }
    const auto* NewDynamicValue = Target->Records.Find(MakeItem(TEXT("NewKey"), 0, 0));
    if (TestNotNull(TEXT("empty collection creates selected dynamic payload"), NewDynamicValue)) {
        TestEqual(TEXT("dynamic creation keeps requested value"), NewDynamicValue->Payload.Get<FUeAgentAlternateNotificationSettings>().Value, 68);
        TestEqual(TEXT("dynamic creation initializes hidden value"), NewDynamicValue->Payload.Get<FUeAgentAlternateNotificationSettings>().ReadOnly, 62);
    }
    TestTrue(TEXT("new dynamic value verifies"), VerifyPrepared(NewDynamic, DynamicResult));
    TestTrue(TEXT("new dynamic value rolls back"), RollbackPrepared(NewDynamic, DynamicResult));
    TestTrue(TEXT("empty collection restored"), Target->Records.IsEmpty());
    Target->Records = Before;
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentSetActionableProjectionTest,
    "UAI.Core.Authoring.SetActionableProjection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentSetActionableProjectionTest::RunTest(const FString&)
{
    using namespace UE::UAI::Composition::HjsonImport;
    using namespace UeAgentAuthoringObjectApply;
    TStrongObjectPtr<UUeAgentSetActionableProjectionTarget> Target(NewObject<UUeAgentSetActionableProjectionTarget>());
    auto Mixed = [](int32 Identity, int32 Count, int32 Hidden) {
        FUeAgentMixedSetProjectionElement Item; Item.Identity = Identity; Item.Count = Count; Item.Empty.Nested.Kept = Hidden; return Item;
    };
    auto Empty = [](int32 Identity, int32 Hidden) {
        FUeAgentEmptySetProjectionElement Item; Item.Identity = Identity; Item.Nested.Kept = Hidden; return Item;
    };
    Target->Records = {Mixed(811, 10, 833), Mixed(922, 20, 944)};
    Target->EmptyRecords = {Empty(811, 855), Empty(922, 966)};
    const auto Before = Target->Records; const auto EmptyBefore = Target->EmptyRecords;
    auto Expected = Before; auto EmptyExpected = EmptyBefore;
    auto SameNative = [&](const auto& Records, const auto& EmptyRecords) {
        bool Same = TestEqual(TEXT("full mixed native count"), Target->Records.Num(), Records.Num());
        Same &= TestEqual(TEXT("full empty native count"), Target->EmptyRecords.Num(), EmptyRecords.Num());
        for (const auto& Item : Records) {
            const auto* Actual = Target->Records.Find(Item);
            if (!TestNotNull(TEXT("mixed native identity retained"), Actual)) { Same = false; continue; }
            Same &= TestEqual(TEXT("mixed identity exact"), Actual->Identity, Item.Identity);
            Same &= TestEqual(TEXT("mixed editable Count exact"), Actual->Count, Item.Count);
            Same &= TestEqual(TEXT("mixed nondefault hidden payload exact"), Actual->Empty.Nested.Kept, Item.Empty.Nested.Kept);
        }
        for (const auto& Item : EmptyRecords) {
            const auto* Actual = Target->EmptyRecords.Find(Item);
            if (!TestNotNull(TEXT("empty native identity retained"), Actual)) { Same = false; continue; }
            Same &= TestEqual(TEXT("empty identity exact"), Actual->Identity, Item.Identity);
            Same &= TestEqual(TEXT("empty nondefault hidden payload exact"), Actual->Nested.Kept, Item.Nested.Kept);
        }
        return Same;
    };
    UeAgentDetailsAuthoringService::FCapturePolicy Policy;
    TSharedPtr<FJsonObject> Properties, Definitions; FString Error;
    if (!CaptureReflectedObjectRecord(*Target.Get(), Policy, Properties, Definitions, Error)) { AddError(Error); return false; }
    SameNative(Before, EmptyBefore);
    const auto Category = Properties->GetObjectField(TEXT("Test"));
    TMap<int32, TSharedPtr<FJsonValue>> MixedByIdentity, EmptyByIdentity;
    for (const FString Name : {FString(TEXT("Records")), FString(TEXT("EmptyRecords"))}) {
        const bool IsMixed = Name == TEXT("Records");
        const auto Row = Category->GetObjectField(Name);
        const auto& Items = Row->GetArrayField(TEXT("value"));
        const auto& Schemas = Definitions->GetObjectField(Row->GetStringField(TEXT("definition")))->GetObjectField(TEXT("value_definition"))->GetArrayField(TEXT("items"));
        if (!TestEqual(TEXT("projection retains two Set elements"), Items.Num(), 2)
            || !TestEqual(TEXT("Set definitions remain paired"), Schemas.Num(), Items.Num())) return false;
        for (int32 Index = 0; Index < Items.Num(); ++Index) {
            const auto Item = Items[Index]->AsObject(); const auto Fields = Item->GetObjectField(TEXT("fields"));
            const auto Schema = Schemas[Index]->AsObject(); const auto SchemaFields = Schema->GetObjectField(TEXT("fields"));
            TestEqual(TEXT("desired element has only actionable fields"), Fields->Values.Num(), IsMixed ? 1 : 0);
            TestEqual(TEXT("element definition matches projected fields"), SchemaFields->Values.Num(), Fields->Values.Num());
            TestFalse(TEXT("empty member omitted from desired"), Fields->HasField(IsMixed ? TEXT("Empty") : TEXT("Nested")));
            TestFalse(TEXT("empty member omitted from definition"), SchemaFields->HasField(IsMixed ? TEXT("Empty") : TEXT("Nested")));
            TestEqual(TEXT("source contract remains on paired definition"), Schema->GetStringField(TEXT("hash_source")), FString(TEXT("from_required_null_creates")));
            const auto SourceFields = Item->GetObjectField(TEXT("from"))->GetObjectField(TEXT("fields"));
            const int32 Identity = static_cast<int32>(SourceFields->GetObjectField(TEXT("Identity"))->GetNumberField(TEXT("literal")));
            if (!TestTrue(TEXT("complete source identity is retained"), Identity == 811 || Identity == 922)) return false;
            const auto HiddenFields = IsMixed ? SourceFields->GetObjectField(TEXT("Empty"))->GetObjectField(TEXT("fields")) : SourceFields;
            const int32 Hidden = static_cast<int32>(HiddenFields->GetObjectField(TEXT("Nested"))->GetObjectField(TEXT("fields"))->GetObjectField(TEXT("Kept"))->GetNumberField(TEXT("literal")));
            TestEqual(TEXT("source hidden facts are neither pruned nor compacted"), Hidden,
                IsMixed ? (Identity == 811 ? 833 : 944) : (Identity == 811 ? 855 : 966));
            if (IsMixed) {
                TestEqual(TEXT("Count remains with its source identity"), Fields->GetNumberField(TEXT("Count")), Identity == 811 ? 10.0 : 20.0);
                TestTrue(TEXT("retained Count definition exists"), SchemaFields->HasField(TEXT("Count")));
                MixedByIdentity.Add(Identity, Items[Index]);
            } else EmptyByIdentity.Add(Identity, Items[Index]);
        }
    }
    FPreparedApply NoOp;
    if (!PrepareReflectedObjectRecord(*Target.Get(), Policy, Properties, NoOp, Error)) { AddError(Error); return false; }
    TestEqual(TEXT("ordinary projected Set is no-op"), NoOp.NumProperties(), 0);
    auto RoundTrip = [&]() {
        FPreparedApply Prepared; FApplyResult Result;
        if (!PrepareReflectedObjectRecord(*Target.Get(), Policy, Properties, Prepared, Error)) { AddError(Error); return false; }
        SameNative(Before, EmptyBefore);
        if (!ApplyPrepared(Prepared, Result)) { AddError(Result.ErrorCode); return false; }
        const bool Same = SameNative(Expected, EmptyExpected);
        TestTrue(TEXT("projected Set complete values verify"), VerifyPrepared(Prepared, Result));
        if (!RollbackPrepared(Prepared, Result)) { AddError(Result.ErrorCode); return false; }
        SameNative(Before, EmptyBefore);
        return Same;
    };
    const auto MixedRow = Category->GetObjectField(TEXT("Records")); const auto EmptyRow = Category->GetObjectField(TEXT("EmptyRecords"));
    MixedRow->SetArrayField(TEXT("value"), {MixedByIdentity.FindChecked(922), MixedByIdentity.FindChecked(811)});
    EmptyRow->SetArrayField(TEXT("value"), {EmptyByIdentity.FindChecked(922), EmptyByIdentity.FindChecked(811)});
    FPreparedApply Reordered;
    if (!PrepareReflectedObjectRecord(*Target.Get(), Policy, Properties, Reordered, Error)) { AddError(Error); return false; }
    TestEqual(TEXT("pure Set reorder is no-op after projection"), Reordered.NumProperties(), 0);
    MixedByIdentity.FindChecked(811)->AsObject()->GetObjectField(TEXT("fields"))->SetNumberField(TEXT("Count"), 57);
    Expected.Find(Mixed(811, 0, 0))->Count = 57;
    if (!RoundTrip()) return false;
    auto NewItem = [](bool IsMixed) {
        auto Fields = MakeShared<FJsonObject>(); if (IsMixed) Fields->SetNumberField(TEXT("Count"), 68);
        auto Item = MakeShared<FJsonObject>(); Item->SetObjectField(TEXT("fields"), Fields); Item->SetField(TEXT("from"), MakeShared<FJsonValueNull>());
        return MakeShared<FJsonValueObject>(Item);
    };
    Expected.Add(Mixed(7, 68, 73)); EmptyExpected.Remove(Empty(811, 0)); EmptyExpected.Add(Empty(7, 73));
    MixedRow->SetArrayField(TEXT("value"), {MixedByIdentity.FindChecked(922), MixedByIdentity.FindChecked(811), NewItem(true)});
    EmptyRow->SetArrayField(TEXT("value"), {EmptyByIdentity.FindChecked(922), NewItem(false)});
    if (!RoundTrip()) return false;
    Expected.Empty(); EmptyExpected.Empty();
    MixedRow->SetArrayField(TEXT("value"), {}); EmptyRow->SetArrayField(TEXT("value"), {});
    if (!RoundTrip()) return false;
    MixedByIdentity.FindChecked(811)->AsObject()->GetObjectField(TEXT("fields"))->SetNumberField(TEXT("Identity"), 999);
    MixedRow->SetArrayField(TEXT("value"), {MixedByIdentity.FindChecked(811)});
    FPreparedApply Rejected;
    TestFalse(TEXT("desired identity changes remain forbidden"), PrepareReflectedObjectRecord(*Target.Get(), Policy, Properties, Rejected, Error));
    TestTrue(TEXT("readonly identity rejection stays explicit"), Error.Contains(TEXT("read_only_or_non_editable")));
    SameNative(Before, EmptyBefore);
    return !HasAnyErrors();
}
