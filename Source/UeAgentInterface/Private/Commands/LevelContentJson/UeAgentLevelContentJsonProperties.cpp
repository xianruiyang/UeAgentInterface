// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentLevelContentJsonProperties.h"
#include "UeAgentLevelContentJsonCore.h"

#include "Components/ActorComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/PackageName.h"

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

	bool ReadLinearColorValue(const TSharedPtr<FJsonValue>& Value, FString& OutText)
	{
		if (!Value.IsValid())
		{
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> Arr;
		if (Value->Type == EJson::Array)
		{
			Arr = Value->AsArray();
		}
		else if (Value->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> Obj = Value->AsObject();
			const TSharedPtr<FJsonValue> Inner = Obj.IsValid() ? Obj->TryGetField(TEXT("value")) : nullptr;
			if (Inner.IsValid() && Inner->Type == EJson::Array)
			{
				Arr = Inner->AsArray();
			}
		}
		if (Arr.Num() < 3)
		{
			return false;
		}
		const double R = Arr[0].IsValid() ? Arr[0]->AsNumber() : 0.0;
		const double G = Arr[1].IsValid() ? Arr[1]->AsNumber() : 0.0;
		const double B = Arr[2].IsValid() ? Arr[2]->AsNumber() : 0.0;
		const double A = Arr.Num() > 3 && Arr[3].IsValid() ? Arr[3]->AsNumber() : 1.0;
		OutText = FString::Printf(TEXT("(R=%g,G=%g,B=%g,A=%g)"), R, G, B, A);
		return true;
	}

	FString JsonValueToImportText(const TSharedPtr<FJsonValue>& Value, UWorld* World)
	{
		if (!Value.IsValid())
		{
			return TEXT("None");
		}
		if (Value->Type == EJson::Boolean)
		{
			return Value->AsBool() ? TEXT("true") : TEXT("false");
		}
		if (Value->Type == EJson::Number)
		{
			return FString::SanitizeFloat(Value->AsNumber(), 6);
		}
		if (Value->Type == EJson::String)
		{
			return Value->AsString();
		}
		if (Value->Type == EJson::Array)
		{
			FVector Vector;
			if (TryReadVectorValue(Value, Vector))
			{
				return FString::Printf(TEXT("(X=%g,Y=%g,Z=%g)"), Vector.X, Vector.Y, Vector.Z);
			}
			return JsonValueToString(Value);
		}
		if (Value->Type != EJson::Object)
		{
			return JsonValueToString(Value);
		}

		const TSharedPtr<FJsonObject> Obj = Value->AsObject();
		if (!Obj.IsValid())
		{
			return TEXT("None");
		}

		FString ValueText;
		if (Obj->TryGetStringField(TEXT("value_text"), ValueText))
		{
			return ValueText;
		}
		if (Obj->TryGetStringField(TEXT("asset"), ValueText) || Obj->TryGetStringField(TEXT("object"), ValueText) ||
			Obj->TryGetStringField(TEXT("class"), ValueText))
		{
			return ValueText;
		}
		FString ActorId;
		if (Obj->TryGetStringField(TEXT("actor_id"), ActorId) && World)
		{
			if (AActor* Actor = FindActorByNameLabelOrManagedId(World, ActorId))
			{
				return Actor->GetPathName();
			}
			return ActorId;
		}
		const TSharedPtr<FJsonValue> InnerValue = Obj->TryGetField(TEXT("value"));
		FString TypeText;
		Obj->TryGetStringField(TEXT("type"), TypeText);
		if (InnerValue.IsValid())
		{
			if (TypeText.Equals(TEXT("LinearColor"), ESearchCase::IgnoreCase) ||
				TypeText.Equals(TEXT("Color"), ESearchCase::IgnoreCase))
			{
				FString ColorText;
				if (ReadLinearColorValue(InnerValue, ColorText))
				{
					return ColorText;
				}
			}
			if (TypeText.Equals(TEXT("Rotator"), ESearchCase::IgnoreCase))
			{
				FRotator Rotator;
				if (TryReadRotatorValue(InnerValue, Rotator))
				{
					return FString::Printf(TEXT("(Pitch=%g,Yaw=%g,Roll=%g)"), Rotator.Pitch, Rotator.Yaw, Rotator.Roll);
				}
			}
			return JsonValueToImportText(InnerValue, World);
		}
		return JsonValueToString(Value);
	}

	TSharedPtr<FJsonObject> MakePropertyResult(const FString& Target, const FString& PropertyPath, const FString& Status, const FString& Requested, const FString& ReadBack, const FString& Error)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("target"), Target);
		Result->SetStringField(TEXT("property_path"), PropertyPath);
		Result->SetStringField(TEXT("status"), Status);
		Result->SetStringField(TEXT("requested_value_text"), Requested);
		Result->SetStringField(TEXT("property_value_read_back"), ReadBack);
		Result->SetBoolField(TEXT("property_import_verified"), Error.IsEmpty() && !ReadBack.IsEmpty());
		if (!Error.IsEmpty())
		{
			Result->SetStringField(TEXT("error"), Error);
		}
		return Result;
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
		if (!Value.IsValid())
		{
			return false;
		}
		if (Value->Type == EJson::Boolean)
		{
			OutValue = Value->AsBool();
			return true;
		}
		if (Value->Type == EJson::Number)
		{
			OutValue = !FMath::IsNearlyZero(Value->AsNumber());
			return true;
		}
		if (Value->Type == EJson::String)
		{
			const FString Text = Value->AsString().TrimStartAndEnd();
			if (Text.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("1"), ESearchCase::IgnoreCase))
			{
				OutValue = true;
				return true;
			}
			if (Text.Equals(TEXT("false"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("0"), ESearchCase::IgnoreCase))
			{
				OutValue = false;
				return true;
			}
		}
		return false;
	}

	bool TryReadNumberImportValue(const TSharedPtr<FJsonValue>& Value, double& OutValue)
	{
		if (!Value.IsValid())
		{
			return false;
		}
		if (Value->Type == EJson::Number)
		{
			OutValue = Value->AsNumber();
			return true;
		}
		if (Value->Type == EJson::String)
		{
			return LexTryParseString(OutValue, *Value->AsString().TrimStartAndEnd());
		}
		return false;
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

	FString SanitizeNameImportText(const FString& ImportText);
	void RefreshObjectAfterPropertyImport(UObject* TargetObject, const FString& ResolvedPropertyPath, const FString& ImportText, TSharedPtr<FJsonObject>& InOutResult);
	bool SetSkeletalMeshComponentAssetFromJson(UObject* TargetObject, const FString& PropertyPath, const FString& ResolvedPropertyPath, FProperty* Property, void* ValuePtr, const FString& ImportText, TSharedPtr<FJsonObject>& OutResult, FString& OutError);

	bool SetPrimitivePhysicsVirtualPropertyFromJson(UObject* Obj, const FString& PropertyPath, const TSharedPtr<FJsonValue>& Value, UWorld* World, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
	{
		UPrimitiveComponent* PrimitiveComponent = nullptr;
		FString ResolvedPropertyPath;
		EPrimitivePhysicsVirtualProperty Kind = EPrimitivePhysicsVirtualProperty::None;
		if (!ResolvePrimitivePhysicsVirtualPropertyTarget(Obj, PropertyPath, PrimitiveComponent, ResolvedPropertyPath, Kind))
		{
			return false;
		}

		const FString ImportText = JsonValueToImportText(Value, World);
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
			ECollisionEnabled::Type CollisionEnabled = ECollisionEnabled::NoCollision;
			if (!TryParseCollisionEnabledValue(SanitizeNameImportText(ImportText), CollisionEnabled))
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
			OutResult = MakePropertyResult(PrimitiveComponent->GetPathName(), PropertyPath, TEXT("import_failed"), ImportText, TEXT(""), OutError);
			return true;
		}

		PrimitiveComponent->PostEditChange();
		PrimitiveComponent->MarkPackageDirty();
		FString ReadBack;
		ExportPrimitivePhysicsVirtualPropertyText(PrimitiveComponent, Kind, ReadBack);
		OutResult = MakePropertyResult(PrimitiveComponent->GetPathName(), PropertyPath, TEXT("imported_and_read_back"), ImportText, ReadBack, TEXT(""));
		OutResult->SetStringField(TEXT("resolved_property_path"), ResolvedPropertyPath);
		OutResult->SetStringField(TEXT("property_assignment_method"), TEXT("primitive_physics_setter"));
		OutResult->SetBoolField(TEXT("primitive_physics_setter_applied"), true);
		RefreshObjectAfterPropertyImport(PrimitiveComponent, ResolvedPropertyPath, ImportText, OutResult);
		return true;
	}

	FString SanitizeNameImportText(const FString& ImportText)
	{
		FString Clean = ImportText.TrimStartAndEnd();
		if (Clean.StartsWith(TEXT("Name="), ESearchCase::IgnoreCase))
		{
			Clean = Clean.Mid(5).TrimStartAndEnd();
		}
		Clean.TrimQuotesInline();
		return Clean;
	}

	FString NormalizeObjectPathImportText(const FString& ImportText)
	{
		FString Clean = SanitizeNameImportText(ImportText);
		if (Clean.StartsWith(TEXT("Object="), ESearchCase::IgnoreCase))
		{
			Clean = Clean.Mid(7).TrimStartAndEnd();
		}
		Clean.TrimQuotesInline();

		int32 FirstQuote = INDEX_NONE;
		int32 LastQuote = INDEX_NONE;
		if (Clean.FindChar(TEXT('\''), FirstQuote) && Clean.FindLastChar(TEXT('\''), LastQuote) && LastQuote > FirstQuote)
		{
			Clean = Clean.Mid(FirstQuote + 1, LastQuote - FirstQuote - 1).TrimStartAndEnd();
		}

		if (Clean.StartsWith(TEXT("/")) && !Clean.Contains(TEXT(".")))
		{
			const FString AssetName = FPackageName::GetLongPackageAssetName(Clean);
			if (!AssetName.IsEmpty())
			{
				Clean = FString::Printf(TEXT("%s.%s"), *Clean, *AssetName);
			}
		}
		return Clean;
	}

	bool IsSkeletalMeshComponentAssetPropertyPath(const FString& ResolvedPropertyPath)
	{
		const FString Normalized = ResolvedPropertyPath.TrimStartAndEnd();
		return Normalized.Equals(TEXT("SkeletalMesh"), ESearchCase::IgnoreCase)
			|| Normalized.Equals(TEXT("SkeletalMeshAsset"), ESearchCase::IgnoreCase)
			|| Normalized.Equals(TEXT("SkinnedAsset"), ESearchCase::IgnoreCase);
	}

	bool SetSkeletalMeshComponentAssetFromJson(UObject* TargetObject, const FString& PropertyPath, const FString& ResolvedPropertyPath, FProperty* Property, void* ValuePtr, const FString& ImportText, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
	{
		USkeletalMeshComponent* SkeletalMeshComponent = Cast<USkeletalMeshComponent>(TargetObject);
		if (!SkeletalMeshComponent || !IsSkeletalMeshComponentAssetPropertyPath(ResolvedPropertyPath))
		{
			return false;
		}

		const FString MeshObjectPath = NormalizeObjectPathImportText(ImportText);
		USkeletalMesh* Mesh = nullptr;
		if (!MeshObjectPath.IsEmpty()
			&& !MeshObjectPath.Equals(TEXT("None"), ESearchCase::IgnoreCase)
			&& !MeshObjectPath.Equals(TEXT("null"), ESearchCase::IgnoreCase))
		{
			Mesh = Cast<USkeletalMesh>(StaticLoadObject(USkeletalMesh::StaticClass(), nullptr, *MeshObjectPath));
			if (!Mesh)
			{
				OutError = TEXT("skeletal_mesh_asset_not_found");
				OutResult = MakePropertyResult(TargetObject->GetPathName(), PropertyPath, TEXT("import_failed"), ImportText, TEXT(""), OutError);
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

		FString Exported;
		if (Property && ValuePtr)
		{
			Property->ExportTextItem_Direct(Exported, ValuePtr, nullptr, TargetObject, PPF_None);
		}
		if (Exported.IsEmpty())
		{
			Exported = Mesh ? Mesh->GetPathName() : TEXT("None");
		}

		OutResult = MakePropertyResult(TargetObject->GetPathName(), PropertyPath, TEXT("imported_and_read_back"), ImportText, Exported, TEXT(""));
		OutResult->SetStringField(TEXT("resolved_property_path"), ResolvedPropertyPath);
		OutResult->SetStringField(TEXT("property_assignment_method"), TEXT("skeletal_mesh_component_setter"));
		OutResult->SetBoolField(TEXT("skeletal_mesh_setter_applied"), true);
		OutResult->SetStringField(TEXT("normalized_object_path"), MeshObjectPath);
		OutResult->SetStringField(TEXT("skeletal_mesh_path_read_back"), Mesh ? Mesh->GetPathName() : TEXT("None"));
		RefreshObjectAfterPropertyImport(TargetObject, ResolvedPropertyPath, ImportText, OutResult);
		Exported.Reset();
		if (Property && ValuePtr)
		{
			Property->ExportTextItem_Direct(Exported, ValuePtr, nullptr, TargetObject, PPF_None);
			if (!Exported.IsEmpty())
			{
				OutResult->SetStringField(TEXT("property_value_read_back"), Exported);
			}
		}
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

	void RefreshObjectAfterPropertyImport(UObject* TargetObject, const FString& ResolvedPropertyPath, const FString& ImportText, TSharedPtr<FJsonObject>& InOutResult)
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
				const FString ProfileName = SanitizeNameImportText(ImportText);
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
			OutResult = MakePropertyResult(TEXT(""), PropertyPath, TEXT("target_null"), TEXT(""), TEXT(""), OutError);
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
			OutResult = MakePropertyResult(Obj->GetPathName(), PropertyPath, TEXT("property_not_found"), TEXT(""), TEXT(""), OutError);
			return false;
		}

		const FString ImportText = JsonValueToImportText(Value, World);
		TargetObject->Modify();
		if (SetSkeletalMeshComponentAssetFromJson(TargetObject, PropertyPath, ResolvedPropertyPath, Property, ValuePtr, ImportText, OutResult, OutError))
		{
			return OutError.IsEmpty();
		}
		if (UPrimitiveComponent* PrimitiveComponent = Cast<UPrimitiveComponent>(TargetObject))
		{
			if (IsPrimitiveCollisionProfilePropertyPath(ResolvedPropertyPath))
			{
				const FString ProfileName = SanitizeNameImportText(ImportText);
				if (ProfileName.IsEmpty())
				{
					OutError = TEXT("empty_collision_profile_name");
					OutResult = MakePropertyResult(TargetObject->GetPathName(), PropertyPath, TEXT("import_failed"), ImportText, TEXT(""), OutError);
					return false;
				}

				PrimitiveComponent->SetCollisionProfileName(FName(*ProfileName));
				TargetObject->PostEditChange();
				TargetObject->MarkPackageDirty();

				FString Exported;
				Property->ExportTextItem_Direct(Exported, ValuePtr, nullptr, TargetObject, PPF_None);
				OutResult = MakePropertyResult(TargetObject->GetPathName(), PropertyPath, TEXT("imported_and_read_back"), ImportText, Exported, TEXT(""));
				OutResult->SetStringField(TEXT("resolved_property_path"), ResolvedPropertyPath);
				OutResult->SetStringField(TEXT("property_assignment_method"), TEXT("collision_profile_setter"));
				OutResult->SetBoolField(TEXT("collision_profile_setter_applied"), true);
				RefreshObjectAfterPropertyImport(TargetObject, ResolvedPropertyPath, ImportText, OutResult);
				Exported.Reset();
				Property->ExportTextItem_Direct(Exported, ValuePtr, nullptr, TargetObject, PPF_None);
				OutResult->SetStringField(TEXT("property_value_read_back"), Exported);
				return true;
			}
		}
		if (!Property->ImportText_Direct(*ImportText, ValuePtr, TargetObject, PPF_None))
		{
			OutError = TEXT("property_import_failed");
			OutResult = MakePropertyResult(TargetObject->GetPathName(), PropertyPath, TEXT("import_failed"), ImportText, TEXT(""), OutError);
			return false;
		}
		TargetObject->PostEditChange();
		TargetObject->MarkPackageDirty();

		FString Exported;
		Property->ExportTextItem_Direct(Exported, ValuePtr, nullptr, TargetObject, PPF_None);
		OutResult = MakePropertyResult(TargetObject->GetPathName(), PropertyPath, TEXT("imported_and_read_back"), ImportText, Exported, TEXT(""));
		OutResult->SetStringField(TEXT("resolved_property_path"), ResolvedPropertyPath);
		RefreshObjectAfterPropertyImport(TargetObject, ResolvedPropertyPath, ImportText, OutResult);
		Exported.Reset();
		Property->ExportTextItem_Direct(Exported, ValuePtr, nullptr, TargetObject, PPF_None);
		OutResult->SetStringField(TEXT("property_value_read_back"), Exported);
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
