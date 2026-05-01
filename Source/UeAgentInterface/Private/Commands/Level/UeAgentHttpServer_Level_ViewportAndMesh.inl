bool FUeAgentHttpServer::CmdViewportGetCamera(TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FLevelEditorViewportClient* LVC = UeAgentLevelOps::GetPreferredLevelViewportClient();
	if (!LVC)
	{
		OutError = TEXT("no_level_editor_viewport");
		return false;
	}

	OutData->SetObjectField(TEXT("location"), VecToJson(LVC->GetViewLocation()));
	OutData->SetObjectField(TEXT("rotation"), RotToJson(LVC->GetViewRotation()));
	OutData->SetNumberField(TEXT("fov"), LVC->ViewFOV);
	OutData->SetBoolField(TEXT("realtime"), LVC->IsRealtime());
	OutData->SetBoolField(TEXT("game_view"), LVC->IsInGameView());
	return true;
}

bool FUeAgentHttpServer::CmdViewportSetCamera(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FLevelEditorViewportClient* LVC = UeAgentLevelOps::GetPreferredLevelViewportClient();
	if (!LVC)
	{
		OutError = TEXT("no_level_editor_viewport");
		return false;
	}

	FVector Location = LVC->GetViewLocation();
	FRotator Rotation = LVC->GetViewRotation();
	JsonTryGetVector(Ctx.Params, TEXT("location"), Location);
	JsonTryGetRotator(Ctx.Params, TEXT("rotation"), Rotation);

	double FovD = (double)LVC->ViewFOV;
	if (JsonTryGetNumber(Ctx.Params, TEXT("fov"), FovD))
	{
		LVC->ViewFOV = (float)FMath::Clamp(FovD, 5.0, 170.0);
	}

	LVC->SetViewLocation(Location);
	LVC->SetViewRotation(Rotation);
	LVC->Invalidate();

	OutData->SetObjectField(TEXT("location"), VecToJson(LVC->GetViewLocation()));
	OutData->SetObjectField(TEXT("rotation"), RotToJson(LVC->GetViewRotation()));
	OutData->SetNumberField(TEXT("fov"), LVC->ViewFOV);
	OutData->SetBoolField(TEXT("realtime"), LVC->IsRealtime());
	OutData->SetBoolField(TEXT("game_view"), LVC->IsInGameView());
	return true;
}

bool FUeAgentHttpServer::CmdViewportSetRealtime(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FLevelEditorViewportClient* LVC = UeAgentLevelOps::GetPreferredLevelViewportClient();
	if (!LVC)
	{
		OutError = TEXT("no_level_editor_viewport");
		return false;
	}

	bool bRealtime = true;
	if (!JsonTryGetBool(Ctx.Params, TEXT("realtime"), bRealtime))
	{
		OutError = TEXT("missing_realtime");
		return false;
	}

	bool bStoreCurrentValue = false;
	JsonTryGetBool(Ctx.Params, TEXT("store_current_value"), bStoreCurrentValue);

	TArray<FLevelEditorViewportClient*> TargetClients;
	if (GEditor)
	{
		for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
		{
			if (ViewportClient && ViewportClient->Viewport)
			{
				TargetClients.AddUnique(ViewportClient);
			}
		}
	}
	if (TargetClients.Num() == 0)
	{
		TargetClients.Add(LVC);
	}

	const FText RealtimeOverrideSystem = NSLOCTEXT("UeAgentInterface", "RealtimeOverrideSystem", "UeAgentInterface");
	int32 MatchedRealtimeClients = 0;
	for (FLevelEditorViewportClient* ViewportClient : TargetClients)
	{
		if (!ViewportClient)
		{
			continue;
		}

		if (ViewportClient->HasRealtimeOverride(RealtimeOverrideSystem))
		{
			ViewportClient->RemoveRealtimeOverride(RealtimeOverrideSystem, false);
		}
		ViewportClient->SetRealtime(bRealtime);
		ViewportClient->AddRealtimeOverride(bRealtime, RealtimeOverrideSystem);
		ViewportClient->Invalidate();
		if (ViewportClient->Viewport)
		{
			ViewportClient->Viewport->Invalidate();
		}

		if (ViewportClient->IsRealtime() == bRealtime)
		{
			++MatchedRealtimeClients;
		}

		FUeAgentInterfaceLogger::Log(
			TEXT("ViewportSetRealtime: client=%p requested=%s applied=%s store_current_value=%s"),
			ViewportClient,
			bRealtime ? TEXT("true") : TEXT("false"),
			ViewportClient->IsRealtime() ? TEXT("true") : TEXT("false"),
			bStoreCurrentValue ? TEXT("true") : TEXT("false"));
	}

	OutData->SetBoolField(TEXT("realtime"), LVC->IsRealtime());
	OutData->SetNumberField(TEXT("target_client_count"), TargetClients.Num());
	OutData->SetNumberField(TEXT("matched_realtime_client_count"), MatchedRealtimeClients);
	OutData->SetBoolField(TEXT("stored_value_updated"), false);
	OutData->SetBoolField(TEXT("store_current_value_ignored"), bStoreCurrentValue);
	OutData->SetObjectField(TEXT("location"), VecToJson(LVC->GetViewLocation()));
	OutData->SetObjectField(TEXT("rotation"), RotToJson(LVC->GetViewRotation()));
	OutData->SetNumberField(TEXT("fov"), LVC->ViewFOV);
	return true;
}

bool FUeAgentHttpServer::CmdViewportSetGameView(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FLevelEditorViewportClient* LVC = UeAgentLevelOps::GetPreferredLevelViewportClient();
	if (!LVC)
	{
		OutError = TEXT("no_level_editor_viewport");
		return false;
	}

	bool bGameView = true;
	if (!JsonTryGetBool(Ctx.Params, TEXT("game_view"), bGameView))
	{
		OutError = TEXT("missing_game_view");
		return false;
	}

	TArray<FLevelEditorViewportClient*> TargetClients;
	if (GEditor)
	{
		for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
		{
			if (ViewportClient && ViewportClient->Viewport)
			{
				TargetClients.AddUnique(ViewportClient);
			}
		}
	}
	if (TargetClients.Num() == 0)
	{
		TargetClients.Add(LVC);
	}

	int32 MatchedGameViewClients = 0;
	for (FLevelEditorViewportClient* ViewportClient : TargetClients)
	{
		if (!ViewportClient)
		{
			continue;
		}

		ViewportClient->SetGameView(bGameView);
		ViewportClient->Invalidate();
		if (ViewportClient->Viewport)
		{
			ViewportClient->Viewport->Invalidate();
		}

		if (ViewportClient->IsInGameView() == bGameView)
		{
			++MatchedGameViewClients;
		}
	}

	OutData->SetBoolField(TEXT("game_view"), LVC->IsInGameView());
	OutData->SetNumberField(TEXT("target_client_count"), TargetClients.Num());
	OutData->SetNumberField(TEXT("matched_game_view_client_count"), MatchedGameViewClients);
	OutData->SetBoolField(TEXT("realtime"), LVC->IsRealtime());
	OutData->SetObjectField(TEXT("location"), VecToJson(LVC->GetViewLocation()));
	OutData->SetObjectField(TEXT("rotation"), RotToJson(LVC->GetViewRotation()));
	OutData->SetNumberField(TEXT("fov"), LVC->ViewFOV);
	return true;
}

bool FUeAgentHttpServer::CmdViewportFocusActor(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FString Id;
	if (!JsonTryGetString(Ctx.Params, TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("missing_id");
		return false;
	}

	AActor* Actor = FindActorByNameOrLabel(World, Id);
	if (!Actor)
	{
		OutError = TEXT("actor_not_found");
		return false;
	}

	FLevelEditorViewportClient* LVC = GCurrentLevelEditingViewportClient;
	LVC = UeAgentLevelOps::GetPreferredLevelViewportClient();
	if (!LVC)
	{
		OutError = TEXT("no_level_editor_viewport");
		return false;
	}

	const FBox Box = Actor->GetComponentsBoundingBox(/*bNonColliding*/ true);
	if (!Box.IsValid)
	{
		OutError = TEXT("actor_has_invalid_bounds");
		return false;
	}

	LVC->FocusViewportOnBox(Box, /*bInstant*/ true);
	LVC->Invalidate();
	OutData->SetStringField(TEXT("focused_name"), Actor->GetName());
	OutData->SetStringField(TEXT("focused_label"), Actor->GetActorLabel());
	return true;
}

bool FUeAgentHttpServer::CmdViewportFocusActorSafe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	// `viewport_focus_actor` uses the editor's FocusViewportOnBox, which is convenient but not collision-aware.
	// This "safe" variant delegates to `viewport_frame_actor` with better defaults for indoor whitebox validation.
	TSharedPtr<FJsonObject> ParamsCopy = MakeShared<FJsonObject>();
	if (Ctx.Params.IsValid())
	{
		ParamsCopy->Values = Ctx.Params->Values;
	}

	if (!ParamsCopy->HasField(TEXT("collision_aware")))
	{
		ParamsCopy->SetBoolField(TEXT("collision_aware"), true);
	}
	if (!ParamsCopy->HasField(TEXT("look_at")))
	{
		ParamsCopy->SetBoolField(TEXT("look_at"), true);
	}
	if (!ParamsCopy->HasField(TEXT("auto_fallback")))
	{
		ParamsCopy->SetBoolField(TEXT("auto_fallback"), true);
	}

	FUeAgentRequestContext NewCtx = Ctx;
	NewCtx.Params = ParamsCopy;
	return CmdViewportFrameActor(NewCtx, OutData, OutError);
}

bool FUeAgentHttpServer::CmdViewportFrameActor(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FString Id;
	if (!JsonTryGetString(Ctx.Params, TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("missing_id");
		return false;
	}

	AActor* Actor = FindActorByNameOrLabel(World, Id);
	if (!Actor)
	{
		OutError = TEXT("actor_not_found");
		return false;
	}

	FLevelEditorViewportClient* LVC = UeAgentLevelOps::GetPreferredLevelViewportClient();
	if (!LVC)
	{
		OutError = TEXT("no_level_editor_viewport");
		return false;
	}

	FBox Bounds(EForceInit::ForceInit);
	if (!UeAgentLevelOps::GetActorBoundsBox(Actor, Bounds, OutError))
	{
		return false;
	}

	double Padding = 1.1;
	JsonTryGetNumber(Ctx.Params, TEXT("padding"), Padding);

	const FVector PreviousLocation = LVC->GetViewLocation();
	const FRotator PreviousRotation = LVC->GetViewRotation();
	FVector DesiredLocation = PreviousLocation;
	FVector NewLocation = PreviousLocation;
	if (!UeAgentLevelOps::ComputeViewportLocationToFrameBounds(LVC, Bounds, PreviousRotation, Padding, NewLocation, OutError))
	{
		return false;
	}

	DesiredLocation = NewLocation;

	bool bCollisionAware = false;
	JsonTryGetBool(Ctx.Params, TEXT("collision_aware"), bCollisionAware);

	bool bLookAt = false;
	JsonTryGetBool(Ctx.Params, TEXT("look_at"), bLookAt);

	bool bCollisionAdjusted = false;
	bool bFallbackUsed = false;
	int32 FallbackIndex = 0;
	FVector FallbackOffset = FVector::ZeroVector;
	TSharedPtr<FJsonObject> CollisionTrace;
	if (bCollisionAware)
	{
		double SafetyOffsetCm = 15.0;
		JsonTryGetNumber(Ctx.Params, TEXT("safety_offset_cm"), SafetyOffsetCm);
		SafetyOffsetCm = FMath::Clamp(SafetyOffsetCm, 0.0, 10000.0);

		ECollisionChannel TraceChannel = ECC_Visibility;
		if (!UeAgentLevelOps::ResolveTraceChannel(Ctx.Params, TraceChannel))
		{
			OutError = TEXT("unsupported_trace_channel");
			return false;
		}

		bool bTraceComplex = false;
		JsonTryGetBool(Ctx.Params, TEXT("trace_complex"), bTraceComplex);

		FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(UeAgentViewportFrameActorCollision), bTraceComplex);
		QueryParams.AddIgnoredActor(Actor);

		bool bAutoFallback = false;
		JsonTryGetBool(Ctx.Params, TEXT("auto_fallback"), bAutoFallback);

		double FallbackStepCm = 200.0;
		JsonTryGetNumber(Ctx.Params, TEXT("fallback_step_cm"), FallbackStepCm);
		FallbackStepCm = FMath::Clamp(FallbackStepCm, 0.0, 10000.0);

		auto TryParseVectorObject = [](const TSharedPtr<FJsonObject>& Obj, FVector& OutVec) -> bool
		{
			if (!Obj.IsValid())
			{
				return false;
			}
			double X = 0.0;
			double Y = 0.0;
			double Z = 0.0;
			if (!UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("x"), TEXT("X") }, X)
				|| !UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("y"), TEXT("Y") }, Y)
				|| !UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("z"), TEXT("Z") }, Z))
			{
				return false;
			}
			OutVec = FVector(X, Y, Z);
			return true;
		};

		TArray<FVector> CandidateOffsets;
		CandidateOffsets.Reserve(8);
		CandidateOffsets.Add(FVector::ZeroVector);

		const TArray<TSharedPtr<FJsonValue>>* ExplicitOffsets = nullptr;
		if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("fallback_offsets_cm"), ExplicitOffsets) && ExplicitOffsets)
		{
			for (const TSharedPtr<FJsonValue>& Value : *ExplicitOffsets)
			{
				if (!Value.IsValid() || Value->Type != EJson::Object)
				{
					continue;
				}

				FVector Offset = FVector::ZeroVector;
				if (TryParseVectorObject(Value->AsObject(), Offset) && !Offset.IsNearlyZero())
				{
					CandidateOffsets.Add(Offset);
				}
			}
		}

		const FVector Target = Bounds.GetCenter();
		if (bAutoFallback && CandidateOffsets.Num() <= 1 && FallbackStepCm > KINDA_SMALL_NUMBER)
		{
			FVector BaseDir = (DesiredLocation - Target).GetSafeNormal();
			if (BaseDir.IsNearlyZero(KINDA_SMALL_NUMBER))
			{
				BaseDir = (PreviousLocation - Target).GetSafeNormal();
			}
			if (BaseDir.IsNearlyZero(KINDA_SMALL_NUMBER))
			{
				BaseDir = FVector::BackwardVector;
			}

			const FVector WorldUp = FVector::UpVector;
			FVector Right = FVector::CrossProduct(WorldUp, BaseDir).GetSafeNormal();
			if (Right.IsNearlyZero(KINDA_SMALL_NUMBER))
			{
				Right = FVector::CrossProduct(FVector::RightVector, BaseDir).GetSafeNormal();
			}
			FVector Up = FVector::CrossProduct(BaseDir, Right).GetSafeNormal();
			if (Up.IsNearlyZero(KINDA_SMALL_NUMBER))
			{
				Up = WorldUp;
			}

			CandidateOffsets.Add(Right * (float)FallbackStepCm);
			CandidateOffsets.Add(-Right * (float)FallbackStepCm);
			CandidateOffsets.Add(Up * (float)FallbackStepCm);
			CandidateOffsets.Add(-Up * (float)FallbackStepCm);
			CandidateOffsets.Add(BaseDir * (float)FallbackStepCm);
			CandidateOffsets.Add(-BaseDir * (float)FallbackStepCm);
		}

		struct FCollisionCandidate
		{
			int32 Index = 0;
			FVector Offset = FVector::ZeroVector;
			FVector Desired = FVector::ZeroVector;
			FVector Location = FVector::ZeroVector;
			bool bHit = false;
			bool bAdjusted = false;
			FHitResult Hit;
			double Score = 0.0;
		};

		FCollisionCandidate Best;
		Best.Score = -TNumericLimits<double>::Max();

		for (int32 Index = 0; Index < CandidateOffsets.Num(); ++Index)
		{
			const FVector Offset = CandidateOffsets[Index];
			const FVector CandidateDesired = DesiredLocation + Offset;

			FCollisionCandidate Candidate;
			Candidate.Index = Index;
			Candidate.Offset = Offset;
			Candidate.Desired = CandidateDesired;

			const bool bHit = World->LineTraceSingleByChannel(Candidate.Hit, Target, CandidateDesired, TraceChannel, QueryParams);
			Candidate.bHit = bHit;

			if (bHit && Candidate.Hit.bBlockingHit)
			{
				FVector Dir = (CandidateDesired - Target).GetSafeNormal();
				if (Dir.IsNearlyZero(KINDA_SMALL_NUMBER))
				{
					Dir = (DesiredLocation - Target).GetSafeNormal();
				}
				if (Dir.IsNearlyZero(KINDA_SMALL_NUMBER))
				{
					Dir = FVector::BackwardVector;
				}

				Candidate.Location = Candidate.Hit.ImpactPoint - (Dir * (float)SafetyOffsetCm);
				Candidate.bAdjusted = true;
			}
			else
			{
				Candidate.Location = CandidateDesired;
			}

			const double Distance = FVector::Dist(Target, Candidate.Location);
			Candidate.Score = Distance - (Candidate.bAdjusted ? 0.01 : 0.0);
			if (Candidate.Score > Best.Score)
			{
				Best = Candidate;
			}
		}

		NewLocation = Best.Location;
		bCollisionAdjusted = Best.bAdjusted;
		bFallbackUsed = (Best.Index != 0);
		FallbackIndex = Best.Index;
		FallbackOffset = Best.Offset;

		CollisionTrace = MakeShared<FJsonObject>();
		CollisionTrace->SetObjectField(TEXT("target"), VecToJson(Target));
		CollisionTrace->SetObjectField(TEXT("desired_location"), VecToJson(DesiredLocation));
		CollisionTrace->SetBoolField(TEXT("auto_fallback"), bAutoFallback);
		CollisionTrace->SetNumberField(TEXT("fallback_step_cm"), FallbackStepCm);
		CollisionTrace->SetNumberField(TEXT("candidate_count"), CandidateOffsets.Num());
		CollisionTrace->SetBoolField(TEXT("fallback_used"), bFallbackUsed);
		CollisionTrace->SetNumberField(TEXT("fallback_index"), FallbackIndex);
		CollisionTrace->SetObjectField(TEXT("fallback_offset_cm"), VecToJson(FallbackOffset));
		CollisionTrace->SetObjectField(TEXT("candidate_desired_location"), VecToJson(Best.Desired));
		CollisionTrace->SetStringField(TEXT("trace_channel"), FString::Printf(TEXT("%d"), (int32)TraceChannel));
		CollisionTrace->SetBoolField(TEXT("trace_complex"), bTraceComplex);
		CollisionTrace->SetNumberField(TEXT("safety_offset_cm"), SafetyOffsetCm);
		CollisionTrace->SetBoolField(TEXT("hit"), Best.bHit);
		UeAgentLevelOps::FillHitResultJson(Best.Hit, CollisionTrace);
		CollisionTrace->SetBoolField(TEXT("adjusted"), Best.bAdjusted);
		CollisionTrace->SetObjectField(TEXT("new_location"), VecToJson(NewLocation));
		if (Best.bAdjusted)
		{
			CollisionTrace->SetStringField(TEXT("adjustment_mode"), TEXT("hit_point_minus_trace_dir"));
		}
	}

	LVC->SetViewLocation(NewLocation);
	if (bLookAt)
	{
		const FVector Target = Bounds.GetCenter();
		const FVector Dir = (Target - NewLocation);
		if (!Dir.IsNearlyZero(KINDA_SMALL_NUMBER))
		{
			FRotator LookAtRot = Dir.Rotation();
			LookAtRot.Roll = 0.0f;
			LVC->SetViewRotation(LookAtRot);
		}
	}
	LVC->Invalidate();

	FUeAgentInterfaceLogger::Log(TEXT("CmdViewportFrameActor req=%s actor=%s padding=%.3f"), *Ctx.RequestId, *Id, Padding);
	OutData->SetObjectField(TEXT("actor"), UeAgentLevelOps::MakeActorSummaryJson(Actor));
	OutData->SetNumberField(TEXT("padding"), Padding);
	OutData->SetObjectField(TEXT("bounds"), UeAgentLevelOps::MakeBoundsJson(Bounds));
	OutData->SetObjectField(TEXT("previous_location"), VecToJson(PreviousLocation));
	OutData->SetObjectField(TEXT("desired_location"), VecToJson(DesiredLocation));
	OutData->SetObjectField(TEXT("new_location"), VecToJson(LVC->GetViewLocation()));
	OutData->SetBoolField(TEXT("collision_aware"), bCollisionAware);
	OutData->SetBoolField(TEXT("collision_adjusted"), bCollisionAdjusted);
	OutData->SetBoolField(TEXT("fallback_used"), bFallbackUsed);
	OutData->SetNumberField(TEXT("fallback_index"), FallbackIndex);
	OutData->SetObjectField(TEXT("fallback_offset_cm"), VecToJson(FallbackOffset));
	OutData->SetBoolField(TEXT("look_at"), bLookAt);
	if (CollisionTrace.IsValid())
	{
		OutData->SetObjectField(TEXT("collision_trace"), CollisionTrace);
	}
	OutData->SetObjectField(TEXT("rotation"), RotToJson(LVC->GetViewRotation()));
	OutData->SetNumberField(TEXT("fov"), LVC->ViewFOV);
	OutData->SetBoolField(TEXT("realtime"), LVC->IsRealtime());
	OutData->SetBoolField(TEXT("game_view"), LVC->IsInGameView());
	return true;
}

