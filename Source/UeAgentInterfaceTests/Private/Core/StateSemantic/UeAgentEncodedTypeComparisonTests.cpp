// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/NoExportTypes.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringEncodedTypes.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticCompare.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "StructUtils/InstancedStruct.h"
#include "StructUtils/PropertyBag.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Components/PrimitiveComponent.h"
#include <cmath>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentEncodedTypeComparisonTest,
    "UeAgentInterface.Core.StateSemantic.EncodedTypeComparison",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentEncodedTypeComparisonTest::RunTest(const FString&)
{
    using namespace UeAgentAuthoringState;
    using UeAgentAuthoringEncodedTypes::FScope;
    UeAgentAuthoringSemanticCompare::FComparator Compare;
    if (!TestTrue(TEXT("shared canonical comparator exists"),
        UeAgentAuthoringSemanticCompare::FComparatorRegistry::Get().CopyComparator(TEXT("uai.compare.canonical_value"), Compare))) return false;

    auto Capture = [](const FString& Container, double RotationDelta, int32 Identity, bool bTyped)
    {
        FScope Scope;
        const FTransform Transform(FRotator(9.541382252304087e-08, 17.0, 0.0));
        TSharedPtr<FJsonValue> Value;
        {
            FScope Inner;
            Value = UeAgentPropertyValueCodec::EncodeStructValue(TBaseStructure<FTransform>::Get(), &Transform, nullptr, {});
        }
        // Model a detached property snapshot being projected by the common Details owner.
        FStateEntry Property;
        Property.Type.Kind = TEXT("transform");
        Property.Value = Value;
        Property.bAvailable = Property.bEditable = true;
        const auto Descriptor = UeAgentDetailsAuthoringService::BuildPropertyDescriptor(Property);
        Value = Descriptor->TryGetField(TEXT("value"));
        auto Components = Value->AsObject()->GetArrayField(TEXT("rotation"));
        Components[0] = MakeShared<FJsonValueNumber>(Components[0]->AsNumber() + RotationDelta);
        Value->AsObject()->SetArrayField(TEXT("rotation"), Components);
        UeAgentAuthoringTypeDescriptors::FDescriptor TransformType;
        TransformType.Kind = TEXT("transform");
        TSharedPtr<FJsonValue> Payload = Value;
        if (Container == TEXT("array"))
        {
            // Rewrapping an object and embedding it in a heterogeneous document must retain its type.
            Payload = MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
                MakeShared<FJsonValueString>(TEXT("tag")), MakeShared<FJsonValueObject>(Value->AsObject())});
        }
        else if (Container == TEXT("clone")) Payload = UeAgentAuthoringState::CloneCanonicalValue(Value);
        else if (Container == TEXT("set") || Container.StartsWith(TEXT("map")))
        {
            auto Object = MakeShared<FJsonObject>();
            UeAgentAuthoringTypeDescriptors::FDescriptor Type;
            if (Container == TEXT("set"))
            {
                Type.Kind = TEXT("set");
                Type.ElementType = MakeShared<UeAgentAuthoringTypeDescriptors::FDescriptor>(TransformType);
                Object->SetArrayField(TEXT("items"), {Value});
            }
            else
            {
                Type.Kind = TEXT("map");
                Type.KeyType = MakeShared<UeAgentAuthoringTypeDescriptors::FDescriptor>(TransformType);
                Type.MappedType = MakeShared<UeAgentAuthoringTypeDescriptors::FDescriptor>(TransformType);
                auto Entry = MakeShared<FJsonObject>();
                const auto Constant = UeAgentPropertyValueCodec::EncodeStructValue(TBaseStructure<FTransform>::Get(), &Transform, nullptr, {});
                Entry->SetField(TEXT("key"), Container == TEXT("map_key") ? Value : Constant);
                Entry->SetField(TEXT("value"), Container == TEXT("map_key") ? Constant : Value);
                Object->SetArrayField(TEXT("entries"), {MakeShared<FJsonValueObject>(Entry)});
            }
            Payload = MakeShared<FJsonValueObject>(Object);
            FScope::Record(Payload, Type, nullptr);
        }
        auto Document = MakeShared<FJsonObject>();
        Document->SetField(TEXT("payload"), Payload);
        Document->SetNumberField(TEXT("identity"), Identity);
        FStateEntry Result;
        Result.Type.Kind = TEXT("object");
        Result.Value = MakeShared<FJsonValueObject>(Document);
        if (bTyped) Result.ComparisonValueDefinition = MakeShared<FJsonValueObject>(Scope.BuildDefinition(Result.Value));
        return Result;
    };
    auto Equivalent = [&](const FStateEntry& Left, const FStateEntry& Right)
    {
        bool bEquivalent = false;
        FString Error;
        TestTrue(TEXT("comparison completed"), Compare(Left, Right, bEquivalent, Error));
        return bEquivalent;
    };
    const auto Baseline = Capture(TEXT("object"), 0, 1, true);
    TestFalse(TEXT("comparison metadata does not create an editable definition"), Baseline.ValueDefinition.IsValid());
    TestFalse(TEXT("comparison metadata stays out of serialized snapshot"), Baseline.ToJsonObject()->HasField(TEXT("comparison_value_definition")));
    TestTrue(TEXT("native rotation roundtrip is equivalent in raw document"), Equivalent(Baseline, Capture(TEXT("object"), 1.6e-22, 1, true)));
    TestFalse(TEXT("real rotation edit remains different"), Equivalent(Baseline, Capture(TEXT("object"), 1, 1, true)));
    TestFalse(TEXT("identity remains exact"), Equivalent(Baseline, Capture(TEXT("object"), 0, 2, true)));
    TestFalse(TEXT("untyped lookalike does not acquire tolerance"), Equivalent(Capture(TEXT("object"), 0, 1, false), Capture(TEXT("object"), 1.6e-22, 1, false)));
    TestTrue(TEXT("heterogeneous array carries element types"), Equivalent(Capture(TEXT("array"), 0, 1, true), Capture(TEXT("array"), 1.6e-22, 1, true)));
    TestTrue(TEXT("common clone preserves captured type"), Equivalent(Capture(TEXT("clone"), 0, 1, true), Capture(TEXT("clone"), 1.6e-22, 1, true)));
    TestFalse(TEXT("set element identity stays exact"), Equivalent(Capture(TEXT("set"), 0, 1, true), Capture(TEXT("set"), 1.6e-22, 1, true)));
    TestFalse(TEXT("map key identity stays exact"), Equivalent(Capture(TEXT("map_key"), 0, 1, true), Capture(TEXT("map_key"), 1.6e-22, 1, true)));
    TestTrue(TEXT("map payload uses its declared type"), Equivalent(Capture(TEXT("map_value"), 0, 1, true), Capture(TEXT("map_value"), 1.6e-22, 1, true)));
    TestFalse(TEXT("capture scope does not leak"), FScope::IsActive());
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentFormalTypeComparisonTest,
    "UeAgentInterface.Core.StateSemantic.FormalTypeComparison",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentFormalTypeComparisonTest::RunTest(const FString&)
{
    using namespace UeAgentAuthoringState;
    using namespace UeAgentAuthoringTypeDescriptors;
    UeAgentAuthoringSemanticCompare::FComparator Compare;
    if (!UeAgentAuthoringSemanticCompare::FComparatorRegistry::Get().CopyComparator(TEXT("uai.compare.canonical_value"), Compare)) return false;
    const auto Number = [](const TCHAR* Text) -> TSharedPtr<FJsonValue> { return MakeShared<FJsonValueNumberString>(Text); };
    const auto String = [](const TCHAR* Text) -> TSharedPtr<FJsonValue> { return MakeShared<FJsonValueString>(Text); };
    const auto Type = [](const TCHAR* Kind) { FDescriptor Result; Result.Kind = Kind; return Result; };
    const auto Literal = [](const TSharedPtr<FJsonValue>& Value) -> TSharedPtr<FJsonValue> {
        auto Result = MakeShared<FJsonObject>(); Result->SetField(TEXT("literal"), Value); return MakeShared<FJsonValueObject>(Result);
    };
    const auto Equivalent = [&](const FDescriptor& Declared, const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B) {
        FStateEntry L, R; L.Type = R.Type = Declared; L.Value = A; R.Value = B;
        bool Equal = false; FString Error;
        TestTrue(TEXT("typed comparison executes"), Compare(L, R, Equal, Error)); return Equal;
    };
    TestTrue(TEXT("float authoring decimal uses native float precision"), Equivalent(Type(TEXT("float")), Number(TEXT("0.15")), Literal(MakeShared<FJsonValueNumber>(0.15f))));
    TestFalse(TEXT("adjacent float is still different"), Equivalent(Type(TEXT("float")), Number(TEXT("0.15")), MakeShared<FJsonValueNumber>(std::nextafter(0.15f, 1.0f))));
    TestTrue(TEXT("double lexical decimal and native number agree"), Equivalent(Type(TEXT("double")), Number(TEXT("0.1")), Literal(MakeShared<FJsonValueNumber>(0.1))));
    TestFalse(TEXT("adjacent double is still different"), Equivalent(Type(TEXT("double")), Number(TEXT("0.1")), MakeShared<FJsonValueNumber>(std::nextafter(0.1, 1.0))));
    TestFalse(TEXT("untyped numbers retain exact lexical semantics"), Equivalent(Type(TEXT("object")), Number(TEXT("0.1")), MakeShared<FJsonValueNumber>(0.1)));
    for (const TCHAR* Kind : {TEXT("int8"), TEXT("int16"), TEXT("int32"), TEXT("int64"), TEXT("uint8"), TEXT("uint16"), TEXT("uint32"), TEXT("uint64")}) {
        const bool bWide = FString(Kind).EndsWith(TEXT("64"));
        TestTrue(FString::Printf(TEXT("%s shares integer decoder"), Kind), Equivalent(Type(Kind), bWide ? String(TEXT("7")) : Number(TEXT("7")), Literal(bWide ? String(TEXT("7")) : Number(TEXT("7.0")))));
        TestFalse(FString::Printf(TEXT("%s does not erase a real difference"), Kind), Equivalent(Type(Kind), bWide ? String(TEXT("7")) : Number(TEXT("7")), bWide ? String(TEXT("8")) : Number(TEXT("8"))));
    }
    TestTrue(TEXT("signed 64 bit decimal string retains exact range"), Equivalent(Type(TEXT("int64")), String(TEXT("9223372036854775807")), Literal(String(TEXT("9223372036854775807")))));
    TestFalse(TEXT("64 bit integers beyond double precision stay distinct"), Equivalent(Type(TEXT("uint64")), String(TEXT("18446744073709551614")), Literal(String(TEXT("18446744073709551615")))));
    TestFalse(TEXT("64 bit storage does not silently accept the wrong numeric shape"), Equivalent(Type(TEXT("int64")), Number(TEXT("7")), String(TEXT("7"))));
    TestFalse(TEXT("same invalid int8 value is not valid equivalence"), Equivalent(Type(TEXT("int8")), Number(TEXT("128")), Number(TEXT("128"))));
    TestFalse(TEXT("same invalid uint16 value is not valid equivalence"), Equivalent(Type(TEXT("uint16")), Number(TEXT("65536")), Number(TEXT("65536"))));
    TestTrue(TEXT("name follows native identity"), Equivalent(Type(TEXT("name")), String(TEXT("Alpha")), Literal(String(TEXT("ALPHA")))));
    TestFalse(TEXT("string retains case"), Equivalent(Type(TEXT("string")), String(TEXT("Alpha")), Literal(String(TEXT("ALPHA")))));
    TestTrue(TEXT("boolean literal agrees"), Equivalent(Type(TEXT("bool")), MakeShared<FJsonValueBoolean>(true), Literal(MakeShared<FJsonValueBoolean>(true))));
    TestFalse(TEXT("bool and numeric one are not interchangeable"), Equivalent(Type(TEXT("bool")), MakeShared<FJsonValueBoolean>(true), Number(TEXT("1"))));
    const auto Vec = [](double X) -> TSharedPtr<FJsonValue> {
        return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueNumber>(X), MakeShared<FJsonValueNumber>(2), MakeShared<FJsonValueNumber>(3)});
    };
    TestTrue(TEXT("float vector keeps native storage identity"), Equivalent(DescribeStruct(TVariantStructure<FVector3f>::Get()), Vec(1000.1), Vec(1000.1f)));
    TestFalse(TEXT("double vector does not use blanket epsilon"), Equivalent(DescribeStruct(TBaseStructure<FVector>::Get()), Vec(1), Vec(1 + 1e-10)));
    FMovieSceneDoubleValue Native(0.1); Native.Tangent.ArriveTangent = 0.15f;
    auto* Field = FindFProperty<FProperty>(FMovieSceneDoubleValue::StaticStruct(), TEXT("Value"));
    FString Error; bool Equal = false;
    FMovieSceneDoubleValue Changed = Native; Changed.Value = std::nextafter(Native.Value, 1.0);
    TestTrue(TEXT("native comparison completes"), UeAgentAuthoringSemanticCompare::ArePropertyValuesEquivalent(Field, &Native.Value, &Changed.Value, nullptr, Equal, Error, true));
    TestFalse(TEXT("write elision retains tiny native changes"), Equal);
    auto* EnumField = FindFProperty<FProperty>(FMovieSceneDoubleValue::StaticStruct(), TEXT("InterpMode"));
    const auto EnumValue = MakeShared<FJsonObject>(); EnumValue->SetStringField(TEXT("selected"), TEXT("RCIM_Linear"));
    TestTrue(TEXT("enum shorthand and canonical value use the same resolver"), Equivalent(Describe(EnumField), String(TEXT("RCIM_Linear")), MakeShared<FJsonValueObject>(EnumValue)));
    auto* MaskProperty = FindFProperty<FProperty>(UPrimitiveComponent::StaticClass(), TEXT("ExcludeFromHLODLevels"));
    if (!TestTrue(TEXT("real native bitmask property is available"), MaskProperty && IsBitmaskProperty(MaskProperty))) return false;
    uint8 NativeMask = 3;
    TSharedPtr<FJsonObject> MaskDefinition;
    const auto MaskValue = UeAgentPropertyValueCodec::Encode(MaskProperty, &NativeMask, nullptr, {}, 0, &MaskDefinition);
    if (!TestTrue(TEXT("native bitmask definition is available"), MaskDefinition.IsValid())) return false;
    TestEqual(TEXT("bitmask metadata uses the public definition contract"), MaskDefinition->GetStringField(TEXT("type")), FString(TEXT("bitmask")));
    auto ReorderedMask = MakeShared<FJsonObject>();
    auto SelectedMasks = MaskValue->AsObject()->GetArrayField(TEXT("selected"));
    if (!TestTrue(TEXT("native mask has two independent flags"), SelectedMasks.Num() == 2)) return false;
    Swap(SelectedMasks[0], SelectedMasks[1]);
    ReorderedMask->SetArrayField(TEXT("selected"), SelectedMasks);
    const auto MaskComparison = UeAgentPropertyValueCodec::CompareCanonicalValues(Describe(MaskProperty).ToJsonObject(),
        MaskDefinition, MaskValue, MakeShared<FJsonValueObject>(ReorderedMask), true);
    TestTrue(TEXT("bitmask selected names share native mask semantics"), MaskComparison.IsSet() && MaskComparison->bEquivalent);
    uint8 DecodedMask = 0;
    TestTrue(TEXT("bitmask decoder accepts reordered flags"), UeAgentPropertyValueCodec::Decode(
        MaskProperty, MakeShared<FJsonValueObject>(ReorderedMask), &DecodedMask, nullptr, {}, Error));
    TestEqual(TEXT("bitmask decoder and comparison resolve the same native value"), DecodedMask, NativeMask);
    for (const bool bExtraField : {false, true}) {
        auto InvalidMask = MakeShared<FJsonObject>();
        InvalidMask->SetArrayField(TEXT("selected"), {});
        InvalidMask->SetStringField(bExtraField ? TEXT("unexpected") : TEXT("unmapped_bits"), TEXT("256"));
        const auto InvalidValue = MakeShared<FJsonValueObject>(InvalidMask);
        const auto Compared = UeAgentPropertyValueCodec::CompareCanonicalValues(Describe(MaskProperty).ToJsonObject(),
            MaskDefinition, InvalidValue, InvalidValue, true);
        TestTrue(TEXT("bitmask comparison rejects malformed or overflowing native values"), Compared.IsSet() && !Compared->bEquivalent);
        TestFalse(TEXT("bitmask decoding uses the same rejection"), UeAgentPropertyValueCodec::Decode(
            MaskProperty, InvalidValue, &DecodedMask, nullptr, {}, Error));
    }
    {
        UeAgentAuthoringEncodedTypes::FScope MaskScope;
        const auto CapturedMask = UeAgentPropertyValueCodec::Encode(MaskProperty, &NativeMask, nullptr, {});
        const auto CapturedDefinition = MaskScope.BuildDefinition(CapturedMask);
        TestTrue(TEXT("bitmask contract survives captured type wrapping"), CapturedDefinition->HasField(TEXT("contract")));
    }
    for (const TCHAR* Kind : {TEXT("object_ref"), TEXT("class_ref"), TEXT("soft_object_ref"), TEXT("soft_class_ref"), TEXT("weak_object_ref"), TEXT("lazy_object_ref"), TEXT("interface_ref")})
        TestTrue(FString::Printf(TEXT("%s retains null identity"), Kind), Equivalent(Type(Kind), UeAgentPropertyValueCodec::EncodeReferencePath(TEXT("")), UeAgentPropertyValueCodec::EncodeReferencePath(TEXT(""))));
    FDescriptor Container = Type(TEXT("array")); Container.ElementType = MakeShared<FDescriptor>(Type(TEXT("double")));
    const auto Items = [](TArray<TSharedPtr<FJsonValue>> Values) -> TSharedPtr<FJsonValue> { auto R = MakeShared<FJsonObject>(); R->SetArrayField(TEXT("items"), MoveTemp(Values)); return MakeShared<FJsonValueObject>(R); };
    TestTrue(TEXT("array propagates element type"), Equivalent(Container, Items({Number(TEXT("0.1"))}), Items({Literal(MakeShared<FJsonValueNumber>(0.1))})));
    Container.Kind = TEXT("fixed_array"); Container.FixedLength = 1;
    TestTrue(TEXT("fixed array propagates element type"), Equivalent(Container, Items({Number(TEXT("0.1"))}), Items({Literal(MakeShared<FJsonValueNumber>(0.1))})));
    Container.Kind = TEXT("set"); Container.FixedLength = 0;
    TestTrue(TEXT("set compares native values independent of order"), Equivalent(Container, Items({Number(TEXT("0.1")), Number(TEXT("0.2"))}), Items({MakeShared<FJsonValueNumber>(0.2), MakeShared<FJsonValueNumber>(0.1)})));
    FDescriptor Optional = Type(TEXT("optional")); Optional.InnerType = Container.ElementType;
    auto A = MakeShared<FJsonObject>(), B = MakeShared<FJsonObject>(); A->SetBoolField(TEXT("is_set"), true); B->SetBoolField(TEXT("is_set"), true);
    A->SetField(TEXT("value"), Number(TEXT("0.1"))); B->SetField(TEXT("value"), Literal(MakeShared<FJsonValueNumber>(0.1)));
    TestTrue(TEXT("optional propagates inner type"), Equivalent(Optional, MakeShared<FJsonValueObject>(A), MakeShared<FJsonValueObject>(B)));
    // Reflected nested fields must survive a detached snapshot, not just a codec call.
    UeAgentAuthoringEncodedTypes::FScope Scope;
    FStateEntry Captured; Captured.Type = DescribeStruct(FMovieSceneDoubleValue::StaticStruct());
    Captured.Value = UeAgentPropertyValueCodec::EncodeStructValue(FMovieSceneDoubleValue::StaticStruct(), &Native, nullptr, {});
    Captured.ComparisonValueDefinition = MakeShared<FJsonValueObject>(Scope.BuildDefinition(Captured.Value));
    FStateEntry Authored = Captured; Authored.Value = CloneCanonicalValue(Captured.Value);
    Authored.Value->AsObject()->GetObjectField(TEXT("fields"))->SetField(TEXT("Value"), Literal(Number(TEXT("0.1"))));
    Authored.Value->AsObject()->GetObjectField(TEXT("fields"))->GetObjectField(TEXT("Tangent"))->GetObjectField(TEXT("fields"))->SetField(TEXT("ArriveTangent"), Literal(Number(TEXT("0.15"))));
    TestTrue(TEXT("reflected struct comparison executes"), Compare(Authored, Captured, Equal, Error));
    TestTrue(TEXT("nested float and double use captured formal fields"), Equal);
    // Dynamic reflection must carry the same member declarations as ordinary structs.
    FInstancedPropertyBag Bag;
    Bag.AddProperty(TEXT("FloatValue"), EPropertyBagPropertyType::Float);
    Bag.AddProperty(TEXT("DoubleValue"), EPropertyBagPropertyType::Double);
    Bag.SetValueFloat(TEXT("FloatValue"), 0.15f);
    Bag.SetValueDouble(TEXT("DoubleValue"), 0.1);
    FStateEntry Dynamic; Dynamic.Type = Type(TEXT("property_bag"));
    Dynamic.Value = UeAgentPropertyValueCodec::EncodePropertyBag(Bag, nullptr, {});
    Dynamic.ComparisonValueDefinition = MakeShared<FJsonValueObject>(Scope.BuildDefinition(Dynamic.Value));
    FStateEntry DesiredBag = Dynamic; DesiredBag.Value = CloneCanonicalValue(Dynamic.Value);
    DesiredBag.Value->AsObject()->GetObjectField(TEXT("fields"))->SetField(TEXT("FloatValue"), Literal(Number(TEXT("0.15"))));
    DesiredBag.Value->AsObject()->GetObjectField(TEXT("fields"))->SetField(TEXT("DoubleValue"), Literal(Number(TEXT("0.1"))));
    TestTrue(TEXT("property bag comparison executes"), Compare(DesiredBag, Dynamic, Equal, Error));
    TestTrue(TEXT("dynamic bag keeps float and double member types"), Equal);
    FInstancedStruct Instanced; Instanced.InitializeAs(FMovieSceneDoubleValue::StaticStruct(), reinterpret_cast<const uint8*>(&Native));
    Dynamic.Type = DescribeStruct(FInstancedStruct::StaticStruct());
    Dynamic.Value = UeAgentPropertyValueCodec::EncodeStructValue(FInstancedStruct::StaticStruct(), &Instanced, nullptr, {});
    Dynamic.ComparisonValueDefinition = MakeShared<FJsonValueObject>(Scope.BuildDefinition(Dynamic.Value));
    FStateEntry DesiredStruct = Dynamic; DesiredStruct.Value = CloneCanonicalValue(Dynamic.Value);
    DesiredStruct.Value->AsObject()->GetObjectField(TEXT("fields"))->SetField(TEXT("Value"), Literal(Number(TEXT("0.1"))));
    TestTrue(TEXT("instanced struct comparison executes"), Compare(DesiredStruct, Dynamic, Equal, Error));
    TestTrue(TEXT("instanced struct keeps its native member type"), Equal);
    FInstancedStruct DecodedInstance;
    TestTrue(TEXT("direct struct decoder shares the dynamic struct entry"), UeAgentPropertyValueCodec::DecodeStructValue(FInstancedStruct::StaticStruct(), DesiredStruct.Value, &DecodedInstance, Error));
    TestTrue(TEXT("dynamic struct roundtrip retains native double"), DecodedInstance.GetScriptStruct() == FMovieSceneDoubleValue::StaticStruct()
        && reinterpret_cast<const FMovieSceneDoubleValue*>(DecodedInstance.GetMemory())->Value == Native.Value);
    FStateEntry EmptyL, EmptyR;
    EmptyL.Type = EmptyR.Type = Container;
    EmptyL.Value = EmptyR.Value = Items({});
    EmptyR.Type.ElementType = MakeShared<FDescriptor>(Type(TEXT("float")));
    TestTrue(TEXT("empty collection type check executes"), Compare(EmptyL, EmptyR, Equal, Error));
    TestFalse(TEXT("empty collections do not erase element type changes"), Equal);
    EmptyL.Type.ElementType = MakeShared<FDescriptor>(Type(TEXT("vector3")));
    EmptyR.Type.ElementType = MakeShared<FDescriptor>(DescribeStruct(TBaseStructure<FVector>::Get()));
    TestTrue(TEXT("legacy nested metadata check executes"), Compare(EmptyL, EmptyR, Equal, Error));
    TestTrue(TEXT("missing optional struct metadata can use current native type"), Equal);
    FDescriptor Map = Type(TEXT("map")); Map.KeyType = MakeShared<FDescriptor>(Type(TEXT("name")));
    Map.MappedType = MakeShared<FDescriptor>(Type(TEXT("double")));
    const auto MapValue = [&](const TCHAR* Key, double Value, const TCHAR* Extra) -> TSharedPtr<FJsonValue> {
        auto Entry = MakeShared<FJsonObject>(); Entry->SetStringField(TEXT("key"), Key);
        Entry->SetNumberField(TEXT("value"), Value); Entry->SetStringField(TEXT("source"), Extra);
        auto Record = MakeShared<FJsonObject>(); Record->SetArrayField(TEXT("entries"), {MakeShared<FJsonValueObject>(Entry)});
        return MakeShared<FJsonValueObject>(Record);
    };
    TestFalse(TEXT("map preserves additional row information"), Equivalent(Map, MapValue(TEXT("A"), 1, TEXT("old")), MapValue(TEXT("a"), 1, TEXT("new"))));
    EmptyL.Type = EmptyR.Type = Map; EmptyL.Value = MapValue(TEXT("A"), 1, TEXT("same")); EmptyR.Value = MapValue(TEXT("a"), 2, TEXT("same"));
    TestTrue(TEXT("map difference diagnosis executes"), Compare(EmptyL, EmptyR, Equal, Error));
    TestEqual(TEXT("map reports the original entry value path"), Error, FString(TEXT("canonical_value_mismatch:/entries/0/value")));
    UE::UAI::HjsonWorkspace::FWorkspaceError ExportFailure;
    AddExpectedError(TEXT("UAI workspace operation: export_failed"), EAutomationExpectedErrorFlags::Contains, 1);
    ExportFailure.SetExternalFailure(TEXT("export_failed"), TEXT("/Game/TypeProbe.TypeProbe"), TEXT("composition_value_definition_invalid:internal-subject"));
    const FString PublicFailure = ExportFailure.ToPublicDetail();
    TestTrue(TEXT("workspace bridge retains the original cause"), PublicFailure.Contains(TEXT("composition_value_definition_invalid")));
    TestFalse(TEXT("workspace bridge does not publish opaque provider detail"), PublicFailure.Contains(TEXT("internal-subject")));
    return !HasAnyErrors();
}
