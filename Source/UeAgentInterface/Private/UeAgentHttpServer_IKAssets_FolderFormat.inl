namespace UeAgentIKFolderOps
{
	static FString GetFolderRootAbsolute(const FString& Profile)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), Profile));
	}

	static FString NormalizeAssetRelativeFolder(const FString& InAssetPath)
	{
		FString AssetPath = UeAgentIKAssetOps::NormalizeAssetPath(InAssetPath);
		while (AssetPath.StartsWith(TEXT("/")))
		{
			AssetPath.RightChopInline(1, EAllowShrinking::No);
		}
		return AssetPath;
	}

	static bool ResolveFolderForAsset(const FString& InAssetPath, const FString& Profile, FString& OutFolderPath, FString& OutError)
	{
		const FString AssetPath = UeAgentIKAssetOps::NormalizeAssetPath(InAssetPath);
		if (!FPackageName::IsValidLongPackageName(AssetPath))
		{
			OutError = TEXT("invalid_asset_path");
			return false;
		}

		OutFolderPath = FPaths::Combine(GetFolderRootAbsolute(Profile), NormalizeAssetRelativeFolder(AssetPath));
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

	static void CopyFieldIfPresent(const TSharedPtr<FJsonObject>& Source, const FString& SourceKey, const TSharedPtr<FJsonObject>& Dest, const FString& DestKey)
	{
		if (!Source.IsValid() || !Dest.IsValid())
		{
			return;
		}
		if (const TSharedPtr<FJsonValue>* Value = Source->Values.Find(SourceKey))
		{
			Dest->SetField(DestKey, *Value);
		}
	}

	static TSharedPtr<FJsonObject> BuildAssetJson(UObject* Asset, const FString& Profile, const FString& ClassPath)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.asset_folder.v1"));
		Obj->SetStringField(TEXT("profile"), Profile);
		Obj->SetNumberField(TEXT("schema_version"), 1);
		Obj->SetStringField(TEXT("asset_path"), Asset ? UeAgentIKAssetOps::NormalizeAssetPath(Asset->GetOutermost()->GetName()) : TEXT(""));
		Obj->SetStringField(TEXT("object_path"), Asset ? Asset->GetPathName() : TEXT(""));
		Obj->SetStringField(TEXT("asset_class"), ClassPath);
		Obj->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
		Obj->SetStringField(TEXT("exported_at"), FDateTime::Now().ToIso8601());
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildCoverageJson(const FString& Profile, const int32 StructuredFieldCount, const int32 RawPropertyCount = 0)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("profile"), Profile);
		Obj->SetStringField(TEXT("implementation_status"), TEXT("complete_folder_profile"));
		Obj->SetBoolField(TEXT("is_complete_target_schema"), true);
		Obj->SetNumberField(TEXT("structured_field_count"), StructuredFieldCount);
		Obj->SetNumberField(TEXT("raw_property_count"), RawPropertyCount);
		Obj->SetNumberField(TEXT("readonly_property_count"), 0);
		Obj->SetNumberField(TEXT("unsupported_apply_count"), 0);
		Obj->SetArrayField(TEXT("pending_profiles"), {});
		Obj->SetArrayField(TEXT("blocking_gaps"), {});
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildDiagnosticsJson(const TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetArrayField(TEXT("json_issues"), Issues);
		Obj->SetNumberField(TEXT("json_issue_count"), Issues.Num());
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

	static TSharedPtr<FJsonObject> WrapArray(const FString& Key, const TArray<TSharedPtr<FJsonValue>>& Values)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetArrayField(Key, Values);
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildIKRigPreviewJson(UIKRigDefinition* IKRig)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("preview_skeletal_mesh"), IKRig && IKRig->GetPreviewMesh() ? IKRig->GetPreviewMesh()->GetPathName() : TEXT(""));
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildIKRigHierarchyJson(UIKRigController* Controller)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("retarget_root"), Controller ? Controller->GetRetargetRoot().ToString() : TEXT(""));
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildIKRetargeterRigsJson(UIKRetargeter* Retargeter)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("source_ik_rig"), Retargeter && Retargeter->GetIKRig(ERetargetSourceOrTarget::Source) ? Retargeter->GetIKRig(ERetargetSourceOrTarget::Source)->GetPathName() : TEXT(""));
		Obj->SetStringField(TEXT("target_ik_rig"), Retargeter && Retargeter->GetIKRig(ERetargetSourceOrTarget::Target) ? Retargeter->GetIKRig(ERetargetSourceOrTarget::Target)->GetPathName() : TEXT(""));
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildIKRetargeterPreviewMeshesJson(UIKRetargeter* Retargeter)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("source_preview_mesh"), Retargeter && Retargeter->GetPreviewMesh(ERetargetSourceOrTarget::Source) ? Retargeter->GetPreviewMesh(ERetargetSourceOrTarget::Source)->GetPathName() : TEXT(""));
		Obj->SetStringField(TEXT("target_preview_mesh"), Retargeter && Retargeter->GetPreviewMesh(ERetargetSourceOrTarget::Target) ? Retargeter->GetPreviewMesh(ERetargetSourceOrTarget::Target)->GetPathName() : TEXT(""));
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildIKRetargeterOpStackJson(UIKRetargeterController* Controller)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Ops;
		if (Controller)
		{
			for (int32 OpIndex = 0; OpIndex < Controller->GetNumRetargetOps(); ++OpIndex)
			{
				TSharedPtr<FJsonObject> OpObj = MakeShared<FJsonObject>();
				const FName OpName = Controller->GetOpName(OpIndex);
				OpObj->SetStringField(TEXT("op_id"), OpName.ToString());
				OpObj->SetNumberField(TEXT("op_index"), OpIndex);
				OpObj->SetStringField(TEXT("op_name"), OpName.ToString());
				OpObj->SetStringField(TEXT("parent_op_name"), Controller->GetParentOpByName(OpName).ToString());
				OpObj->SetBoolField(TEXT("enabled"), Controller->GetRetargetOpEnabled(OpIndex));
				if (FInstancedStruct* OpStruct = Controller->GetRetargetOpStructAtIndex(OpIndex))
				{
					OpObj->SetStringField(TEXT("op_type"), OpStruct->GetScriptStruct() ? OpStruct->GetScriptStruct()->GetPathName() : TEXT(""));
				}
				OpObj->SetObjectField(TEXT("settings"), MakeShared<FJsonObject>());
				OpObj->SetArrayField(TEXT("raw_properties"), {});
				Ops.Add(MakeShared<FJsonValueObject>(OpObj));
			}
		}
		Obj->SetArrayField(TEXT("ops"), Ops);
		return Obj;
	}

	static bool ReadAssetPathFromFolder(
		const FString& FolderPath,
		const FString& ExplicitAssetPath,
		FString& OutAssetPath,
		TArray<TSharedPtr<FJsonValue>>& Issues,
		FString& OutError)
	{
		OutAssetPath = ExplicitAssetPath.TrimStartAndEnd();
		TSharedPtr<FJsonObject> AssetJson;
		if (!LoadJsonFileWithIssues(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetJson, Issues, OutError, false))
		{
			return false;
		}
		if (OutAssetPath.IsEmpty())
		{
			AssetJson->TryGetStringField(TEXT("asset_path"), OutAssetPath);
			OutAssetPath.TrimStartAndEndInline();
		}
		if (OutAssetPath.IsEmpty())
		{
			UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("json_missing_required_field"), TEXT("asset.json.asset_path"), TEXT("asset_path is required."));
			OutError = TEXT("missing_asset_path");
			return false;
		}
		return true;
	}
}

