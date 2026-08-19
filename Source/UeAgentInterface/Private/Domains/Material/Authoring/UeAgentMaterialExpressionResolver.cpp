// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Material/Authoring/UeAgentMaterialExpressionResolver.h"

#include "Commands/PaletteSearch/UeAgentPaletteSearchAuthoring.h"
#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"
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
			FString& OutError)
		{
			if (!Candidate || !Candidate->IsChildOf(UMaterialExpression::StaticClass())
				|| Candidate->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
			{
				OutError = TEXT("material_expression_class_invalid:") + Source;
				return false;
			}
			if (InOutResolved && InOutResolved != Candidate)
			{
				OutError = FString::Printf(
					TEXT("material_expression_class_conflict:%s != %s"),
					*InOutResolved->GetPathName(),
					*Candidate->GetPathName());
				return false;
			}
			InOutResolved = Candidate;
			return true;
		}

		bool AddUniqueFunctionCandidate(
			UMaterialFunctionInterface* Candidate,
			const FString& Source,
			UMaterialFunctionInterface*& InOutResolved,
			FString& OutError)
		{
			if (!Candidate)
			{
				OutError = TEXT("material_function_not_found:") + Source;
				return false;
			}
			if (InOutResolved && InOutResolved != Candidate)
			{
				OutError = FString::Printf(
					TEXT("material_function_identity_conflict:%s != %s"),
					*InOutResolved->GetPathName(),
					*Candidate->GetPathName());
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
		FString& OutError)
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
			UeAgentPaletteSearch::FCandidate Candidate;
			FString ResolveError;
			if (!UeAgentPaletteSearch::ResolveCatalogNameToCandidate(
				TEXT("material_node"),
				CatalogName,
				Candidate,
				ResolveError,
				Request.ContextAssetPath,
				Request.ContextGraphName))
			{
				OutError = ResolveError.IsEmpty()
					? TEXT("material_expression_catalog_unresolved:") + CatalogName
					: ResolveError;
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
			OutError = TEXT("material_expression_identity_missing");
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
