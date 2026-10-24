#include "Composition/GraphCompound/UeAgentGraphCompoundAnimationBindings.h"
#include "Misc/ScopeExit.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
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
			UE::UAI::Contract::FDiagnostic JsonError;
			if (!Json::TryParseObject(Input.GetJsonValue(), Object, JsonError)
				|| !Object.IsValid())
			{
				SetError(OutError, JsonError, Profile.GetCapabilityId());
				return false;
			}
			if (IsAssetCreate(Profile.GetInputKind()))
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
					OutError = BindingFailure(TEXT("graph_compound_animation_target_invalid"),
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
			if (!FPackageName::IsValidObjectPath(OutTargetRef, &PackageReason)
				|| !FPackageName::IsValidLongPackageName(
					FPackageName::ObjectPathToPackageName(OutTargetRef), false, &PackageReason))
			{
				OutError = BindingFailure(TEXT("graph_compound_animation_target_invalid"),
					PackageReason.ToString(), OutTargetRef, TEXT("target_ref"),
					MakeShared<FJsonValueString>(OutTargetRef),
					MakeShared<FJsonValueString>(TEXT("valid object path and long package name")));
				OutError.SourceKind = TEXT("command_input");
				OutError.Source.JsonPath = TEXT("/target_ref");
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
					OutError = DescriptorMismatch(Request.GetDescriptorIdentity(), GetDescriptorIdentity());
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
				if (!TryDecodePcgAnimationGraphTypedOperation(
						Profile, Request.GetPayload(), Snapshot.ToSharedRef(),
						*Resources, Payload, RouteError))
				{
					SetError(OutError, RouteError);
					return false;
				}
				const FGraphCompoundAnimationOperationRequest TypedRequest(
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
						TEXT("graph_compound_animation_draft_invalid"),
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
		if (TargetRef.IsEmpty() || TargetKind.IsEmpty())
		{
			ValidationDiagnostic = BindingFailure(TEXT("graph_compound_animation_target_invalid"),
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
		return ValidationDiagnostic.IsEmpty();
	}

	const FDiagnostic& FGraphCompoundAnimationTargetRequest::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic;
	}

	FGraphCompoundAnimationOperationRequest::
	FGraphCompoundAnimationOperationRequest(
		FGraphCompoundAnimationTargetRequest InTarget,
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
			ValidationDiagnostic = BindingFailure(TEXT("graph_compound_animation_draft_invalid"),
				TEXT("The graph operation payload uses an unexpected canonical schema."),
				Target.GetTargetRef(), TEXT("schema_id"),
				MakeShared<FJsonValueString>(Payload.GetSchemaId()),
				MakeShared<FJsonValueString>(GetGraphCompoundCanonicalSchemaId()));
		}
		else if (Payload.GetEdits().IsEmpty())
		{
			ValidationDiagnostic = BindingFailure(TEXT("graph_compound_animation_draft_invalid"),
				TEXT("The graph operation contains no canonical edits."),
				Target.GetTargetRef(), TEXT("edits.count"), MakeShared<FJsonValueNumber>(0),
				MakeShared<FJsonValueString>(TEXT("at least one edit")));
		}
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
		return ValidationDiagnostic.IsEmpty();
	}

	const FDiagnostic& FGraphCompoundAnimationOperationRequest::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic;
	}

	bool TryCreateAnimationGraphDescriptorRegistry(
		TSharedPtr<const FUeAgentDescriptorRegistry>& OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError)
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
		const TArray<FPcgAnimationGraphRouteProfile>* Profiles = nullptr;
		if (!TryGetPcgAnimationGraphRouteProfiles(Profiles, OutError)) return false;
		for (const FPcgAnimationGraphRouteProfile& Profile : *Profiles)
		{
			if (Profile.GetDomainId() != AnimationDomainId)
			{
				continue;
			}
			if (!Profile.IsValid())
			{
				OutError = Profile.GetValidationDiagnostic();
				OutBindings.Reset();
				return false;
			}
			OutBindings.Add(MakeShared<FAnimationOperationBinding>(
				Profile, Environment));
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
		UE::UAI::Contract::FDiagnostic RegistryError;
		if (!TryCreateAnimationGraphDescriptorRegistry(
				Registry, RegistryError)
			|| !Registry.IsValid())
		{
			SetError(OutError, RegistryError, AnimationDomainId);
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
