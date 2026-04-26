// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_Level.h"

#include "UeAgentInterfaceLogger.h"
#include "UeAgentInterfaceSettings.h"

#include "Async/Async.h"
#include "Async/TaskGraphInterfaces.h"
#include "ActorFactories/ActorFactory.h"
#include "Builders/CubeBuilder.h"
#include "Components/ActorComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "EditorActorFolders.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "EditorViewportClient.h"
#include "Engine/CollisionProfile.h"
#include "Engine/OverlapResult.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TargetPoint.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "ImageUtils.h"
#include "LevelEditorViewport.h"
#include "MeshDescription.h"
#include "HAL/PlatformProcess.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "NiagaraComponent.h"
#include "RenderingThread.h"
#include "Selection.h"
#include "ScopedTransaction.h"
#include "SceneView.h"
#include "StaticMeshAttributes.h"
#include "Subsystems/EditorActorSubsystem.h"
#include <atomic>

namespace UeAgentLevelOps
{
	static bool TryGetString(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FString& OutValue)
	{
		return Obj.IsValid() && Obj->TryGetStringField(Key, OutValue);
	}

	static bool TryGetNumber(const TSharedPtr<FJsonObject>& Obj, const FString& Key, double& OutValue)
	{
		return Obj.IsValid() && Obj->TryGetNumberField(Key, OutValue);
	}

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

