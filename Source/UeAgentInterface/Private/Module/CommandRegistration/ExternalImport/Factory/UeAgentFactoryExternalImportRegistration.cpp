#include "Module/CommandRegistration/ExternalImport/Factory/UeAgentFactoryExternalImportRegistration.h"

#include "CommandHandlers/ExternalImport/Factory/UeAgentFactoryExternalImportCommandHandlers.h"

namespace UE::UAI::Module::CommandRegistration::ExternalImport::Factory
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandHandlers::ExternalImport::Factory;
	using namespace CommandRegistry;
	using namespace Module::CommandRegistration::Active;
	using namespace OperationOwners::ExternalImport::Factory;
	using DomainAdapters::ExternalImport::Factory::
		EFactoryExternalImportKind;
	using DomainAdapters::ExternalImport::Factory::
		EFactoryExternalImportOperation;

	namespace
	{
		const FString Owner = TEXT("factory-external-import-owner");

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
			const FUeAgentFactoryExternalImportCommandRoute& Route)
		{
			FUeAgentCommandDeclaration Result;
			Result.CommandId = Route.CommandId;
			Result.PythonNamespace = TEXT("t28_external_factory_import");
			Result.PythonFunction = Route.PythonFunction;
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::AssetState;
			Result.StateLayers = {TEXT("asset_persisted_state")};
			Result.InputFields = {
				Field(
					TEXT("source_ref"),
					TEXT("string"),
					true,
					TEXT("Exact external source path; the same identity is retained in persisted ImportData readback.")),
			};
			if (Route.Operation == EFactoryExternalImportOperation::Import)
			{
				Result.InputFields.Add(Field(
					TEXT("destination_ref"),
					TEXT("string"),
					true,
					TEXT("Exact /Game destination package identity for a new generated asset.")));
				if (Route.Kind == EFactoryExternalImportKind::FbxAnimation)
				{
					Result.InputFields.Add(Field(
						TEXT("skeleton_ref"),
						TEXT("string"),
						true,
						TEXT("Exact existing Skeleton object identity consumed by the FBX animation factory.")));
				}
			}
			else
			{
				Result.InputFields.Add(Field(
					TEXT("target_ref"),
					TEXT("string"),
					true,
					TEXT("Exact persisted asset identity to reimport through its formal reimport handler.")));
			}
			Result.InputFields.Add(Field(
				TEXT("options"),
				TEXT("object"),
				false,
				TEXT("Bounded factory options. FBX static mesh combines source nodes into one asset and supports import_materials and import_textures. Texture supports srgb, compression_settings, mip_gen_settings and lod_group. FBX skeletal mesh supports skeleton_ref, import_morph_targets, import_animations, update_skeleton_reference_pose and expected result validation. Unknown or inapplicable options fail before mutation.")));
			Result.ResultFields = {
				Field(TEXT("command_id"), TEXT("string"), true,
					TEXT("The unchanged exact public command identity.")),
				Field(TEXT("terminal_status"), TEXT("string"), true,
					TEXT("The terminal business outcome after the factory lifecycle.")),
				Field(TEXT("readback"), TEXT("object"), true,
					TEXT("Small readable business readback: target/source references, asset class and relevant option/state summaries.")),
			};
			Result.Errors = {
				Error(TEXT("factory_import_authority_required"),
					TEXT("The public uai:operate authority is absent.")),
				Error(TEXT("factory_import_source_not_found"),
					TEXT("The exact external source does not exist.")),
				Error(TEXT("factory_import_option_unknown"),
					TEXT("An option is not part of the bounded factory contract.")),
				Error(TEXT("factory_import_destination_conflict"),
					TEXT("A create destination already exists.")),
				Error(TEXT("factory_reimport_type_mismatch"),
					TEXT("The persisted reimport target is not the route's exact asset class.")),
				Error(TEXT("factory_import_native_failed"),
					TEXT("The formal UE factory did not create the expected asset.")),
				Error(TEXT("factory_reimport_native_failed"),
					TEXT("The formal UE reimport handler rejected the source.")),
				Error(TEXT("factory_import_readback_mismatch"),
					TEXT("Reloaded ImportData or persisted provenance differs from the request.")),
				Error(TEXT("factory_import_rollback_unconfirmed"),
					TEXT("A post-mutation failure could not prove cleanup or restoration.")),
			};
			Result.Purpose.Summary =
				Route.Operation == EFactoryExternalImportOperation::Import
					? TEXT("Create one external-content asset through its formal UE factory and shared lifecycle.")
					: TEXT("Reimport one persisted external-content asset through its formal UE reimport handler and shared lifecycle.");
			Result.Purpose.Target =
				TEXT("One exact persisted asset and one exact external source identity.");
			Result.Purpose.Preconditions = {
				TEXT("Use the exact public command identity and contract hash unchanged."),
				TEXT("The external source extension must match the selected factory route."),
				TEXT("Create source basename must match the destination asset name."),
			};
			Result.Purpose.ParameterGuidance =
				TEXT("Pass source, destination/target, skeleton and bounded options without aliases or SDK-side domain mapping.");
			Result.Purpose.ResultGuidance =
				TEXT("Trust success only when the terminal, persistence receipt and independent ImportData readback are present.");
			Result.Purpose.FailureGuidance =
				TEXT("Prewrite failures do not mutate; post-mutation failures roll back or report state_unknown without fallback.");
			Result.Purpose.SideEffectGuidance =
				TEXT("Creates or reimports and saves one asset; related factory packages share the same lifecycle and cleanup boundary.");
			Result.Purpose.MinimumExample = Route.PythonFunction
				+ TEXT("(client, source_ref, destination_or_target_ref, options={})");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.search"),
				TEXT("query.command.usage"),
				TEXT("asset persisted-state query"),
			};
			Result.Purpose.Keywords = {
				TEXT("external import"),
				TEXT("factory"),
				TEXT("reimport"),
				TEXT("ImportData"),
			};
			Result.Purpose.Synonyms = {
				TEXT("import source file"),
				TEXT("refresh imported asset"),
			};
			return Result;
		}

		class FContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			explicit FContribution(
				TSharedRef<IUeAgentFactoryExternalImportCommandService> InService)
				: Service(MoveTemp(InService))
			{
			}

			virtual const FString& GetContributionId() const override
			{
				static const FString Id = TEXT("external-import-factory");
				return Id;
			}

			virtual void CollectDeclarations(
				TArray<FUeAgentCommandDeclaration>& OutDeclarations) const override
			{
				for (const FUeAgentFactoryExternalImportCommandRoute& Route
					: GetFactoryExternalImportCommandRoutes())
				{
					OutDeclarations.Add(Declaration(Route));
				}
			}

			virtual bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
				UE::UAI::Contract::FDiagnostic& OutError) const override
			{
				TArray<TSharedRef<IUeAgentExactCommandHandler>> Handlers;
				if (!TryCreateFactoryExternalImportCommandHandlers(
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
			TSharedRef<IUeAgentFactoryExternalImportCommandService> Service;
		};

		const FUeAgentActiveCommandContributionFactoryRegistration
			GFactoryExternalImportContributionRegistration(
				TEXT("external-import-factory"),
				&TryCreateProductionFactoryExternalImportContribution);
	}

	bool TryCreateFactoryExternalImportContribution(
		TSharedRef<IUeAgentFactoryExternalImportCommandService> Service,
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutContribution = MakeShared<FContribution>(Service);
		OutError.Reset();
		return true;
	}

	bool TryCreateProductionFactoryExternalImportContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutContribution.Reset();
		TSharedPtr<IUeAgentFactoryExternalImportCommandService> Service;
		if (!TryCreateProductionFactoryExternalImportCommandService(
				Service,
				OutError))
		{
			return false;
		}
		return TryCreateFactoryExternalImportContribution(
			Service.ToSharedRef(),
			OutContribution,
			OutError);
	}

	bool TryCreateFactoryExternalImportRegistration(
		TSharedRef<IUeAgentFactoryExternalImportCommandService> Service,
		TSharedPtr<const FUeAgentImmutableCommandRegistry>& OutRegistry,
		TSharedPtr<const FUeAgentExactCommandDispatch>& OutDispatch,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRegistry.Reset();
		OutDispatch.Reset();
		OutError.Reset();
		TSharedPtr<IUeAgentActiveCommandContribution> Contribution;
		FUeAgentActiveCommandComposition Composition;
		if (!TryCreateFactoryExternalImportContribution(
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
