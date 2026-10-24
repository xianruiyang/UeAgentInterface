#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "DomainAdapters/ExternalImport/SkinWeight/UeAgentSkinWeightImportProvider.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::OperationOwners::ExternalImport::SkinWeight
{
	struct UEAGENTINTERFACE_API FUeAgentSkinWeightImportCommandRoute final
	{
		FString CommandId;
		FString PythonFunction;

		bool IsValid() const;
		bool TryValidate(UE::UAI::Contract::FDiagnostic& OutError) const;
	};

	UEAGENTINTERFACE_API const TArray<FUeAgentSkinWeightImportCommandRoute>&
	GetSkinWeightImportCommandRoutes();

	struct UEAGENTINTERFACE_API FUeAgentSkinWeightImportCommandRequest
	{
		FString CommandId;
		FString RequestId;
		FString CanonicalParamsJson;
		FString CallerId;
		TArray<FString> AuthorityScopes;
		FString CorrelationId;
		bool bCancellationRequested = false;
	};

	struct UEAGENTINTERFACE_API FUeAgentSkinWeightImportCommandResponse
	{
		FString PayloadJson;
	};

	using FUeAgentSkinWeightImportCommandError = UE::UAI::Contract::FDiagnostic;

	class UEAGENTINTERFACE_API IUeAgentSkinWeightImportCommandService
	{
	public:
		virtual ~IUeAgentSkinWeightImportCommandService() = default;

		virtual bool Execute(
			const FUeAgentSkinWeightImportCommandRequest& Request,
			FUeAgentSkinWeightImportCommandResponse& OutResponse,
			FUeAgentSkinWeightImportCommandError& OutError) = 0;
	};

	UEAGENTINTERFACE_API bool TryCreateProductionSkinWeightImportCommandService(
		TSharedPtr<IUeAgentSkinWeightImportCommandService>& OutService,
		UE::UAI::Contract::FDiagnostic& OutError);
}
