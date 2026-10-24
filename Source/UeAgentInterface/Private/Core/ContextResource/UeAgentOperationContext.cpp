#include "Core/ContextResource/UeAgentOperationContext.h"

namespace UE::UAI::ContextResource
{
	namespace
	{
		using namespace Contract;

		void Reset(
			FUeAgentExecutionContextRefPtr& OutContext,
			FUeAgentExecutionContextError& OutError)
		{
			OutContext.Reset();
			OutError = FUeAgentExecutionContextError();
		}

		void Reset(
			TSharedPtr<FUeAgentOperationContext>& OutContext,
			FUeAgentOperationContextError& OutError)
		{
			OutContext.Reset();
			OutError = FUeAgentOperationContextError();
		}

		bool Fail(
			const TCHAR* Code,
			const FString& Reason,
			FUeAgentExecutionContextError& OutError,
			const FString& Property,
			const EUeAgentExecutionContextFacet Facet =
				EUeAgentExecutionContextFacet::Invalid)
		{
			OutError = FDiagnostic::Failure(Code, Reason, TEXT("context"));
			OutError.Schema = TEXT("uai.runtime.diagnostic/1");
			OutError.PropertyPath = Property;
			OutError.Facet = Facet;
			return false;
		}

		

		void Normalize(FString& Value)
		{
			Value.TrimStartAndEndInline();
		}

		bool NormalizeIdentities(TArray<FString>& Values, const FString& Property,
			FUeAgentExecutionContextError& OutError)
		{
			TSet<FString> Seen;
			for (int32 Index = 0; Index < Values.Num(); ++Index)
			{
				FString& Value = Values[Index];
				Normalize(Value);
				if (Value.IsEmpty() || Seen.Contains(Value))
				{
					return Fail(TEXT("execution_context_identity_invalid"),
						Value.IsEmpty() ? TEXT("The execution context contains an empty identity.")
							: TEXT("The execution context contains a duplicate identity."),
						OutError, FString::Printf(TEXT("%s[%d]"), *Property, Index));
				}
				Seen.Add(Value);
			}
			return true;
		}

		const TCHAR* FacetProperty(const EUeAgentExecutionContextFacet Facet)
		{
			switch (Facet)
			{
			case EUeAgentExecutionContextFacet::World: return TEXT("world");
			case EUeAgentExecutionContextFacet::Level: return TEXT("level");
			case EUeAgentExecutionContextFacet::DataLayer: return TEXT("data_layers");
			case EUeAgentExecutionContextFacet::WorldPartition: return TEXT("world_partition");
			case EUeAgentExecutionContextFacet::PieInstance: return TEXT("pie_instance_id");
			case EUeAgentExecutionContextFacet::NetMode: return TEXT("net_mode");
			case EUeAgentExecutionContextFacet::NetRole: return TEXT("net_role");
			case EUeAgentExecutionContextFacet::AssetEditor: return TEXT("asset_editor");
			case EUeAgentExecutionContextFacet::Viewport: return TEXT("viewport");
			case EUeAgentExecutionContextFacet::Selection: return TEXT("selection");
			default: return TEXT("required_facets");
			}
		}

		bool HasFacet(
			const FUeAgentExecutionContextInput& Input,
			const EUeAgentExecutionContextFacet Facet)
		{
			switch (Facet)
			{
			case EUeAgentExecutionContextFacet::World:
				return Input.WorldType
						!= EUeAgentExecutionWorldType::Invalid
					&& !Input.WorldIdentity.IsEmpty();
			case EUeAgentExecutionContextFacet::Level:
				return !Input.LevelIdentity.IsEmpty();
			case EUeAgentExecutionContextFacet::DataLayer:
				return !Input.DataLayerIdentities.IsEmpty();
			case EUeAgentExecutionContextFacet::WorldPartition:
				return !Input.WorldPartitionIdentity.IsEmpty();
			case EUeAgentExecutionContextFacet::PieInstance:
				return Input.PieInstanceId.IsSet()
					&& Input.PieInstanceId.GetValue() >= 0;
			case EUeAgentExecutionContextFacet::NetMode:
				return !Input.NetMode.IsEmpty();
			case EUeAgentExecutionContextFacet::NetRole:
				return !Input.NetRole.IsEmpty();
			case EUeAgentExecutionContextFacet::AssetEditor:
				return !Input.AssetEditorIdentity.IsEmpty();
			case EUeAgentExecutionContextFacet::Viewport:
				return !Input.ViewportIdentity.IsEmpty();
			case EUeAgentExecutionContextFacet::Selection:
				return !Input.SelectionIdentities.IsEmpty();
			default:
				return false;
			}
		}

	}

