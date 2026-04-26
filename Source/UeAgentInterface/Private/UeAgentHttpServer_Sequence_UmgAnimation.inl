bool FUeAgentHttpServer::CmdSequenceListUmgAnimations(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UWidgetBlueprint* WidgetBlueprint = UeAgentSequenceOps::LoadWidgetBlueprint(AssetPath);
	if (!WidgetBlueprint)
	{
		OutError = TEXT("widget_blueprint_not_found");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> AnimationsJson;
	for (UWidgetAnimation* Animation : WidgetBlueprint->Animations)
	{
		if (!Animation)
		{
			continue;
		}

		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Animation->GetName());
#if WITH_EDITOR
		Item->SetStringField(TEXT("display_label"), Animation->GetDisplayLabel());
#else
		Item->SetStringField(TEXT("display_label"), Animation->GetName());
#endif
		Item->SetNumberField(TEXT("start_seconds"), Animation->GetStartTime());
		Item->SetNumberField(TEXT("end_seconds"), Animation->GetEndTime());
		if (UMovieScene* MovieScene = Animation->GetMovieScene())
		{
			Item->SetStringField(TEXT("display_rate"), MovieScene->GetDisplayRate().ToPrettyText().ToString());
			Item->SetStringField(TEXT("tick_resolution"), MovieScene->GetTickResolution().ToPrettyText().ToString());
		}
		AnimationsJson.Add(MakeShared<FJsonValueObject>(Item));
	}

	OutData->SetStringField(TEXT("asset_path"), WidgetBlueprint->GetPathName());
	OutData->SetNumberField(TEXT("animation_count"), AnimationsJson.Num());
	OutData->SetArrayField(TEXT("animations"), AnimationsJson);
	return true;
}

bool FUeAgentHttpServer::CmdSequenceGetUmgAnimationInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString AnimationName;
	if (!JsonTryGetString(Ctx.Params, TEXT("animation_name"), AnimationName) || AnimationName.IsEmpty())
	{
		OutError = TEXT("missing_animation_name");
		return false;
	}

	UWidgetBlueprint* WidgetBlueprint = UeAgentSequenceOps::LoadWidgetBlueprint(AssetPath);
	if (!WidgetBlueprint)
	{
		OutError = TEXT("widget_blueprint_not_found");
		return false;
	}

	UWidgetAnimation* TargetAnimation = UeAgentSequenceOps::FindWidgetAnimationByAnyName(WidgetBlueprint, AnimationName);
	if (!TargetAnimation)
	{
		OutError = TEXT("widget_animation_not_found");
		return false;
	}

	UMovieScene* MovieScene = TargetAnimation->GetMovieScene();
	if (!MovieScene)
	{
		OutError = TEXT("movie_scene_not_found");
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), WidgetBlueprint->GetPathName());
	OutData->SetStringField(TEXT("animation_name"), TargetAnimation->GetName());
#if WITH_EDITOR
	OutData->SetStringField(TEXT("display_label"), TargetAnimation->GetDisplayLabel());
#else
	OutData->SetStringField(TEXT("display_label"), TargetAnimation->GetName());
#endif
	OutData->SetNumberField(TEXT("start_seconds"), TargetAnimation->GetStartTime());
	OutData->SetNumberField(TEXT("end_seconds"), TargetAnimation->GetEndTime());
	OutData->SetStringField(TEXT("display_rate"), MovieScene->GetDisplayRate().ToPrettyText().ToString());
	OutData->SetStringField(TEXT("tick_resolution"), MovieScene->GetTickResolution().ToPrettyText().ToString());
	OutData->SetNumberField(TEXT("binding_count"), MovieScene->GetBindings().Num());

	TArray<TSharedPtr<FJsonValue>> BindingsJson;
	for (const FWidgetAnimationBinding& Binding : TargetAnimation->AnimationBindings)
	{
		TSharedPtr<FJsonObject> BindingObject = MakeShared<FJsonObject>();
		BindingObject->SetStringField(TEXT("widget_name"), Binding.WidgetName.ToString());
		BindingObject->SetStringField(TEXT("animation_guid"), Binding.AnimationGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		BindingObject->SetBoolField(TEXT("is_root_widget"), Binding.bIsRootWidget);

		if (const FMovieSceneBinding* MovieSceneBinding = MovieScene->FindBinding(Binding.AnimationGuid))
		{
			BindingObject->SetStringField(TEXT("binding_name"), MovieSceneBinding->GetName());
			BindingObject->SetNumberField(TEXT("track_count"), MovieSceneBinding->GetTracks().Num());

			TArray<TSharedPtr<FJsonValue>> TracksJson;
			for (UMovieSceneTrack* Track : MovieSceneBinding->GetTracks())
			{
				if (Track)
				{
					TracksJson.Add(MakeShared<FJsonValueObject>(UeAgentSequenceOps::BuildMovieSceneTrackSummary(MovieScene, Track)));
				}
			}
			BindingObject->SetArrayField(TEXT("tracks"), TracksJson);
		}
		else
		{
			BindingObject->SetStringField(TEXT("binding_name"), TEXT(""));
			BindingObject->SetNumberField(TEXT("track_count"), 0);
			BindingObject->SetArrayField(TEXT("tracks"), TArray<TSharedPtr<FJsonValue>>());
		}

		BindingsJson.Add(MakeShared<FJsonValueObject>(BindingObject));
	}

	OutData->SetArrayField(TEXT("bindings"), BindingsJson);
	return true;
}

