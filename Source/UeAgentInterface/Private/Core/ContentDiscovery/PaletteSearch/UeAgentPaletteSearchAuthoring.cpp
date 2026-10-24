// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchAuthoring.h"

#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
#include "Misc/PackageName.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDomainRegistry.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringRegistrySnapshot.h"
#include "Utilities/AuthoringOperationApply/UeAgentApplyInputContract.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentApplyWorkspaceContract.h"

namespace UeAgentPaletteSearch
{
namespace
{
	TArray<TSharedPtr<FJsonValue>> StringArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const FString& Value : Values) Result.Add(MakeShared<FJsonValueString>(Value));
		return Result;
	}

	TSharedRef<FJsonObject> StringMap(const TMap<FString, FString>& Values)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& Pair : Values) Result->SetStringField(Pair.Key, Pair.Value);
		return Result;
	}
}

bool BuildAuthoringProjection(
	const FCandidate& Candidate,
	TSharedPtr<FJsonObject>& OutSeed,
	TSharedPtr<FJsonObject>& OutPlacement,
	UE::UAI::Contract::FDiagnostic& OutError)
{
	OutSeed.Reset();
	OutPlacement.Reset();
	OutError.Reset();

	UeAgentAuthoringDomainRegistry::FCatalogBinding Binding;
	if (!UeAgentAuthoringDomainRegistry::FRegistry::Get().FindCatalogBinding(Candidate.Kind, Binding))
	{
		return true;
	}

	UeAgentApplyWorkspaceContract::FContract WorkspaceContract;
	if (!UeAgentApplyWorkspaceContract::FRegistry::Get().FindContract(Binding.WorkspaceContractId, WorkspaceContract))
	{
		OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("catalog_workspace_contract_not_registered"), TEXT("The candidate catalog's workspace contract is not registered."), TEXT("candidate_authoring_projection"));
		OutError.CandidateName = Candidate.Name; OutError.PropertyPath = TEXT("workspace_contract_id"); OutError.SourceKind = TEXT("native_result"); OutError.ExpectedValue = MakeShared<FJsonValueString>(Binding.WorkspaceContractId); OutError.ActualValuePresent = false;
		OutError.InternalContext = Binding.WorkspaceContractId;
		return false;
	}
	UeAgentApplyInputContract::FContract InputContract;
	if (!UeAgentApplyInputContract::FRegistry::Get().FindContract(Binding.InputContractId, InputContract))
	{
		OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("catalog_input_contract_not_registered"), TEXT("The candidate catalog's operation input contract is not registered."), TEXT("candidate_authoring_projection"));
		OutError.CandidateName = Candidate.Name; OutError.PropertyPath = TEXT("input_contract_id"); OutError.SourceKind = TEXT("native_result"); OutError.ExpectedValue = MakeShared<FJsonValueString>(Binding.InputContractId); OutError.ActualValuePresent = false;
		OutError.InternalContext = Binding.InputContractId;
		return false;
	}
	const UeAgentApplyWorkspaceContract::FUnitDescriptor* Unit = WorkspaceContract.FindUnit(Binding.UnitId);
	const FString ExpectedMergePolicy = Binding.MergeMode == TEXT("merge_object")
		? FString(TEXT("replace"))
		: Binding.MergeMode;
	if (!Unit || Unit->ApplyContractRef != Binding.InputContractId
		|| Unit->PathPattern != Binding.PathPattern || Unit->MergePolicy != ExpectedMergePolicy)
	{
		OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("catalog_workspace_unit_contract_mismatch"), !Unit ? TEXT("The catalog binding refers to a missing workspace unit.")
			: Unit->ApplyContractRef != Binding.InputContractId ? TEXT("The workspace unit's apply contract differs from the candidate catalog binding.")
			: Unit->PathPattern != Binding.PathPattern ? TEXT("The workspace unit's path pattern differs from the candidate catalog binding.")
			: TEXT("The workspace unit's merge policy differs from the candidate catalog binding."), TEXT("candidate_authoring_projection"));
		OutError.CandidateName = Candidate.Name; OutError.SourceKind = TEXT("native_result");
		OutError.PropertyPath = !Unit ? TEXT("unit_id") : Unit->ApplyContractRef != Binding.InputContractId ? TEXT("apply_contract_ref") : Unit->PathPattern != Binding.PathPattern ? TEXT("path_pattern") : TEXT("merge_policy");
		OutError.ExpectedValue = MakeShared<FJsonValueString>(!Unit ? Binding.UnitId : Unit->ApplyContractRef != Binding.InputContractId ? Binding.InputContractId : Unit->PathPattern != Binding.PathPattern ? Binding.PathPattern : ExpectedMergePolicy);
		if (Unit) OutError.ActualValue = MakeShared<FJsonValueString>(Unit->ApplyContractRef != Binding.InputContractId ? Unit->ApplyContractRef : Unit->PathPattern != Binding.PathPattern ? Unit->PathPattern : Unit->MergePolicy);
		else OutError.ActualValuePresent = false;
		OutError.InternalContext = Binding.Kind;
		return false;
	}
	TSharedRef<FJsonObject> Seed = MakeShared<FJsonObject>();
	for (const FString& Field : Binding.RequiredSeedFields)
	{
		const FString Value = Field == TEXT("candidate_id")
			? Candidate.GetPublicId()
			: Candidate.Identity.GetSelector(Field);
		if (Value.IsEmpty())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("catalog_seed_selector_missing"), TEXT("The candidate is missing a required selector for its authoring seed."), TEXT("candidate_authoring_projection"));
			OutError.CandidateName = Candidate.Name; OutError.PropertyPath = TEXT("seed.") + Field; OutError.SourceKind = TEXT("native_result"); OutError.ExpectedValuePresent = true; OutError.ActualValuePresent = false;
			return false;
		}
		Seed->SetStringField(Field, Value);
	}
	for (const FString& Field : Binding.OptionalSeedFields)
	{
		const FString Value = Field == TEXT("candidate_id")
			? Candidate.GetPublicId()
			: Candidate.Identity.GetSelector(Field);
		if (!Value.IsEmpty())
		{
			Seed->SetStringField(Field, Value);
		}
	}

	TSharedPtr<FJsonObject> Snapshot;
	if (!UeAgentAuthoringRegistrySnapshot::BuildSnapshot(Snapshot, OutError) || !Snapshot.IsValid()) return false;
	TSharedRef<FJsonObject> Placement = MakeShared<FJsonObject>();
	Placement->SetStringField(TEXT("schema"), TEXT("uai.catalog_placement.v2"));
	Placement->SetStringField(TEXT("contract_hash"), Snapshot->GetStringField(TEXT("registry_hash")));
	Placement->SetStringField(TEXT("kind"), Binding.Kind);
	Placement->SetStringField(TEXT("domain_id"), Binding.DomainId);
	Placement->SetStringField(TEXT("workspace_contract_id"), WorkspaceContract.ContractId);
	Placement->SetStringField(TEXT("input_contract_id"), InputContract.ContractId);
	Placement->SetStringField(TEXT("unit_id"), Binding.UnitId);
	Placement->SetStringField(TEXT("path_pattern"), Binding.PathPattern);
	Placement->SetStringField(TEXT("relative_path_template"), Binding.RelativePathTemplate);
	Placement->SetArrayField(TEXT("exact_identity_fields"), StringArray(Binding.ExactIdentityFields));
	Placement->SetStringField(TEXT("pointer_template"), Binding.PointerTemplate);
	Placement->SetStringField(TEXT("merge_mode"), Binding.MergeMode);
	Placement->SetObjectField(TEXT("operation_map"), StringMap(Binding.OperationMap));
	Placement->SetObjectField(TEXT("seed_bindings"), StringMap(Binding.SeedBindings));
	Placement->SetArrayField(TEXT("required_context"), StringArray(Binding.RequiredContext));
	Placement->SetObjectField(TEXT("context_bindings"), StringMap(Binding.ContextBindings));
	Placement->SetObjectField(TEXT("context_value_aliases"), StringMap(Binding.ContextValueAliases));
	Placement->SetObjectField(TEXT("constant_bindings"), StringMap(Binding.ConstantBindings));
	Placement->SetArrayField(TEXT("required_seed_fields"), StringArray(Binding.RequiredSeedFields));
	Placement->SetArrayField(TEXT("optional_seed_fields"), StringArray(Binding.OptionalSeedFields));
	OutSeed = Seed;
	OutPlacement = Placement;
	return true;
}

