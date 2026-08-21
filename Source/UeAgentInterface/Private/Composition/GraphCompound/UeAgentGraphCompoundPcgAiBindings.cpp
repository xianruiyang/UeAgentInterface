#include "Composition/GraphCompound/UeAgentGraphCompoundPcgAiBindings.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "PCGComponent.h"

namespace UE::UAI::Composition::GraphCompound
{
	using namespace ContextResource;
	using namespace Contract;
	using namespace PlanningPreflight;
	using namespace StateSemantic;

	namespace
	{
		const FString PcgAiDomainId = TEXT("pcg_ai_node_graph");

		struct FSupportedAssetCreateProfile final
		{
			const TCHAR* CapabilityId;
			const TCHAR* TargetKind;
		};

		const TArray<FSupportedAssetCreateProfile>&
		SupportedAssetCreateProfiles()
		{
			static const TArray<FSupportedAssetCreateProfile> Profiles = {
				{TEXT("ai.behavior_tree.asset.create"), TEXT("behavior_tree")},
				{TEXT("ai.blackboard.asset.create"), TEXT("blackboard")},
				{TEXT("ai.eqs.asset.create"), TEXT("eqs")},
				{TEXT("ai.smart_object_definition.asset.create"),
					TEXT("smart_object_definition")},
				{TEXT("ai.state_tree.asset.create"), TEXT("state_tree")},
				{TEXT("pcg.graph.asset.create"), TEXT("pcg_graph")},
			};
			return Profiles;
		}

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

		bool IsSupportedAssetCreate(
			const FPcgAnimationGraphRouteProfile& Profile)
		{
			if (Profile.GetDomainId() != PcgAiDomainId
				|| Profile.GetInputKind()
					!= EPcgAnimationGraphInputKind::AssetCreateSimple)
			{
				return false;
			}
			for (const FSupportedAssetCreateProfile& Supported
				: SupportedAssetCreateProfiles())
			{
				if (Profile.GetCapabilityId() == Supported.CapabilityId
					&& Profile.GetTargetKind() == Supported.TargetKind)
				{
					return true;
				}
			}
			return false;
		}

		bool IsSupportedSchemaMutation(
			const FPcgAnimationGraphRouteProfile& Profile)
		{
			return Profile.GetDomainId() == PcgAiDomainId
				&& ((Profile.GetCapabilityId()
						== TEXT("ai.behavior_tree.graph.apply")
					&& Profile.GetInputKind()
						== EPcgAnimationGraphInputKind::GraphEdits
					&& Profile.GetTargetKind() == TEXT("behavior_tree"))
					|| (Profile.GetCapabilityId()
						== TEXT("ai.blackboard.schema.apply")
					&& Profile.GetInputKind()
						== EPcgAnimationGraphInputKind::BlackboardSchema
					&& Profile.GetTargetKind() == TEXT("blackboard"))
					|| (Profile.GetCapabilityId()
						== TEXT("ai.eqs.graph.apply")
					&& Profile.GetInputKind()
						== EPcgAnimationGraphInputKind::GraphEdits
					&& Profile.GetTargetKind() == TEXT("eqs"))
					|| (Profile.GetCapabilityId()
						== TEXT("ai.smart_object_definition.apply")
					&& Profile.GetInputKind()
						== EPcgAnimationGraphInputKind::SmartObjectDefinition
					&& Profile.GetTargetKind()
						== TEXT("smart_object_definition"))
					|| (Profile.GetCapabilityId()
						== TEXT("ai.state_tree.hierarchy.apply")
					&& Profile.GetInputKind()
						== EPcgAnimationGraphInputKind::GraphEdits
					&& Profile.GetTargetKind() == TEXT("state_tree"))
					|| (Profile.GetCapabilityId() == TEXT("pcg.graph.apply")
					&& Profile.GetInputKind()
						== EPcgAnimationGraphInputKind::GraphEdits
					&& Profile.GetTargetKind() == TEXT("pcg_graph"))
					|| (Profile.GetCapabilityId()
						== TEXT("pcg.graph_instance.apply")
					&& Profile.GetInputKind()
						== EPcgAnimationGraphInputKind::PcgGraphInstance
					&& Profile.GetTargetKind() == TEXT("pcg_graph_instance"))
					|| (Profile.GetCapabilityId()
						== TEXT("pcg.graph_parameters.apply")
					&& Profile.GetInputKind()
						== EPcgAnimationGraphInputKind::PcgParameters
					&& Profile.GetTargetKind() == TEXT("pcg_graph"))
					|| (Profile.GetCapabilityId()
						== TEXT("pcg.component.apply")
					&& Profile.GetInputKind()
						== EPcgAnimationGraphInputKind::PcgComponentBinding
					&& Profile.GetTargetKind() == TEXT("pcg_component"))
					|| (Profile.GetCapabilityId()
						== TEXT("pcg.component.world_binding.apply")
					&& Profile.GetInputKind()
						== EPcgAnimationGraphInputKind::PcgWorldBinding
					&& Profile.GetTargetKind() == TEXT("pcg_component"))
					|| (Profile.GetCapabilityId()
						== TEXT("pcg.data_asset.apply")
					&& Profile.GetInputKind()
						== EPcgAnimationGraphInputKind::PcgDataAsset
					&& Profile.GetTargetKind() == TEXT("pcg_data_asset"))
					|| (Profile.GetCapabilityId()
						== TEXT("pcg.data_asset.create_from_level")
					&& Profile.GetInputKind()
						== EPcgAnimationGraphInputKind::PcgDataFromLevel
					&& Profile.GetTargetKind() == TEXT("pcg_data_asset")));
		}

