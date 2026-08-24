#pragma once

#define UAI_HJSON_DOMAIN_PROVIDER_RESOLVES_CONTRACT 1

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonImportInitialization.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::HjsonWorkspace
{
	class UEAGENTINTERFACE_API FImportInitializationDomainRequest final
	{
	public:
		FImportInitializationDomainRequest(
			FString InCommandId,
			FString InSchemaId,
			FString InTargetIdentity,
			FString InRootTypeIdentity,
			EInitializationNodeRole InRole,
			FString InCanonicalParamsJson);

		const FString& GetCommandId() const;
		const FString& GetSchemaId() const;
		const FString& GetTargetIdentity() const;
		const FString& GetRootTypeIdentity() const;
		EInitializationNodeRole GetRole() const;
		const FString& GetCanonicalParamsJson() const;
		bool IsValid() const;

	private:
		FString CommandId;
		FString SchemaId;
		FString TargetIdentity;
		FString RootTypeIdentity;
		EInitializationNodeRole Role = EInitializationNodeRole::Value;
		FString CanonicalParamsJson;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FImportInitializationDomainResponse final
	{
	public:
		FImportInitializationDomainResponse() = default;
		FImportInitializationDomainResponse(
			FString InPayloadJson,
			bool bInReadOnly);

		const FString& GetPayloadJson() const;
		bool IsReadOnly() const;
		bool IsValid() const;

	private:
		FString PayloadJson;
		bool bReadOnly = false;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API IImportInitializationDomainProvider
	{
	public:
		virtual ~IImportInitializationDomainProvider() = default;

		virtual const FString& GetProviderId() const = 0;
		virtual bool SupportsExplicitCommand(
			const FString& CommandId) const
		{
			(void)CommandId;
			return false;
		}
		virtual bool Matches(
			const FImportInitializationDomainRequest& Request) const = 0;
		virtual bool TryResolveInitializationContract(
			const FImportInitializationDomainRequest& Request,
			FString& OutInitializationContractId,
			FWorkspaceError& OutError) const = 0;
		virtual bool TryExecute(
			const FImportInitializationDomainRequest& Request,
			const FString& InitializationContractId,
			FImportInitializationDomainResponse& OutResponse,
			FWorkspaceError& OutError) const = 0;
	};

	class UEAGENTINTERFACE_API
		FImportInitializationDomainResolution final
	{
	public:
		FImportInitializationDomainResolution() = default;
		FImportInitializationDomainResolution(
			TSharedPtr<const IImportInitializationDomainProvider> InProvider,
			FString InInitializationContractId);

		const TSharedPtr<const IImportInitializationDomainProvider>&
		GetProvider() const;
		const FString& GetInitializationContractId() const;
		bool IsValid() const;
		void Reset();

	private:
		TSharedPtr<const IImportInitializationDomainProvider> Provider;
		FString InitializationContractId;
	};

	class UEAGENTINTERFACE_API
		FImportInitializationDomainProviderRegistry final
	{
	public:
		static bool TryFreeze(
			TArray<TSharedRef<IImportInitializationDomainProvider>> Providers,
			TSharedPtr<
				const FImportInitializationDomainProviderRegistry>& OutRegistry,
			FWorkspaceError& OutError);

		bool TryResolve(
			const FImportInitializationDomainRequest& Request,
			FImportInitializationDomainResolution& OutResolution,
			FWorkspaceError& OutError) const;

		int32 Num() const;

	private:
		explicit FImportInitializationDomainProviderRegistry(
			TArray<TSharedRef<IImportInitializationDomainProvider>> InProviders);

		TArray<TSharedRef<IImportInitializationDomainProvider>> Providers;
	};

	using FImportInitializationDomainProviderFactory = bool (*)(
		TSharedPtr<IImportInitializationDomainProvider>& OutProvider,
		FWorkspaceError& OutError);

	class UEAGENTINTERFACE_API
		FImportInitializationDomainProviderFactoryRegistration final
	{
	public:
		FImportInitializationDomainProviderFactoryRegistration(
			const TCHAR* ProviderId,
			FImportInitializationDomainProviderFactory Factory);
		FImportInitializationDomainProviderFactoryRegistration(
			const TCHAR* DomainId,
			const TCHAR* ProviderId,
			FImportInitializationDomainProviderFactory Factory);
	};

	UEAGENTINTERFACE_API bool
	TryFreezeProcessImportInitializationDomainProviders(
		TSharedPtr<const FImportInitializationDomainProviderRegistry>&
			OutRegistry,
		FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool
	TryFreezeImportInitializationDomainProviders(
		const TArray<TSharedRef<IImportInitializationDomainProvider>>& Providers,
		TSharedPtr<const FImportInitializationDomainProviderRegistry>&
			OutRegistry,
		FWorkspaceError& OutError);
}
