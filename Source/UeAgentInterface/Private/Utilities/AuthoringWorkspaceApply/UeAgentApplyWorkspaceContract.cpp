// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringWorkspaceApply/UeAgentApplyWorkspaceContract.h"

#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Internationalization/Regex.h"
#include "Misc/Paths.h"
#include "Misc/ScopeRWLock.h"
#include "Misc/ScopeExit.h"

namespace
{
 using namespace UeAgentApplyWorkspaceContract;
 using UeAgentAuthoringDiagnostics::MakeValueFailure;
 TSharedPtr<FJsonValue> StringValue(const FString& Value) { return MakeShared<FJsonValueString>(Value); }
 FString ChildPath(const FString& Path, FString Field) {
   Field.ReplaceInline(TEXT("~"), TEXT("~0")); Field.ReplaceInline(TEXT("/"), TEXT("~1"));
   return Path + TEXT("/") + Field;
 }
 TSharedPtr<FJsonValue> FieldValue(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field) {
   if (Object) if (const auto* Value = Object->Values.Find(Field)) return *Value;
   return nullptr;
 }
 TSharedPtr<FJsonValue> StringArrayValue(const TArray<FString>& Values) {
   TArray<TSharedPtr<FJsonValue>> Result;
   for (const auto& Value : Values) Result.Add(StringValue(Value));
   return MakeShared<FJsonValueArray>(Result);
 }
 TSharedPtr<FJsonValue> ContractLeafValue(const FString& Value) { return StringValue(Value); }
 TSharedPtr<FJsonValue> ContractLeafValue(bool Value) { return MakeShared<FJsonValueBoolean>(Value); }
 TSharedPtr<FJsonValue> ContractLeafValue(int32 Value) { return MakeShared<FJsonValueNumber>(Value); }
 TSharedPtr<FJsonValue> ContractLeafValue(ESourceKind Value) {
   const FString Name = SourceKindToString(Value);
   return StringValue(Name == TEXT("unknown") ? FString::Printf(TEXT("unknown (%d)"), static_cast<int32>(Value)) : Name);
 }
 template<typename TValue>
 bool ContractValuesEqual(const TValue& Expected, const TValue& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error) {
   if (Expected == Actual) return true;
   if (Error) {
     *Error = MakeValueFailure(TEXT("apply_workspace_contract_conflict"), Path, ContractLeafValue(Expected), ContractLeafValue(Actual),
         TEXT("contract"), TEXT("The new workspace contract differs from the contract already registered with this identifier."));
     Error->SourceKind = TEXT("native_result");
   }
   return false;
 }
 bool ContractValuesEqual(const FSourceDescriptor& Expected, const FSourceDescriptor& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error);
 bool ContractValuesEqual(const FEntryDescriptor& Expected, const FEntryDescriptor& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error);
 bool ContractValuesEqual(const FUnitDescriptor& Expected, const FUnitDescriptor& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error);
 bool ContractValuesEqual(const FReferencePolicy& Expected, const FReferencePolicy& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error);
 bool ContractValuesEqual(const FDependencyPolicy& Expected, const FDependencyPolicy& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error);
 bool ContractValuesEqual(const FPreflightPolicy& Expected, const FPreflightPolicy& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error);
 bool ContractValuesEqual(const FTransactionPolicy& Expected, const FTransactionPolicy& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error);
 bool ContractValuesEqual(const FExecutionPolicy& Expected, const FExecutionPolicy& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error);
 bool ContractValuesEqual(const FContract& Expected, const FContract& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error);
 template<typename TValue>
 bool ContractValuesEqual(const TArray<TValue>& Expected, const TArray<TValue>& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error) {
   if (Expected.Num() != Actual.Num()) return ContractValuesEqual(Expected.Num(), Actual.Num(), Path + TEXT("/length"), Error);
   for (int32 Index = 0; Index < Expected.Num(); ++Index)
     if (!ContractValuesEqual(Expected[Index], Actual[Index], Path + TEXT("/") + FString::FromInt(Index), Error)) return false;
   return true;
 }
 bool ContractValuesEqual(const FSourceDescriptor& Expected, const FSourceDescriptor& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error) {
   return ContractValuesEqual(Expected.Kind, Actual.Kind, Path + TEXT("/kind"), Error)
     && ContractValuesEqual(Expected.EntryMode, Actual.EntryMode, Path + TEXT("/entry_mode"), Error)
     && ContractValuesEqual(Expected.EntryRole, Actual.EntryRole, Path + TEXT("/entry_role"), Error);
 }
 bool ContractValuesEqual(const FEntryDescriptor& Expected, const FEntryDescriptor& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error) {
   return ContractValuesEqual(Expected.DirectoryEntry, Actual.DirectoryEntry, Path + TEXT("/directory_entry"), Error)
     && ContractValuesEqual(Expected.VirtualUnitId, Actual.VirtualUnitId, Path + TEXT("/virtual_unit_id"), Error);
 }
 bool ContractValuesEqual(const FUnitDescriptor& Expected, const FUnitDescriptor& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error) {
   return ContractValuesEqual(Expected.UnitId, Actual.UnitId, Path + TEXT("/unit_id"), Error)
     && ContractValuesEqual(Expected.PathPattern, Actual.PathPattern, Path + TEXT("/path_pattern"), Error)
     && ContractValuesEqual(Expected.Role, Actual.Role, Path + TEXT("/role"), Error)
     && ContractValuesEqual(Expected.Cardinality, Actual.Cardinality, Path + TEXT("/cardinality"), Error)
     && ContractValuesEqual(Expected.bRequired, Actual.bRequired, Path + TEXT("/required"), Error)
     && ContractValuesEqual(Expected.RouteId, Actual.RouteId, Path + TEXT("/route_id"), Error)
     && ContractValuesEqual(Expected.ApplyContractRef, Actual.ApplyContractRef, Path + TEXT("/apply_contract_ref"), Error)
     && ContractValuesEqual(Expected.MergePolicy, Actual.MergePolicy, Path + TEXT("/merge_policy"), Error)
     && ContractValuesEqual(Expected.AbsentSemantics, Actual.AbsentSemantics, Path + TEXT("/absent_semantics"), Error)
     && ContractValuesEqual(Expected.EmptySemantics, Actual.EmptySemantics, Path + TEXT("/empty_semantics"), Error);
 }
 bool ContractValuesEqual(const FReferencePolicy& Expected, const FReferencePolicy& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error) {
   return ContractValuesEqual(Expected.Base, Actual.Base, Path + TEXT("/base"), Error)
     && ContractValuesEqual(Expected.RootContainment, Actual.RootContainment, Path + TEXT("/root_containment"), Error)
     && ContractValuesEqual(Expected.ExternalReferences, Actual.ExternalReferences, Path + TEXT("/external_references"), Error)
     && ContractValuesEqual(Expected.CaseCollision, Actual.CaseCollision, Path + TEXT("/case_collision"), Error);
 }
 bool ContractValuesEqual(const FDependencyPolicy& Expected, const FDependencyPolicy& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error) {
   return ContractValuesEqual(Expected.EdgeSource, Actual.EdgeSource, Path + TEXT("/edge_source"), Error)
     && ContractValuesEqual(Expected.CyclePolicy, Actual.CyclePolicy, Path + TEXT("/cycle_policy"), Error)
     && ContractValuesEqual(Expected.UnresolvedPolicy, Actual.UnresolvedPolicy, Path + TEXT("/unresolved_policy"), Error)
     && ContractValuesEqual(Expected.Order, Actual.Order, Path + TEXT("/order"), Error);
 }
 bool ContractValuesEqual(const FPreflightPolicy& Expected, const FPreflightPolicy& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error) {
   return ContractValuesEqual(Expected.Scope, Actual.Scope, Path + TEXT("/scope"), Error)
     && ContractValuesEqual(Expected.MutationBeforeSuccess, Actual.MutationBeforeSuccess, Path + TEXT("/mutation_before_success"), Error)
     && ContractValuesEqual(Expected.bDiagnosticsIncludeSource, Actual.bDiagnosticsIncludeSource, Path + TEXT("/diagnostics_include_source"), Error);
 }
 bool ContractValuesEqual(const FTransactionPolicy& Expected, const FTransactionPolicy& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error) {
   return ContractValuesEqual(Expected.Mode, Actual.Mode, Path + TEXT("/mode"), Error)
     && ContractValuesEqual(Expected.RollbackOn, Actual.RollbackOn, Path + TEXT("/rollback_on"), Error);
 }
 bool ContractValuesEqual(const FExecutionPolicy& Expected, const FExecutionPolicy& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error) {
   return ContractValuesEqual(Expected.Compile, Actual.Compile, Path + TEXT("/compile"), Error)
     && ContractValuesEqual(Expected.Save, Actual.Save, Path + TEXT("/save"), Error)
     && ContractValuesEqual(Expected.Verification, Actual.Verification, Path + TEXT("/verification"), Error);
 }
 bool ContractValuesEqual(const FContract& Expected, const FContract& Actual, const FString& Path, UE::UAI::Contract::FDiagnostic* Error) {
   return ContractValuesEqual(Expected.Schema, Actual.Schema, Path + TEXT("/schema"), Error)
     && ContractValuesEqual(Expected.ContractId, Actual.ContractId, Path + TEXT("/contract_id"), Error)
     && ContractValuesEqual(Expected.ContractVersion, Actual.ContractVersion, Path + TEXT("/contract_version"), Error)
     && ContractValuesEqual(Expected.ProfileId, Actual.ProfileId, Path + TEXT("/profile_id"), Error)
     && ContractValuesEqual(Expected.Sources, Actual.Sources, Path + TEXT("/sources"), Error)
     && ContractValuesEqual(Expected.Entry, Actual.Entry, Path + TEXT("/entry"), Error)
     && ContractValuesEqual(Expected.Units, Actual.Units, Path + TEXT("/units"), Error)
     && ContractValuesEqual(Expected.ReferencePolicy, Actual.ReferencePolicy, Path + TEXT("/reference_policy"), Error)
     && ContractValuesEqual(Expected.DependencyPolicy, Actual.DependencyPolicy, Path + TEXT("/dependency_policy"), Error)
     && ContractValuesEqual(Expected.Preflight, Actual.Preflight, Path + TEXT("/preflight"), Error)
     && ContractValuesEqual(Expected.Transaction, Actual.Transaction, Path + TEXT("/transaction"), Error)
     && ContractValuesEqual(Expected.Execution, Actual.Execution, Path + TEXT("/execution"), Error)
     && ContractValuesEqual(Expected.UnknownFilePolicy, Actual.UnknownFilePolicy, Path + TEXT("/unknown_file_policy"), Error)
     && ContractValuesEqual(Expected.DuplicateTargetPolicy, Actual.DuplicateTargetPolicy, Path + TEXT("/duplicate_target_policy"), Error);
 }

