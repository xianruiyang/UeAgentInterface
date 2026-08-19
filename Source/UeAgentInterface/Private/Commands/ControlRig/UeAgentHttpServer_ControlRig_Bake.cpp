#include "UeAgentHttpServer_ControlRig.h"

#include "Domains/Rigging/UeAgentControlRigDomainBackend.h"

#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/ActorComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "ControlRig.h"
#include "ControlRigSequencerEditorLibrary.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Exporters/AnimSeqExportOption.h"
#include "GameFramework/Actor.h"
#include "LevelSequence.h"
#include "LevelSequenceActor.h"
#include "LevelSequencePlayer.h"
#include "Misc/PackageName.h"
#include "MovieScene.h"
#include "MovieSceneBindingProxy.h"
#include "MovieSceneSequenceID.h"
#include "MovieSceneSequencePlayer.h"
#include "Rigs/FKControlRig.h"
#include "SequencerTools.h"
#include "Units/Execution/RigUnit_InverseExecution.h"
#include "UObject/Package.h"

static bool ParseGuidParam(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, FGuid& OutGuid)
{
	FString Text;
	if (!Params.IsValid() || !Params->TryGetStringField(FieldName, Text))
	{
		return false;
	}
	return FGuid::Parse(Text, OutGuid);
}

struct FUeAgentControlRigBakeBindingInfo
{
	bool bBindingExists = false;
	bool bPlayerCreated = false;
	int32 BoundObjectCount = 0;
	TArray<FString> BoundObjectPaths;
	FString FirstSkeletalMeshComponentPath;
	USkeletalMesh* SkeletalMesh = nullptr;
	USkeleton* Skeleton = nullptr;
	FString Error;
};

static TSharedPtr<FJsonObject> MakeBakeBindingInfoJson(const FUeAgentControlRigBakeBindingInfo& Info)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetBoolField(TEXT("binding_exists"), Info.bBindingExists);
	Obj->SetBoolField(TEXT("player_created"), Info.bPlayerCreated);
	Obj->SetNumberField(TEXT("bound_object_count"), Info.BoundObjectCount);
	TArray<TSharedPtr<FJsonValue>> BoundObjects;
	for (const FString& Path : Info.BoundObjectPaths)
	{
		BoundObjects.Add(MakeShared<FJsonValueString>(Path));
	}
	Obj->SetArrayField(TEXT("bound_objects"), BoundObjects);
	Obj->SetStringField(TEXT("skeletal_mesh_component"), Info.FirstSkeletalMeshComponentPath);
	Obj->SetStringField(TEXT("skeletal_mesh"), Info.SkeletalMesh ? Info.SkeletalMesh->GetPathName() : TEXT(""));
	Obj->SetStringField(TEXT("skeleton"), Info.Skeleton ? Info.Skeleton->GetPathName() : TEXT(""));
	Obj->SetStringField(TEXT("error"), Info.Error);
	return Obj;
}

static USkeletalMeshComponent* FindSkeletalMeshComponentInBoundObject(UObject* BoundObject)
{
	if (!BoundObject)
	{
		return nullptr;
	}

	if (AActor* Actor = Cast<AActor>(BoundObject))
	{
		for (UActorComponent* Component : Actor->GetComponents())
		{
			if (USkeletalMeshComponent* SkeletalMeshComponent = Cast<USkeletalMeshComponent>(Component))
			{
				if (SkeletalMeshComponent->GetSkeletalMeshAsset())
				{
					return SkeletalMeshComponent;
				}
			}
		}
	}
	else if (USkeletalMeshComponent* SkeletalMeshComponent = Cast<USkeletalMeshComponent>(BoundObject))
	{
		if (SkeletalMeshComponent->GetSkeletalMeshAsset())
		{
			return SkeletalMeshComponent;
		}
	}

	return nullptr;
}

