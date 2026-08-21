#include "LegacyMechanics/UeAgentLegacyMechanics.h"

#include "UeAgentHttpServer_LevelTopologyOps.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "DataLayer/DataLayerEditorSubsystem.h"
#include "DataLayer/DataLayerFactory.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "EditorLevelUtils.h"
#include "Engine/LevelStreaming.h"
#include "Engine/LevelStreamingAlwaysLoaded.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/DataLayerInstanceWithAsset.h"
#include "WorldPartition/DataLayer/DataLayerUtils.h"
#include "WorldPartition/DataLayer/WorldDataLayers.h"
#include "WorldPartition/ErrorHandling/WorldPartitionStreamingGenerationErrorHandler.h"
#include "WorldPartition/HLOD/HLODActor.h"
#include "WorldPartition/HLOD/HLODLayer.h"
#include "WorldPartition/HLOD/HLODLayerFactory.h"
#include "WorldPartition/HLOD/HLODModifier.h"
#include "WorldPartition/LoaderAdapter/LoaderAdapterActorList.h"
#include "WorldPartition/LoaderAdapter/LoaderAdapterShape.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionActorDescInstanceViewInterface.h"
#include "WorldPartition/WorldPartitionActorLoaderInterface.h"

namespace
{
	struct FStoredWorldPartitionLoader
	{
		FString LoaderId;
		FString Kind;
		FString Label;
		FString WorldPath;
		TUniquePtr<IWorldPartitionActorLoaderInterface::ILoaderAdapter> Adapter;
	};

	TMap<FString, TUniquePtr<FStoredWorldPartitionLoader>>& GetLoaderRegistry()
	{
		static TMap<FString, TUniquePtr<FStoredWorldPartitionLoader>> Registry;
		return Registry;
	}

	bool UnloadWorldPartitionLoader(
		const FUeAgentRequestContext& Ctx,
		const FString& ExpectedKind,
		const FString& ResultSchema,
		TSharedPtr<FJsonObject>& OutData,
		FString& OutError)
	{
		FString LoaderId;
		if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetStringField(TEXT("loader_id"), LoaderId) || LoaderId.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("missing_loader_id");
			return false;
		}
		TUniquePtr<FStoredWorldPartitionLoader>* EntryPtr = GetLoaderRegistry().Find(LoaderId);
		if (!EntryPtr || !EntryPtr->IsValid())
		{
			OutData->SetStringField(TEXT("error_code"), TEXT("world_partition_loader_not_found"));
			OutError = TEXT("world_partition_loader_not_found");
			return false;
		}
		FStoredWorldPartitionLoader* Entry = EntryPtr->Get();
		if (!Entry->Kind.Equals(ExpectedKind, ESearchCase::IgnoreCase))
		{
			OutData->SetStringField(TEXT("error_code"), TEXT("world_partition_loader_kind_mismatch"));
			OutData->SetStringField(TEXT("loader_id"), LoaderId);
			OutData->SetStringField(TEXT("expected_kind"), ExpectedKind);
			OutData->SetStringField(TEXT("actual_kind"), Entry->Kind);
			OutError = TEXT("world_partition_loader_kind_mismatch");
			return false;
		}
		bool bDryRun = true;
		bool bAllowDestructive = false;
		Ctx.Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
		Ctx.Params->TryGetBoolField(TEXT("allow_destructive"), bAllowDestructive);
		OutData->SetStringField(TEXT("schema"), ResultSchema);
		OutData->SetStringField(TEXT("loader_id"), LoaderId);
		OutData->SetStringField(TEXT("kind"), Entry->Kind);
		OutData->SetBoolField(TEXT("unloaded"), false);
		OutData->SetBoolField(TEXT("dry_run"), bDryRun);
		OutData->SetBoolField(TEXT("allow_destructive"), bAllowDestructive);
		if (bDryRun)
		{
			OutData->SetStringField(TEXT("status"), TEXT("planned"));
			return true;
		}
		if (!bAllowDestructive)
		{
			OutData->SetStringField(TEXT("status"), TEXT("blocked"));
			OutData->SetStringField(TEXT("error_code"), TEXT("requires_allow_destructive"));
			OutError = TEXT("requires_allow_destructive");
			return false;
		}
		if (!Entry->Adapter.IsValid())
		{
			OutData->SetStringField(TEXT("status"), TEXT("failed"));
			OutData->SetStringField(TEXT("error_code"), TEXT("world_partition_loader_adapter_invalid"));
			OutError = TEXT("world_partition_loader_adapter_invalid");
			return false;
		}
		Entry->Adapter->Unload();
		const bool bUnloaded = !Entry->Adapter->IsLoaded();
		OutData->SetBoolField(TEXT("unloaded"), bUnloaded);
		if (!bUnloaded)
		{
			OutData->SetStringField(TEXT("status"), TEXT("failed"));
			OutData->SetStringField(TEXT("error_code"), TEXT("world_partition_loader_unload_failed"));
			OutError = TEXT("world_partition_loader_unload_failed");
			return false;
		}
		GetLoaderRegistry().Remove(LoaderId);
		OutData->SetStringField(TEXT("status"), TEXT("unloaded"));
		return true;
	}

	TSharedPtr<FJsonObject> CopyParamsWithExecutionAuthorization(
		const TSharedPtr<FJsonObject>& Source,
		bool bAllowDestructive,
		bool bAllowDelete)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		if (Source.IsValid())
		{
			Params->Values = Source->Values;
		}
		Params->SetBoolField(TEXT("dry_run"), false);
		Params->SetBoolField(TEXT("allow_destructive"), bAllowDestructive);
		Params->SetBoolField(TEXT("allow_delete"), bAllowDelete);
		return Params;
	}

	bool ValidateTopologyDestructiveAuthorization(
		const TSharedPtr<FJsonObject>& Root,
		bool bAllowDestructive,
		bool bAllowDelete,
		FString& OutError)
	{
		auto RequireDestructive = [&bAllowDestructive, &OutError]()
		{
			if (!bAllowDestructive)
			{
				OutError = TEXT("topology_requires_allow_destructive");
				return false;
			}
			return true;
		};
		auto RequireDelete = [&bAllowDelete, &OutError]()
		{
			if (!bAllowDelete)
			{
				OutError = TEXT("topology_requires_allow_delete");
				return false;
			}
			return true;
		};
		auto CheckBuildRequest = [&RequireDestructive, &RequireDelete](const TSharedPtr<FJsonObject>& Build)
		{
			if (!Build.IsValid())
			{
				return true;
			}
			bool bExecute = false;
			bool bReportOnly = true;
			Build->TryGetBoolField(TEXT("execute"), bExecute);
			Build->TryGetBoolField(TEXT("report_only"), bReportOnly);
			if (!bExecute || bReportOnly)
			{
				return true;
			}
			if (!RequireDestructive())
			{
				return false;
			}
			FString Step;
			Build->TryGetStringField(TEXT("step"), Step);
			return !Step.Equals(TEXT("delete"), ESearchCase::IgnoreCase) || RequireDelete();
		};

		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (Root->TryGetArrayField(TEXT("streaming_levels"), Items) && Items)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Items)
			{
				const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr;
				FString Operation;
				if (Item.IsValid() && Item->TryGetStringField(TEXT("operation"), Operation) && Operation.Equals(TEXT("remove"), ESearchCase::IgnoreCase))
				{
					if (!RequireDestructive()) return false;
					bool bDeleteAsset = false;
					Item->TryGetBoolField(TEXT("delete_asset"), bDeleteAsset);
					if (bDeleteAsset && !RequireDelete()) return false;
				}
			}
		}
		if (Root->TryGetArrayField(TEXT("data_layers"), Items) && Items)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Items)
			{
				const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr;
				FString Operation;
				if (Item.IsValid() && Item->TryGetStringField(TEXT("operation"), Operation) && Operation.Equals(TEXT("delete"), ESearchCase::IgnoreCase))
				{
					if (!RequireDestructive() || !RequireDelete()) return false;
				}
			}
		}
		if (Root->TryGetArrayField(TEXT("actor_assignments"), Items) && Items)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Items)
			{
				const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr;
				const TSharedPtr<FJsonObject>* AssignmentsPtr = nullptr;
				const TSharedPtr<FJsonObject> Assignments = Item.IsValid() && Item->TryGetObjectField(TEXT("assignments"), AssignmentsPtr) && AssignmentsPtr && AssignmentsPtr->IsValid() ? *AssignmentsPtr : Item;
				bool bClear = false;
				if (Assignments.IsValid() && Assignments->TryGetBoolField(TEXT("clear_hlod_layer"), bClear) && bClear && !RequireDestructive()) return false;
			}
		}
		const TSharedPtr<FJsonObject>* WorldPartitionObj = nullptr;
		if (Root->TryGetObjectField(TEXT("world_partition"), WorldPartitionObj) && WorldPartitionObj && WorldPartitionObj->IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* UnloadItems = nullptr;
			if (((*WorldPartitionObj)->TryGetArrayField(TEXT("unload_regions"), UnloadItems) && UnloadItems && !UnloadItems->IsEmpty())
				|| ((*WorldPartitionObj)->TryGetArrayField(TEXT("unload_actors"), UnloadItems) && UnloadItems && !UnloadItems->IsEmpty()))
			{
				if (!RequireDestructive()) return false;
			}
			const TSharedPtr<FJsonObject>* Generate = nullptr;
			if ((*WorldPartitionObj)->TryGetObjectField(TEXT("generate_streaming"), Generate) && Generate && Generate->IsValid())
			{
				bool bAllowWrite = false;
				(*Generate)->TryGetBoolField(TEXT("allow_write"), bAllowWrite);
				if (bAllowWrite && !RequireDestructive()) return false;
			}
		}
		const TSharedPtr<FJsonObject>* HLODObj = nullptr;
		if (Root->TryGetObjectField(TEXT("hlod"), HLODObj) && HLODObj && HLODObj->IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Assignments = nullptr;
			if ((*HLODObj)->TryGetArrayField(TEXT("actor_assignments"), Assignments) && Assignments)
			{
				for (const TSharedPtr<FJsonValue>& Value : *Assignments)
				{
					const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr;
					bool bClear = false;
					if (Item.IsValid() && Item->TryGetBoolField(TEXT("clear_hlod_layer"), bClear) && bClear && !RequireDestructive()) return false;
				}
			}
			const TSharedPtr<FJsonObject>* Build = nullptr;
			if ((*HLODObj)->TryGetObjectField(TEXT("build"), Build) && Build && !CheckBuildRequest(*Build)) return false;
		}
		if (Root->TryGetArrayField(TEXT("tasks"), Items) && Items)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Items)
			{
				const TSharedPtr<FJsonObject> Task = Value.IsValid() ? Value->AsObject() : nullptr;
				FString Type;
				if (!Task.IsValid() || (!Task->TryGetStringField(TEXT("type"), Type) && !Task->TryGetStringField(TEXT("command"), Type))) continue;
				if (Type.Equals(TEXT("hlod_build"), ESearchCase::IgnoreCase) && !CheckBuildRequest(Task)) return false;
				if (Type.Equals(TEXT("world_partition_generate_streaming"), ESearchCase::IgnoreCase))
				{
					bool bAllowWrite = false;
					Task->TryGetBoolField(TEXT("allow_write"), bAllowWrite);
					if (bAllowWrite && !RequireDestructive()) return false;
				}
			}
		}
		return true;
	}

	class FWorldPartitionIssueCollector final : public IStreamingGenerationErrorHandler
	{
	public:
		TArray<TSharedPtr<FJsonValue>> Issues;

		void Add(const FString& Code, const FString& Actor, const FString& Message)
		{
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("severity"), TEXT("error"));
			Issue->SetStringField(TEXT("code"), Code);
			Issue->SetStringField(TEXT("path"), Actor);
			Issue->SetStringField(TEXT("message"), Message);
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
		}

		FString ActorName(const IWorldPartitionActorDescInstanceView& ActorDescView)
		{
			return IStreamingGenerationErrorHandler::GetFullActorName(ActorDescView);
		}

		virtual void OnInvalidRuntimeGrid(const IWorldPartitionActorDescInstanceView& ActorDescView, FName GridName) override
		{
			Add(TEXT("invalid_runtime_grid"), ActorName(ActorDescView), GridName.ToString());
		}

		virtual void OnInvalidReference(const IWorldPartitionActorDescInstanceView& ActorDescView, const FGuid& ReferenceGuid, IWorldPartitionActorDescInstanceView*) override
		{
			Add(TEXT("invalid_reference"), ActorName(ActorDescView), ReferenceGuid.ToString(EGuidFormats::DigitsWithHyphens));
		}

		virtual void OnInvalidReferenceGridPlacement(const IWorldPartitionActorDescInstanceView& ActorDescView, const IWorldPartitionActorDescInstanceView& ReferenceActorDescView) override
		{
			Add(TEXT("invalid_reference_grid_placement"), ActorName(ActorDescView), ActorName(ReferenceActorDescView));
		}

		virtual void OnInvalidReferenceDataLayers(const IWorldPartitionActorDescInstanceView& ActorDescView, const IWorldPartitionActorDescInstanceView& ReferenceActorDescView, EDataLayerInvalidReason) override
		{
			Add(TEXT("invalid_reference_data_layers"), ActorName(ActorDescView), ActorName(ReferenceActorDescView));
		}

		virtual void OnInvalidReferenceRuntimeGrid(const IWorldPartitionActorDescInstanceView& ActorDescView, const IWorldPartitionActorDescInstanceView& ReferenceActorDescView) override
		{
			Add(TEXT("invalid_reference_runtime_grid"), ActorName(ActorDescView), ActorName(ReferenceActorDescView));
		}

		virtual void OnDataLayersLoadFilterMismatch(const IWorldPartitionActorDescInstanceView& ActorDescView) override
		{
			Add(TEXT("data_layers_load_filter_mismatch"), ActorName(ActorDescView), TEXT(""));
		}

		virtual void OnInvalidWorldReference(const IWorldPartitionActorDescInstanceView& ActorDescView, EWorldReferenceInvalidReason) override
		{
			Add(TEXT("invalid_world_reference"), ActorName(ActorDescView), TEXT(""));
		}

		virtual void OnInvalidReferenceDataLayerAsset(const UDataLayerInstanceWithAsset* DataLayerInstance) override
		{
			Add(TEXT("invalid_reference_data_layer_asset"), DataLayerInstance ? DataLayerInstance->GetPathName() : FString(), TEXT(""));
		}

		virtual void OnDataLayerHierarchyTypeMismatch(const UDataLayerInstance* DataLayerInstance, const UDataLayerInstance*, EDataLayerHierarchyInvalidReason) override
		{
			Add(TEXT("data_layer_hierarchy_type_mismatch"), DataLayerInstance ? DataLayerInstance->GetPathName() : FString(), TEXT(""));
		}

		virtual void OnInvalidWorldDataLayersReference(const AWorldDataLayers* WorldDataLayers, const UDataLayerInstance* DataLayerInstance, const FText& Reason) override
		{
			Add(TEXT("invalid_world_data_layers_reference"), WorldDataLayers ? WorldDataLayers->GetPathName() : FString(), FString::Printf(TEXT("%s %s"), DataLayerInstance ? *DataLayerInstance->GetPathName() : TEXT(""), *Reason.ToString()));
		}

		virtual void OnDataLayerAssetConflict(const UDataLayerInstanceWithAsset* DataLayerInstance, const UDataLayerInstanceWithAsset* ConflictingDataLayerInstance) override
		{
			Add(TEXT("data_layer_asset_conflict"), DataLayerInstance ? DataLayerInstance->GetPathName() : FString(), ConflictingDataLayerInstance ? ConflictingDataLayerInstance->GetPathName() : FString());
		}

		virtual void OnInvalidDataLayerAssetType(const UDataLayerInstanceWithAsset* DataLayerInstance, const UDataLayerAsset* DataLayerAsset) override
		{
			Add(TEXT("invalid_data_layer_asset_type"), DataLayerInstance ? DataLayerInstance->GetPathName() : FString(), DataLayerAsset ? DataLayerAsset->GetPathName() : FString());
		}

		virtual void OnActorNeedsResave(const IWorldPartitionActorDescInstanceView& ActorDescView) override
		{
			Add(TEXT("actor_needs_resave"), ActorName(ActorDescView), TEXT(""));
		}

		virtual void OnLevelInstanceInvalidWorldAsset(const IWorldPartitionActorDescInstanceView& ActorDescView, FName WorldAsset, ELevelInstanceInvalidReason) override
		{
			Add(TEXT("level_instance_invalid_world_asset"), ActorName(ActorDescView), WorldAsset.ToString());
		}

		virtual void OnInvalidActorFilterReference(const IWorldPartitionActorDescInstanceView& ActorDescView, const IWorldPartitionActorDescInstanceView& ReferenceActorDescView) override
		{
			Add(TEXT("invalid_actor_filter_reference"), ActorName(ActorDescView), ActorName(ReferenceActorDescView));
		}

		virtual void OnInvalidHLODLayer(const IWorldPartitionActorDescInstanceView& ActorDescView) override
		{
			Add(TEXT("invalid_hlod_layer"), ActorName(ActorDescView), TEXT(""));
		}
	};
}

