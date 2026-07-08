#include "Utilities/UeAgentLevelFolderApply.h"
#include "Utilities/UeAgentDetailsAuthoring.h"
#include "Utilities/UeAgentUObjectProperties.h"

#include "Commands/PaletteSearch/UeAgentPaletteSearchAuthoring.h"
#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UeAgentLevelFolderApply
{
	FApplyOptions::FApplyOptions()
		: bCreateMissingActors(true)
	{
	}

	namespace
	{
		constexpr const TCHAR* ApplySchema = TEXT("uai.level_folder_apply.v1");

		bool LoadJsonObjectFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutObject, FString& OutError)
		{
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *FilePath))
			{
				OutError = FString::Printf(TEXT("json_read_failed:%s"), *FilePath);
				return false;
			}

			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
			if (!FJsonSerializer::Deserialize(Reader, OutObject) || !OutObject.IsValid())
			{
				OutError = FString::Printf(TEXT("json_parse_failed:%s"), *FilePath);
				return false;
			}
			return true;
		}

		FString GetStringFieldAny(const TSharedPtr<FJsonObject>& Object, const TArray<FString>& FieldNames)
		{
			if (!Object.IsValid())
			{
				return FString();
			}

			for (const FString& FieldName : FieldNames)
			{
				FString Value;
				if (Object->TryGetStringField(FieldName, Value))
				{
					return Value;
				}
			}
			return FString();
		}

		AActor* FindActorByObjectPathOrLabel(UWorld* World, const FString& ObjectPath, const FString& Label)
		{
			if (!World)
			{
				return nullptr;
			}

			for (ULevel* Level : World->GetLevels())
			{
				if (!Level)
				{
					continue;
				}
				for (AActor* Actor : Level->Actors)
				{
					if (!IsValid(Actor))
					{
						continue;
					}
					if (!ObjectPath.IsEmpty() && Actor->GetPathName() == ObjectPath)
					{
						return Actor;
					}
					if (!Label.IsEmpty() && Actor->GetActorLabel() == Label)
					{
						return Actor;
					}
				}
			}
			return nullptr;
		}

		UClass* ResolveActorClass(const FString& ClassPath)
		{
			if (ClassPath.IsEmpty())
			{
				return nullptr;
			}

			UClass* ActorClass = FindObject<UClass>(nullptr, *ClassPath);
			if (!ActorClass)
			{
				ActorClass = LoadObject<UClass>(nullptr, *ClassPath);
			}
			if (!ActorClass || !ActorClass->IsChildOf(AActor::StaticClass()))
			{
				return nullptr;
			}
			return ActorClass;
		}

		UActorComponent* FindComponentBySummary(AActor* Actor, const TSharedPtr<FJsonObject>& ComponentJson)
		{
			if (!Actor || !ComponentJson.IsValid())
			{
				return nullptr;
			}

			const FString ObjectPath = GetStringFieldAny(ComponentJson, { TEXT("object_path") });
			const FString Name = GetStringFieldAny(ComponentJson, { TEXT("name"), TEXT("component_name") });
			const FString ClassPath = GetStringFieldAny(ComponentJson, { TEXT("path"), TEXT("class_path"), TEXT("component_class_path") });

			TArray<UActorComponent*> Components;
			Actor->GetComponents(Components);
			for (UActorComponent* Component : Components)
			{
				if (!Component)
				{
					continue;
				}
				if (!ObjectPath.IsEmpty() && Component->GetPathName() == ObjectPath)
				{
					return Component;
				}
				if (!Name.IsEmpty() && Component->GetName() == Name)
				{
					return Component;
				}
				if (!ClassPath.IsEmpty() && Component->GetClass() && Component->GetClass()->GetPathName() == ClassPath)
				{
					return Component;
				}
			}
			return nullptr;
		}

		bool TryReadVectorField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, FVector& OutVector)
		{
			if (!Object.IsValid())
			{
				return false;
			}

			const TSharedPtr<FJsonObject>* VectorObject = nullptr;
			if (Object->TryGetObjectField(FieldName, VectorObject) && VectorObject && VectorObject->IsValid())
			{
				double X = 0.0;
				double Y = 0.0;
				double Z = 0.0;
				if ((*VectorObject)->TryGetNumberField(TEXT("x"), X) &&
					(*VectorObject)->TryGetNumberField(TEXT("y"), Y) &&
					(*VectorObject)->TryGetNumberField(TEXT("z"), Z))
				{
					OutVector = FVector(X, Y, Z);
					return true;
				}
			}

			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (Object->TryGetArrayField(FieldName, Values) && Values && Values->Num() >= 3)
			{
				OutVector = FVector((*Values)[0]->AsNumber(), (*Values)[1]->AsNumber(), (*Values)[2]->AsNumber());
				return true;
			}
			return false;
		}

		bool TryReadRotatorField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, FRotator& OutRotator)
		{
			if (!Object.IsValid())
			{
				return false;
			}

			const TSharedPtr<FJsonObject>* RotatorObject = nullptr;
			if (Object->TryGetObjectField(FieldName, RotatorObject) && RotatorObject && RotatorObject->IsValid())
			{
				double Pitch = 0.0;
				double Yaw = 0.0;
				double Roll = 0.0;
				if ((*RotatorObject)->TryGetNumberField(TEXT("pitch"), Pitch) &&
					(*RotatorObject)->TryGetNumberField(TEXT("yaw"), Yaw) &&
					(*RotatorObject)->TryGetNumberField(TEXT("roll"), Roll))
				{
					OutRotator = FRotator(Pitch, Yaw, Roll);
					return true;
				}
			}

			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (Object->TryGetArrayField(FieldName, Values) && Values && Values->Num() >= 3)
			{
				OutRotator = FRotator((*Values)[0]->AsNumber(), (*Values)[1]->AsNumber(), (*Values)[2]->AsNumber());
				return true;
			}
			return false;
		}

		bool TryReadActorTransform(const TSharedPtr<FJsonObject>& ActorJson, AActor* Actor, FTransform& OutTransform)
		{
			const TSharedPtr<FJsonObject>* TransformObject = nullptr;
			if (!ActorJson.IsValid() || !ActorJson->TryGetObjectField(TEXT("transform"), TransformObject) || !TransformObject || !TransformObject->IsValid())
			{
				return false;
			}

			FVector Location = Actor ? Actor->GetActorLocation() : FVector::ZeroVector;
			FRotator Rotation = Actor ? Actor->GetActorRotation() : FRotator::ZeroRotator;
			FVector Scale = Actor ? Actor->GetActorScale3D() : FVector::OneVector;
			TryReadVectorField(*TransformObject, TEXT("location"), Location);
			TryReadRotatorField(*TransformObject, TEXT("rotation"), Rotation);
			TryReadVectorField(*TransformObject, TEXT("scale"), Scale);
			OutTransform = FTransform(Rotation, Location, Scale);
			return true;
		}

		UeAgentDetailsAuthoring::FOwnerInfo MakeLevelApplyOwnerInfo(
			const FString& OwnerKind,
			const FString& OwnerId,
			UObject* Object = nullptr)
		{
			UeAgentDetailsAuthoring::FOwnerInfo OwnerInfo;
			OwnerInfo.OwnerKind = OwnerKind;
			OwnerInfo.OwnerId = OwnerId;
			OwnerInfo.SourceCommand = TEXT("level_apply_folder");
			if (Object)
			{
				OwnerInfo.OwnerName = Object->GetName();
				OwnerInfo.OwnerPath = Object->GetPathName();
				OwnerInfo.OwnerClassPath = Object->GetClass() ? Object->GetClass()->GetPathName() : FString();
			}
			return OwnerInfo;
		}

		bool BuildDirectLegacyFileRejectionIfPresent(
			const FString& OwnerFolder,
			const FString& JsonPathPrefix,
			const UeAgentDetailsAuthoring::FOwnerInfo& OwnerInfo,
			TSharedPtr<FJsonObject>& OutRejection)
		{
			OutRejection.Reset();

			struct FLegacyFileRule
			{
				const TCHAR* FileName;
				const TCHAR* LegacySurface;
				const TCHAR* ErrorCode;
			};

			static const FLegacyFileRule Rules[] = {
				{ TEXT("properties.json"), TEXT("properties.json"), TEXT("legacy_properties_file_not_supported") },
				{ TEXT("raw_properties.json"), TEXT("raw_properties.json"), TEXT("legacy_raw_properties_not_supported") },
				{ TEXT("readonly_properties.json"), TEXT("readonly_properties.json"), TEXT("readonly_properties_not_authoring") }
			};

			for (const FLegacyFileRule& Rule : Rules)
			{
				if (!IFileManager::Get().FileExists(*(OwnerFolder / Rule.FileName)))
				{
					continue;
				}

				const FString JsonPath = JsonPathPrefix.IsEmpty()
					? FString(Rule.FileName)
					: FPaths::Combine(JsonPathPrefix, Rule.FileName);
				OutRejection = UeAgentDetailsAuthoring::MakeLegacyPropertiesRejection(
					Rule.ErrorCode,
					Rule.LegacySurface,
					OwnerInfo,
					JsonPath,
					FString(),
					TEXT("details/index.json"));
				return true;
			}
			return false;
		}

		bool BuildLevelApplyLegacyRejection(
			const FString& Root,
			const FString& ActorItemsRoot,
			TSharedPtr<FJsonObject>& OutRejection,
			FString& OutError)
		{
			OutRejection.Reset();
			OutError.Reset();

			const UeAgentDetailsAuthoring::FOwnerInfo WorldSettingsOwner = MakeLevelApplyOwnerInfo(TEXT("world_settings"), TEXT("world_settings"));
			const FString WorldSettingsRoot = Root / TEXT("world_settings");
			if (BuildDirectLegacyFileRejectionIfPresent(WorldSettingsRoot, TEXT("world_settings"), WorldSettingsOwner, OutRejection))
			{
				return true;
			}
			TSharedPtr<FJsonObject> WorldSettingsJson;
			FString WorldSettingsError;
			if (IFileManager::Get().FileExists(*(WorldSettingsRoot / TEXT("summary.json"))) &&
				LoadJsonObjectFile(WorldSettingsRoot / TEXT("summary.json"), WorldSettingsJson, WorldSettingsError) &&
				UeAgentDetailsAuthoring::BuildLegacyPropertiesRejectionIfPresent(WorldSettingsJson, WorldSettingsOwner, TEXT("details/index.json"), OutRejection))
			{
				return true;
			}

			TArray<FString> ActorDirectories;
			IFileManager::Get().FindFiles(ActorDirectories, *(ActorItemsRoot / TEXT("*")), false, true);
			for (const FString& ActorDirectoryName : ActorDirectories)
			{
				const FString ActorRoot = ActorItemsRoot / ActorDirectoryName;
				const FString ActorJsonPath = ActorRoot / TEXT("actor.json");
				TSharedPtr<FJsonObject> ActorJson;
				FString ActorError;
				LoadJsonObjectFile(ActorJsonPath, ActorJson, ActorError);
				const FString ActorId = GetStringFieldAny(ActorJson, { TEXT("actor_id") }).IsEmpty()
					? ActorDirectoryName
					: GetStringFieldAny(ActorJson, { TEXT("actor_id") });
				const UeAgentDetailsAuthoring::FOwnerInfo ActorOwner = MakeLevelApplyOwnerInfo(TEXT("actor"), ActorId);
				if (ActorJson.IsValid() &&
					UeAgentDetailsAuthoring::BuildLegacyPropertiesRejectionIfPresent(ActorJson, ActorOwner, TEXT("details/index.json"), OutRejection))
				{
					return true;
				}
				if (BuildDirectLegacyFileRejectionIfPresent(
					ActorRoot,
					FPaths::Combine(TEXT("actors"), TEXT("items"), ActorDirectoryName),
					ActorOwner,
					OutRejection))
				{
					return true;
				}

				const FString ComponentsRoot = ActorRoot / TEXT("components");
				if (!IFileManager::Get().DirectoryExists(*ComponentsRoot))
				{
					continue;
				}

				TArray<FString> ComponentDirectories;
				IFileManager::Get().FindFiles(ComponentDirectories, *(ComponentsRoot / TEXT("*")), false, true);
				for (const FString& ComponentDirectoryName : ComponentDirectories)
				{
					const FString ComponentRoot = ComponentsRoot / ComponentDirectoryName;
					const FString ComponentJsonPath = ComponentRoot / TEXT("component.json");
					TSharedPtr<FJsonObject> ComponentJson;
					FString ComponentError;
					LoadJsonObjectFile(ComponentJsonPath, ComponentJson, ComponentError);
					const FString ComponentId = GetStringFieldAny(ComponentJson, { TEXT("component_id"), TEXT("name"), TEXT("component_name") }).IsEmpty()
						? ComponentDirectoryName
						: GetStringFieldAny(ComponentJson, { TEXT("component_id"), TEXT("name"), TEXT("component_name") });
					const UeAgentDetailsAuthoring::FOwnerInfo ComponentOwner = MakeLevelApplyOwnerInfo(TEXT("component"), ComponentId);
					if (ComponentJson.IsValid() &&
						UeAgentDetailsAuthoring::BuildLegacyPropertiesRejectionIfPresent(ComponentJson, ComponentOwner, TEXT("details/index.json"), OutRejection))
					{
						return true;
					}
					if (BuildDirectLegacyFileRejectionIfPresent(
						ComponentRoot,
						FPaths::Combine(TEXT("actors"), TEXT("items"), ActorDirectoryName, TEXT("components"), ComponentDirectoryName),
						ComponentOwner,
						OutRejection))
					{
						return true;
					}
				}
			}

			const UeAgentDetailsAuthoring::FOwnerInfo LevelOwner = MakeLevelApplyOwnerInfo(TEXT("level_folder"), Root);
			if (UeAgentDetailsAuthoring::BuildLegacyPropertiesFileRejectionIfPresent(Root, LevelOwner, TEXT("details/index.json"), OutRejection))
			{
				return true;
			}

			return false;
		}

		bool FailWithLegacyRejection(
			const TSharedPtr<FJsonObject>& Rejection,
			TSharedPtr<FJsonObject>& OutSummary,
			FString& OutError)
		{
			OutSummary = Rejection;
			if (!OutSummary.IsValid() || !OutSummary->TryGetStringField(TEXT("error_code"), OutError) || OutError.IsEmpty())
			{
				OutError = TEXT("legacy_properties_not_supported");
			}
			return false;
		}

		TSharedPtr<FJsonObject> ApplyGroupedDetailsIfExists(
			UObject* Object,
			const FString& DetailsRoot,
			const FString& OwnerKind,
			const FString& OwnerId,
			FString& OutWarning)
		{
			OutWarning.Reset();
			if (!UeAgentDetailsAuthoring::HasGroupedDetails(DetailsRoot))
			{
				return nullptr;
			}

			UeAgentDetailsAuthoring::FOwnerDetailsApplyRequest Request;
			Request.Object = Object;
			Request.OwnerInfo = MakeLevelApplyOwnerInfo(OwnerKind, OwnerId, Object);
			Request.DetailsRoot = DetailsRoot;
			Request.IndexFile = TEXT("details/index.json");
			Request.RefFieldName = TEXT("details");

			TSharedPtr<FJsonObject> ApplySummary;
			FString Error;
			if (!UeAgentDetailsAuthoring::ApplyOwnerDetails(Request, ApplySummary, Error))
			{
				OutWarning = Error;
				TSharedPtr<FJsonObject> Failed = MakeShared<FJsonObject>();
				Failed->SetStringField(TEXT("schema"), TEXT("uai.grouped_details_apply.v1"));
				Failed->SetBoolField(TEXT("ok"), false);
				Failed->SetStringField(TEXT("status"), TEXT("failed"));
				Failed->SetStringField(TEXT("reason"), Error);
				return Failed;
			}

			return ApplySummary;
		}

		TArray<TSharedPtr<FJsonValue>> ApplyComponentPackages(AActor* Actor, const FString& ComponentsRoot, TArray<TSharedPtr<FJsonValue>>& Warnings)
		{
			TArray<TSharedPtr<FJsonValue>> Results;
			if (!Actor || !IFileManager::Get().DirectoryExists(*ComponentsRoot))
			{
				return Results;
			}

			TArray<FString> ComponentDirectories;
			IFileManager::Get().FindFiles(ComponentDirectories, *(ComponentsRoot / TEXT("*")), false, true);
			for (const FString& ComponentDirectoryName : ComponentDirectories)
			{
				const FString ComponentRoot = ComponentsRoot / ComponentDirectoryName;
				TSharedPtr<FJsonObject> ComponentResult = MakeShared<FJsonObject>();
				ComponentResult->SetStringField(TEXT("component_id"), ComponentDirectoryName);

				TSharedPtr<FJsonObject> ComponentJson;
				FString ComponentError;
				if (!LoadJsonObjectFile(ComponentRoot / TEXT("component.json"), ComponentJson, ComponentError))
				{
					ComponentResult->SetStringField(TEXT("status"), TEXT("skipped"));
					ComponentResult->SetStringField(TEXT("reason"), ComponentError);
					Warnings.Add(MakeShared<FJsonValueString>(ComponentError));
					Results.Add(MakeShared<FJsonValueObject>(ComponentResult));
					continue;
				}

				UActorComponent* Component = FindComponentBySummary(Actor, ComponentJson);
				if (!Component)
				{
					ComponentResult->SetStringField(TEXT("status"), TEXT("skipped"));
					ComponentResult->SetStringField(TEXT("reason"), TEXT("component_not_found"));
					Results.Add(MakeShared<FJsonValueObject>(ComponentResult));
					continue;
				}

				ComponentResult->SetStringField(TEXT("status"), TEXT("matched"));
				ComponentResult->SetStringField(TEXT("name"), Component->GetName());
				ComponentResult->SetStringField(TEXT("object_path"), Component->GetPathName());

				FString DetailsWarning;
				TSharedPtr<FJsonObject> DetailsResult = ApplyGroupedDetailsIfExists(
					Component,
					ComponentRoot / TEXT("details"),
					TEXT("component"),
					ComponentDirectoryName,
					DetailsWarning);
				if (!DetailsWarning.IsEmpty())
				{
					Warnings.Add(MakeShared<FJsonValueString>(DetailsWarning));
				}
				if (DetailsResult.IsValid())
				{
					ComponentResult->SetObjectField(TEXT("properties_apply"), DetailsResult);
				}
				Results.Add(MakeShared<FJsonValueObject>(ComponentResult));
			}
			return Results;
		}

		TSharedPtr<FJsonObject> ApplyWorldSettingsPackage(UWorld* World, const FString& Root, TArray<TSharedPtr<FJsonValue>>& Warnings)
		{
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("status"), TEXT("skipped"));
			AWorldSettings* WorldSettings = World ? World->GetWorldSettings(false, false) : nullptr;
			if (!WorldSettings)
			{
				Result->SetStringField(TEXT("reason"), TEXT("world_settings_not_available"));
				return Result;
			}

			Result->SetStringField(TEXT("status"), TEXT("matched"));
			Result->SetStringField(TEXT("object_path"), WorldSettings->GetPathName());

			FString DetailsWarning;
			TSharedPtr<FJsonObject> DetailsApply = ApplyGroupedDetailsIfExists(
				WorldSettings,
				Root / TEXT("world_settings") / TEXT("details"),
				TEXT("world_settings"),
				TEXT("world_settings"),
				DetailsWarning);
			if (!DetailsWarning.IsEmpty())
			{
				Warnings.Add(MakeShared<FJsonValueString>(DetailsWarning));
			}
			if (DetailsApply.IsValid())
			{
				Result->SetObjectField(TEXT("properties_apply"), DetailsApply);
			}
			return Result;
		}

		TSharedPtr<FJsonObject> MakeActorResult(
			const FString& ActorId,
			const FString& Label,
			const FString& ClassPath,
			const FString& Status,
			AActor* Actor,
			const FString& Reason = FString())
		{
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("actor_id"), ActorId);
			Result->SetStringField(TEXT("label"), Label);
			Result->SetStringField(TEXT("path"), ClassPath);
			Result->SetStringField(TEXT("status"), Status);
			if (Actor)
			{
				Result->SetStringField(TEXT("object_path"), Actor->GetPathName());
			}
			if (!Reason.IsEmpty())
			{
				Result->SetStringField(TEXT("reason"), Reason);
			}
			return Result;
		}
	}

	bool ApplyLevelFolder(
		UWorld* World,
		const FApplyOptions& Options,
		TSharedPtr<FJsonObject>& OutSummary,
		FString& OutError)
	{
		if (!World)
		{
			OutError = TEXT("missing_world");
			return false;
		}

		const FString Root = FPaths::ConvertRelativePathToFull(Options.InputFolder);
		if (Root.IsEmpty() || !IFileManager::Get().DirectoryExists(*Root))
		{
			OutError = TEXT("input_folder_not_found");
			return false;
		}

		const FString ActorItemsRoot = Root / TEXT("actors") / TEXT("items");
		if (!IFileManager::Get().DirectoryExists(*ActorItemsRoot))
		{
			OutError = TEXT("actor_items_folder_not_found");
			return false;
		}

		TSharedPtr<FJsonObject> LegacyRejection;
		if (BuildLevelApplyLegacyRejection(Root, ActorItemsRoot, LegacyRejection, OutError))
		{
			return FailWithLegacyRejection(LegacyRejection, OutSummary, OutError);
		}

		TArray<FString> ActorDirectories;
		IFileManager::Get().FindFiles(ActorDirectories, *(ActorItemsRoot / TEXT("*")), false, true);

		int32 Created = 0;
		int32 Updated = 0;
		int32 Skipped = 0;
		int32 ActorPropertiesApplied = 0;
		int32 ComponentPropertiesApplied = 0;
		int32 WorldSettingsPropertiesApplied = 0;
		TArray<TSharedPtr<FJsonValue>> ActorResults;
		TArray<TSharedPtr<FJsonValue>> Warnings;
		TSharedPtr<FJsonObject> WorldSettingsResult = ApplyWorldSettingsPackage(World, Root, Warnings);
		const TSharedPtr<FJsonObject>* WorldSettingsApply = nullptr;
		if (WorldSettingsResult.IsValid() &&
			WorldSettingsResult->TryGetObjectField(TEXT("properties_apply"), WorldSettingsApply) &&
			WorldSettingsApply &&
			WorldSettingsApply->IsValid())
		{
			WorldSettingsPropertiesApplied += static_cast<int32>((*WorldSettingsApply)->GetNumberField(TEXT("properties_applied")));
		}

		for (const FString& ActorDirectoryName : ActorDirectories)
		{
			const FString ActorJsonPath = ActorItemsRoot / ActorDirectoryName / TEXT("actor.json");
			TSharedPtr<FJsonObject> ActorJson;
			FString ActorError;
			if (!LoadJsonObjectFile(ActorJsonPath, ActorJson, ActorError))
			{
				Skipped++;
				Warnings.Add(MakeShared<FJsonValueString>(ActorError));
				ActorResults.Add(MakeShared<FJsonValueObject>(MakeActorResult(ActorDirectoryName, FString(), FString(), TEXT("skipped"), nullptr, ActorError)));
				continue;
			}

			const FString ActorId = GetStringFieldAny(ActorJson, { TEXT("actor_id") });
			const FString Label = GetStringFieldAny(ActorJson, { TEXT("label"), TEXT("actor_label") });
			FString ClassPath = GetStringFieldAny(ActorJson, { TEXT("path"), TEXT("class_path") });
			if (ClassPath.TrimStartAndEnd().IsEmpty())
			{
				const FString CatalogName = GetStringFieldAny(ActorJson, { TEXT("catalog_name"), TEXT("name") });
				if (!CatalogName.TrimStartAndEnd().IsEmpty())
				{
					UeAgentPaletteSearch::FCandidate CatalogCandidate;
					FString ResolveError;
					if (UeAgentPaletteSearch::ResolveCatalogNameToCandidate(TEXT("placeable_actor"), CatalogName, CatalogCandidate, ResolveError))
					{
						ClassPath = CatalogCandidate.FullName;
					}
					else
					{
						Warnings.Add(MakeShared<FJsonValueString>(ResolveError + TEXT(":") + ActorDirectoryName));
					}
				}
			}
			const FString ObjectPath = GetStringFieldAny(ActorJson, { TEXT("object_path") });
			const FString FolderPath = GetStringFieldAny(ActorJson, { TEXT("folder_path") });
			FString ActorStatus;

			AActor* Actor = FindActorByObjectPathOrLabel(World, ObjectPath, Label);
			if (Actor)
			{
				if (!Label.IsEmpty())
				{
					Actor->SetActorLabel(Label);
				}
				Actor->SetFolderPath(FName(*FolderPath));
				Updated++;
				ActorStatus = TEXT("updated");
			}
			else if (!Options.bCreateMissingActors)
			{
				Skipped++;
				ActorResults.Add(MakeShared<FJsonValueObject>(MakeActorResult(ActorId, Label, ClassPath, TEXT("skipped"), nullptr, TEXT("create_missing_actors_false"))));
				continue;
			}
			else
			{
				UClass* ActorClass = ResolveActorClass(ClassPath);
				if (!ActorClass)
				{
					Skipped++;
					const FString Reason = ClassPath.IsEmpty() ? TEXT("missing_path") : TEXT("actor_class_not_found_or_invalid");
					Warnings.Add(MakeShared<FJsonValueString>(Reason + TEXT(":") + ActorDirectoryName));
					ActorResults.Add(MakeShared<FJsonValueObject>(MakeActorResult(ActorId, Label, ClassPath, TEXT("skipped"), nullptr, Reason)));
					continue;
				}

				FActorSpawnParameters SpawnParameters;
				SpawnParameters.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
				Actor = World->SpawnActor<AActor>(ActorClass, FTransform::Identity, SpawnParameters);
				if (!Actor)
				{
					Skipped++;
					Warnings.Add(MakeShared<FJsonValueString>(TEXT("actor_spawn_failed:") + ActorDirectoryName));
					ActorResults.Add(MakeShared<FJsonValueObject>(MakeActorResult(ActorId, Label, ClassPath, TEXT("skipped"), nullptr, TEXT("actor_spawn_failed"))));
					continue;
				}

				if (!Label.IsEmpty())
				{
					Actor->SetActorLabel(Label);
				}
				Actor->SetFolderPath(FName(*FolderPath));
				Created++;
				ActorStatus = TEXT("created");
			}

			FTransform DesiredTransform;
			if (TryReadActorTransform(ActorJson, Actor, DesiredTransform))
			{
				Actor->SetActorTransform(DesiredTransform);
			}

			TSharedPtr<FJsonObject> ActorResult = MakeActorResult(ActorId, Label, ClassPath, ActorStatus, Actor);
			const FString ActorRoot = ActorItemsRoot / ActorDirectoryName;
			FString ActorDetailsWarning;
			TSharedPtr<FJsonObject> ActorDetailsApply = ApplyGroupedDetailsIfExists(
				Actor,
				ActorRoot / TEXT("details"),
				TEXT("actor"),
				ActorId.IsEmpty() ? ActorDirectoryName : ActorId,
				ActorDetailsWarning);
			if (!ActorDetailsWarning.IsEmpty())
			{
				Warnings.Add(MakeShared<FJsonValueString>(ActorDetailsWarning));
			}
			if (ActorDetailsApply.IsValid())
			{
				ActorResult->SetObjectField(TEXT("properties_apply"), ActorDetailsApply);
				ActorPropertiesApplied += static_cast<int32>(ActorDetailsApply->GetNumberField(TEXT("properties_applied")));
			}

			TArray<TSharedPtr<FJsonValue>> ComponentResults = ApplyComponentPackages(Actor, ActorRoot / TEXT("components"), Warnings);
			for (const TSharedPtr<FJsonValue>& ComponentValue : ComponentResults)
			{
				const TSharedPtr<FJsonObject> ComponentObject = ComponentValue.IsValid() ? ComponentValue->AsObject() : nullptr;
				const TSharedPtr<FJsonObject>* PropertyApply = nullptr;
				if (ComponentObject.IsValid() && ComponentObject->TryGetObjectField(TEXT("properties_apply"), PropertyApply) && PropertyApply && PropertyApply->IsValid())
				{
					ComponentPropertiesApplied += static_cast<int32>((*PropertyApply)->GetNumberField(TEXT("properties_applied")));
				}
			}
			ActorResult->SetArrayField(TEXT("components"), ComponentResults);
			ActorResults.Add(MakeShared<FJsonValueObject>(ActorResult));
		}

		OutSummary = MakeShared<FJsonObject>();
		OutSummary->SetStringField(TEXT("schema"), ApplySchema);
		OutSummary->SetStringField(TEXT("status"), TEXT("applied"));
		OutSummary->SetStringField(TEXT("folder_path"), Root);
		OutSummary->SetNumberField(TEXT("actors_seen"), ActorDirectories.Num());
		OutSummary->SetNumberField(TEXT("actors_created"), Created);
		OutSummary->SetNumberField(TEXT("actors_updated"), Updated);
		OutSummary->SetNumberField(TEXT("actors_skipped"), Skipped);
		OutSummary->SetObjectField(TEXT("world_settings"), WorldSettingsResult);
		OutSummary->SetNumberField(TEXT("world_settings_properties_applied"), WorldSettingsPropertiesApplied);
		OutSummary->SetNumberField(TEXT("actor_properties_applied"), ActorPropertiesApplied);
		OutSummary->SetNumberField(TEXT("component_properties_applied"), ComponentPropertiesApplied);
		OutSummary->SetArrayField(TEXT("actors"), ActorResults);
		OutSummary->SetArrayField(TEXT("warnings"), Warnings);
		return true;
	}
}
