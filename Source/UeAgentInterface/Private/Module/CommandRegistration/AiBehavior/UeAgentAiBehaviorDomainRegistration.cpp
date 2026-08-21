#include "Module/CommandRegistration/AiBehavior/UeAgentAiBehaviorDomainRegistration.h"

#include "CommandHandlers/AiBehavior/UeAgentAiBehaviorDomainCommandHandlers.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "OperationOwners/AiBehavior/UeAgentAiBehaviorDomainService.h"

namespace UE::UAI::Module::CommandRegistration::AiBehavior
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandHandlers::AiBehavior;
	using namespace CommandRegistry;
	using namespace OperationOwners::AiBehavior;

	namespace
	{
		const FString ProviderRegistrationId = TEXT("ai-behavior-domain-provider");
		const FString CandidateCommandId = TEXT("ai.behavior.candidates");
		const FString CandidateOwner = TEXT("ai-behavior-domain-owner");
		const HjsonWorkspace::FImportInitializationDomainProviderFactoryRegistration ProviderRegistration(
			TEXT("ai-behavior-domain-candidates"),
			TEXT("ai-behavior-domain-provider"),
			&OperationOwners::AiBehavior::TryCreateAiBehaviorDomainProvider);

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
			Result.PythonNamespace = TEXT("ai");
			Result.PythonFunction = TEXT("behavior_candidates");
			Result.ContractVersion = 1;
			Result.PublicOwner = CandidateOwner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::None;
			Result.StateLayers = {TEXT("asset_persisted_state")};
			Result.InputFields = {
				Field(TEXT("target"), TEXT("string"), true, TEXT("Exact persisted BehaviorTree, BlackboardData, EnvQuery, StateTree, or SmartObjectDefinition package.object identity.")),
				Field(TEXT("query"), TEXT("string"), false, TEXT("Optional bounded class identity filter.")),
				Field(TEXT("limit"), TEXT("integer"), false, TEXT("Optional page size from 1 through 64; defaults to 64.")),
				Field(TEXT("cursor"), TEXT("integer"), false, TEXT("Optional non-negative result offset; defaults to zero.")),
			};
			Result.ResultFields = {
				Field(TEXT("command_id"), TEXT("string"), true, TEXT("The unchanged exact public command identity.")),
				Field(TEXT("read_only"), TEXT("boolean"), true, TEXT("Always true.")),
				Field(TEXT("target"), TEXT("string"), true, TEXT("The unchanged exact target identity.")),
				Field(TEXT("target_revision"), TEXT("string"), true, TEXT("The current persisted target revision.")),
				Field(TEXT("candidates"), TEXT("array"), true, TEXT("Exact non-abstract BehaviorTree node/service/decorator, Blackboard key, EQS generator, StateTree node, or SmartObject behavior class and struct identities.")),
				Field(TEXT("total_matches"), TEXT("integer"), true, TEXT("Total candidates matching this query.")),
				Field(TEXT("next_cursor"), TEXT("integer"), true, TEXT("Next result offset, or -1 when complete.")),
				Field(TEXT("complete"), TEXT("boolean"), true, TEXT("True only on the final page.")),
				Field(TEXT("has_more"), TEXT("boolean"), true, TEXT("True only while another page remains.")),
			};
			Result.Errors = {
				{TEXT("ai_behavior_candidate_query_authority_required"), TEXT("The uai:query authority is missing.")},
				{TEXT("ai_behavior_candidate_target_unresolved"), TEXT("The exact target could not be loaded.")},
				{TEXT("ai_behavior_candidate_target_type_unsupported"), TEXT("The target is not an exact supported AI behavior root.")},
				{TEXT("ai_behavior_candidate_page_invalid"), TEXT("The requested page is outside the bounded contract.")},
			};
			Result.Purpose.Summary = TEXT("Resolve exact BehaviorTree, Blackboard, EQS, StateTree, and SmartObject creation candidates from loaded concrete UE class and struct identities.");
			Result.Purpose.Target = TEXT("One exact persisted BehaviorTree, BlackboardData, EnvQuery, StateTree, or SmartObjectDefinition.");
			Result.Purpose.Preconditions = {
				TEXT("Copy the target unchanged from public content search or readback."),
				TEXT("Run in the Editor on the UE game thread with uai:query."),
			};
			Result.Purpose.ParameterGuidance = TEXT("Copy candidate.name and binding_revision unchanged into the matching AI behavior HJSON entry.");
			Result.Purpose.ResultGuidance = TEXT("Treat name, kind, binding_revision, target_revision, and placement contract as opaque exact values; continue paging while has_more is true.");
			Result.Purpose.FailureGuidance = TEXT("Unresolved targets, unsupported types, and invalid pages fail before any write.");
			Result.Purpose.SideEffectGuidance = TEXT("Read-only; it creates no node, package, file, or external state.");
			Result.Purpose.MinimumExample = TEXT("client.ai.behavior_candidates(target, query='Sequence')");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.search"),
				TEXT("query.command.detail"),
				TEXT("content_graph.summary"),
				TEXT("content_graph.import_workspace"),
				TEXT("content_graph.items"),
			};
			Result.Purpose.Keywords = {TEXT("ai"), TEXT("behavior tree"), TEXT("state tree"), TEXT("blackboard"), TEXT("eqs"), TEXT("smart object"), TEXT("candidate"), TEXT("hjson")};
			Result.Purpose.Synonyms = {TEXT("find behavior node"), TEXT("resolve state tree node"), TEXT("resolve blackboard key"), TEXT("resolve eqs generator"), TEXT("resolve smart object behavior")};
			return Result;
		}

		class FAiBehaviorCandidateContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			explicit FAiBehaviorCandidateContribution(
				TSharedRef<IUeAgentAiBehaviorCandidateCommandService> InService)
				: Service(MoveTemp(InService))
			{
			}

			virtual const FString& GetContributionId() const override
			{
				static const FString Id = TEXT("ai-behavior-domain-candidates");
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
				return TryCreateAiBehaviorCandidateCommandHandlers(Service, OutHandlers, OutError);
			}

		private:
			TSharedRef<IUeAgentAiBehaviorCandidateCommandService> Service;
		};

		const FUeAgentActiveCommandContributionFactoryRegistration CandidateRegistration(
			TEXT("ai-behavior-domain-candidates"),
			&TryCreateProductionAiBehaviorCandidateContribution);
	}

	const FString& GetProviderRegistrationId()
	{
		return ProviderRegistrationId;
	}

	bool TryCreateProductionAiBehaviorCandidateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
	{
		TSharedPtr<IUeAgentAiBehaviorCandidateCommandService> Service;
		if (!TryCreateProductionAiBehaviorCandidateCommandService(Service, OutError)
			|| !Service.IsValid())
		{
			OutContribution.Reset();
			return false;
		}
		OutContribution = MakeShared<FAiBehaviorCandidateContribution>(Service.ToSharedRef());
		OutError.Reset();
		return true;
	}
}
