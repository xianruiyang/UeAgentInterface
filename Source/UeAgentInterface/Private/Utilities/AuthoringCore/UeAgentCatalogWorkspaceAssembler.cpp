// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringCore/UeAgentCatalogWorkspaceAssembler.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDomainRegistry.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringRegistrySnapshot.h"
#include "Utilities/AuthoringOperationApply/UeAgentApplyInputContract.h"

namespace UeAgentCatalogWorkspaceAssembler
{
namespace
{
	using UeAgentAuthoringDomainRegistry::FCatalogBinding;

	bool Fail(FBuildFailure& Failure, const FString& Code, const FString& Detail = FString())
	{
		Failure.ErrorCode = Code;
		Failure.ErrorDetail = Detail.IsEmpty() ? Code : Detail;
		return false;
	}

	bool ReadNonEmptyString(const TSharedPtr<FJsonObject>& Object, const FString& Field, FString& OutValue)
	{
		OutValue.Reset();
		return Object.IsValid() && Object->TryGetStringField(Field, OutValue)
			&& !OutValue.TrimStartAndEnd().IsEmpty();
	}

	bool StringMapsEqual(const TMap<FString, FString>& Left, const TMap<FString, FString>& Right)
	{
		if (Left.Num() != Right.Num()) return false;
		for (const TPair<FString, FString>& Pair : Left)
		{
			const FString* Value = Right.Find(Pair.Key);
			if (!Value || *Value != Pair.Value) return false;
		}
		return true;
	}

	FString UnescapePointerSegment(FString Value)
	{
		Value.ReplaceInline(TEXT("~1"), TEXT("/"));
		Value.ReplaceInline(TEXT("~0"), TEXT("~"));
		return Value;
	}

	bool SplitPointer(const FString& Pointer, TArray<FString>& OutSegments)
	{
		OutSegments.Reset();
		if (!Pointer.StartsWith(TEXT("/"))) return false;
		Pointer.Mid(1).ParseIntoArray(OutSegments, TEXT("/"), false);
		for (FString& Segment : OutSegments) Segment = UnescapePointerSegment(MoveTemp(Segment));
		return !OutSegments.IsEmpty() && !OutSegments.Contains(FString());
	}

	bool SetPointerValue(
		const TSharedRef<FJsonObject>& Root,
		const FString& Pointer,
		const TSharedPtr<FJsonValue>& Value,
		FBuildFailure& Failure)
	{
		TArray<FString> Segments;
		if (!SplitPointer(Pointer, Segments) || !Value.IsValid())
		{
			return Fail(Failure, TEXT("catalog_pointer_invalid"), Pointer);
		}
		TSharedPtr<FJsonObject> Current = Root;
		for (int32 Index = 0; Index + 1 < Segments.Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* Existing = nullptr;
			if (Current->TryGetObjectField(Segments[Index], Existing) && Existing && Existing->IsValid())
			{
				Current = *Existing;
				continue;
			}
			if (Current->HasField(Segments[Index]))
			{
				return Fail(Failure, TEXT("catalog_pointer_type_conflict"), Pointer);
			}
			TSharedRef<FJsonObject> Child = MakeShared<FJsonObject>();
			Current->SetObjectField(Segments[Index], Child);
			Current = Child;
		}
		Current->SetField(Segments.Last(), Value);
		return true;
	}

	bool EnsurePointerObject(
		const TSharedRef<FJsonObject>& Root,
		const FString& Pointer,
		FBuildFailure& Failure)
	{
		TArray<FString> Segments;
		if (!SplitPointer(Pointer, Segments)) return Fail(Failure, TEXT("catalog_pointer_invalid"), Pointer);
		TSharedPtr<FJsonObject> Current = Root;
		for (const FString& Segment : Segments)
		{
			const TSharedPtr<FJsonObject>* Existing = nullptr;
			if (Current->TryGetObjectField(Segment, Existing) && Existing && Existing->IsValid())
			{
				Current = *Existing;
				continue;
			}
			if (Current->HasField(Segment)) return Fail(Failure, TEXT("catalog_pointer_type_conflict"), Pointer);
			TSharedRef<FJsonObject> Child = MakeShared<FJsonObject>();
			Current->SetObjectField(Segment, Child);
			Current = Child;
		}
		return true;
	}

