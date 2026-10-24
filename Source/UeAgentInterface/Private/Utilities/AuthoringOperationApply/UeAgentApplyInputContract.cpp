// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringOperationApply/UeAgentApplyInputContract.h"
#include "Misc/ScopeExit.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Internationalization/Regex.h"
#include "Misc/ScopeRWLock.h"

namespace
{
	using namespace UeAgentApplyInputContract;
	using UeAgentAuthoringDiagnostics::MakeValueFailure;

	FString NormalizeId(FString Value)
	{
		Value.TrimStartAndEndInline();
		Value.ToLowerInline();
		return Value;
	}

	bool Matches(const FString& Value, const TCHAR* Pattern)
	{
		const FRegexPattern RegexPattern(Pattern);
		FRegexMatcher Matcher(RegexPattern, Value);
		return Matcher.FindNext() && Matcher.GetMatchBeginning() == 0 && Matcher.GetMatchEnding() == Value.Len();
	}

	TSharedPtr<FJsonValue> StringValue(const FString& Value)
	{
		return MakeShared<FJsonValueString>(Value);
	}

	TSharedPtr<FJsonValue> StringArrayValue(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		for (const auto& Value : Values) Items.Add(StringValue(Value));
		return MakeShared<FJsonValueArray>(MoveTemp(Items));
	}

	bool ValidateUniqueStrings(const TArray<FString>& Values, const FString& Path,
		const FString& Code, UE::UAI::Contract::FDiagnostic& Error)
	{
		TSet<FString> Seen;
		for (int32 Index = 0; Index < Values.Num(); ++Index)
		{
			const FString& Value = Values[Index];
			if (Value.IsEmpty() || Seen.Contains(Value))
			{
				Error = MakeValueFailure(Code, Path + TEXT("/") + FString::FromInt(Index),
					StringValue(TEXT("nonempty unique string")), StringValue(Value), TEXT("contract"),
					Value.IsEmpty() ? TEXT("The contract entry is empty after normalization.") : TEXT("The contract entry duplicates an earlier entry."));
				return false;
			}
			Seen.Add(Value);
		}
		return true;
	}


	bool HasExactSet(const TArray<FString>& Values, std::initializer_list<const TCHAR*> Expected)
	{
		if (Values.Num() != static_cast<int32>(Expected.size())) return false;
		TSet<FString> Actual;
		for (const FString& Value : Values) Actual.Add(Value);
		for (const TCHAR* Value : Expected)
		{
			if (!Actual.Contains(Value)) return false;
		}
		return true;
	}

	bool IsOneOf(const FString& Value, std::initializer_list<const TCHAR*> Allowed)
	{
		for (const TCHAR* Candidate : Allowed)
		{
			if (Value == Candidate) return true;
		}
		return false;
	}

