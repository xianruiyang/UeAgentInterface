// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"

class FJsonObject;
class FProperty;
class FScriptArrayHelper;
class UObject;
class UStruct;

namespace UeAgentAuthoringTargetLocator
{
	using UE::UAI::Contract::FDiagnostic;
	/** Establish a prospective native element without overflowing the array count. */
	UEAGENTINTERFACE_API bool EnsureArrayElement(FScriptArrayHelper& Array, int32 Index, FDiagnostic& OutError);

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
		FDiagnostic Diagnostic;
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
		// Actual leaf container type, which can derive from the property's declaring struct.
		const UStruct* PropertyScope = nullptr;
		// First declaration in the requested root, retained across owned UObject boundaries.
		FProperty* RootProperty = nullptr;
		void* ValuePtr = nullptr;
		UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
		// Relative to NotificationOwner; empty for independent struct data.
		TArray<FProperty*> NotificationChain;
	};

	UEAGENTINTERFACE_API bool ResolveExact(
		const FTargetQuery& Query,
		const TArray<UeAgentAuthoringIdentity::FCanonicalIdentity>& Candidates,
		FResolveResult& OutResult,
		int32 MaxDiagnosticCandidates = -1,
		const UeAgentAuthoringIdentity::FRegistry* Registry = nullptr);

	/** Validate one current identity through the same exact-resolution rules, without a success candidate copy. */
	UEAGENTINTERFACE_API bool ValidateExact(
		const FTargetQuery& Query,
		const UeAgentAuthoringIdentity::FCanonicalIdentity& Candidate,
		FDiagnostic& OutError,
		const UeAgentAuthoringIdentity::FRegistry* Registry = nullptr);

	/** Validate a mutable current identity with the original key/model rules,
	 * borrowing its fields instead of manufacturing an owning self-query. */
	UEAGENTINTERFACE_API bool ValidateIdentity(
		const UeAgentAuthoringIdentity::FCanonicalIdentity& Identity,
		FDiagnostic& OutError,
		const UeAgentAuthoringIdentity::FRegistry* Registry = nullptr);

	UEAGENTINTERFACE_API bool BuildPropertyIdentity(
		const UObject* RootObject,
		const FString& PropertyPath,
		const UObject* NotificationOwner,
		UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity,
		FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool BuildPropertyIdentity(
		const FString& RootOwnerPath,
		const FString& PropertyPath,
		const FString& NotificationOwnerPath,
		UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity,
		FDiagnostic& OutError);

	/** Same current property source and identity grammar, comparison-only.
	 * OutActualKey is produced only for a real mismatch diagnostic. */
	bool ComparePropertyIdentity(const FString& RootOwnerPath, const FString& PropertyPath,
		const FString& NotificationOwnerPath, const FString& ExpectedKey,
		bool& OutMatches, FString& OutActualKey, FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool ResolvePropertyPath(
		UObject* RootObject,
		const FString& PropertyPath,
		FResolvedPropertyTarget& OutTarget,
		FResolveResult& OutResult,
		int32 MaxDiagnosticCandidates = -1,
		bool bAllowArrayAppend = false);

	UEAGENTINTERFACE_API bool ResolvePropertyPath(
		UStruct* RootStruct,
		void* RootData,
		const FString& RootOwnerPath,
		UObject* NotificationOwner,
		const FString& PropertyPath,
		FResolvedPropertyTarget& OutTarget,
		FResolveResult& OutResult,
		int32 MaxDiagnosticCandidates = -1,
		bool bAllowArrayAppend = false);
}
