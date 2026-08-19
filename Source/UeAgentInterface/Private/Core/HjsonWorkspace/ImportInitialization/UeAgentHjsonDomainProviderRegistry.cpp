#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"

#include "Core/DomainCapability/UeAgentDomainCapability.h"

namespace UE::UAI::HjsonWorkspace
{
	namespace
	{
		bool IsExactIdentity(const FString& Value)
		{
			FString Normalized = Value;
			Normalized.TrimStartAndEndInline();
			return !Normalized.IsEmpty() && Normalized == Value;
		}

	}

	FImportInitializationDomainRequest::
		FImportInitializationDomainRequest(
			FString InCommandId,
			FString InSchemaId,
			FString InTargetIdentity,
			FString InRootTypeIdentity,
			const EInitializationNodeRole InRole,
			FString InCanonicalParamsJson)
		: CommandId(MoveTemp(InCommandId))
		, SchemaId(MoveTemp(InSchemaId))
		, TargetIdentity(MoveTemp(InTargetIdentity))
		, RootTypeIdentity(MoveTemp(InRootTypeIdentity))
		, Role(InRole)
		, CanonicalParamsJson(MoveTemp(InCanonicalParamsJson))
	{
		bValid = IsExactIdentity(CommandId)
			&& IsExactIdentity(SchemaId)
			&& IsExactIdentity(TargetIdentity)
			&& IsExactIdentity(RootTypeIdentity)
			&& !CanonicalParamsJson.IsEmpty();
	}

	const FString& FImportInitializationDomainRequest::GetCommandId() const
	{
		return CommandId;
	}

	const FString& FImportInitializationDomainRequest::GetSchemaId() const
	{
		return SchemaId;
	}

	const FString& FImportInitializationDomainRequest::GetTargetIdentity() const
	{
		return TargetIdentity;
	}

	const FString& FImportInitializationDomainRequest::GetRootTypeIdentity() const
	{
		return RootTypeIdentity;
	}

	EInitializationNodeRole FImportInitializationDomainRequest::GetRole() const
	{
		return Role;
	}

	const FString& FImportInitializationDomainRequest::GetCanonicalParamsJson()
		const
	{
		return CanonicalParamsJson;
	}

	bool FImportInitializationDomainRequest::IsValid() const
	{
		return bValid;
	}

	FImportInitializationDomainResponse::
		FImportInitializationDomainResponse(
			FString InPayloadJson,
			const bool bInReadOnly)
		: PayloadJson(MoveTemp(InPayloadJson))
		, bReadOnly(bInReadOnly)
	{
		bValid = !PayloadJson.IsEmpty();
	}

	const FString& FImportInitializationDomainResponse::GetPayloadJson() const
	{
		return PayloadJson;
	}

	bool FImportInitializationDomainResponse::IsReadOnly() const
	{
		return bReadOnly;
	}

	bool FImportInitializationDomainResponse::IsValid() const
	{
		return bValid;
	}

	FImportInitializationDomainResolution::
		FImportInitializationDomainResolution(
			TSharedPtr<const IImportInitializationDomainProvider> InProvider,
			FString InInitializationContractId)
		: Provider(MoveTemp(InProvider))
		, InitializationContractId(MoveTemp(InInitializationContractId))
	{
	}

	const TSharedPtr<const IImportInitializationDomainProvider>&
	FImportInitializationDomainResolution::GetProvider() const
	{
		return Provider;
	}

	const FString& FImportInitializationDomainResolution::
		GetInitializationContractId() const
	{
		return InitializationContractId;
	}

	bool FImportInitializationDomainResolution::IsValid() const
	{
		return Provider.IsValid()
			&& IsExactIdentity(InitializationContractId);
	}

	void FImportInitializationDomainResolution::Reset()
	{
		Provider.Reset();
		InitializationContractId.Reset();
	}

	FImportInitializationDomainProviderRegistry::
		FImportInitializationDomainProviderRegistry(
			TArray<TSharedRef<IImportInitializationDomainProvider>>
				InProviders)
		: Providers(MoveTemp(InProviders))
	{
	}

	bool FImportInitializationDomainProviderRegistry::TryFreeze(
		TArray<TSharedRef<IImportInitializationDomainProvider>> InProviders,
		TSharedPtr<const FImportInitializationDomainProviderRegistry>&
			OutRegistry,
		FWorkspaceError& OutError)
	{
		OutRegistry.Reset();
		OutError.Reset();
		if (InProviders.IsEmpty())
		{
			OutError.Set(TEXT("hjson_domain_provider_empty"));
			return false;
		}
		TSet<FString> ProviderIds;
		for (const TSharedRef<IImportInitializationDomainProvider>& Provider :
			InProviders)
		{
			const FString& ProviderId = Provider->GetProviderId();
			if (!IsExactIdentity(ProviderId))
			{
				OutError.Set(
					TEXT("hjson_domain_provider_identity_invalid"),
					ProviderId);
				return false;
			}
			if (ProviderIds.Contains(ProviderId))
			{
				OutError.Set(
					TEXT("hjson_domain_provider_duplicate"),
					ProviderId);
				return false;
			}
			ProviderIds.Add(ProviderId);
		}
		InProviders.Sort(
			[](const TSharedRef<IImportInitializationDomainProvider>& Left,
			   const TSharedRef<IImportInitializationDomainProvider>& Right)
			{
				return Left->GetProviderId() < Right->GetProviderId();
			});
		OutRegistry = TSharedPtr<FImportInitializationDomainProviderRegistry>(
			new FImportInitializationDomainProviderRegistry(
				MoveTemp(InProviders)));
		return true;
	}

