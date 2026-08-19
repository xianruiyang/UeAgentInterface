#include "Misc/AutomationTest.h"

#if __has_include("DomainAdapters/UObjectDataAsset/UeAgentRecursiveDataAsset.h") \
	&& __has_include("OperationOwners/UObjectDataAsset/UeAgentUObjectDataAssetCommandService.h") \
	&& __has_include("Module/CommandRegistration/UObjectDataAsset/UeAgentUObjectDataAssetRegistration.h")
#define UAI_T13_UOBJECT_RECURSIVE_INIT_AVAILABLE 1
#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "Composition/ReadExportCommand/UeAgentReadExportCommandHandlers.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "DomainAdapters/UObjectDataAsset/UeAgentRecursiveDataAsset.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "Module/CommandRegistration/UObjectDataAsset/UeAgentUObjectDataAssetRegistration.h"
#include "ObjectTools.h"
#include "OperationOwners/UObjectDataAsset/UeAgentUObjectDataAssetCommandService.h"
#include "UObject/UObjectGlobals.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END
#else
#define UAI_T13_UOBJECT_RECURSIVE_INIT_AVAILABLE 0
#endif

#if UAI_T13_UOBJECT_RECURSIVE_INIT_AVAILABLE
namespace
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandRegistry;
	using namespace UE::UAI::Composition::ReadExportCommand;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::DomainAdapters::UObjectDataAsset;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Module::CommandRegistration::UObjectDataAsset;
	using namespace UE::UAI::OperationOwners::UObjectDataAsset;

	const TArray<FString> ExpectedCommandIds = {
		TEXT("content_graph.compound"),
		TEXT("content_graph.diagnostics"),
		TEXT("content_graph.export_workspace"),
		TEXT("content_graph.import_workspace"),
		TEXT("content_graph.items"),
		TEXT("content_graph.properties"),
		TEXT("content_graph.relations"),
		TEXT("content_graph.structure"),
		TEXT("content_graph.summary"),
	};

	class FNeverCancelled final : public IUeAgentCancellationSignal
	{
	public:
		virtual bool IsCancelled() const override { return false; }
	};

	class FFixtureService final : public IUeAgentUObjectDataAssetCommandService
	{
	public:
		virtual bool Execute(
			const FUeAgentUObjectDataAssetCommandRequest& Request,
			FUeAgentUObjectDataAssetCommandResponse& OutResponse,
			FUeAgentUObjectDataAssetCommandError& OutError) override
		{
			++Calls;
			LastRequest = Request;
			OutResponse = {};
			OutError = {};
			if (bFail)
			{
				OutError.Code = TEXT("fixture_t13_failed");
				OutError.SubjectId = Request.CommandId;
				OutError.Detail = TEXT("fixture failure remains typed");
				return false;
			}
			OutResponse.PayloadJson = FString::Printf(
				TEXT("{\"command_id\":\"%s\",\"read_only\":%s}"),
				*Request.CommandId,
				Request.CommandId == TEXT("content_graph.import_workspace")
					? TEXT("false")
					: TEXT("true"));
			OutResponse.bReadOnly =
				Request.CommandId != TEXT("content_graph.import_workspace");
			return true;
		}

		int32 Calls = 0;
		FUeAgentUObjectDataAssetCommandRequest LastRequest;
		bool bFail = false;
	};

	struct FFixtureRegistration
	{
		TSharedRef<FFixtureService> Service = MakeShared<FFixtureService>();
		TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
		TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
		FString Error;
	};

	FFixtureRegistration MakeRegistration(FAutomationTestBase* Test)
	{
		FFixtureRegistration Fixture;
		Test->TestTrue(
			TEXT("T13 contribution composes in isolation"),
			TryCreateUObjectDataAssetRegistration(
				Fixture.Service,
				Fixture.Registry,
				Fixture.Dispatch,
				Fixture.Error));
		Test->TestTrue(TEXT("T13 immutable registry exists"), Fixture.Registry.IsValid());
		Test->TestTrue(TEXT("T13 exact dispatch exists"), Fixture.Dispatch.IsValid());
		return Fixture;
	}

	bool DecodeRequest(
		const FString& CommandId,
		const FString& ContractHash,
		const FString& ParamsJson,
		TOptional<FUeAgentCommandRequest>& OutRequest)
	{
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"corr-t13\",")
			TEXT("\"request_id\":\"req-t13\",")
			TEXT("\"command_id\":\"%s\",")
			TEXT("\"command_binding_hash\":\"%s\",\"params\":%s}"),
			*CommandId,
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
		const FString& CommandId,
		const FString& ParamsJson,
		TArray<FString> Authorities = {
			TEXT("uai:query"),
			TEXT("uai:export"),
			TEXT("uai:operate"),
		})
	{
		const auto* Contract = Registry.IsValid()
			? Registry->FindExact(CommandId)
			: nullptr;
		if (!Contract || !Dispatch.IsValid())
		{
			return FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t13"), TEXT("req-t13"), CommandId,
				TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
				TEXT("fixture_contract_missing"),
				TEXT("T13 fixture could not find the exact contract"));
		}
		TOptional<FUeAgentCommandRequest> Request;
		if (!DecodeRequest(
				CommandId,
				Contract->GetContractHash(),
				ParamsJson,
				Request)
			|| !Request.IsSet())
		{
			return FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t13"), TEXT("req-t13"), CommandId,
				Contract->GetContractHash(),
				TEXT("fixture_request_decode_failed"),
				TEXT("T13 fixture could not decode its request"));
		}
		return Dispatch->Submit(
			Request.GetValue(),
			FUeAgentCallerContext(
				TEXT("strict-v2-t13"),
				MoveTemp(Authorities),
				TEXT("corr-t13")),
			MakeShared<FNeverCancelled>());
	}

	FString RecursiveDocument(const bool bOwnershipCycle = false)
	{
		const FString Ownership = bOwnershipCycle
			? TEXT(R"json(
				"ownership": [
					{"owner":"left","child":"right"},
					{"owner":"right","child":"left"}
				],)json")
			: FString();
		return FString::Printf(TEXT(R"json({
			"schema":"uai.uobject_dataasset.recursive/1",
			"root":{
				"id":"root",
				"type":"/Script/UeAgentInterface.UeAgentRecursiveDataAsset",
				"values":{
					"label":"explicit-root",
					"explicit_number":42,
					"numbers":[3,5,8],
					"named_numbers":{"alpha":13,"beta":21}
				},
				"owned":[
					{"id":"shared","name":"Shared","type":"/Script/UeAgentInterface.UeAgentRecursiveOwnedObject","values":{"label":"shared"},"owned":[{"id":"deep","name":"Deep","type":"/Script/UeAgentInterface.UeAgentRecursiveOwnedObject","values":{"label":"deep"}}]},
					{"id":"left","name":"Left","type":"/Script/UeAgentInterface.UeAgentRecursiveOwnedObject","values":{"label":"left"}},
					{"id":"right","name":"Right","type":"/Script/UeAgentInterface.UeAgentRecursiveOwnedObject","values":{"label":"right"}}
				],
				%s
				"references":[
					{"from":"left","field":"peer","to":"right"},
					{"from":"right","field":"peer","to":"left"},
					{"from":"left","field":"shared","to":"shared"},
					{"from":"right","field":"shared","to":"shared"}
				]
			}
		})json"), *Ownership);
	}

	FString Sha256Text(const FString& Text)
	{
		FTCHARToUTF8 Utf8(*Text);
		uint8 Digest[SHA256_DIGEST_LENGTH] = {};
		SHA256(
			reinterpret_cast<const uint8*>(Utf8.Get()),
			Utf8.Length(),
			Digest);
		return TEXT("sha256:")
			+ BytesToHex(Digest, SHA256_DIGEST_LENGTH).ToLower();
	}

	FString ImportParamsForSource(
		const FString& Target,
		const TSharedRef<FJsonObject>& Source,
		const int32 MaxNodes = 64)
	{
		const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), Target);
		Params->SetObjectField(TEXT("source"), Source);
		Params->SetStringField(TEXT("expected_revision"), TEXT("absent"));
		Params->SetStringField(TEXT("authorization_id"), TEXT("auth:t13:automation"));
		const TSharedPtr<FJsonObject> Limits = MakeShared<FJsonObject>();
		Limits->SetNumberField(TEXT("max_nodes"), MaxNodes);
		Limits->SetNumberField(TEXT("max_work_units"), 4096);
		Params->SetObjectField(TEXT("limits"), Limits);
		FString Result;
		FString Error;
		return Json::TryCanonicalize(Params, Result, Error)
			? Result
			: FString(TEXT("{}"));
	}

	FString ImportParams(
		const FString& Target,
		const FString& Document,
		const int32 MaxNodes = 64)
	{
		const TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("kind"), TEXT("inline"));
		Source->SetStringField(TEXT("document"), Document);
		return ImportParamsForSource(Target, Source, MaxNodes);
	}

	FString FileImportParams(
		const FString& Target,
		const FString& Kind,
		const FString& Path)
	{
		const TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("kind"), Kind);
		Source->SetStringField(TEXT("path"), Path);
		return ImportParamsForSource(Target, Source);
	}

	bool WriteText(const FString& Path, const FString& Text)
	{
		return IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true)
			&& FFileHelper::SaveStringToFile(
				Text,
				*Path,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	bool WriteDirectoryManifest(
		const FString& Root,
		const FString& Target,
		const FString& EntryFile,
		const FString& ContentHash)
	{
		const TSharedRef<FJsonObject> Manifest = MakeShared<FJsonObject>();
		Manifest->SetStringField(
			TEXT("workspace_schema"),
			TEXT("uai.content_graph.workspace/1"));
		Manifest->SetNumberField(TEXT("format_version"), 1);
		Manifest->SetStringField(TEXT("target_identity"), Target);
		Manifest->SetStringField(TEXT("base_revision"), TEXT("absent"));
		Manifest->SetStringField(TEXT("entry_file"), EntryFile);
		const TSharedRef<FJsonObject> File = MakeShared<FJsonObject>();
		File->SetStringField(TEXT("path"), EntryFile);
		File->SetStringField(TEXT("content_hash"), ContentHash);
		Manifest->SetArrayField(
			TEXT("files"),
			{MakeShared<FJsonValueObject>(File)});
		Manifest->SetArrayField(
			TEXT("references"),
			TArray<TSharedPtr<FJsonValue>>{});
		FString Text;
		FString Error;
		return Json::TryCanonicalize(Manifest, Text, Error)
			&& WriteText(
				FPaths::Combine(Root, TEXT(".uai/workspace.hjson")),
				Text);
	}

	bool WriteDirectoryWorkspace(
		const FString& Root,
		const FString& Target,
		const FString& Document)
	{
		return WriteText(FPaths::Combine(Root, TEXT("asset.hjson")), Document)
			&& WriteDirectoryManifest(
				Root,
				Target,
				TEXT("asset.hjson"),
				Sha256Text(Document));
	}

	void DeleteAssetIfPresent(const FString& ObjectPath)
	{
		if (UObject* Existing = StaticLoadObject(
			UObject::StaticClass(), nullptr, *ObjectPath))
		{
			ObjectTools::DeleteObjectsUnchecked({Existing});
		}
	}

	TSharedPtr<FJsonObject> Payload(
		FAutomationTestBase* Test,
		const FUeAgentCommandResult& Result)
	{
		TSharedPtr<FJsonObject> Object;
		FString Error;
		Test->TestTrue(
			TEXT("public response is canonical JSON"),
			Json::TryParseObject(Result.GetPayloadJson(), Object, Error));
		return Object;
	}
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T13DomainProviderActivationTest,
	"UAI.StrictV2.T13.UOBJECT.RECURSIVE.INIT.DomainProviderActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T13DomainProviderActivationTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T13_UOBJECT_RECURSIVE_INIT_AVAILABLE
	TestTrue(TEXT("T13 production domain provider is available"), false);
	return false;
#else
	using namespace UE::UAI::HjsonWorkspace;
	FWorkspaceError Error;
	TSharedPtr<const FImportInitializationDomainProviderRegistry> Registry;
	if (!TestTrue(
		TEXT("the process HJSON domain provider catalog freezes"),
		TryFreezeProcessImportInitializationDomainProviders(
			Registry, Error)))
	{
		AddError(Error.GetCode());
		return false;
	}
#if defined(UAI_HJSON_DOMAIN_PROVIDER_RESOLVES_CONTRACT)
	const FImportInitializationDomainRequest Request(
		TEXT("content_graph.import_workspace"),
		TEXT("uai.uobject_dataasset.recursive/1"),
		TEXT("/Game/UAI/StrictV2/T13/ProviderFixture.ProviderFixture"),
		TEXT("/Script/UeAgentInterface.UeAgentRecursiveDataAsset"),
		EInitializationNodeRole::RootTarget,
		TEXT("{}"));
	FImportInitializationDomainResolution Resolution;
	TestTrue(
		TEXT("the UObject/DataAsset schema and root type resolve one provider"),
		Registry->TryResolve(Request, Resolution, Error));
	TestTrue(TEXT("the resolved provider and contract exist"), Resolution.IsValid());
	if (Resolution.IsValid())
	{
		TestEqual(
			TEXT("the provider identity is exact"),
			Resolution.GetProvider()->GetProviderId(),
			FString(TEXT("uobject-dataasset-domain-provider")));
		TestEqual(
			TEXT("the provider supplies the root initialization contract"),
			Resolution.GetInitializationContractId(),
			FString(TEXT("uai.init.uobject_dataasset.root/1")));
	}
#else
	const FImportInitializationDomainRequest Request(
		TEXT("content_graph.import_workspace"),
		TEXT("uai.uobject_dataasset.recursive/1"),
		TEXT("/Game/UAI/StrictV2/T13/ProviderFixture.ProviderFixture"),
		TEXT("/Script/UeAgentInterface.UeAgentRecursiveDataAsset"),
		EInitializationNodeRole::RootTarget,
		TEXT("uai.init.uobject_dataasset.root/1"),
		TEXT("{}"));
	TSharedPtr<const IImportInitializationDomainProvider> Provider;
	TestTrue(
		TEXT("the UObject/DataAsset schema resolves one provider"),
		Registry->TryResolve(Request, Provider, Error));
#endif
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T13InventoryAndLegacyExitTest,
	"UAI.StrictV2.T13.UOBJECT.RECURSIVE.INIT.InventoryAndLegacyExit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T13InventoryAndLegacyExitTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T13_UOBJECT_RECURSIVE_INIT_AVAILABLE
	TestTrue(TEXT("T13 UObject/DataAsset owner is implemented"), false);
	return false;
#else
	TArray<FString> Actual;
	for (const FUeAgentUObjectDataAssetCommandRoute& Route
		: GetUObjectDataAssetCommandRoutes())
	{
		Actual.Add(Route.CommandId);
		TestTrue(TEXT("every T13 route is valid"), Route.IsValid());
	}
	Actual.Sort();
	TestEqual(TEXT("T13 owns exactly the audited nine commands"), Actual, ExpectedCommandIds);
	TestEqual(TEXT("T13 routes remain unique"), TSet<FString>(Actual).Num(), 9);

	TSet<FString> LegacyReadExport;
	for (const FUeAgentReadExportCommandRoute& Route
		: GetReadExportQueryCommandRoutes())
	{
		LegacyReadExport.Add(Route.CommandId);
	}
	for (const FString& CommandId : ExpectedCommandIds)
	{
		TestFalse(
			TEXT("migrated command is absent from the old read/export authority"),
			LegacyReadExport.Contains(CommandId));
	}

	TSharedPtr<const FUeAgentImmutableCommandRegistry> ProcessRegistry;
	TSharedPtr<const FUeAgentExactCommandDispatch> ProcessDispatch;
	FString Error;
	TestTrue(
		TEXT("process active composition includes T13"),
		TryComposeProcessActiveCommandComposition(
			ProcessRegistry,
			ProcessDispatch,
			Error));
	for (const FString& CommandId : ExpectedCommandIds)
	{
		const auto* Contract = ProcessRegistry.IsValid()
			? ProcessRegistry->FindExact(CommandId)
			: nullptr;
		TestNotNull(TEXT("every T13 command has one active exact contract"), Contract);
		if (Contract)
		{
			TestEqual(
				TEXT("the active contract publishes the unique T13 owner"),
				Contract->GetPublicOwner(),
				FString(TEXT("uobject-dataasset-recursive-initialization-owner")));
		}
	}
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T13ExactDispatchTest,
	"UAI.StrictV2.T13.UOBJECT.RECURSIVE.INIT.ExactDispatchAndTypedFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T13ExactDispatchTest::RunTest(const FString& Parameters)
{
#if !UAI_T13_UOBJECT_RECURSIVE_INIT_AVAILABLE
	TestTrue(TEXT("T13 exact public dispatch is implemented"), false);
	return false;
#else
	FFixtureRegistration Fixture = MakeRegistration(this);
	if (!Fixture.Registry.IsValid() || !Fixture.Dispatch.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("isolated T13 registry contains nine contracts"), Fixture.Registry->GetContracts().Num(), 9);
	const FUeAgentCommandResult Query = Submit(
		Fixture.Registry,
		Fixture.Dispatch,
		TEXT("content_graph.summary"),
		TEXT("{\"target\":\"/Game/StrictV2/T13/Fixture.Fixture\"}"));
	TestTrue(TEXT("exact query route delegates successfully"), Query.IsOk());
	TestEqual(TEXT("one request reaches one service call"), Fixture.Service->Calls, 1);
	TestEqual(
		TEXT("service receives the unchanged exact command identity"),
		Fixture.Service->LastRequest.CommandId,
		FString(TEXT("content_graph.summary")));

	const FUeAgentCommandResult Unauthorized = Submit(
		Fixture.Registry,
		Fixture.Dispatch,
		TEXT("content_graph.import_workspace"),
		TEXT("{}"),
		{});
	TestTrue(TEXT("missing operation authority is a business failure"), !Unauthorized.IsOk() && !Unauthorized.IsProtocolError());
	TestTrue(TEXT("authority failure remains typed"), Unauthorized.GetErrorJson().Contains(TEXT("uobject_dataasset_authority_required")));
	TestEqual(TEXT("unauthorized request never reaches the owner"), Fixture.Service->Calls, 1);

	Fixture.Service->bFail = true;
	const FUeAgentCommandResult Failure = Submit(
		Fixture.Registry,
		Fixture.Dispatch,
		TEXT("content_graph.summary"),
		TEXT("{\"target\":\"/Game/StrictV2/T13/Fixture.Fixture\"}"));
	TestTrue(TEXT("owner failure remains a business failure"), !Failure.IsOk() && !Failure.IsProtocolError());
	TestTrue(TEXT("owner code is preserved"), Failure.GetErrorJson().Contains(TEXT("fixture_t13_failed")));
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T13ProductionRoundTripTest,
	"UAI.StrictV2.T13.UOBJECT.RECURSIVE.INIT.ProductionPublicPersistReloadExport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T13ProductionRoundTripTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T13_UOBJECT_RECURSIVE_INIT_AVAILABLE
	TestTrue(TEXT("T13 production recursive initialization is implemented"), false);
	return false;
#else
	TestTrue(TEXT("production UObject/DataAsset mechanics run on the game thread"), IsInGameThread());
	const FString Target = TEXT("/Game/StrictV2/T13/DA_Recursive.DA_Recursive");
	const FString Workspace = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / TEXT("Automation/StrictV2/T13/export"));
	const FString SourceRoot = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / TEXT("Automation/StrictV2/T13/import_sources"));
	const FString SingleTarget =
		TEXT("/Game/StrictV2/T13/DA_RecursiveSingle.DA_RecursiveSingle");
	const FString DirectoryTarget =
		TEXT("/Game/StrictV2/T13/DA_RecursiveDirectory.DA_RecursiveDirectory");
	const FString SingleFile =
		FPaths::Combine(SourceRoot, TEXT("single/asset.hjson"));
	const FString DirectoryRoot = FPaths::Combine(SourceRoot, TEXT("directory"));
	DeleteAssetIfPresent(Target);
	DeleteAssetIfPresent(SingleTarget);
	DeleteAssetIfPresent(DirectoryTarget);
	IFileManager::Get().DeleteDirectory(*Workspace, false, true);
	IFileManager::Get().DeleteDirectory(*SourceRoot, false, true);
	TestTrue(
		TEXT("single-file source fixture persists"),
		WriteText(SingleFile, RecursiveDocument()));
	TestTrue(
		TEXT("directory source fixture persists its document and machine manifest"),
		WriteDirectoryWorkspace(
			DirectoryRoot,
			DirectoryTarget,
			RecursiveDocument()));

	TSharedPtr<IUeAgentUObjectDataAssetCommandService> Service;
	FString Error;
	TestTrue(
		TEXT("production T13 service composes"),
		TryCreateProductionUObjectDataAssetCommandService(Service, Error));
	if (!Service.IsValid())
	{
		AddError(Error);
		return false;
	}
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	TestTrue(
		TEXT("production T13 contribution composes"),
		TryCreateUObjectDataAssetRegistration(
			Service.ToSharedRef(), Registry, Dispatch, Error));
	if (!Registry.IsValid() || !Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}

	const FUeAgentCommandResult Imported = Submit(
		Registry,
		Dispatch,
		TEXT("content_graph.import_workspace"),
		ImportParams(Target, RecursiveDocument()));
	TestTrue(TEXT("public HJSON import succeeds"), Imported.IsOk());
	if (!Imported.IsOk())
	{
		AddError(Imported.GetErrorJson());
	}
	const TSharedPtr<FJsonObject> ImportPayload = Imported.IsOk()
		? Payload(this, Imported)
		: nullptr;
	if (ImportPayload.IsValid())
	{
		TestEqual(TEXT("operation reaches a succeeded terminal"), ImportPayload->GetStringField(TEXT("terminal_status")), FString(TEXT("succeeded")));
		TestTrue(TEXT("operation reports that mutation started"), ImportPayload->GetBoolField(TEXT("mutation_started")));
		TestTrue(TEXT("persistence receipt is present"), !ImportPayload->GetStringField(TEXT("persistence_receipt_id")).IsEmpty());
		const TSharedPtr<FJsonObject>* Readback = nullptr;
		TestTrue(TEXT("independent readback is returned"), ImportPayload->TryGetObjectField(TEXT("independent_readback"), Readback));
		if (Readback && Readback->IsValid())
		{
			TestTrue(TEXT("asset was unloaded and reloaded"), (*Readback)->GetBoolField(TEXT("reloaded")));
			TestEqual(TEXT("root type survives reload"), (*Readback)->GetStringField(TEXT("asset_class")), FString(TEXT("/Script/UeAgentInterface.UeAgentRecursiveDataAsset")));
			TestEqual(TEXT("native root default is preserved"), static_cast<int32>((*Readback)->GetNumberField(TEXT("native_counter"))), 11);
			TestEqual(TEXT("explicit override survives reload"), (*Readback)->GetStringField(TEXT("label")), FString(TEXT("explicit-root")));
			TestEqual(TEXT("explicit numeric override survives reload"), static_cast<int32>((*Readback)->GetNumberField(TEXT("explicit_number"))), 42);
			TestEqual(TEXT("root owns three direct objects"), static_cast<int32>((*Readback)->GetNumberField(TEXT("owned_count"))), 3);
			TestEqual(TEXT("deep ownership survives reload"), static_cast<int32>((*Readback)->GetNumberField(TEXT("deep_owned_count"))), 1);
			TestTrue(TEXT("native default subobjects survive reload"), (*Readback)->GetBoolField(TEXT("native_default_subobjects_preserved")));
			TestTrue(TEXT("forward shared references preserve identity"), (*Readback)->GetBoolField(TEXT("shared_reference_preserved")));
			TestTrue(TEXT("reference cycles remain legal"), (*Readback)->GetBoolField(TEXT("reference_cycle_preserved")));
			TestTrue(TEXT("container values survive reload"), (*Readback)->GetBoolField(TEXT("containers_preserved")));
		}
	}

	const FUeAgentCommandResult SingleImported = Submit(
		Registry,
		Dispatch,
		TEXT("content_graph.import_workspace"),
		FileImportParams(SingleTarget, TEXT("single_file"), SingleFile));
	TestTrue(
		TEXT("the same public command imports a single-file source"),
		SingleImported.IsOk());
	const TSharedPtr<FJsonObject> SinglePayload = SingleImported.IsOk()
		? Payload(this, SingleImported)
		: nullptr;
	if (SinglePayload.IsValid())
	{
		const TSharedPtr<FJsonObject>* Readback = nullptr;
		TestTrue(
			TEXT("single-file import returns independent readback"),
			SinglePayload->TryGetObjectField(TEXT("independent_readback"), Readback));
		if (Readback && Readback->IsValid())
		{
			TestTrue(
				TEXT("single-file import persists and reloads"),
				(*Readback)->GetBoolField(TEXT("reloaded")));
		}
	}

	const FUeAgentCommandResult DirectoryImported = Submit(
		Registry,
		Dispatch,
		TEXT("content_graph.import_workspace"),
		FileImportParams(DirectoryTarget, TEXT("directory"), DirectoryRoot));
	TestTrue(
		TEXT("the same public command imports a directory source"),
		DirectoryImported.IsOk());
	const TSharedPtr<FJsonObject> DirectoryPayload = DirectoryImported.IsOk()
		? Payload(this, DirectoryImported)
		: nullptr;
	if (DirectoryPayload.IsValid())
	{
		const TSharedPtr<FJsonObject>* Readback = nullptr;
		TestTrue(
			TEXT("directory import returns independent readback"),
			DirectoryPayload->TryGetObjectField(TEXT("independent_readback"), Readback));
		if (Readback && Readback->IsValid())
		{
			TestTrue(
				TEXT("directory import persists and reloads"),
				(*Readback)->GetBoolField(TEXT("reloaded")));
		}
	}

	if (Imported.IsOk())
	{
		const FUeAgentCommandResult Queried = Submit(
			Registry,
			Dispatch,
			TEXT("content_graph.compound"),
			FString::Printf(
				TEXT("{\"target\":\"%s\",\"input\":{\"sections\":[\"summary\",\"structure\",\"items\",\"relations\",\"properties\"]}}"),
				*Target));
		TestTrue(TEXT("public content graph read succeeds after reload"), Queried.IsOk());
		const TSharedPtr<FJsonObject> QueryPayload = Queried.IsOk()
			? Payload(this, Queried)
			: nullptr;
		if (QueryPayload.IsValid())
		{
			TestTrue(TEXT("content graph query attests read-only"), QueryPayload->GetBoolField(TEXT("read_only")));
			TestEqual(TEXT("real mechanics identity is explicit"), QueryPayload->GetStringField(TEXT("mechanics_command_id")), FString(TEXT("content_graph.read.compound")));
			TestTrue(TEXT("query result is a typed object"), QueryPayload->HasTypedField<EJson::Object>(TEXT("query_result")));
		}

		const TSharedPtr<FJsonObject> ExportParamsObject = MakeShared<FJsonObject>();
		ExportParamsObject->SetStringField(TEXT("target"), Target);
		ExportParamsObject->SetStringField(TEXT("workspace_path"), Workspace);
		ExportParamsObject->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		FString ExportParams;
		FString CanonicalError;
		Json::TryCanonicalize(ExportParamsObject, ExportParams, CanonicalError);
		const FUeAgentCommandResult Exported = Submit(
			Registry,
			Dispatch,
			TEXT("content_graph.export_workspace"),
			ExportParams);
		TestTrue(TEXT("public verified HJSON export succeeds"), Exported.IsOk());
		const TSharedPtr<FJsonObject> ExportPayload = Exported.IsOk()
			? Payload(this, Exported)
			: nullptr;
		if (ExportPayload.IsValid())
		{
			TestEqual(TEXT("verified export is published"), ExportPayload->GetStringField(TEXT("status")), FString(TEXT("published")));
			TestTrue(TEXT("export publishes a plan fingerprint"), ExportPayload->GetStringField(TEXT("plan_fingerprint")).StartsWith(TEXT("sha256:")));
			TestTrue(TEXT("export publishes semantic verification"), ExportPayload->HasTypedField<EJson::Object>(TEXT("semantic_verification")));
		}
	}

	DeleteAssetIfPresent(Target);
	DeleteAssetIfPresent(SingleTarget);
	DeleteAssetIfPresent(DirectoryTarget);
	IFileManager::Get().DeleteDirectory(*Workspace, false, true);
	IFileManager::Get().DeleteDirectory(*SourceRoot, false, true);
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T13FailureCleanupTest,
	"UAI.StrictV2.T13.UOBJECT.RECURSIVE.INIT.PrewriteAndRollbackCleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T13FailureCleanupTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T13_UOBJECT_RECURSIVE_INIT_AVAILABLE
	TestTrue(TEXT("T13 failure terminals and cleanup are implemented"), false);
	return false;
