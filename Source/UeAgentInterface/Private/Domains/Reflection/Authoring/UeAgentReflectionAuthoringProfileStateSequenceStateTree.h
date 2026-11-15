// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FJsonValue;
class UObject;
class UPackage;
namespace UeAgentAuthoringState { class FStateSnapshot; }
namespace UE::UAI::Authoring::Timeline { struct FMovieSceneCreationState; }

namespace UeAgentReflectionAuthoring
{
	struct FProfile;

	namespace ProfileStateSequenceStateTree
	{
		UEAGENTINTERFACE_API bool Supports(const FProfile& Profile);
		UEAGENTINTERFACE_API bool Validate(
			const FProfile& Profile,
			const TSharedPtr<FJsonValue>& Value,
			UE::UAI::Contract::FDiagnostic& OutError);
		UEAGENTINTERFACE_API bool Read(
			const FProfile& Profile,
			UObject& Asset,
			const FString& SemanticAssetPath,
			TSharedPtr<FJsonValue>& OutValue,
			bool& bOutAuthoringVisible,
			UE::UAI::Contract::FDiagnostic& OutError);
		bool Read(const FProfile& Profile, UObject& Asset, const FString& SemanticAssetPath, TSharedPtr<FJsonValue>& OutValue,
			bool& bOutAuthoringVisible, UE::UAI::Contract::FDiagnostic& OutError,
			TOptional<UeAgentAuthoringState::FStateSnapshot>* OutStructure);
		UEAGENTINTERFACE_API bool Apply(
			const FProfile& Profile,
			UObject& Asset,
			const TSharedPtr<FJsonValue>& Value,
			const FString& SemanticAssetPath,
			UE::UAI::Contract::FDiagnostic& OutError,
			bool bPreserveExistingStructure = false,
			UPackage* TextIdentityPackage = nullptr,
			UE::UAI::Authoring::Timeline::FMovieSceneCreationState* MovieSceneCreation = nullptr);
		UEAGENTINTERFACE_API bool CompileIfNeeded(
			const FProfile& Profile,
			UObject& Asset,
			UE::UAI::Contract::FDiagnostic& OutError);
	}
}
