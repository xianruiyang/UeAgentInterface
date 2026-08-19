// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_Modeling.h"
#include "UeAgentHttpServer_ModelingOps.h"

#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/UeAgentUObjectProperties.h"
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

	TSharedPtr<FJsonValueObject> MakeToolPropertyLiteralEntry(const FString& PropertySet, const FString& PropertyPath, const FString& ValueKind, const TSharedPtr<FJsonValue>& Literal)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("property_set"), PropertySet);
		Obj->SetStringField(TEXT("property_path"), PropertyPath);
		Obj->SetStringField(TEXT("value_kind"), ValueKind);
		TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
		Value->SetField(TEXT("literal"), Literal);
		Obj->SetObjectField(TEXT("value"), Value);
		return MakeShared<FJsonValueObject>(Obj);
	}

	TSharedPtr<FJsonValueObject> MakeToolPropertySelectionEntry(const FString& PropertySet, const FString& PropertyPath, const FString& Selected)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("property_set"), PropertySet);
		Obj->SetStringField(TEXT("property_path"), PropertyPath);
		Obj->SetStringField(TEXT("value_kind"), TEXT("enum"));
		TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
		Value->SetStringField(TEXT("selected"), Selected);
		Obj->SetObjectField(TEXT("value"), Value);
		return MakeShared<FJsonValueObject>(Obj);
	}

	TSharedPtr<FJsonObject> FindExplicitToolProperty(const TSharedPtr<FJsonObject>& Params, const FString& PropertySet, const FString& PropertyPath)
	{
		const TArray<TSharedPtr<FJsonValue>>* PropertiesArray = nullptr;
		if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("tool_properties"), PropertiesArray) || !PropertiesArray)
		{
			return nullptr;
		}

		for (const TSharedPtr<FJsonValue>& EntryValue : *PropertiesArray)
		{
			const TSharedPtr<FJsonObject> Entry = EntryValue.IsValid() ? EntryValue->AsObject() : nullptr;
			if (!Entry.IsValid()) continue;
			FString EntrySet;
			FString EntryPath;
			Entry->TryGetStringField(TEXT("property_set"), EntrySet);
			Entry->TryGetStringField(TEXT("property_path"), EntryPath);
			if (EntryPath.Equals(PropertyPath, ESearchCase::IgnoreCase)
				&& (PropertySet.IsEmpty() || EntrySet.Equals(PropertySet, ESearchCase::IgnoreCase)))
			{
				return Entry;
			}
		}
		return nullptr;
	}

	TSharedPtr<FJsonValue> FindExplicitToolPropertyLiteral(const TSharedPtr<FJsonObject>& Params, const FString& PropertySet, const FString& PropertyPath)
	{
		const TSharedPtr<FJsonObject> Entry = FindExplicitToolProperty(Params, PropertySet, PropertyPath);
		const TSharedPtr<FJsonObject>* Value = nullptr;
		if (Entry.IsValid() && Entry->TryGetObjectField(TEXT("value"), Value) && Value && Value->IsValid())
		{
			return (*Value)->TryGetField(TEXT("literal"));
		}
		return nullptr;
	}

	bool TryGetExplicitToolPropertyNumber(const TSharedPtr<FJsonObject>& Params, const FString& PropertySet, const FString& PropertyPath, double& OutValue)
	{
		const TSharedPtr<FJsonValue> Literal = FindExplicitToolPropertyLiteral(Params, PropertySet, PropertyPath);
		if (!Literal.IsValid() || Literal->Type != EJson::Number || !FMath::IsFinite(Literal->AsNumber())) return false;
		OutValue = Literal->AsNumber();
		return true;
	}

	bool TryGetExplicitToolPropertyInt(const TSharedPtr<FJsonObject>& Params, const FString& PropertySet, const FString& PropertyPath, int32& OutValue)
	{
		double Number = 0.0;
		if (!TryGetExplicitToolPropertyNumber(Params, PropertySet, PropertyPath, Number)
			|| FMath::TruncToDouble(Number) != Number || Number < MIN_int32 || Number > MAX_int32) return false;
		OutValue = static_cast<int32>(Number);
		return true;
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
		ComputedProperties.Add(MakeToolPropertySelectionEntry(TEXT("ShapeSettings"), TEXT("TargetSurface"), TEXT("AtOrigin")));
		ComputedProperties.Add(MakeToolPropertySelectionEntry(TEXT("ShapeSettings"), TEXT("PivotLocation"), TEXT("Centered")));

		const FVector FullSize = OutPlacement.Extent * 2.0f;
		TSharedPtr<FJsonObject> DerivedData = MakeShared<FJsonObject>();
		DerivedData->SetStringField(TEXT("tool_identifier"), ToolIdentifier);
		DerivedData->SetStringField(TEXT("primitive_kind"), WrapperCommand);
		DerivedData->SetObjectField(TEXT("bounds_center"), MakeVectorJson(OutPlacement.Center));
		DerivedData->SetObjectField(TEXT("bounds_extent"), MakeVectorJson(OutPlacement.Extent));
		DerivedData->SetObjectField(TEXT("rotation"), MakeRotatorJson(OutPlacement.Rotation));

		auto AddDerivedNumber = [&ComputedProperties, &DerivedData](const TCHAR* PropertyName, double Value)
		{
			ComputedProperties.Add(MakeToolPropertyLiteralEntry(TEXT("ShapeSettings"), PropertyName, TEXT("float"), MakeShared<FJsonValueNumber>(Value)));
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
			ComputedProperties.Add(MakeToolPropertyLiteralEntry(TEXT("ShapeSettings"), TEXT("NumSteps"), TEXT("int32"), MakeShared<FJsonValueNumber>(NumSteps)));
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

	const TMap<FString, FModelingToolWrapperDefinition>& GetModelingToolWrapperRegistry()
	{
		static const TMap<FString, FModelingToolWrapperDefinition> Registry = {
			{TEXT("modeling_convert_actor_to_dynamic_mesh"), {TEXT("BeginConvertMeshesTool"), TEXT(""), TEXT(""), true, false, EModelingOutputTypePolicy::DynamicMeshActorRequired}},
			{TEXT("modeling_duplicate_to_new_static_mesh"), {TEXT("BeginDuplicateMeshesTool"), TEXT(""), TEXT(""), true, false, EModelingOutputTypePolicy::StaticMeshRequired}},
			{TEXT("modeling_create_box"), {TEXT("BeginAddBoxPrimitiveTool"), TEXT(""), TEXT(""), true, true}},
			{TEXT("modeling_create_cylinder"), {TEXT("BeginAddCylinderPrimitiveTool"), TEXT(""), TEXT(""), true, true}},
			{TEXT("modeling_create_sphere"), {TEXT("BeginAddSpherePrimitiveTool"), TEXT(""), TEXT(""), true, true}},
			{TEXT("modeling_create_plane"), {TEXT("BeginAddRectanglePrimitiveTool"), TEXT(""), TEXT(""), true, true}},
			{TEXT("modeling_create_stairs"), {TEXT("BeginAddStairsPrimitiveTool"), TEXT(""), TEXT(""), true, true}},
			{TEXT("modeling_create_ramp"), {TEXT("BeginAddRampPrimitiveTool"), TEXT(""), TEXT(""), true, true}},
			{TEXT("modeling_create_ramp_corner"), {TEXT("BeginAddRampCornerPrimitiveTool"), TEXT(""), TEXT(""), true, true}},
			{TEXT("modeling_extrude_faces"), {TEXT("BeginSelectionExtrudeTool"), TEXT(""), TEXT(""), true, false}},
			{TEXT("modeling_inset_faces"), {TEXT("BeginPolyEditTool"), TEXT("Inset"), TEXT(""), true, false}},
			{TEXT("modeling_bevel_edges"), {TEXT("BeginPolyEditTool"), TEXT("Bevel"), TEXT(""), true, false}},
			{TEXT("modeling_offset"), {TEXT("BeginSelectionOffsetTool"), TEXT(""), TEXT(""), true, false}},
			{TEXT("modeling_push_pull"), {TEXT("BeginPolyEditTool"), TEXT("PushPull"), TEXT(""), true, false}},
			{TEXT("modeling_mirror"), {TEXT("BeginMirrorTool"), TEXT(""), TEXT(""), true, false}},
			{TEXT("modeling_duplicate_faces"), {TEXT("BeginPolyEditTool"), TEXT("Duplicate"), TEXT(""), true, false}},
			{TEXT("modeling_boolean"), {TEXT("BeginMeshBooleanTool"), TEXT(""), TEXT(""), true, false}},
			{TEXT("modeling_trim"), {TEXT("BeginMeshTrimTool"), TEXT(""), TEXT(""), true, false}},
			{TEXT("modeling_plane_cut"), {TEXT("BeginPlaneCutTool"), TEXT(""), TEXT(""), true, false}},
			{TEXT("modeling_mesh_cut"), {TEXT("BeginCutMeshWithMeshTool"), TEXT(""), TEXT(""), true, false}},
			{TEXT("modeling_voxel_boolean"), {TEXT("BeginVoxelBooleanTool"), TEXT(""), TEXT(""), true, false}},
			{TEXT("modeling_remesh"), {TEXT("BeginRemeshMeshTool"), TEXT(""), TEXT(""), true, false}},
			{TEXT("modeling_simplify"), {TEXT("BeginSimplifyMeshTool"), TEXT(""), TEXT(""), true, false}},
			{TEXT("modeling_subdivide"), {TEXT("BeginSubdividePolyTool"), TEXT(""), TEXT(""), true, false}},
			{TEXT("modeling_weld_edges"), {TEXT("BeginWeldEdgesTool"), TEXT(""), TEXT(""), true, false}},
			{TEXT("modeling_fill_holes"), {TEXT("BeginHoleFillTool"), TEXT(""), TEXT(""), true, false}},
			{TEXT("modeling_recompute_normals"), {TEXT("BeginEditNormalsTool"), TEXT(""), TEXT(""), true, false}},
			{TEXT("modeling_bake_transform"), {TEXT("BeginBakeTransformTool"), TEXT(""), TEXT(""), true, false}},
			{TEXT("modeling_align_to_world"), {TEXT("BeginAlignObjectsTool"), TEXT(""), TEXT(""), true, false}},
			{TEXT("modeling_auto_uv"), {TEXT("BeginGlobalUVGenerateTool"), TEXT(""), TEXT(""), true, false}},
			{TEXT("modeling_project_uv"), {TEXT("BeginUVProjectionTool"), TEXT(""), TEXT(""), true, false}},
			{TEXT("modeling_set_material_slot"), {TEXT("BeginEditMeshMaterialsTool"), TEXT("AssignActiveMaterial"), TEXT("modeling_set_material_slot_action_unsupported"), true, false}},
			{TEXT("modeling_add_material_slot"), {TEXT("BeginEditMeshMaterialsTool"), TEXT("AddMaterialSlot"), TEXT("modeling_add_material_slot_action_unsupported"), false, false}},
			{TEXT("modeling_remove_material_slot"), {TEXT("BeginEditMeshMaterialsTool"), TEXT("RemoveMaterialSlot"), TEXT("modeling_remove_material_slot_action_unsupported"), false, false}},
			{TEXT("modeling_generate_simple_collision"), {TEXT("BeginSimpleCollisionEditorTool"), TEXT(""), TEXT(""), true, false}},
			{TEXT("modeling_generate_convex_collision"), {TEXT("BeginSetCollisionGeometryTool"), TEXT(""), TEXT(""), true, false}}
		};
		return Registry;
	}

	bool ResolveAllowedModelingToolIdentifier(const FString& InValue, FString& OutToolIdentifier)
	{
		OutToolIdentifier = ResolveToolIdentifierAlias(InValue);
		if (OutToolIdentifier.IsEmpty())
		{
			return false;
		}

		for (const TPair<FString, FModelingToolWrapperDefinition>& Pair : GetModelingToolWrapperRegistry())
		{
			if (Pair.Value.ToolIdentifier.Equals(OutToolIdentifier, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}

		OutToolIdentifier.Reset();
		return false;
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

		UeAgentUObjectProperties::FExportOptions Options;
		Options.Profile = UeAgentUObjectProperties::EExportProfile::Authoring;
		TSharedPtr<FJsonObject> PropertySetObj = UeAgentUObjectProperties::ExportObjectProperties(PropertySet, Options);
		PropertySetObj->SetStringField(TEXT("name"), PropertySet->GetName());
		PropertySetObj->SetStringField(TEXT("class"), PropertySet->GetClass()->GetPathName());
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

		return FindExplicitToolProperty(Params, TEXT("ShapeSettings"), TEXT("TargetSurface")).IsValid();
	}

	FString GetModelingOutputTypeIdentifier(EModelingOutputTypePolicy Policy)
	{
		return Policy == EModelingOutputTypePolicy::StaticMeshRequired
			? UCreateMeshObjectTypeProperties::StaticMeshIdentifier
			: UCreateMeshObjectTypeProperties::DynamicMeshActorIdentifier;
	}

	FString GetModelingOutputTypePolicyName(EModelingOutputTypePolicy Policy)
	{
		switch (Policy)
		{
		case EModelingOutputTypePolicy::DynamicMeshActorRequired:
			return TEXT("dynamic_mesh_actor_required");
		case EModelingOutputTypePolicy::StaticMeshRequired:
			return TEXT("static_mesh_required");
		default:
			return TEXT("dynamic_mesh_actor_default");
		}
	}

	void ConfigureDefaultModelingOutputType(EModelingOutputTypePolicy Policy)
	{
		UCreateMeshObjectTypeProperties::bEnableDynamicMeshActorSupport = true;
		UCreateMeshObjectTypeProperties::DefaultObjectTypeIdentifier = GetModelingOutputTypeIdentifier(Policy);
	}

	bool ApplyInternalTextProperty(UObject* Object, const FString& PropertyPath, const FString& TextValue, FString& OutError, TSharedPtr<FJsonValue>* OutReadback = nullptr);

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
			Entry->TryGetStringField(TEXT("property_path"), EntryName);
			if (EntryName.Equals(TEXT("OutputType"), ESearchCase::IgnoreCase) && IsCreateMeshObjectTypePropertySetSelector(EntrySet))
			{
				return true;
			}
		}
		return false;
	}

	bool AutoPrepareModelingOutputType(const TSharedPtr<FJsonObject>& Params, UInteractiveTool* Tool, EModelingOutputTypePolicy Policy, FString& OutError, TSharedPtr<FJsonObject>& OutData)
	{
		if (!Tool)
		{
			return true;
		}

		const bool bRequiredPolicy = Policy != EModelingOutputTypePolicy::DynamicMeshActorDefault;
		if (OutData.IsValid())
		{
			OutData->SetStringField(TEXT("output_type_policy"), GetModelingOutputTypePolicyName(Policy));
		}
		if (HasExplicitOutputTypeOverride(Params))
		{
			if (bRequiredPolicy)
			{
				OutError = TEXT("modeling_required_output_type_override_forbidden");
				return false;
			}
			return true;
		}

		UObject* PropertySet = FindToolPropertySet(Tool, TEXT("CreateMeshObjectTypeProperties"));
		if (!PropertySet)
		{
			if (bRequiredPolicy)
			{
				OutError = TEXT("modeling_required_output_type_property_set_missing");
				return false;
			}
			if (OutData.IsValid())
			{
				OutData->SetBoolField(TEXT("default_output_type_applied"), false);
				OutData->SetStringField(TEXT("default_output_type_skip_reason"), TEXT("missing_create_mesh_output_type_property"));
			}
			return true;
		}

		TSharedPtr<FJsonValue> ReadbackValue;
		if (!ApplyInternalTextProperty(
			PropertySet,
			TEXT("OutputType"),
			GetModelingOutputTypeIdentifier(Policy),
			OutError,
			&ReadbackValue))
		{
			return false;
		}

		NotifyToolPropertySetUpdated(Tool, PropertySet);
		if (OutData.IsValid())
		{
			OutData->SetBoolField(TEXT("default_output_type_applied"), true);
			OutData->SetStringField(TEXT("default_output_type"), GetModelingOutputTypeIdentifier(Policy));
			OutData->SetField(TEXT("default_output_type_readback"), ReadbackValue);
		}
		return true;
	}

	FString ResolveAcceptedOutputAssetPath(AActor* Actor)
	{
		if (!Actor)
		{
			return FString();
		}

		TArray<UStaticMeshComponent*> Components;
		Actor->GetComponents<UStaticMeshComponent>(Components);
		for (UStaticMeshComponent* Component : Components)
		{
			if (Component && Component->GetStaticMesh())
			{
				return FPackageName::ObjectPathToPackageName(Component->GetStaticMesh()->GetPathName());
			}
		}
		return FString();
	}

	bool IsAcceptedDynamicMeshOutput(AActor* Actor)
	{
		return Actor && Actor->FindComponentByClass<UDynamicMeshComponent>() != nullptr;
	}

	bool ApplyInternalTextProperty(UObject* Object, const FString& PropertyPath, const FString& TextValue, FString& OutError, TSharedPtr<FJsonValue>* OutReadback)
	{
		FProperty* Property = Object && Object->GetClass() ? FindFProperty<FProperty>(Object->GetClass(), *PropertyPath) : nullptr;
		if (!Property)
		{
			OutError = TEXT("property_not_found");
			return false;
		}
		TSharedPtr<FJsonObject> Descriptor = MakeShared<FJsonObject>();
		Descriptor->SetStringField(TEXT("property_path"), PropertyPath);
		const FString ValueKind = UeAgentPropertyValueCodec::GetValueKind(Property);
		Descriptor->SetStringField(TEXT("value_kind"), ValueKind);
		UeAgentPropertyValueCodec::AddTypeMetadata(Property, Descriptor);
		TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
		if (ValueKind == TEXT("enum"))
		{
			Value->SetStringField(TEXT("selected"), TextValue);
		}
		else if (ValueKind == TEXT("string") || ValueKind == TEXT("name"))
		{
			Value->SetStringField(TEXT("literal"), TextValue);
		}
		else
		{
			OutError = TEXT("internal_text_value_kind_unsupported");
			return false;
		}
		Descriptor->SetObjectField(TEXT("value"), Value);
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("schema"), TEXT("uai.uobject_properties.v2"));
		Payload->SetArrayField(TEXT("properties"), { MakeShared<FJsonValueObject>(Descriptor) });
		UeAgentUObjectProperties::FApplyOptions ApplyOptions;
		ApplyOptions.bAllowReadOnly = true;
		const TSharedPtr<FJsonObject> Summary = UeAgentUObjectProperties::ApplyObjectProperties(Object, Payload, ApplyOptions);
		FString Status;
		Summary->TryGetStringField(TEXT("status"), Status);
		if (Status != TEXT("applied"))
		{
			Summary->TryGetStringField(TEXT("reason"), OutError);
			if (OutError.IsEmpty()) OutError = TEXT("internal_property_apply_failed");
			return false;
		}
		if (OutReadback)
		{
			UeAgentPropertyValueCodec::FOptions CodecOptions;
			*OutReadback = UeAgentPropertyValueCodec::Encode(Property, Property->ContainerPtrToValuePtr<void>(Object), Object, CodecOptions);
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

		if (!ApplyInternalTextProperty(PropertySet, TEXT("TargetSurface"), TEXT("AtOrigin"), OutError))
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

		struct FPropertyGroup
		{
			UObject* Owner = nullptr;
			FString Selector;
			TArray<TSharedPtr<FJsonValue>> Descriptors;
		};
		TArray<FPropertyGroup> Groups;
		TMap<UObject*, int32> GroupByOwner;
		for (const TSharedPtr<FJsonValue>& EntryValue : *PropertiesArray)
		{
			const TSharedPtr<FJsonObject> Entry = EntryValue.IsValid() ? EntryValue->AsObject() : nullptr;
			if (!Entry.IsValid())
			{
				OutError = TEXT("invalid_tool_property_descriptor");
				return false;
			}
			FString Selector;
			Entry->TryGetStringField(TEXT("property_set"), Selector);
			UObject* PropertySet = FindToolPropertySet(Tool, Selector);
			if (!PropertySet)
			{
				OutError = TEXT("tool_property_set_not_found");
				return false;
			}
			TSharedPtr<FJsonObject> Descriptor = MakeShared<FJsonObject>();
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Entry->Values)
			{
				if (Pair.Key != TEXT("property_set")) Descriptor->SetField(Pair.Key, Pair.Value);
			}
			int32* ExistingGroup = GroupByOwner.Find(PropertySet);
			if (!ExistingGroup)
			{
				const int32 NewIndex = Groups.Add({ PropertySet, Selector, {} });
				GroupByOwner.Add(PropertySet, NewIndex);
				ExistingGroup = GroupByOwner.Find(PropertySet);
			}
			Groups[*ExistingGroup].Descriptors.Add(MakeShared<FJsonValueObject>(Descriptor));
		}

		for (FPropertyGroup& Group : Groups)
		{
			TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
			Payload->SetStringField(TEXT("schema"), TEXT("uai.uobject_properties.v2"));
			Payload->SetArrayField(TEXT("properties"), Group.Descriptors);
			UeAgentUObjectProperties::FApplyOptions ApplyOptions;
			const TSharedPtr<FJsonObject> Summary = UeAgentUObjectProperties::ApplyObjectProperties(Group.Owner, Payload, ApplyOptions);
			FString Status;
			Summary->TryGetStringField(TEXT("status"), Status);
			if (OutPropertyResults)
			{
				const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
				if (Summary->TryGetArrayField(TEXT("properties"), Results) && Results)
				{
					for (const TSharedPtr<FJsonValue>& ResultValue : *Results)
					{
						TSharedPtr<FJsonObject> Result = ResultValue.IsValid() ? ResultValue->AsObject() : nullptr;
						if (Result.IsValid()) Result->SetStringField(TEXT("property_set"), Group.Selector);
						OutPropertyResults->Add(ResultValue);
					}
				}
			}
			if (Status != TEXT("applied"))
			{
				Summary->TryGetStringField(TEXT("reason"), OutError);
				if (OutError.IsEmpty()) OutError = TEXT("tool_property_apply_failed");
				return false;
			}
			NotifyToolPropertySetUpdated(Tool, Group.Owner);
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

	bool SaveActorStaticMeshes(const TArray<AActor*>& Actors, TArray<FString>& OutSavedPackagePaths)
	{
		OutSavedPackagePaths.Reset();
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

		for (const UPackage* Package : PackagesToSave)
		{
			if (Package)
			{
				OutSavedPackagePaths.Add(Package->GetName());
			}
		}
		OutSavedPackagePaths.Sort();
		return true;
	}
}

