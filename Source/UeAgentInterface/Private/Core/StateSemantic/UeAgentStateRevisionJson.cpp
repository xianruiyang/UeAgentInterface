#include "Core/StateSemantic/UeAgentStateRevisionJson.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"

#include "Dom/JsonValue.h"

namespace UE::UAI::StateSemantic
{
	namespace
	{
		Contract::FDiagnostic RevisionFailure(const FString& Field, const FString& Expected,
			const TSharedPtr<FJsonValue>& Actual, const bool bOpaque = false)
		{
			auto Error = Contract::FDiagnostic::Failure(TEXT("state_revision_field_invalid"),
				TEXT("The state revision field does not satisfy its required representation."), TEXT("state_semantic"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Field;
			Error.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			Error.ActualValuePresent = Actual.IsValid();
			if (bOpaque) Error.ActualEvidence = Actual;
			else Error.ActualValue = Actual;
			return Error;
		}

		TSharedPtr<FJsonValue> CanonicalRevisionFact(const FUeAgentCanonicalValuePtr& Value)
		{
			if (!Value.IsValid()) return nullptr;
			switch (Value->GetKind())
			{
			case EUeAgentValueKind::Null: return MakeShared<FJsonValueNull>();
			case EUeAgentValueKind::Boolean: return MakeShared<FJsonValueBoolean>(Value->GetBoolean());
			case EUeAgentValueKind::String: return MakeShared<FJsonValueString>(Value->GetText());
			case EUeAgentValueKind::Integer:
			case EUeAgentValueKind::Decimal: return MakeShared<FJsonValueString>(Value->GetLexeme());
			case EUeAgentValueKind::Array: return MakeShared<FJsonValueString>(TEXT("array"));
			case EUeAgentValueKind::Object: return MakeShared<FJsonValueString>(TEXT("object"));
			case EUeAgentValueKind::Temporal: return MakeShared<FJsonValueString>(TEXT("temporal"));
			case EUeAgentValueKind::Spatial: return MakeShared<FJsonValueString>(TEXT("spatial"));
			default: return MakeShared<FJsonValueNumber>(static_cast<int32>(Value->GetKind()));
			}
		}

		bool TryReadExactSequence(const FString& Lexeme, int64& OutSequence, Contract::FDiagnostic& OutError)
		{
			OutSequence = -1;
			if (!TryParseInt64Number(Lexeme, OutSequence) || OutSequence < 0
				|| OutSequence > FUeAgentStateRevisionJson::MaxExactJsonSequence)
			{
				OutError = RevisionFailure(TEXT("sequence"),
					TEXT("integer from 0 through 9007199254740991"), MakeShared<FJsonValueString>(Lexeme), true);
				return false;
			}
			return true;
		}

		FUeAgentCanonicalValuePtr FindField(
			const FUeAgentCanonicalValuePtr& Object,
			const FString& Field)
		{
			if (!Object.IsValid()
				|| Object->GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
				: Object->GetObjectItems())
			{
				if (Item.Key == Field)
				{
					return FUeAgentCanonicalValuePtr(Item.Value);
				}
			}
			return nullptr;
		}
	}

	bool FUeAgentStateRevisionJson::TryParse(
		const TSharedPtr<FJsonObject>& Object,
		TOptional<FUeAgentStateRevision>& OutRevision,
		Contract::FDiagnostic& OutError)
	{
		OutRevision.Reset();
		OutError.Reset();
		if (!Object.IsValid())
		{
			OutError = RevisionFailure(TEXT("target_revision"), TEXT("object"), nullptr);
			return false;
		}
		FString SourceId;
		FString Generation;
		FString SequenceNumber;
		int64 Sequence = -1;
		if (!Object->TryGetStringField(TEXT("source_id"), SourceId))
		{
			OutError = RevisionFailure(TEXT("source_id"), TEXT("string"), Object->TryGetField(TEXT("source_id")), true);
			return false;
		}
		if (!Object->TryGetStringField(TEXT("generation"), Generation))
		{
			OutError = RevisionFailure(TEXT("generation"), TEXT("string"), Object->TryGetField(TEXT("generation")), true);
			return false;
		}
		const TSharedPtr<FJsonValue> SequenceValue = Object->TryGetField(TEXT("sequence"));
		if (!SequenceValue.IsValid() || !CanonicalJson::TryGetNumberLexeme(*SequenceValue, SequenceNumber))
		{
			OutError = RevisionFailure(TEXT("sequence"), TEXT("JSON number"), SequenceValue, true);
			return false;
		}
		if (!TryReadExactSequence(SequenceNumber, Sequence, OutError)) return false;
		FUeAgentStateRevision Revision(MoveTemp(SourceId), MoveTemp(Generation), Sequence);
		if (!Revision.IsValid())
		{
			OutError = Revision.GetValidationDiagnostic();
			return false;
		}
		OutRevision.Emplace(MoveTemp(Revision));
		return true;
	}

	bool FUeAgentStateRevisionJson::TryReadCanonical(
		const FUeAgentCanonicalValuePtr& Value,
		TOptional<FUeAgentStateRevision>& OutRevision,
		Contract::FDiagnostic& OutError)
	{
		OutRevision.Reset();
		OutError.Reset();
		const FUeAgentCanonicalValuePtr SourceIdValue = FindField(Value, TEXT("source_id"));
		const FUeAgentCanonicalValuePtr GenerationValue = FindField(Value, TEXT("generation"));
		const FUeAgentCanonicalValuePtr SequenceValue = FindField(Value, TEXT("sequence"));
		if (!SourceIdValue.IsValid() || SourceIdValue->GetKind() != EUeAgentValueKind::String)
		{
			OutError = !Value.IsValid() || Value->GetKind() != EUeAgentValueKind::Object
				? RevisionFailure(TEXT("target_revision"), TEXT("object"), CanonicalRevisionFact(Value), true)
				: RevisionFailure(TEXT("source_id"), TEXT("string"), CanonicalRevisionFact(SourceIdValue), true);
			return false;
		}
		if (!GenerationValue.IsValid() || GenerationValue->GetKind() != EUeAgentValueKind::String)
		{
			OutError = RevisionFailure(TEXT("generation"), TEXT("string"), CanonicalRevisionFact(GenerationValue), true);
			return false;
		}
		if (!SequenceValue.IsValid() || (SequenceValue->GetKind() != EUeAgentValueKind::Integer
			&& SequenceValue->GetKind() != EUeAgentValueKind::Decimal))
		{
			OutError = RevisionFailure(TEXT("sequence"), TEXT("number"), CanonicalRevisionFact(SequenceValue), true);
			return false;
		}
		int64 Sequence = -1;
		if (!TryReadExactSequence(SequenceValue->GetLexeme(), Sequence, OutError)) return false;
		FUeAgentStateRevision Revision(SourceIdValue->GetText(), GenerationValue->GetText(), Sequence);
		if (!Revision.IsValid())
		{
			OutError = Revision.GetValidationDiagnostic();
			return false;
		}
		OutRevision.Emplace(MoveTemp(Revision));
		return true;
	}

	TSharedRef<FJsonObject> FUeAgentStateRevisionJson::ToJson(
		const FUeAgentStateRevision& Revision)
	{
		check(Revision.IsValid());
		check(Revision.GetSequence() <= MaxExactJsonSequence);
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(
			TEXT("source_id"),
			Revision.GetSourceId());
		Object->SetStringField(
			TEXT("generation"),
			Revision.GetGeneration());
		Object->SetNumberField(
			TEXT("sequence"),
			static_cast<double>(Revision.GetSequence()));
		return Object;
	}
}
