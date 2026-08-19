// Copyright Epic Games, Inc. All Rights Reserved.

#include "Server/UeAgentHttpServer.h"
#include "Server/UeAgentCommandRegistry.h"
#include "Server/Generated/UeAgentCommandSpecBuildInfo.h"
#include "Server/Generated/UeAgentCommandSpecFailurePolicy.h"
#include "Server/Generated/UeAgentCommandSpecResponsePolicy.h"

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

	static bool TryGetSystemCompactPolicy(
		const FString& Command,
		TSet<FString>& OutCompactFields,
		TSet<FString>& OutArtifactFields,
		UeAgentCommandSpecResponsePolicy::FMaxItemsPolicy& OutMaxItemsPolicy)
	{
		OutCompactFields.Reset();
		OutArtifactFields.Reset();
		OutMaxItemsPolicy.Reset();
		if (Command.Equals(TEXT("ping"), ESearchCase::IgnoreCase))
		{
			return true;
		}
		if (Command.Equals(TEXT("error"), ESearchCase::IgnoreCase))
		{
			OutCompactFields = {TEXT("error_code")};
			OutArtifactFields = {TEXT("error_message")};
			return true;
		}
		if (Command.Equals(TEXT("status"), ESearchCase::IgnoreCase))
		{
			OutCompactFields = {
				TEXT("running"),
				TEXT("listen_address"),
				TEXT("port"),
				TEXT("token_masked"),
				TEXT("version_tag"),
				TEXT("plugin_version_tag"),
				TEXT("server_instance_id"),
				TEXT("command_spec_schema"),
				TEXT("command_spec_version"),
				TEXT("command_spec_hash"),
				TEXT("command_spec_shared_types_hash"),
				TEXT("command_count"),
				TEXT("active_command_count"),
				TEXT("deprecated_command_count"),
				TEXT("screenshots_enabled"),
			};
			return true;
		}
		return false;
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
		if (!FPaths::IsRelative(RootRel))
		{
			return FString();
		}
		const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		const FString ResponseDir = FPaths::ConvertRelativePathToFull(ProjectDir, RootRel / TEXT("Responses"));
		FString RelativeResponseDir = ResponseDir;
		if (!FPaths::MakePathRelativeTo(RelativeResponseDir, *ProjectDir))
		{
			return FString();
		}
		FPaths::NormalizeFilename(RelativeResponseDir);
		if (RelativeResponseDir.Equals(TEXT("..")) || RelativeResponseDir.StartsWith(TEXT("../")))
		{
			return FString();
		}
		const FString SafeCommand = SanitizeArtifactSegment(Ctx.Command.IsEmpty() ? TEXT("command") : Ctx.Command);
		const FString SafeRequestId = SanitizeArtifactSegment(Ctx.RequestId.IsEmpty() ? TEXT("request") : Ctx.RequestId);
		const FString SafeField = SanitizeArtifactSegment(FieldName);
		const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		return FPaths::Combine(ResponseDir, SafeCommand, FString::Printf(TEXT("%s_%s_%s.hjson"), *SafeRequestId, *SafeField, *Guid));
	}

	static TSharedPtr<FJsonObject> MakeResponseArtifactDescriptor(
		const TSharedPtr<FJsonValue>& Value,
		const FString& FilePath)
	{
		TSharedPtr<FJsonObject> Descriptor = MakeShared<FJsonObject>();
		Descriptor->SetStringField(TEXT("kind"), TEXT("response_data"));
		FString RelativePath = FilePath;
		FPaths::MakePathRelativeTo(RelativePath, *FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
		FPaths::NormalizeFilename(RelativePath);
		Descriptor->SetStringField(TEXT("path"), RelativePath);
		Descriptor->SetStringField(TEXT("read_section"), TEXT("value"));
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
		if (OutFilePath.IsEmpty())
		{
			return false;
		}

		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.response_artifact.v1"));
		Payload->SetStringField(TEXT("command"), Ctx.Command);
		Payload->SetStringField(TEXT("request_id"), Ctx.RequestId);
		Payload->SetStringField(TEXT("field"), FieldName);
		Payload->SetField(TEXT("value"), Value.IsValid() ? Value : MakeShared<FJsonValueNull>());

		const FString JsonText = FUeAgentStructuredJsonFormatter::WriteObject(Payload.ToSharedRef()) + LINE_TERMINATOR;
		if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutFilePath), true))
		{
			return false;
		}
		return FFileHelper::SaveStringToFile(JsonText, *OutFilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	static void AppendExternalizedHiddenDataArtifacts(
		const FUeAgentRequestContext& Ctx,
		const TSharedPtr<FJsonObject>& HiddenData,
		const TSet<FString>& PolicyArtifactFields,
		TArray<TSharedPtr<FJsonValue>>& InOutArtifacts,
		TArray<TSharedPtr<FJsonValue>>& InOutWarnings)
	{
		if (!HiddenData.IsValid())
		{
			return;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : HiddenData->Values)
		{
			if (!PolicyArtifactFields.Contains(Pair.Key.ToLower()))
			{
				continue;
			}

			FString FilePath;
			if (SaveResponseArtifact(Ctx, Pair.Key, Pair.Value, FilePath))
			{
				InOutArtifacts.Add(MakeShared<FJsonValueObject>(MakeResponseArtifactDescriptor(Pair.Value, FilePath)));
			}
			else
			{
				InOutWarnings.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("artifact_externalize_failed:%s"), *Pair.Key)));
			}
		}
	}

	static bool SplitCompactDataFields(
		const FUeAgentRequestContext& Ctx,
		const TSharedPtr<FJsonObject>& Data,
		TSharedPtr<FJsonObject>& OutVisible,
		TSharedPtr<FJsonObject>& OutHidden,
		TSet<FString>& OutArtifactFields,
		UeAgentCommandSpecResponsePolicy::FMaxItemsPolicy& OutMaxItemsPolicy)
	{
		OutVisible = MakeShared<FJsonObject>();
		OutHidden = MakeShared<FJsonObject>();
		TSet<FString> CompactFields;
		const bool bHasPolicy = UeAgentCommandSpecResponsePolicy::TryGetPolicy(
			Ctx.Command,
			CompactFields,
			OutArtifactFields,
			OutMaxItemsPolicy)
			|| TryGetSystemCompactPolicy(Ctx.Command, CompactFields, OutArtifactFields, OutMaxItemsPolicy);

		if (!Data.IsValid())
		{
			return bHasPolicy;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Data->Values)
		{
			if (!bHasPolicy || !CompactFields.Contains(Pair.Key.ToLower()))
			{
				OutHidden->SetField(Pair.Key, Pair.Value);
			}
			else
			{
				OutVisible->SetField(Pair.Key, Pair.Value);
			}
		}
		return bHasPolicy;
	}

	static void ApplyCompactMaxItems(
		const FUeAgentResponseOptions& Options,
		const UeAgentCommandSpecResponsePolicy::FMaxItemsPolicy& Policy,
		const TSharedPtr<FJsonObject>& VisibleData,
		TArray<TSharedPtr<FJsonValue>>& InOutWarnings)
	{
		if (!Policy.IsEnabled() || !VisibleData.IsValid())
		{
			return;
		}

		const int32 RequestedMaxItems = Options.MaxItems;
		const int32 EffectiveMaxItems = RequestedMaxItems > 0
			? FMath::Min(RequestedMaxItems, Policy.HardMaxItems)
			: Policy.DefaultMaxItems;
		if (RequestedMaxItems > Policy.HardMaxItems)
		{
			InOutWarnings.Add(MakeShared<FJsonValueString>(FString::Printf(
				TEXT("max_items_clamped:%d:%d"),
				RequestedMaxItems,
				Policy.HardMaxItems)));
		}

		TArray<FString> Fields = Policy.Fields.Array();
		Fields.Sort();
		TSharedPtr<FJsonObject> FieldPages = MakeShared<FJsonObject>();
		int32 TotalCount = 0;
		int32 ReturnedCount = 0;
		bool bAnyTruncated = false;
		for (const FString& Field : Fields)
		{
			const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
			if (!VisibleData->TryGetArrayField(Field, Array) || !Array)
			{
				continue;
			}

			const int32 FieldTotal = Array->Num();
			const int32 FieldReturned = FMath::Min(FieldTotal, EffectiveMaxItems);
			const bool bFieldTruncated = FieldReturned < FieldTotal;
			if (bFieldTruncated)
			{
				TArray<TSharedPtr<FJsonValue>> Truncated;
				Truncated.Append(Array->GetData(), FieldReturned);
				VisibleData->SetArrayField(Field, MoveTemp(Truncated));
			}

			TSharedPtr<FJsonObject> FieldPage = MakeShared<FJsonObject>();
			FieldPage->SetNumberField(TEXT("returned"), FieldReturned);
			FieldPage->SetNumberField(TEXT("total"), FieldTotal);
			FieldPage->SetBoolField(TEXT("truncated"), bFieldTruncated);
			FieldPages->SetObjectField(Field, FieldPage);
			TotalCount += FieldTotal;
			ReturnedCount += FieldReturned;
			bAnyTruncated = bAnyTruncated || bFieldTruncated;
		}

		if (FieldPages->Values.Num() == 0)
		{
			return;
		}

		TSharedPtr<FJsonObject> Page = MakeShared<FJsonObject>();
		const TSharedPtr<FJsonObject>* ExistingPage = nullptr;
		if (VisibleData->TryGetObjectField(Policy.PageField, ExistingPage) && ExistingPage && ExistingPage->IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*ExistingPage)->Values)
			{
				Page->SetField(Pair.Key, Pair.Value);
			}
		}
		Page->SetNumberField(TEXT("returned"), ReturnedCount);
		Page->SetNumberField(TEXT("total"), TotalCount);
		Page->SetBoolField(TEXT("truncated"), bAnyTruncated);
		Page->SetObjectField(TEXT("fields"), FieldPages);
		VisibleData->SetObjectField(Policy.PageField, Page);
	}

	static TSharedPtr<FJsonObject> BuildDebugObject(const FUeAgentCommandOutput& Output)
	{
		TSharedPtr<FJsonObject> Debug = MakeShared<FJsonObject>();

		if (Output.Debug.IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Output.Debug->Values)
			{
				Debug->SetField(Pair.Key, Pair.Value);
			}
		}

		return Debug;
	}

	static TSharedPtr<FJsonObject> BuildFailureObject(
		const FUeAgentRequestContext& Ctx,
		const FString& Error)
	{
		UeAgentCommandSpecFailurePolicy::FResolvedFailure Resolved;
		if (Ctx.Command.Equals(TEXT("error"), ESearchCase::IgnoreCase))
		{
			Resolved.Code = Error.IsEmpty() ? TEXT("invalid_request") : Error;
			Resolved.Stage = TEXT("transport");
			Resolved.bRetrySafe = true;
			Resolved.bRuleMatched = true;
		}
		else
		{
			UeAgentCommandSpecFailurePolicy::Resolve(Ctx.Command, Error, Resolved);
		}

		TSharedPtr<FJsonObject> Failure = MakeShared<FJsonObject>();
		Failure->SetStringField(TEXT("code"), Resolved.Code);
		Failure->SetStringField(TEXT("stage"), Resolved.Stage);
		Failure->SetBoolField(TEXT("retry_safe"), Resolved.bRetrySafe);
		Failure->SetStringField(TEXT("command"), Ctx.Command);
		Failure->SetStringField(
			TEXT("diagnostic_id"),
			FString::Printf(TEXT("%s:%s"), *Ctx.Command, *Resolved.Code));
		if (!Error.IsEmpty() && Error != Resolved.Code)
		{
			constexpr int32 MaxWireErrorChars = 512;
			Failure->SetStringField(TEXT("wire_error"), Error.Left(MaxWireErrorChars));
			if (Error.Len() > MaxWireErrorChars)
			{
				Failure->SetBoolField(TEXT("wire_error_truncated"), true);
			}
		}
		return Failure;
	}

	static void PreserveCompactFailureDiagnostics(
		const FUeAgentCommandOutput& Output,
		const TSharedPtr<FJsonObject>& VisibleData)
	{
		if (!VisibleData.IsValid())
		{
			return;
		}

		const TSharedPtr<FJsonObject> SourceData = Output.GetDataOrEmpty();
		static const TCHAR* DiagnosticFields[] = {
			TEXT("status"),
			TEXT("phase"),
			TEXT("error_code"),
			TEXT("error_detail"),
			TEXT("rollback_attempted"),
			TEXT("rollback_succeeded"),
		};
		for (const TCHAR* FieldName : DiagnosticFields)
		{
			if (const TSharedPtr<FJsonValue>* Value = SourceData->Values.Find(FieldName))
			{
				VisibleData->SetField(FieldName, *Value);
			}
		}
	}

	static bool IsDebugResponseView(const FUeAgentResponseOptions& Options)
	{
		return Options.View.Equals(TEXT("debug"), ESearchCase::IgnoreCase);
	}

	static bool IsRawResponseView(const FUeAgentResponseOptions& Options)
	{
		return Options.View.Equals(TEXT("raw"), ESearchCase::IgnoreCase);
	}

	static bool IsCompactResponseView(const FUeAgentResponseOptions& Options)
	{
		return Options.View.Equals(TEXT("llm_compact"), ESearchCase::IgnoreCase);
	}
}