	bool IsOneOf(const FString& Value, std::initializer_list<const TCHAR*> Allowed)
	{
		for (const TCHAR* Candidate : Allowed)
		{
			if (Value == Candidate) return true;
		}
		return false;
	}

	bool MatchesPattern(const FString& Value, const TCHAR* Pattern)
	{
		FRegexMatcher Matcher(FRegexPattern(Pattern), Value);
		return Matcher.FindNext()
			&& Matcher.GetMatchBeginning() == 0
			&& Matcher.GetMatchEnding() == Value.Len();
	}

	bool CheckAllowedFields(
		const TSharedPtr<FJsonObject>& Object,
		std::initializer_list<const TCHAR*> AllowedFields,
		const FString& Path,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!Object.IsValid())
		{
			OutError = MakeValueFailure(TEXT("apply_workspace_contract_object_required"), Path, StringValue(TEXT("object")), nullptr, TEXT("contract"), TEXT("The workspace contract requires an object at this path."));
			return false;
		}

		TSet<FString> Allowed;
		for (const TCHAR* Field : AllowedFields) Allowed.Add(Field);
		TArray<FString> ActualFields;
		Object->Values.GetKeys(ActualFields);
		ActualFields.Sort();
		for (const FString& Field : ActualFields)
		{
			if (!Allowed.Contains(Field))
			{
				OutError = MakeValueFailure(TEXT("apply_workspace_contract_unknown_field"), ChildPath(Path, Field), nullptr, MakeShared<FJsonValueBoolean>(true), TEXT("contract"), TEXT("This field is not declared by the workspace contract format."));
                OutError.ActualValue.Reset();
                OutError.ExpectedValuePresent = false;
                OutError.ActualValuePresent = true;
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
		if (!Object.IsValid() || !Object->TryGetStringField(Field, OutValue) || OutValue.IsEmpty())
		{
			OutError = MakeValueFailure(TEXT("apply_workspace_contract_string_required"), ChildPath(Path, Field), StringValue(TEXT("nonempty string")), FieldValue(Object, Field), TEXT("contract"), TEXT("The contract field requires a nonempty string."));
			return false;
		}
		return true;
	}

	bool ReadBool(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		const FString& Path,
		bool& bOutValue,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!Object.IsValid() || !Object->TryGetBoolField(Field, bOutValue))
		{
			OutError = MakeValueFailure(TEXT("apply_workspace_contract_bool_required"), ChildPath(Path, Field), StringValue(TEXT("boolean")), FieldValue(Object, Field), TEXT("contract"), TEXT("The contract field requires a boolean."));
			return false;
		}
		return true;
	}

