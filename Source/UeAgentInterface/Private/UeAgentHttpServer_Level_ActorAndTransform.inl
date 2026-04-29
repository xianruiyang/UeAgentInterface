bool FUeAgentHttpServer::CmdGetWorldState(TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	UPackage* Pkg = World->GetOutermost();
	OutData->SetStringField(TEXT("map_name"), World->GetMapName());
	OutData->SetStringField(TEXT("package_name"), Pkg ? Pkg->GetName() : TEXT(""));
	OutData->SetBoolField(TEXT("dirty"), Pkg ? Pkg->IsDirty() : false);
	return true;
}

bool FUeAgentHttpServer::CmdListActors(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	const int32 Limit = (Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("limit"))) ? (int32)Ctx.Params->GetNumberField(TEXT("limit")) : 200;
	TArray<TSharedPtr<FJsonValue>> Arr;
	int32 Count = 0;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* A = *It;
		if (!A)
		{
			continue;
		}

		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), A->GetName());
		Item->SetStringField(TEXT("label"), A->GetActorLabel());
		Item->SetStringField(TEXT("class"), A->GetClass() ? A->GetClass()->GetPathName() : TEXT(""));
		Item->SetObjectField(TEXT("location"), VecToJson(A->GetActorLocation()));
		Item->SetObjectField(TEXT("rotation"), RotToJson(A->GetActorRotation()));
		Item->SetObjectField(TEXT("scale"), VecToJson(A->GetActorScale3D()));
		Arr.Add(MakeShared<FJsonValueObject>(Item));
		Count++;
		if (Count >= Limit)
		{
			break;
		}
	}

	OutData->SetNumberField(TEXT("count"), Count);
	OutData->SetArrayField(TEXT("actors"), Arr);
	return true;
}

bool FUeAgentHttpServer::CmdActorListComponents(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	bool bIncludeNonScene = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_non_scene"), bIncludeNonScene);

	TArray<UActorComponent*> Components;
	Actor->GetComponents(Components);
	TArray<TSharedPtr<FJsonValue>> Arr;
	for (UActorComponent* Comp : Components)
	{
		if (!Comp)
		{
			continue;
		}

		USceneComponent* SceneComp = Cast<USceneComponent>(Comp);
		if (!bIncludeNonScene && !SceneComp)
		{
			continue;
		}

		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Comp->GetName());
		Item->SetStringField(TEXT("path"), Comp->GetPathName());
		Item->SetStringField(TEXT("class"), Comp->GetClass() ? Comp->GetClass()->GetPathName() : TEXT(""));
		Item->SetBoolField(TEXT("registered"), Comp->IsRegistered());
		Item->SetNumberField(TEXT("creation_method"), (int32)Comp->CreationMethod);
		Item->SetBoolField(TEXT("is_scene_component"), SceneComp != nullptr);

		if (SceneComp)
		{
			Item->SetObjectField(TEXT("relative_location"), VecToJson(SceneComp->GetRelativeLocation()));
			Item->SetObjectField(TEXT("relative_rotation"), RotToJson(SceneComp->GetRelativeRotation()));
			Item->SetObjectField(TEXT("relative_scale"), VecToJson(SceneComp->GetRelativeScale3D()));
			Item->SetObjectField(TEXT("world_location"), VecToJson(SceneComp->GetComponentLocation()));
			Item->SetObjectField(TEXT("world_rotation"), RotToJson(SceneComp->GetComponentRotation()));
			Item->SetObjectField(TEXT("world_scale"), VecToJson(SceneComp->GetComponentScale()));
			Item->SetStringField(TEXT("attach_parent"), SceneComp->GetAttachParent() ? SceneComp->GetAttachParent()->GetName() : TEXT(""));
		}

		Arr.Add(MakeShared<FJsonValueObject>(Item));
	}

	OutData->SetStringField(TEXT("actor_name"), Actor->GetName());
	OutData->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
	OutData->SetNumberField(TEXT("count"), Arr.Num());
	OutData->SetArrayField(TEXT("components"), Arr);
	return true;
}

bool FUeAgentHttpServer::CmdLevelGetActorProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FString Id;
	FString PropertyName;
	if (!JsonTryGetString(Ctx.Params, TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("missing_id");
		return false;
	}
	if (!JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		OutError = TEXT("missing_property_name");
		return false;
	}

	FUeAgentInterfaceLogger::Log(TEXT("CmdLevelGetActorProperty req=%s actor=%s property=%s"), *Ctx.RequestId, *Id, *PropertyName);

	AActor* Actor = FindActorByNameOrLabel(World, Id);
	if (!Actor)
	{
		OutError = TEXT("actor_not_found");
		return false;
	}

	FString ValueText;
	FString PropertyClass;
	if (!UeAgentLevelOps::ExportResolvedProperty(Actor, PropertyName, ValueText, PropertyClass, OutError))
	{
		FUeAgentInterfaceLogger::Log(TEXT("CmdLevelGetActorProperty req=%s failed actor=%s property=%s error=%s"), *Ctx.RequestId, *Id, *PropertyName, *OutError);
		return false;
	}

	OutData->SetStringField(TEXT("actor_name"), Actor->GetName());
	OutData->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
	OutData->SetStringField(TEXT("property_name"), PropertyName);
	OutData->SetStringField(TEXT("property_class"), PropertyClass);
	OutData->SetStringField(TEXT("value_text"), ValueText);
	return true;
}

bool FUeAgentHttpServer::CmdActorSetProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
	FString PropertyName;
	if (!JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		OutError = TEXT("missing_property_name");
		return false;
	}
	FString ValueText;
	if (!JsonTryGetString(Ctx.Params, TEXT("value_text"), ValueText))
	{
		OutError = TEXT("missing_value_text");
		return false;
	}

	AActor* Actor = FindActorByNameOrLabel(World, Id);
	if (!Actor)
	{
		OutError = TEXT("actor_not_found");
		return false;
	}

	FProperty* Property = nullptr;
	void* ValuePtr = nullptr;
	if (!UeAgentLevelOps::ResolvePropertyPath(Actor, PropertyName, Property, ValuePtr) || !Property || !ValuePtr)
	{
		OutError = TEXT("property_not_found");
		return false;
	}

	Actor->Modify();
	if (!Property->ImportText_Direct(*ValueText, ValuePtr, Actor, PPF_None))
	{
		const FString ImportError = FString::Printf(TEXT("property_import_failed:%s:%s"), *PropertyName, *ValueText);
		OutData->SetStringField(TEXT("actor_name"), Actor->GetName());
		OutData->SetStringField(TEXT("property_name"), PropertyName);
		OutData->SetStringField(TEXT("value_text"), ValueText);
		SetPropertyImportResultFields(OutData, Property, ValueText, TEXT(""), TEXT("import_failed"), ImportError);
		OutError = ImportError;
		return false;
	}

	Actor->PostEditChange();
	Actor->MarkPackageDirty();

	FString ExportedValue;
	Property->ExportTextItem_Direct(ExportedValue, ValuePtr, nullptr, Actor, PPF_None);
	OutData->SetStringField(TEXT("actor_name"), Actor->GetName());
	OutData->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
	OutData->SetStringField(TEXT("property_name"), PropertyName);
	OutData->SetStringField(TEXT("value_text"), ValueText);
	OutData->SetStringField(TEXT("applied_value_text"), ExportedValue);
	SetPropertyImportResultFields(OutData, Property, ValueText, ExportedValue, TEXT("imported_and_read_back"));
	return true;
}

bool FUeAgentHttpServer::CmdLevelGetComponentProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FString Id;
	FString ComponentId;
	FString PropertyName;
	if (!JsonTryGetString(Ctx.Params, TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("missing_id");
		return false;
	}
	if (!JsonTryGetString(Ctx.Params, TEXT("component"), ComponentId) || ComponentId.IsEmpty())
	{
		JsonTryGetString(Ctx.Params, TEXT("component_id"), ComponentId);
	}
	if (ComponentId.IsEmpty())
	{
		OutError = TEXT("missing_component");
		return false;
	}
	if (!JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		OutError = TEXT("missing_property_name");
		return false;
	}

	FUeAgentInterfaceLogger::Log(TEXT("CmdLevelGetComponentProperty req=%s actor=%s component=%s property=%s"), *Ctx.RequestId, *Id, *ComponentId, *PropertyName);

	AActor* Actor = FindActorByNameOrLabel(World, Id);
	if (!Actor)
	{
		OutError = TEXT("actor_not_found");
		return false;
	}

	UActorComponent* Component = UeAgentLevelOps::FindComponentByNameOrPath(Actor, ComponentId);
	if (!Component)
	{
		OutError = TEXT("component_not_found");
		return false;
	}

	FString ValueText;
	FString PropertyClass;
	if (!UeAgentLevelOps::ExportResolvedProperty(Component, PropertyName, ValueText, PropertyClass, OutError))
	{
		FUeAgentInterfaceLogger::Log(TEXT("CmdLevelGetComponentProperty req=%s failed actor=%s component=%s property=%s error=%s"), *Ctx.RequestId, *Id, *ComponentId, *PropertyName, *OutError);
		return false;
	}

	OutData->SetStringField(TEXT("actor_name"), Actor->GetName());
	OutData->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
	OutData->SetStringField(TEXT("component_name"), Component->GetName());
	OutData->SetStringField(TEXT("component_path"), Component->GetPathName());
	OutData->SetStringField(TEXT("property_name"), PropertyName);
	OutData->SetStringField(TEXT("property_class"), PropertyClass);
	OutData->SetStringField(TEXT("value_text"), ValueText);
	return true;
}

