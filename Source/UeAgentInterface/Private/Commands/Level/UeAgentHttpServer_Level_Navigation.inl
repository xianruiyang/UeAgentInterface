bool FUeAgentHttpServer::CmdNavmeshBuild(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!NavSys)
	{
		OutError = TEXT("navmesh_navigation_system_not_found");
		return false;
	}

	int32 NavDataCount = 0;
	{
		for (TActorIterator<ANavigationData> It(World); It; ++It)
		{
			if (IsValid(*It))
			{
				++NavDataCount;
			}
		}
	}

	if (NavDataCount <= 0)
	{
		OutError = TEXT("navmesh_no_nav_data");
		return false;
	}

	bool bWaitForFinish = false;
	JsonTryGetBool(Ctx.Params, TEXT("wait_for_finish"), bWaitForFinish);

	double TimeoutSeconds = 10.0;
	JsonTryGetNumber(Ctx.Params, TEXT("timeout_seconds"), TimeoutSeconds);
	TimeoutSeconds = FMath::Clamp(TimeoutSeconds, 0.1, 120.0);

	NavSys->Build();

	if (bWaitForFinish)
	{
		const double StartTime = FPlatformTime::Seconds();
		while (NavSys->IsNavigationBuildInProgress() && (FPlatformTime::Seconds() - StartTime) < TimeoutSeconds)
		{
			if (FTaskGraphInterface::IsRunning())
			{
				FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			}
			FTSTicker::GetCoreTicker().Tick(0.01f);
			FPlatformProcess::Sleep(0.01f);
		}
	}

	const bool bInProgress = NavSys->IsNavigationBuildInProgress();

	OutData->SetBoolField(TEXT("build_requested"), true);
	OutData->SetBoolField(TEXT("build_in_progress"), bInProgress);
	OutData->SetNumberField(TEXT("nav_data_count"), NavDataCount);
	OutData->SetBoolField(TEXT("wait_for_finish"), bWaitForFinish);
	OutData->SetNumberField(TEXT("timeout_seconds"), TimeoutSeconds);
	return true;
}

bool FUeAgentHttpServer::CmdNavmeshProjectPoint(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!NavSys)
	{
		OutError = TEXT("navmesh_navigation_system_not_found");
		return false;
	}

	int32 NavDataCount = 0;
	for (TActorIterator<ANavigationData> It(World); It; ++It)
	{
		if (IsValid(*It))
		{
			++NavDataCount;
		}
	}
	if (NavDataCount <= 0)
	{
		OutError = TEXT("navmesh_no_nav_data");
		return false;
	}

	FVector Point = FVector::ZeroVector;
	if (!JsonTryGetVector(Ctx.Params, TEXT("point"), Point) && !JsonTryGetVector(Ctx.Params, TEXT("location"), Point))
	{
		OutError = TEXT("missing_point");
		return false;
	}

	FVector ProjectQueryExtent(50.0f, 50.0f, 200.0f);
	JsonTryGetVector(Ctx.Params, TEXT("project_query_extent"), ProjectQueryExtent);
	JsonTryGetVector(Ctx.Params, TEXT("project_query_extent_cm"), ProjectQueryExtent);
	ProjectQueryExtent.X = FMath::Clamp(FMath::Abs(ProjectQueryExtent.X), 1.0f, 100000.0f);
	ProjectQueryExtent.Y = FMath::Clamp(FMath::Abs(ProjectQueryExtent.Y), 1.0f, 100000.0f);
	ProjectQueryExtent.Z = FMath::Clamp(FMath::Abs(ProjectQueryExtent.Z), 1.0f, 100000.0f);

	FNavLocation Projected;
	const bool bProjected = NavSys->ProjectPointToNavigation(Point, Projected, ProjectQueryExtent);

	OutData->SetObjectField(TEXT("point"), VecToJson(Point));
	OutData->SetObjectField(TEXT("project_query_extent"), VecToJson(ProjectQueryExtent));
	OutData->SetBoolField(TEXT("projected"), bProjected);
	if (bProjected)
	{
		OutData->SetObjectField(TEXT("projected_point"), VecToJson(Projected.Location));
	}
	OutData->SetNumberField(TEXT("nav_data_count"), NavDataCount);
	return true;
}

