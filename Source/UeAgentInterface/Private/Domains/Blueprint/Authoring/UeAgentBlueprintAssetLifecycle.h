// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"

class UBlueprint;
class UClass;
class UPackage;
class AActor;

namespace UeAgentBlueprintAssetLifecycle
{
    using UE::UAI::Contract::FDiagnostic;
	// Native node actions may open nested transactions. Keep scratch-only records
	// out of the user's undo history, without ever cancelling an existing transaction.
	class FScratchBlueprintPreflightTransaction
	{
	public:
		FScratchBlueprintPreflightTransaction();
		~FScratchBlueprintPreflightTransaction();
		bool Begin(const FString& TargetPath, FDiagnostic& OutError);
	private:
		struct FImpl;
		TUniquePtr<FImpl> Impl;
	};

	// Own only packages created for this authoring operation. Retire every object
	// in those packages on every exit, including preflight failures before a state exists.
	// Only Blueprint roots are marked garbage; native GC owns class/function/frame order.
	class FScratchBlueprintPackages
	{
	public:
		FScratchBlueprintPackages();
		~FScratchBlueprintPackages();
		UPackage* Create(const TCHAR* Purpose);
		void Reset();
	private:
		struct FImpl;
		TUniquePtr<FImpl> Impl;
	};
	// Flush retired authoring objects at a safe boundary before native PIE scans
	// loaded Blueprints; its error list can still include garbage awaiting GC.
	void CollectRetiredScratchBlueprints();
	// Native component instance data survives template deletion and package reload.
	class FComponentInstanceRollback
	{
	public:
		FComponentInstanceRollback();
		~FComponentInstanceRollback();
		void SetActors(const TArray<AActor*>& Actors);
		void CollectObjects(TArray<UObject*>& OutObjects) const;
		bool Capture(FDiagnostic& OutError);
		bool Restore(FDiagnostic& OutError);
	private:
		struct FImpl;
		TUniquePtr<FImpl> Impl;
	};

	UEAGENTINTERFACE_API UBlueprint* LoadBlueprint(const FString& InPath);
	// The authoring target owns public identity; a working copy owns native mutation.
	UEAGENTINTERFACE_API void SetAuthoringContext(
		UBlueprint& Working, const FString& TargetPath, UBlueprint* ExistingTarget);
	UEAGENTINTERFACE_API bool IsAuthoringWorkingCopy(const UBlueprint& Blueprint);
	UEAGENTINTERFACE_API bool ValidateBlueprintStructure(const UBlueprint& Blueprint, FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool ValidateBlueprintPersistence(const UBlueprint& Blueprint, FDiagnostic& OutError);
	UEAGENTINTERFACE_API FString GetAuthoringTargetPath(const UBlueprint& Blueprint);
	UEAGENTINTERFACE_API FString GetAuthoringClassPath(
		const UBlueprint& Blueprint, const UClass& Class);
	// A type's public self identity is stable across skeleton/generated phases.
	// Other objects retain their native path. This is distinct from native class identity above.
	UEAGENTINTERFACE_API FString GetAuthoringTypeObjectPath(
		const UBlueprint* Blueprint, const UObject& Object,
		const FString& CanonicalAssetPath = FString());
	// Returns true for an owned self identity, even when its working class is unavailable.
	// Callers must not fall back to loading the old published class in that case.
	UEAGENTINTERFACE_API bool TryResolveAuthoringSelfClass(
		UBlueprint& Blueprint, const FString& ClassPath, UClass*& OutClass);
	// Working copy without editor GC keep flags; PublishBlueprint owns durable copies.
	UEAGENTINTERFACE_API UBlueprint* DuplicateBlueprint(
		UBlueprint* Source,
		UObject* Outer,
		FName Name = NAME_None,
		bool bPreserveTextIdentity = false,
		FDiagnostic* OutError = nullptr);
	UEAGENTINTERFACE_API UBlueprint* PublishBlueprint(
		UBlueprint* Source,
		UPackage* Package,
		const FString& Name,
		FDiagnostic& OutError);
	UEAGENTINTERFACE_API void RebindBlueprintSelfMemberReferences(
		UBlueprint& Blueprint);
	UEAGENTINTERFACE_API bool DeleteBlueprint(UBlueprint* Blueprint, const FString& TargetPath, FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool CompileBlueprint(UBlueprint* Blueprint, FDiagnostic& OutError);
	UEAGENTINTERFACE_API FString GetStoredCompileObservationStatus(const UBlueprint& Blueprint);
}