bool FUeAgentHttpServer::CmdComponentSetProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
	FString ComponentId;
	if (!JsonTryGetString(Ctx.Params, TEXT("component"), ComponentId) || ComponentId.IsEmpty())
	{
		JsonTryGetString(Ctx.Params, TEXT("component_id"), ComponentId);
	}
	if (ComponentId.IsEmpty())
	{
		OutError = TEXT("missing_component");
		return false;
	}
	FString PropertyName;
	if (!JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		OutError = TEXT("missing_property_name");
		return false;
	}
	FString ValueText;
	if (!JsonTryGetString(Ctx.Params, TEXT("value_text"), ValueText))
	{
		OutError = TEXT("missing_value_text");
		return false;
	}

	AActor* Actor = FindActorByNameOrLabel(World, Id);
	if (!Actor)
	{
		OutError = TEXT("actor_not_found");
		return false;
	}

	UActorComponent* Component = UeAgentLevelOps::FindComponentByNameOrPath(Actor, ComponentId);
	if (!Component)
	{
		OutError = TEXT("component_not_found");
		return false;
	}

	FProperty* Property = nullptr;
	void* ValuePtr = nullptr;
	if (!UeAgentLevelOps::ResolvePropertyPath(Component, PropertyName, Property, ValuePtr) || !Property || !ValuePtr)
	{
		OutError = TEXT("property_not_found");
		return false;
	}

	Actor->Modify();
	Component->Modify();

	// Special-case: setting StaticMesh via ImportText does not reliably rebuild render/physics state, leading to
	// "looks correct but trace/overlap can't hit it" issues when spawning blockout meshes.
	if (UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(Component))
	{
		if (PropertyName.Equals(TEXT("StaticMesh"), ESearchCase::IgnoreCase))
		{
			auto NormalizeObjectPath = [](const FString& InText) -> FString
			{
				FString S = InText.TrimStartAndEnd();
				int32 FirstQuote = INDEX_NONE;
				if (S.FindChar(TEXT('\''), FirstQuote))
				{
					int32 LastQuote = INDEX_NONE;
					if (S.FindLastChar(TEXT('\''), LastQuote) && LastQuote > FirstQuote)
					{
						return S.Mid(FirstQuote + 1, LastQuote - FirstQuote - 1).TrimStartAndEnd();
					}
				}
				if (S.Len() >= 2 && ((S[0] == TEXT('"') && S[S.Len() - 1] == TEXT('"')) || (S[0] == TEXT('\'') && S[S.Len() - 1] == TEXT('\''))))
				{
					return S.Mid(1, S.Len() - 2).TrimStartAndEnd();
				}
				return S;
			};

			const FString MeshPath = NormalizeObjectPath(ValueText);
			UStaticMesh* Mesh = nullptr;
			if (!MeshPath.IsEmpty() && !MeshPath.Equals(TEXT("None"), ESearchCase::IgnoreCase))
			{
				Mesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);
				if (!Mesh)
				{
					OutError = FString::Printf(TEXT("failed_to_load_static_mesh: %s"), *MeshPath);
					return false;
				}
			}

			SMC->SetStaticMesh(Mesh);
			SMC->RecreatePhysicsState();
			SMC->UpdateBounds();
			SMC->MarkRenderStateDirty();
			SMC->MarkRenderTransformDirty();
			SMC->PostEditChange();
			Actor->MarkPackageDirty();

			FString ExportedValue;
			Property->ExportTextItem_Direct(ExportedValue, ValuePtr, nullptr, Component, PPF_None);
			OutData->SetStringField(TEXT("actor_name"), Actor->GetName());
			OutData->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
			OutData->SetStringField(TEXT("component"), Component->GetName());
			OutData->SetStringField(TEXT("component_path"), Component->GetPathName());
			OutData->SetStringField(TEXT("property_name"), PropertyName);
			OutData->SetStringField(TEXT("value_text"), ValueText);
			OutData->SetStringField(TEXT("applied_value_text"), ExportedValue);
			SetPropertyImportResultFields(OutData, Property, ValueText, ExportedValue, TEXT("assigned_object_reference"));
			OutData->SetStringField(TEXT("resolved_object_path"), MeshPath);
			return true;
		}
	}

	if (!Property->ImportText_Direct(*ValueText, ValuePtr, Component, PPF_None))
	{
		const FString ImportError = FString::Printf(TEXT("property_import_failed:%s:%s"), *PropertyName, *ValueText);
		OutData->SetStringField(TEXT("actor_name"), Actor->GetName());
		OutData->SetStringField(TEXT("component"), Component->GetName());
		OutData->SetStringField(TEXT("property_name"), PropertyName);
		OutData->SetStringField(TEXT("value_text"), ValueText);
		SetPropertyImportResultFields(OutData, Property, ValueText, TEXT(""), TEXT("import_failed"), ImportError);
		OutError = ImportError;
		return false;
	}

	Component->PostEditChange();
	Component->MarkRenderStateDirty();

	bool bPrimitiveCollisionStateRefreshed = false;
	if (UPrimitiveComponent* PrimitiveComponent = Cast<UPrimitiveComponent>(Component))
	{
		const FString NormalizedPropertyName = PropertyName.TrimStartAndEnd();
		const bool bTouchesCollisionState =
			NormalizedPropertyName.StartsWith(TEXT("BodyInstance."), ESearchCase::IgnoreCase)
			|| NormalizedPropertyName.Contains(TEXT("Collision"), ESearchCase::IgnoreCase)
			|| NormalizedPropertyName.Equals(TEXT("CanCharacterStepUpOn"), ESearchCase::IgnoreCase);
		if (bTouchesCollisionState)
		{
			PrimitiveComponent->RecreatePhysicsState();
			PrimitiveComponent->UpdateBounds();
			PrimitiveComponent->MarkRenderStateDirty();
			bPrimitiveCollisionStateRefreshed = true;
		}
	}

	if (UNiagaraComponent* NiagaraComponent = Cast<UNiagaraComponent>(Component))
	{
		NiagaraComponent->ReinitializeSystem();
		NiagaraComponent->Activate(true);
		FUeAgentInterfaceLogger::Log(
			TEXT("CmdComponentSetProperty NiagaraReinit actor=%s component=%s property=%s"),
			*Actor->GetName(),
			*NiagaraComponent->GetName(),
			*PropertyName);
	}
	Actor->MarkPackageDirty();

	FString ExportedValue;
	Property->ExportTextItem_Direct(ExportedValue, ValuePtr, nullptr, Component, PPF_None);
	OutData->SetStringField(TEXT("actor_name"), Actor->GetName());
	OutData->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
	OutData->SetStringField(TEXT("component"), Component->GetName());
	OutData->SetStringField(TEXT("component_path"), Component->GetPathName());
	OutData->SetStringField(TEXT("property_name"), PropertyName);
	OutData->SetStringField(TEXT("value_text"), ValueText);
	OutData->SetStringField(TEXT("applied_value_text"), ExportedValue);
	SetPropertyImportResultFields(OutData, Property, ValueText, ExportedValue, TEXT("imported_and_read_back"));
	if (bPrimitiveCollisionStateRefreshed)
	{
		OutData->SetBoolField(TEXT("primitive_collision_state_refreshed"), true);
	}
	return true;
}

namespace
{
	static TMap<FString, FTSTicker::FDelegateHandle> GLevelMorphTargetPulseTickers;

	static FString MakeLevelMorphTargetPulseKey(const USkeletalMeshComponent* Component, const FString& MorphName)
	{
		return Component ? FString::Printf(TEXT("%s::%s"), *Component->GetPathName(), *MorphName) : FString();
	}

	static bool StopLevelMorphTargetPulse(const FString& Key)
	{
		if (Key.IsEmpty())
		{
			return false;
		}

		if (FTSTicker::FDelegateHandle* ExistingHandle = GLevelMorphTargetPulseTickers.Find(Key))
		{
			FTSTicker::GetCoreTicker().RemoveTicker(*ExistingHandle);
			GLevelMorphTargetPulseTickers.Remove(Key);
			return true;
		}
		return false;
	}

	static float EvaluateLevelMorphTargetPulseWeight(const double ElapsedSeconds, const double CycleSeconds, const float MinWeight, const float MaxWeight)
	{
		const double SafeCycleSeconds = FMath::Max(CycleSeconds, 0.001);
		const double Phase = FMath::Fmod(FMath::Max(ElapsedSeconds, 0.0), SafeCycleSeconds) / SafeCycleSeconds;
		const double Triangle = Phase < 0.5 ? Phase * 2.0 : (1.0 - Phase) * 2.0;
		const double Smooth = 0.5 - (0.5 * FMath::Cos(PI * Triangle));
		return static_cast<float>(FMath::Lerp(static_cast<double>(MinWeight), static_cast<double>(MaxWeight), Smooth));
	}

	static void ApplyLevelMorphTargetPulseWeight(USkeletalMeshComponent* Component, const FName MorphTargetName, const float Weight)
	{
		if (!Component)
		{
			return;
		}

		Component->SetMorphTarget(MorphTargetName, Weight);
		Component->RefreshBoneTransforms();
		Component->UpdateBounds();
		Component->MarkRenderDynamicDataDirty();
	}
}