bool FUeAgentHttpServer::CmdNavmeshFindPath(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!NavSys)
	{
		OutError = TEXT("navmesh_navigation_system_not_found");
		return false;
	}

	int32 NavDataCount = 0;
	for (TActorIterator<ANavigationData> It(World); It; ++It)
	{
		if (IsValid(*It))
		{
			++NavDataCount;
		}
	}
	if (NavDataCount <= 0)
	{
		OutError = TEXT("navmesh_no_nav_data");
		return false;
	}

	FVector Start = FVector::ZeroVector;
	FVector End = FVector::ZeroVector;
	if (!JsonTryGetVector(Ctx.Params, TEXT("start"), Start))
	{
		OutError = TEXT("missing_start");
		return false;
	}
	if (!JsonTryGetVector(Ctx.Params, TEXT("end"), End))
	{
		OutError = TEXT("missing_end");
		return false;
	}

	bool bAllowPartial = false;
	JsonTryGetBool(Ctx.Params, TEXT("allow_partial"), bAllowPartial);

	bool bProjectToNav = true;
	JsonTryGetBool(Ctx.Params, TEXT("project_to_nav"), bProjectToNav);

	bool bAllowProjectionFailure = false;
	JsonTryGetBool(Ctx.Params, TEXT("allow_projection_failure"), bAllowProjectionFailure);

	FVector StartUsed = Start;
	FVector EndUsed = End;
	FVector StartProjected = Start;
	FVector EndProjected = End;
	FVector ProjectQueryExtent(50.0f, 50.0f, 200.0f);

	bool bStartProjectedOk = true;
	bool bEndProjectedOk = true;
	FString ProjectionFailedReason;
	if (bProjectToNav)
	{
		JsonTryGetVector(Ctx.Params, TEXT("project_query_extent"), ProjectQueryExtent);
		JsonTryGetVector(Ctx.Params, TEXT("project_query_extent_cm"), ProjectQueryExtent);
		ProjectQueryExtent.X = FMath::Clamp(FMath::Abs(ProjectQueryExtent.X), 1.0f, 100000.0f);
		ProjectQueryExtent.Y = FMath::Clamp(FMath::Abs(ProjectQueryExtent.Y), 1.0f, 100000.0f);
		ProjectQueryExtent.Z = FMath::Clamp(FMath::Abs(ProjectQueryExtent.Z), 1.0f, 100000.0f);
		FNavLocation NavStart;
		FNavLocation NavEnd;
		bStartProjectedOk = NavSys->ProjectPointToNavigation(Start, NavStart, ProjectQueryExtent);
		bEndProjectedOk = NavSys->ProjectPointToNavigation(End, NavEnd, ProjectQueryExtent);
		if (!bStartProjectedOk || !bEndProjectedOk)
		{
			if (!bStartProjectedOk && !bEndProjectedOk)
			{
				ProjectionFailedReason = TEXT("start_and_end_projection_failed");
			}
			else if (!bStartProjectedOk)
			{
				ProjectionFailedReason = TEXT("start_projection_failed");
			}
			else
			{
				ProjectionFailedReason = TEXT("end_projection_failed");
			}

			if (!bAllowProjectionFailure)
			{
				OutError = !bStartProjectedOk ? TEXT("navmesh_project_start_failed") : TEXT("navmesh_project_end_failed");
				return false;
			}
		}

		if (bStartProjectedOk)
		{
			StartUsed = NavStart.Location;
			StartProjected = StartUsed;
		}
		if (bEndProjectedOk)
		{
			EndUsed = NavEnd.Location;
			EndProjected = EndUsed;
		}
	}

	if (bProjectToNav && bAllowProjectionFailure && (!bStartProjectedOk || !bEndProjectedOk))
	{
		OutData->SetObjectField(TEXT("start"), VecToJson(Start));
		OutData->SetObjectField(TEXT("end"), VecToJson(End));
		OutData->SetBoolField(TEXT("allow_partial"), bAllowPartial);
		OutData->SetBoolField(TEXT("project_to_nav"), bProjectToNav);
		OutData->SetBoolField(TEXT("allow_projection_failure"), bAllowProjectionFailure);
		OutData->SetObjectField(TEXT("project_query_extent"), VecToJson(ProjectQueryExtent));
		OutData->SetBoolField(TEXT("start_projected_ok"), bStartProjectedOk);
		OutData->SetBoolField(TEXT("end_projected_ok"), bEndProjectedOk);
		if (bStartProjectedOk)
		{
			OutData->SetObjectField(TEXT("start_projected"), VecToJson(StartProjected));
			OutData->SetNumberField(TEXT("start_projected_distance_cm"), FVector::Dist(Start, StartProjected));
		}
		if (bEndProjectedOk)
		{
			OutData->SetObjectField(TEXT("end_projected"), VecToJson(EndProjected));
			OutData->SetNumberField(TEXT("end_projected_distance_cm"), FVector::Dist(End, EndProjected));
		}
		OutData->SetStringField(TEXT("projection_failed_reason"), ProjectionFailedReason);
		OutData->SetObjectField(TEXT("start_used"), VecToJson(StartUsed));
		OutData->SetObjectField(TEXT("end_used"), VecToJson(EndUsed));
		OutData->SetBoolField(TEXT("path_found"), false);
		OutData->SetBoolField(TEXT("is_partial"), false);
		OutData->SetNumberField(TEXT("path_length_cm"), 0.0);
		OutData->SetArrayField(TEXT("path_points"), TArray<TSharedPtr<FJsonValue>>());
		OutData->SetNumberField(TEXT("nav_data_count"), NavDataCount);
		return true;
	}

	UNavigationPath* Path = UNavigationSystemV1::FindPathToLocationSynchronously(World, StartUsed, EndUsed, nullptr);
	const bool bValidPath = Path && Path->IsValid();
	const bool bIsPartial = bValidPath ? Path->IsPartial() : false;
	const bool bAccepted = bValidPath && (bAllowPartial || !bIsPartial);

	OutData->SetObjectField(TEXT("start"), VecToJson(Start));
	OutData->SetObjectField(TEXT("end"), VecToJson(End));
	OutData->SetBoolField(TEXT("allow_partial"), bAllowPartial);
	OutData->SetBoolField(TEXT("project_to_nav"), bProjectToNav);
	OutData->SetBoolField(TEXT("allow_projection_failure"), bAllowProjectionFailure);
	OutData->SetObjectField(TEXT("start_used"), VecToJson(StartUsed));
	OutData->SetObjectField(TEXT("end_used"), VecToJson(EndUsed));
	if (bProjectToNav)
	{
		OutData->SetObjectField(TEXT("project_query_extent"), VecToJson(ProjectQueryExtent));
		OutData->SetBoolField(TEXT("start_projected_ok"), bStartProjectedOk);
		OutData->SetBoolField(TEXT("end_projected_ok"), bEndProjectedOk);
		if (bStartProjectedOk)
		{
			OutData->SetObjectField(TEXT("start_projected"), VecToJson(StartProjected));
			OutData->SetNumberField(TEXT("start_projected_distance_cm"), FVector::Dist(Start, StartProjected));
		}
		if (bEndProjectedOk)
		{
			OutData->SetObjectField(TEXT("end_projected"), VecToJson(EndProjected));
			OutData->SetNumberField(TEXT("end_projected_distance_cm"), FVector::Dist(End, EndProjected));
		}
	}
	OutData->SetBoolField(TEXT("path_found"), bAccepted);
	OutData->SetBoolField(TEXT("is_partial"), bIsPartial);

	double PathLength = 0.0;
	TArray<TSharedPtr<FJsonValue>> PathPoints;
	if (bValidPath)
	{
		PathLength = Path->GetPathLength();
		PathPoints.Reserve(Path->PathPoints.Num());
		for (const FVector& Point : Path->PathPoints)
		{
			PathPoints.Add(MakeShared<FJsonValueObject>(VecToJson(Point)));
		}
	}

	OutData->SetNumberField(TEXT("path_length_cm"), PathLength);
	OutData->SetArrayField(TEXT("path_points"), PathPoints);
	OutData->SetNumberField(TEXT("nav_data_count"), NavDataCount);
	return true;
}

