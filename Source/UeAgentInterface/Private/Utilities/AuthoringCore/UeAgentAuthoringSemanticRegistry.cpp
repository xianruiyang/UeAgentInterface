// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticRegistry.h"

#include "Misc/ScopeRWLock.h"

namespace UeAgentAuthoringSemanticRegistry
{
	namespace
	{
		FString NormalizeId(FString Value)
		{
			Value.TrimStartAndEndInline();
			Value.ToLowerInline();
			return Value;
		}

		void NormalizeList(TArray<FString>& Values)
		{
			for (FString& Value : Values) Value = NormalizeId(MoveTemp(Value));
			Values.RemoveAll([](const FString& Value) { return Value.IsEmpty(); });
			Values.Sort();
			for (int32 Index = Values.Num() - 1; Index > 0; --Index)
			{
				if (Values[Index] == Values[Index - 1]) Values.RemoveAt(Index);
			}
		}

		FDomainDescriptor Normalize(FDomainDescriptor Descriptor)
		{
			Descriptor.DomainId = NormalizeId(MoveTemp(Descriptor.DomainId));
			Descriptor.MutationProviderId = NormalizeId(MoveTemp(Descriptor.MutationProviderId));
			NormalizeList(Descriptor.TargetKinds);
			NormalizeList(Descriptor.CapabilityTags);
			return Descriptor;
		}

		FSemanticDescriptor Normalize(FSemanticDescriptor Descriptor)
		{
			Descriptor.SemanticId = NormalizeId(MoveTemp(Descriptor.SemanticId));
			Descriptor.DomainId = NormalizeId(MoveTemp(Descriptor.DomainId));
			Descriptor.TargetKind = NormalizeId(MoveTemp(Descriptor.TargetKind));
			Descriptor.ValueType = NormalizeId(MoveTemp(Descriptor.ValueType));
			Descriptor.OwnerModel = NormalizeId(MoveTemp(Descriptor.OwnerModel));
			Descriptor.IdentityModel = NormalizeId(MoveTemp(Descriptor.IdentityModel));
			Descriptor.StateAccessor = NormalizeId(MoveTemp(Descriptor.StateAccessor));
			Descriptor.ConstraintSource = NormalizeId(MoveTemp(Descriptor.ConstraintSource));
			Descriptor.AvailabilitySource = NormalizeId(MoveTemp(Descriptor.AvailabilitySource));
			Descriptor.DefaultAccessor = NormalizeId(MoveTemp(Descriptor.DefaultAccessor));
			Descriptor.Comparator = NormalizeId(MoveTemp(Descriptor.Comparator));
			NormalizeList(Descriptor.CapabilityTags);
			return Descriptor;
		}

		bool Validate(const FDomainDescriptor& Descriptor, FString& OutError)
		{
			if (Descriptor.DomainId.IsEmpty())
			{
				OutError = TEXT("missing_domain_id");
				return false;
			}
			OutError.Reset();
			return true;
		}

		bool Validate(const FSemanticDescriptor& Descriptor, FString& OutError)
		{
			if (Descriptor.SemanticId.IsEmpty()) OutError = TEXT("missing_semantic_id");
			else if (Descriptor.DomainId.IsEmpty()) OutError = TEXT("missing_domain_id");
			else if (Descriptor.TargetKind.IsEmpty()) OutError = TEXT("missing_target_kind");
			else if (Descriptor.ValueType.IsEmpty()) OutError = TEXT("missing_value_type");
			else if (Descriptor.IdentityModel.IsEmpty()) OutError = TEXT("missing_identity_model");
			else if (Descriptor.StateAccessor.IsEmpty()) OutError = TEXT("missing_state_accessor");
			else if (Descriptor.Comparator.IsEmpty()) OutError = TEXT("missing_comparator");
			else
			{
				OutError.Reset();
				return true;
			}
			return false;
		}
	}

	bool FDomainDescriptor::operator==(const FDomainDescriptor& Other) const
	{
		return DomainId == Other.DomainId
			&& MutationProviderId == Other.MutationProviderId
			&& TargetKinds == Other.TargetKinds
			&& CapabilityTags == Other.CapabilityTags;
	}

	bool FSemanticDescriptor::operator==(const FSemanticDescriptor& Other) const
	{
		return SemanticId == Other.SemanticId
			&& DomainId == Other.DomainId
			&& TargetKind == Other.TargetKind
			&& ValueType == Other.ValueType
			&& OwnerModel == Other.OwnerModel
			&& IdentityModel == Other.IdentityModel
			&& StateAccessor == Other.StateAccessor
			&& WriteCapability == Other.WriteCapability
			&& ConstraintSource == Other.ConstraintSource
			&& AvailabilitySource == Other.AvailabilitySource
			&& DefaultSourceKind == Other.DefaultSourceKind
			&& DefaultAccessor == Other.DefaultAccessor
			&& Comparator == Other.Comparator
			&& CapabilityTags == Other.CapabilityTags;
	}

	FRegistry& FRegistry::Get()
	{
		static FRegistry Registry;
		static const bool bBuiltInsRegistered = []()
		{
			FString Error;
			return RegisterBuiltInSemantics(Registry, Error);
		}();
		(void)bBuiltInsRegistered;
		return Registry;
	}

