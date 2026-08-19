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
			FUeAgentExecutionContextError& OutError,
			const EUeAgentExecutionContextFacet Facet =
				EUeAgentExecutionContextFacet::Invalid)
		{
			OutError.Code = Code;
			OutError.Facet = Facet;
			return false;
		}

		bool Fail(
			const TCHAR* Code,
			FUeAgentOperationContextError& OutError)
		{
			OutError.Code = Code;
			return false;
		}

		void Normalize(FString& Value)
		{
			Value.TrimStartAndEndInline();
		}

		bool NormalizeIdentities(TArray<FString>& Values)
		{
			TSet<FString> Seen;
			for (FString& Value : Values)
			{
				Normalize(Value);
				if (Value.IsEmpty() || Seen.Contains(Value))
				{
					return false;
				}
				Seen.Add(Value);
			}
			return true;
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
					OutError);
			}
			if (!CallerContext.IsValid())
			{
				return Fail(
					TEXT("operation_context_caller_invalid"),
					OutError);
			}
			if (!(ExecutionContext->GetDescriptorIdentity()
					== DescriptorIdentity))
			{
				return Fail(
					TEXT("operation_context_descriptor_mismatch"),
					OutError);
			}
			if (ExecutionContext->GetRequestKind() != RequestKind)
			{
				return Fail(
					TEXT("operation_context_request_kind_mismatch"),
					OutError);
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
				OutError);
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
		if (!NormalizeIdentities(Frozen.DataLayerIdentities)
			|| !NormalizeIdentities(Frozen.SelectionIdentities))
		{
			return Fail(
				TEXT("execution_context_identity_invalid"),
				OutError);
		}
		if (Frozen.ProjectInstanceId.IsEmpty()
			|| Frozen.EditorInstanceId.IsEmpty()
			|| Frozen.ContextRevision.IsEmpty()
			|| Frozen.StatePlane == EUeAgentStatePlane::Invalid)
		{
			return Fail(
				TEXT("execution_context_input_invalid"),
				OutError);
		}
		if (!Policy.GetAllowedStatePlanes().Contains(Frozen.StatePlane))
		{
			return Fail(
				TEXT("execution_context_state_plane_not_allowed"),
				OutError);
		}
		if (Policy.GetResolutionPolicy()
				== EUeAgentContextResolutionPolicy::ExplicitOnly
			&& Frozen.bResolvedFromCurrent)
		{
			return Fail(
				TEXT(
					"execution_context_resolve_current_not_allowed"),
				OutError);
		}
		if ((Frozen.WorldType == EUeAgentExecutionWorldType::Pie
				|| Frozen.WorldType
					== EUeAgentExecutionWorldType::SimulateInEditor)
			&& (!Frozen.PieInstanceId.IsSet()
				|| Frozen.PieInstanceId.GetValue() < 0))
		{
			return Fail(
				TEXT("execution_context_pie_instance_required"),
				OutError,
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
				OutError);
		}
		for (const EUeAgentExecutionContextFacet Facet :
			Policy.GetRequiredFacets())
		{
			if (!HasFacet(Frozen, Facet))
			{
				return Fail(
					TEXT("execution_context_required_facet_missing"),
					OutError,
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
			OutError);
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
		if (!(Context.GetDescriptorIdentity()
				== Expected->GetDescriptorIdentity())
			|| Context.GetRequestKind()
				!= Expected->GetRequestKind()
			|| Context.GetProjectInstanceId()
				!= Expected->GetProjectInstanceId()
			|| Context.GetEditorInstanceId()
				!= Expected->GetEditorInstanceId()
			|| Context.GetStatePlane()
				!= Expected->GetStatePlane()
			|| Context.GetWorldType()
				!= Expected->GetWorldType()
			|| Context.GetWorldIdentity()
				!= Expected->GetWorldIdentity()
			|| Context.GetLevelIdentity()
				!= Expected->GetLevelIdentity()
			|| Context.GetDataLayerIdentities()
				!= Expected->GetDataLayerIdentities()
			|| Context.GetWorldPartitionIdentity()
				!= Expected->GetWorldPartitionIdentity()
			|| Context.GetPieInstanceId()
				!= Expected->GetPieInstanceId()
			|| Context.GetNetMode() != Expected->GetNetMode()
			|| Context.GetNetRole() != Expected->GetNetRole()
			|| Context.GetAssetEditorIdentity()
				!= Expected->GetAssetEditorIdentity()
			|| Context.GetViewportIdentity()
				!= Expected->GetViewportIdentity()
			|| Context.GetSelectionIdentities()
				!= Expected->GetSelectionIdentities()
			|| Context.WasResolvedFromCurrent()
				!= Expected->WasResolvedFromCurrent())
		{
			OutError.Code =
				TEXT("execution_context_identity_conflict");
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
				OutError);
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
				OutError);
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
