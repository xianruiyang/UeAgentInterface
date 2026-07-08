// Copyright Epic Games, Inc. All Rights Reserved.

#include "Server/UeAgentHttpServer.h"
#include "Server/UeAgentCommandRegistry.h"

#include "Utilities/UeAgentStructuredJsonFormatter.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Module/UeAgentInterfaceLogger.h"
#include "UeAgentInterfaceSettings.h"

#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HttpServerModule.h"
#include "IHttpRouter.h"
#include "IPAddress.h"
#include "Misc/Guid.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

namespace UeAgentHttpServer
{
	static const FString ContentTypeJson = TEXT("application/json; charset=utf-8");
	static const FString HeaderTokenLower = TEXT("x-ueagentinterface-token");

	static const TCHAR* VerbToString(EHttpServerRequestVerbs Verb)
	{
		switch (Verb)
		{
		case EHttpServerRequestVerbs::VERB_GET: return TEXT("GET");
		case EHttpServerRequestVerbs::VERB_POST: return TEXT("POST");
		case EHttpServerRequestVerbs::VERB_PUT: return TEXT("PUT");
		case EHttpServerRequestVerbs::VERB_PATCH: return TEXT("PATCH");
		case EHttpServerRequestVerbs::VERB_DELETE: return TEXT("DELETE");
		case EHttpServerRequestVerbs::VERB_OPTIONS: return TEXT("OPTIONS");
		default: return TEXT("NONE");
		}
	}

	static EHttpServerResponseCodes ToResponseCode(int32 Code)
	{
		switch (Code)
		{
		case 200: return EHttpServerResponseCodes::Ok;
		case 204: return EHttpServerResponseCodes::NoContent;
		case 400: return EHttpServerResponseCodes::BadRequest;
		case 401: return EHttpServerResponseCodes::Denied;
		case 404: return EHttpServerResponseCodes::NotFound;
		case 408: return EHttpServerResponseCodes::RequestTimeout;
		default:  return EHttpServerResponseCodes::ServerError;
		}
	}

	static bool IsLoopbackPeer(const TSharedPtr<FInternetAddr>& Addr)
	{
		if (!Addr.IsValid() || !Addr->IsValid())
		{
			return false;
		}

		const TArray<uint8> Raw = Addr->GetRawIp();
		if (Raw.Num() == 4)
		{
			return Raw[0] == 127;
		}
		if (Raw.Num() == 16)
		{
			for (int32 i = 0; i < 15; i++)
			{
				if (Raw[i] != 0)
				{
					return false;
				}
			}
			return Raw[15] == 1;
		}
		return false;
	}

	static FString MaskToken(const FString& Token)
	{
		if (Token.Len() <= 8)
		{
			return TEXT("********");
		}
		return Token.Left(4) + TEXT("...") + Token.Right(4);
	}

	static bool CanBindTcpPort(const FString& ListenAddress, int32 Port, FString& OutError)
	{
		if (Port <= 0 || Port > 65535)
		{
			OutError = FString::Printf(TEXT("invalid_port:%d"), Port);
			return false;
		}

		ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		if (!SocketSubsystem)
		{
			OutError = TEXT("socket_subsystem_unavailable");
			return false;
		}

		FString AddressText = ListenAddress;
		if (AddressText.IsEmpty() || AddressText.Equals(TEXT("localhost"), ESearchCase::IgnoreCase))
		{
			AddressText = TEXT("127.0.0.1");
		}

		bool bIsValidAddress = false;
		TSharedRef<FInternetAddr> Address = SocketSubsystem->CreateInternetAddr();
		Address->SetIp(*AddressText, bIsValidAddress);
		if (!bIsValidAddress)
		{
			OutError = FString::Printf(TEXT("invalid_listen_address:%s"), *ListenAddress);
			return false;
		}
		Address->SetPort(Port);

		FSocket* ProbeSocket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("UeAgentInterfacePortProbe"), false);
		if (!ProbeSocket)
		{
			OutError = TEXT("create_probe_socket_failed");
			return false;
		}

		ProbeSocket->SetReuseAddr(false);
		const bool bCanBind = ProbeSocket->Bind(*Address);
		SocketSubsystem->DestroySocket(ProbeSocket);

		if (!bCanBind)
		{
			OutError = FString::Printf(TEXT("port_unavailable listen=%s port=%d"), *AddressText, Port);
			return false;
		}

