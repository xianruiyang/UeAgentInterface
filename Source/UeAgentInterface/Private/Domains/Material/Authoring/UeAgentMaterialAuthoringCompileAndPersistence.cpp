// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Material/Authoring/UeAgentMaterialAuthoringCompileAndPersistence.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/EditorResources/UeAgentEditorResourcePersistence.h"

#include "Dom/JsonValue.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "Materials/MaterialInterface.h"
#include "MaterialShared.h"
#include "UObject/Object.h"
#include "UObject/Package.h"

namespace UeAgentMaterialAuthoring
{
	bool SaveAssetPackage(UObject* Asset, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!Asset)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("asset_not_found"),
				TEXT("The material asset is no longer available for saving."), TEXT("save"));
			OutError.PropertyPath = TEXT("asset");
			OutError.ActualValuePresent = false;
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("live material asset"));
			OutError.SourceKind = TEXT("native_result");
			return false;
		}

		TArray<UPackage*> PackagesToSave;
		PackagesToSave.Add(Asset->GetOutermost());
		if (!UE::UAI::EditorResources::SavePackages(PackagesToSave, false, OutError))
		{
			OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("save_asset_failed"), OutError, TEXT(""));
			return false;
		}
		return true;
	}

	bool ParseCompileSeverityFilter(const FString& InFilterText, ECompileMessageSeverityFilter& OutFilter)
	{
		FString Normalized = InFilterText.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));

		if (Normalized.IsEmpty() || Normalized == TEXT("all"))
		{
			OutFilter = ECompileMessageSeverityFilter::All;
			return true;
		}
		if (Normalized == TEXT("error") || Normalized == TEXT("errors"))
		{
			OutFilter = ECompileMessageSeverityFilter::Error;
			return true;
		}
		if (Normalized == TEXT("warning") || Normalized == TEXT("warnings") || Normalized == TEXT("warn"))
		{
			OutFilter = ECompileMessageSeverityFilter::Warning;
			return true;
		}
		if (Normalized == TEXT("info") || Normalized == TEXT("note") || Normalized == TEXT("notes"))
		{
			OutFilter = ECompileMessageSeverityFilter::Info;
			return true;
		}
		if (Normalized == TEXT("warningorerror") || Normalized == TEXT("warnorerror") || Normalized == TEXT("errorsandwarnings"))
		{
			OutFilter = ECompileMessageSeverityFilter::WarningOrError;
			return true;
		}
		return false;
	}

	static bool ShouldIncludeCompileMessage(const ECompileMessageSeverityFilter InFilter)
	{
		switch (InFilter)
		{
		case ECompileMessageSeverityFilter::All:
		case ECompileMessageSeverityFilter::Error:
		case ECompileMessageSeverityFilter::WarningOrError:
			return true;
		case ECompileMessageSeverityFilter::Warning:
		case ECompileMessageSeverityFilter::Info:
		default:
			return false;
		}
	}

	static FString FeatureLevelToString(const ERHIFeatureLevel::Type InFeatureLevel)
	{
		switch (InFeatureLevel)
		{
		case ERHIFeatureLevel::ES2_REMOVED: return TEXT("ES2_REMOVED");
		case ERHIFeatureLevel::ES3_1: return TEXT("ES3_1");
		case ERHIFeatureLevel::SM4_REMOVED: return TEXT("SM4_REMOVED");
		case ERHIFeatureLevel::SM5: return TEXT("SM5");
		case ERHIFeatureLevel::SM6: return TEXT("SM6");
		case ERHIFeatureLevel::Num: return TEXT("Num");
		default: return TEXT("Unknown");
		}
	}

	void CollectMaterialCompileMessages(
		UMaterialInterface* MaterialInterface,
		const ECompileMessageSeverityFilter InFilter,
		TArray<TSharedPtr<FJsonValue>>& OutMessages,
		int32& OutErrorCount)
	{
		OutMessages.Reset();
		OutErrorCount = 0;
		if (!MaterialInterface || !ShouldIncludeCompileMessage(InFilter))
		{
			return;
		}

		TSet<FString> DedupSet;
		UMaterialInterface::IterateOverActiveFeatureLevels([&](const ERHIFeatureLevel::Type FeatureLevel)
		{
			const FMaterialResource* MaterialResource = MaterialInterface->GetMaterialResource(FeatureLevel);
			if (!MaterialResource)
			{
				return;
			}

			const TArray<FString>& CompileErrors = MaterialResource->GetCompileErrors();
			if (CompileErrors.Num() == 0)
			{
				return;
			}

			const FString FeatureLevelName = FeatureLevelToString(FeatureLevel);
			for (const FString& CompileError : CompileErrors)
			{
				const FString MessageText = CompileError.TrimStartAndEnd();
				if (MessageText.IsEmpty())
				{
					continue;
				}

				const FString DedupKey = FeatureLevelName + TEXT("::") + MessageText;
				if (DedupSet.Contains(DedupKey))
				{
					continue;
				}
				DedupSet.Add(DedupKey);
				++OutErrorCount;

				TSharedPtr<FJsonObject> MessageObj = MakeShared<FJsonObject>();
				MessageObj->SetStringField(TEXT("severity"), TEXT("Error"));
				MessageObj->SetStringField(TEXT("feature_level"), FeatureLevelName);
				MessageObj->SetStringField(TEXT("message"), MessageText);
				OutMessages.Add(MakeShared<FJsonValueObject>(MessageObj));
			}
		});
	}
}
