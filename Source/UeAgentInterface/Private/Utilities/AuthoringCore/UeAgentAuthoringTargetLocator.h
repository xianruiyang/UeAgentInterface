// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"

class FJsonObject;
class FProperty;
class UObject;
class UStruct;

namespace UeAgentAuthoringTargetLocator
{
	struct UEAGENTINTERFACE_API FTargetQuery
	{
		FString ModelId;
		FString TargetKind;
		FString CanonicalKey;
		TMap<FString, FString> Selectors;
	};

	struct UEAGENTINTERFACE_API FDiagnosticCandidate
	{
		int32 SourceIndex = INDEX_NONE;
		int32 MatchedCriteria = 0;
		UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
	};

	struct UEAGENTINTERFACE_API FResolveResult
	{
		FString Status;
		FString ErrorCode;
		int32 ResolvedIndex = INDEX_NONE;
		TArray<FString> ConflictingSelectors;
		TArray<FString> RequiredDiscriminators;
		TArray<FDiagnosticCandidate> Candidates;

		bool IsResolved() const;
		TSharedRef<FJsonObject> ToJsonObject() const;
	};

	struct UEAGENTINTERFACE_API FResolvedPropertyTarget
	{
		UObject* NotificationOwner = nullptr;
		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
	};

	UEAGENTINTERFACE_API bool ResolveExact(
		const FTargetQuery& Query,
		const TArray<UeAgentAuthoringIdentity::FCanonicalIdentity>& Candidates,
		FResolveResult& OutResult,
		int32 MaxDiagnosticCandidates = 8,
		const UeAgentAuthoringIdentity::FRegistry* Registry = nullptr);

	UEAGENTINTERFACE_API bool BuildPropertyIdentity(
		const UObject* RootObject,
		const FString& PropertyPath,
		const UObject* NotificationOwner,
		UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity,
		FString& OutError);

	UEAGENTINTERFACE_API bool BuildPropertyIdentity(
		const FString& RootOwnerPath,
		const FString& PropertyPath,
		const FString& NotificationOwnerPath,
		UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity,
		FString& OutError);

	UEAGENTINTERFACE_API bool ResolvePropertyPath(
		UObject* RootObject,
		const FString& PropertyPath,
		FResolvedPropertyTarget& OutTarget,
		FResolveResult& OutResult,
		int32 MaxDiagnosticCandidates = 8);

	UEAGENTINTERFACE_API bool ResolvePropertyPath(
		UStruct* RootStruct,
		void* RootData,
		const FString& RootOwnerPath,
		UObject* NotificationOwner,
		const FString& PropertyPath,
		FResolvedPropertyTarget& OutTarget,
		FResolveResult& OutResult,
		int32 MaxDiagnosticCandidates = 8);
}