	bool FUeAgentOperationContext::TryCreateInternal(
		const Contract::FUeAgentDescriptorIdentity& DescriptorIdentity,
		const EUeAgentRequestKind RequestKind,
		const FString& RequestId,
		const FString& IdempotencyKey,
		const Contract::FUeAgentCallerContext& CallerContext,
		FUeAgentExecutionContextRefOwner ExecutionContext,
		FUeAgentResourceTrackerOwner ResourceTracker,
		FUeAgentOperationDeadline Deadline,
		FUeAgentCancellationToken CancellationToken,
		TSharedPtr<FUeAgentOperationContext>& OutContext,
		FUeAgentOperationContextError& OutError)
	{
			Reset(OutContext, OutError);
			if (!DescriptorIdentity.IsValid()
				|| RequestKind == EUeAgentRequestKind::Invalid
				|| RequestId.IsEmpty()
				|| (RequestKind == EUeAgentRequestKind::Operation
					&& IdempotencyKey.IsEmpty()))
			{
				return Fail(
					TEXT("operation_context_request_invalid"),
					TEXT("The operation context requires a valid descriptor, request kind and request ID; operations also require an idempotency key."),
					OutError, !DescriptorIdentity.IsValid() ? TEXT("descriptor") :
						RequestKind == EUeAgentRequestKind::Invalid ? TEXT("request_kind") :
						RequestId.IsEmpty() ? TEXT("request_id") : TEXT("idempotency_key"));
			}
			if (!CallerContext.IsValid())
			{
				return Fail(
					TEXT("operation_context_caller_invalid"),
					TEXT("The caller context is not valid for operation execution."), OutError, TEXT("caller"));
			}
			if (!(ExecutionContext->GetDescriptorIdentity()
					== DescriptorIdentity))
			{
				return Fail(
					TEXT("operation_context_descriptor_mismatch"),
					TEXT("The execution context belongs to a different descriptor than the request."), OutError, TEXT("descriptor"));
			}
			if (ExecutionContext->GetRequestKind() != RequestKind)
			{
				return Fail(
					TEXT("operation_context_request_kind_mismatch"),
					TEXT("The execution context request kind does not match the request."), OutError, TEXT("request_kind"));
			}

			OutContext = MakeShareable(new FUeAgentOperationContext(
				DescriptorIdentity,
				RequestKind,
				RequestId,
				IdempotencyKey,
				CallerContext,
				MoveTemp(ExecutionContext),
				MoveTemp(ResourceTracker),
				MoveTemp(Deadline),
				MoveTemp(CancellationToken)));
			return true;
	}