#else
	TSharedPtr<IUeAgentUObjectDataAssetCommandService> Service;
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	TestTrue(TEXT("production service composes"), TryCreateProductionUObjectDataAssetCommandService(Service, Error));
	TestTrue(TEXT("production registration composes"), Service.IsValid() && TryCreateUObjectDataAssetRegistration(Service.ToSharedRef(), Registry, Dispatch, Error));
	if (!Registry.IsValid() || !Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}

	const FString CycleTarget = TEXT("/Game/StrictV2/T13/DA_Cycle.DA_Cycle");
	DeleteAssetIfPresent(CycleTarget);
	const FUeAgentCommandResult Cycle = Submit(
		Registry, Dispatch, TEXT("content_graph.import_workspace"),
		ImportParams(CycleTarget, RecursiveDocument(true)));
	TestTrue(TEXT("ownership cycle is a typed business failure"), !Cycle.IsOk() && Cycle.GetErrorJson().Contains(TEXT("hjson_import_ownership_cycle")));
	TestNull(TEXT("ownership cycle performs no write"), StaticFindObject(UObject::StaticClass(), nullptr, *CycleTarget));

	const FString UnknownTarget = TEXT("/Game/StrictV2/T13/DA_Unknown.DA_Unknown");
	DeleteAssetIfPresent(UnknownTarget);
	const FString UnknownDocument = RecursiveDocument().Replace(
		TEXT("/Script/UeAgentInterface.UeAgentRecursiveOwnedObject"),
		TEXT("/Script/UeAgentInterface.DoesNotExist"),
		ESearchCase::CaseSensitive);
	const FUeAgentCommandResult Unknown = Submit(
		Registry, Dispatch, TEXT("content_graph.import_workspace"),
		ImportParams(UnknownTarget, UnknownDocument));
	TestTrue(TEXT("unknown type is rejected before write"), !Unknown.IsOk() && Unknown.GetErrorJson().Contains(TEXT("uobject_dataasset_type_unknown")));
	TestNull(TEXT("unknown type leaves no asset"), StaticFindObject(UObject::StaticClass(), nullptr, *UnknownTarget));

	const FString BudgetTarget = TEXT("/Game/StrictV2/T13/DA_Budget.DA_Budget");
	DeleteAssetIfPresent(BudgetTarget);
	const FUeAgentCommandResult Budget = Submit(
		Registry, Dispatch, TEXT("content_graph.import_workspace"),
		ImportParams(BudgetTarget, RecursiveDocument(), 2));
	TestTrue(TEXT("real node budget is enforced before write"), !Budget.IsOk() && Budget.GetErrorJson().Contains(TEXT("resource_budget_exceeded")));
	TestNull(TEXT("budget failure leaves no asset"), StaticFindObject(UObject::StaticClass(), nullptr, *BudgetTarget));

	const FString BoundaryRoot = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / TEXT("Automation/StrictV2/T13/source_failures"));
	IFileManager::Get().DeleteDirectory(*BoundaryRoot, false, true);
	const FString MissingTarget = TEXT("/Game/StrictV2/T13/DA_MissingManifest.DA_MissingManifest");
	const FString MissingRoot = FPaths::Combine(BoundaryRoot, TEXT("missing_manifest"));
	DeleteAssetIfPresent(MissingTarget);
	TestTrue(
		TEXT("missing-manifest source root exists"),
		IFileManager::Get().MakeDirectory(*MissingRoot, true));
	const FUeAgentCommandResult Missing = Submit(
		Registry,
		Dispatch,
		TEXT("content_graph.import_workspace"),
		FileImportParams(MissingTarget, TEXT("directory"), MissingRoot));
	TestTrue(
		TEXT("directory without its machine manifest is rejected before write"),
		!Missing.IsOk()
			&& Missing.GetErrorJson().Contains(
				TEXT("content_graph_workspace_manifest_missing")));
	TestNull(
		TEXT("missing-manifest failure leaves no asset"),
		StaticFindObject(UObject::StaticClass(), nullptr, *MissingTarget));

	const FString DigestTarget = TEXT("/Game/StrictV2/T13/DA_DigestMismatch.DA_DigestMismatch");
	const FString DigestRoot = FPaths::Combine(BoundaryRoot, TEXT("digest_mismatch"));
	DeleteAssetIfPresent(DigestTarget);
	TestTrue(
		TEXT("digest-mismatch document persists"),
		WriteText(FPaths::Combine(DigestRoot, TEXT("asset.hjson")), RecursiveDocument()));
	TestTrue(
		TEXT("digest-mismatch manifest persists"),
		WriteDirectoryManifest(
			DigestRoot,
			DigestTarget,
			TEXT("asset.hjson"),
			TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000")));
	const FUeAgentCommandResult Digest = Submit(
		Registry,
		Dispatch,
		TEXT("content_graph.import_workspace"),
		FileImportParams(DigestTarget, TEXT("directory"), DigestRoot));
	TestTrue(
		TEXT("directory digest mismatch is rejected before write"),
		!Digest.IsOk()
			&& Digest.GetErrorJson().Contains(
				TEXT("content_graph_workspace_digest_mismatch")));
	TestNull(
		TEXT("digest mismatch leaves no asset"),
		StaticFindObject(UObject::StaticClass(), nullptr, *DigestTarget));

	const FString EscapeTarget = TEXT("/Game/StrictV2/T13/DA_PathEscape.DA_PathEscape");
	const FString EscapeRoot = FPaths::Combine(BoundaryRoot, TEXT("path_escape"));
	DeleteAssetIfPresent(EscapeTarget);
	TestTrue(
		TEXT("path-escape manifest persists"),
		WriteDirectoryManifest(
			EscapeRoot,
			EscapeTarget,
			TEXT("../outside.hjson"),
			Sha256Text(RecursiveDocument())));
	const FUeAgentCommandResult Escape = Submit(
		Registry,
		Dispatch,
		TEXT("content_graph.import_workspace"),
		FileImportParams(EscapeTarget, TEXT("directory"), EscapeRoot));
	TestTrue(
		TEXT("directory path escape is rejected before write"),
		!Escape.IsOk()
			&& Escape.GetErrorJson().Contains(TEXT("workspace_path_invalid")));
	TestNull(
		TEXT("path escape leaves no asset"),
		StaticFindObject(UObject::StaticClass(), nullptr, *EscapeTarget));

	const FString RollbackTarget = TEXT("/Game/StrictV2/T13/DA_Rollback.DA_Rollback");
	DeleteAssetIfPresent(RollbackTarget);
	{
		UE::UAI::OperationOwners::UObjectDataAsset::TestHooks::
			FScopedFailureAfterNode Failure(TEXT("deep"));
		const FUeAgentCommandResult RolledBack = Submit(
			Registry, Dispatch, TEXT("content_graph.import_workspace"),
			ImportParams(RollbackTarget, RecursiveDocument()));
		TestTrue(TEXT("child creation failure is surfaced"), !RolledBack.IsOk() && RolledBack.GetErrorJson().Contains(TEXT("uobject_dataasset_child_create_failed")));
		TestTrue(TEXT("child failure reaches rolled-back terminal"), RolledBack.GetErrorJson().Contains(TEXT("rolled_back")));
	}
	TestNull(TEXT("confirmed rollback removes the root asset"), StaticFindObject(UObject::StaticClass(), nullptr, *RollbackTarget));
	IFileManager::Get().DeleteDirectory(*BoundaryRoot, false, true);
	return true;
#endif
}