	bool ReadObject(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		const FString& Path,
		TSharedPtr<FJsonObject>& OutValue,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		const TSharedPtr<FJsonObject>* Value = nullptr;
		if (!Object.IsValid() || !Object->TryGetObjectField(Field, Value) || !Value || !Value->IsValid())
		{
			OutError = MakeValueFailure(TEXT("apply_workspace_contract_object_required"), ChildPath(Path, Field), StringValue(TEXT("object")), FieldValue(Object, Field), TEXT("contract"), TEXT("The contract field requires an object."));
			return false;
		}
		OutValue = *Value;
		return true;
	}

	bool ReadArray(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		const FString& Path,
		const TArray<TSharedPtr<FJsonValue>>*& OutValues,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutValues = nullptr;
		if (!Object.IsValid() || !Object->TryGetArrayField(Field, OutValues) || !OutValues)
		{
			OutError = MakeValueFailure(TEXT("apply_workspace_contract_array_required"), ChildPath(Path, Field), StringValue(TEXT("array")), FieldValue(Object, Field), TEXT("contract"), TEXT("The contract field requires an array."));
			return false;
		}
		return true;
	}

	bool ReadStringArray(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		const FString& Path,
		TArray<FString>& OutValues,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!ReadArray(Object, Field, Path, Values, OutError)) return false;
		OutValues.Reset();
		for (int32 Index = 0; Index < Values->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& Value = (*Values)[Index];
			if (!Value.IsValid() || Value->Type != EJson::String || Value->AsString().IsEmpty())
			{
				OutError = MakeValueFailure(TEXT("apply_workspace_contract_string_required"), ChildPath(Path, Field) + TEXT("/") + FString::FromInt(Index), StringValue(TEXT("nonempty string")), Value, TEXT("contract"), TEXT("This contract array entry requires a nonempty string."));
				return false;
			}
			OutValues.Add(Value->AsString());
		}
		return true;
	}


	UeAgentApplyWorkspaceContract::FContract CanonicalizeContract(
		const UeAgentApplyWorkspaceContract::FContract& Contract)
	{
		UeAgentApplyWorkspaceContract::FContract Result = Contract;
		Result.Sources.Sort([](
			const UeAgentApplyWorkspaceContract::FSourceDescriptor& Left,
			const UeAgentApplyWorkspaceContract::FSourceDescriptor& Right)
		{
			return static_cast<uint8>(Left.Kind) < static_cast<uint8>(Right.Kind);
		});
		Result.Units.Sort([](
			const UeAgentApplyWorkspaceContract::FUnitDescriptor& Left,
			const UeAgentApplyWorkspaceContract::FUnitDescriptor& Right)
		{
			return Left.UnitId < Right.UnitId;
		});
		Result.Transaction.RollbackOn.Sort();
		return Result;
	}
}

namespace UeAgentApplyWorkspaceContract
{
	bool FSourceDescriptor::operator==(const FSourceDescriptor& Other) const
	{
		return ContractValuesEqual(*this, Other, FString(), nullptr);
	}

	bool FEntryDescriptor::operator==(const FEntryDescriptor& Other) const
	{
		return ContractValuesEqual(*this, Other, FString(), nullptr);
	}

	bool FUnitDescriptor::operator==(const FUnitDescriptor& Other) const
	{
		return ContractValuesEqual(*this, Other, FString(), nullptr);
	}

	bool FReferencePolicy::operator==(const FReferencePolicy& Other) const
	{
		return ContractValuesEqual(*this, Other, FString(), nullptr);
	}

	bool FDependencyPolicy::operator==(const FDependencyPolicy& Other) const
	{
		return ContractValuesEqual(*this, Other, FString(), nullptr);
	}

	bool FPreflightPolicy::operator==(const FPreflightPolicy& Other) const
	{
		return ContractValuesEqual(*this, Other, FString(), nullptr);
	}

	bool FTransactionPolicy::operator==(const FTransactionPolicy& Other) const
	{
		return ContractValuesEqual(*this, Other, FString(), nullptr);
	}

	bool FExecutionPolicy::operator==(const FExecutionPolicy& Other) const
	{
		return ContractValuesEqual(*this, Other, FString(), nullptr);
	}

	const FSourceDescriptor* FContract::FindSource(ESourceKind Kind) const
	{
		return Sources.FindByPredicate([Kind](const FSourceDescriptor& Source) { return Source.Kind == Kind; });
	}

	const FUnitDescriptor* FContract::FindUnit(const FString& UnitId) const
	{
		return Units.FindByPredicate([&UnitId](const FUnitDescriptor& Unit) { return Unit.UnitId == UnitId; });
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
		OutResult = ERegistrationResult::Registered;
		OutError.Reset();
		UE::UAI::Contract::FDiagnostic ValidationError;
		if (!ValidateContract(Contract, ValidationError))
		{
			OutError = ValidationError;
			return false;
		}

		const FContract CanonicalContract = CanonicalizeContract(Contract);
		FWriteScopeLock Lock(RegistryLock);
		if (const FContract* Existing = Contracts.Find(CanonicalContract.ContractId))
		{
			if (ContractValuesEqual(*Existing, CanonicalContract, FString(), &OutError))
			{
				OutResult = ERegistrationResult::AlreadyRegistered;
				return true;
			}
			OutError.CandidateName = CanonicalContract.ContractId;
			return false;
		}
		Contracts.Add(CanonicalContract.ContractId, CanonicalContract);
		return true;
	}

