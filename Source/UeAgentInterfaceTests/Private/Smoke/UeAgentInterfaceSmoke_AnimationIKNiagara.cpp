#if WITH_DEV_AUTOMATION_TESTS

#include "UeAgentSmokeTestSharedIncludes.inl"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceMontageSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.MontageWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceMontageSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString MontageAssetPath = MakeAutomationAssetPath(TEXT("MNT"));
	const FString SkeletonAssetPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton.TutorialTPP_Skeleton");
	const FString PreviewMeshAssetPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP.TutorialTPP");
	const FString IdleAnimationAssetPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Idle.Tutorial_Idle");
	const FString WalkAnimationAssetPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Walk_Fwd.Tutorial_Walk_Fwd");
	const FString AddedSlotName = TEXT("UpperBody");
	const FString RenamedSlotName = TEXT("UpperBodyAim");
	const FString AddedSectionName = TEXT("Loop");
	const FString RenamedSectionName = TEXT("LoopRenamed");
	const FString NotifyTrackName = TEXT("MontageNotifyTrack");
	const FString NotifyName = TEXT("SmokeNotify");
	const FString SkeletonSlotName = FString::Printf(TEXT("SmokeSlot_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(6));
	const FString RenamedSkeletonSlotName = SkeletonSlotName + TEXT("_Renamed");
	const FString SkeletonSlotGroupName = TEXT("SmokeMontageGroup");
	const FString SyncGroupName = TEXT("SmokeSyncGroup");

	auto ListContainsMontage = [](const TSharedPtr<FJsonObject>* ListData, const FString& AssetPath, bool& bFoundOut) -> bool
	{
		bFoundOut = false;
		if (!ListData || !ListData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (!(*ListData)->TryGetArrayField(TEXT("items"), Items) || !Items)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& ItemValue : *Items)
		{
			const TSharedPtr<FJsonObject>* ItemObject = nullptr;
			if (!ItemValue.IsValid() || !ItemValue->TryGetObject(ItemObject) || !ItemObject || !ItemObject->IsValid())
			{
				continue;
			}
			if ((*ItemObject)->GetStringField(TEXT("package_name")) == AssetPath)
			{
				bFoundOut = true;
				break;
			}
		}

		return true;
	};

	auto FindSlotTrackEntry = [](const TSharedPtr<FJsonObject>* InfoData, const FString& SlotName, TSharedPtr<FJsonObject>& OutEntryObject) -> bool
	{
		if (!InfoData || !InfoData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* SlotTracks = nullptr;
		if (!(*InfoData)->TryGetArrayField(TEXT("slot_tracks"), SlotTracks) || !SlotTracks)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& SlotTrackValue : *SlotTracks)
		{
			const TSharedPtr<FJsonObject>* SlotTrackObject = nullptr;
			if (!SlotTrackValue.IsValid() || !SlotTrackValue->TryGetObject(SlotTrackObject) || !SlotTrackObject || !SlotTrackObject->IsValid())
			{
				continue;
			}
			if ((*SlotTrackObject)->GetStringField(TEXT("slot_name")) == SlotName)
			{
				OutEntryObject = *SlotTrackObject;
				return true;
			}
		}

		return false;
	};

	auto FindSectionEntry = [](const TSharedPtr<FJsonObject>* InfoData, const FString& SectionName, TSharedPtr<FJsonObject>& OutEntryObject) -> bool
	{
		if (!InfoData || !InfoData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
		if (!(*InfoData)->TryGetArrayField(TEXT("sections"), Sections) || !Sections)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
		{
			const TSharedPtr<FJsonObject>* SectionObject = nullptr;
			if (!SectionValue.IsValid() || !SectionValue->TryGetObject(SectionObject) || !SectionObject || !SectionObject->IsValid())
			{
				continue;
			}
			if ((*SectionObject)->GetStringField(TEXT("section_name")) == SectionName)
			{
				OutEntryObject = *SectionObject;
				return true;
			}
		}

		return false;
	};

	auto FindNotifyTrackEntry = [](const TSharedPtr<FJsonObject>* InfoData, const FString& TrackName, TSharedPtr<FJsonObject>& OutEntryObject) -> bool
	{
		if (!InfoData || !InfoData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Tracks = nullptr;
		if (!(*InfoData)->TryGetArrayField(TEXT("notify_tracks"), Tracks) || !Tracks)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& TrackValue : *Tracks)
		{
			const TSharedPtr<FJsonObject>* TrackObject = nullptr;
			if (!TrackValue.IsValid() || !TrackValue->TryGetObject(TrackObject) || !TrackObject || !TrackObject->IsValid())
			{
				continue;
			}
			if ((*TrackObject)->GetStringField(TEXT("track_name")) == TrackName)
			{
				OutEntryObject = *TrackObject;
				return true;
			}
		}

		return false;
	};

	auto FindNotifyEntry = [](const TSharedPtr<FJsonObject>* InfoData, const FString& NotifyNameNeedle, const bool bRequireState, TSharedPtr<FJsonObject>& OutEntryObject) -> bool
	{
		if (!InfoData || !InfoData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Notifies = nullptr;
		if (!(*InfoData)->TryGetArrayField(TEXT("notifies"), Notifies) || !Notifies)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& NotifyValue : *Notifies)
		{
			const TSharedPtr<FJsonObject>* NotifyObject = nullptr;
			if (!NotifyValue.IsValid() || !NotifyValue->TryGetObject(NotifyObject) || !NotifyObject || !NotifyObject->IsValid())
			{
				continue;
			}
			if ((*NotifyObject)->GetStringField(TEXT("notify_name")) == NotifyNameNeedle
				&& (*NotifyObject)->GetBoolField(TEXT("is_state")) == bRequireState)
			{
				OutEntryObject = *NotifyObject;
				return true;
			}
		}

		return false;
	};

	auto SkeletonGroupContainsSlot = [](const TSharedPtr<FJsonObject>* DataObject, const FString& GroupName, const FString& SlotName, bool& bFoundOut) -> bool
	{
		bFoundOut = false;
		if (!DataObject || !DataObject->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* SlotGroups = nullptr;
		if (!(*DataObject)->TryGetArrayField(TEXT("slot_groups"), SlotGroups) || !SlotGroups)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& GroupValue : *SlotGroups)
		{
			const TSharedPtr<FJsonObject>* GroupObject = nullptr;
			if (!GroupValue.IsValid() || !GroupValue->TryGetObject(GroupObject) || !GroupObject || !GroupObject->IsValid())
			{
				continue;
			}
			if ((*GroupObject)->GetStringField(TEXT("group_name")) != GroupName)
			{
				continue;
			}

			const TArray<TSharedPtr<FJsonValue>>* SlotNames = nullptr;
			if (!(*GroupObject)->TryGetArrayField(TEXT("slot_names"), SlotNames) || !SlotNames)
			{
				return false;
			}
			for (const TSharedPtr<FJsonValue>& SlotValue : *SlotNames)
			{
				if (SlotValue.IsValid() && SlotValue->AsString() == SlotName)
				{
					bFoundOut = true;
					return true;
				}
			}
			return true;
		}

		return true;
	};

	const FString CreateMontageBody = MakeExecRequestBody(
		TEXT("smoke-montage-create-001"),
		TEXT("montage_create"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"target_skeleton\":\"%s\",\"source_animation\":\"%s\",\"preview_skeletal_mesh\":\"%s\",\"open_editor\":false,\"save_after_create\":false}"), *MontageAssetPath, *SkeletonAssetPath, *IdleAnimationAssetPath, *PreviewMeshAssetPath));
	FHttpSmokeResult CreateMontageResult;
	TestTrue(TEXT("Montage create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateMontageBody, CreateMontageResult));
	TestEqual(TEXT("Montage create status code"), CreateMontageResult.StatusCode, 200);

	const FString ListMontagesBody = MakeExecRequestBody(
		TEXT("smoke-montage-list-001"),
		TEXT("montage_list_montages"),
		TEXT("{\"root_path\":\"/Game/__UeAgentInterfaceSmoke\",\"limit\":64}"));
	FHttpSmokeResult ListMontagesResult;
	TestTrue(TEXT("Montage list request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ListMontagesBody, ListMontagesResult));
	TestEqual(TEXT("Montage list status code"), ListMontagesResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ListMontagesJson;
	TestTrue(TEXT("Montage list parses JSON"), ParseJson(ListMontagesResult.Body, ListMontagesJson));
	const TSharedPtr<FJsonObject>* ListMontagesData = nullptr;
	TestTrue(TEXT("Montage list contains data"), ListMontagesJson.IsValid() && ListMontagesJson->TryGetObjectField(TEXT("data"), ListMontagesData) && ListMontagesData && ListMontagesData->IsValid());
	if (ListMontagesData && ListMontagesData->IsValid())
	{
		bool bFoundMontage = false;
		TestTrue(TEXT("Montage list items array exists"), ListContainsMontage(ListMontagesData, MontageAssetPath, bFoundMontage));
		TestTrue(TEXT("Montage list contains created montage"), bFoundMontage);
	}

	const FString ClearPreviewMeshBody = MakeExecRequestBody(
		TEXT("smoke-montage-preview-clear-001"),
		TEXT("montage_set_preview_mesh"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"clear_preview_mesh\":true,\"save_after_set\":false}"), *MontageAssetPath));
	FHttpSmokeResult ClearPreviewMeshResult;
	TestTrue(TEXT("Montage clear preview mesh request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ClearPreviewMeshBody, ClearPreviewMeshResult));
	TestEqual(TEXT("Montage clear preview mesh status code"), ClearPreviewMeshResult.StatusCode, 200);

	const FString RestorePreviewMeshBody = MakeExecRequestBody(
		TEXT("smoke-montage-preview-restore-001"),
		TEXT("montage_set_preview_mesh"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"skeletal_mesh_path\":\"%s\",\"save_after_set\":false}"), *MontageAssetPath, *PreviewMeshAssetPath));
	FHttpSmokeResult RestorePreviewMeshResult;
	TestTrue(TEXT("Montage restore preview mesh request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RestorePreviewMeshBody, RestorePreviewMeshResult));
	TestEqual(TEXT("Montage restore preview mesh status code"), RestorePreviewMeshResult.StatusCode, 200);

	const FString AddSlotTrackBody = MakeExecRequestBody(
		TEXT("smoke-montage-add-slot-001"),
		TEXT("montage_add_slot_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"slot_name\":\"%s\",\"save_after_add\":false}"), *MontageAssetPath, *AddedSlotName));
	FHttpSmokeResult AddSlotTrackResult;
	TestTrue(TEXT("Montage add slot track request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddSlotTrackBody, AddSlotTrackResult));
	TestEqual(TEXT("Montage add slot track status code"), AddSlotTrackResult.StatusCode, 200);

	const FString RenameSlotTrackBody = MakeExecRequestBody(
		TEXT("smoke-montage-rename-slot-001"),
		TEXT("montage_rename_slot_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"slot_name\":\"%s\",\"new_slot_name\":\"%s\",\"save_after_rename\":false}"), *MontageAssetPath, *AddedSlotName, *RenamedSlotName));
	FHttpSmokeResult RenameSlotTrackResult;
	TestTrue(TEXT("Montage rename slot track request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RenameSlotTrackBody, RenameSlotTrackResult));
	TestEqual(TEXT("Montage rename slot track status code"), RenameSlotTrackResult.StatusCode, 200);

	const FString AddSegmentBody = MakeExecRequestBody(
		TEXT("smoke-montage-add-segment-001"),
		TEXT("montage_add_segment"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"slot_name\":\"%s\",\"animation_asset\":\"%s\",\"start_pos\":0.0,\"play_rate\":1.0,\"loop_count\":1,\"save_after_add\":false}"), *MontageAssetPath, *RenamedSlotName, *WalkAnimationAssetPath));
	FHttpSmokeResult AddSegmentResult;
	TestTrue(TEXT("Montage add segment request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddSegmentBody, AddSegmentResult));
	TestEqual(TEXT("Montage add segment status code"), AddSegmentResult.StatusCode, 200);

	const FString UpdateSegmentBody = MakeExecRequestBody(
		TEXT("smoke-montage-update-segment-001"),
		TEXT("montage_update_segment"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"slot_name\":\"%s\",\"segment_index\":0,\"play_rate\":1.5,\"loop_count\":2,\"save_after_set\":false}"), *MontageAssetPath, *RenamedSlotName));
	FHttpSmokeResult UpdateSegmentResult;
	TestTrue(TEXT("Montage update segment request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, UpdateSegmentBody, UpdateSegmentResult));
	TestEqual(TEXT("Montage update segment status code"), UpdateSegmentResult.StatusCode, 200);

	const FString AddSectionBody = MakeExecRequestBody(
		TEXT("smoke-montage-add-section-001"),
		TEXT("montage_add_section"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"section_name\":\"%s\",\"time_seconds\":0.5,\"save_after_add\":false}"), *MontageAssetPath, *AddedSectionName));
	FHttpSmokeResult AddSectionResult;
	TestTrue(TEXT("Montage add section request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddSectionBody, AddSectionResult));
	TestEqual(TEXT("Montage add section status code"), AddSectionResult.StatusCode, 200);

	const FString RenameSectionBody = MakeExecRequestBody(
		TEXT("smoke-montage-rename-section-001"),
		TEXT("montage_rename_section"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"section_name\":\"%s\",\"new_section_name\":\"%s\",\"save_after_rename\":false}"), *MontageAssetPath, *AddedSectionName, *RenamedSectionName));
	FHttpSmokeResult RenameSectionResult;
	TestTrue(TEXT("Montage rename section request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RenameSectionBody, RenameSectionResult));
	TestEqual(TEXT("Montage rename section status code"), RenameSectionResult.StatusCode, 200);

	const FString SetSectionTimeBody = MakeExecRequestBody(
		TEXT("smoke-montage-set-section-time-001"),
		TEXT("montage_set_section_time"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"section_name\":\"%s\",\"time_seconds\":0.75,\"save_after_set\":false}"), *MontageAssetPath, *RenamedSectionName));
	FHttpSmokeResult SetSectionTimeResult;
	TestTrue(TEXT("Montage set section time request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetSectionTimeBody, SetSectionTimeResult));
	TestEqual(TEXT("Montage set section time status code"), SetSectionTimeResult.StatusCode, 200);

	const FString SetNextSectionBody = MakeExecRequestBody(
		TEXT("smoke-montage-set-next-001"),
		TEXT("montage_set_next_section"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"section_name\":\"Default\",\"next_section_name\":\"%s\",\"save_after_set\":false}"), *MontageAssetPath, *RenamedSectionName));
	FHttpSmokeResult SetNextSectionResult;
	TestTrue(TEXT("Montage set next section request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetNextSectionBody, SetNextSectionResult));
	TestEqual(TEXT("Montage set next section status code"), SetNextSectionResult.StatusCode, 200);

	const FString SetBlendOptionsBody = MakeExecRequestBody(
		TEXT("smoke-montage-blend-001"),
		TEXT("montage_set_blend_options"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"blend_in_time\":0.15,\"blend_out_time\":0.4,\"enable_auto_blend_out\":false,\"blend_mode_in\":\"standard\",\"blend_mode_out\":\"standard\",\"save_after_set\":false}"), *MontageAssetPath));
	FHttpSmokeResult SetBlendOptionsResult;
	TestTrue(TEXT("Montage set blend options request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetBlendOptionsBody, SetBlendOptionsResult));
	TestEqual(TEXT("Montage set blend options status code"), SetBlendOptionsResult.StatusCode, 200);

	const FString AddNotifyTrackBody = MakeExecRequestBody(
		TEXT("smoke-montage-add-notify-track-001"),
		TEXT("montage_add_notify_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"track_name\":\"%s\",\"track_color\":{\"r\":0.15,\"g\":0.35,\"b\":0.65,\"a\":1.0},\"save_after_add\":false}"), *MontageAssetPath, *NotifyTrackName));
	FHttpSmokeResult AddNotifyTrackResult;
	TestTrue(TEXT("Montage add notify track request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddNotifyTrackBody, AddNotifyTrackResult));
	TestEqual(TEXT("Montage add notify track status code"), AddNotifyTrackResult.StatusCode, 200);

	const FString AddNotifyBody = MakeExecRequestBody(
		TEXT("smoke-montage-add-notify-001"),
		TEXT("montage_add_notify"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"track_name\":\"%s\",\"notify_name\":\"%s\",\"time_seconds\":0.2,\"notify_color\":{\"r\":0.20,\"g\":0.80,\"b\":0.25,\"a\":1.0},\"trigger_weight_threshold\":0.35,\"notify_trigger_chance\":0.55,\"notify_filter_type\":\"lod\",\"notify_filter_lod\":2,\"can_be_filtered_via_request\":true,\"trigger_on_dedicated_server\":false,\"trigger_on_follower\":true,\"save_after_add\":false}"), *MontageAssetPath, *NotifyTrackName, *NotifyName));
	FHttpSmokeResult AddNotifyResult;
	TestTrue(TEXT("Montage add notify request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddNotifyBody, AddNotifyResult));
	TestEqual(TEXT("Montage add notify status code"), AddNotifyResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddNotifyJson;
	TestTrue(TEXT("Montage add notify parses JSON"), ParseJson(AddNotifyResult.Body, AddNotifyJson));
	int32 NotifyIndex = INDEX_NONE;
	const TSharedPtr<FJsonObject>* AddNotifyData = nullptr;
	TestTrue(TEXT("Montage add notify contains data"), AddNotifyJson.IsValid() && AddNotifyJson->TryGetObjectField(TEXT("data"), AddNotifyData) && AddNotifyData && AddNotifyData->IsValid());
	if (AddNotifyData && AddNotifyData->IsValid())
	{
		NotifyIndex = static_cast<int32>((*AddNotifyData)->GetIntegerField(TEXT("notify_index")));
	}

	const FString AddNotifyStateBody = MakeExecRequestBody(
		TEXT("smoke-montage-add-notify-state-001"),
		TEXT("montage_add_notify_state"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"track_name\":\"%s\",\"notify_state_class\":\"/Script/Engine.AnimNotifyState_DisableRootMotion\",\"time_seconds\":0.3,\"duration_seconds\":0.4,\"notify_color\":{\"r\":0.20,\"g\":0.40,\"b\":0.90,\"a\":1.0},\"trigger_weight_threshold\":0.25,\"notify_trigger_chance\":0.90,\"notify_filter_type\":\"none\",\"can_be_filtered_via_request\":false,\"trigger_on_dedicated_server\":true,\"trigger_on_follower\":false,\"save_after_add\":false}"), *MontageAssetPath, *NotifyTrackName));
	FHttpSmokeResult AddNotifyStateResult;
	TestTrue(TEXT("Montage add notify state request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddNotifyStateBody, AddNotifyStateResult));
	TestEqual(TEXT("Montage add notify state status code"), AddNotifyStateResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddNotifyStateJson;
	TestTrue(TEXT("Montage add notify state parses JSON"), ParseJson(AddNotifyStateResult.Body, AddNotifyStateJson));
	int32 NotifyStateIndex = INDEX_NONE;
	FString NotifyStateName;
	const TSharedPtr<FJsonObject>* AddNotifyStateData = nullptr;
	TestTrue(TEXT("Montage add notify state contains data"), AddNotifyStateJson.IsValid() && AddNotifyStateJson->TryGetObjectField(TEXT("data"), AddNotifyStateData) && AddNotifyStateData && AddNotifyStateData->IsValid());
	if (AddNotifyStateData && AddNotifyStateData->IsValid())
	{
		NotifyStateIndex = static_cast<int32>((*AddNotifyStateData)->GetIntegerField(TEXT("notify_index")));
		NotifyStateName = (*AddNotifyStateData)->GetStringField(TEXT("notify_name"));
	}

	const FString UpdateNotifyBody = MakeExecRequestBody(
		TEXT("smoke-montage-update-notify-001"),
		TEXT("montage_update_notify"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"notify_index\":%d,\"time_seconds\":0.25,\"tick_type\":\"branching_point\",\"notify_color\":{\"r\":0.90,\"g\":0.30,\"b\":0.10,\"a\":1.0},\"trigger_weight_threshold\":0.60,\"notify_trigger_chance\":0.80,\"notify_filter_type\":\"lod\",\"notify_filter_lod\":1,\"can_be_filtered_via_request\":true,\"trigger_on_dedicated_server\":false,\"trigger_on_follower\":true,\"save_after_set\":false}"), *MontageAssetPath, NotifyIndex));
	FHttpSmokeResult UpdateNotifyResult;
	TestTrue(TEXT("Montage update notify request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, UpdateNotifyBody, UpdateNotifyResult));
	TestEqual(TEXT("Montage update notify status code"), UpdateNotifyResult.StatusCode, 200);

	const FString SetSkeletonSlotGroupBody = MakeExecRequestBody(
		TEXT("smoke-montage-set-skeleton-slot-group-001"),
		TEXT("montage_set_skeleton_slot_group"),
		FString::Printf(TEXT("{\"skeleton_path\":\"%s\",\"slot_name\":\"%s\",\"group_name\":\"%s\",\"save_skeleton\":false}"), *SkeletonAssetPath, *SkeletonSlotName, *SkeletonSlotGroupName));
	FHttpSmokeResult SetSkeletonSlotGroupResult;
	TestTrue(TEXT("Montage set skeleton slot group request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetSkeletonSlotGroupBody, SetSkeletonSlotGroupResult));
	TestEqual(TEXT("Montage set skeleton slot group status code"), SetSkeletonSlotGroupResult.StatusCode, 200);

	const FString RenameSkeletonSlotBody = MakeExecRequestBody(
		TEXT("smoke-montage-rename-skeleton-slot-001"),
		TEXT("montage_rename_skeleton_slot"),
		FString::Printf(TEXT("{\"skeleton_path\":\"%s\",\"slot_name\":\"%s\",\"new_slot_name\":\"%s\",\"save_skeleton\":false}"), *SkeletonAssetPath, *SkeletonSlotName, *RenamedSkeletonSlotName));
	FHttpSmokeResult RenameSkeletonSlotResult;
	TestTrue(TEXT("Montage rename skeleton slot request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RenameSkeletonSlotBody, RenameSkeletonSlotResult));
	TestEqual(TEXT("Montage rename skeleton slot status code"), RenameSkeletonSlotResult.StatusCode, 200);

	const FString ListSkeletonSlotsBody = MakeExecRequestBody(
		TEXT("smoke-montage-list-skeleton-slots-001"),
		TEXT("montage_list_skeleton_slots"),
		FString::Printf(TEXT("{\"skeleton_path\":\"%s\"}"), *SkeletonAssetPath));
	FHttpSmokeResult ListSkeletonSlotsResult;
	TestTrue(TEXT("Montage list skeleton slots request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ListSkeletonSlotsBody, ListSkeletonSlotsResult));
	TestEqual(TEXT("Montage list skeleton slots status code"), ListSkeletonSlotsResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ListSkeletonSlotsJson;
	TestTrue(TEXT("Montage list skeleton slots parses JSON"), ParseJson(ListSkeletonSlotsResult.Body, ListSkeletonSlotsJson));
	const TSharedPtr<FJsonObject>* ListSkeletonSlotsData = nullptr;
	TestTrue(TEXT("Montage list skeleton slots contains data"), ListSkeletonSlotsJson.IsValid() && ListSkeletonSlotsJson->TryGetObjectField(TEXT("data"), ListSkeletonSlotsData) && ListSkeletonSlotsData && ListSkeletonSlotsData->IsValid());
	if (ListSkeletonSlotsData && ListSkeletonSlotsData->IsValid())
	{
		bool bSkeletonSlotFound = false;
		TestTrue(TEXT("Montage skeleton slot group query succeeds"), SkeletonGroupContainsSlot(ListSkeletonSlotsData, SkeletonSlotGroupName, RenamedSkeletonSlotName, bSkeletonSlotFound));
		TestTrue(TEXT("Montage skeleton slot found in target group"), bSkeletonSlotFound);
	}

	const FString SetSyncGroupBody = MakeExecRequestBody(
		TEXT("smoke-montage-set-sync-group-001"),
		TEXT("montage_set_sync_group"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"sync_group_name\":\"%s\",\"sync_slot_name\":\"%s\",\"save_after_set\":false}"), *MontageAssetPath, *SyncGroupName, *RenamedSlotName));
	FHttpSmokeResult SetSyncGroupResult;
	TestTrue(TEXT("Montage set sync group request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetSyncGroupBody, SetSyncGroupResult));
	TestEqual(TEXT("Montage set sync group status code"), SetSyncGroupResult.StatusCode, 200);

	const FString GetInfoBody = MakeExecRequestBody(
		TEXT("smoke-montage-info-001"),
		TEXT("montage_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *MontageAssetPath));
	FHttpSmokeResult GetInfoResult;
	TestTrue(TEXT("Montage get info request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoBody, GetInfoResult));
	TestEqual(TEXT("Montage get info status code"), GetInfoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetInfoJson;
	TestTrue(TEXT("Montage get info parses JSON"), ParseJson(GetInfoResult.Body, GetInfoJson));
	const TSharedPtr<FJsonObject>* GetInfoData = nullptr;
	TestTrue(TEXT("Montage get info contains data"), GetInfoJson.IsValid() && GetInfoJson->TryGetObjectField(TEXT("data"), GetInfoData) && GetInfoData && GetInfoData->IsValid());
	if (GetInfoData && GetInfoData->IsValid())
	{
		TestEqual(TEXT("Montage preview mesh matches"), (*GetInfoData)->GetStringField(TEXT("preview_skeletal_mesh")), PreviewMeshAssetPath);
		TestEqual(TEXT("Montage auto blend out updated"), (*GetInfoData)->GetBoolField(TEXT("enable_auto_blend_out")), false);
		TestEqual(TEXT("Montage sync group updated"), (*GetInfoData)->GetStringField(TEXT("sync_group_name")), SyncGroupName);
		TestEqual(TEXT("Montage sync slot updated"), (*GetInfoData)->GetStringField(TEXT("sync_slot_name")), RenamedSlotName);
		TestEqual(TEXT("Montage sync slot valid"), (*GetInfoData)->GetBoolField(TEXT("sync_slot_valid")), true);
		TestTrue(TEXT("Montage sync marker count is non-negative"), static_cast<int32>((*GetInfoData)->GetIntegerField(TEXT("sync_marker_count"))) >= 0);

		TSharedPtr<FJsonObject> SlotTrackEntry;
		TSharedPtr<FJsonObject> SectionEntry;
		TSharedPtr<FJsonObject> NotifyTrackEntry;
		TSharedPtr<FJsonObject> NotifyEntry;
		TSharedPtr<FJsonObject> NotifyStateEntry;
		TestTrue(TEXT("Montage renamed slot exists"), FindSlotTrackEntry(GetInfoData, RenamedSlotName, SlotTrackEntry));
		TestTrue(TEXT("Montage renamed section exists"), FindSectionEntry(GetInfoData, RenamedSectionName, SectionEntry));
		TestTrue(TEXT("Montage notify track exists"), FindNotifyTrackEntry(GetInfoData, NotifyTrackName, NotifyTrackEntry));
		TestTrue(TEXT("Montage named notify exists"), FindNotifyEntry(GetInfoData, NotifyName, false, NotifyEntry));
		TestTrue(TEXT("Montage notify state exists"), FindNotifyEntry(GetInfoData, NotifyStateName, true, NotifyStateEntry));
		if (SlotTrackEntry.IsValid())
		{
			TestEqual(TEXT("Montage renamed slot segment count"), static_cast<int32>(SlotTrackEntry->GetIntegerField(TEXT("segment_count"))), 1);
			TestEqual(TEXT("Montage renamed slot registered on skeleton"), SlotTrackEntry->GetBoolField(TEXT("skeleton_slot_registered")), false);
		}
		if (SectionEntry.IsValid())
		{
			TestEqual(TEXT("Montage section next name remains empty"), SectionEntry->GetStringField(TEXT("next_section_name")), FString(TEXT("")));
			TestEqual(TEXT("Montage section time updated"), static_cast<float>(SectionEntry->GetNumberField(TEXT("time_seconds"))), 0.75f);
		}

		TSharedPtr<FJsonObject> DefaultSectionEntry;
		TestTrue(TEXT("Montage default section exists"), FindSectionEntry(GetInfoData, TEXT("Default"), DefaultSectionEntry));
		if (DefaultSectionEntry.IsValid())
		{
			TestEqual(TEXT("Montage default section next points to renamed section"), DefaultSectionEntry->GetStringField(TEXT("next_section_name")), RenamedSectionName);
		}
		if (NotifyTrackEntry.IsValid())
		{
			TestEqual(TEXT("Montage notify track count updated"), static_cast<int32>(NotifyTrackEntry->GetIntegerField(TEXT("notify_count"))), 2);
			CheckSmokeLinearColorField(this, NotifyTrackEntry, TEXT("track_color_linear"), FLinearColor(0.15f, 0.35f, 0.65f, 1.0f), TEXT("Montage notify track"));
		}
		if (NotifyEntry.IsValid())
		{
			TestEqual(TEXT("Montage notify updated to branching point"), NotifyEntry->GetBoolField(TEXT("branching_point")), true);
			TestEqual(TEXT("Montage notify tick type updated"), NotifyEntry->GetStringField(TEXT("tick_type")), FString(TEXT("branching_point")));
			TestEqual(TEXT("Montage notify filter type updated"), NotifyEntry->GetStringField(TEXT("notify_filter_type")), FString(TEXT("lod")));
			TestEqual(TEXT("Montage notify filter lod updated"), static_cast<int32>(NotifyEntry->GetIntegerField(TEXT("notify_filter_lod"))), 1);
			TestEqual(TEXT("Montage notify trigger weight threshold updated"), static_cast<float>(NotifyEntry->GetNumberField(TEXT("trigger_weight_threshold"))), 0.60f);
			TestEqual(TEXT("Montage notify trigger chance updated"), static_cast<float>(NotifyEntry->GetNumberField(TEXT("notify_trigger_chance"))), 0.80f);
			TestEqual(TEXT("Montage notify can be filtered via request updated"), NotifyEntry->GetBoolField(TEXT("can_be_filtered_via_request")), true);
			TestEqual(TEXT("Montage notify trigger on dedicated server updated"), NotifyEntry->GetBoolField(TEXT("trigger_on_dedicated_server")), false);
			TestEqual(TEXT("Montage notify trigger on follower updated"), NotifyEntry->GetBoolField(TEXT("trigger_on_follower")), true);
			CheckSmokeLinearColorField(this, NotifyEntry, TEXT("notify_color_linear"), FLinearColor(0.90f, 0.30f, 0.10f, 1.0f), TEXT("Montage notify"));
		}
		if (NotifyStateEntry.IsValid())
		{
			TestEqual(TEXT("Montage notify state duration updated"), static_cast<float>(NotifyStateEntry->GetNumberField(TEXT("duration_seconds"))), 0.4f);
			TestEqual(TEXT("Montage notify state filter type updated"), NotifyStateEntry->GetStringField(TEXT("notify_filter_type")), FString(TEXT("none")));
			TestEqual(TEXT("Montage notify state trigger chance updated"), static_cast<float>(NotifyStateEntry->GetNumberField(TEXT("notify_trigger_chance"))), 0.90f);
			TestEqual(TEXT("Montage notify state can be filtered via request updated"), NotifyStateEntry->GetBoolField(TEXT("can_be_filtered_via_request")), false);
			TestEqual(TEXT("Montage notify state trigger on dedicated server updated"), NotifyStateEntry->GetBoolField(TEXT("trigger_on_dedicated_server")), true);
			TestEqual(TEXT("Montage notify state trigger on follower updated"), NotifyStateEntry->GetBoolField(TEXT("trigger_on_follower")), false);
			CheckSmokeLinearColorField(this, NotifyStateEntry, TEXT("notify_color_linear"), FLinearColor(0.20f, 0.40f, 0.90f, 1.0f), TEXT("Montage notify state"));
		}
	}

	const int32 RemoveNotifyStateFirstIndex = FMath::Max(NotifyIndex, NotifyStateIndex);
	const int32 RemoveNotifySecondIndex = FMath::Min(NotifyIndex, NotifyStateIndex);

	const FString RemoveNotifyStateBody = MakeExecRequestBody(
		TEXT("smoke-montage-remove-notify-state-001"),
		TEXT("montage_remove_notify"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"notify_index\":%d,\"save_after_remove\":false}"), *MontageAssetPath, RemoveNotifyStateFirstIndex));
	FHttpSmokeResult RemoveNotifyStateResult;
	TestTrue(TEXT("Montage remove notify state request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveNotifyStateBody, RemoveNotifyStateResult));
	TestEqual(TEXT("Montage remove notify state status code"), RemoveNotifyStateResult.StatusCode, 200);

	const FString RemoveNotifyBody = MakeExecRequestBody(
		TEXT("smoke-montage-remove-notify-001"),
		TEXT("montage_remove_notify"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"notify_index\":%d,\"save_after_remove\":false}"), *MontageAssetPath, RemoveNotifySecondIndex == RemoveNotifyStateFirstIndex ? 0 : RemoveNotifySecondIndex));
	FHttpSmokeResult RemoveNotifyResult;
	TestTrue(TEXT("Montage remove notify request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveNotifyBody, RemoveNotifyResult));
	TestEqual(TEXT("Montage remove notify status code"), RemoveNotifyResult.StatusCode, 200);

	const FString RemoveNotifyTrackBody = MakeExecRequestBody(
		TEXT("smoke-montage-remove-notify-track-001"),
		TEXT("montage_remove_notify_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"track_name\":\"%s\",\"save_after_remove\":false}"), *MontageAssetPath, *NotifyTrackName));
	FHttpSmokeResult RemoveNotifyTrackResult;
	TestTrue(TEXT("Montage remove notify track request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveNotifyTrackBody, RemoveNotifyTrackResult));
	TestEqual(TEXT("Montage remove notify track status code"), RemoveNotifyTrackResult.StatusCode, 200);

	const FString RemoveSkeletonSlotBody = MakeExecRequestBody(
		TEXT("smoke-montage-remove-skeleton-slot-001"),
		TEXT("montage_remove_skeleton_slot"),
		FString::Printf(TEXT("{\"skeleton_path\":\"%s\",\"slot_name\":\"%s\",\"save_skeleton\":false}"), *SkeletonAssetPath, *RenamedSkeletonSlotName));
	FHttpSmokeResult RemoveSkeletonSlotResult;
	TestTrue(TEXT("Montage remove skeleton slot request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveSkeletonSlotBody, RemoveSkeletonSlotResult));
	TestEqual(TEXT("Montage remove skeleton slot status code"), RemoveSkeletonSlotResult.StatusCode, 200);

	const FString ResolveSkeletonDirtyBody = MakeExecRequestBody(
		TEXT("smoke-montage-resolve-skeleton-dirty-001"),
		TEXT("editor_resolve_dirty_resources"),
		FString::Printf(TEXT("{\"discard_resource_paths\":[\"%s\"]}"), *SkeletonAssetPath));
	FHttpSmokeResult ResolveSkeletonDirtyResult;
	TestTrue(TEXT("Montage resolve skeleton dirty request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ResolveSkeletonDirtyBody, ResolveSkeletonDirtyResult));
	TestEqual(TEXT("Montage resolve skeleton dirty status code"), ResolveSkeletonDirtyResult.StatusCode, 200);

	const FString RemoveSectionBody = MakeExecRequestBody(
		TEXT("smoke-montage-remove-section-001"),
		TEXT("montage_remove_section"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"section_name\":\"%s\",\"save_after_remove\":false}"), *MontageAssetPath, *RenamedSectionName));
	FHttpSmokeResult RemoveSectionResult;
	TestTrue(TEXT("Montage remove section request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveSectionBody, RemoveSectionResult));
	TestEqual(TEXT("Montage remove section status code"), RemoveSectionResult.StatusCode, 200);

	const FString RemoveSegmentBody = MakeExecRequestBody(
		TEXT("smoke-montage-remove-segment-001"),
		TEXT("montage_remove_segment"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"slot_name\":\"%s\",\"segment_index\":0,\"save_after_remove\":false}"), *MontageAssetPath, *RenamedSlotName));
	FHttpSmokeResult RemoveSegmentResult;
	TestTrue(TEXT("Montage remove segment request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveSegmentBody, RemoveSegmentResult));
	TestEqual(TEXT("Montage remove segment status code"), RemoveSegmentResult.StatusCode, 200);

	const FString RemoveSlotTrackBody = MakeExecRequestBody(
		TEXT("smoke-montage-remove-slot-001"),
		TEXT("montage_remove_slot_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"slot_name\":\"%s\",\"save_after_remove\":false}"), *MontageAssetPath, *RenamedSlotName));
	FHttpSmokeResult RemoveSlotTrackResult;
	TestTrue(TEXT("Montage remove slot track request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveSlotTrackBody, RemoveSlotTrackResult));
	TestEqual(TEXT("Montage remove slot track status code"), RemoveSlotTrackResult.StatusCode, 200);

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceMontageJsonSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.MontageJsonWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceMontageJsonSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString MontageAssetPath = MakeAutomationAssetPath(TEXT("MNTJSON"));
	const FString SkeletonAssetPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton.TutorialTPP_Skeleton");
	const FString PreviewMeshAssetPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP.TutorialTPP");
	const FString IdleAnimationAssetPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Idle.Tutorial_Idle");
	const FString WalkAnimationAssetPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Walk_Fwd.Tutorial_Walk_Fwd");
	const FString AddedSlotName = TEXT("JsonUpperBody");
	const FString AddedSectionName = TEXT("JsonLoop");
	const FString NotifyTrackName = TEXT("JsonNotifyTrack");
	const FString NotifyName = TEXT("JsonNotify");
	const FString SyncGroupName = TEXT("JsonSyncGroup");
	const FString JsonDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("MontageJson"));
	const FString JsonPath = FPaths::Combine(JsonDir, TEXT("Montage.json"));
	const FString CreateIfMissingJsonPath = FPaths::Combine(JsonDir, TEXT("MontageCreateIfMissing.json"));
	const FString CreateIfMissingAssetPath = MakeAutomationAssetPath(TEXT("MNTJSON_CREATE"));

	auto LoadJsonFile = [this](const FString& FilePath, TSharedPtr<FJsonObject>& OutObj) -> bool
	{
		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
		{
			AddError(FString::Printf(TEXT("Load file failed: %s"), *FilePath));
			return false;
		}
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, OutObj) || !OutObj.IsValid())
		{
			AddError(FString::Printf(TEXT("Parse json failed: %s"), *FilePath));
			return false;
		}
		return true;
	};

	auto SaveJsonFile = [this](const FString& FilePath, const TSharedPtr<FJsonObject>& Obj) -> bool
	{
		FString JsonText;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
		if (!FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer))
		{
			AddError(FString::Printf(TEXT("Serialize json failed: %s"), *FilePath));
			return false;
		}
		if (!FFileHelper::SaveStringToFile(JsonText, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			AddError(FString::Printf(TEXT("Save file failed: %s"), *FilePath));
			return false;
		}
		return true;
	};

	IFileManager::Get().MakeDirectory(*JsonDir, true);

	const FString CreateMontageBody = MakeExecRequestBody(
		TEXT("smoke-montage-json-create-001"),
		TEXT("montage_create"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"target_skeleton\":\"%s\",\"source_animation\":\"%s\",\"preview_skeletal_mesh\":\"%s\",\"open_editor\":false,\"save_after_create\":false}"), *MontageAssetPath, *SkeletonAssetPath, *IdleAnimationAssetPath, *PreviewMeshAssetPath));
	FHttpSmokeResult CreateMontageResult;
	TestTrue(TEXT("Montage json create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateMontageBody, CreateMontageResult));
	TestEqual(TEXT("Montage json create status code"), CreateMontageResult.StatusCode, 200);

	const FString ExportBody = MakeExecRequestBody(
		TEXT("smoke-montage-json-export-001"),
		TEXT("montage_export_json"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"output_file\":\"%s\"}"), *MontageAssetPath, *JsonPath.Replace(TEXT("\\"), TEXT("/"))));
	FHttpSmokeResult ExportResult;
	TestTrue(TEXT("Montage json export request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ExportBody, ExportResult));
	TestEqual(TEXT("Montage json export status code"), ExportResult.StatusCode, 200);

	TSharedPtr<FJsonObject> MontageJson;
	TestTrue(TEXT("Montage json file loads"), LoadJsonFile(JsonPath, MontageJson));
	if (MontageJson.IsValid())
	{
		TSharedPtr<FJsonObject> CreateIfMissingJson = MakeShared<FJsonObject>(*MontageJson);
		CreateIfMissingJson->SetStringField(TEXT("asset_path"), CreateIfMissingAssetPath);
		CreateIfMissingJson->SetStringField(TEXT("asset_name"), FPackageName::GetLongPackageAssetName(CreateIfMissingAssetPath));
		TestTrue(TEXT("Montage create-if-missing json file saves"), SaveJsonFile(CreateIfMissingJsonPath, CreateIfMissingJson));

		const FString CreateIfMissingApplyBody = MakeExecRequestBody(
			TEXT("smoke-montage-json-create-if-missing-apply-001"),
			TEXT("montage_apply_json"),
			FString::Printf(TEXT("{\"json_file\":\"%s\",\"create_if_missing\":true,\"save_after_apply\":false}"), *CreateIfMissingJsonPath.Replace(TEXT("\\"), TEXT("/"))));
		FHttpSmokeResult CreateIfMissingApplyResult;
		TestTrue(TEXT("Montage json create-if-missing apply request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateIfMissingApplyBody, CreateIfMissingApplyResult));
		TestEqual(TEXT("Montage json create-if-missing apply status code"), CreateIfMissingApplyResult.StatusCode, 200);

		const FString CreateIfMissingInfoBody = MakeExecRequestBody(
			TEXT("smoke-montage-json-create-if-missing-info-001"),
			TEXT("montage_get_info"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *CreateIfMissingAssetPath));
		FHttpSmokeResult CreateIfMissingInfoResult;
		TestTrue(TEXT("Montage json create-if-missing get info request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateIfMissingInfoBody, CreateIfMissingInfoResult));
		TestEqual(TEXT("Montage json create-if-missing get info status code"), CreateIfMissingInfoResult.StatusCode, 200);
	}
	if (MontageJson.IsValid())
	{
		MontageJson->SetArrayField(TEXT("skeleton_slots"), TArray<TSharedPtr<FJsonValue>>());

		const TSharedPtr<FJsonObject>* SettingsObj = nullptr;
		TestTrue(TEXT("Montage json settings exists"), MontageJson->TryGetObjectField(TEXT("settings"), SettingsObj) && SettingsObj && (*SettingsObj).IsValid());
		if (SettingsObj && (*SettingsObj).IsValid())
		{
			(*SettingsObj)->SetNumberField(TEXT("blend_in_time"), 0.11);
			(*SettingsObj)->SetNumberField(TEXT("blend_out_time"), 0.33);
			(*SettingsObj)->SetBoolField(TEXT("enable_auto_blend_out"), false);
		}

		const TSharedPtr<FJsonObject>* SyncObj = nullptr;
		TestTrue(TEXT("Montage json sync exists"), MontageJson->TryGetObjectField(TEXT("sync"), SyncObj) && SyncObj && (*SyncObj).IsValid());
		if (SyncObj && (*SyncObj).IsValid())
		{
			(*SyncObj)->SetStringField(TEXT("sync_group_name"), SyncGroupName);
			(*SyncObj)->SetStringField(TEXT("sync_slot_name"), AddedSlotName);
		}

		const TArray<TSharedPtr<FJsonValue>>* SlotTracks = nullptr;
		TestTrue(TEXT("Montage json slot tracks exists"), MontageJson->TryGetArrayField(TEXT("slot_tracks"), SlotTracks) && SlotTracks != nullptr);
		if (SlotTracks)
		{
			TSharedPtr<FJsonObject> AddedSlotObj = MakeShared<FJsonObject>();
			AddedSlotObj->SetStringField(TEXT("slot_name"), AddedSlotName);
			TArray<TSharedPtr<FJsonValue>> Segments;
			TSharedPtr<FJsonObject> SegmentObj = MakeShared<FJsonObject>();
			SegmentObj->SetStringField(TEXT("animation_asset"), WalkAnimationAssetPath);
			SegmentObj->SetNumberField(TEXT("start_pos"), 0.0);
			SegmentObj->SetNumberField(TEXT("anim_start_time"), 0.0);
			SegmentObj->SetNumberField(TEXT("anim_end_time"), 0.6);
			SegmentObj->SetNumberField(TEXT("play_rate"), 1.25);
			SegmentObj->SetNumberField(TEXT("loop_count"), 1);
			Segments.Add(MakeShared<FJsonValueObject>(SegmentObj));
			AddedSlotObj->SetArrayField(TEXT("segments"), Segments);
			const_cast<TArray<TSharedPtr<FJsonValue>>*>(SlotTracks)->Add(MakeShared<FJsonValueObject>(AddedSlotObj));
		}

		const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
		TestTrue(TEXT("Montage json sections exists"), MontageJson->TryGetArrayField(TEXT("sections"), Sections) && Sections != nullptr);
		if (Sections)
		{
			for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
			{
				const TSharedPtr<FJsonObject>* SectionObj = nullptr;
				if (!SectionValue.IsValid() || !SectionValue->TryGetObject(SectionObj) || !SectionObj || !(*SectionObj).IsValid())
				{
					continue;
				}
				if ((*SectionObj)->GetStringField(TEXT("section_name")) == TEXT("Default"))
				{
					(*SectionObj)->SetStringField(TEXT("next_section_name"), AddedSectionName);
				}
			}

			TSharedPtr<FJsonObject> AddedSectionObj = MakeShared<FJsonObject>();
			AddedSectionObj->SetStringField(TEXT("section_name"), AddedSectionName);
			AddedSectionObj->SetNumberField(TEXT("time_seconds"), 0.4);
			AddedSectionObj->SetStringField(TEXT("next_section_name"), TEXT(""));
			const_cast<TArray<TSharedPtr<FJsonValue>>*>(Sections)->Add(MakeShared<FJsonValueObject>(AddedSectionObj));
		}

		const TArray<TSharedPtr<FJsonValue>>* NotifyTracks = nullptr;
		TestTrue(TEXT("Montage json notify tracks exists"), MontageJson->TryGetArrayField(TEXT("notify_tracks"), NotifyTracks) && NotifyTracks != nullptr);
		if (NotifyTracks)
		{
			TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
			TrackObj->SetStringField(TEXT("track_name"), NotifyTrackName);
			TSharedPtr<FJsonObject> TrackColor = MakeShared<FJsonObject>();
			TrackColor->SetNumberField(TEXT("r"), 0.25);
			TrackColor->SetNumberField(TEXT("g"), 0.5);
			TrackColor->SetNumberField(TEXT("b"), 0.75);
			TrackColor->SetNumberField(TEXT("a"), 1.0);
			TrackObj->SetObjectField(TEXT("track_color_linear"), TrackColor);
			const_cast<TArray<TSharedPtr<FJsonValue>>*>(NotifyTracks)->Add(MakeShared<FJsonValueObject>(TrackObj));
		}

		const TArray<TSharedPtr<FJsonValue>>* Notifies = nullptr;
		TestTrue(TEXT("Montage json notifies exists"), MontageJson->TryGetArrayField(TEXT("notifies"), Notifies) && Notifies != nullptr);
		if (Notifies)
		{
			TSharedPtr<FJsonObject> NotifyObj = MakeShared<FJsonObject>();
			NotifyObj->SetStringField(TEXT("kind"), TEXT("notify"));
			NotifyObj->SetStringField(TEXT("notify_name"), NotifyName);
			NotifyObj->SetStringField(TEXT("track_name"), NotifyTrackName);
			NotifyObj->SetNumberField(TEXT("time_seconds"), 0.2);
			NotifyObj->SetBoolField(TEXT("branching_point"), false);
			TSharedPtr<FJsonObject> NotifyColor = MakeShared<FJsonObject>();
			NotifyColor->SetNumberField(TEXT("r"), 0.8);
			NotifyColor->SetNumberField(TEXT("g"), 0.2);
			NotifyColor->SetNumberField(TEXT("b"), 0.1);
			NotifyColor->SetNumberField(TEXT("a"), 1.0);
			NotifyObj->SetObjectField(TEXT("notify_color_linear"), NotifyColor);
			const_cast<TArray<TSharedPtr<FJsonValue>>*>(Notifies)->Add(MakeShared<FJsonValueObject>(NotifyObj));
		}
	}
	TestTrue(TEXT("Montage json file saves"), SaveJsonFile(JsonPath, MontageJson));

	const FString ApplyBody = MakeExecRequestBody(
		TEXT("smoke-montage-json-apply-001"),
		TEXT("montage_apply_json"),
		FString::Printf(TEXT("{\"json_file\":\"%s\",\"save_after_apply\":false}"), *JsonPath.Replace(TEXT("\\"), TEXT("/"))));
	FHttpSmokeResult ApplyResult;
	TestTrue(TEXT("Montage json apply request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ApplyBody, ApplyResult));
	TestEqual(TEXT("Montage json apply status code"), ApplyResult.StatusCode, 200);

	const FString GetInfoBody = MakeExecRequestBody(
		TEXT("smoke-montage-json-info-001"),
		TEXT("montage_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *MontageAssetPath));
	FHttpSmokeResult GetInfoResult;
	TestTrue(TEXT("Montage json get info request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoBody, GetInfoResult));
	TestEqual(TEXT("Montage json get info status code"), GetInfoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetInfoJson;
	TestTrue(TEXT("Montage json get info parses JSON"), ParseJson(GetInfoResult.Body, GetInfoJson));
	const TSharedPtr<FJsonObject>* GetInfoData = nullptr;
	TestTrue(TEXT("Montage json get info contains data"), GetInfoJson.IsValid() && GetInfoJson->TryGetObjectField(TEXT("data"), GetInfoData) && GetInfoData && GetInfoData->IsValid());
	if (GetInfoData && GetInfoData->IsValid())
	{
		TestEqual(TEXT("Montage json auto blend out updated"), (*GetInfoData)->GetBoolField(TEXT("enable_auto_blend_out")), false);
		TestEqual(TEXT("Montage json sync group updated"), (*GetInfoData)->GetStringField(TEXT("sync_group_name")), SyncGroupName);
		TestEqual(TEXT("Montage json sync slot updated"), (*GetInfoData)->GetStringField(TEXT("sync_slot_name")), AddedSlotName);
		TestTrue(TEXT("Montage json slot track count >= 2"), static_cast<int32>((*GetInfoData)->GetIntegerField(TEXT("slot_track_count"))) >= 2);
		TestTrue(TEXT("Montage json section count >= 2"), static_cast<int32>((*GetInfoData)->GetIntegerField(TEXT("section_count"))) >= 2);

		const TArray<TSharedPtr<FJsonValue>>* SlotTracks = nullptr;
		TestTrue(TEXT("Montage json slot tracks array exists"), (*GetInfoData)->TryGetArrayField(TEXT("slot_tracks"), SlotTracks) && SlotTracks != nullptr);
		bool bFoundAddedSlot = false;
		if (SlotTracks)
		{
			for (const TSharedPtr<FJsonValue>& SlotTrackValue : *SlotTracks)
			{
				const TSharedPtr<FJsonObject>* SlotTrackObj = nullptr;
				if (!SlotTrackValue.IsValid() || !SlotTrackValue->TryGetObject(SlotTrackObj) || !SlotTrackObj || !(*SlotTrackObj).IsValid())
				{
					continue;
				}
				if ((*SlotTrackObj)->GetStringField(TEXT("slot_name")) == AddedSlotName)
				{
					bFoundAddedSlot = true;
				}
			}
		}
		TestTrue(TEXT("Montage json added slot exists"), bFoundAddedSlot);

		const TArray<TSharedPtr<FJsonValue>>* Notifies = nullptr;
		TestTrue(TEXT("Montage json notifies array exists"), (*GetInfoData)->TryGetArrayField(TEXT("notifies"), Notifies) && Notifies != nullptr);
		bool bFoundNotify = false;
		if (Notifies)
		{
			for (const TSharedPtr<FJsonValue>& NotifyValue : *Notifies)
			{
				const TSharedPtr<FJsonObject>* NotifyObj = nullptr;
				if (!NotifyValue.IsValid() || !NotifyValue->TryGetObject(NotifyObj) || !NotifyObj || !(*NotifyObj).IsValid())
				{
					continue;
				}
				if ((*NotifyObj)->GetStringField(TEXT("notify_name")) == NotifyName)
				{
					bFoundNotify = true;
				}
			}
		}
		TestTrue(TEXT("Montage json notify exists"), bFoundNotify);
	}

	const FString NoDefaultFirstSectionName = TEXT("Combo_NoDefault");
	const FString NoDefaultJsonPath = FPaths::Combine(JsonDir, TEXT("montage_no_default_roundtrip.json"));
	const FString RenameDefaultBody = MakeExecRequestBody(
		TEXT("smoke-montage-json-no-default-rename-001"),
		TEXT("montage_rename_section"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"section_name\":\"Default\",\"new_section_name\":\"%s\",\"save_after_rename\":false}"), *MontageAssetPath, *NoDefaultFirstSectionName));
	FHttpSmokeResult RenameDefaultResult;
	TestTrue(TEXT("Montage no-default section rename request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RenameDefaultBody, RenameDefaultResult));
	TestEqual(TEXT("Montage no-default section rename status code"), RenameDefaultResult.StatusCode, 200);

	const FString NoDefaultExportBody = MakeExecRequestBody(
		TEXT("smoke-montage-json-no-default-export-001"),
		TEXT("montage_export_json"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"output_file\":\"%s\"}"), *MontageAssetPath, *NoDefaultJsonPath.Replace(TEXT("\\"), TEXT("/"))));
	FHttpSmokeResult NoDefaultExportResult;
	TestTrue(TEXT("Montage no-default json export request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, NoDefaultExportBody, NoDefaultExportResult));
	TestEqual(TEXT("Montage no-default json export status code"), NoDefaultExportResult.StatusCode, 200);

	const FString NoDefaultApplyBody = MakeExecRequestBody(
		TEXT("smoke-montage-json-no-default-apply-001"),
		TEXT("montage_apply_json"),
		FString::Printf(TEXT("{\"json_file\":\"%s\",\"save_after_apply\":false}"), *NoDefaultJsonPath.Replace(TEXT("\\"), TEXT("/"))));
	FHttpSmokeResult NoDefaultApplyResult;
	TestTrue(TEXT("Montage no-default json apply request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, NoDefaultApplyBody, NoDefaultApplyResult));
	TestEqual(TEXT("Montage no-default json apply status code"), NoDefaultApplyResult.StatusCode, 200);

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceAnimBlueprintSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.AnimBlueprintWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceAnimBlueprintSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString SkeletonObjectPath = TEXT("/Engine/EngineMeshes/SkeletalCube_Skeleton.SkeletalCube_Skeleton");
	const FString SkeletalMeshObjectPath = TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube");
	const FString AnimBlueprintAssetPath = MakeAutomationAssetPath(TEXT("ABP"));
	const FString PreviewAnimBlueprintAssetPath = MakeAutomationAssetPath(TEXT("ABPPrev"));
	const FString TemplateAnimBlueprintAssetPath = MakeAutomationAssetPath(TEXT("ABPTemplate"));
	const FString PreviewTag = TEXT("SmokePreviewTag");
	const FString StructureSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString SmokeLayerName = FString::Printf(TEXT("SmokeLayer_%s"), *StructureSuffix);
	const FString SmokeStateMachineName = FString::Printf(TEXT("SmokeStateMachine_%s"), *StructureSuffix);

	auto FindAnimGraphEntry = [](const TSharedPtr<FJsonObject>* ListData, TSharedPtr<FJsonObject>& OutGraphObject) -> bool
	{
		if (!ListData || !ListData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
		if (!(*ListData)->TryGetArrayField(TEXT("graphs"), Graphs) || !Graphs)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& GraphValue : *Graphs)
		{
			const TSharedPtr<FJsonObject>* GraphObject = nullptr;
			if (!GraphValue.IsValid() || !GraphValue->TryGetObject(GraphObject) || !GraphObject || !GraphObject->IsValid())
			{
				continue;
			}

			if ((*GraphObject)->GetBoolField(TEXT("is_anim_graph")))
			{
				OutGraphObject = *GraphObject;
				return true;
			}
		}

		return false;
	};

	auto FindNamedStateMachineEntry = [](const TSharedPtr<FJsonObject>* ListData, const FString& ExpectedName, TSharedPtr<FJsonObject>& OutEntryObject) -> bool
	{
		if (!ListData || !ListData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!(*ListData)->TryGetArrayField(TEXT("state_machines"), Entries) || !Entries)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& EntryValue : *Entries)
		{
			const TSharedPtr<FJsonObject>* EntryObject = nullptr;
			if (!EntryValue.IsValid() || !EntryValue->TryGetObject(EntryObject) || !EntryObject || !EntryObject->IsValid())
			{
				continue;
			}

			if ((*EntryObject)->GetStringField(TEXT("state_machine_name")) == ExpectedName)
			{
				OutEntryObject = *EntryObject;
				return true;
			}
		}

		return false;
	};

	auto FindNamedAnimLayerEntry = [](const TSharedPtr<FJsonObject>* ListData, const FString& ExpectedName, TSharedPtr<FJsonObject>& OutEntryObject) -> bool
	{
		if (!ListData || !ListData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!(*ListData)->TryGetArrayField(TEXT("anim_layers"), Entries) || !Entries)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& EntryValue : *Entries)
		{
			const TSharedPtr<FJsonObject>* EntryObject = nullptr;
			if (!EntryValue.IsValid() || !EntryValue->TryGetObject(EntryObject) || !EntryObject || !EntryObject->IsValid())
			{
				continue;
			}

			if ((*EntryObject)->GetStringField(TEXT("layer_name")) == ExpectedName)
			{
				OutEntryObject = *EntryObject;
				return true;
			}
		}

		return false;
	};

	const FString CreateBody = MakeExecRequestBody(
		TEXT("smoke-animbp-create-001"),
		TEXT("anim_blueprint_create"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"parent_class\":\"/Script/Engine.AnimInstance\",\"target_skeleton\":\"%s\",\"preview_skeletal_mesh\":\"%s\",\"compile_after_create\":false,\"open_editor\":false,\"save_after_create\":false}"),
			*AnimBlueprintAssetPath,
			*SkeletonObjectPath,
			*SkeletalMeshObjectPath));
	FHttpSmokeResult CreateResult;
	TestTrue(TEXT("AnimBlueprint create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateBody, CreateResult));
	TestEqual(TEXT("AnimBlueprint create status code"), CreateResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CreateJson;
	TestTrue(TEXT("AnimBlueprint create response parses as JSON"), ParseJson(CreateResult.Body, CreateJson));
	TestTrue(TEXT("AnimBlueprint create root ok=true"), CreateJson.IsValid() && CreateJson->GetBoolField(TEXT("ok")));

	FString AnimBlueprintObjectPath;
	const TSharedPtr<FJsonObject>* CreateData = nullptr;
	TestTrue(TEXT("AnimBlueprint create contains data object"), CreateJson.IsValid() && CreateJson->TryGetObjectField(TEXT("data"), CreateData) && CreateData && CreateData->IsValid());
	if (CreateData && CreateData->IsValid())
	{
		AnimBlueprintObjectPath = (*CreateData)->GetStringField(TEXT("object_path"));
		TestEqual(TEXT("AnimBlueprint create asset path"), (*CreateData)->GetStringField(TEXT("asset_path")), AnimBlueprintAssetPath);
		TestFalse(TEXT("AnimBlueprint create object path non-empty"), AnimBlueprintObjectPath.IsEmpty());
		TestEqual(TEXT("AnimBlueprint create target skeleton"), (*CreateData)->GetStringField(TEXT("target_skeleton")), SkeletonObjectPath);
		TestEqual(TEXT("AnimBlueprint create preview mesh"), (*CreateData)->GetStringField(TEXT("preview_skeletal_mesh")), SkeletalMeshObjectPath);
		TestFalse(TEXT("AnimBlueprint create not template"), (*CreateData)->GetBoolField(TEXT("is_template")));
	}

	const FString CreatePreviewBody = MakeExecRequestBody(
		TEXT("smoke-animbp-create-002"),
		TEXT("anim_blueprint_create"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"target_skeleton\":\"%s\",\"compile_after_create\":false,\"open_editor\":false,\"save_after_create\":false}"),
			*PreviewAnimBlueprintAssetPath,
			*SkeletonObjectPath));
	FHttpSmokeResult CreatePreviewResult;
	TestTrue(TEXT("AnimBlueprint preview create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreatePreviewBody, CreatePreviewResult));
	TestEqual(TEXT("AnimBlueprint preview create status code"), CreatePreviewResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CreatePreviewJson;
	TestTrue(TEXT("AnimBlueprint preview create response parses as JSON"), ParseJson(CreatePreviewResult.Body, CreatePreviewJson));
	TestTrue(TEXT("AnimBlueprint preview create root ok=true"), CreatePreviewJson.IsValid() && CreatePreviewJson->GetBoolField(TEXT("ok")));

	FString PreviewAnimBlueprintObjectPath;
	FString PreviewGeneratedClassPath;
	const TSharedPtr<FJsonObject>* CreatePreviewData = nullptr;
	TestTrue(TEXT("AnimBlueprint preview create contains data object"), CreatePreviewJson.IsValid() && CreatePreviewJson->TryGetObjectField(TEXT("data"), CreatePreviewData) && CreatePreviewData && CreatePreviewData->IsValid());
	if (CreatePreviewData && CreatePreviewData->IsValid())
	{
		PreviewAnimBlueprintObjectPath = (*CreatePreviewData)->GetStringField(TEXT("object_path"));
		PreviewGeneratedClassPath = (*CreatePreviewData)->GetStringField(TEXT("generated_class"));
		TestFalse(TEXT("AnimBlueprint preview object path non-empty"), PreviewAnimBlueprintObjectPath.IsEmpty());
		TestFalse(TEXT("AnimBlueprint preview generated class non-empty"), PreviewGeneratedClassPath.IsEmpty());
	}

	const FString CreateTemplateBody = MakeExecRequestBody(
		TEXT("smoke-animbp-create-003"),
		TEXT("anim_blueprint_create"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"template\":true,\"compile_after_create\":false,\"open_editor\":false,\"save_after_create\":false}"),
			*TemplateAnimBlueprintAssetPath));
	FHttpSmokeResult CreateTemplateResult;
	TestTrue(TEXT("AnimBlueprint template create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateTemplateBody, CreateTemplateResult));
	TestEqual(TEXT("AnimBlueprint template create status code"), CreateTemplateResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CreateTemplateJson;
	TestTrue(TEXT("AnimBlueprint template create response parses as JSON"), ParseJson(CreateTemplateResult.Body, CreateTemplateJson));
	TestTrue(TEXT("AnimBlueprint template create root ok=true"), CreateTemplateJson.IsValid() && CreateTemplateJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* CreateTemplateData = nullptr;
	TestTrue(TEXT("AnimBlueprint template create contains data object"), CreateTemplateJson.IsValid() && CreateTemplateJson->TryGetObjectField(TEXT("data"), CreateTemplateData) && CreateTemplateData && CreateTemplateData->IsValid());
	if (CreateTemplateData && CreateTemplateData->IsValid())
	{
		TestTrue(TEXT("AnimBlueprint template create is template"), (*CreateTemplateData)->GetBoolField(TEXT("is_template")));
		TestEqual(TEXT("AnimBlueprint template target skeleton empty"), (*CreateTemplateData)->GetStringField(TEXT("target_skeleton")), FString(TEXT("")));
	}

	const FString GetInfoBody = MakeExecRequestBody(
		TEXT("smoke-animbp-info-001"),
		TEXT("anim_blueprint_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult GetInfoResult;
	TestTrue(TEXT("AnimBlueprint get_info request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoBody, GetInfoResult));
	TestEqual(TEXT("AnimBlueprint get_info status code"), GetInfoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetInfoJson;
	TestTrue(TEXT("AnimBlueprint get_info response parses as JSON"), ParseJson(GetInfoResult.Body, GetInfoJson));
	TestTrue(TEXT("AnimBlueprint get_info root ok=true"), GetInfoJson.IsValid() && GetInfoJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* GetInfoData = nullptr;
	TestTrue(TEXT("AnimBlueprint get_info contains data object"), GetInfoJson.IsValid() && GetInfoJson->TryGetObjectField(TEXT("data"), GetInfoData) && GetInfoData && GetInfoData->IsValid());
	if (GetInfoData && GetInfoData->IsValid())
	{
		TestEqual(TEXT("AnimBlueprint get_info target skeleton"), (*GetInfoData)->GetStringField(TEXT("target_skeleton")), SkeletonObjectPath);
		TestEqual(TEXT("AnimBlueprint get_info preview mesh"), (*GetInfoData)->GetStringField(TEXT("preview_skeletal_mesh")), SkeletalMeshObjectPath);
		TestTrue(TEXT("AnimBlueprint get_info generated class non-empty"), !(*GetInfoData)->GetStringField(TEXT("generated_class")).IsEmpty());
		TestTrue(TEXT("AnimBlueprint get_info supports anim layers"), (*GetInfoData)->GetBoolField(TEXT("supports_anim_layers")));
	}

	const FString ListGraphsBody = MakeExecRequestBody(
		TEXT("smoke-animbp-list-001"),
		TEXT("anim_blueprint_list_graphs"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult ListGraphsResult;
	TestTrue(TEXT("AnimBlueprint list_graphs request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ListGraphsBody, ListGraphsResult));
	TestEqual(TEXT("AnimBlueprint list_graphs status code"), ListGraphsResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ListGraphsJson;
	TestTrue(TEXT("AnimBlueprint list_graphs response parses as JSON"), ParseJson(ListGraphsResult.Body, ListGraphsJson));
	TestTrue(TEXT("AnimBlueprint list_graphs root ok=true"), ListGraphsJson.IsValid() && ListGraphsJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* ListGraphsData = nullptr;
	TestTrue(TEXT("AnimBlueprint list_graphs contains data object"), ListGraphsJson.IsValid() && ListGraphsJson->TryGetObjectField(TEXT("data"), ListGraphsData) && ListGraphsData && ListGraphsData->IsValid());
	if (ListGraphsData && ListGraphsData->IsValid())
	{
		TestTrue(TEXT("AnimBlueprint list_graphs graph_count>=1"), static_cast<int32>((*ListGraphsData)->GetIntegerField(TEXT("graph_count"))) >= 1);
		TestTrue(TEXT("AnimBlueprint list_graphs anim_graph_count>=1"), static_cast<int32>((*ListGraphsData)->GetIntegerField(TEXT("anim_graph_count"))) >= 1);

		TSharedPtr<FJsonObject> AnimGraphObject;
		TestTrue(TEXT("AnimBlueprint list_graphs contains anim graph entry"), FindAnimGraphEntry(ListGraphsData, AnimGraphObject));
		if (AnimGraphObject.IsValid())
		{
			TestEqual(TEXT("AnimBlueprint list_graphs anim graph kind"), AnimGraphObject->GetStringField(TEXT("graph_kind")), FString(TEXT("anim")));
		}
	}

	const FString AddGenericAnimNodeBody = MakeExecRequestBody(
		TEXT("smoke-animbp-generic-node-001"),
		TEXT("anim_blueprint_add_node_by_class"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_name\":\"AnimGraph\",\"node_class\":\"/Script/AnimGraph.AnimGraphNode_IdentityPose\",\"pos_x\":240,\"pos_y\":128,\"compile_after_add\":false,\"save_after_add\":false}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult AddGenericAnimNodeResult;
	TestTrue(TEXT("AnimBlueprint add_node_by_class request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddGenericAnimNodeBody, AddGenericAnimNodeResult));
	TestEqual(TEXT("AnimBlueprint add_node_by_class status code"), AddGenericAnimNodeResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddGenericAnimNodeJson;
	TestTrue(TEXT("AnimBlueprint add_node_by_class parses JSON"), ParseJson(AddGenericAnimNodeResult.Body, AddGenericAnimNodeJson));
	TestTrue(TEXT("AnimBlueprint add_node_by_class root ok=true"), AddGenericAnimNodeJson.IsValid() && AddGenericAnimNodeJson->GetBoolField(TEXT("ok")));

	FString GenericAnimNodeGuid;
	const TSharedPtr<FJsonObject>* AddGenericAnimNodeData = nullptr;
	TestTrue(TEXT("AnimBlueprint add_node_by_class contains data object"), AddGenericAnimNodeJson.IsValid() && AddGenericAnimNodeJson->TryGetObjectField(TEXT("data"), AddGenericAnimNodeData) && AddGenericAnimNodeData && AddGenericAnimNodeData->IsValid());
	if (AddGenericAnimNodeData && AddGenericAnimNodeData->IsValid())
	{
		GenericAnimNodeGuid = (*AddGenericAnimNodeData)->GetStringField(TEXT("node_guid"));
		TestTrue(TEXT("AnimBlueprint generic node guid non-empty"), !GenericAnimNodeGuid.IsEmpty());
		TestEqual(TEXT("AnimBlueprint generic node graph name"), (*AddGenericAnimNodeData)->GetStringField(TEXT("graph_name")), FString(TEXT("AnimGraph")));
		TestTrue(TEXT("AnimBlueprint generic node graph path non-empty"), !(*AddGenericAnimNodeData)->GetStringField(TEXT("graph_path")).IsEmpty());
	}

	const FString LayoutAnimGraphBody = MakeExecRequestBody(
		TEXT("smoke-animbp-layout-anim-001"),
		TEXT("anim_blueprint_layout_graph"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_name\":\"AnimGraph\",\"horizontal_spacing\":360,\"vertical_spacing\":96,\"node_padding\":16,\"compile_after_layout\":false,\"save_after_layout\":false}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult LayoutAnimGraphResult;
	TestTrue(TEXT("AnimBlueprint layout_graph request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, LayoutAnimGraphBody, LayoutAnimGraphResult));
	TestEqual(TEXT("AnimBlueprint layout_graph status code"), LayoutAnimGraphResult.StatusCode, 200);

	TSharedPtr<FJsonObject> LayoutAnimGraphJson;
	TestTrue(TEXT("AnimBlueprint layout_graph parses JSON"), ParseJson(LayoutAnimGraphResult.Body, LayoutAnimGraphJson));
	TestTrue(TEXT("AnimBlueprint layout_graph root ok=true"), LayoutAnimGraphJson.IsValid() && LayoutAnimGraphJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* LayoutAnimGraphData = nullptr;
	TestTrue(TEXT("AnimBlueprint layout_graph contains data object"), LayoutAnimGraphJson.IsValid() && LayoutAnimGraphJson->TryGetObjectField(TEXT("data"), LayoutAnimGraphData) && LayoutAnimGraphData && LayoutAnimGraphData->IsValid());
	if (LayoutAnimGraphData && LayoutAnimGraphData->IsValid())
	{
		TestTrue(TEXT("AnimBlueprint layout_graph covers anim graph nodes"), static_cast<int32>((*LayoutAnimGraphData)->GetIntegerField(TEXT("node_count"))) >= 2);
		TestEqual(TEXT("AnimBlueprint layout_graph removes estimated node overlaps"), static_cast<int32>((*LayoutAnimGraphData)->GetIntegerField(TEXT("overlap_count_after"))), 0);
		TestTrue(TEXT("AnimBlueprint layout_graph returns node list"), (*LayoutAnimGraphData)->HasField(TEXT("nodes")));
	}

	const FString InspectAnimGraphBody = MakeExecRequestBody(
		TEXT("smoke-animbp-inspect-anim-001"),
		TEXT("anim_blueprint_inspect_nodes"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_name\":\"AnimGraph\",\"include_pins\":true,\"limit_per_graph\":64}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult InspectAnimGraphResult;
	TestTrue(TEXT("AnimBlueprint inspect_nodes on anim graph request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, InspectAnimGraphBody, InspectAnimGraphResult));
	TestEqual(TEXT("AnimBlueprint inspect_nodes on anim graph status code"), InspectAnimGraphResult.StatusCode, 200);

	TSharedPtr<FJsonObject> InspectAnimGraphJson;
	TestTrue(TEXT("AnimBlueprint inspect_nodes on anim graph parses JSON"), ParseJson(InspectAnimGraphResult.Body, InspectAnimGraphJson));
	TestTrue(TEXT("AnimBlueprint inspect_nodes on anim graph root ok=true"), InspectAnimGraphJson.IsValid() && InspectAnimGraphJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* InspectAnimGraphData = nullptr;
	TestTrue(TEXT("AnimBlueprint inspect_nodes on anim graph contains data object"), InspectAnimGraphJson.IsValid() && InspectAnimGraphJson->TryGetObjectField(TEXT("data"), InspectAnimGraphData) && InspectAnimGraphData && InspectAnimGraphData->IsValid());

	auto FindAnimGraphNodeInInspect = [](const TSharedPtr<FJsonObject>* InspectData, const FString& NodeGuid, TSharedPtr<FJsonObject>& OutNodeObject) -> bool
	{
		if (!InspectData || !InspectData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
		if (!(*InspectData)->TryGetArrayField(TEXT("graphs"), Graphs) || !Graphs)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& GraphValue : *Graphs)
		{
			const TSharedPtr<FJsonObject>* GraphObject = nullptr;
			if (!GraphValue.IsValid() || !GraphValue->TryGetObject(GraphObject) || !GraphObject || !GraphObject->IsValid())
			{
				continue;
			}

			const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
			if (!(*GraphObject)->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
			{
				continue;
			}

			for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
			{
				const TSharedPtr<FJsonObject>* NodeObject = nullptr;
				if (!NodeValue.IsValid() || !NodeValue->TryGetObject(NodeObject) || !NodeObject || !NodeObject->IsValid())
				{
					continue;
				}

				if ((*NodeObject)->GetStringField(TEXT("node_guid")) == NodeGuid)
				{
					OutNodeObject = *NodeObject;
					return true;
				}
			}
		}

		return false;
	};

	TSharedPtr<FJsonObject> GenericAnimNodeObject;
	TestTrue(TEXT("AnimBlueprint inspect_nodes finds generic anim node"), FindAnimGraphNodeInInspect(InspectAnimGraphData, GenericAnimNodeGuid, GenericAnimNodeObject));
	if (GenericAnimNodeObject.IsValid())
	{
		TestTrue(TEXT("AnimBlueprint generic node class contains identity pose"), GenericAnimNodeObject->GetStringField(TEXT("class")).Contains(TEXT("AnimGraphNode_IdentityPose")));
	}

	UAnimBlueprint* EditableAnimBlueprint = AnimBlueprintObjectPath.IsEmpty() ? nullptr : LoadObject<UAnimBlueprint>(nullptr, *AnimBlueprintObjectPath);
	TestNotNull(TEXT("AnimBlueprint smoke local asset loads"), EditableAnimBlueprint);
	if (EditableAnimBlueprint)
	{
		UEdGraph* DefaultAnimGraph = nullptr;
		for (UEdGraph* Graph : EditableAnimBlueprint->FunctionGraphs)
		{
			if (Graph && AnimationEditorUtils::IsAnimGraph(Graph) && Graph->GetFName() == UEdGraphSchema_K2::GN_AnimGraph)
			{
				DefaultAnimGraph = Graph;
				break;
			}
		}

		TestNotNull(TEXT("AnimBlueprint smoke default anim graph exists"), DefaultAnimGraph);

		UEdGraph* LayerGraph = FBlueprintEditorUtils::CreateNewGraph(EditableAnimBlueprint, FName(*SmokeLayerName), UAnimationGraph::StaticClass(), UAnimationGraphSchema::StaticClass());
		TestNotNull(TEXT("AnimBlueprint smoke layer graph created"), LayerGraph);
		if (LayerGraph)
		{
			FBlueprintEditorUtils::AddDomainSpecificGraph(EditableAnimBlueprint, LayerGraph);
		}

		UAnimGraphNode_StateMachine* StateMachineNode = DefaultAnimGraph ? AddAutomationNodeToGraph<UAnimGraphNode_StateMachine>(DefaultAnimGraph) : nullptr;
		TestNotNull(TEXT("AnimBlueprint smoke state machine node created"), StateMachineNode);

		UAnimationStateMachineGraph* StateMachineGraph = StateMachineNode ? StateMachineNode->EditorStateMachineGraph : nullptr;
		TestNotNull(TEXT("AnimBlueprint smoke state machine graph created"), StateMachineGraph);
		if (StateMachineGraph)
		{
			FBlueprintEditorUtils::RenameGraph(StateMachineGraph, SmokeStateMachineName);
		}

		UAnimStateNode* StateNode = StateMachineGraph ? AddAutomationNodeToGraph<UAnimStateNode>(StateMachineGraph) : nullptr;
		TestNotNull(TEXT("AnimBlueprint smoke state node created"), StateNode);
		if (StateMachineGraph && StateMachineGraph->EntryNode && StateNode && StateMachineGraph->EntryNode->Pins.Num() > 0)
		{
			StateNode->AutowireNewNode(StateMachineGraph->EntryNode->Pins[0]);
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(EditableAnimBlueprint);
		FKismetEditorUtilities::CompileBlueprint(EditableAnimBlueprint);
		TestTrue(TEXT("AnimBlueprint smoke structure compile has no error"), EditableAnimBlueprint->Status != BS_Error);
	}

	const FString ListStateMachinesBody = MakeExecRequestBody(
		TEXT("smoke-animbp-state-machines-001"),
		TEXT("anim_blueprint_list_state_machines"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult ListStateMachinesResult;
	TestTrue(TEXT("AnimBlueprint list_state_machines request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ListStateMachinesBody, ListStateMachinesResult));
	TestEqual(TEXT("AnimBlueprint list_state_machines status code"), ListStateMachinesResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ListStateMachinesJson;
	TestTrue(TEXT("AnimBlueprint list_state_machines parses JSON"), ParseJson(ListStateMachinesResult.Body, ListStateMachinesJson));
	TestTrue(TEXT("AnimBlueprint list_state_machines root ok=true"), ListStateMachinesJson.IsValid() && ListStateMachinesJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* ListStateMachinesData = nullptr;
	TestTrue(TEXT("AnimBlueprint list_state_machines contains data object"), ListStateMachinesJson.IsValid() && ListStateMachinesJson->TryGetObjectField(TEXT("data"), ListStateMachinesData) && ListStateMachinesData && ListStateMachinesData->IsValid());
	if (ListStateMachinesData && ListStateMachinesData->IsValid())
	{
		TestTrue(TEXT("AnimBlueprint list_state_machines count>=1"), static_cast<int32>((*ListStateMachinesData)->GetIntegerField(TEXT("state_machine_count"))) >= 1);
		TestTrue(TEXT("AnimBlueprint list_state_machines compiled_count field exists"), (*ListStateMachinesData)->HasField(TEXT("compiled_state_machine_count")));

		TSharedPtr<FJsonObject> StateMachineEntry;
		TestTrue(TEXT("AnimBlueprint list_state_machines contains created state machine"), FindNamedStateMachineEntry(ListStateMachinesData, SmokeStateMachineName, StateMachineEntry));
		if (StateMachineEntry.IsValid())
		{
			TestEqual(TEXT("AnimBlueprint list_state_machines owner anim graph"), StateMachineEntry->GetStringField(TEXT("owner_anim_graph_name")), FString(TEXT("AnimGraph")));
			TestTrue(TEXT("AnimBlueprint list_state_machines state_count>=1"), static_cast<int32>(StateMachineEntry->GetIntegerField(TEXT("state_count"))) >= 1);
			TestTrue(TEXT("AnimBlueprint list_state_machines has entry node"), StateMachineEntry->GetBoolField(TEXT("has_entry_node")));
			TestTrue(TEXT("AnimBlueprint list_state_machines compiled flag exists"), StateMachineEntry->HasField(TEXT("compiled_machine_found")));
		}
	}

	const FString ListAnimLayersBody = MakeExecRequestBody(
		TEXT("smoke-animbp-layers-001"),
		TEXT("anim_blueprint_list_anim_layers"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult ListAnimLayersResult;
	TestTrue(TEXT("AnimBlueprint list_anim_layers request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ListAnimLayersBody, ListAnimLayersResult));
	TestEqual(TEXT("AnimBlueprint list_anim_layers status code"), ListAnimLayersResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ListAnimLayersJson;
	TestTrue(TEXT("AnimBlueprint list_anim_layers parses JSON"), ParseJson(ListAnimLayersResult.Body, ListAnimLayersJson));
	TestTrue(TEXT("AnimBlueprint list_anim_layers root ok=true"), ListAnimLayersJson.IsValid() && ListAnimLayersJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* ListAnimLayersData = nullptr;
	TestTrue(TEXT("AnimBlueprint list_anim_layers contains data object"), ListAnimLayersJson.IsValid() && ListAnimLayersJson->TryGetObjectField(TEXT("data"), ListAnimLayersData) && ListAnimLayersData && ListAnimLayersData->IsValid());
	if (ListAnimLayersData && ListAnimLayersData->IsValid())
	{
		TestTrue(TEXT("AnimBlueprint list_anim_layers count>=1"), static_cast<int32>((*ListAnimLayersData)->GetIntegerField(TEXT("layer_count"))) >= 1);
		TestTrue(TEXT("AnimBlueprint list_anim_layers graph_count>=1"), static_cast<int32>((*ListAnimLayersData)->GetIntegerField(TEXT("layer_graph_count"))) >= 1);
		TestTrue(TEXT("AnimBlueprint list_anim_layers compiled_count>=1"), static_cast<int32>((*ListAnimLayersData)->GetIntegerField(TEXT("compiled_layer_function_count"))) >= 1);

		TSharedPtr<FJsonObject> LayerEntry;
		TestTrue(TEXT("AnimBlueprint list_anim_layers contains created layer"), FindNamedAnimLayerEntry(ListAnimLayersData, SmokeLayerName, LayerEntry));
		if (LayerEntry.IsValid())
		{
			TestTrue(TEXT("AnimBlueprint list_anim_layers has graph"), LayerEntry->GetBoolField(TEXT("has_graph")));
			TestTrue(TEXT("AnimBlueprint list_anim_layers compiled function found"), LayerEntry->GetBoolField(TEXT("compiled_function_found")));
			TestTrue(TEXT("AnimBlueprint list_anim_layers implemented field exists"), LayerEntry->HasField(TEXT("is_implemented")));
		}
	}

	const FString ClearPreviewMeshBody = MakeExecRequestBody(
		TEXT("smoke-animbp-clear-preview-mesh-001"),
		TEXT("anim_blueprint_set_preview_mesh"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"clear_preview_mesh\":true,\"save_after_set\":false}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult ClearPreviewMeshResult;
	TestTrue(TEXT("AnimBlueprint clear preview mesh request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ClearPreviewMeshBody, ClearPreviewMeshResult));
	TestEqual(TEXT("AnimBlueprint clear preview mesh status code"), ClearPreviewMeshResult.StatusCode, 200);

	const FString SetPreviewMeshBody = MakeExecRequestBody(
		TEXT("smoke-animbp-set-preview-mesh-001"),
		TEXT("anim_blueprint_set_preview_mesh"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"skeletal_mesh_path\":\"%s\",\"save_after_set\":false}"), *AnimBlueprintAssetPath, *SkeletalMeshObjectPath));
	FHttpSmokeResult SetPreviewMeshResult;
	TestTrue(TEXT("AnimBlueprint set preview mesh request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetPreviewMeshBody, SetPreviewMeshResult));
	TestEqual(TEXT("AnimBlueprint set preview mesh status code"), SetPreviewMeshResult.StatusCode, 200);

	const FString SetPreviewAnimBlueprintBody = MakeExecRequestBody(
		TEXT("smoke-animbp-set-preview-abp-001"),
		TEXT("anim_blueprint_set_preview_animation_blueprint"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"preview_anim_blueprint_path\":\"%s\",\"preview_application_method\":\"linked_anim_graph\",\"preview_animation_blueprint_tag\":\"%s\",\"save_after_set\":false}"), *AnimBlueprintAssetPath, *PreviewAnimBlueprintAssetPath, *PreviewTag));
	FHttpSmokeResult SetPreviewAnimBlueprintResult;
	TestTrue(TEXT("AnimBlueprint set preview anim blueprint request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetPreviewAnimBlueprintBody, SetPreviewAnimBlueprintResult));
	TestEqual(TEXT("AnimBlueprint set preview anim blueprint status code"), SetPreviewAnimBlueprintResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SetPreviewAnimBlueprintJson;
	TestTrue(TEXT("AnimBlueprint set preview anim blueprint response parses as JSON"), ParseJson(SetPreviewAnimBlueprintResult.Body, SetPreviewAnimBlueprintJson));
	TestTrue(TEXT("AnimBlueprint set preview anim blueprint root ok=true"), SetPreviewAnimBlueprintJson.IsValid() && SetPreviewAnimBlueprintJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* SetPreviewAnimBlueprintData = nullptr;
	TestTrue(TEXT("AnimBlueprint set preview anim blueprint contains data object"), SetPreviewAnimBlueprintJson.IsValid() && SetPreviewAnimBlueprintJson->TryGetObjectField(TEXT("data"), SetPreviewAnimBlueprintData) && SetPreviewAnimBlueprintData && SetPreviewAnimBlueprintData->IsValid());
	if (SetPreviewAnimBlueprintData && SetPreviewAnimBlueprintData->IsValid())
	{
		TestEqual(TEXT("AnimBlueprint set preview anim blueprint object path"), (*SetPreviewAnimBlueprintData)->GetStringField(TEXT("preview_animation_blueprint")), PreviewAnimBlueprintObjectPath);
		TestEqual(TEXT("AnimBlueprint set preview anim blueprint method"), (*SetPreviewAnimBlueprintData)->GetStringField(TEXT("preview_application_method")), FString(TEXT("linked_anim_graph")));
		TestEqual(TEXT("AnimBlueprint set preview anim blueprint tag"), (*SetPreviewAnimBlueprintData)->GetStringField(TEXT("preview_animation_blueprint_tag")), PreviewTag);
	}

	const FString GetInfoAfterPreviewBody = MakeExecRequestBody(
		TEXT("smoke-animbp-info-002"),
		TEXT("anim_blueprint_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult GetInfoAfterPreviewResult;
	TestTrue(TEXT("AnimBlueprint get_info after preview request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoAfterPreviewBody, GetInfoAfterPreviewResult));
	TestEqual(TEXT("AnimBlueprint get_info after preview status code"), GetInfoAfterPreviewResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetInfoAfterPreviewJson;
	TestTrue(TEXT("AnimBlueprint get_info after preview response parses as JSON"), ParseJson(GetInfoAfterPreviewResult.Body, GetInfoAfterPreviewJson));
	TestTrue(TEXT("AnimBlueprint get_info after preview root ok=true"), GetInfoAfterPreviewJson.IsValid() && GetInfoAfterPreviewJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* GetInfoAfterPreviewData = nullptr;
	TestTrue(TEXT("AnimBlueprint get_info after preview contains data object"), GetInfoAfterPreviewJson.IsValid() && GetInfoAfterPreviewJson->TryGetObjectField(TEXT("data"), GetInfoAfterPreviewData) && GetInfoAfterPreviewData && GetInfoAfterPreviewData->IsValid());
	if (GetInfoAfterPreviewData && GetInfoAfterPreviewData->IsValid())
	{
		TestEqual(TEXT("AnimBlueprint get_info after preview mesh restored"), (*GetInfoAfterPreviewData)->GetStringField(TEXT("preview_skeletal_mesh")), SkeletalMeshObjectPath);
		TestEqual(TEXT("AnimBlueprint get_info after preview anim blueprint"), (*GetInfoAfterPreviewData)->GetStringField(TEXT("preview_animation_blueprint")), PreviewAnimBlueprintObjectPath);
		TestEqual(TEXT("AnimBlueprint get_info after preview method"), (*GetInfoAfterPreviewData)->GetStringField(TEXT("preview_application_method")), FString(TEXT("linked_anim_graph")));
		TestEqual(TEXT("AnimBlueprint get_info after preview tag"), (*GetInfoAfterPreviewData)->GetStringField(TEXT("preview_animation_blueprint_tag")), PreviewTag);
	}

	const FString ReparentBody = MakeExecRequestBody(
		TEXT("smoke-animbp-reparent-001"),
		TEXT("anim_blueprint_reparent"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"parent_class\":\"%s\",\"compile_after_reparent\":false,\"save_after_reparent\":false}"), *AnimBlueprintAssetPath, *PreviewGeneratedClassPath));
	FHttpSmokeResult ReparentResult;
	TestTrue(TEXT("AnimBlueprint reparent request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ReparentBody, ReparentResult));
	TestEqual(TEXT("AnimBlueprint reparent status code"), ReparentResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ReparentJson;
	TestTrue(TEXT("AnimBlueprint reparent response parses as JSON"), ParseJson(ReparentResult.Body, ReparentJson));
	TestTrue(TEXT("AnimBlueprint reparent root ok=true"), ReparentJson.IsValid() && ReparentJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* ReparentData = nullptr;
	TestTrue(TEXT("AnimBlueprint reparent contains data object"), ReparentJson.IsValid() && ReparentJson->TryGetObjectField(TEXT("data"), ReparentData) && ReparentData && ReparentData->IsValid());
	if (ReparentData && ReparentData->IsValid())
	{
		TestEqual(TEXT("AnimBlueprint reparent parent class"), (*ReparentData)->GetStringField(TEXT("parent_class")), PreviewGeneratedClassPath);
	}

	const FString CompileBody = MakeExecRequestBody(
		TEXT("smoke-animbp-compile-001"),
		TEXT("anim_blueprint_compile"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"include_messages\":true,\"severity_filter\":\"all\",\"max_messages\":16,\"save_after_compile\":false}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult CompileResult;
	TestTrue(TEXT("AnimBlueprint compile request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CompileBody, CompileResult));
	TestEqual(TEXT("AnimBlueprint compile status code"), CompileResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CompileJson;
	TestTrue(TEXT("AnimBlueprint compile response parses as JSON"), ParseJson(CompileResult.Body, CompileJson));
	TestTrue(TEXT("AnimBlueprint compile root ok=true"), CompileJson.IsValid() && CompileJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* CompileData = nullptr;
	TestTrue(TEXT("AnimBlueprint compile contains data object"), CompileJson.IsValid() && CompileJson->TryGetObjectField(TEXT("data"), CompileData) && CompileData && CompileData->IsValid());
	if (CompileData && CompileData->IsValid())
	{
		TestFalse(TEXT("AnimBlueprint compile reports no error"), (*CompileData)->GetBoolField(TEXT("has_error")));
		TestTrue(TEXT("AnimBlueprint compile error count is zero"), static_cast<int32>((*CompileData)->GetIntegerField(TEXT("error_count"))) == 0);
	}

	const FString CompileLogBody = MakeExecRequestBody(
		TEXT("smoke-animbp-compile-log-001"),
		TEXT("anim_blueprint_get_compile_log"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"severity_filter\":\"all\",\"max_messages\":16,\"save_after_compile\":false}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult CompileLogResult;
	TestTrue(TEXT("AnimBlueprint get_compile_log request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CompileLogBody, CompileLogResult));
	TestEqual(TEXT("AnimBlueprint get_compile_log status code"), CompileLogResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CompileLogJson;
	TestTrue(TEXT("AnimBlueprint get_compile_log response parses as JSON"), ParseJson(CompileLogResult.Body, CompileLogJson));
	TestTrue(TEXT("AnimBlueprint get_compile_log root ok=true"), CompileLogJson.IsValid() && CompileLogJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* CompileLogData = nullptr;
	TestTrue(TEXT("AnimBlueprint get_compile_log contains data object"), CompileLogJson.IsValid() && CompileLogJson->TryGetObjectField(TEXT("data"), CompileLogData) && CompileLogData && CompileLogData->IsValid());
	if (CompileLogData && CompileLogData->IsValid())
	{
		TestFalse(TEXT("AnimBlueprint get_compile_log reports no error"), (*CompileLogData)->GetBoolField(TEXT("has_error")));
		TestTrue(TEXT("AnimBlueprint get_compile_log error count is zero"), static_cast<int32>((*CompileLogData)->GetIntegerField(TEXT("error_count"))) == 0);
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceAnimBlueprintCommandsSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.AnimBlueprintCommandsWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceAnimBlueprintCommandsSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString SkeletonObjectPath = TEXT("/Engine/EngineMeshes/SkeletalCube_Skeleton.SkeletalCube_Skeleton");
	const FString SkeletalMeshObjectPath = TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube");
	const FString AnimBlueprintAssetPath = MakeAutomationAssetPath(TEXT("ABPFull"));
	const FString LayerInterfaceAssetPath = MakeAutomationAssetPath(TEXT("ABPInterface"));
	const FString VariableName = TEXT("SmokeFlag");
	const FString LogicFunctionName = TEXT("SmokeLogicFunction");
	const FString LocalLayerName = TEXT("SmokeLocalLayer");
	const FString RenamedLocalLayerName = TEXT("SmokeLocalLayerRenamed");
	const FString InterfaceLayerName = TEXT("SmokeInterfaceLayer");
	const FString StateMachineName = TEXT("SmokeMachine");
	const FString RenamedStateMachineName = TEXT("SmokeMachineRenamed");
	const FString IdleStateName = TEXT("Idle");
	const FString RunStateName = TEXT("Run");
	const FString RenamedRunStateName = TEXT("RunFast");
	const FString ConduitName = TEXT("Gate");
	const FString AliasName = TEXT("IdleAlias");
	const FString CustomEventName = TEXT("SmokeAnimEvent");
	const FString StringValue = TEXT("SmokeAnimMessage");

	auto FindNodeInInspect = [](const TSharedPtr<FJsonObject>* InspectData, const FString& NodeGuid, TSharedPtr<FJsonObject>& OutNodeObject) -> bool
	{
		if (!InspectData || !InspectData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
		if (!(*InspectData)->TryGetArrayField(TEXT("graphs"), Graphs) || !Graphs)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& GraphValue : *Graphs)
		{
			const TSharedPtr<FJsonObject>* GraphObject = nullptr;
			if (!GraphValue.IsValid() || !GraphValue->TryGetObject(GraphObject) || !GraphObject || !GraphObject->IsValid())
			{
				continue;
			}

			const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
			if (!(*GraphObject)->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
			{
				continue;
			}

			for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
			{
				const TSharedPtr<FJsonObject>* NodeObject = nullptr;
				if (!NodeValue.IsValid() || !NodeValue->TryGetObject(NodeObject) || !NodeObject || !NodeObject->IsValid())
				{
					continue;
				}

				if ((*NodeObject)->GetStringField(TEXT("node_guid")) == NodeGuid)
				{
					OutNodeObject = *NodeObject;
					return true;
				}
			}
		}

		return false;
	};

	auto FindPinName = [](const TSharedPtr<FJsonObject>& NodeObject, const FString& Direction, const FString& Category, FString& OutPinName) -> bool
	{
		if (!NodeObject.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
		if (!NodeObject->TryGetArrayField(TEXT("pins"), Pins) || !Pins)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& PinValue : *Pins)
		{
			const TSharedPtr<FJsonObject>* PinObject = nullptr;
			if (!PinValue.IsValid() || !PinValue->TryGetObject(PinObject) || !PinObject || !PinObject->IsValid())
			{
				continue;
			}

			if ((*PinObject)->GetStringField(TEXT("direction")) == Direction &&
				(*PinObject)->GetStringField(TEXT("category")).Equals(Category, ESearchCase::IgnoreCase))
			{
				OutPinName = (*PinObject)->GetStringField(TEXT("name"));
				return true;
			}
		}

		return false;
	};

	auto FindNodeByClassContains = [](const TSharedPtr<FJsonObject>* InspectData, const FString& ClassNeedle, TSharedPtr<FJsonObject>& OutNodeObject) -> bool
	{
		if (!InspectData || !InspectData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
		if (!(*InspectData)->TryGetArrayField(TEXT("graphs"), Graphs) || !Graphs)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& GraphValue : *Graphs)
		{
			const TSharedPtr<FJsonObject>* GraphObject = nullptr;
			if (!GraphValue.IsValid() || !GraphValue->TryGetObject(GraphObject) || !GraphObject || !GraphObject->IsValid())
			{
				continue;
			}

			const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
			if (!(*GraphObject)->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
			{
				continue;
			}

			for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
			{
				const TSharedPtr<FJsonObject>* NodeObject = nullptr;
				if (!NodeValue.IsValid() || !NodeValue->TryGetObject(NodeObject) || !NodeObject || !NodeObject->IsValid())
				{
					continue;
				}

				if ((*NodeObject)->GetStringField(TEXT("class")).Contains(ClassNeedle))
				{
					OutNodeObject = *NodeObject;
					return true;
				}
			}
		}

		return false;
	};

	auto CountGraphsByType = [](const TSharedPtr<FJsonObject>* ListData, const FString& GraphType) -> int32
	{
		if (!ListData || !ListData->IsValid())
		{
			return 0;
		}

		const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
		if (!(*ListData)->TryGetArrayField(TEXT("graphs"), Graphs) || !Graphs)
		{
			return 0;
		}

		int32 Count = 0;
		for (const TSharedPtr<FJsonValue>& GraphValue : *Graphs)
		{
			const TSharedPtr<FJsonObject>* GraphObject = nullptr;
			if (!GraphValue.IsValid() || !GraphValue->TryGetObject(GraphObject) || !GraphObject || !GraphObject->IsValid())
			{
				continue;
			}

			if ((*GraphObject)->GetStringField(TEXT("graph_type")).Equals(GraphType, ESearchCase::IgnoreCase))
			{
				++Count;
			}
		}

		return Count;
	};

	auto FindGraphByName = [](const TSharedPtr<FJsonObject>* ListData, const FString& GraphName) -> bool
	{
		if (!ListData || !ListData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
		if (!(*ListData)->TryGetArrayField(TEXT("graphs"), Graphs) || !Graphs)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& GraphValue : *Graphs)
		{
			const TSharedPtr<FJsonObject>* GraphObject = nullptr;
			if (!GraphValue.IsValid() || !GraphValue->TryGetObject(GraphObject) || !GraphObject || !GraphObject->IsValid())
			{
				continue;
			}

			if ((*GraphObject)->GetStringField(TEXT("graph_name")) == GraphName)
			{
				return true;
			}
		}

		return false;
	};

	auto FindNamedStateMachineEntry = [](const TSharedPtr<FJsonObject>* ListData, const FString& ExpectedName, TSharedPtr<FJsonObject>& OutEntryObject) -> bool
	{
		if (!ListData || !ListData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!(*ListData)->TryGetArrayField(TEXT("state_machines"), Entries) || !Entries)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& EntryValue : *Entries)
		{
			const TSharedPtr<FJsonObject>* EntryObject = nullptr;
			if (!EntryValue.IsValid() || !EntryValue->TryGetObject(EntryObject) || !EntryObject || !EntryObject->IsValid())
			{
				continue;
			}

			if ((*EntryObject)->GetStringField(TEXT("state_machine_name")) == ExpectedName)
			{
				OutEntryObject = *EntryObject;
				return true;
			}
		}

		return false;
	};

	auto FindNamedAnimLayerEntry = [](const TSharedPtr<FJsonObject>* ListData, const FString& ExpectedName, TSharedPtr<FJsonObject>& OutEntryObject) -> bool
	{
		if (!ListData || !ListData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!(*ListData)->TryGetArrayField(TEXT("anim_layers"), Entries) || !Entries)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& EntryValue : *Entries)
		{
			const TSharedPtr<FJsonObject>* EntryObject = nullptr;
			if (!EntryValue.IsValid() || !EntryValue->TryGetObject(EntryObject) || !EntryObject || !EntryObject->IsValid())
			{
				continue;
			}

			if ((*EntryObject)->GetStringField(TEXT("layer_name")) == ExpectedName)
			{
				OutEntryObject = *EntryObject;
				return true;
			}
		}

		return false;
	};

	auto FindLayerInterfaceEntry = [](const TSharedPtr<FJsonObject>* ListData, const FString& InterfaceClassPath, TSharedPtr<FJsonObject>& OutEntryObject) -> bool
	{
		if (!ListData || !ListData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!(*ListData)->TryGetArrayField(TEXT("interfaces"), Entries) || !Entries)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& EntryValue : *Entries)
		{
			const TSharedPtr<FJsonObject>* EntryObject = nullptr;
			if (!EntryValue.IsValid() || !EntryValue->TryGetObject(EntryObject) || !EntryObject || !EntryObject->IsValid())
			{
				continue;
			}

			if ((*EntryObject)->GetStringField(TEXT("interface_class")) == InterfaceClassPath)
			{
				OutEntryObject = *EntryObject;
				return true;
			}
		}

		return false;
	};

	auto FindStateNodeEntry = [](const TSharedPtr<FJsonObject>& StateMachineEntry, const FString& ExpectedStateName, TSharedPtr<FJsonObject>& OutEntryObject) -> bool
	{
		if (!StateMachineEntry.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* StateNodes = nullptr;
		if (!StateMachineEntry->TryGetArrayField(TEXT("state_nodes"), StateNodes) || !StateNodes)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& NodeValue : *StateNodes)
		{
			const TSharedPtr<FJsonObject>* NodeObject = nullptr;
			if (!NodeValue.IsValid() || !NodeValue->TryGetObject(NodeObject) || !NodeObject || !NodeObject->IsValid())
			{
				continue;
			}

			if ((*NodeObject)->GetStringField(TEXT("state_name")) == ExpectedStateName)
			{
				OutEntryObject = *NodeObject;
				return true;
			}
		}

		return false;
	};

	auto FindTransitionEntry = [](const TSharedPtr<FJsonObject>& StateMachineEntry, const FString& SourceStateName, const FString& TargetStateName, TSharedPtr<FJsonObject>& OutEntryObject) -> bool
	{
		if (!StateMachineEntry.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* TransitionNodes = nullptr;
		if (!StateMachineEntry->TryGetArrayField(TEXT("transition_nodes"), TransitionNodes) || !TransitionNodes)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& NodeValue : *TransitionNodes)
		{
			const TSharedPtr<FJsonObject>* NodeObject = nullptr;
			if (!NodeValue.IsValid() || !NodeValue->TryGetObject(NodeObject) || !NodeObject || !NodeObject->IsValid())
			{
				continue;
			}

			if ((*NodeObject)->GetStringField(TEXT("source_state_name")) == SourceStateName
				&& (*NodeObject)->GetStringField(TEXT("target_state_name")) == TargetStateName)
			{
				OutEntryObject = *NodeObject;
				return true;
			}
		}

		return false;
	};

	const FString CreateInterfaceBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-create-interface-001"),
		TEXT("anim_blueprint_create_layer_interface"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"compile_after_create\":false,\"open_editor\":false,\"save_after_create\":false}"), *LayerInterfaceAssetPath));
	FHttpSmokeResult CreateInterfaceResult;
	TestTrue(TEXT("AnimBlueprint create_layer_interface request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateInterfaceBody, CreateInterfaceResult));
	TestEqual(TEXT("AnimBlueprint create_layer_interface status code"), CreateInterfaceResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CreateInterfaceJson;
	TestTrue(TEXT("AnimBlueprint create_layer_interface response parses as JSON"), ParseJson(CreateInterfaceResult.Body, CreateInterfaceJson));
	TestTrue(TEXT("AnimBlueprint create_layer_interface root ok=true"), CreateInterfaceJson.IsValid() && CreateInterfaceJson->GetBoolField(TEXT("ok")));

	FString LayerInterfaceGeneratedClassPath;
	const TSharedPtr<FJsonObject>* CreateInterfaceData = nullptr;
	TestTrue(TEXT("AnimBlueprint create_layer_interface contains data object"), CreateInterfaceJson.IsValid() && CreateInterfaceJson->TryGetObjectField(TEXT("data"), CreateInterfaceData) && CreateInterfaceData && CreateInterfaceData->IsValid());
	if (CreateInterfaceData && CreateInterfaceData->IsValid())
	{
		LayerInterfaceGeneratedClassPath = (*CreateInterfaceData)->GetStringField(TEXT("generated_class"));
		TestTrue(TEXT("AnimBlueprint create_layer_interface is interface"), (*CreateInterfaceData)->GetBoolField(TEXT("is_layer_interface")));
		TestFalse(TEXT("AnimBlueprint create_layer_interface generated class non-empty"), LayerInterfaceGeneratedClassPath.IsEmpty());
	}

	const FString AddInterfaceLayerBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-add-interface-layer-001"),
		TEXT("anim_blueprint_add_anim_layer"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"layer_name\":\"%s\",\"compile_after_add\":false,\"save_after_add\":false}"), *LayerInterfaceAssetPath, *InterfaceLayerName));
	FHttpSmokeResult AddInterfaceLayerResult;
	TestTrue(TEXT("AnimBlueprint add_anim_layer on interface request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddInterfaceLayerBody, AddInterfaceLayerResult));
	TestEqual(TEXT("AnimBlueprint add_anim_layer on interface status code"), AddInterfaceLayerResult.StatusCode, 200);

	const FString CompileInterfaceBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-compile-interface-001"),
		TEXT("anim_blueprint_compile"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"include_messages\":true,\"max_messages\":16,\"save_after_compile\":false}"), *LayerInterfaceAssetPath));
	FHttpSmokeResult CompileInterfaceResult;
	TestTrue(TEXT("AnimBlueprint compile interface request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CompileInterfaceBody, CompileInterfaceResult));
	TestEqual(TEXT("AnimBlueprint compile interface status code"), CompileInterfaceResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CompileInterfaceJson;
	TestTrue(TEXT("AnimBlueprint compile interface response parses as JSON"), ParseJson(CompileInterfaceResult.Body, CompileInterfaceJson));
	TestTrue(TEXT("AnimBlueprint compile interface root ok=true"), CompileInterfaceJson.IsValid() && CompileInterfaceJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* CompileInterfaceData = nullptr;
	TestTrue(TEXT("AnimBlueprint compile interface contains data object"), CompileInterfaceJson.IsValid() && CompileInterfaceJson->TryGetObjectField(TEXT("data"), CompileInterfaceData) && CompileInterfaceData && CompileInterfaceData->IsValid());
	if (CompileInterfaceData && CompileInterfaceData->IsValid())
	{
		TestFalse(TEXT("AnimBlueprint compile interface reports no error"), (*CompileInterfaceData)->GetBoolField(TEXT("has_error")));
	}

	const FString CreateAnimBlueprintBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-create-001"),
		TEXT("anim_blueprint_create"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"parent_class\":\"/Script/Engine.AnimInstance\",\"target_skeleton\":\"%s\",\"preview_skeletal_mesh\":\"%s\",\"compile_after_create\":false,\"open_editor\":false,\"save_after_create\":false}"), *AnimBlueprintAssetPath, *SkeletonObjectPath, *SkeletalMeshObjectPath));
	FHttpSmokeResult CreateAnimBlueprintResult;
	TestTrue(TEXT("AnimBlueprint create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateAnimBlueprintBody, CreateAnimBlueprintResult));
	TestEqual(TEXT("AnimBlueprint create status code"), CreateAnimBlueprintResult.StatusCode, 200);

	const FString AddVariableBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-add-variable-001"),
		TEXT("anim_blueprint_add_variable"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"variable_name\":\"%s\",\"pin_category\":\"bool\",\"compile_after_add\":false,\"save_after_add\":false}"), *AnimBlueprintAssetPath, *VariableName));
	FHttpSmokeResult AddVariableResult;
	TestTrue(TEXT("AnimBlueprint add_variable request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddVariableBody, AddVariableResult));
	TestEqual(TEXT("AnimBlueprint add_variable status code"), AddVariableResult.StatusCode, 200);

	const FString AddCustomEventBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-custom-event-001"),
		TEXT("anim_blueprint_add_custom_event_node"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"event_name\":\"%s\",\"graph_name\":\"EventGraph\",\"pos_x\":-320,\"pos_y\":0,\"compile_after_add\":false,\"save_after_add\":false}"), *AnimBlueprintAssetPath, *CustomEventName));
	FHttpSmokeResult AddCustomEventResult;
	TestTrue(TEXT("AnimBlueprint add_custom_event_node request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddCustomEventBody, AddCustomEventResult));
	TestEqual(TEXT("AnimBlueprint add_custom_event_node status code"), AddCustomEventResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddCustomEventJson;
	TestTrue(TEXT("AnimBlueprint add_custom_event_node response parses as JSON"), ParseJson(AddCustomEventResult.Body, AddCustomEventJson));
	FString CustomEventNodeGuid;
	const TSharedPtr<FJsonObject>* AddCustomEventData = nullptr;
	TestTrue(TEXT("AnimBlueprint add_custom_event_node contains data object"), AddCustomEventJson.IsValid() && AddCustomEventJson->TryGetObjectField(TEXT("data"), AddCustomEventData) && AddCustomEventData && AddCustomEventData->IsValid());
	if (AddCustomEventData && AddCustomEventData->IsValid())
	{
		CustomEventNodeGuid = (*AddCustomEventData)->GetStringField(TEXT("node_guid"));
	}

	const FString AddCallFunctionBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-call-function-001"),
		TEXT("anim_blueprint_add_call_function_node"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_name\":\"EventGraph\",\"function_owner_class\":\"/Script/Engine.KismetSystemLibrary\",\"function_name\":\"PrintString\",\"pos_x\":48,\"pos_y\":0,\"compile_after_add\":false,\"save_after_add\":false}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult AddCallFunctionResult;
	TestTrue(TEXT("AnimBlueprint add_call_function_node request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddCallFunctionBody, AddCallFunctionResult));
	TestEqual(TEXT("AnimBlueprint add_call_function_node status code"), AddCallFunctionResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddCallFunctionJson;
	TestTrue(TEXT("AnimBlueprint add_call_function_node response parses as JSON"), ParseJson(AddCallFunctionResult.Body, AddCallFunctionJson));
	FString CallFunctionNodeGuid;
	const TSharedPtr<FJsonObject>* AddCallFunctionData = nullptr;
	TestTrue(TEXT("AnimBlueprint add_call_function_node contains data object"), AddCallFunctionJson.IsValid() && AddCallFunctionJson->TryGetObjectField(TEXT("data"), AddCallFunctionData) && AddCallFunctionData && AddCallFunctionData->IsValid());
	if (AddCallFunctionData && AddCallFunctionData->IsValid())
	{
		CallFunctionNodeGuid = (*AddCallFunctionData)->GetStringField(TEXT("node_guid"));
	}

	const FString AddVariableNodeBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-variable-node-001"),
		TEXT("anim_blueprint_add_variable_node"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_name\":\"EventGraph\",\"variable_name\":\"%s\",\"node_type\":\"get\",\"pos_x\":-160,\"pos_y\":120,\"compile_after_add\":false,\"save_after_add\":false}"), *AnimBlueprintAssetPath, *VariableName));
	FHttpSmokeResult AddVariableNodeResult;
	TestTrue(TEXT("AnimBlueprint add_variable_node request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddVariableNodeBody, AddVariableNodeResult));
	TestEqual(TEXT("AnimBlueprint add_variable_node status code"), AddVariableNodeResult.StatusCode, 200);

	const FString InspectEventGraphBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-inspect-eventgraph-001"),
		TEXT("anim_blueprint_inspect_nodes"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_name\":\"EventGraph\",\"include_pins\":true,\"limit_per_graph\":64}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult InspectEventGraphResult;
	TestTrue(TEXT("AnimBlueprint inspect_nodes on event graph request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, InspectEventGraphBody, InspectEventGraphResult));
	TestEqual(TEXT("AnimBlueprint inspect_nodes on event graph status code"), InspectEventGraphResult.StatusCode, 200);

	TSharedPtr<FJsonObject> InspectEventGraphJson;
	TestTrue(TEXT("AnimBlueprint inspect_nodes on event graph parses JSON"), ParseJson(InspectEventGraphResult.Body, InspectEventGraphJson));
	const TSharedPtr<FJsonObject>* InspectEventGraphData = nullptr;
	TestTrue(TEXT("AnimBlueprint inspect_nodes on event graph contains data object"), InspectEventGraphJson.IsValid() && InspectEventGraphJson->TryGetObjectField(TEXT("data"), InspectEventGraphData) && InspectEventGraphData && InspectEventGraphData->IsValid());

	TSharedPtr<FJsonObject> CustomEventNodeObject;
	TSharedPtr<FJsonObject> CallFunctionNodeObject;
	TestTrue(TEXT("AnimBlueprint inspect_nodes finds custom event"), FindNodeInInspect(InspectEventGraphData, CustomEventNodeGuid, CustomEventNodeObject));
	TestTrue(TEXT("AnimBlueprint inspect_nodes finds call function"), FindNodeInInspect(InspectEventGraphData, CallFunctionNodeGuid, CallFunctionNodeObject));

	FString EventExecPinName;
	FString CallExecPinName;
	FString CallStringPinName;
	TestTrue(TEXT("AnimBlueprint custom event has exec output pin"), FindPinName(CustomEventNodeObject, TEXT("output"), TEXT("exec"), EventExecPinName));
	TestTrue(TEXT("AnimBlueprint call function has exec input pin"), FindPinName(CallFunctionNodeObject, TEXT("input"), TEXT("exec"), CallExecPinName));
	TestTrue(TEXT("AnimBlueprint call function has string input pin"), FindPinName(CallFunctionNodeObject, TEXT("input"), TEXT("string"), CallStringPinName));

	const FString ConnectPinsBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-connect-001"),
		TEXT("anim_blueprint_connect_pins"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_name\":\"EventGraph\",\"from_node_guid\":\"%s\",\"from_pin\":\"%s\",\"to_node_guid\":\"%s\",\"to_pin\":\"%s\",\"compile_after_connect\":false,\"save_after_connect\":false}"), *AnimBlueprintAssetPath, *CustomEventNodeGuid, *EventExecPinName, *CallFunctionNodeGuid, *CallExecPinName));
	FHttpSmokeResult ConnectPinsResult;
	TestTrue(TEXT("AnimBlueprint connect_pins request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ConnectPinsBody, ConnectPinsResult));
	TestEqual(TEXT("AnimBlueprint connect_pins status code"), ConnectPinsResult.StatusCode, 200);

	const FString DisconnectPinsBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-disconnect-001"),
		TEXT("anim_blueprint_disconnect_pins"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_name\":\"EventGraph\",\"from_node_guid\":\"%s\",\"from_pin\":\"%s\",\"to_node_guid\":\"%s\",\"to_pin\":\"%s\",\"compile_after_disconnect\":false,\"save_after_disconnect\":false}"), *AnimBlueprintAssetPath, *CustomEventNodeGuid, *EventExecPinName, *CallFunctionNodeGuid, *CallExecPinName));
	FHttpSmokeResult DisconnectPinsResult;
	TestTrue(TEXT("AnimBlueprint disconnect_pins request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DisconnectPinsBody, DisconnectPinsResult));
	TestEqual(TEXT("AnimBlueprint disconnect_pins status code"), DisconnectPinsResult.StatusCode, 200);

	const FString ReconnectPinsBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-connect-002"),
		TEXT("anim_blueprint_connect_pins"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_name\":\"EventGraph\",\"from_node_guid\":\"%s\",\"from_pin\":\"%s\",\"to_node_guid\":\"%s\",\"to_pin\":\"%s\",\"compile_after_connect\":false,\"save_after_connect\":false}"), *AnimBlueprintAssetPath, *CustomEventNodeGuid, *EventExecPinName, *CallFunctionNodeGuid, *CallExecPinName));
	FHttpSmokeResult ReconnectPinsResult;
	TestTrue(TEXT("AnimBlueprint reconnect_pins request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ReconnectPinsBody, ReconnectPinsResult));
	TestEqual(TEXT("AnimBlueprint reconnect_pins status code"), ReconnectPinsResult.StatusCode, 200);

	const FString SetPinDefaultBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-set-pin-001"),
		TEXT("anim_blueprint_set_pin_default_value"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_name\":\"EventGraph\",\"node_guid\":\"%s\",\"pin_name\":\"%s\",\"default_value\":\"%s\",\"compile_after_set\":false,\"save_after_set\":false}"), *AnimBlueprintAssetPath, *CallFunctionNodeGuid, *CallStringPinName, *StringValue));
	FHttpSmokeResult SetPinDefaultResult;
	TestTrue(TEXT("AnimBlueprint set_pin_default_value request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetPinDefaultBody, SetPinDefaultResult));
	TestEqual(TEXT("AnimBlueprint set_pin_default_value status code"), SetPinDefaultResult.StatusCode, 200);

	const FString AddFunctionGraphBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-add-function-001"),
		TEXT("anim_blueprint_add_function_graph"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"function_name\":\"%s\",\"compile_after_add\":false,\"save_after_add\":false}"), *AnimBlueprintAssetPath, *LogicFunctionName));
	FHttpSmokeResult AddFunctionGraphResult;
	TestTrue(TEXT("AnimBlueprint add_function_graph request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddFunctionGraphBody, AddFunctionGraphResult));
	TestEqual(TEXT("AnimBlueprint add_function_graph status code"), AddFunctionGraphResult.StatusCode, 200);

	const FString AddLocalLayerBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-add-layer-001"),
		TEXT("anim_blueprint_add_anim_layer"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"layer_name\":\"%s\",\"compile_after_add\":false,\"save_after_add\":false}"), *AnimBlueprintAssetPath, *LocalLayerName));
	FHttpSmokeResult AddLocalLayerResult;
	TestTrue(TEXT("AnimBlueprint add_anim_layer request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddLocalLayerBody, AddLocalLayerResult));
	TestEqual(TEXT("AnimBlueprint add_anim_layer status code"), AddLocalLayerResult.StatusCode, 200);

	const FString RenameLocalLayerBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-rename-layer-001"),
		TEXT("anim_blueprint_rename_anim_layer"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"layer_name\":\"%s\",\"new_layer_name\":\"%s\",\"compile_after_rename\":false,\"save_after_rename\":false}"), *AnimBlueprintAssetPath, *LocalLayerName, *RenamedLocalLayerName));
	FHttpSmokeResult RenameLocalLayerResult;
	TestTrue(TEXT("AnimBlueprint rename_anim_layer request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RenameLocalLayerBody, RenameLocalLayerResult));
	TestEqual(TEXT("AnimBlueprint rename_anim_layer status code"), RenameLocalLayerResult.StatusCode, 200);

	const FString ImplementLayerInterfaceBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-implement-interface-001"),
		TEXT("anim_blueprint_implement_layer_interface"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"interface_class\":\"%s\",\"compile_after_add\":false,\"save_after_add\":false}"), *AnimBlueprintAssetPath, *LayerInterfaceGeneratedClassPath));
	FHttpSmokeResult ImplementLayerInterfaceResult;
	TestTrue(TEXT("AnimBlueprint implement_layer_interface request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ImplementLayerInterfaceBody, ImplementLayerInterfaceResult));
	TestEqual(TEXT("AnimBlueprint implement_layer_interface status code"), ImplementLayerInterfaceResult.StatusCode, 200);

	const FString AddStateMachineBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-add-sm-001"),
		TEXT("anim_blueprint_add_state_machine"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"state_machine_name\":\"%s\",\"anim_graph_name\":\"AnimGraph\",\"pos_x\":-400,\"pos_y\":80,\"compile_after_add\":false,\"save_after_add\":false}"), *AnimBlueprintAssetPath, *StateMachineName));
	FHttpSmokeResult AddStateMachineResult;
	TestTrue(TEXT("AnimBlueprint add_state_machine request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddStateMachineBody, AddStateMachineResult));
	TestEqual(TEXT("AnimBlueprint add_state_machine status code"), AddStateMachineResult.StatusCode, 200);

	const FString AddIdleStateBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-add-state-idle-001"),
		TEXT("anim_blueprint_add_state"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"state_machine_name\":\"%s\",\"state_name\":\"%s\",\"pos_x\":-128,\"pos_y\":0,\"compile_after_add\":false,\"save_after_add\":false}"), *AnimBlueprintAssetPath, *StateMachineName, *IdleStateName));
	FHttpSmokeResult AddIdleStateResult;
	TestTrue(TEXT("AnimBlueprint add_state idle request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddIdleStateBody, AddIdleStateResult));
	TestEqual(TEXT("AnimBlueprint add_state idle status code"), AddIdleStateResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddIdleStateJson;
	TestTrue(TEXT("AnimBlueprint add_state idle response parses as JSON"), ParseJson(AddIdleStateResult.Body, AddIdleStateJson));
	FString IdleStateNodeGuid;
	const TSharedPtr<FJsonObject>* AddIdleStateData = nullptr;
	TestTrue(TEXT("AnimBlueprint add_state idle contains data object"), AddIdleStateJson.IsValid() && AddIdleStateJson->TryGetObjectField(TEXT("data"), AddIdleStateData) && AddIdleStateData && AddIdleStateData->IsValid());
	if (AddIdleStateData && AddIdleStateData->IsValid())
	{
		IdleStateNodeGuid = (*AddIdleStateData)->GetStringField(TEXT("node_guid"));
	}

	const FString AddRunStateBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-add-state-run-001"),
		TEXT("anim_blueprint_add_state"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"state_machine_name\":\"%s\",\"state_name\":\"%s\",\"pos_x\":160,\"pos_y\":0,\"compile_after_add\":false,\"save_after_add\":false}"), *AnimBlueprintAssetPath, *StateMachineName, *RunStateName));
	FHttpSmokeResult AddRunStateResult;
	TestTrue(TEXT("AnimBlueprint add_state run request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddRunStateBody, AddRunStateResult));
	TestEqual(TEXT("AnimBlueprint add_state run status code"), AddRunStateResult.StatusCode, 200);

	const FString SetEntryStateBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-set-entry-001"),
		TEXT("anim_blueprint_set_entry_state"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"state_machine_name\":\"%s\",\"state_name\":\"%s\",\"compile_after_set\":false,\"save_after_set\":false}"), *AnimBlueprintAssetPath, *StateMachineName, *IdleStateName));
	FHttpSmokeResult SetEntryStateResult;
	TestTrue(TEXT("AnimBlueprint set_entry_state request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetEntryStateBody, SetEntryStateResult));
	TestEqual(TEXT("AnimBlueprint set_entry_state status code"), SetEntryStateResult.StatusCode, 200);

	const FString SetStatePropertiesBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-set-state-props-001"),
		TEXT("anim_blueprint_set_state_properties"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"state_machine_name\":\"%s\",\"state_name\":\"%s\",\"pos_x\":192,\"pos_y\":-32,\"always_reset_on_entry\":true,\"state_type\":\"blend_graph\",\"compile_after_set\":false,\"save_after_set\":false}"), *AnimBlueprintAssetPath, *StateMachineName, *RunStateName));
	FHttpSmokeResult SetStatePropertiesResult;
	TestTrue(TEXT("AnimBlueprint set_state_properties request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetStatePropertiesBody, SetStatePropertiesResult));
	TestEqual(TEXT("AnimBlueprint set_state_properties status code"), SetStatePropertiesResult.StatusCode, 200);

	const FString AddConduitBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-add-conduit-001"),
		TEXT("anim_blueprint_add_conduit"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"state_machine_name\":\"%s\",\"conduit_name\":\"%s\",\"pos_x\":16,\"pos_y\":160,\"compile_after_add\":false,\"save_after_add\":false}"), *AnimBlueprintAssetPath, *StateMachineName, *ConduitName));
	FHttpSmokeResult AddConduitResult;
	TestTrue(TEXT("AnimBlueprint add_conduit request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddConduitBody, AddConduitResult));
	TestEqual(TEXT("AnimBlueprint add_conduit status code"), AddConduitResult.StatusCode, 200);

	const FString AddAliasBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-add-alias-001"),
		TEXT("anim_blueprint_add_state_alias"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"state_machine_name\":\"%s\",\"alias_name\":\"%s\",\"pos_x\":16,\"pos_y\":-160,\"compile_after_add\":false,\"save_after_add\":false}"), *AnimBlueprintAssetPath, *StateMachineName, *AliasName));
	FHttpSmokeResult AddAliasResult;
	TestTrue(TEXT("AnimBlueprint add_state_alias request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddAliasBody, AddAliasResult));
	TestEqual(TEXT("AnimBlueprint add_state_alias status code"), AddAliasResult.StatusCode, 200);

	const FString SetAliasTargetsBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-set-alias-targets-001"),
		TEXT("anim_blueprint_set_state_alias_targets"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"state_machine_name\":\"%s\",\"alias_name\":\"%s\",\"global_alias\":false,\"alias_target_states\":[\"%s\"],\"compile_after_set\":false,\"save_after_set\":false}"), *AnimBlueprintAssetPath, *StateMachineName, *AliasName, *IdleStateName));
	FHttpSmokeResult SetAliasTargetsResult;
	TestTrue(TEXT("AnimBlueprint set_state_alias_targets request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetAliasTargetsBody, SetAliasTargetsResult));
	TestEqual(TEXT("AnimBlueprint set_state_alias_targets status code"), SetAliasTargetsResult.StatusCode, 200);

	const FString AddTransitionBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-add-transition-001"),
		TEXT("anim_blueprint_add_transition"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"state_machine_name\":\"%s\",\"source_state_name\":\"%s\",\"target_state_name\":\"%s\",\"priority_order\":2,\"crossfade_duration\":0.35,\"compile_after_add\":false,\"save_after_add\":false}"), *AnimBlueprintAssetPath, *StateMachineName, *IdleStateName, *RunStateName));
	FHttpSmokeResult AddTransitionResult;
	TestTrue(TEXT("AnimBlueprint add_transition request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddTransitionBody, AddTransitionResult));
	TestEqual(TEXT("AnimBlueprint add_transition status code"), AddTransitionResult.StatusCode, 200);

	const FString SetTransitionPropertiesBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-set-transition-props-001"),
		TEXT("anim_blueprint_set_transition_properties"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"state_machine_name\":\"%s\",\"source_state_name\":\"%s\",\"target_state_name\":\"%s\",\"priority_order\":3,\"crossfade_duration\":0.5,\"disabled\":true,\"automatic_rule\":true,\"pos_x\":12,\"pos_y\":44,\"compile_after_set\":false,\"save_after_set\":false}"), *AnimBlueprintAssetPath, *StateMachineName, *IdleStateName, *RunStateName));
	FHttpSmokeResult SetTransitionPropertiesResult;
	TestTrue(TEXT("AnimBlueprint set_transition_properties request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetTransitionPropertiesBody, SetTransitionPropertiesResult));
	TestEqual(TEXT("AnimBlueprint set_transition_properties status code"), SetTransitionPropertiesResult.StatusCode, 200);

	const FString CompileMainBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-compile-main-001"),
		TEXT("anim_blueprint_compile"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"include_messages\":true,\"max_messages\":24,\"save_after_compile\":false}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult CompileMainResult;
	TestTrue(TEXT("AnimBlueprint compile main request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CompileMainBody, CompileMainResult));
	TestEqual(TEXT("AnimBlueprint compile main status code"), CompileMainResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CompileMainJson;
	TestTrue(TEXT("AnimBlueprint compile main response parses as JSON"), ParseJson(CompileMainResult.Body, CompileMainJson));
	TestTrue(TEXT("AnimBlueprint compile main root ok=true"), CompileMainJson.IsValid() && CompileMainJson->GetBoolField(TEXT("ok")));
	const TSharedPtr<FJsonObject>* CompileMainData = nullptr;
	TestTrue(TEXT("AnimBlueprint compile main contains data object"), CompileMainJson.IsValid() && CompileMainJson->TryGetObjectField(TEXT("data"), CompileMainData) && CompileMainData && CompileMainData->IsValid());
	if (CompileMainData && CompileMainData->IsValid())
	{
		TestFalse(TEXT("AnimBlueprint compile main reports no error"), (*CompileMainData)->GetBoolField(TEXT("has_error")));
	}

	const FString SetCdoPropertyBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-set-cdo-001"),
		TEXT("anim_blueprint_set_cdo_property"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"property_name\":\"%s\",\"value_text\":\"true\"}"), *AnimBlueprintAssetPath, *VariableName));
	FHttpSmokeResult SetCdoPropertyResult;
	TestTrue(TEXT("AnimBlueprint set_cdo_property request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetCdoPropertyBody, SetCdoPropertyResult));
	TestEqual(TEXT("AnimBlueprint set_cdo_property status code"), SetCdoPropertyResult.StatusCode, 200);

	const FString ListGraphsBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-list-graphs-001"),
		TEXT("anim_blueprint_list_graphs"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult ListGraphsResult;
	TestTrue(TEXT("AnimBlueprint list_graphs request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ListGraphsBody, ListGraphsResult));
	TestEqual(TEXT("AnimBlueprint list_graphs status code"), ListGraphsResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ListGraphsJson;
	TestTrue(TEXT("AnimBlueprint list_graphs response parses as JSON"), ParseJson(ListGraphsResult.Body, ListGraphsJson));
	const TSharedPtr<FJsonObject>* ListGraphsData = nullptr;
	TestTrue(TEXT("AnimBlueprint list_graphs contains data object"), ListGraphsJson.IsValid() && ListGraphsJson->TryGetObjectField(TEXT("data"), ListGraphsData) && ListGraphsData && ListGraphsData->IsValid());
	if (ListGraphsData && ListGraphsData->IsValid())
	{
		TestTrue(TEXT("AnimBlueprint list_graphs includes logic function"), FindGraphByName(ListGraphsData, LogicFunctionName));
		TestTrue(TEXT("AnimBlueprint list_graphs includes local anim layer"), FindGraphByName(ListGraphsData, RenamedLocalLayerName));
		TestTrue(TEXT("AnimBlueprint list_graphs function_count>=2"), CountGraphsByType(ListGraphsData, TEXT("function")) >= 2);
	}

	const FString ListLayerInterfacesBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-list-interfaces-001"),
		TEXT("anim_blueprint_list_layer_interfaces"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult ListLayerInterfacesResult;
	TestTrue(TEXT("AnimBlueprint list_layer_interfaces request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ListLayerInterfacesBody, ListLayerInterfacesResult));
	TestEqual(TEXT("AnimBlueprint list_layer_interfaces status code"), ListLayerInterfacesResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ListLayerInterfacesJson;
	TestTrue(TEXT("AnimBlueprint list_layer_interfaces response parses as JSON"), ParseJson(ListLayerInterfacesResult.Body, ListLayerInterfacesJson));
	const TSharedPtr<FJsonObject>* ListLayerInterfacesData = nullptr;
	TestTrue(TEXT("AnimBlueprint list_layer_interfaces contains data object"), ListLayerInterfacesJson.IsValid() && ListLayerInterfacesJson->TryGetObjectField(TEXT("data"), ListLayerInterfacesData) && ListLayerInterfacesData && ListLayerInterfacesData->IsValid());
	if (ListLayerInterfacesData && ListLayerInterfacesData->IsValid())
	{
		TestTrue(TEXT("AnimBlueprint list_layer_interfaces count>=1"), static_cast<int32>((*ListLayerInterfacesData)->GetIntegerField(TEXT("implemented_interface_count"))) >= 1);

		TSharedPtr<FJsonObject> InterfaceEntry;
		TestTrue(TEXT("AnimBlueprint list_layer_interfaces contains implemented interface"), FindLayerInterfaceEntry(ListLayerInterfacesData, LayerInterfaceGeneratedClassPath, InterfaceEntry));
		if (InterfaceEntry.IsValid())
		{
			TestTrue(TEXT("AnimBlueprint list_layer_interfaces graph_count>=1"), static_cast<int32>(InterfaceEntry->GetIntegerField(TEXT("graph_count"))) >= 1);
		}
	}

	const FString ListAnimLayersBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-list-layers-001"),
		TEXT("anim_blueprint_list_anim_layers"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult ListAnimLayersResult;
	TestTrue(TEXT("AnimBlueprint list_anim_layers request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ListAnimLayersBody, ListAnimLayersResult));
	TestEqual(TEXT("AnimBlueprint list_anim_layers status code"), ListAnimLayersResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ListAnimLayersJson;
	TestTrue(TEXT("AnimBlueprint list_anim_layers response parses as JSON"), ParseJson(ListAnimLayersResult.Body, ListAnimLayersJson));
	const TSharedPtr<FJsonObject>* ListAnimLayersData = nullptr;
	TestTrue(TEXT("AnimBlueprint list_anim_layers contains data object"), ListAnimLayersJson.IsValid() && ListAnimLayersJson->TryGetObjectField(TEXT("data"), ListAnimLayersData) && ListAnimLayersData && ListAnimLayersData->IsValid());
	if (ListAnimLayersData && ListAnimLayersData->IsValid())
	{
		TSharedPtr<FJsonObject> LocalLayerEntry;
		TSharedPtr<FJsonObject> InterfaceLayerEntry;
		TestTrue(TEXT("AnimBlueprint list_anim_layers contains renamed local layer"), FindNamedAnimLayerEntry(ListAnimLayersData, RenamedLocalLayerName, LocalLayerEntry));
		TestTrue(TEXT("AnimBlueprint list_anim_layers contains interface layer"), FindNamedAnimLayerEntry(ListAnimLayersData, InterfaceLayerName, InterfaceLayerEntry));
		if (LocalLayerEntry.IsValid())
		{
			TestTrue(TEXT("AnimBlueprint local layer has graph"), LocalLayerEntry->GetBoolField(TEXT("has_graph")));
		}
		if (InterfaceLayerEntry.IsValid())
		{
			TestTrue(TEXT("AnimBlueprint interface layer marked as interface layer"), InterfaceLayerEntry->GetBoolField(TEXT("is_interface_layer")));
		}
	}

	const FString ListStateMachinesBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-list-sm-001"),
		TEXT("anim_blueprint_list_state_machines"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult ListStateMachinesResult;
	TestTrue(TEXT("AnimBlueprint list_state_machines request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ListStateMachinesBody, ListStateMachinesResult));
	TestEqual(TEXT("AnimBlueprint list_state_machines status code"), ListStateMachinesResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ListStateMachinesJson;
	TestTrue(TEXT("AnimBlueprint list_state_machines response parses as JSON"), ParseJson(ListStateMachinesResult.Body, ListStateMachinesJson));
	const TSharedPtr<FJsonObject>* ListStateMachinesData = nullptr;
	TestTrue(TEXT("AnimBlueprint list_state_machines contains data object"), ListStateMachinesJson.IsValid() && ListStateMachinesJson->TryGetObjectField(TEXT("data"), ListStateMachinesData) && ListStateMachinesData && ListStateMachinesData->IsValid());
	FString IdleStateGraphPath;
	FString TransitionGraphPath;
	if (ListStateMachinesData && ListStateMachinesData->IsValid())
	{
		TSharedPtr<FJsonObject> StateMachineEntry;
		TestTrue(TEXT("AnimBlueprint list_state_machines contains machine"), FindNamedStateMachineEntry(ListStateMachinesData, StateMachineName, StateMachineEntry));
		if (StateMachineEntry.IsValid())
		{
			TestTrue(TEXT("AnimBlueprint state machine state_count>=2"), static_cast<int32>(StateMachineEntry->GetIntegerField(TEXT("state_count"))) >= 2);
			TestTrue(TEXT("AnimBlueprint state machine conduit_count>=1"), static_cast<int32>(StateMachineEntry->GetIntegerField(TEXT("conduit_count"))) >= 1);
			TestTrue(TEXT("AnimBlueprint state machine alias_count>=1"), static_cast<int32>(StateMachineEntry->GetIntegerField(TEXT("alias_count"))) >= 1);
			TestTrue(TEXT("AnimBlueprint state machine transition_count>=1"), static_cast<int32>(StateMachineEntry->GetIntegerField(TEXT("transition_count"))) >= 1);
			TestEqual(TEXT("AnimBlueprint state machine entry state is idle"), StateMachineEntry->GetStringField(TEXT("entry_connected_state_name")), IdleStateName);

			const TArray<TSharedPtr<FJsonValue>>* StateNodes = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* TransitionNodes = nullptr;
			TestTrue(TEXT("AnimBlueprint state machine includes state_nodes array"), StateMachineEntry->TryGetArrayField(TEXT("state_nodes"), StateNodes) && StateNodes != nullptr);
			TestTrue(TEXT("AnimBlueprint state machine includes transition_nodes array"), StateMachineEntry->TryGetArrayField(TEXT("transition_nodes"), TransitionNodes) && TransitionNodes != nullptr);

			TSharedPtr<FJsonObject> IdleStateEntry;
			TSharedPtr<FJsonObject> RunStateEntry;
			TSharedPtr<FJsonObject> TransitionEntry;
			TestTrue(TEXT("AnimBlueprint state machine contains idle state entry"), FindStateNodeEntry(StateMachineEntry, IdleStateName, IdleStateEntry));
			TestTrue(TEXT("AnimBlueprint state machine contains run state entry"), FindStateNodeEntry(StateMachineEntry, RunStateName, RunStateEntry));
			TestTrue(TEXT("AnimBlueprint state machine contains transition entry"), FindTransitionEntry(StateMachineEntry, IdleStateName, RunStateName, TransitionEntry));
			if (IdleStateEntry.IsValid())
			{
				IdleStateGraphPath = IdleStateEntry->GetStringField(TEXT("bound_graph_path"));
				TestFalse(TEXT("AnimBlueprint idle state bound graph path non-empty"), IdleStateGraphPath.IsEmpty());
			}
			if (RunStateEntry.IsValid())
			{
				TestEqual(TEXT("AnimBlueprint run state type updated"), RunStateEntry->GetStringField(TEXT("state_type")), FString(TEXT("blend_graph")));
				TestTrue(TEXT("AnimBlueprint run state always_reset_on_entry updated"), RunStateEntry->GetBoolField(TEXT("always_reset_on_entry")));
			}
			if (TransitionEntry.IsValid())
			{
				TransitionGraphPath = TransitionEntry->GetStringField(TEXT("bound_graph_path"));
				TestFalse(TEXT("AnimBlueprint transition bound graph path non-empty"), TransitionGraphPath.IsEmpty());
				TestTrue(TEXT("AnimBlueprint transition disabled updated"), TransitionEntry->GetBoolField(TEXT("disabled")));
				TestTrue(TEXT("AnimBlueprint transition automatic_rule updated"), TransitionEntry->GetBoolField(TEXT("automatic_rule")));
				TestEqual(TEXT("AnimBlueprint transition priority updated"), static_cast<int32>(TransitionEntry->GetIntegerField(TEXT("priority_order"))), 3);
			}
		}
	}

	const FString InspectStateMachineBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-inspect-sm-001"),
		TEXT("anim_blueprint_inspect_nodes"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_name\":\"%s\",\"include_pins\":true,\"limit_per_graph\":128}"), *AnimBlueprintAssetPath, *StateMachineName));
	FHttpSmokeResult InspectStateMachineResult;
	TestTrue(TEXT("AnimBlueprint inspect_nodes on state machine request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, InspectStateMachineBody, InspectStateMachineResult));
	TestEqual(TEXT("AnimBlueprint inspect_nodes on state machine status code"), InspectStateMachineResult.StatusCode, 200);

	TSharedPtr<FJsonObject> InspectStateMachineJson;
	TestTrue(TEXT("AnimBlueprint inspect_nodes on state machine response parses as JSON"), ParseJson(InspectStateMachineResult.Body, InspectStateMachineJson));
	const TSharedPtr<FJsonObject>* InspectStateMachineData = nullptr;
	TestTrue(TEXT("AnimBlueprint inspect_nodes on state machine contains data object"), InspectStateMachineJson.IsValid() && InspectStateMachineJson->TryGetObjectField(TEXT("data"), InspectStateMachineData) && InspectStateMachineData && InspectStateMachineData->IsValid());
	if (InspectStateMachineData && InspectStateMachineData->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
		TestTrue(TEXT("AnimBlueprint inspect_nodes on state machine returns graph array"), (*InspectStateMachineData)->TryGetArrayField(TEXT("graphs"), Graphs) && Graphs != nullptr);
		if (Graphs)
		{
			TestTrue(TEXT("AnimBlueprint inspect_nodes on state machine graph_count>=1"), Graphs->Num() >= 1);
		}
	}

	const FString InspectIdleStateGraphBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-inspect-state-graph-001"),
		TEXT("anim_blueprint_inspect_nodes"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_name\":\"%s\",\"include_pins\":true,\"limit_per_graph\":64}"), *AnimBlueprintAssetPath, *IdleStateGraphPath));
	FHttpSmokeResult InspectIdleStateGraphResult;
	TestTrue(TEXT("AnimBlueprint inspect_nodes on state graph request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, InspectIdleStateGraphBody, InspectIdleStateGraphResult));
	TestEqual(TEXT("AnimBlueprint inspect_nodes on state graph status code"), InspectIdleStateGraphResult.StatusCode, 200);

	const FString InspectTransitionGraphBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-inspect-transition-graph-001"),
		TEXT("anim_blueprint_inspect_nodes"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_name\":\"%s\",\"include_pins\":true,\"limit_per_graph\":64}"), *AnimBlueprintAssetPath, *TransitionGraphPath));
	FHttpSmokeResult InspectTransitionGraphResult;
	TestTrue(TEXT("AnimBlueprint inspect_nodes on transition graph request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, InspectTransitionGraphBody, InspectTransitionGraphResult));
	TestEqual(TEXT("AnimBlueprint inspect_nodes on transition graph status code"), InspectTransitionGraphResult.StatusCode, 200);

	TSharedPtr<FJsonObject> InspectTransitionGraphJson;
	TestTrue(TEXT("AnimBlueprint inspect_nodes on transition graph parses JSON"), ParseJson(InspectTransitionGraphResult.Body, InspectTransitionGraphJson));
	const TSharedPtr<FJsonObject>* InspectTransitionGraphData = nullptr;
	TestTrue(TEXT("AnimBlueprint inspect_nodes on transition graph contains data object"), InspectTransitionGraphJson.IsValid() && InspectTransitionGraphJson->TryGetObjectField(TEXT("data"), InspectTransitionGraphData) && InspectTransitionGraphData && InspectTransitionGraphData->IsValid());

	TSharedPtr<FJsonObject> TransitionResultNodeObject;
	FString TransitionResultNodeGuid;
	FString TransitionResultPinName;
	TestTrue(TEXT("AnimBlueprint transition graph exposes result node"), FindNodeByClassContains(InspectTransitionGraphData, TEXT("TransitionResult"), TransitionResultNodeObject));
	if (TransitionResultNodeObject.IsValid())
	{
		TransitionResultNodeGuid = TransitionResultNodeObject->GetStringField(TEXT("node_guid"));
		TestTrue(TEXT("AnimBlueprint transition result has bool input pin"), FindPinName(TransitionResultNodeObject, TEXT("input"), TEXT("bool"), TransitionResultPinName));
	}

	const FString SetTransitionRulePinBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-set-transition-rule-pin-001"),
		TEXT("anim_blueprint_set_pin_default_value"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_name\":\"%s\",\"node_guid\":\"%s\",\"pin_name\":\"%s\",\"default_value\":\"false\",\"compile_after_set\":false,\"save_after_set\":false}"), *AnimBlueprintAssetPath, *TransitionGraphPath, *TransitionResultNodeGuid, *TransitionResultPinName));
	FHttpSmokeResult SetTransitionRulePinResult;
	TestTrue(TEXT("AnimBlueprint set_pin_default_value on transition graph request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetTransitionRulePinBody, SetTransitionRulePinResult));
	TestEqual(TEXT("AnimBlueprint set_pin_default_value on transition graph status code"), SetTransitionRulePinResult.StatusCode, 200);

	const FString RenameStateNodeBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-rename-state-001"),
		TEXT("anim_blueprint_rename_state_node"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"state_machine_name\":\"%s\",\"state_name\":\"%s\",\"new_state_name\":\"%s\",\"compile_after_rename\":false,\"save_after_rename\":false}"), *AnimBlueprintAssetPath, *StateMachineName, *RunStateName, *RenamedRunStateName));
	FHttpSmokeResult RenameStateNodeResult;
	TestTrue(TEXT("AnimBlueprint rename_state_node request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RenameStateNodeBody, RenameStateNodeResult));
	TestEqual(TEXT("AnimBlueprint rename_state_node status code"), RenameStateNodeResult.StatusCode, 200);

	const FString RenameStateMachineBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-rename-sm-001"),
		TEXT("anim_blueprint_rename_state_machine"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"state_machine_name\":\"%s\",\"new_state_machine_name\":\"%s\",\"compile_after_rename\":false,\"save_after_rename\":false}"), *AnimBlueprintAssetPath, *StateMachineName, *RenamedStateMachineName));
	FHttpSmokeResult RenameStateMachineResult;
	TestTrue(TEXT("AnimBlueprint rename_state_machine request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RenameStateMachineBody, RenameStateMachineResult));
	TestEqual(TEXT("AnimBlueprint rename_state_machine status code"), RenameStateMachineResult.StatusCode, 200);

	const FString ClearEntryStateBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-clear-entry-001"),
		TEXT("anim_blueprint_clear_entry_state"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"state_machine_name\":\"%s\",\"compile_after_clear\":false,\"save_after_clear\":false}"), *AnimBlueprintAssetPath, *RenamedStateMachineName));
	FHttpSmokeResult ClearEntryStateResult;
	TestTrue(TEXT("AnimBlueprint clear_entry_state request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ClearEntryStateBody, ClearEntryStateResult));
	TestEqual(TEXT("AnimBlueprint clear_entry_state status code"), ClearEntryStateResult.StatusCode, 200);

	const FString RemoveTransitionBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-remove-transition-001"),
		TEXT("anim_blueprint_remove_transition"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"state_machine_name\":\"%s\",\"source_state_name\":\"%s\",\"target_state_name\":\"%s\",\"save_after_remove\":false}"), *AnimBlueprintAssetPath, *RenamedStateMachineName, *IdleStateName, *RenamedRunStateName));
	FHttpSmokeResult RemoveTransitionResult;
	TestTrue(TEXT("AnimBlueprint remove_transition request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveTransitionBody, RemoveTransitionResult));
	TestEqual(TEXT("AnimBlueprint remove_transition status code"), RemoveTransitionResult.StatusCode, 200);

	const FString RemoveAliasBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-remove-alias-001"),
		TEXT("anim_blueprint_remove_state_node"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"state_machine_name\":\"%s\",\"state_name\":\"%s\",\"save_after_remove\":false}"), *AnimBlueprintAssetPath, *RenamedStateMachineName, *AliasName));
	FHttpSmokeResult RemoveAliasResult;
	TestTrue(TEXT("AnimBlueprint remove_state_node alias request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveAliasBody, RemoveAliasResult));
	TestEqual(TEXT("AnimBlueprint remove_state_node alias status code"), RemoveAliasResult.StatusCode, 200);

	const FString RemoveConduitBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-remove-conduit-001"),
		TEXT("anim_blueprint_remove_state_node"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"state_machine_name\":\"%s\",\"state_name\":\"%s\",\"save_after_remove\":false}"), *AnimBlueprintAssetPath, *RenamedStateMachineName, *ConduitName));
	FHttpSmokeResult RemoveConduitResult;
	TestTrue(TEXT("AnimBlueprint remove_state_node conduit request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveConduitBody, RemoveConduitResult));
	TestEqual(TEXT("AnimBlueprint remove_state_node conduit status code"), RemoveConduitResult.StatusCode, 200);

	const FString RemoveLocalLayerBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-remove-layer-001"),
		TEXT("anim_blueprint_remove_anim_layer"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"layer_name\":\"%s\",\"compile_after_remove\":false,\"save_after_remove\":false}"), *AnimBlueprintAssetPath, *RenamedLocalLayerName));
	FHttpSmokeResult RemoveLocalLayerResult;
	TestTrue(TEXT("AnimBlueprint remove_anim_layer request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveLocalLayerBody, RemoveLocalLayerResult));
	TestEqual(TEXT("AnimBlueprint remove_anim_layer status code"), RemoveLocalLayerResult.StatusCode, 200);

	const FString RemoveLayerInterfaceBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-remove-interface-001"),
		TEXT("anim_blueprint_remove_layer_interface"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"interface_class\":\"%s\",\"compile_after_remove\":false,\"save_after_remove\":false}"), *AnimBlueprintAssetPath, *LayerInterfaceGeneratedClassPath));
	FHttpSmokeResult RemoveLayerInterfaceResult;
	TestTrue(TEXT("AnimBlueprint remove_layer_interface request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveLayerInterfaceBody, RemoveLayerInterfaceResult));
	TestEqual(TEXT("AnimBlueprint remove_layer_interface status code"), RemoveLayerInterfaceResult.StatusCode, 200);

	const FString RemoveStateMachineBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-remove-sm-001"),
		TEXT("anim_blueprint_remove_state_machine"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"state_machine_name\":\"%s\",\"save_after_remove\":false}"), *AnimBlueprintAssetPath, *RenamedStateMachineName));
	FHttpSmokeResult RemoveStateMachineResult;
	TestTrue(TEXT("AnimBlueprint remove_state_machine request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveStateMachineBody, RemoveStateMachineResult));
	TestEqual(TEXT("AnimBlueprint remove_state_machine status code"), RemoveStateMachineResult.StatusCode, 200);

	const FString FinalCompileBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-final-compile-001"),
		TEXT("anim_blueprint_compile"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"include_messages\":true,\"max_messages\":24,\"save_after_compile\":false}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult FinalCompileResult;
	TestTrue(TEXT("AnimBlueprint final compile request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, FinalCompileBody, FinalCompileResult));
	TestEqual(TEXT("AnimBlueprint final compile status code"), FinalCompileResult.StatusCode, 200);

	TSharedPtr<FJsonObject> FinalCompileJson;
	TestTrue(TEXT("AnimBlueprint final compile response parses as JSON"), ParseJson(FinalCompileResult.Body, FinalCompileJson));
	TestTrue(TEXT("AnimBlueprint final compile root ok=true"), FinalCompileJson.IsValid() && FinalCompileJson->GetBoolField(TEXT("ok")));
	const TSharedPtr<FJsonObject>* FinalCompileData = nullptr;
	TestTrue(TEXT("AnimBlueprint final compile contains data object"), FinalCompileJson.IsValid() && FinalCompileJson->TryGetObjectField(TEXT("data"), FinalCompileData) && FinalCompileData && FinalCompileData->IsValid());
	if (FinalCompileData && FinalCompileData->IsValid())
	{
		TestFalse(TEXT("AnimBlueprint final compile reports no error"), (*FinalCompileData)->GetBoolField(TEXT("has_error")));
		TestTrue(TEXT("AnimBlueprint final compile error count is zero"), static_cast<int32>((*FinalCompileData)->GetIntegerField(TEXT("error_count"))) == 0);
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceAnimBlueprintFolderSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.AnimBlueprintFolderWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceAnimBlueprintFolderSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString SkeletonObjectPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton.TutorialTPP_Skeleton");
	const FString SkeletalMeshObjectPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP.TutorialTPP");
	const FString BlendSpaceObjectPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/NewBlendSpace1D.NewBlendSpace1D");
	const FString AnimBlueprintAssetPath = MakeAutomationAssetPath(TEXT("ABPFolder"));
	FString RelativeFolder = AnimBlueprintAssetPath;
	while (RelativeFolder.StartsWith(TEXT("/")))
	{
		RelativeFolder.RightChopInline(1);
	}
	const FString FolderPath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("AnimBlueprint"), RelativeFolder));
	IFileManager::Get().DeleteDirectory(*FolderPath, false, true);

	auto LoadJsonFile = [this](const FString& FilePath, TSharedPtr<FJsonObject>& OutObj) -> bool
	{
		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
		{
			AddError(FString::Printf(TEXT("Load file failed: %s"), *FilePath));
			return false;
		}

		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, OutObj) || !OutObj.IsValid())
		{
			AddError(FString::Printf(TEXT("Parse json failed: %s"), *FilePath));
			return false;
		}

		return true;
	};

	auto SaveJsonFile = [this](const FString& FilePath, const TSharedPtr<FJsonObject>& Obj) -> bool
	{
		if (!Obj.IsValid())
		{
			AddError(FString::Printf(TEXT("Save json failed, invalid object: %s"), *FilePath));
			return false;
		}

		FString JsonText;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
		if (!FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer))
		{
			AddError(FString::Printf(TEXT("Serialize json failed: %s"), *FilePath));
			return false;
		}

		if (!FFileHelper::SaveStringToFile(JsonText, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			AddError(FString::Printf(TEXT("Write file failed: %s"), *FilePath));
			return false;
		}

		return true;
	};

	auto FindNodeObjectByClass = [](const TSharedPtr<FJsonObject>& GraphRoot, const FString& NodeClassPath, TSharedPtr<FJsonObject>& OutNodeObj) -> bool
	{
		if (!GraphRoot.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
		if (!GraphRoot->TryGetArrayField(TEXT("nodes"), NodesArray) || !NodesArray)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& NodeValue : *NodesArray)
		{
			const TSharedPtr<FJsonObject>* NodeObjPtr = nullptr;
			if (!NodeValue.IsValid() || !NodeValue->TryGetObject(NodeObjPtr) || !NodeObjPtr || !NodeObjPtr->IsValid())
			{
				continue;
			}

			FString NodeClass;
			if ((*NodeObjPtr)->TryGetStringField(TEXT("node_class"), NodeClass) && NodeClass.Equals(NodeClassPath, ESearchCase::IgnoreCase))
			{
				OutNodeObj = *NodeObjPtr;
				return true;
			}
		}

		return false;
	};

	auto FindPropertyObject = [](const TSharedPtr<FJsonObject>& NodeObj, const FString& PropertyName, TSharedPtr<FJsonObject>& OutPropertyObj) -> bool
	{
		if (!NodeObj.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* PropertiesArray = nullptr;
		if (!NodeObj->TryGetArrayField(TEXT("properties"), PropertiesArray) || !PropertiesArray)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& PropertyValue : *PropertiesArray)
		{
			const TSharedPtr<FJsonObject>* PropertyObjPtr = nullptr;
			if (!PropertyValue.IsValid() || !PropertyValue->TryGetObject(PropertyObjPtr) || !PropertyObjPtr || !PropertyObjPtr->IsValid())
			{
				continue;
			}

			FString CandidateName;
			if ((*PropertyObjPtr)->TryGetStringField(TEXT("property_name"), CandidateName) && CandidateName == PropertyName)
			{
				OutPropertyObj = *PropertyObjPtr;
				return true;
			}
		}

		return false;
	};

	auto FindBlendSpacePlayerNode = [](UAnimBlueprint* AnimBlueprint) -> UAnimGraphNode_BlendSpacePlayer*
	{
		if (!AnimBlueprint)
		{
			return nullptr;
		}

		TArray<UEdGraph*> AllGraphs;
		AnimBlueprint->GetAllGraphs(AllGraphs);
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph)
			{
				continue;
			}

			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UAnimGraphNode_BlendSpacePlayer* BlendSpaceNode = Cast<UAnimGraphNode_BlendSpacePlayer>(Node))
				{
					return BlendSpaceNode;
				}
			}
		}

		return nullptr;
	};

	const FString CreateBody = MakeExecRequestBody(
		TEXT("smoke-animbp-folder-create-001"),
		TEXT("anim_blueprint_create"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"parent_class\":\"/Script/Engine.AnimInstance\",\"target_skeleton\":\"%s\",\"preview_skeletal_mesh\":\"%s\",\"compile_after_create\":false,\"open_editor\":false,\"save_after_create\":false}"),
			*AnimBlueprintAssetPath,
			*SkeletonObjectPath,
			*SkeletalMeshObjectPath));
	FHttpSmokeResult CreateResult;
	TestTrue(TEXT("AnimBlueprint folder create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateBody, CreateResult));
	TestEqual(TEXT("AnimBlueprint folder create status code"), CreateResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CreateJson;
	TestTrue(TEXT("AnimBlueprint folder create parses JSON"), ParseJson(CreateResult.Body, CreateJson));
	TestTrue(TEXT("AnimBlueprint folder create root ok=true"), CreateJson.IsValid() && CreateJson->GetBoolField(TEXT("ok")));

	FString AnimBlueprintObjectPath;
	const TSharedPtr<FJsonObject>* CreateData = nullptr;
	TestTrue(TEXT("AnimBlueprint folder create contains data object"), CreateJson.IsValid() && CreateJson->TryGetObjectField(TEXT("data"), CreateData) && CreateData && CreateData->IsValid());
	if (CreateData && CreateData->IsValid())
	{
		AnimBlueprintObjectPath = (*CreateData)->GetStringField(TEXT("object_path"));
	}

	UAnimBlueprint* EditableAnimBlueprint = AnimBlueprintObjectPath.IsEmpty() ? nullptr : LoadObject<UAnimBlueprint>(nullptr, *AnimBlueprintObjectPath);
	TestNotNull(TEXT("AnimBlueprint folder smoke local asset loads"), EditableAnimBlueprint);
	UBlendSpace* BlendSpaceAsset = LoadObject<UBlendSpace>(nullptr, *BlendSpaceObjectPath);
	TestNotNull(TEXT("AnimBlueprint folder smoke BlendSpace asset loads"), BlendSpaceAsset);
	if (EditableAnimBlueprint && BlendSpaceAsset)
	{
		UEdGraph* DefaultAnimGraph = nullptr;
		for (UEdGraph* Graph : EditableAnimBlueprint->FunctionGraphs)
		{
			if (Graph && AnimationEditorUtils::IsAnimGraph(Graph) && Graph->GetFName() == UEdGraphSchema_K2::GN_AnimGraph)
			{
				DefaultAnimGraph = Graph;
				break;
			}
		}

		TestNotNull(TEXT("AnimBlueprint folder smoke default anim graph exists"), DefaultAnimGraph);
		UAnimGraphNode_Root* RootNode = nullptr;
		if (DefaultAnimGraph)
		{
			for (UEdGraphNode* Node : DefaultAnimGraph->Nodes)
			{
				if (UAnimGraphNode_Root* CandidateRoot = Cast<UAnimGraphNode_Root>(Node))
				{
					RootNode = CandidateRoot;
					break;
				}
			}
		}
		TestNotNull(TEXT("AnimBlueprint folder smoke root node exists"), RootNode);

		UAnimGraphNode_BlendSpacePlayer* BlendSpaceNode = DefaultAnimGraph ? AddAutomationNodeToGraph<UAnimGraphNode_BlendSpacePlayer>(DefaultAnimGraph) : nullptr;
		TestNotNull(TEXT("AnimBlueprint folder smoke blendspace player node created"), BlendSpaceNode);
		if (BlendSpaceNode)
		{
			BlendSpaceNode->Node.SetBlendSpace(BlendSpaceAsset);
			BlendSpaceNode->Node.SetPosition(FVector(25.0f, 0.0f, 0.0f));
			BlendSpaceNode->NodePosX = 240;
			BlendSpaceNode->NodePosY = 120;
		}

		if (BlendSpaceNode && RootNode)
		{
			UEdGraphPin* OutputPin = nullptr;
			UEdGraphPin* InputPin = nullptr;
			for (UEdGraphPin* Pin : BlendSpaceNode->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Output)
				{
					OutputPin = Pin;
					break;
				}
			}
			for (UEdGraphPin* Pin : RootNode->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Input)
				{
					InputPin = Pin;
					break;
				}
			}

			TestNotNull(TEXT("AnimBlueprint folder smoke blendspace output pin exists"), OutputPin);
			TestNotNull(TEXT("AnimBlueprint folder smoke root input pin exists"), InputPin);
			if (OutputPin && InputPin)
			{
				if (const UEdGraphSchema* Schema = DefaultAnimGraph->GetSchema())
				{
					Schema->TryCreateConnection(OutputPin, InputPin);
				}
				else
				{
					OutputPin->MakeLinkTo(InputPin);
				}
			}
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(EditableAnimBlueprint);
		FKismetEditorUtilities::CompileBlueprint(EditableAnimBlueprint);
	}

	const FString ExportBody = MakeExecRequestBody(
		TEXT("smoke-animbp-folder-export-001"),
		TEXT("anim_blueprint_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"clean_output_dir\":true,\"include_validation\":false}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult ExportResult;
	TestTrue(TEXT("AnimBlueprint folder export request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ExportBody, ExportResult));
	TestEqual(TEXT("AnimBlueprint folder export status code"), ExportResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ExportJson;
	TestTrue(TEXT("AnimBlueprint folder export parses JSON"), ParseJson(ExportResult.Body, ExportJson));
	TestTrue(TEXT("AnimBlueprint folder export root ok=true"), ExportJson.IsValid() && ExportJson->GetBoolField(TEXT("ok")));

	const FString AnimGraphJsonPath = FPaths::Combine(FolderPath, TEXT("graphs"), TEXT("AnimLayer_AnimGraph.json"));
	TSharedPtr<FJsonObject> AnimGraphJson;
	TestTrue(TEXT("AnimBlueprint folder exported AnimGraph json loads"), LoadJsonFile(AnimGraphJsonPath, AnimGraphJson));

	TSharedPtr<FJsonObject> BlendSpaceNodeJson;
	TestTrue(
		TEXT("AnimBlueprint folder exported graph contains BlendSpace player node"),
		FindNodeObjectByClass(AnimGraphJson, TEXT("/Script/AnimGraph.AnimGraphNode_BlendSpacePlayer"), BlendSpaceNodeJson));

	TSharedPtr<FJsonObject> BlendSpacePropertyJson;
	FString OriginalBlendSpaceValueText;
	TestTrue(
		TEXT("AnimBlueprint folder exported BlendSpace property exists"),
		FindPropertyObject(BlendSpaceNodeJson, TEXT("Node.BlendSpace"), BlendSpacePropertyJson));
	if (BlendSpacePropertyJson.IsValid())
	{
		OriginalBlendSpaceValueText = BlendSpacePropertyJson->GetStringField(TEXT("value_text"));
		TestTrue(TEXT("AnimBlueprint folder exported BlendSpace property references asset"), OriginalBlendSpaceValueText.Contains(TEXT("NewBlendSpace1D")));
		BlendSpacePropertyJson->SetStringField(TEXT("value_text"), TEXT("None"));
	}

	TestTrue(TEXT("AnimBlueprint folder mutated AnimGraph json saves"), SaveJsonFile(AnimGraphJsonPath, AnimGraphJson));

	const FString ApplyNoneBody = MakeExecRequestBody(
		TEXT("smoke-animbp-folder-apply-001"),
		TEXT("anim_blueprint_apply_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"create_if_missing\":false,\"compile_after_apply\":false,\"save_after_apply\":false}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult ApplyNoneResult;
	TestTrue(TEXT("AnimBlueprint folder apply request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ApplyNoneBody, ApplyNoneResult));
	TestEqual(TEXT("AnimBlueprint folder apply status code"), ApplyNoneResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ApplyNoneJson;
	TestTrue(TEXT("AnimBlueprint folder apply parses JSON"), ParseJson(ApplyNoneResult.Body, ApplyNoneJson));
	TestTrue(TEXT("AnimBlueprint folder apply root ok=true"), ApplyNoneJson.IsValid() && ApplyNoneJson->GetBoolField(TEXT("ok")));

	UAnimBlueprint* AppliedNoneAnimBlueprint = LoadObject<UAnimBlueprint>(nullptr, *AnimBlueprintObjectPath);
	TestNotNull(TEXT("AnimBlueprint folder smoke reload after apply none"), AppliedNoneAnimBlueprint);
	if (UAnimGraphNode_BlendSpacePlayer* BlendSpaceNodeAfterNone = FindBlendSpacePlayerNode(AppliedNoneAnimBlueprint))
	{
		TestTrue(TEXT("AnimBlueprint folder apply sets BlendSpace to None"), BlendSpaceNodeAfterNone->Node.GetBlendSpace() == nullptr);
	}
	else
	{
		AddError(TEXT("AnimBlueprint folder apply could not find BlendSpace player node after apply."));
	}

	if (BlendSpacePropertyJson.IsValid())
	{
		BlendSpacePropertyJson->SetStringField(TEXT("value_text"), OriginalBlendSpaceValueText);
	}
	TestTrue(TEXT("AnimBlueprint folder restored AnimGraph json saves"), SaveJsonFile(AnimGraphJsonPath, AnimGraphJson));

	const FString ApplyRestoreBody = MakeExecRequestBody(
		TEXT("smoke-animbp-folder-apply-002"),
		TEXT("anim_blueprint_apply_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"create_if_missing\":false,\"compile_after_apply\":true,\"save_after_apply\":false}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult ApplyRestoreResult;
	TestTrue(TEXT("AnimBlueprint folder apply restore request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ApplyRestoreBody, ApplyRestoreResult));
	TestEqual(TEXT("AnimBlueprint folder apply restore status code"), ApplyRestoreResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ApplyRestoreJson;
	TestTrue(TEXT("AnimBlueprint folder apply restore parses JSON"), ParseJson(ApplyRestoreResult.Body, ApplyRestoreJson));
	TestTrue(TEXT("AnimBlueprint folder apply restore root ok=true"), ApplyRestoreJson.IsValid() && ApplyRestoreJson->GetBoolField(TEXT("ok")));

	UAnimBlueprint* AppliedRestoreAnimBlueprint = LoadObject<UAnimBlueprint>(nullptr, *AnimBlueprintObjectPath);
	TestNotNull(TEXT("AnimBlueprint folder smoke reload after restore"), AppliedRestoreAnimBlueprint);
	if (UAnimGraphNode_BlendSpacePlayer* BlendSpaceNodeAfterRestore = FindBlendSpacePlayerNode(AppliedRestoreAnimBlueprint))
	{
		const UBlendSpace* RestoredBlendSpace = BlendSpaceNodeAfterRestore->Node.GetBlendSpace();
		TestNotNull(TEXT("AnimBlueprint folder apply restores BlendSpace asset"), RestoredBlendSpace);
		if (RestoredBlendSpace)
		{
			TestEqual(TEXT("AnimBlueprint folder restored BlendSpace asset path matches"), RestoredBlendSpace->GetPathName(), BlendSpaceObjectPath);
		}
	}
	else
	{
		AddError(TEXT("AnimBlueprint folder restore could not find BlendSpace player node after apply."));
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceAnimationAssetsSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.AnimationAssetsWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceAnimationAssetsSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString SourceAnimSequencePath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Idle");
	const FString SourceSkeletonPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton");
	const FString SourceSkeletalMeshPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP");
	const FString AnimMetadataClassPath = TEXT("/Script/UeAgentInterfaceTests.UeAgentInterfaceTestAnimMetaData");
	const FString BoneCompressionSettingsPath = TEXT("/Engine/Animation/DefaultRecorderBoneCompression");
	const FString CurveCompressionSettingsPath = TEXT("/Engine/Animation/DefaultAnimCurveCompressionSettings");
	const FString VariableFrameStrippingSettingsPath = TEXT("/Engine/Animation/DefaultVariableFrameStrippingSettings");
	const FString SequenceAssetPath = MakeAutomationAssetPath(TEXT("AnimSeq"));
	const FString SkeletonAssetPath = MakeAutomationAssetPath(TEXT("Skeleton"));
	const FString NotifyTrackName = TEXT("Stage20Markers");
	const FString NotifyName = TEXT("Stage22Notify");
	const FString FloatCurveName = TEXT("Stage23FloatCurve");
	const FString MarkerA = TEXT("Stage20MarkerA");
	const FString MarkerB = TEXT("Stage20MarkerB");
	const FString SocketName = TEXT("Stage20Socket");
	FString VirtualBoneName;

	auto FindNotifyTrack = [](const TSharedPtr<FJsonObject>* InfoData, const FString& TrackName, TSharedPtr<FJsonObject>& OutTrackObject) -> bool
	{
		if (!InfoData || !InfoData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Tracks = nullptr;
		if (!(*InfoData)->TryGetArrayField(TEXT("notify_tracks"), Tracks) || !Tracks)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& TrackValue : *Tracks)
		{
			const TSharedPtr<FJsonObject>* TrackObject = nullptr;
			if (!TrackValue.IsValid() || !TrackValue->TryGetObject(TrackObject) || !TrackObject || !TrackObject->IsValid())
			{
				continue;
			}
			if ((*TrackObject)->GetStringField(TEXT("track_name")) == TrackName)
			{
				OutTrackObject = *TrackObject;
				return true;
			}
		}

		return false;
	};

	auto FindNotifyNamed = [](const TSharedPtr<FJsonObject>* InfoData, const FString& InNotifyName, TSharedPtr<FJsonObject>& OutNotifyObject) -> bool
	{
		if (!InfoData || !InfoData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Notifies = nullptr;
		if (!(*InfoData)->TryGetArrayField(TEXT("notifies"), Notifies) || !Notifies)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& NotifyValue : *Notifies)
		{
			const TSharedPtr<FJsonObject>* NotifyObject = nullptr;
			if (!NotifyValue.IsValid() || !NotifyValue->TryGetObject(NotifyObject) || !NotifyObject || !NotifyObject->IsValid())
			{
				continue;
			}
			if ((*NotifyObject)->GetStringField(TEXT("notify_name")) == InNotifyName)
			{
				OutNotifyObject = *NotifyObject;
				return true;
			}
		}
		return false;
	};

	auto FindCurveNamed = [](const TSharedPtr<FJsonObject>* InfoData, const FString& InCurveName, TSharedPtr<FJsonObject>& OutCurveObject) -> bool
	{
		if (!InfoData || !InfoData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Curves = nullptr;
		if (!(*InfoData)->TryGetArrayField(TEXT("curves"), Curves) || !Curves)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& CurveValue : *Curves)
		{
			const TSharedPtr<FJsonObject>* CurveObject = nullptr;
			if (!CurveValue.IsValid() || !CurveValue->TryGetObject(CurveObject) || !CurveObject || !CurveObject->IsValid())
			{
				continue;
			}
			if ((*CurveObject)->GetStringField(TEXT("curve_name")) == InCurveName)
			{
				OutCurveObject = *CurveObject;
				return true;
			}
		}
		return false;
	};

	auto GetTrackNames = [](const TSharedPtr<FJsonObject>* InfoData, TArray<FString>& OutTrackNames) -> bool
	{
		OutTrackNames.Reset();
		if (!InfoData || !InfoData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* TrackNames = nullptr;
		if (!(*InfoData)->TryGetArrayField(TEXT("track_names"), TrackNames) || !TrackNames)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *TrackNames)
		{
			if (Value.IsValid())
			{
				OutTrackNames.Add(Value->AsString());
			}
		}
		return true;
	};

	auto HasMetadataClass = [](const TSharedPtr<FJsonObject>* InfoData, const FString& InClassPath) -> bool
	{
		if (!InfoData || !InfoData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* MetaDataArray = nullptr;
		if (!(*InfoData)->TryGetArrayField(TEXT("metadata"), MetaDataArray) || !MetaDataArray)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& MetaValue : *MetaDataArray)
		{
			const TSharedPtr<FJsonObject>* MetaObject = nullptr;
			if (!MetaValue.IsValid() || !MetaValue->TryGetObject(MetaObject) || !MetaObject || !MetaObject->IsValid())
			{
				continue;
			}
			if ((*MetaObject)->GetStringField(TEXT("class")) == InClassPath)
			{
				return true;
			}
		}
		return false;
	};

	auto FindMetadataByClass = [](const TSharedPtr<FJsonObject>* InfoData, const FString& InClassPath, TSharedPtr<FJsonObject>& OutMetadataObject) -> bool
	{
		if (!InfoData || !InfoData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* MetaDataArray = nullptr;
		if (!(*InfoData)->TryGetArrayField(TEXT("metadata"), MetaDataArray) || !MetaDataArray)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& MetaValue : *MetaDataArray)
		{
			const TSharedPtr<FJsonObject>* MetaObject = nullptr;
			if (!MetaValue.IsValid() || !MetaValue->TryGetObject(MetaObject) || !MetaObject || !MetaObject->IsValid())
			{
				continue;
			}
			if ((*MetaObject)->GetStringField(TEXT("class")) == InClassPath)
			{
				OutMetadataObject = *MetaObject;
				return true;
			}
		}
		return false;
	};

	auto HasMarkerNamed = [](const TSharedPtr<FJsonObject>* InfoData, const FString& MarkerName) -> bool
	{
		if (!InfoData || !InfoData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Markers = nullptr;
		if (!(*InfoData)->TryGetArrayField(TEXT("sync_markers"), Markers) || !Markers)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& MarkerValue : *Markers)
		{
			const TSharedPtr<FJsonObject>* MarkerObject = nullptr;
			if (!MarkerValue.IsValid() || !MarkerValue->TryGetObject(MarkerObject) || !MarkerObject || !MarkerObject->IsValid())
			{
				continue;
			}
			if ((*MarkerObject)->GetStringField(TEXT("marker_name")) == MarkerName)
			{
				return true;
			}
		}
		return false;
	};

	auto FindSocketNamed = [](const TSharedPtr<FJsonObject>* InfoData, const FString& InSocketName, TSharedPtr<FJsonObject>& OutSocketObject) -> bool
	{
		if (!InfoData || !InfoData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Sockets = nullptr;
		if (!(*InfoData)->TryGetArrayField(TEXT("sockets"), Sockets) || !Sockets)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& SocketValue : *Sockets)
		{
			const TSharedPtr<FJsonObject>* SocketObject = nullptr;
			if (!SocketValue.IsValid() || !SocketValue->TryGetObject(SocketObject) || !SocketObject || !SocketObject->IsValid())
			{
				continue;
			}
			if ((*SocketObject)->GetStringField(TEXT("socket_name")) == InSocketName)
			{
				OutSocketObject = *SocketObject;
				return true;
			}
		}
		return false;
	};

	auto FindVirtualBoneNamed = [](const TSharedPtr<FJsonObject>* InfoData, const FString& InVirtualBoneName) -> bool
	{
		if (!InfoData || !InfoData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* VirtualBones = nullptr;
		if (!(*InfoData)->TryGetArrayField(TEXT("virtual_bones"), VirtualBones) || !VirtualBones)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& VirtualBoneValue : *VirtualBones)
		{
			const TSharedPtr<FJsonObject>* VirtualBoneObject = nullptr;
			if (!VirtualBoneValue.IsValid() || !VirtualBoneValue->TryGetObject(VirtualBoneObject) || !VirtualBoneObject || !VirtualBoneObject->IsValid())
			{
				continue;
			}
			if ((*VirtualBoneObject)->GetStringField(TEXT("virtual_bone_name")) == InVirtualBoneName)
			{
				return true;
			}
		}
		return false;
	};

	const FString DuplicateSequenceBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-dup-seq-001"),
		TEXT("asset_duplicate"),
		FString::Printf(TEXT("{\"source_asset_path\":\"%s\",\"destination_asset_path\":\"%s\",\"save_after_duplicate\":false}"), *SourceAnimSequencePath, *SequenceAssetPath));
	FHttpSmokeResult DuplicateSequenceResult;
	TestTrue(TEXT("Animation assets duplicate sequence request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DuplicateSequenceBody, DuplicateSequenceResult));
	TestEqual(TEXT("Animation assets duplicate sequence status code"), DuplicateSequenceResult.StatusCode, 200);

	const FString DuplicateSkeletonBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-dup-skeleton-001"),
		TEXT("asset_duplicate"),
		FString::Printf(TEXT("{\"source_asset_path\":\"%s\",\"destination_asset_path\":\"%s\",\"save_after_duplicate\":false}"), *SourceSkeletonPath, *SkeletonAssetPath));
	FHttpSmokeResult DuplicateSkeletonResult;
	TestTrue(TEXT("Animation assets duplicate skeleton request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DuplicateSkeletonBody, DuplicateSkeletonResult));
	TestEqual(TEXT("Animation assets duplicate skeleton status code"), DuplicateSkeletonResult.StatusCode, 200);

	const FString SequenceInfoBeforeBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-seq-info-before-001"),
		TEXT("anim_sequence_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *SequenceAssetPath));
	FHttpSmokeResult SequenceInfoBeforeResult;
	TestTrue(TEXT("Animation assets anim_sequence_get_info before request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SequenceInfoBeforeBody, SequenceInfoBeforeResult));
	TestEqual(TEXT("Animation assets anim_sequence_get_info before status code"), SequenceInfoBeforeResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SequenceInfoBeforeJson;
	TestTrue(TEXT("Animation assets anim_sequence_get_info before parses JSON"), ParseJson(SequenceInfoBeforeResult.Body, SequenceInfoBeforeJson));
	TestTrue(TEXT("Animation assets anim_sequence_get_info before root ok=true"), SequenceInfoBeforeJson.IsValid() && SequenceInfoBeforeJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* SequenceInfoBeforeData = nullptr;
	TestTrue(TEXT("Animation assets anim_sequence_get_info before contains data object"), SequenceInfoBeforeJson.IsValid() && SequenceInfoBeforeJson->TryGetObjectField(TEXT("data"), SequenceInfoBeforeData) && SequenceInfoBeforeData && SequenceInfoBeforeData->IsValid());
	if (SequenceInfoBeforeData && SequenceInfoBeforeData->IsValid())
	{
		TestTrue(TEXT("Animation assets sequence length > 0"), (*SequenceInfoBeforeData)->GetNumberField(TEXT("sequence_length")) > 0.0);
		TestTrue(TEXT("Animation assets sequence frames > 0"), static_cast<int32>((*SequenceInfoBeforeData)->GetIntegerField(TEXT("num_frames"))) > 0);
	}

	const FString RootMotionAnalysisBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-root-motion-analysis-001"),
		TEXT("anim_sequence_analyze_root_motion"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"sample_count\":5,\"include_samples\":false,\"include_suggested_keys\":true}"), *SequenceAssetPath));
	FHttpSmokeResult RootMotionAnalysisResult;
	TestTrue(TEXT("Animation assets anim_sequence_analyze_root_motion request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RootMotionAnalysisBody, RootMotionAnalysisResult));
	TestEqual(TEXT("Animation assets anim_sequence_analyze_root_motion status code"), RootMotionAnalysisResult.StatusCode, 200);

	TSharedPtr<FJsonObject> RootMotionAnalysisJson;
	TestTrue(TEXT("Animation assets anim_sequence_analyze_root_motion parses JSON"), ParseJson(RootMotionAnalysisResult.Body, RootMotionAnalysisJson));
	const TSharedPtr<FJsonObject>* RootMotionAnalysisData = nullptr;
	TestTrue(TEXT("Animation assets anim_sequence_analyze_root_motion contains data object"), RootMotionAnalysisJson.IsValid() && RootMotionAnalysisJson->TryGetObjectField(TEXT("data"), RootMotionAnalysisData) && RootMotionAnalysisData && RootMotionAnalysisData->IsValid());
	if (RootMotionAnalysisData && RootMotionAnalysisData->IsValid())
	{
		TestTrue(TEXT("Animation assets root motion analysis length > 0"), (*RootMotionAnalysisData)->GetNumberField(TEXT("sequence_length")) > 0.0);
		TestEqual(TEXT("Animation assets root motion analysis sample count"), static_cast<int32>((*RootMotionAnalysisData)->GetIntegerField(TEXT("sample_count"))), 5);
		const TArray<TSharedPtr<FJsonValue>>* SuggestedMarkers = nullptr;
		TestTrue(TEXT("Animation assets root motion analysis suggested markers exists"), (*RootMotionAnalysisData)->TryGetArrayField(TEXT("suggested_markers"), SuggestedMarkers) && SuggestedMarkers != nullptr && SuggestedMarkers->Num() == 3);
		const TArray<TSharedPtr<FJsonValue>>* SuggestedTurnYawKeys = nullptr;
		TestTrue(TEXT("Animation assets root motion analysis suggested yaw keys exist"), (*RootMotionAnalysisData)->TryGetArrayField(TEXT("suggested_turn_yaw_keys"), SuggestedTurnYawKeys) && SuggestedTurnYawKeys != nullptr && SuggestedTurnYawKeys->Num() == 5);
	}

	TArray<FString> InitialTrackNames;
	TestTrue(TEXT("Animation assets initial track names exist"), GetTrackNames(SequenceInfoBeforeData, InitialTrackNames));
	FString TrackToRemove;
	for (const FString& TrackName : InitialTrackNames)
	{
		if (!TrackName.Equals(TEXT("Root"), ESearchCase::IgnoreCase))
		{
			TrackToRemove = TrackName;
			break;
		}
	}
	if (TrackToRemove.IsEmpty() && InitialTrackNames.Num() > 0)
	{
		TrackToRemove = InitialTrackNames[0];
	}
	TestTrue(TEXT("Animation assets selected bone track to remove is non-empty"), !TrackToRemove.IsEmpty());

	const FString SetSequenceSettingsBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-seq-settings-001"),
		TEXT("anim_sequence_set_settings"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"rate_scale\":1.25,\"enable_root_motion\":true,\"force_root_lock\":true,\"root_motion_lock_type\":\"zero\",\"interpolation_type\":\"step\",\"additive_base_pose_type\":\"anim_frame\",\"base_pose_sequence_path\":\"%s\",\"base_pose_frame_index\":3,\"retarget_source_asset_path\":\"%s\",\"bone_compression_settings_path\":\"%s\",\"curve_compression_settings_path\":\"%s\",\"variable_frame_stripping_settings_path\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *SourceAnimSequencePath, *SourceSkeletalMeshPath, *BoneCompressionSettingsPath, *CurveCompressionSettingsPath, *VariableFrameStrippingSettingsPath));
	FHttpSmokeResult SetSequenceSettingsResult;
	TestTrue(TEXT("Animation assets anim_sequence_set_settings request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetSequenceSettingsBody, SetSequenceSettingsResult));
	TestEqual(TEXT("Animation assets anim_sequence_set_settings status code"), SetSequenceSettingsResult.StatusCode, 200);

	const FString SetSequencePreviewMeshBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-seq-preview-001"),
		TEXT("anim_sequence_set_preview_mesh"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"skeletal_mesh_path\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *SourceSkeletalMeshPath));
	FHttpSmokeResult SetSequencePreviewMeshResult;
	TestTrue(TEXT("Animation assets anim_sequence_set_preview_mesh request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetSequencePreviewMeshBody, SetSequencePreviewMeshResult));
	TestEqual(TEXT("Animation assets anim_sequence_set_preview_mesh status code"), SetSequencePreviewMeshResult.StatusCode, 200);

	const FString SetNotifyTrackBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-notify-track-001"),
		TEXT("anim_sequence_set_notify_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"track_name\":\"%s\",\"track_color\":{\"r\":0.25,\"g\":0.45,\"b\":0.80,\"a\":1.0},\"save_after_set\":false}"), *SequenceAssetPath, *NotifyTrackName));
	FHttpSmokeResult SetNotifyTrackResult;
	TestTrue(TEXT("Animation assets anim_sequence_set_notify_track request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetNotifyTrackBody, SetNotifyTrackResult));
	TestEqual(TEXT("Animation assets anim_sequence_set_notify_track status code"), SetNotifyTrackResult.StatusCode, 200);

	const FString AddNotifyBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-notify-001"),
		TEXT("anim_sequence_set_notify"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"track_name\":\"%s\",\"time_seconds\":0.02,\"notify_name\":\"%s\",\"trigger_weight_threshold\":0.25,\"notify_trigger_chance\":1.0,\"notify_filter_type\":\"lod\",\"notify_filter_lod\":1,\"notify_color\":{\"r\":0.9,\"g\":0.3,\"b\":0.2,\"a\":1.0},\"save_after_set\":false}"), *SequenceAssetPath, *NotifyTrackName, *NotifyName));
	FHttpSmokeResult AddNotifyResult;
	TestTrue(TEXT("Animation assets anim_sequence_set_notify add request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddNotifyBody, AddNotifyResult));
	TestEqual(TEXT("Animation assets anim_sequence_set_notify add status code"), AddNotifyResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddNotifyJson;
	TestTrue(TEXT("Animation assets anim_sequence_set_notify add parses JSON"), ParseJson(AddNotifyResult.Body, AddNotifyJson));
	const TSharedPtr<FJsonObject>* AddNotifyData = nullptr;
	TestTrue(TEXT("Animation assets anim_sequence_set_notify add contains data object"), AddNotifyJson.IsValid() && AddNotifyJson->TryGetObjectField(TEXT("data"), AddNotifyData) && AddNotifyData && AddNotifyData->IsValid());
	int32 NotifyIndex = INDEX_NONE;
	if (AddNotifyData && AddNotifyData->IsValid())
	{
		const TSharedPtr<FJsonObject>* NotifyObject = nullptr;
		TestTrue(TEXT("Animation assets anim_sequence_set_notify add contains notify object"), (*AddNotifyData)->TryGetObjectField(TEXT("notify"), NotifyObject) && NotifyObject && NotifyObject->IsValid());
		if (NotifyObject && NotifyObject->IsValid())
		{
			NotifyIndex = static_cast<int32>((*NotifyObject)->GetIntegerField(TEXT("notify_index")));
			TestEqual(TEXT("Animation assets notify add name"), (*NotifyObject)->GetStringField(TEXT("notify_name")), NotifyName);
		}
	}

	const FString UpdateNotifyBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-notify-002"),
		TEXT("anim_sequence_set_notify"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"notify_index\":%d,\"time_seconds\":0.04,\"notify_color\":{\"r\":0.15,\"g\":0.65,\"b\":0.95,\"a\":1.0},\"save_after_set\":false}"), *SequenceAssetPath, NotifyIndex));
	FHttpSmokeResult UpdateNotifyResult;
	TestTrue(TEXT("Animation assets anim_sequence_set_notify update request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, UpdateNotifyBody, UpdateNotifyResult));
	TestEqual(TEXT("Animation assets anim_sequence_set_notify update status code"), UpdateNotifyResult.StatusCode, 200);

	const FString AddFloatCurveBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-curve-001"),
		TEXT("anim_sequence_set_curve"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"curve_name\":\"%s\",\"curve_type\":\"float\",\"clear_existing_keys\":true,\"keys\":[{\"time_seconds\":0.02,\"value\":1.0},{\"time_seconds\":0.04,\"value\":2.0}],\"save_after_set\":false}"), *SequenceAssetPath, *FloatCurveName));
	FHttpSmokeResult AddFloatCurveResult;
	TestTrue(TEXT("Animation assets anim_sequence_set_curve float request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddFloatCurveBody, AddFloatCurveResult));
	TestEqual(TEXT("Animation assets anim_sequence_set_curve float status code"), AddFloatCurveResult.StatusCode, 200);

	const FString ApplyBoneTracksBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-apply-bone-tracks-001"),
		TEXT("anim_sequence_apply_bone_tracks"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"frame_rate\":{\"numerator\":30,\"denominator\":1},\"num_frames\":2,\"tracks\":[{\"bone\":\"%s\",\"keys\":[{\"frame\":0,\"rotation\":{\"pitch\":0,\"yaw\":0,\"roll\":0}},{\"frame\":1,\"rotation\":{\"pitch\":0,\"yaw\":20,\"roll\":0}},{\"frame\":2,\"rotation\":{\"pitch\":0,\"yaw\":0,\"roll\":0}}]}],\"save_after_apply\":false}"), *SequenceAssetPath, *TrackToRemove));
	FHttpSmokeResult ApplyBoneTracksResult;
	TestTrue(TEXT("Animation assets anim_sequence_apply_bone_tracks request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ApplyBoneTracksBody, ApplyBoneTracksResult));
	TestEqual(TEXT("Animation assets anim_sequence_apply_bone_tracks status code"), ApplyBoneTracksResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ApplyBoneTracksJson;
	TestTrue(TEXT("Animation assets anim_sequence_apply_bone_tracks parses JSON"), ParseJson(ApplyBoneTracksResult.Body, ApplyBoneTracksJson));
	const TSharedPtr<FJsonObject>* ApplyBoneTracksData = nullptr;
	TestTrue(TEXT("Animation assets anim_sequence_apply_bone_tracks contains data object"), ApplyBoneTracksJson.IsValid() && ApplyBoneTracksJson->TryGetObjectField(TEXT("data"), ApplyBoneTracksData) && ApplyBoneTracksData && ApplyBoneTracksData->IsValid());
	if (ApplyBoneTracksData && ApplyBoneTracksData->IsValid())
	{
		TestEqual(TEXT("Animation assets applied bone track count"), static_cast<int32>((*ApplyBoneTracksData)->GetIntegerField(TEXT("applied_track_count"))), 1);
		TestTrue(TEXT("Animation assets applied bone track output track count > 0"), static_cast<int32>((*ApplyBoneTracksData)->GetIntegerField(TEXT("track_count"))) > 0);
		TestEqual(TEXT("Animation assets applied bone track sample key count"), static_cast<int32>((*ApplyBoneTracksData)->GetIntegerField(TEXT("sample_key_count"))), 3);
	}

	const FString AddMetadataBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-metadata-001"),
		TEXT("anim_sequence_set_metadata"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"metadata_class_path\":\"%s\",\"metadata_values\":{\"tag\":\"Stage26Tag\",\"note\":\"Stage26Note\"},\"save_after_set\":false}"), *SequenceAssetPath, *AnimMetadataClassPath));
	FHttpSmokeResult AddMetadataResult;
	TestTrue(TEXT("Animation assets anim_sequence_set_metadata add request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddMetadataBody, AddMetadataResult));
	TestEqual(TEXT("Animation assets anim_sequence_set_metadata add status code"), AddMetadataResult.StatusCode, 200);

	const FString UpdateMetadataBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-metadata-001b"),
		TEXT("anim_sequence_set_metadata"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"metadata_class_path\":\"%s\",\"metadata_values\":{\"tag\":\"Stage26Tag\",\"note\":\"Stage26NoteUpdated\"},\"save_after_set\":false}"), *SequenceAssetPath, *AnimMetadataClassPath));
	FHttpSmokeResult UpdateMetadataResult;
	TestTrue(TEXT("Animation assets anim_sequence_set_metadata update request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, UpdateMetadataBody, UpdateMetadataResult));
	TestEqual(TEXT("Animation assets anim_sequence_set_metadata update status code"), UpdateMetadataResult.StatusCode, 200);

	const FString RemoveBoneTrackBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-bones-001"),
		TEXT("anim_sequence_set_bones"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"remove_bone_names\":[\"%s\"],\"include_children\":false,\"finalize_after_set\":true,\"save_after_set\":false}"), *SequenceAssetPath, *TrackToRemove));
	FHttpSmokeResult RemoveBoneTrackResult;
	TestTrue(TEXT("Animation assets anim_sequence_set_bones request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveBoneTrackBody, RemoveBoneTrackResult));
	TestEqual(TEXT("Animation assets anim_sequence_set_bones status code"), RemoveBoneTrackResult.StatusCode, 200);

	const FString SetSyncMarkersBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-sync-markers-001"),
		TEXT("anim_sequence_set_sync_markers"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"add_markers\":[{\"marker_name\":\"%s\",\"time_seconds\":0.02,\"notify_track_name\":\"%s\"},{\"marker_name\":\"%s\",\"time_seconds\":0.04,\"notify_track_name\":\"%s\"}],\"save_after_set\":false}"), *SequenceAssetPath, *MarkerA, *NotifyTrackName, *MarkerB, *NotifyTrackName));
	FHttpSmokeResult SetSyncMarkersResult;
	TestTrue(TEXT("Animation assets anim_sequence_set_sync_markers add request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetSyncMarkersBody, SetSyncMarkersResult));
	TestEqual(TEXT("Animation assets anim_sequence_set_sync_markers add status code"), SetSyncMarkersResult.StatusCode, 200);

	const FString SequenceInfoAfterBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-seq-info-after-001"),
		TEXT("anim_sequence_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"include_curve_keys\":true}"), *SequenceAssetPath));
	FHttpSmokeResult SequenceInfoAfterResult;
	TestTrue(TEXT("Animation assets anim_sequence_get_info after request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SequenceInfoAfterBody, SequenceInfoAfterResult));
	TestEqual(TEXT("Animation assets anim_sequence_get_info after status code"), SequenceInfoAfterResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SequenceInfoAfterJson;
	TestTrue(TEXT("Animation assets anim_sequence_get_info after parses JSON"), ParseJson(SequenceInfoAfterResult.Body, SequenceInfoAfterJson));
	const TSharedPtr<FJsonObject>* SequenceInfoAfterData = nullptr;
	TestTrue(TEXT("Animation assets anim_sequence_get_info after contains data object"), SequenceInfoAfterJson.IsValid() && SequenceInfoAfterJson->TryGetObjectField(TEXT("data"), SequenceInfoAfterData) && SequenceInfoAfterData && SequenceInfoAfterData->IsValid());
	if (SequenceInfoAfterData && SequenceInfoAfterData->IsValid())
	{
		TestEqual(TEXT("Animation assets sequence preview mesh updated"), (*SequenceInfoAfterData)->GetStringField(TEXT("preview_skeletal_mesh")), SourceSkeletalMeshPath + TEXT(".TutorialTPP"));
		TestTrue(TEXT("Animation assets sequence rate scale updated"), FMath::IsNearlyEqual(static_cast<float>((*SequenceInfoAfterData)->GetNumberField(TEXT("rate_scale"))), 1.25f, 0.001f));
		TestTrue(TEXT("Animation assets sequence root motion enabled"), (*SequenceInfoAfterData)->GetBoolField(TEXT("root_motion_enabled")));
		TestTrue(TEXT("Animation assets sequence force root lock enabled"), (*SequenceInfoAfterData)->GetBoolField(TEXT("force_root_lock")));
		TestEqual(TEXT("Animation assets sequence root motion lock type updated"), (*SequenceInfoAfterData)->GetStringField(TEXT("root_motion_lock_type")), FString(TEXT("zero")));
		TestEqual(TEXT("Animation assets sequence interpolation updated"), (*SequenceInfoAfterData)->GetStringField(TEXT("interpolation_type")), FString(TEXT("step")));
		TestTrue(TEXT("Animation assets sequence base pose sequence updated"), (*SequenceInfoAfterData)->GetStringField(TEXT("additive_base_pose_sequence")).StartsWith(SourceAnimSequencePath));
		TestEqual(TEXT("Animation assets sequence base pose frame updated"), static_cast<int32>((*SequenceInfoAfterData)->GetIntegerField(TEXT("additive_base_pose_frame_index"))), 3);
		TestTrue(TEXT("Animation assets sequence retarget source asset updated"), (*SequenceInfoAfterData)->GetStringField(TEXT("retarget_source_asset")).StartsWith(SourceSkeletalMeshPath));
		TestTrue(TEXT("Animation assets sequence bone compression settings updated"), (*SequenceInfoAfterData)->GetStringField(TEXT("bone_compression_settings")).StartsWith(BoneCompressionSettingsPath));
		TestTrue(TEXT("Animation assets sequence curve compression settings updated"), (*SequenceInfoAfterData)->GetStringField(TEXT("curve_compression_settings")).StartsWith(CurveCompressionSettingsPath));
		TestTrue(TEXT("Animation assets sequence variable frame stripping settings updated"), (*SequenceInfoAfterData)->GetStringField(TEXT("variable_frame_stripping_settings")).StartsWith(VariableFrameStrippingSettingsPath));
		TArray<FString> TrackNamesAfter;
		TestTrue(TEXT("Animation assets track names after exist"), GetTrackNames(SequenceInfoAfterData, TrackNamesAfter));
		TestFalse(TEXT("Animation assets removed bone track absent after"), TrackNamesAfter.Contains(TrackToRemove));
		TestTrue(TEXT("Animation assets metadata class exists"), HasMetadataClass(SequenceInfoAfterData, AnimMetadataClassPath));
		TSharedPtr<FJsonObject> MetadataObject;
		TestTrue(TEXT("Animation assets metadata object exists"), FindMetadataByClass(SequenceInfoAfterData, AnimMetadataClassPath, MetadataObject));
		if (MetadataObject.IsValid())
		{
			const TSharedPtr<FJsonObject>* PropertiesObject = nullptr;
			TestTrue(TEXT("Animation assets metadata object has properties"), MetadataObject->TryGetObjectField(TEXT("properties"), PropertiesObject) && PropertiesObject && PropertiesObject->IsValid());
			if (PropertiesObject && PropertiesObject->IsValid())
			{
				TestEqual(TEXT("Animation assets metadata Tag property"), (*PropertiesObject)->GetStringField(TEXT("Tag")), FString(TEXT("Stage26Tag")));
				TestEqual(TEXT("Animation assets metadata Note property"), (*PropertiesObject)->GetStringField(TEXT("Note")), FString(TEXT("Stage26NoteUpdated")));
			}
		}
		TSharedPtr<FJsonObject> NotifyTrackObject;
		TestTrue(TEXT("Animation assets notify track exists"), FindNotifyTrack(SequenceInfoAfterData, NotifyTrackName, NotifyTrackObject));
		TSharedPtr<FJsonObject> NotifyObject;
		TestTrue(TEXT("Animation assets notify exists"), FindNotifyNamed(SequenceInfoAfterData, NotifyName, NotifyObject));
		if (NotifyObject.IsValid())
		{
			TestEqual(TEXT("Animation assets notify track name"), NotifyObject->GetStringField(TEXT("track_name")), NotifyTrackName);
			TestTrue(TEXT("Animation assets notify moved time updated"), FMath::IsNearlyEqual(static_cast<float>(NotifyObject->GetNumberField(TEXT("time_seconds"))), 0.04f, 0.001f));
		}
		TSharedPtr<FJsonObject> FloatCurveObject;
		TestTrue(TEXT("Animation assets float curve exists"), FindCurveNamed(SequenceInfoAfterData, FloatCurveName, FloatCurveObject));
		if (FloatCurveObject.IsValid())
		{
			TestEqual(TEXT("Animation assets float curve type"), FloatCurveObject->GetStringField(TEXT("curve_type")), FString(TEXT("float")));
			TestEqual(TEXT("Animation assets float curve key count"), static_cast<int32>(FloatCurveObject->GetIntegerField(TEXT("key_count"))), 2);
		}
		TestTrue(TEXT("Animation assets marker A exists"), HasMarkerNamed(SequenceInfoAfterData, MarkerA));
		TestTrue(TEXT("Animation assets marker B exists"), HasMarkerNamed(SequenceInfoAfterData, MarkerB));
	}

	const FString RemoveMarkersBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-sync-markers-002"),
		TEXT("anim_sequence_set_sync_markers"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"remove_marker_names\":[\"%s\"],\"remove_notify_track_names\":[\"%s\"],\"save_after_set\":false}"), *SequenceAssetPath, *MarkerA, *NotifyTrackName));
	FHttpSmokeResult RemoveMarkersResult;
	TestTrue(TEXT("Animation assets anim_sequence_set_sync_markers remove request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveMarkersBody, RemoveMarkersResult));
	TestEqual(TEXT("Animation assets anim_sequence_set_sync_markers remove status code"), RemoveMarkersResult.StatusCode, 200);

	const FString RemoveNotifyBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-notify-003"),
		TEXT("anim_sequence_set_notify"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"notify_index\":%d,\"remove\":true,\"save_after_set\":false}"), *SequenceAssetPath, NotifyIndex));
	FHttpSmokeResult RemoveNotifyResult;
	TestTrue(TEXT("Animation assets anim_sequence_set_notify remove request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveNotifyBody, RemoveNotifyResult));
	TestEqual(TEXT("Animation assets anim_sequence_set_notify remove status code"), RemoveNotifyResult.StatusCode, 200);

	const FString RemoveMetadataBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-metadata-002"),
		TEXT("anim_sequence_set_metadata"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"metadata_class_path\":\"%s\",\"remove\":true,\"save_after_set\":false}"), *SequenceAssetPath, *AnimMetadataClassPath));
	FHttpSmokeResult RemoveMetadataResult;
	TestTrue(TEXT("Animation assets anim_sequence_set_metadata remove request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveMetadataBody, RemoveMetadataResult));
	TestEqual(TEXT("Animation assets anim_sequence_set_metadata remove status code"), RemoveMetadataResult.StatusCode, 200);

	const FString RemoveNotifyTrackBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-notify-track-002"),
		TEXT("anim_sequence_set_notify_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"track_name\":\"%s\",\"remove\":true,\"save_after_set\":false}"), *SequenceAssetPath, *NotifyTrackName));
	FHttpSmokeResult RemoveNotifyTrackResult;
	TestTrue(TEXT("Animation assets anim_sequence_set_notify_track remove request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveNotifyTrackBody, RemoveNotifyTrackResult));
	TestEqual(TEXT("Animation assets anim_sequence_set_notify_track remove status code"), RemoveNotifyTrackResult.StatusCode, 200);

	const FString SkeletonBonesBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-skeleton-bones-001"),
		TEXT("skeleton_list_bones"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *SkeletonAssetPath));
	FHttpSmokeResult SkeletonBonesResult;
	TestTrue(TEXT("Animation assets skeleton_list_bones request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SkeletonBonesBody, SkeletonBonesResult));
	TestEqual(TEXT("Animation assets skeleton_list_bones status code"), SkeletonBonesResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SkeletonBonesJson;
	TestTrue(TEXT("Animation assets skeleton_list_bones parses JSON"), ParseJson(SkeletonBonesResult.Body, SkeletonBonesJson));
	const TSharedPtr<FJsonObject>* SkeletonBonesData = nullptr;
	TestTrue(TEXT("Animation assets skeleton_list_bones contains data object"), SkeletonBonesJson.IsValid() && SkeletonBonesJson->TryGetObjectField(TEXT("data"), SkeletonBonesData) && SkeletonBonesData && SkeletonBonesData->IsValid());

	FString RootBoneName;
	FString SecondaryBoneName;
	if (SkeletonBonesData && SkeletonBonesData->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Bones = nullptr;
		TestTrue(TEXT("Animation assets skeleton_list_bones contains bones array"), (*SkeletonBonesData)->TryGetArrayField(TEXT("bones"), Bones) && Bones != nullptr);
		if (Bones && Bones->Num() > 0)
		{
			const TSharedPtr<FJsonObject>* RootBoneObject = nullptr;
			if ((*Bones)[0].IsValid() && (*Bones)[0]->TryGetObject(RootBoneObject) && RootBoneObject && RootBoneObject->IsValid())
			{
				RootBoneName = (*RootBoneObject)->GetStringField(TEXT("bone_name"));
			}
			if (Bones->Num() > 1)
			{
				const TSharedPtr<FJsonObject>* SecondaryBoneObject = nullptr;
				if ((*Bones)[1].IsValid() && (*Bones)[1]->TryGetObject(SecondaryBoneObject) && SecondaryBoneObject && SecondaryBoneObject->IsValid())
				{
					SecondaryBoneName = (*SecondaryBoneObject)->GetStringField(TEXT("bone_name"));
				}
			}
		}
	}
	TestTrue(TEXT("Animation assets skeleton root bone name is non-empty"), !RootBoneName.IsEmpty());
	TestTrue(TEXT("Animation assets skeleton secondary bone name is non-empty"), !SecondaryBoneName.IsEmpty());

	const FString SetSkeletonPreviewMeshBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-skeleton-preview-001"),
		TEXT("skeleton_set_preview_mesh"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"skeletal_mesh_path\":\"%s\",\"save_after_set\":false}"), *SkeletonAssetPath, *SourceSkeletalMeshPath));
	FHttpSmokeResult SetSkeletonPreviewMeshResult;
	TestTrue(TEXT("Animation assets skeleton_set_preview_mesh request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetSkeletonPreviewMeshBody, SetSkeletonPreviewMeshResult));
	TestEqual(TEXT("Animation assets skeleton_set_preview_mesh status code"), SetSkeletonPreviewMeshResult.StatusCode, 200);

	const FString SetCompatibleSkeletonsBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-compatible-skeletons-001"),
		TEXT("skeleton_set_compatible_skeletons"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"add_compatible_skeleton_paths\":[\"%s\"],\"use_retarget_modes_from_compatible_skeleton\":true,\"save_after_set\":false}"), *SkeletonAssetPath, *SourceSkeletonPath));
	FHttpSmokeResult SetCompatibleSkeletonsResult;
	TestTrue(TEXT("Animation assets skeleton_set_compatible_skeletons request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetCompatibleSkeletonsBody, SetCompatibleSkeletonsResult));
	TestEqual(TEXT("Animation assets skeleton_set_compatible_skeletons status code"), SetCompatibleSkeletonsResult.StatusCode, 200);

	const FString SetSkeletonSocketBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-socket-001"),
		TEXT("skeleton_set_socket"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"socket_name\":\"%s\",\"bone_name\":\"%s\",\"relative_location\":{\"x\":3.0,\"y\":4.0,\"z\":5.0},\"save_after_set\":false}"), *SkeletonAssetPath, *SocketName, *RootBoneName));
	FHttpSmokeResult SetSkeletonSocketResult;
	TestTrue(TEXT("Animation assets skeleton_set_socket request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetSkeletonSocketBody, SetSkeletonSocketResult));
	TestEqual(TEXT("Animation assets skeleton_set_socket status code"), SetSkeletonSocketResult.StatusCode, 200);

	const FString SetVirtualBoneBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-virtual-bone-001"),
		TEXT("skeleton_set_virtual_bone"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"source_bone_name\":\"%s\",\"target_bone_name\":\"%s\",\"save_after_set\":false}"), *SkeletonAssetPath, *RootBoneName, *SecondaryBoneName));
	FHttpSmokeResult SetVirtualBoneResult;
	TestTrue(TEXT("Animation assets skeleton_set_virtual_bone request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetVirtualBoneBody, SetVirtualBoneResult));
	TestEqual(TEXT("Animation assets skeleton_set_virtual_bone status code"), SetVirtualBoneResult.StatusCode, 200);
	TSharedPtr<FJsonObject> SetVirtualBoneJson;
	TestTrue(TEXT("Animation assets skeleton_set_virtual_bone parses JSON"), ParseJson(SetVirtualBoneResult.Body, SetVirtualBoneJson));
	const TSharedPtr<FJsonObject>* SetVirtualBoneData = nullptr;
	TestTrue(TEXT("Animation assets skeleton_set_virtual_bone contains data object"), SetVirtualBoneJson.IsValid() && SetVirtualBoneJson->TryGetObjectField(TEXT("data"), SetVirtualBoneData) && SetVirtualBoneData && SetVirtualBoneData->IsValid());
	if (SetVirtualBoneData && SetVirtualBoneData->IsValid())
	{
		VirtualBoneName = (*SetVirtualBoneData)->GetStringField(TEXT("virtual_bone_name"));
		TestTrue(TEXT("Animation assets virtual bone name non-empty"), !VirtualBoneName.IsEmpty());
	}

	const FString SkeletonInfoBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-skeleton-info-001"),
		TEXT("skeleton_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *SkeletonAssetPath));
	FHttpSmokeResult SkeletonInfoResult;
	TestTrue(TEXT("Animation assets skeleton_get_info request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SkeletonInfoBody, SkeletonInfoResult));
	TestEqual(TEXT("Animation assets skeleton_get_info status code"), SkeletonInfoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SkeletonInfoJson;
	TestTrue(TEXT("Animation assets skeleton_get_info parses JSON"), ParseJson(SkeletonInfoResult.Body, SkeletonInfoJson));
	const TSharedPtr<FJsonObject>* SkeletonInfoData = nullptr;
	TestTrue(TEXT("Animation assets skeleton_get_info contains data object"), SkeletonInfoJson.IsValid() && SkeletonInfoJson->TryGetObjectField(TEXT("data"), SkeletonInfoData) && SkeletonInfoData && SkeletonInfoData->IsValid());
	if (SkeletonInfoData && SkeletonInfoData->IsValid())
	{
		TSharedPtr<FJsonObject> SocketObject;
		TestTrue(TEXT("Animation assets skeleton socket exists"), FindSocketNamed(SkeletonInfoData, SocketName, SocketObject));
		if (SocketObject.IsValid())
		{
			TestEqual(TEXT("Animation assets skeleton socket bone name"), SocketObject->GetStringField(TEXT("bone_name")), RootBoneName);
		}
		TestTrue(TEXT("Animation assets skeleton virtual bone exists"), FindVirtualBoneNamed(SkeletonInfoData, VirtualBoneName));
		const TArray<TSharedPtr<FJsonValue>>* CompatibleSkeletons = nullptr;
		TestTrue(TEXT("Animation assets skeleton compatible skeleton array exists"), (*SkeletonInfoData)->TryGetArrayField(TEXT("compatible_skeletons"), CompatibleSkeletons) && CompatibleSkeletons != nullptr);
		bool bContainsSourceSkeleton = false;
		if (CompatibleSkeletons)
		{
			for (const TSharedPtr<FJsonValue>& CompatibleValue : *CompatibleSkeletons)
			{
				if (CompatibleValue.IsValid() && CompatibleValue->AsString().StartsWith(SourceSkeletonPath))
				{
					bContainsSourceSkeleton = true;
					break;
				}
			}
		}
		TestTrue(TEXT("Animation assets skeleton compatible skeletons contains source"), bContainsSourceSkeleton);
		TestTrue(TEXT("Animation assets skeleton use compatible retarget modes enabled"), (*SkeletonInfoData)->GetBoolField(TEXT("use_retarget_modes_from_compatible_skeleton")));
	}

	const FString SkeletonFolderPath = MakeSmokeAssetFolderPath(TEXT("Skeleton"), SkeletonAssetPath);
	IFileManager::Get().DeleteDirectory(*SkeletonFolderPath, false, true);
	const FString SkeletonFolderJsonPath = ToSmokeJsonPath(SkeletonFolderPath);

	const FString SkeletonExportFolderBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-skeleton-folder-export-001"),
		TEXT("skeleton_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *SkeletonAssetPath));
	FHttpSmokeResult SkeletonExportFolderResult;
	TestTrue(TEXT("Animation assets skeleton_export_folder request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SkeletonExportFolderBody, SkeletonExportFolderResult));
	TestEqual(TEXT("Animation assets skeleton_export_folder status code"), SkeletonExportFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SkeletonExportFolderJson;
	TestTrue(TEXT("Animation assets skeleton_export_folder parses JSON"), ParseJson(SkeletonExportFolderResult.Body, SkeletonExportFolderJson));
	const TSharedPtr<FJsonObject>* SkeletonExportFolderData = nullptr;
	TestTrue(TEXT("Animation assets skeleton_export_folder contains data object"), SkeletonExportFolderJson.IsValid() && SkeletonExportFolderJson->TryGetObjectField(TEXT("data"), SkeletonExportFolderData) && SkeletonExportFolderData && SkeletonExportFolderData->IsValid());
	if (SkeletonExportFolderData && SkeletonExportFolderData->IsValid())
	{
		TestTrue(TEXT("Animation assets skeleton folder export marked complete"), (*SkeletonExportFolderData)->GetBoolField(TEXT("is_complete_target_schema")));
		TestTrue(TEXT("Animation assets skeleton folder export wrote files"), static_cast<int32>((*SkeletonExportFolderData)->GetIntegerField(TEXT("file_count"))) >= 10);
	}

	TSharedPtr<FJsonObject> SkeletonAssetFolderJson;
	TestTrue(TEXT("Animation assets skeleton folder asset.json exists"), LoadSmokeJsonFile(this, FPaths::Combine(SkeletonFolderPath, TEXT("asset.json")), SkeletonAssetFolderJson, TEXT("Skeleton folder asset.json")));
	TSharedPtr<FJsonObject> SkeletonCoverageJson;
	TestTrue(TEXT("Animation assets skeleton folder coverage exists"), LoadSmokeJsonFile(this, FPaths::Combine(SkeletonFolderPath, TEXT("validation"), TEXT("coverage_report.json")), SkeletonCoverageJson, TEXT("Skeleton folder coverage_report.json")));

	const FString SkeletonValidateFolderBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-skeleton-folder-validate-001"),
		TEXT("skeleton_validate_folder"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\"}"), *SkeletonFolderJsonPath));
	FHttpSmokeResult SkeletonValidateFolderResult;
	TestTrue(TEXT("Animation assets skeleton_validate_folder request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SkeletonValidateFolderBody, SkeletonValidateFolderResult));
	TestEqual(TEXT("Animation assets skeleton_validate_folder status code"), SkeletonValidateFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SkeletonValidateFolderJson;
	TestTrue(TEXT("Animation assets skeleton_validate_folder parses JSON"), ParseJson(SkeletonValidateFolderResult.Body, SkeletonValidateFolderJson));
	const TSharedPtr<FJsonObject>* SkeletonValidateFolderData = nullptr;
	TestTrue(TEXT("Animation assets skeleton_validate_folder contains data object"), SkeletonValidateFolderJson.IsValid() && SkeletonValidateFolderJson->TryGetObjectField(TEXT("data"), SkeletonValidateFolderData) && SkeletonValidateFolderData && SkeletonValidateFolderData->IsValid());
	if (SkeletonValidateFolderData && SkeletonValidateFolderData->IsValid())
	{
		TestTrue(TEXT("Animation assets skeleton_validate_folder is dry run"), (*SkeletonValidateFolderData)->GetBoolField(TEXT("dry_run")));
		TestEqual(TEXT("Animation assets skeleton_validate_folder has no JSON issues"), static_cast<int32>((*SkeletonValidateFolderData)->GetIntegerField(TEXT("json_issue_count"))), 0);
	}

	const FString SkeletonApplyFolderBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-skeleton-folder-apply-001"),
		TEXT("skeleton_apply_folder"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\",\"save_after_apply\":false}"), *SkeletonFolderJsonPath));
	FHttpSmokeResult SkeletonApplyFolderResult;
	TestTrue(TEXT("Animation assets skeleton_apply_folder request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SkeletonApplyFolderBody, SkeletonApplyFolderResult));
	TestEqual(TEXT("Animation assets skeleton_apply_folder status code"), SkeletonApplyFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SkeletonApplyFolderJson;
	TestTrue(TEXT("Animation assets skeleton_apply_folder parses JSON"), ParseJson(SkeletonApplyFolderResult.Body, SkeletonApplyFolderJson));
	const TSharedPtr<FJsonObject>* SkeletonApplyFolderData = nullptr;
	TestTrue(TEXT("Animation assets skeleton_apply_folder contains data object"), SkeletonApplyFolderJson.IsValid() && SkeletonApplyFolderJson->TryGetObjectField(TEXT("data"), SkeletonApplyFolderData) && SkeletonApplyFolderData && SkeletonApplyFolderData->IsValid());
	if (SkeletonApplyFolderData && SkeletonApplyFolderData->IsValid())
	{
		TestTrue(TEXT("Animation assets skeleton_apply_folder applied"), (*SkeletonApplyFolderData)->GetBoolField(TEXT("applied")));
		TestEqual(TEXT("Animation assets skeleton_apply_folder has no JSON issues"), static_cast<int32>((*SkeletonApplyFolderData)->GetIntegerField(TEXT("json_issue_count"))), 0);
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceIKAssetsSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.IKAssetsWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceIKAssetsSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString SourceSkeletonPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton");
	const FString SourceSkeletalMeshPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP");
	const FString SourceAnimationAssetPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Idle");
	const FString TargetMeshAssetPath = MakeAutomationAssetPath(TEXT("TargetMesh"));
	const FString SourceIKRigAssetPath = MakeAutomationAssetPath(TEXT("IKRigSrc"));
	const FString TargetIKRigAssetPath = MakeAutomationAssetPath(TEXT("IKRigDst"));
	const FString AutoIKRigAssetPath = MakeAutomationAssetPath(TEXT("IKRigAuto"));
	const FString RetargeterAssetPath = MakeAutomationAssetPath(TEXT("RTG"));
	const FString RetargetOutputFolder = FString::Printf(
		TEXT("/Game/__UeAgentInterfaceSmoke/RetargetStage20_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString GoalName = TEXT("Stage20Goal");
	const FString ChainName = TEXT("Stage20Chain");
	const FString TargetPoseName = TEXT("Stage22Pose");
	const FString TargetPoseCopyName = TEXT("Stage22PoseCopy");
	const FString TargetPoseRenamed = TEXT("Stage22PoseRenamed");
	const FString BodyMoverSolverType = TEXT("/Script/IKRig.IKRigBodyMoverSolver");
	const FString FBIKSolverType = TEXT("/Script/IKRig.IKRigFullBodyIKSolver");

	const FString SkeletonBonesBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-bones-001"),
		TEXT("skeleton_list_bones"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *SourceSkeletonPath));
	FHttpSmokeResult SkeletonBonesResult;
	TestTrue(TEXT("IK assets skeleton_list_bones request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SkeletonBonesBody, SkeletonBonesResult));
	TestEqual(TEXT("IK assets skeleton_list_bones status code"), SkeletonBonesResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SkeletonBonesJson;
	TestTrue(TEXT("IK assets skeleton_list_bones parses JSON"), ParseJson(SkeletonBonesResult.Body, SkeletonBonesJson));
	const TSharedPtr<FJsonObject>* SkeletonBonesData = nullptr;
	TestTrue(TEXT("IK assets skeleton_list_bones contains data object"), SkeletonBonesJson.IsValid() && SkeletonBonesJson->TryGetObjectField(TEXT("data"), SkeletonBonesData) && SkeletonBonesData && SkeletonBonesData->IsValid());

	FString RootBoneName;
	FString SecondaryBoneName;
	if (SkeletonBonesData && SkeletonBonesData->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Bones = nullptr;
		TestTrue(TEXT("IK assets skeleton_list_bones contains bones array"), (*SkeletonBonesData)->TryGetArrayField(TEXT("bones"), Bones) && Bones != nullptr);
		if (Bones && Bones->Num() > 0)
		{
			const TSharedPtr<FJsonObject>* BoneObject = nullptr;
			if ((*Bones)[0].IsValid() && (*Bones)[0]->TryGetObject(BoneObject) && BoneObject && BoneObject->IsValid())
			{
				RootBoneName = (*BoneObject)->GetStringField(TEXT("bone_name"));
			}
		}
		if (Bones && Bones->Num() > 1)
		{
			const TSharedPtr<FJsonObject>* BoneObject = nullptr;
			if ((*Bones)[1].IsValid() && (*Bones)[1]->TryGetObject(BoneObject) && BoneObject && BoneObject->IsValid())
			{
				SecondaryBoneName = (*BoneObject)->GetStringField(TEXT("bone_name"));
			}
		}
	}
	TestTrue(TEXT("IK assets root bone name is non-empty"), !RootBoneName.IsEmpty());
	TestTrue(TEXT("IK assets secondary bone name is non-empty"), !SecondaryBoneName.IsEmpty());

	const FString DuplicateTargetMeshBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-dup-mesh-001"),
		TEXT("asset_duplicate"),
		FString::Printf(TEXT("{\"source_asset_path\":\"%s\",\"destination_asset_path\":\"%s\",\"save_after_duplicate\":false}"), *SourceSkeletalMeshPath, *TargetMeshAssetPath));
	FHttpSmokeResult DuplicateTargetMeshResult;
	TestTrue(TEXT("IK assets duplicate target mesh request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DuplicateTargetMeshBody, DuplicateTargetMeshResult));
	TestEqual(TEXT("IK assets duplicate target mesh status code"), DuplicateTargetMeshResult.StatusCode, 200);

	const FString SkeletalMeshInfoBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-skelmesh-info-001"),
		TEXT("skeletal_mesh_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *TargetMeshAssetPath));
	FHttpSmokeResult SkeletalMeshInfoResult;
	TestTrue(TEXT("IK assets skeletal_mesh_get_info request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SkeletalMeshInfoBody, SkeletalMeshInfoResult));
	TestEqual(TEXT("IK assets skeletal_mesh_get_info status code"), SkeletalMeshInfoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SkeletalMeshInfoJson;
	TestTrue(TEXT("IK assets skeletal_mesh_get_info parses JSON"), ParseJson(SkeletalMeshInfoResult.Body, SkeletalMeshInfoJson));
	const TSharedPtr<FJsonObject>* SkeletalMeshInfoData = nullptr;
	TestTrue(TEXT("IK assets skeletal_mesh_get_info contains data object"), SkeletalMeshInfoJson.IsValid() && SkeletalMeshInfoJson->TryGetObjectField(TEXT("data"), SkeletalMeshInfoData) && SkeletalMeshInfoData && SkeletalMeshInfoData->IsValid());
	if (SkeletalMeshInfoData && SkeletalMeshInfoData->IsValid())
	{
		TestTrue(TEXT("IK assets skeletal mesh has materials"), static_cast<int32>((*SkeletalMeshInfoData)->GetIntegerField(TEXT("material_slot_count"))) >= 1);
		TestTrue(TEXT("IK assets skeletal mesh has lods"), static_cast<int32>((*SkeletalMeshInfoData)->GetIntegerField(TEXT("lod_count"))) >= 1);
	}

	const FString SkeletalMeshFolderPath = MakeSmokeAssetFolderPath(TEXT("SkeletalMesh"), TargetMeshAssetPath);
	IFileManager::Get().DeleteDirectory(*SkeletalMeshFolderPath, false, true);
	const FString SkeletalMeshFolderJsonPath = ToSmokeJsonPath(SkeletalMeshFolderPath);

	const FString SkeletalMeshExportFolderBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-skelmesh-folder-export-001"),
		TEXT("skeletal_mesh_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *TargetMeshAssetPath));
	FHttpSmokeResult SkeletalMeshExportFolderResult;
	TestTrue(TEXT("IK assets skeletal_mesh_export_folder request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SkeletalMeshExportFolderBody, SkeletalMeshExportFolderResult));
	TestEqual(TEXT("IK assets skeletal_mesh_export_folder status code"), SkeletalMeshExportFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SkeletalMeshExportFolderJson;
	TestTrue(TEXT("IK assets skeletal_mesh_export_folder parses JSON"), ParseJson(SkeletalMeshExportFolderResult.Body, SkeletalMeshExportFolderJson));
	const TSharedPtr<FJsonObject>* SkeletalMeshExportFolderData = nullptr;
	TestTrue(TEXT("IK assets skeletal_mesh_export_folder contains data object"), SkeletalMeshExportFolderJson.IsValid() && SkeletalMeshExportFolderJson->TryGetObjectField(TEXT("data"), SkeletalMeshExportFolderData) && SkeletalMeshExportFolderData && SkeletalMeshExportFolderData->IsValid());
	if (SkeletalMeshExportFolderData && SkeletalMeshExportFolderData->IsValid())
	{
		TestTrue(TEXT("IK assets skeletal mesh folder export marked complete"), (*SkeletalMeshExportFolderData)->GetBoolField(TEXT("is_complete_target_schema")));
		TestTrue(TEXT("IK assets skeletal mesh folder export wrote files"), static_cast<int32>((*SkeletalMeshExportFolderData)->GetIntegerField(TEXT("file_count"))) >= 10);
	}

	TSharedPtr<FJsonObject> SkeletalMeshAssetFolderJson;
	TestTrue(TEXT("IK assets skeletal mesh folder asset.json exists"), LoadSmokeJsonFile(this, FPaths::Combine(SkeletalMeshFolderPath, TEXT("asset.json")), SkeletalMeshAssetFolderJson, TEXT("SkeletalMesh folder asset.json")));
	TSharedPtr<FJsonObject> SkeletalMeshCoverageJson;
	TestTrue(TEXT("IK assets skeletal mesh folder coverage exists"), LoadSmokeJsonFile(this, FPaths::Combine(SkeletalMeshFolderPath, TEXT("validation"), TEXT("coverage_report.json")), SkeletalMeshCoverageJson, TEXT("SkeletalMesh folder coverage_report.json")));

	const FString SkeletalMeshValidateFolderBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-skelmesh-folder-validate-001"),
		TEXT("skeletal_mesh_validate_folder"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\"}"), *SkeletalMeshFolderJsonPath));
	FHttpSmokeResult SkeletalMeshValidateFolderResult;
	TestTrue(TEXT("IK assets skeletal_mesh_validate_folder request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SkeletalMeshValidateFolderBody, SkeletalMeshValidateFolderResult));
	TestEqual(TEXT("IK assets skeletal_mesh_validate_folder status code"), SkeletalMeshValidateFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SkeletalMeshValidateFolderJson;
	TestTrue(TEXT("IK assets skeletal_mesh_validate_folder parses JSON"), ParseJson(SkeletalMeshValidateFolderResult.Body, SkeletalMeshValidateFolderJson));
	const TSharedPtr<FJsonObject>* SkeletalMeshValidateFolderData = nullptr;
	TestTrue(TEXT("IK assets skeletal_mesh_validate_folder contains data object"), SkeletalMeshValidateFolderJson.IsValid() && SkeletalMeshValidateFolderJson->TryGetObjectField(TEXT("data"), SkeletalMeshValidateFolderData) && SkeletalMeshValidateFolderData && SkeletalMeshValidateFolderData->IsValid());
	if (SkeletalMeshValidateFolderData && SkeletalMeshValidateFolderData->IsValid())
	{
		TestTrue(TEXT("IK assets skeletal_mesh_validate_folder is dry run"), (*SkeletalMeshValidateFolderData)->GetBoolField(TEXT("dry_run")));
		TestEqual(TEXT("IK assets skeletal_mesh_validate_folder has no JSON issues"), static_cast<int32>((*SkeletalMeshValidateFolderData)->GetIntegerField(TEXT("json_issue_count"))), 0);
	}

	const FString SkeletalMeshApplyFolderBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-skelmesh-folder-apply-001"),
		TEXT("skeletal_mesh_apply_folder"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\",\"save_after_apply\":false}"), *SkeletalMeshFolderJsonPath));
	FHttpSmokeResult SkeletalMeshApplyFolderResult;
	TestTrue(TEXT("IK assets skeletal_mesh_apply_folder request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SkeletalMeshApplyFolderBody, SkeletalMeshApplyFolderResult));
	TestEqual(TEXT("IK assets skeletal_mesh_apply_folder status code"), SkeletalMeshApplyFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SkeletalMeshApplyFolderJson;
	TestTrue(TEXT("IK assets skeletal_mesh_apply_folder parses JSON"), ParseJson(SkeletalMeshApplyFolderResult.Body, SkeletalMeshApplyFolderJson));
	const TSharedPtr<FJsonObject>* SkeletalMeshApplyFolderData = nullptr;
	TestTrue(TEXT("IK assets skeletal_mesh_apply_folder contains data object"), SkeletalMeshApplyFolderJson.IsValid() && SkeletalMeshApplyFolderJson->TryGetObjectField(TEXT("data"), SkeletalMeshApplyFolderData) && SkeletalMeshApplyFolderData && SkeletalMeshApplyFolderData->IsValid());
	if (SkeletalMeshApplyFolderData && SkeletalMeshApplyFolderData->IsValid())
	{
		TestTrue(TEXT("IK assets skeletal_mesh_apply_folder applied"), (*SkeletalMeshApplyFolderData)->GetBoolField(TEXT("applied")));
		TestEqual(TEXT("IK assets skeletal_mesh_apply_folder has no JSON issues"), static_cast<int32>((*SkeletalMeshApplyFolderData)->GetIntegerField(TEXT("json_issue_count"))), 0);
	}

	const FString CreateAutoIKRigBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-create-auto-rig-001"),
		TEXT("ik_rig_create"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"preview_skeletal_mesh\":\"%s\",\"save_after_create\":false}"), *AutoIKRigAssetPath, *SourceSkeletalMeshPath));
	FHttpSmokeResult CreateAutoIKRigResult;
	TestTrue(TEXT("IK assets create auto IK rig request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateAutoIKRigBody, CreateAutoIKRigResult));
	TestEqual(TEXT("IK assets create auto IK rig status code"), CreateAutoIKRigResult.StatusCode, 200);

	const FString ApplyAutoDefinitionBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-auto-def-001"),
		TEXT("ik_rig_apply_auto_retarget_definition"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"save_after_set\":false}"), *AutoIKRigAssetPath));
	FHttpSmokeResult ApplyAutoDefinitionResult;
	TestTrue(TEXT("IK assets apply auto retarget definition request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ApplyAutoDefinitionBody, ApplyAutoDefinitionResult));
	TestEqual(TEXT("IK assets apply auto retarget definition status code"), ApplyAutoDefinitionResult.StatusCode, 200);

	const FString CreateSourceIKRigBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-create-src-rig-001"),
		TEXT("ik_rig_create"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"preview_skeletal_mesh\":\"%s\",\"save_after_create\":false}"), *SourceIKRigAssetPath, *SourceSkeletalMeshPath));
	FHttpSmokeResult CreateSourceIKRigResult;
	TestTrue(TEXT("IK assets create source IK rig request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateSourceIKRigBody, CreateSourceIKRigResult));
	TestEqual(TEXT("IK assets create source IK rig status code"), CreateSourceIKRigResult.StatusCode, 200);

	const FString CreateTargetIKRigBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-create-dst-rig-001"),
		TEXT("ik_rig_create"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"preview_skeletal_mesh\":\"%s\",\"save_after_create\":false}"), *TargetIKRigAssetPath, *TargetMeshAssetPath));
	FHttpSmokeResult CreateTargetIKRigResult;
	TestTrue(TEXT("IK assets create target IK rig request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateTargetIKRigBody, CreateTargetIKRigResult));
	TestEqual(TEXT("IK assets create target IK rig status code"), CreateTargetIKRigResult.StatusCode, 200);

	const FString SetSourceRootBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-src-root-001"),
		TEXT("ik_rig_set_retarget_root"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"root_bone_name\":\"%s\",\"save_after_set\":false}"), *SourceIKRigAssetPath, *RootBoneName));
	FHttpSmokeResult SetSourceRootResult;
	TestTrue(TEXT("IK assets set source retarget root request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetSourceRootBody, SetSourceRootResult));
	TestEqual(TEXT("IK assets set source retarget root status code"), SetSourceRootResult.StatusCode, 200);

	const FString SetTargetRootBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-dst-root-001"),
		TEXT("ik_rig_set_retarget_root"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"root_bone_name\":\"%s\",\"save_after_set\":false}"), *TargetIKRigAssetPath, *RootBoneName));
	FHttpSmokeResult SetTargetRootResult;
	TestTrue(TEXT("IK assets set target retarget root request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetTargetRootBody, SetTargetRootResult));
	TestEqual(TEXT("IK assets set target retarget root status code"), SetTargetRootResult.StatusCode, 200);

	const FString SetGoalBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-goal-001"),
		TEXT("ik_rig_set_goal"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"goal_name\":\"%s\",\"bone_name\":\"%s\",\"position_alpha\":0.25,\"rotation_alpha\":0.75,\"position\":{\"x\":11.0,\"y\":22.0,\"z\":33.0},\"rotation\":{\"pitch\":10.0,\"yaw\":20.0,\"roll\":30.0},\"save_after_set\":false}"), *SourceIKRigAssetPath, *GoalName, *SecondaryBoneName));
	FHttpSmokeResult SetGoalResult;
	TestTrue(TEXT("IK assets set goal request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetGoalBody, SetGoalResult));
	TestEqual(TEXT("IK assets set goal status code"), SetGoalResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SetGoalJson;
	TestTrue(TEXT("IK assets set goal parses JSON"), ParseJson(SetGoalResult.Body, SetGoalJson));
	const TSharedPtr<FJsonObject>* SetGoalData = nullptr;
	TestTrue(TEXT("IK assets set goal contains data object"), SetGoalJson.IsValid() && SetGoalJson->TryGetObjectField(TEXT("data"), SetGoalData) && SetGoalData && SetGoalData->IsValid());
	if (SetGoalData && SetGoalData->IsValid())
	{
		TestEqual(TEXT("IK assets set goal response bone name updated"), (*SetGoalData)->GetStringField(TEXT("bone_name")), SecondaryBoneName);
		TestTrue(TEXT("IK assets set goal response position alpha updated"), FMath::IsNearlyEqual(static_cast<float>((*SetGoalData)->GetNumberField(TEXT("position_alpha"))), 0.25f));
		TestTrue(TEXT("IK assets set goal response rotation alpha updated"), FMath::IsNearlyEqual(static_cast<float>((*SetGoalData)->GetNumberField(TEXT("rotation_alpha"))), 0.75f));
		const TSharedPtr<FJsonObject>* GoalLocation = nullptr;
		TestTrue(TEXT("IK assets set goal response current location exists"), (*SetGoalData)->TryGetObjectField(TEXT("current_location"), GoalLocation) && GoalLocation && GoalLocation->IsValid());
		if (GoalLocation && GoalLocation->IsValid())
		{
			TestTrue(TEXT("IK assets set goal response current location x updated"), FMath::IsNearlyEqual(static_cast<float>((*GoalLocation)->GetNumberField(TEXT("x"))), 11.0f));
			TestTrue(TEXT("IK assets set goal response current location z updated"), FMath::IsNearlyEqual(static_cast<float>((*GoalLocation)->GetNumberField(TEXT("z"))), 33.0f));
		}
		const TSharedPtr<FJsonObject>* GoalRotation = nullptr;
		TestTrue(TEXT("IK assets set goal response current rotation exists"), (*SetGoalData)->TryGetObjectField(TEXT("current_rotation"), GoalRotation) && GoalRotation && GoalRotation->IsValid());
		if (GoalRotation && GoalRotation->IsValid())
		{
			TestTrue(TEXT("IK assets set goal response current rotation yaw updated"), FMath::IsNearlyEqual(static_cast<float>((*GoalRotation)->GetNumberField(TEXT("yaw"))), 20.0f));
		}
	}

	const FString SetSourceChainBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-src-chain-001"),
		TEXT("ik_rig_set_retarget_chain"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"chain_name\":\"%s\",\"start_bone_name\":\"%s\",\"end_bone_name\":\"%s\",\"goal_name\":\"%s\",\"save_after_set\":false}"), *SourceIKRigAssetPath, *ChainName, *RootBoneName, *SecondaryBoneName, *GoalName));
	FHttpSmokeResult SetSourceChainResult;
	TestTrue(TEXT("IK assets set source chain request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetSourceChainBody, SetSourceChainResult));
	TestEqual(TEXT("IK assets set source chain status code"), SetSourceChainResult.StatusCode, 200);

	const FString AddSourceSolverBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-src-solver-001"),
		TEXT("ik_rig_set_solver"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"solver_type\":\"%s\",\"start_bone_name\":\"%s\",\"enabled\":true,\"connect_goal_names\":[\"%s\"],\"settings\":{\"position_alpha\":0.5,\"position_positive_x\":0.6,\"position_negative_x\":0.7,\"rotation_alpha\":0.8,\"rotate_z_alpha\":0.9},\"goal_settings\":[{\"goal_name\":\"%s\",\"influence_multiplier\":2.25}],\"save_after_set\":false}"), *SourceIKRigAssetPath, *BodyMoverSolverType, *RootBoneName, *GoalName, *GoalName));
	FHttpSmokeResult AddSourceSolverResult;
	TestTrue(TEXT("IK assets add source solver request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddSourceSolverBody, AddSourceSolverResult));
	TestEqual(TEXT("IK assets add source solver status code"), AddSourceSolverResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddSourceSolverJson;
	TestTrue(TEXT("IK assets add source solver parses JSON"), ParseJson(AddSourceSolverResult.Body, AddSourceSolverJson));
	int32 SourceSolverIndex = INDEX_NONE;
	const TSharedPtr<FJsonObject>* AddSourceSolverData = nullptr;
	TestTrue(TEXT("IK assets add source solver contains data object"), AddSourceSolverJson.IsValid() && AddSourceSolverJson->TryGetObjectField(TEXT("data"), AddSourceSolverData) && AddSourceSolverData && AddSourceSolverData->IsValid());
	if (AddSourceSolverData && AddSourceSolverData->IsValid())
	{
		SourceSolverIndex = static_cast<int32>((*AddSourceSolverData)->GetIntegerField(TEXT("solver_index")));
		TestTrue(TEXT("IK assets source solver struct contains body mover"), (*AddSourceSolverData)->GetStringField(TEXT("solver_struct")).Contains(TEXT("IKRigBodyMoverSolver")));
		TestEqual(TEXT("IK assets source solver kind is body mover"), (*AddSourceSolverData)->GetStringField(TEXT("solver_kind")), FString(TEXT("body_mover")));
		const TSharedPtr<FJsonObject>* SolverSettings = nullptr;
		TestTrue(TEXT("IK assets source solver settings object exists"), (*AddSourceSolverData)->TryGetObjectField(TEXT("settings"), SolverSettings) && SolverSettings && SolverSettings->IsValid());
		if (SolverSettings && SolverSettings->IsValid())
		{
			TestTrue(TEXT("IK assets source solver position alpha updated"), FMath::IsNearlyEqual(static_cast<float>((*SolverSettings)->GetNumberField(TEXT("position_alpha"))), 0.5f));
			TestTrue(TEXT("IK assets source solver rotate z alpha updated"), FMath::IsNearlyEqual(static_cast<float>((*SolverSettings)->GetNumberField(TEXT("rotate_z_alpha"))), 0.9f));
		}
		const TArray<TSharedPtr<FJsonValue>>* GoalSettings = nullptr;
		TestTrue(TEXT("IK assets source solver goal settings array exists"), (*AddSourceSolverData)->TryGetArrayField(TEXT("goal_settings"), GoalSettings) && GoalSettings != nullptr);
		if (GoalSettings && GoalSettings->Num() > 0)
		{
			const TSharedPtr<FJsonObject>* GoalSettingsObject = nullptr;
			TestTrue(TEXT("IK assets source solver first goal settings parses"), (*GoalSettings)[0].IsValid() && (*GoalSettings)[0]->TryGetObject(GoalSettingsObject) && GoalSettingsObject && GoalSettingsObject->IsValid());
			if (GoalSettingsObject && GoalSettingsObject->IsValid())
			{
				TestEqual(TEXT("IK assets source solver goal name updated"), (*GoalSettingsObject)->GetStringField(TEXT("goal_name")), GoalName);
				TestTrue(TEXT("IK assets source solver goal influence updated"), FMath::IsNearlyEqual(static_cast<float>((*GoalSettingsObject)->GetNumberField(TEXT("influence_multiplier"))), 2.25f));
			}
		}
	}

	const FString DisableSourceSolverBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-src-solver-002"),
		TEXT("ik_rig_set_solver"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"solver_index\":%d,\"enabled\":false,\"save_after_set\":false}"), *SourceIKRigAssetPath, SourceSolverIndex));
	FHttpSmokeResult DisableSourceSolverResult;
	TestTrue(TEXT("IK assets disable source solver request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DisableSourceSolverBody, DisableSourceSolverResult));
	TestEqual(TEXT("IK assets disable source solver status code"), DisableSourceSolverResult.StatusCode, 200);

	const FString AddFBIKSolverBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-src-solver-003"),
		TEXT("ik_rig_set_solver"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"solver_type\":\"%s\",\"start_bone_name\":\"%s\",\"enabled\":true,\"connect_goal_names\":[\"%s\"],\"settings\":{\"iterations\":16,\"sub_iterations\":4,\"mass_multiplier\":1.5,\"allow_stretch\":true,\"root_behavior\":\"free\",\"global_pull_chain_alpha\":0.85,\"max_angle\":25.0,\"over_relaxation\":1.1},\"goal_settings\":[{\"goal_name\":\"%s\",\"chain_depth\":2,\"strength_alpha\":0.6,\"pull_chain_alpha\":0.7,\"pin_rotation\":0.8}],\"bone_settings\":[{\"bone_name\":\"%s\",\"rotation_stiffness\":0.3,\"position_stiffness\":0.4,\"use_preferred_angles\":true,\"preferred_angles\":{\"x\":5.0,\"y\":10.0,\"z\":15.0}}],\"save_after_set\":false}"), *SourceIKRigAssetPath, *FBIKSolverType, *RootBoneName, *GoalName, *GoalName, *RootBoneName));
	FHttpSmokeResult AddFBIKSolverResult;
	TestTrue(TEXT("IK assets add FBIK solver request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddFBIKSolverBody, AddFBIKSolverResult));
	TestEqual(TEXT("IK assets add FBIK solver status code"), AddFBIKSolverResult.StatusCode, 200);

	const FString ProbeIKGoalLegacyAssetBody = MakeExecRequestBody(
		TEXT("smoke-probe-ik-goal-legacy-asset-rejected-001"),
		TEXT("probe_ik_goal_candidates"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"goal_name\":\"%s\",\"candidates\":[{\"label\":\"legacy\",\"transform\":{\"location\":{\"x\":20.0,\"y\":0.0,\"z\":0.0}}}]}"),
			*SourceIKRigAssetPath,
			*GoalName));
	FHttpSmokeResult ProbeIKGoalLegacyAssetResult;
	TestTrue(TEXT("Probe IK goal legacy asset request reached server"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ProbeIKGoalLegacyAssetBody, ProbeIKGoalLegacyAssetResult, 30.0));
	TestEqual(TEXT("Probe IK goal legacy asset status code"), ProbeIKGoalLegacyAssetResult.StatusCode, 400);

	TSharedPtr<FJsonObject> ProbeIKGoalLegacyAssetJson;
	TestTrue(TEXT("Probe IK goal legacy asset parses JSON"), ParseJson(ProbeIKGoalLegacyAssetResult.Body, ProbeIKGoalLegacyAssetJson));
	TestFalse(TEXT("Probe IK goal legacy asset mode rejected"), ProbeIKGoalLegacyAssetJson.IsValid() && ProbeIKGoalLegacyAssetJson->GetBoolField(TEXT("ok")));
	TestTrue(TEXT("Probe IK goal legacy asset rejection explains missing sequence"), ProbeIKGoalLegacyAssetResult.Body.Contains(TEXT("missing_sequence_path")));

	const FString SetTargetChainBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-dst-chain-001"),
		TEXT("ik_rig_set_retarget_chain"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"chain_name\":\"%s\",\"start_bone_name\":\"%s\",\"end_bone_name\":\"%s\",\"save_after_set\":false}"), *TargetIKRigAssetPath, *ChainName, *RootBoneName, *SecondaryBoneName));
	FHttpSmokeResult SetTargetChainResult;
	TestTrue(TEXT("IK assets set target chain request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetTargetChainBody, SetTargetChainResult));
	TestEqual(TEXT("IK assets set target chain status code"), SetTargetChainResult.StatusCode, 200);

	const FString SourceIKRigInfoBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-src-info-001"),
		TEXT("ik_rig_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *SourceIKRigAssetPath));
	FHttpSmokeResult SourceIKRigInfoResult;
	TestTrue(TEXT("IK assets source ik rig get_info request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SourceIKRigInfoBody, SourceIKRigInfoResult));
	TestEqual(TEXT("IK assets source ik rig get_info status code"), SourceIKRigInfoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SourceIKRigInfoJson;
	TestTrue(TEXT("IK assets source ik rig get_info parses JSON"), ParseJson(SourceIKRigInfoResult.Body, SourceIKRigInfoJson));
	const TSharedPtr<FJsonObject>* SourceIKRigInfoData = nullptr;
	TestTrue(TEXT("IK assets source ik rig get_info contains data object"), SourceIKRigInfoJson.IsValid() && SourceIKRigInfoJson->TryGetObjectField(TEXT("data"), SourceIKRigInfoData) && SourceIKRigInfoData && SourceIKRigInfoData->IsValid());
	if (SourceIKRigInfoData && SourceIKRigInfoData->IsValid())
	{
		TestEqual(TEXT("IK assets source ik rig solver count >= 1"), static_cast<int32>((*SourceIKRigInfoData)->GetIntegerField(TEXT("solver_count"))) >= 1, true);
		const TArray<TSharedPtr<FJsonValue>>* Goals = nullptr;
		TestTrue(TEXT("IK assets source ik rig contains goals array"), (*SourceIKRigInfoData)->TryGetArrayField(TEXT("goals"), Goals) && Goals != nullptr);
		if (Goals && Goals->Num() > 0)
		{
			const TSharedPtr<FJsonObject>* GoalObject = nullptr;
			if ((*Goals)[0].IsValid() && (*Goals)[0]->TryGetObject(GoalObject) && GoalObject && GoalObject->IsValid())
			{
				TestEqual(TEXT("IK assets source goal name in info"), (*GoalObject)->GetStringField(TEXT("goal_name")), GoalName);
				TestTrue(TEXT("IK assets source goal position alpha updated"), FMath::IsNearlyEqual(static_cast<float>((*GoalObject)->GetNumberField(TEXT("position_alpha"))), 0.25f));
				TestTrue(TEXT("IK assets source goal rotation alpha updated"), FMath::IsNearlyEqual(static_cast<float>((*GoalObject)->GetNumberField(TEXT("rotation_alpha"))), 0.75f));
				const TSharedPtr<FJsonObject>* GoalLocation = nullptr;
				TestTrue(TEXT("IK assets source goal current location exists"), (*GoalObject)->TryGetObjectField(TEXT("current_location"), GoalLocation) && GoalLocation && GoalLocation->IsValid());
				const TSharedPtr<FJsonObject>* GoalRotation = nullptr;
				TestTrue(TEXT("IK assets source goal current rotation exists"), (*GoalObject)->TryGetObjectField(TEXT("current_rotation"), GoalRotation) && GoalRotation && GoalRotation->IsValid());
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* Solvers = nullptr;
		TestTrue(TEXT("IK assets source ik rig contains solvers array"), (*SourceIKRigInfoData)->TryGetArrayField(TEXT("solvers"), Solvers) && Solvers != nullptr);
		if (Solvers && Solvers->Num() > 0)
		{
			bool bFoundBodyMover = false;
			bool bFoundFBIK = false;
			for (const TSharedPtr<FJsonValue>& SolverValue : *Solvers)
			{
				const TSharedPtr<FJsonObject>* SolverObject = nullptr;
				if (!SolverValue.IsValid() || !SolverValue->TryGetObject(SolverObject) || !SolverObject || !SolverObject->IsValid())
				{
					continue;
				}

				const FString SolverStructPath = (*SolverObject)->GetStringField(TEXT("solver_struct"));
				if (SolverStructPath.Contains(TEXT("IKRigBodyMoverSolver")))
				{
					bFoundBodyMover = true;
					TestEqual(TEXT("IK assets source solver disabled in info"), (*SolverObject)->GetBoolField(TEXT("enabled")), false);
					const TSharedPtr<FJsonObject>* SolverSettings = nullptr;
					TestTrue(TEXT("IK assets source solver settings in info exist"), (*SolverObject)->TryGetObjectField(TEXT("settings"), SolverSettings) && SolverSettings && SolverSettings->IsValid());
					if (SolverSettings && SolverSettings->IsValid())
					{
						TestTrue(TEXT("IK assets source solver position alpha in info updated"), FMath::IsNearlyEqual(static_cast<float>((*SolverSettings)->GetNumberField(TEXT("position_alpha"))), 0.5f));
					}
					const TArray<TSharedPtr<FJsonValue>>* GoalSettings = nullptr;
					TestTrue(TEXT("IK assets source solver goal settings in info exist"), (*SolverObject)->TryGetArrayField(TEXT("goal_settings"), GoalSettings) && GoalSettings != nullptr);
					if (GoalSettings && GoalSettings->Num() > 0)
					{
						const TSharedPtr<FJsonObject>* GoalSettingsObject = nullptr;
						TestTrue(TEXT("IK assets source solver goal settings info parses"), (*GoalSettings)[0].IsValid() && (*GoalSettings)[0]->TryGetObject(GoalSettingsObject) && GoalSettingsObject && GoalSettingsObject->IsValid());
						if (GoalSettingsObject && GoalSettingsObject->IsValid())
						{
							TestTrue(TEXT("IK assets source solver goal influence in info updated"), FMath::IsNearlyEqual(static_cast<float>((*GoalSettingsObject)->GetNumberField(TEXT("influence_multiplier"))), 2.25f));
						}
					}
				}
				else if (SolverStructPath.Contains(TEXT("IKRigFullBodyIKSolver")))
				{
					bFoundFBIK = true;
					TestEqual(TEXT("IK assets source FBIK solver kind in info"), (*SolverObject)->GetStringField(TEXT("solver_kind")), FString(TEXT("full_body_ik")));
					const TSharedPtr<FJsonObject>* SolverSettings = nullptr;
					TestTrue(TEXT("IK assets source FBIK settings in info exist"), (*SolverObject)->TryGetObjectField(TEXT("settings"), SolverSettings) && SolverSettings && SolverSettings->IsValid());
					if (SolverSettings && SolverSettings->IsValid())
					{
						TestEqual(TEXT("IK assets source FBIK root behavior in info"), (*SolverSettings)->GetStringField(TEXT("root_behavior")), FString(TEXT("free")));
						TestTrue(TEXT("IK assets source FBIK iterations in info updated"), static_cast<int32>((*SolverSettings)->GetIntegerField(TEXT("iterations"))) == 16);
						TestTrue(TEXT("IK assets source FBIK allow stretch in info updated"), (*SolverSettings)->GetBoolField(TEXT("allow_stretch")));
					}
					const TArray<TSharedPtr<FJsonValue>>* GoalSettings = nullptr;
					TestTrue(TEXT("IK assets source FBIK goal settings in info exist"), (*SolverObject)->TryGetArrayField(TEXT("goal_settings"), GoalSettings) && GoalSettings != nullptr);
					if (GoalSettings && GoalSettings->Num() > 0)
					{
						const TSharedPtr<FJsonObject>* GoalSettingsObject = nullptr;
						TestTrue(TEXT("IK assets source FBIK goal settings info parses"), (*GoalSettings)[0].IsValid() && (*GoalSettings)[0]->TryGetObject(GoalSettingsObject) && GoalSettingsObject && GoalSettingsObject->IsValid());
						if (GoalSettingsObject && GoalSettingsObject->IsValid())
						{
							TestTrue(TEXT("IK assets source FBIK chain depth in info updated"), static_cast<int32>((*GoalSettingsObject)->GetIntegerField(TEXT("chain_depth"))) == 2);
							TestTrue(TEXT("IK assets source FBIK strength alpha in info updated"), FMath::IsNearlyEqual(static_cast<float>((*GoalSettingsObject)->GetNumberField(TEXT("strength_alpha"))), 0.6f));
						}
					}

					const TArray<TSharedPtr<FJsonValue>>* BoneSettings = nullptr;
					TestTrue(TEXT("IK assets source FBIK bone settings in info exist"), (*SolverObject)->TryGetArrayField(TEXT("bone_settings"), BoneSettings) && BoneSettings != nullptr);
					if (BoneSettings && BoneSettings->Num() > 0)
					{
						const TSharedPtr<FJsonObject>* BoneSettingsObject = nullptr;
						TestTrue(TEXT("IK assets source FBIK bone settings info parses"), (*BoneSettings)[0].IsValid() && (*BoneSettings)[0]->TryGetObject(BoneSettingsObject) && BoneSettingsObject && BoneSettingsObject->IsValid());
						if (BoneSettingsObject && BoneSettingsObject->IsValid())
						{
							TestEqual(TEXT("IK assets source FBIK bone settings root bone"), (*BoneSettingsObject)->GetStringField(TEXT("bone_name")), RootBoneName);
							TestTrue(TEXT("IK assets source FBIK bone rotation stiffness in info updated"), FMath::IsNearlyEqual(static_cast<float>((*BoneSettingsObject)->GetNumberField(TEXT("rotation_stiffness"))), 0.3f));
						}
					}
				}
			}
			TestTrue(TEXT("IK assets source body mover solver found in info"), bFoundBodyMover);
			TestTrue(TEXT("IK assets source FBIK solver found in info"), bFoundFBIK);
		}
	}

	const FString IKRigFolderPath = MakeSmokeAssetFolderPath(TEXT("IKRig"), SourceIKRigAssetPath);
	IFileManager::Get().DeleteDirectory(*IKRigFolderPath, false, true);
	const FString IKRigFolderJsonPath = ToSmokeJsonPath(IKRigFolderPath);

	const FString IKRigExportFolderBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rig-folder-export-001"),
		TEXT("ik_rig_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *SourceIKRigAssetPath));
	FHttpSmokeResult IKRigExportFolderResult;
	TestTrue(TEXT("IK assets ik_rig_export_folder request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, IKRigExportFolderBody, IKRigExportFolderResult));
	TestEqual(TEXT("IK assets ik_rig_export_folder status code"), IKRigExportFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> IKRigExportFolderJson;
	TestTrue(TEXT("IK assets ik_rig_export_folder parses JSON"), ParseJson(IKRigExportFolderResult.Body, IKRigExportFolderJson));
	const TSharedPtr<FJsonObject>* IKRigExportFolderData = nullptr;
	TestTrue(TEXT("IK assets ik_rig_export_folder contains data object"), IKRigExportFolderJson.IsValid() && IKRigExportFolderJson->TryGetObjectField(TEXT("data"), IKRigExportFolderData) && IKRigExportFolderData && IKRigExportFolderData->IsValid());
	if (IKRigExportFolderData && IKRigExportFolderData->IsValid())
	{
		TestTrue(TEXT("IK assets ik rig folder export marked complete"), (*IKRigExportFolderData)->GetBoolField(TEXT("is_complete_target_schema")));
		TestTrue(TEXT("IK assets ik rig folder export wrote files"), static_cast<int32>((*IKRigExportFolderData)->GetIntegerField(TEXT("file_count"))) >= 8);
	}

	TSharedPtr<FJsonObject> IKRigAssetFolderJson;
	TestTrue(TEXT("IK assets ik rig folder asset.json exists"), LoadSmokeJsonFile(this, FPaths::Combine(IKRigFolderPath, TEXT("asset.json")), IKRigAssetFolderJson, TEXT("IKRig folder asset.json")));
	TSharedPtr<FJsonObject> IKRigCoverageJson;
	TestTrue(TEXT("IK assets ik rig folder coverage exists"), LoadSmokeJsonFile(this, FPaths::Combine(IKRigFolderPath, TEXT("validation"), TEXT("coverage_report.json")), IKRigCoverageJson, TEXT("IKRig folder coverage_report.json")));

	const FString IKRigValidateFolderBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rig-folder-validate-001"),
		TEXT("ik_rig_validate_folder"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\"}"), *IKRigFolderJsonPath));
	FHttpSmokeResult IKRigValidateFolderResult;
	TestTrue(TEXT("IK assets ik_rig_validate_folder request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, IKRigValidateFolderBody, IKRigValidateFolderResult));
	TestEqual(TEXT("IK assets ik_rig_validate_folder status code"), IKRigValidateFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> IKRigValidateFolderJson;
	TestTrue(TEXT("IK assets ik_rig_validate_folder parses JSON"), ParseJson(IKRigValidateFolderResult.Body, IKRigValidateFolderJson));
	const TSharedPtr<FJsonObject>* IKRigValidateFolderData = nullptr;
	TestTrue(TEXT("IK assets ik_rig_validate_folder contains data object"), IKRigValidateFolderJson.IsValid() && IKRigValidateFolderJson->TryGetObjectField(TEXT("data"), IKRigValidateFolderData) && IKRigValidateFolderData && IKRigValidateFolderData->IsValid());
	if (IKRigValidateFolderData && IKRigValidateFolderData->IsValid())
	{
		TestTrue(TEXT("IK assets ik_rig_validate_folder is dry run"), (*IKRigValidateFolderData)->GetBoolField(TEXT("dry_run")));
		TestEqual(TEXT("IK assets ik_rig_validate_folder has no JSON issues"), static_cast<int32>((*IKRigValidateFolderData)->GetIntegerField(TEXT("json_issue_count"))), 0);
	}

	const FString IKRigApplyFolderBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rig-folder-apply-001"),
		TEXT("ik_rig_apply_folder"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\",\"save_after_apply\":false}"), *IKRigFolderJsonPath));
	FHttpSmokeResult IKRigApplyFolderResult;
	TestTrue(TEXT("IK assets ik_rig_apply_folder request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, IKRigApplyFolderBody, IKRigApplyFolderResult));
	TestEqual(TEXT("IK assets ik_rig_apply_folder status code"), IKRigApplyFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> IKRigApplyFolderJson;
	TestTrue(TEXT("IK assets ik_rig_apply_folder parses JSON"), ParseJson(IKRigApplyFolderResult.Body, IKRigApplyFolderJson));
	const TSharedPtr<FJsonObject>* IKRigApplyFolderData = nullptr;
	TestTrue(TEXT("IK assets ik_rig_apply_folder contains data object"), IKRigApplyFolderJson.IsValid() && IKRigApplyFolderJson->TryGetObjectField(TEXT("data"), IKRigApplyFolderData) && IKRigApplyFolderData && IKRigApplyFolderData->IsValid());
	if (IKRigApplyFolderData && IKRigApplyFolderData->IsValid())
	{
		TestTrue(TEXT("IK assets ik_rig_apply_folder applied"), (*IKRigApplyFolderData)->GetBoolField(TEXT("applied")));
		TestEqual(TEXT("IK assets ik_rig_apply_folder has no JSON issues"), static_cast<int32>((*IKRigApplyFolderData)->GetIntegerField(TEXT("json_issue_count"))), 0);
	}

	const FString CreateRetargeterBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-create-rtg-001"),
		TEXT("ik_retargeter_create"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"save_after_create\":false}"), *RetargeterAssetPath));
	FHttpSmokeResult CreateRetargeterResult;
	TestTrue(TEXT("IK assets create retargeter request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateRetargeterBody, CreateRetargeterResult));
	TestEqual(TEXT("IK assets create retargeter status code"), CreateRetargeterResult.StatusCode, 200);

	const FString SetRetargeterSourceRigBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rtg-src-rig-001"),
		TEXT("ik_retargeter_set_ik_rig"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"source_or_target\":\"source\",\"ik_rig_path\":\"%s\",\"save_after_set\":false}"), *RetargeterAssetPath, *SourceIKRigAssetPath));
	FHttpSmokeResult SetRetargeterSourceRigResult;
	TestTrue(TEXT("IK assets retargeter set source rig request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetRetargeterSourceRigBody, SetRetargeterSourceRigResult));
	TestEqual(TEXT("IK assets retargeter set source rig status code"), SetRetargeterSourceRigResult.StatusCode, 200);

	const FString SetRetargeterTargetRigBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rtg-dst-rig-001"),
		TEXT("ik_retargeter_set_ik_rig"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"source_or_target\":\"target\",\"ik_rig_path\":\"%s\",\"save_after_set\":false}"), *RetargeterAssetPath, *TargetIKRigAssetPath));
	FHttpSmokeResult SetRetargeterTargetRigResult;
	TestTrue(TEXT("IK assets retargeter set target rig request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetRetargeterTargetRigBody, SetRetargeterTargetRigResult));
	TestEqual(TEXT("IK assets retargeter set target rig status code"), SetRetargeterTargetRigResult.StatusCode, 200);

	const FString SetRetargeterSourceMeshBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rtg-src-mesh-001"),
		TEXT("ik_retargeter_set_preview_mesh"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"source_or_target\":\"source\",\"skeletal_mesh_path\":\"%s\",\"save_after_set\":false}"), *RetargeterAssetPath, *SourceSkeletalMeshPath));
	FHttpSmokeResult SetRetargeterSourceMeshResult;
	TestTrue(TEXT("IK assets retargeter set source mesh request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetRetargeterSourceMeshBody, SetRetargeterSourceMeshResult));
	TestEqual(TEXT("IK assets retargeter set source mesh status code"), SetRetargeterSourceMeshResult.StatusCode, 200);

	const FString SetRetargeterTargetMeshBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rtg-dst-mesh-001"),
		TEXT("ik_retargeter_set_preview_mesh"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"source_or_target\":\"target\",\"skeletal_mesh_path\":\"%s\",\"save_after_set\":false}"), *RetargeterAssetPath, *TargetMeshAssetPath));
	FHttpSmokeResult SetRetargeterTargetMeshResult;
	TestTrue(TEXT("IK assets retargeter set target mesh request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetRetargeterTargetMeshBody, SetRetargeterTargetMeshResult));
	TestEqual(TEXT("IK assets retargeter set target mesh status code"), SetRetargeterTargetMeshResult.StatusCode, 200);

	const FString AutoMapChainsBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rtg-automap-001"),
		TEXT("ik_retargeter_auto_map_chains"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"auto_map_type\":\"exact\",\"force_remap\":true,\"save_after_set\":false}"), *RetargeterAssetPath));
	FHttpSmokeResult AutoMapChainsResult;
	TestTrue(TEXT("IK assets retargeter auto map chains request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AutoMapChainsBody, AutoMapChainsResult));
	TestEqual(TEXT("IK assets retargeter auto map chains status code"), AutoMapChainsResult.StatusCode, 200);

	const FString SetRetargeterSettingsBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rtg-settings-001"),
		TEXT("ik_retargeter_set_settings"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"global_settings\":{\"enable_root\":false,\"enable_fk\":false,\"copy_base_pose\":true,\"copy_base_pose_root\":\"%s\",\"source_scale_factor\":1.25,\"warping\":true,\"direction_source\":\"chain\",\"forward_direction\":\"y\",\"direction_chain\":\"%s\",\"warp_forwards\":1.15,\"sideways_offset\":2.5,\"warp_splay\":1.2},\"root_settings\":{\"rotation_alpha\":0.75,\"translation_alpha\":0.6,\"blend_to_source\":0.25,\"blend_to_source_weights\":{\"x\":1.0,\"y\":0.5,\"z\":0.25},\"scale_horizontal\":1.1,\"scale_vertical\":0.9,\"translation_offset\":{\"x\":4.0,\"y\":5.0,\"z\":6.0},\"rotation_offset\":{\"pitch\":7.0,\"yaw\":8.0,\"roll\":9.0},\"affect_ik_horizontal\":0.4,\"affect_ik_vertical\":0.3},\"save_after_set\":false}"),
			*RetargeterAssetPath,
			*RootBoneName,
			*ChainName));
	FHttpSmokeResult SetRetargeterSettingsResult;
	TestTrue(TEXT("IK assets retargeter set settings request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetRetargeterSettingsBody, SetRetargeterSettingsResult));
	TestEqual(TEXT("IK assets retargeter set settings status code"), SetRetargeterSettingsResult.StatusCode, 200);

	const FString CreateTargetPoseBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rtg-pose-001"),
		TEXT("ik_retargeter_set_pose"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"source_or_target\":\"target\",\"pose_name\":\"%s\",\"create_if_missing\":true,\"set_current\":true,\"save_after_set\":false}"), *RetargeterAssetPath, *TargetPoseName));
	FHttpSmokeResult CreateTargetPoseResult;
	TestTrue(TEXT("IK assets retargeter create pose request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateTargetPoseBody, CreateTargetPoseResult));
	TestEqual(TEXT("IK assets retargeter create pose status code"), CreateTargetPoseResult.StatusCode, 200);

	const FString UpdateTargetPoseBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rtg-pose-002"),
		TEXT("ik_retargeter_set_pose"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"source_or_target\":\"target\",\"pose_name\":\"%s\",\"set_current\":true,\"root_offset\":{\"x\":1.0,\"y\":2.0,\"z\":3.0},\"bone_rotation_offsets\":[{\"bone_name\":\"%s\",\"rotation_offset\":{\"pitch\":5.0,\"yaw\":10.0,\"roll\":15.0}}],\"save_after_set\":false}"), *RetargeterAssetPath, *TargetPoseName, *SecondaryBoneName));
	FHttpSmokeResult UpdateTargetPoseResult;
	TestTrue(TEXT("IK assets retargeter update pose request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, UpdateTargetPoseBody, UpdateTargetPoseResult));
	TestEqual(TEXT("IK assets retargeter update pose status code"), UpdateTargetPoseResult.StatusCode, 200);

	const FString DuplicateTargetPoseBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rtg-pose-003"),
		TEXT("ik_retargeter_set_pose"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"source_or_target\":\"target\",\"pose_name\":\"%s\",\"duplicate_from_pose\":\"%s\",\"save_after_set\":false}"), *RetargeterAssetPath, *TargetPoseCopyName, *TargetPoseName));
	FHttpSmokeResult DuplicateTargetPoseResult;
	TestTrue(TEXT("IK assets retargeter duplicate pose request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DuplicateTargetPoseBody, DuplicateTargetPoseResult));
	TestEqual(TEXT("IK assets retargeter duplicate pose status code"), DuplicateTargetPoseResult.StatusCode, 200);

	const FString RenameTargetPoseBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rtg-pose-004"),
		TEXT("ik_retargeter_set_pose"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"source_or_target\":\"target\",\"pose_name\":\"%s\",\"rename_from_pose\":\"%s\",\"set_current\":true,\"save_after_set\":false}"), *RetargeterAssetPath, *TargetPoseRenamed, *TargetPoseCopyName));
	FHttpSmokeResult RenameTargetPoseResult;
	TestTrue(TEXT("IK assets retargeter rename pose request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RenameTargetPoseBody, RenameTargetPoseResult));
	TestEqual(TEXT("IK assets retargeter rename pose status code"), RenameTargetPoseResult.StatusCode, 200);

	const FString UpdateRenamedTargetPoseBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rtg-pose-004b"),
		TEXT("ik_retargeter_set_pose"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"source_or_target\":\"target\",\"pose_name\":\"%s\",\"set_current\":true,\"root_offset\":{\"x\":1.0,\"y\":2.0,\"z\":3.0},\"bone_rotation_offsets\":[{\"bone_name\":\"%s\",\"rotation_offset\":{\"pitch\":5.0,\"yaw\":10.0,\"roll\":15.0}}],\"save_after_set\":false}"), *RetargeterAssetPath, *TargetPoseRenamed, *SecondaryBoneName));
	FHttpSmokeResult UpdateRenamedTargetPoseResult;
	TestTrue(TEXT("IK assets retargeter update renamed pose request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, UpdateRenamedTargetPoseBody, UpdateRenamedTargetPoseResult));
	TestEqual(TEXT("IK assets retargeter update renamed pose status code"), UpdateRenamedTargetPoseResult.StatusCode, 200);

	const FString RemoveOriginalTargetPoseBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rtg-pose-005"),
		TEXT("ik_retargeter_set_pose"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"source_or_target\":\"target\",\"pose_name\":\"%s\",\"remove\":true,\"save_after_set\":false}"), *RetargeterAssetPath, *TargetPoseName));
	FHttpSmokeResult RemoveOriginalTargetPoseResult;
	TestTrue(TEXT("IK assets retargeter remove pose request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveOriginalTargetPoseBody, RemoveOriginalTargetPoseResult));
	TestEqual(TEXT("IK assets retargeter remove pose status code"), RemoveOriginalTargetPoseResult.StatusCode, 200);

	const FString RetargeterInfoBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rtg-info-001"),
		TEXT("ik_retargeter_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *RetargeterAssetPath));
	FHttpSmokeResult RetargeterInfoResult;
	TestTrue(TEXT("IK assets retargeter get_info request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RetargeterInfoBody, RetargeterInfoResult));
	TestEqual(TEXT("IK assets retargeter get_info status code"), RetargeterInfoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> RetargeterInfoJson;
	TestTrue(TEXT("IK assets retargeter get_info parses JSON"), ParseJson(RetargeterInfoResult.Body, RetargeterInfoJson));
	const TSharedPtr<FJsonObject>* RetargeterInfoData = nullptr;
	TestTrue(TEXT("IK assets retargeter get_info contains data object"), RetargeterInfoJson.IsValid() && RetargeterInfoJson->TryGetObjectField(TEXT("data"), RetargeterInfoData) && RetargeterInfoData && RetargeterInfoData->IsValid());
	if (RetargeterInfoData && RetargeterInfoData->IsValid())
	{
		TestTrue(TEXT("IK assets retargeter source rig updated"), (*RetargeterInfoData)->GetStringField(TEXT("source_ik_rig")).StartsWith(SourceIKRigAssetPath));
		TestTrue(TEXT("IK assets retargeter target rig updated"), (*RetargeterInfoData)->GetStringField(TEXT("target_ik_rig")).StartsWith(TargetIKRigAssetPath));
		TestTrue(TEXT("IK assets retargeter op count >= 1"), static_cast<int32>((*RetargeterInfoData)->GetIntegerField(TEXT("retarget_op_count"))) >= 1);
		TestEqual(TEXT("IK assets retargeter current target pose updated"), (*RetargeterInfoData)->GetStringField(TEXT("target_retarget_pose")), TargetPoseRenamed);
		const TArray<TSharedPtr<FJsonValue>>* TargetPoseNames = nullptr;
		TestTrue(TEXT("IK assets retargeter target pose name array exists"), (*RetargeterInfoData)->TryGetArrayField(TEXT("target_pose_names"), TargetPoseNames) && TargetPoseNames != nullptr);
		bool bHasRenamedPose = false;
		bool bHasRemovedOriginalPose = false;
		if (TargetPoseNames)
		{
			for (const TSharedPtr<FJsonValue>& PoseValue : *TargetPoseNames)
			{
				if (!PoseValue.IsValid())
				{
					continue;
				}
				const FString PoseName = PoseValue->AsString();
				bHasRenamedPose |= (PoseName == TargetPoseRenamed);
				bHasRemovedOriginalPose |= (PoseName == TargetPoseName);
			}
		}
		TestTrue(TEXT("IK assets retargeter renamed pose exists"), bHasRenamedPose);
		TestFalse(TEXT("IK assets retargeter original pose removed"), bHasRemovedOriginalPose);
		const TSharedPtr<FJsonObject>* CurrentTargetPose = nullptr;
		TestTrue(TEXT("IK assets retargeter current target pose data exists"), (*RetargeterInfoData)->TryGetObjectField(TEXT("current_target_pose"), CurrentTargetPose) && CurrentTargetPose && CurrentTargetPose->IsValid());
		if (CurrentTargetPose && CurrentTargetPose->IsValid())
		{
			const TSharedPtr<FJsonObject>* RootOffset = nullptr;
			TestTrue(TEXT("IK assets retargeter current target pose root offset exists"), (*CurrentTargetPose)->TryGetObjectField(TEXT("root_offset"), RootOffset) && RootOffset && RootOffset->IsValid());
			TestTrue(TEXT("IK assets retargeter target pose has bone offsets"), static_cast<int32>((*CurrentTargetPose)->GetIntegerField(TEXT("bone_rotation_offset_count"))) >= 1);
		}

		const TSharedPtr<FJsonObject>* GlobalSettings = nullptr;
		TestTrue(TEXT("IK assets retargeter global settings exists"), (*RetargeterInfoData)->TryGetObjectField(TEXT("global_settings"), GlobalSettings) && GlobalSettings && GlobalSettings->IsValid());
		if (GlobalSettings && GlobalSettings->IsValid())
		{
			TestFalse(TEXT("IK assets retargeter global enable_root updated"), (*GlobalSettings)->GetBoolField(TEXT("enable_root")));
			TestFalse(TEXT("IK assets retargeter global enable_fk updated"), (*GlobalSettings)->GetBoolField(TEXT("enable_fk")));
			TestTrue(TEXT("IK assets retargeter global copy_base_pose updated"), (*GlobalSettings)->GetBoolField(TEXT("copy_base_pose")));
			TestEqual(TEXT("IK assets retargeter global direction source updated"), (*GlobalSettings)->GetStringField(TEXT("direction_source")), FString(TEXT("chain")));
			TestEqual(TEXT("IK assets retargeter global direction chain updated"), (*GlobalSettings)->GetStringField(TEXT("direction_chain")), ChainName);
		}

		const TSharedPtr<FJsonObject>* RootSettings = nullptr;
		TestTrue(TEXT("IK assets retargeter root settings exists"), (*RetargeterInfoData)->TryGetObjectField(TEXT("root_settings"), RootSettings) && RootSettings && RootSettings->IsValid());
		if (RootSettings && RootSettings->IsValid())
		{
			TestTrue(TEXT("IK assets retargeter root rotation alpha updated"), FMath::IsNearlyEqual(static_cast<float>((*RootSettings)->GetNumberField(TEXT("rotation_alpha"))), 0.75f));
			TestTrue(TEXT("IK assets retargeter root translation alpha updated"), FMath::IsNearlyEqual(static_cast<float>((*RootSettings)->GetNumberField(TEXT("translation_alpha"))), 0.6f));
			const TSharedPtr<FJsonObject>* TranslationOffset = nullptr;
			TestTrue(TEXT("IK assets retargeter root translation offset exists"), (*RootSettings)->TryGetObjectField(TEXT("translation_offset"), TranslationOffset) && TranslationOffset && TranslationOffset->IsValid());
		}
	}

	const FString IKRetargeterFolderPath = MakeSmokeAssetFolderPath(TEXT("IKRetargeter"), RetargeterAssetPath);
	IFileManager::Get().DeleteDirectory(*IKRetargeterFolderPath, false, true);
	const FString IKRetargeterFolderJsonPath = ToSmokeJsonPath(IKRetargeterFolderPath);

	const FString IKRetargeterExportFolderBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rtg-folder-export-001"),
		TEXT("ik_retargeter_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *RetargeterAssetPath));
	FHttpSmokeResult IKRetargeterExportFolderResult;
	TestTrue(TEXT("IK assets ik_retargeter_export_folder request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, IKRetargeterExportFolderBody, IKRetargeterExportFolderResult));
	TestEqual(TEXT("IK assets ik_retargeter_export_folder status code"), IKRetargeterExportFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> IKRetargeterExportFolderJson;
	TestTrue(TEXT("IK assets ik_retargeter_export_folder parses JSON"), ParseJson(IKRetargeterExportFolderResult.Body, IKRetargeterExportFolderJson));
	const TSharedPtr<FJsonObject>* IKRetargeterExportFolderData = nullptr;
	TestTrue(TEXT("IK assets ik_retargeter_export_folder contains data object"), IKRetargeterExportFolderJson.IsValid() && IKRetargeterExportFolderJson->TryGetObjectField(TEXT("data"), IKRetargeterExportFolderData) && IKRetargeterExportFolderData && IKRetargeterExportFolderData->IsValid());
	if (IKRetargeterExportFolderData && IKRetargeterExportFolderData->IsValid())
	{
		TestTrue(TEXT("IK assets ik retargeter folder export marked complete"), (*IKRetargeterExportFolderData)->GetBoolField(TEXT("is_complete_target_schema")));
		TestTrue(TEXT("IK assets ik retargeter folder export wrote files"), static_cast<int32>((*IKRetargeterExportFolderData)->GetIntegerField(TEXT("file_count"))) >= 12);
	}

	TSharedPtr<FJsonObject> IKRetargeterAssetFolderJson;
	TestTrue(TEXT("IK assets ik retargeter folder asset.json exists"), LoadSmokeJsonFile(this, FPaths::Combine(IKRetargeterFolderPath, TEXT("asset.json")), IKRetargeterAssetFolderJson, TEXT("IKRetargeter folder asset.json")));
	TSharedPtr<FJsonObject> IKRetargeterCoverageJson;
	TestTrue(TEXT("IK assets ik retargeter folder coverage exists"), LoadSmokeJsonFile(this, FPaths::Combine(IKRetargeterFolderPath, TEXT("validation"), TEXT("coverage_report.json")), IKRetargeterCoverageJson, TEXT("IKRetargeter folder coverage_report.json")));

	const FString IKRetargeterValidateFolderBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rtg-folder-validate-001"),
		TEXT("ik_retargeter_validate_folder"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\"}"), *IKRetargeterFolderJsonPath));
	FHttpSmokeResult IKRetargeterValidateFolderResult;
	TestTrue(TEXT("IK assets ik_retargeter_validate_folder request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, IKRetargeterValidateFolderBody, IKRetargeterValidateFolderResult));
	TestEqual(TEXT("IK assets ik_retargeter_validate_folder status code"), IKRetargeterValidateFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> IKRetargeterValidateFolderJson;
	TestTrue(TEXT("IK assets ik_retargeter_validate_folder parses JSON"), ParseJson(IKRetargeterValidateFolderResult.Body, IKRetargeterValidateFolderJson));
	const TSharedPtr<FJsonObject>* IKRetargeterValidateFolderData = nullptr;
	TestTrue(TEXT("IK assets ik_retargeter_validate_folder contains data object"), IKRetargeterValidateFolderJson.IsValid() && IKRetargeterValidateFolderJson->TryGetObjectField(TEXT("data"), IKRetargeterValidateFolderData) && IKRetargeterValidateFolderData && IKRetargeterValidateFolderData->IsValid());
	if (IKRetargeterValidateFolderData && IKRetargeterValidateFolderData->IsValid())
	{
		TestTrue(TEXT("IK assets ik_retargeter_validate_folder is dry run"), (*IKRetargeterValidateFolderData)->GetBoolField(TEXT("dry_run")));
		TestEqual(TEXT("IK assets ik_retargeter_validate_folder has no JSON issues"), static_cast<int32>((*IKRetargeterValidateFolderData)->GetIntegerField(TEXT("json_issue_count"))), 0);
	}

	const FString IKRetargeterApplyFolderBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rtg-folder-apply-001"),
		TEXT("ik_retargeter_apply_folder"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\",\"save_after_apply\":false}"), *IKRetargeterFolderJsonPath));
	FHttpSmokeResult IKRetargeterApplyFolderResult;
	TestTrue(TEXT("IK assets ik_retargeter_apply_folder request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, IKRetargeterApplyFolderBody, IKRetargeterApplyFolderResult));
	TestEqual(TEXT("IK assets ik_retargeter_apply_folder status code"), IKRetargeterApplyFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> IKRetargeterApplyFolderJson;
	TestTrue(TEXT("IK assets ik_retargeter_apply_folder parses JSON"), ParseJson(IKRetargeterApplyFolderResult.Body, IKRetargeterApplyFolderJson));
	const TSharedPtr<FJsonObject>* IKRetargeterApplyFolderData = nullptr;
	TestTrue(TEXT("IK assets ik_retargeter_apply_folder contains data object"), IKRetargeterApplyFolderJson.IsValid() && IKRetargeterApplyFolderJson->TryGetObjectField(TEXT("data"), IKRetargeterApplyFolderData) && IKRetargeterApplyFolderData && IKRetargeterApplyFolderData->IsValid());
	if (IKRetargeterApplyFolderData && IKRetargeterApplyFolderData->IsValid())
	{
		TestTrue(TEXT("IK assets ik_retargeter_apply_folder applied"), (*IKRetargeterApplyFolderData)->GetBoolField(TEXT("applied")));
		TestEqual(TEXT("IK assets ik_retargeter_apply_folder has no JSON issues"), static_cast<int32>((*IKRetargeterApplyFolderData)->GetIntegerField(TEXT("json_issue_count"))), 0);
	}

	const FString DuplicateAndRetargetBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-retarget-001"),
		TEXT("ik_retargeter_duplicate_and_retarget"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"asset_paths\":[\"%s\"],\"source_mesh_path\":\"%s\",\"target_mesh_path\":\"%s\",\"output_folder\":\"%s\",\"prefix\":\"RTG20_\",\"include_referenced_assets\":false}"), *RetargeterAssetPath, *SourceAnimationAssetPath, *SourceSkeletalMeshPath, *TargetMeshAssetPath, *RetargetOutputFolder));
	FHttpSmokeResult DuplicateAndRetargetResult;
	TestTrue(TEXT("IK assets duplicate_and_retarget request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DuplicateAndRetargetBody, DuplicateAndRetargetResult));
	TestEqual(TEXT("IK assets duplicate_and_retarget status code"), DuplicateAndRetargetResult.StatusCode, 200);

	TSharedPtr<FJsonObject> DuplicateAndRetargetJson;
	TestTrue(TEXT("IK assets duplicate_and_retarget parses JSON"), ParseJson(DuplicateAndRetargetResult.Body, DuplicateAndRetargetJson));
	const TSharedPtr<FJsonObject>* DuplicateAndRetargetData = nullptr;
	TestTrue(TEXT("IK assets duplicate_and_retarget contains data object"), DuplicateAndRetargetJson.IsValid() && DuplicateAndRetargetJson->TryGetObjectField(TEXT("data"), DuplicateAndRetargetData) && DuplicateAndRetargetData && DuplicateAndRetargetData->IsValid());
	if (DuplicateAndRetargetData && DuplicateAndRetargetData->IsValid())
	{
		TestTrue(TEXT("IK assets duplicate_and_retarget created assets"), static_cast<int32>((*DuplicateAndRetargetData)->GetIntegerField(TEXT("created_asset_count"))) >= 1);
	}

	const FString RetargetBatchOutputFolder = FString::Printf(TEXT("/Game/__UeAgentInterfaceSmoke/RetargetBatch_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString RetargetBatchJsonFile = ToSmokeJsonPath(FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("RetargetBatch"), FString::Printf(TEXT("Batch_%s.json"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)))));

	const FString RetargetBatchExportJsonBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-retarget-batch-export-001"),
		TEXT("retarget_batch_export_json"),
		FString::Printf(
			TEXT("{\"output_file\":\"%s\",\"retargeter\":\"%s\",\"asset_paths\":[\"%s\"],\"source_mesh\":\"%s\",\"target_mesh\":\"%s\",\"output_folder\":\"%s\",\"prefix\":\"RTGBatch_\"}"),
			*RetargetBatchJsonFile,
			*RetargeterAssetPath,
			*SourceAnimationAssetPath,
			*SourceSkeletalMeshPath,
			*TargetMeshAssetPath,
			*RetargetBatchOutputFolder));
	FHttpSmokeResult RetargetBatchExportJsonResult;
	TestTrue(TEXT("IK assets retarget_batch_export_json request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RetargetBatchExportJsonBody, RetargetBatchExportJsonResult));
	TestEqual(TEXT("IK assets retarget_batch_export_json status code"), RetargetBatchExportJsonResult.StatusCode, 200);

	TSharedPtr<FJsonObject> RetargetBatchFileJson;
	TestTrue(TEXT("IK assets retarget batch file exists"), LoadSmokeJsonFile(this, RetargetBatchJsonFile, RetargetBatchFileJson, TEXT("Retarget batch json")));

	const FString RetargetBatchValidateJsonBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-retarget-batch-validate-001"),
		TEXT("retarget_batch_validate_json"),
		FString::Printf(TEXT("{\"json_file\":\"%s\"}"), *RetargetBatchJsonFile));
	FHttpSmokeResult RetargetBatchValidateJsonResult;
	TestTrue(TEXT("IK assets retarget_batch_validate_json request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RetargetBatchValidateJsonBody, RetargetBatchValidateJsonResult));
	TestEqual(TEXT("IK assets retarget_batch_validate_json status code"), RetargetBatchValidateJsonResult.StatusCode, 200);

	TSharedPtr<FJsonObject> RetargetBatchValidateJson;
	TestTrue(TEXT("IK assets retarget_batch_validate_json parses JSON"), ParseJson(RetargetBatchValidateJsonResult.Body, RetargetBatchValidateJson));
	const TSharedPtr<FJsonObject>* RetargetBatchValidateData = nullptr;
	TestTrue(TEXT("IK assets retarget_batch_validate_json contains data object"), RetargetBatchValidateJson.IsValid() && RetargetBatchValidateJson->TryGetObjectField(TEXT("data"), RetargetBatchValidateData) && RetargetBatchValidateData && RetargetBatchValidateData->IsValid());
	if (RetargetBatchValidateData && RetargetBatchValidateData->IsValid())
	{
		TestTrue(TEXT("IK assets retarget_batch_validate_json valid"), (*RetargetBatchValidateData)->GetBoolField(TEXT("valid")));
		TestEqual(TEXT("IK assets retarget_batch_validate_json has no JSON issues"), static_cast<int32>((*RetargetBatchValidateData)->GetIntegerField(TEXT("json_issue_count"))), 0);
	}

	const FString EmptyRetargetBatchJsonFile = ToSmokeJsonPath(FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("RetargetBatch"), FString::Printf(TEXT("BatchEmpty_%s.json"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)))));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(EmptyRetargetBatchJsonFile), true);
	TestTrue(TEXT("IK assets empty retarget batch json saved"), FFileHelper::SaveStringToFile(
		FString::Printf(TEXT("{\"schema\":\"ue_agent_interface.retarget_batch.v1\",\"retargeter\":\"%s\",\"source_assets\":[],\"output_folder\":\"%s\"}"), *RetargeterAssetPath, *RetargetBatchOutputFolder),
		*EmptyRetargetBatchJsonFile,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	const FString EmptyRetargetBatchValidateBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-retarget-batch-empty-validate-001"),
		TEXT("retarget_batch_validate_json"),
		FString::Printf(TEXT("{\"json_file\":\"%s\"}"), *EmptyRetargetBatchJsonFile));
	FHttpSmokeResult EmptyRetargetBatchValidateResult;
	TestTrue(TEXT("IK assets empty retarget_batch_validate_json request completed"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, EmptyRetargetBatchValidateBody, EmptyRetargetBatchValidateResult));
	TestTrue(TEXT("IK assets empty retarget_batch_validate_json returns command failure"), EmptyRetargetBatchValidateResult.StatusCode == 200 || EmptyRetargetBatchValidateResult.StatusCode == 400);
	TSharedPtr<FJsonObject> EmptyRetargetBatchValidateJson;
	TestTrue(TEXT("IK assets empty retarget_batch_validate_json parses JSON"), ParseJson(EmptyRetargetBatchValidateResult.Body, EmptyRetargetBatchValidateJson));
	if (EmptyRetargetBatchValidateJson.IsValid())
	{
		bool bEmptyBatchOk = true;
		EmptyRetargetBatchValidateJson->TryGetBoolField(TEXT("ok"), bEmptyBatchOk);
		TestFalse(TEXT("IK assets empty retarget_batch_validate_json ok=false"), bEmptyBatchOk);
		TestTrue(TEXT("IK assets empty retarget_batch_validate_json reports empty source assets"), EmptyRetargetBatchValidateResult.Body.Contains(TEXT("json_empty_required_field")));
	}

	const FString RetargetBatchApplyJsonBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-retarget-batch-apply-001"),
		TEXT("retarget_batch_apply_json"),
		FString::Printf(TEXT("{\"json_file\":\"%s\"}"), *RetargetBatchJsonFile));
	FHttpSmokeResult RetargetBatchApplyJsonResult;
	TestTrue(TEXT("IK assets retarget_batch_apply_json request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RetargetBatchApplyJsonBody, RetargetBatchApplyJsonResult));
	TestEqual(TEXT("IK assets retarget_batch_apply_json status code"), RetargetBatchApplyJsonResult.StatusCode, 200);

	TSharedPtr<FJsonObject> RetargetBatchApplyJson;
	TestTrue(TEXT("IK assets retarget_batch_apply_json parses JSON"), ParseJson(RetargetBatchApplyJsonResult.Body, RetargetBatchApplyJson));
	const TSharedPtr<FJsonObject>* RetargetBatchApplyData = nullptr;
	TestTrue(TEXT("IK assets retarget_batch_apply_json contains data object"), RetargetBatchApplyJson.IsValid() && RetargetBatchApplyJson->TryGetObjectField(TEXT("data"), RetargetBatchApplyData) && RetargetBatchApplyData && RetargetBatchApplyData->IsValid());
	if (RetargetBatchApplyData && RetargetBatchApplyData->IsValid())
	{
		TestTrue(TEXT("IK assets retarget_batch_apply_json created assets"), static_cast<int32>((*RetargetBatchApplyData)->GetIntegerField(TEXT("created_asset_count"))) >= 1);
		TestEqual(TEXT("IK assets retarget_batch_apply_json has no JSON issues"), static_cast<int32>((*RetargetBatchApplyData)->GetIntegerField(TEXT("json_issue_count"))), 0);
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceNiagaraFolderSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.NiagaraFolderWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceNiagaraFolderSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString SystemAssetPath = MakeAutomationAssetPath(TEXT("NSFolder"));
	const FString EmitterAssetPath = MakeAutomationAssetPath(TEXT("NEFolder"));
	const FString AppliedSystemAssetPath = MakeAutomationAssetPath(TEXT("NSFolderApplied"));
	const FString AppliedEmitterAssetPath = MakeAutomationAssetPath(TEXT("NEFolderApplied"));
	const FString ScriptAssetPath = TEXT("/Niagara/Modules/Arrays/FillFloatArray");
	const FString AppliedScriptAssetPath = MakeAutomationAssetPath(TEXT("NMScriptFolderApplied"));
	const FString DeleteRecreateEmitterAssetPath = MakeAutomationAssetPath(TEXT("NEDeleteRecreate"));

	FString RelativeFolder = SystemAssetPath;
	while (RelativeFolder.StartsWith(TEXT("/")))
	{
		RelativeFolder.RightChopInline(1);
	}
	const FString FolderPath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("NiagaraSystem"), RelativeFolder));
	IFileManager::Get().DeleteDirectory(*FolderPath, false, true);

	FString RelativeEmitterFolder = EmitterAssetPath;
	while (RelativeEmitterFolder.StartsWith(TEXT("/")))
	{
		RelativeEmitterFolder.RightChopInline(1);
	}
	const FString EmitterFolderPath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("NiagaraEmitter"), RelativeEmitterFolder));
	IFileManager::Get().DeleteDirectory(*EmitterFolderPath, false, true);

	const FString ScriptFolderPath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("NiagaraScript"), TEXT("Smoke"), TEXT("FillFloatArray")));
	IFileManager::Get().DeleteDirectory(*ScriptFolderPath, false, true);

	auto LoadJsonFile = [this](const FString& FilePath, TSharedPtr<FJsonObject>& OutObj) -> bool
	{
		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
		{
			AddError(FString::Printf(TEXT("Niagara folder load failed: %s"), *FilePath));
			return false;
		}

		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, OutObj) || !OutObj.IsValid())
		{
			AddError(FString::Printf(TEXT("Niagara folder parse failed: %s"), *FilePath));
			return false;
		}
		return true;
	};

	const FString CreateEmitterBody = MakeExecRequestBody(
		TEXT("smoke-niagara-folder-create-emitter-001"),
		TEXT("niagara_create_emitter"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"add_default_modules_and_renderers\":true,\"save_after_create\":false}"), *EmitterAssetPath));
	FHttpSmokeResult CreateEmitterResult;
	TestTrue(TEXT("Niagara folder create emitter request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateEmitterBody, CreateEmitterResult, 20.0));
	TestEqual(TEXT("Niagara folder create emitter status code"), CreateEmitterResult.StatusCode, 200);

	const FString AddRendererBody = MakeExecRequestBody(
		TEXT("smoke-niagara-folder-add-renderer-001"),
		TEXT("niagara_emitter_add_renderer"),
		FString::Printf(TEXT("{\"emitter_asset_path\":\"%s\",\"renderer_type\":\"sprite\",\"renderer_name\":\"FolderSmokeSprite\",\"compile_after_set\":false,\"save_after_set\":false}"), *EmitterAssetPath));
	FHttpSmokeResult AddRendererResult;
	TestTrue(TEXT("Niagara folder add renderer request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddRendererBody, AddRendererResult, 20.0));
	TestEqual(TEXT("Niagara folder add renderer status code"), AddRendererResult.StatusCode, 200);

	const FString EmitterExportBody = MakeExecRequestBody(
		TEXT("smoke-niagara-emitter-folder-export-001"),
		TEXT("niagara_emitter_export_folder"),
		FString::Printf(TEXT("{\"emitter_asset_path\":\"%s\",\"clean_output_dir\":true}"), *EmitterAssetPath));
	FHttpSmokeResult EmitterExportResult;
	TestTrue(TEXT("Niagara emitter folder export request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, EmitterExportBody, EmitterExportResult, 30.0));
	TestEqual(TEXT("Niagara emitter folder export status code"), EmitterExportResult.StatusCode, 200);

	TSharedPtr<FJsonObject> EmitterExportJson;
	TestTrue(TEXT("Niagara emitter folder export parses JSON"), ParseJson(EmitterExportResult.Body, EmitterExportJson));
	TestTrue(TEXT("Niagara emitter folder export root ok=true"), EmitterExportJson.IsValid() && EmitterExportJson->GetBoolField(TEXT("ok")));
	const TSharedPtr<FJsonObject>* EmitterExportData = nullptr;
	TestTrue(TEXT("Niagara emitter folder export contains data object"), EmitterExportJson.IsValid() && EmitterExportJson->TryGetObjectField(TEXT("data"), EmitterExportData) && EmitterExportData && EmitterExportData->IsValid());
	if (EmitterExportData && EmitterExportData->IsValid())
	{
		TestEqual(TEXT("Niagara emitter folder export folder path"), (*EmitterExportData)->GetStringField(TEXT("folder_path")), EmitterFolderPath);
		TestTrue(TEXT("Niagara emitter folder export has files"), static_cast<int32>((*EmitterExportData)->GetIntegerField(TEXT("file_count"))) >= 8);
	}

	TSharedPtr<FJsonObject> EmitterCoverageJson;
	TestTrue(TEXT("Niagara emitter folder exported coverage report exists"), LoadJsonFile(FPaths::Combine(EmitterFolderPath, TEXT("validation"), TEXT("coverage_report.json")), EmitterCoverageJson));
	if (EmitterCoverageJson.IsValid())
	{
		TestEqual(TEXT("Niagara emitter folder implementation status"), EmitterCoverageJson->GetStringField(TEXT("implementation_status")), FString(TEXT("complete_folder_profile")));
		TestTrue(TEXT("Niagara emitter folder complete target schema is true"), EmitterCoverageJson->GetBoolField(TEXT("is_complete_target_schema")));
		TestTrue(TEXT("Niagara emitter folder lossless roundtrip is true"), EmitterCoverageJson->GetBoolField(TEXT("is_lossless_roundtrip")));
		const TArray<TSharedPtr<FJsonValue>>* PendingProfiles = nullptr;
		TestTrue(TEXT("Niagara emitter folder pending profiles empty"), EmitterCoverageJson->TryGetArrayField(TEXT("pending_profiles"), PendingProfiles) && PendingProfiles && PendingProfiles->Num() == 0);
	}

	const FString EmitterApplyBody = MakeExecRequestBody(
		TEXT("smoke-niagara-emitter-folder-apply-001"),
		TEXT("niagara_emitter_apply_folder"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\",\"emitter_asset_path\":\"%s\",\"create_if_missing\":true,\"save_after_apply\":false,\"strict\":false}"), *EmitterFolderPath.Replace(TEXT("\\"), TEXT("/")), *AppliedEmitterAssetPath));
	FHttpSmokeResult EmitterApplyResult;
	TestTrue(TEXT("Niagara emitter folder apply request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, EmitterApplyBody, EmitterApplyResult, 30.0));
	TestEqual(TEXT("Niagara emitter folder apply status code"), EmitterApplyResult.StatusCode, 200);

	TSharedPtr<FJsonObject> EmitterApplyJson;
	TestTrue(TEXT("Niagara emitter folder apply parses JSON"), ParseJson(EmitterApplyResult.Body, EmitterApplyJson));
	TestTrue(TEXT("Niagara emitter folder apply root ok=true"), EmitterApplyJson.IsValid() && EmitterApplyJson->GetBoolField(TEXT("ok")));
	const TSharedPtr<FJsonObject>* EmitterApplyData = nullptr;
	TestTrue(TEXT("Niagara emitter folder apply contains data object"), EmitterApplyJson.IsValid() && EmitterApplyJson->TryGetObjectField(TEXT("data"), EmitterApplyData) && EmitterApplyData && EmitterApplyData->IsValid());
	if (EmitterApplyData && EmitterApplyData->IsValid())
	{
		TestTrue(TEXT("Niagara emitter folder apply created emitter"), (*EmitterApplyData)->GetBoolField(TEXT("created")));
		TestTrue(TEXT("Niagara emitter folder apply property count"), static_cast<int32>((*EmitterApplyData)->GetIntegerField(TEXT("emitter_properties_applied"))) >= 1);
		TestTrue(TEXT("Niagara emitter folder apply defaults compile after apply"), (*EmitterApplyData)->GetBoolField(TEXT("compile_after_apply")));
		TestTrue(TEXT("Niagara emitter folder apply defaults force compile"), (*EmitterApplyData)->GetBoolField(TEXT("force_compile")));
		TestTrue(TEXT("Niagara emitter folder apply compile log read ok"), (*EmitterApplyData)->GetBoolField(TEXT("compile_log_read_ok")));
		TestTrue(TEXT("Niagara emitter folder apply compilation complete"), (*EmitterApplyData)->GetBoolField(TEXT("compilation_complete")));
		TestFalse(TEXT("Niagara emitter folder apply compile has no error"), (*EmitterApplyData)->GetBoolField(TEXT("compile_has_error")));
	}

	const FString AppliedEmitterRenderersBody = MakeExecRequestBody(
		TEXT("smoke-niagara-emitter-folder-applied-renderers-001"),
		TEXT("niagara_emitter_list_renderers"),
		FString::Printf(TEXT("{\"emitter_asset_path\":\"%s\"}"), *AppliedEmitterAssetPath));
	FHttpSmokeResult AppliedEmitterRenderersResult;
	TestTrue(TEXT("Niagara emitter folder applied list renderers request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AppliedEmitterRenderersBody, AppliedEmitterRenderersResult, 20.0));
	TestEqual(TEXT("Niagara emitter folder applied list renderers status code"), AppliedEmitterRenderersResult.StatusCode, 200);
	TSharedPtr<FJsonObject> AppliedEmitterRenderersJson;
	TestTrue(TEXT("Niagara emitter folder applied list renderers parses JSON"), ParseJson(AppliedEmitterRenderersResult.Body, AppliedEmitterRenderersJson));
	const TSharedPtr<FJsonObject>* AppliedEmitterRenderersData = nullptr;
	TestTrue(TEXT("Niagara emitter folder applied list renderers contains data object"), AppliedEmitterRenderersJson.IsValid() && AppliedEmitterRenderersJson->TryGetObjectField(TEXT("data"), AppliedEmitterRenderersData) && AppliedEmitterRenderersData && AppliedEmitterRenderersData->IsValid());
	if (AppliedEmitterRenderersData && AppliedEmitterRenderersData->IsValid())
	{
		TestTrue(TEXT("Niagara emitter folder applied has renderer"), static_cast<int32>((*AppliedEmitterRenderersData)->GetIntegerField(TEXT("renderer_count"))) >= 1);
	}

	const FString ScriptExportBody = MakeExecRequestBody(
		TEXT("smoke-niagara-script-folder-export-001"),
		TEXT("niagara_script_export_folder"),
		FString::Printf(TEXT("{\"script_asset_path\":\"%s\",\"folder_path\":\"%s\",\"clean_output_dir\":true}"), *ScriptAssetPath, *ScriptFolderPath.Replace(TEXT("\\"), TEXT("/"))));
	FHttpSmokeResult ScriptExportResult;
	TestTrue(TEXT("Niagara script folder export request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ScriptExportBody, ScriptExportResult, 30.0));
	TestEqual(TEXT("Niagara script folder export status code"), ScriptExportResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ScriptExportJson;
	TestTrue(TEXT("Niagara script folder export parses JSON"), ParseJson(ScriptExportResult.Body, ScriptExportJson));
	TestTrue(TEXT("Niagara script folder export root ok=true"), ScriptExportJson.IsValid() && ScriptExportJson->GetBoolField(TEXT("ok")));

	TSharedPtr<FJsonObject> ScriptCoverageJson;
	TestTrue(TEXT("Niagara script folder exported coverage report exists"), LoadJsonFile(FPaths::Combine(ScriptFolderPath, TEXT("validation"), TEXT("coverage_report.json")), ScriptCoverageJson));
	if (ScriptCoverageJson.IsValid())
	{
		TestEqual(TEXT("Niagara script folder implementation status"), ScriptCoverageJson->GetStringField(TEXT("implementation_status")), FString(TEXT("complete_folder_profile")));
		TestTrue(TEXT("Niagara script folder complete target schema is true"), ScriptCoverageJson->GetBoolField(TEXT("is_complete_target_schema")));
	}

	const FString ScriptApplyBody = MakeExecRequestBody(
		TEXT("smoke-niagara-script-folder-apply-001"),
		TEXT("niagara_script_apply_folder"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\",\"script_asset_path\":\"%s\",\"create_if_missing\":true,\"compile_after_apply\":false,\"save_after_apply\":false,\"strict\":false}"), *ScriptFolderPath.Replace(TEXT("\\"), TEXT("/")), *AppliedScriptAssetPath));
	FHttpSmokeResult ScriptApplyResult;
	TestTrue(TEXT("Niagara script folder apply request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ScriptApplyBody, ScriptApplyResult, 30.0));
	TestEqual(TEXT("Niagara script folder apply status code"), ScriptApplyResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ScriptApplyJson;
	TestTrue(TEXT("Niagara script folder apply parses JSON"), ParseJson(ScriptApplyResult.Body, ScriptApplyJson));
	TestTrue(TEXT("Niagara script folder apply root ok=true"), ScriptApplyJson.IsValid() && ScriptApplyJson->GetBoolField(TEXT("ok")));
	const TSharedPtr<FJsonObject>* ScriptApplyData = nullptr;
	TestTrue(TEXT("Niagara script folder apply contains data object"), ScriptApplyJson.IsValid() && ScriptApplyJson->TryGetObjectField(TEXT("data"), ScriptApplyData) && ScriptApplyData && ScriptApplyData->IsValid());
	if (ScriptApplyData && ScriptApplyData->IsValid())
	{
		TestTrue(TEXT("Niagara script folder apply created script"), (*ScriptApplyData)->GetBoolField(TEXT("created")));
	}

	const FString CreateSystemBody = MakeExecRequestBody(
		TEXT("smoke-niagara-folder-create-system-001"),
		TEXT("niagara_create_system"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"create_default_nodes\":true,\"save_after_create\":false}"), *SystemAssetPath));
	FHttpSmokeResult CreateSystemResult;
	TestTrue(TEXT("Niagara folder create system request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateSystemBody, CreateSystemResult, 20.0));
	TestEqual(TEXT("Niagara folder create system status code"), CreateSystemResult.StatusCode, 200);

	const FString AddEmitterBody = MakeExecRequestBody(
		TEXT("smoke-niagara-folder-add-emitter-001"),
		TEXT("niagara_system_add_emitter"),
		FString::Printf(TEXT("{\"system_asset_path\":\"%s\",\"emitter_asset_path\":\"%s\",\"emitter_name\":\"FolderSmokeEmitter\",\"compile_after_set\":false,\"save_after_set\":false}"), *SystemAssetPath, *EmitterAssetPath));
	FHttpSmokeResult AddEmitterResult;
	TestTrue(TEXT("Niagara folder add emitter request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddEmitterBody, AddEmitterResult, 20.0));
	TestEqual(TEXT("Niagara folder add emitter status code"), AddEmitterResult.StatusCode, 200);

	const FString AddUserParameterBody = MakeExecRequestBody(
		TEXT("smoke-niagara-folder-add-user-param-001"),
		TEXT("niagara_user_parameter_add"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"parameter_name\":\"FolderSmokeFloat\",\"parameter_type\":\"float\",\"default_value_text\":\"3.500000\",\"compile_after_set\":false,\"save_after_set\":false}"), *SystemAssetPath));
	FHttpSmokeResult AddUserParameterResult;
	TestTrue(TEXT("Niagara folder add user parameter request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddUserParameterBody, AddUserParameterResult, 20.0));
	TestEqual(TEXT("Niagara folder add user parameter status code"), AddUserParameterResult.StatusCode, 200);

	const FString ExportBody = MakeExecRequestBody(
		TEXT("smoke-niagara-folder-export-001"),
		TEXT("niagara_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"clean_output_dir\":true}"), *SystemAssetPath));
	FHttpSmokeResult ExportResult;
	TestTrue(TEXT("Niagara folder export request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ExportBody, ExportResult, 30.0));
	TestEqual(TEXT("Niagara folder export status code"), ExportResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ExportJson;
	TestTrue(TEXT("Niagara folder export parses JSON"), ParseJson(ExportResult.Body, ExportJson));
	TestTrue(TEXT("Niagara folder export root ok=true"), ExportJson.IsValid() && ExportJson->GetBoolField(TEXT("ok")));
	const TSharedPtr<FJsonObject>* ExportData = nullptr;
	TestTrue(TEXT("Niagara folder export contains data object"), ExportJson.IsValid() && ExportJson->TryGetObjectField(TEXT("data"), ExportData) && ExportData && ExportData->IsValid());
	if (ExportData && ExportData->IsValid())
	{
		TestEqual(TEXT("Niagara folder export folder path"), (*ExportData)->GetStringField(TEXT("folder_path")), FolderPath);
		TestTrue(TEXT("Niagara folder export has files"), static_cast<int32>((*ExportData)->GetIntegerField(TEXT("file_count"))) >= 12);
		TestTrue(TEXT("Niagara folder export has emitter"), static_cast<int32>((*ExportData)->GetIntegerField(TEXT("emitter_count"))) >= 1);
		TestTrue(TEXT("Niagara folder export has user parameter"), static_cast<int32>((*ExportData)->GetIntegerField(TEXT("user_parameter_count"))) >= 1);
	}

	TSharedPtr<FJsonObject> AssetJson;
	TestTrue(TEXT("Niagara folder exported asset.json exists"), LoadJsonFile(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetJson));
	if (AssetJson.IsValid())
	{
		TestEqual(TEXT("Niagara folder exported asset kind"), AssetJson->GetStringField(TEXT("asset_kind")), FString(TEXT("niagara_system")));
		TestEqual(TEXT("Niagara folder exported asset path"), AssetJson->GetStringField(TEXT("asset_path")), SystemAssetPath);
	}

	TSharedPtr<FJsonObject> EmittersIndexJson;
	TestTrue(TEXT("Niagara folder exported emitters index exists"), LoadJsonFile(FPaths::Combine(FolderPath, TEXT("emitters"), TEXT("index.json")), EmittersIndexJson));
	if (EmittersIndexJson.IsValid())
	{
		TestTrue(TEXT("Niagara folder exported emitters count >= 1"), static_cast<int32>(EmittersIndexJson->GetIntegerField(TEXT("emitter_count"))) >= 1);
	}

	TSharedPtr<FJsonObject> CoverageJson;
	TestTrue(TEXT("Niagara folder exported coverage report exists"), LoadJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("coverage_report.json")), CoverageJson));
	if (CoverageJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* CoverageAreas = nullptr;
		TestTrue(TEXT("Niagara folder coverage areas exist"), CoverageJson->TryGetArrayField(TEXT("coverage_areas"), CoverageAreas) && CoverageAreas != nullptr);
		TestTrue(TEXT("Niagara folder coverage area count reasonable"), CoverageAreas && CoverageAreas->Num() >= 8);
		TestEqual(TEXT("Niagara folder implementation status"), CoverageJson->GetStringField(TEXT("implementation_status")), FString(TEXT("complete_folder_profile")));
		TestTrue(TEXT("Niagara folder complete target schema is true"), CoverageJson->GetBoolField(TEXT("is_complete_target_schema")));
		TestTrue(TEXT("Niagara folder lossless roundtrip is true"), CoverageJson->GetBoolField(TEXT("is_lossless_roundtrip")));
		const TArray<TSharedPtr<FJsonValue>>* BlockingGaps = nullptr;
		TestTrue(TEXT("Niagara folder blocking gaps empty"), CoverageJson->TryGetArrayField(TEXT("blocking_gaps"), BlockingGaps) && BlockingGaps && BlockingGaps->Num() == 0);
	}

	const FString ApplyBody = MakeExecRequestBody(
		TEXT("smoke-niagara-folder-apply-001"),
		TEXT("niagara_apply_folder"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\",\"asset_path\":\"%s\",\"create_if_missing\":true,\"apply_referenced_emitters\":true,\"compile_after_apply\":true,\"force_compile\":true,\"wait_for_complete\":true,\"save_after_apply\":false,\"strict\":false}"), *FolderPath.Replace(TEXT("\\"), TEXT("/")), *AppliedSystemAssetPath));
	FHttpSmokeResult ApplyResult;
	TestTrue(TEXT("Niagara folder apply request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ApplyBody, ApplyResult, 30.0));
	TestEqual(TEXT("Niagara folder apply status code"), ApplyResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ApplyJson;
	TestTrue(TEXT("Niagara folder apply parses JSON"), ParseJson(ApplyResult.Body, ApplyJson));
	TestTrue(TEXT("Niagara folder apply root ok=true"), ApplyJson.IsValid() && ApplyJson->GetBoolField(TEXT("ok")));
	const TSharedPtr<FJsonObject>* ApplyData = nullptr;
	TestTrue(TEXT("Niagara folder apply contains data object"), ApplyJson.IsValid() && ApplyJson->TryGetObjectField(TEXT("data"), ApplyData) && ApplyData && ApplyData->IsValid());
	if (ApplyData && ApplyData->IsValid())
	{
		TestTrue(TEXT("Niagara folder apply created system"), (*ApplyData)->GetBoolField(TEXT("created")));
		TestTrue(TEXT("Niagara folder apply user parameters"), static_cast<int32>((*ApplyData)->GetIntegerField(TEXT("user_parameters_applied"))) >= 1);
		TestTrue(TEXT("Niagara folder apply emitter handles"), static_cast<int32>((*ApplyData)->GetIntegerField(TEXT("emitter_handles_applied"))) >= 1);
		TestTrue(TEXT("Niagara folder apply force compile"), (*ApplyData)->GetBoolField(TEXT("force_compile")));
		TestTrue(TEXT("Niagara folder apply wait for complete"), (*ApplyData)->GetBoolField(TEXT("wait_for_complete")));
		TestTrue(TEXT("Niagara folder apply compile log read ok"), (*ApplyData)->GetBoolField(TEXT("compile_log_read_ok")));
		TestTrue(TEXT("Niagara folder apply compilation complete"), (*ApplyData)->GetBoolField(TEXT("compilation_complete")));
		TestFalse(TEXT("Niagara folder apply no outstanding compile"), (*ApplyData)->GetBoolField(TEXT("has_outstanding_compilation_requests")));
		TestFalse(TEXT("Niagara folder apply compile has no error"), (*ApplyData)->GetBoolField(TEXT("compile_has_error")));
	}

	const FString AppliedInfoBody = MakeExecRequestBody(
		TEXT("smoke-niagara-folder-applied-info-001"),
		TEXT("niagara_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *AppliedSystemAssetPath));
	FHttpSmokeResult AppliedInfoResult;
	TestTrue(TEXT("Niagara folder applied get_info request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AppliedInfoBody, AppliedInfoResult, 20.0));
	TestEqual(TEXT("Niagara folder applied get_info status code"), AppliedInfoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AppliedInfoJson;
	TestTrue(TEXT("Niagara folder applied get_info parses JSON"), ParseJson(AppliedInfoResult.Body, AppliedInfoJson));
	const TSharedPtr<FJsonObject>* AppliedInfoData = nullptr;
	TestTrue(TEXT("Niagara folder applied get_info contains data object"), AppliedInfoJson.IsValid() && AppliedInfoJson->TryGetObjectField(TEXT("data"), AppliedInfoData) && AppliedInfoData && AppliedInfoData->IsValid());
	if (AppliedInfoData && AppliedInfoData->IsValid())
	{
		TestTrue(TEXT("Niagara folder applied system has emitter"), static_cast<int32>((*AppliedInfoData)->GetIntegerField(TEXT("emitter_handle_count"))) >= 1);
		TestTrue(TEXT("Niagara folder applied system has user parameter"), static_cast<int32>((*AppliedInfoData)->GetIntegerField(TEXT("user_parameter_count"))) >= 1);
	}

	const FString DeleteRecreateCreateBody = MakeExecRequestBody(
		TEXT("smoke-niagara-delete-recreate-create-001"),
		TEXT("niagara_create_emitter"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"add_default_modules_and_renderers\":true,\"save_after_create\":false}"), *DeleteRecreateEmitterAssetPath));
	FHttpSmokeResult DeleteRecreateCreateResult;
	TestTrue(TEXT("Niagara delete recreate initial create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DeleteRecreateCreateBody, DeleteRecreateCreateResult, 20.0));
	TestEqual(TEXT("Niagara delete recreate initial create status code"), DeleteRecreateCreateResult.StatusCode, 200);

	const FString DeleteRecreateDeleteBody = MakeExecRequestBody(
		TEXT("smoke-niagara-delete-recreate-delete-001"),
		TEXT("asset_delete"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"dry_run\":false,\"allow_dirty\":true,\"include_path_string_hits\":false}"), *DeleteRecreateEmitterAssetPath));
	FHttpSmokeResult DeleteRecreateDeleteResult;
	TestTrue(TEXT("Niagara delete recreate asset_delete request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DeleteRecreateDeleteBody, DeleteRecreateDeleteResult, 20.0));
	TestEqual(TEXT("Niagara delete recreate asset_delete status code"), DeleteRecreateDeleteResult.StatusCode, 200);

	TSharedPtr<FJsonObject> DeleteRecreateDeleteJson;
	TestTrue(TEXT("Niagara delete recreate asset_delete parses JSON"), ParseJson(DeleteRecreateDeleteResult.Body, DeleteRecreateDeleteJson));
	const TSharedPtr<FJsonObject>* DeleteRecreateDeleteData = nullptr;
	TestTrue(TEXT("Niagara delete recreate asset_delete contains data object"), DeleteRecreateDeleteJson.IsValid() && DeleteRecreateDeleteJson->TryGetObjectField(TEXT("data"), DeleteRecreateDeleteData) && DeleteRecreateDeleteData && DeleteRecreateDeleteData->IsValid());
	if (DeleteRecreateDeleteData && DeleteRecreateDeleteData->IsValid())
	{
		TestEqual(TEXT("Niagara delete recreate asset_delete deleted_count"), (*DeleteRecreateDeleteData)->GetIntegerField(TEXT("deleted_count")), 1);
		TestEqual(TEXT("Niagara delete recreate asset_delete remaining_package_count"), (*DeleteRecreateDeleteData)->GetIntegerField(TEXT("remaining_package_count")), 0);
	}

	const FString DeleteRecreateCreateAgainBody = MakeExecRequestBody(
		TEXT("smoke-niagara-delete-recreate-create-002"),
		TEXT("niagara_create_emitter"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"add_default_modules_and_renderers\":true,\"save_after_create\":false}"), *DeleteRecreateEmitterAssetPath));
	FHttpSmokeResult DeleteRecreateCreateAgainResult;
	TestTrue(TEXT("Niagara delete recreate second create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DeleteRecreateCreateAgainBody, DeleteRecreateCreateAgainResult, 20.0));
	TestEqual(TEXT("Niagara delete recreate second create status code"), DeleteRecreateCreateAgainResult.StatusCode, 200);

	const FString DeleteRecreateCleanupBody = MakeExecRequestBody(
		TEXT("smoke-niagara-delete-recreate-cleanup-001"),
		TEXT("niagara_delete_asset"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"force_delete\":true}"), *DeleteRecreateEmitterAssetPath));
	FHttpSmokeResult DeleteRecreateCleanupResult;
	TestTrue(TEXT("Niagara delete recreate cleanup request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DeleteRecreateCleanupBody, DeleteRecreateCleanupResult, 20.0));
	TestEqual(TEXT("Niagara delete recreate cleanup status code"), DeleteRecreateCleanupResult.StatusCode, 200);

	return !HasAnyErrors();
}

#endif
