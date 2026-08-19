#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialNative.h"

#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Compilation/MovieSceneCompiledDataManager.h"
#include "ControlRig.h"
#include "ControlRigObjectBinding.h"
#include "ControlRigSequencerEditorLibrary.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "FileHelpers.h"
#include "Exporters/AnimSeqExportOption.h"
#include "HAL/PlatformTime.h"
#include "LevelSequence.h"
#include "Misc/Crc.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneBindingProxy.h"
#include "Rigs/FKControlRig.h"
#include "Sequencer/MovieSceneControlRigParameterSection.h"
#include "SequencerTools.h"
#include "Sequencer/MovieSceneControlRigParameterTrack.h"
#include "Tracks/MovieSceneSkeletalAnimationTrack.h"
#include "Units/Execution/RigUnit_InverseExecution.h"

namespace UE::UAI::DomainAdapters::RigSimulationSpatial
{
	using namespace BindingGateway::Operation;
	using namespace ContextResource;
	using namespace Discovery;
	using namespace StateSemantic;
	using namespace Verification;

	namespace
	{
		bool Fail(
			FUeAgentRigSimulationSpatialExecutionError& OutError,
			FString Code,
			FString Subject = FString(),
			FString Detail = FString())
		{
			OutError = {};
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(Subject);
			OutError.Detail = MoveTemp(Detail);
			return false;
		}

		bool TrySaveProjectAsset(
			UObject& Object,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			UPackage* Package = Object.GetOutermost();
			if (!Package || !Package->GetName().StartsWith(TEXT("/Game/"))
				|| !Package->IsDirty())
			{
				return true;
			}
			if (!UEditorLoadingAndSavingUtils::SavePackages({Package}, false))
			{
				return Fail(OutError,
					TEXT("rig_simulation_spatial_persistence_failed"),
					Object.GetPathName(), Package->GetName());
			}
			return true;
		}

		bool IsImplementedCapability(const FString& CapabilityId)
		{
			return CapabilityId == TEXT("rig.skeleton.preview_mesh.bind")
				|| CapabilityId == TEXT("rig.skeleton.socket.update")
				|| CapabilityId == TEXT("rig.control.bake.animation")
				|| CapabilityId == TEXT("rig.control.bake.controls");
		}

		const FUeAgentRigSimulationSpatialRequest* ProductionRequest(
			const FUeAgentRigSimulationSpatialTypedRequestRef& Request,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			if (Request->GetTypeId().IsEmpty() || Request->GetFamilyId().IsEmpty()
				|| Request->GetRequestKindId().IsEmpty())
			{
				Fail(OutError,
					TEXT("rig_simulation_spatial_native_typed_request_invalid"),
					Request->GetTypeId());
				return nullptr;
			}
			return static_cast<const FUeAgentRigSimulationSpatialRequest*>(&Request.Get());
		}

		TOptional<FUeAgentCandidateBinding> FirstTarget(
			const TArray<FUeAgentOperationTargetBinding>& Targets)
		{
			return Targets.IsEmpty() ? TOptional<FUeAgentCandidateBinding>()
				: TOptional<FUeAgentCandidateBinding>(Targets[0].GetBinding());
		}

		bool TryRate(
			const FRigSimulationSpatialRate& Rate,
			FFrameRate& OutRate,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			int32 Numerator = 0;
			int32 Denominator = 0;
			if (!Rate.IsValid()
				|| !LexTryParseString(Numerator, *Rate.Numerator)
				|| !LexTryParseString(Denominator, *Rate.Denominator)
				|| Numerator <= 0 || Denominator <= 0)
			{
				return Fail(OutError,
					TEXT("rig_simulation_spatial_native_rate_invalid"),
					Rate.Numerator + TEXT("/") + Rate.Denominator);
			}
			OutRate = FFrameRate(Numerator, Denominator);
			return true;
		}

		bool TryRangeFrames(
			const FUeAgentTemporalRange& Range,
			int32& OutLower,
			int32& OutUpper,
			FFrameRate& OutDisplayRate,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			const TOptional<FUeAgentTemporalPoint>& Lower =
				Range.GetLowerBound().GetPoint();
			const TOptional<FUeAgentTemporalPoint>& Upper =
				Range.GetUpperBound().GetPoint();
			int64 LowerValue = 0;
			int64 UpperValue = 0;
			int32 Numerator = 0;
			int32 Denominator = 0;
			if (!Range.IsValid() || !Lower.IsSet() || !Upper.IsSet()
				|| !LexTryParseString(LowerValue, *Lower->GetTick())
				|| !LexTryParseString(UpperValue, *Upper->GetTick())
				|| !LexTryParseString(Numerator, *Lower->GetRateNumerator())
				|| !LexTryParseString(Denominator, *Lower->GetRateDenominator())
				|| LowerValue < MIN_int32 || LowerValue > MAX_int32
				|| UpperValue < MIN_int32 || UpperValue > MAX_int32
				|| UpperValue < LowerValue || Numerator <= 0 || Denominator <= 0
				|| Upper->GetRateNumerator() != Lower->GetRateNumerator()
				|| Upper->GetRateDenominator() != Lower->GetRateDenominator())
			{
				return Fail(OutError,
					TEXT("rig_simulation_spatial_native_authored_range_invalid"),
					Range.GetClockId());
			}
			OutLower = static_cast<int32>(LowerValue);
			OutUpper = static_cast<int32>(UpperValue);
			OutDisplayRate = FFrameRate(Numerator, Denominator);
			return true;
		}

