#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringApplication.h"

#include "Async/Async.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContractSchema.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"

namespace UE::UAI::Composition::SimpleAuthoring
{
	namespace
	{
		using namespace BindingGateway::Operation;
		using namespace BindingGateway::Ticket;
		using namespace ContextResource;
		using namespace Contract;
		using namespace Discovery;
		using namespace PlanningPreflight;
		using namespace Projection;
		using namespace StateSemantic;

		FDiagnostic ApplicationFailure(const FString& Code, const FString& SubjectId, const FString& Reason)
		{
			FDiagnostic Diagnostic = FDiagnostic::Failure(Code, Reason, TEXT("application"));
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			Diagnostic.SubjectId = SubjectId;
			return Diagnostic;
		}

		

		TSharedPtr<FJsonValue> CanonicalValueToJson(
			const FUeAgentCanonicalValue& Value)
		{
			switch (Value.GetKind())
			{
			case EUeAgentValueKind::Null:
				return MakeShared<FJsonValueNull>();
			case EUeAgentValueKind::Boolean:
				return MakeShared<FJsonValueBoolean>(
					Value.GetBoolean());
			case EUeAgentValueKind::String:
				return MakeShared<FJsonValueString>(
					Value.GetText());
			case EUeAgentValueKind::Integer:
			case EUeAgentValueKind::Decimal:
				return MakeShared<FJsonValueString>(
					Value.GetLexeme());
			case EUeAgentValueKind::Array:
			{
				TArray<TSharedPtr<FJsonValue>> Items;
				Items.Reserve(Value.GetArrayItems().Num());
				for (const FUeAgentCanonicalValueRef& Item :
					Value.GetArrayItems())
				{
					Items.Add(CanonicalValueToJson(*Item));
				}
				return MakeShared<FJsonValueArray>(
					MoveTemp(Items));
			}
			case EUeAgentValueKind::Object:
			{
				TSharedRef<FJsonObject> Object =
					MakeShared<FJsonObject>();
				for (const TPair<
					FString,
					FUeAgentCanonicalValueRef>& Item :
					Value.GetObjectItems())
				{
					Object->SetField(
						Item.Key,
						CanonicalValueToJson(*Item.Value));
				}
				return MakeShared<FJsonValueObject>(Object);
			}
			case EUeAgentValueKind::Temporal:
			{
				TSharedRef<FJsonObject> Object =
					MakeShared<FJsonObject>();
				if (Value.GetTemporal().IsValid())
				{
					Object->SetStringField(
						TEXT("clock_id"),
						Value.GetTemporal()->GetClockId());
					Object->SetStringField(
						TEXT("tick"),
						Value.GetTemporal()->GetTick());
					Object->SetStringField(
						TEXT("rate_numerator"),
						Value.GetTemporal()
							->GetRateNumerator());
					Object->SetStringField(
						TEXT("rate_denominator"),
						Value.GetTemporal()
							->GetRateDenominator());
					Object->SetStringField(
						TEXT("state"),
						Value.GetTemporal()->GetState());
				}
				return MakeShared<FJsonValueObject>(Object);
			}
			case EUeAgentValueKind::Spatial:
			default:
				return MakeShared<FJsonValueNull>();
			}
		}

		FUeAgentOperationResult ErrorResult(const FUeAgentOperationRequest& Request, FDiagnostic Diagnostic)
		{
			if (Diagnostic.IsEmpty())
			{
				Diagnostic.Code = TEXT("simple_authoring_operation_failed");
				Diagnostic.PublicMessage = TEXT("The operation application received a failure without a primary diagnostic.");
				Diagnostic.Stage = TEXT("application");
			}
			if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = Request.GetDescriptorIdentity().GetDescriptorId();
			return FUeAgentOperationResult(Request.GetDescriptorIdentity(), Request.GetRequestId(), MoveTemp(Diagnostic));
		}

