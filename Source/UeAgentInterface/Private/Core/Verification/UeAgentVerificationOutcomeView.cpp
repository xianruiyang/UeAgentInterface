#include "Core/Verification/UeAgentVerificationOutcomeView.h"

namespace UE::UAI::Verification
{
	using UE::UAI::StateSemantic::EUeAgentDifferenceKind;
	using UE::UAI::StateSemantic::FUeAgentCanonicalIdentity;
	using UE::UAI::StateSemantic::FUeAgentCanonicalValue;
	using UE::UAI::StateSemantic::FUeAgentCanonicalValuePtr;
	using UE::UAI::StateSemantic::FUeAgentCanonicalValueRef;
	using UE::UAI::StateSemantic::FUeAgentSemanticComparison;
	using UE::UAI::StateSemantic::FUeAgentSemanticDifference;
	using UE::UAI::StateSemantic::FUeAgentStateSemanticError;

	namespace
	{
		constexpr const TCHAR* OutcomeViewSchema =
			TEXT("uai.c9.canonical_outcome_view.v1");

		class FCanonicalViewWriter final
		{
		public:
			FCanonicalViewWriter(
				FUeAgentResourceTracker& InResources,
				FVerificationError& InError)
				: Resources(InResources)
				, Error(InError)
			{
			}

			bool TryString(
				const FString& Value,
				FUeAgentCanonicalValuePtr& OutValue)
			{
				FUeAgentStateSemanticError StateError;
				if (!FUeAgentCanonicalValue::TryString(
					Value,
					Resources,
					OutValue,
					StateError))
				{
					return Fail(StateError);
				}
				return true;
			}

			bool TryBoolean(
				const bool Value,
				FUeAgentCanonicalValuePtr& OutValue)
			{
				FUeAgentStateSemanticError StateError;
				if (!FUeAgentCanonicalValue::TryBoolean(
					Value,
					Resources,
					OutValue,
					StateError))
				{
					return Fail(StateError);
				}
				return true;
			}

			bool TryInteger(
				const int64 Value,
				FUeAgentCanonicalValuePtr& OutValue)
			{
				FUeAgentStateSemanticError StateError;
				if (!FUeAgentCanonicalValue::TryInteger(
					FString::Printf(TEXT("%lld"), Value),
					Resources,
					OutValue,
					StateError))
				{
					return Fail(StateError);
				}
				return true;
			}

			bool TryArray(
				const TArray<FUeAgentCanonicalValueRef>& Items,
				FUeAgentCanonicalValuePtr& OutValue)
			{
				FUeAgentStateSemanticError StateError;
				if (!FUeAgentCanonicalValue::TryArray(
					Items,
					Resources,
					OutValue,
					StateError))
				{
					return Fail(StateError);
				}
				return true;
			}

			bool TryObject(
				const TArray<
					TPair<FString, FUeAgentCanonicalValueRef>>& Items,
				FUeAgentCanonicalValuePtr& OutValue)
			{
				FUeAgentStateSemanticError StateError;
				if (!FUeAgentCanonicalValue::TryObject(
					Items,
					Resources,
					OutValue,
					StateError))
				{
					return Fail(StateError);
				}
				return true;
			}

		private:
			bool Fail(const FUeAgentStateSemanticError& StateError)
			{
				FString Detail = StateError.Detail;
				if (Detail.IsEmpty())
				{
					Detail = StateError.Path;
				}
				Error = FVerificationError(
					StateError.Code,
					MoveTemp(Detail));
				return false;
			}

			FUeAgentResourceTracker& Resources;
			FVerificationError& Error;
		};

		void AddField(
			TArray<TPair<FString, FUeAgentCanonicalValueRef>>& Fields,
			const TCHAR* Name,
			const FUeAgentCanonicalValuePtr& Value)
		{
			Fields.Emplace(FString(Name), Value.ToSharedRef());
		}

