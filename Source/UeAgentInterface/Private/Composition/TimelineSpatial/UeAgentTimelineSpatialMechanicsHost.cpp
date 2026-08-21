#include "Composition/TimelineSpatial/UeAgentTimelineSpatialMechanicsHost.h"

#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringRuntime.h"
#include "Composition/TimelineSpatial/UeAgentTimelineSpatialRuntime.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/Projection/UeAgentProjection.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformProcess.h"
#include "LevelSequence.h"
#include "Misc/App.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "MovieScene.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "Retargeter/IKRetargeter.h"
#include "Rig/IKRigDefinition.h"
#include "RigEditor/IKRigController.h"
#include "Rigs/RigHierarchy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Sequencer/MovieSceneControlRigParameterTrack.h"
#include "UniversalObjectLocatorResolveParams.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::Composition::TimelineSpatial
{
	using namespace BindingGateway::Ticket;
	using namespace CommandEdge;
	using namespace Composition::SimpleAuthoring;
	using namespace ContextResource;
	using namespace Discovery;
	using namespace Projection;
	using namespace StateSemantic;

	namespace
	{
		struct FProcessHost final
		{
			FCriticalSection Mutex;
			TMap<FString, TSharedRef<IUeAgentCommandHandler>> Handlers;
			TArray<FString> AuthorityScopes;
			TWeakObjectPtr<UWorld> BoundWorld;
			FString SessionId;
		};

		FProcessHost& ProcessHost()
		{
			static FProcessHost Host;
			return Host;
		}

		class FConservativeTokenEstimator final : public IUeAgentTokenEstimator
		{
		public:
			bool TryEstimate(
				const FUeAgentProjectedPayload& Payload,
				const TArray<FUeAgentArtifactDescriptor>& Artifacts,
				const TArray<FUeAgentProjectionDiagnostic>& Diagnostics,
				FUeAgentResourceTracker& Resources,
				const FUeAgentOperationDeadline& Deadline,
				const FUeAgentCancellationToken& Cancellation,
				FUeAgentTokenEstimate& OutEstimate,
				FUeAgentProjectionError& OutError) override
			{
				OutError = {};
				FUeAgentCancellationRequested Cancelled;
				if (!Cancellation.TryCheckpoint(Cancelled))
				{
					OutError.Code = Cancelled.Code;
					OutError.SubjectId = Cancelled.Reason;
					OutError.Detail = TEXT("TimelineSpatial mechanics projection cancelled");
					return false;
				}
				FUeAgentOperationDeadlineExceeded Expired;
				if (!Deadline.TryCheckpoint(TEXT("timeline_spatial.mechanics_projection"), Expired))
				{
					OutError.Code = Expired.Code;
					OutError.SubjectId = Expired.Stage;
					OutError.Detail = TEXT("TimelineSpatial mechanics projection deadline expired");
					return false;
				}
				FUeAgentResourceBudgetExceeded BudgetError;
				if (!Resources.TryCharge(
						EUeAgentResourceDimension::WorkUnits, 1,
						TEXT("timeline_spatial.mechanics_projection"), BudgetError))
				{
					OutError.Code = BudgetError.Code;
					OutError.SubjectId = BudgetError.Path;
					OutError.Detail = TEXT("TimelineSpatial mechanics projection budget exceeded");
					OutError.ResourceDimension = BudgetError.Dimension;
					OutError.ResourceLimit = BudgetError.Limit;
					OutError.ResourceObserved = BudgetError.Observed;
					return false;
				}
				int64 Bytes = 512 + Artifacts.Num() * 512 + Diagnostics.Num() * 256;
				if (Payload.GetSnapshot().IsValid())
				{
					Bytes += Payload.GetSnapshot()->GetEntries().Num() * 2048;
				}
				OutEstimate = FUeAgentTokenEstimate(
					EUeAgentTokenEstimateKind::Conservative,
					FMath::Max<int64>(1, (Bytes + 2) / 3), Bytes);
				return true;
			}
		};

		namespace Timeline = DomainAdapters::TimelineAnimation;
		namespace RigSpatial = DomainAdapters::RigSimulationSpatial;

		bool RequiredString(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			FString& OutValue,
			FString& OutError)
		{
			OutValue.Reset();
			if (!Object.IsValid() || !Object->TryGetStringField(Field, OutValue)
				|| OutValue.IsEmpty())
			{
				OutError = FString(TEXT("timeline_spatial_request_field_required:")) + Field;
				return false;
			}
			return true;
		}

		bool RequiredObject(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			TSharedPtr<FJsonObject>& OutValue,
			FString& OutError)
		{
			OutValue.Reset();
			const TSharedPtr<FJsonObject>* Found = nullptr;
			if (!Object.IsValid() || !Object->TryGetObjectField(Field, Found)
				|| !Found || !Found->IsValid())
			{
				OutError = FString(TEXT("timeline_spatial_request_object_required:")) + Field;
				return false;
			}
			OutValue = *Found;
			return true;
		}

		UObject* ResolveExplicitObject(const FString& Identity)
		{
			if (Identity.IsEmpty()) return nullptr;
			if (UObject* Existing = FindObject<UObject>(nullptr, *Identity)) return Existing;
			return StaticLoadObject(UObject::StaticClass(), nullptr, *Identity,
				nullptr, LOAD_NoWarn | LOAD_Quiet);
		}

		bool AddRigObject(
			FUeAgentTimelineSpatialRuntimeConfig& Config,
			const FString& Identity,
			UObject*& OutObject,
			FString& OutError)
		{
			OutObject = ResolveExplicitObject(Identity);
			if (!IsValid(OutObject))
			{
				OutError = TEXT("timeline_spatial_explicit_object_unavailable:") + Identity;
				return false;
			}
			if (!Config.RigObjectBindings.ContainsByPredicate(
				[&](const auto& Binding) { return Binding.Identity == Identity; }))
			{
				Config.RigObjectBindings.Emplace(Identity, OutObject);
			}
			return true;
		}

		bool ReadRate(
			const TSharedPtr<FJsonObject>& Point,
			FString& OutClock,
			FString& OutNumerator,
			FString& OutDenominator,
			FString& OutError)
		{
			TSharedPtr<FJsonObject> Rate;
			return RequiredString(Point, TEXT("clock_id"), OutClock, OutError)
				&& RequiredObject(Point, TEXT("rate"), Rate, OutError)
				&& RequiredString(Rate, TEXT("numerator"), OutNumerator, OutError)
				&& RequiredString(Rate, TEXT("denominator"), OutDenominator, OutError);
		}

		bool AddRigClock(
			FUeAgentTimelineSpatialRuntimeConfig& Config,
			const FString& ClockId)
		{
			if (ClockId.IsEmpty()) return false;
			if (!Config.RigClockBindings.ContainsByPredicate(
				[&](const auto& Binding)
				{
					return Binding.ClockId == ClockId || Binding.NativeClockId == ClockId;
				}))
			{
				Config.RigClockBindings.Emplace(
					ClockId, ClockId, TEXT("t58-request-clock:") + ClockId);
			}
			return true;
		}

		bool AddRigFrame(
			FUeAgentTimelineSpatialRuntimeConfig& Config,
			const TSharedPtr<FJsonObject>& Frame,
			bool bNative,
			FString& OutFrameId,
			FString& OutError)
		{
			FString BasisFrameId;
			FString Unit;
			FString BasisEvidenceId;
			double Dimension = 0.0;
			if (!RequiredString(Frame, TEXT("frame_id"), OutFrameId, OutError)
				|| !RequiredString(Frame, TEXT("basis_frame_id"), BasisFrameId, OutError)
				|| !Frame->TryGetNumberField(TEXT("dimension"), Dimension)
				|| static_cast<int32>(Dimension) != 3
				|| !RequiredString(Frame, TEXT("unit"), Unit, OutError)
				|| !RequiredString(Frame, TEXT("basis_evidence_id"), BasisEvidenceId, OutError))
			{
				if (OutError.IsEmpty()) OutError = TEXT("timeline_spatial_frame_invalid");
				return false;
			}
			if (Config.RigFrameBindings.ContainsByPredicate(
				[&](const auto& Binding) { return Binding.Frame.GetFrameId() == OutFrameId; }))
			{
				return true;
			}
			const TArray<TArray<FString>> Basis = {
				{TEXT("1"), TEXT("0"), TEXT("0")},
				{TEXT("0"), TEXT("1"), TEXT("0")},
				{TEXT("0"), TEXT("0"), TEXT("1")},
			};
			const FUeAgentTemporalPoint ObservedAt(
				TEXT("t58://request-frame"), TEXT("0"), TEXT("1"), TEXT("1"),
				TEXT("authored"));
			FUeAgentSpatialFrame SpatialFrame(
				OutFrameId, BasisFrameId, 3, Basis,
				{TEXT("0"), TEXT("0"), TEXT("0")}, Unit,
				BasisEvidenceId, ObservedAt);
			if (!SpatialFrame.IsValid())
			{
				OutError = TEXT("timeline_spatial_frame_contract_invalid:") + OutFrameId;
				return false;
			}
			Config.RigFrameBindings.Emplace(MoveTemp(SpatialFrame), bNative);
			return true;
		}

		bool AddNativeRigFrame(
			FUeAgentTimelineSpatialRuntimeConfig& Config,
			const FString& FrameId,
			const FString& Unit = TEXT("cm"))
		{
			if (FrameId.IsEmpty()) return false;
			if (Config.RigFrameBindings.ContainsByPredicate(
				[&](const auto& Binding) { return Binding.Frame.GetFrameId() == FrameId; }))
			{
				return true;
			}
			const TArray<TArray<FString>> Basis = {
				{TEXT("1"), TEXT("0"), TEXT("0")},
				{TEXT("0"), TEXT("1"), TEXT("0")},
				{TEXT("0"), TEXT("0"), TEXT("1")},
			};
			const FUeAgentTemporalPoint ObservedAt(
				TEXT("t58://native-frame"), TEXT("0"), TEXT("1"), TEXT("1"),
				TEXT("observed"));
			FUeAgentSpatialFrame Frame(
				FrameId, TEXT("t58://native-basis"), 3, Basis,
				{TEXT("0"), TEXT("0"), TEXT("0")}, Unit,
				TEXT("t58://native-basis-evidence"), ObservedAt);
			if (!Frame.IsValid()) return false;
			Config.RigFrameBindings.Emplace(MoveTemp(Frame), true);
			return true;
		}

		bool AddIkBinding(
			FUeAgentTimelineSpatialRuntimeConfig& Config,
			const FString& Identity,
			FString& OutError)
		{
			if (Config.RigIkBindings.ContainsByPredicate(
				[&](const auto& Binding) { return Binding.Identity == Identity; }))
			{
				return true;
			}
			UObject* Object = nullptr;
			if (!AddRigObject(Config, Identity, Object, OutError)) return false;
			UIKRigDefinition* Rig = Cast<UIKRigDefinition>(Object);
			UIKRigController* Controller = Rig ? UIKRigController::GetController(Rig) : nullptr;
			USkeletalMesh* Mesh = Controller ? Controller->GetSkeletalMesh() : nullptr;
			if (!Rig || !Controller || !Mesh)
			{
				OutError = TEXT("timeline_spatial_ik_binding_unavailable:") + Identity;
				return false;
			}
			RigSpatial::FUeAgentRigSimulationSpatialIkRigBinding Binding;
			Binding.Identity = Identity;
			Binding.Rig = Rig;
			Binding.PreviewMeshIdentity = Mesh->GetPathName();
			UObject* MeshObject = nullptr;
			if (!AddRigObject(Config, Binding.PreviewMeshIdentity, MeshObject, OutError))
				return false;
			const FReferenceSkeleton& ReferenceSkeleton = Mesh->GetRefSkeleton();
			for (int32 Index = 0; Index < ReferenceSkeleton.GetNum(); ++Index)
			{
				const FName Name = ReferenceSkeleton.GetBoneName(Index);
				Binding.BoneBindings.Add(Name.ToString(), Name);
			}
			for (UIKRigEffectorGoal* Goal : Controller->GetAllGoals())
				if (Goal) Binding.GoalBindings.Add(Goal->GoalName.ToString(), Goal->GoalName);
			for (const FBoneChain& Chain : Controller->GetRetargetChains())
				Binding.ChainBindings.Add(Chain.ChainName.ToString(), Chain.ChainName);
			if (!Binding.IsValid())
			{
				OutError = TEXT("timeline_spatial_ik_binding_invalid:") + Identity;
				return false;
			}
			Config.RigIkBindings.Add(MoveTemp(Binding));
			return true;
		}

		FName NativeNameFromIdentity(const FString& Identity)
		{
			int32 Separator = INDEX_NONE;
			if (Identity.FindLastChar(TEXT('/'), Separator) && Separator + 1 < Identity.Len())
				return FName(*Identity.Mid(Separator + 1));
			return FName(*Identity);
		}

		RigSpatial::FUeAgentRigSimulationSpatialIkRigBinding* FindIkBinding(
			FUeAgentTimelineSpatialRuntimeConfig& Config,
			const FString& Identity)
		{
			return Config.RigIkBindings.FindByPredicate(
				[&](const auto& Binding) { return Binding.Identity == Identity; });
		}

		void AddExistingIdentityAlias(
			TMap<FString, FName>& Bindings,
			const FString& Identity)
		{
			if (Bindings.Contains(Identity)) return;
			const FName Candidate = NativeNameFromIdentity(Identity);
			if (Bindings.FindKey(Candidate)) Bindings.Add(Identity, Candidate);
		}

		bool AddRetargeterBinding(
			FUeAgentTimelineSpatialRuntimeConfig& Config,
			const FString& Identity,
			const FString& RequestedSourceFrame,
			const FString& RequestedTargetFrame,
			FString& OutError)
		{
			if (Config.RigRetargeterBindings.ContainsByPredicate(
				[&](const auto& Binding) { return Binding.Identity == Identity; }))
			{
				return true;
			}
			UObject* Object = nullptr;
			if (!AddRigObject(Config, Identity, Object, OutError)) return false;
			UIKRetargeter* Retargeter = Cast<UIKRetargeter>(Object);
			UIKRetargeterController* Controller = Retargeter
				? UIKRetargeterController::GetController(Retargeter) : nullptr;
			const UIKRigDefinition* Source = Controller
				? Controller->GetIKRig(ERetargetSourceOrTarget::Source) : nullptr;
			const UIKRigDefinition* Target = Controller
				? Controller->GetIKRig(ERetargetSourceOrTarget::Target) : nullptr;
			if (!Retargeter || !Controller || !Source || !Target || Source == Target)
			{
				OutError = TEXT("timeline_spatial_retargeter_binding_unavailable:") + Identity;
				return false;
			}
			const FString SourceIdentity = Source->GetPathName();
			const FString TargetIdentity = Target->GetPathName();
			if (!AddIkBinding(Config, SourceIdentity, OutError)
				|| !AddIkBinding(Config, TargetIdentity, OutError)) return false;
			RigSpatial::FUeAgentRigSimulationSpatialRetargeterBinding Binding;
			Binding.Identity = Identity;
			Binding.Retargeter = Retargeter;
			Binding.SourceRigIdentity = SourceIdentity;
			Binding.TargetRigIdentity = TargetIdentity;
			Binding.SourceNativePoseFrameId = RequestedSourceFrame.IsEmpty()
				? TEXT("t58://retarget/source/") + Source->GetName() : RequestedSourceFrame;
			Binding.TargetNativePoseFrameId = RequestedTargetFrame.IsEmpty()
				? TEXT("t58://retarget/target/") + Target->GetName() : RequestedTargetFrame;
			if (Binding.SourceNativePoseFrameId == Binding.TargetNativePoseFrameId)
				Binding.TargetNativePoseFrameId += TEXT("/target");
			if (!AddNativeRigFrame(Config, Binding.SourceNativePoseFrameId)
				|| !AddNativeRigFrame(Config, Binding.TargetNativePoseFrameId))
			{
				OutError = TEXT("timeline_spatial_retargeter_native_frame_invalid:") + Identity;
				return false;
			}
			if (!Binding.IsValid())
			{
				OutError = TEXT("timeline_spatial_retargeter_binding_invalid:") + Identity;
				return false;
			}
			Config.RigRetargeterBindings.Add(MoveTemp(Binding));
			return true;
		}

		bool AddRigBakeBinding(
			FUeAgentTimelineSpatialRuntimeConfig& Config,
			UWorld& World,
			const FString& SequenceIdentity,
			const FString& BindingIdentity,
			const FString& ClockId,
			FString& OutError)
		{
			UObject* Object = nullptr;
			if (!AddRigObject(Config, SequenceIdentity, Object, OutError)) return false;
			ULevelSequence* Sequence = Cast<ULevelSequence>(Object);
			FGuid BindingGuid;
			if (!Sequence || !FGuid::Parse(BindingIdentity, BindingGuid)
				|| !BindingGuid.IsValid())
			{
				OutError = TEXT("timeline_spatial_rig_bake_identity_invalid:") + BindingIdentity;
				return false;
			}
			TArray<UObject*, TInlineAllocator<1>> BoundObjects;
			Sequence->LocateBoundObjects(
				BindingGuid, UE::UniversalObjectLocator::FResolveParams(&World),
				nullptr, BoundObjects);
			USkeletalMeshComponent* Component = nullptr;
			for (UObject* Bound : BoundObjects)
			{
				Component = Cast<USkeletalMeshComponent>(Bound);
				if (!Component)
					if (AActor* Actor = Cast<AActor>(Bound))
						Component = Actor->FindComponentByClass<USkeletalMeshComponent>();
				if (Component) break;
			}
			if (!Component || !Component->GetSkeletalMeshAsset())
			{
				OutError = TEXT("timeline_spatial_rig_bake_component_unavailable:")
					+ BindingIdentity;
				return false;
			}
			RigSpatial::FUeAgentRigSimulationSpatialRigBakeBinding Binding;
			Binding.Identity = BindingIdentity;
			Binding.World = &World;
			Binding.Sequence = Sequence;
			Binding.SkeletalMeshComponent = Component;
			Binding.BindingGuid = BindingGuid;
			Binding.NativeAuthoredClockId = ClockId;
			Binding.NativeControlFrameId = TEXT("t58://control-frame/") + BindingIdentity;
			if (!Binding.IsValid() || !AddRigClock(Config, ClockId)
				|| !AddNativeRigFrame(Config, Binding.NativeControlFrameId))
			{
				OutError = TEXT("timeline_spatial_rig_bake_binding_invalid:") + BindingIdentity;
				return false;
			}
			Config.RigBakeBindings.Add(MoveTemp(Binding));
			return true;
		}

		bool ConfigureRequestBindings(
			const FUeAgentCommandRequest& Request,
			UWorld& World,
			FUeAgentTimelineSpatialRuntimeConfig& Config,
			FString& OutError)
		{
			OutError.Reset();
			const TSharedPtr<FJsonObject>& Params = Request.GetParams();
			TSharedPtr<FJsonObject> Input;
			if (!RequiredObject(Params, TEXT("input"), Input, OutError)) return false;
			FString Target;
			if (Params->HasField(TEXT("target"))
				&& !RequiredString(Params, TEXT("target"), Target, OutError)) return false;
			const FString& CommandId = Request.GetCommandId();

			if (CommandId == TEXT("animation.subject.pose.update")
				|| CommandId == TEXT("animation.subject.morph_weights.update"))
			{
				UObject* SubjectObject = ResolveExplicitObject(Target);
				UPoseableMeshComponent* Pose = Cast<UPoseableMeshComponent>(SubjectObject);
				USkeletalMeshComponent* Morph = Cast<USkeletalMeshComponent>(SubjectObject);
				if (AActor* Actor = Cast<AActor>(SubjectObject))
				{
					if (!Pose) Pose = Actor->FindComponentByClass<UPoseableMeshComponent>();
					if (!Morph) Morph = Actor->FindComponentByClass<USkeletalMeshComponent>();
				}
				TSharedPtr<FJsonObject> Point;
				FString Clock, Numerator, Denominator;
				if ((!Pose && !Morph)
					|| !RequiredObject(Input, TEXT("evaluation_point"), Point, OutError)
					|| !ReadRate(Point, Clock, Numerator, Denominator, OutError))
				{
					if (OutError.IsEmpty())
						OutError = TEXT("timeline_spatial_subject_binding_unavailable:") + Target;
					return false;
				}
				Timeline::FUeAgentTimelineAnimationSubjectBinding Binding;
				Binding.Identity = Target;
				Binding.PoseComponent = Pose;
				Binding.MorphComponent = Morph;
				Binding.NativeFrameId = TEXT("t58://subject-frame/") + Target;
				if (CommandId == TEXT("animation.subject.pose.update"))
				{
					TSharedPtr<FJsonObject> PoseFrame;
					if (!RequiredObject(Input, TEXT("pose_frame"), PoseFrame, OutError)
						|| !RequiredString(PoseFrame, TEXT("frame_id"), Binding.NativeFrameId,
							OutError)) return false;
				}
				Binding.EvaluationClockId = Clock;
				Binding.EvaluationRateNumerator = Numerator;
				Binding.EvaluationRateDenominator = Denominator;
				if (!Binding.IsValid())
				{
					OutError = TEXT("timeline_spatial_subject_binding_invalid:") + Target;
					return false;
				}
				Config.TimelineSubjectBindings.Add(MoveTemp(Binding));
				return true;
			}

			if (CommandId == TEXT("animation.ik_probe_candidate.apply"))
			{
				ULevelSequence* Sequence = Cast<ULevelSequence>(ResolveExplicitObject(Target));
				TSharedPtr<FJsonObject> Candidate;
				TSharedPtr<FJsonObject> CandidateFrame;
				TSharedPtr<FJsonObject> AuthoredTime;
				FString RigRef, GoalRef, NativeFrame, Clock, Numerator, Denominator;
				if (!Sequence || !RequiredString(Input, TEXT("rig_ref"), RigRef, OutError)
					|| !RequiredString(Input, TEXT("goal_ref"), GoalRef, OutError)
					|| !RequiredObject(Input, TEXT("candidate"), Candidate, OutError)
					|| !RequiredObject(Candidate, TEXT("frame"), CandidateFrame, OutError)
					|| !RequiredString(CandidateFrame, TEXT("frame_id"), NativeFrame, OutError)
					|| !RequiredObject(Input, TEXT("authored_time"), AuthoredTime, OutError)
					|| !ReadRate(AuthoredTime, Clock, Numerator, Denominator, OutError))
				{
					if (OutError.IsEmpty())
						OutError = TEXT("timeline_spatial_rig_candidate_sequence_unavailable:") + Target;
					return false;
				}
				UControlRig* Rig = nullptr;
				auto InspectTrack = [&](UMovieSceneTrack* Track)
				{
					if (auto* RigTrack = Cast<UMovieSceneControlRigParameterTrack>(Track))
					{
						UControlRig* CandidateRig = RigTrack->GetControlRig();
						if (CandidateRig && (RigRef == CandidateRig->GetPathName()
							|| RigRef == CandidateRig->GetName() || !Rig)) Rig = CandidateRig;
					}
				};
				if (UMovieScene* Scene = Sequence->GetMovieScene())
				{
					for (UMovieSceneTrack* Track : Scene->GetTracks()) InspectTrack(Track);
					for (const FMovieSceneBinding& MovieBinding : Scene->GetBindings())
						for (UMovieSceneTrack* Track : MovieBinding.GetTracks()) InspectTrack(Track);
				}
				if (!Rig)
				{
					OutError = TEXT("timeline_spatial_rig_candidate_binding_unavailable:") + RigRef;
					return false;
				}
				Timeline::FUeAgentTimelineAnimationRigCandidateBinding Binding;
				Binding.Identity = Target;
				Binding.Sequence = Sequence;
				Binding.Rig = Rig;
				Binding.RigIdentity = Rig->GetPathName();
				Binding.ControlName = FName(*GoalRef);
				Binding.NativeFrameId = NativeFrame;
				Binding.AuthoredClockId = Clock;
				Binding.AuthoredRateNumerator = Numerator;
				Binding.AuthoredRateDenominator = Denominator;
				if (!Binding.IsValid())
				{
					OutError = TEXT("timeline_spatial_rig_candidate_binding_invalid:") + Target;
					return false;
				}
				Config.TimelineRigCandidateBindings.Add(MoveTemp(Binding));
				return true;
			}

			if (CommandId == TEXT("rig.skeleton.preview_mesh.bind"))
			{
				FString SkeletonRef, MeshRef;
				UObject* SkeletonObject = nullptr;
				UObject* MeshObject = nullptr;
				if (!RequiredString(Input, TEXT("skeleton_ref"), SkeletonRef, OutError)
					|| !RequiredString(Input, TEXT("preview_mesh_ref"), MeshRef, OutError)
					|| !AddRigObject(Config, SkeletonRef, SkeletonObject, OutError)
					|| !AddRigObject(Config, MeshRef, MeshObject, OutError)
					|| !Cast<USkeleton>(SkeletonObject) || !Cast<USkeletalMesh>(MeshObject))
				{
					if (OutError.IsEmpty())
						OutError = TEXT("timeline_spatial_skeleton_preview_binding_invalid");
					return false;
				}
				RigSpatial::FUeAgentRigSimulationSpatialSkeletonBinding Binding;
				Binding.Identity = SkeletonRef;
				Binding.Skeleton = Cast<USkeleton>(SkeletonObject);
				Config.RigSkeletonBindings.Add(MoveTemp(Binding));
				return true;
			}

			if (CommandId == TEXT("rig.skeleton.socket.update"))
			{
				FString SkeletonRef, SocketRef, ParentRef, FrameId;
				TSharedPtr<FJsonObject> Transform;
				TSharedPtr<FJsonObject> Frame;
				UObject* SkeletonObject = nullptr;
				if (!RequiredString(Input, TEXT("skeleton_ref"), SkeletonRef, OutError)
					|| !RequiredString(Input, TEXT("socket_ref"), SocketRef, OutError)
					|| !RequiredString(Input, TEXT("parent_bone_ref"), ParentRef, OutError)
					|| !RequiredObject(Input, TEXT("socket_transform"), Transform, OutError)
					|| !RequiredObject(Transform, TEXT("frame"), Frame, OutError)
					|| !AddRigFrame(Config, Frame, true, FrameId, OutError)
					|| !AddRigObject(Config, SkeletonRef, SkeletonObject, OutError)
					|| !Cast<USkeleton>(SkeletonObject)) return false;
				RigSpatial::FUeAgentRigSimulationSpatialSkeletonBinding SkeletonBinding;
				SkeletonBinding.Identity = SkeletonRef;
				SkeletonBinding.Skeleton = Cast<USkeleton>(SkeletonObject);
				Config.RigSkeletonBindings.Add(MoveTemp(SkeletonBinding));
				RigSpatial::FUeAgentRigSimulationSpatialSocketBinding SocketBinding;
				SocketBinding.SkeletonIdentity = SkeletonRef;
				SocketBinding.SocketIdentity = SocketRef;
				SocketBinding.SocketName = FName(*SocketRef);
				SocketBinding.ParentBoneIdentity = ParentRef;
				SocketBinding.ParentBoneName = FName(*ParentRef);
				SocketBinding.NativeParentFrameId = FrameId;
				if (!SocketBinding.IsValid())
				{
					OutError = TEXT("timeline_spatial_socket_binding_invalid:") + SocketRef;
					return false;
				}
				Config.RigSocketBindings.Add(MoveTemp(SocketBinding));
				return true;
			}

			if (CommandId == TEXT("rig.control.bake.animation")
				|| CommandId == TEXT("rig.control.bake.controls"))
			{
				FString SequenceRef, BindingRef, RigRef, ClockId;
				TSharedPtr<FJsonObject> Range;
				if (!RequiredString(Input,
						CommandId.EndsWith(TEXT("animation"))
							? TEXT("source_sequence_ref") : TEXT("target_sequence_ref"),
						SequenceRef, OutError)
					|| !RequiredString(Input, TEXT("binding_ref"), BindingRef, OutError)
					|| !RequiredString(Input, TEXT("rig_ref"), RigRef, OutError)
					|| !RequiredObject(Input, TEXT("authored_range"), Range, OutError)
					|| !RequiredString(Range, TEXT("clock_id"), ClockId, OutError)
					|| !AddRigBakeBinding(Config, World, SequenceRef, BindingRef, ClockId,
						OutError)) return false;
				UObject* RigObject = nullptr;
				if (!AddRigObject(Config, RigRef, RigObject, OutError)
					|| !Cast<UClass>(RigObject)) return false;
				if (CommandId.EndsWith(TEXT("animation")))
				{
					FString TargetAnimationRef;
					UObject* TargetAnimation = nullptr;
					if (!RequiredString(Input, TEXT("target_animation_ref"),
							TargetAnimationRef, OutError)
						|| !AddRigObject(Config, TargetAnimationRef, TargetAnimation, OutError))
						return false;
				}
				return true;
			}

			if (CommandId == TEXT("rig.ik.definition.update"))
			{
				FString IkRef;
				if (!RequiredString(Input, TEXT("ik_rig_ref"), IkRef, OutError)
					|| !AddIkBinding(Config, IkRef, OutError)) return false;
				RigSpatial::FUeAgentRigSimulationSpatialIkRigBinding* Binding =
					FindIkBinding(Config, IkRef);
				const TArray<TSharedPtr<FJsonValue>>* Changes = nullptr;
				if (!Binding || !Input->TryGetArrayField(TEXT("definition_changes"), Changes)
					|| !Changes)
				{
					OutError = TEXT("timeline_spatial_ik_definition_bindings_invalid:") + IkRef;
					return false;
				}
				for (const TSharedPtr<FJsonValue>& Value : *Changes)
				{
					const TSharedPtr<FJsonObject>* Change = nullptr;
					if (!Value.IsValid() || !Value->TryGetObject(Change) || !Change || !Change->IsValid())
					{
						OutError = TEXT("timeline_spatial_ik_definition_change_invalid:") + IkRef;
						return false;
					}
					FString Kind;
					FString Primary;
					if (!RequiredString(*Change, TEXT("kind"), Kind, OutError)
						|| !RequiredString(*Change, TEXT("primary_ref"), Primary, OutError))
						return false;
					if (Kind == TEXT("goal"))
						Binding->GoalBindings.FindOrAdd(Primary, NativeNameFromIdentity(Primary));
					else if (Kind == TEXT("retarget_chain"))
						Binding->ChainBindings.FindOrAdd(Primary, NativeNameFromIdentity(Primary));
					FString Secondary;
					if ((*Change)->TryGetStringField(TEXT("secondary_ref"), Secondary))
						AddExistingIdentityAlias(Binding->BoneBindings, Secondary);
					FString Tertiary;
					if ((*Change)->TryGetStringField(TEXT("tertiary_ref"), Tertiary))
						AddExistingIdentityAlias(Binding->BoneBindings, Tertiary);
				}
				const TSharedPtr<FJsonObject>* Preview = nullptr;
				if (Input->TryGetObjectField(TEXT("preview_context"), Preview)
					&& Preview && Preview->IsValid())
				{
					FString MeshRef;
					UObject* Mesh = nullptr;
					if (!RequiredString(*Preview, TEXT("preview_mesh_ref"), MeshRef, OutError)
						|| !AddRigObject(Config, MeshRef, Mesh, OutError)) return false;
				}
				return true;
			}

			if (CommandId.StartsWith(TEXT("rig.retarget.")))
			{
				FString RetargeterRef;
				if (!RequiredString(Input, TEXT("retargeter_ref"), RetargeterRef, OutError))
					return false;
				FString SourceFrame;
				FString TargetFrame;
				if (CommandId == TEXT("rig.retarget.pose.update"))
				{
					FString Side;
					TSharedPtr<FJsonObject> PoseFrame;
					if (!RequiredString(Input, TEXT("subject_side"), Side, OutError)
						|| !RequiredObject(Input, TEXT("pose_frame"), PoseFrame, OutError))
						return false;
					FString FrameId;
					if (!AddRigFrame(Config, PoseFrame, true, FrameId, OutError)) return false;
					if (Side == TEXT("source")) SourceFrame = FrameId;
					else TargetFrame = FrameId;
				}
				if (!AddRetargeterBinding(
						Config, RetargeterRef, SourceFrame, TargetFrame, OutError)) return false;
				if (CommandId == TEXT("rig.retarget.definition.update"))
				{
					FString SourceRef, TargetRef;
					if (!RequiredString(Input, TEXT("source_rig_ref"), SourceRef, OutError)
						|| !RequiredString(Input, TEXT("target_rig_ref"), TargetRef, OutError)
						|| !AddIkBinding(Config, SourceRef, OutError)
						|| !AddIkBinding(Config, TargetRef, OutError)) return false;
					RigSpatial::FUeAgentRigSimulationSpatialIkRigBinding* SourceBinding =
						FindIkBinding(Config, SourceRef);
					RigSpatial::FUeAgentRigSimulationSpatialIkRigBinding* TargetBinding =
						FindIkBinding(Config, TargetRef);
					const TArray<TSharedPtr<FJsonValue>>* Mappings = nullptr;
					if (!SourceBinding || !TargetBinding
						|| !Input->TryGetArrayField(TEXT("chain_mappings"), Mappings) || !Mappings)
					{
						OutError = TEXT("timeline_spatial_retarget_definition_bindings_invalid:")
							+ RetargeterRef;
						return false;
					}
					for (const TSharedPtr<FJsonValue>& Value : *Mappings)
					{
						const TSharedPtr<FJsonObject>* Mapping = nullptr;
						if (!Value.IsValid() || !Value->TryGetObject(Mapping)
							|| !Mapping || !Mapping->IsValid()) continue;
						FString Identity;
						if ((*Mapping)->TryGetStringField(TEXT("source_chain_ref"), Identity))
							AddExistingIdentityAlias(SourceBinding->ChainBindings, Identity);
						if ((*Mapping)->TryGetStringField(TEXT("target_chain_ref"), Identity))
							AddExistingIdentityAlias(TargetBinding->ChainBindings, Identity);
						if ((*Mapping)->TryGetStringField(TEXT("goal_ref"), Identity))
							AddExistingIdentityAlias(TargetBinding->GoalBindings, Identity);
					}
				}
				if (CommandId == TEXT("rig.retarget.assets.generate"))
				{
					FString SourceRef, TargetRef, DestinationRoot;
					UObject* Source = nullptr;
					UObject* TargetObject = nullptr;
					if (!RequiredString(Input, TEXT("source_subject_ref"), SourceRef, OutError)
						|| !RequiredString(Input, TEXT("target_subject_ref"), TargetRef, OutError)
						|| !RequiredString(Input, TEXT("destination_root"), DestinationRoot, OutError)
						|| !AddRigObject(Config, SourceRef, Source, OutError)
						|| !AddRigObject(Config, TargetRef, TargetObject, OutError)) return false;
					if (DestinationRoot != Target)
					{
						OutError = TEXT("timeline_spatial_retarget_destination_target_mismatch:")
							+ DestinationRoot;
						return false;
					}
					const auto* RetargeterBinding = Config.RigRetargeterBindings.FindByPredicate(
						[&](const auto& Binding) { return Binding.Identity == RetargeterRef; });
					if (!RetargeterBinding || !RetargeterBinding->Retargeter.IsValid())
					{
						OutError = TEXT("timeline_spatial_retarget_destination_binding_invalid:")
							+ DestinationRoot;
						return false;
					}
					Config.RigObjectBindings.Emplace(
						DestinationRoot, RetargeterBinding->Retargeter.Get());
					RigSpatial::FUeAgentRigSimulationSpatialExecutionError Error;
					if (!RigSpatial::TryCreateRigSimulationSpatialProductionAssetEffectPort(
							Config.RigAssetEffectPort, Error))
					{
						OutError = Error.Code + TEXT(":") + Error.Detail;
						return false;
					}
				}
				return true;
			}

			OutError = TEXT("timeline_spatial_request_binding_command_unsupported:")
				+ CommandId;
			return false;
		}

		bool TryCreateMechanics(
			UWorld& World,
			const FString& SessionId,
			TMap<FString, TSharedRef<IUeAgentCommandHandler>>& OutHandlers,
			TArray<FString>& OutScopes,
			FString& OutError,
			const FUeAgentCommandRequest* BoundRequest = nullptr)
		{
			OutHandlers.Reset();
			OutScopes.Reset();
			OutError.Reset();
			if (!IsInGameThread())
			{
				OutError = TEXT("timeline_spatial_mechanics_game_thread_required");
				return false;
			}
			if (!IsValid(&World) || SessionId.IsEmpty())
			{
				OutError = TEXT("timeline_spatial_mechanics_context_invalid");
				return false;
			}

			TSharedPtr<const FUeAgentOperationTicketService> Tickets;
			FUeAgentOperationTicketError TicketError;
			if (!FUeAgentOperationTicketService::TryCreate(
					FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower),
					300.0,
					[]() { return FPlatformTime::Seconds(); },
					[]() { return FGuid::NewGuid().ToString(EGuidFormats::Digits); },
					Tickets, TicketError))
			{
				OutError = TicketError.Code + TEXT(":") + TicketError.Detail;
				return false;
			}
			TSharedPtr<const FUeAgentResponseProjector> Projector;
			FUeAgentProjectionError ProjectionError;
			if (!FUeAgentResponseProjector::TryCreate(
					MakeShared<FConservativeTokenEstimator>(), Projector, ProjectionError))
			{
				OutError = ProjectionError.Code + TEXT(":") + ProjectionError.Detail;
				return false;
			}

			FUeAgentSimpleAuthoringRuntimeConfig SimpleConfig;
			SimpleConfig.ProjectInstanceId = TEXT("project:") + FString(FApp::GetProjectName());
			SimpleConfig.EditorInstanceId = FString::Printf(
				TEXT("editor-process:%u"), FPlatformProcess::GetCurrentProcessId());
			SimpleConfig.ServerSessionId = SessionId;
			SimpleConfig.RegistryEpoch = 1;
			SimpleConfig.TimeoutSeconds = 120.0;
			SimpleConfig.MaxFrameBytes = 16 * 1024 * 1024;
			SimpleConfig.JournalRoot = FPaths::ProjectSavedDir()
				/ TEXT("UeAgentInterface/SimpleAuthoring/TimelineSpatialMechanicsFoundation");
			TSharedPtr<FUeAgentSimpleAuthoringRuntime> SimpleRuntime;
			FUeAgentSimpleAuthoringRuntimeError SimpleError;
			if (!FUeAgentSimpleAuthoringRuntime::TryCreate(
					SimpleConfig, World, Tickets.ToSharedRef(), Projector.ToSharedRef(),
					SimpleRuntime, SimpleError)
				|| !SimpleRuntime.IsValid())
			{
				OutError = SimpleError.Code + TEXT(":") + SimpleError.Detail;
				return false;
			}
			TSharedPtr<const FUeAgentDiscoveryService> Discovery;
			FUeAgentDiscoveryError DiscoveryError;
			if (!FUeAgentDiscoveryService::TryCreate(
					SimpleRuntime->GetCatalogSources(), {}, Discovery, DiscoveryError)
				|| !Discovery.IsValid())
			{
				OutError = DiscoveryError.Code + TEXT(":") + DiscoveryError.Detail;
				return false;
			}

			FUeAgentTimelineSpatialRuntimeConfig RuntimeConfig;
			RuntimeConfig.ProjectInstanceId = SimpleConfig.ProjectInstanceId;
			RuntimeConfig.EditorInstanceId = SimpleConfig.EditorInstanceId;
			RuntimeConfig.ServerSessionId = SessionId;
			RuntimeConfig.RegistryEpoch = 1;
			RuntimeConfig.TimeoutSeconds = 120.0;
			RuntimeConfig.MaxFrameBytes = 16 * 1024 * 1024;
			if (BoundRequest
				&& !ConfigureRequestBindings(*BoundRequest, World, RuntimeConfig, OutError))
			{
				return false;
			}
			TSharedPtr<FUeAgentTimelineSpatialRuntime> Runtime;
			FUeAgentTimelineSpatialRuntimeError RuntimeError;
			if (!FUeAgentTimelineSpatialRuntime::TryCreate(
					RuntimeConfig, World, Tickets.ToSharedRef(), Discovery.ToSharedRef(),
					Projector.ToSharedRef(), Runtime, RuntimeError)
				|| !Runtime.IsValid())
			{
				OutError = RuntimeError.Code + TEXT(":") + RuntimeError.Detail;
				return false;
			}
			for (const TSharedRef<IUeAgentCommandHandler>& Handler : Runtime->GetCommandHandlers())
			{
				if (OutHandlers.Contains(Handler->GetCommandId()))
				{
					OutHandlers.Reset();
					OutError = TEXT("timeline_spatial_mechanics_duplicate:") + Handler->GetCommandId();
					return false;
				}
				OutHandlers.Add(Handler->GetCommandId(), Handler);
			}
			OutScopes = Runtime->GetAuthorityScopes();
			if (OutHandlers.Num() != Runtime->GetCommandHandlers().Num())
			{
				OutHandlers.Reset();
				OutError = TEXT("timeline_spatial_mechanics_count_mismatch");
				return false;
			}
			return true;
		}

		bool IsRequestBoundRigSpatialCommand(const FString& CommandId)
		{
			static const TSet<FString> Commands = {
				TEXT("animation.ik_probe_candidate.apply"),
				TEXT("animation.subject.morph_weights.update"),
				TEXT("animation.subject.pose.update"),
				TEXT("rig.control.bake.animation"),
				TEXT("rig.control.bake.controls"),
				TEXT("rig.ik.definition.update"),
				TEXT("rig.retarget.assets.generate"),
				TEXT("rig.retarget.definition.update"),
				TEXT("rig.retarget.pose.auto_align"),
				TEXT("rig.retarget.pose.update"),
				TEXT("rig.skeleton.preview_mesh.bind"),
				TEXT("rig.skeleton.socket.update"),
			};
			return Commands.Contains(CommandId);
		}

		FUeAgentCommandResult RequestBindingFailure(
			const FUeAgentCommandRequest& Request,
			const FString& Detail)
		{
			const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(
				TEXT("code"), TEXT("timeline_spatial_request_binding_unavailable"));
			Error->SetStringField(TEXT("subject_id"), Request.GetCommandId());
			Error->SetStringField(TEXT("detail"), Detail);
			FString Text;
			const auto Writer = TJsonWriterFactory<TCHAR,
				TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
			FJsonSerializer::Serialize(Error, Writer);
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"), Text);
		}

		class FRequestBoundMechanicsHandler final : public IUeAgentCommandHandler
		{
		public:
			FRequestBoundMechanicsHandler(
				FString InCommandId,
				UWorld& InWorld,
				FString InSessionId)
				: CommandId(MoveTemp(InCommandId))
				, BoundWorld(&InWorld)
				, SessionId(MoveTemp(InSessionId))
			{
			}

			const FString& GetCommandId() const override { return CommandId; }

			FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const Contract::FUeAgentCallerContext& Caller,
				const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation) override
			{
				TMap<FString, TSharedRef<IUeAgentCommandHandler>> Handlers;
				TArray<FString> Scopes;
				FString Error;
				UWorld* const World = BoundWorld.Get();
				UWorld* const CurrentWorld = IsInGameThread() && GEditor
					? GEditor->GetEditorWorldContext().World()
					: nullptr;
				if (Request.GetCommandId() != CommandId
					|| !World
					|| CurrentWorld != World)
				{
					return RequestBindingFailure(
						Request,
						TEXT("timeline_spatial_request_bound_world_changed"));
				}
				if (!TryCreateMechanics(
						*World,
						SessionId,
						Handlers,
						Scopes,
						Error,
						&Request))
				{
					return RequestBindingFailure(Request, Error);
				}
				const TSharedRef<IUeAgentCommandHandler>* Found = Handlers.Find(CommandId);
				return Found ? (*Found)->Submit(Request, Caller, Cancellation)
					: RequestBindingFailure(
						Request, TEXT("timeline_spatial_request_bound_handler_missing:" )
							+ CommandId);
			}

		private:
			FString CommandId;
			TWeakObjectPtr<UWorld> BoundWorld;
			FString SessionId;
		};
	}

	bool TryResolveTimelineSpatialMechanics(
		const FString& CommandId,
		TSharedPtr<IUeAgentCommandHandler>& OutHandler,
		TArray<FString>& OutAuthorityScopes,
		FString& OutError)
	{
		OutHandler.Reset();
		OutAuthorityScopes.Reset();
		OutError.Reset();
		if (!IsInGameThread())
		{
			OutError = TEXT("timeline_spatial_mechanics_game_thread_required");
			return false;
		}
		UWorld* const World = GEditor
			? GEditor->GetEditorWorldContext().World()
			: nullptr;
		if (!World)
		{
			OutError = TEXT("timeline_spatial_mechanics_editor_world_unavailable");
			return false;
		}
		FProcessHost& Host = ProcessHost();
		FScopeLock Lock(&Host.Mutex);
		if (Host.Handlers.IsEmpty() || Host.BoundWorld.Get() != World)
		{
			TMap<FString, TSharedRef<IUeAgentCommandHandler>> NewHandlers;
			TArray<FString> NewScopes;
			const FString NewSessionId =
				TEXT("timeline-spatial-mechanics-session:")
				+ FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
			if (!TryCreateMechanics(
					*World,
					NewSessionId,
					NewHandlers,
					NewScopes,
					OutError))
			{
				Host.Handlers.Reset();
				Host.AuthorityScopes.Reset();
				Host.BoundWorld.Reset();
				Host.SessionId.Reset();
				return false;
			}
			Host.Handlers = MoveTemp(NewHandlers);
			Host.AuthorityScopes = MoveTemp(NewScopes);
			Host.BoundWorld = World;
			Host.SessionId = NewSessionId;
		}
		if (IsRequestBoundRigSpatialCommand(CommandId))
		{
			OutHandler = MakeShared<FRequestBoundMechanicsHandler>(
				CommandId,
				*World,
				Host.SessionId);
			OutAuthorityScopes = Host.AuthorityScopes;
			return true;
		}
		const TSharedRef<IUeAgentCommandHandler>* Found = Host.Handlers.Find(CommandId);
		if (!Found)
		{
			OutError = TEXT("timeline_spatial_mechanics_missing:") + CommandId;
			return false;
		}
		OutHandler = *Found;
		OutAuthorityScopes = Host.AuthorityScopes;
		return true;
	}
}