bool FUeAgentHttpServer::CmdSequenceCreateUmgAnimation(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UWidgetBlueprint* WidgetBlueprint = UeAgentSequenceOps::LoadWidgetBlueprint(AssetPath);
	if (!WidgetBlueprint)
	{
		OutError = TEXT("widget_blueprint_not_found");
		return false;
	}

	FString BaseName = TEXT("NewAnimation");
	JsonTryGetString(Ctx.Params, TEXT("animation_name"), BaseName);
	BaseName.TrimStartAndEndInline();
	if (BaseName.IsEmpty())
	{
		BaseName = TEXT("NewAnimation");
	}

	const FName UniqueName = MakeUniqueObjectName(WidgetBlueprint, UWidgetAnimation::StaticClass(), *BaseName);
	UWidgetAnimation* NewAnimation = NewObject<UWidgetAnimation>(WidgetBlueprint, UniqueName, RF_Transactional);
	if (!NewAnimation)
	{
		OutError = TEXT("create_widget_animation_failed");
		return false;
	}

	NewAnimation->SetDisplayLabel(UniqueName.ToString());
	NewAnimation->MovieScene = NewObject<UMovieScene>(NewAnimation, UniqueName, RF_Transactional);
	if (!NewAnimation->MovieScene)
	{
		OutError = TEXT("create_widget_animation_movie_scene_failed");
		return false;
	}

	double StartSeconds = 0.0;
	double DurationSeconds = 1.0;
	JsonTryGetNumber(Ctx.Params, TEXT("start_seconds"), StartSeconds);
	JsonTryGetNumber(Ctx.Params, TEXT("duration_seconds"), DurationSeconds);
	if (DurationSeconds <= 0.0)
	{
		DurationSeconds = 1.0;
	}
	UeAgentSequenceOps::SetMovieScenePlaybackRangeSeconds(NewAnimation->MovieScene, StartSeconds, DurationSeconds);
	NewAnimation->MovieScene->SetDisplayRate(FFrameRate(20, 1));

	WidgetBlueprint->Modify();
	WidgetBlueprint->Animations.Add(NewAnimation);
	WidgetBlueprint->OnVariableAdded(NewAnimation->GetFName());
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);

	bool bCompileAfterCreate = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_create"), bCompileAfterCreate);
	if (bCompileAfterCreate)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
	}

	bool bSaveAfterCreate = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSaveAfterCreate);
	if (bSaveAfterCreate)
	{
		if (!UeAgentSequenceOps::SaveAssetPackage(WidgetBlueprint, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), WidgetBlueprint->GetPathName());
	OutData->SetStringField(TEXT("animation_name"), NewAnimation->GetName());
	OutData->SetStringField(TEXT("display_label"), NewAnimation->GetDisplayLabel());
	OutData->SetNumberField(TEXT("start_seconds"), NewAnimation->GetStartTime());
	OutData->SetNumberField(TEXT("end_seconds"), NewAnimation->GetEndTime());
	OutData->SetBoolField(TEXT("compiled"), bCompileAfterCreate);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCreate);
	return true;
}

bool FUeAgentHttpServer::CmdSequenceRenameUmgAnimation(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString AnimationName;
	if (!JsonTryGetString(Ctx.Params, TEXT("animation_name"), AnimationName) || AnimationName.IsEmpty())
	{
		OutError = TEXT("missing_animation_name");
		return false;
	}

	FString NewAnimationName;
	if (!JsonTryGetString(Ctx.Params, TEXT("new_animation_name"), NewAnimationName) || NewAnimationName.IsEmpty())
	{
		OutError = TEXT("missing_new_animation_name");
		return false;
	}

	UWidgetBlueprint* WidgetBlueprint = UeAgentSequenceOps::LoadWidgetBlueprint(AssetPath);
	if (!WidgetBlueprint)
	{
		OutError = TEXT("widget_blueprint_not_found");
		return false;
	}

	UWidgetAnimation* TargetAnimation = UeAgentSequenceOps::FindWidgetAnimationByAnyName(WidgetBlueprint, AnimationName);
	if (!TargetAnimation)
	{
		OutError = TEXT("widget_animation_not_found");
		return false;
	}

	UWidgetAnimation* Existing = UeAgentSequenceOps::FindWidgetAnimationByAnyName(WidgetBlueprint, NewAnimationName);
	if (Existing && Existing != TargetAnimation)
	{
		OutError = TEXT("animation_name_already_exists");
		return false;
	}

	const FName OldName = TargetAnimation->GetFName();
	const FName NewName(*NewAnimationName.TrimStartAndEnd());
	if (NewName.IsNone())
	{
		OutError = TEXT("invalid_new_animation_name");
		return false;
	}

	WidgetBlueprint->Modify();
	TargetAnimation->Modify();
	if (UMovieScene* MovieScene = TargetAnimation->GetMovieScene())
	{
		MovieScene->Modify();
		MovieScene->Rename(*NewName.ToString(), nullptr, REN_DontCreateRedirectors);
	}

	TargetAnimation->SetDisplayLabel(NewName.ToString());
	TargetAnimation->Rename(*NewName.ToString(), nullptr, REN_DontCreateRedirectors);

	WidgetBlueprint->OnVariableRenamed(OldName, NewName);
	FBlueprintEditorUtils::ReplaceVariableReferences(WidgetBlueprint, OldName, NewName);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);

	bool bCompileAfterRename = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_rename"), bCompileAfterRename);
	if (bCompileAfterRename)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
	}

	bool bSaveAfterRename = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_rename"), bSaveAfterRename);
	if (bSaveAfterRename)
	{
		if (!UeAgentSequenceOps::SaveAssetPackage(WidgetBlueprint, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), WidgetBlueprint->GetPathName());
	OutData->SetStringField(TEXT("old_animation_name"), OldName.ToString());
	OutData->SetStringField(TEXT("new_animation_name"), NewName.ToString());
	OutData->SetBoolField(TEXT("compiled"), bCompileAfterRename);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterRename);
	return true;
}

