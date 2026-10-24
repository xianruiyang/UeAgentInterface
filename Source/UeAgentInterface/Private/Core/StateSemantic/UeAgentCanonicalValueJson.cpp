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

	bool TryFromJsonValue(
		const TSharedPtr<FJsonValue>& Value,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		struct FVisit final
		{
			TSharedPtr<FJsonValue> Value;
			bool bExpanded = false;
			FString Path;
		};

		OutValue.Reset();
		OutError.Reset();
		if (!Value.IsValid())
		{
			OutError = ConversionFailure(TEXT("canonical_json_value_missing"), TEXT("The input JSON value is missing."), TEXT("/"));
			return false;
		}

		TArray<FVisit> Work;
		TMap<const FJsonValue*, FUeAgentCanonicalValuePtr> Built;
		TSet<const FJsonValue*> Expanded;
		Work.Add({Value, false, TEXT("/")});
		while (!Work.IsEmpty())
		{
			const FVisit Current = Work.Pop(EAllowShrinking::No);
			if (!Current.Value.IsValid())
			{
				OutError = ConversionFailure(TEXT("canonical_json_value_missing"), TEXT("The value at this conversion path is missing."), Current.Path);
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
				Work.Add({Current.Value, true, Current.Path});
				if (Current.Value->Type == EJson::Array)
				{
					const TArray<TSharedPtr<FJsonValue>>& Items =
						Current.Value->AsArray();
					for (int32 Index = Items.Num() - 1; Index >= 0; --Index)
					{
						Work.Add({Items[Index], false, ChildPath(Current.Path, FString::FromInt(Index))});
					}
				}
				else
				{
					const TSharedPtr<FJsonObject> Object =
						Current.Value->AsObject();
					if (!Object.IsValid())
					{
						OutError = ConversionFailure(TEXT("canonical_json_object_invalid"), TEXT("The JSON object value has no object storage."), Current.Path);
						return false;
					}
					TArray<FString> Keys;
					Object->Values.GetKeys(Keys);
					Keys.Sort();
					for (int32 Index = Keys.Num() - 1; Index >= 0; --Index)
					{
						Work.Add({Object->Values.FindChecked(Keys[Index]), false, ChildPath(Current.Path, Keys[Index])});
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
			{
				FString Lexeme;
				if (!TryGetNumberLexeme(*Current.Value, Lexeme))
				{
					OutError = ConversionFailure(TEXT("canonical_json_number_lexeme_invalid"), TEXT("The JSON number cannot provide a finite exact numeric spelling."), Current.Path,
						Current.Value->PreferStringRepresentation() ? nullptr : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(FString::Printf(TEXT("%.17g"), Current.Value->AsNumber()))),
						MakeShared<FJsonValueString>(TEXT("finite numeric spelling")));
					return false;
				}
				if (Current.Value->PreferStringRepresentation())
				{
					// Use the canonical numeric grammar rather than a double or a
					// second HJSON parse. Invalid integer grammar allocates nothing;
					// allocation failures must not be retried as another value kind.
					bCreated = FUeAgentCanonicalValue::TryInteger(Lexeme,
						Resources, Canonical, SemanticError);
					if (!bCreated && SemanticError.Code == TEXT("integer_lexeme_not_canonical"))
						bCreated = FUeAgentCanonicalValue::TryDecimal(Lexeme,
							Resources, Canonical, SemanticError);
					break;
				}
				bCreated = FUeAgentCanonicalValue::TryDecimal(
					Lexeme,
					Resources, Canonical, SemanticError);
				break;
			}
			case EJson::Array:
			{
				TArray<FUeAgentCanonicalValueRef> Items;
				const auto& Children = Current.Value->AsArray();
				for (int32 Index = 0; Index < Children.Num(); ++Index)
				{
					const auto& Item = Children[Index];
					const FUeAgentCanonicalValuePtr* Found = Built.Find(Item.Get());
					if (!Found || !Found->IsValid())
					{
						OutError = ConversionFailure(TEXT("canonical_json_array_child_missing"), TEXT("The JSON array element has no converted canonical value."), ChildPath(Current.Path, FString::FromInt(Index)));
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
					OutError = ConversionFailure(TEXT("canonical_json_object_invalid"), TEXT("The JSON object value has no object storage."), Current.Path);
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
						OutError = ConversionFailure(TEXT("canonical_json_object_child_missing"), TEXT("The JSON object member has no converted canonical value."), ChildPath(Current.Path, Key));
						return false;
					}
					Items.Emplace(Key, Found->ToSharedRef());
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
			Built.Add(Current.Value.Get(), Canonical);
		}

		const FUeAgentCanonicalValuePtr* Root = Built.Find(Value.Get());
		if (!Root || !Root->IsValid())
		{
			OutError = ConversionFailure(TEXT("canonical_json_root_missing"), TEXT("Canonical conversion produced no root value."), TEXT("/"));
			return false;
		}
		OutValue = *Root;
		return true;
	}

	bool TryToJsonValue(
		const FUeAgentCanonicalValue& Value,
		TSharedPtr<FJsonValue>& OutValue,
		UE::UAI::Contract::FDiagnostic& OutError)
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
				JsonValue = MakeShared<FJsonValueNumberString>(
					Current.Value->GetLexeme());
				break;
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
}