bool FUeAgentHttpServer::CmdLevelSetSkeletalMeshMorphTarget(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	FString ComponentId;
	JsonTryGetString(Ctx.Params, TEXT("component"), ComponentId);
	if (ComponentId.IsEmpty())
	{
		JsonTryGetString(Ctx.Params, TEXT("component_id"), ComponentId);
	}

	FString MorphName;
	if (!JsonTryGetString(Ctx.Params, TEXT("morph_target"), MorphName) || MorphName.TrimStartAndEnd().IsEmpty())
	{
		if (!JsonTryGetString(Ctx.Params, TEXT("morph_target_name"), MorphName) || MorphName.TrimStartAndEnd().IsEmpty())
		{
			JsonTryGetString(Ctx.Params, TEXT("name"), MorphName);
		}
	}
	MorphName = MorphName.TrimStartAndEnd();
	if (MorphName.IsEmpty())
	{
		OutError = TEXT("missing_morph_target");
		return false;
	}

	TSharedPtr<FJsonObject> PulseObj;
	const bool bHasPulseObject = JsonTryGetObject(Ctx.Params, TEXT("pulse"), PulseObj);
	bool bStartPulse = bHasPulseObject;
	JsonTryGetBool(Ctx.Params, TEXT("pulse"), bStartPulse);
	JsonTryGetBool(Ctx.Params, TEXT("start_pulse"), bStartPulse);
	JsonTryGetBool(Ctx.Params, TEXT("animate"), bStartPulse);

	bool bStopPulse = false;
	JsonTryGetBool(Ctx.Params, TEXT("stop_pulse"), bStopPulse);
	JsonTryGetBool(Ctx.Params, TEXT("stop_animation"), bStopPulse);

	auto TryGetPulseNumber = [&](const TCHAR* Key, double& InOutValue) -> bool
	{
		double Value = 0.0;
		if (PulseObj.IsValid() && PulseObj->TryGetNumberField(Key, Value))
		{
			InOutValue = Value;
			return true;
		}
		if (Ctx.Params.IsValid() && Ctx.Params->TryGetNumberField(Key, Value))
		{
			InOutValue = Value;
			return true;
		}
		return false;
	};

	auto TryGetPulseBool = [&](const TCHAR* Key, bool& InOutValue) -> bool
	{
		bool Value = false;
		if (PulseObj.IsValid() && PulseObj->TryGetBoolField(Key, Value))
		{
			InOutValue = Value;
			return true;
		}
		if (Ctx.Params.IsValid() && Ctx.Params->TryGetBoolField(Key, Value))
		{
			InOutValue = Value;
			return true;
		}
		return false;
	};

	double MinWeightNumber = 0.0;
	double MaxWeightNumber = 1.0;
	double CycleSecondsNumber = 4.8;
	double DurationSecondsNumber = -1.0;
	double TickerIntervalSecondsNumber = 0.0;
	bool bLoopPulse = true;
	TryGetPulseNumber(TEXT("min_weight"), MinWeightNumber);
	TryGetPulseNumber(TEXT("max_weight"), MaxWeightNumber);
	TryGetPulseNumber(TEXT("cycle_seconds"), CycleSecondsNumber);
	TryGetPulseNumber(TEXT("duration_seconds"), DurationSecondsNumber);
	TryGetPulseNumber(TEXT("tick_interval_seconds"), TickerIntervalSecondsNumber);
	TryGetPulseBool(TEXT("loop"), bLoopPulse);

	const bool bHasExplicitWeight = Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("weight"));
	double WeightNumber = (bStartPulse && !bHasExplicitWeight) ? MinWeightNumber : 1.0;
	if (bStopPulse && !bHasExplicitWeight)
	{
		WeightNumber = MinWeightNumber;
	}
	JsonTryGetNumber(Ctx.Params, TEXT("weight"), WeightNumber);
	const float Weight = (float)WeightNumber;

	AActor* Actor = FindActorByNameOrLabel(World, Id);
	if (!Actor)
	{
		OutError = TEXT("actor_not_found");
		return false;
	}

	USkeletalMeshComponent* SkeletalComponent = nullptr;
	if (!ComponentId.TrimStartAndEnd().IsEmpty())
	{
		SkeletalComponent = Cast<USkeletalMeshComponent>(UeAgentLevelOps::FindComponentByNameOrPath(Actor, ComponentId));
		if (!SkeletalComponent)
		{
			OutError = TEXT("skeletal_mesh_component_not_found");
			return false;
		}
	}
	else
	{
		SkeletalComponent = Actor->FindComponentByClass<USkeletalMeshComponent>();
		if (!SkeletalComponent)
		{
			OutError = TEXT("actor_has_no_skeletal_mesh_component");
			return false;
		}
	}

	Actor->Modify();
	SkeletalComponent->Modify();

	FString SkeletalMeshPath;
	USkeletalMesh* Mesh = SkeletalComponent->GetSkeletalMeshAsset();
	if (JsonTryGetString(Ctx.Params, TEXT("skeletal_mesh"), SkeletalMeshPath) && !SkeletalMeshPath.TrimStartAndEnd().IsEmpty())
	{
		SkeletalMeshPath = SkeletalMeshPath.TrimStartAndEnd();
		Mesh = LoadObject<USkeletalMesh>(nullptr, *SkeletalMeshPath);
		if (!Mesh)
		{
			OutError = FString::Printf(TEXT("failed_to_load_skeletal_mesh: %s"), *SkeletalMeshPath);
			return false;
		}
		SkeletalComponent->SetSkeletalMesh(Mesh, true);
	}

	if (!Mesh)
	{
		OutError = TEXT("skeletal_mesh_component_has_no_mesh");
		return false;
	}

	UMorphTarget* FoundMorph = nullptr;
	for (const TObjectPtr<UMorphTarget>& MorphTarget : Mesh->GetMorphTargets())
	{
		if (MorphTarget && MorphTarget->GetName().Equals(MorphName, ESearchCase::IgnoreCase))
		{
			FoundMorph = MorphTarget.Get();
			MorphName = FoundMorph->GetName();
			break;
		}
	}
	if (!FoundMorph)
	{
		OutError = TEXT("morph_target_not_found");
		return false;
	}

	bool bClearExisting = false;
	JsonTryGetBool(Ctx.Params, TEXT("clear_existing"), bClearExisting);
	if (bClearExisting)
	{
		SkeletalComponent->ClearMorphTargets();
	}

	const FString PulseKey = MakeLevelMorphTargetPulseKey(SkeletalComponent, MorphName);
	bool bStoppedExistingPulse = false;
	if (bStartPulse || bStopPulse)
	{
		bStoppedExistingPulse = StopLevelMorphTargetPulse(PulseKey);
	}

	const FName MorphTargetFName(*MorphName);
	SkeletalComponent->SetMorphTarget(MorphTargetFName, Weight);
	SkeletalComponent->RefreshBoneTransforms();
	SkeletalComponent->UpdateBounds();
	SkeletalComponent->MarkRenderStateDirty();
	SkeletalComponent->PostEditChange();
	Actor->MarkPackageDirty();

	bool bPulseStarted = false;
	if (bStartPulse)
	{
		TWeakObjectPtr<USkeletalMeshComponent> WeakComponent(SkeletalComponent);
		const FString CapturedPulseKey = PulseKey;
		const FName CapturedMorphTargetName = MorphTargetFName;
		const float MinWeight = static_cast<float>(MinWeightNumber);
		const float MaxWeight = static_cast<float>(MaxWeightNumber);
		const double CycleSeconds = FMath::Max(CycleSecondsNumber, 0.001);
		const double DurationSeconds = DurationSecondsNumber;
		const bool bLoop = bLoopPulse;
		const double StartSeconds = FPlatformTime::Seconds();

		const FTSTicker::FDelegateHandle PulseTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda(
				[WeakComponent, CapturedPulseKey, CapturedMorphTargetName, MinWeight, MaxWeight, CycleSeconds, DurationSeconds, bLoop, StartSeconds](float)
				{
					USkeletalMeshComponent* LiveComponent = WeakComponent.Get();
					if (!LiveComponent)
					{
						GLevelMorphTargetPulseTickers.Remove(CapturedPulseKey);
						return false;
					}

					const double ElapsedSeconds = FPlatformTime::Seconds() - StartSeconds;
					if (DurationSeconds > 0.0 && ElapsedSeconds >= DurationSeconds)
					{
						ApplyLevelMorphTargetPulseWeight(LiveComponent, CapturedMorphTargetName, MinWeight);
						GLevelMorphTargetPulseTickers.Remove(CapturedPulseKey);
						return false;
					}
					if (!bLoop && ElapsedSeconds >= CycleSeconds)
					{
						ApplyLevelMorphTargetPulseWeight(LiveComponent, CapturedMorphTargetName, MinWeight);
						GLevelMorphTargetPulseTickers.Remove(CapturedPulseKey);
						return false;
					}

					const float PulseWeight = EvaluateLevelMorphTargetPulseWeight(ElapsedSeconds, CycleSeconds, MinWeight, MaxWeight);
					ApplyLevelMorphTargetPulseWeight(LiveComponent, CapturedMorphTargetName, PulseWeight);
					return true;
				}),
			static_cast<float>(FMath::Max(TickerIntervalSecondsNumber, 0.0)));

		GLevelMorphTargetPulseTickers.Add(PulseKey, PulseTickerHandle);
		bPulseStarted = true;
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("actor_name"), Actor->GetName());
	OutData->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
	OutData->SetStringField(TEXT("component_name"), SkeletalComponent->GetName());
	OutData->SetStringField(TEXT("component_path"), SkeletalComponent->GetPathName());
	OutData->SetStringField(TEXT("skeletal_mesh"), Mesh->GetPathName());
	OutData->SetStringField(TEXT("morph_target"), MorphName);
	OutData->SetNumberField(TEXT("requested_weight"), Weight);
	OutData->SetNumberField(TEXT("applied_weight"), SkeletalComponent->GetMorphTarget(FName(*MorphName)));
	OutData->SetBoolField(TEXT("clear_existing"), bClearExisting);
	OutData->SetBoolField(TEXT("pulse_started"), bPulseStarted);
	OutData->SetBoolField(TEXT("pulse_stopped"), bStopPulse || bStoppedExistingPulse);
	OutData->SetBoolField(TEXT("stopped_existing_pulse"), bStoppedExistingPulse);
	OutData->SetStringField(TEXT("pulse_key"), PulseKey);
	if (bPulseStarted)
	{
		OutData->SetNumberField(TEXT("pulse_min_weight"), MinWeightNumber);
		OutData->SetNumberField(TEXT("pulse_max_weight"), MaxWeightNumber);
		OutData->SetNumberField(TEXT("pulse_cycle_seconds"), CycleSecondsNumber);
		OutData->SetNumberField(TEXT("pulse_duration_seconds"), DurationSecondsNumber);
		OutData->SetBoolField(TEXT("pulse_loop"), bLoopPulse);
		OutData->SetNumberField(TEXT("pulse_tick_interval_seconds"), TickerIntervalSecondsNumber);
	}
	OutData->SetStringField(TEXT("display_status"), bPulseStarted ? TEXT("scene_component_morph_pulse_started") : TEXT("scene_component_morph_set"));
	return true;
}

bool FUeAgentHttpServer::CmdSpawnActor(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FString ClassPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("class_path"), ClassPath) || ClassPath.IsEmpty())
	{
		OutError = TEXT("missing_class_path");
		return false;
	}

	UClass* ActorClass = LoadClass<AActor>(nullptr, *ClassPath);
	if (!ActorClass)
	{
		OutError = FString::Printf(TEXT("failed_to_load_class: %s"), *ClassPath);
		return false;
	}

	FVector Location(0, 0, 0);
	FRotator Rotation(0, 0, 0);
	FVector Scale(1, 1, 1);
	JsonTryGetVector(Ctx.Params, TEXT("location"), Location);
	JsonTryGetRotator(Ctx.Params, TEXT("rotation"), Rotation);
	JsonTryGetVector(Ctx.Params, TEXT("scale"), Scale);

	bool bSnapToGround = false;
	bool bSnapUseActorBounds = false;
	bool bRequireGroundHit = false;
	double GroundOffset = 0.0;
	bool bGroundHit = false;
	double GroundZ = Location.Z;
	JsonTryGetBool(Ctx.Params, TEXT("snap_to_ground"), bSnapToGround);
	JsonTryGetBool(Ctx.Params, TEXT("snap_use_actor_bounds"), bSnapUseActorBounds);
	JsonTryGetBool(Ctx.Params, TEXT("require_ground_hit"), bRequireGroundHit);
	JsonTryGetNumber(Ctx.Params, TEXT("ground_offset"), GroundOffset);
	if (bSnapToGround)
	{
		double TraceUp = 50000.0;
		double TraceDown = 50000.0;
		JsonTryGetNumber(Ctx.Params, TEXT("ground_trace_up"), TraceUp);
		JsonTryGetNumber(Ctx.Params, TEXT("ground_trace_down"), TraceDown);
		TraceUp = FMath::Max(100.0, TraceUp);
		TraceDown = FMath::Max(100.0, TraceDown);

		const FVector TraceStart = Location + FVector(0.0, 0.0, TraceUp);
		const FVector TraceEnd = Location - FVector(0.0, 0.0, TraceDown);
		FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(UeAgentSpawnSnapToGround), true);
		QueryParams.bReturnPhysicalMaterial = false;
		FHitResult Hit;
		if (World->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_WorldStatic, QueryParams))
		{
			bGroundHit = true;
			GroundZ = Hit.ImpactPoint.Z + GroundOffset;
			Location.Z = GroundZ;
		}
		else if (bRequireGroundHit)
		{
			OutError = TEXT("ground_not_found");
			return false;
		}
	}

	const bool bUseAutoTx = [&]()
	{
		FScopeLock Lock(&TransactionMutex);
		return ActiveTransactionIndex == INDEX_NONE;
	}();
	TOptional<FScopedTransaction> AutoTx;
	if (bUseAutoTx)
	{
		AutoTx.Emplace(NSLOCTEXT("UeAgentInterface", "SpawnActorTx", "UeAgentInterface Spawn Actor"));
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.ObjectFlags |= RF_Transactional;

	AActor* NewActor = World->SpawnActor<AActor>(ActorClass, FTransform(Rotation, Location, Scale), SpawnParams);
	if (!NewActor)
	{
		OutError = TEXT("spawn_failed");
		return false;
	}

	FString Label;
	if (JsonTryGetString(Ctx.Params, TEXT("label"), Label) && !Label.IsEmpty())
	{
		NewActor->SetActorLabel(Label);
	}

	FString FolderPath;
	JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath);
	FolderPath = FolderPath.TrimStartAndEnd();
	if (!FolderPath.IsEmpty())
	{
		NewActor->Modify();
		NewActor->SetFolderPath(*FolderPath);
	}

	const TArray<TSharedPtr<FJsonValue>>* TagValues = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("tags"), TagValues) && TagValues)
	{
		NewActor->Modify();
		for (const TSharedPtr<FJsonValue>& TagValue : *TagValues)
		{
			FString TagStr = TagValue.IsValid() ? TagValue->AsString() : FString();
			TagStr = TagStr.TrimStartAndEnd();
			if (!TagStr.IsEmpty())
			{
				NewActor->Tags.AddUnique(FName(*TagStr));
			}
		}
	}

	FString StaticMeshPath;
	if (JsonTryGetString(Ctx.Params, TEXT("static_mesh"), StaticMeshPath) && !StaticMeshPath.IsEmpty())
	{
		bSnapUseActorBounds = true;

		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *StaticMeshPath);
		if (!Mesh)
		{
			OutError = FString::Printf(TEXT("failed_to_load_static_mesh: %s"), *StaticMeshPath);
			return false;
		}

		UStaticMeshComponent* SMC = nullptr;
		if (AStaticMeshActor* SMA = Cast<AStaticMeshActor>(NewActor))
		{
			SMC = SMA->GetStaticMeshComponent();
		}
		if (!SMC)
		{
			SMC = NewActor->FindComponentByClass<UStaticMeshComponent>();
		}
		if (!SMC)
		{
			OutError = TEXT("actor_has_no_static_mesh_component");
			return false;
		}

		NewActor->Modify();
		SMC->Modify();
		SMC->SetStaticMesh(Mesh);
		SMC->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		SMC->MarkRenderStateDirty();
	}

	if (bSnapToGround && bGroundHit && bSnapUseActorBounds)
	{
		const FBox Bounds = NewActor->GetComponentsBoundingBox(/*bNonColliding*/ true);
		if (Bounds.IsValid)
		{
			const double DeltaZ = GroundZ - Bounds.Min.Z;
			if (!FMath::IsNearlyZero(DeltaZ, KINDA_SMALL_NUMBER))
			{
				NewActor->Modify();
				NewActor->AddActorWorldOffset(FVector(0.0, 0.0, DeltaZ), false, nullptr, ETeleportType::TeleportPhysics);
			}
		}
	}

	OutData->SetStringField(TEXT("name"), NewActor->GetName());
	OutData->SetStringField(TEXT("label"), NewActor->GetActorLabel());
	OutData->SetStringField(TEXT("class"), NewActor->GetClass()->GetPathName());
	OutData->SetObjectField(TEXT("location"), VecToJson(NewActor->GetActorLocation()));
	OutData->SetStringField(TEXT("folder_path"), NewActor->GetFolderPath().ToString());

	TArray<TSharedPtr<FJsonValue>> TagsJson;
	TagsJson.Reserve(NewActor->Tags.Num());
	for (const FName& Tag : NewActor->Tags)
	{
		TagsJson.Add(MakeShared<FJsonValueString>(Tag.ToString()));
	}
	OutData->SetArrayField(TEXT("tags"), TagsJson);
	OutData->SetBoolField(TEXT("snap_to_ground"), bSnapToGround);
	OutData->SetBoolField(TEXT("snap_ground_hit"), bGroundHit);
	OutData->SetBoolField(TEXT("snap_use_actor_bounds"), bSnapUseActorBounds);
	OutData->SetNumberField(TEXT("ground_z"), GroundZ);
	return true;
}

