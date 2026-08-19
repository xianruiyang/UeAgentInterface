// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentInterfaceModule.h"

#include "CommandRegistry/UeAgentActiveCommandManifest.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "NetworkTransport/UeAgentHttpTransportAdapter.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "Module/UeAgentInterfaceLogger.h"
#include "ServiceComposition/CommandService/UeAgentCommandService.h"
#include "ServiceComposition/Query/UeAgentQueryCommandIngress.h"
#include "ServiceComposition/Transport/UeAgentTransportService.h"
#include "Utilities/UeAgentCustomModelingPrimitives.h"
#include "Utilities/UeAgentModelingToolCommands.h"
#include "UeAgentInterfaceSettings.h"

#include "Features/IModularFeatures.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/IConsoleManager.h"
#include "Interfaces/IMainFrameModule.h"
#include "ModelingModeToolExtensions.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Guid.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "ToolMenus.h"
#include "Widgets/SWindow.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Editor.h"

namespace
{
bool IsStartMinimizedMainFrameRequested()
{
	const TCHAR* CommandLine = FCommandLine::Get();
	return FParse::Param(CommandLine, TEXT("UeAgentInterfaceStartMinimized"))
		|| FParse::Param(CommandLine, TEXT("UAIStartMinimized"));
}

bool ShouldDisablePersistentLayoutsForHeadlessRun()
{
	const TCHAR* CommandLine = FCommandLine::Get();
	return IsRunningCommandlet()
		|| FApp::IsUnattended()
		|| FParse::Param(CommandLine, TEXT("NullRHI"));
}

void DisablePersistentLayoutsForHeadlessRun()
{
	if (!ShouldDisablePersistentLayoutsForHeadlessRun())
	{
		return;
	}

	FGlobalTabmanager::Get()->SetCanSavePersistentLayouts(false);
	FUeAgentInterfaceLogger::Log(TEXT("StartupModule: persistent layout saving disabled for headless run"));
}

class FModuleRequestContextInvalidator final
	: public UE::UAI::ServiceComposition::CommandService::
		IUeAgentRequestContextInvalidator
{
public:
	virtual void InvalidateWorld(
		const FString& WorldIdentity) override
	{
		LastWorldIdentity = WorldIdentity;
		++Generation;
		FUeAgentInterfaceLogger::Log(
			TEXT("Request context invalidated generation=%llu world=%s"),
			Generation,
			*WorldIdentity);
	}

private:
	FString LastWorldIdentity;
	uint64 Generation = 0;
};

class FUeAgentModelingModeToolExtension final : public IModelingModeToolExtension
{
public:
	virtual FText GetExtensionName() override
	{
		return FText::FromString(TEXT("UeAgentInterface"));
	}

	virtual FText GetToolSectionName() override
	{
		return FText::FromString(TEXT("UAI Primitive"));
	}

	virtual void GetExtensionTools(const FExtensionToolQueryInfo& QueryInfo, TArray<FExtensionToolDescription>& ToolsOut) override
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

	virtual bool GetExtensionExtendedInfo(FModelingModeExtensionExtendedInfo& InfoOut) override
	{
		InfoOut.ExtensionCommand = FUeAgentModelingToolCommands::Get().UeAgentPrimitivesTabButton;
		InfoOut.ToolPaletteButtonTooltip = FText::FromString(TEXT("UeAgentInterface custom primitive tools"));
		return true;
	}
};

TUniquePtr<FUeAgentModelingModeToolExtension> GModelingToolExtension;
}

FUeAgentInterfaceModule::FUeAgentInterfaceModule()
	: ConsoleCmdStart(nullptr)
	, ConsoleCmdStop(nullptr)
	, ConsoleCmdRestart(nullptr)
	, bAppliedRootWindowMaximizeOverride(false)
	, bHadPreviousRootWindowInitiallyMaximizedValue(false)
{
}