	bool FUeAgentExecutionContextRef::TryCreate(
		const FUeAgentExecutionContextInput& Input,
		const Contract::FUeAgentDescriptorIdentity& DescriptorIdentity,
		const EUeAgentRequestKind RequestKind,
		const Contract::FUeAgentExecutionContextPolicy& Policy,
		FUeAgentExecutionContextRefPtr& OutContext,
		FUeAgentExecutionContextError& OutError)
	{
		using namespace Contract;
		Reset(OutContext, OutError);
		if (!DescriptorIdentity.IsValid()
			|| RequestKind == EUeAgentRequestKind::Invalid
			|| !Policy.IsValid())
		{
			return Fail(
				TEXT("execution_context_contract_invalid"),
				TEXT("The execution context requires a valid descriptor, request kind and context policy."), OutError,
				!DescriptorIdentity.IsValid() ? TEXT("descriptor") :
					RequestKind == EUeAgentRequestKind::Invalid ? TEXT("request_kind") : TEXT("context_policy"));
		}

		FUeAgentExecutionContextInput Frozen = Input;
		Normalize(Frozen.ProjectInstanceId);
		Normalize(Frozen.EditorInstanceId);
		Normalize(Frozen.WorldIdentity);
		Normalize(Frozen.LevelIdentity);
		Normalize(Frozen.WorldPartitionIdentity);
		Normalize(Frozen.NetMode);
		Normalize(Frozen.NetRole);
		Normalize(Frozen.AssetEditorIdentity);
		Normalize(Frozen.ViewportIdentity);
		Normalize(Frozen.ContextRevision);
		if (!NormalizeIdentities(Frozen.DataLayerIdentities, TEXT("data_layers"), OutError)
			|| !NormalizeIdentities(Frozen.SelectionIdentities, TEXT("selection"), OutError))
		{
			return false;
		}
		if (Frozen.ProjectInstanceId.IsEmpty()
			|| Frozen.EditorInstanceId.IsEmpty()
			|| Frozen.ContextRevision.IsEmpty()
			|| Frozen.StatePlane == EUeAgentStatePlane::Invalid)
		{
			return Fail(
				TEXT("execution_context_input_invalid"),
				TEXT("The execution context is missing a required identity or state plane."), OutError,
				Frozen.ProjectInstanceId.IsEmpty() ? TEXT("project_instance_id") :
					Frozen.EditorInstanceId.IsEmpty() ? TEXT("editor_instance_id") :
					Frozen.ContextRevision.IsEmpty() ? TEXT("context_revision") : TEXT("state_plane"));
		}
		if (!Policy.GetAllowedStatePlanes().Contains(Frozen.StatePlane))
		{
			return Fail(
				TEXT("execution_context_state_plane_not_allowed"),
				TEXT("The requested state plane is not allowed by the descriptor execution policy."), OutError, TEXT("state_plane"));
		}
		if (Policy.GetResolutionPolicy()
				== EUeAgentContextResolutionPolicy::ExplicitOnly
			&& Frozen.bResolvedFromCurrent)
		{
			return Fail(
				TEXT(
					"execution_context_resolve_current_not_allowed"),
				TEXT("The descriptor requires an explicit execution context; resolving the current context is not allowed."), OutError, TEXT("resolved_from_current"));
		}
		if ((Frozen.WorldType == EUeAgentExecutionWorldType::Pie
				|| Frozen.WorldType
					== EUeAgentExecutionWorldType::SimulateInEditor)
			&& (!Frozen.PieInstanceId.IsSet()
				|| Frozen.PieInstanceId.GetValue() < 0))
		{
			return Fail(
				TEXT("execution_context_pie_instance_required"),
				TEXT("A PIE or simulate-in-editor context requires a non-negative PIE instance ID."), OutError, TEXT("pie_instance_id"),
				EUeAgentExecutionContextFacet::PieInstance);
		}
		if (Frozen.WorldType == EUeAgentExecutionWorldType::Preview
			&& RequestKind == EUeAgentRequestKind::Operation
			&& Policy.GetTransientContextPolicy()
				!= EUeAgentTransientContextPolicy::
					TemporaryMutationAllowed)
		{
			return Fail(
				TEXT(
					"execution_context_transient_mutation_not_allowed"),
				TEXT("The descriptor does not allow mutations in a transient preview world."), OutError, TEXT("world_type"));
		}
		for (const EUeAgentExecutionContextFacet Facet :
			Policy.GetRequiredFacets())
		{
			if (!HasFacet(Frozen, Facet))
			{
				return Fail(
					TEXT("execution_context_required_facet_missing"),
					TEXT("A required execution context facet is missing or invalid."), OutError, FacetProperty(Facet),
					Facet);
			}
		}

		OutContext = FUeAgentExecutionContextRefPtr(
			new FUeAgentExecutionContextRef(
				MoveTemp(Frozen),
				DescriptorIdentity,
				RequestKind));
		return true;
	}