bool FUeAgentLegacyMechanics::CmdWorldPartitionQuery(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	UWorldPartition* WorldPartition = World->GetWorldPartition();
	const bool bEnabled = WorldPartition != nullptr;
	OutData->SetStringField(TEXT("schema"), TEXT("uai.world_partition.query.v1"));
	OutData->SetStringField(TEXT("world"), World->GetPathName());
	OutData->SetBoolField(TEXT("enabled"), bEnabled);
	if (!WorldPartition)
	{
		OutData->SetBoolField(TEXT("streaming_enabled"), false);
		OutData->SetArrayField(TEXT("loaded_regions"), TArray<TSharedPtr<FJsonValue>>());
		return true;
	}
	OutData->SetStringField(TEXT("editor_name"), WorldPartition->GetWorldPartitionEditorName().ToString());
	OutData->SetBoolField(TEXT("streaming_enabled"), WorldPartition->IsStreamingEnabledInEditor());
	OutData->SetObjectField(TEXT("editor_world_bounds"), UeAgentLevelTopology::BoxToJson(WorldPartition->GetEditorWorldBounds()));
	OutData->SetObjectField(TEXT("runtime_world_bounds"), UeAgentLevelTopology::BoxToJson(WorldPartition->GetRuntimeWorldBounds()));
	TArray<TSharedPtr<FJsonValue>> RegionValues;
	for (const FBox& Region : WorldPartition->GetUserLoadedEditorRegions())
	{
		RegionValues.Add(MakeShared<FJsonValueObject>(UeAgentLevelTopology::BoxToJson(Region)));
	}
	OutData->SetArrayField(TEXT("loaded_regions"), RegionValues);
	OutData->SetNumberField(TEXT("loaded_region_count"), RegionValues.Num());
	return true;
}

bool FUeAgentLegacyMechanics::CmdWorldPartitionSetStreamingEnabled(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	UWorldPartition* WorldPartition = World->GetWorldPartition();
	if (!WorldPartition)
	{
		OutData->SetStringField(TEXT("error_code"), TEXT("world_partition_not_enabled"));
		OutError = TEXT("world_partition_not_enabled");
		return false;
	}
	bool bEnabled = true;
	if (!JsonTryGetBool(Ctx.Params, TEXT("enabled"), bEnabled))
	{
		OutError = TEXT("missing_enabled");
		return false;
	}
	bool bTransaction = true;
	JsonTryGetBool(Ctx.Params, TEXT("transaction"), bTransaction);
	TUniquePtr<FScopedTransaction> Transaction;
	if (bTransaction)
	{
		Transaction = MakeUnique<FScopedTransaction>(NSLOCTEXT("UeAgentInterface", "WorldPartitionSetStreaming", "UeAgentInterface World Partition Set Streaming"));
	}
	WorldPartition->Modify();
	WorldPartition->SetEnableStreaming(bEnabled);
	WorldPartition->OnEnableStreamingChanged();
	WorldPartition->MarkPackageDirty();
	OutData->SetStringField(TEXT("schema"), TEXT("uai.world_partition.streaming_result.v1"));
	OutData->SetBoolField(TEXT("streaming_enabled"), WorldPartition->IsStreamingEnabledInEditor());
	OutData->SetBoolField(TEXT("transaction"), bTransaction);
	return true;
}

bool FUeAgentLegacyMechanics::CmdWorldPartitionLoadRegion(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	UWorldPartition* WorldPartition = World->GetWorldPartition();
	if (!WorldPartition)
	{
		OutData->SetStringField(TEXT("error_code"), TEXT("world_partition_not_enabled"));
		OutError = TEXT("world_partition_not_enabled");
		return false;
	}
	bool bDryRun = false;
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	TSharedPtr<FJsonObject> BoundsJson;
	if (!JsonTryGetObject(Ctx.Params, TEXT("bounds"), BoundsJson))
	{
		OutError = TEXT("missing_bounds");
		return false;
	}
	FBox Bounds;
	if (!UeAgentLevelTopology::TryReadBox(BoundsJson, Bounds))
	{
		OutError = TEXT("bounds_invalid");
		return false;
	}
	FString Label = TEXT("UAI_WP_Region");
	JsonTryGetString(Ctx.Params, TEXT("label"), Label);
	const FString LoaderId = FString::Printf(TEXT("wp_region_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	OutData->SetStringField(TEXT("schema"), TEXT("uai.world_partition.load_region_result.v1"));
	OutData->SetStringField(TEXT("loader_id"), LoaderId);
	OutData->SetStringField(TEXT("label"), Label);
	OutData->SetObjectField(TEXT("bounds"), UeAgentLevelTopology::BoxToJson(Bounds));
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	if (bDryRun)
	{
		OutData->SetBoolField(TEXT("loaded"), false);
		OutData->SetStringField(TEXT("status"), TEXT("planned"));
		return true;
	}
	TUniquePtr<FStoredWorldPartitionLoader> Entry = MakeUnique<FStoredWorldPartitionLoader>();
	Entry->LoaderId = LoaderId;
	Entry->Kind = TEXT("region");
	Entry->Label = Label;
	Entry->WorldPath = World->GetPathName();
	Entry->Adapter = MakeUnique<FLoaderAdapterShape>(World, Bounds, Label);
	Entry->Adapter->SetUserCreated(true);
	Entry->Adapter->Load();
	const bool bLoaded = Entry->Adapter->IsLoaded();
	GetLoaderRegistry().Add(LoaderId, MoveTemp(Entry));
	OutData->SetBoolField(TEXT("loaded"), bLoaded);
	OutData->SetStringField(TEXT("status"), bLoaded ? TEXT("loaded") : TEXT("load_requested"));
	return true;
}

bool FUeAgentLegacyMechanics::CmdWorldPartitionUnloadRegion(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return UnloadWorldPartitionLoader(Ctx, TEXT("region"), TEXT("uai.world_partition.unload_region_result.v1"), OutData, OutError);
}

bool FUeAgentLegacyMechanics::CmdWorldPartitionLoadActors(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	if (!World->GetWorldPartition())
	{
		OutData->SetStringField(TEXT("error_code"), TEXT("world_partition_not_enabled"));
		OutError = TEXT("world_partition_not_enabled");
		return false;
	}
	TArray<FGuid> Guids;
	for (const FString& GuidText : UeAgentLevelTopology::ReadStringArray(Ctx.Params, TEXT("actor_guids")))
	{
		FGuid Guid;
		if (FGuid::Parse(GuidText, Guid))
		{
			Guids.Add(Guid);
		}
	}
	TArray<AActor*> Actors;
	TArray<TSharedPtr<FJsonValue>> ActorResults;
	FString ActorError;
	if (Ctx.Params->HasField(TEXT("actor_ids")) && UeAgentLevelTopology::ResolveActors(World, Ctx.Params, Actors, ActorResults, ActorError))
	{
		for (AActor* Actor : Actors)
		{
			if (Actor)
			{
				Guids.AddUnique(Actor->GetActorGuid());
			}
		}
	}
	if (Guids.IsEmpty())
	{
		OutError = TEXT("missing_actor_guids");
		return false;
	}
	bool bDryRun = false;
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	const FString LoaderId = FString::Printf(TEXT("wp_actor_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	OutData->SetStringField(TEXT("schema"), TEXT("uai.world_partition.load_actors_result.v1"));
	OutData->SetStringField(TEXT("loader_id"), LoaderId);
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	OutData->SetNumberField(TEXT("actor_guid_count"), Guids.Num());
	OutData->SetArrayField(TEXT("actor_results"), ActorResults);
	if (bDryRun)
	{
		OutData->SetStringField(TEXT("status"), TEXT("planned"));
		return true;
	}
	TUniquePtr<FStoredWorldPartitionLoader> Entry = MakeUnique<FStoredWorldPartitionLoader>();
	Entry->LoaderId = LoaderId;
	Entry->Kind = TEXT("actors");
	Entry->Label = TEXT("UAI_WP_Actors");
	Entry->WorldPath = World->GetPathName();
	TUniquePtr<FLoaderAdapterActorList> ActorList = MakeUnique<FLoaderAdapterActorList>(World);
	ActorList->AddActors(Guids);
	Entry->Adapter = MoveTemp(ActorList);
	Entry->Adapter->SetUserCreated(true);
	Entry->Adapter->Load();
	const bool bLoaded = Entry->Adapter->IsLoaded();
	GetLoaderRegistry().Add(LoaderId, MoveTemp(Entry));
	OutData->SetBoolField(TEXT("loaded"), bLoaded);
	OutData->SetStringField(TEXT("status"), bLoaded ? TEXT("loaded") : TEXT("load_requested"));
	return true;
}

bool FUeAgentLegacyMechanics::CmdWorldPartitionUnloadActors(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return UnloadWorldPartitionLoader(Ctx, TEXT("actors"), TEXT("uai.world_partition.unload_actors_result.v1"), OutData, OutError);
}

bool FUeAgentLegacyMechanics::CmdWorldPartitionListLoadedRegions(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> UaiLoaders;
	for (const TPair<FString, TUniquePtr<FStoredWorldPartitionLoader>>& Pair : GetLoaderRegistry())
	{
		if (!Pair.Value.IsValid())
		{
			continue;
		}
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("loader_id"), Pair.Value->LoaderId);
		Obj->SetStringField(TEXT("kind"), Pair.Value->Kind);
		Obj->SetStringField(TEXT("label"), Pair.Value->Label);
		Obj->SetStringField(TEXT("world"), Pair.Value->WorldPath);
		Obj->SetBoolField(TEXT("loaded"), Pair.Value->Adapter.IsValid() && Pair.Value->Adapter->IsLoaded());
		UaiLoaders.Add(MakeShared<FJsonValueObject>(Obj));
	}
	TArray<TSharedPtr<FJsonValue>> UserRegions;
	if (UWorldPartition* WorldPartition = World->GetWorldPartition())
	{
		for (const FBox& Region : WorldPartition->GetUserLoadedEditorRegions())
		{
			UserRegions.Add(MakeShared<FJsonValueObject>(UeAgentLevelTopology::BoxToJson(Region)));
		}
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.world_partition.loaded_regions.v1"));
	OutData->SetArrayField(TEXT("uai_loaders"), UaiLoaders);
	OutData->SetArrayField(TEXT("user_loaded_regions"), UserRegions);
	return true;
}

bool FUeAgentLegacyMechanics::CmdWorldPartitionCheckErrors(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	UWorldPartition* WorldPartition = World->GetWorldPartition();
	if (!WorldPartition)
	{
		OutData->SetStringField(TEXT("error_code"), TEXT("world_partition_not_enabled"));
		OutError = TEXT("world_partition_not_enabled");
		return false;
	}
	bool bEnableStreaming = WorldPartition->IsStreamingEnabledInEditor();
	JsonTryGetBool(Ctx.Params, TEXT("enable_streaming"), bEnableStreaming);
	FWorldPartitionIssueCollector Handler;
	UWorldPartition::FCheckForErrorsParams Params;
	Params.SetErrorHandler(&Handler);
	Params.SetActorDescContainerInstanceCollection(WorldPartition);
	Params.SetEnableStreaming(bEnableStreaming);
	UWorldPartition::CheckForErrors(Params);
	OutData->SetStringField(TEXT("schema"), TEXT("uai.world_partition.check_errors_result.v1"));
	OutData->SetArrayField(TEXT("issues"), Handler.Issues);
	OutData->SetNumberField(TEXT("error_count"), Handler.Issues.Num());
	OutData->SetStringField(TEXT("status"), Handler.Issues.IsEmpty() ? TEXT("valid") : TEXT("invalid"));
	return true;
}

bool FUeAgentLegacyMechanics::CmdWorldPartitionGenerateStreaming(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	UWorldPartition* WorldPartition = World->GetWorldPartition();
	if (!WorldPartition)
	{
		OutData->SetStringField(TEXT("error_code"), TEXT("world_partition_not_enabled"));
		OutError = TEXT("world_partition_not_enabled");
		return false;
	}
	bool bDryRun = true;
	bool bAllowWrite = false;
	bool bAllowCommandlet = false;
	bool bAllowDestructive = false;
	double TimeoutSeconds = 600.0;
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	JsonTryGetBool(Ctx.Params, TEXT("allow_write"), bAllowWrite);
	JsonTryGetBool(Ctx.Params, TEXT("allow_commandlet"), bAllowCommandlet);
	JsonTryGetBool(Ctx.Params, TEXT("allow_destructive"), bAllowDestructive);
	JsonTryGetNumber(Ctx.Params, TEXT("timeout_seconds"), TimeoutSeconds);
	OutData->SetStringField(TEXT("schema"), TEXT("uai.world_partition.generate_streaming_result.v1"));
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	OutData->SetBoolField(TEXT("allow_write"), bAllowWrite);
	OutData->SetBoolField(TEXT("allow_commandlet"), bAllowCommandlet);
	OutData->SetBoolField(TEXT("allow_destructive"), bAllowDestructive);
	OutData->SetNumberField(TEXT("timeout_seconds"), TimeoutSeconds);
	const FString Args = TEXT("-run=WorldPartitionBuilderCommandlet -Builder=WorldPartitionBuilder -AllowCommandletRendering");
	OutData->SetStringField(TEXT("commandlet_plan"), Args);
	if (!FMath::IsFinite(TimeoutSeconds) || TimeoutSeconds <= 0.0)
	{
		OutData->SetStringField(TEXT("error_code"), TEXT("timeout_seconds_invalid"));
		OutError = TEXT("timeout_seconds_invalid");
		return false;
	}
	if (bDryRun || !bAllowWrite)
	{
		OutData->SetStringField(TEXT("status"), TEXT("planned"));
		return true;
	}
	if (!bAllowCommandlet)
	{
		OutData->SetStringField(TEXT("error_code"), TEXT("requires_commandlet"));
		OutError = TEXT("requires_commandlet");
		return false;
	}
	if (!bAllowDestructive)
	{
		OutData->SetStringField(TEXT("error_code"), TEXT("requires_allow_destructive"));
		OutError = TEXT("requires_allow_destructive");
		return false;
	}
	const bool bOk = UeAgentLevelTopology::RunCommandletProcess(Args, TimeoutSeconds, OutData, OutError);
	OutData->SetStringField(TEXT("status"), bOk ? TEXT("generated") : TEXT("failed"));
	return bOk;
}

bool FUeAgentLegacyMechanics::CmdHLODLayerApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> Root;
	FString Source;
	if (!UeAgentLevelTopology::LoadJsonFromParams(Ctx.Params, Root, Source, OutError))
	{
		return false;
	}
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath))
	{
		Root->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	FString OverwritePolicy = TEXT("replace");
	JsonTryGetString(Ctx.Params, TEXT("overwrite_policy"), OverwritePolicy);
	bool bSaveAfterApply = false;
	bool bTransaction = true;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
	JsonTryGetBool(Ctx.Params, TEXT("transaction"), bTransaction);
	TUniquePtr<FScopedTransaction> Transaction;
	if (bTransaction)
	{
		Transaction = MakeUnique<FScopedTransaction>(NSLOCTEXT("UeAgentInterface", "HLODLayerApply", "UeAgentInterface HLOD Layer Apply"));
	}
	UHLODLayerFactory* Factory = NewObject<UHLODLayerFactory>();
	UObject* AssetObject = nullptr;
	if (!UeAgentLevelTopology::CreateOrLoadAssetWithFactory(UeAgentLevelTopology::NormalizePackagePath(AssetPath), UHLODLayer::StaticClass(), Factory, OverwritePolicy, AssetObject, OutError))
	{
		return false;
	}
	UHLODLayer* Layer = Cast<UHLODLayer>(AssetObject);
	if (!Layer)
	{
		OutError = TEXT("hlod_layer_create_failed");
		return false;
	}
	Layer->Modify();
	TArray<TSharedPtr<FJsonValue>> PropertyResults;
	FString LayerTypeText;
	if (Root->TryGetStringField(TEXT("layer_type"), LayerTypeText))
	{
		EHLODLayerType LayerType = EHLODLayerType::Instancing;
		if (!UeAgentLevelTopology::ParseHLODLayerType(LayerTypeText, LayerType))
		{
			OutError = TEXT("hlod_layer_type_invalid");
			return false;
		}
		Layer->SetLayerType(LayerType);
	}
	bool bIsSpatiallyLoaded = true;
	if (Root->TryGetBoolField(TEXT("is_spatially_loaded"), bIsSpatiallyLoaded))
	{
		Layer->SetIsSpatiallyLoaded(bIsSpatiallyLoaded);
	}
	double CellSize = 0.0;
	if (Root->TryGetNumberField(TEXT("cell_size"), CellSize))
	{
		TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
		Value->SetNumberField(TEXT("literal"), static_cast<int32>(CellSize));
		TSharedPtr<FJsonObject> ApplyResult;
		if (!UeAgentLevelTopology::ApplyCanonicalProperty(Layer, TEXT("CellSize"), MakeShared<FJsonValueObject>(Value), ApplyResult))
		{
			OutError = TEXT("hlod_cell_size_apply_failed");
			return false;
		}
		PropertyResults.Add(MakeShared<FJsonValueObject>(ApplyResult));
	}
	double LoadingRange = 0.0;
	if (Root->TryGetNumberField(TEXT("loading_range"), LoadingRange))
	{
		TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
		Value->SetNumberField(TEXT("literal"), LoadingRange);
		TSharedPtr<FJsonObject> ApplyResult;
		if (!UeAgentLevelTopology::ApplyCanonicalProperty(Layer, TEXT("LoadingRange"), MakeShared<FJsonValueObject>(Value), ApplyResult))
		{
			OutError = TEXT("hlod_loading_range_apply_failed");
			return false;
		}
		PropertyResults.Add(MakeShared<FJsonValueObject>(ApplyResult));
	}
	FString ParentLayerPath;
	if (Root->TryGetStringField(TEXT("parent_layer"), ParentLayerPath) && !ParentLayerPath.IsEmpty())
	{
		Layer->SetParentLayer(Cast<UHLODLayer>(UeAgentLevelTopology::LoadObjectByPath(ParentLayerPath)));
	}
	Layer->MarkPackageDirty();
	TArray<FString> SavedPackages;
	if (bSaveAfterApply)
	{
		TArray<UPackage*> Packages;
		Packages.Add(Layer->GetOutermost());
		if (!UeAgentLevelTopology::SavePackages(Packages, SavedPackages, OutError))
		{
			return false;
		}
	}
	OutData = UeAgentLevelTopology::HLODLayerToJson(Layer);
	OutData->SetStringField(TEXT("schema"), TEXT("uai.hlod_layer.apply_result.v1"));
	OutData->SetStringField(TEXT("status"), TEXT("applied"));
	OutData->SetStringField(TEXT("source"), Source);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterApply);
	OutData->SetBoolField(TEXT("transaction"), bTransaction);
	OutData->SetArrayField(TEXT("property_results"), PropertyResults);
	return true;
}

bool FUeAgentLegacyMechanics::CmdHLODLayerCreateJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return CmdHLODLayerApplyJson(Ctx, OutData, OutError);
}