static FUeAgentControlRigBakeBindingInfo InspectBakeBinding(UWorld* World, ULevelSequence* Sequence, const FGuid& BindingGuid)
{
	FUeAgentControlRigBakeBindingInfo Info;
	if (!World)
	{
		Info.Error = TEXT("editor_world_not_found");
		return Info;
	}
	if (!Sequence || !Sequence->GetMovieScene())
	{
		Info.Error = TEXT("level_sequence_not_found");
		return Info;
	}

	Info.bBindingExists = Sequence->GetMovieScene()->FindBinding(BindingGuid) != nullptr;
	if (!Info.bBindingExists)
	{
		Info.Error = TEXT("binding_not_found_in_level_sequence");
		return Info;
	}

	ALevelSequenceActor* OutActor = nullptr;
	FMovieSceneSequencePlaybackSettings Settings;
	ULevelSequencePlayer* Player = ULevelSequencePlayer::CreateLevelSequencePlayer(World, Sequence, Settings, OutActor);
	Info.bPlayerCreated = Player != nullptr;
	if (!Player)
	{
		Info.Error = TEXT("level_sequence_player_create_failed");
		return Info;
	}

	if (UMovieScene* MovieScene = Sequence->GetMovieScene())
	{
		const FFrameNumber StartFrame = MovieScene->GetPlaybackRange().GetLowerBoundValue();
		Player->SetPlaybackPosition(FMovieSceneSequencePlaybackParams(FFrameTime(StartFrame), EUpdatePositionMethod::Jump));
	}

	for (TWeakObjectPtr<> RuntimeObject : Player->FindBoundObjects(BindingGuid, MovieSceneSequenceID::Root))
	{
		UObject* BoundObject = RuntimeObject.Get();
		if (!BoundObject)
		{
			continue;
		}

		++Info.BoundObjectCount;
		Info.BoundObjectPaths.Add(BoundObject->GetPathName());
		if (!Info.SkeletalMesh)
		{
			if (USkeletalMeshComponent* SkeletalMeshComponent = FindSkeletalMeshComponentInBoundObject(BoundObject))
			{
				Info.FirstSkeletalMeshComponentPath = SkeletalMeshComponent->GetPathName();
				Info.SkeletalMesh = SkeletalMeshComponent->GetSkeletalMeshAsset();
				Info.Skeleton = Info.SkeletalMesh ? Info.SkeletalMesh->GetSkeleton() : nullptr;
			}
		}
	}

	if (!Info.SkeletalMesh || !Info.Skeleton)
	{
		Info.Error = Info.BoundObjectCount > 0 ? TEXT("bound_object_has_no_skeletal_mesh") : TEXT("binding_resolved_no_objects");
	}

	Player->Stop();
	if (OutActor)
	{
		World->DestroyActor(OutActor);
	}
	return Info;
}

static UAnimSequence* CreateAnimSequenceAssetForBake(const FString& OutputAssetPath, USkeleton* Skeleton, USkeletalMesh* PreviewMesh, FString& OutError)
{
	const FString NormalizedPath = UeAgentControlRigOps::NormalizeAssetPath(OutputAssetPath);
	if (!FPackageName::IsValidLongPackageName(NormalizedPath))
	{
		OutError = TEXT("invalid_output_anim_sequence_path");
		return nullptr;
	}
	if (!Skeleton)
	{
		OutError = TEXT("target_skeleton_required_for_new_anim_sequence");
		return nullptr;
	}

	const FString AssetName = FPackageName::GetLongPackageAssetName(NormalizedPath);
	if (AssetName.IsEmpty())
	{
		OutError = TEXT("invalid_output_anim_sequence_path");
		return nullptr;
	}

	UPackage* Package = CreatePackage(*NormalizedPath);
	if (!Package)
	{
		OutError = TEXT("create_anim_sequence_package_failed");
		return nullptr;
	}
	Package->FullyLoad();
	if (UObject* ExistingObject = FindObject<UObject>(Package, *AssetName))
	{
		OutError = ExistingObject->IsA<UAnimSequence>() ? TEXT("anim_sequence_already_exists") : TEXT("output_asset_exists_with_different_class");
		return nullptr;
	}

	UAnimSequence* AnimSequence = NewObject<UAnimSequence>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	if (!AnimSequence)
	{
		OutError = TEXT("create_anim_sequence_failed");
		return nullptr;
	}

	AnimSequence->SetSkeleton(Skeleton);
	if (PreviewMesh)
	{
		AnimSequence->SetPreviewMesh(PreviewMesh, true);
	}
	FAssetRegistryModule::AssetCreated(AnimSequence);
	Package->MarkPackageDirty();
	return AnimSequence;
}

