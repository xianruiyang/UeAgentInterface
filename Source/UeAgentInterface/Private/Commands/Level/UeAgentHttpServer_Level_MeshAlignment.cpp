#include "UeAgentHttpServer_Level.h"

#include "UeAgentHttpServer_LevelOps.h"

#include "Module/UeAgentInterfaceLogger.h"

#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "LevelEditorViewport.h"
#include "MeshDescription.h"
#include "Selection.h"
#include "StaticMeshAttributes.h"

bool FUeAgentLegacyMechanics::CmdViewportPickActorAtScreen(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FLevelEditorViewportClient* ViewportClient = UeAgentLevelOps::GetPreferredLevelViewportClient();
	if (!ViewportClient || !ViewportClient->Viewport)
	{
		OutError = TEXT("no_level_editor_viewport");
		return false;
	}

	FVector2D ScreenPoint(0.0f, 0.0f);
	FVector WorldOrigin = FVector::ZeroVector;
	FVector WorldDirection = FVector::ForwardVector;
	double TraceDistance = HALF_WORLD_MAX;
	ECollisionChannel TraceChannel = ECC_Visibility;
	bool bTraceComplex = true;
	FHitResult Hit;
	if (!UeAgentLevelOps::TraceActorFromScreenPoint(
		World,
		ViewportClient,
		Ctx.Params,
		ScreenPoint,
		WorldOrigin,
		WorldDirection,
		TraceDistance,
		TraceChannel,
		bTraceComplex,
		Hit,
		OutError))
	{
		return false;
	}

	AActor* HitActor = Hit.GetActor();

	OutData->SetObjectField(TEXT("screen_point"), VecToJson(FVector(ScreenPoint.X, ScreenPoint.Y, 0.0)));
	OutData->SetObjectField(TEXT("world_origin"), VecToJson(WorldOrigin));
	OutData->SetObjectField(TEXT("world_direction"), VecToJson(WorldDirection));
	OutData->SetNumberField(TEXT("trace_distance"), TraceDistance);
	OutData->SetStringField(TEXT("trace_channel"), FString::Printf(TEXT("%d"), (int32)TraceChannel));
	OutData->SetBoolField(TEXT("trace_complex"), bTraceComplex);
	OutData->SetObjectField(TEXT("actor"), UeAgentLevelOps::MakeActorSummaryJson(HitActor));
	OutData->SetObjectField(TEXT("actor_info"), UeAgentLevelOps::MakeActorInfoJson(HitActor));
	if (Hit.GetComponent())
	{
		OutData->SetStringField(TEXT("component_name"), Hit.GetComponent()->GetName());
		OutData->SetStringField(TEXT("component_path"), Hit.GetComponent()->GetPathName());
	}
	UeAgentLevelOps::FillHitResultJson(Hit, OutData);
	return true;
}