		bool TryNativeTransform(
			const FUeAgentSpatialValue& Value,
			FTransform& OutTransform,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			const TArray<FString>& Components = Value.GetComponents();
			if (!Value.IsValid()
				|| Value.GetQuantityKind() != EUeAgentSpatialQuantityKind::Transform
				|| Components.Num() != 16)
			{
				return Fail(OutError,
					TEXT("rig_simulation_spatial_native_transform_invalid"),
					Value.GetSubject());
			}
			FMatrix Matrix = FMatrix::Identity;
			for (int32 Row = 0; Row < 4; ++Row)
			{
				for (int32 Column = 0; Column < 4; ++Column)
				{
					double Component = 0.0;
					if (!LexTryParseString(Component,
							*Components[Row * 4 + Column])
						|| !FMath::IsFinite(Component))
					{
						return Fail(OutError,
							TEXT("rig_simulation_spatial_native_transform_numeric_invalid"),
							Value.GetSubject());
					}
					// C4 is a column-vector homogeneous matrix.  UE's FMatrix is
					// row-vector based, so the explicit representation is transposed.
					Matrix.M[Column][Row] = Component;
				}
			}
			OutTransform = FTransform(Matrix);
			OutTransform.NormalizeRotation();
			return OutTransform.IsValid();
		}

		FString TransformText(const FTransform& Transform)
		{
			const FVector Translation = Transform.GetTranslation();
			const FQuat Rotation = Transform.GetRotation();
			const FVector Scale = Transform.GetScale3D();
			return FString::Printf(
				TEXT("t=%.9g,%.9g,%.9g|q=%.9g,%.9g,%.9g,%.9g|s=%.9g,%.9g,%.9g"),
				Translation.X, Translation.Y, Translation.Z,
				Rotation.X, Rotation.Y, Rotation.Z, Rotation.W,
				Scale.X, Scale.Y, Scale.Z);
		}

		int32 CountControlKeys(const ULevelSequence& Sequence)
		{
			int32 Result = 0;
			const UMovieScene* Scene = Sequence.GetMovieScene();
			if (!Scene) return Result;
			for (const FMovieSceneBinding& Binding : Scene->GetBindings())
			{
				for (const UMovieSceneTrack* Track : Binding.GetTracks())
				{
					const UMovieSceneControlRigParameterTrack* RigTrack =
						Cast<UMovieSceneControlRigParameterTrack>(Track);
					if (!RigTrack) continue;
					for (const UMovieSceneSection* RawSection : RigTrack->GetAllSections())
					{
						const UMovieSceneControlRigParameterSection* Section =
							Cast<UMovieSceneControlRigParameterSection>(RawSection);
						if (!Section) continue;
						for (const FTransformParameterNameAndCurves& Curves
							: Section->GetTransformParameterNamesAndCurves())
						{
							for (int32 Axis = 0; Axis < 3; ++Axis)
							{
								Result += Curves.Translation[Axis]
									.GetData().GetTimes().Num();
								Result += Curves.Rotation[Axis]
									.GetData().GetTimes().Num();
								Result += Curves.Scale[Axis]
									.GetData().GetTimes().Num();
							}
						}
					}
				}
			}
			return Result;
		}

		bool TryStringValue(
			const FString& Text,
			const FUeAgentOperationContext& Context,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			FUeAgentStateSemanticError Error;
			return FUeAgentCanonicalValue::TryString(
				Text, Context.GetResourceTracker(), OutValue, Error)
				? true
				: Fail(OutError,
					TEXT("rig_simulation_spatial_native_string_failed"),
					Error.Path, Error.Code);
		}

