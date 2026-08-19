#include "Composition/GraphCompound/UeAgentGraphCompoundAudioMediaBindings.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"

namespace UE::UAI::Composition::GraphCompound
{
	using namespace ContextResource;
	using namespace Contract;
	using namespace PlanningPreflight;
	using namespace StateSemantic;

	namespace
	{
		void SetError(
			FGraphCompoundRuntimeError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError = {};
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
		}

		bool TryReadTarget(
			const FAudioMediaGraphRouteProfile& Profile,
			const FUeAgentContractValue& Input,
			FString& OutTargetRef,
			FGraphCompoundRuntimeError& OutError)
		{
			OutTargetRef.Reset();
			TSharedPtr<FJsonObject> Object;
			FString JsonError;
			if (!Json::TryParseObject(Input.GetJsonValue(), Object, JsonError)
				|| !Object.IsValid())
			{
				SetError(OutError, TEXT("graph_compound_typed_request_invalid"),
					Profile.GetCapabilityId(), JsonError);
				return false;
			}
			if (Profile.GetInputKind() == EAudioMediaGraphInputKind::AssetCreate)
			{
				FString AssetName;
				FString PackagePath;
				if (!Object->TryGetStringField(TEXT("asset_name"), AssetName)
					|| !Object->TryGetStringField(TEXT("package_path"), PackagePath))
				{
					SetError(OutError,
						TEXT("graph_compound_typed_request_invalid"),
						Profile.GetCapabilityId());
					return false;
				}
				AssetName.TrimStartAndEndInline();
				PackagePath.TrimStartAndEndInline();
				while (PackagePath.EndsWith(TEXT("/")))
				{
					PackagePath.LeftChopInline(1);
				}
				OutTargetRef = PackagePath + TEXT("/") + AssetName;
				if (AssetName.IsEmpty()
					|| !FPackageName::IsValidLongPackageName(OutTargetRef))
				{
					SetError(OutError,
						TEXT("graph_compound_audio_media_target_invalid"),
						OutTargetRef);
					return false;
				}
				return true;
			}
			if (!Object->TryGetStringField(TEXT("target_ref"), OutTargetRef))
			{
				SetError(OutError, TEXT("graph_compound_typed_request_invalid"),
					Profile.GetCapabilityId());
				return false;
			}
			OutTargetRef.TrimStartAndEndInline();
			if (!FPackageName::IsValidLongPackageName(
					FPackageName::ObjectPathToPackageName(OutTargetRef)))
			{
				SetError(OutError,
					TEXT("graph_compound_audio_media_target_invalid"),
					OutTargetRef);
				return false;
			}
			return true;
		}

