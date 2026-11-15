// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringProfileState.h"

class FJsonValue;
class UObject;

namespace UeAgentReflectionAuthoring
{
	struct FProfile;

	namespace ProfileStateAI
	{
		UEAGENTINTERFACE_API bool Supports(const FProfile& Profile);
		UEAGENTINTERFACE_API bool Validate(
			const FProfile& Profile,
			const TSharedPtr<FJsonValue>& Value,
			UE::UAI::Contract::FDiagnostic& OutError);
		UEAGENTINTERFACE_API bool Read(
			const FProfile& Profile,
			UObject& Asset,
			TSharedPtr<FJsonValue>& OutValue,
			UE::UAI::Contract::FDiagnostic& OutError);
		bool Read(const FProfile& Profile, UObject& Asset, TSharedPtr<FJsonValue>& OutValue,
			UE::UAI::Contract::FDiagnostic& OutError, TOptional<UeAgentAuthoringState::FStateSnapshot>* OutStructure);
		UEAGENTINTERFACE_API bool Apply(
			const FProfile& Profile,
			UObject& Asset,
			const TSharedPtr<FJsonValue>& Value,
			UE::UAI::Contract::FDiagnostic& OutError);
		UEAGENTINTERFACE_API bool Apply(
			const FProfile& Profile,
			UObject& Asset,
			const TSharedPtr<FJsonValue>& Value,
			UE::UAI::Contract::FDiagnostic& OutError, ProfileState::FIntentVerifier* OutIntent);
	}
}
