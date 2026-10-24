#include "Misc/AutomationTest.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/UnrealType.h"
#include "UObject/ObjectRedirector.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphSchema.h"
#include "Vertical/T167/UeAgentT167AssetReferenceTestTypes.h"
#include "Utilities/AuthoringObjectApply/UeAgentObjectApplyNotificationTestTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentStructFieldCodecTest,
	"UeAgentInterface.Utilities.PropertyValueCodec.StructFieldFilter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentStructFieldCodecTest::RunTest(const FString&)
{
	using namespace UeAgentPropertyValueCodec;
	FOptions Options; Options.bAllowPartialStructFields = true; Options.bCaptureStructFieldDescriptors = true;
	Options.StructFieldFilter = [](const UScriptStruct* Struct, const FProperty* Field) {
		return IsStructFieldAuthoringEditable(Field) && !(Struct == FMovieSceneDoubleValue::StaticStruct() && Field->GetFName() == TEXT("Value"));
	};
	FMovieSceneDoubleValue Native(1.0000000000000002); Native.Tangent.ArriveTangent = 7.25f; Native.Tangent.LeaveTangent = -8.5f;
	TSharedPtr<FJsonObject> Definition;
	const auto Encoded = EncodeStructValue(FMovieSceneDoubleValue::StaticStruct(), &Native, nullptr, Options, 0, &Definition);
	const auto Fields = Encoded->AsObject()->GetObjectField(TEXT("fields"));
	TestFalse(TEXT("excluded value has one separate owner"), Fields->HasField(TEXT("Value")));
	TestFalse(TEXT("noneditable native padding is not authorable"), Fields->HasField(TEXT("PaddingByte")));
	if (!TestTrue(TEXT("struct definitions include typed enum descriptor and candidates"), Definition.IsValid())) return false;
	const auto Interp = Definition->GetObjectField(TEXT("fields"))->GetObjectField(TEXT("InterpMode"));
	TestEqual(TEXT("enum descriptor uses shared type"), Interp->GetObjectField(TEXT("type"))->GetStringField(TEXT("kind")), FString(TEXT("enum")));
	TestEqual(TEXT("ordinary candidates remain names only"), Interp->GetArrayField(TEXT("candidates_header")).Num(), 1);
	bool bConstant = false;
	for (const auto& Row : Interp->GetArrayField(TEXT("candidates"))) bConstant |= Row->AsArray()[0]->AsString() == TEXT("RCIM_Constant");
	TestTrue(TEXT("shared candidates include native constant mode"), bConstant);
	FMovieSceneDoubleValue Desired = Native; FString Error;
	TestTrue(TEXT("filtered complete encode decodes symmetrically"), DecodeStructValue(FMovieSceneDoubleValue::StaticStruct(), Encoded, &Desired, nullptr, Options, Error));
	TestTrue(TEXT("complete round trip retains excluded native value"), Desired == Native);
	const auto Patch = MakeShared<FJsonObject>(); const auto PatchFields = MakeShared<FJsonObject>();
	const auto Tangent = MakeShared<FJsonObject>(); const auto TangentFields = MakeShared<FJsonObject>(); const auto Number = MakeShared<FJsonObject>();
	Number->SetNumberField(TEXT("literal"), 9.5); TangentFields->SetObjectField(TEXT("ArriveTangent"), Number);
	Tangent->SetObjectField(TEXT("fields"), TangentFields); PatchFields->SetObjectField(TEXT("Tangent"), Tangent); Patch->SetObjectField(TEXT("fields"), PatchFields);
	TestTrue(TEXT("recursive sparse decode accepts one tangent member"), DecodeStructValue(FMovieSceneDoubleValue::StaticStruct(), MakeShared<FJsonValueObject>(Patch), &Desired, nullptr, Options, Error));
	TestTrue(TEXT("sparse decode preserves every unrelated value"), Desired.Tangent.ArriveTangent == 9.5f && Desired.Tangent.LeaveTangent == Native.Tangent.LeaveTangent && Desired.Value == Native.Value && Desired.InterpMode == Native.InterpMode);
	PatchFields->SetObjectField(TEXT("Value"), Number);
	TestFalse(TEXT("filtered field cannot be reintroduced"), DecodeStructValue(FMovieSceneDoubleValue::StaticStruct(), MakeShared<FJsonValueObject>(Patch), &Desired, nullptr, Options, Error));
	TestTrue(TEXT("filtered field rejection is explicit"), Error.Contains(TEXT("filtered_struct_field_Value")));
	FOptions Full;
	// The generic codec retains its existing canonical-number policy.
	FMovieSceneDoubleValue FullNative = Native; FullNative.Value = 1.25;
	const auto FullValue = EncodeStructValue(FMovieSceneDoubleValue::StaticStruct(), &FullNative, nullptr, Full);
	TestTrue(TEXT("empty filter keeps historical full value surface"), FullValue->AsObject()->GetObjectField(TEXT("fields"))->HasField(TEXT("Value")));
	FMovieSceneDoubleValue FullRead;
	TestTrue(TEXT("empty filter full decode unchanged"), DecodeStructValue(FMovieSceneDoubleValue::StaticStruct(), FullValue, &FullRead, nullptr, Full, Error) && FullRead == FullNative);
	const auto* Enum = UeAgentAuthoringTypeDescriptors::GetPropertyEnum(FindFProperty<FProperty>(FMovieSceneDoubleValue::StaticStruct(), TEXT("InterpMode")));
	const auto Names = UeAgentAuthoringTypeDescriptors::BuildEnumCandidates(Enum);
	const auto Rich = UeAgentAuthoringTypeDescriptors::BuildEnumCandidates(Enum, true);
	TestEqual(TEXT("rich and names-only consumers share the same filtered inventory"), Names->GetArrayField(TEXT("candidates")).Num(), Rich->GetArrayField(TEXT("candidates")).Num());
	for (int32 Index = 0; Index < Names->GetArrayField(TEXT("candidates")).Num(); ++Index)
		TestEqual(TEXT("candidate order and names are shared"), Names->GetArrayField(TEXT("candidates"))[Index]->AsArray()[0]->AsString(), Rich->GetArrayField(TEXT("candidates"))[Index]->AsArray()[0]->AsString());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentEnumValueCodecTest,
	"UeAgentInterface.Utilities.PropertyValueCodec.EnumValue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentEnumValueCodecTest::RunTest(const FString&)
{
	using namespace UeAgentPropertyValueCodec;
	auto* Field = FindFProperty<FProperty>(FMovieSceneDoubleValue::StaticStruct(), TEXT("InterpMode"));
	const auto* Enum = UeAgentAuthoringTypeDescriptors::GetPropertyEnum(Field);
	FMovieSceneDoubleValue Native; Native.InterpMode = RCIM_Constant; FOptions Options; FString Error;
	const auto Encoded = Encode(Field, Field->ContainerPtrToValuePtr<void>(&Native), nullptr, Options);
	TestEqual(TEXT("property and scalar encode share native enum name"), Encoded->AsObject()->GetStringField(TEXT("selected")), EncodeEnumValueName(Enum, RCIM_Constant));
	int64 Raw = 0;
	TestTrue(TEXT("shared enum accepts selected object"), DecodeEnumValue(Enum, Encoded, Raw, Error) && Raw == RCIM_Constant);
	const auto Linear = MakeShared<FJsonValueString>(TEXT("RCIM_Linear"));
	TestTrue(TEXT("shared enum accepts native string name"), DecodeEnumValue(Enum, Linear, Raw, Error) && Raw == RCIM_Linear);
	TestTrue(TEXT("property decoder consumes same enum contract"), Decode(Field, Linear, Field->ContainerPtrToValuePtr<void>(&Native), nullptr, Options, Error) && Native.InterpMode == RCIM_Linear);
	TestFalse(TEXT("unknown names never fall back to numbers"), DecodeEnumValue(Enum, MakeShared<FJsonValueString>(TEXT("12345")), Raw, Error));
	Encoded->AsObject()->SetBoolField(TEXT("extra"), true);
	TestFalse(TEXT("enum envelope keeps exact member policy"), DecodeEnumValue(Enum, Encoded, Raw, Error));
	TestEqual(TEXT("unnamed raw value has no invented selection"), EncodeEnumValueName(Enum, 12345), FString());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentReferenceValueCodecTest,
	"UeAgentInterface.Utilities.PropertyValueCodec.ReferenceValue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentReferenceValueCodecTest::RunTest(const FString&)
{
	using namespace UeAgentPropertyValueCodec;
	auto* Host = NewObject<UUeAgentT167AssetReferenceData>(); auto* Asset = NewObject<UUeAgentT167AssetReferenceData>();
	auto* Hard = FindFProperty<FObjectPropertyBase>(Host->GetClass(), TEXT("HardReference"));
	auto* Soft = FindFProperty<FSoftObjectProperty>(Host->GetClass(), TEXT("SoftReference"));
	if (!Hard || !Soft) return false;
	FOptions Options; FString Error; UObject* Resolved = nullptr;
	const auto Reference = EncodeReferencePath(Asset->GetPathName());
	TestTrue(TEXT("common hard resolver and real FProperty decode agree"), DecodeObjectReference(Reference, UObject::StaticClass(), nullptr, Resolved, Error)
		&& Resolved == Asset && Decode(Hard, Reference, Hard->ContainerPtrToValuePtr<void>(Host), Host, Options, Error) && Host->HardReference == Asset);
	const auto Encoded = Encode(Hard, Hard->ContainerPtrToValuePtr<void>(Host), Host, Options);
	TestEqual(TEXT("real object encoder shares ordinary path shape"), Encoded->AsObject()->GetStringField(TEXT("path")), Asset->GetPathName());
	auto* Redirector = NewObject<UObjectRedirector>(); Redirector->DestinationObject = Asset;
	TestTrue(TEXT("shared hard resolver follows loaded redirector"), DecodeObjectReference(EncodeReferencePath(Redirector->GetPathName()), UObject::StaticClass(), nullptr, Resolved, Error) && Resolved == Asset);
	const auto Null = EncodeReferencePath(TEXT(""));
	TestTrue(TEXT("null reference clears real hard property"), Decode(Hard, Null, Hard->ContainerPtrToValuePtr<void>(Host), Host, Options, Error) && !Host->HardReference);
	const FString Missing = TEXT("/Game/UAI/Transient/MissingSoftCodec.MissingSoftCodec");
	TestTrue(TEXT("soft property retains dangling path without hard loading"), Decode(Soft, EncodeReferencePath(Missing), Soft->ContainerPtrToValuePtr<void>(Host), Host, Options, Error)
		&& Host->SoftReference.ToString() == Missing && !Host->SoftReference.IsValid() && !FindObject<UObject>(nullptr, *Missing));
	TestEqual(TEXT("soft encoder shares the same path envelope"), Encode(Soft, Soft->ContainerPtrToValuePtr<void>(Host), Host, Options)->AsObject()->GetStringField(TEXT("path")), Missing);
	auto* Graph = NewObject<UEdGraph>(); auto* Class = FindFProperty<FClassProperty>(UEdGraph::StaticClass(), TEXT("Schema")); if (!Class) return false;
	TestTrue(TEXT("real class property and shared MetaClass constraint accept valid class"), Decode(Class, EncodeReferencePath(UEdGraphSchema::StaticClass()->GetPathName()), Class->ContainerPtrToValuePtr<void>(Graph), Graph, Options, Error)
		&& Graph->Schema == UEdGraphSchema::StaticClass());
	TestFalse(TEXT("real class property rejects unrelated class"), Decode(Class, EncodeReferencePath(UDataAsset::StaticClass()->GetPathName()), Class->ContainerPtrToValuePtr<void>(Graph), Graph, Options, Error));
	TestFalse(TEXT("direct common class resolver rejects the same unrelated class"), DecodeObjectReference(EncodeReferencePath(UDataAsset::StaticClass()->GetPathName()), UClass::StaticClass(), UEdGraphSchema::StaticClass(), Resolved, Error));
	bool bCustomCalled = false; Options.ObjectReferenceEncoder = [&](const FObjectPropertyBase*, const void*, UObject*, int32, TSharedPtr<FJsonValue>& Out, TSharedPtr<FJsonObject>*) { bCustomCalled = true; Out = MakeShared<FJsonValueString>(TEXT("owned-contract")); return true; };
	TestTrue(TEXT("owned reference encoder remains a distinct prior contract"), Encode(Hard, Hard->ContainerPtrToValuePtr<void>(Host), Host, Options)->AsString() == TEXT("owned-contract") && bCustomCalled);
	return !HasAnyErrors();
}
