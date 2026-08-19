// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentLevelContentJsonApply.h"
#include "UeAgentLevelContentJsonCore.h"
#include "UeAgentLevelContentJsonProperties.h"
#include "UeAgentLevelContentJsonQuery.h"
#include "UeAgentLevelContentJsonValidatePlan.h"

#include "ActorFactories/ActorFactory.h"
#include "Builders/CubeBuilder.h"
#include "Components/ActorComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SplineComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorActorFolders.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "Misc/PackageName.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "Subsystems/EditorActorSubsystem.h"

namespace UeAgentLevelContentJson
{
	bool ApplyStaticMeshAdapter(AActor* Actor, const TSharedPtr<FJsonObject>& Resource, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
	{
		FString MeshPath;
		if (!TryGetStringAny(Resource, { TEXT("static_mesh"), TEXT("mesh") }, MeshPath) || MeshPath.TrimStartAndEnd().IsEmpty())
		{
			TSharedPtr<FJsonObject> Asset;
			if (TryGetObject(Resource, TEXT("asset"), Asset))
			{
				TryGetStringAny(Asset, { TEXT("static_mesh"), TEXT("mesh") }, MeshPath);
			}
		}
		if (MeshPath.TrimStartAndEnd().IsEmpty())
		{
			TSharedPtr<FJsonObject> Properties;
			if (TryGetObject(Resource, TEXT("properties"), Properties))
			{
				TSharedPtr<FJsonObject> StaticMeshObj;
				if (TryGetObject(Properties, TEXT("StaticMesh"), StaticMeshObj))
				{
					StaticMeshObj->TryGetStringField(TEXT("asset"), MeshPath);
				}
			}
		}
		if (MeshPath.TrimStartAndEnd().IsEmpty())
		{
			return true;
		}
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);
		if (!Mesh)
		{
			OutError = TEXT("static_mesh_not_found");
			return false;
		}
		UStaticMeshComponent* Component = nullptr;
		if (AStaticMeshActor* StaticMeshActor = Cast<AStaticMeshActor>(Actor))
		{
			Component = StaticMeshActor->GetStaticMeshComponent();
		}
		if (!Component)
		{
			Component = Actor ? Actor->FindComponentByClass<UStaticMeshComponent>() : nullptr;
		}
		if (!Component)
		{
			OutError = TEXT("static_mesh_component_not_found");
			return false;
		}
		Actor->Modify();
		Component->Modify();
		Component->SetStaticMesh(Mesh);
		Component->MarkRenderStateDirty();
		OutResult->SetStringField(TEXT("static_mesh"), MeshPath);
		return true;
	}

	bool ApplyActorTransform(AActor* Actor, const TSharedPtr<FJsonObject>& Resource, TSharedPtr<FJsonObject>& OutResult)
	{
		if (!Actor || !Resource.IsValid())
		{
			return false;
		}
		TSharedPtr<FJsonObject> Transform;
		if (!TryGetObject(Resource, TEXT("transform"), Transform))
		{
			return true;
		}
		FVector Location = Actor->GetActorLocation();
		FRotator Rotation = Actor->GetActorRotation();
		FVector Scale = Actor->GetActorScale3D();
		const bool bHasLocation = TryGetVectorField(Transform, TEXT("location"), Location);
		const bool bHasRotation = TryGetRotatorField(Transform, TEXT("rotation"), Rotation);
		const bool bHasScale = TryGetVectorField(Transform, TEXT("scale"), Scale);
		if (!bHasLocation && !bHasRotation && !bHasScale)
		{
			return true;
		}
		Actor->Modify();
		Actor->SetActorTransform(FTransform(Rotation, Location, Scale), false, nullptr, ETeleportType::TeleportPhysics);
		TSharedPtr<FJsonObject> Applied = MakeShared<FJsonObject>();
		Applied->SetObjectField(TEXT("location"), MakeVectorJson(Actor->GetActorLocation()));
		Applied->SetObjectField(TEXT("rotation"), MakeRotatorJson(Actor->GetActorRotation()));
		Applied->SetObjectField(TEXT("scale"), MakeVectorJson(Actor->GetActorScale3D()));
		OutResult->SetObjectField(TEXT("transform"), Applied);
		return true;
	}

	bool TryReadNavMeshBoundsSpec(const TSharedPtr<FJsonObject>& Resource, AActor* Actor, bool bActorCreatedThisCall, FBox& OutBounds, FRotator& OutRotation, bool& bOutExplicitBounds)
	{
		bOutExplicitBounds = false;
		OutBounds = FBox(ForceInit);
		if (!Resource.IsValid() || !Actor)
		{
			return false;
		}

		TSharedPtr<FJsonObject> BoundsObj;
		if ((TryGetObject(Resource, TEXT("bounds"), BoundsObj) ||
			TryGetObject(Resource, TEXT("nav_bounds"), BoundsObj) ||
			TryGetObject(Resource, TEXT("brush_bounds"), BoundsObj)) &&
			TryReadBoundsSpec(BoundsObj, OutBounds) &&
			OutBounds.IsValid != 0)
		{
			bOutExplicitBounds = true;
		}
		else if (TryReadBoundsSpec(Resource, OutBounds) && OutBounds.IsValid != 0)
		{
			bOutExplicitBounds = true;
		}
		else if (bActorCreatedThisCall)
		{
			const FVector Scale = Actor->GetActorScale3D();
			FVector Extent(
				FMath::Abs(Scale.X) * 100.0,
				FMath::Abs(Scale.Y) * 100.0,
				FMath::Abs(Scale.Z) * 100.0);
			if (Extent.X <= KINDA_SMALL_NUMBER)
			{
				Extent.X = 100.0;
			}
			if (Extent.Y <= KINDA_SMALL_NUMBER)
			{
				Extent.Y = 100.0;
			}
			if (Extent.Z <= KINDA_SMALL_NUMBER)
			{
				Extent.Z = 100.0;
			}
			OutBounds = FBox::BuildAABB(Actor->GetActorLocation(), Extent);
		}
		else
		{
			return false;
		}

		FVector Center = OutBounds.GetCenter();
		FVector Extent = OutBounds.GetExtent();
		Extent = FVector(FMath::Abs(Extent.X), FMath::Abs(Extent.Y), FMath::Abs(Extent.Z));
		if (Extent.X <= KINDA_SMALL_NUMBER || Extent.Y <= KINDA_SMALL_NUMBER || Extent.Z <= KINDA_SMALL_NUMBER)
		{
			return false;
		}
		OutBounds = FBox::BuildAABB(Center, Extent);

		OutRotation = Actor->GetActorRotation();
		TSharedPtr<FJsonObject> Transform;
		if (TryGetObject(Resource, TEXT("transform"), Transform))
		{
			TryGetRotatorField(Transform, TEXT("rotation"), OutRotation);
		}
		return true;
	}

