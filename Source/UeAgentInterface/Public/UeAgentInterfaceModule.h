// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"
#include "ModelingModeToolExtensions.h"

class FUeAgentHttpServer;
class IConsoleObject;

struct FExtensionToolQueryInfo;
struct FExtensionToolDescription;
struct FModelingModeExtensionExtendedInfo;

class FUeAgentInterfaceModule final : public IModuleInterface, public IModelingModeToolExtension
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	virtual FText GetExtensionName() override;
	virtual FText GetToolSectionName() override;
	virtual void GetExtensionTools(const FExtensionToolQueryInfo& QueryInfo, TArray<FExtensionToolDescription>& ToolsOut) override;
	virtual bool GetExtensionExtendedInfo(FModelingModeExtensionExtendedInfo& InfoOut) override;

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

	bool EnsureAuthToken() const;
	bool StartServerInternal();
	void StopServerInternal();

	TUniquePtr<FUeAgentHttpServer> Server;

	IConsoleObject* ConsoleCmdStart = nullptr;
	IConsoleObject* ConsoleCmdStop = nullptr;
	IConsoleObject* ConsoleCmdRestart = nullptr;
};
