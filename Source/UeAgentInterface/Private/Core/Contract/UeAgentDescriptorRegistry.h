#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Contract/UeAgentContractSchema.h"
#include "Core/Contract/UeAgentExecutionContextPolicy.h"
#include "Templates/SharedPointer.h"

#define UAI_OPERATION_COMPILED_SCHEMA_ACCESS_V1 1
#define UAI_OPERATION_COMPILED_RESULT_SCHEMA_ACCESS_V1 1

namespace UE::UAI::Contract
{
	enum class EUeAgentDescriptorOperationKind : uint8
	{
		Invalid,
		Query,
		Operation,
	};

	enum class EUeAgentQueryKind : uint8
	{
		Invalid,
		Capability,
		Catalog,
		State,
		Reference,
		RuntimeProbe,
	};

	enum class EUeAgentQueryMaterializationPolicy : uint8
	{
		Invalid,
		IndexOnly,
		LoadReadonly,
		RuntimeObserve,
	};

	enum class EUeAgentQuerySideEffectPolicy : uint8
	{
		Invalid,
		NoPersistentMutation,
	};

	enum class EUeAgentDescriptorCompatibility : uint8
	{
		Exact,
		VersionIncompatible,
		HashIncompatible,
		NotFound,
	};

	class UEAGENTINTERFACE_API FUeAgentQueryBinding final
	{
	public:
		FUeAgentQueryBinding(
			FString InProviderBinding,
			EUeAgentQueryKind InQueryKind,
			EUeAgentQueryMaterializationPolicy
				InMaterializationPolicy,
			EUeAgentQuerySideEffectPolicy
				InSideEffectPolicy);

		const FString& GetProviderBinding() const;
		EUeAgentQueryKind GetQueryKind() const;
		EUeAgentQueryMaterializationPolicy
			GetMaterializationPolicy() const;
		EUeAgentQuerySideEffectPolicy
			GetSideEffectPolicy() const;
		bool IsValid() const;

	private:
		FString ProviderBinding;
		EUeAgentQueryKind QueryKind =
			EUeAgentQueryKind::Invalid;
		EUeAgentQueryMaterializationPolicy MaterializationPolicy =
			EUeAgentQueryMaterializationPolicy::Invalid;
		EUeAgentQuerySideEffectPolicy SideEffectPolicy =
			EUeAgentQuerySideEffectPolicy::Invalid;
	};

	class UEAGENTINTERFACE_API FUeAgentOperationDescriptor final
	{
	public:
		FUeAgentOperationDescriptor(
			FUeAgentDescriptorIdentity InIdentity,
			FString InCapabilityId,
			FString InDomainId,
			EUeAgentDescriptorOperationKind InOperationKind,
			FUeAgentExecutionContextPolicy
				InExecutionContextPolicy,
			FUeAgentContractValue InInputSchema,
			FUeAgentContractValue InResultSchema,
			TOptional<FUeAgentQueryBinding> InQueryBinding = {});

		const FUeAgentDescriptorIdentity& GetIdentity() const;
		const FString& GetCapabilityId() const;
		const FString& GetDomainId() const;
		EUeAgentDescriptorOperationKind GetOperationKind() const;
		const FUeAgentExecutionContextPolicy&
			GetExecutionContextPolicy() const;
		const FUeAgentContractValue& GetInputSchema() const;
		const FUeAgentContractValue& GetResultSchema() const;
		const TOptional<FUeAgentQueryBinding>&
			GetQueryBinding() const;

	private:
		friend class FUeAgentDescriptorRegistry;

		FUeAgentDescriptorIdentity Identity;
		FString CapabilityId;
		FString DomainId;
		EUeAgentDescriptorOperationKind OperationKind =
			EUeAgentDescriptorOperationKind::Invalid;
		FUeAgentExecutionContextPolicy ExecutionContextPolicy;
		FUeAgentContractValue InputSchema;
		FUeAgentContractValue ResultSchema;
		TOptional<FUeAgentQueryBinding> QueryBinding;
	};

	class FUeAgentDescriptorRegistry;

	class UEAGENTINTERFACE_API FUeAgentQueryDescriptor final
	{
	public:
		const FUeAgentOperationDescriptor&
			GetOperationDescriptor() const;
		const FUeAgentDescriptorIdentity& GetIdentity() const;
		const FString& GetCapabilityId() const;
		const FString& GetDomainId() const;
		const FUeAgentExecutionContextPolicy&
			GetExecutionContextPolicy() const;
		const FUeAgentContractValue& GetInputSchema() const;
		const FUeAgentContractSchema& GetCompiledInputSchema() const;
		const FUeAgentContractValue& GetResultSchema() const;
		const FString& GetProviderBinding() const;
		EUeAgentQueryKind GetQueryKind() const;
		EUeAgentQueryMaterializationPolicy
			GetMaterializationPolicy() const;
		EUeAgentQuerySideEffectPolicy
			GetSideEffectPolicy() const;

	private:
		friend class FUeAgentDescriptorRegistry;

		explicit FUeAgentQueryDescriptor(
			TSharedRef<const FUeAgentDescriptorRegistry>
				InRegistryOwner,
			const FUeAgentOperationDescriptor& InDescriptor,
			TSharedRef<const FUeAgentContractSchema>
				InCompiledInputSchema);

		TSharedPtr<const FUeAgentDescriptorRegistry> RegistryOwner;
		const FUeAgentOperationDescriptor* Descriptor = nullptr;
		TSharedPtr<const FUeAgentContractSchema> CompiledInputSchema;
	};

	class UEAGENTINTERFACE_API FUeAgentDescriptorRegistry final
		: public TSharedFromThis<FUeAgentDescriptorRegistry>
	{
	public:
		static bool TryCreate(
			TArray<FUeAgentOperationDescriptor> Descriptors,
			TSharedPtr<const FUeAgentDescriptorRegistry>&
				OutRegistry,
			FString& OutError);

		const TArray<FUeAgentOperationDescriptor>&
			GetDescriptors() const;
		const FString& GetRegistryHash() const;

		const FUeAgentOperationDescriptor* FindByDescriptorId(
			const FString& DescriptorId) const;
		const FUeAgentOperationDescriptor* FindExact(
			const FUeAgentDescriptorIdentity& Identity) const;
		const FUeAgentContractSchema* FindCompiledInputSchemaExact(
			const FUeAgentDescriptorIdentity& Identity) const;
		const FUeAgentContractSchema* FindCompiledResultSchemaExact(
			const FUeAgentDescriptorIdentity& Identity) const;
		TOptional<FUeAgentQueryDescriptor> FindQueryExact(
			const FUeAgentDescriptorIdentity& Identity) const;
		EUeAgentDescriptorCompatibility CheckCompatibility(
			const FUeAgentDescriptorIdentity& Identity) const;

	private:
		FUeAgentDescriptorRegistry(
			TArray<FUeAgentOperationDescriptor> InDescriptors,
			TArray<TSharedRef<const FUeAgentContractSchema>>
				InCompiledInputSchemas,
			TArray<TSharedRef<const FUeAgentContractSchema>>
				InCompiledResultSchemas,
			FString InRegistryHash);

		TArray<FUeAgentOperationDescriptor> Descriptors;
		TArray<TSharedRef<const FUeAgentContractSchema>>
			CompiledInputSchemas;
		TArray<TSharedRef<const FUeAgentContractSchema>>
			CompiledResultSchemas;
		TMap<FString, int32> IndexByDescriptorId;
		FString RegistryHash;
	};
}
