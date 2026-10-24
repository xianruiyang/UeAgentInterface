// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringCore/UeAgentCatalogWorkspaceAssembler.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

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

	bool Fail(FBuildFailure& Failure, UE::UAI::Contract::FDiagnostic Diagnostic)
    {
        Failure.Diagnostic = MoveTemp(Diagnostic);
        return false;
    }

	bool ReadNonEmptyString(const TSharedPtr<FJsonObject>& Object, const FString& Field, FString& OutValue)
	{
		OutValue.Reset();
		return Object.IsValid() && Object->TryGetStringField(Field, OutValue)
			&& !OutValue.TrimStartAndEnd().IsEmpty();
	}

	using UE::UAI::Contract::FDiagnostic;
	const TCHAR* JsonTypeName(EJson Type) {
		switch (Type) {
		case EJson::String: return TEXT("string"); case EJson::Number: return TEXT("number");
		case EJson::Boolean: return TEXT("boolean"); case EJson::Null: return TEXT("null");
		case EJson::Object: return TEXT("object"); case EJson::Array: return TEXT("array");
		default: return TEXT("none");
		}
	}
	FString EscapeSegment(FString Field) { return Field.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")); }
	FDiagnostic FieldFailure(const TSharedPtr<FJsonObject>& Root, const FString& Field,
		const TCHAR* Code, const TCHAR* Reason, const TSharedPtr<FJsonValue>& Expected)
	{
		auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, TEXT("/") + EscapeSegment(Field), Expected,
			Root ? Root->TryGetField(Field) : nullptr, TEXT("catalog_workspace"), Reason);
		if (Root) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Root);
		return Error;
	}
	FDiagnostic RequiredTextFailure(const TSharedPtr<FJsonObject>& Root, const FString& Field, const TCHAR* Code, const TCHAR* Reason)
	{
		auto Error = FieldFailure(Root, Field, Code, Reason, MakeShared<FJsonValueString>(TEXT("non-empty string")));
		// Seed/context values are identity-bearing. Their type and absence are public;
		// retain the original value only as internal evidence.
		Error.ActualEvidence = Error.ActualValue;
		if (Error.ActualValue) Error.ActualValue = MakeShared<FJsonValueString>(
			Error.ActualValue->Type == EJson::String ? TEXT("empty string") : JsonTypeName(Error.ActualValue->Type));
		return Error;
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
			auto Error = FDiagnostic::Failure(TEXT("catalog_pointer_invalid"), !Value ? TEXT("The catalog binding supplied no value.") : TEXT("The catalog pointer must start with a slash and contain non-empty segments."), TEXT("catalog_workspace"), Pointer);
			Error.PropertyPath = !Value ? TEXT("binding_value") : TEXT("pointer");
			Error.ExpectedValuePresent = true; Error.ActualValuePresent = Value.IsValid();
			return Fail(Failure, MoveTemp(Error));
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
				auto Error = FDiagnostic::Failure(TEXT("catalog_pointer_type_conflict"), TEXT("A catalog pointer traverses a value that is not an object."), TEXT("catalog_workspace"), Pointer);
				Error.PropertyPath = TEXT("pointer/segments/") + FString::FromInt(Index);
				Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
				Error.ActualValue = MakeShared<FJsonValueString>(JsonTypeName(Current->TryGetField(Segments[Index])->Type));
				return Fail(Failure, MoveTemp(Error));
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
		if (!SplitPointer(Pointer, Segments)) return Fail(Failure, FDiagnostic::Failure(TEXT("catalog_pointer_invalid"),
			TEXT("The catalog pointer must start with a slash and contain non-empty segments."), TEXT("catalog_workspace"), Pointer));
		TSharedPtr<FJsonObject> Current = Root;
		for (const FString& Segment : Segments)
		{
			const TSharedPtr<FJsonObject>* Existing = nullptr;
			if (Current->TryGetObjectField(Segment, Existing) && Existing && Existing->IsValid())
			{
				Current = *Existing;
				continue;
			}
			if (Current->HasField(Segment)) {
				auto Error = FDiagnostic::Failure(TEXT("catalog_pointer_type_conflict"), TEXT("A catalog pointer traverses a value that is not an object."), TEXT("catalog_workspace"), Pointer);
				Error.PropertyPath = TEXT("pointer/segments/") + FString::FromInt(static_cast<int32>(&Segment - Segments.GetData()));
				Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
				Error.ActualValue = MakeShared<FJsonValueString>(JsonTypeName(Current->TryGetField(Segment)->Type));
				return Fail(Failure, MoveTemp(Error));
			}
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
		FBuildFailure& Failure)
	{
		OutValue = Template;

		int32 SearchFrom = 0;
		while (true)
		{
			const int32 Open = OutValue.Find(TEXT("{"), ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
			if (Open == INDEX_NONE) return true;
			const int32 Close = OutValue.Find(TEXT("}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Open + 1);
			if (Close == INDEX_NONE)
			{
				auto Error = FDiagnostic::Failure(TEXT("catalog_template_invalid"), TEXT("The catalog binding template has an opening brace without a closing brace."), TEXT("catalog_workspace"));
				Error.PropertyPath = TEXT("template"); Error.ActualValue = MakeShared<FJsonValueString>(Template);
				Error.SourceKind = TEXT("native_result"); return Fail(Failure, MoveTemp(Error));
			}
			const FString Field = OutValue.Mid(Open + 1, Close - Open - 1);
			FString Replacement;
			if (!ReadNonEmptyString(Seed, Field, Replacement)
				&& !ReadNonEmptyString(Context, Field, Replacement))
			{
				auto Error = RequiredTextFailure(Seed && Seed->HasField(Field) ? Seed : Context, Field,
					TEXT("catalog_pointer_binding_unresolved"), TEXT("The binding template requires a non-empty string in the seed or context."));
				return Fail(Failure, MoveTemp(Error));
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
		return ResolveTemplate(Template, Request.Seed, Request.Context, OutPointer, Failure);
	}

	bool PlacementMatchesBinding(const FBuildRequest& Request, const FCatalogBinding& Binding, FBuildFailure& Failure)
	{
		auto TextMatches = [&](const TCHAR* Field, const FString& Expected) {
			FString Actual;
			if (ReadNonEmptyString(Request.Placement, Field, Actual) && Actual == Expected) return true;
			return Fail(Failure, FieldFailure(Request.Placement, Field, TEXT("catalog_placement_contract_mismatch"),
				TEXT("The catalog placement field differs from its registered contract."), MakeShared<FJsonValueString>(Expected)));
		};
		auto MapMatches = [&](const TCHAR* Field, const TMap<FString, FString>& Expected) {
			const TSharedPtr<FJsonObject>* Map = nullptr;
			if (!Request.Placement->TryGetObjectField(Field, Map) || !Map || !*Map)
				return Fail(Failure, FieldFailure(Request.Placement, Field, TEXT("catalog_placement_contract_mismatch"), TEXT("The catalog binding map must be an object of strings."), MakeShared<FJsonValueString>(TEXT("object of strings"))));
			TArray<FString> Keys; Expected.GenerateKeyArray(Keys);
			for (const auto& Pair : (*Map)->Values) Keys.AddUnique(Pair.Key); Keys.Sort();
			for (const auto& Key : Keys) {
				const FString* Wanted = Expected.Find(Key); FString Actual;
				if (Wanted && (*Map)->TryGetStringField(Key, Actual) && Actual == *Wanted) continue;
				auto Error = FieldFailure(*Map, Key, TEXT("catalog_placement_contract_mismatch"), TEXT("The catalog binding map entry differs from its registered contract."), Wanted ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(*Wanted)) : nullptr);
				if (Error.Source.SourceFile.IsEmpty()) {
					UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, TEXT("/") + FString(Field));
					UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Request.Placement);
				}
				return Fail(Failure, MoveTemp(Error));
			}
			return true;
		};
		return TextMatches(TEXT("domain_id"), Binding.DomainId)
			&& TextMatches(TEXT("workspace_contract_id"), Binding.WorkspaceContractId)
			&& TextMatches(TEXT("input_contract_id"), Binding.InputContractId)
			&& TextMatches(TEXT("unit_id"), Binding.UnitId)
			&& TextMatches(TEXT("path_pattern"), Binding.PathPattern)
			&& TextMatches(TEXT("relative_path_template"), Binding.RelativePathTemplate)
			&& TextMatches(TEXT("pointer_template"), Binding.PointerTemplate)
			&& TextMatches(TEXT("merge_mode"), Binding.MergeMode)
			&& MapMatches(TEXT("operation_map"), Binding.OperationMap)
			&& MapMatches(TEXT("seed_bindings"), Binding.SeedBindings)
			&& MapMatches(TEXT("context_bindings"), Binding.ContextBindings)
			&& MapMatches(TEXT("context_value_aliases"), Binding.ContextValueAliases)
			&& MapMatches(TEXT("constant_bindings"), Binding.ConstantBindings);
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
		return Fail(OutFailure, FieldFailure(Request.Placement, TEXT("schema"), TEXT("catalog_placement_schema_invalid"), TEXT("The catalog placement schema is not supported."), MakeShared<FJsonValueString>(TEXT("uai.catalog_placement.v2"))));
	}
	FString Kind;
	if (!ReadNonEmptyString(Request.Placement, TEXT("kind"), Kind))
	{
		return Fail(OutFailure, FieldFailure(Request.Placement, TEXT("kind"), TEXT("catalog_placement_kind_missing"), TEXT("The catalog placement requires a non-empty kind."), MakeShared<FJsonValueString>(TEXT("non-empty string"))));
	}
	FCatalogBinding Binding;
	if (!UeAgentAuthoringDomainRegistry::FRegistry::Get().FindCatalogBinding(Kind, Binding))
	{
		return Fail(OutFailure, FieldFailure(Request.Placement, TEXT("kind"), TEXT("catalog_binding_not_registered"), TEXT("No catalog binding is registered for this kind."), MakeShared<FJsonValueString>(TEXT("registered catalog kind"))));
	}
	if (!Request.Seed.IsValid()) return Fail(OutFailure, FieldFailure(nullptr, TEXT("seed"), TEXT("catalog_seed_invalid"), TEXT("A catalog seed object is required."), MakeShared<FJsonValueString>(TEXT("object"))));
	if (!Request.Context.IsValid()) return Fail(OutFailure, FieldFailure(nullptr, TEXT("context"), TEXT("seed_context_required"), TEXT("A catalog context object is required."), MakeShared<FJsonValueString>(TEXT("object"))));

	for (const FString& Field : Binding.RequiredContext)
	{
		if (!ReadNonEmptyString(Request.Context, Field, Value)) OutFailure.RequiredContext.Add(Field);
	}
	if (!OutFailure.RequiredContext.IsEmpty())
	{
		for (const auto& Field : OutFailure.RequiredContext)
			OutFailure.Diagnostic.Append(RequiredTextFailure(Request.Context, Field, TEXT("seed_context_required"), TEXT("A required catalog context field must be a non-empty string.")));
		return false;
	}

	FString PointerTemplate;
	if (!Request.Placement->TryGetStringField(TEXT("pointer_template"), PointerTemplate))
		return Fail(OutFailure, FieldFailure(Request.Placement, TEXT("pointer_template"), TEXT("catalog_placement_contract_mismatch"),
			TEXT("The catalog placement pointer template must be a string."), MakeShared<FJsonValueString>(TEXT("string"))));
	FString ResolvedPointer;
	if (!ResolvePointer(PointerTemplate, Request, ResolvedPointer, OutFailure))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Snapshot;
	UE::UAI::Contract::FDiagnostic SnapshotError;
	if (!UeAgentAuthoringRegistrySnapshot::BuildSnapshot(Snapshot, SnapshotError) || !Snapshot.IsValid())
	{
		return Fail(OutFailure, SnapshotError.IsEmpty() ? UE::UAI::Contract::FDiagnostic::Failure(TEXT("catalog_contract_snapshot_unavailable"), TEXT("The registry returned no catalog contract snapshot."), TEXT("catalog_workspace")) : SnapshotError);
	}
	FString ContractHash;
	if (!ReadNonEmptyString(Request.Placement, TEXT("contract_hash"), ContractHash)
		|| ContractHash != Snapshot->GetStringField(TEXT("registry_hash")))
	{
		auto Error = FDiagnostic::Failure(TEXT("catalog_contract_stale"), TEXT("The catalog placement was produced from a different or missing registry revision."), TEXT("catalog_workspace"));
		Error.PropertyPath = TEXT("contract_hash");
		Error.ExpectedEvidence = Snapshot->TryGetField(TEXT("registry_hash"));
		Error.ActualEvidence = Request.Placement->TryGetField(TEXT("contract_hash"));
		UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Request.Placement, TEXT("contract_hash"));
		return Fail(OutFailure, MoveTemp(Error));
	}
	if (!PlacementMatchesBinding(Request, Binding, OutFailure)) return false;

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Request.Seed->Values)
	{
		if (!Binding.SeedBindings.Contains(Pair.Key))
		{
			TArray<FString> Names; Binding.SeedBindings.GenerateKeyArray(Names); Names.Sort();
			TArray<TSharedPtr<FJsonValue>> Allowed; for (const auto& Name : Names) Allowed.Add(MakeShared<FJsonValueString>(Name));
			auto Error = FieldFailure(Request.Seed, Pair.Key, TEXT("catalog_seed_field_unknown"), TEXT("The catalog seed contains an undeclared field."), MakeShared<FJsonValueArray>(Allowed));
			Error.ActualValue = MakeShared<FJsonValueString>(Pair.Key); return Fail(OutFailure, MoveTemp(Error));
		}
	}
	for (const FString& Field : Binding.RequiredSeedFields)
	{
		if (!ReadNonEmptyString(Request.Seed, Field, Value))
		{
			return Fail(OutFailure, RequiredTextFailure(Request.Seed, Field, TEXT("catalog_seed_field_required"), TEXT("A required catalog seed field must be a non-empty string.")));
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
		auto Error = FDiagnostic::Failure(TEXT("catalog_workspace_contract_not_registered"), TEXT("The catalog workspace contract is not registered."), TEXT("catalog_workspace"));
		Error.PropertyPath = TEXT("workspace_contract_id"); Error.ActualValue = MakeShared<FJsonValueString>(Binding.WorkspaceContractId); Error.SourceKind = TEXT("native_result");
		return Fail(OutFailure, MoveTemp(Error));
	}
	UeAgentApplyInputContract::FContract InputContract;
	if (!UeAgentApplyInputContract::FRegistry::Get().FindContract(Binding.InputContractId, InputContract))
	{
		auto Error = FDiagnostic::Failure(TEXT("catalog_input_contract_not_registered"), TEXT("The catalog input contract is not registered."), TEXT("catalog_workspace"));
		Error.PropertyPath = TEXT("input_contract_id"); Error.ActualValue = MakeShared<FJsonValueString>(Binding.InputContractId); Error.SourceKind = TEXT("native_result");
		return Fail(OutFailure, MoveTemp(Error));
	}
	const UeAgentApplyWorkspaceContract::FUnitDescriptor* Descriptor =
		OutResult.WorkspaceContract.FindUnit(Binding.UnitId);
	if (!Descriptor || Descriptor->ApplyContractRef != Binding.InputContractId
		|| Descriptor->PathPattern != Binding.PathPattern
		|| Descriptor->MergePolicy != WorkspaceMergePolicy(Binding.MergeMode))
	{
		auto Error = FDiagnostic::Failure(TEXT("catalog_workspace_unit_contract_mismatch"), !Descriptor
			? TEXT("The catalog binding references an unknown workspace unit.") : TEXT("The workspace unit differs from the catalog binding contract."), TEXT("catalog_workspace"));
		Error.CandidateName = Kind; Error.SourceKind = TEXT("native_result");
		Error.PropertyPath = !Descriptor ? TEXT("unit_id") : Descriptor->ApplyContractRef != Binding.InputContractId ? TEXT("input_contract_id") : Descriptor->PathPattern != Binding.PathPattern ? TEXT("path_pattern") : TEXT("merge_policy");
		Error.ExpectedValue = MakeShared<FJsonValueString>(!Descriptor ? Binding.UnitId : Descriptor->ApplyContractRef != Binding.InputContractId ? Binding.InputContractId : Descriptor->PathPattern != Binding.PathPattern ? Binding.PathPattern : WorkspaceMergePolicy(Binding.MergeMode));
		if (Descriptor) Error.ActualValue = MakeShared<FJsonValueString>(Descriptor->ApplyContractRef != Binding.InputContractId ? Descriptor->ApplyContractRef : Descriptor->PathPattern != Binding.PathPattern ? Descriptor->PathPattern : Descriptor->MergePolicy);
		Error.ExpectedValuePresent = true; Error.ActualValuePresent = Descriptor != nullptr;
		return Fail(OutFailure, MoveTemp(Error));
	}
	FString RelativePath;
	if (!ResolveTemplate(
		Binding.RelativePathTemplate, Request.Seed, Request.Context, RelativePath, OutFailure)) return false;
	if (!UeAgentAuthoringWorkspaceRouter::MatchesPathPattern(Binding.PathPattern, RelativePath))
	{
		auto Error = FDiagnostic::Failure(TEXT("catalog_relative_path_contract_mismatch"), TEXT("The generated relative workspace path does not match the registered path pattern."), TEXT("catalog_workspace"));
		Error.PropertyPath = TEXT("relative_path"); Error.ExpectedValue = MakeShared<FJsonValueString>(Binding.PathPattern);
		Error.ActualEvidence = MakeShared<FJsonValueString>(RelativePath); Error.CandidateName = Kind;
		return Fail(OutFailure, MoveTemp(Error));
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
		return Fail(OutFailure, ParseFailure.Diagnostic);
	}
	UeAgentAuthoringWorkspaceDependencyGraph::FGraphFailure GraphFailure;
	if (!UeAgentAuthoringWorkspaceDependencyGraph::BuildDependencyGraph(
		OutResult.WorkspaceContract,
		OutResult.Workspace,
		UeAgentAuthoringWorkspaceDependencyGraph::FDependencyInputs(),
		OutResult.DependencyGraph,
		GraphFailure))
	{
		return Fail(OutFailure, GraphFailure.Diagnostic);
	}
	OutResult.CanonicalDocument = Document;
	return true;
}
}