bool FUeAgentHttpServer::CmdViewportFrameSelection(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FLevelEditorViewportClient* LVC = UeAgentLevelOps::GetPreferredLevelViewportClient();
	if (!LVC)
	{
		OutError = TEXT("no_level_editor_viewport");
		return false;
	}
	if (!GEditor)
	{
		OutError = TEXT("missing_editor");
		return false;
	}

	const bool bInstant = [&Ctx]()
	{
		bool bValue = true;
		JsonTryGetBool(Ctx.Params, TEXT("instant"), bValue);
		return bValue;
	}();

	USelection* SelectedActors = GEditor->GetSelectedActors();
	if (!SelectedActors || SelectedActors->Num() <= 0)
	{
		OutError = TEXT("selection_empty");
		return false;
	}

	FBox CombinedBox(EForceInit::ForceInit);
	TArray<TSharedPtr<FJsonValue>> FocusedActors;
	for (FSelectionIterator It(*SelectedActors); It; ++It)
	{
		AActor* Actor = Cast<AActor>(*It);
		if (!Actor)
		{
			continue;
		}

		const FBox ActorBox = Actor->GetComponentsBoundingBox(true);
		if (ActorBox.IsValid)
		{
			CombinedBox += ActorBox;
		}
		FocusedActors.Add(MakeShared<FJsonValueObject>(UeAgentLevelOps::MakeActorSummaryJson(Actor)));
	}

	if (!CombinedBox.IsValid || FocusedActors.Num() <= 0)
	{
		OutError = TEXT("selection_has_invalid_bounds");
		return false;
	}

	FUeAgentInterfaceLogger::Log(TEXT("CmdViewportFrameSelection req=%s count=%d instant=%s"), *Ctx.RequestId, FocusedActors.Num(), bInstant ? TEXT("true") : TEXT("false"));
	LVC->FocusViewportOnBox(CombinedBox, bInstant);
	LVC->Invalidate();

	OutData->SetNumberField(TEXT("focused_count"), FocusedActors.Num());
	OutData->SetBoolField(TEXT("instant"), bInstant);
	OutData->SetArrayField(TEXT("actors"), FocusedActors);
	OutData->SetObjectField(TEXT("bounds"), UeAgentLevelOps::MakeBoundsJson(CombinedBox));
	return true;
}

bool FUeAgentHttpServer::CmdViewportFrameActors(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FLevelEditorViewportClient* LVC = UeAgentLevelOps::GetPreferredLevelViewportClient();
	if (!LVC)
	{
		OutError = TEXT("no_level_editor_viewport");
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* ActorIds = nullptr;
	if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetArrayField(TEXT("actor_ids"), ActorIds) || !ActorIds || ActorIds->Num() <= 0)
	{
		OutError = TEXT("missing_actor_ids");
		return false;
	}

	bool bInstant = true;
	JsonTryGetBool(Ctx.Params, TEXT("instant"), bInstant);

	FBox CombinedBox(EForceInit::ForceInit);
	TArray<TSharedPtr<FJsonValue>> FocusedActors;
	TArray<AActor*> FocusedActorPtrs;
	for (const TSharedPtr<FJsonValue>& Value : *ActorIds)
	{
		const FString ActorId = Value.IsValid() ? Value->AsString() : FString();
		if (ActorId.IsEmpty())
		{
			continue;
		}

		AActor* Actor = FindActorByNameOrLabel(World, ActorId);
		if (!Actor)
		{
			OutError = FString::Printf(TEXT("actor_not_found:%s"), *ActorId);
			return false;
		}

		const FBox ActorBox = Actor->GetComponentsBoundingBox(true);
		if (!ActorBox.IsValid)
		{
			OutError = FString::Printf(TEXT("actor_has_invalid_bounds:%s"), *ActorId);
			return false;
		}

		CombinedBox += ActorBox;
		FocusedActors.Add(MakeShared<FJsonValueObject>(UeAgentLevelOps::MakeActorSummaryJson(Actor)));
		FocusedActorPtrs.Add(Actor);
	}

	if (!CombinedBox.IsValid || FocusedActors.Num() <= 0)
	{
		OutError = TEXT("actor_bounds_invalid");
		return false;
	}

	const FVector PreviousLocation = LVC->GetViewLocation();

	FUeAgentInterfaceLogger::Log(TEXT("CmdViewportFrameActors req=%s count=%d instant=%s"), *Ctx.RequestId, FocusedActors.Num(), bInstant ? TEXT("true") : TEXT("false"));
	LVC->FocusViewportOnBox(CombinedBox, bInstant);

	const FVector DesiredLocation = LVC->GetViewLocation();
	FVector NewLocation = DesiredLocation;

	bool bCollisionAware = false;
	JsonTryGetBool(Ctx.Params, TEXT("collision_aware"), bCollisionAware);

	bool bCollisionAdjusted = false;
	bool bFallbackUsed = false;
	int32 FallbackIndex = 0;
	FVector FallbackOffset = FVector::ZeroVector;
	TSharedPtr<FJsonObject> CollisionTrace;
	if (bCollisionAware)
	{
		double SafetyOffsetCm = 15.0;
		JsonTryGetNumber(Ctx.Params, TEXT("safety_offset_cm"), SafetyOffsetCm);
		SafetyOffsetCm = FMath::Clamp(SafetyOffsetCm, 0.0, 10000.0);

		ECollisionChannel TraceChannel = ECC_Visibility;
		if (!UeAgentLevelOps::ResolveTraceChannel(Ctx.Params, TraceChannel))
		{
			OutError = TEXT("unsupported_trace_channel");
			return false;
		}

		bool bTraceComplex = false;
		JsonTryGetBool(Ctx.Params, TEXT("trace_complex"), bTraceComplex);

		FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(UeAgentViewportFrameActorsCollision), bTraceComplex);
		for (AActor* Actor : FocusedActorPtrs)
		{
			if (Actor)
			{
				QueryParams.AddIgnoredActor(Actor);
			}
		}

		bool bAutoFallback = false;
		JsonTryGetBool(Ctx.Params, TEXT("auto_fallback"), bAutoFallback);

		double FallbackStepCm = 200.0;
		JsonTryGetNumber(Ctx.Params, TEXT("fallback_step_cm"), FallbackStepCm);
		FallbackStepCm = FMath::Clamp(FallbackStepCm, 0.0, 10000.0);

		auto TryParseVectorObject = [](const TSharedPtr<FJsonObject>& Obj, FVector& OutVec) -> bool
		{
			if (!Obj.IsValid())
			{
				return false;
			}
			double X = 0.0;
			double Y = 0.0;
			double Z = 0.0;
			if (!UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("x"), TEXT("X") }, X)
				|| !UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("y"), TEXT("Y") }, Y)
				|| !UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("z"), TEXT("Z") }, Z))
			{
				return false;
			}
			OutVec = FVector(X, Y, Z);
			return true;
		};

		TArray<FVector> CandidateOffsets;
		CandidateOffsets.Reserve(8);
		CandidateOffsets.Add(FVector::ZeroVector);

		const TArray<TSharedPtr<FJsonValue>>* ExplicitOffsets = nullptr;
		if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("fallback_offsets_cm"), ExplicitOffsets) && ExplicitOffsets)
		{
			for (const TSharedPtr<FJsonValue>& Value : *ExplicitOffsets)
			{
				if (!Value.IsValid() || Value->Type != EJson::Object)
				{
					continue;
				}

				FVector Offset = FVector::ZeroVector;
				if (TryParseVectorObject(Value->AsObject(), Offset) && !Offset.IsNearlyZero())
				{
					CandidateOffsets.Add(Offset);
				}
			}
		}

		const FVector Target = CombinedBox.GetCenter();
		if (bAutoFallback && CandidateOffsets.Num() <= 1 && FallbackStepCm > KINDA_SMALL_NUMBER)
		{
			FVector BaseDir = (DesiredLocation - Target).GetSafeNormal();
			if (BaseDir.IsNearlyZero(KINDA_SMALL_NUMBER))
			{
				BaseDir = (PreviousLocation - Target).GetSafeNormal();
			}
			if (BaseDir.IsNearlyZero(KINDA_SMALL_NUMBER))
			{
				BaseDir = FVector::BackwardVector;
			}

			const FVector WorldUp = FVector::UpVector;
			FVector Right = FVector::CrossProduct(WorldUp, BaseDir).GetSafeNormal();
			if (Right.IsNearlyZero(KINDA_SMALL_NUMBER))
			{
				Right = FVector::CrossProduct(FVector::RightVector, BaseDir).GetSafeNormal();
			}
			FVector Up = FVector::CrossProduct(BaseDir, Right).GetSafeNormal();
			if (Up.IsNearlyZero(KINDA_SMALL_NUMBER))
			{
				Up = WorldUp;
			}

			CandidateOffsets.Add(Right * (float)FallbackStepCm);
			CandidateOffsets.Add(-Right * (float)FallbackStepCm);
			CandidateOffsets.Add(Up * (float)FallbackStepCm);
			CandidateOffsets.Add(-Up * (float)FallbackStepCm);
			CandidateOffsets.Add(BaseDir * (float)FallbackStepCm);
			CandidateOffsets.Add(-BaseDir * (float)FallbackStepCm);
		}

		struct FCollisionCandidate
		{
			int32 Index = 0;
			FVector Offset = FVector::ZeroVector;
			FVector Desired = FVector::ZeroVector;
			FVector Location = FVector::ZeroVector;
			bool bHit = false;
			bool bAdjusted = false;
			FHitResult Hit;
			double Score = 0.0;
		};

		FCollisionCandidate Best;
		Best.Score = -TNumericLimits<double>::Max();

		for (int32 Index = 0; Index < CandidateOffsets.Num(); ++Index)
		{
			const FVector Offset = CandidateOffsets[Index];
			const FVector CandidateDesired = DesiredLocation + Offset;

			FCollisionCandidate Candidate;
			Candidate.Index = Index;
			Candidate.Offset = Offset;
			Candidate.Desired = CandidateDesired;

			const bool bHit = World->LineTraceSingleByChannel(Candidate.Hit, Target, CandidateDesired, TraceChannel, QueryParams);
			Candidate.bHit = bHit;

			if (bHit && Candidate.Hit.bBlockingHit)
			{
				FVector Dir = (CandidateDesired - Target).GetSafeNormal();
				if (Dir.IsNearlyZero(KINDA_SMALL_NUMBER))
				{
					Dir = (DesiredLocation - Target).GetSafeNormal();
				}
				if (Dir.IsNearlyZero(KINDA_SMALL_NUMBER))
				{
					Dir = FVector::BackwardVector;
				}

				Candidate.Location = Candidate.Hit.ImpactPoint - (Dir * (float)SafetyOffsetCm);
				Candidate.bAdjusted = true;
			}
			else
			{
				Candidate.Location = CandidateDesired;
			}

			const double Distance = FVector::Dist(Target, Candidate.Location);
			Candidate.Score = Distance - (Candidate.bAdjusted ? 0.01 : 0.0);
			if (Candidate.Score > Best.Score)
			{
				Best = Candidate;
			}
		}

		NewLocation = Best.Location;
		bCollisionAdjusted = Best.bAdjusted;
		bFallbackUsed = (Best.Index != 0);
		FallbackIndex = Best.Index;
		FallbackOffset = Best.Offset;

		CollisionTrace = MakeShared<FJsonObject>();
		CollisionTrace->SetObjectField(TEXT("target"), VecToJson(Target));
		CollisionTrace->SetObjectField(TEXT("desired_location"), VecToJson(DesiredLocation));
		CollisionTrace->SetBoolField(TEXT("auto_fallback"), bAutoFallback);
		CollisionTrace->SetNumberField(TEXT("fallback_step_cm"), FallbackStepCm);
		CollisionTrace->SetNumberField(TEXT("candidate_count"), CandidateOffsets.Num());
		CollisionTrace->SetBoolField(TEXT("fallback_used"), bFallbackUsed);
		CollisionTrace->SetNumberField(TEXT("fallback_index"), FallbackIndex);
		CollisionTrace->SetObjectField(TEXT("fallback_offset_cm"), VecToJson(FallbackOffset));
		CollisionTrace->SetObjectField(TEXT("candidate_desired_location"), VecToJson(Best.Desired));
		CollisionTrace->SetStringField(TEXT("trace_channel"), FString::Printf(TEXT("%d"), (int32)TraceChannel));
		CollisionTrace->SetBoolField(TEXT("trace_complex"), bTraceComplex);
		CollisionTrace->SetNumberField(TEXT("safety_offset_cm"), SafetyOffsetCm);
		CollisionTrace->SetBoolField(TEXT("hit"), Best.bHit);
		UeAgentLevelOps::FillHitResultJson(Best.Hit, CollisionTrace);
		CollisionTrace->SetBoolField(TEXT("adjusted"), Best.bAdjusted);
		CollisionTrace->SetObjectField(TEXT("new_location"), VecToJson(NewLocation));
		if (Best.bAdjusted)
		{
			CollisionTrace->SetStringField(TEXT("adjustment_mode"), TEXT("hit_point_minus_trace_dir"));
		}
	}

	if (!NewLocation.Equals(DesiredLocation, 0.01))
	{
		LVC->SetViewLocation(NewLocation);
	}

	LVC->Invalidate();

	OutData->SetNumberField(TEXT("focused_count"), FocusedActors.Num());
	OutData->SetBoolField(TEXT("instant"), bInstant);
	OutData->SetArrayField(TEXT("actors"), FocusedActors);
	OutData->SetObjectField(TEXT("bounds"), UeAgentLevelOps::MakeBoundsJson(CombinedBox));
	OutData->SetObjectField(TEXT("previous_location"), VecToJson(PreviousLocation));
	OutData->SetObjectField(TEXT("desired_location"), VecToJson(DesiredLocation));
	OutData->SetObjectField(TEXT("new_location"), VecToJson(LVC->GetViewLocation()));
	OutData->SetBoolField(TEXT("collision_aware"), bCollisionAware);
	OutData->SetBoolField(TEXT("collision_adjusted"), bCollisionAdjusted);
	OutData->SetBoolField(TEXT("fallback_used"), bFallbackUsed);
	OutData->SetNumberField(TEXT("fallback_index"), FallbackIndex);
	OutData->SetObjectField(TEXT("fallback_offset_cm"), VecToJson(FallbackOffset));
	if (CollisionTrace.IsValid())
	{
		OutData->SetObjectField(TEXT("collision_trace"), CollisionTrace);
	}
	return true;
}

