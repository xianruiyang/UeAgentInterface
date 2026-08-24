namespace
{
	struct FUvLayerReadback
	{
		int32 ElementCount = 0;
		int32 SetTriangleCount = 0;
		FVector2f Min = FVector2f::ZeroVector;
		FVector2f Max = FVector2f::ZeroVector;
		bool bHasValue = false;
		bool bAllFinite = true;
	};

	struct FAttributeCollisionReadback
	{
		int32 TriangleCount = 0;
		int32 UvLayerCount = 0;
		int32 UvElementCount = 0;
		FVector2f UvMin = FVector2f::ZeroVector;
		FVector2f UvMax = FVector2f::ZeroVector;
		bool bHasUv = false;
		TArray<FUvLayerReadback> UvLayers;
		FNormalSemanticObservation NormalObservation;
		TMap<int32, int32> MaterialIdByTriangle;
		int32 MaterialSlotCount = 0;
		TArray<FName> MaterialSlotNames;
		TArray<FString> MaterialRefs;
		int32 CollisionBoxCount = 0;
		int32 CollisionSphereCount = 0;
		int32 CollisionCapsuleCount = 0;
		int32 CollisionConvexCount = 0;
	};

	void ReadAttributeMesh(
		const FDynamicMesh3& Mesh,
		FAttributeCollisionReadback& Out)
	{
		Out.TriangleCount = Mesh.TriangleCount();
		Out.NormalObservation = ObserveNormalSemantics(Mesh);
		if (!Mesh.HasAttributes())
		{
			return;
		}
		const UE::Geometry::FDynamicMeshAttributeSet* Attributes =
			Mesh.Attributes();
		Out.UvLayerCount = Attributes->NumUVLayers();
		Out.UvLayers.SetNum(Out.UvLayerCount);
		for (int32 LayerIndex = 0;
			LayerIndex < Attributes->NumUVLayers();
			++LayerIndex)
		{
			const UE::Geometry::FDynamicMeshUVOverlay* Uv =
				Attributes->GetUVLayer(LayerIndex);
			FUvLayerReadback& Layer = Out.UvLayers[LayerIndex];
			Layer.ElementCount = Uv->ElementCount();
			Out.UvElementCount += Uv->ElementCount();
			for (const int32 TriangleId : Mesh.TriangleIndicesItr())
			{
				Layer.SetTriangleCount +=
					Uv->IsSetTriangle(TriangleId) ? 1 : 0;
			}
			for (const int32 ElementId : Uv->ElementIndicesItr())
			{
				const FVector2f Value = Uv->GetElement(ElementId);
				Layer.bAllFinite = Layer.bAllFinite
					&& FMath::IsFinite(Value.X)
					&& FMath::IsFinite(Value.Y);
				if (!Layer.bHasValue)
				{
					Layer.Min = Value;
					Layer.Max = Value;
					Layer.bHasValue = true;
				}
				else
				{
					Layer.Min.X = FMath::Min(Layer.Min.X, Value.X);
					Layer.Min.Y = FMath::Min(Layer.Min.Y, Value.Y);
					Layer.Max.X = FMath::Max(Layer.Max.X, Value.X);
					Layer.Max.Y = FMath::Max(Layer.Max.Y, Value.Y);
				}
				if (!Out.bHasUv)
				{
					Out.UvMin = Value;
					Out.UvMax = Value;
					Out.bHasUv = true;
				}
				else
				{
					Out.UvMin.X = FMath::Min(Out.UvMin.X, Value.X);
					Out.UvMin.Y = FMath::Min(Out.UvMin.Y, Value.Y);
					Out.UvMax.X = FMath::Max(Out.UvMax.X, Value.X);
					Out.UvMax.Y = FMath::Max(Out.UvMax.Y, Value.Y);
				}
			}
		}
		if (const UE::Geometry::FDynamicMeshMaterialAttribute* Materials =
			Attributes->GetMaterialID())
		{
			for (const int32 TriangleId : Mesh.TriangleIndicesItr())
			{
				Out.MaterialIdByTriangle.Add(
					TriangleId,
					Materials->GetValue(TriangleId));
			}
		}
	}

	void ReadCollision(
		const UBodySetup* BodySetup,
		FAttributeCollisionReadback& Out)
	{
		if (!BodySetup)
		{
			return;
		}
		const FKAggregateGeom& Geometry = BodySetup->AggGeom;
		Out.CollisionBoxCount = Geometry.BoxElems.Num();
		Out.CollisionSphereCount = Geometry.SphereElems.Num();
		Out.CollisionCapsuleCount =
			Geometry.SphylElems.Num()
			+ Geometry.TaperedCapsuleElems.Num();
		Out.CollisionConvexCount = Geometry.ConvexElems.Num();
	}

	bool ReadAttributeCollisionDynamic(
		ADynamicMeshActor& Actor,
		FAttributeCollisionReadback& Out)
	{
		Out = FAttributeCollisionReadback();
		UDynamicMeshComponent* Component =
			Actor.GetDynamicMeshComponent();
		UDynamicMesh* DynamicMesh =
			Component ? Component->GetDynamicMesh() : nullptr;
		if (!Component || !DynamicMesh)
		{
			return false;
		}
		DynamicMesh->ProcessMesh(
			[&](const FDynamicMesh3& Mesh)
			{
				ReadAttributeMesh(Mesh, Out);
			});
		Out.MaterialSlotCount = Component->GetNumMaterials();
		Out.MaterialSlotNames = Component->GetMaterialSlotNames();
		for (int32 Index = 0;
			Index < Component->GetNumMaterials();
			++Index)
		{
			UMaterialInterface* Material = Component->GetMaterial(Index);
			Out.MaterialRefs.Add(
				Material ? Material->GetPathName() : FString());
		}
		ReadCollision(Component->GetBodySetup(), Out);
		return true;
	}

	bool ReadAttributeCollisionStatic(
		UStaticMesh& StaticMesh,
		FAttributeCollisionReadback& Out)
	{
		Out = FAttributeCollisionReadback();
		const FMeshDescription* Description =
			StaticMesh.GetMeshDescription(0);
		if (!Description)
		{
			return false;
		}
		FDynamicMesh3 Mesh;
		FMeshDescriptionToDynamicMesh Converter;
		Converter.Convert(Description, Mesh);
		ReadAttributeMesh(Mesh, Out);
		Out.MaterialSlotCount =
			StaticMesh.GetStaticMaterials().Num();
		for (const FStaticMaterial& Material
			: StaticMesh.GetStaticMaterials())
		{
			Out.MaterialSlotNames.Add(Material.MaterialSlotName);
			Out.MaterialRefs.Add(
				Material.MaterialInterface
					? Material.MaterialInterface->GetPathName()
					: FString());
		}
		ReadCollision(StaticMesh.GetBodySetup(), Out);
		return true;
	}

	void PrepareCollisionCarrier(UDynamicMesh& DynamicMesh)
	{
		DynamicMesh.EditMesh(
			[](FDynamicMesh3& Mesh)
			{
				TArray<int32> SourceVertices;
				TArray<int32> SourceTriangles;
				for (const int32 VertexId : Mesh.VertexIndicesItr())
				{
					SourceVertices.Add(VertexId);
				}
				for (const int32 TriangleId : Mesh.TriangleIndicesItr())
				{
					SourceTriangles.Add(TriangleId);
				}
				TMap<int32, int32> VertexMap;
				for (const int32 VertexId : SourceVertices)
				{
					VertexMap.Add(
						VertexId,
						Mesh.AppendVertex(
							Mesh.GetVertex(VertexId)
								+ FVector3d(250.0, 0.0, 0.0)));
				}
				for (const int32 TriangleId : SourceTriangles)
				{
					const UE::Geometry::FIndex3i Triangle =
						Mesh.GetTriangle(TriangleId);
					Mesh.AppendTriangle(
						VertexMap[Triangle.A],
						VertexMap[Triangle.B],
						VertexMap[Triangle.C]);
				}
			});
	}

	TSharedPtr<FJsonObject> AttributeProjectionFrame()
	{
		const TSharedPtr<FJsonObject> Value =
			MakeShared<FJsonObject>();
		Value->SetStringField(
			TEXT("frame_id"),
			TEXT("ue.mesh.target_local"));
		Value->SetStringField(TEXT("unit"), TEXT("centimeter"));
		Value->SetArrayField(
			TEXT("origin"),
			Numbers({0.0, 0.0, 0.0}));
		Value->SetArrayField(
			TEXT("axis_u"),
			Numbers({1.0, 0.0, 0.0}));
		Value->SetArrayField(
			TEXT("axis_v"),
			Numbers({0.0, 1.0, 0.0}));
		Value->SetArrayField(
			TEXT("axis_w"),
			Numbers({0.0, 0.0, 1.0}));
		Value->SetStringField(
			TEXT("basis_evidence"),
			TEXT("exact target-local orthonormal basis"));
		return Value;
	}

	TSharedPtr<FJsonObject> BuildAttributeCollisionPayload(
		const TSharedPtr<FJsonObject>& Row,
		UObject& TargetCarrier)
	{
		const TSharedPtr<FJsonObject> Input =
			BuildTopologyPayload(Row, TargetCarrier, nullptr);
		const FString OperationKey =
			Row->GetStringField(TEXT("operation_key"));
		if (OperationKey == TEXT("add_material_slot"))
		{
			Input->SetStringField(
				TEXT("slot_name"),
				TEXT("SA008_Slot"));
			Input->SetStringField(
				TEXT("material_ref"),
				TEXT("uai://material/default"));
		}
		else if (OperationKey == TEXT("auto_uv"))
		{
			Input->SetNumberField(TEXT("uv_channel"), 2.0);
			Input->SetBoolField(TEXT("pack"), true);
			Input->SetNumberField(
				TEXT("texture_resolution"),
				512.0);
		}
		else if (OperationKey == TEXT("project_uv"))
		{
			Input->SetNumberField(TEXT("uv_channel"), 2.0);
			Input->SetObjectField(
				TEXT("projection_frame"),
				AttributeProjectionFrame());
			Input->SetArrayField(
				TEXT("dimensions_cm"),
				Numbers({200.0, 100.0, 50.0}));
		}
		else if (OperationKey == TEXT("recompute_normals"))
		{
			Input->SetStringField(
				TEXT("mode"),
				TEXT("face"));
			Input->SetBoolField(
				TEXT("preserve_existing_hard_edges"),
				false);
		}
		else if (OperationKey == TEXT("assign_material_slot"))
		{
			Input->SetNumberField(TEXT("slot_index"), 1.0);
		}
		else if (OperationKey == TEXT("generate_convex_collision"))
		{
			Input->SetStringField(
				TEXT("mode"),
				TEXT("decomposition"));
			Input->SetNumberField(TEXT("max_hulls"), 4.0);
			Input->SetNumberField(
				TEXT("max_vertices_per_hull"),
				64.0);
			Input->SetBoolField(TEXT("replace_existing"), true);
		}
		else if (OperationKey == TEXT("generate_simple_collision"))
		{
			Input->SetStringField(
				TEXT("method"),
				TEXT("aligned_boxes"));
			Input->SetNumberField(TEXT("max_shapes"), 4.0);
			Input->SetBoolField(TEXT("replace_existing"), true);
		}
		return Input;
	}

	bool VerifyAttributeCollisionResult(
		FAutomationTestBase* Test,
		const FString& DescriptorId,
		const FString& OperationKey,
		const TSharedPtr<FJsonObject>& Input,
		const FAttributeCollisionReadback& Before,
		const FAttributeCollisionReadback& After)
	{
		if (OperationKey == TEXT("add_material_slot"))
		{
			Test->TestEqual(
				*(DescriptorId + TEXT(" adds exactly one slot")),
				After.MaterialSlotCount,
				Before.MaterialSlotCount + 1);
			Test->TestTrue(
				*(DescriptorId + TEXT(" preserves requested slot name")),
				After.MaterialSlotNames.Contains(
					FName(TEXT("SA008_Slot"))));
			Test->TestTrue(
				*(DescriptorId + TEXT(" binds requested material")),
				After.MaterialRefs.Contains(
					UMaterial::GetDefaultMaterial(MD_Surface)
						->GetPathName()));
		}
		else if (OperationKey == TEXT("auto_uv")
			|| OperationKey == TEXT("project_uv"))
		{
			const int32 Channel = static_cast<int32>(
				Input->GetNumberField(TEXT("uv_channel")));
			const FUvLayerReadback* Layer =
				After.UvLayers.IsValidIndex(Channel)
					? &After.UvLayers[Channel]
					: nullptr;
			Test->TestTrue(
				*(DescriptorId + TEXT(" writes requested UV channel")),
				Layer
					&& Layer->ElementCount > 0
					&& Layer->SetTriangleCount == After.TriangleCount);
			Test->TestTrue(
				*(DescriptorId + TEXT(" produces normalized UVs")),
				Layer
					&& Layer->bHasValue
					&& Layer->bAllFinite
					&& Layer->Min.X >= -0.001f
					&& Layer->Min.Y >= -0.001f
					&& Layer->Max.X <= 1.001f
					&& Layer->Max.Y <= 1.001f);
		}
		else if (OperationKey == TEXT("recompute_normals"))
		{
			const FNormalSemanticObservation& Observation =
				After.NormalObservation;
			Test->TestTrue(
				*(DescriptorId + TEXT(" writes face normals on every corner")),
				IsCompleteFaceNormalObservation(Observation));
		}
		else if (OperationKey == TEXT("assign_material_slot"))
		{
			const TSharedPtr<FJsonObject> Selection =
				Input->GetObjectField(TEXT("selection"));
			for (const TSharedPtr<FJsonValue>& Value
				: Selection->GetArrayField(TEXT("element_ids")))
			{
				const int32 TriangleId =
					static_cast<int32>(Value->AsNumber());
				const int32* MaterialId =
					After.MaterialIdByTriangle.Find(TriangleId);
				Test->TestTrue(
					*FString::Printf(
						TEXT("%s assigns triangle %d"),
						*DescriptorId,
						TriangleId),
					MaterialId && *MaterialId == 1);
			}
		}
		else if (OperationKey == TEXT("generate_convex_collision"))
		{
			Test->TestTrue(
				*(DescriptorId + TEXT(" creates only component hulls")),
				After.CollisionConvexCount >= 2
					&& After.CollisionBoxCount == 0
					&& After.CollisionSphereCount == 0
					&& After.CollisionCapsuleCount == 0);
		}
		else
		{
			Test->TestTrue(
				*(DescriptorId + TEXT(" creates two aligned boxes")),
				After.CollisionBoxCount == 2
					&& After.CollisionConvexCount == 0
					&& After.CollisionSphereCount == 0
					&& After.CollisionCapsuleCount == 0);
		}
		return !Test->HasAnyErrors();
	}

	void SeedMixedHardEdgeNormals(UDynamicMesh& DynamicMesh)
	{
		DynamicMesh.EditMesh(
			[](FDynamicMesh3& Mesh)
			{
				if (!Mesh.HasAttributes())
				{
					Mesh.EnableAttributes();
				}
				FDynamicMeshNormalOverlay* Normals =
					Mesh.Attributes()->PrimaryNormals();
				const double DotThreshold = FMath::Cos(
					FMath::DegreesToRadians(30.0));
				Normals->CreateFromPredicate(
					[&Mesh, DotThreshold](
						int32,
						const int32 FirstTriangleId,
						const int32 SecondTriangleId)
					{
						return FVector3d::DotProduct(
							Mesh.GetTriNormal(FirstTriangleId),
							Mesh.GetTriNormal(SecondTriangleId))
							> DotThreshold;
					},
					0.0f);
				int32 ElementOrdinal = 0;
				for (const int32 ElementId
					: Normals->ElementIndicesItr())
				{
					const float DirectionAngle =
						0.37f * static_cast<float>(++ElementOrdinal);
					const FVector3f WrongIslandDirection(
						0.6f * FMath::Cos(DirectionAngle),
						0.6f * FMath::Sin(DirectionAngle),
						0.8f);
					Normals->SetElement(
						ElementId,
						WrongIslandDirection);
				}
			});
	}

	bool HasCompleteMixedHardEdgeSeed(
		const FNormalSemanticObservation& Observation)
	{
		return Observation.InteriorEdgeCount > 0
			&& Observation.InvalidInteriorEdgeCount == 0
			&& Observation.PartiallySplitInteriorEdgeCount == 0
			&& Observation.FullySmoothInteriorEdgeCount > 0
			&& Observation.FullySplitInteriorEdgeCount > 0
			&& Observation.SplitInteriorEdgeEndpointKeys.Num() > 0
			&& Observation.SplitInteriorEdgeEndpointKeys.Num()
				< Observation.InteriorEdgeCount * 2
			&& Observation.ReferencedNormalElementCount > 0
			&& Observation.WeightedMisalignedNormalElementCount
				== Observation.ReferencedNormalElementCount;
	}

	bool HasCompletePreservedHardEdgeObservation(
		const FNormalSemanticObservation& Before,
		const FNormalSemanticObservation& After)
	{
		return HasCompleteMixedHardEdgeSeed(Before)
			&& IsCompleteWeightedNormalObservation(After)
			&& After.InteriorEdgeCount == Before.InteriorEdgeCount
			&& After.InvalidInteriorEdgeCount == 0
			&& After.PartiallySplitInteriorEdgeCount == 0
			&& After.FullySmoothInteriorEdgeCount
				== Before.FullySmoothInteriorEdgeCount
			&& After.FullySplitInteriorEdgeCount
				== Before.FullySplitInteriorEdgeCount
			&& After.SplitInteriorEdgeEndpointKeys
				== Before.SplitInteriorEdgeEndpointKeys;
	}

	bool RunNormalsLiveCase(
		FAutomationTestBase* Test,
		const bool bStaticCarrier,
		const FString& Mode,
		const bool bPreserveExistingHardEdges,
		const TOptional<double>& AngleDegrees,
		const FString& Scope)
	{
		const TSharedPtr<FJsonObject> Fixture =
			LoadTopologyFixture(Test);
		TSharedPtr<FJsonObject> Row;
		if (Fixture.IsValid())
		{
			for (const TSharedPtr<FJsonValue>& Value
				: Fixture->GetArrayField(TEXT("descriptors")))
			{
				const TSharedPtr<FJsonObject> Candidate =
					Value->AsObject();
				if (Candidate.IsValid()
					&& Candidate->GetStringField(TEXT("capability_id"))
						== TEXT("mesh.attribute.normals.recompute"))
				{
					Row = Candidate;
					break;
				}
			}
		}
		if (!Test->TestNotNull(
				*(Scope + TEXT(" descriptor fixture exists")),
				Row.Get()))
		{
			return false;
		}

		TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
		FString RegistryError;
		const FUeAgentSimpleAuthoringCommandRoute* Route =
			FindRoute(TEXT("mesh.attribute.normals.recompute"));
		if (!Test->TestTrue(
				*(Scope + TEXT(" descriptors compose")),
				TryCreateSimpleAuthoringDescriptorRegistry(
					Registry,
					RegistryError))
			|| !Test->TestNotNull(
				*(Scope + TEXT(" has one exact route")),
				Route)
			|| !Registry.IsValid())
		{
			return false;
		}

		FOwnedModelingWorld Owner;
		ADynamicMeshActor* Target = Owner.SpawnCubeActor(
			TEXT("SA008_Modeling_") + Scope,
			FVector(1000.0, 2000.0, 300.0),
			FRotator(0.0, 37.0, 0.0),
			FVector(1.0, 2.0, 0.5));
		UDynamicMeshComponent* TargetComponent = Target
			? Target->GetDynamicMeshComponent()
			: nullptr;
		UDynamicMesh* TargetMesh = TargetComponent
			? TargetComponent->GetDynamicMesh()
			: nullptr;
		if (TargetMesh && bPreserveExistingHardEdges)
		{
			SeedMixedHardEdgeNormals(*TargetMesh);
		}
		UStaticMesh* StaticTarget = bStaticCarrier && TargetMesh
			? CreateTopologyStaticCarrier(*Owner.Package, *TargetMesh)
			: nullptr;
		UObject* TargetCarrier = bStaticCarrier
			? static_cast<UObject*>(StaticTarget)
			: static_cast<UObject*>(Target);
		if (!Test->TestTrue(
				*(Scope + TEXT(" owns exact carrier")),
				Owner.IsValid()
					&& Target != nullptr
					&& TargetMesh != nullptr
					&& TargetCarrier != nullptr))
		{
			return false;
		}
		const FString TargetRef = TEXT("uai://mesh/fixture-target");
		const TSharedRef<FExactReferenceResolver, ESPMode::ThreadSafe>
			Resolver = MakeShared<
				FExactReferenceResolver,
				ESPMode::ThreadSafe>();
		Resolver->Objects.Add(TargetRef, TargetCarrier);

		const TSharedPtr<FJsonObject> Payload =
			BuildAttributeCollisionPayload(Row, *TargetCarrier);
		if (Payload.IsValid())
		{
			Payload->SetStringField(TEXT("mode"), Mode);
			Payload->SetBoolField(
				TEXT("preserve_existing_hard_edges"),
				bPreserveExistingHardEdges);
			if (AngleDegrees.IsSet())
			{
				Payload->SetNumberField(
					TEXT("angle_degrees"),
					AngleDegrees.GetValue());
			}
		}
		FString PayloadJson;
		if (!Test->TestTrue(
				*(Scope + TEXT(" payload serializes")),
				Payload.IsValid()
					&& Serialize(Payload, PayloadJson)))
		{
			return false;
		}

		FAttributeCollisionReadback Before;
		if (!Test->TestTrue(
				*(Scope + TEXT(" prestate reads independently")),
				bStaticCarrier
					? ReadAttributeCollisionStatic(
						*StaticTarget,
						Before)
					: ReadAttributeCollisionDynamic(*Target, Before)))
		{
			return false;
		}
		if (bPreserveExistingHardEdges
			&& !Test->TestTrue(
				*(Scope + TEXT(" mixed hard-edge seed survives carrier projection")),
				HasCompleteMixedHardEdgeSeed(
					Before.NormalObservation)))
		{
			return false;
		}
		Owner.Package->SetDirtyFlag(false);

		const TSharedRef<FRecordingDurableService, ESPMode::ThreadSafe>
			Durable = MakeShared<
				FRecordingDurableService,
				ESPMode::ThreadSafe>();
		TSharedPtr<FUeAgentSimpleAuthoringApplication> Application;
		FString ApplicationError;
		if (!Test->TestTrue(
				*(Scope + TEXT(" production application composes")),
				TryCreateTopologyApplication(
					Owner,
					Registry,
					Resolver,
					Resolver,
					Durable,
					*Route,
					Application,
					ApplicationError)))
		{
			Test->AddError(
				Scope + TEXT(":composition_failed:")
					+ ApplicationError);
			return false;
		}

		const FUeAgentOperationResult Result = Application->Submit(
			FUeAgentOperationRequest(
				Route->DescriptorIdentity,
				TEXT("request:") + Scope,
				TEXT("idempotency:") + Scope,
				FUeAgentContractValue(PayloadJson)),
			FUeAgentCallerContext(
				TEXT("caller:") + Scope,
				{TEXT("uai:modeling-geometry:write")},
				TEXT("correlation:") + Scope),
			MakeShared<FNeverCancelled>());
		if (!Test->TestTrue(
				*(Scope + TEXT(" completes verified lifecycle")),
				IsComplete(Result)))
		{
			Test->AddError(Result.GetValue().GetJsonValue());
			return false;
		}

		FAttributeCollisionReadback After;
		Test->TestTrue(
			*(Scope + TEXT(" committed carrier reads independently")),
			bStaticCarrier
				? ReadAttributeCollisionStatic(*StaticTarget, After)
				: ReadAttributeCollisionDynamic(*Target, After));
		if (Mode == TEXT("area_angle_weighted")
			&& bPreserveExistingHardEdges)
		{
			Test->TestTrue(
				*(Scope + TEXT(" preserves mixed hard-edge topology and recomputes directions")),
				HasCompletePreservedHardEdgeObservation(
					Before.NormalObservation,
					After.NormalObservation));
		}
		else if (Mode == TEXT("area_angle_weighted"))
		{
			Test->TestTrue(
				*(Scope + TEXT(" writes weighted per-vertex normals")),
				IsCompleteAreaAnglePerVertexNormalObservation(
					After.NormalObservation));
		}
		else if (Mode == TEXT("angle_threshold")
			&& AngleDegrees.IsSet())
		{
			Test->TestTrue(
				*(Scope + TEXT(" writes the requested split topology")),
				IsCompleteAngleThresholdNormalObservation(
					After.NormalObservation,
					AngleDegrees.GetValue()));
		}
		Test->TestEqual(
			*(Scope + TEXT(" prepares durable state once")),
			Durable->PrepareCount,
			1);
		Test->TestEqual(
			*(Scope + TEXT(" persists durable state once")),
			Durable->PersistCount,
			1);
		Test->TestEqual(
			*(Scope + TEXT(" acknowledges after C9 once")),
			Durable->AcknowledgeCount,
			1);
		return !Test->HasAnyErrors();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringModelingAttributeCollisionProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"ModelingAttributeCollisionProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringModelingAttributeCollisionProductionContractTest::
	RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture =
		LoadTopologyFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	FString RegistryError;
	if (!TestTrue(
			TEXT("Simple Authoring descriptors compose for attributes and collision"),
			TryCreateSimpleAuthoringDescriptorRegistry(
				Registry,
				RegistryError))
		|| !Registry.IsValid())
	{
		AddError(
			TEXT("sa008_modeling_attribute_collision_registry_invalid:")
			+ RegistryError);
		return false;
	}

	int32 Executed = 0;
	for (const TSharedPtr<FJsonValue>& Value
		: Fixture->GetArrayField(TEXT("descriptors")))
	{
		const TSharedPtr<FJsonObject> Row = Value->AsObject();
		if (!Row.IsValid())
		{
			continue;
		}
		const FString NativeKind =
			Row->GetStringField(TEXT("native_operation_type"));
		if (NativeKind != TEXT("geometry.attribute.edit")
			&& NativeKind != TEXT("geometry.collision.edit"))
		{
			continue;
		}
		++Executed;
		const FString DescriptorId =
			Row->GetStringField(TEXT("descriptor_id"));
		const FString CapabilityId =
			Row->GetStringField(TEXT("capability_id"));
		const FString OperationKey =
			Row->GetStringField(TEXT("operation_key"));
		const TSharedPtr<FJsonObject> Scenario =
			FindTopologyScenario(Fixture, DescriptorId);
		if (!TestNotNull(
				*(DescriptorId + TEXT(" has an E2 scenario")),
				Scenario.Get()))
		{
			continue;
		}
		const FUeAgentSimpleAuthoringCommandRoute* Route =
			FindRoute(CapabilityId);
		if (!TestNotNull(
				*(CapabilityId + TEXT(" has one exact route")),
				Route))
		{
			continue;
		}

		FOwnedModelingWorld Owner;
		ADynamicMeshActor* Target = Owner.SpawnCubeActor(
			TEXT("SA008_Modeling_AttributeCollision_Target"),
			FVector(1000.0, 2000.0, 300.0),
			FRotator(0.0, 37.0, 0.0),
			FVector(1.0, 2.0, 0.5));
		UDynamicMeshComponent* TargetComponent =
			Target ? Target->GetDynamicMeshComponent() : nullptr;
		UDynamicMesh* TargetMesh =
			TargetComponent
				? TargetComponent->GetDynamicMesh()
				: nullptr;
		if (!TestTrue(
				*(DescriptorId + TEXT(" owns exact target carrier")),
				Owner.IsValid() && TargetMesh != nullptr))
		{
			continue;
		}
		if (NativeKind == TEXT("geometry.collision.edit"))
		{
			PrepareCollisionCarrier(*TargetMesh);
		}
		TargetComponent->SetMaterial(
			0,
			UMaterial::GetDefaultMaterial(MD_Surface));
		TargetComponent->SetMaterial(
			1,
			UMaterial::GetDefaultMaterial(MD_Surface));

		FString CarrierKind;
		const bool bStaticCarrier =
			Row->TryGetStringField(
				TEXT("test_carrier_kind"),
				CarrierKind)
			&& CarrierKind == TEXT("static_mesh_asset");
		UStaticMesh* StaticTarget = bStaticCarrier
			? CreateTopologyStaticCarrier(*Owner.Package, *TargetMesh)
			: nullptr;
		if (bStaticCarrier
			&& !TestNotNull(
				*(DescriptorId + TEXT(" owns static carrier")),
				StaticTarget))
		{
			continue;
		}

		const TSharedRef<
			FExactReferenceResolver,
			ESPMode::ThreadSafe> Resolver =
				MakeShared<
					FExactReferenceResolver,
					ESPMode::ThreadSafe>();
		Resolver->Objects.Add(
			TEXT("uai://mesh/fixture-target"),
			bStaticCarrier
				? static_cast<UObject*>(StaticTarget)
				: static_cast<UObject*>(Target));
		Resolver->Objects.Add(
			TEXT("uai://material/default"),
			UMaterial::GetDefaultMaterial(MD_Surface));

		UObject* TargetCarrier = bStaticCarrier
			? static_cast<UObject*>(StaticTarget)
			: static_cast<UObject*>(Target);
		const TSharedPtr<FJsonObject> PayloadObject =
			TargetCarrier
				? BuildAttributeCollisionPayload(
					Row,
					*TargetCarrier)
				: nullptr;
		if (!TestNotNull(
				*(DescriptorId + TEXT(" carrier revision reads")),
				PayloadObject.Get()))
		{
			continue;
		}
		PatchTopologySelection(Row, PayloadObject, *TargetMesh);
		FString PayloadJson;
		TestTrue(
			*(DescriptorId + TEXT(" payload serializes")),
			Serialize(PayloadObject, PayloadJson));

		FAttributeCollisionReadback Before;
		if (!TestTrue(
				*(DescriptorId + TEXT(" reads prestate independently")),
				bStaticCarrier
					? ReadAttributeCollisionStatic(
						*StaticTarget,
						Before)
					: ReadAttributeCollisionDynamic(
						*Target,
						Before)))
		{
			continue;
		}
		Owner.Package->SetDirtyFlag(false);

		const TSharedRef<
			FRecordingDurableService,
			ESPMode::ThreadSafe> Durable =
				MakeShared<
					FRecordingDurableService,
					ESPMode::ThreadSafe>();
		TSharedPtr<FUeAgentSimpleAuthoringApplication> Application;
		FString ApplicationError;
		if (!TestTrue(
				*(DescriptorId + TEXT(" composes production application")),
				TryCreateTopologyApplication(
					Owner,
					Registry,
					Resolver,
					Resolver,
					Durable,
					*Route,
					Application,
					ApplicationError)))
		{
			AddError(
				DescriptorId
				+ TEXT(":production_composition_failed:")
				+ ApplicationError);
			continue;
		}

		const FUeAgentCallerContext Caller(
			TEXT("caller:sa008-modeling-attribute-collision"),
			{TEXT("uai:modeling-geometry:write")},
			TEXT("correlation:sa008-modeling-attribute-collision"));
		const FUeAgentOperationResult Result =
			Application->Submit(
				FUeAgentOperationRequest(
					Route->DescriptorIdentity,
					TEXT("request:sa008-modeling-attribute-collision:")
						+ FString::FromInt(Executed),
					TEXT("idempotency:sa008-modeling-attribute-collision:")
						+ FString::FromInt(Executed),
					FUeAgentContractValue(PayloadJson)),
				Caller,
				MakeShared<FNeverCancelled>());
		if (!IsComplete(Result))
		{
			AddError(
				DescriptorId
					+ TEXT(
						" did not complete verified lifecycle; result=")
					+ Result.GetValue().GetJsonValue());
			continue;
		}

		FAttributeCollisionReadback After;
		TestTrue(
			*(DescriptorId + TEXT(" reads committed carrier")),
			bStaticCarrier
				? ReadAttributeCollisionStatic(*StaticTarget, After)
				: ReadAttributeCollisionDynamic(*Target, After));
		VerifyAttributeCollisionResult(
			this,
			DescriptorId,
			OperationKey,
			PayloadObject,
			Before,
			After);
		TestEqual(
			*(DescriptorId + TEXT(" prepares durable state once")),
			Durable->PrepareCount,
			1);
		TestEqual(
			*(DescriptorId + TEXT(" persists durable state once")),
			Durable->PersistCount,
			1);
		TestEqual(
			*(DescriptorId + TEXT(" acknowledges after C9 once")),
			Durable->AcknowledgeCount,
			1);
	}
	TestEqual(
		TEXT("all seven attribute and collision profiles execute"),
		Executed,
		7);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringModelingDynamicFaceNormalsProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"ModelingDynamicFaceNormalsProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringModelingDynamicFaceNormalsProductionContractTest::
	RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture =
		LoadTopologyFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	TSharedPtr<FJsonObject> Row;
	for (const TSharedPtr<FJsonValue>& Value
		: Fixture->GetArrayField(TEXT("descriptors")))
	{
		const TSharedPtr<FJsonObject> Candidate = Value->AsObject();
		if (Candidate.IsValid()
			&& Candidate->GetStringField(TEXT("capability_id"))
				== TEXT("mesh.attribute.normals.recompute"))
		{
			Row = Candidate;
			break;
		}
	}
	if (!TestNotNull(
			TEXT("dynamic face normals descriptor fixture exists"),
			Row.Get()))
	{
		return false;
	}

	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	FString RegistryError;
	const FUeAgentSimpleAuthoringCommandRoute* Route =
		FindRoute(TEXT("mesh.attribute.normals.recompute"));
	if (!TestTrue(
			TEXT("dynamic face normals descriptors compose"),
			TryCreateSimpleAuthoringDescriptorRegistry(
				Registry,
				RegistryError))
		|| !TestNotNull(
			TEXT("dynamic face normals has one exact route"),
			Route)
		|| !Registry.IsValid())
	{
		return false;
	}

	FOwnedModelingWorld Owner;
	ADynamicMeshActor* Target = Owner.SpawnCubeActor(
		TEXT("SA008_Modeling_Dynamic_Face_Normals"),
		FVector(1000.0, 2000.0, 300.0),
		FRotator(0.0, 37.0, 0.0),
		FVector(1.0, 2.0, 0.5));
	if (!TestTrue(
			TEXT("dynamic face normals owns exact carrier"),
			Owner.IsValid() && Target != nullptr))
	{
		return false;
	}
	const FString TargetRef = TEXT("uai://mesh/fixture-target");
	const TSharedRef<FExactReferenceResolver, ESPMode::ThreadSafe>
		Resolver = MakeShared<
			FExactReferenceResolver,
			ESPMode::ThreadSafe>();
	Resolver->Objects.Add(TargetRef, Target);

	const TSharedPtr<FJsonObject> PayloadObject =
		BuildAttributeCollisionPayload(Row, *Target);
	FString PayloadJson;
	if (!TestNotNull(
			TEXT("dynamic face normals carrier revision reads"),
			PayloadObject.Get())
		|| !TestTrue(
			TEXT("dynamic face normals payload serializes"),
			Serialize(PayloadObject, PayloadJson)))
	{
		return false;
	}

	FAttributeCollisionReadback Before;
	if (!TestTrue(
			TEXT("dynamic face normals prestate reads independently"),
			ReadAttributeCollisionDynamic(*Target, Before)))
	{
		return false;
	}
	TestFalse(
		TEXT("dynamic carrier does not already satisfy face normals"),
		IsCompleteFaceNormalObservation(Before.NormalObservation));
	Owner.Package->SetDirtyFlag(false);

	const TSharedRef<FRecordingDurableService, ESPMode::ThreadSafe>
		Durable = MakeShared<
			FRecordingDurableService,
			ESPMode::ThreadSafe>();
	TSharedPtr<FUeAgentSimpleAuthoringApplication> Application;
	FString ApplicationError;
	if (!TestTrue(
			TEXT("dynamic face normals production application composes"),
			TryCreateTopologyApplication(
				Owner,
				Registry,
				Resolver,
				Resolver,
				Durable,
				*Route,
				Application,
				ApplicationError)))
	{
		AddError(
			TEXT("dynamic_face_normals_composition_failed:")
				+ ApplicationError);
		return false;
	}

	const FUeAgentOperationResult Result = Application->Submit(
		FUeAgentOperationRequest(
			Route->DescriptorIdentity,
			TEXT("request:sa008-modeling-dynamic-face-normals"),
			TEXT("idempotency:sa008-modeling-dynamic-face-normals"),
			FUeAgentContractValue(PayloadJson)),
		FUeAgentCallerContext(
			TEXT("caller:sa008-modeling-dynamic-face-normals"),
			{TEXT("uai:modeling-geometry:write")},
			TEXT("correlation:sa008-modeling-dynamic-face-normals")),
		MakeShared<FNeverCancelled>());
	if (!TestTrue(
			TEXT("dynamic face normals completes verified lifecycle"),
			IsComplete(Result)))
	{
		AddError(Result.GetValue().GetJsonValue());
		return false;
	}

	FAttributeCollisionReadback After;
	TestTrue(
		TEXT("dynamic face normals committed carrier reads independently"),
		ReadAttributeCollisionDynamic(*Target, After));
	TestTrue(
		TEXT("dynamic face normals writes every triangle corner"),
		IsCompleteFaceNormalObservation(After.NormalObservation));
	TestEqual(
		TEXT("dynamic face normals prepares durable state once"),
		Durable->PrepareCount,
		1);
	TestEqual(
		TEXT("dynamic face normals persists durable state once"),
		Durable->PersistCount,
		1);
	TestEqual(
		TEXT("dynamic face normals acknowledges after C9 once"),
		Durable->AcknowledgeCount,
		1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringModelingDynamicAreaAngleNormalsProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"ModelingDynamicAreaAngleNormalsProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringModelingDynamicAreaAngleNormalsProductionContractTest::
	RunTest(const FString& Parameters)
{
	return RunNormalsLiveCase(
		this,
		false,
		TEXT("area_angle_weighted"),
		false,
		TOptional<double>(),
		TEXT("sa008-modeling-dynamic-area-angle"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringModelingStaticAreaAngleNormalsProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"ModelingStaticAreaAngleNormalsProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringModelingStaticAreaAngleNormalsProductionContractTest::
	RunTest(const FString& Parameters)
{
	return RunNormalsLiveCase(
		this,
		true,
		TEXT("area_angle_weighted"),
		false,
		TOptional<double>(),
		TEXT("sa008-modeling-static-area-angle"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringModelingDynamicPreserveHardEdgesNormalsProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"ModelingDynamicPreserveHardEdgesNormalsProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringModelingDynamicPreserveHardEdgesNormalsProductionContractTest::
	RunTest(const FString& Parameters)
{
	return RunNormalsLiveCase(
		this,
		false,
		TEXT("area_angle_weighted"),
		true,
		TOptional<double>(),
		TEXT("sa008-modeling-dynamic-preserve-hard-edges"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringModelingStaticPreserveHardEdgesNormalsProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"ModelingStaticPreserveHardEdgesNormalsProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringModelingStaticPreserveHardEdgesNormalsProductionContractTest::
	RunTest(const FString& Parameters)
{
	return RunNormalsLiveCase(
		this,
		true,
		TEXT("area_angle_weighted"),
		true,
		TOptional<double>(),
		TEXT("sa008-modeling-static-preserve-hard-edges"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringModelingDynamicAngleThresholdNormalsProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"ModelingDynamicAngleThresholdNormalsProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringModelingDynamicAngleThresholdNormalsProductionContractTest::
	RunTest(const FString& Parameters)
{
	return RunNormalsLiveCase(
		this,
		false,
		TEXT("angle_threshold"),
		false,
		TOptional<double>(30.0),
		TEXT("sa008-modeling-dynamic-angle-threshold"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringModelingStaticAngleThresholdNormalsProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"ModelingStaticAngleThresholdNormalsProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringModelingStaticAngleThresholdNormalsProductionContractTest::
	RunTest(const FString& Parameters)
{
	return RunNormalsLiveCase(
		this,
		true,
		TEXT("angle_threshold"),
		false,
		TOptional<double>(30.0),
		TEXT("sa008-modeling-static-angle-threshold"));
}