void FUeAgentInterfaceModule::StartupModule()
{
	FUeAgentInterfaceLogger::Log(TEXT("StartupModule begin command_line=%s"), FCommandLine::Get());

	DisablePersistentLayoutsForHeadlessRun();
	InstallStartMinimizedMainFrameHookIfRequested();

	EditorMapOpenedHandle = FEditorDelegates::OnMapOpened.AddRaw(
		this,
		&FUeAgentInterfaceModule::HandleEditorMapOpened);

	FUeAgentModelingToolCommands::Register();
	FUeAgentInterfaceLogger::Log(TEXT("StartupModule: modeling tool commands registered"));

	GModelingToolExtension = MakeUnique<FUeAgentModelingModeToolExtension>();
	IModularFeatures::Get().RegisterModularFeature(IModelingModeToolExtension::GetModularFeatureName(), GModelingToolExtension.Get());
	FUeAgentInterfaceLogger::Log(TEXT("StartupModule: modeling modular feature registered"));

	RegisterConsoleCommands();
	FUeAgentInterfaceLogger::Log(TEXT("StartupModule: console commands registered"));

	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FUeAgentInterfaceModule::RegisterMenus));
	FUeAgentInterfaceLogger::Log(TEXT("StartupModule: menu startup callback registered"));

	if (FParse::Param(FCommandLine::Get(), TEXT("UeAgentInterfaceAutoStart")))
	{
		FUeAgentInterfaceLogger::Log(TEXT("StartupModule: AutoStart requested"));
		const bool bOk = StartServerInternal();
		FUeAgentInterfaceLogger::Log(TEXT("AutoStart from command line: ok=%s"), bOk ? TEXT("true") : TEXT("false"));
	}
	else
	{
		FUeAgentInterfaceLogger::Log(TEXT("StartupModule: AutoStart not requested"));
	}

	FUeAgentInterfaceLogger::Log(TEXT("StartupModule end"));
}

void FUeAgentInterfaceModule::ShutdownModule()
{
	FUeAgentInterfaceLogger::Log(TEXT("ShutdownModule begin"));
	UninstallStartMinimizedMainFrameHook();
	if (EditorMapOpenedHandle.IsValid())
	{
		FEditorDelegates::OnMapOpened.Remove(EditorMapOpenedHandle);
		EditorMapOpenedHandle.Reset();
	}
	if (GModelingToolExtension)
	{
		IModularFeatures::Get().UnregisterModularFeature(IModelingModeToolExtension::GetModularFeatureName(), GModelingToolExtension.Get());
		GModelingToolExtension.Reset();
	}
	FUeAgentModelingToolCommands::Unregister();
	UnregisterMenus();
	UnregisterConsoleCommands();
	StopServerInternal();
	TransportService.Reset();
	CommandService.Reset();
	FUeAgentInterfaceLogger::Log(TEXT("ShutdownModule end"));
}

void FUeAgentInterfaceModule::InstallStartMinimizedMainFrameHookIfRequested()
{
	if (!IsStartMinimizedMainFrameRequested())
	{
		FUeAgentInterfaceLogger::Log(TEXT("StartupModule: StartMinimizedMainFrame not requested"));
		return;
	}

	if (IsRunningCommandlet())
	{
		FUeAgentInterfaceLogger::Log(TEXT("StartupModule: StartMinimizedMainFrame skipped for commandlet"));
		return;
	}

	IMainFrameModule& MainFrameModule = FModuleManager::LoadModuleChecked<IMainFrameModule>(TEXT("MainFrame"));
	if (MainFrameModule.IsWindowInitialized())
	{
		TSharedPtr<SWindow> ParentWindow = MainFrameModule.GetParentWindow();
		if (ParentWindow.IsValid())
		{
			ParentWindow->Minimize();
		}
		FUeAgentInterfaceLogger::Log(
			TEXT("StartupModule: StartMinimizedMainFrame requested after main frame initialization; minimized_existing_window=%s"),
			ParentWindow.IsValid() ? TEXT("true") : TEXT("false"));
		return;
	}

	ApplyTemporaryRootWindowMaximizeOverride();

	MainFrameModule.EnableDelayedShowMainFrame();
	MainFrameCreationFinishedForStartMinimizedHandle = MainFrameModule.OnMainFrameCreationFinished().AddRaw(
		this,
		&FUeAgentInterfaceModule::HandleMainFrameCreationFinishedForStartMinimized);

	FUeAgentInterfaceLogger::Log(TEXT("StartupModule: StartMinimizedMainFrame hook installed; delayed main frame show enabled"));
}

void FUeAgentInterfaceModule::UninstallStartMinimizedMainFrameHook()
{
	if (MainFrameCreationFinishedForStartMinimizedHandle.IsValid())
	{
		if (IMainFrameModule* MainFrameModule = FModuleManager::Get().GetModulePtr<IMainFrameModule>(TEXT("MainFrame")))
		{
			MainFrameModule->OnMainFrameCreationFinished().Remove(MainFrameCreationFinishedForStartMinimizedHandle);
		}
		MainFrameCreationFinishedForStartMinimizedHandle.Reset();
	}

	RestoreTemporaryRootWindowMaximizeOverride();
}

