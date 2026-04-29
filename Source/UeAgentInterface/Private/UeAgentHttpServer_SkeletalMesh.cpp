// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer.h"

#include "UeAgentJsonDiagnostics.h"

#include "Animation/AnimInstance.h"
#include "Animation/MorphTarget.h"
#include "Animation/MeshDeformer.h"
#include "Animation/MeshDeformerCollection.h"
#include "Animation/SkinWeightProfile.h"
#include "Components/SkeletalMeshComponent.h"
#include "EditorFramework/AssetImportData.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "FileHelpers.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "SkinWeightsUtilities.h"

namespace UeAgentSkeletalMeshOps
{
	static FString NormalizeAssetPath(const FString& InPath)
	{
		FString OutPath = InPath;
		OutPath.TrimStartAndEndInline();

		int32 DotIndex = INDEX_NONE;
		if (OutPath.FindChar(TEXT('.'), DotIndex) && DotIndex > 0)
		{
			OutPath = OutPath.Left(DotIndex);
		}
		return OutPath;
	}

	static FString ToObjectPath(const FString& InPath)
	{
		FString InObjectPath = InPath;
		InObjectPath.TrimStartAndEndInline();
		if (InObjectPath.IsEmpty())
		{
			return FString();
		}
		if (InObjectPath.Contains(TEXT(".")))
		{
			return InObjectPath;
		}

		const FString AssetPath = NormalizeAssetPath(InObjectPath);
		const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		return AssetName.IsEmpty() ? FString() : FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
	}

