bool FUeAgentHttpServer::ExecuteSequenceCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!OutData.IsValid())
	{
		OutData = MakeShared<FJsonObject>();
	}
	else
	{
		OutData->Values.Reset();
	}

	if (CommandLower == TEXT("sequence_list_level_sequences")) return CmdSequenceListLevelSequences(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_create_level_sequence")) return CmdSequenceCreateLevelSequence(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_open_level_sequence")) return CmdSequenceOpenLevelSequence(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_get_level_sequence_info")) return CmdSequenceGetLevelSequenceInfo(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_export_folder")) return CmdSequenceExportFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_apply_folder")) return CmdSequenceApplyFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_set_level_sequence_playback_range")) return CmdSequenceSetLevelSequencePlaybackRange(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_set_level_sequence_display_rate")) return CmdSequenceSetLevelSequenceDisplayRate(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_add_actor_binding")) return CmdSequenceAddActorBinding(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_remove_actor_binding")) return CmdSequenceRemoveActorBinding(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_add_property_track")) return CmdSequenceAddPropertyTrack(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_add_property_key")) return CmdSequenceAddPropertyKey(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_add_visibility_track")) return CmdSequenceAddVisibilityTrack(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_add_visibility_key")) return CmdSequenceAddVisibilityKey(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_add_bool_property_track")) return CmdSequenceAddBoolPropertyTrack(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_add_bool_property_key")) return CmdSequenceAddBoolPropertyKey(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_add_float_property_track")) return CmdSequenceAddFloatPropertyTrack(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_add_float_property_key")) return CmdSequenceAddFloatPropertyKey(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_add_integer_property_track")) return CmdSequenceAddIntegerPropertyTrack(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_add_integer_property_key")) return CmdSequenceAddIntegerPropertyKey(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_add_transform_track")) return CmdSequenceAddTransformTrack(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_add_transform_key")) return CmdSequenceAddTransformKey(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_add_skeletal_animation_track")) return CmdSequenceAddSkeletalAnimationTrack(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_add_skeletal_animation_section")) return CmdSequenceAddSkeletalAnimationSection(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_update_skeletal_animation_section")) return CmdSequenceUpdateSkeletalAnimationSection(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_remove_skeletal_animation_section")) return CmdSequenceRemoveSkeletalAnimationSection(Ctx, OutData, OutError);

	if (CommandLower == TEXT("sequence_list_umg_animations")) return CmdSequenceListUmgAnimations(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_get_umg_animation_info")) return CmdSequenceGetUmgAnimationInfo(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_create_umg_animation")) return CmdSequenceCreateUmgAnimation(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_rename_umg_animation")) return CmdSequenceRenameUmgAnimation(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_remove_umg_animation")) return CmdSequenceRemoveUmgAnimation(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_set_umg_animation_playback_range")) return CmdSequenceSetUmgAnimationPlaybackRange(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_set_umg_animation_display_rate")) return CmdSequenceSetUmgAnimationDisplayRate(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_add_umg_widget_transform_key")) return CmdSequenceAddUmgWidgetTransformKey(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_add_umg_widget_translation_key")) return CmdSequenceAddUmgWidgetTranslationKey(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_add_umg_widget_opacity_key")) return CmdSequenceAddUmgWidgetOpacityKey(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_add_umg_widget_float_key")) return CmdSequenceAddUmgWidgetFloatKey(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_add_umg_widget_color_key")) return CmdSequenceAddUmgWidgetColorKey(Ctx, OutData, OutError);

	OutError = TEXT("unknown_sequence_command");
	return false;
}

bool FUeAgentHttpServer::CmdSequenceListLevelSequences(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString RootPath = TEXT("/Game");
	JsonTryGetString(Ctx.Params, TEXT("root_path"), RootPath);
	RootPath.TrimStartAndEndInline();
	if (RootPath.IsEmpty())
	{
		RootPath = TEXT("/Game");
	}
	if (!RootPath.StartsWith(TEXT("/")))
	{
		RootPath = TEXT("/") + RootPath;
	}
	if (!FPackageName::IsValidPath(RootPath))
	{
		OutError = TEXT("invalid_root_path");
		return false;
	}

	int32 Limit = 200;
	double LimitNumber = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("limit"), LimitNumber))
	{
		Limit = FMath::Clamp(FMath::RoundToInt(LimitNumber), 1, 5000);
	}

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(ULevelSequence::StaticClass()->GetClassPathName());
	Filter.PackagePaths.Add(*RootPath);
	Filter.bRecursivePaths = true;

	TArray<FAssetData> AssetList;
	AssetRegistry.GetAssets(Filter, AssetList);

	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Reserve(FMath::Min(AssetList.Num(), Limit));
	for (const FAssetData& AssetData : AssetList)
	{
		if (Items.Num() >= Limit)
		{
			break;
		}

		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
		Item->SetStringField(TEXT("package_name"), AssetData.PackageName.ToString());
		Item->SetStringField(TEXT("package_path"), AssetData.PackagePath.ToString());
		Item->SetStringField(TEXT("object_path"), AssetData.GetObjectPathString());
		Items.Add(MakeShared<FJsonValueObject>(Item));
	}

	OutData->SetStringField(TEXT("root_path"), RootPath);
	OutData->SetNumberField(TEXT("total_count"), AssetList.Num());
	OutData->SetNumberField(TEXT("returned_count"), Items.Num());
	OutData->SetArrayField(TEXT("items"), Items);
	return true;
}

