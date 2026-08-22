#include "Core/StateSemantic/UeAgentStateRevisionJson.h"

#include "Dom/JsonValue.h"

namespace UE::UAI::StateSemantic
{
	namespace
	{
		bool TryReadExactSequence(
			const double Value,
			int64& OutSequence)
		{
			OutSequence = -1;
			if (!FMath::IsFinite(Value)
				|| Value < 0.0
				|| Value
					> static_cast<double>(
						FUeAgentStateRevisionJson::
							MaxExactJsonSequence))
			{
				return false;
			}
			OutSequence = static_cast<int64>(Value);
			return static_cast<double>(OutSequence) == Value;
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
		TOptional<FUeAgentStateRevision>& OutRevision)
	{
		OutRevision.Reset();
		FString SourceId;
		FString Generation;
		double SequenceNumber = -1.0;
		int64 Sequence = -1;
		if (!Object.IsValid()
			|| !Object->TryGetStringField(TEXT("source_id"), SourceId)
			|| !Object->TryGetStringField(TEXT("generation"), Generation)
			|| !Object->TryGetNumberField(
				TEXT("sequence"),
				SequenceNumber)
			|| !TryReadExactSequence(SequenceNumber, Sequence))
		{
			return false;
		}
		FUeAgentStateRevision Revision(
			MoveTemp(SourceId),
			MoveTemp(Generation),
			Sequence);
		if (!Revision.IsValid())
		{
			return false;
		}
		OutRevision.Emplace(MoveTemp(Revision));
		return true;
	}

	bool FUeAgentStateRevisionJson::TryReadCanonical(
		const FUeAgentCanonicalValuePtr& Value,
		TOptional<FUeAgentStateRevision>& OutRevision)
	{
		OutRevision.Reset();
		const FUeAgentCanonicalValuePtr SourceIdValue =
			FindField(Value, TEXT("source_id"));
		const FUeAgentCanonicalValuePtr GenerationValue =
			FindField(Value, TEXT("generation"));
		const FUeAgentCanonicalValuePtr SequenceValue =
			FindField(Value, TEXT("sequence"));
		if (!SourceIdValue.IsValid()
			|| SourceIdValue->GetKind() != EUeAgentValueKind::String
			|| !GenerationValue.IsValid()
			|| GenerationValue->GetKind() != EUeAgentValueKind::String
			|| !SequenceValue.IsValid()
			|| (SequenceValue->GetKind() != EUeAgentValueKind::Integer
				&& SequenceValue->GetKind()
					!= EUeAgentValueKind::Decimal))
		{
			return false;
		}
		double SequenceNumber = -1.0;
		int64 Sequence = -1;
		if (!LexTryParseString(
				SequenceNumber,
				*SequenceValue->GetLexeme())
			|| !TryReadExactSequence(SequenceNumber, Sequence))
		{
			return false;
		}
		FUeAgentStateRevision Revision(
			SourceIdValue->GetText(),
			GenerationValue->GetText(),
			Sequence);
		if (!Revision.IsValid())
		{
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