		bool IsPcgDataFromLevel(
			const FPcgAnimationGraphRouteProfile& Profile)
		{
			return Profile.GetCapabilityId()
					== TEXT("pcg.data_asset.create_from_level")
				&& Profile.GetInputKind()
					== EPcgAnimationGraphInputKind::PcgDataFromLevel
				&& Profile.GetTargetKind() == TEXT("pcg_data_asset");
		}

		bool IsPcgComponentMutation(
			const FPcgAnimationGraphRouteProfile& Profile)
		{
			return Profile.GetTargetKind() == TEXT("pcg_component")
				&& ((Profile.GetCapabilityId() == TEXT("pcg.component.apply")
						&& Profile.GetInputKind()
							== EPcgAnimationGraphInputKind::PcgComponentBinding)
					|| (Profile.GetCapabilityId()
							== TEXT("pcg.component.world_binding.apply")
						&& Profile.GetInputKind()
							== EPcgAnimationGraphInputKind::PcgWorldBinding));
		}

		bool TryReadAssetCreateTarget(
			const FPcgAnimationGraphRouteProfile& Profile,
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
				SetError(OutError,
					TEXT("graph_compound_typed_request_invalid"),
					Profile.GetCapabilityId(), JsonError);
				return false;
			}
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
			if (AssetName.IsEmpty() || PackagePath.IsEmpty())
			{
				SetError(OutError,
					TEXT("graph_compound_typed_request_invalid"),
					Profile.GetCapabilityId());
				return false;
			}
			OutTargetRef = PackagePath + TEXT("/") + AssetName;
			return true;
		}

		bool TryReadMutationTarget(
			const FPcgAnimationGraphRouteProfile& Profile,
			const FUeAgentContractValue& Input,
			FString& OutTargetRef,
			FGraphCompoundRuntimeError& OutError)
		{
			OutTargetRef.Reset();
			TSharedPtr<FJsonObject> Object;
			FString JsonError;
			if (!Json::TryParseObject(Input.GetJsonValue(), Object, JsonError)
				|| !Object.IsValid()
				|| !Object->TryGetStringField(TEXT("target_ref"), OutTargetRef))
			{
				SetError(OutError,
					TEXT("graph_compound_typed_request_invalid"),
					Profile.GetCapabilityId(), JsonError);
				return false;
			}
			OutTargetRef.TrimStartAndEndInline();
			const FString PackageName =
				FPackageName::ObjectPathToPackageName(OutTargetRef);
			if (OutTargetRef.IsEmpty()
				|| !FPackageName::IsValidObjectPath(OutTargetRef)
				|| !FPackageName::IsValidLongPackageName(PackageName))
			{
				SetError(OutError,
					TEXT("graph_compound_pcg_ai_target_invalid"),
					OutTargetRef);
				return false;
			}
			return true;
		}

