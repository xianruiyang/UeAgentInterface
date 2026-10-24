#include "Misc/AutomationTest.h"

#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"


#include "Core/ContextResource/UeAgentContextResource.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::HjsonWorkspace;

	static FString FixturePath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectDir(),
			TEXT("Plugins/UeAgentInterface/Source/UeAgentInterfaceTests/")
			TEXT("Private/Core/HjsonWorkspace/Fixtures/")
			TEXT("hjson_workspace_cases.json")));
	}

	static FString StandardSha256(const TArray<uint8>& Bytes)
	{
		uint8 Digest[SHA256_DIGEST_LENGTH] = {};
		const uint8 Empty = 0;
		SHA256(
			Bytes.IsEmpty() ? &Empty : Bytes.GetData(),
			static_cast<size_t>(Bytes.Num()),
			Digest);
		return TEXT("sha256:")
			+ BytesToHex(Digest, SHA256_DIGEST_LENGTH).ToLower();
	}

	static FString Utf8Text(const TArray<uint8>& Bytes)
	{
		const FUTF8ToTCHAR Converted(
			reinterpret_cast<const ANSICHAR*>(Bytes.GetData()),
			Bytes.Num());
		return FString(Converted.Length(), Converted.Get());
	}

	static TSharedPtr<FJsonObject> LoadFixture(FAutomationTestBase* Test)
	{
		FString Text;
		if (!Test->TestTrue(
			TEXT("load shared C5 fixture"),
			FFileHelper::LoadFileToString(Text, *FixturePath())))
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> Fixture;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Text);
		if (!Test->TestTrue(
			TEXT("parse shared C5 fixture"),
			FJsonSerializer::Deserialize(Reader, Fixture)
				&& Fixture.IsValid()))
		{
			return nullptr;
		}
		Test->TestEqual(
			TEXT("fixture schema"),
			Fixture->GetStringField(TEXT("schema")),
			FString(TEXT("uai.c5.hjson_workspace.test_fixture.v1")));
		return Fixture;
	}

	static FUeAgentResourceTracker MakeTracker(
		const TSharedPtr<FJsonObject>&)
	{
		return FUeAgentResourceTracker();
	}

	static FWorkspacePolicy MakePolicy(
		const TSharedPtr<FJsonObject>& Fixture)
	{
		const TSharedPtr<FJsonObject> Row =
			Fixture->GetObjectField(TEXT("workspace_policy"));
		TArray<FString> ReferenceFields;
		for (const TSharedPtr<FJsonValue>& Value
			: Row->GetArrayField(TEXT("reference_fields")))
		{
			ReferenceFields.Add(Value->AsString());
		}
		return FWorkspacePolicy(
			Row->GetStringField(TEXT("dialect_id")),
			Row->GetStringField(TEXT("contract_id")),
			static_cast<int32>(
				Row->GetNumberField(TEXT("contract_version"))),
			Row->GetStringField(TEXT("profile_id")),
			Row->GetStringField(TEXT("entry_unit")),
			MoveTemp(ReferenceFields));
	}

	static bool WriteOwnedFile(
		const FString& Root,
		const FString& RelativePath,
		const FString& Text)
	{
		const FString Path = FPaths::Combine(Root, RelativePath);
		IFileManager::Get().MakeDirectory(
			*FPaths::GetPath(Path),
			true);
		return FFileHelper::SaveStringToFile(
			Text,
			*Path,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}


}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentHjsonWorkspaceCodecTest,
	"UeAgentInterface.Core.HjsonWorkspace.CodecProvenanceAndDepth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentHjsonWorkspaceCodecTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Source =
		Fixture->GetObjectField(TEXT("source_document"));
	{
		// Python hjson emits unquoted multiword values on ordinary workspace edits.
		const FString Unquoted = TEXT("{\nname: Create Points Grid \t\ncount: 9007199254740993 \t\n\"selection key\": Missing settings\nvalues:\n[\n160\n96\n]\n}\n");
		FUeAgentResourceTracker Tracker = MakeTracker(Fixture);
		FHjsonDocumentPtr Parsed;
		FWorkspaceError ParseError;
		if (!TestTrue(TEXT("standard HJSON multiword values and quoted keys parse"),
			TryParseHjsonDocument(Unquoted, TEXT("unquoted.hjson"), Tracker, Parsed, ParseError)))
		{
			AddError(ParseError.GetCode());
			return false;
		}
		const FHjsonNode* Name = Parsed->Find(TEXT("/name"));
		const FHjsonNode* Count = Parsed->Find(TEXT("/count"));
		const FHjsonNode* Selection = Parsed->Find(TEXT("/selection key"));
		TestTrue(TEXT("internal spaces are retained and trailing whitespace is excluded"),
			Name && Name->GetText() == TEXT("Create Points Grid")
				&& Name->GetLexeme() == TEXT("Create Points Grid")
				&& Name->GetProvenance().GetByteEnd() - Name->GetProvenance().GetByteStart() == 18);
		TestTrue(TEXT("trailing whitespace does not turn an exact integer into text"),
			Count && Count->GetKind() == EHjsonNodeKind::Integer
				&& Count->GetLexeme() == TEXT("9007199254740993"));
		TestTrue(TEXT("a multiword key resolves its complete value"),
			Selection && Selection->GetText() == TEXT("Missing settings"));
		const FHjsonNode* Last = Parsed->Find(TEXT("/values/1"));
		TestTrue(TEXT("newline still separates array values"), Last && Last->GetLexeme() == TEXT("96"));
		FUeAgentResourceTracker InvalidTracker = MakeTracker(Fixture);
		FHjsonDocumentPtr Invalid;
		TestFalse(TEXT("an unquoted multiword key remains invalid"), TryParseHjsonDocument(
			TEXT("{\nselection key: value\n}\n"), TEXT("invalid-key.hjson"), InvalidTracker, Invalid, ParseError));
	}
	FUeAgentResourceTracker ParseTracker = MakeTracker(Fixture);
	{
		const FString References = TEXT("{\nrefs:\n[\nstructure:state_tree2\nasset:/Game/Test\n]\ntime: 12:34\ntype: CustomDefine[]\ncode: return float4(1, 2, 3, 4); // literal # text\nurl: https://example.test/a#part\nquotes: text with \"quotes\" and {braces} [brackets]\nscalars: [1, -0.5, true, false, null]\ncommented: 42 # actual comment\n}\n");
		FUeAgentResourceTracker Tracker = MakeTracker(Fixture);
		FHjsonDocumentPtr Parsed;
		FWorkspaceError ParseError;
		if (!TestTrue(TEXT("unquoted colon values retain their scalar meaning"),
			TryParseHjsonDocument(References, TEXT("colon-values.hjson"), Tracker, Parsed, ParseError)))
		{ AddError(ParseError.GetCode()); return false; }
		const FHjsonNode* Ref = Parsed->Find(TEXT("/refs/0"));
		const FHjsonNode* Asset = Parsed->Find(TEXT("/refs/1"));
		const FHjsonNode* Time = Parsed->Find(TEXT("/time"));
		TestTrue(TEXT("reference text and complete source span are preserved"), Ref
			&& Ref->GetText() == TEXT("structure:state_tree2")
			&& Ref->GetLexeme() == TEXT("structure:state_tree2")
			&& Ref->GetProvenance().GetByteEnd() - Ref->GetProvenance().GetByteStart() == 21);
		TestTrue(TEXT("path and numeric-looking colon values remain strings"), Asset && Time
			&& Asset->GetText() == TEXT("asset:/Game/Test")
			&& Time->GetKind() == EHjsonNodeKind::String && Time->GetText() == TEXT("12:34"));
		const FHjsonNode* Type = Parsed->Find(TEXT("/type"));
		const FHjsonNode* Code = Parsed->Find(TEXT("/code"));
		const FHjsonNode* Url = Parsed->Find(TEXT("/url"));
		const FHjsonNode* Quotes = Parsed->Find(TEXT("/quotes"));
		TestTrue(TEXT("punctuation and comment text inside quoteless values are literal"),
			Type && Type->GetText() == TEXT("CustomDefine[]")
			&& Code && Code->GetText() == TEXT("return float4(1, 2, 3, 4); // literal # text")
			&& Url && Url->GetText() == TEXT("https://example.test/a#part")
			&& Quotes && Quotes->GetText() == TEXT("text with \"quotes\" and {braces} [brackets]"));
		const FHjsonNode* Scalar = Parsed->Find(TEXT("/scalars/4"));
		const FHjsonNode* Commented = Parsed->Find(TEXT("/commented"));
		TestTrue(TEXT("JSON scalar boundaries and their comments remain structural"),
			Scalar && Scalar->GetKind() == EHjsonNodeKind::Null
			&& Commented && Commented->GetKind() == EHjsonNodeKind::Integer
			&& Commented->GetLexeme() == TEXT("42"));
	}
	FHjsonDocumentPtr Document;
	FWorkspaceError Error;
	const bool bParsed = TestTrue(
		TEXT("parse independent HJSON source"),
		TryParseHjsonDocument(
			Source->GetStringField(TEXT("text")),
			TEXT("index.hjson"),
			ParseTracker,
			Document,
			Error));
	if (!bParsed || !Document.IsValid())
	{
		return false;
	}
	const FHjsonNode* Precise = Document.IsValid()
		? Document->Find(TEXT("/precise"))
		: nullptr;
	TestTrue(TEXT("precise AST node exists"), Precise != nullptr);
	if (Precise)
	{
		TestEqual(TEXT("exact integer kind"), Precise->GetKind(), EHjsonNodeKind::Integer);
		TestEqual(
			TEXT("exact integer lexeme"),
			Precise->GetLexeme(),
			Source->GetStringField(TEXT("precise_lexeme")));
		const TSharedPtr<FJsonObject> Expected =
			Source->GetObjectField(TEXT("precise_provenance"));
		TestEqual(
			TEXT("source pointer"),
			Precise->GetProvenance().GetJsonPointer(),
			Expected->GetStringField(TEXT("json_pointer")));
		TestEqual(
			TEXT("source byte start"),
			Precise->GetProvenance().GetByteStart(),
			static_cast<int64>(Expected->GetNumberField(TEXT("byte_start"))));
	}

	FUeAgentResourceTracker WriteTracker = MakeTracker(Fixture);
	TArray<uint8> CanonicalUtf8;
	TestTrue(
		TEXT("write canonical UTF-8"),
		TryWriteHjsonDocument(
			Document.ToSharedRef(),
			WriteTracker,
			CanonicalUtf8,
			Error));
	const FString CanonicalText = Utf8Text(CanonicalUtf8);
	FUeAgentResourceTracker RoundTripParseTracker = MakeTracker(Fixture);
	FHjsonDocumentPtr RoundTripDocument;
	TestTrue(
		TEXT("canonical UTF-8 parses again"),
		TryParseHjsonDocument(
			CanonicalText,
			TEXT("canonical.hjson"),
			RoundTripParseTracker,
			RoundTripDocument,
			Error));
	TArray<uint8> RoundTripUtf8;
	if (RoundTripDocument.IsValid())
	{
		const TSharedPtr<FJsonObject> Policy =
			Fixture->GetObjectField(TEXT("workspace_policy"));
		const FHjsonNode& RoundTripRoot =
			RoundTripDocument->GetRoot().Get();
		TestEqual(
			TEXT("round-trip preserves the complete field set"),
			RoundTripRoot.GetEntries().Num(),
			4);
		const FHjsonNode* ContractId =
			RoundTripDocument->Find(TEXT("/contract_id"));
		const FHjsonNode* ContractVersion =
			RoundTripDocument->Find(TEXT("/contract_version"));
		const FHjsonNode* ProfileId =
			RoundTripDocument->Find(TEXT("/profile_id"));
		const FHjsonNode* RoundTripPrecise =
			RoundTripDocument->Find(TEXT("/precise"));
		TestTrue(TEXT("round-trip contract_id exists"), ContractId != nullptr);
		TestTrue(
			TEXT("round-trip contract_version exists"),
			ContractVersion != nullptr);
		TestTrue(TEXT("round-trip profile_id exists"), ProfileId != nullptr);
		TestTrue(
			TEXT("round-trip precise AST node exists"),
			RoundTripPrecise != nullptr);
		if (ContractId)
		{
			TestEqual(
				TEXT("round-trip contract_id type"),
				ContractId->GetKind(),
				EHjsonNodeKind::String);
			TestEqual(
				TEXT("round-trip contract_id value"),
				ContractId->GetText(),
				Policy->GetStringField(TEXT("contract_id")));
		}
		if (ContractVersion)
		{
			TestEqual(
				TEXT("round-trip contract_version type"),
				ContractVersion->GetKind(),
				EHjsonNodeKind::Integer);
			TestEqual(
				TEXT("round-trip contract_version value"),
				ContractVersion->GetLexeme(),
				FString::FromInt(static_cast<int32>(
					Policy->GetNumberField(TEXT("contract_version")))));
		}
		if (ProfileId)
		{
			TestEqual(
				TEXT("round-trip profile_id type"),
				ProfileId->GetKind(),
				EHjsonNodeKind::String);
			TestEqual(
				TEXT("round-trip profile_id value"),
				ProfileId->GetText(),
				Policy->GetStringField(TEXT("profile_id")));
		}
		if (RoundTripPrecise)
		{
			TestEqual(
				TEXT("round-trip precise type"),
				RoundTripPrecise->GetKind(),
				EHjsonNodeKind::Integer);
			TestEqual(
				TEXT("round-trip preserves exact integer lexeme"),
				RoundTripPrecise->GetLexeme(),
				Source->GetStringField(TEXT("precise_lexeme")));
		}
		FUeAgentResourceTracker RoundTripWriteTracker = MakeTracker(Fixture);
		TestTrue(
			TEXT("round-trip canonical UTF-8 writes"),
			TryWriteHjsonDocument(
				RoundTripDocument.ToSharedRef(),
				RoundTripWriteTracker,
				RoundTripUtf8,
				Error));
		TestEqual(
			TEXT("canonical UTF-8 is deterministic after round-trip"),
			RoundTripUtf8,
			CanonicalUtf8);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentHjsonWorkspaceSourceParityTest,
	"UeAgentInterface.Core.HjsonWorkspace.SourceParityPathAndReferenceGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentHjsonWorkspaceSourceParityTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	const FString Root = FPaths::Combine(
		FPaths::ProjectIntermediateDir(),
		TEXT("UAI/P3_001_C5_SourceParity"));
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	IFileManager::Get().MakeDirectory(*Root, true);

	const FString Text =
		Fixture->GetObjectField(TEXT("source_document"))
			->GetStringField(TEXT("text"));
	const FString DirectoryRoot = FPaths::Combine(Root, TEXT("directory"));
	const FString SingleRoot = FPaths::Combine(Root, TEXT("single"));
	TestTrue(TEXT("write directory source"), WriteOwnedFile(DirectoryRoot, TEXT("index.hjson"), Text));
	TestTrue(TEXT("write single source"), WriteOwnedFile(SingleRoot, TEXT("index.hjson"), Text));

	const FWorkspacePolicy Policy = MakePolicy(Fixture);
	const TArray<FWorkspaceSource> Sources = {
		FWorkspaceSource::Directory(DirectoryRoot),
		FWorkspaceSource::SingleFile(FPaths::Combine(SingleRoot, TEXT("index.hjson"))),
		FWorkspaceSource::Inline(Text, TEXT("index.hjson"), TEXT("fixture-inline")),
	};
	TArray<FCanonicalWorkspacePtr> Workspaces;
	int32 SourceIndex = 0;
	for (const FWorkspaceSource& Source : Sources)
	{
		FUeAgentResourceTracker Tracker = MakeTracker(Fixture);
		FCanonicalWorkspacePtr Workspace;
		FWorkspaceError Error;
		const bool bLoaded = TryLoadWorkspace(
			Source,
			Policy,
			Tracker,
			Workspace,
			Error);
		TestTrue(
			TEXT("load source through common Workspace"),
			bLoaded);
		if (!bLoaded)
		{
			AddError(FString::Printf(
				TEXT(
					"workspace source adapter %d failed: "
					"code=%s source=%s pointer=%s observed=%lld"),
				SourceIndex,
				*Error.GetCode(),
				*Error.GetSourceUnit(),
				*Error.GetJsonPointer(),
				Error.GetObserved()));
		}
		Workspaces.Add(Workspace);
		++SourceIndex;
	}
	TestEqual(
		TEXT("three source adapters loaded"),
		Workspaces.Num(),
		3);
	if (Workspaces.Num() == 3
		&& Workspaces[0].IsValid()
		&& Workspaces[1].IsValid()
		&& Workspaces[2].IsValid())
	{
		TestEqual(
			TEXT("directory and single semantic fingerprint"),
			Workspaces[0]->GetSemanticFingerprint(),
			Workspaces[1]->GetSemanticFingerprint());
		TestEqual(
			TEXT("directory and inline semantic fingerprint"),
			Workspaces[0]->GetSemanticFingerprint(),
			Workspaces[2]->GetSemanticFingerprint());
		for (const FCanonicalWorkspacePtr& Workspace : Workspaces)
		{
			TestEqual(
				TEXT("canonical bytes use standard SHA-256"),
				Workspace->GetUnits()[0].GetContentHash(),
				StandardSha256(Workspace->GetUnits()[0].GetCanonicalUtf8()));
		}
	}

	IFileManager::Get().DeleteDirectory(*Root, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentHjsonWorkspaceReferenceBudgetTest,
	"UeAgentInterface.Core.HjsonWorkspace.ReferenceDagCycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentHjsonWorkspaceReferenceBudgetTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject> References =
		Fixture->GetObjectField(TEXT("references"));
	const FString Root = FPaths::Combine(
		FPaths::ProjectIntermediateDir(),
		TEXT("UAI/P3_001_C5_ReferenceBudget"));
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	IFileManager::Get().MakeDirectory(*Root, true);

	const FString SharedRoot = FPaths::Combine(Root, TEXT("shared"));
	WriteOwnedFile(
		SharedRoot,
		TEXT("index.hjson"),
		References->GetStringField(TEXT("shared_entry_text")));
	WriteOwnedFile(
		SharedRoot,
		TEXT("units/child.hjson"),
		References->GetStringField(TEXT("child_text")));
	FUeAgentResourceTracker SharedTracker = MakeTracker(Fixture);
	FCanonicalWorkspacePtr SharedWorkspace;
	FWorkspaceError Error;
	const bool bSharedLoaded = TryLoadWorkspace(
		FWorkspaceSource::Directory(SharedRoot),
		MakePolicy(Fixture),
		SharedTracker,
		SharedWorkspace,
		Error);
	TestTrue(
		TEXT("shared target is a valid DAG"),
		bSharedLoaded);
	if (!bSharedLoaded)
	{
		AddError(FString::Printf(
			TEXT(
				"shared DAG load failed: code=%s source=%s "
				"pointer=%s observed=%lld"),
			*Error.GetCode(),
			*Error.GetSourceUnit(),
			*Error.GetJsonPointer(),
			Error.GetObserved()));
	}
	else
	{
		TestEqual(
			TEXT("two edges may share one target"),
			SharedWorkspace->GetReferenceEdges().Num(),
			2);
	}

	const FString CycleRoot = FPaths::Combine(Root, TEXT("cycle"));
	WriteOwnedFile(
		CycleRoot,
		TEXT("index.hjson"),
		References->GetStringField(TEXT("cycle_a_text")));
	WriteOwnedFile(
		CycleRoot,
		TEXT("b.hjson"),
		References->GetStringField(TEXT("cycle_b_text")));
	FUeAgentResourceTracker CycleTracker = MakeTracker(Fixture);
	FCanonicalWorkspacePtr CycleWorkspace;
	TestFalse(
		TEXT("reference cycle is rejected"),
		TryLoadWorkspace(
			FWorkspaceSource::Directory(CycleRoot),
			MakePolicy(Fixture),
			CycleTracker,
			CycleWorkspace,
			Error));
	TestEqual(
		TEXT("cycle error is typed"),
		Error.GetCode(),
		FString(TEXT("workspace_ref_cycle")));

	IFileManager::Get().DeleteDirectory(*Root, false, true);
	return true;
}