	bool FImportInitializationDomainProviderRegistry::TryResolve(
		const FImportInitializationDomainRequest& Request,
		FImportInitializationDomainResolution& OutResolution,
		FWorkspaceError& OutError) const
	{
		OutResolution.Reset();
		OutError.Reset();
		if (!Request.IsValid())
		{
			OutError.Set(TEXT("hjson_domain_request_invalid"));
			return false;
		}
		TArray<FImportInitializationDomainResolution> Matches;
		for (const TSharedRef<IImportInitializationDomainProvider>& Provider :
			Providers)
		{
			if (!Provider->Matches(Request))
			{
				continue;
			}
			FString InitializationContractId;
			FWorkspaceError ProviderError;
			if (!Provider->TryResolveInitializationContract(
					Request,
					InitializationContractId,
					ProviderError))
			{
				if (!ProviderError.GetCode().IsEmpty())
				{
					OutError = ProviderError;
					return false;
				}
				continue;
			}
			if (!IsExactIdentity(InitializationContractId))
			{
				OutError.Set(
					TEXT("hjson_domain_contract_identity_invalid"),
					Provider->GetProviderId());
				return false;
			}
			Matches.Emplace(
				TSharedPtr<const IImportInitializationDomainProvider>(Provider),
				MoveTemp(InitializationContractId));
		}
		if (Matches.IsEmpty())
		{
			OutError.Set(
				TEXT("hjson_domain_provider_unresolved"),
				Request.GetRootTypeIdentity());
			return false;
		}
		if (Matches.Num() != 1)
		{
			OutError.Set(
				TEXT("hjson_domain_provider_ambiguous"),
				Request.GetRootTypeIdentity());
			return false;
		}
		OutResolution = MoveTemp(Matches[0]);
		return true;
	}

	int32 FImportInitializationDomainProviderRegistry::Num() const
	{
		return Providers.Num();
	}

	FImportInitializationDomainProviderFactoryRegistration::
		FImportInitializationDomainProviderFactoryRegistration(
			const TCHAR* ProviderId,
			const FImportInitializationDomainProviderFactory Factory)
		: FImportInitializationDomainProviderFactoryRegistration(
			ProviderId,
			ProviderId,
			Factory)
	{
	}

	FImportInitializationDomainProviderFactoryRegistration::
		FImportInitializationDomainProviderFactoryRegistration(
			const TCHAR* DomainId,
			const TCHAR* ProviderId,
			const FImportInitializationDomainProviderFactory Factory)
	{
		DomainCapability::RegisterProcessDomainHjsonCapabilityFactory(
			DomainId ? FString(DomainId) : FString(),
			ProviderId ? FString(ProviderId) : FString(),
			Factory);
	}

	bool TryFreezeImportInitializationDomainProviders(
		const TArray<TSharedRef<IImportInitializationDomainProvider>>&
			ExactProviders,
		TSharedPtr<const FImportInitializationDomainProviderRegistry>&
			OutRegistry,
		FWorkspaceError& OutError)
	{
		OutRegistry.Reset();
		OutError.Reset();
		TArray<TSharedRef<IImportInitializationDomainProvider>> Providers =
			ExactProviders;
		TSharedPtr<const FImportInitializationDomainProviderRegistry>
			BuiltRegistry;
		if (!FImportInitializationDomainProviderRegistry::TryFreeze(
			MoveTemp(Providers),
			BuiltRegistry,
			OutError))
		{
			return false;
		}
		OutRegistry = MoveTemp(BuiltRegistry);
		return true;
	}

	bool TryFreezeProcessImportInitializationDomainProviders(
		TSharedPtr<const FImportInitializationDomainProviderRegistry>&
			OutRegistry,
		FWorkspaceError& OutError)
	{
		OutRegistry.Reset();
		OutError.Reset();
		FString CapabilityError;
		DomainCapability::FDomainCapabilityAuthorities Authorities;
		if (!DomainCapability::TryGetOrBuildProcessDomainCapabilityAuthorities(
				Authorities,
				CapabilityError)
			|| !Authorities.GetHjsonRegistry().IsValid())
		{
			OutError.Set(
				TEXT("hjson_domain_provider_authority_unavailable"),
				CapabilityError);
			return false;
		}
		OutRegistry = Authorities.GetHjsonRegistry();
		return true;
	}
}
