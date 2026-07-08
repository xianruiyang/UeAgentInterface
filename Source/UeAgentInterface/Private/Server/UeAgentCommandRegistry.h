// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Server/UeAgentHttpServerTypes.h"

class FUeAgentHttpServer;

namespace UeAgentCommandRegistry
{
	using FExactCommandInvoker = bool (*)(const FUeAgentHttpServer*, const FUeAgentRequestContext&, TSharedPtr<FJsonObject>&, TArray<TSharedPtr<FJsonValue>>&, FString&);
	using FPrefixedCommandInvoker = bool (*)(const FUeAgentHttpServer*, const FString&, const FUeAgentRequestContext&, TSharedPtr<FJsonObject>&, FString&);

	UEAGENTINTERFACE_API FExactCommandInvoker FindExactCommandInvoker(const FString& CommandLower);
	UEAGENTINTERFACE_API FPrefixedCommandInvoker FindPrefixedCommandInvoker(const FString& CommandLower);
}