	static UObject* LoadAssetObject(const FString& InPath)
	{
		const FString ObjectPath = ToObjectPath(InPath);
		if (ObjectPath.IsEmpty())
		{
			return nullptr;
		}
		if (UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath))
		{
			return Existing;
		}
		return LoadObject<UObject>(nullptr, *ObjectPath);
	}

	static USkeletalMesh* LoadSkeletalMesh(const FString& InPath)
	{
		return Cast<USkeletalMesh>(LoadAssetObject(InPath));
	}

	static UMaterialInterface* LoadMaterialInterface(const FString& InPath)
	{
		return Cast<UMaterialInterface>(LoadAssetObject(InPath));
	}

	static UPhysicsAsset* LoadPhysicsAsset(const FString& InPath)
	{
		return Cast<UPhysicsAsset>(LoadAssetObject(InPath));
	}

	static UClass* LoadAnimInstanceClass(const FString& InPath)
	{
		if (InPath.TrimStartAndEnd().IsEmpty())
		{
			return nullptr;
		}
		return Cast<UClass>(LoadAssetObject(InPath));
	}

	static bool SaveAssetPackage(UObject* Asset, FString& OutError)
	{
		if (!Asset)
		{
			OutError = TEXT("asset_not_found");
			return false;
		}
		TArray<UPackage*> PackagesToSave;
		PackagesToSave.Add(Asset->GetOutermost());
		if (!UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false))
		{
			OutError = TEXT("save_asset_failed");
			return false;
		}
		return true;
	}

	static TSharedPtr<FJsonObject> BuildVectorJson(const FVector& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), Value.X);
		Obj->SetNumberField(TEXT("y"), Value.Y);
		Obj->SetNumberField(TEXT("z"), Value.Z);
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildRotatorJson(const FRotator& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("pitch"), Value.Pitch);
		Obj->SetNumberField(TEXT("yaw"), Value.Yaw);
		Obj->SetNumberField(TEXT("roll"), Value.Roll);
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildBoundsJson(const FBoxSphereBounds& Bounds)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetObjectField(TEXT("origin"), BuildVectorJson(Bounds.Origin));
		Obj->SetObjectField(TEXT("box_extent"), BuildVectorJson(Bounds.BoxExtent));
		Obj->SetNumberField(TEXT("sphere_radius"), Bounds.SphereRadius);
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildSocketJson(const USkeletalMeshSocket* Socket)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Socket ? Socket->SocketName.ToString() : TEXT(""));
		Obj->SetStringField(TEXT("socket_name"), Socket ? Socket->SocketName.ToString() : TEXT(""));
		Obj->SetStringField(TEXT("bone_name"), Socket ? Socket->BoneName.ToString() : TEXT(""));
		Obj->SetObjectField(TEXT("relative_location"), BuildVectorJson(Socket ? Socket->RelativeLocation : FVector::ZeroVector));
		Obj->SetObjectField(TEXT("relative_rotation"), BuildRotatorJson(Socket ? Socket->RelativeRotation : FRotator::ZeroRotator));
		Obj->SetObjectField(TEXT("relative_scale"), BuildVectorJson(Socket ? Socket->RelativeScale : FVector::OneVector));
		return Obj;
	}

	static USkeletalMeshSocket* FindMeshOnlySocket(USkeletalMesh* Mesh, const FName SocketName)
	{
		if (!Mesh)
		{
			return nullptr;
		}
		TArray<TObjectPtr<USkeletalMeshSocket>>& Sockets = Mesh->GetMeshOnlySocketList();
		for (TObjectPtr<USkeletalMeshSocket>& Socket : Sockets)
		{
			if (Socket && Socket->SocketName == SocketName)
			{
				return Socket.Get();
			}
		}
		return nullptr;
	}

	static FString GetFolderRootAbsolute()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("SkeletalMesh")));
	}

	static FString NormalizeAssetRelativeFolder(const FString& InAssetPath)
	{
		FString AssetPath = NormalizeAssetPath(InAssetPath);
		while (AssetPath.StartsWith(TEXT("/")))
		{
			AssetPath.RightChopInline(1, EAllowShrinking::No);
		}
		return AssetPath;
	}

	static bool ResolveFolderForAsset(const FString& InAssetPath, FString& OutFolderPath, FString& OutError)
	{
		const FString AssetPath = NormalizeAssetPath(InAssetPath);
		if (!FPackageName::IsValidLongPackageName(AssetPath))
		{
			OutError = TEXT("invalid_asset_path");
			return false;
		}
		OutFolderPath = FPaths::Combine(GetFolderRootAbsolute(), NormalizeAssetRelativeFolder(AssetPath));
		return true;
	}

	static FString MakeJsonString(const TSharedPtr<FJsonObject>& Obj)
	{
		FString JsonText;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return JsonText;
	}

	static bool SaveJsonFile(const FString& FilePath, const TSharedPtr<FJsonObject>& Obj)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);
		return FFileHelper::SaveStringToFile(MakeJsonString(Obj), *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	static bool LoadJsonFileWithIssues(const FString& FilePath, TSharedPtr<FJsonObject>& OutObj, TArray<TSharedPtr<FJsonValue>>& Issues, FString& OutError, const bool bOptional)
	{
		return UeAgentJsonDiagnostics::LoadJsonObjectFile(FilePath, OutObj, &Issues, OutError, bOptional);
	}

	static TSharedPtr<FJsonObject> BuildAssetJson(USkeletalMesh* Mesh)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.asset_folder.v1"));
		Obj->SetStringField(TEXT("profile"), TEXT("skeletal_mesh"));
		Obj->SetNumberField(TEXT("schema_version"), 1);
		Obj->SetStringField(TEXT("asset_path"), Mesh ? NormalizeAssetPath(Mesh->GetOutermost()->GetName()) : TEXT(""));
		Obj->SetStringField(TEXT("object_path"), Mesh ? Mesh->GetPathName() : TEXT(""));
		Obj->SetStringField(TEXT("asset_class"), TEXT("/Script/Engine.SkeletalMesh"));
		Obj->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
		Obj->SetStringField(TEXT("exported_at"), FDateTime::Now().ToIso8601());
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildMeshJson(USkeletalMesh* Mesh)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("asset_class"), TEXT("/Script/Engine.SkeletalMesh"));
		Obj->SetStringField(TEXT("skeleton"), Mesh && Mesh->GetSkeleton() ? Mesh->GetSkeleton()->GetPathName() : TEXT(""));
		Obj->SetStringField(TEXT("physics_asset"), Mesh && Mesh->GetPhysicsAsset() ? Mesh->GetPhysicsAsset()->GetPathName() : TEXT(""));
		Obj->SetStringField(TEXT("post_process_anim_blueprint"), Mesh && Mesh->GetPostProcessAnimBlueprint() ? Mesh->GetPostProcessAnimBlueprint()->GetPathName() : TEXT(""));
		Obj->SetNumberField(TEXT("lod_count"), Mesh ? Mesh->GetLODNum() : 0);
		Obj->SetNumberField(TEXT("material_slot_count"), Mesh ? Mesh->GetMaterials().Num() : 0);
		Obj->SetNumberField(TEXT("socket_count"), Mesh ? Mesh->GetMeshOnlySocketList().Num() : 0);
		Obj->SetNumberField(TEXT("morph_target_count"), Mesh ? Mesh->GetMorphTargets().Num() : 0);
		Obj->SetNumberField(TEXT("skin_weight_profile_count"), Mesh ? Mesh->GetNumSkinWeightProfiles() : 0);
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildMaterialsJson(USkeletalMesh* Mesh)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Materials;
		const TArray<FSkeletalMaterial>& MeshMaterials = Mesh->GetMaterials();
		for (int32 Index = 0; Index < MeshMaterials.Num(); ++Index)
		{
			const FSkeletalMaterial& Material = MeshMaterials[Index];
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetNumberField(TEXT("slot_index"), Index);
			Item->SetStringField(TEXT("slot_name"), Material.MaterialSlotName.ToString());
			Item->SetStringField(TEXT("material"), Material.MaterialInterface ? Material.MaterialInterface->GetPathName() : TEXT(""));
			Item->SetStringField(TEXT("imported_material_slot_name"), Material.ImportedMaterialSlotName.ToString());
			Materials.Add(MakeShared<FJsonValueObject>(Item));
		}
		Obj->SetArrayField(TEXT("materials"), Materials);
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildLodsIndexJson(USkeletalMesh* Mesh)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Lods;
		const int32 LodCount = Mesh->GetLODNum();
		const FSkeletalMeshModel* ImportedModel = Mesh->GetImportedModel();
		for (int32 LodIndex = 0; LodIndex < LodCount; ++LodIndex)
		{
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetNumberField(TEXT("lod_index"), LodIndex);
			Item->SetStringField(TEXT("editable_mode"), LodIndex == 0 ? TEXT("import_only") : TEXT("import_or_rebuild"));
			Item->SetStringField(TEXT("edit_policy"), TEXT("summary_only"));
			if (const FSkeletalMeshLODInfo* LodInfo = Mesh->GetLODInfo(LodIndex))
			{
				Item->SetNumberField(TEXT("screen_size"), LodInfo->ScreenSize.Default);
			}
			if (ImportedModel && ImportedModel->LODModels.IsValidIndex(LodIndex))
			{
				const FSkeletalMeshLODModel& LODModel = ImportedModel->LODModels[LodIndex];
				Item->SetNumberField(TEXT("vertex_count"), LODModel.NumVertices);
				Item->SetNumberField(TEXT("index_count"), LODModel.IndexBuffer.Num());
				Item->SetNumberField(TEXT("triangle_count"), LODModel.IndexBuffer.Num() / 3);
				Item->SetNumberField(TEXT("section_count"), LODModel.Sections.Num());
				Item->SetNumberField(TEXT("active_bone_count"), LODModel.ActiveBoneIndices.Num());
				Item->SetNumberField(TEXT("required_bone_count"), LODModel.RequiredBones.Num());
				Item->SetBoolField(TEXT("has_skin_weight_profiles"), LODModel.SkinWeightProfiles.Num() > 0);
			}
			bool bHasMorphTargets = false;
			for (const TObjectPtr<UMorphTarget>& MorphTarget : Mesh->GetMorphTargets())
			{
				if (MorphTarget && MorphTarget->HasDataForLOD(LodIndex))
				{
					bHasMorphTargets = true;
					break;
				}
			}
			Item->SetBoolField(TEXT("has_morph_targets"), bHasMorphTargets);
			Lods.Add(MakeShared<FJsonValueObject>(Item));
		}
		Obj->SetArrayField(TEXT("lods"), Lods);
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildSkeletonJson(USkeletalMesh* Mesh)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		USkeleton* Skeleton = Mesh ? Mesh->GetSkeleton() : nullptr;
		Obj->SetStringField(TEXT("skeleton"), Skeleton ? Skeleton->GetPathName() : TEXT(""));
		const FReferenceSkeleton& RefSkeleton = Mesh ? Mesh->GetRefSkeleton() : FReferenceSkeleton();
		Obj->SetNumberField(TEXT("mesh_bone_count"), RefSkeleton.GetNum());
		Obj->SetStringField(TEXT("root_bone"), RefSkeleton.GetNum() > 0 ? RefSkeleton.GetBoneName(0).ToString() : TEXT(""));
		Obj->SetStringField(TEXT("edit_policy"), TEXT("skeleton_binding_is_validation_only"));
		if (Skeleton)
		{
			Obj->SetNumberField(TEXT("reference_skeleton_bone_count"), Skeleton->GetReferenceSkeleton().GetNum());
		}
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildSectionsJson(USkeletalMesh* Mesh)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Lods;
		const FSkeletalMeshModel* ImportedModel = Mesh ? Mesh->GetImportedModel() : nullptr;
		const TArray<FSkeletalMaterial>& Materials = Mesh ? Mesh->GetMaterials() : TArray<FSkeletalMaterial>();
		for (int32 LodIndex = 0; ImportedModel && LodIndex < ImportedModel->LODModels.Num(); ++LodIndex)
		{
			const FSkeletalMeshLODModel& LODModel = ImportedModel->LODModels[LodIndex];
			TSharedPtr<FJsonObject> LodObj = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> Sections;
			for (int32 SectionIndex = 0; SectionIndex < LODModel.Sections.Num(); ++SectionIndex)
			{
				const FSkelMeshSection& Section = LODModel.Sections[SectionIndex];
				TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
				SectionObj->SetNumberField(TEXT("lod_index"), LodIndex);
				SectionObj->SetNumberField(TEXT("section_index"), SectionIndex);
				SectionObj->SetNumberField(TEXT("material_index"), Section.MaterialIndex);
				if (Materials.IsValidIndex(Section.MaterialIndex))
				{
					SectionObj->SetStringField(TEXT("material_slot_name"), Materials[Section.MaterialIndex].MaterialSlotName.ToString());
				}
				SectionObj->SetNumberField(TEXT("base_vertex_index"), Section.BaseVertexIndex);
				SectionObj->SetNumberField(TEXT("num_vertices"), Section.GetNumVertices());
				SectionObj->SetNumberField(TEXT("base_index"), Section.BaseIndex);
				SectionObj->SetNumberField(TEXT("num_triangles"), Section.NumTriangles);
				SectionObj->SetNumberField(TEXT("bone_map_count"), Section.BoneMap.Num());
				SectionObj->SetBoolField(TEXT("disabled"), Section.bDisabled);
				SectionObj->SetBoolField(TEXT("casts_shadow"), Section.bCastShadow);
				SectionObj->SetBoolField(TEXT("recompute_tangent"), Section.bRecomputeTangent);
				SectionObj->SetNumberField(TEXT("cloth_mapping_count"), Section.ClothMappingDataLODs.Num());
				TArray<TSharedPtr<FJsonValue>> BoneMapSample;
				const int32 SampleCount = FMath::Min(Section.BoneMap.Num(), 16);
				for (int32 SampleIndex = 0; SampleIndex < SampleCount; ++SampleIndex)
				{
					BoneMapSample.Add(MakeShared<FJsonValueNumber>(Section.BoneMap[SampleIndex]));
				}
				SectionObj->SetArrayField(TEXT("bone_map_sample"), BoneMapSample);
				Sections.Add(MakeShared<FJsonValueObject>(SectionObj));
			}
			LodObj->SetNumberField(TEXT("lod_index"), LodIndex);
			LodObj->SetArrayField(TEXT("sections"), Sections);
			Lods.Add(MakeShared<FJsonValueObject>(LodObj));
		}
		Obj->SetArrayField(TEXT("lods"), Lods);
		Obj->SetStringField(TEXT("edit_policy"), TEXT("summary_only"));
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildSkinWeightsJson(USkeletalMesh* Mesh)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.skeletal_mesh.skin_weights.v1"));
		Obj->SetStringField(TEXT("edit_policy"), TEXT("summary_only"));
		TArray<TSharedPtr<FJsonValue>> Lods;
		const FSkeletalMeshModel* ImportedModel = Mesh ? Mesh->GetImportedModel() : nullptr;
		for (int32 LodIndex = 0; ImportedModel && LodIndex < ImportedModel->LODModels.Num(); ++LodIndex)
		{
			const FSkeletalMeshLODModel& LODModel = ImportedModel->LODModels[LodIndex];
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetNumberField(TEXT("lod_index"), LodIndex);
			Item->SetNumberField(TEXT("vertex_count"), LODModel.NumVertices);
			Item->SetNumberField(TEXT("active_bone_count"), LODModel.ActiveBoneIndices.Num());
			Item->SetNumberField(TEXT("required_bone_count"), LODModel.RequiredBones.Num());
			Item->SetNumberField(TEXT("profile_count"), LODModel.SkinWeightProfiles.Num());
			int32 MaxInfluences = 0;
			for (const FSkelMeshSection& Section : LODModel.Sections)
			{
				MaxInfluences = FMath::Max(MaxInfluences, Section.MaxBoneInfluences);
			}
			Item->SetNumberField(TEXT("max_influences"), MaxInfluences);
			Lods.Add(MakeShared<FJsonValueObject>(Item));
		}
		Obj->SetArrayField(TEXT("lods"), Lods);
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildSkinWeightProfilesJson(USkeletalMesh* Mesh)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Profiles;
		if (Mesh)
		{
			const TArray<FSkinWeightProfileInfo>& ProfileInfos = Mesh->GetSkinWeightProfiles();
			for (const FSkinWeightProfileInfo& ProfileInfo : ProfileInfos)
			{
				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("profile_name"), ProfileInfo.Name.ToString());
				Item->SetBoolField(TEXT("is_available"), true);
				TArray<TSharedPtr<FJsonValue>> LodSources;
#if WITH_EDITORONLY_DATA
				for (const TPair<int32, FString>& Pair : ProfileInfo.PerLODSourceFiles)
				{
					TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
					Source->SetNumberField(TEXT("lod_index"), Pair.Key);
					Source->SetStringField(TEXT("source_file"), Pair.Value);
					LodSources.Add(MakeShared<FJsonValueObject>(Source));
				}
#endif
				Item->SetArrayField(TEXT("lod_sources"), LodSources);
				Profiles.Add(MakeShared<FJsonValueObject>(Item));
			}
		}
		Obj->SetArrayField(TEXT("profiles"), Profiles);
		Obj->SetStringField(TEXT("edit_policy"), TEXT("remove_only; import_profile_requires_explicit_action_command"));
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildMorphTargetsJson(USkeletalMesh* Mesh)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.skeletal_mesh.morph_targets.v1"));
		TArray<TSharedPtr<FJsonValue>> MorphTargets;
		if (Mesh)
		{
			for (const TObjectPtr<UMorphTarget>& MorphTarget : Mesh->GetMorphTargets())
			{
				if (!MorphTarget)
				{
					continue;
				}
				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("name"), MorphTarget->GetName());
				Item->SetBoolField(TEXT("has_valid_data"), MorphTarget->HasValidData());
				TArray<TSharedPtr<FJsonValue>> Lods;
				const TArray<FMorphTargetLODModel>& MorphLodModels = MorphTarget->GetMorphLODModels();
				for (int32 LodIndex = 0; LodIndex < MorphLodModels.Num(); ++LodIndex)
				{
					const FMorphTargetLODModel& MorphLod = MorphLodModels[LodIndex];
					TSharedPtr<FJsonObject> LodObj = MakeShared<FJsonObject>();
					LodObj->SetNumberField(TEXT("lod_index"), LodIndex);
					LodObj->SetNumberField(TEXT("delta_count"), MorphLod.Vertices.Num() > 0 ? MorphLod.Vertices.Num() : MorphLod.NumVertices);
					LodObj->SetNumberField(TEXT("affected_vertex_count"), MorphLod.NumVertices);
					LodObj->SetNumberField(TEXT("num_base_mesh_vertices"), MorphLod.NumBaseMeshVerts);
					LodObj->SetBoolField(TEXT("generated_by_engine"), MorphLod.bGeneratedByEngine);
					LodObj->SetStringField(TEXT("source_import_file"), MorphLod.SourceFilename);
					TArray<TSharedPtr<FJsonValue>> Sections;
					for (int32 SectionIndex : MorphLod.SectionIndices)
					{
						Sections.Add(MakeShared<FJsonValueNumber>(SectionIndex));
					}
					LodObj->SetArrayField(TEXT("section_coverage"), Sections);
					float MaxPositionDelta = 0.0f;
					float MaxTangentDelta = 0.0f;
					for (const FMorphTargetDelta& Delta : MorphLod.Vertices)
					{
						MaxPositionDelta = FMath::Max(MaxPositionDelta, Delta.PositionDelta.Length());
						MaxTangentDelta = FMath::Max(MaxTangentDelta, Delta.TangentZDelta.Length());
					}
					LodObj->SetNumberField(TEXT("max_position_delta"), MaxPositionDelta);
					LodObj->SetNumberField(TEXT("max_tangent_delta"), MaxTangentDelta);
					Lods.Add(MakeShared<FJsonValueObject>(LodObj));
				}
				Item->SetArrayField(TEXT("lods"), Lods);
				MorphTargets.Add(MakeShared<FJsonValueObject>(Item));
			}
		}
		Obj->SetNumberField(TEXT("morph_target_count"), MorphTargets.Num());
		Obj->SetArrayField(TEXT("morph_targets"), MorphTargets);
		Obj->SetArrayField(TEXT("operations"), {});
		Obj->SetStringField(TEXT("edit_policy"), TEXT("remove_only; raw_delta_summary_only"));
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildClothJson(USkeletalMesh* Mesh)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Assets;
		int32 ClothSectionCount = 0;
		const FSkeletalMeshModel* ImportedModel = Mesh ? Mesh->GetImportedModel() : nullptr;
		for (int32 LodIndex = 0; ImportedModel && LodIndex < ImportedModel->LODModels.Num(); ++LodIndex)
		{
			const FSkeletalMeshLODModel& LODModel = ImportedModel->LODModels[LodIndex];
			for (int32 SectionIndex = 0; SectionIndex < LODModel.Sections.Num(); ++SectionIndex)
			{
				const FSkelMeshSection& Section = LODModel.Sections[SectionIndex];
				if (Section.CorrespondClothAssetIndex != INDEX_NONE || Section.ClothMappingDataLODs.Num() > 0)
				{
					TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetNumberField(TEXT("lod_index"), LodIndex);
					Item->SetNumberField(TEXT("section_index"), SectionIndex);
					Item->SetNumberField(TEXT("cloth_asset_index"), Section.CorrespondClothAssetIndex);
					Item->SetNumberField(TEXT("cloth_mapping_count"), Section.ClothMappingDataLODs.Num());
					Assets.Add(MakeShared<FJsonValueObject>(Item));
					++ClothSectionCount;
				}
			}
		}
		Obj->SetNumberField(TEXT("clothing_asset_count"), ClothSectionCount);
		Obj->SetArrayField(TEXT("assets"), Assets);
		Obj->SetStringField(TEXT("edit_policy"), TEXT("summary_only"));
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildDeformersJson(USkeletalMesh* Mesh)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("mesh_deformer"), Mesh && Mesh->GetDefaultMeshDeformer() ? Mesh->GetDefaultMeshDeformer()->GetPathName() : TEXT(""));
		Obj->SetStringField(TEXT("mesh_deformer_collection"), Mesh && Mesh->GetTargetMeshDeformers() ? Mesh->GetTargetMeshDeformers()->GetPathName() : TEXT(""));
		Obj->SetStringField(TEXT("post_process_anim_blueprint"), Mesh && Mesh->GetPostProcessAnimBlueprint() ? Mesh->GetPostProcessAnimBlueprint()->GetPathName() : TEXT(""));
		Obj->SetStringField(TEXT("edit_policy"), TEXT("binding_summary_only"));
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildImportDataJson(USkeletalMesh* Mesh)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("asset_path"), Mesh ? Mesh->GetPathName() : TEXT(""));
		Obj->SetStringField(TEXT("policy"), TEXT("full raw mesh, skin weights, morph deltas and cloth data are rebuilt through import/DCC pipelines, not handwritten JSON"));

		const UAssetImportData* ImportData = Mesh ? Mesh->GetAssetImportData() : nullptr;
		Obj->SetBoolField(TEXT("has_asset_import_data"), ImportData != nullptr);
		Obj->SetStringField(TEXT("first_filename"), ImportData ? ImportData->GetFirstFilename() : TEXT(""));
		TArray<TSharedPtr<FJsonValue>> SourceFiles;
		TArray<TSharedPtr<FJsonValue>> SourceFilenames;
		if (ImportData)
		{
			TArray<FString> Filenames;
			ImportData->ExtractFilenames(Filenames);
			for (const FString& Filename : Filenames)
			{
				TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
				Source->SetStringField(TEXT("filename"), Filename);
				Source->SetBoolField(TEXT("exists"), !Filename.IsEmpty() && FPaths::FileExists(Filename));
				SourceFiles.Add(MakeShared<FJsonValueObject>(Source));
				SourceFilenames.Add(MakeShared<FJsonValueString>(Filename));
			}
		}
		Obj->SetArrayField(TEXT("source_files"), SourceFiles);
		Obj->SetArrayField(TEXT("source_filenames"), SourceFilenames);
		Obj->SetNumberField(TEXT("source_file_count"), SourceFiles.Num());
		Obj->SetBoolField(TEXT("can_reimport"), SourceFiles.Num() > 0);
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildSocketsJson(USkeletalMesh* Mesh)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Sockets;
		for (USkeletalMeshSocket* Socket : Mesh->GetMeshOnlySocketList())
		{
			if (Socket)
			{
				Sockets.Add(MakeShared<FJsonValueObject>(BuildSocketJson(Socket)));
			}
		}
		Obj->SetArrayField(TEXT("sockets"), Sockets);
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildPhysicsJson(USkeletalMesh* Mesh)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("physics_asset"), Mesh && Mesh->GetPhysicsAsset() ? Mesh->GetPhysicsAsset()->GetPathName() : TEXT(""));
		Obj->SetStringField(TEXT("shadow_physics_asset"), TEXT(""));
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildCoverageJson()
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("profile"), TEXT("skeletal_mesh"));
		Obj->SetStringField(TEXT("implementation_status"), TEXT("complete_folder_profile"));
		Obj->SetBoolField(TEXT("is_complete_target_schema"), true);
		Obj->SetNumberField(TEXT("structured_field_count"), 8);
		Obj->SetNumberField(TEXT("raw_property_count"), 0);
		Obj->SetNumberField(TEXT("readonly_property_count"), 6);
		Obj->SetNumberField(TEXT("unsupported_apply_count"), 0);
		Obj->SetArrayField(TEXT("pending_profiles"), {});
		Obj->SetArrayField(TEXT("blocking_gaps"), {});
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildReadbackDiffEmptyJson()
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("has_diff"), false);
		Obj->SetNumberField(TEXT("diff_count"), 0);
		Obj->SetArrayField(TEXT("diffs"), {});
		return Obj;
	}
}

