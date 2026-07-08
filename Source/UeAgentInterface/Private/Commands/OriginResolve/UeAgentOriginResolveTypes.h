// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace UeAgentOriginResolve
{
	struct FResolveParams
	{
		FString TargetSource;
		FString Kind;
		FString Name;
		FString FullName;
		FString CandidateId;
		FString FolderPath;
		FString JsonFile;
		FString JsonPointer;
		FString AssetPath;
		FString GraphName;
		FString NodeGuid;
	};

	struct FCodePath
	{
		FCodePath();

		FString Path;
		int32 Line;
		int32 Column;
		FString Symbol;
		FString Module;
		FString Status;
	};

	struct FResolveResult
	{
		FResolveResult();

		bool bResolved;
		FString Status;
		FString Kind;
		FString Name;
		FString FullName;
		TArray<FString> AssetPaths;
		TArray<FString> PackagePaths;
		TArray<FString> PackageFiles;
		TArray<FCodePath> CodePaths;
		TArray<FString> Warnings;
	};

	bool ParseResolveParams(const TSharedPtr<FJsonObject>& Params, FResolveParams& OutParams, FString& OutError);
	bool ResolveOrigin(const FResolveParams& Params, FResolveResult& OutResult, FString& OutError);
	TSharedRef<FJsonObject> BuildResolveResponse(const FResolveParams& Params, const FResolveResult& Result);
}