	FUeAgentExecutionContextRef::FUeAgentExecutionContextRef(
		FUeAgentExecutionContextInput InInput,
		Contract::FUeAgentDescriptorIdentity InDescriptorIdentity,
		const EUeAgentRequestKind InRequestKind)
		: Input(MoveTemp(InInput))
		, DescriptorIdentity(MoveTemp(InDescriptorIdentity))
		, RequestKind(InRequestKind)
	{
	}

	const FString&
	FUeAgentExecutionContextRef::GetProjectInstanceId() const
	{
		return Input.ProjectInstanceId;
	}

	const FString&
	FUeAgentExecutionContextRef::GetEditorInstanceId() const
	{
		return Input.EditorInstanceId;
	}

	Contract::EUeAgentStatePlane
	FUeAgentExecutionContextRef::GetStatePlane() const
	{
		return Input.StatePlane;
	}

	EUeAgentExecutionWorldType
	FUeAgentExecutionContextRef::GetWorldType() const
	{
		return Input.WorldType;
	}

	const FString& FUeAgentExecutionContextRef::GetWorldIdentity() const
	{
		return Input.WorldIdentity;
	}

	const FString& FUeAgentExecutionContextRef::GetLevelIdentity() const
	{
		return Input.LevelIdentity;
	}

	const TArray<FString>&
	FUeAgentExecutionContextRef::GetDataLayerIdentities() const
	{
		return Input.DataLayerIdentities;
	}

	const FString&
	FUeAgentExecutionContextRef::GetWorldPartitionIdentity() const
	{
		return Input.WorldPartitionIdentity;
	}

	const TOptional<int32>&
	FUeAgentExecutionContextRef::GetPieInstanceId() const
	{
		return Input.PieInstanceId;
	}

	const FString& FUeAgentExecutionContextRef::GetNetMode() const
	{
		return Input.NetMode;
	}

	const FString& FUeAgentExecutionContextRef::GetNetRole() const
	{
		return Input.NetRole;
	}

	const FString&
	FUeAgentExecutionContextRef::GetAssetEditorIdentity() const
	{
		return Input.AssetEditorIdentity;
	}

	const FString&
	FUeAgentExecutionContextRef::GetViewportIdentity() const
	{
		return Input.ViewportIdentity;
	}

	const TArray<FString>&
	FUeAgentExecutionContextRef::GetSelectionIdentities() const
	{
		return Input.SelectionIdentities;
	}

	const FString&
	FUeAgentExecutionContextRef::GetContextRevision() const
	{
		return Input.ContextRevision;
	}

	bool FUeAgentExecutionContextRef::WasResolvedFromCurrent() const
	{
		return Input.bResolvedFromCurrent;
	}

	const Contract::FUeAgentDescriptorIdentity&
	FUeAgentExecutionContextRef::GetDescriptorIdentity() const
	{
		return DescriptorIdentity;
	}

	EUeAgentRequestKind
	FUeAgentExecutionContextRef::GetRequestKind() const
	{
		return RequestKind;
	}

	bool FUeAgentExecutionContextRef::TryValidateRevision(
		const FString& ObservedRevision,
		FUeAgentExecutionContextError& OutError) const
	{
		OutError = FUeAgentExecutionContextError();
		FString Normalized = ObservedRevision;
		Normalize(Normalized);
		if (Normalized == Input.ContextRevision)
		{
			return true;
		}
		return Fail(
			TEXT("execution_context_revision_conflict"),
			TEXT("The execution context changed after it was captured; resolve the context again before retrying."), OutError, TEXT("context_revision"));
	}

	FUeAgentExactExecutionContextValidator::
		FUeAgentExactExecutionContextValidator(
			FUeAgentExecutionContextRefOwner InExpected)
		: Expected(MoveTemp(InExpected))
	{
	}