	bool ResolveTemplate(
		const FString& Template,
		const TSharedPtr<FJsonObject>& Seed,
		const TSharedPtr<FJsonObject>& Context,
		FString& OutValue,
		FString& OutMissing)
	{
		OutValue = Template;
		OutMissing.Reset();
		int32 SearchFrom = 0;
		while (true)
		{
			const int32 Open = OutValue.Find(TEXT("{"), ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
			if (Open == INDEX_NONE) return true;
			const int32 Close = OutValue.Find(TEXT("}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Open + 1);
			if (Close == INDEX_NONE)
			{
				OutMissing = OutValue.Mid(Open + 1);
				return false;
			}
			const FString Field = OutValue.Mid(Open + 1, Close - Open - 1);
			FString Replacement;
			if (!ReadNonEmptyString(Seed, Field, Replacement)
				&& !ReadNonEmptyString(Context, Field, Replacement))
			{
				OutMissing = Field;
				return false;
			}
			OutValue = OutValue.Left(Open) + Replacement + OutValue.Mid(Close + 1);
			SearchFrom = Open + Replacement.Len();
		}
	}

	bool ResolvePointer(
		const FString& Template,
		const FBuildRequest& Request,
		FString& OutPointer,
		FBuildFailure& Failure)
	{
		FString Missing;
		if (!ResolveTemplate(Template, Request.Seed, Request.Context, OutPointer, Missing))
		{
			return Fail(Failure, TEXT("catalog_pointer_binding_unresolved"), Missing);
		}
		return true;
	}

	bool ReadStringMap(
		const TSharedPtr<FJsonObject>& Object,
		const FString& Field,
		TMap<FString, FString>& OutValues)
	{
		OutValues.Reset();
		const TSharedPtr<FJsonObject>* Map = nullptr;
		if (!Object.IsValid() || !Object->TryGetObjectField(Field, Map) || !Map || !Map->IsValid()) return false;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Map)->Values)
		{
			FString Value;
			if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(Value)) return false;
			OutValues.Add(Pair.Key, MoveTemp(Value));
		}
		return true;
	}

	bool PlacementMatchesBinding(const FBuildRequest& Request, const FCatalogBinding& Binding)
	{
		FString Value;
		TMap<FString, FString> Map;
		return ReadNonEmptyString(Request.Placement, TEXT("domain_id"), Value) && Value == Binding.DomainId
			&& ReadNonEmptyString(Request.Placement, TEXT("workspace_contract_id"), Value) && Value == Binding.WorkspaceContractId
			&& ReadNonEmptyString(Request.Placement, TEXT("input_contract_id"), Value) && Value == Binding.InputContractId
			&& ReadNonEmptyString(Request.Placement, TEXT("unit_id"), Value) && Value == Binding.UnitId
			&& ReadNonEmptyString(Request.Placement, TEXT("path_pattern"), Value) && Value == Binding.PathPattern
			&& ReadNonEmptyString(Request.Placement, TEXT("relative_path_template"), Value) && Value == Binding.RelativePathTemplate
			&& ReadNonEmptyString(Request.Placement, TEXT("pointer_template"), Value) && Value == Binding.PointerTemplate
			&& ReadNonEmptyString(Request.Placement, TEXT("merge_mode"), Value) && Value == Binding.MergeMode
			&& ReadStringMap(Request.Placement, TEXT("operation_map"), Map) && StringMapsEqual(Map, Binding.OperationMap)
			&& ReadStringMap(Request.Placement, TEXT("seed_bindings"), Map) && StringMapsEqual(Map, Binding.SeedBindings)
			&& ReadStringMap(Request.Placement, TEXT("context_bindings"), Map) && StringMapsEqual(Map, Binding.ContextBindings)
			&& ReadStringMap(Request.Placement, TEXT("context_value_aliases"), Map) && StringMapsEqual(Map, Binding.ContextValueAliases)
			&& ReadStringMap(Request.Placement, TEXT("constant_bindings"), Map) && StringMapsEqual(Map, Binding.ConstantBindings);
	}