FUeAgentResponseOptions::FUeAgentResponseOptions()
	: View(TEXT("llm_compact"))
	, MaxItems(0)
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

FUeAgentHttpServer::FUeAgentHttpServer()
	: bRunning(false)
	, bPythonConsoleCommandExecuted(false)
	, ServerInstanceId(FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower))
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
	const TSharedPtr<FJsonObject> Failure = bOk
		? TSharedPtr<FJsonObject>()
		: UeAgentHttpServer::BuildFailureObject(Ctx, Error);

	if (UeAgentHttpServer::IsRawResponseView(Ctx.ResponseOptions))
	{
		Root->SetStringField(TEXT("request_id"), Ctx.RequestId);
		Root->SetStringField(TEXT("error"), bOk ? TEXT("") : (Error.IsEmpty() ? TEXT("command_failed") : Error));
		Root->SetStringField(TEXT("timestamp"), UeAgentHttpServer::NowIso());
		Root->SetObjectField(TEXT("data"), Output.GetDataOrEmpty());
		Root->SetArrayField(TEXT("artifacts"), Output.Artifacts);
		Root->SetArrayField(TEXT("warnings"), Output.Warnings);
		if (Failure.IsValid())
		{
			Root->SetObjectField(TEXT("failure"), Failure);
		}
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
	const bool bCompactView = UeAgentHttpServer::IsCompactResponseView(Ctx.ResponseOptions);
	if (bCompactView)
	{
		TSet<FString> PolicyArtifactFields;
		UeAgentCommandSpecResponsePolicy::FMaxItemsPolicy MaxItemsPolicy;
		const bool bHasPolicy = UeAgentHttpServer::SplitCompactDataFields(
			Ctx,
			Output.GetDataOrEmpty(),
			VisibleData,
			HiddenData,
			PolicyArtifactFields,
			MaxItemsPolicy);
		UeAgentHttpServer::ApplyCompactMaxItems(Ctx.ResponseOptions, MaxItemsPolicy, VisibleData, Warnings);
		UeAgentHttpServer::AppendExternalizedHiddenDataArtifacts(Ctx, HiddenData, PolicyArtifactFields, Artifacts, Warnings);
		if (!bHasPolicy)
		{
			Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("compact_policy_missing:%s"), *Ctx.Command)));
		}
		if (!bOk)
		{
			UeAgentHttpServer::PreserveCompactFailureDiagnostics(Output, VisibleData);
		}
	}

	Root->SetObjectField(TEXT("data"), VisibleData);
	if (!bOk)
	{
		Root->SetStringField(TEXT("error"), Failure->GetStringField(TEXT("code")));
		Root->SetObjectField(TEXT("failure"), Failure);
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
		const TSharedPtr<FJsonObject> Debug = UeAgentHttpServer::BuildDebugObject(Output);
		if (!bOk)
		{
			Debug->SetStringField(TEXT("error_message"), Error.IsEmpty() ? TEXT("command_failed") : Error);
		}
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

FString FUeAgentHttpServer::GetServerInstanceId() const
{
	return ServerInstanceId;
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
	const FString PluginVersionTag = FUeAgentInterfaceLogger::GetVersionTag();
	Data->SetStringField(TEXT("version_tag"), PluginVersionTag);
	Data->SetStringField(TEXT("plugin_version_tag"), PluginVersionTag);
	Data->SetStringField(TEXT("server_instance_id"), ServerInstanceId);
	Data->SetStringField(TEXT("command_spec_schema"), UeAgentCommandSpecBuildInfo::Schema);
	Data->SetNumberField(TEXT("command_spec_version"), UeAgentCommandSpecBuildInfo::Version);
	Data->SetStringField(TEXT("command_spec_hash"), UeAgentCommandSpecBuildInfo::Hash);
	Data->SetStringField(TEXT("command_spec_shared_types_hash"), UeAgentCommandSpecBuildInfo::SharedTypesHash);
	Data->SetNumberField(TEXT("command_count"), UeAgentCommandSpecBuildInfo::CommandCount);
	Data->SetNumberField(TEXT("active_command_count"), UeAgentCommandSpecBuildInfo::ActiveCommandCount);
	Data->SetNumberField(TEXT("deprecated_command_count"), UeAgentCommandSpecBuildInfo::DeprecatedCommandCount);
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
	static const TSet<FString> AllowedFields = {TEXT("view"), TEXT("max_items")};
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*OptionsObj)->Values)
	{
		if (!AllowedFields.Contains(Pair.Key))
		{
			OutError = FString::Printf(TEXT("invalid_response_options.unknown_field:%s"), *Pair.Key);
			return false;
		}
	}

	FString View;
	if (JsonTryGetString(*OptionsObj, TEXT("view"), View) && !View.IsEmpty())
	{
		const FString NormalizedView = View.ToLower();
		if (NormalizedView != TEXT("llm_compact") && NormalizedView != TEXT("debug") && NormalizedView != TEXT("raw"))
		{
			OutError = TEXT("invalid_response_options.view");
			return false;
		}
		OutOptions.View = NormalizedView;
	}

	double MaxItems = 0.0;
	if (JsonTryGetNumber(*OptionsObj, TEXT("max_items"), MaxItems))
	{
		if (MaxItems <= 0.0
			|| MaxItems > static_cast<double>(MAX_int32)
			|| !FMath::IsNearlyEqual(MaxItems, FMath::RoundToDouble(MaxItems)))
		{
			OutError = TEXT("invalid_response_options.max_items");
			return false;
		}
		OutOptions.MaxItems = static_cast<int32>(MaxItems);
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

		if (UeAgentCommandRegistry::IsExplicitlyUnsupportedCommand(Cmd))
		{
			Err = TEXT("capability_not_supported");
			Output.Data = MakeShared<FJsonObject>();
			return false;
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

	// This is only a thread-affinity bridge. Deadline, cancellation and Outcome
	// belong to the application execution owner; returning while a queued Game
	// Thread mutation can still run would create ownerless late work.
	Done->Wait();
	FPlatformProcess::ReturnSynchEventToPool(Done);

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
