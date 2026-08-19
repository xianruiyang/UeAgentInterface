#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"

#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::StateSemantic::CanonicalJson
{
	using UE::UAI::ContextResource::FUeAgentResourceTracker;

	namespace
	{
		TSharedRef<FJsonObject> TemporalObject(
			const FUeAgentTemporalPoint& Temporal)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("clock_id"), Temporal.GetClockId());
			Object->SetStringField(TEXT("tick"), Temporal.GetTick());
			Object->SetStringField(
				TEXT("rate_numerator"), Temporal.GetRateNumerator());
			Object->SetStringField(
				TEXT("rate_denominator"), Temporal.GetRateDenominator());
			Object->SetStringField(TEXT("state"), Temporal.GetState());
			return Object;
		}

		TArray<TSharedPtr<FJsonValue>> StringArray(
			const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			Result.Reserve(Values.Num());
			for (const FString& Value : Values)
			{
				Result.Add(MakeShared<FJsonValueString>(Value));
			}
			return Result;
		}

		const TCHAR* SpatialQuantityText(
			const EUeAgentSpatialQuantityKind Kind)
		{
			switch (Kind)
			{
			case EUeAgentSpatialQuantityKind::Point:
				return TEXT("point");
			case EUeAgentSpatialQuantityKind::Direction:
				return TEXT("direction");
			case EUeAgentSpatialQuantityKind::Normal:
				return TEXT("normal");
			case EUeAgentSpatialQuantityKind::Transform:
				return TEXT("transform");
			default:
				return TEXT("unknown");
			}
		}

		TSharedRef<FJsonObject> SpatialObject(
			const FUeAgentSpatialValue& Spatial)
		{
			const FUeAgentSpatialFrame& Frame = Spatial.GetFrame();
			const TSharedRef<FJsonObject> FrameObject = MakeShared<FJsonObject>();
			FrameObject->SetStringField(TEXT("frame_id"), Frame.GetFrameId());
			FrameObject->SetStringField(
				TEXT("basis_frame_id"), Frame.GetBasisFrameId());
			FrameObject->SetNumberField(TEXT("dimension"), Frame.GetDimension());
			TArray<TSharedPtr<FJsonValue>> Basis;
			Basis.Reserve(Frame.GetBasis().Num());
			for (const TArray<FString>& Row : Frame.GetBasis())
			{
				Basis.Add(MakeShared<FJsonValueArray>(StringArray(Row)));
			}
			FrameObject->SetArrayField(TEXT("basis"), MoveTemp(Basis));
			FrameObject->SetArrayField(TEXT("origin"), StringArray(Frame.GetOrigin()));
			FrameObject->SetStringField(TEXT("unit"), Frame.GetUnit());
			FrameObject->SetStringField(
				TEXT("basis_evidence"), Frame.GetBasisEvidence());
			FrameObject->SetObjectField(
				TEXT("observed_at"), TemporalObject(Frame.GetObservedAt()));

			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("subject"), Spatial.GetSubject());
			Object->SetStringField(
				TEXT("quantity_kind"),
				SpatialQuantityText(Spatial.GetQuantityKind()));
			Object->SetObjectField(TEXT("frame"), FrameObject);
			Object->SetArrayField(
				TEXT("components"), StringArray(Spatial.GetComponents()));
			Object->SetObjectField(
				TEXT("observed_at"), TemporalObject(Spatial.GetObservedAt()));
			Object->SetArrayField(
				TEXT("conversion_evidence"),
				StringArray(Spatial.GetConversionEvidence()));
			return Object;
		}
	}

	bool TryFromJsonValue(
		const TSharedPtr<FJsonValue>& Value,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FString& OutError)
	{
		struct FVisit final
		{
			TSharedPtr<FJsonValue> Value;
			bool bExpanded = false;
		};

		OutValue.Reset();
		OutError.Reset();
		if (!Value.IsValid())
		{
			OutError = TEXT("canonical_json_value_missing");
			return false;
		}

		TArray<FVisit> Work;
		TMap<const FJsonValue*, FUeAgentCanonicalValuePtr> Built;
		TSet<const FJsonValue*> Expanded;
		Work.Add({Value, false});
		while (!Work.IsEmpty())
		{
			const FVisit Current = Work.Pop(EAllowShrinking::No);
			if (!Current.Value.IsValid())
			{
				OutError = TEXT("canonical_json_value_missing");
				return false;
			}
			if (!Current.bExpanded
				&& (Current.Value->Type == EJson::Array
					|| Current.Value->Type == EJson::Object))
			{
				if (Expanded.Contains(Current.Value.Get()))
				{
					continue;
				}
				Expanded.Add(Current.Value.Get());
				Work.Add({Current.Value, true});
				if (Current.Value->Type == EJson::Array)
				{
					const TArray<TSharedPtr<FJsonValue>>& Items =
						Current.Value->AsArray();
					for (int32 Index = Items.Num() - 1; Index >= 0; --Index)
					{
						Work.Add({Items[Index], false});
					}
				}
				else
				{
					const TSharedPtr<FJsonObject> Object =
						Current.Value->AsObject();
					if (!Object.IsValid())
					{
						OutError = TEXT("canonical_json_object_invalid");
						return false;
					}
					TArray<FString> Keys;
					Object->Values.GetKeys(Keys);
					Keys.Sort();
					for (int32 Index = Keys.Num() - 1; Index >= 0; --Index)
					{
						Work.Add({Object->Values.FindChecked(Keys[Index]), false});
					}
				}
				continue;
			}

			FUeAgentCanonicalValuePtr Canonical;
			FUeAgentStateSemanticError SemanticError;
			bool bCreated = false;
			switch (Current.Value->Type)
			{
			case EJson::Null:
			case EJson::None:
				bCreated = FUeAgentCanonicalValue::TryNull(
					Resources, Canonical, SemanticError);
				break;
			case EJson::Boolean:
				bCreated = FUeAgentCanonicalValue::TryBoolean(
					Current.Value->AsBool(), Resources, Canonical, SemanticError);
				break;
			case EJson::String:
				bCreated = FUeAgentCanonicalValue::TryString(
					Current.Value->AsString(), Resources, Canonical, SemanticError);
				break;
			case EJson::Number:
				bCreated = FUeAgentCanonicalValue::TryDecimal(
					FString::SanitizeFloat(Current.Value->AsNumber()),
					Resources, Canonical, SemanticError);
				break;
			case EJson::Array:
			{
				TArray<FUeAgentCanonicalValueRef> Items;
				for (const TSharedPtr<FJsonValue>& Item : Current.Value->AsArray())
				{
					const FUeAgentCanonicalValuePtr* Found = Built.Find(Item.Get());
					if (!Found || !Found->IsValid())
					{
						OutError = TEXT("canonical_json_array_child_missing");
						return false;
					}
					Items.Add(Found->ToSharedRef());
				}
				bCreated = FUeAgentCanonicalValue::TryArray(
					Items, Resources, Canonical, SemanticError);
				break;
			}
			case EJson::Object:
			{
				const TSharedPtr<FJsonObject> Object = Current.Value->AsObject();
				if (!Object.IsValid())
				{
					OutError = TEXT("canonical_json_object_invalid");
					return false;
				}
				TArray<FString> Keys;
				Object->Values.GetKeys(Keys);
				Keys.Sort();
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
				for (const FString& Key : Keys)
				{
					const TSharedPtr<FJsonValue>& Child =
						Object->Values.FindChecked(Key);
					const FUeAgentCanonicalValuePtr* Found = Built.Find(Child.Get());
					if (!Found || !Found->IsValid())
					{
						OutError = TEXT("canonical_json_object_child_missing");
						return false;
					}
					Items.Emplace(Key, Found->ToSharedRef());
				}
				bCreated = FUeAgentCanonicalValue::TryObject(
					Items, Resources, Canonical, SemanticError);
				break;
			}
			default:
				OutError = TEXT("canonical_json_kind_unsupported");
				return false;
			}
			if (!bCreated || !Canonical.IsValid())
			{
				OutError = SemanticError.Code.IsEmpty()
					? TEXT("canonical_json_value_create_failed")
					: SemanticError.Code;
				return false;
			}
			Built.Add(Current.Value.Get(), Canonical);
		}

		const FUeAgentCanonicalValuePtr* Root = Built.Find(Value.Get());
		if (!Root || !Root->IsValid())
		{
			OutError = TEXT("canonical_json_root_missing");
			return false;
		}
		OutValue = *Root;
		return true;
	}

	bool TryToJsonValue(
		const FUeAgentCanonicalValue& Value,
		TSharedPtr<FJsonValue>& OutValue,
		FString& OutError)
	{
		struct FVisit final
		{
			const FUeAgentCanonicalValue* Value = nullptr;
			bool bExpanded = false;
		};

		OutValue.Reset();
		OutError.Reset();
		TArray<FVisit> Work;
		TMap<const FUeAgentCanonicalValue*, TSharedPtr<FJsonValue>> Built;
		Work.Add({&Value, false});
		while (!Work.IsEmpty())
		{
			const FVisit Current = Work.Pop(EAllowShrinking::No);
			if (!Current.Value)
			{
				OutError = TEXT("canonical_json_value_missing");
				return false;
			}
			if (!Current.bExpanded
				&& (Current.Value->GetKind() == EUeAgentValueKind::Array
					|| Current.Value->GetKind() == EUeAgentValueKind::Object))
			{
				Work.Add({Current.Value, true});
				if (Current.Value->GetKind() == EUeAgentValueKind::Array)
				{
					const auto& Items = Current.Value->GetArrayItems();
					for (int32 Index = Items.Num() - 1; Index >= 0; --Index)
					{
						Work.Add({&Items[Index].Get(), false});
					}
				}
				else
				{
					const auto& Items = Current.Value->GetObjectItems();
					for (int32 Index = Items.Num() - 1; Index >= 0; --Index)
					{
						Work.Add({&Items[Index].Value.Get(), false});
					}
				}
				continue;
			}

			TSharedPtr<FJsonValue> JsonValue;
			switch (Current.Value->GetKind())
			{
			case EUeAgentValueKind::Null:
				JsonValue = MakeShared<FJsonValueNull>();
				break;
			case EUeAgentValueKind::Boolean:
				JsonValue = MakeShared<FJsonValueBoolean>(
					Current.Value->GetBoolean());
				break;
			case EUeAgentValueKind::String:
				JsonValue = MakeShared<FJsonValueString>(Current.Value->GetText());
				break;
			case EUeAgentValueKind::Integer:
			case EUeAgentValueKind::Decimal:
				JsonValue = MakeShared<FJsonValueNumberString>(
					Current.Value->GetLexeme());
				break;
			case EUeAgentValueKind::Array:
			{
				TArray<TSharedPtr<FJsonValue>> Items;
				Items.Reserve(Current.Value->GetArrayItems().Num());
				for (const FUeAgentCanonicalValueRef& Item
					: Current.Value->GetArrayItems())
				{
					const TSharedPtr<FJsonValue>* Found = Built.Find(&Item.Get());
					if (!Found || !Found->IsValid())
					{
						OutError = TEXT("canonical_json_array_child_missing");
						return false;
					}
					Items.Add(*Found);
				}
				JsonValue = MakeShared<FJsonValueArray>(MoveTemp(Items));
				break;
			}
			case EUeAgentValueKind::Object:
			{
				const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
				for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
					: Current.Value->GetObjectItems())
				{
					const TSharedPtr<FJsonValue>* Found = Built.Find(&Item.Value.Get());
					if (!Found || !Found->IsValid())
					{
						OutError = TEXT("canonical_json_object_child_missing");
						return false;
					}
					Object->SetField(Item.Key, *Found);
				}
				JsonValue = MakeShared<FJsonValueObject>(Object);
				break;
			}
			case EUeAgentValueKind::Temporal:
				if (!Current.Value->GetTemporal().IsValid())
				{
					OutError = TEXT("canonical_json_temporal_invalid");
					return false;
				}
				JsonValue = MakeShared<FJsonValueObject>(
					TemporalObject(*Current.Value->GetTemporal()));
				break;
			case EUeAgentValueKind::Spatial:
				if (!Current.Value->GetSpatial().IsValid())
				{
					OutError = TEXT("canonical_json_spatial_invalid");
					return false;
				}
				JsonValue = MakeShared<FJsonValueObject>(
					SpatialObject(*Current.Value->GetSpatial()));
				break;
			default:
				OutError = TEXT("canonical_json_kind_unsupported");
				return false;
			}
			Built.Add(Current.Value, MoveTemp(JsonValue));
		}

		const TSharedPtr<FJsonValue>* Root = Built.Find(&Value);
		if (!Root || !Root->IsValid())
		{
			OutError = TEXT("canonical_json_root_missing");
			return false;
		}
		OutValue = *Root;
		return true;
	}
}
