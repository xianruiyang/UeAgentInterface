bool FUeAgentHttpServer::CmdModelingSaveMeshAsset(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentModelingOps;
	OutData = MakeShared<FJsonObject>();

	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	TArray<AActor*> Actors;
	if (!ResolveSelectionActorsFromContext(Ctx, World, Actors, OutError))
	{
		return false;
	}

	int32 SavedCount = 0;
	if (!SaveActorStaticMeshes(Actors, SavedCount))
	{
		OutError = TEXT("no_static_mesh_packages_saved");
		return false;
	}

	OutData->SetNumberField(TEXT("saved_package_count"), SavedCount);
	FUeAgentInterfaceLogger::Log(TEXT("ModelingSaveMeshAsset req=%s saved_packages=%d"), *Ctx.RequestId, SavedCount);
	return true;
}

bool FUeAgentHttpServer::CmdModelingReplaceActorMesh(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentModelingOps;
	OutData = MakeShared<FJsonObject>();

	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FString ActorId;
	FString MeshPath;
	if (!Ctx.Params.IsValid() ||
		!Ctx.Params->TryGetStringField(TEXT("actor_id"), ActorId))
	{
		OutError = TEXT("actor_id_or_static_mesh_path_missing");
		return false;
	}

	if (!Ctx.Params->TryGetStringField(TEXT("static_mesh_path"), MeshPath))
	{
		Ctx.Params->TryGetStringField(TEXT("static_mesh_asset"), MeshPath);
	}

	if (MeshPath.IsEmpty())
	{
		OutError = TEXT("actor_id_or_static_mesh_path_missing");
		return false;
	}

	AActor* Actor = FindActorByNameOrLabel(World, ActorId);
	if (!Actor)
	{
		OutError = TEXT("actor_not_found");
		return false;
	}

	UStaticMesh* Mesh = LoadStaticMeshAsset(MeshPath);
	if (!Mesh)
	{
		OutError = TEXT("static_mesh_not_found");
		return false;
	}

	UStaticMeshComponent* StaticMeshComponent = Actor->FindComponentByClass<UStaticMeshComponent>();
	if (!StaticMeshComponent)
	{
		OutError = TEXT("static_mesh_component_not_found");
		return false;
	}

	StaticMeshComponent->Modify();
	StaticMeshComponent->SetStaticMesh(Mesh);
	StaticMeshComponent->MarkRenderStateDirty();
	Actor->MarkPackageDirty();

	OutData->SetStringField(TEXT("actor_name"), Actor->GetName());
	OutData->SetStringField(TEXT("static_mesh"), Mesh->GetPathName());
	return true;
}

bool FUeAgentHttpServer::CmdModelingSnapToGround(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentModelingOps;
	OutData = MakeShared<FJsonObject>();

	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FString ActorId;
	if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetStringField(TEXT("actor_id"), ActorId) || ActorId.IsEmpty())
	{
		OutError = TEXT("actor_id_required");
		return false;
	}

	AActor* Actor = FindActorByNameOrLabel(World, ActorId);
	if (!Actor)
	{
		OutError = TEXT("actor_not_found");
		return false;
	}

	double TraceDistance = 100000.0;
	Ctx.Params->TryGetNumberField(TEXT("trace_distance"), TraceDistance);

	const FVector Origin = Actor->GetActorLocation();
	FVector BoundsOrigin = FVector::ZeroVector;
	FVector BoundsExtent = FVector::ZeroVector;
	Actor->GetActorBounds(true, BoundsOrigin, BoundsExtent);

	const FVector TraceStart = FVector(Origin.X, Origin.Y, BoundsOrigin.Z + FMath::Max(BoundsExtent.Z, 10.0f) + 100.0f);
	const FVector TraceEnd = TraceStart - FVector(0, 0, TraceDistance);

	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(UeAgentModelingSnapToGround), true, Actor);
	if (!World->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_WorldStatic, Params))
	{
		OutError = TEXT("ground_trace_miss");
		return false;
	}

	const FVector NewLocation = FVector(Origin.X, Origin.Y, Hit.ImpactPoint.Z + BoundsExtent.Z);
	Actor->Modify();
	Actor->SetActorLocation(NewLocation, false, nullptr, ETeleportType::TeleportPhysics);
	Actor->PostEditMove(true);
	Actor->MarkPackageDirty();

	OutData->SetObjectField(TEXT("location"), MakeVectorJson(NewLocation));
	OutData->SetObjectField(TEXT("hit_location"), MakeVectorJson(Hit.ImpactPoint));
	OutData->SetObjectField(TEXT("hit_normal"), MakeVectorJson(Hit.ImpactNormal));
	return true;
}