bool FUeAgentHttpServer::CmdViewportFrameFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FLevelEditorViewportClient* LVC = UeAgentLevelOps::GetPreferredLevelViewportClient();
	if (!LVC)
	{
		OutError = TEXT("no_level_editor_viewport");
		return false;
	}

	FString FolderPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath) || FolderPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_folder_path");
		return false;
	}

	bool bIncludeChildFolders = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_child_folders"), bIncludeChildFolders);

	double Padding = 1.1;
	JsonTryGetNumber(Ctx.Params, TEXT("padding"), Padding);

	FBox CombinedBounds(EForceInit::ForceInit);
	TArray<AActor*> FolderActors;
	if (!UeAgentLevelOps::GetFolderActorBounds(World, FolderPath, bIncludeChildFolders, CombinedBounds, FolderActors, OutError))
	{
		return false;
	}

	const FVector PreviousLocation = LVC->GetViewLocation();
	const FRotator PreviousRotation = LVC->GetViewRotation();
	FVector DesiredLocation = PreviousLocation;
	FVector NewLocation = PreviousLocation;
	if (!UeAgentLevelOps::ComputeViewportLocationToFrameBounds(LVC, CombinedBounds, PreviousRotation, Padding, NewLocation, OutError))
	{
		return false;
	}

	DesiredLocation = NewLocation;

	bool bCollisionAware = false;
	JsonTryGetBool(Ctx.Params, TEXT("collision_aware"), bCollisionAware);

	bool bLookAt = false;
	JsonTryGetBool(Ctx.Params, TEXT("look_at"), bLookAt);

	bool bCollisionAdjusted = false;
	bool bFallbackUsed = false;
	int32 FallbackIndex = 0;
	FVector FallbackOffset = FVector::ZeroVector;
	TSharedPtr<FJsonObject> CollisionTrace;
	if (bCollisionAware)
	{
		double SafetyOffsetCm = 15.0;
		JsonTryGetNumber(Ctx.Params, TEXT("safety_offset_cm"), SafetyOffsetCm);
		SafetyOffsetCm = FMath::Clamp(SafetyOffsetCm, 0.0, 10000.0);

		ECollisionChannel TraceChannel = ECC_Visibility;
		if (!UeAgentLevelOps::ResolveTraceChannel(Ctx.Params, TraceChannel))
		{
			OutError = TEXT("unsupported_trace_channel");
			return false;
		}

		bool bTraceComplex = false;
		JsonTryGetBool(Ctx.Params, TEXT("trace_complex"), bTraceComplex);

		FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(UeAgentViewportFrameFolderCollision), bTraceComplex);
		for (AActor* Actor : FolderActors)
		{
			if (Actor)
			{
				QueryParams.AddIgnoredActor(Actor);
			}
		}

		bool bAutoFallback = false;
		JsonTryGetBool(Ctx.Params, TEXT("auto_fallback"), bAutoFallback);

		double FallbackStepCm = 200.0;
		JsonTryGetNumber(Ctx.Params, TEXT("fallback_step_cm"), FallbackStepCm);
		FallbackStepCm = FMath::Clamp(FallbackStepCm, 0.0, 10000.0);

		auto TryParseVectorObject = [](const TSharedPtr<FJsonObject>& Obj, FVector& OutVec) -> bool
		{
			if (!Obj.IsValid())
			{
				return false;
			}
			double X = 0.0;
			double Y = 0.0;
			double Z = 0.0;
			if (!UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("x"), TEXT("X") }, X)
				|| !UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("y"), TEXT("Y") }, Y)
				|| !UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("z"), TEXT("Z") }, Z))
			{
				return false;
			}
			OutVec = FVector(X, Y, Z);
			return true;
		};

		TArray<FVector> CandidateOffsets;
		CandidateOffsets.Reserve(8);
		CandidateOffsets.Add(FVector::ZeroVector);

		const TArray<TSharedPtr<FJsonValue>>* ExplicitOffsets = nullptr;
		if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("fallback_offsets_cm"), ExplicitOffsets) && ExplicitOffsets)
		{
			for (const TSharedPtr<FJsonValue>& Value : *ExplicitOffsets)
			{
				if (!Value.IsValid() || Value->Type != EJson::Object)
				{
					continue;
				}

				FVector Offset = FVector::ZeroVector;
				if (TryParseVectorObject(Value->AsObject(), Offset) && !Offset.IsNearlyZero())
				{
					CandidateOffsets.Add(Offset);
				}
			}
		}

		const FVector Target = CombinedBounds.GetCenter();
		if (bAutoFallback && CandidateOffsets.Num() <= 1 && FallbackStepCm > KINDA_SMALL_NUMBER)
		{
			FVector BaseDir = (DesiredLocation - Target).GetSafeNormal();
			if (BaseDir.IsNearlyZero(KINDA_SMALL_NUMBER))
			{
				BaseDir = (PreviousLocation - Target).GetSafeNormal();
			}
			if (BaseDir.IsNearlyZero(KINDA_SMALL_NUMBER))
			{
				BaseDir = FVector::BackwardVector;
			}

			const FVector WorldUp = FVector::UpVector;
			FVector Right = FVector::CrossProduct(WorldUp, BaseDir).GetSafeNormal();
			if (Right.IsNearlyZero(KINDA_SMALL_NUMBER))
			{
				Right = FVector::CrossProduct(FVector::RightVector, BaseDir).GetSafeNormal();
			}
			FVector Up = FVector::CrossProduct(BaseDir, Right).GetSafeNormal();
			if (Up.IsNearlyZero(KINDA_SMALL_NUMBER))
			{
				Up = WorldUp;
			}

			CandidateOffsets.Add(Right * (float)FallbackStepCm);
			CandidateOffsets.Add(-Right * (float)FallbackStepCm);
			CandidateOffsets.Add(Up * (float)FallbackStepCm);
			CandidateOffsets.Add(-Up * (float)FallbackStepCm);
			CandidateOffsets.Add(BaseDir * (float)FallbackStepCm);
			CandidateOffsets.Add(-BaseDir * (float)FallbackStepCm);
		}

		struct FCollisionCandidate
		{
			int32 Index = 0;
			FVector Offset = FVector::ZeroVector;
			FVector Desired = FVector::ZeroVector;
			FVector Location = FVector::ZeroVector;
			bool bHit = false;
			bool bAdjusted = false;
			FHitResult Hit;
			double Score = 0.0;
		};

		FCollisionCandidate Best;
		Best.Score = -TNumericLimits<double>::Max();

		for (int32 Index = 0; Index < CandidateOffsets.Num(); ++Index)
		{
			const FVector Offset = CandidateOffsets[Index];
			const FVector CandidateDesired = DesiredLocation + Offset;

			FCollisionCandidate Candidate;
			Candidate.Index = Index;
			Candidate.Offset = Offset;
			Candidate.Desired = CandidateDesired;

			const bool bHit = World->LineTraceSingleByChannel(Candidate.Hit, Target, CandidateDesired, TraceChannel, QueryParams);
			Candidate.bHit = bHit;

			if (bHit && Candidate.Hit.bBlockingHit)
			{
				FVector Dir = (CandidateDesired - Target).GetSafeNormal();
				if (Dir.IsNearlyZero(KINDA_SMALL_NUMBER))
				{
					Dir = (DesiredLocation - Target).GetSafeNormal();
				}
				if (Dir.IsNearlyZero(KINDA_SMALL_NUMBER))
				{
					Dir = FVector::BackwardVector;
				}

				Candidate.Location = Candidate.Hit.ImpactPoint - (Dir * (float)SafetyOffsetCm);
				Candidate.bAdjusted = true;
			}
			else
			{
				Candidate.Location = CandidateDesired;
			}

			const double Distance = FVector::Dist(Target, Candidate.Location);
			Candidate.Score = Distance - (Candidate.bAdjusted ? 0.01 : 0.0);
			if (Candidate.Score > Best.Score)
			{
				Best = Candidate;
			}
		}

		NewLocation = Best.Location;
		bCollisionAdjusted = Best.bAdjusted;
		bFallbackUsed = (Best.Index != 0);
		FallbackIndex = Best.Index;
		FallbackOffset = Best.Offset;

		CollisionTrace = MakeShared<FJsonObject>();
		CollisionTrace->SetObjectField(TEXT("target"), VecToJson(Target));
		CollisionTrace->SetObjectField(TEXT("desired_location"), VecToJson(DesiredLocation));
		CollisionTrace->SetBoolField(TEXT("auto_fallback"), bAutoFallback);
		CollisionTrace->SetNumberField(TEXT("fallback_step_cm"), FallbackStepCm);
		CollisionTrace->SetNumberField(TEXT("candidate_count"), CandidateOffsets.Num());
		CollisionTrace->SetBoolField(TEXT("fallback_used"), bFallbackUsed);
		CollisionTrace->SetNumberField(TEXT("fallback_index"), FallbackIndex);
		CollisionTrace->SetObjectField(TEXT("fallback_offset_cm"), VecToJson(FallbackOffset));
		CollisionTrace->SetObjectField(TEXT("candidate_desired_location"), VecToJson(Best.Desired));
		CollisionTrace->SetStringField(TEXT("trace_channel"), FString::Printf(TEXT("%d"), (int32)TraceChannel));
		CollisionTrace->SetBoolField(TEXT("trace_complex"), bTraceComplex);
		CollisionTrace->SetNumberField(TEXT("safety_offset_cm"), SafetyOffsetCm);
		CollisionTrace->SetBoolField(TEXT("hit"), Best.bHit);
		UeAgentLevelOps::FillHitResultJson(Best.Hit, CollisionTrace);
		CollisionTrace->SetBoolField(TEXT("adjusted"), Best.bAdjusted);
		CollisionTrace->SetObjectField(TEXT("new_location"), VecToJson(NewLocation));
		if (Best.bAdjusted)
		{
			CollisionTrace->SetStringField(TEXT("adjustment_mode"), TEXT("hit_point_minus_trace_dir"));
		}
	}

	LVC->SetViewLocation(NewLocation);
	if (bLookAt)
	{
		const FVector Target = CombinedBounds.GetCenter();
		const FVector Dir = (Target - NewLocation);
		if (!Dir.IsNearlyZero(KINDA_SMALL_NUMBER))
		{
			FRotator LookAtRot = Dir.Rotation();
			LookAtRot.Roll = 0.0f;
			LVC->SetViewRotation(LookAtRot);
		}
	}
	LVC->Invalidate();

	TArray<TSharedPtr<FJsonValue>> FocusedActors;
	FocusedActors.Reserve(FolderActors.Num());
	for (AActor* Actor : FolderActors)
	{
		FocusedActors.Add(MakeShared<FJsonValueObject>(UeAgentLevelOps::MakeActorSummaryJson(Actor)));
	}

	FUeAgentInterfaceLogger::Log(
		TEXT("CmdViewportFrameFolder req=%s folder=%s include_child_folders=%s actor_count=%d padding=%.3f"),
		*Ctx.RequestId,
		*FolderPath,
		bIncludeChildFolders ? TEXT("true") : TEXT("false"),
		FolderActors.Num(),
		Padding);

	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetBoolField(TEXT("include_child_folders"), bIncludeChildFolders);
	OutData->SetNumberField(TEXT("focused_count"), FocusedActors.Num());
	OutData->SetNumberField(TEXT("padding"), Padding);
	OutData->SetArrayField(TEXT("actors"), FocusedActors);
	OutData->SetObjectField(TEXT("bounds"), UeAgentLevelOps::MakeBoundsJson(CombinedBounds));
	OutData->SetObjectField(TEXT("previous_location"), VecToJson(PreviousLocation));
	OutData->SetObjectField(TEXT("desired_location"), VecToJson(DesiredLocation));
	OutData->SetObjectField(TEXT("new_location"), VecToJson(LVC->GetViewLocation()));
	OutData->SetBoolField(TEXT("collision_aware"), bCollisionAware);
	OutData->SetBoolField(TEXT("collision_adjusted"), bCollisionAdjusted);
	OutData->SetBoolField(TEXT("fallback_used"), bFallbackUsed);
	OutData->SetNumberField(TEXT("fallback_index"), FallbackIndex);
	OutData->SetObjectField(TEXT("fallback_offset_cm"), VecToJson(FallbackOffset));
	OutData->SetBoolField(TEXT("look_at"), bLookAt);
	if (CollisionTrace.IsValid())
	{
		OutData->SetObjectField(TEXT("collision_trace"), CollisionTrace);
	}
	OutData->SetObjectField(TEXT("rotation"), RotToJson(LVC->GetViewRotation()));
	OutData->SetNumberField(TEXT("fov"), LVC->ViewFOV);
	OutData->SetBoolField(TEXT("realtime"), LVC->IsRealtime());
	OutData->SetBoolField(TEXT("game_view"), LVC->IsInGameView());
	return true;
}

bool FUeAgentHttpServer::CmdViewportDeprojectScreenToWorld(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FLevelEditorViewportClient* ViewportClient = UeAgentLevelOps::GetPreferredLevelViewportClient();
	if (!ViewportClient || !ViewportClient->Viewport)
	{
		OutError = TEXT("no_level_editor_viewport");
		return false;
	}

	FVector2D ScreenPoint(0.0f, 0.0f);
	if (!UeAgentLevelOps::ResolveViewportScreenPoint(ViewportClient, Ctx.Params, ScreenPoint, OutError))
	{
		return false;
	}

	FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
		ViewportClient->Viewport,
		ViewportClient->GetWorld()->Scene,
		ViewportClient->EngineShowFlags).SetRealtimeUpdate(ViewportClient->IsRealtime()));
	FSceneView* SceneView = ViewportClient->CalcSceneView(&ViewFamily);
	if (!SceneView)
	{
		OutError = TEXT("calc_scene_view_failed");
		return false;
	}

	FVector WorldOrigin = FVector::ZeroVector;
	FVector WorldDirection = FVector::ForwardVector;
	SceneView->DeprojectFVector2D(ScreenPoint, WorldOrigin, WorldDirection);

	OutData->SetObjectField(TEXT("screen_point"), VecToJson(FVector(ScreenPoint.X, ScreenPoint.Y, 0.0)));
	OutData->SetObjectField(TEXT("world_origin"), VecToJson(WorldOrigin));
	OutData->SetObjectField(TEXT("world_direction"), VecToJson(WorldDirection));
	return true;
}

bool FUeAgentHttpServer::CmdViewportTraceScreenPoint(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
	if (!UeAgentLevelOps::ResolveViewportScreenPoint(ViewportClient, Ctx.Params, ScreenPoint, OutError))
	{
		return false;
	}

	FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
		ViewportClient->Viewport,
		World->Scene,
		ViewportClient->EngineShowFlags).SetRealtimeUpdate(ViewportClient->IsRealtime()));
	FSceneView* SceneView = ViewportClient->CalcSceneView(&ViewFamily);
	if (!SceneView)
	{
		OutError = TEXT("calc_scene_view_failed");
		return false;
	}

	FVector WorldOrigin = FVector::ZeroVector;
	FVector WorldDirection = FVector::ForwardVector;
	SceneView->DeprojectFVector2D(ScreenPoint, WorldOrigin, WorldDirection);

	double TraceDistance = HALF_WORLD_MAX;
	JsonTryGetNumber(Ctx.Params, TEXT("trace_distance"), TraceDistance);
	TraceDistance = FMath::Clamp(TraceDistance, 1.0, (double)HALF_WORLD_MAX);

	ECollisionChannel TraceChannel = ECC_Visibility;
	if (!UeAgentLevelOps::ResolveTraceChannel(Ctx.Params, TraceChannel))
	{
		OutError = TEXT("unsupported_trace_channel");
		return false;
	}

	bool bTraceComplex = true;
	JsonTryGetBool(Ctx.Params, TEXT("trace_complex"), bTraceComplex);

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(UeAgentTraceScreenPoint), bTraceComplex);
	FHitResult Hit;
	const FVector TraceEnd = WorldOrigin + (WorldDirection * TraceDistance);
	const bool bHit = World->LineTraceSingleByChannel(Hit, WorldOrigin, TraceEnd, TraceChannel, QueryParams);

	OutData->SetObjectField(TEXT("screen_point"), VecToJson(FVector(ScreenPoint.X, ScreenPoint.Y, 0.0)));
	OutData->SetObjectField(TEXT("world_origin"), VecToJson(WorldOrigin));
	OutData->SetObjectField(TEXT("world_direction"), VecToJson(WorldDirection));
	OutData->SetNumberField(TEXT("trace_distance"), TraceDistance);
	OutData->SetStringField(TEXT("trace_channel"), FString::Printf(TEXT("%d"), (int32)TraceChannel));
	OutData->SetBoolField(TEXT("trace_complex"), bTraceComplex);
	OutData->SetBoolField(TEXT("hit"), bHit);
	if (bHit)
	{
		UeAgentLevelOps::FillHitResultJson(Hit, OutData);
	}
	return true;
}

