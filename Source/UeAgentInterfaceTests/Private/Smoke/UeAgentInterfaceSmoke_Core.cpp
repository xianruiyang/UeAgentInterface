#if WITH_DEV_AUTOMATION_TESTS

#include "UeAgentSmokeTestSharedIncludes.inl"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfacePingStatusSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.PingStatus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfacePingStatusSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	FHttpSmokeResult PingResult;
	TestTrue(TEXT("Ping request succeeded"), ExecuteHttpJsonRequest(TEXT("GET"), ServerScope.BaseUrl + TEXT("/api/ping"), FString(), FString(), PingResult));
	TestEqual(TEXT("Ping status code"), PingResult.StatusCode, 200);

	TSharedPtr<FJsonObject> PingJson;
	TestTrue(TEXT("Ping response parses as JSON"), ParseJson(PingResult.Body, PingJson));
	TestTrue(TEXT("Ping response ok=true"), PingJson.IsValid() && PingJson->GetBoolField(TEXT("ok")));

	FHttpSmokeResult StatusResult;
	TestTrue(TEXT("Status request succeeded"), ExecuteHttpJsonRequest(TEXT("GET"), ServerScope.BaseUrl + TEXT("/api/status"), ServerScope.Token, FString(), StatusResult));
	TestEqual(TEXT("Status status code"), StatusResult.StatusCode, 200);

	TSharedPtr<FJsonObject> StatusJson;
	TestTrue(TEXT("Status response parses as JSON"), ParseJson(StatusResult.Body, StatusJson));
	TestTrue(TEXT("Status root ok=true"), StatusJson.IsValid() && StatusJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* StatusData = nullptr;
	TestTrue(TEXT("Status contains data object"), StatusJson.IsValid() && StatusJson->TryGetObjectField(TEXT("data"), StatusData) && StatusData && StatusData->IsValid());
	if (StatusData && StatusData->IsValid())
	{
		TestTrue(TEXT("Status data running=true"), (*StatusData)->GetBoolField(TEXT("running")));
		TestEqual(TEXT("Status data port matches settings"), static_cast<int32>((*StatusData)->GetIntegerField(TEXT("port"))), UUeAgentInterfaceSettings::Get()->Port);
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceAuthorizationSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.AuthorizationFailures",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceAuthorizationSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	FHttpSmokeResult StatusResult;
	TestTrue(TEXT("Unauthorized status request completed"), ExecuteHttpJsonRequest(TEXT("GET"), ServerScope.BaseUrl + TEXT("/api/status"), FString(), FString(), StatusResult));
	TestEqual(TEXT("Unauthorized status code"), StatusResult.StatusCode, 401);

	TSharedPtr<FJsonObject> StatusJson;
	TestTrue(TEXT("Unauthorized status response parses as JSON"), ParseJson(StatusResult.Body, StatusJson));
	TestTrue(TEXT("Unauthorized status root ok=false"), StatusJson.IsValid() && !StatusJson->GetBoolField(TEXT("ok")));
	TestEqual(TEXT("Unauthorized status error code"), StatusJson.IsValid() ? StatusJson->GetStringField(TEXT("error")) : FString(), FString(TEXT("unauthorized")));

	const TSharedPtr<FJsonObject>* StatusData = nullptr;
	TestTrue(TEXT("Unauthorized status contains data object"), StatusJson.IsValid() && StatusJson->TryGetObjectField(TEXT("data"), StatusData) && StatusData && StatusData->IsValid());
	if (StatusData && StatusData->IsValid())
	{
		TestEqual(TEXT("Unauthorized status nested error code"), (*StatusData)->GetStringField(TEXT("error_code")), FString(TEXT("unauthorized")));
		TestTrue(TEXT("Unauthorized status nested error message is non-empty"), !(*StatusData)->GetStringField(TEXT("error_message")).IsEmpty());
	}

	const FString UnauthorizedExecBody = MakeExecRequestBody(TEXT("smoke-auth-exec-001"), TEXT("get_world_state"));
	FHttpSmokeResult ExecResult;
	TestTrue(TEXT("Unauthorized exec request completed"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), TEXT("wrong-token"), UnauthorizedExecBody, ExecResult));
	TestEqual(TEXT("Unauthorized exec status code"), ExecResult.StatusCode, 401);

	TSharedPtr<FJsonObject> ExecJson;
	TestTrue(TEXT("Unauthorized exec response parses as JSON"), ParseJson(ExecResult.Body, ExecJson));
	TestTrue(TEXT("Unauthorized exec root ok=false"), ExecJson.IsValid() && !ExecJson->GetBoolField(TEXT("ok")));
	TestEqual(TEXT("Unauthorized exec error code"), ExecJson.IsValid() ? ExecJson->GetStringField(TEXT("error")) : FString(), FString(TEXT("unauthorized")));

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceAssetReferenceGraphSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.AssetReferenceGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceAssetReferenceGraphSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	auto JsonQuote = [](const FString& Text) -> FString
	{
		FString Out = Text;
		Out.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Out.ReplaceInline(TEXT("\""), TEXT("\\\""));
		Out.ReplaceInline(TEXT("\n"), TEXT("\\n"));
		Out.ReplaceInline(TEXT("\r"), TEXT("\\r"));
		Out.ReplaceInline(TEXT("\t"), TEXT("\\t"));
		return FString::Printf(TEXT("\"%s\""), *Out);
	};

	auto ExecCommand = [this, &ServerScope](const FString& RequestId, const FString& Command, const FString& ParamsJson, TSharedPtr<FJsonObject>& OutData, const int32 ExpectedStatus = 200) -> bool
	{
		FHttpSmokeResult Result;
		const FString Body = MakeExecRequestBody(RequestId, Command, ParamsJson);
		if (!TestTrue(Command + TEXT(" request completed"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, Body, Result, 20.0)))
		{
			return false;
		}
		TestEqual(Command + TEXT(" status"), Result.StatusCode, ExpectedStatus);

		TSharedPtr<FJsonObject> Root;
		if (!TestTrue(Command + TEXT(" response parses"), ParseJson(Result.Body, Root)) || !Root.IsValid())
		{
			return false;
		}

		const bool bOk = Root->GetBoolField(TEXT("ok"));
		TestEqual(Command + TEXT(" ok state"), bOk, ExpectedStatus == 200);
		const TSharedPtr<FJsonObject>* DataObj = nullptr;
		if (Root->TryGetObjectField(TEXT("data"), DataObj) && DataObj && DataObj->IsValid())
		{
			OutData = *DataObj;
		}
		if (ExpectedStatus != 200)
		{
			return true;
		}

		if (!TestTrue(Command + TEXT(" contains data"), Root->TryGetObjectField(TEXT("data"), DataObj) && DataObj && DataObj->IsValid()))
		{
			return false;
		}
		OutData = *DataObj;
		return true;
	};

	const FString MeshPath = MakeAutomationAssetPath(TEXT("SMReferenceGraphCube"));
	TSharedPtr<FJsonObject> Data;
	TestTrue(TEXT("asset_duplicate setup"), ExecCommand(
		TEXT("smoke-refgraph-duplicate"),
		TEXT("asset_duplicate"),
		FString::Printf(TEXT("{\"source_asset_path\":\"/Engine/BasicShapes/Cube\",\"destination_asset_path\":%s,\"save_after_duplicate\":true}"), *JsonQuote(MeshPath)),
		Data));

	const FString MoveSourcePath = MakeAutomationAssetPath(TEXT("SMAssetMoveSourceCube"));
	const FString MoveTargetPath = MakeAutomationAssetPath(TEXT("SMAssetMoveMovedCube"));
	TestTrue(TEXT("asset_move duplicate setup"), ExecCommand(
		TEXT("smoke-asset-move-duplicate"),
		TEXT("asset_duplicate"),
		FString::Printf(TEXT("{\"source_asset_path\":\"/Engine/BasicShapes/Cube\",\"destination_asset_path\":%s,\"save_after_duplicate\":true}"), *JsonQuote(MoveSourcePath)),
		Data));

	TSharedPtr<FJsonObject> MoveData;
	TestTrue(TEXT("asset_move moves asset"), ExecCommand(
		TEXT("smoke-asset-move"),
		TEXT("asset_move"),
		FString::Printf(TEXT("{\"source_asset_path\":%s,\"destination_asset_path\":%s,\"save_after_move\":true}"), *JsonQuote(MoveSourcePath), *JsonQuote(MoveTargetPath)),
		MoveData));
	TestEqual(TEXT("asset_move target path"), MoveData.IsValid() ? MoveData->GetStringField(TEXT("asset_path")) : FString(), MoveTargetPath);
	TestTrue(TEXT("asset_move reports changed"), MoveData.IsValid() && MoveData->GetBoolField(TEXT("changed")));
	TestTrue(TEXT("asset_move reports saved"), MoveData.IsValid() && MoveData->GetBoolField(TEXT("saved")));

	TSharedPtr<FJsonObject> MoveRefData;
	TestTrue(TEXT("asset_move moved asset is queryable"), ExecCommand(
		TEXT("smoke-asset-move-refgraph"),
		TEXT("asset_reference_graph"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"direction\":\"dependencies\",\"recursive\":false,\"include_searchable_names\":false,\"force_scan\":true}"), *JsonQuote(MoveTargetPath)),
		MoveRefData));
	TestTrue(TEXT("asset_move moved asset found"), MoveRefData.IsValid() && MoveRefData->GetBoolField(TEXT("asset_found")));

	TSharedPtr<FJsonObject> FixupData;
	TestTrue(TEXT("asset_fixup_redirectors deletes source redirector"), ExecCommand(
		TEXT("smoke-asset-fixup-redirectors"),
		TEXT("asset_fixup_redirectors"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"dry_run\":false}"), *JsonQuote(MoveSourcePath)),
		FixupData));
	const int32 FixupRedirectorCount = FixupData.IsValid() ? FixupData->GetIntegerField(TEXT("redirector_count")) : 0;
	const int32 FixupDeletableRedirectorCount = FixupData.IsValid() ? FixupData->GetIntegerField(TEXT("deletable_redirector_count")) : 0;
	TestTrue(TEXT("asset_fixup_redirectors handles absent or deletable redirector"), FixupRedirectorCount == 0 || FixupDeletableRedirectorCount == FixupRedirectorCount);
	TestEqual(TEXT("asset_fixup_redirectors removed package"), FixupData.IsValid() ? FixupData->GetIntegerField(TEXT("remaining_package_count")) : 1, 0);

	TSharedPtr<FJsonObject> MoveSourceAfterFixupData;
	TestTrue(TEXT("asset_fixup_redirectors source path is gone"), ExecCommand(
		TEXT("smoke-asset-fixup-source-refgraph"),
		TEXT("asset_reference_graph"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"allow_missing\":true,\"direction\":\"both\",\"recursive\":false,\"force_scan\":true}"), *JsonQuote(MoveSourcePath)),
		MoveSourceAfterFixupData));
	TestFalse(TEXT("asset_fixup_redirectors source redirector not found"), MoveSourceAfterFixupData.IsValid() && MoveSourceAfterFixupData->GetBoolField(TEXT("asset_found")));

	const FString DeleteAssetPath = MakeAutomationAssetPath(TEXT("SMAssetDeleteCube"));
	TestTrue(TEXT("asset_delete duplicate setup"), ExecCommand(
		TEXT("smoke-asset-delete-duplicate"),
		TEXT("asset_duplicate"),
		FString::Printf(TEXT("{\"source_asset_path\":\"/Engine/BasicShapes/Cube\",\"destination_asset_path\":%s,\"save_after_duplicate\":true}"), *JsonQuote(DeleteAssetPath)),
		Data));

	TSharedPtr<FJsonObject> DeleteDryRunData;
	TestTrue(TEXT("asset_delete dry run succeeds for unreferenced asset"), ExecCommand(
		TEXT("smoke-asset-delete-dry-run"),
		TEXT("asset_delete"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"dry_run\":true,\"include_path_string_hits\":true}"), *JsonQuote(DeleteAssetPath)),
		DeleteDryRunData));
	TestEqual(TEXT("asset_delete dry run sees one deletable asset"), DeleteDryRunData.IsValid() ? DeleteDryRunData->GetIntegerField(TEXT("deletable_count")) : 0, 1);
	TestEqual(TEXT("asset_delete dry run deletes nothing"), DeleteDryRunData.IsValid() ? DeleteDryRunData->GetIntegerField(TEXT("deleted_count")) : -1, 0);

	TSharedPtr<FJsonObject> DeleteData;
	TestTrue(TEXT("asset_delete deletes unreferenced asset"), ExecCommand(
		TEXT("smoke-asset-delete"),
		TEXT("asset_delete"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"dry_run\":false,\"include_path_string_hits\":true}"), *JsonQuote(DeleteAssetPath)),
		DeleteData,
		200));
	TestEqual(TEXT("asset_delete deleted_count"), DeleteData.IsValid() ? DeleteData->GetIntegerField(TEXT("deleted_count")) : 0, 1);
	TestEqual(TEXT("asset_delete remaining_package_count"), DeleteData.IsValid() ? DeleteData->GetIntegerField(TEXT("remaining_package_count")) : 1, 0);

	TSharedPtr<FJsonObject> DeleteAfterData;
	TestTrue(TEXT("asset_delete target is gone after delete"), ExecCommand(
		TEXT("smoke-asset-delete-refgraph-after"),
		TEXT("asset_reference_graph"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"allow_missing\":true,\"direction\":\"both\",\"recursive\":false,\"force_scan\":true}"), *JsonQuote(DeleteAssetPath)),
		DeleteAfterData));
	TestFalse(TEXT("asset_delete removed asset is not found"), DeleteAfterData.IsValid() && DeleteAfterData->GetBoolField(TEXT("asset_found")));

	const FString DeleteFolderRoot = TEXT("/Game/__UeAgentInterfaceSmoke/AssetDeleteFolder");
	const FString DeleteFolderNested = DeleteFolderRoot + TEXT("/Nested");
	const FString DeleteInternalParentPath = DeleteFolderNested + TEXT("/MAssetDeleteInternalParent");
	const FString DeleteInternalChildPath = DeleteFolderNested + TEXT("/MIAssetDeleteInternalChild");
	TestTrue(TEXT("asset_delete internal parent material setup"), ExecCommand(
		TEXT("smoke-asset-delete-internal-parent"),
		TEXT("material_create"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"save_after_create\":true}"), *JsonQuote(DeleteInternalParentPath)),
		Data));
	TestTrue(TEXT("asset_delete internal child material instance setup"), ExecCommand(
		TEXT("smoke-asset-delete-internal-child"),
		TEXT("material_instance_create"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"parent_material_path\":%s,\"save_after_create\":true}"), *JsonQuote(DeleteInternalChildPath), *JsonQuote(DeleteInternalParentPath)),
		Data));

	TSharedPtr<FJsonObject> InternalDeleteDryRunData;
	TestTrue(TEXT("asset_delete ignores same batch referencers"), ExecCommand(
		TEXT("smoke-asset-delete-internal-dry-run"),
		TEXT("asset_delete"),
		FString::Printf(TEXT("{\"asset_paths\":[%s,%s],\"dry_run\":true,\"include_path_string_hits\":false}"), *JsonQuote(DeleteInternalParentPath), *JsonQuote(DeleteInternalChildPath)),
		InternalDeleteDryRunData));
	TestEqual(TEXT("asset_delete internal dry run blocked_count"), InternalDeleteDryRunData.IsValid() ? InternalDeleteDryRunData->GetIntegerField(TEXT("blocked_count")) : -1, 0);
	TestEqual(TEXT("asset_delete internal dry run deletable_count"), InternalDeleteDryRunData.IsValid() ? InternalDeleteDryRunData->GetIntegerField(TEXT("deletable_count")) : 0, 2);
	bool bSawInternalReferencer = false;
	const TArray<TSharedPtr<FJsonValue>>* InternalAssets = nullptr;
	if (InternalDeleteDryRunData.IsValid() && InternalDeleteDryRunData->TryGetArrayField(TEXT("assets"), InternalAssets) && InternalAssets)
	{
		for (const TSharedPtr<FJsonValue>& AssetValue : *InternalAssets)
		{
			const TSharedPtr<FJsonObject>* AssetObj = nullptr;
			if (AssetValue.IsValid() && AssetValue->TryGetObject(AssetObj) && AssetObj && AssetObj->IsValid())
			{
				bSawInternalReferencer |= (*AssetObj)->GetIntegerField(TEXT("internal_referencer_count")) > 0;
			}
		}
	}
	TestTrue(TEXT("asset_delete reports internal referencer diagnostics"), bSawInternalReferencer);

	TSharedPtr<FJsonObject> FolderDeleteDryRunData;
	TestTrue(TEXT("asset_delete folder recursive dry run succeeds"), ExecCommand(
		TEXT("smoke-asset-delete-folder-dry-run"),
		TEXT("asset_delete"),
		FString::Printf(TEXT("{\"folder_path\":%s,\"recursive\":true,\"dry_run\":true,\"include_path_string_hits\":false}"), *JsonQuote(DeleteFolderRoot)),
		FolderDeleteDryRunData));
	TestEqual(TEXT("asset_delete folder dry run requested folder count"), FolderDeleteDryRunData.IsValid() ? FolderDeleteDryRunData->GetIntegerField(TEXT("requested_folder_count")) : 0, 1);
	TestEqual(TEXT("asset_delete folder dry run unique package count"), FolderDeleteDryRunData.IsValid() ? FolderDeleteDryRunData->GetIntegerField(TEXT("unique_package_count")) : 0, 2);
	TestEqual(TEXT("asset_delete folder dry run deletable count"), FolderDeleteDryRunData.IsValid() ? FolderDeleteDryRunData->GetIntegerField(TEXT("deletable_count")) : 0, 2);

	TSharedPtr<FJsonObject> FolderDeleteData;
	TestTrue(TEXT("asset_delete folder recursive deletes resolved assets"), ExecCommand(
		TEXT("smoke-asset-delete-folder-delete"),
		TEXT("asset_delete"),
		FString::Printf(TEXT("{\"folder_path\":%s,\"recursive\":true,\"dry_run\":false,\"include_path_string_hits\":false}"), *JsonQuote(DeleteFolderRoot)),
		FolderDeleteData,
		200));
	TestEqual(TEXT("asset_delete folder recursive deleted_count"), FolderDeleteData.IsValid() ? FolderDeleteData->GetIntegerField(TEXT("deleted_count")) : 0, 2);
	TestEqual(TEXT("asset_delete folder recursive remaining_package_count"), FolderDeleteData.IsValid() ? FolderDeleteData->GetIntegerField(TEXT("remaining_package_count")) : 1, 0);
	TestEqual(TEXT("asset_delete folder recursive folder_delete_count"), FolderDeleteData.IsValid() ? FolderDeleteData->GetIntegerField(TEXT("folder_delete_count")) : 0, 1);
	const FString DeleteFolderRootFilename = FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(DeleteFolderRoot));
	TestFalse(TEXT("asset_delete folder recursive removes requested empty folder"), IFileManager::Get().DirectoryExists(*DeleteFolderRootFilename));

	const FString DeleteKeepFolderRoot = TEXT("/Game/__UeAgentInterfaceSmoke/AssetDeleteKeepFolder");
	const FString DeleteKeepFolderAssetPath = DeleteKeepFolderRoot + TEXT("/MAssetDeleteKeepFolder");
	TestTrue(TEXT("asset_delete keep folder material setup"), ExecCommand(
		TEXT("smoke-asset-delete-keep-folder-asset"),
		TEXT("material_create"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"save_after_create\":true}"), *JsonQuote(DeleteKeepFolderAssetPath)),
		Data));
	const FString DeleteKeepFolderFilename = FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(DeleteKeepFolderRoot));
	TestTrue(TEXT("asset_delete keep folder exists before delete"), IFileManager::Get().DirectoryExists(*DeleteKeepFolderFilename));

	TSharedPtr<FJsonObject> KeepFolderDeleteData;
	TestTrue(TEXT("asset_delete can keep empty folder when requested"), ExecCommand(
		TEXT("smoke-asset-delete-keep-folder-delete"),
		TEXT("asset_delete"),
		FString::Printf(TEXT("{\"folder_path\":%s,\"recursive\":true,\"dry_run\":false,\"include_path_string_hits\":false,\"delete_empty_folders\":false}"), *JsonQuote(DeleteKeepFolderRoot)),
		KeepFolderDeleteData,
		200));
	TestEqual(TEXT("asset_delete keep folder deleted_count"), KeepFolderDeleteData.IsValid() ? KeepFolderDeleteData->GetIntegerField(TEXT("deleted_count")) : 0, 1);
	TestEqual(TEXT("asset_delete keep folder folder_delete_count"), KeepFolderDeleteData.IsValid() ? KeepFolderDeleteData->GetIntegerField(TEXT("folder_delete_count")) : -1, 0);
	TestTrue(TEXT("asset_delete keep folder leaves requested empty folder"), IFileManager::Get().DirectoryExists(*DeleteKeepFolderFilename));

	TSharedPtr<FJsonObject> EmptyFolderDeleteData;
	TestTrue(TEXT("asset_delete deletes empty folder with no resolved assets"), ExecCommand(
		TEXT("smoke-asset-delete-empty-folder-delete"),
		TEXT("asset_delete"),
		FString::Printf(TEXT("{\"folder_path\":%s,\"recursive\":true,\"dry_run\":false,\"include_path_string_hits\":false}"), *JsonQuote(DeleteKeepFolderRoot)),
		EmptyFolderDeleteData,
		200));
	TestEqual(TEXT("asset_delete empty folder deleted_count"), EmptyFolderDeleteData.IsValid() ? EmptyFolderDeleteData->GetIntegerField(TEXT("deleted_count")) : -1, 0);
	TestEqual(TEXT("asset_delete empty folder folder_delete_count"), EmptyFolderDeleteData.IsValid() ? EmptyFolderDeleteData->GetIntegerField(TEXT("folder_delete_count")) : 0, 1);
	TestFalse(TEXT("asset_delete empty folder removes requested folder"), IFileManager::Get().DirectoryExists(*DeleteKeepFolderFilename));

	const FString DeleteMultiFolderRoot = TEXT("/Game/__UeAgentInterfaceSmoke/AssetDeleteMultiFolder");
	const FString DeleteMultiFolderA = DeleteMultiFolderRoot + TEXT("/A");
	const FString DeleteMultiFolderB = DeleteMultiFolderRoot + TEXT("/B");
	const FString DeleteMultiParentPath = DeleteMultiFolderA + TEXT("/MAssetDeleteMultiParent");
	const FString DeleteMultiChildPath = DeleteMultiFolderB + TEXT("/MIAssetDeleteMultiChild");
	TestTrue(TEXT("asset_delete multi folder parent material setup"), ExecCommand(
		TEXT("smoke-asset-delete-multi-folder-parent"),
		TEXT("material_create"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"save_after_create\":true}"), *JsonQuote(DeleteMultiParentPath)),
		Data));
	TestTrue(TEXT("asset_delete multi folder child material instance setup"), ExecCommand(
		TEXT("smoke-asset-delete-multi-folder-child"),
		TEXT("material_instance_create"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"parent_material_path\":%s,\"save_after_create\":true}"), *JsonQuote(DeleteMultiChildPath), *JsonQuote(DeleteMultiParentPath)),
		Data));

	TSharedPtr<FJsonObject> MultiFolderDeleteDryRunData;
	TestTrue(TEXT("asset_delete multi folder dry run succeeds"), ExecCommand(
		TEXT("smoke-asset-delete-multi-folder-dry-run"),
		TEXT("asset_delete"),
		FString::Printf(TEXT("{\"folder_paths\":[%s,%s],\"recursive\":false,\"dry_run\":true,\"include_path_string_hits\":false}"), *JsonQuote(DeleteMultiFolderA), *JsonQuote(DeleteMultiFolderB)),
		MultiFolderDeleteDryRunData));
	TestEqual(TEXT("asset_delete multi folder dry run requested folder count"), MultiFolderDeleteDryRunData.IsValid() ? MultiFolderDeleteDryRunData->GetIntegerField(TEXT("requested_folder_count")) : 0, 2);
	TestEqual(TEXT("asset_delete multi folder dry run unique package count"), MultiFolderDeleteDryRunData.IsValid() ? MultiFolderDeleteDryRunData->GetIntegerField(TEXT("unique_package_count")) : 0, 2);
	TestEqual(TEXT("asset_delete multi folder dry run blocked count"), MultiFolderDeleteDryRunData.IsValid() ? MultiFolderDeleteDryRunData->GetIntegerField(TEXT("blocked_count")) : -1, 0);
	bool bSawMultiFolderInternalReferencer = false;
	const TArray<TSharedPtr<FJsonValue>>* MultiFolderAssets = nullptr;
	if (MultiFolderDeleteDryRunData.IsValid() && MultiFolderDeleteDryRunData->TryGetArrayField(TEXT("assets"), MultiFolderAssets) && MultiFolderAssets)
	{
		for (const TSharedPtr<FJsonValue>& AssetValue : *MultiFolderAssets)
		{
			const TSharedPtr<FJsonObject>* AssetObj = nullptr;
			if (AssetValue.IsValid() && AssetValue->TryGetObject(AssetObj) && AssetObj && AssetObj->IsValid())
			{
				bSawMultiFolderInternalReferencer |= (*AssetObj)->GetIntegerField(TEXT("internal_referencer_count")) > 0;
			}
		}
	}
	TestTrue(TEXT("asset_delete multi folder reports internal referencer diagnostics"), bSawMultiFolderInternalReferencer);

	TSharedPtr<FJsonObject> MultiFolderDeleteData;
	TestTrue(TEXT("asset_delete multi folder deletes resolved assets"), ExecCommand(
		TEXT("smoke-asset-delete-multi-folder-delete"),
		TEXT("asset_delete"),
		FString::Printf(TEXT("{\"folder_paths\":[%s,%s],\"recursive\":false,\"dry_run\":false,\"include_path_string_hits\":false}"), *JsonQuote(DeleteMultiFolderA), *JsonQuote(DeleteMultiFolderB)),
		MultiFolderDeleteData,
		200));
	TestEqual(TEXT("asset_delete multi folder deleted_count"), MultiFolderDeleteData.IsValid() ? MultiFolderDeleteData->GetIntegerField(TEXT("deleted_count")) : 0, 2);
	TestEqual(TEXT("asset_delete multi folder remaining_package_count"), MultiFolderDeleteData.IsValid() ? MultiFolderDeleteData->GetIntegerField(TEXT("remaining_package_count")) : 1, 0);
	TestEqual(TEXT("asset_delete multi folder folder_delete_count"), MultiFolderDeleteData.IsValid() ? MultiFolderDeleteData->GetIntegerField(TEXT("folder_delete_count")) : 0, 2);
	TestFalse(TEXT("asset_delete multi folder removes folder A"), IFileManager::Get().DirectoryExists(*FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(DeleteMultiFolderA))));
	TestFalse(TEXT("asset_delete multi folder removes folder B"), IFileManager::Get().DirectoryExists(*FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(DeleteMultiFolderB))));

	TestTrue(TEXT("asset_reference_graph dependencies"), ExecCommand(
		TEXT("smoke-refgraph-deps"),
		TEXT("asset_reference_graph"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"direction\":\"dependencies\",\"recursive\":false,\"include_searchable_names\":false,\"force_scan\":true}"), *JsonQuote(MeshPath)),
		Data));
	TestTrue(TEXT("reference graph asset_found"), Data.IsValid() && Data->GetBoolField(TEXT("asset_found")));
	TestTrue(TEXT("reference graph has dependencies"), Data.IsValid() && Data->GetIntegerField(TEXT("dependency_count")) > 0);
	TestTrue(TEXT("reference graph has edges"), Data.IsValid() && Data->GetIntegerField(TEXT("edge_count")) > 0);
	TestTrue(TEXT("reference graph has nodes"), Data.IsValid() && Data->GetIntegerField(TEXT("node_count")) >= 2);

	const FString MeshObjectPath = FString::Printf(TEXT("%s.%s"), *MeshPath, *FPackageName::GetLongPackageAssetName(MeshPath));
	TSharedPtr<FJsonObject> BothData;
	TestTrue(TEXT("asset_reference_graph object path both query"), ExecCommand(
		TEXT("smoke-refgraph-objectpath-both"),
		TEXT("asset_reference_graph"),
		FString::Printf(TEXT("{\"object_path\":%s,\"direction\":\"both\",\"recursive\":true,\"max_depth\":2,\"max_nodes\":1,\"include_path_string_hits\":true,\"max_path_string_hits\":1,\"force_scan\":true}"), *JsonQuote(MeshObjectPath)),
		BothData));
	TestEqual(TEXT("object path normalizes to package"), BothData.IsValid() ? BothData->GetStringField(TEXT("package_name")) : FString(), MeshPath);
	TestTrue(TEXT("max_nodes truncates graph"), BothData.IsValid() && BothData->GetBoolField(TEXT("truncated")));
	TestTrue(TEXT("path string hit count exists"), BothData.IsValid() && BothData->HasTypedField<EJson::Number>(TEXT("path_string_hit_count")));

	FString FirstDependencyPackage;
	const TArray<TSharedPtr<FJsonValue>>* Dependencies = nullptr;
	if (Data.IsValid() && Data->TryGetArrayField(TEXT("dependencies"), Dependencies) && Dependencies)
	{
		for (const TSharedPtr<FJsonValue>& EdgeValue : *Dependencies)
		{
			const TSharedPtr<FJsonObject>* EdgeObj = nullptr;
			if (!EdgeValue.IsValid() || !EdgeValue->TryGetObject(EdgeObj) || !EdgeObj || !EdgeObj->IsValid())
			{
				continue;
			}
			const FString Candidate = (*EdgeObj)->GetStringField(TEXT("target_package"));
			if (!Candidate.IsEmpty() && !Candidate.StartsWith(TEXT("/Script/")))
			{
				FirstDependencyPackage = Candidate;
				break;
			}
			if (FirstDependencyPackage.IsEmpty())
			{
				FirstDependencyPackage = Candidate;
			}
		}
	}
	TestTrue(TEXT("reference graph exposes dependency target"), !FirstDependencyPackage.IsEmpty());

	if (!FirstDependencyPackage.IsEmpty())
	{
		TSharedPtr<FJsonObject> RefData;
		TestTrue(TEXT("asset_reference_graph referencers"), ExecCommand(
			TEXT("smoke-refgraph-refs"),
			TEXT("asset_reference_graph"),
			FString::Printf(TEXT("{\"asset_path\":%s,\"direction\":\"referencers\",\"recursive\":false,\"include_searchable_names\":false,\"force_scan\":true}"), *JsonQuote(FirstDependencyPackage)),
			RefData));

		bool bFoundMeshAsReferencer = false;
		const TArray<TSharedPtr<FJsonValue>>* Referencers = nullptr;
		if (RefData.IsValid() && RefData->TryGetArrayField(TEXT("referencers"), Referencers) && Referencers)
		{
			for (const TSharedPtr<FJsonValue>& EdgeValue : *Referencers)
			{
				const TSharedPtr<FJsonObject>* EdgeObj = nullptr;
				if (!EdgeValue.IsValid() || !EdgeValue->TryGetObject(EdgeObj) || !EdgeObj || !EdgeObj->IsValid())
				{
					continue;
				}
				bFoundMeshAsReferencer |= (*EdgeObj)->GetStringField(TEXT("source_package")) == MeshPath;
			}
		}
		TestTrue(TEXT("referencer graph links back to duplicated mesh"), bFoundMeshAsReferencer);

		TSharedPtr<FJsonObject> BlockedDeleteData;
		TestTrue(TEXT("asset_delete blocks referenced dependency"), ExecCommand(
			TEXT("smoke-asset-delete-blocked-referenced"),
			TEXT("asset_delete"),
			FString::Printf(TEXT("{\"asset_path\":%s,\"dry_run\":true,\"allow_non_game\":true,\"include_path_string_hits\":false}"), *JsonQuote(FirstDependencyPackage)),
			BlockedDeleteData,
			400));
		TestTrue(TEXT("asset_delete reports blocked referenced dependency"), BlockedDeleteData.IsValid() && BlockedDeleteData->GetIntegerField(TEXT("blocked_count")) > 0);
	}

	TSharedPtr<FJsonObject> MissingData;
	TestTrue(TEXT("asset_reference_graph missing asset fails"), ExecCommand(
		TEXT("smoke-refgraph-missing-fail"),
		TEXT("asset_reference_graph"),
		TEXT("{\"asset_path\":\"/Game/__UeAgentInterfaceSmoke/DefinitelyMissingAsset\"}"),
		MissingData,
		400));

	TestTrue(TEXT("asset_reference_graph invalid direction fails"), ExecCommand(
		TEXT("smoke-refgraph-invalid-direction"),
		TEXT("asset_reference_graph"),
		TEXT("{\"asset_path\":\"/Game/__UeAgentInterfaceSmoke/DefinitelyMissingAsset\",\"allow_missing\":true,\"direction\":\"sideways\"}"),
		MissingData,
		400));

	TestTrue(TEXT("asset_reference_graph allow_missing succeeds"), ExecCommand(
		TEXT("smoke-refgraph-missing-allow"),
		TEXT("asset_reference_graph"),
		TEXT("{\"asset_path\":\"/Game/__UeAgentInterfaceSmoke/DefinitelyMissingAsset\",\"allow_missing\":true}"),
		MissingData));
	TestFalse(TEXT("allow_missing reports asset_found=false"), MissingData.IsValid() && MissingData->GetBoolField(TEXT("asset_found")));

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceJsonParseVisibilitySmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.JsonParseVisibilityWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceJsonParseVisibilitySmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString BlueprintAssetPath = MakeAutomationAssetPath(TEXT("BPBadJson"));
	FString RelativeFolder = BlueprintAssetPath;
	while (RelativeFolder.StartsWith(TEXT("/")))
	{
		RelativeFolder.RightChopInline(1);
	}
	const FString FolderPath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("ActorBlueprint"), RelativeFolder));
	IFileManager::Get().DeleteDirectory(*FolderPath, false, true);
	IFileManager::Get().MakeDirectory(*FPaths::Combine(FolderPath, TEXT("members")), true);

	auto SaveUtf8Json = [this](const FString& FilePath, const FString& JsonText) -> bool
	{
		const bool bOk = FFileHelper::SaveStringToFile(JsonText, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		TestTrue(FString::Printf(TEXT("Save JSON file: %s"), *FilePath), bOk);
		return bOk;
	};

	if (!SaveUtf8Json(
		FPaths::Combine(FolderPath, TEXT("asset.json")),
		FString::Printf(
			TEXT("{\n  \"format_version\": 1,\n  \"asset_kind\": \"actor_blueprint\",\n  \"asset_path\": \"%s\",\n  \"asset_name\": \"%s\",\n  \"parent_class\": \"/Script/Engine.Actor\"\n}\n"),
			*BlueprintAssetPath,
			*FPackageName::GetLongPackageAssetName(BlueprintAssetPath))))
	{
		return false;
	}

	if (!SaveUtf8Json(
		FPaths::Combine(FolderPath, TEXT("members"), TEXT("variables.json")),
		TEXT("{ \"variables\": [\n")))
	{
		return false;
	}

	const FString BadFolderApplyBody = MakeExecRequestBody(
		TEXT("smoke-json-parse-visibility-folder-bad-001"),
		TEXT("blueprint_apply_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"create_if_missing\":true,\"compile_after_apply\":false,\"save_after_apply\":false}"), *BlueprintAssetPath));
	FHttpSmokeResult BadFolderApplyResult;
	TestTrue(TEXT("Bad optional folder JSON request completed"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, BadFolderApplyBody, BadFolderApplyResult));
	TestEqual(TEXT("Bad optional folder JSON status code"), BadFolderApplyResult.StatusCode, 400);
	TestTrue(TEXT("Bad optional folder JSON response names parse failure"), BadFolderApplyResult.Body.Contains(TEXT("json_parse_failed")));
	TestTrue(TEXT("Bad optional folder JSON response names file"), BadFolderApplyResult.Body.Contains(TEXT("variables.json")));

	TSharedPtr<FJsonObject> BadFolderApplyJson;
	TestTrue(TEXT("Bad optional folder JSON response parses"), ParseJson(BadFolderApplyResult.Body, BadFolderApplyJson));
	TestTrue(TEXT("Bad optional folder JSON root ok=false"), BadFolderApplyJson.IsValid() && !BadFolderApplyJson->GetBoolField(TEXT("ok")));

	if (!SaveUtf8Json(
		FPaths::Combine(FolderPath, TEXT("members"), TEXT("variables.json")),
		TEXT("{\n  \"variables\": [\n    \"not-an-object\",\n    { \"type\": { \"category\": \"bool\" } }\n  ]\n}\n")))
	{
		return false;
	}

	const FString WarningFolderApplyBody = MakeExecRequestBody(
		TEXT("smoke-json-parse-visibility-folder-warnings-001"),
		TEXT("blueprint_apply_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"create_if_missing\":true,\"compile_after_apply\":false,\"save_after_apply\":false}"), *BlueprintAssetPath));
	FHttpSmokeResult WarningFolderApplyResult;
	TestTrue(TEXT("Recoverable bad folder JSON items request completed"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, WarningFolderApplyBody, WarningFolderApplyResult));
	TestEqual(TEXT("Recoverable bad folder JSON items status code"), WarningFolderApplyResult.StatusCode, 200);

	TSharedPtr<FJsonObject> WarningFolderApplyJson;
	TestTrue(TEXT("Recoverable bad folder JSON items response parses"), ParseJson(WarningFolderApplyResult.Body, WarningFolderApplyJson));
	TestTrue(TEXT("Recoverable bad folder JSON items root ok=true"), WarningFolderApplyJson.IsValid() && WarningFolderApplyJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* WarningData = nullptr;
	TestTrue(TEXT("Recoverable bad folder JSON items contains data"), WarningFolderApplyJson.IsValid() && WarningFolderApplyJson->TryGetObjectField(TEXT("data"), WarningData) && WarningData && WarningData->IsValid());
	if (WarningData && WarningData->IsValid())
	{
		TestTrue(TEXT("Recoverable bad folder JSON items warning_count"), (*WarningData)->GetIntegerField(TEXT("warning_count")) >= 2);
		const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
		TestTrue(TEXT("Recoverable bad folder JSON items warnings array"), (*WarningData)->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings);
		if (Warnings)
		{
			bool bFoundInvalidItem = false;
			bool bFoundMissingName = false;
			for (const TSharedPtr<FJsonValue>& WarningValue : *Warnings)
			{
				const TSharedPtr<FJsonObject>* WarningObj = nullptr;
				if (!WarningValue.IsValid() || !WarningValue->TryGetObject(WarningObj) || !WarningObj || !WarningObj->IsValid())
				{
					continue;
				}
				const FString Code = (*WarningObj)->GetStringField(TEXT("code"));
				bFoundInvalidItem |= Code == TEXT("variable_invalid_item");
				bFoundMissingName |= Code == TEXT("variable_missing_name");
			}
			TestTrue(TEXT("Recoverable bad folder JSON items includes variable_invalid_item"), bFoundInvalidItem);
			TestTrue(TEXT("Recoverable bad folder JSON items includes variable_missing_name"), bFoundMissingName);
		}
	}

	const FString InvalidPropertyBody = MakeExecRequestBody(
		TEXT("smoke-json-parse-visibility-property-invalid-001"),
		TEXT("asset_apply_property_json"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"properties\":[\"not-an-object\"],\"save_after_apply\":false}"), *BlueprintAssetPath));
	FHttpSmokeResult InvalidPropertyResult;
	TestTrue(TEXT("Invalid property JSON item request completed"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, InvalidPropertyBody, InvalidPropertyResult));
	TestEqual(TEXT("Invalid property JSON item status code"), InvalidPropertyResult.StatusCode, 400);
	TestTrue(TEXT("Invalid property JSON item response names error"), InvalidPropertyResult.Body.Contains(TEXT("invalid_property_entry")));
	TestTrue(TEXT("Invalid property JSON item response includes property_results"), InvalidPropertyResult.Body.Contains(TEXT("property_results")));

	TSharedPtr<FJsonObject> InvalidPropertyJson;
	TestTrue(TEXT("Invalid property JSON item response parses"), ParseJson(InvalidPropertyResult.Body, InvalidPropertyJson));
	TestTrue(TEXT("Invalid property JSON item root ok=false"), InvalidPropertyJson.IsValid() && !InvalidPropertyJson->GetBoolField(TEXT("ok")));

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceJsonDiagnosticsHelperSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.JsonDiagnosticsHelpers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceJsonDiagnosticsHelperSmokeTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("has_ovverride"), TEXT("true"));
	Obj->SetObjectField(TEXT("has_override"), MakeShared<FJsonObject>());
	Obj->SetStringField(TEXT("display_name"), TEXT("Sample"));
	TSharedPtr<FJsonObject> UpperVectorObj = MakeShared<FJsonObject>();
	UpperVectorObj->SetNumberField(TEXT("X"), 1.0);
	UpperVectorObj->SetNumberField(TEXT("Y"), 2.0);
	UpperVectorObj->SetNumberField(TEXT("Z"), 3.0);
	Obj->SetObjectField(TEXT("upper_vector"), UpperVectorObj);
	TSharedPtr<FJsonObject> LowerRotatorObj = MakeShared<FJsonObject>();
	LowerRotatorObj->SetNumberField(TEXT("pitch"), 10.0);
	LowerRotatorObj->SetNumberField(TEXT("yaw"), 20.0);
	LowerRotatorObj->SetNumberField(TEXT("roll"), 30.0);
	Obj->SetObjectField(TEXT("lower_rotator"), LowerRotatorObj);
	TArray<TSharedPtr<FJsonValue>> VectorArray;
	VectorArray.Add(MakeShared<FJsonValueNumber>(4.0));
	VectorArray.Add(MakeShared<FJsonValueNumber>(5.0));
	VectorArray.Add(MakeShared<FJsonValueNumber>(6.0));
	Obj->SetArrayField(TEXT("array_vector"), VectorArray);

	TArray<TSharedPtr<FJsonValue>> Issues;
	UeAgentJsonDiagnostics::WarnUnknownFields(
		Obj,
		TEXT("root"),
		{ TEXT("has_override"), TEXT("display_name"), TEXT("items"), TEXT("upper_vector"), TEXT("lower_rotator"), TEXT("array_vector") },
		Issues);

	bool bHasOverride = false;
	TestFalse(
		TEXT("JSON diagnostics bool type mismatch returns false"),
		UeAgentJsonDiagnostics::ReadBoolField(Obj, TEXT("has_override"), TEXT("root.has_override"), bHasOverride, Issues, true));

	FString MissingValue;
	TestFalse(
		TEXT("JSON diagnostics missing required field returns false"),
		UeAgentJsonDiagnostics::ReadStringField(Obj, TEXT("missing_required"), TEXT("root.missing_required"), MissingValue, Issues, true));

	FVector ParsedUpperVector = FVector::ZeroVector;
	TestTrue(
		TEXT("JSON diagnostics reads UE-style uppercase vector"),
		UeAgentJsonDiagnostics::ReadVectorField(Obj, TEXT("upper_vector"), TEXT("root.upper_vector"), ParsedUpperVector, Issues, true));
	TestEqual(TEXT("Uppercase vector X"), ParsedUpperVector.X, 1.0);
	TestEqual(TEXT("Uppercase vector Y"), ParsedUpperVector.Y, 2.0);
	TestEqual(TEXT("Uppercase vector Z"), ParsedUpperVector.Z, 3.0);

	FVector ParsedArrayVector = FVector::ZeroVector;
	TestTrue(
		TEXT("JSON diagnostics reads array vector"),
		UeAgentJsonDiagnostics::ReadVectorField(Obj, TEXT("array_vector"), TEXT("root.array_vector"), ParsedArrayVector, Issues, true));
	TestEqual(TEXT("Array vector X"), ParsedArrayVector.X, 4.0);
	TestEqual(TEXT("Array vector Y"), ParsedArrayVector.Y, 5.0);
	TestEqual(TEXT("Array vector Z"), ParsedArrayVector.Z, 6.0);

	FRotator ParsedLowerRotator = FRotator::ZeroRotator;
	TestTrue(
		TEXT("JSON diagnostics reads lowercase rotator"),
		UeAgentJsonDiagnostics::ReadRotatorField(Obj, TEXT("lower_rotator"), TEXT("root.lower_rotator"), ParsedLowerRotator, Issues, true));
	TestEqual(TEXT("Lowercase rotator Pitch"), ParsedLowerRotator.Pitch, 10.0);
	TestEqual(TEXT("Lowercase rotator Yaw"), ParsedLowerRotator.Yaw, 20.0);
	TestEqual(TEXT("Lowercase rotator Roll"), ParsedLowerRotator.Roll, 30.0);

	TSharedPtr<FJsonValue> BadArrayItem = MakeShared<FJsonValueString>(TEXT("not-an-object"));
	TSharedPtr<FJsonObject> ParsedArrayObject;
	TestFalse(
		TEXT("JSON diagnostics array object type mismatch returns false"),
		UeAgentJsonDiagnostics::ReadObjectFromValue(BadArrayItem, TEXT("root.items[0]"), ParsedArrayObject, Issues));

	bool bFoundUnknownField = false;
	bool bFoundTypeMismatch = false;
	bool bFoundMissingRequired = false;
	bool bFoundArrayItemMismatch = false;
	for (const TSharedPtr<FJsonValue>& IssueValue : Issues)
	{
		const TSharedPtr<FJsonObject>* IssueObj = nullptr;
		if (!IssueValue.IsValid() || !IssueValue->TryGetObject(IssueObj) || !IssueObj || !IssueObj->IsValid())
		{
			continue;
		}

		const FString Code = (*IssueObj)->GetStringField(TEXT("code"));
		bFoundUnknownField |= Code == TEXT("json_unknown_field");
		bFoundTypeMismatch |= Code == TEXT("json_field_type_mismatch");
		bFoundMissingRequired |= Code == TEXT("json_missing_required_field");
		bFoundArrayItemMismatch |= Code == TEXT("json_array_item_type_mismatch");
		TestTrue(TEXT("JSON diagnostics issue has severity"), (*IssueObj)->HasField(TEXT("severity")));
		TestTrue(TEXT("JSON diagnostics issue has path"), (*IssueObj)->HasField(TEXT("path")));
		TestTrue(TEXT("JSON diagnostics issue has message"), (*IssueObj)->HasField(TEXT("message")));
	}

	TestTrue(TEXT("JSON diagnostics reports unknown fields"), bFoundUnknownField);
	TestTrue(TEXT("JSON diagnostics reports type mismatch"), bFoundTypeMismatch);
	TestTrue(TEXT("JSON diagnostics reports missing required fields"), bFoundMissingRequired);
	TestTrue(TEXT("JSON diagnostics reports array item mismatch"), bFoundArrayItemMismatch);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceExecSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.ExecAndBatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceExecSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString ExecBody = MakeExecRequestBody(TEXT("smoke-exec-001"), TEXT("list_actors"), TEXT("{\"limit\":1}"));
	FHttpSmokeResult ExecResult;
	TestTrue(TEXT("Exec request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ExecBody, ExecResult));
	TestEqual(TEXT("Exec status code"), ExecResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ExecJson;
	TestTrue(TEXT("Exec response parses as JSON"), ParseJson(ExecResult.Body, ExecJson));
	TestTrue(TEXT("Exec root ok=true"), ExecJson.IsValid() && ExecJson->GetBoolField(TEXT("ok")));

	const FString BatchBody =
		TEXT("{")
		TEXT("\"request_id\":\"smoke-batch-001\",")
		TEXT("\"command\":\"exec_batch\",")
		TEXT("\"params\":{")
		TEXT("\"stop_on_error\":true,")
		TEXT("\"commands\":[")
		TEXT("{\"request_id\":\"step-1\",\"command\":\"get_world_state\",\"params\":{}},")
		TEXT("{\"request_id\":\"step-2\",\"command\":\"list_actors\",\"params\":{\"limit\":1}}")
		TEXT("]}}");

	FHttpSmokeResult BatchResult;
	TestTrue(TEXT("Batch request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, BatchBody, BatchResult));
	TestEqual(TEXT("Batch status code"), BatchResult.StatusCode, 200);

	TSharedPtr<FJsonObject> BatchJson;
	TestTrue(TEXT("Batch response parses as JSON"), ParseJson(BatchResult.Body, BatchJson));
	TestTrue(TEXT("Batch root ok=true"), BatchJson.IsValid() && BatchJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* BatchData = nullptr;
	TestTrue(TEXT("Batch contains data object"), BatchJson.IsValid() && BatchJson->TryGetObjectField(TEXT("data"), BatchData) && BatchData && BatchData->IsValid());
	if (BatchData && BatchData->IsValid())
	{
		TestEqual(TEXT("Batch mode"), (*BatchData)->GetStringField(TEXT("mode")), FString(TEXT("exec_batch")));
		TestEqual(TEXT("Batch executed count"), static_cast<int32>((*BatchData)->GetIntegerField(TEXT("executed"))), 2);
		TestEqual(TEXT("Batch succeeded count"), static_cast<int32>((*BatchData)->GetIntegerField(TEXT("succeeded"))), 2);
	}

	const FString ConsoleExecBody = MakeExecRequestBody(
		TEXT("smoke-editor-console-exec-001"),
		TEXT("editor_console_exec"),
		TEXT("{\"command\":\"r.ViewDistanceScale\",\"max_output_chars\":2048,\"max_output_lines\":16}"));
	FHttpSmokeResult ConsoleExecResult;
	TestTrue(TEXT("Editor console exec request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ConsoleExecBody, ConsoleExecResult));
	TestEqual(TEXT("Editor console exec status code"), ConsoleExecResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ConsoleExecJson;
	TestTrue(TEXT("Editor console exec response parses as JSON"), ParseJson(ConsoleExecResult.Body, ConsoleExecJson));
	TestTrue(TEXT("Editor console exec root ok=true"), ConsoleExecJson.IsValid() && ConsoleExecJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* ConsoleExecData = nullptr;
	TestTrue(TEXT("Editor console exec contains data"), ConsoleExecJson.IsValid() && ConsoleExecJson->TryGetObjectField(TEXT("data"), ConsoleExecData) && ConsoleExecData && ConsoleExecData->IsValid());
	if (ConsoleExecData && ConsoleExecData->IsValid())
	{
		TestEqual(TEXT("Editor console exec executor"), (*ConsoleExecData)->GetStringField(TEXT("executor")), FString(TEXT("Cmd")));
		TestEqual(TEXT("Editor console exec command count"), static_cast<int32>((*ConsoleExecData)->GetIntegerField(TEXT("command_count"))), 1);
		TestEqual(TEXT("Editor console exec handled count"), static_cast<int32>((*ConsoleExecData)->GetIntegerField(TEXT("handled_count"))), 1);
		const TArray<TSharedPtr<FJsonValue>>* ConsoleResults = nullptr;
		TestTrue(TEXT("Editor console exec returns results"), (*ConsoleExecData)->TryGetArrayField(TEXT("results"), ConsoleResults) && ConsoleResults && ConsoleResults->Num() == 1);
		if (ConsoleResults && ConsoleResults->Num() == 1)
		{
			const TSharedPtr<FJsonObject>* ResultObj = nullptr;
			TestTrue(TEXT("Editor console exec first result is object"), (*ConsoleResults)[0].IsValid() && (*ConsoleResults)[0]->TryGetObject(ResultObj) && ResultObj && ResultObj->IsValid());
			if (ResultObj && ResultObj->IsValid())
			{
				TestTrue(TEXT("Editor console exec first result handled"), (*ResultObj)->GetBoolField(TEXT("handled")));
				TestFalse(TEXT("Editor console exec first result not blocked"), (*ResultObj)->GetBoolField(TEXT("blocked")));
			}
		}
	}

	const FString BlockedConsoleExecBody = MakeExecRequestBody(
		TEXT("smoke-editor-console-exec-blocked-001"),
		TEXT("editor_console_exec"),
		TEXT("{\"command\":\"QUIT_EDITOR\"}"));
	FHttpSmokeResult BlockedConsoleExecResult;
	TestTrue(TEXT("Blocked editor console exec request completed"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, BlockedConsoleExecBody, BlockedConsoleExecResult));
	TestEqual(TEXT("Blocked editor console exec status code"), BlockedConsoleExecResult.StatusCode, 400);

	TSharedPtr<FJsonObject> BlockedConsoleExecJson;
	TestTrue(TEXT("Blocked editor console exec response parses as JSON"), ParseJson(BlockedConsoleExecResult.Body, BlockedConsoleExecJson));
	TestTrue(TEXT("Blocked editor console exec root ok=false"), BlockedConsoleExecJson.IsValid() && !BlockedConsoleExecJson->GetBoolField(TEXT("ok")));
	TestEqual(TEXT("Blocked editor console exec error code"), BlockedConsoleExecJson.IsValid() ? BlockedConsoleExecJson->GetStringField(TEXT("error")) : FString(), FString(TEXT("editor_console_command_blocked_high_risk")));

	const TSharedPtr<FJsonObject>* BlockedConsoleExecData = nullptr;
	TestTrue(TEXT("Blocked editor console exec contains data"), BlockedConsoleExecJson.IsValid() && BlockedConsoleExecJson->TryGetObjectField(TEXT("data"), BlockedConsoleExecData) && BlockedConsoleExecData && BlockedConsoleExecData->IsValid());
	if (BlockedConsoleExecData && BlockedConsoleExecData->IsValid())
	{
		TestEqual(TEXT("Blocked editor console exec blocked count"), static_cast<int32>((*BlockedConsoleExecData)->GetIntegerField(TEXT("blocked_count"))), 1);
		TestEqual(TEXT("Blocked editor console exec executed count"), static_cast<int32>((*BlockedConsoleExecData)->GetIntegerField(TEXT("executed_count"))), 0);
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceEditorDirtyResolveAndCloseSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.EditorDirtyResolveAndClose",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceEditorDirtyResolveAndCloseSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	{
		const FString PreDirtyListBody = MakeExecRequestBody(TEXT("smoke-editor-preclean-list-001"), TEXT("editor_list_dirty_resources"));
		FHttpSmokeResult PreDirtyListResult;
		TestTrue(TEXT("Pre-clean dirty resource list request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, PreDirtyListBody, PreDirtyListResult));
		TestEqual(TEXT("Pre-clean dirty resource list status code"), PreDirtyListResult.StatusCode, 200);

		TSharedPtr<FJsonObject> PreDirtyListJson;
		TestTrue(TEXT("Pre-clean dirty resource list parses as JSON"), ParseJson(PreDirtyListResult.Body, PreDirtyListJson));
		TestTrue(TEXT("Pre-clean dirty resource list root ok=true"), PreDirtyListJson.IsValid() && PreDirtyListJson->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* PreDirtyListData = nullptr;
		TestTrue(TEXT("Pre-clean dirty resource list contains data"), PreDirtyListJson.IsValid() && PreDirtyListJson->TryGetObjectField(TEXT("data"), PreDirtyListData) && PreDirtyListData && PreDirtyListData->IsValid());
		const int32 PreDirtyCount = (PreDirtyListData && PreDirtyListData->IsValid()) ? static_cast<int32>((*PreDirtyListData)->GetIntegerField(TEXT("dirty_resource_count"))) : 0;
		if (PreDirtyCount > 0)
		{
			const FString PreResolveBody = MakeExecRequestBody(
				TEXT("smoke-editor-preclean-resolve-001"),
				TEXT("editor_resolve_dirty_resources"),
				TEXT("{\"discard_all\":true,\"close_all_asset_editors\":true}"));
			FHttpSmokeResult PreResolveResult;
			TestTrue(TEXT("Pre-clean resolve dirty resources request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, PreResolveBody, PreResolveResult));
			TestEqual(TEXT("Pre-clean resolve dirty resources status code"), PreResolveResult.StatusCode, 200);

			TSharedPtr<FJsonObject> PreResolveJson;
			TestTrue(TEXT("Pre-clean resolve dirty resources parses as JSON"), ParseJson(PreResolveResult.Body, PreResolveJson));
			TestTrue(TEXT("Pre-clean resolve dirty resources root ok=true"), PreResolveJson.IsValid() && PreResolveJson->GetBoolField(TEXT("ok")));
		}
	}

	const FString UniqueSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ActorLabel = FString::Printf(TEXT("SmokeEditorDirty_%s"), *UniqueSuffix);

	const FString SpawnBody = MakeExecRequestBody(
		TEXT("smoke-editor-dirty-spawn-001"),
		TEXT("spawn_actor"),
		FString::Printf(
			TEXT("{\"class_path\":\"/Script/Engine.StaticMeshActor\",\"label\":\"%s\",\"static_mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":{\"x\":-1200.0,\"y\":-300.0,\"z\":120.0}}"),
			*ActorLabel));
	FHttpSmokeResult SpawnResult;
	TestTrue(TEXT("Spawn dirty actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnBody, SpawnResult));
	TestEqual(TEXT("Spawn dirty actor status code"), SpawnResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SpawnJson;
	TestTrue(TEXT("Spawn dirty actor response parses as JSON"), ParseJson(SpawnResult.Body, SpawnJson));
	TestTrue(TEXT("Spawn dirty actor root ok=true"), SpawnJson.IsValid() && SpawnJson->GetBoolField(TEXT("ok")));

	const FString DirtyListBody = MakeExecRequestBody(TEXT("smoke-editor-dirty-list-001"), TEXT("editor_list_dirty_resources"));
	FHttpSmokeResult DirtyListResult;
	TestTrue(TEXT("Dirty resource list request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DirtyListBody, DirtyListResult));
	TestEqual(TEXT("Dirty resource list status code"), DirtyListResult.StatusCode, 200);

	TSharedPtr<FJsonObject> DirtyListJson;
	TestTrue(TEXT("Dirty resource list parses as JSON"), ParseJson(DirtyListResult.Body, DirtyListJson));
	TestTrue(TEXT("Dirty resource list root ok=true"), DirtyListJson.IsValid() && DirtyListJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* DirtyListData = nullptr;
	TestTrue(TEXT("Dirty resource list contains data"), DirtyListJson.IsValid() && DirtyListJson->TryGetObjectField(TEXT("data"), DirtyListData) && DirtyListData && DirtyListData->IsValid());
	if (DirtyListData && DirtyListData->IsValid())
	{
		TestTrue(TEXT("Dirty resource count is positive"), static_cast<int32>((*DirtyListData)->GetIntegerField(TEXT("dirty_resource_count"))) > 0);
	}

	const FString CloseBody = MakeExecRequestBody(
		TEXT("smoke-editor-close-001"),
		TEXT("editor_close"),
		TEXT("{\"request_exit\":false,\"close_all_asset_editors\":false}"));
	FHttpSmokeResult CloseResult;
	TestTrue(TEXT("Editor close request completed"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CloseBody, CloseResult));
	TestEqual(TEXT("Editor close blocked status code"), CloseResult.StatusCode, 400);

	TSharedPtr<FJsonObject> CloseJson;
	TestTrue(TEXT("Editor close blocked parses as JSON"), ParseJson(CloseResult.Body, CloseJson));
	TestTrue(TEXT("Editor close blocked root ok=false"), CloseJson.IsValid() && !CloseJson->GetBoolField(TEXT("ok")));
	TestEqual(TEXT("Editor close blocked error code"), CloseJson.IsValid() ? CloseJson->GetStringField(TEXT("error")) : FString(), FString(TEXT("editor_has_unresolved_dirty_resources")));

	const TSharedPtr<FJsonObject>* CloseData = nullptr;
	TestTrue(TEXT("Editor close blocked contains data"), CloseJson.IsValid() && CloseJson->TryGetObjectField(TEXT("data"), CloseData) && CloseData && CloseData->IsValid());
	if (CloseData && CloseData->IsValid())
	{
		TestTrue(TEXT("Editor close blocked returns dirty list"), static_cast<int32>((*CloseData)->GetIntegerField(TEXT("dirty_resource_count"))) > 0);
	}

	const FString ResolveBody = MakeExecRequestBody(
		TEXT("smoke-editor-resolve-001"),
		TEXT("editor_resolve_dirty_resources"),
		TEXT("{\"discard_current_level\":true,\"discard_all\":true}"));
	FHttpSmokeResult ResolveResult;
	TestTrue(TEXT("Editor resolve dirty resources request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ResolveBody, ResolveResult));
	TestEqual(TEXT("Editor resolve dirty resources status code"), ResolveResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ResolveJson;
	TestTrue(TEXT("Editor resolve dirty resources parses as JSON"), ParseJson(ResolveResult.Body, ResolveJson));
	TestTrue(TEXT("Editor resolve dirty resources root ok=true"), ResolveJson.IsValid() && ResolveJson->GetBoolField(TEXT("ok")));

	const FString CloseAfterResolveBody = MakeExecRequestBody(
		TEXT("smoke-editor-close-002"),
		TEXT("editor_close"),
		TEXT("{\"request_exit\":false,\"close_all_asset_editors\":false}"));
	FHttpSmokeResult CloseAfterResolveResult;
	TestTrue(TEXT("Editor close after resolve request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CloseAfterResolveBody, CloseAfterResolveResult));
	TestEqual(TEXT("Editor close after resolve status code"), CloseAfterResolveResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CloseAfterResolveJson;
	TestTrue(TEXT("Editor close after resolve parses as JSON"), ParseJson(CloseAfterResolveResult.Body, CloseAfterResolveJson));
	TestTrue(TEXT("Editor close after resolve root ok=true"), CloseAfterResolveJson.IsValid() && CloseAfterResolveJson->GetBoolField(TEXT("ok")));

	return !HasAnyErrors();
}

#endif
