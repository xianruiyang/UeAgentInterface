#include "Module/CommandRegistration/Niagara/UeAgentNiagaraDomainRegistration.h"

#include "CommandHandlers/Niagara/UeAgentNiagaraDomainCommandHandlers.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "OperationOwners/Niagara/UeAgentNiagaraDomainService.h"

namespace UE::UAI::Module::CommandRegistration::Niagara
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandHandlers::Niagara;
	using namespace CommandRegistry;
	using namespace OperationOwners::Niagara;

	namespace
	{
		const FString ProviderRegistrationId = TEXT("niagara-domain-provider");
		const FString CandidateCommandId = TEXT("niagara.module.candidates");
		const FString CandidateOwner = TEXT("niagara-domain-owner");
		const HjsonWorkspace::FImportInitializationDomainProviderFactoryRegistration ProviderRegistration(
			TEXT("niagara-domain-candidates"),
			TEXT("niagara-domain-provider"),
			&OperationOwners::Niagara::TryCreateNiagaraDomainProvider);

		FUeAgentCommandFieldDeclaration Field(
			const TCHAR* Name,
			const TCHAR* WireType,
			const bool bRequired,
			const TCHAR* Description)
		{
			return {Name, WireType, bRequired, false, Description};
		}

		FUeAgentCommandDeclaration CandidateDeclaration()
		{
			FUeAgentCommandDeclaration Result;
			Result.CommandId = CandidateCommandId;
			Result.PythonNamespace = TEXT("niagara");
			Result.PythonFunction = TEXT("module_candidates");
			Result.ContractVersion = 1;
			Result.PublicOwner = CandidateOwner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::None;
			Result.StateLayers = {TEXT("asset_persisted_state")};
			Result.InputFields = {
				Field(TEXT("target"), TEXT("string"), true, TEXT("Exact persisted Niagara System package.object identity; standalone Emitter native action ownership is not yet exposed by this command.")),
				Field(TEXT("target_usage"), TEXT("string"), true, TEXT("Niagara stack usage resolved against a real output node, such as system_update or particle_update.")),
				Field(TEXT("target_usage_id"), TEXT("string"), false, TEXT("Optional exact output usage GUID; required when usage alone is ambiguous.")),
				Field(TEXT("emitter_handle_id"), TEXT("string"), false, TEXT("Optional exact System emitter handle GUID; required for emitter or particle stacks when the target has multiple matching emitters.")),
				Field(TEXT("query"), TEXT("string"), false, TEXT("Optional bounded native module display-name, category, or identity filter.")),
				Field(TEXT("limit"), TEXT("integer"), false, TEXT("Optional page size from 1 through 20; defaults to 20.")),
				Field(TEXT("cursor"), TEXT("integer"), false, TEXT("Optional non-negative result offset; defaults to zero.")),
			};
			Result.ResultFields = {
				Field(TEXT("command_id"), TEXT("string"), true, TEXT("The unchanged exact public command identity.")),
				Field(TEXT("read_only"), TEXT("boolean"), true, TEXT("Always true.")),
				Field(TEXT("target"), TEXT("string"), true, TEXT("The unchanged exact target identity.")),
				Field(TEXT("target_usage"), TEXT("string"), true, TEXT("The canonical usage of the resolved real output node.")),
				Field(TEXT("target_revision"), TEXT("string"), true, TEXT("The current persisted target revision.")),
				Field(TEXT("query_context"), TEXT("object"), true, TEXT("Resolved target_usage, target_usage_id, optional emitter_handle_id, and stage_key; keep these outside candidate when authoring HJSON.")),
				Field(TEXT("coverage_scope"), TEXT("string"), true, TEXT("The exact native Add Module action surface covered by this result.")),
				Field(TEXT("coverage_complete"), TEXT("boolean"), true, TEXT("True when actions came from the exact native Stack AddUtilities owner for this output context.")),
				Field(TEXT("creation_coverage_complete"), TEXT("boolean"), true, TEXT("True only when every returned native action kind has a noninteractive HJSON creation contract.")),
				Field(TEXT("unsupported_action_kinds"), TEXT("array"), true, TEXT("Native actions that are searchable but intentionally fail closed on create until their dedicated noninteractive contract exists.")),
				Field(TEXT("candidates"), TEXT("array"), true, TEXT("All native Add Module actions with reusable id/name selectors; supported marks actions currently safe to create.")),
				Field(TEXT("total_matches"), TEXT("integer"), true, TEXT("Total candidates matching this query.")),
				Field(TEXT("next_cursor"), TEXT("integer"), true, TEXT("Next result offset, or -1 when complete.")),
				Field(TEXT("complete"), TEXT("boolean"), true, TEXT("True only on the final page.")),
				Field(TEXT("has_more"), TEXT("boolean"), true, TEXT("True only while another page remains.")),
			};
			Result.Errors = {
				{TEXT("niagara_candidate_query_authority_required"), TEXT("The uai:query authority is missing.")},
				{TEXT("niagara_candidate_target_unresolved"), TEXT("The exact target could not be loaded.")},
				{TEXT("niagara_candidate_target_type_unsupported"), TEXT("The target is not a Niagara System stack owner.")},
				{TEXT("niagara_candidate_target_usage_required"), TEXT("The target stack usage is missing.")},
				{TEXT("niagara_candidate_target_usage_invalid"), TEXT("The requested target usage is not a Niagara script usage.")},
				{TEXT("niagara_candidate_stage_not_found"), TEXT("No real output node matches the requested context.")},
				{TEXT("niagara_candidate_stage_ambiguous"), TEXT("Usage alone matches multiple output contexts; provide usage/emitter GUIDs from readback.")},
				{TEXT("niagara_candidate_page_invalid"), TEXT("The requested page is outside the bounded contract.")},
				{TEXT("niagara_candidate_palette_query_failed"), TEXT("The real Niagara module palette could not be queried.")},
			};
			Result.Purpose.Summary = TEXT("Resolve the exact native Niagara Stack Add Module action set in one real output-node context.");
			Result.Purpose.Target = TEXT("One exact persisted Niagara System stack owner and one resolved System or emitter-handle output node.");
			Result.Purpose.Preconditions = {
				TEXT("Copy the target unchanged from public content search or readback."),
				TEXT("Run in the Editor on the UE game thread with uai:query."),
			};
			Result.Purpose.ParameterGuidance = TEXT("Copy only candidate.id (preferred) or candidate.name into the module candidate object. Put query_context usage, usage_id, emitter_handle_id, and module_index on the stage/module siblings, not inside candidate.");
			Result.Purpose.ResultGuidance = TEXT("coverage_complete describes search equivalence. Check candidate.supported and creation_coverage_complete separately: asset-module actions replay now, while scratch-pad, assignment/set-specific, and wizard actions remain searchable but fail closed until their HJSON contracts exist; continue paging while has_more is true.");
			Result.Purpose.FailureGuidance = TEXT("Invalid or ambiguous output contexts and unresolved or ambiguous name/id selectors fail before mutation; the server re-enumerates the same context again at mutation.");
			Result.Purpose.SideEffectGuidance = TEXT("Read-only; it creates no module, package, file, or external state.");
			Result.Purpose.MinimumExample = TEXT("client.niagara.module_candidates(target, target_usage='system_update', query='System')");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.search"),
				TEXT("query.command.detail"),
				TEXT("content_graph.summary"),
				TEXT("content_graph.import_workspace"),
				TEXT("content_graph.items"),
			};
			Result.Purpose.Keywords = {TEXT("niagara"), TEXT("module"), TEXT("candidate"), TEXT("stack"), TEXT("hjson")};
			Result.Purpose.Synonyms = {TEXT("find niagara module"), TEXT("resolve niagara stack module")};
			return Result;
		}

		class FNiagaraCandidateContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			explicit FNiagaraCandidateContribution(
				TSharedRef<IUeAgentNiagaraCandidateCommandService> InService)
				: Service(MoveTemp(InService))
			{
			}

			virtual const FString& GetContributionId() const override
			{
				static const FString Id = TEXT("niagara-domain-candidates");
				return Id;
			}

			virtual void CollectDeclarations(TArray<FUeAgentCommandDeclaration>& OutDeclarations) const override
			{
				OutDeclarations.Add(CandidateDeclaration());
			}

			virtual bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
				FString& OutError) const override
			{
				(void)Registry;
				return TryCreateNiagaraCandidateCommandHandlers(Service, OutHandlers, OutError);
			}

		private:
			TSharedRef<IUeAgentNiagaraCandidateCommandService> Service;
		};

		const FUeAgentActiveCommandContributionFactoryRegistration CandidateRegistration(
			TEXT("niagara-domain-candidates"),
			&TryCreateProductionNiagaraCandidateContribution);
	}

	const FString& GetProviderRegistrationId()
	{
		return ProviderRegistrationId;
	}

	bool TryCreateProductionNiagaraCandidateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
	{
		TSharedPtr<IUeAgentNiagaraCandidateCommandService> Service;
		if (!TryCreateProductionNiagaraCandidateCommandService(Service, OutError)
			|| !Service.IsValid())
		{
			OutContribution.Reset();
			return false;
		}
		OutContribution = MakeShared<FNiagaraCandidateContribution>(Service.ToSharedRef());
		OutError.Reset();
		return true;
	}
}