bool FUeAgentHttpServer::CmdLevelTraceWorldRay(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* RaysJson = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("rays"), RaysJson) && RaysJson && RaysJson->Num() > 0)
	{
		bool bContinueOnError = false;
		JsonTryGetBool(Ctx.Params, TEXT("continue_on_error"), bContinueOnError);

		double DefaultTraceDistance = HALF_WORLD_MAX;
		if (!JsonTryGetNumber(Ctx.Params, TEXT("trace_distance"), DefaultTraceDistance))
		{
			JsonTryGetNumber(Ctx.Params, TEXT("distance"), DefaultTraceDistance);
		}
		DefaultTraceDistance = FMath::Clamp(DefaultTraceDistance, 1.0, (double)HALF_WORLD_MAX);

		ECollisionChannel DefaultTraceChannel = ECC_Visibility;
		FString DefaultChannelName;
		if (Ctx.Params->TryGetStringField(TEXT("trace_channel"), DefaultChannelName) && !DefaultChannelName.TrimStartAndEnd().IsEmpty())
		{
			if (!UeAgentLevelOps::ResolveTraceChannel(Ctx.Params, DefaultTraceChannel))
			{
				OutError = TEXT("unsupported_trace_channel");
				return false;
			}
		}

		bool bDefaultTraceComplex = true;
		JsonTryGetBool(Ctx.Params, TEXT("trace_complex"), bDefaultTraceComplex);

		bool bDefaultIncludeAllHits = false;
		JsonTryGetBool(Ctx.Params, TEXT("include_all_hits"), bDefaultIncludeAllHits);

		int32 DefaultMaxHits = 32;
		{
			double MaxHitsNumber = (double)DefaultMaxHits;
			JsonTryGetNumber(Ctx.Params, TEXT("max_hits"), MaxHitsNumber);
			DefaultMaxHits = (int32)FMath::Clamp(MaxHitsNumber, 1.0, 2000.0);
		}

		bool bDefaultIncludeActorFolderTags = false;
		JsonTryGetBool(Ctx.Params, TEXT("include_actor_folder_tags"), bDefaultIncludeActorFolderTags);

		int32 MaxItems = 4096;
		{
			double MaxItemsNumber = (double)MaxItems;
			JsonTryGetNumber(Ctx.Params, TEXT("max_items"), MaxItemsNumber);
			MaxItems = (int32)FMath::Clamp(MaxItemsNumber, 1.0, 20000.0);
		}

		TArray<AActor*> GlobalIgnoredActors;
		const TArray<TSharedPtr<FJsonValue>>* GlobalIgnoreIds = nullptr;
		if (Ctx.Params->TryGetArrayField(TEXT("ignore_actor_ids"), GlobalIgnoreIds) && GlobalIgnoreIds)
		{
			for (const TSharedPtr<FJsonValue>& IgnoreValue : *GlobalIgnoreIds)
			{
				const FString IgnoreId = IgnoreValue.IsValid() ? IgnoreValue->AsString() : FString();
				if (IgnoreId.IsEmpty())
				{
					continue;
				}

				if (AActor* IgnoreActor = FindActorByNameOrLabel(World, IgnoreId))
				{
					GlobalIgnoredActors.Add(IgnoreActor);
				}
			}
		}
		UeAgentLevelOps::AppendIgnoredActorsFromFilters(World, Ctx.Params, GlobalIgnoredActors);

		const int32 InputCount = RaysJson->Num();
		const int32 ExecuteCount = FMath::Min(InputCount, MaxItems);
		const bool bTruncated = InputCount > ExecuteCount;

		int32 ErrorCount = 0;
		int32 HitCount = 0;
		int32 FirstErrorIndex = INDEX_NONE;
		FString FirstError;

		TArray<TSharedPtr<FJsonValue>> Results;
		Results.Reserve(ExecuteCount);

		for (int32 Index = 0; Index < ExecuteCount; ++Index)
		{
			const TSharedPtr<FJsonValue>& RayValue = (*RaysJson)[Index];
			const TSharedPtr<FJsonObject> RayObj = RayValue.IsValid() ? RayValue->AsObject() : nullptr;
			if (!RayObj.IsValid())
			{
				++ErrorCount;
				if (FirstErrorIndex == INDEX_NONE)
				{
					FirstErrorIndex = Index;
					FirstError = TEXT("rays_invalid_item");
				}
				if (!bContinueOnError)
				{
					OutError = TEXT("rays_invalid_item");
					return false;
				}

				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("index"), Index);
				Item->SetBoolField(TEXT("ok"), false);
				Item->SetStringField(TEXT("error"), TEXT("rays_invalid_item"));
				Results.Add(MakeShared<FJsonValueObject>(Item));
				continue;
			}

			FVector Start = FVector::ZeroVector;
			if (!JsonTryGetVector(RayObj, TEXT("start"), Start))
			{
				++ErrorCount;
				if (FirstErrorIndex == INDEX_NONE)
				{
					FirstErrorIndex = Index;
					FirstError = TEXT("missing_start");
				}
				if (!bContinueOnError)
				{
					OutError = TEXT("missing_start");
					return false;
				}

				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("index"), Index);
				Item->SetBoolField(TEXT("ok"), false);
				Item->SetStringField(TEXT("error"), TEXT("missing_start"));
				Results.Add(MakeShared<FJsonValueObject>(Item));
				continue;
			}

			FVector Direction = FVector::ForwardVector;
			if (!JsonTryGetVector(RayObj, TEXT("direction"), Direction))
			{
				++ErrorCount;
				if (FirstErrorIndex == INDEX_NONE)
				{
					FirstErrorIndex = Index;
					FirstError = TEXT("missing_direction");
				}
				if (!bContinueOnError)
				{
					OutError = TEXT("missing_direction");
					return false;
				}

				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("index"), Index);
				Item->SetBoolField(TEXT("ok"), false);
				Item->SetStringField(TEXT("error"), TEXT("missing_direction"));
				Item->SetObjectField(TEXT("start"), VecToJson(Start));
				Results.Add(MakeShared<FJsonValueObject>(Item));
				continue;
			}

			const FVector DirectionNormalized = Direction.GetSafeNormal();
			if (DirectionNormalized.IsNearlyZero())
			{
				++ErrorCount;
				if (FirstErrorIndex == INDEX_NONE)
				{
					FirstErrorIndex = Index;
					FirstError = TEXT("invalid_direction");
				}
				if (!bContinueOnError)
				{
					OutError = TEXT("invalid_direction");
					return false;
				}

				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("index"), Index);
				Item->SetBoolField(TEXT("ok"), false);
				Item->SetStringField(TEXT("error"), TEXT("invalid_direction"));
				Item->SetObjectField(TEXT("start"), VecToJson(Start));
				Item->SetObjectField(TEXT("direction"), VecToJson(Direction));
				Results.Add(MakeShared<FJsonValueObject>(Item));
				continue;
			}

			double TraceDistance = DefaultTraceDistance;
			if (!JsonTryGetNumber(RayObj, TEXT("trace_distance"), TraceDistance))
			{
				JsonTryGetNumber(RayObj, TEXT("distance"), TraceDistance);
			}
			TraceDistance = FMath::Clamp(TraceDistance, 1.0, (double)HALF_WORLD_MAX);

			ECollisionChannel TraceChannel = DefaultTraceChannel;
			FString ChannelName;
			if (RayObj->TryGetStringField(TEXT("trace_channel"), ChannelName) && !ChannelName.TrimStartAndEnd().IsEmpty())
			{
				if (!UeAgentLevelOps::ResolveTraceChannel(RayObj, TraceChannel))
				{
					++ErrorCount;
					if (FirstErrorIndex == INDEX_NONE)
					{
						FirstErrorIndex = Index;
						FirstError = TEXT("unsupported_trace_channel");
					}
					if (!bContinueOnError)
					{
						OutError = TEXT("unsupported_trace_channel");
						return false;
					}

					TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetNumberField(TEXT("index"), Index);
					Item->SetBoolField(TEXT("ok"), false);
					Item->SetStringField(TEXT("error"), TEXT("unsupported_trace_channel"));
					Item->SetObjectField(TEXT("start"), VecToJson(Start));
					Item->SetObjectField(TEXT("direction"), VecToJson(DirectionNormalized));
					Results.Add(MakeShared<FJsonValueObject>(Item));
					continue;
				}
			}

			bool bTraceComplex = bDefaultTraceComplex;
			RayObj->TryGetBoolField(TEXT("trace_complex"), bTraceComplex);

			bool bIncludeAllHits = bDefaultIncludeAllHits;
			RayObj->TryGetBoolField(TEXT("include_all_hits"), bIncludeAllHits);

			int32 MaxHits = DefaultMaxHits;
			{
				double MaxHitsNumber = (double)MaxHits;
				JsonTryGetNumber(RayObj, TEXT("max_hits"), MaxHitsNumber);
				MaxHits = (int32)FMath::Clamp(MaxHitsNumber, 1.0, 2000.0);
			}

			bool bIncludeActorFolderTags = bDefaultIncludeActorFolderTags;
			RayObj->TryGetBoolField(TEXT("include_actor_folder_tags"), bIncludeActorFolderTags);

			FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(UeAgentTraceWorldRayBatch), bTraceComplex);
			for (AActor* Ignored : GlobalIgnoredActors)
			{
				if (Ignored)
				{
					QueryParams.AddIgnoredActor(Ignored);
				}
			}

			const TArray<TSharedPtr<FJsonValue>>* IgnoreIds = nullptr;
			if (RayObj->TryGetArrayField(TEXT("ignore_actor_ids"), IgnoreIds) && IgnoreIds)
			{
				for (const TSharedPtr<FJsonValue>& IgnoreValue : *IgnoreIds)
				{
					const FString IgnoreId = IgnoreValue.IsValid() ? IgnoreValue->AsString() : FString();
					if (IgnoreId.IsEmpty())
					{
						continue;
					}

					if (AActor* IgnoreActor = FindActorByNameOrLabel(World, IgnoreId))
					{
						QueryParams.AddIgnoredActor(IgnoreActor);
					}
				}
			}

			const FVector TraceEnd = Start + (DirectionNormalized * TraceDistance);

			FHitResult PrimaryHit;
			TArray<FHitResult> Hits;
			bool bBlockingHit = false;
			if (bIncludeAllHits)
			{
				World->LineTraceMultiByChannel(Hits, Start, TraceEnd, TraceChannel, QueryParams);
				Hits.Sort([](const FHitResult& A, const FHitResult& B) { return A.Distance < B.Distance; });
				for (const FHitResult& H : Hits)
				{
					if (H.bBlockingHit)
					{
						PrimaryHit = H;
						bBlockingHit = true;
						break;
					}
				}
			}
			else
			{
				bBlockingHit = World->LineTraceSingleByChannel(PrimaryHit, Start, TraceEnd, TraceChannel, QueryParams);
			}
			if (bBlockingHit)
			{
				++HitCount;
			}

			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetNumberField(TEXT("index"), Index);
			Item->SetBoolField(TEXT("ok"), true);
			Item->SetObjectField(TEXT("start"), VecToJson(Start));
			Item->SetObjectField(TEXT("direction"), VecToJson(DirectionNormalized));
			Item->SetNumberField(TEXT("trace_distance"), TraceDistance);
			Item->SetStringField(TEXT("trace_channel"), FString::Printf(TEXT("%d"), (int32)TraceChannel));
			Item->SetBoolField(TEXT("trace_complex"), bTraceComplex);
			Item->SetBoolField(TEXT("include_all_hits"), bIncludeAllHits);
			Item->SetNumberField(TEXT("max_hits"), MaxHits);
			Item->SetBoolField(TEXT("include_actor_folder_tags"), bIncludeActorFolderTags);
			Item->SetBoolField(TEXT("hit"), bBlockingHit);
			if (bBlockingHit)
			{
				if (bIncludeActorFolderTags)
				{
					if (AActor* HitActor = PrimaryHit.GetActor())
					{
						Item->SetStringField(TEXT("hit_actor_folder_path"), HitActor->GetFolderPath().ToString());

						TArray<TSharedPtr<FJsonValue>> TagValues;
						TagValues.Reserve(HitActor->Tags.Num());
						for (const FName& Tag : HitActor->Tags)
						{
							TagValues.Add(MakeShared<FJsonValueString>(Tag.ToString()));
						}
						Item->SetArrayField(TEXT("hit_actor_tags"), TagValues);
					}
				}
				UeAgentLevelOps::FillHitResultJson(PrimaryHit, Item);
			}
			if (bIncludeAllHits)
			{
				Item->SetNumberField(TEXT("hit_count"), Hits.Num());
				Item->SetBoolField(TEXT("hits_truncated"), Hits.Num() > MaxHits);

				TArray<TSharedPtr<FJsonValue>> HitItems;
				HitItems.Reserve(FMath::Min(MaxHits, Hits.Num()));
				for (int32 HitIndex = 0; HitIndex < Hits.Num() && HitItems.Num() < MaxHits; ++HitIndex)
				{
					const FHitResult& H = Hits[HitIndex];
					TSharedPtr<FJsonObject> HitObj = UeAgentLevelOps::MakeHitResultSummaryJson(H, bIncludeActorFolderTags, false);
					HitObj->SetNumberField(TEXT("hit_index"), HitIndex);
					HitItems.Add(MakeShared<FJsonValueObject>(HitObj));
				}
				Item->SetArrayField(TEXT("hits"), HitItems);
			}
			Results.Add(MakeShared<FJsonValueObject>(Item));
		}

		OutData->SetStringField(TEXT("mode"), TEXT("batch"));
		OutData->SetNumberField(TEXT("input_ray_count"), InputCount);
		OutData->SetNumberField(TEXT("ray_count"), Results.Num());
		OutData->SetNumberField(TEXT("max_items"), MaxItems);
		OutData->SetBoolField(TEXT("truncated"), bTruncated);
		OutData->SetBoolField(TEXT("continue_on_error"), bContinueOnError);
		OutData->SetNumberField(TEXT("hit_count"), HitCount);
		OutData->SetNumberField(TEXT("error_count"), ErrorCount);
		OutData->SetNumberField(TEXT("first_error_index"), FirstErrorIndex);
		if (FirstErrorIndex != INDEX_NONE)
		{
			OutData->SetStringField(TEXT("first_error"), FirstError);
		}
		OutData->SetArrayField(TEXT("rays"), Results);
		return true;
	}

	FVector Start = FVector::ZeroVector;
	FVector Direction = FVector::ForwardVector;
	if (!JsonTryGetVector(Ctx.Params, TEXT("start"), Start))
	{
		OutError = TEXT("missing_start");
		return false;
	}
	if (!JsonTryGetVector(Ctx.Params, TEXT("direction"), Direction))
	{
		OutError = TEXT("missing_direction");
		return false;
	}

	const FVector DirectionNormalized = Direction.GetSafeNormal();
	if (DirectionNormalized.IsNearlyZero())
	{
		OutError = TEXT("invalid_direction");
		return false;
	}

	double TraceDistance = HALF_WORLD_MAX;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("trace_distance"), TraceDistance))
	{
		JsonTryGetNumber(Ctx.Params, TEXT("distance"), TraceDistance);
	}
	TraceDistance = FMath::Clamp(TraceDistance, 1.0, (double)HALF_WORLD_MAX);

	ECollisionChannel TraceChannel = ECC_Visibility;
	if (!UeAgentLevelOps::ResolveTraceChannel(Ctx.Params, TraceChannel))
	{
		OutError = TEXT("unsupported_trace_channel");
		return false;
	}

	bool bTraceComplex = true;
	JsonTryGetBool(Ctx.Params, TEXT("trace_complex"), bTraceComplex);

	bool bIncludeAllHits = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_all_hits"), bIncludeAllHits);

	int32 MaxHits = 32;
	{
		double MaxHitsNumber = (double)MaxHits;
		JsonTryGetNumber(Ctx.Params, TEXT("max_hits"), MaxHitsNumber);
		MaxHits = (int32)FMath::Clamp(MaxHitsNumber, 1.0, 2000.0);
	}

	bool bIncludeActorFolderTags = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_actor_folder_tags"), bIncludeActorFolderTags);

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(UeAgentTraceWorldRay), bTraceComplex);
	TArray<AActor*> IgnoredActors;
	const TArray<TSharedPtr<FJsonValue>>* IgnoreIds = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("ignore_actor_ids"), IgnoreIds) && IgnoreIds)
	{
		for (const TSharedPtr<FJsonValue>& IgnoreValue : *IgnoreIds)
		{
			const FString IgnoreId = IgnoreValue.IsValid() ? IgnoreValue->AsString() : FString();
			if (IgnoreId.IsEmpty())
			{
				continue;
			}

			if (AActor* IgnoreActor = FindActorByNameOrLabel(World, IgnoreId))
			{
				IgnoredActors.Add(IgnoreActor);
			}
		}
	}
	UeAgentLevelOps::AppendIgnoredActorsFromFilters(World, Ctx.Params, IgnoredActors);
	for (AActor* Ignored : IgnoredActors)
	{
		if (Ignored)
		{
			QueryParams.AddIgnoredActor(Ignored);
		}
	}

	const FVector TraceEnd = Start + (DirectionNormalized * TraceDistance);

	FHitResult PrimaryHit;
	TArray<FHitResult> Hits;
	bool bBlockingHit = false;
	if (bIncludeAllHits)
	{
		World->LineTraceMultiByChannel(Hits, Start, TraceEnd, TraceChannel, QueryParams);
		Hits.Sort([](const FHitResult& A, const FHitResult& B) { return A.Distance < B.Distance; });
		for (const FHitResult& H : Hits)
		{
			if (H.bBlockingHit)
			{
				PrimaryHit = H;
				bBlockingHit = true;
				break;
			}
		}
	}
	else
	{
		bBlockingHit = World->LineTraceSingleByChannel(PrimaryHit, Start, TraceEnd, TraceChannel, QueryParams);
	}

	OutData->SetObjectField(TEXT("start"), VecToJson(Start));
	OutData->SetObjectField(TEXT("direction"), VecToJson(DirectionNormalized));
	OutData->SetNumberField(TEXT("trace_distance"), TraceDistance);
	OutData->SetStringField(TEXT("trace_channel"), FString::Printf(TEXT("%d"), (int32)TraceChannel));
	OutData->SetBoolField(TEXT("trace_complex"), bTraceComplex);
	OutData->SetBoolField(TEXT("include_all_hits"), bIncludeAllHits);
	OutData->SetNumberField(TEXT("max_hits"), MaxHits);
	OutData->SetBoolField(TEXT("include_actor_folder_tags"), bIncludeActorFolderTags);
	OutData->SetBoolField(TEXT("hit"), bBlockingHit);
	if (bBlockingHit)
	{
		if (bIncludeActorFolderTags)
		{
			if (AActor* HitActor = PrimaryHit.GetActor())
			{
				OutData->SetStringField(TEXT("hit_actor_folder_path"), HitActor->GetFolderPath().ToString());

				TArray<TSharedPtr<FJsonValue>> TagValues;
				TagValues.Reserve(HitActor->Tags.Num());
				for (const FName& Tag : HitActor->Tags)
				{
					TagValues.Add(MakeShared<FJsonValueString>(Tag.ToString()));
				}
				OutData->SetArrayField(TEXT("hit_actor_tags"), TagValues);
			}
		}
		UeAgentLevelOps::FillHitResultJson(PrimaryHit, OutData);
	}
	if (bIncludeAllHits)
	{
		OutData->SetNumberField(TEXT("hit_count"), Hits.Num());
		OutData->SetBoolField(TEXT("hits_truncated"), Hits.Num() > MaxHits);

		TArray<TSharedPtr<FJsonValue>> HitItems;
		HitItems.Reserve(FMath::Min(MaxHits, Hits.Num()));
		for (int32 HitIndex = 0; HitIndex < Hits.Num() && HitItems.Num() < MaxHits; ++HitIndex)
		{
			const FHitResult& H = Hits[HitIndex];
			TSharedPtr<FJsonObject> HitObj = UeAgentLevelOps::MakeHitResultSummaryJson(H, bIncludeActorFolderTags, false);
			HitObj->SetNumberField(TEXT("hit_index"), HitIndex);
			HitItems.Add(MakeShared<FJsonValueObject>(HitObj));
		}
		OutData->SetArrayField(TEXT("hits"), HitItems);
	}
	return true;
}

bool FUeAgentHttpServer::CmdLevelSnapToSurface(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FString Id;
	if (!JsonTryGetString(Ctx.Params, TEXT("id"), Id) || Id.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_id");
		return false;
	}
	Id = Id.TrimStartAndEnd();

	AActor* Actor = FindActorByNameOrLabel(World, Id);
	if (!Actor)
	{
		OutError = TEXT("actor_not_found");
		return false;
	}

	FVector Start = Actor->GetActorLocation();
	JsonTryGetVector(Ctx.Params, TEXT("start"), Start);

	FVector Direction = FVector(0.0f, 0.0f, -1.0f);
	JsonTryGetVector(Ctx.Params, TEXT("direction"), Direction);
	const FVector DirectionNormalized = Direction.GetSafeNormal();
	if (DirectionNormalized.IsNearlyZero())
	{
		OutError = TEXT("invalid_direction");
		return false;
	}

	double TraceDistance = 100000.0;
	JsonTryGetNumber(Ctx.Params, TEXT("trace_distance"), TraceDistance);
	TraceDistance = FMath::Clamp(TraceDistance, 1.0, (double)HALF_WORLD_MAX);

	ECollisionChannel TraceChannel = ECC_Visibility;
	if (!UeAgentLevelOps::ResolveTraceChannel(Ctx.Params, TraceChannel))
	{
		OutError = TEXT("unsupported_trace_channel");
		return false;
	}

	bool bTraceComplex = true;
	JsonTryGetBool(Ctx.Params, TEXT("trace_complex"), bTraceComplex);

	double OffsetCm = 0.0;
	JsonTryGetNumber(Ctx.Params, TEXT("offset_cm"), OffsetCm);
	OffsetCm = FMath::Clamp(OffsetCm, -100000.0, 100000.0);

	FString OffsetMode = TEXT("normal");
	JsonTryGetString(Ctx.Params, TEXT("offset_mode"), OffsetMode);
	OffsetMode = OffsetMode.TrimStartAndEnd().ToLower();
	if (OffsetMode.IsEmpty())
	{
		OffsetMode = TEXT("normal");
	}
	if (OffsetMode != TEXT("normal") && OffsetMode != TEXT("direction"))
	{
		OutError = TEXT("unsupported_offset_mode");
		return false;
	}

	bool bAlignRotation = false;
	JsonTryGetBool(Ctx.Params, TEXT("align_rotation"), bAlignRotation);

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(UeAgentSnapToSurface), bTraceComplex);
	QueryParams.AddIgnoredActor(Actor);

	const TArray<TSharedPtr<FJsonValue>>* IgnoreIds = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("ignore_actor_ids"), IgnoreIds) && IgnoreIds)
	{
		for (const TSharedPtr<FJsonValue>& IgnoreValue : *IgnoreIds)
		{
			const FString IgnoreId = IgnoreValue.IsValid() ? IgnoreValue->AsString() : FString();
			if (IgnoreId.IsEmpty())
			{
				continue;
			}

			if (AActor* IgnoreActor = FindActorByNameOrLabel(World, IgnoreId))
			{
				QueryParams.AddIgnoredActor(IgnoreActor);
			}
		}
	}

	const FVector TraceEnd = Start + (DirectionNormalized * TraceDistance);
	FHitResult Hit;
	const bool bHit = World->LineTraceSingleByChannel(Hit, Start, TraceEnd, TraceChannel, QueryParams);

	const FVector PreviousLocation = Actor->GetActorLocation();
	const FRotator PreviousRotation = Actor->GetActorRotation();

	OutData->SetObjectField(TEXT("actor"), UeAgentLevelOps::MakeActorInfoJson(Actor));
	OutData->SetObjectField(TEXT("start"), VecToJson(Start));
	OutData->SetObjectField(TEXT("direction"), VecToJson(DirectionNormalized));
	OutData->SetNumberField(TEXT("trace_distance"), TraceDistance);
	OutData->SetStringField(TEXT("trace_channel"), FString::Printf(TEXT("%d"), (int32)TraceChannel));
	OutData->SetBoolField(TEXT("trace_complex"), bTraceComplex);
	OutData->SetNumberField(TEXT("offset_cm"), OffsetCm);
	OutData->SetStringField(TEXT("offset_mode"), OffsetMode);
	OutData->SetBoolField(TEXT("align_rotation"), bAlignRotation);
	OutData->SetObjectField(TEXT("previous_location"), VecToJson(PreviousLocation));
	OutData->SetObjectField(TEXT("previous_rotation"), RotToJson(PreviousRotation));

	if (!bHit || !Hit.bBlockingHit)
	{
		OutData->SetBoolField(TEXT("snapped"), false);
		UeAgentLevelOps::FillHitResultJson(Hit, OutData);
		return true;
	}

	const FVector OffsetDir = (OffsetMode == TEXT("direction")) ? DirectionNormalized : Hit.ImpactNormal.GetSafeNormal();
	const FVector NewLocation = Hit.ImpactPoint + (OffsetDir * (float)OffsetCm);

	FRotator NewRotation = PreviousRotation;
	if (bAlignRotation)
	{
		const FVector Up = Hit.ImpactNormal.GetSafeNormal();
		FVector Forward = Actor->GetActorForwardVector();
		Forward = (Forward - (Up * FVector::DotProduct(Forward, Up))).GetSafeNormal();
		if (Forward.IsNearlyZero())
		{
			Forward = FVector::CrossProduct(FVector::RightVector, Up).GetSafeNormal();
			if (Forward.IsNearlyZero())
			{
				Forward = FVector::ForwardVector;
			}
		}

		NewRotation = FRotationMatrix::MakeFromXZ(Forward, Up).Rotator();
		NewRotation.Roll = 0.0f;
	}

	Actor->Modify();
	Actor->SetActorLocationAndRotation(NewLocation, NewRotation, false, nullptr, ETeleportType::TeleportPhysics);
	UeAgentLevelOps::FinishActorTransformEdit(Actor);

	OutData->SetBoolField(TEXT("snapped"), true);
	OutData->SetObjectField(TEXT("new_location"), VecToJson(Actor->GetActorLocation()));
	OutData->SetObjectField(TEXT("new_rotation"), RotToJson(Actor->GetActorRotation()));
	UeAgentLevelOps::FillHitResultJson(Hit, OutData);
	return true;
}

