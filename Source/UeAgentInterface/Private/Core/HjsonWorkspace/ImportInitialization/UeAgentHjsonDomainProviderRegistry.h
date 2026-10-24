#pragma once

#define UAI_HJSON_DOMAIN_PROVIDER_RESOLVES_CONTRACT 1

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonImportInitialization.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Templates/SharedPointer.h"

class SClassViewer;
class FObjectPropertyBase;
class UObject;
class UClass;

namespace UE::UAI::HjsonWorkspace
{
	class FAuthoringModel;
	class UEAGENTINTERFACE_API FImportInitializationDomainRequest final
	{
	public:
		FImportInitializationDomainRequest(
			FString InCommandId,
			FString InSchemaId,
			FString InTargetIdentity,
			FString InRootTypeIdentity,
			EInitializationNodeRole InRole,
			FString InCanonicalParamsJson,
			FString InRequestId = FString(),
			TOptional<FSourceProvenance> InSchemaSource = {},
			TOptional<FSourceProvenance> InRootTypeSource = {});

		const FString& GetCommandId() const;
		const FString& GetSchemaId() const;
		const FString& GetTargetIdentity() const;
		const FString& GetRootTypeIdentity() const;
		EInitializationNodeRole GetRole() const;
		const FString& GetCanonicalParamsJson() const;
		const FString& GetRequestId() const;
		bool IsValid() const;
		const Contract::FDiagnostic& GetValidationDiagnostic() const;
		const TOptional<FSourceProvenance>& GetSchemaSource() const;
		const TOptional<FSourceProvenance>& GetRootTypeSource() const;

	private:
		FString CommandId;
		FString SchemaId;
		FString TargetIdentity;
		FString RootTypeIdentity;
		EInitializationNodeRole Role = EInitializationNodeRole::Value;
		FString CanonicalParamsJson;
		FString RequestId;
		TOptional<FSourceProvenance> SchemaSource;
		TOptional<FSourceProvenance> RootTypeSource;
		TSharedPtr<const Contract::FDiagnostic> ValidationDiagnostic;
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
		const Contract::FDiagnostic& GetValidationDiagnostic() const;

	private:
		FString PayloadJson;
		bool bReadOnly = false;
		bool bValid = false;
	};

	/**
	 * Domain providers accept their inline domain schema and the shared
	 * semantic-workspace schema emitted by content_graph.export_workspace.
	 * Non-import commands do not carry an authoring document schema.
	 */
	UEAGENTINTERFACE_API bool MatchesDomainImportSchema(
		const FImportInitializationDomainRequest& Request,
		const FString& DomainSchemaId);

	class UEAGENTINTERFACE_API IImportInitializationDomainProvider
	{
	public:
		virtual ~IImportInitializationDomainProvider() = default;

		virtual const FString& GetProviderId() const = 0;
		/** A generic reflected owner is used only when no dedicated owner matches. */
		virtual bool IsFallbackProvider() const { return false; }
		/** Explicit non-UObject workspace contracts route by their hidden target type. */
		virtual bool SupportsWorkspaceModelContract(const FString& ContractId) const { return false; }
		virtual bool SupportsWorkspacePreparation() const { return false; }
		/** Declares a verified native owned initialization context; never supplies candidate classes. */
		virtual bool SupportsNativeOwnedCreationContext(const FString& OwnerType, const FString& PropertyPath) const { return false; }
		/** Some native editors create owned records outside a Details property handle. */
		virtual bool UsesNativeOwnedFactory(FObjectPropertyBase* Property) const { return false; }
		virtual TSharedPtr<SClassViewer> MakeNativeOwnedClassPicker(UObject* Owner, FObjectPropertyBase* Property) const { return nullptr; }
		/** Native editor wrappers expose scoped records through the shared Details picker. */
		virtual UObject* CreateNativeOwnedDetailsContext(UObject* Owner, FObjectPropertyBase* Property, FString& PropertyPath) const { return nullptr; }
		virtual UObject* CreateNativeOwnedObject(UObject* Owner, UClass* Class) const { return nullptr; }
		/** Finish native work before a prospective owner is retired by the shared scratch scope. */
		virtual void FinishNativeOwnedCreation(UObject* Owner) const {}
		virtual bool SupportsExplicitCommand(
			const FString& CommandId) const
		{
			(void)CommandId;
			return false;
		}
		virtual bool Matches(
			const FImportInitializationDomainRequest& Request) const = 0;
		/** False with no diagnostic declines this request; a non-empty diagnostic is a failure. */
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
		bool TryResolveWorkspaceRootType(const FAuthoringModel& Model,
			const FString& TargetIdentity, FString& OutRootType, FWorkspaceError& OutError) const;
		bool ValidateNativeOwnedCreationContext(const FString& OwnerType, const FString& PropertyPath, FWorkspaceError& OutError) const;
		bool ResolveNativeOwnedCreationProvider(const FString& OwnerType, const FString& PropertyPath,
			TSharedPtr<const IImportInitializationDomainProvider>& OutProvider, FWorkspaceError& OutError) const;

	private:
		explicit FImportInitializationDomainProviderRegistry(
			TArray<TSharedRef<IImportInitializationDomainProvider>> InProviders);

		TArray<TSharedRef<IImportInitializationDomainProvider>> Providers;
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