bool FUeAgentHttpServer::CmdLevelSpawnWallWithOpening(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	TSharedPtr<FJsonObject> Plane;
	if (!JsonTryGetObject(Ctx.Params, TEXT("plane"), Plane) || !Plane.IsValid())
	{
		OutError = TEXT("missing_plane");
		return false;
	}

	FVector PlaneCenter(0.0, 0.0, 0.0);
	if (!JsonTryGetVector(Plane, TEXT("center"), PlaneCenter))
	{
		OutError = TEXT("missing_plane_center");
		return false;
	}

	FVector PlaneNormal(1.0, 0.0, 0.0);
	if (!JsonTryGetVector(Plane, TEXT("normal"), PlaneNormal) || PlaneNormal.IsNearlyZero(KINDA_SMALL_NUMBER))
	{
		OutError = TEXT("missing_plane_normal");
		return false;
	}

	FVector PlaneUp(0.0, 0.0, 1.0);
	JsonTryGetVector(Plane, TEXT("up"), PlaneUp);
	if (PlaneUp.IsNearlyZero(KINDA_SMALL_NUMBER))
	{
		PlaneUp = FVector(0.0, 0.0, 1.0);
	}

	PlaneNormal.Normalize();
	PlaneUp.Normalize();
	if (FMath::Abs(PlaneNormal | PlaneUp) > 0.99)
	{
		PlaneUp = FVector(0.0, 1.0, 0.0);
		if (FMath::Abs(PlaneNormal | PlaneUp) > 0.99)
		{
			PlaneUp = FVector(1.0, 0.0, 0.0);
		}
	}

	const FMatrix PlaneBasis = FRotationMatrix::MakeFromXZ(PlaneNormal, PlaneUp);
	const FVector BasisNormal = PlaneBasis.GetScaledAxis(EAxis::X);
	const FVector BasisRight = PlaneBasis.GetScaledAxis(EAxis::Y);
	const FVector BasisUp = PlaneBasis.GetScaledAxis(EAxis::Z);
	const FRotator WallRotation = PlaneBasis.Rotator();

	TSharedPtr<FJsonObject> WallSize;
	if (!JsonTryGetObject(Ctx.Params, TEXT("wall_size"), WallSize) || !WallSize.IsValid())
	{
		OutError = TEXT("missing_wall_size");
		return false;
	}

	double ThicknessCm = 0.0;
	double WallWidthCm = 0.0;
	double WallHeightCm = 0.0;
	if (!JsonTryGetNumber(WallSize, TEXT("thickness_cm"), ThicknessCm))
	{
		OutError = TEXT("missing_wall_size_thickness_cm");
		return false;
	}
	if (!JsonTryGetNumber(WallSize, TEXT("width_cm"), WallWidthCm))
	{
		OutError = TEXT("missing_wall_size_width_cm");
		return false;
	}
	if (!JsonTryGetNumber(WallSize, TEXT("height_cm"), WallHeightCm))
	{
		OutError = TEXT("missing_wall_size_height_cm");
		return false;
	}
	if (ThicknessCm <= 0.0 || WallWidthCm <= 0.0 || WallHeightCm <= 0.0)
	{
		OutError = TEXT("invalid_wall_size");
		return false;
	}

	double OpeningCenterRightCm = 0.0;
	double OpeningCenterUpCm = 0.0;
	TSharedPtr<FJsonObject> OpeningCenter;
	if (JsonTryGetObject(Ctx.Params, TEXT("opening_center"), OpeningCenter) && OpeningCenter.IsValid())
	{
		if (!JsonTryGetNumber(OpeningCenter, TEXT("right_cm"), OpeningCenterRightCm))
		{
			OutError = TEXT("missing_opening_center_right_cm");
			return false;
		}
		if (!JsonTryGetNumber(OpeningCenter, TEXT("up_cm"), OpeningCenterUpCm))
		{
			OutError = TEXT("missing_opening_center_up_cm");
			return false;
		}
	}

	double OpeningWidthCm = 0.0;
	double OpeningHeightCm = 0.0;
	TSharedPtr<FJsonObject> OpeningSize;
	const bool bHasOpeningSize = JsonTryGetObject(Ctx.Params, TEXT("opening_size"), OpeningSize) && OpeningSize.IsValid();
	if (bHasOpeningSize)
	{
		if (!JsonTryGetNumber(OpeningSize, TEXT("width_cm"), OpeningWidthCm))
		{
			OutError = TEXT("missing_opening_size_width_cm");
			return false;
		}
		if (!JsonTryGetNumber(OpeningSize, TEXT("height_cm"), OpeningHeightCm))
		{
			OutError = TEXT("missing_opening_size_height_cm");
			return false;
		}
	}

	double OpeningPaddingCm = 0.0;
	JsonTryGetNumber(Ctx.Params, TEXT("opening_padding_cm"), OpeningPaddingCm);
	OpeningPaddingCm = FMath::Max(0.0, OpeningPaddingCm);
	if (OpeningWidthCm > 0.0 && OpeningHeightCm > 0.0)
	{
		OpeningWidthCm += OpeningPaddingCm * 2.0;
		OpeningHeightCm += OpeningPaddingCm * 2.0;
	}

	double MinSegmentSizeCm = 1.0;
	JsonTryGetNumber(Ctx.Params, TEXT("min_segment_size_cm"), MinSegmentSizeCm);
	MinSegmentSizeCm = FMath::Max(0.0, MinSegmentSizeCm);

	double EpsilonCm = 0.01;
	JsonTryGetNumber(Ctx.Params, TEXT("epsilon_cm"), EpsilonCm);
	EpsilonCm = FMath::Max(0.0, EpsilonCm);

	bool bClampOpening = false;
	JsonTryGetBool(Ctx.Params, TEXT("clamp_opening"), bClampOpening);

	FString ClassPath;
	JsonTryGetString(Ctx.Params, TEXT("class_path"), ClassPath);
	if (ClassPath.IsEmpty())
	{
		ClassPath = TEXT("/Script/Engine.StaticMeshActor");
	}
	UClass* ActorClass = LoadClass<AActor>(nullptr, *ClassPath);
	if (!ActorClass)
	{
		OutError = FString::Printf(TEXT("failed_to_load_class: %s"), *ClassPath);
		return false;
	}

	FString StaticMeshPath;
	JsonTryGetString(Ctx.Params, TEXT("static_mesh"), StaticMeshPath);
	if (StaticMeshPath.IsEmpty())
	{
		StaticMeshPath = TEXT("/Engine/BasicShapes/Cube.Cube");
	}
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *StaticMeshPath);
	if (!Mesh)
	{
		OutError = FString::Printf(TEXT("failed_to_load_static_mesh: %s"), *StaticMeshPath);
		return false;
	}

	FString FolderPath;
	JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath);

	FString LabelPrefix;
	JsonTryGetString(Ctx.Params, TEXT("label_prefix"), LabelPrefix);
	if (LabelPrefix.IsEmpty())
	{
		LabelPrefix = FString::Printf(TEXT("WallWithOpening_%s"), *Ctx.RequestId);
	}

	FUeAgentInterfaceLogger::Log(
		TEXT("CmdLevelSpawnWallWithOpening req=%s label_prefix=%s wall_width=%.2f wall_height=%.2f opening_width=%.2f opening_height=%.2f clamp=%s"),
		*Ctx.RequestId,
		*LabelPrefix,
		WallWidthCm,
		WallHeightCm,
		OpeningWidthCm,
		OpeningHeightCm,
		bClampOpening ? TEXT("true") : TEXT("false"));

	const bool bUseAutoTx = [&]()
	{
		FScopeLock Lock(&TransactionMutex);
		return ActiveTransactionIndex == INDEX_NONE;
	}();
	TOptional<FScopedTransaction> AutoTx;
	if (bUseAutoTx)
	{
		AutoTx.Emplace(NSLOCTEXT("UeAgentInterface", "SpawnWallWithOpeningTx", "UeAgentInterface Spawn Wall With Opening"));
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.ObjectFlags |= RF_Transactional;

	auto SpawnSegment = [&](const FString& SegmentSuffix, const double SegmentCenterRightCm, const double SegmentCenterUpCm, const double SegmentWidthCm, const double SegmentHeightCm, AActor*& OutActor) -> bool
	{
		OutActor = nullptr;
		if (SegmentWidthCm <= MinSegmentSizeCm || SegmentHeightCm <= MinSegmentSizeCm)
		{
			return true;
		}

		const FVector SegmentLocation = PlaneCenter + BasisRight * SegmentCenterRightCm + BasisUp * SegmentCenterUpCm;
		const FVector SegmentScale(ThicknessCm / 100.0, SegmentWidthCm / 100.0, SegmentHeightCm / 100.0);

		AActor* NewActor = World->SpawnActor<AActor>(ActorClass, FTransform(WallRotation, SegmentLocation, SegmentScale), SpawnParams);
		if (!NewActor)
		{
			OutError = TEXT("spawn_failed");
			return false;
		}

		if (!LabelPrefix.IsEmpty())
		{
			NewActor->SetActorLabel(LabelPrefix + SegmentSuffix);
		}

		if (!FolderPath.IsEmpty())
		{
			NewActor->Modify();
			NewActor->SetFolderPath(*FolderPath);
		}

		UStaticMeshComponent* SMC = nullptr;
		if (AStaticMeshActor* SMA = Cast<AStaticMeshActor>(NewActor))
		{
			SMC = SMA->GetStaticMeshComponent();
		}
		if (!SMC)
		{
			SMC = NewActor->FindComponentByClass<UStaticMeshComponent>();
		}
		if (!SMC)
		{
			OutError = TEXT("actor_has_no_static_mesh_component");
			return false;
		}

		NewActor->Modify();
		SMC->Modify();
		SMC->SetStaticMesh(Mesh);
		SMC->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		SMC->MarkRenderStateDirty();

		OutActor = NewActor;
		return true;
	};

	TArray<AActor*> SpawnedActors;
	SpawnedActors.Reserve(4);

	const double WallHalfW = WallWidthCm * 0.5;
	const double WallHalfH = WallHeightCm * 0.5;

	if (OpeningWidthCm <= EpsilonCm || OpeningHeightCm <= EpsilonCm)
	{
		AActor* FullWall = nullptr;
		if (!SpawnSegment(TEXT(""), 0.0, 0.0, WallWidthCm, WallHeightCm, FullWall))
		{
			return false;
		}
		if (FullWall)
		{
			SpawnedActors.Add(FullWall);
		}
	}
	else
	{
		const double OpenHalfW = OpeningWidthCm * 0.5;
		const double OpenHalfH = OpeningHeightCm * 0.5;
		double OpenMinX = OpeningCenterRightCm - OpenHalfW;
		double OpenMaxX = OpeningCenterRightCm + OpenHalfW;
		double OpenMinY = OpeningCenterUpCm - OpenHalfH;
		double OpenMaxY = OpeningCenterUpCm + OpenHalfH;

		const bool bOutOfBounds =
			(OpenMinX < (-WallHalfW - EpsilonCm)) ||
			(OpenMaxX > (WallHalfW + EpsilonCm)) ||
			(OpenMinY < (-WallHalfH - EpsilonCm)) ||
			(OpenMaxY > (WallHalfH + EpsilonCm));

		if (bOutOfBounds && !bClampOpening)
		{
			OutError = TEXT("opening_out_of_bounds");
			return false;
		}

		if (bOutOfBounds && bClampOpening)
		{
			OpenMinX = FMath::Clamp(OpenMinX, -WallHalfW, WallHalfW);
			OpenMaxX = FMath::Clamp(OpenMaxX, -WallHalfW, WallHalfW);
			OpenMinY = FMath::Clamp(OpenMinY, -WallHalfH, WallHalfH);
			OpenMaxY = FMath::Clamp(OpenMaxY, -WallHalfH, WallHalfH);
		}

		if ((OpenMaxX - OpenMinX) <= EpsilonCm || (OpenMaxY - OpenMinY) <= EpsilonCm)
		{
			OutError = TEXT("opening_collapsed");
			return false;
		}

		const double LeftWidth = OpenMinX + WallHalfW;
		const double RightWidth = WallHalfW - OpenMaxX;
		const double BottomHeight = OpenMinY + WallHalfH;
		const double TopHeight = WallHalfH - OpenMaxY;
		const double OpeningWidthClamped = OpenMaxX - OpenMinX;

		AActor* Seg = nullptr;
		if (!SpawnSegment(TEXT("_Left"), (-WallHalfW + OpenMinX) * 0.5, 0.0, LeftWidth, WallHeightCm, Seg))
		{
			return false;
		}
		if (Seg)
		{
			SpawnedActors.Add(Seg);
		}

		if (!SpawnSegment(TEXT("_Right"), (OpenMaxX + WallHalfW) * 0.5, 0.0, RightWidth, WallHeightCm, Seg))
		{
			return false;
		}
		if (Seg)
		{
			SpawnedActors.Add(Seg);
		}

		if (!SpawnSegment(TEXT("_Bottom"), (OpenMinX + OpenMaxX) * 0.5, (-WallHalfH + OpenMinY) * 0.5, OpeningWidthClamped, BottomHeight, Seg))
		{
			return false;
		}
		if (Seg)
		{
			SpawnedActors.Add(Seg);
		}

		if (!SpawnSegment(TEXT("_Top"), (OpenMinX + OpenMaxX) * 0.5, (OpenMaxY + WallHalfH) * 0.5, OpeningWidthClamped, TopHeight, Seg))
		{
			return false;
		}
		if (Seg)
		{
			SpawnedActors.Add(Seg);
		}
	}

	TArray<TSharedPtr<FJsonValue>> Segments;
	Segments.Reserve(SpawnedActors.Num());
	for (AActor* A : SpawnedActors)
	{
		if (!A)
		{
			continue;
		}
		Segments.Add(MakeShared<FJsonValueObject>(UeAgentLevelOps::MakeActorSummaryJson(A)));
	}

	TSharedPtr<FJsonObject> OutPlane = MakeShared<FJsonObject>();
	OutPlane->SetObjectField(TEXT("center"), VecToJson(PlaneCenter));
	OutPlane->SetObjectField(TEXT("normal"), VecToJson(BasisNormal));
	OutPlane->SetObjectField(TEXT("right"), VecToJson(BasisRight));
	OutPlane->SetObjectField(TEXT("up"), VecToJson(BasisUp));
	OutPlane->SetObjectField(TEXT("rotation"), RotToJson(WallRotation));

	TSharedPtr<FJsonObject> OutWallSize = MakeShared<FJsonObject>();
	OutWallSize->SetNumberField(TEXT("thickness_cm"), ThicknessCm);
	OutWallSize->SetNumberField(TEXT("width_cm"), WallWidthCm);
	OutWallSize->SetNumberField(TEXT("height_cm"), WallHeightCm);

	TSharedPtr<FJsonObject> OutOpening = MakeShared<FJsonObject>();
	OutOpening->SetNumberField(TEXT("center_right_cm"), OpeningCenterRightCm);
	OutOpening->SetNumberField(TEXT("center_up_cm"), OpeningCenterUpCm);
	OutOpening->SetNumberField(TEXT("width_cm"), OpeningWidthCm);
	OutOpening->SetNumberField(TEXT("height_cm"), OpeningHeightCm);
	OutOpening->SetNumberField(TEXT("padding_cm"), OpeningPaddingCm);
	OutOpening->SetBoolField(TEXT("clamp_opening"), bClampOpening);

	OutData->SetStringField(TEXT("label_prefix"), LabelPrefix);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetObjectField(TEXT("plane"), OutPlane);
	OutData->SetObjectField(TEXT("wall_size"), OutWallSize);
	OutData->SetObjectField(TEXT("opening"), OutOpening);
	OutData->SetNumberField(TEXT("segment_count"), Segments.Num());
	OutData->SetArrayField(TEXT("segments"), Segments);
	return true;
}

