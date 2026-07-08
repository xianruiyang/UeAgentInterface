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
		AddSectionMetadataJson(SectionObj, MovieScene, TransformSection);

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
		AddSectionMetadataJson(SectionObj, MovieScene, Section);

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
		AddSectionMetadataJson(SectionObj, MovieScene, Section);

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
		AddSectionMetadataJson(SectionObj, MovieScene, Section);

		TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
		TrackObj->SetStringField(TEXT("track_type"), TEXT("visibility"));
		TrackObj->SetStringField(TEXT("property_name"), Track->GetPropertyName().ToString());
		TrackObj->SetStringField(TEXT("property_path"), Track->GetPropertyPath().ToString());
		TrackObj->SetArrayField(TEXT("sections"), { MakeShared<FJsonValueObject>(SectionObj) });
		return TrackObj;
	}

	static TSharedPtr<FJsonObject> BuildSkeletalAnimationTrackJson(UMovieSceneSkeletalAnimationTrack* Track, UMovieScene* MovieScene)
	{
		if (!Track || !MovieScene)
		{
			return nullptr;
		}

		TArray<TSharedPtr<FJsonValue>> Sections;
		const FFrameRate TickResolution = MovieScene->GetTickResolution();
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
			SectionObj->SetNumberField(TEXT("start_seconds"), TickResolution.AsSeconds(FFrameTime(Section->GetInclusiveStartFrame())));
			SectionObj->SetNumberField(TEXT("end_seconds"), TickResolution.AsSeconds(FFrameTime(Section->GetExclusiveEndFrame())));
			SectionObj->SetNumberField(TEXT("duration_seconds"), TickResolution.AsSeconds(FFrameTime(Section->GetExclusiveEndFrame() - Section->GetInclusiveStartFrame())));
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
			AddSectionMetadataJson(SectionObj, MovieScene, Section);
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
		AddSectionMetadataJson(SectionObj, MovieScene, Section);

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
		AddSectionMetadataJson(SectionObj, MovieScene, Section);

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
		AddSectionMetadataJson(SectionObj, MovieScene, SectionBase);

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
		AddSectionMetadataJson(SectionObj, MovieScene, Section);

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
		AddSectionMetadataJson(SectionObj, MovieScene, Section);

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
		AddSectionMetadataJson(SectionObj, MovieScene, Section);

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
		AddSectionMetadataJson(SectionObj, MovieScene, Section);

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
			AddSectionMetadataJson(SectionObj, MovieScene, Section);
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

			AddSectionMetadataJson(SectionObj, MovieScene, Section);
			Sections.Add(MakeShared<FJsonValueObject>(SectionObj));
		}

		TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
		TrackObj->SetStringField(TEXT("track_type"), bCinematicShot ? TEXT("cinematic_shot") : TEXT("sub_sequence"));
		TrackObj->SetArrayField(TEXT("sections"), Sections);
		return TrackObj;
	}

	static FString ControlRigControlTypeToString(const ERigControlType Type)
	{
		switch (Type)
		{
		case ERigControlType::Bool: return TEXT("bool");
		case ERigControlType::Float: return TEXT("float");
		case ERigControlType::Integer: return TEXT("integer");
		case ERigControlType::Vector2D: return TEXT("vector2d");
		case ERigControlType::Position: return TEXT("position");
		case ERigControlType::Scale: return TEXT("scale");
		case ERigControlType::Rotator: return TEXT("rotator");
		case ERigControlType::Transform: return TEXT("transform");
		case ERigControlType::TransformNoScale: return TEXT("transform_no_scale");
		case ERigControlType::EulerTransform: return TEXT("euler_transform");
		case ERigControlType::ScaleFloat: return TEXT("scale_float");
		default: return TEXT("unknown");
		}
	}

	static FString GetControlRigControlTypeName(UControlRig* ControlRig, const FName& ControlName, const FString& Fallback = FString())
	{
		if (ControlRig)
		{
			if (const FRigControlElement* ControlElement = ControlRig->FindControl(ControlName))
			{
				if (ControlElement->Settings.ControlType == ERigControlType::Integer && ControlElement->Settings.ControlEnum)
				{
					return TEXT("enum");
				}
				return ControlRigControlTypeToString(ControlElement->Settings.ControlType);
			}
		}
		return Fallback.IsEmpty() ? TEXT("unknown") : Fallback;
	}

	static TSharedPtr<FJsonObject> MakeControlRigVectorJson(const FVector& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), Value.X);
		Obj->SetNumberField(TEXT("y"), Value.Y);
		Obj->SetNumberField(TEXT("z"), Value.Z);
		return Obj;
	}

	static TSharedPtr<FJsonObject> MakeControlRigVector2DJson(const FVector2D& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), Value.X);
		Obj->SetNumberField(TEXT("y"), Value.Y);
		return Obj;
	}

	static TSharedPtr<FJsonObject> MakeControlRigRotatorJson(const FRotator& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("pitch"), Value.Pitch);
		Obj->SetNumberField(TEXT("yaw"), Value.Yaw);
		Obj->SetNumberField(TEXT("roll"), Value.Roll);
		return Obj;
	}

	static TSharedPtr<FJsonObject> MakeControlRigTransformJson(const FVector& Location, const FRotator& Rotation, const FVector& Scale)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetObjectField(TEXT("location"), MakeControlRigVectorJson(Location));
		Obj->SetObjectField(TEXT("rotation"), MakeControlRigRotatorJson(Rotation));
		Obj->SetObjectField(TEXT("scale"), MakeControlRigVectorJson(Scale));
		return Obj;
	}

	static bool TryReadControlRigVector(const TSharedPtr<FJsonObject>& Obj, FVector& OutValue)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		double X = 0.0;
		double Y = 0.0;
		double Z = 0.0;
		if (!UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("x"), TEXT("X") }, X)
			|| !UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("y"), TEXT("Y") }, Y))
		{
			return false;
		}
		UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("z"), TEXT("Z") }, Z);
		OutValue = FVector(X, Y, Z);
		return true;
	}

	static bool TryReadControlRigVector2D(const TSharedPtr<FJsonObject>& Obj, FVector2D& OutValue)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		double X = 0.0;
		double Y = 0.0;
		if (!UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("x"), TEXT("X") }, X)
			|| !UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("y"), TEXT("Y") }, Y))
		{
			return false;
		}
		OutValue = FVector2D(X, Y);
		return true;
	}

	static bool TryReadControlRigVectorValue(const TSharedPtr<FJsonValue>& Value, FVector& OutValue)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (Value.IsValid() && Value->TryGetObject(Obj) && Obj && Obj->IsValid())
		{
			return TryReadControlRigVector(*Obj, OutValue);
		}

		const TArray<TSharedPtr<FJsonValue>>* ArrayPtr = nullptr;
		if (Value.IsValid() && Value->TryGetArray(ArrayPtr) && ArrayPtr && ArrayPtr->Num() >= 2)
		{
			double X = 0.0;
			double Y = 0.0;
			double Z = 0.0;
			if (UeAgentJsonDiagnostics::TryReadNumberValue((*ArrayPtr)[0], X)
				&& UeAgentJsonDiagnostics::TryReadNumberValue((*ArrayPtr)[1], Y)
				&& (ArrayPtr->Num() < 3 || UeAgentJsonDiagnostics::TryReadNumberValue((*ArrayPtr)[2], Z)))
			{
				OutValue = FVector(X, Y, Z);
				return true;
			}
		}

		return false;
	}

	static bool TryReadControlRigVector2DValue(const TSharedPtr<FJsonValue>& Value, FVector2D& OutValue)
	{
		return UeAgentJsonDiagnostics::TryReadVector2DValue(Value, OutValue);
	}

	static bool TryReadControlRigRotator(const TSharedPtr<FJsonObject>& Obj, FRotator& OutValue)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		double Pitch = 0.0;
		double Yaw = 0.0;
		double Roll = 0.0;
		const bool bHasPitchYawRoll =
			UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("pitch"), TEXT("Pitch") }, Pitch)
			&& UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("yaw"), TEXT("Yaw") }, Yaw)
			&& UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("roll"), TEXT("Roll") }, Roll);
		if (!bHasPitchYawRoll)
		{
			double X = 0.0;
			double Y = 0.0;
			double Z = 0.0;
			if (!UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("x"), TEXT("X") }, X)
				|| !UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("y"), TEXT("Y") }, Y)
				|| !UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("z"), TEXT("Z") }, Z))
			{
				return false;
			}
			Roll = X;
			Pitch = Y;
			Yaw = Z;
		}
		OutValue = FRotator(Pitch, Yaw, Roll);
		return true;
	}

	static bool TryReadControlRigRotatorValue(const TSharedPtr<FJsonValue>& Value, FRotator& OutValue)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (Value.IsValid() && Value->TryGetObject(Obj) && Obj && Obj->IsValid())
		{
			return TryReadControlRigRotator(*Obj, OutValue);
		}

		return UeAgentJsonDiagnostics::TryReadRotatorValue(Value, OutValue);
	}

	static bool TryReadControlRigTransform(const TSharedPtr<FJsonObject>& Obj, FTransform& OutValue, const bool bRequireScale)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		FVector Scale = FVector::OneVector;
		const TSharedPtr<FJsonValue>* LocationValue = UeAgentJsonDiagnostics::FindFieldValue(Obj, TEXT("location"));
		const TSharedPtr<FJsonValue>* RotationValue = UeAgentJsonDiagnostics::FindFieldValue(Obj, TEXT("rotation"));
		const TSharedPtr<FJsonValue>* ScaleValue = UeAgentJsonDiagnostics::FindFieldValue(Obj, TEXT("scale"));
		const bool bHasLocation = LocationValue && TryReadControlRigVectorValue(*LocationValue, Location);
		const bool bHasRotation = RotationValue && TryReadControlRigRotatorValue(*RotationValue, Rotation);
		const bool bHasScale = ScaleValue && TryReadControlRigVectorValue(*ScaleValue, Scale);
		if (!bHasLocation && !bHasRotation && !bHasScale)
		{
			return false;
		}
		if (bRequireScale && !bHasScale)
		{
			Scale = FVector::OneVector;
		}
		OutValue = FTransform(Rotation, Location, Scale);
		return true;
	}

	static bool TryReadControlRigKeyFrame(UMovieScene* MovieScene, const TSharedPtr<FJsonObject>& KeyObj, FFrameNumber& OutFrame)
	{
		if (!MovieScene || !KeyObj.IsValid())
		{
			return false;
		}
		double TimeSeconds = 0.0;
		if (KeyObj->TryGetNumberField(TEXT("time_seconds"), TimeSeconds))
		{
			OutFrame = MovieScene->GetTickResolution().AsFrameTime(TimeSeconds).RoundToFrame();
			return true;
		}
		double FrameNumber = 0.0;
		if (KeyObj->TryGetNumberField(TEXT("frame"), FrameNumber))
		{
			OutFrame = FFrameNumber(static_cast<int32>(FMath::RoundToInt(FrameNumber)));
			return true;
		}
		return false;
	}

	static TSharedPtr<FJsonObject> MakeControlRigKeyJson(
		UMovieScene* MovieScene,
		const FName& ControlName,
		const FString& ControlType,
		const FFrameNumber Frame)
	{
		TSharedPtr<FJsonObject> KeyObj = BuildKeyObject(MovieScene->GetTickResolution(), Frame.Value);
		KeyObj->SetStringField(TEXT("control"), ControlName.ToString());
		KeyObj->SetStringField(TEXT("control_type"), ControlType);
		return KeyObj;
	}

	static void AddFloatChannelFrames(const FMovieSceneFloatChannel& Channel, TSet<int32>& Frames)
	{
		const auto ChannelData = Channel.GetData();
		for (const FFrameNumber& Frame : ChannelData.GetTimes())
		{
			Frames.Add(Frame.Value);
		}
	}

	static float EvaluateFloatChannelAtFrame(const FMovieSceneFloatChannel& Channel, const FFrameNumber Frame, const float DefaultValue)
	{
		float Value = DefaultValue;
		if (!Channel.Evaluate(Frame, Value))
		{
			if (TOptional<float> Default = Channel.GetDefault())
			{
				Value = Default.GetValue();
			}
		}
		return Value;
	}

	static TSharedPtr<FJsonObject> BuildControlRigTrackJson(UMovieSceneControlRigParameterTrack* Track, UMovieScene* MovieScene)
	{
		if (!Track || !MovieScene)
		{
			return nullptr;
		}

		UControlRig* ControlRig = Track->GetControlRig();
		TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
		TrackObj->SetStringField(TEXT("track_type"), TEXT("control_rig"));
		TrackObj->SetStringField(TEXT("track_name"), Track->GetTrackName().ToString());
		TrackObj->SetStringField(TEXT("display_name"), Track->GetDisplayName().ToString());
		TrackObj->SetStringField(TEXT("control_rig_class"), ControlRig ? ControlRig->GetClass()->GetPathName() : TEXT(""));
		TrackObj->SetBoolField(TEXT("is_additive"), ControlRig ? ControlRig->IsAdditive() : false);

		TArray<TSharedPtr<FJsonValue>> Sections;
		for (UMovieSceneSection* Section : Track->GetAllSections())
		{
			UMovieSceneControlRigParameterSection* ControlRigSection = Cast<UMovieSceneControlRigParameterSection>(Section);
			if (!ControlRigSection)
			{
				continue;
			}

			TArray<TSharedPtr<FJsonValue>> Keys;
			for (const FScalarParameterNameAndCurve& Scalar : ControlRigSection->GetScalarParameterNamesAndCurves())
			{
				const auto ChannelData = Scalar.ParameterCurve.GetData();
				TArrayView<const FFrameNumber> Times = ChannelData.GetTimes();
				TArrayView<const FMovieSceneFloatValue> Values = ChannelData.GetValues();
				for (int32 Index = 0; Index < Times.Num() && Index < Values.Num(); ++Index)
				{
					TSharedPtr<FJsonObject> KeyObj = MakeControlRigKeyJson(MovieScene, Scalar.ParameterName, GetControlRigControlTypeName(ControlRig, Scalar.ParameterName, TEXT("float")), Times[Index]);
					KeyObj->SetNumberField(TEXT("value"), Values[Index].Value);
					Keys.Add(MakeShared<FJsonValueObject>(KeyObj));
				}
			}

			for (const FBoolParameterNameAndCurve& Bool : ControlRigSection->GetBoolParameterNamesAndCurves())
			{
				const auto ChannelData = Bool.ParameterCurve.GetData();
				TArrayView<const FFrameNumber> Times = ChannelData.GetTimes();
				TArrayView<const bool> Values = ChannelData.GetValues();
				for (int32 Index = 0; Index < Times.Num() && Index < Values.Num(); ++Index)
				{
					TSharedPtr<FJsonObject> KeyObj = MakeControlRigKeyJson(MovieScene, Bool.ParameterName, TEXT("bool"), Times[Index]);
					KeyObj->SetBoolField(TEXT("value"), Values[Index]);
					Keys.Add(MakeShared<FJsonValueObject>(KeyObj));
				}
			}

			for (const FIntegerParameterNameAndCurve& Integer : ControlRigSection->GetIntegerParameterNamesAndCurves())
			{
				const auto ChannelData = Integer.ParameterCurve.GetData();
				TArrayView<const FFrameNumber> Times = ChannelData.GetTimes();
				TArrayView<const int32> Values = ChannelData.GetValues();
				for (int32 Index = 0; Index < Times.Num() && Index < Values.Num(); ++Index)
				{
					TSharedPtr<FJsonObject> KeyObj = MakeControlRigKeyJson(MovieScene, Integer.ParameterName, TEXT("integer"), Times[Index]);
					KeyObj->SetNumberField(TEXT("value"), Values[Index]);
					Keys.Add(MakeShared<FJsonValueObject>(KeyObj));
				}
			}

			for (const FEnumParameterNameAndCurve& Enum : ControlRigSection->GetEnumParameterNamesAndCurves())
			{
				const auto ChannelData = Enum.ParameterCurve.GetData();
				TArrayView<const FFrameNumber> Times = ChannelData.GetTimes();
				TArrayView<const uint8> Values = ChannelData.GetValues();
				for (int32 Index = 0; Index < Times.Num() && Index < Values.Num(); ++Index)
				{
					TSharedPtr<FJsonObject> KeyObj = MakeControlRigKeyJson(MovieScene, Enum.ParameterName, TEXT("enum"), Times[Index]);
					KeyObj->SetNumberField(TEXT("value"), Values[Index]);
					Keys.Add(MakeShared<FJsonValueObject>(KeyObj));
				}
			}

			for (const FVector2DParameterNameAndCurves& Vector2D : ControlRigSection->GetVector2DParameterNamesAndCurves())
			{
				TSet<int32> Frames;
				AddFloatChannelFrames(Vector2D.XCurve, Frames);
				AddFloatChannelFrames(Vector2D.YCurve, Frames);
				TArray<int32> SortedFrames = Frames.Array();
				SortedFrames.Sort();
				for (const int32 FrameValue : SortedFrames)
				{
					const FFrameNumber Frame(FrameValue);
					TSharedPtr<FJsonObject> KeyObj = MakeControlRigKeyJson(MovieScene, Vector2D.ParameterName, TEXT("vector2d"), Frame);
					KeyObj->SetObjectField(TEXT("value"), MakeControlRigVector2DJson(FVector2D(
						EvaluateFloatChannelAtFrame(Vector2D.XCurve, Frame, 0.0f),
						EvaluateFloatChannelAtFrame(Vector2D.YCurve, Frame, 0.0f))));
					Keys.Add(MakeShared<FJsonValueObject>(KeyObj));
				}
			}

			for (const FVectorParameterNameAndCurves& Vector : ControlRigSection->GetVectorParameterNamesAndCurves())
			{
				TSet<int32> Frames;
				AddFloatChannelFrames(Vector.XCurve, Frames);
				AddFloatChannelFrames(Vector.YCurve, Frames);
				AddFloatChannelFrames(Vector.ZCurve, Frames);
				TArray<int32> SortedFrames = Frames.Array();
				SortedFrames.Sort();
				const FString ControlType = GetControlRigControlTypeName(ControlRig, Vector.ParameterName, TEXT("vector"));
				for (const int32 FrameValue : SortedFrames)
				{
					const FFrameNumber Frame(FrameValue);
					const FVector Value(
						EvaluateFloatChannelAtFrame(Vector.XCurve, Frame, 0.0f),
						EvaluateFloatChannelAtFrame(Vector.YCurve, Frame, 0.0f),
						EvaluateFloatChannelAtFrame(Vector.ZCurve, Frame, 0.0f));
					TSharedPtr<FJsonObject> KeyObj = MakeControlRigKeyJson(MovieScene, Vector.ParameterName, ControlType, Frame);
					if (ControlType.Equals(TEXT("rotator"), ESearchCase::IgnoreCase))
					{
						KeyObj->SetObjectField(TEXT("value"), MakeControlRigRotatorJson(FRotator(Value.Y, Value.Z, Value.X)));
					}
					else
					{
						KeyObj->SetObjectField(TEXT("value"), MakeControlRigVectorJson(Value));
					}
					Keys.Add(MakeShared<FJsonValueObject>(KeyObj));
				}
			}

			for (const FTransformParameterNameAndCurves& Transform : ControlRigSection->GetTransformParameterNamesAndCurves())
			{
				TSet<int32> Frames;
				for (int32 Index = 0; Index < 3; ++Index)
				{
					AddFloatChannelFrames(Transform.Translation[Index], Frames);
					AddFloatChannelFrames(Transform.Rotation[Index], Frames);
					AddFloatChannelFrames(Transform.Scale[Index], Frames);
				}
				TArray<int32> SortedFrames = Frames.Array();
				SortedFrames.Sort();
				const FString ControlType = GetControlRigControlTypeName(ControlRig, Transform.ParameterName, TEXT("transform"));
				for (const int32 FrameValue : SortedFrames)
				{
					const FFrameNumber Frame(FrameValue);
					const FVector Location(
						EvaluateFloatChannelAtFrame(Transform.Translation[0], Frame, 0.0f),
						EvaluateFloatChannelAtFrame(Transform.Translation[1], Frame, 0.0f),
						EvaluateFloatChannelAtFrame(Transform.Translation[2], Frame, 0.0f));
					const FRotator Rotation(
						EvaluateFloatChannelAtFrame(Transform.Rotation[1], Frame, 0.0f),
						EvaluateFloatChannelAtFrame(Transform.Rotation[2], Frame, 0.0f),
						EvaluateFloatChannelAtFrame(Transform.Rotation[0], Frame, 0.0f));
					const FVector Scale(
						EvaluateFloatChannelAtFrame(Transform.Scale[0], Frame, 1.0f),
						EvaluateFloatChannelAtFrame(Transform.Scale[1], Frame, 1.0f),
						EvaluateFloatChannelAtFrame(Transform.Scale[2], Frame, 1.0f));
					TSharedPtr<FJsonObject> KeyObj = MakeControlRigKeyJson(MovieScene, Transform.ParameterName, ControlType, Frame);
					KeyObj->SetObjectField(TEXT("value"), MakeControlRigTransformJson(Location, Rotation, Scale));
					Keys.Add(MakeShared<FJsonValueObject>(KeyObj));
				}
			}

			TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
			SectionObj->SetStringField(TEXT("section_type"), TEXT("control_rig"));
			SectionObj->SetStringField(TEXT("control_rig_class"), ControlRigSection->ControlRigClass ? ControlRigSection->ControlRigClass->GetPathName() : TEXT(""));
			SectionObj->SetArrayField(TEXT("keys"), Keys);
			AddSectionMetadataJson(SectionObj, MovieScene, ControlRigSection);
			Sections.Add(MakeShared<FJsonValueObject>(SectionObj));
		}

		TrackObj->SetArrayField(TEXT("sections"), Sections);
		return TrackObj;
	}
