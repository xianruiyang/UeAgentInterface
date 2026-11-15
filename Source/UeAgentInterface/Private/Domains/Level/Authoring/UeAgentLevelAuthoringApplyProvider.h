// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceLoader.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"

class AActor;
class FJsonObject;
class UWorld;
class UPackage;

namespace UeAgentAuthoringOperationApply
{
	class IApplyProvider;
}

namespace UeAgentLevelAuthoring
{
	// Resolves the same native placement selectors as Apply without instantiating an actor.
	bool ResolvePlacementSelectors(const TSharedPtr<const FJsonObject>& Extension,
		TMap<FString, FString>& OutSelectors, UE::UAI::Contract::FDiagnostic& OutError);
	struct UEAGENTINTERFACE_API FWorkspaceApplyOptions
	{
		bool bDryRun = false;
		bool bUseEditorTransaction = true;
		bool bPersistentReadback = false;
		// Internal custody from the verified semantic workspace, never a property payload.
		FString AuthoringProfileId;
		TSharedPtr<UeAgentAuthoringOperationApply::IDurablePackageStager> PackageStager;
		TFunction<bool(UeAgentAuthoringOperationApply::FOperationFailure&)> FinalReadback;
		// Only the native move result can replace an existing logical Actor binding.
		TFunction<void(const FGuid&, const AActor&)> OnActorMoved;

		// The Level caller's selected partition references share the native
		// transaction lifetime. nullptr releases them before unload; a reloaded
		// World reestablishes the same GUID closure for actual readback.
		// Non-null package output reads the same native scope's package names;
		// null output retains/releases references for readback as described above.
		TFunction<void(UWorld*, TArray<UPackage*>*)> PartitionReadScope;
		// Actual preflight coverage, not a cache/admission flag. Complete-domain
		// callers retain their existing complete package collection.
		bool bScopedPackageCapture = false;
	};

	struct UEAGENTINTERFACE_API FWorkspaceApplyResult
	{
		FString Status;
		FString Phase;
		UE::UAI::Contract::FDiagnostic Diagnostic;
		int32 UnitsChecked = 0;
		int32 MutationsApplied = 0;
		int32 ActorsCreated = 0;
		int32 ActorsUpdated = 0;
		int32 ActorsRemoved = 0;
		int32 ComponentsCreated = 0;
		int32 ComponentsUpdated = 0;
		int32 ComponentsRemoved = 0;
		bool bRollbackAttempted = false;
		bool bRollbackSucceeded = false;

		bool IsSuccessful() const
		{
			return Status == TEXT("applied") || Status == TEXT("ready");
		}
		TSharedRef<FJsonObject> ToJsonObject() const;
	};

	UEAGENTINTERFACE_API bool RegisterApplyContracts(UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API TSharedRef<UeAgentAuthoringOperationApply::IApplyProvider> MakeApplyProvider();
	UEAGENTINTERFACE_API bool ApplyWorkspaceSource(
		UWorld& World,
		const UeAgentAuthoringWorkspaceLoader::FSourceRequest& Source,
		const FWorkspaceApplyOptions& Options,
		FWorkspaceApplyResult& OutResult,
		const FString& RequestId = FString());
	UEAGENTINTERFACE_API bool ApplyWorkspaceDirectory(
		UWorld& World,
		const FString& WorkspaceRoot,
		const FWorkspaceApplyOptions& Options,
		FWorkspaceApplyResult& OutResult,
		const FString& RequestId = FString());
	UEAGENTINTERFACE_API bool ValidateWorkspaceDirectory(
		UWorld& World,
		const FString& WorkspaceRoot,
		FWorkspaceApplyResult& OutResult);
}
