bool FUeAgentHttpServer::CmdSequenceAddActorBinding(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString ActorId;
	if (!JsonTryGetString(Ctx.Params, TEXT("actor_id"), ActorId) || ActorId.IsEmpty())
	{
		OutError = TEXT("missing_actor_id");
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

	FString WorldError;
	UWorld* World = GetEditorWorld(WorldError);
	if (!World)
	{
		OutError = WorldError.IsEmpty() ? TEXT("editor_world_not_found") : WorldError;
		return false;
	}

	AActor* Actor = FindActorByNameOrLabel(World, ActorId);
	if (!Actor)
	{
		OutError = TEXT("actor_not_found");
		return false;
	}

	Sequence->Modify();
	MovieScene->Modify();

	FGuid BindingGuid;
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	BindingGuid = Sequence->FindBindingFromObject(Actor, World);
	PRAGMA_ENABLE_DEPRECATION_WARNINGS

	const bool bAlreadyBound = BindingGuid.IsValid();
	if (!BindingGuid.IsValid())
	{
		const FString BindingName = Actor->GetActorLabel().IsEmpty() ? Actor->GetName() : Actor->GetActorLabel();
		BindingGuid = MovieScene->AddPossessable(BindingName, Actor->GetClass());
		if (!BindingGuid.IsValid())
		{
			OutError = TEXT("add_possessable_failed");
			return false;
		}
		Sequence->BindPossessableObject(BindingGuid, *Actor, World);
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
	OutData->SetStringField(TEXT("actor_name"), Actor->GetName());
	OutData->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
	OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetBoolField(TEXT("already_bound"), bAlreadyBound);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdSequenceRemoveActorBinding(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString BindingGuidText;
	if (!JsonTryGetString(Ctx.Params, TEXT("binding_guid"), BindingGuidText) || BindingGuidText.IsEmpty())
	{
		OutError = TEXT("missing_binding_guid");
		return false;
	}
	FGuid BindingGuid;
	if (!FGuid::Parse(BindingGuidText, BindingGuid))
	{
		OutError = TEXT("invalid_binding_guid");
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
	if (!MovieScene->FindBinding(BindingGuid))
	{
		OutError = TEXT("binding_not_found");
		return false;
	}

	Sequence->Modify();
	MovieScene->Modify();

	bool bRemoved = false;
	if (MovieScene->FindPossessable(BindingGuid))
	{
		MovieScene->RemovePossessable(BindingGuid);
		bRemoved = true;
	}
	else if (MovieScene->FindSpawnable(BindingGuid))
	{
		MovieScene->RemoveSpawnable(BindingGuid);
		bRemoved = true;
	}
	if (!bRemoved)
	{
		OutError = TEXT("remove_binding_failed");
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
	OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdSequenceAddTransformTrack(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString BindingGuidText;
	if (!JsonTryGetString(Ctx.Params, TEXT("binding_guid"), BindingGuidText) || BindingGuidText.IsEmpty())
	{
		OutError = TEXT("missing_binding_guid");
		return false;
	}
	FGuid BindingGuid;
	if (!FGuid::Parse(BindingGuidText, BindingGuid))
	{
		OutError = TEXT("invalid_binding_guid");
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
	if (!MovieScene->FindBinding(BindingGuid))
	{
		OutError = TEXT("binding_not_found");
		return false;
	}

	Sequence->Modify();
	MovieScene->Modify();

	UMovieScene3DTransformTrack* TransformTrack = MovieScene->FindTrack<UMovieScene3DTransformTrack>(BindingGuid);
	const bool bTrackAlreadyExists = TransformTrack != nullptr;
	if (!TransformTrack)
	{
		TransformTrack = MovieScene->AddTrack<UMovieScene3DTransformTrack>(BindingGuid);
	}
	if (!TransformTrack)
	{
		OutError = TEXT("create_transform_track_failed");
		return false;
	}

	UMovieScene3DTransformSection* TransformSection = nullptr;
	const TArray<UMovieSceneSection*>& ExistingSections = TransformTrack->GetAllSections();
	if (ExistingSections.Num() > 0)
	{
		TransformSection = Cast<UMovieScene3DTransformSection>(ExistingSections[0]);
	}
	if (!TransformSection)
	{
		TransformSection = Cast<UMovieScene3DTransformSection>(TransformTrack->CreateNewSection());
		if (!TransformSection)
		{
			OutError = TEXT("create_transform_section_failed");
			return false;
		}
		TransformTrack->AddSection(*TransformSection);
	}
	TransformSection->SetRange(TRange<FFrameNumber>::All());

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
	OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdSequenceAddVisibilityTrack(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString BindingGuidText;
	if (!JsonTryGetString(Ctx.Params, TEXT("binding_guid"), BindingGuidText) || BindingGuidText.IsEmpty())
	{
		OutError = TEXT("missing_binding_guid");
		return false;
	}
	FGuid BindingGuid;
	if (!FGuid::Parse(BindingGuidText, BindingGuid))
	{
		OutError = TEXT("invalid_binding_guid");
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
	if (!MovieScene->FindBinding(BindingGuid))
	{
		OutError = TEXT("binding_not_found");
		return false;
	}

	Sequence->Modify();
	MovieScene->Modify();

	const FName PropertyName(TEXT("bHidden"));
	UMovieSceneVisibilityTrack* VisibilityTrack = MovieScene->FindTrack<UMovieSceneVisibilityTrack>(BindingGuid, PropertyName);
	const bool bTrackAlreadyExists = VisibilityTrack != nullptr;
	if (!VisibilityTrack)
	{
		VisibilityTrack = MovieScene->AddTrack<UMovieSceneVisibilityTrack>(BindingGuid);
	}
	if (!VisibilityTrack)
	{
		OutError = TEXT("create_visibility_track_failed");
		return false;
	}
	VisibilityTrack->SetPropertyNameAndPath(PropertyName, PropertyName.ToString());

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
	OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("property_name"), PropertyName.ToString());
	OutData->SetStringField(TEXT("property_path"), PropertyName.ToString());
	OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdSequenceAddVisibilityKey(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString BindingGuidText;
	if (!JsonTryGetString(Ctx.Params, TEXT("binding_guid"), BindingGuidText) || BindingGuidText.IsEmpty())
	{
		OutError = TEXT("missing_binding_guid");
		return false;
	}
	FGuid BindingGuid;
	if (!FGuid::Parse(BindingGuidText, BindingGuid))
	{
		OutError = TEXT("invalid_binding_guid");
		return false;
	}

	double TimeSeconds = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("time_seconds"), TimeSeconds))
	{
		OutError = TEXT("missing_time_seconds");
		return false;
	}

	bool bVisible = true;
	if (!JsonTryGetBool(Ctx.Params, TEXT("visible"), bVisible))
	{
		OutError = TEXT("missing_visible");
		return false;
	}

	bool bVisibleBeforeKey = true;
	JsonTryGetBool(Ctx.Params, TEXT("visible_before_key"), bVisibleBeforeKey);

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
	if (!MovieScene->FindBinding(BindingGuid))
	{
		OutError = TEXT("binding_not_found");
		return false;
	}

	Sequence->Modify();
	MovieScene->Modify();

	const FName PropertyName(TEXT("bHidden"));
	UMovieSceneVisibilityTrack* VisibilityTrack = MovieScene->FindTrack<UMovieSceneVisibilityTrack>(BindingGuid, PropertyName);
	const bool bTrackAlreadyExists = VisibilityTrack != nullptr;
	if (!VisibilityTrack)
	{
		VisibilityTrack = MovieScene->AddTrack<UMovieSceneVisibilityTrack>(BindingGuid);
	}
	if (!VisibilityTrack)
	{
		OutError = TEXT("create_visibility_track_failed");
		return false;
	}
	VisibilityTrack->SetPropertyNameAndPath(PropertyName, PropertyName.ToString());

	UMovieSceneVisibilitySection* VisibilitySection = nullptr;
	const TArray<UMovieSceneSection*>& ExistingSections = VisibilityTrack->GetAllSections();
	if (ExistingSections.Num() > 0)
	{
		VisibilitySection = Cast<UMovieSceneVisibilitySection>(ExistingSections[0]);
	}
	if (!VisibilitySection)
	{
		VisibilitySection = Cast<UMovieSceneVisibilitySection>(VisibilityTrack->CreateNewSection());
		if (!VisibilitySection)
		{
			OutError = TEXT("create_visibility_section_failed");
			return false;
		}
		VisibilityTrack->AddSection(*VisibilitySection);
	}

	const FFrameRate TickResolution = MovieScene->GetTickResolution();
	const FFrameNumber KeyFrame = TickResolution.AsFrameTime(TimeSeconds).RoundToFrame();
	const FFrameNumber PlaybackStartFrame = MovieScene->GetPlaybackRange().GetLowerBoundValue();

	VisibilityTrack->Modify();
	VisibilitySection->Modify();
	VisibilitySection->SetRange(TRange<FFrameNumber>::All());
	FMovieSceneBoolChannel& Channel = VisibilitySection->GetChannel();
	if (Channel.GetData().GetTimes().Num() == 0 && KeyFrame > PlaybackStartFrame)
	{
		Channel.GetData().AddKey(PlaybackStartFrame, bVisibleBeforeKey);
	}
	Channel.GetData().UpdateOrAddKey(KeyFrame, bVisible);

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
	OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("property_name"), PropertyName.ToString());
	OutData->SetStringField(TEXT("property_path"), PropertyName.ToString());
	OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
	OutData->SetNumberField(TEXT("time_seconds"), TimeSeconds);
	OutData->SetNumberField(TEXT("frame_number"), KeyFrame.Value);
	OutData->SetBoolField(TEXT("visible"), bVisible);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdSequenceAddPropertyTrack(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString PropertyTypeText;
	if (!JsonTryGetString(Ctx.Params, TEXT("property_type"), PropertyTypeText) || PropertyTypeText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_property_type");
		return false;
	}

	UeAgentSequenceOps::ESequencePropertyTrackType PropertyType;
	if (!UeAgentSequenceOps::ParseSequencePropertyTrackType(PropertyTypeText, PropertyType))
	{
		OutError = TEXT("invalid_property_type");
		return false;
	}

	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::Bool)
	{
		return CmdSequenceAddBoolPropertyTrack(Ctx, OutData, OutError);
	}
	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::Byte)
	{
		// handled below through unified byte path
	}
	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::String)
	{
		// handled below through unified string path
	}
	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::Vector)
	{
		// handled below through unified vector path
	}
	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::Rotator)
	{
		// handled below through unified rotator path
	}
	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::ActorReference)
	{
		// handled below through unified actor reference path
	}
	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::Float)
	{
		return CmdSequenceAddFloatPropertyTrack(Ctx, OutData, OutError);
	}
	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::Integer)
	{
		return CmdSequenceAddIntegerPropertyTrack(Ctx, OutData, OutError);
	}

	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString BindingGuidText;
	if (!JsonTryGetString(Ctx.Params, TEXT("binding_guid"), BindingGuidText) || BindingGuidText.IsEmpty())
	{
		OutError = TEXT("missing_binding_guid");
		return false;
	}
	FGuid BindingGuid;
	if (!FGuid::Parse(BindingGuidText, BindingGuid))
	{
		OutError = TEXT("invalid_binding_guid");
		return false;
	}

	FString PropertyNameText;
	if (!JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyNameText) || PropertyNameText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_property_name");
		return false;
	}
	PropertyNameText = PropertyNameText.TrimStartAndEnd();

	FString PropertyPathText;
	JsonTryGetString(Ctx.Params, TEXT("property_path"), PropertyPathText);
	PropertyPathText = PropertyPathText.TrimStartAndEnd();
	if (PropertyPathText.IsEmpty())
	{
		PropertyPathText = PropertyNameText;
	}

	auto ResolveObjectPropertyClass = [&](UClass*& OutClass) -> bool
	{
		OutClass = nullptr;

		FString PropertyClassPath;
		JsonTryGetString(Ctx.Params, TEXT("property_class_path"), PropertyClassPath);
		PropertyClassPath = PropertyClassPath.TrimStartAndEnd();
		if (!PropertyClassPath.IsEmpty())
		{
			OutClass = LoadObject<UClass>(nullptr, *PropertyClassPath);
			if (!OutClass && !PropertyClassPath.EndsWith(TEXT("_C")))
			{
				OutClass = LoadObject<UClass>(nullptr, *(PropertyClassPath + TEXT("_C")));
			}
			if (!OutClass)
			{
				OutError = TEXT("property_class_not_found");
				return false;
			}
			return true;
		}

		if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::Object)
		{
			OutError = TEXT("missing_property_class_path");
			return false;
		}

		return true;
	};

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
	if (!MovieScene->FindBinding(BindingGuid))
	{
		OutError = TEXT("binding_not_found");
		return false;
	}

	Sequence->Modify();
	MovieScene->Modify();

	const FName PropertyName(*PropertyNameText);
	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::Vector)
	{
		auto ResolveVectorTrackSettings = [&](UeAgentSequenceOps::ESequenceVectorPrecision& OutPrecision, int32& OutChannels) -> bool
		{
			bool bHasExplicitPrecision = false;
			UeAgentSequenceOps::ESequenceVectorPrecision ExplicitPrecision = UeAgentSequenceOps::ESequenceVectorPrecision::Double;
			FString VectorPrecisionText;
			if (JsonTryGetString(Ctx.Params, TEXT("vector_precision"), VectorPrecisionText) && !VectorPrecisionText.TrimStartAndEnd().IsEmpty())
			{
				if (!UeAgentSequenceOps::ParseSequenceVectorPrecision(VectorPrecisionText, ExplicitPrecision))
				{
					OutError = TEXT("invalid_vector_precision");
					return false;
				}
				bHasExplicitPrecision = true;
			}

			bool bHasExplicitChannels = false;
			int32 ExplicitChannels = 3;
			double ChannelsUsedNumber = 0.0;
			if (JsonTryGetNumber(Ctx.Params, TEXT("channels_used"), ChannelsUsedNumber))
			{
				ExplicitChannels = static_cast<int32>(ChannelsUsedNumber);
				if (ExplicitChannels < 2 || ExplicitChannels > 4)
				{
					OutError = TEXT("invalid_channels_used");
					return false;
				}
				bHasExplicitChannels = true;
			}

			bool bHasInferredPrecision = false;
			UeAgentSequenceOps::ESequenceVectorPrecision InferredPrecision = UeAgentSequenceOps::ESequenceVectorPrecision::Double;
			bool bHasInferredChannels = false;
			int32 InferredChannels = 3;
			UeAgentSequenceOps::InferSequenceVectorDefaultsFromTypeText(PropertyTypeText, bHasInferredPrecision, InferredPrecision, bHasInferredChannels, InferredChannels);

			UMovieSceneFloatVectorTrack* ExistingFloatTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneFloatVectorTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
			UMovieSceneDoubleVectorTrack* ExistingDoubleTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneDoubleVectorTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
			if (ExistingFloatTrack && ExistingDoubleTrack)
			{
				OutError = TEXT("multiple_vector_tracks_for_property");
				return false;
			}

			if (bHasExplicitPrecision)
			{
				OutPrecision = ExplicitPrecision;
			}
			else if (ExistingFloatTrack)
			{
				OutPrecision = UeAgentSequenceOps::ESequenceVectorPrecision::Float;
			}
			else if (ExistingDoubleTrack)
			{
				OutPrecision = UeAgentSequenceOps::ESequenceVectorPrecision::Double;
			}
			else if (bHasInferredPrecision)
			{
				OutPrecision = InferredPrecision;
			}
			else
			{
				OutPrecision = UeAgentSequenceOps::ESequenceVectorPrecision::Double;
			}

			if (bHasExplicitChannels)
			{
				OutChannels = ExplicitChannels;
			}
			else if (ExistingFloatTrack)
			{
				OutChannels = FMath::Clamp(ExistingFloatTrack->GetNumChannelsUsed(), 2, 4);
			}
			else if (ExistingDoubleTrack)
			{
				OutChannels = FMath::Clamp(ExistingDoubleTrack->GetNumChannelsUsed(), 2, 4);
			}
			else if (bHasInferredChannels)
			{
				OutChannels = InferredChannels;
			}
			else
			{
				OutChannels = 3;
			}

			if (ExistingFloatTrack && OutPrecision != UeAgentSequenceOps::ESequenceVectorPrecision::Float)
			{
				OutError = TEXT("vector_track_precision_mismatch");
				return false;
			}
			if (ExistingDoubleTrack && OutPrecision != UeAgentSequenceOps::ESequenceVectorPrecision::Double)
			{
				OutError = TEXT("vector_track_precision_mismatch");
				return false;
			}
			if (ExistingFloatTrack && ExistingFloatTrack->GetNumChannelsUsed() != OutChannels)
			{
				OutError = TEXT("vector_track_channels_mismatch");
				return false;
			}
			if (ExistingDoubleTrack && ExistingDoubleTrack->GetNumChannelsUsed() != OutChannels)
			{
				OutError = TEXT("vector_track_channels_mismatch");
				return false;
			}

			return true;
		};

		UeAgentSequenceOps::ESequenceVectorPrecision VectorPrecision;
		int32 ChannelsUsed = 3;
		if (!ResolveVectorTrackSettings(VectorPrecision, ChannelsUsed))
		{
			return false;
		}

		if (VectorPrecision == UeAgentSequenceOps::ESequenceVectorPrecision::Float)
		{
			UMovieSceneFloatVectorTrack* VectorTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneFloatVectorTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
			const bool bTrackAlreadyExists = VectorTrack != nullptr;
			if (!VectorTrack)
			{
				VectorTrack = MovieScene->AddTrack<UMovieSceneFloatVectorTrack>(BindingGuid);
			}
			if (!VectorTrack)
			{
				OutError = TEXT("create_float_vector_property_track_failed");
				return false;
			}
			VectorTrack->SetPropertyNameAndPath(PropertyName, PropertyPathText);
			VectorTrack->SetNumChannelsUsed(ChannelsUsed);

			UMovieSceneFloatVectorSection* VectorSection = nullptr;
			const TArray<UMovieSceneSection*>& ExistingSections = VectorTrack->GetAllSections();
			if (ExistingSections.Num() > 0)
			{
				VectorSection = Cast<UMovieSceneFloatVectorSection>(ExistingSections[0]);
			}
			if (!VectorSection)
			{
				VectorSection = Cast<UMovieSceneFloatVectorSection>(VectorTrack->CreateNewSection());
				if (!VectorSection)
				{
					OutError = TEXT("create_float_vector_property_section_failed");
					return false;
				}
				VectorTrack->AddSection(*VectorSection);
			}
			VectorSection->SetChannelsUsed(ChannelsUsed);
			VectorSection->SetRange(TRange<FFrameNumber>::All());

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
			OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
			OutData->SetStringField(TEXT("property_type"), TEXT("vector"));
			OutData->SetStringField(TEXT("vector_precision"), UeAgentSequenceOps::LexToString(VectorPrecision));
			OutData->SetNumberField(TEXT("channels_used"), ChannelsUsed);
			OutData->SetStringField(TEXT("property_name"), PropertyName.ToString());
			OutData->SetStringField(TEXT("property_path"), PropertyPathText);
			OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
			OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
			return true;
		}

		UMovieSceneDoubleVectorTrack* VectorTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneDoubleVectorTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
		const bool bTrackAlreadyExists = VectorTrack != nullptr;
		if (!VectorTrack)
		{
			VectorTrack = MovieScene->AddTrack<UMovieSceneDoubleVectorTrack>(BindingGuid);
		}
		if (!VectorTrack)
		{
			OutError = TEXT("create_double_vector_property_track_failed");
			return false;
		}
		VectorTrack->SetPropertyNameAndPath(PropertyName, PropertyPathText);
		VectorTrack->SetNumChannelsUsed(ChannelsUsed);

		UMovieSceneDoubleVectorSection* VectorSection = nullptr;
		const TArray<UMovieSceneSection*>& ExistingSections = VectorTrack->GetAllSections();
		if (ExistingSections.Num() > 0)
		{
			VectorSection = Cast<UMovieSceneDoubleVectorSection>(ExistingSections[0]);
		}
		if (!VectorSection)
		{
			VectorSection = Cast<UMovieSceneDoubleVectorSection>(VectorTrack->CreateNewSection());
			if (!VectorSection)
			{
				OutError = TEXT("create_double_vector_property_section_failed");
				return false;
			}
			VectorTrack->AddSection(*VectorSection);
		}
		VectorSection->SetChannelsUsed(ChannelsUsed);
		VectorSection->SetRange(TRange<FFrameNumber>::All());

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
		OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetStringField(TEXT("property_type"), TEXT("vector"));
		OutData->SetStringField(TEXT("vector_precision"), UeAgentSequenceOps::LexToString(VectorPrecision));
		OutData->SetNumberField(TEXT("channels_used"), ChannelsUsed);
		OutData->SetStringField(TEXT("property_name"), PropertyName.ToString());
		OutData->SetStringField(TEXT("property_path"), PropertyPathText);
		OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
		OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
		return true;
	}
	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::Rotator)
	{
		UMovieSceneRotatorTrack* RotatorTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneRotatorTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
		const bool bTrackAlreadyExists = RotatorTrack != nullptr;
		if (!RotatorTrack)
		{
			RotatorTrack = MovieScene->AddTrack<UMovieSceneRotatorTrack>(BindingGuid);
		}
		if (!RotatorTrack)
		{
			OutError = TEXT("create_rotator_property_track_failed");
			return false;
		}
		RotatorTrack->SetPropertyNameAndPath(PropertyName, PropertyPathText);

		UMovieSceneRotatorSection* RotatorSection = nullptr;
		const TArray<UMovieSceneSection*>& ExistingSections = RotatorTrack->GetAllSections();
		if (ExistingSections.Num() > 0)
		{
			RotatorSection = Cast<UMovieSceneRotatorSection>(ExistingSections[0]);
		}
		if (!RotatorSection)
		{
			RotatorSection = Cast<UMovieSceneRotatorSection>(RotatorTrack->CreateNewSection());
			if (!RotatorSection)
			{
				OutError = TEXT("create_rotator_property_section_failed");
				return false;
			}
			RotatorTrack->AddSection(*RotatorSection);
		}
		RotatorSection->SetRange(TRange<FFrameNumber>::All());

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
		OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetStringField(TEXT("property_type"), TEXT("rotator"));
		OutData->SetStringField(TEXT("property_name"), PropertyName.ToString());
		OutData->SetStringField(TEXT("property_path"), PropertyPathText);
		OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
		OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
		return true;
	}
	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::ActorReference)
	{
		UMovieSceneActorReferenceTrack* ActorReferenceTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneActorReferenceTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
		const bool bTrackAlreadyExists = ActorReferenceTrack != nullptr;
		if (!ActorReferenceTrack)
		{
			ActorReferenceTrack = MovieScene->AddTrack<UMovieSceneActorReferenceTrack>(BindingGuid);
		}
		if (!ActorReferenceTrack)
		{
			OutError = TEXT("create_actor_reference_property_track_failed");
			return false;
		}
		ActorReferenceTrack->SetPropertyNameAndPath(PropertyName, PropertyPathText);

		UMovieSceneActorReferenceSection* ActorReferenceSection = nullptr;
		const TArray<UMovieSceneSection*>& ExistingSections = ActorReferenceTrack->GetAllSections();
		if (ExistingSections.Num() > 0)
		{
			ActorReferenceSection = Cast<UMovieSceneActorReferenceSection>(ExistingSections[0]);
		}
		if (!ActorReferenceSection)
		{
			ActorReferenceSection = Cast<UMovieSceneActorReferenceSection>(ActorReferenceTrack->CreateNewSection());
			if (!ActorReferenceSection)
			{
				OutError = TEXT("create_actor_reference_property_section_failed");
				return false;
			}
			ActorReferenceTrack->AddSection(*ActorReferenceSection);
		}
		ActorReferenceSection->SetRange(TRange<FFrameNumber>::All());

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
		OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetStringField(TEXT("property_type"), TEXT("actor_reference"));
		OutData->SetStringField(TEXT("property_name"), PropertyName.ToString());
		OutData->SetStringField(TEXT("property_path"), PropertyPathText);
		OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
		OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
		return true;
	}
	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::Byte)
	{
		UEnum* ByteEnum = nullptr;
		FString EnumPath;
		JsonTryGetString(Ctx.Params, TEXT("enum_path"), EnumPath);
		EnumPath = EnumPath.TrimStartAndEnd();
		if (!EnumPath.IsEmpty())
		{
			ByteEnum = LoadObject<UEnum>(nullptr, *EnumPath);
			if (!ByteEnum)
			{
				OutError = TEXT("enum_not_found");
				return false;
			}
		}

		UMovieSceneByteTrack* ByteTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneByteTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
		const bool bTrackAlreadyExists = ByteTrack != nullptr;
		if (!ByteTrack)
		{
			ByteTrack = MovieScene->AddTrack<UMovieSceneByteTrack>(BindingGuid);
		}
		if (!ByteTrack)
		{
			OutError = TEXT("create_byte_property_track_failed");
			return false;
		}
		ByteTrack->SetPropertyNameAndPath(PropertyName, PropertyPathText);
		if (ByteEnum)
		{
			ByteTrack->SetEnum(ByteEnum);
		}

		UMovieSceneByteSection* ByteSection = nullptr;
		const TArray<UMovieSceneSection*>& ExistingSections = ByteTrack->GetAllSections();
		if (ExistingSections.Num() > 0)
		{
			ByteSection = Cast<UMovieSceneByteSection>(ExistingSections[0]);
		}
		if (!ByteSection)
		{
			ByteSection = Cast<UMovieSceneByteSection>(ByteTrack->CreateNewSection());
			if (!ByteSection)
			{
				OutError = TEXT("create_byte_property_section_failed");
				return false;
			}
			ByteTrack->AddSection(*ByteSection);
		}
		ByteSection->SetRange(TRange<FFrameNumber>::All());

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
		OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetStringField(TEXT("property_type"), TEXT("byte"));
		OutData->SetStringField(TEXT("property_name"), PropertyName.ToString());
		OutData->SetStringField(TEXT("property_path"), PropertyPathText);
		OutData->SetStringField(TEXT("enum_path"), ByteTrack->GetEnum() ? ByteTrack->GetEnum()->GetPathName() : TEXT(""));
		OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
		OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
		return true;
	}
	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::String)
	{
		UMovieSceneStringTrack* StringTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneStringTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
		const bool bTrackAlreadyExists = StringTrack != nullptr;
		if (!StringTrack)
		{
			StringTrack = MovieScene->AddTrack<UMovieSceneStringTrack>(BindingGuid);
		}
		if (!StringTrack)
		{
			OutError = TEXT("create_string_property_track_failed");
			return false;
		}
		StringTrack->SetPropertyNameAndPath(PropertyName, PropertyPathText);

		UMovieSceneStringSection* StringSection = nullptr;
		const TArray<UMovieSceneSection*>& ExistingSections = StringTrack->GetAllSections();
		if (ExistingSections.Num() > 0)
		{
			StringSection = Cast<UMovieSceneStringSection>(ExistingSections[0]);
		}
		if (!StringSection)
		{
			StringSection = Cast<UMovieSceneStringSection>(StringTrack->CreateNewSection());
			if (!StringSection)
			{
				OutError = TEXT("create_string_property_section_failed");
				return false;
			}
			StringTrack->AddSection(*StringSection);
		}
		StringSection->SetRange(TRange<FFrameNumber>::All());

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
		OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetStringField(TEXT("property_type"), TEXT("string"));
		OutData->SetStringField(TEXT("property_name"), PropertyName.ToString());
		OutData->SetStringField(TEXT("property_path"), PropertyPathText);
		OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
		OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
		return true;
	}
	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::Object)
	{
		UClass* PropertyClass = nullptr;
		if (!ResolveObjectPropertyClass(PropertyClass))
		{
			return false;
		}

		UMovieSceneObjectPropertyTrack* ObjectTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneObjectPropertyTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
		const bool bTrackAlreadyExists = ObjectTrack != nullptr;
		if (!ObjectTrack)
		{
			ObjectTrack = MovieScene->AddTrack<UMovieSceneObjectPropertyTrack>(BindingGuid);
		}
		if (!ObjectTrack)
		{
			OutError = TEXT("create_object_property_track_failed");
			return false;
		}
		ObjectTrack->PropertyClass = PropertyClass;
		ObjectTrack->SetPropertyNameAndPath(PropertyName, PropertyPathText);

		UMovieSceneObjectPropertySection* ObjectSection = nullptr;
		const TArray<UMovieSceneSection*>& ExistingSections = ObjectTrack->GetAllSections();
		if (ExistingSections.Num() > 0)
		{
			ObjectSection = Cast<UMovieSceneObjectPropertySection>(ExistingSections[0]);
		}
		if (!ObjectSection)
		{
			ObjectSection = Cast<UMovieSceneObjectPropertySection>(ObjectTrack->CreateNewSection());
			if (!ObjectSection)
			{
				OutError = TEXT("create_object_property_section_failed");
				return false;
			}
			ObjectTrack->AddSection(*ObjectSection);
		}
		ObjectSection->SetRange(TRange<FFrameNumber>::All());

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
		OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetStringField(TEXT("property_type"), TEXT("object"));
		OutData->SetStringField(TEXT("property_name"), PropertyName.ToString());
		OutData->SetStringField(TEXT("property_path"), PropertyPathText);
		OutData->SetStringField(TEXT("property_class_path"), PropertyClass->GetPathName());
		OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
		OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
		return true;
	}

	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::Double)
	{
		UMovieSceneDoubleTrack* DoubleTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneDoubleTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
		const bool bTrackAlreadyExists = DoubleTrack != nullptr;
		if (!DoubleTrack)
		{
			DoubleTrack = MovieScene->AddTrack<UMovieSceneDoubleTrack>(BindingGuid);
		}
		if (!DoubleTrack)
		{
			OutError = TEXT("create_double_property_track_failed");
			return false;
		}
		DoubleTrack->SetPropertyNameAndPath(PropertyName, PropertyPathText);

		UMovieSceneDoubleSection* DoubleSection = nullptr;
		const TArray<UMovieSceneSection*>& ExistingSections = DoubleTrack->GetAllSections();
		if (ExistingSections.Num() > 0)
		{
			DoubleSection = Cast<UMovieSceneDoubleSection>(ExistingSections[0]);
		}
		if (!DoubleSection)
		{
			DoubleSection = Cast<UMovieSceneDoubleSection>(DoubleTrack->CreateNewSection());
			if (!DoubleSection)
			{
				OutError = TEXT("create_double_property_section_failed");
				return false;
			}
			DoubleTrack->AddSection(*DoubleSection);
		}
		DoubleSection->SetRange(TRange<FFrameNumber>::All());

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
		OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetStringField(TEXT("property_type"), TEXT("double"));
		OutData->SetStringField(TEXT("property_name"), PropertyName.ToString());
		OutData->SetStringField(TEXT("property_path"), PropertyPathText);
		OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
		OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
		return true;
	}

	UMovieSceneColorTrack* ColorTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneColorTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
	const bool bTrackAlreadyExists = ColorTrack != nullptr;
	if (!ColorTrack)
	{
		ColorTrack = MovieScene->AddTrack<UMovieSceneColorTrack>(BindingGuid);
	}
	if (!ColorTrack)
	{
		OutError = TEXT("create_color_property_track_failed");
		return false;
	}
	ColorTrack->SetPropertyNameAndPath(PropertyName, PropertyPathText);

	UMovieSceneColorSection* ColorSection = nullptr;
	const TArray<UMovieSceneSection*>& ExistingSections = ColorTrack->GetAllSections();
	if (ExistingSections.Num() > 0)
	{
		ColorSection = Cast<UMovieSceneColorSection>(ExistingSections[0]);
	}
	if (!ColorSection)
	{
		ColorSection = Cast<UMovieSceneColorSection>(ColorTrack->CreateNewSection());
		if (!ColorSection)
		{
			OutError = TEXT("create_color_property_section_failed");
			return false;
		}
		ColorTrack->AddSection(*ColorSection);
	}
	ColorSection->SetRange(TRange<FFrameNumber>::All());

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
	OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("property_type"), TEXT("color"));
	OutData->SetStringField(TEXT("property_name"), PropertyName.ToString());
	OutData->SetStringField(TEXT("property_path"), PropertyPathText);
	OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdSequenceAddPropertyKey(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString PropertyTypeText;
	if (!JsonTryGetString(Ctx.Params, TEXT("property_type"), PropertyTypeText) || PropertyTypeText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_property_type");
		return false;
	}

	UeAgentSequenceOps::ESequencePropertyTrackType PropertyType;
	if (!UeAgentSequenceOps::ParseSequencePropertyTrackType(PropertyTypeText, PropertyType))
	{
		OutError = TEXT("invalid_property_type");
		return false;
	}

	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::Bool)
	{
		return CmdSequenceAddBoolPropertyKey(Ctx, OutData, OutError);
	}
	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::Byte)
	{
		// handled below through unified byte path
	}
	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::Vector)
	{
		// handled below through unified vector path
	}
	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::Rotator)
	{
		// handled below through unified rotator path
	}
	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::ActorReference)
	{
		// handled below through unified actor reference path
	}
	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::Float)
	{
		return CmdSequenceAddFloatPropertyKey(Ctx, OutData, OutError);
	}
	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::Integer)
	{
		return CmdSequenceAddIntegerPropertyKey(Ctx, OutData, OutError);
	}

	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString BindingGuidText;
	if (!JsonTryGetString(Ctx.Params, TEXT("binding_guid"), BindingGuidText) || BindingGuidText.IsEmpty())
	{
		OutError = TEXT("missing_binding_guid");
		return false;
	}
	FGuid BindingGuid;
	if (!FGuid::Parse(BindingGuidText, BindingGuid))
	{
		OutError = TEXT("invalid_binding_guid");
		return false;
	}

	FString PropertyNameText;
	if (!JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyNameText) || PropertyNameText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_property_name");
		return false;
	}
	PropertyNameText = PropertyNameText.TrimStartAndEnd();

	FString PropertyPathText;
	JsonTryGetString(Ctx.Params, TEXT("property_path"), PropertyPathText);
	PropertyPathText = PropertyPathText.TrimStartAndEnd();
	if (PropertyPathText.IsEmpty())
	{
		PropertyPathText = PropertyNameText;
	}

	auto ResolveObjectPropertyClassAndValue = [&](UClass*& OutClass, UObject*& OutValueObject, UObject*& OutValueBeforeKeyObject) -> bool
	{
		OutClass = nullptr;
		OutValueObject = nullptr;
		OutValueBeforeKeyObject = nullptr;

		FString ValuePath;
		JsonTryGetString(Ctx.Params, TEXT("value_path"), ValuePath);
		if (ValuePath.TrimStartAndEnd().IsEmpty())
		{
			JsonTryGetString(Ctx.Params, TEXT("object_path"), ValuePath);
		}
		ValuePath = ValuePath.TrimStartAndEnd();
		if (!ValuePath.IsEmpty())
		{
			OutValueObject = UeAgentSequenceOps::LoadAssetObject(ValuePath);
			if (!OutValueObject)
			{
				OutError = TEXT("value_object_not_found");
				return false;
			}
		}

		FString ValueBeforeKeyPath;
		JsonTryGetString(Ctx.Params, TEXT("value_before_key_path"), ValueBeforeKeyPath);
		if (ValueBeforeKeyPath.TrimStartAndEnd().IsEmpty())
		{
			JsonTryGetString(Ctx.Params, TEXT("object_before_key_path"), ValueBeforeKeyPath);
		}
		ValueBeforeKeyPath = ValueBeforeKeyPath.TrimStartAndEnd();
		if (!ValueBeforeKeyPath.IsEmpty())
		{
			OutValueBeforeKeyObject = UeAgentSequenceOps::LoadAssetObject(ValueBeforeKeyPath);
			if (!OutValueBeforeKeyObject)
			{
				OutError = TEXT("value_before_key_object_not_found");
				return false;
			}
		}

		FString PropertyClassPath;
		JsonTryGetString(Ctx.Params, TEXT("property_class_path"), PropertyClassPath);
		PropertyClassPath = PropertyClassPath.TrimStartAndEnd();
		if (!PropertyClassPath.IsEmpty())
		{
			OutClass = LoadObject<UClass>(nullptr, *PropertyClassPath);
			if (!OutClass && !PropertyClassPath.EndsWith(TEXT("_C")))
			{
				OutClass = LoadObject<UClass>(nullptr, *(PropertyClassPath + TEXT("_C")));
			}
			if (!OutClass)
			{
				OutError = TEXT("property_class_not_found");
				return false;
			}
		}
		else if (OutValueObject)
		{
			OutClass = OutValueObject->GetClass();
		}

		if (!OutClass)
		{
			OutError = TEXT("missing_property_class_path");
			return false;
		}
		if (!OutValueObject)
		{
			OutError = TEXT("missing_value_path");
			return false;
		}

		return true;
	};

	double TimeSeconds = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("time_seconds"), TimeSeconds))
	{
		OutError = TEXT("missing_time_seconds");
		return false;
	}

	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::Byte)
	{
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
		if (!MovieScene->FindBinding(BindingGuid))
		{
			OutError = TEXT("binding_not_found");
			return false;
		}

		Sequence->Modify();
		MovieScene->Modify();

		const FName PropertyName(*PropertyNameText);
		UMovieSceneByteTrack* ByteTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneByteTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
		const bool bTrackAlreadyExists = ByteTrack != nullptr;
		if (!ByteTrack)
		{
			ByteTrack = MovieScene->AddTrack<UMovieSceneByteTrack>(BindingGuid);
		}
		if (!ByteTrack)
		{
			OutError = TEXT("create_byte_property_track_failed");
			return false;
		}
		ByteTrack->SetPropertyNameAndPath(PropertyName, PropertyPathText);

		UEnum* ByteEnum = ByteTrack->GetEnum();
		FString EnumPath;
		JsonTryGetString(Ctx.Params, TEXT("enum_path"), EnumPath);
		EnumPath = EnumPath.TrimStartAndEnd();
		if (!EnumPath.IsEmpty())
		{
			ByteEnum = LoadObject<UEnum>(nullptr, *EnumPath);
			if (!ByteEnum)
			{
				OutError = TEXT("enum_not_found");
				return false;
			}
			ByteTrack->SetEnum(ByteEnum);
		}

		auto ParseByteValue = [&](const TCHAR* FieldName, const TCHAR* NameField, uint8& OutValue, bool& bOutProvided) -> bool
		{
			bOutProvided = false;

			double NumericValue = 0.0;
			if (JsonTryGetNumber(Ctx.Params, FieldName, NumericValue))
			{
				bOutProvided = true;
				const int32 Rounded = FMath::RoundToInt(NumericValue);
				if (Rounded < 0 || Rounded > 255)
				{
					OutError = TEXT("invalid_byte_value");
					return false;
				}
				OutValue = static_cast<uint8>(Rounded);
				return true;
			}

			FString NameValue;
			if (JsonTryGetString(Ctx.Params, NameField, NameValue) && !NameValue.TrimStartAndEnd().IsEmpty())
			{
				if (!ByteEnum)
				{
					OutError = TEXT("missing_enum_path");
					return false;
				}
				const int64 EnumValue = ByteEnum->GetValueByNameString(NameValue.TrimStartAndEnd());
				if (EnumValue == INDEX_NONE || EnumValue < 0 || EnumValue > 255)
				{
					OutError = TEXT("invalid_enum_value_name");
					return false;
				}
				bOutProvided = true;
				OutValue = static_cast<uint8>(EnumValue);
				return true;
			}

			return true;
		};

		uint8 ByteValue = 0;
		bool bHasByteValue = false;
		if (!ParseByteValue(TEXT("value"), TEXT("value_name"), ByteValue, bHasByteValue))
		{
			return false;
		}
		if (!bHasByteValue)
		{
			OutError = TEXT("missing_value");
			return false;
		}

		uint8 ByteValueBeforeKey = 0;
		bool bHasValueBeforeKey = false;
		if (!ParseByteValue(TEXT("value_before_key"), TEXT("value_before_key_name"), ByteValueBeforeKey, bHasValueBeforeKey))
		{
			return false;
		}

		UMovieSceneByteSection* ByteSection = nullptr;
		const TArray<UMovieSceneSection*>& ExistingSections = ByteTrack->GetAllSections();
		if (ExistingSections.Num() > 0)
		{
			ByteSection = Cast<UMovieSceneByteSection>(ExistingSections[0]);
		}
		if (!ByteSection)
		{
			ByteSection = Cast<UMovieSceneByteSection>(ByteTrack->CreateNewSection());
			if (!ByteSection)
			{
				OutError = TEXT("create_byte_property_section_failed");
				return false;
			}
			ByteTrack->AddSection(*ByteSection);
		}

		const FFrameRate TickResolution = MovieScene->GetTickResolution();
		const FFrameNumber KeyFrame = TickResolution.AsFrameTime(TimeSeconds).RoundToFrame();
		const FFrameNumber PlaybackStartFrame = MovieScene->GetPlaybackRange().GetLowerBoundValue();

		ByteTrack->Modify();
		ByteSection->Modify();
		ByteSection->SetRange(TRange<FFrameNumber>::All());
		ByteSection->ByteCurve.SetEnum(ByteTrack->GetEnum());

		if (bHasValueBeforeKey && ByteSection->ByteCurve.GetData().GetTimes().Num() == 0 && KeyFrame > PlaybackStartFrame)
		{
			ByteSection->ByteCurve.GetData().AddKey(PlaybackStartFrame, ByteValueBeforeKey);
		}
		ByteSection->ByteCurve.GetData().UpdateOrAddKey(KeyFrame, ByteValue);

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
		OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetStringField(TEXT("property_type"), TEXT("byte"));
		OutData->SetStringField(TEXT("property_name"), PropertyName.ToString());
		OutData->SetStringField(TEXT("property_path"), PropertyPathText);
		OutData->SetStringField(TEXT("enum_path"), ByteTrack->GetEnum() ? ByteTrack->GetEnum()->GetPathName() : TEXT(""));
		OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
		OutData->SetNumberField(TEXT("time_seconds"), TimeSeconds);
		OutData->SetNumberField(TEXT("frame_number"), KeyFrame.Value);
		OutData->SetNumberField(TEXT("value"), ByteValue);
		OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
		return true;
	}
	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::String)
	{
		FString ValueText;
		if (!JsonTryGetString(Ctx.Params, TEXT("value"), ValueText))
		{
			OutError = TEXT("missing_value");
			return false;
		}

		FString ValueBeforeKey;
		const bool bHasValueBeforeKey = JsonTryGetString(Ctx.Params, TEXT("value_before_key"), ValueBeforeKey);

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
		if (!MovieScene->FindBinding(BindingGuid))
		{
			OutError = TEXT("binding_not_found");
			return false;
		}

		Sequence->Modify();
		MovieScene->Modify();

		const FName PropertyName(*PropertyNameText);
		UMovieSceneStringTrack* StringTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneStringTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
		const bool bTrackAlreadyExists = StringTrack != nullptr;
		if (!StringTrack)
		{
			StringTrack = MovieScene->AddTrack<UMovieSceneStringTrack>(BindingGuid);
		}
		if (!StringTrack)
		{
			OutError = TEXT("create_string_property_track_failed");
			return false;
		}
		StringTrack->SetPropertyNameAndPath(PropertyName, PropertyPathText);

		UMovieSceneStringSection* StringSection = nullptr;
		const TArray<UMovieSceneSection*>& ExistingSections = StringTrack->GetAllSections();
		if (ExistingSections.Num() > 0)
		{
			StringSection = Cast<UMovieSceneStringSection>(ExistingSections[0]);
		}
		if (!StringSection)
		{
			StringSection = Cast<UMovieSceneStringSection>(StringTrack->CreateNewSection());
			if (!StringSection)
			{
				OutError = TEXT("create_string_property_section_failed");
				return false;
			}
			StringTrack->AddSection(*StringSection);
		}

		const FFrameRate TickResolution = MovieScene->GetTickResolution();
		const FFrameNumber KeyFrame = TickResolution.AsFrameTime(TimeSeconds).RoundToFrame();
		const FFrameNumber PlaybackStartFrame = MovieScene->GetPlaybackRange().GetLowerBoundValue();

		StringTrack->Modify();
		StringSection->Modify();
		StringSection->SetRange(TRange<FFrameNumber>::All());

		FMovieSceneStringChannel& Channel = const_cast<FMovieSceneStringChannel&>(StringSection->GetChannel());
		if (bHasValueBeforeKey && Channel.GetData().GetTimes().Num() == 0 && KeyFrame > PlaybackStartFrame)
		{
			Channel.GetData().AddKey(PlaybackStartFrame, ValueBeforeKey);
		}
		Channel.GetData().UpdateOrAddKey(KeyFrame, ValueText);

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
		OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetStringField(TEXT("property_type"), TEXT("string"));
		OutData->SetStringField(TEXT("property_name"), PropertyName.ToString());
		OutData->SetStringField(TEXT("property_path"), PropertyPathText);
		OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
		OutData->SetNumberField(TEXT("time_seconds"), TimeSeconds);
		OutData->SetNumberField(TEXT("frame_number"), KeyFrame.Value);
		OutData->SetStringField(TEXT("value"), ValueText);
		OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
		return true;
	}
	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::Vector)
	{
		struct FParsedSequenceVectorValue
		{
			double Components[4] = { 0.0, 0.0, 0.0, 0.0 };
			int32 ChannelsUsed = 0;
		};

		auto ParseVectorObject = [](const TSharedPtr<FJsonObject>& Obj, const FString& FieldName, FParsedSequenceVectorValue& OutValue) -> bool
		{
			const TSharedPtr<FJsonObject>* VectorObject = nullptr;
			if (!Obj.IsValid() || !Obj->TryGetObjectField(FieldName, VectorObject) || !VectorObject || !VectorObject->IsValid())
			{
				return false;
			}

			double X = 0.0;
			double Y = 0.0;
			double Z = 0.0;
			double W = 0.0;
			const bool bHasX = (*VectorObject)->TryGetNumberField(TEXT("x"), X);
			const bool bHasY = (*VectorObject)->TryGetNumberField(TEXT("y"), Y);
			const bool bHasZ = (*VectorObject)->TryGetNumberField(TEXT("z"), Z);
			const bool bHasW = (*VectorObject)->TryGetNumberField(TEXT("w"), W);
			if (!bHasX || !bHasY)
			{
				return false;
			}

			OutValue.Components[0] = X;
			OutValue.Components[1] = Y;
			OutValue.Components[2] = bHasZ ? Z : 0.0;
			OutValue.Components[3] = bHasW ? W : 0.0;
			OutValue.ChannelsUsed = bHasW ? 4 : (bHasZ ? 3 : 2);
			return true;
		};

		auto ParseVectorTopLevel = [](const TSharedPtr<FJsonObject>& Obj, FParsedSequenceVectorValue& OutValue) -> bool
		{
			if (!Obj.IsValid())
			{
				return false;
			}

			double X = 0.0;
			double Y = 0.0;
			double Z = 0.0;
			double W = 0.0;
			const bool bHasX = JsonTryGetNumber(Obj, TEXT("x"), X);
			const bool bHasY = JsonTryGetNumber(Obj, TEXT("y"), Y);
			const bool bHasZ = JsonTryGetNumber(Obj, TEXT("z"), Z);
			const bool bHasW = JsonTryGetNumber(Obj, TEXT("w"), W);
			if (!bHasX || !bHasY)
			{
				return false;
			}

			OutValue.Components[0] = X;
			OutValue.Components[1] = Y;
			OutValue.Components[2] = bHasZ ? Z : 0.0;
			OutValue.Components[3] = bHasW ? W : 0.0;
			OutValue.ChannelsUsed = bHasW ? 4 : (bHasZ ? 3 : 2);
			return true;
		};

		FParsedSequenceVectorValue VectorValue;
		bool bHasVectorValue = ParseVectorObject(Ctx.Params, TEXT("value"), VectorValue);
		if (!bHasVectorValue)
		{
			bHasVectorValue = ParseVectorTopLevel(Ctx.Params, VectorValue);
		}
		if (!bHasVectorValue)
		{
			OutError = TEXT("missing_or_invalid_vector_value");
			return false;
		}

		FParsedSequenceVectorValue VectorValueBeforeKey;
		const bool bHasVectorValueBeforeKey = ParseVectorObject(Ctx.Params, TEXT("value_before_key"), VectorValueBeforeKey);

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
		if (!MovieScene->FindBinding(BindingGuid))
		{
			OutError = TEXT("binding_not_found");
			return false;
		}

		Sequence->Modify();
		MovieScene->Modify();

		const FName PropertyName(*PropertyNameText);

		auto ResolveVectorTrackSettings = [&](UeAgentSequenceOps::ESequenceVectorPrecision& OutPrecision, int32& OutChannels) -> bool
		{
			bool bHasExplicitPrecision = false;
			UeAgentSequenceOps::ESequenceVectorPrecision ExplicitPrecision = UeAgentSequenceOps::ESequenceVectorPrecision::Double;
			FString VectorPrecisionText;
			if (JsonTryGetString(Ctx.Params, TEXT("vector_precision"), VectorPrecisionText) && !VectorPrecisionText.TrimStartAndEnd().IsEmpty())
			{
				if (!UeAgentSequenceOps::ParseSequenceVectorPrecision(VectorPrecisionText, ExplicitPrecision))
				{
					OutError = TEXT("invalid_vector_precision");
					return false;
				}
				bHasExplicitPrecision = true;
			}

			bool bHasExplicitChannels = false;
			int32 ExplicitChannels = 3;
			double ChannelsUsedNumber = 0.0;
			if (JsonTryGetNumber(Ctx.Params, TEXT("channels_used"), ChannelsUsedNumber))
			{
				ExplicitChannels = static_cast<int32>(ChannelsUsedNumber);
				if (ExplicitChannels < 2 || ExplicitChannels > 4)
				{
					OutError = TEXT("invalid_channels_used");
					return false;
				}
				bHasExplicitChannels = true;
			}

			bool bHasInferredPrecision = false;
			UeAgentSequenceOps::ESequenceVectorPrecision InferredPrecision = UeAgentSequenceOps::ESequenceVectorPrecision::Double;
			bool bHasInferredChannels = false;
			int32 InferredChannels = 3;
			UeAgentSequenceOps::InferSequenceVectorDefaultsFromTypeText(PropertyTypeText, bHasInferredPrecision, InferredPrecision, bHasInferredChannels, InferredChannels);

			UMovieSceneFloatVectorTrack* ExistingFloatTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneFloatVectorTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
			UMovieSceneDoubleVectorTrack* ExistingDoubleTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneDoubleVectorTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
			if (ExistingFloatTrack && ExistingDoubleTrack)
			{
				OutError = TEXT("multiple_vector_tracks_for_property");
				return false;
			}

			if (bHasExplicitPrecision)
			{
				OutPrecision = ExplicitPrecision;
			}
			else if (ExistingFloatTrack)
			{
				OutPrecision = UeAgentSequenceOps::ESequenceVectorPrecision::Float;
			}
			else if (ExistingDoubleTrack)
			{
				OutPrecision = UeAgentSequenceOps::ESequenceVectorPrecision::Double;
			}
			else if (bHasInferredPrecision)
			{
				OutPrecision = InferredPrecision;
			}
			else
			{
				OutPrecision = UeAgentSequenceOps::ESequenceVectorPrecision::Double;
			}

			if (bHasExplicitChannels)
			{
				OutChannels = ExplicitChannels;
			}
			else if (ExistingFloatTrack)
			{
				OutChannels = FMath::Clamp(ExistingFloatTrack->GetNumChannelsUsed(), 2, 4);
			}
			else if (ExistingDoubleTrack)
			{
				OutChannels = FMath::Clamp(ExistingDoubleTrack->GetNumChannelsUsed(), 2, 4);
			}
			else if (bHasInferredChannels)
			{
				OutChannels = InferredChannels;
			}
			else
			{
				OutChannels = VectorValue.ChannelsUsed > 0 ? VectorValue.ChannelsUsed : 3;
			}

			if (OutChannels < 2 || OutChannels > 4)
			{
				OutError = TEXT("invalid_channels_used");
				return false;
			}
			if (ExistingFloatTrack && OutPrecision != UeAgentSequenceOps::ESequenceVectorPrecision::Float)
			{
				OutError = TEXT("vector_track_precision_mismatch");
				return false;
			}
			if (ExistingDoubleTrack && OutPrecision != UeAgentSequenceOps::ESequenceVectorPrecision::Double)
			{
				OutError = TEXT("vector_track_precision_mismatch");
				return false;
			}
			if (ExistingFloatTrack && ExistingFloatTrack->GetNumChannelsUsed() != OutChannels)
			{
				OutError = TEXT("vector_track_channels_mismatch");
				return false;
			}
			if (ExistingDoubleTrack && ExistingDoubleTrack->GetNumChannelsUsed() != OutChannels)
			{
				OutError = TEXT("vector_track_channels_mismatch");
				return false;
			}
			if (VectorValue.ChannelsUsed != OutChannels)
			{
				OutError = TEXT("vector_value_channels_mismatch");
				return false;
			}
			if (bHasVectorValueBeforeKey && VectorValueBeforeKey.ChannelsUsed != OutChannels)
			{
				OutError = TEXT("vector_value_before_key_channels_mismatch");
				return false;
			}

			return true;
		};

		UeAgentSequenceOps::ESequenceVectorPrecision VectorPrecision;
		int32 ChannelsUsed = 3;
		if (!ResolveVectorTrackSettings(VectorPrecision, ChannelsUsed))
		{
			return false;
		}

		const FFrameRate TickResolution = MovieScene->GetTickResolution();
		const FFrameNumber KeyFrame = TickResolution.AsFrameTime(TimeSeconds).RoundToFrame();
		const FFrameNumber PlaybackStartFrame = MovieScene->GetPlaybackRange().GetLowerBoundValue();

		TSharedPtr<FJsonObject> ValueObject = MakeShared<FJsonObject>();
		ValueObject->SetNumberField(TEXT("x"), VectorValue.Components[0]);
		ValueObject->SetNumberField(TEXT("y"), VectorValue.Components[1]);
		if (ChannelsUsed >= 3)
		{
			ValueObject->SetNumberField(TEXT("z"), VectorValue.Components[2]);
		}
		if (ChannelsUsed >= 4)
		{
			ValueObject->SetNumberField(TEXT("w"), VectorValue.Components[3]);
		}

		if (VectorPrecision == UeAgentSequenceOps::ESequenceVectorPrecision::Float)
		{
			UMovieSceneFloatVectorTrack* VectorTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneFloatVectorTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
			const bool bTrackAlreadyExists = VectorTrack != nullptr;
			if (!VectorTrack)
			{
				VectorTrack = MovieScene->AddTrack<UMovieSceneFloatVectorTrack>(BindingGuid);
			}
			if (!VectorTrack)
			{
				OutError = TEXT("create_float_vector_property_track_failed");
				return false;
			}
			VectorTrack->SetPropertyNameAndPath(PropertyName, PropertyPathText);
			VectorTrack->SetNumChannelsUsed(ChannelsUsed);

			UMovieSceneFloatVectorSection* VectorSection = nullptr;
			const TArray<UMovieSceneSection*>& ExistingSections = VectorTrack->GetAllSections();
			if (ExistingSections.Num() > 0)
			{
				VectorSection = Cast<UMovieSceneFloatVectorSection>(ExistingSections[0]);
			}
			if (!VectorSection)
			{
				VectorSection = Cast<UMovieSceneFloatVectorSection>(VectorTrack->CreateNewSection());
				if (!VectorSection)
				{
					OutError = TEXT("create_float_vector_property_section_failed");
					return false;
				}
				VectorTrack->AddSection(*VectorSection);
			}

			VectorTrack->Modify();
			VectorSection->Modify();
			VectorSection->SetChannelsUsed(ChannelsUsed);
			VectorSection->SetRange(TRange<FFrameNumber>::All());

			bool bSectionEmpty = true;
			for (int32 ChannelIndex = 0; ChannelIndex < ChannelsUsed; ++ChannelIndex)
			{
				if (VectorSection->GetChannel(ChannelIndex).GetData().GetTimes().Num() > 0)
				{
					bSectionEmpty = false;
					break;
				}
			}
			if (bHasVectorValueBeforeKey && bSectionEmpty && KeyFrame > PlaybackStartFrame)
			{
				for (int32 ChannelIndex = 0; ChannelIndex < ChannelsUsed; ++ChannelIndex)
				{
					FMovieSceneFloatChannel& Channel = const_cast<FMovieSceneFloatChannel&>(VectorSection->GetChannel(ChannelIndex));
					Channel.GetData().AddKey(PlaybackStartFrame, FMovieSceneFloatValue(static_cast<float>(VectorValueBeforeKey.Components[ChannelIndex])));
				}
			}

			for (int32 ChannelIndex = 0; ChannelIndex < ChannelsUsed; ++ChannelIndex)
			{
				FMovieSceneFloatChannel& Channel = const_cast<FMovieSceneFloatChannel&>(VectorSection->GetChannel(ChannelIndex));
				Channel.GetData().UpdateOrAddKey(KeyFrame, FMovieSceneFloatValue(static_cast<float>(VectorValue.Components[ChannelIndex])));
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
			OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
			OutData->SetStringField(TEXT("property_type"), TEXT("vector"));
			OutData->SetStringField(TEXT("vector_precision"), UeAgentSequenceOps::LexToString(VectorPrecision));
			OutData->SetNumberField(TEXT("channels_used"), ChannelsUsed);
			OutData->SetStringField(TEXT("property_name"), PropertyName.ToString());
			OutData->SetStringField(TEXT("property_path"), PropertyPathText);
			OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
			OutData->SetNumberField(TEXT("time_seconds"), TimeSeconds);
			OutData->SetNumberField(TEXT("frame_number"), KeyFrame.Value);
			OutData->SetObjectField(TEXT("value"), ValueObject);
			OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
			return true;
		}

		UMovieSceneDoubleVectorTrack* VectorTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneDoubleVectorTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
		const bool bTrackAlreadyExists = VectorTrack != nullptr;
		if (!VectorTrack)
		{
			VectorTrack = MovieScene->AddTrack<UMovieSceneDoubleVectorTrack>(BindingGuid);
		}
		if (!VectorTrack)
		{
			OutError = TEXT("create_double_vector_property_track_failed");
			return false;
		}
		VectorTrack->SetPropertyNameAndPath(PropertyName, PropertyPathText);
		VectorTrack->SetNumChannelsUsed(ChannelsUsed);

		UMovieSceneDoubleVectorSection* VectorSection = nullptr;
		const TArray<UMovieSceneSection*>& ExistingSections = VectorTrack->GetAllSections();
		if (ExistingSections.Num() > 0)
		{
			VectorSection = Cast<UMovieSceneDoubleVectorSection>(ExistingSections[0]);
		}
		if (!VectorSection)
		{
			VectorSection = Cast<UMovieSceneDoubleVectorSection>(VectorTrack->CreateNewSection());
			if (!VectorSection)
			{
				OutError = TEXT("create_double_vector_property_section_failed");
				return false;
			}
			VectorTrack->AddSection(*VectorSection);
		}

		VectorTrack->Modify();
		VectorSection->Modify();
		VectorSection->SetChannelsUsed(ChannelsUsed);
		VectorSection->SetRange(TRange<FFrameNumber>::All());

		bool bSectionEmpty = true;
		for (int32 ChannelIndex = 0; ChannelIndex < ChannelsUsed; ++ChannelIndex)
		{
			if (VectorSection->GetChannel(ChannelIndex).GetData().GetTimes().Num() > 0)
			{
				bSectionEmpty = false;
				break;
			}
		}
		if (bHasVectorValueBeforeKey && bSectionEmpty && KeyFrame > PlaybackStartFrame)
		{
			for (int32 ChannelIndex = 0; ChannelIndex < ChannelsUsed; ++ChannelIndex)
			{
				FMovieSceneDoubleChannel& Channel = const_cast<FMovieSceneDoubleChannel&>(VectorSection->GetChannel(ChannelIndex));
				Channel.GetData().AddKey(PlaybackStartFrame, FMovieSceneDoubleValue(VectorValueBeforeKey.Components[ChannelIndex]));
			}
		}

		for (int32 ChannelIndex = 0; ChannelIndex < ChannelsUsed; ++ChannelIndex)
		{
			FMovieSceneDoubleChannel& Channel = const_cast<FMovieSceneDoubleChannel&>(VectorSection->GetChannel(ChannelIndex));
			Channel.GetData().UpdateOrAddKey(KeyFrame, FMovieSceneDoubleValue(VectorValue.Components[ChannelIndex]));
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
		OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetStringField(TEXT("property_type"), TEXT("vector"));
		OutData->SetStringField(TEXT("vector_precision"), UeAgentSequenceOps::LexToString(VectorPrecision));
		OutData->SetNumberField(TEXT("channels_used"), ChannelsUsed);
		OutData->SetStringField(TEXT("property_name"), PropertyName.ToString());
		OutData->SetStringField(TEXT("property_path"), PropertyPathText);
		OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
		OutData->SetNumberField(TEXT("time_seconds"), TimeSeconds);
		OutData->SetNumberField(TEXT("frame_number"), KeyFrame.Value);
		OutData->SetObjectField(TEXT("value"), ValueObject);
		OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
		return true;
	}
	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::Rotator)
	{
		struct FParsedSequenceRotatorValue
		{
			double Pitch = 0.0;
			double Yaw = 0.0;
			double Roll = 0.0;
		};

		auto ParseRotatorObject = [](const TSharedPtr<FJsonObject>& Obj, const FString& FieldName, FParsedSequenceRotatorValue& OutValue) -> bool
		{
			const TSharedPtr<FJsonObject>* RotatorObject = nullptr;
			if (!Obj.IsValid() || !Obj->TryGetObjectField(FieldName, RotatorObject) || !RotatorObject || !RotatorObject->IsValid())
			{
				return false;
			}

			const bool bHasPitch = (*RotatorObject)->TryGetNumberField(TEXT("pitch"), OutValue.Pitch);
			const bool bHasYaw = (*RotatorObject)->TryGetNumberField(TEXT("yaw"), OutValue.Yaw);
			const bool bHasRoll = (*RotatorObject)->TryGetNumberField(TEXT("roll"), OutValue.Roll);
			return bHasPitch && bHasYaw && bHasRoll;
		};

		auto ParseRotatorTopLevel = [](const TSharedPtr<FJsonObject>& Obj, FParsedSequenceRotatorValue& OutValue) -> bool
		{
			if (!Obj.IsValid())
			{
				return false;
			}

			const bool bHasPitch = JsonTryGetNumber(Obj, TEXT("pitch"), OutValue.Pitch);
			const bool bHasYaw = JsonTryGetNumber(Obj, TEXT("yaw"), OutValue.Yaw);
			const bool bHasRoll = JsonTryGetNumber(Obj, TEXT("roll"), OutValue.Roll);
			return bHasPitch && bHasYaw && bHasRoll;
		};

		FParsedSequenceRotatorValue RotatorValue;
		bool bHasRotatorValue = ParseRotatorObject(Ctx.Params, TEXT("value"), RotatorValue);
		if (!bHasRotatorValue)
		{
			bHasRotatorValue = ParseRotatorTopLevel(Ctx.Params, RotatorValue);
		}
		if (!bHasRotatorValue)
		{
			OutError = TEXT("missing_or_invalid_rotator_value");
			return false;
		}

		FParsedSequenceRotatorValue RotatorValueBeforeKey;
		const bool bHasRotatorValueBeforeKey = ParseRotatorObject(Ctx.Params, TEXT("value_before_key"), RotatorValueBeforeKey);

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
		if (!MovieScene->FindBinding(BindingGuid))
		{
			OutError = TEXT("binding_not_found");
			return false;
		}

		Sequence->Modify();
		MovieScene->Modify();

		const FName PropertyName(*PropertyNameText);
		UMovieSceneRotatorTrack* RotatorTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneRotatorTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
		const bool bTrackAlreadyExists = RotatorTrack != nullptr;
		if (!RotatorTrack)
		{
			RotatorTrack = MovieScene->AddTrack<UMovieSceneRotatorTrack>(BindingGuid);
		}
		if (!RotatorTrack)
		{
			OutError = TEXT("create_rotator_property_track_failed");
			return false;
		}
		RotatorTrack->SetPropertyNameAndPath(PropertyName, PropertyPathText);

		UMovieSceneRotatorSection* RotatorSection = nullptr;
		const TArray<UMovieSceneSection*>& ExistingSections = RotatorTrack->GetAllSections();
		if (ExistingSections.Num() > 0)
		{
			RotatorSection = Cast<UMovieSceneRotatorSection>(ExistingSections[0]);
		}
		if (!RotatorSection)
		{
			RotatorSection = Cast<UMovieSceneRotatorSection>(RotatorTrack->CreateNewSection());
			if (!RotatorSection)
			{
				OutError = TEXT("create_rotator_property_section_failed");
				return false;
			}
			RotatorTrack->AddSection(*RotatorSection);
		}

		const FFrameRate TickResolution = MovieScene->GetTickResolution();
		const FFrameNumber KeyFrame = TickResolution.AsFrameTime(TimeSeconds).RoundToFrame();
		const FFrameNumber PlaybackStartFrame = MovieScene->GetPlaybackRange().GetLowerBoundValue();

		RotatorTrack->Modify();
		RotatorSection->Modify();
		RotatorSection->SetRange(TRange<FFrameNumber>::All());

		auto AddRotatorKey = [&](const int32 ChannelIndex, const double Value)
		{
			FMovieSceneDoubleChannel& Channel = const_cast<FMovieSceneDoubleChannel&>(RotatorSection->GetChannel(ChannelIndex));
			Channel.GetData().UpdateOrAddKey(KeyFrame, FMovieSceneDoubleValue(Value));
		};

		const bool bSectionEmpty =
			RotatorSection->GetChannel(UMovieSceneRotatorSection::PitchChannelIndex).GetData().GetTimes().Num() == 0 &&
			RotatorSection->GetChannel(UMovieSceneRotatorSection::YawChannelIndex).GetData().GetTimes().Num() == 0 &&
			RotatorSection->GetChannel(UMovieSceneRotatorSection::RollChannelIndex).GetData().GetTimes().Num() == 0;
		if (bHasRotatorValueBeforeKey && bSectionEmpty && KeyFrame > PlaybackStartFrame)
		{
			const_cast<FMovieSceneDoubleChannel&>(RotatorSection->GetChannel(UMovieSceneRotatorSection::PitchChannelIndex)).GetData().AddKey(PlaybackStartFrame, FMovieSceneDoubleValue(RotatorValueBeforeKey.Pitch));
			const_cast<FMovieSceneDoubleChannel&>(RotatorSection->GetChannel(UMovieSceneRotatorSection::YawChannelIndex)).GetData().AddKey(PlaybackStartFrame, FMovieSceneDoubleValue(RotatorValueBeforeKey.Yaw));
			const_cast<FMovieSceneDoubleChannel&>(RotatorSection->GetChannel(UMovieSceneRotatorSection::RollChannelIndex)).GetData().AddKey(PlaybackStartFrame, FMovieSceneDoubleValue(RotatorValueBeforeKey.Roll));
		}

		AddRotatorKey(UMovieSceneRotatorSection::PitchChannelIndex, RotatorValue.Pitch);
		AddRotatorKey(UMovieSceneRotatorSection::YawChannelIndex, RotatorValue.Yaw);
		AddRotatorKey(UMovieSceneRotatorSection::RollChannelIndex, RotatorValue.Roll);

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

		TSharedPtr<FJsonObject> ValueObject = MakeShared<FJsonObject>();
		ValueObject->SetNumberField(TEXT("pitch"), RotatorValue.Pitch);
		ValueObject->SetNumberField(TEXT("yaw"), RotatorValue.Yaw);
		ValueObject->SetNumberField(TEXT("roll"), RotatorValue.Roll);

		OutData->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
		OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetStringField(TEXT("property_type"), TEXT("rotator"));
		OutData->SetStringField(TEXT("property_name"), PropertyName.ToString());
		OutData->SetStringField(TEXT("property_path"), PropertyPathText);
		OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
		OutData->SetNumberField(TEXT("time_seconds"), TimeSeconds);
		OutData->SetNumberField(TEXT("frame_number"), KeyFrame.Value);
		OutData->SetObjectField(TEXT("value"), ValueObject);
		OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
		return true;
	}
	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::ActorReference)
	{
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
		if (!MovieScene->FindBinding(BindingGuid))
		{
			OutError = TEXT("binding_not_found");
			return false;
		}

		Sequence->Modify();
		MovieScene->Modify();

		auto ResolveActorReferenceKey = [&](const TCHAR* BindingGuidField, const TCHAR* ActorIdField, const TCHAR* ComponentField, const TCHAR* SocketField, FMovieSceneActorReferenceKey& OutKey, bool& bOutProvided, FString& OutResolvedActorId) -> bool
		{
			OutKey = FMovieSceneActorReferenceKey();
			bOutProvided = false;
			OutResolvedActorId.Reset();

			FString TargetBindingGuidText;
			if (JsonTryGetString(Ctx.Params, BindingGuidField, TargetBindingGuidText) && !TargetBindingGuidText.TrimStartAndEnd().IsEmpty())
			{
				FGuid TargetBindingGuid;
				if (!FGuid::Parse(TargetBindingGuidText, TargetBindingGuid))
				{
					OutError = TEXT("invalid_value_binding_guid");
					return false;
				}
				if (!MovieScene->FindBinding(TargetBindingGuid))
				{
					OutError = TEXT("value_binding_not_found");
					return false;
				}
				OutKey.Object = UE::MovieScene::FRelativeObjectBindingID(TargetBindingGuid);
				bOutProvided = true;
			}
			else
			{
				FString ActorId;
				if (JsonTryGetString(Ctx.Params, ActorIdField, ActorId) && !ActorId.TrimStartAndEnd().IsEmpty())
				{
					FString WorldError;
					UWorld* World = GetEditorWorld(WorldError);
					if (!World)
					{
						OutError = WorldError.IsEmpty() ? TEXT("editor_world_not_found") : WorldError;
						return false;
					}

					AActor* TargetActor = FindActorByNameOrLabel(World, ActorId.TrimStartAndEnd());
					if (!TargetActor)
					{
						OutError = TEXT("value_actor_not_found");
						return false;
					}

					FGuid TargetBindingGuid;
					PRAGMA_DISABLE_DEPRECATION_WARNINGS
					TargetBindingGuid = Sequence->FindBindingFromObject(TargetActor, World);
					PRAGMA_ENABLE_DEPRECATION_WARNINGS
					if (!TargetBindingGuid.IsValid())
					{
						const FString BindingName = TargetActor->GetActorLabel().IsEmpty() ? TargetActor->GetName() : TargetActor->GetActorLabel();
						TargetBindingGuid = MovieScene->AddPossessable(BindingName, TargetActor->GetClass());
						if (!TargetBindingGuid.IsValid())
						{
							OutError = TEXT("add_possessable_failed");
							return false;
						}
						Sequence->BindPossessableObject(TargetBindingGuid, *TargetActor, World);
					}

					OutKey.Object = UE::MovieScene::FRelativeObjectBindingID(TargetBindingGuid);
					OutResolvedActorId = TargetActor->GetActorLabel().IsEmpty() ? TargetActor->GetName() : TargetActor->GetActorLabel();
					bOutProvided = true;
				}
			}

			if (bOutProvided)
			{
				FString ComponentName;
				if (JsonTryGetString(Ctx.Params, ComponentField, ComponentName) && !ComponentName.TrimStartAndEnd().IsEmpty())
				{
					OutKey.ComponentName = FName(*ComponentName.TrimStartAndEnd());
				}
				FString SocketName;
				if (JsonTryGetString(Ctx.Params, SocketField, SocketName) && !SocketName.TrimStartAndEnd().IsEmpty())
				{
					OutKey.SocketName = FName(*SocketName.TrimStartAndEnd());
				}
			}

			return true;
		};

		const FName PropertyName(*PropertyNameText);
		UMovieSceneActorReferenceTrack* ActorReferenceTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneActorReferenceTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
		const bool bTrackAlreadyExists = ActorReferenceTrack != nullptr;
		if (!ActorReferenceTrack)
		{
			ActorReferenceTrack = MovieScene->AddTrack<UMovieSceneActorReferenceTrack>(BindingGuid);
		}
		if (!ActorReferenceTrack)
		{
			OutError = TEXT("create_actor_reference_property_track_failed");
			return false;
		}
		ActorReferenceTrack->SetPropertyNameAndPath(PropertyName, PropertyPathText);

		FMovieSceneActorReferenceKey ValueKey;
		bool bHasValueKey = false;
		FString ResolvedActorId;
		if (!ResolveActorReferenceKey(TEXT("value_binding_guid"), TEXT("value_actor_id"), TEXT("component_name"), TEXT("socket_name"), ValueKey, bHasValueKey, ResolvedActorId))
		{
			return false;
		}
		if (!bHasValueKey)
		{
			OutError = TEXT("missing_value_binding_guid_or_value_actor_id");
			return false;
		}

		FMovieSceneActorReferenceKey ValueBeforeKey;
		bool bHasValueBeforeKey = false;
		FString ResolvedBeforeActorId;
		if (!ResolveActorReferenceKey(TEXT("value_before_key_binding_guid"), TEXT("value_before_key_actor_id"), TEXT("value_before_key_component_name"), TEXT("value_before_key_socket_name"), ValueBeforeKey, bHasValueBeforeKey, ResolvedBeforeActorId))
		{
			return false;
		}

		UMovieSceneActorReferenceSection* ActorReferenceSection = nullptr;
		const TArray<UMovieSceneSection*>& ExistingSections = ActorReferenceTrack->GetAllSections();
		if (ExistingSections.Num() > 0)
		{
			ActorReferenceSection = Cast<UMovieSceneActorReferenceSection>(ExistingSections[0]);
		}
		if (!ActorReferenceSection)
		{
			ActorReferenceSection = Cast<UMovieSceneActorReferenceSection>(ActorReferenceTrack->CreateNewSection());
			if (!ActorReferenceSection)
			{
				OutError = TEXT("create_actor_reference_property_section_failed");
				return false;
			}
			ActorReferenceTrack->AddSection(*ActorReferenceSection);
		}

		const FFrameRate TickResolution = MovieScene->GetTickResolution();
		const FFrameNumber KeyFrame = TickResolution.AsFrameTime(TimeSeconds).RoundToFrame();
		const FFrameNumber PlaybackStartFrame = MovieScene->GetPlaybackRange().GetLowerBoundValue();

		ActorReferenceTrack->Modify();
		ActorReferenceSection->Modify();
		ActorReferenceSection->SetRange(TRange<FFrameNumber>::All());

		FMovieSceneActorReferenceData& Channel = const_cast<FMovieSceneActorReferenceData&>(ActorReferenceSection->GetActorReferenceData());
		if (bHasValueBeforeKey && Channel.GetData().GetTimes().Num() == 0 && KeyFrame > PlaybackStartFrame)
		{
			Channel.GetData().AddKey(PlaybackStartFrame, ValueBeforeKey);
		}
		Channel.GetData().UpdateOrAddKey(KeyFrame, ValueKey);

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
		OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetStringField(TEXT("property_type"), TEXT("actor_reference"));
		OutData->SetStringField(TEXT("property_name"), PropertyName.ToString());
		OutData->SetStringField(TEXT("property_path"), PropertyPathText);
		OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
		OutData->SetNumberField(TEXT("time_seconds"), TimeSeconds);
		OutData->SetNumberField(TEXT("frame_number"), KeyFrame.Value);
		OutData->SetStringField(TEXT("value_binding_guid"), ValueKey.Object.GetGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetStringField(TEXT("value_actor_id"), ResolvedActorId);
		OutData->SetStringField(TEXT("component_name"), ValueKey.ComponentName.ToString());
		OutData->SetStringField(TEXT("socket_name"), ValueKey.SocketName.ToString());
		if (bHasValueBeforeKey)
		{
			OutData->SetStringField(TEXT("value_before_key_binding_guid"), ValueBeforeKey.Object.GetGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
			OutData->SetStringField(TEXT("value_before_key_actor_id"), ResolvedBeforeActorId);
			OutData->SetStringField(TEXT("value_before_key_component_name"), ValueBeforeKey.ComponentName.ToString());
			OutData->SetStringField(TEXT("value_before_key_socket_name"), ValueBeforeKey.SocketName.ToString());
		}
		OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
		return true;
	}

	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::Object)
	{
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
		if (!MovieScene->FindBinding(BindingGuid))
		{
			OutError = TEXT("binding_not_found");
			return false;
		}

		Sequence->Modify();
		MovieScene->Modify();

		const FName PropertyName(*PropertyNameText);
		UClass* PropertyClass = nullptr;
		UObject* ValueObject = nullptr;
		UObject* ValueBeforeKeyObject = nullptr;
		if (!ResolveObjectPropertyClassAndValue(PropertyClass, ValueObject, ValueBeforeKeyObject))
		{
			return false;
		}

		UMovieSceneObjectPropertyTrack* ObjectTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneObjectPropertyTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
		const bool bTrackAlreadyExists = ObjectTrack != nullptr;
		if (!ObjectTrack)
		{
			ObjectTrack = MovieScene->AddTrack<UMovieSceneObjectPropertyTrack>(BindingGuid);
		}
		if (!ObjectTrack)
		{
			OutError = TEXT("create_object_property_track_failed");
			return false;
		}
		ObjectTrack->PropertyClass = PropertyClass;
		ObjectTrack->SetPropertyNameAndPath(PropertyName, PropertyPathText);

		UMovieSceneObjectPropertySection* ObjectSection = nullptr;
		const TArray<UMovieSceneSection*>& ExistingSections = ObjectTrack->GetAllSections();
		if (ExistingSections.Num() > 0)
		{
			ObjectSection = Cast<UMovieSceneObjectPropertySection>(ExistingSections[0]);
		}
		if (!ObjectSection)
		{
			ObjectSection = Cast<UMovieSceneObjectPropertySection>(ObjectTrack->CreateNewSection());
			if (!ObjectSection)
			{
				OutError = TEXT("create_object_property_section_failed");
				return false;
			}
			ObjectTrack->AddSection(*ObjectSection);
		}

		const FFrameRate TickResolution = MovieScene->GetTickResolution();
		const FFrameNumber KeyFrame = TickResolution.AsFrameTime(TimeSeconds).RoundToFrame();
		const FFrameNumber PlaybackStartFrame = MovieScene->GetPlaybackRange().GetLowerBoundValue();

		ObjectTrack->Modify();
		ObjectSection->Modify();
		ObjectSection->SetRange(TRange<FFrameNumber>::All());

		FMovieSceneObjectPathChannel& ObjectChannel = ObjectSection->ObjectChannel;
		TMovieSceneChannelData<FMovieSceneObjectPathChannelKeyValue> ChannelData = ObjectChannel.GetData();
		if (ValueBeforeKeyObject && ChannelData.GetTimes().Num() == 0 && KeyFrame > PlaybackStartFrame)
		{
			ChannelData.AddKey(PlaybackStartFrame, FMovieSceneObjectPathChannelKeyValue(ValueBeforeKeyObject));
		}
		ChannelData.UpdateOrAddKey(KeyFrame, FMovieSceneObjectPathChannelKeyValue(ValueObject));

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
		OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetStringField(TEXT("property_type"), TEXT("object"));
		OutData->SetStringField(TEXT("property_name"), PropertyName.ToString());
		OutData->SetStringField(TEXT("property_path"), PropertyPathText);
		OutData->SetStringField(TEXT("property_class_path"), PropertyClass->GetPathName());
		OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
		OutData->SetNumberField(TEXT("time_seconds"), TimeSeconds);
		OutData->SetNumberField(TEXT("frame_number"), KeyFrame.Value);
		OutData->SetStringField(TEXT("value_path"), ValueObject->GetPathName());
		OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
		return true;
	}

	if (PropertyType == UeAgentSequenceOps::ESequencePropertyTrackType::Double)
	{
		double ValueNumber = 0.0;
		if (!JsonTryGetNumber(Ctx.Params, TEXT("value"), ValueNumber))
		{
			OutError = TEXT("missing_value");
			return false;
		}

		double ValueBeforeKey = 0.0;
		const bool bHasValueBeforeKey = JsonTryGetNumber(Ctx.Params, TEXT("value_before_key"), ValueBeforeKey);

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
		if (!MovieScene->FindBinding(BindingGuid))
		{
			OutError = TEXT("binding_not_found");
			return false;
		}

		Sequence->Modify();
		MovieScene->Modify();

		const FName PropertyName(*PropertyNameText);
		UMovieSceneDoubleTrack* DoubleTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneDoubleTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
		const bool bTrackAlreadyExists = DoubleTrack != nullptr;
		if (!DoubleTrack)
		{
			DoubleTrack = MovieScene->AddTrack<UMovieSceneDoubleTrack>(BindingGuid);
		}
		if (!DoubleTrack)
		{
			OutError = TEXT("create_double_property_track_failed");
			return false;
		}
		DoubleTrack->SetPropertyNameAndPath(PropertyName, PropertyPathText);

		UMovieSceneDoubleSection* DoubleSection = nullptr;
		const TArray<UMovieSceneSection*>& ExistingSections = DoubleTrack->GetAllSections();
		if (ExistingSections.Num() > 0)
		{
			DoubleSection = Cast<UMovieSceneDoubleSection>(ExistingSections[0]);
		}
		if (!DoubleSection)
		{
			DoubleSection = Cast<UMovieSceneDoubleSection>(DoubleTrack->CreateNewSection());
			if (!DoubleSection)
			{
				OutError = TEXT("create_double_property_section_failed");
				return false;
			}
			DoubleTrack->AddSection(*DoubleSection);
		}

		const FFrameRate TickResolution = MovieScene->GetTickResolution();
		const FFrameNumber KeyFrame = TickResolution.AsFrameTime(TimeSeconds).RoundToFrame();
		const FFrameNumber PlaybackStartFrame = MovieScene->GetPlaybackRange().GetLowerBoundValue();

		DoubleTrack->Modify();
		DoubleSection->Modify();
		DoubleSection->SetRange(TRange<FFrameNumber>::All());

		FMovieSceneDoubleChannel& Channel = DoubleSection->GetChannel();
		if (bHasValueBeforeKey && Channel.GetData().GetTimes().Num() == 0 && KeyFrame > PlaybackStartFrame)
		{
			Channel.GetData().AddKey(PlaybackStartFrame, FMovieSceneDoubleValue(ValueBeforeKey));
		}
		Channel.GetData().UpdateOrAddKey(KeyFrame, FMovieSceneDoubleValue(ValueNumber));

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
		OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetStringField(TEXT("property_type"), TEXT("double"));
		OutData->SetStringField(TEXT("property_name"), PropertyName.ToString());
		OutData->SetStringField(TEXT("property_path"), PropertyPathText);
		OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
		OutData->SetNumberField(TEXT("time_seconds"), TimeSeconds);
		OutData->SetNumberField(TEXT("frame_number"), KeyFrame.Value);
		OutData->SetNumberField(TEXT("value"), ValueNumber);
		OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
		return true;
	}

	auto ParseColorObject = [](const TSharedPtr<FJsonObject>& Obj, const FString& FieldName, FLinearColor& OutColor) -> bool
	{
		const TSharedPtr<FJsonObject>* ColorObject = nullptr;
		if (!Obj.IsValid() || !Obj->TryGetObjectField(FieldName, ColorObject) || !ColorObject || !ColorObject->IsValid())
		{
			return false;
		}

		double Red = 0.0;
		double Green = 0.0;
		double Blue = 0.0;
		double Alpha = 1.0;
		const bool bHasRed = (*ColorObject)->TryGetNumberField(TEXT("r"), Red) || (*ColorObject)->TryGetNumberField(TEXT("red"), Red);
		const bool bHasGreen = (*ColorObject)->TryGetNumberField(TEXT("g"), Green) || (*ColorObject)->TryGetNumberField(TEXT("green"), Green);
		const bool bHasBlue = (*ColorObject)->TryGetNumberField(TEXT("b"), Blue) || (*ColorObject)->TryGetNumberField(TEXT("blue"), Blue);
		(*ColorObject)->TryGetNumberField(TEXT("a"), Alpha) || (*ColorObject)->TryGetNumberField(TEXT("alpha"), Alpha);
		if (!bHasRed || !bHasGreen || !bHasBlue)
		{
			return false;
		}
		OutColor = FLinearColor(static_cast<float>(Red), static_cast<float>(Green), static_cast<float>(Blue), static_cast<float>(Alpha));
		return true;
	};

	FLinearColor ColorValue = FLinearColor::White;
	bool bHasColorValue = ParseColorObject(Ctx.Params, TEXT("value"), ColorValue);
	if (!bHasColorValue)
	{
		double Red = 0.0;
		double Green = 0.0;
		double Blue = 0.0;
		double Alpha = 1.0;
		const bool bHasRed = JsonTryGetNumber(Ctx.Params, TEXT("red"), Red);
		const bool bHasGreen = JsonTryGetNumber(Ctx.Params, TEXT("green"), Green);
		const bool bHasBlue = JsonTryGetNumber(Ctx.Params, TEXT("blue"), Blue);
		JsonTryGetNumber(Ctx.Params, TEXT("alpha"), Alpha);
		if (!bHasRed || !bHasGreen || !bHasBlue)
		{
			OutError = TEXT("missing_or_invalid_color_value");
			return false;
		}
		ColorValue = FLinearColor(static_cast<float>(Red), static_cast<float>(Green), static_cast<float>(Blue), static_cast<float>(Alpha));
	}

	FLinearColor ColorValueBeforeKey = FLinearColor::Black;
	const bool bHasColorValueBeforeKey = ParseColorObject(Ctx.Params, TEXT("value_before_key"), ColorValueBeforeKey);

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
	if (!MovieScene->FindBinding(BindingGuid))
	{
		OutError = TEXT("binding_not_found");
		return false;
	}

	Sequence->Modify();
	MovieScene->Modify();

	const FName PropertyName(*PropertyNameText);

	UMovieSceneColorTrack* ColorTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneColorTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
	const bool bTrackAlreadyExists = ColorTrack != nullptr;
	if (!ColorTrack)
	{
		ColorTrack = MovieScene->AddTrack<UMovieSceneColorTrack>(BindingGuid);
	}
	if (!ColorTrack)
	{
		OutError = TEXT("create_color_property_track_failed");
		return false;
	}
	ColorTrack->SetPropertyNameAndPath(PropertyName, PropertyPathText);

	UMovieSceneColorSection* ColorSection = nullptr;
	const TArray<UMovieSceneSection*>& ExistingSections = ColorTrack->GetAllSections();
	if (ExistingSections.Num() > 0)
	{
		ColorSection = Cast<UMovieSceneColorSection>(ExistingSections[0]);
	}
	if (!ColorSection)
	{
		ColorSection = Cast<UMovieSceneColorSection>(ColorTrack->CreateNewSection());
		if (!ColorSection)
		{
			OutError = TEXT("create_color_property_section_failed");
			return false;
		}
		ColorTrack->AddSection(*ColorSection);
	}

	const FFrameRate TickResolution = MovieScene->GetTickResolution();
	const FFrameNumber KeyFrame = TickResolution.AsFrameTime(TimeSeconds).RoundToFrame();
	const FFrameNumber PlaybackStartFrame = MovieScene->GetPlaybackRange().GetLowerBoundValue();

	ColorTrack->Modify();
	ColorSection->Modify();
	ColorSection->SetRange(TRange<FFrameNumber>::All());

	auto AddColorKey = [KeyFrame](FMovieSceneFloatChannel& Channel, const float Value)
	{
		Channel.GetData().UpdateOrAddKey(KeyFrame, FMovieSceneFloatValue(Value));
	};

	const bool bChannelEmpty = ColorSection->GetRedChannel().GetData().GetTimes().Num() == 0 &&
		ColorSection->GetGreenChannel().GetData().GetTimes().Num() == 0 &&
		ColorSection->GetBlueChannel().GetData().GetTimes().Num() == 0 &&
		ColorSection->GetAlphaChannel().GetData().GetTimes().Num() == 0;
	if (bHasColorValueBeforeKey && bChannelEmpty && KeyFrame > PlaybackStartFrame)
	{
		ColorSection->GetRedChannel().GetData().AddKey(PlaybackStartFrame, FMovieSceneFloatValue(ColorValueBeforeKey.R));
		ColorSection->GetGreenChannel().GetData().AddKey(PlaybackStartFrame, FMovieSceneFloatValue(ColorValueBeforeKey.G));
		ColorSection->GetBlueChannel().GetData().AddKey(PlaybackStartFrame, FMovieSceneFloatValue(ColorValueBeforeKey.B));
		ColorSection->GetAlphaChannel().GetData().AddKey(PlaybackStartFrame, FMovieSceneFloatValue(ColorValueBeforeKey.A));
	}

	AddColorKey(ColorSection->GetRedChannel(), ColorValue.R);
	AddColorKey(ColorSection->GetGreenChannel(), ColorValue.G);
	AddColorKey(ColorSection->GetBlueChannel(), ColorValue.B);
	AddColorKey(ColorSection->GetAlphaChannel(), ColorValue.A);

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

	TSharedPtr<FJsonObject> ColorObj = MakeShared<FJsonObject>();
	ColorObj->SetNumberField(TEXT("r"), ColorValue.R);
	ColorObj->SetNumberField(TEXT("g"), ColorValue.G);
	ColorObj->SetNumberField(TEXT("b"), ColorValue.B);
	ColorObj->SetNumberField(TEXT("a"), ColorValue.A);

	OutData->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
	OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("property_type"), TEXT("color"));
	OutData->SetStringField(TEXT("property_name"), PropertyName.ToString());
	OutData->SetStringField(TEXT("property_path"), PropertyPathText);
	OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
	OutData->SetNumberField(TEXT("time_seconds"), TimeSeconds);
	OutData->SetNumberField(TEXT("frame_number"), KeyFrame.Value);
	OutData->SetObjectField(TEXT("value"), ColorObj);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdSequenceAddBoolPropertyTrack(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString BindingGuidText;
	if (!JsonTryGetString(Ctx.Params, TEXT("binding_guid"), BindingGuidText) || BindingGuidText.IsEmpty())
	{
		OutError = TEXT("missing_binding_guid");
		return false;
	}
	FGuid BindingGuid;
	if (!FGuid::Parse(BindingGuidText, BindingGuid))
	{
		OutError = TEXT("invalid_binding_guid");
		return false;
	}

	FString PropertyNameText;
	if (!JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyNameText) || PropertyNameText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_property_name");
		return false;
	}
	PropertyNameText = PropertyNameText.TrimStartAndEnd();

	FString PropertyPathText;
	JsonTryGetString(Ctx.Params, TEXT("property_path"), PropertyPathText);
	PropertyPathText = PropertyPathText.TrimStartAndEnd();
	if (PropertyPathText.IsEmpty())
	{
		PropertyPathText = PropertyNameText;
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
	if (!MovieScene->FindBinding(BindingGuid))
	{
		OutError = TEXT("binding_not_found");
		return false;
	}

	Sequence->Modify();
	MovieScene->Modify();

	const FName PropertyName(*PropertyNameText);
	UMovieSceneBoolTrack* BoolTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneBoolTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
	const bool bTrackAlreadyExists = BoolTrack != nullptr;
	if (!BoolTrack)
	{
		BoolTrack = MovieScene->AddTrack<UMovieSceneBoolTrack>(BindingGuid);
	}
	if (!BoolTrack)
	{
		OutError = TEXT("create_bool_property_track_failed");
		return false;
	}
	BoolTrack->SetPropertyNameAndPath(PropertyName, PropertyPathText);

	UMovieSceneBoolSection* BoolSection = nullptr;
	const TArray<UMovieSceneSection*>& ExistingSections = BoolTrack->GetAllSections();
	if (ExistingSections.Num() > 0)
	{
		BoolSection = Cast<UMovieSceneBoolSection>(ExistingSections[0]);
	}
	if (!BoolSection)
	{
		BoolSection = Cast<UMovieSceneBoolSection>(BoolTrack->CreateNewSection());
		if (!BoolSection)
		{
			OutError = TEXT("create_bool_property_section_failed");
			return false;
		}
		BoolTrack->AddSection(*BoolSection);
	}
	BoolSection->SetRange(TRange<FFrameNumber>::All());

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
	OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("property_name"), PropertyName.ToString());
	OutData->SetStringField(TEXT("property_path"), PropertyPathText);
	OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdSequenceAddBoolPropertyKey(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString BindingGuidText;
	if (!JsonTryGetString(Ctx.Params, TEXT("binding_guid"), BindingGuidText) || BindingGuidText.IsEmpty())
	{
		OutError = TEXT("missing_binding_guid");
		return false;
	}
	FGuid BindingGuid;
	if (!FGuid::Parse(BindingGuidText, BindingGuid))
	{
		OutError = TEXT("invalid_binding_guid");
		return false;
	}

	FString PropertyNameText;
	if (!JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyNameText) || PropertyNameText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_property_name");
		return false;
	}
	PropertyNameText = PropertyNameText.TrimStartAndEnd();

	FString PropertyPathText;
	JsonTryGetString(Ctx.Params, TEXT("property_path"), PropertyPathText);
	PropertyPathText = PropertyPathText.TrimStartAndEnd();
	if (PropertyPathText.IsEmpty())
	{
		PropertyPathText = PropertyNameText;
	}

	double TimeSeconds = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("time_seconds"), TimeSeconds))
	{
		OutError = TEXT("missing_time_seconds");
		return false;
	}

	bool bValue = false;
	if (!JsonTryGetBool(Ctx.Params, TEXT("value"), bValue))
	{
		OutError = TEXT("missing_value");
		return false;
	}

	bool bValueBeforeKey = false;
	const bool bHasValueBeforeKey = JsonTryGetBool(Ctx.Params, TEXT("value_before_key"), bValueBeforeKey);

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
	if (!MovieScene->FindBinding(BindingGuid))
	{
		OutError = TEXT("binding_not_found");
		return false;
	}

	Sequence->Modify();
	MovieScene->Modify();

	const FName PropertyName(*PropertyNameText);
	UMovieSceneBoolTrack* BoolTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneBoolTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
	const bool bTrackAlreadyExists = BoolTrack != nullptr;
	if (!BoolTrack)
	{
		BoolTrack = MovieScene->AddTrack<UMovieSceneBoolTrack>(BindingGuid);
	}
	if (!BoolTrack)
	{
		OutError = TEXT("create_bool_property_track_failed");
		return false;
	}
	BoolTrack->SetPropertyNameAndPath(PropertyName, PropertyPathText);

	UMovieSceneBoolSection* BoolSection = nullptr;
	const TArray<UMovieSceneSection*>& ExistingSections = BoolTrack->GetAllSections();
	if (ExistingSections.Num() > 0)
	{
		BoolSection = Cast<UMovieSceneBoolSection>(ExistingSections[0]);
	}
	if (!BoolSection)
	{
		BoolSection = Cast<UMovieSceneBoolSection>(BoolTrack->CreateNewSection());
		if (!BoolSection)
		{
			OutError = TEXT("create_bool_property_section_failed");
			return false;
		}
		BoolTrack->AddSection(*BoolSection);
	}

	const FFrameRate TickResolution = MovieScene->GetTickResolution();
	const FFrameNumber KeyFrame = TickResolution.AsFrameTime(TimeSeconds).RoundToFrame();
	const FFrameNumber PlaybackStartFrame = MovieScene->GetPlaybackRange().GetLowerBoundValue();

	BoolTrack->Modify();
	BoolSection->Modify();
	BoolSection->SetRange(TRange<FFrameNumber>::All());
	FMovieSceneBoolChannel& Channel = BoolSection->GetChannel();
	if (bHasValueBeforeKey && Channel.GetData().GetTimes().Num() == 0 && KeyFrame > PlaybackStartFrame)
	{
		Channel.GetData().AddKey(PlaybackStartFrame, bValueBeforeKey);
	}
	Channel.GetData().UpdateOrAddKey(KeyFrame, bValue);

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
	OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("property_name"), PropertyName.ToString());
	OutData->SetStringField(TEXT("property_path"), PropertyPathText);
	OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
	OutData->SetNumberField(TEXT("time_seconds"), TimeSeconds);
	OutData->SetNumberField(TEXT("frame_number"), KeyFrame.Value);
	OutData->SetBoolField(TEXT("value"), bValue);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdSequenceAddFloatPropertyTrack(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString BindingGuidText;
	if (!JsonTryGetString(Ctx.Params, TEXT("binding_guid"), BindingGuidText) || BindingGuidText.IsEmpty())
	{
		OutError = TEXT("missing_binding_guid");
		return false;
	}
	FGuid BindingGuid;
	if (!FGuid::Parse(BindingGuidText, BindingGuid))
	{
		OutError = TEXT("invalid_binding_guid");
		return false;
	}

	FString PropertyNameText;
	if (!JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyNameText) || PropertyNameText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_property_name");
		return false;
	}
	PropertyNameText = PropertyNameText.TrimStartAndEnd();

	FString PropertyPathText;
	JsonTryGetString(Ctx.Params, TEXT("property_path"), PropertyPathText);
	PropertyPathText = PropertyPathText.TrimStartAndEnd();
	if (PropertyPathText.IsEmpty())
	{
		PropertyPathText = PropertyNameText;
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
	if (!MovieScene->FindBinding(BindingGuid))
	{
		OutError = TEXT("binding_not_found");
		return false;
	}

	Sequence->Modify();
	MovieScene->Modify();

	const FName PropertyName(*PropertyNameText);
	UMovieSceneFloatTrack* FloatTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneFloatTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
	const bool bTrackAlreadyExists = FloatTrack != nullptr;
	if (!FloatTrack)
	{
		FloatTrack = MovieScene->AddTrack<UMovieSceneFloatTrack>(BindingGuid);
	}
	if (!FloatTrack)
	{
		OutError = TEXT("create_float_property_track_failed");
		return false;
	}
	FloatTrack->SetPropertyNameAndPath(PropertyName, PropertyPathText);

	UMovieSceneFloatSection* FloatSection = nullptr;
	const TArray<UMovieSceneSection*>& ExistingSections = FloatTrack->GetAllSections();
	if (ExistingSections.Num() > 0)
	{
		FloatSection = Cast<UMovieSceneFloatSection>(ExistingSections[0]);
	}
	if (!FloatSection)
	{
		FloatSection = Cast<UMovieSceneFloatSection>(FloatTrack->CreateNewSection());
		if (!FloatSection)
		{
			OutError = TEXT("create_float_property_section_failed");
			return false;
		}
		FloatTrack->AddSection(*FloatSection);
	}
	FloatSection->SetRange(TRange<FFrameNumber>::All());

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
	OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("property_name"), PropertyName.ToString());
	OutData->SetStringField(TEXT("property_path"), PropertyPathText);
	OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdSequenceAddFloatPropertyKey(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString BindingGuidText;
	if (!JsonTryGetString(Ctx.Params, TEXT("binding_guid"), BindingGuidText) || BindingGuidText.IsEmpty())
	{
		OutError = TEXT("missing_binding_guid");
		return false;
	}
	FGuid BindingGuid;
	if (!FGuid::Parse(BindingGuidText, BindingGuid))
	{
		OutError = TEXT("invalid_binding_guid");
		return false;
	}

	FString PropertyNameText;
	if (!JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyNameText) || PropertyNameText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_property_name");
		return false;
	}
	PropertyNameText = PropertyNameText.TrimStartAndEnd();

	FString PropertyPathText;
	JsonTryGetString(Ctx.Params, TEXT("property_path"), PropertyPathText);
	PropertyPathText = PropertyPathText.TrimStartAndEnd();
	if (PropertyPathText.IsEmpty())
	{
		PropertyPathText = PropertyNameText;
	}

	double TimeSeconds = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("time_seconds"), TimeSeconds))
	{
		OutError = TEXT("missing_time_seconds");
		return false;
	}

	double ValueNumber = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("value"), ValueNumber))
	{
		OutError = TEXT("missing_value");
		return false;
	}

	double ValueBeforeKey = 0.0;
	const bool bHasValueBeforeKey = JsonTryGetNumber(Ctx.Params, TEXT("value_before_key"), ValueBeforeKey);

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
	if (!MovieScene->FindBinding(BindingGuid))
	{
		OutError = TEXT("binding_not_found");
		return false;
	}

	Sequence->Modify();
	MovieScene->Modify();

	const FName PropertyName(*PropertyNameText);
	UMovieSceneFloatTrack* FloatTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneFloatTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
	const bool bTrackAlreadyExists = FloatTrack != nullptr;
	if (!FloatTrack)
	{
		FloatTrack = MovieScene->AddTrack<UMovieSceneFloatTrack>(BindingGuid);
	}
	if (!FloatTrack)
	{
		OutError = TEXT("create_float_property_track_failed");
		return false;
	}
	FloatTrack->SetPropertyNameAndPath(PropertyName, PropertyPathText);

	UMovieSceneFloatSection* FloatSection = nullptr;
	const TArray<UMovieSceneSection*>& ExistingSections = FloatTrack->GetAllSections();
	if (ExistingSections.Num() > 0)
	{
		FloatSection = Cast<UMovieSceneFloatSection>(ExistingSections[0]);
	}
	if (!FloatSection)
	{
		FloatSection = Cast<UMovieSceneFloatSection>(FloatTrack->CreateNewSection());
		if (!FloatSection)
		{
			OutError = TEXT("create_float_property_section_failed");
			return false;
		}
		FloatTrack->AddSection(*FloatSection);
	}

	const FFrameRate TickResolution = MovieScene->GetTickResolution();
	const FFrameNumber KeyFrame = TickResolution.AsFrameTime(TimeSeconds).RoundToFrame();
	const FFrameNumber PlaybackStartFrame = MovieScene->GetPlaybackRange().GetLowerBoundValue();

	FloatTrack->Modify();
	FloatSection->Modify();
	FloatSection->SetRange(TRange<FFrameNumber>::All());
	FMovieSceneFloatChannel& Channel = FloatSection->GetChannel();
	if (bHasValueBeforeKey && Channel.GetData().GetTimes().Num() == 0 && KeyFrame > PlaybackStartFrame)
	{
		Channel.GetData().AddKey(PlaybackStartFrame, FMovieSceneFloatValue(static_cast<float>(ValueBeforeKey)));
	}
	Channel.GetData().UpdateOrAddKey(KeyFrame, FMovieSceneFloatValue(static_cast<float>(ValueNumber)));

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
	OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("property_name"), PropertyName.ToString());
	OutData->SetStringField(TEXT("property_path"), PropertyPathText);
	OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
	OutData->SetNumberField(TEXT("time_seconds"), TimeSeconds);
	OutData->SetNumberField(TEXT("frame_number"), KeyFrame.Value);
	OutData->SetNumberField(TEXT("value"), ValueNumber);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdSequenceAddIntegerPropertyTrack(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString BindingGuidText;
	if (!JsonTryGetString(Ctx.Params, TEXT("binding_guid"), BindingGuidText) || BindingGuidText.IsEmpty())
	{
		OutError = TEXT("missing_binding_guid");
		return false;
	}
	FGuid BindingGuid;
	if (!FGuid::Parse(BindingGuidText, BindingGuid))
	{
		OutError = TEXT("invalid_binding_guid");
		return false;
	}

	FString PropertyNameText;
	if (!JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyNameText) || PropertyNameText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_property_name");
		return false;
	}
	PropertyNameText = PropertyNameText.TrimStartAndEnd();

	FString PropertyPathText;
	JsonTryGetString(Ctx.Params, TEXT("property_path"), PropertyPathText);
	PropertyPathText = PropertyPathText.TrimStartAndEnd();
	if (PropertyPathText.IsEmpty())
	{
		PropertyPathText = PropertyNameText;
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
	if (!MovieScene->FindBinding(BindingGuid))
	{
		OutError = TEXT("binding_not_found");
		return false;
	}

	Sequence->Modify();
	MovieScene->Modify();

	const FName PropertyName(*PropertyNameText);
	UMovieSceneIntegerTrack* IntegerTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneIntegerTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
	const bool bTrackAlreadyExists = IntegerTrack != nullptr;
	if (!IntegerTrack)
	{
		IntegerTrack = MovieScene->AddTrack<UMovieSceneIntegerTrack>(BindingGuid);
	}
	if (!IntegerTrack)
	{
		OutError = TEXT("create_integer_property_track_failed");
		return false;
	}
	IntegerTrack->SetPropertyNameAndPath(PropertyName, PropertyPathText);

	UMovieSceneIntegerSection* IntegerSection = nullptr;
	const TArray<UMovieSceneSection*>& ExistingSections = IntegerTrack->GetAllSections();
	if (ExistingSections.Num() > 0)
	{
		IntegerSection = Cast<UMovieSceneIntegerSection>(ExistingSections[0]);
	}
	if (!IntegerSection)
	{
		IntegerSection = Cast<UMovieSceneIntegerSection>(IntegerTrack->CreateNewSection());
		if (!IntegerSection)
		{
			OutError = TEXT("create_integer_property_section_failed");
			return false;
		}
		IntegerTrack->AddSection(*IntegerSection);
	}
	IntegerSection->SetRange(TRange<FFrameNumber>::All());

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
	OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("property_name"), PropertyName.ToString());
	OutData->SetStringField(TEXT("property_path"), PropertyPathText);
	OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdSequenceAddIntegerPropertyKey(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString BindingGuidText;
	if (!JsonTryGetString(Ctx.Params, TEXT("binding_guid"), BindingGuidText) || BindingGuidText.IsEmpty())
	{
		OutError = TEXT("missing_binding_guid");
		return false;
	}
	FGuid BindingGuid;
	if (!FGuid::Parse(BindingGuidText, BindingGuid))
	{
		OutError = TEXT("invalid_binding_guid");
		return false;
	}

	FString PropertyNameText;
	if (!JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyNameText) || PropertyNameText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_property_name");
		return false;
	}
	PropertyNameText = PropertyNameText.TrimStartAndEnd();

	FString PropertyPathText;
	JsonTryGetString(Ctx.Params, TEXT("property_path"), PropertyPathText);
	PropertyPathText = PropertyPathText.TrimStartAndEnd();
	if (PropertyPathText.IsEmpty())
	{
		PropertyPathText = PropertyNameText;
	}

	double TimeSeconds = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("time_seconds"), TimeSeconds))
	{
		OutError = TEXT("missing_time_seconds");
		return false;
	}

	double ValueNumber = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("value"), ValueNumber))
	{
		OutError = TEXT("missing_value");
		return false;
	}

	double ValueBeforeKey = 0.0;
	const bool bHasValueBeforeKey = JsonTryGetNumber(Ctx.Params, TEXT("value_before_key"), ValueBeforeKey);

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
	if (!MovieScene->FindBinding(BindingGuid))
	{
		OutError = TEXT("binding_not_found");
		return false;
	}

	Sequence->Modify();
	MovieScene->Modify();

	const FName PropertyName(*PropertyNameText);
	UMovieSceneIntegerTrack* IntegerTrack = UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneIntegerTrack>(MovieScene, BindingGuid, PropertyName, PropertyPathText);
	const bool bTrackAlreadyExists = IntegerTrack != nullptr;
	if (!IntegerTrack)
	{
		IntegerTrack = MovieScene->AddTrack<UMovieSceneIntegerTrack>(BindingGuid);
	}
	if (!IntegerTrack)
	{
		OutError = TEXT("create_integer_property_track_failed");
		return false;
	}
	IntegerTrack->SetPropertyNameAndPath(PropertyName, PropertyPathText);

	UMovieSceneIntegerSection* IntegerSection = nullptr;
	const TArray<UMovieSceneSection*>& ExistingSections = IntegerTrack->GetAllSections();
	if (ExistingSections.Num() > 0)
	{
		IntegerSection = Cast<UMovieSceneIntegerSection>(ExistingSections[0]);
	}
	if (!IntegerSection)
	{
		IntegerSection = Cast<UMovieSceneIntegerSection>(IntegerTrack->CreateNewSection());
		if (!IntegerSection)
		{
			OutError = TEXT("create_integer_property_section_failed");
			return false;
		}
		IntegerTrack->AddSection(*IntegerSection);
	}

	const FFrameRate TickResolution = MovieScene->GetTickResolution();
	const FFrameNumber KeyFrame = TickResolution.AsFrameTime(TimeSeconds).RoundToFrame();
	const FFrameNumber PlaybackStartFrame = MovieScene->GetPlaybackRange().GetLowerBoundValue();

	IntegerTrack->Modify();
	IntegerSection->Modify();
	IntegerSection->SetRange(TRange<FFrameNumber>::All());
	TArrayView<FMovieSceneIntegerChannel*> Channels = IntegerSection->GetChannelProxy().GetChannels<FMovieSceneIntegerChannel>();
	if (Channels.Num() <= 0)
	{
		OutError = TEXT("invalid_integer_channel_count");
		return false;
	}
	FMovieSceneIntegerChannel* Channel = Channels[0];
	if (bHasValueBeforeKey && Channel->GetData().GetTimes().Num() == 0 && KeyFrame > PlaybackStartFrame)
	{
		Channel->GetData().AddKey(PlaybackStartFrame, static_cast<int32>(FMath::RoundToInt(ValueBeforeKey)));
	}
	Channel->GetData().UpdateOrAddKey(KeyFrame, static_cast<int32>(FMath::RoundToInt(ValueNumber)));

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
	OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("property_name"), PropertyName.ToString());
	OutData->SetStringField(TEXT("property_path"), PropertyPathText);
	OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
	OutData->SetNumberField(TEXT("time_seconds"), TimeSeconds);
	OutData->SetNumberField(TEXT("frame_number"), KeyFrame.Value);
	OutData->SetNumberField(TEXT("value"), static_cast<int32>(FMath::RoundToInt(ValueNumber)));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdSequenceAddTransformKey(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString BindingGuidText;
	if (!JsonTryGetString(Ctx.Params, TEXT("binding_guid"), BindingGuidText) || BindingGuidText.IsEmpty())
	{
		OutError = TEXT("missing_binding_guid");
		return false;
	}
	FGuid BindingGuid;
	if (!FGuid::Parse(BindingGuidText, BindingGuid))
	{
		OutError = TEXT("invalid_binding_guid");
		return false;
	}

	double TimeSeconds = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("time_seconds"), TimeSeconds))
	{
		OutError = TEXT("missing_time_seconds");
		return false;
	}

	FVector Location(0.0, 0.0, 0.0);
	FVector Scale(1.0, 1.0, 1.0);
	FRotator Rotation(0.0, 0.0, 0.0);
	const bool bHasLocation = JsonTryGetVector(Ctx.Params, TEXT("location"), Location);
	const bool bHasScale = JsonTryGetVector(Ctx.Params, TEXT("scale"), Scale);
	const bool bHasRotation = JsonTryGetRotator(Ctx.Params, TEXT("rotation"), Rotation);
	if (!bHasLocation && !bHasScale && !bHasRotation)
	{
		OutError = TEXT("missing_transform_values");
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

	UMovieScene3DTransformTrack* TransformTrack = MovieScene->FindTrack<UMovieScene3DTransformTrack>(BindingGuid);
	if (!TransformTrack)
	{
		OutError = TEXT("transform_track_not_found");
		return false;
	}

	UMovieScene3DTransformSection* TransformSection = nullptr;
	const TArray<UMovieSceneSection*>& ExistingSections = TransformTrack->GetAllSections();
	if (ExistingSections.Num() > 0)
	{
		TransformSection = Cast<UMovieScene3DTransformSection>(ExistingSections[0]);
	}
	if (!TransformSection)
	{
		TransformSection = Cast<UMovieScene3DTransformSection>(TransformTrack->CreateNewSection());
		if (!TransformSection)
		{
			OutError = TEXT("create_transform_section_failed");
			return false;
		}
		TransformTrack->AddSection(*TransformSection);
	}

	TArrayView<FMovieSceneDoubleChannel*> Channels = TransformSection->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
	if (Channels.Num() < 9)
	{
		OutError = TEXT("invalid_transform_channel_count");
		return false;
	}

	const FFrameRate TickResolution = MovieScene->GetTickResolution();
	const FFrameNumber KeyFrame = TickResolution.AsFrameTime(TimeSeconds).RoundToFrame();

	Sequence->Modify();
	MovieScene->Modify();
	TransformTrack->Modify();
	TransformSection->Modify();
	TransformSection->SetRange(TRange<FFrameNumber>::All());

	auto SetKey = [&Channels, KeyFrame](const int32 ChannelIndex, const double Value)
	{
		Channels[ChannelIndex]->GetData().UpdateOrAddKey(KeyFrame, FMovieSceneDoubleValue(Value));
	};

	if (bHasLocation)
	{
		SetKey(0, Location.X);
		SetKey(1, Location.Y);
		SetKey(2, Location.Z);
	}
	if (bHasRotation)
	{
		SetKey(3, Rotation.Roll);
		SetKey(4, Rotation.Pitch);
		SetKey(5, Rotation.Yaw);
	}
	if (bHasScale)
	{
		SetKey(6, Scale.X);
		SetKey(7, Scale.Y);
		SetKey(8, Scale.Z);
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
	OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetNumberField(TEXT("time_seconds"), TimeSeconds);
	OutData->SetNumberField(TEXT("frame_number"), KeyFrame.Value);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdSequenceAddSkeletalAnimationTrack(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString BindingGuidText;
	if (!JsonTryGetString(Ctx.Params, TEXT("binding_guid"), BindingGuidText) || BindingGuidText.IsEmpty())
	{
		OutError = TEXT("missing_binding_guid");
		return false;
	}
	FGuid BindingGuid;
	if (!FGuid::Parse(BindingGuidText, BindingGuid))
	{
		OutError = TEXT("invalid_binding_guid");
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
	if (!MovieScene->FindBinding(BindingGuid))
	{
		OutError = TEXT("binding_not_found");
		return false;
	}

	Sequence->Modify();
	MovieScene->Modify();

	UMovieSceneSkeletalAnimationTrack* SkeletalAnimationTrack = MovieScene->FindTrack<UMovieSceneSkeletalAnimationTrack>(BindingGuid);
	const bool bTrackAlreadyExists = SkeletalAnimationTrack != nullptr;
	if (!SkeletalAnimationTrack)
	{
		SkeletalAnimationTrack = MovieScene->AddTrack<UMovieSceneSkeletalAnimationTrack>(BindingGuid);
	}
	if (!SkeletalAnimationTrack)
	{
		OutError = TEXT("create_skeletal_animation_track_failed");
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
	OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdSequenceAddSkeletalAnimationSection(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString BindingGuidText;
	if (!JsonTryGetString(Ctx.Params, TEXT("binding_guid"), BindingGuidText) || BindingGuidText.IsEmpty())
	{
		OutError = TEXT("missing_binding_guid");
		return false;
	}
	FGuid BindingGuid;
	if (!FGuid::Parse(BindingGuidText, BindingGuid))
	{
		OutError = TEXT("invalid_binding_guid");
		return false;
	}

	FString AnimationAssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("animation_asset"), AnimationAssetPath) || AnimationAssetPath.IsEmpty())
	{
		OutError = TEXT("missing_animation_asset");
		return false;
	}

	double StartSeconds = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("start_seconds"), StartSeconds))
	{
		OutError = TEXT("missing_start_seconds");
		return false;
	}

	double PlayRateValue = 1.0;
	JsonTryGetNumber(Ctx.Params, TEXT("play_rate"), PlayRateValue);
	if (FMath::IsNearlyZero(PlayRateValue))
	{
		OutError = TEXT("invalid_play_rate");
		return false;
	}

	UAnimSequenceBase* AnimationAsset = UeAgentSequenceOps::LoadAnimSequenceBase(AnimationAssetPath);
	if (!AnimationAsset)
	{
		OutError = TEXT("animation_asset_not_found");
		return false;
	}

	double DurationSeconds = AnimationAsset->GetPlayLength() / FMath::Abs(PlayRateValue);
	JsonTryGetNumber(Ctx.Params, TEXT("duration_seconds"), DurationSeconds);
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
	if (!MovieScene->FindBinding(BindingGuid))
	{
		OutError = TEXT("binding_not_found");
		return false;
	}

	Sequence->Modify();
	MovieScene->Modify();

	UMovieSceneSkeletalAnimationTrack* SkeletalAnimationTrack = MovieScene->FindTrack<UMovieSceneSkeletalAnimationTrack>(BindingGuid);
	const bool bTrackAlreadyExists = SkeletalAnimationTrack != nullptr;
	if (!SkeletalAnimationTrack)
	{
		SkeletalAnimationTrack = MovieScene->AddTrack<UMovieSceneSkeletalAnimationTrack>(BindingGuid);
	}
	if (!SkeletalAnimationTrack)
	{
		OutError = TEXT("create_skeletal_animation_track_failed");
		return false;
	}

	UMovieSceneSkeletalAnimationSection* NewSection = Cast<UMovieSceneSkeletalAnimationSection>(SkeletalAnimationTrack->CreateNewSection());
	if (!NewSection)
	{
		OutError = TEXT("create_skeletal_animation_section_failed");
		return false;
	}

	const FFrameRate TickResolution = MovieScene->GetTickResolution();
	FFrameNumber StartFrame = TickResolution.AsFrameTime(StartSeconds).RoundToFrame();
	FFrameNumber EndExclusiveFrame = TickResolution.AsFrameTime(StartSeconds + DurationSeconds).CeilToFrame();
	if (EndExclusiveFrame.Value <= StartFrame.Value)
	{
		EndExclusiveFrame = FFrameNumber(StartFrame.Value + 1);
	}

	NewSection->Modify();
	NewSection->SetRange(TRange<FFrameNumber>(StartFrame, EndExclusiveFrame));
	NewSection->Params.Animation = AnimationAsset;
	NewSection->Params.PlayRate = FMovieSceneTimeWarpVariant(PlayRateValue);

	bool bReverse = false;
	JsonTryGetBool(Ctx.Params, TEXT("reverse"), bReverse);
	NewSection->Params.bReverse = bReverse;

	FString SlotName;
	if (JsonTryGetString(Ctx.Params, TEXT("slot_name"), SlotName) && !SlotName.TrimStartAndEnd().IsEmpty())
	{
		NewSection->Params.SlotName = FName(*SlotName.TrimStartAndEnd());
	}

	SkeletalAnimationTrack->AddSection(*NewSection);
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
	OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("animation_asset"), AnimationAsset->GetPathName());
	OutData->SetBoolField(TEXT("track_already_exists"), bTrackAlreadyExists);
	OutData->SetNumberField(TEXT("section_index"), SkeletalAnimationTrack->GetAllSections().Num() - 1);
	OutData->SetNumberField(TEXT("start_seconds"), StartSeconds);
	OutData->SetNumberField(TEXT("duration_seconds"), DurationSeconds);
	OutData->SetNumberField(TEXT("play_rate"), PlayRateValue);
	OutData->SetBoolField(TEXT("reverse"), bReverse);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdSequenceUpdateSkeletalAnimationSection(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString BindingGuidText;
	if (!JsonTryGetString(Ctx.Params, TEXT("binding_guid"), BindingGuidText) || BindingGuidText.IsEmpty())
	{
		OutError = TEXT("missing_binding_guid");
		return false;
	}
	FGuid BindingGuid;
	if (!FGuid::Parse(BindingGuidText, BindingGuid))
	{
		OutError = TEXT("invalid_binding_guid");
		return false;
	}

	double SectionIndexValue = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("section_index"), SectionIndexValue))
	{
		OutError = TEXT("missing_section_index");
		return false;
	}
	const int32 SectionIndex = FMath::RoundToInt(SectionIndexValue);

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
	if (!MovieScene->FindBinding(BindingGuid))
	{
		OutError = TEXT("binding_not_found");
		return false;
	}

	UMovieSceneSkeletalAnimationTrack* SkeletalAnimationTrack = MovieScene->FindTrack<UMovieSceneSkeletalAnimationTrack>(BindingGuid);
	if (!SkeletalAnimationTrack)
	{
		OutError = TEXT("skeletal_animation_track_not_found");
		return false;
	}

	const TArray<UMovieSceneSection*>& Sections = SkeletalAnimationTrack->GetAllSections();
	if (!Sections.IsValidIndex(SectionIndex))
	{
		OutError = TEXT("skeletal_animation_section_not_found");
		return false;
	}

	UMovieSceneSkeletalAnimationSection* TargetSection = Cast<UMovieSceneSkeletalAnimationSection>(Sections[SectionIndex]);
	if (!TargetSection)
	{
		OutError = TEXT("skeletal_animation_section_not_found");
		return false;
	}

	Sequence->Modify();
	MovieScene->Modify();
	SkeletalAnimationTrack->Modify();
	TargetSection->Modify();

	UAnimSequenceBase* AnimationAsset = TargetSection->Params.Animation;
	FString AnimationAssetPath;
	if (JsonTryGetString(Ctx.Params, TEXT("animation_asset"), AnimationAssetPath) && !AnimationAssetPath.TrimStartAndEnd().IsEmpty())
	{
		AnimationAsset = UeAgentSequenceOps::LoadAnimSequenceBase(AnimationAssetPath);
		if (!AnimationAsset)
		{
			OutError = TEXT("animation_asset_not_found");
			return false;
		}
		TargetSection->Params.Animation = AnimationAsset;
	}

	const FFrameRate TickResolution = MovieScene->GetTickResolution();
	const TRange<FFrameNumber> ExistingRange = TargetSection->GetRange();
	double StartSeconds = ExistingRange.HasLowerBound() ? TickResolution.AsSeconds(ExistingRange.GetLowerBoundValue()) : 0.0;
	if (ExistingRange.HasUpperBound() && ExistingRange.HasLowerBound())
	{
		StartSeconds = TickResolution.AsSeconds(ExistingRange.GetLowerBoundValue());
	}
	JsonTryGetNumber(Ctx.Params, TEXT("start_seconds"), StartSeconds);

	double DurationSeconds = 1.0;
	if (ExistingRange.HasLowerBound() && ExistingRange.HasUpperBound())
	{
		DurationSeconds = TickResolution.AsSeconds(ExistingRange.GetUpperBoundValue() - ExistingRange.GetLowerBoundValue());
	}
	if (AnimationAsset && DurationSeconds <= 0.0)
	{
		double ExistingPlayRate = 1.0;
		if (TargetSection->Params.PlayRate.GetType() == EMovieSceneTimeWarpType::FixedPlayRate)
		{
			ExistingPlayRate = TargetSection->Params.PlayRate.AsFixedPlayRateFloat();
		}
		DurationSeconds = AnimationAsset->GetPlayLength() / FMath::Max(KINDA_SMALL_NUMBER, FMath::Abs(ExistingPlayRate));
	}
	if (JsonTryGetNumber(Ctx.Params, TEXT("duration_seconds"), DurationSeconds) && DurationSeconds <= 0.0)
	{
		OutError = TEXT("invalid_duration_seconds");
		return false;
	}

	double PlayRateValue = 1.0;
	if (TargetSection->Params.PlayRate.GetType() == EMovieSceneTimeWarpType::FixedPlayRate)
	{
		PlayRateValue = TargetSection->Params.PlayRate.AsFixedPlayRateFloat();
	}
	if (JsonTryGetNumber(Ctx.Params, TEXT("play_rate"), PlayRateValue))
	{
		if (FMath::IsNearlyZero(PlayRateValue))
		{
			OutError = TEXT("invalid_play_rate");
			return false;
		}
		TargetSection->Params.PlayRate = FMovieSceneTimeWarpVariant(PlayRateValue);
	}

	bool bReverse = TargetSection->Params.bReverse;
	if (JsonTryGetBool(Ctx.Params, TEXT("reverse"), bReverse))
	{
		TargetSection->Params.bReverse = bReverse;
	}

	bool bSkipAnimNotifiers = TargetSection->Params.bSkipAnimNotifiers;
	if (JsonTryGetBool(Ctx.Params, TEXT("skip_anim_notifiers"), bSkipAnimNotifiers))
	{
		TargetSection->Params.bSkipAnimNotifiers = bSkipAnimNotifiers;
	}

	bool bForceCustomMode = TargetSection->Params.bForceCustomMode;
	if (JsonTryGetBool(Ctx.Params, TEXT("force_custom_mode"), bForceCustomMode))
	{
		TargetSection->Params.bForceCustomMode = bForceCustomMode;
	}

	FString SlotName;
	if (JsonTryGetString(Ctx.Params, TEXT("slot_name"), SlotName))
	{
		TargetSection->Params.SlotName = FName(*SlotName.TrimStartAndEnd());
	}
	bool bClearSlotName = false;
	if (JsonTryGetBool(Ctx.Params, TEXT("clear_slot_name"), bClearSlotName) && bClearSlotName)
	{
		TargetSection->Params.SlotName = NAME_None;
	}

	double FirstLoopStartOffsetSeconds = TickResolution.AsSeconds(TargetSection->Params.FirstLoopStartFrameOffset);
	if (JsonTryGetNumber(Ctx.Params, TEXT("first_loop_start_offset_seconds"), FirstLoopStartOffsetSeconds))
	{
		TargetSection->Params.FirstLoopStartFrameOffset = TickResolution.AsFrameTime(FirstLoopStartOffsetSeconds).RoundToFrame();
	}

	double StartOffsetSeconds = TickResolution.AsSeconds(TargetSection->Params.StartFrameOffset);
	if (JsonTryGetNumber(Ctx.Params, TEXT("start_offset_seconds"), StartOffsetSeconds))
	{
		TargetSection->Params.StartFrameOffset = TickResolution.AsFrameTime(StartOffsetSeconds).RoundToFrame();
	}

	double EndOffsetSeconds = TickResolution.AsSeconds(TargetSection->Params.EndFrameOffset);
	if (JsonTryGetNumber(Ctx.Params, TEXT("end_offset_seconds"), EndOffsetSeconds))
	{
		TargetSection->Params.EndFrameOffset = TickResolution.AsFrameTime(EndOffsetSeconds).RoundToFrame();
	}

	double RowIndexValue = static_cast<double>(TargetSection->GetRowIndex());
	if (JsonTryGetNumber(Ctx.Params, TEXT("row_index"), RowIndexValue))
	{
		TargetSection->SetRowIndex(FMath::Max(0, FMath::RoundToInt(RowIndexValue)));
	}

	FFrameNumber StartFrame = TickResolution.AsFrameTime(StartSeconds).RoundToFrame();
	FFrameNumber EndExclusiveFrame = TickResolution.AsFrameTime(StartSeconds + DurationSeconds).CeilToFrame();
	if (EndExclusiveFrame.Value <= StartFrame.Value)
	{
		EndExclusiveFrame = FFrameNumber(StartFrame.Value + 1);
	}
	TargetSection->SetRange(TRange<FFrameNumber>(StartFrame, EndExclusiveFrame));

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
	OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetNumberField(TEXT("section_index"), SectionIndex);
	OutData->SetStringField(TEXT("animation_asset"), TargetSection->Params.Animation ? TargetSection->Params.Animation->GetPathName() : TEXT(""));
	OutData->SetNumberField(TEXT("start_seconds"), StartSeconds);
	OutData->SetNumberField(TEXT("duration_seconds"), DurationSeconds);
	OutData->SetNumberField(TEXT("play_rate"), PlayRateValue);
	OutData->SetBoolField(TEXT("reverse"), TargetSection->Params.bReverse);
	OutData->SetStringField(TEXT("slot_name"), TargetSection->Params.SlotName.ToString());
	OutData->SetNumberField(TEXT("row_index"), TargetSection->GetRowIndex());
	OutData->SetBoolField(TEXT("skip_anim_notifiers"), TargetSection->Params.bSkipAnimNotifiers);
	OutData->SetBoolField(TEXT("force_custom_mode"), TargetSection->Params.bForceCustomMode);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdSequenceRemoveSkeletalAnimationSection(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString BindingGuidText;
	if (!JsonTryGetString(Ctx.Params, TEXT("binding_guid"), BindingGuidText) || BindingGuidText.IsEmpty())
	{
		OutError = TEXT("missing_binding_guid");
		return false;
	}
	FGuid BindingGuid;
	if (!FGuid::Parse(BindingGuidText, BindingGuid))
	{
		OutError = TEXT("invalid_binding_guid");
		return false;
	}

	double SectionIndexValue = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("section_index"), SectionIndexValue))
	{
		OutError = TEXT("missing_section_index");
		return false;
	}
	const int32 SectionIndex = FMath::RoundToInt(SectionIndexValue);

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
	if (!MovieScene->FindBinding(BindingGuid))
	{
		OutError = TEXT("binding_not_found");
		return false;
	}

	UMovieSceneSkeletalAnimationTrack* SkeletalAnimationTrack = MovieScene->FindTrack<UMovieSceneSkeletalAnimationTrack>(BindingGuid);
	if (!SkeletalAnimationTrack)
	{
		OutError = TEXT("skeletal_animation_track_not_found");
		return false;
	}

	const TArray<UMovieSceneSection*>& Sections = SkeletalAnimationTrack->GetAllSections();
	if (!Sections.IsValidIndex(SectionIndex))
	{
		OutError = TEXT("skeletal_animation_section_not_found");
		return false;
	}

	UMovieSceneSection* TargetSection = Sections[SectionIndex];
	if (!TargetSection)
	{
		OutError = TEXT("skeletal_animation_section_not_found");
		return false;
	}

	Sequence->Modify();
	MovieScene->Modify();
	SkeletalAnimationTrack->Modify();
	TargetSection->Modify();

	SkeletalAnimationTrack->RemoveSection(*TargetSection);
	Sequence->MarkPackageDirty();

	bool bSaveAfterRemove = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_remove"), bSaveAfterRemove);
	if (bSaveAfterRemove)
	{
		if (!UeAgentSequenceOps::SaveAssetPackage(Sequence, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
	OutData->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetNumberField(TEXT("removed_section_index"), SectionIndex);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterRemove);
	return true;
}
