#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"

#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::StateSemantic::CanonicalJson
{
	using UE::UAI::ContextResource::FUeAgentResourceTracker;

	bool TryGetNumberLexeme(const FJsonValue& Value, FString& OutLexeme)
	{
		if (Value.Type != EJson::Number) return false;
		if (Value.PreferStringRepresentation()) return Value.TryGetString(OutLexeme);
		const double Number = Value.AsNumber();
		if (!FMath::IsFinite(Number)) return false;
		// Match the JSON writer's round-trip precision; SanitizeFloat uses %f.
		OutLexeme = FString::Printf(TEXT("%.17g"), Number == 0.0 ? 0.0 : Number);
		return true;
	}

	namespace
	{
		FString ChildPath(const FString& Parent, const FString& Field) {
			return (Parent == TEXT("/") ? FString() : Parent) + TEXT("/") + Field.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
		}
		Contract::FDiagnostic ConversionFailure(const TCHAR* Code, const TCHAR* Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual = nullptr, TSharedPtr<FJsonValue> Expected = nullptr) {
			auto Error = Contract::FDiagnostic::Failure(Code, Reason, TEXT("canonical_json"));
			Error.Source.JsonPath = Path; Error.SemanticPointer = Path;
			Error.ActualValue = MoveTemp(Actual); Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}
		// Canonical factories report their input under /value. Bind that logical root
		// to the exact node being converted without replacing the producer's facts.
		void LocateConversionFailure(Contract::FDiagnostic& Error, const FString& Path) {
			if (Error.Source.SourceFile.IsEmpty()) {
				auto Bind = [&](FString& Relative) {
					if (Relative == TEXT("/value") || Relative == TEXT("/")) Relative.Reset();
					else if (Relative.StartsWith(TEXT("/value/"))) Relative.RightChopInline(6);
					Relative = (Path == TEXT("/") ? FString() : Path) + Relative;
					if (Relative.IsEmpty()) Relative = TEXT("/");
				};
				Bind(Error.Source.JsonPath);
				if (!Error.Source.InternalJsonPath.IsEmpty()) Bind(Error.Source.InternalJsonPath);
				if (Error.SemanticPointer.IsEmpty()) Error.SemanticPointer = Error.Source.JsonPath;
				else Bind(Error.SemanticPointer);
			}
			for (auto& Child : Error.Diagnostics) LocateConversionFailure(Child, Path);
			if (Error.PublicCause) {
				auto Cause = MakeShared<Contract::FDiagnostic>(*Error.PublicCause);
				LocateConversionFailure(*Cause, Path); Error.PublicCause = MoveTemp(Cause);
			}
		}
		class FTypedProjectionPass
		{
		public:
			FTypedProjectionPass(bool bInPreflight, TFunctionRef<bool(int64)> InCharge)
				: bPreflight(bInPreflight), Charge(InCharge) {}
			bool bPreflight;
			bool bOk = true;
			int64 RejectedBytes = 0;
			TFunctionRef<bool(int64)> Charge;
			void Reserve(int64 Bytes) { if (bPreflight && bOk && !Charge(Bytes)) { bOk = false; RejectedBytes = Bytes; } }
			TSharedPtr<FJsonObject> Object()
			{
				Reserve(512);
				if (bPreflight) return nullptr;
				return MakeShared<FJsonObject>();
			}
			void String(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, const FString& Value)
			{
				Reserve(512 + 2ll * (FCString::Strlen(Key) + static_cast<int64>(Value.Len()) + 2) * sizeof(TCHAR));
				if (!bPreflight) Object->SetStringField(Key, Value);
			}
			void Number(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, double Value)
			{
				Reserve(512 + 2ll * (FCString::Strlen(Key) + 1) * sizeof(TCHAR));
				if (!bPreflight) Object->SetNumberField(Key, Value);
			}
			void ObjectField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, const TSharedPtr<FJsonObject>& Value)
			{
				Reserve(512 + 2ll * (FCString::Strlen(Key) + 1) * sizeof(TCHAR));
				if (!bPreflight) Object->SetObjectField(Key, Value);
			}
			void ArrayField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, TArray<TSharedPtr<FJsonValue>> Value)
			{
				Reserve(512 + 2ll * (FCString::Strlen(Key) + 1) * sizeof(TCHAR));
				if (!bPreflight) Object->SetArrayField(Key, MoveTemp(Value));
			}
		};

		TSharedPtr<FJsonObject> TemporalObject(
			const FUeAgentTemporalPoint& Temporal, FTypedProjectionPass& Pass)
		{
			const auto Object = Pass.Object();
			Pass.String(Object, TEXT("clock_id"), Temporal.GetClockId());
			Pass.String(Object, TEXT("tick"), Temporal.GetTick());
			Pass.String(Object,
				TEXT("rate_numerator"), Temporal.GetRateNumerator());
			Pass.String(Object,
				TEXT("rate_denominator"), Temporal.GetRateDenominator());
			Pass.String(Object, TEXT("state"), Temporal.GetState());
			return Object;
		}

		TArray<TSharedPtr<FJsonValue>> StringArray(
			const TArray<FString>& Values, FTypedProjectionPass& Pass)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			Pass.Reserve(128 + 2ll * Values.Num() * sizeof(TSharedPtr<FJsonValue>));
			if (!Pass.bPreflight) Result.Reserve(Values.Num());
			for (const FString& Value : Values)
			{
				if (!Pass.bOk) break;
				Pass.Reserve(256 + 2ll * (static_cast<int64>(Value.Len()) + 1) * sizeof(TCHAR));
				if (!Pass.bPreflight) Result.Add(MakeShared<FJsonValueString>(Value));
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

		TSharedPtr<FJsonObject> SpatialObject(
			const FUeAgentSpatialValue& Spatial, FTypedProjectionPass& Pass)
		{
			const FUeAgentSpatialFrame& Frame = Spatial.GetFrame();
			const auto FrameObject = Pass.Object();
			Pass.String(FrameObject, TEXT("frame_id"), Frame.GetFrameId());
			Pass.String(FrameObject,
				TEXT("basis_frame_id"), Frame.GetBasisFrameId());
			Pass.Number(FrameObject, TEXT("dimension"), Frame.GetDimension());
			TArray<TSharedPtr<FJsonValue>> Basis;
			Pass.Reserve(128 + 2ll * Frame.GetBasis().Num() * sizeof(TSharedPtr<FJsonValue>));
			if (!Pass.bPreflight) Basis.Reserve(Frame.GetBasis().Num());
			for (const TArray<FString>& Row : Frame.GetBasis())
			{
				if (!Pass.bOk) break;
				auto Values = StringArray(Row, Pass);
				if (!Pass.bPreflight) Basis.Add(MakeShared<FJsonValueArray>(MoveTemp(Values)));
			}
			Pass.ArrayField(FrameObject, TEXT("basis"), MoveTemp(Basis));
			Pass.ArrayField(FrameObject, TEXT("origin"), StringArray(Frame.GetOrigin(), Pass));
			Pass.String(FrameObject, TEXT("unit"), Frame.GetUnit());
			Pass.String(FrameObject,
				TEXT("basis_evidence"), Frame.GetBasisEvidence());
			Pass.ObjectField(FrameObject,
				TEXT("observed_at"), TemporalObject(Frame.GetObservedAt(), Pass));

			const auto Object = Pass.Object();
			Pass.String(Object, TEXT("subject"), Spatial.GetSubject());
			Pass.String(Object,
				TEXT("quantity_kind"),
				SpatialQuantityText(Spatial.GetQuantityKind()));
			Pass.ObjectField(Object, TEXT("frame"), FrameObject);
			Pass.ArrayField(Object,
				TEXT("components"), StringArray(Spatial.GetComponents(), Pass));
			Pass.ObjectField(Object,
				TEXT("observed_at"), TemporalObject(Spatial.GetObservedAt(), Pass));
			Pass.ArrayField(Object,
				TEXT("conversion_evidence"),
				StringArray(Spatial.GetConversionEvidence(), Pass));
			return Object;
		}
	}

	bool AreNumbersEquivalent(const FJsonValue& Left, const FJsonValue& Right)
	{
		if (Left.Type != EJson::Number || Right.Type != EJson::Number) return false;
		if (!Left.PreferStringRepresentation() && !Right.PreferStringRepresentation())
			return FMath::IsFinite(Left.AsNumber()) && FMath::IsFinite(Right.AsNumber())
				&& Left.AsNumber() == Right.AsNumber();
		FString LeftLexeme;
		FString RightLexeme;
		return TryGetNumberLexeme(Left, LeftLexeme) && TryGetNumberLexeme(Right, RightLexeme)
			&& AreNumericLexemesEquivalent(LeftLexeme, RightLexeme);
	}

	FValueComparison FValueComparison::Different(FString Path,
		TSharedPtr<FJsonValue> Expected, TSharedPtr<FJsonValue> Actual)
	{
		FValueComparison Result;
		Result.bEquivalent = false;
		Result.Path = MoveTemp(Path);
		Result.ExpectedValue = MoveTemp(Expected);
		Result.ActualValue = MoveTemp(Actual);
		return Result;
	}

	FValueComparison FValueComparison::Failed(UE::UAI::Contract::FDiagnostic Diagnostic)
	{
		FValueComparison Result;
		Result.bEquivalent = false;
		Result.Path = Diagnostic.Source.JsonPath.IsEmpty() ? Diagnostic.SemanticPointer : Diagnostic.Source.JsonPath;
		Result.ComparisonFailure = MakeShared<UE::UAI::Contract::FDiagnostic>(MoveTemp(Diagnostic));
		return Result;
	}

	FValueComparison FValueComparison::WithPrefix(const FString& Prefix) const
	{
		FValueComparison Result = *this;
		if (!Result.bEquivalent) Result.Path = Prefix + Result.Path;
		if (Result.ComparisonFailure && !Result.ComparisonFailure->SemanticPointer.IsEmpty()) {
			auto Failure = MakeShared<UE::UAI::Contract::FDiagnostic>(*Result.ComparisonFailure);
			Failure->SemanticPointer = Prefix + Failure->SemanticPointer;
			Result.ComparisonFailure = MoveTemp(Failure);
		}
		return Result;
	}

	void FValueComparison::ApplyTo(UE::UAI::Contract::FDiagnostic& Diagnostic) const
	{
		if (bEquivalent) return;
		if (ComparisonFailure)
		{
			Diagnostic = *ComparisonFailure;
			Diagnostic.Source.JsonPath = Path;
			return;
		}
		Diagnostic.Source.JsonPath = Path;
		Diagnostic.ExpectedValue = ExpectedValue;
		Diagnostic.ActualValue = ActualValue;
		Diagnostic.ExpectedValuePresent = ExpectedValue.IsValid();
		Diagnostic.ActualValuePresent = ActualValue.IsValid();
	}

	bool AreJsonValuesEquivalent(const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right)
	{
		return CompareJsonValues(Left, Right).bEquivalent;
	}

	FValueComparison CompareJsonValues(
		const TSharedPtr<FJsonValue>& Left,
		const TSharedPtr<FJsonValue>& Right,
		const FString& RootPath,
		const FDeclaredValueComparison* DeclaredComparison)
	{
		return CompareJsonValuesWithCase(Left, Right, ESearchCase::CaseSensitive, RootPath, DeclaredComparison);
	}

	FValueComparison CompareJsonValuesWithCase(
		const TSharedPtr<FJsonValue>& Left,
		const TSharedPtr<FJsonValue>& Right,
		const ESearchCase::Type CaseSensitivity,
		const FString& RootPath,
		const FDeclaredValueComparison* DeclaredComparison)
	{
		struct FJsonCompareNode
		{
			TSharedPtr<FJsonValue> Left;
			TSharedPtr<FJsonValue> Right;
			FString Path;
		};
		auto SetDifference = [](const FJsonCompareNode& Node)
		{
			return FValueComparison::Different(Node.Path, Node.Left, Node.Right);
		};
		TArray<FJsonCompareNode> Pending = {{Left, Right, RootPath}};
		while (!Pending.IsEmpty())
		{
			FJsonCompareNode Node = Pending.Pop(EAllowShrinking::No);
			if (!Node.Left.IsValid() || !Node.Right.IsValid())
			{
				return SetDifference(Node);
			}
			if (DeclaredComparison)
			{
				const auto Compared = (*DeclaredComparison)(Node.Left, Node.Right, Node.Path);
				if (Compared.IsSet())
				{
					if (!Compared->bEquivalent) return Compared->WithPrefix(Node.Path);
					continue;
				}
			}
			if (Node.Left->Type != Node.Right->Type) return SetDifference(Node);
			switch (Node.Left->Type)
			{
			case EJson::String:
				if (!Node.Left->AsString().Equals(Node.Right->AsString(), CaseSensitivity)) return SetDifference(Node);
				break;
			case EJson::Number:
				// An untyped document number may be an integer, index or identity.
				// Tolerances belong to the native/declared type, not arbitrary JSON.
				if (!AreNumbersEquivalent(*Node.Left, *Node.Right)) return SetDifference(Node);
				break;
			case EJson::Boolean:
				if (Node.Left->AsBool() != Node.Right->AsBool()) return SetDifference(Node);
				break;
			case EJson::Array:
			{
				const TArray<TSharedPtr<FJsonValue>>& LeftItems = Node.Left->AsArray();
				const TArray<TSharedPtr<FJsonValue>>& RightItems = Node.Right->AsArray();
				if (LeftItems.Num() != RightItems.Num())
				{
					const int32 Index = FMath::Min(LeftItems.Num(), RightItems.Num());
					return FValueComparison::Different(Node.Path + TEXT("/") + LexToString(Index),
						LeftItems.IsValidIndex(Index) ? LeftItems[Index] : nullptr,
						RightItems.IsValidIndex(Index) ? RightItems[Index] : nullptr);
				}
				for (int32 Index = LeftItems.Num() - 1; Index >= 0; --Index)
				{
					Pending.Add({
						LeftItems[Index], RightItems[Index],
						Node.Path + TEXT("/") + LexToString(Index)});
				}
				break;
			}
			case EJson::Object:
			{
				const TSharedPtr<FJsonObject> LeftObject = Node.Left->AsObject();
				const TSharedPtr<FJsonObject> RightObject = Node.Right->AsObject();
				if (!LeftObject.IsValid() || !RightObject.IsValid()) return SetDifference(Node);
				TArray<FString> LeftKeys;
				TArray<FString> RightKeys;
				LeftObject->Values.GenerateKeyArray(LeftKeys);
				RightObject->Values.GenerateKeyArray(RightKeys);
				const auto KeyLess = [CaseSensitivity](const FString& A, const FString& B)
				{
					return A.Compare(B, CaseSensitivity) < 0;
				};
				LeftKeys.Sort(KeyLess);
				RightKeys.Sort(KeyLess);
				int32 LeftIndex = 0;
				int32 RightIndex = 0;
				while (LeftIndex < LeftKeys.Num() || RightIndex < RightKeys.Num())
				{
					const int32 Order = LeftIndex == LeftKeys.Num() ? 1
						: RightIndex == RightKeys.Num() ? -1
						: LeftKeys[LeftIndex].Compare(RightKeys[RightIndex], CaseSensitivity);
					const FString& Key = Order < 0 ? LeftKeys[LeftIndex] : RightKeys[RightIndex];
					Pending.Add({
						Order <= 0 ? LeftObject->TryGetField(LeftKeys[LeftIndex]) : nullptr,
						Order >= 0 ? RightObject->TryGetField(RightKeys[RightIndex]) : nullptr,
						Node.Path + TEXT("/") + Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"))});
					if (Order <= 0) ++LeftIndex;
					if (Order >= 0) ++RightIndex;
				}
				break;
			}
			case EJson::Null:
				break;
			default:
				return SetDifference(Node);
			}
		}
		return FValueComparison();
	}

	bool TryProjectTypedLeaf(const FUeAgentCanonicalValue& Value,
		TFunctionRef<bool(int64)> ChargeMemory, TSharedPtr<FJsonValue>& OutValue, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutValue.Reset(); OutError.Reset();
		const bool bTemporal = Value.GetKind() == EUeAgentValueKind::Temporal;
		if ((bTemporal && (!Value.GetTemporal() || !Value.GetTemporal()->IsValid()))
			|| (!bTemporal && (Value.GetKind() != EUeAgentValueKind::Spatial || !Value.GetSpatial() || !Value.GetSpatial()->IsValid())))
		{
			if (bTemporal && Value.GetTemporal()) OutError = Value.GetTemporal()->GetValidationDiagnostic();
			else if (Value.GetKind() == EUeAgentValueKind::Spatial && Value.GetSpatial()) OutError = Value.GetSpatial()->GetValidationDiagnostic();
			else OutError = ConversionFailure(TEXT("canonical_json_typed_leaf_invalid"), TEXT("Typed JSON projection requires a present temporal or spatial value."), TEXT("/"),
				MakeShared<FJsonValueNumber>(static_cast<int32>(Value.GetKind())), MakeShared<FJsonValueString>(TEXT("temporal or spatial value")));
			return false;
		}
		FTypedProjectionPass Preflight(true, ChargeMemory);
		Preflight.Reserve(128);
		if (bTemporal) TemporalObject(*Value.GetTemporal(), Preflight); else SpatialObject(*Value.GetSpatial(), Preflight);
		if (!Preflight.bOk) {
			OutError = ConversionFailure(TEXT("canonical_json_typed_leaf_accounting_failed"), TEXT("Typed JSON projection could not reserve the requested allocation during preflight."), TEXT("/"),
				MakeShared<FJsonValueNumber>(Preflight.RejectedBytes), MakeShared<FJsonValueString>(TEXT("accepted allocation in bytes")));
			return false;
		}
		FTypedProjectionPass Build(false, ChargeMemory);
		OutValue = MakeShared<FJsonValueObject>(bTemporal ? TemporalObject(*Value.GetTemporal(), Build) : SpatialObject(*Value.GetSpatial(), Build));
		return true;
	}

	bool MatchesCanonicalProjection(const TSharedPtr<FJsonValue>& Current, const FUeAgentCanonicalValue& Previous)
	{
		using FPair = TPair<const FJsonValue*, const FUeAgentCanonicalValue*>;
		TArray<FPair> Work;
		TMap<const FJsonValue*, const FUeAgentCanonicalValue*> Compared;
		Work.Emplace(Current.Get(), &Previous);
		while (!Work.IsEmpty()) {
			const FPair Pair = Work.Pop(EAllowShrinking::No);
			if (!Pair.Key || !Pair.Value) return false;
			if (const auto* Prior = Compared.Find(Pair.Key); Prior && *Prior == Pair.Value) continue;
			Compared.Add(Pair.Key, Pair.Value);
			const auto& Json = *Pair.Key;
			const auto& Canonical = *Pair.Value;
			switch (Json.Type) {
			case EJson::None:
			case EJson::Null:
				if (Canonical.GetKind() != EUeAgentValueKind::Null) return false;
				break;
			case EJson::Boolean:
				if (Canonical.GetKind() != EUeAgentValueKind::Boolean || Json.AsBool() != Canonical.GetBoolean()) return false;
				break;
			case EJson::String:
				if (Canonical.GetKind() != EUeAgentValueKind::String || !Json.AsString().Equals(Canonical.GetText(), ESearchCase::CaseSensitive)) return false;
				break;
			case EJson::Number: {
				// TryFromJsonValue uses Decimal for native doubles and first tries
				// Integer for exact number strings. The uncommon decimal-string
				// case retains ordinary conversion rather than guessing its kind.
				const auto Expected = Json.PreferStringRepresentation() ? EUeAgentValueKind::Integer : EUeAgentValueKind::Decimal;
				FString Lexeme;
				if (Canonical.GetKind() != Expected || !TryGetNumberLexeme(Json, Lexeme)
					|| !Lexeme.Equals(Canonical.GetText(), ESearchCase::CaseSensitive)) return false;
				break;
			}
			case EJson::Array: {
				if (Canonical.GetKind() != EUeAgentValueKind::Array || Json.AsArray().Num() != Canonical.GetArrayItems().Num()) return false;
				for (int32 Index = 0; Index < Json.AsArray().Num(); ++Index)
					Work.Emplace(Json.AsArray()[Index].Get(), &Canonical.GetArrayItems()[Index].Get());
				break;
			}
			case EJson::Object: {
				const auto Object = Json.AsObject();
				if (Canonical.GetKind() != EUeAgentValueKind::Object || !Object
					|| Object->Values.Num() != Canonical.GetObjectItems().Num()) return false;
				TArray<FString> Keys; Object->Values.GetKeys(Keys);
				Keys.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
				for (int32 Index = 0; Index < Keys.Num(); ++Index) {
					const auto& Item = Canonical.GetObjectItems()[Index];
					if (!Keys[Index].Equals(Item.Key, ESearchCase::CaseSensitive)) return false;
					Work.Emplace(Object->Values.FindChecked(Keys[Index]).Get(), &Item.Value.Get());
				}
				break;
			}
			default: return false;
			}
		}
		return true;
	}

	static bool TryFromJsonScalar(const FJsonValue& Value, FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue, UE::UAI::Contract::FDiagnostic& OutError, const FString& Path)
	{
		FUeAgentStateSemanticError SemanticError; bool bCreated = false;
		switch (Value.Type) {
			case EJson::Null:
			case EJson::None:
				bCreated = FUeAgentCanonicalValue::TryNull(
					Resources, OutValue, SemanticError);
				break;
			case EJson::Boolean:
				bCreated = FUeAgentCanonicalValue::TryBoolean(
					Value.AsBool(), Resources, OutValue, SemanticError);
				break;
			case EJson::String:
				bCreated = FUeAgentCanonicalValue::TryString(
					Value.AsString(), Resources, OutValue, SemanticError);
				break;
			case EJson::Number:
			{
				FString Lexeme;
				if (!TryGetNumberLexeme(Value, Lexeme))
				{
					OutError = ConversionFailure(TEXT("canonical_json_number_lexeme_invalid"), TEXT("The JSON number cannot provide a finite exact numeric spelling."), Path,
						Value.PreferStringRepresentation() ? nullptr : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(FString::Printf(TEXT("%.17g"), Value.AsNumber()))),
						MakeShared<FJsonValueString>(TEXT("finite numeric spelling")));
					return false;
				}
				if (Value.PreferStringRepresentation())
				{
					// Use the canonical numeric grammar rather than a double or a
					// second HJSON parse. Invalid integer grammar allocates nothing;
					// allocation failures must not be retried as another value kind.
					bCreated = FUeAgentCanonicalValue::TryInteger(Lexeme,
						Resources, OutValue, SemanticError);
					if (!bCreated && SemanticError.Code == TEXT("integer_lexeme_not_canonical"))
						bCreated = FUeAgentCanonicalValue::TryDecimal(Lexeme,
							Resources, OutValue, SemanticError);
					break;
				}
				bCreated = FUeAgentCanonicalValue::TryDecimal(
					Lexeme,
					Resources, OutValue, SemanticError);
				break;
			}
		default:
			OutError = ConversionFailure(TEXT("canonical_json_kind_unsupported"), TEXT("This JSON value kind is not supported by canonical conversion."), Path,
				MakeShared<FJsonValueNumber>(static_cast<int32>(Value.Type))); return false;
		}
		if (!bCreated || !OutValue.IsValid()) {
			OutError = SemanticError.IsEmpty()
				? ConversionFailure(TEXT("canonical_json_value_create_failed"), TEXT("The canonical value factory returned no value or diagnostic."), TEXT("/"), MakeShared<FJsonValueNumber>(static_cast<int32>(Value.Type)))
				: SemanticError;
			LocateConversionFailure(OutError, Path); return false;
		}
		return true;
	}

	bool TryFromJsonScalarValue(const FJsonValue& Value, FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutValue.Reset(); OutError.Reset();
		return TryFromJsonScalar(Value, Resources, OutValue, OutError, TEXT("/"));
	}

	bool TryFromJsonValue(
		const TSharedPtr<FJsonValue>& Value,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		struct FVisit final
		{
			const FJsonValue* Value = nullptr;
			bool bExpanded = false;
			FString Path;
			TArray<FString> Keys;
		};

		OutValue.Reset();
		OutError.Reset();
		if (!Value.IsValid())
		{
			OutError = ConversionFailure(TEXT("canonical_json_value_missing"), TEXT("The input JSON value is missing."), TEXT("/"));
			return false;
		}

		if (Value->Type != EJson::Array && Value->Type != EJson::Object)
			return TryFromJsonScalar(*Value, Resources, OutValue, OutError, TEXT("/"));

		TArray<FVisit> Work;
		struct FConverted { FUeAgentCanonicalValuePtr Value; bool bExpanded = false; };
		TMap<const FJsonValue*, FConverted> Built;
		Work.Add({Value.Get(), false, TEXT("/")});
		while (!Work.IsEmpty())
		{
			FVisit Current = Work.Pop(EAllowShrinking::No);
			if (!Current.Value)
			{
				OutError = ConversionFailure(TEXT("canonical_json_value_missing"), TEXT("The value at this conversion path is missing."), Current.Path);
				return false;
			}
			if (!Current.bExpanded
				&& (Current.Value->Type == EJson::Array
					|| Current.Value->Type == EJson::Object))
			{
				auto& Converted = Built.FindOrAdd(Current.Value);
				if (Converted.bExpanded) continue;
				Converted.bExpanded = true;
				if (Current.Value->Type == EJson::Array)
				{
					const TArray<TSharedPtr<FJsonValue>>& Items = Current.Value->AsArray();
					Work.Add({Current.Value, true, Current.Path});
					for (int32 Index = Items.Num() - 1; Index >= 0; --Index)
					{
						Work.Add({Items[Index].Get(), false, ChildPath(Current.Path, FString::FromInt(Index))});
					}
				}
				else
				{
					const TSharedPtr<FJsonObject>& Object =
						Current.Value->AsObject();
					if (!Object.IsValid())
					{
						OutError = ConversionFailure(TEXT("canonical_json_object_invalid"), TEXT("The JSON object value has no object storage."), Current.Path);
						return false;
					}
					TArray<FString> Keys; Object->Values.GetKeys(Keys); Keys.Sort();
					const int32 Count = Keys.Num(); const FString* OrderedKeys = Keys.GetData();
					// The original expanded frame owns this allocation through the join;
					// moving/reallocating Work moves TArray ownership, not its elements.
					Work.Add({Current.Value, true, Current.Path, MoveTemp(Keys)});
					for (int32 Index = Count - 1; Index >= 0; --Index)
						Work.Add({Object->Values.FindChecked(OrderedKeys[Index]).Get(), false, ChildPath(Current.Path, OrderedKeys[Index])});
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
			case EJson::Boolean:
			case EJson::String:
			case EJson::Number:
				if (!TryFromJsonScalar(*Current.Value, Resources, Canonical, OutError, Current.Path)) return false;
				bCreated = true; break;

			case EJson::Array:
			{
				TArray<FUeAgentCanonicalValueRef> Items;
				const auto& Children = Current.Value->AsArray();
				for (int32 Index = 0; Index < Children.Num(); ++Index)
				{
					const auto& Item = Children[Index];
					const auto* Found = Built.Find(Item.Get());
					if (!Found || !Found->Value.IsValid())
					{
						OutError = ConversionFailure(TEXT("canonical_json_array_child_missing"), TEXT("The JSON array element has no converted canonical value."), ChildPath(Current.Path, FString::FromInt(Index)));
						return false;
					}
					Items.Add(Found->Value.ToSharedRef());
				}
				bCreated = FUeAgentCanonicalValue::TryArray(
					Items, Resources, Canonical, SemanticError);
				break;
			}
			case EJson::Object:
			{
				const TSharedPtr<FJsonObject>& Object = Current.Value->AsObject();
				if (!Object.IsValid())
				{
					OutError = ConversionFailure(TEXT("canonical_json_object_invalid"), TEXT("The JSON object value has no object storage."), Current.Path);
					return false;
				}
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
				for (const FString& Key : Current.Keys)
				{
					const TSharedPtr<FJsonValue>& Child =
						Object->Values.FindChecked(Key);
					const auto* Found = Built.Find(Child.Get());
					if (!Found || !Found->Value.IsValid())
					{
						OutError = ConversionFailure(TEXT("canonical_json_object_child_missing"), TEXT("The JSON object member has no converted canonical value."), ChildPath(Current.Path, Key));
						return false;
					}
					Items.Emplace(Key, Found->Value.ToSharedRef());
				}
				bCreated = FUeAgentCanonicalValue::TryObject(
					Items, Resources, Canonical, SemanticError);
				break;
			}
			default:
				OutError = ConversionFailure(TEXT("canonical_json_kind_unsupported"), TEXT("This JSON value kind is not supported by canonical conversion."), Current.Path, MakeShared<FJsonValueNumber>(static_cast<int32>(Current.Value->Type)));
				return false;
			}
			if (!bCreated || !Canonical.IsValid())
			{
				OutError = SemanticError.IsEmpty()
					? ConversionFailure(TEXT("canonical_json_value_create_failed"), TEXT("The canonical value factory returned no value or diagnostic."), TEXT("/"), MakeShared<FJsonValueNumber>(static_cast<int32>(Current.Value->Type)))
					: SemanticError;
				LocateConversionFailure(OutError, Current.Path);
				return false;
			}
			Built.FindOrAdd(Current.Value).Value = MoveTemp(Canonical);
		}

		const auto* Root = Built.Find(Value.Get());
		if (!Root || !Root->Value.IsValid())
		{
			OutError = ConversionFailure(TEXT("canonical_json_root_missing"), TEXT("Canonical conversion produced no root value."), TEXT("/"));
			return false;
		}
		OutValue = Root->Value;
		return true;
	}

	bool NeedsDecimalPoint(const FString& DecimalLexeme)
	{
		return !DecimalLexeme.Contains(TEXT(".")) && !DecimalLexeme.Contains(TEXT("e"), ESearchCase::IgnoreCase);
	}

enum class EJsonProjectionPurpose { ExactLexeme, AuthoringValue, AuthoringDeclaration };

static bool ProjectToJson(
		const FUeAgentCanonicalValue& Value,
		TSharedPtr<FJsonValue>& OutValue,
	UE::UAI::Contract::FDiagnostic& OutError, const EJsonProjectionPurpose Purpose)
	{
		struct FVisit final
		{
			const FUeAgentCanonicalValue* Value = nullptr;
			bool bExpanded = false;
			FString Path;
		};

		OutValue.Reset();
		OutError.Reset();
		TArray<FVisit> Work;
		TMap<const FUeAgentCanonicalValue*, TSharedPtr<FJsonValue>> Built;
		Work.Add({&Value, false, TEXT("/")});
		while (!Work.IsEmpty())
		{
			const FVisit Current = Work.Pop(EAllowShrinking::No);
			if (!Current.Value)
			{
				OutError = ConversionFailure(TEXT("canonical_json_value_missing"), TEXT("The value at this conversion path is missing."), Current.Path);
				return false;
			}
			if (!Current.bExpanded
				&& (Current.Value->GetKind() == EUeAgentValueKind::Array
					|| Current.Value->GetKind() == EUeAgentValueKind::Object))
			{
				Work.Add({Current.Value, true, Current.Path});
				if (Current.Value->GetKind() == EUeAgentValueKind::Array)
				{
					const auto& Items = Current.Value->GetArrayItems();
					for (int32 Index = Items.Num() - 1; Index >= 0; --Index)
					{
						Work.Add({&Items[Index].Get(), false, ChildPath(Current.Path, FString::FromInt(Index))});
					}
				}
				else
				{
					const auto& Items = Current.Value->GetObjectItems();
					for (int32 Index = Items.Num() - 1; Index >= 0; --Index)
					{
						Work.Add({&Items[Index].Value.Get(), false, ChildPath(Current.Path, Items[Index].Key)});
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
			{
				const FString& Lexeme = Current.Value->GetLexeme();
				if (Purpose != EJsonProjectionPurpose::ExactLexeme && Current.Value->GetKind() == EUeAgentValueKind::Decimal && NeedsDecimalPoint(Lexeme)) {
					if (Purpose == EJsonProjectionPurpose::AuthoringValue) {
						JsonValue = MakeShared<FJsonValueNumberString>(Lexeme + TEXT(".0"));
						break;
					}
					if (Purpose == EJsonProjectionPurpose::AuthoringDeclaration) {
						// Native JSON doubles enter a capture as Decimal even when the writer
						// spells an integer. Exact number strings with that spelling enter as Integer.
						const auto Native = MakeShared<FJsonValueNumber>(FCString::Atod(*Lexeme));
						FString Written;
						if (TryGetNumberLexeme(*Native, Written) && Written.Equals(Lexeme, ESearchCase::CaseSensitive)) {
							JsonValue = Native;
							break;
						}
					}
				}
				JsonValue = MakeShared<FJsonValueNumberString>(Lexeme);
				break;
			}
			case EUeAgentValueKind::Array:
			{
				TArray<TSharedPtr<FJsonValue>> Items;
				Items.Reserve(Current.Value->GetArrayItems().Num());
				const auto& Children = Current.Value->GetArrayItems();
				for (int32 Index = 0; Index < Children.Num(); ++Index)
				{
					const auto& Item = Children[Index];
					const TSharedPtr<FJsonValue>* Found = Built.Find(&Item.Get());
					if (!Found || !Found->IsValid())
					{
						OutError = ConversionFailure(TEXT("canonical_json_array_child_missing"), TEXT("The canonical array element has no projected JSON value."), ChildPath(Current.Path, FString::FromInt(Index)));
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
						OutError = ConversionFailure(TEXT("canonical_json_object_child_missing"), TEXT("The canonical object member has no projected JSON value."), ChildPath(Current.Path, Item.Key));
						return false;
					}
					if (Object->Values.Contains(Item.Key))
					{
						OutError = ConversionFailure(TEXT("canonical_json_object_key_collision"), TEXT("The projected JSON object contains a duplicate member key."), ChildPath(Current.Path, Item.Key), MakeShared<FJsonValueString>(Item.Key));
						return false;
					}
					Object->SetField(Item.Key, *Found);
				}
				JsonValue = MakeShared<FJsonValueObject>(Object);
				break;
			}
			case EUeAgentValueKind::Temporal:
			case EUeAgentValueKind::Spatial:
				if (!TryProjectTypedLeaf(*Current.Value, [](int64) { return true; }, JsonValue, OutError)) {
					LocateConversionFailure(OutError, Current.Path); return false;
				}
				break;
			default:
				OutError = ConversionFailure(TEXT("canonical_json_kind_unsupported"), TEXT("This canonical value kind has no JSON projection."), Current.Path, MakeShared<FJsonValueNumber>(static_cast<int32>(Current.Value->GetKind())));
				return false;
			}
			Built.Add(Current.Value, MoveTemp(JsonValue));
		}

		const TSharedPtr<FJsonValue>* Root = Built.Find(&Value);
		if (!Root || !Root->IsValid())
		{
			OutError = ConversionFailure(TEXT("canonical_json_root_missing"), TEXT("Canonical JSON projection produced no root value."), TEXT("/"));
			return false;
		}
		OutValue = *Root;
		return true;
	}

	bool TryToJsonValue(const FUeAgentCanonicalValue& Value, TSharedPtr<FJsonValue>& OutValue,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		return ProjectToJson(Value, OutValue, OutError, EJsonProjectionPurpose::ExactLexeme);
	}

	bool TryToAuthoringCodecJson(const FUeAgentCanonicalValue& Value, TSharedPtr<FJsonValue>& OutValue,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		return ProjectToJson(Value, OutValue, OutError, EJsonProjectionPurpose::AuthoringValue);
	}

	bool TryToAuthoringDeclarationJson(const FUeAgentCanonicalValue& Value, TSharedPtr<FJsonValue>& OutValue,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		return ProjectToJson(Value, OutValue, OutError, EJsonProjectionPurpose::AuthoringDeclaration);
	}
}
