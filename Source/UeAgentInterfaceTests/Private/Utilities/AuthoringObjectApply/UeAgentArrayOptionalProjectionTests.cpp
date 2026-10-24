#include "Misc/AutomationTest.h"
#include "Utilities/AuthoringObjectApply/UeAgentObjectApplyNotificationTestTypes.h"
#include "Composition/HjsonImport/UeAgentReflectedObjectRecord.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentArrayOptionalProjectionTest,
    "UAI.Core.Authoring.ArrayOptionalActionableProjection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentArrayOptionalProjectionTest::RunTest(const FString&)
{
    using namespace UE::UAI::Composition::HjsonImport;
    using namespace UeAgentAuthoringObjectApply;
    TStrongObjectPtr<UUeAgentArrayOptionalProjectionTarget> Target(NewObject<UUeAgentArrayOptionalProjectionTarget>());
    Target->Records.SetNum(2); Target->EmptyRecords.SetNum(2);
    Target->Records[0].Count = 10; Target->Records[0].Empty.Nested.Kept = 811;
    Target->Records[1].Count = 20; Target->Records[1].Empty.Nested.Kept = 922;
    Target->EmptyRecords[0].Nested.Kept = 833; Target->EmptyRecords[1].Nested.Kept = 944;
    Target->Current.Emplace(); Target->Current->Count = 30; Target->Current->Empty.Nested.Kept = 955;
    Target->EmptyCurrent.Emplace(); Target->EmptyCurrent->Nested.Kept = 966;
    TStrongObjectPtr<UUeAgentArrayOptionalProjectionTarget> Before(DuplicateObject(Target.Get(), GetTransientPackage()));
    TStrongObjectPtr<UUeAgentArrayOptionalProjectionTarget> Expected(DuplicateObject(Target.Get(), GetTransientPackage()));
    auto SameNative = [&](const UUeAgentArrayOptionalProjectionTarget* Desired) {
        bool bSame = true;
        for (TFieldIterator<FProperty> It(Target->GetClass()); It; ++It)
            bSame &= TestTrue(*(TEXT("complete native ") + It->GetName()), It->Identical_InContainer(Target.Get(), Desired));
        return bSame;
    };
    UeAgentDetailsAuthoringService::FCapturePolicy Policy;
    TSharedPtr<FJsonObject> Properties, Definitions;
    FString Error;
    if (!CaptureReflectedObjectRecord(*Target.Get(), Policy, Properties, Definitions, Error)) { AddError(Error); return false; }
    SameNative(Before.Get());
    auto Category = Properties->GetObjectField(TEXT("Test"));
    auto CheckFields = [&](const TSharedPtr<FJsonValue>& Value, const TSharedPtr<FJsonObject>& Definition, bool bMixed) {
        const auto Fields = Value->AsObject()->GetObjectField(TEXT("fields"));
        const auto FieldDefinitions = Definition->GetObjectField(TEXT("fields"));
        TestEqual(TEXT("value retains only actionable fields or the empty shell"), Fields->Values.Num(), bMixed ? 1 : 0);
        TestEqual(TEXT("paired definition has exactly the visible fields"), FieldDefinitions->Values.Num(), Fields->Values.Num());
        TestFalse(TEXT("empty descendant is absent from value"), Fields->HasField(bMixed ? TEXT("Empty") : TEXT("Nested")));
        TestFalse(TEXT("empty descendant is absent from definition"), FieldDefinitions->HasField(bMixed ? TEXT("Empty") : TEXT("Nested")));
        if (bMixed) TestTrue(TEXT("Count keeps its ordinary type"), FieldDefinitions->GetObjectField(TEXT("Count"))->HasTypedField<EJson::Object>(TEXT("type")));
    };
    for (const FString Name : {FString(TEXT("Records")), FString(TEXT("EmptyRecords"))}) {
        const bool bMixed = Name == TEXT("Records");
        const auto Row = Category->GetObjectField(Name);
        const auto& Items = Row->GetArrayField(TEXT("value"));
        const auto& Schemas = Definitions->GetObjectField(Row->GetStringField(TEXT("definition")))->GetObjectField(TEXT("value_definition"))->GetArrayField(TEXT("items"));
        if (!TestEqual(TEXT("ordinary array retains its count"), Items.Num(), 2)
            || !TestEqual(TEXT("array definitions remain paired"), Schemas.Num(), Items.Num())) return false;
        for (int32 Index = 0; Index < Items.Num(); ++Index) {
            CheckFields(Items[Index], Schemas[Index]->AsObject(), bMixed);
            if (bMixed) TestEqual(TEXT("ordinary array retains order"), Items[Index]->AsObject()->GetObjectField(TEXT("fields"))->GetNumberField(TEXT("Count")),
                static_cast<double>(Before->Records[Index].Count));
        }
    }
    for (const FString Name : {FString(TEXT("Current")), FString(TEXT("EmptyCurrent"))}) {
        const auto Row = Category->GetObjectField(Name);
        const auto Value = Row->GetObjectField(TEXT("value"));
        TestTrue(TEXT("set optional retains presence"), Value->GetBoolField(TEXT("is_set")));
        const auto Schema = Definitions->GetObjectField(Row->GetStringField(TEXT("definition")))->GetObjectField(TEXT("value_definition"))
            ->GetObjectField(TEXT("fields"))->GetObjectField(TEXT("value"));
        CheckFields(Value->TryGetField(TEXT("value")), Schema, Name == TEXT("Current"));
    }
    FPreparedApply Unchanged;
    if (!PrepareReflectedObjectRecord(*Target.Get(), Policy, Properties, Unchanged, Error)) { AddError(Error); return false; }
    TestEqual(TEXT("ordinary projected array and optionals prepare no edits"), Unchanged.NumProperties(), 0);
    SameNative(Before.Get());
    auto Items = Category->GetObjectField(TEXT("Records"))->GetArrayField(TEXT("value"));
    auto EmptyItems = Category->GetObjectField(TEXT("EmptyRecords"))->GetArrayField(TEXT("value"));
    Items[0]->AsObject()->GetObjectField(TEXT("fields"))->SetNumberField(TEXT("Count"), 57);
    Category->GetObjectField(TEXT("Current"))->GetObjectField(TEXT("value"))->GetObjectField(TEXT("value"))->GetObjectField(TEXT("fields"))->SetNumberField(TEXT("Count"), 58);
    Expected->Records[0].Count = 57; Expected->Current->Count = 58;
    auto RoundTrip = [&]() {
        FPreparedApply Prepared; FApplyResult Result;
        if (!PrepareReflectedObjectRecord(*Target.Get(), Policy, Properties, Prepared, Error)) { AddError(Error); return false; }
        SameNative(Before.Get());
        if (!ApplyPrepared(Prepared, Result)) { AddError(Result.ErrorCode); return false; }
        const bool bMatches = SameNative(Expected.Get());
        TestTrue(TEXT("completed array and optional native values verify"), VerifyPrepared(Prepared, Result));
        if (!RollbackPrepared(Prepared, Result)) { AddError(Result.ErrorCode); return false; }
        SameNative(Before.Get());
        return bMatches;
    };
    if (!RoundTrip()) return false;
    auto StructValue = [](bool bMixed) {
        auto Fields = MakeShared<FJsonObject>(); if (bMixed) Fields->SetNumberField(TEXT("Count"), 68);
        auto Value = MakeShared<FJsonObject>(); Value->SetObjectField(TEXT("fields"), Fields);
        return MakeShared<FJsonValueObject>(Value);
    };
    Items.Add(StructValue(true)); EmptyItems.Add(StructValue(false));
    Category->GetObjectField(TEXT("Records"))->SetArrayField(TEXT("value"), Items);
    Category->GetObjectField(TEXT("EmptyRecords"))->SetArrayField(TEXT("value"), EmptyItems);
    Expected->Records.AddDefaulted_GetRef().Count = 68; Expected->EmptyRecords.AddDefaulted();
    if (!RoundTrip()) return false;
    // Tail removal keeps the original index-zero base; it does not assert any
    // stable identity for values shifted by deleting an earlier array element.
    Items.SetNum(1); EmptyItems.SetNum(1);
    Category->GetObjectField(TEXT("Records"))->SetArrayField(TEXT("value"), Items);
    Category->GetObjectField(TEXT("EmptyRecords"))->SetArrayField(TEXT("value"), EmptyItems);
    Expected->Records.SetNum(1); Expected->EmptyRecords.SetNum(1);
    for (const FString Name : {FString(TEXT("Current")), FString(TEXT("EmptyCurrent"))}) {
        const auto Value = Category->GetObjectField(Name)->GetObjectField(TEXT("value"));
        Value->SetBoolField(TEXT("is_set"), false); Value->RemoveField(TEXT("value"));
    }
    Expected->Current.Reset(); Expected->EmptyCurrent.Reset();
    if (!RoundTrip()) return false;
    Category->GetObjectField(TEXT("Current"))->GetObjectField(TEXT("value"))->SetField(TEXT("value"), StructValue(true));
    FPreparedApply Rejected;
    TestFalse(TEXT("explicit unset optional with a value is rejected"),
        PrepareReflectedObjectRecord(*Target.Get(), Policy, Properties, Rejected, Error));
    TestTrue(TEXT("native optional contradiction policy remains authoritative"), Error.Contains(TEXT("optional_unset_has_value")));
    SameNative(Before.Get());
    // Re-capture the same properties from a genuinely unset native baseline.
    Target->Current.Reset(); Target->EmptyCurrent.Reset();
    Before->Current.Reset(); Before->EmptyCurrent.Reset();
    Expected->Records = Before->Records; Expected->EmptyRecords = Before->EmptyRecords;
    if (!CaptureReflectedObjectRecord(*Target.Get(), Policy, Properties, Definitions, Error)) { AddError(Error); return false; }
    Category = Properties->GetObjectField(TEXT("Test"));
    FPreparedApply UnsetUnchanged;
    if (!PrepareReflectedObjectRecord(*Target.Get(), Policy, Properties, UnsetUnchanged, Error)) { AddError(Error); return false; }
    TestEqual(TEXT("unset ordinary input also prepares no edits"), UnsetUnchanged.NumProperties(), 0);
    for (const FString Name : {FString(TEXT("Current")), FString(TEXT("EmptyCurrent"))}) {
        const auto Value = Category->GetObjectField(Name)->GetObjectField(TEXT("value"));
        TestFalse(TEXT("unset optional retains presence"), Value->GetBoolField(TEXT("is_set")));
        TestFalse(TEXT("unset optional has no invented value"), Value->HasField(TEXT("value")));
        Value->SetBoolField(TEXT("is_set"), true); Value->SetField(TEXT("value"), StructValue(Name == TEXT("Current")));
    }
    Expected->Current.Emplace().Count = 68; Expected->EmptyCurrent.Emplace();
    if (!RoundTrip()) return false;
    return !HasAnyErrors();
}
