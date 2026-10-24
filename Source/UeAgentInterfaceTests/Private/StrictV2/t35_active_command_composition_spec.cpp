#include "Misc/AutomationTest.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "Core/Contract/Command/UeAgentCommandLifecyclePolicy.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "Module/CommandRegistration/QueryCalibration/UeAgentQueryCalibrationRegistration.h"

namespace
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandRegistry;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Module::CommandRegistration::QueryCalibration;

	class FNeverCancelled final : public IUeAgentCancellationSignal
	{
	public:
		virtual bool IsCancelled() const override
		{
			return false;
		}
	};

	FUeAgentCommandFieldDeclaration Field(
		const TCHAR* Name,
		const TCHAR* WireType,
		const bool bRequired,
		const TCHAR* Description)
	{
		return {Name, WireType, bRequired, false, Description};
	}

	FUeAgentCommandDeclaration SyntheticDeclaration()
	{
		FUeAgentCommandDeclaration Result;
		Result.CommandId = TEXT("query.synthetic.extra");
		Result.PythonNamespace = TEXT("synthetic");
		Result.PythonFunction = TEXT("extra");
		Result.ContractVersion = 1;
		Result.PublicOwner = TEXT("strict-v2-t35-synthetic-owner");
		Result.Availability = EUeAgentCommandAvailability::Enabled;
		Result.SideEffect = EUeAgentCommandSideEffect::None;
		Result.StateLayers = {TEXT("fixture_current_state")};
		Result.InputFields = {
			Field(TEXT("confirm"), TEXT("boolean"), false,
				TEXT("Fixture-only confirmation field.")),
		};
		Result.ResultFields = {
			Field(TEXT("synthetic"), TEXT("boolean"), true, TEXT("True for the composition fixture.")),
		};
		Result.Errors = {
			{TEXT("synthetic_failure"), TEXT("The synthetic handler failed.")},
		};
		Result.Purpose.Summary = TEXT("Read an extra synthetic command contributed outside query calibration.");
		Result.Purpose.Target = TEXT("The strict_v2 active composition fixture.");
		Result.Purpose.Preconditions = {TEXT("The fixture contribution is active.")};
		Result.Purpose.ParameterGuidance = TEXT("No parameters are accepted.");
		Result.Purpose.ResultGuidance = TEXT("Use the synthetic boolean only as test evidence.");
		Result.Purpose.FailureGuidance = TEXT("Failures are typed and never fall back.");
		Result.Purpose.SideEffectGuidance = TEXT("Read-only and fixture-local.");
		Result.Purpose.MinimumExample = TEXT("execute_exact('query.synthetic.extra', {})");
		Result.Purpose.RelatedFlows = {TEXT("query.command.search")};
		Result.Purpose.Keywords = {TEXT("synthetic"), TEXT("extra"), TEXT("composition")};
		Result.Purpose.Synonyms = {TEXT("extra contributed command")};
		return Result;
	}

	class FSyntheticHandler final : public IUeAgentExactCommandHandler
	{
	public:
		virtual const FString& GetCommandId() const override
		{
			static const FString Value = TEXT("query.synthetic.extra");
			return Value;
		}

		virtual const FString& GetPublicOwner() const override
		{
			static const FString Value = TEXT("strict-v2-t35-synthetic-owner");
			return Value;
		}

		virtual FUeAgentCommandResult Submit(
			const FUeAgentCommandRequest& Request,
			const FUeAgentCallerContext& CallerContext,
			const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
		{
		return FUeAgentCommandResult::Success(
			Request,
			TEXT("{\"synthetic\":true}"));
		}
	};

	class FSyntheticContribution final : public IUeAgentActiveCommandContribution
	{
	public:
		explicit FSyntheticContribution(FString InId = TEXT("strict-v2-t35-synthetic"))
			: Id(MoveTemp(InId))
		{
		}

		virtual const FString& GetContributionId() const override
		{
			return Id;
		}

		virtual void CollectDeclarations(
			TArray<FUeAgentCommandDeclaration>& OutDeclarations) const override
		{
			OutDeclarations.Add(SyntheticDeclaration());
		}

		virtual bool CreateHandlers(
			TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
			TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
			FString& OutError) const override
		{
			if (!Registry->FindExact(TEXT("query.command.search"))
				|| !Registry->FindExact(TEXT("query.synthetic.extra")))
			{
				OutError = TEXT("final_registry_not_visible_to_contribution");
				return false;
			}
			OutHandlers.Add(MakeShared<FSyntheticHandler>());
			return true;
		}

	private:
		FString Id;
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
			TEXT("\"correlation_id\":\"corr-t35\",")
			TEXT("\"request_id\":\"req-t35\",")
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
		const TSharedRef<const FUeAgentExactCommandDispatch> Dispatch,
		const FUeAgentFrozenCommandContract& Contract,
		const FString& ParamsJson)
	{
		TOptional<FUeAgentCommandRequest> Request;
		if (!DecodeRequest(
			Contract.GetCommandId(),
			Contract.GetContractHash(),
			ParamsJson,
			Request)
			|| !Request.IsSet())
		{
			return FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t35"),
				TEXT("req-t35"),
				Contract.GetCommandId(),
				Contract.GetContractHash(),
				TEXT("test_request_decode_failed"),
				TEXT("strict_v2 fixture could not decode its request"));
		}
		return Dispatch->Submit(
			Request.GetValue(),
			FUeAgentCallerContext(
				TEXT("strict-v2-t35"),
				{TEXT("uai:query")},
				TEXT("corr-t35")),
			MakeShared<FNeverCancelled>());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T35TwoPhaseCompositionTest,
	"UAI.StrictV2.T35.ACTIVE.COMMAND.COMPOSITION.TwoPhase",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T35TwoPhaseCompositionTest::RunTest(
	const FString& Parameters)
{
	FUeAgentActiveCommandComposition Composition;
	FString Error;
	TSharedPtr<IUeAgentActiveCommandContribution> QueryContribution;
	TestTrue(
		TEXT("query calibration exposes a contribution instead of freezing a private registry"),
		TryCreateQueryCalibrationContribution(
			QueryContribution,
			Error));
	if (!QueryContribution.IsValid())
	{
		return false;
	}
	TestTrue(
		TEXT("query contribution is accepted before sealing"),
		Composition.TryAdd(QueryContribution.ToSharedRef(), Error));
	TestTrue(
		TEXT("an independent domain contribution is accepted before sealing"),
		Composition.TryAdd(MakeShared<FSyntheticContribution>(), Error));

	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	TestTrue(
		TEXT("all declarations freeze once before handlers are created"),
		Composition.TryCompose(Registry, Dispatch, Error));
	TestTrue(TEXT("composition is sealed after compose"), Composition.IsSealed());
	TestTrue(TEXT("one immutable registry is produced"), Registry.IsValid());
	TestTrue(TEXT("one exact dispatch is produced"), Dispatch.IsValid());
	if (!Registry.IsValid() || !Dispatch.IsValid())
	{
		return false;
	}
	const FUeAgentFrozenCommandContract* Search =
		Registry->FindExact(TEXT("query.command.search"));
	TestNotNull(TEXT("query discovery remains in the aggregate registry"), Search);
	if (!Search)
	{
		return false;
	}
	const FUeAgentCommandResult SearchResult = Submit(
		Dispatch.ToSharedRef(),
		*Search,
		TEXT("{\"intent\":\"extra synthetic command\"}"));
	TestTrue(TEXT("query discovery dispatch succeeds against the aggregate registry"), SearchResult.IsOk());
	TestTrue(
		TEXT("T05 discovery sees commands from later independent contributions"),
		SearchResult.GetPayloadJson().Contains(TEXT("\"commands\":["))
			&& SearchResult.GetPayloadJson().Contains(TEXT("query.synthetic.extra")));
	TestFalse(
		TEXT("T05 names-only discovery omits machine and ranking fields"),
		SearchResult.GetPayloadJson().Contains(TEXT("contract_hash"))
			|| SearchResult.GetPayloadJson().Contains(TEXT("match_reason"))
			|| SearchResult.GetPayloadJson().Contains(TEXT("read_only")));
	const FUeAgentFrozenCommandContract* Usage =
		Registry->FindExact(TEXT("query.command.usage"));
	TestNotNull(TEXT("batch usage command is composed"), Usage);
	if (Usage)
	{
		const FUeAgentCommandResult UsageResult = Submit(
			Dispatch.ToSharedRef(),
			*Usage,
			TEXT("{\"commands\":[\"query.command.search\",\"query.command.search\"]}"));
		TestTrue(TEXT("batch usage dispatch succeeds"), UsageResult.IsOk());
		TestTrue(
			TEXT("batch usage returns a deduplicated usage map"),
			UsageResult.GetPayloadJson().Contains(TEXT("\"usage\""))
				&& UsageResult.GetPayloadJson().Contains(TEXT("client.query.command_search(intent)")));
		TestFalse(
			TEXT("batch usage omits hashes and generic exact invocation"),
			UsageResult.GetPayloadJson().Contains(TEXT("hash"))
				|| UsageResult.GetPayloadJson().Contains(TEXT("execute_exact")));
		const FUeAgentCommandResult ConfirmedUsageResult = Submit(
			Dispatch.ToSharedRef(),
			*Usage,
			TEXT("{\"commands\":[\"query.synthetic.extra\"]}"));
		TestTrue(
			TEXT("confirmation commands expose the required two-step usage"),
			ConfirmedUsageResult.IsOk()
				&& ConfirmedUsageResult.GetPayloadJson().Contains(TEXT("confirm=True")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T35SealingFailureTest,
	"UAI.StrictV2.T35.ACTIVE.COMMAND.COMPOSITION.SealingFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T35SealingFailureTest::RunTest(
	const FString& Parameters)
{
	FString Error;
	FUeAgentActiveCommandComposition DuplicateContribution;
	TestTrue(
		TEXT("first contribution id is accepted"),
		DuplicateContribution.TryAdd(
			MakeShared<FSyntheticContribution>(),
			Error));
	TestFalse(
		TEXT("duplicate contribution id is rejected before compose"),
		DuplicateContribution.TryAdd(
			MakeShared<FSyntheticContribution>(),
			Error));
	TestTrue(
		TEXT("duplicate contribution failure is typed"),
		Error.StartsWith(TEXT("active_command_contribution_duplicate:")));

	FUeAgentActiveCommandComposition DuplicateCommand;
	TestTrue(
		TEXT("first command contribution is accepted"),
		DuplicateCommand.TryAdd(
			MakeShared<FSyntheticContribution>(TEXT("synthetic-a")),
			Error));
	TestTrue(
		TEXT("different contribution id may declare independently"),
		DuplicateCommand.TryAdd(
			MakeShared<FSyntheticContribution>(TEXT("synthetic-b")),
			Error));
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	TestFalse(
		TEXT("duplicate command declarations prevent activation"),
		DuplicateCommand.TryCompose(Registry, Dispatch, Error));
	TestTrue(TEXT("failed compose still seals the catalog"), DuplicateCommand.IsSealed());
	TestFalse(TEXT("failed compose publishes no registry"), Registry.IsValid());
	TestFalse(TEXT("failed compose publishes no dispatch"), Dispatch.IsValid());

	TestFalse(
		TEXT("late contributions are rejected after the activation boundary"),
		DuplicateCommand.TryAdd(
			MakeShared<FSyntheticContribution>(TEXT("synthetic-late")),
			Error));
	TestTrue(
		TEXT("late contribution failure is typed"),
		Error.StartsWith(TEXT("active_command_composition_sealed:")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T35FinalRuntimeAuthorityTest,
	"UAI.StrictV2.T35.ACTIVE.COMMAND.COMPOSITION.FinalRuntimeAuthority",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T35FinalRuntimeAuthorityTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	TestTrue(
		TEXT("the process composes one final immutable registry"),
		TryComposeProcessActiveCommandComposition(
			Registry,
			Dispatch,
			Error));
	if (!Registry.IsValid() || !Dispatch.IsValid())
	{
		AddError(FString::Printf(
			TEXT("process composition unavailable: %s"),
			*Error));
		return false;
	}

	TestNull(
		TEXT("the replaced catalog command is absent from the final authority"),
		Registry->FindExact(TEXT("reference.catalog_search")));
	TestNotNull(
		TEXT("intent discovery is in the final authority"),
		Registry->FindExact(TEXT("query.command.search")));
	TestNotNull(
		TEXT("batch usage is in the final authority"),
		Registry->FindExact(TEXT("query.command.usage")));
	const FUeAgentFrozenCommandContract* UsageContract =
		Registry->FindExact(TEXT("query.command.usage"));
	if (TestNotNull(
			TEXT("workspace usage is projected by the public SDK contract"),
			UsageContract))
	{
		const FUeAgentCommandResult WorkspaceUsage = Submit(
			Dispatch.ToSharedRef(),
			*UsageContract,
			TEXT("{\"commands\":[\"content_graph.export_workspace\","
				"\"content_graph.import_workspace\"]}"));
		TestTrue(
			TEXT("workspace usage query succeeds"),
			WorkspaceUsage.IsOk());
		TestTrue(
			TEXT("workspace usage uses SDK-owned paths and typed handle"),
			WorkspaceUsage.GetPayloadJson().Contains(
				TEXT("client.content_graph.export_workspace(target)"))
				&& WorkspaceUsage.GetPayloadJson().Contains(
					TEXT("workspace.import_changes()")));
		TestFalse(
			TEXT("workspace usage does not expose machine workspace paths"),
			WorkspaceUsage.GetPayloadJson().Contains(TEXT("workspace_path")));
	}

	const FUeAgentFrozenCommandContract* SearchContract =
		Registry->FindExact(TEXT("query.command.search"));
	if (!TestNotNull(TEXT("lifecycle matrix uses public command search"),
			SearchContract))
	{
		return false;
	}
	for (const UE::UAI::CommandLifecycle::FCommandLifecyclePolicy& Policy
		: UE::UAI::CommandLifecycle::GetPolicies())
	{
		if (Policy.Availability != EUeAgentCommandAvailability::Dormant)
		{
			continue;
		}
		const FString& CommandId = Policy.CommandId;
		const FUeAgentFrozenCommandContract* Contract =
			Registry->FindExact(CommandId);
		if (!TestNotNull(
				*FString::Printf(TEXT("%s remains registered"), *CommandId),
				Contract))
		{
			continue;
		}
		TestEqual(
			*FString::Printf(TEXT("%s has central dormant lifecycle"), *CommandId),
			Contract->GetAvailability(),
			EUeAgentCommandAvailability::Dormant);
		const FUeAgentCommandResult ActiveSearch = Submit(
			Dispatch.ToSharedRef(),
			*SearchContract,
			FString::Printf(TEXT("{\"intent\":\"%s\"}"), *CommandId));
		TestTrue(TEXT("ordinary lifecycle search succeeds"),
			ActiveSearch.IsOk());
		TestFalse(
			*FString::Printf(TEXT("ordinary search hides %s"), *CommandId),
			ActiveSearch.GetPayloadJson().Contains(CommandId));
		TestFalse(
			TEXT("ordinary search exposes no machine identity fields"),
			ActiveSearch.GetPayloadJson().Contains(TEXT("contract_hash"))
				|| ActiveSearch.GetPayloadJson().Contains(TEXT("match_reason"))
				|| ActiveSearch.GetPayloadJson().Contains(TEXT("summary")));
	}

	return true;
}
