// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentLevelContentJsonQuery.h"
#include "UeAgentLevelContentJsonCore.h"
#include "UeAgentLevelContentJsonProperties.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SplineComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

namespace UeAgentLevelContentJson
{
	bool ActorMatchesText(AActor* Actor, const FString& Query, bool bContains)
	{
		if (!Actor || Query.TrimStartAndEnd().IsEmpty())
		{
			return true;
		}
		const FString CleanQuery = Query.TrimStartAndEnd();
		const FString Label = Actor->GetActorLabel();
		const FString Name = Actor->GetName();
		const FString Path = Actor->GetPathName();
		const FString ManagedId = GetManagedId(Actor);
		if (bContains)
		{
			return Label.Contains(CleanQuery, ESearchCase::IgnoreCase) ||
				Name.Contains(CleanQuery, ESearchCase::IgnoreCase) ||
				Path.Contains(CleanQuery, ESearchCase::IgnoreCase) ||
				ManagedId.Contains(CleanQuery, ESearchCase::IgnoreCase);
		}
		return Label.Equals(CleanQuery, ESearchCase::IgnoreCase) ||
			Name.Equals(CleanQuery, ESearchCase::IgnoreCase) ||
			Path.Equals(CleanQuery, ESearchCase::IgnoreCase) ||
			ManagedId.Equals(CleanQuery, ESearchCase::IgnoreCase);
	}

