#include "Module/CommandRegistration/AnimationRigGraph/UeAgentAnimationRigGraphDomainRegistration.h"

#include "CommandHandlers/AnimationRigGraph/UeAgentAnimationRigGraphDomainCommandHandlers.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "OperationOwners/AnimationRigGraph/UeAgentAnimationRigGraphDomainService.h"

namespace UE::UAI::Module::CommandRegistration::AnimationRigGraph
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandHandlers::AnimationRigGraph;
	using namespace CommandRegistry;
	using namespace OperationOwners::AnimationRigGraph;

	namespace
	{
		const FString ProviderRegistrationId = TEXT("animation-rig-graph-domain-provider");
		const FString CandidateCommandId =
			TEXT("animation_rig_graph.candidates");
		const FString CandidateOwner = TEXT("animation-rig-graph-domain-owner");
		const HjsonWorkspace::FImportInitializationDomainProviderFactoryRegistration ProviderRegistration(
			TEXT("animation-rig-graph-domain-candidates"),
			TEXT("animation-rig-graph-domain-provider"),
			&OperationOwners::AnimationRigGraph::TryCreateAnimationRigGraphDomainProvider);

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
			Result.PythonNamespace = TEXT("animation_rig_graph");
			Result.PythonFunction = TEXT("candidates");
			Result.ContractVersion = 1;
			Result.PublicOwner = CandidateOwner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::None;
			Result.StateLayers = {TEXT("asset_persisted_state")};
			Result.InputFields = {
				Field(TEXT("target"), TEXT("string"), true, TEXT("Exact persisted ControlRigBlueprint package.object identity whose default RigVM graph supplies the native blank-context menu.")),
				Field(TEXT("query"), TEXT("string"), false, TEXT("Optional text filter over the native menu name, category, tooltip, keywords, and spawner type.")),
				Field(TEXT("id"), TEXT("string"), false, TEXT("Optional exact opaque candidate id filter.")),
				Field(TEXT("limit"), TEXT("integer"), false, TEXT("Optional page size from 1 through 64; defaults to 64.")),
				Field(TEXT("cursor"), TEXT("integer"), false, TEXT("Optional non-negative result offset; defaults to zero.")),
			};
			Result.ResultFields = {
				Field(TEXT("command_id"), TEXT("string"), true, TEXT("The unchanged exact public command identity.")),
				Field(TEXT("read_only"), TEXT("boolean"), true, TEXT("Always true.")),
				Field(TEXT("target"), TEXT("string"), true, TEXT("The unchanged exact target identity.")),
				Field(TEXT("target_revision"), TEXT("string"), true, TEXT("The current persisted target revision.")),
				Field(TEXT("graph_name"), TEXT("string"), true, TEXT("The exact default RigVM editor graph used for native menu enumeration.")),
				Field(TEXT("coverage_scope"), TEXT("string"), true, TEXT("The explicit native creation context covered by this result.")),
				Field(TEXT("coverage_complete"), TEXT("boolean"), true, TEXT("True when every native action in the covered menu context is projected.")),
				Field(TEXT("creation_coverage_complete"), TEXT("boolean"), true, TEXT("True only when every projected native action kind is executable by the current HJSON writer.")),
				Field(TEXT("native_action_count"), TEXT("integer"), true, TEXT("Total native menu actions in the covered context.")),
				Field(TEXT("executable_action_count"), TEXT("integer"), true, TEXT("Native actions currently executable by the HJSON writer.")),
				Field(TEXT("candidates"), TEXT("array"), true, TEXT("Compact native RigVM menu actions with opaque id, UI name, presentation, and truthful write support.")),
				Field(TEXT("total_matches"), TEXT("integer"), true, TEXT("Total candidates matching this query.")),
				Field(TEXT("next_cursor"), TEXT("integer"), true, TEXT("Next result offset, or -1 when complete.")),
				Field(TEXT("complete"), TEXT("boolean"), true, TEXT("True only on the final page.")),
				Field(TEXT("has_more"), TEXT("boolean"), true, TEXT("True only while another page remains.")),
			};
			Result.Errors = {
				{TEXT("animation_rig_graph_candidate_query_authority_required"), TEXT("The uai:query authority is missing.")},
				{TEXT("animation_rig_graph_candidate_target_unresolved"), TEXT("The exact target could not be loaded.")},
				{TEXT("animation_rig_graph_candidate_target_type_unsupported"), TEXT("The target is not an exact supported animation rig graph root.")},
				{TEXT("animation_rig_graph_candidate_page_invalid"), TEXT("The requested page is outside the bounded contract.")},
			};
			Result.Purpose.Summary = TEXT("Enumerate the same native blank-context action menu used by the default Control Rig graph and return compact contextual-create candidates.");
			Result.Purpose.Target = TEXT("One exact persisted ControlRigBlueprint default RigVM graph.");
			Result.Purpose.Preconditions = {
				TEXT("Copy the target unchanged from public content search or readback."),
				TEXT("Run in the Editor on the UE game thread with uai:query."),
			};
			Result.Purpose.ParameterGuidance = TEXT("Search by query or exact id, then put only candidate.name or candidate.id in the create entry; graph context and position remain outside candidate.");
			Result.Purpose.ResultGuidance = TEXT("Continue paging while has_more is true; supported=false entries are exact native search results whose HJSON persistence path is not yet available.");
			Result.Purpose.FailureGuidance = TEXT("Unresolved targets, unsupported types, and invalid pages fail before any write.");
			Result.Purpose.SideEffectGuidance = TEXT("Read-only; it creates no node, package, file, or external state.");
			Result.Purpose.MinimumExample = TEXT("client.animation_rig_graph.candidates(target, query='Add Spaces')");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.search"),
				TEXT("query.command.detail"),
				TEXT("content_graph.summary"),
				TEXT("content_graph.import_workspace"),
				TEXT("animation.control_rig.hierarchy_graph.apply"),
				TEXT("animation.control_rig.shape_library.apply"),
				TEXT("animation.ik_rig.definition.apply"),
				TEXT("animation.ik_rig.definition.auto_generate"),
				TEXT("animation.ik_retargeter.definition.apply"),
				TEXT("animation.ik_retargeter.settings.apply"),
				TEXT("animation.skeleton.definition.apply"),
				TEXT("animation.skeleton.compatibility.apply"),
				TEXT("animation.skeleton.virtual_bone.apply"),
			};
			Result.Purpose.Keywords = {TEXT("animation"), TEXT("control rig"), TEXT("ik rig"), TEXT("skeleton"), TEXT("space"), TEXT("candidate"), TEXT("hjson")};
			Result.Purpose.Synonyms = {TEXT("find rig unit"), TEXT("resolve control rig node"), TEXT("resolve ik graph operation")};
			return Result;
		}

		class FAnimationRigGraphCandidateContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			explicit FAnimationRigGraphCandidateContribution(
				TSharedRef<IUeAgentAnimationRigGraphCandidateCommandService> InService)
				: Service(MoveTemp(InService))
			{
			}

			virtual const FString& GetContributionId() const override
			{
				static const FString Id = TEXT("animation-rig-graph-domain-candidates");
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
				return TryCreateAnimationRigGraphCandidateCommandHandlers(Service, OutHandlers, OutError);
			}

		private:
			TSharedRef<IUeAgentAnimationRigGraphCandidateCommandService> Service;
		};

		const FUeAgentActiveCommandContributionFactoryRegistration CandidateRegistration(
			TEXT("animation-rig-graph-domain-candidates"),
			&TryCreateProductionAnimationRigGraphCandidateContribution);
	}

	const FString& GetProviderRegistrationId()
	{
		return ProviderRegistrationId;
	}

	bool TryCreateProductionAnimationRigGraphCandidateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
	{
		TSharedPtr<IUeAgentAnimationRigGraphCandidateCommandService> Service;
		if (!TryCreateProductionAnimationRigGraphCandidateCommandService(Service, OutError)
			|| !Service.IsValid())
		{
			OutContribution.Reset();
			return false;
		}
		OutContribution = MakeShared<FAnimationRigGraphCandidateContribution>(Service.ToSharedRef());
		OutError.Reset();
		return true;
	}
}
