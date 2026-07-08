// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentLevelContentJsonValidatePlan.h"
#include "UeAgentLevelContentJsonCore.h"
#include "UeAgentLevelContentJsonProperties.h"
#include "UeAgentLevelContentJsonQuery.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace UeAgentLevelContentJson
{
	bool ValidateAttachmentGraph(const TArray<TSharedPtr<FJsonObject>>& Resources, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		TMap<FString, FString> ParentById;
		for (const TSharedPtr<FJsonObject>& Resource : Resources)
		{
			const FString Id = GetResourceId(Resource);
			if (Id.IsEmpty())
			{
				continue;
			}
			TSharedPtr<FJsonObject> Attachments;
			if (TryGetObject(Resource, TEXT("attachments"), Attachments))
			{
				FString ParentActor;
				if (TryGetStringAny(Attachments, { TEXT("parent_actor"), TEXT("parent_id") }, ParentActor) && !ParentActor.TrimStartAndEnd().IsEmpty())
				{
					ParentById.Add(Id, ParentActor.TrimStartAndEnd());
				}
			}
		}

		for (const TPair<FString, FString>& Pair : ParentById)
		{
			TSet<FString> Seen;
			FString Cursor = Pair.Key;
			while (ParentById.Contains(Cursor))
			{
				if (Seen.Contains(Cursor))
				{
					AddIssue(Issues, TEXT("error"), TEXT("attachment_cycle"), TEXT("resources.attachments"), FString::Printf(TEXT("Attachment cycle includes %s"), *Cursor));
					return false;
				}
				Seen.Add(Cursor);
				Cursor = ParentById[Cursor];
			}
		}
		return true;
	}

	void ValidateOrganizationAndTasks(UWorld* World, const TSharedPtr<FJsonObject>& Content, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		TSharedPtr<FJsonObject> Scope;
		if (Content.IsValid())
		{
			TryGetObject(Content, TEXT("scope"), Scope);
		}
		if (Scope.IsValid())
		{
			TSharedPtr<FJsonObject> WorldPartition;
			if (TryGetObject(Scope, TEXT("world_partition"), WorldPartition))
			{
				bool bAllowUnloadedChanges = false;
				TryGetBoolAny(WorldPartition, { TEXT("allow_unloaded_actor_changes") }, bAllowUnloadedChanges);
				if (!bAllowUnloadedChanges && WorldPartition->HasField(TEXT("load_region")))
				{
					AddIssue(Issues, TEXT("warning"), TEXT("world_partition_region_report_only"), TEXT("scope.world_partition"), TEXT("World Partition load region is validated as report-only by this command"));
				}
			}
			TSharedPtr<FJsonObject> Streaming;
			if (TryGetObject(Scope, TEXT("streaming"), Streaming))
			{
				const TArray<FString> Required = ReadStringArray(Streaming, TEXT("required_loaded_levels"));
				for (const FString& RequiredLevel : Required)
				{
					bool bFoundLoaded = false;
					if (World)
					{
						for (ULevelStreaming* StreamingLevel : World->GetStreamingLevels())
						{
							if (StreamingLevel && StreamingLevel->GetWorldAssetPackageName().Contains(RequiredLevel) && StreamingLevel->IsLevelLoaded())
							{
								bFoundLoaded = true;
								break;
							}
						}
					}
					if (!bFoundLoaded)
					{
						AddIssue(Issues, TEXT("error"), TEXT("streaming_level_not_loaded"), TEXT("scope.streaming.required_loaded_levels"), RequiredLevel);
					}
				}
			}
		}

		TSharedPtr<FJsonObject> Organization;
		if (TryGetObject(Content, TEXT("organization"), Organization))
		{
			if (Organization->HasField(TEXT("hlod")) || Organization->HasField(TEXT("hlod_layers")))
			{
				AddIssue(Issues, TEXT("warning"), TEXT("feature_unavailable_hlod_assignment_report_only"), TEXT("organization.hlod"), TEXT("HLOD assignment is reported and validated structurally; build is gated"));
			}
			if (Organization->HasField(TEXT("data_layers")))
			{
				AddIssue(Issues, TEXT("warning"), TEXT("feature_unavailable_data_layer_mutation_report_only"), TEXT("organization.data_layers"), TEXT("Data Layer mutation is gated; actor CRUD still applies"));
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Tasks = nullptr;
		if (Content.IsValid() && TryGetArray(Content, TEXT("tasks"), Tasks) && Tasks)
		{
			for (int32 Index = 0; Index < Tasks->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Task = (*Tasks)[Index].IsValid() ? (*Tasks)[Index]->AsObject() : nullptr;
				if (!Task.IsValid())
				{
					AddIssue(Issues, TEXT("error"), TEXT("task_invalid"), FString::Printf(TEXT("tasks[%d]"), Index), TEXT("Task must be an object"));
					continue;
				}
				FString Type;
				TryGetStringAny(Task, { TEXT("type"), TEXT("kind"), TEXT("task") }, Type);
				Type = Type.ToLower();
				double TimeoutSeconds = 0.0;
				if (Task->TryGetNumberField(TEXT("timeout_seconds"), TimeoutSeconds) && TimeoutSeconds < 0.0)
				{
					AddIssue(Issues, TEXT("error"), TEXT("task_invalid_timeout"), FString::Printf(TEXT("tasks[%d].timeout_seconds"), Index), TEXT("timeout_seconds must be >= 0"));
				}
				if (Type == TEXT("navmesh_build") || Type == TEXT("cleanup_empty_folders") || Type == TEXT("report") || Type == TEXT("validate"))
				{
					continue;
				}
				if (Type == TEXT("hlod_build") || Type == TEXT("build_hlod") || Type == TEXT("lighting_build") || Type == TEXT("reflection_capture_update") || Type == TEXT("pcg_generate") ||
					Type == TEXT("load_streaming_level_for_edit") || Type == TEXT("unload_streaming_level_after_apply"))
				{
					bool bRequired = false;
					TryGetBoolAny(Task, { TEXT("required") }, bRequired);
					AddIssue(Issues, bRequired ? TEXT("error") : TEXT("warning"), TEXT("feature_unavailable_task_report_only"), FString::Printf(TEXT("tasks[%d]"), Index), Type);
					continue;
				}
				AddIssue(Issues, TEXT("error"), TEXT("unsupported_task_type"), FString::Printf(TEXT("tasks[%d]"), Index), Type);
			}
		}
	}

	bool ValidateLevelContent(UWorld* World, const TSharedPtr<FJsonObject>& Content, const TSharedPtr<FJsonObject>& Params, bool bActorOnly, TSharedPtr<FJsonObject>& OutData)
	{
		TArray<TSharedPtr<FJsonValue>> Issues;
		TArray<TSharedPtr<FJsonValue>> ResourcesJson;
		TArray<TSharedPtr<FJsonObject>> Resources;
		CollectResourceObjects(Content, Resources);

		FString Schema;
		if (Content.IsValid() && Content->TryGetStringField(TEXT("schema"), Schema) && !Schema.Equals(LevelContentSchema, ESearchCase::IgnoreCase))
		{
			AddIssue(Issues, TEXT("error"), TEXT("unsupported_schema"), TEXT("schema"), Schema);
		}

		TSet<FString> ResourceIds;
		for (int32 Index = 0; Index < Resources.Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>& Resource = Resources[Index];
			const FString Path = FString::Printf(TEXT("resources[%d]"), Index);
			const FString Id = GetResourceId(Resource);
			if (Id.IsEmpty())
			{
				AddIssue(Issues, TEXT("error"), TEXT("missing_resource_id"), Path + TEXT(".id"), TEXT("Each resource requires id"));
			}
			else if (ResourceIds.Contains(Id))
			{
				AddIssue(Issues, TEXT("error"), TEXT("duplicate_resource_id"), Path + TEXT(".id"), Id);
			}
			else
			{
				ResourceIds.Add(Id);
			}

			const FString Category = GetResourceCategory(Resource);
			const FString Kind = GetResourceKind(Resource);
			const FString Operation = GetResourceOperation(Resource, TEXT("upsert"));
			const bool bActorResource = IsSpatialActorResource(Resource);
			if (bActorOnly && !bActorResource && Operation != TEXT("reference"))
			{
				AddIssue(Issues, TEXT("error"), TEXT("actor_alias_non_actor_resource"), Path, Kind);
			}
			if (Resource->HasField(TEXT("lods")) || Resource->HasField(TEXT("static_mesh_lods")))
			{
				AddIssue(Issues, TEXT("error"), TEXT("asset_lod_out_of_level_scope"), Path, TEXT("StaticMesh internal LOD must use StaticMesh asset commands"));
			}
			if (!bActorResource)
			{
				if (Category == TEXT("organization") || Category == TEXT("level_container") || Category == TEXT("terrain") || Kind.Contains(TEXT("hlod")) || Kind.Contains(TEXT("streaming")))
				{
					AddIssue(Issues, Operation == TEXT("reference") ? TEXT("warning") : TEXT("error"), TEXT("resource_kind_report_only_or_unsupported_for_mutation"), Path, Kind);
				}
				else
				{
					AddIssue(Issues, TEXT("error"), TEXT("unknown_category_or_kind"), Path, Category + TEXT(":") + Kind);
				}
				continue;
			}

			const FString ClassPath = GetClassPathForResource(Resource);
			UClass* ActorClass = LoadClass<AActor>(nullptr, *ClassPath);
			if (!ActorClass)
			{
				const FString Code = (Kind == TEXT("pcg_volume") || Kind == TEXT("smart_object")) ? TEXT("plugin_unavailable_or_class_missing") : TEXT("actor_class_not_found");
				AddIssue(Issues, TEXT("error"), Code, Path + TEXT(".class"), ClassPath);
			}

			if (Kind == TEXT("custom"))
			{
				bool bStrictAdapter = false;
				TryGetBoolAny(Resource, { TEXT("strict_adapter") }, bStrictAdapter);
				FString Adapter;
				TryGetStringAny(Resource, { TEXT("adapter") }, Adapter);
				if (!Adapter.IsEmpty() && bStrictAdapter)
				{
					AddIssue(Issues, TEXT("error"), TEXT("custom_adapter_not_registered"), Path + TEXT(".adapter"), Adapter);
				}
				else if (Adapter.IsEmpty())
				{
					AddIssue(Issues, TEXT("warning"), TEXT("custom_actor_reflection_fallback"), Path, TEXT("No adapter specified; reflection fallback will be used"));
				}
			}

			if (ActorClass)
			{
				UObject* CDO = ActorClass->GetDefaultObject();
				TSharedPtr<FJsonObject> Properties;
				if (TryGetObject(Resource, TEXT("properties"), Properties))
				{
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
					{
						if (!ValidatePropertyExists(CDO, Pair.Key))
						{
							AddIssue(Issues, TEXT("error"), TEXT("property_not_found"), Path + TEXT(".properties.") + Pair.Key, Pair.Key);
						}
					}
				}
			}
		}

		ValidateAttachmentGraph(Resources, Issues);
		ValidateOrganizationAndTasks(World, Content, Issues);

		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("schema"), ValidationSchema);
		OutData->SetBoolField(TEXT("actor_only"), bActorOnly);
		OutData->SetNumberField(TEXT("resource_count"), Resources.Num());
		OutData->SetArrayField(TEXT("issues"), Issues);
		OutData->SetArrayField(TEXT("json_issues"), Issues);
		OutData->SetBoolField(TEXT("valid"), !HasErrorIssue(Issues));
		OutData->SetObjectField(TEXT("feature_gates"), BuildFeatureGateReport(Content));
		return true;
	}

	TArray<FString> ReadProtectionTags(const TSharedPtr<FJsonObject>& Content)
	{
		TArray<FString> Tags;
		TSharedPtr<FJsonObject> Protection;
		if (TryGetObject(Content, TEXT("protection"), Protection))
		{
			Tags = ReadStringArray(Protection, TEXT("protected_tags"));
		}
		const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
		if (Content.IsValid() && TryGetArray(Content, TEXT("operations"), Operations) && Operations)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Operations)
			{
				const TSharedPtr<FJsonObject> Op = Value.IsValid() ? Value->AsObject() : nullptr;
				FString Type;
				if (Op.IsValid() && TryGetStringAny(Op, { TEXT("type"), TEXT("operation") }, Type) && Type.Equals(TEXT("protect"), ESearchCase::IgnoreCase))
				{
					Tags.Append(ReadStringArray(Op, TEXT("tags")));
					FString Tag;
					if (TryGetStringAny(Op, { TEXT("tag") }, Tag) && !Tag.IsEmpty())
					{
						Tags.Add(Tag);
					}
				}
			}
		}
		return Tags;
	}

	bool ActorIsProtected(AActor* Actor, const TArray<FString>& ProtectionTags)
	{
		for (const FString& Tag : ProtectionTags)
		{
			if (ActorHasTagString(Actor, Tag))
			{
				return true;
			}
		}
		return false;
	}

	TSharedPtr<FJsonObject> BuildEvidenceReport(UWorld* World, const TSharedPtr<FJsonObject>& Content, const TSharedPtr<FJsonObject>& Params, const FString& Phase, bool bDryRun, bool bAllowFileWrite, FString& OutError)
	{
		TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
		Report->SetStringField(TEXT("schema"), TEXT("uai.level_content.evidence.v1"));
		Report->SetStringField(TEXT("phase"), Phase);
		Report->SetBoolField(TEXT("dry_run"), bDryRun);
		Report->SetObjectField(TEXT("world"), BuildWorldInfo(World));

		TSharedPtr<FJsonObject> Evidence;
		if (!TryGetObject(Content, TEXT("evidence"), Evidence))
		{
			Report->SetBoolField(TEXT("report_requested"), false);
			Report->SetStringField(TEXT("status"), TEXT("not_requested"));
			return Report;
		}

		bool bReportRequested = false;
		TryGetBoolAny(Evidence, { TEXT("report"), TEXT("enabled"), TEXT("capture_report") }, bReportRequested);
		const TArray<TSharedPtr<FJsonValue>>* ViewBookmarks = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Screenshots = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Captures = nullptr;
		const bool bHasViewBookmarks = TryGetArray(Evidence, TEXT("view_bookmarks"), ViewBookmarks) && ViewBookmarks;
		const bool bHasScreenshots = TryGetArray(Evidence, TEXT("screenshots"), Screenshots) && Screenshots;
		const bool bHasCaptures = TryGetArray(Evidence, TEXT("captures"), Captures) && Captures;
		bReportRequested = bReportRequested || bHasViewBookmarks || bHasScreenshots || bHasCaptures;

		Report->SetBoolField(TEXT("report_requested"), bReportRequested);
		Report->SetStringField(TEXT("status"), bReportRequested ? TEXT("report_generated") : TEXT("not_requested"));
		Report->SetStringField(TEXT("visual_capture_status"), TEXT("report_only"));
		Report->SetStringField(TEXT("visual_capture_reason"), TEXT("viewport screenshots and GPU readback remain delegated to dedicated crash-guarded capture commands"));
		if (bHasViewBookmarks)
		{
			Report->SetArrayField(TEXT("view_bookmarks"), *ViewBookmarks);
			Report->SetNumberField(TEXT("view_bookmark_count"), ViewBookmarks->Num());
		}
		if (bHasScreenshots)
		{
			Report->SetArrayField(TEXT("screenshots"), *Screenshots);
			Report->SetNumberField(TEXT("screenshot_count"), Screenshots->Num());
		}
		if (bHasCaptures)
		{
			Report->SetArrayField(TEXT("captures"), *Captures);
			Report->SetNumberField(TEXT("capture_count"), Captures->Num());
		}

		FString OutputFile;
		if (bAllowFileWrite && (TryGetStringAny(Evidence, { TEXT("output_file"), TEXT("report_file"), TEXT("evidence_file") }, OutputFile)) && !OutputFile.TrimStartAndEnd().IsEmpty())
		{
			OutputFile = ResolveFilePath(OutputFile);
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutputFile), true);
			if (!FFileHelper::SaveStringToFile(JsonToString(Report, false), *OutputFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				OutError = TEXT("evidence_output_file_write_failed");
				Report->SetStringField(TEXT("status"), TEXT("write_failed"));
				Report->SetStringField(TEXT("error"), OutError);
				return Report;
			}
			Report->SetStringField(TEXT("output_file"), OutputFile);
			Report->SetStringField(TEXT("output_format"), TEXT("structured_compact"));
		}
		return Report;
	}

	TSharedPtr<FJsonObject> BuildPlan(UWorld* World, const TSharedPtr<FJsonObject>& Content, const TSharedPtr<FJsonObject>& Params, bool bActorOnly)
	{
		TSharedPtr<FJsonObject> Plan = MakeShared<FJsonObject>();
		Plan->SetStringField(TEXT("schema"), PlanSchema);
		Plan->SetBoolField(TEXT("actor_only"), bActorOnly);
		Plan->SetObjectField(TEXT("world"), BuildWorldInfo(World));
		TArray<TSharedPtr<FJsonValue>> Steps;
		TArray<TSharedPtr<FJsonObject>> Resources;
		CollectResourceObjects(Content, Resources);
		const FString DefaultMode = Params.IsValid() && Params->HasField(TEXT("mode")) ? Params->GetStringField(TEXT("mode")).ToLower() : TEXT("upsert");
		for (int32 Index = 0; Index < Resources.Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>& Resource = Resources[Index];
			TSharedPtr<FJsonObject> Step = MakeShared<FJsonObject>();
			const FString Id = GetResourceId(Resource);
			const FString Operation = GetResourceOperation(Resource, DefaultMode == TEXT("create_only") ? TEXT("create") : TEXT("upsert"));
			Step->SetStringField(TEXT("id"), Id);
			Step->SetStringField(TEXT("operation"), Operation);
			Step->SetStringField(TEXT("category"), GetResourceCategory(Resource));
			Step->SetStringField(TEXT("kind"), GetResourceKind(Resource));
			Step->SetStringField(TEXT("class"), GetClassPathForResource(Resource));
			Step->SetBoolField(TEXT("actor_resource"), IsSpatialActorResource(Resource));
			AActor* Existing = FindActorByNameLabelOrManagedId(World, Id);
			Step->SetBoolField(TEXT("exists"), Existing != nullptr);
			if (Existing)
			{
				Step->SetStringField(TEXT("existing_object_path"), Existing->GetPathName());
			}
			Steps.Add(MakeShared<FJsonValueObject>(Step));
		}

		const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
		if (Content.IsValid() && TryGetArray(Content, TEXT("operations"), Operations) && Operations)
		{
			for (int32 Index = 0; Index < Operations->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Op = (*Operations)[Index].IsValid() ? (*Operations)[Index]->AsObject() : nullptr;
				if (!Op.IsValid())
				{
					continue;
				}
				TSharedPtr<FJsonObject> Step = MakeShared<FJsonObject>();
				FString Type;
				TryGetStringAny(Op, { TEXT("type"), TEXT("operation") }, Type);
				Step->SetStringField(TEXT("operation"), Type);
				Step->SetStringField(TEXT("phase"), TEXT("explicit_operation"));
				Step->SetNumberField(TEXT("operation_index"), Index);
				if (Type.Equals(TEXT("convert"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("migrate"), ESearchCase::IgnoreCase))
				{
					Step->SetStringField(TEXT("execution"), TEXT("report_only_destructive_conversion_requires_dedicated_adapter"));
				}
				else if (Type.Equals(TEXT("clone"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("attach"), ESearchCase::IgnoreCase) ||
					Type.Equals(TEXT("detach"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("set_folder"), ESearchCase::IgnoreCase) ||
					Type.Equals(TEXT("add_tag"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("remove_tag"), ESearchCase::IgnoreCase) ||
					Type.Equals(TEXT("align"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("delete"), ESearchCase::IgnoreCase))
				{
					Step->SetStringField(TEXT("execution"), TEXT("editor_safe_operation"));
				}
				else if (Type.Equals(TEXT("screenshot"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("evidence"), ESearchCase::IgnoreCase))
				{
					Step->SetStringField(TEXT("execution"), TEXT("report_only_visual_capture"));
				}
				Steps.Add(MakeShared<FJsonValueObject>(Step));
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Tasks = nullptr;
		if (Content.IsValid() && TryGetArray(Content, TEXT("tasks"), Tasks) && Tasks)
		{
			for (int32 Index = 0; Index < Tasks->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Task = (*Tasks)[Index].IsValid() ? (*Tasks)[Index]->AsObject() : nullptr;
				if (!Task.IsValid())
				{
					continue;
				}
				FString Type;
				TryGetStringAny(Task, { TEXT("type"), TEXT("kind"), TEXT("task") }, Type);
				Type = Type.ToLower();
				TSharedPtr<FJsonObject> Step = MakeShared<FJsonObject>();
				Step->SetStringField(TEXT("phase"), TEXT("explicit_task"));
				Step->SetStringField(TEXT("task"), Type);
				Step->SetNumberField(TEXT("task_index"), Index);
				if (Type == TEXT("navmesh_build") || Type == TEXT("cleanup_empty_folders"))
				{
					Step->SetStringField(TEXT("execution"), TEXT("editor_safe_task"));
				}
				else
				{
					Step->SetStringField(TEXT("execution"), TEXT("feature_gated_report_only"));
				}
				Steps.Add(MakeShared<FJsonValueObject>(Step));
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Validations = nullptr;
		if (Content.IsValid() && TryGetArray(Content, TEXT("validation"), Validations) && Validations)
		{
			for (int32 Index = 0; Index < Validations->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Validation = (*Validations)[Index].IsValid() ? (*Validations)[Index]->AsObject() : nullptr;
				if (!Validation.IsValid())
				{
					continue;
				}
				FString Type;
				TryGetStringAny(Validation, { TEXT("type"), TEXT("kind") }, Type);
				TSharedPtr<FJsonObject> Step = MakeShared<FJsonObject>();
				Step->SetStringField(TEXT("phase"), TEXT("validation"));
				Step->SetStringField(TEXT("validation"), Type.ToLower());
				Step->SetNumberField(TEXT("validation_index"), Index);
				Step->SetStringField(TEXT("execution"), TEXT("read_only_world_probe"));
				Steps.Add(MakeShared<FJsonValueObject>(Step));
			}
		}

		TSharedPtr<FJsonObject> Evidence;
		if (TryGetObject(Content, TEXT("evidence"), Evidence))
		{
			TSharedPtr<FJsonObject> Step = MakeShared<FJsonObject>();
			Step->SetStringField(TEXT("phase"), TEXT("evidence"));
			Step->SetStringField(TEXT("operation"), TEXT("evidence_report"));
			Step->SetStringField(TEXT("execution"), TEXT("report_only_visual_capture"));
			Steps.Add(MakeShared<FJsonValueObject>(Step));

			FString EvidenceError;
			Plan->SetObjectField(TEXT("evidence"), BuildEvidenceReport(World, Content, Params, TEXT("plan"), true, false, EvidenceError));
		}
		Plan->SetNumberField(TEXT("step_count"), Steps.Num());
		Plan->SetArrayField(TEXT("steps"), Steps);
		Plan->SetObjectField(TEXT("feature_gates"), BuildFeatureGateReport(Content));
		return Plan;
	}
}
