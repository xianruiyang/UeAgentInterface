// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentLevelContentJsonProperties.h"
#include "UeAgentLevelContentJsonCore.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/UeAgentUObjectProperties.h"

#include "Components/ActorComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

namespace UeAgentLevelContentJson
{
	FProperty* FindPropertyByNameLoose(UStruct* StructType, const FString& Segment)
	{
		if (!StructType || Segment.TrimStartAndEnd().IsEmpty())
		{
			return nullptr;
		}
		const FString Normalized = Segment.TrimStartAndEnd();
		const FString WithoutB = Normalized.StartsWith(TEXT("b")) ? Normalized.Mid(1) : Normalized;
		for (TFieldIterator<FProperty> It(StructType, EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			const FString PropertyName = Property->GetName();
			const FString PropertyWithoutB = PropertyName.StartsWith(TEXT("b")) ? PropertyName.Mid(1) : PropertyName;
			if (PropertyName.Equals(Normalized, ESearchCase::IgnoreCase) ||
				PropertyWithoutB.Equals(WithoutB, ESearchCase::IgnoreCase) ||
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
		if (!Obj || PropertyPath.TrimStartAndEnd().IsEmpty())
		{
			return false;
		}

		TArray<FString> Segments;
		PropertyPath.ParseIntoArray(Segments, TEXT("."), true);
		if (Segments.Num() <= 0)
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
			if (StructProperty)
			{
				CurrentStruct = StructProperty->Struct;
				CurrentContainer = ValuePtr;
				continue;
			}
			FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property);
			if (ObjectProperty)
			{
				UObject* ObjectValue = ObjectProperty->GetObjectPropertyValue(ValuePtr);
				if (!ObjectValue)
				{
					return false;
				}
				CurrentStruct = ObjectValue->GetClass();
				CurrentContainer = ObjectValue;
				continue;
			}
			return false;
		}
		return false;
	}

	bool ResolveObjectPropertyPath(UObject* Obj, const FString& PropertyPath, UObject*& OutTargetObject, FString& OutResolvedPropertyPath, FProperty*& OutProperty, void*& OutValuePtr)
	{
		OutTargetObject = Obj;
		OutResolvedPropertyPath = PropertyPath;
		OutProperty = nullptr;
		OutValuePtr = nullptr;
		if (!Obj || PropertyPath.TrimStartAndEnd().IsEmpty())
		{
			return false;
		}

		if (AActor* Actor = Cast<AActor>(Obj))
		{
			TArray<FString> Segments;
			PropertyPath.ParseIntoArray(Segments, TEXT("."), true);
			if (Segments.Num() > 1)
			{
				if (UActorComponent* Component = FindComponentByName(Actor, Segments[0]))
				{
					FString Remainder = Segments[1];
					for (int32 Index = 2; Index < Segments.Num(); ++Index)
					{
						Remainder += TEXT(".");
						Remainder += Segments[Index];
					}
					if (ResolvePropertyPath(Component, Remainder, OutProperty, OutValuePtr) && OutProperty && OutValuePtr)
					{
						OutTargetObject = Component;
						OutResolvedPropertyPath = Remainder;
						return true;
					}
					return false;
				}
			}
		}

		return ResolvePropertyPath(Obj, PropertyPath, OutProperty, OutValuePtr) && OutProperty && OutValuePtr;
	}

	enum class EPrimitivePhysicsVirtualProperty : uint8
	{
		None,
		SimulatePhysics,
		EnableGravity,
		UseCCD,
		MassOverrideKg,
		LinearDamping,
		AngularDamping,
		CollisionEnabled
	};
	bool ResolvePrimitivePhysicsVirtualPropertyTarget(UObject* Obj, const FString& PropertyPath, UPrimitiveComponent*& OutPrimitiveComponent, FString& OutResolvedPropertyPath, EPrimitivePhysicsVirtualProperty& OutKind);
	bool ExportPrimitivePhysicsVirtualPropertyText(UPrimitiveComponent* PrimitiveComponent, EPrimitivePhysicsVirtualProperty Kind, FString& OutText);

	bool ExportPropertyText(UObject* Obj, const FString& PropertyPath, FString& OutText, FString& OutClass)
	{
		UPrimitiveComponent* PrimitiveComponent = nullptr;
		FString VirtualResolvedPropertyPath;
		EPrimitivePhysicsVirtualProperty Kind = EPrimitivePhysicsVirtualProperty::None;
		if (ResolvePrimitivePhysicsVirtualPropertyTarget(Obj, PropertyPath, PrimitiveComponent, VirtualResolvedPropertyPath, Kind) &&
			ExportPrimitivePhysicsVirtualPropertyText(PrimitiveComponent, Kind, OutText))
		{
			OutClass = TEXT("PrimitivePhysicsVirtualProperty");
			return true;
		}
		UObject* TargetObject = nullptr;
		FString ResolvedPropertyPath;
		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		if (!ResolveObjectPropertyPath(Obj, PropertyPath, TargetObject, ResolvedPropertyPath, Property, ValuePtr) || !TargetObject || !Property || !ValuePtr)
		{
			return false;
		}
		Property->ExportTextItem_Direct(OutText, ValuePtr, nullptr, TargetObject, PPF_None);
		OutClass = Property->GetClass() ? Property->GetClass()->GetName() : FString();
		return true;
	}

	bool TryReadCanonicalLiteral(const TSharedPtr<FJsonValue>& Value, TSharedPtr<FJsonValue>& OutLiteral)
	{
		if (!Value.IsValid() || Value->Type != EJson::Object) return false;
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		if (!Object.IsValid() || Object->Values.Num() != 1) return false;
		OutLiteral = Object->TryGetField(TEXT("literal"));
		return OutLiteral.IsValid();
	}

	bool TryReadCanonicalPath(const TSharedPtr<FJsonValue>& Value, FString& OutPath, bool& bOutNull)
	{
		bOutNull = false;
		if (!Value.IsValid() || Value->Type != EJson::Object) return false;
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		if (!Object.IsValid() || Object->Values.Num() != 1) return false;
		const TSharedPtr<FJsonValue> Path = Object->TryGetField(TEXT("path"));
		if (!Path.IsValid()) return false;
		if (Path->Type == EJson::Null)
		{
			bOutNull = true;
			OutPath.Reset();
			return true;
		}
		if (Path->Type != EJson::String) return false;
		OutPath = Path->AsString();
		return true;
	}

	TSharedPtr<FJsonObject> MakePropertyResult(
		const FString& Target,
		const FString& PropertyPath,
		const FString& Status,
		const TSharedPtr<FJsonValue>& Requested,
		const TSharedPtr<FJsonValue>& ReadBack,
		const FString& Error)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("target"), Target);
		Result->SetStringField(TEXT("property_path"), PropertyPath);
		Result->SetStringField(TEXT("status"), Status);
		if (Requested.IsValid()) Result->SetField(TEXT("requested_value"), Requested);
		if (ReadBack.IsValid()) Result->SetField(TEXT("value_read_back"), ReadBack);
		Result->SetBoolField(TEXT("readback_verified"), Error.IsEmpty() && ReadBack.IsValid());
		if (!Error.IsEmpty())
		{
			Result->SetStringField(TEXT("error"), Error);
		}
		return Result;
	}

	TSharedPtr<FJsonValue> MakeLiteralValue(const TSharedPtr<FJsonValue>& Literal)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetField(TEXT("literal"), Literal);
		return MakeShared<FJsonValueObject>(Object);
	}

	TSharedPtr<FJsonValue> MakeSelectedValue(const FString& Selected)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("selected"), Selected);
		return MakeShared<FJsonValueObject>(Object);
	}

	FString JoinPathSegments(const TArray<FString>& Segments, int32 StartIndex)
	{
		FString Result;
		for (int32 Index = StartIndex; Index < Segments.Num(); ++Index)
		{
			if (!Result.IsEmpty())
			{
				Result += TEXT(".");
			}
			Result += Segments[Index];
		}
		return Result;
	}

	EPrimitivePhysicsVirtualProperty GetPrimitivePhysicsVirtualPropertyKind(const FString& PropertyPath)
	{
		FString Normalized = PropertyPath.TrimStartAndEnd();
		if (Normalized.StartsWith(TEXT("BodyInstance."), ESearchCase::IgnoreCase))
		{
			Normalized = Normalized.Mid(13);
		}
		if (Normalized.Equals(TEXT("bSimulatePhysics"), ESearchCase::IgnoreCase) ||
			Normalized.Equals(TEXT("SimulatePhysics"), ESearchCase::IgnoreCase) ||
			Normalized.Equals(TEXT("bInstanceSimulatePhysics"), ESearchCase::IgnoreCase))
		{
			return EPrimitivePhysicsVirtualProperty::SimulatePhysics;
		}
		if (Normalized.Equals(TEXT("bEnableGravity"), ESearchCase::IgnoreCase) ||
			Normalized.Equals(TEXT("EnableGravity"), ESearchCase::IgnoreCase))
		{
			return EPrimitivePhysicsVirtualProperty::EnableGravity;
		}
		if (Normalized.Equals(TEXT("bUseCCD"), ESearchCase::IgnoreCase) ||
			Normalized.Equals(TEXT("UseCCD"), ESearchCase::IgnoreCase))
		{
			return EPrimitivePhysicsVirtualProperty::UseCCD;
		}
		if (Normalized.Equals(TEXT("MassInKgOverride"), ESearchCase::IgnoreCase) ||
			Normalized.Equals(TEXT("MassOverrideKg"), ESearchCase::IgnoreCase) ||
			Normalized.Equals(TEXT("MassOverrideInKg"), ESearchCase::IgnoreCase))
		{
			return EPrimitivePhysicsVirtualProperty::MassOverrideKg;
		}
		if (Normalized.Equals(TEXT("LinearDamping"), ESearchCase::IgnoreCase))
		{
			return EPrimitivePhysicsVirtualProperty::LinearDamping;
		}
		if (Normalized.Equals(TEXT("AngularDamping"), ESearchCase::IgnoreCase))
		{
			return EPrimitivePhysicsVirtualProperty::AngularDamping;
		}
		if (Normalized.Equals(TEXT("CollisionEnabled"), ESearchCase::IgnoreCase))
		{
			return EPrimitivePhysicsVirtualProperty::CollisionEnabled;
		}
		return EPrimitivePhysicsVirtualProperty::None;
	}

	bool ResolvePrimitivePhysicsVirtualPropertyTarget(UObject* Obj, const FString& PropertyPath, UPrimitiveComponent*& OutPrimitiveComponent, FString& OutResolvedPropertyPath, EPrimitivePhysicsVirtualProperty& OutKind)
	{
		OutPrimitiveComponent = nullptr;
		OutResolvedPropertyPath.Reset();
		OutKind = EPrimitivePhysicsVirtualProperty::None;
		if (!Obj || PropertyPath.TrimStartAndEnd().IsEmpty())
		{
			return false;
		}

		if (UPrimitiveComponent* PrimitiveComponent = Cast<UPrimitiveComponent>(Obj))
		{
			const EPrimitivePhysicsVirtualProperty Kind = GetPrimitivePhysicsVirtualPropertyKind(PropertyPath);
			if (Kind != EPrimitivePhysicsVirtualProperty::None)
			{
				OutPrimitiveComponent = PrimitiveComponent;
				OutResolvedPropertyPath = PropertyPath.TrimStartAndEnd();
				OutKind = Kind;
				return true;
			}
		}

		if (AActor* Actor = Cast<AActor>(Obj))
		{
			TArray<FString> Segments;
			PropertyPath.ParseIntoArray(Segments, TEXT("."), true);
			if (Segments.Num() > 1)
			{
				if (UPrimitiveComponent* PrimitiveComponent = Cast<UPrimitiveComponent>(FindComponentByName(Actor, Segments[0])))
				{
					const FString Remainder = JoinPathSegments(Segments, 1);
					const EPrimitivePhysicsVirtualProperty Kind = GetPrimitivePhysicsVirtualPropertyKind(Remainder);
					if (Kind != EPrimitivePhysicsVirtualProperty::None)
					{
						OutPrimitiveComponent = PrimitiveComponent;
						OutResolvedPropertyPath = Remainder;
						OutKind = Kind;
						return true;
					}
				}
			}
		}

		return false;
	}

	bool TryReadBoolImportValue(const TSharedPtr<FJsonValue>& Value, bool& OutValue)
	{
		TSharedPtr<FJsonValue> Literal;
		if (!TryReadCanonicalLiteral(Value, Literal) || Literal->Type != EJson::Boolean) return false;
		OutValue = Literal->AsBool();
		return true;
	}

	bool TryReadNumberImportValue(const TSharedPtr<FJsonValue>& Value, double& OutValue)
	{
		TSharedPtr<FJsonValue> Literal;
		if (!TryReadCanonicalLiteral(Value, Literal) || Literal->Type != EJson::Number || !FMath::IsFinite(Literal->AsNumber())) return false;
		OutValue = Literal->AsNumber();
		return true;
	}

	bool TryParseCollisionEnabledValue(const FString& Text, ECollisionEnabled::Type& OutValue)
	{
		const FString Clean = Text.TrimStartAndEnd();
		if (Clean.Equals(TEXT("NoCollision"), ESearchCase::IgnoreCase))
		{
			OutValue = ECollisionEnabled::NoCollision;
			return true;
		}
		if (Clean.Equals(TEXT("QueryOnly"), ESearchCase::IgnoreCase))
		{
			OutValue = ECollisionEnabled::QueryOnly;
			return true;
		}
		if (Clean.Equals(TEXT("PhysicsOnly"), ESearchCase::IgnoreCase))
		{
			OutValue = ECollisionEnabled::PhysicsOnly;
			return true;
		}
		if (Clean.Equals(TEXT("QueryAndPhysics"), ESearchCase::IgnoreCase))
		{
			OutValue = ECollisionEnabled::QueryAndPhysics;
			return true;
		}
		if (Clean.Equals(TEXT("ProbeOnly"), ESearchCase::IgnoreCase))
		{
			OutValue = ECollisionEnabled::ProbeOnly;
			return true;
		}
		if (Clean.Equals(TEXT("QueryAndProbe"), ESearchCase::IgnoreCase))
		{
			OutValue = ECollisionEnabled::QueryAndProbe;
			return true;
		}
		return false;
	}

	FString CollisionEnabledToString(ECollisionEnabled::Type Value)
	{
		switch (Value)
		{
		case ECollisionEnabled::NoCollision:
			return TEXT("NoCollision");
		case ECollisionEnabled::QueryOnly:
			return TEXT("QueryOnly");
		case ECollisionEnabled::PhysicsOnly:
			return TEXT("PhysicsOnly");
		case ECollisionEnabled::QueryAndPhysics:
			return TEXT("QueryAndPhysics");
		case ECollisionEnabled::ProbeOnly:
			return TEXT("ProbeOnly");
		case ECollisionEnabled::QueryAndProbe:
			return TEXT("QueryAndProbe");
		default:
			return FString::FromInt(static_cast<int32>(Value));
		}
	}

	bool ExportPrimitivePhysicsVirtualPropertyText(UPrimitiveComponent* PrimitiveComponent, EPrimitivePhysicsVirtualProperty Kind, FString& OutText)
	{
		if (!PrimitiveComponent)
		{
			return false;
		}
		switch (Kind)
		{
		case EPrimitivePhysicsVirtualProperty::SimulatePhysics:
			OutText = PrimitiveComponent->IsSimulatingPhysics() ? TEXT("true") : TEXT("false");
			return true;
		case EPrimitivePhysicsVirtualProperty::EnableGravity:
			OutText = PrimitiveComponent->IsGravityEnabled() ? TEXT("true") : TEXT("false");
			return true;
		case EPrimitivePhysicsVirtualProperty::UseCCD:
			OutText = PrimitiveComponent->BodyInstance.bUseCCD ? TEXT("true") : TEXT("false");
			return true;
		case EPrimitivePhysicsVirtualProperty::MassOverrideKg:
			OutText = FString::SanitizeFloat(PrimitiveComponent->BodyInstance.GetMassOverride(), 6);
			return true;
		case EPrimitivePhysicsVirtualProperty::LinearDamping:
			OutText = FString::SanitizeFloat(PrimitiveComponent->GetLinearDamping(), 6);
			return true;
		case EPrimitivePhysicsVirtualProperty::AngularDamping:
			OutText = FString::SanitizeFloat(PrimitiveComponent->GetAngularDamping(), 6);
			return true;
		case EPrimitivePhysicsVirtualProperty::CollisionEnabled:
			OutText = CollisionEnabledToString(PrimitiveComponent->GetCollisionEnabled());
			return true;
		default:
			return false;
		}
	}

	void RefreshObjectAfterPropertyImport(UObject* TargetObject, const FString& ResolvedPropertyPath, const FString& CollisionProfileName, TSharedPtr<FJsonObject>& InOutResult);
	bool SetSkeletalMeshComponentAssetFromJson(UObject* TargetObject, const FString& PropertyPath, const FString& ResolvedPropertyPath, const TSharedPtr<FJsonValue>& Value, TSharedPtr<FJsonObject>& OutResult, FString& OutError);

	bool SetPrimitivePhysicsVirtualPropertyFromJson(UObject* Obj, const FString& PropertyPath, const TSharedPtr<FJsonValue>& Value, UWorld* World, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
	{
		UPrimitiveComponent* PrimitiveComponent = nullptr;
		FString ResolvedPropertyPath;
		EPrimitivePhysicsVirtualProperty Kind = EPrimitivePhysicsVirtualProperty::None;
		if (!ResolvePrimitivePhysicsVirtualPropertyTarget(Obj, PropertyPath, PrimitiveComponent, ResolvedPropertyPath, Kind))
		{
			return false;
		}

		PrimitiveComponent->Modify();
		bool bApplied = false;
		switch (Kind)
		{
		case EPrimitivePhysicsVirtualProperty::SimulatePhysics:
		{
			bool bValue = false;
			if (!TryReadBoolImportValue(Value, bValue))
			{
				OutError = TEXT("invalid_bool_value");
				break;
			}
			PrimitiveComponent->SetSimulatePhysics(bValue);
			bApplied = true;
			break;
		}
		case EPrimitivePhysicsVirtualProperty::EnableGravity:
		{
			bool bValue = false;
			if (!TryReadBoolImportValue(Value, bValue))
			{
				OutError = TEXT("invalid_bool_value");
				break;
			}
			PrimitiveComponent->SetEnableGravity(bValue);
			bApplied = true;
			break;
		}
		case EPrimitivePhysicsVirtualProperty::UseCCD:
		{
			bool bValue = false;
			if (!TryReadBoolImportValue(Value, bValue))
			{
				OutError = TEXT("invalid_bool_value");
				break;
			}
			PrimitiveComponent->SetAllUseCCD(bValue);
			bApplied = true;
			break;
		}
		case EPrimitivePhysicsVirtualProperty::MassOverrideKg:
		{
			double NumberValue = 0.0;
			if (!TryReadNumberImportValue(Value, NumberValue))
			{
				OutError = TEXT("invalid_number_value");
				break;
			}
			PrimitiveComponent->SetMassOverrideInKg(NAME_None, static_cast<float>(NumberValue), NumberValue > 0.0);
			bApplied = true;
			break;
		}
		case EPrimitivePhysicsVirtualProperty::LinearDamping:
		{
			double NumberValue = 0.0;
			if (!TryReadNumberImportValue(Value, NumberValue))
			{
				OutError = TEXT("invalid_number_value");
				break;
			}
			PrimitiveComponent->SetLinearDamping(static_cast<float>(NumberValue));
			bApplied = true;
			break;
		}
		case EPrimitivePhysicsVirtualProperty::AngularDamping:
		{
			double NumberValue = 0.0;
			if (!TryReadNumberImportValue(Value, NumberValue))
			{
				OutError = TEXT("invalid_number_value");
				break;
			}
			PrimitiveComponent->SetAngularDamping(static_cast<float>(NumberValue));
			bApplied = true;
			break;
		}
		case EPrimitivePhysicsVirtualProperty::CollisionEnabled:
		{
			const TSharedPtr<FJsonObject> Object = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
			FString Selected;
			ECollisionEnabled::Type CollisionEnabled = ECollisionEnabled::NoCollision;
			if (!Object.IsValid() || Object->Values.Num() != 1 || !Object->TryGetStringField(TEXT("selected"), Selected)
				|| !TryParseCollisionEnabledValue(Selected, CollisionEnabled))
			{
				OutError = TEXT("invalid_collision_enabled");
				break;
			}
			PrimitiveComponent->SetCollisionEnabled(CollisionEnabled);
			bApplied = true;
			break;
		}
		default:
			OutError = TEXT("unsupported_primitive_physics_virtual_property");
			break;
		}

		if (!bApplied)
		{
			OutResult = MakePropertyResult(PrimitiveComponent->GetPathName(), PropertyPath, TEXT("apply_failed"), Value, nullptr, OutError);
			return true;
		}

		PrimitiveComponent->PostEditChange();
		PrimitiveComponent->MarkPackageDirty();
		TSharedPtr<FJsonValue> ReadBack;
		switch (Kind)
		{
		case EPrimitivePhysicsVirtualProperty::SimulatePhysics:
			ReadBack = MakeLiteralValue(MakeShared<FJsonValueBoolean>(PrimitiveComponent->IsSimulatingPhysics()));
			break;
		case EPrimitivePhysicsVirtualProperty::EnableGravity:
			ReadBack = MakeLiteralValue(MakeShared<FJsonValueBoolean>(PrimitiveComponent->IsGravityEnabled()));
			break;
		case EPrimitivePhysicsVirtualProperty::UseCCD:
			ReadBack = MakeLiteralValue(MakeShared<FJsonValueBoolean>(PrimitiveComponent->BodyInstance.bUseCCD != 0));
			break;
		case EPrimitivePhysicsVirtualProperty::MassOverrideKg:
			ReadBack = MakeLiteralValue(MakeShared<FJsonValueNumber>(PrimitiveComponent->BodyInstance.GetMassOverride()));
			break;
		case EPrimitivePhysicsVirtualProperty::LinearDamping:
			ReadBack = MakeLiteralValue(MakeShared<FJsonValueNumber>(PrimitiveComponent->GetLinearDamping()));
			break;
		case EPrimitivePhysicsVirtualProperty::AngularDamping:
			ReadBack = MakeLiteralValue(MakeShared<FJsonValueNumber>(PrimitiveComponent->GetAngularDamping()));
			break;
		case EPrimitivePhysicsVirtualProperty::CollisionEnabled:
			ReadBack = MakeSelectedValue(CollisionEnabledToString(PrimitiveComponent->GetCollisionEnabled()));
			break;
		default:
			break;
		}
		OutResult = MakePropertyResult(PrimitiveComponent->GetPathName(), PropertyPath, TEXT("applied"), Value, ReadBack, TEXT(""));
		OutResult->SetStringField(TEXT("resolved_property_path"), ResolvedPropertyPath);
		OutResult->SetStringField(TEXT("property_assignment_method"), TEXT("primitive_physics_setter"));
		OutResult->SetBoolField(TEXT("primitive_physics_setter_applied"), true);
		RefreshObjectAfterPropertyImport(PrimitiveComponent, ResolvedPropertyPath, FString(), OutResult);
		return true;
	}

	bool IsSkeletalMeshComponentAssetPropertyPath(const FString& ResolvedPropertyPath)
	{
		const FString Normalized = ResolvedPropertyPath.TrimStartAndEnd();
		return Normalized.Equals(TEXT("SkeletalMesh"), ESearchCase::IgnoreCase)
			|| Normalized.Equals(TEXT("SkeletalMeshAsset"), ESearchCase::IgnoreCase)
			|| Normalized.Equals(TEXT("SkinnedAsset"), ESearchCase::IgnoreCase);
	}

	bool SetSkeletalMeshComponentAssetFromJson(UObject* TargetObject, const FString& PropertyPath, const FString& ResolvedPropertyPath, const TSharedPtr<FJsonValue>& Value, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
	{
		USkeletalMeshComponent* SkeletalMeshComponent = Cast<USkeletalMeshComponent>(TargetObject);
		if (!SkeletalMeshComponent || !IsSkeletalMeshComponentAssetPropertyPath(ResolvedPropertyPath))
		{
			return false;
		}

		FString MeshObjectPath;
		bool bNullPath = false;
		if (!TryReadCanonicalPath(Value, MeshObjectPath, bNullPath))
		{
			OutError = TEXT("invalid_skeletal_mesh_reference");
			OutResult = MakePropertyResult(TargetObject->GetPathName(), PropertyPath, TEXT("invalid_value"), Value, nullptr, OutError);
			return true;
		}
		USkeletalMesh* Mesh = nullptr;
		if (!bNullPath)
		{
			Mesh = Cast<USkeletalMesh>(StaticLoadObject(USkeletalMesh::StaticClass(), nullptr, *MeshObjectPath));
			if (!Mesh)
			{
				OutError = TEXT("skeletal_mesh_asset_not_found");
				OutResult = MakePropertyResult(TargetObject->GetPathName(), PropertyPath, TEXT("apply_failed"), Value, nullptr, OutError);
				OutResult->SetStringField(TEXT("resolved_property_path"), ResolvedPropertyPath);
				OutResult->SetStringField(TEXT("property_assignment_method"), TEXT("skeletal_mesh_component_setter"));
				OutResult->SetStringField(TEXT("normalized_object_path"), MeshObjectPath);
				return true;
			}
		}

		SkeletalMeshComponent->Modify();
		SkeletalMeshComponent->SetSkeletalMesh(Mesh, true);
		SkeletalMeshComponent->UpdateBounds();
		SkeletalMeshComponent->MarkRenderTransformDirty();
		SkeletalMeshComponent->MarkRenderStateDirty();
		SkeletalMeshComponent->PostEditChange();
		SkeletalMeshComponent->MarkPackageDirty();

		TSharedPtr<FJsonObject> ReadBackObject = MakeShared<FJsonObject>();
		if (Mesh) ReadBackObject->SetStringField(TEXT("path"), Mesh->GetPathName()); else ReadBackObject->SetField(TEXT("path"), MakeShared<FJsonValueNull>());
		OutResult = MakePropertyResult(TargetObject->GetPathName(), PropertyPath, TEXT("applied"), Value, MakeShared<FJsonValueObject>(ReadBackObject), TEXT(""));
		OutResult->SetStringField(TEXT("resolved_property_path"), ResolvedPropertyPath);
		OutResult->SetStringField(TEXT("property_assignment_method"), TEXT("skeletal_mesh_component_setter"));
		OutResult->SetBoolField(TEXT("skeletal_mesh_setter_applied"), true);
		OutResult->SetStringField(TEXT("normalized_object_path"), MeshObjectPath);
		OutResult->SetStringField(TEXT("skeletal_mesh_path_read_back"), Mesh ? Mesh->GetPathName() : TEXT("None"));
		RefreshObjectAfterPropertyImport(TargetObject, ResolvedPropertyPath, FString(), OutResult);
		return true;
	}

	bool PropertyPathTouchesPrimitiveCollisionState(const FString& PropertyPath)
	{
		const FString Normalized = PropertyPath.TrimStartAndEnd();
		return Normalized.StartsWith(TEXT("BodyInstance."), ESearchCase::IgnoreCase)
			|| Normalized.Contains(TEXT("Collision"), ESearchCase::IgnoreCase)
			|| Normalized.Equals(TEXT("CanCharacterStepUpOn"), ESearchCase::IgnoreCase);
	}

	bool IsPrimitiveCollisionProfilePropertyPath(const FString& PropertyPath)
	{
		const FString Normalized = PropertyPath.TrimStartAndEnd();
		return Normalized.Equals(TEXT("BodyInstance.CollisionProfileName"), ESearchCase::IgnoreCase)
			|| Normalized.Equals(TEXT("CollisionProfileName"), ESearchCase::IgnoreCase);
	}

	void RefreshObjectAfterPropertyImport(UObject* TargetObject, const FString& ResolvedPropertyPath, const FString& CollisionProfileName, TSharedPtr<FJsonObject>& InOutResult)
	{
		if (!TargetObject || !InOutResult.IsValid())
		{
			return;
		}

		UActorComponent* Component = Cast<UActorComponent>(TargetObject);
		UPrimitiveComponent* PrimitiveComponent = Cast<UPrimitiveComponent>(TargetObject);
		if (!Component)
		{
			return;
		}

		bool bRenderStateRefreshed = false;
		bool bPrimitiveCollisionStateRefreshed = false;
		bool bCollisionProfileSetterApplied = false;

		const FString NormalizedPropertyPath = ResolvedPropertyPath.TrimStartAndEnd();
		if (PrimitiveComponent)
		{
			if (IsPrimitiveCollisionProfilePropertyPath(NormalizedPropertyPath))
			{
				const FString ProfileName = CollisionProfileName.TrimStartAndEnd();
				if (!ProfileName.IsEmpty())
				{
					PrimitiveComponent->SetCollisionProfileName(FName(*ProfileName));
					if (ProfileName.Equals(TEXT("NoCollision"), ESearchCase::IgnoreCase))
					{
						PrimitiveComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
						InOutResult->SetBoolField(TEXT("collision_enabled_no_collision_applied"), true);
					}
					bCollisionProfileSetterApplied = true;
				}
			}

			if (PropertyPathTouchesPrimitiveCollisionState(NormalizedPropertyPath))
			{
				PrimitiveComponent->RecreatePhysicsState();
				PrimitiveComponent->UpdateOverlaps();
				PrimitiveComponent->UpdateBounds();
				PrimitiveComponent->MarkRenderStateDirty();
				PrimitiveComponent->MarkRenderTransformDirty();
				bPrimitiveCollisionStateRefreshed = true;
				bRenderStateRefreshed = true;
			}
			else
			{
				PrimitiveComponent->MarkRenderStateDirty();
				bRenderStateRefreshed = true;
			}
		}
		else if (USceneComponent* SceneComponent = Cast<USceneComponent>(Component))
		{
			SceneComponent->MarkRenderStateDirty();
			bRenderStateRefreshed = true;
		}

		if (bCollisionProfileSetterApplied)
		{
			InOutResult->SetBoolField(TEXT("collision_profile_setter_applied"), true);
		}
		if (bPrimitiveCollisionStateRefreshed)
		{
			InOutResult->SetBoolField(TEXT("primitive_collision_state_refreshed"), true);
		}
		if (bRenderStateRefreshed)
		{
			InOutResult->SetBoolField(TEXT("render_state_refreshed"), true);
		}
	}

	bool SetObjectPropertyFromJson(UObject* Obj, const FString& PropertyPath, const TSharedPtr<FJsonValue>& Value, UWorld* World, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
	{
		OutResult = MakeShared<FJsonObject>();
		if (!Obj)
		{
			OutError = TEXT("property_target_null");
			OutResult = MakePropertyResult(TEXT(""), PropertyPath, TEXT("target_null"), Value, nullptr, OutError);
			return false;
		}
		if (SetPrimitivePhysicsVirtualPropertyFromJson(Obj, PropertyPath, Value, World, OutResult, OutError))
		{
			return OutError.IsEmpty();
		}
		UObject* TargetObject = nullptr;
		FString ResolvedPropertyPath;
		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		if (!ResolveObjectPropertyPath(Obj, PropertyPath, TargetObject, ResolvedPropertyPath, Property, ValuePtr) || !TargetObject || !Property || !ValuePtr)
		{
			OutError = TEXT("property_not_found");
			OutResult = MakePropertyResult(Obj->GetPathName(), PropertyPath, TEXT("property_not_found"), Value, nullptr, OutError);
			return false;
		}

		if (SetSkeletalMeshComponentAssetFromJson(TargetObject, PropertyPath, ResolvedPropertyPath, Value, OutResult, OutError))
		{
			return OutError.IsEmpty();
		}
		if (UPrimitiveComponent* PrimitiveComponent = Cast<UPrimitiveComponent>(TargetObject))
		{
			if (IsPrimitiveCollisionProfilePropertyPath(ResolvedPropertyPath))
			{
				TSharedPtr<FJsonValue> Literal;
				const FString ProfileName = TryReadCanonicalLiteral(Value, Literal) && Literal->Type == EJson::String ? Literal->AsString() : FString();
				if (ProfileName.IsEmpty())
				{
					OutError = TEXT("empty_collision_profile_name");
					OutResult = MakePropertyResult(TargetObject->GetPathName(), PropertyPath, TEXT("invalid_value"), Value, nullptr, OutError);
					return false;
				}

				TargetObject->Modify();
				PrimitiveComponent->SetCollisionProfileName(FName(*ProfileName));
				TargetObject->PostEditChange();
				TargetObject->MarkPackageDirty();

				const TSharedPtr<FJsonValue> ReadBack = MakeLiteralValue(MakeShared<FJsonValueString>(PrimitiveComponent->GetCollisionProfileName().ToString()));
				OutResult = MakePropertyResult(TargetObject->GetPathName(), PropertyPath, TEXT("applied"), Value, ReadBack, TEXT(""));
				OutResult->SetStringField(TEXT("resolved_property_path"), ResolvedPropertyPath);
				OutResult->SetStringField(TEXT("property_assignment_method"), TEXT("collision_profile_setter"));
				OutResult->SetBoolField(TEXT("collision_profile_setter_applied"), true);
				RefreshObjectAfterPropertyImport(TargetObject, ResolvedPropertyPath, ProfileName, OutResult);
				return true;
			}
		}

		TSharedPtr<FJsonObject> Descriptor = MakeShared<FJsonObject>();
		Descriptor->SetStringField(TEXT("property_path"), ResolvedPropertyPath);
		Descriptor->SetStringField(TEXT("value_kind"), UeAgentPropertyValueCodec::GetValueKind(Property));
		UeAgentPropertyValueCodec::AddTypeMetadata(Property, Descriptor);
		Descriptor->SetField(TEXT("value"), Value);
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("schema"), TEXT("uai.uobject_properties.v2"));
		Payload->SetArrayField(TEXT("properties"), { MakeShared<FJsonValueObject>(Descriptor) });
		UeAgentUObjectProperties::FApplyOptions ApplyOptions;
		const TSharedPtr<FJsonObject> ApplySummary = UeAgentUObjectProperties::ApplyObjectProperties(TargetObject, Payload, ApplyOptions);
		FString ApplyStatus;
		ApplySummary->TryGetStringField(TEXT("status"), ApplyStatus);
		if (ApplyStatus != TEXT("applied"))
		{
			ApplySummary->TryGetStringField(TEXT("reason"), OutError);
			if (OutError.IsEmpty()) OutError = TEXT("property_apply_failed");
			OutResult = MakePropertyResult(TargetObject->GetPathName(), PropertyPath, TEXT("apply_failed"), Value, nullptr, OutError);
			OutResult->SetObjectField(TEXT("apply_summary"), ApplySummary);
			return false;
		}

		UeAgentPropertyValueCodec::FOptions CodecOptions;
		const TSharedPtr<FJsonValue> ReadBack = UeAgentPropertyValueCodec::Encode(Property, ValuePtr, TargetObject, CodecOptions);
		OutResult = MakePropertyResult(TargetObject->GetPathName(), PropertyPath, TEXT("applied"), Value, ReadBack, TEXT(""));
		OutResult->SetStringField(TEXT("resolved_property_path"), ResolvedPropertyPath);
		OutResult->SetObjectField(TEXT("apply_summary"), ApplySummary);
		RefreshObjectAfterPropertyImport(TargetObject, ResolvedPropertyPath, FString(), OutResult);
		return true;
	}

	bool ValidatePropertyExists(UObject* Obj, const FString& PropertyPath)
	{
		UPrimitiveComponent* PrimitiveComponent = nullptr;
		FString VirtualResolvedPropertyPath;
		EPrimitivePhysicsVirtualProperty Kind = EPrimitivePhysicsVirtualProperty::None;
		if (ResolvePrimitivePhysicsVirtualPropertyTarget(Obj, PropertyPath, PrimitiveComponent, VirtualResolvedPropertyPath, Kind))
		{
			return true;
		}
		UObject* TargetObject = nullptr;
		FString ResolvedPropertyPath;
		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		return ResolveObjectPropertyPath(Obj, PropertyPath, TargetObject, ResolvedPropertyPath, Property, ValuePtr) && TargetObject && Property && ValuePtr;
	}
}
