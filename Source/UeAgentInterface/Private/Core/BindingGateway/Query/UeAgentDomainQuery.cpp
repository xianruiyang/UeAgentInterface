#include "Core/BindingGateway/Query/UeAgentDomainQuery.h"

#include "Core/BindingGateway/Query/UeAgentQueryBinding.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::BindingGateway::Query
{
	FUeAgentDomainQueryTarget::FUeAgentDomainQueryTarget(
		FUeAgentDomainQueryTicket InTicket,
		UE::UAI::Discovery::FUeAgentCandidateBinding InBinding)
		: Ticket(MoveTemp(InTicket))
		, Binding(MoveTemp(InBinding))
	{
		if (!Ticket.IsValid()) ValidationDiagnostic = Ticket.GetValidationDiagnostic();
		else if (!Binding.IsValid()) ValidationDiagnostic = *Binding.GetValidationDiagnostic();
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
		return ValidationDiagnostic.IsEmpty();
	}

	const FUeAgentQueryBindingError& FUeAgentDomainQueryTarget::GetValidationDiagnostic() const { return ValidationDiagnostic; }

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
		if (!Session.IsValid()) ValidationDiagnostic = Session.GetValidationDiagnostic();
		else if (!Input.IsValid())
		{
			ValidationDiagnostic = FUeAgentQueryBindingError::Failure(TEXT("query_input_invalid"),
				TEXT("The domain query input JSON is empty."), TEXT("query_binding"));
			ValidationDiagnostic.PropertyPath = TEXT("input");
		}
		else if (!Projection.IsValid()) ValidationDiagnostic = Projection.GetValidationDiagnostic();
		else if (Target.IsSet() && !Target->IsValid()) ValidationDiagnostic = Target->GetValidationDiagnostic();
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
		return ValidationDiagnostic.IsEmpty();
	}

	const FUeAgentQueryBindingError& FUeAgentDomainQuery::GetValidationDiagnostic() const { return ValidationDiagnostic; }

	FUeAgentDomainQueryProviderRegistration::
		FUeAgentDomainQueryProviderRegistration(
			FString InProviderBinding,
			TSharedRef<IUeAgentDomainQueryProvider> InProvider)
		: ProviderBinding(MoveTemp(InProviderBinding))
		, Provider(MoveTemp(InProvider))
	{
		ProviderBinding.TrimStartAndEndInline();
		if (ProviderBinding.IsEmpty())
		{
			ValidationDiagnostic = FUeAgentQueryBindingError::Failure(TEXT("query_provider_registration_invalid"), TEXT("The provider binding must not be empty."), TEXT("query_binding"));
			ValidationDiagnostic.PropertyPath = TEXT("provider_binding");
		}
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
		return ValidationDiagnostic.IsEmpty();
	}
	const FUeAgentQueryBindingError& FUeAgentDomainQueryProviderRegistration::GetValidationDiagnostic() const { return ValidationDiagnostic; }

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
				OutError = Registration.GetValidationDiagnostic();
				return false;
			}
			if (Providers.Contains(
					Registration.GetProviderBinding()))
			{
				OutError = FUeAgentQueryBindingError::Failure(TEXT("query_provider_binding_duplicate"), TEXT("The same provider binding is registered more than once."), TEXT("query_binding"));
				OutError.PropertyPath = TEXT("provider_binding");
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Registration.GetProviderBinding());
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
