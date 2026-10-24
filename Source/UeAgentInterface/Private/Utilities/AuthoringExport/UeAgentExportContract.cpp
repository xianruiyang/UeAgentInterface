// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringExport/UeAgentExportContract.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Internationalization/Regex.h"
#include "Misc/ScopeExit.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

namespace
{
	using namespace UeAgentExportContract;
	using UeAgentAuthoringDiagnostics::MakeValueFailure;
	TSharedPtr<FJsonValue> StringValue(const FString& Value) { return MakeShared<FJsonValueString>(Value); }
	TSharedPtr<FJsonValue> StringArrayValue(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		for (const auto& Value : Values) Items.Add(StringValue(Value));
		return MakeShared<FJsonValueArray>(MoveTemp(Items));
	}
	bool ReadObject(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Field, const FString& ParentPath,
		const TSharedPtr<FJsonObject>*& OutObject, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!Parent->TryGetObjectField(Field, OutObject) || !OutObject || !OutObject->IsValid())
		{
			OutError = MakeValueFailure(TEXT("export_contract_object_required"), ParentPath + TEXT("/") + Field,
				StringValue(TEXT("object")), Parent->TryGetField(Field), TEXT("export_contract"), TEXT("The export contract section must be an object."));
			OutError.ActualValuePresent = Parent->HasField(Field);
			return false;
		}
		return true;
	}

	FString NormalizeId(FString Value)
	{
		Value.TrimStartAndEndInline();
		Value.ToLowerInline();
		return Value;
	}

	void NormalizeIds(TArray<FString>& Values)
	{
		for (FString& Value : Values) Value = NormalizeId(MoveTemp(Value));
	}

	bool Matches(const FString& Value, const TCHAR* Pattern)
	{
		const FRegexPattern RegexPattern(Pattern);
		FRegexMatcher Matcher(RegexPattern, Value);
		return Matcher.FindNext() && Matcher.GetMatchBeginning() == 0 && Matcher.GetMatchEnding() == Value.Len();
	}

	bool IsOneOf(const FString& Value, std::initializer_list<const TCHAR*> Allowed)
	{
		for (const TCHAR* Candidate : Allowed)
		{
			if (Value == Candidate) return true;
		}
		return false;
	}

	bool ValidateUniqueStrings(const TArray<FString>& Values, const FString& Path, const TCHAR* Code, UE::UAI::Contract::FDiagnostic& Error)
	{
		TSet<FString> Seen;
		for (int32 Index = 0; Index < Values.Num(); ++Index)
		{
			const auto& Value = Values[Index];
			if (Value.IsEmpty() || Seen.Contains(Value))
			{
				Error = MakeValueFailure(Code, Path + TEXT("/") + FString::FromInt(Index), StringValue(TEXT("nonempty unique string")), StringValue(Value),
					TEXT("export_contract"), Value.IsEmpty() ? TEXT("The export contract array entry is empty.") : TEXT("The export contract array entry duplicates an earlier entry."));
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
		for (const TCHAR* ExpectedValue : Expected)
		{
			if (!Actual.Contains(ExpectedValue)) return false;
		}
		return true;
	}

	TSharedPtr<FJsonValue> ContractLeafValue(const FString& Value) { return StringValue(Value); }
	TSharedPtr<FJsonValue> ContractLeafValue(bool Value) { return MakeShared<FJsonValueBoolean>(Value); }
	TSharedPtr<FJsonValue> ContractLeafValue(int32 Value) { return MakeShared<FJsonValueNumber>(Value); }
	TSharedPtr<FJsonValue> ContractLeafValue(EFieldSourceKind Value) { return StringValue(FieldSourceKindToString(Value)); }
	template<typename TValue>
	bool ContractValuesEqual(const TValue& Expected, const TValue& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error)
	{
		if (Expected == Actual) return true;
		if (Error) *Error = MakeValueFailure(TEXT("conflicting_export_contract"), Path, ContractLeafValue(Expected), ContractLeafValue(Actual),
			TEXT("export_contract"), TEXT("The new export contract differs from the contract already registered with this identifier."));
		return false;
	}
	bool ContractValuesEqual(const FProfileDescriptor& Expected, const FProfileDescriptor& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error);
	bool ContractValuesEqual(const FFileDescriptor& Expected, const FFileDescriptor& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error);
	bool ContractValuesEqual(const FFieldSourceDescriptor& Expected, const FFieldSourceDescriptor& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error);
	bool ContractValuesEqual(const FFieldDescriptor& Expected, const FFieldDescriptor& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error);
	bool ContractValuesEqual(const FWriterPolicy& Expected, const FWriterPolicy& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error);
	bool ContractValuesEqual(const FExtensionPolicy& Expected, const FExtensionPolicy& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error);
	bool ContractValuesEqual(const FSortPolicy& Expected, const FSortPolicy& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error);
	bool ContractValuesEqual(const FContract& Expected, const FContract& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error);

	template<typename TValue>
	bool ContractValuesEqual(const TArray<TValue>& Expected, const TArray<TValue>& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error)
	{
		if (Expected.Num() != Actual.Num()) return ContractValuesEqual(Expected.Num(), Actual.Num(), Path + TEXT("/length"), Error);
		for (int32 Index = 0; Index < Expected.Num(); ++Index)
			if (!ContractValuesEqual(Expected[Index], Actual[Index], Path + TEXT("/") + FString::FromInt(Index), Error)) return false;
		return true;
	}

	bool ContractValuesEqual(const FProfileDescriptor& Expected, const FProfileDescriptor& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error)
	{
		return ContractValuesEqual(Expected.ProfileId, Actual.ProfileId, Path + TEXT("/profile_id"), Error)
			&& ContractValuesEqual(Expected.IncludedRoles, Actual.IncludedRoles, Path + TEXT("/included_roles"), Error);
	}

	bool ContractValuesEqual(const FFileDescriptor& Expected, const FFileDescriptor& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error)
	{
		return ContractValuesEqual(Expected.FileId, Actual.FileId, Path + TEXT("/file_id"), Error)
			&& ContractValuesEqual(Expected.Path, Actual.Path, Path + TEXT("/path"), Error)
			&& ContractValuesEqual(Expected.Role, Actual.Role, Path + TEXT("/role"), Error)
			&& ContractValuesEqual(Expected.Profiles, Actual.Profiles, Path + TEXT("/profiles"), Error)
			&& ContractValuesEqual(Expected.bRequired, Actual.bRequired, Path + TEXT("/required"), Error)
			&& ContractValuesEqual(Expected.ProjectionId, Actual.ProjectionId, Path + TEXT("/projection_id"), Error)
			&& ContractValuesEqual(Expected.MergePolicy, Actual.MergePolicy, Path + TEXT("/merge_policy"), Error);
	}

	bool ContractValuesEqual(const FFieldSourceDescriptor& Expected, const FFieldSourceDescriptor& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error)
	{
		return ContractValuesEqual(Expected.Kind, Actual.Kind, Path + TEXT("/kind"), Error)
			&& ContractValuesEqual(Expected.SemanticId, Actual.SemanticId, Path + TEXT("/semantic_id"), Error)
			&& ContractValuesEqual(Expected.ProjectorId, Actual.ProjectorId, Path + TEXT("/projector_id"), Error);
	}

	bool ContractValuesEqual(const FFieldDescriptor& Expected, const FFieldDescriptor& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error)
	{
		return ContractValuesEqual(Expected.FieldId, Actual.FieldId, Path + TEXT("/field_id"), Error)
			&& ContractValuesEqual(Expected.OutputPath, Actual.OutputPath, Path + TEXT("/output_path"), Error)
			&& ContractValuesEqual(Expected.Source, Actual.Source, Path + TEXT("/source"), Error)
			&& ContractValuesEqual(Expected.Role, Actual.Role, Path + TEXT("/role"), Error)
			&& ContractValuesEqual(Expected.bEditable, Actual.bEditable, Path + TEXT("/editable"), Error)
			&& ContractValuesEqual(Expected.bRequired, Actual.bRequired, Path + TEXT("/required"), Error)
			&& ContractValuesEqual(Expected.Representation, Actual.Representation, Path + TEXT("/representation"), Error)
			&& ContractValuesEqual(Expected.EmptyPolicy, Actual.EmptyPolicy, Path + TEXT("/empty_policy"), Error)
			&& ContractValuesEqual(Expected.DefaultPolicy, Actual.DefaultPolicy, Path + TEXT("/default_policy"), Error)
			&& ContractValuesEqual(Expected.Externalization, Actual.Externalization, Path + TEXT("/externalization"), Error);
	}

	bool ContractValuesEqual(const FWriterPolicy& Expected, const FWriterPolicy& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error)
	{
		return ContractValuesEqual(Expected.Format, Actual.Format, Path + TEXT("/format"), Error)
			&& ContractValuesEqual(Expected.InlineStructMaxFields, Actual.InlineStructMaxFields, Path + TEXT("/inline_struct_max_fields"), Error)
			&& ContractValuesEqual(Expected.bListCountsAsOneField, Actual.bListCountsAsOneField, Path + TEXT("/list_counts_as_one_field"), Error)
			&& ContractValuesEqual(Expected.bScalarListInline, Actual.bScalarListInline, Path + TEXT("/scalar_list_inline"), Error)
			&& ContractValuesEqual(Expected.KnownStructComponents, Actual.KnownStructComponents, Path + TEXT("/known_struct_components"), Error)
			&& ContractValuesEqual(Expected.TransformShape, Actual.TransformShape, Path + TEXT("/transform_shape"), Error)
			&& ContractValuesEqual(Expected.PreserveValueTextProfiles, Actual.PreserveValueTextProfiles, Path + TEXT("/preserve_value_text_profiles"), Error)
			&& ContractValuesEqual(Expected.RemoveFields, Actual.RemoveFields, Path + TEXT("/remove_fields"), Error)
			&& ContractValuesEqual(Expected.PropertyPathPolicy, Actual.PropertyPathPolicy, Path + TEXT("/property_path_policy"), Error)
			&& ContractValuesEqual(Expected.UnknownOutputFieldPolicy, Actual.UnknownOutputFieldPolicy, Path + TEXT("/unknown_output_field_policy"), Error);
	}

	bool ContractValuesEqual(const FExtensionPolicy& Expected, const FExtensionPolicy& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error)
	{
		return ContractValuesEqual(Expected.Input, Actual.Input, Path + TEXT("/input"), Error)
			&& ContractValuesEqual(Expected.bDirectUObjectRead, Actual.bDirectUObjectRead, Path + TEXT("/direct_ue_object_read"), Error);
	}

	bool ContractValuesEqual(const FSortPolicy& Expected, const FSortPolicy& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error)
	{
		return ContractValuesEqual(Expected.bDeterministic, Actual.bDeterministic, Path + TEXT("/deterministic"), Error)
			&& ContractValuesEqual(Expected.bIdentityBeforeContent, Actual.bIdentityBeforeContent, Path + TEXT("/identity_before_content"), Error);
	}

	bool ContractValuesEqual(const FContract& Expected, const FContract& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error)
	{
		return ContractValuesEqual(Expected.Schema, Actual.Schema, Path + TEXT("/schema"), Error)
			&& ContractValuesEqual(Expected.ContractId, Actual.ContractId, Path + TEXT("/contract_id"), Error)
			&& ContractValuesEqual(Expected.ContractVersion, Actual.ContractVersion, Path + TEXT("/contract_version"), Error)
			&& ContractValuesEqual(Expected.TypeId, Actual.TypeId, Path + TEXT("/type_id"), Error)
			&& ContractValuesEqual(Expected.SemanticDescriptorRefs, Actual.SemanticDescriptorRefs, Path + TEXT("/semantic_descriptor_refs"), Error)
			&& ContractValuesEqual(Expected.DefaultProfile, Actual.DefaultProfile, Path + TEXT("/default_profile"), Error)
			&& ContractValuesEqual(Expected.Profiles, Actual.Profiles, Path + TEXT("/profiles"), Error)
			&& ContractValuesEqual(Expected.Files, Actual.Files, Path + TEXT("/files"), Error)
			&& ContractValuesEqual(Expected.Fields, Actual.Fields, Path + TEXT("/fields"), Error)
			&& ContractValuesEqual(Expected.WriterPolicy, Actual.WriterPolicy, Path + TEXT("/writer_policy"), Error)
			&& ContractValuesEqual(Expected.ExtensionPolicy, Actual.ExtensionPolicy, Path + TEXT("/extension_policy"), Error)
			&& ContractValuesEqual(Expected.SortPolicy, Actual.SortPolicy, Path + TEXT("/sort_policy"), Error);
	}

	bool RequireOnlyFields(
		const TSharedPtr<FJsonObject>& Object,
		std::initializer_list<const TCHAR*> AllowedFields,
		const FString& Path,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!Object.IsValid())
		{
			OutError = MakeValueFailure(TEXT("export_contract_object_required"), Path, StringValue(TEXT("object")), nullptr,
				TEXT("export_contract"), TEXT("The export contract section must be an object."));
			return false;
		}
		TSet<FString> Allowed;
		for (const TCHAR* Field : AllowedFields) Allowed.Add(Field);
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
		{
			if (!Allowed.Contains(Pair.Key))
			{
				OutError = MakeValueFailure(TEXT("export_contract_unknown_field"), Pair.Key, nullptr, nullptr,
					TEXT("export_contract"), TEXT("The export contract section does not accept this field."));
				UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, Path, true);
				OutError.ActualValuePresent = true;
				OutError.ExpectedValuePresent = false;
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!Object.IsValid() || !Object->TryGetStringField(Field, OutValue) || OutValue.TrimStartAndEnd().IsEmpty())
		{
			OutError = MakeValueFailure(TEXT("export_contract_missing"), Path + TEXT("/") + Field, StringValue(TEXT("nonempty string")),
				Object ? Object->TryGetField(Field) : nullptr, TEXT("export_contract"), TEXT("The export contract field must contain a nonempty string."));
			OutError.ActualValuePresent = Object && Object->HasField(Field);
			return false;
		}
		OutValue.TrimStartAndEndInline();
		return true;
	}

	bool ReadBool(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		const FString& Path,
		bool& OutValue,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!Object.IsValid() || !Object->TryGetBoolField(Field, OutValue))
		{
			OutError = MakeValueFailure(TEXT("export_contract_missing"), Path + TEXT("/") + Field, StringValue(TEXT("boolean")),
				Object ? Object->TryGetField(Field) : nullptr, TEXT("export_contract"), TEXT("The export contract field must contain a boolean."));
			OutError.ActualValuePresent = Object && Object->HasField(Field);
			return false;
		}
		return true;
	}

	bool ReadPositiveInt(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		const FString& Path,
		int32& OutValue,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		double Number = 0.0;
		if (!Object.IsValid() || !Object->TryGetNumberField(Field, Number)
			|| Number < 1.0 || Number > static_cast<double>(MAX_int32)
			|| FMath::Floor(Number) != Number)
		{
			OutError = MakeValueFailure(TEXT("export_contract_invalid"), Path + TEXT("/") + Field, StringValue(TEXT("integer in [1, 2147483647]")),
				Object ? Object->TryGetField(Field) : nullptr, TEXT("export_contract"), TEXT("The export contract field must contain a positive 32-bit integer."));
			OutError.ActualValuePresent = Object && Object->HasField(Field);
			return false;
		}
		OutValue = static_cast<int32>(Number);
		return true;
	}

	bool ReadStringArray(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		const FString& Path,
		TArray<FString>& OutValues,
		UE::UAI::Contract::FDiagnostic& OutError,
		bool bAllowEmpty = false)
	{
		OutValues.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object.IsValid() || !Object->TryGetArrayField(Field, Values) || !Values
			|| (!bAllowEmpty && Values->IsEmpty()))
		{
			OutError = MakeValueFailure(TEXT("export_contract_invalid"), Path + TEXT("/") + Field,
				StringValue(bAllowEmpty ? TEXT("array of strings") : TEXT("nonempty array of strings")),
				Object ? Object->TryGetField(Field) : nullptr, TEXT("export_contract"), TEXT("The export contract field must contain the required string array."));
			OutError.ActualValuePresent = Object && Object->HasField(Field);
			return false;
		}
		for (int32 Index = 0; Index < Values->Num(); ++Index)
		{
			const auto& Value = (*Values)[Index];
			if (!Value.IsValid() || Value->Type != EJson::String || Value->AsString().TrimStartAndEnd().IsEmpty())
			{
				OutError = MakeValueFailure(TEXT("export_contract_invalid"), Path + TEXT("/") + Field + TEXT("/") + FString::FromInt(Index),
					StringValue(TEXT("nonempty string")), Value, TEXT("export_contract"), TEXT("The export contract array entry must contain a nonempty string."));
				return false;
			}
			OutValues.Add(Value->AsString().TrimStartAndEnd());
		}
		return true;
	}

	bool IsSafeExportFilePath(const FString& Path)
	{
		if (Path.IsEmpty() || Path.StartsWith(TEXT("/")) || Path.Contains(TEXT("\\"))
			|| Path.Contains(TEXT("*")) || !Path.EndsWith(TEXT(".hjson"), ESearchCase::IgnoreCase))
		{
			return false;
		}
		TArray<FString> Segments;
		Path.ParseIntoArray(Segments, TEXT("/"), true);
		if (Segments.IsEmpty()) return false;
		for (const FString& Segment : Segments)
		{
			if (Segment.IsEmpty() || Segment == TEXT(".") || Segment == TEXT("..")) return false;
		}
		return true;
	}

}