bool FUeAgentHttpServer::CmdDestroyActor(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	const bool bUseAutoTx = [&]()
	{
		FScopeLock Lock(&TransactionMutex);
		return ActiveTransactionIndex == INDEX_NONE;
	}();
	TOptional<FScopedTransaction> AutoTx;
	if (bUseAutoTx)
	{
		AutoTx.Emplace(NSLOCTEXT("UeAgentInterface", "DestroyActorTx", "UeAgentInterface Destroy Actor"));
	}

	if (GEditor)
	{
		const bool bWasSelected = Actor->IsSelected();
		if (bWasSelected)
		{
			USelection* SelectedActors = GEditor->GetSelectedActors();
			USelection* SelectedComponents = GEditor->GetSelectedComponents();
			if (SelectedActors)
			{
				SelectedActors->BeginBatchSelectOperation();
			}
			if (SelectedComponents)
			{
				SelectedComponents->BeginBatchSelectOperation();
				SelectedComponents->DeselectAll(UActorComponent::StaticClass());
				SelectedComponents->EndBatchSelectOperation(false);
			}
			if (SelectedActors)
			{
				SelectedActors->Deselect(Actor);
				SelectedActors->EndBatchSelectOperation(false);
			}
			GEditor->NoteSelectionChange();
		}
	}

	Actor->Modify();
	Actor->Destroy();

	OutData->SetBoolField(TEXT("destroyed"), true);
	OutData->SetStringField(TEXT("id"), Id);
	return true;
}

bool FUeAgentHttpServer::CmdLevelMarkProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FVector Location = FVector::ZeroVector;
	if (!JsonTryGetVector(Ctx.Params, TEXT("location"), Location))
	{
		OutError = TEXT("missing_location");
		return false;
	}

	FRotator Rotation = FRotator::ZeroRotator;
	JsonTryGetRotator(Ctx.Params, TEXT("rotation"), Rotation);

	FString Label;
	JsonTryGetString(Ctx.Params, TEXT("label"), Label);
	Label = Label.TrimStartAndEnd();

	FString FolderPath;
	JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath);
	FolderPath = FolderPath.TrimStartAndEnd();

	const TArray<TSharedPtr<FJsonValue>>* Tags = nullptr;

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.ObjectFlags |= RF_Transactional;

	ATargetPoint* Probe = World->SpawnActor<ATargetPoint>(Location, Rotation, SpawnParams);
	if (!Probe)
	{
		OutError = TEXT("probe_spawn_failed");
		return false;
	}

	if (!Label.IsEmpty())
	{
		Probe->SetActorLabel(Label);
	}
	if (!FolderPath.IsEmpty())
	{
		Probe->SetFolderPath(FName(*FolderPath));
	}
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("tags"), Tags) && Tags)
	{
		for (const TSharedPtr<FJsonValue>& TagValue : *Tags)
		{
			const FString TagStr = TagValue.IsValid() ? TagValue->AsString() : FString();
			if (TagStr.TrimStartAndEnd().IsEmpty())
			{
				continue;
			}
			Probe->Tags.AddUnique(FName(*TagStr.TrimStartAndEnd()));
		}
	}

	OutData->SetObjectField(TEXT("probe"), UeAgentLevelOps::MakeActorInfoJson(Probe));
	OutData->SetObjectField(TEXT("location"), VecToJson(Probe->GetActorLocation()));
	OutData->SetObjectField(TEXT("rotation"), RotToJson(Probe->GetActorRotation()));
	return true;
}

