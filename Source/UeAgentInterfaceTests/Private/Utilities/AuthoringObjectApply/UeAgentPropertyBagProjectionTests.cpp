#include "Misc/AutomationTest.h"
#include "Utilities/AuthoringObjectApply/UeAgentObjectApplyNotificationTestTypes.h"
#include "Composition/HjsonImport/UeAgentReflectedObjectRecord.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentPropertyBagProjectionTest,
    "UAI.Core.Authoring.PropertyBagActionableProjection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentPropertyBagProjectionTest::RunTest(const FString&)
{
    using namespace UE::UAI::Composition::HjsonImport;
    using namespace UeAgentAuthoringObjectApply;
    TStrongObjectPtr<UUeAgentPropertyBagProjectionTarget> Target(NewObject<UUeAgentPropertyBagProjectionTarget>());
    FPropertyBagPropertyDesc MixedDesc(TEXT("Mixed"), EPropertyBagPropertyType::Struct, FUeAgentMixedActionableSettings::StaticStruct());
    FPropertyBagPropertyDesc EmptyDesc(TEXT("Empty"), EPropertyBagPropertyType::Struct, FUeAgentEmptyNestedSettings::StaticStruct());
    MixedDesc.ID = FGuid::NewGuid(); EmptyDesc.ID = FGuid::NewGuid();
    MixedDesc.PropertyFlags |= CPF_AdvancedDisplay;
#if WITH_EDITORONLY_DATA
    MixedDesc.MetaData.Emplace(TEXT("ProjectionEvidence"), TEXT("mixed-native"));
    EmptyDesc.MetaData.Emplace(TEXT("ProjectionEvidence"), TEXT("empty-native"));
#endif
    Target->Bag.AddProperties({MixedDesc, EmptyDesc});
    FUeAgentMixedActionableSettings Mixed; Mixed.Count = 10; Mixed.Empty.Nested.Kept = 811;
    FUeAgentEmptyNestedSettings Empty; Empty.Nested.Kept = 922;
    Target->Bag.SetValueStruct(TEXT("Mixed"), Mixed); Target->Bag.SetValueStruct(TEXT("Empty"), Empty);
    const FInstancedPropertyBag Before = Target->Bag;
    MixedDesc = *Before.FindPropertyDescByName(TEXT("Mixed")); EmptyDesc = *Before.FindPropertyDescByName(TEXT("Empty"));
    auto SameNative = [&](const FInstancedPropertyBag& Expected, const FInstancedPropertyBag* IdentityBase) {
        bool Same = TestEqual(TEXT("native bag descriptor count"), Target->Bag.GetNumPropertiesInBag(), Expected.GetNumPropertiesInBag());
        if (!Expected.GetPropertyBagStruct()) return Same && TestFalse(TEXT("cleared native bag has no schema"), Target->Bag.IsValid());
        const auto& Descs = Expected.GetPropertyBagStruct()->GetPropertyDescs();
        for (const auto& Desc : Descs) {
            const auto* Actual = Target->Bag.FindPropertyDescByName(Desc.Name);
            if (!TestNotNull(TEXT("native schema retains expected field"), Actual)) { Same = false; continue; }
            Same &= TestTrue(TEXT("native descriptor complete type"), Actual->ValueType == Desc.ValueType && Actual->ValueTypeObject == Desc.ValueTypeObject && Actual->ContainerTypes == Desc.ContainerTypes);
            Same &= TestEqual(TEXT("native descriptor flags"), Actual->PropertyFlags, Desc.PropertyFlags);
            const auto* Existing = IdentityBase ? IdentityBase->FindPropertyDescByName(Desc.Name) : nullptr;
            Same &= Existing ? TestTrue(TEXT("retained name keeps native ID"), Actual->ID == Existing->ID) : TestTrue(TEXT("new native ID is allocated"), Actual->ID.IsValid());
#if WITH_EDITORONLY_DATA
            Same &= TestTrue(TEXT("native descriptor metadata"), Actual->MetaData == Desc.MetaData && Actual->MetaClass == Desc.MetaClass);
#endif
            if (Actual->CachedProperty && Desc.CachedProperty && Actual->CachedProperty->SameType(Desc.CachedProperty))
                Same &= TestTrue(TEXT("complete native field including hidden descendants"), Actual->CachedProperty->Identical(
                    Actual->CachedProperty->ContainerPtrToValuePtr<void>(Target->Bag.GetValue().GetMemory()),
                    Desc.CachedProperty->ContainerPtrToValuePtr<void>(Expected.GetValue().GetMemory()), PPF_None));
            else { AddError(TEXT("native cached field types differ")); Same = false; }
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
    auto Row = Properties->GetObjectField(TEXT("Test"))->GetObjectField(TEXT("Bag"));
    if (!TestTrue(TEXT("ordinary bag retains schema/fields object"), Row->HasTypedField<EJson::Object>(TEXT("value")))) return false;
    auto Bag = Row->GetObjectField(TEXT("value"));
    const auto& Schema = Bag->GetArrayField(TEXT("schema"));
    TestEqual(TEXT("projection keeps both schema entries"), Schema.Num(), 2);
    const auto Fields = Bag->GetObjectField(TEXT("fields"));
    const auto FieldDefinitions = Definitions->GetObjectField(Row->GetStringField(TEXT("definition")))->GetObjectField(TEXT("value_definition"))->GetObjectField(TEXT("fields"));
    TestEqual(TEXT("all bag inventory fields remain present"), Fields->Values.Num(), 2);
    TestEqual(TEXT("all bag inventory definitions remain present"), FieldDefinitions->Values.Num(), 2);
    for (const FString Name : {FString(TEXT("Mixed")), FString(TEXT("Empty"))}) {
        const auto Members = Fields->GetObjectField(Name)->GetObjectField(TEXT("fields"));
        const auto MemberDefinitions = FieldDefinitions->GetObjectField(Name)->GetObjectField(TEXT("value_definition"))->GetObjectField(TEXT("fields"));
        TestEqual(TEXT("bag member recursively retains actionable fields or empty shell"), Members->Values.Num(), Name == TEXT("Mixed") ? 1 : 0);
        TestEqual(TEXT("bag field definition matches projected value"), MemberDefinitions->Values.Num(), Members->Values.Num());
    }
    TMap<FString, TSharedPtr<FJsonValue>> ByName;
    for (const auto& Desc : Schema) ByName.Add(Desc->AsObject()->GetStringField(TEXT("name")), Desc);
    if (!TestTrue(TEXT("ordinary schema has expected names"), ByName.Contains(TEXT("Mixed")) && ByName.Contains(TEXT("Empty")))) return false;
    auto NoOp = [&]() {
        FPreparedApply Prepared;
        if (!PrepareReflectedObjectRecord(*Target.Get(), Policy, Properties, Prepared, Error)) { AddError(Error); return false; }
        return TestEqual(TEXT("ordinary bag export is no-op"), Prepared.NumProperties(), 0);
    };
    if (!NoOp() || !SameNative(Before, &Before)) return false;
    auto StructValue = [](TOptional<int32> Count = {}) {
        auto Value = MakeShared<FJsonObject>(); auto Members = MakeShared<FJsonObject>();
        if (Count.IsSet()) Members->SetNumberField(TEXT("Count"), Count.GetValue());
        Value->SetObjectField(TEXT("fields"), Members); return MakeShared<FJsonValueObject>(Value);
    };
    auto SetInput = [&](const TArray<TSharedPtr<FJsonValue>>& DesiredSchema, const TSharedPtr<FJsonObject>& DesiredFields) {
        auto Value = MakeShared<FJsonObject>(); Value->SetArrayField(TEXT("schema"), DesiredSchema); Value->SetObjectField(TEXT("fields"), DesiredFields);
        Row->SetObjectField(TEXT("value"), Value);
    };
    auto DesiredFields = MakeShared<FJsonObject>(); DesiredFields->SetField(TEXT("Mixed"), StructValue(57)); DesiredFields->SetField(TEXT("Empty"), StructValue());
    SetInput(Schema, DesiredFields);
    FInstancedPropertyBag Edited = Before;
    auto EditedMixed = Mixed; EditedMixed.Count = 57; Edited.SetValueStruct(TEXT("Mixed"), EditedMixed);
    auto RoundTrip = [&](const FInstancedPropertyBag& Expected) {
        FPreparedApply Prepared; FApplyResult Result;
        if (!PrepareReflectedObjectRecord(*Target.Get(), Policy, Properties, Prepared, Error)) { AddError(Error); return false; }
        SameNative(Before, &Before);
        if (!ApplyPrepared(Prepared, Result)) { AddError(Result.ErrorCode); return false; }
        const bool Same = SameNative(Expected, &Before);
        TestTrue(TEXT("complete bag verifies"), VerifyPrepared(Prepared, Result));
        if (!RollbackPrepared(Prepared, Result)) { AddError(Result.ErrorCode); return false; }
        return SameNative(Before, &Before) && Same;
    };
    if (!RoundTrip(Edited)) return false;
    SetInput({ByName.FindChecked(TEXT("Empty")), ByName.FindChecked(TEXT("Mixed"))}, DesiredFields);
    if (!RoundTrip(Edited)) return false;
    auto AddedSchema = MakeShared<FJsonObject>(); AddedSchema->Values = ByName.FindChecked(TEXT("Mixed"))->AsObject()->Values; AddedSchema->SetStringField(TEXT("name"), TEXT("Added"));
    DesiredFields->SetField(TEXT("Added"), StructValue(68));
    SetInput({ByName.FindChecked(TEXT("Empty")), MakeShared<FJsonValueObject>(AddedSchema), ByName.FindChecked(TEXT("Mixed"))}, DesiredFields);
    FInstancedPropertyBag Added = Edited; Added.AddProperty(TEXT("Added"), EPropertyBagPropertyType::Struct, FUeAgentMixedActionableSettings::StaticStruct());
    FUeAgentMixedActionableSettings AddedMixed; AddedMixed.Count = 68; Added.SetValueStruct(TEXT("Added"), AddedMixed);
    if (!RoundTrip(Added)) return false;
    DesiredFields->RemoveField(TEXT("Added")); DesiredFields->RemoveField(TEXT("Empty"));
    SetInput({ByName.FindChecked(TEXT("Mixed"))}, DesiredFields);
    FInstancedPropertyBag Removed; Removed.AddProperties({MixedDesc}); Removed.SetValueStruct(TEXT("Mixed"), EditedMixed);
    if (!RoundTrip(Removed)) return false;
    auto ChangedSchema = MakeShared<FJsonObject>(); ChangedSchema->Values = ByName.FindChecked(TEXT("Empty"))->AsObject()->Values; ChangedSchema->SetStringField(TEXT("name"), TEXT("Mixed"));
    DesiredFields->SetField(TEXT("Mixed"), StructValue()); DesiredFields->SetField(TEXT("Empty"), StructValue());
    SetInput({MakeShared<FJsonValueObject>(ChangedSchema), ByName.FindChecked(TEXT("Empty"))}, DesiredFields);
    auto ChangedDesc = MixedDesc; ChangedDesc.ValueTypeObject = FUeAgentEmptyNestedSettings::StaticStruct();
    FInstancedPropertyBag Changed; Changed.AddProperties({ChangedDesc, EmptyDesc}); Changed.SetValueStruct(TEXT("Empty"), Empty);
    if (!RoundTrip(Changed)) return false;
    SetInput({}, MakeShared<FJsonObject>());
    FPreparedApply Clear; FApplyResult Result;
    if (!PrepareReflectedObjectRecord(*Target.Get(), Policy, Properties, Clear, Error)) { AddError(Error); return false; }
    SameNative(Before, &Before);
    if (!ApplyPrepared(Clear, Result)) { AddError(Result.ErrorCode); return false; }
    FInstancedPropertyBag Unset;
    SameNative(Unset, nullptr); TestTrue(TEXT("cleared bag verifies"), VerifyPrepared(Clear, Result));
    if (!Capture() || !NoOp()) return false;
    Row = Properties->GetObjectField(TEXT("Test"))->GetObjectField(TEXT("Bag"));
    auto ResetFields = MakeShared<FJsonObject>(); ResetFields->SetField(TEXT("Mixed"), StructValue(69)); ResetFields->SetField(TEXT("Empty"), StructValue());
    SetInput({ByName.FindChecked(TEXT("Mixed")), ByName.FindChecked(TEXT("Empty"))}, ResetFields);
    FPreparedApply SetAgain;
    if (!PrepareReflectedObjectRecord(*Target.Get(), Policy, Properties, SetAgain, Error)) { AddError(Error); return false; }
    SameNative(Unset, nullptr);
    if (!ApplyPrepared(SetAgain, Result)) { AddError(Result.ErrorCode); return false; }
    FInstancedPropertyBag Reset;
    Reset.AddProperty(TEXT("Mixed"), EPropertyBagPropertyType::Struct, FUeAgentMixedActionableSettings::StaticStruct());
    Reset.AddProperty(TEXT("Empty"), EPropertyBagPropertyType::Struct, FUeAgentEmptyNestedSettings::StaticStruct());
    FUeAgentMixedActionableSettings ResetMixed; ResetMixed.Count = 69; Reset.SetValueStruct(TEXT("Mixed"), ResetMixed);
    SameNative(Reset, nullptr); TestTrue(TEXT("bag set after clear verifies"), VerifyPrepared(SetAgain, Result));
    if (!RollbackPrepared(SetAgain, Result)) { AddError(Result.ErrorCode); return false; }
    SameNative(Unset, nullptr);
    if (!RollbackPrepared(Clear, Result)) { AddError(Result.ErrorCode); return false; }
    SameNative(Before, &Before);
    return true;
}