		return true;
	}

	static FString GetHeaderFirst(const TMap<FString, TArray<FString>>& Headers, const FString& KeyLower)
	{
		for (const auto& Pair : Headers)
		{
			if (Pair.Key.ToLower() == KeyLower)
			{
				return Pair.Value.Num() > 0 ? Pair.Value[0] : FString();
			}
		}
		return FString();
	}

	static FString NowIso()
	{
		return FDateTime::Now().ToString(TEXT("%Y-%m-%dT%H:%M:%S.%s"));
	}

	static bool IsObjectEmpty(const TSharedPtr<FJsonObject>& Object)
	{
		return !Object.IsValid() || Object->Values.Num() == 0;
	}

	static bool IsHiddenDefaultDataField(const FString& FieldName)
	{
		static const TSet<FString> HiddenExactFields = {
			TEXT("timestamp"),
			TEXT("request_id"),
			TEXT("error"),
			TEXT("compatibility"),
			TEXT("command_manifest_validation"),
			TEXT("command_param_validation"),
			TEXT("metrics"),
			TEXT("diagnostics"),
			TEXT("coverage_report"),
			TEXT("validation_report"),
			TEXT("compile_log"),
			TEXT("stack_issue_report"),
			TEXT("raw"),
			TEXT("readback"),
			TEXT("complete_readback"),
			TEXT("diff"),
			TEXT("structural_diff"),
		};

		const FString Key = FieldName.ToLower();
		return HiddenExactFields.Contains(Key) || Key.StartsWith(TEXT("raw_")) || Key.StartsWith(TEXT("debug_"));
	}

	static bool ShouldExternalizeDefaultDataField(const FString& FieldName)
	{
		const FString Key = FieldName.ToLower();
		static const TSet<FString> ExternalizedExactFields = {
			TEXT("command_manifest_validation"),
			TEXT("command_param_validation"),
			TEXT("coverage_report"),
			TEXT("validation_report"),
			TEXT("compile_log"),
			TEXT("stack_issue_report"),
			TEXT("readback"),
			TEXT("complete_readback"),
			TEXT("diff"),
			TEXT("structural_diff"),
			TEXT("raw"),
		};
		return ExternalizedExactFields.Contains(Key) || Key.StartsWith(TEXT("raw_")) || Key.EndsWith(TEXT("_report")) || Key.EndsWith(TEXT("_log"));
	}

	static FString SanitizeArtifactSegment(const FString& Value)
	{
		FString Out;
		Out.Reserve(Value.Len());
		for (const TCHAR Ch : Value)
		{
			if (FChar::IsAlnum(Ch) || Ch == TCHAR('-') || Ch == TCHAR('_'))
			{
				Out.AppendChar(Ch);
			}
			else
			{
				Out.AppendChar(TCHAR('_'));
			}
		}
		Out.TrimStartAndEndInline();
		return Out.IsEmpty() ? TEXT("artifact") : Out.Left(64);
	}

	static FString MakeResponseArtifactPath(const FUeAgentRequestContext& Ctx, const FString& FieldName)
	{
		const UUeAgentInterfaceSettings* Settings = UUeAgentInterfaceSettings::Get();
		const FString RootRel = Settings ? Settings->ArtifactsDirRelative : TEXT("Saved/RemoteArtifacts");
		const FString ResponseDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), RootRel / TEXT("Responses"));
		const FString SafeCommand = SanitizeArtifactSegment(Ctx.Command.IsEmpty() ? TEXT("command") : Ctx.Command);
		const FString SafeRequestId = SanitizeArtifactSegment(Ctx.RequestId.IsEmpty() ? TEXT("request") : Ctx.RequestId);
		const FString SafeField = SanitizeArtifactSegment(FieldName);
		const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		return FPaths::Combine(ResponseDir, SafeCommand, FString::Printf(TEXT("%s_%s_%s.json"), *SafeRequestId, *SafeField, *Guid));
	}

	static FString JsonValueTypeName(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return TEXT("null");
		}
		switch (Value->Type)
		{
		case EJson::Object:
			return TEXT("object");
		case EJson::Array:
			return TEXT("array");
		case EJson::String:
			return TEXT("string");
		case EJson::Number:
			return TEXT("number");
		case EJson::Boolean:
			return TEXT("boolean");
		case EJson::Null:
		case EJson::None:
		default:
			return TEXT("null");
		}
	}

	static TSharedPtr<FJsonObject> MakeResponseArtifactDescriptor(
		const FString& FieldName,
		const TSharedPtr<FJsonValue>& Value,
		const FString& FilePath)
	{
		TSharedPtr<FJsonObject> Descriptor = MakeShared<FJsonObject>();
		Descriptor->SetStringField(TEXT("kind"), TEXT("response_data"));
		Descriptor->SetStringField(TEXT("field"), FieldName);
		Descriptor->SetStringField(TEXT("path"), FilePath);
		Descriptor->SetStringField(TEXT("value_type"), JsonValueTypeName(Value));
		if (Value.IsValid() && Value->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			Descriptor->SetNumberField(TEXT("field_count"), Object.IsValid() ? Object->Values.Num() : 0);
		}
		else if (Value.IsValid() && Value->Type == EJson::Array)
		{
			Descriptor->SetNumberField(TEXT("item_count"), Value->AsArray().Num());
		}
		return Descriptor;
	}

	static bool SaveResponseArtifact(
		const FUeAgentRequestContext& Ctx,
		const FString& FieldName,
		const TSharedPtr<FJsonValue>& Value,
		FString& OutFilePath)
	{
		OutFilePath = MakeResponseArtifactPath(Ctx, FieldName);

		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.response_artifact.v1"));
		Payload->SetStringField(TEXT("command"), Ctx.Command);
		Payload->SetStringField(TEXT("request_id"), Ctx.RequestId);
		Payload->SetStringField(TEXT("field"), FieldName);
		Payload->SetField(TEXT("value"), Value.IsValid() ? Value : MakeShared<FJsonValueNull>());

		const FString JsonText = FUeAgentStructuredJsonFormatter::WriteObject(Payload.ToSharedRef());
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutFilePath), true);
		return FFileHelper::SaveStringToFile(JsonText, *OutFilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	static void AppendExternalizedHiddenDataArtifacts(
		const FUeAgentRequestContext& Ctx,
		const TSharedPtr<FJsonObject>& HiddenData,
		TArray<TSharedPtr<FJsonValue>>& InOutArtifacts,
		TArray<TSharedPtr<FJsonValue>>& InOutWarnings)
	{
		if (!HiddenData.IsValid())
		{
			return;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : HiddenData->Values)
		{
			if (!ShouldExternalizeDefaultDataField(Pair.Key))
			{
				continue;
			}

			FString FilePath;
			if (SaveResponseArtifact(Ctx, Pair.Key, Pair.Value, FilePath))
			{
				InOutArtifacts.Add(MakeShared<FJsonValueObject>(MakeResponseArtifactDescriptor(Pair.Key, Pair.Value, FilePath)));
			}
			else
			{
				InOutWarnings.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("artifact_externalize_failed:%s"), *Pair.Key)));
			}
		}
	}

	static void SplitDefaultDataFields(
		const TSharedPtr<FJsonObject>& Data,
		TSharedPtr<FJsonObject>& OutVisible,
		TSharedPtr<FJsonObject>& OutHidden)
	{
		OutVisible = MakeShared<FJsonObject>();
		OutHidden = MakeShared<FJsonObject>();

		if (!Data.IsValid())
		{
			return;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Data->Values)
		{
			if (IsHiddenDefaultDataField(Pair.Key))
			{
				OutHidden->SetField(Pair.Key, Pair.Value);
			}
			else
			{
				OutVisible->SetField(Pair.Key, Pair.Value);
			}
		}
	}

	static TSharedPtr<FJsonObject> BuildDebugObject(
		const FUeAgentCommandOutput& Output,
		const TSharedPtr<FJsonObject>& HiddenData)
	{
		TSharedPtr<FJsonObject> Debug = MakeShared<FJsonObject>();

		if (Output.Debug.IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Output.Debug->Values)
			{
				Debug->SetField(Pair.Key, Pair.Value);
			}
		}

		if (!IsObjectEmpty(HiddenData))
		{
			Debug->SetObjectField(TEXT("filtered_data"), HiddenData);
		}

		return Debug;
	}

	static bool IsDebugResponseView(const FUeAgentResponseOptions& Options)
	{
		return Options.bDebug || Options.View.Equals(TEXT("debug"), ESearchCase::IgnoreCase);
	}

	static bool IsRawResponseView(const FUeAgentResponseOptions& Options)
	{
		return Options.View.Equals(TEXT("raw"), ESearchCase::IgnoreCase);
	}

	static bool IsSummaryResponseView(const FUeAgentResponseOptions& Options)
	{
		return Options.View.Equals(TEXT("summary"), ESearchCase::IgnoreCase)
			|| Options.View.Equals(TEXT("llm_summary"), ESearchCase::IgnoreCase)
			|| Options.View.Equals(TEXT("compact_summary"), ESearchCase::IgnoreCase);
	}
}