		bool TrySnapshot(
			const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
			EUeAgentRigSimulationSpatialReadPhase Phase,
			const FString& PublicRef,
			const FString& State,
			const FUeAgentOperationContext& Context,
			TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			FUeAgentCanonicalValuePtr RefValue;
			FUeAgentCanonicalValuePtr CapabilityValue;
			FUeAgentCanonicalValuePtr StateValue;
			FUeAgentCanonicalValuePtr ObjectValue;
			FUeAgentStateSemanticError Error;
			if (!TryStringValue(PublicRef, Context, RefValue, OutError)
				|| !TryStringValue(Contract.GetCapabilityId(), Context,
					CapabilityValue, OutError)
				|| !TryStringValue(State, Context, StateValue, OutError)
				|| !FUeAgentCanonicalValue::TryObject({
						{TEXT("public_ref"), RefValue.ToSharedRef()},
						{TEXT("capability_id"), CapabilityValue.ToSharedRef()},
						{TEXT("observed_state"), StateValue.ToSharedRef()}},
					Context.GetResourceTracker(), ObjectValue, Error))
			{
				return OutError.IsValid() ? false : Fail(OutError,
					TEXT("rig_simulation_spatial_native_object_failed"),
					Error.Path, Error.Code);
			}
			const uint64 Upper = FCrc::StrCrc32(*State);
			const uint64 Lower = FCrc::StrCrc32(*(Contract.GetReadbackId() + State));
			const FUeAgentStateRevision Revision(Contract.GetReadbackId(),
				TEXT("native-observation-v1"),
				static_cast<int64>(((Upper << 32) | Lower) & MAX_int64));
			const FUeAgentStateEntry Entry(
				FUeAgentCanonicalIdentity(
					TEXT("rig-simulation-spatial:") + Contract.GetCapabilityId(),
					TEXT("rig_simulation_spatial_target"), PublicRef, PublicRef,
					TEXT("result"), TEXT("native-observation-v1")),
				FUeAgentCanonicalType(Contract.GetTypedResultId(),
					EUeAgentValueKind::Object, false, TEXT("object")),
				ObjectValue.ToSharedRef(),
				FUeAgentDefaultValue(EUeAgentDefaultSourceKind::DomainNative,
					Contract.GetReadbackId(), TEXT("independent-native-readback"),
					ObjectValue.ToSharedRef()),
				FUeAgentStateDetails(TEXT("result"),
					TEXT("independent RigSimulationSpatial native observation"),
					{TEXT("RigSimulationSpatial"), Contract.GetFamilyId()}, false,
					TEXT("available"), {}), Revision);
			if (!FUeAgentCanonicalSnapshot::TryCreate(
					FString::Printf(TEXT("rig-simulation-spatial:%s:%d:%s"),
						*Contract.GetCapabilityId(), static_cast<int32>(Phase),
						*Context.GetRequestId()),
					Context.GetExecutionContext().GetStatePlane(), Revision,
					FUeAgentTemporalPoint(TEXT("editor-native"),
						FString::Printf(TEXT("%llu"), FPlatformTime::Cycles64())),
					FUeAgentStateProvenance(TEXT("independent_readback"),
						Contract.GetReadbackId(),
						TEXT("native-state:") + Contract.GetCapabilityId()),
					{Entry}, Context.GetResourceTracker(), OutSnapshot, Error))
			{
				return Fail(OutError,
					TEXT("rig_simulation_spatial_native_snapshot_failed"),
					Error.Path, Error.Code);
			}
			return true;
		}

		FString SnapshotState(const FUeAgentCanonicalSnapshot& Snapshot)
		{
			if (Snapshot.GetEntries().IsEmpty()) return FString();
			const FUeAgentCanonicalValueRef& Root =
				Snapshot.GetEntries()[0].GetValue();
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Pair
				: Root->GetObjectItems())
			{
				if (Pair.Key == TEXT("observed_state"))
				{
					return Pair.Value->GetLexeme();
				}
			}
			return FString();
		}

		bool CheckRevision(
			IUeAgentRigSimulationSpatialNativeEnvironment& Environment,
			const FString& ExplicitRef,
			const FRigSimulationSpatialRevision& Expected,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			FRigSimulationSpatialRevision Actual;
			if (!Environment.TryReadRevision(ExplicitRef, Actual, OutError)) return false;
			return Actual == Expected ? true : Fail(OutError,
				TEXT("rig_simulation_spatial_revision_mismatch"), ExplicitRef);
		}

		FString ReadState(
			const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
			const FUeAgentRigSimulationSpatialRequest& Request,
			IUeAgentRigSimulationSpatialNativeEnvironment& Environment,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			if (Contract.GetCapabilityId()
				== TEXT("rig.skeleton.preview_mesh.bind"))
			{
				const auto& Input =
					Request.GetPayload().Get<FRigSkeletonPreviewMeshBindInput>();
				const auto* Binding = Environment.FindSkeletonBinding(Input.SkeletonRef);
				USkeleton* Skeleton = Binding ? Binding->Skeleton.Get() : nullptr;
				if (!Skeleton) return Fail(OutError,
					TEXT("rig_simulation_spatial_skeleton_binding_unavailable"),
					Input.SkeletonRef), FString();
				USkeletalMesh* Preview = Skeleton->GetPreviewMesh(true);
				const bool bMatches = Preview
					&& Preview == Environment.ResolveExplicitObject(Input.PreviewMeshRef);
				return FString::Printf(TEXT("skeleton=%s|preview_ref=%s|preview=%s|matches=%d"),
					*Input.SkeletonRef, *Input.PreviewMeshRef,
					Preview ? *Preview->GetPathName() : TEXT("none"), bMatches ? 1 : 0);
			}
			if (Contract.GetCapabilityId() == TEXT("rig.skeleton.socket.update"))
			{
				const auto& Input =
					Request.GetPayload().Get<FRigSkeletonSocketUpdateInput>();
				const auto* SkeletonBinding =
					Environment.FindSkeletonBinding(Input.SkeletonRef);
				const auto* SocketBinding = Environment.FindSocketBinding(
					Input.SkeletonRef, Input.SocketRef);
				USkeleton* Skeleton = SkeletonBinding
					? SkeletonBinding->Skeleton.Get() : nullptr;
				if (!Skeleton || !SocketBinding) return Fail(OutError,
					TEXT("rig_simulation_spatial_socket_binding_unavailable"),
					Input.SocketRef), FString();
				const USkeletalMeshSocket* Socket =
					Skeleton->FindSocket(SocketBinding->SocketName);
				const bool bParentMatches = Socket
					&& Socket->BoneName == SocketBinding->ParentBoneName;
				return FString::Printf(
					TEXT("skeleton=%s|socket_ref=%s|present=%d|parent_ref=%s|parent_matches=%d|%s"),
					*Input.SkeletonRef, *Input.SocketRef, Socket ? 1 : 0,
					*Input.ParentBoneRef, bParentMatches ? 1 : 0,
					Socket ? *TransformText(FTransform(Socket->RelativeRotation,
						Socket->RelativeLocation, Socket->RelativeScale)) : TEXT("none"));
			}
			if (Contract.GetCapabilityId() == TEXT("rig.control.bake.animation"))
			{
				const auto& Input =
					Request.GetPayload().Get<FRigControlBakeAnimationInput>();
				const UAnimSequence* Animation = Cast<UAnimSequence>(
					Environment.ResolveExplicitObject(Input.TargetAnimationRef));
				const auto* Binding = Environment.FindRigBakeBinding(Input.BindingRef);
				const IAnimationDataModel* Model = Animation
					? Animation->GetDataModel() : nullptr;
				TArray<FName> Names;
				if (Model) Model->GetBoneTrackNames(Names);
				return FString::Printf(
					TEXT("animation=%s|binding=%s|clock=%s|bone_tracks=%d|keys=%d"),
					*Input.TargetAnimationRef, *Input.BindingRef,
					Binding ? *Binding->NativeAuthoredClockId : TEXT("none"), Names.Num(),
					Model ? Model->GetNumberOfKeys() : 0);
			}
			if (Contract.GetCapabilityId() == TEXT("rig.control.bake.controls"))
			{
				const auto& Input =
					Request.GetPayload().Get<FRigControlBakeControlsInput>();
				const auto* Binding = Environment.FindRigBakeBinding(Input.BindingRef);
				ULevelSequence* Sequence = Binding ? Binding->Sequence.Get() : nullptr;
				return FString::Printf(
					TEXT("sequence=%s|binding=%s|clock=%s|control_frame=%s|control_keys=%d"),
					*Input.TargetSequenceRef, *Input.BindingRef,
					Binding ? *Binding->NativeAuthoredClockId : TEXT("none"),
					Binding ? *Binding->NativeControlFrameId : TEXT("none"),
					Sequence ? CountControlKeys(*Sequence) : 0);
			}
			Fail(OutError, TEXT("rig_simulation_spatial_family_port_unavailable"),
				Contract.GetCapabilityId());
			return FString();
		}

