#include "Core/StateSemantic/UeAgentStateSemanticEvidence.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"

namespace UE::UAI::StateSemantic
{
	Contract::FDiagnostic MakeComparisonDiagnostic(const FUeAgentSemanticComparison& Comparison,
		const FString& Code, const FString& Reason, const FString& Stage, const FString& Schema)
	{
		auto Result = Contract::FDiagnostic::Failure(Code, Reason, Stage);
		Result.Schema = Schema;
		for (const auto& Difference : Comparison.GetDifferences())
		{
			auto Item = Contract::FDiagnostic::Failure(Code, Difference.GetPublicReason(), Stage);
			Item.Schema = Schema;
			Item.Source.JsonPath = Difference.GetPublicPath();
			Item.ExpectedValuePresent = Difference.GetLeft().IsValid();
			Item.ActualValuePresent = Difference.GetRight().IsValid();
			Contract::FDiagnostic ProjectionError;
			if (Difference.GetPublicLeft() && !CanonicalJson::TryToJsonValue(*Difference.GetPublicLeft(), Item.ExpectedValue, ProjectionError))
				Item.Diagnostics.Add(ProjectionError);
			if (Difference.GetPublicRight() && !CanonicalJson::TryToJsonValue(*Difference.GetPublicRight(), Item.ActualValue, ProjectionError))
				Item.Diagnostics.Add(ProjectionError);
			Item.Source.InternalJsonPath = Difference.GetPath();
			if (Difference.GetLeft() && !CanonicalJson::TryToJsonValue(*Difference.GetLeft(), Item.ExpectedEvidence, ProjectionError))
				Item.Diagnostics.Add(ProjectionError);
			if (Difference.GetRight() && !CanonicalJson::TryToJsonValue(*Difference.GetRight(), Item.ActualEvidence, ProjectionError))
				Item.Diagnostics.Add(ProjectionError);
			Result.Diagnostics.Add(MoveTemp(Item));
		}
		return Result;
	}

	namespace
	{
		using FEvidenceFields =
			TArray<TPair<FString, FUeAgentCanonicalValueRef>>;

		void ResetEvidence(
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError)
		{
			OutValue.Reset();
			OutError = FUeAgentStateSemanticError();
		}

		bool FailEvidence(
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError,
			const FString& Detail)
		{
			OutValue.Reset();
			OutError = FUeAgentStateSemanticError::Failure(TEXT("semantic_difference_evidence_invalid"), Detail, TEXT("state_semantic"));
			OutError.Source.JsonPath = TEXT("/difference_evidence");
			return false;
		}

		FString ValueKindName(EUeAgentValueKind Value)
		{
			switch (Value)
			{
			case EUeAgentValueKind::Null: return TEXT("null");
			case EUeAgentValueKind::Boolean: return TEXT("boolean");
			case EUeAgentValueKind::String: return TEXT("string");
			case EUeAgentValueKind::Integer: return TEXT("integer");
			case EUeAgentValueKind::Decimal: return TEXT("decimal");
			case EUeAgentValueKind::Array: return TEXT("array");
			case EUeAgentValueKind::Object: return TEXT("object");
			case EUeAgentValueKind::Temporal: return TEXT("temporal");
			case EUeAgentValueKind::Spatial: return TEXT("spatial");
			default: return FString();
			}
		}

		FString DefaultSourceKindName(
			EUeAgentDefaultSourceKind Value)
		{
			switch (Value)
			{
			case EUeAgentDefaultSourceKind::Factory:
				return TEXT("factory");
			case EUeAgentDefaultSourceKind::ClassDefaultObject:
				return TEXT("class_default_object");
			case EUeAgentDefaultSourceKind::Parent:
				return TEXT("parent");
			case EUeAgentDefaultSourceKind::Constructor:
				return TEXT("constructor");
			case EUeAgentDefaultSourceKind::StructInitializer:
				return TEXT("struct_initializer");
			case EUeAgentDefaultSourceKind::Schema:
				return TEXT("schema");
			case EUeAgentDefaultSourceKind::DomainNative:
				return TEXT("domain_native");
			case EUeAgentDefaultSourceKind::PostCreateReadback:
				return TEXT("post_create_readback");
			default:
				return FString();
			}
		}

