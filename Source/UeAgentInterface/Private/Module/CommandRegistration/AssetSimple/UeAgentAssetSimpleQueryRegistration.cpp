#include "Module/CommandRegistration/AssetSimple/UeAgentAssetSimpleQueryRegistration.h"

#include "CommandHandlers/AssetSimple/UeAgentAssetSimpleQueryCommandHandlers.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "QueryOwners/AssetSimple/UeAgentAssetSimpleQueryBoundary.h"

namespace UE::UAI::Module::CommandRegistration::AssetSimple
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandHandlers::AssetSimple;
	using namespace CommandRegistry;

	namespace
	{
		const FString CommandId = TEXT("asset.factory.candidates");
		const FString Owner = TEXT("asset-simple-query-owner");

		FUeAgentCommandFieldDeclaration Field(
			const TCHAR* Name,
			const TCHAR* Type,
			const bool bRequired,
			const TCHAR* Description)
		{
			return {Name, Type, bRequired, false, Description};
		}

		FUeAgentCommandDeclaration Declaration()
		{
			FUeAgentCommandDeclaration Result;
			Result.CommandId = CommandId;
			Result.PythonNamespace = TEXT("asset");
			Result.PythonFunction = TEXT("factory_candidates");
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::None;
			Result.StateLayers = {TEXT("editor_state")};
			Result.InputFields = {
				Field(
					TEXT("create_command"), TEXT("string"), true,
					TEXT("Exact AssetSimple create command that consumes factory_ref.")),
				Field(
					TEXT("query"), TEXT("string"), false,
					TEXT("Optional bounded class, label, or candidate-ref filter.")),
				Field(
					TEXT("limit"), TEXT("integer"), false,
					TEXT("Optional page size from 1 through 20; defaults to 20.")),
				Field(
					TEXT("cursor"), TEXT("integer"), false,
					TEXT("Optional non-negative result offset; defaults to zero.")),
			};
			Result.ResultFields = {
				Field(TEXT("command_id"), TEXT("string"), true,
					TEXT("The unchanged query command identity.")),
				Field(TEXT("read_only"), TEXT("boolean"), true,
					TEXT("Always true.")),
				Field(TEXT("create_command"), TEXT("string"), true,
					TEXT("The unchanged selected create command identity.")),
				Field(TEXT("profile_id"), TEXT("string"), true,
					TEXT("The exact internal creation profile selected by the command.")),
				Field(TEXT("catalog_revision"), TEXT("object"), true,
					TEXT("Opaque source_id, generation, and sequence for this page.")),
				Field(TEXT("candidates"), TEXT("array"), true,
					TEXT("Candidates whose factory_ref object is directly consumable by create_command.")),
				Field(TEXT("total_matches"), TEXT("integer"), true,
					TEXT("Total matching candidates for this command and filter.")),
				Field(TEXT("next_cursor"), TEXT("integer"), true,
					TEXT("Next result offset, or -1 when complete.")),
				Field(TEXT("complete"), TEXT("boolean"), true,
					TEXT("True only when this response includes the final page.")),
				Field(TEXT("has_more"), TEXT("boolean"), true,
					TEXT("True only when another page remains.")),
			};
			Result.Errors = {
				{TEXT("asset_factory_candidate_authority_required"),
					TEXT("The uai:query authority is missing.")},
				{TEXT("asset_factory_candidate_command_unsupported"),
					TEXT("The command does not consume a factory_ref candidate.")},
				{TEXT("asset_factory_candidate_catalog_unavailable"),
					TEXT("The process-owned factory catalog could not be resolved.")},
				{TEXT("asset_factory_candidate_page_invalid"),
					TEXT("The requested page is outside the bounded contract.")},
			};
			Result.Purpose.Summary =
				TEXT("Resolve directly consumable factory_ref values for one exact typed asset create command.");
			Result.Purpose.Target =
				TEXT("The process-owned AssetSimple factory catalog used by mutation planning.");
			Result.Purpose.Preconditions = {
				TEXT("Select an exact create command from query.command.search or detail."),
				TEXT("Run in the Editor on the UE game thread with uai:query."),
			};
			Result.Purpose.ParameterGuidance =
				TEXT("Pass the exact create command; use query only to narrow its real compatible factory classes.");
			Result.Purpose.ResultGuidance =
				TEXT("Copy candidate.factory_ref unchanged into create_command; continue paging while has_more is true.");
			Result.Purpose.FailureGuidance =
				TEXT("Unsupported commands and unavailable catalogs fail before mutation; never synthesize candidate_ref or catalog_revision.");
			Result.Purpose.SideEffectGuidance =
				TEXT("Read-only; it creates no asset, package, file, window, or external state.");
			Result.Purpose.MinimumExample =
				TEXT("client.asset.factory_candidates('asset.media_source.create')");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.search"),
				TEXT("query.command.detail"),
				TEXT("asset.media_source.create"),
			};
			Result.Purpose.Keywords = {
				TEXT("asset"), TEXT("factory"), TEXT("candidate"),
				TEXT("media source"),
			};
			Result.Purpose.Synonyms = {
				TEXT("find asset factory"),
				TEXT("resolve factory ref"),
			};
			return Result;
		}

		class FContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			const FString& GetContributionId() const override
			{
				static const FString Id = TEXT("asset-simple-query");
				return Id;
			}

			void CollectDeclarations(
				TArray<FUeAgentCommandDeclaration>& OutDeclarations) const override
			{
				OutDeclarations.Add(Declaration());
			}

			bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
				FString& OutError) const override
			{
				(void)Registry;
				return TryCreateAssetSimpleQueryCommandHandlers(
					OutHandlers, OutError);
			}
		};

		const FUeAgentActiveCommandContributionFactoryRegistration Registration(
			TEXT("asset-simple-query"),
			&TryCreateAssetSimpleQueryContribution);
	}

	bool TryCreateAssetSimpleQueryContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
	{
		OutContribution = MakeShared<FContribution>();
		OutError.Reset();
		return QueryOwners::AssetSimple::
			GetAssetSimpleOwnedQueryCommandCount() == 1;
	}
}