bool FUeAgentLegacyMechanics::CmdHLODLayerExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath))
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UHLODLayer* Layer = Cast<UHLODLayer>(UeAgentLevelTopology::LoadObjectByPath(AssetPath));
	if (!Layer)
	{
		OutError = TEXT("hlod_layer_not_found");
		return false;
	}
	OutData = UeAgentLevelTopology::HLODLayerToJson(Layer);
	return UeAgentLevelTopology::SaveJsonIfRequested(OutData, Ctx.Params, OutData, OutError);
}

bool FUeAgentLegacyMechanics::CmdHLODLayerValidateJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> Root;
	FString Source;
	if (!UeAgentLevelTopology::LoadJsonFromParams(Ctx.Params, Root, Source, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	FString AssetPath;
	if (!Root->TryGetStringField(TEXT("asset_path"), AssetPath) && !JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath))
	{
		UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("missing_asset_path"), TEXT("$.asset_path"), TEXT("HLODLayer asset path is required."));
	}
	else
	{
		AssetPath = UeAgentLevelTopology::NormalizePackagePath(AssetPath);
		if (!UeAgentLevelTopology::IsValidLongPackagePath(AssetPath))
		{
			UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("hlod_layer_path_invalid"), TEXT("$.asset_path"), TEXT("HLODLayer asset path is invalid."));
		}
		else if (UObject* ExistingAsset = UeAgentLevelTopology::LoadObjectByPath(AssetPath))
		{
			if (!ExistingAsset->IsA<UHLODLayer>())
			{
				UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("hlod_layer_type_mismatch"), TEXT("$.asset_path"), TEXT("Existing asset is not an HLODLayer."));
			}
		}
	}
	FString LayerTypeText;
	if (Root->TryGetStringField(TEXT("layer_type"), LayerTypeText))
	{
		EHLODLayerType ParsedType = EHLODLayerType::Instancing;
		if (!UeAgentLevelTopology::ParseHLODLayerType(LayerTypeText, ParsedType))
		{
			UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("hlod_layer_type_invalid"), TEXT("$.layer_type"), TEXT("Unknown HLOD layer type."));
		}
	}
	FString ParentLayerPath;
	if (Root->TryGetStringField(TEXT("parent_layer"), ParentLayerPath) && !ParentLayerPath.IsEmpty() && !Cast<UHLODLayer>(UeAgentLevelTopology::LoadObjectByPath(ParentLayerPath)))
	{
		UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("hlod_parent_layer_not_found"), TEXT("$.parent_layer"), TEXT("Parent HLOD layer was not found."));
	}
	const int32 ErrorCount = UeAgentLevelTopology::CountIssues(Issues, TEXT("error"));
	OutData->SetStringField(TEXT("schema"), TEXT("uai.hlod_layer.validate_result.v1"));
	OutData->SetStringField(TEXT("source"), Source);
	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetStringField(TEXT("status"), ErrorCount == 0 ? TEXT("valid") : TEXT("invalid"));
	OutData->SetArrayField(TEXT("issues"), Issues);
	OutData->SetNumberField(TEXT("error_count"), ErrorCount);
	return ErrorCount == 0;
}

bool FUeAgentLegacyMechanics::CmdHLODAssignActors(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	TArray<AActor*> Actors;
	TArray<TSharedPtr<FJsonValue>> ActorResults;
	if (!UeAgentLevelTopology::ResolveActors(World, Ctx.Params, Actors, ActorResults, OutError))
	{
		return false;
	}
	FString HLODLayerPath;
	const bool bHasHLODLayer = JsonTryGetString(Ctx.Params, TEXT("hlod_layer"), HLODLayerPath);
	bool bClearHLODLayer = false;
	bool bAllowDestructive = false;
	JsonTryGetBool(Ctx.Params, TEXT("clear_hlod_layer"), bClearHLODLayer);
	JsonTryGetBool(Ctx.Params, TEXT("allow_destructive"), bAllowDestructive);
	if (bHasHLODLayer && bClearHLODLayer)
	{
		OutError = TEXT("hlod_layer_assignment_conflict");
		return false;
	}
	if (bHasHLODLayer && HLODLayerPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("hlod_layer_empty_requires_clear_hlod_layer");
		return false;
	}
	if (bClearHLODLayer && !bAllowDestructive)
	{
		OutError = TEXT("requires_allow_destructive");
		return false;
	}
	UHLODLayer* Layer = bHasHLODLayer ? Cast<UHLODLayer>(UeAgentLevelTopology::LoadObjectByPath(HLODLayerPath)) : nullptr;
	if (bHasHLODLayer && !Layer)
	{
		OutError = TEXT("hlod_layer_not_found");
		return false;
	}
	bool bTransaction = true;
	JsonTryGetBool(Ctx.Params, TEXT("transaction"), bTransaction);
	TUniquePtr<FScopedTransaction> Transaction;
	if (bTransaction)
	{
		Transaction = MakeUnique<FScopedTransaction>(NSLOCTEXT("UeAgentInterface", "HLODAssignActors", "UeAgentInterface HLOD Assign Actors"));
	}
	FString RuntimeGrid;
	const bool bSetRuntimeGrid = JsonTryGetString(Ctx.Params, TEXT("runtime_grid"), RuntimeGrid);
	bool bIsSpatiallyLoaded = true;
	const bool bSetSpatial = JsonTryGetBool(Ctx.Params, TEXT("is_spatially_loaded"), bIsSpatiallyLoaded);
	if (!bHasHLODLayer && !bClearHLODLayer && !bSetRuntimeGrid && !bSetSpatial)
	{
		OutError = TEXT("missing_hlod_assignment_fields");
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Results;
	for (AActor* Actor : Actors)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("name"), Actor ? Actor->GetName() : FString());
		Result->SetStringField(TEXT("label"), Actor ? Actor->GetActorLabel() : FString());
		if (Actor)
		{
			UHLODLayer* BeforeLayer = Actor->GetHLODLayer();
			const FName BeforeRuntimeGrid = Actor->GetRuntimeGrid();
			const bool bBeforeSpatiallyLoaded = Actor->GetIsSpatiallyLoaded();
			Actor->Modify();
			if (bHasHLODLayer || bClearHLODLayer)
			{
				Actor->SetHLODLayer(bClearHLODLayer ? nullptr : Layer);
			}
			if (bSetRuntimeGrid)
			{
				Actor->SetRuntimeGrid(FName(*RuntimeGrid));
			}
			if (bSetSpatial)
			{
				Actor->SetIsSpatiallyLoaded(bIsSpatiallyLoaded);
			}
			const bool bChanged = BeforeLayer != Actor->GetHLODLayer()
				|| BeforeRuntimeGrid != Actor->GetRuntimeGrid()
				|| bBeforeSpatiallyLoaded != Actor->GetIsSpatiallyLoaded();
			if (bChanged)
			{
				Actor->MarkPackageDirty();
			}
			Result->SetStringField(TEXT("hlod_layer"), Actor->GetHLODLayer() ? UeAgentLevelTopology::ObjectPackagePath(Actor->GetHLODLayer()) : FString());
			Result->SetStringField(TEXT("runtime_grid"), Actor->GetRuntimeGrid().ToString());
			Result->SetBoolField(TEXT("is_spatially_loaded"), Actor->GetIsSpatiallyLoaded());
			Result->SetBoolField(TEXT("changed"), bChanged);
		}
		Results.Add(MakeShared<FJsonValueObject>(Result));
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.hlod.assign_actors_result.v1"));
	OutData->SetNumberField(TEXT("matched_count"), Actors.Num());
	OutData->SetArrayField(TEXT("actor_results"), ActorResults);
	OutData->SetArrayField(TEXT("results"), Results);
	OutData->SetBoolField(TEXT("transaction"), bTransaction);
	return true;
}