FUeAgentResponseOptions::FUeAgentResponseOptions()
	: View(TEXT("llm_compact"))
	, bDebug(false)
	, Format(TEXT("structured_compact"))
	, MaxItems(50)
{
}

void FUeAgentCommandOutput::Reset()
{
	Data.Reset();
	Debug.Reset();
	Artifacts.Reset();
	Warnings.Reset();
}

TSharedPtr<FJsonObject> FUeAgentCommandOutput::GetDataOrEmpty() const
{
	return Data.IsValid() ? Data : MakeShared<FJsonObject>();
}

FUeAgentCommandOutput FUeAgentCommandOutput::FromLegacy(
	const TSharedPtr<FJsonObject>& InData,
	const TArray<TSharedPtr<FJsonValue>>& InArtifacts)
{
	FUeAgentCommandOutput Output;
	Output.Data = InData.IsValid() ? InData : MakeShared<FJsonObject>();
	Output.Artifacts = InArtifacts;
	return Output;
}

FUeAgentHttpServer::FTransactionSpawnedActorRecord::FTransactionSpawnedActorRecord()
	: bLevelPackageWasDirtyBeforeSpawn(false)
	, bActorPackageWasDirtyBeforeSpawn(false)
{
}

FUeAgentHttpServer::FUeAgentHttpServer()
	: bRunning(false)
	, bPythonConsoleCommandExecuted(false)
	, ActiveTransactionIndex(INDEX_NONE)
{
}

FUeAgentHttpServer::~FUeAgentHttpServer()
{
	Stop();
}

TSharedPtr<FJsonObject> FUeAgentResponseBuilder::BuildResponseEnvelope(
	const FUeAgentRequestContext& Ctx,
	bool bOk,
	const FString& Error,
	const FUeAgentCommandOutput& Output)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), bOk);

	if (UeAgentHttpServer::IsRawResponseView(Ctx.ResponseOptions))
	{
		Root->SetStringField(TEXT("request_id"), Ctx.RequestId);
		Root->SetStringField(TEXT("error"), bOk ? TEXT("") : (Error.IsEmpty() ? TEXT("command_failed") : Error));
		Root->SetStringField(TEXT("timestamp"), UeAgentHttpServer::NowIso());
		Root->SetObjectField(TEXT("data"), Output.GetDataOrEmpty());
		Root->SetArrayField(TEXT("artifacts"), Output.Artifacts);
		Root->SetArrayField(TEXT("warnings"), Output.Warnings);
		if (!UeAgentHttpServer::IsObjectEmpty(Output.Debug))
		{
			Root->SetObjectField(TEXT("debug"), Output.Debug);
		}
		return Root;
	}

	TSharedPtr<FJsonObject> VisibleData = Output.GetDataOrEmpty();
	TSharedPtr<FJsonObject> HiddenData = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Artifacts = Output.Artifacts;
	TArray<TSharedPtr<FJsonValue>> Warnings = Output.Warnings;
	const bool bSummaryView = UeAgentHttpServer::IsSummaryResponseView(Ctx.ResponseOptions);
	if (bSummaryView)
	{
		UeAgentHttpServer::SplitDefaultDataFields(Output.GetDataOrEmpty(), VisibleData, HiddenData);
		UeAgentHttpServer::AppendExternalizedHiddenDataArtifacts(Ctx, HiddenData, Artifacts, Warnings);
	}

	Root->SetObjectField(TEXT("data"), VisibleData);
	if (!bOk)
	{
		Root->SetStringField(TEXT("error"), Error.IsEmpty() ? TEXT("command_failed") : Error);
	}

	if (Artifacts.Num() > 0)
	{
		Root->SetArrayField(TEXT("artifacts"), Artifacts);
	}
	if (Warnings.Num() > 0)
	{
		Root->SetArrayField(TEXT("warnings"), Warnings);
	}

	if (UeAgentHttpServer::IsDebugResponseView(Ctx.ResponseOptions))
	{
		Root->SetStringField(TEXT("request_id"), Ctx.RequestId);
		const TSharedPtr<FJsonObject> Debug = UeAgentHttpServer::BuildDebugObject(Output, bSummaryView ? HiddenData : nullptr);
		if (!UeAgentHttpServer::IsObjectEmpty(Debug))
		{
			Root->SetObjectField(TEXT("debug"), Debug);
		}
	}

	return Root;
}

