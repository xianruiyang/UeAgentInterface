#include "Module/CommandRegistration/Pcg/UeAgentPcgDomainRegistration.h"

#include "CommandHandlers/Pcg/UeAgentPcgDomainCommandHandlers.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "OperationOwners/Pcg/UeAgentPcgDomainService.h"

#define UAI_T19_EXPECTED_RED_PROVIDER_DISABLED 0

namespace UE::UAI::Module::CommandRegistration::Pcg
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandHandlers::Pcg;
	using namespace CommandRegistry;
	using namespace OperationOwners::Pcg;

	namespace
	{
		const FString ProviderRegistrationId = TEXT("pcg-domain-provider");
		const FString CandidateCommandId = TEXT("pcg.node.candidates");
		const FString CandidateOwner = TEXT("pcg-domain-owner");
	#if !UAI_T19_EXPECTED_RED_PROVIDER_DISABLED
		const HjsonWorkspace::FImportInitializationDomainProviderFactoryRegistration ProviderRegistration(
			TEXT("pcg-domain-candidates"),
			TEXT("pcg-domain-provider"),
			&OperationOwners::Pcg::TryCreatePcgDomainProvider);
	#endif

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
			Result.PythonNamespace = TEXT("pcg");
			Result.PythonFunction = TEXT("node_candidates");
			Result.ContractVersion = 1;
			Result.PublicOwner = CandidateOwner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::None;
			Result.StateLayers = {TEXT("asset_persisted_state")};
			Result.InputFields = {
				Field(TEXT("target"), TEXT("string"), true, TEXT("Exact persisted PCG Graph package.object identity.")),
				Field(TEXT("query"), TEXT("string"), false, TEXT("Optional bounded search over the native PCG graph context-menu text.")),
				Field(TEXT("id"), TEXT("string"), false, TEXT("Optional exact compact candidate id returned by this command.")),
				Field(TEXT("limit"), TEXT("integer"), false, TEXT("Optional page size from 1 through 64; defaults to 20.")),
				Field(TEXT("cursor"), TEXT("integer"), false, TEXT("Optional non-negative result offset; defaults to zero.")),
			};
			Result.ResultFields = {
				Field(TEXT("command_id"), TEXT("string"), true, TEXT("The unchanged exact public command identity.")),
				Field(TEXT("read_only"), TEXT("boolean"), true, TEXT("Always true.")),
				Field(TEXT("target"), TEXT("string"), true, TEXT("The unchanged exact target identity.")),
				Field(TEXT("target_revision"), TEXT("string"), true, TEXT("The current persisted target revision.")),
				Field(TEXT("candidates"), TEXT("array"), true, TEXT("Compact projections of the exact native PCG graph context-menu actions.")),
				Field(TEXT("total_matches"), TEXT("integer"), true, TEXT("Total candidates matching this query.")),
				Field(TEXT("next_cursor"), TEXT("integer"), true, TEXT("Next result offset, or -1 when complete.")),
				Field(TEXT("complete"), TEXT("boolean"), true, TEXT("True only on the final page.")),
				Field(TEXT("has_more"), TEXT("boolean"), true, TEXT("True only while another page remains.")),
				Field(TEXT("coverage_complete"), TEXT("boolean"), true, TEXT("True when search projected the complete native context-menu action set.")),
				Field(TEXT("creation_coverage_complete"), TEXT("boolean"), true, TEXT("True only when every searched native action kind is executable through HJSON.")),
				Field(TEXT("native_action_count"), TEXT("integer"), true, TEXT("Complete native action count before filtering.")),
				Field(TEXT("executable_action_count"), TEXT("integer"), true, TEXT("Native actions currently verified for HJSON creation.")),
			};
			Result.Errors = {
				{TEXT("pcg_candidate_query_authority_required"), TEXT("The uai:query authority is missing.")},
				{TEXT("pcg_candidate_target_unresolved"), TEXT("The exact target could not be loaded.")},
				{TEXT("pcg_candidate_target_type_unsupported"), TEXT("The target is not an exact PCG Graph.")},
				{TEXT("pcg_candidate_page_invalid"), TEXT("The requested page is outside the bounded contract.")},
				{TEXT("pcg_candidate_editor_context_unavailable"), TEXT("The native PCG editor graph context could not be composed without UI.")},
				{TEXT("pcg_candidate_identity_ambiguous"), TEXT("The complete native action set did not yield unique compact identities.")},
			};
			Result.Purpose.Summary = TEXT("Search the same native PCG graph background context-menu actions used by the Editor and return compact name/id creation selectors.");
			Result.Purpose.Target = TEXT("One exact persisted PCG Graph and its native background graph context.");
			Result.Purpose.Preconditions = {
				TEXT("Copy the target unchanged from public content search or readback."),
				TEXT("Run in the Editor on the UE game thread with uai:query."),
			};
			Result.Purpose.ParameterGuidance = TEXT("Use query to search like the PCG graph right-click menu, or id to read one exact candidate. Put only candidate.name or candidate.id in HJSON; graph, placement, semantic node identity, and optional assertions stay outside candidate.");
			Result.Purpose.ResultGuidance = TEXT("Copy one unique name or exact compact id. Continue paging while has_more is true. coverage_complete describes search coverage; creation_coverage_complete separately describes executable action kinds.");
			Result.Purpose.FailureGuidance = TEXT("Unresolved targets, unavailable native context, identity collisions, and invalid pages fail without graph mutation.");
			Result.Purpose.SideEffectGuidance = TEXT("Content read-only: it may load the PCGEditor module and native action metadata but creates no node, package, file, or asset state.");
			Result.Purpose.MinimumExample = TEXT("client.pcg.node_candidates(target, query='Point')");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.search"),
				TEXT("query.command.detail"),
				TEXT("content_graph.summary"),
				TEXT("content_graph.import_workspace"),
				TEXT("pcg.graph.apply"),
			};
			Result.Purpose.Keywords = {TEXT("pcg"), TEXT("node"), TEXT("candidate"), TEXT("graph"), TEXT("hjson")};
			Result.Purpose.Synonyms = {TEXT("find pcg node"), TEXT("search pcg right click menu"), TEXT("resolve pcg native action")};
			return Result;
		}

		class FPcgCandidateContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			explicit FPcgCandidateContribution(
				TSharedRef<IUeAgentPcgCandidateCommandService> InService)
				: Service(MoveTemp(InService))
			{
			}

			virtual const FString& GetContributionId() const override
			{
				static const FString Id = TEXT("pcg-domain-candidates");
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
				return TryCreatePcgCandidateCommandHandlers(Service, OutHandlers, OutError);
			}

		private:
			TSharedRef<IUeAgentPcgCandidateCommandService> Service;
		};

		const FUeAgentActiveCommandContributionFactoryRegistration CandidateRegistration(
			TEXT("pcg-domain-candidates"),
			&TryCreateProductionPcgCandidateContribution);
	}

	const FString& GetProviderRegistrationId()
	{
		return ProviderRegistrationId;
	}

	bool TryCreateProductionPcgCandidateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
	{
		TSharedPtr<IUeAgentPcgCandidateCommandService> Service;
		if (!TryCreateProductionPcgCandidateCommandService(Service, OutError)
			|| !Service.IsValid())
		{
			OutContribution.Reset();
			return false;
		}
		OutContribution = MakeShared<FPcgCandidateContribution>(Service.ToSharedRef());
		OutError.Reset();
		return true;
	}
}