	bool FRegistry::FindContract(const FString& ContractId, FContract& OutContract) const
	{
		FReadScopeLock Lock(RegistryLock);
		const FContract* Contract = Contracts.Find(ContractId);
		if (!Contract) return false;
		OutContract = *Contract;
		return true;
	}

	TArray<FContract> FRegistry::ListContracts() const
	{
		FReadScopeLock Lock(RegistryLock);
		TArray<FContract> Result;
		Contracts.GenerateValueArray(Result);
		Result.Sort([](const FContract& Left, const FContract& Right) { return Left.ContractId < Right.ContractId; });
		return Result;
	}

	int32 FRegistry::GetContractCount() const
	{
		FReadScopeLock Lock(RegistryLock);
		return Contracts.Num();
	}

	FString SourceKindToString(ESourceKind Kind)
	{
		switch (Kind)
		{
		case ESourceKind::Directory: return TEXT("directory");
		case ESourceKind::SingleFile: return TEXT("single_file");
		case ESourceKind::Inline: return TEXT("inline");
		default: return TEXT("unknown");
		}
	}

	bool ParseSourceKind(const FString& Text, ESourceKind& OutKind)
	{
		if (Text == TEXT("directory")) OutKind = ESourceKind::Directory;
		else if (Text == TEXT("single_file")) OutKind = ESourceKind::SingleFile;
		else if (Text == TEXT("inline")) OutKind = ESourceKind::Inline;
		else return false;
		return true;
	}

