// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentInterfaceSettings.h"

UUeAgentInterfaceSettings::UUeAgentInterfaceSettings()
	: ListenAddress(TEXT("127.0.0.1"))
	, Port(17777)
	, AuthToken()
	, ArtifactsDirRelative(TEXT("Saved/RemoteArtifacts"))
	, ImportRootDirs()
	, DefaultImportDestPath(TEXT("/Game/Imported"))
{
}

const UUeAgentInterfaceSettings* UUeAgentInterfaceSettings::Get()
{
	return GetDefault<UUeAgentInterfaceSettings>();
}

UUeAgentInterfaceSettings* UUeAgentInterfaceSettings::GetMutable()
{
	return GetMutableDefault<UUeAgentInterfaceSettings>();
}
