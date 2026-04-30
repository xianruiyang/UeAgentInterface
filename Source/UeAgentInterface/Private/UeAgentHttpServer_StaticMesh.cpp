// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_StaticMesh.h"

#include "UeAgentJsonDiagnostics.h"

#include "Editor.h"
#include "EditorReimportHandler.h"
#include "EditorViewportClient.h"
#include "EditorFramework/AssetImportData.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSourceData.h"
#include "Engine/StaticMeshSocket.h"
#include "FileHelpers.h"
#include "IStaticMeshEditor.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/BoxElem.h"
#include "PhysicsEngine/SphereElem.h"
#include "PhysicsEngine/SphylElem.h"
#include "StaticMeshResources.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditorToolkit.h"

namespace UeAgentStaticMeshOps
{
	static TSharedPtr<FJsonObject> MakeVectorJson(const FVector& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), Value.X);
		Obj->SetNumberField(TEXT("y"), Value.Y);
		Obj->SetNumberField(TEXT("z"), Value.Z);
		return Obj;
	}

	static TSharedPtr<FJsonObject> MakeRotatorJson(const FRotator& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("pitch"), Value.Pitch);
		Obj->SetNumberField(TEXT("yaw"), Value.Yaw);
		Obj->SetNumberField(TEXT("roll"), Value.Roll);
		return Obj;
	}

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
		if (AssetName.IsEmpty())
		{
			return FString();
		}
		return FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
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

	static UStaticMesh* LoadStaticMeshAsset(const FString& InPath)
	{
		return Cast<UStaticMesh>(LoadAssetObject(InPath));
	}

	static UMaterialInterface* LoadMaterialAsset(const FString& InPath)
	{
		return Cast<UMaterialInterface>(LoadAssetObject(InPath));
	}

	static bool SaveMeshPackage(UStaticMesh* Mesh, FString& OutError)
	{
		if (!Mesh)
		{
			OutError = TEXT("static_mesh_not_found");
			return false;
		}
		TArray<UPackage*> PackagesToSave;
		PackagesToSave.Add(Mesh->GetOutermost());
		if (!UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false))
		{
			OutError = TEXT("save_asset_failed");
			return false;
		}
		return true;
	}

	static FProperty* FindPropertyByNameLoose(UStruct* StructType, const FString& Segment)
	{
		if (!StructType || Segment.IsEmpty())
		{
			return nullptr;
		}
		const FString Normalized = Segment.TrimStartAndEnd();
		const FString NormalizedWithoutB = Normalized.StartsWith(TEXT("b")) ? Normalized.Mid(1) : Normalized;
		for (TFieldIterator<FProperty> It(StructType, EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			const FString PropertyName = Property->GetName();
			const FString PropertyNameWithoutB = PropertyName.StartsWith(TEXT("b")) ? PropertyName.Mid(1) : PropertyName;
			if (PropertyName.Equals(Normalized, ESearchCase::IgnoreCase) ||
				PropertyNameWithoutB.Equals(NormalizedWithoutB, ESearchCase::IgnoreCase) ||
				Property->GetAuthoredName().Equals(Normalized, ESearchCase::IgnoreCase))
			{
				return Property;
			}
		}
		return nullptr;
	}

	static bool ResolvePropertyPath(UObject* Obj, const FString& PropertyPath, FProperty*& OutProperty, void*& OutValuePtr)
	{
		OutProperty = nullptr;
		OutValuePtr = nullptr;
		if (!Obj || PropertyPath.IsEmpty())
		{
			return false;
		}

		TArray<FString> Segments;
		PropertyPath.ParseIntoArray(Segments, TEXT("."), true);
		if (Segments.Num() == 0)
		{
			return false;
		}

		UStruct* CurrentStruct = Obj->GetClass();
		void* CurrentContainer = Obj;
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			FProperty* Property = FindPropertyByNameLoose(CurrentStruct, Segments[Index]);
			if (!Property)
			{
				return false;
			}

			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(CurrentContainer);
			if (Index == Segments.Num() - 1)
			{
				OutProperty = Property;
				OutValuePtr = ValuePtr;
				return true;
			}

			FStructProperty* StructProperty = CastField<FStructProperty>(Property);
			if (!StructProperty)
			{
				return false;
			}
			CurrentStruct = StructProperty->Struct;
			CurrentContainer = ValuePtr;
		}
		return false;
	}

	static void ApplyMeshPostEdit(UStaticMesh* Mesh, bool bRebuildPhysics)
	{
		if (!Mesh)
		{
			return;
		}

		if (bRebuildPhysics)
		{
			Mesh->CreateBodySetup();
			if (UBodySetup* BodySetup = Mesh->GetBodySetup())
			{
				BodySetup->InvalidatePhysicsData();
				BodySetup->CreatePhysicsMeshes();
			}
		}

		Mesh->MarkPackageDirty();
		Mesh->PostEditChange();
	}

	static IStaticMeshEditor* GetStaticMeshEditor(UStaticMesh* Mesh, const bool bOpenIfNeeded)
	{
		if (!Mesh || !GEditor)
		{
			return nullptr;
		}

		UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (!AssetEditorSubsystem)
		{
			return nullptr;
		}

		if (bOpenIfNeeded)
		{
			AssetEditorSubsystem->OpenEditorForAsset(Mesh);
		}

		IAssetEditorInstance* EditorInstance = AssetEditorSubsystem->FindEditorForAsset(Mesh, false);
		if (!EditorInstance)
		{
			return nullptr;
		}

		if (EditorInstance->GetEditorName() != FName(TEXT("StaticMeshEditor")))
		{
			return nullptr;
		}

		FAssetEditorToolkit* Toolkit = static_cast<FAssetEditorToolkit*>(EditorInstance);
		return Toolkit ? static_cast<IStaticMeshEditor*>(Toolkit) : nullptr;
	}

	static TSharedPtr<FJsonObject> SocketToJson(const UStaticMeshSocket* Socket)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Socket)
		{
			return Obj;
		}
		Obj->SetStringField(TEXT("socket_name"), Socket->SocketName.ToString());
		Obj->SetStringField(TEXT("tag"), Socket->Tag);
		Obj->SetNumberField(TEXT("location_x"), Socket->RelativeLocation.X);
		Obj->SetNumberField(TEXT("location_y"), Socket->RelativeLocation.Y);
		Obj->SetNumberField(TEXT("location_z"), Socket->RelativeLocation.Z);
		Obj->SetNumberField(TEXT("rotation_pitch"), Socket->RelativeRotation.Pitch);
		Obj->SetNumberField(TEXT("rotation_yaw"), Socket->RelativeRotation.Yaw);
		Obj->SetNumberField(TEXT("rotation_roll"), Socket->RelativeRotation.Roll);
		Obj->SetNumberField(TEXT("scale_x"), Socket->RelativeScale.X);
		Obj->SetNumberField(TEXT("scale_y"), Socket->RelativeScale.Y);
		Obj->SetNumberField(TEXT("scale_z"), Socket->RelativeScale.Z);
		return Obj;
	}

	static TSharedPtr<FJsonObject> MaterialSlotToJson(const FStaticMaterial& MaterialSlot, const int32 SlotIndex)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("slot_index"), SlotIndex);
		Obj->SetStringField(TEXT("slot_name"), MaterialSlot.MaterialSlotName.ToString());
		Obj->SetStringField(TEXT("imported_slot_name"), MaterialSlot.ImportedMaterialSlotName.ToString());
		Obj->SetStringField(TEXT("material_path"), MaterialSlot.MaterialInterface ? MaterialSlot.MaterialInterface->GetPathName() : TEXT(""));
		Obj->SetStringField(TEXT("overlay_material_path"), MaterialSlot.OverlayMaterialInterface ? MaterialSlot.OverlayMaterialInterface->GetPathName() : TEXT(""));
		return Obj;
	}

	static TSharedPtr<FJsonObject> SphereElemToJson(const FKSphereElem& SphereElem)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetObjectField(TEXT("center"), MakeVectorJson(SphereElem.Center));
		Obj->SetNumberField(TEXT("radius"), SphereElem.Radius);
		return Obj;
	}

	static TSharedPtr<FJsonObject> CapsuleElemToJson(const FKSphylElem& CapsuleElem)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetObjectField(TEXT("center"), MakeVectorJson(CapsuleElem.Center));
		Obj->SetObjectField(TEXT("rotation"), MakeRotatorJson(CapsuleElem.Rotation));
		Obj->SetNumberField(TEXT("radius"), CapsuleElem.Radius);
		Obj->SetNumberField(TEXT("length"), CapsuleElem.Length);
		return Obj;
	}

	static int32 ResolveMaterialSlotIndex(const UStaticMesh* Mesh, const TSharedPtr<FJsonObject>& Params, FString& OutError)
	{
		if (!Mesh)
		{
			OutError = TEXT("static_mesh_not_found");
			return INDEX_NONE;
		}

		double SlotIndexValue = 0.0;
		if (Params.IsValid() && Params->TryGetNumberField(TEXT("slot_index"), SlotIndexValue))
		{
			const int32 SlotIndex = static_cast<int32>(SlotIndexValue);
			if (!Mesh->GetStaticMaterials().IsValidIndex(SlotIndex))
			{
				OutError = TEXT("invalid_slot_index");
				return INDEX_NONE;
			}
			return SlotIndex;
		}

		FString SlotName;
		if (Params.IsValid() && Params->TryGetStringField(TEXT("slot_name"), SlotName))
		{
			SlotName = SlotName.TrimStartAndEnd();
			for (int32 SlotIndex = 0; SlotIndex < Mesh->GetStaticMaterials().Num(); ++SlotIndex)
			{
				const FStaticMaterial& MaterialSlot = Mesh->GetStaticMaterials()[SlotIndex];
				if (MaterialSlot.MaterialSlotName.ToString().Equals(SlotName, ESearchCase::IgnoreCase) ||
					MaterialSlot.ImportedMaterialSlotName.ToString().Equals(SlotName, ESearchCase::IgnoreCase))
				{
					return SlotIndex;
				}
			}

			OutError = TEXT("slot_name_not_found");
			return INDEX_NONE;
		}

		OutError = TEXT("missing_slot_index_or_slot_name");
		return INDEX_NONE;
	}

	static void FillBoundsFields(const FBoxSphereBounds& Bounds, TSharedPtr<FJsonObject>& OutData)
	{
		const FVector Min = Bounds.Origin - Bounds.BoxExtent;
		const FVector Max = Bounds.Origin + Bounds.BoxExtent;
		OutData->SetObjectField(TEXT("bounds_origin"), MakeVectorJson(Bounds.Origin));
		OutData->SetObjectField(TEXT("bounds_extent"), MakeVectorJson(Bounds.BoxExtent));
		OutData->SetObjectField(TEXT("bounds_min"), MakeVectorJson(Min));
		OutData->SetObjectField(TEXT("bounds_max"), MakeVectorJson(Max));
		OutData->SetNumberField(TEXT("bounds_sphere_radius"), Bounds.SphereRadius);
	}

	static void FillLocalCornersFields(const FBoxSphereBounds& Bounds, TSharedPtr<FJsonObject>& OutData)
	{
		const FVector Min = Bounds.Origin - Bounds.BoxExtent;
		const FVector Max = Bounds.Origin + Bounds.BoxExtent;

		struct FNamedCorner
		{
			const TCHAR* Name;
			FVector Position;
		};

		const FNamedCorner Corners[] =
		{
			{ TEXT("min_min_min"), FVector(Min.X, Min.Y, Min.Z) },
			{ TEXT("min_min_max"), FVector(Min.X, Min.Y, Max.Z) },
			{ TEXT("min_max_min"), FVector(Min.X, Max.Y, Min.Z) },
			{ TEXT("min_max_max"), FVector(Min.X, Max.Y, Max.Z) },
			{ TEXT("max_min_min"), FVector(Max.X, Min.Y, Min.Z) },
			{ TEXT("max_min_max"), FVector(Max.X, Min.Y, Max.Z) },
			{ TEXT("max_max_min"), FVector(Max.X, Max.Y, Min.Z) },
			{ TEXT("max_max_max"), FVector(Max.X, Max.Y, Max.Z) }
		};

		TArray<TSharedPtr<FJsonValue>> CornerArray;
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(Corners); ++Index)
		{
			TSharedPtr<FJsonObject> CornerObj = MakeShared<FJsonObject>();
			CornerObj->SetNumberField(TEXT("index"), Index);
			CornerObj->SetStringField(TEXT("name"), Corners[Index].Name);
			CornerObj->SetObjectField(TEXT("position"), MakeVectorJson(Corners[Index].Position));
			CornerArray.Add(MakeShared<FJsonValueObject>(CornerObj));
		}

		OutData->SetArrayField(TEXT("local_corners"), CornerArray);
		OutData->SetNumberField(TEXT("corner_count"), CornerArray.Num());
	}

	static FString GetFolderRootAbsolute()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("StaticMesh")));
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

	static TSharedPtr<FJsonObject> BuildAssetJson(UStaticMesh* Mesh)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.asset_folder.v1"));
		Obj->SetStringField(TEXT("profile"), TEXT("static_mesh"));
		Obj->SetNumberField(TEXT("schema_version"), 1);
		Obj->SetStringField(TEXT("asset_path"), Mesh ? NormalizeAssetPath(Mesh->GetOutermost()->GetName()) : TEXT(""));
		Obj->SetStringField(TEXT("object_path"), Mesh ? Mesh->GetPathName() : TEXT(""));
		Obj->SetStringField(TEXT("asset_class"), TEXT("/Script/Engine.StaticMesh"));
		Obj->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
		Obj->SetStringField(TEXT("exported_at"), FDateTime::Now().ToIso8601());
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildBuildSettingsJson(const FMeshBuildSettings& Settings)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("use_mikk_tspace"), Settings.bUseMikkTSpace != 0);
		Obj->SetBoolField(TEXT("recompute_normals"), Settings.bRecomputeNormals != 0);
		Obj->SetBoolField(TEXT("recompute_tangents"), Settings.bRecomputeTangents != 0);
		Obj->SetBoolField(TEXT("compute_weighted_normals"), Settings.bComputeWeightedNormals != 0);
		Obj->SetBoolField(TEXT("remove_degenerates"), Settings.bRemoveDegenerates != 0);
		Obj->SetBoolField(TEXT("build_reversed_index_buffer"), Settings.bBuildReversedIndexBuffer != 0);
		Obj->SetBoolField(TEXT("use_high_precision_tangent_basis"), Settings.bUseHighPrecisionTangentBasis != 0);
		Obj->SetBoolField(TEXT("use_full_precision_uvs"), Settings.bUseFullPrecisionUVs != 0);
		Obj->SetBoolField(TEXT("generate_lightmap_uvs"), Settings.bGenerateLightmapUVs != 0);
		Obj->SetBoolField(TEXT("generate_distance_field_as_if_two_sided"), Settings.bGenerateDistanceFieldAsIfTwoSided != 0);
		Obj->SetNumberField(TEXT("min_lightmap_resolution"), Settings.MinLightmapResolution);
		Obj->SetNumberField(TEXT("source_lightmap_index"), Settings.SrcLightmapIndex);
		Obj->SetNumberField(TEXT("destination_lightmap_index"), Settings.DstLightmapIndex);
		Obj->SetObjectField(TEXT("build_scale_3d"), MakeVectorJson(Settings.BuildScale3D));
		Obj->SetNumberField(TEXT("distance_field_resolution_scale"), Settings.DistanceFieldResolutionScale);
		Obj->SetNumberField(TEXT("max_lumen_mesh_cards"), Settings.MaxLumenMeshCards);
		Obj->SetStringField(TEXT("distance_field_replacement_mesh"), Settings.DistanceFieldReplacementMesh ? Settings.DistanceFieldReplacementMesh->GetPathName() : TEXT(""));
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildNaniteJson(UStaticMesh* Mesh)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Mesh)
		{
			return Obj;
		}
		const FMeshNaniteSettings& Settings = Mesh->NaniteSettings;
		Obj->SetBoolField(TEXT("enabled"), Settings.bEnabled != 0);
		Obj->SetBoolField(TEXT("preserve_area"), Settings.bPreserveArea != 0);
		Obj->SetBoolField(TEXT("explicit_tangents"), Settings.bExplicitTangents != 0);
		Obj->SetBoolField(TEXT("lerp_uvs"), Settings.bLerpUVs != 0);
		Obj->SetNumberField(TEXT("keep_percent_triangles"), Settings.KeepPercentTriangles);
		Obj->SetNumberField(TEXT("trim_relative_error"), Settings.TrimRelativeError);
		Obj->SetNumberField(TEXT("fallback_percent_triangles"), Settings.FallbackPercentTriangles);
		Obj->SetNumberField(TEXT("fallback_relative_error"), Settings.FallbackRelativeError);
		Obj->SetNumberField(TEXT("max_edge_length_factor"), Settings.MaxEdgeLengthFactor);
		Obj->SetNumberField(TEXT("position_precision"), Settings.PositionPrecision);
		Obj->SetNumberField(TEXT("normal_precision"), Settings.NormalPrecision);
		Obj->SetNumberField(TEXT("tangent_precision"), Settings.TangentPrecision);
		Obj->SetNumberField(TEXT("target_minimum_residency_kb"), Settings.TargetMinimumResidencyInKB);
		Obj->SetStringField(TEXT("edit_policy"), TEXT("safe_settings"));
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildMeshJson(UStaticMesh* Mesh)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("static_mesh"), Mesh ? Mesh->GetPathName() : TEXT(""));
		Obj->SetNumberField(TEXT("lod_count"), Mesh ? Mesh->GetNumLODs() : 0);
		Obj->SetNumberField(TEXT("source_model_count"), Mesh ? Mesh->GetNumSourceModels() : 0);
		Obj->SetNumberField(TEXT("material_slot_count"), Mesh ? Mesh->GetStaticMaterials().Num() : 0);
		Obj->SetNumberField(TEXT("socket_count"), Mesh ? Mesh->Sockets.Num() : 0);
		int32 SimpleCollisionCount = 0;
		int32 TraceFlag = -1;
		if (Mesh)
		{
			if (UBodySetup* BodySetup = Mesh->GetBodySetup())
			{
				SimpleCollisionCount = BodySetup->AggGeom.BoxElems.Num() + BodySetup->AggGeom.SphereElems.Num() + BodySetup->AggGeom.SphylElems.Num() + BodySetup->AggGeom.ConvexElems.Num();
				TraceFlag = static_cast<int32>(BodySetup->CollisionTraceFlag);
			}
			Obj->SetBoolField(TEXT("nanite_enabled"), Mesh->NaniteSettings.bEnabled != 0);
			Obj->SetNumberField(TEXT("lightmap_resolution"), Mesh->GetLightMapResolution());
			Obj->SetNumberField(TEXT("lightmap_coordinate_index"), Mesh->GetLightMapCoordinateIndex());
			Obj->SetBoolField(TEXT("allow_cpu_access"), Mesh->bAllowCPUAccess);
			Obj->SetObjectField(TEXT("bounds"), MakeVectorJson(Mesh->GetBounds().BoxExtent));
		}
		Obj->SetNumberField(TEXT("simple_collision_count"), SimpleCollisionCount);
		Obj->SetNumberField(TEXT("complex_collision_mode"), TraceFlag);
		Obj->SetStringField(TEXT("edit_policy"), TEXT("safe_settings_without_raw_geometry"));
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildMaterialsFolderJson(UStaticMesh* Mesh)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Materials;
		if (Mesh)
		{
			for (int32 SlotIndex = 0; SlotIndex < Mesh->GetStaticMaterials().Num(); ++SlotIndex)
			{
				TSharedPtr<FJsonObject> Item = MaterialSlotToJson(Mesh->GetStaticMaterials()[SlotIndex], SlotIndex);
				TArray<TSharedPtr<FJsonValue>> SectionRefs;
				const int32 LodCount = Mesh->GetNumLODs();
				for (int32 LodIndex = 0; LodIndex < LodCount; ++LodIndex)
				{
					const int32 SectionCount = Mesh->GetNumSections(LodIndex);
					for (int32 SectionIndex = 0; SectionIndex < SectionCount; ++SectionIndex)
					{
						const FMeshSectionInfo Info = Mesh->GetSectionInfoMap().Get(LodIndex, SectionIndex);
						if (Info.MaterialIndex == SlotIndex)
						{
							TSharedPtr<FJsonObject> Ref = MakeShared<FJsonObject>();
							Ref->SetNumberField(TEXT("lod_index"), LodIndex);
							Ref->SetNumberField(TEXT("section_index"), SectionIndex);
							SectionRefs.Add(MakeShared<FJsonValueObject>(Ref));
						}
					}
				}
				Item->SetArrayField(TEXT("section_references"), SectionRefs);
				Materials.Add(MakeShared<FJsonValueObject>(Item));
			}
		}
		Obj->SetArrayField(TEXT("materials"), Materials);
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildLodsIndexJson(UStaticMesh* Mesh)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Lods;
		const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
		const int32 LodCount = Mesh ? Mesh->GetNumLODs() : 0;
		for (int32 LodIndex = 0; LodIndex < LodCount; ++LodIndex)
		{
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetNumberField(TEXT("lod_index"), LodIndex);
			Item->SetNumberField(TEXT("section_count"), Mesh->GetNumSections(LodIndex));
			Item->SetStringField(TEXT("edit_policy"), TEXT("safe_settings"));
			if (RenderData && RenderData->LODResources.IsValidIndex(LodIndex))
			{
				const FStaticMeshLODResources& LOD = RenderData->LODResources[LodIndex];
				Item->SetNumberField(TEXT("vertex_count"), LOD.GetNumVertices());
				Item->SetNumberField(TEXT("index_count"), LOD.IndexBuffer.GetNumIndices());
				Item->SetNumberField(TEXT("triangle_count"), LOD.GetNumTriangles());
				Item->SetNumberField(TEXT("uv_channel_count"), LOD.GetNumTexCoords());
			}
			if (LodIndex < Mesh->GetNumSourceModels())
			{
				const FStaticMeshSourceModel& SourceModel = Mesh->GetSourceModel(LodIndex);
				Item->SetNumberField(TEXT("screen_size"), SourceModel.ScreenSize.Default);
				Item->SetStringField(TEXT("source_import_filename"), SourceModel.SourceImportFilename);
				Item->SetObjectField(TEXT("build_settings_summary"), BuildBuildSettingsJson(SourceModel.BuildSettings));
			}
			Lods.Add(MakeShared<FJsonValueObject>(Item));
		}
		Obj->SetNumberField(TEXT("lod_count"), LodCount);
		Obj->SetArrayField(TEXT("lods"), Lods);
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildSectionsJson(UStaticMesh* Mesh)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> LodItems;
		const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
		const int32 LodCount = Mesh ? Mesh->GetNumLODs() : 0;
		for (int32 LodIndex = 0; LodIndex < LodCount; ++LodIndex)
		{
			TSharedPtr<FJsonObject> LodObj = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> Sections;
			const int32 SectionCount = Mesh->GetNumSections(LodIndex);
			for (int32 SectionIndex = 0; SectionIndex < SectionCount; ++SectionIndex)
			{
				TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
				const FMeshSectionInfo Info = Mesh->GetSectionInfoMap().Get(LodIndex, SectionIndex);
				SectionObj->SetNumberField(TEXT("lod_index"), LodIndex);
				SectionObj->SetNumberField(TEXT("section_index"), SectionIndex);
				SectionObj->SetNumberField(TEXT("material_index"), Info.MaterialIndex);
				SectionObj->SetBoolField(TEXT("collision_enabled"), Info.bEnableCollision);
				SectionObj->SetBoolField(TEXT("casts_shadow"), Info.bCastShadow);
				SectionObj->SetBoolField(TEXT("visible_in_ray_tracing"), Info.bVisibleInRayTracing);
				if (Mesh->GetStaticMaterials().IsValidIndex(Info.MaterialIndex))
				{
					SectionObj->SetStringField(TEXT("material_slot_name"), Mesh->GetStaticMaterials()[Info.MaterialIndex].MaterialSlotName.ToString());
				}
				if (RenderData && RenderData->LODResources.IsValidIndex(LodIndex) && RenderData->LODResources[LodIndex].Sections.IsValidIndex(SectionIndex))
				{
					const FStaticMeshSection& Section = RenderData->LODResources[LodIndex].Sections[SectionIndex];
					SectionObj->SetNumberField(TEXT("first_index"), Section.FirstIndex);
					SectionObj->SetNumberField(TEXT("num_triangles"), Section.NumTriangles);
					SectionObj->SetNumberField(TEXT("min_vertex_index"), Section.MinVertexIndex);
					SectionObj->SetNumberField(TEXT("max_vertex_index"), Section.MaxVertexIndex);
				}
				Sections.Add(MakeShared<FJsonValueObject>(SectionObj));
			}
			LodObj->SetNumberField(TEXT("lod_index"), LodIndex);
			LodObj->SetArrayField(TEXT("sections"), Sections);
			LodItems.Add(MakeShared<FJsonValueObject>(LodObj));
		}
		Obj->SetArrayField(TEXT("lods"), LodItems);
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildCollisionJson(UStaticMesh* Mesh)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Mesh)
		{
			return Obj;
		}
		TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
		FUeAgentRequestContext DummyCtx;
		if (UBodySetup* BodySetup = Mesh->GetBodySetup())
		{
			Obj->SetNumberField(TEXT("collision_complexity"), static_cast<int32>(BodySetup->CollisionTraceFlag));
			TArray<TSharedPtr<FJsonValue>> Boxes;
			for (const FKBoxElem& BoxElem : BodySetup->AggGeom.BoxElems)
			{
				TSharedPtr<FJsonObject> BoxObj = MakeShared<FJsonObject>();
				BoxObj->SetObjectField(TEXT("center"), MakeVectorJson(BoxElem.Center));
				BoxObj->SetObjectField(TEXT("rotation"), MakeRotatorJson(BoxElem.Rotation));
				BoxObj->SetNumberField(TEXT("x"), BoxElem.X);
				BoxObj->SetNumberField(TEXT("y"), BoxElem.Y);
				BoxObj->SetNumberField(TEXT("z"), BoxElem.Z);
				Boxes.Add(MakeShared<FJsonValueObject>(BoxObj));
			}
			Obj->SetArrayField(TEXT("boxes"), Boxes);
			TArray<TSharedPtr<FJsonValue>> Spheres;
			for (const FKSphereElem& SphereElem : BodySetup->AggGeom.SphereElems)
			{
				Spheres.Add(MakeShared<FJsonValueObject>(SphereElemToJson(SphereElem)));
			}
			Obj->SetArrayField(TEXT("spheres"), Spheres);
			TArray<TSharedPtr<FJsonValue>> Capsules;
			for (const FKSphylElem& CapsuleElem : BodySetup->AggGeom.SphylElems)
			{
				Capsules.Add(MakeShared<FJsonValueObject>(CapsuleElemToJson(CapsuleElem)));
			}
			Obj->SetArrayField(TEXT("capsules"), Capsules);
			Obj->SetNumberField(TEXT("convex_hull_count"), BodySetup->AggGeom.ConvexElems.Num());
		}
		Obj->SetStringField(TEXT("edit_policy"), TEXT("simple_collision_safe; convex_hulls_summary_only"));
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildSocketsFolderJson(UStaticMesh* Mesh)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Sockets;
		if (Mesh)
		{
			for (const TObjectPtr<UStaticMeshSocket>& Socket : Mesh->Sockets)
			{
				if (Socket)
				{
					Sockets.Add(MakeShared<FJsonValueObject>(SocketToJson(Socket)));
				}
			}
		}
		Obj->SetArrayField(TEXT("sockets"), Sockets);
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildLightmapUvJson(UStaticMesh* Mesh)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Mesh)
		{
			return Obj;
		}
		Obj->SetNumberField(TEXT("lightmap_resolution"), Mesh->GetLightMapResolution());
		Obj->SetNumberField(TEXT("lightmap_coordinate_index"), Mesh->GetLightMapCoordinateIndex());
		TArray<TSharedPtr<FJsonValue>> Channels;
		const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
		if (RenderData && RenderData->LODResources.Num() > 0)
		{
			const int32 NumChannels = RenderData->LODResources[0].GetNumTexCoords();
			Obj->SetNumberField(TEXT("uv_channel_count"), NumChannels);
			for (int32 ChannelIndex = 0; ChannelIndex < NumChannels; ++ChannelIndex)
			{
				TSharedPtr<FJsonObject> Channel = MakeShared<FJsonObject>();
				Channel->SetNumberField(TEXT("channel_index"), ChannelIndex);
				Channel->SetStringField(TEXT("raw_uv_policy"), TEXT("summary_only"));
				Channels.Add(MakeShared<FJsonValueObject>(Channel));
			}
		}
		Obj->SetArrayField(TEXT("uv_channels"), Channels);
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildRawMeshSummaryJson(UStaticMesh* Mesh)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Lods;
		if (Mesh)
		{
			const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
			for (int32 LodIndex = 0; RenderData && LodIndex < RenderData->LODResources.Num(); ++LodIndex)
			{
				const FStaticMeshLODResources& LOD = RenderData->LODResources[LodIndex];
				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("lod_index"), LodIndex);
				Item->SetNumberField(TEXT("vertex_count"), LOD.GetNumVertices());
				Item->SetNumberField(TEXT("index_count"), LOD.IndexBuffer.GetNumIndices());
				Item->SetNumberField(TEXT("triangle_count"), LOD.GetNumTriangles());
				Item->SetNumberField(TEXT("uv_channel_count"), LOD.GetNumTexCoords());
				Item->SetStringField(TEXT("edit_policy"), TEXT("readonly_summary"));
				Lods.Add(MakeShared<FJsonValueObject>(Item));
			}
		}
		Obj->SetArrayField(TEXT("lods"), Lods);
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildBuildSettingsFolderJson(UStaticMesh* Mesh)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Lods;
		if (Mesh)
		{
			for (int32 LodIndex = 0; LodIndex < Mesh->GetNumSourceModels(); ++LodIndex)
			{
				const FStaticMeshSourceModel& SourceModel = Mesh->GetSourceModel(LodIndex);
				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("lod_index"), LodIndex);
				Item->SetObjectField(TEXT("build_settings"), BuildBuildSettingsJson(SourceModel.BuildSettings));
				Lods.Add(MakeShared<FJsonValueObject>(Item));
			}
		}
		Obj->SetArrayField(TEXT("lods"), Lods);
		Obj->SetStringField(TEXT("edit_policy"), TEXT("safe_settings"));
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

	static TSharedPtr<FJsonObject> BuildImportDataJson(UStaticMesh* Mesh)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("asset_path"), Mesh ? Mesh->GetPathName() : TEXT(""));
		Obj->SetStringField(TEXT("policy"), TEXT("source file tracking and reimport validation only; raw geometry rebuilds must use import/modeling commands"));

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

	static TSharedPtr<FJsonObject> BuildCoverageJson()
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("profile"), TEXT("static_mesh"));
		Obj->SetStringField(TEXT("implementation_status"), TEXT("folder_profile_with_safe_settings"));
		Obj->SetBoolField(TEXT("is_complete_target_schema"), true);
		Obj->SetNumberField(TEXT("structured_field_count"), 10);
		Obj->SetNumberField(TEXT("raw_property_count"), 0);
		Obj->SetNumberField(TEXT("readonly_property_count"), 4);
		Obj->SetArrayField(TEXT("raw_geometry_policy"), { MakeShared<FJsonValueString>(TEXT("raw vertex/index/uv buffers are summary-only; rebuild via import/modeling commands")) });
		Obj->SetArrayField(TEXT("blocking_gaps"), {});
		return Obj;
	}
}