	bool RequireOnlyFields(const TSharedPtr<FJsonObject>& Object,
		std::initializer_list<const TCHAR*> AllowedFields, const FString& Path,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!Object.IsValid())
		{
			OutError = MakeValueFailure(TEXT("apply_input_object_required"), Path, StringValue(TEXT("object")), nullptr,
				TEXT("contract"), TEXT("The contract section must be an object."));
			OutError.ActualValuePresent = false;
			return false;
		}
		TSet<FString> Allowed;
		for (const TCHAR* Field : AllowedFields) Allowed.Add(Field);
		for (const auto& Pair : Object->Values)
		{
			if (!Allowed.Contains(Pair.Key))
			{
				OutError = MakeValueFailure(TEXT("apply_input_unknown_field"), Pair.Key, nullptr, nullptr,
					TEXT("contract"), TEXT("The contract section does not accept this field."));
				UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, Path, true);
				OutError.ActualValuePresent = true;
				return false;
			}
		}
		return true;
	}

	bool ReadObject(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field,
		const TSharedPtr<FJsonObject>*& OutValue, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!Object->TryGetObjectField(Field, OutValue) || !OutValue || !OutValue->IsValid())
		{
			OutError = MakeValueFailure(TEXT("apply_input_object_required"), TEXT("/") + FString(Field),
				StringValue(TEXT("object")), Object->Values.FindRef(Field), TEXT("contract"), TEXT("The contract section must be an object."));
			OutError.ActualValuePresent = Object->HasField(Field);
			return false;
		}
		return true;
	}

	bool ReadString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field,
		const FString& Path, FString& OutValue, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!Object.IsValid() || !Object->TryGetStringField(Field, OutValue) || OutValue.IsEmpty())
		{
			OutError = MakeValueFailure(TEXT("apply_input_missing"), Path + TEXT("/") + Field,
				StringValue(TEXT("nonempty string")), Object ? Object->Values.FindRef(Field) : nullptr,
				TEXT("contract"), TEXT("The contract field must contain a nonempty string."));
			OutError.ActualValuePresent = Object && Object->HasField(Field);
			return false;
		}
		return true;
	}

	bool ReadBool(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field,
		const FString& Path, bool& OutValue, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!Object.IsValid() || !Object->TryGetBoolField(Field, OutValue))
		{
			OutError = MakeValueFailure(TEXT("apply_input_missing"), Path + TEXT("/") + Field,
				StringValue(TEXT("boolean")), Object ? Object->Values.FindRef(Field) : nullptr,
				TEXT("contract"), TEXT("The contract field must contain a boolean."));
			OutError.ActualValuePresent = Object && Object->HasField(Field);
			return false;
		}
		return true;
	}

	bool ReadStringArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field,
		const FString& Path, TArray<FString>& OutValues, UE::UAI::Contract::FDiagnostic& OutError, bool bAllowEmpty = false)
	{
		OutValues.Reset();
		const FString FieldPath = Path + TEXT("/") + Field;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object.IsValid() || !Object->TryGetArrayField(Field, Values) || !Values || (!bAllowEmpty && Values->IsEmpty()))
		{
			OutError = MakeValueFailure(TEXT("apply_input_missing"), FieldPath,
				StringValue(bAllowEmpty ? TEXT("array of strings") : TEXT("nonempty array of strings")),
				Object ? Object->Values.FindRef(Field) : nullptr, TEXT("contract"), TEXT("The contract field must contain the required string array."));
			OutError.ActualValuePresent = Object && Object->HasField(Field);
			return false;
		}
		for (int32 Index = 0; Index < Values->Num(); ++Index)
		{
			const auto& Value = (*Values)[Index];
			if (!Value.IsValid() || Value->Type != EJson::String || Value->AsString().IsEmpty())
			{
				OutError = MakeValueFailure(TEXT("apply_input_invalid"), FieldPath + TEXT("/") + FString::FromInt(Index),
					StringValue(TEXT("nonempty string")), Value, TEXT("contract"), TEXT("The contract array entry must be a nonempty string."));
				return false;
			}
			OutValues.Add(NormalizeId(Value->AsString()));
		}
		return ValidateUniqueStrings(OutValues, FieldPath, TEXT("apply_input_duplicate"), OutError);
	}


	// Equality and registration conflicts share this typed traversal. Only the
	// first differing leaf is projected into diagnostic values; contracts are
	// never serialized and reparsed to discover a difference.
	TSharedPtr<FJsonValue> ContractLeafValue(const FString& Value) { return StringValue(Value); }
	TSharedPtr<FJsonValue> ContractLeafValue(bool Value) { return MakeShared<FJsonValueBoolean>(Value); }
	TSharedPtr<FJsonValue> ContractLeafValue(int32 Value) { return MakeShared<FJsonValueNumber>(Value); }
	TSharedPtr<FJsonValue> ContractLeafValue(EOperation Value) { return StringValue(OperationToString(Value)); }

	template<typename TValue>
	bool ContractValuesEqual(const TValue& Expected, const TValue& Actual, const FString& Path,
		UE::UAI::Contract::FDiagnostic* Error)
	{
		if (Expected == Actual) return true;
		if (Error) *Error = MakeValueFailure(TEXT("conflicting_apply_input_contract"), Path,
			ContractLeafValue(Expected), ContractLeafValue(Actual), TEXT("contract"),
			TEXT("The new contract differs from the contract already registered with this identifier."));
		return false;
	}
	bool ContractValuesEqual(const FInputFieldDescriptor& Expected, const FInputFieldDescriptor& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error);
	bool ContractValuesEqual(const FOperationDescriptor& Expected, const FOperationDescriptor& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error);
	bool ContractValuesEqual(const FReflectionBaseline& Expected, const FReflectionBaseline& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error);
	bool ContractValuesEqual(const FPropertyPayload& Expected, const FPropertyPayload& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error);
	bool ContractValuesEqual(const FVerificationPolicy& Expected, const FVerificationPolicy& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error);
	bool ContractValuesEqual(const FContract& Expected, const FContract& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error);

	template<typename TValue>
	bool ContractValuesEqual(const TArray<TValue>& Expected, const TArray<TValue>& Actual,
		const FString& Path, UE::UAI::Contract::FDiagnostic* Error)
	{
		if (Expected.Num() != Actual.Num())
			return ContractValuesEqual(Expected.Num(), Actual.Num(), Path + TEXT("/length"), Error);
		for (int32 Index = 0; Index < Expected.Num(); ++Index)
			if (!ContractValuesEqual(Expected[Index], Actual[Index], Path + TEXT("/") + FString::FromInt(Index), Error)) return false;
		return true;
	}

	bool ContractValuesEqual(const FInputFieldDescriptor& Expected, const FInputFieldDescriptor& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error)
	{
		return ContractValuesEqual(Expected.Name, Actual.Name, Path + TEXT("/name"), Error)
			&& ContractValuesEqual(Expected.Role, Actual.Role, Path + TEXT("/role"), Error)
			&& ContractValuesEqual(Expected.TypeId, Actual.TypeId, Path + TEXT("/type_id"), Error)
			&& ContractValuesEqual(Expected.RequiredFor, Actual.RequiredFor, Path + TEXT("/required_for"), Error)
			&& ContractValuesEqual(Expected.SemanticId, Actual.SemanticId, Path + TEXT("/semantic_id"), Error);
	}

	bool ContractValuesEqual(const FOperationDescriptor& Expected, const FOperationDescriptor& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error)
	{
		return ContractValuesEqual(Expected.Operation, Actual.Operation, Path + TEXT("/operation"), Error)
			&& ContractValuesEqual(Expected.IdentityOneOf, Actual.IdentityOneOf, Path + TEXT("/identity_one_of"), Error)
			&& ContractValuesEqual(Expected.AcceptedSections, Actual.AcceptedSections, Path + TEXT("/accepted_sections"), Error)
			&& ContractValuesEqual(Expected.OmittedFieldPolicy, Actual.OmittedFieldPolicy, Path + TEXT("/omitted_field_policy"), Error)
			&& ContractValuesEqual(Expected.NativeDefaultPolicy, Actual.NativeDefaultPolicy, Path + TEXT("/native_default_policy"), Error)
			&& ContractValuesEqual(Expected.bDestructive, Actual.bDestructive, Path + TEXT("/destructive"), Error);
	}

	bool ContractValuesEqual(const FReflectionBaseline& Expected, const FReflectionBaseline& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error)
	{
		return ContractValuesEqual(Expected.bEnabled, Actual.bEnabled, Path + TEXT("/enabled"), Error)
			&& ContractValuesEqual(Expected.RootKinds, Actual.RootKinds, Path + TEXT("/root_kinds"), Error)
			&& ContractValuesEqual(Expected.RecursiveNodes, Actual.RecursiveNodes, Path + TEXT("/recursive_nodes"), Error)
			&& ContractValuesEqual(Expected.UnknownFieldPolicy, Actual.UnknownFieldPolicy, Path + TEXT("/unknown_field_policy"), Error)
			&& ContractValuesEqual(Expected.ExternalReferencePolicy, Actual.ExternalReferencePolicy, Path + TEXT("/external_reference_policy"), Error)
			&& ContractValuesEqual(Expected.OwnedObjectPolicy, Actual.OwnedObjectPolicy, Path + TEXT("/owned_object_policy"), Error);
	}

	bool ContractValuesEqual(const FPropertyPayload& Expected, const FPropertyPayload& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error)
	{
		return ContractValuesEqual(Expected.Mode, Actual.Mode, Path + TEXT("/mode"), Error)
			&& ContractValuesEqual(Expected.PropertyPathField, Actual.PropertyPathField, Path + TEXT("/property_path_field"), Error)
			&& ContractValuesEqual(Expected.ValueField, Actual.ValueField, Path + TEXT("/value_field"), Error)
			&& ContractValuesEqual(Expected.OptionalAssertionFields, Actual.OptionalAssertionFields, Path + TEXT("/optional_assertion_fields"), Error)
			&& ContractValuesEqual(Expected.OmittedPatchFieldPolicy, Actual.OmittedPatchFieldPolicy, Path + TEXT("/omitted_patch_field_policy"), Error);
	}

	bool ContractValuesEqual(const FVerificationPolicy& Expected, const FVerificationPolicy& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error)
	{
		return ContractValuesEqual(Expected.StateSource, Actual.StateSource, Path + TEXT("/state_source"), Error)
			&& ContractValuesEqual(Expected.CompareSource, Actual.CompareSource, Path + TEXT("/compare_source"), Error)
			&& ContractValuesEqual(Expected.bRequired, Actual.bRequired, Path + TEXT("/required"), Error);
	}

	bool ContractValuesEqual(const FContract& Expected, const FContract& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error)
	{
		return ContractValuesEqual(Expected.Schema, Actual.Schema, Path + TEXT("/schema"), Error)
			&& ContractValuesEqual(Expected.ContractId, Actual.ContractId, Path + TEXT("/contract_id"), Error)
			&& ContractValuesEqual(Expected.ContractVersion, Actual.ContractVersion, Path + TEXT("/contract_version"), Error)
			&& ContractValuesEqual(Expected.ProfileId, Actual.ProfileId, Path + TEXT("/profile_id"), Error)
			&& ContractValuesEqual(Expected.TargetKind, Actual.TargetKind, Path + TEXT("/target_kind"), Error)
			&& ContractValuesEqual(Expected.SemanticDescriptorRefs, Actual.SemanticDescriptorRefs, Path + TEXT("/semantic_descriptor_refs"), Error)
			&& ContractValuesEqual(Expected.ReflectionBaseline, Actual.ReflectionBaseline, Path + TEXT("/reflection_baseline"), Error)
			&& ContractValuesEqual(Expected.PropertyPayload, Actual.PropertyPayload, Path + TEXT("/property_payload"), Error)
			&& ContractValuesEqual(Expected.InputFields, Actual.InputFields, Path + TEXT("/input_fields"), Error)
			&& ContractValuesEqual(Expected.Operations, Actual.Operations, Path + TEXT("/operations"), Error)
			&& ContractValuesEqual(Expected.ProviderId, Actual.ProviderId, Path + TEXT("/provider_id"), Error)
			&& ContractValuesEqual(Expected.UnknownInputFieldPolicy, Actual.UnknownInputFieldPolicy, Path + TEXT("/unknown_input_field_policy"), Error)
			&& ContractValuesEqual(Expected.Verification, Actual.Verification, Path + TEXT("/verification"), Error);
	}

	void SetParseFailure(
		FParseFailure& OutFailure,
		UE::UAI::Contract::FDiagnostic Diagnostic,
		const FString& FieldPath = FString())
	{
		OutFailure = FParseFailure();
		OutFailure.Diagnostic = MoveTemp(Diagnostic);
		OutFailure.Diagnostic.PropertyPath = FieldPath;
		OutFailure.Diagnostic.Source.JsonPath = FieldPath;
	}

	bool HasMeaningfulValue(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid() || Value->IsNull()) return false;
		if (Value->Type == EJson::String) return !Value->AsString().TrimStartAndEnd().IsEmpty();
		return true;
	}

	bool HasInputField(const FOperationInput& Input, const FInputFieldDescriptor& Field)
	{
		if (Field.Role == TEXT("operation_control")) return true;
		if (Field.Role == TEXT("intent")) return Field.Name == TEXT("properties") && Input.HasProperties();
		if (Field.Role == TEXT("provider_extension"))
		{
			return Input.ProviderExtension.IsValid()
				&& HasMeaningfulValue(Input.ProviderExtension->TryGetField(Field.Name));
		}
		return Input.Target.IsValid() && HasMeaningfulValue(Input.Target->TryGetField(Field.Name));
	}

	bool HasInputName(const FContract& Contract, const FOperationInput& Input, const FString& Name)
	{
		const FInputFieldDescriptor* Field = Contract.FindInputField(Name);
		return Field && HasInputField(Input, *Field);
	}
}

