#include "Utilities/UeAgentLevelFolderExport.h"
#include "Utilities/UeAgentDetailsAuthoring.h"
#include "Utilities/UeAgentStructuredJsonFormatter.h"
#include "Utilities/UeAgentUObjectProperties.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"
#include "WorldPartition/DataLayer/DataLayerManager.h"
#include "WorldPartition/DataLayer/WorldDataLayers.h"
#include "WorldPartition/HLOD/HLODActor.h"
#include "WorldPartition/WorldPartition.h"

namespace UeAgentLevelFolderExport
{
	FExportOptions::FExportOptions()
		: Profile(TEXT("llm_authoring"))
		, bOverwrite(true)
		, bSplitProperties(false)
	{
	}

	namespace
	{
		constexpr const TCHAR* LevelFolderSchema = TEXT("uai.level_folder.v1");

		struct FReferenceRecord
		{
			FString SourceKind;
			FString SourcePath;
			FString SourceClassPath;
			FString PropertyPath;
			FString ReferenceType;
			FString TargetPath;
			FString TargetPackagePath;
			FString TargetClassPath;
			bool bExternalAsset = false;
		};

		TArray<AActor*> GetExportableActors(UWorld* World);
		TArray<UActorComponent*> GetExportableComponents(AActor* Actor);

		FString WorldTypeToString(const EWorldType::Type WorldType)
		{
			switch (WorldType)
			{
			case EWorldType::Game:
				return TEXT("game");
			case EWorldType::Editor:
				return TEXT("editor");
			case EWorldType::PIE:
				return TEXT("pie");
			case EWorldType::EditorPreview:
				return TEXT("editor_preview");
			case EWorldType::GamePreview:
				return TEXT("game_preview");
			case EWorldType::GameRPC:
				return TEXT("game_rpc");
			case EWorldType::Inactive:
				return TEXT("inactive");
			default:
				return TEXT("none");
			}
		}

		FString SafeName(FString Value)
		{
			if (Value.IsEmpty())
			{
				Value = TEXT("UntitledLevel");
			}
			const FString InvalidChars = FPaths::GetInvalidFileSystemChars();
			for (const TCHAR InvalidChar : InvalidChars)
			{
				Value.ReplaceCharInline(InvalidChar, TCHAR('_'));
			}
			Value.ReplaceInline(TEXT(":"), TEXT("_"));
			Value.ReplaceInline(TEXT("/"), TEXT("_"));
			Value.ReplaceInline(TEXT("\\"), TEXT("_"));
			return Value;
		}

		FString ResolveOutputFolder(UWorld* World, const FString& RequestedOutputFolder)
		{
			if (!RequestedOutputFolder.IsEmpty())
			{
				return FPaths::ConvertRelativePathToFull(RequestedOutputFolder);
			}

			const FString PackageName = (World && World->GetOutermost()) ? World->GetOutermost()->GetName() : FString();
			const FString MapName = SafeName(FPackageName::GetShortName(PackageName).IsEmpty() ? (World ? World->GetMapName() : FString()) : FPackageName::GetShortName(PackageName));
			const FString Stamp = FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S"));
			return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("UAI") / TEXT("LevelFolderExports") / (MapName + TEXT("_") + Stamp));
		}

		TSharedPtr<FJsonValue> StringValue(const FString& Value)
		{
			return MakeShared<FJsonValueString>(Value);
		}

		TArray<TSharedPtr<FJsonValue>> StringArray(const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			Result.Reserve(Values.Num());
			for (const FString& Value : Values)
			{
				Result.Add(StringValue(Value));
			}
			return Result;
		}

		FString NormalizeProfile(const FString& Profile)
		{
			const FString Trimmed = Profile.TrimStartAndEnd().ToLower();
			return Trimmed.IsEmpty() ? TEXT("llm_authoring") : Trimmed;
		}

		bool ShouldSplitProperties(const FExportOptions& Options)
		{
			const FString Profile = NormalizeProfile(Options.Profile);
			return Options.bSplitProperties || Profile == TEXT("legacy") || Profile == TEXT("readback") || Profile == TEXT("debug");
		}

		bool ShouldIncludeDiagnostics(const FExportOptions& Options)
		{
			return NormalizeProfile(Options.Profile) == TEXT("debug");
		}

		bool ShouldWriteGroupedDetails(const FExportOptions& Options)
		{
			return !ShouldSplitProperties(Options);
		}