		FString DifferenceKindLabel(
			const EUeAgentDifferenceKind Kind)
		{
			switch (Kind)
			{
			case EUeAgentDifferenceKind::ValueMismatch:
				return TEXT("value_mismatch");
			case EUeAgentDifferenceKind::TypeMismatch:
				return TEXT("type_mismatch");
			case EUeAgentDifferenceKind::IdentityMismatch:
				return TEXT("identity_mismatch");
			case EUeAgentDifferenceKind::DefaultMismatch:
				return TEXT("default_mismatch");
			case EUeAgentDifferenceKind::DetailsMismatch:
				return TEXT("details_mismatch");
			case EUeAgentDifferenceKind::AuthoredPresenceMismatch:
				return TEXT("authored_presence_mismatch");
			case EUeAgentDifferenceKind::OverrideStateMismatch:
				return TEXT("override_state_mismatch");
			case EUeAgentDifferenceKind::StatePlaneMismatch:
				return TEXT("state_plane_mismatch");
			case EUeAgentDifferenceKind::SnapshotRevisionMismatch:
				return TEXT("snapshot_revision_mismatch");
			case EUeAgentDifferenceKind::TimeDomainMismatch:
				return TEXT("time_domain_mismatch");
			case EUeAgentDifferenceKind::TimeStateMismatch:
				return TEXT("time_state_mismatch");
			case EUeAgentDifferenceKind::FrameMismatch:
				return TEXT("frame_mismatch");
			case EUeAgentDifferenceKind::SpatialQuantityMismatch:
				return TEXT("spatial_quantity_mismatch");
			case EUeAgentDifferenceKind::SubjectMismatch:
				return TEXT("subject_mismatch");
			case EUeAgentDifferenceKind::MissingLeft:
				return TEXT("missing_left");
			case EUeAgentDifferenceKind::MissingRight:
				return TEXT("missing_right");
			default:
				return TEXT("unknown");
			}
		}

		bool TryStringArray(
			const TArray<FString>& Values,
			FCanonicalViewWriter& Writer,
			FUeAgentCanonicalValuePtr& OutValue)
		{
			TArray<FUeAgentCanonicalValueRef> Items;
			Items.Reserve(Values.Num());
			for (const FString& Value : Values)
			{
				FUeAgentCanonicalValuePtr Item;
				if (!Writer.TryString(Value, Item))
				{
					return false;
				}
				Items.Add(Item.ToSharedRef());
			}
			return Writer.TryArray(Items, OutValue);
		}

		bool TryIdentityView(
			const FUeAgentCanonicalIdentity& Identity,
			FCanonicalViewWriter& Writer,
			FUeAgentCanonicalValuePtr& OutValue)
		{
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
			FUeAgentCanonicalValuePtr Value;
			if (!Writer.TryString(TEXT("canonical_identity"), Value))
			{
				return false;
			}
			AddField(Fields, TEXT("$type"), Value);
			if (!Writer.TryString(Identity.GetSemanticId(), Value))
			{
				return false;
			}
			AddField(Fields, TEXT("semantic_id"), Value);
			if (!Writer.TryString(Identity.GetTargetKind(), Value))
			{
				return false;
			}
			AddField(Fields, TEXT("target_kind"), Value);
			if (!Writer.TryString(Identity.GetOwnerId(), Value))
			{
				return false;
			}
			AddField(Fields, TEXT("owner_id"), Value);
			if (!Writer.TryString(Identity.GetSelector(), Value))
			{
				return false;
			}
			AddField(Fields, TEXT("selector"), Value);
			if (!Writer.TryString(Identity.GetObjectRole(), Value))
			{
				return false;
			}
			AddField(Fields, TEXT("object_role"), Value);
			if (!Writer.TryString(Identity.GetGeneration(), Value))
			{
				return false;
			}
			AddField(Fields, TEXT("generation"), Value);
			return Writer.TryObject(Fields, OutValue);
		}

		bool TryDifferenceView(
			const FUeAgentSemanticDifference& Difference,
			FCanonicalViewWriter& Writer,
			FUeAgentCanonicalValuePtr& OutValue)
		{
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
			FUeAgentCanonicalValuePtr Value;
			if (!Writer.TryString(TEXT("semantic_difference"), Value))
			{
				return false;
			}
			AddField(Fields, TEXT("$type"), Value);
			if (!Writer.TryString(
				DifferenceKindLabel(Difference.GetKind()),
				Value))
			{
				return false;
			}
			AddField(Fields, TEXT("kind"), Value);
			if (!Writer.TryString(Difference.GetPath(), Value))
			{
				return false;
			}
			AddField(Fields, TEXT("path"), Value);
			if (Difference.GetLeft().IsValid())
			{
				AddField(Fields, TEXT("left"), Difference.GetLeft());
			}
			if (Difference.GetRight().IsValid())
			{
				AddField(Fields, TEXT("right"), Difference.GetRight());
			}
			return Writer.TryObject(Fields, OutValue);
		}

