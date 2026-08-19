#include "Misc/AutomationTest.h"

#if __has_include("DomainAdapters/ExternalImport/Localization/UeAgentLocalizationImportProvider.h") \
	&& __has_include("DomainAdapters/LocalizationStringTable/UeAgentLocalizationStringTableHjsonProvider.h") \
	&& __has_include("OperationOwners/ExternalImport/Localization/UeAgentLocalizationImportCommandService.h") \
	&& __has_include("Module/CommandRegistration/ExternalImport/Localization/UeAgentLocalizationImportRegistration.h")
#define UAI_T30_EXTERNAL_LOCALIZATION_IMPORT_AVAILABLE 1
#include "AssetRegistry/AssetRegistryModule.h"
#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "DomainAdapters/ContentGraphRead/UeAgentExternalLocalizationContentGraphProvider.h"
#include "DomainAdapters/LocalizationStringTable/UeAgentLocalizationStringTableHjsonProvider.h"
#include "HAL/FileManager.h"
#include "LocalizationSettings.h"
#include "LocalizationTargetTypes.h"
#include "Internationalization/StringTable.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "Module/CommandRegistration/ExternalImport/Localization/UeAgentLocalizationImportRegistration.h"
#include "OperationOwners/ExternalImport/Localization/UeAgentLocalizationImportCommandService.h"
#include "OperationOwners/LocalizationStringTable/UeAgentLocalizationStringTableDomainService.h"
#else
#define UAI_T30_EXTERNAL_LOCALIZATION_IMPORT_AVAILABLE 0
#endif