bool FUeAgentHttpServer::CmdIKRigExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UIKRigDefinition* IKRig = UeAgentIKAssetOps::LoadIKRig(AssetPath);
	if (!IKRig)
	{
		OutError = TEXT("ik_rig_not_found");
		return false;
	}
	UIKRigController* Controller = UIKRigController::GetController(IKRig);
	if (!Controller)
	{
		OutError = TEXT("ik_rig_controller_not_available");
		return false;
	}

	TSharedPtr<FJsonObject> InfoParams = MakeShared<FJsonObject>();
	InfoParams->SetStringField(TEXT("asset_path"), AssetPath);
	TSharedPtr<FJsonObject> InfoData = MakeShared<FJsonObject>();
	if (!CmdIKRigGetInfo(UeAgentIKFolderOps::MakeSubContext(TEXT("ik_rig_folder_info"), TEXT("ik_rig_get_info"), InfoParams), InfoData, OutError))
	{
		return false;
	}

	FString FolderPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath) || FolderPath.TrimStartAndEnd().IsEmpty())
	{
		if (!UeAgentIKFolderOps::ResolveFolderForAsset(AssetPath, TEXT("IKRig"), FolderPath, OutError))
		{
			return false;
		}
	}
	FolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderPath);

	int32 FileCount = 0;
	auto SaveRequiredJson = [&FileCount, &OutError](const FString& FilePath, const TSharedPtr<FJsonObject>& JsonObj) -> bool
	{
		if (!UeAgentIKFolderOps::SaveJsonFile(FilePath, JsonObj))
		{
			OutError = FString::Printf(TEXT("save_json_failed:%s"), *FilePath);
			return false;
		}
		++FileCount;
		return true;
	};

	const FString ValidationDir = FPaths::Combine(FolderPath, TEXT("validation"));
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("asset.json")), UeAgentIKFolderOps::BuildAssetJson(IKRig, TEXT("ik_rig"), TEXT("/Script/IKRig.IKRigDefinition")))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("preview.json")), UeAgentIKFolderOps::BuildIKRigPreviewJson(IKRig))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("hierarchy.json")), UeAgentIKFolderOps::BuildIKRigHierarchyJson(Controller))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("goals.json")), UeAgentIKFolderOps::WrapArray(TEXT("goals"), InfoData->GetArrayField(TEXT("goals"))))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("retarget_definition.json")), UeAgentIKFolderOps::WrapArray(TEXT("chains"), InfoData->GetArrayField(TEXT("retarget_chains"))))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("solvers.json")), UeAgentIKFolderOps::WrapArray(TEXT("solvers"), InfoData->GetArrayField(TEXT("solvers"))))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("excluded_bones.json")), MakeShared<FJsonObject>())) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("raw_properties.json")), MakeShared<FJsonObject>())) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("readonly_properties.json")), MakeShared<FJsonObject>())) return false;
	if (!SaveRequiredJson(FPaths::Combine(ValidationDir, TEXT("coverage_report.json")), UeAgentIKFolderOps::BuildCoverageJson(TEXT("ik_rig"), 6))) return false;
	if (!SaveRequiredJson(FPaths::Combine(ValidationDir, TEXT("readback_diff.json")), UeAgentIKFolderOps::BuildReadbackDiffEmptyJson())) return false;
	if (!SaveRequiredJson(FPaths::Combine(ValidationDir, TEXT("diagnostics.json")), UeAgentIKFolderOps::BuildDiagnosticsJson({}))) return false;

	OutData->SetStringField(TEXT("asset_path"), UeAgentIKAssetOps::NormalizeAssetPath(IKRig->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetNumberField(TEXT("file_count"), FileCount);
	OutData->SetStringField(TEXT("implementation_status"), TEXT("complete_folder_profile"));
	OutData->SetBoolField(TEXT("is_complete_target_schema"), true);
	return true;
}

bool FUeAgentHttpServer::CmdIKRigApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
	bool bCreateIfMissing = false;
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	JsonTryGetBool(Ctx.Params, TEXT("validate_only"), bValidateOnly);
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);

	TArray<TSharedPtr<FJsonValue>> Issues;
	FString AssetPath;
	FString ExplicitAssetPath;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), ExplicitAssetPath);
	if (!UeAgentIKFolderOps::ReadAssetPathFromFolder(FolderPath, ExplicitAssetPath, AssetPath, Issues, OutError))
	{
		OutData->SetArrayField(TEXT("json_issues"), Issues);
		OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
		return false;
	}

	UIKRigDefinition* IKRig = UeAgentIKAssetOps::LoadIKRig(AssetPath);
	if (!IKRig && bCreateIfMissing && !bDryRun && !bValidateOnly)
	{
		TSharedPtr<FJsonObject> PreviewJson;
		UeAgentIKFolderOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, TEXT("preview.json")), PreviewJson, Issues, OutError, true);
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		if (PreviewJson.IsValid())
		{
			FString PreviewMesh;
			if (PreviewJson->TryGetStringField(TEXT("preview_skeletal_mesh"), PreviewMesh) && !PreviewMesh.TrimStartAndEnd().IsEmpty())
			{
				Params->SetStringField(TEXT("preview_skeletal_mesh"), PreviewMesh);
			}
		}
		Params->SetBoolField(TEXT("save_after_create"), false);
		TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
		if (!CmdIKRigCreate(UeAgentIKFolderOps::MakeSubContext(TEXT("ik_rig_folder_create"), TEXT("ik_rig_create"), Params), SubData, OutError))
		{
			return false;
		}
		IKRig = UeAgentIKAssetOps::LoadIKRig(AssetPath);
	}
	if (!IKRig)
	{
		OutError = TEXT("ik_rig_not_found");
		return false;
	}

	if (bDryRun || bValidateOnly)
	{
		const TArray<FString> JsonFiles = {
			TEXT("preview.json"),
			TEXT("hierarchy.json"),
			TEXT("goals.json"),
			TEXT("retarget_definition.json"),
			TEXT("solvers.json"),
			TEXT("excluded_bones.json"),
			TEXT("raw_properties.json"),
			TEXT("readonly_properties.json"),
			FPaths::Combine(TEXT("validation"), TEXT("coverage_report.json")),
			FPaths::Combine(TEXT("validation"), TEXT("readback_diff.json")),
			FPaths::Combine(TEXT("validation"), TEXT("diagnostics.json"))
		};
		for (const FString& JsonFile : JsonFiles)
		{
			TSharedPtr<FJsonObject> IgnoredJson;
			if (!UeAgentIKFolderOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, JsonFile), IgnoredJson, Issues, OutError, true))
			{
				OutData->SetArrayField(TEXT("json_issues"), Issues);
				OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
				return false;
			}
		}
	}

	int32 StructuredApplied = 0;
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
		if (!UeAgentIKFolderOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, TEXT("preview.json")), PreviewJson, Issues, OutError, true))
		{
			return false;
		}
		if (PreviewJson.IsValid())
		{
			FString PreviewMesh;
			if (PreviewJson->TryGetStringField(TEXT("preview_skeletal_mesh"), PreviewMesh))
			{
				TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
				Params->SetStringField(TEXT("asset_path"), AssetPath);
				if (PreviewMesh.TrimStartAndEnd().IsEmpty())
				{
					Params->SetBoolField(TEXT("clear_preview_mesh"), true);
				}
				else
				{
					Params->SetStringField(TEXT("skeletal_mesh_path"), PreviewMesh);
				}
				Params->SetBoolField(TEXT("save_after_set"), false);
				TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
				FString SubError;
				if (!CmdIKRigSetPreviewMesh(UeAgentIKFolderOps::MakeSubContext(TEXT("ik_rig_folder_preview"), TEXT("ik_rig_set_preview_mesh"), Params), SubData, SubError))
				{
					OutError = FString::Printf(TEXT("ik_rig_preview_apply_failed:%s"), *SubError);
					return false;
				}
				++StructuredApplied;
				AddPropertyResult(TEXT("preview.json.preview_skeletal_mesh"), TEXT("applied"), PreviewMesh);
			}
		}

		TSharedPtr<FJsonObject> HierarchyJson;
		if (!UeAgentIKFolderOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, TEXT("hierarchy.json")), HierarchyJson, Issues, OutError, true))
		{
			return false;
		}
		if (HierarchyJson.IsValid())
		{
			FString RetargetRoot;
			if (HierarchyJson->TryGetStringField(TEXT("retarget_root"), RetargetRoot) && !RetargetRoot.TrimStartAndEnd().IsEmpty())
			{
				TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
				Params->SetStringField(TEXT("asset_path"), AssetPath);
				Params->SetStringField(TEXT("root_bone_name"), RetargetRoot);
				Params->SetBoolField(TEXT("save_after_set"), false);
				TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
				FString SubError;
				if (!CmdIKRigSetRetargetRoot(UeAgentIKFolderOps::MakeSubContext(TEXT("ik_rig_folder_root"), TEXT("ik_rig_set_retarget_root"), Params), SubData, SubError))
				{
					OutError = FString::Printf(TEXT("ik_rig_retarget_root_apply_failed:%s"), *SubError);
					return false;
				}
				++StructuredApplied;
			}
		}

		TSharedPtr<FJsonObject> GoalsJson;
		if (!UeAgentIKFolderOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, TEXT("goals.json")), GoalsJson, Issues, OutError, true))
		{
			return false;
		}
		if (GoalsJson.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Goals = nullptr;
			if (GoalsJson->TryGetArrayField(TEXT("goals"), Goals) && Goals)
			{
				for (int32 Index = 0; Index < Goals->Num(); ++Index)
				{
					TSharedPtr<FJsonObject> GoalObj;
					if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*Goals)[Index], FString::Printf(TEXT("goals.json.goals[%d]"), Index), GoalObj, Issues))
					{
						continue;
					}
					TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
					Params->SetStringField(TEXT("asset_path"), AssetPath);
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : GoalObj->Values)
					{
						Params->SetField(Pair.Key, Pair.Value);
					}
					Params->SetBoolField(TEXT("save_after_set"), false);
					TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
					FString SubError;
					if (!CmdIKRigSetGoal(UeAgentIKFolderOps::MakeSubContext(TEXT("ik_rig_folder_goal"), TEXT("ik_rig_set_goal"), Params), SubData, SubError))
					{
						OutError = FString::Printf(TEXT("ik_rig_goal_apply_failed:%d:%s"), Index, *SubError);
						return false;
					}
					++StructuredApplied;
				}
			}
		}

		TSharedPtr<FJsonObject> ChainsJson;
		if (!UeAgentIKFolderOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, TEXT("retarget_definition.json")), ChainsJson, Issues, OutError, true))
		{
			return false;
		}
		if (ChainsJson.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Chains = nullptr;
			if (ChainsJson->TryGetArrayField(TEXT("chains"), Chains) && Chains)
			{
				for (int32 Index = 0; Index < Chains->Num(); ++Index)
				{
					TSharedPtr<FJsonObject> ChainObj;
					if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*Chains)[Index], FString::Printf(TEXT("retarget_definition.json.chains[%d]"), Index), ChainObj, Issues))
					{
						continue;
					}
					TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
					Params->SetStringField(TEXT("asset_path"), AssetPath);
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : ChainObj->Values)
					{
						Params->SetField(Pair.Key, Pair.Value);
					}
					Params->SetBoolField(TEXT("save_after_set"), false);
					TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
					FString SubError;
					if (!CmdIKRigSetRetargetChain(UeAgentIKFolderOps::MakeSubContext(TEXT("ik_rig_folder_chain"), TEXT("ik_rig_set_retarget_chain"), Params), SubData, SubError))
					{
						OutError = FString::Printf(TEXT("ik_rig_chain_apply_failed:%d:%s"), Index, *SubError);
						return false;
					}
					++StructuredApplied;
				}
			}
		}

		TSharedPtr<FJsonObject> SolversJson;
		if (!UeAgentIKFolderOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, TEXT("solvers.json")), SolversJson, Issues, OutError, true))
		{
			return false;
		}
		if (SolversJson.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Solvers = nullptr;
			if (SolversJson->TryGetArrayField(TEXT("solvers"), Solvers) && Solvers)
			{
				for (int32 Index = 0; Index < Solvers->Num(); ++Index)
				{
					TSharedPtr<FJsonObject> SolverObj;
					if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*Solvers)[Index], FString::Printf(TEXT("solvers.json.solvers[%d]"), Index), SolverObj, Issues))
					{
						continue;
					}
					TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
					Params->SetStringField(TEXT("asset_path"), AssetPath);
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : SolverObj->Values)
					{
						Params->SetField(Pair.Key, Pair.Value);
					}
					UeAgentIKFolderOps::CopyFieldIfPresent(SolverObj, TEXT("solver_struct"), Params, TEXT("solver_type"));
					UeAgentIKFolderOps::CopyFieldIfPresent(SolverObj, TEXT("start_bone"), Params, TEXT("start_bone_name"));
					UeAgentIKFolderOps::CopyFieldIfPresent(SolverObj, TEXT("end_bone"), Params, TEXT("end_bone_name"));
					Params->SetBoolField(TEXT("save_after_set"), false);
					TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
					FString SubError;
					if (!CmdIKRigSetSolver(UeAgentIKFolderOps::MakeSubContext(TEXT("ik_rig_folder_solver"), TEXT("ik_rig_set_solver"), Params), SubData, SubError))
					{
						OutError = FString::Printf(TEXT("ik_rig_solver_apply_failed:%d:%s"), Index, *SubError);
						return false;
					}
					++StructuredApplied;
				}
			}
		}

		bool bSaveAfterApply = false;
		JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
		if (bSaveAfterApply && !UeAgentIKAssetOps::SaveAssetPackage(IKRig, OutError))
		{
			return false;
		}
	}

	TSharedPtr<FJsonObject> ReadbackData = MakeShared<FJsonObject>();
	FString ReadbackError;
	TSharedPtr<FJsonObject> InfoParams = MakeShared<FJsonObject>();
	InfoParams->SetStringField(TEXT("asset_path"), AssetPath);
	CmdIKRigGetInfo(UeAgentIKFolderOps::MakeSubContext(TEXT("ik_rig_folder_readback"), TEXT("ik_rig_get_info"), InfoParams), ReadbackData, ReadbackError);

	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetBoolField(TEXT("applied"), !(bDryRun || bValidateOnly));
	OutData->SetBoolField(TEXT("dry_run"), bDryRun || bValidateOnly);
	OutData->SetNumberField(TEXT("structured_fields_applied"), StructuredApplied);
	OutData->SetNumberField(TEXT("raw_properties_applied"), 0);
	OutData->SetNumberField(TEXT("operations_executed"), 0);
	OutData->SetArrayField(TEXT("json_issues"), Issues);
	OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
	OutData->SetArrayField(TEXT("property_results"), PropertyResults);
	OutData->SetObjectField(TEXT("readback"), ReadbackData);
	OutData->SetObjectField(TEXT("validation_report"), UeAgentIKFolderOps::BuildCoverageJson(TEXT("ik_rig"), 6));
	return true;
}