		bool TryPrepareOperationLifecycleInvocation(
			const FAudioMediaGraphRouteProfile& Profile,
			const TSharedRef<IGraphCompoundAudioMediaEnvironment>& Environment,
			const FUeAgentOperationRequest& Request,
			const FUeAgentCallerContext& CallerContext,
			TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources,
			const TSharedRef<IUeAgentCancellationSignal>& Cancellation,
			FGraphCompoundAudioMediaLifecycleInvocation& OutInvocation,
			FGraphCompoundRuntimeError& OutError)
		{
			OutInvocation = {};
			OutError = {};
			if (Request.GetDescriptorIdentity()
				!= Profile.GetExecutionContract().GetDescriptorIdentity())
			{
				SetError(OutError,
					TEXT("graph_compound_binding_descriptor_mismatch"),
					Profile.GetDescriptorId());
				return false;
			}
			FString TargetRef;
			if (!TryReadTarget(Profile, Request.GetPayload(), TargetRef, OutError))
			{
				return false;
			}
			const FGraphCompoundAudioMediaTargetRequest TargetRequest(
				TargetRef, Profile.GetTargetKind());
			FUeAgentCanonicalSnapshotPtr Snapshot;
			if (!TargetRequest.IsValid()
				|| !Environment->TryCaptureSnapshot(
					Profile, TargetRequest, Resources, Cancellation,
					Snapshot, OutError)
				|| !Snapshot.IsValid())
			{
				return false;
			}
			FGraphCompoundCanonicalPayload Payload(
				GetGraphCompoundCanonicalSchemaId(), {});
			FGraphCompoundRouteError RouteError;
			if (!TryDecodeAudioMediaGraphTypedOperation(
					Profile, Request.GetPayload(), Snapshot.ToSharedRef(),
					*Resources, Payload, RouteError))
			{
				SetError(OutError, RouteError.Code,
					RouteError.SubjectId, RouteError.Detail);
				return false;
			}
			const FGraphCompoundAudioMediaOperationRequest TypedRequest(
				TargetRequest, MoveTemp(Payload));
			TOptional<FChangeSetDraft> Draft;
			FGraphCompoundCanonicalError CanonicalError;
			if (!TypedRequest.IsValid()
				|| !TryBuildGraphCompoundChangeSetDraft(
					TypedRequest.GetPayload(), Snapshot.ToSharedRef(),
					Request.GetDescriptorIdentity(), Request.GetRequestId(),
					Request.GetIdempotencyKey(), *Resources,
					Draft, CanonicalError)
				|| !Draft.IsSet())
			{
				SetError(OutError,
					CanonicalError.Code.IsEmpty()
						? TEXT("graph_compound_audio_media_draft_invalid")
						: CanonicalError.Code,
					CanonicalError.IdentityKey, CanonicalError.Detail);
				return false;
			}
			FPlanningResult Planning;
			FPlanningPreflightError PlanningError;
			if (!TryPlanChanges(
					FPlanningRequest(
						MoveTemp(Draft.GetValue()), CallerContext, false),
					*Resources, Planning, PlanningError)
				|| !Planning.IsReady() || !Planning.GetPlan().IsValid())
			{
				SetError(OutError,
					PlanningError.GetCode().IsEmpty()
						? TEXT("graph_compound_planning_not_ready")
						: PlanningError.GetCode(),
					PlanningError.GetIdentityKey(), PlanningError.GetDetail());
				return false;
			}
			return Environment->TryCreateOperationLifecycleInvocation(
				Profile, TypedRequest, Planning.GetPlan().ToSharedRef(),
				CallerContext, MoveTemp(Resources), Cancellation,
				OutInvocation, OutError);
		}

		class FAudioMediaOperationBinding final
			: public IGraphCompoundOperationBinding
		{
		public:
			FAudioMediaOperationBinding(
				FAudioMediaGraphRouteProfile InProfile,
				TSharedRef<IGraphCompoundAudioMediaEnvironment> InEnvironment)
				: Profile(MoveTemp(InProfile))
				, Environment(MoveTemp(InEnvironment))
				, OwnerId(Profile.GetExecutionContract().GetBindingId())
			{
			}

			const FUeAgentDescriptorIdentity& GetDescriptorIdentity() const override
			{
				return Profile.GetExecutionContract().GetDescriptorIdentity();
			}
			const FString& GetOwnerId() const override { return OwnerId; }

			bool TryExecute(
				const FUeAgentOperationRequest& Request,
				const FUeAgentCallerContext& CallerContext,
				TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation,
				FUeAgentContractValue& OutResult,
				FGraphCompoundRuntimeError& OutError) override
			{
				OutResult = FUeAgentContractValue(TEXT("{}"));
				FGraphCompoundAudioMediaLifecycleInvocation Invocation;
				if (!TryPrepareOperationLifecycleInvocation(
						Profile, Environment, Request, CallerContext,
						MoveTemp(Resources), Cancellation, Invocation, OutError))
				{
					return false;
				}
				BindingGateway::Operation::FOperationLifecycleReceipt Receipt;
				if (!Invocation.GetService()->Run(
						*Invocation.GetPlan(), *Invocation.GetAuthorization(), Receipt))
				{
					SetError(OutError,
						Receipt.GetErrorCode().IsEmpty()
							? TEXT("graph_compound_audio_media_operation_failed")
							: Receipt.GetErrorCode(),
						Profile.GetCapabilityId(),
						BindingGateway::Operation::LexToString(Receipt.GetStatus()));
					return false;
				}
				return Invocation.GetContext()->TryBuildResult(
					Receipt, OutResult, OutError);
			}

		private:
			FAudioMediaGraphRouteProfile Profile;
			TSharedRef<IGraphCompoundAudioMediaEnvironment> Environment;
			FString OwnerId;
		};