bool FUeAgentHttpServer::CmdLevelGenerateProbes(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FString LabelPrefix = TEXT("Probe");
	JsonTryGetString(Ctx.Params, TEXT("label_prefix"), LabelPrefix);
	LabelPrefix = LabelPrefix.TrimStartAndEnd();
	if (LabelPrefix.IsEmpty())
	{
		LabelPrefix = TEXT("Probe");
	}

	FString FolderPath;
	JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath);
	FolderPath = FolderPath.TrimStartAndEnd();

	FRotator Rotation = FRotator::ZeroRotator;
	JsonTryGetRotator(Ctx.Params, TEXT("rotation"), Rotation);

	const TArray<TSharedPtr<FJsonValue>>* Tags = nullptr;
	const bool bHasTags = Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("tags"), Tags) && Tags;

	TArray<FVector> Locations;
	Locations.Reserve(32);

	const TArray<TSharedPtr<FJsonValue>>* PointsJson = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("points"), PointsJson) && PointsJson)
	{
		for (const TSharedPtr<FJsonValue>& Value : *PointsJson)
		{
			const TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Obj.IsValid())
			{
				OutError = TEXT("points_invalid_vector");
				return false;
			}
			FVector P = FVector::ZeroVector;
			double X = 0.0, Y = 0.0, Z = 0.0;
			if (!UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("x"), TEXT("X") }, X)
				|| !UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("y"), TEXT("Y") }, Y)
				|| !UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("z"), TEXT("Z") }, Z))
			{
				OutError = TEXT("points_invalid_vector");
				return false;
			}
			P.X = (float)X;
			P.Y = (float)Y;
			P.Z = (float)Z;
			Locations.Add(P);
		}
	}
	else
	{
		const TArray<TSharedPtr<FJsonValue>>* ProbesJson = nullptr;
		if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetArrayField(TEXT("probes"), ProbesJson) || !ProbesJson)
		{
			OutError = TEXT("points_or_probes_required");
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *ProbesJson)
		{
			const TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Obj.IsValid())
			{
				OutError = TEXT("probes_invalid_item");
				return false;
			}

			FVector P = FVector::ZeroVector;
			if (!JsonTryGetVector(Obj, TEXT("location"), P))
			{
				OutError = TEXT("probes_missing_location");
				return false;
			}
			Locations.Add(P);
		}
	}

	if (Locations.Num() <= 0)
	{
		OutError = TEXT("no_probe_points");
		return false;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.ObjectFlags |= RF_Transactional;

	TArray<TSharedPtr<FJsonValue>> Created;
	Created.Reserve(Locations.Num());

	for (int32 Index = 0; Index < Locations.Num(); ++Index)
	{
		ATargetPoint* Probe = World->SpawnActor<ATargetPoint>(Locations[Index], Rotation, SpawnParams);
		if (!Probe)
		{
			OutError = TEXT("probe_spawn_failed");
			return false;
		}

		const FString Label = FString::Printf(TEXT("%s_%02d"), *LabelPrefix, Index + 1);
		Probe->SetActorLabel(Label);
		if (!FolderPath.IsEmpty())
		{
			Probe->SetFolderPath(FName(*FolderPath));
		}
		if (bHasTags && Tags)
		{
			for (const TSharedPtr<FJsonValue>& TagValue : *Tags)
			{
				const FString TagStr = TagValue.IsValid() ? TagValue->AsString() : FString();
				if (TagStr.TrimStartAndEnd().IsEmpty())
				{
					continue;
				}
				Probe->Tags.AddUnique(FName(*TagStr.TrimStartAndEnd()));
			}
		}

		Created.Add(MakeShared<FJsonValueObject>(UeAgentLevelOps::MakeActorInfoJson(Probe)));
	}

	OutData->SetStringField(TEXT("label_prefix"), LabelPrefix);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetNumberField(TEXT("count"), Created.Num());
	OutData->SetArrayField(TEXT("probes"), Created);
	return true;
}

bool FUeAgentHttpServer::CmdLevelDestroyFolderActors(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
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

	TArray<AActor*> FolderActors;
	if (!UeAgentLevelOps::GetActorsInFolder(World, FolderPath, bIncludeChildFolders, FolderActors, OutError))
	{
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> ActorSummaries;
	ActorSummaries.Reserve(FolderActors.Num());
	for (AActor* Actor : FolderActors)
	{
		if (!Actor)
		{
			continue;
		}

		ActorSummaries.Add(MakeShared<FJsonValueObject>(UeAgentLevelOps::MakeActorSummaryJson(Actor)));
	}

	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetBoolField(TEXT("include_child_folders"), bIncludeChildFolders);
	OutData->SetNumberField(TEXT("matched_count"), ActorSummaries.Num());

	if (FolderActors.Num() <= 0)
	{
		OutData->SetNumberField(TEXT("deleted_count"), 0);
		OutData->SetArrayField(TEXT("actors"), ActorSummaries);
		return true;
	}

	const bool bUseAutoTx = [&]()
	{
		FScopeLock Lock(&TransactionMutex);
		return ActiveTransactionIndex == INDEX_NONE;
	}();
	TOptional<FScopedTransaction> AutoTx;
	if (bUseAutoTx)
	{
		AutoTx.Emplace(NSLOCTEXT("UeAgentInterface", "DestroyFolderActorsTx", "UeAgentInterface Destroy Folder Actors"));
	}

	UEditorActorSubsystem* ActorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;
	if (!ActorSubsystem)
	{
		OutError = TEXT("editor_actor_subsystem_unavailable");
		return false;
	}

	if (!ActorSubsystem->DestroyActors(FolderActors))
	{
		OutError = TEXT("destroy_folder_actors_failed");
		return false;
	}

	OutData->SetNumberField(TEXT("deleted_count"), ActorSummaries.Num());
	OutData->SetArrayField(TEXT("actors"), ActorSummaries);
	return true;
}

bool FUeAgentHttpServer::CmdLevelCleanupEmptyFolders(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FString Prefix;
	JsonTryGetString(Ctx.Params, TEXT("folder_path_prefix"), Prefix);
	Prefix = Prefix.TrimStartAndEnd();

	bool bDryRun = false;
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);

	FActorFolders& ActorFolders = FActorFolders::Get();
	TArray<FFolder> AllFolders;
	ActorFolders.ForEachFolder(*World, [&AllFolders](const FFolder& Folder)
	{
		AllFolders.Add(Folder);
		return true;
	});

	auto AddFolderAndParents = [](const FString& Path, TSet<FString>& OutSet)
	{
		TArray<FString> Parts;
		Path.ParseIntoArray(Parts, TEXT("/"), /*bCullEmpty*/ true);
		FString Accum;
		for (int32 i = 0; i < Parts.Num(); i++)
		{
			if (Accum.IsEmpty())
			{
				Accum = Parts[i];
			}
			else
			{
				Accum += TEXT("/") + Parts[i];
			}
			OutSet.Add(Accum);
		}
	};

	TSet<FString> UsedFolders;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor || Actor->IsActorBeingDestroyed())
		{
			continue;
		}

		const FName FolderPath = Actor->GetFolderPath();
		if (FolderPath.IsNone())
		{
			continue;
		}

		AddFolderAndParents(FolderPath.ToString(), UsedFolders);
	}

	TArray<FFolder> EmptyFolders;
	EmptyFolders.Reserve(AllFolders.Num());
	for (const FFolder& Folder : AllFolders)
	{
		const FString FolderStr = Folder.ToString();
		if (FolderStr.IsEmpty())
		{
			continue;
		}
		if (!Prefix.IsEmpty() && !FolderStr.StartsWith(Prefix))
		{
			continue;
		}
		if (UsedFolders.Contains(FolderStr))
		{
			continue;
		}
		EmptyFolders.Add(Folder);
	}

	EmptyFolders.Sort([](const FFolder& A, const FFolder& B) { return A.ToString().Len() > B.ToString().Len(); });

	const bool bUseAutoTx = [&]()
	{
		FScopeLock Lock(&TransactionMutex);
		return ActiveTransactionIndex == INDEX_NONE;
	}();
	TOptional<FScopedTransaction> AutoTx;
	if (bUseAutoTx && !bDryRun && EmptyFolders.Num() > 0)
	{
		AutoTx.Emplace(NSLOCTEXT("UeAgentInterface", "CleanupEmptyFoldersTx", "UeAgentInterface Cleanup Empty Folders"));
	}

	TArray<TSharedPtr<FJsonValue>> DeletedArr;
	DeletedArr.Reserve(EmptyFolders.Num());
	int32 DeletedCount = 0;
	for (const FFolder& Folder : EmptyFolders)
	{
		if (!bDryRun)
		{
			ActorFolders.DeleteFolder(*World, Folder);
		}
		DeletedArr.Add(MakeShared<FJsonValueString>(Folder.ToString()));
		DeletedCount++;
	}

	OutData->SetStringField(TEXT("folder_path_prefix"), Prefix);
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	OutData->SetNumberField(TEXT("total_folders"), AllFolders.Num());
	OutData->SetNumberField(TEXT("empty_folders_count"), EmptyFolders.Num());
	OutData->SetNumberField(TEXT("deleted_count"), DeletedCount);
	OutData->SetArrayField(TEXT("deleted_folders"), DeletedArr);
	return true;
}

bool FUeAgentHttpServer::CmdLevelAttachActor(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FString ChildId;
	FString ParentId;
	if (!JsonTryGetString(Ctx.Params, TEXT("child_id"), ChildId) || ChildId.IsEmpty())
	{
		OutError = TEXT("missing_child_id");
		return false;
	}
	if (!JsonTryGetString(Ctx.Params, TEXT("parent_id"), ParentId) || ParentId.IsEmpty())
	{
		OutError = TEXT("missing_parent_id");
		return false;
	}

	AActor* ChildActor = FindActorByNameOrLabel(World, ChildId);
	AActor* ParentActor = FindActorByNameOrLabel(World, ParentId);
	if (!ChildActor)
	{
		OutError = TEXT("child_actor_not_found");
		return false;
	}
	if (!ParentActor)
	{
		OutError = TEXT("parent_actor_not_found");
		return false;
	}

	const FAttachmentTransformRules Rules(EAttachmentRule::KeepWorld, true);
	ChildActor->Modify();
	const bool bAttached = ChildActor->AttachToActor(ParentActor, Rules);
	if (!bAttached)
	{
		OutError = TEXT("attach_failed");
		return false;
	}

	OutData->SetStringField(TEXT("child_name"), ChildActor->GetName());
	OutData->SetStringField(TEXT("parent_name"), ParentActor->GetName());
	OutData->SetStringField(TEXT("attach_parent"), ParentActor->GetName());
	return true;
}

bool FUeAgentHttpServer::CmdLevelDetachActor(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	Actor->Modify();
	Actor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);

	OutData->SetStringField(TEXT("actor_name"), Actor->GetName());
	OutData->SetBoolField(TEXT("detached"), true);
	return true;
}

bool FUeAgentHttpServer::CmdLevelGetSelection(TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!GEditor)
	{
		OutError = TEXT("no_geditor");
		return false;
	}

	USelection* SelectedActors = GEditor->GetSelectedActors();
	if (!SelectedActors)
	{
		OutError = TEXT("selected_actors_unavailable");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Items;
	for (FSelectionIterator It(*SelectedActors); It; ++It)
	{
		AActor* Actor = Cast<AActor>(*It);
		if (!Actor)
		{
			continue;
		}
		Items.Add(MakeShared<FJsonValueObject>(UeAgentLevelOps::MakeActorSummaryJson(Actor)));
	}

	OutData->SetNumberField(TEXT("count"), Items.Num());
	OutData->SetArrayField(TEXT("actors"), Items);
	return true;
}

bool FUeAgentHttpServer::CmdLevelSetSelection(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	const TArray<TSharedPtr<FJsonValue>>* ActorValues = nullptr;
	if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetArrayField(TEXT("actor_ids"), ActorValues) || !ActorValues)
	{
		OutError = TEXT("missing_actor_ids");
		return false;
	}

	FString Mode = TEXT("replace");
	JsonTryGetString(Ctx.Params, TEXT("mode"), Mode);
	Mode = Mode.ToLower();

	USelection* SelectedActors = GEditor->GetSelectedActors();
	if (!SelectedActors)
	{
		OutError = TEXT("selected_actors_unavailable");
		return false;
	}

	SelectedActors->BeginBatchSelectOperation();
	if (Mode == TEXT("replace"))
	{
		GEditor->SelectNone(false, true, false);
	}

	int32 ChangedCount = 0;
	for (const TSharedPtr<FJsonValue>& Value : *ActorValues)
	{
		if (!Value.IsValid())
		{
			continue;
		}
		const FString ActorId = Value->AsString();
		if (ActorId.IsEmpty())
		{
			continue;
		}

		AActor* Actor = FindActorByNameOrLabel(World, ActorId);
		if (!Actor)
		{
			continue;
		}

		const bool bSelect = (Mode != TEXT("remove"));
		GEditor->SelectActor(Actor, bSelect, false, true);
		++ChangedCount;
	}
	SelectedActors->EndBatchSelectOperation();
	GEditor->NoteSelectionChange();

	OutData->SetStringField(TEXT("mode"), Mode);
	OutData->SetNumberField(TEXT("changed"), ChangedCount);
	return CmdLevelGetSelection(OutData, OutError);
}

