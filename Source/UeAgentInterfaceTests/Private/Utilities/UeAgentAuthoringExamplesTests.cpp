#include "Misc/AutomationTest.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringExamples.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "Utilities/AuthoringObjectApply/UeAgentObjectApplyNotificationTestTypes.h"
#include "Dom/JsonValue.h"
#include "Utilities/UeAgentAuthoringExampleTestTypes.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentAuthoringExampleNativeInputTest,
	"UeAgentInterface.HjsonExamples.NativeInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentAuthoringExampleNativeInputTest::RunTest(const FString&)
{
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UeAgentAuthoringObjectApply;
	const FAuthoringExampleDefinitionResolver Resolve = [](const FString&) { return TSharedPtr<FJsonObject>(); };
	auto ApplyExample = [&](UObject* Host, const TCHAR* FieldName)
	{
		auto* Field = FindFProperty<FProperty>(Host->GetClass(), FieldName);
		if (!TestNotNull(FieldName, Field)) return;
		UeAgentPropertyValueCodec::FOptions Capture;
		Capture.bCaptureStructFieldDescriptors = true;
		Capture.StructFieldFilter = [](const UScriptStruct*, const FProperty* P) {
			return UeAgentPropertyValueCodec::IsStructFieldAuthoringEditable(P);
		};
		TSharedPtr<FJsonObject> ValueDefinition;
		UeAgentPropertyValueCodec::Encode(Field, Field->ContainerPtrToValuePtr<void>(Host), Host, Capture, 0, &ValueDefinition);
		const auto Definition = MakeShared<FJsonObject>();
		Definition->SetObjectField(TEXT("type"), UeAgentPropertyValueCodec::BuildTypeDescriptor(Field));
		if (const auto Enum = UeAgentAuthoringTypeDescriptors::BuildEnumCandidates(
			UeAgentAuthoringTypeDescriptors::GetPropertyEnum(Field), false, Field))
			Definition->SetField(TEXT("candidates"), Enum->TryGetField(TEXT("candidates")));
		if (ValueDefinition) Definition->SetObjectField(TEXT("value_definition"), ValueDefinition);
		const auto Recipe = BuildAuthoringTypeExamples(Definition, Resolve);
		const auto Edit = MakeShared<FJsonObject>();
		Edit->SetStringField(TEXT("property_path"), FieldName);
		Edit->SetField(TEXT("value"), Recipe->TryGetField(TEXT("example")));
		TArray<TSharedPtr<FJsonValue>> Edits{MakeShared<FJsonValueObject>(Edit)};
		FOptions Options; Options.bCompactValues = true; Options.bEmitEditorNotifications = false;
		Options.bMarkPackageDirty = false; Options.bRecordOwnerTransaction = false;
		FPreparedApply Prepared; FFailure Failure; FApplyResult Result;
		const bool Ready = PrepareObject(Host, Edits, Options, Prepared, Failure);
		TestTrue(FString(FieldName) + TEXT(" example prepares through common public value conversion: ") + Failure.Diagnostic.Code + TEXT(" ") + Failure.Diagnostic.PublicMessage, Ready);
		if (Ready) TestTrue(FString(FieldName) + TEXT(" example applies natively"), ApplyPrepared(Prepared, Result));
	};
	auto* Basic = NewObject<UUeAgentActionableProjectionTarget>();
	for (const TCHAR* Field : {TEXT("EmptySet"), TEXT("EmptyMap"), TEXT("Optional"), TEXT("Bag"), TEXT("Color"), TEXT("FixedMixed")}) ApplyExample(Basic, Field);
	TestEqual(TEXT("empty set example inserts a native element"), Basic->EmptySet.Num(), 1);
	TestEqual(TEXT("empty map example inserts a native element"), Basic->EmptyMap.Num(), 1);
	TestTrue(TEXT("optional example initializes native value"), Basic->Optional.IsSet());
	auto* Nested = NewObject<UUeAgentArrayOptionalProjectionTarget>();
	ApplyExample(Nested, TEXT("Records")); ApplyExample(Nested, TEXT("Current"));
	TestEqual(TEXT("array example creates struct member"), Nested->Records.Num(), 1);
	if (!Nested->Records.IsEmpty()) TestEqual(TEXT("native readonly defaults survive sparse example"), Nested->Records[0].Empty.Nested.Kept, 73);
	auto* Hashed = NewObject<UUeAgentReadOnlyHashIdentityTarget>();
	ApplyExample(Hashed, TEXT("Records")); ApplyExample(Hashed, TEXT("Lookup"));
	TestEqual(TEXT("new hashed set uses native hash identity"), Hashed->Records.Num(), 1);
	TestEqual(TEXT("new hashed map uses native hash identity"), Hashed->Lookup.Num(), 1);
	auto* Values = NewObject<UUeAgentExampleValueTarget>();
	ApplyExample(Values, TEXT("Rich")); ApplyExample(Values, TEXT("Simple"));
	ApplyExample(Values, TEXT("Runtime")); ApplyExample(Values, TEXT("Flags"));
	TestEqual(TEXT("rich curve example inserts a native key"), Values->Rich.GetNumKeys(), 1);
	TestEqual(TEXT("simple curve example inserts a native key"), Values->Simple.GetNumKeys(), 1);
	TestEqual(TEXT("runtime curve example inserts a native key"), Values->Runtime.GetRichCurveConst()->GetNumKeys(), 1);
	TestEqual(TEXT("bitmask example selects a declared native flag"), Values->Flags, 1);
	return !HasAnyErrors();
}
