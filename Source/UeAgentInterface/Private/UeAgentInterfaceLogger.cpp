// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentInterfaceLogger.h"

#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

FCriticalSection FUeAgentInterfaceLogger::Mutex;
bool FUeAgentInterfaceLogger::bInitialized = false;
FString FUeAgentInterfaceLogger::LogFilePath;
FString FUeAgentInterfaceLogger::VersionTag;

FString FUeAgentInterfaceLogger::GetLogFilePath()
{
	EnsureInitialized();
	return LogFilePath;
}

FString FUeAgentInterfaceLogger::GetVersionTag()
{
	EnsureInitialized();
	return VersionTag;
}

void FUeAgentInterfaceLogger::Init()
{
	FScopeLock Lock(&Mutex);
	bInitialized = false;
	LogFilePath.Reset();
	VersionTag.Reset();

	const FString Hostname = FPlatformProcess::ComputerName();
	const FString LogsDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Logs"));
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	PF.CreateDirectoryTree(*LogsDir);

	LogFilePath = FPaths::Combine(LogsDir, FString::Printf(TEXT("ueagentinterface_%s.log"), *Hostname));
	VersionTag = FDateTime::Now().ToString(TEXT("%Y-%m-%dT%H:%M:%S.%s"));

	FString Header;
	Header += FString::Printf(TEXT("VERSION_TAG=%s\n"), *VersionTag);
	Header += FString::Printf(TEXT("HOSTNAME=%s\n"), *Hostname);
	Header += FString::Printf(TEXT("BUILD=%s %s\n"), TEXT(__DATE__), TEXT(__TIME__));

	FFileHelper::SaveStringToFile(Header, *LogFilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	bInitialized = true;
}

void FUeAgentInterfaceLogger::EnsureInitialized()
{
	if (!bInitialized)
	{
		Init();
	}
}

void FUeAgentInterfaceLogger::Log(const TCHAR* Format, ...)
{
	EnsureInitialized();

	va_list Args;
	va_start(Args, Format);

	TCHAR Buffer[4096];
	FCString::GetVarArgs(Buffer, UE_ARRAY_COUNT(Buffer), Format, Args);
	va_end(Args);

	const FString Line = FString::Printf(TEXT("[%s] %s\n"), *FDateTime::Now().ToString(TEXT("%H:%M:%S.%s")), Buffer);

	FScopeLock Lock(&Mutex);
	FFileHelper::SaveStringToFile(Line, *LogFilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append);
}

