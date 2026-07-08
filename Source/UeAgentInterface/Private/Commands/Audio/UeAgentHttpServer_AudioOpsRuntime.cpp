// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/Audio/UeAgentHttpServer_AudioOps.h"
#include "Components/AudioComponent.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Sound/SoundAttenuation.h"
#include "UObject/UObjectIterator.h"

namespace UeAgentAudioOps
{
	UWorld* GetEditorWorldDirect()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}
	AActor* FindActorByNameOrLabelDirect(UWorld* World, const FString& NameOrLabel)
	{
		if (!World || NameOrLabel.IsEmpty())
		{
			return nullptr;
		}
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor)
			{
				continue;
			}
			if (Actor->GetName().Equals(NameOrLabel, ESearchCase::IgnoreCase) || Actor->GetActorLabel().Equals(NameOrLabel, ESearchCase::IgnoreCase))
			{
				return Actor;
			}
		}
		return nullptr;
	}
	UAudioComponent* FindAudioComponentInWorld(UWorld* World, const TSharedPtr<FJsonObject>& Params, FString& OutError)
	{
		if (!World)
		{
			OutError = TEXT("world_not_found");
			return nullptr;
		}
		FString ActorName;
		JsonTryGetStringAny(Params, { TEXT("actor"), TEXT("actor_name"), TEXT("actor_label") }, ActorName);
		FString ComponentName;
		JsonTryGetStringAny(Params, { TEXT("component"), TEXT("component_name") }, ComponentName);

		if (!ActorName.IsEmpty())
		{
			AActor* Actor = FindActorByNameOrLabelDirect(World, ActorName);
			if (!Actor)
			{
				OutError = TEXT("actor_not_found");
				return nullptr;
			}
			TArray<UAudioComponent*> Components;
			Actor->GetComponents<UAudioComponent>(Components);
			for (UAudioComponent* Component : Components)
			{
				if (!Component)
				{
					continue;
				}
				if (ComponentName.IsEmpty()
					|| Component->GetName().Equals(ComponentName, ESearchCase::IgnoreCase)
					|| Component->GetReadableName().Equals(ComponentName, ESearchCase::IgnoreCase))
				{
					return Component;
				}
			}
			OutError = TEXT("audio_component_not_found");
			return nullptr;
		}

		for (TObjectIterator<UAudioComponent> It; It; ++It)
		{
			UAudioComponent* Component = *It;
			if (!Component || Component->GetWorld() != World)
			{
				continue;
			}
			if (ComponentName.IsEmpty()
				|| Component->GetName().Equals(ComponentName, ESearchCase::IgnoreCase)
				|| Component->GetReadableName().Equals(ComponentName, ESearchCase::IgnoreCase))
			{
				return Component;
			}
		}
		OutError = TEXT("audio_component_not_found");
		return nullptr;
	}
	void AddAudioComponentSnapshot(UAudioComponent* Component, TSharedPtr<FJsonObject>& Out)
	{
		Out->SetStringField(TEXT("component_name"), Component ? Component->GetName() : FString());
		Out->SetStringField(TEXT("owner"), Component && Component->GetOwner() ? Component->GetOwner()->GetActorLabel() : FString());
		Out->SetStringField(TEXT("sound"), Component ? ObjectPathOrEmpty(Component->Sound) : FString());
		Out->SetBoolField(TEXT("is_playing"), Component ? Component->IsPlaying() : false);
		Out->SetStringField(TEXT("play_state"), Component ? PlayStateToString(Component->GetPlayState()) : FString());
		Out->SetNumberField(TEXT("volume_multiplier"), Component ? Component->VolumeMultiplier : 0.0f);
		Out->SetNumberField(TEXT("pitch_multiplier"), Component ? Component->PitchMultiplier : 0.0f);
		Out->SetBoolField(TEXT("auto_activate"), Component ? Component->bAutoActivate : false);
		Out->SetStringField(TEXT("attenuation"), Component ? ObjectPathOrEmpty(Component->AttenuationSettings) : FString());
	}
}