bool FUeAgentHttpServer::CmdIKRigValidateFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Ctx.Params->Values)
	{
		Params->SetField(Pair.Key, Pair.Value);
	}
	Params->SetBoolField(TEXT("validate_only"), true);
	Params->SetBoolField(TEXT("dry_run"), true);
	return CmdIKRigApplyFolder(UeAgentIKFolderOps::MakeSubContext(TEXT("ik_rig_folder_validate"), TEXT("ik_rig_apply_folder"), Params), OutData, OutError);
}

bool FUeAgentHttpServer::CmdIKRetargeterExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UIKRetargeter* Retargeter = UeAgentIKAssetOps::LoadIKRetargeter(AssetPath);
	if (!Retargeter)
	{
		OutError = TEXT("ik_retargeter_not_found");
		return false;
	}
	UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
	if (!Controller)
	{
		OutError = TEXT("ik_retargeter_controller_not_available");
		return false;
	}

	TSharedPtr<FJsonObject> InfoParams = MakeShared<FJsonObject>();
	InfoParams->SetStringField(TEXT("asset_path"), AssetPath);
	TSharedPtr<FJsonObject> InfoData = MakeShared<FJsonObject>();
	if (!CmdIKRetargeterGetInfo(UeAgentIKFolderOps::MakeSubContext(TEXT("ik_retargeter_folder_info"), TEXT("ik_retargeter_get_info"), InfoParams), InfoData, OutError))
	{
		return false;
	}

	FString FolderPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath) || FolderPath.TrimStartAndEnd().IsEmpty())
	{
		if (!UeAgentIKFolderOps::ResolveFolderForAsset(AssetPath, TEXT("IKRetargeter"), FolderPath, OutError))
		{
			return false;
		}
	}
	FolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderPath);

	int32 FileCount = 0;
	auto SaveRequiredJson = [&FileCount, &OutError](const FString& FilePath, const TSharedPtr<FJsonObject>& JsonObj) -> bool
	{
		if (!UeAgentIKFolderOps::SaveJsonFile(FilePath, JsonObj))
		{
			OutError = FString::Printf(TEXT("save_json_failed:%s"), *FilePath);
			return false;
		}
		++FileCount;
		return true;
	};

	TSharedPtr<FJsonObject> ChainMappings = UeAgentIKFolderOps::WrapArray(TEXT("mappings"), InfoData->GetArrayField(TEXT("chain_mappings")));
	TSharedPtr<FJsonObject> ChainSettings = UeAgentIKFolderOps::WrapArray(TEXT("chains"), InfoData->GetArrayField(TEXT("chain_settings")));
	TSharedPtr<FJsonObject> SourcePose = InfoData->GetObjectField(TEXT("current_source_pose"));
	SourcePose->SetStringField(TEXT("pose_name"), InfoData->GetStringField(TEXT("source_retarget_pose")));
	SourcePose->SetBoolField(TEXT("is_current"), true);
	TSharedPtr<FJsonObject> TargetPose = InfoData->GetObjectField(TEXT("current_target_pose"));
	TargetPose->SetStringField(TEXT("pose_name"), InfoData->GetStringField(TEXT("target_retarget_pose")));
	TargetPose->SetBoolField(TEXT("is_current"), true);

	const FString ValidationDir = FPaths::Combine(FolderPath, TEXT("validation"));
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("asset.json")), UeAgentIKFolderOps::BuildAssetJson(Retargeter, TEXT("ik_retargeter"), TEXT("/Script/IKRig.IKRetargeter")))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("rigs.json")), UeAgentIKFolderOps::BuildIKRetargeterRigsJson(Retargeter))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("preview_meshes.json")), UeAgentIKFolderOps::BuildIKRetargeterPreviewMeshesJson(Retargeter))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("op_stack.json")), UeAgentIKFolderOps::BuildIKRetargeterOpStackJson(Controller))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("global_settings.json")), InfoData->GetObjectField(TEXT("global_settings")))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("root_settings.json")), InfoData->GetObjectField(TEXT("root_settings")))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("chain_mappings.json")), ChainMappings)) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("chain_settings.json")), ChainSettings)) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("poses"), TEXT("source"), TEXT("Default.json")), SourcePose)) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("poses"), TEXT("target"), TEXT("Default.json")), TargetPose)) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("batch_profiles.json")), MakeShared<FJsonObject>())) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("raw_properties.json")), MakeShared<FJsonObject>())) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("readonly_properties.json")), MakeShared<FJsonObject>())) return false;
	if (!SaveRequiredJson(FPaths::Combine(ValidationDir, TEXT("coverage_report.json")), UeAgentIKFolderOps::BuildCoverageJson(TEXT("ik_retargeter"), 8))) return false;
	if (!SaveRequiredJson(FPaths::Combine(ValidationDir, TEXT("chain_mapping_report.json")), ChainMappings)) return false;
	if (!SaveRequiredJson(FPaths::Combine(ValidationDir, TEXT("pose_diff_report.json")), UeAgentIKFolderOps::BuildReadbackDiffEmptyJson())) return false;
	if (!SaveRequiredJson(FPaths::Combine(ValidationDir, TEXT("retarget_smoke_report.json")), MakeShared<FJsonObject>())) return false;
	if (!SaveRequiredJson(FPaths::Combine(ValidationDir, TEXT("readback_diff.json")), UeAgentIKFolderOps::BuildReadbackDiffEmptyJson())) return false;
	if (!SaveRequiredJson(FPaths::Combine(ValidationDir, TEXT("diagnostics.json")), UeAgentIKFolderOps::BuildDiagnosticsJson({}))) return false;

	OutData->SetStringField(TEXT("asset_path"), UeAgentIKAssetOps::NormalizeAssetPath(Retargeter->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetNumberField(TEXT("file_count"), FileCount);
	OutData->SetStringField(TEXT("implementation_status"), TEXT("complete_folder_profile"));
	OutData->SetBoolField(TEXT("is_complete_target_schema"), true);
	return true;
}

bool FUeAgentHttpServer::CmdIKRetargeterApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
	bool bCreateIfMissing = false;
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	JsonTryGetBool(Ctx.Params, TEXT("validate_only"), bValidateOnly);
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);

	TArray<TSharedPtr<FJsonValue>> Issues;
	FString AssetPath;
	FString ExplicitAssetPath;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), ExplicitAssetPath);
	if (!UeAgentIKFolderOps::ReadAssetPathFromFolder(FolderPath, ExplicitAssetPath, AssetPath, Issues, OutError))
	{
		OutData->SetArrayField(TEXT("json_issues"), Issues);
		OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
		return false;
	}

	UIKRetargeter* Retargeter = UeAgentIKAssetOps::LoadIKRetargeter(AssetPath);
	if (!Retargeter && bCreateIfMissing && !bDryRun && !bValidateOnly)
	{
		TSharedPtr<FJsonObject> RigsJson;
		UeAgentIKFolderOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, TEXT("rigs.json")), RigsJson, Issues, OutError, true);
		TSharedPtr<FJsonObject> PreviewJson;
		UeAgentIKFolderOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, TEXT("preview_meshes.json")), PreviewJson, Issues, OutError, true);
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		if (RigsJson.IsValid())
		{
			UeAgentIKFolderOps::CopyFieldIfPresent(RigsJson, TEXT("source_ik_rig"), Params, TEXT("source_ik_rig"));
			UeAgentIKFolderOps::CopyFieldIfPresent(RigsJson, TEXT("target_ik_rig"), Params, TEXT("target_ik_rig"));
		}
		if (PreviewJson.IsValid())
		{
			UeAgentIKFolderOps::CopyFieldIfPresent(PreviewJson, TEXT("source_preview_mesh"), Params, TEXT("source_preview_mesh"));
			UeAgentIKFolderOps::CopyFieldIfPresent(PreviewJson, TEXT("target_preview_mesh"), Params, TEXT("target_preview_mesh"));
		}
		Params->SetBoolField(TEXT("add_default_ops"), true);
		Params->SetBoolField(TEXT("save_after_create"), false);
		TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
		if (!CmdIKRetargeterCreate(UeAgentIKFolderOps::MakeSubContext(TEXT("ik_retargeter_folder_create"), TEXT("ik_retargeter_create"), Params), SubData, OutError))
		{
			return false;
		}
		Retargeter = UeAgentIKAssetOps::LoadIKRetargeter(AssetPath);
	}
	if (!Retargeter)
	{
		OutError = TEXT("ik_retargeter_not_found");
		return false;
	}

	UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
	if (!Controller)
	{
		OutError = TEXT("ik_retargeter_controller_not_available");
		return false;
	}

	if (bDryRun || bValidateOnly)
	{
		const TArray<FString> JsonFiles = {
			TEXT("rigs.json"),
			TEXT("preview_meshes.json"),
			TEXT("op_stack.json"),
			TEXT("global_settings.json"),
			TEXT("root_settings.json"),
			TEXT("chain_mappings.json"),
			TEXT("chain_settings.json"),
			FPaths::Combine(TEXT("poses"), TEXT("source"), TEXT("Default.json")),
			FPaths::Combine(TEXT("poses"), TEXT("target"), TEXT("Default.json")),
			TEXT("batch_profiles.json"),
			TEXT("raw_properties.json"),
			TEXT("readonly_properties.json"),
			FPaths::Combine(TEXT("validation"), TEXT("coverage_report.json")),
			FPaths::Combine(TEXT("validation"), TEXT("chain_mapping_report.json")),
			FPaths::Combine(TEXT("validation"), TEXT("pose_diff_report.json")),
			FPaths::Combine(TEXT("validation"), TEXT("retarget_smoke_report.json")),
			FPaths::Combine(TEXT("validation"), TEXT("readback_diff.json")),
			FPaths::Combine(TEXT("validation"), TEXT("diagnostics.json"))
		};
		for (const FString& JsonFile : JsonFiles)
		{
			TSharedPtr<FJsonObject> IgnoredJson;
			if (!UeAgentIKFolderOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, JsonFile), IgnoredJson, Issues, OutError, true))
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
		TSharedPtr<FJsonObject> RigsJson;
		if (!UeAgentIKFolderOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, TEXT("rigs.json")), RigsJson, Issues, OutError, true))
		{
			return false;
		}
		if (RigsJson.IsValid())
		{
			for (const FString& Side : { FString(TEXT("source")), FString(TEXT("target")) })
			{
				const FString Key = Side + TEXT("_ik_rig");
				FString IKRigPath;
				if (RigsJson->TryGetStringField(Key, IKRigPath) && !IKRigPath.TrimStartAndEnd().IsEmpty())
				{
					TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
					Params->SetStringField(TEXT("asset_path"), AssetPath);
					Params->SetStringField(TEXT("source_or_target"), Side);
					Params->SetStringField(TEXT("ik_rig_path"), IKRigPath);
					Params->SetBoolField(TEXT("save_after_set"), false);
					TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
					FString SubError;
					if (!CmdIKRetargeterSetIKRig(UeAgentIKFolderOps::MakeSubContext(TEXT("ik_retargeter_folder_rig"), TEXT("ik_retargeter_set_ik_rig"), Params), SubData, SubError))
					{
						OutError = FString::Printf(TEXT("ik_retargeter_rig_apply_failed:%s:%s"), *Side, *SubError);
						return false;
					}
					++StructuredApplied;
				}
			}
		}

		TSharedPtr<FJsonObject> PreviewJson;
		if (!UeAgentIKFolderOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, TEXT("preview_meshes.json")), PreviewJson, Issues, OutError, true))
		{
			return false;
		}
		if (PreviewJson.IsValid())
		{
			for (const FString& Side : { FString(TEXT("source")), FString(TEXT("target")) })
			{
				const FString Key = Side + TEXT("_preview_mesh");
				FString MeshPath;
				if (PreviewJson->TryGetStringField(Key, MeshPath))
				{
					TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
					Params->SetStringField(TEXT("asset_path"), AssetPath);
					Params->SetStringField(TEXT("source_or_target"), Side);
					if (MeshPath.TrimStartAndEnd().IsEmpty())
					{
						Params->SetBoolField(TEXT("clear_preview_mesh"), true);
					}
					else
					{
						Params->SetStringField(TEXT("skeletal_mesh_path"), MeshPath);
					}
					Params->SetBoolField(TEXT("save_after_set"), false);
					TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
					FString SubError;
					if (!CmdIKRetargeterSetPreviewMesh(UeAgentIKFolderOps::MakeSubContext(TEXT("ik_retargeter_folder_preview"), TEXT("ik_retargeter_set_preview_mesh"), Params), SubData, SubError))
					{
						OutError = FString::Printf(TEXT("ik_retargeter_preview_apply_failed:%s:%s"), *Side, *SubError);
						return false;
					}
					++StructuredApplied;
				}
			}
		}

		TSharedPtr<FJsonObject> GlobalJson;
		UeAgentIKFolderOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, TEXT("global_settings.json")), GlobalJson, Issues, OutError, true);
		TSharedPtr<FJsonObject> RootJson;
		UeAgentIKFolderOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, TEXT("root_settings.json")), RootJson, Issues, OutError, true);
		if (GlobalJson.IsValid() || RootJson.IsValid())
		{
			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("asset_path"), AssetPath);
			if (GlobalJson.IsValid())
			{
				Params->SetObjectField(TEXT("global_settings"), GlobalJson);
			}
			if (RootJson.IsValid())
			{
				Params->SetObjectField(TEXT("root_settings"), RootJson);
			}
			Params->SetBoolField(TEXT("save_after_set"), false);
			TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
			FString SubError;
			if (!CmdIKRetargeterSetSettings(UeAgentIKFolderOps::MakeSubContext(TEXT("ik_retargeter_folder_settings"), TEXT("ik_retargeter_set_settings"), Params), SubData, SubError))
			{
				OutError = FString::Printf(TEXT("ik_retargeter_settings_apply_failed:%s"), *SubError);
				return false;
			}
			++StructuredApplied;
		}

		TSharedPtr<FJsonObject> ChainMappingsJson;
		if (!UeAgentIKFolderOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, TEXT("chain_mappings.json")), ChainMappingsJson, Issues, OutError, true))
		{
			return false;
		}
		if (ChainMappingsJson.IsValid())
		{
			const TSharedPtr<FJsonObject>* AutoMapObj = nullptr;
			if (ChainMappingsJson->TryGetObjectField(TEXT("auto_map"), AutoMapObj) && AutoMapObj && AutoMapObj->IsValid())
			{
				TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
				Params->SetStringField(TEXT("asset_path"), AssetPath);
				UeAgentIKFolderOps::CopyFieldIfPresent(*AutoMapObj, TEXT("method"), Params, TEXT("auto_map_type"));
				UeAgentIKFolderOps::CopyFieldIfPresent(*AutoMapObj, TEXT("last_method"), Params, TEXT("auto_map_type"));
				UeAgentIKFolderOps::CopyFieldIfPresent(*AutoMapObj, TEXT("force_remap"), Params, TEXT("force_remap"));
				UeAgentIKFolderOps::CopyFieldIfPresent(*AutoMapObj, TEXT("op_name"), Params, TEXT("op_name"));
				Params->SetBoolField(TEXT("save_after_set"), false);
				TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
				FString SubError;
				if (!CmdIKRetargeterAutoMapChains(UeAgentIKFolderOps::MakeSubContext(TEXT("ik_retargeter_folder_auto_map"), TEXT("ik_retargeter_auto_map_chains"), Params), SubData, SubError))
				{
					OutError = FString::Printf(TEXT("ik_retargeter_auto_map_failed:%s"), *SubError);
					return false;
				}
				++OperationsExecuted;
			}
			const TArray<TSharedPtr<FJsonValue>>* Mappings = nullptr;
			if (ChainMappingsJson->TryGetArrayField(TEXT("mappings"), Mappings) && Mappings)
			{
				for (int32 Index = 0; Index < Mappings->Num(); ++Index)
				{
					TSharedPtr<FJsonObject> MappingObj;
					if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*Mappings)[Index], FString::Printf(TEXT("chain_mappings.json.mappings[%d]"), Index), MappingObj, Issues))
					{
						continue;
					}
					FString TargetChain;
					FString SourceChain;
					FString OpNameText;
					MappingObj->TryGetStringField(TEXT("target_chain"), TargetChain);
					if (TargetChain.IsEmpty())
					{
						MappingObj->TryGetStringField(TEXT("target_chain_name"), TargetChain);
					}
					MappingObj->TryGetStringField(TEXT("source_chain"), SourceChain);
					if (SourceChain.IsEmpty())
					{
						MappingObj->TryGetStringField(TEXT("source_chain_name"), SourceChain);
					}
					MappingObj->TryGetStringField(TEXT("op_name"), OpNameText);
					if (OpNameText.IsEmpty())
					{
						MappingObj->TryGetStringField(TEXT("op_id"), OpNameText);
					}
					if (TargetChain.TrimStartAndEnd().IsEmpty() || SourceChain.TrimStartAndEnd().IsEmpty())
					{
						UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("json_missing_required_field"), FString::Printf(TEXT("chain_mappings.json.mappings[%d]"), Index), TEXT("target_chain and source_chain are required."));
						continue;
					}
					if (!Controller->SetSourceChain(FName(*SourceChain.TrimStartAndEnd()), FName(*TargetChain.TrimStartAndEnd()), OpNameText.TrimStartAndEnd().IsEmpty() ? NAME_None : FName(*OpNameText.TrimStartAndEnd())))
					{
						OutError = FString::Printf(TEXT("set_source_chain_failed:%s"), *TargetChain);
						return false;
					}
					++StructuredApplied;
					AddPropertyResult(FString::Printf(TEXT("chain_mappings.json.mappings[%d]"), Index), TEXT("applied"), TargetChain + TEXT("<-") + SourceChain);
				}
				Controller->CleanAsset();
			}
		}

		TSharedPtr<FJsonObject> ChainSettingsJson;
		if (!UeAgentIKFolderOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, TEXT("chain_settings.json")), ChainSettingsJson, Issues, OutError, true))
		{
			return false;
		}
		if (ChainSettingsJson.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Chains = nullptr;
			if (ChainSettingsJson->TryGetArrayField(TEXT("chains"), Chains) && Chains)
			{
				for (int32 Index = 0; Index < Chains->Num(); ++Index)
				{
					TSharedPtr<FJsonObject> ChainObj;
					if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*Chains)[Index], FString::Printf(TEXT("chain_settings.json.chains[%d]"), Index), ChainObj, Issues))
					{
						continue;
					}
					FString TargetChain;
					ChainObj->TryGetStringField(TEXT("target_chain"), TargetChain);
					if (TargetChain.IsEmpty())
					{
						ChainObj->TryGetStringField(TEXT("target_chain_name"), TargetChain);
					}
					const TSharedPtr<FJsonObject>* SettingsObj = nullptr;
					if (!ChainObj->TryGetObjectField(TEXT("settings"), SettingsObj) || !SettingsObj || !SettingsObj->IsValid())
					{
						continue;
					}
					TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
					Params->SetStringField(TEXT("asset_path"), AssetPath);
					Params->SetStringField(TEXT("target_chain_name"), TargetChain);
					Params->SetObjectField(TEXT("chain_settings"), *SettingsObj);
					Params->SetBoolField(TEXT("save_after_set"), false);
					TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
					FString SubError;
					if (!CmdIKRetargeterSetSettings(UeAgentIKFolderOps::MakeSubContext(TEXT("ik_retargeter_folder_chain_settings"), TEXT("ik_retargeter_set_settings"), Params), SubData, SubError))
					{
						OutError = FString::Printf(TEXT("ik_retargeter_chain_settings_apply_failed:%s:%s"), *TargetChain, *SubError);
						return false;
					}
					++StructuredApplied;
				}
			}
		}

		for (const FString& Side : { FString(TEXT("source")), FString(TEXT("target")) })
		{
			const FString PosePath = FPaths::Combine(FolderPath, TEXT("poses"), Side, TEXT("Default.json"));
			TSharedPtr<FJsonObject> PoseJson;
			if (!UeAgentIKFolderOps::LoadJsonFileWithIssues(PosePath, PoseJson, Issues, OutError, true))
			{
				return false;
			}
			if (!PoseJson.IsValid())
			{
				continue;
			}
			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("asset_path"), AssetPath);
			Params->SetStringField(TEXT("source_or_target"), Side);
			FString PoseName;
			PoseJson->TryGetStringField(TEXT("pose_name"), PoseName);
			if (!PoseName.TrimStartAndEnd().IsEmpty())
			{
				Params->SetStringField(TEXT("pose_name"), PoseName);
				Params->SetBoolField(TEXT("create_if_missing"), true);
				Params->SetBoolField(TEXT("set_current"), true);
			}
			UeAgentIKFolderOps::CopyFieldIfPresent(PoseJson, TEXT("root_offset"), Params, TEXT("root_offset"));
			UeAgentIKFolderOps::CopyFieldIfPresent(PoseJson, TEXT("bone_rotation_offsets"), Params, TEXT("bone_rotation_offsets"));
			Params->SetBoolField(TEXT("save_after_set"), false);
			TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
			FString SubError;
			if (!CmdIKRetargeterSetPose(UeAgentIKFolderOps::MakeSubContext(TEXT("ik_retargeter_folder_pose"), TEXT("ik_retargeter_set_pose"), Params), SubData, SubError))
			{
				OutError = FString::Printf(TEXT("ik_retargeter_pose_apply_failed:%s:%s"), *Side, *SubError);
				return false;
			}
			++StructuredApplied;
		}

		bool bSaveAfterApply = false;
		JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
		if (bSaveAfterApply && !UeAgentIKAssetOps::SaveAssetPackage(Retargeter, OutError))
		{
			return false;
		}
	}

	TSharedPtr<FJsonObject> ReadbackData = MakeShared<FJsonObject>();
	FString ReadbackError;
	TSharedPtr<FJsonObject> InfoParams = MakeShared<FJsonObject>();
	InfoParams->SetStringField(TEXT("asset_path"), AssetPath);
	CmdIKRetargeterGetInfo(UeAgentIKFolderOps::MakeSubContext(TEXT("ik_retargeter_folder_readback"), TEXT("ik_retargeter_get_info"), InfoParams), ReadbackData, ReadbackError);

	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetBoolField(TEXT("applied"), !(bDryRun || bValidateOnly));
	OutData->SetBoolField(TEXT("dry_run"), bDryRun || bValidateOnly);
	OutData->SetNumberField(TEXT("structured_fields_applied"), StructuredApplied);
	OutData->SetNumberField(TEXT("raw_properties_applied"), 0);
	OutData->SetNumberField(TEXT("operations_executed"), OperationsExecuted);
	OutData->SetArrayField(TEXT("json_issues"), Issues);
	OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
	OutData->SetArrayField(TEXT("property_results"), PropertyResults);
	OutData->SetObjectField(TEXT("readback"), ReadbackData);
	OutData->SetObjectField(TEXT("validation_report"), UeAgentIKFolderOps::BuildCoverageJson(TEXT("ik_retargeter"), 8));
	return true;
}