void FUeAgentInterfaceModule::HandleMainFrameCreationFinishedForStartMinimized(
	TSharedPtr<SWindow> MainFrameWindow,
	bool bShowStartupDialogInPlaceOfMainEditor)
{
	IMainFrameModule& MainFrameModule = FModuleManager::LoadModuleChecked<IMainFrameModule>(TEXT("MainFrame"));

	if (MainFrameCreationFinishedForStartMinimizedHandle.IsValid())
	{
		MainFrameModule.OnMainFrameCreationFinished().Remove(MainFrameCreationFinishedForStartMinimizedHandle);
		MainFrameCreationFinishedForStartMinimizedHandle.Reset();
	}

	RestoreTemporaryRootWindowMaximizeOverride();

	const bool bWindowValid = MainFrameWindow.IsValid();
	if (bWindowValid)
	{
		MainFrameWindow->Minimize();
	}

	FUeAgentInterfaceLogger::Log(
		TEXT("StartMinimizedMainFrame: main frame generated; window_valid=%s startup_dialog=%s initial_minimize_requested=%s"),
		bWindowValid ? TEXT("true") : TEXT("false"),
		bShowStartupDialogInPlaceOfMainEditor ? TEXT("true") : TEXT("false"),
		bWindowValid ? TEXT("true") : TEXT("false"));

	MainFrameModule.ShowDelayedMainFrame();
}

void FUeAgentInterfaceModule::ApplyTemporaryRootWindowMaximizeOverride()
{
	if (bAppliedRootWindowMaximizeOverride || !GConfig)
	{
		return;
	}

	PreviousRootWindowInitiallyMaximizedValue.Reset();
	bHadPreviousRootWindowInitiallyMaximizedValue = GConfig->GetString(
		TEXT("RootWindow"),
		TEXT("InitiallyMaximized"),
		PreviousRootWindowInitiallyMaximizedValue,
		GEditorPerProjectIni);

	GConfig->SetBool(TEXT("RootWindow"), TEXT("InitiallyMaximized"), false, GEditorPerProjectIni);
	bAppliedRootWindowMaximizeOverride = true;
	FUeAgentInterfaceLogger::Log(
		TEXT("StartMinimizedMainFrame: temporarily forced RootWindow.InitiallyMaximized=false previous_exists=%s previous_value=%s"),
		bHadPreviousRootWindowInitiallyMaximizedValue ? TEXT("true") : TEXT("false"),
		*PreviousRootWindowInitiallyMaximizedValue);
}

