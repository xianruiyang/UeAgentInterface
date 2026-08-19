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
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = Route.CommandId == TEXT("operation.status")
				|| Route.CommandId == TEXT("operation.receipt")
				? EUeAgentCommandSideEffect::None
				: EUeAgentCommandSideEffect::EditorState;
			Result.StateLayers = {
				TEXT("server_operation_registry"),
				TEXT("owned_operation_resources"),
				TEXT("terminal_operation_receipt"),
			};
			Result.InputFields = {
				Field(TEXT("operation_ref"), TEXT("object"), true,
					TEXT("Exact operation_id, owner_lease_id and server_session_id returned by the owning start command.")),
			};
			if (Route.CommandId == TEXT("operation.resume")
				|| Route.CommandId == TEXT("operation.reconcile"))
			{
				Result.InputFields.Add(Field(
					TEXT("expected_generation"), TEXT("integer"), true,
					TEXT("Exact last observed native work generation; a mismatch is rejected without replay.")));
			}
			else if (Route.CommandId == TEXT("operation.cancel"))
			{
				Result.InputFields.Add(Field(
					TEXT("reason"), TEXT("string"), true,
					TEXT("Non-empty cancellation reason recorded before the safe stop boundary.")));
			}

			if (IsSnapshotCommand(Route.CommandId))
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
				TEXT("Observe or control one server-owned long-running operation without replaying its initiating side effect.");
			Result.Purpose.Target =
				TEXT("One exact operation reference bound to a server session, owner lease and native generation.");
			Result.Purpose.Preconditions = {
				TEXT("Use the operation_ref returned by the initiating public command."),
				TEXT("Resume and reconcile require the exact last observed generation."),
			};
			Result.Purpose.ParameterGuidance =
				TEXT("Forward operation_ref unchanged; never synthesize a lease or increment a generation locally.");
			Result.Purpose.ResultGuidance =
				TEXT("Accepted, running and stop_requested are nonterminal; trust completion only when terminal is true or a terminal receipt is returned.");
			Result.Purpose.FailureGuidance =
				TEXT("Transport loss is not business failure; reconnect and query the same operation_ref. State_unknown forbids automatic replay.");
			Result.Purpose.SideEffectGuidance =
				TEXT("Cancel requests stop only at the owner's safe boundary; release removes only terminal resources owned by that operation.");
			Result.Purpose.MinimumExample = Route.PythonFunction
				+ TEXT("(client, operation_ref, ...)");
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
				TSharedPtr<Composition::LongRunningSession::
					FUeAgentLongRunningSessionRuntime> InRuntime = nullptr)
				: OperationPort(MoveTemp(InOperationPort))
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
					: GetLongRunningLifecycleRoutes())
				{
					OutDeclarations.Add(Declaration(Route));
				}
			}

			virtual bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
				FString& OutError) const override
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
		FString& OutError)
	{
		OutContribution = MakeShared<FContribution>(MoveTemp(OperationPort));
		OutError.Reset();
		return true;
	}

	bool TryCreateProductionLongRunningLifecycleContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
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
		OutContribution = MakeShared<FContribution>(
			OperationPort.ToSharedRef(), Runtime);
		return true;
	}

	bool TryCreateLongRunningLifecycleRegistration(
		TSharedRef<Contract::IUeAgentOperationApplicationPort> OperationPort,
		TSharedPtr<const FUeAgentImmutableCommandRegistry>& OutRegistry,
		TSharedPtr<const FUeAgentExactCommandDispatch>& OutDispatch,
		FString& OutError)
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
