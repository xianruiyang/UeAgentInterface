#include "Module/CommandRegistration/UnifiedGraphLayout/UeAgentUnifiedGraphLayoutRegistration.h"

#include "CommandHandlers/UnifiedGraphLayout/UeAgentUnifiedGraphLayoutCommandHandler.h"

namespace UE::UAI::Module::CommandRegistration::UnifiedGraphLayout
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandRegistry;

	namespace
	{
		class FContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			const FString& GetContributionId() const override
			{
				static const FString Id(TEXT("unified-graph-layout"));
				return Id;
			}
			void CollectDeclarations(
				TArray<FUeAgentCommandDeclaration>& OutDeclarations) const override
			{
				FUeAgentCommandDeclaration Declaration;
				Declaration.CommandId = TEXT("graph.layout.apply");
				Declaration.ContractVersion = 1;
				Declaration.PublicOwner = TEXT("unified-graph-layout-owner");
				Declaration.Availability = EUeAgentCommandAvailability::Enabled;
				Declaration.SideEffect = EUeAgentCommandSideEffect::AssetState;
				Declaration.StateLayers = {TEXT("persisted")};
				Declaration.InputFields = {
					{TEXT("targets"), TEXT("array"), true, false,
						TEXT("One or more exact graph identities with expected_revision.")},
					{TEXT("strategy"), TEXT("string"), true, false,
						TEXT("The shared layout strategy identity.")},
					{TEXT("parameters"), TEXT("object"), true, false,
						TEXT("Bounded strategy parameters such as spacing.")},
					{TEXT("authorization_id"), TEXT("string"), true, false,
						TEXT("Authorization bound to the complete target impact plan.")},
					{TEXT("projection"), TEXT("object"), false, false,
						TEXT("Optional bounded response projection.")},
				};
				Declaration.ResultFields = {
					{TEXT("terminal_status"), TEXT("string"), true, false,
						TEXT("Succeeded only after save and independent layout readback.")},
					{TEXT("impact_plan"), TEXT("object"), true, false,
						TEXT("The complete multi-target impact and atomicity plan.")},
					{TEXT("independent_readback"), TEXT("array"), true, false,
						TEXT("Persisted node positions and revisions for every target.")},
				};
				Declaration.Errors = {
					{TEXT("unified_graph_layout_authority_required"),
						TEXT("The complete impact plan is not authorized.")},
					{TEXT("unified_graph_layout_target_duplicate"),
						TEXT("A graph identity appears more than once.")},
					{TEXT("unified_graph_layout_target_unsupported"),
						TEXT("The exact graph identity is unknown or unsupported.")},
					{TEXT("unified_graph_layout_stale_revision"),
						TEXT("A target revision changed before the first write.")},
					{TEXT("unified_graph_layout_save_failed"),
						TEXT("The shared owner could not persist all target packages.")},
				};
				Declaration.Purpose.Summary =
					TEXT("Lay out one or more exact editable graphs through one public owner.");
				Declaration.Purpose.Target =
					TEXT("Exact Blueprint, Animation Blueprint, Material, Niagara, PCG, AI, Audio, or Control Rig graph identities.");
				Declaration.Purpose.Preconditions = {
					TEXT("Use graph identities and revisions returned by current query/readback paths."),
					TEXT("Authorize the complete targets array before execution."),
				};
				Declaration.Purpose.ParameterGuidance =
					TEXT("Do not pass provider names; routing is derived from exact graph type.");
				Declaration.Purpose.ResultGuidance =
					TEXT("Verify every target in independent_readback and retain the impact plan.");
				Declaration.Purpose.FailureGuidance =
					TEXT("Unknown, duplicate, unsupported, or stale targets fail before the first write.");
				Declaration.Purpose.SideEffectGuidance =
					TEXT("The owner plans all targets, applies provider mechanics, saves packages, and reads positions back.");
				Declaration.Purpose.MinimumExample =
					TEXT("client.graph.layout_apply(targets, strategy, parameters, authorization_id)");
				Declaration.Purpose.RelatedFlows = {
					TEXT("query.command.search"), TEXT("query.command.detail"),
					TEXT("content_graph.structure"),
				};
				Declaration.Purpose.Keywords = {
					TEXT("graph"), TEXT("layout"), TEXT("multi target"),
				};
				Declaration.Purpose.Synonyms = {
					TEXT("auto layout"), TEXT("arrange graph"),
				};
				OutDeclarations.Add(MoveTemp(Declaration));
			}
			bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
				FString& OutError) const override
			{
				TSharedPtr<IUeAgentExactCommandHandler> Handler;
				if (!CommandHandlers::UnifiedGraphLayout::TryCreateHandler(
						Handler, OutError)
					|| !Handler.IsValid())
				{
					return false;
				}
				OutHandlers.Add(Handler.ToSharedRef());
				return true;
			}
		};

		const FUeAgentActiveCommandContributionFactoryRegistration Registration(
			TEXT("unified-graph-layout"), &TryCreateContribution);
	}

	bool TryCreateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
	{
		OutError.Reset();
		OutContribution = MakeShared<FContribution>();
		return OutContribution.IsValid();
	}
}
