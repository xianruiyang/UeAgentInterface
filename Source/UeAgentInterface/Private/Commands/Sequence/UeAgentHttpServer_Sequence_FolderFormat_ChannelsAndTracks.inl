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

	static const FString* GSequenceDetailsEntryDirectory = nullptr;
	static FString* GSequenceDetailsExportError = nullptr;

	struct FScopedSequenceDetailsExportContext
	{
		const FString* PreviousEntryDirectory = nullptr;
		FString* PreviousError = nullptr;

		FScopedSequenceDetailsExportContext(const FString* EntryDirectory, FString* OutError)
			: PreviousEntryDirectory(GSequenceDetailsEntryDirectory)
			, PreviousError(GSequenceDetailsExportError)
		{
			GSequenceDetailsEntryDirectory = EntryDirectory;
			GSequenceDetailsExportError = OutError;
		}

		~FScopedSequenceDetailsExportContext()
		{
			GSequenceDetailsEntryDirectory = PreviousEntryDirectory;
			GSequenceDetailsExportError = PreviousError;
		}
	};

	static FString MakeDetailsStem(const FString& Category, const FString& OwnerId)
	{
		FString SafeOwnerId = FPaths::MakeValidFileName(OwnerId, TEXT('_'));
		if (SafeOwnerId.IsEmpty())
		{
			SafeOwnerId = TEXT("owner");
		}
		return FPaths::Combine(TEXT("details"), Category, SafeOwnerId);
	}

	static FString MakeObjectOwnerId(UObject* Object, const FString& Fallback)
	{
		if (Object)
		{
			const FString ObjectPath = Object->GetPathName();
			if (!ObjectPath.IsEmpty())
			{
				return ObjectPath;
			}
			const FString ObjectName = Object->GetName();
			if (!ObjectName.IsEmpty())
			{
				return ObjectName;
			}
		}
		return Fallback.IsEmpty() ? TEXT("owner") : Fallback;
	}

	static bool ExportFolderOwnerDetails(
		UObject* Object,
		const FString& EntryDirectory,
		const FString& SourceCommand,
		const FString& OwnerKind,
		const FString& OwnerId,
		const FString& DetailsCategory,
		const TSharedPtr<FJsonObject>& EntryObject,
		FString& OutError)
	{
		if (!Object || !EntryObject.IsValid())
		{
			return true;
		}

		const FString ResolvedOwnerId = OwnerId.IsEmpty() ? MakeObjectOwnerId(Object, TEXT("owner")) : OwnerId;
		const FString DetailsStem = MakeDetailsStem(DetailsCategory, ResolvedOwnerId);
		UeAgentDetailsAuthoring::FOwnerDetailsExportRequest Request =
			UeAgentDetailsAuthoring::MakeOwnerDetailsExportRequest(
				Object,
				OwnerKind,
				ResolvedOwnerId,
				FPaths::Combine(EntryDirectory, DetailsStem),
				SourceCommand,
				FPaths::Combine(DetailsStem, TEXT("index.json")),
				TEXT("details"));

		TSharedPtr<FJsonObject> DetailsSummary;
		if (!UeAgentDetailsAuthoring::ExportOwnerDetails(Request, DetailsSummary, OutError))
		{
			OutError = FString::Printf(TEXT("%s_details_export_failed:%s:%s"), *SourceCommand, *ResolvedOwnerId, *OutError);
			return false;
		}

		EntryObject->RemoveField(TEXT("properties"));
		EntryObject->SetObjectField(TEXT("details"), DetailsSummary);
		return true;
	}

	static UeAgentDetailsAuthoring::FOwnerInfo MakeApplyOwnerInfo(
		const FString& OwnerKind,
		const FString& OwnerId,
		const FString& SourceCommand,
		UObject* Object = nullptr)
	{
		return UeAgentDetailsAuthoring::MakeOwnerInfoForObject(
			Object,
			OwnerKind,
			OwnerId,
			SourceCommand);
	}

	static int32 GetGroupedDetailsAppliedCount(const TSharedPtr<FJsonObject>& ApplySummary)
	{
		if (!ApplySummary.IsValid())
		{
			return 0;
		}
		double Applied = 0.0;
		return ApplySummary->TryGetNumberField(TEXT("properties_applied"), Applied)
			? static_cast<int32>(Applied)
			: 0;
	}

	static bool ApplyFolderOwnerDetailsIfPresent(
		UObject* Object,
		const FString& EntryDirectory,
		const TSharedPtr<FJsonObject>& EntryObject,
		const FString& OwnerKind,
		const FString& OwnerId,
		const FString& SourceCommand,
		FString& OutError,
		int32& InOutAppliedCount)
	{
		bool bHasDetails = false;
		FString DetailsRoot;
		FString IndexFile;
		if (!UeAgentDetailsAuthoring::ResolveDetailsRefRoot(
			EntryDirectory,
			EntryObject,
			TEXT("details"),
			bHasDetails,
			DetailsRoot,
			IndexFile,
			OutError))
		{
			return false;
		}
		if (!bHasDetails)
		{
			return true;
		}
		if (!Object)
		{
			OutError = TEXT("details_owner_missing");
			return false;
		}

		UeAgentDetailsAuthoring::FOwnerDetailsApplyRequest Request;
		Request.Object = Object;
		Request.OwnerInfo = MakeApplyOwnerInfo(OwnerKind, OwnerId, SourceCommand, Object);
		Request.DetailsRoot = DetailsRoot;
		Request.IndexFile = IndexFile;
		Request.RefFieldName = TEXT("details");

		TSharedPtr<FJsonObject> ApplySummary;
		if (!UeAgentDetailsAuthoring::ApplyOwnerDetails(Request, ApplySummary, OutError))
		{
			return false;
		}
		bool bOk = true;
		if (ApplySummary.IsValid() && ApplySummary->HasField(TEXT("ok")))
		{
			bOk = ApplySummary->GetBoolField(TEXT("ok"));
		}
		if (!bOk)
		{
			OutError = TEXT("grouped_details_apply_failed");
			return false;
		}

		InOutAppliedCount += GetGroupedDetailsAppliedCount(ApplySummary);
		return true;
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
			FString SectionType;
			SectionObj->TryGetStringField(TEXT("section_type"), SectionType);
			UeAgentAuthoringJson::ApplyAuthoringMetadata(
				SectionObj,
				TEXT("section_name"),
				Section->GetName(),
				Section->GetName(),
				TEXT("Sequencer/Sections"),
				TEXT("sequencer_section"),
				SectionType.IsEmpty() ? TEXT("section") : SectionType,
				{ TEXT("set_range"), TEXT("edit_keys"), TEXT("remove") });
			if (GSequenceDetailsEntryDirectory && GSequenceDetailsExportError && GSequenceDetailsExportError->IsEmpty())
			{
				ExportFolderOwnerDetails(
					Section,
					*GSequenceDetailsEntryDirectory,
					TEXT("sequence_export_folder"),
					TEXT("sequence_section"),
					MakeObjectOwnerId(Section, Section->GetName()),
					TEXT("sections"),
					SectionObj,
					*GSequenceDetailsExportError);
			}
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

		const TSharedPtr<FJsonValue>* LocationValue = UeAgentJsonDiagnostics::FindFieldValue(KeyObj, TEXT("location"));
		FVector VectorValue = FVector::ZeroVector;
		if (LocationValue && ChannelIndex >= 0 && ChannelIndex <= 2 && UeAgentJsonDiagnostics::TryReadVectorValue(*LocationValue, VectorValue))
		{
			OutValue = (ChannelIndex == 0) ? VectorValue.X : (ChannelIndex == 1) ? VectorValue.Y : VectorValue.Z;
			return true;
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
		const TSharedPtr<FJsonValue>* RotationValue = UeAgentJsonDiagnostics::FindFieldValue(KeyObj, TEXT("rotation"));
		FRotator RotatorValue = FRotator::ZeroRotator;
		if (RotationValue && ChannelIndex >= 3 && ChannelIndex <= 5 && UeAgentJsonDiagnostics::TryReadRotatorValue(*RotationValue, RotatorValue))
		{
			OutValue = (ChannelIndex == 3) ? RotatorValue.Roll : (ChannelIndex == 4) ? RotatorValue.Pitch : RotatorValue.Yaw;
			return true;
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
		const TSharedPtr<FJsonValue>* ScaleValue = UeAgentJsonDiagnostics::FindFieldValue(KeyObj, TEXT("scale"));
		if (ScaleValue && ChannelIndex >= 6 && ChannelIndex <= 8 && UeAgentJsonDiagnostics::TryReadVectorValue(*ScaleValue, VectorValue))
		{
			const int32 ScaleChannel = ChannelIndex - 6;
			OutValue = (ScaleChannel == 0) ? VectorValue.X : (ScaleChannel == 1) ? VectorValue.Y : VectorValue.Z;
			return true;
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
		const TSharedPtr<FJsonValue>* RawValue = UeAgentJsonDiagnostics::FindFieldValue(KeyObj, TEXT("value"));
		const TArray<TSharedPtr<FJsonValue>>* ValueArray = nullptr;
		if (RawValue && (*RawValue).IsValid() && (*RawValue)->TryGetArray(ValueArray) && ValueArray && ValueArray->IsValidIndex(ChannelIndex))
		{
			if (UeAgentJsonDiagnostics::TryReadNumberValue((*ValueArray)[ChannelIndex], OutValue))
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
		int32* ChannelsApplied = nullptr,
		const FString* DetailsEntryDirectory = nullptr,
		FString* OutError = nullptr,
		int32* GroupedDetailsApplied = nullptr)
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

		if (DetailsEntryDirectory && GroupedDetailsApplied)
		{
			FString LocalError;
			if (!ApplyFolderOwnerDetailsIfPresent(
				Section,
				*DetailsEntryDirectory,
				SectionObj,
				TEXT("sequence_section"),
				MakeObjectOwnerId(Section, Section->GetName()),
				TEXT("sequence_apply_folder"),
				LocalError,
				*GroupedDetailsApplied))
			{
				if (OutError && OutError->IsEmpty())
				{
					*OutError = LocalError.IsEmpty() ? FString(TEXT("grouped_details_apply_failed")) : LocalError;
				}
				AddWarning(Warnings, TEXT("section_grouped_details_apply_failed"), FString::Printf(TEXT("Skipped grouped details for %s: %s"), *Context, *LocalError));
			}
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
		int32* ChannelsApplied = nullptr,
		const FString* DetailsEntryDirectory = nullptr,
		FString* OutError = nullptr,
		int32* GroupedDetailsApplied = nullptr)
	{
		if (!MovieScene || !Track || !SectionObj.IsValid())
		{
			return;
		}

	const TArray<UMovieSceneSection*>& Sections = Track->GetAllSections();
	if (Sections.Num() > 0 && Sections[0])
	{
		ApplySectionMetadataJson(MovieScene, Sections[0], SectionObj, Warnings, Context, RawPropertiesApplied, ChannelsApplied, DetailsEntryDirectory, OutError, GroupedDetailsApplied);
	}
}

#include "UeAgentHttpServer_Sequence_FolderFormat_TrackBuilders.inl"

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

	static TSharedPtr<FJsonObject> BuildTrackJson(UMovieScene* MovieScene, UMovieSceneTrack* Track, const FString* DetailsEntryDirectory = nullptr, FString* OutError = nullptr)
	{
		if (!MovieScene || !Track)
		{
			return nullptr;
		}
		FScopedSequenceDetailsExportContext DetailsContext(DetailsEntryDirectory, OutError);

		auto Finish = [MovieScene, Track, DetailsEntryDirectory, OutError](const TSharedPtr<FJsonObject>& TrackObj) -> TSharedPtr<FJsonObject>
		{
			AnnotateTrackRoundtrip(MovieScene, Track, TrackObj);
			if (TrackObj.IsValid())
			{
				FString TrackType;
				TrackObj->TryGetStringField(TEXT("track_type"), TrackType);
				UeAgentAuthoringJson::ApplyAuthoringMetadata(
					TrackObj,
					TEXT("track_name"),
					Track->GetName(),
					Track->GetName(),
					TEXT("Sequencer/Tracks"),
					TEXT("sequencer_track"),
					TrackType.IsEmpty() ? TEXT("track") : TrackType,
					{ TEXT("edit"), TEXT("add_section"), TEXT("remove_section") });
				if (DetailsEntryDirectory && OutError && OutError->IsEmpty())
				{
					ExportFolderOwnerDetails(
						Track,
						*DetailsEntryDirectory,
						TEXT("sequence_export_folder"),
						TEXT("sequence_track"),
						MakeObjectOwnerId(Track, Track->GetName()),
						TEXT("tracks"),
						TrackObj,
						*OutError);
				}
			}
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
		UeAgentAuthoringJson::ApplyAuthoringMetadata(
			BindingObj,
			TEXT("binding_guid"),
			Binding.GetObjectGuid().ToString(EGuidFormats::DigitsWithHyphensLower),
			Binding.GetName(),
			TEXT("Sequencer/Bindings"),
			TEXT("sequencer_binding"),
			TEXT("binding"),
			{ TEXT("edit_tracks"), TEXT("add_track"), TEXT("remove_binding") });

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

	static TSharedPtr<FJsonObject> BuildMasterTracksIndexJson(UMovieScene* MovieScene, FSequenceFolderCoverageStats* CoverageStats, const FString* DetailsEntryDirectory = nullptr, FString* OutError = nullptr);
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
			|| FieldName.Equals(TEXT("details"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("selector"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("category_path"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("control_type"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("value_kind"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("availability"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("operations"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("raw_properties"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("raw_properties_count"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("sequence_raw_properties"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("movie_scene_raw_properties"), ESearchCase::IgnoreCase)
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
		EntryObj->RemoveField(TEXT("details"));
		EntryObj->RemoveField(TEXT("selector"));
		EntryObj->RemoveField(TEXT("display_name"));
		EntryObj->RemoveField(TEXT("category_path"));
		EntryObj->RemoveField(TEXT("control_type"));
		EntryObj->RemoveField(TEXT("value_kind"));
		EntryObj->RemoveField(TEXT("availability"));
		EntryObj->RemoveField(TEXT("operations"));
		NormalizeTrackLikeJsonForStructuralSnapshot(EntryObj, TickResolution);
		const TArray<TSharedPtr<FJsonValue>>* Tracks = nullptr;
		if (EntryObj->TryGetArrayField(TEXT("tracks"), Tracks) && Tracks)
		{
			for (const TSharedPtr<FJsonValue>& TrackValue : *Tracks)
			{
				const TSharedPtr<FJsonObject>* TrackObj = nullptr;
				if (TrackValue.IsValid() && TrackValue->TryGetObject(TrackObj) && TrackObj && TrackObj->IsValid())
				{
					(*TrackObj)->RemoveField(TEXT("details"));
					(*TrackObj)->RemoveField(TEXT("selector"));
					(*TrackObj)->RemoveField(TEXT("display_name"));
					(*TrackObj)->RemoveField(TEXT("category_path"));
					(*TrackObj)->RemoveField(TEXT("control_type"));
					(*TrackObj)->RemoveField(TEXT("value_kind"));
					(*TrackObj)->RemoveField(TEXT("availability"));
					(*TrackObj)->RemoveField(TEXT("operations"));
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


	static bool AddInputFolderStructuralSnapshotEntries(const FString& FolderPath, const FFrameRate& TickResolution, TMap<FString, FString>& Entries, TArray<FString>& Warnings, FString& OutError)
	{
		auto LoadRequiredEntry = [&Entries, &OutError, &TickResolution](const FString& EntryKey, const FString& FilePath) -> bool
		{
			TSharedPtr<FJsonObject> Obj;
			FString ResolvedPath;
			if (!UeAgentSequenceOps::LoadJsonFile(FilePath, Obj, ResolvedPath, OutError))
			{
				return false;
			}
			NormalizeFolderEntryJsonForStructuralSnapshot(Obj, TickResolution);
			if (EntryKey.EndsWith(TEXT("/binding")))
			{
				Obj->RemoveField(TEXT("tracks"));
			}
			AddStructuralSnapshotEntry(Entries, EntryKey, Obj);
			return true;
		};
		auto LoadOptionalEntry = [&Entries, &Warnings, &TickResolution](const FString& EntryKey, const FString& FilePath)
		{
			if (!FPaths::FileExists(FilePath))
			{
				Warnings.Add(FString::Printf(TEXT("optional snapshot file missing: %s"), *FilePath));
				return;
			}
			TSharedPtr<FJsonObject> Obj;
			FString ResolvedPath;
			FString Error;
			if (UeAgentSequenceOps::LoadJsonFile(FilePath, Obj, ResolvedPath, Error))
			{
				NormalizeFolderEntryJsonForStructuralSnapshot(Obj, TickResolution);
				if (EntryKey.EndsWith(TEXT("/binding")))
				{
					Obj->RemoveField(TEXT("tracks"));
				}
				AddStructuralSnapshotEntry(Entries, EntryKey, Obj);
			}
			else
			{
				Warnings.Add(FString::Printf(TEXT("optional snapshot file failed to load: %s error=%s"), *FilePath, *Error));
			}
		};

		if (!LoadRequiredEntry(TEXT("asset"), FPaths::Combine(FolderPath, TEXT("asset.json")))) return false;
		if (!LoadRequiredEntry(TEXT("settings/sequence"), FPaths::Combine(FolderPath, TEXT("settings"), TEXT("sequence.json")))) return false;
		if (!LoadRequiredEntry(TEXT("bindings/index"), FPaths::Combine(FolderPath, TEXT("bindings"), TEXT("index.json")))) return false;
		LoadOptionalEntry(TEXT("outliner/folders"), FPaths::Combine(FolderPath, TEXT("outliner"), TEXT("folders.json")));
		LoadOptionalEntry(TEXT("master_tracks/index"), FPaths::Combine(FolderPath, TEXT("master_tracks"), TEXT("index.json")));

		TSharedPtr<FJsonObject> BindingsIndexJson;
		FString ResolvedBindingsIndexPath;
		if (!UeAgentSequenceOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("bindings"), TEXT("index.json")), BindingsIndexJson, ResolvedBindingsIndexPath, OutError))
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* BindingEntries = nullptr;
		if (BindingsIndexJson.IsValid() && BindingsIndexJson->TryGetArrayField(TEXT("bindings"), BindingEntries) && BindingEntries)
		{
			for (const TSharedPtr<FJsonValue>& BindingEntryValue : *BindingEntries)
			{
				const TSharedPtr<FJsonObject>* BindingEntryObj = nullptr;
				if (!BindingEntryValue.IsValid() || !BindingEntryValue->TryGetObject(BindingEntryObj) || !BindingEntryObj || !BindingEntryObj->IsValid())
				{
					continue;
				}
				FString BindingGuidText;
				FString BindingFolderName;
				(*BindingEntryObj)->TryGetStringField(TEXT("binding_guid"), BindingGuidText);
				(*BindingEntryObj)->TryGetStringField(TEXT("folder_name"), BindingFolderName);
				if (BindingFolderName.IsEmpty())
				{
					continue;
				}
				const FString BindingPrefix = FString::Printf(TEXT("bindings/%s"), *BindingGuidText);
				if (!LoadRequiredEntry(BindingPrefix + TEXT("/binding"), FPaths::Combine(FolderPath, TEXT("bindings"), BindingFolderName, TEXT("binding.json"))))
				{
					return false;
				}
				const FString TracksDir = FPaths::Combine(FolderPath, TEXT("bindings"), BindingFolderName, TEXT("tracks"));
				TArray<FString> TrackFiles;
				IFileManager::Get().FindFiles(TrackFiles, *(FPaths::Combine(TracksDir, TEXT("*.json"))), true, false);
				TrackFiles.Sort();
				TMap<FString, int32> SeenTrackKeys;
				for (const FString& TrackFile : TrackFiles)
				{
					TSharedPtr<FJsonObject> TrackObj;
					FString ResolvedTrackPath;
					if (!UeAgentSequenceOps::LoadJsonFile(FPaths::Combine(TracksDir, TrackFile), TrackObj, ResolvedTrackPath, OutError))
					{
						return false;
					}
					NormalizeFolderEntryJsonForStructuralSnapshot(TrackObj, TickResolution);
					AddStructuralSnapshotEntry(Entries, BindingPrefix + TEXT("/tracks/") + MakeStructuralTrackEntryKey(TrackObj, SeenTrackKeys), TrackObj);
				}
			}
		}
		return true;
	}

	static bool AddCurrentSequenceStructuralSnapshotEntries(const FString& AssetPath, ULevelSequence* Sequence, UMovieScene* MovieScene, TMap<FString, FString>& Entries, FString& OutError)
	{
		if (!Sequence || !MovieScene)
		{
			OutError = TEXT("movie_scene_not_found");
			return false;
		}

		AddStructuralSnapshotEntry(Entries, TEXT("asset"), BuildFolderAssetSnapshotJson(AssetPath, Sequence));
		AddStructuralSnapshotEntry(Entries, TEXT("settings/sequence"), BuildFolderSettingsSnapshotJson(MovieScene));

		TArray<TSharedPtr<FJsonValue>> BindingIndexEntries;
		TMap<const UMovieSceneTrack*, TSharedPtr<FJsonObject>> BindingTrackRefsByTrack;
		const TArray<FMovieSceneBinding>& Bindings = MovieScene->GetBindings();
		for (const FMovieSceneBinding& Binding : Bindings)
		{
			const FString BindingFolderName = MakeBindingFolderName(Binding);
			const FString BindingGuidText = Binding.GetObjectGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
			TSharedPtr<FJsonObject> BindingIndexObj = MakeShared<FJsonObject>();
			BindingIndexObj->SetStringField(TEXT("binding_guid"), BindingGuidText);
			BindingIndexObj->SetStringField(TEXT("binding_id"), BindingGuidText);
			BindingIndexObj->SetStringField(TEXT("binding_name"), Binding.GetName());
			BindingIndexObj->SetStringField(TEXT("folder_name"), BindingFolderName);
			BindingIndexEntries.Add(MakeShared<FJsonValueObject>(BindingIndexObj));

			const FString BindingPrefix = FString::Printf(TEXT("bindings/%s"), *BindingGuidText);
			TSharedPtr<FJsonObject> BindingJson = BuildBindingJson(Sequence, MovieScene, Binding);
			if (BindingJson.IsValid())
			{
				BindingJson->RemoveField(TEXT("tracks"));
			}
			AddStructuralSnapshotEntry(Entries, BindingPrefix + TEXT("/binding"), BindingJson);
			TMap<FString, int32> SeenTrackKeys;
			for (int32 TrackIndex = 0; TrackIndex < Binding.GetTracks().Num(); ++TrackIndex)
			{
				UMovieSceneTrack* Track = Binding.GetTracks()[TrackIndex];
				if (!Track)
				{
					continue;
				}
				TSharedPtr<FJsonObject> TrackJson = BuildTrackJson(MovieScene, Track);
				if (!TrackJson.IsValid())
				{
					continue;
				}
				NormalizeFolderEntryJsonForStructuralSnapshot(TrackJson, MovieScene->GetTickResolution());
				const FString TrackFileName = MakeTrackFileName(Track, TrackIndex);
				AddStructuralSnapshotEntry(Entries, BindingPrefix + TEXT("/tracks/") + MakeStructuralTrackEntryKey(TrackJson, SeenTrackKeys), TrackJson);
				if (TSharedPtr<FJsonObject> TrackRefObj = BuildBindingTrackRefJson(Binding.GetObjectGuid(), TrackFileName, TrackJson))
				{
					BindingTrackRefsByTrack.Add(Track, TrackRefObj);
				}
			}
		}

		TSharedPtr<FJsonObject> BindingsIndexJson = MakeShared<FJsonObject>();
		BindingsIndexJson->SetArrayField(TEXT("bindings"), BindingIndexEntries);
		AddStructuralSnapshotEntry(Entries, TEXT("bindings/index"), BindingsIndexJson);
		AddStructuralSnapshotEntry(Entries, TEXT("outliner/folders"), BuildOutlinerFoldersJson(MovieScene, BindingTrackRefsByTrack));
		TSharedPtr<FJsonObject> MasterTracksIndexJson = BuildMasterTracksIndexJson(MovieScene, nullptr);
		NormalizeFolderEntryJsonForStructuralSnapshot(MasterTracksIndexJson, MovieScene->GetTickResolution());
		AddStructuralSnapshotEntry(Entries, TEXT("master_tracks/index"), MasterTracksIndexJson);
		return true;
	}

	static TSharedPtr<FJsonObject> BuildStructuralSnapshotDiffJson(
		const FString& FolderPath,
		const FString& AssetPath,
		ULevelSequence* Sequence,
		UMovieScene* MovieScene,
		const int32 MaxDifferenceEntries,
		FString& OutError)
	{
		TSharedPtr<FJsonObject> DiffObj = MakeShared<FJsonObject>();
		DiffObj->SetStringField(TEXT("schema"), TEXT("uai.sequence.folder_structural_diff.v1"));
		DiffObj->SetStringField(TEXT("folder_path"), FolderPath);
		DiffObj->SetStringField(TEXT("asset_path"), AssetPath);

		TMap<FString, FString> InputEntries;
		TMap<FString, FString> PostEntries;
		TArray<FString> SnapshotWarnings;
		const FFrameRate SnapshotTickResolution = MovieScene ? MovieScene->GetTickResolution() : FFrameRate(24000, 1);
		if (!AddInputFolderStructuralSnapshotEntries(FolderPath, SnapshotTickResolution, InputEntries, SnapshotWarnings, OutError))
		{
			DiffObj->SetBoolField(TEXT("success"), false);
			DiffObj->SetStringField(TEXT("error"), OutError);
			SetStringArrayFieldFromValues(DiffObj, TEXT("warnings"), SnapshotWarnings);
			return DiffObj;
		}
		if (!AddCurrentSequenceStructuralSnapshotEntries(AssetPath, Sequence, MovieScene, PostEntries, OutError))
		{
			DiffObj->SetBoolField(TEXT("success"), false);
			DiffObj->SetStringField(TEXT("error"), OutError);
			SetStringArrayFieldFromValues(DiffObj, TEXT("warnings"), SnapshotWarnings);
			return DiffObj;
		}

		TArray<FString> InputKeys;
		TArray<FString> PostKeys;
		InputEntries.GetKeys(InputKeys);
		PostEntries.GetKeys(PostKeys);
		InputKeys.Sort();
		PostKeys.Sort();
		TSet<FString> InputKeySet;
		for (const FString& Key : InputKeys)
		{
			InputKeySet.Add(Key);
		}
		TSet<FString> PostKeySet;
		for (const FString& Key : PostKeys)
		{
			PostKeySet.Add(Key);
		}

		TArray<FString> MissingInPost;
		TArray<FString> ExtraInPost;
		TArray<TSharedPtr<FJsonValue>> ChangedEntries;
		for (const FString& Key : InputKeys)
		{
			if (!PostKeySet.Contains(Key))
			{
				MissingInPost.Add(Key);
			}
			else if (InputEntries.FindRef(Key) != PostEntries.FindRef(Key) && ChangedEntries.Num() < FMath::Max(0, MaxDifferenceEntries))
			{
				const FString InputCanonical = InputEntries.FindRef(Key);
				const FString PostCanonical = PostEntries.FindRef(Key);
				int32 FirstDifferenceOffset = 0;
				const int32 CommonLength = FMath::Min(InputCanonical.Len(), PostCanonical.Len());
				while (FirstDifferenceOffset < CommonLength && InputCanonical[FirstDifferenceOffset] == PostCanonical[FirstDifferenceOffset])
				{
					++FirstDifferenceOffset;
				}
				const int32 DifferencePreviewStart = FMath::Max(0, FirstDifferenceOffset - 160);
				TSharedPtr<FJsonObject> ChangeObj = MakeShared<FJsonObject>();
				ChangeObj->SetStringField(TEXT("entry"), Key);
				ChangeObj->SetNumberField(TEXT("input_canonical_length"), InputCanonical.Len());
				ChangeObj->SetNumberField(TEXT("post_canonical_length"), PostCanonical.Len());
				ChangeObj->SetNumberField(TEXT("first_difference_offset"), FirstDifferenceOffset);
				ChangeObj->SetStringField(TEXT("input_canonical_preview"), InputCanonical.Left(512));
				ChangeObj->SetStringField(TEXT("post_canonical_preview"), PostCanonical.Left(512));
				ChangeObj->SetStringField(TEXT("input_difference_preview"), InputCanonical.Mid(DifferencePreviewStart, 768));
				ChangeObj->SetStringField(TEXT("post_difference_preview"), PostCanonical.Mid(DifferencePreviewStart, 768));
				ChangedEntries.Add(MakeShared<FJsonValueObject>(ChangeObj));
			}
		}
		for (const FString& Key : PostKeys)
		{
			if (!InputKeySet.Contains(Key))
			{
				ExtraInPost.Add(Key);
			}
		}

		int32 ChangedCount = 0;
		for (const FString& Key : InputKeys)
		{
			if (PostKeySet.Contains(Key) && InputEntries.FindRef(Key) != PostEntries.FindRef(Key))
			{
				++ChangedCount;
			}
		}

		DiffObj->SetBoolField(TEXT("success"), true);
		DiffObj->SetNumberField(TEXT("input_entry_count"), InputEntries.Num());
		DiffObj->SetNumberField(TEXT("post_entry_count"), PostEntries.Num());
		DiffObj->SetNumberField(TEXT("missing_entry_count"), MissingInPost.Num());
		DiffObj->SetNumberField(TEXT("extra_entry_count"), ExtraInPost.Num());
		DiffObj->SetNumberField(TEXT("changed_entry_count"), ChangedCount);
		DiffObj->SetBoolField(TEXT("is_structurally_equal"), MissingInPost.Num() == 0 && ExtraInPost.Num() == 0 && ChangedCount == 0);
		SetStringArrayFieldFromValues(DiffObj, TEXT("missing_entries_in_post_apply"), MissingInPost);
		SetStringArrayFieldFromValues(DiffObj, TEXT("extra_entries_in_post_apply"), ExtraInPost);
		DiffObj->SetArrayField(TEXT("changed_entries"), ChangedEntries);
		SetStringArrayFieldFromValues(DiffObj, TEXT("warnings"), SnapshotWarnings);
		return DiffObj;
	}

	static bool ScanFolderInputCoverage(const FString& FolderPath, FSequenceFolderCoverageStats& OutStats, FString& OutError)
	{
		TSharedPtr<FJsonObject> BindingsIndexJson;
		FString BindingsIndexResolved;
		if (!UeAgentSequenceOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("bindings"), TEXT("index.json")), BindingsIndexJson, BindingsIndexResolved, OutError))
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* BindingEntries = nullptr;
		if (BindingsIndexJson.IsValid() && BindingsIndexJson->TryGetArrayField(TEXT("bindings"), BindingEntries) && BindingEntries)
		{
			for (const TSharedPtr<FJsonValue>& BindingEntryValue : *BindingEntries)
			{
				const TSharedPtr<FJsonObject>* BindingEntryObj = nullptr;
				if (!BindingEntryValue.IsValid() || !BindingEntryValue->TryGetObject(BindingEntryObj) || !BindingEntryObj || !BindingEntryObj->IsValid())
				{
					continue;
				}

				FString BindingGuidText;
				FString BindingFolderName;
				(*BindingEntryObj)->TryGetStringField(TEXT("binding_guid"), BindingGuidText);
				(*BindingEntryObj)->TryGetStringField(TEXT("folder_name"), BindingFolderName);
				if (BindingFolderName.IsEmpty())
				{
					continue;
				}

				const FString TracksDir = FPaths::Combine(FolderPath, TEXT("bindings"), BindingFolderName, TEXT("tracks"));
				TArray<FString> TrackFiles;
				IFileManager::Get().FindFiles(TrackFiles, *(FPaths::Combine(TracksDir, TEXT("*.json"))), true, false);
				TrackFiles.Sort();
				for (const FString& TrackFile : TrackFiles)
				{
					TSharedPtr<FJsonObject> TrackJson;
					FString ResolvedTrackPath;
					if (!UeAgentSequenceOps::LoadJsonFile(FPaths::Combine(TracksDir, TrackFile), TrackJson, ResolvedTrackPath, OutError))
					{
						return false;
					}
					RecordTrackCoverage(OutStats, nullptr, nullptr, TEXT("binding"), BindingGuidText, BindingFolderName, TrackFile, TrackJson);
				}
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
					if (!MasterTrackValue.IsValid() || !MasterTrackValue->TryGetObject(MasterTrackObj) || !MasterTrackObj || !MasterTrackObj->IsValid())
					{
						continue;
					}
					FString TrackType;
					(*MasterTrackObj)->TryGetStringField(TEXT("track_type"), TrackType);
					RecordTrackCoverage(OutStats, nullptr, nullptr, TEXT("master"), TEXT(""), TEXT(""), TrackType, *MasterTrackObj);
				}
			}
		}
		return true;
	}

	static TSharedPtr<FJsonObject> BuildMasterTracksIndexJson(UMovieScene* MovieScene, FSequenceFolderCoverageStats* CoverageStats, const FString* DetailsEntryDirectory, FString* OutError)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Tracks;
		if (MovieScene)
		{
			if (UMovieSceneTrack* CameraCutTrack = MovieScene->GetCameraCutTrack())
			{
				if (TSharedPtr<FJsonObject> TrackObj = BuildTrackJson(MovieScene, CameraCutTrack, DetailsEntryDirectory, OutError))
				{
					if (CoverageStats)
					{
						RecordTrackCoverage(*CoverageStats, MovieScene, CameraCutTrack, TEXT("master"), TEXT(""), TEXT(""), TEXT("camera_cut"), TrackObj);
					}
					Tracks.Add(MakeShared<FJsonValueObject>(TrackObj));
				}
			}
			for (int32 TrackIndex = 0; TrackIndex < MovieScene->GetTracks().Num(); ++TrackIndex)
			{
				UMovieSceneTrack* Track = MovieScene->GetTracks()[TrackIndex];
				if (Track == MovieScene->GetCameraCutTrack())
				{
					continue;
				}
				if (TSharedPtr<FJsonObject> TrackObj = BuildTrackJson(MovieScene, Track, DetailsEntryDirectory, OutError))
				{
					if (CoverageStats)
					{
						RecordTrackCoverage(*CoverageStats, MovieScene, Track, TEXT("master"), TEXT(""), TEXT(""), MakeTrackFileName(Track, TrackIndex), TrackObj);
					}
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
		Sequence->UnbindPossessableObjects(DesiredGuid);
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
		const FString& TemplateName,
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

		const FString EffectiveTemplateName = TemplateName.IsEmpty() ? BindingName : TemplateName;
		UObject* TemplateObject = Sequence->MakeSpawnableTemplateFromInstance(SourceObject, FName(*EffectiveTemplateName));
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

			FString SpawnableTemplateName;
			(*BindingObj)->TryGetStringField(TEXT("spawnable_template_name"), SpawnableTemplateName);
			if (!AddSpawnableBindingDirect(Sequence, MovieScene, DesiredGuid, BindingName, SpawnableTemplateName, *SourceObject, OutError))
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
		if (TrackType.Equals(TEXT("control_rig"), ESearchCase::IgnoreCase))
		{
			FString ControlRigClassPath;
			TryGetString(TrackObj, TEXT("control_rig_class"), ControlRigClassPath);
			TArray<UMovieSceneTrack*> Tracks = MovieScene->FindTracks(UMovieSceneControlRigParameterTrack::StaticClass(), BindingGuid, NAME_None);
			for (UMovieSceneTrack* Track : Tracks)
			{
				UMovieSceneControlRigParameterTrack* ControlRigTrack = Cast<UMovieSceneControlRigParameterTrack>(Track);
				if (!ControlRigTrack)
				{
					continue;
				}
				if (ControlRigClassPath.IsEmpty())
				{
					return ControlRigTrack;
				}
				UControlRig* ControlRig = ControlRigTrack->GetControlRig();
				if (ControlRig && ControlRig->GetClass() && ControlRig->GetClass()->GetPathName().Equals(ControlRigClassPath, ESearchCase::IgnoreCase))
				{
					return ControlRigTrack;
				}
			}
			return nullptr;
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

	static bool IsDefaultAuthoringRawFieldName(const FString& FieldName)
	{
		return FieldName.Equals(TEXT("raw_properties"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("raw_properties_count"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("sequence_raw_properties"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("movie_scene_raw_properties"), ESearchCase::IgnoreCase)
			|| FieldName.EndsWith(TEXT("_raw_properties"), ESearchCase::IgnoreCase)
			|| FieldName.EndsWith(TEXT("_raw_properties_count"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("readonly_properties"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("readback_diff"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("diagnostics"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("debug_diagnostics"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("compile_report"), ESearchCase::IgnoreCase)
			|| FieldName.Equals(TEXT("runtime_probe_report"), ESearchCase::IgnoreCase);
	}

	static void StripDefaultAuthoringRawFieldsFromValue(const TSharedPtr<FJsonValue>& Value);

	static void StripDefaultAuthoringRawFieldsFromObject(const TSharedPtr<FJsonObject>& Obj)
	{
		if (!Obj.IsValid())
		{
			return;
		}

		TArray<FString> KeysToRemove;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Obj->Values)
		{
			if (IsDefaultAuthoringRawFieldName(Pair.Key))
			{
				KeysToRemove.Add(Pair.Key);
				continue;
			}
			StripDefaultAuthoringRawFieldsFromValue(Pair.Value);
		}
		for (const FString& Key : KeysToRemove)
		{
			Obj->Values.Remove(Key);
		}
	}

	static void StripDefaultAuthoringRawFieldsFromValue(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return;
		}
		if (Value->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (Value->TryGetObject(Obj) && Obj && Obj->IsValid())
			{
				StripDefaultAuthoringRawFieldsFromObject(*Obj);
			}
			return;
		}
		if (Value->Type == EJson::Array)
		{
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
			{
				StripDefaultAuthoringRawFieldsFromValue(Item);
			}
		}
	}

	static UClass* LoadControlRigClassFromPath(const FString& InClassPath)
	{
		const FString ClassPath = InClassPath.TrimStartAndEnd();
		if (ClassPath.IsEmpty())
		{
			return nullptr;
		}
		if (UClass* LoadedClass = LoadObject<UClass>(nullptr, *ClassPath))
		{
			return LoadedClass->IsChildOf(UControlRig::StaticClass()) ? LoadedClass : nullptr;
		}
		UObject* LoadedObject = StaticLoadObject(UObject::StaticClass(), nullptr, *ClassPath);
		if (UBlueprint* Blueprint = Cast<UBlueprint>(LoadedObject))
		{
			UClass* GeneratedClass = Blueprint->GeneratedClass;
			return GeneratedClass && GeneratedClass->IsChildOf(UControlRig::StaticClass()) ? GeneratedClass : nullptr;
		}
		return nullptr;
	}

	static UObject* ResolveControlRigBoundObject(ULevelSequence* Sequence, const FGuid& BindingGuid)
	{
		if (!Sequence || !GEditor || !BindingGuid.IsValid())
		{
			return nullptr;
		}

		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			return nullptr;
		}

		TArray<UObject*, TInlineAllocator<1>> BoundObjects;
		Sequence->LocateBoundObjects(BindingGuid, UE::UniversalObjectLocator::FResolveParams(World), nullptr, BoundObjects);
		for (UObject* BoundObject : BoundObjects)
		{
			if (USkeletalMeshComponent* SkeletalMeshComponent = Cast<USkeletalMeshComponent>(BoundObject))
			{
				return SkeletalMeshComponent;
			}
			if (AActor* Actor = Cast<AActor>(BoundObject))
			{
				if (USkeletalMeshComponent* SkeletalMeshComponent = Actor->FindComponentByClass<USkeletalMeshComponent>())
				{
					return SkeletalMeshComponent;
				}
			}
		}

		return BoundObjects.Num() > 0 ? BoundObjects[0] : nullptr;
	}

	static UMovieSceneControlRigParameterTrack* FindOrCreateControlRigTrack(
		ULevelSequence* Sequence,
		UMovieScene* MovieScene,
		const FGuid& BindingGuid,
		UClass* ControlRigClass,
		const bool bIsAdditive,
		bool& bOutCreated,
		FString& OutError)
	{
		bOutCreated = false;
		if (!Sequence || !MovieScene || !BindingGuid.IsValid() || !ControlRigClass)
		{
			OutError = TEXT("invalid_control_rig_track_context");
			return nullptr;
		}

		TArray<UMovieSceneTrack*> ExistingTracks = MovieScene->FindTracks(UMovieSceneControlRigParameterTrack::StaticClass(), BindingGuid, NAME_None);
		for (UMovieSceneTrack* Track : ExistingTracks)
		{
			UMovieSceneControlRigParameterTrack* ControlRigTrack = Cast<UMovieSceneControlRigParameterTrack>(Track);
			if (ControlRigTrack && ControlRigTrack->GetControlRig() && ControlRigTrack->GetControlRig()->GetClass() == ControlRigClass)
			{
				return ControlRigTrack;
			}
		}

		Sequence->Modify();
		MovieScene->Modify();
		UMovieSceneControlRigParameterTrack* Track = MovieScene->AddTrack<UMovieSceneControlRigParameterTrack>(BindingGuid);
		if (!Track)
		{
			OutError = TEXT("create_control_rig_track_failed");
			return nullptr;
		}

		FString ObjectName = ControlRigClass->GetName();
		ObjectName.RemoveFromEnd(TEXT("_C"));
		UControlRig* ControlRig = NewObject<UControlRig>(Track, ControlRigClass, FName(*ObjectName), RF_Transactional);
		if (!ControlRig)
		{
			OutError = TEXT("create_control_rig_instance_failed");
			return nullptr;
		}

		ControlRig->Modify();
		if (UFKControlRig* FKControlRig = Cast<UFKControlRig>(ControlRig))
		{
			FKControlRig->SetApplyMode(bIsAdditive ? EControlRigFKRigExecuteMode::Additive : EControlRigFKRigExecuteMode::Replace);
		}
		else
		{
			ControlRig->SetIsAdditive(bIsAdditive);
		}
		ControlRig->SetObjectBinding(MakeShared<FControlRigObjectBinding>());
		if (const TSharedPtr<IControlRigObjectBinding> ObjectBinding = ControlRig->GetObjectBinding())
		{
			if (UObject* BoundObject = ResolveControlRigBoundObject(Sequence, BindingGuid))
			{
				ObjectBinding->BindToObject(BoundObject);
			}
		}
		ControlRig->Initialize();
		ControlRig->Evaluate_AnyThread();

		Track->Modify();
		Track->SetTrackName(FName(*ObjectName));
		Track->SetDisplayName(FText::FromString(bIsAdditive ? ObjectName + TEXT(" (Layered)") : ObjectName));
		UMovieSceneSection* NewSection = Track->CreateControlRigSection(0, ControlRig, true);
		if (NewSection)
		{
			NewSection->Modify();
		}
		bOutCreated = true;
		return Track;
	}

	static bool ApplyControlRigKeyJson(
		UMovieScene* MovieScene,
		UControlRig* ControlRig,
		UMovieSceneControlRigParameterSection* Section,
		const TSharedPtr<FJsonObject>& KeyObj,
		TArray<TSharedPtr<FJsonValue>>& Warnings,
		int32& KeysApplied,
		FString& OutError)
	{
		if (!MovieScene || !ControlRig || !Section || !KeyObj.IsValid())
		{
			OutError = TEXT("invalid_control_rig_key_context");
			return false;
		}

		FString ControlNameText;
		if (!KeyObj->TryGetStringField(TEXT("control"), ControlNameText) || ControlNameText.IsEmpty())
		{
			if (!KeyObj->TryGetStringField(TEXT("control_name"), ControlNameText) || ControlNameText.IsEmpty())
			{
				KeyObj->TryGetStringField(TEXT("parameter_name"), ControlNameText);
			}
		}
		if (ControlNameText.IsEmpty())
		{
			AddWarning(Warnings, TEXT("control_rig_key_missing_control"), TEXT("Skipped a Control Rig key because it has no control/control_name field."));
			return true;
		}

		FFrameNumber KeyFrame;
		if (!TryReadControlRigKeyFrame(MovieScene, KeyObj, KeyFrame))
		{
			AddWarning(Warnings, TEXT("control_rig_key_missing_time"), FString::Printf(TEXT("Skipped Control Rig key for '%s' because it has no time_seconds or frame."), *ControlNameText));
			return true;
		}

		const FName ControlName(*ControlNameText);
		FRigControlElement* ControlElement = ControlRig->FindControl(ControlName);
		if (!ControlElement)
		{
			AddWarning(Warnings, TEXT("control_rig_control_not_found"), FString::Printf(TEXT("Skipped Control Rig key for missing control '%s'."), *ControlNameText));
			return true;
		}
		if (URigHierarchy* Hierarchy = ControlRig->GetHierarchy())
		{
			if (!Hierarchy->IsAnimatable(ControlElement))
			{
				AddWarning(Warnings, TEXT("control_rig_control_not_animatable"), FString::Printf(TEXT("Skipped non-animatable Control Rig control '%s'."), *ControlNameText));
				return true;
			}
		}

		const TSharedPtr<FJsonValue>* ValueJson = UeAgentJsonDiagnostics::FindFieldValue(KeyObj, TEXT("value"));
		const TSharedPtr<FJsonObject>* ValueObj = nullptr;
		const bool bHasValueObject = ValueJson && (*ValueJson).IsValid() && (*ValueJson)->TryGetObject(ValueObj) && ValueObj && ValueObj->IsValid();
		double NumberValue = 0.0;
		bool BoolValue = false;

		Section->Modify();
		switch (ControlElement->Settings.ControlType)
		{
		case ERigControlType::Float:
		case ERigControlType::ScaleFloat:
			if (!KeyObj->TryGetNumberField(TEXT("value"), NumberValue))
			{
				AddWarning(Warnings, TEXT("control_rig_key_invalid_value"), FString::Printf(TEXT("Skipped scalar Control Rig key for '%s' because value is not numeric."), *ControlNameText));
				return true;
			}
			Section->AddScalarParameterKey(ControlName, KeyFrame, static_cast<float>(NumberValue));
			break;

		case ERigControlType::Bool:
			if (!KeyObj->TryGetBoolField(TEXT("value"), BoolValue))
			{
				AddWarning(Warnings, TEXT("control_rig_key_invalid_value"), FString::Printf(TEXT("Skipped bool Control Rig key for '%s' because value is not boolean."), *ControlNameText));
				return true;
			}
			Section->AddBoolParameterKey(ControlName, KeyFrame, BoolValue);
			break;

		case ERigControlType::Integer:
			if (ControlElement->Settings.ControlEnum)
			{
				FString ValueName;
				if (KeyObj->TryGetStringField(TEXT("value_name"), ValueName) && !ValueName.IsEmpty())
				{
					const int64 EnumValue = ControlElement->Settings.ControlEnum->GetValueByNameString(ValueName);
					if (EnumValue == INDEX_NONE)
					{
						AddWarning(Warnings, TEXT("control_rig_enum_value_not_found"), FString::Printf(TEXT("Skipped enum Control Rig key for '%s' because value_name '%s' was not found."), *ControlNameText, *ValueName));
						return true;
					}
					Section->AddEnumParameterKey(ControlName, KeyFrame, static_cast<uint8>(EnumValue));
				}
				else if (KeyObj->TryGetNumberField(TEXT("value"), NumberValue))
				{
					Section->AddEnumParameterKey(ControlName, KeyFrame, static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(NumberValue), 0, 255)));
				}
				else
				{
					AddWarning(Warnings, TEXT("control_rig_key_invalid_value"), FString::Printf(TEXT("Skipped enum Control Rig key for '%s' because it has no value/value_name."), *ControlNameText));
					return true;
				}
			}
			else
			{
				if (!KeyObj->TryGetNumberField(TEXT("value"), NumberValue))
				{
					AddWarning(Warnings, TEXT("control_rig_key_invalid_value"), FString::Printf(TEXT("Skipped integer Control Rig key for '%s' because value is not numeric."), *ControlNameText));
					return true;
				}
				Section->AddIntegerParameterKey(ControlName, KeyFrame, FMath::RoundToInt(NumberValue));
			}
			break;

		case ERigControlType::Vector2D:
		{
			FVector2D Value;
			if (!ValueJson || !TryReadControlRigVector2DValue(*ValueJson, Value))
			{
				AddWarning(Warnings, TEXT("control_rig_key_invalid_value"), FString::Printf(TEXT("Skipped vector2d Control Rig key for '%s' because value is invalid."), *ControlNameText));
				return true;
			}
			Section->AddVector2DParameterKey(ControlName, KeyFrame, Value);
			break;
		}

		case ERigControlType::Position:
		case ERigControlType::Scale:
		{
			FVector Value;
			if (!ValueJson || !TryReadControlRigVectorValue(*ValueJson, Value))
			{
				AddWarning(Warnings, TEXT("control_rig_key_invalid_value"), FString::Printf(TEXT("Skipped vector Control Rig key for '%s' because value is invalid."), *ControlNameText));
				return true;
			}
			Section->AddVectorParameterKey(ControlName, KeyFrame, Value);
			break;
		}

		case ERigControlType::Rotator:
		{
			FRotator Value;
			if (!ValueJson || !TryReadControlRigRotatorValue(*ValueJson, Value))
			{
				AddWarning(Warnings, TEXT("control_rig_key_invalid_value"), FString::Printf(TEXT("Skipped rotator Control Rig key for '%s' because value is invalid."), *ControlNameText));
				return true;
			}
			Section->AddVectorParameterKey(ControlName, KeyFrame, FVector(Value.Roll, Value.Pitch, Value.Yaw));
			break;
		}

		case ERigControlType::Transform:
		case ERigControlType::EulerTransform:
		case ERigControlType::TransformNoScale:
		{
			FTransform Value;
			if (!bHasValueObject || !TryReadControlRigTransform(*ValueObj, Value, ControlElement->Settings.ControlType != ERigControlType::TransformNoScale))
			{
				AddWarning(Warnings, TEXT("control_rig_key_invalid_value"), FString::Printf(TEXT("Skipped transform Control Rig key for '%s' because value is invalid."), *ControlNameText));
				return true;
			}
			Section->AddTransformParameterKey(ControlName, KeyFrame, Value);
			break;
		}

		default:
			AddWarning(Warnings, TEXT("control_rig_control_type_not_supported"), FString::Printf(TEXT("Skipped unsupported Control Rig control type '%s' for '%s'."), *ControlRigControlTypeToString(ControlElement->Settings.ControlType), *ControlNameText));
			return true;
		}

		Section->AutoSetTangents(ControlName);
		++KeysApplied;
		return true;
	}

	static bool ApplyControlRigTrackJson(
		ULevelSequence* Sequence,
		UMovieScene* MovieScene,
		const FGuid& BindingGuid,
		const FString& BindingGuidText,
		const TSharedPtr<FJsonObject>& TrackJson,
		TArray<TSharedPtr<FJsonValue>>& Warnings,
		int32& TracksApplied,
		int32& KeysApplied,
		int32& RawPropertiesApplied,
		int32& ChannelsApplied,
		int32& GroupedDetailsApplied,
		const FString& EntryDirectory,
		FString& OutError)
	{
		FString ControlRigClassPath = TEXT("/Script/ControlRig.FKControlRig");
		TryGetString(TrackJson, TEXT("control_rig_class"), ControlRigClassPath);
		if (ControlRigClassPath.IsEmpty())
		{
			ControlRigClassPath = TEXT("/Script/ControlRig.FKControlRig");
		}
		UClass* ControlRigClass = LoadControlRigClassFromPath(ControlRigClassPath);
		if (!ControlRigClass)
		{
			OutError = TEXT("control_rig_class_not_found");
			return false;
		}

		bool bIsAdditive = false;
		TryGetBool(TrackJson, TEXT("is_additive"), bIsAdditive);
		TryGetBool(TrackJson, TEXT("layered"), bIsAdditive);

		bool bTrackCreated = false;
		UMovieSceneControlRigParameterTrack* ControlRigTrack = FindOrCreateControlRigTrack(Sequence, MovieScene, BindingGuid, ControlRigClass, bIsAdditive, bTrackCreated, OutError);
		if (!ControlRigTrack)
		{
			return false;
		}
		ClearTrackSections(ControlRigTrack);

		UControlRig* ControlRig = ControlRigTrack->GetControlRig();
		if (!ControlRig)
		{
			OutError = TEXT("control_rig_instance_not_found");
			return false;
		}
		if (UFKControlRig* FKControlRig = Cast<UFKControlRig>(ControlRig))
		{
			FKControlRig->SetApplyMode(bIsAdditive ? EControlRigFKRigExecuteMode::Additive : EControlRigFKRigExecuteMode::Replace);
		}
		else
		{
			ControlRig->SetIsAdditive(bIsAdditive);
		}

		UMovieSceneControlRigParameterSection* ControlRigSection = Cast<UMovieSceneControlRigParameterSection>(ControlRigTrack->CreateControlRigSection(0, ControlRig, true));
		if (!ControlRigSection)
		{
			OutError = TEXT("create_control_rig_section_failed");
			return false;
		}
		ControlRigSection->Modify();
		ControlRigSection->SetRange(TRange<FFrameNumber>::All());
		++TracksApplied;
		if (!ApplyFolderOwnerDetailsIfPresent(
			ControlRigTrack,
			EntryDirectory,
			TrackJson,
			TEXT("sequence_track"),
			MakeObjectOwnerId(ControlRigTrack, ControlRigTrack->GetName()),
			TEXT("sequence_apply_folder"),
			OutError,
			GroupedDetailsApplied))
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
		if (!TrackJson->TryGetArrayField(TEXT("sections"), Sections) || !Sections)
		{
			return true;
		}
		if (Sections->Num() > 1)
		{
			AddWarning(Warnings, TEXT("control_rig_multi_section_flattened"), FString::Printf(TEXT("Flattened multi-section Control Rig track on binding '%s'."), *BindingGuidText));
		}

		bool bAppliedSectionMetadata = false;
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
				if (!bAppliedSectionMetadata)
				{
					ApplySectionMetadataJson(
						MovieScene,
						ControlRigSection,
						*SectionObj,
						Warnings,
						FString::Printf(TEXT("control rig track on binding '%s'"), *BindingGuidText),
						&RawPropertiesApplied,
						&ChannelsApplied,
						&EntryDirectory,
						&OutError,
						&GroupedDetailsApplied);
					bAppliedSectionMetadata = true;
				}
				continue;
			}
			for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
			{
				const TSharedPtr<FJsonObject>* KeyObj = nullptr;
				if (!KeyValue.IsValid() || !KeyValue->TryGetObject(KeyObj) || !KeyObj || !KeyObj->IsValid())
				{
					continue;
				}
				if (!ApplyControlRigKeyJson(MovieScene, ControlRig, ControlRigSection, *KeyObj, Warnings, KeysApplied, OutError))
				{
					return false;
				}
			}
			if (!bAppliedSectionMetadata)
			{
				ApplySectionMetadataJson(
					MovieScene,
					ControlRigSection,
					*SectionObj,
					Warnings,
					FString::Printf(TEXT("control rig track on binding '%s'"), *BindingGuidText),
					&RawPropertiesApplied,
					&ChannelsApplied,
					&EntryDirectory,
					&OutError,
					&GroupedDetailsApplied);
				bAppliedSectionMetadata = true;
			}
		}
		return true;
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
	bool bIncludeCompleteInventory = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_complete_inventory"), bIncludeCompleteInventory);
	bool bIncludeRawProperties = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_raw_properties"), bIncludeRawProperties);

	if (bCleanOutputDir && IFileManager::Get().DirectoryExists(*FolderPath))
	{
		IFileManager::Get().DeleteDirectory(*FolderPath, false, true);
	}

	const FString SettingsDir = FPaths::Combine(FolderPath, TEXT("settings"));
	const FString BindingsDir = FPaths::Combine(FolderPath, TEXT("bindings"));
	const FString OutlinerDir = FPaths::Combine(FolderPath, TEXT("outliner"));
	const FString MasterTracksDir = FPaths::Combine(FolderPath, TEXT("master_tracks"));
	const FString ValidationDir = FPaths::Combine(FolderPath, TEXT("validation"));
	const FString CompleteDir = FPaths::Combine(FolderPath, TEXT("complete"));
	IFileManager::Get().MakeDirectory(*SettingsDir, true);
	IFileManager::Get().MakeDirectory(*BindingsDir, true);
	IFileManager::Get().MakeDirectory(*OutlinerDir, true);
	IFileManager::Get().MakeDirectory(*MasterTracksDir, true);
	if (bIncludeValidation)
	{
		IFileManager::Get().MakeDirectory(*ValidationDir, true);
	}
	if (bIncludeCompleteInventory)
	{
		IFileManager::Get().MakeDirectory(*CompleteDir, true);
	}

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	int32 FileCount = 0;
	UeAgentSequenceFolderOps::FSequenceFolderCoverageStats CoverageStats;
	auto SaveRequiredJson = [&FileCount, &OutError, bIncludeRawProperties](const FString& FilePath, const TSharedPtr<FJsonObject>& JsonObj) -> bool
	{
		if (!bIncludeRawProperties)
		{
			UeAgentSequenceFolderOps::StripDefaultAuthoringRawFieldsFromObject(JsonObj);
		}
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
	AssetJson->SetStringField(TEXT("schema"), TEXT("uai.level_sequence.folder.v1"));
	AssetJson->SetStringField(TEXT("source_profile"), bIncludeCompleteInventory ? TEXT("authoring_plus_complete_inventory") : TEXT("authoring"));
	AssetJson->SetStringField(TEXT("asset_kind"), TEXT("level_sequence"));
	AssetJson->SetStringField(TEXT("asset_path"), AssetPath);
	AssetJson->SetStringField(TEXT("asset_name"), FPackageName::GetLongPackageAssetName(AssetPath));
	AssetJson->SetArrayField(TEXT("sequence_raw_properties"), UeAgentSequenceFolderOps::BuildSequenceRawPropertiesJson(Sequence));
	OutError.Reset();
	if (!UeAgentSequenceFolderOps::ExportFolderOwnerDetails(
		Sequence,
		FolderPath,
		TEXT("sequence_export_folder"),
		TEXT("sequence_asset"),
		UeAgentSequenceFolderOps::MakeObjectOwnerId(Sequence, AssetPath),
		TEXT("asset"),
		AssetJson,
		OutError))
	{
		return false;
	}
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
	SettingsJson->SetArrayField(TEXT("movie_scene_raw_properties"), UeAgentSequenceFolderOps::BuildSequenceRawPropertiesJson(MovieScene));
	OutError.Reset();
	if (!UeAgentSequenceFolderOps::ExportFolderOwnerDetails(
		MovieScene,
		SettingsDir,
		TEXT("sequence_export_folder"),
		TEXT("movie_scene_settings"),
		UeAgentSequenceFolderOps::MakeObjectOwnerId(MovieScene, TEXT("movie_scene")),
		TEXT("settings"),
		SettingsJson,
		OutError))
	{
		return false;
	}
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
			OutError.Reset();
			TSharedPtr<FJsonObject> TrackJson = UeAgentSequenceFolderOps::BuildTrackJson(MovieScene, Track, &TracksDir, &OutError);
			if (!OutError.IsEmpty())
			{
				return false;
			}
			if (!TrackJson.IsValid())
			{
				continue;
			}
			const FString TrackFileName = UeAgentSequenceFolderOps::MakeTrackFileName(Track, TrackIndex);
			if (TSharedPtr<FJsonObject> TrackRefObj = UeAgentSequenceFolderOps::BuildBindingTrackRefJson(Binding.GetObjectGuid(), TrackFileName, TrackJson))
			{
				BindingTrackRefsByTrack.Add(Track, TrackRefObj);
			}
			UeAgentSequenceFolderOps::RecordTrackCoverage(
				CoverageStats,
				MovieScene,
				Track,
				TEXT("binding"),
				Binding.GetObjectGuid().ToString(EGuidFormats::DigitsWithHyphensLower),
				Binding.GetName(),
				TrackFileName,
				TrackJson);
			if (!SaveRequiredJson(FPaths::Combine(TracksDir, TrackFileName), TrackJson)) return false;
		}
	}

	TSharedPtr<FJsonObject> BindingsIndexJson = MakeShared<FJsonObject>();
	BindingsIndexJson->SetArrayField(TEXT("bindings"), BindingIndexEntries);
	if (!SaveRequiredJson(FPaths::Combine(BindingsDir, TEXT("index.json")), BindingsIndexJson)) return false;

	if (!SaveRequiredJson(FPaths::Combine(OutlinerDir, TEXT("folders.json")), UeAgentSequenceFolderOps::BuildOutlinerFoldersJson(MovieScene, BindingTrackRefsByTrack))) return false;

	OutError.Reset();
	TSharedPtr<FJsonObject> MasterTracksJson = UeAgentSequenceFolderOps::BuildMasterTracksIndexJson(MovieScene, &CoverageStats, &MasterTracksDir, &OutError);
	if (!OutError.IsEmpty())
	{
		return false;
	}
	if (!SaveRequiredJson(FPaths::Combine(MasterTracksDir, TEXT("index.json")), MasterTracksJson)) return false;

	if (bIncludeValidation)
	{
		if (!SaveRequiredJson(FPaths::Combine(ValidationDir, TEXT("checks.json")), UeAgentSequenceFolderOps::ExportValidationJson())) return false;
		if (!SaveRequiredJson(FPaths::Combine(ValidationDir, TEXT("coverage_report.json")), UeAgentSequenceFolderOps::BuildCoverageReportJson(CoverageStats))) return false;
	}
	if (bIncludeCompleteInventory)
	{
		if (!SaveRequiredJson(FPaths::Combine(CompleteDir, TEXT("inventory.json")), UeAgentSequenceFolderOps::BuildCompleteInventoryJson(Sequence, MovieScene))) return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("root_path"), UeAgentSequenceFolderOps::GetFolderRootAbsolute());
	OutData->SetNumberField(TEXT("file_count"), FileCount);
	OutData->SetBoolField(TEXT("complete_inventory_exported"), bIncludeCompleteInventory);
	if (bIncludeCompleteInventory)
	{
		OutData->SetStringField(TEXT("complete_inventory_path"), FPaths::Combine(CompleteDir, TEXT("inventory.json")));
	}
	OutData->SetObjectField(TEXT("coverage_report"), UeAgentSequenceFolderOps::BuildCoverageReportJson(CoverageStats));
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
	TSharedPtr<FJsonObject> LegacyRejection;
	FString LegacyError;
	if (UeAgentDetailsAuthoring::BuildLegacyPropertiesFolderRejectionIfPresent(
		FolderPath,
		UeAgentSequenceFolderOps::MakeApplyOwnerInfo(TEXT("sequence_folder"), AssetPath, TEXT("sequence_apply_folder")),
		TEXT("details.index_file"),
		LegacyRejection,
		LegacyError))
	{
		OutData = LegacyRejection;
		OutError = LegacyError.IsEmpty() ? FString(TEXT("legacy_properties_not_supported")) : LegacyError;
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
		TSharedPtr<FJsonObject> CreateData = MakeShared<FJsonObject>();
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
	TArray<TSharedPtr<FJsonValue>> AutoFixes;
	int32 BindingsApplied = 0;
	int32 TracksApplied = 0;
	int32 KeysApplied = 0;
	int32 FoldersApplied = 0;
	int32 TracksRemoved = 0;
	int32 RawPropertiesApplied = 0;
	int32 GroupedDetailsApplied = 0;
	int32 ChannelsApplied = 0;
	int32 SkeletalAnimationSectionsForceCustomModeForControlRig = 0;
	UeAgentSequenceFolderOps::FSequenceFolderCoverageStats InputCoverageStats;
	auto AddAutoFix = [&AutoFixes](const FString& Code, const FString& Message, const FString& BindingGuidText, const FString& TrackFile, int32 SectionIndex)
	{
		TSharedPtr<FJsonObject> FixObj = MakeShared<FJsonObject>();
		FixObj->SetStringField(TEXT("code"), Code);
		FixObj->SetStringField(TEXT("message"), Message);
		FixObj->SetStringField(TEXT("binding_guid"), BindingGuidText);
		FixObj->SetStringField(TEXT("track_file"), TrackFile);
		FixObj->SetNumberField(TEXT("section_index"), SectionIndex);
		AutoFixes.Add(MakeShared<FJsonValueObject>(FixObj));
	};
	auto ApplySequenceGroupedDetails = [&](UObject* Object, const FString& EntryDirectory, const TSharedPtr<FJsonObject>& EntryObject, const FString& OwnerKind, const FString& FallbackOwnerId) -> bool
	{
		if (!EntryObject.IsValid())
		{
			return true;
		}
		return UeAgentSequenceFolderOps::ApplyFolderOwnerDetailsIfPresent(
			Object,
			EntryDirectory,
			EntryObject,
			OwnerKind,
			UeAgentSequenceFolderOps::MakeObjectOwnerId(Object, FallbackOwnerId),
			TEXT("sequence_apply_folder"),
			OutError,
			GroupedDetailsApplied);
	};

	bool bStrict = false;
	JsonTryGetBool(Ctx.Params, TEXT("strict"), bStrict);
	bool bFailOnWarnings = bStrict;
	JsonTryGetBool(Ctx.Params, TEXT("fail_on_warnings"), bFailOnWarnings);
	bool bFailOnUnsupportedTracks = bStrict;
	JsonTryGetBool(Ctx.Params, TEXT("fail_on_unsupported_tracks"), bFailOnUnsupportedTracks);
	bool bFailOnPartialRoundtrip = false;
	JsonTryGetBool(Ctx.Params, TEXT("fail_on_partial_roundtrip"), bFailOnPartialRoundtrip);
	bool bFailOnStructuralDiff = false;
	JsonTryGetBool(Ctx.Params, TEXT("fail_on_structural_diff"), bFailOnStructuralDiff);
	bool bVerifyAfterApply = true;
	JsonTryGetBool(Ctx.Params, TEXT("verify_after_apply"), bVerifyAfterApply);
	bool bAutoForceSkeletalCustomModeForControlRig = true;
	JsonTryGetBool(Ctx.Params, TEXT("auto_force_skeletal_custom_mode_for_control_rig"), bAutoForceSkeletalCustomModeForControlRig);
	JsonTryGetBool(Ctx.Params, TEXT("force_skeletal_custom_mode_for_control_rig"), bAutoForceSkeletalCustomModeForControlRig);

	if (!UeAgentSequenceFolderOps::ApplyFolderOwnerDetailsIfPresent(
		Sequence,
		FolderPath,
		AssetJson,
		TEXT("sequence_asset"),
		UeAgentSequenceFolderOps::MakeObjectOwnerId(Sequence, AssetPath),
		TEXT("sequence_apply_folder"),
		OutError,
		GroupedDetailsApplied))
	{
		return false;
	}

	FString ReplaceMode = TEXT("patch");
	JsonTryGetString(Ctx.Params, TEXT("replace_mode"), ReplaceMode);
	ReplaceMode = ReplaceMode.TrimStartAndEnd().ToLower();
	if (ReplaceMode == TEXT("replace_binding") || ReplaceMode == TEXT("replace_binding_track") || ReplaceMode == TEXT("binding_tracks"))
	{
		ReplaceMode = TEXT("replace_binding_tracks");
	}
	else if (ReplaceMode == TEXT("replace_all") || ReplaceMode == TEXT("replace_tracks") || ReplaceMode == TEXT("all_tracks"))
	{
		ReplaceMode = TEXT("replace_all_tracks");
	}
	if (ReplaceMode != TEXT("patch") && ReplaceMode != TEXT("replace_binding_tracks") && ReplaceMode != TEXT("replace_all_tracks"))
	{
		OutError = TEXT("invalid_replace_mode");
		return false;
	}
	const bool bReplaceBindingTracks = ReplaceMode == TEXT("replace_binding_tracks") || ReplaceMode == TEXT("replace_all_tracks");
	const bool bReplaceMasterTracks = ReplaceMode == TEXT("replace_all_tracks");

	if (bFailOnUnsupportedTracks || bFailOnPartialRoundtrip)
	{
		UeAgentSequenceFolderOps::FSequenceFolderCoverageStats PreflightCoverageStats;
		if (!UeAgentSequenceFolderOps::ScanFolderInputCoverage(FolderPath, PreflightCoverageStats, OutError))
		{
			return false;
		}

		if ((bFailOnUnsupportedTracks && (PreflightCoverageStats.ReadonlySummaryTrackCount > 0 || PreflightCoverageStats.UnsupportedTrackCount > 0))
			|| (bFailOnPartialRoundtrip && PreflightCoverageStats.PartialRoundtripTrackCount > 0))
		{
			if (!OutData.IsValid())
			{
				OutData = MakeShared<FJsonObject>();
			}
			OutData->SetStringField(TEXT("asset_path"), AssetPath);
			OutData->SetStringField(TEXT("folder_path"), FolderPath);
			OutData->SetStringField(TEXT("replace_mode"), ReplaceMode);
			OutData->SetBoolField(TEXT("strict"), bStrict);
			OutData->SetBoolField(TEXT("fail_on_unsupported_tracks"), bFailOnUnsupportedTracks);
			OutData->SetBoolField(TEXT("fail_on_partial_roundtrip"), bFailOnPartialRoundtrip);
			OutData->SetObjectField(TEXT("input_coverage_report"), UeAgentSequenceFolderOps::BuildCoverageReportJson(PreflightCoverageStats));
			OutData->SetBoolField(TEXT("preflight_failed_before_writes"), true);
			OutError = (bFailOnUnsupportedTracks && (PreflightCoverageStats.ReadonlySummaryTrackCount > 0 || PreflightCoverageStats.UnsupportedTrackCount > 0))
				? TEXT("strict_apply_has_unsupported_tracks")
				: TEXT("strict_apply_has_partial_roundtrip_tracks");
			return false;
		}
	}

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
		if (!UeAgentSequenceFolderOps::ApplyFolderOwnerDetailsIfPresent(
			Sequence->GetMovieScene(),
			FPaths::GetPath(SettingsPath),
			SettingsJson,
			TEXT("movie_scene_settings"),
			UeAgentSequenceFolderOps::MakeObjectOwnerId(Sequence->GetMovieScene(), TEXT("movie_scene")),
			TEXT("sequence_apply_folder"),
			OutError,
			GroupedDetailsApplied))
		{
			return false;
		}

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
		bool bBindingHasControlRigTrackSpec = false;
		for (const FString& TrackFileForScan : TrackFiles)
		{
			TSharedPtr<FJsonObject> ScanTrackJson;
			FString ScanResolvedPath;
			FString ScanError;
			if (!UeAgentSequenceOps::LoadJsonFile(FPaths::Combine(TracksDir, TrackFileForScan), ScanTrackJson, ScanResolvedPath, ScanError))
			{
				OutError = ScanError;
				return false;
			}
			if (!ScanTrackJson.IsValid())
			{
				continue;
			}
			FString ScanTrackType;
			UeAgentSequenceFolderOps::TryGetString(ScanTrackJson, TEXT("track_type"), ScanTrackType);
			if (ScanTrackType.Equals(TEXT("control_rig"), ESearchCase::IgnoreCase))
			{
				bBindingHasControlRigTrackSpec = true;
				break;
			}
		}

		if (bReplaceBindingTracks)
		{
			Sequence = UeAgentSequenceOps::LoadLevelSequence(AssetPath);
			MovieScene = Sequence ? Sequence->GetMovieScene() : nullptr;
			if (!Sequence || !MovieScene)
			{
				OutError = TEXT("movie_scene_not_found");
				return false;
			}
			if (FMovieSceneBinding* Binding = MovieScene->FindBinding(BindingGuid))
			{
				TArray<UMovieSceneTrack*> ExistingBindingTracks = Binding->GetTracks();
				for (UMovieSceneTrack* ExistingBindingTrack : ExistingBindingTracks)
				{
					if (ExistingBindingTrack && MovieScene->RemoveTrack(*ExistingBindingTrack))
					{
						++TracksRemoved;
					}
				}
			}
		}

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

			UeAgentSequenceFolderOps::RecordTrackCoverage(
				InputCoverageStats,
				nullptr,
				nullptr,
				TEXT("binding"),
				BindingGuidText,
				BindingFolderName,
				TrackFile,
				TrackJson);

			Sequence = UeAgentSequenceOps::LoadLevelSequence(AssetPath);
			MovieScene = Sequence ? Sequence->GetMovieScene() : nullptr;
			UMovieSceneTrack* ExistingTrack = UeAgentSequenceFolderOps::FindTrackForSpec(MovieScene, BindingGuid, TrackJson);
			if (!bReplaceBindingTracks && ExistingTrack)
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
				if (UMovieSceneTrack* AppliedTrack = UeAgentSequenceFolderOps::FindTrackForSpec(MovieScene, BindingGuid, TrackJson))
				{
					if (!ApplySequenceGroupedDetails(
						AppliedTrack,
						TracksDir,
						TrackJson,
						TEXT("sequence_track"),
						AppliedTrack->GetName()))
					{
						return false;
					}
				}

				const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
				if (!TrackJson->TryGetArrayField(TEXT("sections"), Sections) || !Sections)
				{
					continue;
				}
				for (int32 SectionIndex = 0; SectionIndex < Sections->Num(); ++SectionIndex)
				{
					const TSharedPtr<FJsonValue>& SectionValue = (*Sections)[SectionIndex];
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
						const TSharedPtr<FJsonValue>* LocationValue = UeAgentJsonDiagnostics::FindFieldValue(*KeyObj, TEXT("location"));
						if (LocationValue && (*LocationValue).IsValid())
						{
							KeyParams->SetField(TEXT("location"), *LocationValue);
						}
						const TSharedPtr<FJsonValue>* RotationValue = UeAgentJsonDiagnostics::FindFieldValue(*KeyObj, TEXT("rotation"));
						if (RotationValue && (*RotationValue).IsValid())
						{
							KeyParams->SetField(TEXT("rotation"), *RotationValue);
						}
						const TSharedPtr<FJsonValue>* ScaleValue = UeAgentJsonDiagnostics::FindFieldValue(*KeyObj, TEXT("scale"));
						if (ScaleValue && (*ScaleValue).IsValid())
						{
							KeyParams->SetField(TEXT("scale"), *ScaleValue);
						}
						KeyParams->SetBoolField(TEXT("save_after_set"), false);
						TSharedPtr<FJsonObject> KeyData;
						if (!InvokeSequenceSubCommand(TEXT("sequence_add_transform_key"), KeyParams, KeyData, OutError))
						{
							return false;
						}
						++KeysApplied;
					}
					UMovieSceneTrack* AppliedTrack = UeAgentSequenceFolderOps::FindTrackForSpec(MovieScene, BindingGuid, TrackJson);
					UeAgentSequenceFolderOps::ApplyFirstSectionMetadataJson(
						MovieScene,
						AppliedTrack,
						*SectionObj,
						Warnings,
						FString::Printf(TEXT("transform track '%s' on binding '%s'"), *TrackFile, *BindingGuidText),
						&RawPropertiesApplied,
						&ChannelsApplied,
						&TracksDir,
						&OutError,
						&GroupedDetailsApplied);
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
				if (!ApplySequenceGroupedDetails(
					SpawnTrack,
					TracksDir,
					TrackJson,
					TEXT("sequence_track"),
					SpawnTrack->GetName()))
				{
					return false;
				}

				const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
				if (!TrackJson->TryGetArrayField(TEXT("sections"), Sections) || !Sections)
				{
					continue;
				}
				for (int32 SectionIndex = 0; SectionIndex < Sections->Num(); ++SectionIndex)
				{
					const TSharedPtr<FJsonValue>& SectionValue = (*Sections)[SectionIndex];
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
					UeAgentSequenceFolderOps::ApplySectionMetadataJson(
						MovieScene,
						SpawnSection,
						*SectionObj,
						Warnings,
						FString::Printf(TEXT("spawn track '%s' on binding '%s'"), *TrackFile, *BindingGuidText),
						&RawPropertiesApplied,
						&ChannelsApplied,
						&TracksDir,
						&OutError,
						&GroupedDetailsApplied);
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
				if (UMovieSceneSkeletalAnimationTrack* SkeletalTrack = MovieScene ? MovieScene->FindTrack<UMovieSceneSkeletalAnimationTrack>(BindingGuid) : nullptr)
				{
					if (!ApplySequenceGroupedDetails(
						SkeletalTrack,
						TracksDir,
						TrackJson,
						TEXT("sequence_track"),
						SkeletalTrack->GetName()))
					{
						return false;
					}
				}

				const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
				if (!TrackJson->TryGetArrayField(TEXT("sections"), Sections) || !Sections)
				{
					continue;
				}
				for (int32 SectionIndex = 0; SectionIndex < Sections->Num(); ++SectionIndex)
				{
					const TSharedPtr<FJsonValue>& SectionValue = (*Sections)[SectionIndex];
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
					bool bSectionForceCustomMode = false;
					const bool bHasSectionForceCustomMode = (*SectionObj)->TryGetBoolField(TEXT("force_custom_mode"), bSectionForceCustomMode);
					if (bHasSectionForceCustomMode) AddSectionParams->SetBoolField(TEXT("force_custom_mode"), bSectionForceCustomMode);
					if (bAutoForceSkeletalCustomModeForControlRig && bBindingHasControlRigTrackSpec)
					{
						AddSectionParams->SetBoolField(TEXT("force_custom_mode"), true);
						if (!bHasSectionForceCustomMode || !bSectionForceCustomMode)
						{
							++SkeletalAnimationSectionsForceCustomModeForControlRig;
							AddAutoFix(
								TEXT("skeletal_animation_force_custom_mode_for_control_rig"),
								TEXT("Forced skeletal animation section force_custom_mode=true because this binding also has a control_rig track."),
								BindingGuidText,
								TrackFile,
								SectionIndex);
						}
					}
					AddSectionParams->SetBoolField(TEXT("save_after_set"), false);
					TSharedPtr<FJsonObject> AddSectionData;
					if (!InvokeSequenceSubCommand(TEXT("sequence_add_skeletal_animation_section"), AddSectionParams, AddSectionData, OutError))
					{
						return false;
					}
					if (UMovieSceneSkeletalAnimationTrack* SkeletalTrack = MovieScene ? MovieScene->FindTrack<UMovieSceneSkeletalAnimationTrack>(BindingGuid) : nullptr)
					{
						const TArray<UMovieSceneSection*>& AppliedSections = SkeletalTrack->GetAllSections();
						if (AppliedSections.Num() > 0)
						{
							UeAgentSequenceFolderOps::ApplySectionMetadataJson(
								MovieScene,
								AppliedSections.Last(),
								*SectionObj,
								Warnings,
								FString::Printf(TEXT("skeletal animation track '%s' on binding '%s'"), *TrackFile, *BindingGuidText),
								&RawPropertiesApplied,
								&ChannelsApplied,
								&TracksDir,
								&OutError,
								&GroupedDetailsApplied);
						}
					}
					++KeysApplied;
				}
				++BindingsApplied;
				continue;
			}

			if (TrackType.Equals(TEXT("control_rig"), ESearchCase::IgnoreCase))
			{
				if (!UeAgentSequenceFolderOps::ApplyControlRigTrackJson(
					Sequence,
					MovieScene,
					BindingGuid,
					BindingGuidText,
					TrackJson,
					Warnings,
					TracksApplied,
					KeysApplied,
					RawPropertiesApplied,
					ChannelsApplied,
					GroupedDetailsApplied,
					TracksDir,
					OutError))
				{
					return false;
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
					if (UMovieSceneTrack* AppliedTrack = UeAgentSequenceFolderOps::FindTrackForSpec(MovieScene, BindingGuid, TrackJson))
					{
						if (!ApplySequenceGroupedDetails(
							AppliedTrack,
							TracksDir,
							TrackJson,
							TEXT("sequence_track"),
							AppliedTrack->GetName()))
						{
							return false;
						}
					}

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
						UMovieSceneTrack* AppliedTrack = UeAgentSequenceFolderOps::FindTrackForSpec(MovieScene, BindingGuid, TrackJson);
						UeAgentSequenceFolderOps::ApplyFirstSectionMetadataJson(
							MovieScene,
							AppliedTrack,
							*SectionObj,
							Warnings,
							FString::Printf(TEXT("visibility track '%s' on binding '%s'"), *TrackFile, *BindingGuidText),
							&RawPropertiesApplied,
							&ChannelsApplied,
							&TracksDir,
							&OutError,
							&GroupedDetailsApplied);
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
				if (UMovieSceneTrack* AppliedTrack = UeAgentSequenceFolderOps::FindTrackForSpec(MovieScene, BindingGuid, TrackJson))
				{
					if (!ApplySequenceGroupedDetails(
						AppliedTrack,
						TracksDir,
						TrackJson,
						TEXT("sequence_track"),
						AppliedTrack->GetName()))
					{
						return false;
					}
				}

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
					UMovieSceneTrack* AppliedTrack = UeAgentSequenceFolderOps::FindTrackForSpec(MovieScene, BindingGuid, TrackJson);
					UeAgentSequenceFolderOps::ApplyFirstSectionMetadataJson(
						MovieScene,
						AppliedTrack,
						*SectionObj,
						Warnings,
						FString::Printf(TEXT("property track '%s' on binding '%s'"), *TrackFile, *BindingGuidText),
						&RawPropertiesApplied,
						&ChannelsApplied,
						&TracksDir,
						&OutError,
						&GroupedDetailsApplied);
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
		const FString MasterTracksDir = FPaths::GetPath(MasterTracksIndexPath);
		if (bReplaceMasterTracks)
		{
			Sequence = UeAgentSequenceOps::LoadLevelSequence(AssetPath);
			UMovieScene* MovieScene = Sequence ? Sequence->GetMovieScene() : nullptr;
			if (!Sequence || !MovieScene)
			{
				OutError = TEXT("movie_scene_not_found");
				return false;
			}
			if (MovieScene->GetCameraCutTrack())
			{
				MovieScene->RemoveCameraCutTrack();
				++TracksRemoved;
			}
			TArray<UMovieSceneTrack*> ExistingMasterTracks = MovieScene->GetTracks();
			for (UMovieSceneTrack* ExistingMasterTrack : ExistingMasterTracks)
			{
				if (ExistingMasterTrack && MovieScene->RemoveTrack(*ExistingMasterTrack))
				{
					++TracksRemoved;
				}
			}
		}

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
				UeAgentSequenceFolderOps::RecordTrackCoverage(
					InputCoverageStats,
					nullptr,
					nullptr,
					TEXT("master"),
					TEXT(""),
					TEXT(""),
					TrackType,
					*MasterTrackObj);

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
					if (!ApplySequenceGroupedDetails(
						CameraCutTrack,
						MasterTracksDir,
						*MasterTrackObj,
						TEXT("sequence_track"),
						CameraCutTrack->GetName()))
					{
						return false;
					}

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
						UeAgentSequenceFolderOps::ApplySectionMetadataJson(
							MovieScene,
							NewSection,
							*SectionObj,
							Warnings,
							TEXT("camera cut master track"),
							&RawPropertiesApplied,
							&ChannelsApplied,
							&MasterTracksDir,
							&OutError,
							&GroupedDetailsApplied);
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
					if (!ApplySequenceGroupedDetails(
						SubTrack,
						MasterTracksDir,
						*MasterTrackObj,
						TEXT("sequence_track"),
						SubTrack->GetName()))
					{
						return false;
					}

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
						UeAgentSequenceFolderOps::ApplySectionMetadataJson(
							MovieScene,
							NewSection,
							*SectionObj,
							Warnings,
							bCinematicShot ? TEXT("cinematic shot master track") : TEXT("sub sequence master track"),
							&RawPropertiesApplied,
							&ChannelsApplied,
							&MasterTracksDir,
							&OutError,
							&GroupedDetailsApplied);

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
	bool bSaved = false;
	auto PopulateApplyResult = [&]()
	{
		if (!OutData.IsValid())
		{
			OutData = MakeShared<FJsonObject>();
		}
		OutData->SetStringField(TEXT("asset_path"), AssetPath);
		OutData->SetStringField(TEXT("folder_path"), FolderPath);
		OutData->SetStringField(TEXT("replace_mode"), ReplaceMode);
		OutData->SetBoolField(TEXT("strict"), bStrict);
		OutData->SetBoolField(TEXT("fail_on_warnings"), bFailOnWarnings);
		OutData->SetBoolField(TEXT("fail_on_unsupported_tracks"), bFailOnUnsupportedTracks);
		OutData->SetBoolField(TEXT("fail_on_partial_roundtrip"), bFailOnPartialRoundtrip);
		OutData->SetBoolField(TEXT("fail_on_structural_diff"), bFailOnStructuralDiff);
		OutData->SetBoolField(TEXT("verify_after_apply"), bVerifyAfterApply);
		OutData->SetBoolField(TEXT("auto_force_skeletal_custom_mode_for_control_rig"), bAutoForceSkeletalCustomModeForControlRig);
		OutData->SetNumberField(TEXT("bindings_applied"), BindingsApplied);
		OutData->SetNumberField(TEXT("tracks_applied"), TracksApplied);
		OutData->SetNumberField(TEXT("keys_applied"), KeysApplied);
		OutData->SetNumberField(TEXT("folders_applied"), FoldersApplied);
		OutData->SetNumberField(TEXT("tracks_removed"), TracksRemoved);
		OutData->SetNumberField(TEXT("raw_properties_applied"), RawPropertiesApplied);
		OutData->SetNumberField(TEXT("grouped_details_applied"), GroupedDetailsApplied);
		OutData->SetNumberField(TEXT("channels_applied"), ChannelsApplied);
		OutData->SetNumberField(TEXT("skeletal_animation_sections_force_custom_mode_for_control_rig"), SkeletalAnimationSectionsForceCustomModeForControlRig);
		OutData->SetBoolField(TEXT("save_after_apply"), bSaveAfterApply);
		OutData->SetBoolField(TEXT("saved"), bSaved);
		OutData->SetNumberField(TEXT("auto_fix_count"), AutoFixes.Num());
		OutData->SetArrayField(TEXT("auto_fixes"), AutoFixes);
		OutData->SetNumberField(TEXT("warning_count"), Warnings.Num());
		OutData->SetArrayField(TEXT("warnings"), Warnings);
		TSharedPtr<FJsonObject> InputCoverageReport = UeAgentSequenceFolderOps::BuildCoverageReportJson(InputCoverageStats);
		OutData->SetObjectField(TEXT("input_coverage_report"), InputCoverageReport);
		if (bVerifyAfterApply)
		{
			ULevelSequence* VerifySequence = UeAgentSequenceOps::LoadLevelSequence(AssetPath);
			UMovieScene* VerifyMovieScene = VerifySequence ? VerifySequence->GetMovieScene() : nullptr;
			if (VerifyMovieScene)
			{
				TSharedPtr<FJsonObject> PostApplyCoverageReport = UeAgentSequenceFolderOps::BuildCoverageReportJson(VerifyMovieScene);
				OutData->SetObjectField(TEXT("post_apply_coverage_report"), PostApplyCoverageReport);
				OutData->SetObjectField(TEXT("coverage_diff"), UeAgentSequenceFolderOps::BuildCoverageDiffJson(InputCoverageReport, PostApplyCoverageReport));
				double MaxStructuralDiffEntriesNumber = 50.0;
				if (Ctx.Params.IsValid())
				{
					Ctx.Params->TryGetNumberField(TEXT("structural_diff_max_entries"), MaxStructuralDiffEntriesNumber);
				}
				FString StructuralDiffError;
				TSharedPtr<FJsonObject> StructuralDiff = UeAgentSequenceFolderOps::BuildStructuralSnapshotDiffJson(
					FolderPath,
					AssetPath,
					VerifySequence,
					VerifyMovieScene,
					FMath::Max(0, FMath::RoundToInt(MaxStructuralDiffEntriesNumber)),
					StructuralDiffError);
				OutData->SetObjectField(TEXT("structural_diff"), StructuralDiff);
			}
		}
	};

	if (bFailOnUnsupportedTracks && (InputCoverageStats.ReadonlySummaryTrackCount > 0 || InputCoverageStats.UnsupportedTrackCount > 0))
	{
		PopulateApplyResult();
		OutError = TEXT("strict_apply_has_unsupported_tracks");
		return false;
	}
	if (bFailOnPartialRoundtrip && InputCoverageStats.PartialRoundtripTrackCount > 0)
	{
		PopulateApplyResult();
		OutError = TEXT("strict_apply_has_partial_roundtrip_tracks");
		return false;
	}
	if (bFailOnWarnings && Warnings.Num() > 0)
	{
		PopulateApplyResult();
		OutError = TEXT("strict_apply_has_warnings");
		return false;
	}
	if (bFailOnStructuralDiff)
	{
		PopulateApplyResult();
		const TSharedPtr<FJsonObject>* StructuralDiff = nullptr;
		const bool bHasStructuralDiff = OutData.IsValid()
			&& OutData->TryGetObjectField(TEXT("structural_diff"), StructuralDiff)
			&& StructuralDiff
			&& StructuralDiff->IsValid();
		if (!bHasStructuralDiff || !(*StructuralDiff)->GetBoolField(TEXT("success")))
		{
			OutError = TEXT("strict_apply_structural_diff_unavailable");
			return false;
		}
		if (!(*StructuralDiff)->GetBoolField(TEXT("is_structurally_equal")))
		{
			OutError = TEXT("strict_apply_has_structural_diff");
			return false;
		}
	}

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
		bSaved = true;
	}

	PopulateApplyResult();
	return true;
}