namespace UeAgentExportContract
{
	bool FProfileDescriptor::IncludesRole(const FString& Role) const
	{
		return IncludedRoles.Contains(NormalizeId(Role));
	}

	bool FProfileDescriptor::operator==(const FProfileDescriptor& Other) const
	{
		return ContractValuesEqual(*this, Other, FString(), nullptr);
	}

	bool FFileDescriptor::IsEnabledForProfile(const FString& ProfileId) const
	{
		return Profiles.Contains(NormalizeId(ProfileId));
	}

	bool FFileDescriptor::operator==(const FFileDescriptor& Other) const
	{
		return ContractValuesEqual(*this, Other, FString(), nullptr);
	}

	bool FFieldSourceDescriptor::operator==(const FFieldSourceDescriptor& Other) const
	{
		return ContractValuesEqual(*this, Other, FString(), nullptr);
	}

	bool FFieldDescriptor::operator==(const FFieldDescriptor& Other) const
	{
		return ContractValuesEqual(*this, Other, FString(), nullptr);
	}

	bool FWriterPolicy::PreservesValueText(const FString& ProfileId) const
	{
		return PreserveValueTextProfiles.Contains(NormalizeId(ProfileId));
	}

	bool FWriterPolicy::operator==(const FWriterPolicy& Other) const
	{
		return ContractValuesEqual(*this, Other, FString(), nullptr);
	}