bool FUeAgentHttpServer::Start()
{
	if (bRunning)
	{
		FUeAgentInterfaceLogger::Log(TEXT("Start: already running."));
		return true;
	}

	FUeAgentInterfaceLogger::Log(TEXT("Start: begin."));

	const UUeAgentInterfaceSettings* Settings = UUeAgentInterfaceSettings::Get();
	if (!Settings)
	{
		FUeAgentInterfaceLogger::Log(TEXT("Start: settings object unavailable."));
		return false;
	}

	FUeAgentInterfaceLogger::Log(TEXT("Start: settings listen=%s port=%d token=%s"),
		*Settings->ListenAddress,
		Settings->Port,
		*UeAgentHttpServer::MaskToken(Settings->AuthToken));

	FString PortPreflightError;
	if (!UeAgentHttpServer::CanBindTcpPort(Settings->ListenAddress, Settings->Port, PortPreflightError))
	{
		FUeAgentInterfaceLogger::Log(TEXT("Start: port preflight failed: %s"), *PortPreflightError);
		return false;
	}

	FUeAgentInterfaceLogger::Log(TEXT("Start: acquiring HTTP router port=%d."), Settings->Port);
	Router = FHttpServerModule::Get().GetHttpRouter((uint32)Settings->Port, /*bFailOnBindFailure*/ true);
	if (!Router.IsValid())
	{
		FUeAgentInterfaceLogger::Log(TEXT("Start: failed to bind http router (port=%d)."), Settings->Port);
		return false;
	}

	FUeAgentInterfaceLogger::Log(TEXT("Start: HTTP router acquired."));
	FUeAgentInterfaceLogger::Log(TEXT("Start: binding routes."));
	if (!BindRoutes())
	{
		FUeAgentInterfaceLogger::Log(TEXT("Start: route binding failed."));
		UnbindRoutes();
		Router.Reset();
		return false;
	}

	FUeAgentInterfaceLogger::Log(TEXT("Start: routes bound, starting HTTP listeners."));
	FHttpServerModule::Get().StartAllListeners();
	bRunning = true;

	FUeAgentInterfaceLogger::Log(TEXT("Server started. listen=%s port=%d token=%s version=%s"),
		*Settings->ListenAddress,
		Settings->Port,
		*UeAgentHttpServer::MaskToken(Settings->AuthToken),
		*FUeAgentInterfaceLogger::GetVersionTag());

	return true;
}

void FUeAgentHttpServer::Stop()
{
	if (!bRunning && !Router.IsValid())
	{
		return;
	}

	UnbindRoutes();
	Router.Reset();
	// HTTPServer's public module API only exposes a module-wide listener stop.
	FHttpServerModule::Get().StopAllListeners();
	bRunning = false;
	FUeAgentInterfaceLogger::Log(TEXT("Server stopped (routes unbound, listeners stopped)."));
}

bool FUeAgentHttpServer::IsRunning() const
{
	return bRunning;
}

int32 FUeAgentHttpServer::GetPort() const
{
	const UUeAgentInterfaceSettings* Settings = UUeAgentInterfaceSettings::Get();
	return Settings ? Settings->Port : 0;
}

FString FUeAgentHttpServer::GetListenAddress() const
{
	const UUeAgentInterfaceSettings* Settings = UUeAgentInterfaceSettings::Get();
	return Settings ? Settings->ListenAddress : TEXT("127.0.0.1");
}

FString FUeAgentHttpServer::GetAuthTokenMasked() const
{
	const UUeAgentInterfaceSettings* Settings = UUeAgentInterfaceSettings::Get();
	return Settings ? UeAgentHttpServer::MaskToken(Settings->AuthToken) : TEXT("********");
}

FString FUeAgentHttpServer::GetAuthToken() const
{
	const UUeAgentInterfaceSettings* Settings = UUeAgentInterfaceSettings::Get();
	return Settings ? Settings->AuthToken : FString();
}

FString FUeAgentHttpServer::FormatResponseJson(const TSharedRef<FJsonObject>& Root, const FUeAgentResponseOptions& Options)
{
	return FUeAgentStructuredJsonFormatter::WriteObject(Root);
}

bool FUeAgentHttpServer::BindRoutes()
{
	if (!Router.IsValid())
	{
		return false;
	}

	auto BindCheckedRoute = [this](const TCHAR* RoutePath, EHttpServerRequestVerbs Verbs, const FHttpRequestHandler& Handler) -> bool
	{
		const FHttpPath Path(RoutePath);
		const FHttpRouteHandle RouteHandle = Router->BindRoute(Path, Verbs, Handler);
		if (!RouteHandle.IsValid())
		{
			FUeAgentInterfaceLogger::Log(TEXT("BindRoutes: failed to bind route path=%s verbs=%u"), *Path.GetPath(), static_cast<uint32>(Verbs));
			return false;
		}

		RouteHandles.Add(RouteHandle);
		return true;
	};

	if (!BindCheckedRoute(TEXT("/api/ping"), EHttpServerRequestVerbs::VERB_GET, FHttpRequestHandler::CreateRaw(this, &FUeAgentHttpServer::HandlePing)))
	{
		return false;
	}

	if (!BindCheckedRoute(TEXT("/api/status"), EHttpServerRequestVerbs::VERB_GET, FHttpRequestHandler::CreateRaw(this, &FUeAgentHttpServer::HandleStatus)))
	{
		return false;
	}

	if (!BindCheckedRoute(TEXT("/api/exec"), EHttpServerRequestVerbs::VERB_POST, FHttpRequestHandler::CreateRaw(this, &FUeAgentHttpServer::HandleExec)))
	{
		return false;
	}

	RequestLogPreprocessorHandle = Router->RegisterRequestPreprocessor(FHttpRequestHandler::CreateLambda(
		[](const FHttpServerRequest& Request, const FHttpResultCallback&)
		{
			FUeAgentInterfaceLogger::Log(
				TEXT("Preprocess request verb=%s path=%s body_bytes=%d"),
				UeAgentHttpServer::VerbToString(Request.Verb),
				*Request.RelativePath.GetPath(),
				Request.Body.Num());
			return false;
		}));

	return true;
}

void FUeAgentHttpServer::UnbindRoutes()
{
	if (!Router.IsValid())
	{
		RequestLogPreprocessorHandle.Reset();
		RouteHandles.Reset();
		return;
	}

	if (RequestLogPreprocessorHandle.IsValid())
	{
		Router->UnregisterRequestPreprocessor(RequestLogPreprocessorHandle);
		RequestLogPreprocessorHandle.Reset();
	}

	for (FHttpRouteHandle& Handle : RouteHandles)
	{
		if (Handle.IsValid())
		{
			Router->UnbindRoute(Handle);
		}
	}
	RouteHandles.Reset();
}