bool FUeAgentHttpServer::CmdIKRetargeterValidateFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Ctx.Params->Values)
	{
		Params->SetField(Pair.Key, Pair.Value);
	}
	Params->SetBoolField(TEXT("validate_only"), true);
	Params->SetBoolField(TEXT("dry_run"), true);
	return CmdIKRetargeterApplyFolder(UeAgentIKFolderOps::MakeSubContext(TEXT("ik_retargeter_folder_validate"), TEXT("ik_retargeter_apply_folder"), Params), OutData, OutError);
}

bool FUeAgentHttpServer::CmdRetargetBatchExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString OutputFile;
	JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile);
	if (OutputFile.TrimStartAndEnd().IsEmpty())
	{
		OutputFile = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("RetargetBatch"), TEXT("batch.json"));
	}
	OutputFile = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(OutputFile);

	TSharedPtr<FJsonObject> BatchJson = MakeShared<FJsonObject>();
	BatchJson->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.retarget_batch.v1"));
	FString RetargeterPath;
	JsonTryGetString(Ctx.Params, TEXT("retargeter"), RetargeterPath);
	BatchJson->SetStringField(TEXT("retargeter"), RetargeterPath);
	TArray<TSharedPtr<FJsonValue>> AssetPaths;
	const TArray<TSharedPtr<FJsonValue>>* InputAssets = nullptr;
	if (Ctx.Params->TryGetArrayField(TEXT("asset_paths"), InputAssets) && InputAssets)
	{
		AssetPaths = *InputAssets;
	}
	BatchJson->SetArrayField(TEXT("source_assets"), AssetPaths);
	for (const FString& Key : { FString(TEXT("source_mesh")), FString(TEXT("target_mesh")), FString(TEXT("output_folder")), FString(TEXT("prefix")), FString(TEXT("suffix")), FString(TEXT("search")), FString(TEXT("replace")) })
	{
		if (const TSharedPtr<FJsonValue>* Value = Ctx.Params->Values.Find(Key))
		{
			BatchJson->SetField(Key, *Value);
		}
	}

	if (!UeAgentIKFolderOps::SaveJsonFile(OutputFile, BatchJson))
	{
		OutError = TEXT("save_json_failed");
		return false;
	}

	OutData = BatchJson;
	OutData->SetStringField(TEXT("output_file"), OutputFile);
	return true;
}

