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

	static TSharedPtr<FJsonObject> BuildMasterTracksIndexJson(UMovieScene* MovieScene, FSequenceFolderCoverageStats* CoverageStats = nullptr)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Tracks;
		if (MovieScene)
		{
			if (UMovieSceneTrack* CameraCutTrack = MovieScene->GetCameraCutTrack())
			{
				if (TSharedPtr<FJsonObject> TrackObj = BuildTrackJson(MovieScene, CameraCutTrack))
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
				if (TSharedPtr<FJsonObject> TrackObj = BuildTrackJson(MovieScene, Track))
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

		const TSharedPtr<FJsonObject>* ValueObj = nullptr;
		const bool bHasValueObject = KeyObj->TryGetObjectField(TEXT("value"), ValueObj) && ValueObj && ValueObj->IsValid();
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
			if (!bHasValueObject || !TryReadControlRigVector2D(*ValueObj, Value))
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
			if (!bHasValueObject || !TryReadControlRigVector(*ValueObj, Value))
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
			if (!bHasValueObject || !TryReadControlRigRotator(*ValueObj, Value))
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
		RawPropertiesApplied += ApplySequenceRawPropertiesJson(
			ControlRigTrack,
			TrackJson,
			TEXT("raw_properties"),
			Warnings,
			FString::Printf(TEXT("control rig track on binding '%s'"), *BindingGuidText));

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
						&ChannelsApplied);
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
					&ChannelsApplied);
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
	AssetJson->SetStringField(TEXT("schema"), TEXT("uai.level_sequence.folder.v1"));
	AssetJson->SetStringField(TEXT("source_profile"), bIncludeCompleteInventory ? TEXT("authoring_plus_complete_inventory") : TEXT("authoring"));
	AssetJson->SetStringField(TEXT("asset_kind"), TEXT("level_sequence"));
	AssetJson->SetStringField(TEXT("asset_path"), AssetPath);
	AssetJson->SetArrayField(TEXT("sequence_raw_properties"), UeAgentSequenceFolderOps::BuildSequenceRawPropertiesJson(Sequence));
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

	if (!SaveRequiredJson(FPaths::Combine(MasterTracksDir, TEXT("index.json")), UeAgentSequenceFolderOps::BuildMasterTracksIndexJson(MovieScene, &CoverageStats))) return false;

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

	RawPropertiesApplied += UeAgentSequenceFolderOps::ApplySequenceRawPropertiesJson(
		Sequence,
		AssetJson,
		TEXT("sequence_raw_properties"),
		Warnings,
		FString::Printf(TEXT("level sequence asset '%s'"), *AssetPath));

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
		RawPropertiesApplied += UeAgentSequenceFolderOps::ApplySequenceRawPropertiesJson(
			Sequence->GetMovieScene(),
			SettingsJson,
			TEXT("movie_scene_raw_properties"),
			Warnings,
			FString::Printf(TEXT("movie scene for '%s'"), *AssetPath));

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
					RawPropertiesApplied += UeAgentSequenceFolderOps::ApplySequenceRawPropertiesJson(
						AppliedTrack,
						TrackJson,
						TEXT("raw_properties"),
						Warnings,
						FString::Printf(TEXT("transform track '%s' on binding '%s'"), *TrackFile, *BindingGuidText));
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
					UMovieSceneTrack* AppliedTrack = UeAgentSequenceFolderOps::FindTrackForSpec(MovieScene, BindingGuid, TrackJson);
					UeAgentSequenceFolderOps::ApplyFirstSectionMetadataJson(
						MovieScene,
						AppliedTrack,
						*SectionObj,
						Warnings,
						FString::Printf(TEXT("transform track '%s' on binding '%s'"), *TrackFile, *BindingGuidText),
						&RawPropertiesApplied,
						&ChannelsApplied);
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
				RawPropertiesApplied += UeAgentSequenceFolderOps::ApplySequenceRawPropertiesJson(
					SpawnTrack,
					TrackJson,
					TEXT("raw_properties"),
					Warnings,
					FString::Printf(TEXT("spawn track '%s' on binding '%s'"), *TrackFile, *BindingGuidText));

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
						&ChannelsApplied);
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
					RawPropertiesApplied += UeAgentSequenceFolderOps::ApplySequenceRawPropertiesJson(
						SkeletalTrack,
						TrackJson,
						TEXT("raw_properties"),
						Warnings,
						FString::Printf(TEXT("skeletal animation track '%s' on binding '%s'"), *TrackFile, *BindingGuidText));
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
								&ChannelsApplied);
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
						RawPropertiesApplied += UeAgentSequenceFolderOps::ApplySequenceRawPropertiesJson(
							AppliedTrack,
							TrackJson,
							TEXT("raw_properties"),
							Warnings,
							FString::Printf(TEXT("visibility track '%s' on binding '%s'"), *TrackFile, *BindingGuidText));
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
							&ChannelsApplied);
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
					RawPropertiesApplied += UeAgentSequenceFolderOps::ApplySequenceRawPropertiesJson(
						AppliedTrack,
						TrackJson,
						TEXT("raw_properties"),
						Warnings,
						FString::Printf(TEXT("property track '%s' on binding '%s'"), *TrackFile, *BindingGuidText));
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
						&ChannelsApplied);
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
					RawPropertiesApplied += UeAgentSequenceFolderOps::ApplySequenceRawPropertiesJson(
						CameraCutTrack,
						*MasterTrackObj,
						TEXT("raw_properties"),
						Warnings,
						TEXT("camera cut master track"));

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
							&ChannelsApplied);
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
					RawPropertiesApplied += UeAgentSequenceFolderOps::ApplySequenceRawPropertiesJson(
						SubTrack,
						*MasterTrackObj,
						TEXT("raw_properties"),
						Warnings,
						bCinematicShot ? TEXT("cinematic shot master track") : TEXT("sub sequence master track"));

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
							&ChannelsApplied);

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
