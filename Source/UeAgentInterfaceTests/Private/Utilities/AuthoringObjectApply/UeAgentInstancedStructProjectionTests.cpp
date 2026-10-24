#include "Misc/AutomationTest.h"
#include "Utilities/AuthoringObjectApply/UeAgentObjectApplyNotificationTestTypes.h"
#include "Composition/HjsonImport/UeAgentReflectedObjectRecord.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentInstancedStructProjectionTest,
    "UAI.Core.Authoring.InstancedStructActionableProjection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentInstancedStructProjectionTest::RunTest(const FString&)
{
    using namespace UE::UAI::Composition::HjsonImport;
    using namespace UeAgentAuthoringObjectApply;
    TStrongObjectPtr<UUeAgentInstancedStructProjectionTarget> Target(NewObject<UUeAgentInstancedStructProjectionTarget>());
    Target->Current.InitializeAs<FUeAgentMixedActionableSettings>();
    Target->Current.GetMutable<FUeAgentMixedActionableSettings>().Count = 10;
    Target->Current.GetMutable<FUeAgentMixedActionableSettings>().Empty.Nested.Kept = 811;
    Target->Empty.InitializeAs<FUeAgentEmptyNestedSettings>();
    Target->Empty.GetMutable<FUeAgentEmptyNestedSettings>().Nested.Kept = 922;
    const FInstancedStruct BeforeCurrent = Target->Current, BeforeEmpty = Target->Empty;
    auto SameNative = [&](const FInstancedStruct& Current, const FInstancedStruct& Empty) {
        bool Same = true;
        for (const FName Name : {FName(TEXT("Current")), FName(TEXT("Empty"))}) {
            const auto& Actual = Name == TEXT("Current") ? Target->Current : Target->Empty;
            const auto& Expected = Name == TEXT("Current") ? Current : Empty;
            if (!TestTrue(TEXT("native dynamic type matches independent expected"), Actual.GetScriptStruct() == Expected.GetScriptStruct())) { Same = false; continue; }
            if (Expected.GetScriptStruct())
                Same &= TestTrue(TEXT("complete native memory including hidden descendants matches"),
                    Expected.GetScriptStruct()->CompareScriptStruct(Actual.GetMemory(), Expected.GetMemory(), PPF_None));
        }
        return Same;
    };
    UeAgentDetailsAuthoringService::FCapturePolicy Policy;
    TSharedPtr<FJsonObject> Properties, Definitions;
    FString Error;
    auto Capture = [&]() {
        if (!CaptureReflectedObjectRecord(*Target.Get(), Policy, Properties, Definitions, Error)) { AddError(Error); return false; }
        return true;
    };
    if (!Capture()) return false;
    auto Category = Properties->GetObjectField(TEXT("Test"));
    const FString CurrentReference = Category->GetObjectField(TEXT("Current"))->GetStringField(TEXT("definition"));
    const FString EmptyReference = Category->GetObjectField(TEXT("Empty"))->GetStringField(TEXT("definition"));
    TestEqual(TEXT("different dynamic definitions retain the same native property type"),
        Category->GetObjectField(TEXT("Current"))->GetStringField(TEXT("type")),
        Category->GetObjectField(TEXT("Empty"))->GetStringField(TEXT("type")));
    TestNotEqual(TEXT("different dynamic definitions have unambiguous readable references"), CurrentReference, EmptyReference);
    TStrongObjectPtr<UUeAgentInstancedStructProjectionTarget> Reordered(NewObject<UUeAgentInstancedStructProjectionTarget>());
    Reordered->Current = BeforeEmpty; Reordered->Empty = BeforeCurrent;
    TSharedPtr<FJsonObject> OtherProperties, OtherDefinitions;
    if (!CaptureReflectedObjectRecord(*Reordered.Get(), Policy, OtherProperties, OtherDefinitions, Error)) { AddError(Error); return false; }
    TestEqual(TEXT("definition enumeration order cannot change the mixed reference"),
        OtherProperties->GetObjectField(TEXT("Test"))->GetObjectField(TEXT("Empty"))->GetStringField(TEXT("definition")), CurrentReference);
    TestEqual(TEXT("definition enumeration order cannot change the empty reference"),
        OtherProperties->GetObjectField(TEXT("Test"))->GetObjectField(TEXT("Current"))->GetStringField(TEXT("definition")), EmptyReference);
    Reordered->Current = BeforeCurrent;
    if (!CaptureReflectedObjectRecord(*Reordered.Get(), Policy, OtherProperties, OtherDefinitions, Error)) { AddError(Error); return false; }
    TestEqual(TEXT("identical complete definitions share a readable reference"),
        OtherProperties->GetObjectField(TEXT("Test"))->GetObjectField(TEXT("Current"))->GetStringField(TEXT("definition")),
        OtherProperties->GetObjectField(TEXT("Test"))->GetObjectField(TEXT("Empty"))->GetStringField(TEXT("definition")));
    for (const FString Name : {FString(TEXT("Current")), FString(TEXT("Empty"))}) {
        const auto Row = Category->GetObjectField(Name);
        if (!TestTrue(TEXT("ordinary dynamic value retains object shape"), Row->HasTypedField<EJson::Object>(TEXT("value")))) return false;
        const auto Value = Row->GetObjectField(TEXT("value"));
        TestEqual(TEXT("dynamic selector remains explicit"), Value->GetStringField(TEXT("struct_path")),
            (Name == TEXT("Current") ? BeforeCurrent : BeforeEmpty).GetScriptStruct()->GetPathName());
        const auto Fields = Value->GetObjectField(TEXT("fields"));
        const auto Schema = Definitions->GetObjectField(Row->GetStringField(TEXT("definition")))->GetObjectField(TEXT("value_definition"))->GetObjectField(TEXT("fields"));
        TestEqual(TEXT("only actionable fields or necessary empty shell remain"), Fields->Values.Num(), Name == TEXT("Current") ? 1 : 0);
        TestEqual(TEXT("dynamic definition matches projected fields"), Schema->Values.Num(), Fields->Values.Num());
        TestFalse(TEXT("no-op descendant omitted from dynamic value"), Fields->HasField(Name == TEXT("Current") ? TEXT("Empty") : TEXT("Nested")));
        if (Name == TEXT("Current")) TestTrue(TEXT("dynamic editable field retains its type definition"), Schema->GetObjectField(TEXT("Count"))->HasTypedField<EJson::Object>(TEXT("type")));
    }
    auto NoOp = [&]() {
        FPreparedApply Prepared;
        if (!PrepareReflectedObjectRecord(*Target.Get(), Policy, Properties, Prepared, Error)) { AddError(Error); return false; }
        return TestEqual(TEXT("ordinary dynamic export is a no-op"), Prepared.NumProperties(), 0);
    };
    if (!NoOp() || !SameNative(BeforeCurrent, BeforeEmpty)) return false;
    Category->GetObjectField(TEXT("Current"))->GetObjectField(TEXT("value"))->GetObjectField(TEXT("fields"))->SetNumberField(TEXT("Count"), 57);
    FInstancedStruct ExpectedCurrent = BeforeCurrent;
    ExpectedCurrent.GetMutable<FUeAgentMixedActionableSettings>().Count = 57;
    auto RoundTrip = [&](const FInstancedStruct& Current, const FInstancedStruct& Empty) {
        FPreparedApply Prepared; FApplyResult Result;
        if (!PrepareReflectedObjectRecord(*Target.Get(), Policy, Properties, Prepared, Error)) { AddError(Error); return false; }
        SameNative(BeforeCurrent, BeforeEmpty);
        if (!ApplyPrepared(Prepared, Result)) { AddError(Result.ErrorCode); return false; }
        const bool Same = SameNative(Current, Empty);
        TestTrue(TEXT("dynamic result verifies"), VerifyPrepared(Prepared, Result));
        if (!RollbackPrepared(Prepared, Result)) { AddError(Result.ErrorCode); return false; }
        return SameNative(BeforeCurrent, BeforeEmpty) && Same;
    };
    if (!RoundTrip(ExpectedCurrent, BeforeEmpty)) return false;
    auto DynamicValue = [](UScriptStruct* Type, TOptional<int32> Count = {}) {
        auto Value = MakeShared<FJsonObject>();
        if (Type) Value->SetStringField(TEXT("struct_path"), Type->GetPathName());
        else Value->SetField(TEXT("struct_path"), MakeShared<FJsonValueNull>());
        auto Fields = MakeShared<FJsonObject>();
        if (Count.IsSet()) Fields->SetNumberField(TEXT("Count"), Count.GetValue());
        Value->SetObjectField(TEXT("fields"), Fields);
        return Value;
    };
    Category->GetObjectField(TEXT("Current"))->GetObjectField(TEXT("value"))->GetObjectField(TEXT("fields"))->SetNumberField(TEXT("Count"), 10);
    Category->GetObjectField(TEXT("Empty"))->SetObjectField(TEXT("value"), DynamicValue(FUeAgentMixedActionableSettings::StaticStruct(), 68));
    FInstancedStruct IndependentlyEditedEmpty;
    IndependentlyEditedEmpty.InitializeAs<FUeAgentMixedActionableSettings>();
    IndependentlyEditedEmpty.GetMutable<FUeAgentMixedActionableSettings>().Count = 68;
    if (!RoundTrip(BeforeCurrent, IndependentlyEditedEmpty)) return false;
    Category->GetObjectField(TEXT("Current"))->SetObjectField(TEXT("value"), DynamicValue(FUeAgentEmptyNestedSettings::StaticStruct()));
    Category->GetObjectField(TEXT("Empty"))->SetObjectField(TEXT("value"), DynamicValue(FUeAgentMixedActionableSettings::StaticStruct()));
    FInstancedStruct DefaultEmpty, DefaultMixed;
    DefaultEmpty.InitializeAs<FUeAgentEmptyNestedSettings>(); DefaultMixed.InitializeAs<FUeAgentMixedActionableSettings>();
    if (!RoundTrip(DefaultEmpty, DefaultMixed)) return false;
    Category->GetObjectField(TEXT("Current"))->SetObjectField(TEXT("value"), DynamicValue(nullptr));
    Category->GetObjectField(TEXT("Empty"))->SetObjectField(TEXT("value"), DynamicValue(nullptr));
    FPreparedApply Clear; FApplyResult Result;
    if (!PrepareReflectedObjectRecord(*Target.Get(), Policy, Properties, Clear, Error)) { AddError(Error); return false; }
    SameNative(BeforeCurrent, BeforeEmpty);
    if (!ApplyPrepared(Clear, Result)) { AddError(Result.ErrorCode); return false; }
    FInstancedStruct Unset;
    SameNative(Unset, Unset);
    TestTrue(TEXT("cleared dynamic value verifies"), VerifyPrepared(Clear, Result));
    if (!Capture() || !NoOp()) return false;
    Category = Properties->GetObjectField(TEXT("Test"));
    for (const FString Name : {FString(TEXT("Current")), FString(TEXT("Empty"))}) {
        const auto Value = Category->GetObjectField(Name)->GetObjectField(TEXT("value"));
        TestTrue(TEXT("cleared ordinary selector is null"), Value->HasTypedField<EJson::Null>(TEXT("struct_path")));
        TestEqual(TEXT("clear retains fields shell"), Value->GetObjectField(TEXT("fields"))->Values.Num(), 0);
    }
    Category->GetObjectField(TEXT("Current"))->SetObjectField(TEXT("value"), DynamicValue(FUeAgentMixedActionableSettings::StaticStruct(), 68));
    Category->GetObjectField(TEXT("Empty"))->SetObjectField(TEXT("value"), DynamicValue(FUeAgentEmptyNestedSettings::StaticStruct()));
    FPreparedApply SetAgain;
    if (!PrepareReflectedObjectRecord(*Target.Get(), Policy, Properties, SetAgain, Error)) { AddError(Error); return false; }
    SameNative(Unset, Unset);
    if (!ApplyPrepared(SetAgain, Result)) { AddError(Result.ErrorCode); return false; }
    DefaultMixed.GetMutable<FUeAgentMixedActionableSettings>().Count = 68;
    SameNative(DefaultMixed, DefaultEmpty);
    TestTrue(TEXT("set after clear uses initialized defaults and verifies"), VerifyPrepared(SetAgain, Result));
    if (!RollbackPrepared(SetAgain, Result)) { AddError(Result.ErrorCode); return false; }
    SameNative(Unset, Unset);
    if (!RollbackPrepared(Clear, Result)) { AddError(Result.ErrorCode); return false; }
    SameNative(BeforeCurrent, BeforeEmpty);
    return true;
}
