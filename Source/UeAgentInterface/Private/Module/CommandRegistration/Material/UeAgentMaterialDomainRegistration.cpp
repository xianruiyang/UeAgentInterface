#include "Module/CommandRegistration/Material/UeAgentMaterialDomainRegistration.h"

#include "CommandHandlers/Material/UeAgentMaterialDomainCommandHandlers.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "OperationOwners/Material/UeAgentMaterialDomainService.h"

namespace UE::UAI::Module::CommandRegistration::Material
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandHandlers::Material;
	using namespace CommandRegistry;
	using namespace OperationOwners::Material;

	namespace
	{
		const FString ProviderRegistrationId = TEXT("material-domain-provider");
		const FString CandidateCommandId = TEXT("material.expression.candidates");
		const FString CandidateOwner = TEXT("material-domain-owner");
		const HjsonWorkspace::FImportInitializationDomainProviderFactoryRegistration
			ProviderRegistration(
			TEXT("material-domain-candidates"),
			TEXT("material-domain-provider"),
			&OperationOwners::Material::TryCreateMaterialDomainProvider);

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
			Result.PythonNamespace = TEXT("material");
			Result.PythonFunction = TEXT("expression_candidates");
			Result.ContractVersion = 1;
			Result.PublicOwner = CandidateOwner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::None;
			Result.StateLayers = {TEXT("asset_persisted_state")};
			Result.InputFields = {
				Field(TEXT("target"), TEXT("string"), true,
					TEXT("Exact persisted Material or Material Function package.object identity.")),
				Field(TEXT("query"), TEXT("string"), false,
					TEXT("Optional bounded label or exact creation-name filter.")),
				Field(TEXT("graph_name"), TEXT("string"), false,
					TEXT("Optional exact MaterialGraph or MaterialFunctionGraph selector; defaults from target type.")),
				Field(TEXT("node_guid"), TEXT("string"), false,
					TEXT("Optional exact graph-node GUID containing the FromPin.")),
				Field(TEXT("pin_guid"), TEXT("string"), false,
					TEXT("Optional exact FromPin GUID for context-sensitive native actions.")),
				Field(TEXT("pin_name"), TEXT("string"), false,
					TEXT("Optional FromPin name; ambiguous names fail closed.")),
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
					TEXT("The unchanged exact target identity.")),
				Field(TEXT("target_revision"), TEXT("string"), true,
					TEXT("The current persisted target revision.")),
				Field(TEXT("candidates"), TEXT("array"), true,
					TEXT("Material expression candidates implementing the complete candidate-name contract.")),
				Field(TEXT("total_matches"), TEXT("integer"), true,
					TEXT("Total candidates matching this query.")),
				Field(TEXT("next_cursor"), TEXT("integer"), true,
					TEXT("Next result offset, or -1 when complete.")),
				Field(TEXT("complete"), TEXT("boolean"), true,
					TEXT("True only when this response includes the final page.")),
				Field(TEXT("has_more"), TEXT("boolean"), true,
					TEXT("True only when another page remains.")),
				Field(TEXT("coverage_scope"), TEXT("string"), true,
					TEXT("Exact native Material action subset represented by this command.")),
				Field(TEXT("coverage_complete"), TEXT("boolean"), true,
					TEXT("True only when the stated native action scope is fully enumerated.")),
				Field(TEXT("query_context"), TEXT("object"), true,
					TEXT("The exact graph and optional node/pin context used for native enumeration.")),
			};
			Result.Errors = {
				{TEXT("material_candidate_query_authority_required"),
					TEXT("The uai:query authority is missing.")},
				{TEXT("material_candidate_target_unresolved"),
					TEXT("The exact target could not be loaded.")},
				{TEXT("material_candidate_target_type_unsupported"),
					TEXT("The target is not a Material or Material Function.")},
				{TEXT("material_candidate_page_invalid"),
					TEXT("The requested page is outside the bounded contract.")},
				{TEXT("material_candidate_palette_query_failed"),
					TEXT("The real Material palette could not be queried.")},
			};
			Result.Purpose.Summary =
				TEXT("Resolve exact Material expression creation candidates from the real Material palette.");
			Result.Purpose.Target =
				TEXT("One exact persisted Material or Material Function graph owner.");
			Result.Purpose.Preconditions = {
				TEXT("Copy the target unchanged from public content search or readback."),
				TEXT("Run in the Editor on the UE game thread with uai:query."),
			};
			Result.Purpose.ParameterGuidance =
				TEXT("Search in the exact target Graph/Pin context, then put only candidate.name or candidate.id in the Material HJSON candidate object; keep graph and pin selectors in sibling candidate_context when needed.");
			Result.Purpose.ResultGuidance =
				TEXT("Use candidate.id when a name is ambiguous. Continue paging while has_more is true and do not claim native-menu coverage unless coverage_complete is true for the required scope.");
			Result.Purpose.FailureGuidance =
				TEXT("Unresolved targets, stale bindings, and unsupported types fail before any write and never fall back to reference.catalog_search.");
			Result.Purpose.SideEffectGuidance =
				TEXT("Read-only; it creates no expression, package, file, or external state.");
			Result.Purpose.MinimumExample =
				TEXT("client.material.expression_candidates(target, query='Multiply', graph_name='MaterialGraph')");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.search"),
				TEXT("query.command.detail"),
				TEXT("content_graph.summary"),
				TEXT("content_graph.import_workspace"),
			};
			Result.Purpose.Keywords = {
				TEXT("material"), TEXT("expression"), TEXT("candidate"),
				TEXT("hjson"), TEXT("palette"),
			};
			Result.Purpose.Synonyms = {
				TEXT("find material node"),
				TEXT("resolve material expression"),
			};
			return Result;
		}

		class FMaterialCandidateContribution final
			: public IUeAgentActiveCommandContribution
		{
		public:
			explicit FMaterialCandidateContribution(
				TSharedRef<IUeAgentMaterialCandidateCommandService> InService)
				: Service(MoveTemp(InService))
			{
			}

			virtual const FString& GetContributionId() const override
			{
				static const FString Id = TEXT("material-domain-candidates");
				return Id;
			}

			virtual void CollectDeclarations(
				TArray<FUeAgentCommandDeclaration>& OutDeclarations) const
				override
			{
				OutDeclarations.Add(CandidateDeclaration());
			}

			virtual bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
				FString& OutError) const override
			{
				(void)Registry;
				return TryCreateMaterialCandidateCommandHandlers(
					Service, OutHandlers, OutError);
			}

		private:
			TSharedRef<IUeAgentMaterialCandidateCommandService> Service;
		};

		const FUeAgentActiveCommandContributionFactoryRegistration
			CandidateRegistration(
				TEXT("material-domain-candidates"),
				&TryCreateProductionMaterialCandidateContribution);
	}

	const FString& GetProviderRegistrationId()
	{
		return ProviderRegistrationId;
	}

	bool TryCreateProductionMaterialCandidateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
	{
		TSharedPtr<IUeAgentMaterialCandidateCommandService> Service;
		if (!TryCreateProductionMaterialCandidateCommandService(
				Service, OutError)
			|| !Service.IsValid())
		{
			OutContribution.Reset();
			return false;
		}
		OutContribution = MakeShared<FMaterialCandidateContribution>(
			Service.ToSharedRef());
		OutError.Reset();
		return true;
	}
}
