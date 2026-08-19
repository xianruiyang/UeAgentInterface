// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringOperationApply/UeAgentApplyInputContract.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Internationalization/Regex.h"

namespace
{
	using namespace UeAgentApplyInputContract;

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

	bool IsUnique(const TArray<FString>& Values)
	{
		TSet<FString> Seen;
		for (const FString& Value : Values)
		{
			if (Value.IsEmpty() || Seen.Contains(Value)) return false;
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

	bool RequireOnlyFields(
		const TSharedPtr<FJsonObject>& Object,
		std::initializer_list<const TCHAR*> AllowedFields,
		const FString& Path,
		FString& OutError)
	{
		if (!Object.IsValid())
		{
			OutError = Path + TEXT("_object_required");
			return false;
		}
		TSet<FString> Allowed;
		for (const TCHAR* Field : AllowedFields) Allowed.Add(Field);
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
		{
			if (!Allowed.Contains(Pair.Key))
			{
				OutError = Path + TEXT("_unknown_field_") + Pair.Key;
				return false;
			}
		}
		return true;
	}

	bool ReadString(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		const FString& Path,
		FString& OutValue,
		FString& OutError)
	{
		if (!Object.IsValid() || !Object->TryGetStringField(Field, OutValue) || OutValue.IsEmpty())
		{
			OutError = Path + TEXT("_missing_") + Field;
			return false;
		}
		return true;
	}

	bool ReadBool(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		const FString& Path,
		bool& OutValue,
		FString& OutError)
	{
		if (!Object.IsValid() || !Object->TryGetBoolField(Field, OutValue))
		{
			OutError = Path + TEXT("_missing_") + Field;
			return false;
		}
		return true;
	}

	bool ReadStringArray(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		const FString& Path,
		TArray<FString>& OutValues,
		FString& OutError,
		bool bAllowEmpty = false)
	{
		OutValues.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object.IsValid() || !Object->TryGetArrayField(Field, Values) || !Values || (!bAllowEmpty && Values->IsEmpty()))
		{
			OutError = Path + TEXT("_missing_") + Field;
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			if (!Value.IsValid() || Value->Type != EJson::String || Value->AsString().IsEmpty())
			{
				OutError = Path + TEXT("_invalid_") + Field;
				return false;
			}
			OutValues.Add(NormalizeId(Value->AsString()));
		}
		if (!IsUnique(OutValues))
		{
			OutError = Path + TEXT("_duplicate_") + Field;
			return false;
		}
		return true;
	}

	void SetParseFailure(
		FParseFailure& OutFailure,
		const FString& ErrorCode,
		const FString& ErrorDetail,
		const FString& FieldPath = FString())
	{
		OutFailure = FParseFailure();
		OutFailure.ErrorCode = ErrorCode;
		OutFailure.ErrorDetail = ErrorDetail.IsEmpty() ? ErrorCode : ErrorDetail;
		OutFailure.FieldPath = FieldPath;
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
		return Name == Other.Name && Role == Other.Role && TypeId == Other.TypeId
			&& RequiredFor == Other.RequiredFor && SemanticId == Other.SemanticId;
	}

	bool FOperationDescriptor::AcceptsSection(const FString& Section) const
	{
		return AcceptedSections.Contains(NormalizeId(Section));
	}

	bool FOperationDescriptor::operator==(const FOperationDescriptor& Other) const
	{
		return Operation == Other.Operation && IdentityOneOf == Other.IdentityOneOf
			&& AcceptedSections == Other.AcceptedSections
			&& OmittedFieldPolicy == Other.OmittedFieldPolicy
			&& NativeDefaultPolicy == Other.NativeDefaultPolicy
			&& bDestructive == Other.bDestructive;
	}

	bool FReflectionBaseline::operator==(const FReflectionBaseline& Other) const
	{
		return bEnabled == Other.bEnabled && RootKinds == Other.RootKinds
			&& RecursiveNodes == Other.RecursiveNodes && UnknownFieldPolicy == Other.UnknownFieldPolicy
			&& ExternalReferencePolicy == Other.ExternalReferencePolicy
			&& OwnedObjectPolicy == Other.OwnedObjectPolicy
			&& RuntimeLimitsSource == Other.RuntimeLimitsSource;
	}

	bool FPropertyPayload::operator==(const FPropertyPayload& Other) const
	{
		return Mode == Other.Mode && PropertyPathField == Other.PropertyPathField
			&& ValueField == Other.ValueField && OptionalAssertionFields == Other.OptionalAssertionFields
			&& OmittedPatchFieldPolicy == Other.OmittedPatchFieldPolicy;
	}

	bool FVerificationPolicy::operator==(const FVerificationPolicy& Other) const
	{
		return StateSource == Other.StateSource && CompareSource == Other.CompareSource
			&& bRequired == Other.bRequired;
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
		FString Error;
		return ValidateContract(*this, Error);
	}

	bool FContract::operator==(const FContract& Other) const
	{
		return Schema == Other.Schema && ContractId == Other.ContractId
			&& ContractVersion == Other.ContractVersion && ProfileId == Other.ProfileId
			&& TargetKind == Other.TargetKind && SemanticDescriptorRefs == Other.SemanticDescriptorRefs
			&& ReflectionBaseline == Other.ReflectionBaseline && PropertyPayload == Other.PropertyPayload
			&& InputFields == Other.InputFields && Operations == Other.Operations
			&& ProviderId == Other.ProviderId && UnknownInputFieldPolicy == Other.UnknownInputFieldPolicy
			&& Verification == Other.Verification;
	}

	FRegistry& FRegistry::Get()
	{
		static FRegistry Registry;
		return Registry;
	}

	bool FRegistry::RegisterContract(
		const FContract& Contract,
		ERegistrationResult& OutResult,
		FString& OutError)
	{
		if (!ValidateContract(Contract, OutError)) return false;
		FWriteScopeLock Lock(RegistryLock);
		if (const FContract* Existing = Contracts.Find(Contract.ContractId))
		{
			if (*Existing != Contract)
			{
				OutError = TEXT("conflicting_apply_input_contract_") + Contract.ContractId;
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

	bool ValidateContract(const FContract& Contract, FString& OutError)
	{
		if (Contract.Schema != TEXT("uai.apply_input_contract.v1")) OutError = TEXT("apply_input_contract_schema_invalid");
		else if (!Matches(Contract.ContractId, TEXT("^uai\\.apply_input\\.[a-z0-9_.-]+$"))) OutError = TEXT("apply_input_contract_id_invalid");
		else if (!Matches(Contract.ContractVersion, TEXT("^[0-9]+\\.[0-9]+\\.[0-9]+$"))) OutError = TEXT("apply_input_contract_version_invalid");
		else if (!Matches(Contract.ProfileId, TEXT("^[a-z][a-z0-9_.-]+$"))) OutError = TEXT("apply_input_profile_id_invalid");
		else if (Contract.TargetKind.IsEmpty()) OutError = TEXT("apply_input_target_kind_missing");
		else if (Contract.SemanticDescriptorRefs.IsEmpty() || !IsUnique(Contract.SemanticDescriptorRefs)) OutError = TEXT("apply_input_semantic_refs_invalid");
		else if (!Contract.ReflectionBaseline.bEnabled
			|| !HasExactSet(Contract.ReflectionBaseline.RootKinds, { TEXT("uobject"), TEXT("ustruct") })
			|| !HasExactSet(Contract.ReflectionBaseline.RecursiveNodes, {
				TEXT("scalar"), TEXT("struct"), TEXT("owned_object"), TEXT("reference"), TEXT("array"),
				TEXT("set"), TEXT("map"), TEXT("fixed_array"), TEXT("optional") })
			|| Contract.ReflectionBaseline.UnknownFieldPolicy != TEXT("reject")
			|| Contract.ReflectionBaseline.ExternalReferencePolicy != TEXT("replace_reference_only")
			|| Contract.ReflectionBaseline.OwnedObjectPolicy != TEXT("create_or_locate_with_real_outer_then_recurse")
			|| Contract.ReflectionBaseline.RuntimeLimitsSource != TEXT("shared_runtime_limits")) OutError = TEXT("apply_input_reflection_baseline_invalid");
		else if (Contract.PropertyPayload.Mode != TEXT("reflection")
			|| Contract.PropertyPayload.PropertyPathField != TEXT("property_path")
			|| Contract.PropertyPayload.ValueField != TEXT("value")
			|| Contract.PropertyPayload.OmittedPatchFieldPolicy != TEXT("preserve_current")
			|| !IsUnique(Contract.PropertyPayload.OptionalAssertionFields)) OutError = TEXT("apply_input_property_payload_invalid");
		else if (Contract.InputFields.IsEmpty()) OutError = TEXT("apply_input_fields_missing");
		else if (Contract.Operations.IsEmpty()) OutError = TEXT("apply_input_operations_missing");
		else if (Contract.ProviderId.IsEmpty()) OutError = TEXT("apply_input_provider_missing");
		else if (Contract.UnknownInputFieldPolicy != TEXT("reject")) OutError = TEXT("apply_input_unknown_field_policy_invalid");
		else if (Contract.Verification.StateSource != TEXT("shared_state_snapshot")
			|| Contract.Verification.CompareSource != TEXT("shared_semantic_compare")
			|| !Contract.Verification.bRequired) OutError = TEXT("apply_input_verification_policy_invalid");
		else
		{
			static const TSet<FString> AssertionFields = {
				TEXT("value_kind"), TEXT("struct_path"), TEXT("enum_path"), TEXT("element_type"),
				TEXT("key_type"), TEXT("mapped_type"), TEXT("inner_type"), TEXT("fixed_length"), TEXT("allowed_class")
			};
			for (const FString& Field : Contract.PropertyPayload.OptionalAssertionFields)
			{
				if (!AssertionFields.Contains(Field))
				{
					OutError = TEXT("apply_input_optional_assertion_invalid_") + Field;
					return false;
				}
			}
			TSet<FString> FieldNames;
			for (const FInputFieldDescriptor& Field : Contract.InputFields)
			{
				if (!Matches(Field.Name, TEXT("^[a-z][a-z0-9_.-]+$")) || Field.TypeId.IsEmpty()
					|| !IsOneOf(Field.Role, { TEXT("identity"), TEXT("selector"), TEXT("intent"), TEXT("operation_control"),
						TEXT("context_override"), TEXT("resolution_hint"), TEXT("type_assertion"), TEXT("provider_extension") })
					|| FieldNames.Contains(Field.Name))
				{
					OutError = TEXT("apply_input_field_invalid_") + Field.Name;
					return false;
				}
				FieldNames.Add(Field.Name);
			}
			TSet<EOperation> Operations;
			for (const FOperationDescriptor& Operation : Contract.Operations)
			{
				if (Operations.Contains(Operation.Operation) || Operation.IdentityOneOf.IsEmpty()
					|| Operation.AcceptedSections.IsEmpty() || !IsUnique(Operation.AcceptedSections)
					|| !IsOneOf(Operation.OmittedFieldPolicy, { TEXT("ue_native_default"), TEXT("preserve_current"), TEXT("not_applicable") })
					|| !IsOneOf(Operation.NativeDefaultPolicy, { TEXT("required"), TEXT("allowed"), TEXT("forbidden"), TEXT("not_applicable") }))
				{
					OutError = TEXT("apply_input_operation_invalid_") + OperationToString(Operation.Operation);
					return false;
				}
				for (const FString& Section : Operation.AcceptedSections)
				{
					if (!IsOneOf(Section, { TEXT("target"), TEXT("properties"), TEXT("children"), TEXT("provider_extension"), TEXT("ordering") }))
					{
						OutError = TEXT("apply_input_operation_section_invalid_") + Section;
						return false;
					}
				}
				for (const TArray<FString>& IdentitySet : Operation.IdentityOneOf)
				{
					if (IdentitySet.IsEmpty() || !IsUnique(IdentitySet))
					{
						OutError = TEXT("apply_input_identity_set_invalid_") + OperationToString(Operation.Operation);
						return false;
					}
					for (const FString& IdentityField : IdentitySet)
					{
						if (!FieldNames.Contains(IdentityField))
						{
							OutError = TEXT("apply_input_identity_field_unknown_") + IdentityField;
							return false;
						}
					}
				}
				Operations.Add(Operation.Operation);
			}
			OutError.Reset();
			return true;
		}
		return false;
	}

	bool ParseContract(const TSharedPtr<FJsonObject>& Object, FContract& OutContract, FString& OutError)
	{
		OutContract = FContract();
		OutError.Reset();
		if (!RequireOnlyFields(Object, {
			TEXT("schema"), TEXT("contract_id"), TEXT("contract_version"), TEXT("profile_id"), TEXT("target_kind"),
			TEXT("semantic_descriptor_refs"), TEXT("reflection_baseline"), TEXT("property_payload"), TEXT("input_fields"),
			TEXT("operations"), TEXT("provider_id"), TEXT("unknown_input_field_policy"), TEXT("verification")
		}, TEXT("apply_input_contract"), OutError)) return false;
		if (!ReadString(Object, TEXT("schema"), TEXT("apply_input_contract"), OutContract.Schema, OutError)
			|| !ReadString(Object, TEXT("contract_id"), TEXT("apply_input_contract"), OutContract.ContractId, OutError)
			|| !ReadString(Object, TEXT("contract_version"), TEXT("apply_input_contract"), OutContract.ContractVersion, OutError)
			|| !ReadString(Object, TEXT("profile_id"), TEXT("apply_input_contract"), OutContract.ProfileId, OutError)
			|| !ReadString(Object, TEXT("target_kind"), TEXT("apply_input_contract"), OutContract.TargetKind, OutError)
			|| !ReadStringArray(Object, TEXT("semantic_descriptor_refs"), TEXT("apply_input_contract"), OutContract.SemanticDescriptorRefs, OutError)
			|| !ReadString(Object, TEXT("provider_id"), TEXT("apply_input_contract"), OutContract.ProviderId, OutError)
			|| !ReadString(Object, TEXT("unknown_input_field_policy"), TEXT("apply_input_contract"), OutContract.UnknownInputFieldPolicy, OutError)) return false;
		OutContract.ContractId = NormalizeId(OutContract.ContractId);
		OutContract.ProfileId = NormalizeId(OutContract.ProfileId);
		OutContract.TargetKind = NormalizeId(OutContract.TargetKind);
		OutContract.ProviderId = NormalizeId(OutContract.ProviderId);
		OutContract.UnknownInputFieldPolicy = NormalizeId(OutContract.UnknownInputFieldPolicy);

		const TSharedPtr<FJsonObject>* Reflection = nullptr;
		if (!Object->TryGetObjectField(TEXT("reflection_baseline"), Reflection) || !Reflection || !Reflection->IsValid()
			|| !RequireOnlyFields(*Reflection, { TEXT("enabled"), TEXT("root_kinds"), TEXT("recursive_nodes"), TEXT("unknown_field_policy"),
				TEXT("external_reference_policy"), TEXT("owned_object_policy"), TEXT("runtime_limits_source") }, TEXT("reflection_baseline"), OutError)
			|| !ReadBool(*Reflection, TEXT("enabled"), TEXT("reflection_baseline"), OutContract.ReflectionBaseline.bEnabled, OutError)
			|| !ReadStringArray(*Reflection, TEXT("root_kinds"), TEXT("reflection_baseline"), OutContract.ReflectionBaseline.RootKinds, OutError)
			|| !ReadStringArray(*Reflection, TEXT("recursive_nodes"), TEXT("reflection_baseline"), OutContract.ReflectionBaseline.RecursiveNodes, OutError)
			|| !ReadString(*Reflection, TEXT("unknown_field_policy"), TEXT("reflection_baseline"), OutContract.ReflectionBaseline.UnknownFieldPolicy, OutError)
			|| !ReadString(*Reflection, TEXT("external_reference_policy"), TEXT("reflection_baseline"), OutContract.ReflectionBaseline.ExternalReferencePolicy, OutError)
			|| !ReadString(*Reflection, TEXT("owned_object_policy"), TEXT("reflection_baseline"), OutContract.ReflectionBaseline.OwnedObjectPolicy, OutError)
			|| !ReadString(*Reflection, TEXT("runtime_limits_source"), TEXT("reflection_baseline"), OutContract.ReflectionBaseline.RuntimeLimitsSource, OutError)) return false;
		OutContract.ReflectionBaseline.UnknownFieldPolicy = NormalizeId(OutContract.ReflectionBaseline.UnknownFieldPolicy);
		OutContract.ReflectionBaseline.ExternalReferencePolicy = NormalizeId(OutContract.ReflectionBaseline.ExternalReferencePolicy);
		OutContract.ReflectionBaseline.OwnedObjectPolicy = NormalizeId(OutContract.ReflectionBaseline.OwnedObjectPolicy);
		OutContract.ReflectionBaseline.RuntimeLimitsSource = NormalizeId(OutContract.ReflectionBaseline.RuntimeLimitsSource);

		const TSharedPtr<FJsonObject>* Payload = nullptr;
		if (!Object->TryGetObjectField(TEXT("property_payload"), Payload) || !Payload || !Payload->IsValid()
			|| !RequireOnlyFields(*Payload, { TEXT("mode"), TEXT("property_path_field"), TEXT("value_field"),
				TEXT("optional_assertion_fields"), TEXT("omitted_patch_field_policy") }, TEXT("property_payload"), OutError)
			|| !ReadString(*Payload, TEXT("mode"), TEXT("property_payload"), OutContract.PropertyPayload.Mode, OutError)
			|| !ReadString(*Payload, TEXT("property_path_field"), TEXT("property_payload"), OutContract.PropertyPayload.PropertyPathField, OutError)
			|| !ReadString(*Payload, TEXT("value_field"), TEXT("property_payload"), OutContract.PropertyPayload.ValueField, OutError)
			|| !ReadStringArray(*Payload, TEXT("optional_assertion_fields"), TEXT("property_payload"), OutContract.PropertyPayload.OptionalAssertionFields, OutError, true)
			|| !ReadString(*Payload, TEXT("omitted_patch_field_policy"), TEXT("property_payload"), OutContract.PropertyPayload.OmittedPatchFieldPolicy, OutError)) return false;
		OutContract.PropertyPayload.Mode = NormalizeId(OutContract.PropertyPayload.Mode);
		OutContract.PropertyPayload.PropertyPathField = NormalizeId(OutContract.PropertyPayload.PropertyPathField);
		OutContract.PropertyPayload.ValueField = NormalizeId(OutContract.PropertyPayload.ValueField);
		OutContract.PropertyPayload.OmittedPatchFieldPolicy = NormalizeId(OutContract.PropertyPayload.OmittedPatchFieldPolicy);

		const TArray<TSharedPtr<FJsonValue>>* InputFields = nullptr;
		if (!Object->TryGetArrayField(TEXT("input_fields"), InputFields) || !InputFields || InputFields->IsEmpty())
		{
			OutError = TEXT("apply_input_contract_missing_input_fields");
			return false;
		}
		for (int32 Index = 0; Index < InputFields->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> FieldObject = (*InputFields)[Index].IsValid() && (*InputFields)[Index]->Type == EJson::Object
				? (*InputFields)[Index]->AsObject() : nullptr;
			const FString Path = FString::Printf(TEXT("input_fields_%d"), Index);
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
			for (const FString& OperationText : RequiredFor)
			{
				EOperation Operation;
				if (!ParseOperation(OperationText, Operation))
				{
					OutError = Path + TEXT("_required_operation_invalid_") + OperationText;
					return false;
				}
				Field.RequiredFor.Add(Operation);
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
		if (!Object->TryGetArrayField(TEXT("operations"), Operations) || !Operations || Operations->IsEmpty())
		{
			OutError = TEXT("apply_input_contract_missing_operations");
			return false;
		}
		for (int32 Index = 0; Index < Operations->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> OperationObject = (*Operations)[Index].IsValid() && (*Operations)[Index]->Type == EJson::Object
				? (*Operations)[Index]->AsObject() : nullptr;
			const FString Path = FString::Printf(TEXT("operations_%d"), Index);
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
				if (OutError.IsEmpty()) OutError = Path + TEXT("_operation_invalid");
				return false;
			}
			Descriptor.OmittedFieldPolicy = NormalizeId(Descriptor.OmittedFieldPolicy);
			Descriptor.NativeDefaultPolicy = NormalizeId(Descriptor.NativeDefaultPolicy);
			const TArray<TSharedPtr<FJsonValue>>* IdentityGroups = nullptr;
			if (!OperationObject->TryGetArrayField(TEXT("identity_one_of"), IdentityGroups) || !IdentityGroups || IdentityGroups->IsEmpty())
			{
				OutError = Path + TEXT("_identity_one_of_missing");
				return false;
			}
			for (const TSharedPtr<FJsonValue>& IdentityGroup : *IdentityGroups)
			{
				if (!IdentityGroup.IsValid() || IdentityGroup->Type != EJson::Array)
				{
					OutError = Path + TEXT("_identity_group_invalid");
					return false;
				}
				TArray<FString>& Group = Descriptor.IdentityOneOf.AddDefaulted_GetRef();
				for (const TSharedPtr<FJsonValue>& IdentityField : IdentityGroup->AsArray())
				{
					if (!IdentityField.IsValid() || IdentityField->Type != EJson::String || IdentityField->AsString().IsEmpty())
					{
						OutError = Path + TEXT("_identity_field_invalid");
						return false;
					}
					Group.Add(NormalizeId(IdentityField->AsString()));
				}
			}
		}

		const TSharedPtr<FJsonObject>* Verification = nullptr;
		if (!Object->TryGetObjectField(TEXT("verification"), Verification) || !Verification || !Verification->IsValid()
			|| !RequireOnlyFields(*Verification, { TEXT("state_source"), TEXT("compare_source"), TEXT("required") }, TEXT("verification"), OutError)
			|| !ReadString(*Verification, TEXT("state_source"), TEXT("verification"), OutContract.Verification.StateSource, OutError)
			|| !ReadString(*Verification, TEXT("compare_source"), TEXT("verification"), OutContract.Verification.CompareSource, OutError)
			|| !ReadBool(*Verification, TEXT("required"), TEXT("verification"), OutContract.Verification.bRequired, OutError)) return false;
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
		if (!Contract.IsValid() || Contract.ContractId != Unit.ApplyContractRef)
		{
			SetParseFailure(OutFailure, TEXT("operation_contract_mismatch"), Unit.ApplyContractRef);
			return false;
		}
		if (Unit.Action == UeAgentAuthoringWorkspaceRouter::EUnitAction::NoChange)
		{
			SetParseFailure(OutFailure, TEXT("operation_no_change_not_preflighted"), Unit.CanonicalUnitKey);
			return false;
		}
		const TSharedPtr<const FJsonObject> Document = Unit.Document;
		if (!Document.IsValid())
		{
			if (Unit.Action != UeAgentAuthoringWorkspaceRouter::EUnitAction::Remove)
			{
				SetParseFailure(OutFailure, TEXT("operation_document_missing"), Unit.CanonicalUnitKey);
				return false;
			}
			OutInput.RequestedOperation = EOperation::Remove;
			OutInput.ResolvedOperation = EOperation::Remove;
			OutInput.bWorkspaceSynthesizedOperation = true;
			if (!Contract.FindOperation(EOperation::Remove))
			{
				SetParseFailure(OutFailure, TEXT("operation_not_allowed"), TEXT("remove"), TEXT("operation"));
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
				SetParseFailure(OutFailure, TEXT("operation_input_unknown_field"), Pair.Key, Pair.Key);
				return false;
			}
		}
		FString OperationText;
		if (!Document->TryGetStringField(TEXT("operation"), OperationText)
			|| !ParseOperation(OperationText, OutInput.RequestedOperation))
		{
			SetParseFailure(OutFailure, TEXT("operation_invalid"), OperationText, TEXT("operation"));
			return false;
		}
		OutInput.ResolvedOperation = OutInput.RequestedOperation;
		const FOperationDescriptor* Operation = Contract.FindOperation(OutInput.RequestedOperation);
		if (!Operation)
		{
			SetParseFailure(OutFailure, TEXT("operation_not_allowed"), OperationText, TEXT("operation"));
			return false;
		}

		const TSharedPtr<FJsonObject>* Target = nullptr;
		if (Document->HasField(TEXT("target")))
		{
			if (!Document->TryGetObjectField(TEXT("target"), Target) || !Target || !Target->IsValid())
			{
				SetParseFailure(OutFailure, TEXT("operation_target_invalid"), TEXT("object_required"), TEXT("target"));
				return false;
			}
			OutInput.Target = *Target;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Target)->Values)
			{
				const FInputFieldDescriptor* Field = Contract.FindInputField(Pair.Key);
				if (!Field || Field->Role == TEXT("intent") || Field->Role == TEXT("operation_control") || Field->Role == TEXT("provider_extension"))
				{
					SetParseFailure(OutFailure, TEXT("operation_target_unknown_field"), Pair.Key, TEXT("target.") + Pair.Key);
					return false;
				}
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (Document->HasField(TEXT("properties")))
		{
			if (!Document->TryGetArrayField(TEXT("properties"), Properties) || !Properties)
			{
				SetParseFailure(OutFailure, TEXT("operation_properties_invalid"), TEXT("array_required"), TEXT("properties"));
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
				SetParseFailure(OutFailure, TEXT("operation_provider_extension_invalid"), TEXT("object_required"), TEXT("provider_extension"));
				return false;
			}
			OutInput.ProviderExtension = *ProviderExtension;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*ProviderExtension)->Values)
			{
				const FInputFieldDescriptor* Field = Contract.FindInputField(Pair.Key);
				if (!Field || Field->Role != TEXT("provider_extension"))
				{
					SetParseFailure(OutFailure, TEXT("operation_provider_extension_unknown_field"), Pair.Key, TEXT("provider_extension.") + Pair.Key);
					return false;
				}
			}
		}

		auto ValidateSection = [&Document, Operation, &OutFailure](const TCHAR* Section)
		{
			if (!Document->HasField(Section) || Operation->AcceptsSection(Section)) return true;
			SetParseFailure(OutFailure, TEXT("operation_section_not_allowed"), Section, Section);
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
			SetParseFailure(OutFailure, TEXT("operation_identity_incomplete"), OperationText, TEXT("target"));
			return false;
		}
		for (const FInputFieldDescriptor& Field : Contract.InputFields)
		{
			if (Field.RequiredFor.Contains(OutInput.RequestedOperation) && !HasInputField(OutInput, Field))
			{
				SetParseFailure(OutFailure, TEXT("operation_required_field_missing"), Field.Name, Field.Name);
				return false;
			}
		}
		return true;
	}
}