		class FAudioMediaQueryBinding final : public IGraphCompoundQueryBinding
		{
		public:
			FAudioMediaQueryBinding(
				FAudioMediaGraphRouteProfile InProfile,
				TSharedRef<IGraphCompoundAudioMediaEnvironment> InEnvironment)
				: Profile(MoveTemp(InProfile))
				, Environment(MoveTemp(InEnvironment))
				, OwnerId(Profile.GetExecutionContract().GetBindingId())
			{
			}

			const FUeAgentDescriptorIdentity& GetDescriptorIdentity() const override
			{
				return Profile.GetExecutionContract().GetDescriptorIdentity();
			}
			const FString& GetOwnerId() const override { return OwnerId; }

			bool TryExecute(
				const FUeAgentQueryRequest& Request,
				const FUeAgentCallerContext& CallerContext,
				TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation,
				FUeAgentContractValue& OutResult,
				FGraphCompoundRuntimeError& OutError) override
			{
				OutResult = FUeAgentContractValue(TEXT("{}"));
				OutError = {};
				if (Request.GetDescriptorIdentity() != GetDescriptorIdentity())
				{
					SetError(OutError,
						TEXT("graph_compound_binding_descriptor_mismatch"),
						Profile.GetDescriptorId());
					return false;
				}
				FAudioMediaGraphArtifactQueryRequest TypedRequest;
				FGraphCompoundRouteError RouteError;
				if (!TryDecodeAudioMediaGraphArtifactQueryRequest(
						Profile, Request.GetPayload(), *Resources,
						TypedRequest, RouteError))
				{
					SetError(OutError, RouteError.Code,
						RouteError.SubjectId, RouteError.Detail);
					return false;
				}
				return Environment->TryExecuteQuery(
					Profile, TypedRequest, CallerContext, MoveTemp(Resources),
					Cancellation, OutResult, OutError);
			}

		private:
			FAudioMediaGraphRouteProfile Profile;
			TSharedRef<IGraphCompoundAudioMediaEnvironment> Environment;
			FString OwnerId;
		};
	}

	bool FGraphCompoundAudioMediaLifecycleInvocation::TryCreate(
		TSharedRef<const BindingGateway::Operation::FOperationLifecyclePlan>
			InPlan,
		TSharedRef<const BindingGateway::Operation::
			FOperationImpactAuthorization> InAuthorization,
		TSharedRef<BindingGateway::Operation::FOperationLifecycleService>
			InService,
		TSharedRef<IGraphCompoundAudioMediaLifecycleContext> InContext,
		FGraphCompoundAudioMediaLifecycleInvocation& OutInvocation,
		FGraphCompoundRuntimeError& OutError)
	{
		OutInvocation = {};
		OutError = {};
		if (!InPlan->IsValid() || !InAuthorization->IsValid()
			|| !InAuthorization->Authorizes(*InPlan))
		{
			SetError(OutError,
				TEXT("graph_compound_audio_media_lifecycle_invalid"),
				InPlan->GetPlanId());
			return false;
		}
		OutInvocation.Plan = MoveTemp(InPlan);
		OutInvocation.Authorization = MoveTemp(InAuthorization);
		OutInvocation.Service = MoveTemp(InService);
		OutInvocation.Context = MoveTemp(InContext);
		return true;
	}

	bool FGraphCompoundAudioMediaLifecycleInvocation::IsValid() const
	{
		return Plan.IsValid() && Authorization.IsValid()
			&& Service.IsValid() && Context.IsValid();
	}

	TSharedRef<const BindingGateway::Operation::FOperationLifecyclePlan>
	FGraphCompoundAudioMediaLifecycleInvocation::GetPlan() const
	{
		return Plan.ToSharedRef();
	}

	TSharedRef<const BindingGateway::Operation::FOperationImpactAuthorization>
	FGraphCompoundAudioMediaLifecycleInvocation::GetAuthorization() const
	{
		return Authorization.ToSharedRef();
	}

