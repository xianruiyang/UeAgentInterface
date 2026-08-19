#include "Module/CommandRegistration/ExternalImport/SkinWeight/UeAgentSkinWeightImportRegistration.h"

#include "CommandHandlers/ExternalImport/SkinWeight/UeAgentSkinWeightImportCommandHandlers.h"

namespace UE::UAI::Module::CommandRegistration::ExternalImport::SkinWeight
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandHandlers::ExternalImport::SkinWeight;
	using namespace CommandRegistry;
	using namespace Module::CommandRegistration::Active;
	using namespace OperationOwners::ExternalImport::SkinWeight;

	namespace
	{
		const FString Owner = TEXT("skin-weight-import-owner");

		FUeAgentCommandFieldDeclaration Field(
			const TCHAR* Name,
			const TCHAR* WireType,
			const bool bRequired,
			const TCHAR* Description)
		{
			return {Name, WireType, bRequired, false, Description};
		}

		FUeAgentCommandErrorDeclaration Error(
			const TCHAR* Code,
			const TCHAR* Description)
		{
			return {Code, Description};
		}

		FUeAgentCommandDeclaration Declaration(
			const FUeAgentSkinWeightImportCommandRoute& Route)
		{
			FUeAgentCommandDeclaration Result;
			Result.CommandId = Route.CommandId;
			Result.PythonNamespace = TEXT("t29_external_skin_weight_import");
			Result.PythonFunction = Route.PythonFunction;
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::AssetState;
			Result.StateLayers = {TEXT("asset_persisted_state")};
			Result.InputFields = {
				Field(TEXT("source_ref"), TEXT("string"), true,
					TEXT("Exact FBX source identity retained in profile-scoped persisted metadata.")),
				Field(TEXT("target_ref"), TEXT("string"), true,
					TEXT("Exact existing persisted SkeletalMesh object identity.")),
				Field(TEXT("profile_name"), TEXT("string"), true,
					TEXT("New Skin Weight Profile identity; conflicts fail before mutation.")),
				Field(TEXT("lod_index"), TEXT("integer"), true,
					TEXT("Exact existing target LOD that receives the alternate weights.")),
			};
			Result.ResultFields = {
				Field(TEXT("command_id"), TEXT("string"), true,
					TEXT("The unchanged exact public command identity.")),
				Field(TEXT("terminal_status"), TEXT("string"), true,
					TEXT("Succeeded only after settle, save, unload/reload and profile verification.")),
				Field(TEXT("mutation_started"), TEXT("boolean"), true,
					TEXT("Whether the lifecycle entered the native importer.")),
				Field(TEXT("persistence_receipt_id"), TEXT("string"), true,
					TEXT("SHA-256 receipt for the persisted SkeletalMesh package.")),
				Field(TEXT("phase_history"), TEXT("array"), true,
					TEXT("Ordered shared OperationService lifecycle phases.")),
				Field(TEXT("independent_readback"), TEXT("object"), true,
					TEXT("Reloaded profile, source, LOD and package identity.")),
			};
			Result.Errors = {
				Error(TEXT("skin_weight_import_authority_required"),
					TEXT("The public uai:operate authority is absent.")),
				Error(TEXT("skin_weight_import_source_not_found"),
					TEXT("The exact FBX source does not exist.")),
				Error(TEXT("skin_weight_import_target_missing"),
					TEXT("The exact persisted SkeletalMesh target is absent.")),
				Error(TEXT("skin_weight_import_lod_invalid"),
					TEXT("The requested LOD does not exist on the target.")),
				Error(TEXT("skin_weight_import_profile_conflict"),
					TEXT("The requested profile already exists.")),
				Error(TEXT("skin_weight_import_native_failed"),
					TEXT("The formal UE alternate skin-weight importer rejected the source.")),
				Error(TEXT("skin_weight_import_readback_mismatch"),
					TEXT("Reloaded profile or source provenance differs from the request.")),
				Error(TEXT("skin_weight_import_rollback_unconfirmed"),
					TEXT("A post-mutation failure could not prove restoration.")),
			};
			Result.Purpose.Summary =
				TEXT("Import one Skin Weight Profile through its dedicated UE mechanics and shared lifecycle.");
			Result.Purpose.Target =
				TEXT("One existing SkeletalMesh LOD, one new profile identity and one FBX source.");
			Result.Purpose.Preconditions = {
				TEXT("The target package exists, is not dirty, and has the requested LOD."),
				TEXT("The profile name is absent and the FBX source exists."),
			};
			Result.Purpose.ParameterGuidance =
				TEXT("Pass source, target, profile and LOD unchanged; do not map them in the SDK.");
			Result.Purpose.ResultGuidance =
				TEXT("Trust success only with the persistence receipt and independent profile/source readback.");
			Result.Purpose.FailureGuidance =
				TEXT("Prewrite failures do not mutate; post-mutation failures restore the package or report state_unknown.");
			Result.Purpose.SideEffectGuidance =
				TEXT("Adds and persists one Skin Weight Profile on an existing SkeletalMesh package.");
			Result.Purpose.MinimumExample =
				TEXT("skin_weight_profile_import(client, source_ref, target_ref, profile_name, lod_index)");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.search"),
				TEXT("query.command.detail"),
				TEXT("asset persisted-state query"),
			};
			Result.Purpose.Keywords = {
				TEXT("skin weight"),
				TEXT("skeletal mesh"),
				TEXT("alternate weights"),
			};
			Result.Purpose.Synonyms = {
				TEXT("import skin weight profile"),
				TEXT("alternate skin weights"),
			};
			return Result;
		}

		class FContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			explicit FContribution(
				TSharedRef<IUeAgentSkinWeightImportCommandService> InService)
				: Service(MoveTemp(InService))
			{
			}

			virtual const FString& GetContributionId() const override
			{
				static const FString Id = TEXT("external-import-skin-weight");
				return Id;
			}

			virtual void CollectDeclarations(
				TArray<FUeAgentCommandDeclaration>& OutDeclarations) const override
			{
				for (const FUeAgentSkinWeightImportCommandRoute& Route
					: GetSkinWeightImportCommandRoutes())
				{
					OutDeclarations.Add(Declaration(Route));
				}
			}

			virtual bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
				FString& OutError) const override
			{
				TArray<TSharedRef<IUeAgentExactCommandHandler>> Handlers;
				if (!TryCreateSkinWeightImportCommandHandlers(
						Service,
						Handlers,
						OutError))
				{
					return false;
				}
				OutHandlers.Append(Handlers);
				return true;
			}

		private:
			TSharedRef<IUeAgentSkinWeightImportCommandService> Service;
		};

		const FUeAgentActiveCommandContributionFactoryRegistration
			GSkinWeightImportContributionRegistration(
				TEXT("external-import-skin-weight"),
				&TryCreateProductionSkinWeightImportContribution);
	}

	bool TryCreateSkinWeightImportContribution(
		TSharedRef<IUeAgentSkinWeightImportCommandService> Service,
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
	{
		OutContribution = MakeShared<FContribution>(Service);
		OutError.Reset();
		return true;
	}

	bool TryCreateProductionSkinWeightImportContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
	{
		OutContribution.Reset();
		TSharedPtr<IUeAgentSkinWeightImportCommandService> Service;
		if (!TryCreateProductionSkinWeightImportCommandService(Service, OutError))
		{
			return false;
		}
		return TryCreateSkinWeightImportContribution(
			Service.ToSharedRef(),
			OutContribution,
			OutError);
	}

	bool TryCreateSkinWeightImportRegistration(
		TSharedRef<IUeAgentSkinWeightImportCommandService> Service,
		TSharedPtr<const FUeAgentImmutableCommandRegistry>& OutRegistry,
		TSharedPtr<const FUeAgentExactCommandDispatch>& OutDispatch,
		FString& OutError)
	{
		OutRegistry.Reset();
		OutDispatch.Reset();
		OutError.Reset();
		TSharedPtr<IUeAgentActiveCommandContribution> Contribution;
		FUeAgentActiveCommandComposition Composition;
		if (!TryCreateSkinWeightImportContribution(
				Service,
				Contribution,
				OutError)
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
