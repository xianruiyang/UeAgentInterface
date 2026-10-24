#include "Composition/GraphCompound/UeAgentGraphCompoundAudioMediaBindings.h"
#include "Misc/ScopeExit.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"

namespace UE::UAI::Composition::GraphCompound
{
	using namespace ContextResource;
	using namespace Contract;
	using namespace PlanningPreflight;
	using namespace StateSemantic;

	namespace
	{
		void SetError(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& Subject = FString())
        { OutError = MoveTemp(Diagnostic); if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject; }

		FDiagnostic BindingFailure(
			const TCHAR* Code, const FString& Reason, const FString& Subject,
			const FString& Field, TSharedPtr<FJsonValue> Actual,
			TSharedPtr<FJsonValue> Expected)
		{
			FDiagnostic Error = FDiagnostic::Failure(Code, Reason, TEXT("graph_compound"), FString(), Subject);
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Field;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}

		FDiagnostic DescriptorMismatch(
			const FUeAgentDescriptorIdentity& Actual,
			const FUeAgentDescriptorIdentity& Expected)
		{
			FDiagnostic Error = FDiagnostic::Failure(
				TEXT("graph_compound_binding_descriptor_mismatch"),
				TEXT("The request descriptor does not match the graph binding."),
				TEXT("graph_compound"), FString(), Expected.GetDescriptorId());
			Error.SourceKind = TEXT("native_result");
			if (Actual.GetDescriptorId() != Expected.GetDescriptorId())
			{
				Error.PropertyPath = TEXT("descriptor_identity.descriptor_id");
				Error.ActualValue = MakeShared<FJsonValueString>(Actual.GetDescriptorId());
				Error.ExpectedValue = MakeShared<FJsonValueString>(Expected.GetDescriptorId());
			}
			else if (Actual.GetVersion() != Expected.GetVersion())
			{
				Error.PropertyPath = TEXT("descriptor_identity.version");
				Error.ActualValue = MakeShared<FJsonValueNumber>(Actual.GetVersion());
				Error.ExpectedValue = MakeShared<FJsonValueNumber>(Expected.GetVersion());
			}
			else
			{
				Error.PropertyPath = TEXT("descriptor_identity.descriptor_hash");
				Error.ActualEvidence = MakeShared<FJsonValueString>(Actual.GetDescriptorHash());
				Error.ExpectedEvidence = MakeShared<FJsonValueString>(Expected.GetDescriptorHash());
			}
			return Error;
		}