bool FUeAgentHttpServer::CmdControlRigBakeToAnimation(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString SequencePath;
	FString OutputAnimSequencePath;
	if (!JsonTryGetString(Ctx.Params, TEXT("sequence_path"), SequencePath) || SequencePath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("sequence_path_required");
		return false;
	}
	if (!JsonTryGetString(Ctx.Params, TEXT("output_anim_sequence"), OutputAnimSequencePath) || OutputAnimSequencePath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("output_anim_sequence_required");
		return false;
	}
	FGuid BindingGuid;
	if (!ParseGuidParam(Ctx.Params, TEXT("binding_id"), BindingGuid) && !ParseGuidParam(Ctx.Params, TEXT("binding_guid"), BindingGuid))
	{
		OutError = TEXT("binding_id_required");
		return false;
	}

	ULevelSequence* Sequence = Cast<ULevelSequence>(UeAgentControlRigOps::LoadAssetObject(SequencePath));
	if (!Sequence)
	{
		OutError = TEXT("level_sequence_not_found");
		return false;
	}

	FString WorldError;
	UWorld* World = GetEditorWorld(WorldError);
	if (!World)
	{
		OutError = WorldError.IsEmpty() ? TEXT("editor_world_not_found") : WorldError;
		return false;
	}

	const FUeAgentControlRigBakeBindingInfo BindingInfo = InspectBakeBinding(World, Sequence, BindingGuid);
	if (!BindingInfo.Error.IsEmpty())
	{
		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("sequence_path"), Sequence->GetOutermost()->GetName());
		OutData->SetStringField(TEXT("output_anim_sequence"), UeAgentControlRigOps::NormalizeAssetPath(OutputAnimSequencePath));
		OutData->SetStringField(TEXT("binding_id"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphens));
		OutData->SetObjectField(TEXT("binding_preflight"), MakeBakeBindingInfoJson(BindingInfo));
		OutError = BindingInfo.Error;
		return false;
	}

	UObject* ExistingOutputObject = UeAgentControlRigOps::LoadAssetObject(OutputAnimSequencePath);
	UAnimSequence* AnimSequence = Cast<UAnimSequence>(ExistingOutputObject);
	if (ExistingOutputObject && !AnimSequence)
	{
		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("sequence_path"), Sequence->GetOutermost()->GetName());
		OutData->SetStringField(TEXT("output_anim_sequence"), ExistingOutputObject->GetPathName());
		OutData->SetStringField(TEXT("output_asset_class"), ExistingOutputObject->GetClass()->GetPathName());
		OutData->SetStringField(TEXT("binding_id"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphens));
		OutData->SetObjectField(TEXT("binding_preflight"), MakeBakeBindingInfoJson(BindingInfo));
		OutError = TEXT("output_asset_exists_with_different_class");
		return false;
	}

	bool bCreated = false;
	if (!AnimSequence)
	{
		const FString OutputAssetPath = UeAgentControlRigOps::NormalizeAssetPath(OutputAnimSequencePath);
		if (!FPackageName::IsValidLongPackageName(OutputAssetPath))
		{
			OutError = TEXT("invalid_output_anim_sequence_path");
			return false;
		}

		FString SkeletonPath;
		FString PreviewMeshPath;
		JsonTryGetString(Ctx.Params, TEXT("target_skeleton"), SkeletonPath);
		JsonTryGetString(Ctx.Params, TEXT("preview_skeletal_mesh"), PreviewMeshPath);
		USkeletalMesh* PreviewMesh = PreviewMeshPath.TrimStartAndEnd().IsEmpty() ? BindingInfo.SkeletalMesh : Cast<USkeletalMesh>(UeAgentControlRigOps::LoadAssetObject(PreviewMeshPath));
		if (!PreviewMeshPath.TrimStartAndEnd().IsEmpty() && !PreviewMesh)
		{
			OutError = TEXT("preview_skeletal_mesh_not_found");
			return false;
		}
		USkeleton* Skeleton = SkeletonPath.TrimStartAndEnd().IsEmpty() ? nullptr : Cast<USkeleton>(UeAgentControlRigOps::LoadAssetObject(SkeletonPath));
		if (!SkeletonPath.TrimStartAndEnd().IsEmpty() && !Skeleton)
		{
			OutError = TEXT("target_skeleton_not_found");
			return false;
		}
		if (!Skeleton && PreviewMesh)
		{
			Skeleton = PreviewMesh->GetSkeleton();
		}
		if (!Skeleton)
		{
			Skeleton = BindingInfo.Skeleton;
		}
		if (!Skeleton)
		{
			OutError = TEXT("target_skeleton_required_for_new_anim_sequence");
			return false;
		}
		if (BindingInfo.Skeleton && !Skeleton->IsCompatibleForEditor(BindingInfo.Skeleton))
		{
			OutError = TEXT("target_skeleton_not_compatible_with_sequence_binding");
			return false;
		}
		if (PreviewMesh && PreviewMesh->GetSkeleton() && !Skeleton->IsCompatibleForEditor(PreviewMesh->GetSkeleton()))
		{
			OutError = TEXT("preview_skeletal_mesh_not_compatible_with_target_skeleton");
			return false;
		}

		FString CreateError;
		AnimSequence = CreateAnimSequenceAssetForBake(OutputAssetPath, Skeleton, PreviewMesh, CreateError);
		if (!AnimSequence)
		{
			OutError = CreateError.IsEmpty() ? TEXT("create_anim_sequence_failed") : CreateError;
			return false;
		}
		bCreated = true;
	}

	bool bCreateLink = true;
	JsonTryGetBool(Ctx.Params, TEXT("create_link"), bCreateLink);
	UAnimSeqExportOption* ExportOptions = NewObject<UAnimSeqExportOption>(GetTransientPackage(), NAME_None, RF_Transient);
	const FMovieSceneBindingProxy Binding(BindingGuid, Sequence);
	const bool bExported = USequencerToolsFunctionLibrary::ExportAnimSequence(World, Sequence, AnimSequence, ExportOptions, Binding, bCreateLink);
	if (bExported)
	{
		AnimSequence->MarkPackageDirty();
	}

	const IAnimationDataModel* DataModel = AnimSequence ? AnimSequence->GetDataModel() : nullptr;
	const int32 BoneTrackCount = DataModel ? DataModel->GetNumBoneTracks() : 0;
	TArray<FName> BoneTrackNames;
	if (DataModel)
	{
		DataModel->GetBoneTrackNames(BoneTrackNames);
	}
	TArray<TSharedPtr<FJsonValue>> BoneTrackNamesJson;
	for (const FName& BoneTrackName : BoneTrackNames)
	{
		BoneTrackNamesJson.Add(MakeShared<FJsonValueString>(BoneTrackName.ToString()));
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("sequence_path"), Sequence->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("output_anim_sequence"), AnimSequence->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("binding_id"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphens));
	OutData->SetStringField(TEXT("export_api"), TEXT("USequencerToolsFunctionLibrary::ExportAnimSequence"));
	OutData->SetObjectField(TEXT("binding_preflight"), MakeBakeBindingInfoJson(BindingInfo));
	OutData->SetBoolField(TEXT("created"), bCreated);
	OutData->SetBoolField(TEXT("exported"), bExported);
	OutData->SetNumberField(TEXT("bone_track_count"), BoneTrackCount);
	OutData->SetArrayField(TEXT("bone_track_names"), BoneTrackNamesJson);
	OutData->SetStringField(TEXT("status"), TEXT("external_sequence_action"));
	if (!bExported)
	{
		OutError = TEXT("control_rig_bake_to_animation_failed");
	}
	else if (BoneTrackCount <= 0)
	{
		OutError = TEXT("control_rig_bake_to_animation_empty_bone_tracks");
		return false;
	}
	return bExported;
}

