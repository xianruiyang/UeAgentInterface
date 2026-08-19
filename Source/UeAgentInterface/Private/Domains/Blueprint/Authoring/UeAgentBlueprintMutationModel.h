// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UBlueprint;

namespace UeAgentBlueprintAuthoring
{
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
			FString& OutError);

		bool ApplyTo(
			UBlueprint& Blueprint,
			FMutationCounters& OutCounters,
			FString& OutError) const;

		bool FinalizeAfterCompile(
			UBlueprint& Blueprint,
			FMutationCounters& OutCounters,
			FString& OutError) const;

		static bool RestoreFromBackup(
			const UBlueprint& Backup,
			UBlueprint& Target,
			FString& OutError);

		bool RequiresCompile() const;

	private:
		struct FImpl;

		explicit FPreparedBlueprintMutation(TUniquePtr<FImpl> InImpl);

		TUniquePtr<FImpl> Impl;
	};
}