bool FUeAgentLegacyMechanics::CmdHLODQueryActors(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	TArray<AActor*> Actors;
	TArray<TSharedPtr<FJsonValue>> ActorResults;
	if (!UeAgentLevelTopology::ResolveActors(World, Ctx.Params, Actors, ActorResults, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> ActorValues;
	for (AActor* Actor : Actors)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Actor ? Actor->GetName() : FString());
		Obj->SetStringField(TEXT("label"), Actor ? Actor->GetActorLabel() : FString());
		Obj->SetStringField(TEXT("hlod_layer"), Actor && Actor->GetHLODLayer() ? UeAgentLevelTopology::ObjectPackagePath(Actor->GetHLODLayer()) : FString());
		Obj->SetStringField(TEXT("runtime_grid"), Actor ? Actor->GetRuntimeGrid().ToString() : FString());
		Obj->SetBoolField(TEXT("is_spatially_loaded"), Actor && Actor->GetIsSpatiallyLoaded());
		ActorValues.Add(MakeShared<FJsonValueObject>(Obj));
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.hlod.query_actors_result.v1"));
	OutData->SetArrayField(TEXT("actors"), ActorValues);
	OutData->SetArrayField(TEXT("actor_results"), ActorResults);
	OutData->SetNumberField(TEXT("count"), ActorValues.Num());
	return true;
}

bool FUeAgentLegacyMechanics::CmdHLODBuild(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString Step = TEXT("stats");
	JsonTryGetString(Ctx.Params, TEXT("step"), Step);
	if (!Step.Equals(TEXT("setup"), ESearchCase::IgnoreCase)
		&& !Step.Equals(TEXT("build"), ESearchCase::IgnoreCase)
		&& !Step.Equals(TEXT("finalize"), ESearchCase::IgnoreCase)
		&& !Step.Equals(TEXT("delete"), ESearchCase::IgnoreCase)
		&& !Step.Equals(TEXT("stats"), ESearchCase::IgnoreCase))
	{
		OutData->SetStringField(TEXT("schema"), TEXT("uai.hlod.build_result.v1"));
		OutData->SetStringField(TEXT("step"), Step);
		OutData->SetStringField(TEXT("error_code"), TEXT("hlod_build_step_invalid"));
		OutError = TEXT("hlod_build_step_invalid");
		return false;
	}
	bool bDryRun = true;
	bool bExecute = false;
	bool bReportOnly = true;
	bool bAllowCommandlet = false;
	bool bAllowRHI = false;
	bool bAllowDestructive = false;
	bool bAllowDelete = false;
	double TimeoutSeconds = 1800.0;
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	JsonTryGetBool(Ctx.Params, TEXT("execute"), bExecute);
	JsonTryGetBool(Ctx.Params, TEXT("report_only"), bReportOnly);
	JsonTryGetBool(Ctx.Params, TEXT("allow_commandlet"), bAllowCommandlet);
	JsonTryGetBool(Ctx.Params, TEXT("allow_rhi"), bAllowRHI);
	JsonTryGetBool(Ctx.Params, TEXT("allow_destructive"), bAllowDestructive);
	JsonTryGetBool(Ctx.Params, TEXT("allow_delete"), bAllowDelete);
	JsonTryGetNumber(Ctx.Params, TEXT("timeout_seconds"), TimeoutSeconds);
	FString BuilderFlag = TEXT("-DumpHLODStats");
	if (Step.Equals(TEXT("setup"), ESearchCase::IgnoreCase)) BuilderFlag = TEXT("-SetupHLODs");
	if (Step.Equals(TEXT("build"), ESearchCase::IgnoreCase)) BuilderFlag = TEXT("-BuildHLODs");
	if (Step.Equals(TEXT("delete"), ESearchCase::IgnoreCase)) BuilderFlag = TEXT("-DeleteHLODs");
	if (Step.Equals(TEXT("finalize"), ESearchCase::IgnoreCase)) BuilderFlag = TEXT("-FinalizeHLODs");
	const FString Args = FString::Printf(TEXT("-run=WorldPartitionBuilderCommandlet -Builder=WorldPartitionHLODsBuilder %s"), *BuilderFlag);
	OutData->SetStringField(TEXT("schema"), TEXT("uai.hlod.build_result.v1"));
	OutData->SetStringField(TEXT("step"), Step);
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	OutData->SetBoolField(TEXT("execute"), bExecute);
	OutData->SetBoolField(TEXT("report_only"), bReportOnly);
	OutData->SetBoolField(TEXT("allow_commandlet"), bAllowCommandlet);
	OutData->SetBoolField(TEXT("allow_rhi"), bAllowRHI);
	OutData->SetBoolField(TEXT("allow_destructive"), bAllowDestructive);
	OutData->SetBoolField(TEXT("allow_delete"), bAllowDelete);
	OutData->SetNumberField(TEXT("timeout_seconds"), TimeoutSeconds);
	OutData->SetStringField(TEXT("commandlet_plan"), Args);
	if (!FMath::IsFinite(TimeoutSeconds) || TimeoutSeconds <= 0.0)
	{
		OutData->SetStringField(TEXT("error_code"), TEXT("timeout_seconds_invalid"));
		OutError = TEXT("timeout_seconds_invalid");
		return false;
	}
	if (bDryRun || !bExecute || bReportOnly)
	{
		OutData->SetStringField(TEXT("status"), TEXT("planned"));
		return true;
	}
	if (!bAllowCommandlet)
	{
		OutData->SetStringField(TEXT("error_code"), TEXT("requires_commandlet"));
		OutError = TEXT("requires_commandlet");
		return false;
	}
	if (!bAllowDestructive)
	{
		OutData->SetStringField(TEXT("error_code"), TEXT("requires_allow_destructive"));
		OutError = TEXT("requires_allow_destructive");
		return false;
	}
	if (Step.Equals(TEXT("delete"), ESearchCase::IgnoreCase) && !bAllowDelete)
	{
		OutData->SetStringField(TEXT("error_code"), TEXT("requires_allow_delete"));
		OutError = TEXT("requires_allow_delete");
		return false;
	}
	if (!bAllowRHI && UeAgentLevelTopology::IsNullRHI() && (Step.Equals(TEXT("build"), ESearchCase::IgnoreCase) || Step.Equals(TEXT("setup"), ESearchCase::IgnoreCase)))
	{
		OutData->SetStringField(TEXT("error_code"), TEXT("requires_rhi"));
		OutError = TEXT("requires_rhi");
		return false;
	}
	const bool bOk = UeAgentLevelTopology::RunCommandletProcess(Args, TimeoutSeconds, OutData, OutError);
	OutData->SetStringField(TEXT("status"), bOk ? TEXT("executed") : TEXT("failed"));
	return bOk;
}

bool FUeAgentLegacyMechanics::CmdHLODValidateBuild(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	int32 HLODActorCount = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if ((*It)->GetClass()->GetName().Contains(TEXT("WorldPartitionHLOD")))
		{
			++HLODActorCount;
		}
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	double ExpectedCountNumber = 0.0;
	double MinimumCountNumber = 0.0;
	const bool bHasExpectedCount = JsonTryGetNumber(Ctx.Params, TEXT("expected_hlod_actor_count"), ExpectedCountNumber);
	const bool bHasMinimumCount = JsonTryGetNumber(Ctx.Params, TEXT("minimum_hlod_actor_count"), MinimumCountNumber);
	if (!bHasExpectedCount && !bHasMinimumCount)
	{
		UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("missing_validation_expectation"), TEXT("$"), TEXT("hlod_validate_build requires expected_hlod_actor_count or minimum_hlod_actor_count."));
	}
	if (bHasExpectedCount && (!FMath::IsFinite(ExpectedCountNumber) || ExpectedCountNumber < 0.0 || !FMath::IsNearlyEqual(ExpectedCountNumber, FMath::RoundToDouble(ExpectedCountNumber))))
	{
		UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("expected_hlod_actor_count_invalid"), TEXT("$.expected_hlod_actor_count"), TEXT("Expected HLOD actor count must be a non-negative integer."));
	}
	else if (bHasExpectedCount && HLODActorCount != static_cast<int32>(ExpectedCountNumber))
	{
		UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("hlod_actor_count_mismatch"), TEXT("$.expected_hlod_actor_count"), TEXT("HLOD actor count does not match the expected value."));
	}
	if (bHasMinimumCount && (!FMath::IsFinite(MinimumCountNumber) || MinimumCountNumber < 0.0 || !FMath::IsNearlyEqual(MinimumCountNumber, FMath::RoundToDouble(MinimumCountNumber))))
	{
		UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("minimum_hlod_actor_count_invalid"), TEXT("$.minimum_hlod_actor_count"), TEXT("Minimum HLOD actor count must be a non-negative integer."));
	}
	else if (bHasMinimumCount && HLODActorCount < static_cast<int32>(MinimumCountNumber))
	{
		UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("hlod_actor_count_below_minimum"), TEXT("$.minimum_hlod_actor_count"), TEXT("HLOD actor count is below the required minimum."));
	}
	const int32 ErrorCount = UeAgentLevelTopology::CountIssues(Issues, TEXT("error"));
	OutData->SetStringField(TEXT("schema"), TEXT("uai.hlod.validate_build_result.v1"));
	OutData->SetNumberField(TEXT("hlod_actor_count"), HLODActorCount);
	OutData->SetStringField(TEXT("status"), ErrorCount == 0 ? TEXT("valid") : TEXT("invalid"));
	OutData->SetArrayField(TEXT("issues"), Issues);
	OutData->SetNumberField(TEXT("error_count"), ErrorCount);
	return ErrorCount == 0;
}

bool FUeAgentLegacyMechanics::CmdLevelTopologySetActorAssignments(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Results;
	TSharedPtr<FJsonObject> Assignments;
	if (!JsonTryGetObject(Ctx.Params, TEXT("assignments"), Assignments))
	{
		Assignments = Ctx.Params;
	}
	if (Assignments->HasField(TEXT("streaming_level")))
	{
		FString TargetLevel;
		Assignments->TryGetStringField(TEXT("streaming_level"), TargetLevel);
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target_level"), TargetLevel);
		if (Ctx.Params->HasField(TEXT("actor_ids")))
		{
			Params->SetArrayField(TEXT("actor_ids"), Ctx.Params->GetArrayField(TEXT("actor_ids")));
		}
		bool bTransaction = true;
		if (Ctx.Params->TryGetBoolField(TEXT("transaction"), bTransaction)) Params->SetBoolField(TEXT("transaction"), bTransaction);
		FUeAgentRequestContext SubCtx = Ctx;
		SubCtx.Params = Params;
		TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
		FString SubError;
		const bool bOk = CmdLevelStreamingMoveActors(SubCtx, SubData, SubError);
		SubData->SetStringField(TEXT("operation"), TEXT("streaming_level"));
		SubData->SetBoolField(TEXT("ok"), bOk);
		SubData->SetStringField(TEXT("error"), SubError);
		Results.Add(MakeShared<FJsonValueObject>(SubData));
		if (!bOk)
		{
			OutError = SubError;
			OutData->SetArrayField(TEXT("results"), Results);
			return false;
		}
	}
	TSharedPtr<FJsonObject> DataLayersObj;
	if (JsonTryGetObject(Assignments, TEXT("data_layers"), DataLayersObj))
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		FString Mode = TEXT("replace");
		DataLayersObj->TryGetStringField(TEXT("mode"), Mode);
		Params->SetStringField(TEXT("mode"), Mode);
		if (DataLayersObj->HasField(TEXT("layers")))
		{
			Params->SetArrayField(TEXT("layers"), DataLayersObj->GetArrayField(TEXT("layers")));
		}
		if (Ctx.Params->HasField(TEXT("actor_ids")))
		{
			Params->SetArrayField(TEXT("actor_ids"), Ctx.Params->GetArrayField(TEXT("actor_ids")));
		}
		bool bTransaction = true;
		if (Ctx.Params->TryGetBoolField(TEXT("transaction"), bTransaction)) Params->SetBoolField(TEXT("transaction"), bTransaction);
		FUeAgentRequestContext SubCtx = Ctx;
		SubCtx.Params = Params;
		TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
		FString SubError;
		const bool bOk = CmdDataLayerSetActorMembership(SubCtx, SubData, SubError);
		SubData->SetStringField(TEXT("operation"), TEXT("data_layers"));
		SubData->SetBoolField(TEXT("ok"), bOk);
		SubData->SetStringField(TEXT("error"), SubError);
		Results.Add(MakeShared<FJsonValueObject>(SubData));
		if (!bOk)
		{
			OutError = SubError;
			OutData->SetArrayField(TEXT("results"), Results);
			return false;
		}
	}
	if (Assignments->HasField(TEXT("hlod_layer")) || Assignments->HasField(TEXT("clear_hlod_layer")) || Assignments->HasField(TEXT("world_partition")))
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		if (Ctx.Params->HasField(TEXT("actor_ids")))
		{
			Params->SetArrayField(TEXT("actor_ids"), Ctx.Params->GetArrayField(TEXT("actor_ids")));
		}
		FString HLODLayer;
		if (Assignments->TryGetStringField(TEXT("hlod_layer"), HLODLayer))
		{
			Params->SetStringField(TEXT("hlod_layer"), HLODLayer);
		}
		bool bClearHLODLayer = false;
		if (Assignments->TryGetBoolField(TEXT("clear_hlod_layer"), bClearHLODLayer))
		{
			Params->SetBoolField(TEXT("clear_hlod_layer"), bClearHLODLayer);
		}
		bool bAllowDestructive = false;
		if (Ctx.Params->TryGetBoolField(TEXT("allow_destructive"), bAllowDestructive))
		{
			Params->SetBoolField(TEXT("allow_destructive"), bAllowDestructive);
		}
		bool bTransaction = true;
		if (Ctx.Params->TryGetBoolField(TEXT("transaction"), bTransaction)) Params->SetBoolField(TEXT("transaction"), bTransaction);
		TSharedPtr<FJsonObject> WpObj;
		if (JsonTryGetObject(Assignments, TEXT("world_partition"), WpObj))
		{
			FString RuntimeGrid;
			if (WpObj->TryGetStringField(TEXT("runtime_grid"), RuntimeGrid))
			{
				Params->SetStringField(TEXT("runtime_grid"), RuntimeGrid);
			}
			bool bSpatial = true;
			if (WpObj->TryGetBoolField(TEXT("is_spatially_loaded"), bSpatial))
			{
				Params->SetBoolField(TEXT("is_spatially_loaded"), bSpatial);
			}
		}
		FUeAgentRequestContext SubCtx = Ctx;
		SubCtx.Params = Params;
		TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
		FString SubError;
		const bool bOk = CmdHLODAssignActors(SubCtx, SubData, SubError);
		SubData->SetStringField(TEXT("operation"), TEXT("hlod_world_partition_actor_fields"));
		SubData->SetBoolField(TEXT("ok"), bOk);
		SubData->SetStringField(TEXT("error"), SubError);
		Results.Add(MakeShared<FJsonValueObject>(SubData));
		if (!bOk)
		{
			OutError = SubError;
			OutData->SetArrayField(TEXT("results"), Results);
			return false;
		}
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.level_topology.assignment_result.v1"));
	OutData->SetArrayField(TEXT("results"), Results);
	OutData->SetBoolField(TEXT("changed"), Results.Num() > 0);
	return true;
}