		class FReadback final : public IUeAgentRigSimulationSpatialReadbackPort
		{
		public:
			explicit FReadback(
				TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> InEnvironment)
				: Environment(MoveTemp(InEnvironment)) {}

			bool TryRead(
				const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
				const FUeAgentRigSimulationSpatialTypedRequestRef& Request,
				const TOptional<FUeAgentCandidateBinding>& Target,
				EUeAgentRigSimulationSpatialReadPhase Phase,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				Environment->CountFamilyDispatch();
				if (!IsImplementedCapability(Contract.GetCapabilityId()))
				{
					return Fail(OutError,
						TEXT("rig_simulation_spatial_family_port_unavailable"),
						Contract.GetCapabilityId());
				}
				const FUeAgentRigSimulationSpatialRequest* Typed =
					ProductionRequest(Request, OutError);
				if (!Typed) return false;
				const FString PublicRef = Typed->GetPrimaryTargetRef();
				if (PublicRef.IsEmpty()
					|| (Target.IsSet() && Target->GetPublicRef() != PublicRef))
				{
					return Fail(OutError,
						TEXT("rig_simulation_spatial_explicit_target_mismatch"),
						PublicRef);
				}
				const FString State = ReadState(Contract, *Typed, *Environment, OutError);
				return !State.IsEmpty() && TrySnapshot(Contract, Phase, PublicRef,
					State, Context, OutSnapshot, OutError);
			}

		private:
			TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> Environment;
		};

		class FMutation final : public IUeAgentRigSimulationSpatialMutationPort
		{
		public:
			explicit FMutation(
				TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> InEnvironment)
				: Environment(MoveTemp(InEnvironment)) {}

