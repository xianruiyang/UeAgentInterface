#if WITH_DEV_AUTOMATION_TESTS

#include "Utilities/UeAgentLevelFolderExport.h"

#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Server/UeAgentCommandRegistry.h"
#include "Server/UeAgentHttpServer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	bool LoadLevelExportJsonObject(const FString& FilePath, TSharedPtr<FJsonObject>& OutObject)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *FilePath))
		{
			return false;
		}
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
	}

	bool HasReadableDetailsGroup(const FString& DetailsRoot)
	{
		TSharedPtr<FJsonObject> Index;
		if (!LoadLevelExportJsonObject(DetailsRoot / TEXT("index.json"), Index))
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Groups = nullptr;
		if (!Index->TryGetArrayField(TEXT("groups"), Groups) || !Groups || Groups->Num() == 0)
		{
			return false;
		}

		const TSharedPtr<FJsonObject> FirstGroup = (*Groups)[0].IsValid() ? (*Groups)[0]->AsObject() : nullptr;
		if (!FirstGroup.IsValid())
		{
			return false;
		}

		FString GroupFile;
		return FirstGroup->TryGetStringField(TEXT("file"), GroupFile)
			&& !GroupFile.IsEmpty()
			&& IFileManager::Get().FileExists(*(DetailsRoot / GroupFile));
	}

	void TestGroupedDetailsRef(
		FAutomationTestBase& Test,
		const FString& Label,
		const TSharedPtr<FJsonObject>& Container,
		const FString& OwnerKind,
		const FString& OwnerId,
		const FString& DetailsRoot)
	{
		const TSharedPtr<FJsonObject>* Details = nullptr;
		Test.TestTrue(Label + TEXT(" has details ref"), Container.IsValid() && Container->TryGetObjectField(TEXT("details"), Details) && Details && Details->IsValid());
		if (!Details || !Details->IsValid())
		{
			return;
		}

		Test.TestEqual(Label + TEXT(" details schema"), (*Details)->GetStringField(TEXT("schema")), FString(TEXT("uai.details_ref.v1")));
		Test.TestEqual(Label + TEXT(" details index file"), (*Details)->GetStringField(TEXT("index_file")), FString(TEXT("details/index.json")));
		Test.TestEqual(Label + TEXT(" details ref field"), (*Details)->GetStringField(TEXT("ref_field")), FString(TEXT("details")));
		Test.TestEqual(Label + TEXT(" details owner kind"), (*Details)->GetStringField(TEXT("owner_kind")), OwnerKind);
		Test.TestEqual(Label + TEXT(" details owner id"), (*Details)->GetStringField(TEXT("owner_id")), OwnerId);
		Test.TestEqual(Label + TEXT(" details status"), (*Details)->GetStringField(TEXT("status")), FString(TEXT("complete")));
		Test.TestTrue(Label + TEXT(" details group count"), (*Details)->GetNumberField(TEXT("group_count")) > 0.0);
		Test.TestTrue(Label + TEXT(" details field count"), (*Details)->GetNumberField(TEXT("field_count")) > 0.0);
		Test.TestTrue(Label + TEXT(" details editable count present"), (*Details)->HasField(TEXT("editable_field_count")));
		Test.TestFalse(Label + TEXT(" details fields are not inlined"), (*Details)->GetBoolField(TEXT("field_details_inlined")));
		Test.TestTrue(Label + TEXT(" details index exists"), IFileManager::Get().FileExists(*(DetailsRoot / TEXT("index.json"))));
		Test.TestTrue(Label + TEXT(" details group readable"), HasReadableDetailsGroup(DetailsRoot));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentLevelFolderExportSkeletonTest,
	"GptProjectTest.UeAgentInterface.Smoke.LevelFolderExport.Skeleton",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentLevelFolderExportSkeletonTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, FName(TEXT("UAI_LevelFolderExportSmokeWorld")));
	TestNotNull(TEXT("world created"), World);
	if (!World)
	{
		return false;
	}
	AActor* RootActor = World->SpawnActor<AActor>();
	TestNotNull(TEXT("root actor spawned"), RootActor);
	if (RootActor)
	{
		RootActor->SetActorLabel(TEXT("UAI_RootActor"));
		USceneComponent* RootSceneComponent = NewObject<USceneComponent>(RootActor, TEXT("UAI_RootSceneComponent"));
		TestNotNull(TEXT("root scene component created"), RootSceneComponent);
		if (RootSceneComponent)
		{
			RootActor->AddInstanceComponent(RootSceneComponent);
			RootActor->SetRootComponent(RootSceneComponent);
			RootSceneComponent->RegisterComponentWithWorld(World);
		}
	}
	AActor* FolderActor = World->SpawnActor<AActor>();
	TestNotNull(TEXT("folder actor spawned"), FolderActor);
	if (FolderActor)
	{
		FolderActor->SetActorLabel(TEXT("UAI_FolderActor"));
		FolderActor->SetFolderPath(FName(TEXT("Gameplay/Entrances")));
	}

	const FString OutputFolder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation") / TEXT("UAI_LevelFolderExportSmoke"));
	IFileManager::Get().DeleteDirectory(*OutputFolder, false, true);

	UeAgentLevelFolderExport::FExportOptions Options;
	Options.OutputFolder = OutputFolder;
	Options.bOverwrite = true;

	TSharedPtr<FJsonObject> Summary;
	FString Error;
	const bool bExported = UeAgentLevelFolderExport::ExportLevelFolder(World, Options, Summary, Error);

	TestTrue(TEXT("export succeeds"), bExported);
	TestTrue(TEXT("summary valid"), Summary.IsValid());
	if (Summary.IsValid())
	{
		TestEqual(TEXT("summary schema"), Summary->GetStringField(TEXT("schema")), FString(TEXT("uai.level_folder.v1")));
		TestEqual(TEXT("folder path"), Summary->GetStringField(TEXT("folder_path")), OutputFolder);
		TestEqual(TEXT("default profile"), Summary->GetStringField(TEXT("profile")), FString(TEXT("llm_authoring")));
		TestTrue(TEXT("files written includes compact level and actor/component packages"), static_cast<int32>(Summary->GetNumberField(TEXT("files_written"))) >= 14);
		TestTrue(TEXT("directories created includes actor/component packages"), static_cast<int32>(Summary->GetNumberField(TEXT("directories_created"))) >= 7);
	}

	const TArray<FString> ExpectedFiles = {
		TEXT("README.md"),
		TEXT("manifest.json"),
		TEXT("index.json"),
		TEXT("level.json"),
		TEXT("world.json"),
		TEXT("world_settings/summary.json"),
		TEXT("world_settings/details/index.json"),
		TEXT("streaming/index.json"),
		TEXT("data_layers.json"),
		TEXT("world_partition.json"),
		TEXT("hlod.json"),
		TEXT("references/assets.json"),
		TEXT("external_assets.json"),
		TEXT("actors/folders.json"),
		TEXT("actors/index.json")
	};
	for (const FString& File : ExpectedFiles)
	{
		TestTrue(FString::Printf(TEXT("file exists: %s"), *File), IFileManager::Get().FileExists(*(OutputFolder / File)));
	}

	const TArray<FString> ExpectedDirectories = {
		TEXT("streaming"),
		TEXT("world_settings"),
		TEXT("world_settings/details"),
		TEXT("actors"),
		TEXT("actors/items"),
		TEXT("references")
	};
	for (const FString& Directory : ExpectedDirectories)
	{
		TestTrue(FString::Printf(TEXT("directory exists: %s"), *Directory), IFileManager::Get().DirectoryExists(*(OutputFolder / Directory)));
	}

	FString ManifestText;
	TestTrue(TEXT("manifest readable"), FFileHelper::LoadFileToString(ManifestText, *(OutputFolder / TEXT("manifest.json"))));
	TSharedPtr<FJsonObject> Manifest;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ManifestText);
	TestTrue(TEXT("manifest parseable"), FJsonSerializer::Deserialize(Reader, Manifest) && Manifest.IsValid());
	if (Manifest.IsValid())
	{
		TestEqual(TEXT("manifest schema"), Manifest->GetStringField(TEXT("schema")), FString(TEXT("uai.level_folder.v1")));
		TestEqual(TEXT("manifest profile"), Manifest->GetStringField(TEXT("profile")), FString(TEXT("llm_authoring")));
	}

	TestFalse(TEXT("default export excludes feature gates"), IFileManager::Get().FileExists(*(OutputFolder / TEXT("feature_gates.json"))));
	TestFalse(TEXT("default export excludes diagnostics"), IFileManager::Get().FileExists(*(OutputFolder / TEXT("diagnostics.json"))));
	TestTrue(TEXT("default export includes world settings package"), IFileManager::Get().DirectoryExists(*(OutputFolder / TEXT("world_settings"))));
	TestTrue(TEXT("default world settings details index exists"), IFileManager::Get().FileExists(*(OutputFolder / TEXT("world_settings") / TEXT("details") / TEXT("index.json"))));
	TestFalse(TEXT("default world settings omits split properties.json"), IFileManager::Get().FileExists(*(OutputFolder / TEXT("world_settings") / TEXT("properties.json"))));
	TestFalse(TEXT("default export excludes editor summary package"), IFileManager::Get().DirectoryExists(*(OutputFolder / TEXT("editor"))));
	TestFalse(TEXT("default export excludes level blueprint package"), IFileManager::Get().DirectoryExists(*(OutputFolder / TEXT("level_blueprint"))));

	TSharedPtr<FJsonObject> WorldSettingsSummary;
	TestTrue(TEXT("world settings summary readable"), LoadLevelExportJsonObject(OutputFolder / TEXT("world_settings") / TEXT("summary.json"), WorldSettingsSummary));
	if (WorldSettingsSummary.IsValid())
	{
		TestEqual(TEXT("world settings summary kind"), WorldSettingsSummary->GetStringField(TEXT("kind")), FString(TEXT("world_settings")));
		TestFalse(TEXT("world settings summary omits inline properties"), WorldSettingsSummary->HasField(TEXT("properties")));
		TestFalse(TEXT("world settings summary omits properties file"), WorldSettingsSummary->HasField(TEXT("properties_file")));
		TestGroupedDetailsRef(*this, TEXT("world settings"), WorldSettingsSummary, TEXT("world_settings"), TEXT("world_settings"), OutputFolder / TEXT("world_settings") / TEXT("details"));
	}

	FString RootIndexText;
	TestTrue(TEXT("root index readable"), FFileHelper::LoadFileToString(RootIndexText, *(OutputFolder / TEXT("index.json"))));
	TSharedPtr<FJsonObject> RootIndexJson;
	const TSharedRef<TJsonReader<>> RootIndexReader = TJsonReaderFactory<>::Create(RootIndexText);
	TestTrue(TEXT("root index parseable"), FJsonSerializer::Deserialize(RootIndexReader, RootIndexJson) && RootIndexJson.IsValid());
	if (RootIndexJson.IsValid())
	{
		TestEqual(TEXT("root index kind"), RootIndexJson->GetStringField(TEXT("kind")), FString(TEXT("level_index")));
		TestTrue(TEXT("root index has sections"), RootIndexJson->HasField(TEXT("sections")));
	}

	FString StreamingIndexText;
	TestTrue(TEXT("streaming index readable"), FFileHelper::LoadFileToString(StreamingIndexText, *(OutputFolder / TEXT("streaming") / TEXT("index.json"))));
	TSharedPtr<FJsonObject> StreamingIndexJson;
	const TSharedRef<TJsonReader<>> StreamingIndexReader = TJsonReaderFactory<>::Create(StreamingIndexText);
	TestTrue(TEXT("streaming index parseable"), FJsonSerializer::Deserialize(StreamingIndexReader, StreamingIndexJson) && StreamingIndexJson.IsValid());
	if (StreamingIndexJson.IsValid())
	{
		TestEqual(TEXT("streaming index kind"), StreamingIndexJson->GetStringField(TEXT("kind")), FString(TEXT("streaming_index")));
		TestFalse(TEXT("streaming child actors not inlined"), StreamingIndexJson->GetBoolField(TEXT("child_level_actors_inlined")));
	}

	FString DataLayersText;
	TestTrue(TEXT("data layers readable"), FFileHelper::LoadFileToString(DataLayersText, *(OutputFolder / TEXT("data_layers.json"))));
	TSharedPtr<FJsonObject> DataLayersJson;
	const TSharedRef<TJsonReader<>> DataLayersReader = TJsonReaderFactory<>::Create(DataLayersText);
	TestTrue(TEXT("data layers parseable"), FJsonSerializer::Deserialize(DataLayersReader, DataLayersJson) && DataLayersJson.IsValid());
	if (DataLayersJson.IsValid())
	{
		TestEqual(TEXT("data layers kind"), DataLayersJson->GetStringField(TEXT("kind")), FString(TEXT("data_layers")));
		TestFalse(TEXT("data layer deep items not inlined"), DataLayersJson->GetBoolField(TEXT("deep_layer_items_exported")));
	}

	FString WorldPartitionText;
	TestTrue(TEXT("world partition readable"), FFileHelper::LoadFileToString(WorldPartitionText, *(OutputFolder / TEXT("world_partition.json"))));
	TSharedPtr<FJsonObject> WorldPartitionJson;
	const TSharedRef<TJsonReader<>> WorldPartitionReader = TJsonReaderFactory<>::Create(WorldPartitionText);
	TestTrue(TEXT("world partition parseable"), FJsonSerializer::Deserialize(WorldPartitionReader, WorldPartitionJson) && WorldPartitionJson.IsValid());
	if (WorldPartitionJson.IsValid())
	{
		TestEqual(TEXT("world partition kind"), WorldPartitionJson->GetStringField(TEXT("kind")), FString(TEXT("world_partition")));
		TestFalse(TEXT("world partition runtime cells not inlined"), WorldPartitionJson->GetBoolField(TEXT("runtime_cells_inlined")));
	}

	FString HlodText;
	TestTrue(TEXT("hlod readable"), FFileHelper::LoadFileToString(HlodText, *(OutputFolder / TEXT("hlod.json"))));
	TSharedPtr<FJsonObject> HlodJson;
	const TSharedRef<TJsonReader<>> HlodReader = TJsonReaderFactory<>::Create(HlodText);
	TestTrue(TEXT("hlod parseable"), FJsonSerializer::Deserialize(HlodReader, HlodJson) && HlodJson.IsValid());
	if (HlodJson.IsValid())
	{
		TestEqual(TEXT("hlod kind"), HlodJson->GetStringField(TEXT("kind")), FString(TEXT("hlod")));
		TestFalse(TEXT("hlod proxy mesh not inlined"), HlodJson->GetBoolField(TEXT("proxy_mesh_inlined")));
	}

	FString ReferencesText;
	TestTrue(TEXT("references readable"), FFileHelper::LoadFileToString(ReferencesText, *(OutputFolder / TEXT("references") / TEXT("assets.json"))));
	TSharedPtr<FJsonObject> ReferencesJson;
	const TSharedRef<TJsonReader<>> ReferencesReader = TJsonReaderFactory<>::Create(ReferencesText);
	TestTrue(TEXT("references parseable"), FJsonSerializer::Deserialize(ReferencesReader, ReferencesJson) && ReferencesJson.IsValid());
	if (ReferencesJson.IsValid())
	{
		TestEqual(TEXT("references kind"), ReferencesJson->GetStringField(TEXT("kind")), FString(TEXT("asset_references")));
		TestFalse(TEXT("reference target assets not inlined"), ReferencesJson->GetBoolField(TEXT("target_assets_inlined")));
		TestTrue(TEXT("references has count"), ReferencesJson->HasField(TEXT("reference_count")));
	}

	FString ExternalAssetsText;
	TestTrue(TEXT("external assets readable"), FFileHelper::LoadFileToString(ExternalAssetsText, *(OutputFolder / TEXT("external_assets.json"))));
	TSharedPtr<FJsonObject> ExternalAssetsJson;
	const TSharedRef<TJsonReader<>> ExternalAssetsReader = TJsonReaderFactory<>::Create(ExternalAssetsText);
	TestTrue(TEXT("external assets parseable"), FJsonSerializer::Deserialize(ExternalAssetsReader, ExternalAssetsJson) && ExternalAssetsJson.IsValid());
	if (ExternalAssetsJson.IsValid())
	{
		TestEqual(TEXT("external assets kind"), ExternalAssetsJson->GetStringField(TEXT("kind")), FString(TEXT("external_assets")));
		TestFalse(TEXT("external assets not inlined"), ExternalAssetsJson->GetBoolField(TEXT("target_assets_inlined")));
		TestTrue(TEXT("external assets has count"), ExternalAssetsJson->HasField(TEXT("asset_count")));
	}

	FString ActorIndexText;
	TestTrue(TEXT("actor index readable"), FFileHelper::LoadFileToString(ActorIndexText, *(OutputFolder / TEXT("actors") / TEXT("index.json"))));
	TSharedPtr<FJsonObject> ActorIndex;
	const TSharedRef<TJsonReader<>> ActorIndexReader = TJsonReaderFactory<>::Create(ActorIndexText);
	TestTrue(TEXT("actor index parseable"), FJsonSerializer::Deserialize(ActorIndexReader, ActorIndex) && ActorIndex.IsValid());
	if (ActorIndex.IsValid())
	{
		TestEqual(TEXT("actor index schema"), ActorIndex->GetStringField(TEXT("schema")), FString(TEXT("uai.level_folder.v1")));
		TestEqual(TEXT("actor index kind"), ActorIndex->GetStringField(TEXT("kind")), FString(TEXT("actor_index")));
		TestTrue(TEXT("actor index includes test actors"), static_cast<int32>(ActorIndex->GetNumberField(TEXT("actor_count"))) >= 2);

		const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
		TestTrue(TEXT("actor index actors array"), ActorIndex->TryGetArrayField(TEXT("actors"), Actors) && Actors);
		bool bHasRootActor = false;
		bool bHasFolderActor = false;
		FString RootActorId;
		FString FolderActorId;
		if (Actors)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Actors)
			{
				const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr;
				if (!Item.IsValid())
				{
					continue;
				}
				if (Item->GetStringField(TEXT("label")) == TEXT("UAI_RootActor") && Item->GetStringField(TEXT("folder_key")) == TEXT("__root__"))
				{
					bHasRootActor = true;
					RootActorId = Item->GetStringField(TEXT("actor_id"));
				}
				if (Item->GetStringField(TEXT("label")) == TEXT("UAI_FolderActor") && Item->GetStringField(TEXT("folder_path")) == TEXT("Gameplay/Entrances"))
				{
					bHasFolderActor = true;
					FolderActorId = Item->GetStringField(TEXT("actor_id"));
				}
			}
		}
		TestTrue(TEXT("root/no-folder actor indexed"), bHasRootActor);
		TestTrue(TEXT("folder actor indexed"), bHasFolderActor);
		TestFalse(TEXT("root actor id available"), RootActorId.IsEmpty());
		TestFalse(TEXT("folder actor id available"), FolderActorId.IsEmpty());
		if (!RootActorId.IsEmpty())
		{
			const FString RootActorPackage = OutputFolder / TEXT("actors") / TEXT("items") / RootActorId;
			TestTrue(TEXT("root actor package actor.json exists"), IFileManager::Get().FileExists(*(RootActorPackage / TEXT("actor.json"))));
			TestFalse(TEXT("root actor package omits default split properties.json"), IFileManager::Get().FileExists(*(RootActorPackage / TEXT("properties.json"))));

			FString RootActorJsonText;
			TestTrue(TEXT("root actor json readable"), FFileHelper::LoadFileToString(RootActorJsonText, *(RootActorPackage / TEXT("actor.json"))));
			TestTrue(TEXT("root actor location is inline structured json"), RootActorJsonText.Contains(TEXT("\"location\": {\"x\":")));
			TestFalse(TEXT("root actor location is not pretty-expanded"), RootActorJsonText.Contains(TEXT("\"location\":\n")));
			TestTrue(TEXT("root actor rotation is inline structured json"), RootActorJsonText.Contains(TEXT("\"rotation\": {\"pitch\":")));
			TestFalse(TEXT("root actor rotation is not pretty-expanded"), RootActorJsonText.Contains(TEXT("\"rotation\":\n")));
			TSharedPtr<FJsonObject> RootActorJson;
			const TSharedRef<TJsonReader<>> RootActorReader = TJsonReaderFactory<>::Create(RootActorJsonText);
			TestTrue(TEXT("root actor json parseable"), FJsonSerializer::Deserialize(RootActorReader, RootActorJson) && RootActorJson.IsValid());
			if (RootActorJson.IsValid())
			{
				TestEqual(TEXT("root actor json kind"), RootActorJson->GetStringField(TEXT("kind")), FString(TEXT("actor")));
				TestTrue(TEXT("root actor json has path"), RootActorJson->HasField(TEXT("path")));
				TestFalse(TEXT("root actor json omits legacy class_path"), RootActorJson->HasField(TEXT("class_path")));
				TestFalse(TEXT("root actor json omits legacy object_path"), RootActorJson->HasField(TEXT("object_path")));
				TestTrue(TEXT("root actor json has transform"), RootActorJson->HasField(TEXT("transform")));
				const TSharedPtr<FJsonObject>* RootTransform = nullptr;
				const TSharedPtr<FJsonObject>* RootTransformSpaceContract = nullptr;
				TestTrue(TEXT("root actor transform has space contract"),
					RootActorJson->TryGetObjectField(TEXT("transform"), RootTransform)
					&& RootTransform
					&& RootTransform->IsValid()
					&& (*RootTransform)->TryGetObjectField(TEXT("space_contract"), RootTransformSpaceContract)
					&& RootTransformSpaceContract
					&& RootTransformSpaceContract->IsValid());
				if (RootTransformSpaceContract && RootTransformSpaceContract->IsValid())
				{
					TestEqual(TEXT("root actor transform read frame"), (*RootTransformSpaceContract)->GetStringField(TEXT("Fread")), FString(TEXT("ue_actor_world_transform")));
				}
				TestFalse(TEXT("root actor json omits inline properties"), RootActorJson->HasField(TEXT("properties")));
				TestFalse(TEXT("root actor json omits properties file"), RootActorJson->HasField(TEXT("properties_file")));
				TestTrue(TEXT("root actor json has details ref"), RootActorJson->HasField(TEXT("details")));
				TestTrue(TEXT("root actor details index exists"), IFileManager::Get().FileExists(*(RootActorPackage / TEXT("details") / TEXT("index.json"))));
				TestGroupedDetailsRef(*this, TEXT("root actor"), RootActorJson, TEXT("actor"), RootActorId, RootActorPackage / TEXT("details"));
				TestTrue(TEXT("root actor has component count"), static_cast<int32>(RootActorJson->GetNumberField(TEXT("component_count"))) >= 1);

				const TArray<TSharedPtr<FJsonValue>>* ComponentIds = nullptr;
				TestTrue(TEXT("root actor component ids array"), RootActorJson->TryGetArrayField(TEXT("component_ids"), ComponentIds) && ComponentIds);
				FString FirstComponentId;
				if (ComponentIds && ComponentIds->Num() > 0 && (*ComponentIds)[0].IsValid())
				{
					FirstComponentId = (*ComponentIds)[0]->AsString();
				}
				TestFalse(TEXT("root actor first component id available"), FirstComponentId.IsEmpty());
				if (!FirstComponentId.IsEmpty())
				{
					const FString ComponentPackage = RootActorPackage / TEXT("components") / FirstComponentId;
					TestTrue(TEXT("root component package component.json exists"), IFileManager::Get().FileExists(*(ComponentPackage / TEXT("component.json"))));
					TestFalse(TEXT("root component package omits default split properties.json"), IFileManager::Get().FileExists(*(ComponentPackage / TEXT("properties.json"))));

					FString ComponentJsonText;
					TestTrue(TEXT("root component json readable"), FFileHelper::LoadFileToString(ComponentJsonText, *(ComponentPackage / TEXT("component.json"))));
					TestTrue(TEXT("root component attachment location is inline structured json"), ComponentJsonText.Contains(TEXT("\"relative_location\": {\"x\":")));
					TestFalse(TEXT("root component attachment location is not pretty-expanded"), ComponentJsonText.Contains(TEXT("\"relative_location\":\n")));
					TSharedPtr<FJsonObject> ComponentJson;
					const TSharedRef<TJsonReader<>> ComponentReader = TJsonReaderFactory<>::Create(ComponentJsonText);
					TestTrue(TEXT("root component json parseable"), FJsonSerializer::Deserialize(ComponentReader, ComponentJson) && ComponentJson.IsValid());
					if (ComponentJson.IsValid())
					{
						TestEqual(TEXT("root component json kind"), ComponentJson->GetStringField(TEXT("kind")), FString(TEXT("component")));
						TestEqual(TEXT("root component owner actor id"), ComponentJson->GetStringField(TEXT("owner_actor_id")), RootActorId);
						TestTrue(TEXT("root component json has path"), ComponentJson->HasField(TEXT("path")));
						TestTrue(TEXT("root component json has attachment"), ComponentJson->HasField(TEXT("attachment")));
						const TSharedPtr<FJsonObject>* Attachment = nullptr;
						const TSharedPtr<FJsonObject>* AttachmentSpaceContract = nullptr;
						TestTrue(TEXT("root component attachment has space contract"),
							ComponentJson->TryGetObjectField(TEXT("attachment"), Attachment)
							&& Attachment
							&& Attachment->IsValid()
							&& (*Attachment)->TryGetObjectField(TEXT("space_contract"), AttachmentSpaceContract)
							&& AttachmentSpaceContract
							&& AttachmentSpaceContract->IsValid());
						if (AttachmentSpaceContract && AttachmentSpaceContract->IsValid())
						{
							TestEqual(TEXT("root component attachment read frame"), (*AttachmentSpaceContract)->GetStringField(TEXT("Fread")), FString(TEXT("ue_scene_component_parent_relative_transform")));
						}
						TestFalse(TEXT("root component json omits inline properties"), ComponentJson->HasField(TEXT("properties")));
						TestFalse(TEXT("root component json omits properties file"), ComponentJson->HasField(TEXT("properties_file")));
						TestTrue(TEXT("root component json has details ref"), ComponentJson->HasField(TEXT("details")));
						TestTrue(TEXT("root component details index exists"), IFileManager::Get().FileExists(*(ComponentPackage / TEXT("details") / TEXT("index.json"))));
						TestGroupedDetailsRef(*this, TEXT("root component"), ComponentJson, TEXT("component"), FirstComponentId, ComponentPackage / TEXT("details"));
					}
				}
			}
		}
	}

	FString FoldersText;
	TestTrue(TEXT("folders readable"), FFileHelper::LoadFileToString(FoldersText, *(OutputFolder / TEXT("actors") / TEXT("folders.json"))));
	TSharedPtr<FJsonObject> Folders;
	const TSharedRef<TJsonReader<>> FoldersReader = TJsonReaderFactory<>::Create(FoldersText);
	TestTrue(TEXT("folders parseable"), FJsonSerializer::Deserialize(FoldersReader, Folders) && Folders.IsValid());
	if (Folders.IsValid())
	{
		TestEqual(TEXT("folders kind"), Folders->GetStringField(TEXT("kind")), FString(TEXT("outliner_folders")));
		const TSharedPtr<FJsonObject>* RootFolder = nullptr;
		TestTrue(TEXT("root folder exists"), Folders->TryGetObjectField(TEXT("root"), RootFolder) && RootFolder && RootFolder->IsValid());
		if (RootFolder && RootFolder->IsValid())
		{
			TestTrue(TEXT("root actor count includes root/no-folder actor"), static_cast<int32>((*RootFolder)->GetNumberField(TEXT("direct_actor_count"))) >= 1);
		}

		const TArray<TSharedPtr<FJsonValue>>* FolderItems = nullptr;
		TestTrue(TEXT("folders array exists"), Folders->TryGetArrayField(TEXT("folders"), FolderItems) && FolderItems);
		bool bHasParent = false;
		bool bHasChild = false;
		if (FolderItems)
		{
			for (const TSharedPtr<FJsonValue>& Value : *FolderItems)
			{
				const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr;
				if (!Item.IsValid())
				{
					continue;
				}
				bHasParent = bHasParent || Item->GetStringField(TEXT("path")) == TEXT("Gameplay");
				bHasChild = bHasChild || (Item->GetStringField(TEXT("path")) == TEXT("Gameplay/Entrances") && static_cast<int32>(Item->GetNumberField(TEXT("direct_actor_count"))) == 1);
			}
		}
		TestTrue(TEXT("parent folder exported"), bHasParent);
		TestTrue(TEXT("child folder exported with direct actor count"), bHasChild);
	}

	World->DestroyWorld(false);

	const FString CommandOutputFolder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation") / TEXT("UAI_LevelFolderExportSmoke_Command"));
	IFileManager::Get().DeleteDirectory(*CommandOutputFolder, false, true);

	UeAgentCommandRegistry::FExactCommandInvoker Invoker = UeAgentCommandRegistry::FindExactCommandInvoker(TEXT("level_export_folder"));
	TestTrue(TEXT("level_export_folder registry entry exists"), Invoker != nullptr);
	if (Invoker)
	{
		FUeAgentHttpServer Server;
		FUeAgentRequestContext Ctx;
		Ctx.Command = TEXT("level_export_folder");
		Ctx.Params = MakeShared<FJsonObject>();
		Ctx.Params->SetStringField(TEXT("output_folder"), CommandOutputFolder);
		Ctx.Params->SetBoolField(TEXT("overwrite"), true);

		TSharedPtr<FJsonObject> CommandData;
		TArray<TSharedPtr<FJsonValue>> Artifacts;
		FString CommandError;
		const bool bCommandOk = Invoker(&Server, Ctx, CommandData, Artifacts, CommandError);
		TestTrue(FString::Printf(TEXT("command succeeds: %s"), *CommandError), bCommandOk);
		TestTrue(TEXT("command data valid"), CommandData.IsValid());
		if (CommandData.IsValid())
		{
			TestEqual(TEXT("command schema"), CommandData->GetStringField(TEXT("schema")), FString(TEXT("uai.level_folder.v1")));
			TestEqual(TEXT("command status"), CommandData->GetStringField(TEXT("status")), FString(TEXT("exported")));
			TestEqual(TEXT("command folder path"), CommandData->GetStringField(TEXT("folder_path")), CommandOutputFolder);
			TestFalse(TEXT("command does not inline manifest"), CommandData->HasField(TEXT("manifest")));
		}
		TestTrue(TEXT("command manifest written"), IFileManager::Get().FileExists(*(CommandOutputFolder / TEXT("manifest.json"))));
		TestTrue(TEXT("command actor index written"), IFileManager::Get().FileExists(*(CommandOutputFolder / TEXT("actors") / TEXT("index.json"))));
		TestTrue(TEXT("command actor items dir written"), IFileManager::Get().DirectoryExists(*(CommandOutputFolder / TEXT("actors") / TEXT("items"))));
	}

	return true;
}

#endif