bool FUeAgentHttpServer::CmdSequenceCreateLevelSequence(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	const FString PackageName = UeAgentSequenceOps::NormalizeAssetPath(AssetPathInput);
	if (!FPackageName::IsValidLongPackageName(PackageName))
	{
		OutError = TEXT("invalid_asset_path");
		return false;
	}
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	if (AssetName.IsEmpty())
	{
		OutError = TEXT("invalid_asset_name");
		return false;
	}

	const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
	if (UeAgentSequenceOps::AssetExists(ObjectPath))
	{
		OutError = TEXT("asset_already_exists");
		return false;
	}

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		OutError = TEXT("create_package_failed");
		return false;
	}

	ULevelSequence* Sequence = NewObject<ULevelSequence>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	if (!Sequence)
	{
		OutError = TEXT("create_level_sequence_failed");
		return false;
	}
	Sequence->Initialize();
	if (!Sequence->GetMovieScene())
	{
		OutError = TEXT("create_movie_scene_failed");
		return false;
	}

	double StartSeconds = 0.0;
	double DurationSeconds = 5.0;
	JsonTryGetNumber(Ctx.Params, TEXT("start_seconds"), StartSeconds);
	JsonTryGetNumber(Ctx.Params, TEXT("duration_seconds"), DurationSeconds);
	if (DurationSeconds <= 0.0)
	{
		DurationSeconds = 5.0;
	}
	UeAgentSequenceOps::SetMovieScenePlaybackRangeSeconds(Sequence->GetMovieScene(), StartSeconds, DurationSeconds);

	double DisplayRateNum = 30.0;
	double DisplayRateDen = 1.0;
	JsonTryGetNumber(Ctx.Params, TEXT("display_rate_num"), DisplayRateNum);
	JsonTryGetNumber(Ctx.Params, TEXT("display_rate_den"), DisplayRateDen);
	const int32 DisplayRateNumInt = FMath::Max(1, FMath::RoundToInt(DisplayRateNum));
	const int32 DisplayRateDenInt = FMath::Max(1, FMath::RoundToInt(DisplayRateDen));
	Sequence->GetMovieScene()->SetDisplayRate(FFrameRate(DisplayRateNumInt, DisplayRateDenInt));

	FAssetRegistryModule::AssetCreated(Sequence);
	Sequence->MarkPackageDirty();
	Package->MarkPackageDirty();

	bool bOpenEditor = false;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor"), bOpenEditor);
	if (bOpenEditor && GEditor)
	{
		if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			AssetEditorSubsystem->OpenEditorForAsset(Sequence);
		}
	}

	bool bSaveAfterCreate = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSaveAfterCreate);
	if (bSaveAfterCreate)
	{
		if (!UeAgentSequenceOps::SaveAssetPackage(Sequence, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), PackageName);
	OutData->SetStringField(TEXT("asset_name"), AssetName);
	OutData->SetStringField(TEXT("object_path"), ObjectPath);
	OutData->SetStringField(TEXT("display_rate"), FString::Printf(TEXT("%d/%d"), DisplayRateNumInt, DisplayRateDenInt));
	OutData->SetNumberField(TEXT("start_seconds"), StartSeconds);
	OutData->SetNumberField(TEXT("duration_seconds"), DurationSeconds);
	OutData->SetBoolField(TEXT("opened_editor"), bOpenEditor);
	return true;
}

bool FUeAgentHttpServer::CmdSequenceOpenLevelSequence(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	ULevelSequence* Sequence = UeAgentSequenceOps::LoadLevelSequence(AssetPath);
	if (!Sequence)
	{
		OutError = TEXT("level_sequence_not_found");
		return false;
	}
	if (!GEditor)
	{
		OutError = TEXT("editor_not_available");
		return false;
	}
	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!AssetEditorSubsystem)
	{
		OutError = TEXT("asset_editor_subsystem_not_available");
		return false;
	}

	AssetEditorSubsystem->OpenEditorForAsset(Sequence);
	OutData->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
	OutData->SetBoolField(TEXT("opened"), true);
	return true;
}