	bool FExtensionPolicy::operator==(const FExtensionPolicy& Other) const
	{
		return ContractValuesEqual(*this, Other, FString(), nullptr);
	}

	bool FSortPolicy::operator==(const FSortPolicy& Other) const
	{
		return ContractValuesEqual(*this, Other, FString(), nullptr);
	}

	const FProfileDescriptor* FContract::FindProfile(const FString& ProfileId) const
	{
		const FString Canonical = NormalizeId(ProfileId);
		return Profiles.FindByPredicate([&Canonical](const FProfileDescriptor& Profile)
		{
			return Profile.ProfileId == Canonical;
		});
	}

	const FFileDescriptor* FContract::FindFile(const FString& FileId) const
	{
		const FString Canonical = NormalizeId(FileId);
		return Files.FindByPredicate([&Canonical](const FFileDescriptor& File)
		{
			return File.FileId == Canonical;
		});
	}

	const FFileDescriptor* FContract::FindFileByPath(const FString& Path) const
	{
		return Files.FindByPredicate([&Path](const FFileDescriptor& File)
		{
			return File.Path == Path;
		});
	}

	const FFieldDescriptor* FContract::FindField(const FString& FieldId) const
	{
		const FString Canonical = NormalizeId(FieldId);
		return Fields.FindByPredicate([&Canonical](const FFieldDescriptor& Field)
		{
			return Field.FieldId == Canonical;
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

	FString FieldSourceKindToString(EFieldSourceKind Kind)
	{
		switch (Kind)
		{
		case EFieldSourceKind::Semantic: return TEXT("semantic");
		case EFieldSourceKind::FormatConstant: return TEXT("format_constant");
		case EFieldSourceKind::DerivedNavigation: return TEXT("derived_navigation");
		default: return TEXT("semantic");
		}
	}

	bool ParseFieldSourceKind(const FString& Text, EFieldSourceKind& OutKind)
	{
		const FString Canonical = NormalizeId(Text);
		if (Canonical == TEXT("semantic")) OutKind = EFieldSourceKind::Semantic;
		else if (Canonical == TEXT("format_constant")) OutKind = EFieldSourceKind::FormatConstant;
		else if (Canonical == TEXT("derived_navigation")) OutKind = EFieldSourceKind::DerivedNavigation;
		else return false;
		return true;
	}

	bool SplitOutputPath(const FString& OutputPath, FString& OutFilePath, FString& OutJsonPointer)
	{
		OutFilePath.Reset();
		OutJsonPointer.Reset();
		const int32 Separator = OutputPath.Find(TEXT("#"), ESearchCase::CaseSensitive);
		if (Separator == INDEX_NONE || OutputPath.Find(TEXT("#"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Separator + 1) != INDEX_NONE)
		{
			return false;
		}
		OutFilePath = OutputPath.Left(Separator);
		OutJsonPointer = OutputPath.Mid(Separator + 1);
		return IsSafeExportFilePath(OutFilePath)
			&& (OutJsonPointer.IsEmpty() || OutJsonPointer.StartsWith(TEXT("/")));
	}

	bool ValidateContract(const FContract& Contract, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		ON_SCOPE_EXIT { if (!OutError.IsEmpty()) OutError.CandidateName = Contract.ContractId; };
		const auto Reject = [&](const TCHAR* Code, const TCHAR* Reason, const FString& Path, const TSharedPtr<FJsonValue>& Expected, const TSharedPtr<FJsonValue>& Actual) {
			OutError = MakeValueFailure(Code, Path, Expected, Actual, TEXT("export_contract"), Reason);
			return false;
		};
		if (Contract.Schema != TEXT("uai.export_contract.v2")) return Reject(TEXT("export_contract_schema_invalid"), TEXT("The export contract schema is unsupported."), TEXT("/schema"), StringValue(TEXT("uai.export_contract.v2")), StringValue(Contract.Schema));
		if (!Matches(Contract.ContractId, TEXT("^uai\\.export\\.[a-z0-9_.-]+$"))) return Reject(TEXT("export_contract_id_invalid"), TEXT("The export contract identifier has an invalid format."), TEXT("/contract_id"), StringValue(TEXT("uai.export.<lowercase identifier>")), StringValue(Contract.ContractId));
		if (!Matches(Contract.ContractVersion, TEXT("^[0-9]+\\.[0-9]+\\.[0-9]+$"))) return Reject(TEXT("export_contract_version_invalid"), TEXT("The export contract version must contain three numeric components."), TEXT("/contract_version"), StringValue(TEXT("major.minor.patch")), StringValue(Contract.ContractVersion));
		if (!Matches(Contract.TypeId, TEXT("^[a-z][a-z0-9_.-]+$"))) return Reject(TEXT("export_contract_type_id_invalid"), TEXT("The export contract type identifier has an invalid format."), TEXT("/type_id"), StringValue(TEXT("lowercase identifier starting with a letter")), StringValue(Contract.TypeId));
		if (Contract.SemanticDescriptorRefs.IsEmpty()) return Reject(TEXT("export_contract_semantic_refs_invalid"), TEXT("The export contract requires semantic descriptor references."), TEXT("/semantic_descriptor_refs"), StringValue(TEXT("nonempty unique string array")), StringArrayValue(Contract.SemanticDescriptorRefs));
		if (!ValidateUniqueStrings(Contract.SemanticDescriptorRefs, TEXT("/semantic_descriptor_refs"), TEXT("export_contract_semantic_refs_invalid"), OutError)) return false;
		if (Contract.DefaultProfile != TEXT("llm_authoring")) return Reject(TEXT("export_contract_default_profile_invalid"), TEXT("The export contract default profile is unsupported."), TEXT("/default_profile"), StringValue(TEXT("llm_authoring")), StringValue(Contract.DefaultProfile));
		for (const TCHAR* Required : {TEXT("llm_authoring"), TEXT("readback"), TEXT("debug"), TEXT("validation")})
			if (!Contract.FindProfile(Required)) return Reject(TEXT("export_contract_profiles_invalid"), TEXT("The export contract is missing a required profile."), TEXT("/profiles"), StringValue(Required), nullptr);
		if (Contract.Files.IsEmpty()) return Reject(TEXT("export_contract_files_missing"), TEXT("The export contract has no files."), TEXT("/files"), StringValue(TEXT("nonempty array")), MakeShared<FJsonValueNumber>(0));
		if (Contract.Fields.IsEmpty()) return Reject(TEXT("export_contract_fields_missing"), TEXT("The export contract has no fields."), TEXT("/fields"), StringValue(TEXT("nonempty array")), MakeShared<FJsonValueNumber>(0));
		if (Contract.WriterPolicy.Format != TEXT("hjson")) return Reject(TEXT("export_contract_writer_policy_invalid"), TEXT("The export contract field does not match the required policy."), TEXT("/writer_policy/format"), StringValue(TEXT("hjson")), ContractLeafValue(Contract.WriterPolicy.Format));
		if (Contract.WriterPolicy.InlineStructMaxFields != 6) return Reject(TEXT("export_contract_writer_policy_invalid"), TEXT("The export contract field does not match the required policy."), TEXT("/writer_policy/inline_struct_max_fields"), ContractLeafValue(6), ContractLeafValue(Contract.WriterPolicy.InlineStructMaxFields));
		if (Contract.WriterPolicy.bListCountsAsOneField != true) return Reject(TEXT("export_contract_writer_policy_invalid"), TEXT("The export contract field does not match the required policy."), TEXT("/writer_policy/list_counts_as_one_field"), ContractLeafValue(true), ContractLeafValue(Contract.WriterPolicy.bListCountsAsOneField));
		if (Contract.WriterPolicy.bScalarListInline != true) return Reject(TEXT("export_contract_writer_policy_invalid"), TEXT("The export contract field does not match the required policy."), TEXT("/writer_policy/scalar_list_inline"), ContractLeafValue(true), ContractLeafValue(Contract.WriterPolicy.bScalarListInline));
		if (Contract.WriterPolicy.KnownStructComponents != TEXT("array")) return Reject(TEXT("export_contract_writer_policy_invalid"), TEXT("The export contract field does not match the required policy."), TEXT("/writer_policy/known_struct_components"), StringValue(TEXT("array")), ContractLeafValue(Contract.WriterPolicy.KnownStructComponents));
		if (Contract.WriterPolicy.TransformShape != TEXT("named_arrays")) return Reject(TEXT("export_contract_writer_policy_invalid"), TEXT("The export contract field does not match the required policy."), TEXT("/writer_policy/transform_shape"), StringValue(TEXT("named_arrays")), ContractLeafValue(Contract.WriterPolicy.TransformShape));
		if (Contract.WriterPolicy.PropertyPathPolicy != TEXT("only_when_needed_for_edit")) return Reject(TEXT("export_contract_writer_policy_invalid"), TEXT("The export contract field does not match the required policy."), TEXT("/writer_policy/property_path_policy"), StringValue(TEXT("only_when_needed_for_edit")), ContractLeafValue(Contract.WriterPolicy.PropertyPathPolicy));
		if (Contract.WriterPolicy.UnknownOutputFieldPolicy != TEXT("reject")) return Reject(TEXT("export_contract_writer_policy_invalid"), TEXT("The export contract field does not match the required policy."), TEXT("/writer_policy/unknown_output_field_policy"), StringValue(TEXT("reject")), ContractLeafValue(Contract.WriterPolicy.UnknownOutputFieldPolicy));
		if (Contract.ExtensionPolicy.Input != TEXT("shared_state_snapshot_only")) return Reject(TEXT("export_contract_extension_policy_invalid"), TEXT("The export contract field does not match the required policy."), TEXT("/extension_policy/input"), StringValue(TEXT("shared_state_snapshot_only")), ContractLeafValue(Contract.ExtensionPolicy.Input));
		if (Contract.ExtensionPolicy.bDirectUObjectRead != false) return Reject(TEXT("export_contract_extension_policy_invalid"), TEXT("The export contract field does not match the required policy."), TEXT("/extension_policy/direct_ue_object_read"), ContractLeafValue(false), ContractLeafValue(Contract.ExtensionPolicy.bDirectUObjectRead));
		if (Contract.SortPolicy.bDeterministic != true) return Reject(TEXT("export_contract_sort_policy_invalid"), TEXT("The export contract field does not match the required policy."), TEXT("/sort_policy/deterministic"), ContractLeafValue(true), ContractLeafValue(Contract.SortPolicy.bDeterministic));
		if (Contract.SortPolicy.bIdentityBeforeContent != true) return Reject(TEXT("export_contract_sort_policy_invalid"), TEXT("The export contract field does not match the required policy."), TEXT("/sort_policy/identity_before_content"), ContractLeafValue(true), ContractLeafValue(Contract.SortPolicy.bIdentityBeforeContent));
		if (!HasExactSet(Contract.WriterPolicy.PreserveValueTextProfiles, {TEXT("readback"), TEXT("debug")})) return Reject(TEXT("export_contract_writer_policy_invalid"), TEXT("The writer policy field differs from the required set."), TEXT("/writer_policy/preserve_value_text_profiles"), StringArrayValue({TEXT("readback"), TEXT("debug")}), StringArrayValue(Contract.WriterPolicy.PreserveValueTextProfiles));
		if (!HasExactSet(Contract.WriterPolicy.RemoveFields, {TEXT("operations"), TEXT("control_type"), TEXT("category_path")})) return Reject(TEXT("export_contract_writer_policy_invalid"), TEXT("The writer policy field differs from the required set."), TEXT("/writer_policy/remove_fields"), StringArrayValue({TEXT("operations"), TEXT("control_type"), TEXT("category_path")}), StringArrayValue(Contract.WriterPolicy.RemoveFields));
		for (int32 Index = 0; Index < Contract.SemanticDescriptorRefs.Num(); ++Index)
			if (!Matches(Contract.SemanticDescriptorRefs[Index], TEXT("^uai\\.semantic\\.[a-z0-9_.-]+$")))
				return Reject(TEXT("export_contract_semantic_ref_invalid"), TEXT("The semantic descriptor reference has an invalid format."), TEXT("/semantic_descriptor_refs/") + FString::FromInt(Index), StringValue(TEXT("uai.semantic.<lowercase identifier>")), StringValue(Contract.SemanticDescriptorRefs[Index]));
		const auto IsRole = [](const FString& Role) { return IsOneOf(Role, {TEXT("identity"), TEXT("index"), TEXT("authoring"), TEXT("reference"), TEXT("readonly"), TEXT("readback"), TEXT("diagnostics"), TEXT("validation")}); };
		const FString AllowedRoles = TEXT("identity, index, authoring, reference, readonly, readback, diagnostics, validation");
		TSet<FString> ProfileIds;
		for (int32 Index = 0; Index < Contract.Profiles.Num(); ++Index)
		{
			const auto& Profile = Contract.Profiles[Index];
			const FString Path = TEXT("/profiles/") + FString::FromInt(Index);
			if (!IsOneOf(Profile.ProfileId, {TEXT("llm_authoring"), TEXT("readback"), TEXT("debug"), TEXT("validation")})) return Reject(TEXT("export_contract_profile_invalid"), TEXT("The export profile identifier is unsupported."), Path + TEXT("/profile_id"), StringValue(TEXT("llm_authoring, readback, debug, validation")), StringValue(Profile.ProfileId));
			if (ProfileIds.Contains(Profile.ProfileId)) return Reject(TEXT("export_contract_profile_invalid"), TEXT("The export profile identifier duplicates an earlier profile."), Path + TEXT("/profile_id"), StringValue(TEXT("unique profile identifier")), StringValue(Profile.ProfileId));
			if (Profile.IncludedRoles.IsEmpty()) return Reject(TEXT("export_contract_profile_invalid"), TEXT("The export profile includes no roles."), Path + TEXT("/included_roles"), StringValue(TEXT("nonempty role array")), StringArrayValue(Profile.IncludedRoles));
			if (!ValidateUniqueStrings(Profile.IncludedRoles, Path + TEXT("/included_roles"), TEXT("export_contract_profile_invalid"), OutError)) return false;
			for (int32 RoleIndex = 0; RoleIndex < Profile.IncludedRoles.Num(); ++RoleIndex)
				if (!IsRole(Profile.IncludedRoles[RoleIndex])) return Reject(TEXT("export_contract_profile_role_invalid"), TEXT("The profile includes an unsupported role."), Path + TEXT("/included_roles/") + FString::FromInt(RoleIndex), StringValue(AllowedRoles), StringValue(Profile.IncludedRoles[RoleIndex]));
			ProfileIds.Add(Profile.ProfileId);
		}
		TSet<FString> FileIds, FilePaths;
		for (int32 Index = 0; Index < Contract.Files.Num(); ++Index)
		{
			const auto& File = Contract.Files[Index];
			const FString Path = TEXT("/files/") + FString::FromInt(Index);
			if (!Matches(File.FileId, TEXT("^[a-z][a-z0-9_.-]+$"))) return Reject(TEXT("export_contract_file_invalid"), TEXT("The export file identifier has an invalid format."), Path + TEXT("/file_id"), StringValue(TEXT("lowercase identifier starting with a letter")), StringValue(File.FileId));
			if (FileIds.Contains(File.FileId)) return Reject(TEXT("export_contract_file_invalid"), TEXT("The export file identifier duplicates an earlier file."), Path + TEXT("/file_id"), StringValue(TEXT("unique file identifier")), StringValue(File.FileId));
			if (!IsSafeExportFilePath(File.Path)) return Reject(TEXT("export_contract_file_invalid"), TEXT("The export file path must be a relative HJSON path without traversal, backslashes or wildcards."), Path + TEXT("/path"), StringValue(TEXT("relative .hjson path")), StringValue(File.Path));
			if (FilePaths.Contains(File.Path)) return Reject(TEXT("export_contract_file_invalid"), TEXT("The export file path duplicates an earlier file."), Path + TEXT("/path"), StringValue(TEXT("unique output file path")), StringValue(File.Path));
			if (!IsRole(File.Role)) return Reject(TEXT("export_contract_file_invalid"), TEXT("The export file role is unsupported."), Path + TEXT("/role"), StringValue(AllowedRoles), StringValue(File.Role));
			if (File.Profiles.IsEmpty()) return Reject(TEXT("export_contract_file_invalid"), TEXT("The export file is not assigned to any profile."), Path + TEXT("/profiles"), StringValue(TEXT("nonempty profile array")), StringArrayValue(File.Profiles));
			if (!ValidateUniqueStrings(File.Profiles, Path + TEXT("/profiles"), TEXT("export_contract_file_invalid"), OutError)) return false;
			if (File.ProjectionId.IsEmpty()) return Reject(TEXT("export_contract_file_invalid"), TEXT("The export file has no projection identifier."), Path + TEXT("/projection_id"), StringValue(TEXT("nonempty projection identifier")), StringValue(File.ProjectionId));
			if (!IsOneOf(File.MergePolicy, {TEXT("inline"), TEXT("split"), TEXT("conditional")})) return Reject(TEXT("export_contract_file_invalid"), TEXT("The export file merge policy is unsupported."), Path + TEXT("/merge_policy"), StringValue(TEXT("inline, split, conditional")), StringValue(File.MergePolicy));
			for (int32 ProfileIndex = 0; ProfileIndex < File.Profiles.Num(); ++ProfileIndex)
			{
				const auto* Profile = Contract.FindProfile(File.Profiles[ProfileIndex]);
				if (!Profile || !Profile->IncludesRole(File.Role)) return Reject(TEXT("export_contract_file_profile_role_mismatch"), !Profile ? TEXT("The export file references an undeclared profile.") : TEXT("The selected profile does not include the file role."), Path + TEXT("/profiles/") + FString::FromInt(ProfileIndex), Profile ? StringArrayValue(Profile->IncludedRoles) : StringArrayValue(ProfileIds.Array()), StringValue(Profile ? File.Role : File.Profiles[ProfileIndex]));
			}
			FileIds.Add(File.FileId); FilePaths.Add(File.Path);
		}
		TSet<FString> FieldIds, OutputPaths;
		for (int32 Index = 0; Index < Contract.Fields.Num(); ++Index)
		{
			const auto& Field = Contract.Fields[Index];
			const FString Path = TEXT("/fields/") + FString::FromInt(Index);
			FString FilePath, JsonPointer;
			if (!Matches(Field.FieldId, TEXT("^[a-z][a-z0-9_.-]+$"))) return Reject(TEXT("export_contract_field_invalid"), TEXT("The export field identifier has an invalid format."), Path + TEXT("/field_id"), StringValue(TEXT("lowercase identifier starting with a letter")), StringValue(Field.FieldId));
			if (FieldIds.Contains(Field.FieldId)) return Reject(TEXT("export_contract_field_invalid"), TEXT("The export field identifier duplicates an earlier field."), Path + TEXT("/field_id"), StringValue(TEXT("unique field identifier")), StringValue(Field.FieldId));
			if (!SplitOutputPath(Field.OutputPath, FilePath, JsonPointer)) return Reject(TEXT("export_contract_field_invalid"), TEXT("The field output path is not a valid relative HJSON file and JSON pointer."), Path + TEXT("/output_path"), StringValue(TEXT("file.hjson#/json/pointer")), StringValue(Field.OutputPath));
			if (OutputPaths.Contains(Field.OutputPath)) return Reject(TEXT("export_contract_field_invalid"), TEXT("The field output path duplicates an earlier field."), Path + TEXT("/output_path"), StringValue(TEXT("unique output path")), StringValue(Field.OutputPath));
			const auto* File = Contract.FindFileByPath(FilePath);
			if (!File) return Reject(TEXT("export_contract_field_invalid"), TEXT("The field output path refers to an undeclared export file."), Path + TEXT("/output_path"), StringArrayValue(FilePaths.Array()), StringValue(FilePath));
			if (!IsRole(Field.Role)) return Reject(TEXT("export_contract_field_invalid"), TEXT("The export field role is unsupported."), Path + TEXT("/role"), StringValue(AllowedRoles), StringValue(Field.Role));
			if (Field.bEditable && Field.Role != TEXT("authoring")) return Reject(TEXT("export_contract_field_invalid"), TEXT("Only authoring fields may be editable."), Path + TEXT("/role"), StringValue(TEXT("authoring")), StringValue(Field.Role));
			if (Field.bRequired && Field.EmptyPolicy == TEXT("omit")) return Reject(TEXT("export_contract_field_invalid"), TEXT("A required field cannot use the omit empty-value policy."), Path + TEXT("/empty_policy"), StringValue(TEXT("emit_empty, emit_null")), StringValue(Field.EmptyPolicy));
			if (!IsOneOf(Field.Representation, {TEXT("canonical"), TEXT("summary"), TEXT("reference")})) return Reject(TEXT("export_contract_field_invalid"), TEXT("The field representation is unsupported."), Path + TEXT("/representation"), StringValue(TEXT("canonical, summary, reference")), StringValue(Field.Representation));
			if (!IsOneOf(Field.EmptyPolicy, {TEXT("omit"), TEXT("emit_empty"), TEXT("emit_null")})) return Reject(TEXT("export_contract_field_invalid"), TEXT("The field empty-value policy is unsupported."), Path + TEXT("/empty_policy"), StringValue(TEXT("omit, emit_empty, emit_null")), StringValue(Field.EmptyPolicy));
			if (!IsOneOf(Field.DefaultPolicy, {TEXT("omit_if_native_default"), TEXT("emit"), TEXT("not_applicable")})) return Reject(TEXT("export_contract_field_invalid"), TEXT("The field default-value policy is unsupported."), Path + TEXT("/default_policy"), StringValue(TEXT("omit_if_native_default, emit, not_applicable")), StringValue(Field.DefaultPolicy));
			if (!IsOneOf(Field.Externalization, {TEXT("inline"), TEXT("file"), TEXT("conditional")})) return Reject(TEXT("export_contract_field_invalid"), TEXT("The field externalization policy is unsupported."), Path + TEXT("/externalization"), StringValue(TEXT("inline, file, conditional")), StringValue(Field.Externalization));
			if (Field.Source.Kind == EFieldSourceKind::Semantic)
			{
				if (!Matches(Field.Source.SemanticId, TEXT("^[a-z][a-z0-9_.-]+$"))) return Reject(TEXT("export_contract_field_semantic_invalid"), TEXT("The semantic field identifier has an invalid format."), Path + TEXT("/source/semantic_id"), StringValue(TEXT("lowercase semantic identifier")), StringValue(Field.Source.SemanticId));
			}
			else if (!Field.Source.SemanticId.IsEmpty() || Field.Source.ProjectorId.IsEmpty())
				return Reject(TEXT("export_contract_field_source_invalid"), !Field.Source.SemanticId.IsEmpty() ? TEXT("A non-semantic source must not declare a semantic identifier.") : TEXT("A non-semantic source requires a projector identifier."), Path + (!Field.Source.SemanticId.IsEmpty() ? TEXT("/source/semantic_id") : TEXT("/source/projector_id")), StringValue(!Field.Source.SemanticId.IsEmpty() ? TEXT("empty string") : TEXT("nonempty projector identifier")), StringValue(!Field.Source.SemanticId.IsEmpty() ? Field.Source.SemanticId : Field.Source.ProjectorId));
			bool bRoleReachable = false;
			TArray<FString> ReachableRoles;
			for (const auto& ProfileId : File->Profiles)
				if (const auto* Profile = Contract.FindProfile(ProfileId)) {
					bRoleReachable |= Profile->IncludesRole(Field.Role);
					for (const auto& Role : Profile->IncludedRoles) ReachableRoles.AddUnique(Role);
				}
			if (!bRoleReachable) return Reject(TEXT("export_contract_field_role_unreachable"), TEXT("No profile assigned to the output file includes this field role."), Path + TEXT("/role"), StringArrayValue(ReachableRoles), StringValue(Field.Role));
			FieldIds.Add(Field.FieldId); OutputPaths.Add(Field.OutputPath);
		}
		return true;
	}

	bool ParseContract(const TSharedPtr<FJsonObject>& Object, FContract& OutContract, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutContract = FContract();
		OutError.Reset();
		if (!RequireOnlyFields(Object, {
			TEXT("schema"), TEXT("contract_id"), TEXT("contract_version"), TEXT("type_id"),
			TEXT("semantic_descriptor_refs"), TEXT("default_profile"), TEXT("profiles"), TEXT("files"),
			TEXT("fields"), TEXT("writer_policy"), TEXT("extension_policy"), TEXT("sort_policy")
		}, FString(), OutError)) return false;
		if (!ReadString(Object, TEXT("schema"), FString(), OutContract.Schema, OutError)
			|| !ReadString(Object, TEXT("contract_id"), FString(), OutContract.ContractId, OutError)
			|| !ReadString(Object, TEXT("contract_version"), FString(), OutContract.ContractVersion, OutError)
			|| !ReadString(Object, TEXT("type_id"), FString(), OutContract.TypeId, OutError)
			|| !ReadStringArray(Object, TEXT("semantic_descriptor_refs"), FString(), OutContract.SemanticDescriptorRefs, OutError)
			|| !ReadString(Object, TEXT("default_profile"), FString(), OutContract.DefaultProfile, OutError)) return false;
		OutContract.ContractId = NormalizeId(OutContract.ContractId);
		OutContract.TypeId = NormalizeId(OutContract.TypeId);
		OutContract.DefaultProfile = NormalizeId(OutContract.DefaultProfile);
		NormalizeIds(OutContract.SemanticDescriptorRefs);

		const TArray<TSharedPtr<FJsonValue>>* Profiles = nullptr;
		if (!Object->TryGetArrayField(TEXT("profiles"), Profiles) || !Profiles || Profiles->IsEmpty())
		{
			OutError = MakeValueFailure(TEXT("export_contract_profiles_missing"), TEXT("/profiles"), StringValue(TEXT("nonempty array")), Object->TryGetField(TEXT("profiles")), TEXT("export_contract"), TEXT("The export contract requires a nonempty profiles array."));
			OutError.ActualValuePresent = Object->HasField(TEXT("profiles"));
			return false;
		}
		for (int32 Index = 0; Index < Profiles->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> ProfileObject = (*Profiles)[Index].IsValid() && (*Profiles)[Index]->Type == EJson::Object
				? (*Profiles)[Index]->AsObject() : nullptr;
			const FString Path = FString::Printf(TEXT("/profiles/%d"), Index);
			if (!ProfileObject.IsValid()) {
				OutError = MakeValueFailure(TEXT("export_contract_object_required"), Path, StringValue(TEXT("object")), (*Profiles)[Index], TEXT("export_contract"), TEXT("The export contract array entry must be an object."));
				return false;
			}
			if (!RequireOnlyFields(ProfileObject, { TEXT("profile_id"), TEXT("included_roles") }, Path, OutError)) return false;
			FProfileDescriptor& Profile = OutContract.Profiles.AddDefaulted_GetRef();
			if (!ReadString(ProfileObject, TEXT("profile_id"), Path, Profile.ProfileId, OutError)
				|| !ReadStringArray(ProfileObject, TEXT("included_roles"), Path, Profile.IncludedRoles, OutError)) return false;
			Profile.ProfileId = NormalizeId(Profile.ProfileId);
			NormalizeIds(Profile.IncludedRoles);
		}

		const TArray<TSharedPtr<FJsonValue>>* Files = nullptr;
		if (!Object->TryGetArrayField(TEXT("files"), Files) || !Files || Files->IsEmpty())
		{
			OutError = MakeValueFailure(TEXT("export_contract_files_missing"), TEXT("/files"), StringValue(TEXT("nonempty array")), Object->TryGetField(TEXT("files")), TEXT("export_contract"), TEXT("The export contract requires a nonempty files array."));
			OutError.ActualValuePresent = Object->HasField(TEXT("files"));
			return false;
		}
		for (int32 Index = 0; Index < Files->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> FileObject = (*Files)[Index].IsValid() && (*Files)[Index]->Type == EJson::Object
				? (*Files)[Index]->AsObject() : nullptr;
			const FString Path = FString::Printf(TEXT("/files/%d"), Index);
			if (!FileObject.IsValid()) {
				OutError = MakeValueFailure(TEXT("export_contract_object_required"), Path, StringValue(TEXT("object")), (*Files)[Index], TEXT("export_contract"), TEXT("The export contract array entry must be an object."));
				return false;
			}
			if (!RequireOnlyFields(FileObject, { TEXT("file_id"), TEXT("path"), TEXT("role"), TEXT("profiles"),
				TEXT("required"), TEXT("projection_id"), TEXT("merge_policy") }, Path, OutError)) return false;
			FFileDescriptor& File = OutContract.Files.AddDefaulted_GetRef();
			if (!ReadString(FileObject, TEXT("file_id"), Path, File.FileId, OutError)
				|| !ReadString(FileObject, TEXT("path"), Path, File.Path, OutError)
				|| !ReadString(FileObject, TEXT("role"), Path, File.Role, OutError)
				|| !ReadStringArray(FileObject, TEXT("profiles"), Path, File.Profiles, OutError)
				|| !ReadBool(FileObject, TEXT("required"), Path, File.bRequired, OutError)
				|| !ReadString(FileObject, TEXT("projection_id"), Path, File.ProjectionId, OutError)
				|| !ReadString(FileObject, TEXT("merge_policy"), Path, File.MergePolicy, OutError)) return false;
			File.FileId = NormalizeId(File.FileId);
			File.Role = NormalizeId(File.Role);
			File.ProjectionId = NormalizeId(File.ProjectionId);
			File.MergePolicy = NormalizeId(File.MergePolicy);
			NormalizeIds(File.Profiles);
		}

		const TArray<TSharedPtr<FJsonValue>>* Fields = nullptr;
		if (!Object->TryGetArrayField(TEXT("fields"), Fields) || !Fields || Fields->IsEmpty())
		{
			OutError = MakeValueFailure(TEXT("export_contract_fields_missing"), TEXT("/fields"), StringValue(TEXT("nonempty array")), Object->TryGetField(TEXT("fields")), TEXT("export_contract"), TEXT("The export contract requires a nonempty fields array."));
			OutError.ActualValuePresent = Object->HasField(TEXT("fields"));
			return false;
		}
		for (int32 Index = 0; Index < Fields->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> FieldObject = (*Fields)[Index].IsValid() && (*Fields)[Index]->Type == EJson::Object
				? (*Fields)[Index]->AsObject() : nullptr;
			const FString Path = FString::Printf(TEXT("/fields/%d"), Index);
			if (!FieldObject.IsValid()) {
				OutError = MakeValueFailure(TEXT("export_contract_object_required"), Path, StringValue(TEXT("object")), (*Fields)[Index], TEXT("export_contract"), TEXT("The export contract array entry must be an object."));
				return false;
			}
			if (!RequireOnlyFields(FieldObject, { TEXT("field_id"), TEXT("output_path"), TEXT("source"), TEXT("role"),
				TEXT("editable"), TEXT("required"), TEXT("representation"), TEXT("empty_policy"),
				TEXT("default_policy"), TEXT("externalization") }, Path, OutError)) return false;
			FFieldDescriptor& Field = OutContract.Fields.AddDefaulted_GetRef();
			if (!ReadString(FieldObject, TEXT("field_id"), Path, Field.FieldId, OutError)
				|| !ReadString(FieldObject, TEXT("output_path"), Path, Field.OutputPath, OutError)
				|| !ReadString(FieldObject, TEXT("role"), Path, Field.Role, OutError)
				|| !ReadBool(FieldObject, TEXT("editable"), Path, Field.bEditable, OutError)
				|| !ReadBool(FieldObject, TEXT("required"), Path, Field.bRequired, OutError)
				|| !ReadString(FieldObject, TEXT("representation"), Path, Field.Representation, OutError)
				|| !ReadString(FieldObject, TEXT("empty_policy"), Path, Field.EmptyPolicy, OutError)
				|| !ReadString(FieldObject, TEXT("default_policy"), Path, Field.DefaultPolicy, OutError)
				|| !ReadString(FieldObject, TEXT("externalization"), Path, Field.Externalization, OutError)) return false;
			Field.FieldId = NormalizeId(Field.FieldId);
			Field.Role = NormalizeId(Field.Role);
			Field.Representation = NormalizeId(Field.Representation);
			Field.EmptyPolicy = NormalizeId(Field.EmptyPolicy);
			Field.DefaultPolicy = NormalizeId(Field.DefaultPolicy);
			Field.Externalization = NormalizeId(Field.Externalization);

			const TSharedPtr<FJsonObject>* SourceObject = nullptr;
			if (!ReadObject(FieldObject, TEXT("source"), Path, SourceObject, OutError)
				|| !RequireOnlyFields(*SourceObject, { TEXT("kind"), TEXT("semantic_id"), TEXT("projector_id") }, Path + TEXT("/source"), OutError)) return false;
			FString KindText;
			if (!ReadString(*SourceObject, TEXT("kind"), Path + TEXT("/source"), KindText, OutError)
				|| !ParseFieldSourceKind(KindText, Field.Source.Kind))
			{
				if (OutError.IsEmpty()) OutError = MakeValueFailure(TEXT("export_contract_source_kind_invalid"), Path + TEXT("/source/kind"), StringValue(TEXT("semantic, format_constant, derived_navigation")), StringValue(KindText), TEXT("export_contract"), TEXT("The export field source kind is unsupported."));
				return false;
			}
			const auto ReadOptionalIdentifier = [&](const TCHAR* Name, FString& Value) {
				if (!(*SourceObject)->HasField(Name) || (*SourceObject)->TryGetStringField(Name, Value)) return true;
				OutError = MakeValueFailure(TEXT("export_contract_source_identifier_invalid"), Path + TEXT("/source/") + Name,
					StringValue(TEXT("string")), (*SourceObject)->TryGetField(Name), TEXT("export_contract"), TEXT("The optional source identifier must be a string when supplied."));
				return false;
			};
			if (!ReadOptionalIdentifier(TEXT("semantic_id"), Field.Source.SemanticId)
				|| !ReadOptionalIdentifier(TEXT("projector_id"), Field.Source.ProjectorId)) return false;
			Field.Source.SemanticId = NormalizeId(Field.Source.SemanticId);
			Field.Source.ProjectorId = NormalizeId(Field.Source.ProjectorId);
		}

		const TSharedPtr<FJsonObject>* Writer = nullptr;
		if (!ReadObject(Object, TEXT("writer_policy"), FString(), Writer, OutError)
			|| !RequireOnlyFields(*Writer, { TEXT("format"), TEXT("inline_struct_max_fields"), TEXT("list_counts_as_one_field"),
				TEXT("scalar_list_inline"), TEXT("known_struct_components"), TEXT("transform_shape"),
				TEXT("preserve_value_text_profiles"), TEXT("remove_fields"), TEXT("property_path_policy"),
				TEXT("unknown_output_field_policy") }, TEXT("/writer_policy"), OutError)
			|| !ReadString(*Writer, TEXT("format"), TEXT("/writer_policy"), OutContract.WriterPolicy.Format, OutError)
			|| !ReadPositiveInt(*Writer, TEXT("inline_struct_max_fields"), TEXT("/writer_policy"), OutContract.WriterPolicy.InlineStructMaxFields, OutError)
			|| !ReadBool(*Writer, TEXT("list_counts_as_one_field"), TEXT("/writer_policy"), OutContract.WriterPolicy.bListCountsAsOneField, OutError)
			|| !ReadBool(*Writer, TEXT("scalar_list_inline"), TEXT("/writer_policy"), OutContract.WriterPolicy.bScalarListInline, OutError)
			|| !ReadString(*Writer, TEXT("known_struct_components"), TEXT("/writer_policy"), OutContract.WriterPolicy.KnownStructComponents, OutError)
			|| !ReadString(*Writer, TEXT("transform_shape"), TEXT("/writer_policy"), OutContract.WriterPolicy.TransformShape, OutError)
			|| !ReadStringArray(*Writer, TEXT("preserve_value_text_profiles"), TEXT("/writer_policy"), OutContract.WriterPolicy.PreserveValueTextProfiles, OutError)
			|| !ReadStringArray(*Writer, TEXT("remove_fields"), TEXT("/writer_policy"), OutContract.WriterPolicy.RemoveFields, OutError)
			|| !ReadString(*Writer, TEXT("property_path_policy"), TEXT("/writer_policy"), OutContract.WriterPolicy.PropertyPathPolicy, OutError)
			|| !ReadString(*Writer, TEXT("unknown_output_field_policy"), TEXT("/writer_policy"), OutContract.WriterPolicy.UnknownOutputFieldPolicy, OutError)) return false;
		OutContract.WriterPolicy.Format = NormalizeId(OutContract.WriterPolicy.Format);
		OutContract.WriterPolicy.KnownStructComponents = NormalizeId(OutContract.WriterPolicy.KnownStructComponents);
		OutContract.WriterPolicy.TransformShape = NormalizeId(OutContract.WriterPolicy.TransformShape);
		OutContract.WriterPolicy.PropertyPathPolicy = NormalizeId(OutContract.WriterPolicy.PropertyPathPolicy);
		OutContract.WriterPolicy.UnknownOutputFieldPolicy = NormalizeId(OutContract.WriterPolicy.UnknownOutputFieldPolicy);
		NormalizeIds(OutContract.WriterPolicy.PreserveValueTextProfiles);
		NormalizeIds(OutContract.WriterPolicy.RemoveFields);

		const TSharedPtr<FJsonObject>* Extension = nullptr;
		if (!ReadObject(Object, TEXT("extension_policy"), FString(), Extension, OutError)
			|| !RequireOnlyFields(*Extension, { TEXT("input"), TEXT("direct_ue_object_read") }, TEXT("/extension_policy"), OutError)
			|| !ReadString(*Extension, TEXT("input"), TEXT("/extension_policy"), OutContract.ExtensionPolicy.Input, OutError)
			|| !ReadBool(*Extension, TEXT("direct_ue_object_read"), TEXT("/extension_policy"), OutContract.ExtensionPolicy.bDirectUObjectRead, OutError)) return false;
		OutContract.ExtensionPolicy.Input = NormalizeId(OutContract.ExtensionPolicy.Input);

		const TSharedPtr<FJsonObject>* Sort = nullptr;
		if (!ReadObject(Object, TEXT("sort_policy"), FString(), Sort, OutError)
			|| !RequireOnlyFields(*Sort, { TEXT("deterministic"), TEXT("identity_before_content") }, TEXT("/sort_policy"), OutError)
			|| !ReadBool(*Sort, TEXT("deterministic"), TEXT("/sort_policy"), OutContract.SortPolicy.bDeterministic, OutError)
			|| !ReadBool(*Sort, TEXT("identity_before_content"), TEXT("/sort_policy"), OutContract.SortPolicy.bIdentityBeforeContent, OutError)) return false;
		return ValidateContract(OutContract, OutError);
	}
}
