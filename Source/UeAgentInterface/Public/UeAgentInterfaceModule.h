// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/UnrealString.h"
#include "Delegates/Delegate.h"
#include "Modules/ModuleManager.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"

class FUeAgentHttpServer;
class IConsoleObject;
class SWindow;

struct FToolMenuContext;

class UEAGENTINTERFACE_API FUeAgentInterfaceModule final : public IModuleInterface
{
public:
	FUeAgentInterfaceModule();

	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void RegisterMenus();
	void UnregisterMenus();

	void StartServerFromUI(const FToolMenuContext& Context);
	void StopServerFromUI(const FToolMenuContext& Context);
	void CopyConnectionInfoToClipboard(const FToolMenuContext& Context);

	void RegisterConsoleCommands();
	void UnregisterConsoleCommands();

	void ConsoleStartServer();
	void ConsoleStopServer();
	void ConsoleRestartServer();

	void InstallStartMinimizedMainFrameHookIfRequested();
	void UninstallStartMinimizedMainFrameHook();
	void HandleMainFrameCreationFinishedForStartMinimized(TSharedPtr<SWindow> MainFrameWindow, bool bShowStartupDialogInPlaceOfMainEditor);
	void ApplyTemporaryRootWindowMaximizeOverride();
	void RestoreTemporaryRootWindowMaximizeOverride();

	bool EnsureAuthToken() const;
	bool StartServerInternal();
	void StopServerInternal();

	TUniquePtr<FUeAgentHttpServer> Server;

	IConsoleObject* ConsoleCmdStart;
	IConsoleObject* ConsoleCmdStop;
	IConsoleObject* ConsoleCmdRestart;

	FDelegateHandle MainFrameCreationFinishedForStartMinimizedHandle;
	FString PreviousRootWindowInitiallyMaximizedValue;
	bool bAppliedRootWindowMaximizeOverride;
	bool bHadPreviousRootWindowInitiallyMaximizedValue;
};