		FUeAgentOperationResult SuccessResult(
			const FUeAgentOperationRequest& Request,
			const FUeAgentOperationBindingResult& Result)
		{
			TArray<FDiagnostic> Diagnostics = Result.GetValue().GetDiagnostics();
			if (!Result.GetValue().IsValid())
			{
				FDiagnostic Error = Result.GetValue().GetValidationDiagnostic();
				Error.Diagnostics.Append(Diagnostics);
				return ErrorResult(Request, MoveTemp(Error));
			}
			const TSharedPtr<const FUeAgentProjectedResponse>&
				Projection =
					Result.GetValue().GetProjection();
			if (!Projection.IsValid()
				|| !Projection->GetPayload()
					.GetValue()
					.IsValid())
			{
				FDiagnostic Error = ApplicationFailure(TEXT(
						"simple_authoring_projection_missing"), Request.GetRequestId(), TEXT(
						"C3O returned no canonical C9 "
						"outcome."));
				Error.PropertyPath = TEXT("projection.payload");
				Error.Diagnostics.Append(Diagnostics);
				return ErrorResult(Request, MoveTemp(Error));
			}
			TSharedRef<FJsonObject> ProjectionJson =
				MakeShared<FJsonObject>();
			ProjectionJson->SetStringField(
				TEXT("profile_id"),
				Projection->GetProfileId());
			ProjectionJson->SetField(
				TEXT("payload"),
				CanonicalValueToJson(
					*Projection->GetPayload().GetValue()));

			TSharedRef<FJsonObject> Root =
				MakeShared<FJsonObject>();
			Root->SetStringField(
				TEXT("operation_id"),
				Result.GetValue().GetOperationId());
			Root->SetObjectField(
				TEXT("projection"),
				ProjectionJson);
			FString Text;
			FDiagnostic JsonError;
			if (!Json::TryCanonicalize(Root, Text, JsonError))
			{
				JsonError.Diagnostics.Append(Diagnostics);
				return ErrorResult(Request, MoveTemp(JsonError));
			}
			return FUeAgentOperationResult(
				Request.GetDescriptorIdentity(),
				Request.GetRequestId(),
				FUeAgentContractValue(MoveTemp(Text)), MoveTemp(Diagnostics));
		}


		class FSimpleAuthoringSessionSource final
			: public IUeAgentOperationSessionSource
		{
		public:
			explicit FSimpleAuthoringSessionSource(
				FUeAgentOperationSessionIdentity InSession)
				: Session(MoveTemp(InSession))
			{
			}

			FUeAgentOperationSessionIdentity
			GetCurrent() const override
			{
				return Session;
			}

		private:
			FUeAgentOperationSessionIdentity Session;
		};

		class FApplicationEntry final
		{
		public:
			FApplicationEntry(
				FUeAgentSimpleAuthoringCommandRoute InRoute,
				TSharedRef<
					IUeAgentSimpleAuthoringCapabilityProvider>
					InProvider,
				FUeAgentExecutionContextRefOwner InContext,
				TSharedRef<const FUeAgentOperationBinding>
					InBinding)
				: Route(MoveTemp(InRoute))
				, Provider(MoveTemp(InProvider))
				, Context(MoveTemp(InContext))
				, Binding(MoveTemp(InBinding))
			{
			}

			FUeAgentSimpleAuthoringCommandRoute Route;
			TSharedRef<
				IUeAgentSimpleAuthoringCapabilityProvider>
				Provider;
			FUeAgentExecutionContextRefOwner Context;
			TSharedRef<const FUeAgentOperationBinding> Binding;
		};
	}

