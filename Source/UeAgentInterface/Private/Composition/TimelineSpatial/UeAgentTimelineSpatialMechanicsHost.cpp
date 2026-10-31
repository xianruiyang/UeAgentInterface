#include "Composition/TimelineSpatial/UeAgentTimelineSpatialMechanicsHost.h"

#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringRuntime.h"
#include "Composition/TimelineSpatial/UeAgentTimelineSpatialRuntime.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/Projection/UeAgentProjection.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
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
#include "MovieSceneBinding.h"
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
#include "UObject/Class.h"

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
					OutError = Cancelled;
					return false;
				}
				FUeAgentResourceChargeError ChargeError;
				if (!Resources.TryCharge(
						EUeAgentResourceDimension::WorkUnits, 1,
						TEXT("timeline_spatial.mechanics_projection"), ChargeError))
				{
					OutError = ChargeError;
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

		UE::UAI::Contract::FDiagnostic BindingFailure(
			const FString& Code, const FString& Reason, const FString& Pointer,
			const TSharedPtr<FJsonValue>& Actual, const FString& Expected,
			const FString& Target = FString())
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("timeline_spatial"));
			Error.Source.JsonPath = Pointer;
			Error.TargetPath = Target;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ActualValue = Actual;
			Error.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			return Error;
		}

		void BindFailureInput(UE::UAI::Contract::FDiagnostic& Error,
			const FString& Pointer, bool bRelated = false)
		{
			if (Error.Source.SourceFile.IsEmpty() && Error.Source.JsonPath.IsEmpty())
			{
				Error.Source.JsonPath = Pointer;
				if (bRelated) Error.SourceKind = TEXT("related_input");
			}
		}

		bool RequireObjectClass(UObject* Object, UClass* ExpectedClass,
			const FString& Identity, const FString& Pointer,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (Object && Object->IsA(ExpectedClass)) return true;
			OutError = BindingFailure(TEXT("timeline_spatial_object_class_invalid"),
				TEXT("The referenced object has an incompatible class."), Pointer,
				Object ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Object->GetClass()->GetPathName())) : nullptr,
				ExpectedClass->GetPathName(), Identity);
			return false;
		}

		bool RequiredString(
			const TSharedPtr<FJsonObject>& Object,
			const FString& ParentPointer,
			const TCHAR* Field,
			FString& OutValue,
			UE::UAI::Contract::FDiagnostic& OutError,
			bool bInternalValue = false)
		{
			OutValue.Reset();
			if (!Object.IsValid() || !Object->TryGetStringField(Field, OutValue)
				|| OutValue.IsEmpty())
			{
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("timeline_spatial_request_field_required"), ParentPointer + TEXT("/") + Field,
					MakeShared<FJsonValueString>(TEXT("non-empty string")), Object.IsValid() ? Object->TryGetField(Field) : nullptr,
					TEXT("timeline_spatial"), TEXT("The required field must contain a non-empty string."));
				OutError.ActualValuePresent = Object.IsValid() && Object->HasField(Field);
				if (bInternalValue) OutError.ActualEvidence = MoveTemp(OutError.ActualValue);
				return false;
			}
			return true;
		}

		bool RequiredObject(
			const TSharedPtr<FJsonObject>& Object,
			const FString& ParentPointer,
			const TCHAR* Field,
			TSharedPtr<FJsonObject>& OutValue,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutValue.Reset();
			const TSharedPtr<FJsonObject>* Found = nullptr;
			if (!Object.IsValid() || !Object->TryGetObjectField(Field, Found)
				|| !Found || !Found->IsValid())
			{
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("timeline_spatial_request_object_required"), ParentPointer + TEXT("/") + Field,
					MakeShared<FJsonValueString>(TEXT("object")), Object.IsValid() ? Object->TryGetField(Field) : nullptr,
					TEXT("timeline_spatial"), TEXT("The required field must be an object."));
				OutError.ActualValuePresent = Object.IsValid() && Object->HasField(Field);
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
			UE::UAI::Contract::FDiagnostic& OutError,
			const FString& Pointer)
		{
			OutObject = ResolveExplicitObject(Identity);
			if (!IsValid(OutObject))
			{
				OutError = BindingFailure(TEXT("timeline_spatial_explicit_object_unavailable"),
					TEXT("The referenced object could not be resolved or loaded."), Pointer,
					MakeShared<FJsonValueString>(Identity), TEXT("existing loadable object reference"), Identity);
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
			const FString& PointPointer,
			FString& OutClock,
			FString& OutNumerator,
			FString& OutDenominator,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			TSharedPtr<FJsonObject> Rate;
			return RequiredString(Point, PointPointer, TEXT("clock_id"), OutClock, OutError)
				&& RequiredObject(Point, PointPointer, TEXT("rate"), Rate, OutError)
				&& RequiredString(Rate, PointPointer + TEXT("/rate"), TEXT("numerator"), OutNumerator, OutError)
				&& RequiredString(Rate, PointPointer + TEXT("/rate"), TEXT("denominator"), OutDenominator, OutError);
		}

		bool AddRigClock(
			FUeAgentTimelineSpatialRuntimeConfig& Config,
			const FString& ClockId,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (ClockId.IsEmpty())
			{
				OutError = BindingFailure(TEXT("timeline_spatial_clock_invalid"),
					TEXT("The authored clock reference must not be empty."), TEXT("/input/authored_range/clock_id"),
					MakeShared<FJsonValueString>(ClockId), TEXT("non-empty clock reference"));
				return false;
			}
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
			const FString& FramePointer,
			bool bNative,
			FString& OutFrameId,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			FString BasisFrameId;
			FString Unit;
			FString BasisEvidenceId;
			double Dimension = 0.0;
			if (!RequiredString(Frame, FramePointer, TEXT("frame_id"), OutFrameId, OutError)
				|| !RequiredString(Frame, FramePointer, TEXT("basis_frame_id"), BasisFrameId, OutError)
				|| !Frame->TryGetNumberField(TEXT("dimension"), Dimension)
				|| static_cast<int32>(Dimension) != 3
				|| !RequiredString(Frame, FramePointer, TEXT("unit"), Unit, OutError)
				|| !RequiredString(Frame, FramePointer, TEXT("basis_evidence_id"), BasisEvidenceId, OutError, true))
			{
				if (OutError.IsEmpty())
				{
					OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("timeline_spatial_frame_invalid"), FramePointer + TEXT("/dimension"), MakeShared<FJsonValueNumber>(3), Frame->TryGetField(TEXT("dimension")), TEXT("timeline_spatial"), TEXT("This binding requires a three-dimensional frame."));
					OutError.ActualValuePresent = Frame->HasField(TEXT("dimension"));
				}
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
				OutError = SpatialFrame.GetValidationDiagnostic();
				const FString& Field = OutError.PropertyPath;
				if (Field == TEXT("frame_id") || Field == TEXT("basis_frame_id") || Field == TEXT("unit"))
					BindFailureInput(OutError, FramePointer + TEXT("/") + Field);
				else if (Field == TEXT("basis_evidence"))
					BindFailureInput(OutError, FramePointer + TEXT("/basis_evidence_id"));
				else
					BindFailureInput(OutError, FramePointer, true);
				return false;
			}
			Config.RigFrameBindings.Emplace(MoveTemp(SpatialFrame), bNative);
			return true;
		}

		bool AddNativeRigFrame(
			FUeAgentTimelineSpatialRuntimeConfig& Config,
			const FString& FrameId,
			UE::UAI::Contract::FDiagnostic& OutError,
			const FString& Unit = TEXT("cm"))
		{
			if (FrameId.IsEmpty())
			{
				OutError = BindingFailure(TEXT("timeline_spatial_native_frame_invalid"),
					TEXT("The native frame reference must not be empty."), FString(),
					MakeShared<FJsonValueString>(FrameId), TEXT("non-empty frame reference"));
				OutError.PropertyPath = TEXT("frame_id");
				return false;
			}
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
			if (!Frame.IsValid())
			{
				OutError = Frame.GetValidationDiagnostic();
				return false;
			}
			Config.RigFrameBindings.Emplace(MoveTemp(Frame), true);
			return true;
		}

		bool AddIkBinding(
			FUeAgentTimelineSpatialRuntimeConfig& Config,
			const FString& Identity,
			UE::UAI::Contract::FDiagnostic& OutError,
			const FString& Pointer)
		{
			if (Config.RigIkBindings.ContainsByPredicate(
				[&](const auto& Binding) { return Binding.Identity == Identity; }))
			{
				return true;
			}
			UObject* Object = nullptr;
			if (!AddRigObject(Config, Identity, Object, OutError, Pointer)) return false;
			UIKRigDefinition* Rig = Cast<UIKRigDefinition>(Object);
			if (!RequireObjectClass(Object, UIKRigDefinition::StaticClass(), Identity, Pointer, OutError)) return false;
			UIKRigController* Controller = Rig ? UIKRigController::GetController(Rig) : nullptr;
			USkeletalMesh* Mesh = Controller ? Controller->GetSkeletalMesh() : nullptr;
			if (!Rig || !Controller || !Mesh)
			{
				OutError = BindingFailure(TEXT("timeline_spatial_ik_binding_unavailable"),
					!Controller ? TEXT("The IK rig has no available controller.") : TEXT("The IK rig has no preview skeletal mesh."),
					Pointer, nullptr, !Controller ? TEXT("IK rig controller") : TEXT("preview skeletal mesh"), Identity);
				OutError.SourceKind = TEXT("related_input");
				return false;
			}
			RigSpatial::FUeAgentRigSimulationSpatialIkRigBinding Binding;
			Binding.Identity = Identity;
			Binding.Rig = Rig;
			Binding.PreviewMeshIdentity = Mesh->GetPathName();
			UObject* MeshObject = nullptr;
			if (!AddRigObject(Config, Binding.PreviewMeshIdentity, MeshObject, OutError, Pointer))
			{
				OutError.SourceKind = TEXT("related_input");
				return false;
			}
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
			if (!Binding.TryValidate(OutError))
			{
				BindFailureInput(OutError, Pointer, true);
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
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (Config.RigRetargeterBindings.ContainsByPredicate(
				[&](const auto& Binding) { return Binding.Identity == Identity; }))
			{
				return true;
			}
			UObject* Object = nullptr;
			const FString Pointer = TEXT("/input/retargeter_ref");
			if (!AddRigObject(Config, Identity, Object, OutError, Pointer)) return false;
			UIKRetargeter* Retargeter = Cast<UIKRetargeter>(Object);
			if (!RequireObjectClass(Object, UIKRetargeter::StaticClass(), Identity, Pointer, OutError)) return false;
			UIKRetargeterController* Controller = Retargeter
				? UIKRetargeterController::GetController(Retargeter) : nullptr;
			const UIKRigDefinition* Source = Controller
				? Controller->GetIKRig(ERetargetSourceOrTarget::Source) : nullptr;
			const UIKRigDefinition* Target = Controller
				? Controller->GetIKRig(ERetargetSourceOrTarget::Target) : nullptr;
			if (!Retargeter || !Controller || !Source || !Target || Source == Target)
			{
				const FString Reason = !Controller ? TEXT("The retargeter has no available controller.")
					: !Source ? TEXT("The retargeter has no source IK rig.")
					: !Target ? TEXT("The retargeter has no target IK rig.")
					: TEXT("The retargeter uses the same IK rig for source and target.");
				OutError = BindingFailure(TEXT("timeline_spatial_retargeter_binding_unavailable"), Reason,
					Pointer, Source && Source == Target ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Source->GetPathName())) : nullptr,
					!Controller ? TEXT("retargeter controller") : TEXT("distinct source and target IK rigs"), Identity);
				OutError.SourceKind = TEXT("related_input");
				return false;
			}
			const FString SourceIdentity = Source->GetPathName();
			const FString TargetIdentity = Target->GetPathName();
			if (!AddIkBinding(Config, SourceIdentity, OutError, Pointer)
				|| !AddIkBinding(Config, TargetIdentity, OutError, Pointer))
			{
				OutError.SourceKind = TEXT("related_input");
				return false;
			}
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
			if (!AddNativeRigFrame(Config, Binding.SourceNativePoseFrameId, OutError)
				|| !AddNativeRigFrame(Config, Binding.TargetNativePoseFrameId, OutError))
			{
				BindFailureInput(OutError, Pointer, true);
				return false;
			}
			if (!Binding.TryValidate(OutError))
			{
				BindFailureInput(OutError, Pointer, true);
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
			UE::UAI::Contract::FDiagnostic& OutError,
			const FString& SequencePointer,
			const FString& PublicIdentity = FString())
		{
			UObject* Object = nullptr;
			if (!AddRigObject(Config, SequenceIdentity, Object, OutError, SequencePointer)) return false;
			ULevelSequence* Sequence = Cast<ULevelSequence>(Object);
			if (!RequireObjectClass(Object, ULevelSequence::StaticClass(), SequenceIdentity, SequencePointer, OutError)) return false;
			FGuid BindingGuid;
			if (!Sequence || !FGuid::Parse(BindingIdentity, BindingGuid)
				|| !BindingGuid.IsValid())
			{
				OutError = BindingFailure(TEXT("timeline_spatial_rig_bake_identity_invalid"),
					TEXT("The sequence binding reference must be a valid non-zero GUID."), TEXT("/input/binding_ref"),
					MakeShared<FJsonValueString>(BindingIdentity), TEXT("non-zero GUID"), SequenceIdentity);
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
				OutError = BindingFailure(TEXT("timeline_spatial_rig_bake_component_unavailable"),
					!Component ? TEXT("The sequence binding resolves to no skeletal mesh component.")
						: TEXT("The bound skeletal mesh component has no skeletal mesh asset."),
					TEXT("/input/binding_ref"), MakeShared<FJsonValueString>(BindingIdentity),
					TEXT("binding to a skeletal mesh component with a mesh asset"), SequenceIdentity);
				return false;
			}
			RigSpatial::FUeAgentRigSimulationSpatialRigBakeBinding Binding;
			Binding.Identity = PublicIdentity.IsEmpty() ? BindingIdentity : PublicIdentity;
			Binding.World = &World;
			Binding.Sequence = Sequence;
			Binding.SkeletalMeshComponent = Component;
			Binding.BindingGuid = BindingGuid;
			Binding.NativeAuthoredClockId = ClockId;
			Binding.NativeControlFrameId = TEXT("t58://control-frame/") + BindingIdentity;
			if (!Binding.TryValidate(OutError))
			{
				const FString& Field = OutError.PropertyPath;
				if (Field == TEXT("native_authored_clock_id"))
					BindFailureInput(OutError, TEXT("/input/authored_range/clock_id"));
				else if (Field == TEXT("sequence"))
					BindFailureInput(OutError, SequencePointer);
				else
					BindFailureInput(OutError, TEXT("/input/binding_ref"), Field != TEXT("identity") && Field != TEXT("binding_guid"));
				return false;
			}
			if (!AddRigClock(Config, ClockId, OutError)) return false;
			if (!AddNativeRigFrame(Config, Binding.NativeControlFrameId, OutError))
			{
				BindFailureInput(OutError, TEXT("/input/binding_ref"), true);
				return false;
			}
			Config.RigBakeBindings.Add(MoveTemp(Binding));
			return true;
		}

		bool ConfigureRequestBindingsImpl(
			const FUeAgentCommandRequest& Request,
			UWorld& World,
			FUeAgentTimelineSpatialRuntimeConfig& Config,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutError.Reset();
			const TSharedPtr<FJsonObject>& Params = Request.GetParams();
			TSharedPtr<FJsonObject> Input;
			if (!RequiredObject(Params, FString(), TEXT("input"), Input, OutError)) return false;
			FString Target;
			if (Params->HasField(TEXT("target"))
				&& !RequiredString(Params, FString(), TEXT("target"), Target, OutError)) return false;
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
					|| !RequiredObject(Input, TEXT("/input"), TEXT("evaluation_point"), Point, OutError)
					|| !ReadRate(Point, TEXT("/input/evaluation_point"), Clock, Numerator, Denominator, OutError))
				{
					if (OutError.IsEmpty())
						OutError = BindingFailure(TEXT("timeline_spatial_subject_binding_unavailable"),
							!SubjectObject ? TEXT("The target object could not be resolved or loaded.")
								: TEXT("The target has neither a poseable mesh component nor a skeletal mesh component."),
							TEXT("/target"), MakeShared<FJsonValueString>(Target),
							TEXT("poseable or skeletal mesh component, or an actor containing one"), Target);
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
					if (!RequiredObject(Input, TEXT("/input"), TEXT("pose_frame"), PoseFrame, OutError)
						|| !RequiredString(PoseFrame, TEXT("/input/pose_frame"), TEXT("frame_id"), Binding.NativeFrameId,
							OutError)) return false;
				}
				Binding.EvaluationClockId = Clock;
				Binding.EvaluationRateNumerator = Numerator;
				Binding.EvaluationRateDenominator = Denominator;
				if (!Binding.TryValidate(OutError))
				{
					const FString& Field = OutError.PropertyPath;
					if (Field.StartsWith(TEXT("evaluation_rate.")))
						BindFailureInput(OutError, TEXT("/input/evaluation_point/rate/") + Field.Mid(16));
					else if (Field == TEXT("evaluation_clock_id"))
						BindFailureInput(OutError, TEXT("/input/evaluation_point/clock_id"));
					else if (Field == TEXT("native_frame_id") && CommandId == TEXT("animation.subject.pose.update"))
						BindFailureInput(OutError, TEXT("/input/pose_frame/frame_id"));
					else
						BindFailureInput(OutError, TEXT("/target"), Field != TEXT("identity"));
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
				if (!Sequence || !RequiredString(Input, TEXT("/input"), TEXT("rig_ref"), RigRef, OutError)
					|| !RequiredString(Input, TEXT("/input"), TEXT("goal_ref"), GoalRef, OutError)
					|| !RequiredObject(Input, TEXT("/input"), TEXT("candidate"), Candidate, OutError)
					|| !RequiredObject(Candidate, TEXT("/input/candidate"), TEXT("frame"), CandidateFrame, OutError)
					|| !RequiredString(CandidateFrame, TEXT("/input/candidate/frame"), TEXT("frame_id"), NativeFrame, OutError)
					|| !RequiredObject(Input, TEXT("/input"), TEXT("authored_time"), AuthoredTime, OutError)
					|| !ReadRate(AuthoredTime, TEXT("/input/authored_time"), Clock, Numerator, Denominator, OutError))
				{
					if (OutError.IsEmpty())
						OutError = BindingFailure(TEXT("timeline_spatial_rig_candidate_sequence_unavailable"),
							TEXT("The target could not be resolved as a level sequence."), TEXT("/target"),
							MakeShared<FJsonValueString>(Target), TEXT("level sequence reference"), Target);
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
					OutError = BindingFailure(TEXT("timeline_spatial_rig_candidate_binding_unavailable"),
						TEXT("The level sequence has no available Control Rig in its parameter tracks."),
						TEXT("/input/rig_ref"), MakeShared<FJsonValueString>(RigRef),
						TEXT("Control Rig available in the target sequence"), Target);
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
				if (!Binding.TryValidate(OutError))
				{
					const FString& Field = OutError.PropertyPath;
					if (Field.StartsWith(TEXT("authored_rate.")))
						BindFailureInput(OutError, TEXT("/input/authored_time/rate/") + Field.Mid(14));
					else if (Field == TEXT("authored_clock_id"))
						BindFailureInput(OutError, TEXT("/input/authored_time/clock_id"));
					else if (Field == TEXT("control_name"))
						BindFailureInput(OutError, TEXT("/input/goal_ref"));
					else if (Field == TEXT("native_frame_id"))
						BindFailureInput(OutError, TEXT("/input/candidate/frame/frame_id"));
					else if (Field == TEXT("rig") || Field == TEXT("rig_identity"))
						BindFailureInput(OutError, TEXT("/input/rig_ref"), true);
					else
						BindFailureInput(OutError, TEXT("/target"), Field != TEXT("identity"));
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
				if (!RequiredString(Input, TEXT("/input"), TEXT("skeleton_ref"), SkeletonRef, OutError)
					|| !RequiredString(Input, TEXT("/input"), TEXT("preview_mesh_ref"), MeshRef, OutError)
					|| !AddRigObject(Config, SkeletonRef, SkeletonObject, OutError, TEXT("/input/skeleton_ref"))
					|| !AddRigObject(Config, MeshRef, MeshObject, OutError, TEXT("/input/preview_mesh_ref"))
					|| !RequireObjectClass(SkeletonObject, USkeleton::StaticClass(), SkeletonRef, TEXT("/input/skeleton_ref"), OutError)
					|| !RequireObjectClass(MeshObject, USkeletalMesh::StaticClass(), MeshRef, TEXT("/input/preview_mesh_ref"), OutError))
				{
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
				if (!RequiredString(Input, TEXT("/input"), TEXT("skeleton_ref"), SkeletonRef, OutError)
					|| !RequiredString(Input, TEXT("/input"), TEXT("socket_ref"), SocketRef, OutError)
					|| !RequiredString(Input, TEXT("/input"), TEXT("parent_bone_ref"), ParentRef, OutError)
					|| !RequiredObject(Input, TEXT("/input"), TEXT("socket_transform"), Transform, OutError)
					|| !RequiredObject(Transform, TEXT("/input/socket_transform"), TEXT("frame"), Frame, OutError)
					|| !AddRigFrame(Config, Frame, TEXT("/input/socket_transform/frame"), true, FrameId, OutError)
					|| !AddRigObject(Config, SkeletonRef, SkeletonObject, OutError, TEXT("/input/skeleton_ref"))
					|| !RequireObjectClass(SkeletonObject, USkeleton::StaticClass(), SkeletonRef, TEXT("/input/skeleton_ref"), OutError)) return false;
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
				if (!SocketBinding.TryValidate(OutError))
				{
					const FString& Field = OutError.PropertyPath;
					if (Field == TEXT("skeleton_identity"))
						BindFailureInput(OutError, TEXT("/input/skeleton_ref"));
					else if (Field == TEXT("socket_identity") || Field == TEXT("socket_name"))
						BindFailureInput(OutError, TEXT("/input/socket_ref"));
					else if (Field == TEXT("parent_bone_identity") || Field == TEXT("parent_bone_name"))
						BindFailureInput(OutError, TEXT("/input/parent_bone_ref"));
					else
						BindFailureInput(OutError, TEXT("/input/socket_transform/frame/frame_id"));
					return false;
				}
				Config.RigSocketBindings.Add(MoveTemp(SocketBinding));
				return true;
			}

			if (CommandId == TEXT("rig.control.bake.context"))
			{
				FString SequenceRef, BindingName, RigRef;
				UObject* SequenceObject = nullptr;
				UObject* RigObject = nullptr;
				if (!RequiredString(Input, TEXT("/input"), TEXT("sequence_ref"), SequenceRef, OutError)
					|| !RequiredString(Input, TEXT("/input"), TEXT("binding_name"), BindingName, OutError)
					|| !RequiredString(Input, TEXT("/input"), TEXT("rig_ref"), RigRef, OutError)
					|| !AddRigObject(Config, SequenceRef, SequenceObject, OutError, TEXT("/input/sequence_ref"))
					|| !RequireObjectClass(SequenceObject, ULevelSequence::StaticClass(), SequenceRef, TEXT("/input/sequence_ref"), OutError)
					|| !AddRigObject(Config, RigRef, RigObject, OutError, TEXT("/input/rig_ref"))
					|| !RequireObjectClass(RigObject, UClass::StaticClass(), RigRef, TEXT("/input/rig_ref"), OutError)) return false;
				ULevelSequence* Sequence = CastChecked<ULevelSequence>(SequenceObject);
				UMovieScene* MovieScene = Sequence->GetMovieScene();
				FGuid BindingGuid;
				int32 Matches = 0;
				if (MovieScene)
				{
					for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
					{
						if (Binding.GetName() == BindingName)
						{
							BindingGuid = Binding.GetObjectGuid();
							++Matches;
						}
					}
				}
				if (Matches != 1)
				{
					OutError = BindingFailure(TEXT("timeline_spatial_rig_bake_binding_name_unresolved"),
						TEXT("The binding name must identify exactly one native MovieScene binding."),
						TEXT("/input/binding_name"), MakeShared<FJsonValueNumber>(Matches),
						TEXT("one binding with the exact readable name"), SequenceRef);
					return false;
				}
				if (!AddRigBakeBinding(Config, World, SequenceRef, BindingGuid.ToString(),
					TEXT("level_sequence:") + SequenceRef, OutError, TEXT("/input/sequence_ref"), BindingName)) return false;
				if (Input->HasField(TEXT("target_animation_ref")))
				{
					FString AnimationRef;
					UObject* Animation = nullptr;
					if (!RequiredString(Input, TEXT("/input"), TEXT("target_animation_ref"), AnimationRef, OutError)
						|| !AddRigObject(Config, AnimationRef, Animation, OutError, TEXT("/input/target_animation_ref"))) return false;
				}
				return true;
			}

			if (CommandId == TEXT("rig.control.bake.animation")
				|| CommandId == TEXT("rig.control.bake.controls"))
			{
				FString SequenceRef, BindingRef, RigRef, ClockId;
				const FString SequencePointer = CommandId.EndsWith(TEXT("animation"))
					? TEXT("/input/source_sequence_ref") : TEXT("/input/target_sequence_ref");
				TSharedPtr<FJsonObject> Range;
				if (!RequiredString(Input, TEXT("/input"),
						CommandId.EndsWith(TEXT("animation"))
							? TEXT("source_sequence_ref") : TEXT("target_sequence_ref"),
						SequenceRef, OutError)
					|| !RequiredString(Input, TEXT("/input"), TEXT("binding_ref"), BindingRef, OutError)
					|| !RequiredString(Input, TEXT("/input"), TEXT("rig_ref"), RigRef, OutError)
					|| !RequiredObject(Input, TEXT("/input"), TEXT("authored_range"), Range, OutError)
					|| !RequiredString(Range, TEXT("/input/authored_range"), TEXT("clock_id"), ClockId, OutError)
					|| !AddRigBakeBinding(Config, World, SequenceRef, BindingRef, ClockId,
						OutError, SequencePointer)) return false;
				UObject* RigObject = nullptr;
				if (!AddRigObject(Config, RigRef, RigObject, OutError, TEXT("/input/rig_ref"))
					|| !RequireObjectClass(RigObject, UClass::StaticClass(), RigRef, TEXT("/input/rig_ref"), OutError)) return false;
				if (CommandId.EndsWith(TEXT("animation")))
				{
					FString TargetAnimationRef;
					UObject* TargetAnimation = nullptr;
					if (!RequiredString(Input, TEXT("/input"), TEXT("target_animation_ref"),
							TargetAnimationRef, OutError)
						|| !AddRigObject(Config, TargetAnimationRef, TargetAnimation, OutError, TEXT("/input/target_animation_ref")))
						return false;
				}
				return true;
			}

			if (CommandId == TEXT("rig.ik.definition.update"))
			{
				FString IkRef;
				if (!RequiredString(Input, TEXT("/input"), TEXT("ik_rig_ref"), IkRef, OutError)
					|| !AddIkBinding(Config, IkRef, OutError, TEXT("/input/ik_rig_ref"))) return false;
				RigSpatial::FUeAgentRigSimulationSpatialIkRigBinding* Binding =
					FindIkBinding(Config, IkRef);
				const TArray<TSharedPtr<FJsonValue>>* Changes = nullptr;
				if (!Binding || !Input->TryGetArrayField(TEXT("definition_changes"), Changes)
					|| !Changes)
				{
					OutError = BindingFailure(TEXT("timeline_spatial_ik_definition_bindings_invalid"),
						!Binding ? TEXT("The IK rig binding is missing after initialization.") : TEXT("Definition changes must be an array."),
						!Binding ? TEXT("/input/ik_rig_ref") : TEXT("/input/definition_changes"),
						!Binding ? MakeShared<FJsonValueString>(IkRef) : Input->TryGetField(TEXT("definition_changes")),
						!Binding ? TEXT("initialized IK rig binding") : TEXT("array"), IkRef);
					return false;
				}
				for (int32 Index = 0; Index < Changes->Num(); ++Index)
				{
					const TSharedPtr<FJsonValue>& Value = (*Changes)[Index];
					const FString ChangePointer = FString::Printf(TEXT("/input/definition_changes/%d"), Index);
					const TSharedPtr<FJsonObject>* Change = nullptr;
					if (!Value.IsValid() || !Value->TryGetObject(Change) || !Change || !Change->IsValid())
					{
						OutError = BindingFailure(TEXT("timeline_spatial_ik_definition_change_invalid"),
							TEXT("Each definition change must be an object."), ChangePointer, Value, TEXT("object"), IkRef);
						return false;
					}
					FString Kind;
					FString Primary;
					if (!RequiredString(*Change, ChangePointer, TEXT("kind"), Kind, OutError)
						|| !RequiredString(*Change, ChangePointer, TEXT("primary_ref"), Primary, OutError))
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
					if (!RequiredString(*Preview, TEXT("/input/preview_context"), TEXT("preview_mesh_ref"), MeshRef, OutError)
						|| !AddRigObject(Config, MeshRef, Mesh, OutError, TEXT("/input/preview_context/preview_mesh_ref"))) return false;
				}
				return true;
			}

			if (CommandId.StartsWith(TEXT("rig.retarget.")))
			{
				FString RetargeterRef;
				if (!RequiredString(Input, TEXT("/input"), TEXT("retargeter_ref"), RetargeterRef, OutError))
					return false;
				FString SourceFrame;
				FString TargetFrame;
				if (CommandId == TEXT("rig.retarget.pose.update"))
				{
					FString Side;
					TSharedPtr<FJsonObject> PoseFrame;
					if (!RequiredString(Input, TEXT("/input"), TEXT("subject_side"), Side, OutError)
						|| !RequiredObject(Input, TEXT("/input"), TEXT("pose_frame"), PoseFrame, OutError))
						return false;
					FString FrameId;
					if (!AddRigFrame(Config, PoseFrame, TEXT("/input/pose_frame"), true, FrameId, OutError)) return false;
					if (Side == TEXT("source")) SourceFrame = FrameId;
					else TargetFrame = FrameId;
				}
				if (!AddRetargeterBinding(
						Config, RetargeterRef, SourceFrame, TargetFrame, OutError)) return false;
				if (CommandId == TEXT("rig.retarget.definition.update"))
				{
					FString SourceRef, TargetRef;
					if (!RequiredString(Input, TEXT("/input"), TEXT("source_rig_ref"), SourceRef, OutError)
						|| !RequiredString(Input, TEXT("/input"), TEXT("target_rig_ref"), TargetRef, OutError)
						|| !AddIkBinding(Config, SourceRef, OutError, TEXT("/input/source_rig_ref"))
						|| !AddIkBinding(Config, TargetRef, OutError, TEXT("/input/target_rig_ref"))) return false;
					RigSpatial::FUeAgentRigSimulationSpatialIkRigBinding* SourceBinding =
						FindIkBinding(Config, SourceRef);
					RigSpatial::FUeAgentRigSimulationSpatialIkRigBinding* TargetBinding =
						FindIkBinding(Config, TargetRef);
					const TArray<TSharedPtr<FJsonValue>>* Mappings = nullptr;
					if (!SourceBinding || !TargetBinding
						|| !Input->TryGetArrayField(TEXT("chain_mappings"), Mappings) || !Mappings)
					{
						OutError = BindingFailure(TEXT("timeline_spatial_retarget_definition_bindings_invalid"),
							!SourceBinding ? TEXT("The source IK rig binding is missing after initialization.")
								: !TargetBinding ? TEXT("The target IK rig binding is missing after initialization.") : TEXT("Chain mappings must be an array."),
							!SourceBinding ? TEXT("/input/source_rig_ref") : !TargetBinding ? TEXT("/input/target_rig_ref") : TEXT("/input/chain_mappings"),
							!SourceBinding ? MakeShared<FJsonValueString>(SourceRef) : !TargetBinding ? MakeShared<FJsonValueString>(TargetRef) : Input->TryGetField(TEXT("chain_mappings")),
							!SourceBinding || !TargetBinding ? TEXT("initialized IK rig binding") : TEXT("array"), RetargeterRef);
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
					if (!RequiredString(Input, TEXT("/input"), TEXT("source_subject_ref"), SourceRef, OutError)
						|| !RequiredString(Input, TEXT("/input"), TEXT("target_subject_ref"), TargetRef, OutError)
						|| !RequiredString(Input, TEXT("/input"), TEXT("destination_root"), DestinationRoot, OutError)
						|| !AddRigObject(Config, SourceRef, Source, OutError, TEXT("/input/source_subject_ref"))
						|| !AddRigObject(Config, TargetRef, TargetObject, OutError, TEXT("/input/target_subject_ref"))) return false;
					if (Target.IsEmpty())
					{
						Target = DestinationRoot;
					}
					else if (DestinationRoot != Target)
					{
						OutError = BindingFailure(TEXT("timeline_spatial_retarget_destination_target_mismatch"),
							TEXT("The destination root does not match the request target."), TEXT("/input/destination_root"),
							MakeShared<FJsonValueString>(DestinationRoot), Target, Target);
						return false;
					}
					const auto* RetargeterBinding = Config.RigRetargeterBindings.FindByPredicate(
						[&](const auto& Binding) { return Binding.Identity == RetargeterRef; });
					if (!RetargeterBinding || !RetargeterBinding->Retargeter.IsValid())
					{
						OutError = BindingFailure(TEXT("timeline_spatial_retarget_destination_binding_invalid"),
							!RetargeterBinding ? TEXT("The retargeter binding is missing after initialization.") : TEXT("The bound retargeter is no longer valid."),
							TEXT("/input/retargeter_ref"), MakeShared<FJsonValueString>(RetargeterRef), TEXT("valid retargeter binding"), DestinationRoot);
						return false;
					}
					Config.RigObjectBindings.Emplace(
						DestinationRoot, RetargeterBinding->Retargeter.Get());
					RigSpatial::FUeAgentRigSimulationSpatialExecutionError Error;
					if (!RigSpatial::TryCreateRigSimulationSpatialProductionAssetEffectPort(
							Config.RigAssetEffectPort, Error))
					{
						OutError = Error;
						return false;
					}
				}
				return true;
			}

			OutError = BindingFailure(TEXT("timeline_spatial_request_binding_command_unsupported"),
				TEXT("This command has no request binding initializer."), FString(),
				MakeShared<FJsonValueString>(CommandId), TEXT("command with a registered request binding initializer"));
			OutError.PropertyPath = TEXT("command_id");
			return false;
		}

		bool ConfigureRequestBindings(
			const FUeAgentCommandRequest& Request, UWorld& World,
			FUeAgentTimelineSpatialRuntimeConfig& Config, UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (ConfigureRequestBindingsImpl(Request, World, Config, OutError)) return true;
			if (Request.GetParams().IsValid() && !OutError.Source.JsonPath.IsEmpty())
			{
				const bool bRelated = OutError.SourceKind == TEXT("related_input");
				UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Request.GetParams(), TEXT("timeline_spatial"));
				if (bRelated) OutError.SourceKind = TEXT("related_input");
			}
			return false;
		}

		bool TryCreateMechanics(
			UWorld& World,
			const FString& SessionId,
			TMap<FString, TSharedRef<IUeAgentCommandHandler>>& OutHandlers,
			TArray<FString>& OutScopes,
			UE::UAI::Contract::FDiagnostic& OutError,
			const FUeAgentCommandRequest* BoundRequest = nullptr)
		{
			OutHandlers.Reset();
			OutScopes.Reset();
			OutError.Reset();
			if (!IsInGameThread())
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_spatial_mechanics_game_thread_required"), TEXT("Timeline spatial mechanics must be initialized on the game thread."), TEXT("thread"));
				return false;
			}
			if (!IsValid(&World) || SessionId.IsEmpty())
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_spatial_mechanics_context_invalid"),
					!IsValid(&World) ? TEXT("The editor world is no longer valid.") : TEXT("The mechanics session identifier is empty."), TEXT("context"));
				OutError.PropertyPath = !IsValid(&World) ? TEXT("world") : TEXT("session_id");
				if (IsValid(&World)) OutError.ActualEvidence = MakeShared<FJsonValueString>(SessionId);
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
				OutError = TicketError;
				return false;
			}
			TSharedPtr<const FUeAgentResponseProjector> Projector;
			FUeAgentProjectionError ProjectionError;
			if (!FUeAgentResponseProjector::TryCreate(
					MakeShared<FConservativeTokenEstimator>(), Projector, ProjectionError))
			{
				OutError = ProjectionError;
				return false;
			}

			FUeAgentSimpleAuthoringRuntimeConfig SimpleConfig;
			SimpleConfig.ProjectInstanceId = TEXT("project:") + FString(FApp::GetProjectName());
			SimpleConfig.EditorInstanceId = FString::Printf(
				TEXT("editor-process:%u"), FPlatformProcess::GetCurrentProcessId());
			SimpleConfig.ServerSessionId = SessionId;
			SimpleConfig.RegistryEpoch = 1;

			SimpleConfig.JournalRoot = FPaths::ProjectSavedDir()
				/ TEXT("UeAgentInterface/SimpleAuthoring/TimelineSpatialMechanicsFoundation");
			TSharedPtr<FUeAgentSimpleAuthoringRuntime> SimpleRuntime;
			FUeAgentSimpleAuthoringRuntimeError SimpleError;
			if (!FUeAgentSimpleAuthoringRuntime::TryCreate(
					SimpleConfig, World, Tickets.ToSharedRef(), Projector.ToSharedRef(),
					SimpleRuntime, SimpleError)
				|| !SimpleRuntime.IsValid())
			{
				OutError = SimpleError;
				if (OutError.IsEmpty()) OutError = UE::UAI::Contract::FDiagnostic::Failure(
					TEXT("timeline_spatial_simple_runtime_missing"), TEXT("The simple authoring runtime factory did not return a runtime."), TEXT("runtime_factory"));
				return false;
			}
			TSharedPtr<const FUeAgentDiscoveryService> Discovery;
			FUeAgentDiscoveryError DiscoveryError;
			if (!FUeAgentDiscoveryService::TryCreate(
					SimpleRuntime->GetCatalogSources(), {}, Discovery, DiscoveryError)
				|| !Discovery.IsValid())
			{
				OutError = DiscoveryError;
				if (OutError.IsEmpty()) OutError = UE::UAI::Contract::FDiagnostic::Failure(
					TEXT("timeline_spatial_discovery_missing"), TEXT("The discovery factory did not return a service."), TEXT("runtime_factory"));
				return false;
			}

			FUeAgentTimelineSpatialRuntimeConfig RuntimeConfig;
			RuntimeConfig.ProjectInstanceId = SimpleConfig.ProjectInstanceId;
			RuntimeConfig.EditorInstanceId = SimpleConfig.EditorInstanceId;
			RuntimeConfig.ServerSessionId = SessionId;
			RuntimeConfig.RegistryEpoch = 1;

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
				OutError = RuntimeError;
				if (OutError.IsEmpty()) OutError = UE::UAI::Contract::FDiagnostic::Failure(
					TEXT("timeline_spatial_runtime_missing"), TEXT("The timeline spatial factory did not return a runtime."), TEXT("runtime_factory"));
				return false;
			}
			for (const TSharedRef<IUeAgentCommandHandler>& Handler : Runtime->GetCommandHandlers())
			{
				if (OutHandlers.Contains(Handler->GetCommandId()))
				{
					OutHandlers.Reset();
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_spatial_mechanics_duplicate"), TEXT("More than one handler is registered for this command."), TEXT("handlers"));
					OutError.PropertyPath = TEXT("command_id");
					OutError.ActualValuePresent = true;
					OutError.ActualValue = MakeShared<FJsonValueString>(Handler->GetCommandId());
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("one handler per command"));
					return false;
				}
				OutHandlers.Add(Handler->GetCommandId(), Handler);
			}
			OutScopes = Runtime->GetAuthorityScopes();
			if (OutHandlers.Num() != Runtime->GetCommandHandlers().Num())
			{
				const int32 ActualCount = OutHandlers.Num();
				OutHandlers.Reset();
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_spatial_mechanics_count_mismatch"), TEXT("The registered handler count differs from the runtime handler count."), TEXT("handlers"));
				OutError.PropertyPath = TEXT("handlers.count");
				OutError.ActualValuePresent = true;
				OutError.ActualValue = MakeShared<FJsonValueNumber>(ActualCount);
				OutError.ExpectedValue = MakeShared<FJsonValueNumber>(Runtime->GetCommandHandlers().Num());
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
				TEXT("rig.control.bake.context"),
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
            const UE::UAI::Contract::FDiagnostic& Diagnostic)
        {
            return FUeAgentCommandResult::Failure(Request, TEXT("{}"), Diagnostic,
                TEXT("timeline_spatial_request_binding_unavailable"));
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
				UE::UAI::Contract::FDiagnostic Error;
				UWorld* const World = BoundWorld.Get();
				UWorld* const CurrentWorld = IsInGameThread() && GEditor
					? GEditor->GetEditorWorldContext().World()
					: nullptr;
				if (Request.GetCommandId() != CommandId
					|| !World
					|| CurrentWorld != World)
				{
					if (Request.GetCommandId() != CommandId)
					{
						Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_spatial_request_bound_command_mismatch"), TEXT("The request command does not match this handler."), TEXT("request_binding"));
						Error.PropertyPath = TEXT("command_id");
						Error.ActualValuePresent = true;
						Error.ActualValue = MakeShared<FJsonValueString>(Request.GetCommandId());
						Error.ExpectedValue = MakeShared<FJsonValueString>(CommandId);
					}
					else
					{
						Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_spatial_request_bound_world_changed"),
							!World ? TEXT("The world bound to this request is no longer valid.")
								: !IsInGameThread() ? TEXT("The request-bound handler must run on the game thread.")
								: !CurrentWorld ? TEXT("No current editor world is available.")
								: TEXT("The current editor world differs from the world bound to this request."), TEXT("request_binding"));
						Error.PropertyPath = TEXT("world");
						if (IsInGameThread() && World && CurrentWorld)
						{
							Error.ActualValuePresent = true;
							Error.ActualValue = MakeShared<FJsonValueString>(CurrentWorld->GetPathName());
							Error.ExpectedValue = MakeShared<FJsonValueString>(World->GetPathName());
						}
					}
					return RequestBindingFailure(Request, Error);
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
				if (Found) return (*Found)->Submit(Request, Caller, Cancellation);
				Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_spatial_request_bound_handler_missing"), TEXT("No handler was returned for the request-bound command."), TEXT("request_binding"));
				Error.PropertyPath = TEXT("command_id");
				Error.ActualValuePresent = true;
				Error.ActualValue = MakeShared<FJsonValueString>(CommandId);
				return RequestBindingFailure(Request, Error);
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandler.Reset();
		OutAuthorityScopes.Reset();
		OutError.Reset();
		if (!IsInGameThread())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_spatial_mechanics_game_thread_required"), TEXT("Timeline spatial mechanics must be resolved on the game thread."), TEXT("thread"));
			return false;
		}
		UWorld* const World = GEditor
			? GEditor->GetEditorWorldContext().World()
			: nullptr;
		if (!World)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_spatial_mechanics_editor_world_unavailable"), TEXT("No editor world is available for timeline spatial mechanics."), TEXT("editor_world"));
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
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_spatial_mechanics_missing"), TEXT("No timeline spatial handler is registered for this command."), TEXT("handler_lookup"));
			OutError.PropertyPath = TEXT("command_id");
			OutError.ActualValuePresent = true;
			OutError.ActualValue = MakeShared<FJsonValueString>(CommandId);
			return false;
		}
		OutHandler = *Found;
		OutAuthorityScopes = Host.AuthorityScopes;
		return true;
	}
}
