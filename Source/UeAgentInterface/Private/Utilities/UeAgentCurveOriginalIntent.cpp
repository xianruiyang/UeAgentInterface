#include "Utilities/UeAgentCurveOriginalIntent.h"

#include "Utilities/UeAgentCurveJson.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UeAgentCurveJson
{
	namespace
	{
		FString Child(const FString& Path, const FString& Key)
		{
			return Path + TEXT("/") + Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
		}

		bool IsNativeCurve(const TSharedPtr<FJsonObject>& Object)
		{
			FString Schema, Kind;
			return Object->TryGetStringField(TEXT("schema"), Schema) && Schema == SchemaName
				&& Object->TryGetStringField(TEXT("curve_kind"), Kind)
				&& (Kind == TEXT("float") || Kind == TEXT("vector") || Kind == TEXT("linear_color"));
		}

		bool Walk(const TSharedPtr<FJsonValue>& Value, const FString& Path,
			const TFunction<bool(const TSharedPtr<FJsonObject>&, const FString&)>& Visit)
		{
			if (!Value) return true;
			if (Value->Type == EJson::Object)
			{
				const auto Object = Value->AsObject();
				if (IsNativeCurve(Object)) return Visit(Object, Path);
				for (const auto& Field : Object->Values) if (!Walk(Field.Value, Child(Path, Field.Key), Visit)) return false;
			}
			else if (Value->Type == EJson::Array)
				for (int32 Index = 0; Index < Value->AsArray().Num(); ++Index)
					if (!Walk(Value->AsArray()[Index], Child(Path, FString::FromInt(Index)), Visit)) return false;
			return true;
		}
	}

	void CollectNativeCurveComparisonTypes(const TSharedPtr<FJsonValue>& Value,
		const FString& Path, TMap<FString, FString>& Types)
	{
		Walk(Value, Path, [&](const auto& Curve, const FString& CurvePath) {
			const TSharedPtr<FJsonObject>* Channels = nullptr;
			if (!Curve->TryGetObjectField(TEXT("channels"), Channels)) return true;
			for (const auto& Channel : (*Channels)->Values)
			{
				if (Channel.Value->Type != EJson::Object) continue;
				const auto Object = Channel.Value->AsObject();
				const FString ChannelPath = Child(Child(CurvePath, TEXT("channels")), Channel.Key);
				Types.Add(ChannelPath + TEXT("/default_value/value"), TEXT("float"));
				const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
				if (!Object->TryGetArrayField(TEXT("keys"), Keys)) continue;
				for (int32 Index = 0; Index < Keys->Num(); ++Index)
					for (const TCHAR* Field : {TEXT("time"), TEXT("time_seconds"), TEXT("value"), TEXT("arrive_tangent"),
						TEXT("leave_tangent"), TEXT("arrive_tangent_weight"), TEXT("leave_tangent_weight")})
						Types.Add(ChannelPath + TEXT("/keys/") + FString::FromInt(Index) + TEXT("/") + Field, TEXT("float"));
			}
			return true;
		});
	}

	bool ProjectOriginalCurveKeyOrder(TSharedPtr<FJsonValue>& Desired,
		TMap<FString, FString>& SourcePaths, UE::UAI::Contract::FDiagnostic& Error)
	{
		bool bCloned = false;
		// First locate a declared curve before paying to clone the document.
		Walk(Desired, FString(), [&](const auto&, const auto&) { bCloned = true; return false; });
		if (!bCloned) return true;
		Desired = UeAgentAuthoringState::CloneCanonicalValue(Desired);
		return Walk(Desired, FString(), [&](const auto& Curve, const FString& CurvePath) {
			const TSharedPtr<FJsonObject>* Channels = nullptr;
			if (!Curve->TryGetObjectField(TEXT("channels"), Channels)) return true;
			for (const auto& Channel : (*Channels)->Values)
			{
				if (Channel.Value->Type != EJson::Object) continue;
				const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
				if (!Channel.Value->AsObject()->TryGetArrayField(TEXT("keys"), Keys)) continue;
				struct FKey { int32 Source; float Time; TSharedPtr<FJsonValue> Value; };
				TArray<FKey> Sorted;
				const FString KeyPath = Child(Child(CurvePath, TEXT("channels")), Channel.Key) + TEXT("/keys/");
				for (int32 Index = 0; Index < Keys->Num(); ++Index)
				{
					const auto Value = (*Keys)[Index];
					const FString SourcePath = KeyPath + FString::FromInt(Index);
					double Time;
					float NativeTime;
					if (!Value || Value->Type != EJson::Object)
					{
						Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("curve_key_time_invalid"),
							TEXT("A curve key must be an object containing a numeric time or time_seconds field."), TEXT("original_intent"));
						Error.Source.JsonPath = SourcePath;
						Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
						Error.ActualValue = Value;
						Error.ActualValuePresent = Value.IsValid();
						return false;
					}
					const auto KeyObject = Value->AsObject();
					const bool bTime = KeyObject->TryGetNumberField(TEXT("time"), Time);
					if (!bTime && !KeyObject->TryGetNumberField(TEXT("time_seconds"), Time))
					{
						Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("curve_key_time_invalid"),
							TEXT("A curve key requires a numeric time or time_seconds field."), TEXT("original_intent"));
						Error.Source.JsonPath = SourcePath;
						Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("numeric time or time_seconds"));
						auto Times = MakeShared<FJsonObject>();
						for (const TCHAR* Field : {TEXT("time"), TEXT("time_seconds")})
						{
							if (const auto* InvalidTime = KeyObject->Values.Find(Field)) Times->SetField(Field, *InvalidTime);
						}
						Error.ActualValue = MakeShared<FJsonValueObject>(Times);
						return false;
					}
					if (!UeAgentPropertyValueCodec::TryConvertFloat32(Time, NativeTime, Error))
					{
						Error.Source.JsonPath = Child(SourcePath, bTime ? TEXT("time") : TEXT("time_seconds"));
						return false;
					}
					Sorted.Add({Index, NativeTime, Value});
				}
				// Same ordering as the shared rich/simple curve codec, not a new
				// asset-specific key policy or a tolerance-based identity merge.
			Sorted.StableSort([](const auto& A, const auto& B) { return A.Time < B.Time; });
				TArray<TSharedPtr<FJsonValue>> Values;
				for (const auto& Key : Sorted)
				{
					SourcePaths.Add(KeyPath + FString::FromInt(Values.Num()), KeyPath + FString::FromInt(Key.Source));
					Values.Add(Key.Value);
				}
				Channel.Value->AsObject()->SetArrayField(TEXT("keys"), MoveTemp(Values));
			}
			return true;
		});
	}
}
