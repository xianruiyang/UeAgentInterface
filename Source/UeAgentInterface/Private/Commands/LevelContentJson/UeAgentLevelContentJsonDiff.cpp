// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentLevelContentJsonDiff.h"
#include "UeAgentLevelContentJsonCore.h"
#include "UeAgentLevelContentJsonQuery.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorActorFolders.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

namespace UeAgentLevelContentJson
{
	TSharedPtr<FJsonObject> DiffAgainstWorld(UWorld* World, const TSharedPtr<FJsonObject>& Content, const TSharedPtr<FJsonObject>& Params, bool bActorOnly)
	{
		TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetStringField(TEXT("schema"), DiffSchema);
		Diff->SetBoolField(TEXT("actor_only"), bActorOnly);
		TArray<TSharedPtr<FJsonValue>> Creates;
		TArray<TSharedPtr<FJsonValue>> Updates;
		TArray<TSharedPtr<FJsonValue>> Deletes;
		TArray<TSharedPtr<FJsonObject>> Resources;
		CollectResourceObjects(Content, Resources);
		int32 CurrentCount = 0;
		TSet<FString> TargetIds;
		for (const TSharedPtr<FJsonObject>& Resource : Resources)
		{
			const FString Id = GetResourceId(Resource);
			if (!Id.IsEmpty())
			{
				TargetIds.Add(Id);
			}
			AActor* Actor = FindActorByNameLabelOrManagedId(World, Id);
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("id"), Id);
			Item->SetStringField(TEXT("kind"), GetResourceKind(Resource));
			if (!Actor)
			{
				Item->SetStringField(TEXT("diff_type"), TEXT("create"));
				Creates.Add(MakeShared<FJsonValueObject>(Item));
				continue;
			}
			++CurrentCount;
			bool bChanged = false;
			const FString ClassPath = GetClassPathForResource(Resource);
			if (!ClassPath.IsEmpty() && Actor->GetClass() && !Actor->GetClass()->GetPathName().Equals(ClassPath, ESearchCase::IgnoreCase))
			{
				Item->SetStringField(TEXT("class_current"), Actor->GetClass()->GetPathName());
				Item->SetStringField(TEXT("class_target"), ClassPath);
				bChanged = true;
			}
			FString Folder;
			if (TryGetStringAny(Resource, { TEXT("folder_path") }, Folder) && !NormalizeFolder(Actor->GetFolderPath().ToString()).Equals(NormalizeFolder(Folder), ESearchCase::IgnoreCase))
			{
				Item->SetStringField(TEXT("folder_current"), Actor->GetFolderPath().ToString());
				Item->SetStringField(TEXT("folder_target"), NormalizeFolder(Folder));
				bChanged = true;
			}
			FString Label;
			if (TryGetStringAny(Resource, { TEXT("label") }, Label) && !Label.TrimStartAndEnd().IsEmpty() && !Actor->GetActorLabel().Equals(Label.TrimStartAndEnd(), ESearchCase::CaseSensitive))
			{
				Item->SetStringField(TEXT("label_current"), Actor->GetActorLabel());
				Item->SetStringField(TEXT("label_target"), Label.TrimStartAndEnd());
				bChanged = true;
			}
			TSharedPtr<FJsonObject> Transform;
			if (TryGetObject(Resource, TEXT("transform"), Transform))
			{
				FVector TargetLocation;
				if (TryGetVectorField(Transform, TEXT("location"), TargetLocation) && !Actor->GetActorLocation().Equals(TargetLocation, 0.1))
				{
					Item->SetObjectField(TEXT("location_current"), MakeVectorJson(Actor->GetActorLocation()));
					Item->SetObjectField(TEXT("location_target"), MakeVectorJson(TargetLocation));
					bChanged = true;
				}
				FRotator TargetRotation;
				if (TryGetRotatorField(Transform, TEXT("rotation"), TargetRotation) && !Actor->GetActorRotation().Equals(TargetRotation, 0.1))
				{
					Item->SetObjectField(TEXT("rotation_current"), MakeRotatorJson(Actor->GetActorRotation()));
					Item->SetObjectField(TEXT("rotation_target"), MakeRotatorJson(TargetRotation));
					bChanged = true;
				}
				FVector TargetScale;
				if (TryGetVectorField(Transform, TEXT("scale"), TargetScale) && !Actor->GetActorScale3D().Equals(TargetScale, 0.001))
				{
					Item->SetObjectField(TEXT("scale_current"), MakeVectorJson(Actor->GetActorScale3D()));
					Item->SetObjectField(TEXT("scale_target"), MakeVectorJson(TargetScale));
					bChanged = true;
				}
			}
			for (const FString& Tag : ReadTags(Resource))
			{
				if (!Tag.TrimStartAndEnd().IsEmpty() && !ActorHasTagString(Actor, Tag.TrimStartAndEnd()))
				{
					Item->SetStringField(TEXT("missing_tag"), Tag.TrimStartAndEnd());
					bChanged = true;
					break;
				}
			}
			TSharedPtr<FJsonObject> Attachments;
			if (TryGetObject(Resource, TEXT("attachments"), Attachments))
			{
				FString ParentId;
				if (TryGetStringAny(Attachments, { TEXT("parent_actor"), TEXT("parent_id") }, ParentId) && !ParentId.TrimStartAndEnd().IsEmpty())
				{
					AActor* Parent = Actor->GetAttachParentActor();
					const FString CurrentParentId = Parent ? ActorStableId(Parent) : FString();
					if (!CurrentParentId.Equals(ParentId.TrimStartAndEnd(), ESearchCase::IgnoreCase))
					{
						Item->SetStringField(TEXT("parent_current"), CurrentParentId);
						Item->SetStringField(TEXT("parent_target"), ParentId.TrimStartAndEnd());
						bChanged = true;
					}
				}
			}
			if (bChanged)
			{
				Item->SetStringField(TEXT("diff_type"), TEXT("update"));
				Updates.Add(MakeShared<FJsonValueObject>(Item));
			}
		}

