#include "Module/CommandRegistration/WorldSpatial/UeAgentLevelWorldDomainRegistration.h"

#include "CommandHandlers/WorldSpatial/UeAgentLevelWorldCommandHandlers.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "OperationOwners/WorldSpatial/UeAgentLevelWorldDomainService.h"

namespace UE::UAI::Module::CommandRegistration::WorldSpatial
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandHandlers::WorldSpatial;
	using namespace CommandRegistry;

	namespace
	{
		const FString ProviderRegistrationId = TEXT("level-world-domain-provider");
		const HjsonWorkspace::FImportInitializationDomainProviderFactoryRegistration
			ProviderRegistration(
				TEXT("spatial-content-operation"),
				TEXT("level-world-domain-provider"),
				&OperationOwners::WorldSpatial::TryCreateLevelWorldDomainProvider);

		const FString CandidateCommandId = TEXT("world.actor.candidates");
		const FString CandidateOwner = TEXT("level-world-domain-owner");

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
			Result.PythonNamespace = TEXT("world");
			Result.PythonFunction = TEXT("actor_candidates");
			Result.ContractVersion = 1;
			Result.PublicOwner = CandidateOwner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::None;
			Result.StateLayers = {TEXT("editor_registry_state"),
				TEXT("asset_persisted_state")};
			Result.InputFields = {
				Field(TEXT("target"), TEXT("string"), true,
					TEXT("Exact persisted World package.object identity that will own the Actor.")),
				Field(TEXT("category"), TEXT("string"), false,
					TEXT("Optional exact native Place Actors category handle or display name.")),
				Field(TEXT("query"), TEXT("string"), false,
					TEXT("Optional native Place Actors text-filter expression; active text search uses AllClasses like the editor UI.")),
				Field(TEXT("id"), TEXT("string"), false,
					TEXT("Optional exact compact candidate id returned by an earlier page.")),
				Field(TEXT("limit"), TEXT("integer"), false,
					TEXT("Optional page size from 1 through 20; defaults to 20.")),
				Field(TEXT("cursor"), TEXT("integer"), false,
					TEXT("Optional non-negative result offset; defaults to zero.")),
			};
			Result.ResultFields = {
				Field(TEXT("command_id"), TEXT("string"), true,
					TEXT("The unchanged exact public command identity.")),
				Field(TEXT("read_only"), TEXT("boolean"), true,
					TEXT("Always true.")),
				Field(TEXT("target"), TEXT("string"), true,
					TEXT("The unchanged target World identity.")),
				Field(TEXT("context"), TEXT("object"), true,
					TEXT("World path, native placement category and exact registry surface used.")),
				Field(TEXT("catalog_revision"), TEXT("string"), true,
					TEXT("Opaque fingerprint of the matching native candidate set.")),
				Field(TEXT("candidates"), TEXT("array"), true,
					TEXT("Native FPlaceableItem entries; name or id is directly consumable by Actor HJSON candidate.")),
				Field(TEXT("total_matching"), TEXT("integer"), true,
					TEXT("Total native candidates matching this context and query.")),
				Field(TEXT("next_cursor"), TEXT("integer"), true,
					TEXT("Next result offset, or -1 when complete.")),
				Field(TEXT("complete"), TEXT("boolean"), true,
					TEXT("True only when this is the final page.")),
				Field(TEXT("has_more"), TEXT("boolean"), true,
					TEXT("True only while another page remains.")),
				Field(TEXT("warnings"), TEXT("array"), true,
					TEXT("Typed native categories that cannot yet execute headlessly.")),
			};
			Result.Errors = {
				{TEXT("world_actor_candidate_query_authority_required"),
					TEXT("The uai:query authority is missing.")},
				{TEXT("world_actor_candidate_target_unresolved"),
					TEXT("The exact persisted World identity does not exist.")},
				{TEXT("world_actor_candidate_page_invalid"),
					TEXT("The requested limit or cursor is outside the bounded contract.")},
				{TEXT("world_actor_candidate_query_failed"),
					TEXT("The native Place Actors registry could not be enumerated.")},
			};
			Result.Purpose.Summary =
				TEXT("Search the same native registry and text filter used by the Place Actors UI.");
			Result.Purpose.Target =
				TEXT("One persisted World target plus one native Placement Mode context.");
			Result.Purpose.Preconditions = {
				TEXT("Copy target unchanged from current World state or content search."),
				TEXT("Use category only when reproducing an explicit Place Actors tab context."),
			};
			Result.Purpose.ParameterGuidance =
				TEXT("Search and page the native registry, then copy only candidate.name or candidate.id into actors[].candidate; copy context.placement_category to actors[].placement_context.category when it is not AllClasses.");
			Result.Purpose.ResultGuidance =
				TEXT("name is sufficient when unique in the returned native context; id preserves category, factory, asset and Actor class. Richer fields are optional assertions, not required creation fields.");
			Result.Purpose.FailureGuidance =
				TEXT("Unknown, ambiguous, stale, unsupported custom-drag or assertion-mismatched candidates fail before persistent mutation; no class-only fallback is used for candidate HJSON.");
			Result.Purpose.SideEffectGuidance =
				TEXT("Read-only; it opens no editor window, selects nothing and creates no Actor.");
			Result.Purpose.MinimumExample =
				TEXT("client.world.actor_candidates(target=world, query='Cube', limit=10)");
			Result.Purpose.RelatedFlows = {
				TEXT("query.content.search"),
				TEXT("content_graph.import_workspace"),
				TEXT("blueprint.component.candidates"),
			};
			Result.Purpose.Keywords = {
				TEXT("world"), TEXT("actor"), TEXT("place actors"),
				TEXT("candidate"), TEXT("hjson"), TEXT("factory"),
			};
			Result.Purpose.Synonyms = {
				TEXT("search Place Actors"),
				TEXT("resolve Actor creation item"),
			};
			return Result;
		}

		class FCandidateContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			virtual const FString& GetContributionId() const override
			{
				static const FString Id = TEXT("level-world-actor-candidates");
				return Id;
			}

			virtual void CollectDeclarations(
				TArray<FUeAgentCommandDeclaration>& OutDeclarations) const override
			{
				OutDeclarations.Add(CandidateDeclaration());
			}

			virtual bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry>,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
				FString& OutError) const override
			{
				return TryCreateWorldSpatialCommandHandlers(
					OutHandlers, OutError);
			}
		};

		const FUeAgentActiveCommandContributionFactoryRegistration
			CandidateRegistration(
				TEXT("level-world-actor-candidates"),
				&TryCreateLevelWorldCandidateContribution);
	}

	const FString& GetProviderRegistrationId()
	{
		return ProviderRegistrationId;
	}

	bool TryCreateLevelWorldCandidateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
	{
		OutContribution = MakeShared<FCandidateContribution>();
		OutError.Reset();
		return true;
	}
}