	TSharedRef<BindingGateway::Operation::FOperationLifecycleService>
	FGraphCompoundAudioMediaLifecycleInvocation::GetService() const
	{
		return Service.ToSharedRef();
	}

	TSharedRef<IGraphCompoundAudioMediaLifecycleContext>
	FGraphCompoundAudioMediaLifecycleInvocation::GetContext() const
	{
		return Context.ToSharedRef();
	}

	FGraphCompoundAudioMediaTargetRequest::
	FGraphCompoundAudioMediaTargetRequest(
		FString InTargetRef,
		FString InTargetKind)
		: TargetRef(MoveTemp(InTargetRef))
		, TargetKind(MoveTemp(InTargetKind))
	{
		TargetRef.TrimStartAndEndInline();
		TargetKind.TrimStartAndEndInline();
		bValid = !TargetRef.IsEmpty() && !TargetKind.IsEmpty();
	}

	const FString& FGraphCompoundAudioMediaTargetRequest::GetTargetRef() const
	{
		return TargetRef;
	}
	const FString& FGraphCompoundAudioMediaTargetRequest::GetTargetKind() const
	{
		return TargetKind;
	}
	bool FGraphCompoundAudioMediaTargetRequest::IsValid() const
	{
		return bValid;
	}

	FGraphCompoundAudioMediaOperationRequest::
	FGraphCompoundAudioMediaOperationRequest(
		FGraphCompoundAudioMediaTargetRequest InTarget,
		FGraphCompoundCanonicalPayload InPayload)
		: Target(MoveTemp(InTarget))
		, Payload(MoveTemp(InPayload))
	{
		bValid = Target.IsValid()
			&& Payload.GetSchemaId() == GetGraphCompoundCanonicalSchemaId()
			&& !Payload.GetEdits().IsEmpty();
	}

	const FGraphCompoundAudioMediaTargetRequest&
	FGraphCompoundAudioMediaOperationRequest::GetTarget() const
	{
		return Target;
	}
	const FString& FGraphCompoundAudioMediaOperationRequest::GetTargetRef() const
	{
		return Target.GetTargetRef();
	}
	const FString& FGraphCompoundAudioMediaOperationRequest::GetTargetKind() const
	{
		return Target.GetTargetKind();
	}
	const FGraphCompoundCanonicalPayload&
	FGraphCompoundAudioMediaOperationRequest::GetPayload() const
	{
		return Payload;
	}
	bool FGraphCompoundAudioMediaOperationRequest::IsValid() const
	{
		return bValid;
	}

	bool FGraphCompoundAudioMediaProductionConfig::IsValid() const
	{
		return FMath::IsFinite(OperationTimeoutSeconds)
			&& OperationTimeoutSeconds > 0.0;
	}

	bool TryCreateAudioMediaGraphBindings(
		TSharedRef<IGraphCompoundAudioMediaEnvironment> Environment,
		TArray<TSharedRef<IGraphCompoundQueryBinding>>& OutQueryBindings,
		TArray<TSharedRef<IGraphCompoundOperationBinding>>& OutOperationBindings,
		FGraphCompoundRuntimeError& OutError)
	{
		OutQueryBindings.Reset();
		OutOperationBindings.Reset();
		OutError = {};
		for (const FAudioMediaGraphRouteProfile& Profile
			: GetAudioMediaGraphRouteProfiles())
		{
			if (!Profile.IsValid())
			{
				SetError(OutError, TEXT("graph_compound_binding_profile_invalid"),
					Profile.GetCapabilityId());
				return false;
			}
			if (Profile.GetRouteKind() == EGraphCompoundDomainRouteKind::Query)
			{
				OutQueryBindings.Add(
					MakeShared<FAudioMediaQueryBinding>(Profile, Environment));
			}
			else
			{
				OutOperationBindings.Add(
					MakeShared<FAudioMediaOperationBinding>(Profile, Environment));
			}
		}
		if (OutQueryBindings.Num() != 1 || OutOperationBindings.Num() != 14)
		{
			SetError(OutError, TEXT("graph_compound_binding_count_invalid"),
				TEXT("audio_media_graph"));
			OutQueryBindings.Reset();
			OutOperationBindings.Reset();
			return false;
		}
		return true;
	}