bool FUeAgentHttpServer::CmdLevelSweepCapsule(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* SweepsJson = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("sweeps"), SweepsJson) && SweepsJson && SweepsJson->Num() > 0)
	{
		bool bContinueOnError = false;
		JsonTryGetBool(Ctx.Params, TEXT("continue_on_error"), bContinueOnError);

		bool bStopOnBlockingHit = false;
		JsonTryGetBool(Ctx.Params, TEXT("stop_on_blocking_hit"), bStopOnBlockingHit);

		int32 MaxItems = 4096;
		{
			double MaxItemsNumber = (double)MaxItems;
			JsonTryGetNumber(Ctx.Params, TEXT("max_items"), MaxItemsNumber);
			MaxItems = (int32)FMath::Clamp(MaxItemsNumber, 1.0, 20000.0);
		}

		double DefaultRadiusCm = 0.0;
		double DefaultHalfHeightCm = 0.0;
		JsonTryGetNumber(Ctx.Params, TEXT("radius_cm"), DefaultRadiusCm);
		JsonTryGetNumber(Ctx.Params, TEXT("half_height_cm"), DefaultHalfHeightCm);
		if (DefaultRadiusCm > 0.0)
		{
			DefaultRadiusCm = FMath::Clamp(DefaultRadiusCm, 0.1, 100000.0);
		}
		if (DefaultHalfHeightCm > 0.0)
		{
			DefaultHalfHeightCm = FMath::Clamp(DefaultHalfHeightCm, 0.1, 100000.0);
		}

		ECollisionChannel DefaultTraceChannel = ECC_Pawn;
		{
			FString ChannelName;
			if (Ctx.Params->TryGetStringField(TEXT("trace_channel"), ChannelName) && !ChannelName.TrimStartAndEnd().IsEmpty())
			{
				if (!UeAgentLevelOps::ResolveTraceChannel(Ctx.Params, DefaultTraceChannel))
				{
					OutError = TEXT("unsupported_trace_channel");
					return false;
				}
			}
		}

		bool bDefaultTraceComplex = false;
		JsonTryGetBool(Ctx.Params, TEXT("trace_complex"), bDefaultTraceComplex);

		bool bDefaultFindInitialOverlaps = true;
		JsonTryGetBool(Ctx.Params, TEXT("find_initial_overlaps"), bDefaultFindInitialOverlaps);

		bool bDefaultIncludeAllHits = false;
		JsonTryGetBool(Ctx.Params, TEXT("include_all_hits"), bDefaultIncludeAllHits);

		int32 DefaultMaxHits = 32;
		{
			double MaxHitsNumber = (double)DefaultMaxHits;
			JsonTryGetNumber(Ctx.Params, TEXT("max_hits"), MaxHitsNumber);
			DefaultMaxHits = (int32)FMath::Clamp(MaxHitsNumber, 1.0, 2000.0);
		}

		bool bDefaultIncludeActorFolderTags = false;
		JsonTryGetBool(Ctx.Params, TEXT("include_actor_folder_tags"), bDefaultIncludeActorFolderTags);

		bool bDefaultReturnPenetrationDepth = false;
		JsonTryGetBool(Ctx.Params, TEXT("return_penetration_depth"), bDefaultReturnPenetrationDepth);

		TArray<AActor*> GlobalIgnoredActors;
		const TArray<TSharedPtr<FJsonValue>>* GlobalIgnoreIds = nullptr;
		if (Ctx.Params->TryGetArrayField(TEXT("ignore_actor_ids"), GlobalIgnoreIds) && GlobalIgnoreIds)
		{
			for (const TSharedPtr<FJsonValue>& IgnoreValue : *GlobalIgnoreIds)
			{
				const FString IgnoreId = IgnoreValue.IsValid() ? IgnoreValue->AsString() : FString();
				if (IgnoreId.IsEmpty())
				{
					continue;
				}

				if (AActor* IgnoreActor = FindActorByNameOrLabel(World, IgnoreId))
				{
					GlobalIgnoredActors.Add(IgnoreActor);
				}
			}
		}
		UeAgentLevelOps::AppendIgnoredActorsFromFilters(World, Ctx.Params, GlobalIgnoredActors);

		const int32 InputCount = SweepsJson->Num();
		const int32 ExecuteCount = FMath::Min(InputCount, MaxItems);
		const bool bTruncatedByMaxItems = InputCount > ExecuteCount;

		int32 ErrorCount = 0;
		int32 BlockingHitCount = 0;
		int32 FirstErrorIndex = INDEX_NONE;
		FString FirstError;
		int32 FirstBlockingIndex = INDEX_NONE;

		TArray<TSharedPtr<FJsonValue>> Results;
		Results.Reserve(ExecuteCount);

		for (int32 Index = 0; Index < ExecuteCount; ++Index)
		{
			const TSharedPtr<FJsonValue>& SweepValue = (*SweepsJson)[Index];
			const TSharedPtr<FJsonObject> SweepObj = SweepValue.IsValid() ? SweepValue->AsObject() : nullptr;
			if (!SweepObj.IsValid())
			{
				++ErrorCount;
				if (FirstErrorIndex == INDEX_NONE)
				{
					FirstErrorIndex = Index;
					FirstError = TEXT("sweeps_invalid_item");
				}
				if (!bContinueOnError)
				{
					OutError = TEXT("sweeps_invalid_item");
					return false;
				}

				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("index"), Index);
				Item->SetBoolField(TEXT("ok"), false);
				Item->SetStringField(TEXT("error"), TEXT("sweeps_invalid_item"));
				Results.Add(MakeShared<FJsonValueObject>(Item));
				continue;
			}

			FVector Start = FVector::ZeroVector;
			FVector End = FVector::ZeroVector;
			if (!JsonTryGetVector(SweepObj, TEXT("start"), Start))
			{
				++ErrorCount;
				if (FirstErrorIndex == INDEX_NONE)
				{
					FirstErrorIndex = Index;
					FirstError = TEXT("missing_start");
				}
				if (!bContinueOnError)
				{
					OutError = TEXT("missing_start");
					return false;
				}

				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("index"), Index);
				Item->SetBoolField(TEXT("ok"), false);
				Item->SetStringField(TEXT("error"), TEXT("missing_start"));
				Results.Add(MakeShared<FJsonValueObject>(Item));
				continue;
			}
			if (!JsonTryGetVector(SweepObj, TEXT("end"), End))
			{
				++ErrorCount;
				if (FirstErrorIndex == INDEX_NONE)
				{
					FirstErrorIndex = Index;
					FirstError = TEXT("missing_end");
				}
				if (!bContinueOnError)
				{
					OutError = TEXT("missing_end");
					return false;
				}

				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("index"), Index);
				Item->SetBoolField(TEXT("ok"), false);
				Item->SetStringField(TEXT("error"), TEXT("missing_end"));
				Item->SetObjectField(TEXT("start"), VecToJson(Start));
				Results.Add(MakeShared<FJsonValueObject>(Item));
				continue;
			}

			double RadiusCm = DefaultRadiusCm;
			double HalfHeightCm = DefaultHalfHeightCm;
			JsonTryGetNumber(SweepObj, TEXT("radius_cm"), RadiusCm);
			JsonTryGetNumber(SweepObj, TEXT("half_height_cm"), HalfHeightCm);
			if (RadiusCm <= 0.0)
			{
				++ErrorCount;
				if (FirstErrorIndex == INDEX_NONE)
				{
					FirstErrorIndex = Index;
					FirstError = TEXT("missing_radius_cm");
				}
				if (!bContinueOnError)
				{
					OutError = TEXT("missing_radius_cm");
					return false;
				}

				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("index"), Index);
				Item->SetBoolField(TEXT("ok"), false);
				Item->SetStringField(TEXT("error"), TEXT("missing_radius_cm"));
				Item->SetObjectField(TEXT("start"), VecToJson(Start));
				Item->SetObjectField(TEXT("end"), VecToJson(End));
				Results.Add(MakeShared<FJsonValueObject>(Item));
				continue;
			}
			if (HalfHeightCm <= 0.0)
			{
				++ErrorCount;
				if (FirstErrorIndex == INDEX_NONE)
				{
					FirstErrorIndex = Index;
					FirstError = TEXT("missing_half_height_cm");
				}
				if (!bContinueOnError)
				{
					OutError = TEXT("missing_half_height_cm");
					return false;
				}

				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("index"), Index);
				Item->SetBoolField(TEXT("ok"), false);
				Item->SetStringField(TEXT("error"), TEXT("missing_half_height_cm"));
				Item->SetObjectField(TEXT("start"), VecToJson(Start));
				Item->SetObjectField(TEXT("end"), VecToJson(End));
				Item->SetNumberField(TEXT("radius_cm"), RadiusCm);
				Results.Add(MakeShared<FJsonValueObject>(Item));
				continue;
			}

			RadiusCm = FMath::Clamp(RadiusCm, 0.1, 100000.0);
			HalfHeightCm = FMath::Clamp(HalfHeightCm, 0.1, 100000.0);

			ECollisionChannel TraceChannel = DefaultTraceChannel;
			{
				FString ChannelName;
				if (SweepObj->TryGetStringField(TEXT("trace_channel"), ChannelName) && !ChannelName.TrimStartAndEnd().IsEmpty())
				{
					if (!UeAgentLevelOps::ResolveTraceChannel(SweepObj, TraceChannel))
					{
						++ErrorCount;
						if (FirstErrorIndex == INDEX_NONE)
						{
							FirstErrorIndex = Index;
							FirstError = TEXT("unsupported_trace_channel");
						}
						if (!bContinueOnError)
						{
							OutError = TEXT("unsupported_trace_channel");
							return false;
						}

						TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetNumberField(TEXT("index"), Index);
						Item->SetBoolField(TEXT("ok"), false);
						Item->SetStringField(TEXT("error"), TEXT("unsupported_trace_channel"));
						Item->SetObjectField(TEXT("start"), VecToJson(Start));
						Item->SetObjectField(TEXT("end"), VecToJson(End));
						Item->SetNumberField(TEXT("radius_cm"), RadiusCm);
						Item->SetNumberField(TEXT("half_height_cm"), HalfHeightCm);
						Results.Add(MakeShared<FJsonValueObject>(Item));
						continue;
					}
				}
			}

			bool bTraceComplex = bDefaultTraceComplex;
			SweepObj->TryGetBoolField(TEXT("trace_complex"), bTraceComplex);

			bool bFindInitialOverlaps = bDefaultFindInitialOverlaps;
			SweepObj->TryGetBoolField(TEXT("find_initial_overlaps"), bFindInitialOverlaps);

			bool bIncludeAllHits = bDefaultIncludeAllHits;
			SweepObj->TryGetBoolField(TEXT("include_all_hits"), bIncludeAllHits);

			int32 MaxHits = DefaultMaxHits;
			{
				double MaxHitsNumber = (double)MaxHits;
				JsonTryGetNumber(SweepObj, TEXT("max_hits"), MaxHitsNumber);
				MaxHits = (int32)FMath::Clamp(MaxHitsNumber, 1.0, 2000.0);
			}

			bool bIncludeActorFolderTags = bDefaultIncludeActorFolderTags;
			SweepObj->TryGetBoolField(TEXT("include_actor_folder_tags"), bIncludeActorFolderTags);

			bool bReturnPenetrationDepth = bDefaultReturnPenetrationDepth;
			SweepObj->TryGetBoolField(TEXT("return_penetration_depth"), bReturnPenetrationDepth);

			FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(UeAgentSweepCapsuleBatch), bTraceComplex);
			QueryParams.bFindInitialOverlaps = bFindInitialOverlaps;
			for (AActor* Ignored : GlobalIgnoredActors)
			{
				if (Ignored)
				{
					QueryParams.AddIgnoredActor(Ignored);
				}
			}

			const TArray<TSharedPtr<FJsonValue>>* IgnoreIds = nullptr;
			if (SweepObj->TryGetArrayField(TEXT("ignore_actor_ids"), IgnoreIds) && IgnoreIds)
			{
				for (const TSharedPtr<FJsonValue>& IgnoreValue : *IgnoreIds)
				{
					const FString IgnoreId = IgnoreValue.IsValid() ? IgnoreValue->AsString() : FString();
					if (IgnoreId.IsEmpty())
					{
						continue;
					}

					if (AActor* IgnoreActor = FindActorByNameOrLabel(World, IgnoreId))
					{
						QueryParams.AddIgnoredActor(IgnoreActor);
					}
				}
			}

			const FCollisionShape Shape = FCollisionShape::MakeCapsule((float)RadiusCm, (float)HalfHeightCm);

			FHitResult PrimaryHit;
			TArray<FHitResult> Hits;
			if (bIncludeAllHits)
			{
				World->SweepMultiByChannel(Hits, Start, End, FQuat::Identity, TraceChannel, Shape, QueryParams);
				Hits.Sort([](const FHitResult& A, const FHitResult& B) { return A.Time < B.Time; });
				for (const FHitResult& H : Hits)
				{
					if (H.bBlockingHit)
					{
						PrimaryHit = H;
						break;
					}
				}
			}
			else
			{
				World->SweepSingleByChannel(PrimaryHit, Start, End, FQuat::Identity, TraceChannel, Shape, QueryParams);
			}

			const bool bBlockingHit = PrimaryHit.bBlockingHit;
			if (bBlockingHit)
			{
				++BlockingHitCount;
				if (FirstBlockingIndex == INDEX_NONE)
				{
					FirstBlockingIndex = Index;
				}
			}

			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetNumberField(TEXT("index"), Index);
			Item->SetBoolField(TEXT("ok"), true);
			Item->SetObjectField(TEXT("start"), VecToJson(Start));
			Item->SetObjectField(TEXT("end"), VecToJson(End));
			Item->SetNumberField(TEXT("radius_cm"), RadiusCm);
			Item->SetNumberField(TEXT("half_height_cm"), HalfHeightCm);
			Item->SetStringField(TEXT("trace_channel"), FString::Printf(TEXT("%d"), (int32)TraceChannel));
			Item->SetBoolField(TEXT("trace_complex"), bTraceComplex);
			Item->SetBoolField(TEXT("find_initial_overlaps"), bFindInitialOverlaps);
			Item->SetBoolField(TEXT("include_all_hits"), bIncludeAllHits);
			Item->SetNumberField(TEXT("max_hits"), MaxHits);
			Item->SetBoolField(TEXT("include_actor_folder_tags"), bIncludeActorFolderTags);
			Item->SetBoolField(TEXT("return_penetration_depth"), bReturnPenetrationDepth);
			Item->SetBoolField(TEXT("blocking_hit"), bBlockingHit);
			Item->SetBoolField(TEXT("start_penetrating"), PrimaryHit.bStartPenetrating);
			if (bReturnPenetrationDepth && PrimaryHit.bStartPenetrating)
			{
				Item->SetNumberField(TEXT("penetration_depth_cm"), PrimaryHit.PenetrationDepth);
			}
			UeAgentLevelOps::MaybeAttachStartPenetratingAdvice(
				TEXT("level_sweep_capsule"),
				PrimaryHit.bStartPenetrating,
				bFindInitialOverlaps,
				false,
				0.0,
				Item);
			UeAgentLevelOps::FillHitResultJson(PrimaryHit, Item);
			if (bIncludeActorFolderTags)
			{
				if (AActor* HitActor = PrimaryHit.GetActor())
				{
					Item->SetStringField(TEXT("hit_actor_folder_path"), HitActor->GetFolderPath().ToString());

					TArray<TSharedPtr<FJsonValue>> TagValues;
					TagValues.Reserve(HitActor->Tags.Num());
					for (const FName& Tag : HitActor->Tags)
					{
						TagValues.Add(MakeShared<FJsonValueString>(Tag.ToString()));
					}
					Item->SetArrayField(TEXT("hit_actor_tags"), TagValues);
				}
			}
			if (bIncludeAllHits)
			{
				Item->SetNumberField(TEXT("hit_count"), Hits.Num());
				Item->SetBoolField(TEXT("hits_truncated"), Hits.Num() > MaxHits);

				TArray<TSharedPtr<FJsonValue>> HitItems;
				HitItems.Reserve(FMath::Min(MaxHits, Hits.Num()));
				for (int32 HitIndex = 0; HitIndex < Hits.Num() && HitItems.Num() < MaxHits; ++HitIndex)
				{
					const FHitResult& H = Hits[HitIndex];
					TSharedPtr<FJsonObject> HitObj = UeAgentLevelOps::MakeHitResultSummaryJson(H, bIncludeActorFolderTags, bReturnPenetrationDepth);
					HitObj->SetNumberField(TEXT("hit_index"), HitIndex);
					HitItems.Add(MakeShared<FJsonValueObject>(HitObj));
				}
				Item->SetArrayField(TEXT("hits"), HitItems);
			}
			Results.Add(MakeShared<FJsonValueObject>(Item));

			if (bBlockingHit && bStopOnBlockingHit)
			{
				break;
			}
		}

		const bool bStoppedEarly = bStopOnBlockingHit && FirstBlockingIndex != INDEX_NONE && (FirstBlockingIndex < ExecuteCount - 1);

		OutData->SetStringField(TEXT("mode"), TEXT("batch"));
		OutData->SetNumberField(TEXT("input_sweep_count"), InputCount);
		OutData->SetNumberField(TEXT("sweep_count"), Results.Num());
		OutData->SetNumberField(TEXT("max_items"), MaxItems);
		OutData->SetBoolField(TEXT("truncated"), bTruncatedByMaxItems || bStoppedEarly);
		OutData->SetBoolField(TEXT("continue_on_error"), bContinueOnError);
		OutData->SetBoolField(TEXT("stop_on_blocking_hit"), bStopOnBlockingHit);
		OutData->SetNumberField(TEXT("blocking_hit_count"), BlockingHitCount);
		OutData->SetNumberField(TEXT("first_blocking_index"), FirstBlockingIndex);
		OutData->SetNumberField(TEXT("error_count"), ErrorCount);
		OutData->SetNumberField(TEXT("first_error_index"), FirstErrorIndex);
		if (FirstErrorIndex != INDEX_NONE)
		{
			OutData->SetStringField(TEXT("first_error"), FirstError);
		}
		OutData->SetArrayField(TEXT("sweeps"), Results);
		return true;
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

	double RadiusCm = 0.0;
	double HalfHeightCm = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("radius_cm"), RadiusCm))
	{
		OutError = TEXT("missing_radius_cm");
		return false;
	}
	if (!JsonTryGetNumber(Ctx.Params, TEXT("half_height_cm"), HalfHeightCm))
	{
		OutError = TEXT("missing_half_height_cm");
		return false;
	}
	RadiusCm = FMath::Clamp(RadiusCm, 0.1, 100000.0);
	HalfHeightCm = FMath::Clamp(HalfHeightCm, 0.1, 100000.0);

	ECollisionChannel TraceChannel = ECC_Pawn;
	{
		FString ChannelName;
		if (Ctx.Params.IsValid() && Ctx.Params->TryGetStringField(TEXT("trace_channel"), ChannelName) && !ChannelName.TrimStartAndEnd().IsEmpty())
		{
			if (!UeAgentLevelOps::ResolveTraceChannel(Ctx.Params, TraceChannel))
			{
				OutError = TEXT("unsupported_trace_channel");
				return false;
			}
		}
	}

	bool bTraceComplex = false;
	JsonTryGetBool(Ctx.Params, TEXT("trace_complex"), bTraceComplex);

	bool bFindInitialOverlaps = true;
	JsonTryGetBool(Ctx.Params, TEXT("find_initial_overlaps"), bFindInitialOverlaps);

	bool bReturnPenetrationDepth = false;
	JsonTryGetBool(Ctx.Params, TEXT("return_penetration_depth"), bReturnPenetrationDepth);

	bool bIncludeActorFolderTags = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_actor_folder_tags"), bIncludeActorFolderTags);

	bool bIncludeAllHits = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_all_hits"), bIncludeAllHits);

	int32 MaxHits = 32;
	{
		double MaxHitsNumber = (double)MaxHits;
		JsonTryGetNumber(Ctx.Params, TEXT("max_hits"), MaxHitsNumber);
		MaxHits = (int32)FMath::Clamp(MaxHitsNumber, 1.0, 2000.0);
	}

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(UeAgentSweepCapsule), bTraceComplex);
	QueryParams.bFindInitialOverlaps = bFindInitialOverlaps;

	TArray<AActor*> IgnoredActors;
	const TArray<TSharedPtr<FJsonValue>>* IgnoreIds = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("ignore_actor_ids"), IgnoreIds) && IgnoreIds)
	{
		for (const TSharedPtr<FJsonValue>& IgnoreValue : *IgnoreIds)
		{
			const FString IgnoreId = IgnoreValue.IsValid() ? IgnoreValue->AsString() : FString();
			if (IgnoreId.IsEmpty())
			{
				continue;
			}

			if (AActor* IgnoreActor = FindActorByNameOrLabel(World, IgnoreId))
			{
				IgnoredActors.Add(IgnoreActor);
			}
		}
	}
	UeAgentLevelOps::AppendIgnoredActorsFromFilters(World, Ctx.Params, IgnoredActors);
	for (AActor* Ignored : IgnoredActors)
	{
		if (Ignored)
		{
			QueryParams.AddIgnoredActor(Ignored);
		}
	}

	const FCollisionShape Shape = FCollisionShape::MakeCapsule((float)RadiusCm, (float)HalfHeightCm);

	FHitResult PrimaryHit;
	TArray<FHitResult> Hits;
	if (bIncludeAllHits)
	{
		World->SweepMultiByChannel(Hits, Start, End, FQuat::Identity, TraceChannel, Shape, QueryParams);
		Hits.Sort([](const FHitResult& A, const FHitResult& B) { return A.Time < B.Time; });
		for (const FHitResult& H : Hits)
		{
			if (H.bBlockingHit)
			{
				PrimaryHit = H;
				break;
			}
		}
	}
	else
	{
		World->SweepSingleByChannel(PrimaryHit, Start, End, FQuat::Identity, TraceChannel, Shape, QueryParams);
	}

	OutData->SetObjectField(TEXT("start"), VecToJson(Start));
	OutData->SetObjectField(TEXT("end"), VecToJson(End));
	OutData->SetNumberField(TEXT("radius_cm"), RadiusCm);
	OutData->SetNumberField(TEXT("half_height_cm"), HalfHeightCm);
	OutData->SetStringField(TEXT("trace_channel"), FString::Printf(TEXT("%d"), (int32)TraceChannel));
	OutData->SetBoolField(TEXT("trace_complex"), bTraceComplex);
	OutData->SetBoolField(TEXT("find_initial_overlaps"), bFindInitialOverlaps);
	OutData->SetBoolField(TEXT("include_all_hits"), bIncludeAllHits);
	OutData->SetNumberField(TEXT("max_hits"), MaxHits);
	OutData->SetBoolField(TEXT("include_actor_folder_tags"), bIncludeActorFolderTags);
	OutData->SetBoolField(TEXT("return_penetration_depth"), bReturnPenetrationDepth);
	OutData->SetBoolField(TEXT("blocking_hit"), PrimaryHit.bBlockingHit);
	OutData->SetBoolField(TEXT("start_penetrating"), PrimaryHit.bStartPenetrating);
	if (bReturnPenetrationDepth && PrimaryHit.bStartPenetrating)
	{
		OutData->SetNumberField(TEXT("penetration_depth_cm"), PrimaryHit.PenetrationDepth);
	}
	UeAgentLevelOps::MaybeAttachStartPenetratingAdvice(
		TEXT("level_sweep_capsule"),
		PrimaryHit.bStartPenetrating,
		bFindInitialOverlaps,
		false,
		0.0,
		OutData);
	UeAgentLevelOps::FillHitResultJson(PrimaryHit, OutData);
	if (bIncludeActorFolderTags)
	{
		if (AActor* HitActor = PrimaryHit.GetActor())
		{
			OutData->SetStringField(TEXT("hit_actor_folder_path"), HitActor->GetFolderPath().ToString());

			TArray<TSharedPtr<FJsonValue>> TagValues;
			TagValues.Reserve(HitActor->Tags.Num());
			for (const FName& Tag : HitActor->Tags)
			{
				TagValues.Add(MakeShared<FJsonValueString>(Tag.ToString()));
			}
			OutData->SetArrayField(TEXT("hit_actor_tags"), TagValues);
		}
	}
	if (bIncludeAllHits)
	{
		OutData->SetNumberField(TEXT("hit_count"), Hits.Num());
		OutData->SetBoolField(TEXT("hits_truncated"), Hits.Num() > MaxHits);

		TArray<TSharedPtr<FJsonValue>> HitItems;
		HitItems.Reserve(FMath::Min(MaxHits, Hits.Num()));
		for (int32 HitIndex = 0; HitIndex < Hits.Num() && HitItems.Num() < MaxHits; ++HitIndex)
		{
			const FHitResult& H = Hits[HitIndex];
			TSharedPtr<FJsonObject> HitObj = UeAgentLevelOps::MakeHitResultSummaryJson(H, bIncludeActorFolderTags, bReturnPenetrationDepth);
			HitObj->SetNumberField(TEXT("hit_index"), HitIndex);
			HitItems.Add(MakeShared<FJsonValueObject>(HitObj));
		}
		OutData->SetArrayField(TEXT("hits"), HitItems);
	}
	return true;
}

