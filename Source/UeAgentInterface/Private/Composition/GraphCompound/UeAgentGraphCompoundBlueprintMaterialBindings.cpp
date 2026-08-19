#include "Composition/GraphCompound/UeAgentGraphCompoundBlueprintMaterialBindings.h"

#include "Composition/HjsonImport/UeAgentHjsonImportOperationRuntime.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"

namespace UE::UAI::Composition::GraphCompound
{
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::PlanningPreflight;

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
					SetError(OutError,
						TEXT("graph_compound_binding_descriptor_mismatch"),
						Profile.GetDescriptorId());
					return false;
				}
				FGraphCompoundEventQueryRequest Decoded;
				FGraphCompoundRouteError DecodeError;
				if (!TryDecodeGraphCompoundEventQueryRequest(
						Profile, Request.GetPayload(), *Resources,
						Decoded, DecodeError))
				{
					SetError(OutError, DecodeError.Code,
						DecodeError.SubjectId, DecodeError.Detail);
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
					SetError(OutError,
						TEXT("graph_compound_binding_descriptor_mismatch"),
						Profile.GetDescriptorId());
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
						SetError(OutError,
							TEXT("graph_compound_production_binding_unimplemented"),
							Profile.GetCapabilityId());
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
								SetError(OutError, RouteError.Code,
									RouteError.SubjectId, RouteError.Detail);
								return false;
							}
							TargetRef = Decoded.GetTargetRef();
							NiagaraSelector.Emplace(MoveTemp(Decoded));
						}
						else if (!TryDecodeGraphCompoundTypedOperationTargetRef(
							Profile, Request.GetPayload(), *Resources,
							TargetRef, RouteError))
						{
							SetError(OutError, RouteError.Code,
								RouteError.SubjectId, RouteError.Detail);
							return false;
						}
						StateSemantic::FUeAgentCanonicalSnapshotPtr Snapshot;
						if (!Environment->TryCaptureTypedOperationSnapshot(
								Profile, TargetRef, NiagaraSelector,
								Resources, Cancellation,
								Snapshot, OutError)
							|| !Snapshot.IsValid())
						{
							return false;
						}
						FGraphCompoundCanonicalPayload Payload(
							GetGraphCompoundCanonicalSchemaId(), {});
						if (!TryDecodeGraphCompoundTypedOperation(
								Profile, Request.GetPayload(),
								Snapshot.ToSharedRef(), *Resources,
								Payload, RouteError))
						{
							SetError(OutError, RouteError.Code,
								RouteError.SubjectId, RouteError.Detail);
							return false;
						}
						TOptional<FChangeSetDraft> Draft;
						FGraphCompoundCanonicalError CanonicalError;
						if (!TryBuildGraphCompoundChangeSetDraft(
								Payload, Snapshot.ToSharedRef(),
								Request.GetDescriptorIdentity(),
								Request.GetRequestId(), Request.GetIdempotencyKey(),
								*Resources, Draft, CanonicalError)
							|| !Draft.IsSet())
						{
							SetError(OutError, CanonicalError.Code,
								CanonicalError.IdentityKey, CanonicalError.Detail);
							return false;
						}
						FPlanningResult Planning;
						FPlanningPreflightError PlanningError;
						if (!TryPlanChanges(
								FPlanningRequest(MoveTemp(Draft.GetValue()),
									CallerContext, false),
								*Resources, Planning, PlanningError)
							|| !Planning.IsReady()
							|| !Planning.GetPlan().IsValid())
						{
							SetError(OutError,
								PlanningError.GetCode().IsEmpty()
									? TEXT("graph_compound_planning_not_ready")
									: PlanningError.GetCode(),
								PlanningError.GetIdentityKey(),
								PlanningError.GetDetail());
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
						SetError(OutError, RouteError.Code,
							RouteError.SubjectId, RouteError.Detail);
						return false;
					}
					StateSemantic::FUeAgentCanonicalSnapshotPtr Snapshot;
					if (!Environment->TryCaptureAssetCreateSnapshot(
							Profile, Decoded, Resources, Cancellation,
							Snapshot, OutError)
						|| !Snapshot.IsValid())
					{
						return false;
					}
					TOptional<FChangeSetDraft> Draft;
					if (!TryBuildGraphCompoundTypedOperationDraft(
							Profile, Request.GetDescriptorIdentity(),
							Request.GetPayload(), Snapshot.ToSharedRef(),
							Request.GetRequestId(), Request.GetIdempotencyKey(),
							*Resources, Draft, RouteError)
						|| !Draft.IsSet())
					{
						SetError(OutError, RouteError.Code,
							RouteError.SubjectId, RouteError.Detail);
						return false;
					}
					FPlanningResult Planning;
					FPlanningPreflightError PlanningError;
					if (!TryPlanChanges(
							FPlanningRequest(MoveTemp(Draft.GetValue()),
								CallerContext, false),
							*Resources, Planning, PlanningError)
						|| !Planning.IsReady() || !Planning.GetPlan().IsValid())
					{
						SetError(OutError,
							PlanningError.GetCode().IsEmpty()
								? TEXT("graph_compound_planning_not_ready")
								: PlanningError.GetCode(),
							PlanningError.GetIdentityKey(),
							PlanningError.GetDetail());
						return false;
					}
					return Environment->TryExecuteAssetCreate(
						Profile, Decoded, Planning.GetPlan().ToSharedRef(),
						CallerContext, MoveTemp(Resources), Cancellation,
						OutResult, OutError);
				}
				if (Profile.GetRouteKind()
					== EGraphCompoundDomainRouteKind::HjsonImport)
				{
					FGraphCompoundHjsonSourceRequest Decoded;
					FGraphCompoundRouteError DecodeError;
					if (!TryDecodeGraphCompoundHjsonSourceRequest(
							Profile, Request.GetPayload(), *Resources,
							Decoded, DecodeError))
					{
						SetError(OutError, DecodeError.Code,
							DecodeError.SubjectId, DecodeError.Detail);
						return false;
					}
					return Environment->TryExecuteHjson(
						Profile, Decoded, Request.GetDescriptorIdentity(),
						Request.GetRequestId(), Request.GetIdempotencyKey(),
						CallerContext,
						MoveTemp(Resources), Cancellation,
						OutResult, OutError);
				}
				SetError(OutError,
					TEXT("graph_compound_binding_route_kind_invalid"),
					Profile.GetCapabilityId());
				return false;
			}

		private:
			FGraphCompoundDomainRouteProfile Profile;
			TSharedRef<IGraphCompoundBlueprintMaterialEnvironment> Environment;
			FString OwnerId;
		};
	}

	bool FGraphCompoundBlueprintMaterialProductionConfig::IsValid() const
	{
		return FMath::IsFinite(OperationTimeoutSeconds)
			&& OperationTimeoutSeconds > 0.0;
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
		for (const FGraphCompoundDomainRouteProfile& Profile
			: GetBlueprintMaterialGraphRouteProfiles())
		{
			if (!Profile.IsValid())
			{
				SetError(OutError,
					TEXT("graph_compound_binding_profile_invalid"),
					Profile.GetCapabilityId());
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
		if (OutQueryBindings.Num() != 2
			|| OutOperationBindings.Num() != 17)
		{
			SetError(OutError,
				TEXT("graph_compound_binding_count_invalid"),
				TEXT("blueprint_material"));
			OutQueryBindings.Reset();
			OutOperationBindings.Reset();
			return false;
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
		FString RegistryError;
		if (!TryCreateBlueprintMaterialGraphDescriptorRegistry(
				Registry, RegistryError)
			|| !Registry.IsValid())
		{
			SetError(OutError,
				TEXT("graph_compound_domain_registry_invalid"),
				TEXT("blueprint_material"), RegistryError);
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
