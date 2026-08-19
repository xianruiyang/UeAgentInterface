// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringExport/UeAgentExportContract.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Internationalization/Regex.h"

namespace
{
	using namespace UeAgentExportContract;

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
		for (const TCHAR* ExpectedValue : Expected)
		{
			if (!Actual.Contains(ExpectedValue)) return false;
		}
		return true;
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
		if (!Object.IsValid() || !Object->TryGetStringField(Field, OutValue) || OutValue.TrimStartAndEnd().IsEmpty())
		{
			OutError = Path + TEXT("_missing_") + Field;
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
		FString& OutError)
	{
		if (!Object.IsValid() || !Object->TryGetBoolField(Field, OutValue))
		{
			OutError = Path + TEXT("_missing_") + Field;
			return false;
		}
		return true;
	}

	bool ReadPositiveInt(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		const FString& Path,
		int32& OutValue,
		FString& OutError)
	{
		double Number = 0.0;
		if (!Object.IsValid() || !Object->TryGetNumberField(Field, Number)
			|| Number < 1.0 || Number > static_cast<double>(MAX_int32)
			|| FMath::Floor(Number) != Number)
		{
			OutError = Path + TEXT("_invalid_") + Field;
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
		FString& OutError,
		bool bAllowEmpty = false)
	{
		OutValues.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object.IsValid() || !Object->TryGetArrayField(Field, Values) || !Values
			|| (!bAllowEmpty && Values->IsEmpty()))
		{
			OutError = Path + TEXT("_invalid_") + Field;
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			if (!Value.IsValid() || Value->Type != EJson::String || Value->AsString().TrimStartAndEnd().IsEmpty())
			{
				OutError = Path + TEXT("_invalid_") + Field;
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
		return ProfileId == Other.ProfileId && IncludedRoles == Other.IncludedRoles
			&& TokenBudget == Other.TokenBudget;
	}

	bool FFileDescriptor::IsEnabledForProfile(const FString& ProfileId) const
	{
		return Profiles.Contains(NormalizeId(ProfileId));
	}

	bool FFileDescriptor::operator==(const FFileDescriptor& Other) const
	{
		return FileId == Other.FileId && Path == Other.Path && Role == Other.Role
			&& Profiles == Other.Profiles && bRequired == Other.bRequired
			&& ProjectionId == Other.ProjectionId && MergePolicy == Other.MergePolicy;
	}

	bool FFieldSourceDescriptor::operator==(const FFieldSourceDescriptor& Other) const
	{
		return Kind == Other.Kind && SemanticId == Other.SemanticId && ProjectorId == Other.ProjectorId;
	}

	bool FFieldDescriptor::operator==(const FFieldDescriptor& Other) const
	{
		return FieldId == Other.FieldId && OutputPath == Other.OutputPath && Source == Other.Source
			&& Role == Other.Role && bEditable == Other.bEditable && bRequired == Other.bRequired
			&& Representation == Other.Representation && EmptyPolicy == Other.EmptyPolicy
			&& DefaultPolicy == Other.DefaultPolicy && Externalization == Other.Externalization;
	}

	bool FWriterPolicy::PreservesValueText(const FString& ProfileId) const
	{
		return PreserveValueTextProfiles.Contains(NormalizeId(ProfileId));
	}

	bool FWriterPolicy::operator==(const FWriterPolicy& Other) const
	{
		return Format == Other.Format && InlineStructMaxFields == Other.InlineStructMaxFields
			&& bListCountsAsOneField == Other.bListCountsAsOneField
			&& bScalarListInline == Other.bScalarListInline
			&& KnownStructComponents == Other.KnownStructComponents
			&& TransformShape == Other.TransformShape
			&& PreserveValueTextProfiles == Other.PreserveValueTextProfiles
			&& RemoveFields == Other.RemoveFields
			&& PropertyPathPolicy == Other.PropertyPathPolicy
			&& UnknownOutputFieldPolicy == Other.UnknownOutputFieldPolicy;
	}

	bool FExtensionPolicy::operator==(const FExtensionPolicy& Other) const
	{
		return Input == Other.Input && bDirectUObjectRead == Other.bDirectUObjectRead;
	}

	bool FSortPolicy::operator==(const FSortPolicy& Other) const
	{
		return bDeterministic == Other.bDeterministic
			&& bIdentityBeforeContent == Other.bIdentityBeforeContent;
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
		FString Error;
		return ValidateContract(*this, Error);
	}

	bool FContract::operator==(const FContract& Other) const
	{
		return Schema == Other.Schema && ContractId == Other.ContractId
			&& ContractVersion == Other.ContractVersion && TypeId == Other.TypeId
			&& SemanticDescriptorRefs == Other.SemanticDescriptorRefs
			&& DefaultProfile == Other.DefaultProfile && Profiles == Other.Profiles
			&& Files == Other.Files && Fields == Other.Fields
			&& WriterPolicy == Other.WriterPolicy && ExtensionPolicy == Other.ExtensionPolicy
			&& SortPolicy == Other.SortPolicy;
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
				OutError = TEXT("conflicting_export_contract_") + Contract.ContractId;
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

	bool ValidateContract(const FContract& Contract, FString& OutError)
	{
		OutError.Reset();
		if (Contract.Schema != TEXT("uai.export_contract.v2")) OutError = TEXT("export_contract_schema_invalid");
		else if (!Matches(Contract.ContractId, TEXT("^uai\\.export\\.[a-z0-9_.-]+$"))) OutError = TEXT("export_contract_id_invalid");
		else if (!Matches(Contract.ContractVersion, TEXT("^[0-9]+\\.[0-9]+\\.[0-9]+$"))) OutError = TEXT("export_contract_version_invalid");
		else if (!Matches(Contract.TypeId, TEXT("^[a-z][a-z0-9_.-]+$"))) OutError = TEXT("export_contract_type_id_invalid");
		else if (Contract.SemanticDescriptorRefs.IsEmpty() || !IsUnique(Contract.SemanticDescriptorRefs)) OutError = TEXT("export_contract_semantic_refs_invalid");
		else if (Contract.DefaultProfile != TEXT("llm_authoring")) OutError = TEXT("export_contract_default_profile_invalid");
		else if (Contract.Profiles.Num() != 4) OutError = TEXT("export_contract_profiles_invalid");
		else if (Contract.Files.IsEmpty()) OutError = TEXT("export_contract_files_missing");
		else if (Contract.Fields.IsEmpty()) OutError = TEXT("export_contract_fields_missing");
		else if (Contract.WriterPolicy.Format != TEXT("hjson")
			|| Contract.WriterPolicy.InlineStructMaxFields != 6
			|| !Contract.WriterPolicy.bListCountsAsOneField
			|| !Contract.WriterPolicy.bScalarListInline
			|| Contract.WriterPolicy.KnownStructComponents != TEXT("array")
			|| Contract.WriterPolicy.TransformShape != TEXT("named_arrays")
			|| !HasExactSet(Contract.WriterPolicy.PreserveValueTextProfiles, { TEXT("readback"), TEXT("debug") })
			|| !HasExactSet(Contract.WriterPolicy.RemoveFields, { TEXT("operations"), TEXT("control_type"), TEXT("category_path") })
			|| Contract.WriterPolicy.PropertyPathPolicy != TEXT("only_when_needed_for_edit")
			|| Contract.WriterPolicy.UnknownOutputFieldPolicy != TEXT("reject")) OutError = TEXT("export_contract_writer_policy_invalid");
		else if (Contract.ExtensionPolicy.Input != TEXT("shared_state_snapshot_only")
			|| Contract.ExtensionPolicy.bDirectUObjectRead) OutError = TEXT("export_contract_extension_policy_invalid");
		else if (!Contract.SortPolicy.bDeterministic
			|| !Contract.SortPolicy.bIdentityBeforeContent) OutError = TEXT("export_contract_sort_policy_invalid");
		else
		{
			for (const FString& Reference : Contract.SemanticDescriptorRefs)
			{
				if (!Matches(Reference, TEXT("^uai\\.semantic\\.[a-z0-9_.-]+$")))
				{
					OutError = TEXT("export_contract_semantic_ref_invalid_") + Reference;
					return false;
				}
			}

			TSet<FString> ProfileIds;
			for (const FProfileDescriptor& Profile : Contract.Profiles)
			{
				if (!IsOneOf(Profile.ProfileId, { TEXT("llm_authoring"), TEXT("readback"), TEXT("debug"), TEXT("validation") })
					|| ProfileIds.Contains(Profile.ProfileId) || Profile.IncludedRoles.IsEmpty()
					|| !IsUnique(Profile.IncludedRoles) || Profile.TokenBudget < 1)
				{
					OutError = TEXT("export_contract_profile_invalid_") + Profile.ProfileId;
					return false;
				}
				for (const FString& Role : Profile.IncludedRoles)
				{
					if (!IsOneOf(Role, { TEXT("identity"), TEXT("index"), TEXT("authoring"), TEXT("reference"),
						TEXT("readonly"), TEXT("readback"), TEXT("diagnostics"), TEXT("validation") }))
					{
						OutError = TEXT("export_contract_profile_role_invalid_") + Role;
						return false;
					}
				}
				ProfileIds.Add(Profile.ProfileId);
			}
			const TArray<FString> RegisteredProfileIds = ProfileIds.Array();
			if (!HasExactSet(RegisteredProfileIds, {
				TEXT("llm_authoring"), TEXT("readback"), TEXT("debug"), TEXT("validation") }))
			{
				OutError = TEXT("export_contract_required_profiles_missing");
				return false;
			}

			TSet<FString> FileIds;
			TSet<FString> FilePaths;
			for (const FFileDescriptor& File : Contract.Files)
			{
				if (!Matches(File.FileId, TEXT("^[a-z][a-z0-9_.-]+$")) || FileIds.Contains(File.FileId)
					|| !IsSafeExportFilePath(File.Path) || FilePaths.Contains(File.Path)
					|| !IsOneOf(File.Role, { TEXT("identity"), TEXT("index"), TEXT("authoring"), TEXT("reference"),
						TEXT("readonly"), TEXT("readback"), TEXT("diagnostics"), TEXT("validation") })
					|| File.Profiles.IsEmpty() || !IsUnique(File.Profiles) || File.ProjectionId.IsEmpty()
					|| !IsOneOf(File.MergePolicy, { TEXT("inline"), TEXT("split"), TEXT("conditional") }))
				{
					OutError = TEXT("export_contract_file_invalid_") + File.FileId;
					return false;
				}
				for (const FString& ProfileId : File.Profiles)
				{
					const FProfileDescriptor* Profile = Contract.FindProfile(ProfileId);
					if (!Profile || !Profile->IncludesRole(File.Role))
					{
						OutError = TEXT("export_contract_file_profile_role_mismatch_") + File.FileId + TEXT("_") + ProfileId;
						return false;
					}
				}
				FileIds.Add(File.FileId);
				FilePaths.Add(File.Path);
			}

			TSet<FString> FieldIds;
			TSet<FString> OutputPaths;
			for (const FFieldDescriptor& Field : Contract.Fields)
			{
				FString FilePath;
				FString JsonPointer;
				const bool bOutputPathValid = SplitOutputPath(Field.OutputPath, FilePath, JsonPointer);
				const FFileDescriptor* File = bOutputPathValid ? Contract.FindFileByPath(FilePath) : nullptr;
				if (!Matches(Field.FieldId, TEXT("^[a-z][a-z0-9_.-]+$")) || FieldIds.Contains(Field.FieldId)
					|| !bOutputPathValid || OutputPaths.Contains(Field.OutputPath) || !File
					|| !IsOneOf(Field.Role, { TEXT("identity"), TEXT("index"), TEXT("authoring"), TEXT("reference"),
						TEXT("readonly"), TEXT("readback"), TEXT("diagnostics"), TEXT("validation") })
					|| (Field.bEditable && Field.Role != TEXT("authoring"))
					|| (Field.bRequired && Field.EmptyPolicy == TEXT("omit"))
					|| !IsOneOf(Field.Representation, { TEXT("canonical"), TEXT("summary"), TEXT("reference") })
					|| !IsOneOf(Field.EmptyPolicy, { TEXT("omit"), TEXT("emit_empty"), TEXT("emit_null") })
					|| !IsOneOf(Field.DefaultPolicy, { TEXT("omit_if_native_default"), TEXT("emit"), TEXT("not_applicable") })
					|| !IsOneOf(Field.Externalization, { TEXT("inline"), TEXT("file"), TEXT("conditional") }))
				{
					OutError = TEXT("export_contract_field_invalid_") + Field.FieldId;
					return false;
				}
				if (Field.Source.Kind == EFieldSourceKind::Semantic)
				{
					if (!Matches(Field.Source.SemanticId, TEXT("^[a-z][a-z0-9_.-]+$")))
					{
						OutError = TEXT("export_contract_field_semantic_invalid_") + Field.FieldId;
						return false;
					}
				}
				else if (!Field.Source.SemanticId.IsEmpty() || Field.Source.ProjectorId.IsEmpty())
				{
					OutError = TEXT("export_contract_field_source_invalid_") + Field.FieldId;
					return false;
				}
				bool bRoleReachable = false;
				for (const FString& ProfileId : File->Profiles)
				{
					const FProfileDescriptor* Profile = Contract.FindProfile(ProfileId);
					bRoleReachable |= Profile && Profile->IncludesRole(Field.Role);
				}
				if (!bRoleReachable)
				{
					OutError = TEXT("export_contract_field_role_unreachable_") + Field.FieldId;
					return false;
				}
				FieldIds.Add(Field.FieldId);
				OutputPaths.Add(Field.OutputPath);
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
			TEXT("schema"), TEXT("contract_id"), TEXT("contract_version"), TEXT("type_id"),
			TEXT("semantic_descriptor_refs"), TEXT("default_profile"), TEXT("profiles"), TEXT("files"),
			TEXT("fields"), TEXT("writer_policy"), TEXT("extension_policy"), TEXT("sort_policy")
		}, TEXT("export_contract"), OutError)) return false;
		if (!ReadString(Object, TEXT("schema"), TEXT("export_contract"), OutContract.Schema, OutError)
			|| !ReadString(Object, TEXT("contract_id"), TEXT("export_contract"), OutContract.ContractId, OutError)
			|| !ReadString(Object, TEXT("contract_version"), TEXT("export_contract"), OutContract.ContractVersion, OutError)
			|| !ReadString(Object, TEXT("type_id"), TEXT("export_contract"), OutContract.TypeId, OutError)
			|| !ReadStringArray(Object, TEXT("semantic_descriptor_refs"), TEXT("export_contract"), OutContract.SemanticDescriptorRefs, OutError)
			|| !ReadString(Object, TEXT("default_profile"), TEXT("export_contract"), OutContract.DefaultProfile, OutError)) return false;
		OutContract.ContractId = NormalizeId(OutContract.ContractId);
		OutContract.TypeId = NormalizeId(OutContract.TypeId);
		OutContract.DefaultProfile = NormalizeId(OutContract.DefaultProfile);
		NormalizeIds(OutContract.SemanticDescriptorRefs);

		const TArray<TSharedPtr<FJsonValue>>* Profiles = nullptr;
		if (!Object->TryGetArrayField(TEXT("profiles"), Profiles) || !Profiles || Profiles->IsEmpty())
		{
			OutError = TEXT("export_contract_profiles_missing");
			return false;
		}
		for (int32 Index = 0; Index < Profiles->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> ProfileObject = (*Profiles)[Index].IsValid() && (*Profiles)[Index]->Type == EJson::Object
				? (*Profiles)[Index]->AsObject() : nullptr;
			const FString Path = FString::Printf(TEXT("profiles_%d"), Index);
			if (!RequireOnlyFields(ProfileObject, { TEXT("profile_id"), TEXT("included_roles"), TEXT("token_budget") }, Path, OutError)) return false;
			FProfileDescriptor& Profile = OutContract.Profiles.AddDefaulted_GetRef();
			if (!ReadString(ProfileObject, TEXT("profile_id"), Path, Profile.ProfileId, OutError)
				|| !ReadStringArray(ProfileObject, TEXT("included_roles"), Path, Profile.IncludedRoles, OutError)
				|| !ReadPositiveInt(ProfileObject, TEXT("token_budget"), Path, Profile.TokenBudget, OutError)) return false;
			Profile.ProfileId = NormalizeId(Profile.ProfileId);
			NormalizeIds(Profile.IncludedRoles);
		}

		const TArray<TSharedPtr<FJsonValue>>* Files = nullptr;
		if (!Object->TryGetArrayField(TEXT("files"), Files) || !Files || Files->IsEmpty())
		{
			OutError = TEXT("export_contract_files_missing");
			return false;
		}
		for (int32 Index = 0; Index < Files->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> FileObject = (*Files)[Index].IsValid() && (*Files)[Index]->Type == EJson::Object
				? (*Files)[Index]->AsObject() : nullptr;
			const FString Path = FString::Printf(TEXT("files_%d"), Index);
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
			OutError = TEXT("export_contract_fields_missing");
			return false;
		}
		for (int32 Index = 0; Index < Fields->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> FieldObject = (*Fields)[Index].IsValid() && (*Fields)[Index]->Type == EJson::Object
				? (*Fields)[Index]->AsObject() : nullptr;
			const FString Path = FString::Printf(TEXT("fields_%d"), Index);
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
			if (!FieldObject->TryGetObjectField(TEXT("source"), SourceObject) || !SourceObject || !SourceObject->IsValid()
				|| !RequireOnlyFields(*SourceObject, { TEXT("kind"), TEXT("semantic_id"), TEXT("projector_id") }, Path + TEXT("_source"), OutError)) return false;
			FString KindText;
			if (!ReadString(*SourceObject, TEXT("kind"), Path + TEXT("_source"), KindText, OutError)
				|| !ParseFieldSourceKind(KindText, Field.Source.Kind))
			{
				if (OutError.IsEmpty()) OutError = Path + TEXT("_source_kind_invalid");
				return false;
			}
			(*SourceObject)->TryGetStringField(TEXT("semantic_id"), Field.Source.SemanticId);
			(*SourceObject)->TryGetStringField(TEXT("projector_id"), Field.Source.ProjectorId);
			Field.Source.SemanticId = NormalizeId(Field.Source.SemanticId);
			Field.Source.ProjectorId = NormalizeId(Field.Source.ProjectorId);
		}

		const TSharedPtr<FJsonObject>* Writer = nullptr;
		if (!Object->TryGetObjectField(TEXT("writer_policy"), Writer) || !Writer || !Writer->IsValid()
			|| !RequireOnlyFields(*Writer, { TEXT("format"), TEXT("inline_struct_max_fields"), TEXT("list_counts_as_one_field"),
				TEXT("scalar_list_inline"), TEXT("known_struct_components"), TEXT("transform_shape"),
				TEXT("preserve_value_text_profiles"), TEXT("remove_fields"), TEXT("property_path_policy"),
				TEXT("unknown_output_field_policy") }, TEXT("writer_policy"), OutError)
			|| !ReadString(*Writer, TEXT("format"), TEXT("writer_policy"), OutContract.WriterPolicy.Format, OutError)
			|| !ReadPositiveInt(*Writer, TEXT("inline_struct_max_fields"), TEXT("writer_policy"), OutContract.WriterPolicy.InlineStructMaxFields, OutError)
			|| !ReadBool(*Writer, TEXT("list_counts_as_one_field"), TEXT("writer_policy"), OutContract.WriterPolicy.bListCountsAsOneField, OutError)
			|| !ReadBool(*Writer, TEXT("scalar_list_inline"), TEXT("writer_policy"), OutContract.WriterPolicy.bScalarListInline, OutError)
			|| !ReadString(*Writer, TEXT("known_struct_components"), TEXT("writer_policy"), OutContract.WriterPolicy.KnownStructComponents, OutError)
			|| !ReadString(*Writer, TEXT("transform_shape"), TEXT("writer_policy"), OutContract.WriterPolicy.TransformShape, OutError)
			|| !ReadStringArray(*Writer, TEXT("preserve_value_text_profiles"), TEXT("writer_policy"), OutContract.WriterPolicy.PreserveValueTextProfiles, OutError)
			|| !ReadStringArray(*Writer, TEXT("remove_fields"), TEXT("writer_policy"), OutContract.WriterPolicy.RemoveFields, OutError)
			|| !ReadString(*Writer, TEXT("property_path_policy"), TEXT("writer_policy"), OutContract.WriterPolicy.PropertyPathPolicy, OutError)
			|| !ReadString(*Writer, TEXT("unknown_output_field_policy"), TEXT("writer_policy"), OutContract.WriterPolicy.UnknownOutputFieldPolicy, OutError)) return false;
		OutContract.WriterPolicy.Format = NormalizeId(OutContract.WriterPolicy.Format);
		OutContract.WriterPolicy.KnownStructComponents = NormalizeId(OutContract.WriterPolicy.KnownStructComponents);
		OutContract.WriterPolicy.TransformShape = NormalizeId(OutContract.WriterPolicy.TransformShape);
		OutContract.WriterPolicy.PropertyPathPolicy = NormalizeId(OutContract.WriterPolicy.PropertyPathPolicy);
		OutContract.WriterPolicy.UnknownOutputFieldPolicy = NormalizeId(OutContract.WriterPolicy.UnknownOutputFieldPolicy);
		NormalizeIds(OutContract.WriterPolicy.PreserveValueTextProfiles);
		NormalizeIds(OutContract.WriterPolicy.RemoveFields);

		const TSharedPtr<FJsonObject>* Extension = nullptr;
		if (!Object->TryGetObjectField(TEXT("extension_policy"), Extension) || !Extension || !Extension->IsValid()
			|| !RequireOnlyFields(*Extension, { TEXT("input"), TEXT("direct_ue_object_read") }, TEXT("extension_policy"), OutError)
			|| !ReadString(*Extension, TEXT("input"), TEXT("extension_policy"), OutContract.ExtensionPolicy.Input, OutError)
			|| !ReadBool(*Extension, TEXT("direct_ue_object_read"), TEXT("extension_policy"), OutContract.ExtensionPolicy.bDirectUObjectRead, OutError)) return false;
		OutContract.ExtensionPolicy.Input = NormalizeId(OutContract.ExtensionPolicy.Input);

		const TSharedPtr<FJsonObject>* Sort = nullptr;
		if (!Object->TryGetObjectField(TEXT("sort_policy"), Sort) || !Sort || !Sort->IsValid()
			|| !RequireOnlyFields(*Sort, { TEXT("deterministic"), TEXT("identity_before_content") }, TEXT("sort_policy"), OutError)
			|| !ReadBool(*Sort, TEXT("deterministic"), TEXT("sort_policy"), OutContract.SortPolicy.bDeterministic, OutError)
			|| !ReadBool(*Sort, TEXT("identity_before_content"), TEXT("sort_policy"), OutContract.SortPolicy.bIdentityBeforeContent, OutError)) return false;
		return ValidateContract(OutContract, OutError);
	}
}
