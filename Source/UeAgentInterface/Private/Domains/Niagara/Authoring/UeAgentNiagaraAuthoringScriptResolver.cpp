// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringScriptResolver.h"

#include "Commands/PaletteSearch/UeAgentPaletteSearchAuthoring.h"
#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "NiagaraScript.h"
#include "UObject/UObjectGlobals.h"

namespace UeAgentNiagaraAuthoring
{
	namespace
	{
		void AddNonEmptyField(
			const TSharedPtr<const FJsonObject>& Input,
			const TCHAR* FieldName,
			TArray<TPair<FString, FString>>& OutValues)
		{
			FString Value;
			if (Input.IsValid() && Input->TryGetStringField(FieldName, Value))
			{
				Value.TrimStartAndEndInline();
				if (!Value.IsEmpty())
				{
					OutValues.Emplace(FieldName, MoveTemp(Value));
				}
			}
		}

		bool ResolveUniqueAlias(
			const TArray<TPair<FString, FString>>& Values,
			FString& OutValue,
			FString& OutError)
		{
			OutValue.Reset();
			for (const TPair<FString, FString>& Pair : Values)
			{
				if (OutValue.IsEmpty())
				{
					OutValue = Pair.Value;
				}
				else if (!OutValue.Equals(Pair.Value, ESearchCase::IgnoreCase))
				{
					OutError = FString::Printf(
						TEXT("niagara_script_reference_conflict:%s=%s"),
						*Pair.Key,
						*Pair.Value);
					return false;
				}
			}
			return true;
		}

		FString ToObjectPath(FString Path)
		{
			Path.TrimStartAndEndInline();
			if (!Path.Contains(TEXT(".")) && FPackageName::IsValidLongPackageName(Path))
			{
				const FString AssetName = FPackageName::GetLongPackageAssetName(Path);
				if (!AssetName.IsEmpty())
				{
					Path = FString::Printf(TEXT("%s.%s"), *Path, *AssetName);
				}
			}
			return Path;
		}

		const TCHAR* CatalogType(const EScriptKind Kind)
		{
			switch (Kind)
			{
			case EScriptKind::DynamicInput: return TEXT("niagara_dynamic_input");
			case EScriptKind::Module: return TEXT("niagara_module");
			default: return TEXT("niagara_module");
			}
		}

		bool ValidateKind(const UNiagaraScript& Script, const EScriptKind Kind, FString& OutError)
		{
			if (Kind == EScriptKind::Module && !Script.IsModuleScript())
			{
				OutError = TEXT("niagara_script_is_not_module");
				return false;
			}
			if (Kind == EScriptKind::DynamicInput && Script.GetUsage() != ENiagaraScriptUsage::DynamicInput)
			{
				OutError = TEXT("niagara_script_is_not_dynamic_input");
				return false;
			}
			return true;
		}
	}

	bool ResolveScript(
		const FScriptResolveRequest& Request,
		FScriptResolveResult& OutResult,
		FString& OutError)
	{
		OutResult = FScriptResolveResult();
		OutError.Reset();
		if (!Request.Input.IsValid())
		{
			OutError = TEXT("niagara_script_reference_missing");
			return false;
		}

		TArray<TPair<FString, FString>> PathAliases;
		AddNonEmptyField(Request.Input, TEXT("path"), PathAliases);
		AddNonEmptyField(Request.Input, TEXT("script_asset_path"), PathAliases);
		AddNonEmptyField(Request.Input, TEXT("module_script_path"), PathAliases);
		AddNonEmptyField(Request.Input, TEXT("module_script_asset_path"), PathAliases);
		AddNonEmptyField(Request.Input, TEXT("dynamic_input_script_asset_path"), PathAliases);
		AddNonEmptyField(Request.Input, TEXT("dynamic_input_script_path"), PathAliases);
		AddNonEmptyField(Request.Input, TEXT("dynamic_input"), PathAliases);
		FString ExplicitPath;
		if (!ResolveUniqueAlias(PathAliases, ExplicitPath, OutError))
		{
			return false;
		}

		FString CatalogName;
		Request.Input->TryGetStringField(TEXT("catalog_name"), CatalogName);
		CatalogName.TrimStartAndEndInline();
		if (CatalogName.IsEmpty() && ExplicitPath.IsEmpty())
		{
			Request.Input->TryGetStringField(TEXT("name"), CatalogName);
			CatalogName.TrimStartAndEndInline();
		}

		FString CatalogPath;
		if (!CatalogName.IsEmpty())
		{
			UeAgentPaletteSearch::FCandidate Candidate;
			if (!UeAgentPaletteSearch::ResolveCatalogNameToCandidate(
				CatalogType(Request.Kind), CatalogName, Candidate, OutError))
			{
				return false;
			}
			CatalogPath = Candidate.FullName;
		}

		FString ResolvedInputPath = ExplicitPath.IsEmpty() ? CatalogPath : ExplicitPath;
		if (ResolvedInputPath.IsEmpty())
		{
			OutError = TEXT("niagara_script_reference_missing");
			return false;
		}
		if (!Request.bAllowPrivateInline && ResolvedInputPath.Contains(TEXT(":")))
		{
			OutError = TEXT("niagara_private_inline_script_unsupported");
			return false;
		}

		UNiagaraScript* Script = LoadObject<UNiagaraScript>(nullptr, *ToObjectPath(ResolvedInputPath));
		if (!Script)
		{
			OutError = TEXT("niagara_script_not_found");
			return false;
		}
		if (!CatalogPath.IsEmpty())
		{
			UNiagaraScript* CatalogScript = LoadObject<UNiagaraScript>(nullptr, *ToObjectPath(CatalogPath));
			if (!CatalogScript)
			{
				OutError = TEXT("niagara_catalog_script_not_found");
				return false;
			}
			if (CatalogScript != Script)
			{
				OutError = TEXT("niagara_script_reference_conflict:catalog_name");
				return false;
			}
		}
		if (!ValidateKind(*Script, Request.Kind, OutError))
		{
			return false;
		}

		OutResult.Script = Script;
		OutResult.ResolvedObjectPath = Script->GetPathName();
		OutResult.Source = ExplicitPath.IsEmpty() ? TEXT("catalog") : TEXT("explicit_path");
		return true;
	}
}
