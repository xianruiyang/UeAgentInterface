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
				Field(TEXT("query"), TEXT("string"), false, TEXT("Optional bounded search over the exact target creation surface.")),
				Field(TEXT("id"), TEXT("string"), false, TEXT("Optional exact compact candidate id returned for a native creation context.")),
				Field(TEXT("limit"), TEXT("integer"), false, TEXT("Optional page size from 1 through 64; defaults to 64.")),
				Field(TEXT("cursor"), TEXT("integer"), false, TEXT("Optional non-negative result offset; defaults to zero.")),
			};
			Result.ResultFields = {
				Field(TEXT("command_id"), TEXT("string"), true, TEXT("The unchanged exact public command identity.")),
				Field(TEXT("read_only"), TEXT("boolean"), true, TEXT("Always true.")),
				Field(TEXT("target"), TEXT("string"), true, TEXT("The unchanged exact target identity.")),
				Field(TEXT("target_revision"), TEXT("string"), true, TEXT("The current persisted target revision.")),
				Field(TEXT("candidates"), TEXT("array"), true, TEXT("Native compact BehaviorTree blank-menu actions, or explicitly marked legacy class identities for AI contexts not yet migrated.")),
				Field(TEXT("total_matches"), TEXT("integer"), true, TEXT("Total candidates matching this query.")),
				Field(TEXT("next_cursor"), TEXT("integer"), true, TEXT("Next result offset, or -1 when complete.")),
				Field(TEXT("complete"), TEXT("boolean"), true, TEXT("True only on the final page.")),
				Field(TEXT("has_more"), TEXT("boolean"), true, TEXT("True only while another page remains.")),
				Field(TEXT("coverage_scope"), TEXT("string"), true, TEXT("The exact native context or legacy compatibility surface represented by this result.")),
				Field(TEXT("coverage_complete"), TEXT("boolean"), true, TEXT("True only when the complete native creation menu was projected.")),
				Field(TEXT("creation_coverage_complete"), TEXT("boolean"), true, TEXT("True only when every projected native menu action has an HJSON execution contract.")),
				Field(TEXT("native_action_count"), TEXT("integer"), true, TEXT("Complete native action count before query filtering, or zero for a legacy compatibility surface.")),
				Field(TEXT("executable_action_count"), TEXT("integer"), true, TEXT("Native actions currently executable through the matching HJSON authoring path.")),
				Field(TEXT("graph_name"), TEXT("string"), false, TEXT("The target editor graph name when the native context already exists, or the transient schema-context graph name used for read-only enumeration.")),
			};
			Result.Errors = {
				{TEXT("ai_behavior_candidate_query_authority_required"), TEXT("The uai:query authority is missing.")},
				{TEXT("ai_behavior_candidate_target_unresolved"), TEXT("The exact target could not be loaded.")},
				{TEXT("ai_behavior_candidate_target_type_unsupported"), TEXT("The target is not an exact supported AI behavior root.")},
				{TEXT("ai_behavior_candidate_page_invalid"), TEXT("The requested page is outside the bounded contract.")},
				{TEXT("ai_behavior_candidate_native_menu_shape_mismatch"), TEXT("The UE 5.6 native BehaviorTree menu no longer matches the fail-closed class/action mapping contract.")},
				{TEXT("ai_behavior_candidate_identity_ambiguous"), TEXT("The complete native action set did not yield unique compact ids.")},
			};
			Result.Purpose.Summary = TEXT("Search the same UE BehaviorTree blank-background menu used by the Editor and expose truthful legacy compatibility results for AI contexts not yet migrated.");
			Result.Purpose.Target = TEXT("One exact persisted BehaviorTree, BlackboardData, EnvQuery, StateTree, or SmartObjectDefinition.");
			Result.Purpose.Preconditions = {
				TEXT("Copy the target unchanged from public content search or readback."),
				TEXT("Run in the Editor on the UE game thread with uai:query."),
			};
			Result.Purpose.ParameterGuidance = TEXT("For a BehaviorTree blank graph, search like the Editor right-click menu or pass id for one exact action. Put only candidate.name or candidate.id in HJSON; graph, semantic node identity, placement and optional assertions stay outside candidate. Other AI target types still return legacy bindings until their native contexts are migrated.");
			Result.Purpose.ResultGuidance = TEXT("Copy one unique BehaviorTree name or exact compact id. Continue paging while has_more is true. coverage_complete describes native search coverage; creation_coverage_complete separately describes executable menu actions. Do not treat legacy compatibility results as native-menu equivalence.");
			Result.Purpose.FailureGuidance = TEXT("Unresolved targets, unavailable native contexts, menu-shape drift, identity collisions and invalid pages fail without graph mutation.");
			Result.Purpose.SideEffectGuidance = TEXT("Content read-only: it may create a transient schema context and load native action metadata, but creates no persisted node, package, file, asset editor or external state.");
			Result.Purpose.MinimumExample = TEXT("client.ai.behavior_candidates(target, query='Wait')");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.search"),
				TEXT("query.command.detail"),
				TEXT("content_graph.summary"),
				TEXT("content_graph.import_workspace"),
				TEXT("content_graph.items"),
			};
			Result.Purpose.Keywords = {TEXT("ai"), TEXT("behavior tree"), TEXT("state tree"), TEXT("blackboard"), TEXT("eqs"), TEXT("smart object"), TEXT("candidate"), TEXT("hjson")};
			Result.Purpose.Synonyms = {TEXT("find behavior node"), TEXT("search behavior tree right click menu"), TEXT("resolve state tree node"), TEXT("resolve blackboard key"), TEXT("resolve eqs generator"), TEXT("resolve smart object behavior")};
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
