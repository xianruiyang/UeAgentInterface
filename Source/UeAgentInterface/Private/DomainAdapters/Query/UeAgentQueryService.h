#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Core/BindingGateway/Query/UeAgentDomainQuery.h"
#include "Core/BindingGateway/Query/UeAgentQueryPublicProjection.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::DomainAdapters::Query
{
	struct UEAGENTINTERFACE_API FQueryRoute final
	{
		FString CommandId;
		FString MechanicsCommandId;
		FString PythonFunction;
		bool bRequiresTarget = false;
		bool bUsesObjectPath = false;
		FString RequiredProperty;
		FString PublicProjectionId = TEXT("legacy_typed_envelope");

		bool IsValid() const;
		bool TryValidate(UE::UAI::Contract::FDiagnostic& OutDiagnostic) const;
	};

	struct UEAGENTINTERFACE_API FQueryServiceRequest final
	{
		FString CommandId;
		FString RequestId;
		FString CanonicalParamsJson = TEXT("{}");
		FString CallerId;
		TArray<FString> AuthorityScopes;
		FString CorrelationId;
		bool bCancellationRequested = false;
	};

	struct UEAGENTINTERFACE_API FQueryServiceResponse final
	{
		FString PayloadJson = TEXT("{}");
		TArray<UE::UAI::Contract::FDiagnostic> Diagnostics;
		bool bReadOnly = true;
	};

	using FQueryServiceError = UE::UAI::Contract::FDiagnostic;

	class UEAGENTINTERFACE_API IUeAgentQueryService
	{
	public:
		virtual ~IUeAgentQueryService() = default;

		virtual bool Execute(
			const FQueryServiceRequest& Request,
			FQueryServiceResponse& OutResponse,
			FQueryServiceError& OutError) const = 0;
	};

	/**
	 * One production composition owns both Query execution and the exact
	 * immutable descriptors that define its command-specific input contracts.
	 * Command registration must consume this same descriptor instance instead
	 * of rebuilding a parallel registry.
	 */
	struct UEAGENTINTERFACE_API FProductionQueryComposition final
	{
		TSharedPtr<IUeAgentQueryService> Service;
		TSharedPtr<const Contract::FUeAgentDescriptorRegistry>
			DescriptorRegistry;

		bool IsValid() const;
	};

	UEAGENTINTERFACE_API const TArray<FQueryRoute>&
	GetNonHjsonQueryRoutes();

	UEAGENTINTERFACE_API bool TryCreateProductionQueryService(
		FProductionQueryComposition& OutComposition,
		UE::UAI::Contract::FDiagnostic& OutError);

	/** Provider contribution owned by the query domain capability. */
	UEAGENTINTERFACE_API bool
	TryCreateProductionQueryProviderRegistrations(
		TArray<
			UE::UAI::BindingGateway::Query::
				FUeAgentDomainQueryProviderRegistration>&
					OutRegistrations,
		UE::UAI::Contract::FDiagnostic& OutError);
}