#if UAI_T30_EXTERNAL_LOCALIZATION_IMPORT_AVAILABLE
namespace
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandRegistry;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Module::CommandRegistration::ExternalImport::Localization;
	using namespace UE::UAI::OperationOwners::ExternalImport::Localization;

	const FString ExpectedCommandId = TEXT("external.localization.po.import");
	const FString EvidenceTargetName = TEXT("StrictV2T30EvidenceTarget");
	const FString EvidenceCulture = TEXT("fr");
	const FString HjsonIntentTargetName = TEXT("StrictV2T30HjsonIntentTarget");
	const FString HjsonAtomicTargetA = TEXT("StrictV2T30HjsonAtomicA");
	const FString HjsonAtomicTargetB = TEXT("StrictV2T30HjsonAtomicB");
	ULocalizationTarget* FindTargetByName(const FString& TargetName)
	{
		ULocalizationTargetSet* const TargetSet =
			ULocalizationSettings::GetGameTargetSet();
		if (!TargetSet)
		{
			return nullptr;
		}
		const TObjectPtr<ULocalizationTarget>* const Match =
			TargetSet->TargetObjects.FindByPredicate(
				[&TargetName](const ULocalizationTarget* Candidate)
				{
					return Candidate
						&& Candidate->Settings.Name == TargetName;
				});
		return Match ? Match->Get() : nullptr;
	}

	FString LocalizationConfigPath(const FString& TargetName)
	{
		return FPaths::ProjectConfigDir()
			/ TEXT("Localization") / (TargetName + TEXT(".ini"));
	}

	FString LocalizationContentPath(const FString& TargetName)
	{
		return FPaths::ProjectContentDir()
			/ TEXT("Localization") / TargetName;
	}

	void RemoveLocalizationTargetFixture(const FString& TargetName)
	{
		if (ULocalizationTargetSet* const TargetSet =
			ULocalizationSettings::GetGameTargetSet())
		{
			TargetSet->TargetObjects.RemoveAll(
				[&TargetName](const ULocalizationTarget* Candidate)
				{
					return Candidate
						&& Candidate->Settings.Name == TargetName;
				});
		}
		IFileManager::Get().Delete(
			*LocalizationConfigPath(TargetName),
			false,
			true,
			true);
		IFileManager::Get().DeleteDirectory(
			*LocalizationConfigPath(TargetName),
			false,
			true);
		IFileManager::Get().DeleteDirectory(
			*LocalizationContentPath(TargetName),
			false,
			true);
	}

	void RemoveHjsonIntentFixture()
	{
		RemoveLocalizationTargetFixture(HjsonIntentTargetName);
		RemoveLocalizationTargetFixture(HjsonAtomicTargetA);
		RemoveLocalizationTargetFixture(HjsonAtomicTargetB);
	}

	bool TryGetLocalizationRevision(FString& OutRevision, FString& OutError)
	{
		using namespace UE::UAI::DomainAdapters::LocalizationStringTable;
		using namespace UE::UAI::HjsonWorkspace;
		const FString Target =
			GetMutableDefault<ULocalizationSettings>()->GetPathName();
		const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), Target);
		FString CanonicalParams;
		if (!Json::TryCanonicalize(Params, CanonicalParams, OutError))
		{
			return false;
		}
		FString Payload;
		bool bReadOnly = false;
		FWorkspaceError WorkspaceError;
		if (!TryExecuteContentGraph(
				TEXT("content_graph.summary"),
				Target,
				ULocalizationSettings::StaticClass()->GetPathName(),
				CanonicalParams,
				Payload,
				bReadOnly,
				WorkspaceError))
		{
			OutError = WorkspaceError.GetCode();
			return false;
		}
		TSharedPtr<FJsonObject> Result;
		if (!Json::TryParseObject(Payload, Result, OutError)
			|| !Result.IsValid()
			|| !Result->TryGetStringField(TEXT("revision"), OutRevision))
		{
			return false;
		}
		return bReadOnly && !OutRevision.IsEmpty();
	}

	bool TryImportLocalizationTargets(
		const FString& ExpectedRevision,
		const TArray<TSharedPtr<FJsonValue>>& Targets,
		FString& OutPayload,
		UE::UAI::HjsonWorkspace::FWorkspaceError& OutError)
	{
		using namespace UE::UAI::DomainAdapters::LocalizationStringTable;
		const FString Target =
			GetMutableDefault<ULocalizationSettings>()->GetPathName();
		const TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(
			TEXT("type"),
			ULocalizationSettings::StaticClass()->GetPathName());
		Root->SetArrayField(TEXT("targets"), Targets);
		const TSharedPtr<FJsonObject> Document = MakeShared<FJsonObject>();
		Document->SetStringField(TEXT("schema"), TEXT("uai.localization.edit/1"));
		Document->SetObjectField(TEXT("root"), Root);
		FString DocumentText;
		FString JsonError;
		if (!Json::TryCanonicalize(Document, DocumentText, JsonError))
		{
			OutError.Set(TEXT("fixture_document_invalid"), JsonError);
			return false;
		}
		const TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("path"), TEXT("localization.hjson"));
		Entry->SetStringField(TEXT("document"), DocumentText);
		const TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("kind"), TEXT("resolved"));
		Source->SetStringField(TEXT("target_identity"), Target);
		Source->SetStringField(TEXT("entry_file"), TEXT("localization.hjson"));
		Source->SetArrayField(
			TEXT("documents"),
			{MakeShared<FJsonValueObject>(Entry)});
		const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), Target);
		Params->SetStringField(TEXT("expected_revision"), ExpectedRevision);
		Params->SetObjectField(TEXT("source"), Source);
		FString CanonicalParams;
		if (!Json::TryCanonicalize(Params, CanonicalParams, JsonError))
		{
			OutError.Set(TEXT("fixture_params_invalid"), JsonError);
			return false;
		}
		bool bReadOnly = true;
		return TryExecuteContentGraph(
			TEXT("content_graph.import_workspace"),
			Target,
			ULocalizationSettings::StaticClass()->GetPathName(),
			CanonicalParams,
			OutPayload,
			bReadOnly,
			OutError)
			&& !bReadOnly;
	}

	bool TryImportStringTable(
		const FString& Target,
		FString& OutPayload,
		UE::UAI::HjsonWorkspace::FWorkspaceError& OutError)
	{
		using namespace UE::UAI::DomainAdapters::LocalizationStringTable;
		const TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("key"), TEXT("hello"));
		Entry->SetStringField(TEXT("source"), TEXT("Hello"));
		const TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("type"), UStringTable::StaticClass()->GetPathName());
		Root->SetStringField(TEXT("namespace"), TEXT("T30Rollback"));
		Root->SetArrayField(
			TEXT("entries"),
			{MakeShared<FJsonValueObject>(Entry)});
		const TSharedPtr<FJsonObject> Document = MakeShared<FJsonObject>();
		Document->SetStringField(TEXT("schema"), TEXT("uai.string_table.edit/1"));
		Document->SetObjectField(TEXT("root"), Root);
		FString DocumentText;
		FString JsonError;
		if (!Json::TryCanonicalize(Document, DocumentText, JsonError))
		{
			OutError.Set(TEXT("fixture_document_invalid"), JsonError);
			return false;
		}
		const TSharedPtr<FJsonObject> SourceDocument = MakeShared<FJsonObject>();
		SourceDocument->SetStringField(TEXT("path"), TEXT("string_table.hjson"));
		SourceDocument->SetStringField(TEXT("document"), DocumentText);
		const TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("kind"), TEXT("resolved"));
		Source->SetStringField(TEXT("target_identity"), Target);
		Source->SetStringField(TEXT("entry_file"), TEXT("string_table.hjson"));
		Source->SetArrayField(
			TEXT("documents"),
			{MakeShared<FJsonValueObject>(SourceDocument)});
		const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), Target);
		Params->SetStringField(TEXT("expected_revision"), TEXT("absent"));
		Params->SetStringField(TEXT("authorization_id"), TEXT("auth:t30-string-table"));
		Params->SetObjectField(TEXT("source"), Source);
		FString CanonicalParams;
		if (!Json::TryCanonicalize(Params, CanonicalParams, JsonError))
		{
			OutError.Set(TEXT("fixture_params_invalid"), JsonError);
			return false;
		}
		bool bReadOnly = true;
		return TryExecuteContentGraph(
			TEXT("content_graph.import_workspace"),
			Target,
			UStringTable::StaticClass()->GetPathName(),
			CanonicalParams,
			OutPayload,
			bReadOnly,
			OutError);
	}

	FString EvidenceSourceRoot()
	{
		return FPaths::ProjectSavedDir()
			/ TEXT("UAI/StrictV2/T30LocalizationFixture");
	}

	FString EvidenceSourcePo()
	{
		return EvidenceSourceRoot() / TEXT("strict-v2-t30-fr.po");
	}

	FString EvidenceTargetRoot()
	{
		return FPaths::ProjectContentDir()
			/ TEXT("Localization") / EvidenceTargetName;
	}

	ULocalizationTarget* FindEvidenceTarget()
	{
		ULocalizationTargetSet* const TargetSet =
			ULocalizationSettings::GetGameTargetSet();
		if (!TargetSet)
		{
			return nullptr;
		}
		const TObjectPtr<ULocalizationTarget>* const Match =
			TargetSet->TargetObjects.FindByPredicate(
				[](const ULocalizationTarget* Candidate)
				{
					return Candidate
						&& Candidate->Settings.Name == EvidenceTargetName;
				});
		return Match ? Match->Get() : nullptr;
	}

	bool RemoveEvidenceFixture(FString& OutError)
	{
		OutError.Reset();
		ULocalizationTargetSet* const TargetSet =
			ULocalizationSettings::GetGameTargetSet();
		if (TargetSet)
		{
			TargetSet->TargetObjects.RemoveAll(
				[](const ULocalizationTarget* Candidate)
				{
					return Candidate
						&& Candidate->Settings.Name == EvidenceTargetName;
				});
		}
		IFileManager& Files = IFileManager::Get();
		const bool bTargetRemoved =
			!Files.DirectoryExists(*EvidenceTargetRoot())
			|| Files.DeleteDirectory(*EvidenceTargetRoot(), false, true);
		const bool bSourceRemoved =
			!Files.DirectoryExists(*EvidenceSourceRoot())
			|| Files.DeleteDirectory(*EvidenceSourceRoot(), false, true);
		const FString ConfigRoot = FPaths::ProjectConfigDir()
			/ TEXT("Localization");
		TArray<FString> ConfigNames;
		Files.FindFiles(
			ConfigNames,
			*(ConfigRoot / (EvidenceTargetName + TEXT("*.ini"))),
			true,
			false);
		bool bConfigsRemoved = true;
		for (const FString& ConfigName : ConfigNames)
		{
			bConfigsRemoved &= Files.Delete(
				*(ConfigRoot / ConfigName),
				false,
				true,
				true);
		}
		if (!bTargetRemoved || !bSourceRemoved || !bConfigsRemoved)
		{
			OutError = TEXT("T30 fixture cleanup could not remove all owned files");
			return false;
		}
		return true;
	}

	bool CreateEvidenceFixture(FString& OutError)
	{
		OutError.Reset();
		if (FindEvidenceTarget())
		{
			OutError = TEXT("T30 evidence target already exists");
			return false;
		}
		IFileManager& Files = IFileManager::Get();
		if (Files.DirectoryExists(*EvidenceTargetRoot())
			|| Files.DirectoryExists(*EvidenceSourceRoot()))
		{
			OutError = TEXT("T30 evidence fixture paths already exist");
			return false;
		}
		ULocalizationTargetSet* const TargetSet =
			ULocalizationSettings::GetGameTargetSet();
		if (!TargetSet)
		{
			OutError = TEXT("project localization target set is unavailable");
			return false;
		}
		ULocalizationTarget* const Target =
			NewObject<ULocalizationTarget>(TargetSet);
		if (!Target)
		{
			OutError = TEXT("could not allocate T30 evidence target");
			return false;
		}
		Target->Settings.Name = EvidenceTargetName;
		Target->Settings.SupportedCulturesStatistics = {
			FCultureStatistics(TEXT("en")),
			FCultureStatistics(EvidenceCulture),
		};
		Target->Settings.NativeCultureIndex = 0;
		TargetSet->TargetObjects.Add(Target);

		const FString Manifest = TEXT(
			"{\n"
			"  \"FormatVersion\": 1,\n"
			"  \"Namespace\": \"\",\n"
			"  \"Subnamespaces\": [{\n"
			"    \"Namespace\": \"StrictV2T30\",\n"
			"    \"Children\": [{\n"
			"      \"Source\": {\"Text\": \"Hello from T30\"},\n"
			"      \"Keys\": [{\"Key\": \"EvidenceKey\", \"Path\": \"StrictV2/T30\"}]\n"
			"    }]\n"
			"  }]\n"
			"}\n");
		const FString Archive = TEXT(
			"{\n"
			"  \"FormatVersion\": 2,\n"
			"  \"Namespace\": \"\",\n"
			"  \"Subnamespaces\": [{\n"
			"    \"Namespace\": \"StrictV2T30\",\n"
			"    \"Children\": [{\n"
			"      \"Source\": {\"Text\": \"Hello from T30\"},\n"
			"      \"Translation\": {\"Text\": \"Bonjour initial\"},\n"
			"      \"Key\": \"EvidenceKey\"\n"
			"    }]\n"
			"  }]\n"
			"}\n");
		const FString Po = TEXT(
			"msgid \"\"\n"
			"msgstr \"\"\n"
			"\"Project-Id-Version: StrictV2T30Evidence\\n\"\n"
			"\"Language: fr\\n\"\n"
			"\"MIME-Version: 1.0\\n\"\n"
			"\"Content-Type: text/plain; charset=UTF-8\\n\"\n"
			"\"Content-Transfer-Encoding: 8bit\\n\"\n"
			"\n"
			"#. Key: EvidenceKey\n"
			"#. SourceLocation: StrictV2/T30\n"
			"msgctxt \"StrictV2T30,EvidenceKey\"\n"
			"msgid \"Hello from T30\"\n"
			"msgstr \"Bonjour depuis T30\"\n");
		const FString CultureRoot = EvidenceTargetRoot() / EvidenceCulture;
		Files.MakeDirectory(*CultureRoot, true);
		Files.MakeDirectory(*EvidenceSourceRoot(), true);
		const bool bSaved = FFileHelper::SaveStringToFile(
			Manifest,
			*(EvidenceTargetRoot() / (EvidenceTargetName + TEXT(".manifest"))),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
			&& FFileHelper::SaveStringToFile(
				Archive,
				*(CultureRoot / (EvidenceTargetName + TEXT(".archive"))),
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
			&& FFileHelper::SaveStringToFile(
				Po,
				*EvidenceSourcePo(),
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		if (!bSaved)
		{
			FString CleanupError;
			RemoveEvidenceFixture(CleanupError);
			OutError = TEXT("could not persist T30 evidence fixture files");
			return false;
		}
		return true;
	}

	class FNeverCancelled final : public IUeAgentCancellationSignal
	{
	public:
		virtual bool IsCancelled() const override { return false; }
	};

	class FFixtureService final : public IUeAgentLocalizationImportCommandService
	{
	public:
		virtual bool Execute(
			const FUeAgentLocalizationImportCommandRequest& Request,
			FUeAgentLocalizationImportCommandResponse& OutResponse,
			FUeAgentLocalizationImportCommandError& OutError) override
		{
			++Calls;
			LastRequest = Request;
			OutResponse = {};
			OutError = {};
			if (bFail)
			{
				OutError.Code = TEXT("fixture_t30_failed");
				OutError.SubjectId = Request.CommandId;
				OutError.Detail = TEXT("fixture failure remains typed");
				OutError.TerminalStatus = TEXT("rejected");
				return false;
			}
			OutResponse.PayloadJson =
				TEXT("{\"command_id\":\"external.localization.po.import\",")
				TEXT("\"independent_readback\":{},\"mutation_started\":true,")
				TEXT("\"persistence_receipt_id\":\"fixture\",")
				TEXT("\"phase_history\":[\"planning\",\"terminal\"],")
				TEXT("\"terminal_status\":\"succeeded\"}");
			return true;
		}

		int32 Calls = 0;
		bool bFail = false;
		FUeAgentLocalizationImportCommandRequest LastRequest;
	};

	bool DecodeRequest(
		const FString& ContractHash,
		const FString& ParamsJson,
		TOptional<FUeAgentCommandRequest>& OutRequest)
	{
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"corr-t30\",")
			TEXT("\"request_id\":\"req-t30\",")
			TEXT("\"command_id\":\"%s\",")
			TEXT("\"command_binding_hash\":\"%s\",\"params\":%s}"),
			*ExpectedCommandId,
			*ContractHash,
			*ParamsJson);
		FTCHARToUTF8 Encoded(*Envelope);
		TArray<uint8> Bytes;
		Bytes.Append(
			reinterpret_cast<const uint8*>(Encoded.Get()),
			Encoded.Length());
		FUeAgentCommandProtocolDiagnostic Diagnostic;
		return FUeAgentCommandProtocolCodec().DecodeRequest(
			Bytes,
			OutRequest,
			Diagnostic);
	}

	FUeAgentCommandResult Submit(
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		const FString& ParamsJson,
		TArray<FString> Authorities = {TEXT("uai:operate")})
	{
		const auto* Contract = Registry.IsValid()
			? Registry->FindExact(ExpectedCommandId)
			: nullptr;
		if (!Contract || !Dispatch.IsValid())
		{
			return FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t30"), TEXT("req-t30"), ExpectedCommandId,
				TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
				TEXT("fixture_contract_missing"),
				TEXT("T30 fixture could not find the exact contract"));
		}
		TOptional<FUeAgentCommandRequest> Request;
		if (!DecodeRequest(Contract->GetContractHash(), ParamsJson, Request)
			|| !Request.IsSet())
		{
			return FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t30"), TEXT("req-t30"), ExpectedCommandId,
				Contract->GetContractHash(),
				TEXT("fixture_request_decode_failed"),
				TEXT("T30 fixture could not decode its request"));
		}
		return Dispatch->Submit(
			Request.GetValue(),
			FUeAgentCallerContext(
				TEXT("strict-v2-t30"),
				MoveTemp(Authorities),
				TEXT("corr-t30")),
			MakeShared<FNeverCancelled>());
	}

	FString Params(const FString& Source, const FString& Target)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("source_ref"), Source);
		Json->SetStringField(TEXT("target_ref"), Target);
		FString Result;
		FString Error;
		return Json::TryCanonicalize(Json, Result, Error)
			? Result
			: FString(TEXT("{}"));
	}
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T30ReadCapabilityBoundaryTest,
	"UAI.StrictV2.T30.EXTERNAL.LOCALIZATION.IMPORT.ReadCapabilityBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T30ReadCapabilityBoundaryTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T30_EXTERNAL_LOCALIZATION_IMPORT_AVAILABLE
	TestTrue(TEXT("T30 read capability owner is implemented"), false);
	return false;