bool FUeAgentHttpServer::HandlePing(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	FUeAgentInterfaceLogger::Log(TEXT("HandlePing verb=%s path=%s"), UeAgentHttpServer::VerbToString(Request.Verb), *Request.RelativePath.GetPath());

	FUeAgentRequestContext Ctx;
	Ctx.Command = TEXT("ping");
	Ctx.Params = MakeShared<FJsonObject>();
	FUeAgentCommandOutput Output;
	Output.Data = MakeShared<FJsonObject>();

	const TSharedPtr<FJsonObject> Root = FUeAgentResponseBuilder::BuildResponseEnvelope(Ctx, true, FString(), Output);
	OnComplete(MakeJsonResponse(200, FormatResponseJson(Root.ToSharedRef(), Ctx.ResponseOptions)));
	return true;
}

bool FUeAgentHttpServer::HandleStatus(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	FUeAgentInterfaceLogger::Log(TEXT("HandleStatus verb=%s path=%s"), UeAgentHttpServer::VerbToString(Request.Verb), *Request.RelativePath.GetPath());

	FString Reason;
	if (!IsAuthorized(Request, Reason))
	{
		FUeAgentInterfaceLogger::Log(TEXT("HandleStatus unauthorized reason=%s"), *Reason);
		OnComplete(MakeErrorResponse(401, TEXT("unauthorized"), Reason));
		return true;
	}

	const UUeAgentInterfaceSettings* Settings = UUeAgentInterfaceSettings::Get();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("running"), bRunning);
	Data->SetStringField(TEXT("listen_address"), Settings ? Settings->ListenAddress : TEXT(""));
	Data->SetNumberField(TEXT("port"), Settings ? Settings->Port : 0);
	Data->SetStringField(TEXT("token_masked"), Settings ? UeAgentHttpServer::MaskToken(Settings->AuthToken) : TEXT("********"));
	Data->SetStringField(TEXT("version_tag"), FUeAgentInterfaceLogger::GetVersionTag());
	Data->SetBoolField(TEXT("screenshots_enabled"), Settings ? Settings->bEnableScreenshots : false);

	FUeAgentRequestContext Ctx;
	Ctx.Command = TEXT("status");
	Ctx.Params = MakeShared<FJsonObject>();
	FUeAgentCommandOutput Output;
	Output.Data = Data;

	const TSharedPtr<FJsonObject> Root = FUeAgentResponseBuilder::BuildResponseEnvelope(Ctx, true, FString(), Output);
	OnComplete(MakeJsonResponse(200, FormatResponseJson(Root.ToSharedRef(), Ctx.ResponseOptions)));
	return true;
}

bool FUeAgentHttpServer::HandleExec(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	FUeAgentInterfaceLogger::Log(TEXT("HandleExec verb=%s path=%s body_bytes=%d"), UeAgentHttpServer::VerbToString(Request.Verb), *Request.RelativePath.GetPath(), Request.Body.Num());

	FString Reason;
	if (!IsAuthorized(Request, Reason))
	{
		FUeAgentInterfaceLogger::Log(TEXT("HandleExec unauthorized reason=%s"), *Reason);
		OnComplete(MakeErrorResponse(401, TEXT("unauthorized"), Reason));
		return true;
	}

	FUeAgentRequestContext Ctx;
	FString ParseError;
	if (!ParseExecBody(Request, Ctx, ParseError))
	{
		FUeAgentInterfaceLogger::Log(TEXT("HandleExec parse_failed error=%s"), *ParseError);
		OnComplete(MakeErrorResponse(400, TEXT("invalid_request"), ParseError));
		return true;
	}

	FUeAgentCommandOutput Output;
	FString ExecError;
	const bool bOk = ExecuteCommandOnGameThread(Ctx, Output, ExecError);
	FUeAgentInterfaceLogger::Log(TEXT("HandleExec command=%s request_id=%s ok=%s error=%s"), *Ctx.Command, *Ctx.RequestId, bOk ? TEXT("true") : TEXT("false"), *ExecError);

	const TSharedPtr<FJsonObject> Root = FUeAgentResponseBuilder::BuildResponseEnvelope(Ctx, bOk, ExecError, Output);
	OnComplete(MakeJsonResponse(bOk ? 200 : 400, FormatResponseJson(Root.ToSharedRef(), Ctx.ResponseOptions)));
	return true;
}

bool FUeAgentHttpServer::IsAuthorized(const FHttpServerRequest& Request, FString& OutReason) const
{
	const UUeAgentInterfaceSettings* Settings = UUeAgentInterfaceSettings::Get();
	if (!Settings)
	{
		OutReason = TEXT("missing_settings");
		return false;
	}

	// Best-effort: if listening on loopback only, reject non-loopback peers.
	if (Settings->ListenAddress == TEXT("127.0.0.1") || Settings->ListenAddress == TEXT("localhost"))
	{
		if (!UeAgentHttpServer::IsLoopbackPeer(Request.PeerAddress))
		{
			OutReason = TEXT("non_loopback_peer");
			return false;
		}
	}

	const FString TokenHeader = UeAgentHttpServer::GetHeaderFirst(Request.Headers, UeAgentHttpServer::HeaderTokenLower);
	if (TokenHeader.IsEmpty())
	{
		OutReason = TEXT("missing_token_header");
		return false;
	}

	if (Settings->AuthToken.IsEmpty())
	{
		OutReason = TEXT("missing_server_token");
		return false;
	}

	if (TokenHeader != Settings->AuthToken)
	{
		OutReason = TEXT("token_mismatch");
		return false;
	}

	return true;
}

TUniquePtr<FHttpServerResponse> FUeAgentHttpServer::MakeJsonResponse(int32 Code, const FString& JsonText)
{
	TUniquePtr<FHttpServerResponse> Resp = FHttpServerResponse::Create(JsonText, UeAgentHttpServer::ContentTypeJson);
	Resp->Code = UeAgentHttpServer::ToResponseCode(Code);
	Resp->Headers.Add(TEXT("Cache-Control"), { TEXT("no-store") });
	return Resp;
}