		bool TryReadPcgDataFromLevelTarget(
			const FPcgAnimationGraphRouteProfile& Profile,
			const FUeAgentContractValue& Input,
			FString& OutTargetRef,
			FGraphCompoundRuntimeError& OutError)
		{
			OutTargetRef.Reset();
			TSharedPtr<FJsonObject> Object;
			FString JsonError;
			if (!Json::TryParseObject(Input.GetJsonValue(), Object, JsonError)
				|| !Object.IsValid()
				|| !Object->TryGetStringField(TEXT("target_ref"), OutTargetRef))
			{
				SetError(OutError,
					TEXT("graph_compound_typed_request_invalid"),
					Profile.GetCapabilityId(), JsonError);
				return false;
			}
			OutTargetRef.TrimStartAndEndInline();
			if (!FPackageName::IsValidLongPackageName(OutTargetRef)
				|| OutTargetRef.Contains(TEXT("."))
				|| FPackageName::GetLongPackageAssetName(OutTargetRef).IsEmpty())
			{
				SetError(OutError,
					TEXT("graph_compound_pcg_ai_target_invalid"), OutTargetRef);
				return false;
			}
			return true;
		}

		bool TryValidatePcgDataAssetType(
			const FPcgAnimationGraphRouteProfile& Profile,
			const FUeAgentContractValue& Input,
			FGraphCompoundRuntimeError& OutError)
		{
			if (Profile.GetCapabilityId() != TEXT("pcg.data_asset.apply"))
			{
				return true;
			}
			TSharedPtr<FJsonObject> Object;
			FString JsonError;
			FString DataTypeRef;
			if (!Json::TryParseObject(Input.GetJsonValue(), Object, JsonError)
				|| !Object.IsValid()
				|| !Object->TryGetStringField(TEXT("data_type_ref"), DataTypeRef))
			{
				SetError(OutError,
					TEXT("graph_compound_typed_request_invalid"),
					Profile.GetCapabilityId(), JsonError);
				return false;
			}
			if (DataTypeRef != TEXT("/Script/PCG.PCGPointData"))
			{
				SetError(OutError,
					TEXT("graph_compound_pcg_data_type_unsupported"),
					DataTypeRef);
				return false;
			}
			return true;
		}

		bool TryReadPcgComponentMutationTarget(
			const FPcgAnimationGraphRouteProfile& Profile,
			const FUeAgentContractValue& Input,
			FString& OutTargetRef,
			FGraphCompoundRuntimeError& OutError)
		{
			OutTargetRef.Reset();
			TSharedPtr<FJsonObject> Object;
			FString JsonError;
			const TCHAR* const TargetField = Profile.GetInputKind()
				== EPcgAnimationGraphInputKind::PcgWorldBinding
				? TEXT("component_ref") : TEXT("target_ref");
			if (!Json::TryParseObject(Input.GetJsonValue(), Object, JsonError)
				|| !Object.IsValid()
				|| !Object->TryGetStringField(TargetField, OutTargetRef))
			{
				SetError(OutError,
					TEXT("graph_compound_typed_request_invalid"),
					Profile.GetCapabilityId(), JsonError);
				return false;
			}
			OutTargetRef.TrimStartAndEndInline();
			UPCGComponent* const Component = OutTargetRef.IsEmpty()
				? nullptr : FindObject<UPCGComponent>(nullptr, *OutTargetRef);
			if (!Component || Component->GetPathName() != OutTargetRef)
			{
				SetError(OutError,
					TEXT("graph_compound_pcg_ai_target_invalid"),
					OutTargetRef);
				return false;
			}
			return true;
		}