bool FUeAgentHttpServer::ExecuteStaticMeshCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (CommandLower == TEXT("static_mesh_open_editor")) return CmdStaticMeshOpenEditor(Ctx, OutData, OutError);
	if (CommandLower == TEXT("static_mesh_get_info")) return CmdStaticMeshGetInfo(Ctx, OutData, OutError);
	if (CommandLower == TEXT("static_mesh_get_bounds")) return CmdStaticMeshGetBounds(Ctx, OutData, OutError);
	if (CommandLower == TEXT("static_mesh_get_local_corners")) return CmdStaticMeshGetLocalCorners(Ctx, OutData, OutError);
	if (CommandLower == TEXT("static_mesh_set_preview_view")) return CmdStaticMeshSetPreviewView(Ctx, OutData, OutError);
	if (CommandLower == TEXT("static_mesh_set_property")) return CmdStaticMeshSetProperty(Ctx, OutData, OutError);
	if (CommandLower == TEXT("static_mesh_set_material_slot")) return CmdStaticMeshSetMaterialSlot(Ctx, OutData, OutError);
	if (CommandLower == TEXT("static_mesh_set_collision_boxes")) return CmdStaticMeshSetCollisionBoxes(Ctx, OutData, OutError);
	if (CommandLower == TEXT("static_mesh_set_collision_spheres")) return CmdStaticMeshSetCollisionSpheres(Ctx, OutData, OutError);
	if (CommandLower == TEXT("static_mesh_set_collision_capsules")) return CmdStaticMeshSetCollisionCapsules(Ctx, OutData, OutError);
	if (CommandLower == TEXT("static_mesh_add_socket")) return CmdStaticMeshAddSocket(Ctx, OutData, OutError);
	if (CommandLower == TEXT("static_mesh_update_socket")) return CmdStaticMeshUpdateSocket(Ctx, OutData, OutError);
	if (CommandLower == TEXT("static_mesh_remove_socket")) return CmdStaticMeshRemoveSocket(Ctx, OutData, OutError);
	if (CommandLower == TEXT("static_mesh_export_folder")) return CmdStaticMeshExportFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("static_mesh_apply_folder")) return CmdStaticMeshApplyFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("static_mesh_validate_folder")) return CmdStaticMeshValidateFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("static_mesh_validate_geometry")) return CmdStaticMeshValidateGeometry(Ctx, OutData, OutError);
	if (CommandLower == TEXT("static_mesh_validate_uvs")) return CmdStaticMeshValidateUvs(Ctx, OutData, OutError);
	if (CommandLower == TEXT("static_mesh_reimport")) return CmdStaticMeshReimport(Ctx, OutData, OutError);
	if (CommandLower == TEXT("static_mesh_build")) return CmdStaticMeshBuild(Ctx, OutData, OutError);
	if (CommandLower == TEXT("static_mesh_preview_collision")) return CmdStaticMeshPreviewCollision(Ctx, OutData, OutError);

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("command"), CommandLower);
	OutData->SetStringField(TEXT("category"), TEXT("static_mesh"));
	OutError = TEXT("unknown_static_mesh_command");
	return false;
}