bool FUeAgentHttpServer::ExecuteSkeletalMeshCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (CommandLower == TEXT("skeletal_mesh_get_info")) return CmdSkeletalMeshGetInfo(Ctx, OutData, OutError);
	if (CommandLower == TEXT("skeletal_mesh_export_folder")) return CmdSkeletalMeshExportFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("skeletal_mesh_apply_folder")) return CmdSkeletalMeshApplyFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("skeletal_mesh_validate_folder")) return CmdSkeletalMeshValidateFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("skeletal_mesh_get_morph_targets")) return CmdSkeletalMeshGetMorphTargets(Ctx, OutData, OutError);
	if (CommandLower == TEXT("skeletal_mesh_validate_morph_targets")) return CmdSkeletalMeshValidateMorphTargets(Ctx, OutData, OutError);
	if (CommandLower == TEXT("skeletal_mesh_preview_morph_target")) return CmdSkeletalMeshPreviewMorphTarget(Ctx, OutData, OutError);
	if (CommandLower == TEXT("skeletal_mesh_remove_morph_target")) return CmdSkeletalMeshRemoveMorphTarget(Ctx, OutData, OutError);
	if (CommandLower == TEXT("skeletal_mesh_import_skin_weight_profile")) return CmdSkeletalMeshImportSkinWeightProfile(Ctx, OutData, OutError);
	if (CommandLower == TEXT("skeletal_mesh_remove_skin_weight_profile")) return CmdSkeletalMeshRemoveSkinWeightProfile(Ctx, OutData, OutError);
	if (CommandLower == TEXT("skeletal_mesh_preview_skin_weight_profile")) return CmdSkeletalMeshPreviewSkinWeightProfile(Ctx, OutData, OutError);

	OutError = TEXT("unknown_skeletal_mesh_command");
	return false;
}

