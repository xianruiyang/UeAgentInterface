#include "Misc/AutomationTest.h"

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
#include "Misc/ScopeExit.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "Module/CommandRegistration/ExternalImport/Localization/UeAgentLocalizationImportRegistration.h"
#include "OperationOwners/ExternalImport/Localization/UeAgentLocalizationImportCommandService.h"
#include "OperationOwners/LocalizationStringTable/UeAgentLocalizationStringTableDomainService.h"

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

	FString LocalizationContentPath(const FString& TargetName)
	{
		return FPaths::ProjectContentDir()
			/ TEXT("Localization") / TargetName;
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
				OutError.InternalContext = TEXT("fixture failure remains typed");
				OutError.TerminalStatus = TEXT("rejected");
				return false;
			}
			OutResponse.PayloadJson =
				TEXT("{\"command_id\":\"external.localization.po.import\",")
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
		return UE::UAI::CommandContract::Json::TryCanonicalize(Json, Result, Error)
			? Result
			: FString(TEXT("{}"));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T30ReadCapabilityBoundaryTest,
	"UAI.StrictV2.T30.EXTERNAL.LOCALIZATION.IMPORT.ReadCapabilityBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T30ReadCapabilityBoundaryTest::RunTest(
	const FString& Parameters)
{
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T30ActiveCompositionTest,
	"UAI.StrictV2.T30.EXTERNAL.LOCALIZATION.IMPORT.ActiveComposition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T30ActiveCompositionTest::RunTest(
	const FString& Parameters)
{
	const TArray<FUeAgentLocalizationImportCommandRoute>& Routes =
		GetLocalizationImportCommandRoutes();
	const FUeAgentLocalizationImportCommandRoute* Route = Routes.FindByPredicate(
		[](const FUeAgentLocalizationImportCommandRoute& Candidate)
		{
			return Candidate.CommandId == ExpectedCommandId;
		});
	TestNotNull(TEXT("the T30 route identity is present"), Route);
	if (Route)
	{
		TestTrue(TEXT("the T30 route is valid"), Route->IsValid());
		TestEqual(TEXT("the T30 route identity is exact"), Route->CommandId, ExpectedCommandId);
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T30HjsonProviderContractTest,
	"UAI.StrictV2.T30.EXTERNAL.LOCALIZATION.IMPORT.HjsonProviderContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T30HjsonProviderContractTest::RunTest(
	const FString& Parameters)
{
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T30ExactDispatchTest,
	"UAI.StrictV2.T30.EXTERNAL.LOCALIZATION.IMPORT.ExactDispatchAndTypedFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T30ExactDispatchTest::RunTest(const FString& Parameters)
{
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T30ProductionFailureCleanupTest,
	"UAI.StrictV2.T30.EXTERNAL.LOCALIZATION.IMPORT.ProductionFailureAndCleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T30ProductionFailureCleanupTest::RunTest(
	const FString& Parameters)
{
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T30ProductionSuccessReadbackTest,
	"UAI.StrictV2.T30.EXTERNAL.LOCALIZATION.IMPORT.ProductionSuccessAndReadback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T30ProductionSuccessReadbackTest::RunTest(
	const FString& Parameters)
{
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
			TEXT("response does not expose lifecycle evidence"),
			!Payload.Contains(TEXT("\"independent_readback\""))
				&& !Payload.Contains(TEXT("\"persistence_receipt_id\""))
				&& !Payload.Contains(TEXT("\"phase_history\""))
				&& !Payload.Contains(TEXT("sha256")));
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208LocalizationWorkspaceTest,
	"UAI.Root.T208.ExternalInput.LocalizationWorkspace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208LocalizationWorkspaceTest::RunTest(const FString&)
{
	FString Error;
	if (!CreateEvidenceFixture(Error)) { AddError(Error); return false; }
	ON_SCOPE_EXIT
	{
		FString CleanupError;
		if (!RemoveEvidenceFixture(CleanupError)) AddError(CleanupError);
	};
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	if (!TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
	auto SubmitPublic = [&](const FString& Command, const TSharedRef<FJsonObject>& Args)
	{
		return UE::UAI::Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, Command, Args,
			TEXT("t208-localization-workspace"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
	};
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()
		/ TEXT("tmp/TROOT-UAI-208-external-input/localization-native") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString Archive = EvidenceTargetRoot() / EvidenceCulture / (EvidenceTargetName + TEXT(".archive"));
	const FString LocRes = EvidenceTargetRoot() / EvidenceCulture / (EvidenceTargetName + TEXT(".locres"));
	const FString PersistedPo = EvidenceTargetRoot() / EvidenceCulture / (EvidenceTargetName + TEXT(".po"));
	auto Prepare = [&](const FString& Stage)
	{
		auto SourceInfo = MakeShared<FJsonObject>();
		SourceInfo->SetStringField(TEXT("identity"), FPaths::ConvertRelativePathToFull(EvidenceSourcePo()));
		SourceInfo->SetStringField(TEXT("format"), TEXT("po"));
		auto Parameters = MakeShared<FJsonObject>(); Parameters->SetStringField(TEXT("target_ref"), EvidenceTargetName);
		auto Policy = MakeShared<FJsonObject>(); Policy->SetStringField(TEXT("command_id"), ExpectedCommandId); Policy->SetObjectField(TEXT("parameters"), Parameters);
		auto External = MakeShared<FJsonObject>(); External->SetObjectField(TEXT("source"), SourceInfo);
		External->SetObjectField(TEXT("options"), MakeShared<FJsonObject>()); External->SetObjectField(TEXT("target_policy"), Policy);
		auto Root = MakeShared<FJsonObject>(); Root->SetStringField(TEXT("type"), TEXT("uai.external_input.localization")); Root->SetObjectField(TEXT("external_input"), External);
		auto Document = MakeShared<FJsonObject>(); Document->SetStringField(TEXT("schema"), TEXT("uai.hjson.external_input/1")); Document->SetObjectField(TEXT("root"), Root);
		FString Text; if (!UE::UAI::CommandContract::Json::TryCanonicalize(Document, Text, Error)) { AddError(Error); return false; }
		auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("inline")); Source->SetStringField(TEXT("document"), Text);
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), EvidenceTargetName);
		Args->SetStringField(TEXT("workspace_path"), Directory / Stage); Args->SetObjectField(TEXT("source"), Source);
		const auto Result = SubmitPublic(TEXT("content_graph.export_workspace"), Args);
		if (!Result.IsOk()) AddError(Result.GetErrorJson());
		return Result.IsOk();
	};
	auto Import = [&](const FString& Stage)
	{
		auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Directory / Stage);
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), EvidenceTargetName); Args->SetObjectField(TEXT("source"), Source);
		return SubmitPublic(TEXT("content_graph.import_workspace"), Args);
	};
	FString Before, After;
	if (!FFileHelper::LoadFileToString(Before, *Archive) || !Prepare(TEXT("initial"))) return false;
	TestTrue(TEXT("source preparation does not write target archive"), FFileHelper::LoadFileToString(After, *Archive) && Before == After);
	TestFalse(TEXT("source preparation does not compile resources"), FPaths::FileExists(LocRes));
	const auto Imported = Import(TEXT("initial"));
	if (!TestTrue(TEXT("public workspace imports through real PO importer"), Imported.IsOk())) { AddError(Imported.GetErrorJson()); return false; }
	TestTrue(TEXT("independent archive contains translation"), FFileHelper::LoadFileToString(After, *Archive) && After.Contains(TEXT("Bonjour depuis T30")));
	TestTrue(TEXT("native PO is persisted"), FPaths::FileExists(PersistedPo));
	TestTrue(TEXT("native locres is compiled"), IFileManager::Get().FileSize(*LocRes) > 0);
	if (!Prepare(TEXT("target-change"))) return false;
	const FString ChangedArchive = After + TEXT("\n");
	if (!FFileHelper::SaveStringToFile(ChangedArchive, *Archive, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	const auto TargetRejected = Import(TEXT("target-change"));
	TestFalse(TEXT("changed target file baseline is rejected"), TargetRejected.IsOk());
	TestTrue(TEXT("target change has exact rejection"), TargetRejected.GetErrorJson().Contains(TEXT("hjson_external_input_target_changed")));
	FString RejectedArchive;
	TestTrue(TEXT("target rejection preserves the newer archive"), FFileHelper::LoadFileToString(RejectedArchive, *Archive) && RejectedArchive == ChangedArchive);
	if (!FFileHelper::SaveStringToFile(After, *Archive, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) || !Prepare(TEXT("source-change"))) return false;
	FString Po; if (!FFileHelper::LoadFileToString(Po, *EvidenceSourcePo())) return false;
	Po.ReplaceInline(TEXT("Bonjour depuis T30"), TEXT("Bonjour modifie"));
	if (!FFileHelper::SaveStringToFile(Po, *EvidenceSourcePo(), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	const auto SourceRejected = Import(TEXT("source-change"));
	TestFalse(TEXT("changed PO source is rejected"), SourceRejected.IsOk());
	TestTrue(TEXT("source change remains distinct from target change"), SourceRejected.GetErrorJson().Contains(TEXT("hjson_external_input_source_digest_mismatch")));
	TestTrue(TEXT("source rejection preserves archive"), FFileHelper::LoadFileToString(RejectedArchive, *Archive) && RejectedArchive == After);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T30PublicFixtureSetupTest,
	"UAI.StrictV2.Fixtures.T30.Localization.Setup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T30PublicFixtureSetupTest::RunTest(
	const FString& Parameters)
{
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T30PublicFixtureCleanupTest,
	"UAI.StrictV2.Fixtures.T30.Localization.Cleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T30PublicFixtureCleanupTest::RunTest(
	const FString& Parameters)
{
	FString Error;
	TestTrue(TEXT("all T30 public fixture state is removed"), RemoveEvidenceFixture(Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
	}
	TestNull(TEXT("fixture target is absent after cleanup"), FindEvidenceTarget());
	return !HasAnyErrors();
}