bool FUeAgentHttpServer::CmdStaticMeshOpenEditor(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UStaticMesh* Mesh = UeAgentStaticMeshOps::LoadStaticMeshAsset(AssetPathInput);
	if (!Mesh)
	{
		OutError = TEXT("static_mesh_not_found");
		return false;
	}

	if (!GEditor)
	{
		OutError = TEXT("missing_editor");
		return false;
	}
	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!AssetEditorSubsystem)
	{
		OutError = TEXT("missing_asset_editor_subsystem");
		return false;
	}

	AssetEditorSubsystem->OpenEditorForAsset(Mesh);
	OutData->SetStringField(TEXT("asset_path"), Mesh->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), Mesh->GetPathName());
	OutData->SetStringField(TEXT("class"), Mesh->GetClass()->GetPathName());
	return true;
}

bool FUeAgentHttpServer::CmdStaticMeshGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UStaticMesh* Mesh = UeAgentStaticMeshOps::LoadStaticMeshAsset(AssetPathInput);
	if (!Mesh)
	{
		OutError = TEXT("static_mesh_not_found");
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Mesh->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), Mesh->GetPathName());
	OutData->SetBoolField(TEXT("dirty"), Mesh->GetOutermost()->IsDirty());

	const FBoxSphereBounds Bounds = Mesh->GetBounds();
	UeAgentStaticMeshOps::FillBoundsFields(Bounds, OutData);

	const FAssetEditorOrbitCameraPosition& EditorCamera = Mesh->EditorCameraPosition;
	TSharedPtr<FJsonObject> CameraObj = MakeShared<FJsonObject>();
	CameraObj->SetBoolField(TEXT("is_set"), EditorCamera.bIsSet);
	CameraObj->SetObjectField(TEXT("orbit_point"), VecToJson(EditorCamera.CamOrbitPoint));
	CameraObj->SetObjectField(TEXT("orbit_zoom"), VecToJson(EditorCamera.CamOrbitZoom));
	CameraObj->SetObjectField(TEXT("orbit_rotation"), RotToJson(EditorCamera.CamOrbitRotation));
	OutData->SetObjectField(TEXT("editor_camera"), CameraObj);

	TArray<TSharedPtr<FJsonValue>> SocketArr;
	for (const TObjectPtr<UStaticMeshSocket>& Socket : Mesh->Sockets)
	{
		if (!Socket)
		{
			continue;
		}
		SocketArr.Add(MakeShared<FJsonValueObject>(UeAgentStaticMeshOps::SocketToJson(Socket)));
	}
	OutData->SetArrayField(TEXT("sockets"), SocketArr);
	OutData->SetNumberField(TEXT("socket_count"), SocketArr.Num());

	TArray<TSharedPtr<FJsonValue>> MaterialSlotArr;
	for (int32 SlotIndex = 0; SlotIndex < Mesh->GetStaticMaterials().Num(); ++SlotIndex)
	{
		MaterialSlotArr.Add(MakeShared<FJsonValueObject>(UeAgentStaticMeshOps::MaterialSlotToJson(Mesh->GetStaticMaterials()[SlotIndex], SlotIndex)));
	}
	OutData->SetArrayField(TEXT("material_slots"), MaterialSlotArr);
	OutData->SetNumberField(TEXT("material_slot_count"), MaterialSlotArr.Num());

	TSharedPtr<FJsonObject> CollisionObj = MakeShared<FJsonObject>();
	if (UBodySetup* BodySetup = Mesh->GetBodySetup())
	{
		CollisionObj->SetNumberField(TEXT("trace_flag"), (int32)BodySetup->CollisionTraceFlag);
		CollisionObj->SetNumberField(TEXT("box_count"), BodySetup->AggGeom.BoxElems.Num());
		CollisionObj->SetNumberField(TEXT("sphere_count"), BodySetup->AggGeom.SphereElems.Num());
		CollisionObj->SetNumberField(TEXT("capsule_count"), BodySetup->AggGeom.SphylElems.Num());
		CollisionObj->SetNumberField(TEXT("convex_count"), BodySetup->AggGeom.ConvexElems.Num());

		TArray<TSharedPtr<FJsonValue>> Boxes;
		for (const FKBoxElem& BoxElem : BodySetup->AggGeom.BoxElems)
		{
			TSharedPtr<FJsonObject> BoxObj = MakeShared<FJsonObject>();
			BoxObj->SetObjectField(TEXT("center"), VecToJson(BoxElem.Center));
			BoxObj->SetObjectField(TEXT("rotation"), RotToJson(BoxElem.Rotation));
			BoxObj->SetNumberField(TEXT("x"), BoxElem.X);
			BoxObj->SetNumberField(TEXT("y"), BoxElem.Y);
			BoxObj->SetNumberField(TEXT("z"), BoxElem.Z);
			Boxes.Add(MakeShared<FJsonValueObject>(BoxObj));
		}
		CollisionObj->SetArrayField(TEXT("boxes"), Boxes);

		TArray<TSharedPtr<FJsonValue>> Spheres;
		for (const FKSphereElem& SphereElem : BodySetup->AggGeom.SphereElems)
		{
			Spheres.Add(MakeShared<FJsonValueObject>(UeAgentStaticMeshOps::SphereElemToJson(SphereElem)));
		}
		CollisionObj->SetArrayField(TEXT("spheres"), Spheres);

		TArray<TSharedPtr<FJsonValue>> Capsules;
		for (const FKSphylElem& CapsuleElem : BodySetup->AggGeom.SphylElems)
		{
			Capsules.Add(MakeShared<FJsonValueObject>(UeAgentStaticMeshOps::CapsuleElemToJson(CapsuleElem)));
		}
		CollisionObj->SetArrayField(TEXT("capsules"), Capsules);
	}
	else
	{
		CollisionObj->SetNumberField(TEXT("trace_flag"), -1);
		CollisionObj->SetNumberField(TEXT("box_count"), 0);
		CollisionObj->SetNumberField(TEXT("sphere_count"), 0);
		CollisionObj->SetNumberField(TEXT("capsule_count"), 0);
		CollisionObj->SetNumberField(TEXT("convex_count"), 0);
		CollisionObj->SetArrayField(TEXT("boxes"), TArray<TSharedPtr<FJsonValue>>());
		CollisionObj->SetArrayField(TEXT("spheres"), TArray<TSharedPtr<FJsonValue>>());
		CollisionObj->SetArrayField(TEXT("capsules"), TArray<TSharedPtr<FJsonValue>>());
	}
	OutData->SetObjectField(TEXT("collision"), CollisionObj);

	return true;
}