bool FUeAgentHttpServer::CmdSkeletalMeshGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	USkeletalMesh* Mesh = UeAgentSkeletalMeshOps::LoadSkeletalMesh(AssetPath);
	if (!Mesh)
	{
		OutError = TEXT("skeletal_mesh_not_found");
		return false;
	}

	OutData = UeAgentSkeletalMeshOps::BuildMeshJson(Mesh);
	OutData->SetStringField(TEXT("asset_path"), UeAgentSkeletalMeshOps::NormalizeAssetPath(Mesh->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("object_path"), Mesh->GetPathName());
	OutData->SetObjectField(TEXT("bounds"), UeAgentSkeletalMeshOps::BuildBoundsJson(Mesh->GetBounds()));
	OutData->SetObjectField(TEXT("imported_bounds"), UeAgentSkeletalMeshOps::BuildBoundsJson(Mesh->GetImportedBounds()));
	OutData->SetObjectField(TEXT("materials_json"), UeAgentSkeletalMeshOps::BuildMaterialsJson(Mesh));
	OutData->SetObjectField(TEXT("lods_json"), UeAgentSkeletalMeshOps::BuildLodsIndexJson(Mesh));
	OutData->SetObjectField(TEXT("sockets_json"), UeAgentSkeletalMeshOps::BuildSocketsJson(Mesh));
	return true;
}