TUniquePtr<FHttpServerResponse> FUeAgentHttpServer::MakeErrorResponse(int32 Code, const FString& ErrorCode, const FString& ErrorMessage)
{
	FUeAgentRequestContext Ctx;
	Ctx.Command = TEXT("error");
	Ctx.Params = MakeShared<FJsonObject>();

	FUeAgentCommandOutput Output;
	Output.Data = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Data = Output.Data;
	Data->SetStringField(TEXT("error_code"), ErrorCode);
	Data->SetStringField(TEXT("error_message"), ErrorMessage);
	Data->SetStringField(TEXT("timestamp"), UeAgentHttpServer::NowIso());

	const TSharedPtr<FJsonObject> Root = FUeAgentResponseBuilder::BuildResponseEnvelope(Ctx, false, ErrorCode, Output);
	return MakeJsonResponse(Code, FormatResponseJson(Root.ToSharedRef(), Ctx.ResponseOptions));
}

bool FUeAgentHttpServer::ParseResponseOptions(const TSharedPtr<FJsonObject>& Root, FUeAgentResponseOptions& OutOptions, FString& OutError)
{
	OutOptions = FUeAgentResponseOptions();

	const TSharedPtr<FJsonObject>* OptionsObj = nullptr;
	if (!Root.IsValid() || !Root->TryGetObjectField(TEXT("response_options"), OptionsObj) || !OptionsObj || !OptionsObj->IsValid())
	{
		return true;
	}

	FString View;
	if (JsonTryGetString(*OptionsObj, TEXT("view"), View) && !View.IsEmpty())
	{
		OutOptions.View = View;
	}

	bool bDebug = false;
	if (JsonTryGetBool(*OptionsObj, TEXT("debug"), bDebug))
	{
		OutOptions.bDebug = bDebug;
	}

	FString Format;
	if (JsonTryGetString(*OptionsObj, TEXT("format"), Format) && !Format.IsEmpty())
	{
		OutOptions.Format = Format;
	}

	double MaxItems = 0.0;
	if (JsonTryGetNumber(*OptionsObj, TEXT("max_items"), MaxItems))
	{
		if (MaxItems <= 0.0)
		{
			OutError = TEXT("invalid_response_options.max_items");
			return false;
		}
		OutOptions.MaxItems = FMath::RoundToInt(MaxItems);
	}

	return true;
}

bool FUeAgentHttpServer::ParseExecBody(const FHttpServerRequest& Request, FUeAgentRequestContext& OutCtx, FString& OutError) const
{
	const FUTF8ToTCHAR BodyConv(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
	const FString BodyStr(BodyConv.Length(), BodyConv.Get());
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(BodyStr);

	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = TEXT("invalid_json");
		return false;
	}

	if (!JsonTryGetString(Root, TEXT("request_id"), OutCtx.RequestId))
	{
		OutCtx.RequestId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	}

	if (!JsonTryGetString(Root, TEXT("command"), OutCtx.Command) || OutCtx.Command.IsEmpty())
	{
		OutError = TEXT("missing_command");
		return false;
	}

	const TSharedPtr<FJsonObject>* PParams = nullptr;
	if (Root->TryGetObjectField(TEXT("params"), PParams) && PParams && PParams->IsValid())
	{
		OutCtx.Params = *PParams;
	}
	else
	{
		OutCtx.Params = MakeShared<FJsonObject>();
	}

	if (!ParseResponseOptions(Root, OutCtx.ResponseOptions, OutError))
	{
		return false;
	}

	return true;
}