			bool TryMutate(
				const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
				const FUeAgentRigSimulationSpatialTypedRequestRef& Request,
				const TArray<FUeAgentOperationTargetBinding>& Targets,
				const FUeAgentOperationContext& Context,
				FUeAgentRigSimulationSpatialMutationResult& OutResult,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				OutResult = {};
				OutError = {};
				Environment->CountFamilyDispatch();
				if (!IsImplementedCapability(Contract.GetCapabilityId()))
				{
					return Fail(OutError,
						TEXT("rig_simulation_spatial_family_port_unavailable"),
						Contract.GetCapabilityId());
				}
				const FUeAgentRigSimulationSpatialRequest* Typed =
					ProductionRequest(Request, OutError);
				if (!Typed) return false;
				const TOptional<FUeAgentCandidateBinding> Target = FirstTarget(Targets);
				if (!Target.IsSet()
					|| Target->GetPublicRef() != Typed->GetPrimaryTargetRef())
				{
					return Fail(OutError,
						TEXT("rig_simulation_spatial_explicit_target_mismatch"),
						Typed->GetPrimaryTargetRef());
				}

				UObject* MutatedObject = nullptr;
				if (Contract.GetCapabilityId()
					== TEXT("rig.skeleton.preview_mesh.bind"))
				{
					const auto& Input = Typed->GetPayload()
						.Get<FRigSkeletonPreviewMeshBindInput>();
					const auto* Binding =
						Environment->FindSkeletonBinding(Input.SkeletonRef);
					USkeleton* Skeleton = Binding ? Binding->Skeleton.Get() : nullptr;
					USkeletalMesh* PreviewMesh = Cast<USkeletalMesh>(
						Environment->ResolveExplicitObject(Input.PreviewMeshRef));
					if (!Skeleton || !PreviewMesh
						|| Environment->ResolveExplicitObject(Input.SkeletonRef) != Skeleton)
					{
						return Fail(OutError,
							TEXT("rig_simulation_spatial_skeleton_preview_binding_unavailable"),
							Input.SkeletonRef);
					}
					if (!CheckRevision(*Environment, Input.SkeletonRef,
							Input.ExpectedRevision, OutError)) return false;
					Skeleton->Modify();
					Skeleton->SetPreviewMesh(PreviewMesh, true);
					Skeleton->MarkPackageDirty();
					MutatedObject = Skeleton;
				}
				else if (Contract.GetCapabilityId()
					== TEXT("rig.skeleton.socket.update"))
				{
					const auto& Input = Typed->GetPayload()
						.Get<FRigSkeletonSocketUpdateInput>();
					const auto* SkeletonBinding =
						Environment->FindSkeletonBinding(Input.SkeletonRef);
					const auto* SocketBinding = Environment->FindSocketBinding(
						Input.SkeletonRef, Input.SocketRef);
					USkeleton* Skeleton = SkeletonBinding
						? SkeletonBinding->Skeleton.Get() : nullptr;
					if (!Skeleton || !SocketBinding
						|| Input.ParentBoneRef != SocketBinding->ParentBoneIdentity
						|| Skeleton->GetReferenceSkeleton().FindBoneIndex(
							SocketBinding->ParentBoneName) == INDEX_NONE)
					{
						return Fail(OutError,
							TEXT("rig_simulation_spatial_socket_binding_unavailable"),
							Input.SocketRef);
					}
					if (!CheckRevision(*Environment, Input.SkeletonRef,
							Input.ExpectedRevision, OutError)) return false;
					USkeletalMeshSocket* Socket =
						Skeleton->FindSocket(SocketBinding->SocketName);
					Skeleton->Modify();
					if (Input.Operation == TEXT("remove"))
					{
						if (!Socket) return Fail(OutError,
							TEXT("rig_simulation_spatial_socket_not_found"),
							Input.SocketRef);
						Skeleton->Sockets.Remove(Socket);
					}
					else
					{
						if ((Input.Operation == TEXT("create") && Socket)
							|| (Input.Operation == TEXT("update") && !Socket))
						{
							return Fail(OutError,
								Input.Operation == TEXT("create")
									? TEXT("rig_simulation_spatial_socket_already_exists")
									: TEXT("rig_simulation_spatial_socket_not_found"),
								Input.SocketRef);
						}
						if (!Socket)
						{
							Socket = NewObject<USkeletalMeshSocket>(
								Skeleton, NAME_None, RF_Transactional);
							if (!Socket) return Fail(OutError,
								TEXT("rig_simulation_spatial_socket_create_failed"),
								Input.SocketRef);
							Skeleton->Sockets.Add(Socket);
						}
						const TArray<FUeAgentSpatialValuePtr>& Values =
							Typed->GetFormalPortRequest().GetSpatialValues();
						FUeAgentSpatialValuePtr Converted;
						FTransform NativeTransform;
						if (Values.Num() != 1 || !Values[0].IsValid()
							|| Values[0]->GetSubject() != Input.SocketRef
							|| !Environment->TryConvertSpatialValueToFrame(
								*Values[0], SocketBinding->NativeParentFrameId,
								Context.GetResourceTracker(), Converted, OutError)
							|| !Converted.IsValid()
							|| !TryNativeTransform(*Converted, NativeTransform, OutError))
						{
							return OutError.IsValid() ? false : Fail(OutError,
								TEXT("rig_simulation_spatial_socket_transform_invalid"),
								Input.SocketRef);
						}
						Socket->SocketName = SocketBinding->SocketName;
						Socket->BoneName = SocketBinding->ParentBoneName;
						Socket->RelativeLocation = NativeTransform.GetTranslation();
						Socket->RelativeRotation = NativeTransform.Rotator();
						Socket->RelativeScale = NativeTransform.GetScale3D();
					}
					Skeleton->MarkPackageDirty();
					MutatedObject = Skeleton;
				}
				else if (Contract.GetCapabilityId()
					== TEXT("rig.control.bake.animation"))
				{
					const auto& Input = Typed->GetPayload()
						.Get<FRigControlBakeAnimationInput>();
					const auto* Binding =
						Environment->FindRigBakeBinding(Input.BindingRef);
					ULevelSequence* Sequence = Cast<ULevelSequence>(
						Environment->ResolveExplicitObject(Input.SourceSequenceRef));
					UAnimSequence* Animation = Cast<UAnimSequence>(
						Environment->ResolveExplicitObject(Input.TargetAnimationRef));
					UClass* RigClass = Cast<UClass>(
						Environment->ResolveExplicitObject(Input.RigRef));
					if (!Binding || !Sequence || Binding->Sequence.Get() != Sequence
						|| !Binding->World.IsValid() || !Animation || !RigClass
						|| !RigClass->IsChildOf(UControlRig::StaticClass()))
					{
						return Fail(OutError,
							TEXT("rig_simulation_spatial_rig_bake_binding_unavailable"),
							Input.BindingRef);
					}
					if (!CheckRevision(*Environment, Input.TargetAnimationRef,
							Input.ExpectedRevision, OutError)) return false;
					TOptional<FUeAgentTemporalRange> NativeRange;
					FFrameRate SampleRate;
					int32 Lower = 0;
					int32 Upper = 0;
					FFrameRate DisplayRate;
					if (Typed->GetFormalPortRequest().GetTemporalRanges().Num() != 1
						|| !Environment->TryConvertTemporalRangeToClock(
							Typed->GetFormalPortRequest().GetTemporalRanges()[0],
							Binding->NativeAuthoredClockId,
							Context.GetResourceTracker(), NativeRange, OutError)
						|| !NativeRange.IsSet()
						|| !TryRangeFrames(*NativeRange, Lower, Upper,
							DisplayRate, OutError)
						|| !TryRate(Input.SampleRate, SampleRate, OutError)) return false;
					UAnimSeqExportOption* Options = NewObject<UAnimSeqExportOption>(
						GetTransientPackage(), NAME_None, RF_Transient);
					Options->bUseCustomTimeRange = true;
					Options->CustomStartFrame = Lower;
					Options->CustomEndFrame = Upper;
					Options->CustomDisplayRate = DisplayRate;
					Options->bUseCustomFrameRate = true;
					Options->CustomFrameRate = SampleRate;
					Animation->Modify();
					if (!USequencerToolsFunctionLibrary::ExportAnimSequence(
							Binding->World.Get(), Sequence, Animation, Options,
							FMovieSceneBindingProxy(Binding->BindingGuid, Sequence), false))
					{
						return Fail(OutError,
							TEXT("rig_simulation_spatial_bake_animation_failed"),
							Input.TargetAnimationRef);
					}
					const IAnimationDataModel* Model = Animation->GetDataModel();
					if (!Model || Model->GetNumBoneTracks() <= 0
						|| Model->GetNumberOfKeys() <= 0)
					{
						return Fail(OutError,
							TEXT("rig_simulation_spatial_bake_animation_empty"),
							Input.TargetAnimationRef);
					}
					Animation->MarkPackageDirty();
					MutatedObject = Animation;
				}
				else if (Contract.GetCapabilityId()
					== TEXT("rig.control.bake.controls"))
				{
					const auto& Input = Typed->GetPayload()
						.Get<FRigControlBakeControlsInput>();
					const auto* Binding =
						Environment->FindRigBakeBinding(Input.BindingRef);
					ULevelSequence* Sequence = Cast<ULevelSequence>(
						Environment->ResolveExplicitObject(Input.TargetSequenceRef));
					UClass* RigClass = Cast<UClass>(
						Environment->ResolveExplicitObject(Input.RigRef));
					const UControlRig* DefaultRig = RigClass
						? RigClass->GetDefaultObject<UControlRig>() : nullptr;
					const bool bSupported = RigClass
						&& (RigClass->IsChildOf(UFKControlRig::StaticClass())
							|| (DefaultRig && (DefaultRig->SupportsEvent(
								FRigUnit_InverseExecution::EventName)
								|| DefaultRig->SupportsEvent(TEXT("Inverse")))));
					USkeletalMeshComponent* SkeletalMeshComponent = Binding
						? Binding->SkeletalMeshComponent.Get() : nullptr;
					if (!Binding || !Sequence || Binding->Sequence.Get() != Sequence
						|| !Binding->World.IsValid() || !SkeletalMeshComponent
						|| !SkeletalMeshComponent->GetSkeletalMeshAsset() || !bSupported)
					{
						return Fail(OutError,
							TEXT("rig_simulation_spatial_rig_bake_binding_unavailable"),
							Input.BindingRef);
					}
					if (!CheckRevision(*Environment, Input.TargetSequenceRef,
							Input.ExpectedRevision, OutError)) return false;
					TOptional<FUeAgentTemporalRange> NativeRange;
					FFrameRate SampleRate;
					int32 Lower = 0;
					int32 Upper = 0;
					FFrameRate DisplayRate;
					if (Typed->GetFormalPortRequest().GetTemporalRanges().Num() != 1
						|| !Environment->TryConvertTemporalRangeToClock(
							Typed->GetFormalPortRequest().GetTemporalRanges()[0],
							Binding->NativeAuthoredClockId,
							Context.GetResourceTracker(), NativeRange, OutError)
						|| !NativeRange.IsSet()
						|| !TryRangeFrames(*NativeRange, Lower, Upper,
							DisplayRate, OutError)
						|| !TryRate(Input.SampleRate, SampleRate, OutError)) return false;
					UAnimSeqExportOption* Options = NewObject<UAnimSeqExportOption>(
						GetTransientPackage(), NAME_None, RF_Transient);
					Options->bUseCustomTimeRange = true;
					Options->CustomStartFrame = Lower;
					Options->CustomEndFrame = Upper;
					Options->CustomDisplayRate = DisplayRate;
					Options->bUseCustomFrameRate = true;
					Options->CustomFrameRate = SampleRate;
					UAnimSequence* TemporaryAnimation = NewObject<UAnimSequence>(
						GetTransientPackage(), NAME_None, RF_Transient);
					TemporaryAnimation->SetSkeleton(
						SkeletalMeshComponent->GetSkeletalMeshAsset()->GetSkeleton());
					if (!USequencerToolsFunctionLibrary::ExportAnimSequence(
							Binding->World.Get(), Sequence, TemporaryAnimation, Options,
							FMovieSceneBindingProxy(Binding->BindingGuid, Sequence), false))
					{
						return Fail(OutError,
							TEXT("rig_simulation_spatial_bake_controls_export_failed"),
							Input.TargetSequenceRef);
					}
					UMovieScene* MovieScene = Sequence->GetMovieScene();
					UMovieSceneControlRigParameterTrack* Track = MovieScene
						? MovieScene->FindTrack<UMovieSceneControlRigParameterTrack>(
							Binding->BindingGuid) : nullptr;
					if (!Track && MovieScene)
					{
						Track = MovieScene->AddTrack<UMovieSceneControlRigParameterTrack>(
							Binding->BindingGuid);
					}
					if (!MovieScene || !Track) return Fail(OutError,
						TEXT("rig_simulation_spatial_control_track_create_failed"),
						Input.TargetSequenceRef);
					Sequence->Modify();
					MovieScene->Modify();
					Track->Modify();
					Track->RemoveAllAnimationData();
					FString RigName = RigClass->GetName();
					RigName.RemoveFromEnd(TEXT("_C"));
					UControlRig* ControlRig = NewObject<UControlRig>(
						Track, RigClass, FName(*RigName), RF_Transactional);
					if (!ControlRig) return Fail(OutError,
						TEXT("rig_simulation_spatial_control_rig_create_failed"), Input.RigRef);
					ControlRig->SetObjectBinding(MakeShared<FControlRigObjectBinding>());
					ControlRig->GetObjectBinding()->BindToObject(SkeletalMeshComponent);
					ControlRig->GetDataSourceRegistry()->RegisterDataSource(
						UControlRig::OwnerComponent,
						ControlRig->GetObjectBinding()->GetBoundObject());
					ControlRig->Initialize();
					ControlRig->RequestInit();
					ControlRig->SetBoneInitialTransformsFromSkeletalMeshComponent(
						SkeletalMeshComponent, true);
					ControlRig->Evaluate_AnyThread();
					UMovieSceneSection* RawSection = Track->CreateControlRigSection(
						0, ControlRig, true);
					if (!RawSection
						|| !UControlRigSequencerEditorLibrary::
							LoadAnimSequenceIntoControlRigSectionWithRange(
								RawSection, TemporaryAnimation, SkeletalMeshComponent,
								FFrameNumber(Lower), false, FFrameNumber(0),
								FFrameNumber(0), EMovieSceneTimeUnit::DisplayRate, false,
								0.001f, EMovieSceneKeyInterpolation::SmartAuto, true, false))
					{
						return Fail(OutError,
							TEXT("rig_simulation_spatial_bake_controls_load_failed"),
							Input.TargetSequenceRef);
					}
					if (const FMovieSceneBinding* MovieBinding =
						MovieScene->FindBinding(Binding->BindingGuid))
					{
						for (UMovieSceneTrack* ExistingTrack : MovieBinding->GetTracks())
						{
							if (UMovieSceneSkeletalAnimationTrack* SkeletalTrack =
								Cast<UMovieSceneSkeletalAnimationTrack>(ExistingTrack))
							{
								for (UMovieSceneSection* Section : SkeletalTrack->GetAllSections())
								{
									if (Section) Section->SetIsActive(false);
								}
							}
						}
					}
					if (CountControlKeys(*Sequence) <= 0)
					{
						return Fail(OutError,
							TEXT("rig_simulation_spatial_bake_controls_empty"),
							Input.TargetSequenceRef);
					}
					Sequence->MarkPackageDirty();
					MutatedObject = Sequence;
				}

				if (!IsValid(MutatedObject)) return Fail(OutError,
					TEXT("rig_simulation_spatial_native_mutation_target_invalid"),
					Contract.GetCapabilityId());
				const FString OperationId = Context.GetRequestId() + TEXT(":")
					+ Contract.GetCapabilityId();
				Environment->RecordMutation(OperationId, MutatedObject);
				OutResult = FUeAgentRigSimulationSpatialMutationResult(OperationId,
					TEXT("native-mutation:") + MutatedObject->GetPathName());
				return OutResult.IsValid();
			}

