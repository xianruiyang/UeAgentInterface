// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;
class UObject;
class UClass;
class UPackage;
namespace UeAgentAuthoringState { class FStateSnapshot; }
namespace UE::UAI::Authoring::Timeline { struct FMovieSceneCreationState; }

namespace UeAgentReflectionAuthoring
{
	struct FProfile;

	namespace ProfileState
	{
		// The domain binds immutable original intent to the current native owner.
		// No native storage pointers survive compile or package replacement here.
		using FIntentVerifier = TFunction<bool(UObject&, UE::UAI::Contract::FDiagnostic&)>;
		UEAGENTINTERFACE_API bool HasSurface(const FProfile& Profile);
		UE::UAI::Contract::FDiagnostic TargetFailure(const FString& Code, const UObject& Asset, const UClass& Expected,
			const FString& Stage = TEXT("profile_state"));
		void BindTarget(UE::UAI::Contract::FDiagnostic& Error, const FString& NativePath, const FString& SemanticAssetPath);
		// Preserve source ownership when profile values overlay captured native defaults.
		void BindInput(UE::UAI::Contract::FDiagnostic& Error, const TSharedPtr<FJsonValue>& Input);
		void MarkNative(UE::UAI::Contract::FDiagnostic& Error);
		void PrefixInputLocation(UE::UAI::Contract::FDiagnostic& Error, const FString& Prefix);
		void AddRelatedInput(UE::UAI::Contract::FDiagnostic& Error, const TSharedPtr<FJsonValue>& Input, const FString& Pointer);
		void LocateSettingsFailure(UE::UAI::Contract::FDiagnostic& Error, const TSharedPtr<FJsonValue>& Settings,
			const FString& Pointer, const TSharedPtr<FJsonValue>& Input);
		// Existing-target preview only; published native Clone keeps its own identity contract.
		bool InitializeUpdatePreview(UObject& Source, UObject& Preview, const FString& SemanticAssetPath,
			UE::UAI::Contract::FDiagnostic& OutError);

		UEAGENTINTERFACE_API bool PreparePatch(
			const FProfile& Profile,
			const TSharedPtr<const FJsonObject>& ProviderExtension,
			TSharedPtr<FJsonValue>& OutPatch,
			bool& bOutHasPatch,
			UE::UAI::Contract::FDiagnostic& OutError);

		UEAGENTINTERFACE_API bool Read(
			const FProfile& Profile,
			UObject& Asset,
			const FString& SemanticAssetPath,
			TSharedPtr<FJsonValue>& OutValue,
			bool& bOutAuthoringVisible,
			UE::UAI::Contract::FDiagnostic& OutError);
		bool Read(const FProfile& Profile, UObject& Asset, const FString& SemanticAssetPath,
			TSharedPtr<FJsonValue>& OutValue, bool& bOutAuthoringVisible,
			UE::UAI::Contract::FDiagnostic& OutError, TOptional<UeAgentAuthoringState::FStateSnapshot>* OutStructure);

		UEAGENTINTERFACE_API bool Apply(
			const FProfile& Profile,
			UObject& Asset,
			const TSharedPtr<FJsonValue>& Patch,
			const FString& SemanticAssetPath,
			UE::UAI::Contract::FDiagnostic& OutError,
			bool bPreserveExistingStructure = false,
			UPackage* TextIdentityPackage = nullptr,
			UE::UAI::Authoring::Timeline::FMovieSceneCreationState* MovieSceneCreation = nullptr,
			FIntentVerifier* OutIntent = nullptr);

		UEAGENTINTERFACE_API bool CompileIfNeeded(
			const FProfile& Profile,
			UObject& Asset,
			const FString& SemanticAssetPath,
			UE::UAI::Contract::FDiagnostic& OutError);
	}
}