		bool TryComparisonView(
			const FUeAgentSemanticComparison& Comparison,
			FCanonicalViewWriter& Writer,
			FUeAgentCanonicalValuePtr& OutValue)
		{
			TArray<FUeAgentCanonicalValueRef> DifferenceItems;
			DifferenceItems.Reserve(
				Comparison.GetDifferences().Num());
			for (
				const FUeAgentSemanticDifference& Difference
				: Comparison.GetDifferences())
			{
				FUeAgentCanonicalValuePtr DifferenceValue;
				if (!TryDifferenceView(
					Difference,
					Writer,
					DifferenceValue))
				{
					return false;
				}
				DifferenceItems.Add(DifferenceValue.ToSharedRef());
			}

			FUeAgentCanonicalValuePtr Differences;
			if (!Writer.TryArray(DifferenceItems, Differences))
			{
				return false;
			}
			FUeAgentCanonicalValuePtr Type;
			if (!Writer.TryString(TEXT("semantic_comparison"), Type))
			{
				return false;
			}
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
			AddField(Fields, TEXT("$type"), Type);
			AddField(Fields, TEXT("differences"), Differences);
			return Writer.TryObject(Fields, OutValue);
		}

		bool TryTargetView(
			const FTargetVerification& Target,
			FCanonicalViewWriter& Writer,
			FUeAgentCanonicalValuePtr& OutValue)
		{
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
			FUeAgentCanonicalValuePtr Value;
			if (!Writer.TryString(TEXT("target_verification"), Value))
			{
				return false;
			}
			AddField(Fields, TEXT("$type"), Value);
			if (!TryIdentityView(Target.GetIdentity(), Writer, Value))
			{
				return false;
			}
			AddField(Fields, TEXT("identity"), Value);
			if (!Writer.TryString(
				LexToString(Target.GetOracleKind()),
				Value))
			{
				return false;
			}
			AddField(Fields, TEXT("oracle_kind"), Value);
			if (!Writer.TryString(
				LexToString(Target.GetStatus()),
				Value))
			{
				return false;
			}
			AddField(Fields, TEXT("status"), Value);
			if (!Writer.TryString(
				Target.GetExpectedSnapshotId(),
				Value))
			{
				return false;
			}
			AddField(Fields, TEXT("expected_snapshot_id"), Value);
			if (!Writer.TryString(
				Target.GetActualSnapshotId(),
				Value))
			{
				return false;
			}
			AddField(Fields, TEXT("actual_snapshot_id"), Value);
			if (Target.HasComparison())
			{
				if (!TryComparisonView(
					Target.GetComparison(),
					Writer,
					Value))
				{
					return false;
				}
				AddField(Fields, TEXT("comparison"), Value);
			}
			if (!TryStringArray(
				Target.GetEvidenceIds(),
				Writer,
				Value))
			{
				return false;
			}
			AddField(Fields, TEXT("evidence_ids"), Value);
			return Writer.TryObject(Fields, OutValue);
		}

