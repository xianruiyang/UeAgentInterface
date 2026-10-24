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
					TEXT("Exact AssetSimple create command whose compatible factory names are requested.")),
				Field(
					TEXT("query"), TEXT("string"), false,
					TEXT("Optional bounded factory name filter.")),
			};
			Result.ResultFields = {
				Field(TEXT("scope"), TEXT("object"), true, TEXT("The create_command whose compatible factories were queried; response metadata, not a selector.")),
				Field(TEXT("names"), TEXT("array"), true,
					TEXT("Compatible factory names for the selected create command.")),
			};
			Result.Errors = {
				{TEXT("asset_factory_candidate_authority_required"),
					TEXT("The uai:query authority is missing.")},
				{TEXT("asset_factory_candidate_command_unsupported"),
					TEXT("The command does not consume a factory candidate selected by readable name.")},
				{TEXT("asset_factory_candidate_catalog_unavailable"),
					TEXT("The process-owned factory catalog could not be resolved.")},
			};
			Result.Purpose.Summary = TEXT(
				"Find compatible factory names for one exact asset create command.");
			Result.Purpose.Target =
				TEXT("The process-owned AssetSimple catalog, including native New Asset factories and registered static or domain constructors for asset.object.create.");
			Result.Purpose.Preconditions = {
				TEXT("Select an exact create command from query.command.search or detail."),
				TEXT("Run in the Editor on the UE game thread with uai:query."),
			};
			Result.Purpose.ParameterGuidance = TEXT(
				"Pass the exact create command. Use asset.object.create for the unified constructor catalog; use query only to narrow compatible candidates.");
			Result.Purpose.ResultGuidance = TEXT(
				"Use a returned name with scope.create_command; query again if the create command changes. scope is response metadata, not a selector. Ambiguous names require only the documented short semantic selection.");
			Result.Purpose.FailureGuidance =
				TEXT("Unsupported commands and unavailable catalogs fail before mutation; only use a returned readable factory name.");
			Result.Purpose.SideEffectGuidance =
				TEXT("Read-only; it creates no asset, package, file, window, or external state.");
			Result.Purpose.MinimumExample = TEXT(
				"client.asset.factory_candidates('asset.object.create', query='world')");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.search"),
				TEXT("query.command.usage"),
				TEXT("asset.object.create"),
				TEXT("asset.media_source.create"),
			};
			Result.Purpose.Keywords = {
				TEXT("asset"), TEXT("factory"), TEXT("candidate"),
				TEXT("generic"), TEXT("native factory"), TEXT("new asset"),
				TEXT("level"), TEXT("map"), TEXT("material"),
				TEXT("media source"),
			};
			Result.Purpose.Synonyms = {
				TEXT("find asset factory"),
				TEXT("resolve factory ref"),
				TEXT("find native new asset factory"),
				TEXT("find level or map factory"),
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
				UE::UAI::Contract::FDiagnostic& OutError) const override
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutContribution = MakeShared<FContribution>();
		OutError.Reset();
		return true;
	}
}
