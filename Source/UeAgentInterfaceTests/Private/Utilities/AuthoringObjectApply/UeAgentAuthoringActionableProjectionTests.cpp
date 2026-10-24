#include "Misc/AutomationTest.h"
#include "Utilities/AuthoringObjectApply/UeAgentObjectApplyNotificationTestTypes.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/UeAgentUObjectProperties.h"
#include "Composition/HjsonImport/UeAgentReflectedObjectRecord.h"
#include "UObject/StrongObjectPtr.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentActionableProjectionTest,
    "UAI.Core.Authoring.ActionableProjection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentActionableProjectionTest::RunTest(const FString&)
{
    auto* Target = NewObject<UUeAgentActionableProjectionTarget>();
    UeAgentAuthoringState::FEditablePropertyReadOptions Options;
    TArray<UeAgentAuthoringState::FReadRequest> Requests;
    UeAgentAuthoringState::FCaptureResult Capture;
    FString Error;
    if (!UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(Target, Options, Requests, Error)
        || !UeAgentAuthoringState::CaptureSnapshot(Requests, Capture)) {
        AddError(Error + Capture.Diagnostic.Code); return false;
    }
    const auto Entries = Capture.Snapshot.CopyEntries();
    TestEqual(TEXT("complete snapshot retains every qualified property"), Entries.Num(), 12);
    for (const auto& Entry : Entries) {
        const FString Name = Entry.TargetIdentity.GetSelector(TEXT("property_path"));
        const bool Expected = Name != TEXT("ReadOnly") && Name != TEXT("Nested") && Name != TEXT("FixedEmpty");
        TSharedPtr<FJsonValue> Value, Definition;
        const bool Actual = UeAgentPropertyValueCodec::ProjectAuthoringValue(
            Entry.Type.ToJsonObject(), Entry.Value, Entry.ValueDefinition, Value, Definition);
        TestEqual(*FString::Printf(TEXT("%s actionability follows schema"), *Name), Actual, Expected);
        UeAgentDetailsAuthoringService::FProjectionPolicy Policy;
        Policy.bRequireEditable = true;
        TestEqual(TEXT("Details consumes the same ordinary qualification"),
            UeAgentDetailsAuthoringService::BuildPropertyDescriptor(Entry, Policy).IsValid(), Expected);
        UeAgentUObjectProperties::FExportOptions ExportOptions;
        ExportOptions.Profile = UeAgentUObjectProperties::EExportProfile::Authoring;
        FProperty* Property = FindFProperty<FProperty>(Target->GetClass(), *Name);
        TestEqual(TEXT("UObject authoring consumes the same qualification"),
            UeAgentUObjectProperties::BuildPropertyDescriptor(Target, Property, ExportOptions).IsValid(), Expected);
        if (Name == TEXT("Mixed")) {
            TestTrue(TEXT("snapshot keeps nested no-leaf field"), Entry.Value->AsObject()->GetObjectField(TEXT("fields"))->HasField(TEXT("Empty")));
            TestFalse(TEXT("ordinary mixed value removes no-leaf field"), Value->AsObject()->GetObjectField(TEXT("fields"))->HasField(TEXT("Empty")));
            TestTrue(TEXT("ordinary mixed writable field remains"), Value->AsObject()->GetObjectField(TEXT("fields"))->HasField(TEXT("Count")));
        }
        if (Name == TEXT("FixedMixed")) {
            for (const auto& Item : Value->AsObject()->GetArrayField(TEXT("items")))
                TestFalse(TEXT("fixed element removes nested no-leaf member"), Item->AsObject()->GetObjectField(TEXT("fields"))->HasField(TEXT("Empty")));
        }
    }
    FProperty* ReadOnly = FindFProperty<FProperty>(Target->GetClass(), TEXT("ReadOnly"));
    UeAgentPropertyValueCodec::FOptions FullOptions;
    const auto Full = UeAgentPropertyValueCodec::Encode(ReadOnly, ReadOnly->ContainerPtrToValuePtr<void>(Target), Target, FullOptions);
    TestEqual(TEXT("full codec retains readonly native content"),
        Full->AsObject()->GetObjectField(TEXT("fields"))->GetObjectField(TEXT("Kept"))->GetNumberField(TEXT("literal")), 73.0);
    TestEqual(TEXT("native content remains untouched"), Target->Mixed.Empty.Nested.Kept, 73);
    // A caller filter can remove the last writable descendant without changing complete support.
    FProperty* Mixed = FindFProperty<FProperty>(Target->GetClass(), TEXT("Mixed"));
    UeAgentPropertyValueCodec::FOptions Filtered;
    Filtered.bCaptureStructFieldDescriptors = true;
    Filtered.StructFieldFilter = [](const UScriptStruct*, const FProperty* Field) {
        return UeAgentPropertyValueCodec::IsStructFieldAuthoringEditable(Field) && Field->GetName() != TEXT("Count");
    };
    TSharedPtr<FJsonObject> Schema;
    const auto Encoded = UeAgentPropertyValueCodec::Encode(Mixed, Mixed->ContainerPtrToValuePtr<void>(Target), Target, Filtered, 0, &Schema);
    TSharedPtr<FJsonValue> Projected, ProjectedSchema;
    TestFalse(TEXT("effective caller filter determines actionable descendants"), UeAgentPropertyValueCodec::ProjectAuthoringValue(
        UeAgentPropertyValueCodec::BuildTypeDescriptor(Mixed), Encoded, MakeShared<FJsonValueObject>(Schema), Projected, ProjectedSchema));
    TestTrue(TEXT("complete authoring support remains independent"), UeAgentPropertyValueCodec::IsAuthoringSupported(Mixed, Mixed->ContainerPtrToValuePtr<void>(Target), Error));
    UeAgentUObjectProperties::FExportOptions FilteredExport;
    FilteredExport.Profile = UeAgentUObjectProperties::EExportProfile::Authoring;
    FilteredExport.StructFieldFilter = [](const UScriptStruct*, const FProperty* Field) { return Field->GetName() != TEXT("Count"); };
    TestFalse(TEXT("UObject caller filter remains effective"), UeAgentUObjectProperties::BuildPropertyDescriptor(Target, Mixed, FilteredExport).IsValid());
    FilteredExport.Profile = UeAgentUObjectProperties::EExportProfile::Readback;
    TestTrue(TEXT("readback does not adopt ordinary visibility filtering"), UeAgentUObjectProperties::BuildPropertyDescriptor(Target, ReadOnly, FilteredExport).IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentOwnedActionableProjectionTest,
    "UAI.Core.Authoring.OwnedActionableProjection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentOwnedActionableProjectionTest::RunTest(const FString&)
{
    using namespace UE::UAI::Composition::HjsonImport;
    using namespace UeAgentAuthoringObjectApply;
    TStrongObjectPtr<UUeAgentOwnedActionableProjectionTarget> Root(NewObject<UUeAgentOwnedActionableProjectionTarget>());
    Root->Child = NewObject<UUeAgentActionableProjectionTarget>(Root.Get(), TEXT("ExistingChild"));
    auto* Child = Root->Child.Get();
    Child->ReadOnly.Kept = 811;
    Child->Nested.Nested.Kept = 812;
    Child->Mixed.Empty.Nested.Kept = 813;
    Child->Mixed.Count = 19;
    Child->FixedEmpty[1].Nested.Kept = 814;
    Child->FixedMixed[0].Empty.Nested.Kept = 815;
    TStrongObjectPtr<UUeAgentActionableProjectionTarget> Before(DuplicateObject(Child, GetTransientPackage(),
        MakeUniqueObjectName(GetTransientPackage(), Child->GetClass(), TEXT("BeforeOwnedProjection"))));
    TStrongObjectPtr<UUeAgentActionableProjectionTarget> Expected(DuplicateObject(Child, GetTransientPackage(),
        MakeUniqueObjectName(GetTransientPackage(), Child->GetClass(), TEXT("ExpectedOwnedProjection"))));
    Expected->Mixed.Count = 29;
    auto SameNative = [&](const UUeAgentActionableProjectionTarget* Desired) {
        bool bSame = TestTrue(TEXT("existing owned instance and owner remain identical"), Root->Child.Get() == Child && Child->GetOuter() == Root.Get());
        for (TFieldIterator<FProperty> It(Child->GetClass()); It; ++It)
            bSame &= TestTrue(*FString::Printf(TEXT("complete native field %s retained"), *It->GetName()), It->Identical_InContainer(Child, Desired));
        return bSame;
    };
    UeAgentDetailsAuthoringService::FCapturePolicy Policy;
    TSharedPtr<FJsonObject> Properties, Definitions;
    FString Error;
    if (!CaptureReflectedObjectRecord(*Root.Get(), Policy, Properties, Definitions, Error)) { AddError(Error); return false; }
    const auto Row = Properties->GetObjectField(TEXT("Owned"))->GetObjectField(TEXT("Child"));
    const auto Value = Row->GetObjectField(TEXT("value"));
    const auto Owned = Value->GetObjectField(TEXT("owned_object"));
    const auto Rows = Owned->GetObjectField(TEXT("properties"));
    TestFalse(TEXT("owned category with only nonactionable properties is absent"), Rows->HasField(TEXT("Empty")));
    TestTrue(TEXT("owned operations and ordinary type remain available"), !Row->GetStringField(TEXT("type")).IsEmpty());
    TestTrue(TEXT("empty mutable containers remain available"), Rows->GetObjectField(TEXT("Containers"))->HasField(TEXT("EmptyArray")));
    const auto MixedRow = Rows->GetObjectField(TEXT("Mixed"))->GetObjectField(TEXT("Mixed"));
    const auto MixedValue = MixedRow->GetObjectField(TEXT("value"))->GetObjectField(TEXT("fields"));
    TestFalse(TEXT("owned nested nonactionable member is absent"), MixedValue->HasField(TEXT("Empty")));
    TestEqual(TEXT("owned writable value remains directly readable"), MixedValue->GetNumberField(TEXT("Count")), 19.0);
    // The same public reflected-record import used by native owners consumes a sparse category map.
    MixedValue->SetNumberField(TEXT("Count"), 29);
    auto SparseCategory = MakeShared<FJsonObject>(); SparseCategory->SetObjectField(TEXT("Mixed"), MixedRow);
    Rows->Values.Empty(); Rows->SetObjectField(TEXT("Mixed"), SparseCategory);
    FPreparedApply Prepared; FApplyResult Result;
    if (!PrepareReflectedObjectRecord(*Root.Get(), Policy, Properties, Prepared, Error)) { AddError(Error); return false; }
    SameNative(Before.Get());
    if (!ApplyPrepared(Prepared, Result)) { AddError(Result.ErrorCode); return false; }
    SameNative(Expected.Get());
    TestTrue(TEXT("public prepared edit verifies"), VerifyPrepared(Prepared, Result));
    if (!RollbackPrepared(Prepared, Result)) { AddError(Result.ErrorCode); return false; }
    SameNative(Before.Get());
    // Complete capture is independent of ordinary projection, including hidden nested native state.
    UeAgentPropertyValueCodec::FOptions Full;
    auto* NativeMixed = FindFProperty<FProperty>(Child->GetClass(), TEXT("Mixed"));
    const auto Encoded = UeAgentPropertyValueCodec::Encode(NativeMixed, &Child->Mixed, Child, Full);
    TestEqual(TEXT("complete codec retains nondefault readonly owned descendant"),
        Encoded->AsObject()->GetObjectField(TEXT("fields"))->GetObjectField(TEXT("Empty"))->GetObjectField(TEXT("fields"))
        ->GetObjectField(TEXT("Nested"))->GetObjectField(TEXT("fields"))->GetObjectField(TEXT("Kept"))->GetNumberField(TEXT("literal")), 813.0);
    return true;
}