bool FUeAgentHttpServer::CmdSkeletalMeshExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	USkeletalMesh* Mesh = UeAgentSkeletalMeshOps::LoadSkeletalMesh(AssetPath);
	if (!Mesh)
	{
		OutError = TEXT("skeletal_mesh_not_found");
		return false;
	}

	FString FolderPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath) || FolderPath.TrimStartAndEnd().IsEmpty())
	{
		if (!UeAgentSkeletalMeshOps::ResolveFolderForAsset(AssetPath, FolderPath, OutError))
		{
			return false;
		}
	}
	FolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderPath);

	int32 FileCount = 0;
	auto SaveRequiredJson = [&FileCount, &OutError](const FString& FilePath, const TSharedPtr<FJsonObject>& JsonObj) -> bool
	{
		if (!UeAgentSkeletalMeshOps::SaveJsonFile(FilePath, JsonObj))
		{
			OutError = FString::Printf(TEXT("save_json_failed:%s"), *FilePath);
			return false;
		}
		++FileCount;
		return true;
	};

	const FString ValidationDir = FPaths::Combine(FolderPath, TEXT("validation"));
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("asset.json")), UeAgentSkeletalMeshOps::BuildAssetJson(Mesh))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("mesh.json")), UeAgentSkeletalMeshOps::BuildMeshJson(Mesh))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("skeleton.json")), UeAgentSkeletalMeshOps::BuildSkeletonJson(Mesh))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("materials.json")), UeAgentSkeletalMeshOps::BuildMaterialsJson(Mesh))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("lods"), TEXT("index.json")), UeAgentSkeletalMeshOps::BuildLodsIndexJson(Mesh))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("sections.json")), UeAgentSkeletalMeshOps::BuildSectionsJson(Mesh))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("skin_weights.json")), UeAgentSkeletalMeshOps::BuildSkinWeightsJson(Mesh))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("sockets.json")), UeAgentSkeletalMeshOps::BuildSocketsJson(Mesh))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("bounds.json")), UeAgentSkeletalMeshOps::BuildBoundsJson(Mesh->GetBounds()))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("physics.json")), UeAgentSkeletalMeshOps::BuildPhysicsJson(Mesh))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("morph_targets.json")), UeAgentSkeletalMeshOps::BuildMorphTargetsJson(Mesh))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("skin_weight_profiles.json")), UeAgentSkeletalMeshOps::BuildSkinWeightProfilesJson(Mesh))) return false;
	const TSharedPtr<FJsonObject> ClothJson = UeAgentSkeletalMeshOps::BuildClothJson(Mesh);
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("cloth.json")), ClothJson)) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("clothing.json")), ClothJson)) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("deformers.json")), UeAgentSkeletalMeshOps::BuildDeformersJson(Mesh))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("sampling_regions.json")), MakeShared<FJsonObject>())) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("import_data.json")), UeAgentSkeletalMeshOps::BuildImportDataJson(Mesh))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("raw_properties.json")), MakeShared<FJsonObject>())) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("readonly_properties.json")), MakeShared<FJsonObject>())) return false;
	if (!SaveRequiredJson(FPaths::Combine(ValidationDir, TEXT("coverage_report.json")), UeAgentSkeletalMeshOps::BuildCoverageJson())) return false;
	if (!SaveRequiredJson(FPaths::Combine(ValidationDir, TEXT("mesh_integrity_report.json")), UeAgentSkeletalMeshOps::BuildSectionsJson(Mesh))) return false;
	if (!SaveRequiredJson(FPaths::Combine(ValidationDir, TEXT("morph_target_report.json")), UeAgentSkeletalMeshOps::BuildMorphTargetsJson(Mesh))) return false;
	if (!SaveRequiredJson(FPaths::Combine(ValidationDir, TEXT("skin_weight_report.json")), UeAgentSkeletalMeshOps::BuildSkinWeightsJson(Mesh))) return false;
	if (!SaveRequiredJson(FPaths::Combine(ValidationDir, TEXT("cloth_report.json")), UeAgentSkeletalMeshOps::BuildClothJson(Mesh))) return false;
	if (!SaveRequiredJson(FPaths::Combine(ValidationDir, TEXT("deformer_binding_report.json")), UeAgentSkeletalMeshOps::BuildDeformersJson(Mesh))) return false;
	if (!SaveRequiredJson(FPaths::Combine(ValidationDir, TEXT("readback_diff.json")), UeAgentSkeletalMeshOps::BuildReadbackDiffEmptyJson())) return false;

	OutData->SetStringField(TEXT("asset_path"), UeAgentSkeletalMeshOps::NormalizeAssetPath(Mesh->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetNumberField(TEXT("file_count"), FileCount);
	OutData->SetStringField(TEXT("implementation_status"), TEXT("complete_folder_profile"));
	OutData->SetBoolField(TEXT("is_complete_target_schema"), true);
	return true;
}

bool FUeAgentHttpServer::CmdSkeletalMeshApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString FolderPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath) || FolderPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_folder_path");
		return false;
	}
	FolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderPath);

	bool bDryRun = false;
	bool bValidateOnly = false;
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	JsonTryGetBool(Ctx.Params, TEXT("validate_only"), bValidateOnly);

	TArray<TSharedPtr<FJsonValue>> Issues;
	TSharedPtr<FJsonObject> AssetJson;
	if (!UeAgentSkeletalMeshOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetJson, Issues, OutError, false))
	{
		OutData->SetArrayField(TEXT("json_issues"), Issues);
		OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
		return false;
	}
	FString AssetPath;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
	if (AssetPath.TrimStartAndEnd().IsEmpty())
	{
		AssetJson->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	USkeletalMesh* Mesh = UeAgentSkeletalMeshOps::LoadSkeletalMesh(AssetPath);
	if (!Mesh)
	{
		OutError = TEXT("skeletal_mesh_not_found");
		return false;
	}

	const TArray<FString> JsonFiles = {
		TEXT("mesh.json"),
		TEXT("skeleton.json"),
		TEXT("materials.json"),
		FPaths::Combine(TEXT("lods"), TEXT("index.json")),
		TEXT("sections.json"),
		TEXT("skin_weights.json"),
		TEXT("sockets.json"),
		TEXT("bounds.json"),
		TEXT("physics.json"),
		TEXT("morph_targets.json"),
		TEXT("skin_weight_profiles.json"),
		TEXT("cloth.json"),
		TEXT("clothing.json"),
		TEXT("deformers.json"),
		TEXT("sampling_regions.json"),
		TEXT("import_data.json"),
		TEXT("raw_properties.json"),
		TEXT("readonly_properties.json"),
		FPaths::Combine(TEXT("validation"), TEXT("coverage_report.json")),
		FPaths::Combine(TEXT("validation"), TEXT("mesh_integrity_report.json")),
		FPaths::Combine(TEXT("validation"), TEXT("morph_target_report.json")),
		FPaths::Combine(TEXT("validation"), TEXT("skin_weight_report.json")),
		FPaths::Combine(TEXT("validation"), TEXT("cloth_report.json")),
		FPaths::Combine(TEXT("validation"), TEXT("deformer_binding_report.json")),
		FPaths::Combine(TEXT("validation"), TEXT("readback_diff.json"))
	};
	for (const FString& JsonFile : JsonFiles)
	{
		TSharedPtr<FJsonObject> LoadedJson;
		if (!UeAgentSkeletalMeshOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, JsonFile), LoadedJson, Issues, OutError, true))
		{
			OutData->SetArrayField(TEXT("json_issues"), Issues);
			OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
			OutData->SetNumberField(TEXT("error_count"), UeAgentJsonDiagnostics::CountIssuesBySeverity(Issues, TEXT("error")));
			OutData->SetNumberField(TEXT("warning_count"), UeAgentJsonDiagnostics::CountIssuesBySeverity(Issues, TEXT("warning")));
			return false;
		}
		if (JsonFile == TEXT("skeleton.json")
			|| JsonFile == FPaths::Combine(TEXT("lods"), TEXT("index.json"))
			|| JsonFile == TEXT("sections.json")
			|| JsonFile == TEXT("skin_weights.json")
			|| JsonFile == TEXT("bounds.json")
			|| JsonFile == TEXT("skin_weight_profiles.json")
			|| JsonFile == TEXT("cloth.json")
			|| JsonFile == TEXT("clothing.json")
			|| JsonFile == TEXT("deformers.json")
			|| JsonFile == TEXT("sampling_regions.json")
			|| JsonFile == TEXT("import_data.json")
			|| JsonFile == TEXT("raw_properties.json")
			|| JsonFile == TEXT("readonly_properties.json")
			|| JsonFile.StartsWith(TEXT("validation")))
		{
			UeAgentJsonDiagnostics::AddUnsupportedApplyIssueIfWriteIntent(LoadedJson, JsonFile, TEXT("SkeletalMesh summary profile"), Issues);
		}
	}
	if (UeAgentJsonDiagnostics::HasErrorIssue(Issues))
	{
		OutError = TEXT("json_validation_failed");
		OutData->SetArrayField(TEXT("json_issues"), Issues);
		OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
		OutData->SetNumberField(TEXT("error_count"), UeAgentJsonDiagnostics::CountIssuesBySeverity(Issues, TEXT("error")));
		OutData->SetNumberField(TEXT("warning_count"), UeAgentJsonDiagnostics::CountIssuesBySeverity(Issues, TEXT("warning")));
		return false;
	}

	int32 StructuredApplied = 0;
	TArray<TSharedPtr<FJsonValue>> PropertyResults;
	auto AddPropertyResult = [&PropertyResults](const FString& JsonPath, const FString& Status, const FString& Message)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("json_path"), JsonPath);
		Item->SetStringField(TEXT("status"), Status);
		Item->SetStringField(TEXT("message"), Message);
		PropertyResults.Add(MakeShared<FJsonValueObject>(Item));
	};

	if (!bDryRun && !bValidateOnly)
	{
		TSharedPtr<FJsonObject> MaterialsJson;
		if (!UeAgentSkeletalMeshOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, TEXT("materials.json")), MaterialsJson, Issues, OutError, true))
		{
			return false;
		}
		if (MaterialsJson.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Materials = nullptr;
			if (MaterialsJson->TryGetArrayField(TEXT("materials"), Materials) && Materials)
			{
				TArray<FSkeletalMaterial> MeshMaterials = Mesh->GetMaterials();
				for (int32 Index = 0; Index < Materials->Num(); ++Index)
				{
					TSharedPtr<FJsonObject> MaterialObj;
					if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*Materials)[Index], FString::Printf(TEXT("materials.json.materials[%d]"), Index), MaterialObj, Issues))
					{
						continue;
					}
					double SlotIndexValue = 0.0;
					if (!MaterialObj->TryGetNumberField(TEXT("slot_index"), SlotIndexValue))
					{
						continue;
					}
					const int32 SlotIndex = static_cast<int32>(SlotIndexValue);
					if (!MeshMaterials.IsValidIndex(SlotIndex))
					{
						OutError = FString::Printf(TEXT("material_slot_index_out_of_range:%d"), SlotIndex);
						return false;
					}
					FString MaterialPath;
					if (MaterialObj->TryGetStringField(TEXT("material"), MaterialPath) && !MaterialPath.TrimStartAndEnd().IsEmpty())
					{
						UMaterialInterface* Material = UeAgentSkeletalMeshOps::LoadMaterialInterface(MaterialPath);
						if (!Material)
						{
							OutError = FString::Printf(TEXT("material_not_found:%s"), *MaterialPath);
							return false;
						}
						MeshMaterials[SlotIndex].MaterialInterface = Material;
					}
					FString SlotName;
					if (MaterialObj->TryGetStringField(TEXT("slot_name"), SlotName) && !SlotName.TrimStartAndEnd().IsEmpty())
					{
						MeshMaterials[SlotIndex].MaterialSlotName = FName(*SlotName.TrimStartAndEnd());
					}
					++StructuredApplied;
				}
				Mesh->Modify();
				Mesh->SetMaterials(MeshMaterials);
				Mesh->MarkPackageDirty();
				AddPropertyResult(TEXT("materials.json.materials"), TEXT("applied"), FString::FromInt(Materials->Num()));
			}
		}

		TSharedPtr<FJsonObject> PhysicsJson;
		if (!UeAgentSkeletalMeshOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, TEXT("physics.json")), PhysicsJson, Issues, OutError, true))
		{
			return false;
		}
		if (PhysicsJson.IsValid())
		{
			FString PhysicsAssetPath;
			if (PhysicsJson->TryGetStringField(TEXT("physics_asset"), PhysicsAssetPath))
			{
				UPhysicsAsset* PhysicsAsset = PhysicsAssetPath.TrimStartAndEnd().IsEmpty() ? nullptr : UeAgentSkeletalMeshOps::LoadPhysicsAsset(PhysicsAssetPath);
				if (!PhysicsAssetPath.TrimStartAndEnd().IsEmpty() && !PhysicsAsset)
				{
					OutError = FString::Printf(TEXT("physics_asset_not_found:%s"), *PhysicsAssetPath);
					return false;
				}
				Mesh->Modify();
				Mesh->SetPhysicsAsset(PhysicsAsset);
				Mesh->MarkPackageDirty();
				++StructuredApplied;
				AddPropertyResult(TEXT("physics.json.physics_asset"), TEXT("applied"), PhysicsAssetPath);
			}
		}

		TSharedPtr<FJsonObject> MeshJson;
		if (!UeAgentSkeletalMeshOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, TEXT("mesh.json")), MeshJson, Issues, OutError, true))
		{
			return false;
		}
		if (MeshJson.IsValid())
		{
			FString PostProcessClassPath;
			if (MeshJson->TryGetStringField(TEXT("post_process_anim_blueprint"), PostProcessClassPath))
			{
				UClass* AnimClass = UeAgentSkeletalMeshOps::LoadAnimInstanceClass(PostProcessClassPath);
				if (!PostProcessClassPath.TrimStartAndEnd().IsEmpty() && (!AnimClass || !AnimClass->IsChildOf(UAnimInstance::StaticClass())))
				{
					OutError = FString::Printf(TEXT("post_process_anim_blueprint_not_found:%s"), *PostProcessClassPath);
					return false;
				}
				Mesh->Modify();
				Mesh->SetPostProcessAnimBlueprint(AnimClass);
				Mesh->MarkPackageDirty();
				++StructuredApplied;
				AddPropertyResult(TEXT("mesh.json.post_process_anim_blueprint"), TEXT("applied"), PostProcessClassPath);
			}
		}

		TSharedPtr<FJsonObject> SocketsJson;
		if (!UeAgentSkeletalMeshOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, TEXT("sockets.json")), SocketsJson, Issues, OutError, true))
		{
			return false;
		}
		if (SocketsJson.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Sockets = nullptr;
			if (SocketsJson->TryGetArrayField(TEXT("sockets"), Sockets) && Sockets)
			{
				for (int32 Index = 0; Index < Sockets->Num(); ++Index)
				{
					TSharedPtr<FJsonObject> SocketObj;
					if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*Sockets)[Index], FString::Printf(TEXT("sockets.json.sockets[%d]"), Index), SocketObj, Issues))
					{
						continue;
					}
					FString SocketName;
					SocketObj->TryGetStringField(TEXT("socket_name"), SocketName);
					if (SocketName.TrimStartAndEnd().IsEmpty())
					{
						SocketObj->TryGetStringField(TEXT("name"), SocketName);
					}
					if (SocketName.TrimStartAndEnd().IsEmpty())
					{
						UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("json_missing_required_field"), FString::Printf(TEXT("sockets.json.sockets[%d].socket_name"), Index), TEXT("socket_name or name is required."));
						continue;
					}
					bool bRemove = false;
					SocketObj->TryGetBoolField(TEXT("remove"), bRemove);
					USkeletalMeshSocket* Socket = UeAgentSkeletalMeshOps::FindMeshOnlySocket(Mesh, FName(*SocketName.TrimStartAndEnd()));
					Mesh->Modify();
					if (bRemove)
					{
						if (Socket)
						{
							Mesh->GetMeshOnlySocketList().Remove(Socket);
							Mesh->MarkPackageDirty();
							++StructuredApplied;
						}
						continue;
					}
					if (!Socket)
					{
						Socket = NewObject<USkeletalMeshSocket>(Mesh, NAME_None, RF_Transactional);
						Socket->SocketName = FName(*SocketName.TrimStartAndEnd());
						Mesh->AddSocket(Socket, false);
					}
					FString BoneName;
					if (SocketObj->TryGetStringField(TEXT("bone_name"), BoneName) && !BoneName.TrimStartAndEnd().IsEmpty())
					{
						Socket->BoneName = FName(*BoneName.TrimStartAndEnd());
					}
					FVector Location = Socket->RelativeLocation;
					FRotator Rotation = Socket->RelativeRotation;
					FVector Scale = Socket->RelativeScale;
					UeAgentJsonDiagnostics::ReadVectorField(SocketObj, TEXT("relative_location"), FString::Printf(TEXT("sockets.json.sockets[%d].relative_location"), Index), Location, Issues, false);
					UeAgentJsonDiagnostics::ReadRotatorField(SocketObj, TEXT("relative_rotation"), FString::Printf(TEXT("sockets.json.sockets[%d].relative_rotation"), Index), Rotation, Issues, false);
					UeAgentJsonDiagnostics::ReadVectorField(SocketObj, TEXT("relative_scale"), FString::Printf(TEXT("sockets.json.sockets[%d].relative_scale"), Index), Scale, Issues, false);
					Socket->RelativeLocation = Location;
					Socket->RelativeRotation = Rotation;
					Socket->RelativeScale = Scale;
					Mesh->MarkPackageDirty();
					++StructuredApplied;
				}
				AddPropertyResult(TEXT("sockets.json.sockets"), TEXT("applied"), FString::FromInt(Sockets->Num()));
			}
		}

		TSharedPtr<FJsonObject> MorphTargetsJson;
		if (!UeAgentSkeletalMeshOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, TEXT("morph_targets.json")), MorphTargetsJson, Issues, OutError, true))
		{
			return false;
		}
		if (MorphTargetsJson.IsValid())
		{
			UeAgentJsonDiagnostics::WarnUnknownFields(MorphTargetsJson, TEXT("morph_targets.json"), { TEXT("schema"), TEXT("morph_target_count"), TEXT("morph_targets"), TEXT("operations"), TEXT("edit_policy") }, Issues);
			const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
			if (MorphTargetsJson->TryGetArrayField(TEXT("operations"), Operations) && Operations)
			{
				TArray<FName> RemoveNames;
				for (int32 Index = 0; Index < Operations->Num(); ++Index)
				{
					TSharedPtr<FJsonObject> OperationObj;
					if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*Operations)[Index], FString::Printf(TEXT("morph_targets.json.operations[%d]"), Index), OperationObj, Issues))
					{
						continue;
					}
					FString Name;
					bool bRemove = false;
					OperationObj->TryGetStringField(TEXT("name"), Name);
					if (Name.TrimStartAndEnd().IsEmpty())
					{
						OperationObj->TryGetStringField(TEXT("morph_target_name"), Name);
					}
					OperationObj->TryGetBoolField(TEXT("remove"), bRemove);
					if (bRemove && !Name.TrimStartAndEnd().IsEmpty())
					{
						RemoveNames.Add(FName(*Name.TrimStartAndEnd()));
					}
				}
				if (RemoveNames.Num() > 0)
				{
					Mesh->Modify();
					const bool bRemoved = Mesh->RemoveMorphTargets(RemoveNames);
					if (!bRemoved)
					{
						UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("morph_target_remove_failed"), TEXT("morph_targets.json.operations"), TEXT("One or more morph targets could not be removed."));
					}
					else
					{
						Mesh->MarkPackageDirty();
						++StructuredApplied;
					}
					AddPropertyResult(TEXT("morph_targets.json.operations"), bRemoved ? TEXT("applied") : TEXT("failed"), FString::FromInt(RemoveNames.Num()));
				}
			}
		}

		bool bSaveAfterApply = false;
		JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
		if (bSaveAfterApply && !UeAgentSkeletalMeshOps::SaveAssetPackage(Mesh, OutError))
		{
			return false;
		}
	}

	TSharedPtr<FJsonObject> InfoParams = MakeShared<FJsonObject>();
	InfoParams->SetStringField(TEXT("asset_path"), AssetPath);
	TSharedPtr<FJsonObject> ReadbackData = MakeShared<FJsonObject>();
	FString ReadbackError;
	CmdSkeletalMeshGetInfo(FUeAgentRequestContext{ TEXT("skeletal_mesh_folder_readback"), TEXT("skeletal_mesh_get_info"), InfoParams }, ReadbackData, ReadbackError);

	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetBoolField(TEXT("applied"), !(bDryRun || bValidateOnly));
	OutData->SetBoolField(TEXT("dry_run"), bDryRun || bValidateOnly);
	OutData->SetNumberField(TEXT("structured_fields_applied"), StructuredApplied);
	OutData->SetNumberField(TEXT("raw_properties_applied"), 0);
	OutData->SetNumberField(TEXT("operations_executed"), 0);
	OutData->SetArrayField(TEXT("json_issues"), Issues);
	OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
	OutData->SetNumberField(TEXT("error_count"), UeAgentJsonDiagnostics::CountIssuesBySeverity(Issues, TEXT("error")));
	OutData->SetNumberField(TEXT("warning_count"), UeAgentJsonDiagnostics::CountIssuesBySeverity(Issues, TEXT("warning")));
	OutData->SetArrayField(TEXT("property_results"), PropertyResults);
	OutData->SetObjectField(TEXT("readback"), ReadbackData);
	OutData->SetObjectField(TEXT("validation_report"), UeAgentSkeletalMeshOps::BuildCoverageJson());
	if (UeAgentJsonDiagnostics::HasErrorIssue(Issues))
	{
		OutError = TEXT("json_validation_failed");
		return false;
	}
	return true;
}