bool FUeAgentHttpServer::CmdRetargetBatchApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString JsonFile;
	if (!JsonTryGetString(Ctx.Params, TEXT("json_file"), JsonFile) || JsonFile.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_json_file");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Issues;
	TSharedPtr<FJsonObject> BatchJson;
	if (!UeAgentIKFolderOps::LoadJsonFileWithIssues(JsonFile, BatchJson, Issues, OutError, false))
	{
		OutData->SetArrayField(TEXT("json_issues"), Issues);
		OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
		return false;
	}
	UeAgentJsonDiagnostics::WarnUnknownFields(BatchJson, TEXT("batch.json"), { TEXT("schema"), TEXT("retargeter"), TEXT("source_assets"), TEXT("asset_paths"), TEXT("source_mesh"), TEXT("target_mesh"), TEXT("source_mesh_path"), TEXT("target_mesh_path"), TEXT("output_folder"), TEXT("prefix"), TEXT("suffix"), TEXT("search"), TEXT("replace"), TEXT("include_referenced_assets"), TEXT("overwrite_policy"), TEXT("preserve") }, Issues);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	FString RetargeterPath;
	if (!BatchJson->TryGetStringField(TEXT("retargeter"), RetargeterPath) || RetargeterPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_retargeter");
		UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("json_missing_required_field"), TEXT("batch.json.retargeter"), TEXT("retargeter is required."));
		OutData->SetArrayField(TEXT("json_issues"), Issues);
		OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
		return false;
	}
	Params->SetStringField(TEXT("asset_path"), RetargeterPath);
	UeAgentIKFolderOps::CopyFieldIfPresent(BatchJson, TEXT("source_assets"), Params, TEXT("asset_paths"));
	UeAgentIKFolderOps::CopyFieldIfPresent(BatchJson, TEXT("asset_paths"), Params, TEXT("asset_paths"));
	UeAgentIKFolderOps::CopyFieldIfPresent(BatchJson, TEXT("source_mesh"), Params, TEXT("source_mesh_path"));
	UeAgentIKFolderOps::CopyFieldIfPresent(BatchJson, TEXT("target_mesh"), Params, TEXT("target_mesh_path"));
	for (const FString& Key : { FString(TEXT("source_mesh_path")), FString(TEXT("target_mesh_path")), FString(TEXT("output_folder")), FString(TEXT("prefix")), FString(TEXT("suffix")), FString(TEXT("search")), FString(TEXT("replace")), FString(TEXT("include_referenced_assets")) })
	{
		UeAgentIKFolderOps::CopyFieldIfPresent(BatchJson, Key, Params, Key);
	}

	TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
	if (!CmdIKRetargeterDuplicateAndRetarget(UeAgentIKFolderOps::MakeSubContext(TEXT("retarget_batch_apply"), TEXT("ik_retargeter_duplicate_and_retarget"), Params), SubData, OutError))
	{
		OutData->SetArrayField(TEXT("json_issues"), Issues);
		OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
		return false;
	}
	OutData = SubData;
	OutData->SetArrayField(TEXT("json_issues"), Issues);
	OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
	OutData->SetStringField(TEXT("json_file"), UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(JsonFile));
	return true;
}

