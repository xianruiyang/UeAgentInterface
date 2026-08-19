#include "Core/BindingGateway/Query/UeAgentDomainQuery.h"

#include "Core/BindingGateway/Query/UeAgentQueryBinding.h"

namespace UE::UAI::BindingGateway::Query
{
	namespace
	{
		void SetError(
			FUeAgentQueryBindingError& OutError,
			const FString& Code,
			const FString& SubjectId,
			const FString& Detail)
		{
			OutError = {};
			OutError.Code = Code;
			OutError.SubjectId = SubjectId;
			OutError.Detail = Detail;
		}
	}

	FUeAgentDomainQueryTarget::FUeAgentDomainQueryTarget(
		FUeAgentDomainQueryTicket InTicket,
		UE::UAI::Discovery::FUeAgentCandidateBinding InBinding)
		: Ticket(MoveTemp(InTicket))
		, Binding(MoveTemp(InBinding))
	{
		bValid = Ticket.IsValid() && Binding.IsValid();
	}

	const FUeAgentDomainQueryTicket&
	FUeAgentDomainQueryTarget::GetTicket() const
	{
		return Ticket;
	}

	const UE::UAI::Discovery::FUeAgentCandidateBinding&
	FUeAgentDomainQueryTarget::GetBinding() const
	{
		return Binding;
	}

	bool FUeAgentDomainQueryTarget::IsValid() const
	{
		return bValid;
	}

	FUeAgentDomainQuery::FUeAgentDomainQuery(
		FUeAgentDomainQuerySessionIdentity InSession,
		UE::UAI::ContextResource::FUeAgentExecutionContextInput
			InExecutionContext,
		UE::UAI::Contract::FUeAgentContractValue InInput,
		TOptional<FUeAgentDomainQueryTarget> InTarget,
		UE::UAI::Projection::FUeAgentProjectionRequest InProjection)
		: Session(MoveTemp(InSession))
		, ExecutionContext(MoveTemp(InExecutionContext))
		, Input(MoveTemp(InInput))
		, Target(MoveTemp(InTarget))
		, Projection(MoveTemp(InProjection))
	{
		bValid = Session.IsValid()
			&& Input.IsValid()
			&& Projection.IsValid()
			&& (!Target.IsSet() || Target->IsValid());
	}

	const FUeAgentDomainQuerySessionIdentity&
	FUeAgentDomainQuery::GetSession() const
	{
		return Session;
	}

	const UE::UAI::ContextResource::FUeAgentExecutionContextInput&
	FUeAgentDomainQuery::GetExecutionContext() const
	{
		return ExecutionContext;
	}

	const UE::UAI::Contract::FUeAgentContractValue&
	FUeAgentDomainQuery::GetInput() const
	{
		return Input;
	}

	const TOptional<FUeAgentDomainQueryTarget>&
	FUeAgentDomainQuery::GetTarget() const
	{
		return Target;
	}

	const UE::UAI::Projection::FUeAgentProjectionRequest&
	FUeAgentDomainQuery::GetProjection() const
	{
		return Projection;
	}

	bool FUeAgentDomainQuery::IsValid() const
	{
		return bValid;
	}

	FUeAgentDomainQueryProviderRegistration::
		FUeAgentDomainQueryProviderRegistration(
			FString InProviderBinding,
			TSharedRef<IUeAgentDomainQueryProvider> InProvider)
		: ProviderBinding(MoveTemp(InProviderBinding))
		, Provider(MoveTemp(InProvider))
	{
		ProviderBinding.TrimStartAndEndInline();
		bValid = !ProviderBinding.IsEmpty();
	}

	const FString&
	FUeAgentDomainQueryProviderRegistration::GetProviderBinding() const
	{
		return ProviderBinding;
	}

	const TSharedRef<IUeAgentDomainQueryProvider>&
	FUeAgentDomainQueryProviderRegistration::GetProvider() const
	{
		return Provider;
	}

	bool FUeAgentDomainQueryProviderRegistration::IsValid() const
	{
		return bValid;
	}

	FUeAgentDomainQueryProviderRegistry::
		FUeAgentDomainQueryProviderRegistry(
			TMap<FString, TSharedRef<IUeAgentDomainQueryProvider>>
				InProviders)
		: Providers(MoveTemp(InProviders))
	{
	}

	bool FUeAgentDomainQueryProviderRegistry::TryCreate(
		TArray<FUeAgentDomainQueryProviderRegistration> Registrations,
		TSharedPtr<const FUeAgentDomainQueryProviderRegistry>& OutRegistry,
		FUeAgentQueryBindingError& OutError)
	{
		OutRegistry.Reset();
		OutError = {};
		TMap<FString, TSharedRef<IUeAgentDomainQueryProvider>> Providers;
		for (FUeAgentDomainQueryProviderRegistration& Registration :
			Registrations)
		{
			if (!Registration.IsValid())
			{
				SetError(
					OutError,
					TEXT("query_provider_registration_invalid"),
					Registration.GetProviderBinding(),
					TEXT(
						"Provider binding and provider are required."));
				return false;
			}
			if (Providers.Contains(
					Registration.GetProviderBinding()))
			{
				SetError(
					OutError,
					TEXT("query_provider_binding_duplicate"),
					Registration.GetProviderBinding(),
					TEXT(
						"Provider binding is registered more than once."));
				return false;
			}
			Providers.Add(
				Registration.GetProviderBinding(),
				Registration.GetProvider());
		}
		OutRegistry = MakeShareable(
			new FUeAgentDomainQueryProviderRegistry(
				MoveTemp(Providers)));
		return true;
	}

	TSharedPtr<IUeAgentDomainQueryProvider>
	FUeAgentDomainQueryProviderRegistry::FindExact(
		const FString& ProviderBinding) const
	{
		const TSharedRef<IUeAgentDomainQueryProvider>* Provider =
			Providers.Find(ProviderBinding);
		return Provider
			? Provider->ToSharedPtr()
			: TSharedPtr<IUeAgentDomainQueryProvider>();
	}
}