bool FUeAgentLegacyMechanics::CmdLevelTopologyExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	bool bIncludeStreaming = true;
	bool bIncludeDataLayers = true;
	bool bIncludeWorldPartition = true;
	bool bIncludeHLOD = true;
	bool bIncludeAssignments = false;
	double AssignmentLimitNumber = 100.0;
	JsonTryGetBool(Ctx.Params, TEXT("include_streaming_levels"), bIncludeStreaming);
	JsonTryGetBool(Ctx.Params, TEXT("include_data_layers"), bIncludeDataLayers);
	JsonTryGetBool(Ctx.Params, TEXT("include_world_partition"), bIncludeWorldPartition);
	JsonTryGetBool(Ctx.Params, TEXT("include_hlod"), bIncludeHLOD);
	JsonTryGetBool(Ctx.Params, TEXT("include_actor_assignments"), bIncludeAssignments);
	JsonTryGetNumber(Ctx.Params, TEXT("actor_assignment_limit"), AssignmentLimitNumber);
	OutData->SetStringField(TEXT("schema"), TEXT("uai.level_topology.v1"));
	TSharedPtr<FJsonObject> WorldObj = MakeShared<FJsonObject>();
	WorldObj->SetStringField(TEXT("path"), World->GetPathName());
	WorldObj->SetStringField(TEXT("package"), World->GetOutermost() ? World->GetOutermost()->GetName() : FString());
	OutData->SetObjectField(TEXT("world"), WorldObj);
	if (bIncludeStreaming)
	{
		TSharedPtr<FJsonObject> StreamData = MakeShared<FJsonObject>();
		FUeAgentRequestContext SubCtx = Ctx;
		SubCtx.Params = MakeShared<FJsonObject>();
		FString SubError;
		CmdLevelStreamingQuery(SubCtx, StreamData, SubError);
		OutData->SetArrayField(TEXT("streaming_levels"), StreamData->GetArrayField(TEXT("streaming_levels")));
	}
	if (bIncludeDataLayers)
	{
		TSharedPtr<FJsonObject> DataLayerData = MakeShared<FJsonObject>();
		FUeAgentRequestContext SubCtx = Ctx;
		SubCtx.Params = MakeShared<FJsonObject>();
		SubCtx.Params->SetBoolField(TEXT("include_actors"), false);
		FString SubError;
		CmdDataLayerQuery(SubCtx, DataLayerData, SubError);
		OutData->SetArrayField(TEXT("data_layers"), DataLayerData->GetArrayField(TEXT("data_layers")));
	}
	if (bIncludeWorldPartition)
	{
		TSharedPtr<FJsonObject> WpData = MakeShared<FJsonObject>();
		FUeAgentRequestContext SubCtx = Ctx;
		SubCtx.Params = MakeShared<FJsonObject>();
		FString SubError;
		CmdWorldPartitionQuery(SubCtx, WpData, SubError);
		OutData->SetObjectField(TEXT("world_partition"), WpData);
	}
	if (bIncludeHLOD)
	{
		TSharedPtr<FJsonObject> HLODObj = MakeShared<FJsonObject>();
		HLODObj->SetBoolField(TEXT("available"), true);
		OutData->SetObjectField(TEXT("hlod"), HLODObj);
	}
	if (bIncludeAssignments)
	{
		TArray<TSharedPtr<FJsonValue>> AssignmentValues;
		const int32 AssignmentLimit = FMath::Max(1, static_cast<int32>(AssignmentLimitNumber));
		for (TActorIterator<AActor> It(World); It && AssignmentValues.Num() < AssignmentLimit; ++It)
		{
			AActor* Actor = *It;
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> ActorIds;
			ActorIds.Add(MakeShared<FJsonValueString>(Actor->GetName()));
			Obj->SetArrayField(TEXT("actor_ids"), ActorIds);
			Obj->SetStringField(TEXT("name"), Actor->GetName());
			Obj->SetStringField(TEXT("label"), Actor->GetActorLabel());
			Obj->SetStringField(TEXT("level"), Actor->GetLevel() && Actor->GetLevel()->GetOutermost() ? Actor->GetLevel()->GetOutermost()->GetName() : FString());
			Obj->SetStringField(TEXT("runtime_grid"), Actor->GetRuntimeGrid().ToString());
			Obj->SetBoolField(TEXT("is_spatially_loaded"), Actor->GetIsSpatiallyLoaded());
			Obj->SetStringField(TEXT("hlod_layer"), Actor->GetHLODLayer() ? UeAgentLevelTopology::ObjectPackagePath(Actor->GetHLODLayer()) : FString());
			TArray<TSharedPtr<FJsonValue>> DlValues;
			for (const FName& DlName : Actor->GetDataLayerInstanceNames())
			{
				DlValues.Add(MakeShared<FJsonValueString>(DlName.ToString()));
			}
			Obj->SetArrayField(TEXT("data_layers"), DlValues);
			AssignmentValues.Add(MakeShared<FJsonValueObject>(Obj));
		}
		OutData->SetArrayField(TEXT("actor_assignments"), AssignmentValues);
	}
	TSharedPtr<FJsonObject> FeatureGates = MakeShared<FJsonObject>();
	FeatureGates->SetBoolField(TEXT("level_streaming"), true);
	FeatureGates->SetBoolField(TEXT("data_layer_editor"), UeAgentLevelTopology::GetDataLayerSubsystem() != nullptr);
	FeatureGates->SetBoolField(TEXT("world_partition"), World->GetWorldPartition() != nullptr);
	FeatureGates->SetBoolField(TEXT("hlod"), true);
	OutData->SetObjectField(TEXT("feature_gates"), FeatureGates);
	return UeAgentLevelTopology::SaveJsonIfRequested(OutData, Ctx.Params, OutData, OutError);
}

bool FUeAgentLegacyMechanics::CmdLevelTopologyValidateJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> Root;
	FString Source;
	if (!UeAgentLevelTopology::LoadJsonFromParams(Ctx.Params, Root, Source, OutError))
	{
		return false;
	}
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	const bool bHasExpectation = Root->HasField(TEXT("streaming_levels"))
		|| Root->HasField(TEXT("data_layers"))
		|| Root->HasField(TEXT("actor_assignments"))
		|| Root->HasField(TEXT("world_partition"))
		|| Root->HasField(TEXT("hlod"))
		|| Root->HasField(TEXT("tasks"));
	if (!bHasExpectation)
	{
		UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("missing_validation_expectation"), TEXT("$"), TEXT("Topology input must contain at least one actionable domain."));
	}
	const TArray<TSharedPtr<FJsonValue>>* StreamingLevels = nullptr;
	if (Root->TryGetArrayField(TEXT("streaming_levels"), StreamingLevels) && StreamingLevels)
	{
		for (int32 Index = 0; Index < StreamingLevels->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Item = (*StreamingLevels)[Index].IsValid() ? (*StreamingLevels)[Index]->AsObject() : nullptr;
			FString LevelPackage;
			if (!Item.IsValid() || !Item->TryGetStringField(TEXT("level_package"), LevelPackage) || !UeAgentLevelTopology::IsValidLongPackagePath(LevelPackage))
			{
				UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("streaming_level_package_invalid"), FString::Printf(TEXT("$.streaming_levels[%d].level_package"), Index), TEXT("Streaming level package path is invalid."));
				continue;
			}
			FString Operation = TEXT("ensure");
			Item->TryGetStringField(TEXT("operation"), Operation);
			if (!Operation.Equals(TEXT("ensure"), ESearchCase::IgnoreCase)
				&& !Operation.Equals(TEXT("create"), ESearchCase::IgnoreCase)
				&& !Operation.Equals(TEXT("add_existing"), ESearchCase::IgnoreCase)
				&& !Operation.Equals(TEXT("set_state"), ESearchCase::IgnoreCase)
				&& !Operation.Equals(TEXT("remove"), ESearchCase::IgnoreCase))
			{
				UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("streaming_operation_invalid"), FString::Printf(TEXT("$.streaming_levels[%d].operation"), Index), TEXT("Unknown streaming level operation."));
			}
			if (Operation.Equals(TEXT("add_existing"), ESearchCase::IgnoreCase) && !UeAgentLevelTopology::AssetExists(LevelPackage))
			{
				UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("streaming_level_asset_not_found"), FString::Printf(TEXT("$.streaming_levels[%d].level_package"), Index), TEXT("add_existing requires an existing level package."));
			}
			if ((Operation.Equals(TEXT("remove"), ESearchCase::IgnoreCase) || Operation.Equals(TEXT("set_state"), ESearchCase::IgnoreCase)) && !UeAgentLevelTopology::FindStreamingLevel(World, LevelPackage))
			{
				UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("streaming_level_not_found"), FString::Printf(TEXT("$.streaming_levels[%d].level_package"), Index), TEXT("Requested streaming level is not present in the current world."));
			}
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* DataLayers = nullptr;
	if (Root->TryGetArrayField(TEXT("data_layers"), DataLayers) && DataLayers)
	{
		for (int32 Index = 0; Index < DataLayers->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Item = (*DataLayers)[Index].IsValid() ? (*DataLayers)[Index]->AsObject() : nullptr;
			FString AssetPath;
			if (!Item.IsValid() || !Item->TryGetStringField(TEXT("asset_path"), AssetPath))
			{
				UeAgentLevelTopology::AddIssue(Issues, TEXT("warning"), TEXT("data_layer_asset_not_specified"), FString::Printf(TEXT("$.data_layers[%d].asset_path"), Index), TEXT("DataLayer asset path is not specified."));
				continue;
			}
			if (!UeAgentLevelTopology::IsValidLongPackagePath(AssetPath))
			{
				UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("data_layer_asset_path_invalid"), FString::Printf(TEXT("$.data_layers[%d].asset_path"), Index), TEXT("DataLayer asset path is not a valid long package name."));
			}
			FString Operation = TEXT("ensure");
			Item->TryGetStringField(TEXT("operation"), Operation);
			if (!Operation.Equals(TEXT("ensure"), ESearchCase::IgnoreCase) && !Operation.Equals(TEXT("delete"), ESearchCase::IgnoreCase))
			{
				UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("data_layer_operation_invalid"), FString::Printf(TEXT("$.data_layers[%d].operation"), Index), TEXT("Unknown DataLayer operation."));
			}
			FString MembershipMode;
			if (Item->TryGetStringField(TEXT("membership_mode"), MembershipMode)
				&& !MembershipMode.Equals(TEXT("add"), ESearchCase::IgnoreCase)
				&& !MembershipMode.Equals(TEXT("remove"), ESearchCase::IgnoreCase)
				&& !MembershipMode.Equals(TEXT("replace"), ESearchCase::IgnoreCase)
				&& !MembershipMode.Equals(TEXT("clear"), ESearchCase::IgnoreCase))
			{
				UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("data_layer_membership_mode_invalid"), FString::Printf(TEXT("$.data_layers[%d].membership_mode"), Index), TEXT("Unknown DataLayer membership mode."));
			}
			bool bCreateInstance = false;
			Item->TryGetBoolField(TEXT("create_instance"), bCreateInstance);
			if (bCreateInstance && !UeAgentLevelTopology::SupportsDataLayerInstanceAuthoring(World))
			{
				UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("level_external_objects_required_for_data_layer"), FString::Printf(TEXT("$.data_layers[%d]"), Index), TEXT("DataLayer instance authoring requires the persistent level to use external objects."));
			}
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* ActorAssignments = nullptr;
	if (Root->TryGetArrayField(TEXT("actor_assignments"), ActorAssignments) && ActorAssignments)
	{
		for (int32 Index = 0; Index < ActorAssignments->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Item = (*ActorAssignments)[Index].IsValid() ? (*ActorAssignments)[Index]->AsObject() : nullptr;
			TArray<AActor*> Actors;
			TArray<TSharedPtr<FJsonValue>> ActorResults;
			FString ActorError;
			if (!Item.IsValid() || !UeAgentLevelTopology::ResolveActors(World, Item, Actors, ActorResults, ActorError) || Actors.IsEmpty())
			{
				UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("actor_assignment_selection_invalid"), FString::Printf(TEXT("$.actor_assignments[%d]"), Index), ActorError.IsEmpty() ? FString(TEXT("Actor assignment has no valid actor selection.")) : ActorError);
			}
		}
	}
	const TSharedPtr<FJsonObject>* WorldPartitionObj = nullptr;
	if (Root->TryGetObjectField(TEXT("world_partition"), WorldPartitionObj) && WorldPartitionObj && WorldPartitionObj->IsValid())
	{
		bool bDeclaredEnabled = true;
		const bool bHasDeclaredEnabled = (*WorldPartitionObj)->TryGetBoolField(TEXT("enabled"), bDeclaredEnabled);
		const bool bRequestsWpWrite = ((*WorldPartitionObj)->HasField(TEXT("streaming_enabled")) && (!bHasDeclaredEnabled || bDeclaredEnabled))
			|| (*WorldPartitionObj)->HasField(TEXT("load_regions"))
			|| (*WorldPartitionObj)->HasField(TEXT("load_actors"))
			|| (*WorldPartitionObj)->HasField(TEXT("generate_streaming"));
		if (bRequestsWpWrite && !World->GetWorldPartition())
		{
			UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("world_partition_not_enabled"), TEXT("$.world_partition"), TEXT("The current world is not a World Partition world."));
		}
	}
	const TSharedPtr<FJsonObject>* HLODObj = nullptr;
	if (Root->TryGetObjectField(TEXT("hlod"), HLODObj) && HLODObj && HLODObj->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Layers = nullptr;
		if ((*HLODObj)->TryGetArrayField(TEXT("layers"), Layers) && Layers)
		{
			for (int32 Index = 0; Index < Layers->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Item = (*Layers)[Index].IsValid() ? (*Layers)[Index]->AsObject() : nullptr;
				FString AssetPath;
				if (!Item.IsValid() || !Item->TryGetStringField(TEXT("asset_path"), AssetPath) || !UeAgentLevelTopology::IsValidLongPackagePath(AssetPath))
				{
					UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("hlod_layer_path_invalid"), FString::Printf(TEXT("$.hlod.layers[%d].asset_path"), Index), TEXT("HLODLayer asset path is invalid."));
				}
				FString LayerTypeText;
				if (Item.IsValid() && Item->TryGetStringField(TEXT("layer_type"), LayerTypeText))
				{
					EHLODLayerType ParsedType = EHLODLayerType::Instancing;
					if (!UeAgentLevelTopology::ParseHLODLayerType(LayerTypeText, ParsedType))
					{
						UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("hlod_layer_type_invalid"), FString::Printf(TEXT("$.hlod.layers[%d].layer_type"), Index), TEXT("Unknown HLOD layer type."));
					}
				}
			}
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* Tasks = nullptr;
	if (Root->TryGetArrayField(TEXT("tasks"), Tasks) && Tasks)
	{
		for (int32 Index = 0; Index < Tasks->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Task = (*Tasks)[Index].IsValid() ? (*Tasks)[Index]->AsObject() : nullptr;
			FString Type;
			if (!Task.IsValid() || (!Task->TryGetStringField(TEXT("type"), Type) && !Task->TryGetStringField(TEXT("command"), Type)))
			{
				UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("task_type_missing"), FString::Printf(TEXT("$.tasks[%d]"), Index), TEXT("Topology task requires type or command."));
			}
			else if (!Type.Equals(TEXT("hlod_build"), ESearchCase::IgnoreCase)
				&& !Type.Equals(TEXT("world_partition_generate_streaming"), ESearchCase::IgnoreCase))
			{
				UeAgentLevelTopology::AddIssue(Issues, TEXT("error"), TEXT("task_type_unsupported"), FString::Printf(TEXT("$.tasks[%d].type"), Index), TEXT("Unsupported topology task type."));
			}
		}
	}
	const int32 ErrorCount = UeAgentLevelTopology::CountIssues(Issues, TEXT("error"));
	OutData->SetStringField(TEXT("schema"), TEXT("uai.level_topology.validate_result.v1"));
	OutData->SetStringField(TEXT("source"), Source);
	OutData->SetStringField(TEXT("status"), ErrorCount == 0 ? TEXT("valid") : TEXT("invalid"));
	OutData->SetArrayField(TEXT("issues"), Issues);
	OutData->SetNumberField(TEXT("error_count"), ErrorCount);
	return ErrorCount == 0;
}

