#include "Module/CommandRegistration/LongRunning/UeAgentLongRunningLifecycleRegistration.h"

#include "CommandHandlers/LongRunning/UeAgentLongRunningLifecycleCommandHandlers.h"
#include "OperationOwners/LongRunning/UeAgentLongRunningLifecycleOwner.h"

namespace UE::UAI::Module::CommandRegistration::LongRunning
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandHandlers::LongRunning;
	using namespace CommandRegistry;
	using namespace Module::CommandRegistration::Active;
	using namespace OperationOwners::LongRunning;

	namespace
	{
		const FString Owner = TEXT("long-running-session-owner");

		FUeAgentCommandFieldDeclaration Field(
			const TCHAR* Name,
			const TCHAR* WireType,
			const bool bRequired,
			const TCHAR* Description)
		{
			return {Name, WireType, bRequired, false, Description};
		}

		FUeAgentCommandErrorDeclaration Error(
			const TCHAR* Code,
			const TCHAR* Description)
		{
			return {Code, Description};
		}

		bool IsSnapshotCommand(const FString& CommandId)
		{
			return CommandId != TEXT("operation.receipt")
				&& CommandId != TEXT("operation.release");
		}

		FUeAgentCommandDeclaration Declaration(
			const FUeAgentLongRunningLifecycleRoute& Route)
		{
			FUeAgentCommandDeclaration Result;
			Result.CommandId = Route.CommandId;
			Result.PythonNamespace = TEXT("long_running");
			Result.PythonFunction = Route.PythonFunction;
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			// Lifecycle commands remain exact-dispatchable for the SDK's opaque
			// internal handle, but are not model-facing discovery/usage entries.
			Result.Availability = EUeAgentCommandAvailability::Dormant;
			Result.SideEffect = Route.CommandId == TEXT("operation.status")
				|| Route.CommandId == TEXT("operation.receipt")
				|| Route.CommandId == TEXT("operation.discover")
				|| Route.CommandId == TEXT("operation.recover")
				? EUeAgentCommandSideEffect::None
				: EUeAgentCommandSideEffect::EditorState;
			Result.StateLayers = {
				TEXT("server_operation_registry"),
				TEXT("owned_operation_resources"),
				TEXT("terminal_operation_receipt"),
			};
			Result.InputFields = {
				Field(TEXT("operation_ref"), TEXT("object"), true,
					TEXT("SDK-owned opaque lifecycle handle returned by the owning start command; model-facing callers must not construct or edit it.")),
			};
			if (Route.CommandId == TEXT("operation.discover")) Result.InputFields.Reset();
			else if (Route.CommandId == TEXT("operation.recover"))
			{
				Result.InputFields = {
					Field(TEXT("request_id"), TEXT("string"), true, TEXT("SDK-persisted original request identity; never replayed.")),
					Field(TEXT("command_id"), TEXT("string"), true, TEXT("Original command associated with the saved request.")),
				};
			}
			else if (Route.CommandId == TEXT("operation.takeover"))
			{
				Result.InputFields = {
					Field(TEXT("operation_id"), TEXT("string"), true, TEXT("SDK-owned identity obtained from discovery; explicitly replaces custody.")),
				};
			}
			if (Route.CommandId == TEXT("operation.resume")
				|| Route.CommandId == TEXT("operation.reconcile"))
			{
				Result.InputFields.Add(Field(
					TEXT("expected_generation"), TEXT("integer"), true,
					TEXT("SDK-owned continuation state; the client forwards the exact last observed generation and never constructs or increments it.")));
			}
			else if (Route.CommandId == TEXT("operation.cancel"))
			{
				Result.InputFields.Add(Field(
					TEXT("reason"), TEXT("string"), true,
					TEXT("Non-empty cancellation reason recorded before the safe stop boundary.")));
			}

			if (Route.CommandId == TEXT("operation.discover"))
			{
				Result.ResultFields = {
					Field(TEXT("schema"), TEXT("string"), true, TEXT("uai.long_running.discovery.v1.")),
					Field(TEXT("operations"), TEXT("array"), true, TEXT("Retained operations belonging to the authenticated local principal; no custody leases.")),
				};
			}
			else if (IsSnapshotCommand(Route.CommandId))
			{
				Result.ResultFields = {
					Field(TEXT("schema"), TEXT("string"), true,
						TEXT("uai.long_running.snapshot.v1.")),
					Field(TEXT("operation_ref"), TEXT("object"), true,
						TEXT("The unchanged operation, lease and server-session identity.")),
					Field(TEXT("generation"), TEXT("integer"), true,
						TEXT("Current native work generation.")),
					Field(TEXT("phase"), TEXT("string"), true,
						TEXT("Accepted, running, stop_requested, joining or terminal.")),
					Field(TEXT("lifecycle_status"), TEXT("string"), true,
						TEXT("None, succeeded, cancelled, failed or state_unknown.")),
					Field(TEXT("stop_requested"), TEXT("boolean"), true,
						TEXT("Whether the owner recorded a cancellation request.")),
					Field(TEXT("terminal"), TEXT("boolean"), true,
						TEXT("True only when the owner can prove the terminal state.")),
					Field(TEXT("retry_safe"), TEXT("boolean"), true,
						TEXT("Whether a new side-effecting request is safe after this state.")),
					Field(TEXT("latest_signal"), TEXT("object"), false,
						TEXT("Latest typed operation signal, when one exists.")),
					Field(TEXT("cleanup_complete"), TEXT("boolean"), true,
						TEXT("True only after native resources were released or cancellation preceded native start.")),
					Field(TEXT("progress_diagnostic"), TEXT("object"), false,
						TEXT("Current observation or cleanup failure, separate from the business outcome.")),
				};
			}
			else
			{
				Result.ResultFields = {
					Field(TEXT("schema"), TEXT("string"), true,
						TEXT("uai.long_running.receipt.v1.")),
					Field(TEXT("operation_ref"), TEXT("object"), true,
						TEXT("The exact released or terminal operation identity.")),
					Field(TEXT("generation"), TEXT("integer"), true,
						TEXT("Final observed native work generation.")),
					Field(TEXT("lifecycle_status"), TEXT("string"), true,
						TEXT("Proven terminal status or state_unknown.")),
					Field(TEXT("error_code"), TEXT("string"), true,
						TEXT("Stable terminal or cleanup error code, empty on success.")),
					Field(TEXT("retry_safe"), TEXT("boolean"), true,
						TEXT("Whether replaying the original side effect is safe.")),
					Field(TEXT("cleanup_complete"), TEXT("boolean"), true,
						TEXT("Whether native resource cleanup was confirmed.")),
				};
			}
			Result.Errors = {
				Error(TEXT("long_running_lifecycle_authority_required"),
					TEXT("The caller lacks public operation authority.")),
				Error(TEXT("long_running_session_operation_ref_invalid"),
					TEXT("The operation reference is incomplete or malformed.")),
				Error(TEXT("operation_not_found"),
					TEXT("The server owns no operation with that identity; no request is replayed.")),
				Error(TEXT("operation_owner_lease_mismatch"),
					TEXT("The owner lease does not match the operation.")),
				Error(TEXT("operation_server_session_mismatch"),
					TEXT("The operation belongs to another server session.")),
				Error(TEXT("operation_generation_mismatch"),
					TEXT("The expected generation is stale; no advance or replay occurs.")),
				Error(TEXT("operation_reconcile_evidence_missing"),
					TEXT("The terminal state cannot be proven and remains state_unknown.")),
			};
			Result.Purpose.Summary =
				TEXT("Internal SDK lifecycle service for one server-owned operation; not a model-facing workflow entry.");
			Result.Purpose.Target =
				TEXT("One SDK-owned opaque operation handle bound to a server session, owner lease and native generation.");
			Result.Purpose.Preconditions = {
				TEXT("Only the SDK lifecycle owner may retain and forward operation_ref."),
				TEXT("Only the SDK lifecycle owner may forward the exact last observed generation."),
			};
			Result.Purpose.ParameterGuidance =
				TEXT("SDK internal handle custody only; never expose, synthesize, inspect, or increment operation identity, lease, session, or generation in model context.");
			Result.Purpose.ResultGuidance =
				TEXT("Accepted, running and stop_requested are nonterminal; trust completion only when terminal is true or a terminal receipt is returned.");
			Result.Purpose.FailureGuidance =
				TEXT("Transport loss is not business failure; reconnect and query the same operation_ref. State_unknown forbids automatic replay.");
			Result.Purpose.SideEffectGuidance =
				TEXT("Cancel requests stop only at the owner's safe boundary; release removes only terminal resources owned by that operation.");
			Result.Purpose.MinimumExample =
				TEXT("SDK-internal lifecycle forwarding; no model-facing invocation.");
			Result.Purpose.RelatedFlows = {
				TEXT("operation.status"),
				TEXT("operation.receipt"),
				TEXT("operation.cancel"),
				TEXT("operation.resume"),
				TEXT("operation.reconcile"),
				TEXT("operation.release"),
			};
			Result.Purpose.Keywords = {
				TEXT("long running"), TEXT("operation"), TEXT("cancel"),
				TEXT("resume"), TEXT("reconcile"), TEXT("lease"),
			};
			Result.Purpose.Synonyms = {
				TEXT("job status"), TEXT("reattach task"), TEXT("stop operation"),
			};
			return Result;
		}

		class FContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			FContribution(
				TSharedRef<Contract::IUeAgentOperationApplicationPort> InOperationPort,
				TArray<FUeAgentLongRunningLifecycleRoute> InRoutes,
				TSharedPtr<Composition::LongRunningSession::
					FUeAgentLongRunningSessionRuntime> InRuntime = nullptr)
				: OperationPort(MoveTemp(InOperationPort))
				, Routes(MoveTemp(InRoutes))
				, Runtime(MoveTemp(InRuntime))
			{
			}

			virtual const FString& GetContributionId() const override
			{
				static const FString Id = TEXT("long-running-lifecycle");
				return Id;
			}

			virtual void CollectDeclarations(
				TArray<FUeAgentCommandDeclaration>& OutDeclarations) const override
			{
				for (const FUeAgentLongRunningLifecycleRoute& Route
					: Routes)
				{
					OutDeclarations.Add(Declaration(Route));
				}
			}

			virtual bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
				UE::UAI::Contract::FDiagnostic& OutError) const override
			{
				TArray<TSharedRef<IUeAgentExactCommandHandler>> Handlers;
				if (!TryCreateLongRunningLifecycleCommandHandlers(
						OperationPort,
						Handlers,
						OutError))
				{
					return false;
				}
				OutHandlers.Append(Handlers);
				return true;
			}

		private:
			TSharedRef<Contract::IUeAgentOperationApplicationPort> OperationPort;
			TArray<FUeAgentLongRunningLifecycleRoute> Routes;
			TSharedPtr<Composition::LongRunningSession::
				FUeAgentLongRunningSessionRuntime> Runtime;
		};

		const FUeAgentActiveCommandContributionFactoryRegistration
			GLongRunningLifecycleContributionRegistration(
				TEXT("long-running-lifecycle"),
				&TryCreateProductionLongRunningLifecycleContribution);
	}

	bool TryCreateLongRunningLifecycleContribution(
		TSharedRef<Contract::IUeAgentOperationApplicationPort> OperationPort,
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutContribution.Reset();
		TArray<FUeAgentLongRunningLifecycleRoute> Routes;
		if (!TryGetLongRunningLifecycleRoutes(Routes, OutError)) return false;
		OutContribution = MakeShared<FContribution>(MoveTemp(OperationPort), MoveTemp(Routes));
		return true;
	}

	bool TryCreateProductionLongRunningLifecycleContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutContribution.Reset();
		TSharedPtr<Contract::IUeAgentOperationApplicationPort> OperationPort;
		TSharedPtr<Composition::LongRunningSession::
			FUeAgentLongRunningSessionRuntime> Runtime;
		if (!TryGetProductionLongRunningLifecyclePort(
				OperationPort,
				Runtime,
				OutError)
			|| !OperationPort.IsValid())
		{
			return false;
		}
		TArray<FUeAgentLongRunningLifecycleRoute> Routes;
		if (!TryGetLongRunningLifecycleRoutes(Routes, OutError)) return false;
		OutContribution = MakeShared<FContribution>(
			OperationPort.ToSharedRef(), MoveTemp(Routes), Runtime);
		return true;
	}

	bool TryCreateLongRunningLifecycleRegistration(
		TSharedRef<Contract::IUeAgentOperationApplicationPort> OperationPort,
		TSharedPtr<const FUeAgentImmutableCommandRegistry>& OutRegistry,
		TSharedPtr<const FUeAgentExactCommandDispatch>& OutDispatch,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRegistry.Reset();
		OutDispatch.Reset();
		TSharedPtr<IUeAgentActiveCommandContribution> Contribution;
		FUeAgentActiveCommandComposition Composition;
		if (!TryCreateLongRunningLifecycleContribution(
				OperationPort,
				Contribution,
				OutError)
			|| !Composition.TryAdd(Contribution.ToSharedRef(), OutError)
			|| !Composition.TryCompose(OutRegistry, OutDispatch, OutError))
		{
			OutRegistry.Reset();
			OutDispatch.Reset();
			return false;
		}
		return true;
	}
}