bool FUeAgentHttpServer::CmdSequenceGetLevelSequenceInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	ULevelSequence* Sequence = UeAgentSequenceOps::LoadLevelSequence(AssetPath);
	if (!Sequence)
	{
		OutError = TEXT("level_sequence_not_found");
		return false;
	}
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene)
	{
		OutError = TEXT("movie_scene_not_found");
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
	OutData->SetStringField(TEXT("display_rate"), MovieScene->GetDisplayRate().ToPrettyText().ToString());
	OutData->SetStringField(TEXT("tick_resolution"), MovieScene->GetTickResolution().ToPrettyText().ToString());
	OutData->SetNumberField(TEXT("playback_start_frame"), MovieScene->GetPlaybackRange().GetLowerBoundValue().Value);
	OutData->SetNumberField(TEXT("playback_end_exclusive_frame"), MovieScene->GetPlaybackRange().GetUpperBoundValue().Value);
	OutData->SetNumberField(TEXT("master_track_count"), MovieScene->GetTracks().Num());
	OutData->SetNumberField(TEXT("binding_count"), MovieScene->GetBindings().Num());

	TArray<TSharedPtr<FJsonValue>> MasterTracksJson;
	for (UMovieSceneTrack* Track : MovieScene->GetTracks())
	{
		if (Track)
		{
			MasterTracksJson.Add(MakeShared<FJsonValueObject>(UeAgentSequenceOps::BuildMovieSceneTrackSummary(MovieScene, Track)));
		}
	}

	TArray<TSharedPtr<FJsonValue>> BindingsJson;
	for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
	{
		BindingsJson.Add(MakeShared<FJsonValueObject>(UeAgentSequenceOps::BuildMovieSceneBindingSummary(MovieScene, Binding)));
	}
	OutData->SetArrayField(TEXT("master_tracks"), MasterTracksJson);
	OutData->SetArrayField(TEXT("bindings"), BindingsJson);
	return true;
}

bool FUeAgentHttpServer::CmdSequenceSetLevelSequencePlaybackRange(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	double StartSeconds = 0.0;
	double DurationSeconds = 5.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("start_seconds"), StartSeconds))
	{
		OutError = TEXT("missing_start_seconds");
		return false;
	}
	if (!JsonTryGetNumber(Ctx.Params, TEXT("duration_seconds"), DurationSeconds))
	{
		OutError = TEXT("missing_duration_seconds");
		return false;
	}
	if (DurationSeconds <= 0.0)
	{
		OutError = TEXT("invalid_duration_seconds");
		return false;
	}

	ULevelSequence* Sequence = UeAgentSequenceOps::LoadLevelSequence(AssetPath);
	if (!Sequence)
	{
		OutError = TEXT("level_sequence_not_found");
		return false;
	}
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene)
	{
		OutError = TEXT("movie_scene_not_found");
		return false;
	}

	MovieScene->Modify();
	Sequence->Modify();
	if (!UeAgentSequenceOps::SetMovieScenePlaybackRangeSeconds(MovieScene, StartSeconds, DurationSeconds))
	{
		OutError = TEXT("set_playback_range_failed");
		return false;
	}
	Sequence->MarkPackageDirty();

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentSequenceOps::SaveAssetPackage(Sequence, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
	OutData->SetNumberField(TEXT("start_seconds"), StartSeconds);
	OutData->SetNumberField(TEXT("duration_seconds"), DurationSeconds);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdSequenceSetLevelSequenceDisplayRate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	double DisplayRateNumValue = 0.0;
	double DisplayRateDenValue = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("display_rate_num"), DisplayRateNumValue))
	{
		OutError = TEXT("missing_display_rate_num");
		return false;
	}
	if (!JsonTryGetNumber(Ctx.Params, TEXT("display_rate_den"), DisplayRateDenValue))
	{
		OutError = TEXT("missing_display_rate_den");
		return false;
	}
	const int32 DisplayRateNum = FMath::RoundToInt(DisplayRateNumValue);
	const int32 DisplayRateDen = FMath::RoundToInt(DisplayRateDenValue);
	if (DisplayRateNum <= 0 || DisplayRateDen <= 0)
	{
		OutError = TEXT("invalid_display_rate");
		return false;
	}

	ULevelSequence* Sequence = UeAgentSequenceOps::LoadLevelSequence(AssetPath);
	if (!Sequence)
	{
		OutError = TEXT("level_sequence_not_found");
		return false;
	}
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene)
	{
		OutError = TEXT("movie_scene_not_found");
		return false;
	}

	MovieScene->Modify();
	Sequence->Modify();
	MovieScene->SetDisplayRate(FFrameRate(DisplayRateNum, DisplayRateDen));
	Sequence->MarkPackageDirty();

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentSequenceOps::SaveAssetPackage(Sequence, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
	OutData->SetStringField(TEXT("display_rate"), FString::Printf(TEXT("%d/%d"), DisplayRateNum, DisplayRateDen));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}