bool FUeAgentLegacyMechanics::CmdLevelTopologyDiffJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> Root;
	FString Source;
	if (!UeAgentLevelTopology::LoadJsonFromParams(Ctx.Params, Root, Source, OutError))
	{
		return false;
	}
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Diffs;
	auto AddDiff = [&Diffs](const FString& Type, const FString& Path, const FString& Message)
	{
		TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetStringField(TEXT("type"), Type);
		Diff->SetStringField(TEXT("path"), Path);
		Diff->SetStringField(TEXT("message"), Message);
		Diffs.Add(MakeShared<FJsonValueObject>(Diff));
		return Diff;
	};

	const TArray<TSharedPtr<FJsonValue>>* StreamingLevels = nullptr;
	if (Root->TryGetArrayField(TEXT("streaming_levels"), StreamingLevels) && StreamingLevels)
	{
		TSet<FString> DesiredStreamingPackages;
		for (const TSharedPtr<FJsonValue>& Value : *StreamingLevels)
		{
			const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr;
			FString LevelPackage;
			if (!Item.IsValid() || !Item->TryGetStringField(TEXT("level_package"), LevelPackage))
			{
				continue;
			}
			LevelPackage = UeAgentLevelTopology::NormalizePackagePath(LevelPackage);
			DesiredStreamingPackages.Add(LevelPackage);
			ULevelStreaming* StreamingLevel = UeAgentLevelTopology::FindStreamingLevel(World, LevelPackage);
			if (!StreamingLevel)
			{
				TSharedPtr<FJsonObject> Diff = AddDiff(TEXT("streaming_level_added"), TEXT("$.streaming_levels"), TEXT("Target streaming level is missing from the current world."));
				Diff->SetStringField(TEXT("level_package"), LevelPackage);
				continue;
			}
			bool ExpectedLoaded = false;
			if (Item->TryGetBoolField(TEXT("loaded"), ExpectedLoaded) && StreamingLevel->IsLevelLoaded() != ExpectedLoaded)
			{
				TSharedPtr<FJsonObject> Diff = AddDiff(TEXT("streaming_state_changed"), LevelPackage, TEXT("Streaming loaded state differs."));
				Diff->SetStringField(TEXT("field"), TEXT("loaded"));
				Diff->SetBoolField(TEXT("current"), StreamingLevel->IsLevelLoaded());
				Diff->SetBoolField(TEXT("target"), ExpectedLoaded);
			}
			bool ExpectedVisible = false;
			if (Item->TryGetBoolField(TEXT("visible"), ExpectedVisible) && StreamingLevel->IsLevelVisible() != ExpectedVisible)
			{
				TSharedPtr<FJsonObject> Diff = AddDiff(TEXT("streaming_state_changed"), LevelPackage, TEXT("Streaming visible state differs."));
				Diff->SetStringField(TEXT("field"), TEXT("visible"));
				Diff->SetBoolField(TEXT("current"), StreamingLevel->IsLevelVisible());
				Diff->SetBoolField(TEXT("target"), ExpectedVisible);
			}
		}
		for (ULevelStreaming* StreamingLevel : World->GetStreamingLevels())
		{
			if (!StreamingLevel)
			{
				continue;
			}
			const FString CurrentPackage = UeAgentLevelTopology::NormalizePackagePath(StreamingLevel->GetWorldAssetPackageName());
			if (!CurrentPackage.IsEmpty() && !DesiredStreamingPackages.Contains(CurrentPackage))
			{
				TSharedPtr<FJsonObject> Diff = AddDiff(TEXT("streaming_level_removed"), CurrentPackage, TEXT("Current streaming level is not present in the target topology."));
				Diff->SetStringField(TEXT("level_package"), CurrentPackage);
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* DataLayers = nullptr;
	if (Root->TryGetArrayField(TEXT("data_layers"), DataLayers) && DataLayers)
	{
		TArray<UDataLayerInstance*> DesiredInstances;
		for (int32 Index = 0; Index < DataLayers->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Item = (*DataLayers)[Index].IsValid() ? (*DataLayers)[Index]->AsObject() : nullptr;
			if (!Item.IsValid())
			{
				continue;
			}
			FString LayerKey;
			if (!Item->TryGetStringField(TEXT("layer"), LayerKey) && !Item->TryGetStringField(TEXT("instance_name"), LayerKey))
			{
				Item->TryGetStringField(TEXT("asset_path"), LayerKey);
			}
			if (LayerKey.IsEmpty())
			{
				continue;
			}
			UDataLayerInstance* Instance = UeAgentLevelTopology::FindDataLayerInstance(World, LayerKey);
			if (!Instance)
			{
				TSharedPtr<FJsonObject> Diff = AddDiff(TEXT("data_layer_added"), FString::Printf(TEXT("$.data_layers[%d]"), Index), TEXT("Target DataLayer instance is missing from the current world."));
				Diff->SetStringField(TEXT("layer"), LayerKey);
				continue;
			}
			DesiredInstances.AddUnique(Instance);
			FString ExpectedRuntimeState;
			if (Item->TryGetStringField(TEXT("initial_runtime_state"), ExpectedRuntimeState))
			{
				EDataLayerRuntimeState ParsedState = EDataLayerRuntimeState::Unloaded;
				if (UeAgentLevelTopology::ParseDataLayerRuntimeState(ExpectedRuntimeState, ParsedState) && Instance->GetInitialRuntimeState() != ParsedState)
				{
					TSharedPtr<FJsonObject> Diff = AddDiff(TEXT("data_layer_state_changed"), LayerKey, TEXT("DataLayer initial runtime state differs."));
					Diff->SetStringField(TEXT("field"), TEXT("initial_runtime_state"));
					Diff->SetStringField(TEXT("current"), GetDataLayerRuntimeStateName(Instance->GetInitialRuntimeState()));
					Diff->SetStringField(TEXT("target"), ExpectedRuntimeState);
				}
			}
			bool ExpectedVisible = false;
			if (Item->TryGetBoolField(TEXT("visible"), ExpectedVisible) && Instance->IsVisible() != ExpectedVisible)
			{
				TSharedPtr<FJsonObject> Diff = AddDiff(TEXT("data_layer_state_changed"), LayerKey, TEXT("DataLayer visibility differs."));
				Diff->SetStringField(TEXT("field"), TEXT("visible"));
				Diff->SetBoolField(TEXT("current"), Instance->IsVisible());
				Diff->SetBoolField(TEXT("target"), ExpectedVisible);
			}
			bool ExpectedLoaded = false;
			if (Item->TryGetBoolField(TEXT("loaded_in_editor"), ExpectedLoaded) && Instance->IsLoadedInEditor() != ExpectedLoaded)
			{
				TSharedPtr<FJsonObject> Diff = AddDiff(TEXT("data_layer_state_changed"), LayerKey, TEXT("DataLayer editor loaded state differs."));
				Diff->SetStringField(TEXT("field"), TEXT("loaded_in_editor"));
				Diff->SetBoolField(TEXT("current"), Instance->IsLoadedInEditor());
				Diff->SetBoolField(TEXT("target"), ExpectedLoaded);
			}
		}
		if (AWorldDataLayers* WorldDataLayers = UeAgentLevelTopology::FindWorldDataLayers(World))
		{
			WorldDataLayers->ForEachDataLayerInstance([&DesiredInstances, &AddDiff](UDataLayerInstance* Instance)
			{
				if (Instance && !DesiredInstances.Contains(Instance))
				{
					TSharedPtr<FJsonObject> Diff = AddDiff(TEXT("data_layer_removed"), Instance->GetDataLayerFName().ToString(), TEXT("Current DataLayer instance is not present in the target topology."));
					Diff->SetStringField(TEXT("instance_name"), Instance->GetDataLayerFName().ToString());
				}
				return true;
			});
		}
	}

	const TSharedPtr<FJsonObject>* WorldPartitionObj = nullptr;
	if (Root->TryGetObjectField(TEXT("world_partition"), WorldPartitionObj) && WorldPartitionObj && WorldPartitionObj->IsValid())
	{
		if (UWorldPartition* WorldPartition = World->GetWorldPartition())
		{
			bool ExpectedStreaming = false;
			if ((*WorldPartitionObj)->TryGetBoolField(TEXT("streaming_enabled"), ExpectedStreaming) && WorldPartition->IsStreamingEnabledInEditor() != ExpectedStreaming)
			{
				TSharedPtr<FJsonObject> Diff = AddDiff(TEXT("world_partition_streaming_changed"), TEXT("$.world_partition.streaming_enabled"), TEXT("World Partition streaming enabled state differs."));
				Diff->SetBoolField(TEXT("current"), WorldPartition->IsStreamingEnabledInEditor());
				Diff->SetBoolField(TEXT("target"), ExpectedStreaming);
			}
			const TArray<TSharedPtr<FJsonValue>>* LoadRegions = nullptr;
			if ((*WorldPartitionObj)->TryGetArrayField(TEXT("load_regions"), LoadRegions) && LoadRegions && LoadRegions->Num() > 0)
			{
				AddDiff(TEXT("world_partition_region_load_changed"), TEXT("$.world_partition.load_regions"), TEXT("Target topology requests World Partition editor region loaders."));
			}
		}
		else
		{
			AddDiff(TEXT("world_partition_streaming_changed"), TEXT("$.world_partition"), TEXT("Target topology references World Partition but the current world is not partitioned."));
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* ActorAssignments = nullptr;
	if (Root->TryGetArrayField(TEXT("actor_assignments"), ActorAssignments) && ActorAssignments)
	{
		for (int32 Index = 0; Index < ActorAssignments->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Item = (*ActorAssignments)[Index].IsValid() ? (*ActorAssignments)[Index]->AsObject() : nullptr;
			if (!Item.IsValid())
			{
				continue;
			}
			const TSharedPtr<FJsonObject>* AssignmentsPtr = nullptr;
			const TSharedPtr<FJsonObject> Assignments = (Item->TryGetObjectField(TEXT("assignments"), AssignmentsPtr) && AssignmentsPtr && AssignmentsPtr->IsValid()) ? *AssignmentsPtr : Item;
			TArray<AActor*> Actors;
			TArray<TSharedPtr<FJsonValue>> ActorResults;
			FString ActorError;
			if (!UeAgentLevelTopology::ResolveActors(World, Item, Actors, ActorResults, ActorError))
			{
				AddDiff(TEXT("actor_assignment_selection_invalid"), FString::Printf(TEXT("$.actor_assignments[%d]"), Index), ActorError);
				continue;
			}
			for (AActor* Actor : Actors)
			{
				if (!Actor)
				{
					continue;
				}
				const FString ActorLabel = Actor->GetActorLabel();
				FString TargetLevel;
				if (Assignments->TryGetStringField(TEXT("streaming_level"), TargetLevel))
				{
					const FString CurrentLevel = Actor->GetLevel() && Actor->GetLevel()->GetOutermost() ? UeAgentLevelTopology::NormalizePackagePath(Actor->GetLevel()->GetOutermost()->GetName()) : FString();
					if (!CurrentLevel.Equals(UeAgentLevelTopology::NormalizePackagePath(TargetLevel), ESearchCase::IgnoreCase))
					{
						TSharedPtr<FJsonObject> Diff = AddDiff(TEXT("actor_level_changed"), ActorLabel, TEXT("Actor level assignment differs."));
						Diff->SetStringField(TEXT("current"), CurrentLevel);
						Diff->SetStringField(TEXT("target"), UeAgentLevelTopology::NormalizePackagePath(TargetLevel));
					}
				}
				TArray<FString> ExpectedDataLayers = UeAgentLevelTopology::ReadStringArray(Assignments, TEXT("data_layers"));
				if (!ExpectedDataLayers.IsEmpty())
				{
					bool bMembershipDiffers = false;
					for (const FString& ExpectedLayer : ExpectedDataLayers)
					{
						UDataLayerInstance* Layer = UeAgentLevelTopology::FindDataLayerInstance(World, ExpectedLayer);
						if (!Layer || !UeAgentLevelTopology::ActorContainsDataLayerInstance(Actor, Layer))
						{
							bMembershipDiffers = true;
							break;
						}
					}
					if (bMembershipDiffers || Actor->GetDataLayerInstanceNames().Num() != ExpectedDataLayers.Num())
					{
						TSharedPtr<FJsonObject> Diff = AddDiff(TEXT("actor_data_layer_membership_changed"), ActorLabel, TEXT("Actor DataLayer membership differs."));
						Diff->SetNumberField(TEXT("current_count"), Actor->GetDataLayerInstanceNames().Num());
						Diff->SetNumberField(TEXT("target_count"), ExpectedDataLayers.Num());
					}
				}
				FString HLODLayerPath;
				if (Assignments->TryGetStringField(TEXT("hlod_layer"), HLODLayerPath))
				{
					const FString CurrentHLOD = Actor->GetHLODLayer() ? UeAgentLevelTopology::ObjectPackagePath(Actor->GetHLODLayer()) : FString();
					if (!CurrentHLOD.Equals(UeAgentLevelTopology::NormalizePackagePath(HLODLayerPath), ESearchCase::IgnoreCase))
					{
						TSharedPtr<FJsonObject> Diff = AddDiff(TEXT("actor_hlod_layer_changed"), ActorLabel, TEXT("Actor HLODLayer assignment differs."));
						Diff->SetStringField(TEXT("current"), CurrentHLOD);
						Diff->SetStringField(TEXT("target"), UeAgentLevelTopology::NormalizePackagePath(HLODLayerPath));
					}
				}
				const TSharedPtr<FJsonObject>* WpActorObj = nullptr;
				if (Assignments->TryGetObjectField(TEXT("world_partition"), WpActorObj) && WpActorObj && WpActorObj->IsValid())
				{
					FString RuntimeGrid;
					if ((*WpActorObj)->TryGetStringField(TEXT("runtime_grid"), RuntimeGrid) && !Actor->GetRuntimeGrid().ToString().Equals(RuntimeGrid, ESearchCase::IgnoreCase))
					{
						TSharedPtr<FJsonObject> Diff = AddDiff(TEXT("world_partition_streaming_changed"), ActorLabel, TEXT("Actor runtime grid differs."));
						Diff->SetStringField(TEXT("field"), TEXT("runtime_grid"));
						Diff->SetStringField(TEXT("current"), Actor->GetRuntimeGrid().ToString());
						Diff->SetStringField(TEXT("target"), RuntimeGrid);
					}
					bool bSpatial = true;
					if ((*WpActorObj)->TryGetBoolField(TEXT("is_spatially_loaded"), bSpatial) && Actor->GetIsSpatiallyLoaded() != bSpatial)
					{
						TSharedPtr<FJsonObject> Diff = AddDiff(TEXT("world_partition_streaming_changed"), ActorLabel, TEXT("Actor spatial loading flag differs."));
						Diff->SetStringField(TEXT("field"), TEXT("is_spatially_loaded"));
						Diff->SetBoolField(TEXT("current"), Actor->GetIsSpatiallyLoaded());
						Diff->SetBoolField(TEXT("target"), bSpatial);
					}
				}
			}
		}
	}

	const TSharedPtr<FJsonObject>* HLODObj = nullptr;
	if (Root->TryGetObjectField(TEXT("hlod"), HLODObj) && HLODObj && HLODObj->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Layers = nullptr;
		if ((*HLODObj)->TryGetArrayField(TEXT("layers"), Layers) && Layers)
		{
			for (int32 Index = 0; Index < Layers->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Item = (*Layers)[Index].IsValid() ? (*Layers)[Index]->AsObject() : nullptr;
				FString AssetPath;
				if (Item.IsValid() && Item->TryGetStringField(TEXT("asset_path"), AssetPath) && !UeAgentLevelTopology::AssetExists(AssetPath))
				{
					TSharedPtr<FJsonObject> Diff = AddDiff(TEXT("hlod_layer_asset_changed"), FString::Printf(TEXT("$.hlod.layers[%d]"), Index), TEXT("Target HLODLayer asset is missing."));
					Diff->SetStringField(TEXT("asset_path"), UeAgentLevelTopology::NormalizePackagePath(AssetPath));
				}
			}
		}
		bool bBuildRequired = false;
		if ((*HLODObj)->TryGetBoolField(TEXT("build_required"), bBuildRequired) && bBuildRequired)
		{
			AddDiff(TEXT("hlod_build_required"), TEXT("$.hlod.build_required"), TEXT("Target topology explicitly requests an HLOD build task."));
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Tasks = nullptr;
	if (Root->TryGetArrayField(TEXT("tasks"), Tasks) && Tasks)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Tasks)
		{
			const TSharedPtr<FJsonObject> Task = Value.IsValid() ? Value->AsObject() : nullptr;
			FString Type;
			if (Task.IsValid() && (Task->TryGetStringField(TEXT("type"), Type) || Task->TryGetStringField(TEXT("command"), Type)) && Type.Equals(TEXT("hlod_build"), ESearchCase::IgnoreCase))
			{
				AddDiff(TEXT("hlod_build_required"), TEXT("$.tasks"), TEXT("Target topology requests an HLOD build task."));
			}
		}
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.level_topology.diff_result.v1"));
	OutData->SetStringField(TEXT("source"), Source);
	OutData->SetArrayField(TEXT("diffs"), Diffs);
	OutData->SetNumberField(TEXT("diff_count"), Diffs.Num());
	OutData->SetBoolField(TEXT("changed"), Diffs.Num() > 0);
	return true;
}

bool FUeAgentLegacyMechanics::CmdLevelTopologyApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> Root;
	FString Source;
	if (!UeAgentLevelTopology::LoadJsonFromParams(Ctx.Params, Root, Source, OutError))
	{
		return false;
	}
	bool bDryRun = true;
	bool bAllowDestructive = false;
	bool bAllowDelete = false;
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	JsonTryGetBool(Ctx.Params, TEXT("allow_destructive"), bAllowDestructive);
	JsonTryGetBool(Ctx.Params, TEXT("allow_delete"), bAllowDelete);
	TArray<TSharedPtr<FJsonValue>> Results;
	TSharedPtr<FJsonObject> PreflightData = MakeShared<FJsonObject>();
	FString PreflightError;
	const bool bPreflightOk = CmdLevelTopologyValidateJson(Ctx, PreflightData, PreflightError);
	if (!bPreflightOk)
	{
		OutData->SetStringField(TEXT("schema"), TEXT("uai.level_topology.apply_result.v1"));
		OutData->SetStringField(TEXT("source"), Source);
		OutData->SetStringField(TEXT("status"), TEXT("preflight_failed"));
		OutData->SetBoolField(TEXT("dry_run"), bDryRun);
		OutData->SetBoolField(TEXT("partial_commit"), false);
		OutData->SetBoolField(TEXT("partial_commit_possible"), false);
		OutData->SetObjectField(TEXT("preflight"), PreflightData);
		OutData->SetArrayField(TEXT("results"), Results);
		OutError = PreflightError.IsEmpty() ? TEXT("topology_preflight_failed") : PreflightError;
		return false;
	}
	if (!bDryRun && !ValidateTopologyDestructiveAuthorization(Root, bAllowDestructive, bAllowDelete, PreflightError))
	{
		OutData->SetStringField(TEXT("schema"), TEXT("uai.level_topology.apply_result.v1"));
		OutData->SetStringField(TEXT("source"), Source);
		OutData->SetStringField(TEXT("status"), TEXT("preflight_failed"));
		OutData->SetBoolField(TEXT("dry_run"), false);
		OutData->SetBoolField(TEXT("partial_commit"), false);
		OutData->SetBoolField(TEXT("partial_commit_possible"), false);
		OutData->SetStringField(TEXT("preflight_error"), PreflightError);
		OutData->SetObjectField(TEXT("preflight"), PreflightData);
		OutData->SetArrayField(TEXT("results"), Results);
		OutError = PreflightError;
		return false;
	}
	if (bDryRun)
	{
		OutData->SetStringField(TEXT("schema"), TEXT("uai.level_topology.apply_result.v1"));
		OutData->SetStringField(TEXT("source"), Source);
		OutData->SetStringField(TEXT("status"), TEXT("planned"));
		OutData->SetBoolField(TEXT("dry_run"), true);
		OutData->SetBoolField(TEXT("changed"), false);
		OutData->SetBoolField(TEXT("partial_commit"), false);
		OutData->SetBoolField(TEXT("partial_commit_possible"), false);
		OutData->SetObjectField(TEXT("preflight"), PreflightData);
		OutData->SetArrayField(TEXT("results"), Results);
		return true;
	}
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	auto FailApply = [&OutData, &OutError, &Results, &Source, &PreflightData](const FString& Error)
	{
		int32 CompletedCount = 0;
		FString FailedOperation;
		for (const TSharedPtr<FJsonValue>& Value : Results)
		{
			const TSharedPtr<FJsonObject> Result = Value.IsValid() ? Value->AsObject() : nullptr;
			bool bOk = false;
			if (Result.IsValid() && Result->TryGetBoolField(TEXT("ok"), bOk) && bOk)
			{
				++CompletedCount;
			}
			else if (Result.IsValid())
			{
				Result->TryGetStringField(TEXT("operation"), FailedOperation);
			}
		}
		OutData->SetStringField(TEXT("schema"), TEXT("uai.level_topology.apply_result.v1"));
		OutData->SetStringField(TEXT("source"), Source);
		OutData->SetStringField(TEXT("status"), CompletedCount > 0 ? TEXT("partial_failure") : TEXT("failed"));
		OutData->SetBoolField(TEXT("dry_run"), false);
		OutData->SetBoolField(TEXT("changed"), CompletedCount > 0);
		OutData->SetBoolField(TEXT("partial_commit"), CompletedCount > 0);
		OutData->SetBoolField(TEXT("partial_commit_possible"), true);
		OutData->SetNumberField(TEXT("completed_count"), CompletedCount);
		OutData->SetStringField(TEXT("failed_operation"), FailedOperation);
		OutData->SetStringField(TEXT("failed_error"), Error);
		OutData->SetObjectField(TEXT("preflight"), PreflightData);
		OutData->SetArrayField(TEXT("results"), Results);
		OutError = Error.IsEmpty() ? TEXT("topology_apply_failed") : Error;
		return false;
	};
	const TArray<TSharedPtr<FJsonValue>>* StreamingLevels = nullptr;
	if (Root->TryGetArrayField(TEXT("streaming_levels"), StreamingLevels) && StreamingLevels)
	{
		for (const TSharedPtr<FJsonValue>& Value : *StreamingLevels)
		{
			const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Item.IsValid())
			{
				continue;
			}
			FUeAgentRequestContext SubCtx = Ctx;
			SubCtx.Params = CopyParamsWithExecutionAuthorization(Item, bAllowDestructive, bAllowDelete);
			TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
			FString SubError;
			FString Operation = TEXT("ensure");
			Item->TryGetStringField(TEXT("operation"), Operation);
			bool bOk = false;
			if (Operation.Equals(TEXT("remove"), ESearchCase::IgnoreCase))
			{
				bOk = CmdLevelStreamingRemove(SubCtx, SubData, SubError);
			}
			else if (Operation.Equals(TEXT("set_state"), ESearchCase::IgnoreCase))
			{
				bOk = CmdLevelStreamingSetState(SubCtx, SubData, SubError);
			}
			else if (Operation.Equals(TEXT("add_existing"), ESearchCase::IgnoreCase))
			{
				bOk = CmdLevelStreamingAddExisting(SubCtx, SubData, SubError);
			}
			else
			{
				FString LevelPackage;
				Item->TryGetStringField(TEXT("level_package"), LevelPackage);
				if (ULevelStreaming* Existing = UeAgentLevelTopology::FindStreamingLevel(World, LevelPackage))
				{
					SubData = UeAgentLevelTopology::BuildStreamingLevelJson(Existing, World);
					SubData->SetStringField(TEXT("status"), TEXT("already_present"));
					bOk = true;
				}
				else
				{
					bOk = CmdLevelStreamingCreateLevel(SubCtx, SubData, SubError);
				}
			}
			if (bOk && !Operation.Equals(TEXT("remove"), ESearchCase::IgnoreCase) && !Operation.Equals(TEXT("set_state"), ESearchCase::IgnoreCase)
				&& (Item->HasField(TEXT("loaded")) || Item->HasField(TEXT("visible")) || Item->HasField(TEXT("current")) || Item->HasField(TEXT("transform"))))
			{
				TSharedPtr<FJsonObject> StateData = MakeShared<FJsonObject>();
				FString StateError;
				const bool bStateOk = CmdLevelStreamingSetState(SubCtx, StateData, StateError);
				SubData->SetObjectField(TEXT("state_result"), StateData);
				SubData->SetBoolField(TEXT("state_ok"), bStateOk);
				SubData->SetStringField(TEXT("state_error"), StateError);
				bOk = bStateOk;
				if (!bOk)
				{
					SubError = StateError;
				}
			}
			SubData->SetStringField(TEXT("operation"), TEXT("streaming_level"));
			SubData->SetBoolField(TEXT("ok"), bOk);
			SubData->SetStringField(TEXT("error"), SubError);
			Results.Add(MakeShared<FJsonValueObject>(SubData));
			if (!bOk)
			{
				return FailApply(SubError);
			}
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* DataLayers = nullptr;
	if (Root->TryGetArrayField(TEXT("data_layers"), DataLayers) && DataLayers)
	{
		for (const TSharedPtr<FJsonValue>& Value : *DataLayers)
		{
			const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Item.IsValid())
			{
				continue;
			}
			FUeAgentRequestContext SubCtx = Ctx;
			SubCtx.Params = CopyParamsWithExecutionAuthorization(Item, bAllowDestructive, bAllowDelete);
			TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
			FString SubError;
			bool bOk = true;
			FString Operation = TEXT("ensure");
			Item->TryGetStringField(TEXT("operation"), Operation);
			if (Operation.Equals(TEXT("delete"), ESearchCase::IgnoreCase))
			{
				bOk = CmdDataLayerDeleteInstance(SubCtx, SubData, SubError);
			}
			else if (Item->HasField(TEXT("asset_path")))
			{
				bOk = CmdDataLayerCreateAsset(SubCtx, SubData, SubError);
			}
			bool bCreateInstance = false;
			Item->TryGetBoolField(TEXT("create_instance"), bCreateInstance);
			if (bOk && !Operation.Equals(TEXT("delete"), ESearchCase::IgnoreCase) && bCreateInstance)
			{
				bOk = CmdDataLayerCreateInstance(SubCtx, SubData, SubError);
			}
			if (bOk && !Operation.Equals(TEXT("delete"), ESearchCase::IgnoreCase)
				&& (Item->HasField(TEXT("visible")) || Item->HasField(TEXT("loaded_in_editor")) || Item->HasField(TEXT("initial_runtime_state")) || Item->HasField(TEXT("initially_visible")) || Item->HasField(TEXT("short_name"))))
			{
				TSharedPtr<FJsonObject> StateParams = MakeShared<FJsonObject>();
				FString LayerKey;
				if (!Item->TryGetStringField(TEXT("layer"), LayerKey) && !Item->TryGetStringField(TEXT("instance_name"), LayerKey))
				{
					Item->TryGetStringField(TEXT("asset_path"), LayerKey);
				}
				StateParams->SetStringField(TEXT("layer"), LayerKey);
				bool BoolValue = false;
				FString StringValue;
				if (Item->TryGetBoolField(TEXT("visible"), BoolValue)) StateParams->SetBoolField(TEXT("visible"), BoolValue);
				if (Item->TryGetBoolField(TEXT("loaded_in_editor"), BoolValue)) StateParams->SetBoolField(TEXT("loaded_in_editor"), BoolValue);
				if (Item->TryGetBoolField(TEXT("initially_visible"), BoolValue)) StateParams->SetBoolField(TEXT("initially_visible"), BoolValue);
				if (Item->TryGetStringField(TEXT("initial_runtime_state"), StringValue)) StateParams->SetStringField(TEXT("initial_runtime_state"), StringValue);
				if (Item->TryGetStringField(TEXT("short_name"), StringValue)) StateParams->SetStringField(TEXT("short_name"), StringValue);
				FUeAgentRequestContext StateCtx = Ctx;
				StateCtx.Params = StateParams;
				TSharedPtr<FJsonObject> StateData = MakeShared<FJsonObject>();
				FString StateError;
				const bool bStateOk = CmdDataLayerSetState(StateCtx, StateData, StateError);
				SubData->SetObjectField(TEXT("state_result"), StateData);
				SubData->SetBoolField(TEXT("state_ok"), bStateOk);
				SubData->SetStringField(TEXT("state_error"), StateError);
				bOk = bStateOk;
				if (!bOk)
				{
					SubError = StateError;
				}
			}
			if (bOk && !Operation.Equals(TEXT("delete"), ESearchCase::IgnoreCase) && Item->HasField(TEXT("actor_ids"))
				&& (Item->HasField(TEXT("layers")) || Item->HasField(TEXT("data_layers"))))
			{
				TSharedPtr<FJsonObject> MembershipParams = MakeShared<FJsonObject>();
				MembershipParams->SetArrayField(TEXT("actor_ids"), Item->GetArrayField(TEXT("actor_ids")));
				if (Item->HasField(TEXT("layers")))
				{
					MembershipParams->SetArrayField(TEXT("layers"), Item->GetArrayField(TEXT("layers")));
				}
				else
				{
					MembershipParams->SetArrayField(TEXT("layers"), Item->GetArrayField(TEXT("data_layers")));
				}
				FString Mode = TEXT("replace");
				Item->TryGetStringField(TEXT("membership_mode"), Mode);
				MembershipParams->SetStringField(TEXT("mode"), Mode);
				FUeAgentRequestContext MembershipCtx = Ctx;
				MembershipCtx.Params = MembershipParams;
				TSharedPtr<FJsonObject> MembershipData = MakeShared<FJsonObject>();
				FString MembershipError;
				const bool bMembershipOk = CmdDataLayerSetActorMembership(MembershipCtx, MembershipData, MembershipError);
				SubData->SetObjectField(TEXT("membership_result"), MembershipData);
				SubData->SetBoolField(TEXT("membership_ok"), bMembershipOk);
				SubData->SetStringField(TEXT("membership_error"), MembershipError);
				bOk = bMembershipOk;
				if (!bOk)
				{
					SubError = MembershipError;
				}
			}
			SubData->SetStringField(TEXT("operation"), TEXT("data_layer"));
			SubData->SetBoolField(TEXT("ok"), bOk);
			SubData->SetStringField(TEXT("error"), SubError);
			Results.Add(MakeShared<FJsonValueObject>(SubData));
			if (!bOk)
			{
				return FailApply(SubError);
			}
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* ActorAssignments = nullptr;
	if (Root->TryGetArrayField(TEXT("actor_assignments"), ActorAssignments) && ActorAssignments)
	{
		for (const TSharedPtr<FJsonValue>& Value : *ActorAssignments)
		{
			const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Item.IsValid())
			{
				continue;
			}
			FUeAgentRequestContext SubCtx = Ctx;
			SubCtx.Params = CopyParamsWithExecutionAuthorization(Item, bAllowDestructive, bAllowDelete);
			TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
			FString SubError;
			const bool bOk = CmdLevelTopologySetActorAssignments(SubCtx, SubData, SubError);
			SubData->SetStringField(TEXT("operation"), TEXT("actor_assignment"));
			SubData->SetBoolField(TEXT("ok"), bOk);
			SubData->SetStringField(TEXT("error"), SubError);
			Results.Add(MakeShared<FJsonValueObject>(SubData));
			if (!bOk)
			{
				return FailApply(SubError);
			}
		}
	}
	const TSharedPtr<FJsonObject>* WorldPartitionObj = nullptr;
	if (Root->TryGetObjectField(TEXT("world_partition"), WorldPartitionObj) && WorldPartitionObj && WorldPartitionObj->IsValid())
	{
		if ((*WorldPartitionObj)->HasField(TEXT("streaming_enabled")))
		{
			FUeAgentRequestContext SubCtx = Ctx;
		SubCtx.Params = CopyParamsWithExecutionAuthorization(*WorldPartitionObj, bAllowDestructive, bAllowDelete);
			TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
			FString SubError;
			const bool bOk = CmdWorldPartitionSetStreamingEnabled(SubCtx, SubData, SubError);
			SubData->SetStringField(TEXT("operation"), TEXT("world_partition_set_streaming_enabled"));
			SubData->SetBoolField(TEXT("ok"), bOk);
			SubData->SetStringField(TEXT("error"), SubError);
			Results.Add(MakeShared<FJsonValueObject>(SubData));
			if (!bOk)
			{
				return FailApply(SubError);
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* LoadRegions = nullptr;
		if ((*WorldPartitionObj)->TryGetArrayField(TEXT("load_regions"), LoadRegions) && LoadRegions)
		{
			for (const TSharedPtr<FJsonValue>& Value : *LoadRegions)
			{
				const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr;
				if (!Item.IsValid())
				{
					continue;
				}
				FUeAgentRequestContext SubCtx = Ctx;
				SubCtx.Params = CopyParamsWithExecutionAuthorization(Item, bAllowDestructive, bAllowDelete);
				TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
				FString SubError;
				const bool bOk = CmdWorldPartitionLoadRegion(SubCtx, SubData, SubError);
				SubData->SetStringField(TEXT("operation"), TEXT("world_partition_load_region"));
				SubData->SetBoolField(TEXT("ok"), bOk);
				SubData->SetStringField(TEXT("error"), SubError);
				Results.Add(MakeShared<FJsonValueObject>(SubData));
				if (!bOk)
				{
					return FailApply(SubError);
				}
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* LoadActors = nullptr;
		if ((*WorldPartitionObj)->TryGetArrayField(TEXT("load_actors"), LoadActors) && LoadActors)
		{
			for (const TSharedPtr<FJsonValue>& Value : *LoadActors)
			{
				const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr;
				if (!Item.IsValid())
				{
					continue;
				}
				FUeAgentRequestContext SubCtx = Ctx;
				SubCtx.Params = CopyParamsWithExecutionAuthorization(Item, bAllowDestructive, bAllowDelete);
				TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
				FString SubError;
				const bool bOk = CmdWorldPartitionLoadActors(SubCtx, SubData, SubError);
				SubData->SetStringField(TEXT("operation"), TEXT("world_partition_load_actors"));
				SubData->SetBoolField(TEXT("ok"), bOk);
				SubData->SetStringField(TEXT("error"), SubError);
				Results.Add(MakeShared<FJsonValueObject>(SubData));
				if (!bOk)
				{
					return FailApply(SubError);
				}
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* UnloadRegions = nullptr;
		if ((*WorldPartitionObj)->TryGetArrayField(TEXT("unload_regions"), UnloadRegions) && UnloadRegions)
		{
			for (const TSharedPtr<FJsonValue>& Value : *UnloadRegions)
			{
				const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr;
				if (!Item.IsValid())
				{
					continue;
				}
				FUeAgentRequestContext SubCtx = Ctx;
				SubCtx.Params = CopyParamsWithExecutionAuthorization(Item, bAllowDestructive, bAllowDelete);
				TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
				FString SubError;
				const bool bOk = CmdWorldPartitionUnloadRegion(SubCtx, SubData, SubError);
				SubData->SetStringField(TEXT("operation"), TEXT("world_partition_unload_region"));
				SubData->SetBoolField(TEXT("ok"), bOk);
				SubData->SetStringField(TEXT("error"), SubError);
				Results.Add(MakeShared<FJsonValueObject>(SubData));
				if (!bOk)
				{
					return FailApply(SubError);
				}
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* UnloadActors = nullptr;
		if ((*WorldPartitionObj)->TryGetArrayField(TEXT("unload_actors"), UnloadActors) && UnloadActors)
		{
			for (const TSharedPtr<FJsonValue>& Value : *UnloadActors)
			{
				const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr;
				if (!Item.IsValid())
				{
					continue;
				}
				FUeAgentRequestContext SubCtx = Ctx;
				SubCtx.Params = CopyParamsWithExecutionAuthorization(Item, bAllowDestructive, bAllowDelete);
				TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
				FString SubError;
				const bool bOk = CmdWorldPartitionUnloadActors(SubCtx, SubData, SubError);
				SubData->SetStringField(TEXT("operation"), TEXT("world_partition_unload_actors"));
				SubData->SetBoolField(TEXT("ok"), bOk);
				SubData->SetStringField(TEXT("error"), SubError);
				Results.Add(MakeShared<FJsonValueObject>(SubData));
				if (!bOk)
				{
					return FailApply(SubError);
				}
			}
		}
		const TSharedPtr<FJsonObject>* GenerateStreaming = nullptr;
		if ((*WorldPartitionObj)->TryGetObjectField(TEXT("generate_streaming"), GenerateStreaming) && GenerateStreaming && GenerateStreaming->IsValid())
		{
			FUeAgentRequestContext SubCtx = Ctx;
			SubCtx.Params = CopyParamsWithExecutionAuthorization(*GenerateStreaming, bAllowDestructive, bAllowDelete);
			TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
			FString SubError;
			const bool bOk = CmdWorldPartitionGenerateStreaming(SubCtx, SubData, SubError);
			SubData->SetStringField(TEXT("operation"), TEXT("world_partition_generate_streaming"));
			SubData->SetBoolField(TEXT("ok"), bOk);
			SubData->SetStringField(TEXT("error"), SubError);
			Results.Add(MakeShared<FJsonValueObject>(SubData));
			if (!bOk)
			{
				return FailApply(SubError);
			}
		}
	}
	const TSharedPtr<FJsonObject>* HLODObj = nullptr;
	if (Root->TryGetObjectField(TEXT("hlod"), HLODObj) && HLODObj && HLODObj->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* HLODLayers = nullptr;
		if ((*HLODObj)->TryGetArrayField(TEXT("layers"), HLODLayers) && HLODLayers)
		{
			for (const TSharedPtr<FJsonValue>& Value : *HLODLayers)
			{
				const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr;
				if (!Item.IsValid())
				{
					continue;
				}
				TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
				Params->SetObjectField(TEXT("json"), Item);
				FString AssetPath;
				if (Item->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					Params->SetStringField(TEXT("asset_path"), AssetPath);
				}
				FUeAgentRequestContext SubCtx = Ctx;
				SubCtx.Params = Params;
				TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
				FString SubError;
				const bool bOk = CmdHLODLayerApplyJson(SubCtx, SubData, SubError);
				SubData->SetStringField(TEXT("operation"), TEXT("hlod_layer_apply_json"));
				SubData->SetBoolField(TEXT("ok"), bOk);
				SubData->SetStringField(TEXT("error"), SubError);
				Results.Add(MakeShared<FJsonValueObject>(SubData));
				if (!bOk)
				{
					return FailApply(SubError);
				}
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* HLODAssignments = nullptr;
		if ((*HLODObj)->TryGetArrayField(TEXT("actor_assignments"), HLODAssignments) && HLODAssignments)
		{
			for (const TSharedPtr<FJsonValue>& Value : *HLODAssignments)
			{
				const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr;
				if (!Item.IsValid())
				{
					continue;
				}
				FUeAgentRequestContext SubCtx = Ctx;
				SubCtx.Params = CopyParamsWithExecutionAuthorization(Item, bAllowDestructive, bAllowDelete);
				TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
				FString SubError;
				const bool bOk = CmdHLODAssignActors(SubCtx, SubData, SubError);
				SubData->SetStringField(TEXT("operation"), TEXT("hlod_assign_actors"));
				SubData->SetBoolField(TEXT("ok"), bOk);
				SubData->SetStringField(TEXT("error"), SubError);
				Results.Add(MakeShared<FJsonValueObject>(SubData));
				if (!bOk)
				{
					return FailApply(SubError);
				}
			}
		}
		const TSharedPtr<FJsonObject>* HLODBuild = nullptr;
		if ((*HLODObj)->TryGetObjectField(TEXT("build"), HLODBuild) && HLODBuild && HLODBuild->IsValid())
		{
			FUeAgentRequestContext SubCtx = Ctx;
			SubCtx.Params = CopyParamsWithExecutionAuthorization(*HLODBuild, bAllowDestructive, bAllowDelete);
			TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
			FString SubError;
			const bool bOk = CmdHLODBuild(SubCtx, SubData, SubError);
			SubData->SetStringField(TEXT("operation"), TEXT("hlod_build"));
			SubData->SetBoolField(TEXT("ok"), bOk);
			SubData->SetStringField(TEXT("error"), SubError);
			Results.Add(MakeShared<FJsonValueObject>(SubData));
			if (!bOk)
			{
				return FailApply(SubError);
			}
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* Tasks = nullptr;
	if (Root->TryGetArrayField(TEXT("tasks"), Tasks) && Tasks)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Tasks)
		{
			const TSharedPtr<FJsonObject> Task = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Task.IsValid())
			{
				continue;
			}
			FString Type;
			Task->TryGetStringField(TEXT("type"), Type);
			if (Type.IsEmpty())
			{
				Task->TryGetStringField(TEXT("command"), Type);
			}
			FUeAgentRequestContext SubCtx = Ctx;
			SubCtx.Params = CopyParamsWithExecutionAuthorization(Task, bAllowDestructive, bAllowDelete);
			TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
			FString SubError;
			bool bOk = false;
			if (Type.Equals(TEXT("hlod_build"), ESearchCase::IgnoreCase))
			{
				bOk = CmdHLODBuild(SubCtx, SubData, SubError);
			}
			else if (Type.Equals(TEXT("world_partition_generate_streaming"), ESearchCase::IgnoreCase))
			{
				bOk = CmdWorldPartitionGenerateStreaming(SubCtx, SubData, SubError);
			}
			else
			{
				SubError = TEXT("task_type_unsupported");
			}
			SubData->SetStringField(TEXT("operation"), Type);
			SubData->SetBoolField(TEXT("ok"), bOk);
			SubData->SetStringField(TEXT("error"), SubError);
			Results.Add(MakeShared<FJsonValueObject>(SubData));
			if (!bOk)
			{
				return FailApply(SubError);
			}
		}
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.level_topology.apply_result.v1"));
	OutData->SetStringField(TEXT("source"), Source);
	OutData->SetStringField(TEXT("status"), TEXT("applied"));
	OutData->SetBoolField(TEXT("dry_run"), false);
	OutData->SetArrayField(TEXT("results"), Results);
	OutData->SetBoolField(TEXT("changed"), Results.Num() > 0);
	OutData->SetBoolField(TEXT("partial_commit"), false);
	OutData->SetBoolField(TEXT("partial_commit_possible"), false);
	OutData->SetNumberField(TEXT("completed_count"), Results.Num());
	OutData->SetObjectField(TEXT("preflight"), PreflightData);
	return true;
}
