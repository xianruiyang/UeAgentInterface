namespace
{
	struct FAttributeCollisionReadback
	{
		int32 TriangleCount = 0;
		int32 UvLayerCount = 0;
		int32 UvElementCount = 0;
		FVector2f UvMin = FVector2f::ZeroVector;
		FVector2f UvMax = FVector2f::ZeroVector;
		bool bHasUv = false;
		int32 NormalElementCount = 0;
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
		if (!Mesh.HasAttributes())
		{
			return;
		}
		const UE::Geometry::FDynamicMeshAttributeSet* Attributes =
			Mesh.Attributes();
		Out.UvLayerCount = Attributes->NumUVLayers();
		for (int32 LayerIndex = 0;
			LayerIndex < Attributes->NumUVLayers();
			++LayerIndex)
		{
			const UE::Geometry::FDynamicMeshUVOverlay* Uv =
				Attributes->GetUVLayer(LayerIndex);
			Out.UvElementCount += Uv->ElementCount();
			for (const int32 ElementId : Uv->ElementIndicesItr())
			{
				const FVector2f Value = Uv->GetElement(ElementId);
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
		if (const UE::Geometry::FDynamicMeshNormalOverlay* Normals =
			Attributes->PrimaryNormals())
		{
			Out.NormalElementCount = Normals->ElementCount();
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
		const TSharedPtr<FJsonObject>& Row)
	{
		const TSharedPtr<FJsonObject> Input =
			BuildTopologyPayload(Row);
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
				TEXT("area_angle_weighted"));
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
			Test->TestTrue(
				*(DescriptorId + TEXT(" writes requested UV channel")),
				After.UvLayerCount >= 3
					&& After.UvElementCount > 0);
			Test->TestTrue(
				*(DescriptorId + TEXT(" produces normalized UVs")),
				After.bHasUv
					&& After.UvMin.X >= -0.001f
					&& After.UvMin.Y >= -0.001f
					&& After.UvMax.X <= 1.001f
					&& After.UvMax.Y <= 1.001f);
		}
		else if (OperationKey == TEXT("recompute_normals"))
		{
			Test->TestTrue(
				*(DescriptorId + TEXT(" creates shared normal overlay")),
				After.NormalElementCount > 0
					&& After.NormalElementCount
						< After.TriangleCount * 3);
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
			? CreateTopologyStaticCarrier(Owner, *TargetMesh)
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

		const TSharedPtr<FJsonObject> PayloadObject =
			BuildAttributeCollisionPayload(Row);
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
