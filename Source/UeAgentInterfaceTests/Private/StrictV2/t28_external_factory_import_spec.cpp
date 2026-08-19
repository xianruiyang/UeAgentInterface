#include "Misc/AutomationTest.h"

#if __has_include("DomainAdapters/ExternalImport/Factory/UeAgentFactoryExternalImportProvider.h") \
	&& __has_include("OperationOwners/ExternalImport/Factory/UeAgentFactoryExternalImportCommandService.h") \
	&& __has_include("Module/CommandRegistration/ExternalImport/Factory/UeAgentFactoryExternalImportRegistration.h")
#define UAI_T28_EXTERNAL_FACTORY_IMPORT_AVAILABLE 1
#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/Base64.h"
#include "Misc/Paths.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "Module/CommandRegistration/ExternalImport/Factory/UeAgentFactoryExternalImportRegistration.h"
#include "ObjectTools.h"
#include "OperationOwners/ExternalImport/Factory/UeAgentFactoryExternalImportCommandService.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#else
#define UAI_T28_EXTERNAL_FACTORY_IMPORT_AVAILABLE 0
#endif

#if UAI_T28_EXTERNAL_FACTORY_IMPORT_AVAILABLE
namespace
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandRegistry;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Module::CommandRegistration::ExternalImport::Factory;
	using namespace UE::UAI::OperationOwners::ExternalImport::Factory;

	const TArray<FString> ExpectedCommandIds = {
		TEXT("asset.reimport_sound_wave"),
		TEXT("asset.reimport_sparse_volume_texture"),
		TEXT("asset.reimport_static_mesh"),
		TEXT("asset.reimport_texture"),
		TEXT("external.fbx.animation.import"),
		TEXT("external.fbx.skeletal_mesh.import"),
		TEXT("external.geometry_cache.import"),
		TEXT("external.light_profile.import"),
		TEXT("external.sound_wave.import"),
		TEXT("external.sparse_volume_texture.import"),
		TEXT("external.texture.import"),
	};

	class FNeverCancelled final : public IUeAgentCancellationSignal
	{
	public:
		virtual bool IsCancelled() const override { return false; }
	};

	class FFixtureService final
		: public IUeAgentFactoryExternalImportCommandService
	{
	public:
		virtual bool Execute(
			const FUeAgentFactoryExternalImportCommandRequest& Request,
			FUeAgentFactoryExternalImportCommandResponse& OutResponse,
			FUeAgentFactoryExternalImportCommandError& OutError) override
		{
			++Calls;
			LastRequest = Request;
			OutResponse = {};
			OutError = {};
			if (bFail)
			{
				OutError.Code = TEXT("fixture_t28_failed");
				OutError.SubjectId = Request.CommandId;
				OutError.Detail = TEXT("fixture failure remains typed");
				OutError.TerminalStatus = TEXT("rejected");
				return false;
			}
			OutResponse.PayloadJson = FString::Printf(
				TEXT("{\"command_id\":\"%s\",\"independent_readback\":{},")
				TEXT("\"mutation_started\":true,\"persistence_receipt_id\":\"fixture\",")
				TEXT("\"phase_history\":[\"planning\",\"terminal\"],")
				TEXT("\"terminal_status\":\"succeeded\"}"),
				*Request.CommandId);
			return true;
		}

		int32 Calls = 0;
		FUeAgentFactoryExternalImportCommandRequest LastRequest;
		bool bFail = false;
	};

	struct FFixtureRegistration final
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
			TEXT("T28 contribution composes in isolation"),
			TryCreateFactoryExternalImportRegistration(
				Fixture.Service,
				Fixture.Registry,
				Fixture.Dispatch,
				Fixture.Error));
		Test->TestTrue(TEXT("T28 immutable registry exists"), Fixture.Registry.IsValid());
		Test->TestTrue(TEXT("T28 exact dispatch exists"), Fixture.Dispatch.IsValid());
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
			TEXT("\"correlation_id\":\"corr-t28\",")
			TEXT("\"request_id\":\"req-t28\",")
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
		TArray<FString> Authorities = {TEXT("uai:operate")})
	{
		const auto* Contract = Registry.IsValid()
			? Registry->FindExact(CommandId)
			: nullptr;
		if (!Contract || !Dispatch.IsValid())
		{
			return FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t28"), TEXT("req-t28"), CommandId,
				TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
				TEXT("fixture_contract_missing"),
				TEXT("T28 fixture could not find the exact contract"));
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
				TEXT("corr-t28"), TEXT("req-t28"), CommandId,
				Contract->GetContractHash(),
				TEXT("fixture_request_decode_failed"),
				TEXT("T28 fixture could not decode its request"));
		}
		return Dispatch->Submit(
			Request.GetValue(),
			FUeAgentCallerContext(
				TEXT("strict-v2-t28"),
				MoveTemp(Authorities),
				TEXT("corr-t28")),
			MakeShared<FNeverCancelled>());
	}

	FString CanonicalParams(const TSharedRef<FJsonObject>& Params)
	{
		FString Result;
		FString Error;
		return Json::TryCanonicalize(Params, Result, Error)
			? Result
			: FString(TEXT("{}"));
	}

	FString ImportParams(
		const FString& Source,
		const FString& Destination,
		const TSharedPtr<FJsonObject>& Options = MakeShared<FJsonObject>())
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("source_ref"), Source);
		Params->SetStringField(TEXT("destination_ref"), Destination);
		Params->SetObjectField(TEXT("options"), Options);
		return CanonicalParams(Params);
	}

	FString ReimportParams(
		const FString& Target,
		const FString& Source,
		const TSharedPtr<FJsonObject>& Options = MakeShared<FJsonObject>())
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target_ref"), Target);
		Params->SetStringField(TEXT("source_ref"), Source);
		Params->SetObjectField(TEXT("options"), Options);
		return CanonicalParams(Params);
	}

	TSharedPtr<FJsonObject> ParsePayload(
		FAutomationTestBase* Test,
		const FUeAgentCommandResult& Result)
	{
		TSharedPtr<FJsonObject> Payload;
		FString Error;
		Test->TestTrue(
			TEXT("T28 response payload is canonical JSON"),
			Json::TryParseObject(Result.GetPayloadJson(), Payload, Error));
		return Payload;
	}

	bool CreatePng(const FString& Filename)
	{
		TArray<uint8> Bytes;
		return FBase64::Decode(
				TEXT("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="),
				Bytes)
			&& FFileHelper::SaveArrayToFile(Bytes, *Filename);
	}

	void DeleteAssetIfPresent(const FString& ObjectPath)
	{
		if (UObject* Existing = StaticLoadObject(
				UObject::StaticClass(), nullptr, *ObjectPath))
		{
			ObjectTools::DeleteObjectsUnchecked({Existing});
		}
	}

	FString MissingParams(const FString& CommandId, const int32 Index)
	{
		const FString Source = FPaths::ProjectIntermediateDir()
			/ TEXT("UAI/StrictV2/T28/missing_")
			+ FString::FromInt(Index) + TEXT(".missing");
		const FString Target = FString::Printf(
			TEXT("/Game/StrictV2/T28/Missing_%02d"), Index);
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("source_ref"), Source);
		if (CommandId.StartsWith(TEXT("asset.reimport_")))
		{
			Params->SetStringField(TEXT("target_ref"), Target + TEXT(".Missing"));
		}
		else
		{
			Params->SetStringField(TEXT("destination_ref"), Target);
			if (CommandId == TEXT("external.fbx.animation.import"))
			{
				Params->SetStringField(
					TEXT("skeleton_ref"),
					TEXT("/Engine/EngineMeshes/SkeletalCube_Skeleton.SkeletalCube_Skeleton"));
			}
		}
		Params->SetObjectField(TEXT("options"), MakeShared<FJsonObject>());
		return CanonicalParams(Params);
	}
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T28InventoryAndLegacyExitTest,
	"UAI.StrictV2.T28.EXTERNAL.FACTORY.IMPORT.InventoryAndLegacyExit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T28InventoryAndLegacyExitTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T28_EXTERNAL_FACTORY_IMPORT_AVAILABLE
	TestTrue(TEXT("T28 factory import owner is implemented"), false);
	return false;