		class FPcgAiOperationBinding final
			: public IGraphCompoundOperationBinding
		{
		public:
			FPcgAiOperationBinding(
				FPcgAnimationGraphRouteProfile InProfile,
				TSharedRef<IGraphCompoundPcgAiEnvironment> InEnvironment)
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
				const bool bAssetCreate = IsSupportedAssetCreate(Profile);
				if (!bAssetCreate && !IsSupportedSchemaMutation(Profile))
				{
					SetError(OutError,
						TEXT("graph_compound_production_binding_unimplemented"),
						Profile.GetCapabilityId());
					return false;
				}

				FString TargetRef;
				const bool bPcgComponentMutation =
					IsPcgComponentMutation(Profile);
				const bool bPcgDataFromLevel = IsPcgDataFromLevel(Profile);
				if (!(bAssetCreate
						? TryReadAssetCreateTarget(
							Profile, Request.GetPayload(), TargetRef, OutError)
						: bPcgComponentMutation
							? TryReadPcgComponentMutationTarget(
								Profile, Request.GetPayload(), TargetRef, OutError)
							: bPcgDataFromLevel
								? TryReadPcgDataFromLevelTarget(
									Profile, Request.GetPayload(), TargetRef, OutError)
								: TryReadMutationTarget(
								Profile, Request.GetPayload(), TargetRef, OutError)))
				{
					return false;
				}
				if (!TryValidatePcgDataAssetType(
						Profile, Request.GetPayload(), OutError))
				{
					return false;
				}

				const FGraphCompoundPcgAiTargetRequest TargetRequest(
					TargetRef, Profile.GetTargetKind());
				if (!TargetRequest.IsValid())
				{
					SetError(OutError,
						TEXT("graph_compound_pcg_ai_typed_request_invalid"),
						Profile.GetCapabilityId());
					return false;
				}

				FUeAgentCanonicalSnapshotPtr Snapshot;
				if (!Environment->TryCaptureSnapshot(
						Profile, TargetRequest, Resources, Cancellation,
						Snapshot, OutError)
					|| !Snapshot.IsValid())
				{
					return false;
				}

				FGraphCompoundCanonicalPayload Payload(
					GetGraphCompoundCanonicalSchemaId(), {});
				FGraphCompoundRouteError RouteError;
				if (!TryDecodePcgAnimationGraphTypedOperation(
						Profile, Request.GetPayload(), Snapshot.ToSharedRef(),
						*Resources, Payload, RouteError))
				{
					SetError(OutError, RouteError.Code,
						RouteError.SubjectId, RouteError.Detail);
					return false;
				}
				const FGraphCompoundPcgAiOperationRequest TypedRequest(
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
							? TEXT("graph_compound_pcg_ai_draft_invalid")
							: CanonicalError.Code,
						CanonicalError.IdentityKey,
						CanonicalError.Detail);
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
						PlanningError.GetIdentityKey(),
						PlanningError.GetDetail());
					return false;
				}
				return Environment->TryExecute(
					Profile, TypedRequest, Planning.GetPlan().ToSharedRef(),
					CallerContext, MoveTemp(Resources), Cancellation,
					OutResult, OutError);
			}

		private:
			FPcgAnimationGraphRouteProfile Profile;
			TSharedRef<IGraphCompoundPcgAiEnvironment> Environment;
			FString OwnerId;
		};
	}

	FGraphCompoundPcgAiTargetRequest::FGraphCompoundPcgAiTargetRequest(
		FString InTargetRef,
		FString InTargetKind)
		: TargetRef(MoveTemp(InTargetRef))
		, TargetKind(MoveTemp(InTargetKind))
	{
		TargetRef.TrimStartAndEndInline();
		TargetKind.TrimStartAndEndInline();
		bValid = !TargetRef.IsEmpty() && !TargetKind.IsEmpty();
	}

	const FString& FGraphCompoundPcgAiTargetRequest::GetTargetRef() const
	{
		return TargetRef;
	}

	const FString& FGraphCompoundPcgAiTargetRequest::GetTargetKind() const
	{
		return TargetKind;
	}

	bool FGraphCompoundPcgAiTargetRequest::IsValid() const
	{
		return bValid;
	}

	FGraphCompoundPcgAiOperationRequest::
	FGraphCompoundPcgAiOperationRequest(
		FGraphCompoundPcgAiTargetRequest InTarget,
		FGraphCompoundCanonicalPayload InPayload)
		: Target(MoveTemp(InTarget))
		, Payload(MoveTemp(InPayload))
	{
		bValid = Target.IsValid()
			&& Payload.GetSchemaId() == GetGraphCompoundCanonicalSchemaId()
			&& !Payload.GetEdits().IsEmpty();
	}

	const FGraphCompoundPcgAiTargetRequest&
	FGraphCompoundPcgAiOperationRequest::GetTarget() const
	{
		return Target;
	}

	const FString& FGraphCompoundPcgAiOperationRequest::GetTargetRef() const
	{
		return Target.GetTargetRef();
	}

	const FString& FGraphCompoundPcgAiOperationRequest::GetTargetKind() const
	{
		return Target.GetTargetKind();
	}

	const FGraphCompoundCanonicalPayload&
	FGraphCompoundPcgAiOperationRequest::GetPayload() const
	{
		return Payload;
	}

	bool FGraphCompoundPcgAiOperationRequest::IsValid() const
	{
		return bValid;
	}

	bool FGraphCompoundPcgAiProductionConfig::IsValid() const
	{
		return FMath::IsFinite(OperationTimeoutSeconds)
			&& OperationTimeoutSeconds > 0.0;
	}

	bool TryCreatePcgAiGraphDescriptorRegistry(
		TSharedPtr<const FUeAgentDescriptorRegistry>& OutRegistry,
		FString& OutError)
	{
		OutRegistry.Reset();
		OutError.Reset();
		TSharedPtr<const FUeAgentDescriptorRegistry> Combined;
		if (!TryCreatePcgAnimationGraphDescriptorRegistry(
				Combined, OutError)
			|| !Combined.IsValid())
		{
			return false;
		}
		TArray<FUeAgentOperationDescriptor> Descriptors;
		for (const FUeAgentOperationDescriptor& Descriptor
			: Combined->GetDescriptors())
		{
			if (Descriptor.GetDomainId() == PcgAiDomainId)
			{
				Descriptors.Add(Descriptor);
			}
		}
		if (Descriptors.Num() != 18)
		{
			OutError = FString::Printf(
				TEXT("Expected 17 PCG/AI descriptors, found %d."),
				Descriptors.Num());
			return false;
		}
		return FUeAgentDescriptorRegistry::TryCreate(
			MoveTemp(Descriptors), OutRegistry, OutError);
	}

	bool TryCreatePcgAiGraphBindings(
		TSharedRef<IGraphCompoundPcgAiEnvironment> Environment,
		TArray<TSharedRef<IGraphCompoundOperationBinding>>& OutBindings,
		FGraphCompoundRuntimeError& OutError)
	{
		OutBindings.Reset();
		OutError = {};
		for (const FPcgAnimationGraphRouteProfile& Profile
			: GetPcgAnimationGraphRouteProfiles())
		{
			if (Profile.GetDomainId() != PcgAiDomainId)
			{
				continue;
			}
			if (!Profile.IsValid())
			{
				SetError(OutError,
					TEXT("graph_compound_binding_profile_invalid"),
					Profile.GetCapabilityId());
				OutBindings.Reset();
				return false;
			}
			OutBindings.Add(MakeShared<FPcgAiOperationBinding>(
				Profile, Environment));
		}
		if (OutBindings.Num() != 18)
		{
			SetError(OutError,
				TEXT("graph_compound_binding_count_invalid"),
				PcgAiDomainId);
			OutBindings.Reset();
			return false;
		}
		return true;
	}

	bool TryCreatePcgAiGraphProductionRuntime(
		const FGraphCompoundRuntimeConfig& RuntimeConfig,
		const FGraphCompoundPcgAiProductionConfig& ProductionConfig,
		TSharedPtr<FGraphCompoundRuntime>& OutRuntime,
		FGraphCompoundRuntimeError& OutError)
	{
		OutRuntime.Reset();
		TSharedPtr<IGraphCompoundPcgAiEnvironment> Environment;
		if (!TryCreatePcgAiGraphProductionEnvironment(
				ProductionConfig, Environment, OutError)
			|| !Environment.IsValid())
		{
			return false;
		}
		TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
		FString RegistryError;
		if (!TryCreatePcgAiGraphDescriptorRegistry(
				Registry, RegistryError)
			|| !Registry.IsValid())
		{
			SetError(OutError,
				TEXT("graph_compound_domain_registry_invalid"),
				PcgAiDomainId, RegistryError);
			return false;
		}
		TArray<TSharedRef<IGraphCompoundQueryBinding>> QueryBindings;
		TArray<TSharedRef<IGraphCompoundOperationBinding>> OperationBindings;
		if (!TryCreatePcgAiGraphBindings(
				Environment.ToSharedRef(), OperationBindings, OutError))
		{
			return false;
		}
		return FGraphCompoundRuntime::TryCreate(
			RuntimeConfig, Registry.ToSharedRef(), MoveTemp(QueryBindings),
			MoveTemp(OperationBindings), OutRuntime, OutError);
	}
}
