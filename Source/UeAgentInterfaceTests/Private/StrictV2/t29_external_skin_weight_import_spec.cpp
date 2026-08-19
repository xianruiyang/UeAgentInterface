#include "Misc/AutomationTest.h"

#if __has_include("DomainAdapters/ExternalImport/SkinWeight/UeAgentSkinWeightImportProvider.h") \
	&& __has_include("OperationOwners/ExternalImport/SkinWeight/UeAgentSkinWeightImportCommandService.h") \
	&& __has_include("Module/CommandRegistration/ExternalImport/SkinWeight/UeAgentSkinWeightImportRegistration.h")
#define UAI_T29_EXTERNAL_SKIN_WEIGHT_IMPORT_AVAILABLE 1
#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Misc/Paths.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "Module/CommandRegistration/ExternalImport/SkinWeight/UeAgentSkinWeightImportRegistration.h"
#include "OperationOwners/ExternalImport/SkinWeight/UeAgentSkinWeightImportCommandService.h"
#else
#define UAI_T29_EXTERNAL_SKIN_WEIGHT_IMPORT_AVAILABLE 0
#endif

#if UAI_T29_EXTERNAL_SKIN_WEIGHT_IMPORT_AVAILABLE
namespace
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandRegistry;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Module::CommandRegistration::ExternalImport::SkinWeight;
	using namespace UE::UAI::OperationOwners::ExternalImport::SkinWeight;

	const FString ExpectedCommandId =
		TEXT("external.skin_weight_profile.import");

	class FNeverCancelled final : public IUeAgentCancellationSignal
	{
	public:
		virtual bool IsCancelled() const override { return false; }
	};

	class FFixtureService final : public IUeAgentSkinWeightImportCommandService
	{
	public:
		virtual bool Execute(
			const FUeAgentSkinWeightImportCommandRequest& Request,
			FUeAgentSkinWeightImportCommandResponse& OutResponse,
			FUeAgentSkinWeightImportCommandError& OutError) override
		{
			++Calls;
			LastRequest = Request;
			OutResponse = {};
			OutError = {};
			if (bFail)
			{
				OutError.Code = TEXT("fixture_t29_failed");
				OutError.SubjectId = Request.CommandId;
				OutError.Detail = TEXT("fixture failure remains typed");
				OutError.TerminalStatus = TEXT("rejected");
				return false;
			}
			OutResponse.PayloadJson =
				TEXT("{\"command_id\":\"external.skin_weight_profile.import\",")
				TEXT("\"independent_readback\":{},\"mutation_started\":true,")
				TEXT("\"persistence_receipt_id\":\"fixture\",")
				TEXT("\"phase_history\":[\"planning\",\"terminal\"],")
				TEXT("\"terminal_status\":\"succeeded\"}");
			return true;
		}

		int32 Calls = 0;
		bool bFail = false;
		FUeAgentSkinWeightImportCommandRequest LastRequest;
	};

	bool DecodeRequest(
		const FString& ContractHash,
		const FString& ParamsJson,
		TOptional<FUeAgentCommandRequest>& OutRequest)
	{
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"corr-t29\",")
			TEXT("\"request_id\":\"req-t29\",")
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
				TEXT("corr-t29"), TEXT("req-t29"), ExpectedCommandId,
				TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
				TEXT("fixture_contract_missing"),
				TEXT("T29 fixture could not find the exact contract"));
		}
		TOptional<FUeAgentCommandRequest> Request;
		if (!DecodeRequest(Contract->GetContractHash(), ParamsJson, Request)
			|| !Request.IsSet())
		{
			return FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t29"), TEXT("req-t29"), ExpectedCommandId,
				Contract->GetContractHash(),
				TEXT("fixture_request_decode_failed"),
				TEXT("T29 fixture could not decode its request"));
		}
		return Dispatch->Submit(
			Request.GetValue(),
			FUeAgentCallerContext(
				TEXT("strict-v2-t29"),
				MoveTemp(Authorities),
				TEXT("corr-t29")),
			MakeShared<FNeverCancelled>());
	}

	FString Params(
		const FString& Source,
		const FString& Target,
		const FString& ProfileName,
		const int32 LodIndex)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("source_ref"), Source);
		Json->SetStringField(TEXT("target_ref"), Target);
		Json->SetStringField(TEXT("profile_name"), ProfileName);
		Json->SetNumberField(TEXT("lod_index"), LodIndex);
		FString Result;
		FString Error;
		return Json::TryCanonicalize(Json, Result, Error)
			? Result
			: FString(TEXT("{}"));
	}
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T29InventoryAndLegacyExitTest,
	"UAI.StrictV2.T29.EXTERNAL.SKIN.WEIGHT.IMPORT.InventoryAndLegacyExit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T29InventoryAndLegacyExitTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T29_EXTERNAL_SKIN_WEIGHT_IMPORT_AVAILABLE
	TestTrue(TEXT("T29 skin-weight import owner is implemented"), false);
	return false;
