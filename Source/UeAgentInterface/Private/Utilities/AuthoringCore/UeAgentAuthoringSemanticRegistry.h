// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"

namespace UeAgentAuthoringSemanticRegistry
{
	enum class ERegistrationResult : uint8
	{
		Registered,
		AlreadyRegistered
	};

	enum class EWriteCapability : uint8
	{
		ReadOnly,
		PropertyAssignment,
		DomainMutation
	};

	enum class EDefaultSourceKind : uint8
	{
		None,
		ExistingInstance,
		Factory,
		Constructor,
		StructInitialization,
		Schema,
		DomainNative,
		PostCreate
	};

	struct UEAGENTINTERFACE_API FDomainDescriptor
	{
		FString DomainId;
		FString MutationProviderId;
		TArray<FString> TargetKinds;
		TArray<FString> CapabilityTags;

		bool operator==(const FDomainDescriptor& Other) const;
		bool operator!=(const FDomainDescriptor& Other) const { return !(*this == Other); }
	};

	struct UEAGENTINTERFACE_API FSemanticDescriptor
	{
		FString SemanticId;
		FString DomainId;
		FString TargetKind;
		FString ValueType;
		FString OwnerModel;
		FString IdentityModel;
		FString StateAccessor;
		EWriteCapability WriteCapability = EWriteCapability::ReadOnly;
		FString ConstraintSource;
		FString AvailabilitySource;
		EDefaultSourceKind DefaultSourceKind = EDefaultSourceKind::None;
		FString DefaultAccessor;
		FString Comparator;
		TArray<FString> CapabilityTags;

		bool operator==(const FSemanticDescriptor& Other) const;
		bool operator!=(const FSemanticDescriptor& Other) const { return !(*this == Other); }
	};

	class UEAGENTINTERFACE_API FRegistry
	{
	public:
		FRegistry() = default;
		FRegistry(const FRegistry&) = delete;
		FRegistry& operator=(const FRegistry&) = delete;

		static FRegistry& Get();

		bool RegisterDomain(
			const FDomainDescriptor& Descriptor,
			ERegistrationResult& OutResult,
			FString& OutError);
		bool RegisterSemantic(
			const FSemanticDescriptor& Descriptor,
			ERegistrationResult& OutResult,
			FString& OutError);
		bool FindDomain(const FString& DomainId, FDomainDescriptor& OutDescriptor) const;
		bool FindSemantic(const FString& SemanticId, FSemanticDescriptor& OutDescriptor) const;
		TArray<FDomainDescriptor> ListDomains() const;
		TArray<FSemanticDescriptor> ListSemantics() const;
		int32 GetDomainCount() const;
		int32 GetSemanticCount() const;

	private:
		mutable FRWLock RegistryLock;
		TMap<FString, FDomainDescriptor> Domains;
		TMap<FString, FSemanticDescriptor> Semantics;
	};

	UEAGENTINTERFACE_API bool RegisterBuiltInSemantics(FRegistry& Registry, FString& OutError);
}
