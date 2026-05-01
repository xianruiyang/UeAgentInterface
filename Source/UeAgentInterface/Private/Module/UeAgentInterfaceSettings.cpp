// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentInterfaceSettings.h"

const UUeAgentInterfaceSettings* UUeAgentInterfaceSettings::Get()
{
	return GetDefault<UUeAgentInterfaceSettings>();
}

UUeAgentInterfaceSettings* UUeAgentInterfaceSettings::GetMutable()
{
	return GetMutableDefault<UUeAgentInterfaceSettings>();
}