	bool FUeAgentExactExecutionContextValidator::TryValidate(
		const FUeAgentExecutionContextRef& Context,
		FUeAgentExecutionContextError& OutError) const
	{
		OutError = {};
		if (!Context.TryValidateRevision(
				Expected->GetContextRevision(),
				OutError))
		{
			return false;
		}
		const auto Matches = [&OutError](const bool bMatches, const TCHAR* Property)
		{
			return bMatches || Fail(TEXT("execution_context_identity_conflict"),
				TEXT("The captured execution context no longer matches the expected context."), OutError, Property);
		};
		if (!Matches(Context.GetDescriptorIdentity() == Expected->GetDescriptorIdentity(), TEXT("descriptor"))
			|| !Matches(Context.GetRequestKind() == Expected->GetRequestKind(), TEXT("request_kind"))
			|| !Matches(Context.GetProjectInstanceId() == Expected->GetProjectInstanceId(), TEXT("project_instance_id"))
			|| !Matches(Context.GetEditorInstanceId() == Expected->GetEditorInstanceId(), TEXT("editor_instance_id"))
			|| !Matches(Context.GetStatePlane() == Expected->GetStatePlane(), TEXT("state_plane"))
			|| !Matches(Context.GetWorldType() == Expected->GetWorldType(), TEXT("world_type"))
			|| !Matches(Context.GetWorldIdentity() == Expected->GetWorldIdentity(), TEXT("world"))
			|| !Matches(Context.GetLevelIdentity() == Expected->GetLevelIdentity(), TEXT("level"))
			|| !Matches(Context.GetDataLayerIdentities() == Expected->GetDataLayerIdentities(), TEXT("data_layers"))
			|| !Matches(Context.GetWorldPartitionIdentity() == Expected->GetWorldPartitionIdentity(), TEXT("world_partition"))
			|| !Matches(Context.GetPieInstanceId() == Expected->GetPieInstanceId(), TEXT("pie_instance_id"))
			|| !Matches(Context.GetNetMode() == Expected->GetNetMode(), TEXT("net_mode"))
			|| !Matches(Context.GetNetRole() == Expected->GetNetRole(), TEXT("net_role"))
			|| !Matches(Context.GetAssetEditorIdentity() == Expected->GetAssetEditorIdentity(), TEXT("asset_editor"))
			|| !Matches(Context.GetViewportIdentity() == Expected->GetViewportIdentity(), TEXT("viewport"))
			|| !Matches(Context.GetSelectionIdentities() == Expected->GetSelectionIdentities(), TEXT("selection"))
			|| !Matches(Context.WasResolvedFromCurrent() == Expected->WasResolvedFromCurrent(), TEXT("resolved_from_current")))
		{
			return false;
		}
		return true;
	}

	bool FUeAgentOperationContext::TryCreate(
		const Contract::FUeAgentQueryRequest& Request,
		const Contract::FUeAgentCallerContext& CallerContext,
		FUeAgentExecutionContextRefOwner ExecutionContext,
		FUeAgentResourceTrackerOwner ResourceTracker,
		FUeAgentOperationDeadline Deadline,
		FUeAgentCancellationToken CancellationToken,
		TSharedPtr<FUeAgentOperationContext>& OutContext,
		FUeAgentOperationContextError& OutError)
	{
		if (!Request.IsValid())
		{
			Reset(OutContext, OutError);
			return Fail(
				TEXT("operation_context_request_invalid"),
				TEXT("The query request does not contain a valid descriptor, request ID and input."), OutError, TEXT("query_request"));
		}
		return TryCreateInternal(
			Request.GetDescriptorIdentity(),
			EUeAgentRequestKind::Query,
			Request.GetRequestId(),
			FString(),
			CallerContext,
			MoveTemp(ExecutionContext),
			MoveTemp(ResourceTracker),
			MoveTemp(Deadline),
			MoveTemp(CancellationToken),
			OutContext,
			OutError);
	}

