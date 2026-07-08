// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_Modeling.h"
#include "UeAgentHttpServer_ModelingOps.h"

#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Module/UeAgentInterfaceLogger.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Components/BoxComponent.h"
#include "Components/DynamicMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshOverlay.h"
#include "DynamicMeshActor.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "Engine/Selection.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "InteractiveTool.h"
#include "InteractiveToolManager.h"
#include "InteractiveToolQueryInterfaces.h"
#include "LevelEditorViewport.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "ModelingToolsEditorMode.h"
#include "ModelingToolsEditorModeSettings.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "PhysicsEngine/BodySetup.h"
#include "PropertySets/CreateMeshObjectTypeProperties.h"
#include "SceneView.h"
#include "Selection/GeometrySelectionManager.h"
#include "Selection/GeometrySelector.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Tools/EdModeInteractiveToolsContext.h"
#include "UDynamicMesh.h"
#include "StaticMeshAttributes.h"

namespace UeAgentModelingOps
{
	FModelingContext::FModelingContext()
		: Mode(nullptr)
		, ToolsContext(nullptr)
		, ViewportClient(nullptr)
	{
	}

	TSharedPtr<FJsonObject> MakeVectorJson(const FVector& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), Value.X);
		Obj->SetNumberField(TEXT("y"), Value.Y);
		Obj->SetNumberField(TEXT("z"), Value.Z);
		return Obj;
	}

	TSharedPtr<FJsonObject> MakeRotatorJson(const FRotator& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("pitch"), Value.Pitch);
		Obj->SetNumberField(TEXT("yaw"), Value.Yaw);
		Obj->SetNumberField(TEXT("roll"), Value.Roll);
		return Obj;
	}

	TSharedPtr<FJsonObject> MakeActorSummaryJson(AActor* Actor)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Actor ? Actor->GetName() : TEXT(""));
		Obj->SetStringField(TEXT("label"), Actor ? Actor->GetActorLabel() : TEXT(""));
		Obj->SetStringField(TEXT("class"), (Actor && Actor->GetClass()) ? Actor->GetClass()->GetPathName() : TEXT(""));
		return Obj;
	}

	bool TryGetNumberField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, double& OutValue)
	{
		return Obj.IsValid() && Obj->TryGetNumberField(FieldName, OutValue);
	}

	bool TryGetVectorField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, FVector& OutValue)
	{
		const TSharedPtr<FJsonValue>* FieldValue = UeAgentJsonDiagnostics::FindFieldValue(Obj, FieldName);
		if (!FieldValue)
		{
			return false;
		}

		return UeAgentJsonDiagnostics::TryReadVectorValue(*FieldValue, OutValue);
	}

	bool TryGetRotatorField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, FRotator& OutValue)
	{
		const TSharedPtr<FJsonValue>* FieldValue = UeAgentJsonDiagnostics::FindFieldValue(Obj, FieldName);
		if (!FieldValue)
		{
			return false;
		}

		return UeAgentJsonDiagnostics::TryReadRotatorValue(*FieldValue, OutValue);
	}

	FPrimitiveBoundsPlacement::FPrimitiveBoundsPlacement()
		: bEnabled(false)
		, Center(FVector::ZeroVector)
		, Extent(FVector::ZeroVector)
		, Rotation(FRotator::ZeroRotator)
	{
	}

	bool ParsePrimitiveBoundsPlacement(const TSharedPtr<FJsonObject>& Params, FPrimitiveBoundsPlacement& OutPlacement, FString& OutError)
	{
		OutPlacement = FPrimitiveBoundsPlacement{};
		if (!Params.IsValid())
		{
			return true;
		}

		const TSharedPtr<FJsonObject>* BoundsObject = nullptr;
		const bool bHasBoundsObject = Params->TryGetObjectField(TEXT("bounds"), BoundsObject) && BoundsObject && BoundsObject->IsValid();

		FVector Center = FVector::ZeroVector;
		FVector Extent = FVector::ZeroVector;
		bool bHasCenterExtent = false;
		if (bHasBoundsObject)
		{
			bHasCenterExtent = TryGetVectorField(*BoundsObject, TEXT("center"), Center) && TryGetVectorField(*BoundsObject, TEXT("extent"), Extent);
		}
		if (!bHasCenterExtent)
		{
			bHasCenterExtent = TryGetVectorField(Params, TEXT("bounds_center"), Center) && TryGetVectorField(Params, TEXT("bounds_extent"), Extent);
		}

		if (!bHasCenterExtent)
		{
			FVector Min = FVector::ZeroVector;
			FVector Max = FVector::ZeroVector;
			bool bHasMinMax = false;
			if (bHasBoundsObject)
			{
				bHasMinMax = TryGetVectorField(*BoundsObject, TEXT("min"), Min) && TryGetVectorField(*BoundsObject, TEXT("max"), Max);
			}
			if (!bHasMinMax)
			{
				bHasMinMax = TryGetVectorField(Params, TEXT("bounds_min"), Min) && TryGetVectorField(Params, TEXT("bounds_max"), Max);
			}

			if (bHasMinMax)
			{
				Center = (Min + Max) * 0.5f;
				Extent = (Max - Min) * 0.5f;
				bHasCenterExtent = true;
			}
		}

		if (!bHasCenterExtent)
		{
			return true;
		}

		if (Extent.X <= 0.0f || Extent.Y <= 0.0f || Extent.Z < 0.0f)
		{
			OutError = TEXT("primitive_bounds_extent_invalid");
			return false;
		}

		FRotator Rotation = FRotator::ZeroRotator;
		if (!TryGetRotatorField(Params, TEXT("rotation"), Rotation))
		{
			TryGetRotatorField(Params, TEXT("orientation"), Rotation);
		}

		OutPlacement.bEnabled = true;
		OutPlacement.Center = Center;
		OutPlacement.Extent = Extent;
		OutPlacement.Rotation = Rotation;
		return true;
	}

	TSharedPtr<FJsonValueObject> MakeToolPropertyEntry(const FString& PropertySet, const FString& PropertyName, const FString& ValueText)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("property_set"), PropertySet);
		Obj->SetStringField(TEXT("property_name"), PropertyName);
		Obj->SetStringField(TEXT("value_text"), ValueText);
		return MakeShared<FJsonValueObject>(Obj);
	}

	bool TryGetExplicitToolPropertyValue(const TSharedPtr<FJsonObject>& Params, const FString& PropertySet, const FString& PropertyName, FString& OutValueText)
	{
		const TArray<TSharedPtr<FJsonValue>>* PropertiesArray = nullptr;
		if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("tool_properties"), PropertiesArray) || !PropertiesArray)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& EntryValue : *PropertiesArray)
		{
			const TSharedPtr<FJsonObject> Entry = EntryValue.IsValid() ? EntryValue->AsObject() : nullptr;
			if (!Entry.IsValid())
			{
				continue;
			}

			FString EntrySet;
			FString EntryName;
			FString EntryValueText;
			Entry->TryGetStringField(TEXT("property_set"), EntrySet);
			Entry->TryGetStringField(TEXT("property_name"), EntryName);
			Entry->TryGetStringField(TEXT("value_text"), EntryValueText);

			if (EntryName.Equals(PropertyName, ESearchCase::IgnoreCase) &&
				(PropertySet.IsEmpty() || EntrySet.Equals(PropertySet, ESearchCase::IgnoreCase)))
			{
				OutValueText = EntryValueText;
				return true;
			}
		}
		return false;
	}

	bool TryGetExplicitToolPropertyNumber(const TSharedPtr<FJsonObject>& Params, const FString& PropertySet, const FString& PropertyName, double& OutValue)
	{
		FString ValueText;
		return TryGetExplicitToolPropertyValue(Params, PropertySet, PropertyName, ValueText) && LexTryParseString(OutValue, *ValueText);
	}

	bool TryGetExplicitToolPropertyInt(const TSharedPtr<FJsonObject>& Params, const FString& PropertySet, const FString& PropertyName, int32& OutValue)
	{
		FString ValueText;
		return TryGetExplicitToolPropertyValue(Params, PropertySet, PropertyName, ValueText) && LexTryParseString(OutValue, *ValueText);
	}

	void GetSelectedActors(TArray<AActor*>& OutActors)
	{
		OutActors.Reset();
		if (!GEditor)
		{
			return;
		}

		for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
		{
			if (AActor* Actor = Cast<AActor>(*It))
			{
				OutActors.Add(Actor);
			}
		}
	}

	AActor* ResolveCreatedActorFromSelectionDelta(const TArray<AActor*>& BeforeSelection, const TArray<AActor*>& AfterSelection)
	{
		TSet<const AActor*> BeforeSet;
		for (AActor* Actor : BeforeSelection)
		{
			if (Actor)
			{
				BeforeSet.Add(Actor);
			}
		}

		TArray<AActor*> NewActors;
		for (AActor* Actor : AfterSelection)
		{
			if (Actor && !BeforeSet.Contains(Actor))
			{
				NewActors.Add(Actor);
			}
		}

		if (NewActors.Num() == 1)
		{
			return NewActors[0];
		}
		if (AfterSelection.Num() == 1)
		{
			return AfterSelection[0];
		}
		return nullptr;
	}

	bool PreparePrimitivePlacementParameters(
		const FString& WrapperCommand,
		const FString& ToolIdentifier,
		const TSharedPtr<FJsonObject>& InParams,
		TSharedPtr<FJsonObject>& OutParams,
		FPrimitiveBoundsPlacement& OutPlacement,
		TSharedPtr<FJsonObject>& OutDerivedData,
		FString& OutError)
	{
		OutPlacement = FPrimitiveBoundsPlacement{};
		OutDerivedData.Reset();
		OutParams = InParams.IsValid() ? MakeShared<FJsonObject>(*InParams) : MakeShared<FJsonObject>();

		if (!ParsePrimitiveBoundsPlacement(OutParams, OutPlacement, OutError))
		{
			return false;
		}
		if (!OutPlacement.bEnabled)
		{
			return true;
		}

		OutPlacement.PrimitiveKind = WrapperCommand;
		if (OutParams->HasTypedField<EJson::Boolean>(TEXT("accept")) && !OutParams->GetBoolField(TEXT("accept")))
		{
			OutError = TEXT("primitive_bounds_require_accept");
			return false;
		}
		OutParams->SetBoolField(TEXT("accept"), true);

		TArray<TSharedPtr<FJsonValue>> ComputedProperties;
		ComputedProperties.Add(MakeToolPropertyEntry(TEXT("ShapeSettings"), TEXT("TargetSurface"), TEXT("AtOrigin")));
		ComputedProperties.Add(MakeToolPropertyEntry(TEXT("ShapeSettings"), TEXT("PivotLocation"), TEXT("Centered")));

		const FVector FullSize = OutPlacement.Extent * 2.0f;
		TSharedPtr<FJsonObject> DerivedData = MakeShared<FJsonObject>();
		DerivedData->SetStringField(TEXT("tool_identifier"), ToolIdentifier);
		DerivedData->SetStringField(TEXT("primitive_kind"), WrapperCommand);
		DerivedData->SetObjectField(TEXT("bounds_center"), MakeVectorJson(OutPlacement.Center));
		DerivedData->SetObjectField(TEXT("bounds_extent"), MakeVectorJson(OutPlacement.Extent));
		DerivedData->SetObjectField(TEXT("rotation"), MakeRotatorJson(OutPlacement.Rotation));

		auto AddDerivedNumber = [&ComputedProperties, &DerivedData](const TCHAR* PropertyName, double Value)
		{
			ComputedProperties.Add(MakeToolPropertyEntry(TEXT("ShapeSettings"), PropertyName, FString::SanitizeFloat(Value)));
			DerivedData->SetNumberField(PropertyName, Value);
		};

		if (ToolIdentifier == TEXT("BeginAddBoxPrimitiveTool"))
		{
			AddDerivedNumber(TEXT("Depth"), FullSize.X);
			AddDerivedNumber(TEXT("Width"), FullSize.Y);
			AddDerivedNumber(TEXT("Height"), FullSize.Z);
		}
		else if (ToolIdentifier == TEXT("BeginAddRectanglePrimitiveTool"))
		{
			AddDerivedNumber(TEXT("Depth"), FullSize.X);
			AddDerivedNumber(TEXT("Width"), FullSize.Y);
		}
		else if (ToolIdentifier == TEXT("BeginAddCylinderPrimitiveTool"))
		{
			AddDerivedNumber(TEXT("Radius"), FMath::Max<double>(OutPlacement.Extent.X, OutPlacement.Extent.Y));
			AddDerivedNumber(TEXT("Height"), FullSize.Z);
		}
		else if (ToolIdentifier == TEXT("BeginAddSpherePrimitiveTool"))
		{
			AddDerivedNumber(TEXT("Radius"), FMath::Max3<double>(OutPlacement.Extent.X, OutPlacement.Extent.Y, OutPlacement.Extent.Z));
		}
		else if (ToolIdentifier == TEXT("BeginAddStairsPrimitiveTool"))
		{
			const double TotalRun = FullSize.X;
			const double TotalWidth = FullSize.Y;
			const double TotalRise = FullSize.Z;

			double ExplicitStepWidth = 0.0;
			double ExplicitStepHeight = 0.0;
			double ExplicitStepDepth = 0.0;
			int32 ExplicitNumSteps = 0;
			const bool bHasExplicitStepWidth = TryGetExplicitToolPropertyNumber(OutParams, TEXT("ShapeSettings"), TEXT("StepWidth"), ExplicitStepWidth) && ExplicitStepWidth > 0.0;
			const bool bHasExplicitStepHeight = TryGetExplicitToolPropertyNumber(OutParams, TEXT("ShapeSettings"), TEXT("StepHeight"), ExplicitStepHeight) && ExplicitStepHeight > 0.0;
			const bool bHasExplicitStepDepth = TryGetExplicitToolPropertyNumber(OutParams, TEXT("ShapeSettings"), TEXT("StepDepth"), ExplicitStepDepth) && ExplicitStepDepth > 0.0;
			const bool bHasExplicitNumSteps = TryGetExplicitToolPropertyInt(OutParams, TEXT("ShapeSettings"), TEXT("NumSteps"), ExplicitNumSteps) && ExplicitNumSteps >= 2;

			int32 NumSteps = 0;
			if (bHasExplicitNumSteps)
			{
				NumSteps = ExplicitNumSteps;
			}
			else if (bHasExplicitStepHeight)
			{
				NumSteps = FMath::Max(2, FMath::RoundToInt(TotalRise / ExplicitStepHeight));
			}
			else if (bHasExplicitStepDepth)
			{
				NumSteps = FMath::Max(2, FMath::RoundToInt(TotalRun / ExplicitStepDepth));
			}
			else
			{
				NumSteps = FMath::Max(2, FMath::RoundToInt(TotalRise / 20.0));
			}

			const double StepWidth = bHasExplicitStepWidth ? ExplicitStepWidth : TotalWidth;
			const double StepHeight = bHasExplicitStepHeight ? ExplicitStepHeight : (NumSteps > 0 ? TotalRise / NumSteps : TotalRise);
			const double StepDepth = bHasExplicitStepDepth ? ExplicitStepDepth : (NumSteps > 0 ? TotalRun / NumSteps : TotalRun);

			AddDerivedNumber(TEXT("StepWidth"), StepWidth);
			AddDerivedNumber(TEXT("StepHeight"), StepHeight);
			AddDerivedNumber(TEXT("StepDepth"), StepDepth);
			ComputedProperties.Add(MakeToolPropertyEntry(TEXT("ShapeSettings"), TEXT("NumSteps"), FString::FromInt(NumSteps)));
			DerivedData->SetNumberField(TEXT("NumSteps"), NumSteps);
		}
		else if (ToolIdentifier == TEXT("BeginAddRampPrimitiveTool") || ToolIdentifier == TEXT("BeginAddRampCornerPrimitiveTool"))
		{
			AddDerivedNumber(TEXT("Depth"), FullSize.X);
			AddDerivedNumber(TEXT("Width"), FullSize.Y);
			AddDerivedNumber(TEXT("Height"), FullSize.Z);
		}
		else
		{
			return true;
		}

		const TArray<TSharedPtr<FJsonValue>>* ExistingProperties = nullptr;
		TArray<TSharedPtr<FJsonValue>> MergedProperties = ComputedProperties;
		if (OutParams->TryGetArrayField(TEXT("tool_properties"), ExistingProperties) && ExistingProperties)
		{
			MergedProperties.Append(*ExistingProperties);
		}
		OutParams->SetArrayField(TEXT("tool_properties"), MergedProperties);
		OutDerivedData = DerivedData;
		return true;
	}

	bool ApplyPrimitivePlacementToActor(AActor* Actor, const FPrimitiveBoundsPlacement& Placement, FString& OutError)
	{
		if (!Actor)
		{
			OutError = TEXT("created_actor_resolution_failed");
			return false;
		}

		Actor->Modify();
		Actor->SetActorScale3D(FVector::OneVector);
		Actor->SetActorLocationAndRotation(Placement.Center, Placement.Rotation, false, nullptr, ETeleportType::TeleportPhysics);
		Actor->MarkPackageDirty();
		return true;
	}

	void ApplyOptionalCreatedActorMetadata(AActor* Actor, const TSharedPtr<FJsonObject>& Params)
	{
		if (!Actor || !Params.IsValid())
		{
			return;
		}

		FString Label;
		if (Params->TryGetStringField(TEXT("label"), Label) && !Label.TrimStartAndEnd().IsEmpty())
		{
			Actor->SetActorLabel(Label);
			Actor->MarkPackageDirty();
		}

		FString FolderPath;
		if (Params->TryGetStringField(TEXT("folder_path"), FolderPath) && !FolderPath.TrimStartAndEnd().IsEmpty())
		{
			Actor->SetFolderPath(*FolderPath);
			Actor->MarkPackageDirty();
		}
	}

	static constexpr const TCHAR* ModelingPrimitiveCollisionTagPrefix = TEXT("UAIModelingPrimitiveCollision:");
	static constexpr int32 DefaultModelingPrimitiveCollisionMaxBoxes = 256;
	static constexpr double DefaultModelingPrimitiveCollisionMaxStepHeight = 20.0;

	struct FModelingPrimitiveCollisionOptions
	{
		bool bGenerate = true;
		bool bReplaceExisting = true;
		FString Mode = TEXT("simple_auto");
		FString ProfileName = TEXT("BlockAll");
		FString EnabledText = TEXT("query_and_physics");
		ECollisionEnabled::Type CollisionEnabled = ECollisionEnabled::QueryAndPhysics;
		int32 MaxBoxes = DefaultModelingPrimitiveCollisionMaxBoxes;
		double MaxStepHeight = DefaultModelingPrimitiveCollisionMaxStepHeight;
	};

	struct FModelingPrimitiveCollisionShapeSpec
	{
		FString Shape = TEXT("box");
		FString Kind;
		FVector Center = FVector::ZeroVector;
		FVector Extent = FVector::ZeroVector;
		double Radius = 0.0;
	};

	static ECollisionEnabled::Type ParseModelingPrimitiveCollisionEnabled(const FString& Value)
	{
		const FString Normalized = Value.TrimStartAndEnd().ToLower();
		if (Normalized == TEXT("none") || Normalized == TEXT("no_collision") || Normalized == TEXT("disabled"))
		{
			return ECollisionEnabled::NoCollision;
		}
		if (Normalized == TEXT("query_only"))
		{
			return ECollisionEnabled::QueryOnly;
		}
		if (Normalized == TEXT("physics_only"))
		{
			return ECollisionEnabled::PhysicsOnly;
		}
		return ECollisionEnabled::QueryAndPhysics;
	}

	bool IsModelingPrimitiveCollisionModeDisabled(const FString& Mode)
	{
		const FString Normalized = Mode.TrimStartAndEnd().ToLower();
		return Normalized == TEXT("none") || Normalized == TEXT("disabled") || Normalized == TEXT("no_collision");
	}

	bool ParseModelingPrimitiveCollisionOptions(const TSharedPtr<FJsonObject>& Params, FModelingPrimitiveCollisionOptions& OutOptions)
	{
		OutOptions = FModelingPrimitiveCollisionOptions{};
		if (!Params.IsValid())
		{
			return true;
		}

		Params->TryGetBoolField(TEXT("auto_generate_collision"), OutOptions.bGenerate);

		const TSharedPtr<FJsonObject>* CollisionObjectPtr = nullptr;
		if (Params->TryGetObjectField(TEXT("collision"), CollisionObjectPtr) && CollisionObjectPtr && CollisionObjectPtr->IsValid())
		{
			const TSharedPtr<FJsonObject>& CollisionObject = *CollisionObjectPtr;
			CollisionObject->TryGetStringField(TEXT("mode"), OutOptions.Mode);
			if (!CollisionObject->TryGetStringField(TEXT("profile_name"), OutOptions.ProfileName))
			{
				CollisionObject->TryGetStringField(TEXT("profile"), OutOptions.ProfileName);
			}
			CollisionObject->TryGetStringField(TEXT("enabled"), OutOptions.EnabledText);
			CollisionObject->TryGetBoolField(TEXT("replace_existing"), OutOptions.bReplaceExisting);

			double MaxBoxesNumber = static_cast<double>(OutOptions.MaxBoxes);
			if (CollisionObject->TryGetNumberField(TEXT("max_boxes"), MaxBoxesNumber) ||
				CollisionObject->TryGetNumberField(TEXT("max_shapes"), MaxBoxesNumber))
			{
				OutOptions.MaxBoxes = FMath::Clamp(FMath::RoundToInt(MaxBoxesNumber), 0, 10000);
			}

			double MaxStepHeightNumber = OutOptions.MaxStepHeight;
			if (CollisionObject->TryGetNumberField(TEXT("max_step_height_cm"), MaxStepHeightNumber) ||
				CollisionObject->TryGetNumberField(TEXT("max_step_height"), MaxStepHeightNumber) ||
				CollisionObject->TryGetNumberField(TEXT("step_height"), MaxStepHeightNumber))
			{
				OutOptions.MaxStepHeight = FMath::Max(1.0, MaxStepHeightNumber);
			}
		}
		else
		{
			FString CollisionMode;
			if (Params->TryGetStringField(TEXT("collision"), CollisionMode))
			{
				OutOptions.Mode = CollisionMode;
			}
		}

		OutOptions.Mode = OutOptions.Mode.TrimStartAndEnd().ToLower();
		if (OutOptions.Mode.IsEmpty())
		{
			OutOptions.Mode = TEXT("simple_auto");
		}
		OutOptions.ProfileName = OutOptions.ProfileName.TrimStartAndEnd();
		if (OutOptions.ProfileName.IsEmpty())
		{
			OutOptions.ProfileName = TEXT("BlockAll");
		}
		OutOptions.EnabledText = OutOptions.EnabledText.TrimStartAndEnd().ToLower();
		if (OutOptions.EnabledText.IsEmpty())
		{
			OutOptions.EnabledText = TEXT("query_and_physics");
		}
		OutOptions.CollisionEnabled = ParseModelingPrimitiveCollisionEnabled(OutOptions.EnabledText);
		if (IsModelingPrimitiveCollisionModeDisabled(OutOptions.Mode) || OutOptions.CollisionEnabled == ECollisionEnabled::NoCollision)
		{
			OutOptions.bGenerate = false;
		}
		return true;
	}

	void RemoveModelingPrimitiveCollisionComponents(AActor* Actor)
	{
		if (!Actor)
		{
			return;
		}

		TArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		for (UActorComponent* Component : Components)
		{
			if (!Component)
			{
				continue;
			}
			if (!Component->IsA<UBoxComponent>() && !Component->IsA<USphereComponent>())
			{
				continue;
			}

			bool bMatches = Component->GetName().StartsWith(TEXT("UAI_ModelingCollision_"));
			for (const FName& TagName : Component->ComponentTags)
			{
				if (TagName.ToString().StartsWith(ModelingPrimitiveCollisionTagPrefix))
				{
					bMatches = true;
					break;
				}
			}

			if (bMatches)
			{
				Component->Modify();
				Component->DestroyComponent();
			}
		}
	}

	static FVector SanitizeModelingCollisionExtent(const FVector& Extent)
	{
		return FVector(
			FMath::Max(0.5f, FMath::Abs(Extent.X)),
			FMath::Max(0.5f, FMath::Abs(Extent.Y)),
			FMath::Max(0.5f, FMath::Abs(Extent.Z)));
	}

	void AddModelingCollisionBoxSpec(
		TArray<FModelingPrimitiveCollisionShapeSpec>& Specs,
		const FString& Kind,
		const FVector& Center,
		const FVector& Extent)
	{
		FModelingPrimitiveCollisionShapeSpec Spec;
		Spec.Shape = TEXT("box");
		Spec.Kind = Kind;
		Spec.Center = Center;
		Spec.Extent = SanitizeModelingCollisionExtent(Extent);
		Specs.Add(Spec);
	}

	void AddModelingCollisionSphereSpec(
		TArray<FModelingPrimitiveCollisionShapeSpec>& Specs,
		const FString& Kind,
		const FVector& Center,
		const double Radius)
	{
		FModelingPrimitiveCollisionShapeSpec Spec;
		Spec.Shape = TEXT("sphere");
		Spec.Kind = Kind;
		Spec.Center = Center;
		Spec.Radius = FMath::Max(0.5, Radius);
		Spec.Extent = FVector(Spec.Radius, Spec.Radius, Spec.Radius);
		Specs.Add(Spec);
	}

	bool TryGetDerivedPrimitiveNumber(const TSharedPtr<FJsonObject>& DerivedPrimitiveData, const TCHAR* FieldName, double& OutValue)
	{
		return DerivedPrimitiveData.IsValid() && DerivedPrimitiveData->TryGetNumberField(FieldName, OutValue);
	}

	bool IsModelingPrimitiveCollisionModeComplexMesh(const FString& Mode)
	{
		const FString Normalized = Mode.TrimStartAndEnd().ToLower();
		return Normalized == TEXT("complex") ||
			Normalized == TEXT("complex_mesh") ||
			Normalized == TEXT("dynamic_mesh_complex") ||
			Normalized == TEXT("mesh_complex");
	}

	bool BuildModelingPrimitiveCollisionShapes(
		const FPrimitiveBoundsPlacement& Placement,
		const TSharedPtr<FJsonObject>& DerivedPrimitiveData,
		const FModelingPrimitiveCollisionOptions& Options,
		TArray<FModelingPrimitiveCollisionShapeSpec>& OutSpecs,
		FString& OutModeUsed,
		FString& OutSource,
		bool& bOutUseDynamicMeshComplex,
		FString& OutError)
	{
		OutSpecs.Reset();
		OutSource = TEXT("actor_box_components");
		bOutUseDynamicMeshComplex = false;
		const FString PrimitiveKind = Placement.PrimitiveKind.TrimStartAndEnd().ToLower();
		const FVector FullSize = Placement.Extent * 2.0f;
		const FString RequestedMode = Options.Mode.IsEmpty() ? TEXT("simple_auto") : Options.Mode;

		if (IsModelingPrimitiveCollisionModeComplexMesh(RequestedMode) ||
			(RequestedMode == TEXT("simple_auto") &&
				(PrimitiveKind == TEXT("modeling_create_cylinder") ||
				 PrimitiveKind == TEXT("modeling_create_ramp") ||
				 PrimitiveKind == TEXT("modeling_create_ramp_corner"))))
		{
			bOutUseDynamicMeshComplex = true;
			OutModeUsed = TEXT("complex_mesh");
			OutSource = TEXT("dynamic_mesh_component");
			return true;
		}

		if (RequestedMode == TEXT("sphere") || RequestedMode == TEXT("sphere_component") ||
			(RequestedMode == TEXT("simple_auto") && PrimitiveKind == TEXT("modeling_create_sphere")))
		{
			const double Radius = FMath::Max3(FMath::Abs(Placement.Extent.X), FMath::Abs(Placement.Extent.Y), FMath::Abs(Placement.Extent.Z));
			AddModelingCollisionSphereSpec(OutSpecs, TEXT("sphere_component"), FVector::ZeroVector, Radius);
			OutModeUsed = TEXT("sphere_component");
			OutSource = TEXT("actor_sphere_component");
			return true;
		}

		if (RequestedMode == TEXT("bounds_box") || RequestedMode == TEXT("box") || RequestedMode == TEXT("box_component"))
		{
			AddModelingCollisionBoxSpec(OutSpecs, TEXT("bounds_box"), FVector::ZeroVector, Placement.Extent);
			OutModeUsed = TEXT("bounds_box");
			return true;
		}

		if (RequestedMode != TEXT("simple_auto") &&
			RequestedMode != TEXT("simple_box_components") &&
			RequestedMode != TEXT("box_components") &&
			RequestedMode != TEXT("stairs_step_boxes") &&
			RequestedMode != TEXT("ramp_step_boxes") &&
			RequestedMode != TEXT("ramp_corner_step_boxes") &&
			RequestedMode != TEXT("sphere") &&
			RequestedMode != TEXT("sphere_component"))
		{
			OutError = TEXT("unsupported_modeling_primitive_collision_mode");
			return false;
		}

		if (PrimitiveKind == TEXT("modeling_create_stairs") || RequestedMode == TEXT("stairs_step_boxes"))
		{
			double StepWidth = 0.0;
			double StepHeight = 0.0;
			double StepDepth = 0.0;
			double NumStepsNumber = 0.0;
			if (!TryGetDerivedPrimitiveNumber(DerivedPrimitiveData, TEXT("StepWidth"), StepWidth) ||
				!TryGetDerivedPrimitiveNumber(DerivedPrimitiveData, TEXT("StepHeight"), StepHeight) ||
				!TryGetDerivedPrimitiveNumber(DerivedPrimitiveData, TEXT("StepDepth"), StepDepth) ||
				!TryGetDerivedPrimitiveNumber(DerivedPrimitiveData, TEXT("NumSteps"), NumStepsNumber))
			{
				OutError = TEXT("invalid_stairs_shape_settings");
				return false;
			}

			const int32 NumSteps = FMath::RoundToInt(NumStepsNumber);
			if (NumSteps < 2 || StepWidth <= 0.0 || StepHeight <= 0.0 || StepDepth <= 0.0)
			{
				OutError = TEXT("invalid_stairs_shape_settings");
				return false;
			}
			if (NumSteps > Options.MaxBoxes)
			{
				OutError = TEXT("modeling_primitive_collision_box_limit_exceeded");
				return false;
			}

			const double TotalRun = StepDepth * NumSteps;
			const double TotalRise = StepHeight * NumSteps;
			const double MinX = -TotalRun * 0.5;
			const double MinZ = -TotalRise * 0.5;
			for (int32 StepIndex = 0; StepIndex < NumSteps; ++StepIndex)
			{
				const double StepSolidHeight = StepHeight * (StepIndex + 1);
				AddModelingCollisionBoxSpec(
					OutSpecs,
					TEXT("stairs_step_box"),
					FVector(
						(float)(MinX + StepDepth * (StepIndex + 0.5)),
						0.0f,
						(float)(MinZ + StepSolidHeight * 0.5)),
					FVector(
						(float)(StepDepth * 0.5),
						(float)(StepWidth * 0.5),
						(float)(StepSolidHeight * 0.5)));
			}
			OutModeUsed = TEXT("stairs_step_boxes");
			return true;
		}

		if (PrimitiveKind == TEXT("modeling_create_ramp") || RequestedMode == TEXT("ramp_step_boxes"))
		{
			const int32 SegmentCount = FMath::Max(1, FMath::CeilToInt((double)FullSize.Z / FMath::Max(1.0, Options.MaxStepHeight)));
			if (SegmentCount > Options.MaxBoxes)
			{
				OutError = TEXT("modeling_primitive_collision_box_limit_exceeded");
				return false;
			}

			const double SegmentDepth = FullSize.X / SegmentCount;
			const double SegmentRise = FullSize.Z / SegmentCount;
			const double MinX = -FullSize.X * 0.5;
			const double MinZ = -FullSize.Z * 0.5;
			for (int32 SegmentIndex = 0; SegmentIndex < SegmentCount; ++SegmentIndex)
			{
				const double SolidHeight = SegmentRise * (SegmentIndex + 1);
				AddModelingCollisionBoxSpec(
					OutSpecs,
					TEXT("ramp_step_box"),
					FVector(
						(float)(MinX + SegmentDepth * (SegmentIndex + 0.5)),
						0.0f,
						(float)(MinZ + SolidHeight * 0.5)),
					FVector(
						(float)(SegmentDepth * 0.5),
						Placement.Extent.Y,
						(float)(SolidHeight * 0.5)));
			}
			OutModeUsed = TEXT("ramp_step_boxes");
			return true;
		}

		if (PrimitiveKind == TEXT("modeling_create_ramp_corner") || RequestedMode == TEXT("ramp_corner_step_boxes"))
		{
			const int32 SegmentCount = FMath::Max(1, FMath::CeilToInt((double)FullSize.Z / FMath::Max(1.0, Options.MaxStepHeight)));
			if (SegmentCount * SegmentCount > Options.MaxBoxes)
			{
				OutError = TEXT("modeling_primitive_collision_box_limit_exceeded");
				return false;
			}

			const double SegmentX = FullSize.X / SegmentCount;
			const double SegmentY = FullSize.Y / SegmentCount;
			const double SegmentRise = FullSize.Z / SegmentCount;
			const double MinX = -FullSize.X * 0.5;
			const double MinY = -FullSize.Y * 0.5;
			const double MinZ = -FullSize.Z * 0.5;
			for (int32 XIndex = 0; XIndex < SegmentCount; ++XIndex)
			{
				for (int32 YIndex = 0; YIndex < SegmentCount; ++YIndex)
				{
					const double SolidHeight = SegmentRise * (FMath::Max(XIndex, YIndex) + 1);
					AddModelingCollisionBoxSpec(
						OutSpecs,
						TEXT("ramp_corner_step_box"),
						FVector(
							(float)(MinX + SegmentX * (XIndex + 0.5)),
							(float)(MinY + SegmentY * (YIndex + 0.5)),
							(float)(MinZ + SolidHeight * 0.5)),
						FVector(
							(float)(SegmentX * 0.5),
							(float)(SegmentY * 0.5),
							(float)(SolidHeight * 0.5)));
				}
			}
			OutModeUsed = TEXT("ramp_corner_step_boxes");
			return true;
		}

		AddModelingCollisionBoxSpec(OutSpecs, TEXT("bounds_box"), FVector::ZeroVector, Placement.Extent);
		OutModeUsed = TEXT("bounds_box");
		return true;
	}

	void DisableDynamicMeshVisualCollision(AActor* Actor, int32& OutDisabledComponentCount)
	{
		OutDisabledComponentCount = 0;
		if (!Actor)
		{
			return;
		}

		TArray<UActorComponent*> Components;
		Actor->GetComponents(UDynamicMeshComponent::StaticClass(), Components);
		for (UActorComponent* Component : Components)
		{
			if (UDynamicMeshComponent* DynamicMeshComponent = Cast<UDynamicMeshComponent>(Component))
			{
				DynamicMeshComponent->Modify();
				DynamicMeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				DynamicMeshComponent->SetCollisionProfileName(FName(TEXT("NoCollision")));
				DynamicMeshComponent->SetComplexAsSimpleCollisionEnabled(false, true);
				DynamicMeshComponent->UpdateOverlaps();
				++OutDisabledComponentCount;
			}
		}
	}

	bool CreateModelingPrimitiveCollisionComponents(
		AActor* Actor,
		const FModelingPrimitiveCollisionOptions& Options,
		const TArray<FModelingPrimitiveCollisionShapeSpec>& Specs,
		TArray<TSharedPtr<FJsonValue>>& OutComponents,
		FString& OutError)
	{
		if (!Actor)
		{
			OutError = TEXT("created_actor_resolution_failed");
			return false;
		}
		if (Specs.Num() > Options.MaxBoxes)
		{
			OutError = TEXT("modeling_primitive_collision_box_limit_exceeded");
			return false;
		}

		USceneComponent* Root = Actor->GetRootComponent();
		if (!Root)
		{
			OutError = TEXT("modeling_primitive_actor_root_missing");
			return false;
		}

		const FName CollisionTagName(*FString::Printf(TEXT("%s%s"), ModelingPrimitiveCollisionTagPrefix, *Actor->GetName()));
		for (int32 Index = 0; Index < Specs.Num(); ++Index)
		{
			const FModelingPrimitiveCollisionShapeSpec& Spec = Specs[Index];
			UPrimitiveComponent* CollisionComponent = nullptr;
			if (Spec.Shape == TEXT("sphere"))
			{
				USphereComponent* SphereComponent = NewObject<USphereComponent>(Actor, *FString::Printf(TEXT("UAI_ModelingCollision_%03d"), Index), RF_Transactional);
				if (SphereComponent)
				{
					SphereComponent->SetSphereRadius(Spec.Radius, false);
				}
				CollisionComponent = SphereComponent;
			}
			else
			{
				UBoxComponent* BoxComponent = NewObject<UBoxComponent>(Actor, *FString::Printf(TEXT("UAI_ModelingCollision_%03d"), Index), RF_Transactional);
				if (BoxComponent)
				{
					BoxComponent->SetBoxExtent(Spec.Extent, false);
				}
				CollisionComponent = BoxComponent;
			}

			if (!CollisionComponent)
			{
				OutError = TEXT("modeling_primitive_collision_component_create_failed");
				return false;
			}

			CollisionComponent->SetMobility(EComponentMobility::Movable);
			CollisionComponent->SetupAttachment(Root);
			CollisionComponent->SetRelativeLocation(Spec.Center);
			CollisionComponent->SetRelativeRotation(FRotator::ZeroRotator);
			CollisionComponent->SetRelativeScale3D(FVector::OneVector);
			CollisionComponent->SetCollisionProfileName(FName(*Options.ProfileName));
			CollisionComponent->SetCollisionEnabled(Options.CollisionEnabled);
			CollisionComponent->SetHiddenInGame(true);
			CollisionComponent->SetVisibility(false, true);
			CollisionComponent->ComponentTags.AddUnique(CollisionTagName);
			Actor->AddInstanceComponent(CollisionComponent);
			CollisionComponent->RegisterComponent();
			CollisionComponent->UpdateOverlaps();

			TSharedPtr<FJsonObject> ComponentObj = MakeShared<FJsonObject>();
			ComponentObj->SetStringField(TEXT("name"), CollisionComponent->GetName());
			ComponentObj->SetStringField(TEXT("shape"), Spec.Shape);
			ComponentObj->SetStringField(TEXT("kind"), Spec.Kind);
			ComponentObj->SetObjectField(TEXT("relative_center"), MakeVectorJson(Spec.Center));
			if (Spec.Shape == TEXT("sphere"))
			{
				ComponentObj->SetNumberField(TEXT("radius"), Spec.Radius);
			}
			else
			{
				ComponentObj->SetObjectField(TEXT("extent"), MakeVectorJson(Spec.Extent));
			}
			OutComponents.Add(MakeShared<FJsonValueObject>(ComponentObj));
		}

		return true;
	}

	bool EnableDynamicMeshComplexCollision(
		AActor* Actor,
		const FModelingPrimitiveCollisionOptions& Options,
		TArray<TSharedPtr<FJsonValue>>& OutComponents,
		FString& OutError)
	{
		if (!Actor)
		{
			OutError = TEXT("created_actor_resolution_failed");
			return false;
		}

		TArray<UActorComponent*> Components;
		Actor->GetComponents(UDynamicMeshComponent::StaticClass(), Components);
		for (UActorComponent* Component : Components)
		{
			UDynamicMeshComponent* DynamicMeshComponent = Cast<UDynamicMeshComponent>(Component);
			if (!DynamicMeshComponent)
			{
				continue;
			}

			DynamicMeshComponent->Modify();
			DynamicMeshComponent->SetCollisionProfileName(FName(*Options.ProfileName));
			DynamicMeshComponent->SetCollisionEnabled(Options.CollisionEnabled);
			DynamicMeshComponent->SetDeferredCollisionUpdatesEnabled(false, false);
			DynamicMeshComponent->SetComplexAsSimpleCollisionEnabled(true, true);
			DynamicMeshComponent->UpdateOverlaps();

			TSharedPtr<FJsonObject> ComponentObj = MakeShared<FJsonObject>();
			ComponentObj->SetStringField(TEXT("name"), DynamicMeshComponent->GetName());
			ComponentObj->SetStringField(TEXT("shape"), TEXT("complex_mesh"));
			ComponentObj->SetStringField(TEXT("kind"), TEXT("dynamic_mesh_complex"));
			ComponentObj->SetBoolField(TEXT("complex_as_simple"), true);
			ComponentObj->SetStringField(TEXT("collision_trace_flag"), TEXT("use_complex_as_simple"));
			OutComponents.Add(MakeShared<FJsonValueObject>(ComponentObj));
		}

		if (OutComponents.Num() <= 0)
		{
			OutError = TEXT("dynamic_mesh_component_not_found");
			return false;
		}
		return true;
	}

	bool AutoGeneratePrimitiveCollisionIfMissing(
		AActor* Actor,
		const FPrimitiveBoundsPlacement& Placement,
		const TSharedPtr<FJsonObject>& DerivedPrimitiveData,
		const TSharedPtr<FJsonObject>& Params,
		TSharedPtr<FJsonObject>& OutData,
		FString& OutError)
	{
		FModelingPrimitiveCollisionOptions Options;
		ParseModelingPrimitiveCollisionOptions(Params, Options);

		TSharedPtr<FJsonObject> CollisionReadback = MakeShared<FJsonObject>();
		CollisionReadback->SetStringField(TEXT("requested_mode"), Options.Mode);
		CollisionReadback->SetStringField(TEXT("profile_name"), Options.ProfileName);
		CollisionReadback->SetStringField(TEXT("enabled"), Options.EnabledText);
		CollisionReadback->SetBoolField(TEXT("replace_existing"), Options.bReplaceExisting);
		CollisionReadback->SetNumberField(TEXT("max_boxes"), Options.MaxBoxes);
		CollisionReadback->SetNumberField(TEXT("max_step_height_cm"), Options.MaxStepHeight);

		if (!Options.bGenerate)
		{
			if (Options.bReplaceExisting)
			{
				RemoveModelingPrimitiveCollisionComponents(Actor);
			}

			int32 DisabledDynamicMeshCollisionCount = 0;
			DisableDynamicMeshVisualCollision(Actor, DisabledDynamicMeshCollisionCount);
			if (Actor)
			{
				Actor->MarkPackageDirty();
			}

			CollisionReadback->SetBoolField(TEXT("generated"), false);
			CollisionReadback->SetBoolField(TEXT("skipped"), true);
			CollisionReadback->SetStringField(TEXT("skip_reason"), TEXT("collision_disabled"));
			CollisionReadback->SetNumberField(TEXT("disabled_dynamic_mesh_collision_components"), DisabledDynamicMeshCollisionCount);
			if (OutData.IsValid())
			{
				OutData->SetObjectField(TEXT("collision"), CollisionReadback);
				OutData->SetBoolField(TEXT("auto_collision_skipped"), true);
				OutData->SetStringField(TEXT("auto_collision_skip_reason"), TEXT("collision_disabled"));
				OutData->SetNumberField(TEXT("disabled_dynamic_mesh_collision_components"), DisabledDynamicMeshCollisionCount);
			}
			return true;
		}

		TArray<FModelingPrimitiveCollisionShapeSpec> Specs;
		FString ModeUsed;
		FString SourceUsed;
		bool bUseDynamicMeshComplex = false;
		if (!BuildModelingPrimitiveCollisionShapes(Placement, DerivedPrimitiveData, Options, Specs, ModeUsed, SourceUsed, bUseDynamicMeshComplex, OutError))
		{
			return false;
		}

		if (Options.bReplaceExisting)
		{
			RemoveModelingPrimitiveCollisionComponents(Actor);
		}

		int32 DisabledDynamicMeshCollisionCount = 0;

		TArray<TSharedPtr<FJsonValue>> ComponentReadback;
		if (bUseDynamicMeshComplex)
		{
			if (!EnableDynamicMeshComplexCollision(Actor, Options, ComponentReadback, OutError))
			{
				return false;
			}
		}
		else
		{
			DisableDynamicMeshVisualCollision(Actor, DisabledDynamicMeshCollisionCount);
			if (!CreateModelingPrimitiveCollisionComponents(Actor, Options, Specs, ComponentReadback, OutError))
			{
				return false;
			}
		}

		if (Actor)
		{
			Actor->MarkPackageDirty();
		}

		int32 BoxCount = 0;
		int32 SphereCount = 0;
		for (const FModelingPrimitiveCollisionShapeSpec& Spec : Specs)
		{
			if (Spec.Shape == TEXT("sphere"))
			{
				++SphereCount;
			}
			else
			{
				++BoxCount;
			}
		}
		const int32 ShapeCount = bUseDynamicMeshComplex ? ComponentReadback.Num() : Specs.Num();
		CollisionReadback->SetStringField(TEXT("mode"), ModeUsed);
		CollisionReadback->SetBoolField(TEXT("generated"), ShapeCount > 0);
		CollisionReadback->SetStringField(TEXT("source"), SourceUsed);
		CollisionReadback->SetNumberField(TEXT("box_count"), BoxCount);
		CollisionReadback->SetNumberField(TEXT("sphere_count"), SphereCount);
		CollisionReadback->SetNumberField(TEXT("complex_component_count"), bUseDynamicMeshComplex ? ComponentReadback.Num() : 0);
		CollisionReadback->SetNumberField(TEXT("shape_count"), ShapeCount);
		CollisionReadback->SetNumberField(TEXT("disabled_dynamic_mesh_collision_components"), DisabledDynamicMeshCollisionCount);
		CollisionReadback->SetArrayField(TEXT("components"), ComponentReadback);
		if (OutData.IsValid())
		{
			OutData->SetObjectField(TEXT("collision"), CollisionReadback);
			OutData->SetBoolField(TEXT("auto_collision_generated"), ShapeCount > 0);
			OutData->SetStringField(TEXT("auto_collision_kind"), ModeUsed);
			OutData->SetNumberField(TEXT("auto_collision_box_count"), BoxCount);
			OutData->SetNumberField(TEXT("auto_collision_shape_count"), ShapeCount);
		}
		return true;
	}

	AActor* FindActorByNameOrLabelLoose(UWorld* World, const FString& NameOrLabel)
	{
		if (!World || NameOrLabel.IsEmpty())
		{
			return nullptr;
		}

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor)
			{
				continue;
			}

			if (Actor->GetName().Equals(NameOrLabel, ESearchCase::IgnoreCase) ||
				Actor->GetActorLabel().Equals(NameOrLabel, ESearchCase::IgnoreCase) ||
				Actor->GetPathName().Equals(NameOrLabel, ESearchCase::IgnoreCase))
			{
				return Actor;
			}
		}

		return nullptr;
	}

	FString NormalizeToolIdentifier(const FString& InValue)
	{
		const FString Value = InValue.TrimStartAndEnd();
		if (Value.IsEmpty())
		{
			return FString();
		}
		if (Value.StartsWith(TEXT("Begin")) || Value.StartsWith(TEXT("PolyEdit_")))
		{
			return Value;
		}
		return FString::Printf(TEXT("Begin%s"), *Value);
	}

	FString ResolveToolIdentifierAlias(const FString& InValue)
	{
		const FString RawValue = InValue.TrimStartAndEnd();
		if (RawValue.IsEmpty())
		{
			return FString();
		}

		static const TMap<FString, FString> ToolAliases = {
			{TEXT("BeginSelectionAction_Extrude"), TEXT("BeginSelectionExtrudeTool")},
			{TEXT("BeginSelectionAction_Offset"), TEXT("BeginSelectionOffsetTool")},
			{TEXT("BeginPolyModelTool_Inset"), TEXT("PolyEdit_Inset")},
			{TEXT("BeginPolyModelTool_PushPull"), TEXT("PolyEdit_PushPull")},
			{TEXT("BeginPolyModelTool_Bevel"), TEXT("PolyEdit_Bevel")},
			{TEXT("BeginPolyModelTool_PolyEd"), TEXT("BeginSelectionPolyEdTool")},
			{TEXT("BeginPolyModelTool_TriSel"), TEXT("BeginSelectionTriEdTool")},
			{TEXT("BeginRampPrimitiveTool"), TEXT("BeginAddRampPrimitiveTool")},
			{TEXT("BeginCornerPrimitiveTool"), TEXT("BeginAddRampCornerPrimitiveTool")},
			{TEXT("BeginRampCornerPrimitiveTool"), TEXT("BeginAddRampCornerPrimitiveTool")}
		};

		if (const FString* Alias = ToolAliases.Find(RawValue))
		{
			return *Alias;
		}

		const FString Normalized = NormalizeToolIdentifier(RawValue);
		if (Normalized.IsEmpty())
		{
			return FString();
		}

		if (const FString* Alias = ToolAliases.Find(Normalized))
		{
			return *Alias;
		}

		return Normalized;
	}

	FString ToObjectPath(const FString& InPath)
	{
		FString ObjectPath = InPath.TrimStartAndEnd();
		if (ObjectPath.IsEmpty())
		{
			return FString();
		}
		if (ObjectPath.Contains(TEXT(".")))
		{
			return ObjectPath;
		}

		const FString AssetName = FPackageName::GetLongPackageAssetName(ObjectPath);
		return AssetName.IsEmpty() ? FString() : FString::Printf(TEXT("%s.%s"), *ObjectPath, *AssetName);
	}

	UObject* LoadAssetObject(const FString& InPath)
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

	UStaticMesh* LoadStaticMeshAsset(const FString& InPath)
	{
		return Cast<UStaticMesh>(LoadAssetObject(InPath));
	}

	FEditorViewportClient* GetFocusedViewportClient()
	{
		FEditorModeTools& ModeTools = GLevelEditorModeTools();
		if (FEditorViewportClient* FocusedViewportClient = ModeTools.GetFocusedViewportClient())
		{
			if (FocusedViewportClient->Viewport)
			{
				return FocusedViewportClient;
			}
		}
		return nullptr;
	}

	FEditorViewportClient* GetPreferredViewportClient()
	{
		if (FEditorViewportClient* FocusedViewportClient = GetFocusedViewportClient())
		{
			return FocusedViewportClient;
		}

		if (GCurrentLevelEditingViewportClient && GCurrentLevelEditingViewportClient->Viewport)
		{
			return GCurrentLevelEditingViewportClient;
		}

		if (GEditor)
		{
			for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
			{
				if (ViewportClient && ViewportClient->Viewport && ViewportClient->ViewportType == LVT_Perspective)
				{
					return ViewportClient;
				}
			}
			for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
			{
				if (ViewportClient && ViewportClient->Viewport)
				{
					return ViewportClient;
				}
			}
		}
		return nullptr;
	}

	FEditorViewportClient* ResolveTickViewportClient(FEditorViewportClient* ViewportClient)
	{
		if (FEditorViewportClient* FocusedViewportClient = GetFocusedViewportClient())
		{
			return FocusedViewportClient;
		}
		return (ViewportClient && ViewportClient->Viewport) ? ViewportClient : GetPreferredViewportClient();
	}

	void EnsureViewportClientFocused(FEditorViewportClient* ViewportClient)
	{
		if (!ViewportClient || !ViewportClient->Viewport)
		{
			return;
		}

		FEditorModeTools& ModeTools = GLevelEditorModeTools();
		if (ModeTools.GetFocusedViewportClient() != ViewportClient)
		{
			ModeTools.ReceivedFocus(ViewportClient, ViewportClient->Viewport);
		}
	}

	FString ElementTypeToString(UGeometrySelectionManager::EGeometryElementType ElementType)
	{
		using EElement = UGeometrySelectionManager::EGeometryElementType;
		switch (ElementType)
		{
		case EElement::Vertex: return TEXT("vertex");
		case EElement::Edge: return TEXT("edge");
		case EElement::Face: return TEXT("face");
		default: return TEXT("unknown");
		}
	}

	FString TopologyModeToString(UGeometrySelectionManager::EMeshTopologyMode Mode)
	{
		switch (Mode)
		{
		case UGeometrySelectionManager::EMeshTopologyMode::Triangle: return TEXT("triangle");
		case UGeometrySelectionManager::EMeshTopologyMode::Polygroup: return TEXT("polygroup");
		default: return TEXT("none");
		}
	}

	UGeometrySelectionManager::EGeometryElementType ParseElementType(const FString& InValue)
	{
		const FString Value = InValue.TrimStartAndEnd().ToLower();
		if (Value == TEXT("vertex") || Value == TEXT("vertices"))
		{
			return UGeometrySelectionManager::EGeometryElementType::Vertex;
		}
		if (Value == TEXT("edge") || Value == TEXT("edges"))
		{
			return UGeometrySelectionManager::EGeometryElementType::Edge;
		}
		return UGeometrySelectionManager::EGeometryElementType::Face;
	}

	UGeometrySelectionManager::EMeshTopologyMode ParseTopologyMode(const FString& InValue)
	{
		const FString Value = InValue.TrimStartAndEnd().ToLower();
		if (Value == TEXT("polygroup") || Value == TEXT("group"))
		{
			return UGeometrySelectionManager::EMeshTopologyMode::Polygroup;
		}
		if (Value == TEXT("none"))
		{
			return UGeometrySelectionManager::EMeshTopologyMode::None;
		}
		return UGeometrySelectionManager::EMeshTopologyMode::Triangle;
	}

	EGeometrySelectionChangeType ParseSelectionChangeType(const FString& InValue)
	{
		const FString Value = InValue.TrimStartAndEnd().ToLower();
		if (Value == TEXT("add") || Value == TEXT("append"))
		{
			return EGeometrySelectionChangeType::Add;
		}
		if (Value == TEXT("remove") || Value == TEXT("subtract") || Value == TEXT("toggle_off"))
		{
			return EGeometrySelectionChangeType::Remove;
		}
		return EGeometrySelectionChangeType::Replace;
	}

	FString SelectionChangeTypeToString(EGeometrySelectionChangeType InValue)
	{
		switch (InValue)
		{
		case EGeometrySelectionChangeType::Add:
			return TEXT("add");
		case EGeometrySelectionChangeType::Remove:
			return TEXT("remove");
		case EGeometrySelectionChangeType::Replace:
		default:
			return TEXT("replace");
		}
	}

	bool ResolveViewportScreenPoint(const FEditorViewportClient* ViewportClient, const TSharedPtr<FJsonObject>& Params, FVector2D& OutScreenPoint, FString& OutError)
	{
		if (!ViewportClient || !ViewportClient->Viewport)
		{
			OutError = TEXT("editor_viewport_not_found");
			return false;
		}

		double ScreenX = 0.0;
		double ScreenY = 0.0;
		if (!Params.IsValid() || !Params->TryGetNumberField(TEXT("screen_x"), ScreenX) || !Params->TryGetNumberField(TEXT("screen_y"), ScreenY))
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

	bool DeprojectViewportScreenPoint(const FModelingContext& Context, const FVector2D& ScreenPoint, FRay3d& OutWorldRay, FString& OutError)
	{
		if (!Context.ViewportClient || !Context.ViewportClient->Viewport)
		{
			OutError = TEXT("editor_viewport_not_found");
			return false;
		}

		UWorld* World = Context.ViewportClient->GetWorld();
		if (!World)
		{
			OutError = TEXT("editor_viewport_world_not_found");
			return false;
		}

		FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
			Context.ViewportClient->Viewport,
			World->Scene,
			Context.ViewportClient->EngineShowFlags).SetRealtimeUpdate(Context.ViewportClient->IsRealtime()));
		FSceneView* SceneView = Context.ViewportClient->CalcSceneView(&ViewFamily);
		if (!SceneView)
		{
			OutError = TEXT("calc_scene_view_failed");
			return false;
		}

		FVector WorldOrigin = FVector::ZeroVector;
		FVector WorldDirection = FVector::ForwardVector;
		SceneView->DeprojectFVector2D(ScreenPoint, WorldOrigin, WorldDirection);
		OutWorldRay = FRay3d(WorldOrigin, WorldDirection.GetSafeNormal());
		return true;
	}

	bool ResolveWorldRayFromParams(const TSharedPtr<FJsonObject>& Params, FRay3d& OutWorldRay, FString& OutError)
	{
		if (!Params.IsValid())
		{
			OutError = TEXT("missing_params");
			return false;
		}

		const TSharedPtr<FJsonValue>* OriginValue = UeAgentJsonDiagnostics::FindFieldValue(Params, TEXT("world_origin"));
		const TSharedPtr<FJsonValue>* DirectionValue = UeAgentJsonDiagnostics::FindFieldValue(Params, TEXT("world_direction"));
		if (!OriginValue)
		{
			OutError = TEXT("missing_world_origin");
			return false;
		}
		if (!DirectionValue)
		{
			OutError = TEXT("missing_world_direction");
			return false;
		}

		FVector Origin = FVector::ZeroVector;
		FVector Direction = FVector::ZeroVector;
		if (!UeAgentJsonDiagnostics::TryReadVectorValue(*OriginValue, Origin))
		{
			OutError = TEXT("invalid_world_origin");
			return false;
		}
		if (!UeAgentJsonDiagnostics::TryReadVectorValue(*DirectionValue, Direction))
		{
			OutError = TEXT("invalid_world_direction");
			return false;
		}

		if (Direction.IsNearlyZero())
		{
			OutError = TEXT("invalid_world_direction");
			return false;
		}

		OutWorldRay = FRay3d(Origin, Direction.GetSafeNormal());
		return true;
	}

	void FillSelectionUpdateJson(const UGeometrySelectionManager* SelectionManager, const FGeometrySelectionUpdateConfig& UpdateConfig, const FGeometrySelectionUpdateResult& Result, TSharedPtr<FJsonObject>& OutData)
	{
		OutData->SetStringField(TEXT("change_type"), SelectionChangeTypeToString(UpdateConfig.ChangeType));
		OutData->SetBoolField(TEXT("selection_missed"), Result.bSelectionMissed);
		OutData->SetBoolField(TEXT("selection_modified"), Result.bSelectionModified);
		OutData->SetNumberField(TEXT("delta_added_count"), Result.SelectionDelta.Added.Num());
		OutData->SetNumberField(TEXT("delta_removed_count"), Result.SelectionDelta.Removed.Num());

		UGeometrySelectionManager::EGeometryTopologyType TopologyType = UGeometrySelectionManager::EGeometryTopologyType::Triangle;
		UGeometrySelectionManager::EGeometryElementType ElementType = UGeometrySelectionManager::EGeometryElementType::Face;
		int32 NumTargets = 0;
		bool bIsEmpty = true;
		if (SelectionManager)
		{
			SelectionManager->GetActiveSelectionInfo(TopologyType, ElementType, NumTargets, bIsEmpty);
		}

		OutData->SetStringField(TEXT("element_type"), SelectionManager ? ElementTypeToString(ElementType) : TEXT("face"));
		OutData->SetStringField(TEXT("topology_mode"), SelectionManager ? TopologyModeToString(SelectionManager->GetMeshTopologyMode()) : TEXT("triangle"));
		OutData->SetNumberField(TEXT("target_count"), NumTargets);
		OutData->SetBoolField(TEXT("is_empty"), bIsEmpty);
		OutData->SetBoolField(TEXT("has_active_targets"), SelectionManager ? SelectionManager->HasActiveTargets() : false);
	}

	FProperty* FindPropertyByNameLoose(UStruct* StructType, const FString& Segment)
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

	bool ResolvePropertyPath(UObject* Obj, const FString& PropertyPath, FProperty*& OutProperty, void*& OutValuePtr)
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

	bool ImportResolvedProperty(UObject* Object, const FString& PropertyPath, const FString& ValueText, FString& OutError, FString* OutAppliedValueText, FString* OutCppType)
	{
		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		if (!ResolvePropertyPath(Object, PropertyPath, Property, ValuePtr) || !Property || !ValuePtr)
		{
			OutError = TEXT("property_not_found");
			return false;
		}

		if (OutCppType)
		{
			*OutCppType = Property->GetCPPType();
		}
		Object->Modify();
		Object->PreEditChange(Property);
		const TCHAR* Buffer = *ValueText;
		if (!Property->ImportText_Direct(Buffer, ValuePtr, Object, PPF_None))
		{
			OutError = FString::Printf(TEXT("property_import_failed:%s:%s"), *PropertyPath, *ValueText);
			return false;
		}

		FPropertyChangedEvent ChangedEvent(Property, EPropertyChangeType::ValueSet);
		Object->PostEditChangeProperty(ChangedEvent);
		Object->MarkPackageDirty();
		if (OutAppliedValueText)
		{
			Property->ExportTextItem_Direct(*OutAppliedValueText, ValuePtr, nullptr, Object, PPF_None);
		}
		return true;
	}

	void NotifyToolPropertySetUpdated(UInteractiveTool* Tool, UObject* PropertySet)
	{
		if (!Tool || !PropertySet)
		{
			return;
		}

		if (UInteractiveToolPropertySet* InteractivePropertySet = Cast<UInteractiveToolPropertySet>(PropertySet))
		{
			InteractivePropertySet->CheckAndUpdateWatched();
		}
	}

	bool MatchPropertySet(UObject* PropertySet, const FString& Selector)
	{
		if (!PropertySet)
		{
			return false;
		}
		if (Selector.IsEmpty())
		{
			return true;
		}

		const FString Value = Selector.TrimStartAndEnd();
		return PropertySet->GetName().Equals(Value, ESearchCase::IgnoreCase) ||
			PropertySet->GetClass()->GetName().Equals(Value, ESearchCase::IgnoreCase) ||
			PropertySet->GetClass()->GetPathName().Equals(Value, ESearchCase::IgnoreCase);
	}

	UObject* FindToolPropertySet(UInteractiveTool* Tool, const FString& Selector)
	{
		if (!Tool)
		{
			return nullptr;
		}

		const TArray<UObject*> PropertySets = Tool->GetToolProperties(false);
		for (UObject* PropertySet : PropertySets)
		{
			if (MatchPropertySet(PropertySet, Selector))
			{
				return PropertySet;
			}
		}
		return nullptr;
	}

	void AppendPropertySetJson(UObject* PropertySet, TArray<TSharedPtr<FJsonValue>>& OutPropertySets)
	{
		if (!PropertySet)
		{
			return;
		}

		TSharedPtr<FJsonObject> PropertySetObj = MakeShared<FJsonObject>();
		PropertySetObj->SetStringField(TEXT("name"), PropertySet->GetName());
		PropertySetObj->SetStringField(TEXT("class"), PropertySet->GetClass()->GetPathName());

		TArray<TSharedPtr<FJsonValue>> Properties;
		for (TFieldIterator<FProperty> It(PropertySet->GetClass(), EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property || Property->HasAnyPropertyFlags(CPF_Transient | CPF_DisableEditOnInstance))
			{
				continue;
			}

			FString ValueText;
			Property->ExportTextItem_Direct(ValueText, Property->ContainerPtrToValuePtr<void>(PropertySet), nullptr, PropertySet, PPF_None);

			TSharedPtr<FJsonObject> PropertyObj = MakeShared<FJsonObject>();
			PropertyObj->SetStringField(TEXT("name"), Property->GetName());
			PropertyObj->SetStringField(TEXT("display_name"), Property->GetAuthoredName());
			PropertyObj->SetStringField(TEXT("property_class"), Property->GetClass()->GetName());
			PropertyObj->SetStringField(TEXT("value_text"), ValueText);
			Properties.Add(MakeShared<FJsonValueObject>(PropertyObj));
		}

		PropertySetObj->SetArrayField(TEXT("properties"), Properties);
		OutPropertySets.Add(MakeShared<FJsonValueObject>(PropertySetObj));
	}

	bool GetModelingContext(FModelingContext& OutContext, FString& OutError)
	{
		UModelingToolsEditorModeSettings* ModelingSettings = GetMutableDefault<UModelingToolsEditorModeSettings>();
		const bool bNeedEnableMeshSelections = (ModelingSettings && !ModelingSettings->GetMeshSelectionsEnabled());
		if (bNeedEnableMeshSelections)
		{
			ModelingSettings->SetMeshSelectionsEnabled(true);
			ModelingSettings->SaveConfig();
		}

		FEditorModeTools& ModeTools = GLevelEditorModeTools();
		if (bNeedEnableMeshSelections &&
			ModeTools.IsModeActive(UModelingToolsEditorMode::EM_ModelingToolsEditorModeId))
		{
			ModeTools.DeactivateMode(UModelingToolsEditorMode::EM_ModelingToolsEditorModeId);
		}
		ModeTools.ActivateMode(UModelingToolsEditorMode::EM_ModelingToolsEditorModeId);

		UModelingToolsEditorMode* Mode = Cast<UModelingToolsEditorMode>(ModeTools.GetActiveScriptableMode(UModelingToolsEditorMode::EM_ModelingToolsEditorModeId));
		if (!Mode)
		{
			OutError = TEXT("modeling_mode_unavailable");
			return false;
		}

		UEditorInteractiveToolsContext* ToolsContext = Mode->GetInteractiveToolsContext();
		if (!ToolsContext)
		{
			OutError = TEXT("modeling_tools_context_unavailable");
			return false;
		}

		FEditorViewportClient* ViewportClient = GetPreferredViewportClient();
		if (!ViewportClient)
		{
			OutError = TEXT("editor_viewport_not_found");
			return false;
		}

		OutContext.Mode = Mode;
		OutContext.ToolsContext = ToolsContext;
		OutContext.ViewportClient = ViewportClient;

		return true;
	}

	void FlushToolsContext(const FModelingContext& Context, int32 NumTicks)
	{
		if (!Context.ToolsContext || !Context.ViewportClient)
		{
			return;
		}

		FEditorViewportClient* TickViewportClient = ResolveTickViewportClient(Context.ViewportClient);
		EnsureViewportClientFocused(TickViewportClient);

		for (int32 Index = 0; Index < NumTicks; ++Index)
		{
			if (Context.ToolsContext)
			{
				Context.ToolsContext->Tick(TickViewportClient, 0.0f);
			}
			if (Context.Mode)
			{
				Context.Mode->Tick(TickViewportClient, 0.0f);
			}
		}

		if (TickViewportClient && TickViewportClient->Viewport)
		{
			TickViewportClient->Viewport->InvalidateDisplay();
		}
		if (TickViewportClient)
		{
			TickViewportClient->Invalidate();
		}
		if (GEditor)
		{
			GEditor->RedrawAllViewports(false);
		}
	}

	void FillActiveToolData(const FModelingContext& Context, TSharedPtr<FJsonObject>& OutData)
	{
		UInteractiveToolManager* ToolManager = Context.ToolsContext ? Context.ToolsContext->ToolManager : nullptr;
		UInteractiveTool* Tool = ToolManager ? ToolManager->GetActiveTool(EToolSide::Left) : nullptr;

		OutData->SetBoolField(TEXT("has_active_tool"), Tool != nullptr);
		OutData->SetStringField(TEXT("active_tool_name"), Context.ToolsContext ? Context.ToolsContext->GetActiveToolName() : TEXT(""));
		OutData->SetBoolField(TEXT("can_accept"), Context.ToolsContext ? Context.ToolsContext->CanAcceptActiveTool() : false);
		OutData->SetBoolField(TEXT("can_cancel"), Context.ToolsContext ? Context.ToolsContext->CanCancelActiveTool() : false);
		OutData->SetBoolField(TEXT("tool_has_accept"), Context.ToolsContext ? Context.ToolsContext->ActiveToolHasAccept() : false);

		TArray<TSharedPtr<FJsonValue>> PropertySets;
		if (Tool)
		{
			for (UObject* PropertySet : Tool->GetToolProperties(false))
			{
				AppendPropertySetJson(PropertySet, PropertySets);
			}
		}
		OutData->SetArrayField(TEXT("property_sets"), PropertySets);
	}

	bool StartToolByIdentifier(const FString& ToolIdentifier, const FModelingContext& Context, FString& OutError)
	{
		if (!Context.ToolsContext)
		{
			OutError = TEXT("modeling_tools_context_unavailable");
			return false;
		}

		if (Context.ToolsContext->HasActiveTool())
		{
			OutError = TEXT("active_tool_exists");
			return false;
		}

		if (!Context.ToolsContext->CanStartTool(ToolIdentifier))
		{
			OutError = TEXT("cannot_start_tool");
			return false;
		}

		bool bObservedToolEnd = false;
		FString EndedToolName;
		FString EndedShutdownType = TEXT("");
		bool bObservedUnexpectedShutdown = false;
		bool bUnexpectedShutdownDuringSetup = false;
		FString UnexpectedShutdownMessage;
		FDelegateHandle ToolEndedHandle;
		FDelegateHandle UnexpectedShutdownHandle;
		if (Context.ToolsContext->ToolManager)
		{
			ToolEndedHandle = Context.ToolsContext->ToolManager->OnToolEndedWithStatus.AddLambda(
				[&bObservedToolEnd, &EndedToolName, &EndedShutdownType](UInteractiveToolManager*, UInteractiveTool* EndedTool, EToolShutdownType ShutdownType)
				{
					bObservedToolEnd = true;
					EndedToolName = EndedTool ? EndedTool->GetClass()->GetName() : TEXT("");
					switch (ShutdownType)
					{
					case EToolShutdownType::Accept:
						EndedShutdownType = TEXT("Accept");
						break;
					case EToolShutdownType::Cancel:
						EndedShutdownType = TEXT("Cancel");
						break;
					case EToolShutdownType::Completed:
						EndedShutdownType = TEXT("Completed");
						break;
					default:
						EndedShutdownType = TEXT("Unknown");
						break;
					}
				});
			UnexpectedShutdownHandle = Context.ToolsContext->ToolManager->OnToolUnexpectedShutdownMessage.AddLambda(
				[&bObservedUnexpectedShutdown, &bUnexpectedShutdownDuringSetup, &UnexpectedShutdownMessage](UInteractiveToolManager*, UInteractiveTool*, const FText& Message, bool bWasDuringToolSetup)
				{
					bObservedUnexpectedShutdown = true;
					bUnexpectedShutdownDuringSetup = bWasDuringToolSetup;
					UnexpectedShutdownMessage = Message.ToString();
				});
		}

		Context.ToolsContext->StartTool(ToolIdentifier);
		if (!Context.ToolsContext->HasActiveTool())
		{
			const double StartTime = FPlatformTime::Seconds();
			while (!Context.ToolsContext->HasActiveTool() && (FPlatformTime::Seconds() - StartTime) < 5.0)
			{
				FlushToolsContext(Context, 1);
				FPlatformProcess::Sleep(0.01f);
			}

			if (!Context.ToolsContext->HasActiveTool())
			{
				FlushToolsContext(Context, 3);
				UInteractiveToolManager* ToolManager = Context.ToolsContext->ToolManager;
				UInteractiveTool* LeftTool = ToolManager ? ToolManager->GetActiveTool(EToolSide::Left) : nullptr;
				UInteractiveTool* MouseTool = ToolManager ? ToolManager->GetActiveTool(EToolSide::Mouse) : nullptr;
				const bool bHasLeftTool = ToolManager ? ToolManager->HasActiveTool(EToolSide::Left) : false;
				const bool bHasMouseTool = ToolManager ? ToolManager->HasActiveTool(EToolSide::Mouse) : false;
				const bool bHasAnyTool = ToolManager ? ToolManager->HasAnyActiveTool() : false;
				const bool bCanAccept = Context.ToolsContext->CanAcceptActiveTool();
				const bool bCanCancel = Context.ToolsContext->CanCancelActiveTool();
				const bool bCanComplete = Context.ToolsContext->CanCompleteActiveTool();
				FUeAgentInterfaceLogger::Log(
					TEXT("ModelingStartToolState tool=%s editor_has_active=%s left_has=%s mouse_has=%s any_has=%s active_name=%s left_tool=%s mouse_tool=%s can_accept=%s can_cancel=%s can_complete=%s"),
					*ToolIdentifier,
					Context.ToolsContext->HasActiveTool() ? TEXT("true") : TEXT("false"),
					bHasLeftTool ? TEXT("true") : TEXT("false"),
					bHasMouseTool ? TEXT("true") : TEXT("false"),
					bHasAnyTool ? TEXT("true") : TEXT("false"),
					*Context.ToolsContext->GetActiveToolName(),
					LeftTool ? *LeftTool->GetClass()->GetName() : TEXT(""),
					MouseTool ? *MouseTool->GetClass()->GetName() : TEXT(""),
					bCanAccept ? TEXT("true") : TEXT("false"),
					bCanCancel ? TEXT("true") : TEXT("false"),
					bCanComplete ? TEXT("true") : TEXT("false"));
				FUeAgentInterfaceLogger::Log(
					TEXT("ModelingStartToolEndState tool=%s observed_end=%s ended_tool=%s shutdown=%s"),
					*ToolIdentifier,
					bObservedToolEnd ? TEXT("true") : TEXT("false"),
					*EndedToolName,
					*EndedShutdownType);
				FUeAgentInterfaceLogger::Log(
					TEXT("ModelingStartToolUnexpectedState tool=%s observed=%s during_setup=%s message=%s"),
					*ToolIdentifier,
					bObservedUnexpectedShutdown ? TEXT("true") : TEXT("false"),
					bUnexpectedShutdownDuringSetup ? TEXT("true") : TEXT("false"),
					*UnexpectedShutdownMessage);
				if (ToolManager && ToolEndedHandle.IsValid())
				{
					ToolManager->OnToolEndedWithStatus.Remove(ToolEndedHandle);
				}
				if (ToolManager && UnexpectedShutdownHandle.IsValid())
				{
					ToolManager->OnToolUnexpectedShutdownMessage.Remove(UnexpectedShutdownHandle);
				}
				OutError = TEXT("tool_start_failed");
				return false;
			}
		}
		if (Context.ToolsContext->ToolManager && ToolEndedHandle.IsValid())
		{
			Context.ToolsContext->ToolManager->OnToolEndedWithStatus.Remove(ToolEndedHandle);
		}
		if (Context.ToolsContext->ToolManager && UnexpectedShutdownHandle.IsValid())
		{
			Context.ToolsContext->ToolManager->OnToolUnexpectedShutdownMessage.Remove(UnexpectedShutdownHandle);
		}
		return true;
	}

	bool IsPrimitiveCreationTool(const FString& ToolIdentifier)
	{
		return ToolIdentifier == TEXT("BeginAddBoxPrimitiveTool")
			|| ToolIdentifier == TEXT("BeginAddCylinderPrimitiveTool")
			|| ToolIdentifier == TEXT("BeginAddSpherePrimitiveTool")
			|| ToolIdentifier == TEXT("BeginAddRectanglePrimitiveTool")
			|| ToolIdentifier == TEXT("BeginAddStairsPrimitiveTool")
			|| ToolIdentifier == TEXT("BeginAddRampPrimitiveTool")
			|| ToolIdentifier == TEXT("BeginAddRampCornerPrimitiveTool");
	}

	bool HasExplicitTargetSurfaceOverride(const TSharedPtr<FJsonObject>& Params)
	{
		if (!Params.IsValid())
		{
			return false;
		}

		FString PropertySet;
		FString PropertyName;
		if (Params->TryGetStringField(TEXT("property_set"), PropertySet) && Params->TryGetStringField(TEXT("property_name"), PropertyName))
		{
			return PropertySet.Equals(TEXT("ShapeSettings"), ESearchCase::IgnoreCase)
				&& PropertyName.Equals(TEXT("TargetSurface"), ESearchCase::IgnoreCase);
		}

		FString IgnoredValue;
		return TryGetExplicitToolPropertyValue(Params, TEXT("ShapeSettings"), TEXT("TargetSurface"), IgnoredValue);
	}

	void ConfigureDefaultModelingOutputType()
	{
		UCreateMeshObjectTypeProperties::bEnableDynamicMeshActorSupport = true;
		UCreateMeshObjectTypeProperties::DefaultObjectTypeIdentifier = UCreateMeshObjectTypeProperties::DynamicMeshActorIdentifier;
	}

	bool IsCreateMeshObjectTypePropertySetSelector(const FString& Selector)
	{
		return Selector.IsEmpty()
			|| Selector.Equals(TEXT("CreateMeshObjectTypeProperties"), ESearchCase::IgnoreCase)
			|| Selector.Equals(TEXT("UCreateMeshObjectTypeProperties"), ESearchCase::IgnoreCase)
			|| Selector.Equals(TEXT("CreateMeshObjectType"), ESearchCase::IgnoreCase)
			|| Selector.Equals(TEXT("OutputTypeProperties"), ESearchCase::IgnoreCase);
	}

	bool HasExplicitOutputTypeOverride(const TSharedPtr<FJsonObject>& Params)
	{
		if (!Params.IsValid())
		{
			return false;
		}

		FString PropertySet;
		FString PropertyName;
		if (Params->TryGetStringField(TEXT("property_set"), PropertySet) && Params->TryGetStringField(TEXT("property_name"), PropertyName))
		{
			return PropertyName.Equals(TEXT("OutputType"), ESearchCase::IgnoreCase)
				&& IsCreateMeshObjectTypePropertySetSelector(PropertySet);
		}

		const TArray<TSharedPtr<FJsonValue>>* PropertiesArray = nullptr;
		if (!Params->TryGetArrayField(TEXT("tool_properties"), PropertiesArray) || !PropertiesArray)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& EntryValue : *PropertiesArray)
		{
			const TSharedPtr<FJsonObject> Entry = EntryValue.IsValid() ? EntryValue->AsObject() : nullptr;
			if (!Entry.IsValid())
			{
				continue;
			}

			FString EntrySet;
			FString EntryName;
			Entry->TryGetStringField(TEXT("property_set"), EntrySet);
			Entry->TryGetStringField(TEXT("property_name"), EntryName);
			if (EntryName.Equals(TEXT("OutputType"), ESearchCase::IgnoreCase) && IsCreateMeshObjectTypePropertySetSelector(EntrySet))
			{
				return true;
			}
		}
		return false;
	}

	bool AutoPrepareModelingOutputType(const TSharedPtr<FJsonObject>& Params, UInteractiveTool* Tool, FString& OutError, TSharedPtr<FJsonObject>& OutData)
	{
		if (!Tool || HasExplicitOutputTypeOverride(Params))
		{
			return true;
		}

		UObject* PropertySet = FindToolPropertySet(Tool, TEXT("CreateMeshObjectTypeProperties"));
		if (!PropertySet)
		{
			if (OutData.IsValid())
			{
				OutData->SetBoolField(TEXT("default_output_type_applied"), false);
				OutData->SetStringField(TEXT("default_output_type_skip_reason"), TEXT("missing_create_mesh_output_type_property"));
			}
			return true;
		}

		FString AppliedValueText;
		FString CppType;
		if (!ImportResolvedProperty(
			PropertySet,
			TEXT("OutputType"),
			UCreateMeshObjectTypeProperties::DynamicMeshActorIdentifier,
			OutError,
			&AppliedValueText,
			&CppType))
		{
			return false;
		}

		NotifyToolPropertySetUpdated(Tool, PropertySet);
		if (OutData.IsValid())
		{
			OutData->SetBoolField(TEXT("default_output_type_applied"), true);
			OutData->SetStringField(TEXT("default_output_type"), UCreateMeshObjectTypeProperties::DynamicMeshActorIdentifier);
			OutData->SetStringField(TEXT("default_output_type_applied_value"), AppliedValueText);
			OutData->SetStringField(TEXT("default_output_type_cpp_type"), CppType);
		}
		return true;
	}

	bool AutoPreparePrimitiveCreationTool(const TSharedPtr<FJsonObject>& Params, UInteractiveTool* Tool, const FString& ToolIdentifier, FString& OutError)
	{
		if (!Tool || !IsPrimitiveCreationTool(ToolIdentifier) || HasExplicitTargetSurfaceOverride(Params))
		{
			return true;
		}

		UObject* PropertySet = FindToolPropertySet(Tool, TEXT("ShapeSettings"));
		if (!PropertySet)
		{
			return true;
		}

		if (!ImportResolvedProperty(PropertySet, TEXT("TargetSurface"), TEXT("AtOrigin"), OutError, nullptr, nullptr))
		{
			return false;
		}

		NotifyToolPropertySetUpdated(Tool, PropertySet);
		return true;
	}

	bool WaitForToolShutdownReady(const FModelingContext& Context, EToolShutdownType ShutdownType, double TimeoutSeconds)
	{
		UEditorInteractiveToolsContext* ToolsContext = Context.ToolsContext;
		FEditorViewportClient* ViewportClient = Context.ViewportClient;
		if (!ToolsContext || !ViewportClient)
		{
			return false;
		}

		const double StartTime = FPlatformTime::Seconds();

		while (ToolsContext->HasActiveTool() && (FPlatformTime::Seconds() - StartTime) < TimeoutSeconds)
		{
			FEditorViewportClient* TickViewportClient = ResolveTickViewportClient(ViewportClient);
			EnsureViewportClientFocused(TickViewportClient);

			if (ShutdownType == EToolShutdownType::Accept)
			{
				if (ToolsContext->CanAcceptActiveTool() || ToolsContext->CanCompleteActiveTool())
				{
					return true;
				}
			}
			else if (ShutdownType == EToolShutdownType::Cancel)
			{
				if (ToolsContext->CanCancelActiveTool() || ToolsContext->CanCompleteActiveTool())
				{
					return true;
				}
			}
			else
			{
				return true;
			}

			ToolsContext->Tick(TickViewportClient, 0.05f);
			if (Context.Mode)
			{
				Context.Mode->Tick(TickViewportClient, 0.05f);
			}
			if (TickViewportClient && TickViewportClient->Viewport)
			{
				TickViewportClient->Viewport->InvalidateDisplay();
			}
			if (TickViewportClient)
			{
				TickViewportClient->Invalidate();
			}
			if (GEditor)
			{
				GEditor->RedrawAllViewports(false);
			}
			FPlatformProcess::Sleep(0.02f);
		}

		FlushToolsContext(Context, 6);
		if (ShutdownType == EToolShutdownType::Accept)
		{
			return ToolsContext->CanAcceptActiveTool() || ToolsContext->CanCompleteActiveTool();
		}
		if (ShutdownType == EToolShutdownType::Cancel)
		{
			return ToolsContext->CanCancelActiveTool() || ToolsContext->CanCompleteActiveTool();
		}
		return true;
	}

	bool FinishTool(const FModelingContext& Context, EToolShutdownType ShutdownType, FString& OutError)
	{
		UEditorInteractiveToolsContext* ToolsContext = Context.ToolsContext;
		FEditorViewportClient* ViewportClient = Context.ViewportClient;
		if (!ToolsContext)
		{
			OutError = TEXT("modeling_tools_context_unavailable");
			return false;
		}

		if (!ToolsContext->HasActiveTool())
		{
			OutError = TEXT("no_active_tool");
			return false;
		}

		UInteractiveTool* ActiveTool = ToolsContext->ToolManager ? ToolsContext->ToolManager->GetActiveTool(EToolSide::Mouse) : nullptr;
		if (ActiveTool)
		{
			IInteractiveToolNestedAcceptCancelAPI* NestedAcceptCancelAPI = Cast<IInteractiveToolNestedAcceptCancelAPI>(ActiveTool);
			if (ShutdownType == EToolShutdownType::Accept &&
				NestedAcceptCancelAPI &&
				NestedAcceptCancelAPI->SupportsNestedAcceptCommand() &&
				NestedAcceptCancelAPI->CanCurrentlyNestedAccept())
			{
				if (NestedAcceptCancelAPI->ExecuteNestedAcceptCommand())
				{
					FlushToolsContext(Context, 3);
					if (!ToolsContext->HasActiveTool())
					{
						return true;
					}
				}
			}
			if (ShutdownType == EToolShutdownType::Cancel &&
				NestedAcceptCancelAPI &&
				NestedAcceptCancelAPI->SupportsNestedCancelCommand() &&
				NestedAcceptCancelAPI->CanCurrentlyNestedCancel())
			{
				if (NestedAcceptCancelAPI->ExecuteNestedCancelCommand())
				{
					FlushToolsContext(Context, 3);
					if (!ToolsContext->HasActiveTool())
					{
						return true;
					}
				}
			}
		}

		EToolShutdownType ResolvedShutdownType = ShutdownType;

		if (ShutdownType == EToolShutdownType::Accept)
		{
			WaitForToolShutdownReady(Context, ShutdownType, 8.0);
			if (ToolsContext->CanAcceptActiveTool())
			{
				ResolvedShutdownType = EToolShutdownType::Accept;
			}
			else if (ToolsContext->CanCompleteActiveTool())
			{
				ResolvedShutdownType = EToolShutdownType::Completed;
			}
			else
			{
				OutError = TEXT("cannot_accept_active_tool");
				return false;
			}
		}
		else if (ShutdownType == EToolShutdownType::Cancel)
		{
			WaitForToolShutdownReady(Context, ShutdownType, 8.0);
			if (ToolsContext->CanCancelActiveTool())
			{
				ResolvedShutdownType = EToolShutdownType::Cancel;
			}
			else if (ToolsContext->CanCompleteActiveTool())
			{
				ResolvedShutdownType = EToolShutdownType::Completed;
			}
			else
			{
				OutError = TEXT("cannot_cancel_active_tool");
				return false;
			}
		}

		ToolsContext->EndTool(ResolvedShutdownType);
		FlushToolsContext(Context, 3);
		return true;
	}

	TArray<AActor*> ResolveActorsByIds(UWorld* World, const TArray<TSharedPtr<FJsonValue>>& IdValues)
	{
		TArray<AActor*> Actors;
		for (const TSharedPtr<FJsonValue>& IdValue : IdValues)
		{
			const FString NameOrLabel = IdValue.IsValid() ? IdValue->AsString() : FString();
			if (NameOrLabel.IsEmpty())
			{
				continue;
			}
			if (AActor* Actor = FindActorByNameOrLabelLoose(World, NameOrLabel))
			{
				Actors.AddUnique(Actor);
			}
		}
		return Actors;
	}

	void SelectActors(const TArray<AActor*>& Actors)
	{
		if (!GEditor)
		{
			return;
		}

		USelection* SelectedActors = GEditor->GetSelectedActors();
		USelection* SelectedComponents = GEditor->GetSelectedComponents();
		if (!SelectedActors || !SelectedComponents)
		{
			return;
		}

		SelectedActors->BeginBatchSelectOperation();
		SelectedComponents->BeginBatchSelectOperation();
		SelectedActors->DeselectAll(AActor::StaticClass());
		SelectedComponents->DeselectAll(UActorComponent::StaticClass());
		for (AActor* Actor : Actors)
		{
			if (IsValid(Actor))
			{
				GEditor->SelectActor(Actor, true, false, true);
			}
		}
		SelectedComponents->EndBatchSelectOperation(false);
		SelectedActors->EndBatchSelectOperation(false);
		GEditor->NoteSelectionChange();
	}

	void SyncGeometryTargets(const TArray<AActor*>& Actors, const FModelingContext& Context)
	{
		UGeometrySelectionManager* SelectionManager = Context.Mode ? Context.Mode->GetSelectionManager() : nullptr;
		if (!SelectionManager)
		{
			return;
		}

		TArray<FGeometryIdentifier> DesiredTargets;
		for (AActor* Actor : Actors)
		{
			if (!Actor)
			{
				continue;
			}

			TArray<UActorComponent*> Components;
			Actor->GetComponents(UPrimitiveComponent::StaticClass(), Components);
			for (UActorComponent* Component : Components)
			{
				UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component);
				if (!Primitive)
				{
					continue;
				}

				FGeometryIdentifier::EObjectType ObjectType = FGeometryIdentifier::EObjectType::Unknown;
				if (Primitive->IsA<UStaticMeshComponent>())
				{
					ObjectType = FGeometryIdentifier::EObjectType::StaticMeshComponent;
				}
				DesiredTargets.Add(FGeometryIdentifier::PrimitiveComponent(Primitive, ObjectType));
			}
		}

		bool bOpenedTransaction = false;
		if (Context.ToolsContext && DesiredTargets.Num() > 0)
		{
			Context.ToolsContext->GetTransactionAPI()->BeginUndoTransaction(NSLOCTEXT("UeAgentInterface", "SyncModelingSelection", "Sync Modeling Selection"));
			bOpenedTransaction = true;
		}

		SelectionManager->SynchronizeActiveTargets(DesiredTargets, [SelectionManager]()
		{
			SelectionManager->ClearSelection();
		});

		if (bOpenedTransaction && Context.ToolsContext)
		{
			Context.ToolsContext->GetTransactionAPI()->EndUndoTransaction();
		}
	}

	bool ApplyToolProperties(const TSharedPtr<FJsonObject>& Params, UInteractiveTool* Tool, FString& OutError, TArray<TSharedPtr<FJsonValue>>* OutPropertyResults)
	{
		const TArray<TSharedPtr<FJsonValue>>* PropertiesArray = nullptr;
		if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("tool_properties"), PropertiesArray) || !PropertiesArray)
		{
			return true;
		}

		for (const TSharedPtr<FJsonValue>& EntryValue : *PropertiesArray)
		{
			const TSharedPtr<FJsonObject> Entry = EntryValue.IsValid() ? EntryValue->AsObject() : nullptr;
			if (!Entry.IsValid())
			{
				continue;
			}

			FString Selector;
			Entry->TryGetStringField(TEXT("property_set"), Selector);

			FString PropertyName;
			if (!Entry->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
			{
				if (OutPropertyResults)
				{
					TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
					ResultObj->SetStringField(TEXT("property_set"), Selector);
					ResultObj->SetStringField(TEXT("property_name"), TEXT(""));
					ResultObj->SetStringField(TEXT("property_import_status"), TEXT("missing_property_name"));
					ResultObj->SetBoolField(TEXT("property_import_verified"), false);
					OutPropertyResults->Add(MakeShared<FJsonValueObject>(ResultObj));
				}
				OutError = TEXT("tool_property_name_missing");
				return false;
			}

			FString ValueText;
			if (!Entry->TryGetStringField(TEXT("value_text"), ValueText))
			{
				if (OutPropertyResults)
				{
					TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
					ResultObj->SetStringField(TEXT("property_set"), Selector);
					ResultObj->SetStringField(TEXT("property_name"), PropertyName);
					ResultObj->SetStringField(TEXT("property_import_status"), TEXT("missing_value_text"));
					ResultObj->SetBoolField(TEXT("property_import_verified"), false);
					OutPropertyResults->Add(MakeShared<FJsonValueObject>(ResultObj));
				}
				OutError = TEXT("tool_property_value_missing");
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
				if (OutPropertyResults)
				{
					TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
					ResultObj->SetStringField(TEXT("property_set"), Selector);
					ResultObj->SetStringField(TEXT("property_name"), PropertyName);
					ResultObj->SetStringField(TEXT("requested_value_text"), ValueText);
					ResultObj->SetStringField(TEXT("applied_value_text"), TEXT(""));
					ResultObj->SetBoolField(TEXT("property_value_read_back"), false);
					ResultObj->SetBoolField(TEXT("value_text_exact_match"), false);
					ResultObj->SetBoolField(TEXT("value_text_changed_after_import"), false);
					ResultObj->SetStringField(TEXT("property_import_status"), ImportStatus);
					ResultObj->SetBoolField(TEXT("property_import_verified"), false);
					ResultObj->SetStringField(TEXT("property_import_error"), OutError);
					ResultObj->SetStringField(TEXT("cpp_type"), CppType);
					OutPropertyResults->Add(MakeShared<FJsonValueObject>(ResultObj));
				}
				return false;
			}
			if (OutPropertyResults)
			{
				const bool bExactMatch = AppliedValueText.Equals(ValueText, ESearchCase::CaseSensitive);
				TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
				ResultObj->SetStringField(TEXT("property_set"), Selector);
				ResultObj->SetStringField(TEXT("property_name"), PropertyName);
				ResultObj->SetStringField(TEXT("requested_value_text"), ValueText);
				ResultObj->SetStringField(TEXT("applied_value_text"), AppliedValueText);
				ResultObj->SetStringField(TEXT("property_import_status"), TEXT("imported_and_read_back"));
				ResultObj->SetStringField(TEXT("property_import_error"), TEXT(""));
				ResultObj->SetBoolField(TEXT("property_value_read_back"), true);
				ResultObj->SetBoolField(TEXT("property_import_verified"), true);
				ResultObj->SetBoolField(TEXT("value_text_exact_match"), bExactMatch);
				ResultObj->SetBoolField(TEXT("value_text_changed_after_import"), !bExactMatch);
				ResultObj->SetStringField(TEXT("cpp_type"), CppType);
				OutPropertyResults->Add(MakeShared<FJsonValueObject>(ResultObj));
			}

			NotifyToolPropertySetUpdated(Tool, PropertySet);

			FUeAgentInterfaceLogger::Log(TEXT("ModelingToolProperty req=%s tool=%s set=%s property=%s requested=%s applied=%s cpp_type=%s exact=%s"),
				TEXT("tool_properties"),
				*Tool->GetName(),
				*PropertySet->GetName(),
				*PropertyName,
				*ValueText,
				*AppliedValueText,
				*CppType,
				AppliedValueText.Equals(ValueText, ESearchCase::CaseSensitive) ? TEXT("true") : TEXT("false"));
		}

		return true;
	}

	bool InvokeActionByName(UObject* Target, const FString& ActionName)
	{
		if (!Target || ActionName.IsEmpty())
		{
			return false;
		}

		UFunction* Function = Target->FindFunction(FName(*ActionName));
		if (!Function || Function->ParmsSize != 0)
		{
			return false;
		}

		Target->ProcessEvent(Function, nullptr);
		return true;
	}

	bool InvokeToolAction(UInteractiveTool* Tool, const FString& ActionName)
	{
		if (!Tool)
		{
			return false;
		}

		if (InvokeActionByName(Tool, ActionName))
		{
			return true;
		}

		for (UObject* PropertySet : Tool->GetToolProperties(false))
		{
			if (InvokeActionByName(PropertySet, ActionName))
			{
				return true;
			}
		}
		return false;
	}

	bool ResolveSelectionActorsFromContext(const FUeAgentRequestContext& Ctx, UWorld* World, TArray<AActor*>& OutActors, FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* ActorIds = nullptr;
		if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("actor_ids"), ActorIds) && ActorIds)
		{
			OutActors = ResolveActorsByIds(World, *ActorIds);
		}
		else if (GEditor)
		{
			for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
			{
				if (AActor* Actor = Cast<AActor>(*It))
				{
					OutActors.Add(Actor);
				}
			}
		}

		if (OutActors.Num() == 0)
		{
			OutError = TEXT("no_target_actors");
			return false;
		}
		return true;
	}

	bool SaveActorStaticMeshes(const TArray<AActor*>& Actors, int32& OutSavedCount)
	{
		OutSavedCount = 0;
		TArray<UPackage*> PackagesToSave;

		for (AActor* Actor : Actors)
		{
			if (!Actor)
			{
				continue;
			}

			TArray<UActorComponent*> Components;
			Actor->GetComponents(UStaticMeshComponent::StaticClass(), Components);
			for (UActorComponent* Component : Components)
			{
				UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(Component);
				if (!StaticMeshComponent || !StaticMeshComponent->GetStaticMesh())
				{
					continue;
				}

				UPackage* Package = StaticMeshComponent->GetStaticMesh()->GetOutermost();
				if (Package)
				{
					PackagesToSave.AddUnique(Package);
				}
			}
		}

		if (PackagesToSave.Num() == 0)
		{
			return false;
		}

		if (!UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false))
		{
			return false;
		}

		OutSavedCount = PackagesToSave.Num();
		return true;
	}
}