		private:
			TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> Environment;
		};

		class FSettle final : public IUeAgentRigSimulationSpatialSettlePort
		{
		public:
			explicit FSettle(
				TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> InEnvironment)
				: Environment(MoveTemp(InEnvironment)) {}

			bool TrySettle(
				const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
				const FUeAgentRigSimulationSpatialMutationResult& Mutation,
				const FUeAgentOperationContext& Context,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				OutError = {};
				if (!IsImplementedCapability(Contract.GetCapabilityId()))
				{
					return Fail(OutError,
						TEXT("rig_simulation_spatial_family_port_unavailable"),
						Contract.GetCapabilityId());
				}
				UObject* Object = Environment->FindMutation(Mutation.GetOperationId());
				if (!IsValid(Object)) return Fail(OutError,
					TEXT("rig_simulation_spatial_settle_target_unavailable"),
					Mutation.GetOperationId());
				if (UAnimSequence* Animation = Cast<UAnimSequence>(Object))
				{
					Animation->RefreshCacheData();
				}
				else if (ULevelSequence* Sequence = Cast<ULevelSequence>(Object))
				{
					UMovieSceneCompiledDataManager::GetPrecompiledData()->Compile(Sequence);
				}
				Object->PostEditChange();
				return TrySaveProjectAsset(*Object, OutError);
			}