bool FUeAgentHttpServer::CmdStaticMeshGetBounds(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UStaticMesh* Mesh = UeAgentStaticMeshOps::LoadStaticMeshAsset(AssetPathInput);
	if (!Mesh)
	{
		OutError = TEXT("static_mesh_not_found");
		return false;
	}

	const FBoxSphereBounds Bounds = Mesh->GetBounds();
	OutData->SetStringField(TEXT("asset_path"), Mesh->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), Mesh->GetPathName());
	UeAgentStaticMeshOps::FillBoundsFields(Bounds, OutData);
	return true;
}

bool FUeAgentHttpServer::CmdStaticMeshGetLocalCorners(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UStaticMesh* Mesh = UeAgentStaticMeshOps::LoadStaticMeshAsset(AssetPathInput);
	if (!Mesh)
	{
		OutError = TEXT("static_mesh_not_found");
		return false;
	}

	const FBoxSphereBounds Bounds = Mesh->GetBounds();
	OutData->SetStringField(TEXT("asset_path"), Mesh->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), Mesh->GetPathName());
	UeAgentStaticMeshOps::FillBoundsFields(Bounds, OutData);
	UeAgentStaticMeshOps::FillLocalCornersFields(Bounds, OutData);
	return true;
}

bool FUeAgentHttpServer::CmdStaticMeshSetPreviewView(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UStaticMesh* Mesh = UeAgentStaticMeshOps::LoadStaticMeshAsset(AssetPathInput);
	if (!Mesh)
	{
		OutError = TEXT("static_mesh_not_found");
		return false;
	}

	bool bOpenEditorIfNeeded = true;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor_if_needed"), bOpenEditorIfNeeded);

	IStaticMeshEditor* MeshEditor = UeAgentStaticMeshOps::GetStaticMeshEditor(Mesh, bOpenEditorIfNeeded);
	if (!MeshEditor)
	{
		OutError = TEXT("static_mesh_editor_not_opened");
		return false;
	}

	const FBoxSphereBounds Bounds = Mesh->GetBounds();
	FVector Target = Bounds.Origin;
	JsonTryGetVector(Ctx.Params, TEXT("target"), Target);

	double Yaw = 35.0;
	double Pitch = -20.0;
	double Roll = 0.0;
	double Distance = FMath::Max<double>(Bounds.SphereRadius * 2.5, 50.0);
	double Fov = 60.0;
	const bool bHasFov = JsonTryGetNumber(Ctx.Params, TEXT("fov"), Fov);

	JsonTryGetNumber(Ctx.Params, TEXT("yaw"), Yaw);
	JsonTryGetNumber(Ctx.Params, TEXT("pitch"), Pitch);
	JsonTryGetNumber(Ctx.Params, TEXT("roll"), Roll);
	JsonTryGetNumber(Ctx.Params, TEXT("distance"), Distance);

	const FRotator ViewRotation((float)Pitch, (float)Yaw, (float)Roll);
	const FVector ViewLocation = Target - ViewRotation.Vector() * (float)Distance;

	FEditorViewportClient& ViewportClient = MeshEditor->GetViewportClient();
	ViewportClient.SetLookAtLocation(Target, false);
	ViewportClient.SetViewLocation(ViewLocation);
	ViewportClient.SetViewRotation(ViewRotation);
	if (bHasFov)
	{
		ViewportClient.ViewFOV = (float)FMath::Clamp(Fov, 5.0, 170.0);
	}
	ViewportClient.Invalidate();
	MeshEditor->RefreshViewport();

	bool bPersistToAsset = false;
	JsonTryGetBool(Ctx.Params, TEXT("persist_to_asset"), bPersistToAsset);
	if (bPersistToAsset)
	{
		Mesh->Modify();
		Mesh->EditorCameraPosition = FAssetEditorOrbitCameraPosition(Target, ViewLocation - Target, ViewRotation);
		UeAgentStaticMeshOps::ApplyMeshPostEdit(Mesh, false);
	}

	OutData->SetStringField(TEXT("asset_path"), Mesh->GetOutermost()->GetName());
	OutData->SetObjectField(TEXT("target"), VecToJson(Target));
	OutData->SetObjectField(TEXT("location"), VecToJson(ViewLocation));
	OutData->SetObjectField(TEXT("rotation"), RotToJson(ViewRotation));
	OutData->SetNumberField(TEXT("fov"), ViewportClient.ViewFOV);
	OutData->SetBoolField(TEXT("persist_to_asset"), bPersistToAsset);
	return true;
}