namespace UeAgentApplyInputContract
{
	bool FInputFieldDescriptor::operator==(const FInputFieldDescriptor& Other) const
	{
		return ContractValuesEqual(*this, Other, FString(), nullptr);
	}

	bool FOperationDescriptor::AcceptsSection(const FString& Section) const
	{
		return AcceptedSections.Contains(NormalizeId(Section));
	}

	bool FOperationDescriptor::operator==(const FOperationDescriptor& Other) const
	{
		return ContractValuesEqual(*this, Other, FString(), nullptr);
	}

	bool FReflectionBaseline::operator==(const FReflectionBaseline& Other) const
	{
		return ContractValuesEqual(*this, Other, FString(), nullptr);
	}

	bool FPropertyPayload::operator==(const FPropertyPayload& Other) const
	{
		return ContractValuesEqual(*this, Other, FString(), nullptr);
	}

	bool FVerificationPolicy::operator==(const FVerificationPolicy& Other) const
	{
		return ContractValuesEqual(*this, Other, FString(), nullptr);
	}

	const FOperationDescriptor* FContract::FindOperation(EOperation Operation) const
	{
		return Operations.FindByPredicate([Operation](const FOperationDescriptor& Descriptor)
		{
			return Descriptor.Operation == Operation;
		});
	}

	const FInputFieldDescriptor* FContract::FindInputField(const FString& Name) const
	{
		const FString Canonical = NormalizeId(Name);
		return InputFields.FindByPredicate([&Canonical](const FInputFieldDescriptor& Descriptor)
		{
			return Descriptor.Name == Canonical;
		});
	}

	bool FContract::IsValid() const
	{
		UE::UAI::Contract::FDiagnostic Error;
		return ValidateContract(*this, Error);
	}

	bool FContract::operator==(const FContract& Other) const
	{
		return ContractValuesEqual(*this, Other, FString(), nullptr);
	}

	FRegistry& FRegistry::Get()
	{
		static FRegistry Registry;
		return Registry;
	}