		const FString DeletePolicy = Params.IsValid() && Params->HasField(TEXT("delete_policy")) ? Params->GetStringField(TEXT("delete_policy")).ToLower() : FString();
		if (DeletePolicy == TEXT("only_managed_missing") || DeletePolicy == TEXT("replace_scope_managed"))
		{
			const FString FolderRoot = GetScopeFolderRoot(Content, Params);
			const FString ManagedTag = GetManagedTag(Content, Params);
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				if (!Actor || !FolderMatches(Actor->GetFolderPath().ToString(), FolderRoot, true) || !ActorHasTagString(Actor, ManagedTag))
				{
					continue;
				}
				const FString Id = GetManagedId(Actor);
				if (!Id.IsEmpty() && !TargetIds.Contains(Id))
				{
					TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("id"), Id);
					Item->SetStringField(TEXT("actor"), Actor->GetActorLabel());
					Item->SetStringField(TEXT("diff_type"), TEXT("delete"));
					Deletes.Add(MakeShared<FJsonValueObject>(Item));
				}
			}
		}

		TArray<TSharedPtr<FJsonValue>> Diffs;
		Diffs.Append(Creates);
		Diffs.Append(Updates);
		Diffs.Append(Deletes);
		Diff->SetArrayField(TEXT("create"), Creates);
		Diff->SetArrayField(TEXT("update"), Updates);
		Diff->SetArrayField(TEXT("delete"), Deletes);
		Diff->SetArrayField(TEXT("diffs"), Diffs);
		Diff->SetNumberField(TEXT("create_count"), Creates.Num());
		Diff->SetNumberField(TEXT("update_count"), Updates.Num());
		Diff->SetNumberField(TEXT("delete_count"), Deletes.Num());
		Diff->SetNumberField(TEXT("current_count"), CurrentCount);
		Diff->SetNumberField(TEXT("target_count"), Resources.Num());
		Diff->SetBoolField(TEXT("empty"), Creates.Num() == 0 && Updates.Num() == 0 && Deletes.Num() == 0);
		Diff->SetObjectField(TEXT("feature_gates"), BuildFeatureGateReport(Content));
		return Diff;
	}

	TSharedPtr<FJsonObject> BuildRepairDiagnosis(UWorld* World, const TSharedPtr<FJsonObject>& Params)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("schema"), RepairSchema);
		FString Prefix;
		if (Params.IsValid())
		{
			TryGetStringAny(Params, { TEXT("folder_path_prefix"), TEXT("folder_root") }, Prefix);
		}
		Prefix = NormalizeFolder(Prefix);
		TArray<TSharedPtr<FJsonValue>> EmptyFolders;
		FActorFolders& ActorFolders = FActorFolders::Get();
		TArray<FFolder> AllFolders;
		ActorFolders.ForEachFolder(*World, [&AllFolders](const FFolder& Folder)
		{
			AllFolders.Add(Folder);
			return true;
		});
		TSet<FString> UsedFolders;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor)
			{
				continue;
			}
			const FString FolderPath = Actor->GetFolderPath().ToString();
			if (!FolderPath.IsEmpty())
			{
				UsedFolders.Add(NormalizeFolder(FolderPath));
			}
		}
		for (const FFolder& Folder : AllFolders)
		{
			const FString FolderPath = NormalizeFolder(Folder.ToString());
			if (!FolderMatches(FolderPath, Prefix, true))
			{
				continue;
			}
			if (!UsedFolders.Contains(FolderPath))
			{
				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("repair"), TEXT("empty_folder"));
				Item->SetStringField(TEXT("folder_path"), FolderPath);
				Item->SetStringField(TEXT("safety"), TEXT("safe"));
				EmptyFolders.Add(MakeShared<FJsonValueObject>(Item));
			}
		}
		Data->SetArrayField(TEXT("diagnostics"), EmptyFolders);
		Data->SetNumberField(TEXT("diagnostic_count"), EmptyFolders.Num());
		Data->SetObjectField(TEXT("feature_gates"), BuildFeatureGateReport(nullptr));
		return Data;
	}

	TSharedPtr<FJsonObject> MergeThreeWay(const TSharedPtr<FJsonObject>& Base, const TSharedPtr<FJsonObject>& Current, const TSharedPtr<FJsonObject>& Target)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema"), MergeSchema);
		TArray<TSharedPtr<FJsonValue>> Conflicts;
		TSharedPtr<FJsonObject> Merged = MakeShared<FJsonObject>();
		Merged->SetStringField(TEXT("schema"), LevelContentSchema);

		auto ResourceMap = [](const TSharedPtr<FJsonObject>& Content)
		{
			TMap<FString, TSharedPtr<FJsonObject>> Map;
			TArray<TSharedPtr<FJsonObject>> Resources;
			CollectResourceObjects(Content, Resources);
			for (const TSharedPtr<FJsonObject>& Resource : Resources)
			{
				const FString Id = GetResourceId(Resource);
				if (!Id.IsEmpty())
				{
					Map.Add(Id, Resource);
				}
			}
			return Map;
		};

		const TMap<FString, TSharedPtr<FJsonObject>> BaseMap = ResourceMap(Base);
		const TMap<FString, TSharedPtr<FJsonObject>> CurrentMap = ResourceMap(Current);
		const TMap<FString, TSharedPtr<FJsonObject>> TargetMap = ResourceMap(Target);
		TSet<FString> AllIds;
		for (const TPair<FString, TSharedPtr<FJsonObject>>& Pair : BaseMap) AllIds.Add(Pair.Key);
		for (const TPair<FString, TSharedPtr<FJsonObject>>& Pair : CurrentMap) AllIds.Add(Pair.Key);
		for (const TPair<FString, TSharedPtr<FJsonObject>>& Pair : TargetMap) AllIds.Add(Pair.Key);

		TArray<TSharedPtr<FJsonValue>> MergedResources;
		for (const FString& Id : AllIds)
		{
			const FString BaseText = BaseMap.Contains(Id) ? JsonToString(BaseMap[Id], false) : FString();
			const FString CurrentText = CurrentMap.Contains(Id) ? JsonToString(CurrentMap[Id], false) : FString();
			const FString TargetText = TargetMap.Contains(Id) ? JsonToString(TargetMap[Id], false) : FString();
			if (!BaseText.Equals(CurrentText) && !BaseText.Equals(TargetText) && !CurrentText.Equals(TargetText))
			{
				TSharedPtr<FJsonObject> Conflict = MakeShared<FJsonObject>();
				Conflict->SetStringField(TEXT("id"), Id);
				Conflict->SetStringField(TEXT("base"), BaseText);
				Conflict->SetStringField(TEXT("current"), CurrentText);
				Conflict->SetStringField(TEXT("target"), TargetText);
				Conflicts.Add(MakeShared<FJsonValueObject>(Conflict));
				continue;
			}
			TSharedPtr<FJsonObject> Chosen = TargetMap.Contains(Id) ? TargetMap[Id] : (CurrentMap.Contains(Id) ? CurrentMap[Id] : BaseMap.FindRef(Id));
			if (Chosen.IsValid())
			{
				MergedResources.Add(MakeShared<FJsonValueObject>(Chosen));
			}
		}
		Merged->SetArrayField(TEXT("resources"), MergedResources);
		Result->SetObjectField(TEXT("merged_json"), Merged);
		Result->SetArrayField(TEXT("conflicts"), Conflicts);
		Result->SetNumberField(TEXT("conflict_count"), Conflicts.Num());
		Result->SetBoolField(TEXT("can_apply"), Conflicts.Num() == 0);
		return Result;
	}
}