bool ResolveCatalogNameToCandidate(
	const FString& CatalogType,
	const FString& Name,
	FCandidate& OutCandidate,
	UE::UAI::Contract::FDiagnostic& OutError,
	const FString& AssetPath,
	const FString& GraphName, const TMap<FString, FString>& Select)
{
	const FString TrimmedName = Name.TrimStartAndEnd();
	if (CatalogType.TrimStartAndEnd().IsEmpty())
	{
		OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("missing_catalog_type"), TEXT("A non-empty catalog type is required to resolve a candidate name."), TEXT("candidate_resolve")); OutError.PropertyPath = TEXT("catalog_type"); OutError.ActualValue = MakeShared<FJsonValueString>(CatalogType); OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty catalog type"));
		return false;
	}
	if (TrimmedName.IsEmpty())
	{
		OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("missing_catalog_name"), TEXT("A non-empty public candidate name is required."), TEXT("candidate_resolve")); OutError.PropertyPath = TEXT("name"); OutError.ActualValue = MakeShared<FJsonValueString>(Name); OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty public name"));
		return false;
	}

	FSearchParams Params;
	UeAgentAuthoringDomainRegistry::FCatalogBinding RequestedBinding;
	Params.CatalogType = UeAgentAuthoringDomainRegistry::FRegistry::Get().FindCatalogBinding(
		CatalogType, RequestedBinding)
		? RequestedBinding.Kind
		: CatalogType;
	Params.SearchText = TrimmedName;
	Params.SearchMode = TEXT("exact");
	Params.Sort = TEXT("score");
	Params.AssetPath = AssetPath;
	Params.GraphName = GraphName;
	Params.bIncludeNameContext = true;

	FSearchResult Result;
	if (!ExecuteSearch(Params, Result, OutError))
	{
		return false;
	}

	TArray<UE::UAI::ContentDiscovery::FNameSelectionEntry> Choices;
	for (const FCandidate& Candidate : Result.NameContextCandidates)
	{
		UE::UAI::ContentDiscovery::FNameSelectionEntry Choice;
		Choice.Name = Candidate.Name;
		Choice.Identity = Candidate.Identity.CanonicalKey;
		if (!Candidate.SourceOrigin.IsEmpty()) Choice.Select.Add(TEXT("source"), Candidate.SourceOrigin);
		const FString Asset = FPackageName::ObjectPathToObjectName(Candidate.FullName);
		if (!Asset.IsEmpty()) Choice.Select.Add(TEXT("asset"), Asset);
		const FString AssetSource = UE::UAI::ContentDiscovery::MakeReadableDiscoveryPath(
			Candidate.ObjectPath.IsEmpty() ? Candidate.PackagePath : Candidate.ObjectPath);
		if (!AssetSource.IsEmpty()) Choice.Select.Add(TEXT("asset_source"), AssetSource);
		Choices.Add(MoveTemp(Choice));
	}
	int32 Index = INDEX_NONE;
	if (!UE::UAI::ContentDiscovery::TryResolveNameSelection(Choices, TrimmedName, Select, Index, OutError,
		UE::UAI::ContentDiscovery::ENameComparison::IgnoreCase)) return false;
	OutCandidate = Result.NameContextCandidates[Index];
	return true;
}
}
