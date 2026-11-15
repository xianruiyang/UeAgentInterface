#include "NetworkTransport/UeAgentHttpTransportAdapter.h"

#include "NetworkTransport/UeAgentHttpFraming.h"
#include "Containers/Ticker.h"
#include "HAL/CriticalSection.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "IPAddress.h"
#include "Misc/ScopeLock.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

namespace UE::UAI::NetworkTransport
{
	namespace
	{
		// Admission bounds pending execution, not command parameters or payload size.
		constexpr int32 MaxQueuedRequests = 128;
		constexpr int32 IoChunkBytes = 64 * 1024;
		constexpr double StreamIdleSeconds = 15.0;

		struct FBoundRoute
		{
			FString InstanceId;
			FString Method;
			FString Path;
			FUeAgentHttpRequestHandler Handler;
			FUeAgentHttpImmediateHandler Immediate;
			FCriticalSection ImmediateLifetime;
			EUeAgentHttpRouteExecution Execution = EUeAgentHttpRouteExecution::GameThread;
			std::atomic<bool> Active{true};
		};

		struct FExchange
		{
			enum class EState : uint8 { Queued, Executing, Completed, Disconnected };
			FUeAgentHttpRequest Request;
			TSharedPtr<FBoundRoute, ESPMode::ThreadSafe> Route;
			std::atomic<EState> State{EState::Queued};
			FCriticalSection ResponseMutex;
			TOptional<FUeAgentHttpResponse> Response;
			bool bCompleted = false;
			bool bImmediate = false;
			double AdmittedAt = FPlatformTime::Seconds();
			std::atomic<double> ExecutionStartedAt{0};
			std::atomic<double> CompletedAt{0};

			bool Begin()
			{
				EState Expected = EState::Queued;
				if (!State.compare_exchange_strong(Expected, EState::Executing)) return false;
				ExecutionStartedAt = FPlatformTime::Seconds();
				return true;
			}
			void Complete(FUeAgentHttpResponse Result)
			{
				FScopeLock Lock(&ResponseMutex);
				if (bCompleted || State.load() == EState::Disconnected) return;
				Response.Emplace(MoveTemp(Result));
				CompletedAt = FPlatformTime::Seconds();
				bCompleted = true;
				State.store(EState::Completed);
			}
			void Disconnect()
			{
				// Completion and disconnect share the same terminal-state lock.
				FScopeLock Lock(&ResponseMutex);
				State.store(EState::Disconnected);
				Response.Reset();
			}
			bool TakeResponse(FUeAgentHttpResponse& Out)
			{
				FScopeLock Lock(&ResponseMutex);
				if (!Response.IsSet()) return false;
				Out = MoveTemp(Response.GetValue());
				Response.Reset();
				return true;
			}
		};

		struct FConnection
		{
			FSocket* Socket = nullptr;
			FHttpRequestReader Reader;
			TSharedPtr<FExchange, ESPMode::ThreadSafe> Exchange;
			FUeAgentHttpResponse Response;
			TArray<uint8> ResponseHead;
			int32 HeadOffset = 0;
			int32 BodyOffset = 0;
			double LastProgress = FPlatformTime::Seconds();
			bool bWriting = false;
			bool bClosed = false;
			double AcceptedAt = FPlatformTime::Seconds();
			double ResponseSetupAt = 0;
			double ResponsePreparedAt = 0;

