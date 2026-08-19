// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/Assets/UeAgentReflectionAuthoringCommandBridge.h"

#include "Dom/JsonObject.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringApplyProvider.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringAssets.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringExport.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/UeAgentJsonDiagnostics.h"

namespace UeAgentReflectionAuthoringCommandBridge
{
	namespace
	{
		const UeAgentReflectionAuthoring::FProfile* Profile(const FString& TypeId, FString& OutError)
		{
			const UeAgentReflectionAuthoring::FProfile* Result = UeAgentReflectionAuthoring::FindProfileByTypeId(TypeId);
			if (!Result) OutError = TEXT("reflection_authoring_profile_missing:") + TypeId;
			return Result;
		}

		bool ReadRequiredString(
			const TSharedPtr<FJsonObject>& Params,
			const TCHAR* Field,
			FString& OutValue,
			FString& OutError)
		{
			if (!Params.IsValid() || !Params->TryGetStringField(Field, OutValue) || OutValue.TrimStartAndEnd().IsEmpty())
			{
				OutError = FString(Field) + TEXT("_required");
				return false;
			}
			return true;
		}

		bool TryReadAnyString(
			const TSharedPtr<FJsonObject>& Params,
			const TArray<const TCHAR*>& Fields,
			FString& OutValue,
			FString* OutField = nullptr)
		{
			if (!Params.IsValid()) return false;
			for (const TCHAR* Field : Fields)
			{
				if (Params->TryGetStringField(Field, OutValue) && !OutValue.TrimStartAndEnd().IsEmpty())
				{
					if (OutField) *OutField = Field;
					return true;
				}
			}
			return false;
		}

		bool MakeSource(
			const TSharedPtr<FJsonObject>& Params,
			UeAgentAuthoringWorkspaceLoader::FSourceRequest& OutSource,
			FString& OutError)
		{
			FString InputPath;
			FString Field;
			if (!TryReadAnyString(Params, {TEXT("folder_path"), TEXT("input_folder"), TEXT("json_folder")}, InputPath, &Field)
				&& !TryReadAnyString(Params, {TEXT("json_file"), TEXT("input_file")}, InputPath, &Field))
			{
				OutError = TEXT("folder_path_or_input_file_required");
				return false;
			}
			InputPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(InputPath);
			OutSource = Field.Contains(TEXT("file"))
				? UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeSingleFile(InputPath)
				: UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeDirectory(InputPath);
			return true;
		}
	}

	bool Export(
		const FString& TypeId,
		const TSharedPtr<FJsonObject>& Params,
		TSharedPtr<FJsonObject>& OutData,
		FString& OutError)
	{
		FString AssetPath;
		if (!ReadRequiredString(Params, TEXT("asset_path"), AssetPath, OutError)) return false;
		const UeAgentReflectionAuthoring::FProfile* AssetProfile = Profile(TypeId, OutError);
		if (!AssetProfile) return false;
		UObject* Asset = UeAgentReflectionAuthoring::LoadAsset(*AssetProfile, AssetPath);
		if (!Asset)
		{
			OutError = AssetProfile->TypeId + TEXT("_not_found");
			return false;
		}
		FString ExportProfile = TEXT("llm_authoring");
		if (Params.IsValid()) Params->TryGetStringField(TEXT("profile"), ExportProfile);
		FString OutputPath;
		FString OutputField;
		const bool bHasOutput = TryReadAnyString(Params, {TEXT("folder_path"), TEXT("output_folder"), TEXT("output_file")}, OutputPath, &OutputField);
		if (!bHasOutput && !UeAgentReflectionAuthoring::ResolveDefaultWorkspaceFolder(*AssetProfile, AssetPath, OutputPath, OutError)) return false;
		OutputPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(OutputPath);
		UeAgentExportWorkspaceWriter::FWorkspaceResult Result;
		const bool bExported = OutputField == TEXT("output_file")
			? UeAgentReflectionAuthoring::ExportAssetFile(*AssetProfile, *Asset, OutputPath, ExportProfile, Result, OutError)
			: UeAgentReflectionAuthoring::ExportAssetWorkspace(*AssetProfile, *Asset, OutputPath, ExportProfile, Result, OutError);
		OutData = Result.ToJsonObject();
		OutData->SetStringField(TEXT("asset_path"), UeAgentReflectionAuthoring::NormalizePackagePath(AssetPath));
		return bExported;
	}

	bool Apply(
		const FString& TypeId,
		const TSharedPtr<FJsonObject>& Params,
		TSharedPtr<FJsonObject>& OutData,
		FString& OutError,
		const FString& RequestId)
	{
		const UeAgentReflectionAuthoring::FProfile* AssetProfile = Profile(TypeId, OutError);
		if (!AssetProfile) return false;
		UeAgentAuthoringWorkspaceLoader::FSourceRequest Source;
		if (!MakeSource(Params, Source, OutError)) return false;
		FString AssetPath;
		if (Params.IsValid()) Params->TryGetStringField(TEXT("asset_path"), AssetPath);
		TOptional<bool> SaveOverride;
		bool bSave = false;
		if (Params.IsValid() && Params->TryGetBoolField(TEXT("save"), bSave)) SaveOverride = bSave;
		UeAgentReflectionAuthoring::FAssetApplyResult Result;
		const bool bApplied = UeAgentReflectionAuthoring::ApplyAssetSource(*AssetProfile, Source, AssetPath, Result, SaveOverride, RequestId);
		OutData = Result.ToJsonObject();
		if (!bApplied)
		{
			OutError = UeAgentAuthoringDiagnostics::GetActionableFailureMessage(
				Result.ErrorCode, Result.ErrorDetail, TEXT("reflection_folder_apply_failed"));
		}
		return bApplied;
	}

	bool Validate(
		const FString& TypeId,
		const TSharedPtr<FJsonObject>& Params,
		TSharedPtr<FJsonObject>& OutData,
		FString& OutError)
	{
		const UeAgentReflectionAuthoring::FProfile* AssetProfile = Profile(TypeId, OutError);
		if (!AssetProfile) return false;
		UeAgentAuthoringWorkspaceLoader::FSourceRequest Source;
		if (!MakeSource(Params, Source, OutError)) return false;
		FString AssetPath;
		if (Params.IsValid()) Params->TryGetStringField(TEXT("asset_path"), AssetPath);
		UeAgentReflectionAuthoring::FWorkspaceInspectResult Result;
		const bool bValid = UeAgentReflectionAuthoring::InspectAssetSource(*AssetProfile, Source, AssetPath, Result);
		OutData = Result.ToJsonObject();
		if (!bValid)
		{
			OutError = UeAgentAuthoringDiagnostics::GetActionableFailureMessage(
				Result.ErrorCode, Result.ErrorDetail, TEXT("reflection_folder_validation_failed"));
		}
		return bValid;
	}
}
