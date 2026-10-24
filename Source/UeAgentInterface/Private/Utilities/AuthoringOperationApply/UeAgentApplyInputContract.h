// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceRouter.h"

class FJsonObject;
class FJsonValue;

namespace UeAgentApplyInputContract
{
	enum class EOperation : uint8
	{
		Create,
		Update,
		Replace,
		Upsert,
		Remove,
		Clear,
		Clone,
		Insert,
		Move,
		Reorder,
	};

	enum class ERegistrationResult : uint8
	{
		Registered,
		AlreadyRegistered,
	};

	struct UEAGENTINTERFACE_API FInputFieldDescriptor
	{
		FString Name;
		FString Role;
		FString TypeId;
		TArray<EOperation> RequiredFor;
		FString SemanticId;

		bool operator==(const FInputFieldDescriptor& Other) const;
		bool operator!=(const FInputFieldDescriptor& Other) const { return !(*this == Other); }
	};

	struct UEAGENTINTERFACE_API FOperationDescriptor
	{
		EOperation Operation = EOperation::Update;
		TArray<TArray<FString>> IdentityOneOf;
		TArray<FString> AcceptedSections;
		FString OmittedFieldPolicy;
		FString NativeDefaultPolicy;
		bool bDestructive = false;

		bool AcceptsSection(const FString& Section) const;
		bool operator==(const FOperationDescriptor& Other) const;
		bool operator!=(const FOperationDescriptor& Other) const { return !(*this == Other); }
	};

	struct UEAGENTINTERFACE_API FReflectionBaseline
	{
		bool bEnabled = true;
		TArray<FString> RootKinds;
		TArray<FString> RecursiveNodes;
		FString UnknownFieldPolicy;
		FString ExternalReferencePolicy;
		FString OwnedObjectPolicy;

		bool operator==(const FReflectionBaseline& Other) const;
		bool operator!=(const FReflectionBaseline& Other) const { return !(*this == Other); }
	};

	struct UEAGENTINTERFACE_API FPropertyPayload
	{
		FString Mode;
		FString PropertyPathField;
		FString ValueField;
		TArray<FString> OptionalAssertionFields;
		FString OmittedPatchFieldPolicy;

		bool operator==(const FPropertyPayload& Other) const;
		bool operator!=(const FPropertyPayload& Other) const { return !(*this == Other); }
	};

	struct UEAGENTINTERFACE_API FVerificationPolicy
	{
		FString StateSource;
		FString CompareSource;
		bool bRequired = true;

		bool operator==(const FVerificationPolicy& Other) const;
		bool operator!=(const FVerificationPolicy& Other) const { return !(*this == Other); }
	};

	struct UEAGENTINTERFACE_API FContract
	{
		FString Schema;
		FString ContractId;
		FString ContractVersion;
		FString ProfileId;
		FString TargetKind;
		TArray<FString> SemanticDescriptorRefs;
		FReflectionBaseline ReflectionBaseline;
		FPropertyPayload PropertyPayload;
		TArray<FInputFieldDescriptor> InputFields;
		TArray<FOperationDescriptor> Operations;
		FString ProviderId;
		FString UnknownInputFieldPolicy;
		FVerificationPolicy Verification;

		const FOperationDescriptor* FindOperation(EOperation Operation) const;
		const FInputFieldDescriptor* FindInputField(const FString& Name) const;
		bool IsValid() const;
		bool operator==(const FContract& Other) const;
		bool operator!=(const FContract& Other) const { return !(*this == Other); }
	};

	struct UEAGENTINTERFACE_API FOperationInput
	{
		EOperation RequestedOperation = EOperation::Update;
		EOperation ResolvedOperation = EOperation::Update;
		bool bWorkspaceSynthesizedOperation = false;
		TSharedPtr<const FJsonObject> Target;
		TArray<TSharedPtr<FJsonValue>> Properties;
		TSharedPtr<const FJsonValue> Children;
		TSharedPtr<const FJsonObject> ProviderExtension;
		TSharedPtr<const FJsonValue> Ordering;

		bool HasProperties() const { return !Properties.IsEmpty(); }
	};

	struct UEAGENTINTERFACE_API FParseFailure
	{
		UE::UAI::Contract::FDiagnostic Diagnostic;
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
			UE::UAI::Contract::FDiagnostic& OutError);
		bool FindContract(const FString& ContractId, FContract& OutContract) const;
		TArray<FContract> ListContracts() const;
		int32 GetContractCount() const;

	private:
		mutable FRWLock RegistryLock;
		TMap<FString, FContract> Contracts;
	};

	UEAGENTINTERFACE_API FString OperationToString(EOperation Operation);
	UEAGENTINTERFACE_API bool ParseOperation(const FString& Text, EOperation& OutOperation);
	UEAGENTINTERFACE_API bool ParseContract(
		const TSharedPtr<FJsonObject>& Object,
		FContract& OutContract,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool ValidateContract(const FContract& Contract, UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool ParseOperationInput(
		const FContract& Contract,
		const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit& Unit,
		FOperationInput& OutInput,
		FParseFailure& OutFailure);
}