#else
	using namespace UE::UAI::DomainAdapters::ContentGraphRead;
	TestTrue(
		TEXT("localization validation is a closed compound Query action"),
		IsExternalLocalizationContentGraphRoute(
			TEXT("content_graph.read.compound"),
			TEXT("localization.validate")));
	TestFalse(
		TEXT("PO export cannot enter the read-only Query owner"),
		IsExternalLocalizationContentGraphRoute(
			TEXT("content_graph.read.compound"),
			TEXT("localization.export_po")));
	TestFalse(
		TEXT("a Localization action cannot enter a different provider"),
		IsExternalLocalizationContentGraphRoute(
			TEXT("content_graph.read.summary"),
			TEXT("localization.validate")));

	const TSharedPtr<FJsonObject> Document = MakeShared<FJsonObject>();
	Document->SetStringField(TEXT("schema"), TEXT("uai.localization.v1"));
	Document->SetArrayField(
		TEXT("targets"), TArray<TSharedPtr<FJsonValue>>());
	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetObjectField(TEXT("document"), Document);
	TSharedPtr<FJsonObject> Observation;
	FString Error;
	TestTrue(
		TEXT("valid Localization JSON is checked without mutation"),
		TryExecuteExternalLocalizationContentGraph(
			TEXT("localization.validate"),
			Input,
			Observation,
			Error));
	TestTrue(TEXT("validation returns an observation"), Observation.IsValid());
	if (Observation.IsValid())
	{
		TestTrue(TEXT("valid document is reported valid"),
			Observation->GetBoolField(TEXT("valid")));
		TestEqual(TEXT("valid document has no errors"),
			Observation->GetIntegerField(TEXT("error_count")), 0);
	}
	return !HasAnyErrors();
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T30InventoryAndLegacyExitTest,
	"UAI.StrictV2.T30.EXTERNAL.LOCALIZATION.IMPORT.InventoryAndLegacyExit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T30InventoryAndLegacyExitTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T30_EXTERNAL_LOCALIZATION_IMPORT_AVAILABLE
	TestTrue(TEXT("T30 localization import owner is implemented"), false);
	return false;