		FDiagnostic PlanningFailure(
			const bool bPlanned, const FPlanningResult& Planning, const FString& Subject)
		{
			return BindingFailure(TEXT("graph_compound_planning_not_ready"),
				!bPlanned ? TEXT("Graph change planning failed without a diagnostic.")
					: !Planning.IsReady() ? TEXT("Graph change planning did not reach the ready state.")
					: TEXT("Graph change planning returned ready without a plan."),
				Subject, !bPlanned ? TEXT("planning.succeeded")
					: !Planning.IsReady() ? TEXT("planning.ready") : TEXT("planning.plan_present"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		}



		bool TryReadTarget(
			const FAudioMediaGraphRouteProfile& Profile,
			const FUeAgentContractValue& Input,
			FString& OutTargetRef,
			FGraphCompoundRuntimeError& OutError)
		{
			OutTargetRef.Reset();
			TSharedPtr<FJsonObject> Object;
			UE::UAI::Contract::FDiagnostic JsonError;
			if (!Json::TryParseObject(Input.GetJsonValue(), Object, JsonError)
				|| !Object.IsValid())
			{
				SetError(OutError, JsonError, Profile.GetCapabilityId());
				return false;
			}
			if (Profile.GetInputKind() == EAudioMediaGraphInputKind::AssetCreate)
			{
				FString AssetName;
				FString PackagePath;
				if (!Object->TryGetStringField(TEXT("asset_name"), AssetName)
					|| !Object->TryGetStringField(TEXT("package_path"), PackagePath))
				{
					FString Ignored;
					const TCHAR* Field = Object->TryGetStringField(TEXT("asset_name"), Ignored)
						? TEXT("package_path") : TEXT("asset_name");
					OutError = BindingFailure(TEXT("graph_compound_typed_request_invalid"),
						TEXT("The required asset creation field is missing or is not a string."),
						Profile.GetCapabilityId(), Field, Object->TryGetField(Field),
						MakeShared<FJsonValueString>(TEXT("string")));
					OutError.ActualValuePresent = Object->HasField(Field);
					OutError.SourceKind = TEXT("command_input");
					OutError.Source.JsonPath = FString(TEXT("/")) + Field;
					return false;
				}
				AssetName.TrimStartAndEndInline();
				PackagePath.TrimStartAndEndInline();
				while (PackagePath.EndsWith(TEXT("/")))
				{
					PackagePath.LeftChopInline(1);
				}
				OutTargetRef = PackagePath + TEXT("/") + AssetName;
				FText PackageReason;
				if (AssetName.IsEmpty()
					|| !FPackageName::IsValidLongPackageName(OutTargetRef, false, &PackageReason))
				{
					OutError = BindingFailure(TEXT("graph_compound_audio_media_target_invalid"),
						AssetName.IsEmpty() ? TEXT("asset_name is empty after trimming.") : PackageReason.ToString(),
						OutTargetRef, AssetName.IsEmpty() ? TEXT("asset_name") : TEXT("asset_path"),
						MakeShared<FJsonValueString>(AssetName.IsEmpty() ? AssetName : OutTargetRef),
						MakeShared<FJsonValueString>(AssetName.IsEmpty() ? TEXT("non-empty asset name") : TEXT("valid long package name")));
					OutError.SourceKind = TEXT("command_input");
					OutError.Source.JsonPath = AssetName.IsEmpty() ? TEXT("/asset_name") : TEXT("/");
					return false;
				}
				return true;
			}
			if (!Object->TryGetStringField(TEXT("target_ref"), OutTargetRef))
			{
				OutError = BindingFailure(TEXT("graph_compound_typed_request_invalid"),
					TEXT("target_ref is missing or is not a string."),
					Profile.GetCapabilityId(), TEXT("target_ref"), Object->TryGetField(TEXT("target_ref")),
					MakeShared<FJsonValueString>(TEXT("asset reference string")));
				OutError.ActualValuePresent = Object->HasField(TEXT("target_ref"));
				OutError.SourceKind = TEXT("command_input");
				OutError.Source.JsonPath = TEXT("/target_ref");
				return false;
			}
			OutTargetRef.TrimStartAndEndInline();
			FText PackageReason;
			if (!FPackageName::IsValidLongPackageName(
					FPackageName::ObjectPathToPackageName(OutTargetRef), false, &PackageReason))
			{
				OutError = BindingFailure(TEXT("graph_compound_audio_media_target_invalid"),
					PackageReason.ToString(), OutTargetRef, TEXT("target_ref"),
					MakeShared<FJsonValueString>(OutTargetRef),
					MakeShared<FJsonValueString>(TEXT("asset reference with a valid long package name")));
				OutError.SourceKind = TEXT("command_input");
				OutError.Source.JsonPath = TEXT("/target_ref");
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
				OutError = DescriptorMismatch(Request.GetDescriptorIdentity(), Profile.GetExecutionContract().GetDescriptorIdentity());
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
			if (!TargetRequest.IsValid())
			{
				OutError = TargetRequest.GetValidationDiagnostic();
				return false;
			}
			const bool bCaptured = Environment->TryCaptureSnapshot(
				Profile, TargetRequest, Resources, Cancellation, Snapshot, OutError);
			if (!bCaptured || !Snapshot.IsValid())
			{
				if (OutError.IsEmpty())
				{
					OutError = BindingFailure(TEXT("graph_compound_snapshot_missing"),
						bCaptured ? TEXT("Snapshot capture returned without a graph snapshot.")
							: TEXT("Graph snapshot capture failed without a diagnostic."),
						TargetRef, bCaptured ? TEXT("snapshot.present") : TEXT("snapshot.capture_succeeded"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				}
				return false;
			}
			FGraphCompoundCanonicalPayload Payload(
				GetGraphCompoundCanonicalSchemaId(), {});
			FGraphCompoundRouteError RouteError;
			if (!TryDecodeAudioMediaGraphTypedOperation(
					Profile, Request.GetPayload(), Snapshot.ToSharedRef(),
					*Resources, Payload, RouteError))
			{
				SetError(OutError, RouteError);
				return false;
			}
			const FGraphCompoundAudioMediaOperationRequest TypedRequest(
				TargetRequest, MoveTemp(Payload));
			ON_SCOPE_EXIT { TypedRequest.GetPayload().LocateDiagnostic(OutError); };
			TOptional<FChangeSetDraft> Draft;
			FGraphCompoundCanonicalError CanonicalError;
			if (!TypedRequest.IsValid())
			{
				OutError = TypedRequest.GetValidationDiagnostic();
				return false;
			}
			const bool bBuilt = TryBuildGraphCompoundChangeSetDraft(
				TypedRequest.GetPayload(), Snapshot.ToSharedRef(),
				Request.GetDescriptorIdentity(), Request.GetRequestId(),
				Request.GetIdempotencyKey(), *Resources, Draft, CanonicalError);
			if (!bBuilt || !Draft.IsSet())
			{
				OutError = CanonicalError.IsValid() ? CanonicalError : BindingFailure(
					TEXT("graph_compound_audio_media_draft_invalid"),
					bBuilt ? TEXT("Graph change-set construction returned without a draft.")
						: TEXT("Graph change-set construction failed without a diagnostic."),
					TargetRef, bBuilt ? TEXT("draft.present") : TEXT("draft.build_succeeded"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return false;
			}
			FPlanningResult Planning;
			FPlanningPreflightError PlanningError;
			const bool bPlanned = TryPlanChanges(
				FPlanningRequest(MoveTemp(Draft.GetValue()), CallerContext, false),
				*Resources, Planning, PlanningError);
			if (!bPlanned || !Planning.IsReady() || !Planning.GetPlan().IsValid())
			{
				OutError = PlanningError.IsValid() ? PlanningError : PlanningFailure(bPlanned, Planning, TargetRef);
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
					SetError(OutError, Receipt.GetDiagnostic().IsValid() ? Receipt.GetDiagnostic()
						: UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_audio_media_operation_failed"),
							TEXT("The audio operation did not complete."), TEXT("graph_compound")), Profile.GetCapabilityId());
					OutError.TerminalStatus = BindingGateway::Operation::LexToString(Receipt.GetStatus());
					OutError.MutationStarted = Receipt.DidMutationStart();
					OutError.bRetrySafe = Receipt.IsRetrySafe();
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
					auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_binding_descriptor_mismatch"),
						TEXT("The query descriptor does not match this audio/media binding."), TEXT("graph_compound"));
					Diagnostic.SourceKind = TEXT("native_result");
					Diagnostic.PropertyPath = TEXT("descriptor_matches");
					Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					SetError(OutError, MoveTemp(Diagnostic), Profile.GetDescriptorId());
					return false;
				}
				FAudioMediaGraphArtifactQueryRequest TypedRequest;
				FGraphCompoundRouteError RouteError;
				if (!TryDecodeAudioMediaGraphArtifactQueryRequest(
						Profile, Request.GetPayload(), *Resources,
						TypedRequest, RouteError))
				{
					SetError(OutError, RouteError);
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
		if (!InPlan->IsValid())
		{
			SetError(OutError, InPlan->GetValidationDiagnostic(), InPlan->GetPlanId());
			return false;
		}
		if (!InAuthorization->TryAuthorize(*InPlan, OutError))
		{
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = InPlan->GetPlanId();
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
		if (TargetRef.IsEmpty() || TargetKind.IsEmpty())
		{
			ValidationDiagnostic = BindingFailure(TEXT("graph_compound_audio_media_target_invalid"),
				TargetRef.IsEmpty() ? TEXT("The graph target reference is empty after trimming.")
					: TEXT("The graph route target kind is empty after trimming."),
				TargetRef, TargetRef.IsEmpty() ? TEXT("target_ref") : TEXT("target_kind"),
				MakeShared<FJsonValueString>(TargetRef.IsEmpty() ? TargetRef : TargetKind),
				MakeShared<FJsonValueString>(TEXT("non-empty string")));
			if (TargetRef.IsEmpty())
			{
				ValidationDiagnostic.SourceKind = TEXT("command_input");
				ValidationDiagnostic.Source.JsonPath = TEXT("/target_ref");
			}
		}
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
		return ValidationDiagnostic.IsEmpty();
	}

	const FDiagnostic& FGraphCompoundAudioMediaTargetRequest::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic;
	}

	FGraphCompoundAudioMediaOperationRequest::
	FGraphCompoundAudioMediaOperationRequest(
		FGraphCompoundAudioMediaTargetRequest InTarget,
		FGraphCompoundCanonicalPayload InPayload)
		: Target(MoveTemp(InTarget))
		, Payload(MoveTemp(InPayload))
	{
		if (!Target.IsValid())
		{
			ValidationDiagnostic = Target.GetValidationDiagnostic();
		}
		else if (Payload.GetSchemaId() != GetGraphCompoundCanonicalSchemaId())
		{
			ValidationDiagnostic = BindingFailure(TEXT("graph_compound_audio_media_draft_invalid"),
				TEXT("The graph operation payload uses an unexpected canonical schema."),
				Target.GetTargetRef(), TEXT("schema_id"),
				MakeShared<FJsonValueString>(Payload.GetSchemaId()),
				MakeShared<FJsonValueString>(GetGraphCompoundCanonicalSchemaId()));
		}
		else if (Payload.GetEdits().IsEmpty())
		{
			ValidationDiagnostic = BindingFailure(TEXT("graph_compound_audio_media_draft_invalid"),
				TEXT("The graph operation contains no canonical edits."),
				Target.GetTargetRef(), TEXT("edits.count"), MakeShared<FJsonValueNumber>(0),
				MakeShared<FJsonValueString>(TEXT("at least one edit")));
		}
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
		return ValidationDiagnostic.IsEmpty();
	}

	const FDiagnostic& FGraphCompoundAudioMediaOperationRequest::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic;
	}

	bool FTextureGraphSemanticCompositeRequest::IsValid() const
	{
		return !TargetRef.TrimStartAndEnd().IsEmpty()
			&& (!Nodes.IsEmpty() || !Parameters.IsEmpty() || !Links.IsEmpty() || !Settings.IsEmpty());
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
		const TArray<FAudioMediaGraphRouteProfile>* Profiles = nullptr;
		if (!TryGetAudioMediaGraphRouteProfiles(Profiles, OutError)) return false;
		for (const FAudioMediaGraphRouteProfile& Profile : *Profiles)
		{
			if (!Profile.IsValid())
			{
				OutError = Profile.GetValidationDiagnostic();
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
		UE::UAI::Contract::FDiagnostic RegistryError;
		if (!TryCreateAudioMediaGraphRouteDescriptorRegistry(
				Registry, RegistryError)
			|| !Registry.IsValid())
		{
			SetError(OutError, RegistryError, TEXT("audio_media_graph"));
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
		const TArray<FAudioMediaGraphRouteProfile>* Profiles = nullptr;
		if (!TryGetAudioMediaGraphRouteProfiles(Profiles, OutError)) return false;
		const FAudioMediaGraphRouteProfile* Profile =
			Profiles->FindByPredicate(
				[&CommandId](const FAudioMediaGraphRouteProfile& Candidate)
				{
					return Candidate.GetCapabilityId() == CommandId;
				});
		if (!Profile
			|| Profile->GetRouteKind()
				!= EGraphCompoundDomainRouteKind::TypedOperation)
		{
			OutError = BindingFailure(TEXT("graph_compound_audio_media_operation_profile_missing"),
				!Profile ? TEXT("No audio/media graph operation profile is registered for this command.")
					: TEXT("The selected audio/media graph route is not a typed operation."),
				CommandId, !Profile ? TEXT("profile.present") : TEXT("route_kind"),
				!Profile ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false))
					: TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(static_cast<int32>(Profile->GetRouteKind()))),
				!Profile ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true))
					: TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(static_cast<int32>(EGraphCompoundDomainRouteKind::TypedOperation))));
			return false;
		}
		TSharedPtr<IGraphCompoundAudioMediaEnvironment> Environment;
		if (!TryCreateAudioMediaGraphProductionEnvironment(
				ProductionConfig, Environment, OutError)
			|| !Environment.IsValid())
		{
			return false;
		}
		const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources =
			MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>();
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