bool FUeAgentHttpServer::CmdLevelValidateConnectivity(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!NavSys)
	{
		OutError = TEXT("navmesh_navigation_system_not_found");
		return false;
	}

	int32 NavDataCount = 0;
	for (TActorIterator<ANavigationData> It(World); It; ++It)
	{
		if (IsValid(*It))
		{
			++NavDataCount;
		}
	}
	if (NavDataCount <= 0)
	{
		OutError = TEXT("navmesh_no_nav_data");
		return false;
	}

	bool bAllowPartial = false;
	JsonTryGetBool(Ctx.Params, TEXT("allow_partial"), bAllowPartial);

	bool bProjectToNav = true;
	JsonTryGetBool(Ctx.Params, TEXT("project_to_nav"), bProjectToNav);

	FVector ProjectQueryExtent(50.0f, 50.0f, 200.0f);
	JsonTryGetVector(Ctx.Params, TEXT("project_query_extent"), ProjectQueryExtent);
	JsonTryGetVector(Ctx.Params, TEXT("project_query_extent_cm"), ProjectQueryExtent);
	ProjectQueryExtent.X = FMath::Clamp(FMath::Abs(ProjectQueryExtent.X), 1.0f, 100000.0f);
	ProjectQueryExtent.Y = FMath::Clamp(FMath::Abs(ProjectQueryExtent.Y), 1.0f, 100000.0f);
	ProjectQueryExtent.Z = FMath::Clamp(FMath::Abs(ProjectQueryExtent.Z), 1.0f, 100000.0f);

	double MaxProjectionDistanceCm = -1.0;
	JsonTryGetNumber(Ctx.Params, TEXT("max_projection_distance_cm"), MaxProjectionDistanceCm);
	MaxProjectionDistanceCm = FMath::Clamp(MaxProjectionDistanceCm, -1.0, 10000000.0);

	bool bStopOnFailure = false;
	JsonTryGetBool(Ctx.Params, TEXT("stop_on_failure"), bStopOnFailure);

	auto TryGetVectorFromValue = [](const TSharedPtr<FJsonValue>& Value, FVector& OutVec) -> bool
	{
		if (!Value.IsValid())
		{
			return false;
		}
		const TSharedPtr<FJsonObject> Obj = Value->AsObject();
		if (!Obj.IsValid())
		{
			return false;
		}
		double X = 0.0, Y = 0.0, Z = 0.0;
		if (!UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("x"), TEXT("X") }, X)
			|| !UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("y"), TEXT("Y") }, Y)
			|| !UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("z"), TEXT("Z") }, Z))
		{
			return false;
		}
		OutVec = FVector((float)X, (float)Y, (float)Z);
		return true;
	};

	TArray<FVector> Nodes;
	TArray<TSharedPtr<FJsonValue>> NodeSources;

	const TArray<TSharedPtr<FJsonValue>>* ProbeActorIds = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("probe_actor_ids"), ProbeActorIds) && ProbeActorIds && ProbeActorIds->Num() > 0)
	{
		Nodes.Reserve(ProbeActorIds->Num());
		NodeSources.Reserve(ProbeActorIds->Num());
		for (const TSharedPtr<FJsonValue>& IdValue : *ProbeActorIds)
		{
			const FString Id = IdValue.IsValid() ? IdValue->AsString().TrimStartAndEnd() : FString();
			if (Id.IsEmpty())
			{
				OutError = TEXT("probe_actor_ids_contains_empty");
				return false;
			}

			AActor* Actor = FindActorByNameOrLabel(World, Id);
			if (!Actor)
			{
				OutError = TEXT("probe_actor_not_found");
				return false;
			}

			Nodes.Add(Actor->GetActorLocation());
			NodeSources.Add(MakeShared<FJsonValueObject>(UeAgentLevelOps::MakeActorSummaryJson(Actor)));
		}
	}
	else
	{
		const TArray<TSharedPtr<FJsonValue>>* PointsJson = nullptr;
		if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetArrayField(TEXT("points"), PointsJson) || !PointsJson || PointsJson->Num() <= 0)
		{
			OutError = TEXT("points_or_probe_actor_ids_required");
			return false;
		}

		Nodes.Reserve(PointsJson->Num());
		NodeSources.Reserve(PointsJson->Num());
		for (const TSharedPtr<FJsonValue>& Value : *PointsJson)
		{
			FVector P;
			if (!TryGetVectorFromValue(Value, P))
			{
				OutError = TEXT("points_invalid_vector");
				return false;
			}
			Nodes.Add(P);
			NodeSources.Add(MakeShared<FJsonValueObject>(VecToJson(P)));
		}
	}

	if (Nodes.Num() < 2)
	{
		OutError = TEXT("not_enough_nodes");
		return false;
	}

	struct FPair
	{
		int32 From = 0;
		int32 To = 0;
		FString EdgeType;
	};

	TArray<FPair> Pairs;
	const TArray<TSharedPtr<FJsonValue>>* PairsJson = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("pairs"), PairsJson) && PairsJson && PairsJson->Num() > 0)
	{
		Pairs.Reserve(PairsJson->Num());
		for (const TSharedPtr<FJsonValue>& PairValue : *PairsJson)
		{
			const TSharedPtr<FJsonObject> Obj = PairValue.IsValid() ? PairValue->AsObject() : nullptr;
			if (!Obj.IsValid())
			{
				OutError = TEXT("pairs_invalid_item");
				return false;
			}
			double FromNumber = 0.0;
			double ToNumber = 0.0;
			if (!JsonTryGetNumber(Obj, TEXT("from_index"), FromNumber) || !JsonTryGetNumber(Obj, TEXT("to_index"), ToNumber))
			{
				OutError = TEXT("pairs_missing_indices");
				return false;
			}
			const int32 FromIndex = (int32)FromNumber;
			const int32 ToIndex = (int32)ToNumber;
			if (FromIndex < 0 || FromIndex >= Nodes.Num() || ToIndex < 0 || ToIndex >= Nodes.Num())
			{
				OutError = TEXT("pairs_index_out_of_range");
				return false;
			}

			FString EdgeType;
			JsonTryGetString(Obj, TEXT("edge_type"), EdgeType);
			EdgeType = EdgeType.TrimStartAndEnd();

			FPair P;
			P.From = FromIndex;
			P.To = ToIndex;
			P.EdgeType = EdgeType;
			Pairs.Add(P);
		}
	}
	else
	{
		Pairs.Reserve(Nodes.Num() - 1);
		for (int32 i = 0; i < Nodes.Num() - 1; ++i)
		{
			FPair P;
			P.From = i;
			P.To = i + 1;
			P.EdgeType = TEXT("walk");
			Pairs.Add(P);
		}
	}

	TArray<TSharedPtr<FJsonValue>> Results;
	Results.Reserve(Pairs.Num());

	bool bIncludePathPoints = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_path_points"), bIncludePathPoints);

	int32 MaxPathPoints = 256;
	{
		double MaxPathPointsNumber = (double)MaxPathPoints;
		JsonTryGetNumber(Ctx.Params, TEXT("max_path_points"), MaxPathPointsNumber);
		MaxPathPoints = (int32)FMath::Clamp(MaxPathPointsNumber, 1.0, 5000.0);
	}

	struct FEdgeEval
	{
		int32 From = 0;
		int32 To = 0;
		bool bPassed = false;
		FString EdgeType;
	};
	TArray<FEdgeEval> EvaluatedEdges;
	EvaluatedEdges.Reserve(Pairs.Num());

	bool bPairsTruncated = false;
	int32 FirstFailurePairIndex = INDEX_NONE;
	FString FirstFailureReason;

	bool bAllConnected = true;
	for (int32 PairIndex = 0; PairIndex < Pairs.Num(); ++PairIndex)
	{
		const FPair Pair = Pairs[PairIndex];
		const FVector Start = Nodes[Pair.From];
		const FVector End = Nodes[Pair.To];
		const FString EdgeTypeTrim = Pair.EdgeType.TrimStartAndEnd();
		const FString EdgeTypeNormalized = EdgeTypeTrim.ToLower();
		const bool bWalkEdge = EdgeTypeNormalized.IsEmpty() || EdgeTypeNormalized == TEXT("walk");

		FVector StartUsed = Start;
		FVector EndUsed = End;
		FVector StartProjected = Start;
		FVector EndProjected = End;
		bool bStartProjectedOk = false;
		bool bEndProjectedOk = false;
		double StartProjectedDistanceCm = -1.0;
		double EndProjectedDistanceCm = -1.0;
		bool bStartProjectionWithinLimit = true;
		bool bEndProjectionWithinLimit = true;
		FString FailureReason;

		if (bProjectToNav)
		{
			FNavLocation NavStart;
			FNavLocation NavEnd;
			bStartProjectedOk = NavSys->ProjectPointToNavigation(Start, NavStart, ProjectQueryExtent);
			bEndProjectedOk = NavSys->ProjectPointToNavigation(End, NavEnd, ProjectQueryExtent);
			if (bStartProjectedOk)
			{
				StartUsed = NavStart.Location;
				StartProjected = StartUsed;
				StartProjectedDistanceCm = FVector::Dist(Start, StartProjected);
			}
			if (bEndProjectedOk)
			{
				EndUsed = NavEnd.Location;
				EndProjected = EndUsed;
				EndProjectedDistanceCm = FVector::Dist(End, EndProjected);
			}

			if (MaxProjectionDistanceCm >= 0.0)
			{
				bStartProjectionWithinLimit = bStartProjectedOk && StartProjectedDistanceCm >= 0.0 && StartProjectedDistanceCm <= MaxProjectionDistanceCm;
				bEndProjectionWithinLimit = bEndProjectedOk && EndProjectedDistanceCm >= 0.0 && EndProjectedDistanceCm <= MaxProjectionDistanceCm;
			}
			else
			{
				bStartProjectionWithinLimit = bStartProjectedOk;
				bEndProjectionWithinLimit = bEndProjectedOk;
			}

			if (!bStartProjectedOk || !bEndProjectedOk)
			{
				FailureReason = !bStartProjectedOk ? TEXT("project_start_failed") : TEXT("project_end_failed");
			}
			else if (!bStartProjectionWithinLimit || !bEndProjectionWithinLimit)
			{
				FailureReason = !bStartProjectionWithinLimit ? TEXT("project_start_too_far") : TEXT("project_end_too_far");
			}
		}

		bool bPathFound = false;
		bool bIsPartial = false;
		double PathLength = 0.0;
		TArray<TSharedPtr<FJsonValue>> PathPoints;
		bool bPathPointsTruncated = false;
		if (FailureReason.IsEmpty())
		{
			if (bWalkEdge)
			{
				UNavigationPath* Path = UNavigationSystemV1::FindPathToLocationSynchronously(World, StartUsed, EndUsed, nullptr);
				const bool bValidPath = Path && Path->IsValid();
				bIsPartial = bValidPath ? Path->IsPartial() : false;
				const bool bAccepted = bValidPath && (bAllowPartial || !bIsPartial);
				bPathFound = bAccepted;
				PathLength = bValidPath ? Path->GetPathLength() : 0.0;

				if (bIncludePathPoints && bValidPath)
				{
					const int32 TotalPoints = Path->PathPoints.Num();
					const int32 EmitCount = FMath::Min(MaxPathPoints, TotalPoints);
					bPathPointsTruncated = TotalPoints > EmitCount;
					PathPoints.Reserve(EmitCount);
					for (int32 PointIndex = 0; PointIndex < EmitCount; ++PointIndex)
					{
						PathPoints.Add(MakeShared<FJsonValueObject>(VecToJson(Path->PathPoints[PointIndex])));
					}
				}
			}
			else
			{
				// Explicit edge: only validates endpoints (projection/within_limit). Device semantics are outside plugin scope.
				bPathFound = true;
				bIsPartial = false;
				PathLength = 0.0;
			}
		}

		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("pair_index"), PairIndex);
		Item->SetNumberField(TEXT("from_index"), Pair.From);
		Item->SetNumberField(TEXT("to_index"), Pair.To);
		Item->SetStringField(TEXT("edge_type"), EdgeTypeTrim.IsEmpty() ? TEXT("walk") : EdgeTypeTrim);
		Item->SetBoolField(TEXT("walk_checked"), bWalkEdge);
		Item->SetObjectField(TEXT("start"), VecToJson(Start));
		Item->SetObjectField(TEXT("end"), VecToJson(End));
		Item->SetBoolField(TEXT("project_to_nav"), bProjectToNav);
		Item->SetObjectField(TEXT("project_query_extent"), VecToJson(ProjectQueryExtent));
		Item->SetNumberField(TEXT("max_projection_distance_cm"), MaxProjectionDistanceCm);
		Item->SetBoolField(TEXT("start_projected_ok"), bStartProjectedOk);
		Item->SetBoolField(TEXT("end_projected_ok"), bEndProjectedOk);
		Item->SetBoolField(TEXT("start_projection_within_limit"), bStartProjectionWithinLimit);
		Item->SetBoolField(TEXT("end_projection_within_limit"), bEndProjectionWithinLimit);
		Item->SetObjectField(TEXT("start_projected"), VecToJson(StartProjected));
		Item->SetObjectField(TEXT("end_projected"), VecToJson(EndProjected));
		Item->SetNumberField(TEXT("start_projected_distance_cm"), StartProjectedDistanceCm);
		Item->SetNumberField(TEXT("end_projected_distance_cm"), EndProjectedDistanceCm);
		Item->SetBoolField(TEXT("allow_partial"), bAllowPartial);
		Item->SetBoolField(TEXT("path_found"), bPathFound);
		Item->SetBoolField(TEXT("is_partial"), bIsPartial);
		Item->SetNumberField(TEXT("path_length_cm"), PathLength);
		Item->SetStringField(TEXT("failure_reason"), FailureReason);
		if (bIncludePathPoints)
		{
			Item->SetBoolField(TEXT("path_points_truncated"), bPathPointsTruncated);
			Item->SetArrayField(TEXT("path_points"), PathPoints);
		}

		Results.Add(MakeShared<FJsonValueObject>(Item));

		FEdgeEval Eval;
		Eval.From = Pair.From;
		Eval.To = Pair.To;
		Eval.bPassed = bPathFound;
		Eval.EdgeType = EdgeTypeTrim.IsEmpty() ? TEXT("walk") : EdgeTypeTrim;
		EvaluatedEdges.Add(Eval);

		if (!bPathFound)
		{
			bAllConnected = false;
			if (FirstFailurePairIndex == INDEX_NONE)
			{
				FirstFailurePairIndex = PairIndex;
				FirstFailureReason = FailureReason;
			}
			if (bStopOnFailure)
			{
				bPairsTruncated = PairIndex < (Pairs.Num() - 1);
				break;
			}
		}
	}

	// Graph summary (directed) based on passed edges.
	// - "walk" edges: validated via NavMesh path.
	// - explicit edges (edge_type != "walk"): only validates endpoints (projection/within_limit). Device semantics are outside plugin scope.
	double RootIndexNumber = 0.0;
	JsonTryGetNumber(Ctx.Params, TEXT("graph_root_index"), RootIndexNumber);
	const int32 RootIndex = FMath::Clamp((int32)RootIndexNumber, 0, Nodes.Num() - 1);

	TArray<TArray<int32>> Adj;
	TArray<TArray<int32>> Radj;
	Adj.SetNum(Nodes.Num());
	Radj.SetNum(Nodes.Num());

	int32 PassedEdgeCount = 0;
	for (const FEdgeEval& Edge : EvaluatedEdges)
	{
		if (!Edge.bPassed)
		{
			continue;
		}
		if (Edge.From < 0 || Edge.From >= Nodes.Num() || Edge.To < 0 || Edge.To >= Nodes.Num())
		{
			continue;
		}
		Adj[Edge.From].Add(Edge.To);
		Radj[Edge.To].Add(Edge.From);
		++PassedEdgeCount;
	}

	TArray<bool> Reachable;
	Reachable.Init(false, Nodes.Num());
	TArray<int32> ReachableStack;
	ReachableStack.Reserve(Nodes.Num());
	Reachable[RootIndex] = true;
	ReachableStack.Add(RootIndex);
	while (ReachableStack.Num() > 0)
	{
		const int32 V = ReachableStack.Pop();
		for (const int32 W : Adj[V])
		{
			if (W < 0 || W >= Nodes.Num())
			{
				continue;
			}
			if (!Reachable[W])
			{
				Reachable[W] = true;
				ReachableStack.Add(W);
			}
		}
	}

	TArray<TSharedPtr<FJsonValue>> ReachableIndices;
	TArray<TSharedPtr<FJsonValue>> UnreachableIndices;
	ReachableIndices.Reserve(Nodes.Num());
	UnreachableIndices.Reserve(Nodes.Num());
	bool bAllReachableFromRoot = true;
	for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
	{
		if (Reachable[NodeIndex])
		{
			ReachableIndices.Add(MakeShared<FJsonValueNumber>(NodeIndex));
		}
		else
		{
			bAllReachableFromRoot = false;
			UnreachableIndices.Add(MakeShared<FJsonValueNumber>(NodeIndex));
		}
	}

	// Kosaraju SCC (iterative) for loop/backtrack detection.
	struct FStackEntry
	{
		int32 V = 0;
		int32 NextIndex = 0;
	};
	TArray<uint8> Vis;
	Vis.Init(0, Nodes.Num());
	TArray<int32> Order;
	Order.Reserve(Nodes.Num());
	for (int32 V = 0; V < Nodes.Num(); ++V)
	{
		if (Vis[V])
		{
			continue;
		}
		TArray<FStackEntry> Stack;
		Stack.Reserve(64);
		Stack.Add({ V, 0 });
		Vis[V] = 1;
		while (Stack.Num() > 0)
		{
			FStackEntry& Top = Stack.Last();
			if (Top.NextIndex < Adj[Top.V].Num())
			{
				const int32 W = Adj[Top.V][Top.NextIndex++];
				if (W >= 0 && W < Nodes.Num() && !Vis[W])
				{
					Vis[W] = 1;
					Stack.Add({ W, 0 });
				}
			}
			else
			{
				Order.Add(Top.V);
				Stack.Pop();
			}
		}
	}

	TArray<uint8> Vis2;
	Vis2.Init(0, Nodes.Num());
	TArray<TSharedPtr<FJsonValue>> SccsJson;
	SccsJson.Reserve(Nodes.Num());
	bool bHasCycle = false;
	for (int32 OrderIndex = Order.Num() - 1; OrderIndex >= 0; --OrderIndex)
	{
		const int32 V = Order[OrderIndex];
		if (V < 0 || V >= Nodes.Num() || Vis2[V])
		{
			continue;
		}

		TArray<int32> Component;
		TArray<int32> Dfs;
		Dfs.Add(V);
		Vis2[V] = 1;
		while (Dfs.Num() > 0)
		{
			const int32 N = Dfs.Pop();
			Component.Add(N);
			for (const int32 W : Radj[N])
			{
				if (W >= 0 && W < Nodes.Num() && !Vis2[W])
				{
					Vis2[W] = 1;
					Dfs.Add(W);
				}
			}
		}

		if (Component.Num() > 1)
		{
			bHasCycle = true;
		}
		else
		{
			const int32 Only = Component[0];
			for (const int32 W : Adj[Only])
			{
				if (W == Only)
				{
					bHasCycle = true;
					break;
				}
			}
		}

		TArray<TSharedPtr<FJsonValue>> ComponentJson;
		ComponentJson.Reserve(Component.Num());
		for (const int32 NodeIndex : Component)
		{
			ComponentJson.Add(MakeShared<FJsonValueNumber>(NodeIndex));
		}
		SccsJson.Add(MakeShared<FJsonValueArray>(ComponentJson));
	}

	TSharedPtr<FJsonObject> GraphJson = MakeShared<FJsonObject>();
	GraphJson->SetNumberField(TEXT("root_index"), RootIndex);
	GraphJson->SetNumberField(TEXT("node_count"), Nodes.Num());
	GraphJson->SetNumberField(TEXT("edge_count"), EvaluatedEdges.Num());
	GraphJson->SetNumberField(TEXT("passed_edge_count"), PassedEdgeCount);
	GraphJson->SetBoolField(TEXT("all_reachable_from_root"), bAllReachableFromRoot);
	GraphJson->SetArrayField(TEXT("reachable_indices"), ReachableIndices);
	GraphJson->SetArrayField(TEXT("unreachable_indices"), UnreachableIndices);
	GraphJson->SetBoolField(TEXT("has_cycle"), bHasCycle);
	GraphJson->SetNumberField(TEXT("scc_count"), SccsJson.Num());
	GraphJson->SetArrayField(TEXT("sccs"), SccsJson);

	OutData->SetBoolField(TEXT("all_connected"), bAllConnected);
	OutData->SetNumberField(TEXT("node_count"), Nodes.Num());
	OutData->SetArrayField(TEXT("nodes"), NodeSources);
	OutData->SetNumberField(TEXT("pair_count"), Results.Num());
	OutData->SetArrayField(TEXT("pairs"), Results);
	OutData->SetBoolField(TEXT("pairs_truncated"), bPairsTruncated);
	OutData->SetNumberField(TEXT("first_failure_pair_index"), FirstFailurePairIndex);
	OutData->SetStringField(TEXT("first_failure_reason"), FirstFailureReason);
	OutData->SetBoolField(TEXT("include_path_points"), bIncludePathPoints);
	OutData->SetNumberField(TEXT("max_path_points"), MaxPathPoints);
	OutData->SetObjectField(TEXT("graph"), GraphJson);
	OutData->SetNumberField(TEXT("nav_data_count"), NavDataCount);
	return true;
}

