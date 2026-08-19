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
				Field(TEXT("query"), TEXT("string"), false, TEXT("Optional bounded PCG settings class name filter.")),
				Field(TEXT("limit"), TEXT("integer"), false, TEXT("Optional page size from 1 through 64; defaults to 64.")),
				Field(TEXT("cursor"), TEXT("integer"), false, TEXT("Optional non-negative result offset; defaults to zero.")),
			};
			Result.ResultFields = {
				Field(TEXT("command_id"), TEXT("string"), true, TEXT("The unchanged exact public command identity.")),
				Field(TEXT("read_only"), TEXT("boolean"), true, TEXT("Always true.")),
				Field(TEXT("target"), TEXT("string"), true, TEXT("The unchanged exact target identity.")),
				Field(TEXT("target_revision"), TEXT("string"), true, TEXT("The current persisted target revision.")),
				Field(TEXT("candidates"), TEXT("array"), true, TEXT("Exact non-abstract UPCGSettings class candidates.")),
				Field(TEXT("total_matches"), TEXT("integer"), true, TEXT("Total candidates matching this query.")),
				Field(TEXT("next_cursor"), TEXT("integer"), true, TEXT("Next result offset, or -1 when complete.")),
				Field(TEXT("complete"), TEXT("boolean"), true, TEXT("True only on the final page.")),
				Field(TEXT("has_more"), TEXT("boolean"), true, TEXT("True only while another page remains.")),
			};
			Result.Errors = {
				{TEXT("pcg_candidate_query_authority_required"), TEXT("The uai:query authority is missing.")},
				{TEXT("pcg_candidate_target_unresolved"), TEXT("The exact target could not be loaded.")},
				{TEXT("pcg_candidate_target_type_unsupported"), TEXT("The target is not an exact PCG Graph.")},
				{TEXT("pcg_candidate_page_invalid"), TEXT("The requested page is outside the bounded contract.")},
			};
			Result.Purpose.Summary = TEXT("Resolve exact PCG node creation candidates from loaded non-abstract UPCGSettings classes.");
			Result.Purpose.Target = TEXT("One exact persisted PCG Graph.");
			Result.Purpose.Preconditions = {
				TEXT("Copy the target unchanged from public content search or readback."),
				TEXT("Run in the Editor on the UE game thread with uai:query."),
			};
			Result.Purpose.ParameterGuidance = TEXT("Copy candidate.name and binding_revision unchanged into a PCG HJSON node candidate object.");
			Result.Purpose.ResultGuidance = TEXT("Treat name, binding_revision, target_revision, and placement contract as opaque exact values; continue paging while has_more is true.");
			Result.Purpose.FailureGuidance = TEXT("Unresolved targets, unsupported types, and invalid pages fail before any write.");
			Result.Purpose.SideEffectGuidance = TEXT("Read-only; it creates no node, package, file, or external state.");
			Result.Purpose.MinimumExample = TEXT("client.pcg.node_candidates(target, query='Point')");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.search"),
				TEXT("query.command.detail"),
				TEXT("content_graph.summary"),
				TEXT("content_graph.import_workspace"),
				TEXT("pcg.graph.apply"),
			};
			Result.Purpose.Keywords = {TEXT("pcg"), TEXT("node"), TEXT("candidate"), TEXT("graph"), TEXT("hjson")};
			Result.Purpose.Synonyms = {TEXT("find pcg node"), TEXT("resolve pcg settings class")};
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
