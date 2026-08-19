#include "Module/CommandRegistration/BlueprintComponent/UeAgentBlueprintComponentRegistration.h"

#include "CommandHandlers/BlueprintComponent/UeAgentBlueprintComponentCommandHandlers.h"

namespace UE::UAI::Module::CommandRegistration::BlueprintComponent
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandHandlers::BlueprintComponent;
	using namespace UE::UAI::CommandRegistry;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::OperationOwners::BlueprintComponent;

	namespace
	{
		const FString CommandId = TEXT("blueprint.component.candidates");
		const FString Owner = TEXT("blueprint-component-authoring-owner");

		FUeAgentCommandFieldDeclaration Field(
			const TCHAR* Name,
			const TCHAR* WireType,
			const bool bRequired,
			const TCHAR* Description)
		{
			return {Name, WireType, bRequired, false, Description};
		}

		FUeAgentCommandDeclaration Declaration()
		{
			FUeAgentCommandDeclaration Result;
			Result.CommandId = CommandId;
			Result.PythonNamespace = TEXT("blueprint");
			Result.PythonFunction = TEXT("component_candidates");
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::None;
			Result.StateLayers = {TEXT("asset_persisted_state")};
			Result.InputFields = {
				Field(TEXT("target"), TEXT("string"), true,
					TEXT("Exact public Blueprint package.object identity.")),
				Field(TEXT("component_class"), TEXT("string"), true,
					TEXT("Exact supported SCS Component class identity.")),
				Field(TEXT("requested_name"), TEXT("string"), true,
					TEXT("Proposed Component variable name preserved into HJSON.")),
			};
			Result.ResultFields = {
				Field(TEXT("command_id"), TEXT("string"), true,
					TEXT("The unchanged exact public command identity.")),
				Field(TEXT("read_only"), TEXT("boolean"), true,
					TEXT("Always true.")),
				Field(TEXT("target"), TEXT("string"), true,
					TEXT("The unchanged exact Blueprint identity.")),
				Field(TEXT("scs_revision"), TEXT("string"), true,
					TEXT("Current persisted SCS revision for optimistic fencing.")),
				Field(TEXT("candidates"), TEXT("array"), true,
					TEXT("One bounded candidate whose name is the exact creation identity.")),
				Field(TEXT("existing_components"), TEXT("array"), true,
					TEXT("Independent current SCS readback used after reload.")),
				Field(TEXT("complete"), TEXT("boolean"), true,
					TEXT("True for the bounded supported Component set.")),
				Field(TEXT("has_more"), TEXT("boolean"), true,
					TEXT("Always false.")),
			};
			Result.Errors = {
				{TEXT("blueprint_component_query_authority_required"),
					TEXT("The uai:query scope is missing.")},
				{TEXT("blueprint_component_target_unresolved"),
					TEXT("The exact Blueprint identity could not be resolved.")},
				{TEXT("blueprint_component_class_not_registered"),
					TEXT("The Component class has no T12 initialization contract.")},
				{TEXT("blueprint_component_name_invalid"),
					TEXT("The proposed Component name is invalid or already owned.")},
			};
			Result.Purpose.Summary =
				TEXT("Resolve one exact Blueprint SCS Component creation candidate and current SCS readback.");
			Result.Purpose.Target =
				TEXT("One exact persisted Blueprint SCS graph.");
			Result.Purpose.Preconditions = {
				TEXT("Copy target unchanged from public Blueprint asset creation or search."),
				TEXT("Use one registered exact Component class and a unique requested name."),
			};
			Result.Purpose.ParameterGuidance =
				TEXT("Copy candidate.name, component_class, binding_revision and scs_revision unchanged into the T12 HJSON import.");
			Result.Purpose.ResultGuidance =
				TEXT("Treat candidate identity and revisions as opaque optimistic fences; existing_components is current persisted readback.");
			Result.Purpose.FailureGuidance =
				TEXT("Unregistered classes, stale bindings and duplicate names fail before mutation; there is no alias fallback.");
			Result.Purpose.SideEffectGuidance =
				TEXT("Read-only with no persistent or external side effects.");
			Result.Purpose.MinimumExample =
				TEXT("client.blueprint.component_candidates(target=target, component_class='/Script/Engine.SceneComponent', requested_name='SceneRoot')");
			Result.Purpose.RelatedFlows = {
				TEXT("blueprint.asset.create"),
				TEXT("content_graph.import_workspace"),
				TEXT("blueprint.node.candidates"),
			};
			Result.Purpose.Keywords = {
				TEXT("blueprint"), TEXT("component"), TEXT("candidate"),
				TEXT("scs"), TEXT("hjson"),
			};
			Result.Purpose.Synonyms = {
				TEXT("add Blueprint Component"),
				TEXT("resolve SCS Component"),
			};
			return Result;
		}

		class FContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			explicit FContribution(
				TSharedRef<IUeAgentBlueprintComponentCommandService> InService)
				: Service(MoveTemp(InService))
			{
			}

			virtual const FString& GetContributionId() const override
			{
				static const FString Id = TEXT("blueprint-component");
				return Id;
			}

			virtual void CollectDeclarations(
				TArray<FUeAgentCommandDeclaration>& OutDeclarations) const override
			{
				OutDeclarations.Add(Declaration());
			}

			virtual bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
				FString& OutError) const override
			{
				return TryCreateBlueprintComponentCommandHandlers(
					Service, OutHandlers, OutError);
			}

		private:
			TSharedRef<IUeAgentBlueprintComponentCommandService> Service;
		};

		const FUeAgentActiveCommandContributionFactoryRegistration
			BlueprintComponentContributionRegistration(
				TEXT("blueprint-component"),
				&TryCreateProductionBlueprintComponentContribution);
	}

	bool TryCreateBlueprintComponentContribution(
		TSharedRef<IUeAgentBlueprintComponentCommandService> Service,
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
	{
		OutContribution = MakeShared<FContribution>(MoveTemp(Service));
		OutError.Reset();
		return true;
	}

	bool TryCreateProductionBlueprintComponentContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
	{
		TSharedPtr<IUeAgentBlueprintComponentCommandService> Service;
		if (!TryCreateProductionBlueprintComponentCommandService(Service, OutError))
		{
			OutContribution.Reset();
			return false;
		}
		return TryCreateBlueprintComponentContribution(
			Service.ToSharedRef(), OutContribution, OutError);
	}

	bool TryCreateBlueprintComponentRegistration(
		TSharedRef<IUeAgentBlueprintComponentCommandService> Service,
		TSharedPtr<const FUeAgentImmutableCommandRegistry>& OutRegistry,
		TSharedPtr<const FUeAgentExactCommandDispatch>& OutDispatch,
		FString& OutError)
	{
		OutRegistry.Reset();
		OutDispatch.Reset();
		OutError.Reset();
		TSharedPtr<IUeAgentActiveCommandContribution> Contribution;
		FUeAgentActiveCommandComposition Composition;
		if (!TryCreateBlueprintComponentContribution(
				MoveTemp(Service), Contribution, OutError)
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
