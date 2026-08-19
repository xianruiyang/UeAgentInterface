#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/UObjectGlobals.h"

namespace UeAgentCommandSafety
{
	inline bool ReadStrictBool(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* FieldName,
		bool DefaultValue,
		bool& OutValue,
		FString& OutError)
	{
		OutValue = DefaultValue;
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return true;
		}
		const TSharedPtr<FJsonValue>* Value = Params->Values.Find(FieldName);
		if (!Value || !Value->IsValid() || (*Value)->Type != EJson::Boolean)
		{
			OutError = FString::Printf(TEXT("invalid_%s"), FieldName);
			return false;
		}
		OutValue = (*Value)->AsBool();
		return true;
	}

	inline bool ResolveOverwriteConfirmation(
		const TSharedPtr<FJsonObject>& Params,
		bool& OutReplaceExisting,
		bool& OutAllowOverwrite,
		FString& OutError)
	{
		bool bReplaceExisting = false;
		bool bAllowOverwrite = false;
		if (!ReadStrictBool(Params, TEXT("replace_existing"), false, bReplaceExisting, OutError) ||
			!ReadStrictBool(Params, TEXT("allow_overwrite"), false, bAllowOverwrite, OutError))
		{
			return false;
		}

		OutAllowOverwrite = bReplaceExisting || bAllowOverwrite;
		OutReplaceExisting = OutAllowOverwrite;
		return true;
	}

	inline bool EnsureImportTargetCanBeWritten(
		const FString& DestinationPath,
		const FString& DestinationName,
		const FString& SourceFilename,
		bool bReplaceExisting,
		FString& OutError)
	{
		FString AssetName = DestinationName.TrimStartAndEnd();
		if (AssetName.IsEmpty())
		{
			AssetName = FPaths::GetBaseFilename(SourceFilename);
		}
		const FString AssetPath = DestinationPath / AssetName;
		if (!FPackageName::IsValidLongPackageName(AssetPath))
		{
			OutError = TEXT("invalid_import_target_asset_path");
			return false;
		}
		if (bReplaceExisting)
		{
			return true;
		}

		const FString ObjectPath = AssetPath + TEXT(".") + AssetName;
		if (LoadObject<UObject>(nullptr, *ObjectPath))
		{
			OutError = TEXT("import_target_exists_requires_allow_overwrite");
			return false;
		}
		return true;
	}

	inline bool IsStrictDescendantOf(const FString& CandidatePath, const FString& RootPath)
	{
		FString Candidate = FPaths::ConvertRelativePathToFull(CandidatePath);
		FString Root = FPaths::ConvertRelativePathToFull(RootPath);
		FPaths::NormalizeDirectoryName(Candidate);
		FPaths::NormalizeDirectoryName(Root);
		if (Candidate.Equals(Root, ESearchCase::IgnoreCase))
		{
			return false;
		}
		Root += TEXT("/");
		return Candidate.StartsWith(Root, ESearchCase::IgnoreCase);
	}

	inline TArray<FString> GetOwnedAuthoringArtifactRoots()
	{
		return {
			FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("UAI")),
			FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("UeAssetFolders")),
			FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("UeAgentInterface")),
			FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("runtimeLogs/uai_verification"))
		};
	}

	inline bool ResolveOwnedAuthoringArtifactFolder(
		const FString& RequestedPath,
		FString& OutResolvedPath,
		FString& OutOwnershipRoot,
		FString& OutError)
	{
		OutResolvedPath = FPaths::ConvertRelativePathToFull(RequestedPath);
		FPaths::NormalizeDirectoryName(OutResolvedPath);
		for (FString Root : GetOwnedAuthoringArtifactRoots())
		{
			FPaths::NormalizeDirectoryName(Root);
			if (IsStrictDescendantOf(OutResolvedPath, Root))
			{
				OutOwnershipRoot = Root;
				return true;
			}
		}

		OutError = TEXT("output_folder_outside_uai_authoring_roots");
		return false;
	}
}