	FString WorkspaceMergePolicy(const FString& MergeMode)
	{
		return MergeMode == TEXT("merge_object") ? TEXT("replace") : MergeMode;
	}
}

bool BuildVirtualWorkspace(
	const FBuildRequest& Request,
	FBuildResult& OutResult,
	FBuildFailure& OutFailure)
{
	OutResult = FBuildResult();
	OutFailure = FBuildFailure();
	FString Value;
	if (!ReadNonEmptyString(Request.Placement, TEXT("schema"), Value)
		|| Value != TEXT("uai.catalog_placement.v2"))
	{
		return Fail(OutFailure, TEXT("catalog_placement_schema_invalid"), Value);
	}
	FString Kind;
	if (!ReadNonEmptyString(Request.Placement, TEXT("kind"), Kind))
	{
		return Fail(OutFailure, TEXT("catalog_placement_kind_missing"));
	}
	FCatalogBinding Binding;
	if (!UeAgentAuthoringDomainRegistry::FRegistry::Get().FindCatalogBinding(Kind, Binding))
	{
		return Fail(OutFailure, TEXT("catalog_binding_not_registered"), Kind);
	}
	if (!Request.Seed.IsValid()) return Fail(OutFailure, TEXT("catalog_seed_invalid"));
	if (!Request.Context.IsValid()) return Fail(OutFailure, TEXT("seed_context_required"));

	for (const FString& Field : Binding.RequiredContext)
	{
		if (!ReadNonEmptyString(Request.Context, Field, Value)) OutFailure.RequiredContext.Add(Field);
	}
	if (!OutFailure.RequiredContext.IsEmpty())
	{
		return Fail(OutFailure, TEXT("seed_context_required"), FString::Join(OutFailure.RequiredContext, TEXT(",")));
	}

	FString ResolvedPointer;
	if (!ResolvePointer(Request.Placement->GetStringField(TEXT("pointer_template")), Request, ResolvedPointer, OutFailure))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Snapshot;
	FString SnapshotError;
	if (!UeAgentAuthoringRegistrySnapshot::BuildSnapshot(Snapshot, SnapshotError) || !Snapshot.IsValid())
	{
		return Fail(OutFailure, TEXT("catalog_contract_snapshot_unavailable"), SnapshotError);
	}
	FString ContractHash;
	if (!ReadNonEmptyString(Request.Placement, TEXT("contract_hash"), ContractHash)
		|| ContractHash != Snapshot->GetStringField(TEXT("registry_hash")))
	{
		return Fail(OutFailure, TEXT("catalog_contract_stale"), ContractHash);
	}
	if (!PlacementMatchesBinding(Request, Binding))
	{
		return Fail(OutFailure, TEXT("catalog_placement_contract_mismatch"), Kind);
	}

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Request.Seed->Values)
	{
		if (!Binding.SeedBindings.Contains(Pair.Key))
		{
			return Fail(OutFailure, TEXT("catalog_seed_field_unknown"), Pair.Key);
		}
	}
	for (const FString& Field : Binding.RequiredSeedFields)
	{
		if (!ReadNonEmptyString(Request.Seed, Field, Value))
		{
			return Fail(OutFailure, TEXT("catalog_seed_field_required"), Field);
		}
	}

	TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
	const FString NormalizedOperation = Request.Operation.TrimStartAndEnd().ToLower();
	const FString* MappedOperation = Binding.OperationMap.Find(NormalizedOperation);
	Document->SetStringField(TEXT("operation"), MappedOperation ? *MappedOperation : NormalizedOperation);
	if (!EnsurePointerObject(Document, ResolvedPointer, OutFailure)) return false;
	for (const TPair<FString, FString>& Pair : Binding.ConstantBindings)
	{
		FString Pointer;
		if (!ResolvePointer(Pair.Key, Request, Pointer, OutFailure)
			|| !SetPointerValue(Document, Pointer, MakeShared<FJsonValueString>(Pair.Value), OutFailure)) return false;
	}
	for (const TPair<FString, FString>& Pair : Binding.ContextBindings)
	{
		TSharedPtr<FJsonValue> ContextValue = Request.Context->TryGetField(Pair.Key);
		if (!ContextValue.IsValid()) continue;
		FString ContextText;
		if (ContextValue->TryGetString(ContextText))
		{
			const FString AliasKey = Pair.Key + TEXT(":") + ContextText.TrimStartAndEnd().ToLower();
			if (const FString* AliasValue = Binding.ContextValueAliases.Find(AliasKey))
			{
				ContextValue = MakeShared<FJsonValueString>(*AliasValue);
			}
		}
		FString Pointer;
		if (!ResolvePointer(Pair.Value, Request, Pointer, OutFailure)
			|| !SetPointerValue(Document, Pointer, ContextValue, OutFailure)) return false;
	}
	for (const TPair<FString, FString>& Pair : Binding.SeedBindings)
	{
		const TSharedPtr<FJsonValue> SeedValue = Request.Seed->TryGetField(Pair.Key);
		if (!SeedValue.IsValid()) continue;
		FString Pointer;
		if (!ResolvePointer(Pair.Value, Request, Pointer, OutFailure)
			|| !SetPointerValue(Document, Pointer, SeedValue, OutFailure)) return false;
	}

