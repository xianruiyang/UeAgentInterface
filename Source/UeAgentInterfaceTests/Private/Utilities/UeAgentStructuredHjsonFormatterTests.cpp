#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStructuredHjsonFormatterInlineStringContextTest,
	"UeAgentInterface.Utilities.StructuredHjsonFormatter.InlineStringsAreQuoted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStructuredHjsonFormatterInlineStringContextTest::RunTest(
	const FString& Parameters)
{
	const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
	Row->SetStringField(TEXT("type"), TEXT("integer"));
	Row->SetStringField(TEXT("value"), TEXT("ready"));
	Row->SetStringField(TEXT("escaped"), TEXT("a\"b"));

	TArray<TSharedPtr<FJsonValue>> Labels;
	Labels.Add(MakeShared<FJsonValueString>(TEXT("alpha")));
	Labels.Add(MakeShared<FJsonValueString>(TEXT("beta")));

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("labels"), MoveTemp(Labels));
	Root->SetStringField(TEXT("name"), TEXT("alpha"));
	Root->SetObjectField(TEXT("row"), Row);

	const FString Expected =
		TEXT("labels: [\"alpha\", \"beta\"]\n")
		TEXT("name: alpha\n")
		TEXT("row: { escaped: \"a\\\"b\", type: \"integer\", value: \"ready\" }");
	const FString Text = FUeAgentStructuredHjsonFormatter::WriteObject(Root);
	TestEqual(
		TEXT("inline strings are quoted while line-terminated strings remain bare"),
		Text,
		Expected);

	TSharedPtr<FJsonObject> Parsed;
	FString Error;
	TestTrue(
		TEXT("inline string output parses through the authoritative parser"),
		FUeAgentStructuredHjsonParser::ParseObject(
			Text,
			TEXT("inline-string-context.hjson"),
			Parsed,
			Error));
	if (Parsed.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* ParsedLabels = nullptr;
		const TSharedPtr<FJsonObject>* ParsedRow = nullptr;
		TestTrue(
			TEXT("inline array survives parsing"),
			Parsed->TryGetArrayField(TEXT("labels"), ParsedLabels));
		TestTrue(
			TEXT("inline object survives parsing"),
			Parsed->TryGetObjectField(TEXT("row"), ParsedRow));
		TestEqual(
			TEXT("inline array retains both strings"),
			ParsedLabels ? ParsedLabels->Num() : 0,
			2);
		if (ParsedLabels && ParsedLabels->Num() == 2)
		{
			TestEqual(
				TEXT("first inline array string"),
				(*ParsedLabels)[0]->AsString(),
				FString(TEXT("alpha")));
			TestEqual(
				TEXT("second inline array string"),
				(*ParsedLabels)[1]->AsString(),
				FString(TEXT("beta")));
		}
		if (ParsedRow && ParsedRow->IsValid())
		{
			TestEqual(
				TEXT("nested type string"),
				(*ParsedRow)->GetStringField(TEXT("type")),
				FString(TEXT("integer")));
			TestEqual(
				TEXT("nested value string"),
				(*ParsedRow)->GetStringField(TEXT("value")),
				FString(TEXT("ready")));
			TestEqual(
				TEXT("nested escaped string"),
				(*ParsedRow)->GetStringField(TEXT("escaped")),
				FString(TEXT("a\"b")));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStructuredHjsonFormatterShortObjectTest,
	"UeAgentInterface.Utilities.StructuredHjsonFormatter.ShortObjectRemainsInline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStructuredHjsonFormatterShortObjectTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
	Position->SetNumberField(TEXT("x"), 1.0);
	Position->SetNumberField(TEXT("y"), 2.0);

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetObjectField(TEXT("position"), Position);

	const FString Text = FUeAgentStructuredHjsonFormatter::WriteObject(Root);
	TestEqual(
		TEXT("short tuple remains a local one-line edit"),
		Text,
		FString(TEXT("position: { x: 1, y: 2 }")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStructuredHjsonFormatterInlineBudgetTest,
	"UeAgentInterface.Utilities.StructuredHjsonFormatter.LongAndDeepContainersWrap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStructuredHjsonFormatterInlineBudgetTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FJsonObject> LongObject = MakeShared<FJsonObject>();
	LongObject->SetStringField(
		TEXT("a_deliberately_long_authoring_property_name_that_must_count_toward_the_shared_line_budget"),
		TEXT("a_deliberately_long_authoring_value_that_prevents_a_hundreds_of_characters_single_line"));

	const TSharedRef<FJsonObject> Leaf = MakeShared<FJsonObject>();
	Leaf->SetNumberField(TEXT("value"), 1.0);
	const TSharedRef<FJsonObject> Middle = MakeShared<FJsonObject>();
	Middle->SetObjectField(TEXT("leaf"), Leaf);
	const TSharedRef<FJsonObject> Deep = MakeShared<FJsonObject>();
	Deep->SetObjectField(TEXT("middle"), Middle);

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetObjectField(TEXT("long"), LongObject);
	Root->SetObjectField(TEXT("deep"), Deep);

	const FString Text = FUeAgentStructuredHjsonFormatter::WriteObject(Root);
	TestTrue(TEXT("long object uses a multiline body"), Text.Contains(TEXT("long: {\n")));
	TestTrue(TEXT("deep object does not collapse its full chain onto one line"), Text.Contains(TEXT("deep: {\n")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStructuredHjsonFormatterBudgetRoundTripTest,
	"UeAgentInterface.Utilities.StructuredHjsonFormatter.InlineBudgetRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStructuredHjsonFormatterBudgetRoundTripTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FJsonObject> Nested = MakeShared<FJsonObject>();
	Nested->SetStringField(TEXT("id"), TEXT("candidate:example"));
	Nested->SetStringField(
		TEXT("description"),
		FString::ChrN(140, TEXT('x')));

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetObjectField(TEXT("node"), Nested);

	const FString Text = FUeAgentStructuredHjsonFormatter::WriteObject(Root);
	TSharedPtr<FJsonObject> Parsed;
	FString Error;
	TestTrue(
		TEXT("writer output parses through the authoritative parser"),
		FUeAgentStructuredHjsonParser::ParseObject(
			Text,
			TEXT("inline-budget-roundtrip.hjson"),
			Parsed,
			Error));
	if (!Parsed.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* ParsedNode = nullptr;
	TestTrue(TEXT("roundtrip retains nested object"), Parsed->TryGetObjectField(TEXT("node"), ParsedNode));
	if (!ParsedNode || !ParsedNode->IsValid())
	{
		return false;
	}
	TestEqual(
		TEXT("roundtrip retains id"),
		(*ParsedNode)->GetStringField(TEXT("id")),
		FString(TEXT("candidate:example")));
	TestEqual(TEXT("roundtrip retains long value"), (*ParsedNode)->GetStringField(TEXT("description")), FString::ChrN(140, TEXT('x')));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStructuredHjsonParserCoreDialectTest,
	"UeAgentInterface.Utilities.StructuredHjsonFormatter.CoreParserDialectAndBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStructuredHjsonParserCoreDialectTest::RunTest(const FString& Parameters)
{
	const FString Text =
		TEXT("exact: 9007199254740993\n")
		TEXT("nested: { values: [1, { label: \"ready\" }] }\n")
		TEXT("description: '''\n")
		TEXT("  first line\n")
		TEXT("  second line\n")
		TEXT("  '''\n");
	FUeAgentStructuredHjsonDocument Document;
	FString Error;
	const bool bParsed = FUeAgentStructuredHjsonParser::ParseDocument(
		Text, TEXT("core-parser-dialect.hjson"), Document, Error);
	TestTrue(TEXT("Core parser accepts the authoring dialect through the public adapter"),
		bParsed);
	if (!bParsed)
	{
		AddError(FString(TEXT("core_parser_error:")) + Error);
		return false;
	}
	if (!Document.Root.IsValid()) return false;
	const TSharedPtr<FJsonValue> Exact = Document.Root->TryGetField(TEXT("exact"));
	TestTrue(TEXT("exact integer remains a number string"),
		Exact.IsValid() && Exact->Type == EJson::Number
			&& Exact->PreferStringRepresentation());
	FString ExactLexeme;
	TestTrue(TEXT("exact integer lexeme is independent and unchanged"),
		Exact.IsValid() && Exact->TryGetString(ExactLexeme));
	TestEqual(TEXT("exact integer value"), ExactLexeme,
		FString(TEXT("9007199254740993")));
	TestEqual(TEXT("multiline indentation is removed"),
		Document.Root->GetStringField(TEXT("description")),
		FString(TEXT("first line\nsecond line")));
	const FUeAgentHjsonSourceLocation* NestedLocation =
		Document.FindSourceLocation(TEXT("/nested/values/1/label"));
	TestNotNull(TEXT("nested source location is projected from the shared AST"),
		NestedLocation);
	if (NestedLocation)
	{
		TestEqual(TEXT("nested source file"), NestedLocation->FilePath,
			FString(TEXT("core-parser-dialect.hjson")));
		TestEqual(TEXT("nested source line"), NestedLocation->Line, 2);
		TestEqual(TEXT("nested source column"), NestedLocation->Column, 32);
	}

	FUeAgentStructuredHjsonDocument Rejected;
	TestFalse(TEXT("ordinary single quotes stay outside the authoring dialect"),
		FUeAgentStructuredHjsonParser::ParseDocument(
			TEXT("value: 'legacy'\n"), TEXT("single-quote.hjson"),
			Rejected, Error));
	TestTrue(TEXT("dialect rejection carries source position"),
		Error.Contains(TEXT("code=hjson_unsupported_feature"))
			&& Error.Contains(TEXT("line=1"))
			&& Error.Contains(TEXT("column=8")));
	TestFalse(TEXT("failed parse does not publish a root"), Rejected.Root.IsValid());
	TestEqual(TEXT("failed parse does not publish source locations"),
		Rejected.SourceLocations.Num(), 0);

	FUeAgentStructuredResourceTracker Precharged;
	Error.Reset();
	TestTrue(TEXT("caller can precharge input"),
		Precharged.ChargeInputBytes(9, TEXT("/"), Error));
	TestTrue(TEXT("precharged parse does not charge input twice"),
		FUeAgentStructuredHjsonParser::ParseDocument(
			TEXT("value: 1"), TEXT("precharged.hjson"), Precharged, true,
			Document, Error));
	TestEqual(TEXT("input usage remains caller-owned"),
		Precharged.GetUsage().InputBytes, 9ll);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStructuredHjsonScannerBoundaryTest,
	"UeAgentInterface.Utilities.StructuredHjsonFormatter.ScannerBoundaries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStructuredHjsonScannerBoundaryTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::HjsonWorkspace;
	FUeAgentStructuredHjsonDocument Document;
	FString Error;
	auto Reject = [&](const FString& Text, const TCHAR* Code, int32 Line, int32 Column)
	{
		TestFalse(TEXT("invalid source is rejected"), FUeAgentStructuredHjsonParser::ParseDocument(
			Text, TEXT("scanner.hjson"), Document, Error));
		TestTrue(TEXT("stable syntax code"), Error.Contains(FString(TEXT("code=")) + Code));
		TestTrue(TEXT("source identity survives rejection"), Error.Contains(TEXT("scanner.hjson")));
		TestTrue(TEXT("precise source line"), Error.Contains(FString::Printf(TEXT("line=%d "), Line)));
		TestTrue(TEXT("precise source column"), Error.Contains(FString::Printf(TEXT("column=%d"), Column)));
		TestFalse(TEXT("no partial root"), Document.Root.IsValid());
		TestEqual(TEXT("no partial locations"), Document.SourceLocations.Num(), 0);
	};
	for (const FString& Newline : {FString(TEXT("\n")), FString(TEXT("\r\n")), FString(TEXT("\r"))})
	{
		Reject(TEXT("value: '''") + Newline + TEXT("abc") + Newline + TEXT("  '''"),
			TEXT("hjson_multiline_indentation"), 2, 1);
		const FString Valid = TEXT("value: '''") + Newline + TEXT("  abc") + Newline
			+ TEXT(" ") + Newline + TEXT("   xyz") + Newline + TEXT("  '''") + Newline;
		const bool bParsed = FUeAgentStructuredHjsonParser::ParseDocument(Valid, TEXT("scanner.hjson"), Document, Error);
		TestTrue(TEXT("valid closing indentation and line endings"), bParsed);
		if (bParsed) TestEqual(TEXT("dedent preserves body and blank lines"),
			Document.Root->GetStringField(TEXT("value")), FString(TEXT("abc\n\n xyz")));
	}
	Reject(TEXT("value: \"a\nb\""), TEXT("hjson_unescaped_control"), 1, 10);
	Reject(TEXT("value: \"a\tb\""), TEXT("hjson_unescaped_control"), 1, 10);
	Reject(TEXT("value: \"\\q\""), TEXT("hjson_escape_invalid"), 1, 10);
	Reject(TEXT("value: \"\\'\""), TEXT("hjson_escape_invalid"), 1, 10);
	Reject(TEXT("value: \"\\u12x4\""), TEXT("hjson_unicode_escape_invalid"), 1, 13);
	Reject(TEXT("value: \"\\u12"), TEXT("hjson_unicode_escape_invalid"), 1, 13);
	Reject(TEXT("value: \"abc"), TEXT("hjson_string_unclosed"), 1, 12);
	Reject(TEXT("value: NaN"), TEXT("hjson_nonfinite_number"), 1, 8);
	Reject(TEXT("value: Infinity"), TEXT("hjson_nonfinite_number"), 1, 8);
	Reject(TEXT("value: '''\n  ok\n  ''' extra"), TEXT("hjson_multiline_trailing_token"), 3, 7);
	TestTrue(TEXT("JSON quoted escapes remain supported"), FUeAgentStructuredHjsonParser::ParseDocument(
		TEXT("value: \"\\u0041\\n\\t\\\\\\\"\\/\""), TEXT("scanner.hjson"), Document, Error));
	if (Document.Root.IsValid()) TestEqual(TEXT("decoded escapes"),
		Document.Root->GetStringField(TEXT("value")), FString(TEXT("A\n\t\\\"/")));

	const FString LongString = TEXT("value: \"") + FString::ChrN(8192, TEXT('a')) + TEXT("\"");
	for (const FString& StopStage : {FString(TEXT("hjson_utf8_measure")), FString(TEXT("hjson_utf8_offsets")),
		FString(TEXT("hjson_multiline_content")), FString(TEXT("hjson_string_decode"))})
	{
		FHjsonDocumentPtr CoreDocument;
		FWorkspaceError CoreError;
		FUeAgentResourceTracker Resources;
		int32 Visits = 0;
		const FHjsonOperationCheckpoint Stop = [&](const FString& Stage, FWorkspaceError& Failure)
		{
			if (Stage == StopStage && ++Visits == 2)
			{
				Failure.Set(TEXT("test_deadline"), TEXT("scanner.hjson"), Stage, 0, true);
				return false;
			}
			return true;
		};
		const FString Input = StopStage == TEXT("hjson_multiline_content")
			? TEXT("value: '''\n  ") + FString::ChrN(8192, TEXT('a')) + TEXT("\n  '''") : LongString;
		TestFalse(TEXT("long intermediate scan is interruptible"), TryParseHjsonDocument(
			Input, TEXT("scanner.hjson"), Resources, CoreDocument, CoreError, &Stop));
		TestEqual(TEXT("checkpoint stop propagated"), CoreError.GetCode(), FString(TEXT("test_deadline")));
		TestEqual(TEXT("stopped at requested scan checkpoint"), Visits, 2);
		TestFalse(TEXT("checkpoint failure publishes no AST"), CoreDocument.IsValid());
	}

	FUeAgentStructuredResourceTracker Cumulative;
	TestTrue(TEXT("first parse with shared caller tracker"), FUeAgentStructuredHjsonParser::ParseDocument(
		TEXT("value: 1"), TEXT("one.hjson"), Cumulative, false, Document, Error));
	const auto FirstUsage = Cumulative.GetUsage();
	TestTrue(TEXT("second parse with shared caller tracker"), FUeAgentStructuredHjsonParser::ParseDocument(
		TEXT("value: 1"), TEXT("two.hjson"), Cumulative, false, Document, Error));
	TestEqual(TEXT("caller input accumulates"), Cumulative.GetUsage().InputBytes, FirstUsage.InputBytes * 2);
	TestEqual(TEXT("scanner and projection memory accumulates"),
		Cumulative.GetUsage().EstimatedMemoryBytes, FirstUsage.EstimatedMemoryBytes * 2);

	return true;
}
