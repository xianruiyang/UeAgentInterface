namespace UeAgentAIOps
{
	static TSharedPtr<FJsonObject> ExportPerceptionComponent(UAIPerceptionComponent* Component)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		if (!Component)
		{
			return Root;
		}
		Root->SetStringField(TEXT("component"), Component->GetName());
		Root->SetStringField(TEXT("owner"), Component->GetOwner() ? Component->GetOwner()->GetName() : FString());
		Root->SetStringField(TEXT("class"), Component->GetClass()->GetPathName());
		Root->SetStringField(TEXT("dominant_sense"), Component->GetDominantSense() ? Component->GetDominantSense()->GetPathName() : FString());
		TArray<TSharedPtr<FJsonValue>> Senses;
		for (UAIPerceptionComponent::TAISenseConfigConstIterator It = Component->GetSensesConfigIterator(); It; ++It)
		{
			const UAISenseConfig* Config = *It;
			TSharedPtr<FJsonObject> SenseObj = MakeShared<FJsonObject>();
			SenseObj->SetStringField(TEXT("config_class"), Config ? Config->GetClass()->GetPathName() : FString());
			SenseObj->SetStringField(TEXT("sense_class"), (Config && Config->GetSenseImplementation()) ? Config->GetSenseImplementation()->GetPathName() : FString());
			SenseObj->SetStringField(TEXT("sense_name"), Config ? Config->GetSenseName() : FString());
			Senses.Add(MakeShared<FJsonValueObject>(SenseObj));
		}
		Root->SetArrayField(TEXT("senses"), Senses);
		Root->SetNumberField(TEXT("sense_count"), Senses.Num());
		return Root;
	}

	static TSharedPtr<FJsonObject> ExportStimuliSourceComponent(UAIPerceptionStimuliSourceComponent* Component)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		if (!Component)
		{
			return Root;
		}
		Root->SetStringField(TEXT("component"), Component->GetName());
		Root->SetStringField(TEXT("owner"), Component->GetOwner() ? Component->GetOwner()->GetName() : FString());
		Root->SetStringField(TEXT("class"), Component->GetClass()->GetPathName());
		bool bAutoRegisterAsSource = false;
		Root->SetBoolField(TEXT("auto_register_as_source"), ReadReflectedBool(Component->GetClass(), Component, TEXT("bAutoRegisterAsSource"), bAutoRegisterAsSource) ? bAutoRegisterAsSource : false);
		TArray<TSharedPtr<FJsonValue>> Senses;
		TArray<UClass*> SenseClasses;
		ReadReflectedClassArray(Component->GetClass(), Component, TEXT("RegisterAsSourceForSenses"), SenseClasses);
		for (UClass* SenseClass : SenseClasses)
		{
			Senses.Add(MakeShared<FJsonValueString>(SenseClass ? SenseClass->GetPathName() : FString()));
		}
		Root->SetArrayField(TEXT("register_as_source_for_senses"), Senses);
		return Root;
	}

	static TSharedPtr<FJsonObject> StimulusToJson(const FAIStimulus& Stimulus)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("successfully_sensed"), Stimulus.WasSuccessfullySensed());
		Obj->SetBoolField(TEXT("expired"), Stimulus.IsExpired());
		Obj->SetNumberField(TEXT("age"), Stimulus.GetAge());
		float ExpirationAge = 0.0f;
		Obj->SetNumberField(TEXT("expiration_age"), ReadReflectedFloat(FAIStimulus::StaticStruct(), &Stimulus, TEXT("ExpirationAge"), ExpirationAge) ? ExpirationAge : 0.0f);
		Obj->SetNumberField(TEXT("strength"), Stimulus.Strength);
		Obj->SetObjectField(TEXT("stimulus_location"), VecToJson(Stimulus.StimulusLocation));
		Obj->SetObjectField(TEXT("receiver_location"), VecToJson(Stimulus.ReceiverLocation));
		Obj->SetStringField(TEXT("tag"), Stimulus.Tag.ToString());
		return Obj;
	}

	static UAIPerceptionComponent* FindPerceptionComponentFromAssetOrActor(UWorld* World, const TSharedPtr<FJsonObject>& Params, FString& OutOwner, FString& OutError)
	{
		FString ActorName;
		FString BlueprintAssetPath;
		Params->TryGetStringField(TEXT("actor"), ActorName);
		Params->TryGetStringField(TEXT("blueprint_asset"), BlueprintAssetPath);
		if (!ActorName.IsEmpty())
		{
			AActor* Actor = nullptr;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (It->GetName().Equals(ActorName, ESearchCase::IgnoreCase) || It->GetActorLabel().Equals(ActorName, ESearchCase::IgnoreCase))
				{
					Actor = *It;
					break;
				}
			}
			if (!Actor)
			{
				OutError = TEXT("actor_not_found");
				return nullptr;
			}
			OutOwner = Actor->GetName();
			return Actor->FindComponentByClass<UAIPerceptionComponent>();
		}
		if (!BlueprintAssetPath.IsEmpty())
		{
			if (UBlueprint* Blueprint = LoadAsset<UBlueprint>(BlueprintAssetPath))
			{
				if (AActor* CDO = Blueprint->GeneratedClass ? Cast<AActor>(Blueprint->GeneratedClass->GetDefaultObject()) : nullptr)
				{
					OutOwner = Blueprint->GetPathName();
					return CDO->FindComponentByClass<UAIPerceptionComponent>();
				}
			}
			OutError = TEXT("blueprint_asset_not_found");
			return nullptr;
		}
		OutError = TEXT("missing_actor_or_blueprint_asset");
		return nullptr;
	}
}

