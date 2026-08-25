#include "Misc/AutomationTest.h"

#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Compilation/MovieSceneCompiledDataManager.h"
#include "Animation/BlendSpace.h"
#include "Animation/Skeleton.h"
#include "Animation/WidgetAnimation.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "ControlRig.h"
#include "Core/BindingGateway/Query/UeAgentQueryBindingError.h"
#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationNative.h"
#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationProductionRuntime.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/FileManager.h"
#include "LevelSequence.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "MovieScene.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "PackageTools.h"
#include "PreviewScene.h"
#include "Sequencer/MovieSceneControlRigParameterTrack.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "Rigs/AdditiveControlRig.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	using namespace UE::UAI::BindingGateway::Operation;
	using namespace UE::UAI::BindingGateway::Query;
	using namespace UE::UAI::BindingGateway::Ticket;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Discovery;
	using namespace UE::UAI::DomainAdapters::TimelineAnimation;
	using namespace UE::UAI::ExecutionMutation;
	using namespace UE::UAI::Projection;
	using namespace UE::UAI::StateSemantic;
	using namespace UE::UAI::Verification;

	struct FFixture
	{
		FFixture()
			: PackageRoot(TEXT("/Game/UAI/Tests/TimelineAnimation/TS010A_")
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits))
		{
		}

		FString AddPackage(const FString& Stem)
		{
			const FString PackageName = PackageRoot + TEXT("/") + Stem;
			PackageNames.AddUnique(PackageName);
			return PackageName;
		}

		FString ObjectPath(const FString& PackageName) const
		{
			return PackageName + TEXT(".")
				+ FPackageName::GetLongPackageAssetName(PackageName);
		}

		bool Save(UObject& Asset) const
		{
			UPackage* const Package = Asset.GetOutermost();
			if (!Package || !PackageNames.Contains(Package->GetName()))
			{
				return false;
			}
			Asset.SetFlags(RF_Public | RF_Standalone | RF_Transactional);
			FAssetRegistryModule::AssetCreated(&Asset);
			Package->MarkPackageDirty();
			FSavePackageArgs Args;
			Args.TopLevelFlags = RF_Public | RF_Standalone;
			Args.SaveFlags = SAVE_NoError;
			const FString Filename = FPackageName::LongPackageNameToFilename(
				Package->GetName(), FPackageName::GetAssetPackageExtension());
			const bool bSaved = UPackage::SavePackage(
				Package, &Asset, *Filename, Args);
			Package->SetDirtyFlag(false);
			return bSaved;
		}

		bool Cleanup()
		{
			PreviewScene.Reset();
			Mesh = nullptr;
			Skeleton = nullptr;
			Sequence = nullptr;
			BlendSpace = nullptr;
			Montage = nullptr;
			LevelSequence = nullptr;
			WidgetAnimation = nullptr;
			PoseComponent = nullptr;
			MorphComponent = nullptr;
			RigSequence = nullptr;
			Rig = nullptr;
			CollectGarbage(RF_NoFlags, true);

			bool bClean = true;
			for (const FString& PackageName : PackageNames)
			{
				if (UPackage* const Package = FindPackage(nullptr, *PackageName))
				{
					if (UObject* const Asset = FindObject<UObject>(
							Package,
							*FPackageName::GetLongPackageAssetName(PackageName)))
					{
						FAssetRegistryModule::AssetDeleted(Asset);
					}
					Package->SetDirtyFlag(false);
					FText UnloadError;
					bClean = UPackageTools::UnloadPackages(
						{Package}, UnloadError, true) && bClean;
				}
				CollectGarbage(RF_NoFlags, true);
				const FString Filename = FPackageName::LongPackageNameToFilename(
					PackageName, FPackageName::GetAssetPackageExtension());
				if (IFileManager::Get().FileExists(*Filename))
				{
					bClean = IFileManager::Get().Delete(
						*Filename, false, true) && bClean;
				}
				bClean = !IFileManager::Get().FileExists(*Filename)
					&& !FindPackage(nullptr, *PackageName) && bClean;
			}

			FString ContentDirectory;
			if (!FPackageName::TryConvertLongPackageNameToFilename(
					PackageRoot,
					ContentDirectory))
			{
				return false;
			}
			ContentDirectory = FPaths::ConvertRelativePathToFull(
				ContentDirectory);
			if (IFileManager::Get().DirectoryExists(*ContentDirectory))
			{
				TArray<FString> RemainingFiles;
				IFileManager::Get().FindFilesRecursive(
					RemainingFiles,
					*ContentDirectory,
					TEXT("*"),
					true,
					false,
					false);
				if (!RemainingFiles.IsEmpty())
				{
					bClean = false;
				}
				else
				{
					bClean = IFileManager::Get().DeleteDirectory(
						*ContentDirectory,
						false,
						true) && bClean;
				}
			}
			return !IFileManager::Get().DirectoryExists(*ContentDirectory)
				&& bClean;
		}

		~FFixture()
		{
			Cleanup();
		}

		FString PackageRoot;
		TArray<FString> PackageNames;
		TUniquePtr<FPreviewScene> PreviewScene;
		USkeletalMesh* Mesh = nullptr;
		USkeleton* Skeleton = nullptr;
		UAnimSequence* Sequence = nullptr;
		UBlendSpace* BlendSpace = nullptr;
		UAnimMontage* Montage = nullptr;
		ULevelSequence* LevelSequence = nullptr;
		UWidgetAnimation* WidgetAnimation = nullptr;
		UPoseableMeshComponent* PoseComponent = nullptr;
		USkeletalMeshComponent* MorphComponent = nullptr;
		ULevelSequence* RigSequence = nullptr;
		UControlRig* Rig = nullptr;
		FName RootBone;
		FName MorphName = TEXT("TS010A_Morph");
		FName ControlName = TEXT("Goal");
		FString SubjectIdentity = TEXT("subject:ts010a");
		FString RigCandidateRef;
		FString SequenceRef;
		FString BlendSpaceRef;
		FString MontageRef;
		FString LevelSequenceRef;
		FString WidgetAnimationRef;
		FString RigRef;
	};

	FUeAgentResourceTrackerOwner MakeResources()
	{
		return MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>(
			FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 4 * 1024 * 1024},
				{EUeAgentResourceDimension::OutputBytes, 4 * 1024 * 1024},
				{EUeAgentResourceDimension::Nodes, 100000},
				{EUeAgentResourceDimension::Items, 100000},
				{EUeAgentResourceDimension::StringBytes, 4 * 1024 * 1024},
				{EUeAgentResourceDimension::WorkUnits, 100000},
				{EUeAgentResourceDimension::EstimatedMemoryBytes, 16 * 1024 * 1024},
			}));
	}

	TSharedPtr<FJsonObject> JsonRate(const FTimelineAnimationRate& Value)
	{
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("numerator"), Value.Numerator);
		Result->SetStringField(TEXT("denominator"), Value.Denominator);
		return Result;
	}

	TSharedPtr<FJsonObject> JsonPoint(const FTimelineAnimationPoint& Value)
	{
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("clock_id"), Value.ClockId);
		Result->SetStringField(TEXT("tick"), Value.Tick);
		Result->SetObjectField(TEXT("rate"), JsonRate(Value.Rate));
		Result->SetStringField(TEXT("state"), Value.State);
		return Result;
	}

	TSharedPtr<FJsonObject> JsonBound(const FTimelineAnimationBound& Value)
	{
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("kind"), Value.Kind);
		if (Value.Point.IsSet()) Result->SetObjectField(TEXT("point"), JsonPoint(*Value.Point));
		return Result;
	}

	TSharedPtr<FJsonObject> JsonRange(const FTimelineAnimationRange& Value)
	{
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("clock_id"), Value.ClockId);
		Result->SetObjectField(TEXT("lower"), JsonBound(Value.Lower));
		Result->SetObjectField(TEXT("upper"), JsonBound(Value.Upper));
		Result->SetStringField(TEXT("state"), Value.State);
		return Result;
	}

	TSharedPtr<FJsonObject> JsonFrame(const FTimelineAnimationFrame3d& Value)
	{
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("frame_id"), Value.FrameId);
		Result->SetStringField(TEXT("basis_frame_id"), Value.BasisFrameId);
		Result->SetNumberField(TEXT("dimension"), Value.Dimension);
		Result->SetStringField(TEXT("unit"), Value.Unit);
		Result->SetStringField(TEXT("basis_evidence_id"), Value.BasisEvidenceId);
		return Result;
	}

	TArray<TSharedPtr<FJsonValue>> JsonStrings(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const FString& Value : Values) Result.Add(MakeShared<FJsonValueString>(Value));
		return Result;
	}

	TSharedPtr<FJsonObject> JsonTransform(const FTimelineAnimationTransform3d& Value)
	{
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("subject"), Value.Subject);
		Result->SetStringField(TEXT("quantity"), Value.Quantity);
		Result->SetObjectField(TEXT("frame"), JsonFrame(Value.Frame));
		Result->SetArrayField(TEXT("components"), JsonStrings(Value.Components));
		Result->SetArrayField(TEXT("conversion_evidence_ids"),
			JsonStrings(Value.ConversionEvidenceIds));
		return Result;
	}

	TSharedPtr<FJsonObject> JsonRevision(const FTimelineAnimationRevision& Value)
	{
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("source_id"), Value.SourceId);
		Result->SetStringField(TEXT("generation"), Value.Generation);
		Result->SetStringField(TEXT("sequence"), LexToString(Value.Sequence));
		return Result;
	}

	template <typename ItemType, typename ConvertType>
	TArray<TSharedPtr<FJsonValue>> JsonObjects(
		const TArray<ItemType>& Values, ConvertType Convert)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const ItemType& Value : Values)
			Result.Add(MakeShared<FJsonValueObject>(Convert(Value)));
		return Result;
	}

	TSharedPtr<FJsonObject> JsonTimelineKey(const FTimelineAnimationKey& Value)
	{
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("key_ref"), Value.KeyRef);
		Result->SetObjectField(TEXT("time"), JsonPoint(Value.Time));
		Result->SetStringField(TEXT("value_type"), Value.ValueType);
		if (!Value.ValueLexeme.IsEmpty())
			Result->SetStringField(TEXT("value_lexeme"), Value.ValueLexeme);
		if (Value.Transform.IsSet())
			Result->SetObjectField(TEXT("transform"), JsonTransform(*Value.Transform));
		return Result;
	}

	TSharedPtr<FJsonObject> JsonSection(const FTimelineAnimationSection& Value)
	{
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("section_ref"), Value.SectionRef);
		Result->SetStringField(TEXT("section_kind"), Value.SectionKind);
		Result->SetObjectField(TEXT("range"), JsonRange(Value.Range));
		if (!Value.SourceRef.IsEmpty()) Result->SetStringField(TEXT("source_ref"), Value.SourceRef);
		Result->SetArrayField(TEXT("keys"),
			JsonObjects(Value.Keys, JsonTimelineKey));
		return Result;
	}

	TSharedPtr<FJsonObject> JsonTrack(const FTimelineAnimationTrack& Value)
	{
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("track_ref"), Value.TrackRef);
		Result->SetStringField(TEXT("track_kind"), Value.TrackKind);
		if (!Value.PropertyPath.IsEmpty())
			Result->SetStringField(TEXT("property_path"), Value.PropertyPath);
		Result->SetArrayField(TEXT("sections"), JsonObjects(Value.Sections, JsonSection));
		return Result;
	}

	TSharedPtr<FJsonObject> JsonBinding(const FTimelineAnimationBinding& Value)
	{
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("binding_ref"), Value.BindingRef);
		Result->SetStringField(TEXT("binding_kind"), Value.BindingKind);
		if (!Value.ObjectRef.IsEmpty()) Result->SetStringField(TEXT("object_ref"), Value.ObjectRef);
		Result->SetArrayField(TEXT("tracks"), JsonObjects(Value.Tracks, JsonTrack));
		return Result;
	}

	TSharedPtr<FJsonObject> JsonStructure(const FTimelineAnimationStructure& Value)
	{
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetObjectField(TEXT("display_rate"), JsonRate(Value.DisplayRate));
		Result->SetObjectField(TEXT("tick_resolution"), JsonRate(Value.TickResolution));
		Result->SetObjectField(TEXT("playback_range"), JsonRange(Value.PlaybackRange));
		Result->SetArrayField(TEXT("bindings"), JsonObjects(Value.Bindings, JsonBinding));
		Result->SetArrayField(TEXT("root_tracks"), JsonObjects(Value.RootTracks, JsonTrack));
		return Result;
	}

	FUeAgentContractValue CanonicalInput(
		EUeAgentTimelineAnimationRequestKind Kind,
		const FTimelineAnimationRequestData& Data)
	{
		const TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		auto SetRevision = [&]()
		{
			Root->SetObjectField(TEXT("expected_revision"), JsonRevision(*Data.ExpectedRevision));
		};
		switch (Kind)
		{
		case EUeAgentTimelineAnimationRequestKind::SequenceRead:
			if (Data.TemporalRange.IsSet()) Root->SetObjectField(TEXT("temporal_range"), JsonRange(*Data.TemporalRange));
			if (Data.IncludeTracks.IsSet()) Root->SetBoolField(TEXT("include_tracks"), *Data.IncludeTracks);
			if (Data.IncludeKeys.IsSet()) Root->SetBoolField(TEXT("include_keys"), *Data.IncludeKeys);
			break;
		case EUeAgentTimelineAnimationRequestKind::SequenceRootMotionAnalyze:
			Root->SetObjectField(TEXT("temporal_range"), JsonRange(*Data.TemporalRange));
			Root->SetObjectField(TEXT("sample_rate"), JsonRate(*Data.SampleRate));
			if (!Data.RootBoneRef.IsEmpty()) Root->SetStringField(TEXT("root_bone_ref"), Data.RootBoneRef);
			break;
		case EUeAgentTimelineAnimationRequestKind::SequencePoseSample:
			Root->SetObjectField(TEXT("time_point"), JsonPoint(*Data.TimePoint));
			Root->SetArrayField(TEXT("bone_refs"), JsonStrings(Data.BoneRefs));
			if (Data.OutputFrame.IsSet()) Root->SetObjectField(TEXT("output_frame"), JsonFrame(*Data.OutputFrame));
			break;
		case EUeAgentTimelineAnimationRequestKind::BlendSpaceSample:
			Root->SetArrayField(TEXT("blend_parameters"), JsonObjects(
				Data.BlendParameters, [](const FTimelineAnimationBlendParameter& Value)
				{
					const TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("axis_ref"), Value.AxisRef);
					Item->SetNumberField(TEXT("value"), Value.Value);
					return Item;
				}));
			if (Data.TimePoint.IsSet()) Root->SetObjectField(TEXT("time_point"), JsonPoint(*Data.TimePoint));
			if (!Data.BoneRefs.IsEmpty()) Root->SetArrayField(TEXT("bone_refs"), JsonStrings(Data.BoneRefs));
			if (Data.OutputFrame.IsSet()) Root->SetObjectField(TEXT("output_frame"), JsonFrame(*Data.OutputFrame));
			break;
		case EUeAgentTimelineAnimationRequestKind::SequenceBoneTracksReplace:
			Root->SetObjectField(TEXT("authored_range"), JsonRange(*Data.TemporalRange));
			Root->SetObjectField(TEXT("sample_rate"), JsonRate(*Data.SampleRate));
			Root->SetArrayField(TEXT("bone_tracks"), JsonObjects(
				Data.BoneTracks, [](const FTimelineAnimationBoneTrack& Track)
				{
					const TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("bone_ref"), Track.BoneRef);
					Item->SetArrayField(TEXT("keys"), JsonObjects(
						Track.Keys, [](const FTimelineAnimationBoneKey& Key)
						{
							const TSharedPtr<FJsonObject> KeyObject = MakeShared<FJsonObject>();
							KeyObject->SetObjectField(TEXT("time"), JsonPoint(Key.Time));
							KeyObject->SetObjectField(TEXT("transform"), JsonTransform(Key.Transform));
							return KeyObject;
						}));
					return Item;
				}));
			SetRevision();
			break;
		case EUeAgentTimelineAnimationRequestKind::SequenceCurvesUpdate:
			Root->SetArrayField(TEXT("curve_changes"), JsonObjects(
				Data.CurveChanges, [](const FTimelineAnimationCurveChange& Change)
				{
					const TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("curve_ref"), Change.CurveRef);
					Item->SetStringField(TEXT("operation"), Change.Operation);
					Item->SetArrayField(TEXT("keys"), JsonObjects(
						Change.Keys, [](const FTimelineAnimationCurveKey& Key)
						{
							const TSharedPtr<FJsonObject> KeyObject = MakeShared<FJsonObject>();
							KeyObject->SetObjectField(TEXT("time"), JsonPoint(Key.Time));
							KeyObject->SetNumberField(TEXT("value"), Key.Value);
							KeyObject->SetStringField(TEXT("interpolation"), Key.Interpolation);
							return KeyObject;
						}));
					return Item;
				}));
			SetRevision();
			break;
		case EUeAgentTimelineAnimationRequestKind::SequenceMetadataUpdate:
			Root->SetArrayField(TEXT("metadata_changes"), JsonObjects(
				Data.MetadataChanges, [](const FTimelineAnimationMetadataChange& Change)
				{
					const TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("metadata_ref"), Change.MetadataRef);
					Item->SetStringField(TEXT("operation"), Change.Operation);
					Item->SetStringField(TEXT("value_type"), Change.ValueType);
					Item->SetStringField(TEXT("value_lexeme"), Change.ValueLexeme);
					return Item;
				}));
			SetRevision();
			break;
		case EUeAgentTimelineAnimationRequestKind::SequenceEventsUpdate:
			Root->SetArrayField(TEXT("event_changes"), JsonObjects(
				Data.EventChanges, [](const FTimelineAnimationEventChange& Change)
				{
					const TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("event_ref"), Change.EventRef);
					Item->SetStringField(TEXT("event_kind"), Change.EventKind);
					Item->SetStringField(TEXT("operation"), Change.Operation);
					if (Change.Time.IsSet()) Item->SetObjectField(TEXT("time"), JsonPoint(*Change.Time));
					if (Change.Range.IsSet()) Item->SetObjectField(TEXT("range"), JsonRange(*Change.Range));
					if (!Change.PayloadClassRef.IsEmpty()) Item->SetStringField(TEXT("payload_class_ref"), Change.PayloadClassRef);
					if (!Change.TrackRef.IsEmpty()) Item->SetStringField(TEXT("track_ref"), Change.TrackRef);
					return Item;
				}));
			SetRevision();
			break;
		case EUeAgentTimelineAnimationRequestKind::MontageRead:
			if (Data.IncludeSections.IsSet()) Root->SetBoolField(TEXT("include_sections"), *Data.IncludeSections);
			if (Data.IncludeSlots.IsSet()) Root->SetBoolField(TEXT("include_slots"), *Data.IncludeSlots);
			break;
		case EUeAgentTimelineAnimationRequestKind::MontageCreate:
			Root->SetStringField(TEXT("destination_ref"), Data.DestinationRef);
			Root->SetStringField(TEXT("skeleton_ref"), Data.SkeletonRef);
			Root->SetObjectField(TEXT("initial_structure"), JsonStructure(*Data.Structure));
			if (!Data.SourceAnimationRef.IsEmpty()) Root->SetStringField(TEXT("source_animation_ref"), Data.SourceAnimationRef);
			break;
		case EUeAgentTimelineAnimationRequestKind::MontageStructureReplace:
			Root->SetObjectField(TEXT("structure"), JsonStructure(*Data.Structure));
			SetRevision();
			break;
		case EUeAgentTimelineAnimationRequestKind::SkeletonSlotsRead:
			break;
		case EUeAgentTimelineAnimationRequestKind::LevelSequenceRead:
			if (Data.TemporalRange.IsSet()) Root->SetObjectField(TEXT("temporal_range"), JsonRange(*Data.TemporalRange));
			if (Data.IncludeBindings.IsSet()) Root->SetBoolField(TEXT("include_bindings"), *Data.IncludeBindings);
			if (Data.IncludeTracks.IsSet()) Root->SetBoolField(TEXT("include_tracks"), *Data.IncludeTracks);
			if (Data.IncludeKeys.IsSet()) Root->SetBoolField(TEXT("include_keys"), *Data.IncludeKeys);
			break;
		case EUeAgentTimelineAnimationRequestKind::LevelSequenceCreate:
			Root->SetStringField(TEXT("destination_ref"), Data.DestinationRef);
			Root->SetObjectField(TEXT("initial_structure"), JsonStructure(*Data.Structure));
			if (Data.DisplayRate.IsSet()) Root->SetObjectField(TEXT("display_rate"), JsonRate(*Data.DisplayRate));
			if (Data.TickResolution.IsSet()) Root->SetObjectField(TEXT("tick_resolution"), JsonRate(*Data.TickResolution));
			break;
		case EUeAgentTimelineAnimationRequestKind::LevelSequenceSettingsUpdate:
			if (Data.DisplayRate.IsSet())
				Root->SetObjectField(TEXT("display_rate"), JsonRate(*Data.DisplayRate));
			if (Data.TemporalRange.IsSet())
				Root->SetObjectField(TEXT("playback_range"), JsonRange(*Data.TemporalRange));
			SetRevision();
			break;
		case EUeAgentTimelineAnimationRequestKind::LevelSequenceStructureReplace:
			Root->SetObjectField(TEXT("structure"), JsonStructure(*Data.Structure));
			SetRevision();
			break;
		case EUeAgentTimelineAnimationRequestKind::WidgetAnimationRead:
			if (Data.TemporalRange.IsSet()) Root->SetObjectField(TEXT("temporal_range"), JsonRange(*Data.TemporalRange));
			if (Data.IncludeBindings.IsSet()) Root->SetBoolField(TEXT("include_bindings"), *Data.IncludeBindings);
			if (Data.IncludeTracks.IsSet()) Root->SetBoolField(TEXT("include_tracks"), *Data.IncludeTracks);
			break;
		case EUeAgentTimelineAnimationRequestKind::SubjectPoseUpdate:
			Root->SetObjectField(TEXT("evaluation_point"), JsonPoint(*Data.EvaluationPoint));
			Root->SetObjectField(TEXT("pose_frame"), JsonFrame(*Data.PoseFrame));
			Root->SetArrayField(TEXT("pose"), JsonObjects(
				Data.Pose, [](const FTimelineAnimationPoseItem& ItemValue)
				{
					const TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("bone_ref"), ItemValue.BoneRef);
					Item->SetObjectField(TEXT("transform"), JsonTransform(ItemValue.Transform));
					return Item;
				}));
			if (Data.Weight.IsSet()) Root->SetNumberField(TEXT("weight"), *Data.Weight);
			break;
		case EUeAgentTimelineAnimationRequestKind::SubjectMorphWeightsUpdate:
			Root->SetObjectField(TEXT("evaluation_point"), JsonPoint(*Data.EvaluationPoint));
			Root->SetArrayField(TEXT("morph_weights"), JsonObjects(
				Data.MorphWeights, [](const FTimelineAnimationMorphWeight& Weight)
				{
					const TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("morph_ref"), Weight.MorphRef);
					Item->SetNumberField(TEXT("weight"), Weight.Weight);
					return Item;
				}));
			break;
		case EUeAgentTimelineAnimationRequestKind::IkProbeCandidateApply:
			Root->SetStringField(TEXT("rig_ref"), Data.RigRef);
			Root->SetStringField(TEXT("goal_ref"), Data.GoalRef);
			Root->SetObjectField(TEXT("candidate"), JsonTransform(*Data.Candidate));
			Root->SetObjectField(TEXT("authored_time"), JsonPoint(*Data.AuthoredTime));
			SetRevision();
			break;
		default:
			break;
		}
		FString Text;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
		return FUeAgentContractValue(MoveTemp(Text));
	}

	const FUeAgentTimelineAnimationExecutionChainContract* Contract(
		const FString& Capability)
	{
		return GetTimelineAnimationExecutionChainContracts().FindByPredicate(
			[&](const auto& Item){ return Item.GetCapabilityId() == Capability; });
	}

	const FUeAgentOperationDescriptor* Descriptor(
		const FUeAgentTimelineAnimationProductionRuntime& Runtime,
		const FString& Capability)
	{
		return Runtime.GetDescriptorRegistry()->GetDescriptors().FindByPredicate(
			[&](const auto& Item){ return Item.GetCapabilityId() == Capability; });
	}

	TSharedPtr<FUeAgentOperationContext> MakeContext(
		FAutomationTestBase& Test,
		const FUeAgentOperationDescriptor& DescriptorValue,
		EUeAgentRequestKind Kind,
		const FString& RequestId,
		const FUeAgentResourceTrackerOwner& Resources)
	{
		const TArray<EUeAgentStatePlane>& Planes =
			DescriptorValue.GetExecutionContextPolicy().GetAllowedStatePlanes();
		if (!Test.TestTrue(TEXT("descriptor state plane is explicit"), !Planes.IsEmpty()))
			return nullptr;
		FUeAgentExecutionContextInput Input;
		Input.ProjectInstanceId = TEXT("ts010a-project");
		Input.EditorInstanceId = TEXT("ts010a-editor");
		Input.StatePlane = Planes[0];
		Input.WorldType = EUeAgentExecutionWorldType::Preview;
		Input.WorldIdentity = TEXT("ts010a-world");
		Input.AssetEditorIdentity = TEXT("ts010a-asset-editor");
		Input.ViewportIdentity = TEXT("ts010a-viewport");
		Input.ContextRevision = TEXT("ts010a-context-1");
		Input.bResolvedFromCurrent = false;
		FUeAgentExecutionContextRefPtr Execution;
		FUeAgentExecutionContextError ExecutionError;
		if (!Test.TestTrue(TEXT("freeze explicit timeline context"),
				FUeAgentExecutionContextRef::TryCreate(Input,
					DescriptorValue.GetIdentity(), Kind,
					DescriptorValue.GetExecutionContextPolicy(), Execution,
					ExecutionError))) return nullptr;
		const FUeAgentCallerContext Caller(TEXT("ts010a-native-e2"),
			{TEXT("timeline-animation:read"), TEXT("timeline-animation:write")},
			TEXT("corr-") + RequestId);
		const FUeAgentOperationDeadline Deadline =
			FUeAgentOperationDeadline::FromTimeout(30.0, [](){ return 100.0; });
		const FUeAgentCancellationSource Cancellation([](){ return 100.0; });
		TSharedPtr<FUeAgentOperationContext> Result;
		FUeAgentOperationContextError Error;
		if (Kind == EUeAgentRequestKind::Query)
		{
			const FUeAgentQueryRequest Request(DescriptorValue.GetIdentity(),
				RequestId, FUeAgentContractValue(TEXT("{}")));
			Test.TestTrue(TEXT("create query context"),
				FUeAgentOperationContext::TryCreate(Request, Caller,
					Execution.ToSharedRef(), Resources, Deadline,
					Cancellation.GetToken(), Result, Error));
		}
		else
		{
			const FUeAgentOperationRequest Request(DescriptorValue.GetIdentity(),
				RequestId, TEXT("idem-") + RequestId,
				FUeAgentContractValue(TEXT("{}")));
			Test.TestTrue(TEXT("create operation context"),
				FUeAgentOperationContext::TryCreate(Request, Caller,
					Execution.ToSharedRef(), Resources, Deadline,
					Cancellation.GetToken(), Result, Error));
		}
		return Result;
	}

	FUeAgentTimelineAnimationTypedRequestRef MakeRequest(
		FAutomationTestBase& Test,
		const FUeAgentTimelineAnimationExecutionChainContract& ContractValue,
		EUeAgentTimelineAnimationRequestKind Kind,
		FTimelineAnimationRequestData Data,
		const FUeAgentResourceTrackerOwner& Resources)
	{
		FUeAgentCanonicalValuePtr Intent;
		FUeAgentStateSemanticError Error;
		Test.TestTrue(TEXT("create typed intent payload"),
			FUeAgentCanonicalValue::TryObject({}, *Resources, Intent, Error));
		return MakeShared<FUeAgentTimelineAnimationRequest>(
			ContractValue.GetTypedRequestId(), ContractValue.GetFamilyId(),
			TEXT("ts010a_") + ContractValue.GetCapabilityId(), Kind,
			ContractValue.RequiresNativeMutation(), MoveTemp(Data),
			Intent.ToSharedRef());
	}

	TOptional<FUeAgentCandidateBinding> MakeCandidate(
		const FUeAgentOperationDescriptor& DescriptorValue,
		const FString& Capability, const FString& PublicRef)
	{
		if (PublicRef.IsEmpty()) return {};
		return FUeAgentCandidateBinding(
			TEXT("ts010a-binding:") + Capability,
			FUeAgentCandidateSummary(
				FUeAgentCanonicalIdentity(TEXT("ts010a-target:") + Capability,
					TEXT("timeline_animation_target"), TEXT("ts010a-owner"),
					PublicRef, TEXT("target"), TEXT("ts010a-generation")),
				DescriptorValue.GetIdentity(), TEXT("timeline_animation_target"),
				PublicRef, TEXT("owned TimelineAnimation fixture"),
				{TEXT("TimelineAnimation"), TEXT("Fixture")}),
			FUeAgentCatalogRevision(TEXT("ts010a-catalog"),
				TEXT("ts010a-generation"), 1), PublicRef,
			TEXT("timeline-animation-native"),
			{EUeAgentCandidateOperation::Read, EUeAgentCandidateOperation::Create,
				EUeAgentCandidateOperation::Update, EUeAgentCandidateOperation::Replace},
			TEXT("ts010a-native-binding"));
	}

	TArray<FUeAgentOperationTargetBinding> MakeOperationTargets(
		FAutomationTestBase& Test,
		const FUeAgentOperationDescriptor& DescriptorValue,
		const FString& Capability, const FString& PublicRef)
	{
		TArray<FUeAgentOperationTargetBinding> Result;
		const TOptional<FUeAgentCandidateBinding> Candidate =
			MakeCandidate(DescriptorValue, Capability, PublicRef);
		if (!Candidate.IsSet()) return Result;
		TSharedPtr<const FUeAgentOperationTicketService> Service;
		FUeAgentOperationTicketError ServiceError;
		if (!Test.TestTrue(TEXT("create target ticket service"),
				FUeAgentOperationTicketService::TryCreate(TEXT("ts010a-integrity"),
					60.0, [](){ return 100.0; }, [](){ return TEXT("ts010a-ticket"); },
					Service, ServiceError))) return Result;
		const FUeAgentOperationSessionIdentity Session(
			TEXT("ts010a-session"), 1,
			TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"));
		const FUeAgentCallerContext Caller(TEXT("ts010a-native-e2"),
			{TEXT("timeline-animation:read"), TEXT("timeline-animation:write")},
			TEXT("corr-ticket-") + Capability);
		TOptional<FUeAgentOperationTicket> Ticket;
		if (!Test.TestTrue(TEXT("issue explicit target ticket"),
				Service->TryIssue(*Candidate, DescriptorValue.GetIdentity(), Session,
					Caller, {}, Ticket, ServiceError)) || !Ticket.IsSet()) return Result;
		Result.Emplace(Ticket.GetValue(), Candidate.GetValue());
		return Result;
	}

	FTimelineAnimationRate Rate(int32 Numerator = 30, int32 Denominator = 1)
	{
		return {FString::FromInt(Numerator), FString::FromInt(Denominator)};
	}

	FTimelineAnimationPoint Point(const FString& Clock, int32 Tick,
		int32 Numerator = 30, int32 Denominator = 1)
	{
		return {Clock, FString::FromInt(Tick), Rate(Numerator, Denominator),
			TEXT("authored")};
	}

	FTimelineAnimationRange Range(const FString& Clock, int32 Lower, int32 Upper,
		int32 Numerator = 30, int32 Denominator = 1)
	{
		FTimelineAnimationRange Result;
		Result.ClockId = Clock;
		Result.State = TEXT("authored");
		Result.Lower.Kind = TEXT("inclusive");
		Result.Lower.Point = Point(Clock, Lower, Numerator, Denominator);
		Result.Upper.Kind = TEXT("exclusive");
		Result.Upper.Point = Point(Clock, Upper, Numerator, Denominator);
		return Result;
	}

	FTimelineAnimationFrame3d Frame(const FString& FrameId)
	{
		return {FrameId, FrameId, 3, TEXT("centimeter"),
			TEXT("ts010a-explicit-frame")};
	}

	FTimelineAnimationTransform3d Transform(const FString& Subject,
		const FString& FrameId, double X)
	{
		FTimelineAnimationTransform3d Result;
		Result.Subject = Subject;
		Result.Quantity = TEXT("transform");
		Result.Frame = Frame(FrameId);
		Result.Components = {FString::SanitizeFloat(X), TEXT("0"), TEXT("0"),
			TEXT("0"), TEXT("0"), TEXT("0"), TEXT("1"),
			TEXT("1"), TEXT("1"), TEXT("1")};
		Result.ConversionEvidenceIds = {TEXT("ts010a-identity-frame")};
		return Result;
	}

	FTimelineAnimationStructure Structure(const FString& Clock, int32 End)
	{
		FTimelineAnimationStructure Result;
		Result.DisplayRate = Rate();
		Result.TickResolution = Rate();
		Result.PlaybackRange = Range(Clock, 0, End);
		return Result;
	}

	bool CreateFixture(FAutomationTestBase& Test, FFixture& Out)
	{
		Out.Mesh = LoadObject<USkeletalMesh>(nullptr,
			TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
		if (!Test.TestNotNull(TEXT("load real skeletal mesh"), Out.Mesh)) return false;
		Out.Skeleton = Out.Mesh->GetSkeleton();
		if (!Test.TestNotNull(TEXT("real skeleton exists"), Out.Skeleton)) return false;
		Out.RootBone = Out.Skeleton->GetReferenceSkeleton().GetBoneName(0);

		const FString SequencePackageName = Out.AddPackage(TEXT("Sequence"));
		UPackage* SequencePackage = CreatePackage(*SequencePackageName);
		Out.Sequence = NewObject<UAnimSequence>(SequencePackage, TEXT("Sequence"),
			RF_Public | RF_Standalone | RF_Transactional);
		Out.SequenceRef = Out.ObjectPath(SequencePackageName);
		Out.Sequence->SetSkeleton(Out.Skeleton);
		IAnimationDataController& Controller = Out.Sequence->GetController();
		Controller.InitializeModel();
		Controller.SetFrameRate(FFrameRate(30, 1), false);
		Controller.SetNumberOfFrames(FFrameNumber(1), false);
		Controller.AddBoneCurve(Out.RootBone, false);
		Controller.SetBoneTrackKeys(Out.RootBone,
			{FVector::ZeroVector, FVector(1, 0, 0)},
			{FQuat::Identity, FQuat::Identity},
			{FVector::OneVector, FVector::OneVector}, false);
		Controller.NotifyPopulated();
		if (!Test.TestTrue(TEXT("save sequence fixture"), Out.Save(*Out.Sequence)))
		{
			return false;
		}

		const FString BlendPackageName = Out.AddPackage(TEXT("BlendSpace"));
		UPackage* BlendPackage = CreatePackage(*BlendPackageName);
		Out.BlendSpace = NewObject<UBlendSpace>(BlendPackage, TEXT("BlendSpace"),
			RF_Public | RF_Standalone | RF_Transactional);
		Out.BlendSpaceRef = Out.ObjectPath(BlendPackageName);
		Out.BlendSpace->SetSkeleton(Out.Skeleton);
		Out.BlendSpace->AddSample(Out.Sequence, FVector::ZeroVector);
		Out.BlendSpace->ValidateSampleData();
		Out.BlendSpace->ResampleData();
		if (!Test.TestTrue(TEXT("save BlendSpace fixture"),
				Out.Save(*Out.BlendSpace)))
		{
			return false;
		}

		const FString MontagePackageName = Out.AddPackage(TEXT("Montage"));
		UPackage* MontagePackage = CreatePackage(*MontagePackageName);
		Out.Montage = NewObject<UAnimMontage>(MontagePackage, TEXT("Montage"),
			RF_Public | RF_Standalone | RF_Transactional);
		Out.MontageRef = Out.ObjectPath(MontagePackageName);
		Out.Montage->SetSkeleton(Out.Skeleton);
		Out.Montage->SetCompositeLength(1.0f);
		Out.Montage->AddAnimCompositeSection(TEXT("Initial"), 0.0f);
		if (!Test.TestTrue(TEXT("save Montage fixture"), Out.Save(*Out.Montage)))
		{
			return false;
		}

		const FString LevelPackageName = Out.AddPackage(TEXT("Level"));
		UPackage* LevelPackage = CreatePackage(*LevelPackageName);
		Out.LevelSequence = NewObject<ULevelSequence>(LevelPackage, TEXT("Level"),
			RF_Public | RF_Standalone | RF_Transactional);
		Out.LevelSequenceRef = Out.ObjectPath(LevelPackageName);
		Out.LevelSequence->Initialize();
		Out.LevelSequence->GetMovieScene()->SetDisplayRate(FFrameRate(30, 1));
		Out.LevelSequence->GetMovieScene()->SetTickResolutionDirectly(FFrameRate(30, 1));
		Out.LevelSequence->GetMovieScene()->SetPlaybackRange(0, 30);
		UMovieSceneFloatTrack* LevelTrack =
			Out.LevelSequence->GetMovieScene()->AddTrack<UMovieSceneFloatTrack>();
		if (!Test.TestNotNull(
			TEXT("create LevelSequence preservation track"), LevelTrack))
		{
			return false;
		}
		LevelTrack->SetDisplayName(
			FText::FromString(TEXT("TS010A_PreservedTrack")));
		UMovieSceneFloatSection* LevelSection = Cast<UMovieSceneFloatSection>(
			LevelTrack->CreateNewSection());
		if (!Test.TestNotNull(
			TEXT("create LevelSequence preservation section"), LevelSection))
		{
			return false;
		}
		LevelSection->SetRange(TRange<FFrameNumber>(
			FFrameNumber(0), FFrameNumber(30)));
		LevelTrack->AddSection(*LevelSection);
		FMovieSceneFloatChannel* LevelChannel = LevelSection->
			GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(0);
		if (!Test.TestNotNull(
			TEXT("create LevelSequence preservation channel"), LevelChannel))
		{
			return false;
		}
		LevelChannel->AddLinearKey(FFrameNumber(10), 0.5f);
		if (!Test.TestTrue(TEXT("save LevelSequence fixture"),
				Out.Save(*Out.LevelSequence)))
		{
			return false;
		}

		const FString WidgetPackageName = Out.AddPackage(TEXT("Widget"));
		UPackage* WidgetPackage = CreatePackage(*WidgetPackageName);
		Out.WidgetAnimation = NewObject<UWidgetAnimation>(WidgetPackage,
			TEXT("Widget"), RF_Public | RF_Standalone | RF_Transactional);
		Out.WidgetAnimationRef = Out.ObjectPath(WidgetPackageName);
		Out.WidgetAnimation->MovieScene = NewObject<UMovieScene>(Out.WidgetAnimation,
			TEXT("MovieScene"), RF_Transactional);
		Out.WidgetAnimation->MovieScene->SetDisplayRate(FFrameRate(30, 1));
		Out.WidgetAnimation->MovieScene->SetTickResolutionDirectly(FFrameRate(30, 1));
		if (!Test.TestTrue(TEXT("save WidgetAnimation fixture"),
				Out.Save(*Out.WidgetAnimation)))
		{
			return false;
		}

		Out.PreviewScene = MakeUnique<FPreviewScene>(FPreviewScene::ConstructionValues()
			.SetCreateDefaultLighting(false)
			.SetCreatePhysicsScene(false)
			.SetForceMipsResident(false)
			.ShouldSimulatePhysics(false)
			.SetTransactional(false));
		if (!Test.TestNotNull(TEXT("create owned preview world"),
				Out.PreviewScene->GetWorld())) return false;
		Out.PoseComponent = NewObject<UPoseableMeshComponent>(GetTransientPackage(),
			TEXT("TS010A_Pose"));
		Out.PreviewScene->AddComponent(Out.PoseComponent, FTransform::Identity);
		Out.PoseComponent->SetSkinnedAssetAndUpdate(Out.Mesh, true);
		Out.PoseComponent->RefreshBoneTransforms();
		Out.MorphComponent = NewObject<USkeletalMeshComponent>(GetTransientPackage(),
			TEXT("TS010A_Morph"));
		Out.PreviewScene->AddComponent(Out.MorphComponent, FTransform::Identity);
		Out.MorphComponent->SetSkeletalMeshAsset(Out.Mesh);

		const FString RigSequencePackageName = Out.AddPackage(TEXT("RigSequence"));
		Out.RigCandidateRef = Out.ObjectPath(RigSequencePackageName);
		UPackage* RigSequencePackage = CreatePackage(*RigSequencePackageName);
		Out.RigSequence = NewObject<ULevelSequence>(RigSequencePackage,
			TEXT("RigSequence"), RF_Public | RF_Standalone | RF_Transactional);
		Out.RigSequence->Initialize();
		Out.RigSequence->GetMovieScene()->SetDisplayRate(FFrameRate(30, 1));
		Out.RigSequence->GetMovieScene()->SetTickResolutionDirectly(FFrameRate(30, 1));
		Out.RigSequence->GetMovieScene()->SetPlaybackRange(0, 30);
		Out.Rig = NewObject<UAdditiveControlRig>(GetTransientPackage(), TEXT("TS010A_Rig"));
		Out.Rig->Initialize(true);
		FRigControlSettings Settings;
		Settings.ControlType = ERigControlType::Transform;
		Out.Rig->GetHierarchy()->GetController(true)->AddControl(Out.ControlName,
			FRigElementKey(), Settings, FRigControlValue::Make(FTransform::Identity),
			FTransform::Identity, FTransform::Identity, false);
		UMovieSceneControlRigParameterTrack* RigTrack =
			Out.RigSequence->GetMovieScene()->AddTrack<UMovieSceneControlRigParameterTrack>();
		if (!Test.TestNotNull(TEXT("create real ControlRig sequence track"), RigTrack)) return false;
		if (!Test.TestNotNull(TEXT("create real ControlRig sequence section"),
				RigTrack->CreateControlRigSection(0, Out.Rig, false)))
		{
			return false;
		}
		Out.RigRef = Out.Rig->GetPathName();
		if (!Test.TestTrue(TEXT("save ControlRig sequence fixture"),
				Out.Save(*Out.RigSequence)))
		{
			return false;
		}
		return true;
	}

	bool ExecuteQuery(FAutomationTestBase& Test,
		const FUeAgentTimelineAnimationProductionRuntime& Runtime,
		const FString& Capability,
		EUeAgentTimelineAnimationRequestKind Kind,
		FTimelineAnimationRequestData Data,
		const FString& Ref,
		const FUeAgentResourceTrackerOwner& Resources)
	{
		const auto* ContractValue = Contract(Capability);
		const auto* DescriptorValue = Descriptor(Runtime, Capability);
		if (!Test.TestNotNull(TEXT("query contract"), ContractValue)
			|| !Test.TestNotNull(TEXT("query descriptor"), DescriptorValue)) return false;
		const auto Context = MakeContext(Test, *DescriptorValue,
			EUeAgentRequestKind::Query, TEXT("query-") + Capability, Resources);
		if (!Context.IsValid()) return false;
		const TOptional<FUeAgentQueryDescriptor> QueryDescriptor =
			Runtime.GetDescriptorRegistry()->FindQueryExact(DescriptorValue->GetIdentity());
		if (!Test.TestTrue(TEXT("exact query descriptor: ") + Capability,
				QueryDescriptor.IsSet())) return false;
		const TSharedPtr<IUeAgentDomainQueryProvider> Provider =
			Runtime.GetQueryProviderRegistry()->FindExact(ContractValue->GetBindingId());
		if (!Test.TestTrue(TEXT("exact production C3Q provider: ") + Capability,
				Provider.IsValid())) return false;
		TSharedPtr<const FUeAgentProjectionSourceValue> Source;
		FUeAgentQueryBindingError Error;
		const bool bRead = Provider->TryQuery(*QueryDescriptor,
			CanonicalInput(Kind, Data), MakeCandidate(*DescriptorValue, Capability, Ref),
			*Context, Source, Error);
		if (!Test.TestTrue(TEXT("execute exact production C3Q: ") + Capability, bRead))
			Test.AddError(Capability + TEXT(":") + Error.Code + TEXT(":") + Error.Detail);
		return bRead && Test.TestTrue(TEXT("C3Q returns independent snapshot"),
			Source.IsValid() && Source->IsValid() && Source->GetSnapshot().IsValid()
			&& !Source->GetSnapshot()->GetEntries().IsEmpty());
	}

	bool ExecuteOperation(FAutomationTestBase& Test,
		const FUeAgentTimelineAnimationProductionRuntime& Runtime,
		const FString& Capability,
		EUeAgentTimelineAnimationRequestKind Kind,
		FTimelineAnimationRequestData Data,
		const FString& Ref,
		const FUeAgentResourceTrackerOwner& Resources)
	{
		const auto* ContractValue = Contract(Capability);
		const auto* DescriptorValue = Descriptor(Runtime, Capability);
		if (!Test.TestNotNull(TEXT("operation contract"), ContractValue)
			|| !Test.TestNotNull(TEXT("operation descriptor"), DescriptorValue)) return false;
		const auto Context = MakeContext(Test, *DescriptorValue,
			EUeAgentRequestKind::Operation, TEXT("operation-") + Capability, Resources);
		if (!Context.IsValid()) return false;
		const auto Targets = MakeOperationTargets(Test, *DescriptorValue, Capability, Ref);
		const TSharedRef<FUeAgentTimelineAnimationOperationChain>* Chain =
			Runtime.GetOperationChains().FindByPredicate(
				[&](const TSharedRef<FUeAgentTimelineAnimationOperationChain>& Candidate)
				{
					return Candidate->GetContract().GetCapabilityId() == Capability;
				});
		if (!Test.TestNotNull(TEXT("exact production C3O chain: ") + Capability, Chain))
			return false;
		FUeAgentOperationBindingError Error;
		TOptional<FPlanningRequest> PlanningRequest;
		if (!Test.TestTrue(TEXT("C3O decode and preflight: ") + Capability,
				(*Chain)->TryBuildPlanningRequest(CanonicalInput(Kind, Data), Targets,
					*Context, PlanningRequest, Error)) || !PlanningRequest.IsSet())
		{
			Test.AddError(Capability + TEXT(":preflight:") + Error.Code + TEXT(":") + Error.Detail);
			return false;
		}
		FPlanningResult Planning;
		if (!Test.TestTrue(TEXT("C3O plan: ") + Capability,
				(*Chain)->GetPlanner().TryPlan(*PlanningRequest, *Context, Planning, Error)))
		{
			Test.AddError(Capability + TEXT(":plan:") + Error.Code + TEXT(":") + Error.Detail);
			return false;
		}
		FExecutionReceipt Receipt;
		if (!Test.TestTrue(TEXT("C3O mutate once: ") + Capability,
				(*Chain)->GetExecutor().TryExecute(Planning, Targets, *Context, Receipt, Error)))
		{
			Test.AddError(Capability + TEXT(":execute:") + Error.Code + TEXT(":") + Error.Detail);
			return false;
		}
		FVerificationOutcome Outcome;
		if (!Test.TestTrue(TEXT("C3O settle, readback and compare: ") + Capability,
				(*Chain)->GetVerifier().TryVerify(Receipt, Targets, *Context, Outcome, Error)))
		{
			Test.AddError(Capability + TEXT(":verify:") + Error.Code + TEXT(":") + Error.Detail);
			return false;
		}
		return Test.TestEqual(TEXT("exact production C3O verified: ") + Capability,
			Outcome.GetStatus(), EVerificationStatus::Verified);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentTimelineAnimationProductionNativeCapabilitiesTest,
	"UeAgentInterface.DomainAdapters.TimelineAnimation.E2.ProductionNative.All20Capabilities",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentTimelineAnimationProductionNativeCapabilitiesTest::RunTest(
	const FString& Parameters)
{
	FFixture Fixture;
	if (!CreateFixture(*this, Fixture)) return false;
	FUeAgentTimelineAnimationProductionRuntimeConfig Config;
	Config.ProjectInstanceId = TEXT("ts010a-project");
	Config.EditorInstanceId = TEXT("ts010a-editor");
	Config.ServerSessionId = TEXT("ts010a-session");
	Config.RegistryEpoch = 1;
	FUeAgentTimelineAnimationSubjectBinding Subject;
	Subject.Identity = Fixture.SubjectIdentity;
	Subject.PoseComponent = Fixture.PoseComponent;
	Subject.MorphComponent = Fixture.MorphComponent;
	Subject.NativeFrameId = TEXT("component:ts010a-subject");
	Subject.EvaluationClockId = TEXT("subject-clock");
	Subject.EvaluationRateNumerator = TEXT("30");
	Subject.EvaluationRateDenominator = TEXT("1");
	Config.SubjectBindings.Add(Subject);
	FUeAgentTimelineAnimationRigCandidateBinding Rig;
	Rig.Identity = Fixture.RigCandidateRef;
	Rig.Sequence = Fixture.RigSequence;
	Rig.Rig = Fixture.Rig;
	Rig.RigIdentity = Fixture.RigRef;
	Rig.ControlName = Fixture.ControlName;
	Rig.NativeFrameId = TEXT("control:ts010a-goal");
	Rig.AuthoredClockId = TEXT("rig-clock");
	Rig.AuthoredRateNumerator = TEXT("30");
	Rig.AuthoredRateDenominator = TEXT("1");
	Config.RigCandidateBindings.Add(Rig);
	TSharedPtr<FUeAgentTimelineAnimationProductionRuntime> Runtime;
	FUeAgentTimelineAnimationExecutionError Error;
	if (!TestTrue(TEXT("create explicitly bound production runtime"),
			FUeAgentTimelineAnimationProductionRuntime::TryCreate(
				Config, Runtime, Error)) || !Runtime.IsValid()) return false;
	const auto Resources = MakeResources();
	const FString SequenceRef = Fixture.SequenceRef;
	const FString SequenceClock = TEXT("animation_sequence:") + SequenceRef;
	const FString BlendSpaceRef = Fixture.BlendSpaceRef;
	const FString MontageRef = Fixture.MontageRef;
	const FString SkeletonRef = Fixture.Skeleton->GetPathName();
	const FString LevelRef = Fixture.LevelSequenceRef;
	const FString WidgetAnimationRef = Fixture.WidgetAnimationRef;
	const FString RigRef = Fixture.RigRef;

	FTimelineAnimationRequestData Data;
	Data.IncludeTracks = true;
	ExecuteQuery(*this, *Runtime, TEXT("animation.sequence.read"),
		EUeAgentTimelineAnimationRequestKind::SequenceRead, Data, SequenceRef, Resources);
	Data = {};
	Data.TemporalRange = Range(SequenceClock, 0, 1);
	Data.SampleRate = Rate();
	ExecuteQuery(*this, *Runtime,
		TEXT("animation.sequence.root_motion.analyze"),
		EUeAgentTimelineAnimationRequestKind::SequenceRootMotionAnalyze,
		Data, SequenceRef, Resources);
	Data = {};
	Data.TimePoint = Point(SequenceClock, 0);
	Data.BoneRefs = {Fixture.RootBone.ToString()};
	ExecuteQuery(*this, *Runtime,
		TEXT("animation.sequence.pose.sample"),
		EUeAgentTimelineAnimationRequestKind::SequencePoseSample,
		Data, SequenceRef, Resources);
	Data = {};
	Data.BlendParameters = {{TEXT("x"), 0.0}};
	ExecuteQuery(*this, *Runtime,
		TEXT("animation.blend_space.sample"),
		EUeAgentTimelineAnimationRequestKind::BlendSpaceSample,
		Data, BlendSpaceRef, Resources);
	ExecuteQuery(*this, *Runtime, TEXT("animation.montage.read"),
		EUeAgentTimelineAnimationRequestKind::MontageRead, {},
		MontageRef, Resources);
	ExecuteQuery(*this, *Runtime,
		TEXT("animation.skeleton.slots.read"),
		EUeAgentTimelineAnimationRequestKind::SkeletonSlotsRead, {},
		SkeletonRef, Resources);
	ExecuteQuery(*this, *Runtime, TEXT("timeline.sequence.read"),
		EUeAgentTimelineAnimationRequestKind::LevelSequenceRead, {}, LevelRef, Resources);
	ExecuteQuery(*this, *Runtime,
		TEXT("timeline.widget_animation.read"),
		EUeAgentTimelineAnimationRequestKind::WidgetAnimationRead, {},
		WidgetAnimationRef, Resources);

	FTimelineAnimationRevision Revision;
	Runtime->TryReadRevision(SequenceRef, Revision, Error);
	Data = {};
	Data.TemporalRange = Range(SequenceClock, 0, 3);
	Data.SampleRate = Rate();
	Data.ExpectedRevision = Revision;
	FTimelineAnimationBoneTrack BoneTrack;
	BoneTrack.BoneRef = Fixture.RootBone.ToString();
	const FString BoneFrame = TEXT("animation_sequence:") + SequenceRef
		+ TEXT(":bone:") + BoneTrack.BoneRef;
	BoneTrack.Keys = {{Point(SequenceClock, 0), Transform(BoneTrack.BoneRef, BoneFrame, 0.0)},
		{Point(SequenceClock, 1), Transform(BoneTrack.BoneRef, BoneFrame, 10.0)},
		{Point(SequenceClock, 2), Transform(BoneTrack.BoneRef, BoneFrame, 20.0)}};
	Data.BoneTracks = {BoneTrack};
	ExecuteOperation(*this, *Runtime,
		TEXT("animation.sequence.bone_tracks.replace"),
		EUeAgentTimelineAnimationRequestKind::SequenceBoneTracksReplace,
		Data, SequenceRef, Resources);

	Runtime->TryReadRevision(SequenceRef, Revision, Error);
	Data = {};
	Data.ExpectedRevision = Revision;
	FTimelineAnimationCurveChange Curve;
	Curve.CurveRef = TEXT("TS010A_Curve");
	Curve.Operation = TEXT("upsert");
	Curve.Keys = {{Point(SequenceClock, 0), 0.25, TEXT("linear")},
		{Point(SequenceClock, 2), 0.75, TEXT("linear")}};
	Data.CurveChanges = {Curve};
	ExecuteOperation(*this, *Runtime,
		TEXT("animation.sequence.curves.update"),
		EUeAgentTimelineAnimationRequestKind::SequenceCurvesUpdate,
		Data, SequenceRef, Resources);

	Runtime->TryReadRevision(SequenceRef, Revision, Error);
	Data = {};
	Data.ExpectedRevision = Revision;
	Data.MetadataChanges = {{TEXT("TS010A_Key"), TEXT("upsert"),
		TEXT("string"), TEXT("real-value")}};
	ExecuteOperation(*this, *Runtime,
		TEXT("animation.sequence.metadata.update"),
		EUeAgentTimelineAnimationRequestKind::SequenceMetadataUpdate,
		Data, SequenceRef, Resources);

	Runtime->TryReadRevision(SequenceRef, Revision, Error);
	Data = {};
	Data.ExpectedRevision = Revision;
	FTimelineAnimationEventChange Event;
	Event.EventRef = TEXT("TS010A_Marker");
	Event.EventKind = TEXT("sync_marker");
	Event.Operation = TEXT("upsert");
	Event.Time = Point(SequenceClock, 1);
	Data.EventChanges = {Event};
	ExecuteOperation(*this, *Runtime,
		TEXT("animation.sequence.events.update"),
		EUeAgentTimelineAnimationRequestKind::SequenceEventsUpdate,
		Data, SequenceRef, Resources);

	const FString CreatedMontagePackage = Fixture.AddPackage(
		TEXT("CreatedMontage"));
	const FString CreatedMontageRef = Fixture.ObjectPath(CreatedMontagePackage);
	Data = {};
	Data.DestinationRef = CreatedMontageRef;
	Data.SkeletonRef = SkeletonRef;
	Data.Structure = Structure(TEXT("animation_montage:")
		+ CreatedMontageRef, 60);
	ExecuteOperation(*this, *Runtime, TEXT("animation.montage.create"),
		EUeAgentTimelineAnimationRequestKind::MontageCreate, Data, FString(), Resources);

	Runtime->TryReadRevision(MontageRef, Revision, Error);
	Data = {};
	Data.ExpectedRevision = Revision;
	Data.Structure = Structure(TEXT("animation_montage:")
		+ MontageRef, 90);
	ExecuteOperation(*this, *Runtime,
		TEXT("animation.montage.structure.replace"),
		EUeAgentTimelineAnimationRequestKind::MontageStructureReplace,
		Data, MontageRef, Resources);

	const FString CreatedLevelPackage = Fixture.AddPackage(TEXT("CreatedLevel"));
	const FString CreatedLevelRef = Fixture.ObjectPath(CreatedLevelPackage);
	Data = {};
	Data.DestinationRef = CreatedLevelRef;
	Data.Structure = Structure(TEXT("level_sequence:") + CreatedLevelRef, 60);
	ExecuteOperation(*this, *Runtime, TEXT("timeline.sequence.create"),
		EUeAgentTimelineAnimationRequestKind::LevelSequenceCreate,
		Data, FString(), Resources);
	ULevelSequence* CreatedLevel = FindObject<ULevelSequence>(nullptr,
		*CreatedLevelRef);
	TestNotNull(TEXT("level sequence create produced the requested asset"), CreatedLevel);
	if (CreatedLevel)
	{
		TestTrue(TEXT("level sequence create settled compiled data"),
			UMovieSceneCompiledDataManager::GetPrecompiledData()
				->FindDataID(CreatedLevel).IsValid());
	}

	Runtime->TryReadRevision(LevelRef, Revision, Error);
	ULevelSequence* const BeforeSettingsLevel = LoadObject<ULevelSequence>(
		nullptr, *LevelRef);
	if (!TestNotNull(
			TEXT("reload live LevelSequence before settings update"),
			BeforeSettingsLevel))
	{
		return false;
	}
	const UMovieScene* BeforeSettingsScene =
		BeforeSettingsLevel->GetMovieScene();
	const UMovieSceneTrack* BeforeSettingsTrack =
		BeforeSettingsScene && !BeforeSettingsScene->GetTracks().IsEmpty()
			? BeforeSettingsScene->GetTracks()[0]
			: nullptr;
	const UMovieSceneSection* BeforeSettingsSection =
		BeforeSettingsTrack && !BeforeSettingsTrack->GetAllSections().IsEmpty()
			? BeforeSettingsTrack->GetAllSections()[0]
			: nullptr;
	const FString PreservedTrackPath = BeforeSettingsTrack
		? BeforeSettingsTrack->GetPathName()
		: FString();
	const FString PreservedSectionPath = BeforeSettingsSection
		? BeforeSettingsSection->GetPathName()
		: FString();
	Data = {};
	Data.ExpectedRevision = Revision;
	Data.DisplayRate = FTimelineAnimationRate{TEXT("24"), TEXT("1")};
	Data.TemporalRange = Range(TEXT("level_sequence:") + LevelRef, 5, 45);
	ExecuteOperation(*this, *Runtime,
		TEXT("timeline.sequence.settings.update"),
		EUeAgentTimelineAnimationRequestKind::LevelSequenceSettingsUpdate,
		Data, LevelRef, Resources);
	ULevelSequence* SettingsReloadedLevel = LoadObject<ULevelSequence>(
		nullptr, *LevelRef);
	UMovieScene* SettingsReloadedScene = SettingsReloadedLevel
		? SettingsReloadedLevel->GetMovieScene()
		: nullptr;
	TestNotNull(
		TEXT("level sequence settings update reloads its target"),
		SettingsReloadedScene);
	if (SettingsReloadedScene)
	{
		TestEqual(
			TEXT("settings update changes display rate"),
			SettingsReloadedScene->GetDisplayRate(),
			FFrameRate(24, 1));
		TestEqual(
			TEXT("settings update preserves tick resolution"),
			SettingsReloadedScene->GetTickResolution(),
			FFrameRate(30, 1));
		TestEqual(
			TEXT("settings update changes playback lower tick"),
			SettingsReloadedScene->GetPlaybackRange().GetLowerBoundValue().Value,
			5);
		TestEqual(
			TEXT("settings update changes playback upper tick"),
			SettingsReloadedScene->GetPlaybackRange().GetUpperBoundValue().Value,
			45);
		TestEqual(
			TEXT("settings update preserves root track count"),
			SettingsReloadedScene->GetTracks().Num(),
			1);
		const UMovieSceneTrack* PreservedTrack =
			SettingsReloadedScene->GetTracks().Num() == 1
				? SettingsReloadedScene->GetTracks()[0]
				: nullptr;
		TestEqual(
			TEXT("settings update preserves root track identity"),
			PreservedTrack ? PreservedTrack->GetPathName() : FString(),
			PreservedTrackPath);
		const UMovieSceneSection* PreservedSection =
			PreservedTrack && PreservedTrack->GetAllSections().Num() == 1
				? PreservedTrack->GetAllSections()[0]
				: nullptr;
		TestEqual(
			TEXT("settings update preserves section identity"),
			PreservedSection ? PreservedSection->GetPathName() : FString(),
			PreservedSectionPath);
		const UMovieSceneFloatSection* PreservedFloatSection =
			Cast<UMovieSceneFloatSection>(PreservedSection);
		const FMovieSceneFloatChannel* PreservedChannel =
			PreservedFloatSection
				? PreservedFloatSection->GetChannelProxy()
					.GetChannel<FMovieSceneFloatChannel>(0)
				: nullptr;
		TestEqual(
			TEXT("settings update preserves key count"),
			PreservedChannel
				? PreservedChannel->GetData().GetTimes().Num()
				: -1,
			1);
		if (PreservedChannel
			&& PreservedChannel->GetData().GetTimes().Num() == 1
			&& PreservedChannel->GetData().GetValues().Num() == 1)
		{
			TestEqual(
				TEXT("settings update preserves key time"),
				PreservedChannel->GetData().GetTimes()[0].Value,
				10);
			TestEqual(
				TEXT("settings update preserves key value"),
				PreservedChannel->GetData().GetValues()[0].Value,
				0.5f);
		}
		TestEqual(
			TEXT("settings update preserves section lower tick"),
			PreservedSection
				? PreservedSection->GetRange().GetLowerBoundValue().Value
				: -1,
			0);
		TestEqual(
			TEXT("settings update preserves section upper tick"),
			PreservedSection
				? PreservedSection->GetRange().GetUpperBoundValue().Value
				: -1,
			30);
		TestTrue(
			TEXT("settings update settles compiled data after formal reload"),
			UMovieSceneCompiledDataManager::GetPrecompiledData()
				->FindDataID(SettingsReloadedLevel).IsValid());
	}

	Runtime->TryReadRevision(LevelRef, Revision, Error);
	Data = {};
	Data.ExpectedRevision = Revision;
	Data.Structure = Structure(TEXT("level_sequence:") + LevelRef, 90);
	ExecuteOperation(*this, *Runtime,
		TEXT("timeline.sequence.structure.replace"),
		EUeAgentTimelineAnimationRequestKind::LevelSequenceStructureReplace,
		Data, LevelRef, Resources);
	ULevelSequence* const ReloadedLevel = LoadObject<ULevelSequence>(
		nullptr, *LevelRef);
	TestNotNull(TEXT("level sequence replace reloads its target"), ReloadedLevel);
	TestTrue(TEXT("level sequence replace settled compiled data"),
		ReloadedLevel
			&& UMovieSceneCompiledDataManager::GetPrecompiledData()
				->FindDataID(ReloadedLevel).IsValid());

	Data = {};
	Data.EvaluationPoint = Point(TEXT("subject-clock"), 0);
	Data.PoseFrame = Frame(TEXT("component:ts010a-subject"));
	Data.Weight = 1.0;
	Data.Pose = {{Fixture.RootBone.ToString(),
		Transform(Fixture.RootBone.ToString(), TEXT("component:ts010a-subject"), 15.0)}};
	ExecuteOperation(*this, *Runtime,
		TEXT("animation.subject.pose.update"),
		EUeAgentTimelineAnimationRequestKind::SubjectPoseUpdate,
		Data, Fixture.SubjectIdentity, Resources);

	Data = {};
	Data.EvaluationPoint = Point(TEXT("subject-clock"), 1);
	Data.MorphWeights = {{Fixture.MorphName.ToString(), 0.65}};
	ExecuteOperation(*this, *Runtime,
		TEXT("animation.subject.morph_weights.update"),
		EUeAgentTimelineAnimationRequestKind::SubjectMorphWeightsUpdate,
		Data, Fixture.SubjectIdentity, Resources);

	Runtime->TryReadRevision(Fixture.RigCandidateRef, Revision, Error);
	Data = {};
	Data.RigRef = RigRef;
	Data.GoalRef = Fixture.ControlName.ToString();
	Data.Candidate = Transform(Data.GoalRef, TEXT("control:ts010a-goal"), 25.0);
	Data.AuthoredTime = Point(TEXT("rig-clock"), 5);
	Data.ExpectedRevision = Revision;
	ExecuteOperation(*this, *Runtime,
		TEXT("animation.ik_probe_candidate.apply"),
		EUeAgentTimelineAnimationRequestKind::IkProbeCandidateApply,
		Data, Fixture.RigCandidateRef, Resources);

	Runtime.Reset();
	TestTrue(TEXT("all owned TimelineAnimation fixtures clean up"),
		Fixture.Cleanup());
	return !HasAnyErrors();
}
