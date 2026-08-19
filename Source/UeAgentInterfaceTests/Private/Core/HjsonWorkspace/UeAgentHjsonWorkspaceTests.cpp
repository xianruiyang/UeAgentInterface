#include "Misc/AutomationTest.h"

#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"


#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::StateSemantic;

	static FString FixturePath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectDir(),
			TEXT("UeAgentInterfaceCMD/tests/hjson_workspace/fixtures/")
			TEXT("hjson_workspace_cases.json")));
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
		const TSharedPtr<FJsonObject>& Fixture,
		int64 NodeLimit = -1,
		int64 FileLimit = -1)
	{
		const TSharedPtr<FJsonObject> Budget =
			Fixture->GetObjectField(TEXT("resource_budget"));
		return FUeAgentResourceTracker(FUeAgentResourceBudget({
			{EUeAgentResourceDimension::InputBytes,
				static_cast<int64>(Budget->GetNumberField(TEXT("input_bytes")))},
			{EUeAgentResourceDimension::OutputBytes,
				static_cast<int64>(Budget->GetNumberField(TEXT("output_bytes")))},
			{EUeAgentResourceDimension::StringBytes,
				static_cast<int64>(Budget->GetNumberField(TEXT("string_bytes")))},
			{EUeAgentResourceDimension::Nodes,
				NodeLimit >= 0
					? NodeLimit
					: static_cast<int64>(Budget->GetNumberField(TEXT("nodes")))},
			{EUeAgentResourceDimension::Items,
				static_cast<int64>(Budget->GetNumberField(TEXT("items")))},
			{EUeAgentResourceDimension::Files,
				FileLimit >= 0
					? FileLimit
					: static_cast<int64>(Budget->GetNumberField(TEXT("files")))},
			{EUeAgentResourceDimension::ReferenceEdges,
				static_cast<int64>(Budget->GetNumberField(TEXT("reference_edges")))},
			{EUeAgentResourceDimension::WorkUnits,
				static_cast<int64>(Budget->GetNumberField(TEXT("work_units")))},
			{EUeAgentResourceDimension::EstimatedMemoryBytes,
				static_cast<int64>(
					Budget->GetNumberField(TEXT("estimated_memory_bytes")))},
		}));
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
			MoveTemp(ReferenceFields),
			static_cast<int32>(Row->GetNumberField(TEXT("max_files"))));
	}

	static FString BuildDeepDocument(
		int32 Depth,
		const FString& Leaf)
	{
		return TEXT("{ value: ")
			+ FString::ChrN(Depth, TEXT('['))
			+ TEXT("\"") + Leaf + TEXT("\"")
			+ FString::ChrN(Depth, TEXT(']'))
			+ TEXT(" }\n");
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

	static FUeAgentCanonicalSnapshotPtr BuildSnapshot(
		FAutomationTestBase* Test,
		const TSharedPtr<FJsonObject>& Fixture,
		FUeAgentResourceTracker& Tracker)
	{
		const TSharedPtr<FJsonObject> Row =
			Fixture->GetObjectField(TEXT("export_snapshot"));
		const TSharedPtr<FJsonObject> RevisionRow =
			Row->GetObjectField(TEXT("revision"));
		const FUeAgentStateRevision Revision(
			RevisionRow->GetStringField(TEXT("source_id")),
			RevisionRow->GetStringField(TEXT("generation")),
			static_cast<int64>(
				RevisionRow->GetNumberField(TEXT("sequence"))));
		TArray<FUeAgentStateEntry> Entries;
		for (const TSharedPtr<FJsonValue>& EntryValue
			: Row->GetArrayField(TEXT("entries")))
		{
			const TSharedPtr<FJsonObject> Entry = EntryValue->AsObject();
			const TSharedPtr<FJsonObject> Identity =
				Entry->GetObjectField(TEXT("identity"));
			const bool bInteger =
				Entry->GetStringField(TEXT("kind")) == TEXT("integer");
			FUeAgentStateSemanticError Error;
			FUeAgentCanonicalValuePtr Value;
			FUeAgentCanonicalValuePtr Default;
			const bool bValueCreated = bInteger
				? FUeAgentCanonicalValue::TryInteger(
					Entry->GetStringField(TEXT("value")),
					Tracker,
					Value,
					Error)
				: FUeAgentCanonicalValue::TryString(
					Entry->GetStringField(TEXT("value")),
					Tracker,
					Value,
					Error);
			const bool bDefaultCreated = bInteger
				? FUeAgentCanonicalValue::TryInteger(
					Entry->GetStringField(TEXT("default")),
					Tracker,
					Default,
					Error)
				: FUeAgentCanonicalValue::TryString(
					Entry->GetStringField(TEXT("default")),
					Tracker,
					Default,
					Error);
			Test->TestTrue(TEXT("create C4 export values"), bValueCreated && bDefaultCreated);
			Entries.Emplace(
				FUeAgentCanonicalIdentity(
					Identity->GetStringField(TEXT("semantic_id")),
					Identity->GetStringField(TEXT("target_kind")),
					Identity->GetStringField(TEXT("owner_id")),
					Identity->GetStringField(TEXT("selector")),
					Identity->GetStringField(TEXT("object_role")),
					Identity->GetStringField(TEXT("generation"))),
				FUeAgentCanonicalType(
					Entry->GetStringField(TEXT("type_id")),
					bInteger
						? EUeAgentValueKind::Integer
						: EUeAgentValueKind::String,
					false,
					TEXT("scalar")),
				Value.ToSharedRef(),
				FUeAgentDefaultValue(
					EUeAgentDefaultSourceKind::DomainNative,
					TEXT("fixture:c5:domain-native"),
					TEXT("fixture:c5:independent-default"),
					Default.ToSharedRef()),
				FUeAgentStateDetails(
					Entry->GetStringField(TEXT("display_name")),
					TEXT("independent C5 fixture"),
					{TEXT("Fixture"), TEXT("C5")},
					true,
					TEXT("available"),
					{}),
				Revision);
		}
		const TSharedPtr<FJsonObject> Time =
			Row->GetObjectField(TEXT("captured_at"));
		const TSharedPtr<FJsonObject> Provenance =
			Row->GetObjectField(TEXT("provenance"));
		FUeAgentCanonicalSnapshotPtr Snapshot;
		FUeAgentStateSemanticError Error;
		Test->TestTrue(
			TEXT("create real C4 snapshot"),
			FUeAgentCanonicalSnapshot::TryCreate(
				Row->GetStringField(TEXT("snapshot_id")),
				EUeAgentStatePlane::AuthoringMemory,
				Revision,
				FUeAgentTemporalPoint(
					Time->GetStringField(TEXT("clock_id")),
					Time->GetStringField(TEXT("tick")),
					Time->GetStringField(TEXT("rate_numerator")),
					Time->GetStringField(TEXT("rate_denominator")),
					Time->GetStringField(TEXT("state"))),
				FUeAgentStateProvenance(
					Provenance->GetStringField(TEXT("source_kind")),
					Provenance->GetStringField(TEXT("source_id")),
					Provenance->GetStringField(TEXT("evidence_id"))),
				Entries,
				Tracker,
				Snapshot,
				Error));
		return Snapshot;
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
	FUeAgentResourceTracker ParseTracker = MakeTracker(Fixture);
	FHjsonDocumentPtr Document;
	FWorkspaceError Error;
	TestTrue(
		TEXT("parse independent HJSON source"),
		TryParseHjsonDocument(
			Source->GetStringField(TEXT("text")),
			TEXT("index.hjson"),
			ParseTracker,
			Document,
			Error));
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
	const FTCHARToUTF8 ExpectedUtf8(
		*Source->GetStringField(TEXT("canonical_text")));
	TestEqual(
		TEXT("canonical bytes"),
		CanonicalUtf8,
		TArray<uint8>(
			reinterpret_cast<const uint8*>(ExpectedUtf8.Get()),
			ExpectedUtf8.Length()));

	const TSharedPtr<FJsonObject> Depth =
		Fixture->GetObjectField(TEXT("depth"));
	FUeAgentResourceTracker DeepTracker = MakeTracker(Fixture);
	FHjsonDocumentPtr DeepDocument;
	TestTrue(
		TEXT("257 deep legal document has no fixed depth limit"),
		TryParseHjsonDocument(
			BuildDeepDocument(
				static_cast<int32>(Depth->GetNumberField(TEXT("legal_depth"))),
				Depth->GetStringField(TEXT("leaf"))),
			TEXT("deep.hjson"),
			DeepTracker,
			DeepDocument,
			Error));
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
				FString(
					TEXT(
						"sha256:145d78cf812e7de349d9d420bbe45c9c1"
						"e5cdce01753f964902530f00f6071cc")));
		}
	}

	IFileManager::Get().DeleteDirectory(*Root, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentHjsonWorkspaceReferenceBudgetTest,
	"UeAgentInterface.Core.HjsonWorkspace.ReferenceDagCycleAndFileBudget",
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

	const FString OverflowRoot = FPaths::Combine(Root, TEXT("overflow"));
	WriteOwnedFile(
		OverflowRoot,
		TEXT("index.hjson"),
		References->GetStringField(TEXT("entry_text")));
	WriteOwnedFile(
		OverflowRoot,
		TEXT("units/child.hjson"),
		References->GetStringField(TEXT("child_text")));
	WriteOwnedFile(
		OverflowRoot,
		TEXT("unused.hjson"),
		References->GetStringField(TEXT("child_text")));
	FUeAgentResourceTracker OverflowTracker = MakeTracker(Fixture);
	FCanonicalWorkspacePtr OverflowWorkspace;
	TestFalse(
		TEXT("N plus one stops during enumeration"),
		TryLoadWorkspace(
			FWorkspaceSource::Directory(OverflowRoot),
			MakePolicy(Fixture),
			OverflowTracker,
			OverflowWorkspace,
			Error));
	TestEqual(
		TEXT("file budget error is typed"),
		Error.GetCode(),
		FString(TEXT("workspace_file_budget_exceeded")));
	TestTrue(TEXT("file budget stopped early"), Error.IsStoppedEarly());
	TestEqual(TEXT("N plus one observed"), Error.GetObserved(), 3ll);

	IFileManager::Get().DeleteDirectory(*Root, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentHjsonWorkspaceExportPlanTest,
	"UeAgentInterface.Core.HjsonWorkspace.ImmutableExportPlan",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentHjsonWorkspaceExportPlanTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	FUeAgentResourceTracker SnapshotTracker = MakeTracker(Fixture);
	const FUeAgentCanonicalSnapshotPtr Snapshot =
		BuildSnapshot(this, Fixture, SnapshotTracker);
	if (!Snapshot.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Row =
		Fixture->GetObjectField(TEXT("export_plan"));
	const FExportPlanRequest Request(
		Snapshot.ToSharedRef(),
		EWorkspaceKind::Directory,
		Row->GetStringField(TEXT("entry_file")),
		Row->GetStringField(TEXT("contract_id")),
		static_cast<int32>(
			Row->GetNumberField(TEXT("contract_version"))),
		Row->GetStringField(TEXT("profile_id")),
		static_cast<int64>(
			Row->GetNumberField(TEXT("token_budget"))));
	FUeAgentResourceTracker FirstTracker = MakeTracker(Fixture);
	FUeAgentResourceTracker SecondTracker = MakeTracker(Fixture);
	FExportWorkspacePlanPtr First;
	FExportWorkspacePlanPtr Second;
	FWorkspaceError Error;
	const bool bFirstBuilt =
		TryBuildExportPlan(Request, FirstTracker, First, Error);
	const bool bSecondBuilt =
		TryBuildExportPlan(Request, SecondTracker, Second, Error);
	TestTrue(
		TEXT("build first immutable plan"),
		bFirstBuilt);
	TestTrue(
		TEXT("build deterministic second plan"),
		bSecondBuilt);
	if (!bFirstBuilt || !bSecondBuilt
		|| !First.IsValid() || !Second.IsValid())
	{
		AddError(FString::Printf(
			TEXT(
				"immutable export plan missing: code=%s source=%s "
				"pointer=%s observed=%lld"),
			*Error.GetCode(),
			*Error.GetSourceUnit(),
			*Error.GetJsonPointer(),
			Error.GetObserved()));
		return false;
	}
	TestEqual(
		TEXT("plan fingerprint deterministic"),
		First->GetPlanFingerprint(),
		Second->GetPlanFingerprint());
	TestTrue(
		TEXT("plan binds C4 revision"),
		First->GetSnapshotRevision() == Snapshot->GetRevision());
	TestEqual(TEXT("single reference file"), First->GetFiles().Num(), 1);
	const TArray<uint8>& Bytes = First->GetFiles()[0].GetCanonicalUtf8();
	const FString Text = FString(UTF8_TO_TCHAR(
		reinterpret_cast<const char*>(Bytes.GetData())));
	for (const TSharedPtr<FJsonValue>& Fragment
		: Row->GetArrayField(TEXT("required_fragments")))
	{
		TestTrue(
			TEXT("plan preserves canonical truth"),
			Text.Contains(Fragment->AsString()));
	}

	return true;
}
