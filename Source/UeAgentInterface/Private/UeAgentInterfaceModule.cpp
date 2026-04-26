// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentInterfaceModule.h"

#include "UeAgentCustomModelingPrimitives.h"
#include "UeAgentHttpServer.h"
#include "UeAgentInterfaceLogger.h"
#include "UeAgentModelingToolCommands.h"
#include "UeAgentInterfaceSettings.h"

#include "Features/IModularFeatures.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Guid.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "ToolMenus.h"
#include "Widgets/Notifications/SNotificationList.h"

void FUeAgentInterfaceModule::StartupModule()
{
	Server = MakeUnique<FUeAgentHttpServer>();
	FUeAgentModelingToolCommands::Register();
	IModularFeatures::Get().RegisterModularFeature(IModelingModeToolExtension::GetModularFeatureName(), this);

	RegisterConsoleCommands();

	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FUeAgentInterfaceModule::RegisterMenus));

	if (FParse::Param(FCommandLine::Get(), TEXT("UeAgentInterfaceAutoStart")))
	{
		const bool bOk = StartServerInternal();
		FUeAgentInterfaceLogger::Log(TEXT("AutoStart from command line: ok=%s"), bOk ? TEXT("true") : TEXT("false"));
	}
}

void FUeAgentInterfaceModule::ShutdownModule()
{
	IModularFeatures::Get().UnregisterModularFeature(IModelingModeToolExtension::GetModularFeatureName(), this);
	FUeAgentModelingToolCommands::Unregister();
	UnregisterMenus();
	UnregisterConsoleCommands();
	Server.Reset();
}

FText FUeAgentInterfaceModule::GetExtensionName()
{
	return FText::FromString(TEXT("UeAgentInterface"));
}

FText FUeAgentInterfaceModule::GetToolSectionName()
{
	return FText::FromString(TEXT("UAI Primitive"));
}

void FUeAgentInterfaceModule::GetExtensionTools(const FExtensionToolQueryInfo& QueryInfo, TArray<FExtensionToolDescription>& ToolsOut)
{
	FExtensionToolDescription RampToolInfo;
	RampToolInfo.ToolName = FText::FromString(TEXT("BeginAddRampPrimitiveTool"));
	RampToolInfo.ToolCommand = FUeAgentModelingToolCommands::Get().BeginAddRampPrimitiveTool;
	RampToolInfo.ToolBuilder = QueryInfo.bIsInfoQueryOnly ? nullptr : UUeAgentAddPrimitiveToolBuilder::CreateRampToolBuilder();
	ToolsOut.Add(RampToolInfo);

	FExtensionToolDescription RampCornerToolInfo;
	RampCornerToolInfo.ToolName = FText::FromString(TEXT("BeginAddRampCornerPrimitiveTool"));
	RampCornerToolInfo.ToolCommand = FUeAgentModelingToolCommands::Get().BeginAddRampCornerPrimitiveTool;
	RampCornerToolInfo.ToolBuilder = QueryInfo.bIsInfoQueryOnly ? nullptr : UUeAgentAddPrimitiveToolBuilder::CreateRampCornerToolBuilder();
	ToolsOut.Add(RampCornerToolInfo);
}

bool FUeAgentInterfaceModule::GetExtensionExtendedInfo(FModelingModeExtensionExtendedInfo& InfoOut)
{
	InfoOut.ExtensionCommand = FUeAgentModelingToolCommands::Get().UeAgentPrimitivesTabButton;
	InfoOut.ToolPaletteButtonTooltip = FText::FromString(TEXT("UeAgentInterface custom primitive tools"));
	return true;
}

void FUeAgentInterfaceModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Window");
	FToolMenuSection& Section = Menu->AddSection("UeAgentInterface", FText::FromString("UeAgentInterface"));

	Section.AddMenuEntry(
		"UeAgentInterface_Start",
		FText::FromString("Start UeAgentInterface Server"),
		FText::FromString("Start local HTTP server for automated editing."),
		FSlateIcon(),
		FToolMenuExecuteAction::CreateRaw(this, &FUeAgentInterfaceModule::StartServerFromUI));

	Section.AddMenuEntry(
		"UeAgentInterface_Stop",
		FText::FromString("Stop UeAgentInterface Server"),
		FText::FromString("Stop (unbind routes) for local HTTP server."),
		FSlateIcon(),
		FToolMenuExecuteAction::CreateRaw(this, &FUeAgentInterfaceModule::StopServerFromUI));

	Section.AddMenuEntry(
		"UeAgentInterface_Copy",
		FText::FromString("Copy Connection Info"),
		FText::FromString("Copy base URL and token to clipboard."),
		FSlateIcon(),
		FToolMenuExecuteAction::CreateRaw(this, &FUeAgentInterfaceModule::CopyConnectionInfoToClipboard));
}

void FUeAgentInterfaceModule::UnregisterMenus()
{
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
}

