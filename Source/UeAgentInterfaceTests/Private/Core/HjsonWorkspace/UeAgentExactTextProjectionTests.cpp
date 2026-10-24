#include "Misc/AutomationTest.h"

#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"
#include "Core/HjsonWorkspace/UeAgentKeyedItemAuthoringProjection.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Dom/JsonValue.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentExactTextProjectionTest,
	"UeAgentInterface.Core.HjsonWorkspace.ExactTextProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentExactTextProjectionTest::RunTest(const FString&)
{
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::StateSemantic;
	auto Resources = MakeSemanticWorkspaceResourceTracker();
	FHjsonDocumentPtr Document;
	FWorkspaceError Error;
	if (!TestTrue(TEXT("AST preserves exact-case keys"), TryParseHjsonDocument(
		TEXT("{Case: \"Upper\", case: \"upper\"}"), TEXT("exact.hjson"), Resources, Document, Error))) return false;
	const auto* Upper = Document->GetRoot()->Get(TEXT("Case"));
	const auto* Lower = Document->GetRoot()->Get(TEXT("case"));
	if (!TestTrue(TEXT("exact AST lookup separates values"), Upper && Lower && Upper != Lower
		&& Upper->GetText().Equals(TEXT("Upper"), ESearchCase::CaseSensitive)
		&& Lower->GetText().Equals(TEXT("upper"), ESearchCase::CaseSensitive))) return false;
	TSharedPtr<FJsonValue> Json = MakeShared<FJsonValueNull>();
	TestFalse(TEXT("AST refuses a lossy JSON object"), TryProjectHjsonNodeToJsonValue(*Document->GetRoot(), Resources, Json, Error));
	TestTrue(TEXT("AST JSON failure publishes no partial value"), !Json.IsValid());
	TestEqual(TEXT("AST JSON collision is explicit"), Error.GetCode(), FString(TEXT("hjson_json_object_key_collision")));
	TestFalse(TEXT("exact duplicate is rejected"), TryParseHjsonDocument(
		TEXT("{Case: 1, Case: 2}"), TEXT("duplicate.hjson"), Resources, Document, Error));
	TestTrue(TEXT("duplicate parse publishes no partial document"), !Document.IsValid());
	TestEqual(TEXT("duplicate has the grammar error"), Error.GetCode(), FString(TEXT("hjson_duplicate_key")));
	FUeAgentStateSemanticError StateError;
	FUeAgentCanonicalValuePtr UpperValue, LowerValue, Object;
	if (!FUeAgentCanonicalValue::TryString(TEXT("Upper"), Resources, UpperValue, StateError)
		|| !FUeAgentCanonicalValue::TryString(TEXT("upper"), Resources, LowerValue, StateError)) return false;
	FUeAgentSemanticComparison Comparison;
	if (!TestTrue(TEXT("canonical string comparison succeeds"), TryCompareValues(UpperValue.ToSharedRef(),
		LowerValue.ToSharedRef(), TEXT("/source"), Resources, Comparison, StateError))) return false;
	TestFalse(TEXT("case-only source is a semantic change"), Comparison.IsEquivalent());
	if (!TestTrue(TEXT("canonical object preserves exact-case keys"), FUeAgentCanonicalValue::TryObject(
		{{TEXT("Case"), UpperValue.ToSharedRef()}, {TEXT("case"), LowerValue.ToSharedRef()}}, Resources, Object, StateError))) return false;
	FString Detail;
	Json = MakeShared<FJsonValueNull>();
	TestFalse(TEXT("canonical refuses a lossy JSON object"), CanonicalJson::TryToJsonValue(*Object, Json, Detail));
	TestTrue(TEXT("canonical JSON failure publishes no partial value"), !Json.IsValid());
	TestTrue(TEXT("canonical JSON collision is explicit"), Detail.StartsWith(TEXT("canonical_json_object_key_collision:")));
	const FSemanticUnit IncompatibleUnit(TEXT("fixture:collision"), ESemanticUnitKind::StructureState,
		TEXT("fixture:owner"), TEXT("fixture.semantic"), TEXT("structure"), FString(),
		{{TEXT("data"), Object.ToSharedRef()}}, {}, {}, FString(), FString(), {}, {TEXT("llm_authoring")}, FString());
	if (!TestTrue(TEXT("collision fixture is a valid semantic unit"), IncompatibleUnit.IsValid())) return false;
	TArray<uint8> RejectedBytes{1};
	TestFalse(TEXT("hidden semantic row rejects incompatible JSON projection"), TrySerializeSemanticUnitRow(IncompatibleUnit, RejectedBytes, Error));
	TestTrue(TEXT("hidden row failure publishes no bytes"), RejectedBytes.IsEmpty());
	RejectedBytes.Add(1);
	TestFalse(TEXT("model row rejects incompatible JSON projection"), TrySerializeSemanticModelUnitRow(IncompatibleUnit, RejectedBytes, Error));
	TestTrue(TEXT("model row failure publishes no bytes"), RejectedBytes.IsEmpty());
	RejectedBytes.Add(1);
	TestFalse(TEXT("unit document rejects incompatible JSON projection"), TrySerializeSemanticUnitDocument(IncompatibleUnit, RejectedBytes, Error));
	TestTrue(TEXT("unit document failure publishes no bytes"), RejectedBytes.IsEmpty());
	FString Text;
	if (!TestTrue(TEXT("original writer accepts canonical object without JSON projection"),
		FUeAgentStructuredHjsonFormatter::TryWriteValue(*Object, Text, Detail))) return false;
	if (!TestTrue(TEXT("canonical formatted output parses"), TryParseHjsonDocument(Text, TEXT("formatted.hjson"), Resources, Document, Error))) return false;
	Upper = Document->GetRoot()->Get(TEXT("Case")); Lower = Document->GetRoot()->Get(TEXT("case"));
	TestTrue(TEXT("formatted output retains both keys and exact values"), Document->GetRoot()->GetEntries().Num() == 2
		&& Upper && Lower && Upper != Lower && Upper->GetText().Equals(TEXT("Upper"), ESearchCase::CaseSensitive)
		&& Lower->GetText().Equals(TEXT("upper"), ESearchCase::CaseSensitive));
	TestFalse(TEXT("canonical exact duplicate rejects"), FUeAgentCanonicalValue::TryObject(
		{{TEXT("Case"), UpperValue.ToSharedRef()}, {TEXT("Case"), LowerValue.ToSharedRef()}}, Resources, Object, StateError));
	TestTrue(TEXT("canonical duplicate publishes no partial value"), !Object.IsValid());
	TestTrue(TEXT("nonempty whitespace object key is preserved"), FUeAgentCanonicalValue::TryObject(
		{{TEXT(" "), UpperValue.ToSharedRef()}}, Resources, Object, StateError));
	TestTrue(TEXT("whitespace key is not trimmed"), Object.IsValid() && Object->GetObjectItems().Num() == 1
		&& Object->GetObjectItems()[0].Key.Equals(TEXT(" "), ESearchCase::CaseSensitive));
	const FUeAgentTemporalPoint Time(TEXT("fixture-timeline"), TEXT("24000"), TEXT("24000"), TEXT("1001"), TEXT("authored"));
	const FUeAgentSpatialFrame Frame(TEXT("fixture-local"), TEXT("fixture-world"), 3,
		{{TEXT("1"), TEXT("0"), TEXT("0")}, {TEXT("0"), TEXT("1"), TEXT("0")}, {TEXT("0"), TEXT("0"), TEXT("1")}},
		{TEXT("0"), TEXT("0"), TEXT("0")}, TEXT("cm"), TEXT("fixture:declared-frame"), Time);
	const FUeAgentSpatialValue Position(TEXT("fixture-node"), EUeAgentSpatialQuantityKind::Point, Frame,
		{TEXT("320"), TEXT("-96"), TEXT("0")}, Time, {TEXT("fixture:direct")});
	FUeAgentCanonicalValuePtr Decimal, Temporal, Spatial;
	if (!FUeAgentCanonicalValue::TryDecimal(TEXT("7"), Resources, Decimal, StateError)
		|| !FUeAgentCanonicalValue::TryTemporal(Time, Resources, Temporal, StateError)
		|| !FUeAgentCanonicalValue::TrySpatial(Position, Resources, Spatial, StateError)
		|| !FUeAgentCanonicalValue::TryObject({{TEXT("decimal"), Decimal.ToSharedRef()},
			{TEXT("temporal"), Temporal.ToSharedRef()}, {TEXT("spatial"), Spatial.ToSharedRef()}}, Resources, Object, StateError)) return false;
	Json = MakeShared<FJsonValueNull>();
	TestFalse(TEXT("typed allocation preflight rejects before publication"),
		CanonicalJson::TryProjectTypedLeaf(*Spatial, [](int64) { return false; }, Json, Detail));
	TestTrue(TEXT("typed allocation rejection leaves no partial leaf"), !Json.IsValid());
	TArray<uint8> Bytes;
	if (!TestTrue(TEXT("semantic writer retains typed leaves"), TrySerializeSemanticHjsonObject(*Object, Bytes))) return false;
	const FUTF8ToTCHAR Utf8(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
	if (!TryParseHjsonDocument(FString(Utf8.Length(), Utf8.Get()), TEXT("typed.hjson"), Resources, Document, Error)) return false;
	for (const TCHAR* Kind : {TEXT("temporal"), TEXT("spatial")})
	{
		const auto* Leaf = Document->GetRoot()->Get(Kind);
		const auto* Tag = Leaf ? Leaf->Get(TEXT("$uai")) : nullptr;
		if (!TestTrue(TEXT("semantic kind survives serialization"), Tag && Tag->GetText().Equals(Kind, ESearchCase::CaseSensitive))) return false;
	}
	const auto* DecimalLexeme = Document->GetRoot()->Get(TEXT("decimal"));
	const auto* Tick = Document->GetRoot()->Get(TEXT("temporal"))->Get(TEXT("tick"));
	const auto* Components = Document->GetRoot()->Get(TEXT("spatial"))->Get(TEXT("components"));
	TestTrue(TEXT("integral decimal retains its value and numeric kind"), DecimalLexeme
		&& DecimalLexeme->GetKind() == EHjsonNodeKind::Decimal
		&& AreNumericLexemesEquivalent(DecimalLexeme->GetLexeme(), TEXT("7")));
	TestTrue(TEXT("temporal exact tick survives"), Tick && Tick->GetText().Equals(TEXT("24000"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("spatial components survive"), Components && Components->GetItems().Num() == 3
		&& Components->GetItems()[0]->GetText().Equals(TEXT("320"), ESearchCase::CaseSensitive)
		&& Components->GetItems()[1]->GetText().Equals(TEXT("-96"), ESearchCase::CaseSensitive));
	FUeAgentCanonicalValuePtr EmptyRecords, InvalidRecord, InvalidEntries, Resolved;
	if (!FUeAgentCanonicalValue::TryArray({}, Resources, EmptyRecords, StateError)
		|| !FUeAgentCanonicalValue::TryObject({{TEXT("source"), UpperValue.ToSharedRef()}, {TEXT("Source"), LowerValue.ToSharedRef()}}, Resources, InvalidRecord, StateError)
		|| !FUeAgentCanonicalValue::TryObject({{TEXT("Key"), InvalidRecord.ToSharedRef()}}, Resources, InvalidEntries, StateError)) return false;
	FKeyedItemProjectionContract EntryContract;
	if (!TryGetKeyedItemProjectionContract(TEXT("entries"), EntryContract)) return false;
	bool bChanged = false;
	TestFalse(TEXT("record fields are checked before a case-insensitive map can fold them"), TryResolveKeyedItemDesiredState(
		*InvalidEntries, *EmptyRecords, TEXT("entries.hjson"), EntryContract, nullptr, Resources, Resolved, bChanged, Error));
	TestEqual(TEXT("undeclared record spelling is rejected"), Error.GetCode(), FString(TEXT("keyed_record_field_unknown")));
	TestTrue(TEXT("rejected record has no desired-state output"), !Resolved.IsValid());
	return true;
}