	bool FUeAgentOperationContext::TryCreate(
		const Contract::FUeAgentOperationRequest& Request,
		const Contract::FUeAgentCallerContext& CallerContext,
		FUeAgentExecutionContextRefOwner ExecutionContext,
		FUeAgentResourceTrackerOwner ResourceTracker,
		FUeAgentOperationDeadline Deadline,
		FUeAgentCancellationToken CancellationToken,
		TSharedPtr<FUeAgentOperationContext>& OutContext,
		FUeAgentOperationContextError& OutError)
	{
		if (!Request.IsValid())
		{
			Reset(OutContext, OutError);
			return Fail(
				TEXT("operation_context_request_invalid"),
				TEXT("The operation request does not contain a valid descriptor, request ID, idempotency key and input."), OutError, TEXT("operation_request"));
		}
		return TryCreateInternal(
			Request.GetDescriptorIdentity(),
			EUeAgentRequestKind::Operation,
			Request.GetRequestId(),
			Request.GetIdempotencyKey(),
			CallerContext,
			MoveTemp(ExecutionContext),
			MoveTemp(ResourceTracker),
			MoveTemp(Deadline),
			MoveTemp(CancellationToken),
			OutContext,
			OutError);
	}

	FUeAgentOperationContext::FUeAgentOperationContext(
		Contract::FUeAgentDescriptorIdentity InDescriptorIdentity,
		const EUeAgentRequestKind InRequestKind,
		FString InRequestId,
		FString InIdempotencyKey,
		Contract::FUeAgentCallerContext InCallerContext,
		FUeAgentExecutionContextRefOwner InExecutionContext,
		FUeAgentResourceTrackerOwner InResourceTracker,
		FUeAgentOperationDeadline InDeadline,
		FUeAgentCancellationToken InCancellationToken)
		: DescriptorIdentity(MoveTemp(InDescriptorIdentity))
		, RequestKind(InRequestKind)
		, RequestId(MoveTemp(InRequestId))
		, IdempotencyKey(MoveTemp(InIdempotencyKey))
		, CallerContext(MoveTemp(InCallerContext))
		, ExecutionContext(MoveTemp(InExecutionContext))
		, ResourceTracker(MoveTemp(InResourceTracker))
		, Deadline(MoveTemp(InDeadline))
		, CancellationToken(MoveTemp(InCancellationToken))
	{
	}

	const Contract::FUeAgentDescriptorIdentity&
	FUeAgentOperationContext::GetDescriptorIdentity() const
	{
		return DescriptorIdentity;
	}

	EUeAgentRequestKind
	FUeAgentOperationContext::GetRequestKind() const
	{
		return RequestKind;
	}

	const FString& FUeAgentOperationContext::GetRequestId() const
	{
		return RequestId;
	}

	const FString&
	FUeAgentOperationContext::GetIdempotencyKey() const
	{
		return IdempotencyKey;
	}

	const Contract::FUeAgentCallerContext&
	FUeAgentOperationContext::GetCallerContext() const
	{
		return CallerContext;
	}

	const FUeAgentExecutionContextRef&
	FUeAgentOperationContext::GetExecutionContext() const
	{
		return ExecutionContext.Get();
	}

	const FUeAgentExecutionContextRefOwner&
	FUeAgentOperationContext::GetExecutionContextOwner() const
	{
		return ExecutionContext;
	}

	FUeAgentResourceTracker&
	FUeAgentOperationContext::GetResourceTracker() const
	{
		return ResourceTracker.Get();
	}

	const FUeAgentResourceTrackerOwner&
	FUeAgentOperationContext::GetResourceTrackerOwner() const
	{
		return ResourceTracker;
	}

	const FUeAgentOperationDeadline&
	FUeAgentOperationContext::GetDeadline() const
	{
		return Deadline;
	}

	const FUeAgentCancellationToken&
	FUeAgentOperationContext::GetCancellationToken() const
	{
		return CancellationToken;
	}

	FUeAgentResourceScope FUeAgentOperationContext::Scope(
		FString Path) const
	{
		return ResourceTracker->Scope(MoveTemp(Path));
	}
}
