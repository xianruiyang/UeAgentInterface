// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticRegistry.h"

#include "Misc/ScopeRWLock.h"
#include "Dom/JsonValue.h"

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


        using UE::UAI::Contract::FDiagnostic;
        FDiagnostic RegistryFailure(const TCHAR* Code, const TCHAR* Reason, const FString& Name, const FString& Field = FString()) {
            auto Error = FDiagnostic::Failure(Code, Reason, TEXT("semantic_registry"));
            Error.SourceKind = TEXT("native_result");
            Error.CandidateName = Name;
            Error.PropertyPath = Field;
            return Error;
        }
        FDiagnostic MissingField(const TCHAR* Code, const TCHAR* Reason, const FString& Name, const TCHAR* Field) {
            auto Error = RegistryFailure(Code, Reason, Name, Field);
            Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("nonempty string"));
            Error.ActualValue = MakeShared<FJsonValueString>(FString());
            return Error;
        }
        TSharedPtr<FJsonValue> DescriptorValue(const FString& Value) { return MakeShared<FJsonValueString>(Value); }
        TSharedPtr<FJsonValue> DescriptorValue(int32 Value) { return MakeShared<FJsonValueNumber>(Value); }
        template<typename T>
        bool DescriptorValuesEqual(const T& Expected, const T& Actual, const FString& Path, FDiagnostic* Error) {
            if (Expected == Actual) return true;
            if (Error) { Error->PropertyPath = Path; Error->ExpectedValue = DescriptorValue(Expected); Error->ActualValue = DescriptorValue(Actual); }
            return false;
        }
        bool DescriptorValuesEqual(const TArray<FString>& Expected, const TArray<FString>& Actual, const FString& Path, FDiagnostic* Error) {
            if (!DescriptorValuesEqual(Expected.Num(), Actual.Num(), Path + TEXT("/count"), Error)) return false;
            for (int32 Index = 0; Index < Expected.Num(); ++Index)
                if (!DescriptorValuesEqual(Expected[Index], Actual[Index], Path + TEXT("/") + FString::FromInt(Index), Error)) return false;
            return true;
        }
        bool DescriptorsEqual(const FDomainDescriptor& Expected, const FDomainDescriptor& Actual, FDiagnostic* Error) {
            return DescriptorValuesEqual(Expected.DomainId, Actual.DomainId, TEXT("/domain_id"), Error)
                && DescriptorValuesEqual(Expected.MutationProviderId, Actual.MutationProviderId, TEXT("/mutation_provider_id"), Error)
                && DescriptorValuesEqual(Expected.TargetKinds, Actual.TargetKinds, TEXT("/target_kinds"), Error)
                && DescriptorValuesEqual(Expected.CapabilityTags, Actual.CapabilityTags, TEXT("/capability_tags"), Error);
        }
        bool DescriptorsEqual(const FSemanticDescriptor& Expected, const FSemanticDescriptor& Actual, FDiagnostic* Error) {
            return DescriptorValuesEqual(Expected.SemanticId, Actual.SemanticId, TEXT("/semantic_id"), Error)
                && DescriptorValuesEqual(Expected.DomainId, Actual.DomainId, TEXT("/domain_id"), Error)
                && DescriptorValuesEqual(Expected.TargetKind, Actual.TargetKind, TEXT("/target_kind"), Error)
                && DescriptorValuesEqual(Expected.ValueType, Actual.ValueType, TEXT("/value_type"), Error)
                && DescriptorValuesEqual(Expected.OwnerModel, Actual.OwnerModel, TEXT("/owner_model"), Error)
                && DescriptorValuesEqual(Expected.IdentityModel, Actual.IdentityModel, TEXT("/identity_model"), Error)
                && DescriptorValuesEqual(Expected.StateAccessor, Actual.StateAccessor, TEXT("/state_accessor"), Error)
                && DescriptorValuesEqual(static_cast<int32>(Expected.WriteCapability), static_cast<int32>(Actual.WriteCapability), TEXT("/write_capability"), Error)
                && DescriptorValuesEqual(Expected.ConstraintSource, Actual.ConstraintSource, TEXT("/constraint_source"), Error)
                && DescriptorValuesEqual(Expected.AvailabilitySource, Actual.AvailabilitySource, TEXT("/availability_source"), Error)
                && DescriptorValuesEqual(static_cast<int32>(Expected.DefaultSourceKind), static_cast<int32>(Actual.DefaultSourceKind), TEXT("/default_source_kind"), Error)
                && DescriptorValuesEqual(Expected.DefaultAccessor, Actual.DefaultAccessor, TEXT("/default_accessor"), Error)
                && DescriptorValuesEqual(Expected.Comparator, Actual.Comparator, TEXT("/comparator"), Error)
                && DescriptorValuesEqual(Expected.CapabilityTags, Actual.CapabilityTags, TEXT("/capability_tags"), Error);
        }

		bool Validate(const FDomainDescriptor& Descriptor, UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (Descriptor.DomainId.IsEmpty())
			{
				OutError = MissingField(TEXT("missing_domain_id"), TEXT("The semantic registration domain identifier is empty."), Descriptor.DomainId, TEXT("domain_id"));
				return false;
			}
			OutError.Reset();
			return true;
		}

		bool Validate(const FSemanticDescriptor& Descriptor, UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (Descriptor.SemanticId.IsEmpty()) OutError = MissingField(TEXT("missing_semantic_id"), TEXT("The semantic identifier is empty."), Descriptor.DomainId, TEXT("semantic_id"));
			else if (Descriptor.DomainId.IsEmpty()) OutError = MissingField(TEXT("missing_domain_id"), TEXT("The semantic registration domain identifier is empty."), Descriptor.DomainId, TEXT("domain_id"));
			else if (Descriptor.TargetKind.IsEmpty()) OutError = MissingField(TEXT("missing_target_kind"), TEXT("The semantic target kind is empty."), Descriptor.DomainId, TEXT("target_kind"));
			else if (Descriptor.ValueType.IsEmpty()) OutError = MissingField(TEXT("missing_value_type"), TEXT("The semantic value type is empty."), Descriptor.DomainId, TEXT("value_type"));
			else if (Descriptor.IdentityModel.IsEmpty()) OutError = MissingField(TEXT("missing_identity_model"), TEXT("The semantic identity model is empty."), Descriptor.DomainId, TEXT("identity_model"));
			else if (Descriptor.StateAccessor.IsEmpty()) OutError = MissingField(TEXT("missing_state_accessor"), TEXT("The semantic state accessor identifier is empty."), Descriptor.DomainId, TEXT("state_accessor"));
			else if (Descriptor.Comparator.IsEmpty()) OutError = MissingField(TEXT("missing_comparator"), TEXT("The semantic comparator identifier is empty."), Descriptor.DomainId, TEXT("comparator"));
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
		return DescriptorsEqual(*this, Other, nullptr);
	}

	bool FSemanticDescriptor::operator==(const FSemanticDescriptor& Other) const
	{
		return DescriptorsEqual(*this, Other, nullptr);
	}

	FRegistry& FRegistry::Get()
	{
		static FRegistry Registry;
		static const bool bBuiltInsRegistered = []()
		{
			UE::UAI::Contract::FDiagnostic Error;
			return RegisterBuiltInSemantics(Registry, Error);
		}();
		(void)bBuiltInsRegistered;
		return Registry;
	}

	bool FRegistry::RegisterDomain(const FDomainDescriptor& Descriptor, ERegistrationResult& OutResult, UE::UAI::Contract::FDiagnostic& OutError)
	{
		const FDomainDescriptor Canonical = Normalize(Descriptor);
		if (!Validate(Canonical, OutError)) return false;
		FWriteScopeLock Lock(RegistryLock);
		if (const FDomainDescriptor* Existing = Domains.Find(Canonical.DomainId))
		{
			if (*Existing != Canonical)
			{
				OutError = RegistryFailure(TEXT("conflicting_domain_descriptor"), TEXT("The domain descriptor differs from its existing registration."), Canonical.DomainId);
                DescriptorsEqual(*Existing, Canonical, &OutError);
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

	bool FRegistry::RegisterSemantic(const FSemanticDescriptor& Descriptor, ERegistrationResult& OutResult, UE::UAI::Contract::FDiagnostic& OutError)
	{
		const FSemanticDescriptor Canonical = Normalize(Descriptor);
		if (!Validate(Canonical, OutError)) return false;
		FWriteScopeLock Lock(RegistryLock);
		const FDomainDescriptor* Domain = Domains.Find(Canonical.DomainId);
		if (!Domain)
		{
			OutError = RegistryFailure(TEXT("unknown_semantic_domain"), TEXT("The semantic descriptor references an unregistered domain."), Canonical.SemanticId, TEXT("domain_id"));
            OutError.ActualValue = MakeShared<FJsonValueString>(Canonical.DomainId);
			return false;
		}
		if (Canonical.WriteCapability == EWriteCapability::DomainMutation && Domain->MutationProviderId.IsEmpty())
		{
			OutError = MissingField(TEXT("domain_mutation_provider_missing"), TEXT("A writable semantic requires its domain to declare a mutation provider."), Canonical.DomainId, TEXT("mutation_provider_id"));
			return false;
		}
		if (const FSemanticDescriptor* Existing = Semantics.Find(Canonical.SemanticId))
		{
			if (*Existing != Canonical)
			{
				OutError = RegistryFailure(TEXT("conflicting_semantic_descriptor"), TEXT("The semantic descriptor differs from its existing registration."), Canonical.SemanticId);
                DescriptorsEqual(*Existing, Canonical, &OutError);
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

	bool RegisterBuiltInSemantics(FRegistry& Registry, UE::UAI::Contract::FDiagnostic& OutError)
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