	bool ApplyNavMeshBoundsAdapter(AActor* Actor, const TSharedPtr<FJsonObject>& Resource, UWorld* World, bool bActorCreatedThisCall, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
	{
		ANavMeshBoundsVolume* Volume = Cast<ANavMeshBoundsVolume>(Actor);
		if (!Volume)
		{
			return true;
		}

		FBox RequestedBounds(ForceInit);
		FRotator RequestedRotation = Volume->GetActorRotation();
		bool bExplicitBounds = false;
		if (!TryReadNavMeshBoundsSpec(Resource, Actor, bActorCreatedThisCall, RequestedBounds, RequestedRotation, bExplicitBounds))
		{
			if (bActorCreatedThisCall)
			{
				OutError = TEXT("navmesh_bounds_extent_invalid");
				return false;
			}
			return true;
		}

		const FVector Center = RequestedBounds.GetCenter();
		const FVector Extent = RequestedBounds.GetExtent();
		if (Extent.X <= KINDA_SMALL_NUMBER || Extent.Y <= KINDA_SMALL_NUMBER || Extent.Z <= KINDA_SMALL_NUMBER)
		{
			OutError = TEXT("navmesh_bounds_extent_invalid");
			return false;
		}

		bool bUpdateNavigationBounds = true;
		TryGetBoolAny(Resource, { TEXT("update_navigation_bounds"), TEXT("update_nav_bounds") }, bUpdateNavigationBounds);

		Volume->Modify();
		Volume->SetActorLocation(Center);
		Volume->SetActorRotation(RequestedRotation);
		Volume->SetActorScale3D(FVector::OneVector);

		UCubeBuilder* Builder = NewObject<UCubeBuilder>(GetTransientPackage());
		if (!Builder)
		{
			OutError = TEXT("navmesh_bounds_builder_create_failed");
			return false;
		}
		Builder->X = Extent.X * 2.0f;
		Builder->Y = Extent.Y * 2.0f;
		Builder->Z = Extent.Z * 2.0f;
		UActorFactory::CreateBrushForVolumeActor(Volume, Builder);

		Volume->SetActorLocation(Center);
		Volume->SetActorRotation(RequestedRotation);
		Volume->SetActorScale3D(FVector::OneVector);

		bool bNavBoundsUpdated = false;
		if (bUpdateNavigationBounds)
		{
			if (UNavigationSystemV1* NavSys = World ? FNavigationSystem::GetCurrent<UNavigationSystemV1>(World) : nullptr)
			{
				NavSys->OnNavigationBoundsUpdated(Volume);
				bNavBoundsUpdated = true;
			}
		}

		TSharedPtr<FJsonObject> AppliedTransform = MakeShared<FJsonObject>();
		AppliedTransform->SetObjectField(TEXT("location"), MakeVectorJson(Volume->GetActorLocation()));
		AppliedTransform->SetObjectField(TEXT("rotation"), MakeRotatorJson(Volume->GetActorRotation()));
		AppliedTransform->SetObjectField(TEXT("scale"), MakeVectorJson(Volume->GetActorScale3D()));
		OutResult->SetObjectField(TEXT("transform"), AppliedTransform);
		OutResult->SetObjectField(TEXT("bounds"), MakeBoundsJson(Volume->GetComponentsBoundingBox(true)));
		OutResult->SetObjectField(TEXT("requested_center"), MakeVectorJson(Center));
		OutResult->SetObjectField(TEXT("requested_extent"), MakeVectorJson(Extent));
		OutResult->SetObjectField(TEXT("builder_size"), MakeVectorJson(FVector(Builder->X, Builder->Y, Builder->Z)));
		OutResult->SetBoolField(TEXT("nav_bounds_explicit"), bExplicitBounds);
		OutResult->SetBoolField(TEXT("update_navigation_bounds"), bUpdateNavigationBounds);
		OutResult->SetBoolField(TEXT("nav_bounds_updated"), bNavBoundsUpdated);
		return true;
	}

	ESplinePointType::Type ParseSplinePointType(const FString& TypeText)
	{
		const FString Normalized = TypeText.TrimStartAndEnd().ToLower();
		if (Normalized == TEXT("linear") || Normalized == TEXT("esplinepointtype::linear"))
		{
			return ESplinePointType::Linear;
		}
		if (Normalized == TEXT("constant") || Normalized == TEXT("esplinepointtype::constant"))
		{
			return ESplinePointType::Constant;
		}
		if (Normalized == TEXT("curveclamped") || Normalized == TEXT("curve_clamped") || Normalized == TEXT("esplinepointtype::curveclamped"))
		{
			return ESplinePointType::CurveClamped;
		}
		if (Normalized == TEXT("curvecustomtangent") || Normalized == TEXT("curve_custom_tangent") || Normalized == TEXT("esplinepointtype::curvecustomtangent"))
		{
			return ESplinePointType::CurveCustomTangent;
		}
		return ESplinePointType::Curve;
	}

	bool ApplySplineComponentSpec(USplineComponent* SplineComponent, const TSharedPtr<FJsonObject>& ComponentSpec, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Points = nullptr;
		if (!SplineComponent || !ComponentSpec.IsValid() || !TryGetArray(ComponentSpec, TEXT("spline_points"), Points) || !Points)
		{
			return true;
		}

		FString SpaceText = TEXT("local");
		TryGetStringAny(ComponentSpec, { TEXT("spline_coordinate_space"), TEXT("coordinate_space") }, SpaceText);
		const ESplineCoordinateSpace::Type CoordinateSpace = SpaceText.TrimStartAndEnd().Equals(TEXT("world"), ESearchCase::IgnoreCase)
			? ESplineCoordinateSpace::World
			: ESplineCoordinateSpace::Local;

		bool bClosedLoop = SplineComponent->IsClosedLoop();
		TryGetBoolAny(ComponentSpec, { TEXT("spline_closed_loop"), TEXT("closed_loop") }, bClosedLoop);

		SplineComponent->Modify();
		SplineComponent->ClearSplinePoints(false);
		for (int32 PointIndex = 0; PointIndex < Points->Num(); ++PointIndex)
		{
			const TSharedPtr<FJsonObject> Point = (*Points)[PointIndex].IsValid() ? (*Points)[PointIndex]->AsObject() : nullptr;
			if (!Point.IsValid())
			{
				OutError = TEXT("spline_point_not_object");
				return false;
			}

			FVector Location = FVector::ZeroVector;
			if (!TryGetVectorField(Point, TEXT("location"), Location) && !TryGetVectorField(Point, TEXT("position"), Location))
			{
				OutError = TEXT("spline_point_missing_location");
				return false;
			}

			SplineComponent->AddSplinePoint(Location, CoordinateSpace, false);

			FString TypeText;
			if (TryGetStringAny(Point, { TEXT("type"), TEXT("point_type") }, TypeText))
			{
				SplineComponent->SetSplinePointType(PointIndex, ParseSplinePointType(TypeText), false);
			}

			FVector ArriveTangent = FVector::ZeroVector;
			FVector LeaveTangent = FVector::ZeroVector;
			const bool bHasArriveTangent = TryGetVectorField(Point, TEXT("arrive_tangent"), ArriveTangent);
			const bool bHasLeaveTangent = TryGetVectorField(Point, TEXT("leave_tangent"), LeaveTangent);
			if (bHasArriveTangent || bHasLeaveTangent)
			{
				SplineComponent->SetTangentsAtSplinePoint(PointIndex, ArriveTangent, LeaveTangent, CoordinateSpace, false);
			}
		}

		SplineComponent->SetClosedLoop(bClosedLoop, false);
		SplineComponent->UpdateSpline();
		SplineComponent->MarkPackageDirty();

		OutResult->SetBoolField(TEXT("spline_points_applied"), true);
		OutResult->SetNumberField(TEXT("spline_point_count"), SplineComponent->GetNumberOfSplinePoints());
		OutResult->SetBoolField(TEXT("spline_closed_loop"), SplineComponent->IsClosedLoop());
		return true;
	}

	bool ApplyComponentSpec(AActor* Actor, const TSharedPtr<FJsonObject>& ComponentSpec, UWorld* World, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
	{
		OutResult = MakeShared<FJsonObject>();
		if (!Actor || !ComponentSpec.IsValid())
		{
			OutError = TEXT("invalid_component_spec");
			return false;
		}
		FString Name;
		TryGetStringAny(ComponentSpec, { TEXT("name"), TEXT("id") }, Name);
		if (Name.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("component_missing_name");
			return false;
		}
		Name = Name.TrimStartAndEnd();

		UActorComponent* Component = FindComponentByName(Actor, Name);
		FString Operation = GetResourceOperation(ComponentSpec, TEXT("upsert"));
		if (!Component)
		{
			if (Operation == TEXT("patch"))
			{
				OutError = TEXT("component_not_found");
				return false;
			}
			FString ClassPath;
			if (!TryGetStringAny(ComponentSpec, { TEXT("class"), TEXT("class_path") }, ClassPath) || ClassPath.TrimStartAndEnd().IsEmpty())
			{
				OutError = TEXT("component_missing_class");
				return false;
			}
			UClass* ComponentClass = LoadClass<UActorComponent>(nullptr, *ClassPath);
			if (!ComponentClass)
			{
				OutError = TEXT("component_class_not_found");
				return false;
			}
			Component = NewObject<UActorComponent>(Actor, ComponentClass, FName(*Name), RF_Transactional);
			if (!Component)
			{
				OutError = TEXT("component_create_failed");
				return false;
			}
			Actor->AddInstanceComponent(Component);
			Component->OnComponentCreated();
			if (USceneComponent* SceneComponent = Cast<USceneComponent>(Component))
			{
				USceneComponent* Parent = Actor->GetRootComponent();
				FString ParentName;
				if (TryGetStringAny(ComponentSpec, { TEXT("parent") }, ParentName) && !ParentName.IsEmpty())
				{
					Parent = Cast<USceneComponent>(FindComponentByName(Actor, ParentName));
				}
				if (!Parent)
				{
					Actor->SetRootComponent(SceneComponent);
				}
				else
				{
					SceneComponent->AttachToComponent(Parent, FAttachmentTransformRules::KeepRelativeTransform);
				}
			}
			Component->RegisterComponent();
			OutResult->SetStringField(TEXT("create_status"), TEXT("created"));
		}
		else
		{
			OutResult->SetStringField(TEXT("create_status"), TEXT("existing"));
		}

		if (USceneComponent* SceneComponent = Cast<USceneComponent>(Component))
		{
			TSharedPtr<FJsonObject> Transform;
			if (TryGetObject(ComponentSpec, TEXT("transform"), Transform))
			{
				FVector RelativeLocation = SceneComponent->GetRelativeLocation();
				FRotator RelativeRotation = SceneComponent->GetRelativeRotation();
				FVector RelativeScale = SceneComponent->GetRelativeScale3D();
				const bool bHasLocation = TryGetVectorField(Transform, TEXT("relative_location"), RelativeLocation);
				const bool bHasRotation = TryGetRotatorField(Transform, TEXT("relative_rotation"), RelativeRotation);
				const bool bHasScale = TryGetVectorField(Transform, TEXT("relative_scale"), RelativeScale);
				if (bHasLocation || bHasRotation || bHasScale)
				{
					SceneComponent->Modify();
					SceneComponent->SetRelativeLocation(RelativeLocation);
					SceneComponent->SetRelativeRotation(RelativeRotation);
					SceneComponent->SetRelativeScale3D(RelativeScale);
				}
			}
		}

		if (USplineComponent* SplineComponent = Cast<USplineComponent>(Component))
		{
			if (!ApplySplineComponentSpec(SplineComponent, ComponentSpec, OutResult, OutError))
			{
				return false;
			}
		}

		TArray<TSharedPtr<FJsonValue>> PropertyResults;
		TSharedPtr<FJsonObject> Properties;
		if (TryGetObject(ComponentSpec, TEXT("properties"), Properties))
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
			{
				TSharedPtr<FJsonObject> PropertyResult;
				FString PropertyError;
				if (!SetObjectPropertyFromJson(Component, Pair.Key, Pair.Value, World, PropertyResult, PropertyError))
				{
					PropertyResults.Add(MakeShared<FJsonValueObject>(PropertyResult));
					OutError = PropertyError;
					return false;
				}
				PropertyResults.Add(MakeShared<FJsonValueObject>(PropertyResult));
			}
		}
		OutResult->SetStringField(TEXT("name"), Component->GetName());
		OutResult->SetStringField(TEXT("class"), Component->GetClass() ? Component->GetClass()->GetPathName() : FString());
		OutResult->SetArrayField(TEXT("property_results"), PropertyResults);
		return true;
	}

	bool ApplyResource(UWorld* World, const TSharedPtr<FJsonObject>& Content, const TSharedPtr<FJsonObject>& Params, const TSharedPtr<FJsonObject>& Resource, const FString& Mode, const TArray<FString>& ProtectionTags, TMap<FString, AActor*>& ActorById, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
	{
		OutResult = MakeShared<FJsonObject>();
		const FString Id = GetResourceId(Resource);
		const FString Operation = GetResourceOperation(Resource, Mode == TEXT("create_only") ? TEXT("create") : TEXT("upsert"));
		OutResult->SetStringField(TEXT("id"), Id);
		OutResult->SetStringField(TEXT("operation"), Operation);
		OutResult->SetStringField(TEXT("kind"), GetResourceKind(Resource));

		if (!IsSpatialActorResource(Resource))
		{
			OutResult->SetStringField(TEXT("status"), TEXT("report_only_or_unsupported_resource"));
			OutError = TEXT("unsupported_resource_kind");
			return false;
		}

		AActor* Actor = FindActorByNameLabelOrManagedId(World, Id);
		bool bActorCreatedThisCall = false;
		if (Operation == TEXT("delete"))
		{
			if (!Actor)
			{
				OutResult->SetStringField(TEXT("status"), TEXT("missing_noop"));
				return true;
			}
			if (ActorIsProtected(Actor, ProtectionTags))
			{
				OutError = TEXT("actor_protected");
				OutResult->SetStringField(TEXT("status"), TEXT("blocked_by_protection"));
				return false;
			}
			Actor->Modify();
			Actor->Destroy();
			OutResult->SetStringField(TEXT("status"), TEXT("deleted"));
			return true;
		}

		if (Operation == TEXT("clone"))
		{
			FString SourceActorId;
			if (!TryGetStringAny(Resource, { TEXT("source_actor"), TEXT("from_actor") }, SourceActorId) || SourceActorId.IsEmpty())
			{
				OutError = TEXT("clone_missing_source_actor");
				return false;
			}
			AActor* SourceActor = FindActorByNameLabelOrManagedId(World, SourceActorId);
			if (!SourceActor)
			{
				OutError = TEXT("clone_source_actor_not_found");
				return false;
			}
			UEditorActorSubsystem* ActorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;
			if (!ActorSubsystem)
			{
				OutError = TEXT("editor_actor_subsystem_unavailable");
				return false;
			}
			FVector Offset = FVector::ZeroVector;
			TryGetVectorField(Resource, TEXT("offset"), Offset);
			Actor = ActorSubsystem->DuplicateActor(SourceActor, World, Offset);
			if (!Actor)
			{
				OutError = TEXT("clone_failed");
				return false;
			}
			OutResult->SetStringField(TEXT("clone_source"), SourceActorId);
		}
		else if (!Actor)
		{
			if (Mode == TEXT("patch") || Operation == TEXT("patch"))
			{
				OutError = TEXT("actor_not_found_for_patch");
				return false;
			}
			const FString ClassPath = GetClassPathForResource(Resource);
			UClass* ActorClass = LoadClass<AActor>(nullptr, *ClassPath);
			if (!ActorClass)
			{
				OutError = TEXT("actor_class_not_found");
				return false;
			}
			FVector Location = FVector::ZeroVector;
			FRotator Rotation = FRotator::ZeroRotator;
			FVector Scale(1.0, 1.0, 1.0);
			TSharedPtr<FJsonObject> Transform;
			if (TryGetObject(Resource, TEXT("transform"), Transform))
			{
				TryGetVectorField(Transform, TEXT("location"), Location);
				TryGetRotatorField(Transform, TEXT("rotation"), Rotation);
				TryGetVectorField(Transform, TEXT("scale"), Scale);
			}
			FActorSpawnParameters SpawnParams;
			SpawnParams.ObjectFlags |= RF_Transactional;
			SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			Actor = World->SpawnActor<AActor>(ActorClass, FTransform(Rotation, Location, Scale), SpawnParams);
			if (!Actor)
			{
				OutError = TEXT("spawn_failed");
				return false;
			}
			bActorCreatedThisCall = true;
			OutResult->SetStringField(TEXT("create_status"), TEXT("created"));
		}
		else if (Mode == TEXT("create_only") || Operation == TEXT("create"))
		{
			OutError = TEXT("actor_already_exists");
			return false;
		}
		else
		{
			OutResult->SetStringField(TEXT("create_status"), TEXT("existing"));
		}

		if (!Actor)
		{
			OutError = TEXT("actor_unresolved");
			return false;
		}
		if (ActorIsProtected(Actor, ProtectionTags) && (Operation == TEXT("patch") || Operation == TEXT("upsert")))
		{
			OutError = TEXT("actor_protected");
			OutResult->SetStringField(TEXT("status"), TEXT("blocked_by_protection"));
			return false;
		}

		FString Label;
		if (TryGetStringAny(Resource, { TEXT("label") }, Label) && !Label.TrimStartAndEnd().IsEmpty())
		{
			Actor->SetActorLabel(Label.TrimStartAndEnd());
		}
		else if (Operation == TEXT("clone") && !Id.IsEmpty())
		{
			Actor->SetActorLabel(Id);
		}

		FString FolderPath;
		if (!TryGetStringAny(Resource, { TEXT("folder_path") }, FolderPath) || FolderPath.TrimStartAndEnd().IsEmpty())
		{
			FolderPath = GetDefaultFolderPath(Content);
		}
		if (FolderPath.TrimStartAndEnd().IsEmpty())
		{
			FolderPath = GetScopeFolderRoot(Content, Params);
		}
		FolderPath = NormalizeFolder(FolderPath);
		if (!FolderPath.IsEmpty())
		{
			Actor->Modify();
			Actor->SetFolderPath(FName(*FolderPath));
		}

		TArray<FString> Tags = GetDefaultTags(Content);
		Tags.Append(ReadTags(Resource));
		const FString ManagedTag = GetManagedTag(Content, Params);
		if (!ManagedTag.IsEmpty())
		{
			Tags.AddUnique(ManagedTag);
		}
		if (!Id.IsEmpty())
		{
			Tags.AddUnique(MakeManagedIdTag(Id));
		}
		for (const FString& Tag : Tags)
		{
			const FString CleanTag = Tag.TrimStartAndEnd();
			if (!CleanTag.IsEmpty())
			{
				Actor->Tags.AddUnique(FName(*CleanTag));
			}
		}

		ApplyActorTransform(Actor, Resource, OutResult);
		if (!ApplyNavMeshBoundsAdapter(Actor, Resource, World, bActorCreatedThisCall, OutResult, OutError))
		{
			return false;
		}
		if (!ApplyStaticMeshAdapter(Actor, Resource, OutResult, OutError))
		{
			return false;
		}

		TArray<TSharedPtr<FJsonValue>> PropertyResults;
		TSharedPtr<FJsonObject> Properties;
		if (TryGetObject(Resource, TEXT("properties"), Properties))
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
			{
				if (Pair.Key.Equals(TEXT("StaticMesh"), ESearchCase::IgnoreCase))
				{
					continue;
				}
				TSharedPtr<FJsonObject> PropertyResult;
				FString PropertyError;
				if (!SetObjectPropertyFromJson(Actor, Pair.Key, Pair.Value, World, PropertyResult, PropertyError))
				{
					PropertyResults.Add(MakeShared<FJsonValueObject>(PropertyResult));
					OutError = PropertyError;
					return false;
				}
				PropertyResults.Add(MakeShared<FJsonValueObject>(PropertyResult));
			}
		}

		TArray<TSharedPtr<FJsonValue>> ComponentResults;
		const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
		if (TryGetArray(Resource, TEXT("components"), Components) && Components)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Components)
			{
				const TSharedPtr<FJsonObject> ComponentSpec = Value.IsValid() ? Value->AsObject() : nullptr;
				TSharedPtr<FJsonObject> ComponentResult;
				if (!ApplyComponentSpec(Actor, ComponentSpec, World, ComponentResult, OutError))
				{
					ComponentResults.Add(MakeShared<FJsonValueObject>(ComponentResult));
					return false;
				}
				ComponentResults.Add(MakeShared<FJsonValueObject>(ComponentResult));
			}
			ApplyActorTransform(Actor, Resource, OutResult);
		}