#else
	TArray<FString> Actual;
	for (const FUeAgentFactoryExternalImportCommandRoute& Route
		: GetFactoryExternalImportCommandRoutes())
	{
		Actual.Add(Route.CommandId);
		TestTrue(TEXT("every T28 route is valid"), Route.IsValid());
	}
	Actual.Sort();
	TestEqual(TEXT("T28 owns exactly the audited eleven commands"), Actual, ExpectedCommandIds);
	TestEqual(TEXT("T28 route identities remain unique"), TSet<FString>(Actual).Num(), 11);

	TSharedPtr<const FUeAgentImmutableCommandRegistry> ProcessRegistry;
	TSharedPtr<const FUeAgentExactCommandDispatch> ProcessDispatch;
	FString Error;
	TestTrue(
		TEXT("process active composition includes T28"),
		TryComposeProcessActiveCommandComposition(
			ProcessRegistry,
			ProcessDispatch,
			Error));
	for (const FString& CommandId : ExpectedCommandIds)
	{
		const auto* Contract = ProcessRegistry.IsValid()
			? ProcessRegistry->FindExact(CommandId)
			: nullptr;
		TestNotNull(TEXT("every T28 command has one active exact contract"), Contract);
		if (Contract)
		{
			TestEqual(
				TEXT("the active contract publishes the unique T28 owner"),
				Contract->GetPublicOwner(),
				FString(TEXT("factory-external-import-owner")));
		}
	}
	return !HasAnyErrors();
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T28ExactDispatchTest,
	"UAI.StrictV2.T28.EXTERNAL.FACTORY.IMPORT.ExactDispatchAndTypedFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T28ExactDispatchTest::RunTest(const FString& Parameters)
{
#if !UAI_T28_EXTERNAL_FACTORY_IMPORT_AVAILABLE
	TestTrue(TEXT("T28 exact public dispatch is implemented"), false);
	return false;
#else
	FFixtureRegistration Fixture = MakeRegistration(this);
	if (!Fixture.Registry.IsValid() || !Fixture.Dispatch.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("isolated T28 registry contains eleven contracts"), Fixture.Registry->GetContracts().Num(), 11);
	const FUeAgentCommandResult Imported = Submit(
		Fixture.Registry,
		Fixture.Dispatch,
		TEXT("external.texture.import"),
		TEXT("{\"destination_ref\":\"/Game/StrictV2/T28/Fixture\",\"options\":{},\"source_ref\":\"D:/fixture.png\"}"));
	TestTrue(TEXT("exact import route delegates successfully"), Imported.IsOk());
	TestEqual(TEXT("one request reaches one service call"), Fixture.Service->Calls, 1);
	TestEqual(
		TEXT("service receives the unchanged exact command identity"),
		Fixture.Service->LastRequest.CommandId,
		FString(TEXT("external.texture.import")));

	const FUeAgentCommandResult Unauthorized = Submit(
		Fixture.Registry,
		Fixture.Dispatch,
		TEXT("asset.reimport_texture"),
		TEXT("{}"),
		{});
	TestTrue(TEXT("missing operation authority is a business failure"), !Unauthorized.IsOk() && !Unauthorized.IsProtocolError());
	TestTrue(TEXT("authority failure remains typed"), Unauthorized.GetErrorJson().Contains(TEXT("factory_import_authority_required")));
	TestEqual(TEXT("unauthorized request never reaches the owner"), Fixture.Service->Calls, 1);

	Fixture.Service->bFail = true;
	const FUeAgentCommandResult Failure = Submit(
		Fixture.Registry,
		Fixture.Dispatch,
		TEXT("external.sound_wave.import"),
		TEXT("{\"destination_ref\":\"/Game/StrictV2/T28/FixtureSound\",\"options\":{},\"source_ref\":\"D:/fixture.wav\"}"));
	TestTrue(TEXT("owner failure remains a business failure"), !Failure.IsOk() && !Failure.IsProtocolError());
	TestTrue(TEXT("owner code is preserved"), Failure.GetErrorJson().Contains(TEXT("fixture_t28_failed")));
	return !HasAnyErrors();
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T28ProductionRoundTripTest,
	"UAI.StrictV2.T28.EXTERNAL.FACTORY.IMPORT.ProductionPublicPersistReloadReimport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T28ProductionRoundTripTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T28_EXTERNAL_FACTORY_IMPORT_AVAILABLE
	TestTrue(TEXT("T28 production factory import is implemented"), false);
	return false;
#else
	TestTrue(TEXT("production factory mechanics run on the game thread"), IsInGameThread());
	const FString ObjectPath = TEXT("/Game/StrictV2/T28/OwnedTexture.OwnedTexture");
	const FString PackageName = TEXT("/Game/StrictV2/T28/OwnedTexture");
	const FString SourceRoot = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectIntermediateDir() / TEXT("UAI/StrictV2/T28"));
	const FString Source = SourceRoot / TEXT("OwnedTexture.png");
	DeleteAssetIfPresent(ObjectPath);
	IFileManager::Get().MakeDirectory(*SourceRoot, true);
	if (!TestTrue(TEXT("owned PNG source is created"), CreatePng(Source)))
	{
		return false;
	}

	TSharedPtr<IUeAgentFactoryExternalImportCommandService> Service;
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	TestTrue(
		TEXT("production T28 service composes"),
		TryCreateProductionFactoryExternalImportCommandService(Service, Error));
	TestTrue(
		TEXT("production T28 contribution composes"),
		Service.IsValid()
			&& TryCreateFactoryExternalImportRegistration(
				Service.ToSharedRef(), Registry, Dispatch, Error));
	if (!Registry.IsValid() || !Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}

	const FUeAgentCommandResult Imported = Submit(
		Registry,
		Dispatch,
		TEXT("external.texture.import"),
		ImportParams(Source, PackageName));
	TestTrue(TEXT("public texture import succeeds"), Imported.IsOk());
	if (!Imported.IsOk())
	{
		AddError(Imported.GetErrorJson());
		DeleteAssetIfPresent(ObjectPath);
		return false;
	}
	const TSharedPtr<FJsonObject> ImportPayload = ParsePayload(this, Imported);
	if (ImportPayload.IsValid())
	{
		TestEqual(TEXT("import reaches a succeeded terminal"), ImportPayload->GetStringField(TEXT("terminal_status")), FString(TEXT("succeeded")));
		TestTrue(TEXT("import reports mutation"), ImportPayload->GetBoolField(TEXT("mutation_started")));
		TestTrue(TEXT("persistence receipt is present"), !ImportPayload->GetStringField(TEXT("persistence_receipt_id")).IsEmpty());
		const TSharedPtr<FJsonObject>* Readback = nullptr;
		TestTrue(TEXT("independent readback is returned"), ImportPayload->TryGetObjectField(TEXT("independent_readback"), Readback));
		if (Readback && Readback->IsValid())
		{
			TestTrue(TEXT("asset was unloaded and reloaded"), (*Readback)->GetBoolField(TEXT("reloaded")));
			TestTrue(TEXT("source provenance survives reload"), !(*Readback)->GetStringField(TEXT("source_ref")).IsEmpty());
			TestTrue(TEXT("source hash is reported"), (*Readback)->GetStringField(TEXT("source_sha256")).StartsWith(TEXT("sha256:")));
			TestTrue(TEXT("real ImportData is read back"), !(*Readback)->GetStringField(TEXT("import_data_class")).IsEmpty());
			TestTrue(TEXT("applied options are fingerprinted"), (*Readback)->GetStringField(TEXT("options_fingerprint")).StartsWith(TEXT("sha256:")));
		}
	}

	const TSharedRef<FJsonObject> ReimportOptions = MakeShared<FJsonObject>();
	ReimportOptions->SetBoolField(TEXT("replace_existing"), true);
	const FUeAgentCommandResult Reimported = Submit(
		Registry,
		Dispatch,
		TEXT("asset.reimport_texture"),
		ReimportParams(ObjectPath, Source, ReimportOptions));
	TestTrue(TEXT("public texture reimport succeeds"), Reimported.IsOk());
	if (!Reimported.IsOk())
	{
		AddError(Reimported.GetErrorJson());
	}
	else
	{
		const TSharedPtr<FJsonObject> ReimportPayload = ParsePayload(this, Reimported);
		if (ReimportPayload.IsValid())
		{
			const TSharedPtr<FJsonObject>* Readback = nullptr;
			TestTrue(TEXT("reimport returns independent readback"), ReimportPayload->TryGetObjectField(TEXT("independent_readback"), Readback));
			if (Readback && Readback->IsValid())
			{
				TestEqual(TEXT("readback identifies reimport mechanics"), (*Readback)->GetStringField(TEXT("operation")), FString(TEXT("reimport")));
				TestTrue(TEXT("reimport reloads persisted content"), (*Readback)->GetBoolField(TEXT("reloaded")));
			}
		}
	}

	const FString ConfiguredObjectPath =
		TEXT("/Game/StrictV2/T28/ConfiguredTexture.ConfiguredTexture");
	const FString ConfiguredPackageName =
		TEXT("/Game/StrictV2/T28/ConfiguredTexture");
	const FString ConfiguredSource = SourceRoot / TEXT("ConfiguredTexture.png");
	DeleteAssetIfPresent(ConfiguredObjectPath);
	TestTrue(TEXT("configured PNG source is created"), CreatePng(ConfiguredSource));
	const TSharedRef<FJsonObject> TextureOptions = MakeShared<FJsonObject>();
	TextureOptions->SetBoolField(TEXT("srgb"), false);
	TextureOptions->SetStringField(TEXT("compression_settings"), TEXT("TC_Masks"));
	TextureOptions->SetStringField(TEXT("mip_gen_settings"), TEXT("TMGS_NoMipmaps"));
	TextureOptions->SetStringField(TEXT("lod_group"), TEXT("TEXTUREGROUP_UI"));
	const FUeAgentCommandResult Configured = Submit(
		Registry,
		Dispatch,
		TEXT("external.texture.import"),
		ImportParams(ConfiguredSource, ConfiguredPackageName, TextureOptions));
	TestTrue(TEXT("historical texture options survive the unified import entry"), Configured.IsOk());
	if (Configured.IsOk())
	{
		const TSharedPtr<FJsonObject> Payload = ParsePayload(this, Configured);
		const TSharedPtr<FJsonObject>* Readback = nullptr;
		TestTrue(
			TEXT("configured texture returns independent readback"),
			Payload.IsValid()
				&& Payload->TryGetObjectField(TEXT("independent_readback"), Readback));
		if (Readback && Readback->IsValid())
		{
			TestFalse(TEXT("configured SRGB persists"), (*Readback)->GetBoolField(TEXT("srgb")));
			TestEqual(TEXT("configured compression persists"), (*Readback)->GetStringField(TEXT("compression_settings")), FString(TEXT("TC_Masks")));
			TestEqual(TEXT("configured mip policy persists"), (*Readback)->GetStringField(TEXT("mip_gen_settings")), FString(TEXT("TMGS_NoMipmaps")));
			TestEqual(TEXT("configured LOD group persists"), (*Readback)->GetStringField(TEXT("lod_group")), FString(TEXT("TEXTUREGROUP_UI")));
		}
	}

	DeleteAssetIfPresent(ObjectPath);
	DeleteAssetIfPresent(ConfiguredObjectPath);
	IFileManager::Get().DeleteDirectory(*SourceRoot, false, true);
	return !HasAnyErrors();
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T28FailureCleanupTest,
	"UAI.StrictV2.T28.EXTERNAL.FACTORY.IMPORT.PrewriteRollbackAndCleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T28FailureCleanupTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T28_EXTERNAL_FACTORY_IMPORT_AVAILABLE
	TestTrue(TEXT("T28 failure terminals and cleanup are implemented"), false);
	return false;