bool FUeAgentHttpServer::CmdLevelDuplicateActor(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	FVector Offset = FVector::ZeroVector;
	JsonTryGetVector(Ctx.Params, TEXT("offset"), Offset);

	UEditorActorSubsystem* ActorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;
	if (!ActorSubsystem)
	{
		OutError = TEXT("editor_actor_subsystem_unavailable");
		return false;
	}

	AActor* DuplicatedActor = ActorSubsystem->DuplicateActor(Actor, World, Offset);
	if (!DuplicatedActor)
	{
		OutError = TEXT("duplicate_failed");
		return false;
	}

	OutData->SetObjectField(TEXT("source_actor"), UeAgentLevelOps::MakeActorSummaryJson(Actor));
	OutData->SetObjectField(TEXT("duplicated_actor"), UeAgentLevelOps::MakeActorSummaryJson(DuplicatedActor));
	OutData->SetObjectField(TEXT("location"), VecToJson(DuplicatedActor->GetActorLocation()));
	return true;
}

bool FUeAgentHttpServer::CmdLevelSetActorFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FString Id;
	FString FolderPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("missing_id");
		return false;
	}
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath))
	{
		OutError = TEXT("missing_folder_path");
		return false;
	}

	AActor* Actor = FindActorByNameOrLabel(World, Id);
	if (!Actor)
	{
		OutError = TEXT("actor_not_found");
		return false;
	}

	Actor->Modify();
	Actor->SetFolderPath(*FolderPath);
	OutData->SetStringField(TEXT("actor_name"), Actor->GetName());
	OutData->SetStringField(TEXT("folder_path"), Actor->GetFolderPath().ToString());
	return true;
}

bool FUeAgentHttpServer::CmdLevelAddActorTag(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FString Id;
	FString Tag;
	if (!JsonTryGetString(Ctx.Params, TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("missing_id");
		return false;
	}
	if (!JsonTryGetString(Ctx.Params, TEXT("tag"), Tag) || Tag.IsEmpty())
	{
		OutError = TEXT("missing_tag");
		return false;
	}

	AActor* Actor = FindActorByNameOrLabel(World, Id);
	if (!Actor)
	{
		OutError = TEXT("actor_not_found");
		return false;
	}

	Actor->Modify();
	const FName TagName(*Tag);
	if (!Actor->Tags.Contains(TagName))
	{
		Actor->Tags.Add(TagName);
	}

	TArray<TSharedPtr<FJsonValue>> Tags;
	for (const FName& ExistingTag : Actor->Tags)
	{
		Tags.Add(MakeShared<FJsonValueString>(ExistingTag.ToString()));
	}

	OutData->SetStringField(TEXT("actor_name"), Actor->GetName());
	OutData->SetArrayField(TEXT("tags"), Tags);
	return true;
}

bool FUeAgentHttpServer::CmdLevelGetActorTransform(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	FUeAgentInterfaceLogger::Log(TEXT("CmdLevelGetActorTransform req=%s actor=%s"), *Ctx.RequestId, *Id);
	UeAgentLevelOps::FillActorTransformFields(Actor, OutData);
	return true;
}

bool FUeAgentHttpServer::CmdLevelSetActorTransform(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	FVector NewLocation = Actor->GetActorLocation();
	FRotator NewRotation = Actor->GetActorRotation();
	FVector NewScale = Actor->GetActorScale3D();
	const bool bHasLocation = JsonTryGetVector(Ctx.Params, TEXT("location"), NewLocation);
	const bool bHasRotation = JsonTryGetRotator(Ctx.Params, TEXT("rotation"), NewRotation);
	const bool bHasScale = JsonTryGetVector(Ctx.Params, TEXT("scale"), NewScale);
	if (!bHasLocation && !bHasRotation && !bHasScale)
	{
		OutError = TEXT("missing_transform_fields");
		return false;
	}

	FUeAgentInterfaceLogger::Log(TEXT("CmdLevelSetActorTransform req=%s actor=%s has_location=%s has_rotation=%s has_scale=%s"),
		*Ctx.RequestId, *Id, bHasLocation ? TEXT("true") : TEXT("false"), bHasRotation ? TEXT("true") : TEXT("false"), bHasScale ? TEXT("true") : TEXT("false"));

	bool bCollisionAware = false;
	JsonTryGetBool(Ctx.Params, TEXT("collision_aware"), bCollisionAware);

	TSharedPtr<FJsonObject> CollisionParams;
	JsonTryGetObject(Ctx.Params, TEXT("collision"), CollisionParams);

	FVector FinalLocation = NewLocation;
	TSharedPtr<FJsonObject> CollisionResult = MakeShared<FJsonObject>();
	if (bCollisionAware && bHasLocation)
	{
		ECollisionChannel TraceChannel = ECC_Visibility;
		if (CollisionParams.IsValid() && !UeAgentLevelOps::ResolveTraceChannel(CollisionParams, TraceChannel))
		{
			OutError = TEXT("unsupported_trace_channel");
			return false;
		}

		bool bTraceComplex = false;
		if (CollisionParams.IsValid())
		{
			CollisionParams->TryGetBoolField(TEXT("trace_complex"), bTraceComplex);
		}

		double MaxAllowedOverlapsD = 0.0;
		if (CollisionParams.IsValid())
		{
			JsonTryGetNumber(CollisionParams, TEXT("max_allowed_overlaps"), MaxAllowedOverlapsD);
		}
		const int32 MaxAllowedOverlaps = (int32)FMath::Clamp(MaxAllowedOverlapsD, 0.0, 100000.0);

		bool bIncludeOverlaps = false;
		if (CollisionParams.IsValid())
		{
			CollisionParams->TryGetBoolField(TEXT("include_overlaps"), bIncludeOverlaps);
		}

		int32 OverlapLimit = 10;
		if (CollisionParams.IsValid())
		{
			double OverlapLimitD = (double)OverlapLimit;
			JsonTryGetNumber(CollisionParams, TEXT("overlap_limit"), OverlapLimitD);
			OverlapLimit = (int32)FMath::Clamp(OverlapLimitD, 0.0, 5000.0);
		}

		bool bAutoFallback = true;
		if (CollisionParams.IsValid())
		{
			CollisionParams->TryGetBoolField(TEXT("auto_fallback"), bAutoFallback);
		}

		double FallbackStepCmD = 10.0;
		if (CollisionParams.IsValid())
		{
			JsonTryGetNumber(CollisionParams, TEXT("fallback_step_cm"), FallbackStepCmD);
		}
		const double FallbackStepCm = FMath::Clamp(FallbackStepCmD, 0.1, 100000.0);

		TArray<FVector> FallbackOffsets;
		if (CollisionParams.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* OffsetsJson = nullptr;
			if (CollisionParams->TryGetArrayField(TEXT("fallback_offsets_cm"), OffsetsJson) && OffsetsJson)
			{
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
					if (Obj->HasField(TEXT("x")) || Obj->HasField(TEXT("X")))
					{
						double X = 0.0;
						double Y = 0.0;
						double Z = 0.0;
						if (!UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("x"), TEXT("X") }, X)
							|| !UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("y"), TEXT("Y") }, Y)
							|| !UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("z"), TEXT("Z") }, Z))
						{
							return false;
						}
						OutVec = FVector((float)X, (float)Y, (float)Z);
						return true;
					}

					const TSharedPtr<FJsonObject>* OffsetObjPtr = nullptr;
					if (Obj->TryGetObjectField(TEXT("offset_cm"), OffsetObjPtr) && OffsetObjPtr && OffsetObjPtr->IsValid())
					{
						double X = 0.0;
						double Y = 0.0;
						double Z = 0.0;
						if (!UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*OffsetObjPtr, { TEXT("x"), TEXT("X") }, X)
							|| !UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*OffsetObjPtr, { TEXT("y"), TEXT("Y") }, Y)
							|| !UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*OffsetObjPtr, { TEXT("z"), TEXT("Z") }, Z))
						{
							return false;
						}
						OutVec = FVector((float)X, (float)Y, (float)Z);
						return true;
					}

					return false;
				};

				for (const TSharedPtr<FJsonValue>& Value : *OffsetsJson)
				{
					FVector Offset = FVector::ZeroVector;
					if (TryGetVectorFromValue(Value, Offset))
					{
						FallbackOffsets.Add(Offset);
					}
				}
			}
		}

		if (FallbackOffsets.Num() == 0)
		{
			FallbackOffsets.Add(FVector::ZeroVector);
			if (bAutoFallback)
			{
				const float Step = (float)FallbackStepCm;
				FallbackOffsets.Add(FVector(Step, 0.0f, 0.0f));
				FallbackOffsets.Add(FVector(-Step, 0.0f, 0.0f));
				FallbackOffsets.Add(FVector(0.0f, Step, 0.0f));
				FallbackOffsets.Add(FVector(0.0f, -Step, 0.0f));
				FallbackOffsets.Add(FVector(0.0f, 0.0f, Step));
				FallbackOffsets.Add(FVector(0.0f, 0.0f, -Step));
			}
		}

		FCollisionShape Shape;
		FQuat RotationQuat = FQuat::Identity;
		bool bShapeDerivedFromBounds = false;

		FString ShapeName;
		if (CollisionParams.IsValid())
		{
			JsonTryGetString(CollisionParams, TEXT("shape"), ShapeName);
		}
		const FString ShapeNormalized = ShapeName.TrimStartAndEnd().ToLower();
		if (ShapeNormalized == TEXT("box"))
		{
			FVector Extent = FVector::ZeroVector;
			if (!CollisionParams.IsValid() || !JsonTryGetVector(CollisionParams, TEXT("box_extent"), Extent))
			{
				OutError = TEXT("collision_missing_box_extent");
				CollisionResult->SetStringField(TEXT("error"), OutError);
				OutData->SetObjectField(TEXT("collision_result"), CollisionResult);
				return false;
			}
			Extent.X = (float)FMath::Max(0.1, (double)Extent.X);
			Extent.Y = (float)FMath::Max(0.1, (double)Extent.Y);
			Extent.Z = (float)FMath::Max(0.1, (double)Extent.Z);
			Shape = FCollisionShape::MakeBox(Extent);

			FRotator CollisionRotation = FRotator::ZeroRotator;
			if (JsonTryGetRotator(CollisionParams, TEXT("rotation"), CollisionRotation))
			{
				RotationQuat = CollisionRotation.Quaternion();
			}
		}
		else if (ShapeNormalized == TEXT("sphere"))
		{
			double RadiusCm = 0.0;
			if (!CollisionParams.IsValid() || !JsonTryGetNumber(CollisionParams, TEXT("radius_cm"), RadiusCm))
			{
				OutError = TEXT("collision_missing_radius_cm");
				CollisionResult->SetStringField(TEXT("error"), OutError);
				OutData->SetObjectField(TEXT("collision_result"), CollisionResult);
				return false;
			}
			RadiusCm = FMath::Clamp(RadiusCm, 0.1, 100000.0);
			Shape = FCollisionShape::MakeSphere((float)RadiusCm);
		}
		else if (ShapeNormalized == TEXT("capsule"))
		{
			double RadiusCm = 0.0;
			double HalfHeightCm = 0.0;
			if (!CollisionParams.IsValid() || !JsonTryGetNumber(CollisionParams, TEXT("radius_cm"), RadiusCm))
			{
				OutError = TEXT("collision_missing_radius_cm");
				CollisionResult->SetStringField(TEXT("error"), OutError);
				OutData->SetObjectField(TEXT("collision_result"), CollisionResult);
				return false;
			}
			if (!JsonTryGetNumber(CollisionParams, TEXT("half_height_cm"), HalfHeightCm))
			{
				OutError = TEXT("collision_missing_half_height_cm");
				CollisionResult->SetStringField(TEXT("error"), OutError);
				OutData->SetObjectField(TEXT("collision_result"), CollisionResult);
				return false;
			}
			RadiusCm = FMath::Clamp(RadiusCm, 0.1, 100000.0);
			HalfHeightCm = FMath::Clamp(HalfHeightCm, 0.1, 100000.0);
			Shape = FCollisionShape::MakeCapsule((float)RadiusCm, (float)HalfHeightCm);

			FRotator CollisionRotation = FRotator::ZeroRotator;
			if (JsonTryGetRotator(CollisionParams, TEXT("rotation"), CollisionRotation))
			{
				RotationQuat = CollisionRotation.Quaternion();
			}
		}
		else if (!ShapeNormalized.IsEmpty())
		{
			OutError = TEXT("collision_unsupported_shape");
			CollisionResult->SetStringField(TEXT("error"), OutError);
			CollisionResult->SetStringField(TEXT("shape"), ShapeNormalized);
			OutData->SetObjectField(TEXT("collision_result"), CollisionResult);
			return false;
		}
		else
		{
			// Derive from actor bounds (world AABB).
			FBox Bounds(EForceInit::ForceInit);
			FString BoundsError;
			if (!UeAgentLevelOps::GetActorBoundsBox(Actor, Bounds, BoundsError))
			{
				OutError = TEXT("collision_actor_bounds_invalid");
				CollisionResult->SetStringField(TEXT("error"), OutError);
				CollisionResult->SetStringField(TEXT("bounds_error"), BoundsError);
				OutData->SetObjectField(TEXT("collision_result"), CollisionResult);
				return false;
			}

			double PaddingCm = 0.0;
			if (CollisionParams.IsValid())
			{
				JsonTryGetNumber(CollisionParams, TEXT("bounds_padding_cm"), PaddingCm);
				if (PaddingCm == 0.0)
				{
					JsonTryGetNumber(CollisionParams, TEXT("padding_cm"), PaddingCm);
				}
			}
			PaddingCm = FMath::Clamp(PaddingCm, -100000.0, 100000.0);

			FVector Extent = Bounds.GetExtent() + FVector((float)PaddingCm, (float)PaddingCm, (float)PaddingCm);
			Extent.X = (float)FMath::Max(0.1, (double)Extent.X);
			Extent.Y = (float)FMath::Max(0.1, (double)Extent.Y);
			Extent.Z = (float)FMath::Max(0.1, (double)Extent.Z);
			Shape = FCollisionShape::MakeBox(Extent);
			RotationQuat = FQuat::Identity;
			bShapeDerivedFromBounds = true;
		}

		TArray<AActor*> IgnoredActors;
		IgnoredActors.Add(Actor);
		if (CollisionParams.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* IgnoreIds = nullptr;
			if (CollisionParams->TryGetArrayField(TEXT("ignore_actor_ids"), IgnoreIds) && IgnoreIds)
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
		}
		UeAgentLevelOps::AppendIgnoredActorsFromFilters(World, CollisionParams, IgnoredActors);

		TArray<TSharedPtr<FJsonValue>> AttemptsJson;
		AttemptsJson.Reserve(FallbackOffsets.Num());

		bool bFound = false;
		int32 ChosenIndex = INDEX_NONE;
		FVector ChosenOffset = FVector::ZeroVector;
		int32 ChosenOverlapCount = 0;
		TArray<TSharedPtr<FJsonValue>> ChosenOverlapsJson;

		for (int32 AttemptIndex = 0; AttemptIndex < FallbackOffsets.Num(); ++AttemptIndex)
		{
			const FVector Offset = FallbackOffsets[AttemptIndex];
			const FVector Candidate = NewLocation + Offset;

			FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(UeAgentSetActorTransformCollision), bTraceComplex);
			for (AActor* Ignored : IgnoredActors)
			{
				if (Ignored)
				{
					QueryParams.AddIgnoredActor(Ignored);
				}
			}

			TArray<FOverlapResult> OverlapResults;
			World->OverlapMultiByChannel(OverlapResults, Candidate, RotationQuat, TraceChannel, Shape, QueryParams);

			TSharedPtr<FJsonObject> Attempt = MakeShared<FJsonObject>();
			Attempt->SetNumberField(TEXT("index"), AttemptIndex);
			Attempt->SetObjectField(TEXT("offset_cm"), VecToJson(Offset));
			Attempt->SetObjectField(TEXT("candidate_location"), VecToJson(Candidate));
			Attempt->SetNumberField(TEXT("overlap_count"), OverlapResults.Num());

			TArray<TSharedPtr<FJsonValue>> OverlapsJson;
			if (bIncludeOverlaps && OverlapLimit > 0)
			{
				OverlapsJson.Reserve(FMath::Min(OverlapLimit, OverlapResults.Num()));
				for (const FOverlapResult& Result : OverlapResults)
				{
					if (OverlapsJson.Num() >= OverlapLimit)
					{
						break;
					}
					TSharedPtr<FJsonObject> OverlapItem = MakeShared<FJsonObject>();
					OverlapItem->SetObjectField(TEXT("actor"), UeAgentLevelOps::MakeActorSummaryJson(Result.GetActor()));
					if (Result.GetComponent())
					{
						OverlapItem->SetStringField(TEXT("component_name"), Result.GetComponent()->GetName());
						OverlapItem->SetStringField(TEXT("component_path"), Result.GetComponent()->GetPathName());
					}
					OverlapsJson.Add(MakeShared<FJsonValueObject>(OverlapItem));
				}
			}
			if (bIncludeOverlaps)
			{
				Attempt->SetArrayField(TEXT("overlaps"), OverlapsJson);
			}

			AttemptsJson.Add(MakeShared<FJsonValueObject>(Attempt));

			if ((int32)OverlapResults.Num() <= MaxAllowedOverlaps)
			{
				bFound = true;
				ChosenIndex = AttemptIndex;
				ChosenOffset = Offset;
				ChosenOverlapCount = OverlapResults.Num();
				ChosenOverlapsJson = OverlapsJson;
				FinalLocation = Candidate;
				break;
			}
		}

		CollisionResult->SetBoolField(TEXT("collision_aware"), true);
		CollisionResult->SetObjectField(TEXT("requested_location"), VecToJson(NewLocation));
		CollisionResult->SetObjectField(TEXT("final_location"), VecToJson(FinalLocation));
		CollisionResult->SetStringField(TEXT("shape"), ShapeNormalized.IsEmpty() ? TEXT("actor_bounds_box") : ShapeNormalized);
		CollisionResult->SetBoolField(TEXT("shape_derived_from_bounds"), bShapeDerivedFromBounds);
		CollisionResult->SetStringField(TEXT("trace_channel"), FString::Printf(TEXT("%d"), (int32)TraceChannel));
		CollisionResult->SetBoolField(TEXT("trace_complex"), bTraceComplex);
		CollisionResult->SetNumberField(TEXT("max_allowed_overlaps"), MaxAllowedOverlaps);
		CollisionResult->SetBoolField(TEXT("auto_fallback"), bAutoFallback);
		CollisionResult->SetNumberField(TEXT("fallback_step_cm"), FallbackStepCm);
		CollisionResult->SetNumberField(TEXT("attempt_count"), AttemptsJson.Num());
		CollisionResult->SetArrayField(TEXT("attempts"), AttemptsJson);
		CollisionResult->SetBoolField(TEXT("fallback_used"), ChosenIndex > 0);
		CollisionResult->SetNumberField(TEXT("fallback_index"), ChosenIndex);
		CollisionResult->SetObjectField(TEXT("fallback_offset_cm"), VecToJson(ChosenOffset));
		CollisionResult->SetNumberField(TEXT("overlap_count"), ChosenOverlapCount);
		if (bIncludeOverlaps)
		{
			CollisionResult->SetArrayField(TEXT("overlaps"), ChosenOverlapsJson);
			CollisionResult->SetNumberField(TEXT("overlap_limit"), OverlapLimit);
			CollisionResult->SetBoolField(TEXT("include_overlaps"), true);
		}
		else
		{
			CollisionResult->SetBoolField(TEXT("include_overlaps"), false);
		}

		if (!bFound)
		{
			OutData->SetObjectField(TEXT("collision_result"), CollisionResult);
			OutError = TEXT("collision_aware_no_valid_location");
			return false;
		}
	}

	Actor->Modify();
	if (bHasRotation)
	{
		Actor->SetActorRotation(NewRotation, ETeleportType::TeleportPhysics);
	}
	if (bHasScale)
	{
		Actor->SetActorScale3D(NewScale);
	}
	if (bHasLocation)
	{
		Actor->SetActorLocation(bCollisionAware ? FinalLocation : NewLocation, false, nullptr, ETeleportType::TeleportPhysics);
	}

	UeAgentLevelOps::FinishActorTransformEdit(Actor);
	UeAgentLevelOps::FillActorTransformFields(Actor, OutData);
	if (bCollisionAware && bHasLocation)
	{
		OutData->SetObjectField(TEXT("collision_result"), CollisionResult);
	}
	return true;
}