bool FUeAgentHttpServer::ParseBatchCommands(const FUeAgentRequestContext& BatchCtx, TArray<FUeAgentRequestContext>& OutCommands, bool& bOutStopOnError, FString& OutError) const
{
	OutCommands.Reset();
	bOutStopOnError = true;

	if (!BatchCtx.Params.IsValid())
	{
		OutError = TEXT("missing_params");
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* CommandValues = nullptr;
	if (!BatchCtx.Params->TryGetArrayField(TEXT("commands"), CommandValues) || !CommandValues)
	{
		OutError = TEXT("missing_commands");
		return false;
	}

	if (CommandValues->Num() <= 0)
	{
		OutError = TEXT("commands_empty");
		return false;
	}

	bool bStopOnError = true;
	BatchCtx.Params->TryGetBoolField(TEXT("stop_on_error"), bStopOnError);
	bOutStopOnError = bStopOnError;

	OutCommands.Reserve(CommandValues->Num());
	for (int32 Index = 0; Index < CommandValues->Num(); ++Index)
	{
		const TSharedPtr<FJsonValue>& CommandValue = (*CommandValues)[Index];
		if (!CommandValue.IsValid() || CommandValue->Type != EJson::Object)
		{
			OutError = FString::Printf(TEXT("invalid_command_item_%d"), Index);
			return false;
		}

		const TSharedPtr<FJsonObject> CommandObj = CommandValue->AsObject();
		if (!CommandObj.IsValid())
		{
			OutError = FString::Printf(TEXT("invalid_command_item_object_%d"), Index);
			return false;
		}

		FString CommandName;
		if (!JsonTryGetString(CommandObj, TEXT("command"), CommandName) || CommandName.IsEmpty())
		{
			OutError = FString::Printf(TEXT("missing_command_%d"), Index);
			return false;
		}

		if (CommandName.Equals(TEXT("exec_batch"), ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(TEXT("nested_exec_batch_not_allowed_%d"), Index);
			return false;
		}

		FString RequestId;
		if (!JsonTryGetString(CommandObj, TEXT("request_id"), RequestId) || RequestId.IsEmpty())
		{
			RequestId = FString::Printf(TEXT("%s-%d"), *BatchCtx.RequestId, Index);
		}

		const TSharedPtr<FJsonObject>* ParamsObj = nullptr;
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		if (CommandObj->TryGetObjectField(TEXT("params"), ParamsObj) && ParamsObj && ParamsObj->IsValid())
		{
			Params = *ParamsObj;
		}

		FUeAgentRequestContext SubCommand;
		SubCommand.RequestId = RequestId;
		SubCommand.Command = CommandName;
		SubCommand.Params = Params;
		SubCommand.ResponseOptions = BatchCtx.ResponseOptions;
		OutCommands.Add(SubCommand);
	}

	return true;
}

bool FUeAgentHttpServer::ExecuteBatchCommands(const FUeAgentRequestContext& BatchCtx, TSharedPtr<FJsonObject>& OutData, TArray<TSharedPtr<FJsonValue>>& OutArtifacts, FString& OutError) const
{
	TArray<FUeAgentRequestContext> Commands;
	bool bStopOnError = true;
	FString ParseError;
	if (!ParseBatchCommands(BatchCtx, Commands, bStopOnError, ParseError))
	{
		OutError = ParseError;
		return false;
	}

	FUeAgentInterfaceLogger::Log(TEXT("ExecBatch start request_id=%s count=%d stop_on_error=%s"), *BatchCtx.RequestId, Commands.Num(), bStopOnError ? TEXT("true") : TEXT("false"));

	OutData = MakeShared<FJsonObject>();
	OutArtifacts.Reset();

	TArray<TSharedPtr<FJsonValue>> Results;
	Results.Reserve(Commands.Num());

	int32 Succeeded = 0;
	int32 FailedIndex = INDEX_NONE;
	FString FailedCommand;
	FString FailedRequestId;
	FString FailedError;

	for (int32 Index = 0; Index < Commands.Num(); ++Index)
	{
		const FUeAgentRequestContext& SubCommand = Commands[Index];
		FUeAgentCommandOutput SubOutput;
		FString SubError;
		const bool bSubOk = ExecuteCommandOnGameThread(SubCommand, SubOutput, SubError);

		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("index"), Index);
		Item->SetStringField(TEXT("request_id"), SubCommand.RequestId);
		Item->SetStringField(TEXT("command"), SubCommand.Command);
		Item->SetBoolField(TEXT("ok"), bSubOk);
		Item->SetStringField(TEXT("error"), bSubOk ? TEXT("") : SubError);
		Item->SetObjectField(TEXT("data"), SubOutput.GetDataOrEmpty());
		Item->SetArrayField(TEXT("artifacts"), SubOutput.Artifacts);
		Results.Add(MakeShared<FJsonValueObject>(Item));

		if (bSubOk)
		{
			++Succeeded;
		}
		else if (FailedIndex == INDEX_NONE)
		{
			FailedIndex = Index;
			FailedCommand = SubCommand.Command;
			FailedRequestId = SubCommand.RequestId;
			FailedError = SubError;

			FUeAgentInterfaceLogger::Log(TEXT("ExecBatch interrupted request_id=%s index=%d command=%s error=%s"),
				*BatchCtx.RequestId, FailedIndex, *FailedCommand, *FailedError);

			if (bStopOnError)
			{
				break;
			}
		}
	}

	const int32 Executed = Results.Num();
	const bool bInterrupted = (FailedIndex != INDEX_NONE) && bStopOnError;

	OutData->SetStringField(TEXT("mode"), TEXT("exec_batch"));
	OutData->SetNumberField(TEXT("total"), Commands.Num());
	OutData->SetNumberField(TEXT("executed"), Executed);
	OutData->SetNumberField(TEXT("succeeded"), Succeeded);
	OutData->SetNumberField(TEXT("failed"), Executed - Succeeded);
	OutData->SetBoolField(TEXT("stop_on_error"), bStopOnError);
	OutData->SetBoolField(TEXT("interrupted"), bInterrupted);
	OutData->SetNumberField(TEXT("failed_index"), FailedIndex);
	OutData->SetStringField(TEXT("failed_command"), FailedCommand);
	OutData->SetStringField(TEXT("failed_request_id"), FailedRequestId);
	OutData->SetStringField(TEXT("failed_error"), FailedError);
	OutData->SetArrayField(TEXT("results"), Results);

	if (FailedIndex != INDEX_NONE)
	{
		OutError = FString::Printf(TEXT("batch_interrupted_at_%d:%s"), FailedIndex, *FailedError);
		return false;
	}

	FUeAgentInterfaceLogger::Log(TEXT("ExecBatch done request_id=%s total=%d succeeded=%d"), *BatchCtx.RequestId, Commands.Num(), Succeeded);
	return true;
}

bool FUeAgentHttpServer::ExecuteCommandOnGameThread(const FUeAgentRequestContext& Ctx, FUeAgentCommandOutput& OutOutput, FString& OutError) const
{
	auto ExecOnGT = [this](const FUeAgentRequestContext& InCtx, FUeAgentCommandOutput& Output, FString& Err) -> bool
	{
		Output.Reset();
		Err.Reset();

		TSharedPtr<FJsonObject> LegacyData = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> LegacyArtifacts;
		const FString Cmd = InCtx.Command.ToLower();

		if (const UeAgentCommandRegistry::FExactCommandInvoker ExactInvoker = UeAgentCommandRegistry::FindExactCommandInvoker(Cmd))
		{
			const bool bOk = ExactInvoker(this, InCtx, LegacyData, LegacyArtifacts, Err);
			Output = FUeAgentCommandOutput::FromLegacy(LegacyData, LegacyArtifacts);
			return bOk;
		}

		if (const UeAgentCommandRegistry::FPrefixedCommandInvoker PrefixedInvoker = UeAgentCommandRegistry::FindPrefixedCommandInvoker(Cmd))
		{
			const bool bOk = PrefixedInvoker(this, Cmd, InCtx, LegacyData, Err);
			Output = FUeAgentCommandOutput::FromLegacy(LegacyData, LegacyArtifacts);
			return bOk;
		}

		Err = TEXT("unknown_command");
		Output.Data = MakeShared<FJsonObject>();
		return false;
	};

	if (IsInGameThread())
	{
		FUeAgentCommandOutput Output;
		FString Err;
		const bool bOk = ExecOnGT(Ctx, Output, Err);
		OutOutput = Output;
		if (!bOk)
		{
			OutError = Err;
			return false;
		}
		return true;
	}

	struct FExecResult
	{
		bool bOk = false;
		FUeAgentCommandOutput Output;
		FString Error;
	};

	TSharedRef<FExecResult, ESPMode::ThreadSafe> Result = MakeShared<FExecResult, ESPMode::ThreadSafe>();
	FEvent* Done = FPlatformProcess::GetSynchEventFromPool(true);

	AsyncTask(ENamedThreads::GameThread, [ExecOnGT, Ctx, Result, Done]()
	{
		FString Err;
		FUeAgentCommandOutput Output;
		const bool bOk = ExecOnGT(Ctx, Output, Err);

		Result->bOk = bOk;
		Result->Output = Output;
		Result->Error = Err;
		Done->Trigger();
	});

	const bool bDone = Done->Wait(FTimespan::FromSeconds(30));
	FPlatformProcess::ReturnSynchEventToPool(Done);

	if (!bDone)
	{
		OutError = TEXT("timeout_waiting_gamethread");
		return false;
	}

	OutOutput = Result->Output;
	if (!Result->bOk)
	{
		OutError = Result->Error;
		return false;
	}

	return true;
}

bool FUeAgentHttpServer::ExecuteCommandOnGameThread(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, TArray<TSharedPtr<FJsonValue>>& OutArtifacts, FString& OutError) const
{
	FUeAgentCommandOutput Output;
	const bool bOk = ExecuteCommandOnGameThread(Ctx, Output, OutError);
	OutData = Output.GetDataOrEmpty();
	OutArtifacts = Output.Artifacts;
	return bOk;
}

bool FUeAgentHttpServer::JsonTryGetObject(const TSharedPtr<FJsonObject>& Obj, const FString& Key, TSharedPtr<FJsonObject>& OutObj)
{
	if (!Obj.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* P = nullptr;
	if (Obj->TryGetObjectField(Key, P) && P && P->IsValid())
	{
		OutObj = *P;
		return true;
	}
	return false;
}

bool FUeAgentHttpServer::JsonTryGetString(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FString& OutValue)
{
	return Obj.IsValid() && Obj->TryGetStringField(Key, OutValue);
}

bool FUeAgentHttpServer::JsonTryGetNumber(const TSharedPtr<FJsonObject>& Obj, const FString& Key, double& OutValue)
{
	return UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { Key }, OutValue);
}

bool FUeAgentHttpServer::JsonTryGetBool(const TSharedPtr<FJsonObject>& Obj, const FString& Key, bool& OutValue)
{
	return Obj.IsValid() && Obj->TryGetBoolField(Key, OutValue);
}

bool FUeAgentHttpServer::JsonTryGetVector(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FVector& OutValue)
{
	const TSharedPtr<FJsonValue>* FieldValue = UeAgentJsonDiagnostics::FindFieldValue(Obj, Key);
	return FieldValue && UeAgentJsonDiagnostics::TryReadVectorValue(*FieldValue, OutValue);
}

bool FUeAgentHttpServer::JsonTryGetRotator(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FRotator& OutValue)
{
	const TSharedPtr<FJsonValue>* FieldValue = UeAgentJsonDiagnostics::FindFieldValue(Obj, Key);
	return FieldValue && UeAgentJsonDiagnostics::TryReadRotatorValue(*FieldValue, OutValue);
}

TSharedPtr<FJsonObject> FUeAgentHttpServer::VecToJson(const FVector& V)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetNumberField(TEXT("x"), V.X);
	Obj->SetNumberField(TEXT("y"), V.Y);
	Obj->SetNumberField(TEXT("z"), V.Z);
	return Obj;
}

