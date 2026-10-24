// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Domains/Rigging/Authoring/UeAgentRiggingAuthoringDescriptors.h"

class FJsonObject;
class UObject;

namespace UeAgentRiggingAuthoring
{
	struct UEAGENTINTERFACE_API FMutationStats
	{
		int32 HierarchyElements = 0;
		int32 GraphNodes = 0;
		int32 GraphLinks = 0;
		int32 Goals = 0;
		int32 Solvers = 0;
		int32 Chains = 0;
		int32 RetargetOps = 0;
		int32 Mappings = 0;
		int32 Poses = 0;
	};

	UEAGENTINTERFACE_API FString NormalizePackagePath(const FString& InPath);
	UEAGENTINTERFACE_API FString ToObjectPath(const FString& PackagePath);
	UEAGENTINTERFACE_API UObject* LoadAsset(EAssetKind Kind, const FString& InPath);
	UEAGENTINTERFACE_API UObject* CreateFactorySeed(
		EAssetKind Kind,
		UObject* Outer,
		FName Name,
		EObjectFlags Flags,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool BuildDocument(
		EAssetKind Kind,
		UObject& Asset,
		TSharedPtr<FJsonObject>& OutDocument,
		UE::UAI::Contract::FDiagnostic& OutError,
		const FString& CanonicalAssetPath = FString());
	UEAGENTINTERFACE_API bool ApplyDocument(
		EAssetKind Kind,
		UObject& Asset,
		const TSharedPtr<const FJsonObject>& Document,
		FMutationStats& OutStats,
		UE::UAI::Contract::FDiagnostic& OutError,
		const TSharedPtr<const FJsonObject>& CapturedDocument = nullptr,
		const FString& CanonicalAssetPath = FString());
	UEAGENTINTERFACE_API bool CompileAsset(EAssetKind Kind, UObject& Asset, UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool SaveAsset(EAssetKind Kind, UObject& Asset, UE::UAI::Contract::FDiagnostic& OutError);
}