	if (!UeAgentApplyWorkspaceContract::FRegistry::Get().FindContract(
		Binding.WorkspaceContractId, OutResult.WorkspaceContract))
	{
		return Fail(OutFailure, TEXT("catalog_workspace_contract_not_registered"), Binding.WorkspaceContractId);
	}
	UeAgentApplyInputContract::FContract InputContract;
	if (!UeAgentApplyInputContract::FRegistry::Get().FindContract(Binding.InputContractId, InputContract))
	{
		return Fail(OutFailure, TEXT("catalog_input_contract_not_registered"), Binding.InputContractId);
	}
	const UeAgentApplyWorkspaceContract::FUnitDescriptor* Descriptor =
		OutResult.WorkspaceContract.FindUnit(Binding.UnitId);
	if (!Descriptor || Descriptor->ApplyContractRef != Binding.InputContractId
		|| Descriptor->PathPattern != Binding.PathPattern
		|| Descriptor->MergePolicy != WorkspaceMergePolicy(Binding.MergeMode))
	{
		return Fail(OutFailure, TEXT("catalog_workspace_unit_contract_mismatch"), Kind);
	}
	FString RelativePath;
	FString MissingPathField;
	if (!ResolveTemplate(
		Binding.RelativePathTemplate, Request.Seed, Request.Context, RelativePath, MissingPathField))
	{
		return Fail(OutFailure, TEXT("catalog_pointer_binding_unresolved"), MissingPathField);
	}
	if (!UeAgentAuthoringWorkspaceRouter::MatchesPathPattern(Binding.PathPattern, RelativePath))
	{
		return Fail(OutFailure, TEXT("catalog_relative_path_contract_mismatch"), RelativePath);
	}

	UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit Unit;
	Unit.UnitId = Descriptor->UnitId;
	Unit.CanonicalUnitKey = Descriptor->UnitId + TEXT(":") + RelativePath;
	Unit.RelativePath = RelativePath;
	Unit.Role = Descriptor->Role;
	Unit.RouteId = Descriptor->RouteId;
	Unit.ApplyContractRef = Descriptor->ApplyContractRef;
	Unit.MergePolicy = Descriptor->MergePolicy;
	Unit.Presence = UeAgentAuthoringWorkspaceRouter::EUnitPresence::Present;
	Unit.Action = UeAgentAuthoringWorkspaceRouter::EUnitAction::Apply;
	Unit.Document = Document;
	Unit.Provenance.SourceKind = UeAgentApplyWorkspaceContract::ESourceKind::Inline;
	Unit.Provenance.WorkspaceSource = TEXT("virtual://catalog/") + Kind;
	Unit.Provenance.SourceFile = RelativePath;
	Unit.Provenance.SourceUnit = Descriptor->UnitId;
	Unit.Provenance.RelativePath = RelativePath;
	Unit.Provenance.bVirtual = true;

	OutResult.Workspace.ContractId = OutResult.WorkspaceContract.ContractId;
	OutResult.Workspace.ContractVersion = OutResult.WorkspaceContract.ContractVersion;
	OutResult.Workspace.ProfileId = OutResult.WorkspaceContract.ProfileId;
	OutResult.Workspace.SourceKind = UeAgentApplyWorkspaceContract::ESourceKind::Inline;
	OutResult.Workspace.WorkspaceRoot = Unit.Provenance.WorkspaceSource;
	OutResult.Workspace.Units = { Unit };

	UeAgentApplyInputContract::FOperationInput ParsedInput;
	UeAgentApplyInputContract::FParseFailure ParseFailure;
	if (!UeAgentApplyInputContract::ParseOperationInput(InputContract, OutResult.Workspace.Units[0], ParsedInput, ParseFailure))
	{
		return Fail(
			OutFailure,
			TEXT("catalog_input_contract_rejected"),
			ParseFailure.ErrorCode + TEXT(":") + ParseFailure.FieldPath + TEXT(":") + ParseFailure.ErrorDetail);
	}
	UeAgentAuthoringWorkspaceDependencyGraph::FGraphFailure GraphFailure;
	if (!UeAgentAuthoringWorkspaceDependencyGraph::BuildDependencyGraph(
		OutResult.WorkspaceContract,
		OutResult.Workspace,
		UeAgentAuthoringWorkspaceDependencyGraph::FDependencyInputs(),
		OutResult.DependencyGraph,
		GraphFailure))
	{
		return Fail(OutFailure, GraphFailure.ErrorCode, GraphFailure.ErrorDetail);
	}
	OutResult.CanonicalDocument = Document;
	return true;
}
}