			void SetResponse(FUeAgentHttpResponse Result)
			{
				ResponseSetupAt = FPlatformTime::Seconds();
				Response = MoveTemp(Result);
				ResponseHead = MakeHttpResponseHead(Response);
				ResponsePreparedAt = FPlatformTime::Seconds();
				bWriting = true;
				LastProgress = FPlatformTime::Seconds();
			}
			void CompleteWrite()
			{
				bClosed = true;
				// TakeResponse synchronized completion before SetResponse. These
				// exclusive phases partition the route lifetime; handler time may
				// contain the HJSON timer and must not be added to it again.
				const double Executed = Exchange ? Exchange->ExecutionStartedAt.load() : 0;
				const double Completed = Exchange ? Exchange->CompletedAt.load() : 0;
				if (Executed > 0 && Completed > 0 && Exchange->Route->Execution == EUeAgentHttpRouteExecution::GameThread)
					UE_LOG(LogTemp, Display, TEXT("UAI transport timing phase=http_game_thread_exchange ingress_s=%.6f queue_s=%.6f handler_s=%.6f response_wait_s=%.6f frame_s=%.6f write_s=%.6f execution=%s"),
						Exchange->AdmittedAt - AcceptedAt, Executed - Exchange->AdmittedAt,
						Completed - Executed, ResponseSetupAt - Completed,
						ResponsePreparedAt - ResponseSetupAt, FPlatformTime::Seconds() - ResponsePreparedAt,
						Exchange->bImmediate ? TEXT("network_control") : TEXT("game_thread"));
			}
		};

		void ExecuteExchange(const TSharedRef<FExchange, ESPMode::ThreadSafe>& Exchange)
		{
			if (!Exchange->Route->Active.load())
			{
				Exchange->Complete(FUeAgentHttpResponse::Failure(503, TEXT("transport_stopping"), TEXT("The route stopped before this request executed.")));
				return;
			}
			if (!Exchange->Begin()) return;
			FUeAgentHttpResponse Response = Exchange->Route->Handler(Exchange->Request);
			Exchange->Request.Body.Empty();
			Exchange->Complete(MoveTemp(Response));
		}
	}

