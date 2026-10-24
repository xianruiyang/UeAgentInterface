#include "Misc/AutomationTest.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "CommandEdge/UeAgentCommandProtocol.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "DomainAdapters/LocalizationStringTable/UeAgentStringTableAuthoringApply.h"
#include "DomainAdapters/LocalizationStringTable/UeAgentStringTableAuthoringState.h"
#include "DomainAdapters/LocalizationStringTable/UeAgentLocalizationSettingsAuthoringState.h"
#include "DomainAdapters/LocalizationStringTable/UeAgentLocalizationSettingsAuthoringApply.h"
#include "DomainAdapters/LocalizationStringTable/UeAgentLocalizationSettingsConfig.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Internationalization/StringTableRegistry.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/Culture.h"
#include "LocalizationConfigurationScript.h"
#include "LocalizationSettings.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "ObjectTools.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/SavePackage.h"
#include "UObject/Class.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Vertical/T208/UeAgentT208WorkspaceValueEdit.h"

namespace
{
	TArray<TPair<FString, FString>> NativeSources(const FStringTable& Table)
	{
		TArray<TPair<FString, FString>> Result;
		Table.EnumerateSourceStrings([&](const FString& Key, const FString& Value) { Result.Emplace(Key, Value); return true; });
		Result.Sort([](const auto& A, const auto& B) { return A.Key.Compare(B.Key, ESearchCase::CaseSensitive) < 0; });
		return Result;
	}
	bool HasSource(const TArray<TPair<FString, FString>>& Entries, const FString& Key, const FString& Source)
	{
		const auto* Entry = Entries.FindByPredicate([&](const auto& Item) { return Item.Key.Equals(Key, ESearchCase::CaseSensitive); });
		return Entry && Entry->Value.Equals(Source, ESearchCase::CaseSensitive);
	}
	TSharedPtr<FJsonObject> FindRecord(const TArray<TSharedPtr<FJsonValue>>& Entries, const FString& Key)
	{
		for (const auto& Entry : Entries)
			if (Entry->AsObject()->GetStringField(TEXT("key")).Equals(Key, ESearchCase::CaseSensitive)) return Entry->AsObject();
		return nullptr;
	}
	using namespace UE::UAI;