#else
	TSharedPtr<IUeAgentFactoryExternalImportCommandService> Service;
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	TestTrue(TEXT("production service composes"), TryCreateProductionFactoryExternalImportCommandService(Service, Error));
	TestTrue(TEXT("production registration composes"), Service.IsValid() && TryCreateFactoryExternalImportRegistration(Service.ToSharedRef(), Registry, Dispatch, Error));
	if (!Registry.IsValid() || !Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}

	for (int32 Index = 0; Index < ExpectedCommandIds.Num(); ++Index)
	{
		const FString& CommandId = ExpectedCommandIds[Index];
		const FUeAgentCommandResult Missing = Submit(
			Registry,
			Dispatch,
			CommandId,
			MissingParams(CommandId, Index));
		TestTrue(
			TEXT("missing source is a typed prewrite failure: ") + CommandId,
			!Missing.IsOk()
				&& !Missing.IsProtocolError()
				&& Missing.GetErrorJson().Contains(TEXT("factory_import_source_not_found"))
				&& Missing.GetErrorJson().Contains(TEXT("rejected")));
	}

	const FString ObjectPath = TEXT("/Game/StrictV2/T28/RollbackTexture.RollbackTexture");
	const FString PackageName = TEXT("/Game/StrictV2/T28/RollbackTexture");
	const FString SourceRoot = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectIntermediateDir() / TEXT("UAI/StrictV2/T28Rollback"));
	const FString Source = SourceRoot / TEXT("RollbackTexture.png");
	DeleteAssetIfPresent(ObjectPath);
	IFileManager::Get().MakeDirectory(*SourceRoot, true);
	TestTrue(TEXT("rollback PNG source is created"), CreatePng(Source));
	{
		TestHooks::FScopedFactoryImportFailureAtStage Failure(TEXT("persist"));
		const FUeAgentCommandResult RolledBack = Submit(
			Registry,
			Dispatch,
			TEXT("external.texture.import"),
			ImportParams(Source, PackageName));
		TestTrue(
			TEXT("post-mutation failure reports a rolled-back terminal"),
			!RolledBack.IsOk()
				&& RolledBack.GetErrorJson().Contains(TEXT("factory_import_test_failure"))
				&& RolledBack.GetErrorJson().Contains(TEXT("rolled_back")));
	}
	TestNull(TEXT("rolled-back import leaves no loaded asset"), StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath));
	FString PackageFilename;
	TestFalse(TEXT("rolled-back import leaves no package"), FPackageName::DoesPackageExist(PackageName, &PackageFilename));

	const TSharedRef<FJsonObject> UnknownOptions = MakeShared<FJsonObject>();
	UnknownOptions->SetBoolField(TEXT("unsupported_option"), true);
	const FUeAgentCommandResult UnknownOption = Submit(
		Registry,
		Dispatch,
		TEXT("external.texture.import"),
		ImportParams(Source, PackageName, UnknownOptions));
	TestTrue(
		TEXT("unknown options fail closed before mutation"),
		!UnknownOption.IsOk()
			&& UnknownOption.GetErrorJson().Contains(TEXT("factory_import_option_unknown")));
	TestNull(TEXT("unknown option leaves no loaded asset"), StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath));

	DeleteAssetIfPresent(ObjectPath);
	IFileManager::Get().DeleteDirectory(*SourceRoot, false, true);
	return !HasAnyErrors();
#endif
}
