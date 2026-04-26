bool FUeAgentHttpServer::CmdModelingStartTool(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentModelingOps;
	OutData = MakeShared<FJsonObject>();

	FModelingContext Context;
	if (!GetModelingContext(Context, OutError))
	{
		return false;
	}

	FString ToolIdentifier;
	if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetStringField(TEXT("tool_identifier"), ToolIdentifier))
	{
		OutError = TEXT("tool_identifier_required");
		return false;
	}
	ToolIdentifier = ResolveToolIdentifierAlias(ToolIdentifier);
	if (ToolIdentifier.IsEmpty())
	{
		OutError = TEXT("tool_identifier_required");
		return false;
	}

	FString WrapperCommand;
	if (Ctx.Params.IsValid())
	{
		Ctx.Params->TryGetStringField(TEXT("__uai_wrapper_command"), WrapperCommand);
	}
	TSharedPtr<FJsonObject> EffectiveParams = Ctx.Params.IsValid() ? MakeShared<FJsonObject>(*Ctx.Params) : MakeShared<FJsonObject>();
	FPrimitiveBoundsPlacement PrimitivePlacement;
	TSharedPtr<FJsonObject> DerivedPrimitiveData;
	if (!PreparePrimitivePlacementParameters(WrapperCommand, ToolIdentifier, EffectiveParams, EffectiveParams, PrimitivePlacement, DerivedPrimitiveData, OutError))
	{
		return false;
	}

	TArray<AActor*> SelectionBeforeCreate;
	if (PrimitivePlacement.bEnabled)
	{
		GetSelectedActors(SelectionBeforeCreate);
	}

	if (!StartToolByIdentifier(ToolIdentifier, Context, OutError))
	{
		FUeAgentInterfaceLogger::Log(TEXT("ModelingStartToolFailed req=%s tool=%s error=%s"), *Ctx.RequestId, *ToolIdentifier, *OutError);
		return false;
	}

	UInteractiveTool* Tool = Context.ToolsContext->ToolManager ? Context.ToolsContext->ToolManager->GetActiveTool(EToolSide::Left) : nullptr;
	TArray<TSharedPtr<FJsonValue>> PropertyImportResults;
	if (Tool && !ApplyToolProperties(EffectiveParams, Tool, OutError, &PropertyImportResults))
	{
		OutData->SetArrayField(TEXT("property_import_results"), PropertyImportResults);
		return false;
	}
	if (Tool && !AutoPreparePrimitiveCreationTool(EffectiveParams, Tool, ToolIdentifier, OutError))
	{
		return false;
	}

	FString PostAction;
	if (EffectiveParams.IsValid() && EffectiveParams->TryGetStringField(TEXT("post_action"), PostAction) && !PostAction.IsEmpty())
	{
		if (!InvokeToolAction(Tool, PostAction))
		{
			OutError = TEXT("tool_action_not_found");
			return false;
		}
	}

	FlushToolsContext(Context);

	bool bAutoAccept = false;
	if (EffectiveParams.IsValid())
	{
		EffectiveParams->TryGetBoolField(TEXT("accept"), bAutoAccept);
	}
	if (bAutoAccept)
	{
		if (!FinishTool(Context, EToolShutdownType::Accept, OutError))
		{
			return false;
		}

		if (PrimitivePlacement.bEnabled)
		{
			TArray<AActor*> SelectionAfterCreate;
			GetSelectedActors(SelectionAfterCreate);
			AActor* CreatedActor = ResolveCreatedActorFromSelectionDelta(SelectionBeforeCreate, SelectionAfterCreate);
			if (!ApplyPrimitivePlacementToActor(CreatedActor, PrimitivePlacement, OutError))
			{
				return false;
			}
			ApplyOptionalCreatedActorMetadata(CreatedActor, EffectiveParams);

			auto AutoGeneratePrimitiveCollisionIfMissing = [&WrapperCommand, &DerivedPrimitiveData, &EffectiveParams, &OutData](AActor* InActor)
			{
				const bool bIsBox = WrapperCommand.Equals(TEXT("modeling_create_box"), ESearchCase::IgnoreCase);
				const bool bIsStairs = WrapperCommand.Equals(TEXT("modeling_create_stairs"), ESearchCase::IgnoreCase);
				if (!InActor || (!bIsBox && !bIsStairs))
				{
					return;
				}

				bool bAutoGenerateCollision = true;
				if (EffectiveParams.IsValid())
				{
					EffectiveParams->TryGetBoolField(TEXT("auto_generate_collision"), bAutoGenerateCollision);
				}
				if (!bAutoGenerateCollision)
				{
					if (OutData.IsValid())
					{
						OutData->SetBoolField(TEXT("auto_collision_skipped"), true);
						OutData->SetStringField(TEXT("auto_collision_skip_reason"), TEXT("auto_generate_collision_disabled"));
					}
					return;
				}

				AStaticMeshActor* StaticMeshActor = Cast<AStaticMeshActor>(InActor);
				UStaticMeshComponent* MeshComponent = StaticMeshActor ? StaticMeshActor->GetStaticMeshComponent() : nullptr;
				UStaticMesh* Mesh = MeshComponent ? MeshComponent->GetStaticMesh() : nullptr;
				if (!Mesh)
				{
					if (OutData.IsValid())
					{
						OutData->SetBoolField(TEXT("auto_collision_skipped"), true);
						OutData->SetStringField(TEXT("auto_collision_skip_reason"), TEXT("missing_static_mesh"));
					}
					return;
				}

				Mesh->CreateBodySetup();
				UBodySetup* BodySetup = Mesh->GetBodySetup();
				if (!BodySetup)
				{
					if (OutData.IsValid())
					{
						OutData->SetBoolField(TEXT("auto_collision_skipped"), true);
						OutData->SetStringField(TEXT("auto_collision_skip_reason"), TEXT("missing_body_setup"));
					}
					return;
				}

				const int32 ExistingCollisionShapeCount =
					BodySetup->AggGeom.BoxElems.Num() +
					BodySetup->AggGeom.SphereElems.Num() +
					BodySetup->AggGeom.SphylElems.Num() +
					BodySetup->AggGeom.ConvexElems.Num();
				if (ExistingCollisionShapeCount > 0)
				{
					if (OutData.IsValid())
					{
						OutData->SetBoolField(TEXT("auto_collision_skipped"), true);
						OutData->SetStringField(TEXT("auto_collision_skip_reason"), TEXT("collision_already_present"));
						OutData->SetNumberField(TEXT("auto_collision_existing_shape_count"), ExistingCollisionShapeCount);
					}
					return;
				}

				const FBoxSphereBounds MeshBounds = Mesh->GetBounds();
				const FVector Origin = MeshBounds.Origin;

				Mesh->Modify();
				BodySetup->Modify();
				BodySetup->AggGeom.BoxElems.Reset();

				if (bIsBox)
				{
					FKBoxElem BoxElem;
					BoxElem.Center = Origin;
					BoxElem.Rotation = FRotator::ZeroRotator;
					BoxElem.X = (float)FMath::Max(1.0, (double)MeshBounds.BoxExtent.X * 2.0);
					BoxElem.Y = (float)FMath::Max(1.0, (double)MeshBounds.BoxExtent.Y * 2.0);
					BoxElem.Z = (float)FMath::Max(1.0, (double)MeshBounds.BoxExtent.Z * 2.0);
					BodySetup->AggGeom.BoxElems.Add(BoxElem);

					if (OutData.IsValid())
					{
						OutData->SetBoolField(TEXT("auto_collision_generated"), true);
						OutData->SetStringField(TEXT("auto_collision_kind"), TEXT("box_bounds"));
					}
				}
				else if (bIsStairs)
				{
					double StepWidth = 0.0;
					double StepHeight = 0.0;
					double StepDepth = 0.0;
					double NumStepsNumber = 0.0;
					int32 NumSteps = 0;
					if (DerivedPrimitiveData.IsValid())
					{
						DerivedPrimitiveData->TryGetNumberField(TEXT("StepWidth"), StepWidth);
						DerivedPrimitiveData->TryGetNumberField(TEXT("StepHeight"), StepHeight);
						DerivedPrimitiveData->TryGetNumberField(TEXT("StepDepth"), StepDepth);
						DerivedPrimitiveData->TryGetNumberField(TEXT("NumSteps"), NumStepsNumber);
					}
					NumSteps = FMath::RoundToInt(NumStepsNumber);

					if (NumSteps < 2 || StepWidth <= 0.0 || StepHeight <= 0.0 || StepDepth <= 0.0)
					{
						if (OutData.IsValid())
						{
							OutData->SetBoolField(TEXT("auto_collision_skipped"), true);
							OutData->SetStringField(TEXT("auto_collision_skip_reason"), TEXT("invalid_stairs_shape_settings"));
						}
						return;
					}

					const double TotalRun = StepDepth * NumSteps;
					const double TotalRise = StepHeight * NumSteps;

					const double MinX = Origin.X - (TotalRun * 0.5);
					const double MinZ = Origin.Z - (TotalRise * 0.5);

					for (int32 StepIndex = 0; StepIndex < NumSteps; StepIndex++)
					{
						const double StepSolidHeight = StepHeight * (StepIndex + 1);

						FKBoxElem BoxElem;
						BoxElem.Center = FVector(
							(float)(MinX + StepDepth * (StepIndex + 0.5)),
							(float)Origin.Y,
							(float)(MinZ + StepSolidHeight * 0.5));
						BoxElem.Rotation = FRotator::ZeroRotator;
						BoxElem.X = (float)FMath::Max(1.0, StepDepth);
						BoxElem.Y = (float)FMath::Max(1.0, StepWidth);
						BoxElem.Z = (float)FMath::Max(1.0, StepSolidHeight);
						BodySetup->AggGeom.BoxElems.Add(BoxElem);
					}

					if (OutData.IsValid())
					{
						OutData->SetBoolField(TEXT("auto_collision_generated"), true);
						OutData->SetStringField(TEXT("auto_collision_kind"), TEXT("stairs_step_boxes"));
						OutData->SetNumberField(TEXT("auto_collision_num_steps"), NumSteps);
					}
				}

				BodySetup->InvalidatePhysicsData();
				BodySetup->CreatePhysicsMeshes();
				Mesh->MarkPackageDirty();
				Mesh->PostEditChange();

				if (OutData.IsValid())
				{
					OutData->SetStringField(TEXT("auto_collision_mesh"), Mesh->GetOutermost() ? Mesh->GetOutermost()->GetName() : FString());
					OutData->SetNumberField(TEXT("auto_collision_box_count"), BodySetup->AggGeom.BoxElems.Num());
				}
			};

			AutoGeneratePrimitiveCollisionIfMissing(CreatedActor);

			OutData->SetBoolField(TEXT("used_bounds_placement"), true);
			OutData->SetObjectField(TEXT("created_actor"), MakeActorSummaryJson(CreatedActor));
			OutData->SetObjectField(TEXT("created_actor_location"), MakeVectorJson(CreatedActor->GetActorLocation()));
			OutData->SetObjectField(TEXT("created_actor_rotation"), MakeRotatorJson(CreatedActor->GetActorRotation()));
			if (DerivedPrimitiveData.IsValid())
			{
				OutData->SetObjectField(TEXT("derived_primitive"), DerivedPrimitiveData);
			}
		}
	}

	FillActiveToolData(Context, OutData);
	if (PropertyImportResults.Num() > 0)
	{
		OutData->SetArrayField(TEXT("property_import_results"), PropertyImportResults);
	}
	OutData->SetStringField(TEXT("requested_tool_identifier"), ToolIdentifier);
	OutData->SetBoolField(TEXT("accepted"), bAutoAccept);
	FUeAgentInterfaceLogger::Log(TEXT("ModelingStartTool req=%s tool=%s accepted=%s"), *Ctx.RequestId, *ToolIdentifier, bAutoAccept ? TEXT("true") : TEXT("false"));
	return true;
}