	struct FUeAgentSocketHttpRouteHost::FImpl final
		: FRunnable, TSharedFromThis<FImpl, ESPMode::ThreadSafe>
	{
		explicit FImpl(uint32 InPort) : Port(InPort) {}
		~FImpl() { check(!Thread && !ListenSocket); }

		bool Bind(const FString& InstanceId, const FString& Method, const FString& Path,
			FUeAgentHttpRequestHandler Handler, FUeAgentTransportDiagnostic& Diagnostic, EUeAgentHttpRouteExecution Execution,
			FUeAgentHttpImmediateHandler Immediate = {})
		{
			check(IsInGameThread());
			if (!IsSupportedHttpMethod(Method) || !Path.StartsWith(TEXT("/")) || !Handler)
			{
				Diagnostic = {TEXT("http_route_invalid"), TEXT("A supported method, absolute route path and handler are required.")};
				return false;
			}
			{
				FScopeLock Lock(&RoutesMutex);
				for (const auto& Entry : Routes)
					if (Entry.Key == InstanceId || Entry.Value->Path == Path)
					{
						Diagnostic = {TEXT("http_route_already_bound"), TEXT("This route instance or path is already bound.")};
						return false;
					}
				const auto Route = MakeShared<FBoundRoute, ESPMode::ThreadSafe>();
				Route->InstanceId = InstanceId;
				Route->Method = Method.ToUpper();
				Route->Path = Path;
				Route->Handler = MoveTemp(Handler);
				Route->Immediate = MoveTemp(Immediate);
				Route->Execution = Execution;
				Routes.Add(InstanceId, Route);
			}
			if (Thread) return true;
			if (StartNetwork(Diagnostic)) return true;
			FScopeLock Lock(&RoutesMutex);
			Routes.Remove(InstanceId);
			return false;
		}

		bool Unbind(const FString& InstanceId, FUeAgentTransportDiagnostic& Diagnostic)
		{
			check(IsInGameThread());
			bool bLastRoute;
			TSharedPtr<FBoundRoute, ESPMode::ThreadSafe> RemovedRoute;
			{
				FScopeLock Lock(&RoutesMutex);
				auto* Route = Routes.Find(InstanceId);
				if (!Route)
				{
					Diagnostic = {TEXT("transport_server_instance_mismatch"), TEXT("Route host refuses to unbind a different instance.")};
					return false;
				}
				(*Route)->Active.store(false);
				RemovedRoute = *Route;
				Routes.Remove(InstanceId);
				bLastRoute = Routes.IsEmpty();
			}
			// A non-last unbind also drains its immediate callbacks before their
			// adapter/dispatch owners can be destroyed. Never wait with RoutesMutex.
			if (RemovedRoute->Immediate) { FScopeLock Drain(&RemovedRoute->ImmediateLifetime); }
			if (bLastRoute) StopNetwork();
			return true;
		}

		bool IsBound(const FString& InstanceId) const
		{
			FScopeLock Lock(&RoutesMutex);
			return bListening.load() && Routes.Contains(InstanceId);
		}

		void Shutdown()
		{
			{
				FScopeLock Lock(&RoutesMutex);
				for (const auto& Entry : Routes) Entry.Value->Active.store(false);
				Routes.Empty();
			}
			StopNetwork();
		}

		bool StartNetwork(FUeAgentTransportDiagnostic& Diagnostic)
		{
			Subsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
			if (!Subsystem || Port == 0 || Port > 65535)
			{
				Diagnostic = {TEXT("http_listener_unavailable"), TEXT("Socket subsystem or configured TCP port is unavailable.")};
				return false;
			}
			ListenSocket = Subsystem->CreateSocket(NAME_Stream, TEXT("UAI HTTP ingress"), false);
			const auto Address = Subsystem->CreateInternetAddr();
			bool bAddressValid = false;
			Address->SetIp(TEXT("127.0.0.1"), bAddressValid);
			Address->SetPort(static_cast<int32>(Port));
			if (!ListenSocket || !bAddressValid || !ListenSocket->SetNonBlocking(true)
				|| !ListenSocket->Bind(*Address) || !ListenSocket->Listen(MaxQueuedRequests))
			{
				Diagnostic = {TEXT("http_listener_start_failed"), FString::Printf(TEXT("Could not bind/listen on the configured loopback port (socket error %d)."), static_cast<int32>(Subsystem->GetLastErrorCode()))};
				DestroySocket(ListenSocket);
				return false;
			}
			bStopping.store(false);
			bListening.store(true);
			const TWeakPtr<FImpl, ESPMode::ThreadSafe> Weak = AsShared();
			Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Weak](float)
			{
				const auto Self = Weak.Pin();
				return Self && Self->DispatchNext();
			}));
			Thread = FRunnableThread::Create(this, TEXT("UAI HTTP ingress"));
			if (!Thread)
			{
				FTSTicker::GetCoreTicker().RemoveTicker(Ticker);
				Ticker.Reset();
				bListening.store(false);
				DestroySocket(ListenSocket);
				Diagnostic = {TEXT("http_listener_thread_failed"), TEXT("Could not start the UAI network ingress thread.")};
				return false;
			}
			return true;
		}

		void StopNetwork()
		{
			TArray<TSharedPtr<FExchange, ESPMode::ThreadSafe>> Pending;
			{
				FScopeLock Lock(&QueueMutex);
				bStopping.store(true);
				Pending = MoveTemp(Queue);
			}
			if (Ticker.IsValid())
			{
				FTSTicker::GetCoreTicker().RemoveTicker(Ticker);
				Ticker.Reset();
			}
			for (const auto& Exchange : Pending)
			{
				Exchange->Complete(FUeAgentHttpResponse::Failure(503, TEXT("transport_stopping"), TEXT("The transport stopped before this queued request executed.")));
			}
			if (Thread)
			{
				Thread->WaitForCompletion();
				delete Thread;
				Thread = nullptr;
			}
		}

		bool DispatchNext()
		{
			check(IsInGameThread());
			if (bStopping.load()) return false;
			if (bDispatching) return true;
			TGuardValue<bool> DispatchGuard(bDispatching, true);
			TSharedPtr<FExchange, ESPMode::ThreadSafe> Pending;
			for (;;)
			{
				{
					FScopeLock Lock(&QueueMutex);
					if (bStopping.load()) return false;
					if (Queue.IsEmpty()) break;
					Pending = MoveTemp(Queue[0]);
					Queue.RemoveAt(0);
				}
				if (Pending->State.load() == FExchange::EState::Disconnected) continue;
				ExecuteExchange(Pending.ToSharedRef());
				break; // Let the Editor and operation tickers progress between commands.
			}
			return !bStopping.load();
		}

		void Dispatch(FConnection& Connection)
		{
			TSharedPtr<FBoundRoute, ESPMode::ThreadSafe> Route;
			{
				FScopeLock Lock(&RoutesMutex);
				for (const auto& Entry : Routes)
					if (Entry.Value->Path == Connection.Reader.Request.RoutePath) { Route = Entry.Value; break; }
			}
			if (!Route)
			{
				Connection.SetResponse(FUeAgentHttpResponse::Failure(404, TEXT("transport_route_mismatch"), TEXT("No owned route matches the request path.")));
				return;
			}
			if (Route->Method != Connection.Reader.Request.Method)
			{
				Connection.SetResponse(FUeAgentHttpResponse::Failure(405, TEXT("transport_method_mismatch"), TEXT("Request method does not match the bound route.")));
				return;
			}
			const auto Exchange = MakeShared<FExchange, ESPMode::ThreadSafe>();
			Exchange->Request = MoveTemp(Connection.Reader.Request);
			Exchange->Route = Route;
			Connection.Exchange = Exchange;
			if (!bStopping.load() && Route->Immediate) {
				FScopeLock Lifetime(&Route->ImmediateLifetime);
				if (Route->Active.load()) {
					const double Started = FPlatformTime::Seconds();
					auto Immediate = Route->Immediate(Exchange->Request);
					if (Immediate.IsSet()) {
						Exchange->Begin(); Exchange->ExecutionStartedAt = Started; Exchange->bImmediate = true;
						Exchange->Complete(MoveTemp(Immediate.GetValue()));
						return;
					}
				}
			}
			if (bStopping.load())
				Exchange->Complete(FUeAgentHttpResponse::Failure(503, TEXT("transport_stopping"), TEXT("The transport is stopping; this request was not executed.")));
			else if (Route->Execution == EUeAgentHttpRouteExecution::NetworkThread)
				ExecuteExchange(Exchange);
			else
			{
				FScopeLock Lock(&QueueMutex);
				if (bStopping.load())
					Exchange->Complete(FUeAgentHttpResponse::Failure(503, TEXT("transport_stopping"), TEXT("The transport is stopping; this request was not executed.")));
				else if (Queue.Num() >= MaxQueuedRequests)
					Exchange->Complete(FUeAgentHttpResponse::Failure(503, TEXT("transport_backpressured"), TEXT("The game-thread request queue is full; this request was not executed. Retry after pending requests finish.")));
				else Queue.Add(Exchange);
			}
		}

		void AcceptConnections()
		{
			// This is an I/O fairness batch, not a rejection or per-frame limit.
			for (int32 Accepted = 0; Accepted < 64 && !bStopping.load(); ++Accepted)
			{
				bool bPending = false;
				if (!ListenSocket->HasPendingConnection(bPending) || !bPending) break;
				const auto Peer = Subsystem->CreateInternetAddr();
				FSocket* Socket = ListenSocket->Accept(*Peer, TEXT("UAI HTTP exchange"));
				if (!Socket) break;
				if (!Socket->SetNonBlocking(true)) { DestroySocket(Socket); continue; }
				Socket->SetNoDelay(true);
				auto Connection = MakeUnique<FConnection>();
				Connection->Socket = Socket;
				Connection->Reader.Request.PeerId = Peer->ToString(true);
				Connections.Add(MoveTemp(Connection));
			}
		}

		bool IsWouldBlock() const
		{
			const ESocketErrors Error = Subsystem->GetLastErrorCode();
			return Error == SE_EWOULDBLOCK || Error == SE_EINTR;
		}

		void Read(FConnection& Connection)
		{
			uint8 Buffer[IoChunkBytes];
			int32 Received = 0;
			if (!Connection.Socket->Recv(Buffer, sizeof(Buffer), Received))
			{
				// FSocket normalizes a stream would-block to true with zero bytes;
				// false includes graceful EOF and must not consult stale OS errno.
				Disconnect(Connection);
				return;
			}
			if (Received == 0) return;
			Connection.LastProgress = FPlatformTime::Seconds();
			FUeAgentHttpResponse Failure;
			if (!Connection.Reader.Consume(Buffer, Received, Failure)) Connection.SetResponse(MoveTemp(Failure));
			else if (Connection.Reader.IsComplete()) Dispatch(Connection);
		}

		void ObservePeer(FConnection& Connection)
		{
			uint8 Byte;
			int32 Received = 0;
			if (!Connection.Socket->Recv(&Byte, 1, Received, ESocketReceiveFlags::Peek)) Disconnect(Connection);
		}

		void Write(FConnection& Connection)
		{
			const bool bHeader = Connection.HeadOffset < Connection.ResponseHead.Num();
			const TArray<uint8>& Bytes = bHeader ? Connection.ResponseHead : Connection.Response.Body;
			int32& Offset = bHeader ? Connection.HeadOffset : Connection.BodyOffset;
			if (Offset == Bytes.Num()) { Connection.CompleteWrite(); return; }
			int32 Sent = 0;
			if (!Connection.Socket->Send(Bytes.GetData() + Offset, FMath::Min(IoChunkBytes, Bytes.Num() - Offset), Sent))
			{
				if (!IsWouldBlock()) Connection.bClosed = true;
				return;
			}
			Offset += Sent;
			if (Sent > 0) Connection.LastProgress = FPlatformTime::Seconds();
			if (Connection.HeadOffset == Connection.ResponseHead.Num() && Connection.BodyOffset == Connection.Response.Body.Num())
				Connection.CompleteWrite();
		}

		virtual uint32 Run() override
		{
			double StopDeadline = 0;
			for (;;)
			{
				const bool bStop = bStopping.load();
				if (!bStop) AcceptConnections();
				else if (StopDeadline == 0)
				{
					bListening.store(false);
					DestroySocket(ListenSocket);
					StopDeadline = FPlatformTime::Seconds() + 2.0;
				}
				for (auto& Owned : Connections)
				{
					FConnection& Connection = *Owned;
					if (bStop && !Connection.bWriting)
					{
						if (Connection.Exchange)
							Connection.Exchange->Complete(FUeAgentHttpResponse::Failure(503, TEXT("transport_stopping"),
								Connection.Exchange->State.load() == FExchange::EState::Queued
								? TEXT("The transport stopped before this request executed.")
								: TEXT("The transport stopped before the reply completed; an already executing command may have taken effect.")));
						else Connection.SetResponse(FUeAgentHttpResponse::Failure(503, TEXT("transport_stopping"), TEXT("The transport is stopping; this request was not executed.")));
					}
					if (Connection.Exchange && !Connection.bWriting)
					{
						ObservePeer(Connection);
						FUeAgentHttpResponse Response;
						if (Connection.Exchange->TakeResponse(Response)) Connection.SetResponse(MoveTemp(Response));
					}
					if (!Connection.bClosed)
					{
						if (Connection.bWriting) Write(Connection);
						else if (!Connection.Exchange) Read(Connection);
					}
					if (!Connection.bClosed && !Connection.Exchange && !Connection.bWriting
						&& FPlatformTime::Seconds() - Connection.LastProgress > StreamIdleSeconds)
						Connection.SetResponse(FUeAgentHttpResponse::Failure(408, TEXT("http_request_read_timeout"), TEXT("The HTTP request stopped sending bytes before its frame completed.")));
					if (Connection.bWriting && FPlatformTime::Seconds() - Connection.LastProgress > StreamIdleSeconds) Connection.bClosed = true;
				}
				for (int32 Index = Connections.Num() - 1; Index >= 0; --Index)
					if (Connections[Index]->bClosed) { Close(*Connections[Index]); Connections.RemoveAt(Index); }
				if (bStop && (Connections.IsEmpty() || FPlatformTime::Seconds() >= StopDeadline)) break;
				FPlatformProcess::SleepNoStats(0.001f);
			}
			for (auto& Connection : Connections) Close(*Connection);
			Connections.Empty();
			DestroySocket(ListenSocket);
			bListening.store(false);
			return 0;
		}

		virtual void Stop() override { bStopping.store(true); }
		void DestroySocket(FSocket*& Socket)
		{
			if (Socket) { Socket->Close(); Subsystem->DestroySocket(Socket); Socket = nullptr; }
		}
		void Disconnect(FConnection& Connection)
		{
			Connection.bClosed = true;
			if (Connection.Exchange)
			{
				FScopeLock Lock(&QueueMutex);
				// Remove both the request and its capacity reservation immediately,
				// including while the GameThread is occupied by a different command.
				Queue.RemoveSingle(Connection.Exchange);
				Connection.Exchange->Disconnect();
			}
		}
		void Close(FConnection& Connection)
		{
			Disconnect(Connection);
			DestroySocket(Connection.Socket);
		}

		uint32 Port;
		ISocketSubsystem* Subsystem = nullptr;
		FSocket* ListenSocket = nullptr;
		FRunnableThread* Thread = nullptr;
		FTSTicker::FDelegateHandle Ticker;
		mutable FCriticalSection RoutesMutex;
		TMap<FString, TSharedPtr<FBoundRoute, ESPMode::ThreadSafe>> Routes;
		FCriticalSection QueueMutex;
		TArray<TSharedPtr<FExchange, ESPMode::ThreadSafe>> Queue;
		std::atomic<bool> bStopping{false};
		std::atomic<bool> bListening{false};
		bool bDispatching = false;
		TArray<TUniquePtr<FConnection>> Connections;
	};

	FUeAgentSocketHttpRouteHost::FUeAgentSocketHttpRouteHost(uint32 Port)
		: Impl(MakeShared<FImpl, ESPMode::ThreadSafe>(Port)) {}
	FUeAgentSocketHttpRouteHost::~FUeAgentSocketHttpRouteHost() { Impl->Shutdown(); }
	bool FUeAgentSocketHttpRouteHost::Bind(const FString& InstanceId, const FString& Method, const FString& Path,
		FUeAgentHttpRequestHandler Handler, FUeAgentTransportDiagnostic& Diagnostic, EUeAgentHttpRouteExecution Execution)
	{
		return Impl->Bind(InstanceId, Method, Path, MoveTemp(Handler), Diagnostic, Execution);
	}
	bool FUeAgentSocketHttpRouteHost::Unbind(const FString& InstanceId, FUeAgentTransportDiagnostic& Diagnostic)
	{
		return Impl->Unbind(InstanceId, Diagnostic);
	}
	bool FUeAgentSocketHttpRouteHost::BindWithImmediate(const FString& InstanceId, const FString& Method, const FString& Path,
		FUeAgentHttpRequestHandler Handler, FUeAgentHttpImmediateHandler Immediate, FUeAgentTransportDiagnostic& Diagnostic)
	{
		return Impl->Bind(InstanceId, Method, Path, MoveTemp(Handler), Diagnostic,
			EUeAgentHttpRouteExecution::GameThread, MoveTemp(Immediate));
	}
	bool FUeAgentSocketHttpRouteHost::IsBound(const FString& InstanceId) const { return Impl->IsBound(InstanceId); }
}
