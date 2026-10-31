#include "Composition/SimpleAuthoring/OperationHost/UeAgentSimpleAuthoringOperationHost.h"

#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringRuntime.h"
#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"
#include "Core/Projection/UeAgentProjection.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::Composition::SimpleAuthoring::OperationHost
{
	using namespace BindingGateway::Ticket;
	using namespace BindingGateway::Operation;
	using namespace CommandEdge;
	using namespace ContextResource;
	using namespace Projection;

	namespace
	{
		FString Sha256(const FString& Value)
		{
			const FTCHARToUTF8 Utf8(*Value);
			FSHA256Signature Signature{};
			const uint8 Empty = 0;
			const uint8* Data = Utf8.Length() > 0
				? reinterpret_cast<const uint8*>(Utf8.Get())
				: &Empty;
			if (!SHA256(
					Data, static_cast<size_t>(Utf8.Length()), Signature.Signature))
			{
				return FString();
			}
			return TEXT("sha256:") + Signature.ToString().ToLower();
		}

		Contract::FDiagnostic HostFailure(
			const FString& Code, const FString& Reason, const FString& Stage)
		{
			auto Diagnostic = Contract::FDiagnostic::Failure(Code, Reason, Stage);
			Diagnostic.SourceKind = TEXT("native_result");
			return Diagnostic;
		}

		void ReadFailure(const FUeAgentCommandResult& Result, Contract::FDiagnostic& OutDiagnostic)
		{
			OutDiagnostic = Result.GetDiagnostic();
			for (const Contract::FDiagnostic& Additional : Result.GetDiagnostics())
				OutDiagnostic.Append(Additional);
			if (OutDiagnostic.IsEmpty())
				OutDiagnostic = HostFailure(TEXT("simple_authoring_operation_failed"),
					TEXT("The delegated authoring handler returned failure without a diagnostic."), TEXT("delegated_execute"));
		}

		Contract::FDiagnostic MissingCompletedResult(const FString& Stage, const FString& CommandId)
		{
			auto Diagnostic = HostFailure(TEXT("simple_authoring_operation_result_missing"),
				TEXT("The authoring handler has not supplied a completed result."), Stage);
			Diagnostic.CandidateName = CommandId;
			Diagnostic.PropertyPath = TEXT("result");
			Diagnostic.ActualValuePresent = false;
			Diagnostic.ExpectedValuePresent = true;
			return Diagnostic;
		}

		bool TryReadCompletedResult(
			const FUeAgentCommandResult& Result,
			FString& OutOperationId,
			Contract::FDiagnostic& OutError, bool* OutEquivalent = nullptr)
		{
			OutOperationId.Reset();
			OutError.Reset();
			if (OutEquivalent) *OutEquivalent = false;
			if (!Result.IsOk())
			{
				ReadFailure(Result, OutError);
				return false;
			}
			const auto FailField = [&](const FString& Path, const TSharedPtr<FJsonValue>& Value,
				const FString& Expected)
			{
				OutOperationId.Reset();
				OutError = HostFailure(TEXT("simple_authoring_completed_result_invalid"),
					FString::Printf(TEXT("Completed result field '%s' must be %s."), *Path, *Expected), TEXT("result_decode"));
				OutError.Source.JsonPath = Path;
				OutError.PropertyPath = Path;
				OutError.ActualValuePresent = Value.IsValid();
				OutError.ExpectedValuePresent = true;
				OutError.ExpectedValue = MakeShared<FJsonValueString>(Expected);
				if (Value.IsValid())
				{
					if (Value->Type == EJson::Object || Value->Type == EJson::Array)
						OutError.ActualValue = MakeShared<FJsonValueString>(
							Value->Type == EJson::Object ? TEXT("object") : TEXT("array"));
					else OutError.ActualValue = Value;
				}
				for (const Contract::FDiagnostic& Additional : Result.GetDiagnostics())
					OutError.Append(Additional);
				return false;
			};
			TSharedPtr<FJsonObject> Root;
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(Result.GetPayloadJson());
			if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
			{
				OutError = HostFailure(TEXT("simple_authoring_completed_result_invalid"),
					Reader->GetErrorMessage().IsEmpty()
						? TEXT("The completed result must contain a JSON object document.")
						: Reader->GetErrorMessage(), TEXT("result_decode"));
				OutError.Source.JsonPath = TEXT("/");
				OutError.Source.Line = Reader->GetLineNumber();
				OutError.Source.Column = Reader->GetCharacterNumber();
				for (const Contract::FDiagnostic& Additional : Result.GetDiagnostics())
					OutError.Append(Additional);
				return false;
			}
			if (!Root->TryGetStringField(TEXT("operation_id"), OutOperationId) || OutOperationId.IsEmpty())
				return FailField(TEXT("/operation_id"), Root->Values.FindRef(TEXT("operation_id")), TEXT("a non-empty string"));
			const TSharedPtr<FJsonObject>* Projection = nullptr;
			if (!Root->TryGetObjectField(TEXT("projection"), Projection) || !Projection || !Projection->IsValid())
				return FailField(TEXT("/projection"), Root->Values.FindRef(TEXT("projection")), TEXT("an object"));
			const TSharedPtr<FJsonObject>* Payload = nullptr;
			if (!(*Projection)->TryGetObjectField(TEXT("payload"), Payload) || !Payload || !Payload->IsValid())
				return FailField(TEXT("/projection/payload"), (*Projection)->Values.FindRef(TEXT("payload")), TEXT("an object"));
			FString Status;
			if (!(*Payload)->TryGetStringField(TEXT("status"), Status))
				return FailField(TEXT("/projection/payload/status"), (*Payload)->Values.FindRef(TEXT("status")), TEXT("a string"));
			FString EffectOutcome;
			if (!(*Payload)->TryGetStringField(TEXT("effect_outcome"), EffectOutcome))
				return FailField(TEXT("/projection/payload/effect_outcome"), (*Payload)->Values.FindRef(TEXT("effect_outcome")), TEXT("a string"));
			if (OutEquivalent) *OutEquivalent = Status == TEXT("verified") && EffectOutcome == TEXT("complete");
			return true;
		}

		bool MutationMayHaveStarted(const UE::UAI::Contract::FDiagnostic& Diagnostic)
		{
			return Diagnostic.MutationMayHaveStarted.Get(false)
				|| Diagnostic.MutationStarted.Get(false)
				|| Diagnostic.RollbackSucceeded.Get(false)
				|| Diagnostic.TerminalStatus == TEXT("rolled_back")
				|| Diagnostic.TerminalStatus == TEXT("state_unknown")
				|| Diagnostic.ExecutionStatus == TEXT("rolled_back")
				|| Diagnostic.ExecutionStatus == TEXT("state_unknown")
				|| Diagnostic.TransactionStatus == TEXT("rolled_back")
				|| Diagnostic.TransactionStatus == TEXT("state_unknown");
		}


		class FSimpleAuthoringOperationInvocationState final
			: public IOperationVersionFencePort
			, public IOperationDomainMutationPort
			, public IOperationStabilityPort
			, public IOperationPersistencePort
			, public IOperationIndependentVerificationPort
			, public DomainCapability::IDomainOperationInvocationContext
		{
		public:
			FSimpleAuthoringOperationInvocationState(
				TSharedRef<IUeAgentCommandHandler> InHandler,
				FUeAgentCommandRequest InRequest,
				Contract::FUeAgentCallerContext InCaller,
				TSharedRef<Contract::IUeAgentCancellationSignal> InCancellation)
				: Handler(MoveTemp(InHandler))
				, Request(MoveTemp(InRequest))
				, Caller(MoveTemp(InCaller))
				, Cancellation(MoveTemp(InCancellation))
			{
			}

			bool Check(
				const FOperationLifecyclePlan& Plan,
				const EOperationLifecycleStage Stage,
				bool& bOutCurrent,
				FOperationLifecycleError& OutError) override
			{
				bOutCurrent = false;
				OutError = {};
				if (Cancellation->IsCancelled())
				{
					OutError = HostFailure(TEXT("operation_cancelled"),
						TEXT("The authoring command was cancelled before the lifecycle fence completed."), TEXT("operation_lifecycle"));
					OutError.CandidateName = Request.GetCommandId();
					OutError.PropertyPath = TEXT("cancellation");
					OutError.ActualValue = MakeShared<FJsonValueBoolean>(true);
					return false;
				}
				const FString Scope = Plan.GetImpactScope().IsEmpty()
					? FString() : Plan.GetImpactScope()[0];
				const FString* Expected = Plan.GetExpectedRevisions().Find(Scope);
				if (!Expected || *Expected != Request.GetCommandBindingHash())
				{
					OutError = HostFailure(TEXT("operation_version_conflict"),
						Expected ? TEXT("The command binding differs from the revision recorded in the operation plan.")
							: TEXT("The operation plan has no expected revision for its impact scope."), TEXT("operation_lifecycle"));
					OutError.CandidateName = Request.GetCommandId();
					OutError.PropertyPath = TEXT("command_binding");
					OutError.ActualValuePresent = true;
					OutError.ExpectedValuePresent = Expected != nullptr;
					OutError.ActualEvidence = MakeShared<FJsonValueString>(Request.GetCommandBindingHash());
					if (Expected) OutError.ExpectedEvidence = MakeShared<FJsonValueString>(*Expected);
					OutError.SubjectId = Scope;
					return false;
				}
				if (Stage == EOperationLifecycleStage::PrePersistFence)
				{
					if (!Result.IsSet())
					{
						OutError = MissingCompletedResult(TEXT("pre_persist_fence"), Request.GetCommandId());
						return false;
					}
					if (!TryReadCompletedResult(Result.GetValue(), OperationId, FailureDiagnostic))
					{
						OutError = FailureDiagnostic;
						return false;
					}
				}
				bOutCurrent = true;
				return true;
			}

			bool Execute(
				const FOperationLifecyclePlan& Plan,
				FOperationMutationFacts& OutFacts,
				FOperationLifecycleError& OutError) override
			{
				OutFacts = {};
				OutError = {};
				Result = Handler->Submit(Request, Caller, Cancellation);
				if (!Result->IsOk())
				{
					ReadFailure(
						Result.GetValue(), FailureDiagnostic);
					if (MutationMayHaveStarted(FailureDiagnostic))
					{
						OutFacts = FOperationMutationFacts(
							TEXT("delegated-failure:") + Plan.GetPlanId(),
							Plan.GetImpactScope(),
							true);
					}
					OutError = FailureDiagnostic;
					return false;
				}
				if (!TryReadCompletedResult(
						Result.GetValue(), OperationId, FailureDiagnostic))
				{
					OutError = FailureDiagnostic;
					return false;
				}
				OutFacts = FOperationMutationFacts(
					OperationId, Plan.GetImpactScope(), false);
				return true;
			}

			bool Rollback(
				const FOperationLifecyclePlan&,
				const FOperationMutationFacts&,
				bool& bOutConfirmed,
				FOperationLifecycleError& OutError) override
			{
				bOutConfirmed = FailureDiagnostic.RollbackSucceeded.Get(false) || FailureDiagnostic.TerminalStatus == TEXT("rolled_back") || FailureDiagnostic.ExecutionStatus == TEXT("rolled_back") || FailureDiagnostic.TransactionStatus == TEXT("rolled_back");
				OutError = bOutConfirmed ? FOperationLifecycleError() : FailureDiagnostic;
				if (!bOutConfirmed && OutError.IsEmpty())
				{
					OutError = HostFailure(TEXT("operation_rollback_unconfirmed"),
						TEXT("The delegated operation supplied no successful rollback confirmation."), TEXT("rollback"));
					OutError.CandidateName = Request.GetCommandId();
					OutError.PropertyPath = TEXT("rollback_succeeded");
					OutError.ActualValuePresent = false;
					OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				}
				return bOutConfirmed;
			}

			bool Settle(
				const FOperationLifecyclePlan&,
				const FOperationMutationFacts&,
				FOperationLifecycleError& OutError) override
			{
				OutError = {};
				FString VerifiedOperationId;
				if (!Result.IsSet())
				{
					OutError = MissingCompletedResult(TEXT("settle"), Request.GetCommandId());
					return false;
				}
				if (!TryReadCompletedResult(Result.GetValue(), VerifiedOperationId, FailureDiagnostic))
				{
					OutError = FailureDiagnostic;
					return false;
				}
				return true;
			}

			bool Persist(
				const FOperationLifecyclePlan&,
				const FOperationMutationFacts&,
				FString& OutPersistenceReceiptId,
				FOperationLifecycleError& OutError) override
			{
				OutError = {};
				OutPersistenceReceiptId = OperationId;
				if (OutPersistenceReceiptId.IsEmpty())
				{
					OutError = HostFailure(TEXT("simple_authoring_operation_identity_missing"),
						TEXT("The completed operation has no identity to use as its persistence receipt."), TEXT("persist"));
					OutError.PropertyPath = TEXT("operation_id");
					OutError.ActualValue = MakeShared<FJsonValueString>(OperationId);
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty operation identity"));
					return false;
				}
				return true;
			}

			bool Verify(
				const FOperationLifecyclePlan&,
				const FOperationMutationFacts&,
				const FString& PersistenceReceiptId,
				bool& bOutEquivalent,
				FOperationLifecycleError& OutError) override
			{
				bOutEquivalent = false;
				OutError = {};
				FString VerifiedOperationId;
				if (PersistenceReceiptId != OperationId)
				{
					OutError = HostFailure(TEXT("operation_readback_mismatch"),
						TEXT("The persistence receipt belongs to a different operation."), TEXT("verify"));
					OutError.PropertyPath = TEXT("persistence_receipt_id");
					OutError.ActualEvidence = MakeShared<FJsonValueString>(PersistenceReceiptId);
					OutError.ExpectedEvidence = MakeShared<FJsonValueString>(OperationId);
					return false;
				}
				if (!Result.IsSet())
				{
					OutError = MissingCompletedResult(TEXT("verify"), Request.GetCommandId());
					return false;
				}
				if (!TryReadCompletedResult(Result.GetValue(), VerifiedOperationId, FailureDiagnostic, &bOutEquivalent))
				{
					OutError = FailureDiagnostic;
					return false;
				}
				if (VerifiedOperationId != OperationId)
				{
					OutError = HostFailure(TEXT("operation_readback_mismatch"),
						TEXT("The read-back completed result belongs to a different operation."), TEXT("verify"));
					OutError.PropertyPath = TEXT("operation_id");
					OutError.ActualEvidence = MakeShared<FJsonValueString>(VerifiedOperationId);
					OutError.ExpectedEvidence = MakeShared<FJsonValueString>(OperationId);
					return false;
				}
				return true;
			}

			bool TryBuildSuccessPayload(
				const FOperationLifecycleReceipt& Receipt,
				FString& OutPayloadJson,
				UE::UAI::Contract::FDiagnostic& OutError) const override
			{
				OutPayloadJson.Reset();
				OutError.Reset();
				if (!Result.IsSet())
				{
					OutError = MissingCompletedResult(TEXT("result_projection"), Request.GetCommandId());
					return false;
				}
				if (!Result->IsOk())
				{
					ReadFailure(Result.GetValue(), OutError);
					return false;
				}
				if (Receipt.GetStatus() != EOperationLifecycleStatus::Succeeded)
				{
					OutError = Receipt.GetDiagnostic();
					if (OutError.IsEmpty())
					{
						OutError = HostFailure(TEXT("simple_authoring_operation_result_missing"),
							TEXT("A success payload cannot be projected from an unsuccessful lifecycle receipt."), TEXT("result_projection"));
						OutError.PropertyPath = TEXT("receipt/status");
						OutError.ActualValue = MakeShared<FJsonValueString>(LexToString(Receipt.GetStatus()));
						OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("succeeded"));
					}
					return false;
				}
				OutPayloadJson = Result->GetPayloadJson();
				if (OutPayloadJson.IsEmpty())
				{
					OutError = HostFailure(TEXT("simple_authoring_operation_result_missing"),
						TEXT("The successful authoring handler returned an empty payload."), TEXT("result_projection"));
					OutError.PropertyPath = TEXT("payload");
					OutError.ActualValue = MakeShared<FJsonValueString>(OutPayloadJson);
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty payload"));
					for (const Contract::FDiagnostic& Additional : Result->GetDiagnostics())
						OutError.Append(Additional);
					return false;
				}
				return true;
			}

			

		private:
			TSharedRef<IUeAgentCommandHandler> Handler;
			FUeAgentCommandRequest Request;
			Contract::FUeAgentCallerContext Caller;
			TSharedRef<Contract::IUeAgentCancellationSignal> Cancellation;
			TOptional<FUeAgentCommandResult> Result;
			FString OperationId;
			UE::UAI::Contract::FDiagnostic FailureDiagnostic;
		};

		class FSimpleAuthoringOperationInvocationFactory final
			: public DomainCapability::IDomainOperationInvocationFactory
		{
		public:
			explicit FSimpleAuthoringOperationInvocationFactory(FString InCommandId)
				: CommandId(MoveTemp(InCommandId))
			{
			}

			const FString& GetOperationId() const override { return CommandId; }

			bool TryCreate(
				const FUeAgentCommandRequest& Request,
				const Contract::FUeAgentCallerContext& Caller,
				const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation,
				DomainCapability::FDomainOperationInvocation& OutInvocation,
				DomainCapability::FDomainOperationInvocationError& OutError)
				const override
			{
				OutInvocation = {};
				OutError = {};
				if (Request.GetCommandId() != CommandId)
				{
					OutError = HostFailure(TEXT("simple_authoring_operation_identity_mismatch"),
						TEXT("The requested command does not match this operation factory."), TEXT("operation_lifecycle"));
					OutError.PropertyPath = TEXT("command_id");
					OutError.ActualValue = MakeShared<FJsonValueString>(Request.GetCommandId());
					OutError.ExpectedValue = MakeShared<FJsonValueString>(CommandId);
					return false;
				}
				TSharedPtr<IUeAgentCommandHandler> Handler;
				UE::UAI::Contract::FDiagnostic Error;
				if (!TryResolveHandler(CommandId, Handler, Error))
				{
					OutError = Error;
					return false;
				}
				const TSharedRef<FSimpleAuthoringOperationInvocationState> State =
					MakeShared<FSimpleAuthoringOperationInvocationState>(
						Handler.ToSharedRef(), Request, Caller, Cancellation);
				FString Target = CommandId;
				if (Request.GetParams().IsValid())
				{
					Request.GetParams()->TryGetStringField(TEXT("target_ref"), Target);
				}
				const FString Scope = TEXT("simple_authoring:") + CommandId
					+ TEXT(":") + Target;
				const FString Fingerprint = Sha256(
					CommandId + TEXT("\n")
					+ Request.GetCommandBindingHash() + TEXT("\n")
					+ Request.GetRequestId() + TEXT("\n")
					+ Request.GetParamsJson());
				const TSharedRef<const FOperationLifecyclePlan> Plan =
					MakeShared<const FOperationLifecyclePlan>(
						TEXT("simple-authoring:") + Request.GetRequestId(),
						Fingerprint,
						TArray<FString>{Scope},
						TMap<FString, FString>{{Scope,
							Request.GetCommandBindingHash()}},
						TArray<FString>{TEXT("simple_authoring_completed")});
				const TSharedRef<const FOperationImpactAuthorization> Authorization =
					MakeShared<const FOperationImpactAuthorization>(
						Fingerprint, TArray<FString>{Scope});
				const TSharedRef<FOperationLifecycleService> Service =
					MakeShared<FOperationLifecycleService>(
						StaticCastSharedRef<IOperationVersionFencePort>(State),
						StaticCastSharedRef<IOperationDomainMutationPort>(State),
						StaticCastSharedRef<IOperationStabilityPort>(State),
						StaticCastSharedRef<IOperationPersistencePort>(State),
						StaticCastSharedRef<
							IOperationIndependentVerificationPort>(State));
				return DomainCapability::FDomainOperationInvocation::TryCreate(
					Plan,
					Authorization,
					Service,
					StaticCastSharedRef<DomainCapability::
						IDomainOperationInvocationContext>(State),
					OutInvocation,
					OutError);
			}

		private:
			FString CommandId;
		};

		class FConservativeTokenEstimator final : public IUeAgentTokenEstimator
		{
		public:
			bool TryEstimate(
				const FUeAgentProjectedPayload& Payload,
				const TArray<FUeAgentArtifactDescriptor>& Artifacts,
				const TArray<FUeAgentProjectionDiagnostic>& Diagnostics,
				FUeAgentResourceTracker& Resources,
				const FUeAgentOperationDeadline& Deadline,
				const FUeAgentCancellationToken& Cancellation,
				FUeAgentTokenEstimate& OutEstimate,
				FUeAgentProjectionError& OutError) override
			{
				OutError = {};
				FUeAgentCancellationRequested Cancelled;
				if (!Cancellation.TryCheckpoint(Cancelled))
				{
					OutError = Cancelled;
					return false;
				}
				FUeAgentResourceChargeError ChargeError;
				if (!Resources.TryCharge(
						EUeAgentResourceDimension::WorkUnits,
						1,
						TEXT("simple_authoring.projection"),
						ChargeError))
				{
					OutError = ChargeError;
					return false;
				}
				int64 Utf8Bytes = 512 + Artifacts.Num() * 512
					+ Diagnostics.Num() * 256;
				if (Payload.GetSnapshot().IsValid())
				{
					Utf8Bytes += Payload.GetSnapshot()->GetEntries().Num() * 2048;
				}
				OutEstimate = FUeAgentTokenEstimate(
					EUeAgentTokenEstimateKind::Conservative,
					FMath::Max<int64>(1, (Utf8Bytes + 2) / 3),
					Utf8Bytes);
				return true;
			}
		};

		struct FProcessHost final
		{
			FCriticalSection Mutex;
			TSharedPtr<FUeAgentSimpleAuthoringRuntime> Runtime;
			TWeakObjectPtr<UWorld> RuntimeWorld;
			TMap<FString, TSharedRef<IUeAgentCommandHandler>> Handlers;
			TMap<FString, TSharedRef<Discovery::IUeAgentCatalogSource>>
				CatalogSourcesByKind;
			TMap<FString, TSharedRef<const DomainCapability::
				IDomainOperationInvocationFactory>> OperationFactories;
#if WITH_DEV_AUTOMATION_TESTS
			TWeakObjectPtr<UWorld> AutomationWorld;
			TMap<FString, TSharedRef<IUeAgentCommandHandler>>
				AutomationHandlerOverrides;
#endif
			FString ServerSessionId = TEXT("server-session:")
				+ FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
			FString TicketIntegrityKey = FGuid::NewGuid().ToString(
				EGuidFormats::DigitsWithHyphensLower);
		};

		FProcessHost& ProcessHost()
		{
			static FProcessHost Host;
			return Host;
		}

		void ResetRuntimeLocked(FProcessHost& Host)
		{
			Host.Handlers.Reset();
			Host.CatalogSourcesByKind.Reset();
			Host.Runtime.Reset();
			Host.RuntimeWorld.Reset();
		}

		UWorld* ResolveDesiredWorldLocked(FProcessHost& Host)
		{
#if WITH_DEV_AUTOMATION_TESTS
			if (UWorld* AutomationWorld = Host.AutomationWorld.Get())
			{
				return AutomationWorld;
			}
#endif
			return GEditor
				? GEditor->GetEditorWorldContext().World()
				: nullptr;
		}

		bool TryCreateRuntimeLocked(FProcessHost& Host, UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutError.Reset();
			if (!IsInGameThread())
			{
				OutError = HostFailure(TEXT("simple_authoring_game_thread_required"),
					TEXT("The authoring runtime must be created on the game thread."), TEXT("runtime_factory"));
				OutError.PropertyPath = TEXT("game_thread");
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				return false;
			}
			UWorld* World = ResolveDesiredWorldLocked(Host);
			if (Host.Runtime.IsValid()
				&& Host.RuntimeWorld.Get() == World
				&& World)
			{
				return true;
			}
			ResetRuntimeLocked(Host);
			if (!World)
			{
				OutError = HostFailure(TEXT("simple_authoring_editor_world_unavailable"),
					TEXT("No valid authoring world override or editor world is available."), TEXT("runtime_factory"));
				OutError.PropertyPath = TEXT("world");
				OutError.ActualValuePresent = false;
				OutError.ExpectedValuePresent = true;
				return false;
			}

			TSharedPtr<const FUeAgentOperationTicketService> TicketService;
			FUeAgentOperationTicketError TicketError;
			if (!FUeAgentOperationTicketService::TryCreate(
					Host.TicketIntegrityKey,
					300.0,
					[]() { return FPlatformTime::Seconds(); },
					[]() { return FGuid::NewGuid().ToString(EGuidFormats::Digits); },
					TicketService,
					TicketError))
			{
				OutError = TicketError;
				return false;
			}

			TSharedPtr<const FUeAgentResponseProjector> Projector;
			FUeAgentProjectionError ProjectionError;
			if (!FUeAgentResponseProjector::TryCreate(
					MakeShared<FConservativeTokenEstimator>(),
					Projector,
					ProjectionError))
			{
				OutError = ProjectionError;
				return false;
			}

			FUeAgentSimpleAuthoringRuntimeConfig Config;
			Config.ProjectInstanceId = TEXT("project:") + FString(FApp::GetProjectName());
			Config.EditorInstanceId = FString::Printf(
				TEXT("editor-process:%u"), FPlatformProcess::GetCurrentProcessId());
			Config.ServerSessionId = Host.ServerSessionId;
			Config.RegistryEpoch = 1;

			Config.JournalRoot = FPaths::ProjectSavedDir()
				/ TEXT("UeAgentInterface/SimpleAuthoring");
			FUeAgentSimpleAuthoringRuntimeError RuntimeError;
			if (!FUeAgentSimpleAuthoringRuntime::TryCreate(
					Config,
					*World,
					TicketService.ToSharedRef(),
					Projector.ToSharedRef(),
					Host.Runtime,
					RuntimeError))
			{
				ResetRuntimeLocked(Host);
				OutError = RuntimeError;
				return false;
			}

			for (const TSharedRef<IUeAgentCommandHandler>& Handler
				: Host.Runtime->GetCommandHandlers())
			{
				if (Host.Handlers.Contains(Handler->GetCommandId()))
				{
					OutError = HostFailure(TEXT("simple_authoring_handler_duplicate"),
						TEXT("More than one authoring handler is registered for this command."), TEXT("runtime_factory"));
					OutError.PropertyPath = TEXT("command_id");
					OutError.ActualValue = MakeShared<FJsonValueString>(Handler->GetCommandId());
					ResetRuntimeLocked(Host);
					return false;
				}
				Host.Handlers.Add(Handler->GetCommandId(), Handler);
			}
			for (const TSharedRef<Discovery::IUeAgentCatalogSource>& Source
				: Host.Runtime->GetCatalogSources())
			{
				for (const FString& Kind : Source->GetCatalogKinds())
				{
					if (Kind.IsEmpty()
						|| Host.CatalogSourcesByKind.Contains(Kind))
					{
						OutError = HostFailure(TEXT("simple_authoring_catalog_kind_duplicate"),
							Kind.IsEmpty() ? TEXT("An authoring catalog source registered an empty kind.")
								: TEXT("More than one authoring catalog source registered this kind."), TEXT("runtime_factory"));
						OutError.PropertyPath = TEXT("catalog_kind");
						OutError.ActualValue = MakeShared<FJsonValueString>(Kind);
						ResetRuntimeLocked(Host);
						return false;
					}
					Host.CatalogSourcesByKind.Add(Kind, Source);
				}
			}
			Host.RuntimeWorld = World;
			return true;
		}

		bool TryGetDurableService(
			TSharedPtr<IUeAgentSimpleAuthoringAssetDurableService,
				ESPMode::ThreadSafe>& OutService,
			AssetDurable::FAssetDurableError& OutError)
		{
			OutService.Reset();
			FProcessHost& Host = ProcessHost();
			FScopeLock Lock(&Host.Mutex);
			UE::UAI::Contract::FDiagnostic RuntimeError;
			if (!TryCreateRuntimeLocked(Host, RuntimeError))
			{
				OutError = RuntimeError;
				return false;
			}
			OutService = Host.Runtime->GetDurableService();
			return true;
		}
	}

	bool TryResolveHandler(
		const FString& CommandId,
		TSharedPtr<IUeAgentCommandHandler>& OutHandler,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandler.Reset();
		OutError.Reset();
		FProcessHost& Host = ProcessHost();
		FScopeLock Lock(&Host.Mutex);
#if WITH_DEV_AUTOMATION_TESTS
		if (const TSharedRef<IUeAgentCommandHandler>* Override =
				Host.AutomationHandlerOverrides.Find(CommandId))
		{
			OutHandler = *Override;
			return true;
		}
#endif
		if (!TryCreateRuntimeLocked(Host, OutError))
		{
			return false;
		}
		const TSharedRef<IUeAgentCommandHandler>* Found =
			Host.Handlers.Find(CommandId);
		if (!Found)
		{
			OutError = HostFailure(TEXT("simple_authoring_handler_missing"),
				TEXT("No authoring handler is registered for the requested command."), TEXT("runtime_factory"));
			OutError.PropertyPath = TEXT("command_id");
			OutError.ActualValue = MakeShared<FJsonValueString>(CommandId);
			return false;
		}
		OutHandler = *Found;
		return true;
	}

	bool TryResolveCatalogSource(
		const FString& CatalogKind,
		TSharedPtr<Discovery::IUeAgentCatalogSource>& OutSource,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutSource.Reset();
		OutError.Reset();
		FProcessHost& Host = ProcessHost();
		FScopeLock Lock(&Host.Mutex);
		if (!TryCreateRuntimeLocked(Host, OutError))
		{
			return false;
		}
		const TSharedRef<Discovery::IUeAgentCatalogSource>* Found =
			Host.CatalogSourcesByKind.Find(CatalogKind);
		if (!Found)
		{
			OutError = HostFailure(TEXT("simple_authoring_catalog_kind_missing"),
				TEXT("No authoring catalog source is registered for the requested kind."), TEXT("runtime_factory"));
			OutError.PropertyPath = TEXT("catalog_kind");
			OutError.ActualValue = MakeShared<FJsonValueString>(CatalogKind);
			return false;
		}
		OutSource = *Found;
		return true;
	}

	TSharedRef<const DomainCapability::IDomainOperationInvocationFactory>
	GetOperationInvocationFactory(const FString& CommandId)
	{
		FProcessHost& Host = ProcessHost();
		FScopeLock Lock(&Host.Mutex);
		if (const TSharedRef<const DomainCapability::
			IDomainOperationInvocationFactory>* Existing =
				Host.OperationFactories.Find(CommandId))
		{
			return *Existing;
		}
		const TSharedRef<const DomainCapability::
			IDomainOperationInvocationFactory> Factory =
			MakeShared<const FSimpleAuthoringOperationInvocationFactory>(CommandId);
		Host.OperationFactories.Add(CommandId, Factory);
		return Factory;
	}

	bool TryPrepareDurableJournal(
		const AssetDurable::FAssetDurableRequest& Request,
		ContextResource::FUeAgentResourceTracker& Resources,
		AssetDurable::FAssetDurableSessionPtr& OutSession,
		AssetDurable::FAssetDurableError& OutError)
	{
		TSharedPtr<IUeAgentSimpleAuthoringAssetDurableService, ESPMode::ThreadSafe> Service;
		return TryGetDurableService(Service, OutError)
			&& Service->TryPrepare(Request, Resources, OutSession, OutError);
	}

	bool TryInspectDurableJournal(
		const FString& JournalId,
		ContextResource::FUeAgentResourceTracker& Resources,
		AssetDurable::FAssetDurableSessionPtr& OutSession,
		TArray<AssetDurable::FDurableArtifactState>& OutObservedStates,
		AssetDurable::FAssetDurableError& OutError)
	{
		TSharedPtr<IUeAgentSimpleAuthoringAssetDurableService,
			ESPMode::ThreadSafe> DurableService;
		return TryGetDurableService(DurableService, OutError)
			&& DurableService->TryInspect(
				JournalId,
				Resources,
				OutSession,
				OutObservedStates,
				OutError);
	}

	bool TryReconcileDurableJournal(
		const FString& JournalId,
		const FString& ExpectedJournalOwnerId,
		const int64 ExpectedFencingToken,
		const FString& VerificationId,
		ContextResource::FUeAgentResourceTracker& Resources,
		AssetDurable::FDurableReceipt& OutReceipt,
		AssetDurable::FAssetDurableError& OutError)
	{
		TSharedPtr<IUeAgentSimpleAuthoringAssetDurableService,
			ESPMode::ThreadSafe> DurableService;
		OutReceipt = AssetDurable::FDurableReceipt();
		return TryGetDurableService(DurableService, OutError)
			&& DurableService->TryReconcile(
				JournalId,
				ExpectedJournalOwnerId,
				ExpectedFencingToken,
				VerificationId,
				Resources,
				OutReceipt,
				OutError);
	}

#if WITH_DEV_AUTOMATION_TESTS
	void SetAutomationWorldOverride(
		UWorld* World,
		const bool bInvalidateRuntime)
	{
		FProcessHost& Host = ProcessHost();
		FScopeLock Lock(&Host.Mutex);
		if (bInvalidateRuntime)
		{
			ResetRuntimeLocked(Host);
		}
		Host.AutomationWorld = World;
	}

	void SetAutomationHandlerOverride(
		const FString& CommandId,
		TSharedPtr<IUeAgentCommandHandler> Handler)
	{
		FProcessHost& Host = ProcessHost();
		FScopeLock Lock(&Host.Mutex);
		if (Handler.IsValid())
		{
			Host.AutomationHandlerOverrides.Add(
				CommandId, Handler.ToSharedRef());
		}
		else
		{
			Host.AutomationHandlerOverrides.Remove(CommandId);
		}
	}
#endif
}