bool FUeAgentHttpServer::CmdStaticMeshSetProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
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

	UStaticMesh* Mesh = UeAgentStaticMeshOps::LoadStaticMeshAsset(AssetPathInput);
	if (!Mesh)
	{
		OutError = TEXT("static_mesh_not_found");
		return false;
	}

	FProperty* Property = nullptr;
	void* ValuePtr = nullptr;
	if (!UeAgentStaticMeshOps::ResolvePropertyPath(Mesh, PropertyName, Property, ValuePtr) || !Property || !ValuePtr)
	{
		OutError = TEXT("property_not_found");
		return false;
	}

	Mesh->Modify();
	if (!Property->ImportText_Direct(*ValueText, ValuePtr, Mesh, PPF_None))
	{
		const FString ImportError = FString::Printf(TEXT("property_import_failed:%s:%s"), *PropertyName, *ValueText);
		OutData->SetStringField(TEXT("property_name"), PropertyName);
		OutData->SetStringField(TEXT("value_text"), ValueText);
		SetPropertyImportResultFields(OutData, Property, ValueText, TEXT(""), TEXT("import_failed"), ImportError);
		OutError = ImportError;
		return false;
	}

	UeAgentStaticMeshOps::ApplyMeshPostEdit(Mesh, false);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentStaticMeshOps::SaveMeshPackage(Mesh, OutError))
	{
		return false;
	}

	FString ExportedValue;
	Property->ExportTextItem_Direct(ExportedValue, ValuePtr, nullptr, Mesh, PPF_None);
	OutData->SetStringField(TEXT("asset_path"), Mesh->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("property_name"), PropertyName);
	OutData->SetStringField(TEXT("value_text"), ValueText);
	OutData->SetStringField(TEXT("applied_value_text"), ExportedValue);
	SetPropertyImportResultFields(OutData, Property, ValueText, ExportedValue, TEXT("imported_and_read_back"));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdStaticMeshSetMaterialSlot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString MaterialPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("material_path"), MaterialPath) || MaterialPath.IsEmpty())
	{
		OutError = TEXT("missing_material_path");
		return false;
	}

	UStaticMesh* Mesh = UeAgentStaticMeshOps::LoadStaticMeshAsset(AssetPathInput);
	if (!Mesh)
	{
		OutError = TEXT("static_mesh_not_found");
		return false;
	}

	UMaterialInterface* Material = UeAgentStaticMeshOps::LoadMaterialAsset(MaterialPath);
	if (!Material)
	{
		OutError = TEXT("material_not_found");
		return false;
	}

	const int32 SlotIndex = UeAgentStaticMeshOps::ResolveMaterialSlotIndex(Mesh, Ctx.Params, OutError);
	if (SlotIndex == INDEX_NONE)
	{
		return false;
	}

	Mesh->Modify();
	Mesh->SetMaterial(SlotIndex, Material);
	UeAgentStaticMeshOps::ApplyMeshPostEdit(Mesh, false);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentStaticMeshOps::SaveMeshPackage(Mesh, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Mesh->GetOutermost()->GetName());
	OutData->SetObjectField(TEXT("material_slot"), UeAgentStaticMeshOps::MaterialSlotToJson(Mesh->GetStaticMaterials()[SlotIndex], SlotIndex));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdStaticMeshSetCollisionBoxes(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* BoxValues = nullptr;
	if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetArrayField(TEXT("boxes"), BoxValues) || !BoxValues)
	{
		OutError = TEXT("missing_boxes");
		return false;
	}

	UStaticMesh* Mesh = UeAgentStaticMeshOps::LoadStaticMeshAsset(AssetPathInput);
	if (!Mesh)
	{
		OutError = TEXT("static_mesh_not_found");
		return false;
	}

	Mesh->CreateBodySetup();
	UBodySetup* BodySetup = Mesh->GetBodySetup();
	if (!BodySetup)
	{
		OutError = TEXT("missing_body_setup");
		return false;
	}

	bool bClearOtherShapes = false;
	JsonTryGetBool(Ctx.Params, TEXT("clear_other_shapes"), bClearOtherShapes);

	Mesh->Modify();
	BodySetup->Modify();
	BodySetup->AggGeom.BoxElems.Reset();
	if (bClearOtherShapes)
	{
		BodySetup->AggGeom.SphereElems.Reset();
		BodySetup->AggGeom.SphylElems.Reset();
		BodySetup->AggGeom.ConvexElems.Reset();
	}

	for (const TSharedPtr<FJsonValue>& BoxValue : *BoxValues)
	{
		const TSharedPtr<FJsonObject> BoxObj = BoxValue.IsValid() ? BoxValue->AsObject() : nullptr;
		if (!BoxObj.IsValid())
		{
			OutError = TEXT("invalid_box_item");
			return false;
		}

		FKBoxElem BoxElem;
		JsonTryGetVector(BoxObj, TEXT("center"), BoxElem.Center);
		JsonTryGetRotator(BoxObj, TEXT("rotation"), BoxElem.Rotation);

		double X = BoxElem.X;
		double Y = BoxElem.Y;
		double Z = BoxElem.Z;
		if (JsonTryGetNumber(BoxObj, TEXT("x"), X) &&
			JsonTryGetNumber(BoxObj, TEXT("y"), Y) &&
			JsonTryGetNumber(BoxObj, TEXT("z"), Z))
		{
			BoxElem.X = (float)FMath::Max(1.0, X);
			BoxElem.Y = (float)FMath::Max(1.0, Y);
			BoxElem.Z = (float)FMath::Max(1.0, Z);
		}
		else
		{
			FVector Size(BoxElem.X, BoxElem.Y, BoxElem.Z);
			if (JsonTryGetVector(BoxObj, TEXT("size"), Size))
			{
				BoxElem.X = (float)FMath::Max(1.0, (double)Size.X);
				BoxElem.Y = (float)FMath::Max(1.0, (double)Size.Y);
				BoxElem.Z = (float)FMath::Max(1.0, (double)Size.Z);
			}
		}

		BodySetup->AggGeom.BoxElems.Add(BoxElem);
	}

	UeAgentStaticMeshOps::ApplyMeshPostEdit(Mesh, true);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentStaticMeshOps::SaveMeshPackage(Mesh, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Mesh->GetOutermost()->GetName());
	OutData->SetNumberField(TEXT("box_count"), BodySetup->AggGeom.BoxElems.Num());
	OutData->SetBoolField(TEXT("clear_other_shapes"), bClearOtherShapes);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdStaticMeshSetCollisionSpheres(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* SphereValues = nullptr;
	if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetArrayField(TEXT("spheres"), SphereValues) || !SphereValues)
	{
		OutError = TEXT("missing_spheres");
		return false;
	}

	UStaticMesh* Mesh = UeAgentStaticMeshOps::LoadStaticMeshAsset(AssetPathInput);
	if (!Mesh)
	{
		OutError = TEXT("static_mesh_not_found");
		return false;
	}

	Mesh->CreateBodySetup();
	UBodySetup* BodySetup = Mesh->GetBodySetup();
	if (!BodySetup)
	{
		OutError = TEXT("missing_body_setup");
		return false;
	}

	bool bClearOtherShapes = false;
	JsonTryGetBool(Ctx.Params, TEXT("clear_other_shapes"), bClearOtherShapes);

	Mesh->Modify();
	BodySetup->Modify();
	BodySetup->AggGeom.SphereElems.Reset();
	if (bClearOtherShapes)
	{
		BodySetup->AggGeom.BoxElems.Reset();
		BodySetup->AggGeom.SphylElems.Reset();
		BodySetup->AggGeom.ConvexElems.Reset();
	}

	for (const TSharedPtr<FJsonValue>& SphereValue : *SphereValues)
	{
		const TSharedPtr<FJsonObject> SphereObj = SphereValue.IsValid() ? SphereValue->AsObject() : nullptr;
		if (!SphereObj.IsValid())
		{
			OutError = TEXT("invalid_sphere_item");
			return false;
		}

		FKSphereElem SphereElem;
		JsonTryGetVector(SphereObj, TEXT("center"), SphereElem.Center);

		double Radius = SphereElem.Radius;
		if (!JsonTryGetNumber(SphereObj, TEXT("radius"), Radius))
		{
			OutError = TEXT("missing_sphere_radius");
			return false;
		}

		SphereElem.Radius = static_cast<float>(FMath::Max(1.0, Radius));
		BodySetup->AggGeom.SphereElems.Add(SphereElem);
	}

	UeAgentStaticMeshOps::ApplyMeshPostEdit(Mesh, true);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentStaticMeshOps::SaveMeshPackage(Mesh, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Mesh->GetOutermost()->GetName());
	OutData->SetNumberField(TEXT("sphere_count"), BodySetup->AggGeom.SphereElems.Num());
	OutData->SetBoolField(TEXT("clear_other_shapes"), bClearOtherShapes);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdStaticMeshSetCollisionCapsules(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* CapsuleValues = nullptr;
	if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetArrayField(TEXT("capsules"), CapsuleValues) || !CapsuleValues)
	{
		OutError = TEXT("missing_capsules");
		return false;
	}

	UStaticMesh* Mesh = UeAgentStaticMeshOps::LoadStaticMeshAsset(AssetPathInput);
	if (!Mesh)
	{
		OutError = TEXT("static_mesh_not_found");
		return false;
	}

	Mesh->CreateBodySetup();
	UBodySetup* BodySetup = Mesh->GetBodySetup();
	if (!BodySetup)
	{
		OutError = TEXT("missing_body_setup");
		return false;
	}

	bool bClearOtherShapes = false;
	JsonTryGetBool(Ctx.Params, TEXT("clear_other_shapes"), bClearOtherShapes);

	Mesh->Modify();
	BodySetup->Modify();
	BodySetup->AggGeom.SphylElems.Reset();
	if (bClearOtherShapes)
	{
		BodySetup->AggGeom.BoxElems.Reset();
		BodySetup->AggGeom.SphereElems.Reset();
		BodySetup->AggGeom.ConvexElems.Reset();
	}

	for (const TSharedPtr<FJsonValue>& CapsuleValue : *CapsuleValues)
	{
		const TSharedPtr<FJsonObject> CapsuleObj = CapsuleValue.IsValid() ? CapsuleValue->AsObject() : nullptr;
		if (!CapsuleObj.IsValid())
		{
			OutError = TEXT("invalid_capsule_item");
			return false;
		}

		FKSphylElem CapsuleElem;
		JsonTryGetVector(CapsuleObj, TEXT("center"), CapsuleElem.Center);
		JsonTryGetRotator(CapsuleObj, TEXT("rotation"), CapsuleElem.Rotation);

		double Radius = CapsuleElem.Radius;
		if (!JsonTryGetNumber(CapsuleObj, TEXT("radius"), Radius))
		{
			OutError = TEXT("missing_capsule_radius");
			return false;
		}

		double Length = CapsuleElem.Length;
		if (!JsonTryGetNumber(CapsuleObj, TEXT("length"), Length))
		{
			OutError = TEXT("missing_capsule_length");
			return false;
		}

		CapsuleElem.Radius = static_cast<float>(FMath::Max(1.0, Radius));
		CapsuleElem.Length = static_cast<float>(FMath::Max(1.0, Length));
		BodySetup->AggGeom.SphylElems.Add(CapsuleElem);
	}

	UeAgentStaticMeshOps::ApplyMeshPostEdit(Mesh, true);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentStaticMeshOps::SaveMeshPackage(Mesh, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Mesh->GetOutermost()->GetName());
	OutData->SetNumberField(TEXT("capsule_count"), BodySetup->AggGeom.SphylElems.Num());
	OutData->SetBoolField(TEXT("clear_other_shapes"), bClearOtherShapes);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdStaticMeshAddSocket(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	FString SocketName;
	if (!JsonTryGetString(Ctx.Params, TEXT("socket_name"), SocketName) || SocketName.IsEmpty())
	{
		OutError = TEXT("missing_socket_name");
		return false;
	}

	UStaticMesh* Mesh = UeAgentStaticMeshOps::LoadStaticMeshAsset(AssetPathInput);
	if (!Mesh)
	{
		OutError = TEXT("static_mesh_not_found");
		return false;
	}

	const FName SocketFName(*SocketName);
	if (Mesh->FindSocket(SocketFName))
	{
		OutError = TEXT("socket_already_exists");
		return false;
	}

	Mesh->Modify();
	UStaticMeshSocket* Socket = NewObject<UStaticMeshSocket>(Mesh, NAME_None, RF_Transactional);
	if (!Socket)
	{
		OutError = TEXT("create_socket_failed");
		return false;
	}

	Socket->SocketName = SocketFName;
	JsonTryGetVector(Ctx.Params, TEXT("location"), Socket->RelativeLocation);
	JsonTryGetRotator(Ctx.Params, TEXT("rotation"), Socket->RelativeRotation);
	JsonTryGetVector(Ctx.Params, TEXT("scale"), Socket->RelativeScale);
	JsonTryGetString(Ctx.Params, TEXT("tag"), Socket->Tag);

	Mesh->AddSocket(Socket);
	UeAgentStaticMeshOps::ApplyMeshPostEdit(Mesh, false);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentStaticMeshOps::SaveMeshPackage(Mesh, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Mesh->GetOutermost()->GetName());
	OutData->SetObjectField(TEXT("socket"), UeAgentStaticMeshOps::SocketToJson(Socket));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdStaticMeshUpdateSocket(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	FString SocketName;
	if (!JsonTryGetString(Ctx.Params, TEXT("socket_name"), SocketName) || SocketName.IsEmpty())
	{
		OutError = TEXT("missing_socket_name");
		return false;
	}

	UStaticMesh* Mesh = UeAgentStaticMeshOps::LoadStaticMeshAsset(AssetPathInput);
	if (!Mesh)
	{
		OutError = TEXT("static_mesh_not_found");
		return false;
	}

	UStaticMeshSocket* Socket = Mesh->FindSocket(FName(*SocketName));
	if (!Socket)
	{
		OutError = TEXT("socket_not_found");
		return false;
	}

	Mesh->Modify();
	Socket->Modify();

	FString NewSocketName;
	if (JsonTryGetString(Ctx.Params, TEXT("new_socket_name"), NewSocketName) && !NewSocketName.IsEmpty() &&
		!NewSocketName.Equals(SocketName, ESearchCase::IgnoreCase))
	{
		if (Mesh->FindSocket(FName(*NewSocketName)))
		{
			OutError = TEXT("new_socket_name_already_exists");
			return false;
		}
		Socket->SocketName = FName(*NewSocketName);
	}

	JsonTryGetVector(Ctx.Params, TEXT("location"), Socket->RelativeLocation);
	JsonTryGetRotator(Ctx.Params, TEXT("rotation"), Socket->RelativeRotation);
	JsonTryGetVector(Ctx.Params, TEXT("scale"), Socket->RelativeScale);
	JsonTryGetString(Ctx.Params, TEXT("tag"), Socket->Tag);

	UeAgentStaticMeshOps::ApplyMeshPostEdit(Mesh, false);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentStaticMeshOps::SaveMeshPackage(Mesh, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Mesh->GetOutermost()->GetName());
	OutData->SetObjectField(TEXT("socket"), UeAgentStaticMeshOps::SocketToJson(Socket));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdStaticMeshRemoveSocket(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	FString SocketName;
	if (!JsonTryGetString(Ctx.Params, TEXT("socket_name"), SocketName) || SocketName.IsEmpty())
	{
		OutError = TEXT("missing_socket_name");
		return false;
	}

	UStaticMesh* Mesh = UeAgentStaticMeshOps::LoadStaticMeshAsset(AssetPathInput);
	if (!Mesh)
	{
		OutError = TEXT("static_mesh_not_found");
		return false;
	}

	UStaticMeshSocket* Socket = Mesh->FindSocket(FName(*SocketName));
	if (!Socket)
	{
		OutError = TEXT("socket_not_found");
		return false;
	}

	Mesh->Modify();
	Mesh->RemoveSocket(Socket);
	UeAgentStaticMeshOps::ApplyMeshPostEdit(Mesh, false);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentStaticMeshOps::SaveMeshPackage(Mesh, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Mesh->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("socket_name"), SocketName);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdStaticMeshExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UStaticMesh* Mesh = UeAgentStaticMeshOps::LoadStaticMeshAsset(AssetPathInput);
	if (!Mesh)
	{
		OutError = TEXT("static_mesh_not_found");
		return false;
	}

	FString FolderPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath) || FolderPath.TrimStartAndEnd().IsEmpty())
	{
		if (!UeAgentStaticMeshOps::ResolveFolderForAsset(AssetPathInput, FolderPath, OutError))
		{
			return false;
		}
	}
	FolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderPath);

	int32 FileCount = 0;
	auto SaveRequiredJson = [&FileCount, &OutError](const FString& FilePath, const TSharedPtr<FJsonObject>& JsonObj) -> bool
	{
		if (!UeAgentStaticMeshOps::SaveJsonFile(FilePath, JsonObj))
		{
			OutError = FString::Printf(TEXT("save_json_failed:%s"), *FilePath);
			return false;
		}
		++FileCount;
		return true;
	};

	const FString ValidationDir = FPaths::Combine(FolderPath, TEXT("validation"));
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("asset.json")), UeAgentStaticMeshOps::BuildAssetJson(Mesh))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("mesh.json")), UeAgentStaticMeshOps::BuildMeshJson(Mesh))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("materials.json")), UeAgentStaticMeshOps::BuildMaterialsFolderJson(Mesh))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("lods"), TEXT("index.json")), UeAgentStaticMeshOps::BuildLodsIndexJson(Mesh))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("sections.json")), UeAgentStaticMeshOps::BuildSectionsJson(Mesh))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("collision.json")), UeAgentStaticMeshOps::BuildCollisionJson(Mesh))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("sockets.json")), UeAgentStaticMeshOps::BuildSocketsFolderJson(Mesh))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("lightmap_uv.json")), UeAgentStaticMeshOps::BuildLightmapUvJson(Mesh))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("nanite.json")), UeAgentStaticMeshOps::BuildNaniteJson(Mesh))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("build_settings.json")), UeAgentStaticMeshOps::BuildBuildSettingsFolderJson(Mesh))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("import_data.json")), UeAgentStaticMeshOps::BuildImportDataJson(Mesh))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("raw_mesh_summary.json")), UeAgentStaticMeshOps::BuildRawMeshSummaryJson(Mesh))) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("raw_properties.json")), MakeShared<FJsonObject>())) return false;
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("readonly_properties.json")), MakeShared<FJsonObject>())) return false;
	if (!SaveRequiredJson(FPaths::Combine(ValidationDir, TEXT("coverage_report.json")), UeAgentStaticMeshOps::BuildCoverageJson())) return false;
	if (!SaveRequiredJson(FPaths::Combine(ValidationDir, TEXT("mesh_integrity_report.json")), UeAgentStaticMeshOps::BuildRawMeshSummaryJson(Mesh))) return false;
	if (!SaveRequiredJson(FPaths::Combine(ValidationDir, TEXT("collision_report.json")), UeAgentStaticMeshOps::BuildCollisionJson(Mesh))) return false;
	if (!SaveRequiredJson(FPaths::Combine(ValidationDir, TEXT("uv_report.json")), UeAgentStaticMeshOps::BuildLightmapUvJson(Mesh))) return false;
	if (!SaveRequiredJson(FPaths::Combine(ValidationDir, TEXT("nanite_report.json")), UeAgentStaticMeshOps::BuildNaniteJson(Mesh))) return false;
	if (!SaveRequiredJson(FPaths::Combine(ValidationDir, TEXT("lightmap_report.json")), UeAgentStaticMeshOps::BuildLightmapUvJson(Mesh))) return false;
	if (!SaveRequiredJson(FPaths::Combine(ValidationDir, TEXT("readback_diff.json")), UeAgentStaticMeshOps::BuildReadbackDiffEmptyJson())) return false;
	if (!SaveRequiredJson(FPaths::Combine(ValidationDir, TEXT("diagnostics.json")), UeAgentStaticMeshOps::BuildCoverageJson())) return false;

	OutData->SetStringField(TEXT("asset_path"), UeAgentStaticMeshOps::NormalizeAssetPath(Mesh->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetNumberField(TEXT("file_count"), FileCount);
	OutData->SetStringField(TEXT("implementation_status"), TEXT("folder_profile_with_safe_settings"));
	return true;
}

bool FUeAgentHttpServer::CmdStaticMeshApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
	TArray<TSharedPtr<FJsonValue>> PropertyResults;
	auto AddPropertyResult = [&PropertyResults](const FString& JsonPath, const FString& Status, const FString& Message)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("json_path"), JsonPath);
		Item->SetStringField(TEXT("status"), Status);
		Item->SetStringField(TEXT("message"), Message);
		PropertyResults.Add(MakeShared<FJsonValueObject>(Item));
	};

	TSharedPtr<FJsonObject> AssetJson;
	if (!UeAgentStaticMeshOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetJson, Issues, OutError, false))
	{
		OutData->SetArrayField(TEXT("json_issues"), Issues);
		OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
		return false;
	}
	UeAgentJsonDiagnostics::WarnUnknownFields(AssetJson, TEXT("asset.json"), { TEXT("schema"), TEXT("profile"), TEXT("schema_version"), TEXT("asset_path"), TEXT("object_path"), TEXT("asset_class"), TEXT("engine_version"), TEXT("exported_at") }, Issues);

	FString AssetPath;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
	if (AssetPath.TrimStartAndEnd().IsEmpty())
	{
		AssetJson->TryGetStringField(TEXT("asset_path"), AssetPath);
	}

	UStaticMesh* Mesh = UeAgentStaticMeshOps::LoadStaticMeshAsset(AssetPath);
	if (!Mesh)
	{
		OutError = TEXT("static_mesh_not_found");
		OutData->SetArrayField(TEXT("json_issues"), Issues);
		OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
		return false;
	}

	const TArray<FString> OptionalJsonFiles = {
		TEXT("mesh.json"),
		TEXT("materials.json"),
		FPaths::Combine(TEXT("lods"), TEXT("index.json")),
		TEXT("sections.json"),
		TEXT("collision.json"),
		TEXT("sockets.json"),
		TEXT("lightmap_uv.json"),
		TEXT("nanite.json"),
		TEXT("build_settings.json"),
		TEXT("import_data.json"),
		TEXT("raw_mesh_summary.json"),
		TEXT("raw_properties.json"),
		TEXT("readonly_properties.json"),
		FPaths::Combine(TEXT("validation"), TEXT("coverage_report.json")),
		FPaths::Combine(TEXT("validation"), TEXT("readback_diff.json"))
	};
	for (const FString& JsonFile : OptionalJsonFiles)
	{
		TSharedPtr<FJsonObject> LoadedJson;
		if (!UeAgentStaticMeshOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, JsonFile), LoadedJson, Issues, OutError, true))
		{
			OutData->SetArrayField(TEXT("json_issues"), Issues);
			OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
			OutData->SetNumberField(TEXT("error_count"), UeAgentJsonDiagnostics::CountIssuesBySeverity(Issues, TEXT("error")));
			OutData->SetNumberField(TEXT("warning_count"), UeAgentJsonDiagnostics::CountIssuesBySeverity(Issues, TEXT("warning")));
			return false;
		}
		if (JsonFile == FPaths::Combine(TEXT("lods"), TEXT("index.json"))
			|| JsonFile == TEXT("sections.json")
			|| JsonFile == TEXT("lightmap_uv.json")
			|| JsonFile == TEXT("build_settings.json")
			|| JsonFile == TEXT("import_data.json")
			|| JsonFile == TEXT("raw_mesh_summary.json")
			|| JsonFile == TEXT("raw_properties.json")
			|| JsonFile == TEXT("readonly_properties.json")
			|| JsonFile.StartsWith(TEXT("validation")))
		{
			UeAgentJsonDiagnostics::AddUnsupportedApplyIssueIfWriteIntent(LoadedJson, JsonFile, TEXT("StaticMesh summary profile"), Issues);
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
	bool bNeedsBuild = false;
	if (!bDryRun && !bValidateOnly)
	{
		TSharedPtr<FJsonObject> MeshJson;
		if (!UeAgentStaticMeshOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, TEXT("mesh.json")), MeshJson, Issues, OutError, true)) return false;
		if (MeshJson.IsValid())
		{
			UeAgentJsonDiagnostics::WarnUnknownFields(MeshJson, TEXT("mesh.json"), { TEXT("static_mesh"), TEXT("lod_count"), TEXT("source_model_count"), TEXT("material_slot_count"), TEXT("socket_count"), TEXT("simple_collision_count"), TEXT("complex_collision_mode"), TEXT("nanite_enabled"), TEXT("lightmap_resolution"), TEXT("lightmap_coordinate_index"), TEXT("allow_cpu_access"), TEXT("bounds"), TEXT("edit_policy") }, Issues);
			bool bAllowCpuAccess = Mesh->bAllowCPUAccess;
			if (UeAgentJsonDiagnostics::ReadBoolField(MeshJson, TEXT("allow_cpu_access"), TEXT("mesh.json.allow_cpu_access"), bAllowCpuAccess, Issues, false))
			{
				Mesh->Modify();
				Mesh->bAllowCPUAccess = bAllowCpuAccess;
				++StructuredApplied;
				bNeedsBuild = true;
				AddPropertyResult(TEXT("mesh.json.allow_cpu_access"), TEXT("applied"), bAllowCpuAccess ? TEXT("true") : TEXT("false"));
			}
			double NumberValue = 0.0;
			if (MeshJson->TryGetNumberField(TEXT("lightmap_resolution"), NumberValue))
			{
				Mesh->Modify();
				Mesh->SetLightMapResolution(FMath::Max(0, static_cast<int32>(NumberValue)));
				++StructuredApplied;
				AddPropertyResult(TEXT("mesh.json.lightmap_resolution"), TEXT("applied"), FString::FromInt(Mesh->GetLightMapResolution()));
			}
			if (MeshJson->TryGetNumberField(TEXT("lightmap_coordinate_index"), NumberValue))
			{
				Mesh->Modify();
				Mesh->SetLightMapCoordinateIndex(FMath::Max(0, static_cast<int32>(NumberValue)));
				++StructuredApplied;
				AddPropertyResult(TEXT("mesh.json.lightmap_coordinate_index"), TEXT("applied"), FString::FromInt(Mesh->GetLightMapCoordinateIndex()));
			}
		}

		TSharedPtr<FJsonObject> MaterialsJson;
		if (!UeAgentStaticMeshOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, TEXT("materials.json")), MaterialsJson, Issues, OutError, true)) return false;
		if (MaterialsJson.IsValid())
		{
			UeAgentJsonDiagnostics::WarnUnknownFields(MaterialsJson, TEXT("materials.json"), { TEXT("materials") }, Issues);
			const TArray<TSharedPtr<FJsonValue>>* Materials = nullptr;
			if (MaterialsJson->TryGetArrayField(TEXT("materials"), Materials) && Materials)
			{
				for (int32 Index = 0; Index < Materials->Num(); ++Index)
				{
					TSharedPtr<FJsonObject> MaterialObj;
					if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*Materials)[Index], FString::Printf(TEXT("materials.json.materials[%d]"), Index), MaterialObj, Issues))
					{
						continue;
					}
					UeAgentJsonDiagnostics::WarnUnknownFields(MaterialObj, FString::Printf(TEXT("materials.json.materials[%d]"), Index), { TEXT("slot_index"), TEXT("slot_name"), TEXT("imported_slot_name"), TEXT("imported_material_slot_name"), TEXT("material"), TEXT("material_path"), TEXT("overlay_material_path"), TEXT("section_references"), TEXT("remove") }, Issues);
					double SlotIndexValue = 0.0;
					if (!MaterialObj->TryGetNumberField(TEXT("slot_index"), SlotIndexValue))
					{
						UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("json_missing_required_field"), FString::Printf(TEXT("materials.json.materials[%d].slot_index"), Index), TEXT("slot_index is required."));
						continue;
					}
					const int32 SlotIndex = static_cast<int32>(SlotIndexValue);
					if (!Mesh->GetStaticMaterials().IsValidIndex(SlotIndex))
					{
						UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("material_slot_index_out_of_range"), FString::Printf(TEXT("materials.json.materials[%d].slot_index"), Index), FString::Printf(TEXT("Static mesh has %d material slots."), Mesh->GetStaticMaterials().Num()));
						continue;
					}
					FString MaterialPath;
					if (!MaterialObj->TryGetStringField(TEXT("material"), MaterialPath))
					{
						MaterialObj->TryGetStringField(TEXT("material_path"), MaterialPath);
					}
					if (!MaterialPath.TrimStartAndEnd().IsEmpty())
					{
						UMaterialInterface* Material = UeAgentStaticMeshOps::LoadMaterialAsset(MaterialPath);
						if (!Material)
						{
							UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("material_not_found"), FString::Printf(TEXT("materials.json.materials[%d].material"), Index), MaterialPath);
							continue;
						}
						Mesh->Modify();
						Mesh->SetMaterial(SlotIndex, Material);
						++StructuredApplied;
						AddPropertyResult(FString::Printf(TEXT("materials.json.materials[%d].material"), Index), TEXT("applied"), MaterialPath);
					}
					FString SlotName;
					if (MaterialObj->TryGetStringField(TEXT("slot_name"), SlotName) && !SlotName.TrimStartAndEnd().IsEmpty())
					{
						Mesh->Modify();
						TArray<FStaticMaterial>& StaticMaterials = Mesh->GetStaticMaterials();
						StaticMaterials[SlotIndex].MaterialSlotName = FName(*SlotName.TrimStartAndEnd());
						++StructuredApplied;
						AddPropertyResult(FString::Printf(TEXT("materials.json.materials[%d].slot_name"), Index), TEXT("applied"), SlotName);
					}
				}
			}
		}

		TSharedPtr<FJsonObject> CollisionJson;
		if (!UeAgentStaticMeshOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, TEXT("collision.json")), CollisionJson, Issues, OutError, true)) return false;
		if (CollisionJson.IsValid())
		{
			UeAgentJsonDiagnostics::WarnUnknownFields(CollisionJson, TEXT("collision.json"), { TEXT("collision_complexity"), TEXT("simple_collision"), TEXT("boxes"), TEXT("spheres"), TEXT("capsules"), TEXT("convex_hulls"), TEXT("convex_hull_count"), TEXT("edit_policy") }, Issues);
			Mesh->CreateBodySetup();
			UBodySetup* BodySetup = Mesh->GetBodySetup();
			if (BodySetup)
			{
				double TraceFlagValue = 0.0;
				if (CollisionJson->TryGetNumberField(TEXT("collision_complexity"), TraceFlagValue))
				{
					Mesh->Modify();
					BodySetup->Modify();
					BodySetup->CollisionTraceFlag = static_cast<ECollisionTraceFlag>(static_cast<int32>(TraceFlagValue));
					++StructuredApplied;
					AddPropertyResult(TEXT("collision.json.collision_complexity"), TEXT("applied"), FString::FromInt(static_cast<int32>(TraceFlagValue)));
				}
				bool bTouchedCollision = false;
				const TArray<TSharedPtr<FJsonValue>>* Boxes = nullptr;
				if (CollisionJson->TryGetArrayField(TEXT("boxes"), Boxes) && Boxes)
				{
					BodySetup->AggGeom.BoxElems.Reset();
					for (int32 Index = 0; Index < Boxes->Num(); ++Index)
					{
						TSharedPtr<FJsonObject> BoxObj;
						if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*Boxes)[Index], FString::Printf(TEXT("collision.json.boxes[%d]"), Index), BoxObj, Issues))
						{
							continue;
						}
						FKBoxElem BoxElem;
						UeAgentJsonDiagnostics::ReadVectorField(BoxObj, TEXT("center"), FString::Printf(TEXT("collision.json.boxes[%d].center"), Index), BoxElem.Center, Issues, false);
						UeAgentJsonDiagnostics::ReadRotatorField(BoxObj, TEXT("rotation"), FString::Printf(TEXT("collision.json.boxes[%d].rotation"), Index), BoxElem.Rotation, Issues, false);
						double X = 1.0, Y = 1.0, Z = 1.0;
						if (BoxObj->TryGetNumberField(TEXT("x"), X) && BoxObj->TryGetNumberField(TEXT("y"), Y) && BoxObj->TryGetNumberField(TEXT("z"), Z))
						{
							BoxElem.X = FMath::Max(1.0f, static_cast<float>(X));
							BoxElem.Y = FMath::Max(1.0f, static_cast<float>(Y));
							BoxElem.Z = FMath::Max(1.0f, static_cast<float>(Z));
						}
						BodySetup->AggGeom.BoxElems.Add(BoxElem);
					}
					bTouchedCollision = true;
					AddPropertyResult(TEXT("collision.json.boxes"), TEXT("applied"), FString::FromInt(Boxes->Num()));
				}
				const TArray<TSharedPtr<FJsonValue>>* Spheres = nullptr;
				if (CollisionJson->TryGetArrayField(TEXT("spheres"), Spheres) && Spheres)
				{
					BodySetup->AggGeom.SphereElems.Reset();
					for (int32 Index = 0; Index < Spheres->Num(); ++Index)
					{
						TSharedPtr<FJsonObject> SphereObj;
						if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*Spheres)[Index], FString::Printf(TEXT("collision.json.spheres[%d]"), Index), SphereObj, Issues))
						{
							continue;
						}
						FKSphereElem SphereElem;
						UeAgentJsonDiagnostics::ReadVectorField(SphereObj, TEXT("center"), FString::Printf(TEXT("collision.json.spheres[%d].center"), Index), SphereElem.Center, Issues, false);
						double Radius = 0.0;
						if (SphereObj->TryGetNumberField(TEXT("radius"), Radius))
						{
							SphereElem.Radius = FMath::Max(1.0f, static_cast<float>(Radius));
							BodySetup->AggGeom.SphereElems.Add(SphereElem);
						}
						else
						{
							UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("json_missing_required_field"), FString::Printf(TEXT("collision.json.spheres[%d].radius"), Index), TEXT("radius is required."));
						}
					}
					bTouchedCollision = true;
					AddPropertyResult(TEXT("collision.json.spheres"), TEXT("applied"), FString::FromInt(Spheres->Num()));
				}
				const TArray<TSharedPtr<FJsonValue>>* Capsules = nullptr;
				if (CollisionJson->TryGetArrayField(TEXT("capsules"), Capsules) && Capsules)
				{
					BodySetup->AggGeom.SphylElems.Reset();
					for (int32 Index = 0; Index < Capsules->Num(); ++Index)
					{
						TSharedPtr<FJsonObject> CapsuleObj;
						if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*Capsules)[Index], FString::Printf(TEXT("collision.json.capsules[%d]"), Index), CapsuleObj, Issues))
						{
							continue;
						}
						FKSphylElem CapsuleElem;
						UeAgentJsonDiagnostics::ReadVectorField(CapsuleObj, TEXT("center"), FString::Printf(TEXT("collision.json.capsules[%d].center"), Index), CapsuleElem.Center, Issues, false);
						UeAgentJsonDiagnostics::ReadRotatorField(CapsuleObj, TEXT("rotation"), FString::Printf(TEXT("collision.json.capsules[%d].rotation"), Index), CapsuleElem.Rotation, Issues, false);
						double Radius = 0.0, Length = 0.0;
						if (CapsuleObj->TryGetNumberField(TEXT("radius"), Radius) && CapsuleObj->TryGetNumberField(TEXT("length"), Length))
						{
							CapsuleElem.Radius = FMath::Max(1.0f, static_cast<float>(Radius));
							CapsuleElem.Length = FMath::Max(1.0f, static_cast<float>(Length));
							BodySetup->AggGeom.SphylElems.Add(CapsuleElem);
						}
						else
						{
							UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("json_missing_required_field"), FString::Printf(TEXT("collision.json.capsules[%d]"), Index), TEXT("radius and length are required."));
						}
					}
					bTouchedCollision = true;
					AddPropertyResult(TEXT("collision.json.capsules"), TEXT("applied"), FString::FromInt(Capsules->Num()));
				}
				if (bTouchedCollision)
				{
					++StructuredApplied;
					UeAgentStaticMeshOps::ApplyMeshPostEdit(Mesh, true);
				}
			}
		}

		TSharedPtr<FJsonObject> SocketsJson;
		if (!UeAgentStaticMeshOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, TEXT("sockets.json")), SocketsJson, Issues, OutError, true)) return false;
		if (SocketsJson.IsValid())
		{
			UeAgentJsonDiagnostics::WarnUnknownFields(SocketsJson, TEXT("sockets.json"), { TEXT("sockets") }, Issues);
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
						UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("json_missing_required_field"), FString::Printf(TEXT("sockets.json.sockets[%d].socket_name"), Index), TEXT("socket_name is required."));
						continue;
					}
					bool bRemove = false;
					SocketObj->TryGetBoolField(TEXT("remove"), bRemove);
					UStaticMeshSocket* Socket = Mesh->FindSocket(FName(*SocketName.TrimStartAndEnd()));
					Mesh->Modify();
					if (bRemove)
					{
						if (Socket)
						{
							Mesh->RemoveSocket(Socket);
							++StructuredApplied;
						}
						continue;
					}
					if (!Socket)
					{
						Socket = NewObject<UStaticMeshSocket>(Mesh, NAME_None, RF_Transactional);
						Socket->SocketName = FName(*SocketName.TrimStartAndEnd());
						Mesh->AddSocket(Socket);
					}
					if (Socket)
					{
						FVector Location(Socket->RelativeLocation);
						FRotator Rotation(Socket->RelativeRotation);
						FVector Scale(Socket->RelativeScale);
						if (!UeAgentJsonDiagnostics::ReadVectorField(SocketObj, TEXT("relative_location"), FString::Printf(TEXT("sockets.json.sockets[%d].relative_location"), Index), Location, Issues, false))
						{
							double X = Location.X, Y = Location.Y, Z = Location.Z;
							if (SocketObj->TryGetNumberField(TEXT("location_x"), X)) Location.X = X;
							if (SocketObj->TryGetNumberField(TEXT("location_y"), Y)) Location.Y = Y;
							if (SocketObj->TryGetNumberField(TEXT("location_z"), Z)) Location.Z = Z;
						}
						UeAgentJsonDiagnostics::ReadRotatorField(SocketObj, TEXT("relative_rotation"), FString::Printf(TEXT("sockets.json.sockets[%d].relative_rotation"), Index), Rotation, Issues, false);
						UeAgentJsonDiagnostics::ReadVectorField(SocketObj, TEXT("relative_scale"), FString::Printf(TEXT("sockets.json.sockets[%d].relative_scale"), Index), Scale, Issues, false);
						Socket->RelativeLocation = Location;
						Socket->RelativeRotation = Rotation;
						Socket->RelativeScale = Scale;
						SocketObj->TryGetStringField(TEXT("tag"), Socket->Tag);
						++StructuredApplied;
					}
				}
				AddPropertyResult(TEXT("sockets.json.sockets"), TEXT("applied"), FString::FromInt(Sockets->Num()));
			}
		}

		TSharedPtr<FJsonObject> NaniteJson;
		if (!UeAgentStaticMeshOps::LoadJsonFileWithIssues(FPaths::Combine(FolderPath, TEXT("nanite.json")), NaniteJson, Issues, OutError, true)) return false;
		if (NaniteJson.IsValid())
		{
			UeAgentJsonDiagnostics::WarnUnknownFields(NaniteJson, TEXT("nanite.json"), { TEXT("enabled"), TEXT("preserve_area"), TEXT("explicit_tangents"), TEXT("lerp_uvs"), TEXT("keep_percent_triangles"), TEXT("trim_relative_error"), TEXT("fallback_percent_triangles"), TEXT("fallback_relative_error"), TEXT("max_edge_length_factor"), TEXT("position_precision"), TEXT("normal_precision"), TEXT("tangent_precision"), TEXT("target_minimum_residency_kb"), TEXT("edit_policy") }, Issues);
			bool bBoolValue = false;
			double NumberValue = 0.0;
			Mesh->Modify();
			if (UeAgentJsonDiagnostics::ReadBoolField(NaniteJson, TEXT("enabled"), TEXT("nanite.json.enabled"), bBoolValue, Issues, false)) { Mesh->NaniteSettings.bEnabled = bBoolValue; ++StructuredApplied; bNeedsBuild = true; }
			if (UeAgentJsonDiagnostics::ReadBoolField(NaniteJson, TEXT("preserve_area"), TEXT("nanite.json.preserve_area"), bBoolValue, Issues, false)) { Mesh->NaniteSettings.bPreserveArea = bBoolValue; ++StructuredApplied; bNeedsBuild = true; }
			if (UeAgentJsonDiagnostics::ReadBoolField(NaniteJson, TEXT("explicit_tangents"), TEXT("nanite.json.explicit_tangents"), bBoolValue, Issues, false)) { Mesh->NaniteSettings.bExplicitTangents = bBoolValue; ++StructuredApplied; bNeedsBuild = true; }
			if (NaniteJson->TryGetNumberField(TEXT("keep_percent_triangles"), NumberValue)) { Mesh->NaniteSettings.KeepPercentTriangles = FMath::Clamp(static_cast<float>(NumberValue), 0.0f, 1.0f); ++StructuredApplied; bNeedsBuild = true; }
			if (NaniteJson->TryGetNumberField(TEXT("trim_relative_error"), NumberValue)) { Mesh->NaniteSettings.TrimRelativeError = FMath::Max(0.0f, static_cast<float>(NumberValue)); ++StructuredApplied; bNeedsBuild = true; }
			if (NaniteJson->TryGetNumberField(TEXT("fallback_percent_triangles"), NumberValue)) { Mesh->NaniteSettings.FallbackPercentTriangles = FMath::Clamp(static_cast<float>(NumberValue), 0.0f, 1.0f); ++StructuredApplied; bNeedsBuild = true; }
			if (NaniteJson->TryGetNumberField(TEXT("fallback_relative_error"), NumberValue)) { Mesh->NaniteSettings.FallbackRelativeError = FMath::Max(0.0f, static_cast<float>(NumberValue)); ++StructuredApplied; bNeedsBuild = true; }
			AddPropertyResult(TEXT("nanite.json"), TEXT("applied"), TEXT("safe nanite settings read"));
		}

		bool bBuildAfterApply = bNeedsBuild;
		JsonTryGetBool(Ctx.Params, TEXT("build_after_apply"), bBuildAfterApply);
		if (bBuildAfterApply)
		{
			TArray<FText> BuildErrors;
			Mesh->Build(false, &BuildErrors);
			if (BuildErrors.Num() > 0)
			{
				for (const FText& BuildError : BuildErrors)
				{
					UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("static_mesh_build_error"), TEXT("static_mesh_build"), BuildError.ToString());
				}
			}
			AddPropertyResult(TEXT("static_mesh_build"), BuildErrors.Num() == 0 ? TEXT("applied") : TEXT("build_errors"), FString::FromInt(BuildErrors.Num()));
		}
		else
		{
			UeAgentStaticMeshOps::ApplyMeshPostEdit(Mesh, false);
		}

		bool bSaveAfterApply = false;
		JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
		if (bSaveAfterApply && !UeAgentStaticMeshOps::SaveMeshPackage(Mesh, OutError))
		{
			return false;
		}
	}

	TSharedPtr<FJsonObject> InfoParams = MakeShared<FJsonObject>();
	InfoParams->SetStringField(TEXT("asset_path"), AssetPath);
	TSharedPtr<FJsonObject> ReadbackData = MakeShared<FJsonObject>();
	FString ReadbackError;
	CmdStaticMeshGetInfo(FUeAgentRequestContext{ TEXT("static_mesh_folder_readback"), TEXT("static_mesh_get_info"), InfoParams }, ReadbackData, ReadbackError);

	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetBoolField(TEXT("applied"), !(bDryRun || bValidateOnly));
	OutData->SetBoolField(TEXT("dry_run"), bDryRun || bValidateOnly);
	OutData->SetNumberField(TEXT("structured_fields_applied"), StructuredApplied);
	OutData->SetNumberField(TEXT("raw_properties_applied"), 0);
	OutData->SetArrayField(TEXT("json_issues"), Issues);
	OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
	OutData->SetNumberField(TEXT("error_count"), UeAgentJsonDiagnostics::CountIssuesBySeverity(Issues, TEXT("error")));
	OutData->SetNumberField(TEXT("warning_count"), UeAgentJsonDiagnostics::CountIssuesBySeverity(Issues, TEXT("warning")));
	OutData->SetArrayField(TEXT("property_results"), PropertyResults);
	OutData->SetObjectField(TEXT("readback"), ReadbackData);
	OutData->SetObjectField(TEXT("validation_report"), UeAgentStaticMeshOps::BuildCoverageJson());
	if (UeAgentJsonDiagnostics::HasErrorIssue(Issues))
	{
		OutError = TEXT("json_validation_failed");
		return false;
	}
	return true;
}