void FUeAgentInterfaceModule::RegisterConsoleCommands()
{
	if (ConsoleCmdStart || ConsoleCmdStop || ConsoleCmdRestart)
	{
		return;
	}

	ConsoleCmdStart = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("UeAgentInterface.StartServer"),
		TEXT("Start UeAgentInterface local HTTP server."),
		FConsoleCommandDelegate::CreateRaw(this, &FUeAgentInterfaceModule::ConsoleStartServer),
		ECVF_Default);

	ConsoleCmdStop = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("UeAgentInterface.StopServer"),
		TEXT("Stop UeAgentInterface local HTTP server (unbind routes)."),
		FConsoleCommandDelegate::CreateRaw(this, &FUeAgentInterfaceModule::ConsoleStopServer),
		ECVF_Default);

	ConsoleCmdRestart = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("UeAgentInterface.RestartServer"),
		TEXT("Restart UeAgentInterface local HTTP server."),
		FConsoleCommandDelegate::CreateRaw(this, &FUeAgentInterfaceModule::ConsoleRestartServer),
		ECVF_Default);
}

void FUeAgentInterfaceModule::UnregisterConsoleCommands()
{
	if (ConsoleCmdStart)
	{
		IConsoleManager::Get().UnregisterConsoleObject(ConsoleCmdStart, /*bKeepState*/ false);
		ConsoleCmdStart = nullptr;
	}
	if (ConsoleCmdStop)
	{
		IConsoleManager::Get().UnregisterConsoleObject(ConsoleCmdStop, /*bKeepState*/ false);
		ConsoleCmdStop = nullptr;
	}
	if (ConsoleCmdRestart)
	{
		IConsoleManager::Get().UnregisterConsoleObject(ConsoleCmdRestart, /*bKeepState*/ false);
		ConsoleCmdRestart = nullptr;
	}
}

void FUeAgentInterfaceModule::ConsoleStartServer()
{
	const bool bOk = StartServerInternal();
	FUeAgentInterfaceLogger::Log(TEXT("Console StartServer: ok=%s"), bOk ? TEXT("true") : TEXT("false"));
}

void FUeAgentInterfaceModule::ConsoleStopServer()
{
	StopServerInternal();
	FUeAgentInterfaceLogger::Log(TEXT("Console StopServer"));
}

void FUeAgentInterfaceModule::ConsoleRestartServer()
{
	StopServerInternal();
	const bool bOk = StartServerInternal();
	FUeAgentInterfaceLogger::Log(TEXT("Console RestartServer: ok=%s"), bOk ? TEXT("true") : TEXT("false"));
}

bool FUeAgentInterfaceModule::EnsureAuthToken() const
{
	UUeAgentInterfaceSettings* Settings = UUeAgentInterfaceSettings::GetMutable();
	if (!Settings)
	{
		return false;
	}

	if (Settings->AuthToken.IsEmpty())
	{
		Settings->AuthToken = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		Settings->SaveConfig();
	}

	return true;
}

bool FUeAgentInterfaceModule::StartServerInternal()
{
	EnsureAuthToken();
	return Server ? Server->Start() : false;
}

void FUeAgentInterfaceModule::StopServerInternal()
{
	if (Server)
	{
		Server->Stop();
	}
}

void FUeAgentInterfaceModule::StartServerFromUI(const FToolMenuContext& Context)
{
	const bool bOk = StartServerInternal();

	FNotificationInfo Info(bOk ? FText::FromString("UeAgentInterface: Server started") : FText::FromString("UeAgentInterface: Server failed to start"));
	Info.ExpireDuration = 3.0f;
	FSlateNotificationManager::Get().AddNotification(Info);
}

void FUeAgentInterfaceModule::StopServerFromUI(const FToolMenuContext& Context)
{
	StopServerInternal();

	FNotificationInfo Info(FText::FromString("UeAgentInterface: Server stopped"));
	Info.ExpireDuration = 3.0f;
	FSlateNotificationManager::Get().AddNotification(Info);
}

void FUeAgentInterfaceModule::CopyConnectionInfoToClipboard(const FToolMenuContext& Context)
{
	if (!Server)
	{
		return;
	}

	const FString Url = FString::Printf(TEXT("http://%s:%d"), *Server->GetListenAddress(), Server->GetPort());
	const FString Token = Server->GetAuthToken();
	const FString Text = FString::Printf(TEXT("BASE_URL=%s\nTOKEN=%s\nHEADER=X-UeAgentInterface-Token: %s\n"),
		*Url,
		*Token,
		*Token);

	FPlatformApplicationMisc::ClipboardCopy(*Text);

	FNotificationInfo Info(FText::FromString("UeAgentInterface: Connection info copied to clipboard"));
	Info.ExpireDuration = 3.0f;
	FSlateNotificationManager::Get().AddNotification(Info);
}

IMPLEMENT_MODULE(FUeAgentInterfaceModule, UeAgentInterface)
