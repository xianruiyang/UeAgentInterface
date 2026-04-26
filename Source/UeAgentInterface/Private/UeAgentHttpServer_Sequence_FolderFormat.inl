namespace UeAgentSequenceFolderOps
{
	static FString GetFolderRootAbsolute()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("LevelSequence")));
	}

	static FString NormalizeAssetRelativeFolder(const FString& InAssetPath)
	{
		FString AssetPath = UeAgentSequenceOps::NormalizeAssetPath(InAssetPath);
		while (AssetPath.StartsWith(TEXT("/")))
		{
			AssetPath.RightChopInline(1, EAllowShrinking::No);
		}
		return AssetPath;
	}

	static bool ResolveFolderForAsset(const FString& InAssetPath, FString& OutFolderPath, FString& OutError)
	{
		const FString AssetPath = UeAgentSequenceOps::NormalizeAssetPath(InAssetPath);
		if (!FPackageName::IsValidLongPackageName(AssetPath))
		{
			OutError = TEXT("invalid_asset_path");
			return false;
		}

		OutFolderPath = FPaths::Combine(GetFolderRootAbsolute(), NormalizeAssetRelativeFolder(AssetPath));
		return true;
	}

	static bool TryGetString(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, FString& OutValue)
	{
		return Obj.IsValid() && Obj->TryGetStringField(Key, OutValue);
	}

	static bool TryGetNumber(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, double& OutValue)
	{
		return Obj.IsValid() && Obj->TryGetNumberField(Key, OutValue);
	}

	static bool TryGetBool(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, bool& OutValue)
	{
		return Obj.IsValid() && Obj->TryGetBoolField(Key, OutValue);
	}

	static FString MakeSafeName(const FString& InName)
	{
		const FString Safe = FPaths::MakeValidFileName(InName, TEXT('_'));
		return Safe.IsEmpty() ? TEXT("Item") : Safe;
	}

	static FString MakeBindingFolderName(const FMovieSceneBinding& Binding)
	{
		return FString::Printf(TEXT("%s_%s"),
			*MakeSafeName(Binding.GetName()),
			*Binding.GetObjectGuid().ToString(EGuidFormats::Digits).Left(8));
	}

	static FString MakeTrackFileName(UMovieSceneTrack* Track, const int32 TrackIndex)
	{
		const FString BaseName = Track ? Track->GetName() : TEXT("Track");
		return FString::Printf(TEXT("%02d_%s.json"), TrackIndex, *MakeSafeName(BaseName));
	}

	static TSharedPtr<FJsonObject> BuildBoundObjectJson(UObject* Object)
	{
		if (!Object)
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> ObjectJson = MakeShared<FJsonObject>();
		ObjectJson->SetStringField(TEXT("object_name"), Object->GetName());
		ObjectJson->SetStringField(TEXT("object_path"), Object->GetPathName());
		ObjectJson->SetStringField(TEXT("object_class"), Object->GetClass() ? Object->GetClass()->GetPathName() : TEXT(""));

		if (AActor* Actor = Cast<AActor>(Object))
		{
			ObjectJson->SetStringField(TEXT("object_kind"), TEXT("actor"));
			ObjectJson->SetStringField(TEXT("actor_name"), Actor->GetName());
			ObjectJson->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
			return ObjectJson;
		}

		if (UActorComponent* ActorComponent = Cast<UActorComponent>(Object))
		{
			ObjectJson->SetStringField(TEXT("object_kind"), TEXT("component"));
			ObjectJson->SetStringField(TEXT("component_name"), ActorComponent->GetName());
			if (AActor* OwnerActor = ActorComponent->GetOwner())
			{
				ObjectJson->SetStringField(TEXT("owner_actor_name"), OwnerActor->GetName());
				ObjectJson->SetStringField(TEXT("owner_actor_label"), OwnerActor->GetActorLabel());
			}
			return ObjectJson;
		}

		ObjectJson->SetStringField(TEXT("object_kind"), TEXT("object"));
		return ObjectJson;
	}

	static void AppendResolvedBoundObjects(ULevelSequence* Sequence, const FGuid& BindingGuid, TArray<TSharedPtr<FJsonValue>>& OutObjects)
	{
		if (!Sequence || !GEditor || !BindingGuid.IsValid())
		{
			return;
		}

		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			return;
		}

		TArray<UObject*, TInlineAllocator<1>> BoundObjects;
		Sequence->LocateBoundObjects(BindingGuid, UE::UniversalObjectLocator::FResolveParams(World), nullptr, BoundObjects);
		for (UObject* BoundObject : BoundObjects)
		{
			if (TSharedPtr<FJsonObject> ObjectJson = BuildBoundObjectJson(BoundObject))
			{
				OutObjects.Add(MakeShared<FJsonValueObject>(ObjectJson));
			}
		}
	}

	static void AddWarning(TArray<TSharedPtr<FJsonValue>>& Warnings, const FString& Code, const FString& Message);

	static void BuildFloatKeyMap(const FMovieSceneFloatChannel& Channel, TMap<int32, float>& OutValues)
	{
		OutValues.Reset();
		const auto ChannelData = Channel.GetData();
		TArrayView<const FFrameNumber> Times = ChannelData.GetTimes();
		TArrayView<const FMovieSceneFloatValue> Values = ChannelData.GetValues();
		for (int32 Index = 0; Index < Times.Num() && Index < Values.Num(); ++Index)
		{
			OutValues.Add(Times[Index].Value, Values[Index].Value);
		}
	}

	static void BuildDoubleKeyMap(const FMovieSceneDoubleChannel& Channel, TMap<int32, double>& OutValues)
	{
		OutValues.Reset();
		const auto ChannelData = Channel.GetData();
		TArrayView<const FFrameNumber> Times = ChannelData.GetTimes();
		TArrayView<const FMovieSceneDoubleValue> Values = ChannelData.GetValues();
		for (int32 Index = 0; Index < Times.Num() && Index < Values.Num(); ++Index)
		{
			OutValues.Add(Times[Index].Value, Values[Index].Value);
		}
	}

	static TSharedPtr<FJsonObject> BuildKeyObject(const FFrameRate& TickResolution, const int32 FrameValue)
	{
		TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
		KeyObj->SetNumberField(TEXT("frame"), FrameValue);
		KeyObj->SetNumberField(TEXT("time_seconds"), TickResolution.AsSeconds(FFrameTime(FFrameNumber(FrameValue))));
		return KeyObj;
	}

	static TSharedPtr<FJsonObject> BuildTransformTrackJson(UMovieScene3DTransformTrack* Track, UMovieScene* MovieScene)
	{
		if (!Track || !MovieScene)
		{
			return nullptr;
		}

		const TArray<UMovieSceneSection*>& Sections = Track->GetAllSections();
		UMovieScene3DTransformSection* TransformSection = Sections.Num() > 0 ? Cast<UMovieScene3DTransformSection>(Sections[0]) : nullptr;
		if (!TransformSection)
		{
			return nullptr;
		}

		TArrayView<FMovieSceneDoubleChannel*> Channels = TransformSection->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
		if (Channels.Num() < 9)
		{
			return nullptr;
		}

		TMap<int32, double> XValues; BuildDoubleKeyMap(*Channels[0], XValues);
		TMap<int32, double> YValues; BuildDoubleKeyMap(*Channels[1], YValues);
		TMap<int32, double> ZValues; BuildDoubleKeyMap(*Channels[2], ZValues);
		TMap<int32, double> RollValues; BuildDoubleKeyMap(*Channels[3], RollValues);
		TMap<int32, double> PitchValues; BuildDoubleKeyMap(*Channels[4], PitchValues);
		TMap<int32, double> YawValues; BuildDoubleKeyMap(*Channels[5], YawValues);
		TMap<int32, double> ScaleXValues; BuildDoubleKeyMap(*Channels[6], ScaleXValues);
		TMap<int32, double> ScaleYValues; BuildDoubleKeyMap(*Channels[7], ScaleYValues);
		TMap<int32, double> ScaleZValues; BuildDoubleKeyMap(*Channels[8], ScaleZValues);

		TSet<int32> Frames;
		for (const TPair<int32, double>& Pair : XValues) Frames.Add(Pair.Key);
		for (const TPair<int32, double>& Pair : YValues) Frames.Add(Pair.Key);
		for (const TPair<int32, double>& Pair : ZValues) Frames.Add(Pair.Key);
		for (const TPair<int32, double>& Pair : RollValues) Frames.Add(Pair.Key);
		for (const TPair<int32, double>& Pair : PitchValues) Frames.Add(Pair.Key);
		for (const TPair<int32, double>& Pair : YawValues) Frames.Add(Pair.Key);
		for (const TPair<int32, double>& Pair : ScaleXValues) Frames.Add(Pair.Key);
		for (const TPair<int32, double>& Pair : ScaleYValues) Frames.Add(Pair.Key);
		for (const TPair<int32, double>& Pair : ScaleZValues) Frames.Add(Pair.Key);

		TArray<int32> SortedFrames = Frames.Array();
		SortedFrames.Sort();

		TArray<TSharedPtr<FJsonValue>> Keys;
		for (const int32 FrameValue : SortedFrames)
		{
			TSharedPtr<FJsonObject> KeyObj = BuildKeyObject(MovieScene->GetTickResolution(), FrameValue);
			TSharedPtr<FJsonObject> LocationObj = MakeShared<FJsonObject>();
			bool bHasLocation = false;
			if (const double* Value = XValues.Find(FrameValue)) { LocationObj->SetNumberField(TEXT("x"), *Value); bHasLocation = true; }
			if (const double* Value = YValues.Find(FrameValue)) { LocationObj->SetNumberField(TEXT("y"), *Value); bHasLocation = true; }
			if (const double* Value = ZValues.Find(FrameValue)) { LocationObj->SetNumberField(TEXT("z"), *Value); bHasLocation = true; }
			if (bHasLocation)
			{
				KeyObj->SetObjectField(TEXT("location"), LocationObj);
			}

			TSharedPtr<FJsonObject> RotationObj = MakeShared<FJsonObject>();
			bool bHasRotation = false;
			if (const double* Value = PitchValues.Find(FrameValue)) { RotationObj->SetNumberField(TEXT("pitch"), *Value); bHasRotation = true; }
			if (const double* Value = YawValues.Find(FrameValue)) { RotationObj->SetNumberField(TEXT("yaw"), *Value); bHasRotation = true; }
			if (const double* Value = RollValues.Find(FrameValue)) { RotationObj->SetNumberField(TEXT("roll"), *Value); bHasRotation = true; }
			if (bHasRotation)
			{
				KeyObj->SetObjectField(TEXT("rotation"), RotationObj);
			}

			TSharedPtr<FJsonObject> ScaleObj = MakeShared<FJsonObject>();
			bool bHasScale = false;
			if (const double* Value = ScaleXValues.Find(FrameValue)) { ScaleObj->SetNumberField(TEXT("x"), *Value); bHasScale = true; }
			if (const double* Value = ScaleYValues.Find(FrameValue)) { ScaleObj->SetNumberField(TEXT("y"), *Value); bHasScale = true; }
			if (const double* Value = ScaleZValues.Find(FrameValue)) { ScaleObj->SetNumberField(TEXT("z"), *Value); bHasScale = true; }
			if (bHasScale)
			{
				KeyObj->SetObjectField(TEXT("scale"), ScaleObj);
			}

			Keys.Add(MakeShared<FJsonValueObject>(KeyObj));
		}

		TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
		SectionObj->SetStringField(TEXT("section_type"), TEXT("transform"));
		SectionObj->SetArrayField(TEXT("keys"), Keys);

		TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
		TrackObj->SetStringField(TEXT("track_type"), TEXT("transform"));
		TrackObj->SetArrayField(TEXT("sections"), { MakeShared<FJsonValueObject>(SectionObj) });
		return TrackObj;
	}

	template<typename TSectionType, typename TChannelType, typename TValueType>
	static TSharedPtr<FJsonObject> BuildScalarPropertyTrackJson(
		UMovieScenePropertyTrack* PropertyTrack,
		TSectionType* Section,
		UMovieScene* MovieScene,
		const FString& PropertyType,
		const TFunctionRef<void(TSharedPtr<FJsonObject>&, const TValueType&)>& ApplyValue,
		const TFunctionRef<void(const TChannelType&, TMap<int32, TValueType>&)>& BuildMap)
	{
		if (!PropertyTrack || !Section || !MovieScene)
		{
			return nullptr;
		}

		TMap<int32, TValueType> Values;
		BuildMap(Section->GetChannel(), Values);
		TArray<int32> SortedFrames;
		Values.GetKeys(SortedFrames);
		SortedFrames.Sort();

		TArray<TSharedPtr<FJsonValue>> Keys;
		for (const int32 FrameValue : SortedFrames)
		{
			TSharedPtr<FJsonObject> KeyObj = BuildKeyObject(MovieScene->GetTickResolution(), FrameValue);
			ApplyValue(KeyObj, Values[FrameValue]);
			Keys.Add(MakeShared<FJsonValueObject>(KeyObj));
		}

		TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
		SectionObj->SetStringField(TEXT("section_type"), *PropertyType);
		SectionObj->SetArrayField(TEXT("keys"), Keys);

		TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
		TrackObj->SetStringField(TEXT("track_type"), TEXT("property"));
		TrackObj->SetStringField(TEXT("property_type"), PropertyType);
		TrackObj->SetStringField(TEXT("property_name"), PropertyTrack->GetPropertyName().ToString());
		TrackObj->SetStringField(TEXT("property_path"), PropertyTrack->GetPropertyPath().ToString());
		TrackObj->SetArrayField(TEXT("sections"), { MakeShared<FJsonValueObject>(SectionObj) });
		return TrackObj;
	}

	static TSharedPtr<FJsonObject> BuildColorTrackJson(UMovieSceneColorTrack* Track, UMovieSceneColorSection* Section, UMovieScene* MovieScene)
	{
		if (!Track || !Section || !MovieScene)
		{
			return nullptr;
		}

		TMap<int32, float> RedValues;
		TMap<int32, float> GreenValues;
		TMap<int32, float> BlueValues;
		TMap<int32, float> AlphaValues;
		BuildFloatKeyMap(Section->GetRedChannel(), RedValues);
		BuildFloatKeyMap(Section->GetGreenChannel(), GreenValues);
		BuildFloatKeyMap(Section->GetBlueChannel(), BlueValues);
		BuildFloatKeyMap(Section->GetAlphaChannel(), AlphaValues);

		TSet<int32> Frames;
		for (const TPair<int32, float>& Pair : RedValues) Frames.Add(Pair.Key);
		for (const TPair<int32, float>& Pair : GreenValues) Frames.Add(Pair.Key);
		for (const TPair<int32, float>& Pair : BlueValues) Frames.Add(Pair.Key);
		for (const TPair<int32, float>& Pair : AlphaValues) Frames.Add(Pair.Key);
		TArray<int32> SortedFrames = Frames.Array();
		SortedFrames.Sort();

		TArray<TSharedPtr<FJsonValue>> Keys;
		for (const int32 FrameValue : SortedFrames)
		{
			TSharedPtr<FJsonObject> KeyObj = BuildKeyObject(MovieScene->GetTickResolution(), FrameValue);
			if (const float* Value = RedValues.Find(FrameValue)) KeyObj->SetNumberField(TEXT("red"), *Value);
			if (const float* Value = GreenValues.Find(FrameValue)) KeyObj->SetNumberField(TEXT("green"), *Value);
			if (const float* Value = BlueValues.Find(FrameValue)) KeyObj->SetNumberField(TEXT("blue"), *Value);
			if (const float* Value = AlphaValues.Find(FrameValue)) KeyObj->SetNumberField(TEXT("alpha"), *Value);
			Keys.Add(MakeShared<FJsonValueObject>(KeyObj));
		}

		TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
		SectionObj->SetStringField(TEXT("section_type"), TEXT("color"));
		SectionObj->SetArrayField(TEXT("keys"), Keys);

		TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
		TrackObj->SetStringField(TEXT("track_type"), TEXT("property"));
		TrackObj->SetStringField(TEXT("property_type"), TEXT("color"));
		TrackObj->SetStringField(TEXT("property_name"), Track->GetPropertyName().ToString());
		TrackObj->SetStringField(TEXT("property_path"), Track->GetPropertyPath().ToString());
		TrackObj->SetArrayField(TEXT("sections"), { MakeShared<FJsonValueObject>(SectionObj) });
		return TrackObj;
	}

	static TSharedPtr<FJsonObject> BuildVisibilityTrackJson(UMovieSceneVisibilityTrack* Track, UMovieSceneVisibilitySection* Section, UMovieScene* MovieScene)
	{
		if (!Track || !Section || !MovieScene)
		{
			return nullptr;
		}

		const auto ChannelData = Section->GetChannel().GetData();
		TArrayView<const FFrameNumber> Times = ChannelData.GetTimes();
		TArrayView<const bool> Values = ChannelData.GetValues();

		TArray<TSharedPtr<FJsonValue>> Keys;
		for (int32 Index = 0; Index < Times.Num() && Index < Values.Num(); ++Index)
		{
			TSharedPtr<FJsonObject> KeyObj = BuildKeyObject(MovieScene->GetTickResolution(), Times[Index].Value);
			KeyObj->SetBoolField(TEXT("value"), Values[Index]);
			Keys.Add(MakeShared<FJsonValueObject>(KeyObj));
		}

		TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
		SectionObj->SetStringField(TEXT("section_type"), TEXT("visibility"));
		SectionObj->SetArrayField(TEXT("keys"), Keys);

		TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
		TrackObj->SetStringField(TEXT("track_type"), TEXT("visibility"));
		TrackObj->SetStringField(TEXT("property_name"), Track->GetPropertyName().ToString());
		TrackObj->SetStringField(TEXT("property_path"), Track->GetPropertyPath().ToString());
		TrackObj->SetArrayField(TEXT("sections"), { MakeShared<FJsonValueObject>(SectionObj) });
		return TrackObj;
	}

	static TSharedPtr<FJsonObject> BuildSkeletalAnimationTrackJson(UMovieSceneSkeletalAnimationTrack* Track)
	{
		if (!Track)
		{
			return nullptr;
		}

		TArray<TSharedPtr<FJsonValue>> Sections;
		for (UMovieSceneSection* SectionBase : Track->GetAllSections())
		{
			UMovieSceneSkeletalAnimationSection* Section = Cast<UMovieSceneSkeletalAnimationSection>(SectionBase);
			if (!Section)
			{
				continue;
			}

			TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
			SectionObj->SetStringField(TEXT("section_type"), TEXT("skeletal_animation"));
			SectionObj->SetStringField(TEXT("animation_asset"), Section->Params.Animation ? Section->Params.Animation->GetPathName() : TEXT(""));
			SectionObj->SetStringField(TEXT("slot_name"), Section->Params.SlotName.ToString());
			SectionObj->SetNumberField(TEXT("row_index"), Section->GetRowIndex());
			SectionObj->SetNumberField(TEXT("play_rate"),
				Section->Params.PlayRate.GetType() == EMovieSceneTimeWarpType::FixedPlayRate ? Section->Params.PlayRate.AsFixedPlayRateFloat() : 0.0f);
			SectionObj->SetStringField(TEXT("play_rate_type"),
				Section->Params.PlayRate.GetType() == EMovieSceneTimeWarpType::FixedPlayRate ? TEXT("fixed") : TEXT("custom"));
			SectionObj->SetBoolField(TEXT("reverse"), Section->Params.bReverse);
			SectionObj->SetBoolField(TEXT("skip_anim_notifiers"), Section->Params.bSkipAnimNotifiers);
			SectionObj->SetBoolField(TEXT("force_custom_mode"), Section->Params.bForceCustomMode);
			SectionObj->SetNumberField(TEXT("first_loop_start_offset_frames"), Section->Params.FirstLoopStartFrameOffset.Value);
			SectionObj->SetNumberField(TEXT("start_offset_frames"), Section->Params.StartFrameOffset.Value);
			SectionObj->SetNumberField(TEXT("end_offset_frames"), Section->Params.EndFrameOffset.Value);
			SectionObj->SetNumberField(TEXT("row_index"), Section->GetRowIndex());
			Sections.Add(MakeShared<FJsonValueObject>(SectionObj));
		}

		TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
		TrackObj->SetStringField(TEXT("track_type"), TEXT("skeletal_animation"));
		TrackObj->SetArrayField(TEXT("sections"), Sections);
		return TrackObj;
	}

	static TSharedPtr<FJsonObject> BuildByteTrackJson(UMovieSceneByteTrack* Track, UMovieScene* MovieScene)
	{
		if (!Track || !MovieScene)
		{
			return nullptr;
		}

		UMovieSceneByteSection* Section = Track->GetAllSections().Num() > 0 ? Cast<UMovieSceneByteSection>(Track->GetAllSections()[0]) : nullptr;
		if (!Section)
		{
			return nullptr;
		}

		const auto ChannelData = Section->ByteCurve.GetData();
		TArrayView<const FFrameNumber> Times = ChannelData.GetTimes();
		TArrayView<const uint8> Values = ChannelData.GetValues();
		UEnum* ByteEnum = Track->GetEnum();

		TArray<TSharedPtr<FJsonValue>> Keys;
		for (int32 Index = 0; Index < Times.Num() && Index < Values.Num(); ++Index)
		{
			TSharedPtr<FJsonObject> KeyObj = BuildKeyObject(MovieScene->GetTickResolution(), Times[Index].Value);
			KeyObj->SetNumberField(TEXT("value"), Values[Index]);
			if (ByteEnum)
			{
				KeyObj->SetStringField(TEXT("value_name"), ByteEnum->GetNameStringByValue(Values[Index]));
			}
			Keys.Add(MakeShared<FJsonValueObject>(KeyObj));
		}

		TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
		SectionObj->SetStringField(TEXT("section_type"), TEXT("byte"));
		if (TOptional<uint8> DefaultValue = Section->ByteCurve.GetDefault(); DefaultValue.IsSet())
		{
			SectionObj->SetNumberField(TEXT("default_value"), DefaultValue.GetValue());
			if (ByteEnum)
			{
				SectionObj->SetStringField(TEXT("default_value_name"), ByteEnum->GetNameStringByValue(DefaultValue.GetValue()));
			}
		}
		SectionObj->SetArrayField(TEXT("keys"), Keys);

		TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
		TrackObj->SetStringField(TEXT("track_type"), TEXT("property"));
		TrackObj->SetStringField(TEXT("property_type"), TEXT("byte"));
		TrackObj->SetStringField(TEXT("property_name"), Track->GetPropertyName().ToString());
		TrackObj->SetStringField(TEXT("property_path"), Track->GetPropertyPath().ToString());
		TrackObj->SetStringField(TEXT("enum_path"), ByteEnum ? ByteEnum->GetPathName() : TEXT(""));
		TrackObj->SetArrayField(TEXT("sections"), { MakeShared<FJsonValueObject>(SectionObj) });
		return TrackObj;
	}

	static TSharedPtr<FJsonObject> BuildStringTrackJson(UMovieSceneStringTrack* Track, UMovieScene* MovieScene)
	{
		if (!Track || !MovieScene)
		{
			return nullptr;
		}

		UMovieSceneStringSection* Section = Track->GetAllSections().Num() > 0 ? Cast<UMovieSceneStringSection>(Track->GetAllSections()[0]) : nullptr;
		if (!Section)
		{
			return nullptr;
		}

		const FMovieSceneStringChannel& Channel = Section->GetChannel();
		const auto ChannelData = Channel.GetData();
		TArrayView<const FFrameNumber> Times = ChannelData.GetTimes();
		TArrayView<const FString> Values = ChannelData.GetValues();

		TArray<TSharedPtr<FJsonValue>> Keys;
		for (int32 Index = 0; Index < Times.Num() && Index < Values.Num(); ++Index)
		{
			TSharedPtr<FJsonObject> KeyObj = BuildKeyObject(MovieScene->GetTickResolution(), Times[Index].Value);
			KeyObj->SetStringField(TEXT("value"), Values[Index]);
			Keys.Add(MakeShared<FJsonValueObject>(KeyObj));
		}

		TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
		SectionObj->SetStringField(TEXT("section_type"), TEXT("string"));
		if (TOptional<FString> DefaultValue = Channel.GetDefault(); DefaultValue.IsSet())
		{
			SectionObj->SetStringField(TEXT("default_value"), DefaultValue.GetValue());
		}
		SectionObj->SetArrayField(TEXT("keys"), Keys);

		TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
		TrackObj->SetStringField(TEXT("track_type"), TEXT("property"));
		TrackObj->SetStringField(TEXT("property_type"), TEXT("string"));
		TrackObj->SetStringField(TEXT("property_name"), Track->GetPropertyName().ToString());
		TrackObj->SetStringField(TEXT("property_path"), Track->GetPropertyPath().ToString());
		TrackObj->SetArrayField(TEXT("sections"), { MakeShared<FJsonValueObject>(SectionObj) });
		return TrackObj;
	}

	static TSharedPtr<FJsonObject> BuildVectorTrackJson(UMovieSceneTrack* Track, UMovieSceneSection* SectionBase, UMovieScene* MovieScene, const FString& Precision, const int32 ChannelsUsed)
	{
		if (!Track || !SectionBase || !MovieScene)
		{
			return nullptr;
		}

		TSet<int32> Frames;
		TArray<TMap<int32, double>> DoubleChannelMaps;
		TArray<TMap<int32, float>> FloatChannelMaps;
		const bool bFloat = Precision.Equals(TEXT("float"), ESearchCase::IgnoreCase);

		auto AddFrameMapsDouble = [&](const FMovieSceneDoubleChannel& Channel, TMap<int32, double>& OutMap)
		{
			BuildDoubleKeyMap(Channel, OutMap);
			for (const TPair<int32, double>& Pair : OutMap)
			{
				Frames.Add(Pair.Key);
			}
		};
		auto AddFrameMapsFloat = [&](const FMovieSceneFloatChannel& Channel, TMap<int32, float>& OutMap)
		{
			BuildFloatKeyMap(Channel, OutMap);
			for (const TPair<int32, float>& Pair : OutMap)
			{
				Frames.Add(Pair.Key);
			}
		};

		TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
		SectionObj->SetStringField(TEXT("section_type"), TEXT("vector"));
		SectionObj->SetStringField(TEXT("vector_precision"), Precision);
		SectionObj->SetNumberField(TEXT("channels_used"), ChannelsUsed);

		TSharedPtr<FJsonObject> DefaultValueObj = MakeShared<FJsonObject>();
		bool bHasDefaultValue = true;

		if (bFloat)
		{
			UMovieSceneFloatVectorSection* Section = Cast<UMovieSceneFloatVectorSection>(SectionBase);
			if (!Section)
			{
				return nullptr;
			}
			FloatChannelMaps.SetNum(ChannelsUsed);
			for (int32 ChannelIndex = 0; ChannelIndex < ChannelsUsed; ++ChannelIndex)
			{
				AddFrameMapsFloat(Section->GetChannel(ChannelIndex), FloatChannelMaps[ChannelIndex]);
				const TOptional<float> DefaultValue = Section->GetChannel(ChannelIndex).GetDefault();
				if (!DefaultValue.IsSet())
				{
					bHasDefaultValue = false;
				}
				else if (ChannelIndex == 0) { DefaultValueObj->SetNumberField(TEXT("x"), DefaultValue.GetValue()); }
				else if (ChannelIndex == 1) { DefaultValueObj->SetNumberField(TEXT("y"), DefaultValue.GetValue()); }
				else if (ChannelIndex == 2) { DefaultValueObj->SetNumberField(TEXT("z"), DefaultValue.GetValue()); }
				else if (ChannelIndex == 3) { DefaultValueObj->SetNumberField(TEXT("w"), DefaultValue.GetValue()); }
			}
		}
		else
		{
			UMovieSceneDoubleVectorSection* Section = Cast<UMovieSceneDoubleVectorSection>(SectionBase);
			if (!Section)
			{
				return nullptr;
			}
			DoubleChannelMaps.SetNum(ChannelsUsed);
			for (int32 ChannelIndex = 0; ChannelIndex < ChannelsUsed; ++ChannelIndex)
			{
				AddFrameMapsDouble(Section->GetChannel(ChannelIndex), DoubleChannelMaps[ChannelIndex]);
				const TOptional<double> DefaultValue = Section->GetChannel(ChannelIndex).GetDefault();
				if (!DefaultValue.IsSet())
				{
					bHasDefaultValue = false;
				}
				else if (ChannelIndex == 0) { DefaultValueObj->SetNumberField(TEXT("x"), DefaultValue.GetValue()); }
				else if (ChannelIndex == 1) { DefaultValueObj->SetNumberField(TEXT("y"), DefaultValue.GetValue()); }
				else if (ChannelIndex == 2) { DefaultValueObj->SetNumberField(TEXT("z"), DefaultValue.GetValue()); }
				else if (ChannelIndex == 3) { DefaultValueObj->SetNumberField(TEXT("w"), DefaultValue.GetValue()); }
			}
		}

		if (bHasDefaultValue)
		{
			SectionObj->SetObjectField(TEXT("default_value"), DefaultValueObj);
		}

		TArray<int32> SortedFrames = Frames.Array();
		SortedFrames.Sort();
		TArray<TSharedPtr<FJsonValue>> Keys;
		for (const int32 FrameValue : SortedFrames)
		{
			TSharedPtr<FJsonObject> KeyObj = BuildKeyObject(MovieScene->GetTickResolution(), FrameValue);
			TSharedPtr<FJsonObject> ValueObj = MakeShared<FJsonObject>();
			for (int32 ChannelIndex = 0; ChannelIndex < ChannelsUsed; ++ChannelIndex)
			{
				if (bFloat)
				{
					if (const float* Value = FloatChannelMaps[ChannelIndex].Find(FrameValue))
					{
						if (ChannelIndex == 0) { ValueObj->SetNumberField(TEXT("x"), *Value); }
						else if (ChannelIndex == 1) { ValueObj->SetNumberField(TEXT("y"), *Value); }
						else if (ChannelIndex == 2) { ValueObj->SetNumberField(TEXT("z"), *Value); }
						else if (ChannelIndex == 3) { ValueObj->SetNumberField(TEXT("w"), *Value); }
					}
				}
				else
				{
					if (const double* Value = DoubleChannelMaps[ChannelIndex].Find(FrameValue))
					{
						if (ChannelIndex == 0) { ValueObj->SetNumberField(TEXT("x"), *Value); }
						else if (ChannelIndex == 1) { ValueObj->SetNumberField(TEXT("y"), *Value); }
						else if (ChannelIndex == 2) { ValueObj->SetNumberField(TEXT("z"), *Value); }
						else if (ChannelIndex == 3) { ValueObj->SetNumberField(TEXT("w"), *Value); }
					}
				}
			}
			KeyObj->SetObjectField(TEXT("value"), ValueObj);
			Keys.Add(MakeShared<FJsonValueObject>(KeyObj));
		}
		SectionObj->SetArrayField(TEXT("keys"), Keys);

		TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
		TrackObj->SetStringField(TEXT("track_type"), TEXT("property"));
		TrackObj->SetStringField(TEXT("property_type"), TEXT("vector"));
		if (UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(Track))
		{
			TrackObj->SetStringField(TEXT("property_name"), PropertyTrack->GetPropertyName().ToString());
			TrackObj->SetStringField(TEXT("property_path"), PropertyTrack->GetPropertyPath().ToString());
		}
		TrackObj->SetStringField(TEXT("vector_precision"), Precision);
		TrackObj->SetNumberField(TEXT("channels_used"), ChannelsUsed);
		TrackObj->SetArrayField(TEXT("sections"), { MakeShared<FJsonValueObject>(SectionObj) });
		return TrackObj;
	}

	static TSharedPtr<FJsonObject> BuildRotatorTrackJson(UMovieSceneRotatorTrack* Track, UMovieScene* MovieScene)
	{
		if (!Track || !MovieScene)
		{
			return nullptr;
		}

		UMovieSceneRotatorSection* Section = Track->GetAllSections().Num() > 0 ? Cast<UMovieSceneRotatorSection>(Track->GetAllSections()[0]) : nullptr;
		if (!Section)
		{
			return nullptr;
		}

		TMap<int32, double> PitchValues; BuildDoubleKeyMap(Section->GetChannel(UMovieSceneRotatorSection::PitchChannelIndex), PitchValues);
		TMap<int32, double> YawValues; BuildDoubleKeyMap(Section->GetChannel(UMovieSceneRotatorSection::YawChannelIndex), YawValues);
		TMap<int32, double> RollValues; BuildDoubleKeyMap(Section->GetChannel(UMovieSceneRotatorSection::RollChannelIndex), RollValues);

		TSet<int32> Frames;
		for (const TPair<int32, double>& Pair : PitchValues) Frames.Add(Pair.Key);
		for (const TPair<int32, double>& Pair : YawValues) Frames.Add(Pair.Key);
		for (const TPair<int32, double>& Pair : RollValues) Frames.Add(Pair.Key);

		TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
		SectionObj->SetStringField(TEXT("section_type"), TEXT("rotator"));
		if (const TOptional<double> DefaultPitch = Section->GetChannel(UMovieSceneRotatorSection::PitchChannelIndex).GetDefault();
			DefaultPitch.IsSet()
			&& Section->GetChannel(UMovieSceneRotatorSection::YawChannelIndex).GetDefault().IsSet()
			&& Section->GetChannel(UMovieSceneRotatorSection::RollChannelIndex).GetDefault().IsSet())
		{
			TSharedPtr<FJsonObject> DefaultValueObj = MakeShared<FJsonObject>();
			DefaultValueObj->SetNumberField(TEXT("pitch"), DefaultPitch.GetValue());
			DefaultValueObj->SetNumberField(TEXT("yaw"), Section->GetChannel(UMovieSceneRotatorSection::YawChannelIndex).GetDefault().GetValue());
			DefaultValueObj->SetNumberField(TEXT("roll"), Section->GetChannel(UMovieSceneRotatorSection::RollChannelIndex).GetDefault().GetValue());
			SectionObj->SetObjectField(TEXT("default_value"), DefaultValueObj);
		}

		TArray<int32> SortedFrames = Frames.Array();
		SortedFrames.Sort();
		TArray<TSharedPtr<FJsonValue>> Keys;
		for (const int32 FrameValue : SortedFrames)
		{
			TSharedPtr<FJsonObject> KeyObj = BuildKeyObject(MovieScene->GetTickResolution(), FrameValue);
			TSharedPtr<FJsonObject> ValueObj = MakeShared<FJsonObject>();
			if (const double* Value = PitchValues.Find(FrameValue)) { ValueObj->SetNumberField(TEXT("pitch"), *Value); }
			if (const double* Value = YawValues.Find(FrameValue)) { ValueObj->SetNumberField(TEXT("yaw"), *Value); }
			if (const double* Value = RollValues.Find(FrameValue)) { ValueObj->SetNumberField(TEXT("roll"), *Value); }
			KeyObj->SetObjectField(TEXT("value"), ValueObj);
			Keys.Add(MakeShared<FJsonValueObject>(KeyObj));
		}
		SectionObj->SetArrayField(TEXT("keys"), Keys);

		TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
		TrackObj->SetStringField(TEXT("track_type"), TEXT("property"));
		TrackObj->SetStringField(TEXT("property_type"), TEXT("rotator"));
		TrackObj->SetStringField(TEXT("property_name"), Track->GetPropertyName().ToString());
		TrackObj->SetStringField(TEXT("property_path"), Track->GetPropertyPath().ToString());
		TrackObj->SetArrayField(TEXT("sections"), { MakeShared<FJsonValueObject>(SectionObj) });
		return TrackObj;
	}

	static TSharedPtr<FJsonObject> BuildActorReferenceTrackJson(UMovieSceneActorReferenceTrack* Track, UMovieScene* MovieScene)
	{
		if (!Track || !MovieScene)
		{
			return nullptr;
		}

		UMovieSceneActorReferenceSection* Section = Track->GetAllSections().Num() > 0 ? Cast<UMovieSceneActorReferenceSection>(Track->GetAllSections()[0]) : nullptr;
		if (!Section)
		{
			return nullptr;
		}

		const FMovieSceneActorReferenceData& Channel = Section->GetActorReferenceData();
		const auto ChannelData = Channel.GetData();
		TArrayView<const FFrameNumber> Times = ChannelData.GetTimes();
		TArrayView<const FMovieSceneActorReferenceKey> Values = ChannelData.GetValues();

		TArray<TSharedPtr<FJsonValue>> Keys;
		for (int32 Index = 0; Index < Times.Num() && Index < Values.Num(); ++Index)
		{
			TSharedPtr<FJsonObject> KeyObj = BuildKeyObject(MovieScene->GetTickResolution(), Times[Index].Value);
			KeyObj->SetStringField(TEXT("value_binding_guid"), Values[Index].Object.GetGuid().IsValid() ? Values[Index].Object.GetGuid().ToString(EGuidFormats::DigitsWithHyphensLower) : TEXT(""));
			KeyObj->SetStringField(TEXT("component_name"), Values[Index].ComponentName.ToString());
			KeyObj->SetStringField(TEXT("socket_name"), Values[Index].SocketName.ToString());
			Keys.Add(MakeShared<FJsonValueObject>(KeyObj));
		}

		TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
		SectionObj->SetStringField(TEXT("section_type"), TEXT("actor_reference"));
		if (const FMovieSceneActorReferenceKey& DefaultValue = Channel.GetDefault(); DefaultValue.Object.GetGuid().IsValid())
		{
			SectionObj->SetStringField(TEXT("default_binding_guid"), DefaultValue.Object.GetGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
			SectionObj->SetStringField(TEXT("default_component_name"), DefaultValue.ComponentName.ToString());
			SectionObj->SetStringField(TEXT("default_socket_name"), DefaultValue.SocketName.ToString());
		}
		SectionObj->SetArrayField(TEXT("keys"), Keys);

		TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
		TrackObj->SetStringField(TEXT("track_type"), TEXT("property"));
		TrackObj->SetStringField(TEXT("property_type"), TEXT("actor_reference"));
		TrackObj->SetStringField(TEXT("property_name"), Track->GetPropertyName().ToString());
		TrackObj->SetStringField(TEXT("property_path"), Track->GetPropertyPath().ToString());
		TrackObj->SetArrayField(TEXT("sections"), { MakeShared<FJsonValueObject>(SectionObj) });
		return TrackObj;
	}

	static FString GetObjectPathValue(const FMovieSceneObjectPathChannelKeyValue& Value)
	{
		if (UObject* Object = Value.Get())
		{
			return Object->GetPathName();
		}
		return Value.GetSoftPtr().ToString();
	}

	static TSharedPtr<FJsonObject> BuildObjectPropertyTrackJson(UMovieSceneObjectPropertyTrack* Track, UMovieScene* MovieScene)
	{
		if (!Track || !MovieScene)
		{
			return nullptr;
		}

		UMovieSceneObjectPropertySection* Section = Track->GetAllSections().Num() > 0 ? Cast<UMovieSceneObjectPropertySection>(Track->GetAllSections()[0]) : nullptr;
		if (!Section)
		{
			return nullptr;
		}

		const auto ChannelData = Section->ObjectChannel.GetData();
		TArrayView<const FFrameNumber> Times = ChannelData.GetTimes();
		TArrayView<const FMovieSceneObjectPathChannelKeyValue> Values = ChannelData.GetValues();

		TArray<TSharedPtr<FJsonValue>> Keys;
		for (int32 Index = 0; Index < Times.Num() && Index < Values.Num(); ++Index)
		{
			TSharedPtr<FJsonObject> KeyObj = BuildKeyObject(MovieScene->GetTickResolution(), Times[Index].Value);
			KeyObj->SetStringField(TEXT("value_path"), GetObjectPathValue(Values[Index]));
			Keys.Add(MakeShared<FJsonValueObject>(KeyObj));
		}

		TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
		SectionObj->SetStringField(TEXT("section_type"), TEXT("object"));
		const FString DefaultValuePath = GetObjectPathValue(Section->ObjectChannel.GetDefault());
		if (!DefaultValuePath.IsEmpty())
		{
			SectionObj->SetStringField(TEXT("default_value_path"), DefaultValuePath);
		}
		SectionObj->SetArrayField(TEXT("keys"), Keys);

		TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
		TrackObj->SetStringField(TEXT("track_type"), TEXT("property"));
		TrackObj->SetStringField(TEXT("property_type"), TEXT("object"));
		TrackObj->SetStringField(TEXT("property_name"), Track->GetPropertyName().ToString());
		TrackObj->SetStringField(TEXT("property_path"), Track->GetPropertyPath().ToString());
		TrackObj->SetStringField(TEXT("property_class_path"), Track->PropertyClass ? Track->PropertyClass->GetPathName() : TEXT(""));
		TrackObj->SetArrayField(TEXT("sections"), { MakeShared<FJsonValueObject>(SectionObj) });
		return TrackObj;
	}

	static TSharedPtr<FJsonObject> BuildSpawnTrackJson(UMovieSceneSpawnTrack* Track, UMovieScene* MovieScene)
	{
		if (!Track || !MovieScene)
		{
			return nullptr;
		}

		UMovieSceneSpawnSection* Section = Track->GetAllSections().Num() > 0 ? Cast<UMovieSceneSpawnSection>(Track->GetAllSections()[0]) : nullptr;
		if (!Section)
		{
			return nullptr;
		}

		const auto ChannelData = Section->GetChannel().GetData();
		TArrayView<const FFrameNumber> Times = ChannelData.GetTimes();
		TArrayView<const bool> Values = ChannelData.GetValues();

		TArray<TSharedPtr<FJsonValue>> Keys;
		for (int32 Index = 0; Index < Times.Num() && Index < Values.Num(); ++Index)
		{
			TSharedPtr<FJsonObject> KeyObj = BuildKeyObject(MovieScene->GetTickResolution(), Times[Index].Value);
			KeyObj->SetBoolField(TEXT("value"), Values[Index]);
			Keys.Add(MakeShared<FJsonValueObject>(KeyObj));
		}

		TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
		SectionObj->SetStringField(TEXT("section_type"), TEXT("spawn"));
		SectionObj->SetArrayField(TEXT("keys"), Keys);

		TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
		TrackObj->SetStringField(TEXT("track_type"), TEXT("spawn"));
		TrackObj->SetArrayField(TEXT("sections"), { MakeShared<FJsonValueObject>(SectionObj) });
		return TrackObj;
	}

	static TSharedPtr<FJsonObject> BuildCameraCutTrackJson(UMovieSceneCameraCutTrack* Track, UMovieScene* MovieScene)
	{
		if (!Track || !MovieScene)
		{
			return nullptr;
		}

		TArray<TSharedPtr<FJsonValue>> Sections;
		const FFrameRate TickResolution = MovieScene->GetTickResolution();
		for (UMovieSceneSection* SectionBase : Track->GetAllSections())
		{
			UMovieSceneCameraCutSection* Section = Cast<UMovieSceneCameraCutSection>(SectionBase);
			if (!Section)
			{
				continue;
			}

			TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
			SectionObj->SetStringField(TEXT("section_type"), TEXT("camera_cut"));
			SectionObj->SetStringField(TEXT("camera_binding_guid"), Section->GetCameraBindingID().GetGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
			SectionObj->SetNumberField(TEXT("start_seconds"), TickResolution.AsSeconds(FFrameTime(Section->GetInclusiveStartFrame())));
			SectionObj->SetNumberField(TEXT("duration_seconds"), TickResolution.AsSeconds(FFrameTime(Section->GetExclusiveEndFrame() - Section->GetInclusiveStartFrame())));
			SectionObj->SetBoolField(TEXT("lock_previous_camera"), Section->bLockPreviousCamera);
			Sections.Add(MakeShared<FJsonValueObject>(SectionObj));
		}

		TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
		TrackObj->SetStringField(TEXT("track_type"), TEXT("camera_cut"));
		TrackObj->SetBoolField(TEXT("can_blend"), Track->bCanBlend);
		TrackObj->SetArrayField(TEXT("sections"), Sections);
		return TrackObj;
	}

	static TSharedPtr<FJsonObject> BuildSubSequenceTrackJson(UMovieSceneSubTrack* Track, UMovieScene* MovieScene, const bool bCinematicShot)
	{
		if (!Track || !MovieScene)
		{
			return nullptr;
		}

		const FFrameRate TickResolution = MovieScene->GetTickResolution();
		TArray<TSharedPtr<FJsonValue>> Sections;
		for (UMovieSceneSection* SectionBase : Track->GetAllSections())
		{
			UMovieSceneSubSection* Section = Cast<UMovieSceneSubSection>(SectionBase);
			if (!Section)
			{
				continue;
			}

			TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
			SectionObj->SetStringField(TEXT("section_type"), bCinematicShot ? TEXT("cinematic_shot") : TEXT("sub_sequence"));
			SectionObj->SetStringField(TEXT("sequence_asset"), Section->GetSequence() ? Section->GetSequence()->GetPathName() : TEXT(""));
			SectionObj->SetNumberField(TEXT("start_seconds"), TickResolution.AsSeconds(FFrameTime(Section->GetInclusiveStartFrame())));
			SectionObj->SetNumberField(TEXT("duration_seconds"), TickResolution.AsSeconds(FFrameTime(Section->GetExclusiveEndFrame() - Section->GetInclusiveStartFrame())));
			SectionObj->SetNumberField(TEXT("row_index"), Section->GetRowIndex());
			SectionObj->SetNumberField(TEXT("start_frame_offset"), Section->Parameters.StartFrameOffset.Value);
			SectionObj->SetBoolField(TEXT("can_loop"), Section->Parameters.bCanLoop);
			SectionObj->SetNumberField(TEXT("end_frame_offset"), Section->Parameters.EndFrameOffset.Value);
			SectionObj->SetNumberField(TEXT("first_loop_start_frame_offset"), Section->Parameters.FirstLoopStartFrameOffset.Value);
			SectionObj->SetNumberField(TEXT("hierarchical_bias"), Section->Parameters.HierarchicalBias);
			SectionObj->SetNumberField(TEXT("network_mask"), static_cast<int32>(Section->GetNetworkMask()));

			if (Section->Parameters.TimeScale.GetType() == EMovieSceneTimeWarpType::FixedPlayRate)
			{
				SectionObj->SetStringField(TEXT("time_scale_type"), TEXT("fixed"));
				SectionObj->SetNumberField(TEXT("time_scale"), Section->Parameters.TimeScale.AsFixedPlayRateFloat());
			}
			else
			{
				SectionObj->SetStringField(TEXT("time_scale_type"), TEXT("custom"));
			}

			if (bCinematicShot)
			{
				if (UMovieSceneCinematicShotSection* ShotSection = Cast<UMovieSceneCinematicShotSection>(Section))
				{
					SectionObj->SetStringField(TEXT("shot_display_name"), ShotSection->GetShotDisplayName());
				}
			}

			Sections.Add(MakeShared<FJsonValueObject>(SectionObj));
		}

		TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
		TrackObj->SetStringField(TEXT("track_type"), bCinematicShot ? TEXT("cinematic_shot") : TEXT("sub_sequence"));
		TrackObj->SetArrayField(TEXT("sections"), Sections);
		return TrackObj;
	}

	static TSharedPtr<FJsonObject> BuildUnsupportedTrackJson(UMovieSceneTrack* Track, UMovieScene* MovieScene)
	{
		TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
		TrackObj->SetStringField(TEXT("track_type"), TEXT("unsupported"));
		if (TSharedPtr<FJsonObject> Summary = UeAgentSequenceOps::BuildMovieSceneTrackSummary(MovieScene, Track))
		{
			TrackObj->SetObjectField(TEXT("summary"), Summary);
		}
		return TrackObj;
	}

	static TSharedPtr<FJsonObject> BuildTrackJson(UMovieScene* MovieScene, UMovieSceneTrack* Track)
	{
		if (!MovieScene || !Track)
		{
			return nullptr;
		}

		if (UMovieScene3DTransformTrack* TransformTrack = Cast<UMovieScene3DTransformTrack>(Track))
		{
			return BuildTransformTrackJson(TransformTrack, MovieScene);
		}
		if (UMovieSceneSpawnTrack* SpawnTrack = Cast<UMovieSceneSpawnTrack>(Track))
		{
			return BuildSpawnTrackJson(SpawnTrack, MovieScene);
		}
		if (UMovieSceneSkeletalAnimationTrack* SkeletalAnimationTrack = Cast<UMovieSceneSkeletalAnimationTrack>(Track))
		{
			return BuildSkeletalAnimationTrackJson(SkeletalAnimationTrack);
		}
		if (UMovieSceneCameraCutTrack* CameraCutTrack = Cast<UMovieSceneCameraCutTrack>(Track))
		{
			return BuildCameraCutTrackJson(CameraCutTrack, MovieScene);
		}
		if (UMovieSceneCinematicShotTrack* CinematicShotTrack = Cast<UMovieSceneCinematicShotTrack>(Track))
		{
			return BuildSubSequenceTrackJson(CinematicShotTrack, MovieScene, true);
		}
		if (UMovieSceneSubTrack* SubTrack = Cast<UMovieSceneSubTrack>(Track))
		{
			return BuildSubSequenceTrackJson(SubTrack, MovieScene, false);
		}
		if (UMovieSceneVisibilityTrack* VisibilityTrack = Cast<UMovieSceneVisibilityTrack>(Track))
		{
			UMovieSceneVisibilitySection* VisibilitySection = VisibilityTrack->GetAllSections().Num() > 0 ? Cast<UMovieSceneVisibilitySection>(VisibilityTrack->GetAllSections()[0]) : nullptr;
			return BuildVisibilityTrackJson(VisibilityTrack, VisibilitySection, MovieScene);
		}
		if (UMovieSceneFloatTrack* FloatTrack = Cast<UMovieSceneFloatTrack>(Track))
		{
			UMovieSceneFloatSection* Section = FloatTrack->GetAllSections().Num() > 0 ? Cast<UMovieSceneFloatSection>(FloatTrack->GetAllSections()[0]) : nullptr;
			return BuildScalarPropertyTrackJson<UMovieSceneFloatSection, FMovieSceneFloatChannel, float>(
				FloatTrack, Section, MovieScene, TEXT("float"),
				[](TSharedPtr<FJsonObject>& KeyObj, const float& Value) { KeyObj->SetNumberField(TEXT("value"), Value); },
				[](const FMovieSceneFloatChannel& Channel, TMap<int32, float>& Values) { BuildFloatKeyMap(Channel, Values); });
		}
		if (UMovieSceneDoubleTrack* DoubleTrack = Cast<UMovieSceneDoubleTrack>(Track))
		{
			UMovieSceneDoubleSection* Section = DoubleTrack->GetAllSections().Num() > 0 ? Cast<UMovieSceneDoubleSection>(DoubleTrack->GetAllSections()[0]) : nullptr;
			return BuildScalarPropertyTrackJson<UMovieSceneDoubleSection, FMovieSceneDoubleChannel, double>(
				DoubleTrack, Section, MovieScene, TEXT("double"),
				[](TSharedPtr<FJsonObject>& KeyObj, const double& Value) { KeyObj->SetNumberField(TEXT("value"), Value); },
				[](const FMovieSceneDoubleChannel& Channel, TMap<int32, double>& Values) { BuildDoubleKeyMap(Channel, Values); });
		}
		if (UMovieSceneBoolTrack* BoolTrack = Cast<UMovieSceneBoolTrack>(Track))
		{
			UMovieSceneBoolSection* Section = BoolTrack->GetAllSections().Num() > 0 ? Cast<UMovieSceneBoolSection>(BoolTrack->GetAllSections()[0]) : nullptr;
			if (!Section)
			{
				return nullptr;
			}
			const auto ChannelData = Section->GetChannel().GetData();
			TArrayView<const FFrameNumber> Times = ChannelData.GetTimes();
			TArrayView<const bool> Values = ChannelData.GetValues();
			TArray<TSharedPtr<FJsonValue>> Keys;
			for (int32 Index = 0; Index < Times.Num() && Index < Values.Num(); ++Index)
			{
				TSharedPtr<FJsonObject> KeyObj = BuildKeyObject(MovieScene->GetTickResolution(), Times[Index].Value);
				KeyObj->SetBoolField(TEXT("value"), Values[Index]);
				Keys.Add(MakeShared<FJsonValueObject>(KeyObj));
			}
			TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
			SectionObj->SetStringField(TEXT("section_type"), TEXT("bool"));
			SectionObj->SetArrayField(TEXT("keys"), Keys);
			TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
			TrackObj->SetStringField(TEXT("track_type"), TEXT("property"));
			TrackObj->SetStringField(TEXT("property_type"), TEXT("bool"));
			TrackObj->SetStringField(TEXT("property_name"), BoolTrack->GetPropertyName().ToString());
			TrackObj->SetStringField(TEXT("property_path"), BoolTrack->GetPropertyPath().ToString());
			TrackObj->SetArrayField(TEXT("sections"), { MakeShared<FJsonValueObject>(SectionObj) });
			return TrackObj;
		}
		if (UMovieSceneIntegerTrack* IntegerTrack = Cast<UMovieSceneIntegerTrack>(Track))
		{
			UMovieSceneIntegerSection* Section = IntegerTrack->GetAllSections().Num() > 0 ? Cast<UMovieSceneIntegerSection>(IntegerTrack->GetAllSections()[0]) : nullptr;
			if (!Section)
			{
				return nullptr;
			}
			TArrayView<FMovieSceneIntegerChannel*> Channels = Section->GetChannelProxy().GetChannels<FMovieSceneIntegerChannel>();
			if (Channels.Num() == 0)
			{
				return nullptr;
			}
			const auto ChannelData = Channels[0]->GetData();
			TArrayView<const FFrameNumber> Times = ChannelData.GetTimes();
			TArrayView<const int32> Values = ChannelData.GetValues();
			TArray<TSharedPtr<FJsonValue>> Keys;
			for (int32 Index = 0; Index < Times.Num() && Index < Values.Num(); ++Index)
			{
				TSharedPtr<FJsonObject> KeyObj = BuildKeyObject(MovieScene->GetTickResolution(), Times[Index].Value);
				KeyObj->SetNumberField(TEXT("value"), Values[Index]);
				Keys.Add(MakeShared<FJsonValueObject>(KeyObj));
			}
			TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
			SectionObj->SetStringField(TEXT("section_type"), TEXT("integer"));
			SectionObj->SetArrayField(TEXT("keys"), Keys);
			TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
			TrackObj->SetStringField(TEXT("track_type"), TEXT("property"));
			TrackObj->SetStringField(TEXT("property_type"), TEXT("integer"));
			TrackObj->SetStringField(TEXT("property_name"), IntegerTrack->GetPropertyName().ToString());
			TrackObj->SetStringField(TEXT("property_path"), IntegerTrack->GetPropertyPath().ToString());
			TrackObj->SetArrayField(TEXT("sections"), { MakeShared<FJsonValueObject>(SectionObj) });
			return TrackObj;
		}
		if (UMovieSceneColorTrack* ColorTrack = Cast<UMovieSceneColorTrack>(Track))
		{
			UMovieSceneColorSection* Section = ColorTrack->GetAllSections().Num() > 0 ? Cast<UMovieSceneColorSection>(ColorTrack->GetAllSections()[0]) : nullptr;
			return BuildColorTrackJson(ColorTrack, Section, MovieScene);
		}
		if (UMovieSceneByteTrack* ByteTrack = Cast<UMovieSceneByteTrack>(Track))
		{
			return BuildByteTrackJson(ByteTrack, MovieScene);
		}
		if (UMovieSceneStringTrack* StringTrack = Cast<UMovieSceneStringTrack>(Track))
		{
			return BuildStringTrackJson(StringTrack, MovieScene);
		}
		if (UMovieSceneFloatVectorTrack* FloatVectorTrack = Cast<UMovieSceneFloatVectorTrack>(Track))
		{
			UMovieSceneFloatVectorSection* Section = FloatVectorTrack->GetAllSections().Num() > 0 ? Cast<UMovieSceneFloatVectorSection>(FloatVectorTrack->GetAllSections()[0]) : nullptr;
			return BuildVectorTrackJson(FloatVectorTrack, Section, MovieScene, TEXT("float"), FloatVectorTrack->GetNumChannelsUsed());
		}
		if (UMovieSceneDoubleVectorTrack* DoubleVectorTrack = Cast<UMovieSceneDoubleVectorTrack>(Track))
		{
			UMovieSceneDoubleVectorSection* Section = DoubleVectorTrack->GetAllSections().Num() > 0 ? Cast<UMovieSceneDoubleVectorSection>(DoubleVectorTrack->GetAllSections()[0]) : nullptr;
			return BuildVectorTrackJson(DoubleVectorTrack, Section, MovieScene, TEXT("double"), DoubleVectorTrack->GetNumChannelsUsed());
		}
		if (UMovieSceneRotatorTrack* RotatorTrack = Cast<UMovieSceneRotatorTrack>(Track))
		{
			return BuildRotatorTrackJson(RotatorTrack, MovieScene);
		}
		if (UMovieSceneActorReferenceTrack* ActorReferenceTrack = Cast<UMovieSceneActorReferenceTrack>(Track))
		{
			return BuildActorReferenceTrackJson(ActorReferenceTrack, MovieScene);
		}
		if (UMovieSceneObjectPropertyTrack* ObjectTrack = Cast<UMovieSceneObjectPropertyTrack>(Track))
		{
			return BuildObjectPropertyTrackJson(ObjectTrack, MovieScene);
		}

		return BuildUnsupportedTrackJson(Track, MovieScene);
	}

	static TSharedPtr<FJsonObject> BuildBindingJson(ULevelSequence* Sequence, UMovieScene* MovieScene, const FMovieSceneBinding& Binding)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> BindingObj = MakeShared<FJsonObject>();
		BindingObj->SetStringField(TEXT("binding_guid"), Binding.GetObjectGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
		BindingObj->SetStringField(TEXT("binding_name"), Binding.GetName());
		BindingObj->SetStringField(TEXT("binding_id"), Binding.GetObjectGuid().ToString(EGuidFormats::DigitsWithHyphensLower));

		if (MovieScene)
		{
			if (const FMovieScenePossessable* Possessable = MovieScene->FindPossessable(Binding.GetObjectGuid()))
			{
				BindingObj->SetStringField(TEXT("binding_kind"), TEXT("possessable"));
				const UClass* PossessedObjectClass = Possessable->GetPossessedObjectClass();
				BindingObj->SetStringField(TEXT("possessed_object_class"), PossessedObjectClass ? PossessedObjectClass->GetPathName() : TEXT(""));
				BindingObj->SetStringField(TEXT("parent_binding_guid"), Possessable->GetParent().IsValid() ? Possessable->GetParent().ToString(EGuidFormats::DigitsWithHyphensLower) : TEXT(""));
				if (PossessedObjectClass && PossessedObjectClass->IsChildOf(UActorComponent::StaticClass()))
				{
					BindingObj->SetStringField(TEXT("primary_bound_object_kind"), TEXT("component"));
					BindingObj->SetStringField(TEXT("preferred_component_name"), Binding.GetName());
				}
			}
			else if (const FMovieSceneSpawnable* Spawnable = MovieScene->FindSpawnable(Binding.GetObjectGuid()))
			{
				BindingObj->SetStringField(TEXT("binding_kind"), TEXT("spawnable"));
				BindingObj->SetStringField(TEXT("spawnable_object_class"), (Spawnable->GetObjectTemplate() && Spawnable->GetObjectTemplate()->GetClass()) ? Spawnable->GetObjectTemplate()->GetClass()->GetPathName() : TEXT(""));
				BindingObj->SetStringField(TEXT("spawnable_template_name"), Spawnable->GetObjectTemplate() ? Spawnable->GetObjectTemplate()->GetName() : TEXT(""));
				BindingObj->SetStringField(TEXT("spawnable_template_path"), Spawnable->GetObjectTemplate() ? Spawnable->GetObjectTemplate()->GetPathName() : TEXT(""));
			}
			else
			{
				BindingObj->SetStringField(TEXT("binding_kind"), TEXT("unknown"));
			}
		}

		TArray<TSharedPtr<FJsonValue>> BoundObjects;
		AppendResolvedBoundObjects(Sequence, Binding.GetObjectGuid(), BoundObjects);
		BindingObj->SetArrayField(TEXT("bound_objects"), BoundObjects);
		if (BoundObjects.Num() > 0)
		{
			const TSharedPtr<FJsonObject>* FirstBoundObject = nullptr;
			if (BoundObjects[0].IsValid() && BoundObjects[0]->TryGetObject(FirstBoundObject) && FirstBoundObject && (*FirstBoundObject).IsValid())
			{
				FString ObjectKind;
				if ((*FirstBoundObject)->TryGetStringField(TEXT("object_kind"), ObjectKind))
				{
					FString ExistingPrimaryKind;
					BindingObj->TryGetStringField(TEXT("primary_bound_object_kind"), ExistingPrimaryKind);
					if (ExistingPrimaryKind.IsEmpty() || !ExistingPrimaryKind.Equals(TEXT("component"), ESearchCase::IgnoreCase))
					{
						BindingObj->SetStringField(TEXT("primary_bound_object_kind"), ObjectKind);
					}
					if (ObjectKind.Equals(TEXT("actor"), ESearchCase::IgnoreCase))
					{
						FString ActorLabel;
						FString ActorName;
						if ((*FirstBoundObject)->TryGetStringField(TEXT("actor_label"), ActorLabel))
						{
							if (ExistingPrimaryKind.Equals(TEXT("component"), ESearchCase::IgnoreCase))
							{
								BindingObj->SetStringField(TEXT("owner_actor_label"), ActorLabel);
							}
							else
							{
								BindingObj->SetStringField(TEXT("actor_label"), ActorLabel);
							}
						}
						if ((*FirstBoundObject)->TryGetStringField(TEXT("actor_name"), ActorName))
						{
							if (ExistingPrimaryKind.Equals(TEXT("component"), ESearchCase::IgnoreCase))
							{
								BindingObj->SetStringField(TEXT("owner_actor_name"), ActorName);
								BindingObj->SetStringField(TEXT("preferred_owner_actor_id"), !ActorLabel.IsEmpty() ? ActorLabel : ActorName);
							}
							else
							{
								BindingObj->SetStringField(TEXT("actor_name"), ActorName);
								BindingObj->SetStringField(TEXT("preferred_actor_id"), !ActorLabel.IsEmpty() ? ActorLabel : ActorName);
							}
						}
					}
					else if (ObjectKind.Equals(TEXT("component"), ESearchCase::IgnoreCase))
					{
						FString ComponentName;
						FString OwnerActorLabel;
						FString OwnerActorName;
						if ((*FirstBoundObject)->TryGetStringField(TEXT("component_name"), ComponentName))
						{
							BindingObj->SetStringField(TEXT("preferred_component_name"), ComponentName);
						}
						if ((*FirstBoundObject)->TryGetStringField(TEXT("owner_actor_label"), OwnerActorLabel))
						{
							BindingObj->SetStringField(TEXT("owner_actor_label"), OwnerActorLabel);
						}
						if ((*FirstBoundObject)->TryGetStringField(TEXT("owner_actor_name"), OwnerActorName))
						{
							BindingObj->SetStringField(TEXT("owner_actor_name"), OwnerActorName);
							BindingObj->SetStringField(TEXT("preferred_owner_actor_id"), !OwnerActorLabel.IsEmpty() ? OwnerActorLabel : OwnerActorName);
						}
					}
				}
			}
		}
		Root->SetObjectField(TEXT("binding"), BindingObj);

		TArray<TSharedPtr<FJsonValue>> Tracks;
		for (UMovieSceneTrack* Track : Binding.GetTracks())
		{
			if (TSharedPtr<FJsonObject> TrackObj = BuildTrackJson(MovieScene, Track))
			{
				Tracks.Add(MakeShared<FJsonValueObject>(TrackObj));
			}
		}
		Root->SetArrayField(TEXT("tracks"), Tracks);
		return Root;
	}

	static TSharedPtr<FJsonObject> BuildMasterTracksIndexJson(UMovieScene* MovieScene)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Tracks;
		if (MovieScene)
		{
			if (UMovieSceneTrack* CameraCutTrack = MovieScene->GetCameraCutTrack())
			{
				if (TSharedPtr<FJsonObject> TrackObj = BuildTrackJson(MovieScene, CameraCutTrack))
				{
					Tracks.Add(MakeShared<FJsonValueObject>(TrackObj));
				}
			}
			for (UMovieSceneTrack* Track : MovieScene->GetTracks())
			{
				if (Track == MovieScene->GetCameraCutTrack())
				{
					continue;
				}
				if (TSharedPtr<FJsonObject> TrackObj = BuildTrackJson(MovieScene, Track))
				{
					Tracks.Add(MakeShared<FJsonValueObject>(TrackObj));
				}
			}
		}
		Root->SetArrayField(TEXT("tracks"), Tracks);
		return Root;
	}

	static UMovieSceneTrack* FindTrackForSpec(UMovieScene* MovieScene, const FGuid& BindingGuid, const TSharedPtr<FJsonObject>& TrackObj);

	static FString JoinFolderPathSegments(const TArray<FName>& Segments)
	{
		TArray<FString> Parts;
		Parts.Reserve(Segments.Num());
		for (const FName& Segment : Segments)
		{
			Parts.Add(Segment.ToString());
		}
		return FString::Join(Parts, TEXT("/"));
	}

	static TSharedPtr<FJsonObject> BuildFolderColorJson(const FColor& InColor)
	{
		TSharedPtr<FJsonObject> ColorObj = MakeShared<FJsonObject>();
		ColorObj->SetNumberField(TEXT("r"), InColor.R);
		ColorObj->SetNumberField(TEXT("g"), InColor.G);
		ColorObj->SetNumberField(TEXT("b"), InColor.B);
		ColorObj->SetNumberField(TEXT("a"), InColor.A);
		return ColorObj;
	}

	static bool TryParseFolderColorJson(const TSharedPtr<FJsonObject>& ColorObj, FColor& OutColor)
	{
		if (!ColorObj.IsValid())
		{
			return false;
		}

		double R = 255.0;
		double G = 255.0;
		double B = 255.0;
		double A = 255.0;
		if (!ColorObj->TryGetNumberField(TEXT("r"), R)
			|| !ColorObj->TryGetNumberField(TEXT("g"), G)
			|| !ColorObj->TryGetNumberField(TEXT("b"), B))
		{
			return false;
		}
		ColorObj->TryGetNumberField(TEXT("a"), A);
		OutColor = FColor(
			static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(R), 0, 255)),
			static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(G), 0, 255)),
			static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(B), 0, 255)),
			static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(A), 0, 255)));
		return true;
	}

	static FString GetStableMasterTrackType(UMovieScene* MovieScene, UMovieSceneTrack* Track)
	{
		if (!MovieScene || !Track)
		{
			return FString();
		}
		if (Track == MovieScene->GetCameraCutTrack() || Cast<UMovieSceneCameraCutTrack>(Track))
		{
			return TEXT("camera_cut");
		}
		if (Cast<UMovieSceneCinematicShotTrack>(Track))
		{
			return TEXT("cinematic_shot");
		}
		if (Cast<UMovieSceneSubTrack>(Track))
		{
			return TEXT("sub_sequence");
		}
		return FString();
	}

	static UMovieSceneTrack* FindStableMasterTrackByType(UMovieScene* MovieScene, const FString& TrackType)
	{
		if (!MovieScene)
		{
			return nullptr;
		}
		if (TrackType.Equals(TEXT("camera_cut"), ESearchCase::IgnoreCase))
		{
			return MovieScene->GetCameraCutTrack();
		}
		if (TrackType.Equals(TEXT("cinematic_shot"), ESearchCase::IgnoreCase))
		{
			return MovieScene->FindTrack<UMovieSceneCinematicShotTrack>();
		}
		if (TrackType.Equals(TEXT("sub_sequence"), ESearchCase::IgnoreCase))
		{
			return MovieScene->FindTrack<UMovieSceneSubTrack>();
		}
		return nullptr;
	}

	static TSharedPtr<FJsonObject> BuildBindingTrackRefJson(const FGuid& BindingGuid, const FString& TrackFileName, const TSharedPtr<FJsonObject>& TrackJson)
	{
		if (!BindingGuid.IsValid() || TrackFileName.IsEmpty())
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> TrackRefObj = MakeShared<FJsonObject>();
		TrackRefObj->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		TrackRefObj->SetStringField(TEXT("track_file"), TrackFileName);
		if (TrackJson.IsValid())
		{
			FString FieldValue;
			if (TrackJson->TryGetStringField(TEXT("track_type"), FieldValue) && !FieldValue.IsEmpty())
			{
				TrackRefObj->SetStringField(TEXT("track_type"), FieldValue);
			}
			if (TrackJson->TryGetStringField(TEXT("property_type"), FieldValue) && !FieldValue.IsEmpty())
			{
				TrackRefObj->SetStringField(TEXT("property_type"), FieldValue);
			}
			if (TrackJson->TryGetStringField(TEXT("property_name"), FieldValue) && !FieldValue.IsEmpty())
			{
				TrackRefObj->SetStringField(TEXT("property_name"), FieldValue);
			}
			if (TrackJson->TryGetStringField(TEXT("property_path"), FieldValue) && !FieldValue.IsEmpty())
			{
				TrackRefObj->SetStringField(TEXT("property_path"), FieldValue);
			}
		}
		return TrackRefObj;
	}

	static void AppendFolderJsonRecursive(
		UMovieScene* MovieScene,
		UMovieSceneFolder* Folder,
		const TMap<const UMovieSceneTrack*, TSharedPtr<FJsonObject>>& BindingTrackRefsByTrack,
		TArray<TSharedPtr<FJsonValue>>& OutFolders)
	{
		if (!MovieScene || !Folder)
		{
			return;
		}

		TArray<FName> FolderPathSegments;
		UMovieSceneFolder::CalculateFolderPath(Folder, MovieScene->GetRootFolders(), FolderPathSegments);
		const FString FolderId = JoinFolderPathSegments(FolderPathSegments);
		TArray<FName> ParentSegments = FolderPathSegments;
		if (ParentSegments.Num() > 0)
		{
			ParentSegments.RemoveAt(ParentSegments.Num() - 1);
		}
		const FString ParentFolderId = JoinFolderPathSegments(ParentSegments);

		TSharedPtr<FJsonObject> FolderObj = MakeShared<FJsonObject>();
		FolderObj->SetStringField(TEXT("folder_id"), FolderId);
		FolderObj->SetStringField(TEXT("folder_name"), Folder->GetFolderName().ToString());
		FolderObj->SetStringField(TEXT("folder_path"), FolderId);
		FolderObj->SetStringField(TEXT("parent_folder_id"), ParentFolderId);
#if WITH_EDITORONLY_DATA
		FolderObj->SetObjectField(TEXT("color"), BuildFolderColorJson(Folder->GetFolderColor()));
		FolderObj->SetNumberField(TEXT("sorting_order"), Folder->GetSortingOrder());
#endif

		TArray<TSharedPtr<FJsonValue>> ChildBindingGuids;
		for (const FGuid& ChildBindingGuid : Folder->GetChildObjectBindings())
		{
			ChildBindingGuids.Add(MakeShared<FJsonValueString>(ChildBindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
		}
		FolderObj->SetArrayField(TEXT("child_binding_guids"), ChildBindingGuids);

		TArray<TSharedPtr<FJsonValue>> ChildMasterTracks;
		TArray<TSharedPtr<FJsonValue>> ChildBindingTracks;
		for (UMovieSceneTrack* ChildTrack : Folder->GetChildTracks())
		{
			if (const TSharedPtr<FJsonObject>* BindingTrackRef = BindingTrackRefsByTrack.Find(ChildTrack))
			{
				if (BindingTrackRef && BindingTrackRef->IsValid())
				{
					ChildBindingTracks.Add(MakeShared<FJsonValueObject>(*BindingTrackRef));
				}
				continue;
			}

			const FString TrackType = GetStableMasterTrackType(MovieScene, ChildTrack);
			if (TrackType.IsEmpty())
			{
				continue;
			}

			TSharedPtr<FJsonObject> TrackRefObj = MakeShared<FJsonObject>();
			TrackRefObj->SetStringField(TEXT("track_type"), TrackType);
			ChildMasterTracks.Add(MakeShared<FJsonValueObject>(TrackRefObj));
		}
		FolderObj->SetArrayField(TEXT("child_binding_tracks"), ChildBindingTracks);
		FolderObj->SetArrayField(TEXT("child_master_tracks"), ChildMasterTracks);

		OutFolders.Add(MakeShared<FJsonValueObject>(FolderObj));

		for (UMovieSceneFolder* ChildFolder : Folder->GetChildFolders())
		{
			AppendFolderJsonRecursive(MovieScene, ChildFolder, BindingTrackRefsByTrack, OutFolders);
		}
	}

	static TSharedPtr<FJsonObject> BuildOutlinerFoldersJson(UMovieScene* MovieScene, const TMap<const UMovieSceneTrack*, TSharedPtr<FJsonObject>>& BindingTrackRefsByTrack)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Folders;
		if (MovieScene)
		{
			for (UMovieSceneFolder* RootFolder : MovieScene->GetRootFolders())
			{
				AppendFolderJsonRecursive(MovieScene, RootFolder, BindingTrackRefsByTrack, Folders);
			}
		}
		Root->SetArrayField(TEXT("folders"), Folders);
		return Root;
	}

	static bool ApplyOutlinerFoldersJson(
		ULevelSequence* Sequence,
		UMovieScene* MovieScene,
		const TMap<FGuid, FString>& BindingFolderByGuid,
		const FString& FolderPath,
		const TSharedPtr<FJsonObject>& OutlinerJson,
		TArray<TSharedPtr<FJsonValue>>& Warnings,
		int32& OutFoldersApplied,
		FString& OutError)
	{
		OutFoldersApplied = 0;
		if (!Sequence || !MovieScene || !OutlinerJson.IsValid())
		{
			return true;
		}

		const TArray<TSharedPtr<FJsonValue>>* FolderEntries = nullptr;
		if (!OutlinerJson->TryGetArrayField(TEXT("folders"), FolderEntries) || !FolderEntries)
		{
			return true;
		}

		Sequence->Modify();
		MovieScene->Modify();
		MovieScene->EmptyRootFolders();

		TMap<FString, TSharedPtr<FJsonObject>> FolderSpecById;
		TMap<FString, UMovieSceneFolder*> FolderById;

		for (const TSharedPtr<FJsonValue>& FolderValue : *FolderEntries)
		{
			const TSharedPtr<FJsonObject>* FolderObj = nullptr;
			if (!FolderValue.IsValid() || !FolderValue->TryGetObject(FolderObj) || !FolderObj || !(*FolderObj).IsValid())
			{
				continue;
			}

			FString FolderId;
			if (!(*FolderObj)->TryGetStringField(TEXT("folder_id"), FolderId) || FolderId.IsEmpty())
			{
				(*FolderObj)->TryGetStringField(TEXT("folder_path"), FolderId);
			}
			if (FolderId.IsEmpty())
			{
				FString FolderName;
				(*FolderObj)->TryGetStringField(TEXT("folder_name"), FolderName);
				FolderId = FolderName;
			}
			if (FolderId.IsEmpty())
			{
				AddWarning(Warnings, TEXT("folder_id_missing"), TEXT("Skipped folder entry with no folder_id or folder_name."));
				continue;
			}
			if (FolderSpecById.Contains(FolderId))
			{
				AddWarning(Warnings, TEXT("folder_id_duplicate"), FString::Printf(TEXT("Skipped duplicate folder '%s'."), *FolderId));
				continue;
			}

			FString FolderName;
			(*FolderObj)->TryGetStringField(TEXT("folder_name"), FolderName);
			if (FolderName.IsEmpty())
			{
				FolderName = FolderId;
				int32 SlashIndex = INDEX_NONE;
				if (FolderName.FindLastChar(TEXT('/'), SlashIndex))
				{
					FolderName = FolderName.Mid(SlashIndex + 1);
				}
			}

			UMovieSceneFolder* NewFolder = NewObject<UMovieSceneFolder>(MovieScene, NAME_None, RF_Transactional);
			if (!NewFolder)
			{
				OutError = TEXT("create_movie_scene_folder_failed");
				return false;
			}
			NewFolder->SetFolderName(FName(*FolderName));
#if WITH_EDITORONLY_DATA
			if (const TSharedPtr<FJsonObject>* ColorObj = nullptr; (*FolderObj)->TryGetObjectField(TEXT("color"), ColorObj) && ColorObj && (*ColorObj).IsValid())
			{
				FColor FolderColor;
				if (TryParseFolderColorJson(*ColorObj, FolderColor))
				{
					NewFolder->SetFolderColor(FolderColor);
				}
			}
			double SortingOrder = 0.0;
			if ((*FolderObj)->TryGetNumberField(TEXT("sorting_order"), SortingOrder))
			{
				NewFolder->SetSortingOrder(FMath::RoundToInt(SortingOrder));
			}
#endif
			FolderSpecById.Add(FolderId, *FolderObj);
			FolderById.Add(FolderId, NewFolder);
		}

		TSet<FString> AttachedFolders;
		TSet<FString> AttachingFolders;
		TFunction<void(const FString&)> AttachFolderRecursive = [&](const FString& FolderId)
		{
			if (AttachedFolders.Contains(FolderId))
			{
				return;
			}

			UMovieSceneFolder* Folder = FolderById.FindRef(FolderId);
			TSharedPtr<FJsonObject> FolderSpec = FolderSpecById.FindRef(FolderId);
			if (!Folder || !FolderSpec.IsValid())
			{
				return;
			}

			if (AttachingFolders.Contains(FolderId))
			{
				AddWarning(Warnings, TEXT("folder_cycle_detected"), FString::Printf(TEXT("Detected folder cycle at '%s'; attached it as a root folder."), *FolderId));
				MovieScene->AddRootFolder(Folder);
				AttachedFolders.Add(FolderId);
				return;
			}

			AttachingFolders.Add(FolderId);
			FString ParentFolderId;
			FolderSpec->TryGetStringField(TEXT("parent_folder_id"), ParentFolderId);
			if (ParentFolderId.IsEmpty())
			{
				MovieScene->AddRootFolder(Folder);
			}
			else
			{
				UMovieSceneFolder* ParentFolder = FolderById.FindRef(ParentFolderId);
				if (!ParentFolder)
				{
					AddWarning(Warnings, TEXT("folder_parent_not_found"), FString::Printf(TEXT("Attached folder '%s' as root because parent '%s' was not found."), *FolderId, *ParentFolderId));
					MovieScene->AddRootFolder(Folder);
				}
				else
				{
					AttachFolderRecursive(ParentFolderId);
					ParentFolder->AddChildFolder(Folder);
				}
			}
			AttachingFolders.Remove(FolderId);
			AttachedFolders.Add(FolderId);
		};

		for (const TPair<FString, UMovieSceneFolder*>& Pair : FolderById)
		{
			AttachFolderRecursive(Pair.Key);
		}

		for (const TPair<FString, UMovieSceneFolder*>& Pair : FolderById)
		{
			const FString& FolderId = Pair.Key;
			UMovieSceneFolder* Folder = Pair.Value;
			TSharedPtr<FJsonObject> FolderSpec = FolderSpecById.FindRef(FolderId);
			if (!Folder || !FolderSpec.IsValid())
			{
				continue;
			}

			const TArray<TSharedPtr<FJsonValue>>* ChildBindingGuids = nullptr;
			if (FolderSpec->TryGetArrayField(TEXT("child_binding_guids"), ChildBindingGuids) && ChildBindingGuids)
			{
				for (const TSharedPtr<FJsonValue>& BindingValue : *ChildBindingGuids)
				{
					FString BindingGuidText;
					if (!BindingValue.IsValid() || !BindingValue->TryGetString(BindingGuidText) || BindingGuidText.IsEmpty())
					{
						continue;
					}

					FGuid BindingGuid;
					if (!FGuid::Parse(BindingGuidText, BindingGuid) || !MovieScene->FindBinding(BindingGuid))
					{
						AddWarning(Warnings, TEXT("folder_binding_not_found"), FString::Printf(TEXT("Skipped missing binding '%s' in folder '%s'."), *BindingGuidText, *FolderId));
						continue;
					}
					Folder->AddChildObjectBinding(BindingGuid);
				}
			}

			const TArray<TSharedPtr<FJsonValue>>* ChildMasterTracks = nullptr;
			if (FolderSpec->TryGetArrayField(TEXT("child_master_tracks"), ChildMasterTracks) && ChildMasterTracks)
			{
				for (const TSharedPtr<FJsonValue>& TrackValue : *ChildMasterTracks)
				{
					FString TrackType;
					if (TrackValue.IsValid())
					{
						const TSharedPtr<FJsonObject>* TrackObj = nullptr;
						if (TrackValue->TryGetObject(TrackObj) && TrackObj && (*TrackObj).IsValid())
						{
							(*TrackObj)->TryGetStringField(TEXT("track_type"), TrackType);
						}
						else
						{
							TrackValue->TryGetString(TrackType);
						}
					}

					if (TrackType.IsEmpty())
					{
						continue;
					}

					UMovieSceneTrack* MasterTrack = FindStableMasterTrackByType(MovieScene, TrackType);
					if (!MasterTrack)
					{
						AddWarning(Warnings, TEXT("folder_master_track_not_found"), FString::Printf(TEXT("Skipped missing master track '%s' in folder '%s'."), *TrackType, *FolderId));
						continue;
					}
					Folder->AddChildTrack(MasterTrack);
				}
			}

			const TArray<TSharedPtr<FJsonValue>>* ChildBindingTracks = nullptr;
			if (FolderSpec->TryGetArrayField(TEXT("child_binding_tracks"), ChildBindingTracks) && ChildBindingTracks)
			{
				for (const TSharedPtr<FJsonValue>& TrackValue : *ChildBindingTracks)
				{
					const TSharedPtr<FJsonObject>* TrackRefObj = nullptr;
					if (!TrackValue.IsValid() || !TrackValue->TryGetObject(TrackRefObj) || !TrackRefObj || !(*TrackRefObj).IsValid())
					{
						continue;
					}

					FString BindingGuidText;
					FString TrackFileName;
					if (!(*TrackRefObj)->TryGetStringField(TEXT("binding_guid"), BindingGuidText) || BindingGuidText.IsEmpty()
						|| !(*TrackRefObj)->TryGetStringField(TEXT("track_file"), TrackFileName) || TrackFileName.IsEmpty())
					{
						AddWarning(Warnings, TEXT("folder_binding_track_ref_invalid"), FString::Printf(TEXT("Skipped invalid child_binding_tracks entry in folder '%s'."), *FolderId));
						continue;
					}

					FGuid BindingGuid;
					if (!FGuid::Parse(BindingGuidText, BindingGuid))
					{
						AddWarning(Warnings, TEXT("folder_binding_track_guid_invalid"), FString::Printf(TEXT("Skipped invalid binding guid '%s' in folder '%s'."), *BindingGuidText, *FolderId));
						continue;
					}

					const FString BindingFolderName = BindingFolderByGuid.FindRef(BindingGuid);
					if (BindingFolderName.IsEmpty())
					{
						AddWarning(Warnings, TEXT("folder_binding_track_binding_not_found"), FString::Printf(TEXT("Skipped child binding track because binding '%s' was not found for folder '%s'."), *BindingGuidText, *FolderId));
						continue;
					}

					TSharedPtr<FJsonObject> TrackJson;
					FString ResolvedTrackPath;
					FString TrackLoadError;
					const FString TrackSpecPath = FPaths::Combine(FolderPath, TEXT("bindings"), BindingFolderName, TEXT("tracks"), TrackFileName);
					if (!UeAgentSequenceOps::LoadJsonFile(TrackSpecPath, TrackJson, ResolvedTrackPath, TrackLoadError) || !TrackJson.IsValid())
					{
						AddWarning(Warnings, TEXT("folder_binding_track_spec_load_failed"), FString::Printf(TEXT("Skipped child binding track spec '%s' in folder '%s': %s."), *TrackFileName, *FolderId, *TrackLoadError));
						continue;
					}

					UMovieSceneTrack* BindingTrack = FindTrackForSpec(MovieScene, BindingGuid, TrackJson);
					if (!BindingTrack)
					{
						AddWarning(Warnings, TEXT("folder_binding_track_not_found"), FString::Printf(TEXT("Skipped child binding track '%s' because it could not be resolved in folder '%s'."), *TrackFileName, *FolderId));
						continue;
					}

					Folder->AddChildTrack(BindingTrack);
				}
			}
		}

		Sequence->MarkPackageDirty();
		OutFoldersApplied = FolderById.Num();
		return true;
	}

	static UActorComponent* FindActorComponentByName(AActor* OwnerActor, const FString& ComponentName)
	{
		if (!OwnerActor || ComponentName.IsEmpty())
		{
			return nullptr;
		}

		TArray<UActorComponent*> Components;
		OwnerActor->GetComponents(Components);
		for (UActorComponent* Component : Components)
		{
			if (!Component)
			{
				continue;
			}
			if (Component->GetName().Equals(ComponentName, ESearchCase::IgnoreCase) || Component->GetFName().ToString().Equals(ComponentName, ESearchCase::IgnoreCase))
			{
				return Component;
			}
		}
		return nullptr;
	}

	static AActor* FindActorByNameOrLabelLocal(UWorld* World, const FString& ActorId)
	{
		if (!World || ActorId.IsEmpty())
		{
			return nullptr;
		}

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor)
			{
				continue;
			}
			if (Actor->GetName().Equals(ActorId, ESearchCase::IgnoreCase) || Actor->GetActorLabel().Equals(ActorId, ESearchCase::IgnoreCase))
			{
				return Actor;
			}
		}
		return nullptr;
	}

	static bool AddPossessableBindingDirect(
		ULevelSequence* Sequence,
		UMovieScene* MovieScene,
		const FGuid& DesiredGuid,
		const FString& BindingName,
		UObject& BoundObject,
		UObject* BindingContext,
		const FGuid& ParentGuid,
		FString& OutError)
	{
		if (!Sequence || !MovieScene || !DesiredGuid.IsValid())
		{
			OutError = TEXT("invalid_possessable_recreate_context");
			return false;
		}
		if (MovieScene->FindBinding(DesiredGuid))
		{
			return true;
		}

		Sequence->Modify();
		MovieScene->Modify();

		FMovieScenePossessable NewPossessable(BindingName, BoundObject.GetClass());
		NewPossessable.SetGuid(DesiredGuid);
		if (ParentGuid.IsValid())
		{
			NewPossessable.SetParent(ParentGuid, MovieScene);
		}

		MovieScene->AddPossessable(NewPossessable, FMovieSceneBinding(DesiredGuid, BindingName));
		Sequence->BindPossessableObject(DesiredGuid, BoundObject, BindingContext);
		Sequence->MarkPackageDirty();
		return true;
	}

	static bool BuildSpawnableSourceObject(
		const FString& ClassPath,
		UObject*& OutSourceObject,
		UObject*& OutCleanupObject,
		FString& OutError)
	{
		OutSourceObject = nullptr;
		OutCleanupObject = nullptr;

		UClass* ObjectClass = LoadObject<UClass>(nullptr, *ClassPath);
		if (!ObjectClass)
		{
			OutError = TEXT("spawnable_class_not_found");
			return false;
		}

		if (ObjectClass->IsChildOf(AActor::StaticClass()))
		{
			if (!GEditor)
			{
				OutError = TEXT("editor_not_available");
				return false;
			}
			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (!World)
			{
				OutError = TEXT("editor_world_not_found");
				return false;
			}

			FActorSpawnParameters SpawnParams;
			SpawnParams.ObjectFlags = RF_Transient;
			SpawnParams.bHideFromSceneOutliner = true;
			SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			AActor* TempActor = World->SpawnActor<AActor>(ObjectClass, FTransform::Identity, SpawnParams);
			if (!TempActor)
			{
				OutError = TEXT("spawnable_temp_actor_create_failed");
				return false;
			}
			OutSourceObject = TempActor;
			OutCleanupObject = TempActor;
			return true;
		}

		UObject* TempObject = NewObject<UObject>(GetTransientPackage(), ObjectClass, NAME_None, RF_Transient);
		if (!TempObject)
		{
			OutError = TEXT("spawnable_temp_object_create_failed");
			return false;
		}
		OutSourceObject = TempObject;
		OutCleanupObject = TempObject;
		return true;
	}

	static bool AddSpawnableBindingDirect(
		ULevelSequence* Sequence,
		UMovieScene* MovieScene,
		const FGuid& DesiredGuid,
		const FString& BindingName,
		UObject& SourceObject,
		FString& OutError)
	{
		if (!Sequence || !MovieScene || !DesiredGuid.IsValid())
		{
			OutError = TEXT("invalid_spawnable_recreate_context");
			return false;
		}
		if (MovieScene->FindBinding(DesiredGuid))
		{
			return true;
		}

		Sequence->Modify();
		MovieScene->Modify();

		UObject* TemplateObject = Sequence->MakeSpawnableTemplateFromInstance(SourceObject, FName(*BindingName));
		if (!TemplateObject)
		{
			OutError = TEXT("spawnable_template_create_failed");
			return false;
		}

		FMovieSceneSpawnable NewSpawnable(BindingName, *TemplateObject);
		NewSpawnable.SetGuid(DesiredGuid);
		MovieScene->AddSpawnable(NewSpawnable, FMovieSceneBinding(DesiredGuid, BindingName));

		UMovieSceneSpawnTrack* SpawnTrack = MovieScene->FindTrack<UMovieSceneSpawnTrack>(DesiredGuid);
		if (!SpawnTrack)
		{
			SpawnTrack = MovieScene->AddTrack<UMovieSceneSpawnTrack>(DesiredGuid);
		}
		if (!SpawnTrack)
		{
			OutError = TEXT("spawn_track_create_failed");
			return false;
		}
		SpawnTrack->SetObjectId(DesiredGuid);
		if (SpawnTrack->GetAllSections().Num() <= 0)
		{
			if (UMovieSceneSection* NewSection = SpawnTrack->CreateNewSection())
			{
				NewSection->SetRange(TRange<FFrameNumber>::All());
				SpawnTrack->AddSection(*NewSection);
			}
		}

		Sequence->MarkPackageDirty();
		return true;
	}

	static bool TryResolveBindingFromSpec(
		ULevelSequence* Sequence,
		UMovieScene* MovieScene,
		const TMap<FGuid, FString>& BindingFolderByGuid,
		const FString& FolderPath,
		const FGuid& DesiredGuid,
		const FString& BindingJsonPath,
		TSet<FGuid>& ResolvingBindings,
		TArray<TSharedPtr<FJsonValue>>& Warnings,
		bool& bOutRecovered,
		FString& OutError)
	{
		bOutRecovered = false;
		if (!Sequence || !MovieScene || !DesiredGuid.IsValid())
		{
			OutError = TEXT("binding_recreate_context_invalid");
			return false;
		}
		if (MovieScene->FindBinding(DesiredGuid))
		{
			bOutRecovered = true;
			return true;
		}
		if (ResolvingBindings.Contains(DesiredGuid))
		{
			UeAgentSequenceFolderOps::AddWarning(Warnings, TEXT("binding_recreate_cycle"), FString::Printf(TEXT("Skipped cyclic binding recreation for '%s'."), *DesiredGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
			return true;
		}

		ResolvingBindings.Add(DesiredGuid);
		ON_SCOPE_EXIT { ResolvingBindings.Remove(DesiredGuid); };

		TSharedPtr<FJsonObject> BindingRoot;
		FString ResolvedBindingJsonPath;
		if (!UeAgentSequenceOps::LoadJsonFile(BindingJsonPath, BindingRoot, ResolvedBindingJsonPath, OutError))
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* BindingObj = nullptr;
		if (!BindingRoot.IsValid() || !BindingRoot->TryGetObjectField(TEXT("binding"), BindingObj) || !BindingObj || !(*BindingObj).IsValid())
		{
			UeAgentSequenceFolderOps::AddWarning(Warnings, TEXT("binding_spec_missing"), FString::Printf(TEXT("Skipped missing binding spec for '%s'."), *DesiredGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
			return true;
		}

		FString BindingName;
		if (!(*BindingObj)->TryGetStringField(TEXT("binding_name"), BindingName) || BindingName.IsEmpty())
		{
			BindingName = TEXT("RecoveredBinding");
		}

		FString BindingKind;
		(*BindingObj)->TryGetStringField(TEXT("binding_kind"), BindingKind);

		if (BindingKind.Equals(TEXT("possessable"), ESearchCase::IgnoreCase))
		{
			FString PrimaryKind;
			(*BindingObj)->TryGetStringField(TEXT("primary_bound_object_kind"), PrimaryKind);

			if (PrimaryKind.Equals(TEXT("actor"), ESearchCase::IgnoreCase))
			{
				FString PreferredActorId;
				if (!(*BindingObj)->TryGetStringField(TEXT("preferred_actor_id"), PreferredActorId) || PreferredActorId.IsEmpty())
				{
					(*BindingObj)->TryGetStringField(TEXT("actor_label"), PreferredActorId);
					if (PreferredActorId.IsEmpty())
					{
						(*BindingObj)->TryGetStringField(TEXT("actor_name"), PreferredActorId);
					}
				}
				if (PreferredActorId.IsEmpty())
				{
					UeAgentSequenceFolderOps::AddWarning(Warnings, TEXT("binding_recreate_missing_actor"), FString::Printf(TEXT("Skipped actor binding recreation for '%s' because actor identity is missing."), *DesiredGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
					return true;
				}

				if (!GEditor)
				{
					OutError = TEXT("editor_not_available");
					return false;
				}
				UWorld* World = GEditor->GetEditorWorldContext().World();
				if (!World)
				{
					OutError = TEXT("editor_world_not_found");
					return false;
				}
				AActor* Actor = FindActorByNameOrLabelLocal(World, PreferredActorId);
				if (!Actor)
				{
					UeAgentSequenceFolderOps::AddWarning(Warnings, TEXT("binding_recreate_actor_not_found"), FString::Printf(TEXT("Skipped actor binding recreation for '%s' because actor '%s' was not found."), *DesiredGuid.ToString(EGuidFormats::DigitsWithHyphensLower), *PreferredActorId));
					return true;
				}

				if (!AddPossessableBindingDirect(Sequence, MovieScene, DesiredGuid, BindingName, *Actor, World, FGuid(), OutError))
				{
					return false;
				}
				bOutRecovered = true;
				return true;
			}

			if (PrimaryKind.Equals(TEXT("component"), ESearchCase::IgnoreCase))
			{
				FString ComponentName;
				(*BindingObj)->TryGetStringField(TEXT("preferred_component_name"), ComponentName);
				if (ComponentName.IsEmpty())
				{
					UeAgentSequenceFolderOps::AddWarning(Warnings, TEXT("binding_recreate_missing_component"), FString::Printf(TEXT("Skipped component binding recreation for '%s' because component identity is missing."), *DesiredGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
					return true;
				}

				FGuid ParentGuid;
				FString ParentGuidText;
				if ((*BindingObj)->TryGetStringField(TEXT("parent_binding_guid"), ParentGuidText) && !ParentGuidText.IsEmpty())
				{
					FGuid::Parse(ParentGuidText, ParentGuid);
				}
				if (ParentGuid.IsValid() && !MovieScene->FindBinding(ParentGuid))
				{
					const FString* ParentFolderName = BindingFolderByGuid.Find(ParentGuid);
					if (ParentFolderName)
					{
						const FString ParentBindingJsonPath = FPaths::Combine(FolderPath, TEXT("bindings"), *ParentFolderName, TEXT("binding.json"));
						bool bParentRecovered = false;
						if (!TryResolveBindingFromSpec(Sequence, MovieScene, BindingFolderByGuid, FolderPath, ParentGuid, ParentBindingJsonPath, ResolvingBindings, Warnings, bParentRecovered, OutError))
						{
							return false;
						}
					}
				}

				UObject* ParentObject = nullptr;
				if (ParentGuid.IsValid())
				{
					if (const FMovieSceneSpawnable* ParentSpawnable = MovieScene->FindSpawnable(ParentGuid))
					{
						ParentObject = const_cast<UObject*>(ParentSpawnable->GetObjectTemplate());
					}
					if (!ParentObject && GEditor)
					{
						UWorld* World = GEditor->GetEditorWorldContext().World();
						if (World)
						{
							TArray<UObject*, TInlineAllocator<1>> ParentObjects;
							Sequence->LocateBoundObjects(ParentGuid, UE::UniversalObjectLocator::FResolveParams(World), nullptr, ParentObjects);
							if (ParentObjects.Num() > 0)
							{
								ParentObject = ParentObjects[0];
							}
						}
					}
				}

				AActor* OwnerActor = Cast<AActor>(ParentObject);
				if (!OwnerActor)
				{
					FString PreferredOwnerActorId;
					if (!(*BindingObj)->TryGetStringField(TEXT("preferred_owner_actor_id"), PreferredOwnerActorId) || PreferredOwnerActorId.IsEmpty())
					{
						(*BindingObj)->TryGetStringField(TEXT("owner_actor_label"), PreferredOwnerActorId);
						if (PreferredOwnerActorId.IsEmpty())
						{
							(*BindingObj)->TryGetStringField(TEXT("owner_actor_name"), PreferredOwnerActorId);
						}
					}
					if (PreferredOwnerActorId.IsEmpty() || !GEditor)
					{
						UeAgentSequenceFolderOps::AddWarning(Warnings, TEXT("binding_recreate_component_owner_missing"), FString::Printf(TEXT("Skipped component binding recreation for '%s' because owner actor identity is missing."), *DesiredGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
						return true;
					}
					UWorld* World = GEditor->GetEditorWorldContext().World();
					if (!World)
					{
						OutError = TEXT("editor_world_not_found");
						return false;
					}
					OwnerActor = FindActorByNameOrLabelLocal(World, PreferredOwnerActorId);
				}

				if (!OwnerActor)
				{
					UeAgentSequenceFolderOps::AddWarning(Warnings, TEXT("binding_recreate_component_owner_not_found"), FString::Printf(TEXT("Skipped component binding recreation for '%s' because owner actor was not found."), *DesiredGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
					return true;
				}

				UActorComponent* Component = FindActorComponentByName(OwnerActor, ComponentName);
				if (!Component)
				{
					UeAgentSequenceFolderOps::AddWarning(Warnings, TEXT("binding_recreate_component_not_found"), FString::Printf(TEXT("Skipped component binding recreation for '%s' because component '%s' was not found."), *DesiredGuid.ToString(EGuidFormats::DigitsWithHyphensLower), *ComponentName));
					return true;
				}

				if (!AddPossessableBindingDirect(Sequence, MovieScene, DesiredGuid, BindingName, *Component, OwnerActor, ParentGuid, OutError))
				{
					return false;
				}
				bOutRecovered = true;
				return true;
			}

			UeAgentSequenceFolderOps::AddWarning(Warnings, TEXT("binding_recreate_possessable_kind_unsupported"), FString::Printf(TEXT("Skipped unsupported possessable recreation kind '%s' for '%s'."), *PrimaryKind, *DesiredGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
			return true;
		}

		if (BindingKind.Equals(TEXT("spawnable"), ESearchCase::IgnoreCase))
		{
			UObject* SourceObject = nullptr;
			UObject* CleanupObject = nullptr;
			ON_SCOPE_EXIT
			{
				if (AActor* TempActor = Cast<AActor>(CleanupObject))
				{
					TempActor->Destroy();
				}
				else if (CleanupObject)
				{
					CleanupObject->MarkAsGarbage();
				}
			};

			FString TemplatePath;
			(*BindingObj)->TryGetStringField(TEXT("spawnable_template_path"), TemplatePath);
			if (!TemplatePath.IsEmpty())
			{
				SourceObject = LoadObject<UObject>(nullptr, *TemplatePath);
			}

			if (!SourceObject)
			{
				FString SpawnableClassPath;
				if (!(*BindingObj)->TryGetStringField(TEXT("spawnable_object_class"), SpawnableClassPath) || SpawnableClassPath.IsEmpty())
				{
					UeAgentSequenceFolderOps::AddWarning(Warnings, TEXT("binding_recreate_spawnable_class_missing"), FString::Printf(TEXT("Skipped spawnable binding recreation for '%s' because class info is missing."), *DesiredGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
					return true;
				}
				if (!BuildSpawnableSourceObject(SpawnableClassPath, SourceObject, CleanupObject, OutError))
				{
					return false;
				}
			}

			if (!SourceObject)
			{
				UeAgentSequenceFolderOps::AddWarning(Warnings, TEXT("binding_recreate_spawnable_source_missing"), FString::Printf(TEXT("Skipped spawnable binding recreation for '%s' because source object could not be built."), *DesiredGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
				return true;
			}

			if (!AddSpawnableBindingDirect(Sequence, MovieScene, DesiredGuid, BindingName, *SourceObject, OutError))
			{
				return false;
			}
			bOutRecovered = true;
			return true;
		}

		UeAgentSequenceFolderOps::AddWarning(Warnings, TEXT("binding_recreate_unsupported"), FString::Printf(TEXT("Skipped unsupported binding recreation kind '%s' for '%s'."), *BindingKind, *DesiredGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
		return true;
	}

	static TSharedPtr<FJsonObject> ExportValidationJson()
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Checks;
		TSharedPtr<FJsonObject> Check = MakeShared<FJsonObject>();
		Check->SetStringField(TEXT("kind"), TEXT("sequence_asset_load_success"));
		Checks.Add(MakeShared<FJsonValueObject>(Check));
		Root->SetArrayField(TEXT("checks"), Checks);
		return Root;
	}

	static UMovieSceneTrack* FindTrackForSpec(UMovieScene* MovieScene, const FGuid& BindingGuid, const TSharedPtr<FJsonObject>& TrackObj)
	{
		if (!MovieScene || !TrackObj.IsValid())
		{
			return nullptr;
		}

		FString TrackType;
		TryGetString(TrackObj, TEXT("track_type"), TrackType);
		if (TrackType.Equals(TEXT("transform"), ESearchCase::IgnoreCase))
		{
			return MovieScene->FindTrack<UMovieScene3DTransformTrack>(BindingGuid);
		}
		if (TrackType.Equals(TEXT("spawn"), ESearchCase::IgnoreCase))
		{
			return MovieScene->FindTrack<UMovieSceneSpawnTrack>(BindingGuid);
		}
		if (TrackType.Equals(TEXT("skeletal_animation"), ESearchCase::IgnoreCase))
		{
			return MovieScene->FindTrack<UMovieSceneSkeletalAnimationTrack>(BindingGuid);
		}
		if (TrackType.Equals(TEXT("visibility"), ESearchCase::IgnoreCase))
		{
			FString PropertyName = TEXT("bHidden");
			FString PropertyPath = TEXT("bHidden");
			TryGetString(TrackObj, TEXT("property_name"), PropertyName);
			TryGetString(TrackObj, TEXT("property_path"), PropertyPath);
			return UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneVisibilityTrack>(MovieScene, BindingGuid, FName(*PropertyName), PropertyPath);
		}
		if (TrackType.Equals(TEXT("property"), ESearchCase::IgnoreCase))
		{
			FString PropertyType;
			FString PropertyName;
			FString PropertyPath;
			TryGetString(TrackObj, TEXT("property_type"), PropertyType);
			TryGetString(TrackObj, TEXT("property_name"), PropertyName);
			TryGetString(TrackObj, TEXT("property_path"), PropertyPath);
			const FName PropertyFName(*PropertyName);
			if (PropertyType.Equals(TEXT("float"), ESearchCase::IgnoreCase))
			{
				return UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneFloatTrack>(MovieScene, BindingGuid, PropertyFName, PropertyPath);
			}
			if (PropertyType.Equals(TEXT("double"), ESearchCase::IgnoreCase))
			{
				return UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneDoubleTrack>(MovieScene, BindingGuid, PropertyFName, PropertyPath);
			}
			if (PropertyType.Equals(TEXT("bool"), ESearchCase::IgnoreCase))
			{
				return UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneBoolTrack>(MovieScene, BindingGuid, PropertyFName, PropertyPath);
			}
			if (PropertyType.Equals(TEXT("integer"), ESearchCase::IgnoreCase))
			{
				return UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneIntegerTrack>(MovieScene, BindingGuid, PropertyFName, PropertyPath);
			}
			if (PropertyType.Equals(TEXT("color"), ESearchCase::IgnoreCase))
			{
				return UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneColorTrack>(MovieScene, BindingGuid, PropertyFName, PropertyPath);
			}
			if (PropertyType.Equals(TEXT("byte"), ESearchCase::IgnoreCase))
			{
				return UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneByteTrack>(MovieScene, BindingGuid, PropertyFName, PropertyPath);
			}
			if (PropertyType.Equals(TEXT("string"), ESearchCase::IgnoreCase))
			{
				return UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneStringTrack>(MovieScene, BindingGuid, PropertyFName, PropertyPath);
			}
			if (PropertyType.Equals(TEXT("rotator"), ESearchCase::IgnoreCase))
			{
				return UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneRotatorTrack>(MovieScene, BindingGuid, PropertyFName, PropertyPath);
			}
			if (PropertyType.Equals(TEXT("actor_reference"), ESearchCase::IgnoreCase))
			{
				return UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneActorReferenceTrack>(MovieScene, BindingGuid, PropertyFName, PropertyPath);
			}
			if (PropertyType.Equals(TEXT("object"), ESearchCase::IgnoreCase))
			{
				return UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneObjectPropertyTrack>(MovieScene, BindingGuid, PropertyFName, PropertyPath);
			}
			if (PropertyType.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
			{
				FString VectorPrecision = TEXT("double");
				TryGetString(TrackObj, TEXT("vector_precision"), VectorPrecision);
				if (VectorPrecision.Equals(TEXT("float"), ESearchCase::IgnoreCase))
				{
					return UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneFloatVectorTrack>(MovieScene, BindingGuid, PropertyFName, PropertyPath);
				}
				return UeAgentSequenceOps::FindPropertyTrackByNameAndPath<UMovieSceneDoubleVectorTrack>(MovieScene, BindingGuid, PropertyFName, PropertyPath);
			}
		}
		return nullptr;
	}

	static void ClearTrackSections(UMovieSceneTrack* Track)
	{
		if (!Track)
		{
			return;
		}
		TArray<UMovieSceneSection*> Sections = Track->GetAllSections();
		for (UMovieSceneSection* Section : Sections)
		{
			if (Section)
			{
				Track->RemoveSection(*Section);
			}
		}
	}

	static void AddWarning(TArray<TSharedPtr<FJsonValue>>& Warnings, const FString& Code, const FString& Message)
	{
		TSharedPtr<FJsonObject> WarningObj = MakeShared<FJsonObject>();
		WarningObj->SetStringField(TEXT("code"), Code);
		WarningObj->SetStringField(TEXT("message"), Message);
		Warnings.Add(MakeShared<FJsonValueObject>(WarningObj));
	}
}

bool FUeAgentHttpServer::CmdSequenceExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	ULevelSequence* Sequence = UeAgentSequenceOps::LoadLevelSequence(AssetPathInput);
	if (!Sequence || !Sequence->GetMovieScene())
	{
		OutError = TEXT("level_sequence_not_found");
		return false;
	}

	const FString AssetPath = UeAgentSequenceOps::NormalizeAssetPath(AssetPathInput);
	FString FolderPath;
	if (!UeAgentSequenceFolderOps::ResolveFolderForAsset(AssetPath, FolderPath, OutError))
	{
		return false;
	}

	bool bCleanOutputDir = true;
	JsonTryGetBool(Ctx.Params, TEXT("clean_output_dir"), bCleanOutputDir);
	bool bIncludeValidation = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_validation"), bIncludeValidation);

	if (bCleanOutputDir && IFileManager::Get().DirectoryExists(*FolderPath))
	{
		IFileManager::Get().DeleteDirectory(*FolderPath, false, true);
	}

	const FString SettingsDir = FPaths::Combine(FolderPath, TEXT("settings"));
	const FString BindingsDir = FPaths::Combine(FolderPath, TEXT("bindings"));
	const FString OutlinerDir = FPaths::Combine(FolderPath, TEXT("outliner"));
	const FString MasterTracksDir = FPaths::Combine(FolderPath, TEXT("master_tracks"));
	const FString ValidationDir = FPaths::Combine(FolderPath, TEXT("validation"));
	IFileManager::Get().MakeDirectory(*SettingsDir, true);
	IFileManager::Get().MakeDirectory(*BindingsDir, true);
	IFileManager::Get().MakeDirectory(*OutlinerDir, true);
	IFileManager::Get().MakeDirectory(*MasterTracksDir, true);
	if (bIncludeValidation)
	{
		IFileManager::Get().MakeDirectory(*ValidationDir, true);
	}

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	int32 FileCount = 0;
	auto SaveRequiredJson = [&FileCount, &OutError](const FString& FilePath, const TSharedPtr<FJsonObject>& JsonObj) -> bool
	{
		FString SavedPath;
		FString SaveError;
		if (!UeAgentSequenceOps::SaveJsonFile(FilePath, JsonObj, SavedPath, SaveError))
		{
			OutError = SaveError.IsEmpty() ? TEXT("save_export_file_failed") : SaveError;
			return false;
		}
		++FileCount;
		return true;
	};

	TSharedPtr<FJsonObject> AssetJson = MakeShared<FJsonObject>();
	AssetJson->SetNumberField(TEXT("format_version"), 1);
	AssetJson->SetStringField(TEXT("asset_kind"), TEXT("level_sequence"));
	AssetJson->SetStringField(TEXT("asset_path"), AssetPath);
	AssetJson->SetStringField(TEXT("asset_name"), FPackageName::GetLongPackageAssetName(AssetPath));
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetJson)) return false;

	TSharedPtr<FJsonObject> SettingsJson = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> PlaybackObj = MakeShared<FJsonObject>();
	const TRange<FFrameNumber> PlaybackRange = MovieScene->GetPlaybackRange();
	PlaybackObj->SetNumberField(TEXT("start_seconds"), MovieScene->GetTickResolution().AsSeconds(FFrameTime(PlaybackRange.GetLowerBoundValue())));
	PlaybackObj->SetNumberField(TEXT("duration_seconds"), MovieScene->GetTickResolution().AsSeconds(FFrameTime(PlaybackRange.GetUpperBoundValue() - PlaybackRange.GetLowerBoundValue())));
	SettingsJson->SetObjectField(TEXT("playback"), PlaybackObj);
	TSharedPtr<FJsonObject> DisplayRateObj = MakeShared<FJsonObject>();
	DisplayRateObj->SetNumberField(TEXT("numerator"), MovieScene->GetDisplayRate().Numerator);
	DisplayRateObj->SetNumberField(TEXT("denominator"), MovieScene->GetDisplayRate().Denominator);
	SettingsJson->SetObjectField(TEXT("display_rate"), DisplayRateObj);
	if (!SaveRequiredJson(FPaths::Combine(SettingsDir, TEXT("sequence.json")), SettingsJson)) return false;

	TArray<TSharedPtr<FJsonValue>> BindingIndexEntries;
	TMap<const UMovieSceneTrack*, TSharedPtr<FJsonObject>> BindingTrackRefsByTrack;
	const TArray<FMovieSceneBinding>& Bindings = MovieScene->GetBindings();
	for (const FMovieSceneBinding& Binding : Bindings)
	{
		const FString BindingFolderName = UeAgentSequenceFolderOps::MakeBindingFolderName(Binding);
		const FString BindingFolderPath = FPaths::Combine(BindingsDir, BindingFolderName);
		const FString TracksDir = FPaths::Combine(BindingFolderPath, TEXT("tracks"));
		IFileManager::Get().MakeDirectory(*TracksDir, true);

		TSharedPtr<FJsonObject> BindingIndexObj = MakeShared<FJsonObject>();
		BindingIndexObj->SetStringField(TEXT("binding_guid"), Binding.GetObjectGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
		BindingIndexObj->SetStringField(TEXT("binding_id"), Binding.GetObjectGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
		BindingIndexObj->SetStringField(TEXT("binding_name"), Binding.GetName());
		BindingIndexObj->SetStringField(TEXT("folder_name"), BindingFolderName);
		BindingIndexEntries.Add(MakeShared<FJsonValueObject>(BindingIndexObj));

		if (!SaveRequiredJson(FPaths::Combine(BindingFolderPath, TEXT("binding.json")), UeAgentSequenceFolderOps::BuildBindingJson(Sequence, MovieScene, Binding))) return false;

		for (int32 TrackIndex = 0; TrackIndex < Binding.GetTracks().Num(); ++TrackIndex)
		{
			UMovieSceneTrack* Track = Binding.GetTracks()[TrackIndex];
			if (!Track)
			{
				continue;
			}
			TSharedPtr<FJsonObject> TrackJson = UeAgentSequenceFolderOps::BuildTrackJson(MovieScene, Track);
			if (!TrackJson.IsValid())
			{
				continue;
			}
			const FString TrackFileName = UeAgentSequenceFolderOps::MakeTrackFileName(Track, TrackIndex);
			if (TSharedPtr<FJsonObject> TrackRefObj = UeAgentSequenceFolderOps::BuildBindingTrackRefJson(Binding.GetObjectGuid(), TrackFileName, TrackJson))
			{
				BindingTrackRefsByTrack.Add(Track, TrackRefObj);
			}
			if (!SaveRequiredJson(FPaths::Combine(TracksDir, TrackFileName), TrackJson)) return false;
		}
	}

	TSharedPtr<FJsonObject> BindingsIndexJson = MakeShared<FJsonObject>();
	BindingsIndexJson->SetArrayField(TEXT("bindings"), BindingIndexEntries);
	if (!SaveRequiredJson(FPaths::Combine(BindingsDir, TEXT("index.json")), BindingsIndexJson)) return false;

	if (!SaveRequiredJson(FPaths::Combine(OutlinerDir, TEXT("folders.json")), UeAgentSequenceFolderOps::BuildOutlinerFoldersJson(MovieScene, BindingTrackRefsByTrack))) return false;

	if (!SaveRequiredJson(FPaths::Combine(MasterTracksDir, TEXT("index.json")), UeAgentSequenceFolderOps::BuildMasterTracksIndexJson(MovieScene))) return false;

	if (bIncludeValidation)
	{
		if (!SaveRequiredJson(FPaths::Combine(ValidationDir, TEXT("checks.json")), UeAgentSequenceFolderOps::ExportValidationJson())) return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("root_path"), UeAgentSequenceFolderOps::GetFolderRootAbsolute());
	OutData->SetNumberField(TEXT("file_count"), FileCount);
	return true;
}

bool FUeAgentHttpServer::CmdSequenceApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	const FString AssetPath = UeAgentSequenceOps::NormalizeAssetPath(AssetPathInput);
	FString FolderPath;
	if (!UeAgentSequenceFolderOps::ResolveFolderForAsset(AssetPath, FolderPath, OutError))
	{
		return false;
	}

	TSharedPtr<FJsonObject> AssetJson;
	FString ResolvedAssetJsonPath;
	if (!UeAgentSequenceOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetJson, ResolvedAssetJsonPath, OutError))
	{
		return false;
	}

	bool bCreateIfMissing = true;
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);

	ULevelSequence* Sequence = UeAgentSequenceOps::LoadLevelSequence(AssetPath);
	if (!Sequence)
	{
		if (!bCreateIfMissing)
		{
			OutError = TEXT("level_sequence_not_found");
			return false;
		}

		FUeAgentRequestContext CreateCtx;
		CreateCtx.RequestId = Ctx.RequestId + TEXT("_create");
		CreateCtx.Command = TEXT("sequence_create_level_sequence");
		CreateCtx.Params = MakeShared<FJsonObject>();
		CreateCtx.Params->SetStringField(TEXT("asset_path"), AssetPath);
		CreateCtx.Params->SetNumberField(TEXT("start_seconds"), 0.0);
		CreateCtx.Params->SetNumberField(TEXT("duration_seconds"), 5.0);
		CreateCtx.Params->SetBoolField(TEXT("open_editor"), false);
		CreateCtx.Params->SetBoolField(TEXT("save_after_create"), false);
		TSharedPtr<FJsonObject> CreateData;
		if (!CmdSequenceCreateLevelSequence(CreateCtx, CreateData, OutError))
		{
			return false;
		}
		Sequence = UeAgentSequenceOps::LoadLevelSequence(AssetPath);
	}
	if (!Sequence || !Sequence->GetMovieScene())
	{
		OutError = TEXT("level_sequence_not_found");
		return false;
	}

	auto InvokeSequenceSubCommand = [this](const FString& Command, const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& CommandData, FString& CommandError) -> bool
	{
		FUeAgentRequestContext SubCtx;
		SubCtx.RequestId = TEXT("sequence_folder_apply");
		SubCtx.Command = Command;
		SubCtx.Params = Params;
		return ExecuteSequenceCommand(Command, SubCtx, CommandData, CommandError);
	};

	TArray<TSharedPtr<FJsonValue>> Warnings;
	int32 BindingsApplied = 0;
	int32 TracksApplied = 0;
	int32 KeysApplied = 0;
	int32 FoldersApplied = 0;

	TSharedPtr<FJsonObject> SettingsJson;
	FString SettingsPath = FPaths::Combine(FolderPath, TEXT("settings"), TEXT("sequence.json"));
	FString ResolvedSettingsPath;
	FString SettingsError;
	if (!UeAgentSequenceOps::LoadOptionalJsonFile(SettingsPath, SettingsJson, ResolvedSettingsPath, SettingsError))
	{
		OutError = SettingsError;
		return false;
	}
	if (SettingsJson.IsValid())
	{
		TSharedPtr<FJsonObject> PlaybackObj;
		if (const TSharedPtr<FJsonObject>* PlaybackPtr = nullptr; SettingsJson->TryGetObjectField(TEXT("playback"), PlaybackPtr) && PlaybackPtr && PlaybackPtr->IsValid())
		{
			PlaybackObj = *PlaybackPtr;
			double StartSeconds = 0.0;
			double DurationSeconds = 0.0;
			if (PlaybackObj->TryGetNumberField(TEXT("start_seconds"), StartSeconds) && PlaybackObj->TryGetNumberField(TEXT("duration_seconds"), DurationSeconds))
			{
				TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
				Params->SetStringField(TEXT("asset_path"), AssetPath);
				Params->SetNumberField(TEXT("start_seconds"), StartSeconds);
				Params->SetNumberField(TEXT("duration_seconds"), DurationSeconds);
				Params->SetBoolField(TEXT("save_after_set"), false);
				TSharedPtr<FJsonObject> Data;
				if (!InvokeSequenceSubCommand(TEXT("sequence_set_level_sequence_playback_range"), Params, Data, OutError))
				{
					return false;
				}
			}
		}
		const TSharedPtr<FJsonObject>* DisplayRatePtr = nullptr;
		if (SettingsJson->TryGetObjectField(TEXT("display_rate"), DisplayRatePtr) && DisplayRatePtr && (*DisplayRatePtr).IsValid())
		{
			double Num = 0.0;
			double Den = 1.0;
			if ((*DisplayRatePtr)->TryGetNumberField(TEXT("numerator"), Num) && (*DisplayRatePtr)->TryGetNumberField(TEXT("denominator"), Den))
			{
				TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
				Params->SetStringField(TEXT("asset_path"), AssetPath);
				Params->SetNumberField(TEXT("display_rate_num"), Num);
				Params->SetNumberField(TEXT("display_rate_den"), Den);
				Params->SetBoolField(TEXT("save_after_set"), false);
				TSharedPtr<FJsonObject> Data;
				if (!InvokeSequenceSubCommand(TEXT("sequence_set_level_sequence_display_rate"), Params, Data, OutError))
				{
					return false;
				}
			}
		}
	}

	TSharedPtr<FJsonObject> BindingsIndexJson;
	FString BindingsIndexResolved;
	if (!UeAgentSequenceOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("bindings"), TEXT("index.json")), BindingsIndexJson, BindingsIndexResolved, OutError))
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* BindingEntries = nullptr;
	if (!BindingsIndexJson->TryGetArrayField(TEXT("bindings"), BindingEntries) || !BindingEntries)
	{
		OutError = TEXT("missing_bindings");
		return false;
	}

	TMap<FGuid, FString> BindingFolderByGuid;
	for (const TSharedPtr<FJsonValue>& BindingEntryValue : *BindingEntries)
	{
		const TSharedPtr<FJsonObject>* BindingEntryObj = nullptr;
		if (!BindingEntryValue.IsValid() || !BindingEntryValue->TryGetObject(BindingEntryObj) || !BindingEntryObj || !BindingEntryObj->IsValid())
		{
			continue;
		}

		FString BindingGuidText;
		FString BindingFolderName;
		if (!(*BindingEntryObj)->TryGetStringField(TEXT("binding_guid"), BindingGuidText) || BindingGuidText.IsEmpty())
		{
			continue;
		}
		if (!(*BindingEntryObj)->TryGetStringField(TEXT("folder_name"), BindingFolderName) || BindingFolderName.IsEmpty())
		{
			continue;
		}

		FGuid BindingGuid;
		if (FGuid::Parse(BindingGuidText, BindingGuid))
		{
			BindingFolderByGuid.Add(BindingGuid, BindingFolderName);
		}
	}

	TSet<FGuid> ResolvingBindings;

	for (const TSharedPtr<FJsonValue>& BindingEntryValue : *BindingEntries)
	{
		const TSharedPtr<FJsonObject>* BindingEntryObj = nullptr;
		if (!BindingEntryValue.IsValid() || !BindingEntryValue->TryGetObject(BindingEntryObj) || !BindingEntryObj || !BindingEntryObj->IsValid())
		{
			continue;
		}

		FString BindingGuidText;
		FString BindingFolderName;
		if (!(*BindingEntryObj)->TryGetStringField(TEXT("binding_guid"), BindingGuidText) || BindingGuidText.IsEmpty())
		{
			continue;
		}
		if (!(*BindingEntryObj)->TryGetStringField(TEXT("folder_name"), BindingFolderName) || BindingFolderName.IsEmpty())
		{
			continue;
		}

		FGuid BindingGuid;
		if (!FGuid::Parse(BindingGuidText, BindingGuid))
		{
			UeAgentSequenceFolderOps::AddWarning(Warnings, TEXT("binding_guid_invalid"), FString::Printf(TEXT("Skipped invalid binding guid '%s'."), *BindingGuidText));
			continue;
		}

		Sequence = UeAgentSequenceOps::LoadLevelSequence(AssetPath);
		UMovieScene* MovieScene = Sequence ? Sequence->GetMovieScene() : nullptr;
		if (!Sequence || !MovieScene || !MovieScene->FindBinding(BindingGuid))
		{
			const FString BindingJsonPath = FPaths::Combine(FolderPath, TEXT("bindings"), BindingFolderName, TEXT("binding.json"));
			bool bRecoveredBinding = false;
			if (!UeAgentSequenceFolderOps::TryResolveBindingFromSpec(Sequence, MovieScene, BindingFolderByGuid, FolderPath, BindingGuid, BindingJsonPath, ResolvingBindings, Warnings, bRecoveredBinding, OutError))
			{
				return false;
			}
			if (!bRecoveredBinding)
			{
				UeAgentSequenceFolderOps::AddWarning(Warnings, TEXT("binding_not_found"), FString::Printf(TEXT("Skipped missing binding '%s'."), *BindingGuidText));
				continue;
			}

			Sequence = UeAgentSequenceOps::LoadLevelSequence(AssetPath);
			MovieScene = Sequence ? Sequence->GetMovieScene() : nullptr;
			if (!Sequence || !MovieScene || !MovieScene->FindBinding(BindingGuid))
			{
				UeAgentSequenceFolderOps::AddWarning(Warnings, TEXT("binding_recreate_verify_failed"), FString::Printf(TEXT("Skipped recreated binding '%s' because it could not be reloaded."), *BindingGuidText));
				continue;
			}
		}

		const FString TracksDir = FPaths::Combine(FolderPath, TEXT("bindings"), BindingFolderName, TEXT("tracks"));
		TArray<FString> TrackFiles;
		IFileManager::Get().FindFiles(TrackFiles, *(FPaths::Combine(TracksDir, TEXT("*.json"))), true, false);
		TrackFiles.Sort();

		for (const FString& TrackFile : TrackFiles)
		{
			TSharedPtr<FJsonObject> TrackJson;
			FString ResolvedTrackPath;
			FString TrackError;
			if (!UeAgentSequenceOps::LoadJsonFile(FPaths::Combine(TracksDir, TrackFile), TrackJson, ResolvedTrackPath, TrackError))
			{
				OutError = TrackError;
				return false;
			}
			if (!TrackJson.IsValid())
			{
				continue;
			}

			Sequence = UeAgentSequenceOps::LoadLevelSequence(AssetPath);
			MovieScene = Sequence ? Sequence->GetMovieScene() : nullptr;
			UMovieSceneTrack* ExistingTrack = UeAgentSequenceFolderOps::FindTrackForSpec(MovieScene, BindingGuid, TrackJson);
			if (ExistingTrack)
			{
				UeAgentSequenceFolderOps::ClearTrackSections(ExistingTrack);
			}

			FString TrackType;
			UeAgentSequenceFolderOps::TryGetString(TrackJson, TEXT("track_type"), TrackType);

			if (TrackType.Equals(TEXT("transform"), ESearchCase::IgnoreCase))
			{
				TSharedPtr<FJsonObject> AddTrackParams = MakeShared<FJsonObject>();
				AddTrackParams->SetStringField(TEXT("asset_path"), AssetPath);
				AddTrackParams->SetStringField(TEXT("binding_guid"), BindingGuidText);
				AddTrackParams->SetBoolField(TEXT("save_after_set"), false);
				TSharedPtr<FJsonObject> AddTrackData;
				if (!InvokeSequenceSubCommand(TEXT("sequence_add_transform_track"), AddTrackParams, AddTrackData, OutError))
				{
					return false;
				}
				++TracksApplied;

				const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
				if (!TrackJson->TryGetArrayField(TEXT("sections"), Sections) || !Sections)
				{
					continue;
				}
				for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
				{
					const TSharedPtr<FJsonObject>* SectionObj = nullptr;
					if (!SectionValue.IsValid() || !SectionValue->TryGetObject(SectionObj) || !SectionObj || !SectionObj->IsValid())
					{
						continue;
					}
					const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
					if (!(*SectionObj)->TryGetArrayField(TEXT("keys"), Keys) || !Keys)
					{
						continue;
					}
					for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
					{
						const TSharedPtr<FJsonObject>* KeyObj = nullptr;
						if (!KeyValue.IsValid() || !KeyValue->TryGetObject(KeyObj) || !KeyObj || !KeyObj->IsValid())
						{
							continue;
						}
						double TimeSeconds = 0.0;
						if (!(*KeyObj)->TryGetNumberField(TEXT("time_seconds"), TimeSeconds))
						{
							continue;
						}
						TSharedPtr<FJsonObject> KeyParams = MakeShared<FJsonObject>();
						KeyParams->SetStringField(TEXT("asset_path"), AssetPath);
						KeyParams->SetStringField(TEXT("binding_guid"), BindingGuidText);
						KeyParams->SetNumberField(TEXT("time_seconds"), TimeSeconds);
						const TSharedPtr<FJsonObject>* LocationObj = nullptr;
						if ((*KeyObj)->TryGetObjectField(TEXT("location"), LocationObj) && LocationObj && (*LocationObj).IsValid())
						{
							KeyParams->SetObjectField(TEXT("location"), *LocationObj);
						}
						const TSharedPtr<FJsonObject>* RotationObj = nullptr;
						if ((*KeyObj)->TryGetObjectField(TEXT("rotation"), RotationObj) && RotationObj && (*RotationObj).IsValid())
						{
							KeyParams->SetObjectField(TEXT("rotation"), *RotationObj);
						}
						const TSharedPtr<FJsonObject>* ScaleObj = nullptr;
						if ((*KeyObj)->TryGetObjectField(TEXT("scale"), ScaleObj) && ScaleObj && (*ScaleObj).IsValid())
						{
							KeyParams->SetObjectField(TEXT("scale"), *ScaleObj);
						}
						KeyParams->SetBoolField(TEXT("save_after_set"), false);
						TSharedPtr<FJsonObject> KeyData;
						if (!InvokeSequenceSubCommand(TEXT("sequence_add_transform_key"), KeyParams, KeyData, OutError))
						{
							return false;
						}
						++KeysApplied;
					}
				}
				++BindingsApplied;
				continue;
			}

			if (TrackType.Equals(TEXT("spawn"), ESearchCase::IgnoreCase))
			{
				UMovieSceneSpawnTrack* SpawnTrack = MovieScene ? MovieScene->FindTrack<UMovieSceneSpawnTrack>(BindingGuid) : nullptr;
				if (!SpawnTrack && MovieScene)
				{
					SpawnTrack = MovieScene->AddTrack<UMovieSceneSpawnTrack>(BindingGuid);
					if (SpawnTrack)
					{
						SpawnTrack->SetObjectId(BindingGuid);
					}
				}
				if (!SpawnTrack)
				{
					OutError = TEXT("create_spawn_track_failed");
					return false;
				}
				++TracksApplied;

				const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
				if (!TrackJson->TryGetArrayField(TEXT("sections"), Sections) || !Sections)
				{
					continue;
				}
				for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
				{
					const TSharedPtr<FJsonObject>* SectionObj = nullptr;
					if (!SectionValue.IsValid() || !SectionValue->TryGetObject(SectionObj) || !SectionObj || !SectionObj->IsValid())
					{
						continue;
					}

					UMovieSceneSpawnSection* SpawnSection = SpawnTrack->GetAllSections().Num() > 0 ? Cast<UMovieSceneSpawnSection>(SpawnTrack->GetAllSections()[0]) : nullptr;
					if (!SpawnSection)
					{
						SpawnSection = Cast<UMovieSceneSpawnSection>(SpawnTrack->CreateNewSection());
						if (!SpawnSection)
						{
							OutError = TEXT("create_spawn_section_failed");
							return false;
						}
						SpawnSection->SetRange(TRange<FFrameNumber>::All());
						SpawnTrack->AddSection(*SpawnSection);
					}
					else
					{
						SpawnSection->SetRange(TRange<FFrameNumber>::All());
					}

					const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
					if (!(*SectionObj)->TryGetArrayField(TEXT("keys"), Keys) || !Keys)
					{
						continue;
					}

					FMovieSceneBoolChannel& Channel = SpawnSection->GetChannel();
					Channel.GetData().Reset();
					for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
					{
						const TSharedPtr<FJsonObject>* KeyObj = nullptr;
						if (!KeyValue.IsValid() || !KeyValue->TryGetObject(KeyObj) || !KeyObj || !KeyObj->IsValid())
						{
							continue;
						}
						double TimeSeconds = 0.0;
						bool BoolValue = false;
						if (!(*KeyObj)->TryGetNumberField(TEXT("time_seconds"), TimeSeconds) || !(*KeyObj)->TryGetBoolField(TEXT("value"), BoolValue))
						{
							continue;
						}
						const FFrameNumber KeyFrame = MovieScene->GetTickResolution().AsFrameTime(TimeSeconds).RoundToFrame();
						Channel.GetData().UpdateOrAddKey(KeyFrame, BoolValue);
						++KeysApplied;
					}
				}
				++BindingsApplied;
				continue;
			}

			if (TrackType.Equals(TEXT("skeletal_animation"), ESearchCase::IgnoreCase))
			{
				TSharedPtr<FJsonObject> AddTrackParams = MakeShared<FJsonObject>();
				AddTrackParams->SetStringField(TEXT("asset_path"), AssetPath);
				AddTrackParams->SetStringField(TEXT("binding_guid"), BindingGuidText);
				AddTrackParams->SetBoolField(TEXT("save_after_set"), false);
				TSharedPtr<FJsonObject> AddTrackData;
				if (!InvokeSequenceSubCommand(TEXT("sequence_add_skeletal_animation_track"), AddTrackParams, AddTrackData, OutError))
				{
					return false;
				}
				++TracksApplied;

				const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
				if (!TrackJson->TryGetArrayField(TEXT("sections"), Sections) || !Sections)
				{
					continue;
				}
				for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
				{
					const TSharedPtr<FJsonObject>* SectionObj = nullptr;
					if (!SectionValue.IsValid() || !SectionValue->TryGetObject(SectionObj) || !SectionObj || !SectionObj->IsValid())
					{
						continue;
					}
					TSharedPtr<FJsonObject> AddSectionParams = MakeShared<FJsonObject>();
					AddSectionParams->SetStringField(TEXT("asset_path"), AssetPath);
					AddSectionParams->SetStringField(TEXT("binding_guid"), BindingGuidText);
					FString AnimationAsset;
					if (!(*SectionObj)->TryGetStringField(TEXT("animation_asset"), AnimationAsset) || AnimationAsset.IsEmpty())
					{
						continue;
					}
					AddSectionParams->SetStringField(TEXT("animation_asset"), AnimationAsset);
					double NumberValue = 0.0;
					if ((*SectionObj)->TryGetNumberField(TEXT("start_seconds"), NumberValue)) AddSectionParams->SetNumberField(TEXT("start_seconds"), NumberValue);
					if ((*SectionObj)->TryGetNumberField(TEXT("end_seconds"), NumberValue)) AddSectionParams->SetNumberField(TEXT("duration_seconds"), NumberValue - ((*SectionObj)->GetNumberField(TEXT("start_seconds"))));
					if ((*SectionObj)->TryGetNumberField(TEXT("play_rate"), NumberValue)) AddSectionParams->SetNumberField(TEXT("play_rate"), NumberValue);
					if ((*SectionObj)->TryGetStringField(TEXT("slot_name"), AnimationAsset) && !AnimationAsset.IsEmpty()) AddSectionParams->SetStringField(TEXT("slot_name"), AnimationAsset);
					if ((*SectionObj)->TryGetNumberField(TEXT("row_index"), NumberValue)) AddSectionParams->SetNumberField(TEXT("row_index"), NumberValue);
					bool BoolValue = false;
					if ((*SectionObj)->TryGetBoolField(TEXT("skip_anim_notifiers"), BoolValue)) AddSectionParams->SetBoolField(TEXT("skip_anim_notifiers"), BoolValue);
					if ((*SectionObj)->TryGetBoolField(TEXT("force_custom_mode"), BoolValue)) AddSectionParams->SetBoolField(TEXT("force_custom_mode"), BoolValue);
					AddSectionParams->SetBoolField(TEXT("save_after_set"), false);
					TSharedPtr<FJsonObject> AddSectionData;
					if (!InvokeSequenceSubCommand(TEXT("sequence_add_skeletal_animation_section"), AddSectionParams, AddSectionData, OutError))
					{
						return false;
					}
					++KeysApplied;
				}
				++BindingsApplied;
				continue;
			}

			if (TrackType.Equals(TEXT("property"), ESearchCase::IgnoreCase) || TrackType.Equals(TEXT("visibility"), ESearchCase::IgnoreCase))
			{
				FString PropertyType = TrackType.Equals(TEXT("visibility"), ESearchCase::IgnoreCase) ? TEXT("visibility") : TEXT("");
				FString PropertyName;
				FString PropertyPath;
				UeAgentSequenceFolderOps::TryGetString(TrackJson, TEXT("property_type"), PropertyType);
				UeAgentSequenceFolderOps::TryGetString(TrackJson, TEXT("property_name"), PropertyName);
				UeAgentSequenceFolderOps::TryGetString(TrackJson, TEXT("property_path"), PropertyPath);
				const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
				if (!TrackJson->TryGetArrayField(TEXT("sections"), Sections) || !Sections || Sections->Num() == 0)
				{
					continue;
				}
				if (Sections->Num() > 1)
				{
					UeAgentSequenceFolderOps::AddWarning(Warnings, TEXT("multi_section_flattened"), FString::Printf(TEXT("Flattened multi-section track '%s' on binding '%s'."), *PropertyName, *BindingGuidText));
				}

				if (PropertyType.Equals(TEXT("visibility"), ESearchCase::IgnoreCase))
				{
					TSharedPtr<FJsonObject> AddTrackParams = MakeShared<FJsonObject>();
					AddTrackParams->SetStringField(TEXT("asset_path"), AssetPath);
					AddTrackParams->SetStringField(TEXT("binding_guid"), BindingGuidText);
					AddTrackParams->SetStringField(TEXT("property_name"), PropertyName.IsEmpty() ? TEXT("bHidden") : PropertyName);
					AddTrackParams->SetStringField(TEXT("property_path"), PropertyPath.IsEmpty() ? TEXT("bHidden") : PropertyPath);
					AddTrackParams->SetBoolField(TEXT("save_after_set"), false);
					TSharedPtr<FJsonObject> AddTrackData;
					if (!InvokeSequenceSubCommand(TEXT("sequence_add_visibility_track"), AddTrackParams, AddTrackData, OutError))
					{
						return false;
					}
					++TracksApplied;

					const TSharedPtr<FJsonObject>* SectionObj = nullptr;
					if ((*Sections)[0]->TryGetObject(SectionObj) && SectionObj && (*SectionObj).IsValid())
					{
						const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
						if ((*SectionObj)->TryGetArrayField(TEXT("keys"), Keys) && Keys)
						{
							for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
							{
								const TSharedPtr<FJsonObject>* KeyObj = nullptr;
								if (!KeyValue.IsValid() || !KeyValue->TryGetObject(KeyObj) || !KeyObj || !KeyObj->IsValid())
								{
									continue;
								}
								double TimeSeconds = 0.0;
								bool BoolValue = false;
								if (!(*KeyObj)->TryGetNumberField(TEXT("time_seconds"), TimeSeconds) || !(*KeyObj)->TryGetBoolField(TEXT("value"), BoolValue))
								{
									continue;
								}
								TSharedPtr<FJsonObject> KeyParams = MakeShared<FJsonObject>();
								KeyParams->SetStringField(TEXT("asset_path"), AssetPath);
								KeyParams->SetStringField(TEXT("binding_guid"), BindingGuidText);
								KeyParams->SetStringField(TEXT("property_name"), PropertyName.IsEmpty() ? TEXT("bHidden") : PropertyName);
								KeyParams->SetStringField(TEXT("property_path"), PropertyPath.IsEmpty() ? TEXT("bHidden") : PropertyPath);
								KeyParams->SetNumberField(TEXT("time_seconds"), TimeSeconds);
								KeyParams->SetBoolField(TEXT("value"), BoolValue);
								KeyParams->SetBoolField(TEXT("save_after_set"), false);
								TSharedPtr<FJsonObject> KeyData;
								if (!InvokeSequenceSubCommand(TEXT("sequence_add_visibility_key"), KeyParams, KeyData, OutError))
								{
									return false;
								}
								++KeysApplied;
							}
						}
					}
					++BindingsApplied;
					continue;
				}

				if (!(PropertyType.Equals(TEXT("float"), ESearchCase::IgnoreCase)
					|| PropertyType.Equals(TEXT("double"), ESearchCase::IgnoreCase)
					|| PropertyType.Equals(TEXT("bool"), ESearchCase::IgnoreCase)
					|| PropertyType.Equals(TEXT("integer"), ESearchCase::IgnoreCase)
					|| PropertyType.Equals(TEXT("color"), ESearchCase::IgnoreCase)
					|| PropertyType.Equals(TEXT("byte"), ESearchCase::IgnoreCase)
					|| PropertyType.Equals(TEXT("string"), ESearchCase::IgnoreCase)
					|| PropertyType.Equals(TEXT("vector"), ESearchCase::IgnoreCase)
					|| PropertyType.Equals(TEXT("rotator"), ESearchCase::IgnoreCase)
					|| PropertyType.Equals(TEXT("actor_reference"), ESearchCase::IgnoreCase)
					|| PropertyType.Equals(TEXT("object"), ESearchCase::IgnoreCase)))
				{
					UeAgentSequenceFolderOps::AddWarning(Warnings, TEXT("track_type_not_supported"), FString::Printf(TEXT("Skipped unsupported property track type '%s' on binding '%s'."), *PropertyType, *BindingGuidText));
					continue;
				}

				TSharedPtr<FJsonObject> AddTrackParams = MakeShared<FJsonObject>();
				AddTrackParams->SetStringField(TEXT("asset_path"), AssetPath);
				AddTrackParams->SetStringField(TEXT("binding_guid"), BindingGuidText);
				AddTrackParams->SetStringField(TEXT("property_type"), PropertyType);
				AddTrackParams->SetStringField(TEXT("property_name"), PropertyName);
				AddTrackParams->SetStringField(TEXT("property_path"), PropertyPath);
				FString FieldValue;
				if (TrackJson->TryGetStringField(TEXT("enum_path"), FieldValue) && !FieldValue.IsEmpty())
				{
					AddTrackParams->SetStringField(TEXT("enum_path"), FieldValue);
				}
				if (TrackJson->TryGetStringField(TEXT("property_class_path"), FieldValue) && !FieldValue.IsEmpty())
				{
					AddTrackParams->SetStringField(TEXT("property_class_path"), FieldValue);
				}
				if (TrackJson->TryGetStringField(TEXT("vector_precision"), FieldValue) && !FieldValue.IsEmpty())
				{
					AddTrackParams->SetStringField(TEXT("vector_precision"), FieldValue);
				}
				double NumberField = 0.0;
				if (TrackJson->TryGetNumberField(TEXT("channels_used"), NumberField))
				{
					AddTrackParams->SetNumberField(TEXT("channels_used"), NumberField);
				}
				AddTrackParams->SetBoolField(TEXT("save_after_set"), false);
				TSharedPtr<FJsonObject> AddTrackData;
				if (!InvokeSequenceSubCommand(TEXT("sequence_add_property_track"), AddTrackParams, AddTrackData, OutError))
				{
					return false;
				}
				++TracksApplied;

				const TSharedPtr<FJsonObject>* SectionObj = nullptr;
				if ((*Sections)[0]->TryGetObject(SectionObj) && SectionObj && (*SectionObj).IsValid())
				{
					const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
					if ((*SectionObj)->TryGetArrayField(TEXT("keys"), Keys) && Keys)
					{
						for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
						{
							const TSharedPtr<FJsonObject>* KeyObj = nullptr;
							if (!KeyValue.IsValid() || !KeyValue->TryGetObject(KeyObj) || !KeyObj || !KeyObj->IsValid())
							{
								continue;
							}
							double TimeSeconds = 0.0;
							if (!(*KeyObj)->TryGetNumberField(TEXT("time_seconds"), TimeSeconds))
							{
								continue;
							}
							TSharedPtr<FJsonObject> KeyParams = MakeShared<FJsonObject>();
							KeyParams->SetStringField(TEXT("asset_path"), AssetPath);
							KeyParams->SetStringField(TEXT("binding_guid"), BindingGuidText);
							KeyParams->SetStringField(TEXT("property_type"), PropertyType);
							KeyParams->SetStringField(TEXT("property_name"), PropertyName);
							KeyParams->SetStringField(TEXT("property_path"), PropertyPath);
							KeyParams->SetNumberField(TEXT("time_seconds"), TimeSeconds);
							KeyParams->SetBoolField(TEXT("save_after_set"), false);

							if (PropertyType.Equals(TEXT("bool"), ESearchCase::IgnoreCase))
							{
								bool BoolValue = false;
								if (!(*KeyObj)->TryGetBoolField(TEXT("value"), BoolValue))
								{
									continue;
								}
								KeyParams->SetBoolField(TEXT("value"), BoolValue);
							}
							else if (PropertyType.Equals(TEXT("color"), ESearchCase::IgnoreCase))
							{
								double NumberValue = 0.0;
								if (!(*KeyObj)->TryGetNumberField(TEXT("red"), NumberValue)) continue;
								KeyParams->SetNumberField(TEXT("red"), NumberValue);
								if (!(*KeyObj)->TryGetNumberField(TEXT("green"), NumberValue)) continue;
								KeyParams->SetNumberField(TEXT("green"), NumberValue);
								if (!(*KeyObj)->TryGetNumberField(TEXT("blue"), NumberValue)) continue;
								KeyParams->SetNumberField(TEXT("blue"), NumberValue);
								if ((*KeyObj)->TryGetNumberField(TEXT("alpha"), NumberValue)) KeyParams->SetNumberField(TEXT("alpha"), NumberValue);
							}
							else if (PropertyType.Equals(TEXT("byte"), ESearchCase::IgnoreCase))
							{
								FString EnumValueName;
								double NumberValue = 0.0;
								if ((*KeyObj)->TryGetStringField(TEXT("value_name"), EnumValueName) && !EnumValueName.IsEmpty())
								{
									KeyParams->SetStringField(TEXT("value_name"), EnumValueName);
								}
								else if ((*KeyObj)->TryGetNumberField(TEXT("value"), NumberValue))
								{
									KeyParams->SetNumberField(TEXT("value"), NumberValue);
								}
								else
								{
									continue;
								}
							}
							else if (PropertyType.Equals(TEXT("string"), ESearchCase::IgnoreCase))
							{
								FString StringValue;
								if (!(*KeyObj)->TryGetStringField(TEXT("value"), StringValue))
								{
									continue;
								}
								KeyParams->SetStringField(TEXT("value"), StringValue);
							}
							else if (PropertyType.Equals(TEXT("vector"), ESearchCase::IgnoreCase) || PropertyType.Equals(TEXT("rotator"), ESearchCase::IgnoreCase))
							{
								const TSharedPtr<FJsonObject>* ValueObj = nullptr;
								if (!(*KeyObj)->TryGetObjectField(TEXT("value"), ValueObj) || !ValueObj || !(*ValueObj).IsValid())
								{
									continue;
								}
								KeyParams->SetObjectField(TEXT("value"), *ValueObj);
							}
							else if (PropertyType.Equals(TEXT("actor_reference"), ESearchCase::IgnoreCase))
							{
								FString StringValue;
								if (!(*KeyObj)->TryGetStringField(TEXT("value_binding_guid"), StringValue) || StringValue.IsEmpty())
								{
									continue;
								}
								KeyParams->SetStringField(TEXT("value_binding_guid"), StringValue);
								if ((*KeyObj)->TryGetStringField(TEXT("component_name"), StringValue) && !StringValue.IsEmpty())
								{
									KeyParams->SetStringField(TEXT("component_name"), StringValue);
								}
								if ((*KeyObj)->TryGetStringField(TEXT("socket_name"), StringValue) && !StringValue.IsEmpty())
								{
									KeyParams->SetStringField(TEXT("socket_name"), StringValue);
								}
							}
							else if (PropertyType.Equals(TEXT("object"), ESearchCase::IgnoreCase))
							{
								FString StringValue;
								if (!(*KeyObj)->TryGetStringField(TEXT("value_path"), StringValue) || StringValue.IsEmpty())
								{
									continue;
								}
								KeyParams->SetStringField(TEXT("value_path"), StringValue);
								if (TrackJson->TryGetStringField(TEXT("property_class_path"), StringValue) && !StringValue.IsEmpty())
								{
									KeyParams->SetStringField(TEXT("property_class_path"), StringValue);
								}
							}
							else
							{
								double NumberValue = 0.0;
								if (!(*KeyObj)->TryGetNumberField(TEXT("value"), NumberValue))
								{
									continue;
								}
								KeyParams->SetNumberField(TEXT("value"), NumberValue);
							}

							TSharedPtr<FJsonObject> KeyData;
							if (!InvokeSequenceSubCommand(TEXT("sequence_add_property_key"), KeyParams, KeyData, OutError))
							{
								return false;
							}
							++KeysApplied;
						}
					}
				}
				++BindingsApplied;
				continue;
			}

			UeAgentSequenceFolderOps::AddWarning(Warnings, TEXT("track_type_not_supported"), FString::Printf(TEXT("Skipped unsupported track type '%s' on binding '%s'."), *TrackType, *BindingGuidText));
		}
	}

	const FString MasterTracksIndexPath = FPaths::Combine(FolderPath, TEXT("master_tracks"), TEXT("index.json"));
	if (FPaths::FileExists(MasterTracksIndexPath))
	{
		TSharedPtr<FJsonObject> MasterTracksJson;
		FString ResolvedMasterTracksPath;
		if (!UeAgentSequenceOps::LoadJsonFile(MasterTracksIndexPath, MasterTracksJson, ResolvedMasterTracksPath, OutError))
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* MasterTrackEntries = nullptr;
		if (MasterTracksJson.IsValid() && MasterTracksJson->TryGetArrayField(TEXT("tracks"), MasterTrackEntries) && MasterTrackEntries)
		{
			for (const TSharedPtr<FJsonValue>& MasterTrackValue : *MasterTrackEntries)
			{
				const TSharedPtr<FJsonObject>* MasterTrackObj = nullptr;
				if (!MasterTrackValue.IsValid() || !MasterTrackValue->TryGetObject(MasterTrackObj) || !MasterTrackObj || !(*MasterTrackObj).IsValid())
				{
					continue;
				}

				Sequence = UeAgentSequenceOps::LoadLevelSequence(AssetPath);
				UMovieScene* MovieScene = Sequence ? Sequence->GetMovieScene() : nullptr;
				if (!Sequence || !MovieScene)
				{
					OutError = TEXT("movie_scene_not_found");
					return false;
				}

				FString TrackType;
				UeAgentSequenceFolderOps::TryGetString(*MasterTrackObj, TEXT("track_type"), TrackType);

				if (TrackType.Equals(TEXT("camera_cut"), ESearchCase::IgnoreCase))
				{
					UMovieSceneCameraCutTrack* CameraCutTrack = Cast<UMovieSceneCameraCutTrack>(MovieScene->GetCameraCutTrack());
					if (!CameraCutTrack)
					{
						CameraCutTrack = Cast<UMovieSceneCameraCutTrack>(MovieScene->AddCameraCutTrack(UMovieSceneCameraCutTrack::StaticClass()));
					}
					if (!CameraCutTrack)
					{
						OutError = TEXT("create_camera_cut_track_failed");
						return false;
					}

					CameraCutTrack->Modify();
					CameraCutTrack->SetIsAutoManagingSections(false);
					UeAgentSequenceFolderOps::ClearTrackSections(CameraCutTrack);
					++TracksApplied;

					const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
					if (!(*MasterTrackObj)->TryGetArrayField(TEXT("sections"), Sections) || !Sections)
					{
						continue;
					}
					for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
					{
						const TSharedPtr<FJsonObject>* SectionObj = nullptr;
						if (!SectionValue.IsValid() || !SectionValue->TryGetObject(SectionObj) || !SectionObj || !(*SectionObj).IsValid())
						{
							continue;
						}

						FString CameraBindingGuidText;
						if (!(*SectionObj)->TryGetStringField(TEXT("camera_binding_guid"), CameraBindingGuidText) || CameraBindingGuidText.IsEmpty())
						{
							continue;
						}
						FGuid CameraBindingGuid;
						if (!FGuid::Parse(CameraBindingGuidText, CameraBindingGuid) || !MovieScene->FindBinding(CameraBindingGuid))
						{
							UeAgentSequenceFolderOps::AddWarning(Warnings, TEXT("camera_cut_binding_not_found"), FString::Printf(TEXT("Skipped camera cut section because binding '%s' was not found."), *CameraBindingGuidText));
							continue;
						}

						double StartSeconds = 0.0;
						double DurationSeconds = 0.0;
						if (!(*SectionObj)->TryGetNumberField(TEXT("start_seconds"), StartSeconds) || !(*SectionObj)->TryGetNumberField(TEXT("duration_seconds"), DurationSeconds))
						{
							continue;
						}

						UMovieSceneCameraCutSection* NewSection = Cast<UMovieSceneCameraCutSection>(CameraCutTrack->CreateNewSection());
						if (!NewSection)
						{
							OutError = TEXT("create_camera_cut_section_failed");
							return false;
						}

						const FFrameNumber StartFrame = MovieScene->GetTickResolution().AsFrameTime(StartSeconds).RoundToFrame();
						const FFrameNumber EndFrame = MovieScene->GetTickResolution().AsFrameTime(StartSeconds + FMath::Max(DurationSeconds, 1.0 / MovieScene->GetTickResolution().AsDecimal())).RoundToFrame();
						NewSection->SetRange(TRange<FFrameNumber>(StartFrame, EndFrame));
						NewSection->SetCameraBindingID(FMovieSceneObjectBindingID(UE::MovieScene::FRelativeObjectBindingID(CameraBindingGuid)));
						bool bLockPreviousCamera = false;
						if ((*SectionObj)->TryGetBoolField(TEXT("lock_previous_camera"), bLockPreviousCamera))
						{
							NewSection->bLockPreviousCamera = bLockPreviousCamera;
						}
						CameraCutTrack->AddSection(*NewSection);
						++KeysApplied;
					}
					continue;
				}

				if (TrackType.Equals(TEXT("sub_sequence"), ESearchCase::IgnoreCase) || TrackType.Equals(TEXT("cinematic_shot"), ESearchCase::IgnoreCase))
				{
					const bool bCinematicShot = TrackType.Equals(TEXT("cinematic_shot"), ESearchCase::IgnoreCase);
					UMovieSceneSubTrack* SubTrack = bCinematicShot
						? Cast<UMovieSceneSubTrack>(MovieScene->FindTrack<UMovieSceneCinematicShotTrack>())
						: MovieScene->FindTrack<UMovieSceneSubTrack>();
					if (!SubTrack)
					{
						SubTrack = bCinematicShot
							? Cast<UMovieSceneSubTrack>(MovieScene->AddTrack<UMovieSceneCinematicShotTrack>())
							: MovieScene->AddTrack<UMovieSceneSubTrack>();
					}
					if (!SubTrack)
					{
						OutError = TEXT("create_sub_sequence_track_failed");
						return false;
					}

					SubTrack->Modify();
					UeAgentSequenceFolderOps::ClearTrackSections(SubTrack);
					++TracksApplied;

					const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
					if (!(*MasterTrackObj)->TryGetArrayField(TEXT("sections"), Sections) || !Sections)
					{
						continue;
					}
					for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
					{
						const TSharedPtr<FJsonObject>* SectionObj = nullptr;
						if (!SectionValue.IsValid() || !SectionValue->TryGetObject(SectionObj) || !SectionObj || !(*SectionObj).IsValid())
						{
							continue;
						}

						FString SequenceAssetPath;
						if (!(*SectionObj)->TryGetStringField(TEXT("sequence_asset"), SequenceAssetPath) || SequenceAssetPath.IsEmpty())
						{
							continue;
						}
						UMovieSceneSequence* ChildSequence = Cast<UMovieSceneSequence>(UeAgentSequenceOps::LoadAssetObject(SequenceAssetPath));
						if (!ChildSequence)
						{
							UeAgentSequenceFolderOps::AddWarning(Warnings, TEXT("sub_sequence_asset_not_found"), FString::Printf(TEXT("Skipped sub sequence section because asset '%s' was not found."), *SequenceAssetPath));
							continue;
						}

						double StartSeconds = 0.0;
						double DurationSeconds = 0.0;
						double RowIndexNumber = 0.0;
						if (!(*SectionObj)->TryGetNumberField(TEXT("start_seconds"), StartSeconds) || !(*SectionObj)->TryGetNumberField(TEXT("duration_seconds"), DurationSeconds))
						{
							continue;
						}
						(*SectionObj)->TryGetNumberField(TEXT("row_index"), RowIndexNumber);
						const int32 RowIndex = FMath::RoundToInt(RowIndexNumber);
						const FFrameNumber StartFrame = MovieScene->GetTickResolution().AsFrameTime(StartSeconds).RoundToFrame();
						const FFrameNumber EndFrame = MovieScene->GetTickResolution().AsFrameTime(StartSeconds + FMath::Max(DurationSeconds, 1.0 / MovieScene->GetTickResolution().AsDecimal())).RoundToFrame();
						const int32 DurationFrames = FMath::Max(1, EndFrame.Value - StartFrame.Value);

						UMovieSceneSubSection* NewSection = SubTrack->AddSequenceOnRow(ChildSequence, StartFrame, DurationFrames, RowIndex);
						if (!NewSection)
						{
							OutError = TEXT("create_sub_sequence_section_failed");
							return false;
						}

						NewSection->SetRange(TRange<FFrameNumber>(StartFrame, EndFrame));
						double NumberValue = 0.0;
						bool BoolValue = false;
						if ((*SectionObj)->TryGetNumberField(TEXT("start_frame_offset"), NumberValue)) NewSection->Parameters.StartFrameOffset = FFrameNumber(static_cast<int32>(FMath::RoundToInt(NumberValue)));
						if ((*SectionObj)->TryGetBoolField(TEXT("can_loop"), BoolValue)) NewSection->Parameters.bCanLoop = BoolValue;
						if ((*SectionObj)->TryGetNumberField(TEXT("end_frame_offset"), NumberValue)) NewSection->Parameters.EndFrameOffset = FFrameNumber(static_cast<int32>(FMath::RoundToInt(NumberValue)));
						if ((*SectionObj)->TryGetNumberField(TEXT("first_loop_start_frame_offset"), NumberValue)) NewSection->Parameters.FirstLoopStartFrameOffset = FFrameNumber(static_cast<int32>(FMath::RoundToInt(NumberValue)));
						if ((*SectionObj)->TryGetNumberField(TEXT("hierarchical_bias"), NumberValue)) NewSection->Parameters.HierarchicalBias = FMath::RoundToInt(NumberValue);
						if ((*SectionObj)->TryGetNumberField(TEXT("network_mask"), NumberValue)) NewSection->SetNetworkMask(static_cast<EMovieSceneServerClientMask>(FMath::RoundToInt(NumberValue)));

						FString TimeScaleType;
						if ((*SectionObj)->TryGetStringField(TEXT("time_scale_type"), TimeScaleType))
						{
							if (TimeScaleType.Equals(TEXT("fixed"), ESearchCase::IgnoreCase))
							{
								if ((*SectionObj)->TryGetNumberField(TEXT("time_scale"), NumberValue))
								{
									NewSection->Parameters.TimeScale.Set(static_cast<float>(NumberValue));
								}
							}
							else if (TimeScaleType.Equals(TEXT("custom"), ESearchCase::IgnoreCase))
							{
								UeAgentSequenceFolderOps::AddWarning(Warnings, TEXT("sub_sequence_custom_time_scale_not_supported"), FString::Printf(TEXT("Skipped custom time scale on master track '%s' section."), *TrackType));
							}
						}

						if (bCinematicShot)
						{
							if (UMovieSceneCinematicShotSection* ShotSection = Cast<UMovieSceneCinematicShotSection>(NewSection))
							{
								FString ShotDisplayName;
								if ((*SectionObj)->TryGetStringField(TEXT("shot_display_name"), ShotDisplayName))
								{
									ShotSection->SetShotDisplayName(ShotDisplayName);
								}
							}
						}

						++KeysApplied;
					}

					if (UMovieSceneCinematicShotTrack* CinematicShotTrack = Cast<UMovieSceneCinematicShotTrack>(SubTrack))
					{
						CinematicShotTrack->SortSections();
					}
					continue;
				}

				UeAgentSequenceFolderOps::AddWarning(Warnings, TEXT("master_track_type_not_supported"), FString::Printf(TEXT("Skipped unsupported master track type '%s'."), *TrackType));
			}
		}
	}

	const FString OutlinerFoldersPath = FPaths::Combine(FolderPath, TEXT("outliner"), TEXT("folders.json"));
	if (FPaths::FileExists(OutlinerFoldersPath))
	{
		TSharedPtr<FJsonObject> OutlinerJson;
		FString ResolvedOutlinerPath;
		if (!UeAgentSequenceOps::LoadJsonFile(OutlinerFoldersPath, OutlinerJson, ResolvedOutlinerPath, OutError))
		{
			return false;
		}

		Sequence = UeAgentSequenceOps::LoadLevelSequence(AssetPath);
		UMovieScene* MovieScene = Sequence ? Sequence->GetMovieScene() : nullptr;
		if (!Sequence || !MovieScene)
		{
			OutError = TEXT("movie_scene_not_found");
			return false;
		}
		if (!UeAgentSequenceFolderOps::ApplyOutlinerFoldersJson(Sequence, MovieScene, BindingFolderByGuid, FolderPath, OutlinerJson, Warnings, FoldersApplied, OutError))
		{
			return false;
		}
	}

	bool bSaveAfterApply = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
	if (bSaveAfterApply)
	{
		Sequence = UeAgentSequenceOps::LoadLevelSequence(AssetPath);
		if (!Sequence)
		{
			OutError = TEXT("level_sequence_not_found");
			return false;
		}
		if (!UeAgentSequenceOps::SaveAssetPackage(Sequence, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetNumberField(TEXT("bindings_applied"), BindingsApplied);
	OutData->SetNumberField(TEXT("tracks_applied"), TracksApplied);
	OutData->SetNumberField(TEXT("keys_applied"), KeysApplied);
	OutData->SetNumberField(TEXT("folders_applied"), FoldersApplied);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterApply);
	OutData->SetNumberField(TEXT("warning_count"), Warnings.Num());
	OutData->SetArrayField(TEXT("warnings"), Warnings);
	return true;
}