bool FUeAgentHttpServer::CmdModelingGetActiveTool(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentModelingOps;
	OutData = MakeShared<FJsonObject>();

	FModelingContext Context;
	if (!GetModelingContext(Context, OutError))
	{
		return false;
	}

	FillActiveToolData(Context, OutData);
	return true;
}

bool FUeAgentHttpServer::CmdModelingGetActiveToolProperties(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return CmdModelingGetActiveTool(Ctx, OutData, OutError);
}

bool FUeAgentHttpServer::CmdModelingSetActiveToolProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentModelingOps;
	OutData = MakeShared<FJsonObject>();

	FModelingContext Context;
	if (!GetModelingContext(Context, OutError))
	{
		return false;
	}

	UInteractiveTool* Tool = Context.ToolsContext && Context.ToolsContext->ToolManager ? Context.ToolsContext->ToolManager->GetActiveTool(EToolSide::Left) : nullptr;
	if (!Tool)
	{
		OutError = TEXT("no_active_tool");
		return false;
	}

	FString Selector;
	FString PropertyName;
	FString ValueText;
	bool bHasValueText = false;
	if (Ctx.Params.IsValid())
	{
		Ctx.Params->TryGetStringField(TEXT("property_set"), Selector);
		Ctx.Params->TryGetStringField(TEXT("property_name"), PropertyName);
		bHasValueText = Ctx.Params->TryGetStringField(TEXT("value_text"), ValueText);
	}
	if (PropertyName.IsEmpty() || !bHasValueText)
	{
		OutError = TEXT("tool_property_name_or_value_missing");
		return false;
	}

	UObject* PropertySet = FindToolPropertySet(Tool, Selector);
	if (!PropertySet)
	{
		OutError = TEXT("tool_property_set_not_found");
		return false;
	}
	FString AppliedValueText;
	FString CppType;
	if (!ImportResolvedProperty(PropertySet, PropertyName, ValueText, OutError, &AppliedValueText, &CppType))
	{
		const FString ImportStatus = OutError.Equals(TEXT("property_not_found"), ESearchCase::CaseSensitive) ? TEXT("property_not_found") : TEXT("import_failed");
		OutData->SetStringField(TEXT("property_set"), PropertySet->GetName());
		OutData->SetStringField(TEXT("property_name"), PropertyName);
		OutData->SetStringField(TEXT("value_text"), ValueText);
		SetPropertyImportResultFields(OutData, nullptr, ValueText, TEXT(""), ImportStatus, OutError);
		OutData->SetStringField(TEXT("cpp_type"), CppType);
		return false;
	}
	NotifyToolPropertySetUpdated(Tool, PropertySet);

	FlushToolsContext(Context);
	FillActiveToolData(Context, OutData);
	OutData->SetStringField(TEXT("property_set"), PropertySet->GetName());
	OutData->SetStringField(TEXT("property_name"), PropertyName);
	OutData->SetStringField(TEXT("value_text"), ValueText);
	OutData->SetStringField(TEXT("applied_value_text"), AppliedValueText);
	SetPropertyImportResultFields(OutData, nullptr, ValueText, AppliedValueText, TEXT("imported_and_read_back"));
	OutData->SetStringField(TEXT("cpp_type"), CppType);
	FUeAgentInterfaceLogger::Log(TEXT("ModelingSetActiveToolProperty req=%s tool=%s set=%s property=%s requested=%s applied=%s cpp_type=%s exact=%s"),
		*Ctx.RequestId,
		*Context.ToolsContext->GetActiveToolName(),
		*PropertySet->GetName(),
		*PropertyName,
		*ValueText,
		*AppliedValueText,
		*CppType,
		AppliedValueText.Equals(ValueText, ESearchCase::CaseSensitive) ? TEXT("true") : TEXT("false"));
	return true;
}

