// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintMutationModel.h"

class FJsonObject;
class UWidgetBlueprint;

namespace UeAgentUmgAuthoring
{
	struct UEAGENTINTERFACE_API FMutationCounters
	{
		int32 WidgetsAdded = 0;
		int32 WidgetsUpdated = 0;
		int32 WidgetsRemoved = 0;
		int32 WidgetPropertiesApplied = 0;
		int32 SlotPropertiesApplied = 0;
		int32 BindingsAdded = 0;
		int32 BindingsUpdated = 0;
		int32 BindingsRemoved = 0;
		int32 AnimationsAdded = 0;
		int32 AnimationsUpdated = 0;
		int32 AnimationsRemoved = 0;
		int32 AnimationKeysApplied = 0;
		UeAgentBlueprintAuthoring::FMutationCounters Blueprint;

		void Add(const FMutationCounters& Other);
	};

	class UEAGENTINTERFACE_API FPreparedUmgMutation
	{
	public:
		~FPreparedUmgMutation();

		FPreparedUmgMutation(FPreparedUmgMutation&&) = delete;
		FPreparedUmgMutation& operator=(FPreparedUmgMutation&&) = delete;
		FPreparedUmgMutation(const FPreparedUmgMutation&) = delete;
		FPreparedUmgMutation& operator=(const FPreparedUmgMutation&) = delete;

		static bool Prepare(
			UWidgetBlueprint& ScratchBlueprint,
			const TSharedPtr<const FJsonObject>& ProviderExtension,
			TUniquePtr<FPreparedUmgMutation>& OutMutation,
			FMutationCounters& OutExpectedCounters,
			UE::UAI::Contract::FDiagnostic& OutError,
			const FString& CanonicalAssetPath = FString());

		bool ApplyTo(
			UWidgetBlueprint& Blueprint,
			FMutationCounters& OutCounters,
			UE::UAI::Contract::FDiagnostic& OutError) const;
		bool FinalizeAfterCompile(
			UWidgetBlueprint& Blueprint,
			FMutationCounters& OutCounters,
			bool& bOutRequiresSecondCompile,
			UE::UAI::Contract::FDiagnostic& OutError) const;
		static bool RestoreFromBackup(
			const UWidgetBlueprint& Backup,
			UWidgetBlueprint& Target,
			UE::UAI::Contract::FDiagnostic& OutError);
		bool RequiresCompile() const;
		bool VerifyPropertyIntent(UWidgetBlueprint& Blueprint, UE::UAI::Contract::FDiagnostic& OutError) const;
		bool VerifyIntent(UWidgetBlueprint& Blueprint, UE::UAI::Contract::FDiagnostic& OutError) const;

	private:
		struct FImpl;
		explicit FPreparedUmgMutation(TUniquePtr<FImpl> InImpl);

		TUniquePtr<FImpl> Impl;
	};
}