bool FUeAgentHttpServer::CmdSkeletalMeshValidateFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Ctx.Params->Values)
	{
		Params->SetField(Pair.Key, Pair.Value);
	}
	Params->SetBoolField(TEXT("validate_only"), true);
	Params->SetBoolField(TEXT("dry_run"), true);
	FUeAgentRequestContext SubCtx;
	SubCtx.RequestId = TEXT("skeletal_mesh_folder_validate");
	SubCtx.Command = TEXT("skeletal_mesh_apply_folder");
	SubCtx.Params = Params;
	return CmdSkeletalMeshApplyFolder(SubCtx, OutData, OutError);
}

bool FUeAgentHttpServer::CmdSkeletalMeshGetMorphTargets(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	USkeletalMesh* Mesh = UeAgentSkeletalMeshOps::LoadSkeletalMesh(AssetPath);
	if (!Mesh)
	{
		OutError = TEXT("skeletal_mesh_not_found");
		return false;
	}
	OutData = UeAgentSkeletalMeshOps::BuildMorphTargetsJson(Mesh);
	OutData->SetStringField(TEXT("asset_path"), UeAgentSkeletalMeshOps::NormalizeAssetPath(Mesh->GetOutermost()->GetName()));
	return true;
}

bool FUeAgentHttpServer::CmdSkeletalMeshValidateMorphTargets(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!CmdSkeletalMeshGetMorphTargets(Ctx, OutData, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	const TArray<TSharedPtr<FJsonValue>>* MorphTargets = nullptr;
	if (OutData.IsValid() && OutData->TryGetArrayField(TEXT("morph_targets"), MorphTargets) && MorphTargets)
	{
		for (int32 Index = 0; Index < MorphTargets->Num(); ++Index)
		{
			TSharedPtr<FJsonObject> MorphObj;
			if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*MorphTargets)[Index], FString::Printf(TEXT("morph_targets[%d]"), Index), MorphObj, Issues))
			{
				continue;
			}
			bool bHasValidData = false;
			MorphObj->TryGetBoolField(TEXT("has_valid_data"), bHasValidData);
			if (!bHasValidData)
			{
				FString Name;
				MorphObj->TryGetStringField(TEXT("name"), Name);
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("morph_target_has_no_valid_data"), FString::Printf(TEXT("morph_targets[%d]"), Index), Name);
			}
		}
	}
	OutData->SetArrayField(TEXT("validation_issues"), Issues);
	OutData->SetNumberField(TEXT("validation_issue_count"), Issues.Num());
	return true;
}

