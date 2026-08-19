// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace UeAgentApplyWorkspaceContract
{
	enum class ESourceKind : uint8
	{
		Directory,
		SingleFile,
		Inline,
	};

	enum class ERegistrationResult : uint8
	{
		Registered,
		AlreadyRegistered,
	};

	struct UEAGENTINTERFACE_API FSourceDescriptor
	{
		ESourceKind Kind = ESourceKind::Directory;
		FString EntryMode;
		FString EntryRole;

		bool operator==(const FSourceDescriptor& Other) const;
		bool operator!=(const FSourceDescriptor& Other) const { return !(*this == Other); }
	};

	struct UEAGENTINTERFACE_API FEntryDescriptor
	{
		FString DirectoryEntry;
		FString VirtualUnitId;

		bool operator==(const FEntryDescriptor& Other) const;
		bool operator!=(const FEntryDescriptor& Other) const { return !(*this == Other); }
	};

	struct UEAGENTINTERFACE_API FUnitDescriptor
	{
		FString UnitId;
		FString PathPattern;
		FString Role;
		FString Cardinality;
		bool bRequired = false;
		FString RouteId;
		FString ApplyContractRef;
		FString MergePolicy;
		FString AbsentSemantics;
		FString EmptySemantics;

		bool operator==(const FUnitDescriptor& Other) const;
		bool operator!=(const FUnitDescriptor& Other) const { return !(*this == Other); }
	};

	struct UEAGENTINTERFACE_API FReferencePolicy
	{
		FString Base;
		FString RootContainment;
		FString ExternalReferences;
		FString CaseCollision;

		bool operator==(const FReferencePolicy& Other) const;
		bool operator!=(const FReferencePolicy& Other) const { return !(*this == Other); }
	};

	struct UEAGENTINTERFACE_API FDependencyPolicy
	{
		FString EdgeSource;
		FString CyclePolicy;
		FString UnresolvedPolicy;
		FString Order;

		bool operator==(const FDependencyPolicy& Other) const;
		bool operator!=(const FDependencyPolicy& Other) const { return !(*this == Other); }
	};

	struct UEAGENTINTERFACE_API FPreflightPolicy
	{
		FString Scope;
		FString MutationBeforeSuccess;
		bool bDiagnosticsIncludeSource = false;

		bool operator==(const FPreflightPolicy& Other) const;
		bool operator!=(const FPreflightPolicy& Other) const { return !(*this == Other); }
	};

	struct UEAGENTINTERFACE_API FTransactionPolicy
	{
		FString Mode;
		TArray<FString> RollbackOn;

		bool operator==(const FTransactionPolicy& Other) const;
		bool operator!=(const FTransactionPolicy& Other) const { return !(*this == Other); }
	};

	struct UEAGENTINTERFACE_API FExecutionPolicy
	{
		FString Compile;
		FString Save;
		FString Verification;

		bool operator==(const FExecutionPolicy& Other) const;
		bool operator!=(const FExecutionPolicy& Other) const { return !(*this == Other); }
	};

	struct UEAGENTINTERFACE_API FContract
	{
		FString Schema;
		FString ContractId;
		FString ContractVersion;
		FString ProfileId;
		TArray<FSourceDescriptor> Sources;
		FEntryDescriptor Entry;
		TArray<FUnitDescriptor> Units;
		FReferencePolicy ReferencePolicy;
		FDependencyPolicy DependencyPolicy;
		FPreflightPolicy Preflight;
		FTransactionPolicy Transaction;
		FExecutionPolicy Execution;
		FString UnknownFilePolicy;
		FString DuplicateTargetPolicy;

		const FSourceDescriptor* FindSource(ESourceKind Kind) const;
		const FUnitDescriptor* FindUnit(const FString& UnitId) const;
		bool IsValid() const;
		bool operator==(const FContract& Other) const;
		bool operator!=(const FContract& Other) const { return !(*this == Other); }
	};

	class UEAGENTINTERFACE_API FRegistry
	{
	public:
		FRegistry() = default;
		FRegistry(const FRegistry&) = delete;
		FRegistry& operator=(const FRegistry&) = delete;

		static FRegistry& Get();

		bool RegisterContract(
			const FContract& Contract,
			ERegistrationResult& OutResult,
			FString& OutError);
		bool FindContract(const FString& ContractId, FContract& OutContract) const;
		TArray<FContract> ListContracts() const;
		int32 GetContractCount() const;

	private:
		mutable FRWLock RegistryLock;
		TMap<FString, FContract> Contracts;
	};

	UEAGENTINTERFACE_API FString SourceKindToString(ESourceKind Kind);
	UEAGENTINTERFACE_API bool ParseSourceKind(const FString& Text, ESourceKind& OutKind);

	UEAGENTINTERFACE_API bool ParseContract(
		const TSharedPtr<FJsonObject>& Object,
		FContract& OutContract,
		FString& OutError);

	UEAGENTINTERFACE_API bool ValidateContract(
		const FContract& Contract,
		FString& OutError);
}
