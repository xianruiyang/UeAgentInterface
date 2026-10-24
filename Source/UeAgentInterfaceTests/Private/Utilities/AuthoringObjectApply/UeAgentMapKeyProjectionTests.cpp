#include "Misc/AutomationTest.h"
#include "Utilities/AuthoringObjectApply/UeAgentObjectApplyNotificationTestTypes.h"
#include "Composition/HjsonImport/UeAgentReflectedObjectRecord.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentMapKeyProjectionTest,
    "UAI.Core.Authoring.MapKeyActionableProjection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentMapKeyProjectionTest::RunTest(const FString&)
{
    using namespace UE::UAI::Composition::HjsonImport;
    using namespace UeAgentAuthoringObjectApply;
    TStrongObjectPtr<UUeAgentMapKeyProjectionTarget> Target(NewObject<UUeAgentMapKeyProjectionTarget>());
    auto Mixed = [](int32 Identity, int32 Count, int32 Hidden) {
        FUeAgentMixedSetProjectionElement Key; Key.Identity = Identity; Key.Count = Count; Key.Empty.Nested.Kept = Hidden; return Key;
    };
    auto Empty = [](int32 Identity, int32 Hidden) {
        FUeAgentEmptySetProjectionElement Key; Key.Identity = Identity; Key.Nested.Kept = Hidden; return Key;
    };
    auto Dynamic = [](FName Name, int32 Count, int32 Hidden) {
        FUeAgentKeyedDynamicSettings Key; Key.Key = Name; Key.Payload.InitializeAs<FUeAgentMixedActionableSettings>();
        auto& Payload = Key.Payload.GetMutable<FUeAgentMixedActionableSettings>(); Payload.Count = Count; Payload.Empty.Nested.Kept = Hidden; return Key;
    };
    Target->Records.Add(Mixed(811, 10, 833), 101); Target->Records.Add(Mixed(922, 20, 944), 202);
    Target->EmptyRecords.Add(Empty(811, 855), 111); Target->EmptyRecords.Add(Empty(922, 966), 222);
    Target->DynamicRecords.Add(Dynamic(TEXT("Alpha"), 30, 877), 121); Target->DynamicRecords.Add(Dynamic(TEXT("Beta"), 40, 988), 242);
    const auto Before = Target->Records; const auto EmptyBefore = Target->EmptyRecords; const auto DynamicBefore = Target->DynamicRecords;
    auto Expected = Before; auto EmptyExpected = EmptyBefore; auto DynamicExpected = DynamicBefore;
    auto CheckMap = [&](const auto& Actual, const auto& Wanted, auto CheckKey) {
        bool Same = TestEqual(TEXT("native map count"), Actual.Num(), Wanted.Num());
        for (const auto& Pair : Wanted) {
            const auto* Value = Actual.Find(Pair.Key);
            if (!TestNotNull(TEXT("native map identity retained"), Value)) { Same = false; continue; }
            Same &= TestEqual(TEXT("mapped value exact"), *Value, Pair.Value);
            for (const auto& Found : Actual) if (Found.Key == Pair.Key) Same &= CheckKey(Found.Key, Pair.Key);
        }
        return Same;
    };
    auto SameNative = [&](const auto& Records, const auto& EmptyRecords, const auto& DynamicRecords) {
        bool Same = CheckMap(Target->Records, Records, [&](const auto& A, const auto& B) {
            bool Equal = TestEqual(TEXT("mixed key identity"), A.Identity, B.Identity);
            Equal &= TestEqual(TEXT("mixed key Count"), A.Count, B.Count);
            Equal &= TestEqual(TEXT("mixed key hidden value"), A.Empty.Nested.Kept, B.Empty.Nested.Kept); return Equal;
        });
        Same &= CheckMap(Target->EmptyRecords, EmptyRecords, [&](const auto& A, const auto& B) {
            bool Equal = TestEqual(TEXT("empty key identity"), A.Identity, B.Identity);
            Equal &= TestEqual(TEXT("empty key hidden value"), A.Nested.Kept, B.Nested.Kept); return Equal;
        });
        Same &= CheckMap(Target->DynamicRecords, DynamicRecords, [&](const auto& A, const auto& B) {
            bool Equal = TestEqual(TEXT("dynamic key identity"), A.Key, B.Key);
            if (!TestTrue(TEXT("dynamic key payload type"), A.Payload.GetScriptStruct() == B.Payload.GetScriptStruct())) return false;
            Equal &= TestEqual(TEXT("dynamic key Count"), A.Payload.template Get<FUeAgentMixedActionableSettings>().Count, B.Payload.template Get<FUeAgentMixedActionableSettings>().Count);
            Equal &= TestEqual(TEXT("dynamic key hidden value"), A.Payload.template Get<FUeAgentMixedActionableSettings>().Empty.Nested.Kept, B.Payload.template Get<FUeAgentMixedActionableSettings>().Empty.Nested.Kept); return Equal;
        });
        return Same;
    };
    UeAgentDetailsAuthoringService::FCapturePolicy Policy;
    TSharedPtr<FJsonObject> Properties, Definitions; FString Error;
    if (!CaptureReflectedObjectRecord(*Target.Get(), Policy, Properties, Definitions, Error)) { AddError(Error); return false; }
    SameNative(Before, EmptyBefore, DynamicBefore);
    const auto Category = Properties->GetObjectField(TEXT("Test"));
    TMap<FString, TArray<TSharedPtr<FJsonValue>>> Original;
    for (const FString Name : {FString(TEXT("Records")), FString(TEXT("EmptyRecords")), FString(TEXT("DynamicRecords"))}) {
        const bool IsEmpty = Name == TEXT("EmptyRecords"), IsDynamic = Name == TEXT("DynamicRecords");
        const auto Row = Category->GetObjectField(Name);
        const auto& Entries = Row->GetObjectField(TEXT("value"))->GetArrayField(TEXT("entries"));
        const auto& Schemas = Definitions->GetObjectField(Row->GetStringField(TEXT("definition")))->GetObjectField(TEXT("value_definition"))
            ->GetObjectField(TEXT("fields"))->GetObjectField(TEXT("entries"))->GetArrayField(TEXT("items"));
        if (!TestEqual(TEXT("key projection preserves entries"), Entries.Num(), 2)
            || !TestEqual(TEXT("key definitions remain paired"), Schemas.Num(), Entries.Num())) return false;
        TArray<TSharedPtr<FJsonValue>> Ordered; Ordered.SetNum(2);
        for (int32 Index = 0; Index < Entries.Num(); ++Index) {
            const auto Entry = Entries[Index]->AsObject(); const auto KeyFields = Entry->GetObjectField(TEXT("key"))->GetObjectField(TEXT("fields"));
            const auto Schema = Schemas[Index]->AsObject();
            const auto KeySchema = Schema->GetObjectField(TEXT("fields"))->GetObjectField(TEXT("key"))->GetObjectField(TEXT("fields"));
            TestEqual(TEXT("key desired fields are actionable only"), KeyFields->Values.Num(), IsEmpty ? 0 : IsDynamic ? 2 : 1);
            TestEqual(TEXT("key schema matches desired fields"), KeySchema->Values.Num(), KeyFields->Values.Num());
            TestEqual(TEXT("entry source contract preserved"), Schema->GetStringField(TEXT("hash_source")), FString(TEXT("from_required_null_creates")));
            const auto Source = Entry->GetObjectField(TEXT("from"))->GetObjectField(TEXT("fields"));
            const bool First = IsDynamic ? Source->GetObjectField(TEXT("Key"))->GetStringField(TEXT("literal")) == TEXT("Alpha")
                : Source->GetObjectField(TEXT("Identity"))->GetNumberField(TEXT("literal")) == 811;
            const auto Payload = IsDynamic ? Source->GetObjectField(TEXT("Payload"))->GetObjectField(TEXT("fields")) : Source;
            const auto HiddenFields = IsEmpty ? Payload : Payload->GetObjectField(TEXT("Empty"))->GetObjectField(TEXT("fields"));
            TestEqual(TEXT("source facts remain complete and uncompact"), HiddenFields->GetObjectField(TEXT("Nested"))->GetObjectField(TEXT("fields"))->GetObjectField(TEXT("Kept"))->GetNumberField(TEXT("literal")),
                IsDynamic ? (First ? 877.0 : 988.0) : IsEmpty ? (First ? 855.0 : 966.0) : (First ? 833.0 : 944.0));
            if (IsDynamic) {
                const auto DesiredPayload = KeyFields->GetObjectField(TEXT("Payload"));
                TestEqual(TEXT("dynamic type selection preserved"), DesiredPayload->GetStringField(TEXT("struct_path")), FUeAgentMixedActionableSettings::StaticStruct()->GetPathName());
                TestEqual(TEXT("dynamic payload pruned recursively"), DesiredPayload->GetObjectField(TEXT("fields"))->Values.Num(), 1);
                TestEqual(TEXT("dynamic payload definition pruned recursively"), KeySchema->GetObjectField(TEXT("Payload"))->GetObjectField(TEXT("value_definition"))->GetObjectField(TEXT("fields"))->Values.Num(), 1);
            }
            Ordered[First ? 0 : 1] = Entries[Index];
        }
        if (!TestTrue(TEXT("both source identities retained"), Ordered[0].IsValid() && Ordered[1].IsValid())) return false;
        Original.Add(Name, Ordered);
    }
    auto Entries = [&](const FString& Name, TArray<TSharedPtr<FJsonValue>> Values) { Category->GetObjectField(Name)->GetObjectField(TEXT("value"))->SetArrayField(TEXT("entries"), Values); };
    FPreparedApply NoOp;
    if (!PrepareReflectedObjectRecord(*Target.Get(), Policy, Properties, NoOp, Error)) { AddError(Error); return false; }
    TestEqual(TEXT("ordinary projected Map keys are no-op"), NoOp.NumProperties(), 0);
    for (const auto& Pair : Original) Entries(Pair.Key, {Pair.Value[1], Pair.Value[0]});
    if (!PrepareReflectedObjectRecord(*Target.Get(), Policy, Properties, NoOp, Error)) { AddError(Error); return false; }
    TestEqual(TEXT("pure Map reorder is no-op"), NoOp.NumProperties(), 0);
    auto RoundTrip = [&]() {
        FPreparedApply Prepared; FApplyResult Result;
        if (!PrepareReflectedObjectRecord(*Target.Get(), Policy, Properties, Prepared, Error)) { AddError(Error); return false; }
        SameNative(Before, EmptyBefore, DynamicBefore);
        if (!ApplyPrepared(Prepared, Result)) { AddError(Result.ErrorCode); return false; }
        const bool Same = SameNative(Expected, EmptyExpected, DynamicExpected);
        TestTrue(TEXT("complete projected map verifies"), VerifyPrepared(Prepared, Result));
        if (!RollbackPrepared(Prepared, Result)) { AddError(Result.ErrorCode); return false; }
        SameNative(Before, EmptyBefore, DynamicBefore); return Same;
    };
    const auto MixedFirst = Original.FindChecked(TEXT("Records"))[0]->AsObject();
    const auto DynamicFirst = Original.FindChecked(TEXT("DynamicRecords"))[0]->AsObject();
    MixedFirst->GetObjectField(TEXT("key"))->GetObjectField(TEXT("fields"))->SetNumberField(TEXT("Count"), 57);
    DynamicFirst->GetObjectField(TEXT("key"))->GetObjectField(TEXT("fields"))->GetObjectField(TEXT("Payload"))->GetObjectField(TEXT("fields"))->SetNumberField(TEXT("Count"), 67);
    for (auto& Pair : Expected) if (Pair.Key.Identity == 811) Pair.Key.Count = 57;
    for (auto& Pair : DynamicExpected) if (Pair.Key.Key == TEXT("Alpha")) Pair.Key.Payload.GetMutable<FUeAgentMixedActionableSettings>().Count = 67;
    if (!RoundTrip()) return false;
    MixedFirst->GetObjectField(TEXT("key"))->GetObjectField(TEXT("fields"))->SetNumberField(TEXT("Count"), 10);
    DynamicFirst->GetObjectField(TEXT("key"))->GetObjectField(TEXT("fields"))->GetObjectField(TEXT("Payload"))->GetObjectField(TEXT("fields"))->SetNumberField(TEXT("Count"), 30);
    Expected = Before; DynamicExpected = DynamicBefore;
    MixedFirst->SetNumberField(TEXT("value"), 303); DynamicFirst->SetNumberField(TEXT("value"), 343);
    Expected.FindChecked(Mixed(811, 0, 0)) = 303; DynamicExpected.FindChecked(Dynamic(TEXT("Alpha"), 0, 0)) = 343;
    if (!RoundTrip()) return false;
    auto EmptyKey = MakeShared<FJsonObject>(); EmptyKey->SetObjectField(TEXT("fields"), MakeShared<FJsonObject>());
    auto NewEntry = MakeShared<FJsonObject>(); NewEntry->SetObjectField(TEXT("key"), EmptyKey); NewEntry->SetNumberField(TEXT("value"), 404); NewEntry->SetField(TEXT("from"), MakeShared<FJsonValueNull>());
    Entries(TEXT("EmptyRecords"), {Original.FindChecked(TEXT("EmptyRecords"))[1], MakeShared<FJsonValueObject>(NewEntry)});
    EmptyExpected.Remove(Empty(811, 0)); EmptyExpected.Add(Empty(7, 73), 404);
    if (!RoundTrip()) return false;
    Entries(TEXT("EmptyRecords"), {}); EmptyExpected.Empty();
    if (!RoundTrip()) return false;
    auto Reject = [&](const FString& Code) {
        FPreparedApply Prepared;
        TestFalse(TEXT("invalid source/key edit rejected"), PrepareReflectedObjectRecord(*Target.Get(), Policy, Properties, Prepared, Error));
        TestTrue(TEXT("rejection retains exact protocol reason"), Error.Contains(Code)); SameNative(Before, EmptyBefore, DynamicBefore);
    };
    const auto SourceFacts = MixedFirst->TryGetField(TEXT("from")); MixedFirst->RemoveField(TEXT("from")); Reject(TEXT("hash_source_required")); MixedFirst->SetField(TEXT("from"), SourceFacts);
    const auto Hidden = SourceFacts->AsObject()->GetObjectField(TEXT("fields"))->GetObjectField(TEXT("Empty"))->GetObjectField(TEXT("fields"))->GetObjectField(TEXT("Nested"))->GetObjectField(TEXT("fields"))->GetObjectField(TEXT("Kept"));
    Hidden->SetNumberField(TEXT("literal"), 999); Reject(TEXT("hash_source_facts_mismatch")); Hidden->SetNumberField(TEXT("literal"), 833);
    DynamicFirst->GetObjectField(TEXT("key"))->GetObjectField(TEXT("fields"))->SetStringField(TEXT("Key"), TEXT("ChangedIdentity")); Reject(TEXT("hash_identity_changed"));
    return !HasAnyErrors();
}