bool FUeAgentHttpServer::CmdSequenceRemoveUmgAnimation(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString AnimationName;
	if (!JsonTryGetString(Ctx.Params, TEXT("animation_name"), AnimationName) || AnimationName.IsEmpty())
	{
		OutError = TEXT("missing_animation_name");
		return false;
	}

	UWidgetBlueprint* WidgetBlueprint = UeAgentSequenceOps::LoadWidgetBlueprint(AssetPath);
	if (!WidgetBlueprint)
	{
		OutError = TEXT("widget_blueprint_not_found");
		return false;
	}

	UWidgetAnimation* TargetAnimation = UeAgentSequenceOps::FindWidgetAnimationByAnyName(WidgetBlueprint, AnimationName);
	if (!TargetAnimation)
	{
		OutError = TEXT("widget_animation_not_found");
		return false;
	}

	const FName RemovedName = TargetAnimation->GetFName();
	WidgetBlueprint->Modify();
	TargetAnimation->Modify();
	TargetAnimation->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors);
	WidgetBlueprint->Animations.Remove(TargetAnimation);
	WidgetBlueprint->OnVariableRemoved(RemovedName);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);

	bool bCompileAfterRemove = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_remove"), bCompileAfterRemove);
	if (bCompileAfterRemove)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
	}

	bool bSaveAfterRemove = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_remove"), bSaveAfterRemove);
	if (bSaveAfterRemove)
	{
		if (!UeAgentSequenceOps::SaveAssetPackage(WidgetBlueprint, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), WidgetBlueprint->GetPathName());
	OutData->SetStringField(TEXT("removed_animation_name"), RemovedName.ToString());
	OutData->SetBoolField(TEXT("compiled"), bCompileAfterRemove);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterRemove);
	return true;
}