bool FUeAgentHttpServer::CmdStaticMeshValidateFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Ctx.Params->Values)
	{
		Params->SetField(Pair.Key, Pair.Value);
	}
	Params->SetBoolField(TEXT("validate_only"), true);
	Params->SetBoolField(TEXT("dry_run"), true);
	FUeAgentRequestContext SubCtx;
	SubCtx.RequestId = TEXT("static_mesh_folder_validate");
	SubCtx.Command = TEXT("static_mesh_apply_folder");
	SubCtx.Params = Params;
	return CmdStaticMeshApplyFolder(SubCtx, OutData, OutError);
}

bool FUeAgentHttpServer::CmdStaticMeshValidateGeometry(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UStaticMesh* Mesh = UeAgentStaticMeshOps::LoadStaticMeshAsset(AssetPathInput);
	if (!Mesh)
	{
		OutError = TEXT("static_mesh_not_found");
		return false;
	}
	OutData = UeAgentStaticMeshOps::BuildRawMeshSummaryJson(Mesh);
	OutData->SetStringField(TEXT("asset_path"), Mesh->GetOutermost()->GetName());
	OutData->SetObjectField(TEXT("sections"), UeAgentStaticMeshOps::BuildSectionsJson(Mesh));
	OutData->SetObjectField(TEXT("validation_report"), UeAgentStaticMeshOps::BuildCoverageJson());
	return true;
}