bool FUeAgentHttpServer::CmdControlRigBakeToControlRig(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString SequencePath;
	FString ControlRigClassPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("sequence_path"), SequencePath) || SequencePath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("sequence_path_required");
		return false;
	}
	if (!JsonTryGetString(Ctx.Params, TEXT("control_rig_class"), ControlRigClassPath) || ControlRigClassPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("control_rig_class_required");
		return false;
	}
	FGuid BindingGuid;
	if (!ParseGuidParam(Ctx.Params, TEXT("binding_id"), BindingGuid) && !ParseGuidParam(Ctx.Params, TEXT("binding_guid"), BindingGuid))
	{
		OutError = TEXT("binding_id_required");
		return false;
	}

	ULevelSequence* Sequence = Cast<ULevelSequence>(UeAgentControlRigOps::LoadAssetObject(SequencePath));
	if (!Sequence)
	{
		OutError = TEXT("level_sequence_not_found");
		return false;
	}
	UClass* ControlRigClass = LoadObject<UClass>(nullptr, *ControlRigClassPath);
	if (!ControlRigClass || !ControlRigClass->IsChildOf(UControlRig::StaticClass()))
	{
		OutError = TEXT("invalid_control_rig_class");
		return false;
	}

	FString WorldError;
	UWorld* World = GetEditorWorld(WorldError);
	if (!World)
	{
		OutError = WorldError.IsEmpty() ? TEXT("editor_world_not_found") : WorldError;
		return false;
	}

	const FUeAgentControlRigBakeBindingInfo BindingInfo = InspectBakeBinding(World, Sequence, BindingGuid);
	if (!BindingInfo.Error.IsEmpty())
	{
		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("sequence_path"), Sequence->GetOutermost()->GetName());
		OutData->SetStringField(TEXT("control_rig_class"), ControlRigClass->GetPathName());
		OutData->SetStringField(TEXT("binding_id"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphens));
		OutData->SetObjectField(TEXT("binding_preflight"), MakeBakeBindingInfoJson(BindingInfo));
		OutError = BindingInfo.Error;
		return false;
	}

	const bool bIsFKControlRig = ControlRigClass->IsChildOf(UFKControlRig::StaticClass());
	const UControlRig* DefaultControlRig = ControlRigClass->GetDefaultObject<UControlRig>();
	const bool bSupportsInverseEvent = DefaultControlRig &&
		(DefaultControlRig->SupportsEvent(FRigUnit_InverseExecution::EventName) || DefaultControlRig->SupportsEvent(FName(TEXT("Inverse"))));
	if (!bIsFKControlRig && !bSupportsInverseEvent)
	{
		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("sequence_path"), Sequence->GetOutermost()->GetName());
		OutData->SetStringField(TEXT("control_rig_class"), ControlRigClass->GetPathName());
		OutData->SetStringField(TEXT("binding_id"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphens));
		OutData->SetBoolField(TEXT("is_fk_control_rig"), bIsFKControlRig);
		OutData->SetBoolField(TEXT("supports_inverse_event"), bSupportsInverseEvent);
		OutData->SetObjectField(TEXT("binding_preflight"), MakeBakeBindingInfoJson(BindingInfo));
		OutError = TEXT("control_rig_bake_requires_fk_or_inverse_event");
		return false;
	}

	bool bReduceKeys = false;
	JsonTryGetBool(Ctx.Params, TEXT("reduce_keys"), bReduceKeys);
	double ToleranceNum = 0.001;
	JsonTryGetNumber(Ctx.Params, TEXT("tolerance"), ToleranceNum);
	bool bResetControls = true;
	JsonTryGetBool(Ctx.Params, TEXT("reset_controls"), bResetControls);

	UAnimSeqExportOption* ExportOptions = NewObject<UAnimSeqExportOption>(GetTransientPackage(), NAME_None, RF_Transient);
	const FMovieSceneBindingProxy Binding(BindingGuid, Sequence);
	const bool bBaked = UControlRigSequencerEditorLibrary::BakeToControlRig(World, Sequence, ControlRigClass, ExportOptions, bReduceKeys, static_cast<float>(ToleranceNum), Binding, bResetControls);

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("sequence_path"), Sequence->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("control_rig_class"), ControlRigClass->GetPathName());
	OutData->SetStringField(TEXT("binding_id"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphens));
	OutData->SetBoolField(TEXT("is_fk_control_rig"), bIsFKControlRig);
	OutData->SetBoolField(TEXT("supports_inverse_event"), bSupportsInverseEvent);
	OutData->SetObjectField(TEXT("binding_preflight"), MakeBakeBindingInfoJson(BindingInfo));
	OutData->SetBoolField(TEXT("baked"), bBaked);
	if (!bBaked)
	{
		OutError = TEXT("control_rig_bake_to_control_rig_failed");
	}
	return bBaked;
}
