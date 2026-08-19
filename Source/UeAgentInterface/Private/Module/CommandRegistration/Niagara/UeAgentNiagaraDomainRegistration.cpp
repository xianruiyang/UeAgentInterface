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
				Field(TEXT("target"), TEXT("string"), true, TEXT("Exact persisted Niagara System, Emitter, or Script package.object identity.")),
				Field(TEXT("target_usage"), TEXT("string"), true, TEXT("Exact Niagara stack usage, such as particle_spawn or particle_update.")),
				Field(TEXT("query"), TEXT("string"), false, TEXT("Optional bounded module label or exact asset identity filter.")),
				Field(TEXT("limit"), TEXT("integer"), false, TEXT("Optional page size from 1 through 20; defaults to 20.")),
				Field(TEXT("cursor"), TEXT("integer"), false, TEXT("Optional non-negative result offset; defaults to zero.")),
			};
			Result.ResultFields = {
				Field(TEXT("command_id"), TEXT("string"), true, TEXT("The unchanged exact public command identity.")),
				Field(TEXT("read_only"), TEXT("boolean"), true, TEXT("Always true.")),
				Field(TEXT("target"), TEXT("string"), true, TEXT("The unchanged exact target identity.")),
				Field(TEXT("target_usage"), TEXT("string"), true, TEXT("The unchanged requested stack usage.")),
				Field(TEXT("target_revision"), TEXT("string"), true, TEXT("The current persisted target revision.")),
				Field(TEXT("candidates"), TEXT("array"), true, TEXT("Niagara module candidates implementing the complete candidate-name contract.")),
				Field(TEXT("total_matches"), TEXT("integer"), true, TEXT("Total candidates matching this query.")),
				Field(TEXT("next_cursor"), TEXT("integer"), true, TEXT("Next result offset, or -1 when complete.")),
				Field(TEXT("complete"), TEXT("boolean"), true, TEXT("True only on the final page.")),
				Field(TEXT("has_more"), TEXT("boolean"), true, TEXT("True only while another page remains.")),
			};
			Result.Errors = {
				{TEXT("niagara_candidate_query_authority_required"), TEXT("The uai:query authority is missing.")},
				{TEXT("niagara_candidate_target_unresolved"), TEXT("The exact target could not be loaded.")},
				{TEXT("niagara_candidate_target_type_unsupported"), TEXT("The target is not a Niagara System, Emitter, or Script.")},
				{TEXT("niagara_candidate_target_usage_required"), TEXT("The target stack usage is missing.")},
				{TEXT("niagara_candidate_page_invalid"), TEXT("The requested page is outside the bounded contract.")},
				{TEXT("niagara_candidate_palette_query_failed"), TEXT("The real Niagara module palette could not be queried.")},
			};
			Result.Purpose.Summary = TEXT("Resolve exact Niagara module creation candidates from the real Niagara module palette.");
			Result.Purpose.Target = TEXT("One exact persisted Niagara System, Emitter, or Script stack owner.");
			Result.Purpose.Preconditions = {
				TEXT("Copy the target unchanged from public content search or readback."),
				TEXT("Run in the Editor on the UE game thread with uai:query."),
			};
			Result.Purpose.ParameterGuidance = TEXT("Copy candidate.name and binding_revision unchanged into a Niagara HJSON stack module candidate object with the same target_usage.");
			Result.Purpose.ResultGuidance = TEXT("Treat name, binding_revision, target_revision, placement contract, and target usage as opaque exact values; continue paging while has_more is true.");
			Result.Purpose.FailureGuidance = TEXT("Unresolved targets, stale bindings, unsupported types, and missing target usage fail before any write.");
			Result.Purpose.SideEffectGuidance = TEXT("Read-only; it creates no module, package, file, or external state.");
			Result.Purpose.MinimumExample = TEXT("client.niagara.module_candidates(target, target_usage='particle_spawn', query='Initialize Particle')");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.search"),
				TEXT("query.command.detail"),
				TEXT("content_graph.summary"),
				TEXT("content_graph.import_workspace"),
				TEXT("niagara.stack.apply"),
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