	static FLevelEditorViewportClient* GetPreferredLevelViewportClient()
	{
		if (GEditor)
		{
			if (FViewport* ActiveViewport = GEditor->GetActiveViewport())
			{
				for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
				{
					if (ViewportClient && ViewportClient->Viewport == ActiveViewport)
					{
						return ViewportClient;
					}
				}
			}
		}

		if (GCurrentLevelEditingViewportClient && GCurrentLevelEditingViewportClient->Viewport)
		{
			return GCurrentLevelEditingViewportClient;
		}

		if (GEditor)
		{
			FLevelEditorViewportClient* FirstValid = nullptr;

			FLevelEditorViewportClient* BestPerspectiveByArea = nullptr;
			int64 BestPerspectiveArea = -1;

			FLevelEditorViewportClient* BestAnyByArea = nullptr;
			int64 BestAnyArea = -1;
			for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
			{
				if (!ViewportClient || !ViewportClient->Viewport)
				{
					continue;
				}
				if (!FirstValid)
				{
					FirstValid = ViewportClient;
				}

				const FIntPoint Size = ViewportClient->Viewport->GetSizeXY();
				const int64 Area = (int64)Size.X * (int64)Size.Y;
				if (Area > BestAnyArea)
				{
					BestAnyArea = Area;
					BestAnyByArea = ViewportClient;
				}
				if (ViewportClient->ViewportType == LVT_Perspective && Area > BestPerspectiveArea)
				{
					BestPerspectiveArea = Area;
					BestPerspectiveByArea = ViewportClient;
				}
			}
			if (BestPerspectiveByArea)
			{
				return BestPerspectiveByArea;
			}
			if (BestAnyByArea)
			{
				return BestAnyByArea;
			}
			if (FirstValid)
			{
				return FirstValid;
			}
		}

		return GCurrentLevelEditingViewportClient;
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

	static UActorComponent* FindComponentByNameOrPath(AActor* Actor, const FString& ComponentId)
	{
		if (!Actor || ComponentId.IsEmpty())
		{
			return nullptr;
		}

		const FString IdTrim = ComponentId.TrimStartAndEnd();
		TArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		for (UActorComponent* Comp : Components)
		{
			if (!Comp)
			{
				continue;
			}

			if (Comp->GetName().Equals(IdTrim, ESearchCase::IgnoreCase) ||
				Comp->GetFName().ToString().Equals(IdTrim, ESearchCase::IgnoreCase) ||
				Comp->GetPathName().Equals(IdTrim, ESearchCase::IgnoreCase))
			{
				return Comp;
			}
		}

		return nullptr;
	}

	static TSharedPtr<FJsonObject> MakeActorSummaryJson(AActor* Actor)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Actor ? Actor->GetName() : TEXT(""));
		Item->SetStringField(TEXT("label"), Actor ? Actor->GetActorLabel() : TEXT(""));
		Item->SetStringField(TEXT("class"), (Actor && Actor->GetClass()) ? Actor->GetClass()->GetPathName() : TEXT(""));
		return Item;
	}

	static void AugmentActorSummaryWithFolderTags(AActor* Actor, TSharedPtr<FJsonObject>& ActorSummary)
	{
		if (!Actor || !ActorSummary.IsValid())
		{
			return;
		}

		ActorSummary->SetStringField(TEXT("folder_path"), Actor->GetFolderPath().ToString());

		TArray<TSharedPtr<FJsonValue>> TagValues;
		TagValues.Reserve(Actor->Tags.Num());
		for (const FName& Tag : Actor->Tags)
		{
			TagValues.Add(MakeShared<FJsonValueString>(Tag.ToString()));
		}
		ActorSummary->SetArrayField(TEXT("tags"), TagValues);
	}

	static TSharedPtr<FJsonObject> MakeHitResultSummaryJson(
		const FHitResult& Hit,
		const bool bIncludeActorFolderTags,
		const bool bIncludePenetrationDepth)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetBoolField(TEXT("blocking_hit"), Hit.bBlockingHit);
		Item->SetBoolField(TEXT("start_penetrating"), Hit.bStartPenetrating);
		Item->SetNumberField(TEXT("time"), Hit.Time);
		Item->SetNumberField(TEXT("distance"), Hit.Distance);
		Item->SetObjectField(TEXT("location"), MakeVectorJson(Hit.ImpactPoint));
		Item->SetObjectField(TEXT("normal"), MakeVectorJson(Hit.ImpactNormal));
		Item->SetStringField(TEXT("bone_name"), Hit.BoneName.ToString());
		Item->SetStringField(TEXT("face_index"), FString::FromInt(Hit.FaceIndex));
		if (bIncludePenetrationDepth && Hit.bStartPenetrating)
		{
			Item->SetNumberField(TEXT("penetration_depth_cm"), Hit.PenetrationDepth);
		}

		if (AActor* HitActor = Hit.GetActor())
		{
			TSharedPtr<FJsonObject> ActorSummary = MakeActorSummaryJson(HitActor);
			if (bIncludeActorFolderTags)
			{
				AugmentActorSummaryWithFolderTags(HitActor, ActorSummary);
			}
			Item->SetObjectField(TEXT("actor"), ActorSummary);
		}
		if (Hit.GetComponent())
		{
			Item->SetStringField(TEXT("component_name"), Hit.GetComponent()->GetName());
			Item->SetStringField(TEXT("component_path"), Hit.GetComponent()->GetPathName());
		}
		return Item;
	}

	static void MaybeAttachStartPenetratingAdvice(
		const FString& Context,
		const bool bStartPenetrating,
		const bool bFindInitialOverlaps,
		const bool bIncludeFloorClearanceSuggestion,
		const double FloorClearanceCm,
		TSharedPtr<FJsonObject>& OutData)
	{
		if (!bStartPenetrating || !OutData.IsValid())
		{
			return;
		}

		TSharedPtr<FJsonObject> Advice = MakeShared<FJsonObject>();
		Advice->SetStringField(TEXT("context"), Context);
		Advice->SetStringField(TEXT("reason"), TEXT("start_penetrating"));
		Advice->SetBoolField(TEXT("find_initial_overlaps_current"), bFindInitialOverlaps);

		TArray<TSharedPtr<FJsonValue>> Suggestions;
		{
			TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
			S->SetStringField(TEXT("id"), TEXT("raise_start_end_z"));
			S->SetNumberField(TEXT("delta_cm"), 5.0);
			Suggestions.Add(MakeShared<FJsonValueObject>(S));
		}
		{
			TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
			S->SetStringField(TEXT("id"), TEXT("set_find_initial_overlaps"));
			S->SetBoolField(TEXT("value"), false);
			Suggestions.Add(MakeShared<FJsonValueObject>(S));
		}
		if (bIncludeFloorClearanceSuggestion)
		{
			TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
			S->SetStringField(TEXT("id"), TEXT("increase_floor_clearance_cm"));
			S->SetNumberField(TEXT("current_cm"), FloorClearanceCm);
			S->SetNumberField(TEXT("suggest_min_cm"), 5.0);
			Suggestions.Add(MakeShared<FJsonValueObject>(S));
		}

		Advice->SetArrayField(TEXT("suggestions"), Suggestions);
		OutData->SetObjectField(TEXT("start_penetrating_advice"), Advice);
	}

	static void FillActorTransformFields(AActor* Actor, TSharedPtr<FJsonObject>& OutData)
	{
		if (!Actor)
		{
			return;
		}

		OutData->SetStringField(TEXT("actor_name"), Actor->GetName());
		OutData->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
		OutData->SetObjectField(TEXT("location"), MakeVectorJson(Actor->GetActorLocation()));
		OutData->SetObjectField(TEXT("rotation"), MakeRotatorJson(Actor->GetActorRotation()));
		OutData->SetObjectField(TEXT("scale"), MakeVectorJson(Actor->GetActorScale3D()));
		TSharedPtr<FJsonObject> TransformObj = MakeShared<FJsonObject>();
		TransformObj->SetObjectField(TEXT("location"), MakeVectorJson(Actor->GetActorLocation()));
		TransformObj->SetObjectField(TEXT("rotation"), MakeRotatorJson(Actor->GetActorRotation()));
		TransformObj->SetObjectField(TEXT("scale"), MakeVectorJson(Actor->GetActorScale3D()));
		OutData->SetObjectField(TEXT("transform"), TransformObj);
	}

	static bool ExportResolvedProperty(UObject* Object, const FString& PropertyPath, FString& OutValueText, FString& OutPropertyClass, FString& OutError)
	{
		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		if (!ResolvePropertyPath(Object, PropertyPath, Property, ValuePtr) || !Property || !ValuePtr)
		{
			OutError = TEXT("property_not_found");
			return false;
		}

		OutPropertyClass = Property->GetClass() ? Property->GetClass()->GetName() : TEXT("");
		OutValueText.Reset();
		Property->ExportTextItem_Direct(OutValueText, ValuePtr, nullptr, Object, PPF_None);
		return true;
	}

	static void FinishActorTransformEdit(AActor* Actor)
	{
		if (!Actor)
		{
			return;
		}

		Actor->PostEditMove(true);
		Actor->MarkPackageDirty();
	}

	enum class EBoundsAxis : uint8
	{
		X,
		Y,
		Z
	};

	enum class EBoundsAnchor : uint8
	{
		Min,
		Center,
		Max
	};

	static bool ParseBoundsAxis(const FString& AxisText, EBoundsAxis& OutAxis)
	{
		const FString Normalized = AxisText.TrimStartAndEnd().ToLower();
		if (Normalized == TEXT("x"))
		{
			OutAxis = EBoundsAxis::X;
			return true;
		}
		if (Normalized == TEXT("y"))
		{
			OutAxis = EBoundsAxis::Y;
			return true;
		}
		if (Normalized == TEXT("z"))
		{
			OutAxis = EBoundsAxis::Z;
			return true;
		}
		return false;
	}

	static bool ParseBoundsAnchor(const FString& AnchorText, EBoundsAnchor& OutAnchor)
	{
		const FString Normalized = AnchorText.TrimStartAndEnd().ToLower();
		if (Normalized == TEXT("min"))
		{
			OutAnchor = EBoundsAnchor::Min;
			return true;
		}
		if (Normalized == TEXT("center") || Normalized == TEXT("centre"))
		{
			OutAnchor = EBoundsAnchor::Center;
			return true;
		}
		if (Normalized == TEXT("max"))
		{
			OutAnchor = EBoundsAnchor::Max;
			return true;
		}
		return false;
	}

	static double GetAxisValue(const FVector& Vector, EBoundsAxis Axis)
	{
		switch (Axis)
		{
		case EBoundsAxis::X: return Vector.X;
		case EBoundsAxis::Y: return Vector.Y;
		default: return Vector.Z;
		}
	}

	static void SetAxisValue(FVector& Vector, EBoundsAxis Axis, double Value)
	{
		switch (Axis)
		{
		case EBoundsAxis::X: Vector.X = (float)Value; break;
		case EBoundsAxis::Y: Vector.Y = (float)Value; break;
		default: Vector.Z = (float)Value; break;
		}
	}

	static bool GetActorBoundsBox(AActor* Actor, FBox& OutBox, FString& OutError)
	{
		if (!Actor)
		{
			OutError = TEXT("actor_not_found");
			return false;
		}

		OutBox = Actor->GetComponentsBoundingBox(true);
		if (!OutBox.IsValid)
		{
			OutError = TEXT("actor_has_invalid_bounds");
			return false;
		}
		return true;
	}

	static double GetBoundsAnchorValue(const FBox& Box, EBoundsAxis Axis, EBoundsAnchor Anchor)
	{
		const FVector Center = Box.GetCenter();
		const FVector Min = Box.Min;
		const FVector Max = Box.Max;
		switch (Anchor)
		{
		case EBoundsAnchor::Min:
			return GetAxisValue(Min, Axis);
		case EBoundsAnchor::Center:
			return GetAxisValue(Center, Axis);
		default:
			return GetAxisValue(Max, Axis);
		}
	}

	static TSharedPtr<FJsonObject> MakeBoundsJson(const FBox& Box)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetObjectField(TEXT("min"), MakeVectorJson(Box.Min));
		Obj->SetObjectField(TEXT("max"), MakeVectorJson(Box.Max));
		Obj->SetObjectField(TEXT("center"), MakeVectorJson(Box.GetCenter()));
		Obj->SetObjectField(TEXT("extent"), MakeVectorJson(Box.GetExtent()));
		return Obj;
	}

	static FString NormalizeFolderPathForMatch(const FString& InFolderPath)
	{
		FString Normalized = InFolderPath.TrimStartAndEnd();
		Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (Normalized.RemoveFromStart(TEXT("/")))
		{
		}
		while (Normalized.RemoveFromEnd(TEXT("/")))
		{
		}
		return Normalized;
	}

	static bool DoesActorMatchFolderPath(AActor* Actor, const FString& NormalizedFolderPath, bool bIncludeChildFolders)
	{
		if (!Actor || NormalizedFolderPath.IsEmpty())
		{
			return false;
		}

		const FString ActorFolderPath = NormalizeFolderPathForMatch(Actor->GetFolderPath().ToString());
		if (ActorFolderPath.IsEmpty())
		{
			return false;
		}

		if (ActorFolderPath.Equals(NormalizedFolderPath, ESearchCase::IgnoreCase))
		{
			return true;
		}

		if (!bIncludeChildFolders)
		{
			return false;
		}

		return ActorFolderPath.StartsWith(NormalizedFolderPath + TEXT("/"), ESearchCase::IgnoreCase);
	}

	static void AppendIgnoredActorsFromFilters(UWorld* World, const TSharedPtr<FJsonObject>& Params, TArray<AActor*>& InOutIgnoredActors)
	{
		if (!World || !Params.IsValid())
		{
			return;
		}

		FString IgnoreFolderPathPrefix;
		TryGetString(Params, TEXT("ignore_folder_path_prefix"), IgnoreFolderPathPrefix);
		IgnoreFolderPathPrefix = IgnoreFolderPathPrefix.TrimStartAndEnd();
		const FString NormalizedIgnoreFolderPrefix = NormalizeFolderPathForMatch(IgnoreFolderPathPrefix);
		const bool bHasIgnoreFolderFilter = !NormalizedIgnoreFolderPrefix.IsEmpty();

		auto ParseStringArray = [&Params](const FString& FieldName, TArray<FString>& OutStrings)
		{
			OutStrings.Reset();
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Params.IsValid() || !Params->TryGetArrayField(FieldName, Values) || !Values)
			{
				return;
			}

			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				if (!Value.IsValid())
				{
					continue;
				}
				const FString Str = Value->AsString().TrimStartAndEnd();
				if (!Str.IsEmpty())
				{
					OutStrings.Add(Str);
				}
			}
		};

		TArray<FString> IgnoreTags;
		TArray<FString> IgnoreClassSubstrings;
		ParseStringArray(TEXT("ignore_tags"), IgnoreTags);
		ParseStringArray(TEXT("ignore_class_substrings"), IgnoreClassSubstrings);

		if (!bHasIgnoreFolderFilter && IgnoreTags.Num() <= 0 && IgnoreClassSubstrings.Num() <= 0)
		{
			return;
		}

		TSet<AActor*> IgnoreSet;
		IgnoreSet.Reserve(InOutIgnoredActors.Num() + 64);
		for (AActor* Actor : InOutIgnoredActors)
		{
			if (Actor)
			{
				IgnoreSet.Add(Actor);
			}
		}

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor || Actor->IsActorBeingDestroyed())
			{
				continue;
			}

			bool bIgnored = false;
			if (bHasIgnoreFolderFilter && DoesActorMatchFolderPath(Actor, NormalizedIgnoreFolderPrefix, /*bIncludeChildFolders*/ true))
			{
				bIgnored = true;
			}

			if (!bIgnored && IgnoreTags.Num() > 0)
			{
				for (const FString& TagText : IgnoreTags)
				{
					if (!TagText.IsEmpty() && Actor->ActorHasTag(*TagText))
					{
						bIgnored = true;
						break;
					}
				}
			}

			if (!bIgnored && IgnoreClassSubstrings.Num() > 0)
			{
				const FString ActorClassPath = (Actor->GetClass() ? Actor->GetClass()->GetPathName() : FString());
				for (const FString& Sub : IgnoreClassSubstrings)
				{
					if (!Sub.IsEmpty() && ActorClassPath.Contains(Sub, ESearchCase::IgnoreCase))
					{
						bIgnored = true;
						break;
					}
				}
			}

			if (bIgnored)
			{
				IgnoreSet.Add(Actor);
			}
		}

		InOutIgnoredActors.Reset(IgnoreSet.Num());
		for (AActor* Actor : IgnoreSet)
		{
			if (Actor)
			{
				InOutIgnoredActors.Add(Actor);
			}
		}
	}

	static bool GetFolderActorBounds(
		UWorld* World,
		const FString& FolderPath,
		bool bIncludeChildFolders,
		FBox& OutCombinedBounds,
		TArray<AActor*>& OutActors,
		FString& OutError)
	{
		if (!World)
		{
			OutError = TEXT("missing_world");
			return false;
		}

		const FString NormalizedFolderPath = NormalizeFolderPathForMatch(FolderPath);
		if (NormalizedFolderPath.IsEmpty())
		{
			OutError = TEXT("missing_folder_path");
			return false;
		}

		OutCombinedBounds = FBox(EForceInit::ForceInit);
		OutActors.Reset();
		bool bFoundFolderActor = false;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor || Actor->IsActorBeingDestroyed())
			{
				continue;
			}
			if (!DoesActorMatchFolderPath(Actor, NormalizedFolderPath, bIncludeChildFolders))
			{
				continue;
			}

			bFoundFolderActor = true;
			FBox ActorBounds(EForceInit::ForceInit);
			FString BoundsError;
			if (!GetActorBoundsBox(Actor, ActorBounds, BoundsError))
			{
				continue;
			}

			OutCombinedBounds += ActorBounds;
			OutActors.Add(Actor);
		}

		if (OutActors.Num() <= 0)
		{
			OutError = bFoundFolderActor ? TEXT("folder_actor_bounds_invalid") : TEXT("folder_has_no_actors");
			return false;
		}

		if (!OutCombinedBounds.IsValid)
		{
			OutError = TEXT("folder_bounds_invalid");
			return false;
		}

		return true;
	}

	static bool GetActorsInFolder(
		UWorld* World,
		const FString& FolderPath,
		bool bIncludeChildFolders,
		TArray<AActor*>& OutActors,
		FString& OutError)
	{
		if (!World)
		{
			OutError = TEXT("missing_world");
			return false;
		}

		const FString NormalizedFolderPath = NormalizeFolderPathForMatch(FolderPath);
		if (NormalizedFolderPath.IsEmpty())
		{
			OutError = TEXT("missing_folder_path");
			return false;
		}

		OutActors.Reset();
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor || Actor->IsActorBeingDestroyed())
			{
				continue;
			}
			if (!DoesActorMatchFolderPath(Actor, NormalizedFolderPath, bIncludeChildFolders))
			{
				continue;
			}

			OutActors.Add(Actor);
		}

		return true;
	}

	static void GetBoxCorners(const FBox& Box, TArray<FVector>& OutCorners)
	{
		OutCorners.Reset(8);
		const FVector& Min = Box.Min;
		const FVector& Max = Box.Max;
		OutCorners.Add(FVector(Min.X, Min.Y, Min.Z));
		OutCorners.Add(FVector(Min.X, Min.Y, Max.Z));
		OutCorners.Add(FVector(Min.X, Max.Y, Min.Z));
		OutCorners.Add(FVector(Min.X, Max.Y, Max.Z));
		OutCorners.Add(FVector(Max.X, Min.Y, Min.Z));
		OutCorners.Add(FVector(Max.X, Min.Y, Max.Z));
		OutCorners.Add(FVector(Max.X, Max.Y, Min.Z));
		OutCorners.Add(FVector(Max.X, Max.Y, Max.Z));
	}

	static bool ComputeViewportLocationToFrameBounds(
		FLevelEditorViewportClient* ViewportClient,
		const FBox& Bounds,
		const FRotator& ViewRotation,
		double Padding,
		FVector& OutLocation,
		FString& OutError)
	{
		if (!ViewportClient || !ViewportClient->Viewport)
		{
			OutError = TEXT("no_level_editor_viewport");
			return false;
		}

		const FIntPoint Size = ViewportClient->Viewport->GetSizeXY();
		if (Size.X <= 0 || Size.Y <= 0)
		{
			OutError = TEXT("invalid_viewport_size");
			return false;
		}

		if (ViewportClient->ViewportType != LVT_Perspective)
		{
			OutError = TEXT("viewport_not_perspective");
			return false;
		}

		const double AspectRatio = static_cast<double>(Size.X) / static_cast<double>(Size.Y);
		if (AspectRatio <= KINDA_SMALL_NUMBER)
		{
			OutError = TEXT("invalid_viewport_aspect_ratio");
			return false;
		}

		Padding = FMath::Clamp(Padding, 1.0, 4.0);
		const double HorizontalHalfFovRadians = FMath::DegreesToRadians(FMath::Clamp(static_cast<double>(ViewportClient->ViewFOV), 5.0, 170.0) * 0.5);
		const double TanHalfHorizontalFov = FMath::Tan(HorizontalHalfFovRadians);
		const double TanHalfVerticalFov = TanHalfHorizontalFov / AspectRatio;
		if (TanHalfHorizontalFov <= KINDA_SMALL_NUMBER || TanHalfVerticalFov <= KINDA_SMALL_NUMBER)
		{
			OutError = TEXT("invalid_viewport_fov");
			return false;
		}

		const FVector BoundsCenter = Bounds.GetCenter();
		const FRotationMatrix RotationMatrix(ViewRotation);
		const FVector Forward = RotationMatrix.GetUnitAxis(EAxis::X);
		const FVector Right = RotationMatrix.GetUnitAxis(EAxis::Y);
		const FVector Up = RotationMatrix.GetUnitAxis(EAxis::Z);

		TArray<FVector> Corners;
		GetBoxCorners(Bounds, Corners);

		double RequiredDistance = 1.0;
		for (const FVector& Corner : Corners)
		{
			const FVector Relative = Corner - BoundsCenter;
			const double ForwardOffset = FVector::DotProduct(Relative, Forward);
			const double RightOffset = FMath::Abs(FVector::DotProduct(Relative, Right));
			const double UpOffset = FMath::Abs(FVector::DotProduct(Relative, Up));

			RequiredDistance = FMath::Max(RequiredDistance, (RightOffset / TanHalfHorizontalFov) - ForwardOffset);
			RequiredDistance = FMath::Max(RequiredDistance, (UpOffset / TanHalfVerticalFov) - ForwardOffset);
		}

		OutLocation = BoundsCenter - (Forward * FMath::Max(RequiredDistance * Padding, 1.0));
		return true;
	}

	static bool ResolveViewportScreenPoint(FLevelEditorViewportClient* ViewportClient, const TSharedPtr<FJsonObject>& Params, FVector2D& OutScreenPoint, FString& OutError)
	{
		if (!ViewportClient || !ViewportClient->Viewport)
		{
			OutError = TEXT("no_level_editor_viewport");
			return false;
		}

		double ScreenX = 0.0;
		double ScreenY = 0.0;
		if (!TryGetNumber(Params, TEXT("screen_x"), ScreenX) ||
			!TryGetNumber(Params, TEXT("screen_y"), ScreenY))
		{
			OutError = TEXT("missing_screen_point");
			return false;
		}

		const FIntPoint Size = ViewportClient->Viewport->GetSizeXY();
		if (Size.X <= 0 || Size.Y <= 0)
		{
			OutError = TEXT("invalid_viewport_size");
			return false;
		}

		OutScreenPoint.X = (float)FMath::Clamp(ScreenX, 0.0, (double)Size.X);
		OutScreenPoint.Y = (float)FMath::Clamp(ScreenY, 0.0, (double)Size.Y);
		return true;
	}

	static bool ResolveTraceChannel(const TSharedPtr<FJsonObject>& Params, ECollisionChannel& OutChannel)
	{
		OutChannel = ECC_Visibility;

		FString ChannelName;
		if (!TryGetString(Params, TEXT("trace_channel"), ChannelName) || ChannelName.IsEmpty())
		{
			return true;
		}

		const FString Normalized = ChannelName.TrimStartAndEnd().ToLower();
		if (Normalized == TEXT("visibility") || Normalized == TEXT("ecc_visibility"))
		{
			OutChannel = ECC_Visibility;
			return true;
		}
		if (Normalized == TEXT("camera") || Normalized == TEXT("ecc_camera"))
		{
			OutChannel = ECC_Camera;
			return true;
		}
		if (Normalized == TEXT("worldstatic") || Normalized == TEXT("world_static") || Normalized == TEXT("ecc_worldstatic"))
		{
			OutChannel = ECC_WorldStatic;
			return true;
		}
		if (Normalized == TEXT("worlddynamic") || Normalized == TEXT("world_dynamic") || Normalized == TEXT("ecc_worlddynamic"))
		{
			OutChannel = ECC_WorldDynamic;
			return true;
		}
		if (Normalized == TEXT("pawn") || Normalized == TEXT("ecc_pawn"))
		{
			OutChannel = ECC_Pawn;
			return true;
		}
		if (Normalized == TEXT("physicsbody") || Normalized == TEXT("physics_body") || Normalized == TEXT("ecc_physicsbody"))
		{
			OutChannel = ECC_PhysicsBody;
			return true;
		}

		return false;
	}

	static void FillHitResultJson(const FHitResult& Hit, TSharedPtr<FJsonObject>& OutData)
	{
		OutData->SetBoolField(TEXT("hit"), Hit.bBlockingHit);
		if (!Hit.bBlockingHit)
		{
			return;
		}

		OutData->SetObjectField(TEXT("location"), MakeVectorJson(Hit.ImpactPoint));
		OutData->SetObjectField(TEXT("normal"), MakeVectorJson(Hit.ImpactNormal));
		OutData->SetNumberField(TEXT("distance"), Hit.Distance);
		OutData->SetStringField(TEXT("bone_name"), Hit.BoneName.ToString());
		OutData->SetStringField(TEXT("face_index"), FString::FromInt(Hit.FaceIndex));
		if (Hit.GetActor())
		{
			OutData->SetStringField(TEXT("actor_id"), Hit.GetActor()->GetActorLabel());
			OutData->SetStringField(TEXT("actor_name"), Hit.GetActor()->GetName());
		}
		if (Hit.GetComponent())
		{
			OutData->SetStringField(TEXT("component_name"), Hit.GetComponent()->GetName());
			OutData->SetStringField(TEXT("component_path"), Hit.GetComponent()->GetPathName());
		}
	}

	static bool TraceActorFromScreenPoint(
		UWorld* World,
		FLevelEditorViewportClient* ViewportClient,
		const TSharedPtr<FJsonObject>& Params,
		FVector2D& OutScreenPoint,
		FVector& OutWorldOrigin,
		FVector& OutWorldDirection,
		double& OutTraceDistance,
		ECollisionChannel& OutTraceChannel,
		bool& OutTraceComplex,
		FHitResult& OutHit,
		FString& OutError)
	{
		if (!World)
		{
			OutError = TEXT("missing_world");
			return false;
		}
		if (!ViewportClient || !ViewportClient->Viewport)
		{
			OutError = TEXT("no_level_editor_viewport");
			return false;
		}

		if (!ResolveViewportScreenPoint(ViewportClient, Params, OutScreenPoint, OutError))
		{
			return false;
		}

		FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
			ViewportClient->Viewport,
			World->Scene,
			ViewportClient->EngineShowFlags).SetRealtimeUpdate(ViewportClient->IsRealtime()));
		FSceneView* SceneView = ViewportClient->CalcSceneView(&ViewFamily);
		if (!SceneView)
		{
			OutError = TEXT("calc_scene_view_failed");
			return false;
		}

		OutWorldOrigin = FVector::ZeroVector;
		OutWorldDirection = FVector::ForwardVector;
		SceneView->DeprojectFVector2D(OutScreenPoint, OutWorldOrigin, OutWorldDirection);

		OutTraceDistance = HALF_WORLD_MAX;
		TryGetNumber(Params, TEXT("trace_distance"), OutTraceDistance);
		OutTraceDistance = FMath::Clamp(OutTraceDistance, 1.0, static_cast<double>(HALF_WORLD_MAX));

		OutTraceChannel = ECC_Visibility;
		if (!ResolveTraceChannel(Params, OutTraceChannel))
		{
			OutError = TEXT("unsupported_trace_channel");
			return false;
		}

		OutTraceComplex = true;
		Params.IsValid() && Params->TryGetBoolField(TEXT("trace_complex"), OutTraceComplex);

		bool bAllowNoHit = false;
		Params.IsValid() && Params->TryGetBoolField(TEXT("allow_no_hit"), bAllowNoHit);

		FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(UeAgentPickActorAtScreen), OutTraceComplex);
		if (Params.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* IgnoreIds = nullptr;
			if (Params->TryGetArrayField(TEXT("ignore_actor_ids"), IgnoreIds) && IgnoreIds)
			{
				auto FindIgnoreActor = [](UWorld* InWorld, const FString& InId) -> AActor*
				{
					if (!InWorld || InId.TrimStartAndEnd().IsEmpty())
					{
						return nullptr;
					}

					const FString IdTrim = InId.TrimStartAndEnd();
					for (TActorIterator<AActor> It(InWorld); It; ++It)
					{
						AActor* Actor = *It;
						if (!Actor || Actor->IsActorBeingDestroyed())
						{
							continue;
						}
						if (Actor->GetName().Equals(IdTrim, ESearchCase::IgnoreCase) ||
							Actor->GetActorLabel().Equals(IdTrim, ESearchCase::IgnoreCase))
						{
							return Actor;
						}
					}
					return nullptr;
				};

				for (const TSharedPtr<FJsonValue>& IgnoreValue : *IgnoreIds)
				{
					const FString IgnoreId = IgnoreValue.IsValid() ? IgnoreValue->AsString() : FString();
					if (AActor* IgnoreActor = FindIgnoreActor(World, IgnoreId))
					{
						QueryParams.AddIgnoredActor(IgnoreActor);
					}
				}
			}
		}
		const FVector TraceEnd = OutWorldOrigin + (OutWorldDirection * OutTraceDistance);
		const bool bHit = World->LineTraceSingleByChannel(OutHit, OutWorldOrigin, TraceEnd, OutTraceChannel, QueryParams);
		if (!bHit)
		{
			if (bAllowNoHit)
			{
				OutHit = FHitResult();
				return true;
			}

			OutError = TEXT("actor_not_hit");
			return false;
		}

		if (!OutHit.GetActor())
		{
			if (bAllowNoHit)
			{
				return true;
			}

			OutError = TEXT("hit_actor_missing");
			return false;
		}

		return true;
	}

	static bool ComputeActorLocalObbBox(AActor* Actor, FBox& OutLocalBox, FString& OutError)
	{
		if (!Actor)
		{
			OutError = TEXT("actor_not_found");
			return false;
		}

		const FTransform ActorTransform = Actor->GetActorTransform();
		OutLocalBox = FBox(EForceInit::ForceInit);
		bool bAddedAnyCorner = false;

		TInlineComponentArray<UPrimitiveComponent*> PrimitiveComponents(Actor);
		for (UPrimitiveComponent* PrimitiveComponent : PrimitiveComponents)
		{
			if (!PrimitiveComponent || !PrimitiveComponent->IsRegistered())
			{
				continue;
			}

			const FBoxSphereBounds LocalBounds = PrimitiveComponent->CalcBounds(FTransform::Identity);
			const FBox PrimitiveLocalBox(LocalBounds.Origin - LocalBounds.BoxExtent, LocalBounds.Origin + LocalBounds.BoxExtent);

			TArray<FVector> PrimitiveCorners;
			GetBoxCorners(PrimitiveLocalBox, PrimitiveCorners);
			for (const FVector& PrimitiveLocalCorner : PrimitiveCorners)
			{
				const FVector WorldCorner = PrimitiveComponent->GetComponentTransform().TransformPosition(PrimitiveLocalCorner);
				const FVector ActorLocalCorner = ActorTransform.InverseTransformPosition(WorldCorner);
				OutLocalBox += ActorLocalCorner;
				bAddedAnyCorner = true;
			}
		}

		if (!bAddedAnyCorner)
		{
			FBox WorldBounds(EForceInit::ForceInit);
			if (!GetActorBoundsBox(Actor, WorldBounds, OutError))
			{
				return false;
			}

			TArray<FVector> WorldCorners;
			GetBoxCorners(WorldBounds, WorldCorners);
			for (const FVector& WorldCorner : WorldCorners)
			{
				OutLocalBox += ActorTransform.InverseTransformPosition(WorldCorner);
			}
			bAddedAnyCorner = true;
		}

		if (!bAddedAnyCorner || !OutLocalBox.IsValid)
		{
			OutError = TEXT("actor_obb_invalid");
			return false;
		}

		return true;
	}

	static TSharedPtr<FJsonObject> MakeActorObbJson(AActor* Actor, const FBox& LocalBox)
	{
		const FTransform ActorTransform = Actor->GetActorTransform();
		const FVector LocalCenter = LocalBox.GetCenter();
		const FVector LocalExtent = LocalBox.GetExtent();

		const FVector AxisXScaled = ActorTransform.TransformVector(FVector(1.0f, 0.0f, 0.0f));
		const FVector AxisYScaled = ActorTransform.TransformVector(FVector(0.0f, 1.0f, 0.0f));
		const FVector AxisZScaled = ActorTransform.TransformVector(FVector(0.0f, 0.0f, 1.0f));

		const FVector AxisX = AxisXScaled.GetSafeNormal();
		const FVector AxisY = AxisYScaled.GetSafeNormal();
		const FVector AxisZ = AxisZScaled.GetSafeNormal();

		const double HalfLengthX = LocalExtent.X * AxisXScaled.Size();
		const double HalfLengthY = LocalExtent.Y * AxisYScaled.Size();
		const double HalfLengthZ = LocalExtent.Z * AxisZScaled.Size();

		const FVector WorldCenter = ActorTransform.TransformPosition(LocalCenter);

		TArray<TSharedPtr<FJsonValue>> CornerValues;
		CornerValues.Reserve(8);
		for (int32 SignX = -1; SignX <= 1; SignX += 2)
		{
			for (int32 SignY = -1; SignY <= 1; SignY += 2)
			{
				for (int32 SignZ = -1; SignZ <= 1; SignZ += 2)
				{
					const FVector Corner =
						WorldCenter +
						(AxisX * (HalfLengthX * static_cast<double>(SignX))) +
						(AxisY * (HalfLengthY * static_cast<double>(SignY))) +
						(AxisZ * (HalfLengthZ * static_cast<double>(SignZ)));
					CornerValues.Add(MakeShared<FJsonValueObject>(MakeVectorJson(Corner)));
				}
			}
		}

		TSharedPtr<FJsonObject> HalfLengthsJson = MakeShared<FJsonObject>();
		HalfLengthsJson->SetNumberField(TEXT("x"), HalfLengthX);
		HalfLengthsJson->SetNumberField(TEXT("y"), HalfLengthY);
		HalfLengthsJson->SetNumberField(TEXT("z"), HalfLengthZ);

		TSharedPtr<FJsonObject> ObbJson = MakeShared<FJsonObject>();
		ObbJson->SetObjectField(TEXT("center"), MakeVectorJson(WorldCenter));
		ObbJson->SetObjectField(TEXT("axis_x"), MakeVectorJson(AxisX));
		ObbJson->SetObjectField(TEXT("axis_y"), MakeVectorJson(AxisY));
		ObbJson->SetObjectField(TEXT("axis_z"), MakeVectorJson(AxisZ));
		ObbJson->SetObjectField(TEXT("half_lengths"), HalfLengthsJson);
		ObbJson->SetObjectField(TEXT("local_center"), MakeVectorJson(LocalCenter));
		ObbJson->SetObjectField(TEXT("local_extent"), MakeVectorJson(LocalExtent));
		ObbJson->SetArrayField(TEXT("corners"), CornerValues);
		return ObbJson;
	}

	static TSharedPtr<FJsonObject> MakeActorInfoJson(AActor* Actor)
	{
		TSharedPtr<FJsonObject> Info = MakeActorSummaryJson(Actor);
		if (!Actor)
		{
			return Info;
		}

		Info->SetStringField(TEXT("folder_path"), Actor->GetFolderPath().ToString());
		FillActorTransformFields(Actor, Info);

		TArray<TSharedPtr<FJsonValue>> TagValues;
		TagValues.Reserve(Actor->Tags.Num());
		for (const FName& Tag : Actor->Tags)
		{
			TagValues.Add(MakeShared<FJsonValueString>(Tag.ToString()));
		}
		Info->SetArrayField(TEXT("tags"), TagValues);

		FString BoundsError;
		FBox Bounds(EForceInit::ForceInit);
		if (GetActorBoundsBox(Actor, Bounds, BoundsError))
		{
			Info->SetObjectField(TEXT("bounds"), MakeBoundsJson(Bounds));
		}

		FString ObbError;
		FBox LocalObbBox(EForceInit::ForceInit);
		if (ComputeActorLocalObbBox(Actor, LocalObbBox, ObbError))
		{
			Info->SetObjectField(TEXT("obb"), MakeActorObbJson(Actor, LocalObbBox));
		}

		return Info;
	}

	static bool GetStaticMeshComponentForVertexQuery(AActor* Actor, const FString& ComponentId, UStaticMeshComponent*& OutComponent, FString& OutError)
	{
		OutComponent = nullptr;
		if (!Actor)
		{
			OutError = TEXT("actor_not_found");
			return false;
		}

		if (!ComponentId.IsEmpty())
		{
			OutComponent = Cast<UStaticMeshComponent>(FindComponentByNameOrPath(Actor, ComponentId));
		}
		if (!OutComponent)
		{
			OutComponent = Actor->FindComponentByClass<UStaticMeshComponent>();
		}
		if (!OutComponent)
		{
			OutError = TEXT("static_mesh_component_not_found");
			return false;
		}
		if (!OutComponent->GetStaticMesh())
		{
			OutError = TEXT("static_mesh_not_found");
			return false;
		}
		return true;
	}

	static bool GetStaticMeshVertexLocalPositions(UStaticMesh* StaticMesh, TArray<FVector>& OutPositions, FString& OutError)
	{
		OutPositions.Reset();
		if (!StaticMesh)
		{
			OutError = TEXT("static_mesh_not_found");
			return false;
		}

		FMeshDescription* MeshDescription = StaticMesh->GetMeshDescription(0);
		if (!MeshDescription)
		{
			OutError = TEXT("mesh_description_not_found");
			return false;
		}

		FStaticMeshAttributes Attributes(*MeshDescription);
		TVertexAttributesConstRef<FVector3f> VertexPositions = Attributes.GetVertexPositions();
		for (const FVertexID VertexId : MeshDescription->Vertices().GetElementIDs())
		{
			OutPositions.Add((FVector)VertexPositions[VertexId]);
		}

		if (OutPositions.Num() <= 0)
		{
			OutError = TEXT("vertex_positions_empty");
			return false;
		}
		return true;
	}

	static bool ResolveVertexWorldPosition(UStaticMeshComponent* Component, const TArray<FVector>& LocalPositions, int32 VertexIndex, FVector& OutWorldPosition, FString& OutError)
	{
		if (!Component)
		{
			OutError = TEXT("static_mesh_component_not_found");
			return false;
		}
		if (!LocalPositions.IsValidIndex(VertexIndex))
		{
			OutError = TEXT("vertex_index_out_of_range");
			return false;
		}

		OutWorldPosition = Component->GetComponentTransform().TransformPosition(LocalPositions[VertexIndex]);
		return true;
	}
}


#include "UeAgentHttpServer_Level_ActorAndTransform.inl"
#include "UeAgentHttpServer_Level_ViewportAndMesh.inl"
#include "UeAgentHttpServer_Level_Navigation.inl"
#include "UeAgentHttpServer_Level_CaptureAndPersistence.inl"