bool FUeAgentHttpServer::CmdLevelSweepCapsulePath(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* PointsJson = nullptr;
	if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetArrayField(TEXT("points"), PointsJson) || !PointsJson || PointsJson->Num() < 2)
	{
		OutError = TEXT("points_required");
		return false;
	}

	double RadiusCm = 0.0;
	double HalfHeightCm = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("radius_cm"), RadiusCm))
	{
		OutError = TEXT("missing_radius_cm");
		return false;
	}
	if (!JsonTryGetNumber(Ctx.Params, TEXT("half_height_cm"), HalfHeightCm))
	{
		OutError = TEXT("missing_half_height_cm");
		return false;
	}
	RadiusCm = FMath::Clamp(RadiusCm, 0.1, 100000.0);
	HalfHeightCm = FMath::Clamp(HalfHeightCm, 0.1, 100000.0);

	double StepCm = 50.0;
	JsonTryGetNumber(Ctx.Params, TEXT("step_cm"), StepCm);
	StepCm = FMath::Clamp(StepCm, 0.1, 100000.0);

	int32 MaxSamples = 2048;
	{
		double MaxSamplesNumber = (double)MaxSamples;
		JsonTryGetNumber(Ctx.Params, TEXT("max_samples"), MaxSamplesNumber);
		MaxSamples = (int32)FMath::Clamp(MaxSamplesNumber, 2.0, 20000.0);
	}

	FString PointsMode = TEXT("feet");
	JsonTryGetString(Ctx.Params, TEXT("points_mode"), PointsMode);
	PointsMode = PointsMode.TrimStartAndEnd().ToLower();
	const bool bPointsAreFeet = PointsMode.IsEmpty() || PointsMode == TEXT("feet");
	const bool bPointsAreCenter = PointsMode == TEXT("center");
	if (!bPointsAreFeet && !bPointsAreCenter)
	{
		OutError = TEXT("unsupported_points_mode");
		return false;
	}

	bool bSnapToFloor = bPointsAreFeet;
	JsonTryGetBool(Ctx.Params, TEXT("snap_to_floor"), bSnapToFloor);

	bool bRequireFloor = bPointsAreFeet && bSnapToFloor;
	JsonTryGetBool(Ctx.Params, TEXT("require_floor"), bRequireFloor);

	double FloorClearanceCm = 2.0;
	JsonTryGetNumber(Ctx.Params, TEXT("floor_clearance_cm"), FloorClearanceCm);
	FloorClearanceCm = FMath::Clamp(FloorClearanceCm, 0.0, 1000.0);

	double FloorTraceUpCm = 50.0;
	double FloorTraceDownCm = 200.0;
	JsonTryGetNumber(Ctx.Params, TEXT("floor_trace_up_cm"), FloorTraceUpCm);
	JsonTryGetNumber(Ctx.Params, TEXT("floor_trace_down_cm"), FloorTraceDownCm);
	FloorTraceUpCm = FMath::Clamp(FloorTraceUpCm, 0.0, 100000.0);
	FloorTraceDownCm = FMath::Clamp(FloorTraceDownCm, 0.0, 100000.0);

	bool bIgnoreWalkableFloorHits = false;
	JsonTryGetBool(Ctx.Params, TEXT("ignore_walkable_floor_hits"), bIgnoreWalkableFloorHits);

	double MaxWalkableSlopeDeg = 45.0;
	JsonTryGetNumber(Ctx.Params, TEXT("max_walkable_slope_deg"), MaxWalkableSlopeDeg);
	MaxWalkableSlopeDeg = FMath::Clamp(MaxWalkableSlopeDeg, 0.0, 89.0);
	const double WalkableCos = FMath::Cos(FMath::DegreesToRadians(MaxWalkableSlopeDeg));

	bool bStopOnBlockingHit = true;
	JsonTryGetBool(Ctx.Params, TEXT("stop_on_blocking_hit"), bStopOnBlockingHit);

	bool bIncludeSamples = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_samples"), bIncludeSamples);
	bool bIncludeSegments = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_segments"), bIncludeSegments);

	ECollisionChannel TraceChannel = ECC_Pawn;
	{
		FString ChannelName;
		if (Ctx.Params.IsValid() && Ctx.Params->TryGetStringField(TEXT("trace_channel"), ChannelName) && !ChannelName.TrimStartAndEnd().IsEmpty())
		{
			if (!UeAgentLevelOps::ResolveTraceChannel(Ctx.Params, TraceChannel))
			{
				OutError = TEXT("unsupported_trace_channel");
				return false;
			}
		}
	}

	bool bTraceComplex = false;
	JsonTryGetBool(Ctx.Params, TEXT("trace_complex"), bTraceComplex);

	bool bFindInitialOverlaps = true;
	JsonTryGetBool(Ctx.Params, TEXT("find_initial_overlaps"), bFindInitialOverlaps);

	bool bReturnPenetrationDepth = false;
	JsonTryGetBool(Ctx.Params, TEXT("return_penetration_depth"), bReturnPenetrationDepth);

	bool bIncludeActorFolderTags = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_actor_folder_tags"), bIncludeActorFolderTags);

	// Optional floor trace settings.
	ECollisionChannel FloorTraceChannel = ECC_Visibility;
	{
		FString ChannelName;
		if (Ctx.Params.IsValid() && Ctx.Params->TryGetStringField(TEXT("floor_trace_channel"), ChannelName) && !ChannelName.TrimStartAndEnd().IsEmpty())
		{
			const FString Normalized = ChannelName.TrimStartAndEnd().ToLower();
			if (Normalized == TEXT("visibility") || Normalized == TEXT("ecc_visibility"))
			{
				FloorTraceChannel = ECC_Visibility;
			}
			else if (Normalized == TEXT("camera") || Normalized == TEXT("ecc_camera"))
			{
				FloorTraceChannel = ECC_Camera;
			}
			else if (Normalized == TEXT("worldstatic") || Normalized == TEXT("world_static") || Normalized == TEXT("ecc_worldstatic"))
			{
				FloorTraceChannel = ECC_WorldStatic;
			}
			else if (Normalized == TEXT("worlddynamic") || Normalized == TEXT("world_dynamic") || Normalized == TEXT("ecc_worlddynamic"))
			{
				FloorTraceChannel = ECC_WorldDynamic;
			}
			else if (Normalized == TEXT("pawn") || Normalized == TEXT("ecc_pawn"))
			{
				FloorTraceChannel = ECC_Pawn;
			}
			else if (Normalized == TEXT("physicsbody") || Normalized == TEXT("physics_body") || Normalized == TEXT("ecc_physicsbody"))
			{
				FloorTraceChannel = ECC_PhysicsBody;
			}
			else
			{
				OutError = TEXT("unsupported_floor_trace_channel");
				return false;
			}
		}
	}

	bool bFloorTraceComplex = true;
	JsonTryGetBool(Ctx.Params, TEXT("floor_trace_complex"), bFloorTraceComplex);

	const TArray<TSharedPtr<FJsonValue>>* IgnoreIds = nullptr;
	TArray<AActor*> IgnoredActors;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("ignore_actor_ids"), IgnoreIds) && IgnoreIds)
	{
		for (const TSharedPtr<FJsonValue>& IgnoreValue : *IgnoreIds)
		{
			const FString IgnoreId = IgnoreValue.IsValid() ? IgnoreValue->AsString() : FString();
			if (IgnoreId.IsEmpty())
			{
				continue;
			}

			if (AActor* IgnoreActor = FindActorByNameOrLabel(World, IgnoreId))
			{
				IgnoredActors.Add(IgnoreActor);
			}
		}
	}
	UeAgentLevelOps::AppendIgnoredActorsFromFilters(World, Ctx.Params, IgnoredActors);

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

	TArray<FVector> InputPoints;
	InputPoints.Reserve(PointsJson->Num());
	for (const TSharedPtr<FJsonValue>& Value : *PointsJson)
	{
		FVector Point;
		if (!TryGetVectorFromValue(Value, Point))
		{
			OutError = TEXT("points_invalid_vector");
			return false;
		}
		InputPoints.Add(Point);
	}

	TArray<FVector> SamplePoints;
	SamplePoints.Reserve(FMath::Min(MaxSamples, (int32)(InputPoints.Num() * 8)));
	bool bTruncated = false;
	for (int32 i = 0; i < InputPoints.Num() - 1; ++i)
	{
		const FVector A = InputPoints[i];
		const FVector B = InputPoints[i + 1];
		const double Dist = FVector::Distance(A, B);
		const int32 Steps = FMath::Max(1, (int32)FMath::CeilToDouble(Dist / StepCm));

		for (int32 s = (i == 0 ? 0 : 1); s <= Steps; ++s)
		{
			const double Alpha = Steps > 0 ? ((double)s / (double)Steps) : 1.0;
			const FVector P = FMath::Lerp(A, B, (float)Alpha);
			SamplePoints.Add(P);
			if (SamplePoints.Num() >= MaxSamples)
			{
				bTruncated = true;
				break;
			}
		}
		if (bTruncated)
		{
			break;
		}
	}

	struct FSample
	{
		FVector Input = FVector::ZeroVector;
		FVector Feet = FVector::ZeroVector;
		FVector Center = FVector::ZeroVector;
		bool bFloorHit = false;
		FVector FloorNormal = FVector::UpVector;
	};

	TArray<FSample> Samples;
	Samples.Reserve(SamplePoints.Num());

	bool bFloorMissing = false;
	int32 FirstMissingFloorSampleIndex = INDEX_NONE;

	FCollisionQueryParams FloorQueryParams(SCENE_QUERY_STAT(UeAgentSweepCapsulePathFloor), bFloorTraceComplex);
	for (AActor* Ignored : IgnoredActors)
	{
		if (Ignored)
		{
			FloorQueryParams.AddIgnoredActor(Ignored);
		}
	}

	for (int32 Index = 0; Index < SamplePoints.Num(); ++Index)
	{
		FSample Sample;
		Sample.Input = SamplePoints[Index];

		if (bPointsAreCenter)
		{
			Sample.Center = Sample.Input;
			Sample.Feet = Sample.Input - FVector(0.0f, 0.0f, (float)(HalfHeightCm + FloorClearanceCm));
			Samples.Add(Sample);
			continue;
		}

		Sample.Feet = Sample.Input;
		if (bSnapToFloor)
		{
			const FVector TraceStart = Sample.Feet + FVector(0.0f, 0.0f, (float)FloorTraceUpCm);
			const FVector TraceEnd = Sample.Feet - FVector(0.0f, 0.0f, (float)FloorTraceDownCm);
			FHitResult FloorHit;
			const bool bHit = World->LineTraceSingleByChannel(FloorHit, TraceStart, TraceEnd, FloorTraceChannel, FloorQueryParams);
			if (bHit && FloorHit.bBlockingHit)
			{
				Sample.bFloorHit = true;
				Sample.Feet = FloorHit.ImpactPoint;
				Sample.FloorNormal = FloorHit.ImpactNormal.GetSafeNormal();
			}
		}

		if (bRequireFloor && !Sample.bFloorHit)
		{
			bFloorMissing = true;
			FirstMissingFloorSampleIndex = Index;
			break;
		}

		Sample.Center = Sample.Feet + FVector(0.0f, 0.0f, (float)(HalfHeightCm + FloorClearanceCm));
		Samples.Add(Sample);
	}

	OutData->SetStringField(TEXT("points_mode"), bPointsAreCenter ? TEXT("center") : TEXT("feet"));
	OutData->SetBoolField(TEXT("snap_to_floor"), bSnapToFloor);
	OutData->SetBoolField(TEXT("require_floor"), bRequireFloor);
	OutData->SetNumberField(TEXT("radius_cm"), RadiusCm);
	OutData->SetNumberField(TEXT("half_height_cm"), HalfHeightCm);
	OutData->SetNumberField(TEXT("step_cm"), StepCm);
	OutData->SetNumberField(TEXT("max_samples"), MaxSamples);
	OutData->SetBoolField(TEXT("truncated"), bTruncated);
	OutData->SetNumberField(TEXT("floor_clearance_cm"), FloorClearanceCm);
	OutData->SetNumberField(TEXT("floor_trace_up_cm"), FloorTraceUpCm);
	OutData->SetNumberField(TEXT("floor_trace_down_cm"), FloorTraceDownCm);
	OutData->SetStringField(TEXT("floor_trace_channel"), FString::Printf(TEXT("%d"), (int32)FloorTraceChannel));
	OutData->SetBoolField(TEXT("floor_trace_complex"), bFloorTraceComplex);
	OutData->SetBoolField(TEXT("ignore_walkable_floor_hits"), bIgnoreWalkableFloorHits);
	OutData->SetNumberField(TEXT("max_walkable_slope_deg"), MaxWalkableSlopeDeg);
	OutData->SetStringField(TEXT("trace_channel"), FString::Printf(TEXT("%d"), (int32)TraceChannel));
	OutData->SetBoolField(TEXT("trace_complex"), bTraceComplex);
	OutData->SetBoolField(TEXT("find_initial_overlaps"), bFindInitialOverlaps);
	OutData->SetBoolField(TEXT("return_penetration_depth"), bReturnPenetrationDepth);
	OutData->SetBoolField(TEXT("include_actor_folder_tags"), bIncludeActorFolderTags);
	OutData->SetBoolField(TEXT("stop_on_blocking_hit"), bStopOnBlockingHit);
	OutData->SetBoolField(TEXT("include_samples"), bIncludeSamples);
	OutData->SetBoolField(TEXT("include_segments"), bIncludeSegments);

	OutData->SetNumberField(TEXT("input_point_count"), InputPoints.Num());
	OutData->SetNumberField(TEXT("sample_count"), Samples.Num());
	OutData->SetBoolField(TEXT("floor_missing"), bFloorMissing);
	if (bFloorMissing)
	{
		OutData->SetNumberField(TEXT("first_missing_floor_sample_index"), FirstMissingFloorSampleIndex);
		OutData->SetBoolField(TEXT("blocking_hit"), false);
		OutData->SetNumberField(TEXT("segment_count"), FMath::Max(0, Samples.Num() - 1));
		OutData->SetBoolField(TEXT("path_valid"), false);
		OutData->SetStringField(TEXT("path_invalid_reason"), TEXT("floor_missing"));

		if (bIncludeSamples)
		{
			TArray<TSharedPtr<FJsonValue>> SamplesArr;
			SamplesArr.Reserve(Samples.Num());
			for (const FSample& S : Samples)
			{
				TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
				Obj->SetObjectField(TEXT("input"), VecToJson(S.Input));
				Obj->SetObjectField(TEXT("feet"), VecToJson(S.Feet));
				Obj->SetObjectField(TEXT("center"), VecToJson(S.Center));
				Obj->SetBoolField(TEXT("floor_hit"), S.bFloorHit);
				Obj->SetObjectField(TEXT("floor_normal"), VecToJson(S.FloorNormal));
				SamplesArr.Add(MakeShared<FJsonValueObject>(Obj));
			}
			OutData->SetArrayField(TEXT("samples"), SamplesArr);
		}
		return true;
	}

	FCollisionQueryParams SweepParams(SCENE_QUERY_STAT(UeAgentSweepCapsulePath), bTraceComplex);
	SweepParams.bFindInitialOverlaps = bFindInitialOverlaps;
	for (AActor* Ignored : IgnoredActors)
	{
		if (Ignored)
		{
			SweepParams.AddIgnoredActor(Ignored);
		}
	}

	const FCollisionShape Shape = FCollisionShape::MakeCapsule((float)RadiusCm, (float)HalfHeightCm);

	bool bBlockingHitFound = false;
	int32 FirstBlockingSegmentIndex = INDEX_NONE;
	FHitResult FirstBlockingHit;

	TArray<TSharedPtr<FJsonValue>> SegmentArr;
	if (bIncludeSegments)
	{
		SegmentArr.Reserve(FMath::Max(0, Samples.Num() - 1));
	}

	const double FloorZTolerance = FloorClearanceCm + 5.0;
	for (int32 SegIndex = 0; SegIndex < Samples.Num() - 1; ++SegIndex)
	{
		const FVector StartCenter = Samples[SegIndex].Center;
		const FVector EndCenter = Samples[SegIndex + 1].Center;

		TArray<FHitResult> Hits;
		World->SweepMultiByChannel(Hits, StartCenter, EndCenter, FQuat::Identity, TraceChannel, Shape, SweepParams);
		Hits.Sort([](const FHitResult& A, const FHitResult& B) { return A.Time < B.Time; });

		int32 IgnoredFloorHitCount = 0;
		FHitResult BlockingHit;
		for (const FHitResult& Hit : Hits)
		{
			if (!Hit.bBlockingHit)
			{
				continue;
			}

			bool bIgnored = false;
			if (bIgnoreWalkableFloorHits && bPointsAreFeet && bSnapToFloor && Samples[SegIndex].bFloorHit && Samples[SegIndex + 1].bFloorHit)
			{
				const FVector N = Hit.ImpactNormal.GetSafeNormal();
				const double DotUp = FVector::DotProduct(N, FVector::UpVector);
				if (DotUp >= WalkableCos)
				{
					const double MinFloorZ = FMath::Min((double)Samples[SegIndex].Feet.Z, (double)Samples[SegIndex + 1].Feet.Z) - FloorZTolerance;
					const double MaxFloorZ = FMath::Max((double)Samples[SegIndex].Feet.Z, (double)Samples[SegIndex + 1].Feet.Z) + FloorZTolerance;
					if ((double)Hit.ImpactPoint.Z >= MinFloorZ && (double)Hit.ImpactPoint.Z <= MaxFloorZ)
					{
						bIgnored = true;
					}
				}
			}

			if (bIgnored)
			{
				++IgnoredFloorHitCount;
				continue;
			}

			BlockingHit = Hit;
			break;
		}

		const bool bSegmentBlocked = BlockingHit.bBlockingHit;
		if (bSegmentBlocked && !bBlockingHitFound)
		{
			bBlockingHitFound = true;
			FirstBlockingSegmentIndex = SegIndex;
			FirstBlockingHit = BlockingHit;
		}

		if (bIncludeSegments)
		{
			TSharedPtr<FJsonObject> SegmentObj = MakeShared<FJsonObject>();
			SegmentObj->SetNumberField(TEXT("segment_index"), SegIndex);
			SegmentObj->SetObjectField(TEXT("start_center"), VecToJson(StartCenter));
			SegmentObj->SetObjectField(TEXT("end_center"), VecToJson(EndCenter));
			SegmentObj->SetNumberField(TEXT("hit_count"), Hits.Num());
			SegmentObj->SetNumberField(TEXT("ignored_floor_hit_count"), IgnoredFloorHitCount);
			SegmentObj->SetBoolField(TEXT("blocking_hit"), bSegmentBlocked);
			if (bSegmentBlocked)
			{
				SegmentObj->SetNumberField(TEXT("hit_time"), BlockingHit.Time);
				SegmentObj->SetBoolField(TEXT("start_penetrating"), BlockingHit.bStartPenetrating);
				if (bReturnPenetrationDepth && BlockingHit.bStartPenetrating)
				{
					SegmentObj->SetNumberField(TEXT("penetration_depth_cm"), BlockingHit.PenetrationDepth);
				}
				UeAgentLevelOps::MaybeAttachStartPenetratingAdvice(
					TEXT("level_sweep_capsule_path.segment"),
					BlockingHit.bStartPenetrating,
					bFindInitialOverlaps,
					true,
					FloorClearanceCm,
					SegmentObj);
				if (bIncludeActorFolderTags)
				{
					if (AActor* HitActor = BlockingHit.GetActor())
					{
						SegmentObj->SetStringField(TEXT("hit_actor_folder_path"), HitActor->GetFolderPath().ToString());

						TArray<TSharedPtr<FJsonValue>> TagValues;
						TagValues.Reserve(HitActor->Tags.Num());
						for (const FName& Tag : HitActor->Tags)
						{
							TagValues.Add(MakeShared<FJsonValueString>(Tag.ToString()));
						}
						SegmentObj->SetArrayField(TEXT("hit_actor_tags"), TagValues);
					}
				}
				UeAgentLevelOps::FillHitResultJson(BlockingHit, SegmentObj);
			}
			SegmentArr.Add(MakeShared<FJsonValueObject>(SegmentObj));
		}

		if (bSegmentBlocked && bStopOnBlockingHit)
		{
			break;
		}
	}

	OutData->SetBoolField(TEXT("path_valid"), true);
	OutData->SetNumberField(TEXT("segment_count"), FMath::Max(0, Samples.Num() - 1));
	OutData->SetBoolField(TEXT("blocking_hit"), bBlockingHitFound);
	OutData->SetNumberField(TEXT("first_blocking_segment_index"), FirstBlockingSegmentIndex);
	if (bBlockingHitFound)
	{
		OutData->SetNumberField(TEXT("first_hit_time"), FirstBlockingHit.Time);
		OutData->SetBoolField(TEXT("first_start_penetrating"), FirstBlockingHit.bStartPenetrating);
		if (bReturnPenetrationDepth && FirstBlockingHit.bStartPenetrating)
		{
			OutData->SetNumberField(TEXT("first_penetration_depth_cm"), FirstBlockingHit.PenetrationDepth);
		}
		UeAgentLevelOps::MaybeAttachStartPenetratingAdvice(
			TEXT("level_sweep_capsule_path"),
			FirstBlockingHit.bStartPenetrating,
			bFindInitialOverlaps,
			true,
			FloorClearanceCm,
			OutData);
		if (bIncludeActorFolderTags)
		{
			if (AActor* HitActor = FirstBlockingHit.GetActor())
			{
				OutData->SetStringField(TEXT("hit_actor_folder_path"), HitActor->GetFolderPath().ToString());

				TArray<TSharedPtr<FJsonValue>> TagValues;
				TagValues.Reserve(HitActor->Tags.Num());
				for (const FName& Tag : HitActor->Tags)
				{
					TagValues.Add(MakeShared<FJsonValueString>(Tag.ToString()));
				}
				OutData->SetArrayField(TEXT("hit_actor_tags"), TagValues);
			}
		}
		UeAgentLevelOps::FillHitResultJson(FirstBlockingHit, OutData);
	}

	if (bIncludeSamples)
	{
		TArray<TSharedPtr<FJsonValue>> SamplesArr;
		SamplesArr.Reserve(Samples.Num());
		for (const FSample& S : Samples)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetObjectField(TEXT("input"), VecToJson(S.Input));
			Obj->SetObjectField(TEXT("feet"), VecToJson(S.Feet));
			Obj->SetObjectField(TEXT("center"), VecToJson(S.Center));
			Obj->SetBoolField(TEXT("floor_hit"), S.bFloorHit);
			Obj->SetObjectField(TEXT("floor_normal"), VecToJson(S.FloorNormal));
			SamplesArr.Add(MakeShared<FJsonValueObject>(Obj));
		}
		OutData->SetArrayField(TEXT("samples"), SamplesArr);
	}
	if (bIncludeSegments)
	{
		OutData->SetArrayField(TEXT("segments"), SegmentArr);
	}

	return true;
}

