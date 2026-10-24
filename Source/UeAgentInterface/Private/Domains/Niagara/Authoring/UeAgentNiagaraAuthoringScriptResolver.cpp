// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringScriptResolver.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringJsonFields.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchAuthoring.h"
#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "NiagaraScript.h"
#include "UObject/UObjectGlobals.h"

namespace UeAgentNiagaraAuthoring
{
	namespace
	{
		UE::UAI::Contract::FDiagnostic ScriptFailure(const TCHAR* Code, const FString& Reason,
			const FString& Field, const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("script_resolve"));
			Error.PropertyPath = Field;
			if (!Field.IsEmpty()) Error.Source.JsonPath = TEXT("/") + Field.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
			Error.ActualValue = Actual;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ExpectedValue = Expected;
			Error.ExpectedValuePresent = Expected.IsValid();
			return Error;
		}

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
			UE::UAI::Contract::FDiagnostic& OutError)
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
					OutError = ScriptFailure(TEXT("niagara_script_reference_conflict"),
						FString::Printf(TEXT("Script reference alias '%s' disagrees with '%s'."), *Pair.Key, *Values[0].Key),
						Pair.Key, MakeShared<FJsonValueString>(Pair.Value), MakeShared<FJsonValueString>(OutValue));
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

		bool ValidateKind(const UNiagaraScript& Script, const EScriptKind Kind, UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (Kind == EScriptKind::Module && !Script.IsModuleScript())
			{
				OutError = ScriptFailure(TEXT("niagara_script_is_not_module"), TEXT("The selected Niagara script is not a module."),
					TEXT("usage"), MakeShared<FJsonValueString>(StaticEnum<ENiagaraScriptUsage>()->GetNameStringByValue(static_cast<int64>(Script.GetUsage()))),
					MakeShared<FJsonValueString>(TEXT("Module")));
				return false;
			}
			if (Kind == EScriptKind::DynamicInput && Script.GetUsage() != ENiagaraScriptUsage::DynamicInput)
			{
				OutError = ScriptFailure(TEXT("niagara_script_is_not_dynamic_input"), TEXT("The selected Niagara script is not a dynamic input."),
					TEXT("usage"), MakeShared<FJsonValueString>(StaticEnum<ENiagaraScriptUsage>()->GetNameStringByValue(static_cast<int64>(Script.GetUsage()))),
					MakeShared<FJsonValueString>(TEXT("DynamicInput")));
				return false;
			}
			return true;
		}
	}

	bool ResolveScript(
		const FScriptResolveRequest& Request,
		FScriptResolveResult& OutResult,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutResult = FScriptResolveResult();
		OutError.Reset();
		if (!Request.Input.IsValid())
		{
			OutError = ScriptFailure(TEXT("niagara_script_reference_missing"), TEXT("A Niagara script reference object is required."),
				FString(), nullptr, MakeShared<FJsonValueString>(TEXT("script reference object")));
			return false;
		}
		ON_SCOPE_EXIT {
			if (!OutError.IsEmpty()) UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Request.Input, TEXT("script_resolve"));
		};

		if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Request.Input, {
			{TEXT("path"), EJson::String}, {TEXT("script_asset_path"), EJson::String},
			{TEXT("module_script_path"), EJson::String}, {TEXT("module_script_asset_path"), EJson::String},
			{TEXT("dynamic_input_script_asset_path"), EJson::String}, {TEXT("dynamic_input_script_path"), EJson::String},
			{TEXT("dynamic_input"), EJson::String}, {TEXT("catalog_name"), EJson::String},
			{TEXT("name"), EJson::String}, {TEXT("select"), EJson::Object}}, OutError)) return false;
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
		FString CatalogField = TEXT("catalog_name");
		Request.Input->TryGetStringField(TEXT("catalog_name"), CatalogName);
		CatalogName.TrimStartAndEndInline();
		if (CatalogName.IsEmpty() && ExplicitPath.IsEmpty())
		{
			CatalogField = TEXT("name");
			Request.Input->TryGetStringField(TEXT("name"), CatalogName);
			CatalogName.TrimStartAndEndInline();
		}

		FString CatalogPath;
		if (CatalogName.IsEmpty() && Request.Input->HasField(TEXT("select")))
		{
			OutError = ScriptFailure(TEXT("niagara_script_selector_name_required"),
				TEXT("Script select requires catalog_name or name; a path alone does not use semantic selection."),
				TEXT("catalog_name"), Request.Input->TryGetField(TEXT("catalog_name")), MakeShared<FJsonValueString>(TEXT("non-empty catalog_name or name")));
			return false;
		}
		if (!CatalogName.IsEmpty())
		{
			auto SelectorObject = MakeShared<FJsonObject>();
			SelectorObject->SetStringField(TEXT("name"), CatalogName);
			if (const auto Selection = Request.Input->TryGetField(TEXT("select")); Selection.IsValid())
				SelectorObject->SetField(TEXT("select"), Selection);
			const auto InputLocations = UeAgentAuthoringDiagnostics::FScopedSourceLocations::CaptureProjection(*Request.Input);
			UeAgentAuthoringDiagnostics::FScopedSourceLocations SelectorSources(&SelectorObject.Get(),
				[&InputLocations, &CatalogField](const FString& Pointer) {
					return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(InputLocations,
						Pointer == TEXT("/name") ? TEXT("/") + CatalogField : Pointer);
				});
			UE::UAI::HjsonWorkspace::DomainAuthoring::FDomainHjsonCandidateSelector Selector;
			UE::UAI::HjsonWorkspace::FWorkspaceError SelectorError;
			if (!UE::UAI::HjsonWorkspace::DomainAuthoring::TryReadCandidateSelector(
				SelectorObject, FString(), Selector, SelectorError))
			{
				OutError = SelectorError.GetDiagnostic();
				UeAgentAuthoringDiagnostics::LocateFailure(OutError, *SelectorObject, TEXT("script_resolve"));
				return false;
			}
			UeAgentPaletteSearch::FCandidate Candidate;
			if (!UeAgentPaletteSearch::ResolveCatalogNameToCandidate(
				CatalogType(Request.Kind), CatalogName, Candidate, OutError, FString(), FString(), Selector.Select))
			{
				UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *SelectorObject,
					OutError.PropertyPath == TEXT("select") ? TEXT("select") : TEXT("name"));
				return false;
			}
			CatalogPath = Candidate.FullName;
		}

		FString ResolvedInputPath = ExplicitPath.IsEmpty() ? CatalogPath : ExplicitPath;
		const FString ReferenceField = ExplicitPath.IsEmpty() ? CatalogField : PathAliases[0].Key;
		const FString ReferenceName = ExplicitPath.IsEmpty() ? CatalogName : ExplicitPath;
		if (ResolvedInputPath.IsEmpty())
		{
			OutError = ScriptFailure(TEXT("niagara_script_reference_missing"),
				TEXT("Provide a non-empty Niagara script asset path or catalog name."), ReferenceField,
				Request.Input->TryGetField(ReferenceField), MakeShared<FJsonValueString>(TEXT("script asset path or catalog name")));
			return false;
		}
		if (!Request.bAllowPrivateInline && ResolvedInputPath.Contains(TEXT(":")))
		{
			OutError = ScriptFailure(TEXT("niagara_private_inline_script_unsupported"),
				TEXT("This operation requires a standalone script asset; the selected script is private inline content."), ReferenceField,
				Request.Input->TryGetField(ReferenceField), MakeShared<FJsonValueString>(TEXT("standalone Niagara script asset")));
			return false;
		}

		UNiagaraScript* Script = LoadObject<UNiagaraScript>(nullptr, *ToObjectPath(ResolvedInputPath));
		if (!Script)
		{
			OutError = ScriptFailure(TEXT("niagara_script_not_found"), TEXT("The selected Niagara script could not be loaded."),
				ReferenceField, MakeShared<FJsonValueString>(ReferenceName), MakeShared<FJsonValueString>(TEXT("loadable Niagara script")));
			OutError.InternalContext = ToObjectPath(ResolvedInputPath);
			return false;
		}
		if (!CatalogPath.IsEmpty())
		{
			UNiagaraScript* CatalogScript = LoadObject<UNiagaraScript>(nullptr, *ToObjectPath(CatalogPath));
			if (!CatalogScript)
			{
				OutError = ScriptFailure(TEXT("niagara_catalog_script_not_found"), TEXT("The script selected by the catalog entry could not be loaded."),
					CatalogField, MakeShared<FJsonValueString>(CatalogName), MakeShared<FJsonValueString>(TEXT("catalog entry with a loadable Niagara script")));
				OutError.InternalContext = ToObjectPath(CatalogPath);
				return false;
			}
			if (CatalogScript != Script)
			{
				OutError = ScriptFailure(TEXT("niagara_script_reference_conflict"),
					FString::Printf(TEXT("The explicit script path and catalog entry '%s' select different scripts."), *CatalogName),
					ReferenceField, MakeShared<FJsonValueString>(ExplicitPath), MakeShared<FJsonValueString>(CatalogPath));
				return false;
			}
		}
		if (!ValidateKind(*Script, Request.Kind, OutError))
		{
			OutError.CandidateName = ReferenceName;
			OutError.Source.JsonPath.Reset();
			UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Request.Input, ReferenceField);
			return false;
		}

		OutResult.Script = Script;
		OutResult.ResolvedObjectPath = Script->GetPathName();
		OutResult.Source = ExplicitPath.IsEmpty() ? TEXT("catalog") : TEXT("explicit_path");
		return true;
	}
}