	FUeAgentSimpleAuthoringPreparedOperation::
		FUeAgentSimpleAuthoringPreparedOperation(
			TArray<FUeAgentCandidateBinding> InTargets,
			FPlanningRequest InPlanning,
			FUeAgentProjectionRequest InProjection)
		: Targets(MoveTemp(InTargets))
		, Planning(MoveTemp(InPlanning))
		, Projection(MoveTemp(InProjection))
	{
		if (Targets.IsEmpty())
		{
			ValidationDiagnostic = FDiagnostic::Failure(TEXT("simple_authoring_prepared_operation_invalid"),
				TEXT("A prepared authoring operation must contain at least one target."), TEXT("prepare"));
			ValidationDiagnostic.SourceKind = TEXT("native_result");
			ValidationDiagnostic.PropertyPath = TEXT("targets");
			ValidationDiagnostic.ActualValue = MakeShared<FJsonValueNumber>(0);
			ValidationDiagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("at least one target"));
			return;
		}
		if (!Planning.IsValid())
		{
			ValidationDiagnostic = Planning.GetValidationDiagnostic();
			return;
		}
		if (!Projection.IsValid())
		{
			ValidationDiagnostic = Projection.GetValidationDiagnostic();
			return;
		}
		TSet<FString> IdentityKeys;
		for (int32 Index = 0; Index < Targets.Num(); ++Index)
		{
			const FUeAgentCandidateBinding& Target = Targets[Index];
			const FString IdentityKey = Target.GetSummary().GetIdentity().GetCanonicalKey();
			if (!Target.IsValid())
			{
				ValidationDiagnostic = *Target.GetValidationDiagnostic();
				if (ValidationDiagnostic.TargetPath.IsEmpty()) ValidationDiagnostic.TargetPath = Target.GetPublicRef();
				return;
			}
			if (IdentityKeys.Contains(IdentityKey))
			{
				ValidationDiagnostic = FDiagnostic::Failure(TEXT("simple_authoring_prepared_operation_invalid"),
					TEXT("The prepared operation contains more than one binding for the same target."), TEXT("prepare"));
				ValidationDiagnostic.SourceKind = TEXT("native_result");
				ValidationDiagnostic.PropertyPath = FString::Printf(TEXT("targets/%d/identity"), Index);
				ValidationDiagnostic.TargetPath = Target.GetPublicRef();
				ValidationDiagnostic.SubjectId = IdentityKey;
				ValidationDiagnostic.ActualValue = MakeShared<FJsonValueNumber>(2);
				ValidationDiagnostic.ExpectedValue = MakeShared<FJsonValueNumber>(1);
				return;
			}
			IdentityKeys.Add(IdentityKey);
		}
	}

	const TArray<FUeAgentCandidateBinding>&
	FUeAgentSimpleAuthoringPreparedOperation::
		GetTargets() const
	{
		return Targets;
	}

	const FPlanningRequest&
	FUeAgentSimpleAuthoringPreparedOperation::
		GetPlanning() const
	{
		return Planning;
	}

	const FUeAgentProjectionRequest&
	FUeAgentSimpleAuthoringPreparedOperation::
		GetProjection() const
	{
		return Projection;
	}

	bool FUeAgentSimpleAuthoringPreparedOperation::IsValid() const
	{
		return ValidationDiagnostic.IsEmpty();
	}

	class FUeAgentSimpleAuthoringApplication::FState final
	{
	public:
		FState(
			FUeAgentSimpleAuthoringApplicationConfig InConfig,
			TArray<FApplicationEntry> InEntries,
			TSharedRef<const FUeAgentOperationTicketService>
				InTicketService,
			FUeAgentOperationSessionIdentity InSession)
			: Config(MoveTemp(InConfig))
			, Entries(MoveTemp(InEntries))
			, TicketService(MoveTemp(InTicketService))
			, Session(MoveTemp(InSession))
		{
			for (int32 Index = 0; Index < Entries.Num(); ++Index)
			{
				EntryByDescriptorId.Add(
					Entries[Index]
						.Route
						.DescriptorIdentity
						.GetDescriptorId(),
					Index);
			}
		}

		const FApplicationEntry* FindExact(
			const FUeAgentDescriptorIdentity& Identity) const
		{
			const int32* Index =
				EntryByDescriptorId.Find(
					Identity.GetDescriptorId());
			if (!Index
				|| !Entries.IsValidIndex(*Index)
				|| !(Entries[*Index]
					.Route
					.DescriptorIdentity == Identity))
			{
				return nullptr;
			}
			return &Entries[*Index];
		}

		FUeAgentSimpleAuthoringApplicationConfig Config;
		TArray<FApplicationEntry> Entries;
		TSharedRef<const FUeAgentOperationTicketService>
			TicketService;
		FUeAgentOperationSessionIdentity Session;
		TMap<FString, int32> EntryByDescriptorId;
	};

	FUeAgentSimpleAuthoringApplication::
		FUeAgentSimpleAuthoringApplication(
			TSharedRef<FState, ESPMode::ThreadSafe> InState)
		: State(MoveTemp(InState))
	{
	}

	FUeAgentSimpleAuthoringApplication::
		~FUeAgentSimpleAuthoringApplication() = default;

	bool FUeAgentSimpleAuthoringApplication::TryCreate(
		const FUeAgentSimpleAuthoringApplicationConfig& Config,
		TArray<FUeAgentSimpleAuthoringCommandRoute> Routes,
		TArray<TSharedRef<
			IUeAgentSimpleAuthoringCapabilityProvider>> Providers,
		TSharedRef<const FUeAgentOperationTicketService>
			TicketService,
		TSharedRef<const FUeAgentResponseProjector> Projector,
		TSharedPtr<FUeAgentSimpleAuthoringApplication>&
			OutApplication,
		FUeAgentSimpleAuthoringApplicationError& OutError)
	{
		OutApplication.Reset();
		OutError = {};
		if (!Config.DescriptorRegistry.IsValid()
			|| Config.ServerSessionId.IsEmpty()
			|| Config.RegistryEpoch < 1
			|| Routes.Num() < 1
			|| Providers.Num() < 1)
		{
			OutError = ApplicationFailure(TEXT(
					"simple_authoring_application_config_invalid"), TEXT("simple_authoring"), TEXT(
					"Descriptor registry, session, "
					"routes and providers are required."));
			return false;
		}

		TMap<FString, TSharedRef<
			IUeAgentSimpleAuthoringCapabilityProvider>>
			ProviderByFamily;
		for (const TSharedRef<
			IUeAgentSimpleAuthoringCapabilityProvider>& Provider :
			Providers)
		{
			const FString FamilyId = Provider->GetFamilyId();
			if (FamilyId.IsEmpty()
				|| ProviderByFamily.Contains(FamilyId))
			{
				OutError = ApplicationFailure(TEXT(
						"simple_authoring_provider_duplicate"), FamilyId, TEXT(
						"Each family must have exactly one "
						"production provider."));
				return false;
			}
			ProviderByFamily.Add(FamilyId, Provider);
		}

		const FUeAgentOperationSessionIdentity Session(
			Config.ServerSessionId,
			Config.RegistryEpoch,
			Config.DescriptorRegistry->GetRegistryHash());
		if (!Session.IsValid())
		{
			OutError = ApplicationFailure(TEXT(
					"simple_authoring_session_invalid"), Config.ServerSessionId, TEXT(
					"Server session identity is invalid."));
			return false;
		}
		const TSharedRef<FSimpleAuthoringSessionSource>
			SessionSource =
				MakeShared<FSimpleAuthoringSessionSource>(
					Session);

		TSet<FString> CommandIds;
		TSet<FString> DescriptorIds;
		TSet<FString> UsedFamilies;
		TArray<FApplicationEntry> Entries;
		Entries.Reserve(Routes.Num());
		for (FUeAgentSimpleAuthoringCommandRoute& Route : Routes)
		{
			const FUeAgentOperationDescriptor* Descriptor =
				Config.DescriptorRegistry->FindExact(
					Route.DescriptorIdentity);
			TSharedRef<
				IUeAgentSimpleAuthoringCapabilityProvider>*
				Provider =
					ProviderByFamily.Find(Route.FamilyId);
			if (!Route.TryValidate(OutError)) return false;
			if (!Descriptor
				|| Descriptor->GetOperationKind()
					!= EUeAgentDescriptorOperationKind::
						Operation
				|| Descriptor->GetCapabilityId()
					!= Route.CapabilityId
				|| Descriptor->GetDomainId()
					!= Route.FamilyId
				|| !Config.DescriptorRegistry
					->FindCompiledInputSchemaExact(
						Route.DescriptorIdentity)
				|| !Provider)
			{
				OutError = ApplicationFailure(TEXT(
						"simple_authoring_route_invalid"), Route.CommandId, TEXT(
						"Route, exact descriptor, compiled "
						"schema or family provider is "
						"missing."));
				return false;
			}
			if (CommandIds.Contains(Route.CommandId)
				|| DescriptorIds.Contains(
					Route.DescriptorIdentity
						.GetDescriptorId()))
			{
				OutError = ApplicationFailure(TEXT(
						"simple_authoring_route_duplicate"), Route.CommandId, TEXT(
						"Command and exact descriptor must "
						"each have one application binding."));
				return false;
			}

			FUeAgentExecutionContextRefPtr ExpectedContext;
			FUeAgentExecutionContextError ContextError;
			if (!FUeAgentExecutionContextRef::TryCreate(
					Config.ExecutionContext,
					Descriptor->GetIdentity(),
					EUeAgentRequestKind::Operation,
					Descriptor->GetExecutionContextPolicy(),
					ExpectedContext,
					ContextError)
				|| !ExpectedContext.IsValid())
			{
				OutError = !ContextError.IsEmpty() ? ContextError : ApplicationFailure(TEXT("simple_authoring_context_invalid"), Route.CommandId, TEXT("The explicit execution context does not satisfy the descriptor."));
				return false;
			}
			const TSharedRef<
				FUeAgentExactExecutionContextValidator>
				ContextValidator =
					MakeShared<
						FUeAgentExactExecutionContextValidator>(
						ExpectedContext.ToSharedRef());
			TSharedPtr<const FUeAgentOperationBinding>
				OperationBinding;
			FUeAgentOperationBindingError BindingError;
			if (!FUeAgentOperationBinding::TryCreate(
					Config.DescriptorRegistry.ToSharedRef(),
					Route.DescriptorIdentity,
					ContextValidator,
					(*Provider)->GetPlanner(),
					(*Provider)->GetExecutor(),
					(*Provider)->GetVerifier(),
					Projector,
					SessionSource,
					TicketService,
					OperationBinding,
					BindingError)
				|| !OperationBinding.IsValid())
			{
				OutError = !BindingError.IsEmpty() ? BindingError : ApplicationFailure(TEXT("simple_authoring_binding_invalid"), Route.CommandId, TEXT("The operation binding did not return a valid result."));
				return false;
			}

			CommandIds.Add(Route.CommandId);
			DescriptorIds.Add(
				Route.DescriptorIdentity.GetDescriptorId());
			UsedFamilies.Add(Route.FamilyId);
			Entries.Emplace(
				MoveTemp(Route),
				*Provider,
				ExpectedContext.ToSharedRef(),
				OperationBinding.ToSharedRef());
		}
		if (UsedFamilies.Num() != ProviderByFamily.Num())
		{
			OutError = ApplicationFailure(TEXT(
					"simple_authoring_provider_unbound"), TEXT("simple_authoring"), TEXT(
					"Every injected provider must own at "
					"least one exact route."));
			return false;
		}

		const TSharedRef<FState, ESPMode::ThreadSafe> NewState =
			MakeShared<FState, ESPMode::ThreadSafe>(
				Config,
				MoveTemp(Entries),
				TicketService,
				Session);
		OutApplication =
			MakeShareable(
				new FUeAgentSimpleAuthoringApplication(
					NewState));
		return true;
	}

	FUeAgentOperationResult
	FUeAgentSimpleAuthoringApplication::Submit(
		const FUeAgentOperationRequest& Request,
		const FUeAgentCallerContext& CallerContext,
		const TSharedRef<IUeAgentCancellationSignal>&
			Cancellation)
	{
		if (!IsInGameThread())
		{
			const TSharedRef<TOptional<FUeAgentOperationResult>>
				Result =
					MakeShared<
						TOptional<FUeAgentOperationResult>>();
			FEvent* Completed =
				FPlatformProcess::GetSynchEventFromPool(true);
			const TSharedRef<FState, ESPMode::ThreadSafe>
				StateRef = State;
			AsyncTask(
				ENamedThreads::GameThread,
				[
					StateRef,
					Request,
					CallerContext,
					Cancellation,
					Result,
					Completed
				]()
				{
					FUeAgentSimpleAuthoringApplication
						Application(StateRef);
					Result->Emplace(
						Application.Submit(
							Request,
							CallerContext,
							Cancellation));
					Completed->Trigger();
				});
			Completed->Wait();
			FPlatformProcess::ReturnSynchEventToPool(Completed);
			return Result->GetValue();
		}
		return SubmitOnGameThread(
			Request,
			CallerContext,
			Cancellation);
	}

	FUeAgentOperationResult
	FUeAgentSimpleAuthoringApplication::SubmitOnGameThread(
		const FUeAgentOperationRequest& Request,
		const FUeAgentCallerContext& CallerContext,
		const TSharedRef<IUeAgentCancellationSignal>&
			Cancellation)
	{
		if (!Request.IsValid() || !CallerContext.IsValid())
		{
			return ErrorResult(Request, ApplicationFailure(TEXT(
					"simple_authoring_request_invalid"), Request.GetRequestId(), TEXT(
					"Operation request or caller context is "
					"invalid.")));
		}
		const FApplicationEntry* Entry =
			State->FindExact(Request.GetDescriptorIdentity());
		if (!Entry)
		{
			return ErrorResult(Request, ApplicationFailure(TEXT(
					"simple_authoring_descriptor_not_bound"), Request.GetDescriptorIdentity()
					.GetDescriptorId(), TEXT(
					"Exact descriptor has no active Simple "
					"Authoring binding.")));
		}

		const TSharedRef<
			FUeAgentResourceTracker,
			ESPMode::ThreadSafe> Resources =
			MakeShared<
				FUeAgentResourceTracker,
				ESPMode::ThreadSafe>();
		const auto Clock = []()
		{
			return FPlatformTime::Seconds();
		};
		FUeAgentCancellationSource CancellationSource(Clock);
		if (Cancellation->IsCancelled())
		{
			CancellationSource.Request(
				TEXT("protocol_cancellation_requested"));
		}
		const FUeAgentCancellationToken CancellationToken =
			CancellationSource.GetToken();
		const FUeAgentOperationDeadline Deadline;

		FUeAgentResourceChargeError InputChargeError;
		const FUeAgentTextUnits InputUnits =
			MeasureText(
				Request.GetPayload().GetJsonValue());
		if (!Resources->TryCharge(
				EUeAgentResourceDimension::InputBytes,
				InputUnits.Utf8Bytes,
				TEXT("simple_authoring.c0.input"),
				InputChargeError))
		{
			return ErrorResult(Request, InputChargeError);
		}

		const FUeAgentContractSchema* Schema =
			State->Config.DescriptorRegistry
				->FindCompiledInputSchemaExact(
					Request.GetDescriptorIdentity());
		if (!Schema)
		{
			return ErrorResult(Request, ApplicationFailure(TEXT(
					"simple_authoring_schema_unavailable"), Request.GetDescriptorIdentity()
					.GetDescriptorId(), TEXT(
					"Exact compiled input schema is "
					"unavailable.")));
		}
		TOptional<FUeAgentContractValue> CanonicalPayload;
		FUeAgentContractSchemaError SchemaError;
		const FUeAgentContractSchemaCheckpoint Checkpoint =
			[
				Resources,
				Deadline,
				CancellationToken
			](
				const int64 WorkUnits,
				const FString& InstancePath,
				FUeAgentContractSchemaError& Error)
			{
				FUeAgentCancellationRequested Cancelled;
				if (!CancellationToken.TryCheckpoint(Cancelled))
				{
					Error = Cancelled;
					Error.Code = TEXT("operation_cancelled");
					Error.PublicCause = MakeShared<UE::UAI::Contract::FDiagnostic>(Cancelled);
					Error.Source.JsonPath = InstancePath;
					return false;
				}
				FUeAgentResourceChargeError ChargeError;
				if (!Resources->TryCharge(
						EUeAgentResourceDimension::WorkUnits,
						WorkUnits,
						TEXT("simple_authoring.c0.schema"),
						ChargeError))
				{
					Error = ChargeError;
					Error.Source.JsonPath = InstancePath;
					return false;
				}
				return true;
			};
		if (!Schema->TryValidateAndCanonicalize(
				Request.GetPayload(),
				Checkpoint,
				CanonicalPayload,
				SchemaError)
			|| !CanonicalPayload.IsSet())
		{
			return ErrorResult(Request, !SchemaError.IsEmpty() ? SchemaError : ApplicationFailure(TEXT("simple_authoring_input_invalid"), SchemaError.Source.JsonPath, TEXT("The input does not satisfy the declared schema.")));
		}
		const FUeAgentOperationRequest CanonicalRequest(
			Request.GetDescriptorIdentity(),
			Request.GetRequestId(),
			Request.GetIdempotencyKey(),
			CanonicalPayload.GetValue());

		TOptional<FUeAgentSimpleAuthoringPreparedOperation>
			Prepared;
		FUeAgentSimpleAuthoringApplicationError PrepareError;
		const bool bPrepared = Entry->Provider->TryPrepare(
			CanonicalRequest,
			*Entry->Context,
			CallerContext,
			*Resources,
			Deadline,
			CancellationToken,
			Prepared,
			PrepareError);
		if (!bPrepared || !Prepared.IsSet() || !Prepared->IsValid())
		{
			if (PrepareError.IsEmpty() && Prepared.IsSet() && !Prepared->IsValid())
				PrepareError = Prepared->GetValidationDiagnostic();
			if (PrepareError.IsEmpty())
			{
				PrepareError = ApplicationFailure(TEXT("simple_authoring_prepare_failed"), Request.GetRequestId(),
					bPrepared ? TEXT("The provider reported successful preparation without returning a prepared operation.")
						: TEXT("The authoring provider failed to prepare the operation without reporting a diagnostic."));
				PrepareError.SourceKind = TEXT("native_result");
				PrepareError.CandidateName = Request.GetDescriptorIdentity().GetDescriptorId();
				PrepareError.PropertyPath = TEXT("prepared_operation");
				PrepareError.ActualValuePresent = Prepared.IsSet();
				PrepareError.ExpectedValuePresent = true;
			}
			return ErrorResult(Request, PrepareError);
		}

		const FPlanningRequest& Planning =
			Prepared->GetPlanning();
		const FChangeSetDraft& Draft =
			Planning.GetDraft();
		if (!(Draft.GetDescriptorIdentity()
				== CanonicalRequest
					.GetDescriptorIdentity())
			|| Draft.GetRequestId()
				!= CanonicalRequest.GetRequestId()
			|| Draft.GetIdempotencyKey()
				!= CanonicalRequest.GetIdempotencyKey())
		{
			return ErrorResult(Request, ApplicationFailure(TEXT(
					"simple_authoring_preparation_identity_mismatch"), Request.GetRequestId(), TEXT(
					"Prepared planning request differs from "
					"the canonical operation request.")));
		}

		TArray<FUeAgentOperationTargetBinding>
			TargetBindings;
		TargetBindings.Reserve(
			Prepared->GetTargets().Num());
		for (const FUeAgentCandidateBinding& Candidate :
			Prepared->GetTargets())
		{
			TOptional<FUeAgentOperationTicket> Ticket;
			FUeAgentOperationTicketError TicketError;
			if (!State->TicketService->TryIssue(
					Candidate,
					CanonicalRequest
						.GetDescriptorIdentity(),
					State->Session,
					CallerContext,
					Draft.GetSnapshot()->GetRevision(),
					Ticket,
					TicketError)
				|| !Ticket.IsSet())
			{
				return ErrorResult(Request, !TicketError.IsEmpty() ? TicketError : ApplicationFailure(TEXT("simple_authoring_ticket_issue_failed"), TicketError.SubjectId, TEXT("The operation ticket could not be issued.")));
			}
			TargetBindings.Emplace(
				Ticket.GetValue(),
				Candidate);
		}

		const FUeAgentOperationBindingPayload Payload(
			State->Session,
			MoveTemp(TargetBindings),
			State->Config.ExecutionContext,
			Planning,
			Prepared->GetProjection());
		const FUeAgentOperationBindingRequest TypedRequest(
			CanonicalRequest.GetDescriptorIdentity(),
			CanonicalRequest.GetRequestId(),
			CanonicalRequest.GetIdempotencyKey(),
			Payload);
		TSharedPtr<const FUeAgentOperationBindingResult>
			BindingResult;
		FUeAgentOperationBindingError BindingError;
		if (!Entry->Binding->TrySubmit(
				TypedRequest,
				CallerContext,
				Resources,
				Deadline,
				CancellationToken,
				BindingResult,
				BindingError)
			|| !BindingResult.IsValid())
		{
			if (BindingResult.IsValid()) BindingError.Diagnostics.Append(BindingResult->GetValue().GetDiagnostics());
			return ErrorResult(Request, MoveTemp(BindingError));
		}
		return SuccessResult(
			Request,
			*BindingResult);
	}

	int32 FUeAgentSimpleAuthoringApplication::
		GetBindingCount() const
	{
		return State->Entries.Num();
	}
}