bool FUeAgentLegacyMechanics::CmdViewportSelectActorAtScreen(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	if (!GEditor)
	{
		OutError = TEXT("no_geditor");
		return false;
	}

	FLevelEditorViewportClient* ViewportClient = UeAgentLevelOps::GetPreferredLevelViewportClient();
	if (!ViewportClient || !ViewportClient->Viewport)
	{
		OutError = TEXT("no_level_editor_viewport");
		return false;
	}

	FString SelectionMode = TEXT("replace");
	JsonTryGetString(Ctx.Params, TEXT("selection_mode"), SelectionMode);
	SelectionMode = SelectionMode.ToLower();

	FVector2D ScreenPoint(0.0f, 0.0f);
	FVector WorldOrigin = FVector::ZeroVector;
	FVector WorldDirection = FVector::ForwardVector;
	double TraceDistance = HALF_WORLD_MAX;
	ECollisionChannel TraceChannel = ECC_Visibility;
	bool bTraceComplex = true;
	FHitResult Hit;
	if (!UeAgentLevelOps::TraceActorFromScreenPoint(
		World,
		ViewportClient,
		Ctx.Params,
		ScreenPoint,
		WorldOrigin,
		WorldDirection,
		TraceDistance,
		TraceChannel,
		bTraceComplex,
		Hit,
		OutError))
	{
		return false;
	}

	AActor* HitActor = Hit.GetActor();
	if (!HitActor)
	{
		// When `allow_no_hit=true` is set, TraceActorFromScreenPoint returns success with an empty hit.
		// In that case, do not modify selection; just return the current selection set.
		OutData->SetStringField(TEXT("selection_mode"), SelectionMode);
		OutData->SetObjectField(TEXT("screen_point"), VecToJson(FVector(ScreenPoint.X, ScreenPoint.Y, 0.0)));
		OutData->SetObjectField(TEXT("world_origin"), VecToJson(WorldOrigin));
		OutData->SetObjectField(TEXT("world_direction"), VecToJson(WorldDirection));
		OutData->SetNumberField(TEXT("trace_distance"), TraceDistance);
		OutData->SetStringField(TEXT("trace_channel"), FString::Printf(TEXT("%d"), (int32)TraceChannel));
		OutData->SetBoolField(TEXT("trace_complex"), bTraceComplex);
		UeAgentLevelOps::FillHitResultJson(Hit, OutData);
		return CmdLevelGetSelection(OutData, OutError);
	}

	USelection* SelectedActors = GEditor->GetSelectedActors();
	if (!SelectedActors)
	{
		OutError = TEXT("selected_actors_unavailable");
		return false;
	}

	SelectedActors->BeginBatchSelectOperation();
	if (SelectionMode == TEXT("replace"))
	{
		GEditor->SelectNone(false, true, false);
	}
	const bool bSelect = (SelectionMode != TEXT("remove"));
	GEditor->SelectActor(HitActor, bSelect, false, true);
	SelectedActors->EndBatchSelectOperation();
	GEditor->NoteSelectionChange();

	OutData->SetStringField(TEXT("selection_mode"), SelectionMode);
	OutData->SetObjectField(TEXT("screen_point"), VecToJson(FVector(ScreenPoint.X, ScreenPoint.Y, 0.0)));
	OutData->SetObjectField(TEXT("world_origin"), VecToJson(WorldOrigin));
	OutData->SetObjectField(TEXT("world_direction"), VecToJson(WorldDirection));
	OutData->SetObjectField(TEXT("actor"), UeAgentLevelOps::MakeActorSummaryJson(HitActor));
	OutData->SetObjectField(TEXT("actor_info"), UeAgentLevelOps::MakeActorInfoJson(HitActor));
	if (Hit.GetComponent())
	{
		OutData->SetStringField(TEXT("component_name"), Hit.GetComponent()->GetName());
		OutData->SetStringField(TEXT("component_path"), Hit.GetComponent()->GetPathName());
	}
	UeAgentLevelOps::FillHitResultJson(Hit, OutData);
	return CmdLevelGetSelection(OutData, OutError);
}

