#include "Composition/GraphCompound/UeAgentGraphCompoundAnimationBindings.h"

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
		const FString AnimationDomainId = TEXT("animation_rig_graph");

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

		bool IsAssetCreate(const EPcgAnimationGraphInputKind Kind)
		{
			using E = EPcgAnimationGraphInputKind;
			return Kind >= E::AssetCreateSimple && Kind <= E::AssetCreateIkRig;
		}

		bool TryReadTarget(
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
				SetError(OutError, TEXT("graph_compound_typed_request_invalid"),
					Profile.GetCapabilityId(), JsonError);
				return false;
			}
			if (IsAssetCreate(Profile.GetInputKind()))
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
						TEXT("graph_compound_animation_target_invalid"),
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
			if (!FPackageName::IsValidObjectPath(OutTargetRef)
				|| !FPackageName::IsValidLongPackageName(
					FPackageName::ObjectPathToPackageName(OutTargetRef)))
			{
				SetError(OutError,
					TEXT("graph_compound_animation_target_invalid"),
					OutTargetRef);
				return false;
			}
			return true;
		}

		class FAnimationOperationBinding final
			: public IGraphCompoundOperationBinding
		{
		public:
			FAnimationOperationBinding(
				FPcgAnimationGraphRouteProfile InProfile,
				TSharedRef<IGraphCompoundAnimationEnvironment> InEnvironment)
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
				OutError = {};
				if (Request.GetDescriptorIdentity() != GetDescriptorIdentity())
				{
					SetError(OutError,
						TEXT("graph_compound_binding_descriptor_mismatch"),
						Profile.GetDescriptorId());
					return false;
				}
				FString TargetRef;
				if (!TryReadTarget(
						Profile, Request.GetPayload(), TargetRef, OutError))
				{
					return false;
				}
				const FGraphCompoundAnimationTargetRequest TargetRequest(
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
				if (!TryDecodePcgAnimationGraphTypedOperation(
						Profile, Request.GetPayload(), Snapshot.ToSharedRef(),
						*Resources, Payload, RouteError))
				{
					SetError(OutError, RouteError.Code,
						RouteError.SubjectId, RouteError.Detail);
					return false;
				}
				const FGraphCompoundAnimationOperationRequest TypedRequest(
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
							? TEXT("graph_compound_animation_draft_invalid")
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
			TSharedRef<IGraphCompoundAnimationEnvironment> Environment;
			FString OwnerId;
		};
	}

	FGraphCompoundAnimationTargetRequest::FGraphCompoundAnimationTargetRequest(
		FString InTargetRef,
		FString InTargetKind)
		: TargetRef(MoveTemp(InTargetRef))
		, TargetKind(MoveTemp(InTargetKind))
	{
		TargetRef.TrimStartAndEndInline();
		TargetKind.TrimStartAndEndInline();
		bValid = !TargetRef.IsEmpty() && !TargetKind.IsEmpty();
	}

	const FString& FGraphCompoundAnimationTargetRequest::GetTargetRef() const
	{
		return TargetRef;
	}

	const FString& FGraphCompoundAnimationTargetRequest::GetTargetKind() const
	{
		return TargetKind;
	}

	bool FGraphCompoundAnimationTargetRequest::IsValid() const
	{
		return bValid;
	}

	FGraphCompoundAnimationOperationRequest::
	FGraphCompoundAnimationOperationRequest(
		FGraphCompoundAnimationTargetRequest InTarget,
		FGraphCompoundCanonicalPayload InPayload)
		: Target(MoveTemp(InTarget))
		, Payload(MoveTemp(InPayload))
	{
		bValid = Target.IsValid()
			&& Payload.GetSchemaId() == GetGraphCompoundCanonicalSchemaId()
			&& !Payload.GetEdits().IsEmpty();
	}

	const FGraphCompoundAnimationTargetRequest&
	FGraphCompoundAnimationOperationRequest::GetTarget() const
	{
		return Target;
	}

	const FString& FGraphCompoundAnimationOperationRequest::GetTargetRef() const
	{
		return Target.GetTargetRef();
	}

	const FString& FGraphCompoundAnimationOperationRequest::GetTargetKind() const
	{
		return Target.GetTargetKind();
	}

	const FGraphCompoundCanonicalPayload&
	FGraphCompoundAnimationOperationRequest::GetPayload() const
	{
		return Payload;
	}

	bool FGraphCompoundAnimationOperationRequest::IsValid() const
	{
		return bValid;
	}

	bool FGraphCompoundAnimationProductionConfig::IsValid() const
	{
		return FMath::IsFinite(OperationTimeoutSeconds)
			&& OperationTimeoutSeconds > 0.0;
	}

	bool TryCreateAnimationGraphDescriptorRegistry(
		TSharedPtr<const FUeAgentDescriptorRegistry>& OutRegistry,
		FString& OutError)
	{
		OutRegistry.Reset();
		OutError.Reset();
		TSharedPtr<const FUeAgentDescriptorRegistry> Combined;
		if (!TryCreatePcgAnimationGraphDescriptorRegistry(Combined, OutError)
			|| !Combined.IsValid())
		{
			return false;
		}
		TArray<FUeAgentOperationDescriptor> Descriptors;
		for (const FUeAgentOperationDescriptor& Descriptor
			: Combined->GetDescriptors())
		{
			if (Descriptor.GetDomainId() == AnimationDomainId)
			{
				Descriptors.Add(Descriptor);
			}
		}
		if (Descriptors.Num() != 26)
		{
			OutError = FString::Printf(
				TEXT("Expected 26 Animation/Rig descriptors, found %d."),
				Descriptors.Num());
			return false;
		}
		return FUeAgentDescriptorRegistry::TryCreate(
			MoveTemp(Descriptors), OutRegistry, OutError);
	}

	bool TryCreateAnimationGraphBindings(
		TSharedRef<IGraphCompoundAnimationEnvironment> Environment,
		TArray<TSharedRef<IGraphCompoundOperationBinding>>& OutBindings,
		FGraphCompoundRuntimeError& OutError)
	{
		OutBindings.Reset();
		OutError = {};
		for (const FPcgAnimationGraphRouteProfile& Profile
			: GetPcgAnimationGraphRouteProfiles())
		{
			if (Profile.GetDomainId() != AnimationDomainId)
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
			OutBindings.Add(MakeShared<FAnimationOperationBinding>(
				Profile, Environment));
		}
		if (OutBindings.Num() != 26)
		{
			SetError(OutError,
				TEXT("graph_compound_binding_count_invalid"),
				AnimationDomainId);
			OutBindings.Reset();
			return false;
		}
		return true;
	}

	bool TryCreateAnimationGraphProductionRuntime(
		const FGraphCompoundRuntimeConfig& RuntimeConfig,
		const FGraphCompoundAnimationProductionConfig& ProductionConfig,
		TSharedPtr<FGraphCompoundRuntime>& OutRuntime,
		FGraphCompoundRuntimeError& OutError)
	{
		OutRuntime.Reset();
		TSharedPtr<IGraphCompoundAnimationEnvironment> Environment;
		if (!TryCreateAnimationGraphProductionEnvironment(
				ProductionConfig, Environment, OutError)
			|| !Environment.IsValid())
		{
			return false;
		}
		TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
		FString RegistryError;
		if (!TryCreateAnimationGraphDescriptorRegistry(
				Registry, RegistryError)
			|| !Registry.IsValid())
		{
			SetError(OutError,
				TEXT("graph_compound_domain_registry_invalid"),
				AnimationDomainId, RegistryError);
			return false;
		}
		TArray<TSharedRef<IGraphCompoundQueryBinding>> QueryBindings;
		TArray<TSharedRef<IGraphCompoundOperationBinding>> OperationBindings;
		if (!TryCreateAnimationGraphBindings(
				Environment.ToSharedRef(), OperationBindings, OutError))
		{
			return false;
		}
		return FGraphCompoundRuntime::TryCreate(
			RuntimeConfig, Registry.ToSharedRef(), MoveTemp(QueryBindings),
			MoveTemp(OperationBindings), OutRuntime, OutError);
	}
}