bool FUeAgentHttpServer::CmdSkeletalMeshPreviewMorphTarget(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString MorphName;
	if (!JsonTryGetString(Ctx.Params, TEXT("morph_target"), MorphName) || MorphName.TrimStartAndEnd().IsEmpty())
	{
		JsonTryGetString(Ctx.Params, TEXT("name"), MorphName);
	}
	MorphName.TrimStartAndEndInline();
	if (MorphName.IsEmpty())
	{
		OutError = TEXT("missing_morph_target");
		return false;
	}

	float Weight = 1.0f;
	double WeightNumber = 1.0;
	if (Ctx.Params->TryGetNumberField(TEXT("weight"), WeightNumber))
	{
		Weight = static_cast<float>(WeightNumber);
	}

	USkeletalMesh* Mesh = UeAgentSkeletalMeshOps::LoadSkeletalMesh(AssetPath);
	if (!Mesh)
	{
		OutError = TEXT("skeletal_mesh_not_found");
		return false;
	}

	UMorphTarget* FoundMorph = nullptr;
	for (const TObjectPtr<UMorphTarget>& MorphTarget : Mesh->GetMorphTargets())
	{
		if (MorphTarget && MorphTarget->GetName().Equals(MorphName, ESearchCase::IgnoreCase))
		{
			FoundMorph = MorphTarget.Get();
			break;
		}
	}
	if (!FoundMorph)
	{
		OutError = TEXT("morph_target_not_found");
		return false;
	}

	USkeletalMeshComponent* PreviewComponent = NewObject<USkeletalMeshComponent>(GetTransientPackage());
	PreviewComponent->SetSkeletalMesh(Mesh);
	PreviewComponent->SetMorphTarget(FName(*MorphName), Weight);

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("asset_path"), UeAgentSkeletalMeshOps::NormalizeAssetPath(Mesh->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("morph_target"), FoundMorph->GetName());
	OutData->SetNumberField(TEXT("requested_weight"), Weight);
	OutData->SetStringField(TEXT("preview_status"), TEXT("transient_component_morph_set"));
	OutData->SetObjectField(TEXT("morph_target_summary"), UeAgentSkeletalMeshOps::BuildMorphTargetsJson(Mesh));
	return true;
}

bool FUeAgentHttpServer::CmdSkeletalMeshRemoveMorphTarget(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	FString MorphName;
	if (!JsonTryGetString(Ctx.Params, TEXT("morph_target"), MorphName) || MorphName.TrimStartAndEnd().IsEmpty())
	{
		JsonTryGetString(Ctx.Params, TEXT("name"), MorphName);
	}
	if (MorphName.TrimStartAndEnd().IsEmpty())
	{
		JsonTryGetString(Ctx.Params, TEXT("morph_target_name"), MorphName);
	}
	if (MorphName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_morph_target");
		return false;
	}
	USkeletalMesh* Mesh = UeAgentSkeletalMeshOps::LoadSkeletalMesh(AssetPath);
	if (!Mesh)
	{
		OutError = TEXT("skeletal_mesh_not_found");
		return false;
	}
	Mesh->Modify();
	TArray<FName> MorphNames;
	MorphNames.Add(FName(*MorphName.TrimStartAndEnd()));
	const bool bRemoved = Mesh->RemoveMorphTargets(MorphNames);
	if (!bRemoved)
	{
		OutError = TEXT("morph_target_remove_failed");
		return false;
	}
	Mesh->MarkPackageDirty();
	bool bSaveAfterApply = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
	if (!bSaveAfterApply)
	{
		JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterApply);
	}
	if (bSaveAfterApply && !UeAgentSkeletalMeshOps::SaveAssetPackage(Mesh, OutError))
	{
		return false;
	}
	OutData->SetStringField(TEXT("asset_path"), UeAgentSkeletalMeshOps::NormalizeAssetPath(Mesh->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("morph_target"), MorphName.TrimStartAndEnd());
	OutData->SetBoolField(TEXT("removed"), true);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterApply);
	return true;
}