void FUeAgentInterfaceModule::RestoreTemporaryRootWindowMaximizeOverride()
{
	if (!bAppliedRootWindowMaximizeOverride || !GConfig)
	{
		return;
	}

	if (bHadPreviousRootWindowInitiallyMaximizedValue)
	{
		GConfig->SetString(
			TEXT("RootWindow"),
			TEXT("InitiallyMaximized"),
			*PreviousRootWindowInitiallyMaximizedValue,
			GEditorPerProjectIni);
	}
	else
	{
		GConfig->RemoveKey(TEXT("RootWindow"), TEXT("InitiallyMaximized"), GEditorPerProjectIni);
	}

	FUeAgentInterfaceLogger::Log(
		TEXT("StartMinimizedMainFrame: restored RootWindow.InitiallyMaximized override previous_exists=%s"),
		bHadPreviousRootWindowInitiallyMaximizedValue ? TEXT("true") : TEXT("false"));

	PreviousRootWindowInitiallyMaximizedValue.Reset();
	bHadPreviousRootWindowInitiallyMaximizedValue = false;
	bAppliedRootWindowMaximizeOverride = false;
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
	using namespace UE::UAI::ServiceComposition::CommandService;
	using namespace UE::UAI::ServiceComposition::Transport;
	using namespace UE::UAI::NetworkTransport;

	if (TransportService && TransportService->IsRunning())
	{
		return true;
	}
	StopServerInternal();
	if (!EnsureAuthToken())
	{
		FUeAgentInterfaceLogger::Log(
			TEXT("StartServerInternal: authentication setup failed"));
		return false;
	}

	const UUeAgentInterfaceSettings* Settings =
		UUeAgentInterfaceSettings::Get();
	if (!Settings
		|| Settings->Port <= 0
		|| Settings->Port > 65535
		|| Settings->AuthToken.IsEmpty())
	{
		FUeAgentInterfaceLogger::Log(
			TEXT("StartServerInternal: settings are invalid"));
		return false;
	}
	if (!Settings->ListenAddress.Equals(TEXT("127.0.0.1"))
		&& !Settings->ListenAddress.Equals(
			TEXT("localhost"),
			ESearchCase::IgnoreCase))
	{
		FUeAgentInterfaceLogger::Log(
			TEXT("StartServerInternal: only a loopback listener is supported"));
		return false;
	}

	const TSharedRef<
		FModuleRequestContextInvalidator,
		ESPMode::ThreadSafe> ContextInvalidator =
		MakeShared<
				FModuleRequestContextInvalidator,
				ESPMode::ThreadSafe>();
	UE::UAI::DomainCapability::FDomainCapabilityAuthorities
		DomainCapabilityPreflight;
	FString DomainCapabilityError;
	if (!UE::UAI::DomainCapability::
		TryGetOrBuildProcessDomainCapabilityAuthorities(
			DomainCapabilityPreflight,
			DomainCapabilityError))
	{
		FUeAgentInterfaceLogger::Log(
			TEXT("StartServerInternal: domain capability preflight failed error=%s"),
			*DomainCapabilityError);
		return false;
	}
	TSharedPtr<
		const UE::UAI::CommandRegistry::
			FUeAgentImmutableCommandRegistry> ActiveRegistry =
				DomainCapabilityPreflight.GetCommandRegistry();
	TSharedPtr<
		const UE::UAI::CommandEdge::FUeAgentExactCommandDispatch>
		ActiveDispatch = DomainCapabilityPreflight.GetCommandDispatch();
	FString ActiveCompositionError;
	if (!ActiveRegistry.IsValid() || !ActiveDispatch.IsValid())
	{
		FUeAgentInterfaceLogger::Log(
			TEXT("StartServerInternal: domain capability activation did not publish the process command authority"));
		return false;
	}
	if (!UE::UAI::CommandRegistry::FUeAgentActiveCommandManifest::
		TryVerify(
			ActiveRegistry.ToSharedRef(),
			UE::UAI::CommandContract::GetUaiCommandManifestPluginVersion(),
			UE::UAI::CommandContract::GetUaiCommandManifestBuildId(),
			UE::UAI::CommandContract::GetDefaultCommandManifestPath(),
			ActiveCompositionError))
	{
		FUeAgentInterfaceLogger::Log(
			TEXT("StartServerInternal: active command manifest verification failed error=%s"),
			*ActiveCompositionError);
		return false;
	}
	TSharedPtr<
		IUeAgentTransportRequestHandler,
		ESPMode::ThreadSafe> QueryIngress;
	if (!UE::UAI::ServiceComposition::Query::
		TryCreateQueryCommandIngress(
			ActiveDispatch.ToSharedRef(),
			TEXT("uai-local-authenticated-client"),
			{
				TEXT("uai:query"),
				TEXT("uai:export"),
				TEXT("uai:operate"),
			},
			QueryIngress,
			ActiveCompositionError))
	{
		FUeAgentInterfaceLogger::Log(
			TEXT("StartServerInternal: query ingress failed error=%s"),
			*ActiveCompositionError);
		return false;
	}
	for (const UE::UAI::CommandContract::FUeAgentFrozenCommandContract&
		Contract : ActiveRegistry->GetContracts())
	{
		FUeAgentInterfaceLogger::Log(
			TEXT("Activated exact command id=%s contract=%s owner=%s"),
			*Contract.GetCommandId(),
			*Contract.GetContractHash(),
			*Contract.GetPublicOwner());
	}
	TUniquePtr<FUeAgentCommandService> NewCommandService;
	FString CommandServiceError;
	if (!FUeAgentCommandService::TryCreate(
			QueryIngress.ToSharedRef(),
			ContextInvalidator,
			NewCommandService,
			CommandServiceError))
	{
		FUeAgentInterfaceLogger::Log(
			TEXT("StartServerInternal: command service failed error=%s"),
			*CommandServiceError);
		return false;
	}

	const FString OwnerId = TEXT("uai-module-transport");
	FUeAgentHttpAdapterConfig AdapterConfig;
	AdapterConfig.OwnerId = OwnerId;
	AdapterConfig.InstanceId =
		TEXT("uai-module-http:")
		+ FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	AdapterConfig.Method = TEXT("POST");
	AdapterConfig.RoutePath = GetUaiHttpCommandRoutePath();
	AdapterConfig.AuthHeader = TEXT("X-UeAgentInterface-Token");
	AdapterConfig.AuthToken = Settings->AuthToken;
	AdapterConfig.Channel = TEXT("uai.command.v1");
	AdapterConfig.PrincipalSubject = TEXT("uai-local-authenticated-client");
	AdapterConfig.MaxFrameBytes = 4 * 1024 * 1024;

	const TSharedRef<
		FUeAgentEngineHttpRouteHost,
		ESPMode::ThreadSafe> RouteHost =
		MakeShared<
			FUeAgentEngineHttpRouteHost,
			ESPMode::ThreadSafe>(static_cast<uint32>(Settings->Port));
	TUniquePtr<IUeAgentNetworkServerControl> ServerControl =
		MakeUnique<FUeAgentHttpTransportAdapter>(
			AdapterConfig,
			RouteHost,
			NewCommandService->GetIngressHandler());

	FUeAgentTransportServiceConfig TransportConfig;
	TransportConfig.OwnerId = OwnerId;
	TransportConfig.Capabilities.bOrdered = true;
	TransportConfig.Capabilities.bReliable = true;
	TransportConfig.Capabilities.bPreservesMessageBoundaries = true;
	TransportConfig.Capabilities.MaxFrameBytes =
		AdapterConfig.MaxFrameBytes;
	TransportConfig.Capabilities.bRequestReply = true;
	TransportConfig.Requirements.bRequireOrdered = true;
	TransportConfig.Requirements.bRequireReliable = true;
	TransportConfig.Requirements.bRequireMessageBoundaries = true;
	TransportConfig.Requirements.MaxFrameBytes =
		AdapterConfig.MaxFrameBytes;
	TransportConfig.Requirements.bRequireRequestReply = true;

	TUniquePtr<FUeAgentTransportService> NewTransportService;
	FUeAgentTransportDiagnostic TransportDiagnostic;
	if (!FUeAgentTransportService::TryCreate(
			TransportConfig,
			MoveTemp(ServerControl),
			NewTransportService,
			TransportDiagnostic)
		|| !NewTransportService->Start(TransportDiagnostic))
	{
		FUeAgentInterfaceLogger::Log(
			TEXT("StartServerInternal: transport failed code=%s detail=%s"),
			*TransportDiagnostic.Code,
			*TransportDiagnostic.Message);
		return false;
	}

	CommandService = MoveTemp(NewCommandService);
	TransportService = MoveTemp(NewTransportService);

	FUeAgentInterfaceLogger::Log(
		TEXT("StartServerInternal: ok=true command_route=%s instance=%s registry=%s"),
		*GetUaiHttpCommandRoutePath(),
		*TransportService->GetIdentity().InstanceId,
		*ActiveRegistry->GetRegistryHash());
	return true;
}