bool FUeAgentHttpServer::CmdAIPerceptionGetComponentInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	FString Owner;
	UAIPerceptionComponent* Component = UeAgentAIOps::FindPerceptionComponentFromAssetOrActor(World, Ctx.Params, Owner, OutError);
	if (!Component)
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("ai_perception_component_not_found");
		}
		return false;
	}
	OutData = UeAgentAIOps::ExportPerceptionComponent(Component);
	OutData->SetStringField(TEXT("source_owner"), Owner);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

bool FUeAgentHttpServer::CmdAIPerceptionValidateSetup(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	OutData = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Issues;
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	FString Owner;
	UAIPerceptionComponent* Listener = UeAgentAIOps::FindPerceptionComponentFromAssetOrActor(World, Ctx.Params, Owner, OutError);
	if (!Listener)
	{
		UeAgentAIOps::AddIssue(Issues, TEXT("listener_component_not_found"), TEXT("AI Perception listener component was not found."), TEXT("$.actor"));
	}
	else
	{
		const TArray<TSharedPtr<FJsonValue>>* RequiredSenses = nullptr;
		if (Ctx.Params->TryGetArrayField(TEXT("required_senses"), RequiredSenses) && RequiredSenses)
		{
			TSet<FString> PresentSenses;
			for (UAIPerceptionComponent::TAISenseConfigConstIterator It = Listener->GetSensesConfigIterator(); It; ++It)
			{
				const UAISenseConfig* Config = *It;
				if (Config && Config->GetSenseImplementation())
				{
					PresentSenses.Add(Config->GetSenseImplementation()->GetPathName());
					PresentSenses.Add(Config->GetSenseName());
				}
			}
			for (const TSharedPtr<FJsonValue>& SenseValue : *RequiredSenses)
			{
				FString SenseText;
				if (SenseValue.IsValid() && SenseValue->TryGetString(SenseText) && !PresentSenses.Contains(SenseText))
				{
					UeAgentAIOps::AddIssue(Issues, TEXT("required_sense_missing"), FString::Printf(TEXT("Required sense '%s' is not configured."), *SenseText), TEXT("$.required_senses"));
				}
			}
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* StimuliSources = nullptr;
	TArray<TSharedPtr<FJsonValue>> SourcesOut;
	if (Ctx.Params->TryGetArrayField(TEXT("stimuli_sources"), StimuliSources) && StimuliSources)
	{
		for (const TSharedPtr<FJsonValue>& SourceValue : *StimuliSources)
		{
			FString SourceName;
			if (!SourceValue.IsValid() || !SourceValue->TryGetString(SourceName))
			{
				continue;
			}
			AActor* SourceActor = FindActorByNameOrLabel(World, SourceName);
			UAIPerceptionStimuliSourceComponent* SourceComp = SourceActor ? SourceActor->FindComponentByClass<UAIPerceptionStimuliSourceComponent>() : nullptr;
			if (!SourceActor || !SourceComp)
			{
				UeAgentAIOps::AddIssue(Issues, TEXT("stimuli_source_missing"), FString::Printf(TEXT("Stimuli source '%s' is missing component."), *SourceName), TEXT("$.stimuli_sources"));
			}
			else
			{
				SourcesOut.Add(MakeShared<FJsonValueObject>(UeAgentAIOps::ExportStimuliSourceComponent(SourceComp)));
			}
		}
	}
	OutData->SetObjectField(TEXT("listener"), Listener ? UeAgentAIOps::ExportPerceptionComponent(Listener) : MakeShared<FJsonObject>());
	OutData->SetArrayField(TEXT("stimuli_sources"), SourcesOut);
	OutData->SetStringField(TEXT("status"), Issues.Num() == 0 ? TEXT("ok") : TEXT("validate_failed"));
	UeAgentAIOps::SetDiagnostics(OutData, Issues);
	return Issues.Num() == 0;
}

bool FUeAgentHttpServer::CmdAIPerceptionRuntimeSnapshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	FString ActorName;
	JsonTryGetString(Ctx.Params, TEXT("controller"), ActorName);
	if (ActorName.IsEmpty())
	{
		JsonTryGetString(Ctx.Params, TEXT("pawn"), ActorName);
	}
	UActorComponent* FoundComponent = nullptr;
	AActor* Actor = UeAgentAIOps::ResolveActorOrFirst(World, ActorName, UAIPerceptionComponent::StaticClass(), FoundComponent);
	UAIPerceptionComponent* Component = Cast<UAIPerceptionComponent>(FoundComponent);
	if (!Component)
	{
		OutError = TEXT("ai_perception_component_not_found");
		return false;
	}
	OutData = UeAgentAIOps::ExportPerceptionComponent(Component);
	TArray<AActor*> KnownActors;
	TArray<AActor*> CurrentActors;
	Component->GetKnownPerceivedActors(nullptr, KnownActors);
	Component->GetCurrentlyPerceivedActors(nullptr, CurrentActors);
	TArray<TSharedPtr<FJsonValue>> KnownJson;
	for (AActor* KnownActor : KnownActors)
	{
		TSharedPtr<FJsonObject> ActorObj = MakeShared<FJsonObject>();
		ActorObj->SetStringField(TEXT("name"), KnownActor ? KnownActor->GetName() : FString());
		ActorObj->SetStringField(TEXT("label"), KnownActor ? KnownActor->GetActorLabel() : FString());
		if (KnownActor)
		{
			FActorPerceptionBlueprintInfo Info;
			if (Component->GetActorsPerception(KnownActor, Info))
			{
				TArray<TSharedPtr<FJsonValue>> Stimuli;
				for (const FAIStimulus& Stimulus : Info.LastSensedStimuli)
				{
					Stimuli.Add(MakeShared<FJsonValueObject>(UeAgentAIOps::StimulusToJson(Stimulus)));
				}
				ActorObj->SetArrayField(TEXT("last_sensed_stimuli"), Stimuli);
			}
		}
		KnownJson.Add(MakeShared<FJsonValueObject>(ActorObj));
	}
	TArray<TSharedPtr<FJsonValue>> CurrentJson;
	for (AActor* CurrentActor : CurrentActors)
	{
		CurrentJson.Add(MakeShared<FJsonValueString>(CurrentActor ? CurrentActor->GetName() : FString()));
	}
	OutData->SetStringField(TEXT("snapshot_owner"), Actor ? Actor->GetName() : FString());
	OutData->SetArrayField(TEXT("known_actors"), KnownJson);
	OutData->SetArrayField(TEXT("currently_perceived_actors"), CurrentJson);
	OutData->SetNumberField(TEXT("known_actor_count"), KnownActors.Num());
	OutData->SetNumberField(TEXT("currently_perceived_actor_count"), CurrentActors.Num());
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

bool FUeAgentHttpServer::CmdAIPerceptionRuntimeProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!CmdAIPerceptionRuntimeSnapshot(Ctx, OutData, OutError))
	{
		return false;
	}
	FString StimulusActorName;
	JsonTryGetString(Ctx.Params, TEXT("stimulus_actor"), StimulusActorName);
	if (StimulusActorName.IsEmpty())
	{
		JsonTryGetString(Ctx.Params, TEXT("target_actor"), StimulusActorName);
	}
	bool bExpectedSensed = true;
	JsonTryGetBool(Ctx.Params, TEXT("expected_sensed"), bExpectedSensed);
	bool bFound = false;
	TArray<TSharedPtr<FJsonValue>> TargetStimuli;
	const TArray<TSharedPtr<FJsonValue>>* CurrentActors = nullptr;
	if (OutData->TryGetArrayField(TEXT("currently_perceived_actors"), CurrentActors) && CurrentActors)
	{
		for (const TSharedPtr<FJsonValue>& Value : *CurrentActors)
		{
			FString Name;
			if (Value.IsValid() && Value->TryGetString(Name) && Name.Equals(StimulusActorName, ESearchCase::IgnoreCase))
			{
				bFound = true;
				break;
			}
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* KnownActors = nullptr;
	if (!StimulusActorName.IsEmpty() && OutData->TryGetArrayField(TEXT("known_actors"), KnownActors) && KnownActors)
	{
		for (const TSharedPtr<FJsonValue>& Value : *KnownActors)
		{
			const TSharedPtr<FJsonObject>* ActorObj = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(ActorObj) || !ActorObj || !ActorObj->IsValid())
			{
				continue;
			}
			const FString Name = (*ActorObj)->GetStringField(TEXT("name"));
			const FString Label = (*ActorObj)->GetStringField(TEXT("label"));
			if (Name.Equals(StimulusActorName, ESearchCase::IgnoreCase) || Label.Equals(StimulusActorName, ESearchCase::IgnoreCase))
			{
				const TArray<TSharedPtr<FJsonValue>>* Stimuli = nullptr;
				if ((*ActorObj)->TryGetArrayField(TEXT("last_sensed_stimuli"), Stimuli) && Stimuli)
				{
					TargetStimuli = *Stimuli;
				}
				break;
			}
		}
	}
	TSharedPtr<FJsonObject> Sample = MakeShared<FJsonObject>();
	Sample->SetNumberField(TEXT("known_actor_count"), OutData->GetNumberField(TEXT("known_actor_count")));
	Sample->SetNumberField(TEXT("currently_perceived_actor_count"), OutData->GetNumberField(TEXT("currently_perceived_actor_count")));
	Sample->SetArrayField(TEXT("currently_perceived_actors"), CurrentActors ? *CurrentActors : TArray<TSharedPtr<FJsonValue>>());
	TArray<TSharedPtr<FJsonValue>> Samples;
	Samples.Add(MakeShared<FJsonValueObject>(Sample));
	OutData->SetStringField(TEXT("stimulus_actor"), StimulusActorName);
	OutData->SetStringField(TEXT("target_actor"), StimulusActorName);
	OutData->SetArrayField(TEXT("target_stimuli"), TargetStimuli);
	OutData->SetArrayField(TEXT("samples"), Samples);
	OutData->SetBoolField(TEXT("successfully_sensed"), bFound);
	OutData->SetBoolField(TEXT("expected_sensed"), bExpectedSensed);
	OutData->SetBoolField(TEXT("probe_passed"), StimulusActorName.IsEmpty() || (bFound == bExpectedSensed));
	return true;
}

bool FUeAgentHttpServer::CmdNavigationGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!NavSys)
	{
		OutError = TEXT("navigation_system_not_found");
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("world"), World->GetName());
	OutData->SetBoolField(TEXT("build_in_progress"), NavSys->IsNavigationBuildInProgress());
	TArray<TSharedPtr<FJsonValue>> NavDataItems;
	for (TActorIterator<ANavigationData> It(World); It; ++It)
	{
		ANavigationData* NavData = *It;
		if (!IsValid(NavData))
		{
			continue;
		}
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), NavData->GetName());
		Item->SetStringField(TEXT("class"), NavData->GetClass()->GetPathName());
		Item->SetObjectField(TEXT("location"), VecToJson(NavData->GetActorLocation()));
		NavDataItems.Add(MakeShared<FJsonValueObject>(Item));
	}
	TArray<TSharedPtr<FJsonValue>> BoundsItems;
	for (TActorIterator<ANavMeshBoundsVolume> It(World); It; ++It)
	{
		ANavMeshBoundsVolume* Volume = *It;
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Volume->GetName());
		Item->SetStringField(TEXT("label"), Volume->GetActorLabel());
		Item->SetObjectField(TEXT("location"), VecToJson(Volume->GetActorLocation()));
		Item->SetObjectField(TEXT("extent"), VecToJson(Volume->GetComponentsBoundingBox(true).GetExtent()));
		BoundsItems.Add(MakeShared<FJsonValueObject>(Item));
	}
	OutData->SetArrayField(TEXT("nav_data"), NavDataItems);
	OutData->SetArrayField(TEXT("bounds_volumes"), BoundsItems);
	OutData->SetNumberField(TEXT("nav_data_count"), NavDataItems.Num());
	OutData->SetNumberField(TEXT("bounds_volume_count"), BoundsItems.Num());
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

