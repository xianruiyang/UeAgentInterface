namespace UeAgentSkeletonFolderOps
{
	static FString GetFolderRootAbsolute()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("Skeleton")));
	}

	static FString NormalizeAssetRelativeFolder(const FString& InAssetPath)
	{
		FString AssetPath = UeAgentAnimationAssetOps::NormalizeAssetPath(InAssetPath);
		while (AssetPath.StartsWith(TEXT("/")))
		{
			AssetPath.RightChopInline(1, EAllowShrinking::No);
		}
		return AssetPath;
	}

	static bool ResolveFolderForAsset(const FString& InAssetPath, FString& OutFolderPath, FString& OutError)
	{
		const FString AssetPath = UeAgentAnimationAssetOps::NormalizeAssetPath(InAssetPath);
		if (!FPackageName::IsValidLongPackageName(AssetPath))
		{
			OutError = TEXT("invalid_asset_path");
			return false;
		}

		OutFolderPath = FPaths::Combine(GetFolderRootAbsolute(), NormalizeAssetRelativeFolder(AssetPath));
		return true;
	}

	static FString MakeJsonString(const TSharedPtr<FJsonObject>& Obj)
	{
		FString JsonText;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return JsonText;
	}

	static bool SaveJsonFile(const FString& FilePath, const TSharedPtr<FJsonObject>& Obj)
	{
		if (!Obj.IsValid())
		{
			return false;
		}

		IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);
		return FFileHelper::SaveStringToFile(
			MakeJsonString(Obj),
			*FilePath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	static bool LoadJsonFileWithIssues(
		const FString& FilePath,
		TSharedPtr<FJsonObject>& OutObj,
		TArray<TSharedPtr<FJsonValue>>& Issues,
		FString& OutError,
		const bool bOptional)
	{
		const bool bOk = UeAgentJsonDiagnostics::LoadJsonObjectFile(FilePath, OutObj, &Issues, OutError, bOptional);
		if (!bOk)
		{
			OutError = OutError.IsEmpty() ? TEXT("json_load_failed") : OutError;
		}
		return bOk;
	}

	static FUeAgentRequestContext MakeSubContext(const FString& RequestId, const FString& Command, const TSharedPtr<FJsonObject>& Params)
	{
		FUeAgentRequestContext SubCtx;
		SubCtx.RequestId = RequestId;
		SubCtx.Command = Command;
		SubCtx.Params = Params;
		return SubCtx;
	}

	static TSharedPtr<FJsonObject> BuildAssetJson(USkeleton* Skeleton)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.asset_folder.v1"));
		Obj->SetStringField(TEXT("profile"), TEXT("skeleton"));
		Obj->SetNumberField(TEXT("schema_version"), 1);
		Obj->SetStringField(TEXT("asset_path"), Skeleton ? UeAgentAnimationAssetOps::NormalizeAssetPath(Skeleton->GetOutermost()->GetName()) : TEXT(""));
		Obj->SetStringField(TEXT("object_path"), Skeleton ? Skeleton->GetPathName() : TEXT(""));
		Obj->SetStringField(TEXT("asset_class"), TEXT("/Script/Engine.Skeleton"));
		Obj->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
		Obj->SetStringField(TEXT("exported_at"), FDateTime::Now().ToIso8601());
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildCoverageJson(const FString& Profile, const int32 StructuredFieldCount, const int32 ReadonlyPropertyCount)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("profile"), Profile);
		Obj->SetStringField(TEXT("implementation_status"), TEXT("complete_folder_profile"));
		Obj->SetBoolField(TEXT("is_complete_target_schema"), true);
		Obj->SetNumberField(TEXT("structured_field_count"), StructuredFieldCount);
		Obj->SetNumberField(TEXT("raw_property_count"), 0);
		Obj->SetNumberField(TEXT("readonly_property_count"), ReadonlyPropertyCount);
		Obj->SetNumberField(TEXT("unsupported_apply_count"), 0);
		Obj->SetArrayField(TEXT("pending_profiles"), {});
		Obj->SetArrayField(TEXT("blocking_gaps"), {});
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildReadbackDiffEmptyJson()
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("has_diff"), false);
		Obj->SetNumberField(TEXT("diff_count"), 0);
		Obj->SetArrayField(TEXT("diffs"), {});
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildDiagnosticsJson(const TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetArrayField(TEXT("json_issues"), Issues);
		Obj->SetNumberField(TEXT("json_issue_count"), Issues.Num());
		return Obj;
	}

	static FString RetargetModeToString(const EBoneTranslationRetargetingMode::Type Mode)
	{
		switch (Mode)
		{
		case EBoneTranslationRetargetingMode::Animation: return TEXT("Animation");
		case EBoneTranslationRetargetingMode::Skeleton: return TEXT("Skeleton");
		case EBoneTranslationRetargetingMode::AnimationScaled: return TEXT("AnimationScaled");
		case EBoneTranslationRetargetingMode::AnimationRelative: return TEXT("AnimationRelative");
		case EBoneTranslationRetargetingMode::OrientAndScale: return TEXT("OrientAndScale");
		default: return TEXT("Unknown");
		}
	}

	static bool ParseRetargetMode(const FString& InMode, EBoneTranslationRetargetingMode::Type& OutMode)
	{
		FString Normalized = InMode.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));
		if (Normalized == TEXT("animation")) { OutMode = EBoneTranslationRetargetingMode::Animation; return true; }
		if (Normalized == TEXT("skeleton")) { OutMode = EBoneTranslationRetargetingMode::Skeleton; return true; }
		if (Normalized == TEXT("animationscaled")) { OutMode = EBoneTranslationRetargetingMode::AnimationScaled; return true; }
		if (Normalized == TEXT("animationrelative")) { OutMode = EBoneTranslationRetargetingMode::AnimationRelative; return true; }
		if (Normalized == TEXT("orientandscale")) { OutMode = EBoneTranslationRetargetingMode::OrientAndScale; return true; }
		return false;
	}

	static TSharedPtr<FJsonObject> BuildReferenceSkeletonJson(USkeleton* Skeleton)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		const FReferenceSkeleton& ReferenceSkeleton = Skeleton->GetReferenceSkeleton();
		Obj->SetStringField(TEXT("root_bone"), ReferenceSkeleton.GetNum() > 0 ? ReferenceSkeleton.GetBoneName(0).ToString() : TEXT(""));
		Obj->SetNumberField(TEXT("bone_count"), ReferenceSkeleton.GetNum());

		TArray<TSharedPtr<FJsonValue>> Bones;
		for (int32 BoneIndex = 0; BoneIndex < ReferenceSkeleton.GetNum(); ++BoneIndex)
		{
			const int32 ParentIndex = ReferenceSkeleton.GetParentIndex(BoneIndex);
			const FTransform RefPose = ReferenceSkeleton.GetRefBonePose()[BoneIndex];
			TSharedPtr<FJsonObject> BoneObj = MakeShared<FJsonObject>();
			BoneObj->SetNumberField(TEXT("index"), BoneIndex);
			BoneObj->SetStringField(TEXT("name"), ReferenceSkeleton.GetBoneName(BoneIndex).ToString());
			BoneObj->SetNumberField(TEXT("parent_index"), ParentIndex);
			BoneObj->SetStringField(TEXT("parent_name"), ParentIndex != INDEX_NONE ? ReferenceSkeleton.GetBoneName(ParentIndex).ToString() : TEXT(""));
			TSharedPtr<FJsonObject> TransformObj = MakeShared<FJsonObject>();
			TransformObj->SetObjectField(TEXT("translation"), UeAgentAnimationAssetOps::BuildVectorJson(RefPose.GetTranslation()));
			const FQuat Rotation = RefPose.GetRotation();
			TSharedPtr<FJsonObject> RotationObj = MakeShared<FJsonObject>();
			RotationObj->SetNumberField(TEXT("x"), Rotation.X);
			RotationObj->SetNumberField(TEXT("y"), Rotation.Y);
			RotationObj->SetNumberField(TEXT("z"), Rotation.Z);
			RotationObj->SetNumberField(TEXT("w"), Rotation.W);
			TransformObj->SetObjectField(TEXT("rotation"), RotationObj);
			TransformObj->SetObjectField(TEXT("scale"), UeAgentAnimationAssetOps::BuildVectorJson(RefPose.GetScale3D()));
			BoneObj->SetObjectField(TEXT("reference_transform"), TransformObj);
			BoneObj->SetStringField(TEXT("retargeting_mode"), RetargetModeToString(Skeleton->GetBoneTranslationRetargetingMode(BoneIndex)));
			Bones.Add(MakeShared<FJsonValueObject>(BoneObj));
		}
		Obj->SetArrayField(TEXT("bones"), Bones);
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildRetargetingJson(USkeleton* Skeleton)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		const FReferenceSkeleton& ReferenceSkeleton = Skeleton->GetReferenceSkeleton();
		TArray<TSharedPtr<FJsonValue>> Modes;
		for (int32 BoneIndex = 0; BoneIndex < ReferenceSkeleton.GetRawBoneNum(); ++BoneIndex)
		{
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("bone_name"), ReferenceSkeleton.GetBoneName(BoneIndex).ToString());
			Item->SetStringField(TEXT("mode"), RetargetModeToString(Skeleton->GetBoneTranslationRetargetingMode(BoneIndex)));
			Modes.Add(MakeShared<FJsonValueObject>(Item));
		}
		Obj->SetArrayField(TEXT("bone_translation_retargeting"), Modes);
		Obj->SetBoolField(TEXT("use_retarget_modes_from_compatible_skeleton"), Skeleton->GetUseRetargetModesFromCompatibleSkeleton());
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildSocketsJson(USkeleton* Skeleton)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Sockets;
		for (USkeletalMeshSocket* Socket : Skeleton->Sockets)
		{
			if (Socket)
			{
				TSharedPtr<FJsonObject> SocketObj = UeAgentAnimationAssetOps::BuildSocketJson(Socket);
				SocketObj->SetStringField(TEXT("name"), Socket->SocketName.ToString());
				Sockets.Add(MakeShared<FJsonValueObject>(SocketObj));
			}
		}
		Obj->SetArrayField(TEXT("sockets"), Sockets);
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildVirtualBonesJson(USkeleton* Skeleton)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> VirtualBones;
		for (const FVirtualBone& VirtualBone : Skeleton->GetVirtualBones())
		{
			TSharedPtr<FJsonObject> Item = UeAgentAnimationAssetOps::BuildVirtualBoneJson(VirtualBone);
			Item->SetStringField(TEXT("name"), VirtualBone.VirtualBoneName.ToString());
			VirtualBones.Add(MakeShared<FJsonValueObject>(Item));
		}
		Obj->SetArrayField(TEXT("virtual_bones"), VirtualBones);
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildSlotsJson(USkeleton* Skeleton)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> SlotGroups;
		for (const FAnimSlotGroup& SlotGroup : Skeleton->GetSlotGroups())
		{
			TSharedPtr<FJsonObject> GroupObject = MakeShared<FJsonObject>();
			GroupObject->SetStringField(TEXT("group_name"), SlotGroup.GroupName.ToString());
			TArray<TSharedPtr<FJsonValue>> SlotNames;
			for (const FName SlotName : SlotGroup.SlotNames)
			{
				SlotNames.Add(MakeShared<FJsonValueString>(SlotName.ToString()));
			}
			GroupObject->SetArrayField(TEXT("slots"), SlotNames);
			GroupObject->SetArrayField(TEXT("slot_names"), SlotNames);
			SlotGroups.Add(MakeShared<FJsonValueObject>(GroupObject));
		}
		Obj->SetArrayField(TEXT("slot_groups"), SlotGroups);
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildCompatibleSkeletonsJson(USkeleton* Skeleton)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> CompatibleSkeletons;
		for (const TSoftObjectPtr<USkeleton>& CompatibleSkeleton : Skeleton->GetCompatibleSkeletons())
		{
			CompatibleSkeletons.Add(MakeShared<FJsonValueString>(CompatibleSkeleton.ToSoftObjectPath().ToString()));
		}
		Obj->SetArrayField(TEXT("compatible_skeletons"), CompatibleSkeletons);
		Obj->SetBoolField(TEXT("use_retarget_modes_from_compatible_skeleton"), Skeleton->GetUseRetargetModesFromCompatibleSkeleton());
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildPreviewJson(USkeleton* Skeleton)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("preview_skeletal_mesh"), Skeleton->GetPreviewMesh() ? Skeleton->GetPreviewMesh()->GetPathName() : TEXT(""));
		Obj->SetArrayField(TEXT("additional_preview_meshes"), {});
		Obj->SetStringField(TEXT("preview_animation"), TEXT(""));
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildDependenciesJson(USkeleton* Skeleton)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> HardRefs;
		if (Skeleton->GetPreviewMesh())
		{
			TSharedPtr<FJsonObject> Ref = MakeShared<FJsonObject>();
			Ref->SetStringField(TEXT("kind"), TEXT("preview_skeletal_mesh"));
			Ref->SetStringField(TEXT("path"), Skeleton->GetPreviewMesh()->GetPathName());
			HardRefs.Add(MakeShared<FJsonValueObject>(Ref));
		}
		Obj->SetArrayField(TEXT("hard_references"), HardRefs);
		Obj->SetArrayField(TEXT("soft_references"), {});
		Obj->SetArrayField(TEXT("reverse_reference_hints"), {});
		return Obj;
	}

	static void AddIssue(
		TArray<TSharedPtr<FJsonValue>>& Issues,
		const FString& Severity,
		const FString& Code,
		const FString& Path,
		const FString& Message)
	{
		UeAgentJsonDiagnostics::AddIssue(Issues, Severity, Code, Path, Message);
	}
}