	bool TryCreateAudioMediaGraphProductionRuntime(
		const FGraphCompoundRuntimeConfig& RuntimeConfig,
		const FGraphCompoundAudioMediaProductionConfig& ProductionConfig,
		TSharedPtr<FGraphCompoundRuntime>& OutRuntime,
		FGraphCompoundRuntimeError& OutError)
	{
		OutRuntime.Reset();
		TSharedPtr<IGraphCompoundAudioMediaEnvironment> Environment;
		if (!TryCreateAudioMediaGraphProductionEnvironment(
				ProductionConfig, Environment, OutError)
			|| !Environment.IsValid())
		{
			return false;
		}
		TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
		FString RegistryError;
		if (!TryCreateAudioMediaGraphRouteDescriptorRegistry(
				Registry, RegistryError)
			|| !Registry.IsValid())
		{
			SetError(OutError,
				TEXT("graph_compound_domain_registry_invalid"),
				TEXT("audio_media_graph"), RegistryError);
			return false;
		}
		TArray<TSharedRef<IGraphCompoundQueryBinding>> QueryBindings;
		TArray<TSharedRef<IGraphCompoundOperationBinding>> OperationBindings;
		if (!TryCreateAudioMediaGraphBindings(
				Environment.ToSharedRef(), QueryBindings, OperationBindings, OutError))
		{
			return false;
		}
		return FGraphCompoundRuntime::TryCreate(
			RuntimeConfig, Registry.ToSharedRef(), MoveTemp(QueryBindings),
			MoveTemp(OperationBindings), OutRuntime, OutError);
	}

	bool TryCreateAudioMediaGraphOperationLifecycleInvocation(
		const FString& CommandId,
		const CommandEdge::FUeAgentCommandRequest& Request,
		const FUeAgentCallerContext& CallerContext,
		const TSharedRef<IUeAgentCancellationSignal>& Cancellation,
		const FGraphCompoundAudioMediaProductionConfig& ProductionConfig,
		FGraphCompoundAudioMediaLifecycleInvocation& OutInvocation,
		FGraphCompoundRuntimeError& OutError)
	{
		OutInvocation = {};
		OutError = {};
		const FAudioMediaGraphRouteProfile* Profile =
			GetAudioMediaGraphRouteProfiles().FindByPredicate(
				[&CommandId](const FAudioMediaGraphRouteProfile& Candidate)
				{
					return Candidate.GetCapabilityId() == CommandId;
				});
		if (!Profile
			|| Profile->GetRouteKind()
				!= EGraphCompoundDomainRouteKind::TypedOperation)
		{
			SetError(OutError,
				TEXT("graph_compound_audio_media_operation_profile_missing"),
				CommandId);
			return false;
		}
		TSharedPtr<IGraphCompoundAudioMediaEnvironment> Environment;
		if (!TryCreateAudioMediaGraphProductionEnvironment(
				ProductionConfig, Environment, OutError)
			|| !Environment.IsValid())
		{
			return false;
		}
		constexpr int64 MaxFrameBytes = 16 * 1024 * 1024;
		const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources =
			MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>(
				FUeAgentResourceBudget({
					{EUeAgentResourceDimension::InputBytes, MaxFrameBytes},
					{EUeAgentResourceDimension::OutputBytes, MaxFrameBytes},
					{EUeAgentResourceDimension::StringBytes, MaxFrameBytes},
					{EUeAgentResourceDimension::WorkUnits, MaxFrameBytes * 4},
					{EUeAgentResourceDimension::EstimatedMemoryBytes,
						MaxFrameBytes * 4},
				}));
		return TryPrepareOperationLifecycleInvocation(
			*Profile,
			Environment.ToSharedRef(),
			FUeAgentOperationRequest(
				Profile->GetExecutionContract().GetDescriptorIdentity(),
				Request.GetRequestId(),
				TEXT("uai-command:") + Request.GetRequestId(),
				FUeAgentContractValue(Request.GetParamsJson())),
			CallerContext,
			Resources,
			Cancellation,
			OutInvocation,
			OutError);
	}
}
