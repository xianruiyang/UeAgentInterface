// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/PaletteSearch/UeAgentPaletteSearchAuthoring.h"

#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Dom/JsonObject.h"
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
	FString& OutError)
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
		OutError = TEXT("catalog_workspace_contract_not_registered:") + Binding.WorkspaceContractId;
		return false;
	}
	UeAgentApplyInputContract::FContract InputContract;
	if (!UeAgentApplyInputContract::FRegistry::Get().FindContract(Binding.InputContractId, InputContract))
	{
		OutError = TEXT("catalog_input_contract_not_registered:") + Binding.InputContractId;
		return false;
	}
	const UeAgentApplyWorkspaceContract::FUnitDescriptor* Unit = WorkspaceContract.FindUnit(Binding.UnitId);
	const FString ExpectedMergePolicy = Binding.MergeMode == TEXT("merge_object")
		? FString(TEXT("replace"))
		: Binding.MergeMode;
	if (!Unit || Unit->ApplyContractRef != Binding.InputContractId
		|| Unit->PathPattern != Binding.PathPattern || Unit->MergePolicy != ExpectedMergePolicy)
	{
		OutError = TEXT("catalog_workspace_unit_contract_mismatch:") + Binding.Kind;
		return false;
	}
	TSharedRef<FJsonObject> Seed = MakeShared<FJsonObject>();
	for (const FString& Field : Binding.RequiredSeedFields)
	{
		const FString Value = Candidate.Identity.GetSelector(Field);
		if (Value.IsEmpty())
		{
			OutError = TEXT("catalog_seed_selector_missing:") + Field;
			return false;
		}
		Seed->SetStringField(Field, Value);
	}
	for (const FString& Field : Binding.OptionalSeedFields)
	{
		const FString Value = Candidate.Identity.GetSelector(Field);
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
	FString& OutError,
	const FString& AssetPath,
	const FString& GraphName)
{
	const FString TrimmedName = Name.TrimStartAndEnd();
	if (CatalogType.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_catalog_type");
		return false;
	}
	if (TrimmedName.IsEmpty())
	{
		OutError = TEXT("missing_catalog_name");
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
	Params.Limit = 1;
	Params.bIncludeItems = true;
	Params.bIncludeGroups = false;
	Params.bIncludeCategoryTree = false;
	Params.bIncludeDebugFields = false;

	FSearchResult Result;
	if (!ExecuteSearch(Params, Result, OutError))
	{
		return false;
	}

	if (Result.bHasTargetResolution && Result.TargetResolution.IsResolved() && Result.MatchingCandidates.Num() == 1)
	{
		OutCandidate = Result.MatchingCandidates[0];
		OutError.Reset();
		return true;
	}

	OutError = Result.bHasTargetResolution && !Result.TargetResolution.ErrorCode.IsEmpty()
		? Result.TargetResolution.ErrorCode
		: FString(TEXT("target_not_found"));
	if (Result.bHasTargetResolution && !Result.TargetResolution.Candidates.IsEmpty())
	{
		TArray<FString> CandidateKeys;
		for (const UeAgentAuthoringTargetLocator::FDiagnosticCandidate& Candidate : Result.TargetResolution.Candidates)
		{
			CandidateKeys.Add(Candidate.Identity.CanonicalKey);
		}
		OutError += TEXT(":candidates=") + FString::Join(CandidateKeys, TEXT(","));
	}
	return false;
}
}