TSharedPtr<FJsonObject> FUeAgentHttpServer::RotToJson(const FRotator& R)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetNumberField(TEXT("pitch"), R.Pitch);
	Obj->SetNumberField(TEXT("yaw"), R.Yaw);
	Obj->SetNumberField(TEXT("roll"), R.Roll);
	return Obj;
}

void FUeAgentHttpServer::SetPropertyImportResultFields(
	const TSharedPtr<FJsonObject>& Obj,
	FProperty* Property,
	const FString& RequestedValueText,
	const FString& AppliedValueText,
	const FString& ImportStatus,
	const FString& ImportError)
{
	if (!Obj.IsValid())
	{
		return;
	}

	const bool bReadBackValue = ImportStatus.Equals(TEXT("imported_and_read_back"), ESearchCase::CaseSensitive)
		|| ImportStatus.Equals(TEXT("assigned_object_reference"), ESearchCase::CaseSensitive);
	const bool bExactMatch = bReadBackValue && AppliedValueText.Equals(RequestedValueText, ESearchCase::CaseSensitive);
	Obj->SetStringField(TEXT("requested_value_text"), RequestedValueText);
	Obj->SetStringField(TEXT("applied_value_text"), AppliedValueText);
	Obj->SetBoolField(TEXT("property_value_read_back"), bReadBackValue);
	Obj->SetBoolField(TEXT("value_text_exact_match"), bExactMatch);
	Obj->SetBoolField(TEXT("value_text_changed_after_import"), bReadBackValue && !bExactMatch);
	Obj->SetStringField(TEXT("property_import_status"), ImportStatus);
	Obj->SetBoolField(TEXT("property_import_verified"), bReadBackValue);
	Obj->SetStringField(TEXT("property_import_error"), bReadBackValue ? TEXT("") : (ImportError.IsEmpty() ? ImportStatus : ImportError));
	Obj->SetStringField(TEXT("cpp_type"), Property ? Property->GetCPPType() : TEXT(""));
}

TSharedPtr<FJsonObject> FUeAgentHttpServer::MakePropertyImportResultJson(
	const FString& PropertyName,
	FProperty* Property,
	const FString& RequestedValueText,
	const FString& AppliedValueText,
	const FString& ImportStatus,
	const FString& ImportError)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("property_name"), PropertyName);
	SetPropertyImportResultFields(Obj, Property, RequestedValueText, AppliedValueText, ImportStatus, ImportError);
	return Obj;
}