		FString SpatialQuantityKindName(
			EUeAgentSpatialQuantityKind Value)
		{
			switch (Value)
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
				return FString();
			}
		}

		FString AuthoredPresenceName(
			EUeAgentAuthoredPresence Value)
		{
			switch (Value)
			{
			case EUeAgentAuthoredPresence::Absent:
				return TEXT("absent");
			case EUeAgentAuthoredPresence::Present:
				return TEXT("present");
			default:
				return FString();
			}
		}

		FString OverrideStateName(EUeAgentOverrideState Value)
		{
			switch (Value)
			{
			case EUeAgentOverrideState::None:
				return TEXT("none");
			case EUeAgentOverrideState::Inherited:
				return TEXT("inherited");
			case EUeAgentOverrideState::Overridden:
				return TEXT("overridden");
			default:
				return FString();
			}
		}

		FString StatePlaneName(EUeAgentStatePlane Value)
		{
			return UE::UAI::Contract::LexToString(Value);
		}

		class FEvidenceBuilder final
		{
		public:
			FEvidenceBuilder(
				FUeAgentResourceTracker& InResources,
				FUeAgentStateSemanticError& InError)
				: Resources(InResources)
				, Error(InError)
			{
			}

			bool AddString(
				FEvidenceFields& Fields,
				const FString& Key,
				const FString& Value)
			{
				FUeAgentCanonicalValuePtr Canonical;
				if (!FUeAgentCanonicalValue::TryString(
					Value,
					Resources,
					Canonical,
					Error))
				{
					return false;
				}
				Fields.Emplace(Key, Canonical.ToSharedRef());
				return true;
			}

			bool AddBoolean(
				FEvidenceFields& Fields,
				const FString& Key,
				bool Value)
			{
				FUeAgentCanonicalValuePtr Canonical;
				if (!FUeAgentCanonicalValue::TryBoolean(
					Value,
					Resources,
					Canonical,
					Error))
				{
					return false;
				}
				Fields.Emplace(Key, Canonical.ToSharedRef());
				return true;
			}

			bool AddInteger(
				FEvidenceFields& Fields,
				const FString& Key,
				const FString& Lexeme)
			{
				FUeAgentCanonicalValuePtr Canonical;
				if (!FUeAgentCanonicalValue::TryInteger(
					Lexeme,
					Resources,
					Canonical,
					Error))
				{
					return false;
				}
				Fields.Emplace(Key, Canonical.ToSharedRef());
				return true;
			}

			void AddValue(
				FEvidenceFields& Fields,
				const FString& Key,
				const FUeAgentCanonicalValueRef& Value)
			{
				Fields.Emplace(Key, Value);
			}

			bool AddStringArray(
				FEvidenceFields& Fields,
				const FString& Key,
				const TArray<FString>& Values)
			{
				TArray<FUeAgentCanonicalValueRef> Items;
				Items.Reserve(Values.Num());
				for (const FString& Value : Values)
				{
					FUeAgentCanonicalValuePtr Canonical;
					if (!FUeAgentCanonicalValue::TryString(
						Value,
						Resources,
						Canonical,
						Error))
					{
						return false;
					}
					Items.Add(Canonical.ToSharedRef());
				}
				FUeAgentCanonicalValuePtr Array;
				if (!FUeAgentCanonicalValue::TryArray(
					Items,
					Resources,
					Array,
					Error))
				{
					return false;
				}
				Fields.Emplace(Key, Array.ToSharedRef());
				return true;
			}