	bool FRegistry::RegisterDomain(const FDomainDescriptor& Descriptor, ERegistrationResult& OutResult, FString& OutError)
	{
		const FDomainDescriptor Canonical = Normalize(Descriptor);
		if (!Validate(Canonical, OutError)) return false;
		FWriteScopeLock Lock(RegistryLock);
		if (const FDomainDescriptor* Existing = Domains.Find(Canonical.DomainId))
		{
			if (*Existing != Canonical)
			{
				OutError = FString::Printf(TEXT("conflicting_domain_descriptor_%s"), *Canonical.DomainId);
				return false;
			}
			OutResult = ERegistrationResult::AlreadyRegistered;
			OutError.Reset();
			return true;
		}
		Domains.Add(Canonical.DomainId, Canonical);
		OutResult = ERegistrationResult::Registered;
		OutError.Reset();
		return true;
	}

	bool FRegistry::RegisterSemantic(const FSemanticDescriptor& Descriptor, ERegistrationResult& OutResult, FString& OutError)
	{
		const FSemanticDescriptor Canonical = Normalize(Descriptor);
		if (!Validate(Canonical, OutError)) return false;
		FWriteScopeLock Lock(RegistryLock);
		const FDomainDescriptor* Domain = Domains.Find(Canonical.DomainId);
		if (!Domain)
		{
			OutError = FString::Printf(TEXT("unknown_semantic_domain_%s"), *Canonical.DomainId);
			return false;
		}
		if (Canonical.WriteCapability == EWriteCapability::DomainMutation && Domain->MutationProviderId.IsEmpty())
		{
			OutError = FString::Printf(TEXT("domain_mutation_provider_missing_%s"), *Canonical.DomainId);
			return false;
		}
		if (const FSemanticDescriptor* Existing = Semantics.Find(Canonical.SemanticId))
		{
			if (*Existing != Canonical)
			{
				OutError = FString::Printf(TEXT("conflicting_semantic_descriptor_%s"), *Canonical.SemanticId);
				return false;
			}
			OutResult = ERegistrationResult::AlreadyRegistered;
			OutError.Reset();
			return true;
		}
		Semantics.Add(Canonical.SemanticId, Canonical);
		OutResult = ERegistrationResult::Registered;
		OutError.Reset();
		return true;
	}

	bool FRegistry::FindDomain(const FString& DomainId, FDomainDescriptor& OutDescriptor) const
	{
		FReadScopeLock Lock(RegistryLock);
		if (const FDomainDescriptor* Found = Domains.Find(NormalizeId(DomainId)))
		{
			OutDescriptor = *Found;
			return true;
		}
		return false;
	}

	bool FRegistry::FindSemantic(const FString& SemanticId, FSemanticDescriptor& OutDescriptor) const
	{
		FReadScopeLock Lock(RegistryLock);
		if (const FSemanticDescriptor* Found = Semantics.Find(NormalizeId(SemanticId)))
		{
			OutDescriptor = *Found;
			return true;
		}
		return false;
	}

	TArray<FDomainDescriptor> FRegistry::ListDomains() const
	{
		FReadScopeLock Lock(RegistryLock);
		TArray<FDomainDescriptor> Result;
		Domains.GenerateValueArray(Result);
		Result.Sort([](const FDomainDescriptor& Left, const FDomainDescriptor& Right) { return Left.DomainId < Right.DomainId; });
		return Result;
	}

	TArray<FSemanticDescriptor> FRegistry::ListSemantics() const
	{
		FReadScopeLock Lock(RegistryLock);
		TArray<FSemanticDescriptor> Result;
		Semantics.GenerateValueArray(Result);
		Result.Sort([](const FSemanticDescriptor& Left, const FSemanticDescriptor& Right) { return Left.SemanticId < Right.SemanticId; });
		return Result;
	}

	int32 FRegistry::GetDomainCount() const
	{
		FReadScopeLock Lock(RegistryLock);
		return Domains.Num();
	}

	int32 FRegistry::GetSemanticCount() const
	{
		FReadScopeLock Lock(RegistryLock);
		return Semantics.Num();
	}

	bool RegisterBuiltInSemantics(FRegistry& Registry, FString& OutError)
	{
		FDomainDescriptor Domain;
		Domain.DomainId = TEXT("core.reflection");
		Domain.TargetKinds = { TEXT("property") };
		Domain.CapabilityTags = { TEXT("reflection"), TEXT("property") };
		ERegistrationResult RegistrationResult;
		if (!Registry.RegisterDomain(Domain, RegistrationResult, OutError)) return false;

		FSemanticDescriptor Semantic;
		Semantic.SemanticId = TEXT("core.property");
		Semantic.DomainId = Domain.DomainId;
		Semantic.TargetKind = TEXT("property");
		Semantic.ValueType = TEXT("*");
		Semantic.OwnerModel = TEXT("property_owner");
		Semantic.IdentityModel = TEXT("property_identity");
		Semantic.StateAccessor = TEXT("core.property");
		Semantic.WriteCapability = EWriteCapability::PropertyAssignment;
		Semantic.DefaultSourceKind = EDefaultSourceKind::ExistingInstance;
		Semantic.Comparator = TEXT("uai.compare.canonical_value");
		Semantic.CapabilityTags = { TEXT("reflection"), TEXT("property"), TEXT("editable") };
		return Registry.RegisterSemantic(Semantic, RegistrationResult, OutError);
	}
}
