#include "OperationOwners/LocalizationStringTable/UeAgentLocalizationStringTableDomainService.h"

#include "CommandHandlers/LocalizationStringTable/UeAgentLocalizationStringTableCommandHandlers.h"
#include "Internationalization/StringTable.h"
#include "LocalizationSettings.h"

namespace UE::UAI::OperationOwners::LocalizationStringTable
{
	using namespace HjsonWorkspace;

	namespace
	{
		const FString ProviderId = TEXT("localization-string-table-hjson-provider");
		const FString LocalizationSchema = TEXT("uai.localization.edit/1");
		const FString StringTableSchema = TEXT("uai.string_table.edit/1");
		const FString LocalizationContract = TEXT("uai.init.localization.settings.root/1");
		const FString StringTableContract = TEXT("uai.init.localization.string_table.root/1");

		class FProvider final : public IImportInitializationDomainProvider
		{
		public:
			const FString& GetProviderId() const override { return ProviderId; }

			bool Matches(const FImportInitializationDomainRequest& Request) const override
			{
				const FString& Type = Request.GetRootTypeIdentity();
				const bool bLocalization = Type == ULocalizationSettings::StaticClass()->GetPathName();
				const bool bStringTable = Type == UStringTable::StaticClass()->GetPathName();
				if (Request.GetRole() != EInitializationNodeRole::RootTarget
					|| (!bLocalization && !bStringTable))
				{
					return false;
				}
				return Request.GetCommandId() != TEXT("content_graph.import_workspace")
					|| Request.GetSchemaId() == (bLocalization ? LocalizationSchema : StringTableSchema);
			}

			bool TryResolveInitializationContract(
				const FImportInitializationDomainRequest& Request,
				FString& OutInitializationContractId,
				FWorkspaceError& OutError) const override
			{
				OutInitializationContractId.Reset();
				OutError.Reset();
				if (!Matches(Request)) return false;
				OutInitializationContractId =
					Request.GetRootTypeIdentity() == ULocalizationSettings::StaticClass()->GetPathName()
						? LocalizationContract : StringTableContract;
				return true;
			}

			bool TryExecute(
				const FImportInitializationDomainRequest& Request,
				const FString& InitializationContractId,
				FImportInitializationDomainResponse& OutResponse,
				FWorkspaceError& OutError) const override
			{
				const FString& RootType = Request.GetRootTypeIdentity();
				const bool bExpected =
					(RootType == ULocalizationSettings::StaticClass()->GetPathName()
						&& InitializationContractId == LocalizationContract)
					|| (RootType == UStringTable::StaticClass()->GetPathName()
						&& InitializationContractId == StringTableContract);
				if (!bExpected)
				{
					OutError.Set(TEXT("localization_hjson_contract_not_executable"), InitializationContractId);
					return false;
				}
				return CommandHandlers::LocalizationStringTable::TryHandleDomainRequest(
					Request, OutResponse, OutError);
			}
		};
	}

	bool TryCreateLocalizationStringTableDomainProvider(
		TSharedPtr<IImportInitializationDomainProvider>& OutProvider,
		FWorkspaceError& OutError)
	{
		OutProvider = MakeShared<FProvider>();
		OutError.Reset();
		return true;
	}
}
