#pragma once

#include "CoreMinimal.h"
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

		bool IsValid() const;
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
		bool bReadOnly = true;
	};

	struct UEAGENTINTERFACE_API FQueryServiceError final
	{
		FString Code;
		FString SubjectId;
		FString Detail;
	};

	class UEAGENTINTERFACE_API IUeAgentQueryService
	{
	public:
		virtual ~IUeAgentQueryService() = default;

		virtual bool Execute(
			const FQueryServiceRequest& Request,
			FQueryServiceResponse& OutResponse,
			FQueryServiceError& OutError) const = 0;
	};

	UEAGENTINTERFACE_API const TArray<FQueryRoute>&
	GetNonHjsonQueryRoutes();

	UEAGENTINTERFACE_API bool TryCreateProductionQueryService(
		TSharedPtr<IUeAgentQueryService>& OutService,
		FString& OutError);
}