#else
	const TArray<FUeAgentLocalizationImportCommandRoute>& Routes =
		GetLocalizationImportCommandRoutes();
	TestEqual(TEXT("T30 owns exactly one audited command"), Routes.Num(), 1);
	if (Routes.Num() == 1)
	{
		TestTrue(TEXT("the T30 route is valid"), Routes[0].IsValid());
		TestEqual(TEXT("the T30 route identity is exact"), Routes[0].CommandId, ExpectedCommandId);
	}

	TSharedPtr<const FUeAgentImmutableCommandRegistry> ProcessRegistry;
	TSharedPtr<const FUeAgentExactCommandDispatch> ProcessDispatch;
	FString Error;
	TestTrue(
		TEXT("process active composition includes T30"),
		TryComposeProcessActiveCommandComposition(
			ProcessRegistry,
			ProcessDispatch,
			Error));
	const auto* Contract = ProcessRegistry.IsValid()
		? ProcessRegistry->FindExact(ExpectedCommandId)
		: nullptr;
	TestNotNull(TEXT("the T30 command has one active exact contract"), Contract);
	if (Contract)
	{
		TestEqual(
			TEXT("the active contract publishes the unique T30 owner"),
			Contract->GetPublicOwner(),
			FString(TEXT("localization-import-owner")));
	}
	return !HasAnyErrors();
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T30HjsonProviderContractTest,
	"UAI.StrictV2.T30.EXTERNAL.LOCALIZATION.IMPORT.HjsonProviderContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T30HjsonProviderContractTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T30_EXTERNAL_LOCALIZATION_IMPORT_AVAILABLE
	TestTrue(TEXT("T30 HJSON provider is implemented"), false);
	return false;