bool FUeAgentHttpServer::CmdNavmeshSpawnBoundsVolume(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	OutData = MakeShared<FJsonObject>();

	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	TSharedPtr<FJsonObject> BoundsObj;
	if (!Ctx.Params.IsValid() || !JsonTryGetObject(Ctx.Params, TEXT("bounds"), BoundsObj) || !BoundsObj.IsValid())
	{
		OutError = TEXT("bounds_required");
		return false;
	}

	FVector Center = FVector::ZeroVector;
	FVector Extent = FVector::ZeroVector;
	if (!JsonTryGetVector(BoundsObj, TEXT("center"), Center) || !JsonTryGetVector(BoundsObj, TEXT("extent"), Extent))
	{
		OutError = TEXT("bounds_center_or_extent_required");
		return false;
	}

	Extent = FVector(FMath::Abs(Extent.X), FMath::Abs(Extent.Y), FMath::Abs(Extent.Z));
	if (Extent.X <= KINDA_SMALL_NUMBER || Extent.Y <= KINDA_SMALL_NUMBER || Extent.Z <= KINDA_SMALL_NUMBER)
	{
		OutError = TEXT("bounds_extent_invalid");
		return false;
	}

	FRotator Rotation = FRotator::ZeroRotator;
	JsonTryGetRotator(Ctx.Params, TEXT("rotation"), Rotation);

	FString Label;
	JsonTryGetString(Ctx.Params, TEXT("label"), Label);

	FString FolderPath;
	JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath);

	bool bUpdateNavigationBounds = true;
	JsonTryGetBool(Ctx.Params, TEXT("update_navigation_bounds"), bUpdateNavigationBounds);

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.ObjectFlags |= RF_Transactional;

	ANavMeshBoundsVolume* Volume = World->SpawnActor<ANavMeshBoundsVolume>(Center, Rotation, SpawnParams);
	if (!Volume)
	{
		OutError = TEXT("navmesh_bounds_spawn_failed");
		return false;
	}

	if (!Label.IsEmpty())
	{
		Volume->SetActorLabel(Label);
	}

	// `CreateBrushForVolumeActor` may reset location/rotation; restore after creation.
	Volume->SetActorLocation(Center);
	Volume->SetActorRotation(Rotation);

	UCubeBuilder* Builder = NewObject<UCubeBuilder>(GetTransientPackage());
	Builder->X = Extent.X * 2.0f;
	Builder->Y = Extent.Y * 2.0f;
	Builder->Z = Extent.Z * 2.0f;
	UActorFactory::CreateBrushForVolumeActor(Volume, Builder);

	Volume->SetActorLocation(Center);
	Volume->SetActorRotation(Rotation);

	if (!FolderPath.IsEmpty())
	{
		Volume->SetFolderPath(*FolderPath);
	}

	bool bNavBoundsUpdated = false;
	if (bUpdateNavigationBounds)
	{
		if (UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World))
		{
			NavSys->OnNavigationBoundsUpdated(Volume);
			bNavBoundsUpdated = true;
		}
	}

	const FBox WorldBounds = Volume->GetComponentsBoundingBox(true);
	OutData->SetObjectField(TEXT("created_actor"), UeAgentLevelOps::MakeActorSummaryJson(Volume));
	OutData->SetObjectField(TEXT("bounds"), UeAgentLevelOps::MakeBoundsJson(WorldBounds));
	OutData->SetObjectField(TEXT("requested_center"), VecToJson(Center));
	OutData->SetObjectField(TEXT("requested_extent"), VecToJson(Extent));
	OutData->SetObjectField(TEXT("builder_size"), VecToJson(FVector(Builder->X, Builder->Y, Builder->Z)));
	OutData->SetBoolField(TEXT("update_navigation_bounds"), bUpdateNavigationBounds);
	OutData->SetBoolField(TEXT("nav_bounds_updated"), bNavBoundsUpdated);
	return true;
}
