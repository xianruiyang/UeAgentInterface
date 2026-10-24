#include "Misc/AutomationTest.h"
#include "Utilities/AuthoringObjectApply/UeAgentObjectApplyNotificationTestTypes.h"
#include "Composition/HjsonImport/UeAgentReflectedObjectRecord.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentMapSparseCompletionTest,
    "UAI.Core.Authoring.MapSparseCompletion", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentMapSparseCompletionTest::RunTest(const FString&)
{
    using namespace UE::UAI::Composition::HjsonImport;
    using namespace UeAgentAuthoringObjectApply;
    TStrongObjectPtr<UUeAgentMapCompletionTarget> Target(NewObject<UUeAgentMapCompletionTarget>());
    auto& Alpha = Target->Records.Add(TEXT("Alpha"));
    Alpha.Count = 10; Alpha.Values = {101, 102}; Alpha.ReadOnly = 811;
    auto& Beta = Target->Records.Add(TEXT("Beta"));
    Beta.Count = 20; Beta.Values = {201}; Beta.ReadOnly = 922;
    const auto Before = Target->Records;
    auto Expected = Before;
    Expected.FindChecked(TEXT("Alpha")).Count = 57;
    auto SameNative = [&](const TMap<FName, FUeAgentNotificationSettings>& Desired) {
        auto* MapProperty = FindFProperty<FMapProperty>(Target->GetClass(), TEXT("Records"));
        const bool bSame = TestTrue(TEXT("complete native map matches predetermined keys and all fields"),
            MapProperty->Identical(&Target->Records, &Desired, 0));
        if (!bSame) for (const auto& Pair : Desired) {
            const auto* Actual = Target->Records.Find(Pair.Key);
            if (!Actual) { AddError(TEXT("missing key: ") + Pair.Key.ToString()); continue; }
            TestEqual(*(Pair.Key.ToString() + TEXT(" Count")), Actual->Count, Pair.Value.Count);
            TestEqual(*(Pair.Key.ToString() + TEXT(" ReadOnly")), Actual->ReadOnly, Pair.Value.ReadOnly);
            TestTrue(*(Pair.Key.ToString() + TEXT(" complete Values")), Actual->Values == Pair.Value.Values);
        }
        return bSame;
    };
    UeAgentDetailsAuthoringService::FCapturePolicy Policy;
    TSharedPtr<FJsonObject> Properties, Definitions;
    FString Error;
    if (!CaptureReflectedObjectRecord(*Target.Get(), Policy, Properties, Definitions, Error)) { AddError(Error); return false; }
    FPreparedApply Unchanged;
    if (!PrepareReflectedObjectRecord(*Target.Get(), Policy, Properties, Unchanged, Error)) { AddError(Error); return false; }
    TestEqual(TEXT("ordinary export unchanged prepares no native edits"), Unchanged.NumProperties(), 0);
    SameNative(Before);
    auto Map = Properties->GetObjectField(TEXT("Test"))->GetObjectField(TEXT("Records"))->GetObjectField(TEXT("value"));
    TMap<FString, TSharedPtr<FJsonValue>> ByKey;
    for (const auto& Entry : Map->GetArrayField(TEXT("entries"))) {
        const auto Object = Entry->AsObject();
        TestFalse(TEXT("complete scalar map keys keep the existing entry shape"), Object->HasField(TEXT("from")));
        ByKey.Add(Object->GetStringField(TEXT("key")), Entry);
        const auto Fields = Object->GetObjectField(TEXT("value"))->GetObjectField(TEXT("fields"));
        TestFalse(TEXT("ordinary export excludes readonly map member"), Fields->HasField(TEXT("ReadOnly")));
        const auto Count = Fields->TryGetField(TEXT("Count"));
        Fields->Values.Empty(); Fields->SetField(TEXT("Count"), Count);
    }
    ByKey.FindChecked(TEXT("Alpha"))->AsObject()->GetObjectField(TEXT("value"))->GetObjectField(TEXT("fields"))->SetNumberField(TEXT("Count"), 57);
    // New item explicitly supplies its writable field, leaving all other fields
    // to the native initialized value. Existing entries arrive in reverse order.
    auto NewFields = MakeShared<FJsonObject>(); NewFields->SetNumberField(TEXT("Count"), 68);
    auto NewValue = MakeShared<FJsonObject>(); NewValue->SetObjectField(TEXT("fields"), NewFields);
    auto NewEntry = MakeShared<FJsonObject>(); NewEntry->SetStringField(TEXT("key"), TEXT("NewKey")); NewEntry->SetObjectField(TEXT("value"), NewValue);
    auto RoundTrip = [&]() {
        FPreparedApply Prepared; FApplyResult Result;
        if (!PrepareReflectedObjectRecord(*Target.Get(), Policy, Properties, Prepared, Error)) { AddError(Error); return false; }
        SameNative(Before);
        if (!ApplyPrepared(Prepared, Result)) { AddError(Result.ErrorCode); return false; }
        const bool bMatches = SameNative(Expected);
        TestTrue(TEXT("completed sparse map verifies"), VerifyPrepared(Prepared, Result));
        if (!RollbackPrepared(Prepared, Result)) { AddError(Result.ErrorCode); return false; }
        SameNative(Before);
        return bMatches;
    };
    Map->SetArrayField(TEXT("entries"), {ByKey.FindChecked(TEXT("Beta")), ByKey.FindChecked(TEXT("Alpha"))});
    if (!RoundTrip()) return false;
    Expected.Add(TEXT("NewKey")).Count = 68;
    Map->SetArrayField(TEXT("entries"), {ByKey.FindChecked(TEXT("Beta")), ByKey.FindChecked(TEXT("Alpha")), MakeShared<FJsonValueObject>(NewEntry)});
    if (!RoundTrip()) return false;
    Map->SetArrayField(TEXT("entries"), {ByKey.FindChecked(TEXT("Alpha")), ByKey.FindChecked(TEXT("Alpha"))});
    FPreparedApply Rejected;
    TestFalse(TEXT("duplicate native keys still reject before writing"), PrepareReflectedObjectRecord(*Target.Get(), Policy, Properties, Rejected, Error));
    TestTrue(TEXT("duplicate key rejection uses existing codec policy"), Error.Contains(TEXT("duplicate_map_key")));
    SameNative(Before);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentMapActionableProjectionTest,
    "UAI.Core.Authoring.MapActionableProjection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentMapActionableProjectionTest::RunTest(const FString&)
{
    using namespace UE::UAI::Composition::HjsonImport;
    using namespace UeAgentAuthoringObjectApply;
    TStrongObjectPtr<UUeAgentMapActionableProjectionTarget> Target(NewObject<UUeAgentMapActionableProjectionTarget>());
    auto& Alpha = Target->Records.Add(TEXT("Alpha"));
    Alpha.Count = 10; Alpha.Empty.Nested.Kept = 811;
    auto& Beta = Target->Records.Add(TEXT("Beta"));
    Beta.Count = 20; Beta.Empty.Nested.Kept = 922;
    Target->EmptyRecords.Add(TEXT("Alpha")).Nested.Kept = 833;
    Target->EmptyRecords.Add(TEXT("Beta")).Nested.Kept = 944;
    const auto Before = Target->Records;
    const auto EmptyBefore = Target->EmptyRecords;
    auto Expected = Before;
    auto EmptyExpected = EmptyBefore;
    auto SameNative = [&](const auto& Records, const auto& EmptyRecords) {
        bool bSame = TestTrue(TEXT("complete mixed native map matches keys and hidden fields"),
            FindFProperty<FMapProperty>(Target->GetClass(), TEXT("Records"))->Identical(&Target->Records, &Records, 0));
        bSame &= TestTrue(TEXT("complete nonactionable native map matches keys and hidden fields"),
            FindFProperty<FMapProperty>(Target->GetClass(), TEXT("EmptyRecords"))->Identical(&Target->EmptyRecords, &EmptyRecords, 0));
        return bSame;
    };
    UeAgentDetailsAuthoringService::FCapturePolicy Policy;
    TSharedPtr<FJsonObject> Properties, Definitions;
    FString Error;
    if (!CaptureReflectedObjectRecord(*Target.Get(), Policy, Properties, Definitions, Error)) { AddError(Error); return false; }
    SameNative(Before, EmptyBefore);
    const auto Category = Properties->GetObjectField(TEXT("Test"));
    TMap<FString, TSharedPtr<FJsonValue>> MixedByKey, EmptyByKey;
    for (const FString Name : {FString(TEXT("Records")), FString(TEXT("EmptyRecords"))}) {
        const bool bMixed = Name == TEXT("Records");
        const auto Row = Category->GetObjectField(Name);
        const auto& Entries = Row->GetObjectField(TEXT("value"))->GetArrayField(TEXT("entries"));
        const auto& EntryDefinitions = Definitions->GetObjectField(Row->GetStringField(TEXT("definition")))
            ->GetObjectField(TEXT("value_definition"))->GetObjectField(TEXT("fields"))
            ->GetObjectField(TEXT("entries"))->GetArrayField(TEXT("items"));
        if (!TestEqual(TEXT("ordinary projection retains both Map entries"), Entries.Num(), 2)
            || !TestEqual(TEXT("ordinary definitions remain paired with entries"), EntryDefinitions.Num(), Entries.Num())) return false;
        for (int32 Index = 0; Index < Entries.Num(); ++Index) {
            const auto Entry = Entries[Index]->AsObject();
            const FString Key = Entry->GetStringField(TEXT("key"));
            TestTrue(TEXT("original readable key is preserved"), Key == TEXT("Alpha") || Key == TEXT("Beta"));
            const auto Fields = Entry->GetObjectField(TEXT("value"))->GetObjectField(TEXT("fields"));
            const auto FieldDefinitions = EntryDefinitions[Index]->AsObject()->GetObjectField(TEXT("fields"))
                ->GetObjectField(TEXT("value"))->GetObjectField(TEXT("fields"));
            TestEqual(TEXT("value contains only its actionable fields"), Fields->Values.Num(), bMixed ? 1 : 0);
            TestEqual(TEXT("definition contains exactly the projected fields"), FieldDefinitions->Values.Num(), Fields->Values.Num());
            TestFalse(TEXT("empty nested value is absent"), Fields->HasField(bMixed ? TEXT("Empty") : TEXT("Nested")));
            TestFalse(TEXT("empty nested definition is absent"), FieldDefinitions->HasField(bMixed ? TEXT("Empty") : TEXT("Nested")));
            if (bMixed) {
                TestEqual(TEXT("Count stays bound to its original key"), Fields->GetNumberField(TEXT("Count")),
                    static_cast<double>(Before.FindChecked(FName(*Key)).Count));
                TestTrue(TEXT("retained Count has its ordinary type definition"),
                    FieldDefinitions->GetObjectField(TEXT("Count"))->HasTypedField<EJson::Object>(TEXT("type")));
                MixedByKey.Add(Key, Entries[Index]);
            } else EmptyByKey.Add(Key, Entries[Index]);
        }
    }
    FPreparedApply Unchanged;
    if (!PrepareReflectedObjectRecord(*Target.Get(), Policy, Properties, Unchanged, Error)) { AddError(Error); return false; }
    TestEqual(TEXT("ordinary projected maps prepare no native edits"), Unchanged.NumProperties(), 0);
    SameNative(Before, EmptyBefore);
    const auto MixedMap = Category->GetObjectField(TEXT("Records"))->GetObjectField(TEXT("value"));
    const auto EmptyMap = Category->GetObjectField(TEXT("EmptyRecords"))->GetObjectField(TEXT("value"));
    MixedByKey.FindChecked(TEXT("Alpha"))->AsObject()->GetObjectField(TEXT("value"))->GetObjectField(TEXT("fields"))->SetNumberField(TEXT("Count"), 57);
    Expected.FindChecked(TEXT("Alpha")).Count = 57;
    MixedMap->SetArrayField(TEXT("entries"), {MixedByKey.FindChecked(TEXT("Beta")), MixedByKey.FindChecked(TEXT("Alpha"))});
    EmptyMap->SetArrayField(TEXT("entries"), {EmptyByKey.FindChecked(TEXT("Beta")), EmptyByKey.FindChecked(TEXT("Alpha"))});
    auto RoundTrip = [&]() {
        FPreparedApply Prepared; FApplyResult Result;
        if (!PrepareReflectedObjectRecord(*Target.Get(), Policy, Properties, Prepared, Error)) { AddError(Error); return false; }
        SameNative(Before, EmptyBefore);
        if (!ApplyPrepared(Prepared, Result)) { AddError(Result.ErrorCode); return false; }
        const bool bMatches = SameNative(Expected, EmptyExpected);
        TestTrue(TEXT("projected Map edit verifies complete prepared native values"), VerifyPrepared(Prepared, Result));
        if (!RollbackPrepared(Prepared, Result)) { AddError(Result.ErrorCode); return false; }
        SameNative(Before, EmptyBefore);
        return bMatches;
    };
    if (!RoundTrip()) return false;
    auto NewEntry = [](const bool bMixed) {
        auto Fields = MakeShared<FJsonObject>();
        if (bMixed) Fields->SetNumberField(TEXT("Count"), 68);
        auto Value = MakeShared<FJsonObject>(); Value->SetObjectField(TEXT("fields"), Fields);
        auto Entry = MakeShared<FJsonObject>(); Entry->SetStringField(TEXT("key"), TEXT("NewKey")); Entry->SetObjectField(TEXT("value"), Value);
        return MakeShared<FJsonValueObject>(Entry);
    };
    Expected.Add(TEXT("NewKey")).Count = 68;
    EmptyExpected.Remove(TEXT("Alpha")); EmptyExpected.Add(TEXT("NewKey"));
    MixedMap->SetArrayField(TEXT("entries"), {MixedByKey.FindChecked(TEXT("Beta")), MixedByKey.FindChecked(TEXT("Alpha")), NewEntry(true)});
    EmptyMap->SetArrayField(TEXT("entries"), {EmptyByKey.FindChecked(TEXT("Beta")), NewEntry(false)});
    if (!RoundTrip()) return false;
    return !HasAnyErrors();
}