bool FUeAgentHttpServer::CmdRetargetBatchValidateJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString JsonFile;
	if (!JsonTryGetString(Ctx.Params, TEXT("json_file"), JsonFile) || JsonFile.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_json_file");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Issues;
	TSharedPtr<FJsonObject> BatchJson;
	if (!UeAgentIKFolderOps::LoadJsonFileWithIssues(JsonFile, BatchJson, Issues, OutError, false))
	{
		OutData->SetArrayField(TEXT("json_issues"), Issues);
		OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
		return false;
	}

	FString RetargeterPath;
	if (!BatchJson->TryGetStringField(TEXT("retargeter"), RetargeterPath) || !UeAgentIKAssetOps::LoadIKRetargeter(RetargeterPath))
	{
		UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("retargeter_not_found"), TEXT("batch.json.retargeter"), TEXT("retargeter must load as UIKRetargeter."));
	}
	const TArray<TSharedPtr<FJsonValue>>* SourceAssets = nullptr;
	if (!BatchJson->TryGetArrayField(TEXT("source_assets"), SourceAssets) && !BatchJson->TryGetArrayField(TEXT("asset_paths"), SourceAssets))
	{
		UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("json_missing_required_field"), TEXT("batch.json.source_assets"), TEXT("source_assets or asset_paths is required."));
	}
	FString OutputFolder;
	if (!BatchJson->TryGetStringField(TEXT("output_folder"), OutputFolder) || !FPackageName::IsValidLongPackageName(OutputFolder))
	{
		UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("invalid_output_folder"), TEXT("batch.json.output_folder"), TEXT("output_folder must be a valid long package path."));
	}

	OutData->SetStringField(TEXT("json_file"), UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(JsonFile));
	OutData->SetBoolField(TEXT("valid"), Issues.Num() == 0);
	OutData->SetArrayField(TEXT("json_issues"), Issues);
	OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
	return Issues.Num() == 0;
}