	bool TryReadBoundsSpec(const TSharedPtr<FJsonObject>& Obj, FBox& OutBounds)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		FVector Min;
		FVector Max;
		if ((TryGetVectorField(Obj, TEXT("min"), Min) || TryGetVectorField(Obj, TEXT("minimum"), Min)) &&
			(TryGetVectorField(Obj, TEXT("max"), Max) || TryGetVectorField(Obj, TEXT("maximum"), Max)))
		{
			OutBounds = FBox(Min, Max);
			return OutBounds.IsValid != 0;
		}
		FVector Center;
		FVector Extent;
		if (TryGetVectorField(Obj, TEXT("center"), Center) &&
			(TryGetVectorField(Obj, TEXT("extent"), Extent) || TryGetVectorField(Obj, TEXT("extents"), Extent) || TryGetVectorField(Obj, TEXT("half_extent"), Extent)))
		{
			OutBounds = FBox::BuildAABB(Center, Extent);
			return OutBounds.IsValid != 0;
		}
		return false;
	}

	bool ActorMatchesBounds(AActor* Actor, const TSharedPtr<FJsonObject>& Selection)
	{
		TSharedPtr<FJsonObject> BoundsObj;
		if (!TryGetObject(Selection, TEXT("bounds"), BoundsObj))
		{
			return true;
		}
		FBox QueryBounds(ForceInit);
		if (!TryReadBoundsSpec(BoundsObj, QueryBounds) || QueryBounds.IsValid == 0)
		{
			return true;
		}
		const FBox ActorBounds = Actor ? Actor->GetComponentsBoundingBox(true) : FBox(ForceInit);
		if (ActorBounds.IsValid == 0)
		{
			return false;
		}
		FString Mode = TEXT("intersects");
		TryGetStringAny(BoundsObj, { TEXT("mode"), TEXT("bounds_mode"), TEXT("match") }, Mode);
		Mode = Mode.TrimStartAndEnd().ToLower();
		if (Mode == TEXT("contains_origin") || Mode == TEXT("contains_actor_origin"))
		{
			return QueryBounds.IsInside(Actor->GetActorLocation());
		}
		if (Mode == TEXT("contains") || Mode == TEXT("contains_actor_bounds"))
		{
			return QueryBounds.IsInside(ActorBounds.Min) && QueryBounds.IsInside(ActorBounds.Max);
		}
		if (Mode == TEXT("actor_contains") || Mode == TEXT("actor_contains_bounds"))
		{
			return ActorBounds.IsInside(QueryBounds.Min) && ActorBounds.IsInside(QueryBounds.Max);
		}
		return ActorBounds.Intersect(QueryBounds);
	}

	bool ActorMatchesDataLayerFallback(AActor* Actor, const TArray<FString>& DataLayers)
	{
		if (!Actor || DataLayers.Num() == 0)
		{
			return true;
		}

		FString DataLayerText;
		FString PropertyClass;
		const bool bHasReflectedDataLayerText =
			ExportPropertyText(Actor, TEXT("DataLayerAssets"), DataLayerText, PropertyClass) ||
			ExportPropertyText(Actor, TEXT("DataLayers"), DataLayerText, PropertyClass);
		for (const FString& Layer : DataLayers)
		{
			const FString CleanLayer = Layer.TrimStartAndEnd();
			if (CleanLayer.IsEmpty())
			{
				continue;
			}
			const bool bMatched =
				(bHasReflectedDataLayerText && DataLayerText.Contains(CleanLayer, ESearchCase::IgnoreCase)) ||
				ActorHasTagString(Actor, CleanLayer) ||
				ActorHasTagString(Actor, FString::Printf(TEXT("DataLayer=%s"), *CleanLayer)) ||
				ActorHasTagString(Actor, FString::Printf(TEXT("DataLayer:%s"), *CleanLayer));
			if (!bMatched)
			{
				return false;
			}
		}
		return true;
	}

	bool ActorMatchesQuery(AActor* Actor, const TSharedPtr<FJsonObject>& Selection)
	{
		if (!Actor || Actor->IsActorBeingDestroyed())
		{
			return false;
		}
		if (!Selection.IsValid())
		{
			return true;
		}

		FString Mode;
		TryGetStringAny(Selection, { TEXT("mode") }, Mode);
		Mode = Mode.TrimStartAndEnd().ToLower();
		if (Mode == TEXT("selected"))
		{
			if (!Actor->IsSelected())
			{
				return false;
			}
		}

		FString ExactText;
		if (TryGetStringAny(Selection, { TEXT("label"), TEXT("actor_label"), TEXT("name"), TEXT("actor") }, ExactText) && !ActorMatchesText(Actor, ExactText, false))
		{
			return false;
		}
		FString ContainsText;
		if (TryGetStringAny(Selection, { TEXT("label_contains"), TEXT("name_contains"), TEXT("search") }, ContainsText) && !ActorMatchesText(Actor, ContainsText, true))
		{
			return false;
		}

		FString FolderPath;
		if ((Mode == TEXT("folder") || Selection->HasField(TEXT("folder_path"))) &&
			TryGetStringAny(Selection, { TEXT("folder_path"), TEXT("folder_root") }, FolderPath))
		{
			bool bIncludeChildFolders = true;
			TryGetBoolAny(Selection, { TEXT("include_child_folders") }, bIncludeChildFolders);
			if (!FolderMatches(Actor->GetFolderPath().ToString(), FolderPath, bIncludeChildFolders))
			{
				return false;
			}
		}

		const TArray<FString> TagsAll = ReadStringArray(Selection, TEXT("tags_all"));
		for (const FString& Tag : TagsAll)
		{
			if (!ActorHasTagString(Actor, Tag))
			{
				return false;
			}
		}
		const TArray<FString> TagsAny = ReadStringArray(Selection, TEXT("tags_any"));
		if (TagsAny.Num() > 0)
		{
			bool bMatchedAny = false;
			for (const FString& Tag : TagsAny)
			{
				if (ActorHasTagString(Actor, Tag))
				{
					bMatchedAny = true;
					break;
				}
			}
			if (!bMatchedAny)
			{
				return false;
			}
		}

		TArray<FString> ActorIds = ReadStringArray(Selection, TEXT("actors"));
		if (ActorIds.Num() == 0)
		{
			ActorIds = ReadStringArray(Selection, TEXT("ids"));
		}
		if (ActorIds.Num() > 0)
		{
			bool bMatched = false;
			for (const FString& Id : ActorIds)
			{
				if (Actor->GetName().Equals(Id, ESearchCase::IgnoreCase) ||
					Actor->GetActorLabel().Equals(Id, ESearchCase::IgnoreCase) ||
					GetManagedId(Actor).Equals(Id, ESearchCase::IgnoreCase))
				{
					bMatched = true;
					break;
				}
			}
			if (!bMatched)
			{
				return false;
			}
		}

		TArray<FString> DataLayers = ReadStringArray(Selection, TEXT("data_layers"));
		FString DataLayer;
		if (TryGetStringAny(Selection, { TEXT("data_layer") }, DataLayer) && !DataLayer.TrimStartAndEnd().IsEmpty())
		{
			DataLayers.Add(DataLayer.TrimStartAndEnd());
		}
		if (!ActorMatchesDataLayerFallback(Actor, DataLayers))
		{
			return false;
		}

		TArray<FString> ClassAny = ReadStringArray(Selection, TEXT("class_any"));
		if (ClassAny.Num() == 0)
		{
			ClassAny = ReadStringArray(Selection, TEXT("classes"));
		}
		FString SingleClass;
		if (TryGetStringAny(Selection, { TEXT("class"), TEXT("class_path") }, SingleClass))
		{
			ClassAny.Add(SingleClass);
		}
		if (ClassAny.Num() > 0)
		{
			const FString ActorClass = Actor->GetClass() ? Actor->GetClass()->GetPathName() : FString();
			bool bMatched = false;
			for (const FString& ClassText : ClassAny)
			{
				if (ActorClass.Equals(ClassText, ESearchCase::IgnoreCase) ||
					ActorClass.Contains(ClassText, ESearchCase::IgnoreCase))
				{
					bMatched = true;
					break;
				}
			}
			if (!bMatched)
			{
				return false;
			}
		}

		if (!ActorMatchesBounds(Actor, Selection))
		{
			return false;
		}

		FVector Center;
		double Radius = 0.0;
		if (Selection->TryGetNumberField(TEXT("radius"), Radius) && TryGetVectorField(Selection, TEXT("center"), Center))
		{
			if (FVector::Distance(Actor->GetActorLocation(), Center) > Radius)
			{
				return false;
			}
		}

		return true;
	}

	TSharedPtr<FJsonObject> MakeBoundsJson(const FBox& Box)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("valid"), Box.IsValid != 0);
		if (Box.IsValid != 0)
		{
			Obj->SetObjectField(TEXT("min"), MakeVectorJson(Box.Min));
			Obj->SetObjectField(TEXT("max"), MakeVectorJson(Box.Max));
			Obj->SetObjectField(TEXT("center"), MakeVectorJson(Box.GetCenter()));
			Obj->SetObjectField(TEXT("extent"), MakeVectorJson(Box.GetExtent()));
		}
		return Obj;
	}

	TSharedPtr<FJsonObject> MakeComponentJson(UActorComponent* Component)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Component ? Component->GetName() : FString());
		Obj->SetStringField(TEXT("path"), Component ? Component->GetPathName() : FString());
		Obj->SetStringField(TEXT("class"), (Component && Component->GetClass()) ? Component->GetClass()->GetPathName() : FString());
		Obj->SetBoolField(TEXT("registered"), Component ? Component->IsRegistered() : false);
		Obj->SetBoolField(TEXT("is_scene_component"), Cast<USceneComponent>(Component) != nullptr);
		if (USceneComponent* SceneComponent = Cast<USceneComponent>(Component))
		{
			Obj->SetObjectField(TEXT("relative_location"), MakeVectorJson(SceneComponent->GetRelativeLocation()));
			Obj->SetObjectField(TEXT("relative_rotation"), MakeRotatorJson(SceneComponent->GetRelativeRotation()));
			Obj->SetObjectField(TEXT("relative_scale"), MakeVectorJson(SceneComponent->GetRelativeScale3D()));
			Obj->SetObjectField(TEXT("space_contract"), MakeSpaceContractJson(TEXT("component.relative_transform"), TEXT("ue_scene_component_parent_relative_transform")));
			Obj->SetStringField(TEXT("attach_parent"), SceneComponent->GetAttachParent() ? SceneComponent->GetAttachParent()->GetName() : FString());
		}
		if (USplineComponent* SplineComponent = Cast<USplineComponent>(Component))
		{
			const int32 PointCount = SplineComponent->GetNumberOfSplinePoints();
			TArray<TSharedPtr<FJsonValue>> Points;
			Points.Reserve(PointCount);
			for (int32 PointIndex = 0; PointIndex < PointCount; ++PointIndex)
			{
				TSharedPtr<FJsonObject> Point = MakeShared<FJsonObject>();
				Point->SetNumberField(TEXT("index"), PointIndex);
				Point->SetObjectField(TEXT("location"), MakeVectorJson(SplineComponent->GetLocationAtSplinePoint(PointIndex, ESplineCoordinateSpace::Local)));
				Point->SetObjectField(TEXT("arrive_tangent"), MakeVectorJson(SplineComponent->GetArriveTangentAtSplinePoint(PointIndex, ESplineCoordinateSpace::Local)));
				Point->SetObjectField(TEXT("leave_tangent"), MakeVectorJson(SplineComponent->GetLeaveTangentAtSplinePoint(PointIndex, ESplineCoordinateSpace::Local)));
				Point->SetObjectField(TEXT("space_contract"), MakeSpaceContractJson(TEXT("spline.point"), TEXT("ue_spline_component_local_space")));
				Point->SetStringField(TEXT("type"), UEnum::GetValueAsString(SplineComponent->GetSplinePointType(PointIndex)));
				Points.Add(MakeShared<FJsonValueObject>(Point));
			}
			Obj->SetNumberField(TEXT("spline_point_count"), PointCount);
			Obj->SetBoolField(TEXT("spline_closed_loop"), SplineComponent->IsClosedLoop());
			Obj->SetArrayField(TEXT("spline_points"), Points);
		}
		return Obj;
	}

	TSharedPtr<FJsonObject> MakeActorResourceJson(AActor* Actor, const TSharedPtr<FJsonObject>& Projection, bool bSnapshot)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Actor)
		{
			return Obj;
		}

		Obj->SetStringField(TEXT("id"), ActorStableId(Actor));
		Obj->SetStringField(TEXT("managed_id"), GetManagedId(Actor));
		Obj->SetStringField(TEXT("name"), Actor->GetName());
		Obj->SetStringField(TEXT("label"), Actor->GetActorLabel());
		Obj->SetStringField(TEXT("object_path"), Actor->GetPathName());
		Obj->SetStringField(TEXT("class"), Actor->GetClass() ? Actor->GetClass()->GetPathName() : FString());
		Obj->SetStringField(TEXT("category"), TEXT("spatial"));
		Obj->SetStringField(TEXT("kind"), TEXT("actor"));
		Obj->SetStringField(TEXT("folder_path"), Actor->GetFolderPath().ToString());
		Obj->SetObjectField(TEXT("transform"), MakeShared<FJsonObject>());
		TSharedPtr<FJsonObject> Transform = Obj->GetObjectField(TEXT("transform"));
		Transform->SetObjectField(TEXT("location"), MakeVectorJson(Actor->GetActorLocation()));
		Transform->SetObjectField(TEXT("rotation"), MakeRotatorJson(Actor->GetActorRotation()));
		Transform->SetObjectField(TEXT("scale"), MakeVectorJson(Actor->GetActorScale3D()));
		Transform->SetObjectField(TEXT("space_contract"), MakeSpaceContractJson(TEXT("actor.transform"), TEXT("ue_actor_world_transform")));

		TArray<TSharedPtr<FJsonValue>> Tags;
		for (const FName& Tag : Actor->Tags)
		{
			Tags.Add(MakeShared<FJsonValueString>(Tag.ToString()));
		}
		Obj->SetArrayField(TEXT("tags"), Tags);

		if (Actor->GetAttachParentActor())
		{
			TSharedPtr<FJsonObject> Attachments = MakeShared<FJsonObject>();
			Attachments->SetStringField(TEXT("parent_actor"), ActorStableId(Actor->GetAttachParentActor()));
			Obj->SetObjectField(TEXT("attachments"), Attachments);
		}

		bool bIncludeBounds = bSnapshot;
		bool bIncludeComponents = bSnapshot;
		if (Projection.IsValid())
		{
			TryGetBoolAny(Projection, { TEXT("include_bounds") }, bIncludeBounds);
			TryGetBoolAny(Projection, { TEXT("include_components") }, bIncludeComponents);
		}
		if (bIncludeBounds)
		{
			Obj->SetObjectField(TEXT("bounds"), MakeBoundsJson(Actor->GetComponentsBoundingBox(true)));
		}
		if (bIncludeComponents)
		{
			TArray<UActorComponent*> Components;
			Actor->GetComponents(Components);
			TArray<TSharedPtr<FJsonValue>> ComponentValues;
			for (UActorComponent* Component : Components)
			{
				if (Component)
				{
					ComponentValues.Add(MakeShared<FJsonValueObject>(MakeComponentJson(Component)));
				}
			}
			Obj->SetArrayField(TEXT("components"), ComponentValues);
		}

		const TArray<TSharedPtr<FJsonValue>>* PropertyPaths = nullptr;
		if (Projection.IsValid() && Projection->TryGetArrayField(TEXT("include_properties"), PropertyPaths) && PropertyPaths)
		{
			TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
			for (const TSharedPtr<FJsonValue>& Value : *PropertyPaths)
			{
				const FString Path = Value.IsValid() ? Value->AsString() : FString();
				if (Path.IsEmpty())
				{
					continue;
				}
				FString ReadBack;
				FString PropertyClass;
				if (ExportPropertyText(Actor, Path, ReadBack, PropertyClass))
				{
					TSharedPtr<FJsonObject> Property = MakeShared<FJsonObject>();
					Property->SetStringField(TEXT("value_text"), ReadBack);
					Property->SetStringField(TEXT("property_class"), PropertyClass);
					Properties->SetObjectField(Path, Property);
				}
			}
			Obj->SetObjectField(TEXT("properties"), Properties);
		}
		return Obj;
	}

	TSharedPtr<FJsonObject> BuildWorldInfo(UWorld* World)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("map_name"), World ? World->GetMapName() : FString());
		Obj->SetStringField(TEXT("package_name"), (World && World->GetOutermost()) ? World->GetOutermost()->GetName() : FString());
		Obj->SetBoolField(TEXT("dirty"), (World && World->GetOutermost()) ? World->GetOutermost()->IsDirty() : false);
		TArray<TSharedPtr<FJsonValue>> StreamingLevels;
		if (World)
		{
			for (ULevelStreaming* StreamingLevel : World->GetStreamingLevels())
			{
				if (!StreamingLevel)
				{
					continue;
				}
				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("package_name"), StreamingLevel->GetWorldAssetPackageName());
				Item->SetBoolField(TEXT("loaded"), StreamingLevel->IsLevelLoaded());
				Item->SetBoolField(TEXT("visible"), StreamingLevel->IsLevelVisible());
				StreamingLevels.Add(MakeShared<FJsonValueObject>(Item));
			}
		}
		Obj->SetArrayField(TEXT("streaming_levels"), StreamingLevels);
		return Obj;
	}

	TSharedPtr<FJsonObject> BuildFeatureGateReport(const TSharedPtr<FJsonObject>& Content)
	{
		TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
		Report->SetBoolField(TEXT("world_partition_runtime_region_write"), false);
		Report->SetBoolField(TEXT("streaming_level_mutation"), false);
		Report->SetBoolField(TEXT("data_layer_mutation"), false);
		Report->SetBoolField(TEXT("hlod_build"), false);
		Report->SetBoolField(TEXT("lighting_build"), false);
		Report->SetBoolField(TEXT("pcg_generate"), false);
		Report->SetStringField(TEXT("policy"), TEXT("ordinary_spatial_actor_crud_remains_available; gated systems return structured feature_unavailable unless implemented by dedicated modules"));
		return Report;
	}

