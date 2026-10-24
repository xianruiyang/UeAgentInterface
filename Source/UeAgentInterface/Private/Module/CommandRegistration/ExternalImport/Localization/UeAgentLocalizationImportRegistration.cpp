#include "Module/CommandRegistration/ExternalImport/Localization/UeAgentLocalizationImportRegistration.h"

#include "CommandHandlers/ExternalImport/Localization/UeAgentLocalizationImportCommandHandlers.h"

namespace UE::UAI::Module::CommandRegistration::ExternalImport::Localization
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandHandlers::ExternalImport::Localization;
	using namespace CommandRegistry;
	using namespace Module::CommandRegistration::Active;
	using namespace OperationOwners::ExternalImport::Localization;

	namespace
	{
		const FString Owner = TEXT("localization-import-owner");

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
			const FUeAgentLocalizationImportCommandRoute& Route)
		{
			FUeAgentCommandDeclaration Result;
			Result.CommandId = Route.CommandId;
			Result.PythonNamespace = TEXT("t30_external_localization_import");
			Result.PythonFunction = Route.PythonFunction;
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::ExternalState;
			Result.StateLayers = {
				TEXT("project_localization_files"),
				TEXT("editor_localization_resources"),
			};
			Result.InputFields = {
				Field(TEXT("source_ref"), TEXT("string"), true,
					TEXT("Exact external .po file; its Language header selects one supported target culture.")),
				Field(TEXT("target_ref"), TEXT("string"), true,
					TEXT("Exact existing project Localization Target name.")),
			};
			Result.ResultFields = {
				Field(TEXT("command_id"), TEXT("string"), true,
					TEXT("The unchanged exact public command identity.")),
				Field(TEXT("terminal_status"), TEXT("string"), true,
					TEXT("Succeeded only after formal import, compile, refresh and independent file readback.")),
				Field(TEXT("target_ref"), TEXT("string"), true,
					TEXT("Resolved project Localization Target name.")),
				Field(TEXT("culture"), TEXT("string"), true,
					TEXT("Imported culture selected from the PO Language header.")),
				Field(TEXT("source_ref"), TEXT("string"), true,
					TEXT("The imported external PO source reference.")),
				Field(TEXT("collapse_mode"), TEXT("string"), true,
					TEXT("Business import collapse mode.")),
				Field(TEXT("po_format"), TEXT("string"), true,
					TEXT("Business PO format detected by the importer.")),
				Field(TEXT("target_resolved"), TEXT("boolean"), true,
					TEXT("Whether the requested target resolved.")),
				Field(TEXT("formal_importer_completed"), TEXT("boolean"), true,
					TEXT("Whether formal PO import completed.")),
				Field(TEXT("formal_compiler_completed"), TEXT("boolean"), true,
					TEXT("Whether formal culture compilation completed.")),
				Field(TEXT("refresh_completed"), TEXT("boolean"), true,
					TEXT("Whether editor localization resources were refreshed.")),
			};
			Result.Errors = {
				Error(TEXT("localization_import_authority_required"),
					TEXT("The public uai:operate authority is absent.")),
				Error(TEXT("localization_import_source_not_found"),
					TEXT("The exact .po source does not exist.")),
				Error(TEXT("localization_import_po_culture_invalid"),
					TEXT("The .po Language header does not identify a valid culture.")),
				Error(TEXT("localization_import_target_missing"),
					TEXT("The exact project Localization Target is absent.")),
				Error(TEXT("localization_import_culture_unsupported"),
					TEXT("The source culture is not registered on the target.")),
				Error(TEXT("localization_import_manifest_missing"),
					TEXT("The target manifest required by UE's formal importer is absent.")),
				Error(TEXT("localization_import_native_import_failed"),
					TEXT("UE's formal culture importer rejected the staged PO.")),
				Error(TEXT("localization_import_compile_failed"),
					TEXT("UE's formal culture compiler did not produce the target locres.")),
				Error(TEXT("localization_import_refresh_failed"),
					TEXT("The editor localization resource revision did not advance.")),
				Error(TEXT("localization_import_readback_mismatch"),
					TEXT("Persisted PO, archive, locres, options or refresh state differs from the request.")),
				Error(TEXT("localization_import_rollback_unconfirmed"),
					TEXT("A post-mutation failure could not restore every owned target file.")),
			};
			Result.Purpose.Summary =
				TEXT("Import one PO culture through its dedicated project Localization Target and UE commandlet mechanics.");
			Result.Purpose.Target =
				TEXT("One existing project Localization Target, one supported culture and one exact PO source.");
			Result.Purpose.Preconditions = {
				TEXT("The project Localization Target and its manifest already exist."),
				TEXT("The PO Language header names a culture registered on the target."),
			};
			Result.Purpose.ParameterGuidance =
				TEXT("Pass the source path and exact target name unchanged; culture and UE import options come from the PO and target.");
			Result.Purpose.ResultGuidance =
				TEXT("Trust success only with the terminal status and the returned culture/resource business results.");
			Result.Purpose.FailureGuidance =
				TEXT("Prewrite failures do not mutate; post-mutation failures restore PO/archive/locres/report files or report state_unknown.");
			Result.Purpose.SideEffectGuidance =
				TEXT("Persists the target PO, archive, compiled locres and report data, then refreshes editor resources.");
			Result.Purpose.MinimumExample =
				TEXT("localization_po_import(client, source_ref, target_ref)");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.search"),
				TEXT("query.command.usage"),
				TEXT("Localization Dashboard target configuration"),
			};
			Result.Purpose.Keywords = {
				TEXT("localization"),
				TEXT("PO"),
				TEXT("translation"),
				TEXT("refresh"),
			};
			Result.Purpose.Synonyms = {
				TEXT("import PO"),
				TEXT("import translations"),
			};
			return Result;
		}

		class FContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			explicit FContribution(
				TSharedRef<IUeAgentLocalizationImportCommandService> InService)
				: Service(MoveTemp(InService))
			{
			}

			virtual const FString& GetContributionId() const override
			{
				static const FString Id = TEXT("external-import-localization");
				return Id;
			}

			virtual void CollectDeclarations(
				TArray<FUeAgentCommandDeclaration>& OutDeclarations) const override
			{
				for (const FUeAgentLocalizationImportCommandRoute& Route
					: GetLocalizationImportCommandRoutes())
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
				if (!TryCreateLocalizationImportCommandHandlers(
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
			TSharedRef<IUeAgentLocalizationImportCommandService> Service;
		};

		const FUeAgentActiveCommandContributionFactoryRegistration
			GLocalizationImportContributionRegistration(
				TEXT("external-import-localization"),
				&TryCreateProductionLocalizationImportContribution);
	}

	bool TryCreateLocalizationImportContribution(
		TSharedRef<IUeAgentLocalizationImportCommandService> Service,
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutContribution = MakeShared<FContribution>(Service);
		OutError.Reset();
		return true;
	}

	bool TryCreateProductionLocalizationImportContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutContribution.Reset();
		TSharedPtr<IUeAgentLocalizationImportCommandService> Service;
		if (!TryCreateProductionLocalizationImportCommandService(
				Service,
				OutError))
		{
			return false;
		}
		return TryCreateLocalizationImportContribution(
			Service.ToSharedRef(),
			OutContribution,
			OutError);
	}

	bool TryCreateLocalizationImportRegistration(
		TSharedRef<IUeAgentLocalizationImportCommandService> Service,
		TSharedPtr<const FUeAgentImmutableCommandRegistry>& OutRegistry,
		TSharedPtr<const FUeAgentExactCommandDispatch>& OutDispatch,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRegistry.Reset();
		OutDispatch.Reset();
		OutError.Reset();
		TSharedPtr<IUeAgentActiveCommandContribution> Contribution;
		FUeAgentActiveCommandComposition Composition;
		if (!TryCreateLocalizationImportContribution(
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
