// Copyright Epic Games, Inc. All Rights Reserved.

#include "DomainAdapters/ModelingGeometry/UeAgentModelingMechanics.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/EditorResources/UeAgentEditorResourcePersistence.h"

#include "Core/Contract/UeAgentMechanicsRequestContext.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
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
	UE::UAI::Contract::FDiagnostic ModelingFact(const FString& Code, const FString& Reason,
		const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& Target)
	{
		auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Field, Expected, Actual, TEXT("modeling"), Reason);
		Error.TargetPath = Target;
		Error.SourceKind = TEXT("native_result");
		Error.Source.JsonPath.Reset(); Error.SemanticPointer.Reset();
		return Error;
	}

	UE::UAI::Contract::FDiagnostic ModelingCondition(const FString& Code, const FString& Reason,
		const FString& Field, bool Actual, bool Expected, const FString& Target)
	{
		return ModelingFact(Code, Reason, Field, MakeShared<FJsonValueBoolean>(Actual), MakeShared<FJsonValueBoolean>(Expected), Target);
	}

	UE::UAI::Contract::FDiagnostic ModelingInputFailure(const FString& Code, const FString& Reason,
		const TSharedPtr<FJsonObject>& Params, const FString& Field, TSharedPtr<FJsonValue> Expected)
	{
		const FString Pointer = TEXT("/") + Field.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
		TSharedPtr<FJsonValue> Actual = Params.IsValid() ? Params->Values.FindRef(Field) : nullptr;
		if (Actual.IsValid() && Actual->Type == EJson::Number && !FMath::IsFinite(Actual->AsNumber()))
			Actual = MakeShared<FJsonValueString>(FString::Printf(TEXT("%.17g"), Actual->AsNumber()));
		if (Actual.IsValid() && (Actual->Type == EJson::Array || Actual->Type == EJson::Object))
		{
			const auto Summary = MakeShared<FJsonObject>();
			Summary->SetStringField(TEXT("kind"), Actual->Type == EJson::Array ? TEXT("array") : TEXT("object"));
			Summary->SetNumberField(TEXT("count"), Actual->Type == EJson::Array ? Actual->AsArray().Num() : Actual->AsObject()->Values.Num());
			Actual = MakeShared<FJsonValueObject>(Summary);
		}
		auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Pointer, Expected,
			Actual, TEXT("modeling_input"), Reason);
		Error.SourceKind = TEXT("command_input");
		return Error;
	}

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

	bool TryGetVectorField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, FVector& OutValue, UE::UAI::Contract::FDiagnostic& OutError)
	{
		const auto* Value = UeAgentJsonDiagnostics::FindFieldValue(Obj, FieldName);
		if (!UeAgentJsonDiagnostics::TryReadVectorValue(Value ? *Value : nullptr, OutValue, OutError))
		{
			UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, FString(TEXT("/")) + FieldName);
			return false;
		}
		return true;
	}

	bool TryGetRotatorField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, FRotator& OutValue, UE::UAI::Contract::FDiagnostic& OutError)
	{
		const auto* Value = UeAgentJsonDiagnostics::FindFieldValue(Obj, FieldName);
		if (!UeAgentJsonDiagnostics::TryReadRotatorValue(Value ? *Value : nullptr, OutValue, OutError))
		{
			UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, FString(TEXT("/")) + FieldName);
			return false;
		}
		return true;
	}

	FPrimitiveBoundsPlacement::FPrimitiveBoundsPlacement()
		: bEnabled(false)
		, Center(FVector::ZeroVector)
		, Extent(FVector::ZeroVector)
		, Rotation(FRotator::ZeroRotator)
	{
	}

	bool ParsePrimitiveBoundsPlacement(const TSharedPtr<FJsonObject>& Params, FPrimitiveBoundsPlacement& OutPlacement, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutPlacement = FPrimitiveBoundsPlacement{};
		if (!Params.IsValid()) return true;
		const TSharedPtr<FJsonObject>* BoundsObject = nullptr;
		const bool bHasBounds = Params->HasField(TEXT("bounds"));
		if (bHasBounds && (!Params->TryGetObjectField(TEXT("bounds"), BoundsObject) || !BoundsObject || !BoundsObject->IsValid()))
		{
			OutError = ModelingInputFailure(TEXT("primitive_bounds_invalid"), TEXT("Primitive bounds must be an object."), Params, TEXT("bounds"), MakeShared<FJsonValueString>(TEXT("object")));
			return false;
		}
		FVector Center = FVector::ZeroVector, Extent = FVector::ZeroVector;
		bool bSelected = false;
		FString ExtentPointer;
		const auto Pair = [&](const TSharedPtr<FJsonObject>& Object, const TCHAR* First, const TCHAR* Second, const FString& Prefix, const bool bMinMax)
		{
			if (bSelected || !Object.IsValid() || (!Object->HasField(First) && !Object->HasField(Second))) return true;
			bSelected = true;
			FVector A, B;
			if (!TryGetVectorField(Object, First, A, OutError) || !TryGetVectorField(Object, Second, B, OutError))
			{
				UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, Prefix);
				return false;
			}
			Center = bMinMax ? (A + B) * 0.5 : A;
			Extent = bMinMax ? (B - A) * 0.5 : B;
			ExtentPointer = Prefix + TEXT("/") + Second;
			return true;
		};
		if (!Pair(bHasBounds ? *BoundsObject : nullptr, TEXT("center"), TEXT("extent"), TEXT("/bounds"), false)
			|| !Pair(Params, TEXT("bounds_center"), TEXT("bounds_extent"), FString(), false)
			|| !Pair(bHasBounds ? *BoundsObject : nullptr, TEXT("min"), TEXT("max"), TEXT("/bounds"), true)
			|| !Pair(Params, TEXT("bounds_min"), TEXT("bounds_max"), FString(), true)) return false;
		if (!bSelected)
		{
			if (!bHasBounds) return true;
			OutError = ModelingInputFailure(TEXT("primitive_bounds_invalid"), TEXT("Bounds contain no center/extent or min/max fields."), Params, TEXT("bounds"), MakeShared<FJsonValueString>(TEXT("center and extent, or min and max")));
			return false;
		}
		if (Extent.X <= 0.0 || Extent.Y <= 0.0 || Extent.Z < 0.0)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("primitive_bounds_extent_invalid"), TEXT("Primitive bounds require positive X and Y extents and a non-negative Z extent."), TEXT("input_decode"));
			OutError.PropertyPath = TEXT("/derived_extent");
			OutError.ActualValue = MakeShared<FJsonValueObject>(MakeVectorJson(Extent));
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("X > 0, Y > 0, Z >= 0"));
			OutError.Source.JsonPath = ExtentPointer;
			OutError.SourceKind = TEXT("related_input");
			return false;
		}
		FRotator Rotation = FRotator::ZeroRotator;
		const TCHAR* RotationField = Params->HasField(TEXT("rotation")) ? TEXT("rotation") : TEXT("orientation");
		if (Params->HasField(RotationField) && !TryGetRotatorField(Params, RotationField, Rotation, OutError)) return false;
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
		Obj->SetStringField(TEXT("kind"), ValueKind);
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
		Obj->SetStringField(TEXT("kind"), TEXT("enum"));
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
		UE::UAI::Contract::FDiagnostic& OutError)
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
			OutError = ModelingInputFailure(TEXT("primitive_bounds_require_accept"), TEXT("Bounds placement requires the primitive creation tool to be accepted."), InParams, TEXT("accept"), MakeShared<FJsonValueBoolean>(true));
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

	bool ApplyPrimitivePlacementToActor(AActor* Actor, const FPrimitiveBoundsPlacement& Placement, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!Actor)
		{
			OutError = ModelingCondition(TEXT("created_actor_resolution_failed"), TEXT("The accepted modeling output could not be resolved to a created actor."), TEXT("/created_actor_present"), false, true);
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
	static constexpr double DefaultModelingPrimitiveCollisionMaxStepHeight = 20.0;

	struct FModelingPrimitiveCollisionOptions
	{
		bool bGenerate = true;
		bool bReplaceExisting = true;
		FString Mode = TEXT("simple_auto");
		FString ProfileName = TEXT("BlockAll");
		FString EnabledText = TEXT("query_and_physics");
		ECollisionEnabled::Type CollisionEnabled = ECollisionEnabled::QueryAndPhysics;
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

	bool ParseModelingPrimitiveCollisionOptions(
		const TSharedPtr<FJsonObject>& Params,
		FModelingPrimitiveCollisionOptions& OutOptions,
		UE::UAI::Contract::FDiagnostic& OutError)
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
		UE::UAI::Contract::FDiagnostic& OutError)
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
			OutError = ModelingFact(TEXT("unsupported_modeling_primitive_collision_mode"), TEXT("The primitive collision mode is unsupported."), TEXT("/collision_mode"), MakeShared<FJsonValueString>(RequestedMode), MakeShared<FJsonValueString>(TEXT("supported primitive collision mode")));
			return false;
		}

		if (PrimitiveKind == TEXT("modeling_create_stairs") || RequestedMode == TEXT("stairs_step_boxes"))
		{
			double StepWidth = 0.0;
			double StepHeight = 0.0;
			double StepDepth = 0.0;
			double NumStepsNumber = 0.0;
			const auto ReadSetting = [&](const TCHAR* Field, double& Value)
			{
				if (TryGetDerivedPrimitiveNumber(DerivedPrimitiveData, Field, Value)) return true;
				OutError = ModelingFact(TEXT("invalid_stairs_shape_settings"), TEXT("The derived stairs collision settings require this numeric field."),
					TEXT("/derived_primitive/") + FString(Field), DerivedPrimitiveData.IsValid() ? DerivedPrimitiveData->Values.FindRef(Field) : nullptr, MakeShared<FJsonValueString>(TEXT("number")));
				return false;
			};
			if (!ReadSetting(TEXT("StepWidth"), StepWidth) || !ReadSetting(TEXT("StepHeight"), StepHeight)
				|| !ReadSetting(TEXT("StepDepth"), StepDepth) || !ReadSetting(TEXT("NumSteps"), NumStepsNumber)) return false;

			const int32 NumSteps = FMath::RoundToInt(NumStepsNumber);
			if (NumSteps < 2 || StepWidth <= 0.0 || StepHeight <= 0.0 || StepDepth <= 0.0)
			{
				const FString Field = NumSteps < 2 ? TEXT("NumSteps") : StepWidth <= 0.0 ? TEXT("StepWidth") : StepHeight <= 0.0 ? TEXT("StepHeight") : TEXT("StepDepth");
				const double Value = NumSteps < 2 ? NumStepsNumber : StepWidth <= 0.0 ? StepWidth : StepHeight <= 0.0 ? StepHeight : StepDepth;
				OutError = ModelingFact(TEXT("invalid_stairs_shape_settings"), TEXT("A derived stairs collision dimension or step count is outside its supported range."), TEXT("/derived_primitive/") + Field,
					MakeShared<FJsonValueNumber>(Value), MakeShared<FJsonValueString>(NumSteps < 2 ? TEXT("rounded step count at least 2") : TEXT("positive dimension")));
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!Actor)
		{
			OutError = ModelingCondition(TEXT("created_actor_resolution_failed"), TEXT("The accepted modeling output could not be resolved to a created actor."), TEXT("/created_actor_present"), false, true);
			return false;
		}
		USceneComponent* Root = Actor->GetRootComponent();
		if (!Root)
		{
			OutError = ModelingCondition(TEXT("modeling_primitive_actor_root_missing"), TEXT("The created actor has no root component for collision attachment."), TEXT("/root_component_present"), false, true, Actor->GetPathName());
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
				OutError = ModelingCondition(TEXT("modeling_primitive_collision_component_create_failed"), FString::Printf(TEXT("Native %s collision component creation failed."), *Spec.Shape), FString::Printf(TEXT("/collision_components/%d/created"), Index), false, true, Actor->GetPathName());
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!Actor)
		{
			OutError = ModelingCondition(TEXT("created_actor_resolution_failed"), TEXT("The accepted modeling output could not be resolved to a created actor."), TEXT("/created_actor_present"), false, true);
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
			OutError = ModelingFact(TEXT("dynamic_mesh_component_not_found"), TEXT("No DynamicMesh component could be configured for complex collision."), TEXT("/configured_dynamic_mesh_component_count"), MakeShared<FJsonValueNumber>(OutComponents.Num()), MakeShared<FJsonValueString>(TEXT("at least one")), Actor->GetPathName());
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		FModelingPrimitiveCollisionOptions Options;
		if (!ParseModelingPrimitiveCollisionOptions(Params, Options, OutError))
		{
			return false;
		}

		TSharedPtr<FJsonObject> CollisionReadback = MakeShared<FJsonObject>();
		CollisionReadback->SetStringField(TEXT("requested_mode"), Options.Mode);
		CollisionReadback->SetStringField(TEXT("profile_name"), Options.ProfileName);
		CollisionReadback->SetStringField(TEXT("enabled"), Options.EnabledText);
		CollisionReadback->SetBoolField(TEXT("replace_existing"), Options.bReplaceExisting);
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

	bool ResolveViewportScreenPoint(const FEditorViewportClient* ViewportClient, const TSharedPtr<FJsonObject>& Params, FVector2D& OutScreenPoint, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!ViewportClient || !ViewportClient->Viewport)
		{
			OutError = ModelingCondition(TEXT("editor_viewport_not_found"), ViewportClient ? TEXT("The editor viewport client has no viewport.") : TEXT("No editor viewport client is available."), ViewportClient ? TEXT("/viewport_present") : TEXT("/viewport_client_present"), false, true);
			return false;
		}

		double ScreenX = 0.0;
		double ScreenY = 0.0;
		if (!Params.IsValid() || !Params->TryGetNumberField(TEXT("screen_x"), ScreenX))
		{
			OutError = ModelingInputFailure(TEXT("missing_screen_point"), TEXT("The screen point requires a numeric screen_x coordinate."), Params, TEXT("screen_x"), MakeShared<FJsonValueString>(TEXT("number")));
			return false;
		}
		if (!Params->TryGetNumberField(TEXT("screen_y"), ScreenY))
		{
			OutError = ModelingInputFailure(TEXT("missing_screen_point"), TEXT("The screen point requires a numeric screen_y coordinate."), Params, TEXT("screen_y"), MakeShared<FJsonValueString>(TEXT("number")));
			return false;
		}

		const FIntPoint Size = ViewportClient->Viewport->GetSizeXY();
		if (Size.X <= 0 || Size.Y <= 0)
		{
			OutError = ModelingFact(TEXT("invalid_viewport_size"), TEXT("Screen selection requires positive viewport width and height."), TEXT("/viewport_size"), MakeShared<FJsonValueString>(FString::Printf(TEXT("%dx%d"), Size.X, Size.Y)), MakeShared<FJsonValueString>(TEXT("positive width and height")));
			return false;
		}

		OutScreenPoint.X = (float)FMath::Clamp(ScreenX, 0.0, (double)Size.X);
		OutScreenPoint.Y = (float)FMath::Clamp(ScreenY, 0.0, (double)Size.Y);
		return true;
	}

	bool DeprojectViewportScreenPoint(const FModelingContext& Context, const FVector2D& ScreenPoint, FRay3d& OutWorldRay, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!Context.ViewportClient || !Context.ViewportClient->Viewport)
		{
			OutError = ModelingCondition(TEXT("editor_viewport_not_found"), Context.ViewportClient ? TEXT("The editor viewport client has no viewport.") : TEXT("No editor viewport client is available for deprojection."), Context.ViewportClient ? TEXT("/viewport_present") : TEXT("/viewport_client_present"), false, true);
			return false;
		}

		UWorld* World = Context.ViewportClient->GetWorld();
		if (!World)
		{
			OutError = ModelingCondition(TEXT("editor_viewport_world_not_found"), TEXT("The editor viewport has no world for screen-point deprojection."), TEXT("/viewport_world_present"), false, true);
			return false;
		}

		FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
			Context.ViewportClient->Viewport,
			World->Scene,
			Context.ViewportClient->EngineShowFlags).SetRealtimeUpdate(Context.ViewportClient->IsRealtime()));
		FSceneView* SceneView = Context.ViewportClient->CalcSceneView(&ViewFamily);
		if (!SceneView)
		{
			OutError = ModelingCondition(TEXT("calc_scene_view_failed"), TEXT("The editor viewport could not calculate a scene view for deprojection."), TEXT("/scene_view_present"), false, true, World->GetPathName());
			return false;
		}

		FVector WorldOrigin = FVector::ZeroVector;
		FVector WorldDirection = FVector::ForwardVector;
		SceneView->DeprojectFVector2D(ScreenPoint, WorldOrigin, WorldDirection);
		OutWorldRay = FRay3d(WorldOrigin, WorldDirection.GetSafeNormal());
		return true;
	}

	bool ResolveWorldRayFromParams(const TSharedPtr<FJsonObject>& Params, FRay3d& OutWorldRay, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!Params.IsValid())
		{
			OutError = ModelingInputFailure(TEXT("missing_params"), TEXT("World-ray selection requires a parameter object."), Params, FString(), MakeShared<FJsonValueString>(TEXT("object")));
			return false;
		}

		const TSharedPtr<FJsonValue>* OriginValue = UeAgentJsonDiagnostics::FindFieldValue(Params, TEXT("world_origin"));
		const TSharedPtr<FJsonValue>* DirectionValue = UeAgentJsonDiagnostics::FindFieldValue(Params, TEXT("world_direction"));
		if (!OriginValue)
		{
			OutError = ModelingInputFailure(TEXT("missing_world_origin"), TEXT("The world ray requires an origin vector."), Params, TEXT("world_origin"), MakeShared<FJsonValueString>(TEXT("vector")));
			return false;
		}
		if (!DirectionValue)
		{
			OutError = ModelingInputFailure(TEXT("missing_world_direction"), TEXT("The world ray requires a direction vector."), Params, TEXT("world_direction"), MakeShared<FJsonValueString>(TEXT("non-zero vector")));
			return false;
		}

		FVector Origin = FVector::ZeroVector;
		FVector Direction = FVector::ZeroVector;
		if (!UeAgentJsonDiagnostics::TryReadVectorValue(*OriginValue, Origin, OutError))
		{
			UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/world_origin"));
			OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("invalid_world_origin"), OutError, TEXT("input_decode"), TEXT("The world ray origin could not be decoded."));
			return false;
		}
		if (!UeAgentJsonDiagnostics::TryReadVectorValue(*DirectionValue, Direction, OutError))
		{
			UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/world_direction"));
			OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("invalid_world_direction"), OutError, TEXT("input_decode"), TEXT("The world ray direction could not be decoded."));
			return false;
		}

		if (Direction.IsNearlyZero())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("invalid_world_direction"), TEXT("The world ray direction must be non-zero."), TEXT("input_decode"));
			OutError.Source.JsonPath = TEXT("/world_direction");
			OutError.ActualValue = *DirectionValue;
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-zero vector"));
			OutError.SourceKind = TEXT("command_input");
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

	bool GetModelingContext(FModelingContext& OutContext, UE::UAI::Contract::FDiagnostic& OutError)
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
			OutError = ModelingCondition(TEXT("modeling_mode_unavailable"), TEXT("The modeling editor mode was unavailable after activation."), TEXT("/modeling_mode_available"), false, true);
			return false;
		}

		UEditorInteractiveToolsContext* ToolsContext = Mode->GetInteractiveToolsContext();
		if (!ToolsContext)
		{
			OutError = ModelingCondition(TEXT("modeling_tools_context_unavailable"), TEXT("The modeling mode has no interactive tools context."), TEXT("/tools_context_available"), false, true);
			return false;
		}

		FEditorViewportClient* ViewportClient = GetPreferredViewportClient();
		if (!ViewportClient)
		{
			OutError = ModelingCondition(TEXT("editor_viewport_not_found"), TEXT("No editor viewport client is available for the modeling tools context."), TEXT("/viewport_client_present"), false, true);
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

	bool StartToolByIdentifier(const FString& ToolIdentifier, const FModelingContext& Context, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!Context.ToolsContext)
		{
			OutError = ModelingCondition(TEXT("modeling_tools_context_unavailable"), TEXT("The modeling mode has no interactive tools context."), TEXT("/tools_context_available"), false, true);
			return false;
		}

		if (Context.ToolsContext->HasActiveTool())
		{
			OutError = ModelingFact(TEXT("active_tool_exists"), TEXT("Another modeling tool is already active."), TEXT("/active_tool"), MakeShared<FJsonValueString>(Context.ToolsContext->GetActiveToolName()), MakeShared<FJsonValueString>(TEXT("no active tool")));
			return false;
		}

		if (!Context.ToolsContext->CanStartTool(ToolIdentifier))
		{
			OutError = ModelingFact(TEXT("cannot_start_tool"), TEXT("The interactive tools context rejected starting the requested tool."), TEXT("/tool_identifier"), MakeShared<FJsonValueString>(ToolIdentifier), MakeShared<FJsonValueString>(TEXT("tool allowed to start in the current context")));
			return false;
		}

		bool bObservedToolEnd = false;
		FString EndedToolName;
		FString EndedShutdownType = TEXT("");
		bool bObservedUnexpectedShutdown = false;
		bool bUnexpectedShutdownDuringSetup = false;
		FString UnexpectedShutdownMessage;
		UE::UAI::Contract::FDiagnostic UnexpectedShutdownDiagnostic;
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
				[&bObservedUnexpectedShutdown, &bUnexpectedShutdownDuringSetup, &UnexpectedShutdownMessage, &UnexpectedShutdownDiagnostic](UInteractiveToolManager*, UInteractiveTool* FailedTool, const FText& Message, bool bWasDuringToolSetup)
				{
					bObservedUnexpectedShutdown = true;
					bUnexpectedShutdownDuringSetup = bWasDuringToolSetup;
					UnexpectedShutdownMessage = Message.ToString();
					UnexpectedShutdownDiagnostic = ModelingCondition(TEXT("modeling_tool_unexpected_shutdown"),
						UnexpectedShutdownMessage.IsEmpty() ? TEXT("The native modeling tool reported an unexpected shutdown.") : UnexpectedShutdownMessage,
						TEXT("/unexpected_shutdown"), true, false);
					UnexpectedShutdownDiagnostic.SubjectId = FailedTool ? FailedTool->GetClass()->GetPathName() : FString();
					UnexpectedShutdownDiagnostic.Stage = bWasDuringToolSetup ? TEXT("modeling_tool_setup") : TEXT("modeling_tool_run");
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
				auto Observed = MakeShared<FJsonObject>();
				Observed->SetStringField(TEXT("requested_tool"), ToolIdentifier);
				Observed->SetBoolField(TEXT("active_tool_present"), Context.ToolsContext->HasActiveTool());
				Observed->SetBoolField(TEXT("observed_tool_end"), bObservedToolEnd);
				if (bObservedToolEnd)
				{
					Observed->SetStringField(TEXT("ended_tool"), EndedToolName);
					Observed->SetStringField(TEXT("shutdown_type"), EndedShutdownType);
				}
				OutError = ModelingFact(TEXT("tool_start_failed"), TEXT("The requested modeling tool did not remain active after native startup and the existing readiness wait."), TEXT("/tool_start_state"), MakeShared<FJsonValueObject>(Observed), MakeShared<FJsonValueString>(TEXT("requested tool active")));
				OutError.Append(UnexpectedShutdownDiagnostic);
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

	bool ApplyInternalTextProperty(UObject* Object, const FString& PropertyPath, const FString& TextValue, UE::UAI::Contract::FDiagnostic& OutError, TSharedPtr<FJsonValue>* OutReadback = nullptr);

	bool IsCreateMeshObjectTypePropertySetSelector(const FString& Selector)
	{
		return Selector.IsEmpty()
			|| Selector.Equals(TEXT("CreateMeshObjectTypeProperties"), ESearchCase::IgnoreCase)
			|| Selector.Equals(TEXT("UCreateMeshObjectTypeProperties"), ESearchCase::IgnoreCase)
			|| Selector.Equals(TEXT("CreateMeshObjectType"), ESearchCase::IgnoreCase)
			|| Selector.Equals(TEXT("OutputTypeProperties"), ESearchCase::IgnoreCase);
	}

	bool HasExplicitOutputTypeOverride(const TSharedPtr<FJsonObject>& Params, int32& OutIndex)
	{
		OutIndex = INDEX_NONE;
		if (!Params.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* PropertiesArray = nullptr;
		if (!Params->TryGetArrayField(TEXT("tool_properties"), PropertiesArray) || !PropertiesArray)
		{
			return false;
		}

		for (int32 Index = 0; Index < PropertiesArray->Num(); ++Index)
		{
			const auto& EntryValue = (*PropertiesArray)[Index];
			const TSharedPtr<FJsonObject> Entry = EntryValue.IsValid() && EntryValue->Type == EJson::Object ? EntryValue->AsObject() : nullptr;
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
				OutIndex = Index;
				return true;
			}
		}
		return false;
	}

	bool AutoPrepareModelingOutputType(const TSharedPtr<FJsonObject>& Params, UInteractiveTool* Tool, EModelingOutputTypePolicy Policy, UE::UAI::Contract::FDiagnostic& OutError, TSharedPtr<FJsonObject>& OutData)
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
		int32 OverrideIndex = INDEX_NONE;
		if (HasExplicitOutputTypeOverride(Params, OverrideIndex))
		{
			if (bRequiredPolicy)
			{
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("modeling_required_output_type_override_forbidden"), FString::Printf(TEXT("/tool_properties/%d/property_path"), OverrideIndex), MakeShared<FJsonValueString>(TEXT("no explicit OutputType edit")), MakeShared<FJsonValueString>(TEXT("OutputType")), TEXT("modeling_input"), FString::Printf(TEXT("This tool command fixes output policy to %s; an explicit OutputType edit is forbidden."), *GetModelingOutputTypePolicyName(Policy)));
				OutError.SourceKind = TEXT("command_input");
				return false;
			}
			return true;
		}

		UObject* PropertySet = FindToolPropertySet(Tool, TEXT("CreateMeshObjectTypeProperties"));
		if (!PropertySet)
		{
			if (bRequiredPolicy)
			{
				OutError = ModelingFact(TEXT("modeling_required_output_type_property_set_missing"), TEXT("The active tool has no CreateMeshObjectTypeProperties set for the required output policy."), TEXT("/output_type_property_set"), MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(GetModelingOutputTypePolicyName(Policy)));
				OutError.SubjectId = Tool->GetClass()->GetPathName();
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

	bool ApplyInternalTextProperty(UObject* Object, const FString& PropertyPath, const FString& TextValue, UE::UAI::Contract::FDiagnostic& OutError, TSharedPtr<FJsonValue>* OutReadback)
	{
		FProperty* Property = Object && Object->GetClass() ? FindFProperty<FProperty>(Object->GetClass(), *PropertyPath) : nullptr;
		if (!Property)
		{
			OutError = ModelingFact(TEXT("property_not_found"), Object ? TEXT("The object class has no reflected property with this name.") : TEXT("There is no object for the internal property edit."), TEXT("/property_path"), MakeShared<FJsonValueString>(PropertyPath), MakeShared<FJsonValueString>(TEXT("existing reflected property")));
			OutError.SubjectId = Object ? Object->GetPathName() : FString();
			return false;
		}
		TSharedPtr<FJsonObject> Descriptor = MakeShared<FJsonObject>();
		Descriptor->SetStringField(TEXT("property_path"), PropertyPath);
		const FString ValueKind = UeAgentPropertyValueCodec::GetValueKind(Property);
		Descriptor->SetStringField(TEXT("kind"), ValueKind);
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
			OutError = ModelingFact(TEXT("internal_text_value_kind_unsupported"), TEXT("Internal text assignment does not support this reflected value kind."), PropertyPath, MakeShared<FJsonValueString>(ValueKind), MakeShared<FJsonValueString>(TEXT("enum, string or name")));
			OutError.SubjectId = Object->GetPathName();
			return false;
		}
		Descriptor->SetObjectField(TEXT("value"), Value);
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("schema"), TEXT("uai.uobject_properties.v2"));
		Payload->SetArrayField(TEXT("properties"), { MakeShared<FJsonValueObject>(Descriptor) });
		UeAgentUObjectProperties::FApplyOptions ApplyOptions;
		ApplyOptions.bAllowReadOnly = true;
		UeAgentUObjectProperties::FApplySummaryResult ApplyResult;
        if (!UeAgentUObjectProperties::ApplyObjectProperties(Object, Payload, ApplyOptions, ApplyResult))
		{
			OutError = ApplyResult.Diagnostic;
			return false;
		}
		if (OutReadback)
		{
			UeAgentPropertyValueCodec::FOptions CodecOptions;
			*OutReadback = UeAgentPropertyValueCodec::Encode(Property, Property->ContainerPtrToValuePtr<void>(Object), Object, CodecOptions);
		}
		return true;
	}

	bool AutoPreparePrimitiveCreationTool(const TSharedPtr<FJsonObject>& Params, UInteractiveTool* Tool, const FString& ToolIdentifier, UE::UAI::Contract::FDiagnostic& OutError)
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

	bool FinishTool(const FModelingContext& Context, EToolShutdownType ShutdownType, UE::UAI::Contract::FDiagnostic& OutError)
	{
		UEditorInteractiveToolsContext* ToolsContext = Context.ToolsContext;
		FEditorViewportClient* ViewportClient = Context.ViewportClient;
		if (!ToolsContext)
		{
			OutError = ModelingCondition(TEXT("modeling_tools_context_unavailable"), TEXT("The modeling mode has no interactive tools context."), TEXT("/tools_context_available"), false, true);
			return false;
		}

		if (!ToolsContext->HasActiveTool())
		{
			OutError = ModelingCondition(TEXT("no_active_tool"), TEXT("There is no active modeling tool to finish."), TEXT("/active_tool_present"), false, true);
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
				OutError = ModelingCondition(TEXT("cannot_accept_active_tool"), FString::Printf(TEXT("Active tool '%s' can neither accept nor complete after waiting for shutdown readiness."), *ToolsContext->GetActiveToolName()), TEXT("/can_accept_or_complete"), false, true);
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
				OutError = ModelingCondition(TEXT("cannot_cancel_active_tool"), FString::Printf(TEXT("Active tool '%s' can neither cancel nor complete after waiting for shutdown readiness."), *ToolsContext->GetActiveToolName()), TEXT("/can_cancel_or_complete"), false, true);
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

	static void SupplementToolPropertySource(UE::UAI::Contract::FDiagnostic& Error, int32 SourceIndex, int32 ProjectedIndex)
	{
		for (auto& Child : Error.Diagnostics) SupplementToolPropertySource(Child, SourceIndex, ProjectedIndex);
		if (Error.PublicCause)
		{
			auto Cause = MakeShared<UE::UAI::Contract::FDiagnostic>(*Error.PublicCause);
			SupplementToolPropertySource(*Cause, SourceIndex, ProjectedIndex); Error.PublicCause = MoveTemp(Cause);
		}
		if (Error.SourceKind == TEXT("native_result") || !Error.Source.SourceFile.IsEmpty()) return;
		FString Relative = !Error.Source.InternalJsonPath.IsEmpty() ? Error.Source.InternalJsonPath
			: !Error.SemanticPointer.IsEmpty() ? Error.SemanticPointer : Error.Source.JsonPath;
		if (Relative.StartsWith(TEXT("/tool_properties/")) || Relative.StartsWith(TEXT("/params/"))) return;
		const FString ProjectedPrefix = FString::Printf(TEXT("/properties/%d"), ProjectedIndex);
		if (Relative == ProjectedPrefix || Relative.StartsWith(ProjectedPrefix + TEXT("/"))) Relative.RightChopInline(ProjectedPrefix.Len());
		const FString Pointer = FString::Printf(TEXT("/tool_properties/%d"), SourceIndex)
			+ (Relative.StartsWith(TEXT("/")) && Relative != TEXT("/") ? Relative : FString());
		Error.Source.JsonPath = Pointer; Error.Source.InternalJsonPath = Pointer; Error.SemanticPointer = Pointer;
		Error.SourceKind = TEXT("command_input");
	}

	bool ApplyToolProperties(const TSharedPtr<FJsonObject>& Params, UInteractiveTool* Tool, UE::UAI::Contract::FDiagnostic& OutError, TArray<TSharedPtr<FJsonValue>>* OutPropertyResults)
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
			TArray<int32> SourceIndices;
		};
		TArray<FPropertyGroup> Groups;
		TMap<UObject*, int32> GroupByOwner;
		for (int32 EntryIndex = 0; EntryIndex < PropertiesArray->Num(); ++EntryIndex)
		{
			const TSharedPtr<FJsonValue>& EntryValue = (*PropertiesArray)[EntryIndex];
			const TSharedPtr<FJsonObject> Entry = EntryValue.IsValid() && EntryValue->Type == EJson::Object ? EntryValue->AsObject() : nullptr;
			if (!Entry.IsValid())
			{
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("invalid_tool_property_descriptor"), FString::Printf(TEXT("/tool_properties/%d"), EntryIndex), MakeShared<FJsonValueString>(TEXT("object")), EntryValue, TEXT("modeling_input"), TEXT("Each tool_properties entry must be an object."));
				OutError.SourceKind = TEXT("command_input");
				return false;
			}
			FString Selector;
			Entry->TryGetStringField(TEXT("property_set"), Selector);
			UObject* PropertySet = FindToolPropertySet(Tool, Selector);
			if (!PropertySet)
			{
				OutError = ModelingInputFailure(TEXT("tool_property_set_not_found"), Tool ? TEXT("The active tool exposes no matching property set.") : TEXT("There is no tool from which to resolve a property set."), Entry, TEXT("property_set"), MakeShared<FJsonValueString>(TEXT("property set exposed by the active tool")));
				OutError.Source.JsonPath = FString::Printf(TEXT("/tool_properties/%d/property_set"), EntryIndex);
				OutError.SemanticPointer = OutError.Source.JsonPath;
				OutError.SubjectId = Tool ? Tool->GetClass()->GetPathName() : FString();
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
				const int32 NewIndex = Groups.Add({ PropertySet, Selector, {}, {} });
				GroupByOwner.Add(PropertySet, NewIndex);
				ExistingGroup = GroupByOwner.Find(PropertySet);
			}
			Groups[*ExistingGroup].Descriptors.Add(MakeShared<FJsonValueObject>(Descriptor));
			Groups[*ExistingGroup].SourceIndices.Add(EntryIndex);
		}

		for (FPropertyGroup& Group : Groups)
		{
			TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
			Payload->SetStringField(TEXT("schema"), TEXT("uai.uobject_properties.v2"));
			Payload->SetArrayField(TEXT("properties"), Group.Descriptors);
			UeAgentUObjectProperties::FApplyOptions ApplyOptions;
			ApplyOptions.SupplementDiagnosticSource = [Indices = Group.SourceIndices](UE::UAI::Contract::FDiagnostic& Error, int32 Index)
			{
				if (Indices.IsValidIndex(Index)) SupplementToolPropertySource(Error, Indices[Index], Index);
			};
			UeAgentUObjectProperties::FApplySummaryResult ApplyResult;
            const bool bApplied = UeAgentUObjectProperties::ApplyObjectProperties(Group.Owner, Payload, ApplyOptions, ApplyResult);
            if (!bApplied) {
                const FString NativeOwner = Group.Owner->GetPathName();
                // This group's native property-set instance is implementation context.
                // Referenced asset/actor targets in lower causes remain their own targets.
                TFunction<void(UE::UAI::Contract::FDiagnostic&)> KeepOwnerContext = [&](auto& Error) {
                    if (Error.TargetPath == NativeOwner) {
                        if (Error.SubjectId.IsEmpty()) Error.SubjectId = NativeOwner;
                        Error.TargetPath.Reset();
                    }
                    for (auto& Child : Error.Diagnostics) KeepOwnerContext(Child);
                    if (Error.PublicCause) {
                        auto Cause = MakeShared<UE::UAI::Contract::FDiagnostic>(*Error.PublicCause);
                        KeepOwnerContext(*Cause); Error.PublicCause = MoveTemp(Cause);
                    }
                };
                KeepOwnerContext(ApplyResult.Diagnostic);
            }
            if (OutPropertyResults)
            {
                for (const FString& PropertyPath : ApplyResult.AppliedPropertyPaths)
                {
                    auto Result = MakeShared<FJsonObject>();
                    Result->SetStringField(TEXT("property_set"), Group.Selector);
                    Result->SetStringField(TEXT("property_path"), PropertyPath);
                    Result->SetStringField(TEXT("status"), TEXT("applied"));
                    OutPropertyResults->Add(MakeShared<FJsonValueObject>(Result));
                }
                if (!bApplied)
                {
                    auto Result = MakeShared<FJsonObject>();
                    Result->SetStringField(TEXT("property_set"), Group.Selector);
                    Result->SetStringField(TEXT("property_path"), ApplyResult.Diagnostic.PropertyPath);
                    Result->SetStringField(TEXT("status"), TEXT("failed"));
                    Result->SetObjectField(TEXT("diagnostic"), ApplyResult.Diagnostic.ToPublicJson());
                    OutPropertyResults->Add(MakeShared<FJsonValueObject>(Result));
                }
            }
            if (!bApplied)
            {
                OutError = MoveTemp(ApplyResult.Diagnostic);
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

	bool ResolveSelectionActorsFromContext(const FUeAgentRequestContext& Ctx, UWorld* World, TArray<AActor*>& OutActors, UE::UAI::Contract::FDiagnostic& OutError)
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
			OutError = ModelingFact(TEXT("no_target_actors"), TEXT("No actor references or editor selection resolved to a target actor."), TEXT("/resolved_actor_count"), MakeShared<FJsonValueNumber>(OutActors.Num()), MakeShared<FJsonValueString>(TEXT("at least one")));
			return false;
		}
		return true;
	}

	bool SaveActorStaticMeshes(const TArray<AActor*>& Actors, TArray<FString>& OutSavedPackagePaths, UE::UAI::Contract::FDiagnostic& OutError)
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
			OutError = ModelingFact(TEXT("no_static_mesh_packages_saved"), TEXT("The selected actors have no static mesh packages to save."), TEXT("/static_mesh_package_count"), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one")), Actors.Num() == 1 && Actors[0] ? Actors[0]->GetPathName() : FString());
			OutError.Stage = TEXT("save");
			return false;
		}

		if (!UE::UAI::EditorResources::SavePackages(PackagesToSave, false, OutError))
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

