// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringProfileState.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringProfileStateAI.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringProfileStateSequenceStateTree.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringProfileStateTextureGraph.h"

namespace UeAgentReflectionAuthoring::ProfileState
{
	namespace
	{
		bool ValidateWithOwner(
			const FProfile& Profile,
			const TSharedPtr<FJsonValue>& Patch,
			FString& OutError)
		{
			if (ProfileStateAI::Supports(Profile))
			{
				return ProfileStateAI::Validate(Profile, Patch, OutError);
			}
			if (ProfileStateSequenceStateTree::Supports(Profile))
			{
				return ProfileStateSequenceStateTree::Validate(Profile, Patch, OutError);
			}
			if (ProfileStateTextureGraph::Supports(Profile))
			{
				return ProfileStateTextureGraph::Validate(Profile, Patch, OutError);
			}
			OutError = TEXT("reflection_profile_state_surface_missing:") + Profile.TypeId;
			return false;
		}
	}

	bool HasSurface(const FProfile& Profile)
	{
		return !Profile.ProfileStateSemanticId.IsEmpty()
			&& !Profile.ProfileStateField.IsEmpty()
			&& (ProfileStateAI::Supports(Profile)
				|| ProfileStateSequenceStateTree::Supports(Profile)
				|| ProfileStateTextureGraph::Supports(Profile));
	}

	bool PreparePatch(
		const FProfile& Profile,
		const TSharedPtr<const FJsonObject>& ProviderExtension,
		TSharedPtr<FJsonValue>& OutPatch,
		bool& bOutHasPatch,
		FString& OutError)
	{
		OutPatch.Reset();
		bOutHasPatch = false;
		OutError.Reset();
		if (!HasSurface(Profile) || !ProviderExtension.IsValid())
		{
			return true;
		}
		const TSharedPtr<FJsonValue>* Found = ProviderExtension->Values.Find(Profile.ProfileStateField);
		if (!Found)
		{
			return true;
		}
		if (!Found->IsValid())
		{
			OutError = TEXT("reflection_profile_state_value_invalid:") + Profile.TypeId;
			return false;
		}
		OutPatch = *Found;
		if (!ValidateWithOwner(Profile, OutPatch, OutError))
		{
			OutPatch.Reset();
			return false;
		}
		bOutHasPatch = true;
		return true;
	}

	bool Read(
		const FProfile& Profile,
		UObject& Asset,
		TSharedPtr<FJsonValue>& OutValue,
		FString& OutError)
	{
		OutValue.Reset();
		OutError.Reset();
		if (ProfileStateAI::Supports(Profile))
		{
			return ProfileStateAI::Read(Profile, Asset, OutValue, OutError);
		}
		if (ProfileStateSequenceStateTree::Supports(Profile))
		{
			return ProfileStateSequenceStateTree::Read(Profile, Asset, OutValue, OutError);
		}
		if (ProfileStateTextureGraph::Supports(Profile))
		{
			return ProfileStateTextureGraph::Read(Profile, Asset, OutValue, OutError);
		}
		OutError = TEXT("reflection_profile_state_surface_missing:") + Profile.TypeId;
		return false;
	}

	bool Apply(
		const FProfile& Profile,
		UObject& Asset,
		const TSharedPtr<FJsonValue>& Patch,
		FString& OutError)
	{
		OutError.Reset();
		if (ProfileStateAI::Supports(Profile))
		{
			return ProfileStateAI::Apply(Profile, Asset, Patch, OutError);
		}
		if (ProfileStateSequenceStateTree::Supports(Profile))
		{
			return ProfileStateSequenceStateTree::Apply(Profile, Asset, Patch, OutError);
		}
		if (ProfileStateTextureGraph::Supports(Profile))
		{
			return ProfileStateTextureGraph::Apply(Profile, Asset, Patch, OutError);
		}
		OutError = TEXT("reflection_profile_state_surface_missing:") + Profile.TypeId;
		return false;
	}

	bool CompileIfNeeded(
		const FProfile& Profile,
		UObject& Asset,
		FString& OutError)
	{
		OutError.Reset();
		if (ProfileStateSequenceStateTree::Supports(Profile))
		{
			return ProfileStateSequenceStateTree::CompileIfNeeded(Profile, Asset, OutError);
		}
		return true;
	}
}