	bool FRegistry::RegisterContract(
		const FContract& Contract,
		ERegistrationResult& OutResult,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!ValidateContract(Contract, OutError)) return false;
		FWriteScopeLock Lock(RegistryLock);
		if (const FContract* Existing = Contracts.Find(Contract.ContractId))
		{
			if (!ContractValuesEqual(*Existing, Contract, FString(), &OutError))
			{
				OutError.CandidateName = Contract.ContractId;
				return false;
			}
			OutResult = ERegistrationResult::AlreadyRegistered;
			OutError.Reset();
			return true;
		}
		Contracts.Add(Contract.ContractId, Contract);
		OutResult = ERegistrationResult::Registered;
		OutError.Reset();
		return true;
	}

	bool FRegistry::FindContract(const FString& ContractId, FContract& OutContract) const
	{
		FReadScopeLock Lock(RegistryLock);
		if (const FContract* Found = Contracts.Find(NormalizeId(ContractId)))
		{
			OutContract = *Found;
			return true;
		}
		return false;
	}

	TArray<FContract> FRegistry::ListContracts() const
	{
		FReadScopeLock Lock(RegistryLock);
		TArray<FContract> Result;
		Contracts.GenerateValueArray(Result);
		Result.Sort([](const FContract& Left, const FContract& Right)
		{
			return Left.ContractId < Right.ContractId;
		});
		return Result;
	}

	int32 FRegistry::GetContractCount() const
	{
		FReadScopeLock Lock(RegistryLock);
		return Contracts.Num();
	}

	FString OperationToString(EOperation Operation)
	{
		switch (Operation)
		{
		case EOperation::Create: return TEXT("create");
		case EOperation::Update: return TEXT("update");
		case EOperation::Replace: return TEXT("replace");
		case EOperation::Upsert: return TEXT("upsert");
		case EOperation::Remove: return TEXT("remove");
		case EOperation::Clear: return TEXT("clear");
		case EOperation::Clone: return TEXT("clone");
		case EOperation::Insert: return TEXT("insert");
		case EOperation::Move: return TEXT("move");
		case EOperation::Reorder: return TEXT("reorder");
		default: return TEXT("update");
		}
	}

	bool ParseOperation(const FString& Text, EOperation& OutOperation)
	{
		const FString Canonical = NormalizeId(Text);
		if (Canonical == TEXT("create")) OutOperation = EOperation::Create;
		else if (Canonical == TEXT("update") || Canonical == TEXT("patch")) OutOperation = EOperation::Update;
		else if (Canonical == TEXT("replace")) OutOperation = EOperation::Replace;
		else if (Canonical == TEXT("upsert")) OutOperation = EOperation::Upsert;
		else if (Canonical == TEXT("remove")) OutOperation = EOperation::Remove;
		else if (Canonical == TEXT("clear")) OutOperation = EOperation::Clear;
		else if (Canonical == TEXT("clone")) OutOperation = EOperation::Clone;
		else if (Canonical == TEXT("insert")) OutOperation = EOperation::Insert;
		else if (Canonical == TEXT("move")) OutOperation = EOperation::Move;
		else if (Canonical == TEXT("reorder")) OutOperation = EOperation::Reorder;
		else return false;
		return true;
	}

	bool ValidateContract(const FContract& Contract, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		ON_SCOPE_EXIT { if (!OutError.IsEmpty()) OutError.CandidateName = Contract.ContractId; };
		auto Reject = [&](const TCHAR* Code, const TCHAR* Reason, const FString& Path,
			const FString& Expected, const TSharedPtr<FJsonValue>& Actual)
		{
			OutError = MakeValueFailure(Code, Path, StringValue(Expected), Actual, TEXT("contract"), Reason);
			return false;
		};
		if (Contract.Schema != TEXT("uai.apply_input_contract.v1")) return Reject(TEXT("apply_input_contract_schema_invalid"), TEXT("The apply input contract schema is unsupported."), TEXT("/schema"), TEXT("uai.apply_input_contract.v1"), StringValue(Contract.Schema));
		if (!Matches(Contract.ContractId, TEXT("^uai\\.apply_input\\.[a-z0-9_.-]+$"))) return Reject(TEXT("apply_input_contract_id_invalid"), TEXT("The apply input contract identifier has an invalid format."), TEXT("/contract_id"), TEXT("uai.apply_input.<lowercase identifier>"), StringValue(Contract.ContractId));
		if (!Matches(Contract.ContractVersion, TEXT("^[0-9]+\\.[0-9]+\\.[0-9]+$"))) return Reject(TEXT("apply_input_contract_version_invalid"), TEXT("The contract version must contain three numeric components."), TEXT("/contract_version"), TEXT("major.minor.patch"), StringValue(Contract.ContractVersion));
		if (!Matches(Contract.ProfileId, TEXT("^[a-z][a-z0-9_.-]+$"))) return Reject(TEXT("apply_input_profile_id_invalid"), TEXT("The profile identifier has an invalid format."), TEXT("/profile_id"), TEXT("lowercase identifier starting with a letter"), StringValue(Contract.ProfileId));
		if (Contract.TargetKind.IsEmpty()) return Reject(TEXT("apply_input_target_kind_missing"), TEXT("The contract target kind is empty."), TEXT("/target_kind"), TEXT("nonempty target kind"), StringValue(Contract.TargetKind));
		if (Contract.SemanticDescriptorRefs.IsEmpty()) return Reject(TEXT("apply_input_semantic_refs_invalid"), TEXT("The contract requires at least one semantic descriptor reference."), TEXT("/semantic_descriptor_refs"), TEXT("nonempty array"), StringArrayValue(Contract.SemanticDescriptorRefs));
		if (!ValidateUniqueStrings(Contract.SemanticDescriptorRefs, TEXT("/semantic_descriptor_refs"), TEXT("apply_input_semantic_refs_invalid"), OutError)) return false;
		if (!Contract.ReflectionBaseline.bEnabled) return Reject(TEXT("apply_input_reflection_baseline_invalid"), TEXT("The reflection baseline must be enabled."), TEXT("/reflection_baseline/enabled"), TEXT("true"), MakeShared<FJsonValueBoolean>(false));
		if (!HasExactSet(Contract.ReflectionBaseline.RootKinds, {TEXT("uobject"), TEXT("ustruct")})) return Reject(TEXT("apply_input_reflection_baseline_invalid"), TEXT("The reflection baseline root kinds differ from the required set."), TEXT("/reflection_baseline/root_kinds"), TEXT("uobject, ustruct"), StringArrayValue(Contract.ReflectionBaseline.RootKinds));
		if (!HasExactSet(Contract.ReflectionBaseline.RecursiveNodes, {TEXT("scalar"), TEXT("struct"), TEXT("owned_object"), TEXT("reference"), TEXT("array"), TEXT("set"), TEXT("map"), TEXT("fixed_array"), TEXT("optional")})) return Reject(TEXT("apply_input_reflection_baseline_invalid"), TEXT("The reflection baseline recursive node kinds differ from the required set."), TEXT("/reflection_baseline/recursive_nodes"), TEXT("scalar, struct, owned_object, reference, array, set, map, fixed_array, optional"), StringArrayValue(Contract.ReflectionBaseline.RecursiveNodes));
		if (Contract.ReflectionBaseline.UnknownFieldPolicy != TEXT("reject")) return Reject(TEXT("apply_input_reflection_baseline_invalid"), TEXT("The contract field does not match the required policy."), TEXT("/reflection_baseline/unknown_field_policy"), TEXT("reject"), StringValue(Contract.ReflectionBaseline.UnknownFieldPolicy));
		if (Contract.ReflectionBaseline.ExternalReferencePolicy != TEXT("replace_reference_only")) return Reject(TEXT("apply_input_reflection_baseline_invalid"), TEXT("The contract field does not match the required policy."), TEXT("/reflection_baseline/external_reference_policy"), TEXT("replace_reference_only"), StringValue(Contract.ReflectionBaseline.ExternalReferencePolicy));
		if (Contract.ReflectionBaseline.OwnedObjectPolicy != TEXT("create_or_locate_with_real_outer_then_recurse")) return Reject(TEXT("apply_input_reflection_baseline_invalid"), TEXT("The contract field does not match the required policy."), TEXT("/reflection_baseline/owned_object_policy"), TEXT("create_or_locate_with_real_outer_then_recurse"), StringValue(Contract.ReflectionBaseline.OwnedObjectPolicy));
		if (Contract.PropertyPayload.Mode != TEXT("reflection")) return Reject(TEXT("apply_input_property_payload_invalid"), TEXT("The contract field does not match the required policy."), TEXT("/property_payload/mode"), TEXT("reflection"), StringValue(Contract.PropertyPayload.Mode));
		if (Contract.PropertyPayload.PropertyPathField != TEXT("property_path")) return Reject(TEXT("apply_input_property_payload_invalid"), TEXT("The contract field does not match the required policy."), TEXT("/property_payload/property_path_field"), TEXT("property_path"), StringValue(Contract.PropertyPayload.PropertyPathField));
		if (Contract.PropertyPayload.ValueField != TEXT("value")) return Reject(TEXT("apply_input_property_payload_invalid"), TEXT("The contract field does not match the required policy."), TEXT("/property_payload/value_field"), TEXT("value"), StringValue(Contract.PropertyPayload.ValueField));
		if (Contract.PropertyPayload.OmittedPatchFieldPolicy != TEXT("preserve_current")) return Reject(TEXT("apply_input_property_payload_invalid"), TEXT("The contract field does not match the required policy."), TEXT("/property_payload/omitted_patch_field_policy"), TEXT("preserve_current"), StringValue(Contract.PropertyPayload.OmittedPatchFieldPolicy));
		if (Contract.UnknownInputFieldPolicy != TEXT("reject")) return Reject(TEXT("apply_input_unknown_field_policy_invalid"), TEXT("The contract field does not match the required policy."), TEXT("/unknown_input_field_policy"), TEXT("reject"), StringValue(Contract.UnknownInputFieldPolicy));
		if (Contract.Verification.StateSource != TEXT("shared_state_snapshot")) return Reject(TEXT("apply_input_verification_policy_invalid"), TEXT("The contract field does not match the required policy."), TEXT("/verification/state_source"), TEXT("shared_state_snapshot"), StringValue(Contract.Verification.StateSource));
		if (Contract.Verification.CompareSource != TEXT("shared_semantic_compare")) return Reject(TEXT("apply_input_verification_policy_invalid"), TEXT("The contract field does not match the required policy."), TEXT("/verification/compare_source"), TEXT("shared_semantic_compare"), StringValue(Contract.Verification.CompareSource));
		if (!ValidateUniqueStrings(Contract.PropertyPayload.OptionalAssertionFields, TEXT("/property_payload/optional_assertion_fields"), TEXT("apply_input_property_payload_invalid"), OutError)) return false;
		if (Contract.InputFields.IsEmpty()) return Reject(TEXT("apply_input_fields_missing"), TEXT("The contract has no input fields."), TEXT("/input_fields"), TEXT("at least one field"), MakeShared<FJsonValueNumber>(0));
		if (Contract.Operations.IsEmpty()) return Reject(TEXT("apply_input_operations_missing"), TEXT("The contract has no operations."), TEXT("/operations"), TEXT("at least one operation"), MakeShared<FJsonValueNumber>(0));
		if (Contract.ProviderId.IsEmpty()) return Reject(TEXT("apply_input_provider_missing"), TEXT("The contract provider identifier is empty."), TEXT("/provider_id"), TEXT("nonempty provider identifier"), StringValue(Contract.ProviderId));
		if (!Contract.Verification.bRequired) return Reject(TEXT("apply_input_verification_policy_invalid"), TEXT("The contract must require verification."), TEXT("/verification/required"), TEXT("true"), MakeShared<FJsonValueBoolean>(false));
		static const TSet<FString> AssertionFields = {TEXT("kind"), TEXT("struct_path"), TEXT("enum_path"), TEXT("element_type"), TEXT("key_type"), TEXT("mapped_type"), TEXT("inner_type"), TEXT("fixed_length"), TEXT("allowed_class")};
		for (int32 Index = 0; Index < Contract.PropertyPayload.OptionalAssertionFields.Num(); ++Index)
		{
			const FString& Field = Contract.PropertyPayload.OptionalAssertionFields[Index];
			if (!AssertionFields.Contains(Field)) return Reject(TEXT("apply_input_optional_assertion_invalid"), TEXT("The optional property assertion is unsupported."), FString::Printf(TEXT("/property_payload/optional_assertion_fields/%d"), Index), TEXT("kind, struct_path, enum_path, element_type, key_type, mapped_type, inner_type, fixed_length, allowed_class"), StringValue(Field));
		}
		TSet<FString> FieldNames;
		for (int32 Index = 0; Index < Contract.InputFields.Num(); ++Index)
		{
			const auto& Field = Contract.InputFields[Index];
			const FString Path = FString::Printf(TEXT("/input_fields/%d"), Index);
			if (!Matches(Field.Name, TEXT("^[a-z][a-z0-9_.-]+$"))) return Reject(TEXT("apply_input_field_invalid"), TEXT("The input field name has an invalid format."), Path + TEXT("/name"), TEXT("lowercase identifier starting with a letter"), StringValue(Field.Name));
			if (Field.TypeId.IsEmpty()) return Reject(TEXT("apply_input_field_invalid"), TEXT("The input field type identifier is empty."), Path + TEXT("/type_id"), TEXT("nonempty type identifier"), StringValue(Field.TypeId));
			if (!IsOneOf(Field.Role, {TEXT("identity"), TEXT("selector"), TEXT("intent"), TEXT("operation_control"), TEXT("context_override"), TEXT("resolution_hint"), TEXT("type_assertion"), TEXT("provider_extension")})) return Reject(TEXT("apply_input_field_invalid"), TEXT("The input field role is unsupported."), Path + TEXT("/role"), TEXT("identity, selector, intent, operation_control, context_override, resolution_hint, type_assertion, provider_extension"), StringValue(Field.Role));
			if (FieldNames.Contains(Field.Name)) return Reject(TEXT("apply_input_field_invalid"), TEXT("The input field name duplicates an earlier field."), Path + TEXT("/name"), TEXT("unique field name"), StringValue(Field.Name));
			FieldNames.Add(Field.Name);
		}
		TSet<EOperation> Operations;
		for (int32 Index = 0; Index < Contract.Operations.Num(); ++Index)
		{
			const auto& Operation = Contract.Operations[Index];
			const FString Path = FString::Printf(TEXT("/operations/%d"), Index);
			if (Operations.Contains(Operation.Operation)) return Reject(TEXT("apply_input_operation_invalid"), TEXT("The operation duplicates an earlier declaration."), Path + TEXT("/operation"), TEXT("unique operation"), StringValue(OperationToString(Operation.Operation)));
			if (Operation.IdentityOneOf.IsEmpty()) return Reject(TEXT("apply_input_operation_invalid"), TEXT("The operation has no alternative identity groups."), Path + TEXT("/identity_one_of"), TEXT("nonempty array"), MakeShared<FJsonValueNumber>(0));
			if (Operation.AcceptedSections.IsEmpty()) return Reject(TEXT("apply_input_operation_invalid"), TEXT("The operation accepts no input sections."), Path + TEXT("/accepted_sections"), TEXT("nonempty array"), StringArrayValue(Operation.AcceptedSections));
			if (!ValidateUniqueStrings(Operation.AcceptedSections, Path + TEXT("/accepted_sections"), TEXT("apply_input_operation_invalid"), OutError)) return false;
			if (!IsOneOf(Operation.OmittedFieldPolicy, {TEXT("ue_native_default"), TEXT("preserve_current"), TEXT("not_applicable")})) return Reject(TEXT("apply_input_operation_invalid"), TEXT("The operation omitted-field policy is unsupported."), Path + TEXT("/omitted_field_policy"), TEXT("ue_native_default, preserve_current, not_applicable"), StringValue(Operation.OmittedFieldPolicy));
			if (!IsOneOf(Operation.NativeDefaultPolicy, {TEXT("required"), TEXT("allowed"), TEXT("forbidden"), TEXT("not_applicable")})) return Reject(TEXT("apply_input_operation_invalid"), TEXT("The operation native-default policy is unsupported."), Path + TEXT("/native_default_policy"), TEXT("required, allowed, forbidden, not_applicable"), StringValue(Operation.NativeDefaultPolicy));
			for (int32 SectionIndex = 0; SectionIndex < Operation.AcceptedSections.Num(); ++SectionIndex)
			{
				const FString& Section = Operation.AcceptedSections[SectionIndex];
				if (!IsOneOf(Section, {TEXT("target"), TEXT("properties"), TEXT("children"), TEXT("provider_extension"), TEXT("ordering")})) return Reject(TEXT("apply_input_operation_section_invalid"), TEXT("The operation input section is unsupported."), Path + FString::Printf(TEXT("/accepted_sections/%d"), SectionIndex), TEXT("target, properties, children, provider_extension, ordering"), StringValue(Section));
			}
			for (int32 GroupIndex = 0; GroupIndex < Operation.IdentityOneOf.Num(); ++GroupIndex)
			{
				const auto& Group = Operation.IdentityOneOf[GroupIndex];
				const FString GroupPath = Path + FString::Printf(TEXT("/identity_one_of/%d"), GroupIndex);
				if (Group.IsEmpty()) return Reject(TEXT("apply_input_identity_set_invalid"), TEXT("The operation identity group is empty."), GroupPath, TEXT("nonempty array"), StringArrayValue(Group));
				if (!ValidateUniqueStrings(Group, GroupPath, TEXT("apply_input_identity_set_invalid"), OutError)) return false;
				for (int32 FieldIndex = 0; FieldIndex < Group.Num(); ++FieldIndex)
					if (!FieldNames.Contains(Group[FieldIndex])) return Reject(TEXT("apply_input_identity_field_unknown"), TEXT("The identity group references an undeclared input field."), GroupPath + TEXT("/") + FString::FromInt(FieldIndex), TEXT("declared input field name"), StringValue(Group[FieldIndex]));
			}
			Operations.Add(Operation.Operation);
		}
		return true;
	}

	bool ParseContract(const TSharedPtr<FJsonObject>& Object, FContract& OutContract, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutContract = FContract();
		OutError.Reset();
		if (!RequireOnlyFields(Object, {
			TEXT("schema"), TEXT("contract_id"), TEXT("contract_version"), TEXT("profile_id"), TEXT("target_kind"),
			TEXT("semantic_descriptor_refs"), TEXT("reflection_baseline"), TEXT("property_payload"), TEXT("input_fields"),
			TEXT("operations"), TEXT("provider_id"), TEXT("unknown_input_field_policy"), TEXT("verification")
		}, TEXT(""), OutError)) return false;
		if (!ReadString(Object, TEXT("schema"), TEXT(""), OutContract.Schema, OutError)
			|| !ReadString(Object, TEXT("contract_id"), TEXT(""), OutContract.ContractId, OutError)
			|| !ReadString(Object, TEXT("contract_version"), TEXT(""), OutContract.ContractVersion, OutError)
			|| !ReadString(Object, TEXT("profile_id"), TEXT(""), OutContract.ProfileId, OutError)
			|| !ReadString(Object, TEXT("target_kind"), TEXT(""), OutContract.TargetKind, OutError)
			|| !ReadStringArray(Object, TEXT("semantic_descriptor_refs"), TEXT(""), OutContract.SemanticDescriptorRefs, OutError)
			|| !ReadString(Object, TEXT("provider_id"), TEXT(""), OutContract.ProviderId, OutError)
			|| !ReadString(Object, TEXT("unknown_input_field_policy"), TEXT(""), OutContract.UnknownInputFieldPolicy, OutError)) return false;
		OutContract.ContractId = NormalizeId(OutContract.ContractId);
		OutContract.ProfileId = NormalizeId(OutContract.ProfileId);
		OutContract.TargetKind = NormalizeId(OutContract.TargetKind);
		OutContract.ProviderId = NormalizeId(OutContract.ProviderId);
		OutContract.UnknownInputFieldPolicy = NormalizeId(OutContract.UnknownInputFieldPolicy);

		const TSharedPtr<FJsonObject>* Reflection = nullptr;
		if (!ReadObject(Object, TEXT("reflection_baseline"), Reflection, OutError)
			|| !RequireOnlyFields(*Reflection, { TEXT("enabled"), TEXT("root_kinds"), TEXT("recursive_nodes"), TEXT("unknown_field_policy"),
				TEXT("external_reference_policy"), TEXT("owned_object_policy") }, TEXT("/reflection_baseline"), OutError)
			|| !ReadBool(*Reflection, TEXT("enabled"), TEXT("/reflection_baseline"), OutContract.ReflectionBaseline.bEnabled, OutError)
			|| !ReadStringArray(*Reflection, TEXT("root_kinds"), TEXT("/reflection_baseline"), OutContract.ReflectionBaseline.RootKinds, OutError)
			|| !ReadStringArray(*Reflection, TEXT("recursive_nodes"), TEXT("/reflection_baseline"), OutContract.ReflectionBaseline.RecursiveNodes, OutError)
			|| !ReadString(*Reflection, TEXT("unknown_field_policy"), TEXT("/reflection_baseline"), OutContract.ReflectionBaseline.UnknownFieldPolicy, OutError)
			|| !ReadString(*Reflection, TEXT("external_reference_policy"), TEXT("/reflection_baseline"), OutContract.ReflectionBaseline.ExternalReferencePolicy, OutError)
			|| !ReadString(*Reflection, TEXT("owned_object_policy"), TEXT("/reflection_baseline"), OutContract.ReflectionBaseline.OwnedObjectPolicy, OutError)) return false;
		OutContract.ReflectionBaseline.UnknownFieldPolicy = NormalizeId(OutContract.ReflectionBaseline.UnknownFieldPolicy);
		OutContract.ReflectionBaseline.ExternalReferencePolicy = NormalizeId(OutContract.ReflectionBaseline.ExternalReferencePolicy);
		OutContract.ReflectionBaseline.OwnedObjectPolicy = NormalizeId(OutContract.ReflectionBaseline.OwnedObjectPolicy);

		const TSharedPtr<FJsonObject>* Payload = nullptr;
		if (!ReadObject(Object, TEXT("property_payload"), Payload, OutError)
			|| !RequireOnlyFields(*Payload, { TEXT("mode"), TEXT("property_path_field"), TEXT("value_field"),
				TEXT("optional_assertion_fields"), TEXT("omitted_patch_field_policy") }, TEXT("/property_payload"), OutError)
			|| !ReadString(*Payload, TEXT("mode"), TEXT("/property_payload"), OutContract.PropertyPayload.Mode, OutError)
			|| !ReadString(*Payload, TEXT("property_path_field"), TEXT("/property_payload"), OutContract.PropertyPayload.PropertyPathField, OutError)
			|| !ReadString(*Payload, TEXT("value_field"), TEXT("/property_payload"), OutContract.PropertyPayload.ValueField, OutError)
			|| !ReadStringArray(*Payload, TEXT("optional_assertion_fields"), TEXT("/property_payload"), OutContract.PropertyPayload.OptionalAssertionFields, OutError, true)
			|| !ReadString(*Payload, TEXT("omitted_patch_field_policy"), TEXT("/property_payload"), OutContract.PropertyPayload.OmittedPatchFieldPolicy, OutError)) return false;
		OutContract.PropertyPayload.Mode = NormalizeId(OutContract.PropertyPayload.Mode);
		OutContract.PropertyPayload.PropertyPathField = NormalizeId(OutContract.PropertyPayload.PropertyPathField);
		OutContract.PropertyPayload.ValueField = NormalizeId(OutContract.PropertyPayload.ValueField);
		OutContract.PropertyPayload.OmittedPatchFieldPolicy = NormalizeId(OutContract.PropertyPayload.OmittedPatchFieldPolicy);

		const TArray<TSharedPtr<FJsonValue>>* InputFields = nullptr;
		if (!Object->TryGetArrayField(TEXT("input_fields"), InputFields) || !InputFields || InputFields->IsEmpty())
		{
			OutError = MakeValueFailure(TEXT("apply_input_contract_missing_input_fields"), TEXT("/input_fields"), StringValue(TEXT("nonempty array")), Object->Values.FindRef(TEXT("input_fields")), TEXT("contract"), TEXT("The contract requires a nonempty input_fields array."));
			return false;
		}
		for (int32 Index = 0; Index < InputFields->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> FieldObject = (*InputFields)[Index].IsValid() && (*InputFields)[Index]->Type == EJson::Object
				? (*InputFields)[Index]->AsObject() : nullptr;
			const FString Path = FString::Printf(TEXT("/input_fields/%d"), Index);
			if (!RequireOnlyFields(FieldObject, { TEXT("name"), TEXT("role"), TEXT("type_id"), TEXT("required_for"), TEXT("semantic_id") }, Path, OutError)) return false;
			FInputFieldDescriptor& Field = OutContract.InputFields.AddDefaulted_GetRef();
			if (!ReadString(FieldObject, TEXT("name"), Path, Field.Name, OutError)
				|| !ReadString(FieldObject, TEXT("role"), Path, Field.Role, OutError)
				|| !ReadString(FieldObject, TEXT("type_id"), Path, Field.TypeId, OutError)) return false;
			FieldObject->TryGetStringField(TEXT("semantic_id"), Field.SemanticId);
			Field.Name = NormalizeId(Field.Name);
			Field.Role = NormalizeId(Field.Role);
			Field.TypeId = NormalizeId(Field.TypeId);
			Field.SemanticId = NormalizeId(Field.SemanticId);
			TArray<FString> RequiredFor;
			if (!ReadStringArray(FieldObject, TEXT("required_for"), Path, RequiredFor, OutError, true)) return false;
			for (int32 RequiredIndex = 0; RequiredIndex < RequiredFor.Num(); ++RequiredIndex)
			{
				const FString& OperationText = RequiredFor[RequiredIndex];
				EOperation Operation;
				if (!ParseOperation(OperationText, Operation))
				{
					OutError = MakeValueFailure(TEXT("apply_input_required_operation_invalid"), Path + FString::Printf(TEXT("/required_for/%d"), RequiredIndex), StringValue(TEXT("create, update, patch, replace, upsert, remove, clear, clone, insert, move, reorder")), StringValue(OperationText), TEXT("contract"), TEXT("The input field names an unsupported required operation."));
					return false;
				}
				Field.RequiredFor.Add(Operation);
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
		if (!Object->TryGetArrayField(TEXT("operations"), Operations) || !Operations || Operations->IsEmpty())
		{
			OutError = MakeValueFailure(TEXT("apply_input_contract_missing_operations"), TEXT("/operations"), StringValue(TEXT("nonempty array")), Object->Values.FindRef(TEXT("operations")), TEXT("contract"), TEXT("The contract requires a nonempty operations array."));
			return false;
		}
		for (int32 Index = 0; Index < Operations->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> OperationObject = (*Operations)[Index].IsValid() && (*Operations)[Index]->Type == EJson::Object
				? (*Operations)[Index]->AsObject() : nullptr;
			const FString Path = FString::Printf(TEXT("/operations/%d"), Index);
			if (!RequireOnlyFields(OperationObject, { TEXT("operation"), TEXT("identity_one_of"), TEXT("accepted_sections"),
				TEXT("omitted_field_policy"), TEXT("native_default_policy"), TEXT("destructive") }, Path, OutError)) return false;
			FOperationDescriptor& Descriptor = OutContract.Operations.AddDefaulted_GetRef();
			FString OperationText;
			if (!ReadString(OperationObject, TEXT("operation"), Path, OperationText, OutError)
				|| !ParseOperation(OperationText, Descriptor.Operation)
				|| !ReadStringArray(OperationObject, TEXT("accepted_sections"), Path, Descriptor.AcceptedSections, OutError)
				|| !ReadString(OperationObject, TEXT("omitted_field_policy"), Path, Descriptor.OmittedFieldPolicy, OutError)
				|| !ReadString(OperationObject, TEXT("native_default_policy"), Path, Descriptor.NativeDefaultPolicy, OutError)
				|| !ReadBool(OperationObject, TEXT("destructive"), Path, Descriptor.bDestructive, OutError))
			{
				if (OutError.IsEmpty()) OutError = MakeValueFailure(TEXT("apply_input_operation_invalid"), Path + TEXT("/operation"), StringValue(TEXT("create, update, patch, replace, upsert, remove, clear, clone, insert, move, reorder")), StringValue(OperationText), TEXT("contract"), TEXT("The contract operation is unsupported."));
				return false;
			}
			Descriptor.OmittedFieldPolicy = NormalizeId(Descriptor.OmittedFieldPolicy);
			Descriptor.NativeDefaultPolicy = NormalizeId(Descriptor.NativeDefaultPolicy);
			const TArray<TSharedPtr<FJsonValue>>* IdentityGroups = nullptr;
			if (!OperationObject->TryGetArrayField(TEXT("identity_one_of"), IdentityGroups) || !IdentityGroups || IdentityGroups->IsEmpty())
			{
				OutError = MakeValueFailure(TEXT("apply_input_identity_one_of_missing"), Path + TEXT("/identity_one_of"), StringValue(TEXT("nonempty array of identity groups")), OperationObject->Values.FindRef(TEXT("identity_one_of")), TEXT("contract"), TEXT("The operation requires at least one identity group."));
				return false;
			}
			for (int32 GroupIndex = 0; GroupIndex < IdentityGroups->Num(); ++GroupIndex)
			{
				const auto& IdentityGroup = (*IdentityGroups)[GroupIndex];
				const FString GroupPath = Path + FString::Printf(TEXT("/identity_one_of/%d"), GroupIndex);
				if (!IdentityGroup.IsValid() || IdentityGroup->Type != EJson::Array)
				{
					OutError = MakeValueFailure(TEXT("apply_input_identity_group_invalid"), GroupPath, StringValue(TEXT("array of field names")), IdentityGroup, TEXT("contract"), TEXT("The identity group must be an array."));
					return false;
				}
				TArray<FString>& Group = Descriptor.IdentityOneOf.AddDefaulted_GetRef();
				for (int32 FieldIndex = 0; FieldIndex < IdentityGroup->AsArray().Num(); ++FieldIndex)
				{
					const auto& IdentityField = IdentityGroup->AsArray()[FieldIndex];
					if (!IdentityField.IsValid() || IdentityField->Type != EJson::String || IdentityField->AsString().IsEmpty())
					{
						OutError = MakeValueFailure(TEXT("apply_input_identity_field_invalid"), GroupPath + TEXT("/") + FString::FromInt(FieldIndex), StringValue(TEXT("nonempty field name")), IdentityField, TEXT("contract"), TEXT("The identity field must be a nonempty string."));
						return false;
					}
					Group.Add(NormalizeId(IdentityField->AsString()));
				}
			}
		}

		const TSharedPtr<FJsonObject>* Verification = nullptr;
		if (!ReadObject(Object, TEXT("verification"), Verification, OutError)
			|| !RequireOnlyFields(*Verification, { TEXT("state_source"), TEXT("compare_source"), TEXT("required") }, TEXT("/verification"), OutError)
			|| !ReadString(*Verification, TEXT("state_source"), TEXT("/verification"), OutContract.Verification.StateSource, OutError)
			|| !ReadString(*Verification, TEXT("compare_source"), TEXT("/verification"), OutContract.Verification.CompareSource, OutError)
			|| !ReadBool(*Verification, TEXT("required"), TEXT("/verification"), OutContract.Verification.bRequired, OutError)) return false;
		OutContract.Verification.StateSource = NormalizeId(OutContract.Verification.StateSource);
		OutContract.Verification.CompareSource = NormalizeId(OutContract.Verification.CompareSource);
		return ValidateContract(OutContract, OutError);
	}

	bool ParseOperationInput(
		const FContract& Contract,
		const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit& Unit,
		FOperationInput& OutInput,
		FParseFailure& OutFailure)
	{
		OutInput = FOperationInput();
		OutFailure = FParseFailure();
		if (!ValidateContract(Contract, OutFailure.Diagnostic)) return false;
		ON_SCOPE_EXIT
		{
			if (!OutFailure.Diagnostic.IsEmpty())
			{
				auto& Source = OutFailure.Diagnostic.Source;
				const FString JsonPath = Source.JsonPath;
				Source = Unit.Provenance.ToDiagnosticSource();
				Source.JsonPath = JsonPath;
				if (const FUeAgentHjsonSourceLocation* Location = Unit.FindSourceLocation(JsonPath))
				{ Source.SourceFile = Location->FilePath; Source.Line = Location->Line; Source.Column = Location->Column; }
			}
		};
		auto PointerField = [](const FString& Field)
		{ return Field.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")); };
		if (Contract.ContractId != Unit.ApplyContractRef)
		{
			SetParseFailure(OutFailure, MakeValueFailure(TEXT("operation_contract_mismatch"), FString(),
				StringValue(Contract.ContractId), StringValue(Unit.ApplyContractRef), TEXT("operation_input"),
				TEXT("The operation contract identifier does not match the workspace unit's contract reference.")));
			return false;
		}
		if (Unit.Action == UeAgentAuthoringWorkspaceRouter::EUnitAction::NoChange)
		{
			SetParseFailure(OutFailure, UE::UAI::Contract::FDiagnostic::Failure(TEXT("operation_no_change_not_preflighted"), TEXT("An unchanged workspace unit cannot be submitted for operation preflight."), TEXT("operation_input"), Unit.CanonicalUnitKey));
			return false;
		}
		TArray<FString> AllowedOperations;
		for (const auto& Descriptor : Contract.Operations) AllowedOperations.Add(OperationToString(Descriptor.Operation));
		const auto RejectOperation = [&](const TCHAR* Code, const TCHAR* Reason, const TSharedPtr<FJsonValue>& Actual) {
			SetParseFailure(OutFailure, MakeValueFailure(Code, TEXT("/operation"),
				StringArrayValue(AllowedOperations), Actual, TEXT("operation_input"), Reason), TEXT("/operation"));
			OutFailure.Diagnostic.CandidateName = Contract.ContractId;
			OutFailure.Diagnostic.ActualValuePresent = Actual.IsValid();
		};
		const TSharedPtr<const FJsonObject> Document = Unit.Document;
		if (!Document.IsValid())
		{
			if (Unit.Action != UeAgentAuthoringWorkspaceRouter::EUnitAction::Remove)
			{
				SetParseFailure(OutFailure, UE::UAI::Contract::FDiagnostic::Failure(TEXT("operation_document_missing"), TEXT("The operation document is missing."), TEXT("operation_input"), Unit.CanonicalUnitKey));
				return false;
			}
			OutInput.RequestedOperation = EOperation::Remove;
			OutInput.ResolvedOperation = EOperation::Remove;
			OutInput.bWorkspaceSynthesizedOperation = true;
			if (!Contract.FindOperation(EOperation::Remove))
			{
				RejectOperation(TEXT("operation_not_allowed"), TEXT("The contract does not allow removal of this workspace unit."), StringValue(TEXT("remove")));
				return false;
			}
			return true;
		}

		static const TSet<FString> RootFields = {
			TEXT("operation"), TEXT("target"), TEXT("properties"), TEXT("children"), TEXT("provider_extension"), TEXT("ordering")
		};
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Document->Values)
		{
			if (!RootFields.Contains(Pair.Key))
			{
				SetParseFailure(OutFailure, UE::UAI::Contract::FDiagnostic::Failure(TEXT("operation_input_unknown_field"), TEXT("The operation input contains an unknown field."), TEXT("operation_input"), Pair.Key), TEXT("/") + PointerField(Pair.Key));
				return false;
			}
		}
		FString OperationText;
		if (!Document->TryGetStringField(TEXT("operation"), OperationText)
			|| !ParseOperation(OperationText, OutInput.RequestedOperation))
		{
			RejectOperation(TEXT("operation_invalid"), TEXT("The operation must be a string naming a supported operation."), Document->TryGetField(TEXT("operation")));
			return false;
		}
		OutInput.ResolvedOperation = OutInput.RequestedOperation;
		const FOperationDescriptor* Operation = Contract.FindOperation(OutInput.RequestedOperation);
		if (!Operation)
		{
			RejectOperation(TEXT("operation_not_allowed"), TEXT("The requested operation is not allowed by this contract."), StringValue(OperationText));
			return false;
		}

		const TSharedPtr<FJsonObject>* Target = nullptr;
		if (Document->HasField(TEXT("target")))
		{
			if (!Document->TryGetObjectField(TEXT("target"), Target) || !Target || !Target->IsValid())
			{
				SetParseFailure(OutFailure, MakeValueFailure(TEXT("operation_target_invalid"), TEXT("/target"), StringValue(TEXT("object")), Document->TryGetField(TEXT("target")), TEXT("operation_input"), TEXT("The target must be an object.")), TEXT("/target"));
				return false;
			}
			OutInput.Target = *Target;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Target)->Values)
			{
				const FInputFieldDescriptor* Field = Contract.FindInputField(Pair.Key);
				if (!Field || Field->Role == TEXT("intent") || Field->Role == TEXT("operation_control") || Field->Role == TEXT("provider_extension"))
				{
					SetParseFailure(OutFailure, UE::UAI::Contract::FDiagnostic::Failure(TEXT("operation_target_unknown_field"), TEXT("The target contains an unknown field."), TEXT("operation_input"), Pair.Key), TEXT("/target/") + PointerField(Pair.Key));
					return false;
				}
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (Document->HasField(TEXT("properties")))
		{
			if (!Document->TryGetArrayField(TEXT("properties"), Properties) || !Properties)
			{
				SetParseFailure(OutFailure, MakeValueFailure(TEXT("operation_properties_invalid"), TEXT("/properties"), StringValue(TEXT("array")), Document->TryGetField(TEXT("properties")), TEXT("operation_input"), TEXT("The properties section must be an array.")), TEXT("/properties"));
				return false;
			}
			OutInput.Properties = *Properties;
		}
		if (const TSharedPtr<FJsonValue> Children = Document->TryGetField(TEXT("children")); Children.IsValid()) OutInput.Children = Children;
		if (const TSharedPtr<FJsonValue> Ordering = Document->TryGetField(TEXT("ordering")); Ordering.IsValid()) OutInput.Ordering = Ordering;
		const TSharedPtr<FJsonObject>* ProviderExtension = nullptr;
		if (Document->HasField(TEXT("provider_extension")))
		{
			if (!Document->TryGetObjectField(TEXT("provider_extension"), ProviderExtension)
				|| !ProviderExtension || !ProviderExtension->IsValid())
			{
				SetParseFailure(OutFailure, MakeValueFailure(TEXT("operation_provider_extension_invalid"), TEXT("/provider_extension"), StringValue(TEXT("object")), Document->TryGetField(TEXT("provider_extension")), TEXT("operation_input"), TEXT("The provider extension must be an object.")), TEXT("/provider_extension"));
				return false;
			}
			OutInput.ProviderExtension = *ProviderExtension;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*ProviderExtension)->Values)
			{
				const FInputFieldDescriptor* Field = Contract.FindInputField(Pair.Key);
				if (!Field || Field->Role != TEXT("provider_extension"))
				{
					SetParseFailure(OutFailure, UE::UAI::Contract::FDiagnostic::Failure(TEXT("operation_provider_extension_unknown_field"), TEXT("The provider extension contains an unknown field."), TEXT("operation_input"), Pair.Key), TEXT("/provider_extension/") + PointerField(Pair.Key));
					return false;
				}
			}
		}

		auto ValidateSection = [&Document, Operation, &OutFailure](const TCHAR* Section)
		{
			if (!Document->HasField(Section) || Operation->AcceptsSection(Section)) return true;
			SetParseFailure(OutFailure, UE::UAI::Contract::FDiagnostic::Failure(TEXT("operation_section_not_allowed"), TEXT("This section is not allowed for the requested operation."), TEXT("operation_input"), Section), TEXT("/") + FString(Section));
			return false;
		};
		if (!ValidateSection(TEXT("target")) || !ValidateSection(TEXT("properties"))
			|| !ValidateSection(TEXT("children")) || !ValidateSection(TEXT("provider_extension"))
			|| !ValidateSection(TEXT("ordering"))) return false;

		bool bIdentitySatisfied = false;
		for (const TArray<FString>& IdentitySet : Operation->IdentityOneOf)
		{
			bool bSetSatisfied = true;
			for (const FString& IdentityField : IdentitySet)
			{
				bSetSatisfied &= HasInputName(Contract, OutInput, IdentityField);
			}
			bIdentitySatisfied |= bSetSatisfied;
		}
		if (!bIdentitySatisfied)
		{
			TArray<TSharedPtr<FJsonValue>> Choices;
			TArray<FString> Provided;
			for (const auto& IdentitySet : Operation->IdentityOneOf)
			{
				Choices.Add(StringArrayValue(IdentitySet));
				for (const auto& Name : IdentitySet)
					if (HasInputName(Contract, OutInput, Name)) Provided.AddUnique(Name);
			}
			SetParseFailure(OutFailure, MakeValueFailure(TEXT("operation_identity_incomplete"), TEXT("/target"),
				MakeShared<FJsonValueArray>(MoveTemp(Choices)), StringArrayValue(Provided), TEXT("operation_input"),
				TEXT("The target must supply every field in at least one allowed identity group; actual lists the identity fields supplied.")), TEXT("/target"));
			OutFailure.Diagnostic.CandidateName = OperationText;
			return false;
		}
		for (const FInputFieldDescriptor& Field : Contract.InputFields)
		{
			if (Field.RequiredFor.Contains(OutInput.RequestedOperation) && !HasInputField(OutInput, Field))
			{
				const FString Path = (Field.Role == TEXT("provider_extension") ? TEXT("/provider_extension/")
					: Field.Role == TEXT("intent") ? TEXT("/") : TEXT("/target/")) + PointerField(Field.Name);
				const TSharedPtr<FJsonValue> Actual = Field.Role == TEXT("provider_extension")
					? (OutInput.ProviderExtension ? OutInput.ProviderExtension->TryGetField(Field.Name) : nullptr)
					: Field.Role == TEXT("intent") ? Document->TryGetField(Field.Name)
					: (OutInput.Target ? OutInput.Target->TryGetField(Field.Name) : nullptr);
				SetParseFailure(OutFailure, MakeValueFailure(TEXT("operation_required_field_missing"), Path,
					StringValue(TEXT("non-null, nonempty required field")), Actual, TEXT("operation_input"),
					TEXT("The operation requires this field to contain a value.")), Path);
				OutFailure.Diagnostic.ActualValuePresent = Actual.IsValid();
				return false;
			}
		}
		return true;
	}
}