			bool Finish(
				const FString& Type,
				FEvidenceFields Fields,
				FUeAgentCanonicalValuePtr& OutValue)
			{
				FUeAgentCanonicalValuePtr TypeValue;
				if (!FUeAgentCanonicalValue::TryString(
					Type,
					Resources,
					TypeValue,
					Error))
				{
					return false;
				}
				Fields.Emplace(
					TEXT("$type"),
					TypeValue.ToSharedRef());
				return FUeAgentCanonicalValue::TryObject(
					Fields,
					Resources,
					OutValue,
					Error);
			}

		private:
			FUeAgentResourceTracker& Resources;
			FUeAgentStateSemanticError& Error;
		};

		bool BuildStringMatrix(
			const TArray<TArray<FString>>& Values,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError)
		{
			TArray<FUeAgentCanonicalValueRef> Rows;
			Rows.Reserve(Values.Num());
			for (const TArray<FString>& Row : Values)
			{
				TArray<FUeAgentCanonicalValueRef> Items;
				Items.Reserve(Row.Num());
				for (const FString& Value : Row)
				{
					FUeAgentCanonicalValuePtr Item;
					if (!FUeAgentCanonicalValue::TryString(
						Value,
						Resources,
						Item,
						OutError))
					{
						return false;
					}
					Items.Add(Item.ToSharedRef());
				}
				FUeAgentCanonicalValuePtr CanonicalRow;
				if (!FUeAgentCanonicalValue::TryArray(
					Items,
					Resources,
					CanonicalRow,
					OutError))
				{
					return false;
				}
				Rows.Add(CanonicalRow.ToSharedRef());
			}
			return FUeAgentCanonicalValue::TryArray(
				Rows,
				Resources,
				OutValue,
				OutError);
		}
	}

	bool TryCanonicalizeDifferenceEvidence(
		const FUeAgentCanonicalValuePtr& Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError,
		EDifferenceEvidenceAudience Audience)
	{
		ResetEvidence(OutValue, OutError);
		if (Audience == EDifferenceEvidenceAudience::Internal || !Source) { OutValue = Source; return true; }
		// Walk typed values, not JSON keys. Ordinary user objects retain every key.
		TArray<TPair<FUeAgentCanonicalValuePtr, bool>> Pending;
		TMap<const FUeAgentCanonicalValue*, FUeAgentCanonicalValuePtr> Converted;
		Pending.Emplace(Source, false);
		while (!Pending.IsEmpty())
		{
			auto Task = Pending.Pop(EAllowShrinking::No);
			const auto& Value = Task.Key;
			if (Converted.Contains(Value.Get())) continue;
			const auto Kind = Value->GetKind();
			if (!Task.Value && (Kind == EUeAgentValueKind::Array || Kind == EUeAgentValueKind::Object))
			{
				Pending.Emplace(Value, true);
				if (Kind == EUeAgentValueKind::Array)
					for (const auto& Child : Value->GetArrayItems()) Pending.Emplace(Child, false);
				else
					for (const auto& Child : Value->GetObjectItems()) Pending.Emplace(Child.Value, false);
				continue;
			}
			FUeAgentCanonicalValuePtr Public = Value;
			if (Kind == EUeAgentValueKind::Temporal)
			{
				if (!TryCanonicalizeDifferenceEvidence(*Value->GetTemporal(), Resources, Public, OutError, Audience)) return false;
			}
			else if (Kind == EUeAgentValueKind::Spatial)
			{
				if (!TryCanonicalizeDifferenceEvidence(*Value->GetSpatial(), Resources, Public, OutError, Audience)) return false;
			}
			else if (Kind == EUeAgentValueKind::Array)
			{
				TArray<FUeAgentCanonicalValueRef> Items;
				bool Changed = false;
				for (const auto& Child : Value->GetArrayItems())
				{ const auto& Item = Converted.FindChecked(&Child.Get()); Changed |= Item.Get() != &Child.Get(); Items.Add(Item.ToSharedRef()); }
				if (Changed && !FUeAgentCanonicalValue::TryArray(MoveTemp(Items), Resources, Public, OutError)) return false;
			}
			else if (Kind == EUeAgentValueKind::Object)
			{
				FEvidenceFields Fields;
				bool Changed = false;
				for (const auto& Child : Value->GetObjectItems())
				{ const auto& Item = Converted.FindChecked(&Child.Value.Get()); Changed |= Item.Get() != &Child.Value.Get(); Fields.Emplace(Child.Key, Item.ToSharedRef()); }
				if (Changed && !FUeAgentCanonicalValue::TryObject(MoveTemp(Fields), Resources, Public, OutError)) return false;
			}
			Converted.Add(Value.Get(), MoveTemp(Public));
		}
		OutValue = Converted.FindChecked(Source.Get());
		return true;
	}

	bool TryCanonicalizeDifferenceEvidence(
		const FUeAgentCanonicalIdentity& Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError,
		EDifferenceEvidenceAudience Audience)
	{
		ResetEvidence(OutValue, OutError);
		if (!Source.IsValid())
		{
			return FailEvidence(
				OutValue,
				OutError,
				TEXT("canonical identity is invalid"));
		}
		FEvidenceBuilder Builder(Resources, OutError);
		FEvidenceFields Fields;
		if ((Audience == EDifferenceEvidenceAudience::Internal && !Builder.AddString(
				Fields,
				TEXT("semantic_id"),
				Source.GetSemanticId()))
			|| !Builder.AddString(
				Fields,
				TEXT("target_kind"),
				Source.GetTargetKind())
			|| (Audience == EDifferenceEvidenceAudience::Internal && !Builder.AddString(
				Fields,
				TEXT("owner_id"),
				Source.GetOwnerId()))
			|| (Audience == EDifferenceEvidenceAudience::Internal && !Builder.AddString(
				Fields,
				TEXT("selector"),
				Source.GetSelector()))
			|| !Builder.AddString(
				Fields,
				TEXT("object_role"),
				Source.GetObjectRole())
			|| (Audience == EDifferenceEvidenceAudience::Internal && !Builder.AddString(
				Fields,
				TEXT("generation"),
				Source.GetGeneration())))
		{
			return false;
		}
		if (Audience == EDifferenceEvidenceAudience::Public)
		{
			FEvidenceFields Selectors;
			for (const auto& Selector : Source.GetSelectors())
				if (!Builder.AddString(Selectors, Selector.Key, Selector.Value)) return false;
			FUeAgentCanonicalValuePtr PublicSelectors;
			if (!FUeAgentCanonicalValue::TryObject(MoveTemp(Selectors), Resources, PublicSelectors, OutError)) return false;
			Builder.AddValue(Fields, TEXT("selectors"), PublicSelectors.ToSharedRef());
		}
		return Builder.Finish(
			TEXT("canonical_identity"),
			MoveTemp(Fields),
			OutValue);
	}

	bool TryCanonicalizeDifferenceEvidence(
		const FUeAgentCanonicalType& Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError,
		EDifferenceEvidenceAudience Audience)
	{
		ResetEvidence(OutValue, OutError);
		const FString Kind = ValueKindName(Source.GetValueKind());
		if (!Source.IsValid() || Kind.IsEmpty())
		{
			return FailEvidence(
				OutValue,
				OutError,
				TEXT("canonical type is invalid"));
		}
		FEvidenceBuilder Builder(Resources, OutError);
		FEvidenceFields Fields;
		if (!Builder.AddString(
				Fields,
				TEXT("type_id"),
				Source.GetTypeId())
			|| !Builder.AddString(
				Fields,
				TEXT("kind"),
				Kind)
			|| !Builder.AddBoolean(
				Fields,
				TEXT("nullable"),
				Source.IsNullable())
			|| !Builder.AddString(
				Fields,
				TEXT("container_kind"),
				Source.GetContainerKind()))
		{
			return false;
		}
		return Builder.Finish(
			TEXT("canonical_type"),
			MoveTemp(Fields),
			OutValue);
	}

	bool TryCanonicalizeDifferenceEvidence(
		const FUeAgentDefaultValue& Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError,
		EDifferenceEvidenceAudience Audience)
	{
		ResetEvidence(OutValue, OutError);
		const FString SourceKind =
			DefaultSourceKindName(Source.GetSourceKind());
		if (!Source.IsValid() || SourceKind.IsEmpty())
		{
			return FailEvidence(
				OutValue,
				OutError,
				TEXT("default value is invalid"));
		}
		FEvidenceBuilder Builder(Resources, OutError);
		FEvidenceFields Fields;
		if (!Builder.AddString(
				Fields,
				TEXT("source_kind"),
				SourceKind)
			|| (Audience == EDifferenceEvidenceAudience::Internal && !Builder.AddString(
				Fields,
				TEXT("source_identity"),
				Source.GetSourceIdentity()))
			|| (Audience == EDifferenceEvidenceAudience::Internal && !Builder.AddString(
				Fields,
				TEXT("provenance"),
				Source.GetProvenance())))
		{
			return false;
		}
		FUeAgentCanonicalValuePtr BusinessValue;
		if (!TryCanonicalizeDifferenceEvidence(FUeAgentCanonicalValuePtr(Source.GetValue()), Resources, BusinessValue, OutError, Audience)) return false;
		Builder.AddValue(Fields, TEXT("value"), BusinessValue.ToSharedRef());
		return Builder.Finish(
			TEXT("default_value"),
			MoveTemp(Fields),
			OutValue);
	}

	bool TryCanonicalizeDifferenceEvidence(
		const FUeAgentStateDetails& Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError,
		EDifferenceEvidenceAudience Audience)
	{
		ResetEvidence(OutValue, OutError);
		if (!Source.IsValid())
		{
			return FailEvidence(
				OutValue,
				OutError,
				TEXT("state details are invalid"));
		}
		FEvidenceBuilder Builder(Resources, OutError);
		FEvidenceFields Fields;
		if (!Builder.AddString(
				Fields,
				TEXT("display_name"),
				Source.GetDisplayName())
			|| !Builder.AddString(
				Fields,
				TEXT("tooltip"),
				Source.GetTooltip())
			|| !Builder.AddStringArray(
				Fields,
				TEXT("category_path"),
				Source.GetCategoryPath())
			|| !Builder.AddBoolean(
				Fields,
				TEXT("editable"),
				Source.IsEditable())
			|| !Builder.AddString(
				Fields,
				TEXT("availability"),
				Source.GetAvailability()))
		{
			return false;
		}

		FEvidenceFields Constraints;
		for (const TPair<FString, FString>& Item :
			Source.GetConstraints())
		{
			if (!Builder.AddString(
				Constraints,
				Item.Key,
				Item.Value))
			{
				return false;
			}
		}
		FUeAgentCanonicalValuePtr CanonicalConstraints;
		if (!FUeAgentCanonicalValue::TryObject(
			Constraints,
			Resources,
			CanonicalConstraints,
			OutError))
		{
			return false;
		}
		Builder.AddValue(
			Fields,
			TEXT("constraints"),
			CanonicalConstraints.ToSharedRef());
		return Builder.Finish(
			TEXT("state_details"),
			MoveTemp(Fields),
			OutValue);
	}

	bool TryCanonicalizeDifferenceEvidence(
		const FUeAgentStateRevision& Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError,
		EDifferenceEvidenceAudience Audience)
	{
		ResetEvidence(OutValue, OutError);
		if (!Source.IsValid())
		{
			return FailEvidence(
				OutValue,
				OutError,
				TEXT("state revision is invalid"));
		}
		if (Audience == EDifferenceEvidenceAudience::Public) return true;
		FEvidenceBuilder Builder(Resources, OutError);
		FEvidenceFields Fields;
		if (!Builder.AddString(
				Fields,
				TEXT("source_id"),
				Source.GetSourceId())
			|| (Audience == EDifferenceEvidenceAudience::Internal && !Builder.AddString(
				Fields,
				TEXT("generation"),
				Source.GetGeneration()))
			|| !Builder.AddInteger(
				Fields,
				TEXT("sequence"),
				FString::Printf(
					TEXT("%lld"),
					static_cast<long long>(
						Source.GetSequence()))))
		{
			return false;
		}
		return Builder.Finish(
			TEXT("state_revision"),
			MoveTemp(Fields),
			OutValue);
	}

	bool TryCanonicalizeDifferenceEvidence(
		const FUeAgentTemporalPoint& Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError,
		EDifferenceEvidenceAudience Audience)
	{
		ResetEvidence(OutValue, OutError);
		if (!Source.IsValid())
		{
			return FailEvidence(
				OutValue,
				OutError,
				TEXT("temporal point is invalid"));
		}
		FEvidenceBuilder Builder(Resources, OutError);
		FEvidenceFields Fields;
		if ((Audience == EDifferenceEvidenceAudience::Internal && !Builder.AddString(
				Fields,
				TEXT("clock_id"),
				Source.GetClockId()))
			|| !Builder.AddInteger(
				Fields,
				TEXT("tick"),
				Source.GetTick())
			|| !Builder.AddInteger(
				Fields,
				TEXT("rate_numerator"),
				Source.GetRateNumerator())
			|| !Builder.AddInteger(
				Fields,
				TEXT("rate_denominator"),
				Source.GetRateDenominator())
			|| !Builder.AddString(
				Fields,
				TEXT("state"),
				Source.GetState()))
		{
			return false;
		}
		return Builder.Finish(
			TEXT("temporal_point"),
			MoveTemp(Fields),
			OutValue);
	}

	bool TryCanonicalizeDifferenceEvidence(
		const FUeAgentSpatialFrame& Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError,
		EDifferenceEvidenceAudience Audience)
	{
		ResetEvidence(OutValue, OutError);
		if (!Source.IsValid())
		{
			return FailEvidence(
				OutValue,
				OutError,
				TEXT("spatial frame is invalid"));
		}
		FEvidenceBuilder Builder(Resources, OutError);
		FEvidenceFields Fields;
		if ((Audience == EDifferenceEvidenceAudience::Internal && !Builder.AddString(
				Fields,
				TEXT("frame_id"),
				Source.GetFrameId()))
			|| (Audience == EDifferenceEvidenceAudience::Internal && !Builder.AddString(
				Fields,
				TEXT("basis_frame_id"),
				Source.GetBasisFrameId()))
			|| !Builder.AddInteger(
				Fields,
				TEXT("dimension"),
				FString::FromInt(Source.GetDimension()))
			|| !Builder.AddStringArray(
				Fields,
				TEXT("origin"),
				Source.GetOrigin())
			|| !Builder.AddString(
				Fields,
				TEXT("unit"),
				Source.GetUnit())
			|| (Audience == EDifferenceEvidenceAudience::Internal && !Builder.AddString(
				Fields,
				TEXT("basis_evidence"),
				Source.GetBasisEvidence())))
		{
			return false;
		}

		FUeAgentCanonicalValuePtr Basis;
		if (!BuildStringMatrix(
			Source.GetBasis(),
			Resources,
			Basis,
			OutError))
		{
			return false;
		}
		Builder.AddValue(
			Fields,
			TEXT("basis"),
			Basis.ToSharedRef());

		FUeAgentCanonicalValuePtr ObservedAt;
		if (!TryCanonicalizeDifferenceEvidence(
			Source.GetObservedAt(),
			Resources,
			ObservedAt,
			OutError, Audience))
		{
			return false;
		}
		Builder.AddValue(
			Fields,
			TEXT("observed_at"),
			ObservedAt.ToSharedRef());
		return Builder.Finish(
			TEXT("spatial_frame"),
			MoveTemp(Fields),
			OutValue);
	}

	bool TryCanonicalizeDifferenceEvidence(
		const FUeAgentSpatialValue& Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError,
		EDifferenceEvidenceAudience Audience)
	{
		ResetEvidence(OutValue, OutError);
		const FString QuantityKind =
			SpatialQuantityKindName(Source.GetQuantityKind());
		if (!Source.IsValid() || QuantityKind.IsEmpty())
		{
			return FailEvidence(
				OutValue,
				OutError,
				TEXT("spatial value is invalid"));
		}
		FEvidenceBuilder Builder(Resources, OutError);
		FEvidenceFields Fields;
		if (!Builder.AddString(
				Fields,
				TEXT("subject"),
				Source.GetSubject())
			|| !Builder.AddString(
				Fields,
				TEXT("quantity_kind"),
				QuantityKind)
			|| !Builder.AddStringArray(
				Fields,
				TEXT("components"),
				Source.GetComponents())
			|| (Audience == EDifferenceEvidenceAudience::Internal && !Builder.AddStringArray(
				Fields,
				TEXT("conversion_evidence"),
				Source.GetConversionEvidence())))
		{
			return false;
		}

		FUeAgentCanonicalValuePtr Frame;
		if (!TryCanonicalizeDifferenceEvidence(
			Source.GetFrame(),
			Resources,
			Frame,
			OutError, Audience))
		{
			return false;
		}
		Builder.AddValue(
			Fields,
			TEXT("frame"),
			Frame.ToSharedRef());

		FUeAgentCanonicalValuePtr ObservedAt;
		if (!TryCanonicalizeDifferenceEvidence(
			Source.GetObservedAt(),
			Resources,
			ObservedAt,
			OutError, Audience))
		{
			return false;
		}
		Builder.AddValue(
			Fields,
			TEXT("observed_at"),
			ObservedAt.ToSharedRef());
		return Builder.Finish(
			TEXT("spatial_value"),
			MoveTemp(Fields),
			OutValue);
	}

	bool TryCanonicalizeDifferenceEvidence(
		const FUeAgentStateEntry& Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError,
		EDifferenceEvidenceAudience Audience)
	{
		ResetEvidence(OutValue, OutError);
		if (!Source.IsValid())
		{
			return FailEvidence(
				OutValue,
				OutError,
				TEXT("state entry is invalid"));
		}
		FEvidenceBuilder Builder(Resources, OutError);
		FEvidenceFields Fields;

		if (!Builder.AddString(
			Fields,
			TEXT("authored_presence"),
			AuthoredPresenceName(Source.GetAuthoredPresence())))
		{
			return false;
		}

		if (Source.GetAuthoringDescriptor().IsValid())
			Builder.AddValue(Fields, TEXT("authoring_descriptor"), Source.GetAuthoringDescriptor().ToSharedRef());

		FUeAgentCanonicalValuePtr Identity;
		if (!TryCanonicalizeDifferenceEvidence(
			Source.GetIdentity(),
			Resources,
			Identity,
			OutError, Audience))
		{
			return false;
		}
		Builder.AddValue(
			Fields,
			TEXT("identity"),
			Identity.ToSharedRef());

		FUeAgentCanonicalValuePtr Type;
		if (!TryCanonicalizeDifferenceEvidence(
			Source.GetType(),
			Resources,
			Type,
			OutError, Audience))
		{
			return false;
		}
		Builder.AddValue(
			Fields,
			TEXT("type"),
			Type.ToSharedRef());
		FUeAgentCanonicalValuePtr BusinessValue;
		if (!TryCanonicalizeDifferenceEvidence(FUeAgentCanonicalValuePtr(Source.GetValue()), Resources, BusinessValue, OutError, Audience)) return false;
		Builder.AddValue(Fields, TEXT("value"), BusinessValue.ToSharedRef());

		FUeAgentCanonicalValuePtr Default;
		if (!TryCanonicalizeDifferenceEvidence(
			Source.GetDefault(),
			Resources,
			Default,
			OutError, Audience))
		{
			return false;
		}
		Builder.AddValue(
			Fields,
			TEXT("default"),
			Default.ToSharedRef());

		FUeAgentCanonicalValuePtr Details;
		if (!TryCanonicalizeDifferenceEvidence(
			Source.GetDetails(),
			Resources,
			Details,
			OutError, Audience))
		{
			return false;
		}
		Builder.AddValue(
			Fields,
			TEXT("details"),
			Details.ToSharedRef());

		if (Audience == EDifferenceEvidenceAudience::Internal)
		{
			FUeAgentCanonicalValuePtr Revision;
			if (!TryCanonicalizeDifferenceEvidence(
				Source.GetObservedRevision(),
				Resources,
				Revision,
				OutError, Audience))
			{
				return false;
			}
			Builder.AddValue(
				Fields,
				TEXT("observed_revision"),
				Revision.ToSharedRef());
		}
		if (!Builder.AddString(
			Fields,
			TEXT("override_state"),
			OverrideStateName(Source.GetOverrideState())))
		{
			return false;
		}
		return Builder.Finish(
			TEXT("state_entry"),
			MoveTemp(Fields),
			OutValue);
	}

	bool TryCanonicalizeDifferenceEvidence(
		EUeAgentAuthoredPresence Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError,
		EDifferenceEvidenceAudience Audience)
	{
		ResetEvidence(OutValue, OutError);
		const FString Presence = AuthoredPresenceName(Source);
		if (Presence.IsEmpty())
		{
			return FailEvidence(
				OutValue,
				OutError,
				TEXT("authored presence is invalid"));
		}
		FEvidenceBuilder Builder(Resources, OutError);
		FEvidenceFields Fields;
		if (!Builder.AddString(Fields, TEXT("value"), Presence))
		{
			return false;
		}
		return Builder.Finish(
			TEXT("authored_presence"),
			MoveTemp(Fields),
			OutValue);
	}

	bool TryCanonicalizeDifferenceEvidence(
		EUeAgentOverrideState Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError,
		EDifferenceEvidenceAudience Audience)
	{
		ResetEvidence(OutValue, OutError);
		const FString Override = OverrideStateName(Source);
		if (Override.IsEmpty())
		{
			return FailEvidence(
				OutValue,
				OutError,
				TEXT("override state is invalid"));
		}
		FEvidenceBuilder Builder(Resources, OutError);
		FEvidenceFields Fields;
		if (!Builder.AddString(Fields, TEXT("value"), Override))
		{
			return false;
		}
		return Builder.Finish(
			TEXT("override_state"),
			MoveTemp(Fields),
			OutValue);
	}

	bool TryCanonicalizeDifferenceEvidence(
		EUeAgentStatePlane Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError,
		EDifferenceEvidenceAudience Audience)
	{
		ResetEvidence(OutValue, OutError);
		const FString Plane = StatePlaneName(Source);
		if (Plane.IsEmpty())
		{
			return FailEvidence(
				OutValue,
				OutError,
				TEXT("state plane is invalid"));
		}
		FEvidenceBuilder Builder(Resources, OutError);
		FEvidenceFields Fields;
		if (!Builder.AddString(
			Fields,
			TEXT("value"),
			Plane))
		{
			return false;
		}
		return Builder.Finish(
			TEXT("state_plane"),
			MoveTemp(Fields),
			OutValue);
	}

	bool TryCanonicalizeDifferenceEvidence(
		const TArray<FUeAgentCanonicalValueRef>& Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError,
		EDifferenceEvidenceAudience Audience)
	{
		ResetEvidence(OutValue, OutError);
		FUeAgentCanonicalValuePtr Value;
		if (!FUeAgentCanonicalValue::TryArray(Source, Resources, Value, OutError)) return false;
		return TryCanonicalizeDifferenceEvidence(Value, Resources, OutValue, OutError, Audience);
	}

	bool TryCanonicalizeDifferenceEvidence(
		const TArray<TPair<FString, FUeAgentCanonicalValueRef>>& Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError,
		EDifferenceEvidenceAudience Audience)
	{
		ResetEvidence(OutValue, OutError);
		FUeAgentCanonicalValuePtr Value;
		if (!FUeAgentCanonicalValue::TryObject(Source, Resources, Value, OutError)) return false;
		return TryCanonicalizeDifferenceEvidence(Value, Resources, OutValue, OutError, Audience);
	}
}