		TSharedPtr<FJsonObject> MakeVectorJson(const FVector& Vector)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetNumberField(TEXT("x"), Vector.X);
			Obj->SetNumberField(TEXT("y"), Vector.Y);
			Obj->SetNumberField(TEXT("z"), Vector.Z);
			return Obj;
		}

		TSharedPtr<FJsonObject> MakeRotatorJson(const FRotator& Rotator)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetNumberField(TEXT("pitch"), Rotator.Pitch);
			Obj->SetNumberField(TEXT("yaw"), Rotator.Yaw);
			Obj->SetNumberField(TEXT("roll"), Rotator.Roll);
			return Obj;
		}

		TSharedPtr<FJsonObject> MakeSpaceContractJson(const FString& Subject, const FString& Frame)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("subject"), Subject);
			Obj->SetStringField(TEXT("dimension"), TEXT("3d"));
			Obj->SetStringField(TEXT("Fread"), Frame);
			Obj->SetStringField(TEXT("Fwrite"), Frame);
			Obj->SetStringField(TEXT("Fsemantic"), TEXT("requires_command_or_asset_context"));
			Obj->SetStringField(TEXT("Fverify"), Frame);
			Obj->SetStringField(TEXT("conversion_chain"), TEXT("semantic_context_to_ue_frame_required"));
			return Obj;
		}

		TSharedPtr<FJsonObject> MakeActorTransformJson(AActor* Actor)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetObjectField(TEXT("location"), MakeVectorJson(Actor ? Actor->GetActorLocation() : FVector::ZeroVector));
			Obj->SetObjectField(TEXT("rotation"), MakeRotatorJson(Actor ? Actor->GetActorRotation() : FRotator::ZeroRotator));
			Obj->SetObjectField(TEXT("scale"), MakeVectorJson(Actor ? Actor->GetActorScale3D() : FVector::OneVector));
			Obj->SetObjectField(TEXT("space_contract"), MakeSpaceContractJson(TEXT("actor.transform"), TEXT("ue_actor_world_transform")));
			return Obj;
		}

		TArray<TSharedPtr<FJsonValue>> MakeTagsJson(AActor* Actor)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			if (!Actor)
			{
				return Values;
			}
			Values.Reserve(Actor->Tags.Num());
			for (const FName& Tag : Actor->Tags)
			{
				Values.Add(StringValue(Tag.ToString()));
			}
			return Values;
		}

		TSharedPtr<FJsonObject> MakeSceneAttachmentJson(UActorComponent* Component)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			USceneComponent* SceneComponent = Cast<USceneComponent>(Component);
			Obj->SetBoolField(TEXT("is_scene_component"), SceneComponent != nullptr);
			if (!SceneComponent)
			{
				return Obj;
			}
			Obj->SetStringField(TEXT("parent_component"), SceneComponent->GetAttachParent() ? SceneComponent->GetAttachParent()->GetName() : FString());
			Obj->SetStringField(TEXT("socket_name"), SceneComponent->GetAttachSocketName().ToString());
			Obj->SetObjectField(TEXT("relative_location"), MakeVectorJson(SceneComponent->GetRelativeLocation()));
			Obj->SetObjectField(TEXT("relative_rotation"), MakeRotatorJson(SceneComponent->GetRelativeRotation()));
			Obj->SetObjectField(TEXT("relative_scale"), MakeVectorJson(SceneComponent->GetRelativeScale3D()));
			Obj->SetObjectField(TEXT("space_contract"), MakeSpaceContractJson(TEXT("component.attachment"), TEXT("ue_scene_component_parent_relative_transform")));
			return Obj;
		}

		void InlineProperties(TSharedPtr<FJsonObject> Target, const TSharedPtr<FJsonObject>& Properties)
		{
			if (!Target.IsValid() || !Properties.IsValid())
			{
				return;
			}
			FString Profile;
			if (Properties->TryGetStringField(TEXT("profile"), Profile))
			{
				if (Profile != TEXT("authoring"))
				{
					Target->SetStringField(TEXT("properties_profile"), Profile);
				}
			}
			double PropertyCount = 0.0;
			if (Properties->TryGetNumberField(TEXT("property_count"), PropertyCount))
			{
				Target->SetNumberField(TEXT("property_count"), PropertyCount);
			}
			bool bTruncated = false;
			if (Properties->TryGetBoolField(TEXT("truncated"), bTruncated))
			{
				if (bTruncated)
				{
					Target->SetBoolField(TEXT("properties_truncated"), bTruncated);
				}
			}
			const TArray<TSharedPtr<FJsonValue>>* PropertyValues = nullptr;
			if (Properties->TryGetArrayField(TEXT("properties"), PropertyValues) && PropertyValues)
			{
				Target->SetArrayField(TEXT("properties"), *PropertyValues);
			}
		}

		TSharedPtr<FJsonObject> MakeObjectReferenceJson(UObject* Object)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetBoolField(TEXT("available"), Object != nullptr);
			Obj->SetStringField(TEXT("name"), Object ? Object->GetName() : FString());
			Obj->SetStringField(TEXT("class_path"), (Object && Object->GetClass()) ? Object->GetClass()->GetPathName() : FString());
			Obj->SetStringField(TEXT("object_path"), Object ? Object->GetPathName() : FString());
			return Obj;
		}

		bool ExportOwnerGroupedDetails(
			UObject* Object,
			const FString& OwnerKind,
			const FString& OwnerId,
			const FString& DetailsRoot,
			const FString& SourceCommand,
			const UeAgentUObjectProperties::FExportOptions& PropertyOptions,
			TSharedPtr<FJsonObject>& OutDetailsSummary,
			FString& OutError)
		{
			UeAgentDetailsAuthoring::FOwnerDetailsExportRequest Request =
				UeAgentDetailsAuthoring::MakeOwnerDetailsExportRequest(Object, OwnerKind, OwnerId, DetailsRoot, SourceCommand);
			Request.PropertyOptions = PropertyOptions;
			return UeAgentDetailsAuthoring::ExportOwnerDetails(Request, OutDetailsSummary, OutError);
		}

		bool IsExternalAssetPackage(const FString& SourcePackagePath, const FString& TargetPackagePath)
		{
			if (TargetPackagePath.IsEmpty() || TargetPackagePath == SourcePackagePath)
			{
				return false;
			}
			return TargetPackagePath.StartsWith(TEXT("/Game/")) ||
				TargetPackagePath.StartsWith(TEXT("/Engine/")) ||
				TargetPackagePath.StartsWith(TEXT("/Plugin/"));
		}

		void AddObjectReferenceRecord(
			TArray<FReferenceRecord>& Records,
			UObject* SourceObject,
			const FString& SourceKind,
			const FString& PropertyPath,
			const FString& ReferenceType,
			UObject* TargetObject)
		{
			if (!SourceObject || !TargetObject)
			{
				return;
			}

			FReferenceRecord Record;
			Record.SourceKind = SourceKind;
			Record.SourcePath = SourceObject->GetPathName();
			Record.SourceClassPath = SourceObject->GetClass() ? SourceObject->GetClass()->GetPathName() : FString();
			Record.PropertyPath = PropertyPath;
			Record.ReferenceType = ReferenceType;
			Record.TargetPath = TargetObject->GetPathName();
			Record.TargetPackagePath = TargetObject->GetPackage() ? TargetObject->GetPackage()->GetName() : FString();
			Record.TargetClassPath = TargetObject->GetClass() ? TargetObject->GetClass()->GetPathName() : FString();
			Record.bExternalAsset = IsExternalAssetPackage(SourceObject->GetPackage() ? SourceObject->GetPackage()->GetName() : FString(), Record.TargetPackagePath);
			Records.Add(MoveTemp(Record));
		}

		void AddSoftObjectReferenceRecord(
			TArray<FReferenceRecord>& Records,
			UObject* SourceObject,
			const FString& SourceKind,
			const FString& PropertyPath,
			const FSoftObjectPath& TargetPath)
		{
			if (!SourceObject || TargetPath.IsNull())
			{
				return;
			}

			FReferenceRecord Record;
			Record.SourceKind = SourceKind;
			Record.SourcePath = SourceObject->GetPathName();
			Record.SourceClassPath = SourceObject->GetClass() ? SourceObject->GetClass()->GetPathName() : FString();
			Record.PropertyPath = PropertyPath;
			Record.ReferenceType = TEXT("soft_object");
			Record.TargetPath = TargetPath.ToString();
			Record.TargetPackagePath = TargetPath.GetAssetPathString();
			Record.TargetClassPath = FString();
			Record.bExternalAsset = IsExternalAssetPackage(SourceObject->GetPackage() ? SourceObject->GetPackage()->GetName() : FString(), Record.TargetPackagePath);
			Records.Add(MoveTemp(Record));
		}

		void ScanPropertyReferences(
			TArray<FReferenceRecord>& Records,
			UObject* SourceObject,
			const FString& SourceKind,
			FProperty* Property,
			const void* ValuePtr,
			const FString& PropertyPath,
			int32 Depth)
		{
			if (!SourceObject || !Property || !ValuePtr || Depth > 4)
			{
				return;
			}

			if (FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(Property))
			{
				AddSoftObjectReferenceRecord(Records, SourceObject, SourceKind, PropertyPath, SoftObjectProperty->GetPropertyValue(ValuePtr).ToSoftObjectPath());
				return;
			}

			if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
			{
				AddObjectReferenceRecord(Records, SourceObject, SourceKind, PropertyPath, TEXT("object"), ObjectProperty->GetObjectPropertyValue(ValuePtr));
				return;
			}

			if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
			{
				if (!StructProperty->Struct)
				{
					return;
				}
				for (TFieldIterator<FProperty> It(StructProperty->Struct, EFieldIterationFlags::IncludeSuper); It; ++It)
				{
					FProperty* Inner = *It;
					ScanPropertyReferences(
						Records,
						SourceObject,
						SourceKind,
						Inner,
						Inner->ContainerPtrToValuePtr<void>(ValuePtr),
						PropertyPath + TEXT(".") + Inner->GetName(),
						Depth + 1);
				}
				return;
			}

			if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
			{
				FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
				const int32 Limit = FMath::Min(Helper.Num(), 64);
				for (int32 Index = 0; Index < Limit; ++Index)
				{
					ScanPropertyReferences(
						Records,
						SourceObject,
						SourceKind,
						ArrayProperty->Inner,
						Helper.GetRawPtr(Index),
						FString::Printf(TEXT("%s[%d]"), *PropertyPath, Index),
						Depth + 1);
				}
				return;
			}

			if (FSetProperty* SetProperty = CastField<FSetProperty>(Property))
			{
				FScriptSetHelper Helper(SetProperty, ValuePtr);
				int32 Added = 0;
				for (int32 Index = 0; Index < Helper.GetMaxIndex() && Added < 64; ++Index)
				{
					if (!Helper.IsValidIndex(Index))
					{
						continue;
					}
					ScanPropertyReferences(
						Records,
						SourceObject,
						SourceKind,
						SetProperty->ElementProp,
						Helper.GetElementPtr(Index),
						FString::Printf(TEXT("%s[%d]"), *PropertyPath, Added),
						Depth + 1);
					++Added;
				}
				return;
			}

			if (FMapProperty* MapProperty = CastField<FMapProperty>(Property))
			{
				FScriptMapHelper Helper(MapProperty, ValuePtr);
				int32 Added = 0;
				for (int32 Index = 0; Index < Helper.GetMaxIndex() && Added < 64; ++Index)
				{
					if (!Helper.IsValidIndex(Index))
					{
						continue;
					}
					const FString EntryPath = FString::Printf(TEXT("%s[%d]"), *PropertyPath, Added);
					ScanPropertyReferences(Records, SourceObject, SourceKind, MapProperty->KeyProp, Helper.GetKeyPtr(Index), EntryPath + TEXT(".key"), Depth + 1);
					ScanPropertyReferences(Records, SourceObject, SourceKind, MapProperty->ValueProp, Helper.GetValuePtr(Index), EntryPath + TEXT(".value"), Depth + 1);
					++Added;
				}
			}
		}

		void ScanObjectReferences(TArray<FReferenceRecord>& Records, UObject* SourceObject, const FString& SourceKind)
		{
			if (!SourceObject || !SourceObject->GetClass())
			{
				return;
			}

			for (TFieldIterator<FProperty> It(SourceObject->GetClass(), EFieldIterationFlags::IncludeSuper); It; ++It)
			{
				FProperty* Property = *It;
				if (!Property || Property->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient | CPF_TextExportTransient))
				{
					continue;
				}
				ScanPropertyReferences(Records, SourceObject, SourceKind, Property, Property->ContainerPtrToValuePtr<void>(SourceObject), Property->GetName(), 0);
			}
		}

		TArray<FReferenceRecord> CollectReferenceRecords(UWorld* World)
		{
			TArray<FReferenceRecord> Records;
			if (!World)
			{
				return Records;
			}

			ScanObjectReferences(Records, World->GetWorldSettings(false, false), TEXT("world_settings"));
			if (World->PersistentLevel)
			{
				ScanObjectReferences(Records, World->PersistentLevel->GetLevelScriptBlueprint(true), TEXT("level_blueprint"));
			}

			for (AActor* Actor : GetExportableActors(World))
			{
				ScanObjectReferences(Records, Actor, TEXT("actor"));
				for (UActorComponent* Component : GetExportableComponents(Actor))
				{
					ScanObjectReferences(Records, Component, TEXT("component"));
				}
			}

			Records.Sort([](const FReferenceRecord& A, const FReferenceRecord& B)
			{
				if (A.SourcePath != B.SourcePath)
				{
					return A.SourcePath < B.SourcePath;
				}
				if (A.PropertyPath != B.PropertyPath)
				{
					return A.PropertyPath < B.PropertyPath;
				}
				return A.TargetPath < B.TargetPath;
			});
			return Records;
		}

		TSharedPtr<FJsonObject> MakeReferenceRecordJson(const FReferenceRecord& Record)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("source_kind"), Record.SourceKind);
			Obj->SetStringField(TEXT("source_path"), Record.SourcePath);
			Obj->SetStringField(TEXT("source_class_path"), Record.SourceClassPath);
			Obj->SetStringField(TEXT("property_path"), Record.PropertyPath);
			Obj->SetStringField(TEXT("reference_type"), Record.ReferenceType);
			Obj->SetStringField(TEXT("target_path"), Record.TargetPath);
			Obj->SetStringField(TEXT("target_package_path"), Record.TargetPackagePath);
			Obj->SetStringField(TEXT("target_class_path"), Record.TargetClassPath);
			Obj->SetBoolField(TEXT("external_asset"), Record.bExternalAsset);
			return Obj;
		}

		bool WriteJsonFile(const FString& FilePath, const TSharedPtr<FJsonObject>& Object, FString& OutError)
		{
			return FUeAgentStructuredJsonFormatter::SaveObjectToFile(FilePath, Object, &OutError);
		}

		bool WriteTextFile(const FString& FilePath, const FString& Text, FString& OutError)
		{
			if (!FFileHelper::SaveStringToFile(Text, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				OutError = FString::Printf(TEXT("write_failed:%s"), *FilePath);
				return false;
			}
			return true;
		}

		int32 CountActors(UWorld* World)
		{
			if (!World || !World->PersistentLevel)
			{
				return 0;
			}
			int32 Count = 0;
			for (AActor* Actor : World->PersistentLevel->Actors)
			{
				if (IsValid(Actor))
				{
					++Count;
				}
			}
			return Count;
		}

		TArray<AActor*> GetExportableActors(UWorld* World)
		{
			TArray<AActor*> Actors;
			if (!World || !World->PersistentLevel)
			{
				return Actors;
			}
			for (AActor* Actor : World->PersistentLevel->Actors)
			{
				if (IsValid(Actor))
				{
					Actors.Add(Actor);
				}
			}
			Actors.Sort([](const AActor& A, const AActor& B)
			{
				return A.GetPathName() < B.GetPathName();
			});
			return Actors;
		}

		FString MakeActorId(AActor* Actor)
		{
			return SafeName(Actor ? Actor->GetPathName() : FString());
		}

		FString MakeComponentId(UActorComponent* Component)
		{
			return SafeName(Component ? Component->GetPathName() : FString());
		}

		TArray<UActorComponent*> GetExportableComponents(AActor* Actor)
		{
			TArray<UActorComponent*> Components;
			if (!Actor)
			{
				return Components;
			}
			Actor->GetComponents(Components);
			Components.RemoveAll([](UActorComponent* Component)
			{
				return !IsValid(Component);
			});
			Components.Sort([](const UActorComponent& A, const UActorComponent& B)
			{
				return A.GetPathName() < B.GetPathName();
			});
			return Components;
		}

		void SplitFolderPath(const FString& FolderPath, TArray<FString>& OutParts)
		{
			OutParts.Reset();
			if (!FolderPath.IsEmpty())
			{
				FolderPath.ParseIntoArray(OutParts, TEXT("/"), true);
			}
		}

		FString NormalizeFolderPath(const FName& FolderPath)
		{
			const FString Path = FolderPath.ToString();
			return Path == TEXT("None") ? FString() : Path;
		}

		TSharedPtr<FJsonObject> MakeActorIndexJson(UWorld* World)
		{
			const TArray<AActor*> Actors = GetExportableActors(World);
			TArray<TSharedPtr<FJsonValue>> ActorValues;
			ActorValues.Reserve(Actors.Num());
			for (AActor* Actor : Actors)
			{
				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				const FString FolderPath = NormalizeFolderPath(Actor->GetFolderPath());
				Item->SetStringField(TEXT("actor_id"), MakeActorId(Actor));
				Item->SetStringField(TEXT("name"), Actor->GetName());
				Item->SetStringField(TEXT("label"), Actor->GetActorLabel());
				Item->SetStringField(TEXT("path"), Actor->GetClass() ? Actor->GetClass()->GetPathName() : FString());
				Item->SetStringField(TEXT("folder_path"), FolderPath);
				Item->SetStringField(TEXT("folder_key"), FolderPath.IsEmpty() ? TEXT("__root__") : FolderPath);
				ActorValues.Add(MakeShared<FJsonValueObject>(Item));
			}

			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("schema"), LevelFolderSchema);
			Obj->SetStringField(TEXT("kind"), TEXT("actor_index"));
			Obj->SetNumberField(TEXT("actor_count"), Actors.Num());
			Obj->SetArrayField(TEXT("actors"), ActorValues);
			return Obj;
		}

		TSharedPtr<FJsonObject> MakeRootIndexJson(UWorld* World)
		{
			auto MakeSection = [](const FString& Name, const FString& Path, const FString& Purpose)
			{
				TSharedPtr<FJsonObject> Section = MakeShared<FJsonObject>();
				Section->SetStringField(TEXT("name"), Name);
				Section->SetStringField(TEXT("path"), Path);
				Section->SetStringField(TEXT("purpose"), Purpose);
				return MakeShared<FJsonValueObject>(Section);
			};

			TArray<TSharedPtr<FJsonValue>> Sections;
			Sections.Add(MakeSection(TEXT("level"), TEXT("level.json"), TEXT("level identity and summary")));
			Sections.Add(MakeSection(TEXT("world"), TEXT("world.json"), TEXT("world summary")));
			Sections.Add(MakeSection(TEXT("world_settings"), TEXT("world_settings/summary.json"), TEXT("world settings details index")));
			Sections.Add(MakeSection(TEXT("streaming"), TEXT("streaming/index.json"), TEXT("streaming level summary")));
			Sections.Add(MakeSection(TEXT("actors"), TEXT("actors/index.json"), TEXT("actor collection summary")));

			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("schema"), LevelFolderSchema);
			Obj->SetStringField(TEXT("kind"), TEXT("level_index"));
			Obj->SetStringField(TEXT("level_package"), (World && World->GetOutermost()) ? World->GetOutermost()->GetName() : FString());
			Obj->SetNumberField(TEXT("actor_count"), CountActors(World));
			Obj->SetArrayField(TEXT("sections"), Sections);
			return Obj;
		}

		TSharedPtr<FJsonObject> MakeActorSummaryJson(
			AActor* Actor,
			const TSharedPtr<FJsonObject>& Properties,
			bool bSplitProperties,
			const TSharedPtr<FJsonObject>& DetailsSummary)
		{
			const FString FolderPath = Actor ? NormalizeFolderPath(Actor->GetFolderPath()) : FString();
			const TArray<UActorComponent*> Components = GetExportableComponents(Actor);
			TArray<TSharedPtr<FJsonValue>> ComponentIdValues;
			ComponentIdValues.Reserve(Components.Num());
			for (UActorComponent* Component : Components)
			{
				ComponentIdValues.Add(StringValue(MakeComponentId(Component)));
			}

			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("schema"), LevelFolderSchema);
			Obj->SetStringField(TEXT("kind"), TEXT("actor"));
			Obj->SetStringField(TEXT("actor_id"), MakeActorId(Actor));
			Obj->SetStringField(TEXT("name"), Actor ? Actor->GetName() : FString());
			Obj->SetStringField(TEXT("label"), Actor ? Actor->GetActorLabel() : FString());
			Obj->SetStringField(TEXT("path"), (Actor && Actor->GetClass()) ? Actor->GetClass()->GetPathName() : FString());
			Obj->SetObjectField(TEXT("transform"), MakeActorTransformJson(Actor));
			if (!FolderPath.IsEmpty())
			{
				Obj->SetStringField(TEXT("folder_path"), FolderPath);
			}
			TArray<TSharedPtr<FJsonValue>> Tags = MakeTagsJson(Actor);
			if (Tags.Num() > 0)
			{
				Obj->SetArrayField(TEXT("tags"), Tags);
			}
			if (bSplitProperties)
			{
				Obj->SetStringField(TEXT("properties_file"), TEXT("properties.json"));
			}
			else if (DetailsSummary.IsValid())
			{
				Obj->SetObjectField(TEXT("details"), DetailsSummary);
			}
			Obj->SetNumberField(TEXT("component_count"), Components.Num());
			Obj->SetArrayField(TEXT("component_ids"), ComponentIdValues);
			if (bSplitProperties || !DetailsSummary.IsValid())
			{
				InlineProperties(Obj, Properties);
			}
			return Obj;
		}

		TSharedPtr<FJsonObject> MakeComponentSummaryJson(
			AActor* OwnerActor,
			UActorComponent* Component,
			const TSharedPtr<FJsonObject>& Properties,
			bool bSplitProperties,
			const TSharedPtr<FJsonObject>& DetailsSummary)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("schema"), LevelFolderSchema);
			Obj->SetStringField(TEXT("kind"), TEXT("component"));
			Obj->SetStringField(TEXT("component_id"), MakeComponentId(Component));
			Obj->SetStringField(TEXT("name"), Component ? Component->GetName() : FString());
			Obj->SetStringField(TEXT("path"), (Component && Component->GetClass()) ? Component->GetClass()->GetPathName() : FString());
			Obj->SetStringField(TEXT("owner_actor_id"), MakeActorId(OwnerActor));
			Obj->SetObjectField(TEXT("attachment"), MakeSceneAttachmentJson(Component));
			if (bSplitProperties)
			{
				Obj->SetStringField(TEXT("properties_file"), TEXT("properties.json"));
			}
			else if (DetailsSummary.IsValid())
			{
				Obj->SetObjectField(TEXT("details"), DetailsSummary);
			}
			if (bSplitProperties || !DetailsSummary.IsValid())
			{
				InlineProperties(Obj, Properties);
			}
			return Obj;
		}

		TArray<FString> MakeActorPackageRelativeFiles(UWorld* World, const FExportOptions& Options)
		{
			TArray<FString> Files;
			const bool bSplitProperties = ShouldSplitProperties(Options);
			const bool bGroupedDetails = ShouldWriteGroupedDetails(Options);
			const TArray<AActor*> Actors = GetExportableActors(World);
			for (AActor* Actor : Actors)
			{
				const FString ItemRoot = TEXT("actors/items") / MakeActorId(Actor);
				Files.Add(ItemRoot / TEXT("actor.json"));
				if (bSplitProperties)
				{
					Files.Add(ItemRoot / TEXT("properties.json"));
				}
				if (bGroupedDetails)
				{
					Files.Add(ItemRoot / TEXT("details") / TEXT("index.json"));
				}
				for (UActorComponent* Component : GetExportableComponents(Actor))
				{
					const FString ComponentRoot = ItemRoot / TEXT("components") / MakeComponentId(Component);
					Files.Add(ComponentRoot / TEXT("component.json"));
					if (bSplitProperties)
					{
						Files.Add(ComponentRoot / TEXT("properties.json"));
					}
					if (bGroupedDetails)
					{
						Files.Add(ComponentRoot / TEXT("details") / TEXT("index.json"));
					}
				}
			}
			return Files;
		}

		TArray<FString> MakeActorPackageRelativeDirectories(UWorld* World, const FExportOptions& Options)
		{
			TArray<FString> Directories;
			const bool bGroupedDetails = ShouldWriteGroupedDetails(Options);
			const TArray<AActor*> Actors = GetExportableActors(World);
			for (AActor* Actor : Actors)
			{
				const FString ActorRoot = TEXT("actors/items") / MakeActorId(Actor);
				if (bGroupedDetails)
				{
					Directories.Add(ActorRoot / TEXT("details"));
				}
				const FString ComponentsRoot = ActorRoot / TEXT("components");
				Directories.Add(ComponentsRoot);
				for (UActorComponent* Component : GetExportableComponents(Actor))
				{
					const FString ComponentRoot = ComponentsRoot / MakeComponentId(Component);
					Directories.Add(ComponentRoot);
					if (bGroupedDetails)
					{
						Directories.Add(ComponentRoot / TEXT("details"));
					}
				}
			}
			return Directories;
		}

		bool WriteActorPackages(const FString& Root, UWorld* World, const FExportOptions& Options, FString& OutError)
		{
			const TArray<AActor*> Actors = GetExportableActors(World);
			const bool bSplitProperties = ShouldSplitProperties(Options);
			const bool bGroupedDetails = ShouldWriteGroupedDetails(Options);
			UeAgentUObjectProperties::FExportOptions PropertyOptions;
			PropertyOptions.Profile = UeAgentUObjectProperties::EExportProfile::Authoring;

			for (AActor* Actor : Actors)
			{
				const FString ActorRoot = Root / TEXT("actors") / TEXT("items") / MakeActorId(Actor);
				if (!IFileManager::Get().MakeDirectory(*ActorRoot, true))
				{
					OutError = FString::Printf(TEXT("actor_package_directory_create_failed:%s"), *ActorRoot);
					return false;
				}
				const FString ComponentsRoot = ActorRoot / TEXT("components");
				if (!IFileManager::Get().MakeDirectory(*ComponentsRoot, true))
				{
					OutError = FString::Printf(TEXT("component_package_root_directory_create_failed:%s"), *ComponentsRoot);
					return false;
				}

				TSharedPtr<FJsonObject> Properties = bSplitProperties ? UeAgentUObjectProperties::ExportActorProperties(Actor, PropertyOptions) : nullptr;
				TSharedPtr<FJsonObject> ActorDetailsSummary;
				if (bGroupedDetails &&
					!ExportOwnerGroupedDetails(Actor, TEXT("actor"), MakeActorId(Actor), ActorRoot / TEXT("details"), TEXT("level_export_folder"), PropertyOptions, ActorDetailsSummary, OutError))
				{
					return false;
				}
				if (!WriteJsonFile(ActorRoot / TEXT("actor.json"), MakeActorSummaryJson(Actor, Properties, bSplitProperties, ActorDetailsSummary), OutError))
				{
					return false;
				}
				if (bSplitProperties && !WriteJsonFile(ActorRoot / TEXT("properties.json"), Properties, OutError))
				{
					return false;
				}

				for (UActorComponent* Component : GetExportableComponents(Actor))
				{
					const FString ComponentRoot = ComponentsRoot / MakeComponentId(Component);
					if (!IFileManager::Get().MakeDirectory(*ComponentRoot, true))
					{
						OutError = FString::Printf(TEXT("component_package_directory_create_failed:%s"), *ComponentRoot);
						return false;
					}

					TSharedPtr<FJsonObject> ComponentProperties = bSplitProperties ? UeAgentUObjectProperties::ExportComponentProperties(Component, PropertyOptions) : nullptr;
					TSharedPtr<FJsonObject> ComponentDetailsSummary;
					if (bGroupedDetails &&
						!ExportOwnerGroupedDetails(Component, TEXT("component"), MakeComponentId(Component), ComponentRoot / TEXT("details"), TEXT("level_export_folder"), PropertyOptions, ComponentDetailsSummary, OutError))
					{
						return false;
					}
					if (!WriteJsonFile(ComponentRoot / TEXT("component.json"), MakeComponentSummaryJson(Actor, Component, ComponentProperties, bSplitProperties, ComponentDetailsSummary), OutError))
					{
						return false;
					}
					if (bSplitProperties && !WriteJsonFile(ComponentRoot / TEXT("properties.json"), ComponentProperties, OutError))
					{
						return false;
					}
				}
			}
			return true;
		}

		TSharedPtr<FJsonObject> MakeFoldersJson(UWorld* World)
		{
			const TArray<AActor*> Actors = GetExportableActors(World);
			TMap<FString, int32> ActorCountsByFolder;
			TSet<FString> FolderPaths;
			int32 RootActorCount = 0;

			for (AActor* Actor : Actors)
			{
				const FString FolderPath = NormalizeFolderPath(Actor->GetFolderPath());
				if (FolderPath.IsEmpty())
				{
					++RootActorCount;
					continue;
				}

				ActorCountsByFolder.FindOrAdd(FolderPath)++;
				TArray<FString> Parts;
				SplitFolderPath(FolderPath, Parts);
				FString Current;
				for (const FString& Part : Parts)
				{
					Current = Current.IsEmpty() ? Part : (Current / Part);
					FolderPaths.Add(Current);
				}
			}

			TArray<FString> SortedFolders = FolderPaths.Array();
			SortedFolders.Sort();

			TArray<TSharedPtr<FJsonValue>> FolderValues;
			FolderValues.Reserve(SortedFolders.Num());
			for (const FString& FolderPath : SortedFolders)
			{
				TArray<FString> Parts;
				SplitFolderPath(FolderPath, Parts);
				FString ParentPath;
				if (Parts.Num() > 1)
				{
					TArray<FString> ParentParts = Parts;
					ParentParts.Pop();
					ParentPath = FString::Join(ParentParts, TEXT("/"));
				}

				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("path"), FolderPath);
				Item->SetStringField(TEXT("name"), Parts.Num() > 0 ? Parts.Last() : FolderPath);
				Item->SetStringField(TEXT("parent_path"), ParentPath);
				Item->SetNumberField(TEXT("depth"), Parts.Num());
				Item->SetNumberField(TEXT("direct_actor_count"), ActorCountsByFolder.FindRef(FolderPath));
				FolderValues.Add(MakeShared<FJsonValueObject>(Item));
			}

			TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("key"), TEXT("__root__"));
			Root->SetStringField(TEXT("path"), FString());
			Root->SetNumberField(TEXT("direct_actor_count"), RootActorCount);

			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("schema"), LevelFolderSchema);
			Obj->SetStringField(TEXT("kind"), TEXT("outliner_folders"));
			Obj->SetObjectField(TEXT("root"), Root);
			Obj->SetNumberField(TEXT("folder_count"), SortedFolders.Num());
			Obj->SetArrayField(TEXT("folders"), FolderValues);
			return Obj;
		}

		TSharedPtr<FJsonObject> MakeLevelJson(UWorld* World)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("schema"), LevelFolderSchema);
			Obj->SetStringField(TEXT("kind"), TEXT("level"));
			Obj->SetStringField(TEXT("map_name"), World ? World->GetMapName() : FString());
			Obj->SetStringField(TEXT("package_name"), (World && World->GetOutermost()) ? World->GetOutermost()->GetName() : FString());
			Obj->SetBoolField(TEXT("dirty"), (World && World->GetOutermost()) ? World->GetOutermost()->IsDirty() : false);
			Obj->SetNumberField(TEXT("actor_count"), CountActors(World));
			return Obj;
		}

		TSharedPtr<FJsonObject> MakeWorldJson(UWorld* World)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("schema"), LevelFolderSchema);
			Obj->SetStringField(TEXT("kind"), TEXT("world"));
			Obj->SetStringField(TEXT("name"), World ? World->GetName() : FString());
			Obj->SetStringField(TEXT("world_type"), World ? WorldTypeToString(World->WorldType) : TEXT("none"));
			Obj->SetStringField(TEXT("persistent_level_package"), (World && World->PersistentLevel && World->PersistentLevel->GetOutermost()) ? World->PersistentLevel->GetOutermost()->GetName() : FString());
			Obj->SetNumberField(TEXT("streaming_level_count"), World ? World->GetStreamingLevels().Num() : 0);
			return Obj;
		}

		TSharedPtr<FJsonObject> MakeStreamingIndexJson(UWorld* World)
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			const TArray<ULevelStreaming*> StreamingLevels = World ? World->GetStreamingLevels() : TArray<ULevelStreaming*>();
			Items.Reserve(StreamingLevels.Num());
			for (ULevelStreaming* StreamingLevel : StreamingLevels)
			{
				if (!IsValid(StreamingLevel))
				{
					continue;
				}

				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				ULevel* LoadedLevel = StreamingLevel->GetLoadedLevel();
				Item->SetStringField(TEXT("streaming_id"), SafeName(StreamingLevel->GetPathName()));
				Item->SetStringField(TEXT("name"), StreamingLevel->GetName());
				Item->SetStringField(TEXT("class_path"), StreamingLevel->GetClass() ? StreamingLevel->GetClass()->GetPathName() : FString());
				Item->SetStringField(TEXT("object_path"), StreamingLevel->GetPathName());
				Item->SetStringField(TEXT("world_asset_package_name"), StreamingLevel->GetWorldAssetPackageName());
				Item->SetStringField(TEXT("world_asset_package_f_name"), StreamingLevel->GetWorldAssetPackageFName().ToString());
				Item->SetStringField(TEXT("package_name_to_load"), StreamingLevel->PackageNameToLoad.ToString());
				Item->SetStringField(TEXT("loaded_level_object_path"), LoadedLevel ? LoadedLevel->GetPathName() : FString());
				Item->SetBoolField(TEXT("should_be_loaded"), StreamingLevel->ShouldBeLoaded());
				Item->SetBoolField(TEXT("should_be_visible"), StreamingLevel->ShouldBeVisible());
				Item->SetBoolField(TEXT("is_level_loaded"), LoadedLevel != nullptr);
				Item->SetNumberField(TEXT("state"), static_cast<int32>(StreamingLevel->GetLevelStreamingState()));
#if WITH_EDITOR
				Item->SetBoolField(TEXT("should_be_visible_in_editor"), StreamingLevel->GetShouldBeVisibleInEditor());
#endif
				Items.Add(MakeShared<FJsonValueObject>(Item));
			}

			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("schema"), LevelFolderSchema);
			Obj->SetStringField(TEXT("kind"), TEXT("streaming_index"));
			Obj->SetNumberField(TEXT("streaming_level_count"), Items.Num());
			Obj->SetBoolField(TEXT("child_level_actors_inlined"), false);
			Obj->SetArrayField(TEXT("streaming_levels"), Items);
			return Obj;
		}

		TSharedPtr<FJsonObject> MakeDataLayersJson(UWorld* World)
		{
			AWorldDataLayers* WorldDataLayers = World ? World->GetWorldDataLayers() : nullptr;
			UDataLayerManager* DataLayerManager = World ? World->GetDataLayerManager() : nullptr;

			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("schema"), LevelFolderSchema);
			Obj->SetStringField(TEXT("kind"), TEXT("data_layers"));
			Obj->SetObjectField(TEXT("world_data_layers"), MakeObjectReferenceJson(WorldDataLayers));
			Obj->SetObjectField(TEXT("data_layer_manager"), MakeObjectReferenceJson(DataLayerManager));
			Obj->SetBoolField(TEXT("deep_layer_items_exported"), false);
			Obj->SetStringField(TEXT("deep_export_note"), TEXT("not_inlined; use dedicated data layer export when item-level authoring is needed"));
			return Obj;
		}

		TSharedPtr<FJsonObject> MakeWorldPartitionJson(UWorld* World)
		{
			UWorldPartition* WorldPartition = World ? World->GetWorldPartition() : nullptr;

			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("schema"), LevelFolderSchema);
			Obj->SetStringField(TEXT("kind"), TEXT("world_partition"));
			Obj->SetObjectField(TEXT("world_partition"), MakeObjectReferenceJson(WorldPartition));
			Obj->SetBoolField(TEXT("is_partitioned_world"), World ? World->IsPartitionedWorld() : false);
			Obj->SetBoolField(TEXT("supports_streaming"), WorldPartition ? WorldPartition->SupportsStreaming() : false);
			Obj->SetBoolField(TEXT("is_streaming_enabled"), WorldPartition ? WorldPartition->IsStreamingEnabled() : false);
			Obj->SetBoolField(TEXT("can_stream"), WorldPartition ? WorldPartition->CanStream() : false);
			Obj->SetStringField(TEXT("editor_name"), WorldPartition ? WorldPartition->GetWorldPartitionEditorName().ToString() : FString());
			Obj->SetBoolField(TEXT("runtime_cells_inlined"), false);
			Obj->SetStringField(TEXT("runtime_cells_note"), TEXT("not_inlined; this summary only records world partition state"));
			return Obj;
		}

		TSharedPtr<FJsonObject> MakeHlodJson(UWorld* World)
		{
			TArray<TSharedPtr<FJsonValue>> HlodActors;
			for (AActor* Actor : GetExportableActors(World))
			{
				if (!Actor || !Actor->IsA<AWorldPartitionHLOD>())
				{
					continue;
				}

				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("actor_id"), MakeActorId(Actor));
				Item->SetStringField(TEXT("name"), Actor->GetName());
				Item->SetStringField(TEXT("label"), Actor->GetActorLabel());
				Item->SetStringField(TEXT("class_path"), Actor->GetClass() ? Actor->GetClass()->GetPathName() : FString());
				Item->SetStringField(TEXT("object_path"), Actor->GetPathName());
				HlodActors.Add(MakeShared<FJsonValueObject>(Item));
			}

			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("schema"), LevelFolderSchema);
			Obj->SetStringField(TEXT("kind"), TEXT("hlod"));
			Obj->SetNumberField(TEXT("hlod_actor_count"), HlodActors.Num());
			Obj->SetBoolField(TEXT("proxy_mesh_inlined"), false);
			Obj->SetArrayField(TEXT("hlod_actors"), HlodActors);
			return Obj;
		}

		TSharedPtr<FJsonObject> MakeReferencesAssetsJson(UWorld* World)
		{
			const TArray<FReferenceRecord> Records = CollectReferenceRecords(World);
			TArray<TSharedPtr<FJsonValue>> Items;
			Items.Reserve(Records.Num());
			for (const FReferenceRecord& Record : Records)
			{
				Items.Add(MakeShared<FJsonValueObject>(MakeReferenceRecordJson(Record)));
			}

			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("schema"), LevelFolderSchema);
			Obj->SetStringField(TEXT("kind"), TEXT("asset_references"));
			Obj->SetNumberField(TEXT("reference_count"), Records.Num());
			Obj->SetBoolField(TEXT("target_assets_inlined"), false);
			Obj->SetArrayField(TEXT("references"), Items);
			return Obj;
		}

		TSharedPtr<FJsonObject> MakeExternalAssetsJson(UWorld* World)
		{
			const TArray<FReferenceRecord> Records = CollectReferenceRecords(World);
			TMap<FString, TSharedPtr<FJsonObject>> AssetsByPackage;
			for (const FReferenceRecord& Record : Records)
			{
				if (!Record.bExternalAsset || Record.TargetPackagePath.IsEmpty())
				{
					continue;
				}

				TSharedPtr<FJsonObject>* Existing = AssetsByPackage.Find(Record.TargetPackagePath);
				if (!Existing)
				{
					TSharedPtr<FJsonObject> Asset = MakeShared<FJsonObject>();
					Asset->SetStringField(TEXT("target_package_path"), Record.TargetPackagePath);
					Asset->SetStringField(TEXT("sample_target_path"), Record.TargetPath);
					Asset->SetStringField(TEXT("sample_target_class_path"), Record.TargetClassPath);
					Asset->SetNumberField(TEXT("reference_count"), 1);
					AssetsByPackage.Add(Record.TargetPackagePath, Asset);
				}
				else
				{
					(*Existing)->SetNumberField(TEXT("reference_count"), (*Existing)->GetNumberField(TEXT("reference_count")) + 1);
				}
			}

			TArray<FString> SortedPackages;
			AssetsByPackage.GetKeys(SortedPackages);
			SortedPackages.Sort();

			TArray<TSharedPtr<FJsonValue>> Assets;
			Assets.Reserve(SortedPackages.Num());
			for (const FString& PackagePath : SortedPackages)
			{
				Assets.Add(MakeShared<FJsonValueObject>(AssetsByPackage[PackagePath]));
			}

			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("schema"), LevelFolderSchema);
			Obj->SetStringField(TEXT("kind"), TEXT("external_assets"));
			Obj->SetNumberField(TEXT("asset_count"), Assets.Num());
			Obj->SetBoolField(TEXT("target_assets_inlined"), false);
			Obj->SetArrayField(TEXT("assets"), Assets);
			return Obj;
		}

		TSharedPtr<FJsonObject> MakeWorldSettingsSummaryJson(
			UWorld* World,
			bool bSplitProperties,
			const TSharedPtr<FJsonObject>& DetailsSummary)
		{
			AWorldSettings* WorldSettings = World ? World->GetWorldSettings(false, false) : nullptr;
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("schema"), LevelFolderSchema);
			Obj->SetStringField(TEXT("kind"), TEXT("world_settings"));
			Obj->SetBoolField(TEXT("available"), WorldSettings != nullptr);
			Obj->SetStringField(TEXT("name"), WorldSettings ? WorldSettings->GetName() : FString());
			Obj->SetStringField(TEXT("class_path"), (WorldSettings && WorldSettings->GetClass()) ? WorldSettings->GetClass()->GetPathName() : FString());
			Obj->SetStringField(TEXT("object_path"), WorldSettings ? WorldSettings->GetPathName() : FString());
			if (WorldSettings && bSplitProperties)
			{
				Obj->SetStringField(TEXT("properties_file"), TEXT("properties.json"));
			}
			else if (WorldSettings && DetailsSummary.IsValid())
			{
				Obj->SetObjectField(TEXT("details"), DetailsSummary);
			}
			return Obj;
		}

		TSharedPtr<FJsonObject> MakeEditorSummaryJson(UWorld* World)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("schema"), LevelFolderSchema);
			Obj->SetStringField(TEXT("kind"), TEXT("editor_summary"));
			Obj->SetStringField(TEXT("world_type"), World ? WorldTypeToString(World->WorldType) : TEXT("none"));
			Obj->SetBoolField(TEXT("is_editor_world"), World ? (World->WorldType == EWorldType::Editor) : false);
			Obj->SetBoolField(TEXT("is_pie_world"), World ? (World->WorldType == EWorldType::PIE) : false);
			Obj->SetBoolField(TEXT("package_dirty"), (World && World->GetOutermost()) ? World->GetOutermost()->IsDirty() : false);
			Obj->SetStringField(TEXT("map_name"), World ? World->GetMapName() : FString());
			Obj->SetStringField(TEXT("package_name"), (World && World->GetOutermost()) ? World->GetOutermost()->GetName() : FString());
			return Obj;
		}

		TSharedPtr<FJsonObject> MakeLevelBlueprintSummaryJson(UWorld* World)
		{
			ULevel* PersistentLevel = World ? World->PersistentLevel : nullptr;
			ULevelScriptBlueprint* Blueprint = PersistentLevel ? PersistentLevel->GetLevelScriptBlueprint(true) : nullptr;
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("schema"), LevelFolderSchema);
			Obj->SetStringField(TEXT("kind"), TEXT("level_blueprint_summary"));
			Obj->SetBoolField(TEXT("available"), Blueprint != nullptr);
			Obj->SetStringField(TEXT("name"), Blueprint ? Blueprint->GetName() : FString());
			Obj->SetStringField(TEXT("class_path"), (Blueprint && Blueprint->GetClass()) ? Blueprint->GetClass()->GetPathName() : FString());
			Obj->SetStringField(TEXT("object_path"), Blueprint ? Blueprint->GetPathName() : FString());
			Obj->SetStringField(TEXT("generated_class_path"), (Blueprint && Blueprint->GeneratedClass) ? Blueprint->GeneratedClass->GetPathName() : FString());
			Obj->SetBoolField(TEXT("graph_inline_exported"), false);
			Obj->SetStringField(TEXT("graph_export_note"), TEXT("not_inlined; use dedicated blueprint graph export command when needed"));
			return Obj;
		}

		bool WriteWorldSettingsPackage(const FString& Root, UWorld* World, const FExportOptions& Options, FString& OutError)
		{
			AWorldSettings* WorldSettings = World ? World->GetWorldSettings(false, false) : nullptr;
			const bool bSplitProperties = ShouldSplitProperties(Options);
			const bool bGroupedDetails = ShouldWriteGroupedDetails(Options);
			TSharedPtr<FJsonObject> DetailsSummary;
			if (!WorldSettings)
			{
				return WriteJsonFile(Root / TEXT("world_settings") / TEXT("summary.json"), MakeWorldSettingsSummaryJson(World, bSplitProperties, DetailsSummary), OutError);
			}

			UeAgentUObjectProperties::FExportOptions PropertyOptions;
			PropertyOptions.Profile = UeAgentUObjectProperties::EExportProfile::Authoring;
			TSharedPtr<FJsonObject> Properties = bSplitProperties ? UeAgentUObjectProperties::ExportObjectProperties(WorldSettings, PropertyOptions) : nullptr;
			if (bGroupedDetails &&
				!ExportOwnerGroupedDetails(WorldSettings, TEXT("world_settings"), TEXT("world_settings"), Root / TEXT("world_settings") / TEXT("details"), TEXT("level_export_folder"), PropertyOptions, DetailsSummary, OutError))
			{
				return false;
			}

			if (!WriteJsonFile(Root / TEXT("world_settings") / TEXT("summary.json"), MakeWorldSettingsSummaryJson(World, bSplitProperties, DetailsSummary), OutError))
			{
				return false;
			}
			return !bSplitProperties || WriteJsonFile(Root / TEXT("world_settings") / TEXT("properties.json"), Properties, OutError);
		}

		TSharedPtr<FJsonObject> MakeFeatureGatesJson()
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("schema"), LevelFolderSchema);
			Obj->SetStringField(TEXT("kind"), TEXT("feature_gates"));
			TSharedPtr<FJsonObject> Features = MakeShared<FJsonObject>();
			Features->SetBoolField(TEXT("actor_index"), true);
			Features->SetBoolField(TEXT("actor_packages"), true);
			Features->SetBoolField(TEXT("component_packages"), true);
			Features->SetBoolField(TEXT("world_settings"), true);
			Features->SetBoolField(TEXT("editor_summary"), true);
			Features->SetBoolField(TEXT("level_blueprint_summary"), true);
			Features->SetBoolField(TEXT("streaming_levels"), true);
			Features->SetBoolField(TEXT("data_layers"), true);
			Features->SetBoolField(TEXT("world_partition"), true);
			Features->SetBoolField(TEXT("hlod"), true);
			Features->SetBoolField(TEXT("references"), true);
			Obj->SetObjectField(TEXT("features"), Features);
			return Obj;
		}

		TSharedPtr<FJsonObject> MakeDiagnosticsJson()
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("schema"), LevelFolderSchema);
			Obj->SetStringField(TEXT("kind"), TEXT("diagnostics"));
			Obj->SetArrayField(TEXT("warnings"), {});
			Obj->SetArrayField(TEXT("issues"), {});
			return Obj;
		}
	}

	bool ExportLevelFolder(UWorld* World, const FExportOptions& Options, TSharedPtr<FJsonObject>& OutSummary, FString& OutError)
	{
		if (!World)
		{
			OutError = TEXT("world_not_available");
			return false;
		}

		const FString Root = ResolveOutputFolder(World, Options.OutputFolder);
		IFileManager& FileManager = IFileManager::Get();
		if (FileManager.DirectoryExists(*Root))
		{
			if (!Options.bOverwrite)
			{
				OutError = TEXT("output_folder_exists");
				return false;
			}
			if (!FileManager.DeleteDirectory(*Root, false, true))
			{
				OutError = TEXT("output_folder_delete_failed");
				return false;
			}
		}

		TArray<FString> Directories = {
			TEXT("streaming"),
			TEXT("world_settings"),
			TEXT("actors"),
			TEXT("actors/items"),
			TEXT("references")
		};
		if (ShouldWriteGroupedDetails(Options))
		{
			Directories.Add(TEXT("world_settings/details"));
		}
		if (ShouldIncludeDiagnostics(Options))
		{
			Directories.Add(TEXT("editor"));
			Directories.Add(TEXT("level_blueprint"));
		}
		Directories.Append(MakeActorPackageRelativeDirectories(World, Options));

		if (!FileManager.MakeDirectory(*Root, true))
		{
			OutError = TEXT("output_folder_create_failed");
			return false;
		}
		for (const FString& Directory : Directories)
		{
			if (!FileManager.MakeDirectory(*(Root / Directory), true))
			{
				OutError = FString::Printf(TEXT("directory_create_failed:%s"), *Directory);
				return false;
			}
		}

		TArray<FString> Files = {
			TEXT("README.md"),
			TEXT("manifest.json"),
			TEXT("index.json"),
			TEXT("level.json"),
			TEXT("world.json"),
			TEXT("world_settings/summary.json"),
			TEXT("streaming/index.json"),
			TEXT("data_layers.json"),
			TEXT("world_partition.json"),
			TEXT("hlod.json"),
			TEXT("references/assets.json"),
			TEXT("external_assets.json"),
			TEXT("actors/folders.json"),
			TEXT("actors/index.json")
		};
		if (ShouldSplitProperties(Options))
		{
			Files.Add(TEXT("world_settings/properties.json"));
		}
		if (ShouldWriteGroupedDetails(Options))
		{
			Files.Add(TEXT("world_settings/details/index.json"));
		}
		if (ShouldIncludeDiagnostics(Options))
		{
			Files.Add(TEXT("feature_gates.json"));
			Files.Add(TEXT("diagnostics.json"));
			Files.Add(TEXT("editor/summary.json"));
			Files.Add(TEXT("level_blueprint/summary.json"));
		}
		Files.Append(MakeActorPackageRelativeFiles(World, Options));

		const FString PackageName = World->GetOutermost() ? World->GetOutermost()->GetName() : FString();
		TSharedPtr<FJsonObject> Manifest = MakeShared<FJsonObject>();
		Manifest->SetStringField(TEXT("schema"), LevelFolderSchema);
		Manifest->SetStringField(TEXT("package_version"), TEXT("0.1"));
		Manifest->SetStringField(TEXT("generated_by"), TEXT("UeAgentInterface.level_export_folder"));
		Manifest->SetStringField(TEXT("profile"), NormalizeProfile(Options.Profile));
		Manifest->SetStringField(TEXT("generated_at_utc"), FDateTime::UtcNow().ToIso8601());
		Manifest->SetStringField(TEXT("level_package"), PackageName);
		Manifest->SetStringField(TEXT("world_name"), World->GetName());
		Manifest->SetArrayField(TEXT("files"), StringArray(Files));
		Manifest->SetArrayField(TEXT("directories"), StringArray(Directories));

		const FString Readme =
			TEXT("# UAI Level Folder Package\n\n")
			TEXT("- schema: `uai.level_folder.v1`\n")
			TEXT("- purpose: folder-based Level export for LLM authoring.\n")
			TEXT("- default profile keeps authoring files compact; debug-only files are emitted only by the debug profile.\n");

		if (!WriteTextFile(Root / TEXT("README.md"), Readme, OutError) ||
			!WriteJsonFile(Root / TEXT("manifest.json"), Manifest, OutError) ||
			!WriteJsonFile(Root / TEXT("index.json"), MakeRootIndexJson(World), OutError) ||
			!WriteJsonFile(Root / TEXT("level.json"), MakeLevelJson(World), OutError) ||
			!WriteJsonFile(Root / TEXT("world.json"), MakeWorldJson(World), OutError) ||
			!WriteWorldSettingsPackage(Root, World, Options, OutError) ||
			!WriteJsonFile(Root / TEXT("streaming") / TEXT("index.json"), MakeStreamingIndexJson(World), OutError) ||
			!WriteJsonFile(Root / TEXT("data_layers.json"), MakeDataLayersJson(World), OutError) ||
			!WriteJsonFile(Root / TEXT("world_partition.json"), MakeWorldPartitionJson(World), OutError) ||
			!WriteJsonFile(Root / TEXT("hlod.json"), MakeHlodJson(World), OutError) ||
			!WriteJsonFile(Root / TEXT("references") / TEXT("assets.json"), MakeReferencesAssetsJson(World), OutError) ||
			!WriteJsonFile(Root / TEXT("external_assets.json"), MakeExternalAssetsJson(World), OutError) ||
			!WriteJsonFile(Root / TEXT("actors") / TEXT("folders.json"), MakeFoldersJson(World), OutError) ||
			!WriteJsonFile(Root / TEXT("actors") / TEXT("index.json"), MakeActorIndexJson(World), OutError) ||
			!WriteActorPackages(Root, World, Options, OutError))
		{
			return false;
		}
		if (ShouldIncludeDiagnostics(Options) &&
			(!WriteJsonFile(Root / TEXT("feature_gates.json"), MakeFeatureGatesJson(), OutError) ||
				!WriteJsonFile(Root / TEXT("diagnostics.json"), MakeDiagnosticsJson(), OutError) ||
				!WriteJsonFile(Root / TEXT("editor") / TEXT("summary.json"), MakeEditorSummaryJson(World), OutError) ||
				!WriteJsonFile(Root / TEXT("level_blueprint") / TEXT("summary.json"), MakeLevelBlueprintSummaryJson(World), OutError)))
		{
			return false;
		}

		OutSummary = MakeShared<FJsonObject>();
		OutSummary->SetStringField(TEXT("schema"), LevelFolderSchema);
		OutSummary->SetStringField(TEXT("status"), TEXT("exported"));
		OutSummary->SetStringField(TEXT("folder_path"), Root);
		OutSummary->SetStringField(TEXT("profile"), NormalizeProfile(Options.Profile));
		OutSummary->SetStringField(TEXT("level_package"), PackageName);
		OutSummary->SetNumberField(TEXT("files_written"), Files.Num());
		OutSummary->SetNumberField(TEXT("directories_created"), Directories.Num());
		OutSummary->SetArrayField(TEXT("files"), StringArray(Files));
		OutSummary->SetArrayField(TEXT("directories"), StringArray(Directories));
		return true;
	}
}
