#include "Misc/AutomationTest.h"
#include "Utilities/UeAgentPropertyPath.h"
#include "DomainAdapters/PieRuntimeDebug/UeAgentPiePropertyPathTestTypes.h"
#include "UObject/UnrealType.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentPieRuntimePropertyPathTest,
    "UeAgentInterface.DomainAdapters.PieRuntimeDebug.PropertyPaths",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentPieRuntimePropertyPathTest::RunTest(const FString&)
{
    const auto Select = [](const TCHAR* Kind, TSharedPtr<FJsonValue> Value) -> TSharedPtr<FJsonValue>
    {
        const auto Segment = MakeShared<FJsonObject>();
        Segment->SetField(Kind, Value);
        return MakeShared<FJsonValueObject>(Segment);
    };
    const auto Field = [&](const TCHAR* Name) { return Select(TEXT("field"), MakeShared<FJsonValueString>(Name)); };
    const UeAgentPropertyValueCodec::FOptions Options;
    FString Error;
    UeAgentPropertyPath::FResolved Leaf;
    TStrongObjectPtr<UUeAgentPiePathTarget> MapOwner(NewObject<UUeAgentPiePathTarget>());
    auto& Alpha = MapOwner->Records.Add(TEXT("Alpha"));
    Alpha.Count = 10; Alpha.Values = {101, 102}; Alpha.ReadOnly = 811;
    MapOwner->Records.Add(TEXT("Beta")).Count = 20;
    FProperty* Map = FindFProperty<FProperty>(MapOwner->GetClass(), TEXT("Records"));
    const TArray<TSharedPtr<FJsonValue>> CountPath = {
        Select(TEXT("map_key"), MakeShared<FJsonValueString>(TEXT("Alpha"))), Field(TEXT("Count"))};
    if (!TestTrue(TEXT("map key and nested field resolve"), UeAgentPropertyPath::Resolve(
        Map, &MapOwner->Records, CountPath, MapOwner.Get(), Options, Leaf, Error))) return false;
    TestTrue(TEXT("leaf write uses shared codec"), UeAgentPropertyValueCodec::Decode(
        Leaf.Property, MakeShared<FJsonValueNumber>(57), Leaf.Address, MapOwner.Get(), Options, Error));
    TestEqual(TEXT("selected field changed"), MapOwner->Records.FindChecked(TEXT("Alpha")).Count, 57);
    TestTrue(TEXT("array sibling retained"), MapOwner->Records.FindChecked(TEXT("Alpha")).Values == TArray<int32>{101, 102});
    TestEqual(TEXT("read-only sibling retained"), MapOwner->Records.FindChecked(TEXT("Alpha")).ReadOnly, 811);
    TestEqual(TEXT("other map entry retained"), MapOwner->Records.FindChecked(TEXT("Beta")).Count, 20);
    TestTrue(TEXT("explicit native intent is retained"), UeAgentPropertyValueCodec::VerifyValueIntent(
        Leaf.Property, MakeShared<FJsonValueNumber>(57), Leaf.Address, MapOwner.Get(), Options, Error));
    TestFalse(TEXT("lost explicit intent cannot report success"), UeAgentPropertyValueCodec::VerifyValueIntent(
        Leaf.Property, MakeShared<FJsonValueNumber>(58), Leaf.Address, MapOwner.Get(), Options, Error));
    const TArray<TSharedPtr<FJsonValue>> Missing = {
        Select(TEXT("map_key"), MakeShared<FJsonValueString>(TEXT("Later"))), Field(TEXT("Count"))};
    TestTrue(TEXT("static validation permits a key created by earlier operations"), UeAgentPropertyPath::Resolve(
        Map, nullptr, Missing, MapOwner.Get(), Options, Leaf, Error, true));
    TestFalse(TEXT("execution rejects missing key"), UeAgentPropertyPath::Resolve(
        Map, &MapOwner->Records, Missing, MapOwner.Get(), Options, Leaf, Error));

    TStrongObjectPtr<UUeAgentPiePathTarget> SetOwner(NewObject<UUeAgentPiePathTarget>());
    SetOwner->EmptySet = {1, 2};
    FProperty* Set = FindFProperty<FProperty>(SetOwner->GetClass(), TEXT("EmptySet"));
    TSet<int32> Temporary = SetOwner->EmptySet;
    const TArray<TSharedPtr<FJsonValue>> Element = {Select(TEXT("set_value"), MakeShared<FJsonValueNumber>(1))};
    if (!TestTrue(TEXT("set element resolves"), UeAgentPropertyPath::Resolve(
        Set, &Temporary, Element, SetOwner.Get(), Options, Leaf, Error))) return false;
    UeAgentPropertyValueCodec::Decode(Leaf.Property, MakeShared<FJsonValueNumber>(3), Leaf.Address, SetOwner.Get(), Options, Error);
    TestTrue(TEXT("changed element rehashes"), UeAgentPropertyPath::FinishWrite(Leaf, Error));
    TestTrue(TEXT("native hashed lookup sees new element and preserved sibling"), Temporary.Contains(3) && Temporary.Contains(2) && !Temporary.Contains(1));
    TArray<TSharedPtr<FJsonValue>> ReadbackPath;
    TestTrue(TEXT("capture updated set selector before temporary storage expires"), UeAgentPropertyPath::CaptureWritePath(
        Leaf, Element, SetOwner.Get(), Options, ReadbackPath, Error));
    TSet<int32> ReadbackSet = Temporary;
    UeAgentPropertyPath::FResolved ReadbackLeaf;
    TestTrue(TEXT("updated selector resolves independent native storage"), UeAgentPropertyPath::Resolve(
        Set, &ReadbackSet, ReadbackPath, SetOwner.Get(), Options, ReadbackLeaf, Error));
    TestTrue(TEXT("set edit readback retains explicit value"), UeAgentPropertyValueCodec::VerifyValueIntent(
        ReadbackLeaf.Property, MakeShared<FJsonValueNumber>(3), ReadbackLeaf.Address, SetOwner.Get(), Options, Error));
    TestFalse(TEXT("original selector remains unchanged and no longer resolves"), UeAgentPropertyPath::Resolve(
        Set, &Temporary, Element, SetOwner.Get(), Options, ReadbackLeaf, Error));
    Temporary = SetOwner->EmptySet;
    UeAgentPropertyPath::Resolve(Set, &Temporary, Element, SetOwner.Get(), Options, Leaf, Error);
    UeAgentPropertyValueCodec::Decode(Leaf.Property, MakeShared<FJsonValueNumber>(2), Leaf.Address, SetOwner.Get(), Options, Error);
    TestFalse(TEXT("duplicate set identity rejected before commit"), UeAgentPropertyPath::FinishWrite(Leaf, Error));
    TestTrue(TEXT("rejected temporary never changes original"), SetOwner->EmptySet.Contains(1) && SetOwner->EmptySet.Contains(2) && SetOwner->EmptySet.Num() == 2);

    FProperty* Fixed = FindFProperty<FProperty>(SetOwner->GetClass(), TEXT("FixedMixed"));
    const TArray<TSharedPtr<FJsonValue>> FixedPath = {Select(TEXT("index"), MakeShared<FJsonValueNumber>(1)), Field(TEXT("Count"))};
    if (!TestTrue(TEXT("fixed array element nested field resolves"), UeAgentPropertyPath::Resolve(
        Fixed, SetOwner->FixedMixed, FixedPath, SetOwner.Get(), Options, Leaf, Error))) return false;
    UeAgentPropertyValueCodec::Decode(Leaf.Property, MakeShared<FJsonValueNumber>(9), Leaf.Address, SetOwner.Get(), Options, Error);
    TestEqual(TEXT("first fixed element preserved"), SetOwner->FixedMixed[0].Count, 5);
    TestEqual(TEXT("selected fixed element changed"), SetOwner->FixedMixed[1].Count, 9);
    const TArray<TSharedPtr<FJsonValue>> FixedElement = {Select(TEXT("index"), MakeShared<FJsonValueNumber>(1))};
    UeAgentPropertyPath::Resolve(Fixed, SetOwner->FixedMixed, FixedElement, SetOwner.Get(), Options, Leaf, Error);
    const auto Partial = MakeShared<FJsonObject>();
    const auto Fields = MakeShared<FJsonObject>();
    Fields->SetNumberField(TEXT("Count"), 9);
    Partial->SetObjectField(TEXT("fields"), Fields);
    auto PartialOptions = Options;
    PartialOptions.bAllowPartialStructFields = true;
    TestTrue(TEXT("single fixed struct checks explicit member without reading adjacent storage"),
        UeAgentPropertyValueCodec::VerifyValueIntent(Leaf.Property, MakeShared<FJsonValueObject>(Partial),
            Leaf.Address, SetOwner.Get(), PartialOptions, Error, Leaf.bSingleFixedElement));
    Fields->SetNumberField(TEXT("Count"), 10);
    TestFalse(TEXT("partial struct still rejects a lost explicit member"),
        UeAgentPropertyValueCodec::VerifyValueIntent(Leaf.Property, MakeShared<FJsonValueObject>(Partial),
            Leaf.Address, SetOwner.Get(), PartialOptions, Error, Leaf.bSingleFixedElement));
    return true;
}
