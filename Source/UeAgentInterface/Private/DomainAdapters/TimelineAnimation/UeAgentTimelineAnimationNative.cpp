#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationNative.h"

#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Animation/Skeleton.h"
#include "Animation/WidgetAnimation.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Compilation/MovieSceneCompiledDataManager.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "ControlRig.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "FileHelpers.h"
#include "HAL/PlatformTime.h"
#include "LevelSequence.h"
#include "Misc/Crc.h"
#include "MovieScene.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "PackageTools.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Sequencer/MovieSceneControlRigParameterSection.h"
#include "Sequencer/MovieSceneControlRigParameterTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::DomainAdapters::TimelineAnimation
{
	using namespace BindingGateway::Operation;
	using namespace ContextResource;
	using namespace Discovery;
	using namespace StateSemantic;
	using namespace Verification;

	namespace
	{
		template <typename VisitorType>
		bool VisitMovieSceneTracks(const UMovieScene& Scene, VisitorType&& Visitor)
		{
			for (UMovieSceneTrack* Track : Scene.GetTracks())
			{
				if (Visitor(Track)) return true;
			}
			for (const FMovieSceneBinding& Binding : Scene.GetBindings())
			{
				for (UMovieSceneTrack* Track : Binding.GetTracks())
				{
					if (Visitor(Track)) return true;
				}
			}
			return false;
		}

		bool Fail(FUeAgentTimelineAnimationExecutionError& OutError,
			FString Code, FString Subject = FString(), FString Detail = FString())
		{
			OutError = {};
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(Subject);
			OutError.Detail = MoveTemp(Detail);
			return false;
		}

		void DiscardUnregisteredAsset(UObject*& Asset)
		{
			if (!IsValid(Asset))
			{
				Asset = nullptr;
				return;
			}
			Asset->ClearFlags(RF_Public | RF_Standalone);
			Asset->Rename(nullptr, GetTransientPackage(),
				REN_DontCreateRedirectors | REN_NonTransactional);
			Asset->MarkAsGarbage();
			Asset = nullptr;
		}

		const FUeAgentTimelineAnimationRequest* ProductionRequest(
			const FUeAgentTimelineAnimationTypedRequestRef& Request,
			FUeAgentTimelineAnimationExecutionError& OutError)
		{
			if (Request->GetTypeId().IsEmpty() || Request->GetFamilyId().IsEmpty()
				|| Request->GetRequestKindId().IsEmpty())
			{
				Fail(OutError, TEXT("timeline_animation_native_typed_request_invalid"),
					Request->GetTypeId());
				return nullptr;
			}
			return static_cast<const FUeAgentTimelineAnimationRequest*>(&Request.Get());
		}

		bool TryRate(const FTimelineAnimationRate& Rate, FFrameRate& OutRate,
			FUeAgentTimelineAnimationExecutionError& OutError)
		{
			int32 Numerator = 0;
			int32 Denominator = 0;
			if (!Rate.IsValid()
				|| !LexTryParseString(Numerator, *Rate.Numerator)
				|| !LexTryParseString(Denominator, *Rate.Denominator)
				|| Numerator <= 0 || Denominator <= 0)
			{
				return Fail(OutError, TEXT("timeline_animation_native_rate_invalid"),
					Rate.Numerator + TEXT("/") + Rate.Denominator);
			}
			OutRate = FFrameRate(Numerator, Denominator);
			return true;
		}

		bool TryPointTick(const FTimelineAnimationPoint& Point,
			const FString& ClockId, const FFrameRate& Rate, int64& OutTick,
			FUeAgentTimelineAnimationExecutionError& OutError)
		{
			FFrameRate PointRate;
			if (!TryRate(Point.Rate, PointRate, OutError)
				|| Point.ClockId != ClockId || PointRate != Rate
				|| !LexTryParseString(OutTick, *Point.Tick))
			{
				return OutError.IsValid() ? false : Fail(OutError,
					TEXT("timeline_animation_c4_exact_time_mismatch"), Point.ClockId,
					FString::Printf(TEXT("required_clock=%s required_rate=%d/%d"),
						*ClockId, Rate.Numerator, Rate.Denominator));
			}
			return true;
		}

		bool TryPointSeconds(const FTimelineAnimationPoint& Point,
			double& OutSeconds, FUeAgentTimelineAnimationExecutionError& OutError)
		{
			FFrameRate Rate;
			int64 Tick = 0;
			if (!TryRate(Point.Rate, Rate, OutError)
				|| !LexTryParseString(Tick, *Point.Tick)) return false;
			OutSeconds = static_cast<double>(Tick)
				* static_cast<double>(Rate.Denominator)
				/ static_cast<double>(Rate.Numerator);
			return FMath::IsFinite(OutSeconds);
		}

		bool TryClosedRangeTicks(const FTimelineAnimationRange& RangeValue,
			const FString& ClockId, const FFrameRate& Rate,
			int64& OutLower, int64& OutUpper,
			FUeAgentTimelineAnimationExecutionError& OutError)
		{
			if (RangeValue.ClockId != ClockId || !RangeValue.Lower.Point.IsSet()
				|| !RangeValue.Upper.Point.IsSet()
				|| !TryPointTick(*RangeValue.Lower.Point, ClockId, Rate,
					OutLower, OutError)
				|| !TryPointTick(*RangeValue.Upper.Point, ClockId, Rate,
					OutUpper, OutError)) return false;
			return OutUpper >= OutLower ? true : Fail(OutError,
				TEXT("timeline_animation_c4_range_order_invalid"), ClockId);
		}

		FString ObjectPath(const UObject& Object)
		{
			return Object.GetPathName();
		}

		FString TransformText(const FTransform& Value)
		{
			const FVector P = Value.GetTranslation();
			const FQuat R = Value.GetRotation();
			const FVector S = Value.GetScale3D();
			return FString::Printf(TEXT("p=%.9g,%.9g,%.9g;r=%.9g,%.9g,%.9g,%.9g;s=%.9g,%.9g,%.9g"),
				P.X, P.Y, P.Z, R.X, R.Y, R.Z, R.W, S.X, S.Y, S.Z);
		}

		FString GenericObjectState(UObject* Object)
		{
			if (!IsValid(Object)) return TEXT("absent");
			FString State = Object->GetClass()->GetPathName() + TEXT("|")
				+ Object->GetPathName();
			if (const UAnimSequence* Sequence = Cast<UAnimSequence>(Object))
			{
				const IAnimationDataModel* Model = Sequence->GetDataModel();
				State += FString::Printf(TEXT("|frames=%d|rate=%d/%d|tracks=%d|curves=%d|notifies=%d|markers=%d"),
					Model ? Model->GetNumberOfFrames() : -1,
					Model ? Model->GetFrameRate().Numerator : 0,
					Model ? Model->GetFrameRate().Denominator : 0,
					Model ? Model->GetNumBoneTracks() : -1,
					Model ? Model->GetFloatCurves().Num() : -1,
					Sequence->Notifies.Num(), Sequence->AuthoredSyncMarkers.Num());
				if (Model)
				{
					TArray<FName> BoneNames;
					Model->GetBoneTrackNames(BoneNames);
					for (const FName BoneName : BoneNames)
					{
						TArray<FTransform> BoneTransforms;
						Model->GetBoneTrackTransforms(BoneName, BoneTransforms);
						State += TEXT("|bone=") + BoneName.ToString()
							+ FString::Printf(TEXT(":%d"), BoneTransforms.Num());
					}
					for (const FFloatCurve& Curve : Model->GetFloatCurves())
						State += TEXT("|curve=") + Curve.GetName().ToString()
							+ FString::Printf(TEXT(":%d"), Curve.FloatCurve.GetNumKeys());
				}
				for (const FAnimNotifyEvent& Event : Sequence->Notifies)
					State += TEXT("|notify=") + Event.NotifyName.ToString();
				for (const FAnimSyncMarker& Marker : Sequence->AuthoredSyncMarkers)
					State += TEXT("|marker=") + Marker.MarkerName.ToString();
				if (UPackage* Package = Sequence->GetOutermost())
				{
					Package->GetMetaData();
					if (const TMap<FName, FString>* Values = FMetaData::GetMapForObject(Sequence))
						for (const TPair<FName, FString>& Pair : *Values)
							State += TEXT("|meta=") + Pair.Key.ToString() + TEXT(":") + Pair.Value;
				}
			}
			else if (const UAnimMontage* Montage = Cast<UAnimMontage>(Object))
			{
				State += FString::Printf(TEXT("|length=%.9g|sections=%d|slots=%d"),
					Montage->GetPlayLength(), Montage->CompositeSections.Num(),
					Montage->SlotAnimTracks.Num());
				for (const FCompositeSection& Section : Montage->CompositeSections)
					State += TEXT("|section=") + Section.SectionName.ToString();
				for (const FSlotAnimationTrack& Track : Montage->SlotAnimTracks)
					State += TEXT("|slot=") + Track.SlotName.ToString();
			}
			else if (const USkeleton* Skeleton = Cast<USkeleton>(Object))
			{
				for (const FAnimSlotGroup& Group : Skeleton->GetSlotGroups())
				{
					State += TEXT("|group=") + Group.GroupName.ToString();
					for (const FName Slot : Group.SlotNames)
						State += TEXT("|slot=") + Slot.ToString();
				}
			}
			else if (const ULevelSequence* LevelTimeline = Cast<ULevelSequence>(Object))
			{
				if (const UMovieScene* Scene = LevelTimeline->GetMovieScene())
				{
					State += FString::Printf(TEXT("|display=%d/%d|tick=%d/%d|start=%d|end=%d|bindings=%d|tracks=%d"),
						Scene->GetDisplayRate().Numerator, Scene->GetDisplayRate().Denominator,
						Scene->GetTickResolution().Numerator, Scene->GetTickResolution().Denominator,
						Scene->GetPlaybackRange().GetLowerBoundValue().Value,
						Scene->GetPlaybackRange().GetUpperBoundValue().Value,
						Scene->GetBindings().Num(), Scene->GetTracks().Num());
					int32 BoundTracks = 0;
					int32 Sections = 0;
					int32 Keys = 0;
					auto CountTrack = [&Sections, &Keys](UMovieSceneTrack* Track)
					{
						if (!Track)
						{
							return;
						}
						Sections += Track->GetAllSections().Num();
						for (UMovieSceneSection* Section : Track->GetAllSections())
						{
							const UMovieSceneFloatSection* FloatSection =
								Cast<UMovieSceneFloatSection>(Section);
							const FMovieSceneFloatChannel* Channel = FloatSection
								? FloatSection->GetChannelProxy().
									GetChannel<FMovieSceneFloatChannel>(0)
								: nullptr;
							if (Channel)
							{
								Keys += Channel->GetData().GetTimes().Num();
							}
						}
					};
					for (UMovieSceneTrack* Track : Scene->GetTracks())
					{
						CountTrack(Track);
					}
					for (const FMovieSceneBinding& Binding : Scene->GetBindings())
					{
						BoundTracks += Binding.GetTracks().Num();
						for (UMovieSceneTrack* Track : Binding.GetTracks())
						{
							CountTrack(Track);
						}
					}
					State += FString::Printf(
						TEXT("|bound_tracks=%d|sections=%d|keys=%d"),
						BoundTracks,
						Sections,
						Keys);
					State += TEXT("|signature=") + Scene->GetSignature().ToString();
				}
			}
			else if (const UWidgetAnimation* WidgetAnimation = Cast<UWidgetAnimation>(Object))
			{
				if (const UMovieScene* Scene = WidgetAnimation->GetMovieScene())
					State += FString::Printf(TEXT("|display=%d/%d|tick=%d/%d|bindings=%d|tracks=%d"),
						Scene->GetDisplayRate().Numerator, Scene->GetDisplayRate().Denominator,
						Scene->GetTickResolution().Numerator, Scene->GetTickResolution().Denominator,
						Scene->GetBindings().Num(), Scene->GetTracks().Num());
			}
			return State;
		}

		class FProductionEnvironment final
			: public IUeAgentTimelineAnimationEnvironment
			, public IUeAgentTimelineAnimationRevisionSource
		{
		public:
			FProductionEnvironment(FString InGeneration, FString InSession,
				TArray<FUeAgentTimelineAnimationSubjectBinding> InSubjects,
				TArray<FUeAgentTimelineAnimationRigCandidateBinding> InRigs)
				: Generation(MoveTemp(InGeneration)), Session(MoveTemp(InSession)),
					Subjects(MoveTemp(InSubjects)), Rigs(MoveTemp(InRigs))
			{
				for (const FUeAgentTimelineAnimationSubjectBinding& Binding : Subjects)
				{
					if (Binding.PoseComponent.IsValid())
						PinnedObjects.Emplace(Binding.PoseComponent.Get());
					if (Binding.MorphComponent.IsValid())
						PinnedObjects.Emplace(Binding.MorphComponent.Get());
				}
				for (const FUeAgentTimelineAnimationRigCandidateBinding& Binding : Rigs)
				{
					if (Binding.Sequence.IsValid()) PinnedObjects.Emplace(Binding.Sequence.Get());
					if (Binding.Rig.IsValid()) PinnedObjects.Emplace(Binding.Rig.Get());
				}
			}

			void GetExplicitSubjectIdentities(TArray<FString>& OutIds) const override
			{
				OutIds.Reset();
				for (const auto& Binding : Subjects) OutIds.Add(Binding.Identity);
			}
			void GetExplicitRigCandidateIdentities(TArray<FString>& OutIds) const override
			{
				OutIds.Reset();
				for (const auto& Binding : Rigs) OutIds.Add(Binding.Identity);
			}

			const FUeAgentTimelineAnimationSubjectBinding* FindSubject(
				const FString& Identity) const
			{
				return Subjects.FindByPredicate([&](const auto& B){ return B.Identity == Identity; });
			}
			const FUeAgentTimelineAnimationRigCandidateBinding* FindRig(
				const FString& Identity) const
			{
				return Rigs.FindByPredicate([&](const auto& B){ return B.Identity == Identity; });
			}

			UObject* Resolve(const FString& ExplicitRef) const
			{
				if (const auto* Subject = FindSubject(ExplicitRef))
					return Subject->PoseComponent.IsValid()
						? static_cast<UObject*>(Subject->PoseComponent.Get())
						: static_cast<UObject*>(Subject->MorphComponent.Get());
				if (const auto* Rig = FindRig(ExplicitRef)) return Rig->Sequence.Get();
				return StaticLoadObject(UObject::StaticClass(), nullptr, *ExplicitRef,
					nullptr, LOAD_NoWarn | LOAD_Quiet);
			}

			bool TryReadRevision(const FString& ExplicitRef,
				FTimelineAnimationRevision& OutRevision,
				FUeAgentTimelineAnimationExecutionError& OutError) const override
			{
				OutRevision = {};
				UObject* Object = Resolve(ExplicitRef);
				if (!IsValid(Object)) return Fail(OutError,
					TEXT("timeline_animation_explicit_target_unavailable"), ExplicitRef);
				FString State = GenericObjectState(Object);
				if (const auto* Subject = FindSubject(ExplicitRef))
				{
					if (UPoseableMeshComponent* Pose = Subject->PoseComponent.Get())
					{
						for (int32 Index = 0; Index < Pose->GetNumBones(); ++Index)
							State += TEXT("|subject_bone=")
								+ Pose->GetBoneName(Index).ToString() + TEXT(":")
								+ TransformText(Pose->GetBoneTransform(Index));
					}
					if (USkeletalMeshComponent* Morph = Subject->MorphComponent.Get())
						for (const TPair<FName, float>& Pair : Morph->GetMorphTargetCurves())
							State += FString::Printf(TEXT("|subject_morph=%s:%.9g"),
								*Pair.Key.ToString(), Pair.Value);
				}
				const uint64 Upper = FCrc::StrCrc32(*State);
				const uint64 Lower = FCrc::StrCrc32(*(Session + TEXT(":") + State));
				OutRevision.SourceId = TEXT("timeline-animation:") + ExplicitRef;
				OutRevision.Generation = Generation;
				OutRevision.Sequence = static_cast<int64>(
					((Upper << 32) | Lower) & ((1ULL << 53) - 1));
				return OutRevision.IsValid();
			}

			void RecordMutation(const FString& OperationId, UObject* Object)
			{
				FScopeLock Lock(&Mutex);
				MutatedObjects.Add(OperationId, Object);
			}
			void ForgetMutation(const FString& OperationId)
			{
				FScopeLock Lock(&Mutex);
				MutatedObjects.Remove(OperationId);
			}
			UObject* FindMutation(const FString& OperationId) const
			{
				FScopeLock Lock(&Mutex);
				const TWeakObjectPtr<UObject>* Found = MutatedObjects.Find(OperationId);
				return Found ? Found->Get() : nullptr;
			}
			void PreparePackageUnload(UPackage* Package)
			{
				PinnedObjects.RemoveAllSwap(
					[&](const TStrongObjectPtr<UObject>& Pinned)
					{
						UObject* const Object = Pinned.Get();
						return Object && Object->GetOutermost() == Package;
					});
			}
			bool TryRebindReloadedAsset(const FString& ObjectPath, UObject* Reloaded,
				FUeAgentTimelineAnimationExecutionError& OutError)
			{
				ULevelSequence* const Sequence = Cast<ULevelSequence>(Reloaded);
				for (FUeAgentTimelineAnimationRigCandidateBinding& Binding : Rigs)
				{
					if (Binding.Identity != ObjectPath) continue;
					if (!Sequence) return Fail(OutError,
						TEXT("timeline_animation_reload_sequence_type_mismatch"), ObjectPath);
					UControlRig* Rig = nullptr;
					if (UMovieScene* Scene = Sequence->GetMovieScene())
					{
						VisitMovieSceneTracks(*Scene,
							[&](UMovieSceneTrack* SceneTrack)
							{
								auto* Track = Cast<UMovieSceneControlRigParameterTrack>(SceneTrack);
								UControlRig* const Candidate = Track ? Track->GetControlRig() : nullptr;
								if (!Candidate || Candidate->GetPathName() != Binding.RigIdentity)
									return false;
								Rig = Candidate;
								return true;
							});
					}
					if (!Rig) return Fail(OutError,
						TEXT("timeline_animation_reload_rig_binding_unavailable"),
						Binding.RigIdentity);
					Binding.Sequence = Sequence;
					Binding.Rig = Rig;
					PinnedObjects.Emplace(Sequence);
					PinnedObjects.Emplace(Rig);
				}
				return true;
			}

		private:
			FString Generation;
			FString Session;
			TArray<FUeAgentTimelineAnimationSubjectBinding> Subjects;
			TArray<FUeAgentTimelineAnimationRigCandidateBinding> Rigs;
			TArray<TStrongObjectPtr<UObject>> PinnedObjects;
			mutable FCriticalSection Mutex;
			TMap<FString, TWeakObjectPtr<UObject>> MutatedObjects;
		};

		FString ExplicitRef(const FUeAgentTimelineAnimationRequest& Request,
			const TOptional<FUeAgentCandidateBinding>& Target)
		{
			if (Request.GetKind() == EUeAgentTimelineAnimationRequestKind::MontageCreate
				|| Request.GetKind() == EUeAgentTimelineAnimationRequestKind::LevelSequenceCreate)
				return Request.GetData().DestinationRef;
			return Target.IsSet() ? Target->GetPublicRef() : FString();
		}

		TOptional<FUeAgentCandidateBinding> FirstTarget(
			const TArray<FUeAgentOperationTargetBinding>& Targets)
		{
			return Targets.IsEmpty() ? TOptional<FUeAgentCandidateBinding>()
				: TOptional<FUeAgentCandidateBinding>(Targets[0].GetBinding());
		}

		bool TryStringValue(const FString& Text, const FUeAgentOperationContext& Context,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentTimelineAnimationExecutionError& OutError)
		{
			FUeAgentStateSemanticError Error;
			return FUeAgentCanonicalValue::TryString(Text, Context.GetResourceTracker(),
				OutValue, Error) ? true : Fail(OutError,
				TEXT("timeline_animation_native_string_failed"), Error.Path, Error.Code);
		}

		bool TrySnapshot(const FUeAgentTimelineAnimationExecutionChainContract& Contract,
			EUeAgentTimelineAnimationReadPhase Phase, const FString& PublicRef,
			const FString& State, bool bTargetPresent,
			const FUeAgentStateRevision* NativeRevision,
			const FUeAgentOperationContext& Context,
			TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
			FUeAgentTimelineAnimationExecutionError& OutError)
		{
			FUeAgentCanonicalValuePtr RefValue, CapabilityValue, StateValue, ObjectValue;
			FUeAgentStateSemanticError Error;
			if (!TryStringValue(PublicRef, Context, RefValue, OutError)
				|| !TryStringValue(Contract.GetCapabilityId(), Context, CapabilityValue, OutError)
				|| !TryStringValue(State, Context, StateValue, OutError)
				|| !FUeAgentCanonicalValue::TryObject({
					{TEXT("public_ref"), RefValue.ToSharedRef()},
					{TEXT("capability_id"), CapabilityValue.ToSharedRef()},
					{TEXT("observed_state"), StateValue.ToSharedRef()}},
					Context.GetResourceTracker(), ObjectValue, Error))
			{
				return OutError.IsValid() ? false : Fail(OutError,
					TEXT("timeline_animation_native_object_failed"), Error.Path, Error.Code);
			}
			const uint64 Upper = FCrc::StrCrc32(*State);
			const uint64 Lower = FCrc::StrCrc32(*(Contract.GetReadbackId() + State));
			const FUeAgentStateRevision DerivedRevision(Contract.GetReadbackId(),
				TEXT("native-observation-v1"),
				static_cast<int64>(((Upper << 32) | Lower) & ((1ULL << 53) - 1)));
			const FUeAgentStateRevision& Revision = NativeRevision
				? *NativeRevision : DerivedRevision;
			const FUeAgentStateEntry Entry(
				FUeAgentCanonicalIdentity(
					TEXT("timeline-animation:") + Contract.GetCapabilityId(),
					TEXT("timeline_animation_target"), PublicRef, PublicRef,
					TEXT("result"), TEXT("native-observation-v1")),
				FUeAgentCanonicalType(Contract.GetTypedResultId(),
					EUeAgentValueKind::Object, false, TEXT("object")),
				ObjectValue.ToSharedRef(),
				FUeAgentDefaultValue(EUeAgentDefaultSourceKind::DomainNative,
					Contract.GetReadbackId(), TEXT("independent-native-readback"),
					ObjectValue.ToSharedRef()),
				FUeAgentStateDetails(TEXT("result"),
					TEXT("independent TimelineAnimation native observation"),
					{TEXT("TimelineAnimation"), Contract.GetFamilyId()}, false,
					TEXT("available"), {}), Revision);
			TArray<FUeAgentStateEntry> Entries;
			if (bTargetPresent)
			{
				Entries.Add(Entry);
			}
			if (!FUeAgentCanonicalSnapshot::TryCreate(
					FString::Printf(TEXT("timeline-animation:%s:%d:%s"),
						*Contract.GetCapabilityId(), static_cast<int32>(Phase),
						*Context.GetRequestId()),
					Context.GetExecutionContext().GetStatePlane(), Revision,
					FUeAgentTemporalPoint(TEXT("editor-native"),
						FString::Printf(TEXT("%llu"), FPlatformTime::Cycles64())),
					FUeAgentStateProvenance(TEXT("independent_readback"),
						Contract.GetReadbackId(),
						TEXT("native-state:") + Contract.GetCapabilityId()),
					Entries, Context.GetResourceTracker(), OutSnapshot, Error))
			{
				return Fail(OutError, TEXT("timeline_animation_native_snapshot_failed"),
					Error.Path, Error.Code);
			}
			return true;
		}

		FString SnapshotState(const FUeAgentCanonicalSnapshot& Snapshot)
		{
			if (Snapshot.GetEntries().IsEmpty()) return FString();
			const FUeAgentCanonicalValueRef& Root = Snapshot.GetEntries()[0].GetValue();
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Pair : Root->GetObjectItems())
				if (Pair.Key == TEXT("observed_state")) return Pair.Value->GetLexeme();
			return FString();
		}

		FString QueryState(UObject* Object,
			const FUeAgentTimelineAnimationRequest& Request,
			FUeAgentTimelineAnimationExecutionError& OutError)
		{
			FString State = GenericObjectState(Object);
			const FTimelineAnimationRequestData& Data = Request.GetData();
			if (const UAnimSequence* Sequence = Cast<UAnimSequence>(Object))
			{
				const IAnimationDataModel* Model = Sequence->GetDataModel();
				const FFrameRate NativeRate = Model ? Model->GetFrameRate()
					: FFrameRate(0, 0);
				const FString NativeClock = TEXT("animation_sequence:")
					+ Sequence->GetPathName();
				int64 LowerTick = 0, UpperTick = 0, PointTick = 0;
				if ((Data.TemporalRange.IsSet()
						&& !TryClosedRangeTicks(*Data.TemporalRange, NativeClock,
							NativeRate, LowerTick, UpperTick, OutError))
					|| (Data.TimePoint.IsSet()
						&& !TryPointTick(*Data.TimePoint, NativeClock, NativeRate,
							PointTick, OutError))) return FString();
				if (Request.GetKind() == EUeAgentTimelineAnimationRequestKind::SequenceRootMotionAnalyze)
				{
					double Start = 0.0, End = 0.0;
					if (!Data.TemporalRange.IsSet() || !Data.TemporalRange->Lower.Point.IsSet()
						|| !Data.TemporalRange->Upper.Point.IsSet()
						|| !TryPointSeconds(*Data.TemporalRange->Lower.Point, Start, OutError)
						|| !TryPointSeconds(*Data.TemporalRange->Upper.Point, End, OutError))
						return FString();
					FAnimExtractContext Extract(Start);
					State += TEXT("|root_motion=") + TransformText(
						Sequence->ExtractRootMotionFromRange(Start, End, Extract));
				}
				else if (Request.GetKind() == EUeAgentTimelineAnimationRequestKind::SequencePoseSample)
				{
					double Seconds = 0.0;
					if (!Data.TimePoint.IsSet()
						|| !TryPointSeconds(*Data.TimePoint, Seconds, OutError)) return FString();
					const USkeleton* Skeleton = Sequence->GetSkeleton();
					for (const FString& BoneRef : Data.BoneRefs)
					{
						const int32 Index = Skeleton
							? Skeleton->GetReferenceSkeleton().FindBoneIndex(FName(*BoneRef))
							: INDEX_NONE;
						if (Index == INDEX_NONE) return Fail(OutError,
							TEXT("timeline_animation_bone_unavailable"), BoneRef), FString();
						FTransform Bone;
						const FAnimExtractContext Extraction(Seconds);
						Sequence->GetBoneTransform(Bone,
							FSkeletonPoseBoneIndex(Index), Extraction, true);
						State += TEXT("|pose=") + BoneRef + TEXT(":") + TransformText(Bone);
					}
				}
			}
			else if (const UBlendSpace* BlendSpace = Cast<UBlendSpace>(Object))
			{
				FVector Input = FVector::ZeroVector;
				for (int32 Index = 0; Index < Data.BlendParameters.Num() && Index < 3; ++Index)
					Input[Index] = Data.BlendParameters[Index].Value;
				TArray<FBlendSampleData> Samples;
				int32 Triangulation = INDEX_NONE;
				if (!BlendSpace->GetSamplesFromBlendInput(Input, Samples, Triangulation, true))
					return Fail(OutError, TEXT("timeline_animation_blend_sample_failed"),
						ObjectPath(*BlendSpace)), FString();
				for (const FBlendSampleData& Sample : Samples)
					State += FString::Printf(TEXT("|sample=%d:%.9g"),
						Sample.SampleDataIndex, Sample.TotalWeight);
			}
			return State;
		}

		bool CheckRevision(FProductionEnvironment& Environment,
			const FString& ExplicitRef, const FTimelineAnimationRevision& Expected,
			FUeAgentTimelineAnimationExecutionError& OutError)
		{
			FTimelineAnimationRevision Actual;
			if (!Environment.TryReadRevision(ExplicitRef, Actual, OutError)) return false;
			return Actual.SourceId == Expected.SourceId
				&& Actual.Generation == Expected.Generation
				&& Actual.Sequence == Expected.Sequence ? true : Fail(OutError,
					TEXT("timeline_animation_expected_revision_mismatch"), ExplicitRef,
					FString::Printf(TEXT("expected=%s/%s/%lld actual=%s/%s/%lld"),
						*Expected.SourceId, *Expected.Generation, Expected.Sequence,
						*Actual.SourceId, *Actual.Generation, Actual.Sequence));
		}

		bool SplitAssetRef(const FString& Ref, FString& OutPackage, FString& OutName)
		{
			OutPackage = Ref;
			int32 Dot = INDEX_NONE;
			if (OutPackage.FindLastChar(TEXT('.'), Dot)) OutPackage.LeftInline(Dot);
			int32 Slash = INDEX_NONE;
			if (!OutPackage.FindLastChar(TEXT('/'), Slash) || Slash + 1 >= OutPackage.Len())
				return false;
			OutName = OutPackage.Mid(Slash + 1);
			return OutPackage.StartsWith(TEXT("/Game/")) && !OutName.IsEmpty();
		}

		bool ApplySequenceTracks(UAnimSequence& Sequence,
			const FTimelineAnimationRequestData& Data,
			FUeAgentTimelineAnimationExecutionError& OutError)
		{
			FFrameRate Rate;
			if (!Data.SampleRate.IsSet() || !TryRate(*Data.SampleRate, Rate, OutError)) return false;
			const FString NativeClock = TEXT("animation_sequence:") + Sequence.GetPathName();
			int64 LowerTick = 0, UpperTick = 0;
			if (!Data.TemporalRange.IsSet()
				|| !TryClosedRangeTicks(*Data.TemporalRange, NativeClock, Rate,
					LowerTick, UpperTick, OutError)) return false;
			IAnimationDataController& Controller = Sequence.GetController();
			Controller.OpenBracket(FText::FromString(TEXT("UAI replace bone tracks")), false);
			Controller.RemoveAllBoneTracks(false);
			Controller.SetFrameRate(Rate, false);
			int32 KeyCount = 0;
			for (const auto& Track : Data.BoneTracks) KeyCount = FMath::Max(KeyCount, Track.Keys.Num());
			Controller.SetNumberOfFrames(FFrameNumber(FMath::Max(0, KeyCount - 1)), false);
			bool bOk = true;
			for (const FTimelineAnimationBoneTrack& Track : Data.BoneTracks)
			{
				if (Track.Keys.Num() != KeyCount || UpperTick - LowerTick != KeyCount)
				{
					bOk = Fail(OutError,
						TEXT("timeline_animation_bone_track_sampling_mismatch"),
						Track.BoneRef);
					break;
				}
				TArray<FVector> Positions;
				TArray<FQuat> Rotations;
				TArray<FVector> Scales;
				const FString NativeFrame = TEXT("animation_sequence:")
					+ Sequence.GetPathName() + TEXT(":bone:") + Track.BoneRef;
				for (int32 KeyIndex = 0; KeyIndex < Track.Keys.Num(); ++KeyIndex)
				{
					const FTimelineAnimationBoneKey& Key = Track.Keys[KeyIndex];
					int64 KeyTick = 0;
					if (!TryPointTick(Key.Time, NativeClock, Rate, KeyTick, OutError)
						|| KeyTick != LowerTick + KeyIndex)
					{
						bOk = OutError.IsValid() ? false : Fail(OutError,
							TEXT("timeline_animation_bone_track_key_time_mismatch"),
							Track.BoneRef);
						break;
					}
					FTransform Transform;
					if (!Key.Transform.TryToNativeTransform(NativeFrame, Transform, OutError))
					{
						bOk = false;
						break;
					}
					Positions.Add(Transform.GetTranslation());
					Rotations.Add(Transform.GetRotation());
					Scales.Add(Transform.GetScale3D());
				}
				if (!bOk) break;
				const FName BoneName(*Track.BoneRef);
				if (!Controller.AddBoneCurve(BoneName, false)
					&& !Sequence.GetDataModel()->IsValidBoneTrackName(BoneName))
				{
					bOk = false;
					Fail(OutError, TEXT("timeline_animation_bone_track_add_failed"), Track.BoneRef);
					break;
				}
				bOk = Controller.SetBoneTrackKeys(BoneName, Positions, Rotations, Scales, false);
				if (!bOk) Fail(OutError,
					TEXT("timeline_animation_bone_track_keys_failed"), Track.BoneRef);
			}
			Controller.CloseBracket(false);
			return bOk;
		}

		bool ApplySequenceCurves(UAnimSequence& Sequence,
			const FTimelineAnimationRequestData& Data,
			FUeAgentTimelineAnimationExecutionError& OutError)
		{
			IAnimationDataController& Controller = Sequence.GetController();
			const IAnimationDataModel* Model = Sequence.GetDataModel();
			const FFrameRate NativeRate = Model ? Model->GetFrameRate() : FFrameRate(0, 0);
			const FString NativeClock = TEXT("animation_sequence:") + Sequence.GetPathName();
			Controller.OpenBracket(FText::FromString(TEXT("UAI update curves")), false);
			bool bOk = true;
			for (const auto& Change : Data.CurveChanges)
			{
				const FAnimationCurveIdentifier Id(FName(*Change.CurveRef), ERawCurveTrackTypes::RCT_Float);
				if (Change.Operation == TEXT("remove"))
				{
					Controller.RemoveCurve(Id, false);
					continue;
				}
				Controller.AddCurve(Id, AACF_Editable, false);
				TArray<FRichCurveKey> Keys;
				for (const auto& Source : Change.Keys)
				{
					int64 Tick = 0;
					double Seconds = 0.0;
					if (!TryPointTick(Source.Time, NativeClock, NativeRate, Tick, OutError)
						|| !TryPointSeconds(Source.Time, Seconds, OutError)) { bOk = false; break; }
					FRichCurveKey Key(static_cast<float>(Seconds), static_cast<float>(Source.Value));
					Key.InterpMode = Source.Interpolation == TEXT("constant")
						? RCIM_Constant : Source.Interpolation == TEXT("linear")
							? RCIM_Linear : RCIM_Cubic;
					Keys.Add(Key);
				}
				if (!bOk || !Controller.SetCurveKeys(Id, Keys, false))
				{
					bOk = false;
					if (!OutError.IsValid()) Fail(OutError,
						TEXT("timeline_animation_curve_keys_failed"), Change.CurveRef);
					break;
				}
			}
			Controller.CloseBracket(false);
			return bOk;
		}

		bool ApplySequenceMetadata(UAnimSequence& Sequence,
			const FTimelineAnimationRequestData& Data,
			FUeAgentTimelineAnimationExecutionError& OutError)
		{
			UPackage* Package = Sequence.GetOutermost();
			if (!Package) return Fail(OutError,
				TEXT("timeline_animation_metadata_owner_unavailable"), Sequence.GetPathName());
			FMetaData& Meta = Package->GetMetaData();
			for (const auto& Change : Data.MetadataChanges)
			{
				if (Change.Operation == TEXT("remove"))
					Meta.RemoveValue(&Sequence, FName(*Change.MetadataRef));
				else Meta.SetValue(&Sequence, *Change.MetadataRef, *Change.ValueLexeme);
			}
			return true;
		}

		bool ApplySequenceEvents(UAnimSequence& Sequence,
			const FTimelineAnimationRequestData& Data,
			FUeAgentTimelineAnimationExecutionError& OutError)
		{
			const IAnimationDataModel* Model = Sequence.GetDataModel();
			const FFrameRate NativeRate = Model ? Model->GetFrameRate() : FFrameRate(0, 0);
			const FString NativeClock = TEXT("animation_sequence:") + Sequence.GetPathName();
			for (const auto& Change : Data.EventChanges)
			{
				const FName Name(*Change.EventRef);
				if (Change.EventKind == TEXT("notify_track"))
				{
#if WITH_EDITORONLY_DATA
					Sequence.AnimNotifyTracks.RemoveAll([&](const FAnimNotifyTrack& T){ return T.TrackName == Name; });
					if (Change.Operation == TEXT("upsert"))
					{
						FAnimNotifyTrack Track;
						Track.TrackName = Name;
						Sequence.AnimNotifyTracks.Add(Track);
					}
#endif
				}
				else if (Change.EventKind == TEXT("sync_marker"))
				{
					Sequence.AuthoredSyncMarkers.RemoveAll([&](const FAnimSyncMarker& M){ return M.MarkerName == Name; });
					if (Change.Operation == TEXT("upsert"))
					{
						int64 Tick = 0;
						double Seconds = 0.0;
						if (!Change.Time.IsSet()
							|| !TryPointTick(*Change.Time, NativeClock, NativeRate, Tick, OutError)
							|| !TryPointSeconds(*Change.Time, Seconds, OutError)) return false;
						FAnimSyncMarker Marker;
						Marker.MarkerName = Name;
						Marker.Time = static_cast<float>(Seconds);
						Sequence.AuthoredSyncMarkers.Add(Marker);
					}
				}
				else
				{
					Sequence.Notifies.RemoveAll([&](const FAnimNotifyEvent& E){ return E.NotifyName == Name; });
					if (Change.Operation == TEXT("upsert"))
					{
						int64 Tick = 0;
						double Seconds = 0.0;
						if (!Change.Time.IsSet()
							|| !TryPointTick(*Change.Time, NativeClock, NativeRate, Tick, OutError)
							|| !TryPointSeconds(*Change.Time, Seconds, OutError)) return false;
						FAnimNotifyEvent Event;
						Event.NotifyName = Name;
						Event.SetTime(static_cast<float>(Seconds));
						if (Change.Range.IsSet() && Change.Range->Lower.Point.IsSet()
							&& Change.Range->Upper.Point.IsSet())
						{
							double Start = 0.0, End = 0.0;
							if (!TryPointSeconds(*Change.Range->Lower.Point, Start, OutError)
								|| !TryPointSeconds(*Change.Range->Upper.Point, End, OutError)) return false;
							Event.SetDuration(FMath::Max(0.0, End - Start));
						}
						Sequence.Notifies.Add(Event);
					}
				}
			}
			Sequence.SortNotifies();
			Sequence.AuthoredSyncMarkers.Sort();
			return true;
		}

		bool ApplyMontageStructure(UAnimMontage& Montage,
			const FTimelineAnimationStructure& Structure,
			FUeAgentTimelineAnimationExecutionError& OutError)
		{
			const FString NativeClock = TEXT("animation_montage:") + Montage.GetPathName();
			if (Structure.PlaybackRange.ClockId != NativeClock) return Fail(OutError,
				TEXT("timeline_animation_c4_exact_time_mismatch"),
				Structure.PlaybackRange.ClockId, NativeClock);
			double Start = 0.0, End = 0.0;
			if (!Structure.PlaybackRange.Lower.Point.IsSet()
				|| !Structure.PlaybackRange.Upper.Point.IsSet()
				|| !TryPointSeconds(*Structure.PlaybackRange.Lower.Point, Start, OutError)
				|| !TryPointSeconds(*Structure.PlaybackRange.Upper.Point, End, OutError)) return false;
			Montage.SetCompositeLength(static_cast<float>(FMath::Max(0.0, End - Start)));
			Montage.CompositeSections.Reset();
			Montage.SlotAnimTracks.Reset();
			for (const auto& Binding : Structure.Bindings)
			{
				FSlotAnimationTrack Slot;
				Slot.SlotName = FName(*Binding.BindingRef);
				Montage.SlotAnimTracks.Add(MoveTemp(Slot));
				for (const auto& Track : Binding.Tracks)
					for (const auto& Section : Track.Sections)
					{
						double SectionStart = 0.0;
						if (!Section.Range.Lower.Point.IsSet()
							|| !TryPointSeconds(*Section.Range.Lower.Point, SectionStart, OutError)) return false;
						if (Montage.AddAnimCompositeSection(
								FName(*Section.SectionRef), static_cast<float>(SectionStart))
							== INDEX_NONE)
							return Fail(OutError,
								TEXT("timeline_animation_montage_section_add_failed"),
								Section.SectionRef);
					}
			}
			return true;
		}

		bool AddLevelSequenceFloatTrack(
			UMovieScene& Scene,
			const TOptional<FGuid>& BindingGuid,
			const FTimelineAnimationTrack& Track,
			const FString& NativeClock,
			const FFrameRate& TickResolution,
			FUeAgentTimelineAnimationExecutionError& OutError)
		{
			if (Track.TrackKind != TEXT("float")
				&& Track.TrackKind
					!= TEXT("/Script/MovieSceneTracks.MovieSceneFloatTrack"))
			{
				return Fail(
					OutError,
					TEXT("timeline_animation_level_sequence_track_kind_unsupported"),
					Track.TrackKind,
					Track.TrackRef);
			}
			UMovieSceneFloatTrack* NativeTrack = BindingGuid.IsSet()
				? Scene.AddTrack<UMovieSceneFloatTrack>(*BindingGuid)
				: Scene.AddTrack<UMovieSceneFloatTrack>();
			if (!NativeTrack)
			{
				return Fail(
					OutError,
					TEXT("timeline_animation_level_sequence_track_create_failed"),
					Track.TrackRef);
			}
			NativeTrack->SetDisplayName(FText::FromString(Track.TrackRef));
			if (BindingGuid.IsSet() && !Track.PropertyPath.IsEmpty())
			{
				NativeTrack->SetPropertyNameAndPath(
					FName(*Track.PropertyPath), Track.PropertyPath);
			}
			for (const FTimelineAnimationSection& Section : Track.Sections)
			{
				if (Section.SectionKind != TEXT("float")
					&& Section.SectionKind
						!= TEXT("/Script/MovieSceneTracks.MovieSceneFloatSection"))
				{
					return Fail(
						OutError,
						TEXT("timeline_animation_level_sequence_section_kind_unsupported"),
						Section.SectionKind,
						Section.SectionRef);
				}
				if (!Section.Range.Lower.Point.IsSet()
					|| !Section.Range.Upper.Point.IsSet())
				{
					return Fail(
						OutError,
						TEXT("timeline_animation_level_sequence_section_range_required"),
						Section.SectionRef);
				}
				int64 Lower = 0;
				int64 Upper = 0;
				if (!TryPointTick(
						*Section.Range.Lower.Point,
						NativeClock,
						TickResolution,
						Lower,
						OutError)
					|| !TryPointTick(
						*Section.Range.Upper.Point,
						NativeClock,
						TickResolution,
						Upper,
						OutError)
					|| Lower < MIN_int32 || Upper > MAX_int32 || Upper < Lower)
				{
					return Fail(
						OutError,
						TEXT("timeline_animation_level_sequence_section_range_invalid"),
						Section.SectionRef);
				}
				UMovieSceneFloatSection* NativeSection =
					Cast<UMovieSceneFloatSection>(NativeTrack->CreateNewSection());
				if (!NativeSection)
				{
					return Fail(
						OutError,
						TEXT("timeline_animation_level_sequence_section_create_failed"),
						Section.SectionRef);
				}
				NativeSection->SetRange(TRange<FFrameNumber>(
					TRangeBound<FFrameNumber>::Inclusive(
						FFrameNumber(static_cast<int32>(Lower))),
					TRangeBound<FFrameNumber>::Exclusive(
						FFrameNumber(static_cast<int32>(Upper)))));
				NativeTrack->AddSection(*NativeSection);
				FMovieSceneFloatChannel* Channel = NativeSection->
					GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(0);
				if (!Channel)
				{
					return Fail(
						OutError,
						TEXT("timeline_animation_level_sequence_channel_missing"),
						Section.SectionRef);
				}
				Channel->Reset();
				for (const FTimelineAnimationKey& Key : Section.Keys)
				{
					if (Key.ValueType != TEXT("decimal")
						&& Key.ValueType != TEXT("integer"))
					{
						return Fail(
							OutError,
							TEXT("timeline_animation_level_sequence_key_type_unsupported"),
							Key.ValueType,
							Key.KeyRef);
					}
					int64 Tick = 0;
					if (!TryPointTick(
							Key.Time,
							NativeClock,
							TickResolution,
							Tick,
							OutError)
						|| Tick < MIN_int32 || Tick > MAX_int32)
					{
						return false;
					}
					Channel->AddLinearKey(
						FFrameNumber(static_cast<int32>(Tick)),
						static_cast<float>(FCString::Atod(*Key.ValueLexeme)));
				}
			}
			return true;
		}

		bool ApplyLevelSequenceStructure(ULevelSequence& Sequence,
			const FTimelineAnimationStructure& Structure,
			FUeAgentTimelineAnimationExecutionError& OutError)
		{
			UMovieScene* Scene = Sequence.GetMovieScene();
			FFrameRate Display, Tick;
			const FString NativeClock = TEXT("level_sequence:") + Sequence.GetPathName();
			if (!Scene) return Fail(OutError,
				TEXT("timeline_animation_movie_scene_unavailable"), Sequence.GetPathName());
			if (Structure.PlaybackRange.ClockId != NativeClock) return Fail(OutError,
				TEXT("timeline_animation_c4_exact_time_mismatch"),
				Structure.PlaybackRange.ClockId, NativeClock);
			if (!TryRate(Structure.DisplayRate, Display, OutError)
				|| !TryRate(Structure.TickResolution, Tick, OutError)) return false;
			if (!Structure.PlaybackRange.Lower.Point.IsSet()
				|| !Structure.PlaybackRange.Upper.Point.IsSet()) return Fail(OutError,
					TEXT("timeline_animation_playback_range_closed_required"), Sequence.GetPathName());
			int64 Lower = 0, Upper = 0;
			if (!TryPointTick(*Structure.PlaybackRange.Lower.Point,
					Structure.PlaybackRange.ClockId, Tick, Lower, OutError)
				|| !TryPointTick(*Structure.PlaybackRange.Upper.Point,
					Structure.PlaybackRange.ClockId, Tick, Upper, OutError)) return false;
			if (Lower < MIN_int32 || Upper > MAX_int32 || Upper < Lower) return Fail(OutError,
				TEXT("timeline_animation_playback_range_invalid"), Sequence.GetPathName());
			Scene->Modify();
			Scene->SetDisplayRate(Display);
			Scene->SetTickResolutionDirectly(Tick);
			Scene->SetPlaybackRange(FFrameNumber(static_cast<int32>(Lower)),
				static_cast<int32>(Upper - Lower));
			TArray<UMovieSceneTrack*> ExistingRootTracks;
			for (UMovieSceneTrack* Existing : Scene->GetTracks())
			{
				ExistingRootTracks.Add(Existing);
			}
			for (UMovieSceneTrack* Existing : ExistingRootTracks)
			{
				if (Existing)
				{
					Scene->RemoveTrack(*Existing);
				}
			}
			TArray<FGuid> BindingIds;
			for (const FMovieSceneBinding& Existing : Scene->GetBindings())
			{
				BindingIds.Add(Existing.GetObjectGuid());
			}
			for (const FGuid& BindingId : BindingIds)
			{
				Scene->RemovePossessable(BindingId);
				Scene->RemoveSpawnable(BindingId);
			}
			for (const FTimelineAnimationBinding& Binding : Structure.Bindings)
			{
				if (Binding.BindingKind != TEXT("possessable"))
				{
					return Fail(
						OutError,
						TEXT("timeline_animation_level_sequence_binding_kind_unsupported"),
						Binding.BindingKind,
						Binding.BindingRef);
				}
				const FGuid BindingGuid = Scene->AddPossessable(
					Binding.BindingRef, UObject::StaticClass());
				if (!BindingGuid.IsValid())
				{
					return Fail(
						OutError,
						TEXT("timeline_animation_level_sequence_binding_create_failed"),
						Binding.BindingRef);
				}
				for (const FTimelineAnimationTrack& Track : Binding.Tracks)
				{
					if (!AddLevelSequenceFloatTrack(
							*Scene,
							TOptional<FGuid>(BindingGuid),
							Track,
							NativeClock,
							Tick,
							OutError))
					{
						return false;
					}
				}
			}
			for (const FTimelineAnimationTrack& Track : Structure.RootTracks)
			{
				if (!AddLevelSequenceFloatTrack(
						*Scene,
						TOptional<FGuid>(),
						Track,
						NativeClock,
						Tick,
						OutError))
				{
					return false;
				}
			}
			return true;
		}

		UObject* CreateAsset(const FUeAgentTimelineAnimationRequest& Request,
			FUeAgentTimelineAnimationExecutionError& OutError)
		{
			FString PackageName, AssetName;
			if (!SplitAssetRef(Request.GetData().DestinationRef, PackageName, AssetName))
			{
				Fail(OutError, TEXT("timeline_animation_destination_ref_invalid"),
					Request.GetData().DestinationRef);
				return nullptr;
			}
			if (FindObject<UObject>(nullptr, *(PackageName + TEXT(".") + AssetName)))
			{
				Fail(OutError, TEXT("timeline_animation_destination_exists"), PackageName);
				return nullptr;
			}
			UPackage* Package = CreatePackage(*PackageName);
			UObject* Asset = nullptr;
			if (Request.GetKind() == EUeAgentTimelineAnimationRequestKind::MontageCreate)
			{
				USkeleton* Skeleton = LoadObject<USkeleton>(nullptr,
					*Request.GetData().SkeletonRef, nullptr, LOAD_NoWarn | LOAD_Quiet);
				if (!Skeleton) return Fail(OutError,
					TEXT("timeline_animation_montage_create_dependency_missing"),
					Request.GetData().SkeletonRef), nullptr;
				UAnimMontage* Montage = NewObject<UAnimMontage>(Package, *AssetName,
					RF_Public | RF_Standalone | RF_Transactional);
				if (!Montage) return Fail(OutError,
					TEXT("timeline_animation_montage_create_failed"), PackageName), nullptr;
				Montage->SetSkeleton(Skeleton);
				if (!ApplyMontageStructure(*Montage, *Request.GetData().Structure, OutError))
				{
					Asset = Montage;
					DiscardUnregisteredAsset(Asset);
					return nullptr;
				}
				Asset = Montage;
			}
			else
			{
				ULevelSequence* Sequence = NewObject<ULevelSequence>(Package, *AssetName,
					RF_Public | RF_Standalone | RF_Transactional);
				if (!Sequence) return Fail(OutError,
					TEXT("timeline_animation_level_sequence_create_failed"), PackageName), nullptr;
				Sequence->Initialize();
				if (!ApplyLevelSequenceStructure(*Sequence, *Request.GetData().Structure, OutError))
				{
					Asset = Sequence;
					DiscardUnregisteredAsset(Asset);
					return nullptr;
				}
				Asset = Sequence;
			}
			FAssetRegistryModule::AssetCreated(Asset);
			Package->MarkPackageDirty();
			return Asset;
		}

		class FReadback final : public IUeAgentTimelineAnimationReadbackPort
		{
		public:
			explicit FReadback(TSharedRef<FProductionEnvironment> InEnvironment)
				: Environment(MoveTemp(InEnvironment)) {}
			bool TryRead(const FUeAgentTimelineAnimationExecutionChainContract& Contract,
				const FUeAgentTimelineAnimationTypedRequestRef& Request,
				const TOptional<FUeAgentCandidateBinding>& Target,
				EUeAgentTimelineAnimationReadPhase Phase,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentTimelineAnimationExecutionError& OutError) override
			{
				OutSnapshot.Reset(); OutError = {};
				const FUeAgentTimelineAnimationRequest* Typed = ProductionRequest(Request, OutError);
				if (!Typed) return false;
				const FString Ref = ExplicitRef(*Typed, Target);
				if (Ref.IsEmpty()) return Fail(OutError,
					TEXT("timeline_animation_explicit_target_required"), Contract.GetCapabilityId());
				UObject* Object = Environment->Resolve(Ref);
				if (!Object && Phase == EUeAgentTimelineAnimationReadPhase::Preflight
					&& (Typed->GetKind() == EUeAgentTimelineAnimationRequestKind::MontageCreate
						|| Typed->GetKind() == EUeAgentTimelineAnimationRequestKind::LevelSequenceCreate))
					return TrySnapshot(Contract, Phase, Ref, TEXT("absent:") + Ref, false, nullptr,
						Context, OutSnapshot, OutError);
				if (!IsValid(Object)) return Fail(OutError,
					TEXT("timeline_animation_explicit_target_unavailable"), Ref);
				const EUeAgentTimelineAnimationRequestKind Kind = Typed->GetKind();
				if ((Kind == EUeAgentTimelineAnimationRequestKind::SequenceRead
						|| Kind == EUeAgentTimelineAnimationRequestKind::SequenceRootMotionAnalyze
						|| Kind == EUeAgentTimelineAnimationRequestKind::SequencePoseSample)
					&& !Object->IsA<UAnimSequence>())
				{
					return Fail(OutError,
						TEXT("timeline_animation_sequence_type_mismatch"), Ref,
						Object->GetClass()->GetPathName());
				}
				if (Kind == EUeAgentTimelineAnimationRequestKind::BlendSpaceSample
					&& !Object->IsA<UBlendSpace>())
				{
					return Fail(OutError,
						TEXT("timeline_animation_blend_space_type_mismatch"), Ref,
						Object->GetClass()->GetPathName());
				}
				if (Kind == EUeAgentTimelineAnimationRequestKind::MontageRead
					&& !Object->IsA<UAnimMontage>())
				{
					return Fail(OutError,
						TEXT("timeline_animation_montage_type_mismatch"), Ref,
						Object->GetClass()->GetPathName());
				}
				if (Kind == EUeAgentTimelineAnimationRequestKind::SkeletonSlotsRead
					&& !Object->IsA<USkeleton>())
				{
					return Fail(OutError,
						TEXT("timeline_animation_skeleton_type_mismatch"), Ref,
						Object->GetClass()->GetPathName());
				}
				if (Typed->GetKind() == EUeAgentTimelineAnimationRequestKind::LevelSequenceRead
					&& !Object->IsA<ULevelSequence>())
				{
					return Fail(OutError,
						TEXT("timeline_animation_level_sequence_type_mismatch"), Ref,
						Object->GetClass()->GetPathName());
				}
				if (Typed->GetKind() == EUeAgentTimelineAnimationRequestKind::WidgetAnimationRead
					&& !Object->IsA<UWidgetAnimation>())
				{
					return Fail(OutError,
						TEXT("timeline_animation_widget_animation_type_mismatch"), Ref,
						Object->GetClass()->GetPathName());
				}

				FString State;
				if (const auto* Subject = Environment->FindSubject(Ref))
				{
					FFrameRate EvaluationRate;
					int64 EvaluationTick = 0;
					if (!Typed->GetData().EvaluationPoint.IsSet()
						|| !TryRate({Subject->EvaluationRateNumerator,
							Subject->EvaluationRateDenominator}, EvaluationRate, OutError)
						|| !TryPointTick(*Typed->GetData().EvaluationPoint,
							Subject->EvaluationClockId, EvaluationRate,
							EvaluationTick, OutError)) return false;
					State = GenericObjectState(Object);
					if (Typed->GetKind() == EUeAgentTimelineAnimationRequestKind::SubjectPoseUpdate)
					{
						UPoseableMeshComponent* Component = Subject->PoseComponent.Get();
						if (!Component) return Fail(OutError,
							TEXT("timeline_animation_pose_component_unavailable"), Ref);
						for (const auto& Item : Typed->GetData().Pose)
							State += TEXT("|pose=") + Item.BoneRef + TEXT(":")
								+ TransformText(Component->GetBoneTransformByName(
									FName(*Item.BoneRef), EBoneSpaces::ComponentSpace));
					}
					else
					{
						USkeletalMeshComponent* Component = Subject->MorphComponent.Get();
						if (!Component) return Fail(OutError,
							TEXT("timeline_animation_morph_component_unavailable"), Ref);
						for (const auto& Item : Typed->GetData().MorphWeights)
							State += FString::Printf(TEXT("|morph=%s:%.9g"),
								*Item.MorphRef, Component->GetMorphTarget(FName(*Item.MorphRef)));
					}
				}
				else if (const auto* Rig = Environment->FindRig(Ref))
				{
					int64 Tick = 0;
					FFrameRate Rate;
					if (!TryRate({Rig->AuthoredRateNumerator, Rig->AuthoredRateDenominator},
							Rate, OutError) || !Typed->GetData().AuthoredTime.IsSet()
						|| !TryPointTick(*Typed->GetData().AuthoredTime,
							Rig->AuthoredClockId, Rate, Tick, OutError)) return false;
					FTransform Value = FTransform::Identity;
					bool bObservedKey = false;
					if (const UMovieScene* Scene = Rig->Sequence->GetMovieScene())
					{
						const FFrameTime TickTime = FFrameRate::TransformTime(
							FFrameTime(FFrameNumber(static_cast<int32>(Tick))),
							Rate, Scene->GetTickResolution());
						VisitMovieSceneTracks(*Scene,
							[&](UMovieSceneTrack* SceneTrack)
							{
							const UMovieSceneControlRigParameterTrack* Track =
								Cast<UMovieSceneControlRigParameterTrack>(SceneTrack);
							if (!Track || Track->GetControlRig() != Rig->Rig.Get()) return false;
							for (UMovieSceneSection* SceneSection : Track->GetAllSections())
							{
								UMovieSceneControlRigParameterSection* Section =
									Cast<UMovieSceneControlRigParameterSection>(SceneSection);
								if (!Section) continue;
								const FTransformParameterNameAndCurves* Curves =
									Section->GetTransformParameterNamesAndCurves().FindByPredicate(
										[&](const FTransformParameterNameAndCurves& Item)
										{
											return Item.ParameterName == Rig->ControlName;
										});
								if (Curves)
								{
									float Translation[3] = {0.0f, 0.0f, 0.0f};
									float Rotation[3] = {0.0f, 0.0f, 0.0f};
									float Scale[3] = {1.0f, 1.0f, 1.0f};
									bObservedKey = Curves->Translation[0].Evaluate(TickTime, Translation[0])
										&& Curves->Translation[1].Evaluate(TickTime, Translation[1])
										&& Curves->Translation[2].Evaluate(TickTime, Translation[2])
										&& Curves->Rotation[0].Evaluate(TickTime, Rotation[0])
										&& Curves->Rotation[1].Evaluate(TickTime, Rotation[1])
										&& Curves->Rotation[2].Evaluate(TickTime, Rotation[2])
										&& Curves->Scale[0].Evaluate(TickTime, Scale[0])
										&& Curves->Scale[1].Evaluate(TickTime, Scale[1])
										&& Curves->Scale[2].Evaluate(TickTime, Scale[2]);
									if (bObservedKey)
									{
										Value = FTransform(
											FRotator(Rotation[1], Rotation[2], Rotation[0]),
											FVector(Translation[0], Translation[1], Translation[2]),
											FVector(Scale[0], Scale[1], Scale[2]));
										break;
									}
								}
							}
							return bObservedKey;
						});
					}
					if (!bObservedKey
						&& Phase == EUeAgentTimelineAnimationReadPhase::Preflight)
					{
						Value = Rig->Rig->GetControlLocalTransform(Rig->ControlName);
					}
					else if (!bObservedKey)
					{
						Fail(OutError,
							TEXT("timeline_animation_rig_key_readback_unavailable"), Ref);
						return false;
					}
					State = GenericObjectState(Rig->Sequence.Get()) + TEXT("|rig=")
						+ Rig->Rig->GetPathName() + TEXT("|goal=")
						+ Rig->ControlName.ToString() + TEXT(":") + TransformText(Value);
				}
				else State = QueryState(Object, *Typed, OutError);
				FTimelineAnimationRevision NativeRevision;
				if (State.IsEmpty()
					|| !Environment->TryReadRevision(Ref, NativeRevision, OutError))
				{
					return false;
				}
				const FUeAgentStateRevision SharedRevision(
					NativeRevision.SourceId, NativeRevision.Generation,
					NativeRevision.Sequence);
				return !State.IsEmpty() && TrySnapshot(Contract, Phase, Ref, State, true,
					&SharedRevision, Context, OutSnapshot, OutError);
			}
		private:
			TSharedRef<FProductionEnvironment> Environment;
		};

		class FMutation final : public IUeAgentTimelineAnimationMutationPort
		{
		public:
			explicit FMutation(TSharedRef<FProductionEnvironment> InEnvironment)
				: Environment(MoveTemp(InEnvironment)) {}
			bool TryMutate(const FUeAgentTimelineAnimationExecutionChainContract& Contract,
				const FUeAgentTimelineAnimationTypedRequestRef& Request,
				const TArray<FUeAgentOperationTargetBinding>& Targets,
				const FUeAgentOperationContext& Context,
				FUeAgentTimelineAnimationMutationResult& OutResult,
				FUeAgentTimelineAnimationExecutionError& OutError) override
			{
				OutResult = {}; OutError = {};
				const FUeAgentTimelineAnimationRequest* Typed = ProductionRequest(Request, OutError);
				if (!Typed || !Typed->IsOperation()) return false;
				const FString Ref = ExplicitRef(*Typed, FirstTarget(Targets));
				if (Ref.IsEmpty()) return Fail(OutError,
					TEXT("timeline_animation_explicit_target_required"), Contract.GetCapabilityId());
				UObject* Object = nullptr;
				if (Typed->GetKind() == EUeAgentTimelineAnimationRequestKind::MontageCreate
					|| Typed->GetKind() == EUeAgentTimelineAnimationRequestKind::LevelSequenceCreate)
					Object = CreateAsset(*Typed, OutError);
				else Object = Environment->Resolve(Ref);
				const auto& Data = Typed->GetData();
				if (Typed->GetKind() == EUeAgentTimelineAnimationRequestKind::SubjectPoseUpdate
					|| Typed->GetKind() == EUeAgentTimelineAnimationRequestKind::SubjectMorphWeightsUpdate)
				{
					const auto* Binding = Environment->FindSubject(Ref);
					FFrameRate EvaluationRate;
					int64 EvaluationTick = 0;
					if (!Binding || !Data.EvaluationPoint.IsSet()
						|| !TryRate({Binding->EvaluationRateNumerator,
							Binding->EvaluationRateDenominator}, EvaluationRate, OutError)
						|| !TryPointTick(*Data.EvaluationPoint, Binding->EvaluationClockId,
							EvaluationRate, EvaluationTick, OutError)) return false;
					Object = Typed->GetKind() == EUeAgentTimelineAnimationRequestKind::SubjectPoseUpdate
						? static_cast<UObject*>(Binding->PoseComponent.Get())
						: static_cast<UObject*>(Binding->MorphComponent.Get());
				}
				if (!IsValid(Object)) return OutError.IsValid() ? false : Fail(OutError,
					TEXT("timeline_animation_explicit_target_unavailable"), Ref);
				const EUeAgentTimelineAnimationRequestKind Kind = Typed->GetKind();
				if ((Kind == EUeAgentTimelineAnimationRequestKind::SequenceBoneTracksReplace
						|| Kind == EUeAgentTimelineAnimationRequestKind::SequenceCurvesUpdate
						|| Kind == EUeAgentTimelineAnimationRequestKind::SequenceMetadataUpdate
						|| Kind == EUeAgentTimelineAnimationRequestKind::SequenceEventsUpdate)
					&& !Object->IsA<UAnimSequence>())
				{
					return Fail(OutError,
						TEXT("timeline_animation_sequence_type_mismatch"), Ref,
						Object->GetClass()->GetPathName());
				}
				if (Kind == EUeAgentTimelineAnimationRequestKind::MontageStructureReplace
					&& !Object->IsA<UAnimMontage>())
				{
					return Fail(OutError,
						TEXT("timeline_animation_montage_type_mismatch"), Ref,
						Object->GetClass()->GetPathName());
				}
				if (Typed->GetKind()
						== EUeAgentTimelineAnimationRequestKind::LevelSequenceStructureReplace
					&& !Object->IsA<ULevelSequence>())
				{
					return Fail(OutError,
						TEXT("timeline_animation_level_sequence_type_mismatch"), Ref,
						Object->GetClass()->GetPathName());
				}

				if (Data.ExpectedRevision.IsSet()
					&& !CheckRevision(*Environment, Ref, *Data.ExpectedRevision, OutError)) return false;
				Object->Modify();
				bool bApplied = true;
				switch (Typed->GetKind())
				{
				case EUeAgentTimelineAnimationRequestKind::SequenceBoneTracksReplace:
					bApplied = ApplySequenceTracks(*CastChecked<UAnimSequence>(Object), Data, OutError); break;
				case EUeAgentTimelineAnimationRequestKind::SequenceCurvesUpdate:
					bApplied = ApplySequenceCurves(*CastChecked<UAnimSequence>(Object), Data, OutError); break;
				case EUeAgentTimelineAnimationRequestKind::SequenceMetadataUpdate:
					bApplied = ApplySequenceMetadata(*CastChecked<UAnimSequence>(Object), Data, OutError); break;
				case EUeAgentTimelineAnimationRequestKind::SequenceEventsUpdate:
					bApplied = ApplySequenceEvents(*CastChecked<UAnimSequence>(Object), Data, OutError); break;
				case EUeAgentTimelineAnimationRequestKind::MontageStructureReplace:
					bApplied = ApplyMontageStructure(*CastChecked<UAnimMontage>(Object), *Data.Structure, OutError); break;
				case EUeAgentTimelineAnimationRequestKind::LevelSequenceStructureReplace:
					bApplied = ApplyLevelSequenceStructure(*CastChecked<ULevelSequence>(Object), *Data.Structure, OutError); break;
				case EUeAgentTimelineAnimationRequestKind::SubjectPoseUpdate:
				{
					const auto* Binding = Environment->FindSubject(Ref);
					UPoseableMeshComponent* Component = Binding ? Binding->PoseComponent.Get() : nullptr;
					if (!Component) { bApplied = Fail(OutError,
						TEXT("timeline_animation_pose_component_unavailable"), Ref); break; }
					for (const auto& Item : Data.Pose)
					{
						FTransform Value;
						if (!Item.Transform.TryToNativeTransform(Binding->NativeFrameId, Value, OutError)) { bApplied = false; break; }
						if (Data.Weight.IsSet())
						{
							const FTransform Current = Component->GetBoneTransformByName(
								FName(*Item.BoneRef), EBoneSpaces::ComponentSpace);
							FTransform Blended;
							Blended.Blend(Current, Value, *Data.Weight);
							Value = Blended;
						}
						Component->SetBoneTransformByName(FName(*Item.BoneRef), Value,
							EBoneSpaces::ComponentSpace);
					}
					break;
				}
				case EUeAgentTimelineAnimationRequestKind::SubjectMorphWeightsUpdate:
				{
					const auto* Binding = Environment->FindSubject(Ref);
					USkeletalMeshComponent* Component = Binding ? Binding->MorphComponent.Get() : nullptr;
					if (!Component) { bApplied = Fail(OutError,
						TEXT("timeline_animation_morph_component_unavailable"), Ref); break; }
					for (const auto& Item : Data.MorphWeights)
						Component->SetMorphTarget(FName(*Item.MorphRef), Item.Weight, false);
					break;
				}
				case EUeAgentTimelineAnimationRequestKind::IkProbeCandidateApply:
				{
					const auto* Binding = Environment->FindRig(Ref);
					if (!Binding || !Data.Candidate.IsSet() || !Data.AuthoredTime.IsSet())
					{ bApplied = Fail(OutError, TEXT("timeline_animation_rig_binding_unavailable"), Ref); break; }
					FTransform Value;
					FFrameRate Rate;
					int64 Tick = 0;
					if (!Data.Candidate->TryToNativeTransform(Binding->NativeFrameId, Value, OutError)
						|| !TryRate({Binding->AuthoredRateNumerator,
							Binding->AuthoredRateDenominator}, Rate, OutError)
						|| !TryPointTick(*Data.AuthoredTime, Binding->AuthoredClockId,
							Rate, Tick, OutError)) { bApplied = false; break; }
					UMovieScene* Scene = Binding->Sequence->GetMovieScene();
					if (!Scene)
					{
						bApplied = Fail(OutError,
							TEXT("timeline_animation_rig_movie_scene_unavailable"), Ref);
						break;
					}
					const FFrameNumber KeyFrame = FFrameRate::TransformTime(
						FFrameTime(FFrameNumber(static_cast<int32>(Tick))), Rate,
						Scene->GetTickResolution()).RoundToFrame();
					UMovieSceneControlRigParameterSection* BoundSection = nullptr;
					VisitMovieSceneTracks(*Scene,
						[&](UMovieSceneTrack* SceneTrack)
						{
						UMovieSceneControlRigParameterTrack* Track =
							Cast<UMovieSceneControlRigParameterTrack>(SceneTrack);
						if (!Track || Track->GetControlRig() != Binding->Rig.Get()) return false;
						for (UMovieSceneSection* SceneSection : Track->GetAllSections())
						{
							BoundSection = Cast<UMovieSceneControlRigParameterSection>(SceneSection);
							if (BoundSection) break;
						}
						return BoundSection != nullptr;
					});
					if (!BoundSection)
					{
						bApplied = Fail(OutError,
							TEXT("timeline_animation_rig_parameter_section_unavailable"), Ref);
						break;
					}
					BoundSection->Modify();
					BoundSection->AddTransformParameterKey(Binding->ControlName, KeyFrame, Value);
					Binding->Rig->SetControlLocalTransform(Binding->ControlName, Value, true);
					Scene->Modify();
					Binding->Sequence->Modify();
					break;
				}
				case EUeAgentTimelineAnimationRequestKind::MontageCreate:
				case EUeAgentTimelineAnimationRequestKind::LevelSequenceCreate:
					break;
				default:
					bApplied = Fail(OutError,
						TEXT("timeline_animation_native_operation_unsupported"),
						Contract.GetCapabilityId());
					break;
				}
				if (!bApplied) return false;
				Object->MarkPackageDirty();
				const FString OperationId = Context.GetRequestId() + TEXT(":")
					+ Contract.GetCapabilityId();
				Environment->RecordMutation(OperationId, Object);
				OutResult = FUeAgentTimelineAnimationMutationResult(OperationId,
					TEXT("native-mutation:") + Object->GetPathName());
				return OutResult.IsValid();
			}
		private:
			TSharedRef<FProductionEnvironment> Environment;
		};

		class FSettle final : public IUeAgentTimelineAnimationSettlePort
		{
		public:
			explicit FSettle(TSharedRef<FProductionEnvironment> InEnvironment)
				: Environment(MoveTemp(InEnvironment)) {}
			bool TrySettle(const FUeAgentTimelineAnimationExecutionChainContract& Contract,
				const FUeAgentTimelineAnimationMutationResult& Mutation,
				const FUeAgentOperationContext& Context,
				FUeAgentTimelineAnimationExecutionError& OutError) override
			{
				OutError = {};
				UObject* Object = Environment->FindMutation(Mutation.GetOperationId());
				if (!IsValid(Object)) return Fail(OutError,
					TEXT("timeline_animation_settle_target_unavailable"), Mutation.GetOperationId());
				Object->PostEditChange();
				if (UAnimSequence* Sequence = Cast<UAnimSequence>(Object))
					Sequence->RefreshCacheData();
				else if (UAnimMontage* Montage = Cast<UAnimMontage>(Object))
				{
					Montage->RefreshCacheData();
				}
				else if (ULevelSequence* LevelSequence = Cast<ULevelSequence>(Object))
				{
					UMovieSceneCompiledDataManager::GetPrecompiledData()
						->Compile(LevelSequence);
				}
				else if (UPoseableMeshComponent* Pose = Cast<UPoseableMeshComponent>(Object))
					Pose->RefreshBoneTransforms();
				else if (USkeletalMeshComponent* Mesh = Cast<USkeletalMeshComponent>(Object))
					Mesh->RefreshBoneTransforms();
				if (Object->IsAsset())
				{
					UPackage* Package = Object->GetOutermost();
					const FString ObjectPath = Object->GetPathName();
					UClass* const ExpectedClass = Object->GetClass();
					if (!Package || !Package->GetName().StartsWith(TEXT("/Game/"))
						|| !UEditorLoadingAndSavingUtils::SavePackages({Package}, false))
					{
						return Fail(OutError,
							TEXT("timeline_animation_package_save_failed"), ObjectPath);
					}
					const TWeakObjectPtr<UObject> Original(Object);
					Environment->ForgetMutation(Mutation.GetOperationId());
					Environment->PreparePackageUnload(Package);
					Object = nullptr;
					FText UnloadError;
					const bool bUnloaded = UPackageTools::UnloadPackages(
						{Package}, UnloadError, true);
					CollectGarbage(RF_NoFlags, true);
					if (!bUnloaded || Original.IsValid())
					{
						return Fail(OutError,
							TEXT("timeline_animation_package_unload_failed"),
							ObjectPath, UnloadError.ToString());
					}
					UObject* const Reloaded = StaticLoadObject(
						ExpectedClass, nullptr, *ObjectPath, nullptr,
						LOAD_NoWarn | LOAD_Quiet);
					if (!Reloaded)
					{
						return Fail(OutError,
							TEXT("timeline_animation_package_reload_failed"), ObjectPath);
					}
					if (ULevelSequence* const ReloadedLevelSequence =
							Cast<ULevelSequence>(Reloaded))
					{
						UMovieSceneCompiledDataManager::GetPrecompiledData()
							->Compile(ReloadedLevelSequence);
					}
					if (!Environment->TryRebindReloadedAsset(ObjectPath, Reloaded, OutError))
						return false;
					Environment->RecordMutation(Mutation.GetOperationId(), Reloaded);
				}
				return true;
			}
		private:
			TSharedRef<FProductionEnvironment> Environment;
		};

		class FComparator final : public IUeAgentTimelineAnimationResultComparator
		{
		public:
			bool TryCompare(const FUeAgentTimelineAnimationExecutionChainContract& Contract,
				const FUeAgentTimelineAnimationTypedRequestRef& Request,
				const FUeAgentCanonicalSnapshot& Before,
				const FUeAgentCanonicalSnapshot& After,
				const FUeAgentOperationContext& Context,
				FVerificationOutcome& OutOutcome,
				FUeAgentTimelineAnimationExecutionError& OutError) override
			{
				OutError = {};
				const FUeAgentTimelineAnimationRequest* Typed = ProductionRequest(Request, OutError);
				if (!Typed) return false;
				const FString BeforeState = SnapshotState(Before);
				const FString AfterState = SnapshotState(After);
				const bool bCreate = Typed->GetKind()
						== EUeAgentTimelineAnimationRequestKind::MontageCreate
					|| Typed->GetKind()
						== EUeAgentTimelineAnimationRequestKind::LevelSequenceCreate;
				const bool bBeforeMatches = bCreate
					? Before.GetEntries().IsEmpty()
					: !BeforeState.IsEmpty();
				bool bMatches = bBeforeMatches && !AfterState.IsEmpty()
					&& (bCreate || BeforeState != AfterState)
					&& !AfterState.StartsWith(TEXT("absent:"));
				const auto ContainsAll = [&](const auto& Items, const TCHAR* Prefix,
					auto GetRef)
				{
					for (const auto& Item : Items)
						if (!AfterState.Contains(FString(Prefix) + GetRef(Item))) return false;
					return true;
				};
				const auto& Data = Typed->GetData();
				switch (Typed->GetKind())
				{
				case EUeAgentTimelineAnimationRequestKind::SequenceBoneTracksReplace:
					bMatches &= ContainsAll(Data.BoneTracks, TEXT("|bone="),
						[](const auto& V){ return V.BoneRef; }); break;
				case EUeAgentTimelineAnimationRequestKind::SequenceCurvesUpdate:
					for (const auto& V : Data.CurveChanges) if (V.Operation == TEXT("upsert"))
						bMatches &= AfterState.Contains(TEXT("|curve=") + V.CurveRef); break;
				case EUeAgentTimelineAnimationRequestKind::SequenceMetadataUpdate:
					for (const auto& V : Data.MetadataChanges) if (V.Operation == TEXT("upsert"))
						bMatches &= AfterState.Contains(TEXT("|meta=") + V.MetadataRef + TEXT(":")); break;
				case EUeAgentTimelineAnimationRequestKind::SequenceEventsUpdate:
					for (const auto& V : Data.EventChanges) if (V.Operation == TEXT("upsert"))
						bMatches &= AfterState.Contains(
							(V.EventKind == TEXT("sync_marker") ? TEXT("|marker=") : TEXT("|notify=")) + V.EventRef); break;
				case EUeAgentTimelineAnimationRequestKind::SubjectPoseUpdate:
					bMatches &= ContainsAll(Data.Pose, TEXT("|pose="),
						[](const auto& V){ return V.BoneRef; }); break;
				case EUeAgentTimelineAnimationRequestKind::SubjectMorphWeightsUpdate:
					bMatches &= ContainsAll(Data.MorphWeights, TEXT("|morph="),
						[](const auto& V){ return V.MorphRef; }); break;
				case EUeAgentTimelineAnimationRequestKind::IkProbeCandidateApply:
					bMatches &= AfterState.Contains(TEXT("|goal=") + Data.GoalRef + TEXT(":")); break;
				default: break;
				}
				OutOutcome = FVerificationOutcome(
					TEXT("timeline-animation-verify:") + Contract.GetCapabilityId(),
					bMatches ? EVerificationStatus::Verified : EVerificationStatus::Mismatch,
					bMatches ? EEffectOutcome::Complete : EEffectOutcome::Partial,
					bMatches ? FString() : TEXT("timeline_animation_native_state_mismatch"),
					false, {}, {TEXT("independent-state:") + After.GetSnapshotId()}, {});
				return true;
			}
		};
	}

	bool FUeAgentTimelineAnimationSubjectBinding::IsValid() const
	{
		return !Identity.IsEmpty() && (PoseComponent.IsValid() || MorphComponent.IsValid())
			&& !NativeFrameId.IsEmpty() && !EvaluationClockId.IsEmpty()
			&& FTimelineAnimationRate{EvaluationRateNumerator,
				EvaluationRateDenominator}.IsValid();
	}

	bool FUeAgentTimelineAnimationRigCandidateBinding::IsValid() const
	{
		return !Identity.IsEmpty() && Sequence.IsValid() && Rig.IsValid()
			&& !RigIdentity.IsEmpty()
			&& !ControlName.IsNone() && !NativeFrameId.IsEmpty()
			&& !AuthoredClockId.IsEmpty()
			&& FTimelineAnimationRate{AuthoredRateNumerator,
				AuthoredRateDenominator}.IsValid();
	}

	bool FUeAgentTimelineAnimationNativeDependencies::IsValid() const
	{
		return Environment.IsValid() && RevisionSource.IsValid();
	}

	bool FUeAgentTimelineAnimationNativePorts::IsValid() const
	{
		return Readback.IsValid() && Mutation.IsValid() && Settle.IsValid()
			&& Comparator.IsValid();
	}

	bool TryCreateTimelineAnimationProductionDependencies(
		const FString& RuntimeGeneration, const FString& ServerSessionId,
		const TArray<FUeAgentTimelineAnimationSubjectBinding>& SubjectBindings,
		const TArray<FUeAgentTimelineAnimationRigCandidateBinding>& RigBindings,
		FUeAgentTimelineAnimationNativeDependencies& OutDependencies,
		FUeAgentTimelineAnimationExecutionError& OutError)
	{
		OutDependencies = {}; OutError = {};
		if (RuntimeGeneration.IsEmpty() || ServerSessionId.IsEmpty()
			|| SubjectBindings.ContainsByPredicate([](const auto& B){ return !B.IsValid(); })
			|| RigBindings.ContainsByPredicate([](const auto& B){ return !B.IsValid(); }))
			return Fail(OutError,
				TEXT("timeline_animation_native_dependencies_invalid"));
		TSet<FString> Ids;
		for (const auto& Binding : SubjectBindings)
		{
			if (Ids.Contains(Binding.Identity)) return Fail(OutError,
				TEXT("timeline_animation_explicit_binding_duplicate"), Binding.Identity);
			Ids.Add(Binding.Identity);
		}
		for (const auto& Binding : RigBindings)
		{
			if (Ids.Contains(Binding.Identity)) return Fail(OutError,
				TEXT("timeline_animation_explicit_binding_duplicate"), Binding.Identity);
			Ids.Add(Binding.Identity);
		}
		const TSharedRef<FProductionEnvironment> Environment =
			MakeShared<FProductionEnvironment>(RuntimeGeneration, ServerSessionId,
				SubjectBindings, RigBindings);
		OutDependencies.Environment = Environment;
		OutDependencies.RevisionSource = Environment;
		return OutDependencies.IsValid();
	}

	bool TryCreateTimelineAnimationNativePorts(
		const FUeAgentTimelineAnimationNativeDependencies& Dependencies,
		FUeAgentTimelineAnimationNativePorts& OutPorts,
		FUeAgentTimelineAnimationExecutionError& OutError)
	{
		OutPorts = {}; OutError = {};
		if (!Dependencies.IsValid()) return Fail(OutError,
			TEXT("timeline_animation_native_dependencies_invalid"));
		const TSharedPtr<FProductionEnvironment> Environment =
			StaticCastSharedPtr<FProductionEnvironment>(Dependencies.Environment);
		if (!Environment.IsValid()) return Fail(OutError,
			TEXT("timeline_animation_native_environment_type_invalid"));
		OutPorts.Readback = MakeShared<FReadback>(Environment.ToSharedRef());
		OutPorts.Mutation = MakeShared<FMutation>(Environment.ToSharedRef());
		OutPorts.Settle = MakeShared<FSettle>(Environment.ToSharedRef());
		OutPorts.Comparator = MakeShared<FComparator>();
		return OutPorts.IsValid();
	}
}
