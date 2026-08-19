#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"

#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"

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

		struct FFactoryEntry final
		{
			FString ProviderId;
			FImportInitializationDomainProviderFactory Factory = nullptr;
		};

		struct FProcessCatalog final
		{
			FCriticalSection Mutex;
			bool bSealed = false;
			TArray<FFactoryEntry> Entries;
			TArray<FString> RegistrationErrors;
			TSharedPtr<const FImportInitializationDomainProviderRegistry>
				InstalledAuthority;
		};

		FProcessCatalog& ProcessCatalog()
		{
			static FProcessCatalog Catalog;
			return Catalog;
		}

		bool RegisterFactory(
			const FString& ProviderId,
			const FImportInitializationDomainProviderFactory Factory)
		{
			FProcessCatalog& Catalog = ProcessCatalog();
			FScopeLock Lock(&Catalog.Mutex);
			if (Catalog.bSealed)
			{
				Catalog.RegistrationErrors.AddUnique(
					TEXT("hjson_domain_provider_catalog_sealed:") + ProviderId);
				return false;
			}
			if (!IsExactIdentity(ProviderId) || !Factory)
			{
				Catalog.RegistrationErrors.AddUnique(
					TEXT("hjson_domain_provider_factory_invalid:") + ProviderId);
				return false;
			}
			if (Catalog.Entries.ContainsByPredicate(
				[&ProviderId](const FFactoryEntry& Entry)
				{
					return Entry.ProviderId == ProviderId;
				}))
			{
				Catalog.RegistrationErrors.AddUnique(
					TEXT("hjson_domain_provider_duplicate:") + ProviderId);
				return false;
			}
			Catalog.Entries.Add({ProviderId, Factory});
			return true;
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
	{
		RegisterFactory(
			ProviderId ? FString(ProviderId) : FString(),
			Factory);
	}

	bool TryFreezeProcessImportInitializationDomainProviders(
		const TArray<TSharedRef<IImportInitializationDomainProvider>>&
			DomainOverrides,
		const bool bInstallAsProcessAuthority,
		TSharedPtr<const FImportInitializationDomainProviderRegistry>&
			OutRegistry,
		FWorkspaceError& OutError)
	{
		OutRegistry.Reset();
		OutError.Reset();
		TArray<FFactoryEntry> Entries;
		{
			FProcessCatalog& Catalog = ProcessCatalog();
			FScopeLock Lock(&Catalog.Mutex);
			if (Catalog.InstalledAuthority.IsValid())
			{
				OutRegistry = Catalog.InstalledAuthority;
				return true;
			}
			Catalog.bSealed = true;
			if (!Catalog.RegistrationErrors.IsEmpty())
			{
				OutError.Set(
					TEXT("hjson_domain_provider_registration_failed"),
					Catalog.RegistrationErrors[0]);
				return false;
			}
			Entries = Catalog.Entries;
		}
		Entries.Sort(
			[](const FFactoryEntry& Left, const FFactoryEntry& Right)
			{
				return Left.ProviderId < Right.ProviderId;
			});
		TMap<FString, TSharedRef<IImportInitializationDomainProvider>>
			OverridesById;
		for (const TSharedRef<IImportInitializationDomainProvider>& Override :
			DomainOverrides)
		{
			const FString& OverrideId = Override->GetProviderId();
			if (!IsExactIdentity(OverrideId))
			{
				OutError.Set(
					TEXT("hjson_domain_provider_override_identity_invalid"),
					OverrideId);
				return false;
			}
			if (OverridesById.Contains(OverrideId))
			{
				OutError.Set(
					TEXT("hjson_domain_provider_override_duplicate"),
					OverrideId);
				return false;
			}
			OverridesById.Add(OverrideId, Override);
		}
		TArray<TSharedRef<IImportInitializationDomainProvider>> Providers;
		for (const FFactoryEntry& Entry : Entries)
		{
			if (const TSharedRef<IImportInitializationDomainProvider>* Override =
				OverridesById.Find(Entry.ProviderId))
			{
				Providers.Add(*Override);
				OverridesById.Remove(Entry.ProviderId);
				continue;
			}
			TSharedPtr<IImportInitializationDomainProvider> Provider;
			FWorkspaceError FactoryError;
			if (!Entry.Factory(Provider, FactoryError)
				|| !Provider.IsValid()
				|| Provider->GetProviderId() != Entry.ProviderId)
			{
				OutError.Set(
					TEXT("hjson_domain_provider_factory_failed"),
					Entry.ProviderId + TEXT(":") + FactoryError.GetCode());
				return false;
			}
			Providers.Add(Provider.ToSharedRef());
		}
		TArray<FString> RemainingOverrideIds;
		OverridesById.GetKeys(RemainingOverrideIds);
		RemainingOverrideIds.Sort();
		for (const FString& OverrideId : RemainingOverrideIds)
		{
			Providers.Add(OverridesById.FindChecked(OverrideId));
		}
		TSharedPtr<const FImportInitializationDomainProviderRegistry>
			BuiltRegistry;
		if (!FImportInitializationDomainProviderRegistry::TryFreeze(
			MoveTemp(Providers), BuiltRegistry, OutError))
		{
			return false;
		}
		if (bInstallAsProcessAuthority)
		{
			FProcessCatalog& Catalog = ProcessCatalog();
			FScopeLock Lock(&Catalog.Mutex);
			if (!Catalog.InstalledAuthority.IsValid())
			{
				Catalog.InstalledAuthority = BuiltRegistry;
			}
			BuiltRegistry = Catalog.InstalledAuthority;
		}
		OutRegistry = MoveTemp(BuiltRegistry);
		return true;
	}

	bool TryFreezeProcessImportInitializationDomainProviders(
		TSharedPtr<const FImportInitializationDomainProviderRegistry>&
			OutRegistry,
		FWorkspaceError& OutError)
	{
		return TryFreezeProcessImportInitializationDomainProviders(
			{},
			false,
			OutRegistry,
			OutError);
	}
}