	class FNeverCancelled final
		: public Contract::IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override { return false; }
	};

	CommandEdge::FUeAgentCommandResult SubmitPublicCommand(
		const TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>& Dispatch,
		const FString& CommandId,
		const FString& RequestId,
		const TSharedRef<FJsonObject>& Params)
	{
		const auto* Command = Registry.IsValid()
			? Registry->FindExact(CommandId)
			: nullptr;
		FString ParamsJson;
		FString JsonError;
		if (!Command || !Dispatch.IsValid()
			|| !CommandContract::Json::TryCanonicalize(
				Params, ParamsJson, JsonError))
		{
			return CommandEdge::FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t208-stringtable"),
				RequestId,
				CommandId,
				TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
				TEXT("stringtable_public_command_unavailable"),
				TEXT("the public command or request is unavailable"));
		}
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"corr-t208-stringtable\",")
			TEXT("\"request_id\":\"%s\",\"command_id\":\"%s\",")
			TEXT("\"command_binding_hash\":\"%s\",\"params\":%s}"),
			*RequestId,
			*CommandId,
			*Command->GetContractHash(),
			*ParamsJson);
		FTCHARToUTF8 Encoded(*Envelope);
		TArray<uint8> Bytes;
		Bytes.Append(
			reinterpret_cast<const uint8*>(Encoded.Get()),
			Encoded.Length());
		CommandEdge::FUeAgentCommandProtocolDiagnostic Diagnostic;
		TOptional<CommandEdge::FUeAgentCommandRequest> Request;
		if (!CommandEdge::FUeAgentCommandProtocolCodec().DecodeRequest(
				Bytes, Request, Diagnostic)
			|| !Request.IsSet())
		{
			return CommandEdge::FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t208-stringtable"),
				RequestId,
				CommandId,
				Command->GetContractHash(),
				TEXT("stringtable_public_request_invalid"),
				TEXT("the public request could not be decoded"));
		}
		return Dispatch->Submit(
			Request.GetValue(),
			Contract::FUeAgentCallerContext(
				TEXT("t208-stringtable"),
				{TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")},
				TEXT("corr-t208-stringtable")),
			MakeShared<FNeverCancelled>());
	}

	bool ReadOrdinaryEntries(const FString& Workspace, FString& File, HjsonWorkspace::FHjsonDocumentPtr& Document)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *Workspace, TEXT("*.hjson"), true, false);
		for (const auto& Candidate : Files)
		{
			if (Candidate.Contains(TEXT("/.uai/")) || Candidate.Contains(TEXT("\\.uai\\"))) continue;
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *Candidate)) return false;
			auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
			HjsonWorkspace::FWorkspaceError Error;
			if (!HjsonWorkspace::TryParseHjsonDocument(Text, Candidate, Resources, Document, Error)) return false;
			if (Document->GetRoot()->Get(TEXT("entries"))) { File = Candidate; return true; }
		}
		return false;
	}
	class FOwnedTable
	{
	public:
		explicit FOwnedTable(FAutomationTestBase& InTest) : Test(InTest)
		{
			PackageName = TEXT("/Game/UAI/Transient/T208StringTable/ST_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
			Path = PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageName);
			Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		}
		~FOwnedTable()
		{
			if (UStringTable* Table = FindObject<UStringTable>(nullptr, *Path))
			{
				TGuardValue<bool> Guard(GIsRunningUnattendedScript, true);
				Table->GetOutermost()->SetDirtyFlag(false);
				if (ObjectTools::ForceDeleteObjects({Table}, false) != 1)
					Test.AddError(TEXT("owned StringTable UE deletion failed"));
			}
			if (FindObject<UStringTable>(nullptr, *Path) || FPaths::FileExists(Filename))
				Test.AddError(TEXT("owned StringTable cleanup did not reach object and disk absence"));
		}
		UStringTable* Create()
		{
			UPackage* Package = CreatePackage(*PackageName);
			UStringTable* Table = NewObject<UStringTable>(Package,
				*FPackageName::GetLongPackageAssetName(PackageName), RF_Public | RF_Standalone | RF_Transactional);
			FStringTable& Native = *Table->GetMutableStringTable();
			Native.SetNamespace(TEXT("before"));
			for (const FString& Key : {TEXT("Keep"), TEXT("Clear"), TEXT("Replace"), TEXT("Case"), TEXT("case"), TEXT("Removed")})
			{
				Native.SetSourceString(Key, Key + TEXT(" before"));
				Native.SetMetaData(Key, TEXT("owner"), TEXT("preserved"));
			}
			FAssetRegistryModule::AssetCreated(Table);
			FSavePackageArgs Args;
			Args.TopLevelFlags = RF_Public | RF_Standalone;
			if (!UPackage::SavePackage(Package, Table, *Filename, Args)) return nullptr;
			Package->SetDirtyFlag(false);
			return Table;
		}
		FString PackageName, Path, Filename;
	private:
		FAutomationTestBase& Test;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208StringTablePublicSourceVerticalTest,
	"UAI.Root.T208.StringTable.PublicDirectorySourceVertical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208StringTablePublicSourceVerticalTest::RunTest(const FString&)
{
	FOwnedTable Fixture(*this);
	UStringTable* Table = Fixture.Create();
	if (!TestNotNull(TEXT("owned persistent StringTable"), Table)) return false;
	TWeakObjectPtr<UStringTable> Original(Table);
	const FString Workspace = FPaths::ProjectSavedDir() / TEXT("UeAssetFolders/T208StringTable/edit");
	const FString Readback = Workspace + TEXT("-readback");
	const FString CaseWorkspace = Readback + TEXT("-case");
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*Workspace, false, true);
		IFileManager::Get().DeleteDirectory(*Readback, false, true);
		IFileManager::Get().DeleteDirectory(*CaseWorkspace, false, true);
	};
	using namespace UE::UAI::Module::CommandRegistration::Active;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(TEXT("public command composition"), TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)))
	{
		AddError(Error);
		return false;
	}
	const auto Export = MakeShared<FJsonObject>();
	Export->SetStringField(TEXT("target"), Fixture.Path);
	Export->SetStringField(TEXT("workspace_path"), Workspace);
	Export->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
	const auto ExportResult = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"),
		TEXT("req-t208-stringtable-export"), Export);
	if (!TestTrue(TEXT("public semantic directory export"), ExportResult.IsOk()))
	{
		AddError(ExportResult.GetErrorJson());
		return false;
	}
	FString EntryFile;
	HjsonWorkspace::FHjsonDocumentPtr Document;
	if (!TestTrue(TEXT("ordinary entries document is readable"), ReadOrdinaryEntries(Workspace, EntryFile, Document))) return false;
	const auto* OriginalEntries = Document->GetRoot()->Get(TEXT("entries"));
	const auto* Keep = OriginalEntries ? OriginalEntries->Get(TEXT("Keep")) : nullptr;
	if (!TestTrue(TEXT("export preserves both exact-case native keys"), OriginalEntries
		&& OriginalEntries->GetEntries().Num() == 6 && OriginalEntries->Get(TEXT("Case"))
		&& OriginalEntries->Get(TEXT("case")) && OriginalEntries->Get(TEXT("Case")) != OriginalEntries->Get(TEXT("case")))) return false;
	if (!TestTrue(TEXT("normal export includes native metadata"), Keep && Keep->Get(TEXT("metadata"))
		&& Keep->Get(TEXT("metadata"))->Get(TEXT("owner")))) return false;
	const FString Edited = FString(TEXT("namespace: \"after\"\nentries: {\n")
		TEXT(" Keep: {source: \"Keep after\"}\n Clear: {source: \"Clear after\", metadata: {}}\n")
		TEXT(" Replace: {source: \"Replace after\", metadata: {type: \"replacement\"}}\n")
		TEXT(" Case: {source: \"Upper\"}\n case: {source: \"Lower\"}\n"))
		+ TEXT(" None: {source: \"Added\"")
		+ TEXT(", metadata: {owner: \"new owner\", path: \"data\", \"$uai\": \"decimal\", value: \"7\"}}\n}\n");
	if (!TestTrue(TEXT("edit only the ordinary file"), FFileHelper::SaveStringToFile(Edited, *EntryFile,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) return false;
	const auto Source = MakeShared<FJsonObject>();
	Source->SetStringField(TEXT("kind"), TEXT("directory"));
	Source->SetStringField(TEXT("path"), Workspace);
	const auto Import = MakeShared<FJsonObject>();
	Import->SetStringField(TEXT("target"), Fixture.Path);
	Import->SetObjectField(TEXT("source"), Source);
	Table = nullptr;
	const auto Imported = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"),
		TEXT("req-t208-stringtable-import"), Import);
	if (!TestTrue(TEXT("public desired-state import succeeds"), Imported.IsOk()))
	{
		AddError(Imported.GetErrorJson());
		return false;
	}
	Table = LoadObject<UStringTable>(nullptr, *Fixture.Path);
	if (!TestNotNull(TEXT("persisted StringTable reloads"), Table)) return false;
	TestFalse(TEXT("persistent verification replaced the original UObject"), Original.IsValid());
	TestTrue(TEXT("native registry points to the reloaded table"),
		FStringTableRegistry::Get().FindStringTable(Table->GetStringTableId()) == Table->GetStringTable());
	const FStringTable& Native = *Table->GetStringTable();
	TestTrue(TEXT("namespace persists"), Native.GetNamespace().Equals(TEXT("after"), ESearchCase::CaseSensitive));
	const auto Actual = NativeSources(Native);
	TestEqual(TEXT("one addition and one deletion"), Actual.Num(), 6);
	TestFalse(TEXT("removed key absent"), Actual.ContainsByPredicate([](const auto& Entry) { return Entry.Key.Equals(TEXT("Removed"), ESearchCase::CaseSensitive); }));
	TestTrue(TEXT("existing entry changed"), HasSource(Actual, TEXT("Keep"), TEXT("Keep after")));
	TestTrue(TEXT("uppercase key identity"), HasSource(Actual, TEXT("Case"), TEXT("Upper")));
	TestTrue(TEXT("lowercase key identity"), HasSource(Actual, TEXT("case"), TEXT("Lower")));
	TestTrue(TEXT("None is a valid native key"), HasSource(Actual, TEXT("None"), TEXT("Added")));
	TestTrue(TEXT("metadata tag remains literal"), Native.GetMetaData(TEXT("None"), TEXT("$uai")).Equals(TEXT("decimal"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("metadata tagged value remains a string"), Native.GetMetaData(TEXT("None"), TEXT("value")).Equals(TEXT("7"), ESearchCase::CaseSensitive));
	TestEqual(TEXT("omitted metadata preserved"), Native.GetMetaData(TEXT("Keep"), TEXT("owner")), FString(TEXT("preserved")));
	int32 ClearCount = 0;
	Native.EnumerateMetaData(TEXT("Clear"), [&](FName, const FString&) { ++ClearCount; return true; });
	TestEqual(TEXT("explicit empty metadata clears"), ClearCount, 0);
	TestTrue(TEXT("explicit object replaces old metadata"), Native.GetMetaData(TEXT("Replace"), TEXT("owner")).IsEmpty());
	TestEqual(TEXT("data dictionary type key remains editable"), Native.GetMetaData(TEXT("Replace"), TEXT("type")), FString(TEXT("replacement")));
	TestEqual(TEXT("data dictionary path key remains editable"), Native.GetMetaData(TEXT("None"), TEXT("path")), FString(TEXT("data")));
	Export->SetStringField(TEXT("workspace_path"), Readback);
	const auto Reexport = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"),
		TEXT("req-t208-stringtable-reexport"), Export);
	if (!TestTrue(TEXT("fresh public reexport"), Reexport.IsOk())) { AddError(Reexport.GetErrorJson()); return false; }
	if (!TestTrue(TEXT("fresh ordinary entries parse"), ReadOrdinaryEntries(Readback, EntryFile, Document))) return false;
	const auto* Entries = Document->GetRoot()->Get(TEXT("entries"));
	if (!TestTrue(TEXT("fresh export contains native desired keys"), Entries && Entries->GetEntries().Num() == Actual.Num())) return false;
	for (const auto& Entry : Actual)
	{
		const auto* Record = Entries->Get(Entry.Key);
		const auto* Value = Record ? Record->Get(TEXT("source")) : nullptr;
		if (!TestTrue(TEXT("reexport exact source value"), Value && Value->GetText().Equals(Entry.Value, ESearchCase::CaseSensitive))) return false;
	}
	if (!FFileHelper::SaveStringToFile(TEXT("namespace: \"AFTER\"\n"), *EntryFile,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	Source->SetStringField(TEXT("path"), Readback);
	const auto NamespaceOnly = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"),
		TEXT("req-t208-stringtable-namespace-only"), Import);
	if (!TestTrue(TEXT("namespace-only document preserves omitted entries"), NamespaceOnly.IsOk()))
	{
		AddError(NamespaceOnly.GetErrorJson());
		return false;
	}
	Table = LoadObject<UStringTable>(nullptr, *Fixture.Path);
	if (!TestNotNull(TEXT("namespace-only result reloads"), Table)) return false;
	TestTrue(TEXT("case-only namespace edit persists"), Table->GetStringTable()->GetNamespace().Equals(TEXT("AFTER"), ESearchCase::CaseSensitive));
	const auto AfterNamespace = NativeSources(*Table->GetStringTable());
	TestEqual(TEXT("namespace-only entry count preserved"), AfterNamespace.Num(), Actual.Num());
	for (const auto& Entry : Actual) TestTrue(TEXT("namespace-only source strings preserved"), HasSource(AfterNamespace, Entry.Key, Entry.Value));
	TestEqual(TEXT("namespace-only metadata preserved"), Table->GetStringTable()->GetMetaData(TEXT("Keep"), TEXT("owner")),
		FString(TEXT("preserved")));
	Export->SetStringField(TEXT("workspace_path"), CaseWorkspace);
	const auto CaseExport = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"),
		TEXT("req-t208-stringtable-case-export"), Export);
	if (!CaseExport.IsOk()) { AddError(CaseExport.GetErrorJson()); return false; }
	if (!ReadOrdinaryEntries(CaseWorkspace, EntryFile, Document)) return false;
	FString CaseText;
	if (!FFileHelper::LoadFileToString(CaseText, *EntryFile)) return false;
	if (!TestEqual(TEXT("one source changes only case"), CaseText.ReplaceInline(TEXT("Keep after"), TEXT("KEEP AFTER"), ESearchCase::CaseSensitive), 1)) return false;
	if (!FFileHelper::SaveStringToFile(CaseText, *EntryFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	Source->SetStringField(TEXT("path"), CaseWorkspace);
	const auto CaseImport = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"),
		TEXT("req-t208-stringtable-case-import"), Import);
	if (!CaseImport.IsOk()) { AddError(CaseImport.GetErrorJson()); return false; }
	Table = LoadObject<UStringTable>(nullptr, *Fixture.Path);
	if (!TestNotNull(TEXT("case-only source result reloads"), Table)) return false;
	TestTrue(TEXT("case-only source edit persists"), HasSource(NativeSources(*Table->GetStringTable()), TEXT("Keep"), TEXT("KEEP AFTER")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208StringTablePublicInitializationTest,
	"UAI.Root.T208.StringTable.PublicInitialization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208StringTablePublicInitializationTest::RunTest(const FString&)
{
	using namespace UE::UAI::DomainAdapters::LocalizationStringTable;
	FOwnedTable Fixture(*this);
	if (!TestTrue(TEXT("initialization starts with real absence"), IsStringTableTargetAbsent(Fixture.Path))) return false;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
	const auto Source = MakeShared<FJsonObject>();
	Source->SetStringField(TEXT("kind"), TEXT("inline"));
	Source->SetStringField(TEXT("document"), TEXT("schema: \"uai.string_table.edit/1\"\nroot: {\n")
		TEXT(" type: \"/Script/Engine.StringTable\"\n namespace: \"\"\n entries: {\n")
		TEXT(" Case: {source: \"Upper\", metadata: {owner: \"kept\", \"$uai\": \"decimal\", value: \"7\"}}\n")
		TEXT(" case: {source: \"lower\"}\n None: {source: \"\"}\n }\n}\n"));
	const auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("target"), Fixture.Path);
	Params->SetObjectField(TEXT("source"), Source);
	const auto Created = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"),
		TEXT("req-t208-stringtable-initialize"), Params);
	if (!TestTrue(TEXT("public missing initialization succeeds"), Created.IsOk())) { AddError(Created.GetErrorJson()); return false; }
	UStringTable* Table = LoadObject<UStringTable>(nullptr, *Fixture.Path);
	if (!TestNotNull(TEXT("created persistent asset independently resolves"), Table)) return false;
	TestTrue(TEXT("created package exists on disk"), FPaths::FileExists(Fixture.Filename));
	TestFalse(TEXT("created asset is clean after persistent verification"), Table->GetOutermost()->IsDirty());
	const auto Native = Table->GetStringTable();
	const auto Sources = NativeSources(*Native);
	TestEqual(TEXT("all exact native keys created"), Sources.Num(), 3);
	TestTrue(TEXT("uppercase key and text preserved"), HasSource(Sources, TEXT("Case"), TEXT("Upper")));
	TestTrue(TEXT("lowercase key and text preserved"), HasSource(Sources, TEXT("case"), TEXT("lower")));
	TestTrue(TEXT("None key accepts explicit empty source"), HasSource(Sources, TEXT("None"), TEXT("")));
	TestTrue(TEXT("explicit empty namespace is preserved"), Native->GetNamespace().IsEmpty());
	TestTrue(TEXT("metadata owner preserved"), Native->GetMetaData(TEXT("Case"), TEXT("owner")).Equals(TEXT("kept"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("metadata semantic-looking tag stays literal"), Native->GetMetaData(TEXT("Case"), TEXT("$uai")).Equals(TEXT("decimal"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("created registry binding matches native asset"), FStringTableRegistry::Get().FindStringTable(Table->GetStringTableId()) == Native);
	const FString Workspace = FPaths::ProjectSavedDir() / TEXT("UeAssetFolders/T208StringTable/created-readback");
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Workspace, false, true); };
	const auto Export = MakeShared<FJsonObject>();
	Export->SetStringField(TEXT("target"), Fixture.Path);
	Export->SetStringField(TEXT("workspace_path"), Workspace);
	Export->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
	const auto Exported = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"),
		TEXT("req-t208-stringtable-created-export"), Export);
	if (!TestTrue(TEXT("created asset exports through semantic directory"), Exported.IsOk())) { AddError(Exported.GetErrorJson()); return false; }
	FString File;
	HjsonWorkspace::FHjsonDocumentPtr Document;
	if (!ReadOrdinaryEntries(Workspace, File, Document)) return false;
	const auto* Entries = Document->GetRoot()->Get(TEXT("entries"));
	if (!TestTrue(TEXT("reexport preserves all exact keys"), Entries && Entries->GetEntries().Num() == 3)) return false;
	for (const auto& Pair : Sources)
	{
		const auto* Entry = Entries->Get(Pair.Key);
		const auto* Value = Entry ? Entry->Get(TEXT("source")) : nullptr;
		TestTrue(TEXT("parsed reexport agrees with native source"), Value && Value->GetKind() == HjsonWorkspace::EHjsonNodeKind::String
			&& Value->GetText().Equals(Pair.Value, ESearchCase::CaseSensitive));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208StringTableInitializationLifecycleTest,
	"UAI.Root.T208.StringTable.InitializationLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208StringTableInitializationLifecycleTest::RunTest(const FString&)
{
	using namespace UE::UAI::DomainAdapters::LocalizationStringTable;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UeAgentAuthoringOperationApply;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
	auto Import = [&](const FString& Path, const FString& Fields, const FString& Request)
	{
		const auto Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("kind"), TEXT("inline"));
		Source->SetStringField(TEXT("document"), TEXT("schema: \"uai.string_table.edit/1\"\nroot: {\n type: \"/Script/Engine.StringTable\"\n") + Fields + TEXT("\n}\n"));
		const auto Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), Path);
		Params->SetObjectField(TEXT("source"), Source);
		return SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), Request, Params);
	};
	{
		FOwnedTable Defaults(*this);
		const auto Result = Import(Defaults.Path, TEXT(""), TEXT("req-t208-stringtable-defaults"));
		if (!TestTrue(TEXT("public omitted namespace and entries initialize"), Result.IsOk())) { AddError(Result.GetErrorJson()); return false; }
		UStringTable* Table = LoadObject<UStringTable>(nullptr, *Defaults.Path);
		if (!TestNotNull(TEXT("default initialized asset reloads"), Table)) return false;
		TestTrue(TEXT("omitted namespace uses native short name"), Table->GetStringTable()->GetNamespace().Equals(Table->GetName(), ESearchCase::CaseSensitive));
		TestEqual(TEXT("omitted entries create empty table"), NativeSources(*Table->GetStringTable()).Num(), 0);
		TestTrue(TEXT("default initialized table persisted"), FPaths::FileExists(Defaults.Filename));
	}
	if (HasAnyErrors()) return false;
	FOwnedTable Fixture(*this);
	auto VerifyAbsent = [&]()
	{
		TestNull(TEXT("restored absence has no UObject"), FindObject<UObject>(nullptr, *Fixture.Path));
		TestFalse(TEXT("restored absence has no native registry table"), FStringTableRegistry::Get().FindStringTable(FName(*Fixture.Path)).IsValid());
		TestFalse(TEXT("restored absence has no disk package"), FPaths::FileExists(Fixture.Filename));
		return !HasAnyErrors();
	};
	const auto Invalid = Import(Fixture.Path, TEXT("entries: {\n Good: {source: \"valid\"}\n Bad: {source: true}\n}"), TEXT("req-t208-stringtable-create-invalid"));
	if (!TestFalse(TEXT("non-string source rejects public creation before writing"), Invalid.IsOk()) || !VerifyAbsent()) return false;
	TSharedPtr<FJsonObject> InvalidError;
	if (!TestTrue(TEXT("public rejection preserves typed source error"),
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Invalid.GetErrorJson()), InvalidError)
		&& InvalidError.IsValid() && InvalidError->GetStringField(TEXT("code")) == TEXT("string_table_source_invalid"))) return false;
	const auto Desired = MakeShared<FJsonObject>();
	Desired->SetStringField(TEXT("namespace"), TEXT("created"));
	const auto Record = MakeShared<FJsonObject>();
	Record->SetStringField(TEXT("key"), TEXT("Case"));
	Record->SetStringField(TEXT("source"), TEXT("created source"));
	const auto Metadata = MakeShared<FJsonObject>();
	Metadata->SetStringField(TEXT("owner"), TEXT("created metadata"));
	Record->SetObjectField(TEXT("metadata"), Metadata);
	Desired->SetArrayField(TEXT("entries"), {MakeShared<FJsonValueObject>(Record)});
	class FRejectCreatedStage final : public IDurablePackageStager
	{
	public:
		explicit FRejectCreatedStage(FString InPath) : Path(MoveTemp(InPath)), Inner(CreateDefaultDurablePackageStager()) {}
		bool BuildArtifactInventory(const TArray<FPackageDurablePlan>& Plans,
			TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>& Artifacts, FOperationFailure& Failure) override
		{ return Inner->BuildArtifactInventory(Plans, Artifacts, Failure); }
		bool StagePackages(const TArray<FPackageDurablePlan>& Plans,
			TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>& Artifacts, FOperationFailure& Failure) override
		{
			const auto* Table = FindObject<UStringTable>(nullptr, *Path);
			FString Source;
			if (Table && Table->GetStringTable()->GetSourceString(TEXT("Case"), Source)
				&& Source.Equals(TEXT("created source"), ESearchCase::CaseSensitive))
			{
				bObservedWrite = true;
				Failure.ErrorCode = TEXT("t208_created_stage_rejected");
				Failure.ErrorDetail = Failure.ErrorCode;
				return false;
			}
			return Inner->StagePackages(Plans, Artifacts, Failure);
		}
		bool bObservedWrite = false;
	private:
		FString Path;
		TSharedRef<IDurablePackageStager> Inner;
	};
	const auto Stager = MakeShared<FRejectCreatedStage>(Fixture.Path);
	FBackendOptions Options;
	Options.PackageStager = Stager;
	HjsonWorkspace::DomainAuthoring::FRootPropertyApplyOutcome Early;
	Early.FinalReadback = [](FOperationFailure&) { return true; };
	TestFalse(TEXT("post-create staging rejection fails"), ApplyStringTableAuthoring(Fixture.Path, TEXT("t208-create-early"), Desired, Early, &Options, true));
	if (!TestTrue(TEXT("early rejection observed actual created value"), Stager->bObservedWrite)) { AddError(Early.ErrorCode + Early.ErrorDetail); return false; }
	if (!VerifyAbsent()) return false;
	bool bObservedPublished = false;
	HjsonWorkspace::DomainAuthoring::FRootPropertyApplyOutcome Late;
	Late.FinalReadback = [&](FOperationFailure& Failure)
	{
		const auto* Table = LoadObject<UStringTable>(nullptr, *Fixture.Path);
		FString Source;
		bObservedPublished = Table && FPaths::FileExists(Fixture.Filename) && !Table->GetOutermost()->IsDirty()
			&& !Table->GetOutermost()->HasAnyPackageFlags(PKG_NewlyCreated)
			&& Table->GetStringTable()->GetSourceString(TEXT("Case"), Source)
			&& Source.Equals(TEXT("created source"), ESearchCase::CaseSensitive)
			&& Table->GetStringTable()->GetMetaData(TEXT("Case"), TEXT("owner")).Equals(TEXT("created metadata"), ESearchCase::CaseSensitive)
			&& FStringTableRegistry::Get().FindStringTable(Table->GetStringTableId()) == Table->GetStringTable();
		Failure.ErrorCode = TEXT("t208_created_late_rejected");
		Failure.ErrorDetail = Failure.ErrorCode;
		return false;
	};
	TestFalse(TEXT("published create final rejection fails"), ApplyStringTableAuthoring(Fixture.Path, TEXT("t208-create-late"), Desired, Late, nullptr, true));
	if (!TestTrue(TEXT("late rejection observes published native table and binding"), bObservedPublished)) { AddError(Late.ErrorCode + Late.ErrorDetail); return false; }
	return VerifyAbsent();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208LocalizationSettingsPublicUpdateTest,
	"UAI.Root.T208.LocalizationSettings.PublicUpdate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208LocalizationSettingsPublicUpdateTest::RunTest(const FString&)
{
	using namespace UE::UAI::DomainAdapters::LocalizationStringTable;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	auto* Settings = GetMutableDefault<ULocalizationSettings>();
	auto* Set = ULocalizationSettings::GetGameTargetSet();
	if (!Settings || !Set) return false;
	const FString Filename = Settings->GetDefaultConfigFilename();
	const bool bOriginalFile = FPaths::FileExists(Filename);
	TArray<uint8> OriginalBytes;
	if (bOriginalFile && !FFileHelper::LoadFileToArray(OriginalBytes, *Filename)) return false;
	const auto OriginalObjects = Set->TargetObjects;
	const auto OriginalNative = CaptureLocalizationGameSettings();
	if (OriginalObjects.Num() != OriginalNative.Num()) { AddError(TEXT("native target collection contains null entries")); return false; }
	TArray<FLocalizationTargetSettings> OriginalBacking;
	FString Error;
	if (!GetLocalizationBackingSettings(*Settings, OriginalBacking, Error)) { AddError(Error); return false; }
	const FString WorkRoot = FPaths::ProjectSavedDir() / TEXT("UeAssetFolders/T208LocalizationSettings");
	const FString Stage = WorkRoot / TEXT("seed.ini");
	IFileManager::Get().MakeDirectory(*WorkRoot, true);
	TArray<FString> ScriptPaths;
	ON_SCOPE_EXIT
	{
		Set->TargetObjects = OriginalObjects;
		for (int32 Index = 0; Index < OriginalObjects.Num(); ++Index)
			if (OriginalObjects[Index]) OriginalObjects[Index]->Settings = OriginalNative[Index];
		FString RestoreError;
		if (!SetLocalizationBackingSettings(*Settings, OriginalBacking, RestoreError)) AddError(RestoreError);
		if (bOriginalFile)
		{
			if (!FFileHelper::SaveArrayToFile(OriginalBytes, *Filename)) AddError(TEXT("DefaultEditor original bytes restore failed"));
			TArray<uint8> Restored;
			if (!FFileHelper::LoadFileToArray(Restored, *Filename) || Restored != OriginalBytes) AddError(TEXT("DefaultEditor byte restoration unconfirmed"));
		}
		else if (!IFileManager::Get().Delete(*Filename, false, true)) AddError(TEXT("DefaultEditor absent restoration failed"));
		for (const auto& Path : ScriptPaths)
			if (!IFileManager::Get().Delete(*Path, false, true) || FPaths::FileExists(Path)) AddError(TEXT("owned localization script cleanup failed"));
		IFileManager::Get().DeleteDirectory(*WorkRoot, false, true);
	};
	ULocalizationTarget* Target = NewObject<ULocalizationTarget>(Set);
	Target->Settings.Name = TEXT("T208Settings_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	Target->Settings.RequiredModuleNames = {TEXT("UeAgentInterface")};
	FCultureStatistics English;
	English.CultureName = TEXT("en");
	Target->Settings.SupportedCulturesStatistics = {English};
	Target->Settings.NativeCultureIndex = 0;
	const FGuid OriginalGuid = Target->Settings.Guid;
	ScriptPaths = LocalizationConfigurationScript::GetConfigPaths(Target);
	for (const auto& Path : ScriptPaths) if (FPaths::FileExists(Path)) { ScriptPaths.Reset(); AddError(TEXT("owned script path unexpectedly exists")); return false; }
	// Test native preservation with an owned neighbor, independent of project config
	// inheritance (which may already contain invalid duplicate target identities).
	auto* Neighbor = NewObject<ULocalizationTarget>(Set);
	Neighbor->Settings = Target->Settings;
	Neighbor->Settings.Name += TEXT("_Unchanged");
	Neighbor->Settings.Guid = FGuid::NewGuid();
	const auto NeighborBefore = Neighbor->Settings;
	Set->TargetObjects = {Target, Neighbor};
	const auto Seed = CaptureLocalizationGameSettings();
	if (!SetLocalizationBackingSettings(*Settings, Seed, Error) || !WriteLocalizationSettingsStage(Seed, Stage, Error)) { AddError(Error); return false; }
	TArray<uint8> SeedBytes;
	if (!FFileHelper::LoadFileToArray(SeedBytes, *Stage) || !FFileHelper::SaveArrayToFile(SeedBytes, *Filename)) return false;
	const FString Sentinel = TEXT("\n[T208LocalizationUnrelated]\n; T208 unrelated text remains\nKeep=Unchanged\n");
	FString SeedText;
	if (!FFileHelper::LoadFileToString(SeedText, *Filename)
		|| !FFileHelper::SaveStringToFile(SeedText + Sentinel, *Filename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	FConfigFile BeforeConfig;
	BeforeConfig.Read(Filename);
	TArray<FString> BeforeEngine;
	BeforeConfig.GetArray(*Settings->GetClass()->GetPathName(), TEXT("+EngineTargetsSettings"), BeforeEngine);
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
	const auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("target"), Settings->GetPathName());
	Params->SetStringField(TEXT("workspace_path"), WorkRoot / TEXT("edit"));
	Params->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
	const auto Exported = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"), TEXT("req-t208-localization-export"), Params);
	if (!TestTrue(TEXT("public settings export succeeds"), Exported.IsOk())) { AddError(Exported.GetErrorJson()); return false; }
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *(WorkRoot / TEXT("edit")), TEXT("*.hjson"), true, false);
	FString Editable;
	for (const auto& File : Files)
	{
		if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File)) return false;
		auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
		HjsonWorkspace::FHjsonDocumentPtr Document;
		HjsonWorkspace::FWorkspaceError ParseError;
		if (!HjsonWorkspace::TryParseHjsonDocument(Text, File, Resources, Document, ParseError)) return false;
		const auto* Targets = Document->GetRoot()->Get(TEXT("targets"));
		if (!Targets) continue;
		if (!TestNotNull(TEXT("ordinary keyed target name is visible"), Targets->Get(Target->Settings.Name))) return false;
		Editable = File;
		const auto* Record = Targets->Get(Target->Settings.Name);
		const auto* Cultures = Record ? Record->Get(TEXT("cultures")) : nullptr;
		if (!Cultures) return false;
		int32 Start = 0;
		for (int32 Line = 1; Line < Cultures->GetProvenance().GetLine(); ++Line)
		{
			Start = Text.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
			if (Start == INDEX_NONE) return false;
			++Start;
		}
		Start += Cultures->GetProvenance().GetColumn() - 1;
		const int32 End = Text.Find(TEXT("]"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
		if (End < Start) return false;
		Text = Text.Left(Start) + TEXT("[\"en\", \"fr\"]") + Text.Mid(End + 1);
		TSharedPtr<FJsonObject> Edited;
		if (!FUeAgentStructuredHjsonParser::ParseObject(Text, File, Edited, Error)) { AddError(Error); return false; }
		const auto EditedTarget = Edited->GetObjectField(TEXT("targets"))->GetObjectField(Target->Settings.Name);
		const auto SettingsValue = EditedTarget->TryGetField(TEXT("settings"));
		if (!TestTrue(TEXT("ordinary native settings exposed"), SettingsValue.IsValid())) return false;
		const int32 SettingsEdits = Tests::WorkspaceValueEdit::ReplaceNamedLeaf(SettingsValue, TEXT("RequiredModuleNames"),
			MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueString>(TEXT("Core"))}));
		if (!TestEqual(TEXT("native setting has one shared typed location"), SettingsEdits, 1)) return false;
		Text = FUeAgentStructuredHjsonFormatter::WriteObject(Edited.ToSharedRef());
		if (!FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
		break;
	}
	if (!TestFalse(TEXT("ordinary targets document located"), Editable.IsEmpty())) return false;
	const auto Source = MakeShared<FJsonObject>();
	Source->SetStringField(TEXT("kind"), TEXT("directory"));
	Source->SetStringField(TEXT("path"), WorkRoot / TEXT("edit"));
	const auto Import = MakeShared<FJsonObject>();
	Import->SetStringField(TEXT("target"), Settings->GetPathName());
	Import->SetObjectField(TEXT("source"), Source);
	const auto Imported = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), TEXT("req-t208-localization-import"), Import);
	if (!TestTrue(TEXT("public settings directory update succeeds"), Imported.IsOk())) { AddError(Imported.GetErrorJson()); return false; }
	TestEqual(TEXT("native cultures changed"), Target->Settings.SupportedCulturesStatistics.Num(), 2);
	TestTrue(TEXT("target identity preserved"), Target->Settings.Guid == OriginalGuid);
	TestTrue(TEXT("native settings change applied alongside cultures"), Target->Settings.RequiredModuleNames == TArray<FString>{TEXT("Core")});
	TestTrue(TEXT("unrelated owned target full native settings preserved"),
		FLocalizationTargetSettings::StaticStruct()->CompareScriptStruct(&Neighbor->Settings, &NeighborBefore, PPF_None));
	for (int32 Index = 0; Index < OriginalObjects.Num(); ++Index)
		TestTrue(TEXT("unrelated target full native settings preserved"), OriginalObjects[Index]
			&& FLocalizationTargetSettings::StaticStruct()->CompareScriptStruct(&OriginalObjects[Index]->Settings, &OriginalNative[Index], PPF_None));
	TArray<FLocalizationTargetSettings> Disk;
	if (!ReadLocalizationSettingsFile(Filename, Disk, Error)) { AddError(Error); return false; }
	const auto* Persisted = Disk.FindByPredicate([&](const auto& Item) { return Item.Name == Target->Settings.Name; });
	if (!TestNotNull(TEXT("final disk independently contains native target"), Persisted)) return false;
	TestTrue(TEXT("disk retains GUID and unrelated native fields"), Persisted->Guid == OriginalGuid && Persisted->RequiredModuleNames == Target->Settings.RequiredModuleNames);
	TestTrue(TEXT("disk contains exact requested cultures"), Persisted->SupportedCulturesStatistics.Num() == 2
		&& Persisted->SupportedCulturesStatistics[1].CultureName == TEXT("fr") && Persisted->NativeCultureIndex == 0);
	FString FinalText;
	FFileHelper::LoadFileToString(FinalText, *Filename);
	TestTrue(TEXT("unrelated section and comment survive"), FinalText.Replace(TEXT("\r\n"), TEXT("\n")).Contains(Sentinel.TrimStart()));
	FConfigFile AfterConfig;
	AfterConfig.Read(Filename);
	TArray<FString> AfterEngine;
	AfterConfig.GetArray(*Settings->GetClass()->GetPathName(), TEXT("+EngineTargetsSettings"), AfterEngine);
	TestTrue(TEXT("Engine target config remains unchanged"), BeforeEngine == AfterEngine);
	for (const auto& Path : ScriptPaths)
	{
		FConfigFile Script;
		Script.Read(Path);
		FString NativeCulture;
		TArray<FString> Cultures;
		Script.GetString(TEXT("CommonSettings"), TEXT("NativeCulture"), NativeCulture);
		Script.GetArray(TEXT("CommonSettings"), TEXT("CulturesToGenerate"), Cultures);
		const bool bReport = Path == LocalizationConfigurationScript::GetWordCountReportConfigPath(Target);
		TestTrue(TEXT("formal native script reflects cultures"), FPaths::FileExists(Path)
			&& (bReport || NativeCulture == TEXT("en")) && Cultures.Contains(TEXT("fr")));
	}
	Params->SetStringField(TEXT("workspace_path"), WorkRoot / TEXT("readback"));
	const auto Reexport = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"), TEXT("req-t208-localization-reexport"), Params);
	if (!TestTrue(TEXT("updated settings reexport succeeds"), Reexport.IsOk())) { AddError(Reexport.GetErrorJson()); return false; }
	Files.Reset();
	IFileManager::Get().FindFilesRecursive(Files, *(WorkRoot / TEXT("readback")), TEXT("*.hjson"), true, false);
	bool bReexportObserved = false;
	for (const auto& File : Files)
	{
		if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File)) return false;
		auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
		HjsonWorkspace::FHjsonDocumentPtr Document;
		HjsonWorkspace::FWorkspaceError ParseError;
		if (!HjsonWorkspace::TryParseHjsonDocument(Text, File, Resources, Document, ParseError)) return false;
		const auto* Targets = Document->GetRoot()->Get(TEXT("targets"));
		const auto* Item = Targets ? Targets->Get(Target->Settings.Name) : nullptr;
		const auto* Cultures = Item ? Item->Get(TEXT("cultures")) : nullptr;
		if (Cultures && Cultures->GetKind() == HjsonWorkspace::EHjsonNodeKind::Array && Cultures->GetItems().Num() == 2)
			bReexportObserved = Cultures->GetItems()[1]->GetKind() == HjsonWorkspace::EHjsonNodeKind::String
				&& Cultures->GetItems()[1]->GetText().Equals(TEXT("fr"), ESearchCase::CaseSensitive);
	}
	TestTrue(TEXT("parsed reexport agrees with requested native cultures"), bReexportObserved);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208LocalizationSettingsPublicCreateTest,
	"UAI.Root.T208.LocalizationSettings.PublicCreate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208LocalizationSettingsPublicCreateTest::RunTest(const FString&)
{
	using namespace UE::UAI::DomainAdapters::LocalizationStringTable;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	auto* Settings = GetMutableDefault<ULocalizationSettings>();
	auto* Set = ULocalizationSettings::GetGameTargetSet();
	if (!Settings || !Set) return false;
	const FString Filename = Settings->GetDefaultConfigFilename();
	const bool bOriginalFile = FPaths::FileExists(Filename);
	TArray<uint8> OriginalBytes;
	if (bOriginalFile && !FFileHelper::LoadFileToArray(OriginalBytes, *Filename)) return false;
	const auto OriginalObjects = Set->TargetObjects;
	const auto OriginalNative = CaptureLocalizationGameSettings();
	if (OriginalObjects.Num() != OriginalNative.Num()) return false;
	TArray<FLocalizationTargetSettings> OriginalBacking;
	FString Error;
	if (!GetLocalizationBackingSettings(*Settings, OriginalBacking, Error)) { AddError(Error); return false; }
	const FString Name = TEXT("T208Created_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString WorkRoot = FPaths::ProjectSavedDir() / TEXT("UeAssetFolders/T208LocalizationSettingsCreate");
	TArray<FString> ScriptPaths;
	ULocalizationTarget* Created = nullptr;
	ON_SCOPE_EXIT
	{
		for (const auto& Object : Set->TargetObjects)
			if (Object && Object->Settings.Name.Equals(Name, ESearchCase::CaseSensitive))
			{
				ScriptPaths = LocalizationConfigurationScript::GetConfigPaths(Object.Get());
				Object->MarkAsGarbage();
			}
		Set->TargetObjects = OriginalObjects;
		for (int32 Index = 0; Index < OriginalObjects.Num(); ++Index) OriginalObjects[Index]->Settings = OriginalNative[Index];
		FString RestoreError;
		if (!SetLocalizationBackingSettings(*Settings, OriginalBacking, RestoreError)) AddError(RestoreError);
		if (bOriginalFile)
		{
			TArray<uint8> Restored;
			if (!FFileHelper::SaveArrayToFile(OriginalBytes, *Filename)
				|| !FFileHelper::LoadFileToArray(Restored, *Filename) || Restored != OriginalBytes) AddError(TEXT("create fixture original config restoration failed"));
		}
		else if (!IFileManager::Get().Delete(*Filename, false, true) || FPaths::FileExists(Filename)) AddError(TEXT("create fixture absent config restoration failed"));
		for (const auto& Path : ScriptPaths)
			if (!IFileManager::Get().Delete(*Path, false, true) || FPaths::FileExists(Path)) AddError(TEXT("create fixture script cleanup failed"));
		IFileManager::Get().DeleteDirectory(*WorkRoot, false, true);
	};
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
	const auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("target"), Settings->GetPathName());
	Params->SetStringField(TEXT("workspace_path"), WorkRoot / TEXT("edit"));
	Params->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
	const auto Exported = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"), TEXT("req-t208-settings-create-export"), Params);
	if (!TestTrue(TEXT("public create source export succeeds"), Exported.IsOk())) { AddError(Exported.GetErrorJson()); return false; }
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *(WorkRoot / TEXT("edit")), TEXT("*.hjson"), true, false);
	bool bEdited = false;
	for (const auto& File : Files)
	{
		if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File)) return false;
		auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
		HjsonWorkspace::FHjsonDocumentPtr Document;
		HjsonWorkspace::FWorkspaceError ParseError;
		if (!HjsonWorkspace::TryParseHjsonDocument(Text, File, Resources, Document, ParseError)) return false;
		const auto* Targets = Document->GetRoot()->Get(TEXT("targets"));
		if (!Targets) continue;
		int32 Start = 0;
		for (int32 Line = 1; Line < Targets->GetProvenance().GetLine(); ++Line)
		{
			Start = Text.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
			if (Start == INDEX_NONE) return false;
			++Start;
		}
		Start += Targets->GetProvenance().GetColumn() - 1;
		if (!Text.IsValidIndex(Start) || Text[Start] != TEXT('{')) return false;
		Text.InsertAt(Start + 1, TEXT("\n") + Name + TEXT(": {native_culture: \"en\", cultures: [\"en\", \"fr\"]}\n"));
		if (!FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
		bEdited = true;
		break;
	}
	if (!TestTrue(TEXT("ordinary targets file extended by business key"), bEdited)) return false;
	const auto Source = MakeShared<FJsonObject>();
	Source->SetStringField(TEXT("kind"), TEXT("directory"));
	Source->SetStringField(TEXT("path"), WorkRoot / TEXT("edit"));
	const auto Import = MakeShared<FJsonObject>();
	Import->SetStringField(TEXT("target"), Settings->GetPathName());
	Import->SetObjectField(TEXT("source"), Source);
	const auto Imported = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), TEXT("req-t208-settings-create-import"), Import);
	if (!TestTrue(TEXT("public keyed target creation succeeds"), Imported.IsOk())) { AddError(Imported.GetErrorJson()); return false; }
	for (const auto& Target : Set->TargetObjects) if (Target && Target->Settings.Name.Equals(Name, ESearchCase::CaseSensitive)) Created = Target.Get();
	if (!TestNotNull(TEXT("created native target is in Game set"), Created)) return false;
	TestTrue(TEXT("native target Outer is Game set"), Created->GetOuter() == Set);
	TestTrue(TEXT("native constructor allocated fresh target GUID"), Created->Settings.Guid.IsValid()
		&& !OriginalNative.ContainsByPredicate([&](const auto& Item) { return Item.Guid == Created->Settings.Guid; }));
	TestTrue(TEXT("created native culture values are exact"), Created->Settings.NativeCultureIndex == 0
		&& Created->Settings.SupportedCulturesStatistics.Num() == 2
		&& Created->Settings.SupportedCulturesStatistics[1].CultureName.Equals(TEXT("fr"), ESearchCase::CaseSensitive));
	for (int32 Index = 0; Index < OriginalObjects.Num(); ++Index)
		TestTrue(TEXT("create preserves original target identity and full value"), Set->TargetObjects[Index] == OriginalObjects[Index]
			&& FLocalizationTargetSettings::StaticStruct()->CompareScriptStruct(&OriginalObjects[Index]->Settings, &OriginalNative[Index], PPF_None));
	TArray<FLocalizationTargetSettings> Disk;
	if (!ReadLocalizationSettingsFile(Filename, Disk, Error)) { AddError(Error); return false; }
	const auto* Persisted = Disk.FindByPredicate([&](const auto& Item) { return Item.Name.Equals(Name, ESearchCase::CaseSensitive); });
	if (!TestNotNull(TEXT("created target is independently decoded from final config"), Persisted)) return false;
	TestTrue(TEXT("disk target GUID and cultures agree with actual native target"), Persisted->Guid == Created->Settings.Guid
		&& Persisted->NativeCultureIndex == 0 && Persisted->SupportedCulturesStatistics.Num() == 2
		&& Persisted->SupportedCulturesStatistics[1].CultureName.Equals(TEXT("fr"), ESearchCase::CaseSensitive));
	ScriptPaths = LocalizationConfigurationScript::GetConfigPaths(Created);
	TestEqual(TEXT("eight formal scripts"), ScriptPaths.Num(), 8);
	for (const auto& Path : ScriptPaths)
	{
		FConfigFile Script;
		Script.Read(Path);
		TArray<FString> Cultures;
		Script.GetArray(TEXT("CommonSettings"), TEXT("CulturesToGenerate"), Cultures);
		TestTrue(TEXT("created native script has requested culture"), FPaths::FileExists(Path) && Cultures.Contains(TEXT("fr")));
	}
	TestFalse(TEXT("target creation does not manufacture data tree"), FPaths::DirectoryExists(LocalizationConfigurationScript::GetDataDirectory(Created)));
	Params->SetStringField(TEXT("workspace_path"), WorkRoot / TEXT("readback"));
	const auto Reexported = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"), TEXT("req-t208-settings-create-readback"), Params);
	if (!TestTrue(TEXT("created target reexport succeeds"), Reexported.IsOk())) { AddError(Reexported.GetErrorJson()); return false; }
	Files.Reset();
	IFileManager::Get().FindFilesRecursive(Files, *(WorkRoot / TEXT("readback")), TEXT("*.hjson"), true, false);
	bool bObserved = false;
	for (const auto& File : Files)
	{
		if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File)) return false;
		auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
		HjsonWorkspace::FHjsonDocumentPtr Document;
		HjsonWorkspace::FWorkspaceError ParseError;
		if (!HjsonWorkspace::TryParseHjsonDocument(Text, File, Resources, Document, ParseError)) return false;
		const auto* Targets = Document->GetRoot()->Get(TEXT("targets"));
		const auto* Record = Targets ? Targets->Get(Name) : nullptr;
		const auto* Cultures = Record ? Record->Get(TEXT("cultures")) : nullptr;
		if (Cultures && Cultures->GetKind() == HjsonWorkspace::EHjsonNodeKind::Array) bObserved = Cultures->GetItems().Num() == 2
			&& Cultures->GetItems()[1]->GetKind() == HjsonWorkspace::EHjsonNodeKind::String
			&& Cultures->GetItems()[1]->GetText().Equals(TEXT("fr"), ESearchCase::CaseSensitive);
	}
	TestTrue(TEXT("normal reexport exposes new business key and exact cultures"), bObserved);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208LocalizationSettingsPublicRemoveTest,
	"UAI.Root.T208.LocalizationSettings.PublicRemove",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208LocalizationSettingsPublicRemoveTest::RunTest(const FString&)
{
	using namespace UE::UAI::DomainAdapters::LocalizationStringTable;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	auto* Settings = GetMutableDefault<ULocalizationSettings>();
	auto* Set = ULocalizationSettings::GetGameTargetSet();
	const FString Filename = Settings->GetDefaultConfigFilename();
	const bool bOriginalFile = FPaths::FileExists(Filename);
	TArray<uint8> OriginalBytes;
	if (bOriginalFile && !FFileHelper::LoadFileToArray(OriginalBytes, *Filename)) return false;
	const auto OriginalObjects = Set->TargetObjects;
	const auto OriginalNative = CaptureLocalizationGameSettings();
	if (OriginalObjects.Num() != OriginalNative.Num()) return false;
	TArray<FLocalizationTargetSettings> OriginalBacking;
	FString Error;
	if (!GetLocalizationBackingSettings(*Settings, OriginalBacking, Error)) { AddError(Error); return false; }
	const FString Name = TEXT("T208Removed_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString WorkRoot = FPaths::ProjectSavedDir() / TEXT("UeAssetFolders/T208LocalizationRemove");
	FString DataRoot, UnrelatedRoot;
	TArray<FString> ScriptPaths;
	ON_SCOPE_EXIT
	{
		for (const auto& Object : Set->TargetObjects)
			if (Object && Object->Settings.Name.Equals(Name, ESearchCase::CaseSensitive)) Object->MarkAsGarbage();
		Set->TargetObjects = OriginalObjects;
		for (int32 Index = 0; Index < OriginalObjects.Num(); ++Index) OriginalObjects[Index]->Settings = OriginalNative[Index];
		FString RestoreError;
		if (!SetLocalizationBackingSettings(*Settings, OriginalBacking, RestoreError)) AddError(RestoreError);
		if (bOriginalFile)
		{
			TArray<uint8> Restored;
			if (!FFileHelper::SaveArrayToFile(OriginalBytes, *Filename)
				|| !FFileHelper::LoadFileToArray(Restored, *Filename) || Restored != OriginalBytes) AddError(TEXT("remove fixture original config restoration failed"));
		}
		else if (!IFileManager::Get().Delete(*Filename, false, true)) AddError(TEXT("remove fixture absent config restoration failed"));
		for (const auto& Path : ScriptPaths)
			if (!IFileManager::Get().Delete(*Path, false, true) || FPaths::FileExists(Path)) AddError(TEXT("remove fixture script cleanup failed"));
		for (const auto& Path : {DataRoot, UnrelatedRoot}) if (!Path.IsEmpty())
			if (!IFileManager::Get().DeleteDirectory(*Path, false, true) || FPaths::DirectoryExists(Path)) AddError(TEXT("remove fixture data cleanup failed"));
		IFileManager::Get().DeleteDirectory(*WorkRoot, false, true);
	};
	const auto Seed = CaptureLocalizationSettingsState(OriginalNative);
	if (!Seed) { AddError(TEXT("localization fixture capture failed")); return false; }
	auto Records = Seed->GetArrayField(TEXT("targets"));
	const auto NewRecord = MakeShared<FJsonObject>();
	NewRecord->SetStringField(TEXT("name"), Name);
	NewRecord->SetStringField(TEXT("native_culture"), TEXT("en"));
	NewRecord->SetArrayField(TEXT("cultures"), {MakeShared<FJsonValueString>(TEXT("en")), MakeShared<FJsonValueString>(TEXT("fr"))});
	Records.Add(MakeShared<FJsonValueObject>(NewRecord));
	Seed->SetArrayField(TEXT("targets"), Records);
	HjsonWorkspace::DomainAuthoring::FRootPropertyApplyOutcome Seeded;
	if (!ApplyLocalizationSettings(TEXT("t208-remove-seed"), Seed.ToSharedRef(), Seeded)) { AddError(Seeded.ErrorCode + TEXT(":") + Seeded.ErrorDetail); return false; }
	TWeakObjectPtr<ULocalizationTarget> Target;
	for (const auto& Object : Set->TargetObjects) if (Object && Object->Settings.Name.Equals(Name, ESearchCase::CaseSensitive)) Target = Object.Get();
	if (!Target.IsValid()) return false;
	ScriptPaths = LocalizationConfigurationScript::GetConfigPaths(Target.Get());
	DataRoot = LocalizationConfigurationScript::GetDataDirectory(Target.Get());
	const FString ProposedUnrelated = DataRoot + TEXT("_unrelated");
	if (FPaths::DirectoryExists(DataRoot) || FPaths::FileExists(DataRoot)
		|| FPaths::DirectoryExists(ProposedUnrelated) || FPaths::FileExists(ProposedUnrelated)) { DataRoot.Reset(); AddError(TEXT("owned data path collision")); return false; }
	UnrelatedRoot = ProposedUnrelated;
	if (!IFileManager::Get().MakeDirectory(*(DataRoot / TEXT("nested/empty")), true)
		|| !IFileManager::Get().MakeDirectory(*(DataRoot / TEXT("empty")), true)
		|| !IFileManager::Get().MakeDirectory(*UnrelatedRoot, true)) return false;
	const TArray<uint8> Binary{0, 1, 127, 128, 254, 255, 0};
	if (!FFileHelper::SaveStringToFile(TEXT("owned nested source"), *(DataRoot / TEXT("nested/source.txt")))
		|| !FFileHelper::SaveArrayToFile(Binary, *(DataRoot / TEXT("nested/data.bin")))
		|| !FFileHelper::SaveStringToFile(TEXT("unrelated remains"), *(UnrelatedRoot / TEXT("keep.txt")))) return false;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
	const auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("target"), Settings->GetPathName());
	Params->SetStringField(TEXT("workspace_path"), WorkRoot / TEXT("edit"));
	Params->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
	const auto Exported = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"), TEXT("req-t208-remove-export"), Params);
	if (!Exported.IsOk()) { AddError(Exported.GetErrorJson()); return false; }
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *(WorkRoot / TEXT("edit")), TEXT("*.hjson"), true, false);
	bool bEdited = false;
	for (const auto& File : Files)
	{
		if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File)) return false;
		auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
		HjsonWorkspace::FHjsonDocumentPtr Document;
		HjsonWorkspace::FWorkspaceError ParseError;
		if (!HjsonWorkspace::TryParseHjsonDocument(Text, File, Resources, Document, ParseError)) return false;
		const auto* Targets = Document->GetRoot()->Get(TEXT("targets"));
		if (!Targets || !Targets->Get(Name)) continue;
		int32 Start = Text.Find(Name, ESearchCase::CaseSensitive);
		if (Start == INDEX_NONE) return false;
		if (Start > 0 && Text[Start - 1] == TEXT('"')) --Start;
		int32 End = Text.Find(TEXT("}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
		if (End == INDEX_NONE) return false;
		++End;
		while (Text.IsValidIndex(End) && (Text[End] == TEXT(',') || FChar::IsWhitespace(Text[End]))) ++End;
		Text.RemoveAt(Start, End - Start);
		if (!FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
		bEdited = true;
		break;
	}
	if (!TestTrue(TEXT("ordinary target key removed"), bEdited)) return false;
	const auto Source = MakeShared<FJsonObject>();
	Source->SetStringField(TEXT("kind"), TEXT("directory"));
	Source->SetStringField(TEXT("path"), WorkRoot / TEXT("edit"));
	const auto Import = MakeShared<FJsonObject>();
	Import->SetStringField(TEXT("target"), Settings->GetPathName());
	Import->SetObjectField(TEXT("source"), Source);
	const auto Imported = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), TEXT("req-t208-remove-import"), Import);
	if (!TestTrue(TEXT("public target removal succeeds"), Imported.IsOk())) { AddError(Imported.GetErrorJson()); return false; }
	TestFalse(TEXT("removed native target exits after commit"), Target.IsValid());
	TestTrue(TEXT("remaining native object identities preserved"), Set->TargetObjects == OriginalObjects);
	for (int32 Index = 0; Index < OriginalObjects.Num(); ++Index)
		TestTrue(TEXT("remaining full native values preserved"), FLocalizationTargetSettings::StaticStruct()->CompareScriptStruct(
			&OriginalObjects[Index]->Settings, &OriginalNative[Index], PPF_None));
	TArray<FLocalizationTargetSettings> Disk;
	if (!ReadLocalizationSettingsFile(Filename, Disk, Error)) { AddError(Error); return false; }
	TestFalse(TEXT("final config independently excludes removed target"), Disk.ContainsByPredicate([&](const auto& Item) { return Item.Name.Equals(Name, ESearchCase::CaseSensitive); }));
	for (const auto& Path : ScriptPaths) TestFalse(TEXT("formal script removed"), FPaths::FileExists(Path));
	TestFalse(TEXT("complete data root including empty directories removed"), FPaths::DirectoryExists(DataRoot));
	FString Unrelated;
	TestTrue(TEXT("unrelated data directory unchanged"), FFileHelper::LoadFileToString(Unrelated, *(UnrelatedRoot / TEXT("keep.txt"))) && Unrelated == TEXT("unrelated remains"));
	Params->SetStringField(TEXT("workspace_path"), WorkRoot / TEXT("readback"));
	const auto Reexported = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"), TEXT("req-t208-remove-readback"), Params);
	if (!Reexported.IsOk()) { AddError(Reexported.GetErrorJson()); return false; }
	Files.Reset();
	IFileManager::Get().FindFilesRecursive(Files, *(WorkRoot / TEXT("readback")), TEXT("*.hjson"), true, false);
	bool bObserved = false;
	for (const auto& File : Files)
	{
		if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File)) return false;
		auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
		HjsonWorkspace::FHjsonDocumentPtr Document;
		HjsonWorkspace::FWorkspaceError ParseError;
		if (!HjsonWorkspace::TryParseHjsonDocument(Text, File, Resources, Document, ParseError)) return false;
		if (const auto* Targets = Document->GetRoot()->Get(TEXT("targets"))) bObserved = Targets->Get(Name) == nullptr;
	}
	TestTrue(TEXT("reexport excludes deleted business key"), bObserved);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208LocalizationSettingsRemoveLifecycleTest,
	"UAI.Root.T208.LocalizationSettings.RemoveLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208LocalizationSettingsRemoveLifecycleTest::RunTest(const FString&)
{
	using namespace UE::UAI::DomainAdapters::LocalizationStringTable;
	using namespace UeAgentAuthoringWorkspaceTransaction;
	auto* Settings = GetMutableDefault<ULocalizationSettings>();
	auto* Set = ULocalizationSettings::GetGameTargetSet();
	if (!Settings || !Set) return false;
	const FString Filename = Settings->GetDefaultConfigFilename();
	const bool bOriginalFile = FPaths::FileExists(Filename);
	TArray<uint8> OriginalBytes;
	if (bOriginalFile && !FFileHelper::LoadFileToArray(OriginalBytes, *Filename)) return false;
	const auto OriginalObjects = Set->TargetObjects;
	const auto OriginalNative = CaptureLocalizationGameSettings();
	if (OriginalObjects.Num() != OriginalNative.Num()) return false;
	TArray<FLocalizationTargetSettings> OriginalBacking;
	FString Error;
	if (!GetLocalizationBackingSettings(*Settings, OriginalBacking, Error)) { AddError(Error); return false; }
	const FString Name = TEXT("T208RemoveRollback_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	FString DataRoot;
	TArray<FString> Scripts;
	ON_SCOPE_EXIT
	{
		for (const auto& Object : Set->TargetObjects)
			if (Object && Object->Settings.Name.Equals(Name, ESearchCase::CaseSensitive)) Object->MarkAsGarbage();
		Set->TargetObjects = OriginalObjects;
		for (int32 Index = 0; Index < OriginalObjects.Num(); ++Index) OriginalObjects[Index]->Settings = OriginalNative[Index];
		FString RestoreError;
		if (!SetLocalizationBackingSettings(*Settings, OriginalBacking, RestoreError)) AddError(RestoreError);
		if (bOriginalFile)
		{
			TArray<uint8> Restored;
			if (!FFileHelper::SaveArrayToFile(OriginalBytes, *Filename) || !FFileHelper::LoadFileToArray(Restored, *Filename)
				|| Restored != OriginalBytes) AddError(TEXT("remove lifecycle config cleanup failed"));
		}
		else if (!IFileManager::Get().Delete(*Filename, false, true)) AddError(TEXT("remove lifecycle config absence cleanup failed"));
		for (const auto& Path : Scripts)
			if (!IFileManager::Get().Delete(*Path, false, true) || FPaths::FileExists(Path)) AddError(TEXT("remove lifecycle script cleanup failed"));
		if (!DataRoot.IsEmpty() && (!IFileManager::Get().DeleteDirectory(*DataRoot, false, true) || FPaths::DirectoryExists(DataRoot)))
			AddError(TEXT("remove lifecycle tree cleanup failed"));
	};
	const auto Seed = CaptureLocalizationSettingsState(OriginalNative);
	if (!Seed) { AddError(TEXT("localization fixture capture failed")); return false; }
	auto Records = Seed->GetArrayField(TEXT("targets"));
	const auto Added = MakeShared<FJsonObject>();
	Added->SetStringField(TEXT("name"), Name);
	Added->SetStringField(TEXT("native_culture"), TEXT("en"));
	Added->SetArrayField(TEXT("cultures"), {MakeShared<FJsonValueString>(TEXT("en"))});
	Records.Add(MakeShared<FJsonValueObject>(Added));
	Seed->SetArrayField(TEXT("targets"), Records);
	HjsonWorkspace::DomainAuthoring::FRootPropertyApplyOutcome Seeded;
	if (!ApplyLocalizationSettings(TEXT("t208-remove-lifecycle-seed"), Seed.ToSharedRef(), Seeded)) { AddError(Seeded.ErrorCode + TEXT(":") + Seeded.ErrorDetail); return false; }
	ULocalizationTarget* Target = nullptr;
	for (const auto& Object : Set->TargetObjects) if (Object && Object->Settings.Name.Equals(Name, ESearchCase::CaseSensitive)) Target = Object.Get();
	if (!Target) return false;
	const TWeakObjectPtr<ULocalizationTarget> OriginalTarget(Target);
	Scripts = LocalizationConfigurationScript::GetConfigPaths(Target);
	const FString ProposedDataRoot = LocalizationConfigurationScript::GetDataDirectory(Target);
	if (FPaths::DirectoryExists(ProposedDataRoot) || FPaths::FileExists(ProposedDataRoot)) { AddError(TEXT("owned data path collision")); return false; }
	DataRoot = ProposedDataRoot;
	if (!IFileManager::Get().MakeDirectory(*(DataRoot / TEXT("nested/empty")), true)
		|| !IFileManager::Get().MakeDirectory(*(DataRoot / TEXT("empty")), true)) return false;
	const TArray<uint8> Binary{0, 255, 128, 9, 0, 17};
	const FString Text = TEXT("original nested text\nsecond line");
	if (!FFileHelper::SaveArrayToFile(Binary, *(DataRoot / TEXT("nested/data.bin")))
		|| !FFileHelper::SaveStringToFile(Text, *(DataRoot / TEXT("nested/source.txt")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	TArray<uint8> BeforeDisk;
	if (!FFileHelper::LoadFileToArray(BeforeDisk, *Filename)) return false;
	TArray<TArray<uint8>> BeforeScripts;
	for (const auto& Path : Scripts) if (!FFileHelper::LoadFileToArray(BeforeScripts.AddDefaulted_GetRef(), *Path)) return false;
	Target->Settings.RequiredModuleNames.Add(TEXT("T208DirtyRemovedTarget"));
	Target->Settings.ConflictStatus = ELocalizationTargetConflictStatus::ConflictsPresent;
	const auto BeforeObjects = Set->TargetObjects;
	const auto BeforeMemory = CaptureLocalizationGameSettings();
	if (!SetLocalizationBackingSettings(*Settings, BeforeMemory, Error)) { AddError(Error); return false; }
	const auto Desired = CaptureLocalizationSettingsState(BeforeMemory);
	if (!Desired) { AddError(TEXT("localization fixture capture failed")); return false; }
	Records = Desired->GetArrayField(TEXT("targets"));
	Records.RemoveAll([&](const auto& Record) { return Record->AsObject()->GetStringField(TEXT("name")).Equals(Name, ESearchCase::CaseSensitive); });
	Desired->SetArrayField(TEXT("targets"), Records);
	bool bLateObserved = false;
	HjsonWorkspace::DomainAuthoring::FRootPropertyApplyOutcome Late;
	Late.FinalReadback = [&](FOperationFailure& Failure)
	{
		TArray<FLocalizationTargetSettings> Disk;
		FString ReadError;
		bLateObserved = OriginalTarget.IsValid() && OriginalTarget.Get() == Target && !Set->TargetObjects.Contains(Target)
			&& ReadLocalizationSettingsFile(Filename, Disk, ReadError)
			&& !Disk.ContainsByPredicate([&](const auto& Item) { return Item.Name.Equals(Name, ESearchCase::CaseSensitive); })
			&& !FPaths::DirectoryExists(DataRoot);
		for (const auto& Path : Scripts) bLateObserved = bLateObserved && !FPaths::FileExists(Path);
		Failure.ErrorCode = TEXT("t208_remove_late_rejection");
		Failure.ErrorDetail = Failure.ErrorCode;
		return false;
	};
	TestFalse(TEXT("late removal rejection fails"), ApplyLocalizationSettings(TEXT("t208-settings-remove-late"), Desired.ToSharedRef(), Late));
	TestTrue(TEXT("late observed removed collection/config/scripts/tree while original object remains recoverable"), bLateObserved);
	TestTrue(TEXT("same original native object restored"), OriginalTarget.IsValid() && OriginalTarget.Get() == Target && Set->TargetObjects == BeforeObjects);
	const auto Current = CaptureLocalizationGameSettings();
	TArray<FLocalizationTargetSettings> Backing;
	bool bSame = Current.Num() == BeforeMemory.Num() && GetLocalizationBackingSettings(*Settings, Backing, Error) && Backing.Num() == BeforeMemory.Num();
	if (bSame) for (int32 Index = 0; Index < Current.Num(); ++Index)
		bSame = bSame && FLocalizationTargetSettings::StaticStruct()->CompareScriptStruct(&Current[Index], &BeforeMemory[Index], PPF_None)
			&& FLocalizationTargetSettings::StaticStruct()->CompareScriptStruct(&Backing[Index], &BeforeMemory[Index], PPF_None);
	TestTrue(TEXT("complete dirty native and backing values restored"), bSame);
	TArray<uint8> DiskBytes;
	TestTrue(TEXT("different original config bytes restored"), FFileHelper::LoadFileToArray(DiskBytes, *Filename) && DiskBytes == BeforeDisk);
	for (int32 Index = 0; Index < Scripts.Num(); ++Index)
		TestTrue(TEXT("original script bytes restored"), FFileHelper::LoadFileToArray(DiskBytes, *Scripts[Index]) && DiskBytes == BeforeScripts[Index]);
	TestTrue(TEXT("nested binary restored exactly"), FFileHelper::LoadFileToArray(DiskBytes, *(DataRoot / TEXT("nested/data.bin"))) && DiskBytes == Binary);
	FString RestoredText;
	TestTrue(TEXT("nested text restored exactly"), FFileHelper::LoadFileToString(RestoredText, *(DataRoot / TEXT("nested/source.txt"))) && RestoredText.Equals(Text, ESearchCase::CaseSensitive));
	TestTrue(TEXT("root and both empty directories restored"), FPaths::DirectoryExists(DataRoot / TEXT("empty")) && FPaths::DirectoryExists(DataRoot / TEXT("nested/empty")));
	TArray<FString> RestoredFiles, RestoredDirectories;
	IFileManager::Get().FindFilesRecursive(RestoredFiles, *DataRoot, TEXT("*"), true, false);
	IFileManager::Get().FindFilesRecursive(RestoredDirectories, *DataRoot, TEXT("*"), false, true);
	TestTrue(TEXT("restored tree has exactly the original members"), RestoredFiles.Num() == 2 && RestoredDirectories.Num() == 3);
	TestTrue(TEXT("rollback independently confirmed"), Late.ErrorDetail.Contains(TEXT("rollback=semantically_confirmed")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208LocalizationSettingsCreateLifecycleTest,
	"UAI.Root.T208.LocalizationSettings.CreateLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208LocalizationSettingsCreateLifecycleTest::RunTest(const FString&)
{
	using namespace UE::UAI::DomainAdapters::LocalizationStringTable;
	using namespace UeAgentAuthoringWorkspaceTransaction;
	auto* Settings = GetMutableDefault<ULocalizationSettings>();
	auto* Set = ULocalizationSettings::GetGameTargetSet();
	if (!Settings || !Set || Set->TargetObjects.IsEmpty()) return false;
	const FString Filename = Settings->GetDefaultConfigFilename();
	const bool bOriginalFile = FPaths::FileExists(Filename);
	TArray<uint8> OriginalBytes;
	if (bOriginalFile && !FFileHelper::LoadFileToArray(OriginalBytes, *Filename)) return false;
	const auto OriginalObjects = Set->TargetObjects;
	const auto OriginalNative = CaptureLocalizationGameSettings();
	if (OriginalObjects.Num() != OriginalNative.Num()) return false;
	TArray<FLocalizationTargetSettings> OriginalBacking;
	FString Error;
	if (!GetLocalizationBackingSettings(*Settings, OriginalBacking, Error)) { AddError(Error); return false; }
	const FString Name = TEXT("T208CreateRollback_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString WorkRoot = FPaths::ProjectSavedDir() / TEXT("UeAssetFolders/T208LocalizationCreateLifecycle");
	IFileManager::Get().MakeDirectory(*WorkRoot, true);
	TArray<FString> ScriptPaths;
	ON_SCOPE_EXIT
	{
		for (const auto& Object : Set->TargetObjects)
			if (Object && Object->Settings.Name.Equals(Name, ESearchCase::CaseSensitive)) Object->MarkAsGarbage();
		Set->TargetObjects = OriginalObjects;
		for (int32 Index = 0; Index < OriginalObjects.Num(); ++Index) OriginalObjects[Index]->Settings = OriginalNative[Index];
		FString RestoreError;
		if (!SetLocalizationBackingSettings(*Settings, OriginalBacking, RestoreError)) AddError(RestoreError);
		if (bOriginalFile)
		{
			TArray<uint8> Restored;
			if (!FFileHelper::SaveArrayToFile(OriginalBytes, *Filename)
				|| !FFileHelper::LoadFileToArray(Restored, *Filename) || Restored != OriginalBytes) AddError(TEXT("create lifecycle original config restoration failed"));
		}
		else if (!IFileManager::Get().Delete(*Filename, false, true) || FPaths::FileExists(Filename)) AddError(TEXT("create lifecycle config absence restoration failed"));
		for (const auto& Path : ScriptPaths)
			if (!IFileManager::Get().Delete(*Path, false, true) || FPaths::FileExists(Path)) AddError(TEXT("create lifecycle script cleanup failed"));
		IFileManager::Get().DeleteDirectory(*WorkRoot, false, true);
	};
	const FString SeedPath = WorkRoot / TEXT("seed.ini");
	if (!WriteLocalizationSettingsStage(OriginalNative, SeedPath, Error)) { AddError(Error); return false; }
	TArray<uint8> BeforeDisk;
	if (!FFileHelper::LoadFileToArray(BeforeDisk, *SeedPath) || !FFileHelper::SaveArrayToFile(BeforeDisk, *Filename)) return false;
	OriginalObjects[0]->Settings.RequiredModuleNames.Add(TEXT("T208DirtyMemory"));
	OriginalObjects[0]->Settings.ConflictStatus = ELocalizationTargetConflictStatus::ConflictsPresent;
	const auto BeforeMemory = CaptureLocalizationGameSettings();
	if (!SetLocalizationBackingSettings(*Settings, BeforeMemory, Error)) { AddError(Error); return false; }
	const auto Desired = CaptureLocalizationSettingsState(BeforeMemory);
	if (!Desired) { AddError(TEXT("localization fixture capture failed")); return false; }
	auto Records = Desired->GetArrayField(TEXT("targets"));
	const auto NewRecord = MakeShared<FJsonObject>();
	NewRecord->SetStringField(TEXT("name"), Name);
	Records.Add(MakeShared<FJsonValueObject>(NewRecord));
	Desired->SetArrayField(TEXT("targets"), Records);
	const FString DefaultCulture = FInternationalization::Get().GetCurrentCulture()->GetName();
	TWeakObjectPtr<ULocalizationTarget> Created;
	auto ObserveCreated = [&]()
	{
		for (const auto& Object : Set->TargetObjects)
			if (Object && Object->Settings.Name.Equals(Name, ESearchCase::CaseSensitive)) Created = Object.Get();
		if (!Created.IsValid()) return false;
		const auto& Value = Created->Settings;
		ScriptPaths = LocalizationConfigurationScript::GetConfigPaths(Created.Get());
		return Created->GetOuter() == Set && Value.Guid.IsValid() && Value.NativeCultureIndex == 0
			&& Value.SupportedCulturesStatistics.Num() == 1
			&& Value.SupportedCulturesStatistics[0].CultureName.Equals(DefaultCulture, ESearchCase::CaseSensitive);
	};
	auto CheckRestored = [&]()
	{
		TestFalse(TEXT("new native target has exited on rollback"), Created.IsValid());
		const auto Current = CaptureLocalizationGameSettings();
		TArray<FLocalizationTargetSettings> Backing;
		bool bSame = Set->TargetObjects == OriginalObjects && Current.Num() == BeforeMemory.Num()
			&& GetLocalizationBackingSettings(*Settings, Backing, Error) && Backing.Num() == BeforeMemory.Num();
		if (bSame) for (int32 Index = 0; Index < Current.Num(); ++Index)
			bSame = bSame && FLocalizationTargetSettings::StaticStruct()->CompareScriptStruct(&Current[Index], &BeforeMemory[Index], PPF_None)
				&& FLocalizationTargetSettings::StaticStruct()->CompareScriptStruct(&Backing[Index], &BeforeMemory[Index], PPF_None);
		TestTrue(TEXT("original collection identity and dirty native/backing values restored"), bSame);
		TArray<uint8> Disk;
		TestTrue(TEXT("create rollback restores different original disk bytes"), FFileHelper::LoadFileToArray(Disk, *Filename) && Disk == BeforeDisk);
		for (const auto& Path : ScriptPaths) TestFalse(TEXT("created scripts restore to absence"), FPaths::FileExists(Path));
	};
	bool bEarlyObserved = false;
	FLocalizationSettingsStage RejectStage = [&](TFunctionRef<bool(FOperationFailure&)> NativeStage, FOperationFailure& Failure)
	{
		if (!NativeStage(Failure)) return false;
		TArray<uint8> Disk;
		bEarlyObserved = ObserveCreated() && FFileHelper::LoadFileToArray(Disk, *Filename) && Disk == BeforeDisk;
		for (const auto& Path : ScriptPaths) bEarlyObserved = bEarlyObserved && !FPaths::FileExists(Path);
		Failure.ErrorCode = TEXT("t208_create_early_rejection");
		Failure.ErrorDetail = Failure.ErrorCode;
		return false;
	};
	HjsonWorkspace::DomainAuthoring::FRootPropertyApplyOutcome Early;
	TestFalse(TEXT("created target early rejection fails"), ApplyLocalizationSettings(TEXT("t208-settings-create-early"), Desired.ToSharedRef(), Early, RejectStage));
	TestTrue(TEXT("early observes created default-culture object after real staging before publication"), bEarlyObserved);
	TestTrue(TEXT("create early rollback is confirmed"), Early.ErrorDetail.Contains(TEXT("rollback=semantically_confirmed")));
	CheckRestored();
	if (HasAnyErrors()) return false;
	Created.Reset();
	bool bLateObserved = false;
	HjsonWorkspace::DomainAuthoring::FRootPropertyApplyOutcome Late;
	Late.FinalReadback = [&](FOperationFailure& Failure)
	{
		bLateObserved = ObserveCreated();
		TArray<FLocalizationTargetSettings> Disk;
		FString ReadError;
		bLateObserved = bLateObserved && ReadLocalizationSettingsFile(Filename, Disk, ReadError);
		const auto* Found = Disk.FindByPredicate([&](const auto& Item) { return Item.Name.Equals(Name, ESearchCase::CaseSensitive); });
		bLateObserved = bLateObserved && Found && Found->Guid == Created->Settings.Guid
			&& Found->NativeCultureIndex == 0 && Found->SupportedCulturesStatistics.Num() == 1
			&& Found->SupportedCulturesStatistics[0].CultureName.Equals(DefaultCulture, ESearchCase::CaseSensitive);
		for (const auto& Path : ScriptPaths) bLateObserved = bLateObserved && FPaths::FileExists(Path);
		Failure.ErrorCode = TEXT("t208_create_late_rejection");
		Failure.ErrorDetail = Failure.ErrorCode;
		return false;
	};
	TestFalse(TEXT("created target late rejection fails"), ApplyLocalizationSettings(TEXT("t208-settings-create-late"), Desired.ToSharedRef(), Late));
	TestTrue(TEXT("late observes actual created target and published config/scripts"), bLateObserved);
	TestTrue(TEXT("create late rollback is confirmed"), Late.ErrorDetail.Contains(TEXT("rollback=semantically_confirmed")));
	CheckRestored();
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208LocalizationSettingsOwnedLifecycleTest,
	"UAI.Root.T208.LocalizationSettings.OwnedLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208LocalizationSettingsOwnedLifecycleTest::RunTest(const FString&)
{
	using namespace UE::UAI::DomainAdapters::LocalizationStringTable;
	using namespace UeAgentAuthoringWorkspaceTransaction;
	auto* Settings = GetMutableDefault<ULocalizationSettings>();
	auto* Set = ULocalizationSettings::GetGameTargetSet();
	if (!Settings || !Set) return false;
	const FString Filename = Settings->GetDefaultConfigFilename();
	const bool bOriginalFile = FPaths::FileExists(Filename);
	TArray<uint8> OriginalBytes;
	if (bOriginalFile && !FFileHelper::LoadFileToArray(OriginalBytes, *Filename)) return false;
	const auto OriginalObjects = Set->TargetObjects;
	const auto OriginalNative = CaptureLocalizationGameSettings();
	if (OriginalObjects.Num() != OriginalNative.Num()) return false;
	TArray<FLocalizationTargetSettings> OriginalBacking;
	FString Error;
	if (!GetLocalizationBackingSettings(*Settings, OriginalBacking, Error)) { AddError(Error); return false; }
	const FString WorkRoot = FPaths::ProjectSavedDir() / TEXT("UeAssetFolders/T208LocalizationSettingsLifecycle");
	IFileManager::Get().MakeDirectory(*WorkRoot, true);
	TArray<FString> ScriptPaths;
	ON_SCOPE_EXIT
	{
		Set->TargetObjects = OriginalObjects;
		for (int32 Index = 0; Index < OriginalObjects.Num(); ++Index) OriginalObjects[Index]->Settings = OriginalNative[Index];
		FString RestoreError;
		if (!SetLocalizationBackingSettings(*Settings, OriginalBacking, RestoreError)) AddError(RestoreError);
		if (bOriginalFile)
		{
			TArray<uint8> Restored;
			if (!FFileHelper::SaveArrayToFile(OriginalBytes, *Filename)
				|| !FFileHelper::LoadFileToArray(Restored, *Filename) || Restored != OriginalBytes) AddError(TEXT("original config byte restoration failed"));
		}
		else if (!IFileManager::Get().Delete(*Filename, false, true) || FPaths::FileExists(Filename)) AddError(TEXT("original absent config restoration failed"));
		for (const auto& Path : ScriptPaths)
			if (!IFileManager::Get().Delete(*Path, false, true) || FPaths::FileExists(Path)) AddError(TEXT("owned lifecycle script cleanup failed"));
		IFileManager::Get().DeleteDirectory(*WorkRoot, false, true);
	};
	ULocalizationTarget* Target = NewObject<ULocalizationTarget>(Set);
	Target->Settings.Name = TEXT("T208Lifecycle_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	Target->Settings.RequiredModuleNames = {TEXT("UeAgentInterface")};
	FCultureStatistics English;
	English.CultureName = TEXT("en");
	Target->Settings.SupportedCulturesStatistics = {English};
	Target->Settings.NativeCultureIndex = 0;
	ScriptPaths = LocalizationConfigurationScript::GetConfigPaths(Target);
	for (const auto& Path : ScriptPaths) if (FPaths::FileExists(Path)) { ScriptPaths.Reset(); AddError(TEXT("owned script collision")); return false; }
	Set->TargetObjects.Add(Target);
	const auto DiskState = CaptureLocalizationGameSettings();
	const FString SeedPath = WorkRoot / TEXT("seed.ini");
	if (!WriteLocalizationSettingsStage(DiskState, SeedPath, Error)) { AddError(Error); return false; }
	TArray<uint8> BeforeDisk;
	if (!FFileHelper::LoadFileToArray(BeforeDisk, *SeedPath) || !FFileHelper::SaveArrayToFile(BeforeDisk, *Filename)) return false;
	FCultureStatistics German;
	German.CultureName = TEXT("de");
	German.WordCount = 37;
	Target->Settings.SupportedCulturesStatistics.Add(German);
	Target->Settings.ConflictStatus = ELocalizationTargetConflictStatus::ConflictsPresent;
	const auto BeforeMemory = CaptureLocalizationGameSettings();
	if (!SetLocalizationBackingSettings(*Settings, BeforeMemory, Error)) { AddError(Error); return false; }
	auto Desired = CaptureLocalizationSettingsState(BeforeMemory);
	if (!Desired) { AddError(TEXT("localization fixture capture failed")); return false; }
	TSharedPtr<FJsonObject> Record;
	for (const auto& Item : Desired->GetArrayField(TEXT("targets")))
		if (Item->AsObject()->GetStringField(TEXT("name")).Equals(Target->Settings.Name, ESearchCase::CaseSensitive)) Record = Item->AsObject();
	if (!Record) return false;
	Record->SetArrayField(TEXT("cultures"), {MakeShared<FJsonValueString>(TEXT("en")), MakeShared<FJsonValueBoolean>(true)});
	auto CheckRestored = [&]()
	{
		const auto Current = CaptureLocalizationGameSettings();
		TArray<FLocalizationTargetSettings> Backing;
		bool bSame = Current.Num() == BeforeMemory.Num()
			&& GetLocalizationBackingSettings(*Settings, Backing, Error) && Backing.Num() == BeforeMemory.Num();
		if (bSame) for (int32 Index = 0; Index < Current.Num(); ++Index)
			bSame = bSame && FLocalizationTargetSettings::StaticStruct()->CompareScriptStruct(&Current[Index], &BeforeMemory[Index], PPF_None)
				&& FLocalizationTargetSettings::StaticStruct()->CompareScriptStruct(&Backing[Index], &BeforeMemory[Index], PPF_None);
		TestTrue(TEXT("complete dirty native and backing state restored"), bSame);
		TArray<uint8> Disk;
		TestTrue(TEXT("original disk restored separately from dirty native"), FFileHelper::LoadFileToArray(Disk, *Filename) && Disk == BeforeDisk);
		for (const auto& Path : ScriptPaths) TestFalse(TEXT("originally absent script restored absent"), FPaths::FileExists(Path));
	};
	HjsonWorkspace::DomainAuthoring::FRootPropertyApplyOutcome Rejected;
	TestFalse(TEXT("invalid culture rejects before native writing"), ApplyLocalizationSettings(TEXT("t208-settings-invalid"), Desired.ToSharedRef(), Rejected));
	TestEqual(TEXT("precise preflight cause survives"), Rejected.ErrorCode, FString(TEXT("localization_target_cultures_invalid")));
	CheckRestored();
	if (HasAnyErrors()) return false;
	Record->SetArrayField(TEXT("cultures"), {MakeShared<FJsonValueString>(TEXT("en")), MakeShared<FJsonValueString>(TEXT("fr"))});
	bool bEarlyObserved = false;
	FLocalizationSettingsStage RejectStaged = [&](TFunctionRef<bool(FOperationFailure&)> NativeStage, FOperationFailure& Failure)
	{
		if (!NativeStage(Failure)) return false;
		TArray<uint8> CurrentDisk;
		bEarlyObserved = Target->Settings.SupportedCulturesStatistics.Num() == 2
			&& Target->Settings.SupportedCulturesStatistics[1].CultureName.Equals(TEXT("fr"), ESearchCase::CaseSensitive)
			&& FFileHelper::LoadFileToArray(CurrentDisk, *Filename) && CurrentDisk == BeforeDisk;
		for (const auto& Path : ScriptPaths) bEarlyObserved = bEarlyObserved && !FPaths::FileExists(Path);
		Failure.ErrorCode = TEXT("t208_settings_stage_rejection");
		Failure.ErrorDetail = Failure.ErrorCode;
		return false;
	};
	HjsonWorkspace::DomainAuthoring::FRootPropertyApplyOutcome Early;
	TestFalse(TEXT("post-stage rejection fails apply"), ApplyLocalizationSettings(TEXT("t208-settings-early"), Desired.ToSharedRef(), Early, RejectStaged));
	TestTrue(TEXT("early callback observes changed native and unchanged final disk after native staging"), bEarlyObserved);
	TestTrue(TEXT("early rollback semantically confirmed"), Early.ErrorDetail.Contains(TEXT("rollback=semantically_confirmed")));
	CheckRestored();
	if (HasAnyErrors()) return false;
	bool bLateObserved = false;
	HjsonWorkspace::DomainAuthoring::FRootPropertyApplyOutcome Late;
	Late.FinalReadback = [&](FOperationFailure& Failure)
	{
		TArray<FLocalizationTargetSettings> Disk;
		FString ReadError;
		bool bPublished = ReadLocalizationSettingsFile(Filename, Disk, ReadError);
		const auto* Found = Disk.FindByPredicate([&](const auto& Item) { return Item.Name.Equals(Target->Settings.Name, ESearchCase::CaseSensitive); });
		bPublished = bPublished && Found && Found->SupportedCulturesStatistics.Num() == 2
			&& Found->SupportedCulturesStatistics[1].CultureName.Equals(TEXT("fr"), ESearchCase::CaseSensitive)
			&& Target->Settings.SupportedCulturesStatistics.Num() == 2
			&& Target->Settings.SupportedCulturesStatistics[1].CultureName.Equals(TEXT("fr"), ESearchCase::CaseSensitive);
		for (const auto& Path : ScriptPaths) bPublished = bPublished && FPaths::FileExists(Path);
		bLateObserved = bPublished;
		Failure.ErrorCode = TEXT("t208_settings_late_rejection");
		Failure.ErrorDetail = Failure.ErrorCode;
		return false;
	};
	TestFalse(TEXT("late final readback rejection fails apply"), ApplyLocalizationSettings(TEXT("t208-settings-late"), Desired.ToSharedRef(), Late));
	TestTrue(TEXT("late callback observes actual native and published files"), bLateObserved);
	TestTrue(TEXT("late rollback semantically confirmed"), Late.ErrorDetail.Contains(TEXT("rollback=semantically_confirmed")));
	CheckRestored();
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208StringTableOwnedLifecycleTest,
	"UAI.Root.T208.StringTable.OwnedLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208StringTableOwnedLifecycleTest::RunTest(const FString&)
{
	using namespace UE::UAI::DomainAdapters::LocalizationStringTable;
	using namespace UeAgentAuthoringOperationApply;
	FOwnedTable Fixture(*this);
	UStringTable* Table = Fixture.Create();
	if (!TestNotNull(TEXT("owned StringTable lifecycle fixture"), Table)) return false;
	TArray<uint8> OriginalDisk;
	if (!FFileHelper::LoadFileToArray(OriginalDisk, *Fixture.Filename)) return false;
	Table->GetMutableStringTable()->SetNamespace(TEXT("dirty memory"));
	Table->GetMutableStringTable()->SetSourceString(TEXT("Keep"), TEXT("dirty source"));
	Table->GetMutableStringTable()->SetMetaData(TEXT("Keep"), TEXT("owner"), TEXT("dirty metadata"));
	Table->MarkPackageDirty();
	const auto Desired = CaptureStringTableState(*Table->GetStringTable());
	Desired->SetStringField(TEXT("namespace"), TEXT("applied"));
	auto Entries = Desired->GetArrayField(TEXT("entries"));
	FindRecord(Entries, TEXT("Keep"))->SetStringField(TEXT("source"), TEXT("applied source"));
	FindRecord(Entries, TEXT("Keep"))->GetObjectField(TEXT("metadata"))->SetStringField(TEXT("owner"), TEXT("applied metadata"));
	const auto Added = MakeShared<FJsonObject>();
	Added->SetStringField(TEXT("key"), TEXT("New"));
	Added->SetStringField(TEXT("source"), TEXT("new source"));
	Entries.Add(MakeShared<FJsonValueObject>(Added));
	Entries.RemoveAll([](const auto& Entry) { return Entry->AsObject()->GetStringField(TEXT("key")).Equals(TEXT("Removed"), ESearchCase::CaseSensitive); });
	Desired->SetArrayField(TEXT("entries"), Entries);
	const auto Invalid = CaptureStringTableState(*Table->GetStringTable());
	Invalid->SetStringField(TEXT("namespace"), TEXT("must not leak"));
	const auto InvalidMetadata = MakeShared<FJsonObject>();
	InvalidMetadata->SetStringField(TEXT("owner"), TEXT("one"));
	InvalidMetadata->SetBoolField(TEXT("invalid_value"), true);
	FindRecord(Invalid->GetArrayField(TEXT("entries")), TEXT("Keep"))->SetObjectField(TEXT("metadata"), InvalidMetadata);
	HjsonWorkspace::DomainAuthoring::FRootPropertyApplyOutcome Rejected;
	TestFalse(TEXT("invalid native metadata rejects before writing"),
		ApplyStringTableAuthoring(Fixture.Path, TEXT("t208-stringtable-prepare"), Invalid, Rejected));
	TestEqual(TEXT("invalid metadata has domain error"), Rejected.ErrorCode, FString(TEXT("string_table_metadata_invalid")));
	TestEqual(TEXT("prepare keeps native namespace"), Table->GetStringTable()->GetNamespace(), FString(TEXT("dirty memory")));
	TestEqual(TEXT("prepare keeps native metadata"), Table->GetStringTable()->GetMetaData(TEXT("Keep"), TEXT("owner")),
		FString(TEXT("dirty metadata")));
	if (HasAnyErrors()) return false;

	class FRejectChangedStage final : public IDurablePackageStager
	{
	public:
		explicit FRejectChangedStage(FString InPath) : Path(MoveTemp(InPath)), Inner(CreateDefaultDurablePackageStager()) {}
		bool BuildArtifactInventory(const TArray<FPackageDurablePlan>& Plans,
			TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>& Artifacts, FOperationFailure& Failure) override
		{ return Inner->BuildArtifactInventory(Plans, Artifacts, Failure); }
		bool StagePackages(const TArray<FPackageDurablePlan>& Plans,
			TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>& Artifacts, FOperationFailure& Failure) override
		{
			const UStringTable* Table = FindObject<UStringTable>(nullptr, *Path);
			FString Source;
			if (Table && Table->GetStringTable()->GetNamespace() == TEXT("applied")
				&& Table->GetStringTable()->GetSourceString(TEXT("Keep"), Source) && Source == TEXT("applied source")
				&& Table->GetStringTable()->GetMetaData(TEXT("Keep"), TEXT("owner")) == TEXT("applied metadata"))
			{
				bObservedWrite = true;
				Failure.ErrorCode = TEXT("t208_string_table_stage_rejected_after_write");
				Failure.ErrorDetail = Failure.ErrorCode;
				return false;
			}
			return Inner->StagePackages(Plans, Artifacts, Failure);
		}
		bool bObservedWrite = false;
	private:
		FString Path;
		TSharedRef<IDurablePackageStager> Inner;
	};
	const auto Stager = MakeShared<FRejectChangedStage>(Fixture.Path);
	FBackendOptions Options;
	Options.PackageStager = Stager;
	HjsonWorkspace::DomainAuthoring::FRootPropertyApplyOutcome Early;
	Early.FinalReadback = [](FOperationFailure&) { return true; };
	TestFalse(TEXT("early failure after actual writes rejects"),
		ApplyStringTableAuthoring(Fixture.Path, TEXT("t208-stringtable-early"), Desired, Early, &Options));
	if (!TestTrue(TEXT("stager observes actual writes before rejection"), Stager->bObservedWrite))
	{
		AddError(Early.ErrorCode + TEXT(":") + Early.ErrorDetail);
		return false;
	}
	auto VerifyRestored = [&]()
	{
		UStringTable* Restored = LoadObject<UStringTable>(nullptr, *Fixture.Path);
		if (!TestNotNull(TEXT("restored target exists"), Restored)) return false;
		const auto Native = Restored->GetStringTable();
		FString Source;
		TestEqual(TEXT("dirty namespace restored"), Native->GetNamespace(), FString(TEXT("dirty memory")));
		TestTrue(TEXT("dirty source restored"), Native->GetSourceString(TEXT("Keep"), Source) && Source == TEXT("dirty source"));
		TestEqual(TEXT("dirty metadata restored"), Native->GetMetaData(TEXT("Keep"), TEXT("owner")), FString(TEXT("dirty metadata")));
		TestTrue(TEXT("removed entry restored"), Native->GetSourceString(TEXT("Removed"), Source));
		TestFalse(TEXT("new entry removed by rollback"), Native->GetSourceString(TEXT("New"), Source));
		TestTrue(TEXT("dirty flag restored"), Restored->GetOutermost()->IsDirty());
		TArray<uint8> Disk;
		return TestTrue(TEXT("original disk remains distinct from dirty memory"),
			FFileHelper::LoadFileToArray(Disk, *Fixture.Filename) && Disk == OriginalDisk);
	};
	if (!VerifyRestored()) return false;
	TWeakObjectPtr<UStringTable> BeforeLate(LoadObject<UStringTable>(nullptr, *Fixture.Path));
	bool bLateObserved = false;
	HjsonWorkspace::DomainAuthoring::FRootPropertyApplyOutcome Late;
	Late.FinalReadback = [&](FOperationFailure& Failure)
	{
		const UStringTable* Reloaded = LoadObject<UStringTable>(nullptr, *Fixture.Path);
		FString Source;
		bLateObserved = !BeforeLate.IsValid() && Reloaded && Reloaded->GetStringTable()->GetNamespace() == TEXT("applied")
			&& Reloaded->GetStringTable()->GetSourceString(TEXT("Keep"), Source) && Source == TEXT("applied source")
			&& Reloaded->GetStringTable()->GetMetaData(TEXT("Keep"), TEXT("owner")) == TEXT("applied metadata");
		Failure.ErrorCode = TEXT("t208_string_table_late_rejected_after_readback");
		Failure.ErrorDetail = Failure.ErrorCode;
		return false;
	};
	Table = nullptr;
	TestFalse(TEXT("late persisted verification failure rejects"),
		ApplyStringTableAuthoring(Fixture.Path, TEXT("t208-stringtable-late"), Desired, Late));
	if (!TestTrue(TEXT("late rejection follows new-object native readback"), bLateObserved))
	{
		AddError(Late.ErrorCode + TEXT(":") + Late.ErrorDetail);
		return false;
	}
	return VerifyRestored();
}
