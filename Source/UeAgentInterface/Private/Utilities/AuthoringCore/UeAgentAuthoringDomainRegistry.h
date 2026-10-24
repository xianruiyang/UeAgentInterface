// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

namespace UeAgentAuthoringDomainRegistry
{
	struct UEAGENTINTERFACE_API FCatalogBinding
	{
		FString Kind;
		TArray<FString> CatalogTypeAliases;
		FString DomainId;
		FString WorkspaceContractId;
		FString InputContractId;
		FString UnitId;
		FString PathPattern;
		FString RelativePathTemplate;
		TArray<FString> ExactIdentityFields;
		FString PointerTemplate;
		FString MergeMode;
		TMap<FString, FString> OperationMap;
		TMap<FString, FString> SeedBindings;
		TArray<FString> RequiredContext;
		TMap<FString, FString> ContextBindings;
		TMap<FString, FString> ContextValueAliases;
		TMap<FString, FString> ConstantBindings;
		TArray<FString> RequiredSeedFields;
		TArray<FString> OptionalSeedFields;

		bool operator==(const FCatalogBinding& Other) const;
		bool operator!=(const FCatalogBinding& Other) const { return !(*this == Other); }
	};

	struct UEAGENTINTERFACE_API FDomainRegistration
	{
		FString DomainId;
		TArray<FString> WorkspaceContractIds;
		TArray<FString> InputContractIds;
		TArray<FString> ExportContractIds;
		TArray<FCatalogBinding> CatalogBindings;

		bool operator==(const FDomainRegistration& Other) const;
		bool operator!=(const FDomainRegistration& Other) const { return !(*this == Other); }
	};

	enum class ERegistrationResult : uint8
	{
		Registered,
		AlreadyRegistered,
	};

	class UEAGENTINTERFACE_API FRegistry
	{
	public:
		static FRegistry& Get();

		bool RegisterDomain(
			const FDomainRegistration& Registration,
			ERegistrationResult& OutResult,
			UE::UAI::Contract::FDiagnostic& OutError);
		bool FindDomain(const FString& DomainId, FDomainRegistration& OutRegistration) const;
		bool FindCatalogBinding(const FString& Kind, FCatalogBinding& OutBinding) const;
		TArray<FDomainRegistration> ListDomains() const;
		TArray<FCatalogBinding> ListCatalogBindings() const;

	private:
		mutable FRWLock RegistryLock;
		TMap<FString, FDomainRegistration> Domains;
		TMap<FString, FCatalogBinding> CatalogBindings;
		TMap<FString, FString> CatalogAliases;
	};

	// Generated declarations are consumed only from each domain's RegisterAuthoring root.
	UEAGENTINTERFACE_API bool RegisterGeneratedDomain(const FString& DomainId, UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API const TArray<FString>& GetGeneratedSeedFields();
}