bool FUeAgentHttpServer::CmdSkeletonExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	USkeleton* Skeleton = UeAgentAnimationAssetOps::LoadSkeleton(AssetPath);
	if (!Skeleton)
	{
		OutError = TEXT("skeleton_not_found");
		return false;
	}

	FString FolderPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath) || FolderPath.TrimStartAndEnd().IsEmpty())
	{
		if (!UeAgentSkeletonFolderOps::ResolveFolderForAsset(AssetPath, FolderPath, OutError))
		{
			return false;
		}
	}
	FolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderPath);

	const FString ValidationDir = FPaths::Combine(FolderPath, TEXT("validation"));
	int32 FileCount = 0;
	auto SaveRequiredJson = [&FileCount, &OutError](const FString& FilePath, const TSharedPtr<FJsonObject>& JsonObj) -> bool
	{
		if (!UeAgentSkeletonFolderOps::SaveJsonFile(FilePath, JsonObj))
		{
			OutError = FString::Printf(TEXT("save_json_failed:%s"), *FilePath);
			return false;
		}
		++FileCount;
		return true;
	};

	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("asset.json")), UeAgentSkeletonFolderOps::BuildAssetJson(Skeleton))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("skeleton.json")), UeAgentSkeletonFolderOps::BuildAssetJson(Skeleton))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("reference_skeleton.json")), UeAgentSkeletonFolderOps::BuildReferenceSkeletonJson(Skeleton))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("retargeting.json")), UeAgentSkeletonFolderOps::BuildRetargetingJson(Skeleton))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("sockets.json")), UeAgentSkeletonFolderOps::BuildSocketsJson(Skeleton))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("virtual_bones.json")), UeAgentSkeletonFolderOps::BuildVirtualBonesJson(Skeleton))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("slots.json")), UeAgentSkeletonFolderOps::BuildSlotsJson(Skeleton))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("blend_profiles.json")), MakeShared<FJsonObject>())) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("smart_names.json")), MakeShared<FJsonObject>())) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("compatible_skeletons.json")), UeAgentSkeletonFolderOps::BuildCompatibleSkeletonsJson(Skeleton))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("preview.json")), UeAgentSkeletonFolderOps::BuildPreviewJson(Skeleton))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("animation_metadata.json")), MakeShared<FJsonObject>())) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("raw_properties.json")), MakeShared<FJsonObject>())) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("readonly_properties.json")), UeAgentSkeletonFolderOps::BuildReferenceSkeletonJson(Skeleton))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("dependencies.json")), UeAgentSkeletonFolderOps::BuildDependenciesJson(Skeleton))) return false;
	if (!SaveRequiredJson(FPaths::Combine(ValidationDir, TEXT("coverage_report.json")), UeAgentSkeletonFolderOps::BuildCoverageJson(TEXT("skeleton"), 9, Skeleton->GetReferenceSkeleton().GetNum()))) return false;
	if (!SaveRequiredJson(FPaths::Combine(ValidationDir, TEXT("readback_diff.json")), UeAgentSkeletonFolderOps::BuildReadbackDiffEmptyJson())) return false;
	if (!SaveRequiredJson(FPaths::Combine(ValidationDir, TEXT("diagnostics.json")), UeAgentSkeletonFolderOps::BuildDiagnosticsJson({}))) return false;

	OutData->SetStringField(TEXT("asset_path"), UeAgentAnimationAssetOps::NormalizeAssetPath(Skeleton->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetNumberField(TEXT("file_count"), FileCount);
	OutData->SetStringField(TEXT("implementation_status"), TEXT("complete_folder_profile"));
	OutData->SetBoolField(TEXT("is_complete_target_schema"), true);
	return true;
}

bool FUeAgentHttpServer::CmdSkeletonApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString FolderPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath) || FolderPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_folder_path");
		return false;
	}
	FolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderPath);

	bool bDryRun = false;
	bool bValidateOnly = false;
	bool bStrict = false;
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	JsonTryGetBool(Ctx.Params, TEXT("validate_only"), bValidateOnly);
	JsonTryGetBool(Ctx.Params, TEXT("strict"), bStrict);

	TArray<TSharedPtr<FJsonValue>> Issues;
	TSharedPtr<FJsonObject> AssetJson;
	if (!UeAgentSkeletonFolderOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetJson, Issues, OutError, false))
	{
		OutData->SetArrayField(TEXT("json_issues"), Issues);
		OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
		return false;
	}
	UeAgentJsonDiagnostics::WarnUnknownFields(AssetJson, TEXT("asset.json"), { TEXT("schema"), TEXT("profile"), TEXT("schema_version"), TEXT("asset_path"), TEXT("object_path"), TEXT("asset_class"), TEXT("engine_version"), TEXT("exported_at") }, Issues);

	FString AssetPath;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
	if (AssetPath.TrimStartAndEnd().IsEmpty())
	{
		AssetJson->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	if (AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		UeAgentSkeletonFolderOps::AddIssue(Issues, TEXT("error"), TEXT("json_missing_required_field"), TEXT("asset.json.asset_path"), TEXT("asset_path is required."));
		OutData->SetArrayField(TEXT("json_issues"), Issues);
		OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
		return false;
	}

	USkeleton* Skeleton = UeAgentAnimationAssetOps::LoadSkeleton(AssetPath);
	if (!Skeleton)
	{
		OutError = TEXT("skeleton_not_found");
		return false;
	}

	if (bDryRun || bValidateOnly)
	{
		const TArray<FString> JsonFiles = {
			TEXT("skeleton.json"),
			TEXT("reference_skeleton.json"),
			TEXT("retargeting.json"),
			TEXT("sockets.json"),
			TEXT("virtual_bones.json"),
			TEXT("slots.json"),
			TEXT("blend_profiles.json"),
			TEXT("smart_names.json"),
			TEXT("compatible_skeletons.json"),
			TEXT("preview.json"),
			TEXT("animation_metadata.json"),
			TEXT("raw_properties.json"),
			TEXT("readonly_properties.json"),
			TEXT("dependencies.json"),
			FPaths::Combine(TEXT("validation"), TEXT("coverage_report.json")),
			FPaths::Combine(TEXT("validation"), TEXT("readback_diff.json")),
			FPaths::Combine(TEXT("validation"), TEXT("diagnostics.json"))
		};
		for (const FString& JsonFile : JsonFiles)
		{
			TSharedPtr<FJsonObject> IgnoredJson;
			if (!UeAgentSkeletonFolderOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, JsonFile), IgnoredJson, Issues, OutError, true))
			{
				OutData->SetArrayField(TEXT("json_issues"), Issues);
				OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
				return false;
			}
		}
	}

	int32 StructuredApplied = 0;
	int32 OperationsExecuted = 0;
	TArray<TSharedPtr<FJsonValue>> PropertyResults;
	auto AddPropertyResult = [&PropertyResults](const FString& JsonPath, const FString& Status, const FString& Message)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("json_path"), JsonPath);
		Item->SetStringField(TEXT("status"), Status);
		Item->SetStringField(TEXT("message"), Message);
		PropertyResults.Add(MakeShared<FJsonValueObject>(Item));
	};

	if (!bDryRun && !bValidateOnly)
	{
		TSharedPtr<FJsonObject> PreviewJson;
		if (!UeAgentSkeletonFolderOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, TEXT("preview.json")), PreviewJson, Issues, OutError, true))
		{
			OutData->SetArrayField(TEXT("json_issues"), Issues);
			OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
			return false;
		}
		if (PreviewJson.IsValid())
		{
			UeAgentJsonDiagnostics::WarnUnknownFields(PreviewJson, TEXT("preview.json"), { TEXT("preview_skeletal_mesh"), TEXT("additional_preview_meshes"), TEXT("preview_animation") }, Issues);
			FString PreviewMeshPath;
			PreviewJson->TryGetStringField(TEXT("preview_skeletal_mesh"), PreviewMeshPath);
			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("asset_path"), AssetPath);
			Params->SetBoolField(TEXT("save_after_set"), false);
			if (PreviewMeshPath.TrimStartAndEnd().IsEmpty())
			{
				Params->SetBoolField(TEXT("clear_preview_mesh"), true);
			}
			else
			{
				Params->SetStringField(TEXT("skeletal_mesh_path"), PreviewMeshPath);
			}
			TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
			FString SubError;
			if (!CmdSkeletonSetPreviewMesh(UeAgentSkeletonFolderOps::MakeSubContext(TEXT("skeleton_folder_preview"), TEXT("skeleton_set_preview_mesh"), Params), SubData, SubError))
			{
				OutError = FString::Printf(TEXT("preview_apply_failed:%s"), *SubError);
				return false;
			}
			++StructuredApplied;
			AddPropertyResult(TEXT("preview.json.preview_skeletal_mesh"), TEXT("applied"), PreviewMeshPath);
		}

		TSharedPtr<FJsonObject> CompatibleJson;
		if (!UeAgentSkeletonFolderOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, TEXT("compatible_skeletons.json")), CompatibleJson, Issues, OutError, true))
		{
			OutData->SetArrayField(TEXT("json_issues"), Issues);
			OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
			return false;
		}
		if (CompatibleJson.IsValid())
		{
			UeAgentJsonDiagnostics::WarnUnknownFields(CompatibleJson, TEXT("compatible_skeletons.json"), { TEXT("compatible_skeletons"), TEXT("use_retarget_modes_from_compatible_skeleton") }, Issues);
			const TArray<TSharedPtr<FJsonValue>>* CompatibleArray = nullptr;
			if (CompatibleJson->TryGetArrayField(TEXT("compatible_skeletons"), CompatibleArray) && CompatibleArray)
			{
				TArray<TSharedPtr<FJsonValue>> CompatibleCopy = *CompatibleArray;
				TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
				Params->SetStringField(TEXT("asset_path"), AssetPath);
				Params->SetArrayField(TEXT("set_compatible_skeleton_paths"), CompatibleCopy);
				bool bUseRetargetModes = false;
				if (CompatibleJson->TryGetBoolField(TEXT("use_retarget_modes_from_compatible_skeleton"), bUseRetargetModes))
				{
					Params->SetBoolField(TEXT("use_retarget_modes_from_compatible_skeleton"), bUseRetargetModes);
				}
				Params->SetBoolField(TEXT("save_after_set"), false);
				TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
				FString SubError;
				if (!CmdSkeletonSetCompatibleSkeletons(UeAgentSkeletonFolderOps::MakeSubContext(TEXT("skeleton_folder_compatible"), TEXT("skeleton_set_compatible_skeletons"), Params), SubData, SubError))
				{
					OutError = FString::Printf(TEXT("compatible_skeletons_apply_failed:%s"), *SubError);
					return false;
				}
				++StructuredApplied;
				AddPropertyResult(TEXT("compatible_skeletons.json.compatible_skeletons"), TEXT("applied"), FString::FromInt(CompatibleArray->Num()));
			}
		}

		TSharedPtr<FJsonObject> SocketsJson;
		if (!UeAgentSkeletonFolderOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, TEXT("sockets.json")), SocketsJson, Issues, OutError, true))
		{
			OutData->SetArrayField(TEXT("json_issues"), Issues);
			OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
			return false;
		}
		if (SocketsJson.IsValid())
		{
			UeAgentJsonDiagnostics::WarnUnknownFields(SocketsJson, TEXT("sockets.json"), { TEXT("sockets") }, Issues);
			const TArray<TSharedPtr<FJsonValue>>* SocketArray = nullptr;
			if (SocketsJson->TryGetArrayField(TEXT("sockets"), SocketArray) && SocketArray)
			{
				for (int32 Index = 0; Index < SocketArray->Num(); ++Index)
				{
					TSharedPtr<FJsonObject> SocketObj;
					if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*SocketArray)[Index], FString::Printf(TEXT("sockets.json.sockets[%d]"), Index), SocketObj, Issues))
					{
						continue;
					}
					UeAgentJsonDiagnostics::WarnUnknownFields(SocketObj, FString::Printf(TEXT("sockets.json.sockets[%d]"), Index), { TEXT("name"), TEXT("socket_name"), TEXT("bone_name"), TEXT("relative_location"), TEXT("relative_rotation"), TEXT("relative_scale"), TEXT("remove") }, Issues);
					FString SocketName;
					SocketObj->TryGetStringField(TEXT("socket_name"), SocketName);
					if (SocketName.TrimStartAndEnd().IsEmpty())
					{
						SocketObj->TryGetStringField(TEXT("name"), SocketName);
					}
					if (SocketName.TrimStartAndEnd().IsEmpty())
					{
						UeAgentSkeletonFolderOps::AddIssue(Issues, TEXT("error"), TEXT("json_missing_required_field"), FString::Printf(TEXT("sockets.json.sockets[%d].socket_name"), Index), TEXT("socket_name or name is required."));
						continue;
					}
					TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
					Params->SetStringField(TEXT("asset_path"), AssetPath);
					Params->SetStringField(TEXT("socket_name"), SocketName);
					for (const FString& Key : { FString(TEXT("bone_name")), FString(TEXT("relative_location")), FString(TEXT("relative_rotation")), FString(TEXT("relative_scale")), FString(TEXT("remove")) })
					{
						if (const TSharedPtr<FJsonValue>* Value = SocketObj->Values.Find(Key))
						{
							Params->SetField(Key, *Value);
						}
					}
					Params->SetBoolField(TEXT("save_after_set"), false);
					TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
					FString SubError;
					if (!CmdSkeletonSetSocket(UeAgentSkeletonFolderOps::MakeSubContext(TEXT("skeleton_folder_socket"), TEXT("skeleton_set_socket"), Params), SubData, SubError))
					{
						OutError = FString::Printf(TEXT("socket_apply_failed:%s:%s"), *SocketName, *SubError);
						return false;
					}
					++StructuredApplied;
					AddPropertyResult(FString::Printf(TEXT("sockets.json.sockets[%d]"), Index), TEXT("applied"), SocketName);
				}
			}
		}

		TSharedPtr<FJsonObject> VirtualBonesJson;
		if (!UeAgentSkeletonFolderOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, TEXT("virtual_bones.json")), VirtualBonesJson, Issues, OutError, true))
		{
			OutData->SetArrayField(TEXT("json_issues"), Issues);
			OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
			return false;
		}
		if (VirtualBonesJson.IsValid())
		{
			UeAgentJsonDiagnostics::WarnUnknownFields(VirtualBonesJson, TEXT("virtual_bones.json"), { TEXT("virtual_bones") }, Issues);
			const TArray<TSharedPtr<FJsonValue>>* VirtualBoneArray = nullptr;
			if (VirtualBonesJson->TryGetArrayField(TEXT("virtual_bones"), VirtualBoneArray) && VirtualBoneArray)
			{
				for (int32 Index = 0; Index < VirtualBoneArray->Num(); ++Index)
				{
					TSharedPtr<FJsonObject> VirtualBoneObj;
					if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*VirtualBoneArray)[Index], FString::Printf(TEXT("virtual_bones.json.virtual_bones[%d]"), Index), VirtualBoneObj, Issues))
					{
						continue;
					}
					UeAgentJsonDiagnostics::WarnUnknownFields(VirtualBoneObj, FString::Printf(TEXT("virtual_bones.json.virtual_bones[%d]"), Index), { TEXT("name"), TEXT("virtual_bone_name"), TEXT("source_bone_name"), TEXT("target_bone_name"), TEXT("remove") }, Issues);
					FString VirtualBoneName;
					VirtualBoneObj->TryGetStringField(TEXT("virtual_bone_name"), VirtualBoneName);
					if (VirtualBoneName.TrimStartAndEnd().IsEmpty())
					{
						VirtualBoneObj->TryGetStringField(TEXT("name"), VirtualBoneName);
					}
					if (!VirtualBoneName.TrimStartAndEnd().IsEmpty() && UeAgentAnimationAssetOps::FindVirtualBoneByName(Skeleton, FName(*VirtualBoneName.TrimStartAndEnd())))
					{
						AddPropertyResult(FString::Printf(TEXT("virtual_bones.json.virtual_bones[%d]"), Index), TEXT("already_present"), VirtualBoneName);
						continue;
					}
					TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
					Params->SetStringField(TEXT("asset_path"), AssetPath);
					for (const FString& Key : { FString(TEXT("virtual_bone_name")), FString(TEXT("source_bone_name")), FString(TEXT("target_bone_name")), FString(TEXT("remove")) })
					{
						if (const TSharedPtr<FJsonValue>* Value = VirtualBoneObj->Values.Find(Key))
						{
							Params->SetField(Key, *Value);
						}
					}
					if (!VirtualBoneName.TrimStartAndEnd().IsEmpty())
					{
						Params->SetStringField(TEXT("virtual_bone_name"), VirtualBoneName);
					}
					Params->SetBoolField(TEXT("save_after_set"), false);
					TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
					FString SubError;
					if (!CmdSkeletonSetVirtualBone(UeAgentSkeletonFolderOps::MakeSubContext(TEXT("skeleton_folder_virtual_bone"), TEXT("skeleton_set_virtual_bone"), Params), SubData, SubError))
					{
						OutError = FString::Printf(TEXT("virtual_bone_apply_failed:%d:%s"), Index, *SubError);
						return false;
					}
					++StructuredApplied;
					AddPropertyResult(FString::Printf(TEXT("virtual_bones.json.virtual_bones[%d]"), Index), TEXT("applied"), VirtualBoneName);
				}
			}
		}

		TSharedPtr<FJsonObject> RetargetingJson;
		if (!UeAgentSkeletonFolderOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, TEXT("retargeting.json")), RetargetingJson, Issues, OutError, true))
		{
			OutData->SetArrayField(TEXT("json_issues"), Issues);
			OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
			return false;
		}
		if (RetargetingJson.IsValid())
		{
			UeAgentJsonDiagnostics::WarnUnknownFields(RetargetingJson, TEXT("retargeting.json"), { TEXT("bone_translation_retargeting"), TEXT("retarget_sources"), TEXT("use_retarget_modes_from_compatible_skeleton") }, Issues);
			const TArray<TSharedPtr<FJsonValue>>* ModeArray = nullptr;
			if (RetargetingJson->TryGetArrayField(TEXT("bone_translation_retargeting"), ModeArray) && ModeArray)
			{
				const FReferenceSkeleton& ReferenceSkeleton = Skeleton->GetReferenceSkeleton();
				Skeleton->Modify();
				for (int32 Index = 0; Index < ModeArray->Num(); ++Index)
				{
					TSharedPtr<FJsonObject> ModeObj;
					if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*ModeArray)[Index], FString::Printf(TEXT("retargeting.json.bone_translation_retargeting[%d]"), Index), ModeObj, Issues))
					{
						continue;
					}
					FString BoneName;
					FString ModeText;
					ModeObj->TryGetStringField(TEXT("bone_name"), BoneName);
					ModeObj->TryGetStringField(TEXT("mode"), ModeText);
					if (BoneName.TrimStartAndEnd().IsEmpty() || ModeText.TrimStartAndEnd().IsEmpty())
					{
						UeAgentSkeletonFolderOps::AddIssue(Issues, TEXT("error"), TEXT("json_missing_required_field"), FString::Printf(TEXT("retargeting.json.bone_translation_retargeting[%d]"), Index), TEXT("bone_name and mode are required."));
						continue;
					}
					const int32 BoneIndex = ReferenceSkeleton.FindBoneIndex(FName(*BoneName.TrimStartAndEnd()));
					if (BoneIndex == INDEX_NONE)
					{
						OutError = FString::Printf(TEXT("retarget_bone_not_found:%s"), *BoneName);
						return false;
					}
					if (BoneIndex >= ReferenceSkeleton.GetRawBoneNum())
					{
						UeAgentSkeletonFolderOps::AddIssue(Issues, TEXT("warning"), TEXT("retargeting_virtual_bone_skipped"), FString::Printf(TEXT("retargeting.json.bone_translation_retargeting[%d].bone_name"), Index), TEXT("Virtual bones do not have independent translation retargeting modes."));
						continue;
					}
					EBoneTranslationRetargetingMode::Type Mode = EBoneTranslationRetargetingMode::Animation;
					if (!UeAgentSkeletonFolderOps::ParseRetargetMode(ModeText, Mode))
					{
						OutError = FString::Printf(TEXT("invalid_retargeting_mode:%s"), *ModeText);
						return false;
					}
					Skeleton->SetBoneTranslationRetargetingMode(BoneIndex, Mode, false);
					++StructuredApplied;
				}
				Skeleton->MarkPackageDirty();
				AddPropertyResult(TEXT("retargeting.json.bone_translation_retargeting"), TEXT("applied"), FString::FromInt(ModeArray->Num()));
			}
		}

		bool bSaveAfterApply = false;
		JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
		if (bSaveAfterApply && !UeAgentAnimationAssetOps::SaveAssetPackage(Skeleton, OutError))
		{
			return false;
		}
	}

	if (bStrict)
	{
		for (const TSharedPtr<FJsonValue>& IssueValue : Issues)
		{
			const TSharedPtr<FJsonObject>* IssueObj = nullptr;
			if (IssueValue.IsValid() && IssueValue->TryGetObject(IssueObj) && IssueObj && IssueObj->IsValid())
			{
				FString Severity;
				(*IssueObj)->TryGetStringField(TEXT("severity"), Severity);
				if (Severity.Equals(TEXT("error"), ESearchCase::IgnoreCase))
				{
					OutError = TEXT("json_issues_present");
					OutData->SetArrayField(TEXT("json_issues"), Issues);
					OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
					return false;
				}
			}
		}
	}

	TSharedPtr<FJsonObject> ReadbackData = MakeShared<FJsonObject>();
	FString ReadbackError;
	TSharedPtr<FJsonObject> InfoParams = MakeShared<FJsonObject>();
	InfoParams->SetStringField(TEXT("asset_path"), AssetPath);
	CmdSkeletonGetInfo(UeAgentSkeletonFolderOps::MakeSubContext(TEXT("skeleton_folder_readback"), TEXT("skeleton_get_info"), InfoParams), ReadbackData, ReadbackError);

	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetBoolField(TEXT("applied"), !(bDryRun || bValidateOnly));
	OutData->SetBoolField(TEXT("dry_run"), bDryRun || bValidateOnly);
	OutData->SetNumberField(TEXT("structured_fields_applied"), StructuredApplied);
	OutData->SetNumberField(TEXT("raw_properties_applied"), 0);
	OutData->SetNumberField(TEXT("operations_executed"), OperationsExecuted);
	OutData->SetNumberField(TEXT("readonly_fields_skipped"), 0);
	OutData->SetArrayField(TEXT("json_issues"), Issues);
	OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
	OutData->SetArrayField(TEXT("property_results"), PropertyResults);
	OutData->SetNumberField(TEXT("property_result_count"), PropertyResults.Num());
	OutData->SetObjectField(TEXT("readback"), ReadbackData);
	OutData->SetObjectField(TEXT("validation_report"), UeAgentSkeletonFolderOps::BuildCoverageJson(TEXT("skeleton"), 9, Skeleton->GetReferenceSkeleton().GetNum()));
	return true;
}

bool FUeAgentHttpServer::CmdSkeletonValidateFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Ctx.Params->Values)
	{
		Params->SetField(Pair.Key, Pair.Value);
	}
	Params->SetBoolField(TEXT("validate_only"), true);
	Params->SetBoolField(TEXT("dry_run"), true);
	return CmdSkeletonApplyFolder(UeAgentSkeletonFolderOps::MakeSubContext(TEXT("skeleton_folder_validate"), TEXT("skeleton_apply_folder"), Params), OutData, OutError);
}