bool FUeAgentHttpServer::CmdLevelSetActorLocation(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	FVector NewLocation = Actor->GetActorLocation();
	if (!JsonTryGetVector(Ctx.Params, TEXT("location"), NewLocation))
	{
		OutError = TEXT("missing_location");
		return false;
	}

	FUeAgentInterfaceLogger::Log(TEXT("CmdLevelSetActorLocation req=%s actor=%s"), *Ctx.RequestId, *Id);
	Actor->Modify();
	Actor->SetActorLocation(NewLocation, false, nullptr, ETeleportType::TeleportPhysics);
	UeAgentLevelOps::FinishActorTransformEdit(Actor);
	UeAgentLevelOps::FillActorTransformFields(Actor, OutData);
	return true;
}

bool FUeAgentHttpServer::CmdLevelSetActorRotation(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	FRotator NewRotation = Actor->GetActorRotation();
	if (!JsonTryGetRotator(Ctx.Params, TEXT("rotation"), NewRotation))
	{
		OutError = TEXT("missing_rotation");
		return false;
	}

	FUeAgentInterfaceLogger::Log(TEXT("CmdLevelSetActorRotation req=%s actor=%s"), *Ctx.RequestId, *Id);
	Actor->Modify();
	Actor->SetActorRotation(NewRotation, ETeleportType::TeleportPhysics);
	UeAgentLevelOps::FinishActorTransformEdit(Actor);
	UeAgentLevelOps::FillActorTransformFields(Actor, OutData);
	return true;
}

bool FUeAgentHttpServer::CmdLevelSetActorScale(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	FVector NewScale = Actor->GetActorScale3D();
	if (!JsonTryGetVector(Ctx.Params, TEXT("scale"), NewScale))
	{
		OutError = TEXT("missing_scale");
		return false;
	}

	FUeAgentInterfaceLogger::Log(TEXT("CmdLevelSetActorScale req=%s actor=%s"), *Ctx.RequestId, *Id);
	Actor->Modify();
	Actor->SetActorScale3D(NewScale);
	UeAgentLevelOps::FinishActorTransformEdit(Actor);
	UeAgentLevelOps::FillActorTransformFields(Actor, OutData);
	return true;
}

