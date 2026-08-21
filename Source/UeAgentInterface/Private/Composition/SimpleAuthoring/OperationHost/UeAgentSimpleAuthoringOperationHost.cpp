#include "Composition/SimpleAuthoring/OperationHost/UeAgentSimpleAuthoringOperationHost.h"

#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringRuntime.h"
#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"
#include "Core/Projection/UeAgentProjection.h"
#include "Dom/JsonObject.h"
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

		bool TryReadVerifiedResult(
			const FUeAgentCommandResult& Result,
			FString& OutOperationId,
			FString& OutError)
		{
			OutOperationId.Reset();
			OutError.Reset();
			TSharedPtr<FJsonObject> Root;
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(Result.GetPayloadJson());
			const TSharedPtr<FJsonObject>* Projection = nullptr;
			const TSharedPtr<FJsonObject>* Payload = nullptr;
			FString Status;
			FString EffectOutcome;
			if (!Result.IsOk()
				|| !FJsonSerializer::Deserialize(Reader, Root)
				|| !Root.IsValid()
				|| !Root->TryGetStringField(TEXT("operation_id"), OutOperationId)
				|| OutOperationId.IsEmpty()
				|| !Root->TryGetObjectField(TEXT("projection"), Projection)
				|| !Projection || !Projection->IsValid()
				|| !(*Projection)->TryGetObjectField(TEXT("payload"), Payload)
				|| !Payload || !Payload->IsValid()
				|| !(*Payload)->TryGetStringField(TEXT("status"), Status)
				|| !(*Payload)->TryGetStringField(
					TEXT("effect_outcome"), EffectOutcome)
				|| Status != TEXT("verified")
				|| EffectOutcome != TEXT("complete"))
			{
				OutOperationId.Reset();
				OutError = TEXT("simple_authoring_verified_result_invalid");
				return false;
			}
			return true;
		}

		void ReadFailure(
			const FUeAgentCommandResult& Result,
			FString& OutCode,
			FString& OutDetail)
		{
			OutCode = TEXT("simple_authoring_operation_failed");
			OutDetail = Result.GetErrorJson();
			TSharedPtr<FJsonObject> ErrorObject;
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(Result.GetErrorJson());
			if (FJsonSerializer::Deserialize(Reader, ErrorObject)
				&& ErrorObject.IsValid())
			{
				FString Value;
				if (ErrorObject->TryGetStringField(TEXT("code"), Value)
					&& !Value.IsEmpty())
				{
					OutCode = MoveTemp(Value);
				}
				if (ErrorObject->TryGetStringField(TEXT("detail"), Value)
					&& !Value.IsEmpty())
				{
					OutDetail = MoveTemp(Value);
				}
			}
		}

		bool MutationMayHaveStarted(
			const FString& Code,
			const FString& Detail)
		{
			return Detail == TEXT("rolled_back")
				|| Detail == TEXT("recovery_required")
				|| Detail == TEXT("state_unknown")
				|| Detail == TEXT("failed")
				|| Detail == TEXT("applied_in_memory")
				|| Detail == TEXT("persisted_unverified")
				|| Detail.Contains(TEXT("effect_outcome="))
				|| Code.StartsWith(TEXT("verification_"))
				|| Code.StartsWith(TEXT("operation_verification_"))
				|| Code.StartsWith(TEXT("operation_projection_"))
				|| Code.StartsWith(TEXT("simple_authoring_projection_"));
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
					OutError = FOperationLifecycleError(
						TEXT("operation_cancelled"), Request.GetCommandId());
					return false;
				}
				const FString Scope = Plan.GetImpactScope().IsEmpty()
					? FString() : Plan.GetImpactScope()[0];
				const FString* Expected = Plan.GetExpectedRevisions().Find(Scope);
				if (!Expected || *Expected != Request.GetCommandBindingHash())
				{
					OutError = FOperationLifecycleError(
						TEXT("operation_version_conflict"), Request.GetCommandId());
					return false;
				}
				if (Stage == EOperationLifecycleStage::PrePersistFence)
				{
					if (!Result.IsSet()
						|| !TryReadVerifiedResult(
							Result.GetValue(), OperationId, FailureDetail))
					{
						OutError = FOperationLifecycleError(
							TEXT("simple_authoring_verified_result_invalid"),
							FailureDetail);
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
						Result.GetValue(), FailureCode, FailureDetail);
					if (MutationMayHaveStarted(FailureCode, FailureDetail))
					{
						OutFacts = FOperationMutationFacts(
							TEXT("delegated-failure:") + Plan.GetPlanId(),
							Plan.GetImpactScope(),
							true);
					}
					OutError = FOperationLifecycleError(
						FailureCode, FailureDetail);
					return false;
				}
				if (!TryReadVerifiedResult(
						Result.GetValue(), OperationId, FailureDetail))
				{
					OutError = FOperationLifecycleError(
						TEXT("simple_authoring_verified_result_invalid"),
						FailureDetail);
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
				bOutConfirmed = FailureDetail == TEXT("rolled_back");
				OutError = bOutConfirmed
					? FOperationLifecycleError()
					: FOperationLifecycleError(
						FailureCode.IsEmpty()
							? FString(TEXT("operation_rollback_unconfirmed"))
							: FailureCode,
						FailureDetail);
				return bOutConfirmed;
			}

			bool Settle(
				const FOperationLifecyclePlan&,
				const FOperationMutationFacts&,
				FOperationLifecycleError& OutError) override
			{
				OutError = {};
				FString VerifiedOperationId;
				if (!Result.IsSet()
					|| !TryReadVerifiedResult(
						Result.GetValue(), VerifiedOperationId, FailureDetail))
				{
					OutError = FOperationLifecycleError(
						TEXT("operation_settle_failed"), FailureDetail);
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
				return !OutPersistenceReceiptId.IsEmpty();
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
				if (PersistenceReceiptId != OperationId
					|| !Result.IsSet()
					|| !TryReadVerifiedResult(
						Result.GetValue(), VerifiedOperationId, FailureDetail)
					|| VerifiedOperationId != OperationId)
				{
					OutError = FOperationLifecycleError(
						TEXT("operation_readback_mismatch"), FailureDetail);
					return false;
				}
				bOutEquivalent = true;
				return true;
			}

			bool TryBuildSuccessPayload(
				const FOperationLifecycleReceipt& Receipt,
				FString& OutPayloadJson,
				FString& OutError) const override
			{
				OutPayloadJson.Reset();
				OutError.Reset();
				if (!Result.IsSet()
					|| !Result->IsOk()
					|| Receipt.GetStatus()
						!= EOperationLifecycleStatus::Succeeded)
				{
					OutError = TEXT("simple_authoring_operation_result_missing");
					return false;
				}
				OutPayloadJson = Result->GetPayloadJson();
				return !OutPayloadJson.IsEmpty();
			}

			FString GetFailureDetail() const override
			{
				return FailureDetail;
			}

		private:
			TSharedRef<IUeAgentCommandHandler> Handler;
			FUeAgentCommandRequest Request;
			Contract::FUeAgentCallerContext Caller;
			TSharedRef<Contract::IUeAgentCancellationSignal> Cancellation;
			TOptional<FUeAgentCommandResult> Result;
			FString OperationId;
			FString FailureCode;
			FString FailureDetail;
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
					OutError = DomainCapability::FDomainOperationInvocationError(
						TEXT("simple_authoring_operation_identity_mismatch"),
						Request.GetCommandId(), CommandId);
					return false;
				}
				TSharedPtr<IUeAgentCommandHandler> Handler;
				FString Error;
				if (!TryResolveHandler(CommandId, Handler, Error)
					|| !Handler.IsValid())
				{
					OutError = DomainCapability::FDomainOperationInvocationError(
						TEXT("simple_authoring_runtime_unavailable"),
						CommandId, Error);
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
						TArray<FString>{TEXT("simple_authoring_verified")});
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
					OutError.Code = Cancelled.Code;
					OutError.SubjectId = Cancelled.Reason;
					OutError.Detail = TEXT("SimpleAuthoring projection cancelled");
					return false;
				}
				FUeAgentOperationDeadlineExceeded Expired;
				if (!Deadline.TryCheckpoint(TEXT("simple_authoring.projection"), Expired))
				{
					OutError.Code = Expired.Code;
					OutError.SubjectId = Expired.Stage;
					OutError.Detail = TEXT("SimpleAuthoring projection deadline expired");
					return false;
				}
				FUeAgentResourceBudgetExceeded BudgetError;
				if (!Resources.TryCharge(
						EUeAgentResourceDimension::WorkUnits,
						1,
						TEXT("simple_authoring.projection"),
						BudgetError))
				{
					OutError.Code = BudgetError.Code;
					OutError.SubjectId = BudgetError.Path;
					OutError.Detail = TEXT("SimpleAuthoring projection budget exceeded");
					OutError.ResourceDimension = BudgetError.Dimension;
					OutError.ResourceLimit = BudgetError.Limit;
					OutError.ResourceObserved = BudgetError.Observed;
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

		bool TryCreateRuntimeLocked(FProcessHost& Host, FString& OutError)
		{
			OutError.Reset();
			if (Host.Runtime.IsValid())
			{
				return true;
			}
			if (!IsInGameThread())
			{
				OutError = TEXT("simple_authoring_game_thread_required");
				return false;
			}
			UWorld* World = nullptr;
#if WITH_DEV_AUTOMATION_TESTS
			World = Host.AutomationWorld.Get();
#endif
			if (!World)
			{
				World = GEditor
					? GEditor->GetEditorWorldContext().World()
					: nullptr;
			}
			if (!World)
			{
				OutError = TEXT("simple_authoring_editor_world_unavailable");
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
				OutError = TicketError.Code + TEXT(":") + TicketError.Detail;
				return false;
			}

			TSharedPtr<const FUeAgentResponseProjector> Projector;
			FUeAgentProjectionError ProjectionError;
			if (!FUeAgentResponseProjector::TryCreate(
					MakeShared<FConservativeTokenEstimator>(),
					Projector,
					ProjectionError))
			{
				OutError = ProjectionError.Code + TEXT(":") + ProjectionError.Detail;
				return false;
			}

			FUeAgentSimpleAuthoringRuntimeConfig Config;
			Config.ProjectInstanceId = TEXT("project:") + FString(FApp::GetProjectName());
			Config.EditorInstanceId = FString::Printf(
				TEXT("editor-process:%u"), FPlatformProcess::GetCurrentProcessId());
			Config.ServerSessionId = Host.ServerSessionId;
			Config.RegistryEpoch = 1;
			Config.TimeoutSeconds = 120.0;
			Config.MaxFrameBytes = 4LL * 1024LL * 1024LL;
			Config.JournalRoot = FPaths::ProjectSavedDir()
				/ TEXT("UeAgentInterface/SimpleAuthoring");
			FUeAgentSimpleAuthoringRuntimeError RuntimeError;
			if (!FUeAgentSimpleAuthoringRuntime::TryCreate(
					Config,
					*World,
					TicketService.ToSharedRef(),
					Projector.ToSharedRef(),
					Host.Runtime,
					RuntimeError)
				|| !Host.Runtime.IsValid())
			{
				Host.Runtime.Reset();
				OutError = RuntimeError.Code + TEXT(":") + RuntimeError.Detail;
				return false;
			}

			Host.Handlers.Reset();
			Host.CatalogSourcesByKind.Reset();
			for (const TSharedRef<IUeAgentCommandHandler>& Handler
				: Host.Runtime->GetCommandHandlers())
			{
				if (Host.Handlers.Contains(Handler->GetCommandId()))
				{
					Host.Handlers.Reset();
					Host.Runtime.Reset();
					OutError = TEXT("simple_authoring_handler_duplicate:")
						+ Handler->GetCommandId();
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
						Host.Handlers.Reset();
						Host.CatalogSourcesByKind.Reset();
						Host.Runtime.Reset();
						OutError = TEXT("simple_authoring_catalog_kind_duplicate:")
							+ Kind;
						return false;
					}
					Host.CatalogSourcesByKind.Add(Kind, Source);
				}
			}
			return true;
		}
	}

	bool TryResolveHandler(
		const FString& CommandId,
		TSharedPtr<IUeAgentCommandHandler>& OutHandler,
		FString& OutError)
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
			OutError = TEXT("simple_authoring_handler_missing:") + CommandId;
			return false;
		}
		OutHandler = *Found;
		return true;
	}

	bool TryResolveCatalogSource(
		const FString& CatalogKind,
		TSharedPtr<Discovery::IUeAgentCatalogSource>& OutSource,
		FString& OutError)
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
			OutError = TEXT("simple_authoring_catalog_kind_missing:")
				+ CatalogKind;
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

#if WITH_DEV_AUTOMATION_TESTS
	void SetAutomationWorldOverride(UWorld* World)
	{
		FProcessHost& Host = ProcessHost();
		FScopeLock Lock(&Host.Mutex);
		Host.Handlers.Reset();
		Host.CatalogSourcesByKind.Reset();
		Host.Runtime.Reset();
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