bool FUeAgentLegacyMechanics::CmdLevelGetNearbyActorObbs(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FString Id;
	JsonTryGetString(Ctx.Params, TEXT("id"), Id);
	Id = Id.TrimStartAndEnd();

	FVector ExplicitCenter = FVector::ZeroVector;
	const bool bHasExplicitCenter = JsonTryGetVector(Ctx.Params, TEXT("center"), ExplicitCenter);

	double Radius = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("radius"), Radius) || Radius <= 0.0)
	{
		OutError = TEXT("missing_radius");
		return false;
	}

	bool bIncludeSelf = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_self"), bIncludeSelf);

	AActor* SourceActor = nullptr;
	FVector QueryCenter = FVector::ZeroVector;
	if (bHasExplicitCenter)
	{
		QueryCenter = ExplicitCenter;
		bIncludeSelf = false;
	}
	else
	{
		if (Id.IsEmpty())
		{
			OutError = TEXT("missing_id_or_center");
			return false;
		}

		SourceActor = FindActorByNameOrLabel(World, Id);
		if (!SourceActor)
		{
			OutError = TEXT("actor_not_found");
			return false;
		}

		FBox SourceBounds(EForceInit::ForceInit);
		FString SourceBoundsError;
		QueryCenter = UeAgentLevelOps::GetActorBoundsBox(SourceActor, SourceBounds, SourceBoundsError)
			? SourceBounds.GetCenter()
			: SourceActor->GetActorLocation();
	}
	const double RadiusSquared = Radius * Radius;

	// Optional filtering to reduce noise during incremental blockout validation.
	FString FolderPathPrefix;
	JsonTryGetString(Ctx.Params, TEXT("accept_folder_path_prefix"), FolderPathPrefix);
	if (FolderPathPrefix.TrimStartAndEnd().IsEmpty())
	{
		JsonTryGetString(Ctx.Params, TEXT("folder_path_prefix"), FolderPathPrefix);
	}
	FolderPathPrefix = FolderPathPrefix.TrimStartAndEnd();
	const FString NormalizedFolderPathPrefix = UeAgentLevelOps::NormalizeFolderPathForMatch(FolderPathPrefix);
	const bool bHasFolderFilter = !NormalizedFolderPathPrefix.IsEmpty();

	FString IgnoreFolderPathPrefix;
	JsonTryGetString(Ctx.Params, TEXT("ignore_folder_path_prefix"), IgnoreFolderPathPrefix);
	IgnoreFolderPathPrefix = IgnoreFolderPathPrefix.TrimStartAndEnd();
	const FString NormalizedIgnoreFolderPrefix = UeAgentLevelOps::NormalizeFolderPathForMatch(IgnoreFolderPathPrefix);
	const bool bHasIgnoreFolderFilter = !NormalizedIgnoreFolderPrefix.IsEmpty();

	auto ParseStringArray = [&Ctx](const FString& FieldName, TArray<FString>& OutStrings)
	{
		OutStrings.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetArrayField(FieldName, Values) || !Values)
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			if (!Value.IsValid())
			{
				continue;
			}

			const FString Str = Value->AsString().TrimStartAndEnd();
			if (!Str.IsEmpty())
			{
				OutStrings.Add(Str);
			}
		}
	};

	TArray<FString> AcceptTags;
	TArray<FString> IgnoreTags;
	ParseStringArray(TEXT("accept_tags"), AcceptTags);
	ParseStringArray(TEXT("ignore_tags"), IgnoreTags);

	TArray<FString> AcceptClassSubstrings;
	TArray<FString> IgnoreClassSubstrings;
	ParseStringArray(TEXT("accept_class_substrings"), AcceptClassSubstrings);
	ParseStringArray(TEXT("ignore_class_substrings"), IgnoreClassSubstrings);

	int32 Limit = 0;
	{
		double LimitNumber = 0.0;
		if (JsonTryGetNumber(Ctx.Params, TEXT("limit"), LimitNumber))
		{
			Limit = FMath::Clamp(FMath::RoundToInt(LimitNumber), 0, 10000);
		}
	}

	struct FNearbyObbEntry
	{
		AActor* Actor = nullptr;
		double Distance = 0.0;
		TSharedPtr<FJsonObject> Obb;
	};

	TArray<FNearbyObbEntry> NearbyEntries;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor || Actor->IsActorBeingDestroyed())
		{
			continue;
		}
		if (!bIncludeSelf && SourceActor && Actor == SourceActor)
		{
			continue;
		}

		if (bHasFolderFilter && !UeAgentLevelOps::DoesActorMatchFolderPath(Actor, NormalizedFolderPathPrefix, /*bIncludeChildFolders*/ true))
		{
			continue;
		}
		if (bHasIgnoreFolderFilter && UeAgentLevelOps::DoesActorMatchFolderPath(Actor, NormalizedIgnoreFolderPrefix, /*bIncludeChildFolders*/ true))
		{
			continue;
		}

		if (AcceptTags.Num() > 0)
		{
			bool bMatched = false;
			for (const FString& TagText : AcceptTags)
			{
				if (Actor->ActorHasTag(*TagText))
				{
					bMatched = true;
					break;
				}
			}
			if (!bMatched)
			{
				continue;
			}
		}
		if (IgnoreTags.Num() > 0)
		{
			bool bIgnored = false;
			for (const FString& TagText : IgnoreTags)
			{
				if (Actor->ActorHasTag(*TagText))
				{
					bIgnored = true;
					break;
				}
			}
			if (bIgnored)
			{
				continue;
			}
		}

		const FString ActorClassPath = (Actor->GetClass() ? Actor->GetClass()->GetPathName() : FString());
		if (AcceptClassSubstrings.Num() > 0)
		{
			bool bMatched = false;
			for (const FString& Sub : AcceptClassSubstrings)
			{
				if (!Sub.IsEmpty() && ActorClassPath.Contains(Sub, ESearchCase::IgnoreCase))
				{
					bMatched = true;
					break;
				}
			}
			if (!bMatched)
			{
				continue;
			}
		}
		if (IgnoreClassSubstrings.Num() > 0)
		{
			bool bIgnored = false;
			for (const FString& Sub : IgnoreClassSubstrings)
			{
				if (!Sub.IsEmpty() && ActorClassPath.Contains(Sub, ESearchCase::IgnoreCase))
				{
					bIgnored = true;
					break;
				}
			}
			if (bIgnored)
			{
				continue;
			}
		}

		FBox Bounds(EForceInit::ForceInit);
		FString BoundsError;
		FVector CandidateCenter = Actor->GetActorLocation();
		if (UeAgentLevelOps::GetActorBoundsBox(Actor, Bounds, BoundsError))
		{
			CandidateCenter = Bounds.GetCenter();
		}

		const double DistanceSquared = FVector::DistSquared(QueryCenter, CandidateCenter);
		if (DistanceSquared > RadiusSquared)
		{
			continue;
		}

		FBox LocalObbBox(EForceInit::ForceInit);
		FString ObbError;
		if (!UeAgentLevelOps::ComputeActorLocalObbBox(Actor, LocalObbBox, ObbError))
		{
			continue;
		}

		FNearbyObbEntry Entry;
		Entry.Actor = Actor;
		Entry.Distance = FMath::Sqrt(DistanceSquared);
		Entry.Obb = UeAgentLevelOps::MakeActorObbJson(Actor, LocalObbBox);
		NearbyEntries.Add(Entry);
	}

	NearbyEntries.Sort([](const FNearbyObbEntry& A, const FNearbyObbEntry& B)
	{
		if (!A.Actor || !B.Actor)
		{
			return A.Actor != nullptr;
		}
		if (FMath::Abs(A.Distance - B.Distance) > KINDA_SMALL_NUMBER)
		{
			return A.Distance < B.Distance;
		}
		return A.Actor->GetActorLabel() < B.Actor->GetActorLabel();
	});

	if (Limit > 0 && NearbyEntries.Num() > Limit)
	{
		NearbyEntries.SetNum(Limit);
	}

	TArray<TSharedPtr<FJsonValue>> NearbyActorJson;
	NearbyActorJson.Reserve(NearbyEntries.Num());
	for (const FNearbyObbEntry& Entry : NearbyEntries)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetObjectField(TEXT("actor"), UeAgentLevelOps::MakeActorSummaryJson(Entry.Actor));
		Item->SetNumberField(TEXT("distance"), Entry.Distance);
		Item->SetObjectField(TEXT("obb"), Entry.Obb);
		NearbyActorJson.Add(MakeShared<FJsonValueObject>(Item));
	}

	OutData->SetStringField(TEXT("id"), Id);
	OutData->SetBoolField(TEXT("has_source_actor"), SourceActor != nullptr);
	if (SourceActor)
	{
		FBox SourceLocalObbBox(EForceInit::ForceInit);
		FString SourceObbError;
		TSharedPtr<FJsonObject> SourceObbJson = MakeShared<FJsonObject>();
		if (UeAgentLevelOps::ComputeActorLocalObbBox(SourceActor, SourceLocalObbBox, SourceObbError))
		{
			SourceObbJson = UeAgentLevelOps::MakeActorObbJson(SourceActor, SourceLocalObbBox);
		}

		OutData->SetObjectField(TEXT("source_actor"), UeAgentLevelOps::MakeActorInfoJson(SourceActor));
		OutData->SetObjectField(TEXT("source_obb"), SourceObbJson);
	}
	OutData->SetObjectField(TEXT("query_center"), VecToJson(QueryCenter));
	OutData->SetNumberField(TEXT("radius"), Radius);
	OutData->SetBoolField(TEXT("include_self"), bIncludeSelf);
	OutData->SetStringField(TEXT("folder_path_prefix"), FolderPathPrefix);
	OutData->SetStringField(TEXT("ignore_folder_path_prefix"), IgnoreFolderPathPrefix);
	const auto StringsToJsonArray = [](const TArray<FString>& InStrings)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(InStrings.Num());
		for (const FString& Str : InStrings)
		{
			Out.Add(MakeShared<FJsonValueString>(Str));
		}
		return Out;
	};
	OutData->SetArrayField(TEXT("accept_tags"), StringsToJsonArray(AcceptTags));
	OutData->SetArrayField(TEXT("ignore_tags"), StringsToJsonArray(IgnoreTags));
	OutData->SetArrayField(TEXT("accept_class_substrings"), StringsToJsonArray(AcceptClassSubstrings));
	OutData->SetArrayField(TEXT("ignore_class_substrings"), StringsToJsonArray(IgnoreClassSubstrings));
	OutData->SetNumberField(TEXT("limit"), Limit);
	OutData->SetNumberField(TEXT("count"), NearbyActorJson.Num());
	OutData->SetArrayField(TEXT("actors"), NearbyActorJson);
	return true;
}

