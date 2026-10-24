#include "Module/CommandRegistration/AssetDurable/UeAgentAssetDurableMaintenanceRegistration.h"

namespace UE::UAI::Module::CommandRegistration::AssetDurable
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandHandlers::AssetDurable;
	using namespace CommandRegistry;
	using namespace Module::CommandRegistration::Active;

	namespace
	{
		const FString Owner = TEXT("asset-durable-maintenance-owner");

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

		FUeAgentCommandDeclaration BaseDeclaration(
			const TCHAR* CommandId,
			const TCHAR* PythonFunction,
			const EUeAgentCommandSideEffect SideEffect)
		{
			FUeAgentCommandDeclaration Result;
			Result.CommandId = CommandId;
			Result.PythonNamespace = TEXT("asset_durable");
			Result.PythonFunction = PythonFunction;
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			// The commands remain exact-dispatchable for DurableJournalHandle,
			// but journal/owner/fence/integrity identities are not model-facing.
			Result.Availability = EUeAgentCommandAvailability::Dormant;
			Result.SideEffect = SideEffect;
			Result.StateLayers = {TEXT("asset_persisted_state")};
			return Result;
		}

		FUeAgentCommandDeclaration ReadDeclaration()
		{
			FUeAgentCommandDeclaration Result = BaseDeclaration(
				TEXT("asset_durable.journal.read"),
				TEXT("journal_read"),
				EUeAgentCommandSideEffect::None);
			Result.InputFields = {
				Field(TEXT("journal_id"), TEXT("string"), true,
					TEXT("Exact 32-hex durable journal identity.")),
			};
			Result.ResultFields = {
				Field(TEXT("journal_id"), TEXT("string"), true,
					TEXT("Exact journal identity read from the bound journal.")),
				Field(TEXT("journal_owner_id"), TEXT("string"), true,
					TEXT("Stable durable owner identity required by reconcile.")),
				Field(TEXT("fencing_token"), TEXT("integer"), true,
					TEXT("Persisted monotonic fence required by reconcile.")),
				Field(TEXT("phase"), TEXT("string"), true,
					TEXT("Current durable journal phase.")),
				Field(TEXT("plan_fingerprint"), TEXT("string"), true,
					TEXT("Immutable plan fingerprint bound by the journal.")),
				Field(TEXT("inventory_hash"), TEXT("string"), true,
					TEXT("Immutable durable inventory hash.")),
				Field(TEXT("pre_mutation_states"), TEXT("array"), true,
					TEXT("Journal-bound original artifact presence and hashes.")),
				Field(TEXT("expected_persisted_states"), TEXT("array"), true,
					TEXT("Journal-bound expected persisted artifact states when available.")),
				Field(TEXT("observed_states"), TEXT("array"), true,
					TEXT("Fresh independent artifact readback for this request.")),
			};
			Result.Errors = {
				Error(TEXT("asset_durable_query_authority_required"),
					TEXT("The caller lacks uai:query authority.")),
				Error(TEXT("asset_durable_journal_read_params_invalid"),
					TEXT("The exact journal identity is missing or invalid.")),
				Error(TEXT("durable_journal_integrity_invalid"),
					TEXT("The active journal failed path, schema, integrity or inventory validation.")),
				Error(TEXT("durable_artifact_readback_failed"),
					TEXT("A bound durable artifact could not be independently read.")),
				Error(TEXT("resource_accounting_failed"),
					TEXT("The maintenance read received invalid internal resource accounting.")),
			};
			Result.Purpose.Summary = TEXT(
				"Internal SDK inspection for one typed durable-journal handle.");
			Result.Purpose.Target = TEXT(
				"One active SimpleAuthoring durable journal and only its bound artifact inventory.");
			Result.Purpose.Preconditions = {
				TEXT("The UAI Editor service is running and the caller has uai:query authority."),
				TEXT("Use the exact journal_id from a durable failure or maintenance receipt."),
			};
			Result.Purpose.ParameterGuidance = TEXT(
				"SDK handle custody only; model-facing callers never read or provide journal identities.");
			Result.Purpose.ResultGuidance = TEXT(
				"Preserve journal_id, journal_owner_id, fencing_token, plan_fingerprint and inventory_hash unchanged as one evidence binding.");
			Result.Purpose.FailureGuidance = TEXT(
				"Integrity and readback failures remain typed; never fall back to replaying the original asset command.");
			Result.Purpose.SideEffectGuidance = TEXT(
				"Read-only exact-journal inspection; no journal phase, asset or Editor state is changed.");
			Result.Purpose.MinimumExample = TEXT(
				"result.durable_journals[0].inspect()");
			Result.Purpose.RelatedFlows = {
				TEXT("asset_durable.journal.reconcile"),
				TEXT("query.command.usage"),
			};
			Result.Purpose.Keywords = {
				TEXT("durable"), TEXT("journal"), TEXT("recovery"),
				TEXT("inspect"), TEXT("state_unknown"),
			};
			Result.Purpose.Synonyms = {
				TEXT("inspect failed asset transaction"),
				TEXT("read durable recovery evidence"),
			};
			return Result;
		}

		FUeAgentCommandDeclaration ReconcileDeclaration()
		{
			FUeAgentCommandDeclaration Result = BaseDeclaration(
				TEXT("asset_durable.journal.reconcile"),
				TEXT("journal_reconcile"),
				EUeAgentCommandSideEffect::AssetState);
			Result.InputFields = {
				Field(TEXT("journal_id"), TEXT("string"), true,
					TEXT("Exact journal identity returned by journal.read.")),
				Field(TEXT("expected_journal_owner_id"), TEXT("string"), true,
					TEXT("Exact stable owner returned by journal.read.")),
				Field(TEXT("expected_fencing_token"), TEXT("integer"), true,
					TEXT("Exact persisted fence returned by journal.read.")),
				Field(TEXT("verification_id"), TEXT("string"), true,
					TEXT("Caller-owned non-empty identity for this independent verification.")),
			};
			Result.ResultFields = {
				Field(TEXT("journal_id"), TEXT("string"), true,
					TEXT("Reconciled journal identity.")),
				Field(TEXT("status"), TEXT("string"), true,
					TEXT("verified, rolled_back or state_unknown.")),
				Field(TEXT("recovery_action"), TEXT("string"), true,
					TEXT("Bound terminal action or manual_intervention.")),
				Field(TEXT("verification_id"), TEXT("string"), true,
					TEXT("Caller verification identity bound into the receipt.")),
				Field(TEXT("replayed"), TEXT("boolean"), true,
					TEXT("Always false; reconciliation never replays the business operation.")),
				Field(TEXT("phase_history"), TEXT("array"), true,
					TEXT("Journal phase history including the new disposition.")),
				Field(TEXT("failure"), TEXT("object"), false,
					TEXT("Typed evidence insufficiency when status remains state_unknown.")),
			};
			Result.Errors = {
				Error(TEXT("asset_durable_operate_authority_required"),
					TEXT("The caller lacks uai:operate authority.")),
				Error(TEXT("asset_durable_journal_reconcile_params_invalid"),
					TEXT("The exact journal binding or verification identity is invalid.")),
				Error(TEXT("durable_reconcile_binding_mismatch"),
					TEXT("Journal owner or fence no longer matches the read evidence.")),
				Error(TEXT("durable_reconcile_phase_invalid"),
					TEXT("The current journal phase is already terminal or not reconcilable.")),
				Error(TEXT("durable_reconcile_readback_unconfirmed"),
					TEXT("Independent package/config state did not confirm the proposed result.")),
				Error(TEXT("durable_journal_integrity_invalid"),
					TEXT("The journal changed or failed integrity validation.")),
			};
			Result.Purpose.Summary = TEXT(
				"Internal SDK reconciliation for one typed durable-journal handle.");
			Result.Purpose.Target = TEXT(
				"One active SimpleAuthoring durable journal already inspected through journal.read.");
			Result.Purpose.Preconditions = {
				TEXT("Call asset_durable.journal.read immediately before this command."),
				TEXT("Pass its journal, owner and fence identities unchanged."),
				TEXT("The caller has uai:operate authority."),
			};
			Result.Purpose.ParameterGuidance = TEXT(
				"SDK handle custody generates and forwards journal, owner, fence and verification identities.");
			Result.Purpose.ResultGuidance = TEXT(
				"Only verified or rolled_back is terminal. state_unknown preserves the active journal and reports the missing evidence.");
			Result.Purpose.FailureGuidance = TEXT(
				"Binding, integrity or independent readback failure leaves the journal active; inspect again instead of replaying the original command.");
			Result.Purpose.SideEffectGuidance = TEXT(
				"May advance and archive the exact journal; never recreates, edits or deletes its business target and never activates the Editor UI.");
			Result.Purpose.MinimumExample = TEXT(
				"result.durable_journals[0].reconcile()");
			Result.Purpose.RelatedFlows = {
				TEXT("asset_durable.journal.read"),
				TEXT("query.command.usage"),
			};
			Result.Purpose.Keywords = {
				TEXT("durable"), TEXT("journal"), TEXT("reconcile"),
				TEXT("rollback"), TEXT("verify"), TEXT("no replay"),
			};
			Result.Purpose.Synonyms = {
				TEXT("resolve failed asset transaction"),
				TEXT("archive state unknown journal"),
			};
			return Result;
		}

		class FAssetDurableMaintenanceContribution final
			: public IUeAgentActiveCommandContribution
		{
		public:
			explicit FAssetDurableMaintenanceContribution(
				TSharedRef<IUeAgentAssetDurableMaintenancePort> InPort)
				: Port(MoveTemp(InPort))
			{
			}

			virtual const FString& GetContributionId() const override
			{
				static const FString Value =
					TEXT("asset-durable-maintenance");
				return Value;
			}

			virtual void CollectDeclarations(
				TArray<FUeAgentCommandDeclaration>& OutDeclarations)
				const override
			{
				OutDeclarations.Add(ReadDeclaration());
				OutDeclarations.Add(ReconcileDeclaration());
			}

			virtual bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
				UE::UAI::Contract::FDiagnostic& OutError) const override
			{
				return TryCreateAssetDurableMaintenanceHandlers(
					Port, OutHandlers, OutError);
			}

		private:
			TSharedRef<IUeAgentAssetDurableMaintenancePort> Port;
		};

		const FUeAgentActiveCommandContributionFactoryRegistration
			GAssetDurableMaintenanceContributionRegistration(
				TEXT("asset-durable-maintenance"),
				&TryCreateProductionAssetDurableMaintenanceContribution);
	}

	bool TryCreateAssetDurableMaintenanceContribution(
		TSharedRef<IUeAgentAssetDurableMaintenancePort> Port,
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutContribution.Reset();
		OutError.Reset();
		OutContribution = MakeShared<FAssetDurableMaintenanceContribution>(
			MoveTemp(Port));
		return true;
	}

	bool TryCreateProductionAssetDurableMaintenanceContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		return TryCreateAssetDurableMaintenanceContribution(
			CreateProductionAssetDurableMaintenancePort(),
			OutContribution,
			OutError);
	}

	bool TryCreateAssetDurableMaintenanceRegistration(
		TSharedRef<IUeAgentAssetDurableMaintenancePort> Port,
		TSharedPtr<const FUeAgentImmutableCommandRegistry>& OutRegistry,
		TSharedPtr<const FUeAgentExactCommandDispatch>& OutDispatch,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRegistry.Reset();
		OutDispatch.Reset();
		OutError.Reset();
		TSharedPtr<IUeAgentActiveCommandContribution> Contribution;
		FUeAgentActiveCommandComposition Composition;
		if (!TryCreateAssetDurableMaintenanceContribution(
				MoveTemp(Port), Contribution, OutError)
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
