#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "DomainAdapters/ExternalImport/Localization/UeAgentLocalizationImportProvider.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::OperationOwners::ExternalImport::Localization
{
	struct UEAGENTINTERFACE_API FUeAgentLocalizationImportCommandRoute final
	{
		FString CommandId;
		FString PythonFunction;

		bool IsValid() const;
		bool TryValidate(UE::UAI::Contract::FDiagnostic& OutError) const;
	};

	UEAGENTINTERFACE_API const TArray<FUeAgentLocalizationImportCommandRoute>&
	GetLocalizationImportCommandRoutes();

	struct UEAGENTINTERFACE_API FUeAgentLocalizationImportCommandRequest
	{
		FString CommandId;
		FString RequestId;
		FString CanonicalParamsJson;
		FString CallerId;
		TArray<FString> AuthorityScopes;
		FString CorrelationId;
		bool bCancellationRequested = false;
	};

	struct UEAGENTINTERFACE_API FUeAgentLocalizationImportCommandResponse
	{
		FString PayloadJson;
	};

	using FUeAgentLocalizationImportCommandError = UE::UAI::Contract::FDiagnostic;

	class UEAGENTINTERFACE_API IUeAgentLocalizationImportCommandService
	{
	public:
		virtual ~IUeAgentLocalizationImportCommandService() = default;

		virtual bool Execute(
			const FUeAgentLocalizationImportCommandRequest& Request,
			FUeAgentLocalizationImportCommandResponse& OutResponse,
			FUeAgentLocalizationImportCommandError& OutError) = 0;
	};

	UEAGENTINTERFACE_API bool TryCreateProductionLocalizationImportCommandService(
		TSharedPtr<IUeAgentLocalizationImportCommandService>& OutService,
		UE::UAI::Contract::FDiagnostic& OutError);
}
