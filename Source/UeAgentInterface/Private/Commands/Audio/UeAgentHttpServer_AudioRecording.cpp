// Copyright Epic Games, Inc. All Rights Reserved.

#include "Server/UeAgentHttpServer.h"
#include "Commands/Audio/UeAgentHttpServer_AudioOps.h"

#include "Components/AudioComponent.h"
#include "Dom/JsonObject.h"

bool FUeAgentHttpServer::CmdAudioStopAllPreviewSounds(
	const FUeAgentRequestContext& Ctx,
	TSharedPtr<FJsonObject>& OutData,
	FString& OutError) const
{
	using namespace UeAgentAudioOps;
	int32 Stopped = 0;
	for (TWeakObjectPtr<UAudioComponent>& WeakComponent : GPreviewComponents)
	{
		if (UAudioComponent* Component = WeakComponent.Get())
		{
			if (Component->IsPlaying())
			{
				Component->Stop();
				++Stopped;
			}
			Component->DestroyComponent();
		}
	}
	GPreviewComponents.Reset();
	OutData = MakeShared<FJsonObject>();
	OutData->SetNumberField(TEXT("stopped_count"), Stopped);
	OutData->SetBoolField(TEXT("cleanup_complete"), true);
	return true;
}