		bool TryAcknowledgementView(
			const FVerificationAcknowledgement& Acknowledgement,
			FCanonicalViewWriter& Writer,
			FUeAgentCanonicalValuePtr& OutValue)
		{
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
			FUeAgentCanonicalValuePtr Value;
			if (!Writer.TryString(
				TEXT("durable_verification_acknowledgement"),
				Value))
			{
				return false;
			}
			AddField(Fields, TEXT("$type"), Value);
			if (!Writer.TryString(
				Acknowledgement.GetJournalId(),
				Value))
			{
				return false;
			}
			AddField(Fields, TEXT("journal_id"), Value);
			if (!Writer.TryString(
				Acknowledgement.GetPlanFingerprint(),
				Value))
			{
				return false;
			}
			AddField(Fields, TEXT("plan_fingerprint"), Value);
			if (!Writer.TryString(
				Acknowledgement.GetInventoryHash(),
				Value))
			{
				return false;
			}
			AddField(Fields, TEXT("inventory_hash"), Value);
			if (!Writer.TryString(
				Acknowledgement.GetVerificationId(),
				Value))
			{
				return false;
			}
			AddField(Fields, TEXT("verification_id"), Value);
			if (!Writer.TryString(
				Acknowledgement.GetJournalOwnerId(),
				Value))
			{
				return false;
			}
			AddField(Fields, TEXT("journal_owner_id"), Value);
			if (!Writer.TryInteger(
				Acknowledgement.GetFencingToken(),
				Value))
			{
				return false;
			}
			AddField(Fields, TEXT("fencing_token"), Value);
			return Writer.TryObject(Fields, OutValue);
		}
	}

	bool TryCanonicalizeVerificationOutcome(
		const FVerificationOutcome& Outcome,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutView,
		FVerificationError& OutError)
	{
		OutView.Reset();
		OutError = FVerificationError();
		FCanonicalViewWriter Writer(Resources, OutError);

		TArray<FTargetVerification> OrderedTargets =
			Outcome.GetTargets();
		OrderedTargets.Sort(
			[](const FTargetVerification& Left,
				const FTargetVerification& Right)
			{
				return Left
					.GetIdentity()
					.GetCanonicalKey()
					.Compare(
						Right.GetIdentity().GetCanonicalKey(),
						ESearchCase::CaseSensitive) < 0;
			});

		TArray<FUeAgentCanonicalValueRef> TargetItems;
		TargetItems.Reserve(OrderedTargets.Num());
		for (const FTargetVerification& Target : OrderedTargets)
		{
			FUeAgentCanonicalValuePtr TargetValue;
			if (!TryTargetView(Target, Writer, TargetValue))
			{
				return false;
			}
			TargetItems.Add(TargetValue.ToSharedRef());
		}

		TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
		FUeAgentCanonicalValuePtr Value;
		if (!Writer.TryString(OutcomeViewSchema, Value))
		{
			return false;
		}
		AddField(Fields, TEXT("$schema"), Value);
		if (!Writer.TryString(TEXT("verification_outcome"), Value))
		{
			return false;
		}
		AddField(Fields, TEXT("$type"), Value);
		if (!Writer.TryString(Outcome.GetVerificationId(), Value))
		{
			return false;
		}
		AddField(Fields, TEXT("verification_id"), Value);
		if (!Writer.TryString(LexToString(Outcome.GetStatus()), Value))
		{
			return false;
		}
		AddField(Fields, TEXT("status"), Value);
		if (!Writer.TryString(
			LexToString(Outcome.GetEffectOutcome()),
			Value))
		{
			return false;
		}
		AddField(Fields, TEXT("effect_outcome"), Value);
		if (!Writer.TryString(Outcome.GetDiagnosticCode(), Value))
		{
			return false;
		}
		AddField(Fields, TEXT("diagnostic_code"), Value);
		if (!Writer.TryBoolean(Outcome.IsRetrySafe(), Value))
		{
			return false;
		}
		AddField(Fields, TEXT("retry_safe"), Value);
		if (!Writer.TryArray(TargetItems, Value))
		{
			return false;
		}
		AddField(Fields, TEXT("targets"), Value);
		if (!TryStringArray(
			Outcome.GetEvidenceIds(),
			Writer,
			Value))
		{
			return false;
		}
		AddField(Fields, TEXT("evidence_ids"), Value);
		if (const FVerificationAcknowledgement* Acknowledgement =
			Outcome.GetAcknowledgement())
		{
			if (!TryAcknowledgementView(
				*Acknowledgement,
				Writer,
				Value))
			{
				return false;
			}
			AddField(Fields, TEXT("acknowledgement"), Value);
		}

		FUeAgentCanonicalValuePtr Root;
		if (!Writer.TryObject(Fields, Root))
		{
			return false;
		}
		OutView = MoveTemp(Root);
		return true;
	}
}