#else
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::OperationOwners::LocalizationStringTable;
	TSharedPtr<IImportInitializationDomainProvider> Provider;
	FWorkspaceError Error;
	TestTrue(
		TEXT("localization and StringTable HJSON provider composes"),
		TryCreateLocalizationStringTableDomainProvider(Provider, Error));
	TestTrue(TEXT("provider is valid"), Provider.IsValid());
	if (!Provider.IsValid())
	{
		return false;
	}
	TestEqual(
		TEXT("provider identity is stable"),
		Provider->GetProviderId(),
		FString(TEXT("localization-string-table-hjson-provider")));
	const FImportInitializationDomainRequest ImportStringTable(
		TEXT("content_graph.import_workspace"),
		TEXT("uai.string_table.edit/1"),
		TEXT("/Game/UAI/StrictV2/T30_ST"),
		UStringTable::StaticClass()->GetPathName(),
		EInitializationNodeRole::RootTarget,
		TEXT("{}"));
	TestTrue(
		TEXT("StringTable imports match the unique HJSON provider"),
		Provider->Matches(ImportStringTable));
	FString ContractId;
	TestTrue(
		TEXT("StringTable imports resolve an explicit initialization contract"),
		Provider->TryResolveInitializationContract(
			ImportStringTable, ContractId, Error));
	TestEqual(
		TEXT("StringTable initialization contract is exact"),
		ContractId,
		FString(TEXT("uai.init.localization.string_table.root/1")));
	FImportInitializationDomainResponse CrossContractResponse;
	Error.Reset();
	TestFalse(
		TEXT("a Localization contract cannot execute a StringTable request"),
		Provider->TryExecute(
			ImportStringTable,
			TEXT("uai.init.localization.settings.root/1"),
			CrossContractResponse,
			Error));
	TestEqual(
		TEXT("cross-contract execution is rejected at the provider boundary"),
		Error.GetCode(),
		FString(TEXT("localization_hjson_contract_not_executable")));
	const FString RollbackSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString RollbackStringTable = FString::Printf(
		TEXT("/Game/UAI/StrictV2/T30_StringTableRollback_%s.T30_StringTableRollback_%s"),
		*RollbackSuffix,
		*RollbackSuffix);
	const FString RollbackPackage =
		FPackageName::ObjectPathToPackageName(RollbackStringTable);
	const FString RollbackFilename = FPackageName::LongPackageNameToFilename(
		RollbackPackage,
		FPackageName::GetAssetPackageExtension());
	IFileManager::Get().Delete(*RollbackFilename, false, true, true);
	IFileManager::Get().DeleteDirectory(*RollbackFilename, false, true);
	FString RollbackPayload;
	Error.Reset();
	AddExpectedError(
		TEXT("LogStreaming: Failed to read file .*T30_StringTableRollback_[0-9A-F]+\\.uasset.*"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	UE::UAI::DomainAdapters::LocalizationStringTable::
		SetStringTableSaveFailureForTests(true);
	TestFalse(
		TEXT("the deterministic StringTable save failure rejects the import"),
		TryImportStringTable(
			RollbackStringTable,
			RollbackPayload,
			Error));
	UE::UAI::DomainAdapters::LocalizationStringTable::
		SetStringTableSaveFailureForTests(false);
	TestEqual(
		TEXT("StringTable save failure is typed"),
		Error.GetCode(),
		FString(TEXT("string_table_save_failed")));
	UStringTable* const FailedTable = FindObject<UStringTable>(
		nullptr,
		*RollbackStringTable);
	TestNull(
		TEXT("a failed StringTable import unregisters the created asset"),
		FailedTable);
	IFileManager::Get().Delete(*RollbackFilename, false, true, true);
	if (FailedTable)
	{
		FAssetRegistryModule::AssetDeleted(FailedTable);
		FailedTable->ClearFlags(RF_Public | RF_Standalone);
		FailedTable->MarkAsGarbage();
	}
	const FImportInitializationDomainRequest WrongSchema(
		TEXT("content_graph.import_workspace"),
		TEXT("uai.localization.edit/1"),
		TEXT("/Game/UAI/StrictV2/T30_ST"),
		UStringTable::StaticClass()->GetPathName(),
		EInitializationNodeRole::RootTarget,
		TEXT("{}"));
	TestFalse(
		TEXT("cross-domain HJSON schema cannot enter the provider"),
		Provider->Matches(WrongSchema));
	const FImportInitializationDomainRequest LocalizationExport(
		TEXT("content_graph.export_workspace"),
		FString(),
		GetMutableDefault<ULocalizationSettings>()->GetPathName(),
		ULocalizationSettings::StaticClass()->GetPathName(),
		EInitializationNodeRole::RootTarget,
		TEXT("{}"));
	TestTrue(
		TEXT("Localization export uses the same domain provider"),
		Provider->Matches(LocalizationExport));
	return !HasAnyErrors();
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T30LocalizationHjsonExplicitIntentTest,
	"UAI.StrictV2.T30.EXTERNAL.LOCALIZATION.IMPORT.LocalizationHjsonExplicitIntent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T30LocalizationHjsonExplicitIntentTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T30_EXTERNAL_LOCALIZATION_IMPORT_AVAILABLE
	TestTrue(TEXT("T30 Localization HJSON provider is implemented"), false);
	return false;
#else
	using namespace UE::UAI::HjsonWorkspace;
	RemoveHjsonIntentFixture();

	FString Revision;
	FString RevisionError;
	TestTrue(
		TEXT("Localization HJSON current revision is independently readable"),
		TryGetLocalizationRevision(Revision, RevisionError));
	if (Revision.IsEmpty())
	{
		AddError(RevisionError);
		return false;
	}

	FString Payload;
	FWorkspaceError WorkspaceError;
	TestFalse(
		TEXT("existing Localization settings reject the absent revision sentinel"),
		TryImportLocalizationTargets(
			TEXT("absent"),
			{},
			Payload,
			WorkspaceError));
	TestEqual(
		TEXT("absent revision rejection is typed"),
		WorkspaceError.GetCode(),
		FString(TEXT("localization_stale_revision")));

	const TSharedPtr<FJsonObject> MissingIntent = MakeShared<FJsonObject>();
	MissingIntent->SetStringField(TEXT("name"), HjsonIntentTargetName);
	MissingIntent->SetStringField(TEXT("native_culture"), TEXT("en"));
	MissingIntent->SetArrayField(
		TEXT("cultures"),
		{MakeShared<FJsonValueString>(TEXT("en"))});
	WorkspaceError.Reset();
	TestFalse(
		TEXT("Localization target mutation requires explicit intent"),
		TryImportLocalizationTargets(
			Revision,
			{MakeShared<FJsonValueObject>(MissingIntent)},
			Payload,
			WorkspaceError));
	TestEqual(
		TEXT("missing target operation is typed"),
		WorkspaceError.GetCode(),
		FString(TEXT("localization_target_operation_required")));
	RemoveHjsonIntentFixture();

	TestTrue(
		TEXT("cleanup restores the revision after the expected-red probe"),
		TryGetLocalizationRevision(Revision, RevisionError));
	const TSharedPtr<FJsonObject> Create = MakeShared<FJsonObject>();
	Create->SetStringField(TEXT("operation"), TEXT("create"));
	Create->SetStringField(TEXT("name"), HjsonIntentTargetName);
	Create->SetStringField(TEXT("native_culture"), TEXT("en"));
	Create->SetArrayField(
		TEXT("cultures"),
		{
			MakeShared<FJsonValueString>(TEXT("en")),
			MakeShared<FJsonValueString>(TEXT("fr")),
		});
	WorkspaceError.Reset();
	TestTrue(
		TEXT("explicit create persists one Localization target"),
		TryImportLocalizationTargets(
			Revision,
			{MakeShared<FJsonValueObject>(Create)},
			Payload,
			WorkspaceError));
	TestNotNull(
		TEXT("created target is independently present"),
		FindTargetByName(HjsonIntentTargetName));
	TestTrue(
		TEXT("created target config is persisted"),
		FPaths::FileExists(LocalizationConfigPath(HjsonIntentTargetName)));

	TestTrue(
		TEXT("post-create revision is independently readable"),
		TryGetLocalizationRevision(Revision, RevisionError));
	const TSharedPtr<FJsonObject> InvalidNativeUpdate = MakeShared<FJsonObject>();
	InvalidNativeUpdate->SetStringField(TEXT("operation"), TEXT("update"));
	InvalidNativeUpdate->SetStringField(TEXT("name"), HjsonIntentTargetName);
	InvalidNativeUpdate->SetStringField(TEXT("native_culture"), TEXT("de"));
	WorkspaceError.Reset();
	TestFalse(
		TEXT("an update cannot select a native culture outside retained cultures"),
		TryImportLocalizationTargets(
			Revision,
			{MakeShared<FJsonValueObject>(InvalidNativeUpdate)},
			Payload,
			WorkspaceError));
	TestEqual(
		TEXT("invalid retained-culture update is typed"),
		WorkspaceError.GetCode(),
		FString(TEXT("localization_target_native_culture_missing")));
	const ULocalizationTarget* const UnchangedTarget =
		FindTargetByName(HjsonIntentTargetName);
	TestTrue(
		TEXT("rejected native-culture update preserves the prior state"),
		UnchangedTarget
			&& UnchangedTarget->Settings.SupportedCulturesStatistics.IsValidIndex(
				UnchangedTarget->Settings.NativeCultureIndex)
			&& UnchangedTarget->Settings.SupportedCulturesStatistics[
				UnchangedTarget->Settings.NativeCultureIndex].CultureName == TEXT("en"));
	TestTrue(
		TEXT("revision remains readable after rejected update"),
		TryGetLocalizationRevision(Revision, RevisionError));
	const TSharedPtr<FJsonObject> Remove = MakeShared<FJsonObject>();
	Remove->SetStringField(TEXT("operation"), TEXT("remove"));
	Remove->SetStringField(TEXT("name"), HjsonIntentTargetName);
	WorkspaceError.Reset();
	TestTrue(
		TEXT("explicit remove cleans one owned Localization target"),
		TryImportLocalizationTargets(
			Revision,
			{MakeShared<FJsonValueObject>(Remove)},
			Payload,
			WorkspaceError));
	TestNull(
		TEXT("removed target is independently absent"),
		FindTargetByName(HjsonIntentTargetName));
	TestFalse(
		TEXT("removed target config is absent"),
		FPaths::FileExists(LocalizationConfigPath(HjsonIntentTargetName)));
	TestFalse(
		TEXT("removed target content is absent"),
		IFileManager::Get().DirectoryExists(
			*LocalizationContentPath(HjsonIntentTargetName)));

	TestTrue(
		TEXT("post-remove revision is independently readable"),
		TryGetLocalizationRevision(Revision, RevisionError));
	const TSharedPtr<FJsonObject> AtomicCreateA = MakeShared<FJsonObject>();
	AtomicCreateA->SetStringField(TEXT("operation"), TEXT("create"));
	AtomicCreateA->SetStringField(TEXT("name"), HjsonAtomicTargetA);
	AtomicCreateA->SetStringField(TEXT("native_culture"), TEXT("en"));
	AtomicCreateA->SetArrayField(
		TEXT("cultures"),
		{MakeShared<FJsonValueString>(TEXT("en"))});
	const TSharedPtr<FJsonObject> AtomicCreateB = MakeShared<FJsonObject>();
	AtomicCreateB->SetStringField(TEXT("operation"), TEXT("create"));
	AtomicCreateB->SetStringField(TEXT("name"), HjsonAtomicTargetB);
	AtomicCreateB->SetStringField(TEXT("native_culture"), TEXT("en"));
	AtomicCreateB->SetArrayField(
		TEXT("cultures"),
		{MakeShared<FJsonValueString>(TEXT("en"))});
	const FString PublishBlocker = LocalizationConfigPath(HjsonAtomicTargetB);
	TestTrue(
		TEXT("the fixture creates a deterministic second-target publish blocker"),
		IFileManager::Get().MakeDirectory(*PublishBlocker, true));
	WorkspaceError.Reset();
	TestFalse(
		TEXT("a multi-target I/O failure rejects the complete change set"),
		TryImportLocalizationTargets(
			Revision,
			{
				MakeShared<FJsonValueObject>(AtomicCreateA),
				MakeShared<FJsonValueObject>(AtomicCreateB),
			},
			Payload,
			WorkspaceError));
	TestEqual(
		TEXT("the injected failure is the config publication boundary"),
		WorkspaceError.GetCode(),
		FString(TEXT("localization_config_publish_failed")));
	TestNull(
		TEXT("the first target is rolled back after the later failure"),
		FindTargetByName(HjsonAtomicTargetA));
	TestNull(
		TEXT("the failing target is rolled back"),
		FindTargetByName(HjsonAtomicTargetB));
	TestFalse(
		TEXT("the first target config is rolled back"),
		FPaths::FileExists(LocalizationConfigPath(HjsonAtomicTargetA)));

	RemoveHjsonIntentFixture();
	return !HasAnyErrors();
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T30ExactDispatchTest,
	"UAI.StrictV2.T30.EXTERNAL.LOCALIZATION.IMPORT.ExactDispatchAndTypedFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T30ExactDispatchTest::RunTest(const FString& Parameters)
{
#if !UAI_T30_EXTERNAL_LOCALIZATION_IMPORT_AVAILABLE
	TestTrue(TEXT("T30 exact public dispatch is implemented"), false);
	return false;
#else
	const TSharedRef<FFixtureService> Service = MakeShared<FFixtureService>();
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	TestTrue(
		TEXT("T30 contribution composes in isolation"),
		TryCreateLocalizationImportRegistration(
			Service,
			Registry,
			Dispatch,
			Error));
	if (!Registry.IsValid() || !Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}

	const FString CanonicalInput = Params(
		TEXT("D:/fixture.po"),
		TEXT("StrictV2T30"));
	const FUeAgentCommandResult Imported = Submit(
		Registry,
		Dispatch,
		CanonicalInput);
	TestTrue(TEXT("exact route delegates successfully"), Imported.IsOk());
	TestEqual(TEXT("one request reaches one service call"), Service->Calls, 1);
	TestEqual(
		TEXT("the thin handler preserves canonical params unchanged"),
		Service->LastRequest.CanonicalParamsJson,
		CanonicalInput);

	const FUeAgentCommandResult Unauthorized = Submit(
		Registry,
		Dispatch,
		TEXT("{}"),
		{});
	TestTrue(TEXT("missing operation authority is typed"), !Unauthorized.IsOk() && !Unauthorized.IsProtocolError());
	TestTrue(TEXT("authority code is stable"), Unauthorized.GetErrorJson().Contains(TEXT("localization_import_authority_required")));
	TestEqual(TEXT("unauthorized input never reaches the owner"), Service->Calls, 1);

	Service->bFail = true;
	const FUeAgentCommandResult Failure = Submit(
		Registry,
		Dispatch,
		Params(TEXT("D:/fixture.po"), TEXT("StrictV2T30")));
	TestTrue(TEXT("owner failure remains a business failure"), !Failure.IsOk() && !Failure.IsProtocolError());
	TestTrue(TEXT("owner code is preserved"), Failure.GetErrorJson().Contains(TEXT("fixture_t30_failed")));
	return !HasAnyErrors();
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T30ProductionFailureCleanupTest,
	"UAI.StrictV2.T30.EXTERNAL.LOCALIZATION.IMPORT.ProductionFailureAndCleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T30ProductionFailureCleanupTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T30_EXTERNAL_LOCALIZATION_IMPORT_AVAILABLE
	TestTrue(TEXT("T30 production failure semantics are implemented"), false);
	return false;
#else
	TSharedPtr<IUeAgentLocalizationImportCommandService> Service;
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	TestTrue(
		TEXT("production T30 service composes"),
		TryCreateProductionLocalizationImportCommandService(Service, Error));
	TestTrue(
		TEXT("production T30 contribution composes"),
		Service.IsValid()
			&& TryCreateLocalizationImportRegistration(
				Service.ToSharedRef(),
				Registry,
				Dispatch,
				Error));
	if (!Registry.IsValid() || !Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}

	const FString MissingSource = FPaths::ProjectIntermediateDir()
		/ TEXT("UAI/StrictV2/T30/missing.po");
	const FString TargetName = TEXT("StrictV2T30Missing");
	const FString Destination = FPaths::ProjectContentDir()
		/ TEXT("Localization") / TargetName / TEXT("en")
		/ (TargetName + TEXT(".archive"));
	const FUeAgentCommandResult Missing = Submit(
		Registry,
		Dispatch,
		Params(MissingSource, TargetName));
	TestTrue(
		TEXT("missing source fails before mutation"),
		!Missing.IsOk()
			&& !Missing.IsProtocolError()
			&& Missing.GetErrorJson().Contains(TEXT("localization_import_source_not_found"))
			&& Missing.GetErrorJson().Contains(TEXT("rejected")));
	TestFalse(
		TEXT("prewrite failure creates no localization archive"),
		FPaths::FileExists(Destination));
	return !HasAnyErrors();
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T30ProductionSuccessReadbackTest,
	"UAI.StrictV2.T30.EXTERNAL.LOCALIZATION.IMPORT.ProductionSuccessAndReadback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T30ProductionSuccessReadbackTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T30_EXTERNAL_LOCALIZATION_IMPORT_AVAILABLE
	TestTrue(TEXT("T30 production localization import is implemented"), false);
	return false;
#else
	FString CleanupError;
	TestTrue(
		TEXT("stale T30 production fixture state is absent or removable"),
		RemoveEvidenceFixture(CleanupError));
	FString FixtureError;
	if (!CreateEvidenceFixture(FixtureError))
	{
		AddError(FixtureError);
		return false;
	}

	TSharedPtr<IUeAgentLocalizationImportCommandService> Service;
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	const bool bComposed =
		TryCreateProductionLocalizationImportCommandService(Service, Error)
		&& Service.IsValid()
		&& TryCreateLocalizationImportRegistration(
			Service.ToSharedRef(),
			Registry,
			Dispatch,
			Error);
	TestTrue(TEXT("production T30 contribution composes"), bComposed);

	FUeAgentCommandResult Imported = FUeAgentCommandResult::ProtocolError(
		TEXT("corr-t30"), TEXT("req-t30"), ExpectedCommandId,
		TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
		TEXT("fixture_composition_failed"), Error);
	if (bComposed)
	{
		Imported = Submit(
			Registry,
			Dispatch,
			Params(EvidenceSourcePo(), EvidenceTargetName));
	}
	TestTrue(TEXT("production import succeeds through exact dispatch"), Imported.IsOk());
	if (Imported.IsOk())
	{
		const FString Payload = Imported.GetPayloadJson();
		TestTrue(
			TEXT("response proves the formal importer completed"),
			Payload.Contains(TEXT("\"formal_importer_completed\":true")));
		TestTrue(
			TEXT("response proves the formal compiler completed"),
			Payload.Contains(TEXT("\"formal_compiler_completed\":true")));
		TestTrue(
			TEXT("response contains independent readback"),
			Payload.Contains(TEXT("\"independent_readback\"")));
	}

	const FString CultureRoot = EvidenceTargetRoot() / EvidenceCulture;
	const FString PersistedPo = CultureRoot / (EvidenceTargetName + TEXT(".po"));
	const FString Archive = CultureRoot / (EvidenceTargetName + TEXT(".archive"));
	const FString LocRes = CultureRoot / (EvidenceTargetName + TEXT(".locres"));
	FString ArchiveText;
	TestTrue(TEXT("canonical PO is persisted"), FPaths::FileExists(PersistedPo));
	TestTrue(
		TEXT("archive is independently readable"),
		FFileHelper::LoadFileToString(ArchiveText, *Archive));
	TestTrue(
		TEXT("archive contains the imported translation"),
		ArchiveText.Contains(TEXT("Bonjour depuis T30")));
	TestTrue(
		TEXT("compiled locres is independently present"),
		IFileManager::Get().FileSize(*LocRes) > 0);

	CleanupError.Reset();
	TestTrue(
		TEXT("all production fixture state is removed"),
		RemoveEvidenceFixture(CleanupError));
	if (!CleanupError.IsEmpty())
	{
		AddError(CleanupError);
	}
	return !HasAnyErrors();
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T30PublicFixtureSetupTest,
	"UAI.StrictV2.Fixtures.T30.Localization.Setup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T30PublicFixtureSetupTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T30_EXTERNAL_LOCALIZATION_IMPORT_AVAILABLE
	TestTrue(TEXT("T30 localization fixture support is implemented"), false);
	return false;
#else
	FString CleanupError;
	TestTrue(
		TEXT("stale owned fixture state is absent or removable"),
		RemoveEvidenceFixture(CleanupError));
	FString Error;
	TestTrue(TEXT("bounded T30 public fixture is created"), CreateEvidenceFixture(Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
	}
	return !HasAnyErrors();
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T30PublicFixtureCleanupTest,
	"UAI.StrictV2.Fixtures.T30.Localization.Cleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T30PublicFixtureCleanupTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T30_EXTERNAL_LOCALIZATION_IMPORT_AVAILABLE
	TestTrue(TEXT("T30 localization fixture cleanup is implemented"), false);
	return false;
#else
	FString Error;
	TestTrue(TEXT("all T30 public fixture state is removed"), RemoveEvidenceFixture(Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
	}
	TestNull(TEXT("fixture target is absent after cleanup"), FindEvidenceTarget());
	return !HasAnyErrors();
#endif
}