bool FUeAgentHttpServer::CmdStaticMeshValidateUvs(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UStaticMesh* Mesh = UeAgentStaticMeshOps::LoadStaticMeshAsset(AssetPathInput);
	if (!Mesh)
	{
		OutError = TEXT("static_mesh_not_found");
		return false;
	}
	OutData = UeAgentStaticMeshOps::BuildLightmapUvJson(Mesh);
	OutData->SetStringField(TEXT("asset_path"), Mesh->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("raw_uv_policy"), TEXT("summary_only"));
	return true;
}

bool FUeAgentHttpServer::CmdStaticMeshBuild(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UStaticMesh* Mesh = UeAgentStaticMeshOps::LoadStaticMeshAsset(AssetPathInput);
	if (!Mesh)
	{
		OutError = TEXT("static_mesh_not_found");
		return false;
	}
	TArray<FText> BuildErrors;
	Mesh->Modify();
	Mesh->Build(false, &BuildErrors);
	UeAgentStaticMeshOps::ApplyMeshPostEdit(Mesh, false);
	TArray<TSharedPtr<FJsonValue>> ErrorArray;
	for (const FText& BuildError : BuildErrors)
	{
		ErrorArray.Add(MakeShared<FJsonValueString>(BuildError.ToString()));
	}
	OutData->SetStringField(TEXT("asset_path"), Mesh->GetOutermost()->GetName());
	OutData->SetNumberField(TEXT("build_error_count"), BuildErrors.Num());
	OutData->SetArrayField(TEXT("build_errors"), ErrorArray);
	return BuildErrors.Num() == 0;
}