bool FUeAgentLegacyMechanics::CmdMeshGetClosestVertex(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FString Id;
	FString ComponentId;
	if (!JsonTryGetString(Ctx.Params, TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("missing_id");
		return false;
	}
	JsonTryGetString(Ctx.Params, TEXT("component"), ComponentId);

	AActor* Actor = FindActorByNameOrLabel(World, Id);
	UStaticMeshComponent* MeshComponent = nullptr;
	if (!UeAgentLevelOps::GetStaticMeshComponentForVertexQuery(Actor, ComponentId, MeshComponent, OutError))
	{
		return false;
	}

	FVector ReferencePoint = FVector::ZeroVector;
	if (!JsonTryGetVector(Ctx.Params, TEXT("world_point"), ReferencePoint))
	{
		OutError = TEXT("missing_world_point");
		return false;
	}

	TArray<FVector> LocalPositions;
	if (!UeAgentLevelOps::GetStaticMeshVertexLocalPositions(MeshComponent->GetStaticMesh(), LocalPositions, OutError))
	{
		return false;
	}

	int32 ClosestVertexIndex = INDEX_NONE;
	double ClosestDistanceSq = TNumericLimits<double>::Max();
	FVector ClosestWorld = FVector::ZeroVector;
	for (int32 Index = 0; Index < LocalPositions.Num(); ++Index)
	{
		const FVector WorldPosition = MeshComponent->GetComponentTransform().TransformPosition(LocalPositions[Index]);
		const double DistanceSq = FVector::DistSquared(WorldPosition, ReferencePoint);
		if (DistanceSq < ClosestDistanceSq)
		{
			ClosestDistanceSq = DistanceSq;
			ClosestVertexIndex = Index;
			ClosestWorld = WorldPosition;
		}
	}

	if (ClosestVertexIndex == INDEX_NONE)
	{
		OutError = TEXT("closest_vertex_not_found");
		return false;
	}

	OutData->SetStringField(TEXT("actor_name"), Actor->GetName());
	OutData->SetStringField(TEXT("component_name"), MeshComponent->GetName());
	OutData->SetNumberField(TEXT("vertex_index"), ClosestVertexIndex);
	OutData->SetObjectField(TEXT("world_position"), VecToJson(ClosestWorld));
	OutData->SetNumberField(TEXT("distance"), FMath::Sqrt(ClosestDistanceSq));
	return true;
}

bool FUeAgentLegacyMechanics::CmdMeshGetVertexWorldPosition(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FString Id;
	FString ComponentId;
	if (!JsonTryGetString(Ctx.Params, TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("missing_id");
		return false;
	}
	JsonTryGetString(Ctx.Params, TEXT("component"), ComponentId);

	double VertexIndexNumber = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("vertex_index"), VertexIndexNumber))
	{
		OutError = TEXT("missing_vertex_index");
		return false;
	}
	const int32 VertexIndex = (int32)VertexIndexNumber;

	AActor* Actor = FindActorByNameOrLabel(World, Id);
	UStaticMeshComponent* MeshComponent = nullptr;
	if (!UeAgentLevelOps::GetStaticMeshComponentForVertexQuery(Actor, ComponentId, MeshComponent, OutError))
	{
		return false;
	}

	TArray<FVector> LocalPositions;
	if (!UeAgentLevelOps::GetStaticMeshVertexLocalPositions(MeshComponent->GetStaticMesh(), LocalPositions, OutError))
	{
		return false;
	}

	FVector WorldPosition = FVector::ZeroVector;
	if (!UeAgentLevelOps::ResolveVertexWorldPosition(MeshComponent, LocalPositions, VertexIndex, WorldPosition, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("actor_name"), Actor->GetName());
	OutData->SetStringField(TEXT("component_name"), MeshComponent->GetName());
	OutData->SetNumberField(TEXT("vertex_index"), VertexIndex);
	OutData->SetObjectField(TEXT("world_position"), VecToJson(WorldPosition));
	return true;
}

bool FUeAgentLegacyMechanics::CmdLevelAlignActorVertexToVertex(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FString SourceId;
	FString TargetId;
	FString SourceComponentId;
	FString TargetComponentId;
	if (!JsonTryGetString(Ctx.Params, TEXT("source_actor_id"), SourceId) || SourceId.IsEmpty())
	{
		OutError = TEXT("missing_source_actor_id");
		return false;
	}
	if (!JsonTryGetString(Ctx.Params, TEXT("target_actor_id"), TargetId) || TargetId.IsEmpty())
	{
		OutError = TEXT("missing_target_actor_id");
		return false;
	}
	JsonTryGetString(Ctx.Params, TEXT("source_component"), SourceComponentId);
	JsonTryGetString(Ctx.Params, TEXT("target_component"), TargetComponentId);

	AActor* SourceActor = FindActorByNameOrLabel(World, SourceId);
	AActor* TargetActor = FindActorByNameOrLabel(World, TargetId);
	if (!SourceActor)
	{
		OutError = TEXT("source_actor_not_found");
		return false;
	}
	if (!TargetActor)
	{
		OutError = TEXT("target_actor_not_found");
		return false;
	}

	UStaticMeshComponent* SourceComponent = nullptr;
	UStaticMeshComponent* TargetComponent = nullptr;
	if (!UeAgentLevelOps::GetStaticMeshComponentForVertexQuery(SourceActor, SourceComponentId, SourceComponent, OutError) ||
		!UeAgentLevelOps::GetStaticMeshComponentForVertexQuery(TargetActor, TargetComponentId, TargetComponent, OutError))
	{
		return false;
	}

	TArray<FVector> SourceLocalPositions;
	TArray<FVector> TargetLocalPositions;
	if (!UeAgentLevelOps::GetStaticMeshVertexLocalPositions(SourceComponent->GetStaticMesh(), SourceLocalPositions, OutError) ||
		!UeAgentLevelOps::GetStaticMeshVertexLocalPositions(TargetComponent->GetStaticMesh(), TargetLocalPositions, OutError))
	{
		return false;
	}

	auto ResolveVertexIndex = [&](const TCHAR* IndexField, const TCHAR* PointField, UStaticMeshComponent* MeshComponent, const TArray<FVector>& LocalPositions, int32& OutVertexIndex, FVector& OutWorldPosition) -> bool
	{
		double IndexNumber = 0.0;
		if (JsonTryGetNumber(Ctx.Params, IndexField, IndexNumber))
		{
			OutVertexIndex = (int32)IndexNumber;
			return UeAgentLevelOps::ResolveVertexWorldPosition(MeshComponent, LocalPositions, OutVertexIndex, OutWorldPosition, OutError);
		}

		FVector WorldPoint = FVector::ZeroVector;
		if (!JsonTryGetVector(Ctx.Params, PointField, WorldPoint))
		{
			OutError = FString::Printf(TEXT("missing_%s_or_%s"), IndexField, PointField);
			return false;
		}

		OutVertexIndex = INDEX_NONE;
		double ClosestDistanceSq = TNumericLimits<double>::Max();
		for (int32 Index = 0; Index < LocalPositions.Num(); ++Index)
		{
			const FVector CandidateWorld = MeshComponent->GetComponentTransform().TransformPosition(LocalPositions[Index]);
			const double DistanceSq = FVector::DistSquared(CandidateWorld, WorldPoint);
			if (DistanceSq < ClosestDistanceSq)
			{
				ClosestDistanceSq = DistanceSq;
				OutVertexIndex = Index;
				OutWorldPosition = CandidateWorld;
			}
		}
		if (OutVertexIndex == INDEX_NONE)
		{
			OutError = TEXT("closest_vertex_not_found");
			return false;
		}
		return true;
	};

	int32 SourceVertexIndex = INDEX_NONE;
	int32 TargetVertexIndex = INDEX_NONE;
	FVector SourceWorldBefore = FVector::ZeroVector;
	FVector TargetWorld = FVector::ZeroVector;
	if (!ResolveVertexIndex(TEXT("source_vertex_index"), TEXT("source_world_point"), SourceComponent, SourceLocalPositions, SourceVertexIndex, SourceWorldBefore) ||
		!ResolveVertexIndex(TEXT("target_vertex_index"), TEXT("target_world_point"), TargetComponent, TargetLocalPositions, TargetVertexIndex, TargetWorld))
	{
		return false;
	}

	const FVector Delta = TargetWorld - SourceWorldBefore;
	SourceActor->Modify();
	SourceActor->SetActorLocation(SourceActor->GetActorLocation() + Delta, false, nullptr, ETeleportType::TeleportPhysics);

	FVector SourceWorldAfter = FVector::ZeroVector;
	if (!UeAgentLevelOps::ResolveVertexWorldPosition(SourceComponent, SourceLocalPositions, SourceVertexIndex, SourceWorldAfter, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("source_actor_name"), SourceActor->GetName());
	OutData->SetStringField(TEXT("target_actor_name"), TargetActor->GetName());
	OutData->SetNumberField(TEXT("source_vertex_index"), SourceVertexIndex);
	OutData->SetNumberField(TEXT("target_vertex_index"), TargetVertexIndex);
	OutData->SetObjectField(TEXT("source_vertex_world_before"), VecToJson(SourceWorldBefore));
	OutData->SetObjectField(TEXT("source_vertex_world_after"), VecToJson(SourceWorldAfter));
	OutData->SetObjectField(TEXT("target_vertex_world"), VecToJson(TargetWorld));
	OutData->SetObjectField(TEXT("applied_delta"), VecToJson(Delta));
	OutData->SetObjectField(TEXT("source_actor_location_after"), VecToJson(SourceActor->GetActorLocation()));
	return true;
}

bool FUeAgentLegacyMechanics::CmdLevelAlignActorByBounds(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FString SourceActorId;
	FString TargetActorId;
	FString AxisText = TEXT("x");
	FString SourceAnchorText = TEXT("min");
	FString TargetAnchorText = TEXT("max");
	double Offset = 0.0;

	if (!JsonTryGetString(Ctx.Params, TEXT("source_actor_id"), SourceActorId) || SourceActorId.IsEmpty())
	{
		OutError = TEXT("missing_source_actor_id");
		return false;
	}
	if (!JsonTryGetString(Ctx.Params, TEXT("target_actor_id"), TargetActorId) || TargetActorId.IsEmpty())
	{
		OutError = TEXT("missing_target_actor_id");
		return false;
	}
	JsonTryGetString(Ctx.Params, TEXT("axis"), AxisText);
	JsonTryGetString(Ctx.Params, TEXT("source_anchor"), SourceAnchorText);
	JsonTryGetString(Ctx.Params, TEXT("target_anchor"), TargetAnchorText);
	JsonTryGetNumber(Ctx.Params, TEXT("offset"), Offset);

	UeAgentLevelOps::EBoundsAxis Axis = UeAgentLevelOps::EBoundsAxis::X;
	if (!UeAgentLevelOps::ParseBoundsAxis(AxisText, Axis))
	{
		OutError = TEXT("invalid_axis");
		return false;
	}

	UeAgentLevelOps::EBoundsAnchor SourceAnchor = UeAgentLevelOps::EBoundsAnchor::Min;
	UeAgentLevelOps::EBoundsAnchor TargetAnchor = UeAgentLevelOps::EBoundsAnchor::Max;
	if (!UeAgentLevelOps::ParseBoundsAnchor(SourceAnchorText, SourceAnchor))
	{
		OutError = TEXT("invalid_source_anchor");
		return false;
	}
	if (!UeAgentLevelOps::ParseBoundsAnchor(TargetAnchorText, TargetAnchor))
	{
		OutError = TEXT("invalid_target_anchor");
		return false;
	}

	AActor* SourceActor = FindActorByNameOrLabel(World, SourceActorId);
	AActor* TargetActor = FindActorByNameOrLabel(World, TargetActorId);
	if (!SourceActor)
	{
		OutError = TEXT("source_actor_not_found");
		return false;
	}
	if (!TargetActor)
	{
		OutError = TEXT("target_actor_not_found");
		return false;
	}

	FBox SourceBoundsBefore(EForceInit::ForceInit);
	FBox TargetBounds(EForceInit::ForceInit);
	if (!UeAgentLevelOps::GetActorBoundsBox(SourceActor, SourceBoundsBefore, OutError) ||
		!UeAgentLevelOps::GetActorBoundsBox(TargetActor, TargetBounds, OutError))
	{
		return false;
	}

	const double SourceValue = UeAgentLevelOps::GetBoundsAnchorValue(SourceBoundsBefore, Axis, SourceAnchor);
	const double TargetValue = UeAgentLevelOps::GetBoundsAnchorValue(TargetBounds, Axis, TargetAnchor);
	const double DeltaValue = TargetValue - SourceValue + Offset;

	FVector NewLocation = SourceActor->GetActorLocation();
	UeAgentLevelOps::SetAxisValue(NewLocation, Axis, UeAgentLevelOps::GetAxisValue(NewLocation, Axis) + DeltaValue);

	FUeAgentInterfaceLogger::Log(
		TEXT("CmdLevelAlignActorByBounds req=%s source=%s target=%s axis=%s source_anchor=%s target_anchor=%s offset=%.3f delta=%.3f"),
		*Ctx.RequestId,
		*SourceActorId,
		*TargetActorId,
		*AxisText,
		*SourceAnchorText,
		*TargetAnchorText,
		Offset,
		DeltaValue);

	SourceActor->Modify();
	SourceActor->SetActorLocation(NewLocation, false, nullptr, ETeleportType::TeleportPhysics);
	UeAgentLevelOps::FinishActorTransformEdit(SourceActor);

	FBox SourceBoundsAfter(EForceInit::ForceInit);
	if (!UeAgentLevelOps::GetActorBoundsBox(SourceActor, SourceBoundsAfter, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("source_actor_name"), SourceActor->GetName());
	OutData->SetStringField(TEXT("target_actor_name"), TargetActor->GetName());
	OutData->SetStringField(TEXT("axis"), AxisText.ToLower());
	OutData->SetStringField(TEXT("source_anchor"), SourceAnchorText.ToLower());
	OutData->SetStringField(TEXT("target_anchor"), TargetAnchorText.ToLower());
	OutData->SetNumberField(TEXT("offset"), Offset);
	OutData->SetNumberField(TEXT("applied_delta"), DeltaValue);
	OutData->SetObjectField(TEXT("source_bounds_before"), UeAgentLevelOps::MakeBoundsJson(SourceBoundsBefore));
	OutData->SetObjectField(TEXT("source_bounds_after"), UeAgentLevelOps::MakeBoundsJson(SourceBoundsAfter));
	OutData->SetObjectField(TEXT("target_bounds"), UeAgentLevelOps::MakeBoundsJson(TargetBounds));
	UeAgentLevelOps::FillActorTransformFields(SourceActor, OutData);
	return true;
}

bool FUeAgentLegacyMechanics::CmdLevelAlignFaceToFace(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString SourceActorId;
	FString TargetActorId;
	if (!JsonTryGetString(Ctx.Params, TEXT("source_actor_id"), SourceActorId) || SourceActorId.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_source_actor_id");
		return false;
	}
	if (!JsonTryGetString(Ctx.Params, TEXT("target_actor_id"), TargetActorId) || TargetActorId.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_target_actor_id");
		return false;
	}
	SourceActorId = SourceActorId.TrimStartAndEnd();
	TargetActorId = TargetActorId.TrimStartAndEnd();

	FString AxisText = TEXT("x");
	JsonTryGetString(Ctx.Params, TEXT("axis"), AxisText);
	AxisText = AxisText.TrimStartAndEnd().ToLower();

	FString SourceFaceText = TEXT("min");
	FString TargetFaceText = TEXT("max");
	JsonTryGetString(Ctx.Params, TEXT("source_face"), SourceFaceText);
	JsonTryGetString(Ctx.Params, TEXT("target_face"), TargetFaceText);
	SourceFaceText = SourceFaceText.TrimStartAndEnd().ToLower();
	TargetFaceText = TargetFaceText.TrimStartAndEnd().ToLower();

	double OffsetCm = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("offset_cm"), OffsetCm))
	{
		JsonTryGetNumber(Ctx.Params, TEXT("offset"), OffsetCm);
	}

	auto ResolveFace = [](const FString& Face, FString& OutAxis, FString& OutAnchor) -> bool
	{
		OutAxis.Empty();
		OutAnchor.Empty();

		if (Face == TEXT("min") || Face == TEXT("max") || Face == TEXT("center"))
		{
			OutAnchor = Face;
			return true;
		}

		if (Face == TEXT("-x") || Face == TEXT("x-") || Face == TEXT("negx"))
		{
			OutAxis = TEXT("x");
			OutAnchor = TEXT("min");
			return true;
		}
		if (Face == TEXT("+x") || Face == TEXT("x+") || Face == TEXT("posx"))
		{
			OutAxis = TEXT("x");
			OutAnchor = TEXT("max");
			return true;
		}
		if (Face == TEXT("-y") || Face == TEXT("y-") || Face == TEXT("negy"))
		{
			OutAxis = TEXT("y");
			OutAnchor = TEXT("min");
			return true;
		}
		if (Face == TEXT("+y") || Face == TEXT("y+") || Face == TEXT("posy"))
		{
			OutAxis = TEXT("y");
			OutAnchor = TEXT("max");
			return true;
		}
		if (Face == TEXT("-z") || Face == TEXT("z-") || Face == TEXT("negz"))
		{
			OutAxis = TEXT("z");
			OutAnchor = TEXT("min");
			return true;
		}
		if (Face == TEXT("+z") || Face == TEXT("z+") || Face == TEXT("posz"))
		{
			OutAxis = TEXT("z");
			OutAnchor = TEXT("max");
			return true;
		}

		return false;
	};

	FString SourceAxisFromFace;
	FString TargetAxisFromFace;
	FString SourceAnchorText;
	FString TargetAnchorText;
	if (!ResolveFace(SourceFaceText, SourceAxisFromFace, SourceAnchorText))
	{
		OutError = TEXT("invalid_source_face");
		return false;
	}
	if (!ResolveFace(TargetFaceText, TargetAxisFromFace, TargetAnchorText))
	{
		OutError = TEXT("invalid_target_face");
		return false;
	}

	FString AxisResolved = AxisText;
	if (!SourceAxisFromFace.IsEmpty())
	{
		AxisResolved = SourceAxisFromFace;
	}
	if (!TargetAxisFromFace.IsEmpty())
	{
		if (AxisResolved.IsEmpty())
		{
			AxisResolved = TargetAxisFromFace;
		}
		else if (AxisResolved != TargetAxisFromFace)
		{
			OutError = TEXT("face_axis_mismatch");
			return false;
		}
	}
	if (AxisResolved.IsEmpty())
	{
		AxisResolved = TEXT("x");
	}

	TSharedPtr<FJsonObject> ParamsCopy = MakeShared<FJsonObject>();
	ParamsCopy->SetStringField(TEXT("source_actor_id"), SourceActorId);
	ParamsCopy->SetStringField(TEXT("target_actor_id"), TargetActorId);
	ParamsCopy->SetStringField(TEXT("axis"), AxisResolved);
	ParamsCopy->SetStringField(TEXT("source_anchor"), SourceAnchorText);
	ParamsCopy->SetStringField(TEXT("target_anchor"), TargetAnchorText);
	ParamsCopy->SetNumberField(TEXT("offset"), OffsetCm);

	FUeAgentRequestContext NewCtx = Ctx;
	NewCtx.Command = TEXT("level_align_actor_by_bounds");
	NewCtx.Params = ParamsCopy;
	const bool bOk = CmdLevelAlignActorByBounds(NewCtx, OutData, OutError);
	if (!bOk)
	{
		return false;
	}

	OutData->SetStringField(TEXT("source_face"), SourceFaceText);
	OutData->SetStringField(TEXT("target_face"), TargetFaceText);
	OutData->SetNumberField(TEXT("offset_cm"), OffsetCm);
	return true;
}
