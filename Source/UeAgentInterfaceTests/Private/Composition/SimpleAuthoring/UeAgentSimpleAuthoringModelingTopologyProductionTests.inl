namespace
{
	struct FTopologyReadback
	{
		int32 VertexCount = 0;
		int32 TriangleCount = 0;
		int32 BoundaryEdgeCount = 0;
		FBox Bounds = FBox(EForceInit::ForceInit);
		int64 VertexChecksum = 0;

		bool operator==(const FTopologyReadback& Other) const
		{
			return VertexCount == Other.VertexCount
				&& TriangleCount == Other.TriangleCount
				&& BoundaryEdgeCount == Other.BoundaryEdgeCount
				&& Bounds.Min.Equals(Other.Bounds.Min)
				&& Bounds.Max.Equals(Other.Bounds.Max)
				&& VertexChecksum == Other.VertexChecksum;
		}
	};

	FString TopologyFixturePath()
	{
		return FPaths::ConvertRelativePathToFull(
			FPaths::Combine(
				FPaths::ProjectPluginsDir(),
				TEXT(
					"UeAgentInterface/Source/"
					"UeAgentInterfaceTests/Private/"
					"DomainAdapters/ModelingGeometryEffect/"
					"Fixtures/"
					"modeling_geometry_effect_contract_cases.json")));
	}

	TSharedPtr<FJsonObject> LoadTopologyFixture(
		FAutomationTestBase* Test)
	{
		FString Text;
		TSharedPtr<FJsonObject> Fixture;
		if (!Test->TestTrue(
				TEXT("load Modeling topology fixture"),
				FFileHelper::LoadFileToString(
					Text,
					*TopologyFixturePath())))
		{
			return nullptr;
		}
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Text);
		if (!Test->TestTrue(
				TEXT("parse Modeling topology fixture"),
				FJsonSerializer::Deserialize(Reader, Fixture)
					&& Fixture.IsValid()))
		{
			return nullptr;
		}
		return Fixture;
	}

	TSharedPtr<FJsonObject> FindTopologyScenario(
		const TSharedPtr<FJsonObject>& Fixture,
		const FString& DescriptorId)
	{
		for (const TSharedPtr<FJsonValue>& Value
			: Fixture->GetArrayField(TEXT("e2_scenarios")))
		{
			const TSharedPtr<FJsonObject> Scenario =
				Value->AsObject();
			if (Scenario.IsValid()
				&& Scenario->GetStringField(TEXT("descriptor_id"))
					== DescriptorId)
			{
				return Scenario;
			}
		}
		return nullptr;
	}

	TSharedPtr<FJsonObject> TopologySpaceContract()
	{
		const TSharedPtr<FJsonObject> Value =
			MakeShared<FJsonObject>();
		Value->SetStringField(
			TEXT("semantic_frame_id"),
			TEXT("agent.mesh.intent"));
		Value->SetStringField(
			TEXT("write_frame_id"),
			TEXT("ue.mesh.target_local"));
		Value->SetStringField(
			TEXT("read_frame_id"),
			TEXT("ue.mesh.target_local"));
		Value->SetStringField(
			TEXT("verify_frame_id"),
			TEXT("agent.mesh.intent"));
		Value->SetStringField(TEXT("unit"), TEXT("centimeter"));
		Value->SetStringField(
			TEXT("basis_evidence"),
			TEXT("exact carrier transform and geometry revision"));
		Value->SetStringField(
			TEXT("conversion_chain"),
			TEXT(
				"semantic -> C4 normalized target local -> "
				"working mesh -> carrier -> independent "
				"target-local readback"));
		return Value;
	}

	TSharedPtr<FJsonObject> TopologySelection(
		const FString& Kind)
	{
		const TSharedPtr<FJsonObject> Value =
			MakeShared<FJsonObject>();
		Value->SetStringField(TEXT("element_kind"), Kind);
		Value->SetArrayField(TEXT("element_ids"), Numbers({0.0}));
		return Value;
	}

	TSharedPtr<FJsonObject> TopologyOperandTransform()
	{
		const TSharedPtr<FJsonObject> Value =
			MakeShared<FJsonObject>();
		Value->SetStringField(
			TEXT("from_frame_id"),
			TEXT("ue.mesh.operand_local"));
		Value->SetStringField(
			TEXT("to_frame_id"),
			TEXT("ue.mesh.target_local"));
		Value->SetStringField(TEXT("unit"), TEXT("centimeter"));
		Value->SetArrayField(
			TEXT("translation"),
			Numbers({25.0, 0.0, 0.0}));
		Value->SetArrayField(
			TEXT("rotation_quaternion_xyzw"),
			Numbers({0.0, 0.0, 0.0, 1.0}));
		Value->SetArrayField(
			TEXT("scale"),
			Numbers({1.0, 1.0, 1.0}));
		Value->SetStringField(
			TEXT("basis_evidence"),
			TEXT("exact target and operand carrier transforms"));
		Value->SetStringField(
			TEXT("conversion_chain"),
			TEXT("operand local -> target local"));
		return Value;
	}

	TSharedPtr<FJsonObject> TopologyPlane()
	{
		const TSharedPtr<FJsonObject> Value =
			MakeShared<FJsonObject>();
		Value->SetStringField(
			TEXT("frame_id"),
			TEXT("ue.mesh.target_local"));
		Value->SetStringField(TEXT("unit"), TEXT("centimeter"));
		Value->SetArrayField(TEXT("origin"), Numbers({0.0, 0.0, 0.0}));
		Value->SetArrayField(TEXT("normal"), Numbers({1.0, 0.0, 0.0}));
		Value->SetStringField(
			TEXT("basis_evidence"),
			TEXT("target-local plane from C4"));
		return Value;
	}

	TSharedPtr<FJsonObject> TopologyDirection()
	{
		const TSharedPtr<FJsonObject> Value =
			MakeShared<FJsonObject>();
		Value->SetStringField(
			TEXT("frame_id"),
			TEXT("ue.mesh.target_local"));
		Value->SetArrayField(TEXT("vector"), Numbers({0.0, 0.0, 1.0}));
		Value->SetStringField(
			TEXT("basis_evidence"),
			TEXT("target-local direction from C4"));
		return Value;
	}

	TSharedPtr<FJsonObject> BuildTopologyPayload(
		const TSharedPtr<FJsonObject>& Row,
		UObject* OperandCarrier)
	{
		const TSharedPtr<FJsonObject> Input =
			MakeShared<FJsonObject>();
		Input->SetStringField(
			TEXT("target_ref"),
			TEXT("uai://mesh/fixture-target"));
		bool bValue = false;
		if (Row->TryGetBoolField(TEXT("space_contract"), bValue)
			&& bValue)
		{
			Input->SetObjectField(
				TEXT("space_contract"),
				TopologySpaceContract());
		}
		FString SelectionKind;
		if (Row->TryGetStringField(
				TEXT("selection_kind"),
				SelectionKind))
		{
			Input->SetObjectField(
				TEXT("selection"),
				TopologySelection(SelectionKind));
		}
		if (Row->TryGetBoolField(TEXT("operand"), bValue)
			&& bValue)
		{
			if (!OperandCarrier)
			{
				return nullptr;
			}
			Input->SetStringField(
				TEXT("operand_ref"),
				TEXT("uai://mesh/fixture-operand"));
			Input->SetObjectField(
				TEXT("operand_to_target"),
				TopologyOperandTransform());
		}
		if (Row->TryGetBoolField(TEXT("plane"), bValue) && bValue)
		{
			Input->SetObjectField(TEXT("plane"), TopologyPlane());
		}
		if (Row->TryGetBoolField(TEXT("direction"), bValue)
			&& bValue)
		{
			Input->SetObjectField(
				TEXT("direction"),
				TopologyDirection());
		}
		if (Row->TryGetBoolField(TEXT("edge_pairs"), bValue)
			&& bValue)
		{
			TArray<TSharedPtr<FJsonValue>> Pair = Numbers({0.0, 1.0});
			TArray<TSharedPtr<FJsonValue>> Pairs;
			Pairs.Add(MakeShared<FJsonValueArray>(MoveTemp(Pair)));
			Input->SetArrayField(TEXT("edge_pairs"), MoveTemp(Pairs));
		}

		for (const TSharedPtr<FJsonValue>& Value
			: Row->GetArrayField(TEXT("parameters")))
		{
			const TSharedPtr<FJsonObject> Spec = Value->AsObject();
			const FString Name = Spec->GetStringField(TEXT("name"));
			const FString Kind = Spec->GetStringField(TEXT("kind"));
			if (Kind == TEXT("boolean"))
			{
				Input->SetBoolField(Name, true);
			}
			else if (Kind == TEXT("enum"))
			{
				Input->SetStringField(
					Name,
					Spec->GetArrayField(TEXT("values"))[0]
						->AsString());
			}
			else if (Kind.EndsWith(TEXT("_array")))
			{
				const int32 Count = static_cast<int32>(
					Spec->GetNumberField(TEXT("count")));
				TArray<TSharedPtr<FJsonValue>> Items;
				for (int32 Index = 0; Index < Count; ++Index)
				{
					Items.Add(MakeShared<FJsonValueNumber>(1.0));
				}
				Input->SetArrayField(Name, MoveTemp(Items));
			}
			else if (Kind == TEXT("string"))
			{
				Input->SetStringField(Name, TEXT("fixture"));
			}
			else
			{
				double Minimum = 1.0;
				Spec->TryGetNumberField(TEXT("minimum"), Minimum);
				Input->SetNumberField(Name, FMath::Max(1.0, Minimum));
			}
		}

		const FString OperationKey =
			Row->GetStringField(TEXT("operation_key"));
		if (OperationKey == TEXT("bevel_edges"))
		{
			Input->SetNumberField(TEXT("distance_cm"), 7.0);
			Input->SetNumberField(TEXT("segments"), 2.0);
			Input->SetNumberField(TEXT("roundness"), 0.5);
		}
		else if (OperationKey == TEXT("duplicate_faces"))
		{
			Input->SetArrayField(
				TEXT("translation_cm"),
				Numbers({0.0, 0.0, 125.0}));
		}
		else if (OperationKey == TEXT("extrude_faces"))
		{
			Input->SetNumberField(TEXT("distance_cm"), 25.0);
			Input->SetStringField(
				TEXT("direction_mode"),
				TEXT("fixed"));
		}
		else if (OperationKey == TEXT("inset_faces"))
		{
			Input->SetNumberField(TEXT("distance_cm"), 15.0);
		}
		else if (OperationKey == TEXT("offset_faces"))
		{
			Input->SetNumberField(TEXT("distance_cm"), 20.0);
			Input->SetStringField(
				TEXT("offset_mode"),
				TEXT("face_normals"));
		}
		else if (OperationKey == TEXT("push_pull_faces"))
		{
			Input->SetNumberField(TEXT("distance_cm"), 30.0);
			Input->SetStringField(
				TEXT("direction_mode"),
				TEXT("fixed"));
		}
		else if (OperationKey == TEXT("plane_cut"))
		{
			Input->SetStringField(TEXT("keep_side"), TEXT("both"));
			Input->SetBoolField(TEXT("fill_hole"), true);
		}
		else if (OperationKey == TEXT("remesh"))
		{
			Input->SetNumberField(
				TEXT("target_edge_length_cm"),
				25.0);
			Input->SetNumberField(TEXT("iterations"), 5.0);
		}
		else if (OperationKey == TEXT("simplify"))
		{
			Input->SetStringField(
				TEXT("target_mode"),
				TEXT("triangle_ratio"));
			Input->SetNumberField(TEXT("target_value"), 0.25);
		}
		else if (OperationKey == TEXT("subdivide"))
		{
			Input->SetStringField(TEXT("scheme"), TEXT("uniform"));
			Input->SetNumberField(TEXT("levels"), 2.0);
		}
		else if (OperationKey == TEXT("voxel_boolean"))
		{
			Input->SetStringField(TEXT("operation"), TEXT("union"));
			Input->SetNumberField(TEXT("voxel_size_cm"), 10.0);
			Input->SetNumberField(TEXT("adaptivity"), 0.25);
		}
		else if (OperationKey == TEXT("weld_edges"))
		{
			Input->SetNumberField(TEXT("tolerance_cm"), 0.1);
		}
		return Input;
	}

	void PrepareTopologyMesh(
		const FString& FixtureKind,
		UDynamicMesh& DynamicMesh)
	{
		if (FixtureKind == TEXT("open_cube"))
		{
			DynamicMesh.EditMesh(
				[](FDynamicMesh3& Mesh)
				{
					if (Mesh.IsTriangle(0))
					{
						Mesh.RemoveTriangle(0, false, false);
					}
					if (Mesh.IsTriangle(2))
					{
						Mesh.RemoveTriangle(2, false, false);
					}
				});
		}
		else if (FixtureKind == TEXT("weld_seam"))
		{
			DynamicMesh.EditMesh(
				[](FDynamicMesh3& Mesh)
				{
					Mesh.Clear();
					Mesh.EnableTriangleGroups();
					const int32 A0 = Mesh.AppendVertex(
						FVector3d(0.0, 0.0, 0.0));
					const int32 A1 = Mesh.AppendVertex(
						FVector3d(100.0, 0.0, 0.0));
					const int32 A2 = Mesh.AppendVertex(
						FVector3d(0.0, 100.0, 0.0));
					const int32 B0 = Mesh.AppendVertex(
						FVector3d(100.0, 0.0, 0.0));
					const int32 B1 = Mesh.AppendVertex(
						FVector3d(100.0, 100.0, 0.0));
					const int32 B2 = Mesh.AppendVertex(
						FVector3d(0.0, 100.0, 0.0));
					Mesh.AppendTriangle(A0, A1, A2);
					Mesh.AppendTriangle(B0, B1, B2);
					Mesh.EnableAttributes();
				});
		}
	}

	void PatchTopologySelection(
		const TSharedPtr<FJsonObject>& Row,
		const TSharedPtr<FJsonObject>& Input,
		UDynamicMesh& DynamicMesh)
	{
		FString SelectionKind;
		if (Row->TryGetStringField(
				TEXT("selection_kind"),
				SelectionKind))
		{
			TArray<int32> ElementIds;
			DynamicMesh.ProcessMesh(
				[&](const FDynamicMesh3& Mesh)
				{
					if (SelectionKind == TEXT("triangle"))
					{
						double HighestZ =
							-TNumericLimits<double>::Max();
						for (const int32 TriangleId
							: Mesh.TriangleIndicesItr())
						{
							const double Z =
								Mesh.GetTriCentroid(TriangleId).Z;
							if (Z
								> HighestZ
									+ UE_DOUBLE_SMALL_NUMBER)
							{
								HighestZ = Z;
								ElementIds.Reset();
							}
							if (FMath::IsNearlyEqual(
									Z,
									HighestZ,
									UE_DOUBLE_SMALL_NUMBER)
								&& ElementIds.Num() < 2)
							{
								ElementIds.Add(TriangleId);
							}
						}
					}
					else
					{
						for (const int32 EdgeId
							: Mesh.EdgeIndicesItr())
						{
							if (SelectionKind
									== TEXT("boundary_edge")
								&& !Mesh.IsBoundaryEdge(EdgeId))
							{
								continue;
							}
							ElementIds.Add(EdgeId);
							if (ElementIds.Num() == 2)
							{
								break;
							}
						}
					}
				});
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const int32 ElementId : ElementIds)
			{
				Values.Add(
					MakeShared<FJsonValueNumber>(ElementId));
			}
			Input->GetObjectField(TEXT("selection"))
				->SetArrayField(
					TEXT("element_ids"),
					MoveTemp(Values));
		}

		bool bEdgePairs = false;
		if (Row->TryGetBoolField(TEXT("edge_pairs"), bEdgePairs)
			&& bEdgePairs)
		{
			UE::Geometry::FIndex2i WeldPair =
				UE::Geometry::FIndex2i::Invalid();
			DynamicMesh.ProcessMesh(
				[&](const FDynamicMesh3& Mesh)
				{
					TArray<int32> BoundaryEdges;
					for (const int32 EdgeId
						: Mesh.EdgeIndicesItr())
					{
						if (Mesh.IsBoundaryEdge(EdgeId))
						{
							BoundaryEdges.Add(EdgeId);
						}
					}
					for (int32 A = 0;
						A < BoundaryEdges.Num()
							&& WeldPair
								== UE::Geometry::FIndex2i::Invalid();
						++A)
					{
						const UE::Geometry::FIndex2i EdgeA =
							Mesh.GetEdgeV(BoundaryEdges[A]);
						for (int32 B = A + 1;
							B < BoundaryEdges.Num();
							++B)
						{
							const UE::Geometry::FIndex2i EdgeB =
								Mesh.GetEdgeV(BoundaryEdges[B]);
							const FVector3d A0 =
								Mesh.GetVertex(EdgeA.A);
							const FVector3d A1 =
								Mesh.GetVertex(EdgeA.B);
							const FVector3d B0 =
								Mesh.GetVertex(EdgeB.A);
							const FVector3d B1 =
								Mesh.GetVertex(EdgeB.B);
							if ((A0.Equals(B0, 0.001)
									&& A1.Equals(B1, 0.001))
								|| (A0.Equals(B1, 0.001)
									&& A1.Equals(B0, 0.001)))
							{
								WeldPair =
									UE::Geometry::FIndex2i(
										BoundaryEdges[A],
										BoundaryEdges[B]);
								break;
							}
						}
					}
				});
			if (WeldPair
				!= UE::Geometry::FIndex2i::Invalid())
			{
				TArray<TSharedPtr<FJsonValue>> Pair;
				Pair.Add(
					MakeShared<FJsonValueNumber>(WeldPair.A));
				Pair.Add(
					MakeShared<FJsonValueNumber>(WeldPair.B));
				TArray<TSharedPtr<FJsonValue>> Pairs;
				Pairs.Add(
					MakeShared<FJsonValueArray>(
						MoveTemp(Pair)));
				Input->SetArrayField(
					TEXT("edge_pairs"),
					MoveTemp(Pairs));
			}
		}
	}

	bool ReadTopologyMesh(
		const FDynamicMesh3& Mesh,
		FTopologyReadback& Out)
	{
		Out = FTopologyReadback();
		Out.VertexCount = Mesh.VertexCount();
		Out.TriangleCount = Mesh.TriangleCount();
		for (const int32 VertexId : Mesh.VertexIndicesItr())
		{
			const FVector3d Position = Mesh.GetVertex(VertexId);
			Out.Bounds += FVector(Position);
			Out.VertexChecksum +=
				FMath::RoundToInt64(Position.X * 100.0)
				+ FMath::RoundToInt64(Position.Y * 1000.0)
				+ FMath::RoundToInt64(Position.Z * 10000.0);
		}
		for (const int32 EdgeId : Mesh.EdgeIndicesItr())
		{
			if (Mesh.IsBoundaryEdge(EdgeId))
			{
				++Out.BoundaryEdgeCount;
			}
		}
		return Out.Bounds.IsValid && Out.TriangleCount > 0;
	}

	bool ReadTopologyDynamic(
		ADynamicMeshActor& Actor,
		FTopologyReadback& Out)
	{
		UDynamicMeshComponent* Component =
			Actor.GetDynamicMeshComponent();
		UDynamicMesh* DynamicMesh =
			Component ? Component->GetDynamicMesh() : nullptr;
		bool bRead = false;
		if (DynamicMesh)
		{
			DynamicMesh->ProcessMesh(
				[&](const FDynamicMesh3& Mesh)
				{
					bRead = ReadTopologyMesh(Mesh, Out);
				});
		}
		return bRead;
	}

	UStaticMesh* CreateTopologyStaticCarrier(
		UPackage& Package,
		UDynamicMesh& Source)
	{
		UStaticMesh* StaticMesh = NewObject<UStaticMesh>(
			&Package,
			*FString::Printf(
				TEXT("SM_SA008_Topology_%s"),
				*FGuid::NewGuid().ToString(EGuidFormats::Digits)),
			RF_Public | RF_Standalone | RF_Transactional);
		if (!StaticMesh)
		{
			return nullptr;
		}
		StaticMesh->SetNumSourceModels(1);
		FMeshDescription Description;
		FStaticMeshAttributes Attributes(Description);
		Attributes.Register();
		Source.ProcessMesh(
			[&](const FDynamicMesh3& Mesh)
			{
				FDynamicMeshToMeshDescription Converter;
				Converter.Convert(&Mesh, Description);
			});
		StaticMesh->CreateMeshDescription(0, Description);
		StaticMesh->CommitMeshDescription(0);
		StaticMesh->GetStaticMaterials().Add(
			FStaticMaterial(
				UMaterial::GetDefaultMaterial(MD_Surface)));
		StaticMesh->GetStaticMaterials().Add(
			FStaticMaterial(
				UMaterial::GetDefaultMaterial(MD_Surface)));
		return StaticMesh;
	}

	bool ReadTopologyStatic(
		UStaticMesh& StaticMesh,
		FTopologyReadback& Out)
	{
		const FMeshDescription* Description =
			StaticMesh.GetMeshDescription(0);
		if (!Description)
		{
			return false;
		}
		FDynamicMesh3 Mesh;
		FMeshDescriptionToDynamicMesh Converter;
		Converter.Convert(Description, Mesh);
		return ReadTopologyMesh(Mesh, Out);
	}

	bool VerifyTopologyResult(
		FAutomationTestBase* Test,
		const FString& DescriptorId,
		const FString& OperationKey,
		const FTopologyReadback& Before,
		const FTopologyReadback& After)
	{
		if (OperationKey == TEXT("bevel_edges"))
		{
			Test->TestTrue(
				*(DescriptorId + TEXT(" bevel adds topology")),
				After.TriangleCount > Before.TriangleCount
					&& After.VertexCount > Before.VertexCount);
		}
		else if (OperationKey == TEXT("boolean"))
		{
			Test->TestTrue(
				*(DescriptorId + TEXT(" union is closed and expanded")),
				After.BoundaryEdgeCount == 0
					&& After.Bounds.Max.X >= Before.Bounds.Max.X + 20.0);
		}
		else if (OperationKey == TEXT("duplicate_faces"))
		{
			Test->TestTrue(
				*(DescriptorId + TEXT(" duplicates translated faces")),
				After.TriangleCount == Before.TriangleCount + 2
					&& After.Bounds.Max.Z >= Before.Bounds.Max.Z + 120.0);
		}
		else if (
			OperationKey == TEXT("extrude_faces")
			|| OperationKey == TEXT("push_pull_faces"))
		{
			const double MinimumDelta =
				OperationKey == TEXT("extrude_faces") ? 20.0 : 25.0;
			Test->TestTrue(
				*(DescriptorId + TEXT(" creates side topology")),
				After.TriangleCount >= Before.TriangleCount + 8
					&& After.Bounds.Max.Z
						>= Before.Bounds.Max.Z + MinimumDelta);
		}
		else if (
			OperationKey == TEXT("fill_holes")
			|| OperationKey == TEXT("weld_edges"))
		{
			Test->TestTrue(
				*(DescriptorId + TEXT(" reduces boundary edges")),
				After.BoundaryEdgeCount < Before.BoundaryEdgeCount);
		}
		else if (
			OperationKey == TEXT("inset_faces")
			|| OperationKey == TEXT("bevel_edges"))
		{
			Test->TestTrue(
				*(DescriptorId + TEXT(" adds vertices and triangles")),
				After.TriangleCount > Before.TriangleCount
					&& After.VertexCount > Before.VertexCount);
		}
		else if (
			OperationKey == TEXT("mesh_cut")
			|| OperationKey == TEXT("plane_cut"))
		{
			Test->TestTrue(
				*(DescriptorId + TEXT(" returns a filled cut")),
				After.BoundaryEdgeCount == 0
					&& After.TriangleCount > Before.TriangleCount);
		}
		else if (OperationKey == TEXT("mirror"))
		{
			Test->TestTrue(
				*(DescriptorId + TEXT(" mirrors the closed mesh")),
				After.TriangleCount >= Before.TriangleCount * 2
					&& After.BoundaryEdgeCount == 0);
		}
		else if (OperationKey == TEXT("offset_faces"))
		{
			Test->TestTrue(
				*(DescriptorId + TEXT(" honors face-normal offset")),
				After.Bounds.Max.Z >= Before.Bounds.Max.Z + 15.0);
		}
		else if (OperationKey == TEXT("remesh"))
		{
			Test->TestTrue(
				*(DescriptorId + TEXT(" changes mesh density")),
				After.TriangleCount >= Before.TriangleCount * 3);
		}
		else if (OperationKey == TEXT("simplify"))
		{
			Test->TestTrue(
				*(DescriptorId + TEXT(" honors triangle ratio")),
				After.TriangleCount
					<= FMath::CeilToInt(
						Before.TriangleCount * 0.25)
						+ 1);
		}
		else if (OperationKey == TEXT("subdivide"))
		{
			Test->TestTrue(
				*(DescriptorId + TEXT(" applies two levels")),
				After.TriangleCount >= Before.TriangleCount * 4);
		}
		else if (OperationKey == TEXT("trim"))
		{
			Test->TestTrue(
				*(DescriptorId + TEXT(" returns closed target")),
				After.BoundaryEdgeCount == 0
					&& After.TriangleCount > 0);
		}
		else if (OperationKey == TEXT("voxel_boolean"))
		{
			Test->TestTrue(
				*(DescriptorId + TEXT(" returns closed voxel union")),
				After.BoundaryEdgeCount == 0
					&& After.TriangleCount > Before.TriangleCount);
		}
		return !Test->HasAnyErrors();
	}

	bool TryCreateModelingApplication(
		const TSharedPtr<const FUeAgentDescriptorRegistry>& Registry,
		const TSharedRef<
			IUeAgentWorldSimpleEffectWorldCatalog,
			ESPMode::ThreadSafe>& WorldCatalog,
		const TSharedRef<
			IUeAgentWorldSimpleEffectWorldCatalog,
			ESPMode::ThreadSafe>& PersistedWorldCatalog,
		const TSharedRef<
			IUeAgentModelingGeometryEffectReferenceResolver,
			ESPMode::ThreadSafe>&
			ReferenceResolver,
		const TSharedRef<
			IUeAgentModelingGeometryEffectReferenceResolver,
			ESPMode::ThreadSafe>&
			PersistedReferenceResolver,
		const TSharedRef<
			IUeAgentSimpleAuthoringAssetDurableService,
			ESPMode::ThreadSafe>&
			Durable,
		const FUeAgentSimpleAuthoringCommandRoute& Route,
		const FString& WorldIdentity,
		const FString& LevelIdentity,
		const FString& ContextRevision,
		const FString& TestScope,
		TSharedPtr<FUeAgentSimpleAuthoringApplication>& OutApplication,
		FString& OutError)
	{
		FUeAgentModelingSimpleAuthoringProviderConfig ProviderConfig;
		ProviderConfig.DescriptorRegistry = Registry;
		ProviderConfig.WorldCatalog = WorldCatalog;
		ProviderConfig.PersistedWorldCatalog =
			PersistedWorldCatalog;
		ProviderConfig.ReferenceResolver = ReferenceResolver;
		ProviderConfig.PersistedReferenceResolver =
			PersistedReferenceResolver;
		ProviderConfig.DurableService = Durable;
		ProviderConfig.AbsenceOracle =
			MakeShared<FUnusedAbsenceOracle>();
		ProviderConfig.ReferenceOracle =
			MakeShared<FUnusedReferenceOracle>();
		ProviderConfig.OwnerLeaseId = TEXT("owner:") + TestScope;
		ProviderConfig.Projection.Emplace(
			FUeAgentProjectionProfile(
				TEXT("compact"),
				false,
				false,
				false),
			TArray<FUeAgentArtifactDescriptor>(),
			TArray<FUeAgentProjectionDiagnostic>());

		TSharedPtr<IUeAgentSimpleAuthoringCapabilityProvider> Provider;
		FUeAgentModelingSimpleAuthoringError ProviderError;
		if (!FUeAgentModelingSimpleAuthoringProviderFactory::TryCreate(
				ProviderConfig,
				Provider,
				ProviderError)
			|| !Provider.IsValid())
		{
			OutError = TEXT("provider:") + ProviderError.Code;
			return false;
		}

		TSharedPtr<const FUeAgentOperationTicketService> TicketService;
		FUeAgentOperationTicketError TicketError;
		const FString TicketId =
			TEXT("ticket:") + TestScope + TEXT(":") + ContextRevision;
		if (!FUeAgentOperationTicketService::TryCreate(
				TestScope + TEXT(":integrity"),
				60.0,
				[]()
				{
					return 100.0;
				},
				[TicketId]()
				{
					return TicketId;
				},
				TicketService,
				TicketError)
			|| !TicketService.IsValid())
		{
			OutError = TEXT("ticket:") + TicketError.Code;
			return false;
		}

		TSharedPtr<const FUeAgentResponseProjector> Projector;
		FUeAgentProjectionError ProjectionError;
		if (!FUeAgentResponseProjector::TryCreate(
				MakeShared<FTokenEstimator>(),
				Projector,
				ProjectionError)
			|| !Projector.IsValid())
		{
			OutError = TEXT("projector:") + ProjectionError.Code;
			return false;
		}

		FUeAgentSimpleAuthoringApplicationConfig ApplicationConfig;
		ApplicationConfig.DescriptorRegistry = Registry;
		ApplicationConfig.ExecutionContext.ProjectInstanceId =
			TEXT("project:") + TestScope;
		ApplicationConfig.ExecutionContext.EditorInstanceId =
			TEXT("editor:") + TestScope;
		ApplicationConfig.ExecutionContext.StatePlane =
			EUeAgentStatePlane::AuthoringMemory;
		ApplicationConfig.ExecutionContext.WorldType =
			EUeAgentExecutionWorldType::Preview;
		ApplicationConfig.ExecutionContext.WorldIdentity =
			WorldIdentity;
		ApplicationConfig.ExecutionContext.LevelIdentity =
			LevelIdentity;
		ApplicationConfig.ExecutionContext.ContextRevision =
			ContextRevision;
		ApplicationConfig.ExecutionContext.bResolvedFromCurrent =
			false;
		ApplicationConfig.ServerSessionId =
			TEXT("session:") + TestScope;
		ApplicationConfig.RegistryEpoch = 1;


		FUeAgentSimpleAuthoringApplicationError ApplicationError;
		if (!FUeAgentSimpleAuthoringApplication::TryCreate(
				ApplicationConfig,
				{Route},
				{Provider.ToSharedRef()},
				TicketService.ToSharedRef(),
				Projector.ToSharedRef(),
				OutApplication,
				ApplicationError)
			|| !OutApplication.IsValid())
		{
			OutError = TEXT("application:") + ApplicationError.Code;
			return false;
		}
		return true;
	}

	bool TryCreateTopologyApplication(
		FOwnedModelingWorld& Owner,
		const TSharedPtr<const FUeAgentDescriptorRegistry>& Registry,
		const TSharedRef<
			IUeAgentModelingGeometryEffectReferenceResolver,
			ESPMode::ThreadSafe>&
			ReferenceResolver,
		const TSharedRef<
			IUeAgentModelingGeometryEffectReferenceResolver,
			ESPMode::ThreadSafe>&
			PersistedReferenceResolver,
		const TSharedRef<
			IUeAgentSimpleAuthoringAssetDurableService,
			ESPMode::ThreadSafe>&
			Durable,
		const FUeAgentSimpleAuthoringCommandRoute& Route,
		TSharedPtr<FUeAgentSimpleAuthoringApplication>& OutApplication,
		FString& OutError)
	{
		const TSharedRef<FOwnedWorldCatalog, ESPMode::ThreadSafe>
			Catalog = MakeShared<
				FOwnedWorldCatalog,
				ESPMode::ThreadSafe>(Owner);
		const TSharedRef<
			IUeAgentWorldSimpleEffectWorldCatalog,
			ESPMode::ThreadSafe> WorldCatalog = Catalog;
		return TryCreateModelingApplication(
			Registry,
			WorldCatalog,
			WorldCatalog,
			ReferenceResolver,
			PersistedReferenceResolver,
			Durable,
			Route,
			Owner.World->GetPathName(),
			Owner.World->PersistentLevel->GetPathName(),
			Owner.ContextRevision,
			TEXT("sa008-modeling-topology"),
			OutApplication,
			OutError);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringModelingTopologyProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"ModelingTopologyProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringModelingTopologyProductionContractTest::
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
			TEXT("Simple Authoring descriptors compose for topology"),
			TryCreateSimpleAuthoringDescriptorRegistry(
				Registry,
				RegistryError))
		|| !Registry.IsValid())
	{
		AddError(
			TEXT("sa008_modeling_topology_registry_invalid:")
			+ RegistryError);
		return false;
	}

	int32 Executed = 0;
	TSet<FString> SubmittedDescriptorIds;
	for (const TSharedPtr<FJsonValue>& Value
		: Fixture->GetArrayField(TEXT("descriptors")))
	{
		const TSharedPtr<FJsonObject> Row = Value->AsObject();
		if (!Row.IsValid()
			|| Row->GetStringField(TEXT("native_operation_type"))
				!= TEXT("geometry.topology.edit"))
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
			TEXT("SA008_Modeling_Topology_Target"),
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
		PrepareTopologyMesh(
			Scenario->GetStringField(TEXT("fixture_kind")),
			*TargetMesh);
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

		bool bUsesOperand = false;
		Row->TryGetBoolField(TEXT("operand"), bUsesOperand);
		ADynamicMeshActor* Operand = bUsesOperand
			? Owner.SpawnCubeActor(
				TEXT("SA008_Modeling_Topology_Operand"),
				FVector(1025.0, 2000.0, 300.0))
			: nullptr;
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
		if (Operand)
		{
			Resolver->Objects.Add(
				TEXT("uai://mesh/fixture-operand"),
				Operand);
		}

		UObject* TargetCarrier = bStaticCarrier
			? static_cast<UObject*>(StaticTarget)
			: static_cast<UObject*>(Target);
		const TSharedPtr<FJsonObject> PayloadObject =
			TargetCarrier
				? BuildTopologyPayload(
					Row,
					Operand)
				: nullptr;
		if (!TestNotNull(
				*(DescriptorId + TEXT(" carrier revision reads")),
				PayloadObject.Get()))
		{
			continue;
		}
		TOptional<FUeAgentStateRevision> CarrierRevisionBefore;
		FExecutionMutationError CarrierRevisionError;
		if (!TestTrue(
				*(DescriptorId
					+ TEXT(" reads exact pre-write carrier revision")),
				TryReadModelingGeometryCarrierRevision(
					TEXT("uai://mesh/fixture-target"),
					*TargetCarrier,
					CarrierRevisionBefore,
					CarrierRevisionError)))
		{
			continue;
		}
		PatchTopologySelection(Row, PayloadObject, *TargetMesh);
		FString PayloadJson;
		TestTrue(
			*(DescriptorId + TEXT(" payload serializes")),
			Serialize(PayloadObject, PayloadJson));

		FTopologyReadback Before;
		if (!TestTrue(
				*(DescriptorId + TEXT(" reads prestate independently")),
				bStaticCarrier
					? ReadTopologyStatic(*StaticTarget, Before)
					: ReadTopologyDynamic(*Target, Before)))
		{
			continue;
		}
		FTopologyReadback OperandBefore;
		if (Operand)
		{
			TestTrue(
				*(DescriptorId + TEXT(" reads operand prestate")),
				ReadTopologyDynamic(*Operand, OperandBefore));
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
				*(DescriptorId
					+ TEXT(" composes production application")),
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
				+ TEXT(":topology_production_composition_failed:")
				+ ApplicationError);
			continue;
		}

		const FUeAgentCallerContext Caller(
			TEXT("caller:sa008-modeling-topology"),
			{TEXT("uai:modeling-geometry:write")},
			TEXT("correlation:sa008-modeling-topology"));
		SubmittedDescriptorIds.Add(DescriptorId);
		const FUeAgentOperationResult Result =
			Application->Submit(
				FUeAgentOperationRequest(
					Route->DescriptorIdentity,
					TEXT("request:sa008-modeling-topology:")
						+ FString::FromInt(Executed),
					TEXT("idempotency:sa008-modeling-topology:")
						+ FString::FromInt(Executed),
					FUeAgentContractValue(PayloadJson)),
				Caller,
				MakeShared<FNeverCancelled>());
		if (!IsComplete(Result))
		{
			AddError(
				DescriptorId
					+ TEXT(
						" did not complete verified lifecycle; "
						"result=")
					+ Result.GetValue().GetJsonValue());
			continue;
		}

		FTopologyReadback After;
		TestTrue(
			*(DescriptorId + TEXT(" reads committed carrier")),
			bStaticCarrier
				? ReadTopologyStatic(*StaticTarget, After)
				: ReadTopologyDynamic(*Target, After));
		TestFalse(
			*(DescriptorId + TEXT(" changes exact carrier state")),
			After == Before);
		TOptional<FUeAgentStateRevision> CarrierRevisionAfter;
		CarrierRevisionError = FExecutionMutationError();
		TestTrue(
			*(DescriptorId
				+ TEXT(" rereads exact post-write carrier revision")),
			TryReadModelingGeometryCarrierRevision(
				TEXT("uai://mesh/fixture-target"),
				*TargetCarrier,
				CarrierRevisionAfter,
				CarrierRevisionError));
		TestTrue(
			*(DescriptorId
				+ TEXT(" advances the carrier revision")),
			CarrierRevisionBefore.IsSet()
				&& CarrierRevisionAfter.IsSet()
				&& CarrierRevisionBefore.GetValue()
					!= CarrierRevisionAfter.GetValue());
		VerifyTopologyResult(
			this,
			DescriptorId,
			OperationKey,
			Before,
			After);
		if (Operand)
		{
			FTopologyReadback OperandAfter;
			TestTrue(
				*(DescriptorId + TEXT(" rereads operand")),
				ReadTopologyDynamic(*Operand, OperandAfter));
			TestTrue(
				*(DescriptorId + TEXT(" preserves operand")),
				OperandAfter == OperandBefore);
		}
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
		TEXT("every selected topology profile reaches submission exactly once"),
		Executed,
		SubmittedDescriptorIds.Num());
	return !HasAnyErrors();
}
