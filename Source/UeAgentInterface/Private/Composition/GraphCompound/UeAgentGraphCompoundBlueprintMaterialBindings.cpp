#include "Composition/GraphCompound/UeAgentGraphCompoundBlueprintMaterialBindings.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeExit.h"

namespace UE::UAI::Composition::GraphCompound
{
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::PlanningPreflight;

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



		class FBlueprintMaterialQueryBinding final
			: public IGraphCompoundQueryBinding
		{
		public:
			FBlueprintMaterialQueryBinding(
				FGraphCompoundDomainRouteProfile InProfile,
				TSharedRef<IGraphCompoundBlueprintMaterialEnvironment>
					InEnvironment)
				: Profile(MoveTemp(InProfile))
				, Environment(MoveTemp(InEnvironment))
				, OwnerId(Profile.GetExecutionContract().GetBindingId())
			{
			}

			const FUeAgentDescriptorIdentity&
			GetDescriptorIdentity() const override
			{
				return Profile.GetExecutionContract().GetDescriptorIdentity();
			}

			const FString& GetOwnerId() const override { return OwnerId; }

			bool TryExecute(
				const FUeAgentQueryRequest& Request,
				const FUeAgentCallerContext& CallerContext,
				TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
					Resources,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation,
				FUeAgentContractValue& OutResult,
				FGraphCompoundRuntimeError& OutError) override
			{
				OutResult = FUeAgentContractValue(TEXT("{}"));
				OutError = {};
				if (Request.GetDescriptorIdentity() != GetDescriptorIdentity())
				{
					OutError = DescriptorMismatch(Request.GetDescriptorIdentity(), GetDescriptorIdentity());
					return false;
				}
				FGraphCompoundEventQueryRequest Decoded;
				FGraphCompoundRouteError DecodeError;
				if (!TryDecodeGraphCompoundEventQueryRequest(
						Profile, Request.GetPayload(), *Resources,
						Decoded, DecodeError))
				{
					SetError(OutError, DecodeError);
					return false;
				}
				return Environment->TryQuery(
					Profile, Decoded, CallerContext, MoveTemp(Resources),
					Cancellation, OutResult, OutError);
			}

		private:
			FGraphCompoundDomainRouteProfile Profile;
			TSharedRef<IGraphCompoundBlueprintMaterialEnvironment> Environment;
			FString OwnerId;
		};

