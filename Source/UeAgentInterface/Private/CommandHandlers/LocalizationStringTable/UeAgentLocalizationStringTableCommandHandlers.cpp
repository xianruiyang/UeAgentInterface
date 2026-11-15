#include "CommandHandlers/LocalizationStringTable/UeAgentLocalizationStringTableCommandHandlers.h"

#include "DomainAdapters/LocalizationStringTable/UeAgentLocalizationStringTableHjsonProvider.h"

namespace UE::UAI::CommandHandlers::LocalizationStringTable
{
	bool TryHandleDomainRequest(
		const HjsonWorkspace::FImportInitializationDomainRequest& Request,
		HjsonWorkspace::FImportInitializationDomainResponse& OutResponse,
		HjsonWorkspace::FWorkspaceError& OutError)
	{
		FString Payload;
		bool bReadOnly = false;
		if (!DomainAdapters::LocalizationStringTable::TryExecuteContentGraph(
				Request.GetCommandId(),
				Request.GetTargetIdentity(),
				Request.GetRootTypeIdentity(),
				Request.GetCanonicalParamsJson(),
				Payload,
				bReadOnly,
				OutError,
				Request.GetWorkspaceExportPreparation().Get()))
		{
			return false;
		}
		OutResponse = HjsonWorkspace::FImportInitializationDomainResponse(
			MoveTemp(Payload), bReadOnly);
		return OutResponse.IsValid();
	}
}
