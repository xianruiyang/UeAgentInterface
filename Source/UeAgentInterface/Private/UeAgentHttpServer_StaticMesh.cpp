// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_StaticMesh.h"

#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "FileHelpers.h"
#include "IStaticMeshEditor.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/BoxElem.h"
#include "PhysicsEngine/SphereElem.h"
#include "PhysicsEngine/SphylElem.h"
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