 bool ValidateContract(const FContract& Contract, UE::UAI::Contract::FDiagnostic& OutError)
 {
   OutError.Reset();
   ON_SCOPE_EXIT { if (!OutError.IsEmpty()) { OutError.CandidateName = Contract.ContractId; OutError.SourceKind = TEXT("native_result"); } };
   const auto Reject = [&](const TCHAR* Code, const TCHAR* Reason, const FString& Path,
                           const TSharedPtr<FJsonValue>& Expected, const TSharedPtr<FJsonValue>& Actual) {
     OutError = MakeValueFailure(Code, Path, Expected, Actual, TEXT("contract"), Reason);
     return false;
   };
   if (Contract.Schema != TEXT("uai.apply_workspace_contract.v1")) return Reject(TEXT("apply_workspace_contract_schema_invalid"), TEXT("The workspace contract schema is unsupported."), TEXT("/schema"), StringValue(TEXT("uai.apply_workspace_contract.v1")), StringValue(Contract.Schema));
   if (!MatchesPattern(Contract.ContractId, TEXT("^uai\\.apply_workspace\\.[a-z0-9_.-]+$"))) return Reject(TEXT("apply_workspace_contract_id_invalid"), TEXT("The workspace contract identifier has an invalid format."), TEXT("/contract_id"), StringValue(TEXT("uai.apply_workspace.<lowercase identifier>")), StringValue(Contract.ContractId));
   if (!MatchesPattern(Contract.ContractVersion, TEXT("^[0-9]+\\.[0-9]+\\.[0-9]+$"))) return Reject(TEXT("apply_workspace_contract_version_invalid"), TEXT("The workspace contract version must contain three numeric components."), TEXT("/contract_version"), StringValue(TEXT("major.minor.patch")), StringValue(Contract.ContractVersion));
   if (!MatchesPattern(Contract.ProfileId, TEXT("^[a-z][a-z0-9_.-]+$"))) return Reject(TEXT("apply_workspace_contract_profile_invalid"), TEXT("The workspace profile identifier has an invalid format."), TEXT("/profile_id"), StringValue(TEXT("lowercase identifier starting with a letter")), StringValue(Contract.ProfileId));
   TSet<ESourceKind> SeenSources;
   TArray<FString> SourceNames;
   for (int32 Index = 0; Index < Contract.Sources.Num(); ++Index) {
     const auto& Source = Contract.Sources[Index];
     const FString Path = TEXT("/sources/") + FString::FromInt(Index);
     if (Source.Kind != ESourceKind::Directory && Source.Kind != ESourceKind::SingleFile && Source.Kind != ESourceKind::Inline)
       return Reject(TEXT("apply_workspace_contract_source_invalid"), TEXT("The workspace source kind is unsupported."), Path + TEXT("/kind"), StringValue(TEXT("directory, single_file, inline")), ContractLeafValue(Source.Kind));
     if (SeenSources.Contains(Source.Kind)) return Reject(TEXT("apply_workspace_contract_source_duplicate"), TEXT("The workspace source kind duplicates an earlier source."), Path + TEXT("/kind"), StringValue(TEXT("unique source kind")), ContractLeafValue(Source.Kind));
     SeenSources.Add(Source.Kind); SourceNames.Add(SourceKindToString(Source.Kind));
     if (!(Source.Kind == ESourceKind::Directory ? Source.EntryMode == TEXT("physical") : IsOneOf(Source.EntryMode, {TEXT("virtual"), TEXT("virtual_bundle")})))
       return Reject(TEXT("apply_workspace_contract_source_invalid"), TEXT("The source entry mode is unsupported for this source kind."), Path + TEXT("/entry_mode"), StringValue(Source.Kind == ESourceKind::Directory ? TEXT("physical") : TEXT("virtual, virtual_bundle")), StringValue(Source.EntryMode));
     if (!IsOneOf(Source.EntryRole, {TEXT("index"), TEXT("object"), TEXT("object_list"), TEXT("control")}))
       return Reject(TEXT("apply_workspace_contract_source_invalid"), TEXT("The workspace source entry role is unsupported."), Path + TEXT("/entry_role"), StringValue(TEXT("index, object, object_list, control")), StringValue(Source.EntryRole));
   }
   if (!SeenSources.Contains(ESourceKind::Directory) || !SeenSources.Contains(ESourceKind::SingleFile) || !SeenSources.Contains(ESourceKind::Inline))
     return Reject(TEXT("apply_workspace_contract_sources_required"), TEXT("The workspace contract must declare all supported source kinds."), TEXT("/sources"), StringArrayValue({TEXT("directory"), TEXT("single_file"), TEXT("inline")}), StringArrayValue(SourceNames));
   if (Contract.Entry.DirectoryEntry.IsEmpty() || Contract.Entry.DirectoryEntry.Contains(TEXT("\\")) || !FPaths::IsRelative(Contract.Entry.DirectoryEntry) || !Contract.Entry.DirectoryEntry.EndsWith(TEXT(".hjson"), ESearchCase::IgnoreCase))
     return Reject(TEXT("apply_workspace_contract_directory_entry_invalid"), TEXT("The directory entry must be a nonempty relative HJSON path using forward slashes."), TEXT("/entry/directory_entry"), StringValue(TEXT("relative .hjson path")), StringValue(Contract.Entry.DirectoryEntry));
   if (Contract.Units.IsEmpty()) return Reject(TEXT("apply_workspace_contract_units_required"), TEXT("The workspace contract declares no units."), TEXT("/units"), StringValue(TEXT("nonempty unit array")), MakeShared<FJsonValueNumber>(0));
   TSet<FString> SeenUnitIds;
   for (int32 Index = 0; Index < Contract.Units.Num(); ++Index) {
     const auto& Unit = Contract.Units[Index];
     const FString Path = TEXT("/units/") + FString::FromInt(Index);
     if (!MatchesPattern(Unit.UnitId, TEXT("^[a-z][a-z0-9_.-]+$"))) return Reject(TEXT("apply_workspace_contract_unit_invalid"), TEXT("The workspace unit identifier has an invalid format."), Path + TEXT("/unit_id"), StringValue(TEXT("lowercase identifier starting with a letter")), StringValue(Unit.UnitId));
     if (SeenUnitIds.Contains(Unit.UnitId)) return Reject(TEXT("apply_workspace_contract_unit_invalid"), TEXT("The workspace unit identifier duplicates an earlier unit."), Path + TEXT("/unit_id"), StringValue(TEXT("unique unit identifier")), StringValue(Unit.UnitId));
   if (Unit.PathPattern.IsEmpty()) return Reject(TEXT("apply_workspace_contract_unit_invalid"), TEXT("The workspace unit field must not be empty."), Path + TEXT("/path_pattern"), StringValue(TEXT("nonempty string")), StringValue(Unit.PathPattern));
   if (Unit.RouteId.IsEmpty()) return Reject(TEXT("apply_workspace_contract_unit_invalid"), TEXT("The workspace unit field must not be empty."), Path + TEXT("/route_id"), StringValue(TEXT("nonempty string")), StringValue(Unit.RouteId));
   if (!MatchesPattern(Unit.ApplyContractRef, TEXT("^uai\\.apply_input\\.[a-z0-9_.-]+$"))) return Reject(TEXT("apply_workspace_contract_unit_invalid"), TEXT("The apply-input contract reference has an invalid format."), Path + TEXT("/apply_contract_ref"), StringValue(TEXT("uai.apply_input.<lowercase identifier>")), StringValue(Unit.ApplyContractRef));
   if (!IsOneOf(Unit.Role, {TEXT("identity"), TEXT("index"), TEXT("settings"), TEXT("object"), TEXT("object_list"), TEXT("details"), TEXT("graph"), TEXT("layout"), TEXT("connections"), TEXT("reference"), TEXT("control")})) return Reject(TEXT("apply_workspace_contract_unit_invalid"), TEXT("The workspace unit field contains an unsupported value."), Path + TEXT("/role"), StringValue(TEXT("identity, index, settings, object, object_list, details, graph, layout, connections, reference, control")), StringValue(Unit.Role));
   if (!IsOneOf(Unit.Cardinality, {TEXT("one"), TEXT("zero_or_one"), TEXT("zero_or_more"), TEXT("one_or_more")})) return Reject(TEXT("apply_workspace_contract_unit_invalid"), TEXT("The workspace unit field contains an unsupported value."), Path + TEXT("/cardinality"), StringValue(TEXT("one, zero_or_one, zero_or_more, one_or_more")), StringValue(Unit.Cardinality));
   if (!IsOneOf(Unit.MergePolicy, {TEXT("replace"), TEXT("merge"), TEXT("append"), TEXT("reject_duplicate")})) return Reject(TEXT("apply_workspace_contract_unit_invalid"), TEXT("The workspace unit field contains an unsupported value."), Path + TEXT("/merge_policy"), StringValue(TEXT("replace, merge, append, reject_duplicate")), StringValue(Unit.MergePolicy));
   if (!IsOneOf(Unit.AbsentSemantics, {TEXT("no_change"), TEXT("error"), TEXT("empty_value")})) return Reject(TEXT("apply_workspace_contract_unit_invalid"), TEXT("The workspace unit field contains an unsupported value."), Path + TEXT("/absent_semantics"), StringValue(TEXT("no_change, error, empty_value")), StringValue(Unit.AbsentSemantics));
   if (!IsOneOf(Unit.EmptySemantics, {TEXT("explicit_empty"), TEXT("remove"), TEXT("no_change"), TEXT("error")})) return Reject(TEXT("apply_workspace_contract_unit_invalid"), TEXT("The workspace unit field contains an unsupported value."), Path + TEXT("/empty_semantics"), StringValue(TEXT("explicit_empty, remove, no_change, error")), StringValue(Unit.EmptySemantics));
     SeenUnitIds.Add(Unit.UnitId);
   }
   if (!Contract.FindUnit(Contract.Entry.VirtualUnitId)) return Reject(TEXT("apply_workspace_contract_virtual_unit_missing"), TEXT("The virtual entry references an undeclared workspace unit."), TEXT("/entry/virtual_unit_id"), StringArrayValue(SeenUnitIds.Array()), StringValue(Contract.Entry.VirtualUnitId));
   if (Contract.ReferencePolicy.ExternalReferences == TEXT("contract_allowlist")) return Reject(TEXT("apply_workspace_external_reference_policy_unimplemented"), TEXT("The contract_allowlist external-reference policy is not implemented."), TEXT("/reference_policy/external_references"), StringValue(TEXT("reject")), StringValue(Contract.ReferencePolicy.ExternalReferences));
   if (Contract.ReferencePolicy.Base != TEXT("workspace_root")) return Reject(TEXT("apply_workspace_contract_reference_policy_invalid"), TEXT("The workspace contract field does not match the required policy."), TEXT("/reference_policy/base"), StringValue(TEXT("workspace_root")), StringValue(Contract.ReferencePolicy.Base));
   if (Contract.ReferencePolicy.RootContainment != TEXT("required")) return Reject(TEXT("apply_workspace_contract_reference_policy_invalid"), TEXT("The workspace contract field does not match the required policy."), TEXT("/reference_policy/root_containment"), StringValue(TEXT("required")), StringValue(Contract.ReferencePolicy.RootContainment));
   if (Contract.ReferencePolicy.ExternalReferences != TEXT("reject")) return Reject(TEXT("apply_workspace_contract_reference_policy_invalid"), TEXT("The workspace contract field does not match the required policy."), TEXT("/reference_policy/external_references"), StringValue(TEXT("reject")), StringValue(Contract.ReferencePolicy.ExternalReferences));
   if (Contract.ReferencePolicy.CaseCollision != TEXT("reject")) return Reject(TEXT("apply_workspace_contract_reference_policy_invalid"), TEXT("The workspace contract field does not match the required policy."), TEXT("/reference_policy/case_collision"), StringValue(TEXT("reject")), StringValue(Contract.ReferencePolicy.CaseCollision));
   if (Contract.DependencyPolicy.EdgeSource != TEXT("contract_and_references")) return Reject(TEXT("apply_workspace_contract_dependency_policy_invalid"), TEXT("The workspace contract field does not match the required policy."), TEXT("/dependency_policy/edge_source"), StringValue(TEXT("contract_and_references")), StringValue(Contract.DependencyPolicy.EdgeSource));
   if (Contract.DependencyPolicy.CyclePolicy != TEXT("reject")) return Reject(TEXT("apply_workspace_contract_dependency_policy_invalid"), TEXT("The workspace contract field does not match the required policy."), TEXT("/dependency_policy/cycle_policy"), StringValue(TEXT("reject")), StringValue(Contract.DependencyPolicy.CyclePolicy));
   if (Contract.DependencyPolicy.UnresolvedPolicy != TEXT("reject")) return Reject(TEXT("apply_workspace_contract_dependency_policy_invalid"), TEXT("The workspace contract field does not match the required policy."), TEXT("/dependency_policy/unresolved_policy"), StringValue(TEXT("reject")), StringValue(Contract.DependencyPolicy.UnresolvedPolicy));
   if (Contract.DependencyPolicy.Order != TEXT("stable_topological")) return Reject(TEXT("apply_workspace_contract_dependency_policy_invalid"), TEXT("The workspace contract field does not match the required policy."), TEXT("/dependency_policy/order"), StringValue(TEXT("stable_topological")), StringValue(Contract.DependencyPolicy.Order));
   if (Contract.Preflight.Scope != TEXT("whole_workspace")) return Reject(TEXT("apply_workspace_contract_preflight_policy_invalid"), TEXT("The workspace contract field does not match the required policy."), TEXT("/preflight/scope"), StringValue(TEXT("whole_workspace")), StringValue(Contract.Preflight.Scope));
   if (Contract.Preflight.MutationBeforeSuccess != TEXT("forbid")) return Reject(TEXT("apply_workspace_contract_preflight_policy_invalid"), TEXT("The workspace contract field does not match the required policy."), TEXT("/preflight/mutation_before_success"), StringValue(TEXT("forbid")), StringValue(Contract.Preflight.MutationBeforeSuccess));
   if (Contract.Transaction.Mode != TEXT("atomic")) return Reject(TEXT("apply_workspace_contract_transaction_policy_invalid"), TEXT("The workspace contract field does not match the required policy."), TEXT("/transaction/mode"), StringValue(TEXT("atomic")), StringValue(Contract.Transaction.Mode));
   if (!Contract.Preflight.bDiagnosticsIncludeSource) return Reject(TEXT("apply_workspace_contract_preflight_policy_invalid"), TEXT("Workspace preflight diagnostics must include input source locations."), TEXT("/preflight/diagnostics_include_source"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
   TSet<FString> SeenTriggers;
   for (int32 Index = 0; Index < Contract.Transaction.RollbackOn.Num(); ++Index) {
     const auto& Trigger = Contract.Transaction.RollbackOn[Index];
     const FString Path = TEXT("/transaction/rollback_on/") + FString::FromInt(Index);
     if (SeenTriggers.Contains(Trigger)) return Reject(TEXT("apply_workspace_contract_transaction_policy_invalid"), TEXT("The rollback trigger duplicates an earlier entry."), Path, StringValue(TEXT("unique trigger")), StringValue(Trigger));
     if (!IsOneOf(Trigger, {TEXT("mutation_failed"), TEXT("compile_failed"), TEXT("save_failed"), TEXT("verification_failed")}))
       return Reject(TEXT("apply_workspace_contract_rollback_trigger_invalid"), TEXT("The rollback trigger is unsupported."), Path, StringValue(TEXT("mutation_failed, compile_failed, save_failed, verification_failed")), StringValue(Trigger));
     SeenTriggers.Add(Trigger);
   }
   for (const TCHAR* Required : {TEXT("mutation_failed"), TEXT("compile_failed"), TEXT("save_failed"), TEXT("verification_failed")})
     if (!SeenTriggers.Contains(Required)) return Reject(TEXT("apply_workspace_contract_transaction_policy_invalid"), TEXT("The atomic transaction policy is missing a required rollback trigger."), TEXT("/transaction/rollback_on"), StringValue(Required), StringArrayValue(Contract.Transaction.RollbackOn));
   if (!IsOneOf(Contract.Execution.Compile, {TEXT("provider_hook"), TEXT("provider_hook_required"), TEXT("none")})) return Reject(TEXT("apply_workspace_contract_execution_policy_invalid"), TEXT("The workspace execution policy contains an unsupported value."), TEXT("/execution/compile"), StringValue(TEXT("provider_hook, provider_hook_required, none")), StringValue(Contract.Execution.Compile));
   if (!IsOneOf(Contract.Execution.Save, {TEXT("caller_controlled"), TEXT("caller_controlled_required_default"), TEXT("required"), TEXT("none")})) return Reject(TEXT("apply_workspace_contract_execution_policy_invalid"), TEXT("The workspace execution policy contains an unsupported value."), TEXT("/execution/save"), StringValue(TEXT("caller_controlled, caller_controlled_required_default, required, none")), StringValue(Contract.Execution.Save));
   if (Contract.Execution.Verification != TEXT("shared_snapshot_required")) return Reject(TEXT("apply_workspace_contract_execution_policy_invalid"), TEXT("Workspace execution requires shared-snapshot verification."), TEXT("/execution/verification"), StringValue(TEXT("shared_snapshot_required")), StringValue(Contract.Execution.Verification));
   if (Contract.UnknownFilePolicy != TEXT("reject")) return Reject(TEXT("apply_workspace_contract_rejection_policy_invalid"), TEXT("The workspace rejection policy must reject unsupported or duplicate input."), TEXT("/unknown_file_policy"), StringValue(TEXT("reject")), StringValue(Contract.UnknownFilePolicy));
   if (Contract.DuplicateTargetPolicy != TEXT("reject")) return Reject(TEXT("apply_workspace_contract_rejection_policy_invalid"), TEXT("The workspace rejection policy must reject unsupported or duplicate input."), TEXT("/duplicate_target_policy"), StringValue(TEXT("reject")), StringValue(Contract.DuplicateTargetPolicy));
   return true;
 }

	bool ParseContract(
		const TSharedPtr<FJsonObject>& Object,
		FContract& OutContract,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutContract = FContract();
		OutError.Reset();
        ON_SCOPE_EXIT { if (!OutError.IsEmpty() && Object) { OutError.SourceKind = TEXT("authored_value"); UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Object); } };
		if (!CheckAllowedFields(Object, {
			TEXT("schema"), TEXT("contract_id"), TEXT("contract_version"), TEXT("profile_id"),
			TEXT("sources"), TEXT("entry"), TEXT("units"), TEXT("reference_policy"),
			TEXT("dependency_policy"), TEXT("preflight"), TEXT("transaction"), TEXT("execution"),
			TEXT("unknown_file_policy"), TEXT("duplicate_target_policy") }, FString(), OutError)) return false;

		FContract Contract;
		if (!ReadString(Object, TEXT("schema"), FString(), Contract.Schema, OutError)
			|| !ReadString(Object, TEXT("contract_id"), FString(), Contract.ContractId, OutError)
			|| !ReadString(Object, TEXT("contract_version"), FString(), Contract.ContractVersion, OutError)
			|| !ReadString(Object, TEXT("profile_id"), FString(), Contract.ProfileId, OutError)) return false;

		const TArray<TSharedPtr<FJsonValue>>* SourceValues = nullptr;
		if (!ReadArray(Object, TEXT("sources"), FString(), SourceValues, OutError)) return false;
		for (int32 Index = 0; Index < SourceValues->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& SourceValue = (*SourceValues)[Index];
			const TSharedPtr<FJsonObject> SourceObject =
				SourceValue.IsValid() && SourceValue->Type == EJson::Object ? SourceValue->AsObject() : nullptr;
			const FString Path = FString::Printf(TEXT("/sources/%d"), Index);
            if (!SourceObject) {
                OutError = MakeValueFailure(TEXT("apply_workspace_contract_object_required"), Path, StringValue(TEXT("object")), SourceValue, TEXT("contract"), TEXT("This contract array entry must be an object."));
                return false;
            }
			if (!CheckAllowedFields(SourceObject, { TEXT("kind"), TEXT("entry_mode"), TEXT("entry_role") }, Path, OutError)) return false;
			FSourceDescriptor Source;
			FString KindText;
            if (!ReadString(SourceObject, TEXT("kind"), Path, KindText, OutError)) return false;
            if (!ParseSourceKind(KindText, Source.Kind)) {
                OutError = MakeValueFailure(TEXT("apply_workspace_contract_source_invalid"), Path + TEXT("/kind"), StringValue(TEXT("directory, single_file, inline")), StringValue(KindText), TEXT("contract"), TEXT("The workspace source kind is unsupported."));
                return false;
            }
            if (!ReadString(SourceObject, TEXT("entry_mode"), Path, Source.EntryMode, OutError)
                || !ReadString(SourceObject, TEXT("entry_role"), Path, Source.EntryRole, OutError)) return false;
			Contract.Sources.Add(MoveTemp(Source));
		}
		TSharedPtr<FJsonObject> EntryObject;
		if (!ReadObject(Object, TEXT("entry"), FString(), EntryObject, OutError)
			|| !CheckAllowedFields(EntryObject, { TEXT("directory_entry"), TEXT("virtual_unit_id") }, TEXT("/entry"), OutError)
			|| !ReadString(EntryObject, TEXT("directory_entry"), TEXT("/entry"), Contract.Entry.DirectoryEntry, OutError)
			|| !ReadString(EntryObject, TEXT("virtual_unit_id"), TEXT("/entry"), Contract.Entry.VirtualUnitId, OutError)) return false;
		const TArray<TSharedPtr<FJsonValue>>* UnitValues = nullptr;
		if (!ReadArray(Object, TEXT("units"), FString(), UnitValues, OutError)) return false;
		for (int32 Index = 0; Index < UnitValues->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& UnitValue = (*UnitValues)[Index];
			const TSharedPtr<FJsonObject> UnitObject =
				UnitValue.IsValid() && UnitValue->Type == EJson::Object ? UnitValue->AsObject() : nullptr;
			const FString Path = FString::Printf(TEXT("/units/%d"), Index);
            if (!UnitObject) {
                OutError = MakeValueFailure(TEXT("apply_workspace_contract_object_required"), Path, StringValue(TEXT("object")), UnitValue, TEXT("contract"), TEXT("This contract array entry must be an object."));
                return false;
            }
			if (!CheckAllowedFields(UnitObject, {
				TEXT("unit_id"), TEXT("path_pattern"), TEXT("role"), TEXT("cardinality"), TEXT("required"),
				TEXT("route_id"), TEXT("apply_contract_ref"), TEXT("merge_policy"),
				TEXT("absent_semantics"), TEXT("empty_semantics") }, Path, OutError)) return false;
			FUnitDescriptor Unit;
			if (!ReadString(UnitObject, TEXT("unit_id"), Path, Unit.UnitId, OutError)
				|| !ReadString(UnitObject, TEXT("path_pattern"), Path, Unit.PathPattern, OutError)
				|| !ReadString(UnitObject, TEXT("role"), Path, Unit.Role, OutError)
				|| !ReadString(UnitObject, TEXT("cardinality"), Path, Unit.Cardinality, OutError)
				|| !ReadBool(UnitObject, TEXT("required"), Path, Unit.bRequired, OutError)
				|| !ReadString(UnitObject, TEXT("route_id"), Path, Unit.RouteId, OutError)
				|| !ReadString(UnitObject, TEXT("apply_contract_ref"), Path, Unit.ApplyContractRef, OutError)
				|| !ReadString(UnitObject, TEXT("merge_policy"), Path, Unit.MergePolicy, OutError)
				|| !ReadString(UnitObject, TEXT("absent_semantics"), Path, Unit.AbsentSemantics, OutError)
				|| !ReadString(UnitObject, TEXT("empty_semantics"), Path, Unit.EmptySemantics, OutError)) return false;
			Contract.Units.Add(MoveTemp(Unit));
		}

		TSharedPtr<FJsonObject> ReferenceObject;
		if (!ReadObject(Object, TEXT("reference_policy"), FString(), ReferenceObject, OutError)
			|| !CheckAllowedFields(ReferenceObject, { TEXT("base"), TEXT("root_containment"), TEXT("external_references"), TEXT("case_collision") }, TEXT("/reference_policy"), OutError)
			|| !ReadString(ReferenceObject, TEXT("base"), TEXT("/reference_policy"), Contract.ReferencePolicy.Base, OutError)
			|| !ReadString(ReferenceObject, TEXT("root_containment"), TEXT("/reference_policy"), Contract.ReferencePolicy.RootContainment, OutError)
			|| !ReadString(ReferenceObject, TEXT("external_references"), TEXT("/reference_policy"), Contract.ReferencePolicy.ExternalReferences, OutError)
			|| !ReadString(ReferenceObject, TEXT("case_collision"), TEXT("/reference_policy"), Contract.ReferencePolicy.CaseCollision, OutError)) return false;

		TSharedPtr<FJsonObject> DependencyObject;
		if (!ReadObject(Object, TEXT("dependency_policy"), FString(), DependencyObject, OutError)
			|| !CheckAllowedFields(DependencyObject, { TEXT("edge_source"), TEXT("cycle_policy"), TEXT("unresolved_policy"), TEXT("order") }, TEXT("/dependency_policy"), OutError)
			|| !ReadString(DependencyObject, TEXT("edge_source"), TEXT("/dependency_policy"), Contract.DependencyPolicy.EdgeSource, OutError)
			|| !ReadString(DependencyObject, TEXT("cycle_policy"), TEXT("/dependency_policy"), Contract.DependencyPolicy.CyclePolicy, OutError)
			|| !ReadString(DependencyObject, TEXT("unresolved_policy"), TEXT("/dependency_policy"), Contract.DependencyPolicy.UnresolvedPolicy, OutError)
			|| !ReadString(DependencyObject, TEXT("order"), TEXT("/dependency_policy"), Contract.DependencyPolicy.Order, OutError)) return false;

		TSharedPtr<FJsonObject> PreflightObject;
		if (!ReadObject(Object, TEXT("preflight"), FString(), PreflightObject, OutError)
			|| !CheckAllowedFields(PreflightObject, { TEXT("scope"), TEXT("mutation_before_success"), TEXT("diagnostics_include_source") }, TEXT("/preflight"), OutError)
			|| !ReadString(PreflightObject, TEXT("scope"), TEXT("/preflight"), Contract.Preflight.Scope, OutError)
			|| !ReadString(PreflightObject, TEXT("mutation_before_success"), TEXT("/preflight"), Contract.Preflight.MutationBeforeSuccess, OutError)
			|| !ReadBool(PreflightObject, TEXT("diagnostics_include_source"), TEXT("/preflight"), Contract.Preflight.bDiagnosticsIncludeSource, OutError)) return false;

		TSharedPtr<FJsonObject> TransactionObject;
		if (!ReadObject(Object, TEXT("transaction"), FString(), TransactionObject, OutError)
			|| !CheckAllowedFields(TransactionObject, { TEXT("mode"), TEXT("rollback_on") }, TEXT("/transaction"), OutError)
			|| !ReadString(TransactionObject, TEXT("mode"), TEXT("/transaction"), Contract.Transaction.Mode, OutError)
			|| !ReadStringArray(TransactionObject, TEXT("rollback_on"), TEXT("/transaction"), Contract.Transaction.RollbackOn, OutError)) return false;

		TSharedPtr<FJsonObject> ExecutionObject;
		if (!ReadObject(Object, TEXT("execution"), FString(), ExecutionObject, OutError)
			|| !CheckAllowedFields(ExecutionObject, { TEXT("compile"), TEXT("save"), TEXT("verification") }, TEXT("/execution"), OutError)
			|| !ReadString(ExecutionObject, TEXT("compile"), TEXT("/execution"), Contract.Execution.Compile, OutError)
			|| !ReadString(ExecutionObject, TEXT("save"), TEXT("/execution"), Contract.Execution.Save, OutError)
			|| !ReadString(ExecutionObject, TEXT("verification"), TEXT("/execution"), Contract.Execution.Verification, OutError)) return false;

		if (!ReadString(Object, TEXT("unknown_file_policy"), FString(), Contract.UnknownFilePolicy, OutError)
			|| !ReadString(Object, TEXT("duplicate_target_policy"), FString(), Contract.DuplicateTargetPolicy, OutError)) return false;

		if (!ValidateContract(Contract, OutError)) return false;
		OutContract = CanonicalizeContract(Contract);
		return true;
	}
}
