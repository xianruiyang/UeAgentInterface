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

	static void SetStringArrayField(TSharedPtr<FJsonObject> Obj, const FString& FieldName, const TArray<FString>& Values)
	{
		if (!Obj.IsValid())
		{
			return;
		}

		TArray<TSharedPtr<FJsonValue>> JsonValues;
		JsonValues.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		Obj->SetArrayField(FieldName, JsonValues);
	}

	static int32 CountTrackJsonSections(const TSharedPtr<FJsonObject>& TrackObj)
	{
		const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
		return TrackObj.IsValid() && TrackObj->TryGetArrayField(TEXT("sections"), Sections) && Sections ? Sections->Num() : 0;
	}

	static int32 CountTrackJsonKeys(const TSharedPtr<FJsonObject>& TrackObj)
	{
		if (!TrackObj.IsValid())
		{
			return 0;
		}

		int32 KeyCount = 0;
		const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
		if (!TrackObj->TryGetArrayField(TEXT("sections"), Sections) || !Sections)
		{
			return 0;
		}

		for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
		{
			const TSharedPtr<FJsonObject>* SectionObj = nullptr;
			if (!SectionValue.IsValid() || !SectionValue->TryGetObject(SectionObj) || !SectionObj || !SectionObj->IsValid())
			{
				continue;
			}
			const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
			if ((*SectionObj)->TryGetArrayField(TEXT("keys"), Keys) && Keys)
			{
				KeyCount += Keys->Num();
			}
		}
		return KeyCount;
	}

	static FString GuessRoundtripStatusFromTrackJson(const TSharedPtr<FJsonObject>& TrackObj)
	{
		if (!TrackObj.IsValid())
		{
			return TEXT("unsupported");
		}

		FString ExistingStatus;
		if (TrackObj->TryGetStringField(TEXT("roundtrip_status"), ExistingStatus) && !ExistingStatus.IsEmpty())
		{
			return ExistingStatus;
		}

		FString TrackType;
		TrackObj->TryGetStringField(TEXT("track_type"), TrackType);
		if (TrackType.Equals(TEXT("unsupported"), ESearchCase::IgnoreCase))
		{
			return TEXT("readonly_summary");
		}
		if (TrackType.Equals(TEXT("property"), ESearchCase::IgnoreCase))
		{
			FString PropertyType;
			TrackObj->TryGetStringField(TEXT("property_type"), PropertyType);
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
				return TEXT("unsupported");
			}
			return TEXT("partial_roundtrip");
		}
		if (TrackType.Equals(TEXT("transform"), ESearchCase::IgnoreCase)
			|| TrackType.Equals(TEXT("spawn"), ESearchCase::IgnoreCase)
			|| TrackType.Equals(TEXT("skeletal_animation"), ESearchCase::IgnoreCase)
			|| TrackType.Equals(TEXT("control_rig"), ESearchCase::IgnoreCase)
			|| TrackType.Equals(TEXT("visibility"), ESearchCase::IgnoreCase)
			|| TrackType.Equals(TEXT("camera_cut"), ESearchCase::IgnoreCase)
			|| TrackType.Equals(TEXT("sub_sequence"), ESearchCase::IgnoreCase)
			|| TrackType.Equals(TEXT("cinematic_shot"), ESearchCase::IgnoreCase))
		{
			return TEXT("partial_roundtrip");
		}
		return TEXT("unsupported");
	}

	static FString GuessApplySupportFromTrackJson(const TSharedPtr<FJsonObject>& TrackObj)
	{
		if (!TrackObj.IsValid())
		{
			return TEXT("unsupported");
		}

		FString ExistingSupport;
		if (TrackObj->TryGetStringField(TEXT("apply_support"), ExistingSupport) && !ExistingSupport.IsEmpty())
		{
			return ExistingSupport;
		}

		const FString Status = GuessRoundtripStatusFromTrackJson(TrackObj);
		if (Status.Equals(TEXT("readonly_summary"), ESearchCase::IgnoreCase) || Status.Equals(TEXT("unsupported"), ESearchCase::IgnoreCase))
		{
			return TEXT("unsupported");
		}
		return TEXT("supported_partial");
	}

	static bool IsPartialRoundtripStatus(const FString& Status)
	{
		return Status.Equals(TEXT("partial_roundtrip"), ESearchCase::IgnoreCase);
	}

	static bool IsUnsupportedRoundtripStatus(const FString& Status)
	{
		return Status.Equals(TEXT("readonly_summary"), ESearchCase::IgnoreCase) || Status.Equals(TEXT("unsupported"), ESearchCase::IgnoreCase);
	}

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

	static FString EnumValueToName(const UEnum* Enum, const int64 Value)
	{
		return Enum ? Enum->GetNameStringByValue(Value) : FString::FromInt(static_cast<int32>(Value));
	}

	static bool TryParseEnumValue(const UEnum* Enum, const FString& Text, int64& OutValue)
	{
		if (Text.IsEmpty())
		{
			return false;
		}
		if (Enum)
		{
			const int64 NamedValue = Enum->GetValueByNameString(Text);
			if (NamedValue != INDEX_NONE)
			{
				OutValue = NamedValue;
				return true;
			}
		}
		if (Text.IsNumeric())
		{
			OutValue = FCString::Atoi64(*Text);
			return true;
		}
		return false;
	}

	static TSharedPtr<FJsonObject> BuildTangentDataJson(const FMovieSceneTangentData& Tangent)
	{
		TSharedPtr<FJsonObject> TangentObj = MakeShared<FJsonObject>();
		TangentObj->SetNumberField(TEXT("arrive_tangent"), Tangent.ArriveTangent);
		TangentObj->SetNumberField(TEXT("leave_tangent"), Tangent.LeaveTangent);
		TangentObj->SetNumberField(TEXT("arrive_tangent_weight"), Tangent.ArriveTangentWeight);
		TangentObj->SetNumberField(TEXT("leave_tangent_weight"), Tangent.LeaveTangentWeight);
		TangentObj->SetStringField(TEXT("tangent_weight_mode"), EnumValueToName(StaticEnum<ERichCurveTangentWeightMode>(), static_cast<int64>(Tangent.TangentWeightMode.GetValue())));
		return TangentObj;
	}

	static void ApplyTangentDataJson(const TSharedPtr<FJsonObject>& TangentObj, FMovieSceneTangentData& Tangent)
	{
		if (!TangentObj.IsValid())
		{
			return;
		}
		double NumberValue = 0.0;
		if (TangentObj->TryGetNumberField(TEXT("arrive_tangent"), NumberValue)) Tangent.ArriveTangent = static_cast<float>(NumberValue);
		if (TangentObj->TryGetNumberField(TEXT("leave_tangent"), NumberValue)) Tangent.LeaveTangent = static_cast<float>(NumberValue);
		if (TangentObj->TryGetNumberField(TEXT("arrive_tangent_weight"), NumberValue)) Tangent.ArriveTangentWeight = static_cast<float>(NumberValue);
		if (TangentObj->TryGetNumberField(TEXT("leave_tangent_weight"), NumberValue)) Tangent.LeaveTangentWeight = static_cast<float>(NumberValue);
		FString ModeText;
		int64 ModeValue = 0;
		if (TangentObj->TryGetStringField(TEXT("tangent_weight_mode"), ModeText)
			&& TryParseEnumValue(StaticEnum<ERichCurveTangentWeightMode>(), ModeText, ModeValue))
		{
			Tangent.TangentWeightMode = static_cast<ERichCurveTangentWeightMode>(ModeValue);
		}
	}

	static TSharedPtr<FJsonObject> BuildDoubleChannelJson(const FMovieSceneDoubleChannel& Channel, const FFrameRate& TickResolution, const int32 ChannelIndex)
	{
		TSharedPtr<FJsonObject> ChannelObj = MakeShared<FJsonObject>();
		ChannelObj->SetStringField(TEXT("channel_type"), TEXT("double"));
		ChannelObj->SetNumberField(TEXT("channel_index"), ChannelIndex);
		ChannelObj->SetStringField(TEXT("channel_name"), FString::Printf(TEXT("double_%d"), ChannelIndex));
		if (const TOptional<double> DefaultValue = Channel.GetDefault())
		{
			ChannelObj->SetBoolField(TEXT("has_default"), true);
			ChannelObj->SetNumberField(TEXT("default_value"), *DefaultValue);
		}
		else
		{
			ChannelObj->SetBoolField(TEXT("has_default"), false);
		}

		TArray<TSharedPtr<FJsonValue>> Keys;
		const auto ChannelData = Channel.GetData();
		TArrayView<const FFrameNumber> Times = ChannelData.GetTimes();
		TArrayView<const FMovieSceneDoubleValue> Values = ChannelData.GetValues();
		for (int32 Index = 0; Index < Times.Num() && Index < Values.Num(); ++Index)
		{
			TSharedPtr<FJsonObject> KeyObj = BuildKeyObject(TickResolution, Times[Index].Value);
			KeyObj->SetNumberField(TEXT("value"), Values[Index].Value);
			KeyObj->SetStringField(TEXT("interp_mode"), EnumValueToName(StaticEnum<ERichCurveInterpMode>(), static_cast<int64>(Values[Index].InterpMode.GetValue())));
			KeyObj->SetStringField(TEXT("tangent_mode"), EnumValueToName(StaticEnum<ERichCurveTangentMode>(), static_cast<int64>(Values[Index].TangentMode.GetValue())));
			KeyObj->SetObjectField(TEXT("tangent"), BuildTangentDataJson(Values[Index].Tangent));
			Keys.Add(MakeShared<FJsonValueObject>(KeyObj));
		}
		ChannelObj->SetArrayField(TEXT("keys"), Keys);
		return ChannelObj;
	}

	static TSharedPtr<FJsonObject> BuildFloatChannelJson(const FMovieSceneFloatChannel& Channel, const FFrameRate& TickResolution, const int32 ChannelIndex)
	{
		TSharedPtr<FJsonObject> ChannelObj = MakeShared<FJsonObject>();
		ChannelObj->SetStringField(TEXT("channel_type"), TEXT("float"));
		ChannelObj->SetNumberField(TEXT("channel_index"), ChannelIndex);
		ChannelObj->SetStringField(TEXT("channel_name"), FString::Printf(TEXT("float_%d"), ChannelIndex));
		if (const TOptional<float> DefaultValue = Channel.GetDefault())
		{
			ChannelObj->SetBoolField(TEXT("has_default"), true);
			ChannelObj->SetNumberField(TEXT("default_value"), *DefaultValue);
		}
		else
		{
			ChannelObj->SetBoolField(TEXT("has_default"), false);
		}

		TArray<TSharedPtr<FJsonValue>> Keys;
		const auto ChannelData = Channel.GetData();
		TArrayView<const FFrameNumber> Times = ChannelData.GetTimes();
		TArrayView<const FMovieSceneFloatValue> Values = ChannelData.GetValues();
		for (int32 Index = 0; Index < Times.Num() && Index < Values.Num(); ++Index)
		{
			TSharedPtr<FJsonObject> KeyObj = BuildKeyObject(TickResolution, Times[Index].Value);
			KeyObj->SetNumberField(TEXT("value"), Values[Index].Value);
			KeyObj->SetStringField(TEXT("interp_mode"), EnumValueToName(StaticEnum<ERichCurveInterpMode>(), static_cast<int64>(Values[Index].InterpMode.GetValue())));
			KeyObj->SetStringField(TEXT("tangent_mode"), EnumValueToName(StaticEnum<ERichCurveTangentMode>(), static_cast<int64>(Values[Index].TangentMode.GetValue())));
			KeyObj->SetObjectField(TEXT("tangent"), BuildTangentDataJson(Values[Index].Tangent));
			Keys.Add(MakeShared<FJsonValueObject>(KeyObj));
		}
		ChannelObj->SetArrayField(TEXT("keys"), Keys);
		return ChannelObj;
	}

	template<typename ChannelType, typename ValueType>
	static TSharedPtr<FJsonObject> BuildDiscreteNumberChannelJson(const ChannelType& Channel, const FFrameRate& TickResolution, const int32 ChannelIndex, const FString& ChannelTypeName)
	{
		TSharedPtr<FJsonObject> ChannelObj = MakeShared<FJsonObject>();
		ChannelObj->SetStringField(TEXT("channel_type"), ChannelTypeName);
		ChannelObj->SetNumberField(TEXT("channel_index"), ChannelIndex);
		ChannelObj->SetStringField(TEXT("channel_name"), FString::Printf(TEXT("%s_%d"), *ChannelTypeName, ChannelIndex));
		if (const TOptional<ValueType> DefaultValue = Channel.GetDefault())
		{
			ChannelObj->SetBoolField(TEXT("has_default"), true);
			ChannelObj->SetNumberField(TEXT("default_value"), static_cast<double>(*DefaultValue));
		}
		else
		{
			ChannelObj->SetBoolField(TEXT("has_default"), false);
		}

		TArray<TSharedPtr<FJsonValue>> Keys;
		const auto ChannelData = Channel.GetData();
		TArrayView<const FFrameNumber> Times = ChannelData.GetTimes();
		TArrayView<const ValueType> Values = ChannelData.GetValues();
		for (int32 Index = 0; Index < Times.Num() && Index < Values.Num(); ++Index)
		{
			TSharedPtr<FJsonObject> KeyObj = BuildKeyObject(TickResolution, Times[Index].Value);
			KeyObj->SetNumberField(TEXT("value"), static_cast<double>(Values[Index]));
			Keys.Add(MakeShared<FJsonValueObject>(KeyObj));
		}
		ChannelObj->SetArrayField(TEXT("keys"), Keys);
		return ChannelObj;
	}

	static TSharedPtr<FJsonObject> BuildBoolChannelJson(const FMovieSceneBoolChannel& Channel, const FFrameRate& TickResolution, const int32 ChannelIndex)
	{
		TSharedPtr<FJsonObject> ChannelObj = MakeShared<FJsonObject>();
		ChannelObj->SetStringField(TEXT("channel_type"), TEXT("bool"));
		ChannelObj->SetNumberField(TEXT("channel_index"), ChannelIndex);
		ChannelObj->SetStringField(TEXT("channel_name"), FString::Printf(TEXT("bool_%d"), ChannelIndex));
		if (const TOptional<bool> DefaultValue = Channel.GetDefault())
		{
			ChannelObj->SetBoolField(TEXT("has_default"), true);
			ChannelObj->SetBoolField(TEXT("default_value"), *DefaultValue);
		}
		else
		{
			ChannelObj->SetBoolField(TEXT("has_default"), false);
		}

		TArray<TSharedPtr<FJsonValue>> Keys;
		const auto ChannelData = Channel.GetData();
		TArrayView<const FFrameNumber> Times = ChannelData.GetTimes();
		TArrayView<const bool> Values = ChannelData.GetValues();
		for (int32 Index = 0; Index < Times.Num() && Index < Values.Num(); ++Index)
		{
			TSharedPtr<FJsonObject> KeyObj = BuildKeyObject(TickResolution, Times[Index].Value);
			KeyObj->SetBoolField(TEXT("value"), Values[Index]);
			Keys.Add(MakeShared<FJsonValueObject>(KeyObj));
		}
		ChannelObj->SetArrayField(TEXT("keys"), Keys);
		return ChannelObj;
	}

	static TSharedPtr<FJsonObject> BuildStringChannelJson(const FMovieSceneStringChannel& Channel, const FFrameRate& TickResolution, const int32 ChannelIndex)
	{
		TSharedPtr<FJsonObject> ChannelObj = MakeShared<FJsonObject>();
		ChannelObj->SetStringField(TEXT("channel_type"), TEXT("string"));
		ChannelObj->SetNumberField(TEXT("channel_index"), ChannelIndex);
		ChannelObj->SetStringField(TEXT("channel_name"), FString::Printf(TEXT("string_%d"), ChannelIndex));
		if (const TOptional<FString> DefaultValue = Channel.GetDefault())
		{
			ChannelObj->SetBoolField(TEXT("has_default"), true);
			ChannelObj->SetStringField(TEXT("default_value"), *DefaultValue);
		}
		else
		{
			ChannelObj->SetBoolField(TEXT("has_default"), false);
		}

		TArray<TSharedPtr<FJsonValue>> Keys;
		const auto ChannelData = Channel.GetData();
		TArrayView<const FFrameNumber> Times = ChannelData.GetTimes();
		TArrayView<const FString> Values = ChannelData.GetValues();
		for (int32 Index = 0; Index < Times.Num() && Index < Values.Num(); ++Index)
		{
			TSharedPtr<FJsonObject> KeyObj = BuildKeyObject(TickResolution, Times[Index].Value);
			KeyObj->SetStringField(TEXT("value"), Values[Index]);
			Keys.Add(MakeShared<FJsonValueObject>(KeyObj));
		}
		ChannelObj->SetArrayField(TEXT("keys"), Keys);
		return ChannelObj;
	}

	static TArray<TSharedPtr<FJsonValue>> BuildSectionChannelsJson(UMovieScene* MovieScene, UMovieSceneSection* Section)
	{
		TArray<TSharedPtr<FJsonValue>> ChannelsJson;
		if (!MovieScene || !Section)
		{
			return ChannelsJson;
		}
		const FFrameRate TickResolution = MovieScene->GetTickResolution();

		TArrayView<FMovieSceneDoubleChannel*> DoubleChannels = Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
		for (int32 Index = 0; Index < DoubleChannels.Num(); ++Index)
		{
			if (DoubleChannels[Index])
			{
				ChannelsJson.Add(MakeShared<FJsonValueObject>(BuildDoubleChannelJson(*DoubleChannels[Index], TickResolution, Index)));
			}
		}

		TArrayView<FMovieSceneFloatChannel*> FloatChannels = Section->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>();
		for (int32 Index = 0; Index < FloatChannels.Num(); ++Index)
		{
			if (FloatChannels[Index])
			{
				ChannelsJson.Add(MakeShared<FJsonValueObject>(BuildFloatChannelJson(*FloatChannels[Index], TickResolution, Index)));
			}
		}

		TArrayView<FMovieSceneBoolChannel*> BoolChannels = Section->GetChannelProxy().GetChannels<FMovieSceneBoolChannel>();
		for (int32 Index = 0; Index < BoolChannels.Num(); ++Index)
		{
			if (BoolChannels[Index])
			{
				ChannelsJson.Add(MakeShared<FJsonValueObject>(BuildBoolChannelJson(*BoolChannels[Index], TickResolution, Index)));
			}
		}

		TArrayView<FMovieSceneIntegerChannel*> IntegerChannels = Section->GetChannelProxy().GetChannels<FMovieSceneIntegerChannel>();
		for (int32 Index = 0; Index < IntegerChannels.Num(); ++Index)
		{
			if (IntegerChannels[Index])
			{
				ChannelsJson.Add(MakeShared<FJsonValueObject>(BuildDiscreteNumberChannelJson<FMovieSceneIntegerChannel, int32>(*IntegerChannels[Index], TickResolution, Index, TEXT("integer"))));
			}
		}

		TArrayView<FMovieSceneByteChannel*> ByteChannels = Section->GetChannelProxy().GetChannels<FMovieSceneByteChannel>();
		for (int32 Index = 0; Index < ByteChannels.Num(); ++Index)
		{
			if (ByteChannels[Index])
			{
				ChannelsJson.Add(MakeShared<FJsonValueObject>(BuildDiscreteNumberChannelJson<FMovieSceneByteChannel, uint8>(*ByteChannels[Index], TickResolution, Index, TEXT("byte"))));
			}
		}

		TArrayView<FMovieSceneStringChannel*> StringChannels = Section->GetChannelProxy().GetChannels<FMovieSceneStringChannel>();
		for (int32 Index = 0; Index < StringChannels.Num(); ++Index)
		{
			if (StringChannels[Index])
			{
				ChannelsJson.Add(MakeShared<FJsonValueObject>(BuildStringChannelJson(*StringChannels[Index], TickResolution, Index)));
			}
		}

		return ChannelsJson;
	}

	static TSharedPtr<FJsonObject> BuildFrameRateJson(const FFrameRate& Rate)
	{
		TSharedPtr<FJsonObject> RateObj = MakeShared<FJsonObject>();
		RateObj->SetNumberField(TEXT("numerator"), Rate.Numerator);
		RateObj->SetNumberField(TEXT("denominator"), Rate.Denominator);
		RateObj->SetNumberField(TEXT("decimal"), Rate.AsDecimal());
		return RateObj;
	}

	static TSharedPtr<FJsonObject> BuildSectionColorJson(const FColor& Color)
	{
		TSharedPtr<FJsonObject> ColorObj = MakeShared<FJsonObject>();
		ColorObj->SetNumberField(TEXT("r"), Color.R);
		ColorObj->SetNumberField(TEXT("g"), Color.G);
		ColorObj->SetNumberField(TEXT("b"), Color.B);
		ColorObj->SetNumberField(TEXT("a"), Color.A);
		return ColorObj;
	}

	static bool TryParseSectionColorJson(const TSharedPtr<FJsonObject>& ColorObj, FColor& OutColor)
	{
		if (!ColorObj.IsValid())
		{
			return false;
		}

		double R = 255.0;
		double G = 255.0;
		double B = 255.0;
		double A = 255.0;
		if (!UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(ColorObj, { TEXT("r"), TEXT("R"), TEXT("red"), TEXT("Red") }, R)
			|| !UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(ColorObj, { TEXT("g"), TEXT("G"), TEXT("green"), TEXT("Green") }, G)
			|| !UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(ColorObj, { TEXT("b"), TEXT("B"), TEXT("blue"), TEXT("Blue") }, B))
		{
			return false;
		}
		UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(ColorObj, { TEXT("a"), TEXT("A"), TEXT("alpha"), TEXT("Alpha") }, A);
		OutColor = FColor(
			static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(R), 0, 255)),
			static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(G), 0, 255)),
			static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(B), 0, 255)),
			static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(A), 0, 255)));
		return true;
	}

	static TSharedPtr<FJsonObject> BuildSectionMetadataJson(UMovieScene* MovieScene, UMovieSceneSection* Section)
	{
		TSharedPtr<FJsonObject> MetadataObj = MakeShared<FJsonObject>();
		if (!MovieScene || !Section)
		{
			return MetadataObj;
		}

		const FFrameRate TickResolution = MovieScene->GetTickResolution();
		MetadataObj->SetStringField(TEXT("schema"), TEXT("uai.sequence.section_metadata.v1"));
		MetadataObj->SetStringField(TEXT("section_name"), Section->GetName());
		MetadataObj->SetStringField(TEXT("section_path"), Section->GetPathName());
		MetadataObj->SetStringField(TEXT("section_class"), Section->GetClass() ? Section->GetClass()->GetPathName() : TEXT(""));
		MetadataObj->SetStringField(TEXT("range_space"), TEXT("tick_resolution"));
		MetadataObj->SetObjectField(TEXT("tick_resolution"), BuildFrameRateJson(TickResolution));

		const bool bHasStartFrame = Section->HasStartFrame();
		const bool bHasEndFrame = Section->HasEndFrame();
		MetadataObj->SetBoolField(TEXT("has_start_frame"), bHasStartFrame);
		MetadataObj->SetBoolField(TEXT("has_end_frame"), bHasEndFrame);
		if (bHasStartFrame)
		{
			const FFrameNumber StartFrame = Section->GetInclusiveStartFrame();
			MetadataObj->SetNumberField(TEXT("start_frame"), StartFrame.Value);
			MetadataObj->SetNumberField(TEXT("start_seconds"), TickResolution.AsSeconds(FFrameTime(StartFrame)));
		}
		if (bHasEndFrame)
		{
			const FFrameNumber EndFrame = Section->GetExclusiveEndFrame();
			MetadataObj->SetNumberField(TEXT("end_frame"), EndFrame.Value);
			MetadataObj->SetNumberField(TEXT("end_seconds"), TickResolution.AsSeconds(FFrameTime(EndFrame)));
		}
		if (bHasStartFrame && bHasEndFrame)
		{
			const FFrameNumber DurationFrames = Section->GetExclusiveEndFrame() - Section->GetInclusiveStartFrame();
			MetadataObj->SetNumberField(TEXT("duration_frames"), DurationFrames.Value);
			MetadataObj->SetNumberField(TEXT("duration_seconds"), TickResolution.AsSeconds(FFrameTime(DurationFrames)));
			MetadataObj->SetStringField(TEXT("range_type"), TEXT("bounded"));
		}
		else if (!bHasStartFrame && !bHasEndFrame)
		{
			MetadataObj->SetStringField(TEXT("range_type"), TEXT("all"));
		}
		else if (!bHasStartFrame)
		{
			MetadataObj->SetStringField(TEXT("range_type"), TEXT("open_start"));
		}
		else
		{
			MetadataObj->SetStringField(TEXT("range_type"), TEXT("open_end"));
		}

		MetadataObj->SetNumberField(TEXT("row_index"), Section->GetRowIndex());
		MetadataObj->SetNumberField(TEXT("overlap_priority"), Section->GetOverlapPriority());
		MetadataObj->SetBoolField(TEXT("is_active"), Section->IsActive());
		MetadataObj->SetBoolField(TEXT("is_locked"), Section->IsLocked());
		MetadataObj->SetNumberField(TEXT("pre_roll_frames"), Section->GetPreRollFrames());
		MetadataObj->SetNumberField(TEXT("post_roll_frames"), Section->GetPostRollFrames());
		MetadataObj->SetObjectField(TEXT("color_tint"), BuildSectionColorJson(Section->GetColorTint()));
		return MetadataObj;
	}

	static TArray<TSharedPtr<FJsonValue>> BuildSequenceRawPropertiesJson(UObject* Owner);
	static TArray<TSharedPtr<FJsonValue>> BuildSectionChannelsJson(UMovieScene* MovieScene, UMovieSceneSection* Section);

	static void AddSectionMetadataJson(TSharedPtr<FJsonObject> SectionObj, UMovieScene* MovieScene, UMovieSceneSection* Section)
	{
		if (SectionObj.IsValid() && MovieScene && Section)
		{
			SectionObj->SetObjectField(TEXT("section_metadata"), BuildSectionMetadataJson(MovieScene, Section));
			SectionObj->SetArrayField(TEXT("raw_properties"), BuildSequenceRawPropertiesJson(Section));
			SectionObj->SetArrayField(TEXT("channels"), BuildSectionChannelsJson(MovieScene, Section));
		}
	}

	static bool IsSequenceRawPropertyStructural(const FString& PropertyName)
	{
		static const TSet<FString> StructuralNames = {
			TEXT("MovieScene"),
			TEXT("ObjectBindings"),
			TEXT("Possessables"),
			TEXT("Spawnables"),
			TEXT("Tracks"),
			TEXT("CameraCutTrack"),
			TEXT("RootFolders"),
			TEXT("MarkedFrames"),
			TEXT("ChannelProxy"),
			TEXT("EvaluationField"),
			TEXT("CompiledDataManager"),
			TEXT("NodeGroups"),
			TEXT("SoloNodes_DEPRECATED"),
			TEXT("MuteNodes_DEPRECATED")
		};
		return StructuralNames.Contains(PropertyName);
	}

	static bool IsSequenceRawPropertyReferenceLike(const FProperty* Property)
	{
		return CastField<FObjectPropertyBase>(Property) != nullptr
			|| CastField<FSoftObjectProperty>(Property) != nullptr
			|| CastField<FClassProperty>(Property) != nullptr
			|| CastField<FSoftClassProperty>(Property) != nullptr;
	}

	static bool IsSequenceRawPropertyContainerLike(const FProperty* Property)
	{
		return CastField<FArrayProperty>(Property) != nullptr
			|| CastField<FMapProperty>(Property) != nullptr
			|| CastField<FSetProperty>(Property) != nullptr;
	}

	static bool ShouldApplySequenceRawProperty(const FProperty* Property)
	{
		if (!Property)
		{
			return false;
		}
		if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_EditConst))
		{
			return false;
		}
		if (!Property->HasAnyPropertyFlags(CPF_Edit))
		{
			return false;
		}
		if (IsSequenceRawPropertyStructural(Property->GetName()))
		{
			return false;
		}
		if (IsSequenceRawPropertyReferenceLike(Property) || IsSequenceRawPropertyContainerLike(Property))
		{
			return false;
		}
		return true;
	}

	static TSharedPtr<FJsonObject> BuildSequenceRawPropertyJson(FProperty* Property, UObject* Owner)
	{
		TSharedPtr<FJsonObject> PropertyObj = MakeShared<FJsonObject>();
		if (!Property || !Owner)
		{
			PropertyObj->SetBoolField(TEXT("valid"), false);
			return PropertyObj;
		}

		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Owner);
		FString ValueText;
		Property->ExportTextItem_Direct(ValueText, ValuePtr, nullptr, Owner, PPF_None);

		const bool bReferenceLike = IsSequenceRawPropertyReferenceLike(Property);
		const bool bContainerLike = IsSequenceRawPropertyContainerLike(Property);
		const bool bStructural = IsSequenceRawPropertyStructural(Property->GetName());
		const bool bCanApplyRaw = ShouldApplySequenceRawProperty(Property);

		PropertyObj->SetBoolField(TEXT("valid"), true);
		PropertyObj->SetStringField(TEXT("property_name"), Property->GetName());
		PropertyObj->SetStringField(TEXT("authored_name"), Property->GetAuthoredName());
		PropertyObj->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
		PropertyObj->SetStringField(TEXT("property_class"), Property->GetClass() ? Property->GetClass()->GetName() : TEXT(""));
		PropertyObj->SetStringField(TEXT("value_text"), ValueText);
		PropertyObj->SetStringField(TEXT("property_flags"), FString::Printf(TEXT("0x%016llx"), static_cast<unsigned long long>(Property->PropertyFlags)));
		PropertyObj->SetBoolField(TEXT("editable"), Property->HasAnyPropertyFlags(CPF_Edit));
		PropertyObj->SetBoolField(TEXT("blueprint_visible"), Property->HasAnyPropertyFlags(CPF_BlueprintVisible));
		PropertyObj->SetBoolField(TEXT("transient"), Property->HasAnyPropertyFlags(CPF_Transient));
		PropertyObj->SetBoolField(TEXT("deprecated"), Property->HasAnyPropertyFlags(CPF_Deprecated));
		PropertyObj->SetBoolField(TEXT("edit_const"), Property->HasAnyPropertyFlags(CPF_EditConst));
		PropertyObj->SetBoolField(TEXT("structural"), bStructural);
		PropertyObj->SetBoolField(TEXT("reference_like"), bReferenceLike);
		PropertyObj->SetBoolField(TEXT("container_like"), bContainerLike);
		PropertyObj->SetBoolField(TEXT("can_apply_raw"), bCanApplyRaw);
		if (!bCanApplyRaw)
		{
			TArray<FString> Reasons;
			if (!Property->HasAnyPropertyFlags(CPF_Edit)) Reasons.Add(TEXT("not_edit_property"));
			if (Property->HasAnyPropertyFlags(CPF_Transient)) Reasons.Add(TEXT("transient"));
			if (Property->HasAnyPropertyFlags(CPF_Deprecated)) Reasons.Add(TEXT("deprecated"));
			if (Property->HasAnyPropertyFlags(CPF_EditConst)) Reasons.Add(TEXT("edit_const"));
			if (bStructural) Reasons.Add(TEXT("structural_sequence_graph_field"));
			if (bReferenceLike) Reasons.Add(TEXT("reference_like"));
			if (bContainerLike) Reasons.Add(TEXT("container_like"));
			SetStringArrayField(PropertyObj, TEXT("raw_apply_block_reasons"), Reasons);
		}
		return PropertyObj;
	}

	static TArray<TSharedPtr<FJsonValue>> BuildSequenceRawPropertiesJson(UObject* Owner)
	{
		TArray<TSharedPtr<FJsonValue>> PropertiesJson;
		if (!Owner || !Owner->GetClass())
		{
			return PropertiesJson;
		}
		for (TFieldIterator<FProperty> It(Owner->GetClass(), EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			if (FProperty* Property = *It)
			{
				PropertiesJson.Add(MakeShared<FJsonValueObject>(BuildSequenceRawPropertyJson(Property, Owner)));
			}
		}
		return PropertiesJson;
	}

	static FProperty* FindSequenceRawPropertyByName(UClass* Class, const FString& PropertyName)
	{
		if (!Class || PropertyName.IsEmpty())
		{
			return nullptr;
		}
		for (TFieldIterator<FProperty> It(Class, EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property)
			{
				continue;
			}
			if (Property->GetName().Equals(PropertyName, ESearchCase::CaseSensitive)
				|| Property->GetAuthoredName().Equals(PropertyName, ESearchCase::CaseSensitive)
				|| Property->GetName().Equals(PropertyName, ESearchCase::IgnoreCase)
				|| Property->GetAuthoredName().Equals(PropertyName, ESearchCase::IgnoreCase))
			{
				return Property;
			}
		}
		return nullptr;
	}

	static int32 ApplySequenceRawPropertiesJson(
		UObject* Target,
		const TSharedPtr<FJsonObject>& SourceObj,
		const FString& FieldName,
		TArray<TSharedPtr<FJsonValue>>& Warnings,
		const FString& Context)
	{
		if (!Target || !SourceObj.IsValid())
		{
			return 0;
		}
		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (!SourceObj->TryGetArrayField(FieldName, Properties) || !Properties)
		{
			return 0;
		}

		int32 Applied = 0;
		for (const TSharedPtr<FJsonValue>& PropertyValue : *Properties)
		{
			const TSharedPtr<FJsonObject>* PropertyObj = nullptr;
			if (!PropertyValue.IsValid() || !PropertyValue->TryGetObject(PropertyObj) || !PropertyObj || !PropertyObj->IsValid())
			{
				continue;
			}

			bool bCanApplyRaw = false;
			(*PropertyObj)->TryGetBoolField(TEXT("can_apply_raw"), bCanApplyRaw);
			if (!bCanApplyRaw)
			{
				continue;
			}

			FString PropertyName;
			FString ValueText;
			if (!(*PropertyObj)->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
			{
				AddWarning(Warnings, TEXT("raw_property_missing_name"), FString::Printf(TEXT("Skipped raw property entry with no property_name for %s."), *Context));
				continue;
			}
			if (!(*PropertyObj)->TryGetStringField(TEXT("value_text"), ValueText))
			{
				AddWarning(Warnings, TEXT("raw_property_missing_value_text"), FString::Printf(TEXT("Skipped raw property '%s' for %s because value_text is missing."), *PropertyName, *Context));
				continue;
			}

			FProperty* Property = FindSequenceRawPropertyByName(Target->GetClass(), PropertyName);
			if (!Property)
			{
				AddWarning(Warnings, TEXT("raw_property_not_found"), FString::Printf(TEXT("Skipped raw property '%s' for %s because the target property was not found."), *PropertyName, *Context));
				continue;
			}
			if (!ShouldApplySequenceRawProperty(Property))
			{
				AddWarning(Warnings, TEXT("raw_property_apply_blocked"), FString::Printf(TEXT("Skipped raw property '%s' for %s because it is not allowed by the Sequence raw property safety policy."), *PropertyName, *Context));
				continue;
			}

			Target->Modify();
			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Target);
			if (!ValuePtr || Property->ImportText_Direct(*ValueText, ValuePtr, Target, PPF_None, GLog) == nullptr)
			{
				AddWarning(Warnings, TEXT("raw_property_import_failed"), FString::Printf(TEXT("ImportText failed for raw property '%s' on %s."), *PropertyName, *Context));
				continue;
			}

			FString AppliedValueText;
			Property->ExportTextItem_Direct(AppliedValueText, ValuePtr, nullptr, Target, PPF_None);
			if (!AppliedValueText.Equals(ValueText, ESearchCase::CaseSensitive))
			{
				AddWarning(Warnings, TEXT("raw_property_value_changed_after_import"), FString::Printf(TEXT("Raw property '%s' on %s read back a different value after import. requested='%s' applied='%s'."), *PropertyName, *Context, *ValueText, *AppliedValueText));
			}
			++Applied;
		}
		return Applied;
	}

	static bool TryReadChannelKeyFrame(const TSharedPtr<FJsonObject>& KeyObj, FFrameNumber& OutFrame)
	{
		if (!KeyObj.IsValid())
		{
			return false;
		}
		double FrameNumber = 0.0;
		if (!KeyObj->TryGetNumberField(TEXT("frame"), FrameNumber))
		{
			return false;
		}
		OutFrame = FFrameNumber(static_cast<int32>(FMath::RoundToInt(FrameNumber)));
		return true;
	}

	static bool TryReadNumberAlias(const TSharedPtr<FJsonObject>& Obj, const TArray<FString>& Aliases, double& OutValue)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		for (const FString& Alias : Aliases)
		{
			if (Obj->TryGetNumberField(Alias, OutValue))
			{
				return true;
			}
		}
		return false;
	}

	static bool TryGetAggregateKeyFrameOverride(const TSharedPtr<FJsonObject>& SectionObj, const int32 KeyIndex, const FFrameRate& TickResolution, FFrameNumber& OutFrame)
	{
		if (!SectionObj.IsValid() || KeyIndex < 0)
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
		if (!SectionObj->TryGetArrayField(TEXT("keys"), Keys) || !Keys)
		{
			return false;
		}
		if (!Keys->IsValidIndex(KeyIndex))
		{
			return false;
		}
		const TSharedPtr<FJsonObject>* KeyObj = nullptr;
		if (!(*Keys)[KeyIndex].IsValid() || !(*Keys)[KeyIndex]->TryGetObject(KeyObj) || !KeyObj || !KeyObj->IsValid())
		{
			return false;
		}
		double TimeSeconds = 0.0;
		if ((*KeyObj)->TryGetNumberField(TEXT("time_seconds"), TimeSeconds))
		{
			OutFrame = TickResolution.AsFrameTime(TimeSeconds).RoundToFrame();
			return true;
		}
		return TryReadChannelKeyFrame(*KeyObj, OutFrame);
	}

	static bool TryReadAggregateNumberFromKey(const TSharedPtr<FJsonObject>& KeyObj, const int32 ChannelIndex, double& OutValue)
	{
		if (!KeyObj.IsValid())
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* ObjectValue = nullptr;
		if (KeyObj->TryGetObjectField(TEXT("location"), ObjectValue) && ObjectValue && ObjectValue->IsValid() && ChannelIndex >= 0 && ChannelIndex <= 2)
		{
			static const TArray<FString> LocationAliases[] = {
				{ TEXT("x"), TEXT("X") },
				{ TEXT("y"), TEXT("Y") },
				{ TEXT("z"), TEXT("Z") }
			};
			if (TryReadNumberAlias(*ObjectValue, LocationAliases[ChannelIndex], OutValue))
			{
				return true;
			}
		}
		if (KeyObj->TryGetObjectField(TEXT("rotation"), ObjectValue) && ObjectValue && ObjectValue->IsValid() && ChannelIndex >= 3 && ChannelIndex <= 5)
		{
			static const TArray<FString> RotationAliases[] = {
				{ TEXT("roll"), TEXT("Roll") },
				{ TEXT("pitch"), TEXT("Pitch") },
				{ TEXT("yaw"), TEXT("Yaw") }
			};
			if (TryReadNumberAlias(*ObjectValue, RotationAliases[ChannelIndex - 3], OutValue))
			{
				return true;
			}
		}
		if (KeyObj->TryGetObjectField(TEXT("scale"), ObjectValue) && ObjectValue && ObjectValue->IsValid() && ChannelIndex >= 6 && ChannelIndex <= 8)
		{
			static const TArray<FString> ScaleAliases[] = {
				{ TEXT("x"), TEXT("X") },
				{ TEXT("y"), TEXT("Y") },
				{ TEXT("z"), TEXT("Z") }
			};
			if (TryReadNumberAlias(*ObjectValue, ScaleAliases[ChannelIndex - 6], OutValue))
			{
				return true;
			}
		}
		if (KeyObj->TryGetObjectField(TEXT("value"), ObjectValue) && ObjectValue && ObjectValue->IsValid())
		{
			static const TArray<FString> VectorAliases[] = {
				{ TEXT("x"), TEXT("X") },
				{ TEXT("y"), TEXT("Y") },
				{ TEXT("z"), TEXT("Z") },
				{ TEXT("w"), TEXT("W") }
			};
			static const TArray<FString> RotatorAliases[] = {
				{ TEXT("pitch"), TEXT("Pitch") },
				{ TEXT("yaw"), TEXT("Yaw") },
				{ TEXT("roll"), TEXT("Roll") }
			};
			if (ChannelIndex >= 0 && ChannelIndex < UE_ARRAY_COUNT(VectorAliases)
				&& TryReadNumberAlias(*ObjectValue, VectorAliases[ChannelIndex], OutValue))
			{
				return true;
			}
			if (ChannelIndex >= 0 && ChannelIndex < UE_ARRAY_COUNT(RotatorAliases)
				&& TryReadNumberAlias(*ObjectValue, RotatorAliases[ChannelIndex], OutValue))
			{
				return true;
			}
		}

		static const TArray<FString> ColorAliases[] = {
			{ TEXT("red"), TEXT("r"), TEXT("R") },
			{ TEXT("green"), TEXT("g"), TEXT("G") },
			{ TEXT("blue"), TEXT("b"), TEXT("B") },
			{ TEXT("alpha"), TEXT("a"), TEXT("A") }
		};
		if (ChannelIndex >= 0 && ChannelIndex < UE_ARRAY_COUNT(ColorAliases)
			&& TryReadNumberAlias(KeyObj, ColorAliases[ChannelIndex], OutValue))
		{
			return true;
		}
		return ChannelIndex == 0 && KeyObj->TryGetNumberField(TEXT("value"), OutValue);
	}

	static bool TryGetAggregateNumberOverride(const TSharedPtr<FJsonObject>& SectionObj, const int32 ChannelIndex, const int32 KeyIndex, const FFrameNumber KeyFrame, double& OutValue)
	{
		if (!SectionObj.IsValid())
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
		if (!SectionObj->TryGetArrayField(TEXT("keys"), Keys) || !Keys)
		{
			return false;
		}
		if (Keys->IsValidIndex(KeyIndex))
		{
			const TSharedPtr<FJsonObject>* IndexedKeyObj = nullptr;
			if ((*Keys)[KeyIndex].IsValid()
				&& (*Keys)[KeyIndex]->TryGetObject(IndexedKeyObj)
				&& IndexedKeyObj
				&& IndexedKeyObj->IsValid()
				&& TryReadAggregateNumberFromKey(*IndexedKeyObj, ChannelIndex, OutValue))
			{
				return true;
			}
		}
		for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
		{
			const TSharedPtr<FJsonObject>* KeyObj = nullptr;
			FFrameNumber CandidateFrame;
			if (!KeyValue.IsValid() || !KeyValue->TryGetObject(KeyObj) || !KeyObj || !KeyObj->IsValid()
				|| !TryReadChannelKeyFrame(*KeyObj, CandidateFrame)
				|| CandidateFrame != KeyFrame)
			{
				continue;
			}
			if (TryReadAggregateNumberFromKey(*KeyObj, ChannelIndex, OutValue))
			{
				return true;
			}
		}
		return false;
	}

	static bool TryGetAggregateBoolOverride(const TSharedPtr<FJsonObject>& SectionObj, const int32 ChannelIndex, const int32 KeyIndex, const FFrameNumber KeyFrame, bool& OutValue)
	{
		if (!SectionObj.IsValid() || ChannelIndex != 0)
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
		if (!SectionObj->TryGetArrayField(TEXT("keys"), Keys) || !Keys)
		{
			return false;
		}
		if (Keys->IsValidIndex(KeyIndex))
		{
			const TSharedPtr<FJsonObject>* IndexedKeyObj = nullptr;
			if ((*Keys)[KeyIndex].IsValid()
				&& (*Keys)[KeyIndex]->TryGetObject(IndexedKeyObj)
				&& IndexedKeyObj
				&& IndexedKeyObj->IsValid()
				&& (*IndexedKeyObj)->TryGetBoolField(TEXT("value"), OutValue))
			{
				return true;
			}
		}
		for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
		{
			const TSharedPtr<FJsonObject>* KeyObj = nullptr;
			FFrameNumber CandidateFrame;
			if (KeyValue.IsValid() && KeyValue->TryGetObject(KeyObj) && KeyObj && KeyObj->IsValid()
				&& TryReadChannelKeyFrame(*KeyObj, CandidateFrame)
				&& CandidateFrame == KeyFrame
				&& (*KeyObj)->TryGetBoolField(TEXT("value"), OutValue))
			{
				return true;
			}
		}
		return false;
	}

	static bool TryGetAggregateStringOverride(const TSharedPtr<FJsonObject>& SectionObj, const int32 ChannelIndex, const int32 KeyIndex, const FFrameNumber KeyFrame, FString& OutValue)
	{
		if (!SectionObj.IsValid() || ChannelIndex != 0)
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
		if (!SectionObj->TryGetArrayField(TEXT("keys"), Keys) || !Keys)
		{
			return false;
		}
		if (Keys->IsValidIndex(KeyIndex))
		{
			const TSharedPtr<FJsonObject>* IndexedKeyObj = nullptr;
			if ((*Keys)[KeyIndex].IsValid()
				&& (*Keys)[KeyIndex]->TryGetObject(IndexedKeyObj)
				&& IndexedKeyObj
				&& IndexedKeyObj->IsValid()
				&& (*IndexedKeyObj)->TryGetStringField(TEXT("value"), OutValue))
			{
				return true;
			}
		}
		for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
		{
			const TSharedPtr<FJsonObject>* KeyObj = nullptr;
			FFrameNumber CandidateFrame;
			if (KeyValue.IsValid() && KeyValue->TryGetObject(KeyObj) && KeyObj && KeyObj->IsValid()
				&& TryReadChannelKeyFrame(*KeyObj, CandidateFrame)
				&& CandidateFrame == KeyFrame
				&& (*KeyObj)->TryGetStringField(TEXT("value"), OutValue))
			{
				return true;
			}
		}
		return false;
	}

	static void ApplyCurveKeyModesJson(const TSharedPtr<FJsonObject>& KeyObj, FMovieSceneDoubleValue& Value)
	{
		if (!KeyObj.IsValid())
		{
			return;
		}
		FString ModeText;
		int64 ModeValue = 0;
		if (KeyObj->TryGetStringField(TEXT("interp_mode"), ModeText)
			&& TryParseEnumValue(StaticEnum<ERichCurveInterpMode>(), ModeText, ModeValue))
		{
			Value.InterpMode = static_cast<ERichCurveInterpMode>(ModeValue);
		}
		if (KeyObj->TryGetStringField(TEXT("tangent_mode"), ModeText)
			&& TryParseEnumValue(StaticEnum<ERichCurveTangentMode>(), ModeText, ModeValue))
		{
			Value.TangentMode = static_cast<ERichCurveTangentMode>(ModeValue);
		}
		const TSharedPtr<FJsonObject>* TangentObj = nullptr;
		if (KeyObj->TryGetObjectField(TEXT("tangent"), TangentObj) && TangentObj && TangentObj->IsValid())
		{
			ApplyTangentDataJson(*TangentObj, Value.Tangent);
		}
	}

	static void ApplyCurveKeyModesJson(const TSharedPtr<FJsonObject>& KeyObj, FMovieSceneFloatValue& Value)
	{
		if (!KeyObj.IsValid())
		{
			return;
		}
		FString ModeText;
		int64 ModeValue = 0;
		if (KeyObj->TryGetStringField(TEXT("interp_mode"), ModeText)
			&& TryParseEnumValue(StaticEnum<ERichCurveInterpMode>(), ModeText, ModeValue))
		{
			Value.InterpMode = static_cast<ERichCurveInterpMode>(ModeValue);
		}
		if (KeyObj->TryGetStringField(TEXT("tangent_mode"), ModeText)
			&& TryParseEnumValue(StaticEnum<ERichCurveTangentMode>(), ModeText, ModeValue))
		{
			Value.TangentMode = static_cast<ERichCurveTangentMode>(ModeValue);
		}
		const TSharedPtr<FJsonObject>* TangentObj = nullptr;
		if (KeyObj->TryGetObjectField(TEXT("tangent"), TangentObj) && TangentObj && TangentObj->IsValid())
		{
			ApplyTangentDataJson(*TangentObj, Value.Tangent);
		}
	}

	static bool ApplyDoubleChannelJson(FMovieSceneDoubleChannel& Channel, const TSharedPtr<FJsonObject>& ChannelObj, const TSharedPtr<FJsonObject>& SectionObj, const int32 ChannelIndex, const FFrameRate& TickResolution, TArray<TSharedPtr<FJsonValue>>& Warnings, const FString& Context)
	{
		if (!ChannelObj.IsValid())
		{
			return false;
		}
		bool bHasDefault = false;
		if (ChannelObj->TryGetBoolField(TEXT("has_default"), bHasDefault))
		{
			if (bHasDefault)
			{
				double DefaultValue = 0.0;
				if (ChannelObj->TryGetNumberField(TEXT("default_value"), DefaultValue))
				{
					Channel.SetDefault(DefaultValue);
				}
			}
			else
			{
				Channel.RemoveDefault();
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
		if (ChannelObj->TryGetArrayField(TEXT("keys"), Keys) && Keys)
		{
			TMovieSceneChannelData<FMovieSceneDoubleValue> ChannelData = Channel.GetData();
			ChannelData.Reset();
			for (int32 KeyIndex = 0; KeyIndex < Keys->Num(); ++KeyIndex)
			{
				const TSharedPtr<FJsonValue>& KeyValue = (*Keys)[KeyIndex];
				const TSharedPtr<FJsonObject>* KeyObj = nullptr;
				FFrameNumber KeyFrame;
				double NumberValue = 0.0;
				if (!KeyValue.IsValid() || !KeyValue->TryGetObject(KeyObj) || !KeyObj || !KeyObj->IsValid()
					|| !TryReadChannelKeyFrame(*KeyObj, KeyFrame)
					|| !(*KeyObj)->TryGetNumberField(TEXT("value"), NumberValue))
				{
					AddWarning(Warnings, TEXT("channel_key_invalid"), FString::Printf(TEXT("Skipped invalid double channel key for %s."), *Context));
					continue;
				}
				TryGetAggregateKeyFrameOverride(SectionObj, KeyIndex, TickResolution, KeyFrame);
				TryGetAggregateNumberOverride(SectionObj, ChannelIndex, KeyIndex, KeyFrame, NumberValue);
				FMovieSceneDoubleValue NewValue(NumberValue);
				ApplyCurveKeyModesJson(*KeyObj, NewValue);
				ChannelData.UpdateOrAddKey(KeyFrame, NewValue);
			}
		}
		return true;
	}

	static bool ApplyFloatChannelJson(FMovieSceneFloatChannel& Channel, const TSharedPtr<FJsonObject>& ChannelObj, const TSharedPtr<FJsonObject>& SectionObj, const int32 ChannelIndex, const FFrameRate& TickResolution, TArray<TSharedPtr<FJsonValue>>& Warnings, const FString& Context)
	{
		if (!ChannelObj.IsValid())
		{
			return false;
		}
		bool bHasDefault = false;
		if (ChannelObj->TryGetBoolField(TEXT("has_default"), bHasDefault))
		{
			if (bHasDefault)
			{
				double DefaultValue = 0.0;
				if (ChannelObj->TryGetNumberField(TEXT("default_value"), DefaultValue))
				{
					Channel.SetDefault(static_cast<float>(DefaultValue));
				}
			}
			else
			{
				Channel.RemoveDefault();
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
		if (ChannelObj->TryGetArrayField(TEXT("keys"), Keys) && Keys)
		{
			TMovieSceneChannelData<FMovieSceneFloatValue> ChannelData = Channel.GetData();
			ChannelData.Reset();
			for (int32 KeyIndex = 0; KeyIndex < Keys->Num(); ++KeyIndex)
			{
				const TSharedPtr<FJsonValue>& KeyValue = (*Keys)[KeyIndex];
				const TSharedPtr<FJsonObject>* KeyObj = nullptr;
				FFrameNumber KeyFrame;
				double NumberValue = 0.0;
				if (!KeyValue.IsValid() || !KeyValue->TryGetObject(KeyObj) || !KeyObj || !KeyObj->IsValid()
					|| !TryReadChannelKeyFrame(*KeyObj, KeyFrame)
					|| !(*KeyObj)->TryGetNumberField(TEXT("value"), NumberValue))
				{
					AddWarning(Warnings, TEXT("channel_key_invalid"), FString::Printf(TEXT("Skipped invalid float channel key for %s."), *Context));
					continue;
				}
				TryGetAggregateKeyFrameOverride(SectionObj, KeyIndex, TickResolution, KeyFrame);
				TryGetAggregateNumberOverride(SectionObj, ChannelIndex, KeyIndex, KeyFrame, NumberValue);
				FMovieSceneFloatValue NewValue(static_cast<float>(NumberValue));
				ApplyCurveKeyModesJson(*KeyObj, NewValue);
				ChannelData.UpdateOrAddKey(KeyFrame, NewValue);
			}
		}
		return true;
	}

	static bool ApplyBoolChannelJson(FMovieSceneBoolChannel& Channel, const TSharedPtr<FJsonObject>& ChannelObj, const TSharedPtr<FJsonObject>& SectionObj, const int32 ChannelIndex, const FFrameRate& TickResolution, TArray<TSharedPtr<FJsonValue>>& Warnings, const FString& Context)
	{
		if (!ChannelObj.IsValid())
		{
			return false;
		}
		bool bHasDefault = false;
		if (ChannelObj->TryGetBoolField(TEXT("has_default"), bHasDefault))
		{
			if (bHasDefault)
			{
				bool DefaultValue = false;
				if (ChannelObj->TryGetBoolField(TEXT("default_value"), DefaultValue))
				{
					Channel.SetDefault(DefaultValue);
				}
			}
			else
			{
				Channel.RemoveDefault();
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
		if (ChannelObj->TryGetArrayField(TEXT("keys"), Keys) && Keys)
		{
			TMovieSceneChannelData<bool> ChannelData = Channel.GetData();
			ChannelData.Reset();
			for (int32 KeyIndex = 0; KeyIndex < Keys->Num(); ++KeyIndex)
			{
				const TSharedPtr<FJsonValue>& KeyValue = (*Keys)[KeyIndex];
				const TSharedPtr<FJsonObject>* KeyObj = nullptr;
				FFrameNumber KeyFrame;
				bool BoolValue = false;
				if (!KeyValue.IsValid() || !KeyValue->TryGetObject(KeyObj) || !KeyObj || !KeyObj->IsValid()
					|| !TryReadChannelKeyFrame(*KeyObj, KeyFrame)
					|| !(*KeyObj)->TryGetBoolField(TEXT("value"), BoolValue))
				{
					AddWarning(Warnings, TEXT("channel_key_invalid"), FString::Printf(TEXT("Skipped invalid bool channel key for %s."), *Context));
					continue;
				}
				TryGetAggregateKeyFrameOverride(SectionObj, KeyIndex, TickResolution, KeyFrame);
				TryGetAggregateBoolOverride(SectionObj, ChannelIndex, KeyIndex, KeyFrame, BoolValue);
				ChannelData.UpdateOrAddKey(KeyFrame, BoolValue);
			}
		}
		return true;
	}

	template<typename ChannelType, typename ValueType>
	static bool ApplyDiscreteNumberChannelJson(ChannelType& Channel, const TSharedPtr<FJsonObject>& ChannelObj, const TSharedPtr<FJsonObject>& SectionObj, const int32 ChannelIndex, const FFrameRate& TickResolution, TArray<TSharedPtr<FJsonValue>>& Warnings, const FString& Context, const FString& ChannelTypeName)
	{
		if (!ChannelObj.IsValid())
		{
			return false;
		}
		bool bHasDefault = false;
		if (ChannelObj->TryGetBoolField(TEXT("has_default"), bHasDefault))
		{
			if (bHasDefault)
			{
				double DefaultValue = 0.0;
				if (ChannelObj->TryGetNumberField(TEXT("default_value"), DefaultValue))
				{
					Channel.SetDefault(static_cast<ValueType>(FMath::RoundToInt(DefaultValue)));
				}
			}
			else
			{
				Channel.RemoveDefault();
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
		if (ChannelObj->TryGetArrayField(TEXT("keys"), Keys) && Keys)
		{
			TMovieSceneChannelData<ValueType> ChannelData = Channel.GetData();
			ChannelData.Reset();
			for (int32 KeyIndex = 0; KeyIndex < Keys->Num(); ++KeyIndex)
			{
				const TSharedPtr<FJsonValue>& KeyValue = (*Keys)[KeyIndex];
				const TSharedPtr<FJsonObject>* KeyObj = nullptr;
				FFrameNumber KeyFrame;
				double NumberValue = 0.0;
				if (!KeyValue.IsValid() || !KeyValue->TryGetObject(KeyObj) || !KeyObj || !KeyObj->IsValid()
					|| !TryReadChannelKeyFrame(*KeyObj, KeyFrame)
					|| !(*KeyObj)->TryGetNumberField(TEXT("value"), NumberValue))
				{
					AddWarning(Warnings, TEXT("channel_key_invalid"), FString::Printf(TEXT("Skipped invalid %s channel key for %s."), *ChannelTypeName, *Context));
					continue;
				}
				TryGetAggregateKeyFrameOverride(SectionObj, KeyIndex, TickResolution, KeyFrame);
				TryGetAggregateNumberOverride(SectionObj, ChannelIndex, KeyIndex, KeyFrame, NumberValue);
				ChannelData.UpdateOrAddKey(KeyFrame, static_cast<ValueType>(FMath::RoundToInt(NumberValue)));
			}
		}
		return true;
	}

	static bool ApplyByteChannelJson(FMovieSceneByteChannel& Channel, const TSharedPtr<FJsonObject>& ChannelObj, const TSharedPtr<FJsonObject>& SectionObj, const int32 ChannelIndex, const FFrameRate& TickResolution, TArray<TSharedPtr<FJsonValue>>& Warnings, const FString& Context)
	{
		if (!ChannelObj.IsValid())
		{
			return false;
		}
		bool bHasDefault = false;
		if (ChannelObj->TryGetBoolField(TEXT("has_default"), bHasDefault))
		{
			if (bHasDefault)
			{
				double DefaultValue = 0.0;
				if (ChannelObj->TryGetNumberField(TEXT("default_value"), DefaultValue))
				{
					Channel.SetDefault(static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(DefaultValue), 0, 255)));
				}
			}
			else
			{
				Channel.RemoveDefault();
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
		if (ChannelObj->TryGetArrayField(TEXT("keys"), Keys) && Keys)
		{
			TMovieSceneChannelData<uint8> ChannelData = Channel.GetData();
			ChannelData.Reset();
			for (int32 KeyIndex = 0; KeyIndex < Keys->Num(); ++KeyIndex)
			{
				const TSharedPtr<FJsonValue>& KeyValue = (*Keys)[KeyIndex];
				const TSharedPtr<FJsonObject>* KeyObj = nullptr;
				FFrameNumber KeyFrame;
				double NumberValue = 0.0;
				if (!KeyValue.IsValid() || !KeyValue->TryGetObject(KeyObj) || !KeyObj || !KeyObj->IsValid()
					|| !TryReadChannelKeyFrame(*KeyObj, KeyFrame)
					|| !(*KeyObj)->TryGetNumberField(TEXT("value"), NumberValue))
				{
					AddWarning(Warnings, TEXT("channel_key_invalid"), FString::Printf(TEXT("Skipped invalid byte channel key for %s."), *Context));
					continue;
				}
				TryGetAggregateKeyFrameOverride(SectionObj, KeyIndex, TickResolution, KeyFrame);
				TryGetAggregateNumberOverride(SectionObj, ChannelIndex, KeyIndex, KeyFrame, NumberValue);
				ChannelData.UpdateOrAddKey(KeyFrame, static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(NumberValue), 0, 255)));
			}
		}
		return true;
	}

	static bool ApplyStringChannelJson(FMovieSceneStringChannel& Channel, const TSharedPtr<FJsonObject>& ChannelObj, const TSharedPtr<FJsonObject>& SectionObj, const int32 ChannelIndex, const FFrameRate& TickResolution, TArray<TSharedPtr<FJsonValue>>& Warnings, const FString& Context)
	{
		if (!ChannelObj.IsValid())
		{
			return false;
		}
		bool bHasDefault = false;
		if (ChannelObj->TryGetBoolField(TEXT("has_default"), bHasDefault))
		{
			if (bHasDefault)
			{
				FString DefaultValue;
				if (ChannelObj->TryGetStringField(TEXT("default_value"), DefaultValue))
				{
					Channel.SetDefault(DefaultValue);
				}
			}
			else
			{
				Channel.RemoveDefault();
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
		if (ChannelObj->TryGetArrayField(TEXT("keys"), Keys) && Keys)
		{
			TMovieSceneChannelData<FString> ChannelData = Channel.GetData();
			ChannelData.Reset();
			for (int32 KeyIndex = 0; KeyIndex < Keys->Num(); ++KeyIndex)
			{
				const TSharedPtr<FJsonValue>& KeyValue = (*Keys)[KeyIndex];
				const TSharedPtr<FJsonObject>* KeyObj = nullptr;
				FFrameNumber KeyFrame;
				FString StringValue;
				if (!KeyValue.IsValid() || !KeyValue->TryGetObject(KeyObj) || !KeyObj || !KeyObj->IsValid()
					|| !TryReadChannelKeyFrame(*KeyObj, KeyFrame)
					|| !(*KeyObj)->TryGetStringField(TEXT("value"), StringValue))
				{
					AddWarning(Warnings, TEXT("channel_key_invalid"), FString::Printf(TEXT("Skipped invalid string channel key for %s."), *Context));
					continue;
				}
				TryGetAggregateKeyFrameOverride(SectionObj, KeyIndex, TickResolution, KeyFrame);
				TryGetAggregateStringOverride(SectionObj, ChannelIndex, KeyIndex, KeyFrame, StringValue);
				ChannelData.UpdateOrAddKey(KeyFrame, StringValue);
			}
		}
		return true;
	}

	static int32 ApplySectionChannelsJson(
		UMovieScene* MovieScene,
		UMovieSceneSection* Section,
		const TSharedPtr<FJsonObject>& SectionObj,
		TArray<TSharedPtr<FJsonValue>>& Warnings,
		const FString& Context)
	{
		if (!MovieScene || !Section || !SectionObj.IsValid())
		{
			return 0;
		}
		const TArray<TSharedPtr<FJsonValue>>* Channels = nullptr;
		if (!SectionObj->TryGetArrayField(TEXT("channels"), Channels) || !Channels)
		{
			return 0;
		}

		int32 Applied = 0;
		const FFrameRate TickResolution = MovieScene->GetTickResolution();
		for (const TSharedPtr<FJsonValue>& ChannelValue : *Channels)
		{
			const TSharedPtr<FJsonObject>* ChannelObj = nullptr;
			if (!ChannelValue.IsValid() || !ChannelValue->TryGetObject(ChannelObj) || !ChannelObj || !ChannelObj->IsValid())
			{
				continue;
			}
			FString ChannelType;
			double ChannelIndexNumber = 0.0;
			if (!(*ChannelObj)->TryGetStringField(TEXT("channel_type"), ChannelType)
				|| !(*ChannelObj)->TryGetNumberField(TEXT("channel_index"), ChannelIndexNumber))
			{
				AddWarning(Warnings, TEXT("channel_missing_identity"), FString::Printf(TEXT("Skipped channel entry with no type/index for %s."), *Context));
				continue;
			}
			const int32 ChannelIndex = FMath::RoundToInt(ChannelIndexNumber);
			Section->Modify();

			if (ChannelType.Equals(TEXT("double"), ESearchCase::IgnoreCase))
			{
				TArrayView<FMovieSceneDoubleChannel*> TypedChannels = Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
				if (!TypedChannels.IsValidIndex(ChannelIndex) || !TypedChannels[ChannelIndex])
				{
					AddWarning(Warnings, TEXT("channel_index_out_of_range"), FString::Printf(TEXT("Skipped double channel %d for %s."), ChannelIndex, *Context));
					continue;
				}
				if (ApplyDoubleChannelJson(*TypedChannels[ChannelIndex], *ChannelObj, SectionObj, ChannelIndex, TickResolution, Warnings, Context)) ++Applied;
			}
			else if (ChannelType.Equals(TEXT("float"), ESearchCase::IgnoreCase))
			{
				TArrayView<FMovieSceneFloatChannel*> TypedChannels = Section->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>();
				if (!TypedChannels.IsValidIndex(ChannelIndex) || !TypedChannels[ChannelIndex])
				{
					AddWarning(Warnings, TEXT("channel_index_out_of_range"), FString::Printf(TEXT("Skipped float channel %d for %s."), ChannelIndex, *Context));
					continue;
				}
				if (ApplyFloatChannelJson(*TypedChannels[ChannelIndex], *ChannelObj, SectionObj, ChannelIndex, TickResolution, Warnings, Context)) ++Applied;
			}
			else if (ChannelType.Equals(TEXT("bool"), ESearchCase::IgnoreCase))
			{
				TArrayView<FMovieSceneBoolChannel*> TypedChannels = Section->GetChannelProxy().GetChannels<FMovieSceneBoolChannel>();
				if (!TypedChannels.IsValidIndex(ChannelIndex) || !TypedChannels[ChannelIndex])
				{
					AddWarning(Warnings, TEXT("channel_index_out_of_range"), FString::Printf(TEXT("Skipped bool channel %d for %s."), ChannelIndex, *Context));
					continue;
				}
				if (ApplyBoolChannelJson(*TypedChannels[ChannelIndex], *ChannelObj, SectionObj, ChannelIndex, TickResolution, Warnings, Context)) ++Applied;
			}
			else if (ChannelType.Equals(TEXT("integer"), ESearchCase::IgnoreCase))
			{
				TArrayView<FMovieSceneIntegerChannel*> TypedChannels = Section->GetChannelProxy().GetChannels<FMovieSceneIntegerChannel>();
				if (!TypedChannels.IsValidIndex(ChannelIndex) || !TypedChannels[ChannelIndex])
				{
					AddWarning(Warnings, TEXT("channel_index_out_of_range"), FString::Printf(TEXT("Skipped integer channel %d for %s."), ChannelIndex, *Context));
					continue;
				}
				if (ApplyDiscreteNumberChannelJson<FMovieSceneIntegerChannel, int32>(*TypedChannels[ChannelIndex], *ChannelObj, SectionObj, ChannelIndex, TickResolution, Warnings, Context, TEXT("integer"))) ++Applied;
			}
			else if (ChannelType.Equals(TEXT("byte"), ESearchCase::IgnoreCase))
			{
				TArrayView<FMovieSceneByteChannel*> TypedChannels = Section->GetChannelProxy().GetChannels<FMovieSceneByteChannel>();
				if (!TypedChannels.IsValidIndex(ChannelIndex) || !TypedChannels[ChannelIndex])
				{
					AddWarning(Warnings, TEXT("channel_index_out_of_range"), FString::Printf(TEXT("Skipped byte channel %d for %s."), ChannelIndex, *Context));
					continue;
				}
				if (ApplyByteChannelJson(*TypedChannels[ChannelIndex], *ChannelObj, SectionObj, ChannelIndex, TickResolution, Warnings, Context)) ++Applied;
			}
			else if (ChannelType.Equals(TEXT("string"), ESearchCase::IgnoreCase))
			{
				TArrayView<FMovieSceneStringChannel*> TypedChannels = Section->GetChannelProxy().GetChannels<FMovieSceneStringChannel>();
				if (!TypedChannels.IsValidIndex(ChannelIndex) || !TypedChannels[ChannelIndex])
				{
					AddWarning(Warnings, TEXT("channel_index_out_of_range"), FString::Printf(TEXT("Skipped string channel %d for %s."), ChannelIndex, *Context));
					continue;
				}
				if (ApplyStringChannelJson(*TypedChannels[ChannelIndex], *ChannelObj, SectionObj, ChannelIndex, TickResolution, Warnings, Context)) ++Applied;
			}
			else
			{
				AddWarning(Warnings, TEXT("channel_type_not_supported"), FString::Printf(TEXT("Skipped unsupported channel type '%s' for %s."), *ChannelType, *Context));
			}
		}
		return Applied;
	}

	static void ApplySectionMetadataJson(
		UMovieScene* MovieScene,
		UMovieSceneSection* Section,
		const TSharedPtr<FJsonObject>& SectionObj,
		TArray<TSharedPtr<FJsonValue>>& Warnings,
		const FString& Context,
		int32* RawPropertiesApplied = nullptr,
		int32* ChannelsApplied = nullptr)
	{
		if (!MovieScene || !Section || !SectionObj.IsValid())
		{
			return;
		}

		const TSharedPtr<FJsonObject>* MetadataPtr = nullptr;
		if (!SectionObj->TryGetObjectField(TEXT("section_metadata"), MetadataPtr) || !MetadataPtr || !MetadataPtr->IsValid())
		{
			return;
		}
		const TSharedPtr<FJsonObject>& MetadataObj = *MetadataPtr;

		bool bHasStart = false;
		bool bHasEnd = false;
		MetadataObj->TryGetBoolField(TEXT("has_start_frame"), bHasStart);
		MetadataObj->TryGetBoolField(TEXT("has_end_frame"), bHasEnd);

		double StartFrameNumber = 0.0;
		double EndFrameNumber = 0.0;
		const bool bStartFrameAvailable = MetadataObj->TryGetNumberField(TEXT("start_frame"), StartFrameNumber);
		const bool bEndFrameAvailable = MetadataObj->TryGetNumberField(TEXT("end_frame"), EndFrameNumber);

		if (bHasStart == bStartFrameAvailable && bHasEnd == bEndFrameAvailable)
		{
			TRangeBound<FFrameNumber> LowerBound = bHasStart
				? TRangeBound<FFrameNumber>::Inclusive(FFrameNumber(static_cast<int32>(FMath::RoundToInt(StartFrameNumber))))
				: TRangeBound<FFrameNumber>::Open();
			TRangeBound<FFrameNumber> UpperBound = bHasEnd
				? TRangeBound<FFrameNumber>::Exclusive(FFrameNumber(static_cast<int32>(FMath::RoundToInt(EndFrameNumber))))
				: TRangeBound<FFrameNumber>::Open();
			if (bHasStart && bHasEnd && UpperBound.GetValue() < LowerBound.GetValue())
			{
				AddWarning(Warnings, TEXT("section_metadata_invalid_range"), FString::Printf(TEXT("Skipped invalid section range metadata for %s."), *Context));
			}
			else
			{
				Section->SetRange(TRange<FFrameNumber>(LowerBound, UpperBound));
			}
		}

		double NumberValue = 0.0;
		if (MetadataObj->TryGetNumberField(TEXT("row_index"), NumberValue))
		{
			Section->SetRowIndex(FMath::RoundToInt(NumberValue));
		}
		if (MetadataObj->TryGetNumberField(TEXT("overlap_priority"), NumberValue))
		{
			Section->SetOverlapPriority(FMath::RoundToInt(NumberValue));
		}
		if (MetadataObj->TryGetNumberField(TEXT("pre_roll_frames"), NumberValue))
		{
			Section->SetPreRollFrames(FMath::Max(0, FMath::RoundToInt(NumberValue)));
		}
		if (MetadataObj->TryGetNumberField(TEXT("post_roll_frames"), NumberValue))
		{
			Section->SetPostRollFrames(FMath::Max(0, FMath::RoundToInt(NumberValue)));
		}

		bool BoolValue = false;
		if (MetadataObj->TryGetBoolField(TEXT("is_active"), BoolValue))
		{
			Section->SetIsActive(BoolValue);
		}
		if (MetadataObj->TryGetBoolField(TEXT("is_locked"), BoolValue))
		{
			Section->SetIsLocked(BoolValue);
		}

		const TSharedPtr<FJsonObject>* ColorObj = nullptr;
		if (MetadataObj->TryGetObjectField(TEXT("color_tint"), ColorObj) && ColorObj && ColorObj->IsValid())
		{
			FColor ColorTint;
			if (TryParseSectionColorJson(*ColorObj, ColorTint))
			{
				Section->SetColorTint(ColorTint);
			}
		}

		const int32 SectionRawPropertiesApplied = ApplySequenceRawPropertiesJson(Section, SectionObj, TEXT("raw_properties"), Warnings, Context);
		if (RawPropertiesApplied)
		{
			*RawPropertiesApplied += SectionRawPropertiesApplied;
		}
		const int32 SectionChannelsApplied = ApplySectionChannelsJson(MovieScene, Section, SectionObj, Warnings, Context);
		if (ChannelsApplied)
		{
			*ChannelsApplied += SectionChannelsApplied;
		}
	}

	static void ApplyFirstSectionMetadataJson(
		UMovieScene* MovieScene,
		UMovieSceneTrack* Track,
		const TSharedPtr<FJsonObject>& SectionObj,
		TArray<TSharedPtr<FJsonValue>>& Warnings,
		const FString& Context,
		int32* RawPropertiesApplied = nullptr,
		int32* ChannelsApplied = nullptr)
	{
		if (!MovieScene || !Track || !SectionObj.IsValid())
		{
			return;
		}

		const TArray<UMovieSceneSection*>& Sections = Track->GetAllSections();
		if (Sections.Num() > 0 && Sections[0])
		{
			ApplySectionMetadataJson(MovieScene, Sections[0], SectionObj, Warnings, Context, RawPropertiesApplied, ChannelsApplied);
		}
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

	static bool TryReadControlRigTransform(const TSharedPtr<FJsonObject>& Obj, FTransform& OutValue, const bool bRequireScale)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		FVector Scale = FVector::OneVector;
		const TSharedPtr<FJsonObject>* LocationObj = nullptr;
		const TSharedPtr<FJsonObject>* RotationObj = nullptr;
		const TSharedPtr<FJsonObject>* ScaleObj = nullptr;
		const bool bHasLocation = Obj->TryGetObjectField(TEXT("location"), LocationObj) && LocationObj && TryReadControlRigVector(*LocationObj, Location);
		const bool bHasRotation = Obj->TryGetObjectField(TEXT("rotation"), RotationObj) && RotationObj && TryReadControlRigRotator(*RotationObj, Rotation);
		const bool bHasScale = Obj->TryGetObjectField(TEXT("scale"), ScaleObj) && ScaleObj && TryReadControlRigVector(*ScaleObj, Scale);
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

	static TSharedPtr<FJsonObject> BuildTrackDiagnosticsJson(UMovieScene* MovieScene, UMovieSceneTrack* Track, const TSharedPtr<FJsonObject>& TrackObj)
	{
		if (!MovieScene || !Track || !TrackObj.IsValid())
		{
			return MakeShared<FJsonObject>();
		}

		FString TrackType;
		TrackObj->TryGetStringField(TEXT("track_type"), TrackType);

		const int32 SourceSectionCount = Track->GetAllSections().Num();
		const int32 ExportedSectionCount = CountTrackJsonSections(TrackObj);
		TArray<FString> Notes;
		FString RoundtripStatus = TEXT("partial_roundtrip");
		FString ApplySupport = TEXT("supported_partial");

		if (TrackType.Equals(TEXT("unsupported"), ESearchCase::IgnoreCase))
		{
			RoundtripStatus = TEXT("readonly_summary");
			ApplySupport = TEXT("unsupported");
			Notes.Add(TEXT("Track class is exported as summary only and is not applied by sequence_apply_folder."));
		}
		else
		{
			Notes.Add(TEXT("Common section metadata is exported and reapplied through section_metadata: range, row index, overlap priority, active/locked, pre/post roll and color tint."));
			Notes.Add(TEXT("Advanced section metadata is still not fully preserved: easing, blend type, condition, editor-only display state and custom time-warp details are partial or summary-only."));
			Notes.Add(TEXT("Common channel defaults, key interpolation, tangents and weights are serialized in section channels[]; non-common channel types, structural references and editor-only state may still require dedicated schema."));

			if (SourceSectionCount > ExportedSectionCount)
			{
				Notes.Add(TEXT("Source track has more sections than the folder JSON exports; apply may flatten or keep only the first section."));
			}

			if (TrackType.Equals(TEXT("transform"), ESearchCase::IgnoreCase)
				|| TrackType.Equals(TEXT("spawn"), ESearchCase::IgnoreCase)
				|| TrackType.Equals(TEXT("visibility"), ESearchCase::IgnoreCase)
				|| TrackType.Equals(TEXT("property"), ESearchCase::IgnoreCase))
			{
				Notes.Add(TEXT("This track family is primarily key/value authoring; complex multi-section layout is not a lossless round-trip target yet."));
			}
			else if (TrackType.Equals(TEXT("skeletal_animation"), ESearchCase::IgnoreCase))
			{
				Notes.Add(TEXT("Skeletal animation apply supports common section fields but does not preserve every UE time-warp, blend and animation parameter."));
				const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
				if (TrackObj->TryGetArrayField(TEXT("sections"), Sections) && Sections)
				{
					for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
					{
						const TSharedPtr<FJsonObject>* SectionObj = nullptr;
						if (SectionValue.IsValid() && SectionValue->TryGetObject(SectionObj) && SectionObj && SectionObj->IsValid())
						{
							FString PlayRateType;
							if ((*SectionObj)->TryGetStringField(TEXT("play_rate_type"), PlayRateType) && PlayRateType.Equals(TEXT("custom"), ESearchCase::IgnoreCase))
							{
								Notes.Add(TEXT("Custom skeletal animation play-rate/time-warp is exported but not reapplied losslessly."));
								break;
							}
						}
					}
				}
			}
			else if (TrackType.Equals(TEXT("control_rig"), ESearchCase::IgnoreCase))
			{
				Notes.Add(TEXT("Control Rig apply writes control keys but does not serialize full Sequencer evaluation context, control defaults or bake state."));
				if (SourceSectionCount > 1)
				{
					Notes.Add(TEXT("Multiple Control Rig sections are flattened into one applied section."));
				}
			}
			else if (TrackType.Equals(TEXT("sub_sequence"), ESearchCase::IgnoreCase) || TrackType.Equals(TEXT("cinematic_shot"), ESearchCase::IgnoreCase))
			{
				Notes.Add(TEXT("Sub sequence apply supports common section parameters; custom time scale/time-warp is not lossless."));
			}
			else if (TrackType.Equals(TEXT("camera_cut"), ESearchCase::IgnoreCase))
			{
				Notes.Add(TEXT("Camera cut apply supports binding, range and lock_previous_camera, not every editor-only section detail."));
			}
		}

		TSharedPtr<FJsonObject> Diagnostics = MakeShared<FJsonObject>();
		Diagnostics->SetStringField(TEXT("ue_class"), Track->GetClass() ? Track->GetClass()->GetPathName() : TEXT(""));
		Diagnostics->SetStringField(TEXT("roundtrip_status"), RoundtripStatus);
		Diagnostics->SetStringField(TEXT("apply_support"), ApplySupport);
		Diagnostics->SetBoolField(TEXT("is_lossless_roundtrip"), RoundtripStatus.Equals(TEXT("full_roundtrip"), ESearchCase::IgnoreCase));
		Diagnostics->SetBoolField(TEXT("roundtrip_lossy"), !RoundtripStatus.Equals(TEXT("full_roundtrip"), ESearchCase::IgnoreCase));
		Diagnostics->SetNumberField(TEXT("source_section_count"), SourceSectionCount);
		Diagnostics->SetNumberField(TEXT("exported_section_count"), ExportedSectionCount);
		Diagnostics->SetNumberField(TEXT("exported_key_count"), CountTrackJsonKeys(TrackObj));
		SetStringArrayField(Diagnostics, TEXT("roundtrip_notes"), Notes);
		return Diagnostics;
	}

	static void AnnotateTrackRoundtrip(UMovieScene* MovieScene, UMovieSceneTrack* Track, const TSharedPtr<FJsonObject>& TrackObj)
	{
		if (!MovieScene || !Track || !TrackObj.IsValid())
		{
			return;
		}

		TrackObj->SetStringField(TEXT("ue_class"), Track->GetClass() ? Track->GetClass()->GetPathName() : TEXT(""));
		TrackObj->SetArrayField(TEXT("raw_properties"), BuildSequenceRawPropertiesJson(Track));
	}

	static TSharedPtr<FJsonObject> BuildTrackJson(UMovieScene* MovieScene, UMovieSceneTrack* Track)
	{
		if (!MovieScene || !Track)
		{
			return nullptr;
		}

		auto Finish = [MovieScene, Track](const TSharedPtr<FJsonObject>& TrackObj) -> TSharedPtr<FJsonObject>
		{
			AnnotateTrackRoundtrip(MovieScene, Track, TrackObj);
			return TrackObj;
		};

		if (UMovieScene3DTransformTrack* TransformTrack = Cast<UMovieScene3DTransformTrack>(Track))
		{
			return Finish(BuildTransformTrackJson(TransformTrack, MovieScene));
		}
		if (UMovieSceneSpawnTrack* SpawnTrack = Cast<UMovieSceneSpawnTrack>(Track))
		{
			return Finish(BuildSpawnTrackJson(SpawnTrack, MovieScene));
		}
		if (UMovieSceneSkeletalAnimationTrack* SkeletalAnimationTrack = Cast<UMovieSceneSkeletalAnimationTrack>(Track))
		{
			return Finish(BuildSkeletalAnimationTrackJson(SkeletalAnimationTrack, MovieScene));
		}
		if (UMovieSceneControlRigParameterTrack* ControlRigTrack = Cast<UMovieSceneControlRigParameterTrack>(Track))
		{
			return Finish(BuildControlRigTrackJson(ControlRigTrack, MovieScene));
		}
		if (UMovieSceneCameraCutTrack* CameraCutTrack = Cast<UMovieSceneCameraCutTrack>(Track))
		{
			return Finish(BuildCameraCutTrackJson(CameraCutTrack, MovieScene));
		}
		if (UMovieSceneCinematicShotTrack* CinematicShotTrack = Cast<UMovieSceneCinematicShotTrack>(Track))
		{
			return Finish(BuildSubSequenceTrackJson(CinematicShotTrack, MovieScene, true));
		}
		if (UMovieSceneSubTrack* SubTrack = Cast<UMovieSceneSubTrack>(Track))
		{
			return Finish(BuildSubSequenceTrackJson(SubTrack, MovieScene, false));
		}
		if (UMovieSceneVisibilityTrack* VisibilityTrack = Cast<UMovieSceneVisibilityTrack>(Track))
		{
			UMovieSceneVisibilitySection* VisibilitySection = VisibilityTrack->GetAllSections().Num() > 0 ? Cast<UMovieSceneVisibilitySection>(VisibilityTrack->GetAllSections()[0]) : nullptr;
			return Finish(BuildVisibilityTrackJson(VisibilityTrack, VisibilitySection, MovieScene));
		}
		if (UMovieSceneFloatTrack* FloatTrack = Cast<UMovieSceneFloatTrack>(Track))
		{
			UMovieSceneFloatSection* Section = FloatTrack->GetAllSections().Num() > 0 ? Cast<UMovieSceneFloatSection>(FloatTrack->GetAllSections()[0]) : nullptr;
			return Finish(BuildScalarPropertyTrackJson<UMovieSceneFloatSection, FMovieSceneFloatChannel, float>(
				FloatTrack, Section, MovieScene, TEXT("float"),
				[](TSharedPtr<FJsonObject>& KeyObj, const float& Value) { KeyObj->SetNumberField(TEXT("value"), Value); },
				[](const FMovieSceneFloatChannel& Channel, TMap<int32, float>& Values) { BuildFloatKeyMap(Channel, Values); }));
		}
		if (UMovieSceneDoubleTrack* DoubleTrack = Cast<UMovieSceneDoubleTrack>(Track))
		{
			UMovieSceneDoubleSection* Section = DoubleTrack->GetAllSections().Num() > 0 ? Cast<UMovieSceneDoubleSection>(DoubleTrack->GetAllSections()[0]) : nullptr;
			return Finish(BuildScalarPropertyTrackJson<UMovieSceneDoubleSection, FMovieSceneDoubleChannel, double>(
				DoubleTrack, Section, MovieScene, TEXT("double"),
				[](TSharedPtr<FJsonObject>& KeyObj, const double& Value) { KeyObj->SetNumberField(TEXT("value"), Value); },
				[](const FMovieSceneDoubleChannel& Channel, TMap<int32, double>& Values) { BuildDoubleKeyMap(Channel, Values); }));
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
			AddSectionMetadataJson(SectionObj, MovieScene, Section);
			TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
			TrackObj->SetStringField(TEXT("track_type"), TEXT("property"));
			TrackObj->SetStringField(TEXT("property_type"), TEXT("bool"));
			TrackObj->SetStringField(TEXT("property_name"), BoolTrack->GetPropertyName().ToString());
			TrackObj->SetStringField(TEXT("property_path"), BoolTrack->GetPropertyPath().ToString());
			TrackObj->SetArrayField(TEXT("sections"), { MakeShared<FJsonValueObject>(SectionObj) });
			return Finish(TrackObj);
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
			AddSectionMetadataJson(SectionObj, MovieScene, Section);
			TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
			TrackObj->SetStringField(TEXT("track_type"), TEXT("property"));
			TrackObj->SetStringField(TEXT("property_type"), TEXT("integer"));
			TrackObj->SetStringField(TEXT("property_name"), IntegerTrack->GetPropertyName().ToString());
			TrackObj->SetStringField(TEXT("property_path"), IntegerTrack->GetPropertyPath().ToString());
			TrackObj->SetArrayField(TEXT("sections"), { MakeShared<FJsonValueObject>(SectionObj) });
			return Finish(TrackObj);
		}
		if (UMovieSceneColorTrack* ColorTrack = Cast<UMovieSceneColorTrack>(Track))
		{
			UMovieSceneColorSection* Section = ColorTrack->GetAllSections().Num() > 0 ? Cast<UMovieSceneColorSection>(ColorTrack->GetAllSections()[0]) : nullptr;
			return Finish(BuildColorTrackJson(ColorTrack, Section, MovieScene));
		}
		if (UMovieSceneByteTrack* ByteTrack = Cast<UMovieSceneByteTrack>(Track))
		{
			return Finish(BuildByteTrackJson(ByteTrack, MovieScene));
		}
		if (UMovieSceneStringTrack* StringTrack = Cast<UMovieSceneStringTrack>(Track))
		{
			return Finish(BuildStringTrackJson(StringTrack, MovieScene));
		}
		if (UMovieSceneFloatVectorTrack* FloatVectorTrack = Cast<UMovieSceneFloatVectorTrack>(Track))
		{
			UMovieSceneFloatVectorSection* Section = FloatVectorTrack->GetAllSections().Num() > 0 ? Cast<UMovieSceneFloatVectorSection>(FloatVectorTrack->GetAllSections()[0]) : nullptr;
			return Finish(BuildVectorTrackJson(FloatVectorTrack, Section, MovieScene, TEXT("float"), FloatVectorTrack->GetNumChannelsUsed()));
		}
		if (UMovieSceneDoubleVectorTrack* DoubleVectorTrack = Cast<UMovieSceneDoubleVectorTrack>(Track))
		{
			UMovieSceneDoubleVectorSection* Section = DoubleVectorTrack->GetAllSections().Num() > 0 ? Cast<UMovieSceneDoubleVectorSection>(DoubleVectorTrack->GetAllSections()[0]) : nullptr;
			return Finish(BuildVectorTrackJson(DoubleVectorTrack, Section, MovieScene, TEXT("double"), DoubleVectorTrack->GetNumChannelsUsed()));
		}
		if (UMovieSceneRotatorTrack* RotatorTrack = Cast<UMovieSceneRotatorTrack>(Track))
		{
			return Finish(BuildRotatorTrackJson(RotatorTrack, MovieScene));
		}
		if (UMovieSceneActorReferenceTrack* ActorReferenceTrack = Cast<UMovieSceneActorReferenceTrack>(Track))
		{
			return Finish(BuildActorReferenceTrackJson(ActorReferenceTrack, MovieScene));
		}
		if (UMovieSceneObjectPropertyTrack* ObjectTrack = Cast<UMovieSceneObjectPropertyTrack>(Track))
		{
			return Finish(BuildObjectPropertyTrackJson(ObjectTrack, MovieScene));
		}

		return Finish(BuildUnsupportedTrackJson(Track, MovieScene));
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

	struct FSequenceFolderCoverageStats
	{
		int32 TotalTrackCount = 0;
		int32 BindingTrackCount = 0;
		int32 MasterTrackCount = 0;
		int32 FullRoundtripTrackCount = 0;
		int32 PartialRoundtripTrackCount = 0;
		int32 ReadonlySummaryTrackCount = 0;
		int32 UnsupportedTrackCount = 0;
		TArray<TSharedPtr<FJsonValue>> Tracks;
	};

	static void RecordTrackCoverage(
		FSequenceFolderCoverageStats& Stats,
		UMovieScene* MovieScene,
		UMovieSceneTrack* Track,
		const FString& Scope,
		const FString& BindingGuid,
		const FString& BindingName,
		const FString& TrackFileName,
		const TSharedPtr<FJsonObject>& TrackObj)
	{
		if (!TrackObj.IsValid())
		{
			return;
		}

		FString TrackType;
		TrackObj->TryGetStringField(TEXT("track_type"), TrackType);
		FString PropertyType;
		TrackObj->TryGetStringField(TEXT("property_type"), PropertyType);
		TSharedPtr<FJsonObject> Diagnostics = BuildTrackDiagnosticsJson(MovieScene, Track, TrackObj);
		FString RoundtripStatus;
		if (!Diagnostics.IsValid() || !Diagnostics->TryGetStringField(TEXT("roundtrip_status"), RoundtripStatus) || RoundtripStatus.IsEmpty())
		{
			RoundtripStatus = GuessRoundtripStatusFromTrackJson(TrackObj);
		}
		FString ApplySupport;
		if (!Diagnostics.IsValid() || !Diagnostics->TryGetStringField(TEXT("apply_support"), ApplySupport) || ApplySupport.IsEmpty())
		{
			ApplySupport = GuessApplySupportFromTrackJson(TrackObj);
		}
		FString UeClass;
		if (Diagnostics.IsValid())
		{
			Diagnostics->TryGetStringField(TEXT("ue_class"), UeClass);
		}

		++Stats.TotalTrackCount;
		if (Scope.Equals(TEXT("master"), ESearchCase::IgnoreCase))
		{
			++Stats.MasterTrackCount;
		}
		else
		{
			++Stats.BindingTrackCount;
		}

		if (RoundtripStatus.Equals(TEXT("full_roundtrip"), ESearchCase::IgnoreCase))
		{
			++Stats.FullRoundtripTrackCount;
		}
		else if (RoundtripStatus.Equals(TEXT("partial_roundtrip"), ESearchCase::IgnoreCase))
		{
			++Stats.PartialRoundtripTrackCount;
		}
		else if (RoundtripStatus.Equals(TEXT("readonly_summary"), ESearchCase::IgnoreCase))
		{
			++Stats.ReadonlySummaryTrackCount;
		}
		else
		{
			++Stats.UnsupportedTrackCount;
		}

		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("scope"), Scope);
		Item->SetStringField(TEXT("binding_guid"), BindingGuid);
		Item->SetStringField(TEXT("binding_name"), BindingName);
		Item->SetStringField(TEXT("track_file"), TrackFileName);
		Item->SetStringField(TEXT("track_type"), TrackType);
		if (!PropertyType.IsEmpty())
		{
			Item->SetStringField(TEXT("property_type"), PropertyType);
		}
		Item->SetStringField(TEXT("ue_class"), UeClass);
		Item->SetStringField(TEXT("roundtrip_status"), RoundtripStatus);
		Item->SetStringField(TEXT("apply_support"), ApplySupport);
		bool bLossless = RoundtripStatus.Equals(TEXT("full_roundtrip"), ESearchCase::IgnoreCase);
		if (Diagnostics.IsValid())
		{
			Diagnostics->TryGetBoolField(TEXT("is_lossless_roundtrip"), bLossless);
		}
		bool bRoundtripLossy = !RoundtripStatus.Equals(TEXT("full_roundtrip"), ESearchCase::IgnoreCase);
		if (Diagnostics.IsValid())
		{
			Diagnostics->TryGetBoolField(TEXT("roundtrip_lossy"), bRoundtripLossy);
		}
		Item->SetBoolField(TEXT("is_lossless_roundtrip"), bLossless);
		Item->SetBoolField(TEXT("roundtrip_lossy"), bRoundtripLossy);
		double NumberValue = 0.0;
		Item->SetNumberField(TEXT("source_section_count"), Diagnostics.IsValid() && Diagnostics->TryGetNumberField(TEXT("source_section_count"), NumberValue) ? NumberValue : CountTrackJsonSections(TrackObj));
		Item->SetNumberField(TEXT("exported_section_count"), Diagnostics.IsValid() && Diagnostics->TryGetNumberField(TEXT("exported_section_count"), NumberValue) ? NumberValue : CountTrackJsonSections(TrackObj));
		Item->SetNumberField(TEXT("exported_key_count"), Diagnostics.IsValid() && Diagnostics->TryGetNumberField(TEXT("exported_key_count"), NumberValue) ? NumberValue : CountTrackJsonKeys(TrackObj));
		const TArray<TSharedPtr<FJsonValue>>* RoundtripNotes = nullptr;
		if (Diagnostics.IsValid() && Diagnostics->TryGetArrayField(TEXT("roundtrip_notes"), RoundtripNotes) && RoundtripNotes)
		{
			Item->SetArrayField(TEXT("roundtrip_notes"), *RoundtripNotes);
		}
		Stats.Tracks.Add(MakeShared<FJsonValueObject>(Item));
	}

	static TSharedPtr<FJsonObject> BuildCoverageReportJson(const FSequenceFolderCoverageStats& Stats)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), TEXT("uai.sequence.folder_coverage.v1"));
		Root->SetStringField(TEXT("implementation_status"), TEXT("partial_folder_profile"));
		Root->SetStringField(TEXT("authoring_contract"), TEXT("curated_common_subset_not_full_ue_sequencer_model"));
		Root->SetBoolField(TEXT("is_complete_target_schema"), false);
		Root->SetBoolField(TEXT("is_lossless_roundtrip"), Stats.PartialRoundtripTrackCount == 0 && Stats.ReadonlySummaryTrackCount == 0 && Stats.UnsupportedTrackCount == 0);
		Root->SetNumberField(TEXT("track_count"), Stats.TotalTrackCount);
		Root->SetNumberField(TEXT("binding_track_count"), Stats.BindingTrackCount);
		Root->SetNumberField(TEXT("master_track_count"), Stats.MasterTrackCount);
		Root->SetNumberField(TEXT("full_roundtrip_track_count"), Stats.FullRoundtripTrackCount);
		Root->SetNumberField(TEXT("partial_roundtrip_track_count"), Stats.PartialRoundtripTrackCount);
		Root->SetNumberField(TEXT("readonly_summary_track_count"), Stats.ReadonlySummaryTrackCount);
		Root->SetNumberField(TEXT("unsupported_track_count"), Stats.UnsupportedTrackCount);

		TArray<FString> SupportedTrackTypes;
		SupportedTrackTypes.Add(TEXT("transform"));
		SupportedTrackTypes.Add(TEXT("spawn"));
		SupportedTrackTypes.Add(TEXT("skeletal_animation"));
		SupportedTrackTypes.Add(TEXT("control_rig"));
		SupportedTrackTypes.Add(TEXT("visibility"));
		SupportedTrackTypes.Add(TEXT("property.float/double/bool/integer/color/byte/string/vector/rotator/actor_reference/object"));
		SupportedTrackTypes.Add(TEXT("camera_cut"));
		SupportedTrackTypes.Add(TEXT("sub_sequence"));
		SupportedTrackTypes.Add(TEXT("cinematic_shot"));
		SetStringArrayField(Root, TEXT("supported_track_types"), SupportedTrackTypes);

		TArray<FString> BlockingGaps;
		BlockingGaps.Add(TEXT("Full UE Sequencer asset round-trip is not implemented."));
		BlockingGaps.Add(TEXT("Many UE track classes are readonly summary or unsupported, including audio/event/attach/path/constraint/material/timewarp/data layer families."));
		BlockingGaps.Add(TEXT("Common section metadata and common channel key data are serialized, but advanced metadata such as easing/blend/condition/custom time-warp, non-common channel types, structural references and editor-only state may still require dedicated schema."));
		BlockingGaps.Add(TEXT("Control Rig track JSON writes control keys but does not fully capture Sequencer evaluation context or bake state."));
		SetStringArrayField(Root, TEXT("blocking_gaps"), BlockingGaps);

		Root->SetArrayField(TEXT("tracks"), Stats.Tracks);
		return Root;
	}

	static TSharedPtr<FJsonObject> BuildCoverageReportJson(UMovieScene* MovieScene)
	{
		FSequenceFolderCoverageStats Stats;
		if (MovieScene)
		{
			for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
			{
				const FString BindingGuid = Binding.GetObjectGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
				for (int32 TrackIndex = 0; TrackIndex < Binding.GetTracks().Num(); ++TrackIndex)
				{
					UMovieSceneTrack* Track = Binding.GetTracks()[TrackIndex];
					if (TSharedPtr<FJsonObject> TrackObj = BuildTrackJson(MovieScene, Track))
					{
						RecordTrackCoverage(Stats, MovieScene, Track, TEXT("binding"), BindingGuid, Binding.GetName(), MakeTrackFileName(Track, TrackIndex), TrackObj);
					}
				}
			}

			if (UMovieSceneTrack* CameraCutTrack = MovieScene->GetCameraCutTrack())
			{
				if (TSharedPtr<FJsonObject> TrackObj = BuildTrackJson(MovieScene, CameraCutTrack))
				{
					RecordTrackCoverage(Stats, MovieScene, CameraCutTrack, TEXT("master"), TEXT(""), TEXT(""), TEXT("camera_cut"), TrackObj);
				}
			}
			for (int32 TrackIndex = 0; TrackIndex < MovieScene->GetTracks().Num(); ++TrackIndex)
			{
				UMovieSceneTrack* Track = MovieScene->GetTracks()[TrackIndex];
				if (!Track || Track == MovieScene->GetCameraCutTrack())
				{
					continue;
				}
				if (TSharedPtr<FJsonObject> TrackObj = BuildTrackJson(MovieScene, Track))
				{
					RecordTrackCoverage(Stats, MovieScene, Track, TEXT("master"), TEXT(""), TEXT(""), MakeTrackFileName(Track, TrackIndex), TrackObj);
				}
			}
		}
		return BuildCoverageReportJson(Stats);
	}

	static void IncrementClassCount(TMap<FString, int32>& Counts, UObject* Object)
	{
		const FString ClassPath = Object && Object->GetClass() ? Object->GetClass()->GetPathName() : TEXT("null");
		int32& Count = Counts.FindOrAdd(ClassPath);
		++Count;
	}

	static TArray<TSharedPtr<FJsonValue>> BuildClassCountArray(const TMap<FString, int32>& Counts)
	{
		TArray<FString> ClassNames;
		Counts.GetKeys(ClassNames);
		ClassNames.Sort();

		TArray<TSharedPtr<FJsonValue>> Items;
		for (const FString& ClassName : ClassNames)
		{
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("class"), ClassName);
			Item->SetNumberField(TEXT("count"), Counts.FindRef(ClassName));
			Items.Add(MakeShared<FJsonValueObject>(Item));
		}
		return Items;
	}

	static TSharedPtr<FJsonObject> BuildCompleteSectionInventoryJson(UMovieScene* MovieScene, UMovieSceneSection* Section, const int32 SectionIndex)
	{
		TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
		SectionObj->SetNumberField(TEXT("section_index"), SectionIndex);
		if (!MovieScene || !Section)
		{
			SectionObj->SetStringField(TEXT("section_class"), TEXT(""));
			return SectionObj;
		}

		SectionObj->SetStringField(TEXT("section_name"), Section->GetName());
		SectionObj->SetStringField(TEXT("section_path"), Section->GetPathName());
		SectionObj->SetStringField(TEXT("section_class"), Section->GetClass() ? Section->GetClass()->GetPathName() : TEXT(""));
		SectionObj->SetObjectField(TEXT("section_metadata"), BuildSectionMetadataJson(MovieScene, Section));
		SectionObj->SetArrayField(TEXT("raw_properties"), BuildSequenceRawPropertiesJson(Section));
		SectionObj->SetArrayField(TEXT("channels"), BuildSectionChannelsJson(MovieScene, Section));

		if (UMovieSceneControlRigParameterSection* ControlRigSection = Cast<UMovieSceneControlRigParameterSection>(Section))
		{
			SectionObj->SetStringField(TEXT("section_family"), TEXT("control_rig"));
			SectionObj->SetStringField(TEXT("control_rig_class"), ControlRigSection->ControlRigClass ? ControlRigSection->ControlRigClass->GetPathName() : TEXT(""));
		}
		else if (UMovieSceneSkeletalAnimationSection* SkeletalSection = Cast<UMovieSceneSkeletalAnimationSection>(Section))
		{
			SectionObj->SetStringField(TEXT("section_family"), TEXT("skeletal_animation"));
			SectionObj->SetStringField(TEXT("animation_asset"), SkeletalSection->Params.Animation ? SkeletalSection->Params.Animation->GetPathName() : TEXT(""));
		}
		else if (UMovieSceneSubSection* SubSection = Cast<UMovieSceneSubSection>(Section))
		{
			SectionObj->SetStringField(TEXT("section_family"), TEXT("sub_sequence"));
			SectionObj->SetStringField(TEXT("sequence_asset"), SubSection->GetSequence() ? SubSection->GetSequence()->GetPathName() : TEXT(""));
		}
		else if (UMovieSceneCameraCutSection* CameraCutSection = Cast<UMovieSceneCameraCutSection>(Section))
		{
			SectionObj->SetStringField(TEXT("section_family"), TEXT("camera_cut"));
			SectionObj->SetStringField(TEXT("camera_binding_guid"), CameraCutSection->GetCameraBindingID().GetGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
		}

		return SectionObj;
	}

	static TSharedPtr<FJsonObject> BuildCompleteTrackInventoryJson(
		UMovieScene* MovieScene,
		UMovieSceneTrack* Track,
		const FString& Scope,
		const FString& BindingGuid,
		const FString& BindingName,
		const int32 TrackIndex)
	{
		TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
		TrackObj->SetStringField(TEXT("scope"), Scope);
		TrackObj->SetStringField(TEXT("binding_guid"), BindingGuid);
		TrackObj->SetStringField(TEXT("binding_name"), BindingName);
		TrackObj->SetNumberField(TEXT("track_index"), TrackIndex);
		TrackObj->SetStringField(TEXT("track_name"), Track ? Track->GetName() : TEXT(""));
		TrackObj->SetStringField(TEXT("track_class"), Track && Track->GetClass() ? Track->GetClass()->GetPathName() : TEXT(""));
		TrackObj->SetNumberField(TEXT("section_count"), Track ? Track->GetAllSections().Num() : 0);
		TrackObj->SetArrayField(TEXT("raw_properties"), BuildSequenceRawPropertiesJson(Track));

		if (TSharedPtr<FJsonObject> Summary = UeAgentSequenceOps::BuildMovieSceneTrackSummary(MovieScene, Track))
		{
			TrackObj->SetObjectField(TEXT("ue_summary"), Summary);
		}

		TSharedPtr<FJsonObject> AuthoringJson = BuildTrackJson(MovieScene, Track);
		if (AuthoringJson.IsValid())
		{
			FString TrackType;
			if (AuthoringJson->TryGetStringField(TEXT("track_type"), TrackType) && !TrackType.IsEmpty())
			{
				TrackObj->SetStringField(TEXT("authoring_track_type"), TrackType);
			}
			FString PropertyType;
			if (AuthoringJson->TryGetStringField(TEXT("property_type"), PropertyType) && !PropertyType.IsEmpty())
			{
				TrackObj->SetStringField(TEXT("authoring_property_type"), PropertyType);
			}
		}

		if (TSharedPtr<FJsonObject> Diagnostics = BuildTrackDiagnosticsJson(MovieScene, Track, AuthoringJson))
		{
			FString RoundtripStatus;
			if (Diagnostics->TryGetStringField(TEXT("roundtrip_status"), RoundtripStatus) && !RoundtripStatus.IsEmpty())
			{
				TrackObj->SetStringField(TEXT("roundtrip_status"), RoundtripStatus);
			}
			FString ApplySupport;
			if (Diagnostics->TryGetStringField(TEXT("apply_support"), ApplySupport) && !ApplySupport.IsEmpty())
			{
				TrackObj->SetStringField(TEXT("apply_support"), ApplySupport);
			}
			bool bLossless = false;
			if (Diagnostics->TryGetBoolField(TEXT("is_lossless_roundtrip"), bLossless))
			{
				TrackObj->SetBoolField(TEXT("is_lossless_roundtrip"), bLossless);
			}
			bool bRoundtripLossy = false;
			if (Diagnostics->TryGetBoolField(TEXT("roundtrip_lossy"), bRoundtripLossy))
			{
				TrackObj->SetBoolField(TEXT("roundtrip_lossy"), bRoundtripLossy);
			}
			double NumberValue = 0.0;
			if (Diagnostics->TryGetNumberField(TEXT("source_section_count"), NumberValue))
			{
				TrackObj->SetNumberField(TEXT("source_section_count"), NumberValue);
			}
			if (Diagnostics->TryGetNumberField(TEXT("exported_section_count"), NumberValue))
			{
				TrackObj->SetNumberField(TEXT("exported_section_count"), NumberValue);
			}
			if (Diagnostics->TryGetNumberField(TEXT("exported_key_count"), NumberValue))
			{
				TrackObj->SetNumberField(TEXT("exported_key_count"), NumberValue);
			}
			const TArray<TSharedPtr<FJsonValue>>* RoundtripNotes = nullptr;
			if (Diagnostics->TryGetArrayField(TEXT("roundtrip_notes"), RoundtripNotes) && RoundtripNotes)
			{
				TrackObj->SetArrayField(TEXT("roundtrip_notes"), *RoundtripNotes);
			}
		}

		TArray<TSharedPtr<FJsonValue>> Sections;
		if (Track)
		{
			const TArray<UMovieSceneSection*>& AllSections = Track->GetAllSections();
			for (int32 SectionIndex = 0; SectionIndex < AllSections.Num(); ++SectionIndex)
			{
				Sections.Add(MakeShared<FJsonValueObject>(BuildCompleteSectionInventoryJson(MovieScene, AllSections[SectionIndex], SectionIndex)));
			}
		}
		TrackObj->SetArrayField(TEXT("sections"), Sections);
		return TrackObj;
	}

	static TSharedPtr<FJsonObject> BuildCompleteInventoryJson(ULevelSequence* Sequence, UMovieScene* MovieScene)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), TEXT("uai.sequence.complete_inventory.v1"));
		Root->SetStringField(TEXT("source_of_truth_role"), TEXT("readonly_complete_sequencer_inventory"));
		Root->SetStringField(TEXT("writeback_contract"), TEXT("folder_authoring_subset_plus_common_section_metadata_plus_safe_raw_uproperty_text"));
		Root->SetBoolField(TEXT("is_complete_read_inventory"), true);
		Root->SetBoolField(TEXT("is_complete_lossless_apply"), false);
		Root->SetStringField(TEXT("sequence_path"), Sequence ? Sequence->GetPathName() : TEXT(""));
		Root->SetStringField(TEXT("sequence_class"), Sequence && Sequence->GetClass() ? Sequence->GetClass()->GetPathName() : TEXT(""));
		Root->SetStringField(TEXT("movie_scene_path"), MovieScene ? MovieScene->GetPathName() : TEXT(""));
		Root->SetStringField(TEXT("movie_scene_class"), MovieScene && MovieScene->GetClass() ? MovieScene->GetClass()->GetPathName() : TEXT(""));
		Root->SetArrayField(TEXT("sequence_raw_properties"), BuildSequenceRawPropertiesJson(Sequence));
		Root->SetArrayField(TEXT("movie_scene_raw_properties"), BuildSequenceRawPropertiesJson(MovieScene));

		int32 TrackCount = 0;
		int32 SectionCount = 0;
		TMap<FString, int32> TrackClassCounts;
		TMap<FString, int32> SectionClassCounts;

		if (MovieScene)
		{
			TSharedPtr<FJsonObject> SettingsObj = MakeShared<FJsonObject>();
			SettingsObj->SetObjectField(TEXT("tick_resolution"), BuildFrameRateJson(MovieScene->GetTickResolution()));
			SettingsObj->SetObjectField(TEXT("display_rate"), BuildFrameRateJson(MovieScene->GetDisplayRate()));
			const TRange<FFrameNumber> PlaybackRange = MovieScene->GetPlaybackRange();
			TSharedPtr<FJsonObject> PlaybackObj = MakeShared<FJsonObject>();
			PlaybackObj->SetNumberField(TEXT("start_frame"), PlaybackRange.GetLowerBoundValue().Value);
			PlaybackObj->SetNumberField(TEXT("end_frame"), PlaybackRange.GetUpperBoundValue().Value);
			PlaybackObj->SetNumberField(TEXT("start_seconds"), MovieScene->GetTickResolution().AsSeconds(FFrameTime(PlaybackRange.GetLowerBoundValue())));
			PlaybackObj->SetNumberField(TEXT("duration_seconds"), MovieScene->GetTickResolution().AsSeconds(FFrameTime(PlaybackRange.GetUpperBoundValue() - PlaybackRange.GetLowerBoundValue())));
			SettingsObj->SetObjectField(TEXT("playback_range"), PlaybackObj);
			if (const UEnum* EvaluationTypeEnum = StaticEnum<EMovieSceneEvaluationType>())
			{
				SettingsObj->SetStringField(TEXT("evaluation_type"), EvaluationTypeEnum->GetNameStringByValue(static_cast<int64>(MovieScene->GetEvaluationType())));
			}
			if (const UEnum* ClockSourceEnum = StaticEnum<EUpdateClockSource>())
			{
				SettingsObj->SetStringField(TEXT("clock_source"), ClockSourceEnum->GetNameStringByValue(static_cast<int64>(MovieScene->GetClockSource())));
			}
			Root->SetObjectField(TEXT("settings"), SettingsObj);
		}

		TArray<TSharedPtr<FJsonValue>> BindingItems;
		TArray<TSharedPtr<FJsonValue>> MasterTrackItems;
		if (MovieScene)
		{
			for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
			{
				TSharedPtr<FJsonObject> BindingObj = MakeShared<FJsonObject>();
				const FString BindingGuid = Binding.GetObjectGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
				BindingObj->SetStringField(TEXT("binding_guid"), BindingGuid);
				BindingObj->SetStringField(TEXT("binding_name"), Binding.GetName());
				BindingObj->SetNumberField(TEXT("track_count"), Binding.GetTracks().Num());
				TArray<TSharedPtr<FJsonValue>> TrackItems;
				for (int32 TrackIndex = 0; TrackIndex < Binding.GetTracks().Num(); ++TrackIndex)
				{
					UMovieSceneTrack* Track = Binding.GetTracks()[TrackIndex];
					TrackItems.Add(MakeShared<FJsonValueObject>(BuildCompleteTrackInventoryJson(MovieScene, Track, TEXT("binding"), BindingGuid, Binding.GetName(), TrackIndex)));
					IncrementClassCount(TrackClassCounts, Track);
					++TrackCount;
					if (Track)
					{
						for (UMovieSceneSection* Section : Track->GetAllSections())
						{
							IncrementClassCount(SectionClassCounts, Section);
							++SectionCount;
						}
					}
				}
				BindingObj->SetArrayField(TEXT("tracks"), TrackItems);
				BindingItems.Add(MakeShared<FJsonValueObject>(BindingObj));
			}

			if (UMovieSceneTrack* CameraCutTrack = MovieScene->GetCameraCutTrack())
			{
				MasterTrackItems.Add(MakeShared<FJsonValueObject>(BuildCompleteTrackInventoryJson(MovieScene, CameraCutTrack, TEXT("master"), TEXT(""), TEXT(""), 0)));
				IncrementClassCount(TrackClassCounts, CameraCutTrack);
				++TrackCount;
				for (UMovieSceneSection* Section : CameraCutTrack->GetAllSections())
				{
					IncrementClassCount(SectionClassCounts, Section);
					++SectionCount;
				}
			}

			for (int32 TrackIndex = 0; TrackIndex < MovieScene->GetTracks().Num(); ++TrackIndex)
			{
				UMovieSceneTrack* Track = MovieScene->GetTracks()[TrackIndex];
				if (!Track || Track == MovieScene->GetCameraCutTrack())
				{
					continue;
				}
				MasterTrackItems.Add(MakeShared<FJsonValueObject>(BuildCompleteTrackInventoryJson(MovieScene, Track, TEXT("master"), TEXT(""), TEXT(""), TrackIndex)));
				IncrementClassCount(TrackClassCounts, Track);
				++TrackCount;
				for (UMovieSceneSection* Section : Track->GetAllSections())
				{
					IncrementClassCount(SectionClassCounts, Section);
					++SectionCount;
				}
			}
		}

		Root->SetArrayField(TEXT("bindings"), BindingItems);
		Root->SetArrayField(TEXT("master_tracks"), MasterTrackItems);
		Root->SetNumberField(TEXT("binding_count"), BindingItems.Num());
		Root->SetNumberField(TEXT("master_track_count"), MasterTrackItems.Num());
		Root->SetNumberField(TEXT("track_count"), TrackCount);
		Root->SetNumberField(TEXT("section_count"), SectionCount);
		Root->SetArrayField(TEXT("track_class_counts"), BuildClassCountArray(TrackClassCounts));
		Root->SetArrayField(TEXT("section_class_counts"), BuildClassCountArray(SectionClassCounts));
		return Root;
	}

	static int32 GetJsonIntegerFieldOrDefault(const TSharedPtr<FJsonObject>& Obj, const FString& FieldName, const int32 DefaultValue = 0)
	{
		if (!Obj.IsValid())
		{
			return DefaultValue;
		}
		double NumberValue = 0.0;
		return Obj->TryGetNumberField(FieldName, NumberValue) ? FMath::RoundToInt(NumberValue) : DefaultValue;
	}

	static FString GetJsonStringFieldOrEmpty(const TSharedPtr<FJsonObject>& Obj, const FString& FieldName)
	{
		FString Value;
		if (Obj.IsValid())
		{
			Obj->TryGetStringField(FieldName, Value);
		}
		return Value;
	}

	static TArray<FString> BuildCoverageTrackSignatures(const TSharedPtr<FJsonObject>& CoverageReport)
	{
		TArray<FString> Signatures;
		const TArray<TSharedPtr<FJsonValue>>* Tracks = nullptr;
		if (!CoverageReport.IsValid() || !CoverageReport->TryGetArrayField(TEXT("tracks"), Tracks) || !Tracks)
		{
			return Signatures;
		}

		for (const TSharedPtr<FJsonValue>& TrackValue : *Tracks)
		{
			const TSharedPtr<FJsonObject>* TrackObj = nullptr;
			if (!TrackValue.IsValid() || !TrackValue->TryGetObject(TrackObj) || !TrackObj || !TrackObj->IsValid())
			{
				continue;
			}
			const FString Signature = FString::Printf(
				TEXT("%s|%s|%s|%s|%s"),
				*GetJsonStringFieldOrEmpty(*TrackObj, TEXT("scope")),
				*GetJsonStringFieldOrEmpty(*TrackObj, TEXT("binding_guid")),
				*GetJsonStringFieldOrEmpty(*TrackObj, TEXT("track_type")),
				*GetJsonStringFieldOrEmpty(*TrackObj, TEXT("property_type")),
				*GetJsonStringFieldOrEmpty(*TrackObj, TEXT("ue_class")));
			Signatures.Add(Signature);
		}
		Signatures.Sort();
		return Signatures;
	}

	static void SetStringArrayFieldFromValues(TSharedPtr<FJsonObject> Obj, const FString& FieldName, const TArray<FString>& Values)
	{
		if (!Obj.IsValid())
		{
			return;
		}
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		Obj->SetArrayField(FieldName, JsonValues);
	}

	static TSharedPtr<FJsonObject> BuildCoverageDiffJson(const TSharedPtr<FJsonObject>& InputReport, const TSharedPtr<FJsonObject>& PostReport)
	{
		TSharedPtr<FJsonObject> DiffObj = MakeShared<FJsonObject>();
		DiffObj->SetStringField(TEXT("schema"), TEXT("uai.sequence.folder_coverage_diff.v1"));
		DiffObj->SetBoolField(TEXT("has_input_report"), InputReport.IsValid());
		DiffObj->SetBoolField(TEXT("has_post_apply_report"), PostReport.IsValid());
		if (!InputReport.IsValid() || !PostReport.IsValid())
		{
			DiffObj->SetBoolField(TEXT("is_track_structure_equal"), false);
			return DiffObj;
		}

		const int32 InputTrackCount = GetJsonIntegerFieldOrDefault(InputReport, TEXT("track_count"));
		const int32 PostTrackCount = GetJsonIntegerFieldOrDefault(PostReport, TEXT("track_count"));
		const int32 InputBindingTrackCount = GetJsonIntegerFieldOrDefault(InputReport, TEXT("binding_track_count"));
		const int32 PostBindingTrackCount = GetJsonIntegerFieldOrDefault(PostReport, TEXT("binding_track_count"));
		const int32 InputMasterTrackCount = GetJsonIntegerFieldOrDefault(InputReport, TEXT("master_track_count"));
		const int32 PostMasterTrackCount = GetJsonIntegerFieldOrDefault(PostReport, TEXT("master_track_count"));
		DiffObj->SetNumberField(TEXT("track_count_delta"), PostTrackCount - InputTrackCount);
		DiffObj->SetNumberField(TEXT("binding_track_count_delta"), PostBindingTrackCount - InputBindingTrackCount);
		DiffObj->SetNumberField(TEXT("master_track_count_delta"), PostMasterTrackCount - InputMasterTrackCount);
		DiffObj->SetNumberField(TEXT("unsupported_track_count_delta"), GetJsonIntegerFieldOrDefault(PostReport, TEXT("unsupported_track_count")) - GetJsonIntegerFieldOrDefault(InputReport, TEXT("unsupported_track_count")));
		DiffObj->SetNumberField(TEXT("readonly_summary_track_count_delta"), GetJsonIntegerFieldOrDefault(PostReport, TEXT("readonly_summary_track_count")) - GetJsonIntegerFieldOrDefault(InputReport, TEXT("readonly_summary_track_count")));
		DiffObj->SetNumberField(TEXT("partial_roundtrip_track_count_delta"), GetJsonIntegerFieldOrDefault(PostReport, TEXT("partial_roundtrip_track_count")) - GetJsonIntegerFieldOrDefault(InputReport, TEXT("partial_roundtrip_track_count")));

		const TArray<FString> InputSignatures = BuildCoverageTrackSignatures(InputReport);
		const TArray<FString> PostSignatures = BuildCoverageTrackSignatures(PostReport);
		TSet<FString> InputSet;
		for (const FString& Signature : InputSignatures)
		{
			InputSet.Add(Signature);
		}
		TSet<FString> PostSet;
		for (const FString& Signature : PostSignatures)
		{
			PostSet.Add(Signature);
		}
		TArray<FString> MissingInPost;
		TArray<FString> ExtraInPost;
		for (const FString& Signature : InputSignatures)
		{
			if (!PostSet.Contains(Signature))
			{
				MissingInPost.Add(Signature);
			}
		}
		for (const FString& Signature : PostSignatures)
		{
			if (!InputSet.Contains(Signature))
			{
				ExtraInPost.Add(Signature);
			}
		}
		SetStringArrayFieldFromValues(DiffObj, TEXT("missing_track_signatures_in_post_apply"), MissingInPost);
		SetStringArrayFieldFromValues(DiffObj, TEXT("extra_track_signatures_in_post_apply"), ExtraInPost);
		DiffObj->SetNumberField(TEXT("missing_track_signature_count"), MissingInPost.Num());
		DiffObj->SetNumberField(TEXT("extra_track_signature_count"), ExtraInPost.Num());
		DiffObj->SetBoolField(
			TEXT("is_track_structure_equal"),
			InputTrackCount == PostTrackCount
			&& InputBindingTrackCount == PostBindingTrackCount
			&& InputMasterTrackCount == PostMasterTrackCount
			&& MissingInPost.Num() == 0
			&& ExtraInPost.Num() == 0);
		return DiffObj;
	}

	static TSharedPtr<FJsonObject> BuildMasterTracksIndexJson(UMovieScene* MovieScene, FSequenceFolderCoverageStats* CoverageStats);
	static TSharedPtr<FJsonObject> BuildBindingTrackRefJson(const FGuid& BindingGuid, const FString& TrackFileName, const TSharedPtr<FJsonObject>& TrackJson);
	static TSharedPtr<FJsonObject> BuildOutlinerFoldersJson(UMovieScene* MovieScene, const TMap<const UMovieSceneTrack*, TSharedPtr<FJsonObject>>& BindingTrackRefsByTrack);

	static bool IsStructuralSnapshotIgnoredField(const FString& FieldName)
	{
		return FieldName.Equals(TEXT("source_profile"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("has_complete_inventory"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("complete_inventory_path"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("section_metadata"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("section_name"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("section_path"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("track_name"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("track_path"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("track_file"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("display_name"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("raw_properties"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("ue_class"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("apply_support"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("roundtrip_status"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("is_lossless_roundtrip"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("roundtrip_lossy"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("roundtrip_notes"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("source_section_count"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("exported_section_count"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("exported_key_count"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("channels_used"), ESearchCase::IgnoreCase);
	}

	static bool IsStructuralRawPropertiesField(const FString& FieldName)
	{
		return FieldName.Equals(TEXT("raw_properties"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("sequence_raw_properties"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("movie_scene_raw_properties"), ESearchCase::IgnoreCase);
	}

	static void CopyJsonFieldIfPresent(const TSharedPtr<FJsonObject>& SourceObj, TSharedPtr<FJsonObject>& DestObj, const FString& FieldName)
	{
		if (!SourceObj.IsValid() || !DestObj.IsValid())
		{
			return;
		}
		if (const TSharedPtr<FJsonValue>* Value = SourceObj->Values.Find(FieldName))
		{
			DestObj->SetField(FieldName, *Value);
		}
	}

	static TArray<TSharedPtr<FJsonValue>> BuildComparableRawPropertiesArray(const TSharedPtr<FJsonValue>& Value)
	{
		TArray<TSharedPtr<FJsonValue>> ComparableValues;
		if (!Value.IsValid() || Value->Type != EJson::Array)
		{
			return ComparableValues;
		}

		for (const TSharedPtr<FJsonValue>& RawValue : Value->AsArray())
		{
			const TSharedPtr<FJsonObject>* RawObj = nullptr;
			if (!RawValue.IsValid() || !RawValue->TryGetObject(RawObj) || !RawObj || !RawObj->IsValid())
			{
				continue;
			}

			TSharedPtr<FJsonObject> ComparableObj = MakeShared<FJsonObject>();
			CopyJsonFieldIfPresent(*RawObj, ComparableObj, TEXT("valid"));
			CopyJsonFieldIfPresent(*RawObj, ComparableObj, TEXT("property_name"));
			CopyJsonFieldIfPresent(*RawObj, ComparableObj, TEXT("authored_name"));
			CopyJsonFieldIfPresent(*RawObj, ComparableObj, TEXT("cpp_type"));
			CopyJsonFieldIfPresent(*RawObj, ComparableObj, TEXT("property_class"));
			CopyJsonFieldIfPresent(*RawObj, ComparableObj, TEXT("editable"));
			CopyJsonFieldIfPresent(*RawObj, ComparableObj, TEXT("blueprint_visible"));
			CopyJsonFieldIfPresent(*RawObj, ComparableObj, TEXT("transient"));
			CopyJsonFieldIfPresent(*RawObj, ComparableObj, TEXT("deprecated"));
			CopyJsonFieldIfPresent(*RawObj, ComparableObj, TEXT("edit_const"));
			CopyJsonFieldIfPresent(*RawObj, ComparableObj, TEXT("structural"));
			CopyJsonFieldIfPresent(*RawObj, ComparableObj, TEXT("reference_like"));
			CopyJsonFieldIfPresent(*RawObj, ComparableObj, TEXT("container_like"));
			CopyJsonFieldIfPresent(*RawObj, ComparableObj, TEXT("can_apply_raw"));

			bool bCanApplyRaw = false;
			(*RawObj)->TryGetBoolField(TEXT("can_apply_raw"), bCanApplyRaw);
			if (bCanApplyRaw)
			{
				CopyJsonFieldIfPresent(*RawObj, ComparableObj, TEXT("value_text"));
			}

			ComparableValues.Add(MakeShared<FJsonValueObject>(ComparableObj));
		}
		return ComparableValues;
	}

	static FString GetStructuralTrackBaseKey(const TSharedPtr<FJsonObject>& TrackObj)
	{
		FString TrackType;
		FString PropertyName;
		FString PropertyPath;
		FString PropertyType;
		FString ControlRigClass;
		FString UeClass;
		if (TrackObj.IsValid())
		{
			TrackObj->TryGetStringField(TEXT("track_type"), TrackType);
			TrackObj->TryGetStringField(TEXT("property_name"), PropertyName);
			TrackObj->TryGetStringField(TEXT("property_path"), PropertyPath);
			TrackObj->TryGetStringField(TEXT("property_type"), PropertyType);
			TrackObj->TryGetStringField(TEXT("control_rig_class"), ControlRigClass);
			TrackObj->TryGetStringField(TEXT("ue_class"), UeClass);
		}
		if (TrackType.IsEmpty())
		{
			TrackType = TEXT("unknown");
		}

		TArray<FString> Parts;
		Parts.Add(TrackType.ToLower());
		if (!PropertyPath.IsEmpty())
		{
			Parts.Add(PropertyPath);
		}
		else if (!PropertyName.IsEmpty())
		{
			Parts.Add(PropertyName);
		}
		if (!PropertyType.IsEmpty())
		{
			Parts.Add(PropertyType);
		}
		if (!ControlRigClass.IsEmpty())
		{
			Parts.Add(FPackageName::GetShortName(ControlRigClass));
		}
		if (!UeClass.IsEmpty() && TrackType.Equals(TEXT("unsupported"), ESearchCase::IgnoreCase))
		{
			Parts.Add(FPackageName::GetShortName(UeClass));
		}
		return MakeSafeName(FString::Join(Parts, TEXT("_")));
	}

	static FString MakeStructuralTrackEntryKey(const TSharedPtr<FJsonObject>& TrackObj, TMap<FString, int32>& SeenTrackKeys)
	{
		const FString BaseKey = GetStructuralTrackBaseKey(TrackObj);
		int32& Count = SeenTrackKeys.FindOrAdd(BaseKey);
		const int32 ThisIndex = Count++;
		return FString::Printf(TEXT("%s_%02d"), *BaseKey, ThisIndex);
	}

	static void NormalizeSectionAggregateKeysForStructuralSnapshot(const TSharedPtr<FJsonObject>& SectionObj, const FFrameRate& TickResolution)
	{
		if (!SectionObj.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* AggregateKeys = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Channels = nullptr;
		if (!SectionObj->TryGetArrayField(TEXT("keys"), AggregateKeys) || !AggregateKeys || AggregateKeys->Num() == 0)
		{
			return;
		}

		for (int32 KeyIndex = 0; KeyIndex < AggregateKeys->Num(); ++KeyIndex)
		{
			const TSharedPtr<FJsonObject>* AggregateKeyObj = nullptr;
			if (!(*AggregateKeys)[KeyIndex].IsValid() || !(*AggregateKeys)[KeyIndex]->TryGetObject(AggregateKeyObj) || !AggregateKeyObj || !AggregateKeyObj->IsValid())
			{
				continue;
			}
			FFrameNumber AggregateFrame = FFrameNumber(0);
			if (TryGetAggregateKeyFrameOverride(SectionObj, KeyIndex, TickResolution, AggregateFrame))
			{
				(*AggregateKeyObj)->SetNumberField(TEXT("frame"), AggregateFrame.Value);
				(*AggregateKeyObj)->SetNumberField(TEXT("time_seconds"), TickResolution.AsSeconds(FFrameTime(AggregateFrame)));
			}
			FString ControlType;
			if ((*AggregateKeyObj)->TryGetStringField(TEXT("control_type"), ControlType)
				&& ControlType.Equals(TEXT("euler_transform"), ESearchCase::IgnoreCase))
			{
				(*AggregateKeyObj)->SetStringField(TEXT("control_type"), TEXT("transform"));
			}
		}

		if (!SectionObj->TryGetArrayField(TEXT("channels"), Channels) || !Channels)
		{
			return;
		}

		for (int32 ChannelArrayIndex = 0; ChannelArrayIndex < Channels->Num(); ++ChannelArrayIndex)
		{
			const TSharedPtr<FJsonObject>* ChannelObj = nullptr;
			if (!(*Channels)[ChannelArrayIndex].IsValid() || !(*Channels)[ChannelArrayIndex]->TryGetObject(ChannelObj) || !ChannelObj || !ChannelObj->IsValid())
			{
				continue;
			}

			double ChannelIndexNumber = static_cast<double>(ChannelArrayIndex);
			(*ChannelObj)->TryGetNumberField(TEXT("channel_index"), ChannelIndexNumber);
			const int32 ChannelIndex = FMath::RoundToInt(ChannelIndexNumber);
			FString ChannelType;
			(*ChannelObj)->TryGetStringField(TEXT("channel_type"), ChannelType);

			const TArray<TSharedPtr<FJsonValue>>* ChannelKeys = nullptr;
			if (!(*ChannelObj)->TryGetArrayField(TEXT("keys"), ChannelKeys) || !ChannelKeys)
			{
				continue;
			}

			for (int32 KeyIndex = 0; KeyIndex < ChannelKeys->Num(); ++KeyIndex)
			{
				const TSharedPtr<FJsonObject>* ChannelKeyObj = nullptr;
				if (!(*ChannelKeys)[KeyIndex].IsValid() || !(*ChannelKeys)[KeyIndex]->TryGetObject(ChannelKeyObj) || !ChannelKeyObj || !ChannelKeyObj->IsValid())
				{
					continue;
				}

				FFrameNumber KeyFrame = FFrameNumber(0);
				double ExistingFrameNumber = 0.0;
				if ((*ChannelKeyObj)->TryGetNumberField(TEXT("frame"), ExistingFrameNumber))
				{
					KeyFrame = FFrameNumber(static_cast<int32>(FMath::RoundToInt64(ExistingFrameNumber)));
				}
				TryGetAggregateKeyFrameOverride(SectionObj, KeyIndex, TickResolution, KeyFrame);
				(*ChannelKeyObj)->SetNumberField(TEXT("frame"), KeyFrame.Value);
				(*ChannelKeyObj)->SetNumberField(TEXT("time_seconds"), TickResolution.AsSeconds(FFrameTime(KeyFrame)));

				if (ChannelType.Equals(TEXT("double"), ESearchCase::IgnoreCase)
					|| ChannelType.Equals(TEXT("float"), ESearchCase::IgnoreCase)
					|| ChannelType.Equals(TEXT("integer"), ESearchCase::IgnoreCase)
					|| ChannelType.Equals(TEXT("byte"), ESearchCase::IgnoreCase))
				{
					double NumberValue = 0.0;
					if (TryGetAggregateNumberOverride(SectionObj, ChannelIndex, KeyIndex, KeyFrame, NumberValue))
					{
						(*ChannelKeyObj)->SetNumberField(TEXT("value"), NumberValue);
					}
				}
				else if (ChannelType.Equals(TEXT("bool"), ESearchCase::IgnoreCase))
				{
					bool BoolValue = false;
					if (TryGetAggregateBoolOverride(SectionObj, ChannelIndex, KeyIndex, KeyFrame, BoolValue))
					{
						(*ChannelKeyObj)->SetBoolField(TEXT("value"), BoolValue);
					}
				}
				else if (ChannelType.Equals(TEXT("string"), ESearchCase::IgnoreCase))
				{
					FString StringValue;
					if (TryGetAggregateStringOverride(SectionObj, ChannelIndex, KeyIndex, KeyFrame, StringValue))
					{
						(*ChannelKeyObj)->SetStringField(TEXT("value"), StringValue);
					}
				}
			}
		}
	}

	static void NormalizeTrackLikeJsonForStructuralSnapshot(const TSharedPtr<FJsonObject>& TrackObj, const FFrameRate& TickResolution)
	{
		if (!TrackObj.IsValid())
		{
			return;
		}
		FString TrackType;
		TrackObj->TryGetStringField(TEXT("track_type"), TrackType);
		FString TrackControlRigClass;
		TrackObj->TryGetStringField(TEXT("control_rig_class"), TrackControlRigClass);
		const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
		if (TrackObj->TryGetArrayField(TEXT("sections"), Sections) && Sections)
		{
			for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
			{
				const TSharedPtr<FJsonObject>* SectionObj = nullptr;
				if (SectionValue.IsValid() && SectionValue->TryGetObject(SectionObj) && SectionObj && SectionObj->IsValid())
				{
					FString SequenceAssetPath;
					if ((*SectionObj)->TryGetStringField(TEXT("sequence_asset"), SequenceAssetPath) && !SequenceAssetPath.IsEmpty())
					{
						const FString PackagePath = FPackageName::ObjectPathToPackageName(SequenceAssetPath);
						if (!PackagePath.IsEmpty())
						{
							(*SectionObj)->SetStringField(TEXT("sequence_asset"), PackagePath);
						}
					}
					NormalizeSectionAggregateKeysForStructuralSnapshot(*SectionObj, TickResolution);
					if (TrackType.Equals(TEXT("control_rig"), ESearchCase::IgnoreCase))
					{
						(*SectionObj)->RemoveField(TEXT("channels"));
						if (!TrackControlRigClass.IsEmpty() && !(*SectionObj)->HasField(TEXT("control_rig_class")))
						{
							(*SectionObj)->SetStringField(TEXT("control_rig_class"), TrackControlRigClass);
						}
					}
				}
			}
		}
	}

	static void NormalizeFolderEntryJsonForStructuralSnapshot(const TSharedPtr<FJsonObject>& EntryObj, const FFrameRate& TickResolution)
	{
		if (!EntryObj.IsValid())
		{
			return;
		}
		NormalizeTrackLikeJsonForStructuralSnapshot(EntryObj, TickResolution);
		const TArray<TSharedPtr<FJsonValue>>* Tracks = nullptr;
		if (EntryObj->TryGetArrayField(TEXT("tracks"), Tracks) && Tracks)
		{
			for (const TSharedPtr<FJsonValue>& TrackValue : *Tracks)
			{
				const TSharedPtr<FJsonObject>* TrackObj = nullptr;
				if (TrackValue.IsValid() && TrackValue->TryGetObject(TrackObj) && TrackObj && TrackObj->IsValid())
				{
					NormalizeTrackLikeJsonForStructuralSnapshot(*TrackObj, TickResolution);
				}
			}
		}
	}

	static void AppendCanonicalJsonString(FString& Out, const FString& Value)
	{
		Out.AppendChar(TEXT('"'));
		for (const TCHAR Char : Value)
		{
			if (Char == TEXT('\\') || Char == TEXT('"'))
			{
				Out.AppendChar(TEXT('\\'));
				Out.AppendChar(Char);
			}
			else if (Char == TEXT('\n'))
			{
				Out += TEXT("\\n");
			}
			else if (Char == TEXT('\r'))
			{
				Out += TEXT("\\r");
			}
			else if (Char == TEXT('\t'))
			{
				Out += TEXT("\\t");
			}
			else
			{
				Out.AppendChar(Char);
			}
		}
		Out.AppendChar(TEXT('"'));
	}

	static void AppendCanonicalJsonValue(FString& Out, const TSharedPtr<FJsonValue>& Value);

	static bool IsStructuralChannelsValueEffectivelyEmpty(const TSharedPtr<FJsonValue>& ChannelsValue)
	{
		if (!ChannelsValue.IsValid() || ChannelsValue->Type != EJson::Array)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& ChannelValue : ChannelsValue->AsArray())
		{
			const TSharedPtr<FJsonObject>* ChannelObj = nullptr;
			if (!ChannelValue.IsValid() || !ChannelValue->TryGetObject(ChannelObj) || !ChannelObj || !ChannelObj->IsValid())
			{
				continue;
			}
			bool bHasDefault = false;
			(*ChannelObj)->TryGetBoolField(TEXT("has_default"), bHasDefault);
			const TArray<TSharedPtr<FJsonValue>>* ChannelKeys = nullptr;
			const bool bHasKeys = (*ChannelObj)->TryGetArrayField(TEXT("keys"), ChannelKeys) && ChannelKeys && ChannelKeys->Num() > 0;
			if (bHasDefault || bHasKeys)
			{
				return false;
			}
		}
		return true;
	}

	static void AppendCanonicalJsonObject(FString& Out, const TSharedPtr<FJsonObject>& Obj)
	{
		Out.AppendChar(TEXT('{'));
		if (Obj.IsValid())
		{
			TArray<FString> Keys;
			Obj->Values.GetKeys(Keys);
			Keys.Sort();
			bool bFirst = true;
			for (const FString& Key : Keys)
			{
				if (IsStructuralSnapshotIgnoredField(Key))
				{
					continue;
				}
				if (Key.Equals(TEXT("channels"), ESearchCase::IgnoreCase))
				{
					const TSharedPtr<FJsonValue> ChannelsValue = Obj->Values.FindRef(Key);
					if (IsStructuralChannelsValueEffectivelyEmpty(ChannelsValue))
					{
						continue;
					}
				}
				if (!bFirst)
				{
					Out.AppendChar(TEXT(','));
				}
				bFirst = false;
				AppendCanonicalJsonString(Out, Key);
				Out.AppendChar(TEXT(':'));
				if (IsStructuralRawPropertiesField(Key))
				{
					AppendCanonicalJsonValue(Out, MakeShared<FJsonValueArray>(BuildComparableRawPropertiesArray(Obj->Values.FindRef(Key))));
				}
				else
				{
					AppendCanonicalJsonValue(Out, Obj->Values.FindRef(Key));
				}
			}
		}
		Out.AppendChar(TEXT('}'));
	}

	static void AppendCanonicalJsonValue(FString& Out, const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid() || Value->Type == EJson::Null)
		{
			Out += TEXT("null");
			return;
		}
		switch (Value->Type)
		{
		case EJson::String:
			AppendCanonicalJsonString(Out, Value->AsString());
			break;
		case EJson::Number:
			Out += FString::Printf(TEXT("%.17g"), Value->AsNumber());
			break;
		case EJson::Boolean:
			Out += Value->AsBool() ? TEXT("true") : TEXT("false");
			break;
		case EJson::Array:
		{
			Out.AppendChar(TEXT('['));
			const TArray<TSharedPtr<FJsonValue>>& Array = Value->AsArray();
			for (int32 Index = 0; Index < Array.Num(); ++Index)
			{
				if (Index > 0)
				{
					Out.AppendChar(TEXT(','));
				}
				AppendCanonicalJsonValue(Out, Array[Index]);
			}
			Out.AppendChar(TEXT(']'));
			break;
		}
		case EJson::Object:
			AppendCanonicalJsonObject(Out, Value->AsObject());
			break;
		default:
			Out += TEXT("null");
			break;
		}
	}

	static FString CanonicalizeJsonObjectForStructuralSnapshot(const TSharedPtr<FJsonObject>& Obj)
	{
		FString Out;
		AppendCanonicalJsonObject(Out, Obj);
		return Out;
	}

	static void AddStructuralSnapshotEntry(TMap<FString, FString>& Entries, const FString& Key, const TSharedPtr<FJsonObject>& Obj)
	{
		if (Obj.IsValid())
		{
			Entries.Add(Key, CanonicalizeJsonObjectForStructuralSnapshot(Obj));
		}
	}

	static TSharedPtr<FJsonObject> BuildFolderAssetSnapshotJson(const FString& AssetPath, ULevelSequence* Sequence)
	{
		TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
		AssetObj->SetNumberField(TEXT("format_version"), 1);
		AssetObj->SetStringField(TEXT("schema"), TEXT("uai.level_sequence.folder.v1"));
		AssetObj->SetStringField(TEXT("asset_kind"), TEXT("level_sequence"));
		AssetObj->SetStringField(TEXT("asset_path"), AssetPath);
		AssetObj->SetStringField(TEXT("asset_name"), FPackageName::GetLongPackageAssetName(AssetPath));
		AssetObj->SetArrayField(TEXT("sequence_raw_properties"), BuildSequenceRawPropertiesJson(Sequence));
		return AssetObj;
	}

	static TSharedPtr<FJsonObject> BuildFolderSettingsSnapshotJson(UMovieScene* MovieScene)
	{
		TSharedPtr<FJsonObject> SettingsJson = MakeShared<FJsonObject>();
		if (!MovieScene)
		{
			return SettingsJson;
		}
		TSharedPtr<FJsonObject> PlaybackObj = MakeShared<FJsonObject>();
		const TRange<FFrameNumber> PlaybackRange = MovieScene->GetPlaybackRange();
		PlaybackObj->SetNumberField(TEXT("start_seconds"), MovieScene->GetTickResolution().AsSeconds(FFrameTime(PlaybackRange.GetLowerBoundValue())));
		PlaybackObj->SetNumberField(TEXT("duration_seconds"), MovieScene->GetTickResolution().AsSeconds(FFrameTime(PlaybackRange.GetUpperBoundValue() - PlaybackRange.GetLowerBoundValue())));
		SettingsJson->SetObjectField(TEXT("playback"), PlaybackObj);
		TSharedPtr<FJsonObject> DisplayRateObj = MakeShared<FJsonObject>();
		DisplayRateObj->SetNumberField(TEXT("numerator"), MovieScene->GetDisplayRate().Numerator);
		DisplayRateObj->SetNumberField(TEXT("denominator"), MovieScene->GetDisplayRate().Denominator);
		SettingsJson->SetObjectField(TEXT("display_rate"), DisplayRateObj);
		SettingsJson->SetArrayField(TEXT("movie_scene_raw_properties"), BuildSequenceRawPropertiesJson(MovieScene));
		return SettingsJson;
	}
