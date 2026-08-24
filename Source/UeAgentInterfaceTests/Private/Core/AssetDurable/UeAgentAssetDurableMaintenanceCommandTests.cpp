#include "Misc/AutomationTest.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandHandlers/AssetDurable/UeAgentAssetDurableMaintenanceHandlers.h"
#include "Core/Contract/UeAgentContract.h"
#include "Module/CommandRegistration/AssetDurable/UeAgentAssetDurableMaintenanceRegistration.h"

namespace
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandHandlers::AssetDurable;
	using namespace UE::UAI::CommandRegistry;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Module::CommandRegistration::AssetDurable;

	class FNeverCancelled final : public IUeAgentCancellationSignal
	{
	public:
		virtual bool IsCancelled() const override
		{
			return false;
		}
	};

	class FRecordingMaintenancePort final
		: public IUeAgentAssetDurableMaintenancePort
	{
	public:
		virtual bool TryInspect(
			const FString& JournalId,
			UE::UAI::ContextResource::FUeAgentResourceTracker& Resources,
			UE::UAI::AssetDurable::FAssetDurableSessionPtr& OutSession,
			TArray<UE::UAI::AssetDurable::FDurableArtifactState>&
				OutObservedStates,
			UE::UAI::AssetDurable::FAssetDurableError& OutError) override
		{
			(void)Resources;
			OutSession.Reset();
			OutObservedStates.Reset();
			LastJournalId = JournalId;
			++InspectCount;
			OutError = UE::UAI::AssetDurable::FAssetDurableError(
				TEXT("fixture_durable_read_failed"),
				JournalId);
			return false;
		}

		virtual bool TryReconcile(
			const FString& JournalId,
			const FString& ExpectedJournalOwnerId,
			const int64 ExpectedFencingToken,
			const FString& VerificationId,
			UE::UAI::ContextResource::FUeAgentResourceTracker& Resources,
			UE::UAI::AssetDurable::FDurableReceipt& OutReceipt,
			UE::UAI::AssetDurable::FAssetDurableError& OutError) override
		{
			(void)Resources;
			OutReceipt = UE::UAI::AssetDurable::FDurableReceipt();
			LastJournalId = JournalId;
			LastOwnerId = ExpectedJournalOwnerId;
			LastFencingToken = ExpectedFencingToken;
			LastVerificationId = VerificationId;
			++ReconcileCount;
			OutError = UE::UAI::AssetDurable::FAssetDurableError(
				TEXT("fixture_durable_reconcile_failed"),
				JournalId);
			return false;
		}

		int32 InspectCount = 0;
		int32 ReconcileCount = 0;
		FString LastJournalId;
		FString LastOwnerId;
		int64 LastFencingToken = -1;
		FString LastVerificationId;
	};

	bool DecodeRequest(
		const FString& CommandId,
		const FString& ContractHash,
		const FString& ParamsJson,
		TOptional<FUeAgentCommandRequest>& OutRequest)
	{
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"corr-t63\",")
			TEXT("\"request_id\":\"req-t63\",")
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
			Bytes, OutRequest, Diagnostic);
	}

	FUeAgentCommandResult Submit(
		const TSharedRef<const FUeAgentExactCommandDispatch>& Dispatch,
		const FUeAgentFrozenCommandContract& Command,
		const FString& ParamsJson,
		TArray<FString> Scopes)
	{
		TOptional<FUeAgentCommandRequest> Request;
		if (!DecodeRequest(
				Command.GetCommandId(),
				Command.GetContractHash(),
				ParamsJson,
				Request)
			|| !Request.IsSet())
		{
			return FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t63"),
				TEXT("req-t63"),
				Command.GetCommandId(),
				Command.GetContractHash(),
				TEXT("test_request_decode_failed"),
				TEXT("T63 fixture request could not be decoded"));
		}
		return Dispatch->Submit(
			Request.GetValue(),
			FUeAgentCallerContext(
				TEXT("t63-maintenance-test"),
				MoveTemp(Scopes),
				TEXT("corr-t63")),
			MakeShared<FNeverCancelled>());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetDurableMaintenanceCommandContractTest,
	"UeAgentInterface.Core.AssetDurable.MaintenanceCommandContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetDurableMaintenanceCommandContractTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const TSharedRef<FRecordingMaintenancePort> Port =
		MakeShared<FRecordingMaintenancePort>();
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(
			TEXT("maintenance contribution composes"),
			TryCreateAssetDurableMaintenanceRegistration(
				Port, Registry, Dispatch, Error))
		|| !Registry.IsValid()
		|| !Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}
	TestEqual(
		TEXT("one owner declares exactly read and reconcile"),
		Registry->GetContracts().Num(),
		2);
	const FUeAgentFrozenCommandContract* Read =
		Registry->FindExact(TEXT("asset_durable.journal.read"));
	const FUeAgentFrozenCommandContract* Reconcile =
		Registry->FindExact(TEXT("asset_durable.journal.reconcile"));
	if (!TestNotNull(TEXT("read contract is registered"), Read)
		|| !TestNotNull(TEXT("reconcile contract is registered"), Reconcile))
	{
		return false;
	}
	TestEqual(
		TEXT("both commands share one public owner"),
		Read->GetPublicOwner(),
		Reconcile->GetPublicOwner());
	TestEqual(
		TEXT("read is side-effect free"),
		Read->GetSideEffect(),
		EUeAgentCommandSideEffect::None);
	TestEqual(
		TEXT("reconcile owns asset journal state"),
		Reconcile->GetSideEffect(),
		EUeAgentCommandSideEffect::AssetState);
	TestEqual(
		TEXT("generated SDK namespace is stable"),
		Read->GetPythonNamespace(),
		FString(TEXT("asset_durable")));

	const FString JournalId =
		TEXT("0123456789ABCDEF0123456789ABCDEF");
	FUeAgentCommandResult Result = Submit(
		Dispatch.ToSharedRef(),
		*Read,
		FString::Printf(TEXT("{\"journal_id\":\"%s\"}"), *JournalId),
		{});
	TestFalse(TEXT("read requires query authority"), Result.IsOk());
	TestTrue(
		TEXT("read authority failure is typed"),
		Result.GetErrorJson().Contains(
			TEXT("asset_durable_query_authority_required")));
	TestEqual(TEXT("unauthorized read never reaches the port"), Port->InspectCount, 0);

	Result = Submit(
		Dispatch.ToSharedRef(),
		*Read,
		FString::Printf(TEXT("{\"journal_id\":\"%s\"}"), *JournalId),
		{TEXT("uai:query")});
	TestFalse(TEXT("fixture read error remains a command failure"), Result.IsOk());
	TestTrue(
		TEXT("read preserves the domain error code"),
		Result.GetErrorJson().Contains(TEXT("fixture_durable_read_failed")));
	TestEqual(TEXT("authorized read reaches the port once"), Port->InspectCount, 1);
	TestEqual(TEXT("read preserves the journal identity"), Port->LastJournalId, JournalId);

	const FString ReconcileParams = FString::Printf(
		TEXT("{\"journal_id\":\"%s\",")
		TEXT("\"expected_journal_owner_id\":\"owner:stable\",")
		TEXT("\"expected_fencing_token\":73,")
		TEXT("\"verification_id\":\"verification:t63\"}"),
		*JournalId);
	Result = Submit(
		Dispatch.ToSharedRef(),
		*Reconcile,
		ReconcileParams,
		{TEXT("uai:query")});
	TestFalse(TEXT("reconcile requires operate authority"), Result.IsOk());
	TestEqual(
		TEXT("unauthorized reconcile never reaches the port"),
		Port->ReconcileCount,
		0);

	Result = Submit(
		Dispatch.ToSharedRef(),
		*Reconcile,
		ReconcileParams,
		{TEXT("uai:operate")});
	TestFalse(
		TEXT("fixture reconcile error remains a command failure"),
		Result.IsOk());
	TestTrue(
		TEXT("reconcile preserves the domain error code"),
		Result.GetErrorJson().Contains(
			TEXT("fixture_durable_reconcile_failed")));
	TestEqual(TEXT("authorized reconcile reaches the port once"), Port->ReconcileCount, 1);
	TestEqual(TEXT("reconcile preserves journal"), Port->LastJournalId, JournalId);
	TestEqual(TEXT("reconcile preserves owner"), Port->LastOwnerId, FString(TEXT("owner:stable")));
	TestEqual(TEXT("reconcile preserves fence"), Port->LastFencingToken, static_cast<int64>(73));
	TestEqual(
		TEXT("reconcile preserves verification identity"),
		Port->LastVerificationId,
		FString(TEXT("verification:t63")));
	return true;
}
