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
				TEXT("Copy candidate.name and binding_revision unchanged into the Material HJSON expression candidate object.");
			Result.Purpose.ResultGuidance =
				TEXT("Treat name, binding_revision, target_revision, and contract references as opaque exact values; continue paging while has_more is true.");
			Result.Purpose.FailureGuidance =
				TEXT("Unresolved targets, stale bindings, and unsupported types fail before any write and never fall back to reference.catalog_search.");
			Result.Purpose.SideEffectGuidance =
				TEXT("Read-only; it creates no expression, package, file, or external state.");
			Result.Purpose.MinimumExample =
				TEXT("client.material.expression_candidates(target, query='Scalar Parameter')");
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