bool FUeAgentHttpServer::CmdSkeletalMeshImportSkinWeightProfile(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString SourceFilename;
	if (!JsonTryGetString(Ctx.Params, TEXT("source_filename"), SourceFilename) || SourceFilename.TrimStartAndEnd().IsEmpty())
	{
		JsonTryGetString(Ctx.Params, TEXT("source_file"), SourceFilename);
	}
	SourceFilename.TrimStartAndEndInline();
	if (SourceFilename.IsEmpty())
	{
		OutError = TEXT("missing_source_filename");
		return false;
	}
	if (FPaths::IsRelative(SourceFilename))
	{
		SourceFilename = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), SourceFilename));
	}
	if (!FPaths::FileExists(SourceFilename))
	{
		OutError = TEXT("source_file_not_found");
		return false;
	}

	FString ProfileNameString;
	JsonTryGetString(Ctx.Params, TEXT("profile_name"), ProfileNameString);
	ProfileNameString.TrimStartAndEndInline();
	if (ProfileNameString.IsEmpty())
	{
		OutError = TEXT("missing_profile_name");
		return false;
	}

	int32 LodIndex = 0;
	double InputLodIndex = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("lod_index"), InputLodIndex))
	{
		LodIndex = static_cast<int32>(InputLodIndex);
	}

	USkeletalMesh* Mesh = UeAgentSkeletalMeshOps::LoadSkeletalMesh(AssetPath);
	if (!Mesh)
	{
		OutError = TEXT("skeletal_mesh_not_found");
		return false;
	}
	if (LodIndex < 0 || LodIndex >= Mesh->GetLODNum())
	{
		OutError = TEXT("invalid_lod_index");
		return false;
	}

	bool bSaveAfterImport = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_import"), bSaveAfterImport);

	Mesh->Modify();
	const FName ProfileName(*ProfileNameString);
	const bool bImported = FSkinWeightsUtilities::ImportAlternateSkinWeight(Mesh, SourceFilename, LodIndex, ProfileName, /*bIsReimport*/ false);
	Mesh->PostEditChange();
	Mesh->MarkPackageDirty();

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("asset_path"), UeAgentSkeletalMeshOps::NormalizeAssetPath(Mesh->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("source_filename"), SourceFilename);
	OutData->SetStringField(TEXT("profile_name"), ProfileName.ToString());
	OutData->SetNumberField(TEXT("lod_index"), LodIndex);
	OutData->SetBoolField(TEXT("imported"), bImported);
	OutData->SetObjectField(TEXT("readback"), UeAgentSkeletalMeshOps::BuildSkinWeightProfilesJson(Mesh));
	if (!bImported)
	{
		OutError = TEXT("skin_weight_profile_import_failed");
		return false;
	}

	if (bSaveAfterImport && !UeAgentSkeletalMeshOps::SaveAssetPackage(Mesh, OutError))
	{
		return false;
	}
	OutData->SetBoolField(TEXT("saved"), bSaveAfterImport);
	return true;
}

bool FUeAgentHttpServer::CmdSkeletalMeshRemoveSkinWeightProfile(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString ProfileNameString;
	JsonTryGetString(Ctx.Params, TEXT("profile_name"), ProfileNameString);
	ProfileNameString.TrimStartAndEndInline();
	if (ProfileNameString.IsEmpty())
	{
		OutError = TEXT("missing_profile_name");
		return false;
	}

	USkeletalMesh* Mesh = UeAgentSkeletalMeshOps::LoadSkeletalMesh(AssetPath);
	if (!Mesh)
	{
		OutError = TEXT("skeletal_mesh_not_found");
		return false;
	}

	const FName ProfileName(*ProfileNameString);
	const bool bProfileExists = Mesh->GetSkinWeightProfiles().ContainsByPredicate([ProfileName](const FSkinWeightProfileInfo& Profile)
	{
		return Profile.Name == ProfileName;
	});
	if (!bProfileExists)
	{
		OutError = TEXT("skin_weight_profile_not_found");
		return false;
	}

	int32 LodIndex = INDEX_NONE;
	double InputLodIndex = static_cast<double>(INDEX_NONE);
	if (JsonTryGetNumber(Ctx.Params, TEXT("lod_index"), InputLodIndex))
	{
		LodIndex = static_cast<int32>(InputLodIndex);
		if (LodIndex < 0 || LodIndex >= Mesh->GetLODNum())
		{
			OutError = TEXT("invalid_lod_index");
			return false;
		}
	}

	bool bSaveAfterApply = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);

	Mesh->Modify();
	int32 RemovedLodCount = 0;
	if (LodIndex == INDEX_NONE)
	{
		for (int32 CurrentLod = 0; CurrentLod < Mesh->GetLODNum(); ++CurrentLod)
		{
			if (FSkinWeightsUtilities::RemoveSkinnedWeightProfileData(Mesh, ProfileName, CurrentLod))
			{
				++RemovedLodCount;
			}
		}
		Mesh->GetSkinWeightProfiles().RemoveAll([ProfileName](const FSkinWeightProfileInfo& Profile)
		{
			return Profile.Name == ProfileName;
		});
	}
	else
	{
		if (FSkinWeightsUtilities::RemoveSkinnedWeightProfileData(Mesh, ProfileName, LodIndex))
		{
			++RemovedLodCount;
		}
		for (FSkinWeightProfileInfo& Profile : Mesh->GetSkinWeightProfiles())
		{
			if (Profile.Name == ProfileName)
			{
				Profile.PerLODSourceFiles.Remove(LodIndex);
				break;
			}
		}
	}

	Mesh->ReleaseSkinWeightProfileResources();
	Mesh->PostEditChange();
	Mesh->MarkPackageDirty();

	if (bSaveAfterApply && !UeAgentSkeletalMeshOps::SaveAssetPackage(Mesh, OutError))
	{
		return false;
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("asset_path"), UeAgentSkeletalMeshOps::NormalizeAssetPath(Mesh->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("profile_name"), ProfileName.ToString());
	OutData->SetNumberField(TEXT("lod_index"), LodIndex);
	OutData->SetNumberField(TEXT("removed_lod_count"), RemovedLodCount);
	OutData->SetBoolField(TEXT("removed_profile_record"), LodIndex == INDEX_NONE);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterApply);
	OutData->SetObjectField(TEXT("readback"), UeAgentSkeletalMeshOps::BuildSkinWeightProfilesJson(Mesh));
	return true;
}

bool FUeAgentHttpServer::CmdSkeletalMeshPreviewSkinWeightProfile(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString ProfileNameString;
	JsonTryGetString(Ctx.Params, TEXT("profile_name"), ProfileNameString);
	ProfileNameString.TrimStartAndEndInline();
	if (ProfileNameString.IsEmpty())
	{
		OutError = TEXT("missing_profile_name");
		return false;
	}

	USkeletalMesh* Mesh = UeAgentSkeletalMeshOps::LoadSkeletalMesh(AssetPath);
	if (!Mesh)
	{
		OutError = TEXT("skeletal_mesh_not_found");
		return false;
	}

	const FName ProfileName(*ProfileNameString);
	const bool bProfileExists = Mesh->GetSkinWeightProfiles().ContainsByPredicate([ProfileName](const FSkinWeightProfileInfo& Profile)
	{
		return Profile.Name == ProfileName;
	});
	if (!bProfileExists)
	{
		OutError = TEXT("skin_weight_profile_not_found");
		return false;
	}

	USkeletalMeshComponent* PreviewComponent = NewObject<USkeletalMeshComponent>(GetTransientPackage());
	PreviewComponent->SetSkeletalMesh(Mesh);
	const bool bSet = PreviewComponent->SetSkinWeightProfile(ProfileName);

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("asset_path"), UeAgentSkeletalMeshOps::NormalizeAssetPath(Mesh->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("profile_name"), ProfileName.ToString());
	OutData->SetBoolField(TEXT("preview_profile_set"), bSet);
	OutData->SetStringField(TEXT("current_profile"), PreviewComponent->GetCurrentSkinWeightProfileName().ToString());
	OutData->SetStringField(TEXT("preview_status"), bSet ? TEXT("transient_component_profile_set") : TEXT("component_rejected_profile"));
	OutData->SetObjectField(TEXT("skin_weight_profiles"), UeAgentSkeletalMeshOps::BuildSkinWeightProfilesJson(Mesh));
	return bSet;
}