		private:
			TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> Environment;
		};

		class FComparator final
			: public IUeAgentRigSimulationSpatialResultComparator
		{
		public:
			bool TryCompare(
				const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
				const FUeAgentRigSimulationSpatialTypedRequestRef& Request,
				const FUeAgentCanonicalSnapshot& Before,
				const FUeAgentCanonicalSnapshot& After,
				const FUeAgentOperationContext& Context,
				FVerificationOutcome& OutOutcome,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				OutError = {};
				if (!IsImplementedCapability(Contract.GetCapabilityId()))
				{
					return Fail(OutError,
						TEXT("rig_simulation_spatial_family_port_unavailable"),
						Contract.GetCapabilityId());
				}
				const FUeAgentRigSimulationSpatialRequest* Typed =
					ProductionRequest(Request, OutError);
				if (!Typed) return false;
				const FString BeforeState = SnapshotState(Before);
				const FString AfterState = SnapshotState(After);
				bool bMatches = !BeforeState.IsEmpty() && !AfterState.IsEmpty();
				if (Contract.GetCapabilityId()
					== TEXT("rig.skeleton.preview_mesh.bind"))
				{
					bMatches &= AfterState.Contains(TEXT("|matches=1"));
				}
				else if (Contract.GetCapabilityId()
					== TEXT("rig.skeleton.socket.update"))
				{
					const auto& Input = Typed->GetPayload()
						.Get<FRigSkeletonSocketUpdateInput>();
					bMatches &= Input.Operation == TEXT("remove")
						? AfterState.Contains(TEXT("|present=0"))
						: AfterState.Contains(TEXT("|present=1"))
							&& AfterState.Contains(TEXT("|parent_matches=1"));
				}
				else if (Contract.GetCapabilityId()
					== TEXT("rig.control.bake.animation"))
				{
					bMatches &= !AfterState.Contains(TEXT("|bone_tracks=0"))
						&& !AfterState.Contains(TEXT("|keys=0"));
				}
				else if (Contract.GetCapabilityId()
					== TEXT("rig.control.bake.controls"))
				{
					bMatches &= !AfterState.Contains(TEXT("|control_keys=0"));
				}
				OutOutcome = FVerificationOutcome(
					TEXT("rig-simulation-spatial-verify:")
						+ Contract.GetCapabilityId(),
					bMatches ? EVerificationStatus::Verified
						: EVerificationStatus::Mismatch,
					bMatches ? EEffectOutcome::Complete : EEffectOutcome::Partial,
					bMatches ? FString()
						: TEXT("rig_simulation_spatial_native_state_mismatch"),
					false, {},
					{TEXT("independent-state:") + After.GetSnapshotId()}, {});
				return true;
			}
		};
	}

	bool FUeAgentRigSimulationSpatialSkeletonBinding::IsValid() const
	{
		return !Identity.IsEmpty() && Skeleton.IsValid();
	}

	bool FUeAgentRigSimulationSpatialSocketBinding::IsValid() const
	{
		return !SkeletonIdentity.IsEmpty() && !SocketIdentity.IsEmpty()
			&& !SocketName.IsNone() && !ParentBoneIdentity.IsEmpty()
			&& !ParentBoneName.IsNone() && !NativeParentFrameId.IsEmpty();
	}

	bool FUeAgentRigSimulationSpatialRigBakeBinding::IsValid() const
	{
		return !Identity.IsEmpty() && World.IsValid() && Sequence.IsValid()
			&& SkeletalMeshComponent.IsValid()
			&& BindingGuid.IsValid() && !NativeAuthoredClockId.IsEmpty()
			&& !NativeControlFrameId.IsEmpty();
	}

	bool FUeAgentRigSimulationSpatialNativePorts::IsValid() const
	{
		return !SupportedCapabilities.IsEmpty()
			&& Readback.IsValid() && Mutation.IsValid() && Settle.IsValid()
			&& Comparator.IsValid();
	}

	bool FUeAgentRigSimulationSpatialNativePorts::Supports(
		const FString& CapabilityId) const
	{
		return SupportedCapabilities.Contains(CapabilityId);
	}

	bool TryCreateRigSimulationSpatialSkeletalRigBakeNativePorts(
		TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> Environment,
		FUeAgentRigSimulationSpatialNativePorts& OutPorts,
		FUeAgentRigSimulationSpatialExecutionError& OutError)
	{
		OutPorts = {};
		OutError = {};
		OutPorts.SupportedCapabilities = {
			TEXT("rig.skeleton.preview_mesh.bind"),
			TEXT("rig.skeleton.socket.update"),
			TEXT("rig.control.bake.animation"),
			TEXT("rig.control.bake.controls"),
		};
		OutPorts.Readback = MakeShared<FReadback>(Environment);
		OutPorts.Mutation = MakeShared<FMutation>(Environment);
		OutPorts.Settle = MakeShared<FSettle>(Environment);
		OutPorts.Comparator = MakeShared<FComparator>();
		return OutPorts.IsValid() ? true : Fail(OutError,
			TEXT("rig_simulation_spatial_native_ports_unavailable"));
	}
}
