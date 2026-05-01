namespace
{
	struct FHttpSmokeResult
	{
		bool bCompleted = false;
		bool bRequestOk = false;
		int32 StatusCode = 0;
		FString Body;
		FString Error;
	};

	static bool ParseJson(const FString& Body, TSharedPtr<FJsonObject>& OutJson)
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
		return FJsonSerializer::Deserialize(Reader, OutJson) && OutJson.IsValid();
	}

	static bool IsHealthyPingResponse(const FHttpSmokeResult& Result)
	{
		if (Result.StatusCode != 200)
		{
			return false;
		}

		TSharedPtr<FJsonObject> Json;
		if (!ParseJson(Result.Body, Json) || !Json.IsValid())
		{
			return false;
		}

		bool bOk = false;
		return Json->TryGetBoolField(TEXT("ok"), bOk) && bOk;
	}

	static bool ExecuteHttpJsonRequest(
		const FString& Verb,
		const FString& Url,
		const FString& Token,
		const FString& Body,
		FHttpSmokeResult& OutResult,
		double TimeoutSeconds = 5.0,
		bool bLogFailures = true)
	{
		static bool bHttpTraceEnabled = FParse::Param(FCommandLine::Get(), TEXT("UeAgentInterfaceSmokeHttpTrace"));

		OutResult = FHttpSmokeResult{};
		TSharedRef<FHttpSmokeResult, ESPMode::ThreadSafe> SharedResult = MakeShared<FHttpSmokeResult, ESPMode::ThreadSafe>();
		if (bLogFailures && bHttpTraceEnabled)
		{
			UE_LOG(LogUeAgentInterfaceSmoke, Display, TEXT("UAI smoke request begin verb=%s url=%s body_bytes=%d"), *Verb, *Url, Body.Len());
		}

		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(Url);
		Request->SetVerb(Verb);
		if (!Token.IsEmpty())
		{
			Request->SetHeader(TEXT("X-UeAgentInterface-Token"), Token);
		}
		if (!Body.IsEmpty())
		{
			Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
			Request->SetContentAsString(Body);
		}

		Request->OnProcessRequestComplete().BindLambda(
			[SharedResult](FHttpRequestPtr, FHttpResponsePtr Response, bool bSucceeded)
			{
				SharedResult->bCompleted = true;
				SharedResult->bRequestOk = bSucceeded;
				if (Response.IsValid())
				{
					SharedResult->StatusCode = Response->GetResponseCode();
					SharedResult->Body = Response->GetContentAsString();
				}
				else
				{
					SharedResult->Error = TEXT("missing_response");
				}
			});

		if (!Request->ProcessRequest())
		{
			OutResult.Error = TEXT("process_request_failed");
			if (bLogFailures)
			{
				UE_LOG(LogUeAgentInterfaceSmoke, Warning, TEXT("UAI smoke request failed to start verb=%s url=%s"), *Verb, *Url);
			}
			return false;
		}

		const float TickIntervalSeconds = 0.01f;
		const double StartTime = FPlatformTime::Seconds();
		while (!SharedResult->bCompleted && (FPlatformTime::Seconds() - StartTime) < TimeoutSeconds)
		{
			FHttpModule::Get().GetHttpManager().Tick(TickIntervalSeconds);
			if (FTaskGraphInterface::IsRunning())
			{
				FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			}
			FTSTicker::GetCoreTicker().Tick(TickIntervalSeconds);
			FPlatformApplicationMisc::PumpMessages(true);
			FPlatformProcess::Sleep(TickIntervalSeconds);
		}

		if (!SharedResult->bCompleted)
		{
			SharedResult->Error = TEXT("request_timeout");
			Request->OnProcessRequestComplete().Unbind();
			Request->CancelRequest();
			OutResult = *SharedResult;
			if (bLogFailures)
			{
				UE_LOG(LogUeAgentInterfaceSmoke, Warning, TEXT("UAI smoke request timeout verb=%s url=%s"), *Verb, *Url);
			}
			return false;
		}

		Request->OnProcessRequestComplete().Unbind();
		OutResult = *SharedResult;
		if (bLogFailures && bHttpTraceEnabled)
		{
			UE_LOG(
				LogUeAgentInterfaceSmoke,
				Display,
				TEXT("UAI smoke request done verb=%s url=%s ok=%s status=%d error=%s body=%s"),
				*Verb,
				*Url,
				OutResult.bRequestOk ? TEXT("true") : TEXT("false"),
				OutResult.StatusCode,
				*OutResult.Error,
				*OutResult.Body.Left(256));
		}
		return OutResult.bRequestOk || OutResult.StatusCode > 0;
	}

	static FString MakeExecRequestBody(const FString& RequestId, const FString& Command, const FString& ParamsJson = TEXT("{}"))
	{
		return FString::Printf(
			TEXT("{\"request_id\":\"%s\",\"command\":\"%s\",\"params\":%s}"),
			*RequestId,
			*Command,
			*ParamsJson);
	}

	static FString MakeAutomationAssetPath(const FString& Prefix)
	{
		return FString::Printf(
			TEXT("/Game/__UeAgentInterfaceSmoke/%s_%s"),
			*Prefix,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	}

	struct FSharedUeAgentHttpServerState
	{
		FString BaseUrl;
		FString Token;
		FString LifecycleLogPath;
		TUniquePtr<FUeAgentHttpServer> OwnedServer;
		FDelegateHandle PreExitHandle;
		FDelegateHandle AfterAllTestsHandle;

		static bool IsLifecycleDebugEnabled()
		{
			static bool bEnabled = FParse::Param(FCommandLine::Get(), TEXT("UeAgentInterfaceSmokeLifecycleTrace"));
			return bEnabled;
		}

		void WriteLifecycleEvent(const FString& Event)
		{
			if (!IsLifecycleDebugEnabled())
			{
				return;
			}

			if (LifecycleLogPath.IsEmpty())
			{
				const FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%dT%H%M%S"));
				const FString Hostname = FPlatformProcess::ComputerName();
				LifecycleLogPath = FPaths::Combine(
					FPaths::ProjectLogDir(),
					FString::Printf(TEXT("uai_smoke_lifecycle_%s_%s.log"), *Hostname, *Timestamp));

				const FString Header = FString::Printf(TEXT("HOSTNAME=%s\n"), *Hostname);
				const bool bHeaderOk = FFileHelper::SaveStringToFile(Header, *LifecycleLogPath);
				UE_LOG(LogUeAgentInterfaceSmoke, Display, TEXT("UAI smoke lifecycle log init path=%s ok=%s"), *LifecycleLogPath, bHeaderOk ? TEXT("true") : TEXT("false"));
			}

			const FString Line = FString::Printf(TEXT("[%s] %s\n"), *FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")), *Event);
			const bool bAppendOk = FFileHelper::SaveStringToFile(Line, *LifecycleLogPath, FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), FILEWRITE_Append);
			UE_LOG(LogUeAgentInterfaceSmoke, Display, TEXT("UAI smoke lifecycle event path=%s ok=%s event=%s"), *LifecycleLogPath, bAppendOk ? TEXT("true") : TEXT("false"), *Event);
		}

		void BindLifecycleDelegatesIfNeeded()
		{
			if (!PreExitHandle.IsValid())
			{
				PreExitHandle = FCoreDelegates::OnPreExit.AddLambda(
					[]()
					{
						FSharedUeAgentHttpServerState::Get().ShutdownOwnedServer(TEXT("pre_exit"));
					});
			}

			if (!AfterAllTestsHandle.IsValid())
			{
				AfterAllTestsHandle = FAutomationTestFramework::Get().OnAfterAllTestsEvent.AddLambda(
					[]()
					{
						FSharedUeAgentHttpServerState::Get().ShutdownOwnedServer(TEXT("after_all_tests"));
					});
			}
		}

		static FSharedUeAgentHttpServerState& Get()
		{
			static FSharedUeAgentHttpServerState State;
			return State;
		}

		void ShutdownOwnedServer(const TCHAR* Reason = TEXT("unknown"))
		{
			if (IsLifecycleDebugEnabled() && (OwnedServer || AfterAllTestsHandle.IsValid() || PreExitHandle.IsValid()))
			{
				UE_LOG(LogUeAgentInterfaceSmoke, Display, TEXT("UAI smoke shared server shutdown reason=%s owned=%s"), Reason, OwnedServer ? TEXT("true") : TEXT("false"));
				WriteLifecycleEvent(FString::Printf(TEXT("shutdown reason=%s owned=%s"), Reason, OwnedServer ? TEXT("true") : TEXT("false")));
			}

			OwnedServer.Reset();
			if (AfterAllTestsHandle.IsValid())
			{
				FAutomationTestFramework::Get().OnAfterAllTestsEvent.Remove(AfterAllTestsHandle);
				AfterAllTestsHandle.Reset();
			}
			if (PreExitHandle.IsValid())
			{
				FCoreDelegates::OnPreExit.Remove(PreExitHandle);
				PreExitHandle.Reset();
			}
		}

		bool EnsureInitialized(FString& OutError)
		{
			UUeAgentInterfaceSettings* Settings = UUeAgentInterfaceSettings::GetMutable();
			if (!Settings)
			{
				OutError = TEXT("missing_settings");
				return false;
			}

			if (Settings->AuthToken.IsEmpty())
			{
				Settings->AuthToken = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
				Settings->SaveConfig();
			}

			BaseUrl = FString::Printf(TEXT("http://%s:%d"), *Settings->ListenAddress, Settings->Port);
			Token = Settings->AuthToken;

			if (OwnedServer && OwnedServer->IsRunning())
			{
				FHttpSmokeResult OwnedPingResult;
				ExecuteHttpJsonRequest(TEXT("GET"), BaseUrl + TEXT("/api/ping"), FString(), FString(), OwnedPingResult, 0.5, false);
				if (IsHealthyPingResponse(OwnedPingResult))
				{
					BindLifecycleDelegatesIfNeeded();
					WriteLifecycleEvent(TEXT("reuse_existing_owned_server"));
					return true;
				}

				ShutdownOwnedServer(TEXT("owned_server_unhealthy"));
			}

			if (OwnedServer && !OwnedServer->IsRunning())
			{
				ShutdownOwnedServer();
			}

			FHttpSmokeResult PingResult;
			ExecuteHttpJsonRequest(TEXT("GET"), BaseUrl + TEXT("/api/ping"), FString(), FString(), PingResult, 0.25, false);
			if (IsHealthyPingResponse(PingResult))
			{
				WriteLifecycleEvent(TEXT("reuse_external_server"));
				return true;
			}

			OwnedServer = MakeUnique<FUeAgentHttpServer>();
			if (!OwnedServer->Start())
			{
				OutError = TEXT("server_start_failed");
				return false;
			}

			BindLifecycleDelegatesIfNeeded();
			WriteLifecycleEvent(FString::Printf(TEXT("start_owned_server base_url=%s"), *BaseUrl));
			if (IsLifecycleDebugEnabled())
			{
				UE_LOG(LogUeAgentInterfaceSmoke, Display, TEXT("UAI smoke lifecycle log path=%s"), *LifecycleLogPath);
			}

			FHttpSmokeResult StartedPingResult;
			ExecuteHttpJsonRequest(TEXT("GET"), BaseUrl + TEXT("/api/ping"), FString(), FString(), StartedPingResult, 1.0);
			if (!IsHealthyPingResponse(StartedPingResult))
			{
				ShutdownOwnedServer();
				OutError = FString::Printf(TEXT("server_healthcheck_failed status=%d error=%s"), StartedPingResult.StatusCode, *StartedPingResult.Error);
				return false;
			}

			return true;
		}
	};

	struct FScopedUeAgentHttpServer
	{
		FString BaseUrl;
		FString Token;

		bool Initialize(FString& OutError)
		{
			FSharedUeAgentHttpServerState& SharedState = FSharedUeAgentHttpServerState::Get();
			if (!SharedState.EnsureInitialized(OutError))
			{
				return false;
			}

			BaseUrl = SharedState.BaseUrl;
			Token = SharedState.Token;
			return true;
		}
	};
}