		Actor->MarkPackageDirty();
		ActorById.Add(Id, Actor);
		OutResult->SetStringField(TEXT("status"), TEXT("applied"));
		OutResult->SetStringField(TEXT("actor_name"), Actor->GetName());
		OutResult->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
		OutResult->SetStringField(TEXT("object_path"), Actor->GetPathName());
		OutResult->SetStringField(TEXT("folder_path"), Actor->GetFolderPath().ToString());
		OutResult->SetArrayField(TEXT("property_results"), PropertyResults);
		OutResult->SetArrayField(TEXT("component_results"), ComponentResults);
		return true;
	}

	bool ApplyAttachments(UWorld* World, const TArray<TSharedPtr<FJsonObject>>& Resources, const TMap<FString, AActor*>& ActorById, TArray<TSharedPtr<FJsonValue>>& Results, FString& OutError)
	{
		for (const TSharedPtr<FJsonObject>& Resource : Resources)
		{
			const FString Id = GetResourceId(Resource);
			TSharedPtr<FJsonObject> Attachments;
			if (!TryGetObject(Resource, TEXT("attachments"), Attachments))
			{
				continue;
			}
			FString ParentId;
			if (!TryGetStringAny(Attachments, { TEXT("parent_actor"), TEXT("parent_id") }, ParentId) || ParentId.TrimStartAndEnd().IsEmpty())
			{
				continue;
			}
			AActor* Child = ActorById.Contains(Id) ? ActorById[Id] : FindActorByNameLabelOrManagedId(World, Id);
			AActor* Parent = ActorById.Contains(ParentId) ? ActorById[ParentId] : FindActorByNameLabelOrManagedId(World, ParentId);
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("operation"), TEXT("attach"));
			Result->SetStringField(TEXT("child_id"), Id);
			Result->SetStringField(TEXT("parent_id"), ParentId);
			if (!Child || !Parent)
			{
				Result->SetStringField(TEXT("status"), TEXT("actor_not_found"));
				Results.Add(MakeShared<FJsonValueObject>(Result));
				OutError = TEXT("attachment_actor_not_found");
				return false;
			}
			Child->Modify();
			Child->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform);
			Result->SetStringField(TEXT("status"), TEXT("attached"));
			Results.Add(MakeShared<FJsonValueObject>(Result));
		}
		return true;
	}

	bool DeleteManagedMissing(UWorld* World, const TSharedPtr<FJsonObject>& Content, const TSharedPtr<FJsonObject>& Params, const TSet<FString>& TargetIds, const TArray<FString>& ProtectionTags, bool bDryRun, TArray<TSharedPtr<FJsonValue>>& Results, FString& OutError)
	{
		const FString FolderRoot = GetScopeFolderRoot(Content, Params);
		const FString ManagedTag = GetManagedTag(Content, Params);
		if (FolderRoot.IsEmpty() && ManagedTag.IsEmpty())
		{
			OutError = TEXT("delete_scope_requires_folder_or_managed_tag");
			return false;
		}

		TArray<AActor*> ToDelete;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor || Actor->IsActorBeingDestroyed())
			{
				continue;
			}
			if (!FolderMatches(Actor->GetFolderPath().ToString(), FolderRoot, true))
			{
				continue;
			}
			if (!ManagedTag.IsEmpty() && !ActorHasTagString(Actor, ManagedTag))
			{
				continue;
			}
			const FString Id = GetManagedId(Actor);
			if (!Id.IsEmpty() && TargetIds.Contains(Id))
			{
				continue;
			}
			if (ActorIsProtected(Actor, ProtectionTags))
			{
				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("operation"), TEXT("delete_missing"));
				Item->SetStringField(TEXT("status"), TEXT("blocked_by_protection"));
				Item->SetStringField(TEXT("actor"), Actor->GetActorLabel());
				Results.Add(MakeShared<FJsonValueObject>(Item));
				continue;
			}
			ToDelete.Add(Actor);
		}

		for (AActor* Actor : ToDelete)
		{
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("operation"), TEXT("delete_missing"));
			Item->SetStringField(TEXT("actor"), Actor->GetActorLabel());
			Item->SetStringField(TEXT("object_path"), Actor->GetPathName());
			if (bDryRun)
			{
				Item->SetStringField(TEXT("status"), TEXT("dry_run"));
			}
			else
			{
				Actor->Modify();
				Actor->Destroy();
				Item->SetStringField(TEXT("status"), TEXT("deleted"));
			}
			Results.Add(MakeShared<FJsonValueObject>(Item));
		}
		return true;
	}

	AActor* ResolveOperationActor(UWorld* World, const TSharedPtr<FJsonObject>& Op, std::initializer_list<const TCHAR*> Keys, FString& OutRef)
	{
		OutRef.Reset();
		if (!TryGetStringAny(Op, Keys, OutRef) || OutRef.TrimStartAndEnd().IsEmpty())
		{
			return nullptr;
		}
		OutRef = OutRef.TrimStartAndEnd();
		return FindActorByNameLabelOrManagedId(World, OutRef);
	}

	TArray<FString> ReadOperationTags(const TSharedPtr<FJsonObject>& Op)
	{
		TArray<FString> Tags = ReadStringArray(Op, TEXT("tags"));
		FString Tag;
		if (TryGetStringAny(Op, { TEXT("tag") }, Tag) && !Tag.TrimStartAndEnd().IsEmpty())
		{
			Tags.Add(Tag.TrimStartAndEnd());
		}
		return Tags;
	}

	void ApplyOperationPatchToActor(AActor* Actor, UWorld* World, const TSharedPtr<FJsonObject>& Patch, TSharedPtr<FJsonObject>& Result, FString& OutError)
	{
		if (!Actor || !Patch.IsValid())
		{
			return;
		}
		ApplyActorTransform(Actor, Patch, Result);
		FString FolderPath;
		if (TryGetStringAny(Patch, { TEXT("folder_path"), TEXT("folder"), TEXT("target_folder") }, FolderPath) && !FolderPath.TrimStartAndEnd().IsEmpty())
		{
			Actor->Modify();
			Actor->SetFolderPath(FName(*NormalizeFolder(FolderPath)));
			Result->SetStringField(TEXT("folder_path"), Actor->GetFolderPath().ToString());
		}
		for (const FString& Tag : ReadOperationTags(Patch))
		{
			const FString CleanTag = Tag.TrimStartAndEnd();
			if (!CleanTag.IsEmpty())
			{
				Actor->Tags.AddUnique(FName(*CleanTag));
			}
		}
		TSharedPtr<FJsonObject> Properties;
		if (TryGetObject(Patch, TEXT("properties"), Properties))
		{
			TArray<TSharedPtr<FJsonValue>> PropertyResults;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
			{
				TSharedPtr<FJsonObject> PropertyResult;
				FString PropertyError;
				if (!SetObjectPropertyFromJson(Actor, Pair.Key, Pair.Value, World, PropertyResult, PropertyError))
				{
					PropertyResults.Add(MakeShared<FJsonValueObject>(PropertyResult));
					OutError = PropertyError;
					Result->SetArrayField(TEXT("property_results"), PropertyResults);
					return;
				}
				PropertyResults.Add(MakeShared<FJsonValueObject>(PropertyResult));
			}
			Result->SetArrayField(TEXT("property_results"), PropertyResults);
		}
	}

	bool ExecuteOperations(UWorld* World, const TSharedPtr<FJsonObject>& Content, const TSharedPtr<FJsonObject>& Params, bool bDryRun, const TArray<FString>& ProtectionTags, TArray<TSharedPtr<FJsonValue>>& Results, FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
		if (!Content.IsValid() || !TryGetArray(Content, TEXT("operations"), Operations) || !Operations)
		{
			return true;
		}
		for (int32 Index = 0; Index < Operations->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Op = (*Operations)[Index].IsValid() ? (*Operations)[Index]->AsObject() : nullptr;
			if (!Op.IsValid())
			{
				continue;
			}
			FString Type;
			TryGetStringAny(Op, { TEXT("type"), TEXT("operation") }, Type);
			Type = Type.ToLower();
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("operation"), Type);
			Result->SetNumberField(TEXT("operation_index"), Index);

			if (Type == TEXT("protect"))
			{
				Result->SetStringField(TEXT("status"), TEXT("active"));
			}
			else if (Type == TEXT("delete"))
			{
				FString ActorRef;
				AActor* Actor = ResolveOperationActor(World, Op, { TEXT("actor"), TEXT("id"), TEXT("actor_id"), TEXT("target_actor") }, ActorRef);
				Result->SetStringField(TEXT("actor_ref"), ActorRef);
				if (!Actor)
				{
					OutError = TEXT("operation_actor_not_found");
					Result->SetStringField(TEXT("status"), TEXT("actor_not_found"));
					Results.Add(MakeShared<FJsonValueObject>(Result));
					return false;
				}
				if (ActorIsProtected(Actor, ProtectionTags))
				{
					OutError = TEXT("actor_protected");
					Result->SetStringField(TEXT("status"), TEXT("blocked_by_protection"));
					Results.Add(MakeShared<FJsonValueObject>(Result));
					return false;
				}
				if (bDryRun)
				{
					Result->SetStringField(TEXT("status"), TEXT("dry_run"));
				}
				else
				{
					Result->SetStringField(TEXT("actor"), Actor->GetActorLabel());
					Actor->Modify();
					Actor->Destroy();
					Result->SetStringField(TEXT("status"), TEXT("deleted"));
				}
			}
			else if (Type == TEXT("clone"))
			{
				FString SourceRef;
				AActor* SourceActor = ResolveOperationActor(World, Op, { TEXT("from_actor"), TEXT("source_actor"), TEXT("source_id"), TEXT("template_id") }, SourceRef);
				FString TargetId;
				TryGetStringAny(Op, { TEXT("id"), TEXT("target_id"), TEXT("new_id") }, TargetId);
				TargetId = TargetId.TrimStartAndEnd();
				Result->SetStringField(TEXT("source_actor"), SourceRef);
				Result->SetStringField(TEXT("id"), TargetId);
				if (!SourceActor)
				{
					OutError = TEXT("clone_source_actor_not_found");
					Result->SetStringField(TEXT("status"), TEXT("source_actor_not_found"));
					Results.Add(MakeShared<FJsonValueObject>(Result));
					return false;
				}
				if (TargetId.IsEmpty())
				{
					OutError = TEXT("clone_missing_target_id");
					Result->SetStringField(TEXT("status"), TEXT("missing_target_id"));
					Results.Add(MakeShared<FJsonValueObject>(Result));
					return false;
				}
				if (FindActorByNameLabelOrManagedId(World, TargetId))
				{
					OutError = TEXT("clone_target_already_exists");
					Result->SetStringField(TEXT("status"), TEXT("target_already_exists"));
					Results.Add(MakeShared<FJsonValueObject>(Result));
					return false;
				}
				if (bDryRun)
				{
					Result->SetStringField(TEXT("status"), TEXT("dry_run"));
				}
				else
				{
					UEditorActorSubsystem* ActorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;
					if (!ActorSubsystem)
					{
						OutError = TEXT("editor_actor_subsystem_unavailable");
						Result->SetStringField(TEXT("status"), TEXT("editor_actor_subsystem_unavailable"));
						Results.Add(MakeShared<FJsonValueObject>(Result));
						return false;
					}
					FVector Offset = FVector::ZeroVector;
					TryGetVectorField(Op, TEXT("offset"), Offset);
					AActor* ClonedActor = ActorSubsystem->DuplicateActor(SourceActor, World, Offset);
					if (!ClonedActor)
					{
						OutError = TEXT("clone_failed");
						Result->SetStringField(TEXT("status"), TEXT("clone_failed"));
						Results.Add(MakeShared<FJsonValueObject>(Result));
						return false;
					}
					FString Label;
					if (TryGetStringAny(Op, { TEXT("label") }, Label) && !Label.TrimStartAndEnd().IsEmpty())
					{
						ClonedActor->SetActorLabel(Label.TrimStartAndEnd());
					}
					else
					{
						ClonedActor->SetActorLabel(TargetId);
					}
					const FString ManagedTag = GetManagedTag(Content, Params);
					if (!ManagedTag.IsEmpty())
					{
						ClonedActor->Tags.AddUnique(FName(*ManagedTag));
					}
					ClonedActor->Tags.AddUnique(FName(*MakeManagedIdTag(TargetId)));
					for (const FString& Tag : ReadOperationTags(Op))
					{
						if (!Tag.TrimStartAndEnd().IsEmpty())
						{
							ClonedActor->Tags.AddUnique(FName(*Tag.TrimStartAndEnd()));
						}
					}
					ApplyOperationPatchToActor(ClonedActor, World, Op, Result, OutError);
					TSharedPtr<FJsonObject> Patch;
					if (OutError.IsEmpty() && TryGetObject(Op, TEXT("patch"), Patch))
					{
						ApplyOperationPatchToActor(ClonedActor, World, Patch, Result, OutError);
					}
					if (!OutError.IsEmpty())
					{
						Result->SetStringField(TEXT("status"), TEXT("patch_failed"));
						Results.Add(MakeShared<FJsonValueObject>(Result));
						return false;
					}
					ClonedActor->MarkPackageDirty();
					Result->SetStringField(TEXT("status"), TEXT("cloned"));
					Result->SetStringField(TEXT("actor"), ClonedActor->GetActorLabel());
					Result->SetStringField(TEXT("object_path"), ClonedActor->GetPathName());
				}
			}
			else if (Type == TEXT("attach"))
			{
				FString ChildRef;
				FString ParentRef;
				AActor* Child = ResolveOperationActor(World, Op, { TEXT("child"), TEXT("child_actor"), TEXT("child_id"), TEXT("actor"), TEXT("id"), TEXT("source_actor") }, ChildRef);
				AActor* Parent = ResolveOperationActor(World, Op, { TEXT("parent"), TEXT("parent_actor"), TEXT("parent_id"), TEXT("target_actor"), TEXT("target") }, ParentRef);
				Result->SetStringField(TEXT("child_ref"), ChildRef);
				Result->SetStringField(TEXT("parent_ref"), ParentRef);
				if (!Child || !Parent)
				{
					OutError = TEXT("attachment_actor_not_found");
					Result->SetStringField(TEXT("status"), TEXT("actor_not_found"));
					Results.Add(MakeShared<FJsonValueObject>(Result));
					return false;
				}
				if (bDryRun)
				{
					Result->SetStringField(TEXT("status"), TEXT("dry_run"));
				}
				else
				{
					Child->Modify();
					Child->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform);
					Result->SetStringField(TEXT("status"), TEXT("attached"));
				}
			}
			else if (Type == TEXT("detach"))
			{
				FString ActorRef;
				AActor* Actor = ResolveOperationActor(World, Op, { TEXT("actor"), TEXT("child"), TEXT("child_actor"), TEXT("id"), TEXT("actor_id") }, ActorRef);
				Result->SetStringField(TEXT("actor_ref"), ActorRef);
				if (!Actor)
				{
					OutError = TEXT("operation_actor_not_found");
					Result->SetStringField(TEXT("status"), TEXT("actor_not_found"));
					Results.Add(MakeShared<FJsonValueObject>(Result));
					return false;
				}
				if (bDryRun)
				{
					Result->SetStringField(TEXT("status"), TEXT("dry_run"));
				}
				else
				{
					Actor->Modify();
					Actor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
					Result->SetStringField(TEXT("status"), TEXT("detached"));
				}
			}
			else if (Type == TEXT("set_folder"))
			{
				FString ActorRef;
				AActor* Actor = ResolveOperationActor(World, Op, { TEXT("actor"), TEXT("id"), TEXT("actor_id"), TEXT("target_actor") }, ActorRef);
				FString FolderPath;
				TryGetStringAny(Op, { TEXT("folder_path"), TEXT("folder"), TEXT("target_folder") }, FolderPath);
				Result->SetStringField(TEXT("actor_ref"), ActorRef);
				Result->SetStringField(TEXT("folder_path"), NormalizeFolder(FolderPath));
				if (!Actor || FolderPath.TrimStartAndEnd().IsEmpty())
				{
					OutError = !Actor ? TEXT("operation_actor_not_found") : TEXT("folder_path_missing");
					Result->SetStringField(TEXT("status"), !Actor ? TEXT("actor_not_found") : TEXT("folder_path_missing"));
					Results.Add(MakeShared<FJsonValueObject>(Result));
					return false;
				}
				if (bDryRun)
				{
					Result->SetStringField(TEXT("status"), TEXT("dry_run"));
				}
				else
				{
					Actor->Modify();
					Actor->SetFolderPath(FName(*NormalizeFolder(FolderPath)));
					Result->SetStringField(TEXT("status"), TEXT("folder_set"));
				}
			}
			else if (Type == TEXT("add_tag") || Type == TEXT("remove_tag"))
			{
				FString ActorRef;
				AActor* Actor = ResolveOperationActor(World, Op, { TEXT("actor"), TEXT("id"), TEXT("actor_id"), TEXT("target_actor") }, ActorRef);
				TArray<FString> Tags = ReadOperationTags(Op);
				Result->SetStringField(TEXT("actor_ref"), ActorRef);
				TArray<TSharedPtr<FJsonValue>> TagValues;
				for (const FString& Tag : Tags)
				{
					TagValues.Add(MakeShared<FJsonValueString>(Tag));
				}
				Result->SetArrayField(TEXT("tags"), TagValues);
				if (!Actor || Tags.Num() == 0)
				{
					OutError = !Actor ? TEXT("operation_actor_not_found") : TEXT("operation_tags_missing");
					Result->SetStringField(TEXT("status"), !Actor ? TEXT("actor_not_found") : TEXT("tags_missing"));
					Results.Add(MakeShared<FJsonValueObject>(Result));
					return false;
				}
				if (bDryRun)
				{
					Result->SetStringField(TEXT("status"), TEXT("dry_run"));
				}
				else
				{
					Actor->Modify();
					for (const FString& Tag : Tags)
					{
						const FString CleanTag = Tag.TrimStartAndEnd();
						if (CleanTag.IsEmpty())
						{
							continue;
						}
						if (Type == TEXT("add_tag"))
						{
							Actor->Tags.AddUnique(FName(*CleanTag));
						}
						else
						{
							Actor->Tags.Remove(FName(*CleanTag));
						}
					}
					Result->SetStringField(TEXT("status"), Type == TEXT("add_tag") ? TEXT("tag_added") : TEXT("tag_removed"));
				}
			}
			else if (Type == TEXT("align"))
			{
				FString SourceRef;
				FString TargetRef;
				AActor* SourceActor = ResolveOperationActor(World, Op, { TEXT("source_actor"), TEXT("source_id"), TEXT("actor"), TEXT("id") }, SourceRef);
				AActor* TargetActor = ResolveOperationActor(World, Op, { TEXT("target_actor"), TEXT("target_id"), TEXT("target") }, TargetRef);
				FString Method = TEXT("copy_location");
				TryGetStringAny(Op, { TEXT("method") }, Method);
				Method = Method.TrimStartAndEnd().ToLower();
				Result->SetStringField(TEXT("source_actor"), SourceRef);
				Result->SetStringField(TEXT("target_actor"), TargetRef);
				Result->SetStringField(TEXT("method"), Method);
				if (!SourceActor || !TargetActor)
				{
					OutError = TEXT("align_actor_not_found");
					Result->SetStringField(TEXT("status"), TEXT("actor_not_found"));
					Results.Add(MakeShared<FJsonValueObject>(Result));
					return false;
				}
				FVector Offset = FVector::ZeroVector;
				TryGetVectorField(Op, TEXT("offset"), Offset);
				FVector TargetLocation = TargetActor->GetActorLocation();
				if (Method == TEXT("bounds_center") || Method == TEXT("by_bounds") || Method == TEXT("actor_bounds_center"))
				{
					const FBox TargetBounds = TargetActor->GetComponentsBoundingBox(true);
					if (TargetBounds.IsValid != 0)
					{
						TargetLocation = TargetBounds.GetCenter();
					}
				}
				else if (!(Method == TEXT("copy_location") || Method == TEXT("actor_to_actor") || Method == TEXT("location")))
				{
					Result->SetStringField(TEXT("status"), TEXT("report_only"));
					Result->SetStringField(TEXT("reason"), TEXT("complex face/vertex/surface alignment remains delegated to dedicated geometry alignment commands"));
					Results.Add(MakeShared<FJsonValueObject>(Result));
					continue;
				}
				if (bDryRun)
				{
					Result->SetStringField(TEXT("status"), TEXT("dry_run"));
				}
				else
				{
					SourceActor->Modify();
					SourceActor->SetActorLocation(TargetLocation + Offset, false, nullptr, ETeleportType::TeleportPhysics);
					Result->SetStringField(TEXT("status"), TEXT("aligned"));
					Result->SetObjectField(TEXT("location"), MakeVectorJson(SourceActor->GetActorLocation()));
				}
			}
			else if (Type == TEXT("screenshot") || Type == TEXT("evidence"))
			{
				Result->SetStringField(TEXT("status"), TEXT("report_only_visual_capture"));
				Result->SetStringField(TEXT("reason"), TEXT("use capture.image.execute with the exact viewport policy"));
			}
			else if (Type == TEXT("convert") || Type == TEXT("migrate") || Type == TEXT("convert_report"))
			{
				Result->SetStringField(TEXT("status"), TEXT("report_only"));
				Result->SetStringField(TEXT("reason"), TEXT("destructive conversion requires dedicated adapter and explicit resource ownership"));
			}
			else if (Type == TEXT("remap_report"))
			{
				Result->SetStringField(TEXT("status"), TEXT("report_only"));
				Result->SetStringField(TEXT("reason"), TEXT("remap_report is diagnostic-only; use remap or retarget for bounded property text remap"));
			}
			else if (Type == TEXT("remap") || Type == TEXT("retarget"))
			{
				FString PropertyPath;
				FString FromText;
				FString ToText;
				TryGetStringAny(Op, { TEXT("property_path"), TEXT("property") }, PropertyPath);
				TryGetStringAny(Op, { TEXT("from"), TEXT("from_text") }, FromText);
				TryGetStringAny(Op, { TEXT("to"), TEXT("to_text") }, ToText);
				double MaxTouchedD = 100.0;
				Op->TryGetNumberField(TEXT("max_resources_touched"), MaxTouchedD);
				const int32 MaxTouched = FMath::Max(0, (int32)MaxTouchedD);
				int32 Matched = 0;
				int32 Touched = 0;
				const FString FolderRoot = GetScopeFolderRoot(Content, Params);
				const FString ManagedTag = GetManagedTag(Content, Params);
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					AActor* Actor = *It;
					if (!Actor || !FolderMatches(Actor->GetFolderPath().ToString(), FolderRoot, true) || !ActorHasTagString(Actor, ManagedTag) || ActorIsProtected(Actor, ProtectionTags))
					{
						continue;
					}
					FString ReadBack;
					FString PropertyClass;
					if (!ExportPropertyText(Actor, PropertyPath, ReadBack, PropertyClass) || !ReadBack.Contains(FromText))
					{
						continue;
					}
					++Matched;
					if (Matched > MaxTouched)
					{
						Result->SetStringField(TEXT("status"), TEXT("degraded_to_report"));
						Result->SetStringField(TEXT("reason"), TEXT("max_resources_touched_exceeded"));
						Result->SetNumberField(TEXT("matched"), Matched);
						Results.Add(MakeShared<FJsonValueObject>(Result));
						return true;
					}
					if (!bDryRun)
					{
						const FString NewText = ReadBack.Replace(*FromText, *ToText, ESearchCase::CaseSensitive);
						TSharedPtr<FJsonObject> PropertyResult;
						FString PropertyError;
						if (!SetObjectPropertyFromJson(Actor, PropertyPath, MakeShared<FJsonValueString>(NewText), World, PropertyResult, PropertyError))
						{
							OutError = PropertyError;
							return false;
						}
					}
					++Touched;
				}
				Result->SetStringField(TEXT("status"), bDryRun ? TEXT("dry_run") : TEXT("applied"));
				Result->SetNumberField(TEXT("matched"), Matched);
				Result->SetNumberField(TEXT("touched"), Touched);
			}
			else if (Type == TEXT("repair") || Type == TEXT("repair_report"))
			{
				Result->SetStringField(TEXT("status"), TEXT("delegated_to_level_content_repair_json"));
			}
			else if (!Type.IsEmpty())
			{
				Result->SetStringField(TEXT("status"), TEXT("unsupported_operation"));
			}
			Results.Add(MakeShared<FJsonValueObject>(Result));
		}
		return true;
	}

	bool ExecuteTasks(UWorld* World, const TSharedPtr<FJsonObject>& Content, bool bDryRun, TArray<TSharedPtr<FJsonValue>>& Results, FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Tasks = nullptr;
		if (!Content.IsValid() || !TryGetArray(Content, TEXT("tasks"), Tasks) || !Tasks)
		{
			return true;
		}
		for (int32 Index = 0; Index < Tasks->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Task = (*Tasks)[Index].IsValid() ? (*Tasks)[Index]->AsObject() : nullptr;
			if (!Task.IsValid())
			{
				continue;
			}
			FString Type;
			TryGetStringAny(Task, { TEXT("type"), TEXT("kind"), TEXT("task") }, Type);
			Type = Type.ToLower();
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("task"), Type);
			Result->SetNumberField(TEXT("task_index"), Index);
			if (Type == TEXT("navmesh_build"))
			{
				if (bDryRun)
				{
					Result->SetStringField(TEXT("status"), TEXT("dry_run"));
				}
				else if (UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World))
				{
					NavSys->Build();
					Result->SetStringField(TEXT("status"), TEXT("executed"));
				}
				else
				{
					Result->SetStringField(TEXT("status"), TEXT("feature_unavailable"));
					Result->SetStringField(TEXT("reason"), TEXT("navigation_system_unavailable"));
				}
			}
			else if (Type == TEXT("cleanup_empty_folders"))
			{
				Result->SetStringField(TEXT("status"), bDryRun ? TEXT("dry_run") : TEXT("handled_by_repair_or_existing_folder_cleanup"));
			}
			else if (Type == TEXT("hlod_build") || Type == TEXT("build_hlod") || Type == TEXT("lighting_build") || Type == TEXT("reflection_capture_update") || Type == TEXT("pcg_generate") ||
				Type == TEXT("load_streaming_level_for_edit") || Type == TEXT("unload_streaming_level_after_apply"))
			{
				Result->SetStringField(TEXT("status"), TEXT("feature_unavailable"));
				Result->SetStringField(TEXT("reason"), TEXT("gated_build_task_report_only"));
			}
			else
			{
				Result->SetStringField(TEXT("status"), TEXT("unsupported_task"));
			}
			Results.Add(MakeShared<FJsonValueObject>(Result));
		}
		return true;
	}

	ECollisionChannel CollisionChannelFromName(const FString& ChannelName, ECollisionChannel DefaultChannel)
	{
		const FString Clean = ChannelName.TrimStartAndEnd().ToLower();
		if (Clean == TEXT("camera"))
		{
			return ECC_Camera;
		}
		if (Clean == TEXT("pawn"))
		{
			return ECC_Pawn;
		}
		if (Clean == TEXT("worldstatic") || Clean == TEXT("world_static"))
		{
			return ECC_WorldStatic;
		}
		if (Clean == TEXT("worlddynamic") || Clean == TEXT("world_dynamic"))
		{
			return ECC_WorldDynamic;
		}
		if (Clean == TEXT("physicsbody") || Clean == TEXT("physics_body"))
		{
			return ECC_PhysicsBody;
		}
		return DefaultChannel;
	}

	bool ExecuteValidations(UWorld* World, const TSharedPtr<FJsonObject>& Content, TArray<TSharedPtr<FJsonValue>>& Results)
	{
		const TArray<TSharedPtr<FJsonValue>>* Validations = nullptr;
		if (!Content.IsValid() || !TryGetArray(Content, TEXT("validation"), Validations) || !Validations)
		{
			return true;
		}
		for (int32 Index = 0; Index < Validations->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Validation = (*Validations)[Index].IsValid() ? (*Validations)[Index]->AsObject() : nullptr;
			if (!Validation.IsValid())
			{
				continue;
			}
			FString Type;
			TryGetStringAny(Validation, { TEXT("type"), TEXT("kind") }, Type);
			Type = Type.ToLower();
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("validation"), Type);
			Result->SetNumberField(TEXT("validation_index"), Index);
			if (Type == TEXT("actor_exists"))
			{
				FString Id;
				TryGetStringAny(Validation, { TEXT("id"), TEXT("actor_id"), TEXT("actor") }, Id);
				AActor* Actor = FindActorByNameLabelOrManagedId(World, Id);
				Result->SetBoolField(TEXT("passed"), Actor != nullptr);
				Result->SetStringField(TEXT("actor"), Actor ? Actor->GetActorLabel() : FString());
			}
			else if (Type == TEXT("property_readback"))
			{
				FString Id;
				FString PropertyPath;
				TryGetStringAny(Validation, { TEXT("id"), TEXT("actor_id") }, Id);
				TryGetStringAny(Validation, { TEXT("property"), TEXT("property_path") }, PropertyPath);
				AActor* Actor = FindActorByNameLabelOrManagedId(World, Id);
				FString ReadBack;
				FString PropertyClass;
				const bool bOk = Actor && ExportPropertyText(Actor, PropertyPath, ReadBack, PropertyClass);
				Result->SetBoolField(TEXT("passed"), bOk);
				Result->SetStringField(TEXT("value_text"), ReadBack);
			}
			else if (Type == TEXT("trace") || Type == TEXT("line_trace"))
			{
				FVector Start;
				FVector End;
				if (!TryGetVectorField(Validation, TEXT("start"), Start) || !TryGetVectorField(Validation, TEXT("end"), End))
				{
					Result->SetBoolField(TEXT("passed"), false);
					Result->SetStringField(TEXT("status"), TEXT("missing_start_or_end"));
					Results.Add(MakeShared<FJsonValueObject>(Result));
					continue;
				}
				FString ChannelName = TEXT("visibility");
				TryGetStringAny(Validation, { TEXT("trace_channel"), TEXT("channel") }, ChannelName);
				FHitResult Hit;
				FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(UeAgentLevelContentTraceValidation), true);
				const bool bHit = World && World->LineTraceSingleByChannel(Hit, Start, End, CollisionChannelFromName(ChannelName, ECC_Visibility), QueryParams);
				bool bExpectedHit = false;
				const bool bHasExpectedHit = TryGetBoolAny(Validation, { TEXT("expected_hit"), TEXT("should_hit") }, bExpectedHit);
				Result->SetBoolField(TEXT("hit"), bHit);
				Result->SetBoolField(TEXT("passed"), !bHasExpectedHit || bHit == bExpectedHit);
				Result->SetObjectField(TEXT("start"), MakeVectorJson(Start));
				Result->SetObjectField(TEXT("end"), MakeVectorJson(End));
				if (bHit)
				{
					Result->SetStringField(TEXT("hit_actor"), Hit.GetActor() ? Hit.GetActor()->GetActorLabel() : FString());
					Result->SetObjectField(TEXT("hit_location"), MakeVectorJson(Hit.ImpactPoint));
				}
			}
			else if (Type == TEXT("overlap") || Type == TEXT("box_overlap"))
			{
				FVector Center;
				FVector Extent(50.0, 50.0, 50.0);
				if (!TryGetVectorField(Validation, TEXT("center"), Center))
				{
					Result->SetBoolField(TEXT("passed"), false);
					Result->SetStringField(TEXT("status"), TEXT("missing_center"));
					Results.Add(MakeShared<FJsonValueObject>(Result));
					continue;
				}
				TryGetVectorField(Validation, TEXT("extent"), Extent);
				TArray<FOverlapResult> Overlaps;
				FCollisionObjectQueryParams ObjectParams;
				ObjectParams.AddObjectTypesToQuery(ECC_WorldStatic);
				ObjectParams.AddObjectTypesToQuery(ECC_WorldDynamic);
				ObjectParams.AddObjectTypesToQuery(ECC_PhysicsBody);
				ObjectParams.AddObjectTypesToQuery(ECC_Pawn);
				FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(UeAgentLevelContentOverlapValidation), true);
				const bool bHasBlockingOverlap = World && World->OverlapMultiByObjectType(Overlaps, Center, FQuat::Identity, ObjectParams, FCollisionShape::MakeBox(Extent), QueryParams);
				TSet<FString> ActorLabels;
				for (const FOverlapResult& Overlap : Overlaps)
				{
					if (AActor* Actor = Overlap.GetActor())
					{
						ActorLabels.Add(Actor->GetActorLabel());
					}
				}
				double MinCount = -1.0;
				double MaxCount = -1.0;
				Validation->TryGetNumberField(TEXT("min_count"), MinCount);
				Validation->TryGetNumberField(TEXT("max_count"), MaxCount);
				const int32 Count = ActorLabels.Num() > 0 ? ActorLabels.Num() : Overlaps.Num();
				bool bPassed = true;
				if (MinCount >= 0.0)
				{
					bPassed = bPassed && Count >= (int32)MinCount;
				}
				if (MaxCount >= 0.0)
				{
					bPassed = bPassed && Count <= (int32)MaxCount;
				}
				Result->SetBoolField(TEXT("has_overlap"), bHasBlockingOverlap);
				Result->SetNumberField(TEXT("overlap_count"), Count);
				Result->SetBoolField(TEXT("passed"), bPassed);
			}
			else if (Type == TEXT("sweep") || Type == TEXT("capsule_sweep") || Type == TEXT("sweep_capsule"))
			{
				FVector Start;
				FVector End;
				if (!TryGetVectorField(Validation, TEXT("start"), Start) || !TryGetVectorField(Validation, TEXT("end"), End))
				{
					Result->SetBoolField(TEXT("passed"), false);
					Result->SetStringField(TEXT("status"), TEXT("missing_start_or_end"));
					Results.Add(MakeShared<FJsonValueObject>(Result));
					continue;
				}
				double Radius = 34.0;
				double HalfHeight = 88.0;
				Validation->TryGetNumberField(TEXT("radius"), Radius);
				Validation->TryGetNumberField(TEXT("half_height"), HalfHeight);
				TArray<FHitResult> Hits;
				FCollisionObjectQueryParams ObjectParams;
				ObjectParams.AddObjectTypesToQuery(ECC_WorldStatic);
				ObjectParams.AddObjectTypesToQuery(ECC_WorldDynamic);
				FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(UeAgentLevelContentSweepValidation), true);
				const bool bHit = World && World->SweepMultiByObjectType(Hits, Start, End, FQuat::Identity, ObjectParams, FCollisionShape::MakeCapsule((float)Radius, (float)HalfHeight), QueryParams);
				bool bExpectedHit = false;
				const bool bHasExpectedHit = TryGetBoolAny(Validation, { TEXT("expected_hit"), TEXT("should_hit") }, bExpectedHit);
				Result->SetBoolField(TEXT("hit"), bHit);
				Result->SetNumberField(TEXT("hit_count"), Hits.Num());
				Result->SetBoolField(TEXT("passed"), !bHasExpectedHit || bHit == bExpectedHit);
			}
			else if (Type == TEXT("nav_path") || Type == TEXT("navmesh_path") || Type == TEXT("nav_find_path"))
			{
				FVector Start;
				FVector End;
				if (!TryGetVectorField(Validation, TEXT("start"), Start) || !TryGetVectorField(Validation, TEXT("end"), End))
				{
					Result->SetBoolField(TEXT("passed"), false);
					Result->SetStringField(TEXT("status"), TEXT("missing_start_or_end"));
					Results.Add(MakeShared<FJsonValueObject>(Result));
					continue;
				}
				UNavigationSystemV1* NavSys = World ? FNavigationSystem::GetCurrent<UNavigationSystemV1>(World) : nullptr;
				if (!NavSys)
				{
					Result->SetBoolField(TEXT("passed"), false);
					Result->SetStringField(TEXT("status"), TEXT("feature_unavailable"));
					Result->SetStringField(TEXT("reason"), TEXT("navigation_system_unavailable"));
					Results.Add(MakeShared<FJsonValueObject>(Result));
					continue;
				}
				UNavigationPath* Path = UNavigationSystemV1::FindPathToLocationSynchronously(World, Start, End);
				const bool bPathFound = Path && Path->IsValid();
				bool bExpectedPath = false;
				const bool bHasExpectedPath = TryGetBoolAny(Validation, { TEXT("expected_path_found"), TEXT("should_find_path") }, bExpectedPath);
				Result->SetBoolField(TEXT("path_found"), bPathFound);
				Result->SetBoolField(TEXT("partial"), Path ? Path->IsPartial() : false);
				Result->SetNumberField(TEXT("point_count"), Path ? Path->PathPoints.Num() : 0);
				Result->SetBoolField(TEXT("passed"), !bHasExpectedPath || bPathFound == bExpectedPath);
			}
			else if (Type == TEXT("streaming_loaded") || Type == TEXT("streaming_state"))
			{
				FString LevelName;
				TryGetStringAny(Validation, { TEXT("level"), TEXT("level_name"), TEXT("package"), TEXT("name") }, LevelName);
				bool bFound = false;
				bool bLoaded = false;
				bool bVisible = false;
				if (World)
				{
					for (ULevelStreaming* StreamingLevel : World->GetStreamingLevels())
					{
						if (!StreamingLevel)
						{
							continue;
						}
						const FString PackageName = StreamingLevel->GetWorldAssetPackageName();
						if (LevelName.IsEmpty() || PackageName.Contains(LevelName, ESearchCase::IgnoreCase) || StreamingLevel->GetName().Contains(LevelName, ESearchCase::IgnoreCase))
						{
							bFound = true;
							bLoaded = StreamingLevel->IsLevelLoaded();
							bVisible = StreamingLevel->IsLevelVisible();
							Result->SetStringField(TEXT("matched_level"), PackageName);
							break;
						}
					}
				}
				bool bExpectedLoaded = false;
				bool bExpectedVisible = false;
				const bool bHasExpectedLoaded = TryGetBoolAny(Validation, { TEXT("expected_loaded"), TEXT("loaded") }, bExpectedLoaded);
				const bool bHasExpectedVisible = TryGetBoolAny(Validation, { TEXT("expected_visible"), TEXT("visible") }, bExpectedVisible);
				bool bPassed = bFound;
				if (bHasExpectedLoaded)
				{
					bPassed = bPassed && bLoaded == bExpectedLoaded;
				}
				if (bHasExpectedVisible)
				{
					bPassed = bPassed && bVisible == bExpectedVisible;
				}
				Result->SetBoolField(TEXT("found"), bFound);
				Result->SetBoolField(TEXT("loaded"), bLoaded);
				Result->SetBoolField(TEXT("visible"), bVisible);
				Result->SetBoolField(TEXT("passed"), bPassed);
				if (!bFound)
				{
					Result->SetStringField(TEXT("status"), TEXT("level_not_found"));
				}
			}
			else if (Type == TEXT("data_layer_membership"))
			{
				FString Id;
				TryGetStringAny(Validation, { TEXT("id"), TEXT("actor_id"), TEXT("actor") }, Id);
				AActor* Actor = FindActorByNameLabelOrManagedId(World, Id);
				TArray<FString> ExpectedLayers = ReadStringArray(Validation, TEXT("expected_layers"));
				ExpectedLayers.Append(ReadStringArray(Validation, TEXT("data_layers")));
				FString SingleLayer;
				if (TryGetStringAny(Validation, { TEXT("data_layer"), TEXT("expected_layer") }, SingleLayer) && !SingleLayer.TrimStartAndEnd().IsEmpty())
				{
					ExpectedLayers.Add(SingleLayer.TrimStartAndEnd());
				}
				const bool bPassed = Actor && ActorMatchesDataLayerFallback(Actor, ExpectedLayers);
				Result->SetBoolField(TEXT("passed"), bPassed);
				Result->SetStringField(TEXT("actor"), Actor ? Actor->GetActorLabel() : FString());
				Result->SetStringField(TEXT("status"), TEXT("checked_with_reflection_or_datalayer_tags"));
			}
			else if (Type == TEXT("feature_gate"))
			{
				TSharedPtr<FJsonObject> Gates = BuildFeatureGateReport(Content);
				TArray<FString> RequestedFeatures = ReadStringArray(Validation, TEXT("features"));
				FString SingleFeature;
				if (TryGetStringAny(Validation, { TEXT("feature"), TEXT("gate"), TEXT("name") }, SingleFeature) && !SingleFeature.TrimStartAndEnd().IsEmpty())
				{
					RequestedFeatures.Add(SingleFeature.TrimStartAndEnd());
				}

				bool bRequired = false;
				TryGetBoolAny(Validation, { TEXT("required"), TEXT("must_be_available") }, bRequired);

				bool bAllAvailable = true;
				TArray<TSharedPtr<FJsonValue>> FeatureResults;
				for (const FString& RequestedFeature : RequestedFeatures)
				{
					const FString Feature = RequestedFeature.TrimStartAndEnd().ToLower();
					FString GateKey = Feature;
					if (Feature == TEXT("world_partition") || Feature == TEXT("runtime_region"))
					{
						GateKey = TEXT("world_partition_runtime_region_write");
					}
					else if (Feature == TEXT("streaming") || Feature == TEXT("streaming_level"))
					{
						GateKey = TEXT("streaming_level_mutation");
					}
					else if (Feature == TEXT("data_layer"))
					{
						GateKey = TEXT("data_layer_mutation");
					}
					else if (Feature == TEXT("hlod"))
					{
						GateKey = TEXT("hlod_build");
					}
					else if (Feature == TEXT("lighting"))
					{
						GateKey = TEXT("lighting_build");
					}
					else if (Feature == TEXT("pcg"))
					{
						GateKey = TEXT("pcg_generate");
					}

					bool bAvailable = false;
					if (Feature == TEXT("navigation") || Feature == TEXT("nav") || Feature == TEXT("navmesh"))
					{
						bAvailable = World && FNavigationSystem::GetCurrent<UNavigationSystemV1>(World) != nullptr;
						GateKey = TEXT("navigation_system");
					}
					else
					{
						Gates->TryGetBoolField(GateKey, bAvailable);
					}
					bAllAvailable = bAllAvailable && bAvailable;

					TSharedPtr<FJsonObject> FeatureResult = MakeShared<FJsonObject>();
					FeatureResult->SetStringField(TEXT("feature"), Feature);
					FeatureResult->SetStringField(TEXT("gate"), GateKey);
					FeatureResult->SetBoolField(TEXT("available"), bAvailable);
					FeatureResults.Add(MakeShared<FJsonValueObject>(FeatureResult));
				}

				const bool bPassed = RequestedFeatures.Num() == 0 || !bRequired || bAllAvailable;
				Result->SetBoolField(TEXT("passed"), bPassed);
				Result->SetBoolField(TEXT("required"), bRequired);
				Result->SetObjectField(TEXT("feature_gates"), Gates);
				Result->SetArrayField(TEXT("features"), FeatureResults);
				Result->SetStringField(TEXT("status"), RequestedFeatures.Num() == 0 ? TEXT("feature_gate_report") : (bAllAvailable ? TEXT("available") : (bRequired ? TEXT("feature_unavailable") : TEXT("feature_unavailable_optional"))));
			}
			else if (Type == TEXT("hlod_assignment") || Type == TEXT("lod_state"))
			{
				Result->SetBoolField(TEXT("passed"), false);
				Result->SetStringField(TEXT("status"), TEXT("feature_unavailable_report_only"));
			}
			else
			{
				Result->SetBoolField(TEXT("passed"), true);
				Result->SetStringField(TEXT("status"), TEXT("structural_only"));
			}
			Results.Add(MakeShared<FJsonValueObject>(Result));
		}
		return true;
	}
}