bool FUeAgentHttpServer::CmdStaticMeshReimport(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UStaticMesh* Mesh = UeAgentStaticMeshOps::LoadStaticMeshAsset(AssetPathInput);
	if (!Mesh)
	{
		OutError = TEXT("static_mesh_not_found");
		return false;
	}

	bool bSaveAfterReimport = true;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_reimport"), bSaveAfterReimport);

	bool bShowNotification = false;
	JsonTryGetBool(Ctx.Params, TEXT("show_notification"), bShowNotification);

	FString PreferredSourceFile;
	JsonTryGetString(Ctx.Params, TEXT("source_filename"), PreferredSourceFile);
	PreferredSourceFile.TrimStartAndEndInline();

	FReimportManager* ReimportManager = FReimportManager::Instance();
	if (!ReimportManager)
	{
		OutError = TEXT("reimport_manager_unavailable");
		return false;
	}

	TArray<FString> SourceFilenames;
	const bool bCanReimport = ReimportManager->CanReimport(Mesh, &SourceFilenames);

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("asset_path"), Mesh->GetOutermost()->GetName());
	OutData->SetBoolField(TEXT("can_reimport"), bCanReimport);
	OutData->SetBoolField(TEXT("save_after_reimport"), bSaveAfterReimport);
	OutData->SetStringField(TEXT("preferred_source_filename"), PreferredSourceFile);
	TArray<TSharedPtr<FJsonValue>> SourceArray;
	for (const FString& SourceFilename : SourceFilenames)
	{
		SourceArray.Add(MakeShared<FJsonValueString>(SourceFilename));
	}
	OutData->SetArrayField(TEXT("source_filenames"), SourceArray);

	if (!bCanReimport)
	{
		OutData->SetStringField(TEXT("reimport_status"), TEXT("cannot_reimport"));
		OutError = TEXT("static_mesh_cannot_reimport");
		return false;
	}

	if (!PreferredSourceFile.IsEmpty() && !FPaths::FileExists(PreferredSourceFile))
	{
		OutData->SetStringField(TEXT("reimport_status"), TEXT("source_file_not_found"));
		OutError = TEXT("source_file_not_found");
		return false;
	}

	Mesh->Modify();
	const bool bReimported = ReimportManager->Reimport(
		Mesh,
		/*bAskForNewFileIfMissing*/ false,
		bShowNotification,
		PreferredSourceFile,
		/*SpecifiedReimportHandler*/ nullptr,
		/*SourceFileIndex*/ INDEX_NONE,
		/*bForceNewFile*/ !PreferredSourceFile.IsEmpty(),
		/*bAutomated*/ true);

	OutData->SetBoolField(TEXT("reimported"), bReimported);
	OutData->SetStringField(TEXT("reimport_status"), bReimported ? TEXT("reimported") : TEXT("reimport_failed"));
	if (!bReimported)
	{
		OutError = TEXT("static_mesh_reimport_failed");
		return false;
	}

	UeAgentStaticMeshOps::ApplyMeshPostEdit(Mesh, true);
	if (bSaveAfterReimport && !UeAgentStaticMeshOps::SaveMeshPackage(Mesh, OutError))
	{
		return false;
	}

	OutData->SetObjectField(TEXT("readback"), UeAgentStaticMeshOps::BuildMeshJson(Mesh));
	return true;
}

bool FUeAgentHttpServer::CmdStaticMeshPreviewCollision(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UStaticMesh* Mesh = UeAgentStaticMeshOps::LoadStaticMeshAsset(AssetPathInput);
	if (!Mesh)
	{
		OutError = TEXT("static_mesh_not_found");
		return false;
	}

	const FBoxSphereBounds Bounds = Mesh->GetBounds();
	TSharedPtr<FJsonObject> BoundsJson = MakeShared<FJsonObject>();
	BoundsJson->SetObjectField(TEXT("origin"), UeAgentStaticMeshOps::MakeVectorJson(Bounds.Origin));
	BoundsJson->SetObjectField(TEXT("box_extent"), UeAgentStaticMeshOps::MakeVectorJson(Bounds.BoxExtent));
	BoundsJson->SetNumberField(TEXT("sphere_radius"), Bounds.SphereRadius);

	TArray<TSharedPtr<FJsonValue>> RecommendedCommands;
	RecommendedCommands.Add(MakeShared<FJsonValueString>(TEXT("level_trace_world_ray")));
	RecommendedCommands.Add(MakeShared<FJsonValueString>(TEXT("level_sweep_capsule")));
	RecommendedCommands.Add(MakeShared<FJsonValueString>(TEXT("level_check_overlaps")));

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("asset_path"), Mesh->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("preview_status"), TEXT("headless_collision_summary"));
	OutData->SetObjectField(TEXT("collision"), UeAgentStaticMeshOps::BuildCollisionJson(Mesh));
	OutData->SetObjectField(TEXT("bounds"), BoundsJson);
	OutData->SetArrayField(TEXT("recommended_runtime_validation_commands"), RecommendedCommands);
	OutData->SetStringField(TEXT("note"), TEXT("This command returns collision preview data without opening a viewport. Validate scene behavior with trace, sweep or overlap commands."));
	return true;
}