#else
	const TArray<FUeAgentSkinWeightImportCommandRoute>& Routes =
		GetSkinWeightImportCommandRoutes();
	TestEqual(TEXT("T29 owns exactly one audited command"), Routes.Num(), 1);
	if (Routes.Num() == 1)
	{
		TestTrue(TEXT("the T29 route is valid"), Routes[0].IsValid());
		TestEqual(TEXT("the T29 route identity is exact"), Routes[0].CommandId, ExpectedCommandId);
	}

	TSharedPtr<const FUeAgentImmutableCommandRegistry> ProcessRegistry;
	TSharedPtr<const FUeAgentExactCommandDispatch> ProcessDispatch;
	FString Error;
	TestTrue(
		TEXT("process active composition includes T29"),
		TryComposeProcessActiveCommandComposition(
			ProcessRegistry,
			ProcessDispatch,
			Error));
	const auto* Contract = ProcessRegistry.IsValid()
		? ProcessRegistry->FindExact(ExpectedCommandId)
		: nullptr;
	TestNotNull(TEXT("the T29 command has one active exact contract"), Contract);
	if (Contract)
	{
		TestEqual(
			TEXT("the active contract publishes the unique T29 owner"),
			Contract->GetPublicOwner(),
			FString(TEXT("skin-weight-import-owner")));
	}
	return !HasAnyErrors();
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T29ExactDispatchTest,
	"UAI.StrictV2.T29.EXTERNAL.SKIN.WEIGHT.IMPORT.ExactDispatchAndTypedFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T29ExactDispatchTest::RunTest(const FString& Parameters)
{
#if !UAI_T29_EXTERNAL_SKIN_WEIGHT_IMPORT_AVAILABLE
	TestTrue(TEXT("T29 exact public dispatch is implemented"), false);
	return false;
#else
	const TSharedRef<FFixtureService> Service = MakeShared<FFixtureService>();
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	TestTrue(
		TEXT("T29 contribution composes in isolation"),
		TryCreateSkinWeightImportRegistration(
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
		TEXT("D:/fixture.fbx"),
		TEXT("/Game/StrictV2/T29/Fixture.Fixture"),
		TEXT("Alternate"),
		0);
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
	TestTrue(TEXT("authority code is stable"), Unauthorized.GetErrorJson().Contains(TEXT("skin_weight_import_authority_required")));
	TestEqual(TEXT("unauthorized input never reaches the owner"), Service->Calls, 1);

	Service->bFail = true;
	const FUeAgentCommandResult Failure = Submit(
		Registry,
		Dispatch,
		Params(
			TEXT("D:/fixture.fbx"),
			TEXT("/Game/StrictV2/T29/Fixture.Fixture"),
			TEXT("Alternate"),
			0));
	TestTrue(TEXT("owner failure remains a business failure"), !Failure.IsOk() && !Failure.IsProtocolError());
	TestTrue(TEXT("owner code is preserved"), Failure.GetErrorJson().Contains(TEXT("fixture_t29_failed")));
	return !HasAnyErrors();
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T29ProductionFailureCleanupTest,
	"UAI.StrictV2.T29.EXTERNAL.SKIN.WEIGHT.IMPORT.ProductionFailureAndCleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T29ProductionFailureCleanupTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T29_EXTERNAL_SKIN_WEIGHT_IMPORT_AVAILABLE
	TestTrue(TEXT("T29 production failure semantics are implemented"), false);
	return false;
#else
	TSharedPtr<IUeAgentSkinWeightImportCommandService> Service;
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	TestTrue(
		TEXT("production T29 service composes"),
		TryCreateProductionSkinWeightImportCommandService(Service, Error));
	TestTrue(
		TEXT("production T29 contribution composes"),
		Service.IsValid()
			&& TryCreateSkinWeightImportRegistration(
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
		/ TEXT("UAI/StrictV2/T29/missing.fbx");
	const FString MissingTarget =
		TEXT("/Game/StrictV2/T29/Missing.Missing");
	const FUeAgentCommandResult Missing = Submit(
		Registry,
		Dispatch,
		Params(MissingSource, MissingTarget, TEXT("Alternate"), 0));
	TestTrue(
		TEXT("missing source fails before mutation"),
		!Missing.IsOk()
			&& !Missing.IsProtocolError()
			&& Missing.GetErrorJson().Contains(TEXT("skin_weight_import_source_not_found"))
			&& Missing.GetErrorJson().Contains(TEXT("rejected")));
	TestNull(
		TEXT("prewrite failure creates no target"),
		StaticFindObject(UObject::StaticClass(), nullptr, *MissingTarget));
	return !HasAnyErrors();
#endif
}
