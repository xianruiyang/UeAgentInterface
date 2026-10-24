// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Material/Authoring/UeAgentMaterialExpressionResolver.h"

#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Dom/JsonValue.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Misc/PackageName.h"

namespace UeAgentMaterialAuthoring
{
	namespace
	{
		bool AddUniqueClassCandidate(
			UClass* Candidate,
			const FString& Source,
			UClass*& InOutResolved,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (!Candidate || !Candidate->IsChildOf(UMaterialExpression::StaticClass())
				|| Candidate->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("material_expression_class_invalid"), FString::Printf(TEXT("The material expression class is invalid: %s."), *Source), TEXT("candidate_resolution"));
				return false;
			}
			if (InOutResolved && InOutResolved != Candidate)
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("material_expression_class_conflict"), FString::Printf(TEXT("The material expression class selectors disagree: %s and %s."), *InOutResolved->GetPathName(), *Candidate->GetPathName()), TEXT("candidate_resolution"));
				return false;
			}
			InOutResolved = Candidate;
			return true;
		}

		bool AddUniqueFunctionCandidate(
			UMaterialFunctionInterface* Candidate,
			const FString& Source,
			UMaterialFunctionInterface*& InOutResolved,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (!Candidate)
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("material_function_not_found"), FString::Printf(TEXT("The material function could not be resolved: %s."), *Source), TEXT("candidate_resolution"));
				return false;
			}
			if (InOutResolved && InOutResolved != Candidate)
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("material_function_identity_conflict"), FString::Printf(TEXT("The material function selectors disagree: %s and %s."), *InOutResolved->GetPathName(), *Candidate->GetPathName()), TEXT("candidate_resolution"));
				return false;
			}
			InOutResolved = Candidate;
			return true;
		}
	}

	UClass* ResolveExpressionClassPath(const FString& InClassPath)
	{
		FString ClassPath = InClassPath.TrimStartAndEnd();
		if (ClassPath.IsEmpty()) return nullptr;
		if (UClass* Existing = FindObject<UClass>(nullptr, *ClassPath)) return Existing;
		if (UClass* Loaded = LoadObject<UClass>(nullptr, *ClassPath)) return Loaded;
		if (!ClassPath.EndsWith(TEXT("_C")))
		{
			return LoadObject<UClass>(nullptr, *(ClassPath + TEXT("_C")));
		}
		return nullptr;
	}

	UMaterialFunctionInterface* ResolveMaterialFunctionPath(const FString& InFunctionPath)
	{
		FString FunctionPath = InFunctionPath.TrimStartAndEnd();
		if (FunctionPath.IsEmpty()) return nullptr;
		if (UMaterialFunctionInterface* Existing = FindObject<UMaterialFunctionInterface>(nullptr, *FunctionPath))
		{
			return Existing;
		}
		if (UMaterialFunctionInterface* Loaded = LoadObject<UMaterialFunctionInterface>(nullptr, *FunctionPath))
		{
			return Loaded;
		}
		if (!FunctionPath.Contains(TEXT(".")) && FPackageName::IsValidLongPackageName(FunctionPath))
		{
			const FString AssetName = FPackageName::GetLongPackageAssetName(FunctionPath);
			return LoadObject<UMaterialFunctionInterface>(
				nullptr,
				*FString::Printf(TEXT("%s.%s"), *FunctionPath, *AssetName));
		}
		return nullptr;
	}

	bool ResolveExpression(
		const FExpressionResolveRequest& Request,
		FExpressionResolveResult& OutResult,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutResult = FExpressionResolveResult();
		OutError.Reset();
		UClass* ResolvedClass = nullptr;
		UMaterialFunctionInterface* ResolvedFunction = nullptr;

		const FString ExplicitClass = Request.ExpressionClassPath.TrimStartAndEnd();
		if (!ExplicitClass.IsEmpty()
			&& !AddUniqueClassCandidate(
				ResolveExpressionClassPath(ExplicitClass), ExplicitClass, ResolvedClass, OutError))
		{
			return false;
		}

		const FString Path = Request.Path.TrimStartAndEnd();
		if (!Path.IsEmpty())
		{
			if (Path.StartsWith(TEXT("/Script/"), ESearchCase::IgnoreCase))
			{
				if (!AddUniqueClassCandidate(
					ResolveExpressionClassPath(Path), Path, ResolvedClass, OutError)) return false;
			}
			else if (!AddUniqueFunctionCandidate(
				ResolveMaterialFunctionPath(Path), Path, ResolvedFunction, OutError))
			{
				return false;
			}
		}

		const FString ExplicitFunction = Request.MaterialFunctionPath.TrimStartAndEnd();
		if (!ExplicitFunction.IsEmpty()
			&& !AddUniqueFunctionCandidate(
				ResolveMaterialFunctionPath(ExplicitFunction), ExplicitFunction, ResolvedFunction, OutError))
		{
			return false;
		}

		const FString CatalogName = Request.CatalogName.TrimStartAndEnd();
		if (!CatalogName.IsEmpty())
		{
			UeAgentPaletteSearch::FSearchParams SearchParams;
			SearchParams.CatalogType = TEXT("material_node");
			SearchParams.SearchText = CatalogName;
			SearchParams.SearchMode = TEXT("exact");
			SearchParams.Sort = TEXT("score");
			SearchParams.AssetPath = Request.ContextAssetPath;
			SearchParams.GraphName = Request.ContextGraphName;
			SearchParams.PinName = Request.ContextPinName;
			UeAgentPaletteSearch::FCandidate Candidate;
			UE::UAI::Contract::FDiagnostic ResolveError;
			if (!UeAgentPaletteSearch::TryResolveMaterialActionCandidate(
				SearchParams,
				CatalogName,
				Request.CatalogSelect,
				FString(),
				Candidate,
				ResolveError, Request.CatalogInputContext.JsonPath.IsEmpty() ? nullptr : &Request.CatalogInputContext))
			{
				OutError = ResolveError;
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CatalogName;
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Request.ContextAssetPath;
				return false;
			}
			if (Candidate.Kind.Equals(TEXT("material_function_call"), ESearchCase::IgnoreCase))
			{
				if (!AddUniqueFunctionCandidate(
					ResolveMaterialFunctionPath(Candidate.FullName), Candidate.FullName, ResolvedFunction, OutError))
				{
					return false;
				}
			}
			else if (!AddUniqueClassCandidate(
				ResolveExpressionClassPath(Candidate.FullName), Candidate.FullName, ResolvedClass, OutError))
			{
				return false;
			}
		}

		if (ResolvedFunction)
		{
			if (!AddUniqueClassCandidate(
				UMaterialExpressionMaterialFunctionCall::StaticClass(),
				ResolvedFunction->GetPathName(),
				ResolvedClass,
				OutError))
			{
				return false;
			}
		}
		if (!ResolvedClass)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("material_expression_identity_missing"), TEXT("Specify a material expression class, function, or catalog candidate."), TEXT("candidate_resolution"));
			OutError.PropertyPath = TEXT("candidate");
			OutError.ActualValuePresent = false;
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("expression class, function path, or catalog candidate"));
			return false;
		}

		OutResult.ExpressionClass = ResolvedClass;
		OutResult.MaterialFunction = ResolvedFunction;
		OutResult.ExpressionClassPath = ResolvedClass->GetPathName();
		OutResult.MaterialFunctionPath = ResolvedFunction
			? ResolvedFunction->GetPathName()
			: FString();
		return true;
	}
}
