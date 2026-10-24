#include "Module/CommandRegistration/AiBehavior/UeAgentAiBehaviorDomainRegistration.h"

#include "CommandHandlers/AiBehavior/UeAgentAiBehaviorDomainCommandHandlers.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
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
		const FString DomainId = TEXT("ai-behavior-domain-candidates");
		const FString ProviderRegistrationId = TEXT("ai-behavior-domain-provider");
		const FString CandidateCommandId = TEXT("ai.behavior.candidates");
		const FString CandidateOwner = TEXT("ai-behavior-domain-owner");

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
				Field(TEXT("target"), TEXT("string"), true, TEXT("Persisted BehaviorTree, BlackboardData, EnvQuery, StateTree, or SmartObjectDefinition name from content search, or its exact package.object path.")),
				Field(TEXT("query"), TEXT("string"), false, TEXT("Optional search over the exact target creation surface.")),
				Field(TEXT("parent"), TEXT("string"), false, TEXT("BehaviorTree parent node alias from the exported graph; required together with attachment_kind.")),
				Field(TEXT("attachment_kind"), TEXT("string"), false, TEXT("BehaviorTree native parent menu: service or decorator; requires parent.")),
				Field(TEXT("graph_owner"), TEXT("string"), false, TEXT("Copy the owner reference from an exported CompositeDecorator graph to search its native inner-node menu; exclusive with parent and attachment_kind.")),
				Field(TEXT("describe"), TEXT("object"), false, TEXT("Optional {name, select?, fields?} in the same target and attachment/graph context. Returns creation.supported/reason; editable fields require a created item.")),
			};
			Result.ResultFields = {
				Field(TEXT("scope"), TEXT("object"), true, TEXT("Public target and supplied parent/attachment_kind or graph_owner context; response metadata, not a selector.")),
				Field(TEXT("names"), TEXT("array"), true, TEXT("Ordered, readable AI behavior names.")),
				Field(TEXT("description"), TEXT("object"), false, TEXT("Selected action's current HJSON creation support, only when describe is requested.")),
			};
			Result.Errors = {
				{TEXT("ai_behavior_candidate_query_authority_required"), TEXT("The uai:query authority is missing.")},
				{TEXT("ai_behavior_candidate_target_unresolved"), TEXT("The exact target could not be loaded.")},
				{TEXT("ai_behavior_candidate_target_type_unsupported"), TEXT("The target is not an exact supported AI behavior root.")},
				{TEXT("ai_behavior_candidate_machine_selector_not_supported"), TEXT("Machine identity selectors are not accepted; use the returned readable name.")},
				{TEXT("ai_behavior_candidate_native_menu_shape_mismatch"), TEXT("The UE 5.6 native BehaviorTree menu no longer matches the fail-closed class/action mapping contract.")},
				{TEXT("ai_behavior_candidate_identity_ambiguous"), TEXT("The complete native action set did not yield unique readable semantic names.")},
			};
			Result.Purpose.Summary = TEXT("Search the UE BehaviorTree blank-background or parent attachment menu; other AI contexts retain their documented compatibility boundary.");
			Result.Purpose.Target = TEXT("One exact persisted BehaviorTree, BlackboardData, EnvQuery, StateTree, or SmartObjectDefinition.");
			Result.Purpose.Preconditions = {
				TEXT("Copy the target unchanged from public content search or readback."),
				TEXT("Run in the Editor on the UE game thread with uai:query."),
			};
			Result.Purpose.ParameterGuidance = TEXT("Omit context for the blank graph menu. For attachments, pass the exported parent alias and service or decorator. For an inner graph, copy its owner field unchanged into graph_owner. Copy the returned name into HJSON candidate.name; context belongs outside candidate.");
			Result.Purpose.ResultGuidance = TEXT("Default discovery contains matching names and one scope, including search-only actions. For an unfamiliar action request describe={name: returned_name} in the same context for creation.supported/reason. Editable fields resolve after creation. scope is response metadata, not a selector; re-query when target or context changes.");
			Result.Purpose.FailureGuidance = TEXT("Unresolved targets, unavailable native contexts, menu-shape drift and readable-name collisions fail without graph mutation.");
			Result.Purpose.SideEffectGuidance = TEXT("Content read-only: it may create a transient schema context and load native action metadata, but creates no persisted node, package, file, asset editor or external state.");
			Result.Purpose.MinimumExample = TEXT("client.ai.behavior_candidates(target, query='Wait')");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.search"),
				TEXT("query.command.usage"),
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
				UE::UAI::Contract::FDiagnostic& OutError) const override
			{
				(void)Registry;
				return TryCreateAiBehaviorCandidateCommandHandlers(Service, OutHandlers, OutError);
			}

		private:
			TSharedRef<IUeAgentAiBehaviorCandidateCommandService> Service;
		};

		bool TryCreateDomainCapability(
			TSharedPtr<const DomainCapability::FDomainCapabilityContribution>&
				OutContribution,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>
				Provider;
			HjsonWorkspace::FWorkspaceError WorkspaceError;
			if (!OperationOwners::AiBehavior::TryCreateAiBehaviorDomainProvider(
					Provider, WorkspaceError)
				|| !Provider.IsValid())
			{
				OutError = WorkspaceError.GetDiagnostic();
				return false;
			}
			DomainCapability::FDomainCapabilityDeclaration Declaration =
				DomainCapability::MakeExplicitlyUnsupportedDomainCapabilityDeclaration(
					DomainId);
			Declaration.Hjson.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Hjson.Providers.Add(Provider.ToSharedRef());
			Declaration.Hjson.Routes = {
				{ProviderRegistrationId, TEXT("content_graph.import_workspace"),
					TEXT("uai.ai_behavior.edit/1"), TEXT("ai_behavior"),
					TEXT("/Script/AIModule.BehaviorTree"),
					HjsonWorkspace::EInitializationNodeRole::RootTarget,
					TEXT("uai.init.ai.behavior_tree.root/1"), true},
				{ProviderRegistrationId, TEXT("content_graph.import_workspace"),
					TEXT("uai.ai_behavior.edit/1"), TEXT("ai_behavior"),
					TEXT("/Script/AIModule.BlackboardData"),
					HjsonWorkspace::EInitializationNodeRole::RootTarget,
					TEXT("uai.init.ai.blackboard.root/1"), true},
				{ProviderRegistrationId, TEXT("content_graph.import_workspace"),
					TEXT("uai.ai_behavior.edit/1"), TEXT("ai_behavior"),
					TEXT("/Script/AIModule.EnvQuery"),
					HjsonWorkspace::EInitializationNodeRole::RootTarget,
					TEXT("uai.init.ai.eqs.root/1"), true},
				{ProviderRegistrationId, TEXT("content_graph.import_workspace"),
					TEXT("uai.ai_behavior.edit/1"), TEXT("ai_behavior"),
					TEXT("/Script/SmartObjectsModule.SmartObjectDefinition"),
					HjsonWorkspace::EInitializationNodeRole::RootTarget,
					TEXT("uai.init.ai.smart_object_definition.root/1"), true},
				{ProviderRegistrationId, TEXT("content_graph.import_workspace"),
					TEXT("uai.ai_behavior.edit/1"), TEXT("ai_behavior"),
					TEXT("/Script/StateTreeModule.StateTree"),
					HjsonWorkspace::EInitializationNodeRole::RootTarget,
					TEXT("uai.init.ai.state_tree.root/1"), true},
			};
			Declaration.Initialization.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Initialization.ContractIds = {
				TEXT("uai.init.ai.behavior_tree.root/1"),
				TEXT("uai.init.ai.blackboard.root/1"),
				TEXT("uai.init.ai.eqs.root/1"),
				TEXT("uai.init.ai.smart_object_definition.root/1"),
				TEXT("uai.init.ai.state_tree.root/1"),
				TEXT("uai.init.ai.behavior_node/1"),
				TEXT("uai.init.ai.behavior_service/1"),
				TEXT("uai.init.ai.behavior_decorator/1"),
				TEXT("uai.init.ai.state_tree_node/1"),
				TEXT("uai.init.ai.blackboard_entry/1"),
				TEXT("uai.init.ai.connection/1"),
			};
			return DomainCapability::FDomainCapabilityContribution::TryCreate(
				MoveTemp(Declaration), OutContribution, OutError);
		}

		const DomainCapability::FDomainCapabilityContributionFactoryRegistration
			DomainCapabilityRegistration(*DomainId, &TryCreateDomainCapability);

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
		UE::UAI::Contract::FDiagnostic& OutError)
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