bool FUeAgentHttpServer::CmdSequenceSetUmgAnimationPlaybackRange(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString AnimationName;
	if (!JsonTryGetString(Ctx.Params, TEXT("animation_name"), AnimationName) || AnimationName.IsEmpty())
	{
		OutError = TEXT("missing_animation_name");
		return false;
	}

	double StartSeconds = 0.0;
	double DurationSeconds = 1.0;
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

	UWidgetBlueprint* WidgetBlueprint = UeAgentSequenceOps::LoadWidgetBlueprint(AssetPath);
	if (!WidgetBlueprint)
	{
		OutError = TEXT("widget_blueprint_not_found");
		return false;
	}

	UWidgetAnimation* TargetAnimation = UeAgentSequenceOps::FindWidgetAnimationByAnyName(WidgetBlueprint, AnimationName);
	if (!TargetAnimation)
	{
		OutError = TEXT("widget_animation_not_found");
		return false;
	}
	UMovieScene* MovieScene = TargetAnimation->GetMovieScene();
	if (!MovieScene)
	{
		OutError = TEXT("movie_scene_not_found");
		return false;
	}

	TargetAnimation->Modify();
	MovieScene->Modify();
	if (!UeAgentSequenceOps::SetMovieScenePlaybackRangeSeconds(MovieScene, StartSeconds, DurationSeconds))
	{
		OutError = TEXT("set_playback_range_failed");
		return false;
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentSequenceOps::SaveAssetPackage(WidgetBlueprint, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), WidgetBlueprint->GetPathName());
	OutData->SetStringField(TEXT("animation_name"), TargetAnimation->GetName());
	OutData->SetNumberField(TEXT("start_seconds"), StartSeconds);
	OutData->SetNumberField(TEXT("duration_seconds"), DurationSeconds);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdSequenceSetUmgAnimationDisplayRate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString AnimationName;
	if (!JsonTryGetString(Ctx.Params, TEXT("animation_name"), AnimationName) || AnimationName.IsEmpty())
	{
		OutError = TEXT("missing_animation_name");
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

	UWidgetBlueprint* WidgetBlueprint = UeAgentSequenceOps::LoadWidgetBlueprint(AssetPath);
	if (!WidgetBlueprint)
	{
		OutError = TEXT("widget_blueprint_not_found");
		return false;
	}

	UWidgetAnimation* TargetAnimation = UeAgentSequenceOps::FindWidgetAnimationByAnyName(WidgetBlueprint, AnimationName);
	if (!TargetAnimation)
	{
		OutError = TEXT("widget_animation_not_found");
		return false;
	}
	UMovieScene* MovieScene = TargetAnimation->GetMovieScene();
	if (!MovieScene)
	{
		OutError = TEXT("movie_scene_not_found");
		return false;
	}

	TargetAnimation->Modify();
	MovieScene->Modify();
	MovieScene->SetDisplayRate(FFrameRate(DisplayRateNum, DisplayRateDen));
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentSequenceOps::SaveAssetPackage(WidgetBlueprint, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), WidgetBlueprint->GetPathName());
	OutData->SetStringField(TEXT("animation_name"), TargetAnimation->GetName());
	OutData->SetStringField(TEXT("display_rate"), FString::Printf(TEXT("%d/%d"), DisplayRateNum, DisplayRateDen));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdSequenceAddUmgWidgetTransformKey(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString AnimationName;
	if (!JsonTryGetString(Ctx.Params, TEXT("animation_name"), AnimationName) || AnimationName.IsEmpty())
	{
		OutError = TEXT("missing_animation_name");
		return false;
	}

	FString WidgetName;
	if (!JsonTryGetString(Ctx.Params, TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
	{
		OutError = TEXT("missing_widget_name");
		return false;
	}

	double TimeSeconds = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("time_seconds"), TimeSeconds))
	{
		OutError = TEXT("missing_time_seconds");
		return false;
	}

	double TranslationXValue = 0.0;
	double TranslationYValue = 0.0;
	double ScaleXValue = 0.0;
	double ScaleYValue = 0.0;
	double ShearXValue = 0.0;
	double ShearYValue = 0.0;
	double AngleValue = 0.0;

	const bool bHasTranslationX = JsonTryGetNumber(Ctx.Params, TEXT("translation_x"), TranslationXValue);
	const bool bHasTranslationY = JsonTryGetNumber(Ctx.Params, TEXT("translation_y"), TranslationYValue);
	const bool bHasScaleX = JsonTryGetNumber(Ctx.Params, TEXT("scale_x"), ScaleXValue);
	const bool bHasScaleY = JsonTryGetNumber(Ctx.Params, TEXT("scale_y"), ScaleYValue);
	const bool bHasShearX = JsonTryGetNumber(Ctx.Params, TEXT("shear_x"), ShearXValue);
	const bool bHasShearY = JsonTryGetNumber(Ctx.Params, TEXT("shear_y"), ShearYValue);
	const bool bHasAngle = JsonTryGetNumber(Ctx.Params, TEXT("angle"), AngleValue);

	if (!bHasTranslationX && !bHasTranslationY && !bHasScaleX && !bHasScaleY && !bHasShearX && !bHasShearY && !bHasAngle)
	{
		OutError = TEXT("missing_transform_values");
		return false;
	}

	UWidgetBlueprint* WidgetBlueprint = UeAgentSequenceOps::LoadWidgetBlueprint(AssetPath);
	if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
	{
		OutError = TEXT("widget_blueprint_not_found");
		return false;
	}

	UWidgetAnimation* TargetAnimation = UeAgentSequenceOps::FindWidgetAnimationByAnyName(WidgetBlueprint, AnimationName);
	if (!TargetAnimation)
	{
		OutError = TEXT("widget_animation_not_found");
		return false;
	}

	UWidget* TargetWidget = UeAgentSequenceOps::FindWidgetByAnyName(WidgetBlueprint, WidgetName);
	if (!TargetWidget)
	{
		OutError = TEXT("widget_not_found");
		return false;
	}

	UMovieScene* MovieScene = nullptr;
	FGuid BindingGuid;
	bool bBindingAlreadyExists = false;
	if (!UeAgentSequenceOps::FindOrCreateWidgetAnimationBinding(WidgetBlueprint, TargetAnimation, TargetWidget, MovieScene, BindingGuid, bBindingAlreadyExists, OutError))
	{
		return false;
	}

	UMovieScene2DTransformTrack* TransformTrack = nullptr;
	UMovieScene2DTransformSection* TransformSection = nullptr;
	bool bTrackAlreadyExists = false;
	if (!UeAgentSequenceOps::FindOrCreateUmgTransformSection(MovieScene, BindingGuid, TransformTrack, TransformSection, bTrackAlreadyExists, OutError))
	{
		return false;
	}

	const FFrameRate TickResolution = MovieScene->GetTickResolution();
	const FFrameNumber KeyFrame = TickResolution.AsFrameTime(TimeSeconds).RoundToFrame();

	EMovieScene2DTransformChannel ChannelMask = TransformSection->GetMask().GetChannels();
	auto AddMask = [&ChannelMask](const EMovieScene2DTransformChannel Channel)
	{
		ChannelMask = ChannelMask | Channel;
	};

	WidgetBlueprint->Modify();
	TargetAnimation->Modify();
	MovieScene->Modify();
	TransformTrack->Modify();
	TransformSection->Modify();
	TransformSection->SetRange(TRange<FFrameNumber>::All());

	if (bHasTranslationX)
	{
		TransformSection->Translation[0].GetData().UpdateOrAddKey(KeyFrame, FMovieSceneFloatValue(static_cast<float>(TranslationXValue)));
		AddMask(EMovieScene2DTransformChannel::TranslationX);
	}
	if (bHasTranslationY)
	{
		TransformSection->Translation[1].GetData().UpdateOrAddKey(KeyFrame, FMovieSceneFloatValue(static_cast<float>(TranslationYValue)));
		AddMask(EMovieScene2DTransformChannel::TranslationY);
	}
	if (bHasScaleX)
	{
		TransformSection->Scale[0].GetData().UpdateOrAddKey(KeyFrame, FMovieSceneFloatValue(static_cast<float>(ScaleXValue)));
		AddMask(EMovieScene2DTransformChannel::ScaleX);
	}
	if (bHasScaleY)
	{
		TransformSection->Scale[1].GetData().UpdateOrAddKey(KeyFrame, FMovieSceneFloatValue(static_cast<float>(ScaleYValue)));
		AddMask(EMovieScene2DTransformChannel::ScaleY);
	}
	if (bHasShearX)
	{
		TransformSection->Shear[0].GetData().UpdateOrAddKey(KeyFrame, FMovieSceneFloatValue(static_cast<float>(ShearXValue)));
		AddMask(EMovieScene2DTransformChannel::ShearX);
	}
	if (bHasShearY)
	{
		TransformSection->Shear[1].GetData().UpdateOrAddKey(KeyFrame, FMovieSceneFloatValue(static_cast<float>(ShearYValue)));
		AddMask(EMovieScene2DTransformChannel::ShearY);
	}
	if (bHasAngle)
	{
		TransformSection->Rotation.GetData().UpdateOrAddKey(KeyFrame, FMovieSceneFloatValue(static_cast<float>(AngleValue)));
		AddMask(EMovieScene2DTransformChannel::Rotation);
	}

	TransformSection->SetMask(FMovieScene2DTransformMask(ChannelMask));
	WidgetBlueprint->MarkPackageDirty();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);

	bool bCompileAfterSet = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_set"), bCompileAfterSet);
	if (bCompileAfterSet)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
	}

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentSequenceOps::SaveAssetPackage(WidgetBlueprint, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), WidgetBlueprint->GetPathName());
	OutData->SetStringField(TEXT("animation_name"), TargetAnimation->GetName());
	OutData->SetStringField(TEXT("widget_name"), TargetWidget->GetName());
	OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetBoolField(TEXT("binding_already_exists"), bBindingAlreadyExists);
	OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
	OutData->SetNumberField(TEXT("time_seconds"), TimeSeconds);
	OutData->SetNumberField(TEXT("frame_number"), KeyFrame.Value);
	if (bHasTranslationX) OutData->SetNumberField(TEXT("translation_x"), TranslationXValue);
	if (bHasTranslationY) OutData->SetNumberField(TEXT("translation_y"), TranslationYValue);
	if (bHasScaleX) OutData->SetNumberField(TEXT("scale_x"), ScaleXValue);
	if (bHasScaleY) OutData->SetNumberField(TEXT("scale_y"), ScaleYValue);
	if (bHasShearX) OutData->SetNumberField(TEXT("shear_x"), ShearXValue);
	if (bHasShearY) OutData->SetNumberField(TEXT("shear_y"), ShearYValue);
	if (bHasAngle) OutData->SetNumberField(TEXT("angle"), AngleValue);
	OutData->SetBoolField(TEXT("compiled"), bCompileAfterSet);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdSequenceAddUmgWidgetTranslationKey(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString AnimationName;
	if (!JsonTryGetString(Ctx.Params, TEXT("animation_name"), AnimationName) || AnimationName.IsEmpty())
	{
		OutError = TEXT("missing_animation_name");
		return false;
	}

	FString WidgetName;
	if (!JsonTryGetString(Ctx.Params, TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
	{
		OutError = TEXT("missing_widget_name");
		return false;
	}

	double TimeSeconds = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("time_seconds"), TimeSeconds))
	{
		OutError = TEXT("missing_time_seconds");
		return false;
	}

	double TranslationXValue = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("translation_x"), TranslationXValue))
	{
		OutError = TEXT("missing_translation_x");
		return false;
	}

	double TranslationYValue = 0.0;
	const bool bHasTranslationY = JsonTryGetNumber(Ctx.Params, TEXT("translation_y"), TranslationYValue);

	UWidgetBlueprint* WidgetBlueprint = UeAgentSequenceOps::LoadWidgetBlueprint(AssetPath);
	if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
	{
		OutError = TEXT("widget_blueprint_not_found");
		return false;
	}

	UWidgetAnimation* TargetAnimation = UeAgentSequenceOps::FindWidgetAnimationByAnyName(WidgetBlueprint, AnimationName);
	if (!TargetAnimation)
	{
		OutError = TEXT("widget_animation_not_found");
		return false;
	}

	UWidget* TargetWidget = UeAgentSequenceOps::FindWidgetByAnyName(WidgetBlueprint, WidgetName);
	if (!TargetWidget)
	{
		OutError = TEXT("widget_not_found");
		return false;
	}

	UMovieScene* MovieScene = TargetAnimation->GetMovieScene();
	if (!MovieScene)
	{
		OutError = TEXT("movie_scene_not_found");
		return false;
	}

	FGuid BindingGuid;
	bool bBindingAlreadyExists = false;
	for (const FWidgetAnimationBinding& Binding : TargetAnimation->AnimationBindings)
	{
		if (Binding.WidgetName == TargetWidget->GetFName())
		{
			BindingGuid = Binding.AnimationGuid;
			bBindingAlreadyExists = true;
			break;
		}
	}

	const bool bNeedsNewBinding = !BindingGuid.IsValid() || !MovieScene->FindBinding(BindingGuid);
	if (bNeedsNewBinding)
	{
		BindingGuid = MovieScene->AddPossessable(TargetWidget->GetName(), TargetWidget->GetClass());
		if (!BindingGuid.IsValid())
		{
			OutError = TEXT("add_widget_possessable_failed");
			return false;
		}

		FWidgetAnimationBinding NewBinding;
		NewBinding.WidgetName = TargetWidget->GetFName();
		NewBinding.AnimationGuid = BindingGuid;
		NewBinding.bIsRootWidget = (WidgetBlueprint->WidgetTree->RootWidget == TargetWidget);
		TargetAnimation->AnimationBindings.Add(NewBinding);
		bBindingAlreadyExists = false;
	}

	UMovieScene2DTransformTrack* TransformTrack = MovieScene->FindTrack<UMovieScene2DTransformTrack>(BindingGuid);
	const bool bTrackAlreadyExists = TransformTrack != nullptr;
	if (!TransformTrack)
	{
		TransformTrack = MovieScene->AddTrack<UMovieScene2DTransformTrack>(BindingGuid);
		if (!TransformTrack)
		{
			OutError = TEXT("create_umg_transform_track_failed");
			return false;
		}
		TransformTrack->SetPropertyNameAndPath(TEXT("RenderTransform"), TEXT("RenderTransform"));
	}

	UMovieScene2DTransformSection* TransformSection = nullptr;
	const TArray<UMovieSceneSection*>& ExistingSections = TransformTrack->GetAllSections();
	if (ExistingSections.Num() > 0)
	{
		TransformSection = Cast<UMovieScene2DTransformSection>(ExistingSections[0]);
	}
	if (!TransformSection)
	{
		TransformSection = Cast<UMovieScene2DTransformSection>(TransformTrack->CreateNewSection());
		if (!TransformSection)
		{
			OutError = TEXT("create_umg_transform_section_failed");
			return false;
		}
		TransformTrack->AddSection(*TransformSection);
	}

	const FFrameRate TickResolution = MovieScene->GetTickResolution();
	const FFrameNumber KeyFrame = TickResolution.AsFrameTime(TimeSeconds).RoundToFrame();

	WidgetBlueprint->Modify();
	TargetAnimation->Modify();
	MovieScene->Modify();
	TransformTrack->Modify();
	TransformSection->Modify();
	TransformSection->SetRange(TRange<FFrameNumber>::All());

	TransformSection->Translation[0].GetData().UpdateOrAddKey(KeyFrame, FMovieSceneFloatValue(static_cast<float>(TranslationXValue)));
	if (bHasTranslationY)
	{
		TransformSection->Translation[1].GetData().UpdateOrAddKey(KeyFrame, FMovieSceneFloatValue(static_cast<float>(TranslationYValue)));
	}

	WidgetBlueprint->MarkPackageDirty();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);

	bool bCompileAfterSet = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_set"), bCompileAfterSet);
	if (bCompileAfterSet)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
	}

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentSequenceOps::SaveAssetPackage(WidgetBlueprint, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), WidgetBlueprint->GetPathName());
	OutData->SetStringField(TEXT("animation_name"), TargetAnimation->GetName());
	OutData->SetStringField(TEXT("widget_name"), TargetWidget->GetName());
	OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetBoolField(TEXT("binding_already_exists"), bBindingAlreadyExists);
	OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
	OutData->SetNumberField(TEXT("time_seconds"), TimeSeconds);
	OutData->SetNumberField(TEXT("frame_number"), KeyFrame.Value);
	OutData->SetNumberField(TEXT("translation_x"), TranslationXValue);
	if (bHasTranslationY)
	{
		OutData->SetNumberField(TEXT("translation_y"), TranslationYValue);
	}
	OutData->SetBoolField(TEXT("compiled"), bCompileAfterSet);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdSequenceAddUmgWidgetOpacityKey(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString AnimationName;
	if (!JsonTryGetString(Ctx.Params, TEXT("animation_name"), AnimationName) || AnimationName.IsEmpty())
	{
		OutError = TEXT("missing_animation_name");
		return false;
	}

	FString WidgetName;
	if (!JsonTryGetString(Ctx.Params, TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
	{
		OutError = TEXT("missing_widget_name");
		return false;
	}

	double TimeSeconds = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("time_seconds"), TimeSeconds))
	{
		OutError = TEXT("missing_time_seconds");
		return false;
	}

	double OpacityValue = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("opacity"), OpacityValue))
	{
		OutError = TEXT("missing_opacity");
		return false;
	}

	UWidgetBlueprint* WidgetBlueprint = UeAgentSequenceOps::LoadWidgetBlueprint(AssetPath);
	if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
	{
		OutError = TEXT("widget_blueprint_not_found");
		return false;
	}

	UWidgetAnimation* TargetAnimation = UeAgentSequenceOps::FindWidgetAnimationByAnyName(WidgetBlueprint, AnimationName);
	if (!TargetAnimation)
	{
		OutError = TEXT("widget_animation_not_found");
		return false;
	}

	UWidget* TargetWidget = UeAgentSequenceOps::FindWidgetByAnyName(WidgetBlueprint, WidgetName);
	if (!TargetWidget)
	{
		OutError = TEXT("widget_not_found");
		return false;
	}

	UMovieScene* MovieScene = nullptr;
	FGuid BindingGuid;
	bool bBindingAlreadyExists = false;
	if (!UeAgentSequenceOps::FindOrCreateWidgetAnimationBinding(WidgetBlueprint, TargetAnimation, TargetWidget, MovieScene, BindingGuid, bBindingAlreadyExists, OutError))
	{
		return false;
	}

	UMovieSceneFloatTrack* OpacityTrack = nullptr;
	UMovieSceneFloatSection* OpacitySection = nullptr;
	bool bTrackAlreadyExists = false;
	if (!UeAgentSequenceOps::FindOrCreateUmgOpacitySection(MovieScene, BindingGuid, OpacityTrack, OpacitySection, bTrackAlreadyExists, OutError))
	{
		return false;
	}

	const FFrameRate TickResolution = MovieScene->GetTickResolution();
	const FFrameNumber KeyFrame = TickResolution.AsFrameTime(TimeSeconds).RoundToFrame();

	WidgetBlueprint->Modify();
	TargetAnimation->Modify();
	MovieScene->Modify();
	OpacityTrack->Modify();
	OpacitySection->Modify();
	OpacitySection->SetRange(TRange<FFrameNumber>::All());
	OpacitySection->GetChannel().GetData().UpdateOrAddKey(KeyFrame, FMovieSceneFloatValue(static_cast<float>(OpacityValue)));

	WidgetBlueprint->MarkPackageDirty();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);

	bool bCompileAfterSet = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_set"), bCompileAfterSet);
	if (bCompileAfterSet)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
	}

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentSequenceOps::SaveAssetPackage(WidgetBlueprint, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), WidgetBlueprint->GetPathName());
	OutData->SetStringField(TEXT("animation_name"), TargetAnimation->GetName());
	OutData->SetStringField(TEXT("widget_name"), TargetWidget->GetName());
	OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetBoolField(TEXT("binding_already_exists"), bBindingAlreadyExists);
	OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
	OutData->SetNumberField(TEXT("time_seconds"), TimeSeconds);
	OutData->SetNumberField(TEXT("frame_number"), KeyFrame.Value);
	OutData->SetNumberField(TEXT("opacity"), OpacityValue);
	OutData->SetBoolField(TEXT("compiled"), bCompileAfterSet);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdSequenceAddUmgWidgetFloatKey(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString AnimationName;
	if (!JsonTryGetString(Ctx.Params, TEXT("animation_name"), AnimationName) || AnimationName.IsEmpty())
	{
		OutError = TEXT("missing_animation_name");
		return false;
	}

	FString WidgetName;
	if (!JsonTryGetString(Ctx.Params, TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
	{
		OutError = TEXT("missing_widget_name");
		return false;
	}

	double TimeSeconds = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("time_seconds"), TimeSeconds))
	{
		OutError = TEXT("missing_time_seconds");
		return false;
	}

	double FloatValue = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("value"), FloatValue))
	{
		OutError = TEXT("missing_value");
		return false;
	}

	FString PropertyPath = TEXT("RenderOpacity");
	JsonTryGetString(Ctx.Params, TEXT("property_path"), PropertyPath);
	PropertyPath = PropertyPath.TrimStartAndEnd();
	if (PropertyPath.IsEmpty())
	{
		PropertyPath = TEXT("RenderOpacity");
	}

	FString PropertyNameText = PropertyPath;
	JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyNameText);
	PropertyNameText = PropertyNameText.TrimStartAndEnd();
	if (PropertyNameText.IsEmpty())
	{
		PropertyNameText = PropertyPath;
	}

	UWidgetBlueprint* WidgetBlueprint = UeAgentSequenceOps::LoadWidgetBlueprint(AssetPath);
	if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
	{
		OutError = TEXT("widget_blueprint_not_found");
		return false;
	}

	UWidgetAnimation* TargetAnimation = UeAgentSequenceOps::FindWidgetAnimationByAnyName(WidgetBlueprint, AnimationName);
	if (!TargetAnimation)
	{
		OutError = TEXT("widget_animation_not_found");
		return false;
	}

	UWidget* TargetWidget = UeAgentSequenceOps::FindWidgetByAnyName(WidgetBlueprint, WidgetName);
	if (!TargetWidget)
	{
		OutError = TEXT("widget_not_found");
		return false;
	}

	UMovieScene* MovieScene = nullptr;
	FGuid BindingGuid;
	bool bBindingAlreadyExists = false;
	if (!UeAgentSequenceOps::FindOrCreateWidgetAnimationBinding(WidgetBlueprint, TargetAnimation, TargetWidget, MovieScene, BindingGuid, bBindingAlreadyExists, OutError))
	{
		return false;
	}

	UMovieSceneFloatTrack* FloatTrack = nullptr;
	UMovieSceneFloatSection* FloatSection = nullptr;
	bool bTrackAlreadyExists = false;
	if (!UeAgentSequenceOps::FindOrCreateUmgFloatSection(MovieScene, BindingGuid, FName(*PropertyNameText), PropertyPath, FloatTrack, FloatSection, bTrackAlreadyExists, OutError))
	{
		return false;
	}

	const FFrameRate TickResolution = MovieScene->GetTickResolution();
	const FFrameNumber KeyFrame = TickResolution.AsFrameTime(TimeSeconds).RoundToFrame();

	WidgetBlueprint->Modify();
	TargetAnimation->Modify();
	MovieScene->Modify();
	FloatTrack->Modify();
	FloatSection->Modify();
	FloatSection->SetRange(TRange<FFrameNumber>::All());
	FloatSection->GetChannel().GetData().UpdateOrAddKey(KeyFrame, FMovieSceneFloatValue(static_cast<float>(FloatValue)));

	WidgetBlueprint->MarkPackageDirty();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);

	bool bCompileAfterSet = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_set"), bCompileAfterSet);
	if (bCompileAfterSet)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
	}

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentSequenceOps::SaveAssetPackage(WidgetBlueprint, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), WidgetBlueprint->GetPathName());
	OutData->SetStringField(TEXT("animation_name"), TargetAnimation->GetName());
	OutData->SetStringField(TEXT("widget_name"), TargetWidget->GetName());
	OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetBoolField(TEXT("binding_already_exists"), bBindingAlreadyExists);
	OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
	OutData->SetStringField(TEXT("property_name"), PropertyNameText);
	OutData->SetStringField(TEXT("property_path"), PropertyPath);
	OutData->SetNumberField(TEXT("time_seconds"), TimeSeconds);
	OutData->SetNumberField(TEXT("frame_number"), KeyFrame.Value);
	OutData->SetNumberField(TEXT("value"), FloatValue);
	OutData->SetBoolField(TEXT("compiled"), bCompileAfterSet);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdSequenceAddUmgWidgetColorKey(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString AnimationName;
	if (!JsonTryGetString(Ctx.Params, TEXT("animation_name"), AnimationName) || AnimationName.IsEmpty())
	{
		OutError = TEXT("missing_animation_name");
		return false;
	}

	FString WidgetName;
	if (!JsonTryGetString(Ctx.Params, TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
	{
		OutError = TEXT("missing_widget_name");
		return false;
	}

	double TimeSeconds = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("time_seconds"), TimeSeconds))
	{
		OutError = TEXT("missing_time_seconds");
		return false;
	}

	double RedValue = 0.0;
	double GreenValue = 0.0;
	double BlueValue = 0.0;
	double AlphaValue = 1.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("red"), RedValue))
	{
		OutError = TEXT("missing_red");
		return false;
	}
	if (!JsonTryGetNumber(Ctx.Params, TEXT("green"), GreenValue))
	{
		OutError = TEXT("missing_green");
		return false;
	}
	if (!JsonTryGetNumber(Ctx.Params, TEXT("blue"), BlueValue))
	{
		OutError = TEXT("missing_blue");
		return false;
	}
	JsonTryGetNumber(Ctx.Params, TEXT("alpha"), AlphaValue);

	FString PropertyPath = TEXT("ColorAndOpacity");
	JsonTryGetString(Ctx.Params, TEXT("property_path"), PropertyPath);
	PropertyPath = PropertyPath.TrimStartAndEnd();
	if (PropertyPath.IsEmpty())
	{
		PropertyPath = TEXT("ColorAndOpacity");
	}

	FString PropertyNameText = PropertyPath;
	JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyNameText);
	PropertyNameText = PropertyNameText.TrimStartAndEnd();
	if (PropertyNameText.IsEmpty())
	{
		PropertyNameText = PropertyPath;
	}

	UWidgetBlueprint* WidgetBlueprint = UeAgentSequenceOps::LoadWidgetBlueprint(AssetPath);
	if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
	{
		OutError = TEXT("widget_blueprint_not_found");
		return false;
	}

	UWidgetAnimation* TargetAnimation = UeAgentSequenceOps::FindWidgetAnimationByAnyName(WidgetBlueprint, AnimationName);
	if (!TargetAnimation)
	{
		OutError = TEXT("widget_animation_not_found");
		return false;
	}

	UWidget* TargetWidget = UeAgentSequenceOps::FindWidgetByAnyName(WidgetBlueprint, WidgetName);
	if (!TargetWidget)
	{
		OutError = TEXT("widget_not_found");
		return false;
	}

	UMovieScene* MovieScene = nullptr;
	FGuid BindingGuid;
	bool bBindingAlreadyExists = false;
	if (!UeAgentSequenceOps::FindOrCreateWidgetAnimationBinding(WidgetBlueprint, TargetAnimation, TargetWidget, MovieScene, BindingGuid, bBindingAlreadyExists, OutError))
	{
		return false;
	}

	UMovieSceneColorTrack* ColorTrack = nullptr;
	UMovieSceneColorSection* ColorSection = nullptr;
	bool bTrackAlreadyExists = false;
	if (!UeAgentSequenceOps::FindOrCreateUmgColorSection(MovieScene, BindingGuid, FName(*PropertyNameText), PropertyPath, ColorTrack, ColorSection, bTrackAlreadyExists, OutError))
	{
		return false;
	}

	const FFrameRate TickResolution = MovieScene->GetTickResolution();
	const FFrameNumber KeyFrame = TickResolution.AsFrameTime(TimeSeconds).RoundToFrame();

	WidgetBlueprint->Modify();
	TargetAnimation->Modify();
	MovieScene->Modify();
	ColorTrack->Modify();
	ColorSection->Modify();
	ColorSection->SetRange(TRange<FFrameNumber>::All());
	ColorSection->GetRedChannel().GetData().UpdateOrAddKey(KeyFrame, FMovieSceneFloatValue(static_cast<float>(RedValue)));
	ColorSection->GetGreenChannel().GetData().UpdateOrAddKey(KeyFrame, FMovieSceneFloatValue(static_cast<float>(GreenValue)));
	ColorSection->GetBlueChannel().GetData().UpdateOrAddKey(KeyFrame, FMovieSceneFloatValue(static_cast<float>(BlueValue)));
	ColorSection->GetAlphaChannel().GetData().UpdateOrAddKey(KeyFrame, FMovieSceneFloatValue(static_cast<float>(AlphaValue)));

	WidgetBlueprint->MarkPackageDirty();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);

	bool bCompileAfterSet = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_set"), bCompileAfterSet);
	if (bCompileAfterSet)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
	}

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentSequenceOps::SaveAssetPackage(WidgetBlueprint, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), WidgetBlueprint->GetPathName());
	OutData->SetStringField(TEXT("animation_name"), TargetAnimation->GetName());
	OutData->SetStringField(TEXT("widget_name"), TargetWidget->GetName());
	OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetBoolField(TEXT("binding_already_exists"), bBindingAlreadyExists);
	OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
	OutData->SetStringField(TEXT("property_name"), PropertyNameText);
	OutData->SetStringField(TEXT("property_path"), PropertyPath);
	OutData->SetNumberField(TEXT("time_seconds"), TimeSeconds);
	OutData->SetNumberField(TEXT("frame_number"), KeyFrame.Value);
	OutData->SetNumberField(TEXT("red"), RedValue);
	OutData->SetNumberField(TEXT("green"), GreenValue);
	OutData->SetNumberField(TEXT("blue"), BlueValue);
	OutData->SetNumberField(TEXT("alpha"), AlphaValue);
	OutData->SetBoolField(TEXT("compiled"), bCompileAfterSet);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}