bool QueryLevelContent(UWorld* World, const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutJson, bool bActorOnly, bool bSnapshot)
	{
		TSharedPtr<FJsonObject> Selection;
		if (Params.IsValid())
		{
			TryGetObject(Params, TEXT("selection"), Selection);
		}
		TSharedPtr<FJsonObject> Projection;
		if (Params.IsValid())
		{
			TryGetObject(Params, TEXT("projection"), Projection);
		}
		if (!Projection.IsValid())
		{
			Projection = MakeShared<FJsonObject>();
			bool bIncludeComponents = false;
			bool bIncludeBounds = false;
			if (Params.IsValid())
			{
				TryGetBoolAny(Params, { TEXT("include_components") }, bIncludeComponents);
				TryGetBoolAny(Params, { TEXT("include_bounds") }, bIncludeBounds);
			}
			Projection->SetBoolField(TEXT("include_components"), bIncludeComponents);
			Projection->SetBoolField(TEXT("include_bounds"), bIncludeBounds);
		}

		TArray<TSharedPtr<FJsonValue>> Resources;
		int32 Visited = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor || Actor->IsActorBeingDestroyed())
			{
				continue;
			}
			++Visited;
			if (!ActorMatchesQuery(Actor, Selection))
			{
				continue;
			}
			Resources.Add(MakeShared<FJsonValueObject>(MakeActorResourceJson(Actor, Projection, bSnapshot)));
		}

		OutJson = MakeShared<FJsonObject>();
		OutJson->SetStringField(TEXT("schema"), bSnapshot ? SnapshotSchema : LevelContentSchema);
		OutJson->SetStringField(TEXT("query_schema"), QuerySchema);
		OutJson->SetBoolField(TEXT("actor_only"), bActorOnly);
		OutJson->SetObjectField(TEXT("world"), BuildWorldInfo(World));
		OutJson->SetNumberField(TEXT("visited_actor_count"), Visited);
		OutJson->SetNumberField(TEXT("resource_count"), Resources.Num());
		OutJson->SetArrayField(TEXT("resources"), Resources);
		OutJson->SetObjectField(TEXT("organization"), MakeShared<FJsonObject>());
		OutJson->SetObjectField(TEXT("feature_gates"), BuildFeatureGateReport(nullptr));
		return true;
	}
}