bool FUeAgentHttpServer::CmdLevelCheckOverlaps(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* ChecksJson = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("checks"), ChecksJson) && ChecksJson && ChecksJson->Num() > 0)
	{
		bool bContinueOnError = false;
		JsonTryGetBool(Ctx.Params, TEXT("continue_on_error"), bContinueOnError);

		bool bStopOnOverlap = false;
		JsonTryGetBool(Ctx.Params, TEXT("stop_on_overlap"), bStopOnOverlap);

		int32 MaxItems = 4096;
		{
			double MaxItemsNumber = (double)MaxItems;
			JsonTryGetNumber(Ctx.Params, TEXT("max_items"), MaxItemsNumber);
			MaxItems = (int32)FMath::Clamp(MaxItemsNumber, 1.0, 20000.0);
		}

		ECollisionChannel DefaultTraceChannel = ECC_Visibility;
		if (!UeAgentLevelOps::ResolveTraceChannel(Ctx.Params, DefaultTraceChannel))
		{
			OutError = TEXT("unsupported_trace_channel");
			return false;
		}

		bool bDefaultTraceComplex = false;
		JsonTryGetBool(Ctx.Params, TEXT("trace_complex"), bDefaultTraceComplex);

		double DefaultLimitD = 100.0;
		JsonTryGetNumber(Ctx.Params, TEXT("limit"), DefaultLimitD);
		const int32 DefaultLimit = static_cast<int32>(FMath::Clamp(DefaultLimitD, 0.0, 5000.0));

		bool bDefaultIncludeOverlaps = true;
		JsonTryGetBool(Ctx.Params, TEXT("include_overlaps"), bDefaultIncludeOverlaps);

		bool bDefaultIncludeActorFolderTags = false;
		JsonTryGetBool(Ctx.Params, TEXT("include_actor_folder_tags"), bDefaultIncludeActorFolderTags);

		TArray<AActor*> GlobalIgnoredActors;
		const TArray<TSharedPtr<FJsonValue>>* GlobalIgnoreIds = nullptr;
		if (Ctx.Params->TryGetArrayField(TEXT("ignore_actor_ids"), GlobalIgnoreIds) && GlobalIgnoreIds)
		{
			for (const TSharedPtr<FJsonValue>& IgnoreValue : *GlobalIgnoreIds)
			{
				const FString IgnoreId = IgnoreValue.IsValid() ? IgnoreValue->AsString() : FString();
				if (IgnoreId.IsEmpty())
				{
					continue;
				}

				if (AActor* IgnoreActor = FindActorByNameOrLabel(World, IgnoreId))
				{
					GlobalIgnoredActors.Add(IgnoreActor);
				}
			}
		}
		UeAgentLevelOps::AppendIgnoredActorsFromFilters(World, Ctx.Params, GlobalIgnoredActors);

		const int32 InputCount = ChecksJson->Num();
		const int32 ExecuteCount = FMath::Min(InputCount, MaxItems);
		const bool bTruncatedByMaxItems = InputCount > ExecuteCount;

		int32 ErrorCount = 0;
		int32 FirstErrorIndex = INDEX_NONE;
		FString FirstError;
		int32 TotalOverlapCount = 0;
		int32 FirstOverlappingIndex = INDEX_NONE;

		TArray<TSharedPtr<FJsonValue>> Results;
		Results.Reserve(ExecuteCount);

		for (int32 Index = 0; Index < ExecuteCount; ++Index)
		{
			const TSharedPtr<FJsonValue>& CheckValue = (*ChecksJson)[Index];
			const TSharedPtr<FJsonObject> CheckObj = CheckValue.IsValid() ? CheckValue->AsObject() : nullptr;
			if (!CheckObj.IsValid())
			{
				++ErrorCount;
				if (FirstErrorIndex == INDEX_NONE)
				{
					FirstErrorIndex = Index;
					FirstError = TEXT("checks_invalid_item");
				}
				if (!bContinueOnError)
				{
					OutError = TEXT("checks_invalid_item");
					return false;
				}

				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("index"), Index);
				Item->SetBoolField(TEXT("ok"), false);
				Item->SetStringField(TEXT("error"), TEXT("checks_invalid_item"));
				Results.Add(MakeShared<FJsonValueObject>(Item));
				continue;
			}

			FString ShapeName;
			if (!JsonTryGetString(CheckObj, TEXT("shape"), ShapeName) || ShapeName.TrimStartAndEnd().IsEmpty())
			{
				++ErrorCount;
				if (FirstErrorIndex == INDEX_NONE)
				{
					FirstErrorIndex = Index;
					FirstError = TEXT("missing_shape");
				}
				if (!bContinueOnError)
				{
					OutError = TEXT("missing_shape");
					return false;
				}

				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("index"), Index);
				Item->SetBoolField(TEXT("ok"), false);
				Item->SetStringField(TEXT("error"), TEXT("missing_shape"));
				Results.Add(MakeShared<FJsonValueObject>(Item));
				continue;
			}

			FVector Center = FVector::ZeroVector;
			if (!JsonTryGetVector(CheckObj, TEXT("center"), Center))
			{
				++ErrorCount;
				if (FirstErrorIndex == INDEX_NONE)
				{
					FirstErrorIndex = Index;
					FirstError = TEXT("missing_center");
				}
				if (!bContinueOnError)
				{
					OutError = TEXT("missing_center");
					return false;
				}

				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("index"), Index);
				Item->SetBoolField(TEXT("ok"), false);
				Item->SetStringField(TEXT("error"), TEXT("missing_center"));
				Item->SetStringField(TEXT("shape"), ShapeName.TrimStartAndEnd().ToLower());
				Results.Add(MakeShared<FJsonValueObject>(Item));
				continue;
			}

			const FString ShapeNormalized = ShapeName.TrimStartAndEnd().ToLower();
			FCollisionShape Shape;
			FQuat RotationQuat = FQuat::Identity;

			if (ShapeNormalized == TEXT("box"))
			{
				FVector Extent = FVector::ZeroVector;
				if (!JsonTryGetVector(CheckObj, TEXT("box_extent"), Extent))
				{
					++ErrorCount;
					if (FirstErrorIndex == INDEX_NONE)
					{
						FirstErrorIndex = Index;
						FirstError = TEXT("missing_box_extent");
					}
					if (!bContinueOnError)
					{
						OutError = TEXT("missing_box_extent");
						return false;
					}

					TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetNumberField(TEXT("index"), Index);
					Item->SetBoolField(TEXT("ok"), false);
					Item->SetStringField(TEXT("error"), TEXT("missing_box_extent"));
					Item->SetStringField(TEXT("shape"), ShapeNormalized);
					Item->SetObjectField(TEXT("center"), VecToJson(Center));
					Results.Add(MakeShared<FJsonValueObject>(Item));
					continue;
				}
				Extent.X = (float)FMath::Max(0.1, (double)Extent.X);
				Extent.Y = (float)FMath::Max(0.1, (double)Extent.Y);
				Extent.Z = (float)FMath::Max(0.1, (double)Extent.Z);
				Shape = FCollisionShape::MakeBox(Extent);

				FRotator Rotation = FRotator::ZeroRotator;
				if (JsonTryGetRotator(CheckObj, TEXT("rotation"), Rotation))
				{
					RotationQuat = Rotation.Quaternion();
				}
			}
			else if (ShapeNormalized == TEXT("sphere"))
			{
				double RadiusCm = 0.0;
				if (!JsonTryGetNumber(CheckObj, TEXT("radius_cm"), RadiusCm))
				{
					++ErrorCount;
					if (FirstErrorIndex == INDEX_NONE)
					{
						FirstErrorIndex = Index;
						FirstError = TEXT("missing_radius_cm");
					}
					if (!bContinueOnError)
					{
						OutError = TEXT("missing_radius_cm");
						return false;
					}

					TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetNumberField(TEXT("index"), Index);
					Item->SetBoolField(TEXT("ok"), false);
					Item->SetStringField(TEXT("error"), TEXT("missing_radius_cm"));
					Item->SetStringField(TEXT("shape"), ShapeNormalized);
					Item->SetObjectField(TEXT("center"), VecToJson(Center));
					Results.Add(MakeShared<FJsonValueObject>(Item));
					continue;
				}
				RadiusCm = FMath::Clamp(RadiusCm, 0.1, 100000.0);
				Shape = FCollisionShape::MakeSphere((float)RadiusCm);
			}
			else if (ShapeNormalized == TEXT("capsule"))
			{
				double RadiusCm = 0.0;
				double HalfHeightCm = 0.0;
				if (!JsonTryGetNumber(CheckObj, TEXT("radius_cm"), RadiusCm))
				{
					++ErrorCount;
					if (FirstErrorIndex == INDEX_NONE)
					{
						FirstErrorIndex = Index;
						FirstError = TEXT("missing_radius_cm");
					}
					if (!bContinueOnError)
					{
						OutError = TEXT("missing_radius_cm");
						return false;
					}

					TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetNumberField(TEXT("index"), Index);
					Item->SetBoolField(TEXT("ok"), false);
					Item->SetStringField(TEXT("error"), TEXT("missing_radius_cm"));
					Item->SetStringField(TEXT("shape"), ShapeNormalized);
					Item->SetObjectField(TEXT("center"), VecToJson(Center));
					Results.Add(MakeShared<FJsonValueObject>(Item));
					continue;
				}
				if (!JsonTryGetNumber(CheckObj, TEXT("half_height_cm"), HalfHeightCm))
				{
					++ErrorCount;
					if (FirstErrorIndex == INDEX_NONE)
					{
						FirstErrorIndex = Index;
						FirstError = TEXT("missing_half_height_cm");
					}
					if (!bContinueOnError)
					{
						OutError = TEXT("missing_half_height_cm");
						return false;
					}

					TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetNumberField(TEXT("index"), Index);
					Item->SetBoolField(TEXT("ok"), false);
					Item->SetStringField(TEXT("error"), TEXT("missing_half_height_cm"));
					Item->SetStringField(TEXT("shape"), ShapeNormalized);
					Item->SetObjectField(TEXT("center"), VecToJson(Center));
					Item->SetNumberField(TEXT("radius_cm"), RadiusCm);
					Results.Add(MakeShared<FJsonValueObject>(Item));
					continue;
				}
				RadiusCm = FMath::Clamp(RadiusCm, 0.1, 100000.0);
				HalfHeightCm = FMath::Clamp(HalfHeightCm, 0.1, 100000.0);
				Shape = FCollisionShape::MakeCapsule((float)RadiusCm, (float)HalfHeightCm);

				FRotator Rotation = FRotator::ZeroRotator;
				if (JsonTryGetRotator(CheckObj, TEXT("rotation"), Rotation))
				{
					RotationQuat = Rotation.Quaternion();
				}
			}
			else
			{
				++ErrorCount;
				if (FirstErrorIndex == INDEX_NONE)
				{
					FirstErrorIndex = Index;
					FirstError = TEXT("unsupported_shape");
				}
				if (!bContinueOnError)
				{
					OutError = TEXT("unsupported_shape");
					return false;
				}

				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("index"), Index);
				Item->SetBoolField(TEXT("ok"), false);
				Item->SetStringField(TEXT("error"), TEXT("unsupported_shape"));
				Item->SetStringField(TEXT("shape"), ShapeNormalized);
				Item->SetObjectField(TEXT("center"), VecToJson(Center));
				Results.Add(MakeShared<FJsonValueObject>(Item));
				continue;
			}

			ECollisionChannel TraceChannel = DefaultTraceChannel;
			if (!UeAgentLevelOps::ResolveTraceChannel(CheckObj, TraceChannel))
			{
				++ErrorCount;
				if (FirstErrorIndex == INDEX_NONE)
				{
					FirstErrorIndex = Index;
					FirstError = TEXT("unsupported_trace_channel");
				}
				if (!bContinueOnError)
				{
					OutError = TEXT("unsupported_trace_channel");
					return false;
				}

				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("index"), Index);
				Item->SetBoolField(TEXT("ok"), false);
				Item->SetStringField(TEXT("error"), TEXT("unsupported_trace_channel"));
				Item->SetStringField(TEXT("shape"), ShapeNormalized);
				Item->SetObjectField(TEXT("center"), VecToJson(Center));
				Results.Add(MakeShared<FJsonValueObject>(Item));
				continue;
			}

			bool bTraceComplex = bDefaultTraceComplex;
			CheckObj->TryGetBoolField(TEXT("trace_complex"), bTraceComplex);

			double LimitD = (double)DefaultLimit;
			JsonTryGetNumber(CheckObj, TEXT("limit"), LimitD);
			const int32 Limit = static_cast<int32>(FMath::Clamp(LimitD, 0.0, 5000.0));

			bool bIncludeOverlaps = bDefaultIncludeOverlaps;
			CheckObj->TryGetBoolField(TEXT("include_overlaps"), bIncludeOverlaps);

			bool bIncludeActorFolderTags = bDefaultIncludeActorFolderTags;
			CheckObj->TryGetBoolField(TEXT("include_actor_folder_tags"), bIncludeActorFolderTags);

			FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(UeAgentCheckOverlapsBatch), bTraceComplex);
			for (AActor* Ignored : GlobalIgnoredActors)
			{
				if (Ignored)
				{
					QueryParams.AddIgnoredActor(Ignored);
				}
			}

			const TArray<TSharedPtr<FJsonValue>>* IgnoreIds = nullptr;
			if (CheckObj->TryGetArrayField(TEXT("ignore_actor_ids"), IgnoreIds) && IgnoreIds)
			{
				for (const TSharedPtr<FJsonValue>& IgnoreValue : *IgnoreIds)
				{
					const FString IgnoreId = IgnoreValue.IsValid() ? IgnoreValue->AsString() : FString();
					if (IgnoreId.IsEmpty())
					{
						continue;
					}

					if (AActor* IgnoreActor = FindActorByNameOrLabel(World, IgnoreId))
					{
						QueryParams.AddIgnoredActor(IgnoreActor);
					}
				}
			}

			TArray<FOverlapResult> OverlapResults;
			World->OverlapMultiByChannel(OverlapResults, Center, RotationQuat, TraceChannel, Shape, QueryParams);

			TotalOverlapCount += OverlapResults.Num();
			if (FirstOverlappingIndex == INDEX_NONE && OverlapResults.Num() > 0)
			{
				FirstOverlappingIndex = Index;
			}

			const bool bReturnOverlaps = bIncludeOverlaps && Limit > 0;
			TArray<TSharedPtr<FJsonValue>> Overlaps;
			if (bReturnOverlaps)
			{
				Overlaps.Reserve(FMath::Min(Limit, OverlapResults.Num()));
				for (const FOverlapResult& Result : OverlapResults)
				{
					if (Overlaps.Num() >= Limit)
					{
						break;
					}

					TSharedPtr<FJsonObject> OverlapItem = MakeShared<FJsonObject>();
					TSharedPtr<FJsonObject> ActorSummary = UeAgentLevelOps::MakeActorSummaryJson(Result.GetActor());
					if (bIncludeActorFolderTags && Result.GetActor())
					{
						UeAgentLevelOps::AugmentActorSummaryWithFolderTags(Result.GetActor(), ActorSummary);
					}
					OverlapItem->SetObjectField(TEXT("actor"), ActorSummary);
					if (Result.GetComponent())
					{
						OverlapItem->SetStringField(TEXT("component_name"), Result.GetComponent()->GetName());
						OverlapItem->SetStringField(TEXT("component_path"), Result.GetComponent()->GetPathName());
					}
					Overlaps.Add(MakeShared<FJsonValueObject>(OverlapItem));
				}
			}

			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetNumberField(TEXT("index"), Index);
			Item->SetBoolField(TEXT("ok"), true);
			Item->SetStringField(TEXT("shape"), ShapeNormalized);
			Item->SetObjectField(TEXT("center"), VecToJson(Center));
			Item->SetStringField(TEXT("trace_channel"), FString::Printf(TEXT("%d"), (int32)TraceChannel));
			Item->SetBoolField(TEXT("trace_complex"), bTraceComplex);
			Item->SetNumberField(TEXT("limit"), Limit);
			Item->SetBoolField(TEXT("include_overlaps"), bIncludeOverlaps);
			Item->SetBoolField(TEXT("overlaps_included"), bReturnOverlaps);
			Item->SetBoolField(TEXT("include_actor_folder_tags"), bIncludeActorFolderTags);
			Item->SetNumberField(TEXT("overlap_count"), OverlapResults.Num());
			if (bReturnOverlaps)
			{
				Item->SetArrayField(TEXT("overlaps"), Overlaps);
			}
			Results.Add(MakeShared<FJsonValueObject>(Item));

			if (bStopOnOverlap && OverlapResults.Num() > 0)
			{
				break;
			}
		}

		const bool bStoppedEarly = bStopOnOverlap && FirstOverlappingIndex != INDEX_NONE && (FirstOverlappingIndex < ExecuteCount - 1);

		OutData->SetStringField(TEXT("mode"), TEXT("batch"));
		OutData->SetNumberField(TEXT("input_check_count"), InputCount);
		OutData->SetNumberField(TEXT("check_count"), Results.Num());
		OutData->SetNumberField(TEXT("max_items"), MaxItems);
		OutData->SetBoolField(TEXT("truncated"), bTruncatedByMaxItems || bStoppedEarly);
		OutData->SetBoolField(TEXT("continue_on_error"), bContinueOnError);
		OutData->SetBoolField(TEXT("stop_on_overlap"), bStopOnOverlap);
		OutData->SetNumberField(TEXT("total_overlap_count"), TotalOverlapCount);
		OutData->SetNumberField(TEXT("first_overlapping_index"), FirstOverlappingIndex);
		OutData->SetNumberField(TEXT("error_count"), ErrorCount);
		OutData->SetNumberField(TEXT("first_error_index"), FirstErrorIndex);
		if (FirstErrorIndex != INDEX_NONE)
		{
			OutData->SetStringField(TEXT("first_error"), FirstError);
		}
		OutData->SetArrayField(TEXT("checks"), Results);
		return true;
	}

	FString ShapeName;
	if (!JsonTryGetString(Ctx.Params, TEXT("shape"), ShapeName) || ShapeName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_shape");
		return false;
	}

	FVector Center = FVector::ZeroVector;
	if (!JsonTryGetVector(Ctx.Params, TEXT("center"), Center))
	{
		OutError = TEXT("missing_center");
		return false;
	}

	const FString ShapeNormalized = ShapeName.TrimStartAndEnd().ToLower();
	FCollisionShape Shape;
	FQuat RotationQuat = FQuat::Identity;

	if (ShapeNormalized == TEXT("box"))
	{
		FVector Extent = FVector::ZeroVector;
		if (!JsonTryGetVector(Ctx.Params, TEXT("box_extent"), Extent))
		{
			OutError = TEXT("missing_box_extent");
			return false;
		}
		Extent.X = (float)FMath::Max(0.1, (double)Extent.X);
		Extent.Y = (float)FMath::Max(0.1, (double)Extent.Y);
		Extent.Z = (float)FMath::Max(0.1, (double)Extent.Z);
		Shape = FCollisionShape::MakeBox(Extent);

		FRotator Rotation = FRotator::ZeroRotator;
		if (JsonTryGetRotator(Ctx.Params, TEXT("rotation"), Rotation))
		{
			RotationQuat = Rotation.Quaternion();
		}
	}
	else if (ShapeNormalized == TEXT("sphere"))
	{
		double RadiusCm = 0.0;
		if (!JsonTryGetNumber(Ctx.Params, TEXT("radius_cm"), RadiusCm))
		{
			OutError = TEXT("missing_radius_cm");
			return false;
		}
		RadiusCm = FMath::Clamp(RadiusCm, 0.1, 100000.0);
		Shape = FCollisionShape::MakeSphere((float)RadiusCm);
	}
	else if (ShapeNormalized == TEXT("capsule"))
	{
		double RadiusCm = 0.0;
		double HalfHeightCm = 0.0;
		if (!JsonTryGetNumber(Ctx.Params, TEXT("radius_cm"), RadiusCm))
		{
			OutError = TEXT("missing_radius_cm");
			return false;
		}
		if (!JsonTryGetNumber(Ctx.Params, TEXT("half_height_cm"), HalfHeightCm))
		{
			OutError = TEXT("missing_half_height_cm");
			return false;
		}
		RadiusCm = FMath::Clamp(RadiusCm, 0.1, 100000.0);
		HalfHeightCm = FMath::Clamp(HalfHeightCm, 0.1, 100000.0);
		Shape = FCollisionShape::MakeCapsule((float)RadiusCm, (float)HalfHeightCm);

		FRotator Rotation = FRotator::ZeroRotator;
		if (JsonTryGetRotator(Ctx.Params, TEXT("rotation"), Rotation))
		{
			RotationQuat = Rotation.Quaternion();
		}
	}
	else
	{
		OutError = TEXT("unsupported_shape");
		return false;
	}

	ECollisionChannel TraceChannel = ECC_Visibility;
	if (!UeAgentLevelOps::ResolveTraceChannel(Ctx.Params, TraceChannel))
	{
		OutError = TEXT("unsupported_trace_channel");
		return false;
	}

	bool bTraceComplex = false;
	JsonTryGetBool(Ctx.Params, TEXT("trace_complex"), bTraceComplex);

	double LimitD = 100.0;
	JsonTryGetNumber(Ctx.Params, TEXT("limit"), LimitD);
	const int32 Limit = static_cast<int32>(FMath::Clamp(LimitD, 0.0, 5000.0));

	bool bIncludeOverlaps = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_overlaps"), bIncludeOverlaps);

	bool bIncludeActorFolderTags = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_actor_folder_tags"), bIncludeActorFolderTags);

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(UeAgentCheckOverlaps), bTraceComplex);

	TArray<AActor*> IgnoredActors;
	const TArray<TSharedPtr<FJsonValue>>* IgnoreIds = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("ignore_actor_ids"), IgnoreIds) && IgnoreIds)
	{
		for (const TSharedPtr<FJsonValue>& IgnoreValue : *IgnoreIds)
		{
			const FString IgnoreId = IgnoreValue.IsValid() ? IgnoreValue->AsString() : FString();
			if (IgnoreId.IsEmpty())
			{
				continue;
			}

			if (AActor* IgnoreActor = FindActorByNameOrLabel(World, IgnoreId))
			{
				IgnoredActors.Add(IgnoreActor);
			}
		}
	}
	UeAgentLevelOps::AppendIgnoredActorsFromFilters(World, Ctx.Params, IgnoredActors);
	for (AActor* Ignored : IgnoredActors)
	{
		if (Ignored)
		{
			QueryParams.AddIgnoredActor(Ignored);
		}
	}

	TArray<FOverlapResult> OverlapResults;
	World->OverlapMultiByChannel(OverlapResults, Center, RotationQuat, TraceChannel, Shape, QueryParams);

	const bool bReturnOverlaps = bIncludeOverlaps && Limit > 0;
	TArray<TSharedPtr<FJsonValue>> Overlaps;
	if (bReturnOverlaps)
	{
		Overlaps.Reserve(FMath::Min(Limit, OverlapResults.Num()));
		for (const FOverlapResult& Result : OverlapResults)
		{
			if (Overlaps.Num() >= Limit)
			{
				break;
			}

			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			TSharedPtr<FJsonObject> ActorSummary = UeAgentLevelOps::MakeActorSummaryJson(Result.GetActor());
			if (bIncludeActorFolderTags && Result.GetActor())
			{
				UeAgentLevelOps::AugmentActorSummaryWithFolderTags(Result.GetActor(), ActorSummary);
			}
			Item->SetObjectField(TEXT("actor"), ActorSummary);
			if (Result.GetComponent())
			{
				Item->SetStringField(TEXT("component_name"), Result.GetComponent()->GetName());
				Item->SetStringField(TEXT("component_path"), Result.GetComponent()->GetPathName());
			}
			Overlaps.Add(MakeShared<FJsonValueObject>(Item));
		}
	}

	OutData->SetStringField(TEXT("shape"), ShapeNormalized);
	OutData->SetObjectField(TEXT("center"), VecToJson(Center));
	OutData->SetStringField(TEXT("trace_channel"), FString::Printf(TEXT("%d"), (int32)TraceChannel));
	OutData->SetBoolField(TEXT("trace_complex"), bTraceComplex);
	OutData->SetNumberField(TEXT("limit"), Limit);
	OutData->SetBoolField(TEXT("include_overlaps"), bIncludeOverlaps);
	OutData->SetBoolField(TEXT("overlaps_included"), bReturnOverlaps);
	OutData->SetBoolField(TEXT("include_actor_folder_tags"), bIncludeActorFolderTags);
	OutData->SetNumberField(TEXT("overlap_count"), OverlapResults.Num());
	if (bReturnOverlaps)
	{
		OutData->SetArrayField(TEXT("overlaps"), Overlaps);
	}
	return true;
}

bool FUeAgentHttpServer::CmdViewportPickActorAtScreen(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentHttpServer::CmdViewportSelectActorAtScreen(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentHttpServer::CmdLevelGetNearbyActorObbs(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentHttpServer::CmdMeshGetClosestVertex(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentHttpServer::CmdMeshGetVertexWorldPosition(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentHttpServer::CmdLevelAlignActorVertexToVertex(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentHttpServer::CmdLevelAlignActorByBounds(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentHttpServer::CmdLevelAlignFaceToFace(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

