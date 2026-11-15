#include "Misc/AutomationTest.h"

#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceResolver.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentNumericValueComparisonTest,
	"UeAgentInterface.Core.StateSemantic.NumericValueComparison",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentNumericValueComparisonTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI::StateSemantic;
	UE::UAI::ContextResource::FUeAgentResourceTracker Resources;
	FUeAgentStateSemanticError Error;
	struct FCase { const TCHAR* Integer; const TCHAR* Decimal; bool bEqual; };
	const FCase Cases[] = {
		{TEXT("0"), TEXT("0"), true},
		{TEXT("0"), TEXT("-0.0"), true},
		{TEXT("100"), TEXT("1e2"), true},
		{TEXT("-12"), TEXT("-12.00"), true},
		{TEXT("9007199254740993"), TEXT("9007199254740993.0"), true},
		{TEXT("9007199254740993"), TEXT("9007199254740992.0"), false},
		{TEXT("1"), TEXT("1.00000000000000000001"), false},
	};
	for (const FCase& Case : Cases)
	{
		FUeAgentCanonicalValuePtr Integer, Decimal;
		if (!TestTrue(TEXT("integer construction"), FUeAgentCanonicalValue::TryInteger(Case.Integer, Resources, Integer, Error))
			|| !TestTrue(TEXT("decimal construction"), FUeAgentCanonicalValue::TryDecimal(Case.Decimal, Resources, Decimal, Error)))
			return false;
		const FString IntegerKey = UeAgentAuthoringState::MakeStableCanonicalValueKey(MakeShared<FJsonValueNumberString>(Case.Integer));
		const FString DecimalKey = UeAgentAuthoringState::MakeStableCanonicalValueKey(MakeShared<FJsonValueNumberString>(Case.Decimal));
		TestEqual(TEXT("stable key follows exact numeric equality"), IntegerKey == DecimalKey, Case.bEqual);
		TestEqual(TEXT("snapshot equality follows exact numeric equality"),
			UeAgentAuthoringState::AreStableCanonicalValuesEqual(MakeShared<FJsonValueNumberString>(Case.Integer),
				MakeShared<FJsonValueNumberString>(Case.Decimal)), Case.bEqual);
		for (bool bReverse : {false, true})
		{
			FUeAgentCanonicalValuePtr Left, Right;
			// Exercise the same recursive object/array boundary as workspace units.
			FUeAgentCanonicalValue::TryArray({bReverse ? Decimal.ToSharedRef() : Integer.ToSharedRef()}, Resources, Left, Error);
			FUeAgentCanonicalValue::TryArray({bReverse ? Integer.ToSharedRef() : Decimal.ToSharedRef()}, Resources, Right, Error);
			FUeAgentCanonicalValuePtr LeftObject, RightObject;
			FUeAgentCanonicalValue::TryObject({{TEXT("values"), Left.ToSharedRef()}}, Resources, LeftObject, Error);
			FUeAgentCanonicalValue::TryObject({{TEXT("values"), Right.ToSharedRef()}}, Resources, RightObject, Error);
			FUeAgentSemanticComparison Comparison;
			if (!TestTrue(TEXT("compare numeric containers"), TryCompareValues(LeftObject.ToSharedRef(), RightObject.ToSharedRef(), TEXT("/value"), Resources, Comparison, Error)))
				return false;
			TestEqual(*FString::Printf(TEXT("exact equality %s / %s"), Case.Integer, Case.Decimal), Comparison.IsEquivalent(), Case.bEqual);
			if (!Case.bEqual && Comparison.GetDifferences().Num() == 1)
			{
				TestEqual(TEXT("numeric difference kind"), Comparison.GetDifferences()[0].GetKind(), EUeAgentDifferenceKind::ValueMismatch);
				TestEqual(TEXT("numeric difference path"), Comparison.GetDifferences()[0].GetPath(), FString(TEXT("/value/values/0")));
			}
		}
	}

	FUeAgentCanonicalValuePtr Native, Parsed, Text;
	FString JsonError;
	if (!TestTrue(TEXT("native JSON number"), CanonicalJson::TryFromJsonValue(MakeShared<FJsonValueNumber>(0.0), Resources, Native, JsonError))
		|| !TestTrue(TEXT("HJSON exact number"), CanonicalJson::TryFromJsonValue(MakeShared<FJsonValueNumberString>(TEXT("0")), Resources, Parsed, JsonError))
		|| !TestTrue(TEXT("string construction"), FUeAgentCanonicalValue::TryString(TEXT("0"), Resources, Text, Error)))
		return false;
	TestEqual(TEXT("native representation retained"), Native->GetKind(), EUeAgentValueKind::Decimal);
	TestEqual(TEXT("parsed representation retained"), Parsed->GetKind(), EUeAgentValueKind::Integer);
	TestTrue(TEXT("transport numeric values equivalent"), CompareValues(Native.ToSharedRef(), Parsed.ToSharedRef(), TEXT("/value"), Resources).IsEquivalent());
	const auto StringComparison = CompareValues(Text.ToSharedRef(), Parsed.ToSharedRef(), TEXT("/value"), Resources);
	TestFalse(TEXT("number is not text"), StringComparison.IsEquivalent());
	if (StringComparison.GetDifferences().Num() == 1)
		TestEqual(TEXT("non-numeric type mismatch retained"), StringComparison.GetDifferences()[0].GetKind(), EUeAgentDifferenceKind::TypeMismatch);

	const FUeAgentCanonicalIdentity Identity(TEXT("core.property"), TEXT("property"), TEXT("/Game/Fixture"), TEXT("Value"), TEXT("object"), TEXT("generation"));
	const FUeAgentCanonicalType Type(TEXT("core.integer"), EUeAgentValueKind::Integer, false, TEXT("scalar"));
	const FUeAgentDefaultValue Default(EUeAgentDefaultSourceKind::ClassDefaultObject, TEXT("/Game/Fixture"), TEXT("fixture"), Parsed.ToSharedRef());
	const FUeAgentStateDetails Details(TEXT("Value"), TEXT(""), {}, true, TEXT("available"), {});
	const FUeAgentStateRevision Revision(TEXT("fixture"), TEXT("generation"), 1);
	TestTrue(TEXT("matching declared type valid"), FUeAgentStateEntry(Identity, Type, Parsed.ToSharedRef(), Default, Details, Revision).IsValid());
	TestFalse(TEXT("declared type validation remains strict"), FUeAgentStateEntry(Identity, Type, Native.ToSharedRef(), Default, Details, Revision).IsValid());

	// Keep ordinary native revision material stable while preserving exact text
	// at the large-integer, overflow and underflow boundaries.
	for (double Number : {0.0, 1.0, -0.5, 100.0, 1e100})
		TestEqual(TEXT("ordinary native key retained"),
			UeAgentAuthoringState::MakeStableCanonicalValueKey(MakeShared<FJsonValueNumber>(Number)),
			TEXT("n") + FString::Printf(TEXT("%.17g"), Number));
	for (const auto& Pair : {TPair<const TCHAR*, const TCHAR*>(TEXT("1e500"), TEXT("10e499")),
		TPair<const TCHAR*, const TCHAR*>(TEXT("1e-500"), TEXT("0"))})
	{
		const bool bEqual = FCString::Strcmp(Pair.Value, TEXT("0")) != 0;
		TestEqual(TEXT("exact exponent key does not overflow or underflow through double"),
			UeAgentAuthoringState::MakeStableCanonicalValueKey(MakeShared<FJsonValueNumberString>(Pair.Key)) ==
			UeAgentAuthoringState::MakeStableCanonicalValueKey(MakeShared<FJsonValueNumberString>(Pair.Value)), bEqual);
	}
	TestFalse(TEXT("stable key retains numeric versus string type"),
		UeAgentAuthoringState::MakeStableCanonicalValueKey(MakeShared<FJsonValueString>(TEXT("0"))) ==
		UeAgentAuthoringState::MakeStableCanonicalValueKey(MakeShared<FJsonValueNumberString>(TEXT("0"))));

	using namespace UE::UAI::HjsonWorkspace;
	FHjsonDocumentPtr Document;
	FWorkspaceError WorkspaceError;
	if (!TestTrue(TEXT("actual HJSON parser"), TryParseHjsonDocument(
		TEXT("{a:true, nil:null, text:\"0\", z:[0,1e2,9007199254740993]}"), TEXT("numeric-alignment.hjson"), Resources, Document, WorkspaceError)))
		return false;
	FUeAgentCanonicalValuePtr HjsonValue, ProjectedValue, NativeObjectValue;
	TSharedPtr<FJsonValue> ProjectedJson;
	if (!TestTrue(TEXT("actual HJSON canonical projection"), TryProjectHjsonCanonicalValue(Document->GetRoot().Get(), Resources, HjsonValue, WorkspaceError))
		|| !TestTrue(TEXT("HJSON JSON projection retains numeric text"), TryProjectHjsonNodeToJsonValue(Document->GetRoot().Get(), Resources, ProjectedJson, WorkspaceError))
		|| !TestTrue(TEXT("projected JSON canonical conversion"), CanonicalJson::TryFromJsonValue(ProjectedJson, Resources, ProjectedValue, JsonError)))
		return false;
	TestTrue(TEXT("HJSON direct and JSON projection agree"), CompareValues(HjsonValue.ToSharedRef(), ProjectedValue.ToSharedRef(), TEXT("/value"), Resources).IsEquivalent());
	const auto NativeObject = MakeShared<FJsonObject>();
	NativeObject->SetArrayField(TEXT("z"), {MakeShared<FJsonValueNumber>(0.0), MakeShared<FJsonValueNumber>(100.0), MakeShared<FJsonValueNumberString>(TEXT("9007199254740993"))});
	NativeObject->SetStringField(TEXT("text"), TEXT("0"));
	NativeObject->SetField(TEXT("nil"), MakeShared<FJsonValueNull>());
	NativeObject->SetBoolField(TEXT("a"), true);
	const TSharedPtr<FJsonValue> NativeJson = MakeShared<FJsonValueObject>(NativeObject);
	if (!TestTrue(TEXT("native JSON canonical conversion"), CanonicalJson::TryFromJsonValue(NativeJson, Resources, NativeObjectValue, JsonError)))
		return false;
	TestTrue(TEXT("native and HJSON values agree across kinds and member order"), CompareValues(HjsonValue.ToSharedRef(), NativeObjectValue.ToSharedRef(), TEXT("/value"), Resources).IsEquivalent());
	TestEqual(TEXT("native and HJSON stable keys agree recursively"), UeAgentAuthoringState::MakeStableCanonicalValueKey(NativeJson), UeAgentAuthoringState::MakeStableCanonicalValueKey(ProjectedJson));
	TestTrue(TEXT("snapshot exact equality ignores object insertion order"), UeAgentAuthoringState::AreStableCanonicalValuesEqual(NativeJson, ProjectedJson));
	TestEqual(TEXT("clone preserves exact key"), UeAgentAuthoringState::MakeStableCanonicalValueKey(UeAgentAuthoringState::CloneCanonicalValue(ProjectedJson)), UeAgentAuthoringState::MakeStableCanonicalValueKey(ProjectedJson));
	const auto Forward = MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueNumber>(1.0), MakeShared<FJsonValueNumber>(2.0)});
	const auto Reverse = MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueNumber>(2.0), MakeShared<FJsonValueNumber>(1.0)});
	TestFalse(TEXT("ordered arrays remain different"), UeAgentAuthoringState::MakeStableCanonicalValueKey(Forward) == UeAgentAuthoringState::MakeStableCanonicalValueKey(Reverse));
	TestFalse(TEXT("snapshot equality retains array order"), UeAgentAuthoringState::AreStableCanonicalValuesEqual(Forward, Reverse));
	const auto Upper = MakeShared<FJsonObject>(); Upper->SetField(TEXT("Value"), MakeShared<FJsonValueNull>());
	const auto Lower = MakeShared<FJsonObject>(); Lower->SetField(TEXT("value"), MakeShared<FJsonValueNull>());
	TestFalse(TEXT("snapshot object keys are case sensitive"), UeAgentAuthoringState::AreStableCanonicalValuesEqual(
		MakeShared<FJsonValueObject>(Upper), MakeShared<FJsonValueObject>(Lower)));
	TestFalse(TEXT("snapshot absent member differs from null"), UeAgentAuthoringState::AreStableCanonicalValuesEqual(
		MakeShared<FJsonValueObject>(Upper), MakeShared<FJsonValueObject>(MakeShared<FJsonObject>())));
	TestFalse(TEXT("snapshot missing value differs from JSON null"), UeAgentAuthoringState::AreStableCanonicalValuesEqual(nullptr, MakeShared<FJsonValueNull>()));
	TestFalse(TEXT("snapshot case-only string edit is not discarded"), UeAgentAuthoringState::AreStableCanonicalValuesEqual(
		MakeShared<FJsonValueString>(TEXT("Blue")), MakeShared<FJsonValueString>(TEXT("blue"))));
	// Preserve the pre-existing malformed-cycle behavior; no cyclic data is
	// admitted to the canonical model by this equality helper.
	const auto CycleA = MakeShared<FJsonObject>(); const auto CycleB = MakeShared<FJsonObject>();
	CycleA->SetObjectField(TEXT("self"), CycleA); CycleB->SetObjectField(TEXT("self"), CycleB);
	TestTrue(TEXT("snapshot malformed cycles retain legacy key comparison"), UeAgentAuthoringState::AreStableCanonicalValuesEqual(
		MakeShared<FJsonValueObject>(CycleA), MakeShared<FJsonValueObject>(CycleB)));
	CycleA->RemoveField(TEXT("self")); CycleB->RemoveField(TEXT("self"));
	TestFalse(TEXT("HJSON nonfinite literal rejected"), TryParseHjsonDocument(TEXT("{value:NaN}"), TEXT("numeric-invalid.hjson"), Resources, Document, WorkspaceError));
	TestFalse(TEXT("native JSON nonfinite number rejected"), CanonicalJson::TryFromJsonValue(MakeShared<FJsonValueNumber>(std::numeric_limits<double>::infinity()), Resources, Parsed, JsonError));
	return true;
}