bool FUeAgentHttpServer::CmdNavigationExportConfigJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!CmdNavigationGetInfo(Ctx, OutData, OutError))
	{
		return false;
	}
	FString OutputFile;
	JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile);
	if (!OutputFile.TrimStartAndEnd().IsEmpty())
	{
		FString SavedPath;
		if (!UeAgentAIOps::SaveJsonFile(OutputFile, OutData, SavedPath, OutError))
		{
			return false;
		}
		OutData->SetStringField(TEXT("output_file"), SavedPath);
	}
	UeAgentAIOps::AttachCoverageReport(OutData);
	return true;
}

bool FUeAgentHttpServer::CmdNavigationValidateLevel(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	const TArray<TSharedPtr<FJsonValue>>* SamplePoints = nullptr;
	if (Ctx.Params->TryGetArrayField(TEXT("nodes"), SamplePoints) || Ctx.Params->TryGetArrayField(TEXT("sample_points"), SamplePoints))
	{
		return CmdLevelValidateConnectivity(Ctx, OutData, OutError);
	}
	if (!CmdNavigationGetInfo(Ctx, OutData, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	const int32 NavDataCount = static_cast<int32>(OutData->GetNumberField(TEXT("nav_data_count")));
	const int32 BoundsCount = static_cast<int32>(OutData->GetNumberField(TEXT("bounds_volume_count")));
	bool bRequiredBounds = true;
	JsonTryGetBool(Ctx.Params, TEXT("required_bounds"), bRequiredBounds);
	if (NavDataCount <= 0)
	{
		UeAgentAIOps::AddIssue(Issues, TEXT("nav_data_missing"), TEXT("No NavigationData actor found."), TEXT("$.nav_data"));
	}
	if (bRequiredBounds && BoundsCount <= 0)
	{
		UeAgentAIOps::AddIssue(Issues, TEXT("nav_bounds_missing"), TEXT("No NavMeshBoundsVolume found."), TEXT("$.bounds_volumes"));
	}
	OutData->SetStringField(TEXT("status"), Issues.Num() == 0 ? TEXT("ok") : TEXT("validate_failed"));
	UeAgentAIOps::SetDiagnostics(OutData, Issues);
	return Issues.Num() == 0;
}

bool FUeAgentHttpServer::CmdNavigationPathProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	const TArray<TSharedPtr<FJsonValue>>* Targets = nullptr;
	if (!Ctx.Params->TryGetArrayField(TEXT("targets"), Targets) || !Targets)
	{
		return CmdNavmeshFindPath(Ctx, OutData, OutError);
	}
	FVector Start = FVector::ZeroVector;
	if (!JsonTryGetVector(Ctx.Params, TEXT("start"), Start))
	{
		OutError = TEXT("missing_start");
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Results;
	bool bAllFound = true;
	for (int32 Index = 0; Index < Targets->Num(); ++Index)
	{
		const TSharedPtr<FJsonObject>* TargetObjPtr = nullptr;
		if (!(*Targets)[Index].IsValid() || !(*Targets)[Index]->TryGetObject(TargetObjPtr) || !TargetObjPtr || !TargetObjPtr->IsValid())
		{
			continue;
		}
		FVector End = FVector::ZeroVector;
		UeAgentAIOps::JsonTryGetVector(*TargetObjPtr, TEXT("location"), End);
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("start"), VecToJson(Start));
		Params->SetObjectField(TEXT("end"), VecToJson(End));
		bool bAllowPartial = false;
		JsonTryGetBool(Ctx.Params, TEXT("accept_partial_path"), bAllowPartial);
		Params->SetBoolField(TEXT("allow_partial"), bAllowPartial);
		FUeAgentRequestContext ChildCtx = Ctx;
		ChildCtx.Params = Params;
		TSharedPtr<FJsonObject> PathData = MakeShared<FJsonObject>();
		FString PathError;
		const bool bOk = CmdNavmeshFindPath(ChildCtx, PathData, PathError);
		PathData->SetNumberField(TEXT("target_index"), Index);
		if (!bOk)
		{
			PathData->SetStringField(TEXT("error"), PathError);
			bAllFound = false;
		}
		else
		{
			bool bPathFound = false;
			PathData->TryGetBoolField(TEXT("path_found"), bPathFound);
			bAllFound = bAllFound && bPathFound;
		}
		Results.Add(MakeShared<FJsonValueObject>(PathData));
	}
	OutData->SetObjectField(TEXT("start"), VecToJson(Start));
	OutData->SetArrayField(TEXT("results"), Results);
	OutData->SetBoolField(TEXT("all_paths_found"), bAllFound);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

bool FUeAgentHttpServer::CmdNavigationAreaCostProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	bool bRunPathProbe = Ctx.Params->HasField(TEXT("start")) && (Ctx.Params->HasField(TEXT("end")) || Ctx.Params->HasField(TEXT("target")));
	JsonTryGetBool(Ctx.Params, TEXT("run_path_probe"), bRunPathProbe);
	if (bRunPathProbe)
	{
		if (!CmdNavmeshFindPath(Ctx, OutData, OutError))
		{
			return false;
		}
	}
	else
	{
		OutData = MakeShared<FJsonObject>();
	}
	const TArray<TSharedPtr<FJsonValue>>* ExpectedAreas = nullptr;
	TArray<TSharedPtr<FJsonValue>> AreasOut;
	if (Ctx.Params->TryGetArrayField(TEXT("expected_areas"), ExpectedAreas) && ExpectedAreas)
	{
		for (const TSharedPtr<FJsonValue>& AreaValue : *ExpectedAreas)
		{
			FString AreaClassPath;
			if (!AreaValue.IsValid() || !AreaValue->TryGetString(AreaClassPath))
			{
				continue;
			}
			UClass* AreaClass = UeAgentAIOps::ResolveClass(AreaClassPath);
			TSharedPtr<FJsonObject> AreaObj = MakeShared<FJsonObject>();
			AreaObj->SetStringField(TEXT("class"), AreaClass ? AreaClass->GetPathName() : AreaClassPath);
			AreaObj->SetBoolField(TEXT("found"), AreaClass && AreaClass->IsChildOf(UNavArea::StaticClass()));
			if (AreaClass && AreaClass->IsChildOf(UNavArea::StaticClass()))
			{
				const UNavArea* AreaCDO = Cast<UNavArea>(AreaClass->GetDefaultObject());
				AreaObj->SetNumberField(TEXT("default_cost"), AreaCDO ? AreaCDO->DefaultCost : 0.0);
				float FixedAreaEnteringCost = 0.0f;
				AreaObj->SetNumberField(TEXT("fixed_area_entering_cost"), (AreaCDO && UeAgentAIOps::ReadReflectedFloat(AreaCDO->GetClass(), AreaCDO, TEXT("FixedAreaEnteringCost"), FixedAreaEnteringCost)) ? FixedAreaEnteringCost : 0.0f);
			}
			AreasOut.Add(MakeShared<FJsonValueObject>(AreaObj));
		}
	}
	OutData->SetArrayField(TEXT("area_costs"), AreasOut);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

bool FUeAgentHttpServer::CmdNavigationRuntimeSnapshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return CmdNavigationGetInfo(Ctx, OutData, OutError);
}