		class FBlueprintMaterialOperationBinding final
			: public IGraphCompoundOperationBinding
		{
		public:
			FBlueprintMaterialOperationBinding(
				FGraphCompoundDomainRouteProfile InProfile,
				TSharedRef<IGraphCompoundBlueprintMaterialEnvironment>
					InEnvironment)
				: Profile(MoveTemp(InProfile))
				, Environment(MoveTemp(InEnvironment))
				, OwnerId(Profile.GetExecutionContract().GetBindingId())
			{
			}

			const FUeAgentDescriptorIdentity&
			GetDescriptorIdentity() const override
			{
				return Profile.GetExecutionContract().GetDescriptorIdentity();
			}

			const FString& GetOwnerId() const override { return OwnerId; }

			bool TryExecute(
				const FUeAgentOperationRequest& Request,
				const FUeAgentCallerContext& CallerContext,
				TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
					Resources,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation,
				FUeAgentContractValue& OutResult,
				FGraphCompoundRuntimeError& OutError) override
			{
				OutResult = FUeAgentContractValue(TEXT("{}"));
				OutError = {};
				if (Request.GetDescriptorIdentity() != GetDescriptorIdentity())
				{
					OutError = DescriptorMismatch(Request.GetDescriptorIdentity(), GetDescriptorIdentity());
					return false;
				}
				if (Profile.GetRouteKind()
					== EGraphCompoundDomainRouteKind::TypedOperation)
				{
					const bool bAssetCreate =
						Profile.GetTypedInputKind()
							== EGraphCompoundTypedInputKind::BlueprintAssetCreate
						|| Profile.GetTypedInputKind()
							== EGraphCompoundTypedInputKind::UmgAssetCreate
						|| Profile.GetTypedInputKind()
							== EGraphCompoundTypedInputKind::MaterialAssetCreate
						|| Profile.GetTypedInputKind()
							== EGraphCompoundTypedInputKind::MaterialInstanceAssetCreate
						|| Profile.GetTypedInputKind()
							== EGraphCompoundTypedInputKind::NiagaraEmitterAssetCreate
						|| Profile.GetTypedInputKind()
							== EGraphCompoundTypedInputKind::NiagaraSystemAssetCreate
						|| Profile.GetTypedInputKind()
							== EGraphCompoundTypedInputKind::NiagaraAssetDuplicate;
					const bool bCanonicalMutation =
						Profile.GetTypedInputKind()
							== EGraphCompoundTypedInputKind::GraphEdits
						|| Profile.GetTypedInputKind()
							== EGraphCompoundTypedInputKind::MaterialInstanceParameters
						|| Profile.GetTypedInputKind()
							== EGraphCompoundTypedInputKind::NiagaraCompileRefreshSettle
						|| Profile.GetTypedInputKind()
							== EGraphCompoundTypedInputKind::NiagaraStackReorder
						|| Profile.GetTypedInputKind()
							== EGraphCompoundTypedInputKind::NiagaraStackIssueFix
						|| Profile.GetTypedInputKind()
							== EGraphCompoundTypedInputKind::NiagaraCompoundInput
						|| Profile.GetTypedInputKind()
							== EGraphCompoundTypedInputKind::NiagaraRendererEventParameter;
					if (!bAssetCreate && !bCanonicalMutation)
					{
						OutError = BindingFailure(TEXT("graph_compound_production_binding_unimplemented"),
							TEXT("The graph binding does not implement this typed input kind."),
							Profile.GetCapabilityId(), TEXT("typed_input_kind"),
							MakeShared<FJsonValueNumber>(static_cast<int32>(Profile.GetTypedInputKind())),
							MakeShared<FJsonValueString>(TEXT("supported asset creation or canonical mutation kind")));
						return false;
					}
					if (bCanonicalMutation)
					{
						FString TargetRef;
						TOptional<FGraphCompoundNiagaraMutationSelector>
							NiagaraSelector;
						FGraphCompoundRouteError RouteError;
						const bool bNiagaraSelectedMutation =
							Profile.GetTypedInputKind()
								== EGraphCompoundTypedInputKind::NiagaraStackReorder
							|| Profile.GetTypedInputKind()
								== EGraphCompoundTypedInputKind::NiagaraStackIssueFix
							|| Profile.GetTypedInputKind()
								== EGraphCompoundTypedInputKind::NiagaraCompoundInput
							|| Profile.GetTypedInputKind()
								== EGraphCompoundTypedInputKind::NiagaraRendererEventParameter;
						if (bNiagaraSelectedMutation)
						{
							FGraphCompoundNiagaraMutationSelector Decoded;
							if (!TryDecodeGraphCompoundNiagaraMutationSelector(
									Profile, Request.GetPayload(), *Resources,
									Decoded, RouteError))
							{
								SetError(OutError, RouteError);
								return false;
							}
							TargetRef = Decoded.GetTargetRef();
							NiagaraSelector.Emplace(MoveTemp(Decoded));
						}
						else if (!TryDecodeGraphCompoundTypedOperationTargetRef(
							Profile, Request.GetPayload(), *Resources,
							TargetRef, RouteError))
						{
							SetError(OutError, RouteError);
							return false;
						}
						StateSemantic::FUeAgentCanonicalSnapshotPtr Snapshot;
						const bool bCaptured = Environment->TryCaptureTypedOperationSnapshot(
							Profile, TargetRef, NiagaraSelector, Resources, Cancellation, Snapshot, OutError);
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
						if (!TryDecodeGraphCompoundTypedOperation(
								Profile, Request.GetPayload(),
								Snapshot.ToSharedRef(), *Resources,
								Payload, RouteError))
						{
							SetError(OutError, RouteError);
							return false;
						}
						ON_SCOPE_EXIT { Payload.LocateDiagnostic(OutError); };
						TOptional<FChangeSetDraft> Draft;
						FGraphCompoundCanonicalError CanonicalError;
						const bool bBuilt = TryBuildGraphCompoundChangeSetDraft(
							Payload, Snapshot.ToSharedRef(), Request.GetDescriptorIdentity(),
							Request.GetRequestId(), Request.GetIdempotencyKey(), *Resources, Draft, CanonicalError);
						if (!bBuilt || !Draft.IsSet())
						{
							OutError = CanonicalError.IsValid() ? CanonicalError : BindingFailure(
								TEXT("graph_compound_draft_invalid"),
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
							OutError = PlanningError.IsValid() ? PlanningError
								: PlanningFailure(bPlanned, Planning, TargetRef);
							return false;
						}
						return Environment->TryExecuteTypedOperation(
							Profile, TargetRef, NiagaraSelector, Payload,
							Planning.GetPlan().ToSharedRef(), CallerContext,
							MoveTemp(Resources), Cancellation,
							OutResult, OutError);
					}
					FGraphCompoundAssetCreateRequest Decoded;
					FGraphCompoundRouteError RouteError;
					if (!TryDecodeGraphCompoundAssetCreateRequest(
							Profile, Request.GetPayload(), *Resources,
							Decoded, RouteError))
					{
						SetError(OutError, RouteError);
						return false;
					}
					StateSemantic::FUeAgentCanonicalSnapshotPtr Snapshot;
					const bool bCaptured = Environment->TryCaptureAssetCreateSnapshot(
						Profile, Decoded, Resources, Cancellation, Snapshot, OutError);
					if (!bCaptured || !Snapshot.IsValid())
					{
						if (OutError.IsEmpty())
						{
							OutError = BindingFailure(TEXT("graph_compound_snapshot_missing"),
								bCaptured ? TEXT("Snapshot capture returned without a graph snapshot.")
									: TEXT("Graph snapshot capture failed without a diagnostic."),
								Profile.GetCapabilityId(), bCaptured ? TEXT("snapshot.present") : TEXT("snapshot.capture_succeeded"),
								MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						}
						return false;
					}
					TOptional<FChangeSetDraft> Draft;
					const bool bBuilt = TryBuildGraphCompoundTypedOperationDraft(
						Profile, Request.GetDescriptorIdentity(), Request.GetPayload(), Snapshot.ToSharedRef(),
						Request.GetRequestId(), Request.GetIdempotencyKey(), *Resources, Draft, RouteError);
					if (!bBuilt || !Draft.IsSet())
					{
						OutError = RouteError.IsValid() ? RouteError : BindingFailure(
							TEXT("graph_compound_draft_invalid"),
							bBuilt ? TEXT("Graph change-set construction returned without a draft.")
								: TEXT("Graph change-set construction failed without a diagnostic."),
							Profile.GetCapabilityId(), bBuilt ? TEXT("draft.present") : TEXT("draft.build_succeeded"),
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
						OutError = PlanningError.IsValid() ? PlanningError
							: PlanningFailure(bPlanned, Planning, Profile.GetCapabilityId());
						return false;
					}
					return Environment->TryExecuteAssetCreate(
						Profile, Decoded, Planning.GetPlan().ToSharedRef(),
						CallerContext, MoveTemp(Resources), Cancellation,
						OutResult, OutError);
				}
				OutError = BindingFailure(TEXT("graph_compound_binding_route_kind_invalid"),
					TEXT("An operation binding requires a typed operation route."),
					Profile.GetCapabilityId(), TEXT("route_kind"),
					MakeShared<FJsonValueNumber>(static_cast<int32>(Profile.GetRouteKind())),
					MakeShared<FJsonValueNumber>(static_cast<int32>(EGraphCompoundDomainRouteKind::TypedOperation)));
				return false;
			}

		private:
			FGraphCompoundDomainRouteProfile Profile;
			TSharedRef<IGraphCompoundBlueprintMaterialEnvironment> Environment;
			FString OwnerId;
		};
	}

	bool TryCreateBlueprintMaterialGraphBindings(
		TSharedRef<IGraphCompoundBlueprintMaterialEnvironment> Environment,
		TArray<TSharedRef<IGraphCompoundQueryBinding>>& OutQueryBindings,
		TArray<TSharedRef<IGraphCompoundOperationBinding>>&
			OutOperationBindings,
		FGraphCompoundRuntimeError& OutError)
	{
		OutQueryBindings.Reset();
		OutOperationBindings.Reset();
		OutError = {};
		const TArray<FGraphCompoundDomainRouteProfile>* Profiles = nullptr;
		if (!TryGetBlueprintMaterialGraphRouteProfiles(Profiles, OutError)) return false;
		for (const FGraphCompoundDomainRouteProfile& Profile : *Profiles)
		{
			if (!Profile.IsValid())
			{
				OutError = Profile.GetValidationDiagnostic();
				OutQueryBindings.Reset();
				OutOperationBindings.Reset();
				return false;
			}
			if (Profile.GetRouteKind()
				== EGraphCompoundDomainRouteKind::Query)
			{
				OutQueryBindings.Add(
					MakeShared<FBlueprintMaterialQueryBinding>(
						Profile, Environment));
			}
			else
			{
				OutOperationBindings.Add(
					MakeShared<FBlueprintMaterialOperationBinding>(
						Profile, Environment));
			}
		}
		return true;
	}

	bool TryCreateBlueprintMaterialGraphProductionRuntime(
		const FGraphCompoundRuntimeConfig& RuntimeConfig,
		const FGraphCompoundBlueprintMaterialProductionConfig&
			ProductionConfig,
		TSharedPtr<FGraphCompoundRuntime>& OutRuntime,
		FGraphCompoundRuntimeError& OutError)
	{
		OutRuntime.Reset();
		TSharedPtr<IGraphCompoundBlueprintMaterialEnvironment> Environment;
		if (!TryCreateBlueprintMaterialGraphProductionEnvironment(
				ProductionConfig, Environment, OutError)
			|| !Environment.IsValid())
		{
			return false;
		}
		TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
		UE::UAI::Contract::FDiagnostic RegistryError;
		if (!TryCreateBlueprintMaterialGraphDescriptorRegistry(
				Registry, RegistryError)
			|| !Registry.IsValid())
		{
			SetError(OutError, RegistryError, TEXT("blueprint_material"));
			return false;
		}
		TArray<TSharedRef<IGraphCompoundQueryBinding>> QueryBindings;
		TArray<TSharedRef<IGraphCompoundOperationBinding>> OperationBindings;
		if (!TryCreateBlueprintMaterialGraphBindings(
				Environment.ToSharedRef(), QueryBindings,
				OperationBindings, OutError))
		{
			return false;
		}
		return FGraphCompoundRuntime::TryCreate(
			RuntimeConfig, Registry.ToSharedRef(), MoveTemp(QueryBindings),
			MoveTemp(OperationBindings), OutRuntime, OutError);
	}
}
