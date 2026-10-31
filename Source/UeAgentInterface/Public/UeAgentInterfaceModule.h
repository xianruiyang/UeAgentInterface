// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/UnrealString.h"
#include "Delegates/Delegate.h"
#include "Modules/ModuleManager.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"

class IConsoleObject;
class SWindow;

namespace UE::UAI::ServiceComposition::CommandService
{
	class FUeAgentCommandService;
}

namespace UE::UAI::ServiceComposition::Transport
{
	class FUeAgentTransportService;
}

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
	void ConsoleExportCommandManifest();

	void InstallStartMinimizedMainFrameHookIfRequested();
	void UninstallStartMinimizedMainFrameHook();
	void HandleMainFrameCreationFinishedForStartMinimized(TSharedPtr<SWindow> MainFrameWindow, bool bShowStartupDialogInPlaceOfMainEditor);
	void HandleEditorMapOpened(const FString& Filename, bool bAsTemplate);
	void ApplyTemporaryRootWindowMaximizeOverride();
	void RestoreTemporaryRootWindowMaximizeOverride();

	bool EnsureAuthToken() const;
	bool StartServerInternal();
	void StopServerInternal();

	TUniquePtr<
		UE::UAI::ServiceComposition::CommandService::
			FUeAgentCommandService> CommandService;
	TUniquePtr<
		UE::UAI::ServiceComposition::Transport::
			FUeAgentTransportService> TransportService;

	IConsoleObject* ConsoleCmdStart;
	IConsoleObject* ConsoleCmdStop;
	IConsoleObject* ConsoleCmdRestart;
	IConsoleObject* ConsoleCmdExportCommandManifest;

	FDelegateHandle MainFrameCreationFinishedForStartMinimizedHandle;
	FDelegateHandle EditorMapOpenedHandle;
	FDelegateHandle StartPieScratchCleanupHandle;
	FString PreviousRootWindowInitiallyMaximizedValue;
	bool bAppliedRootWindowMaximizeOverride;
	bool bHadPreviousRootWindowInitiallyMaximizedValue;
};
