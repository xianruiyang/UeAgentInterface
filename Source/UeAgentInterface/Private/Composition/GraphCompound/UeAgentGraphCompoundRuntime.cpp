#include "Composition/GraphCompound/UeAgentGraphCompoundRuntime.h"

#include "Async/Async.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/Contract/UeAgentContractSchema.h"
#include "Dom/JsonObject.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"

namespace UE::UAI::Composition::GraphCompound
{
	using namespace ContextResource;
	using namespace Contract;

	namespace
	{
		void SetError(
			FGraphCompoundRuntimeError& OutError,
			const FString& Code,
			const FString& SubjectId,
			const FString& Detail)
		{
			OutError = {Code, SubjectId, Detail};
		}

		FString ErrorJson(
			const TCHAR* Envelope,
			const FString& Code,
			const FString& SubjectId,
			const FString& Detail)
		{
			const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), Code);
			Error->SetStringField(TEXT("subject_id"), SubjectId);
			Error->SetStringField(TEXT("detail"), Detail);
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetObjectField(Envelope, Error);
			FString Json;
			FString JsonError;
			return Json::TryCanonicalize(Root, Json, JsonError)
				? Json
				: FString(TEXT("{}"));
		}

		FUeAgentQueryResult QueryErrorResult(
			const FUeAgentQueryRequest& Request,
			const FString& Code,
			const FString& SubjectId,
			const FString& Detail)
		{
			return FUeAgentQueryResult(
				Request.GetDescriptorIdentity(),
				Request.GetRequestId(),
				FUeAgentContractValue(ErrorJson(
					TEXT("query_error"), Code, SubjectId, Detail)));
		}

		FUeAgentOperationResult OperationErrorResult(
			const FUeAgentOperationRequest& Request,
			const FString& Code,
			const FString& SubjectId,
			const FString& Detail)
		{
			return FUeAgentOperationResult(
				Request.GetDescriptorIdentity(),
				Request.GetRequestId(),
				FUeAgentContractValue(ErrorJson(
					TEXT("operation_error"), Code, SubjectId, Detail)));
		}

		FUeAgentResourceBudget RuntimeBudget(const int64 MaxFrameBytes)
		{
			return FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, MaxFrameBytes},
				{EUeAgentResourceDimension::OutputBytes, MaxFrameBytes},
				{EUeAgentResourceDimension::StringBytes, MaxFrameBytes},
				{
					EUeAgentResourceDimension::WorkUnits,
					FMath::Max<int64>(1024, MaxFrameBytes * 4),
				},
				{
					EUeAgentResourceDimension::EstimatedMemoryBytes,
					FMath::Max<int64>(4096, MaxFrameBytes * 4),
				},
			});
		}

		FString ResourceDetail(
			const FUeAgentResourceBudgetExceeded& Error)
		{
			return FString::Printf(
				TEXT("Resource '%s' exceeded at '%s': observed %lld, limit %lld."),
				*LexToString(Error.Dimension),
				*Error.Path,
				Error.Observed,
				Error.Limit);
		}

		bool TryChargeText(
			FUeAgentResourceTracker& Resources,
			const EUeAgentResourceDimension Dimension,
			const FString& Text,
			const FString& Path,
			FUeAgentResourceBudgetExceeded& OutError)
		{
			return Resources.TryCharge(
				Dimension,
				MeasureText(Text).Utf8Bytes,
				Path,
				OutError);
		}

		bool TryValidate(
			const FUeAgentContractSchema& Schema,
			const FUeAgentContractValue& Input,
			FUeAgentResourceTracker& Resources,
			TOptional<FUeAgentContractValue>& OutCanonical,
			FUeAgentContractSchemaError& OutSchemaError,
			TOptional<FUeAgentResourceBudgetExceeded>& OutResourceError)
		{
			OutResourceError.Reset();
			return Schema.TryValidateAndCanonicalize(
				Input,
				[&Resources, &OutResourceError](
					const int64 WorkUnits,
					const FString& InstancePath,
					FUeAgentContractSchemaError& OutError)
				{
					FUeAgentResourceBudgetExceeded ResourceError;
					if (Resources.TryCharge(
							EUeAgentResourceDimension::WorkUnits,
							WorkUnits,
							InstancePath,
							ResourceError))
					{
						return true;
					}
					OutResourceError = ResourceError;
					OutError.Code = TEXT("contract_schema_resource_exceeded");
					OutError.InstancePath = InstancePath;
					OutError.Detail = ResourceDetail(ResourceError);
					return false;
				},
				OutCanonical,
				OutSchemaError);
		}

		FString SchemaDetail(const FUeAgentContractSchemaError& Error)
		{
			return FString::Printf(
				TEXT("%s at instance '%s' and schema '%s'. %s"),
				*Error.Code,
				*Error.InstancePath,
				*Error.SchemaPath,
				*Error.Detail);
		}
	}

	class FGraphCompoundRuntime::FState final
	{
	public:
		FState(
			FGraphCompoundRuntimeConfig InConfig,
			TSharedRef<const FUeAgentDescriptorRegistry> InDescriptorRegistry,
			TMap<FString, TSharedRef<IGraphCompoundQueryBinding>>
				InQueryBindings,
			TMap<FString, TSharedRef<IGraphCompoundOperationBinding>>
				InOperationBindings)
			: Config(MoveTemp(InConfig))
			, DescriptorRegistry(MoveTemp(InDescriptorRegistry))
			, QueryBindings(MoveTemp(InQueryBindings))
			, OperationBindings(MoveTemp(InOperationBindings))
		{
		}

		FGraphCompoundRuntimeConfig Config;
		TSharedRef<const FUeAgentDescriptorRegistry> DescriptorRegistry;
		TMap<FString, TSharedRef<IGraphCompoundQueryBinding>> QueryBindings;
		TMap<FString, TSharedRef<IGraphCompoundOperationBinding>>
			OperationBindings;
	};

	namespace
	{
		FUeAgentQueryResult ExecuteQuery(
			const TSharedRef<FGraphCompoundRuntime::FState, ESPMode::ThreadSafe>&
				State,
			const FUeAgentQueryRequest& Request,
			const FUeAgentCallerContext& CallerContext,
			const TSharedRef<IUeAgentCancellationSignal>& Cancellation)
		{
			const FString SubjectId =
				Request.GetDescriptorIdentity().GetDescriptorId();
			if (!Request.IsValid() || !CallerContext.IsValid())
			{
				return QueryErrorResult(
					Request,
					TEXT("graph_compound_query_request_invalid"),
					SubjectId,
					TEXT("A valid query request and caller context are required."));
			}

			const FUeAgentOperationDescriptor* Descriptor =
				State->DescriptorRegistry->FindExact(
					Request.GetDescriptorIdentity());
			if (!Descriptor)
			{
				return QueryErrorResult(
					Request,
					TEXT("graph_compound_descriptor_not_exact"),
					SubjectId,
					TEXT("The complete DescriptorIdentity is not active."));
			}
			if (Descriptor->GetOperationKind()
				!= EUeAgentDescriptorOperationKind::Query)
			{
				return QueryErrorResult(
					Request,
					TEXT("graph_compound_lane_mismatch"),
					SubjectId,
					TEXT("The descriptor is not a query."));
			}

			TSharedRef<IGraphCompoundQueryBinding>* Binding =
				State->QueryBindings.Find(SubjectId);
			if (!Binding)
			{
				return QueryErrorResult(
					Request,
					TEXT("graph_compound_query_binding_missing"),
					SubjectId,
					TEXT("The exact query owner is unavailable."));
			}
			if (Cancellation->IsCancelled())
			{
				return QueryErrorResult(
					Request,
					TEXT("graph_compound_cancelled"),
					SubjectId,
					TEXT("The query was cancelled before native execution."));
			}

			const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
				Resources = MakeShared<FUeAgentResourceTracker,
					ESPMode::ThreadSafe>(
					RuntimeBudget(State->Config.MaxFrameBytes));
			FUeAgentResourceBudgetExceeded ResourceError;
			if (!TryChargeText(
					*Resources,
					EUeAgentResourceDimension::InputBytes,
					Request.GetPayload().GetJsonValue(),
					TEXT("query.input"),
					ResourceError))
			{
				return QueryErrorResult(
					Request,
					TEXT("graph_compound_resource_exceeded"),
					SubjectId,
					ResourceDetail(ResourceError));
			}

			const FUeAgentContractSchema* InputSchema =
				State->DescriptorRegistry->FindCompiledInputSchemaExact(
					Request.GetDescriptorIdentity());
			TOptional<FUeAgentContractValue> CanonicalInput;
			FUeAgentContractSchemaError SchemaError;
			TOptional<FUeAgentResourceBudgetExceeded> SchemaResourceError;
			if (!InputSchema
				|| !TryValidate(
					*InputSchema,
					Request.GetPayload(),
					*Resources,
					CanonicalInput,
					SchemaError,
					SchemaResourceError)
				|| !CanonicalInput.IsSet())
			{
				return QueryErrorResult(
					Request,
					SchemaResourceError.IsSet()
						? TEXT("graph_compound_resource_exceeded")
						: (SchemaError.Code.IsEmpty()
							? TEXT("graph_compound_input_schema_missing")
							: SchemaError.Code),
					SubjectId,
					SchemaResourceError.IsSet()
						? ResourceDetail(SchemaResourceError.GetValue())
						: SchemaDetail(SchemaError));
			}

			const FUeAgentQueryRequest CanonicalRequest(
				Request.GetDescriptorIdentity(),
				Request.GetRequestId(),
				CanonicalInput.GetValue());
			FUeAgentContractValue NativeResult(TEXT("{}"));
			FGraphCompoundRuntimeError BindingError;
			if (!(*Binding)->TryExecute(
					CanonicalRequest,
					CallerContext,
					Resources,
					Cancellation,
					NativeResult,
					BindingError))
			{
				return QueryErrorResult(
					Request,
					BindingError.IsValid()
						? BindingError.Code
						: TEXT("graph_compound_query_binding_failed"),
					BindingError.SubjectId.IsEmpty()
						? (*Binding)->GetOwnerId()
						: BindingError.SubjectId,
					BindingError.Detail.IsEmpty()
						? TEXT("The exact query owner failed without an error contract.")
						: BindingError.Detail);
			}

			if (!TryChargeText(
					*Resources,
					EUeAgentResourceDimension::OutputBytes,
					NativeResult.GetJsonValue(),
					TEXT("query.output"),
					ResourceError))
			{
				return QueryErrorResult(
					Request,
					TEXT("graph_compound_resource_exceeded"),
					SubjectId,
					ResourceDetail(ResourceError));
			}

			const FUeAgentContractSchema* ResultSchema =
				State->DescriptorRegistry->FindCompiledResultSchemaExact(
					Request.GetDescriptorIdentity());
			TOptional<FUeAgentContractValue> CanonicalResult;
			SchemaError = {};
			SchemaResourceError.Reset();
			if (!ResultSchema
				|| !TryValidate(
					*ResultSchema,
					NativeResult,
					*Resources,
					CanonicalResult,
					SchemaError,
					SchemaResourceError)
				|| !CanonicalResult.IsSet())
			{
				return QueryErrorResult(
					Request,
					SchemaResourceError.IsSet()
						? TEXT("graph_compound_resource_exceeded")
						: TEXT("graph_compound_result_schema_invalid"),
					SubjectId,
					SchemaResourceError.IsSet()
						? ResourceDetail(SchemaResourceError.GetValue())
						: SchemaDetail(SchemaError));
			}

			return FUeAgentQueryResult(
				Request.GetDescriptorIdentity(),
				Request.GetRequestId(),
				CanonicalResult.GetValue());
		}

		FUeAgentOperationResult ExecuteOperation(
			const TSharedRef<FGraphCompoundRuntime::FState, ESPMode::ThreadSafe>&
				State,
			const FUeAgentOperationRequest& Request,
			const FUeAgentCallerContext& CallerContext,
			const TSharedRef<IUeAgentCancellationSignal>& Cancellation)
		{
			const FString SubjectId =
				Request.GetDescriptorIdentity().GetDescriptorId();
			if (!Request.IsValid() || !CallerContext.IsValid())
			{
				return OperationErrorResult(
					Request,
					TEXT("graph_compound_operation_request_invalid"),
					SubjectId,
					TEXT("A valid operation request and caller context are required."));
			}

			const FUeAgentOperationDescriptor* Descriptor =
				State->DescriptorRegistry->FindExact(
					Request.GetDescriptorIdentity());
			if (!Descriptor)
			{
				return OperationErrorResult(
					Request,
					TEXT("graph_compound_descriptor_not_exact"),
					SubjectId,
					TEXT("The complete DescriptorIdentity is not active."));
			}
			if (Descriptor->GetOperationKind()
				!= EUeAgentDescriptorOperationKind::Operation)
			{
				return OperationErrorResult(
					Request,
					TEXT("graph_compound_lane_mismatch"),
					SubjectId,
					TEXT("The descriptor is not an operation."));
			}

			TSharedRef<IGraphCompoundOperationBinding>* Binding =
				State->OperationBindings.Find(SubjectId);
			if (!Binding)
			{
				return OperationErrorResult(
					Request,
					TEXT("graph_compound_operation_binding_missing"),
					SubjectId,
					TEXT("The exact operation owner is unavailable."));
			}
			if (Cancellation->IsCancelled())
			{
				return OperationErrorResult(
					Request,
					TEXT("graph_compound_cancelled"),
					SubjectId,
					TEXT("The operation was cancelled before native execution."));
			}

			const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
				Resources = MakeShared<FUeAgentResourceTracker,
					ESPMode::ThreadSafe>(
					RuntimeBudget(State->Config.MaxFrameBytes));
			FUeAgentResourceBudgetExceeded ResourceError;
			if (!TryChargeText(
					*Resources,
					EUeAgentResourceDimension::InputBytes,
					Request.GetPayload().GetJsonValue(),
					TEXT("operation.input"),
					ResourceError))
			{
				return OperationErrorResult(
					Request,
					TEXT("graph_compound_resource_exceeded"),
					SubjectId,
					ResourceDetail(ResourceError));
			}

			const FUeAgentContractSchema* InputSchema =
				State->DescriptorRegistry->FindCompiledInputSchemaExact(
					Request.GetDescriptorIdentity());
			TOptional<FUeAgentContractValue> CanonicalInput;
			FUeAgentContractSchemaError SchemaError;
			TOptional<FUeAgentResourceBudgetExceeded> SchemaResourceError;
			if (!InputSchema
				|| !TryValidate(
					*InputSchema,
					Request.GetPayload(),
					*Resources,
					CanonicalInput,
					SchemaError,
					SchemaResourceError)
				|| !CanonicalInput.IsSet())
			{
				return OperationErrorResult(
					Request,
					SchemaResourceError.IsSet()
						? TEXT("graph_compound_resource_exceeded")
						: (SchemaError.Code.IsEmpty()
							? TEXT("graph_compound_input_schema_missing")
							: SchemaError.Code),
					SubjectId,
					SchemaResourceError.IsSet()
						? ResourceDetail(SchemaResourceError.GetValue())
						: SchemaDetail(SchemaError));
			}

			const FUeAgentOperationRequest CanonicalRequest(
				Request.GetDescriptorIdentity(),
				Request.GetRequestId(),
				Request.GetIdempotencyKey(),
				CanonicalInput.GetValue());
			FUeAgentContractValue NativeResult(TEXT("{}"));
			FGraphCompoundRuntimeError BindingError;
			if (!(*Binding)->TryExecute(
					CanonicalRequest,
					CallerContext,
					Resources,
					Cancellation,
					NativeResult,
					BindingError))
			{
				return OperationErrorResult(
					Request,
					BindingError.IsValid()
						? BindingError.Code
						: TEXT("graph_compound_operation_binding_failed"),
					BindingError.SubjectId.IsEmpty()
						? (*Binding)->GetOwnerId()
						: BindingError.SubjectId,
					BindingError.Detail.IsEmpty()
						? TEXT("The exact operation owner failed without an error contract.")
						: BindingError.Detail);
			}

			if (!TryChargeText(
					*Resources,
					EUeAgentResourceDimension::OutputBytes,
					NativeResult.GetJsonValue(),
					TEXT("operation.output"),
					ResourceError))
			{
				return OperationErrorResult(
					Request,
					TEXT("graph_compound_resource_exceeded"),
					SubjectId,
					ResourceDetail(ResourceError));
			}

			const FUeAgentContractSchema* ResultSchema =
				State->DescriptorRegistry->FindCompiledResultSchemaExact(
					Request.GetDescriptorIdentity());
			TOptional<FUeAgentContractValue> CanonicalResult;
			SchemaError = {};
			SchemaResourceError.Reset();
			if (!ResultSchema
				|| !TryValidate(
					*ResultSchema,
					NativeResult,
					*Resources,
					CanonicalResult,
					SchemaError,
					SchemaResourceError)
				|| !CanonicalResult.IsSet())
			{
				return OperationErrorResult(
					Request,
					SchemaResourceError.IsSet()
						? TEXT("graph_compound_resource_exceeded")
						: TEXT("graph_compound_result_schema_invalid"),
					SubjectId,
					SchemaResourceError.IsSet()
						? ResourceDetail(SchemaResourceError.GetValue())
						: SchemaDetail(SchemaError));
			}

			return FUeAgentOperationResult(
				Request.GetDescriptorIdentity(),
				Request.GetRequestId(),
				CanonicalResult.GetValue());
		}

		class FQueryApplication final : public IUeAgentQueryApplicationPort
		{
		public:
			explicit FQueryApplication(
				TSharedRef<FGraphCompoundRuntime::FState, ESPMode::ThreadSafe>
					InState)
				: State(MoveTemp(InState))
			{
			}

			FUeAgentQueryResult Submit(
				const FUeAgentQueryRequest& Request,
				const FUeAgentCallerContext& CallerContext,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation)
				override
			{
				if (IsInGameThread())
				{
					return ExecuteQuery(
						State, Request, CallerContext, Cancellation);
				}
				const TSharedRef<TOptional<FUeAgentQueryResult>, ESPMode::ThreadSafe>
					Result = MakeShared<
						TOptional<FUeAgentQueryResult>, ESPMode::ThreadSafe>();
				FEvent* Completed = FPlatformProcess::GetSynchEventFromPool(true);
				const auto StateRef = State;
				AsyncTask(
					ENamedThreads::GameThread,
					[StateRef, Request, CallerContext, Cancellation, Result, Completed]()
					{
						Result->Emplace(ExecuteQuery(
							StateRef, Request, CallerContext, Cancellation));
						Completed->Trigger();
					});
				Completed->Wait();
				FPlatformProcess::ReturnSynchEventToPool(Completed);
				return Result->GetValue();
			}

		private:
			TSharedRef<FGraphCompoundRuntime::FState, ESPMode::ThreadSafe> State;
		};

		class FOperationApplication final
			: public IUeAgentOperationApplicationPort
		{
		public:
			explicit FOperationApplication(
				TSharedRef<FGraphCompoundRuntime::FState, ESPMode::ThreadSafe>
					InState)
				: State(MoveTemp(InState))
			{
			}

			FUeAgentOperationResult Submit(
				const FUeAgentOperationRequest& Request,
				const FUeAgentCallerContext& CallerContext,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation)
				override
			{
				if (IsInGameThread())
				{
					return ExecuteOperation(
						State, Request, CallerContext, Cancellation);
				}
				const TSharedRef<
					TOptional<FUeAgentOperationResult>, ESPMode::ThreadSafe> Result =
						MakeShared<
							TOptional<FUeAgentOperationResult>, ESPMode::ThreadSafe>();
				FEvent* Completed = FPlatformProcess::GetSynchEventFromPool(true);
				const auto StateRef = State;
				AsyncTask(
					ENamedThreads::GameThread,
					[StateRef, Request, CallerContext, Cancellation, Result, Completed]()
					{
						Result->Emplace(ExecuteOperation(
							StateRef, Request, CallerContext, Cancellation));
						Completed->Trigger();
					});
				Completed->Wait();
				FPlatformProcess::ReturnSynchEventToPool(Completed);
				return Result->GetValue();
			}

		private:
			TSharedRef<FGraphCompoundRuntime::FState, ESPMode::ThreadSafe> State;
		};
	}

	bool FGraphCompoundRuntimeConfig::IsValid() const
	{
		return MaxFrameBytes > 0;
	}

	FGraphCompoundRuntime::FGraphCompoundRuntime(
		TSharedRef<FState, ESPMode::ThreadSafe> InState,
		TSharedRef<IUeAgentQueryApplicationPort> InQueryPort,
		TSharedRef<IUeAgentOperationApplicationPort> InOperationPort)
		: State(MoveTemp(InState))
		, QueryPort(MoveTemp(InQueryPort))
		, OperationPort(MoveTemp(InOperationPort))
	{
	}

	bool FGraphCompoundRuntime::TryCreate(
		const FGraphCompoundRuntimeConfig& Config,
		TSharedRef<const FUeAgentDescriptorRegistry> DescriptorRegistry,
		TArray<TSharedRef<IGraphCompoundQueryBinding>> QueryBindings,
		TArray<TSharedRef<IGraphCompoundOperationBinding>> OperationBindings,
		TSharedPtr<FGraphCompoundRuntime>& OutRuntime,
		FGraphCompoundRuntimeError& OutError)
	{
		OutRuntime.Reset();
		OutError = {};
		if (!Config.IsValid())
		{
			SetError(
				OutError,
				TEXT("graph_compound_runtime_config_invalid"),
				TEXT("graph_compound.runtime"),
				TEXT("MaxFrameBytes must be positive."));
			return false;
		}

		TMap<FString, TSharedRef<IGraphCompoundQueryBinding>> QueryById;
		for (const TSharedRef<IGraphCompoundQueryBinding>& Binding
			: QueryBindings)
		{
			const FUeAgentDescriptorIdentity& Identity =
				Binding->GetDescriptorIdentity();
			const FString& DescriptorId = Identity.GetDescriptorId();
			if (Binding->GetOwnerId().TrimStartAndEnd().IsEmpty())
			{
				SetError(
					OutError,
					TEXT("graph_compound_binding_owner_missing"),
					DescriptorId,
					TEXT("Every exact query binding requires an explicit owner."));
				return false;
			}
			const FUeAgentOperationDescriptor* Descriptor =
				DescriptorRegistry->FindExact(Identity);
			if (!Descriptor)
			{
				SetError(
					OutError,
					TEXT("graph_compound_binding_descriptor_unknown"),
					DescriptorId,
					TEXT("The query binding identity is not exact in the registry."));
				return false;
			}
			if (Descriptor->GetOperationKind()
				!= EUeAgentDescriptorOperationKind::Query)
			{
				SetError(
					OutError,
					TEXT("graph_compound_binding_lane_mismatch"),
					DescriptorId,
					TEXT("A query binding cannot own an operation descriptor."));
				return false;
			}
			if (QueryById.Contains(DescriptorId))
			{
				SetError(
					OutError,
					TEXT("graph_compound_binding_duplicate"),
					DescriptorId,
					TEXT("The descriptor already has an exact query owner."));
				return false;
			}
			QueryById.Add(DescriptorId, Binding);
		}

		TMap<FString, TSharedRef<IGraphCompoundOperationBinding>> OperationById;
		for (const TSharedRef<IGraphCompoundOperationBinding>& Binding
			: OperationBindings)
		{
			const FUeAgentDescriptorIdentity& Identity =
				Binding->GetDescriptorIdentity();
			const FString& DescriptorId = Identity.GetDescriptorId();
			if (Binding->GetOwnerId().TrimStartAndEnd().IsEmpty())
			{
				SetError(
					OutError,
					TEXT("graph_compound_binding_owner_missing"),
					DescriptorId,
					TEXT("Every exact operation binding requires an explicit owner."));
				return false;
			}
			const FUeAgentOperationDescriptor* Descriptor =
				DescriptorRegistry->FindExact(Identity);
			if (!Descriptor)
			{
				SetError(
					OutError,
					TEXT("graph_compound_binding_descriptor_unknown"),
					DescriptorId,
					TEXT("The operation binding identity is not exact in the registry."));
				return false;
			}
			if (Descriptor->GetOperationKind()
				!= EUeAgentDescriptorOperationKind::Operation)
			{
				SetError(
					OutError,
					TEXT("graph_compound_binding_lane_mismatch"),
					DescriptorId,
					TEXT("An operation binding cannot own a query descriptor."));
				return false;
			}
			if (OperationById.Contains(DescriptorId))
			{
				SetError(
					OutError,
					TEXT("graph_compound_binding_duplicate"),
					DescriptorId,
					TEXT("The descriptor already has an exact operation owner."));
				return false;
			}
			OperationById.Add(DescriptorId, Binding);
		}

		for (const FUeAgentOperationDescriptor& Descriptor
			: DescriptorRegistry->GetDescriptors())
		{
			const FString& DescriptorId =
				Descriptor.GetIdentity().GetDescriptorId();
			if (Descriptor.GetOperationKind()
					== EUeAgentDescriptorOperationKind::Query
				&& !QueryById.Contains(DescriptorId))
			{
				SetError(
					OutError,
					TEXT("graph_compound_query_binding_missing"),
					DescriptorId,
					TEXT("The query descriptor has no exact native owner."));
				return false;
			}
			if (Descriptor.GetOperationKind()
					== EUeAgentDescriptorOperationKind::Operation
				&& !OperationById.Contains(DescriptorId))
			{
				SetError(
					OutError,
					TEXT("graph_compound_operation_binding_missing"),
					DescriptorId,
					TEXT("The operation descriptor has no exact native owner."));
				return false;
			}
		}

		const TSharedRef<FState, ESPMode::ThreadSafe> State =
			MakeShared<FState, ESPMode::ThreadSafe>(
				Config,
				DescriptorRegistry,
				MoveTemp(QueryById),
				MoveTemp(OperationById));
		const TSharedRef<IUeAgentQueryApplicationPort> QueryPort =
			MakeShared<FQueryApplication>(State);
		const TSharedRef<IUeAgentOperationApplicationPort> OperationPort =
			MakeShared<FOperationApplication>(State);
		OutRuntime = MakeShareable(new FGraphCompoundRuntime(
			State, QueryPort, OperationPort));
		return true;
	}

	const TSharedRef<IUeAgentQueryApplicationPort>&
	FGraphCompoundRuntime::GetQueryPort() const
	{
		return QueryPort;
	}

	const TSharedRef<IUeAgentOperationApplicationPort>&
	FGraphCompoundRuntime::GetOperationPort() const
	{
		return OperationPort;
	}

	int32 FGraphCompoundRuntime::GetQueryBindingCount() const
	{
		return State->QueryBindings.Num();
	}

	int32 FGraphCompoundRuntime::GetOperationBindingCount() const
	{
		return State->OperationBindings.Num();
	}
}
