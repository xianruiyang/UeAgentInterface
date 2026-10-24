// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringCore/UeAgentAuthoringAssetIdentity.h"

#include "Misc/PackageName.h"

namespace UeAgentAuthoringAssetIdentity
{
	FString NormalizePackagePath(const FString& InPath)
	{
		FString Result = InPath.TrimStartAndEnd();
		Result.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);
		if (Result.Contains(TEXT(".")))
		{
			Result = FPackageName::ObjectPathToPackageName(Result);
		}
		while (Result.EndsWith(TEXT("/")))
		{
			Result.LeftChopInline(1, EAllowShrinking::No);
		}
		return Result;
	}

	FString ToObjectPath(const FString& InPath)
	{
		const FString PackagePath = NormalizePackagePath(InPath);
		if (PackagePath.IsEmpty())
		{
			return FString();
		}
		const FString AssetName =
			FPackageName::GetLongPackageAssetName(PackagePath);
		return AssetName.IsEmpty()
			? PackagePath
			: FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
	}
}
