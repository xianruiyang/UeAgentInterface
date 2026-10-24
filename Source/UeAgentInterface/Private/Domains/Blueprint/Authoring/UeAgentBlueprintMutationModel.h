// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"

class FJsonObject;
class UBlueprint;
class AActor;
struct FEdGraphPinType;

namespace UeAgentBlueprintAuthoring
{
	using UE::UAI::Contract::FDiagnostic;
	/** Native member mutation without the editor's interactive confirmation.
	 * The caller owns rollback/persistence of the Blueprint and loaded consumers. */
	UEAGENTINTERFACE_API bool ChangeNativeMemberType(UBlueprint& Blueprint,
		FName VariableName, const FEdGraphPinType& DesiredType, FDiagnostic& OutError);
	UEAGENTINTERFACE_API void CollectNativeMemberTypeDependents(UBlueprint& Blueprint,
		FName VariableName, TArray<UBlueprint*>& OutBlueprints);

	struct UEAGENTINTERFACE_API FMutationCounters
	{
		int32 VariablesAdded = 0;
		int32 VariablesUpdated = 0;
		int32 VariablesRemoved = 0;
		int32 DelegatesAdded = 0;
		int32 DelegatesUpdated = 0;
		int32 DelegatesRemoved = 0;
		int32 InterfacesAdded = 0;
		int32 InterfacesUpdated = 0;
		int32 InterfacesRemoved = 0;
		int32 ComponentsAdded = 0;
		int32 ComponentsUpdated = 0;
		int32 ComponentsRemoved = 0;
		int32 GraphsAdded = 0;
		int32 GraphsUpdated = 0;
		int32 GraphsRemoved = 0;
		int32 NodesAdded = 0;
		int32 NodesUpdated = 0;
		int32 NodesRemoved = 0;
		int32 PinGroupsResized = 0;
		int32 PinDefaultsApplied = 0;
		int32 EdgesAdded = 0;
		int32 EdgesRemoved = 0;
		int32 ClassDefaultsApplied = 0;

		void Add(const FMutationCounters& Other);
	};

	class UEAGENTINTERFACE_API FPreparedBlueprintMutation
	{
	public:
		~FPreparedBlueprintMutation();

		FPreparedBlueprintMutation(FPreparedBlueprintMutation&&) = delete;
		FPreparedBlueprintMutation& operator=(FPreparedBlueprintMutation&&) = delete;
		FPreparedBlueprintMutation(const FPreparedBlueprintMutation&) = delete;
		FPreparedBlueprintMutation& operator=(const FPreparedBlueprintMutation&) = delete;

		static bool Prepare(
			UBlueprint& ScratchBlueprint,
			const TSharedPtr<const FJsonObject>& ProviderExtension,
			TUniquePtr<FPreparedBlueprintMutation>& OutMutation,
			FMutationCounters& OutExpectedCounters,
			FDiagnostic& OutError);

		/** Read-only graph name identity and native rename effects, before scratch or asset writes. */
		static bool ValidateGraphNames(
			UBlueprint& Blueprint,
			const TSharedPtr<const FJsonObject>& ProviderExtension,
			FDiagnostic& OutError);

		/** Read-only delegate member/signature rename qualification and native effects. */
		static bool ValidateMemberNames(
			UBlueprint& Blueprint,
			const TSharedPtr<const FJsonObject>& ProviderExtension,
			FDiagnostic& OutError);

		/** Native CustomEvent name qualification, before any source or scratch mutation. */
		static bool ValidateCustomEventNames(
			UBlueprint& Blueprint,
			const TSharedPtr<const FJsonObject>& ProviderExtension,
			FDiagnostic& OutError);

		/** Validate identities and native deletion/type-change eligibility. */
		static bool ValidateRemovals(
			UBlueprint& Blueprint,
			const TSharedPtr<const FJsonObject>& ProviderExtension,
			FDiagnostic& OutError,
			bool bIncludeExternalConsumers = true);

		/** Already-loaded objects changed by native member/component reconstruction. */
		static bool CollectNativeDependentBlueprints(UBlueprint& Blueprint,
			const TSharedPtr<const FJsonObject>& ProviderExtension,
			TArray<UBlueprint*>& OutBlueprints, TArray<AActor*>& OutActors, FDiagnostic& OutError);

		bool ApplyTo(
			UBlueprint& Blueprint,
			FMutationCounters& OutCounters,
			FDiagnostic& OutError) const;

		bool FinalizeAfterCompile(
			UBlueprint& Blueprint,
			FMutationCounters& OutCounters,
			FDiagnostic& OutError) const;

		static bool RestoreFromBackup(
			const UBlueprint& Backup,
			UBlueprint& Target,
			FDiagnostic& OutError);

		bool RequiresCompile() const;
		bool VerifyPropertyIntent(UBlueprint& Blueprint, FDiagnostic& OutError) const;
		/** Frozen property and relationship intent, rebound to the current native owner. */
		bool VerifyIntent(UBlueprint& Blueprint, FDiagnostic& OutError) const;

	private:
		struct FImpl;

		explicit FPreparedBlueprintMutation(TUniquePtr<FImpl> InImpl);

		TUniquePtr<FImpl> Impl;
	};
}