void FUeAgentInterfaceModule::HandleEditorMapOpened(
	const FString& Filename,
	bool bAsTemplate)
{
	if (!TransportService
		|| !TransportService->IsRunning()
		|| !CommandService)
	{
		return;
	}

	CommandService->NotifyWorldChanged(
		Filename.IsEmpty() ? TEXT("editor-map-event") : Filename);
	FUeAgentInterfaceLogger::Log(
		TEXT("Editor map opened: request context invalidated generation=%llu filename=%s as_template=%s"),
		CommandService->GetContextGeneration(),
		*Filename,
		bAsTemplate ? TEXT("true") : TEXT("false"));
}

void FUeAgentInterfaceModule::StopServerInternal()
{
	using namespace UE::UAI::NetworkTransport;
	if (TransportService)
	{
		FUeAgentTransportDiagnostic Diagnostic;
		if (!TransportService->Stop(Diagnostic))
		{
			FUeAgentInterfaceLogger::Log(
				TEXT("StopServerInternal: transport stop failed code=%s detail=%s"),
				*Diagnostic.Code,
				*Diagnostic.Message);
			return;
		}
		TransportService.Reset();
	}
	CommandService.Reset();
	FUeAgentInterfaceLogger::Log(
		TEXT("StopServerInternal: owned transport stopped"));
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
	const UUeAgentInterfaceSettings* Settings =
		UUeAgentInterfaceSettings::Get();
	if (!Settings
		|| !TransportService
		|| !TransportService->IsRunning())
	{
		return;
	}

	const FString Url = FString::Printf(
		TEXT("http://%s:%d"),
		*Settings->ListenAddress,
		Settings->Port);
	const FString Token = Settings->AuthToken;
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
