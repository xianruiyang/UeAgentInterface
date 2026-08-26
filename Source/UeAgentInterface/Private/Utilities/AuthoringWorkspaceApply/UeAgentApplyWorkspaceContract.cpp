// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringWorkspaceApply/UeAgentApplyWorkspaceContract.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Internationalization/Regex.h"
#include "Misc/Paths.h"
#include "Misc/ScopeRWLock.h"

namespace
{
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
		FString& OutError)
	{
		if (!Object.IsValid())
		{
			OutError = FString::Printf(TEXT("apply_workspace_contract_object_required:%s"), *Path);
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
				OutError = FString::Printf(TEXT("apply_workspace_contract_unknown_field:%s/%s"), *Path, *Field);
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
			OutError = FString::Printf(TEXT("apply_workspace_contract_string_required:%s/%s"), *Path, Field);
			return false;
		}
		return true;
	}

	bool ReadBool(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		const FString& Path,
		bool& bOutValue,
		FString& OutError)
	{
		if (!Object.IsValid() || !Object->TryGetBoolField(Field, bOutValue))
		{
			OutError = FString::Printf(TEXT("apply_workspace_contract_bool_required:%s/%s"), *Path, Field);
			return false;
		}
		return true;
	}

	bool ReadObject(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		const FString& Path,
		TSharedPtr<FJsonObject>& OutValue,
		FString& OutError)
	{
		const TSharedPtr<FJsonObject>* Value = nullptr;
		if (!Object.IsValid() || !Object->TryGetObjectField(Field, Value) || !Value || !Value->IsValid())
		{
			OutError = FString::Printf(TEXT("apply_workspace_contract_object_required:%s/%s"), *Path, Field);
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
		FString& OutError)
	{
		OutValues = nullptr;
		if (!Object.IsValid() || !Object->TryGetArrayField(Field, OutValues) || !OutValues)
		{
			OutError = FString::Printf(TEXT("apply_workspace_contract_array_required:%s/%s"), *Path, Field);
			return false;
		}
		return true;
	}

	bool ReadStringArray(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		const FString& Path,
		TArray<FString>& OutValues,
		FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!ReadArray(Object, Field, Path, Values, OutError)) return false;
		OutValues.Reset();
		for (int32 Index = 0; Index < Values->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& Value = (*Values)[Index];
			if (!Value.IsValid() || Value->Type != EJson::String || Value->AsString().IsEmpty())
			{
				OutError = FString::Printf(TEXT("apply_workspace_contract_string_required:%s/%s/%d"), *Path, Field, Index);
				return false;
			}
			OutValues.Add(Value->AsString());
		}
		return true;
	}

	bool HasDuplicateStrings(const TArray<FString>& Values)
	{
		TSet<FString> Seen;
		for (const FString& Value : Values)
		{
			if (Seen.Contains(Value)) return true;
			Seen.Add(Value);
		}
		return false;
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
		return Kind == Other.Kind && EntryMode == Other.EntryMode && EntryRole == Other.EntryRole;
	}

	bool FEntryDescriptor::operator==(const FEntryDescriptor& Other) const
	{
		return DirectoryEntry == Other.DirectoryEntry && VirtualUnitId == Other.VirtualUnitId;
	}

	bool FUnitDescriptor::operator==(const FUnitDescriptor& Other) const
	{
		return UnitId == Other.UnitId
			&& PathPattern == Other.PathPattern
			&& Role == Other.Role
			&& Cardinality == Other.Cardinality
			&& bRequired == Other.bRequired
			&& RouteId == Other.RouteId
			&& ApplyContractRef == Other.ApplyContractRef
			&& MergePolicy == Other.MergePolicy
			&& AbsentSemantics == Other.AbsentSemantics
			&& EmptySemantics == Other.EmptySemantics;
	}

	bool FReferencePolicy::operator==(const FReferencePolicy& Other) const
	{
		return Base == Other.Base
			&& RootContainment == Other.RootContainment
			&& ExternalReferences == Other.ExternalReferences
			&& CaseCollision == Other.CaseCollision;
	}

	bool FDependencyPolicy::operator==(const FDependencyPolicy& Other) const
	{
		return EdgeSource == Other.EdgeSource
			&& CyclePolicy == Other.CyclePolicy
			&& UnresolvedPolicy == Other.UnresolvedPolicy
			&& Order == Other.Order;
	}

	bool FPreflightPolicy::operator==(const FPreflightPolicy& Other) const
	{
		return Scope == Other.Scope
			&& MutationBeforeSuccess == Other.MutationBeforeSuccess
			&& bDiagnosticsIncludeSource == Other.bDiagnosticsIncludeSource;
	}

	bool FTransactionPolicy::operator==(const FTransactionPolicy& Other) const
	{
		return Mode == Other.Mode && RollbackOn == Other.RollbackOn;
	}

	bool FExecutionPolicy::operator==(const FExecutionPolicy& Other) const
	{
		return Compile == Other.Compile && Save == Other.Save && Verification == Other.Verification;
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
		FString Error;
		return ValidateContract(*this, Error);
	}

	bool FContract::operator==(const FContract& Other) const
	{
		return Schema == Other.Schema
			&& ContractId == Other.ContractId
			&& ContractVersion == Other.ContractVersion
			&& ProfileId == Other.ProfileId
			&& Sources == Other.Sources
			&& Entry == Other.Entry
			&& Units == Other.Units
			&& ReferencePolicy == Other.ReferencePolicy
			&& DependencyPolicy == Other.DependencyPolicy
			&& Preflight == Other.Preflight
			&& Transaction == Other.Transaction
			&& Execution == Other.Execution
			&& UnknownFilePolicy == Other.UnknownFilePolicy
			&& DuplicateTargetPolicy == Other.DuplicateTargetPolicy;
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
		OutResult = ERegistrationResult::Registered;
		OutError.Reset();
		FString ValidationError;
		if (!ValidateContract(Contract, ValidationError))
		{
			OutError = ValidationError;
			return false;
		}

		const FContract CanonicalContract = CanonicalizeContract(Contract);
		FWriteScopeLock Lock(RegistryLock);
		if (const FContract* Existing = Contracts.Find(CanonicalContract.ContractId))
		{
			if (*Existing == CanonicalContract)
			{
				OutResult = ERegistrationResult::AlreadyRegistered;
				return true;
			}
			OutError = FString::Printf(TEXT("apply_workspace_contract_conflict:%s"), *CanonicalContract.ContractId);
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

	bool ValidateContract(const FContract& Contract, FString& OutError)
	{
		OutError.Reset();
		if (Contract.Schema != TEXT("uai.apply_workspace_contract.v1"))
		{
			OutError = TEXT("apply_workspace_contract_schema_invalid");
			return false;
		}
		if (!MatchesPattern(Contract.ContractId, TEXT("^uai\\.apply_workspace\\.[a-z0-9_.-]+$")))
		{
			OutError = FString::Printf(TEXT("apply_workspace_contract_id_invalid:%s"), *Contract.ContractId);
			return false;
		}
		if (!MatchesPattern(Contract.ContractVersion, TEXT("^[0-9]+\\.[0-9]+\\.[0-9]+$")))
		{
			OutError = FString::Printf(TEXT("apply_workspace_contract_version_invalid:%s"), *Contract.ContractVersion);
			return false;
		}
		if (!MatchesPattern(Contract.ProfileId, TEXT("^[a-z][a-z0-9_.-]+$")))
		{
			OutError = FString::Printf(TEXT("apply_workspace_contract_profile_invalid:%s"), *Contract.ProfileId);
			return false;
		}
		if (Contract.Sources.Num() != 3)
		{
			OutError = TEXT("apply_workspace_contract_sources_required:directory,single_file,inline");
			return false;
		}
		TSet<ESourceKind> SeenSources;
		for (const FSourceDescriptor& Source : Contract.Sources)
		{
			if (SeenSources.Contains(Source.Kind))
			{
				OutError = FString::Printf(TEXT("apply_workspace_contract_source_duplicate:%s"), *SourceKindToString(Source.Kind));
				return false;
			}
			SeenSources.Add(Source.Kind);
			const bool bEntryModeValid = Source.Kind == ESourceKind::Directory
				? Source.EntryMode == TEXT("physical")
				: IsOneOf(Source.EntryMode, { TEXT("virtual"), TEXT("virtual_bundle") });
			if (!bEntryModeValid
				|| !IsOneOf(Source.EntryRole, { TEXT("index"), TEXT("object"), TEXT("object_list"), TEXT("control") }))
			{
				OutError = FString::Printf(TEXT("apply_workspace_contract_source_invalid:%s"), *SourceKindToString(Source.Kind));
				return false;
			}
		}
		if (Contract.Entry.DirectoryEntry.IsEmpty()
			|| Contract.Entry.DirectoryEntry.Contains(TEXT("\\"))
			|| !FPaths::IsRelative(Contract.Entry.DirectoryEntry)
			|| !Contract.Entry.DirectoryEntry.EndsWith(TEXT(".hjson"), ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(TEXT("apply_workspace_contract_directory_entry_invalid:%s"), *Contract.Entry.DirectoryEntry);
			return false;
		}
		if (Contract.Units.IsEmpty())
		{
			OutError = TEXT("apply_workspace_contract_units_required");
			return false;
		}
		TSet<FString> SeenUnitIds;
		for (const FUnitDescriptor& Unit : Contract.Units)
		{
			if (!MatchesPattern(Unit.UnitId, TEXT("^[a-z][a-z0-9_.-]+$"))
				|| SeenUnitIds.Contains(Unit.UnitId)
				|| Unit.PathPattern.IsEmpty()
				|| Unit.RouteId.IsEmpty()
				|| !MatchesPattern(Unit.ApplyContractRef, TEXT("^uai\\.apply_input\\.[a-z0-9_.-]+$"))
				|| !IsOneOf(Unit.Role, { TEXT("identity"), TEXT("index"), TEXT("settings"), TEXT("object"), TEXT("object_list"), TEXT("details"), TEXT("graph"), TEXT("layout"), TEXT("connections"), TEXT("reference"), TEXT("control") })
				|| !IsOneOf(Unit.Cardinality, { TEXT("one"), TEXT("zero_or_one"), TEXT("zero_or_more"), TEXT("one_or_more") })
				|| !IsOneOf(Unit.MergePolicy, { TEXT("replace"), TEXT("merge"), TEXT("append"), TEXT("reject_duplicate") })
				|| !IsOneOf(Unit.AbsentSemantics, { TEXT("no_change"), TEXT("error"), TEXT("empty_value") })
				|| !IsOneOf(Unit.EmptySemantics, { TEXT("explicit_empty"), TEXT("remove"), TEXT("no_change"), TEXT("error") }))
			{
				OutError = FString::Printf(TEXT("apply_workspace_contract_unit_invalid:%s"), *Unit.UnitId);
				return false;
			}
			SeenUnitIds.Add(Unit.UnitId);
		}
		if (!Contract.FindUnit(Contract.Entry.VirtualUnitId))
		{
			OutError = FString::Printf(TEXT("apply_workspace_contract_virtual_unit_missing:%s"), *Contract.Entry.VirtualUnitId);
			return false;
		}
		if (Contract.ReferencePolicy.ExternalReferences == TEXT("contract_allowlist"))
		{
			OutError = TEXT("apply_workspace_external_reference_policy_unimplemented:contract_allowlist");
			return false;
		}
		if (Contract.ReferencePolicy.Base != TEXT("workspace_root")
			|| Contract.ReferencePolicy.RootContainment != TEXT("required")
			|| Contract.ReferencePolicy.ExternalReferences != TEXT("reject")
			|| Contract.ReferencePolicy.CaseCollision != TEXT("reject"))
		{
			OutError = TEXT("apply_workspace_contract_reference_policy_invalid");
			return false;
		}
		if (Contract.DependencyPolicy.EdgeSource != TEXT("contract_and_references")
			|| Contract.DependencyPolicy.CyclePolicy != TEXT("reject")
			|| Contract.DependencyPolicy.UnresolvedPolicy != TEXT("reject")
			|| Contract.DependencyPolicy.Order != TEXT("stable_topological"))
		{
			OutError = TEXT("apply_workspace_contract_dependency_policy_invalid");
			return false;
		}
		if (Contract.Preflight.Scope != TEXT("whole_workspace")
			|| Contract.Preflight.MutationBeforeSuccess != TEXT("forbid")
			|| !Contract.Preflight.bDiagnosticsIncludeSource)
		{
			OutError = TEXT("apply_workspace_contract_preflight_policy_invalid");
			return false;
		}
		if (Contract.Transaction.Mode != TEXT("atomic")
			|| Contract.Transaction.RollbackOn.Num() != 4
			|| HasDuplicateStrings(Contract.Transaction.RollbackOn))
		{
			OutError = TEXT("apply_workspace_contract_transaction_policy_invalid");
			return false;
		}
		for (const FString& Trigger : Contract.Transaction.RollbackOn)
		{
			if (!IsOneOf(Trigger, { TEXT("mutation_failed"), TEXT("compile_failed"), TEXT("save_failed"), TEXT("verification_failed") }))
			{
				OutError = FString::Printf(TEXT("apply_workspace_contract_rollback_trigger_invalid:%s"), *Trigger);
				return false;
			}
		}
		if (!IsOneOf(Contract.Execution.Compile, {
				TEXT("provider_hook"),
				TEXT("provider_hook_required"),
				TEXT("none") })
			|| !IsOneOf(Contract.Execution.Save, {
				TEXT("caller_controlled"),
				TEXT("caller_controlled_required_default"),
				TEXT("required"),
				TEXT("none") })
			|| Contract.Execution.Verification != TEXT("shared_snapshot_required"))
		{
			OutError = TEXT("apply_workspace_contract_execution_policy_invalid");
			return false;
		}
		if (Contract.UnknownFilePolicy != TEXT("reject") || Contract.DuplicateTargetPolicy != TEXT("reject"))
		{
			OutError = TEXT("apply_workspace_contract_rejection_policy_invalid");
			return false;
		}
		return true;
	}

	bool ParseContract(
		const TSharedPtr<FJsonObject>& Object,
		FContract& OutContract,
		FString& OutError)
	{
		OutContract = FContract();
		OutError.Reset();
		if (!CheckAllowedFields(Object, {
			TEXT("schema"), TEXT("contract_id"), TEXT("contract_version"), TEXT("profile_id"),
			TEXT("sources"), TEXT("entry"), TEXT("units"), TEXT("reference_policy"),
			TEXT("dependency_policy"), TEXT("preflight"), TEXT("transaction"), TEXT("execution"),
			TEXT("unknown_file_policy"), TEXT("duplicate_target_policy") }, TEXT("$"), OutError)) return false;

		FContract Contract;
		if (!ReadString(Object, TEXT("schema"), TEXT("$"), Contract.Schema, OutError)
			|| !ReadString(Object, TEXT("contract_id"), TEXT("$"), Contract.ContractId, OutError)
			|| !ReadString(Object, TEXT("contract_version"), TEXT("$"), Contract.ContractVersion, OutError)
			|| !ReadString(Object, TEXT("profile_id"), TEXT("$"), Contract.ProfileId, OutError)) return false;

		const TArray<TSharedPtr<FJsonValue>>* SourceValues = nullptr;
		if (!ReadArray(Object, TEXT("sources"), TEXT("$"), SourceValues, OutError)) return false;
		for (int32 Index = 0; Index < SourceValues->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& SourceValue = (*SourceValues)[Index];
			const TSharedPtr<FJsonObject> SourceObject =
				SourceValue.IsValid() && SourceValue->Type == EJson::Object ? SourceValue->AsObject() : nullptr;
			const FString Path = FString::Printf(TEXT("$/sources/%d"), Index);
			if (!CheckAllowedFields(SourceObject, { TEXT("kind"), TEXT("entry_mode"), TEXT("entry_role") }, Path, OutError)) return false;
			FSourceDescriptor Source;
			FString KindText;
			if (!ReadString(SourceObject, TEXT("kind"), Path, KindText, OutError)
				|| !ParseSourceKind(KindText, Source.Kind)
				|| !ReadString(SourceObject, TEXT("entry_mode"), Path, Source.EntryMode, OutError)
				|| !ReadString(SourceObject, TEXT("entry_role"), Path, Source.EntryRole, OutError))
			{
				if (OutError.IsEmpty()) OutError = FString::Printf(TEXT("apply_workspace_contract_source_invalid:%s"), *Path);
				return false;
			}
			Contract.Sources.Add(MoveTemp(Source));
		}
		TSharedPtr<FJsonObject> EntryObject;
		if (!ReadObject(Object, TEXT("entry"), TEXT("$"), EntryObject, OutError)
			|| !CheckAllowedFields(EntryObject, { TEXT("directory_entry"), TEXT("virtual_unit_id") }, TEXT("$/entry"), OutError)
			|| !ReadString(EntryObject, TEXT("directory_entry"), TEXT("$/entry"), Contract.Entry.DirectoryEntry, OutError)
			|| !ReadString(EntryObject, TEXT("virtual_unit_id"), TEXT("$/entry"), Contract.Entry.VirtualUnitId, OutError)) return false;
		const TArray<TSharedPtr<FJsonValue>>* UnitValues = nullptr;
		if (!ReadArray(Object, TEXT("units"), TEXT("$"), UnitValues, OutError)) return false;
		for (int32 Index = 0; Index < UnitValues->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& UnitValue = (*UnitValues)[Index];
			const TSharedPtr<FJsonObject> UnitObject =
				UnitValue.IsValid() && UnitValue->Type == EJson::Object ? UnitValue->AsObject() : nullptr;
			const FString Path = FString::Printf(TEXT("$/units/%d"), Index);
			if (!CheckAllowedFields(UnitObject, {
				TEXT("unit_id"), TEXT("path_pattern"), TEXT("role"), TEXT("cardinality"), TEXT("required"),
				TEXT("route_id"), TEXT("apply_contract_ref"), TEXT("merge_policy"),
				TEXT("absent_semantics"), TEXT("empty_semantics") }, Path, OutError)) return false;
			FUnitDescriptor Unit;
			if (!ReadString(UnitObject, TEXT("unit_id"), Path, Unit.UnitId, OutError)
				|| !MatchesPattern(Unit.UnitId, TEXT("^[a-z][a-z0-9_.-]+$"))
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
		if (!ReadObject(Object, TEXT("reference_policy"), TEXT("$"), ReferenceObject, OutError)
			|| !CheckAllowedFields(ReferenceObject, { TEXT("base"), TEXT("root_containment"), TEXT("external_references"), TEXT("case_collision") }, TEXT("$/reference_policy"), OutError)
			|| !ReadString(ReferenceObject, TEXT("base"), TEXT("$/reference_policy"), Contract.ReferencePolicy.Base, OutError)
			|| !ReadString(ReferenceObject, TEXT("root_containment"), TEXT("$/reference_policy"), Contract.ReferencePolicy.RootContainment, OutError)
			|| !ReadString(ReferenceObject, TEXT("external_references"), TEXT("$/reference_policy"), Contract.ReferencePolicy.ExternalReferences, OutError)
			|| !ReadString(ReferenceObject, TEXT("case_collision"), TEXT("$/reference_policy"), Contract.ReferencePolicy.CaseCollision, OutError)) return false;

		TSharedPtr<FJsonObject> DependencyObject;
		if (!ReadObject(Object, TEXT("dependency_policy"), TEXT("$"), DependencyObject, OutError)
			|| !CheckAllowedFields(DependencyObject, { TEXT("edge_source"), TEXT("cycle_policy"), TEXT("unresolved_policy"), TEXT("order") }, TEXT("$/dependency_policy"), OutError)
			|| !ReadString(DependencyObject, TEXT("edge_source"), TEXT("$/dependency_policy"), Contract.DependencyPolicy.EdgeSource, OutError)
			|| !ReadString(DependencyObject, TEXT("cycle_policy"), TEXT("$/dependency_policy"), Contract.DependencyPolicy.CyclePolicy, OutError)
			|| !ReadString(DependencyObject, TEXT("unresolved_policy"), TEXT("$/dependency_policy"), Contract.DependencyPolicy.UnresolvedPolicy, OutError)
			|| !ReadString(DependencyObject, TEXT("order"), TEXT("$/dependency_policy"), Contract.DependencyPolicy.Order, OutError)) return false;

		TSharedPtr<FJsonObject> PreflightObject;
		if (!ReadObject(Object, TEXT("preflight"), TEXT("$"), PreflightObject, OutError)
			|| !CheckAllowedFields(PreflightObject, { TEXT("scope"), TEXT("mutation_before_success"), TEXT("diagnostics_include_source") }, TEXT("$/preflight"), OutError)
			|| !ReadString(PreflightObject, TEXT("scope"), TEXT("$/preflight"), Contract.Preflight.Scope, OutError)
			|| !ReadString(PreflightObject, TEXT("mutation_before_success"), TEXT("$/preflight"), Contract.Preflight.MutationBeforeSuccess, OutError)
			|| !ReadBool(PreflightObject, TEXT("diagnostics_include_source"), TEXT("$/preflight"), Contract.Preflight.bDiagnosticsIncludeSource, OutError)) return false;

		TSharedPtr<FJsonObject> TransactionObject;
		if (!ReadObject(Object, TEXT("transaction"), TEXT("$"), TransactionObject, OutError)
			|| !CheckAllowedFields(TransactionObject, { TEXT("mode"), TEXT("rollback_on") }, TEXT("$/transaction"), OutError)
			|| !ReadString(TransactionObject, TEXT("mode"), TEXT("$/transaction"), Contract.Transaction.Mode, OutError)
			|| !ReadStringArray(TransactionObject, TEXT("rollback_on"), TEXT("$/transaction"), Contract.Transaction.RollbackOn, OutError)) return false;

		TSharedPtr<FJsonObject> ExecutionObject;
		if (!ReadObject(Object, TEXT("execution"), TEXT("$"), ExecutionObject, OutError)
			|| !CheckAllowedFields(ExecutionObject, { TEXT("compile"), TEXT("save"), TEXT("verification") }, TEXT("$/execution"), OutError)
			|| !ReadString(ExecutionObject, TEXT("compile"), TEXT("$/execution"), Contract.Execution.Compile, OutError)
			|| !ReadString(ExecutionObject, TEXT("save"), TEXT("$/execution"), Contract.Execution.Save, OutError)
			|| !ReadString(ExecutionObject, TEXT("verification"), TEXT("$/execution"), Contract.Execution.Verification, OutError)) return false;

		if (!ReadString(Object, TEXT("unknown_file_policy"), TEXT("$"), Contract.UnknownFilePolicy, OutError)
			|| !ReadString(Object, TEXT("duplicate_target_policy"), TEXT("$"), Contract.DuplicateTargetPolicy, OutError)) return false;

		if (!ValidateContract(Contract, OutError)) return false;
		OutContract = CanonicalizeContract(Contract);
		return true;
	}
}
