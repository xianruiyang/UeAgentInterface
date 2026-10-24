#include "Composition/GraphCompound/UeAgentGraphCompoundPcgAiBindings.h"
#include "Misc/ScopeExit.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
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

		void SetError(FGraphCompoundRuntimeError& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& SubjectId = FString())
        { OutError = MoveTemp(Diagnostic); if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = SubjectId; }

		FDiagnostic BindingFailure(
			const TCHAR* Code, const FString& Reason, const FString& Subject,
			const FString& Field, TSharedPtr<FJsonValue> Actual,
			TSharedPtr<FJsonValue> Expected)
		{
			FDiagnostic Error = FDiagnostic::Failure(Code, Reason, TEXT("graph_compound"), FString(), Subject);
			Error.SourceKind = TEXT("native_result");
			Error.TargetPath = Subject;
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

		FDiagnostic InputFailure(
			const TCHAR* Code, const FString& Reason, const FString& Field,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			FDiagnostic Error = BindingFailure(Code, Reason, FString(), Field,
				Actual, MoveTemp(Expected));
			Error.SourceKind = TEXT("command_input");
			Error.Source.JsonPath = FString(TEXT("/")) + Field;
			Error.ActualValuePresent = Actual.IsValid();
			return Error;
		}

		bool TryReadInputObject(
			const FUeAgentContractValue& Input, TSharedPtr<FJsonObject>& OutObject,
			FDiagnostic& OutError)
		{
			if (!Json::TryParseObject(Input.GetJsonValue(), OutObject, OutError))
			{
				BindGraphCompoundInputValueDiagnostic(OutError, TEXT("/"));
				return false;
			}
			if (!OutObject.IsValid())
			{
				OutError = InputFailure(TEXT("graph_compound_typed_request_invalid"),
					TEXT("The parsed graph request object is absent."), FString(),
					nullptr, MakeShared<FJsonValueString>(TEXT("object")));
				return false;
			}
			return true;
		}

		bool TryReadStringField(
			const FJsonObject& Object, const FString& Field, FString& OutValue,
			FDiagnostic& OutError)
		{
			if (Object.TryGetStringField(Field, OutValue)) return true;
			const TSharedPtr<FJsonValue> Actual = Object.Values.FindRef(Field);
			OutError = InputFailure(TEXT("graph_compound_typed_request_invalid"),
				Actual.IsValid() ? TEXT("The graph request field must be a string.")
					: TEXT("The required graph request field is missing."),
				Field, Actual, MakeShared<FJsonValueString>(TEXT("string")));
			return false;
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
			if (!TryReadInputObject(Input, Object, OutError)) return false;
			FString AssetName;
			FString PackagePath;
			if (!TryReadStringField(*Object, TEXT("asset_name"), AssetName, OutError)
				|| !TryReadStringField(*Object, TEXT("package_path"), PackagePath, OutError))
			{
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
				const FString Field = AssetName.IsEmpty() ? TEXT("asset_name") : TEXT("package_path");
				OutError = InputFailure(TEXT("graph_compound_typed_request_invalid"),
					AssetName.IsEmpty() ? TEXT("The asset name is empty after trimming.")
						: TEXT("The package path is empty after trimming and removing trailing slashes."),
					Field, Object->Values.FindRef(Field),
					MakeShared<FJsonValueString>(TEXT("non-empty string after normalization")));
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
			if (!TryReadInputObject(Input, Object, OutError)
				|| !TryReadStringField(*Object, TEXT("target_ref"), OutTargetRef, OutError))
			{
				return false;
			}
			OutTargetRef.TrimStartAndEndInline();
			const FString PackageName =
				FPackageName::ObjectPathToPackageName(OutTargetRef);
			FText PathReason;
			FString Reason;
			bool bInvalid = true;
			if (OutTargetRef.IsEmpty())
				Reason = TEXT("The target object path is empty after trimming.");
			else if (!FPackageName::IsValidObjectPath(OutTargetRef, &PathReason))
				Reason = PathReason.IsEmpty() ? TEXT("The target is not a valid UE object path.") : PathReason.ToString();
			else if (!FPackageName::IsValidLongPackageName(PackageName, false, &PathReason))
				Reason = PathReason.IsEmpty() ? TEXT("The target object path contains an invalid package name.") : PathReason.ToString();
			else bInvalid = false;
			if (bInvalid)
			{
				OutError = InputFailure(TEXT("graph_compound_pcg_ai_target_invalid"),
					Reason, TEXT("target_ref"), Object->Values.FindRef(TEXT("target_ref")),
					MakeShared<FJsonValueString>(TEXT("valid UE object path, e.g. /Game/Folder/Asset.Asset")));
				OutError.TargetPath = OutTargetRef;
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
			if (!TryReadInputObject(Input, Object, OutError)
				|| !TryReadStringField(*Object, TEXT("target_ref"), OutTargetRef, OutError))
			{
				return false;
			}
			OutTargetRef.TrimStartAndEndInline();
			FText PathReason;
			FString Reason;
			bool bInvalid = true;
			if (!FPackageName::IsValidLongPackageName(OutTargetRef, false, &PathReason))
				Reason = PathReason.IsEmpty() ? TEXT("The destination is not a valid long package name.") : PathReason.ToString();
			else if (OutTargetRef.Contains(TEXT(".")))
				Reason = TEXT("The destination package path must not contain an object suffix.");
			else if (FPackageName::GetLongPackageAssetName(OutTargetRef).IsEmpty())
				Reason = TEXT("The destination package path has no asset name.");
			else bInvalid = false;
			if (bInvalid)
			{
				OutError = InputFailure(TEXT("graph_compound_pcg_ai_target_invalid"),
					Reason, TEXT("target_ref"), Object->Values.FindRef(TEXT("target_ref")),
					MakeShared<FJsonValueString>(TEXT("destination package path, e.g. /Game/Folder/Asset")));
				OutError.TargetPath = OutTargetRef;
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
			FString DataTypeRef;
			if (!TryReadInputObject(Input, Object, OutError)
				|| !TryReadStringField(*Object, TEXT("data_type_ref"), DataTypeRef, OutError))
			{
				return false;
			}
			if (DataTypeRef != TEXT("/Script/PCG.PCGPointData"))
			{
				OutError = InputFailure(TEXT("graph_compound_pcg_data_type_unsupported"),
					TEXT("The PCG data asset operation requires PCGPointData."),
					TEXT("data_type_ref"), MakeShared<FJsonValueString>(DataTypeRef),
					MakeShared<FJsonValueString>(TEXT("/Script/PCG.PCGPointData")));
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
			const TCHAR* const TargetField = Profile.GetInputKind()
				== EPcgAnimationGraphInputKind::PcgWorldBinding
				? TEXT("component_ref") : TEXT("target_ref");
			if (!TryReadInputObject(Input, Object, OutError)
				|| !TryReadStringField(*Object, TargetField, OutTargetRef, OutError))
			{
				return false;
			}
			OutTargetRef.TrimStartAndEndInline();
			UPCGComponent* const Component = OutTargetRef.IsEmpty()
				? nullptr : FindObject<UPCGComponent>(nullptr, *OutTargetRef);
			if (!Component || Component->GetPathName() != OutTargetRef)
			{
				OutError = InputFailure(TEXT("graph_compound_pcg_ai_target_invalid"),
					OutTargetRef.IsEmpty() ? TEXT("The PCG component reference is empty after trimming.")
						: !Component ? TEXT("No loaded PCG component was found for the reference.")
						: TEXT("The resolved PCG component path differs from the requested path."),
					TargetField, Object->Values.FindRef(TargetField),
					MakeShared<FJsonValueString>(Component ? Component->GetPathName() : TEXT("exact path of a loaded PCG component")));
				OutError.TargetPath = OutTargetRef;
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
					OutError = DescriptorMismatch(Request.GetDescriptorIdentity(), GetDescriptorIdentity());
					return false;
				}
				const bool bAssetCreate = IsSupportedAssetCreate(Profile);
				if (!bAssetCreate && !IsSupportedSchemaMutation(Profile))
				{
					const TSharedRef<FJsonObject> Actual = MakeShared<FJsonObject>();
					Actual->SetStringField(TEXT("capability_id"), Profile.GetCapabilityId());
					Actual->SetStringField(TEXT("target_kind"), Profile.GetTargetKind());
					Actual->SetNumberField(TEXT("input_kind"), static_cast<int32>(Profile.GetInputKind()));
					OutError = BindingFailure(TEXT("graph_compound_production_binding_unimplemented"),
						TEXT("No PCG/AI production binding supports this route combination."),
						Profile.GetDescriptorId(), TEXT("route"),
						MakeShared<FJsonValueObject>(Actual),
						MakeShared<FJsonValueString>(TEXT("supported PCG/AI create or schema mutation route")));
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
					OutError = TargetRequest.GetValidationDiagnostic();
					return false;
				}

				FUeAgentCanonicalSnapshotPtr Snapshot;
				const bool bCaptured = Environment->TryCaptureSnapshot(
					Profile, TargetRequest, Resources, Cancellation, Snapshot, OutError);
				if (!bCaptured || !Snapshot.IsValid())
				{
					if (OutError.IsEmpty())
						OutError = BindingFailure(TEXT("graph_compound_pcg_ai_snapshot_invalid"),
							bCaptured ? TEXT("PCG/AI capture returned without a snapshot.")
								: TEXT("PCG/AI capture failed without a diagnostic."),
							TargetRef, bCaptured ? TEXT("snapshot.present") : TEXT("snapshot.capture_succeeded"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}

				FGraphCompoundCanonicalPayload Payload(
					GetGraphCompoundCanonicalSchemaId(), {});
				FGraphCompoundRouteError RouteError;
				if (!TryDecodePcgAnimationGraphTypedOperation(
						Profile, Request.GetPayload(), Snapshot.ToSharedRef(),
						*Resources, Payload, RouteError))
				{
					SetError(OutError, RouteError, RouteError.SubjectId);
					return false;
				}
				const FGraphCompoundPcgAiOperationRequest TypedRequest(
					TargetRequest, MoveTemp(Payload));
				ON_SCOPE_EXIT { TypedRequest.GetPayload().LocateDiagnostic(OutError); };

				TOptional<FChangeSetDraft> Draft;
				FGraphCompoundCanonicalError CanonicalError;
				if (!TypedRequest.IsValid())
				{
					OutError = TypedRequest.GetValidationDiagnostic();
					return false;
				}
				if (!TryBuildGraphCompoundChangeSetDraft(
						TypedRequest.GetPayload(), Snapshot.ToSharedRef(),
						Request.GetDescriptorIdentity(), Request.GetRequestId(),
						Request.GetIdempotencyKey(), *Resources, Draft, CanonicalError))
				{
					OutError = CanonicalError;
					return false;
				}

				FPlanningResult Planning;
				FPlanningPreflightError PlanningError;
				const bool bPlanned = TryPlanChanges(
					FPlanningRequest(MoveTemp(Draft.GetValue()), CallerContext, false),
					*Resources, Planning, PlanningError);
				if (!bPlanned || !Planning.IsReady() || !Planning.GetPlan().IsValid())
				{
					OutError = PlanningError.IsEmpty()
						? PlanningFailure(bPlanned, Planning, TargetRef) : PlanningError;
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
		if (TargetRef.IsEmpty() || TargetKind.IsEmpty())
		{
			ValidationDiagnostic = BindingFailure(TEXT("graph_compound_pcg_ai_typed_request_invalid"),
				TargetRef.IsEmpty() ? TEXT("The PCG/AI target reference is empty after trimming.")
					: TEXT("The PCG/AI target kind is empty after trimming."),
				TargetRef, TargetRef.IsEmpty() ? TEXT("target_ref") : TEXT("target_kind"),
				MakeShared<FJsonValueString>(TargetRef.IsEmpty() ? TargetRef : TargetKind),
				MakeShared<FJsonValueString>(TEXT("non-empty string")));
		}
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
		return ValidationDiagnostic.IsEmpty();
	}

	const FDiagnostic& FGraphCompoundPcgAiTargetRequest::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic;
	}

	FGraphCompoundPcgAiOperationRequest::
	FGraphCompoundPcgAiOperationRequest(
		FGraphCompoundPcgAiTargetRequest InTarget,
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
			ValidationDiagnostic = BindingFailure(TEXT("graph_compound_pcg_ai_typed_request_invalid"),
				TEXT("The PCG/AI operation payload uses an incompatible canonical schema."),
				Target.GetTargetRef(), TEXT("schema_id"),
				MakeShared<FJsonValueString>(Payload.GetSchemaId()),
				MakeShared<FJsonValueString>(GetGraphCompoundCanonicalSchemaId()));
		}
		else if (Payload.GetEdits().IsEmpty())
		{
			ValidationDiagnostic = BindingFailure(TEXT("graph_compound_pcg_ai_typed_request_invalid"),
				TEXT("The PCG/AI operation payload contains no edits."),
				Target.GetTargetRef(), TEXT("edits.count"),
				MakeShared<FJsonValueNumber>(0),
				MakeShared<FJsonValueString>(TEXT("at least one edit")));
		}
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
		return ValidationDiagnostic.IsEmpty();
	}

	const FDiagnostic& FGraphCompoundPcgAiOperationRequest::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic;
	}

	bool TryCreatePcgAiGraphDescriptorRegistry(
		TSharedPtr<const FUeAgentDescriptorRegistry>& OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError)
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
		const TArray<FPcgAnimationGraphRouteProfile>* Profiles = nullptr;
		if (!TryGetPcgAnimationGraphRouteProfiles(Profiles, OutError)) return false;
		for (const FPcgAnimationGraphRouteProfile& Profile : *Profiles)
		{
			if (Profile.GetDomainId() != PcgAiDomainId)
			{
				continue;
			}
			if (!Profile.IsValid())
			{
				OutError = Profile.GetValidationDiagnostic();
				OutBindings.Reset();
				return false;
			}
			OutBindings.Add(MakeShared<FPcgAiOperationBinding>(
				Profile, Environment));
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
		UE::UAI::Contract::FDiagnostic RegistryError;
		if (!TryCreatePcgAiGraphDescriptorRegistry(
				Registry, RegistryError)
			|| !Registry.IsValid())
		{
			SetError(OutError, RegistryError, PcgAiDomainId);
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