bool FUeAgentHttpServer::CmdModelingInvokeActiveToolAction(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentModelingOps;
	OutData = MakeShared<FJsonObject>();

	FModelingContext Context;
	if (!GetModelingContext(Context, OutError))
	{
		return false;
	}

	UInteractiveTool* Tool = Context.ToolsContext && Context.ToolsContext->ToolManager ? Context.ToolsContext->ToolManager->GetActiveTool(EToolSide::Left) : nullptr;
	if (!Tool)
	{
		OutError = TEXT("no_active_tool");
		return false;
	}

	FString ActionName;
	if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetStringField(TEXT("action_name"), ActionName) || ActionName.IsEmpty())
	{
		OutError = TEXT("action_name_required");
		return false;
	}

	if (!InvokeToolAction(Tool, ActionName))
	{
		OutError = TEXT("tool_action_not_found");
		return false;
	}

	FlushToolsContext(Context);
	FillActiveToolData(Context, OutData);
	OutData->SetStringField(TEXT("invoked_action"), ActionName);
	FUeAgentInterfaceLogger::Log(TEXT("ModelingInvokeAction req=%s tool=%s action=%s"), *Ctx.RequestId, *Context.ToolsContext->GetActiveToolName(), *ActionName);
	return true;
}

bool FUeAgentHttpServer::CmdModelingAcceptTool(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentModelingOps;
	OutData = MakeShared<FJsonObject>();

	FModelingContext Context;
	if (!GetModelingContext(Context, OutError))
	{
		return false;
	}

	if (!FinishTool(Context, EToolShutdownType::Accept, OutError))
	{
		return false;
	}

	FillActiveToolData(Context, OutData);
	OutData->SetBoolField(TEXT("accepted"), true);
	FUeAgentInterfaceLogger::Log(TEXT("ModelingAcceptTool req=%s"), *Ctx.RequestId);
	return true;
}

bool FUeAgentHttpServer::CmdModelingCancelTool(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentModelingOps;
	OutData = MakeShared<FJsonObject>();

	FModelingContext Context;
	if (!GetModelingContext(Context, OutError))
	{
		return false;
	}

	if (!FinishTool(Context, EToolShutdownType::Cancel, OutError))
	{
		return false;
	}

	FillActiveToolData(Context, OutData);
	OutData->SetBoolField(TEXT("cancelled"), true);
	FUeAgentInterfaceLogger::Log(TEXT("ModelingCancelTool req=%s"), *Ctx.RequestId);
	return true;
}
