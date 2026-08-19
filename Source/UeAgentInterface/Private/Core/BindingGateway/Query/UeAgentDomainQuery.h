#pragma once

#include "CoreMinimal.h"
#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"
#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/Projection/UeAgentProjection.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::Contract
{
	class FUeAgentQueryDescriptor;
}

namespace UE::UAI::BindingGateway::Query
{
	struct FUeAgentQueryBindingError;

	using FUeAgentDomainQuerySessionIdentity =
		UE::UAI::BindingGateway::Ticket::
			FUeAgentOperationSessionIdentity;
	using FUeAgentDomainQueryTicket =
		UE::UAI::BindingGateway::Ticket::FUeAgentOperationTicket;

	class UEAGENTINTERFACE_API FUeAgentDomainQueryTarget final
	{
	public:
		FUeAgentDomainQueryTarget(
			FUeAgentDomainQueryTicket InTicket,
			UE::UAI::Discovery::FUeAgentCandidateBinding InBinding);

		const FUeAgentDomainQueryTicket& GetTicket() const;
		const UE::UAI::Discovery::FUeAgentCandidateBinding&
			GetBinding() const;
		bool IsValid() const;

	private:
		FUeAgentDomainQueryTicket Ticket;
		UE::UAI::Discovery::FUeAgentCandidateBinding Binding;
		bool bValid = false;
	};

	/**
	 * Fourth C3Q payload. It owns user input and explicit request context only;
	 * descriptor/provider routing remains server-owned.
	 */
	class UEAGENTINTERFACE_API FUeAgentDomainQuery final
	{
	public:
		FUeAgentDomainQuery(
			FUeAgentDomainQuerySessionIdentity InSession,
			UE::UAI::ContextResource::FUeAgentExecutionContextInput
				InExecutionContext,
			UE::UAI::Contract::FUeAgentContractValue InInput,
			TOptional<FUeAgentDomainQueryTarget> InTarget,
			UE::UAI::Projection::FUeAgentProjectionRequest
				InProjection);

		const FUeAgentDomainQuerySessionIdentity& GetSession() const;
		const UE::UAI::ContextResource::FUeAgentExecutionContextInput&
			GetExecutionContext() const;
		const UE::UAI::Contract::FUeAgentContractValue& GetInput() const;
		const TOptional<FUeAgentDomainQueryTarget>& GetTarget() const;
		const UE::UAI::Projection::FUeAgentProjectionRequest&
			GetProjection() const;
		bool IsValid() const;

	private:
		FUeAgentDomainQuerySessionIdentity Session;
		UE::UAI::ContextResource::FUeAgentExecutionContextInput
			ExecutionContext;
		UE::UAI::Contract::FUeAgentContractValue Input;
		TOptional<FUeAgentDomainQueryTarget> Target;
		UE::UAI::Projection::FUeAgentProjectionRequest Projection;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API IUeAgentDomainQueryProvider
	{
	public:
		virtual ~IUeAgentDomainQueryProvider() = default;

		virtual bool TryQuery(
			const UE::UAI::Contract::FUeAgentQueryDescriptor& Descriptor,
			const UE::UAI::Contract::FUeAgentContractValue&
				CanonicalInput,
			const TOptional<
				UE::UAI::Discovery::FUeAgentCandidateBinding>& Target,
			const UE::UAI::ContextResource::FUeAgentOperationContext&
				Context,
			TSharedPtr<
				const UE::UAI::Projection::
					FUeAgentProjectionSourceValue>& OutSource,
			FUeAgentQueryBindingError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API
		FUeAgentDomainQueryProviderRegistration final
	{
	public:
		FUeAgentDomainQueryProviderRegistration(
			FString InProviderBinding,
			TSharedRef<IUeAgentDomainQueryProvider> InProvider);

		const FString& GetProviderBinding() const;
		const TSharedRef<IUeAgentDomainQueryProvider>& GetProvider() const;
		bool IsValid() const;

	private:
		FString ProviderBinding;
		TSharedRef<IUeAgentDomainQueryProvider> Provider;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API
		FUeAgentDomainQueryProviderRegistry final
	{
	public:
		static bool TryCreate(
			TArray<FUeAgentDomainQueryProviderRegistration>
				Registrations,
			TSharedPtr<const FUeAgentDomainQueryProviderRegistry>&
				OutRegistry,
			FUeAgentQueryBindingError& OutError);

		TSharedPtr<IUeAgentDomainQueryProvider> FindExact(
			const FString& ProviderBinding) const;

	private:
		explicit FUeAgentDomainQueryProviderRegistry(
			TMap<FString, TSharedRef<IUeAgentDomainQueryProvider>>
				InProviders);

		const TMap<FString, TSharedRef<IUeAgentDomainQueryProvider>>
			Providers;
	};
}
