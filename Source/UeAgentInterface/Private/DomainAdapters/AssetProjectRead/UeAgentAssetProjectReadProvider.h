#pragma once

#include "CoreMinimal.h"
#include "Core/BindingGateway/Query/UeAgentDomainQuery.h"
#include "Core/BindingGateway/Query/UeAgentQueryBindingError.h"
#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::DomainAdapters::AssetProjectRead
{
	struct UEAGENTINTERFACE_API FUeAgentAssetProjectReadError
	{
		FString Code;
		FString SubjectId;
		FString Detail;
	};

	class UEAGENTINTERFACE_API IUeAgentAssetProjectReadHook
	{
	public:
		virtual ~IUeAgentAssetProjectReadHook() = default;

		virtual bool TryCapture(
			const UE::UAI::Contract::FUeAgentContractValue&
				CanonicalInput,
			const TOptional<
				UE::UAI::Discovery::FUeAgentCandidateBinding>& Target,
			const UE::UAI::ContextResource::FUeAgentOperationContext&
				Context,
			TSharedPtr<
				const UE::UAI::StateSemantic::
					FUeAgentCanonicalSnapshot>& OutSnapshot,
			FUeAgentAssetProjectReadError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API
		FUeAgentAssetProjectReadProvider final
		: public UE::UAI::BindingGateway::Query::
			IUeAgentDomainQueryProvider
	{
	public:
		FUeAgentAssetProjectReadProvider(
			FString InProviderBinding,
			TSharedRef<IUeAgentAssetProjectReadHook> InHook);

		const FString& GetProviderBinding() const;

		bool TryQuery(
			const UE::UAI::Contract::FUeAgentQueryDescriptor&
				Descriptor,
			const UE::UAI::Contract::FUeAgentContractValue&
				CanonicalInput,
			const TOptional<
				UE::UAI::Discovery::FUeAgentCandidateBinding>& Target,
			const UE::UAI::ContextResource::FUeAgentOperationContext&
				Context,
			TSharedPtr<
				const UE::UAI::Projection::
					FUeAgentProjectionSourceValue>& OutSource,
			UE::UAI::BindingGateway::Query::
				FUeAgentQueryBindingError& OutError) override;

	private:
		FString ProviderBinding;
		TSharedRef<IUeAgentAssetProjectReadHook> Hook;
	};
}
