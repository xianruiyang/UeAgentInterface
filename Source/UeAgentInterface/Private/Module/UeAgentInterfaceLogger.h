// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

DECLARE_LOG_CATEGORY_EXTERN(LogUeAgentInterface, Log, All);

class FUeAgentInterfaceLogger
{
public:
	static void Init();
	static void Log(const TCHAR* Format, ...);
	static FString GetLogFilePath();
	static FString GetVersionTag();

private:
	static void EnsureInitialized();

	static FCriticalSection Mutex;
	static bool bInitialized;
	static FString LogFilePath;
	static FString VersionTag;
};
