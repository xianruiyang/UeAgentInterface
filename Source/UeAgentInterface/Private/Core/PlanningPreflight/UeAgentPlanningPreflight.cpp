#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"

#include "Containers/Set.h"
#include "Misc/Char.h"
#include "Templates/SharedPointer.h"

#include <openssl/sha.h>

namespace UE::UAI::PlanningPreflight
{
	using UE::UAI::ContextResource::EUeAgentResourceDimension;
	using UE::UAI::ContextResource::FUeAgentResourceBudgetExceeded;
	using UE::UAI::ContextResource::MeasureText;
	using UE::UAI::StateSemantic::EUeAgentSpatialQuantityKind;
	using UE::UAI::StateSemantic::EUeAgentValueKind;
	using UE::UAI::StateSemantic::FUeAgentCanonicalValue;
	using UE::UAI::StateSemantic::FUeAgentCanonicalValueRef;
	using UE::UAI::StateSemantic::FUeAgentSemanticComparison;
	using UE::UAI::StateSemantic::FUeAgentStateEntry;
	using UE::UAI::StateSemantic::FUeAgentStateSemanticError;
	using UE::UAI::StateSemantic::TryCompareValues;

	namespace
	{
		TArray<FString> NormalizeStrings(TArray<FString> Values)
		{
			for (FString& Value : Values)
			{
				Value.TrimStartAndEndInline();
			}
			Values.RemoveAll([](const FString& Value)
			{
				return Value.IsEmpty();
			});
			Values.Sort();
			for (int32 Index = Values.Num() - 1; Index > 0; --Index)
			{
				if (Values[Index] == Values[Index - 1])
				{
					Values.RemoveAt(Index, 1, EAllowShrinking::No);
				}
			}
			return Values;
		}

		FPlanningPreflightError MakeResourceError(
			const FUeAgentResourceBudgetExceeded& Error)
		{
			return FPlanningPreflightError(
				TEXT("resource_budget_exceeded"),
				FString::Printf(
					TEXT("%s exceeded at %s: observed=%lld limit=%lld"),
					*UE::UAI::ContextResource::LexToString(Error.Dimension),
					*Error.Path,
					Error.Observed,
					Error.Limit));
		}

		FPlanningPreflightError MakeStateError(
			const FUeAgentStateSemanticError& Error,
			const FString& IdentityKey)
		{
			return FPlanningPreflightError(
				Error.Code.IsEmpty()
					? TEXT("planning_state_semantic_error")
					: Error.Code,
				Error.Detail.IsEmpty() ? Error.Path : Error.Detail,
				IdentityKey);
		}

		bool TryCharge(
			FUeAgentResourceTracker& Resources,
			const FString& Path,
			const int64 Nodes,
			const int64 Items,
			const int64 StringBytes,
			FPlanningPreflightError& OutError)
		{
			FUeAgentResourceBudgetExceeded Error;
			if (!Resources.TryChargeMany(
				{
					{EUeAgentResourceDimension::Nodes, Nodes},
					{EUeAgentResourceDimension::Items, Items},
					{EUeAgentResourceDimension::StringBytes, StringBytes},
					{EUeAgentResourceDimension::WorkUnits, Nodes + Items},
					{
						EUeAgentResourceDimension::EstimatedMemoryBytes,
						Nodes * 64 + Items * 24 + StringBytes
					},
				},
				Path,
				Error))
			{
				OutError = MakeResourceError(Error);
				return false;
			}
			return true;
		}

		bool TryEquivalent(
			const FUeAgentCanonicalValuePtr& Left,
			const FUeAgentCanonicalValuePtr& Right,
			const FString& IdentityKey,
			FUeAgentResourceTracker& Resources,
			bool& bOutEquivalent,
			FPlanningPreflightError& OutError)
		{
			if (!Left.IsValid() || !Right.IsValid())
			{
				bOutEquivalent = Left.IsValid() == Right.IsValid();
				return true;
			}
			FUeAgentSemanticComparison Comparison;
			FUeAgentStateSemanticError Error;
			if (!TryCompareValues(
				Left.ToSharedRef(),
				Right.ToSharedRef(),
				TEXT("/"),
				Resources,
				Comparison,
				Error))
			{
				OutError = MakeStateError(Error, IdentityKey);
				return false;
			}
			if (!TryCharge(
				Resources,
				TEXT("/planning/comparison"),
				1,
				Comparison.GetDifferences().Num(),
				MeasureText(IdentityKey).Utf8Bytes,
				OutError))
			{
				return false;
			}
			bOutEquivalent = Comparison.IsEquivalent();
			return true;
		}

		FString ValueKindToString(const EUeAgentValueKind Kind)
		{
			switch (Kind)
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
			default: return TEXT("unknown");
			}
		}

		FString QuantityKindToString(
			const EUeAgentSpatialQuantityKind Kind)
		{
			switch (Kind)
			{
			case EUeAgentSpatialQuantityKind::Point: return TEXT("point");
			case EUeAgentSpatialQuantityKind::Direction:
				return TEXT("direction");
			case EUeAgentSpatialQuantityKind::Normal: return TEXT("normal");
			case EUeAgentSpatialQuantityKind::Transform:
				return TEXT("transform");
			default: return TEXT("unknown");
			}
		}

		class FFingerprintWriter final
		{
		public:
			explicit FFingerprintWriter(FUeAgentResourceTracker& InResources)
				: Resources(InResources)
			{
			}

			bool Text(
				const FString& Label,
				const FString& Value,
				FPlanningPreflightError& OutError)
			{
				const FTCHARToUTF8 LabelUtf8(*Label);
				const FTCHARToUTF8 ValueUtf8(*Value);
				const int64 ByteCount =
					LabelUtf8.Length() + ValueUtf8.Length();
				FUeAgentResourceBudgetExceeded Error;
				if (!Resources.TryChargeMany(
					{
						{
							EUeAgentResourceDimension::InputBytes,
							ByteCount
						},
						{
							EUeAgentResourceDimension::StringBytes,
							ByteCount
						},
						{EUeAgentResourceDimension::WorkUnits, 1},
						{
							EUeAgentResourceDimension::EstimatedMemoryBytes,
							16 + ByteCount
						},
					},
					TEXT("/planning/fingerprint"),
					Error))
				{
					OutError = MakeResourceError(Error);
					return false;
				}
				AppendUInt32(static_cast<uint32>(LabelUtf8.Length()));
				Bytes.Append(
					reinterpret_cast<const uint8*>(LabelUtf8.Get()),
					LabelUtf8.Length());
				AppendUInt64(static_cast<uint64>(ValueUtf8.Length()));
				Bytes.Append(
					reinterpret_cast<const uint8*>(ValueUtf8.Get()),
					ValueUtf8.Length());
				return true;
			}

			bool Count(
				const FString& Label,
				const int64 Value,
				FPlanningPreflightError& OutError)
			{
				return Text(Label, FString::Printf(TEXT("%lld"), Value), OutError);
			}

			bool Identity(
				const FString& Label,
				const FUeAgentCanonicalIdentity& Value,
				FPlanningPreflightError& OutError)
			{
				return Text(Label + TEXT(".semantic_id"), Value.GetSemanticId(), OutError)
					&& Text(Label + TEXT(".target_kind"), Value.GetTargetKind(), OutError)
					&& Text(Label + TEXT(".owner_id"), Value.GetOwnerId(), OutError)
					&& Text(Label + TEXT(".selector"), Value.GetSelector(), OutError)
					&& Text(Label + TEXT(".object_role"), Value.GetObjectRole(), OutError)
					&& Text(Label + TEXT(".generation"), Value.GetGeneration(), OutError);
			}

			bool Revision(
				const FString& Label,
				const FUeAgentStateRevision& Value,
				FPlanningPreflightError& OutError)
			{
				return Text(Label + TEXT(".source_id"), Value.GetSourceId(), OutError)
					&& Text(Label + TEXT(".generation"), Value.GetGeneration(), OutError)
					&& Count(Label + TEXT(".sequence"), Value.GetSequence(), OutError);
			}

			bool Value(
				const FString& Label,
				const FUeAgentCanonicalValuePtr& Root,
				FPlanningPreflightError& OutError)
			{
				if (!Root.IsValid())
				{
					return Text(Label + TEXT(".present"), TEXT("false"), OutError);
				}
				if (!Text(Label + TEXT(".present"), TEXT("true"), OutError))
				{
					return false;
				}

				enum class ETaskKind : uint8
				{
					Visit,
					Key,
				};
				struct FTask
				{
					ETaskKind Kind = ETaskKind::Visit;
					FUeAgentCanonicalValuePtr Node;
					FString Key;
				};

				const FString NodeLabel = Label + TEXT(".node");
				TArray<FTask> Tasks;
				Tasks.Add({ETaskKind::Visit, Root, FString()});
				while (!Tasks.IsEmpty())
				{
					FTask Task = MoveTemp(Tasks.Last());
					Tasks.Pop(EAllowShrinking::No);
					if (Task.Kind == ETaskKind::Key)
					{
						if (!Text(NodeLabel + TEXT(".key"), Task.Key, OutError))
						{
							return false;
						}
						continue;
					}
					if (!Task.Node.IsValid())
					{
						OutError = FPlanningPreflightError(
							TEXT("planning_fingerprint_value_invalid"));
						return false;
					}

					const EUeAgentValueKind Kind = Task.Node->GetKind();
					int64 ChildCount = 0;
					if (Kind == EUeAgentValueKind::Array)
					{
						ChildCount = Task.Node->GetArrayItems().Num();
					}
					else if (Kind == EUeAgentValueKind::Object)
					{
						ChildCount = Task.Node->GetObjectItems().Num();
					}
					if (!TryCharge(
						Resources,
						TEXT("/planning/fingerprint/value"),
						1,
						ChildCount,
						0,
						OutError)
						|| !Text(
							NodeLabel + TEXT(".kind"),
							ValueKindToString(Kind),
							OutError))
					{
						return false;
					}

					switch (Kind)
					{
					case EUeAgentValueKind::Null:
						break;
					case EUeAgentValueKind::Boolean:
						if (!Text(
							NodeLabel + TEXT(".boolean"),
							Task.Node->GetBoolean()
								? TEXT("true")
								: TEXT("false"),
							OutError))
						{
							return false;
						}
						break;
					case EUeAgentValueKind::String:
						if (!Text(
							NodeLabel + TEXT(".scalar"),
							Task.Node->GetText(),
							OutError))
						{
							return false;
						}
						break;
					case EUeAgentValueKind::Integer:
					case EUeAgentValueKind::Decimal:
						if (!Text(
							NodeLabel + TEXT(".scalar"),
							Task.Node->GetLexeme(),
							OutError))
						{
							return false;
						}
						break;
					case EUeAgentValueKind::Array:
					{
						const auto& Items = Task.Node->GetArrayItems();
						if (!Count(
							NodeLabel + TEXT(".count"),
							Items.Num(),
							OutError))
						{
							return false;
						}
						for (int32 Index = Items.Num() - 1; Index >= 0; --Index)
						{
							Tasks.Add({
								ETaskKind::Visit,
								Items[Index],
								FString()
							});
						}
						break;
					}
					case EUeAgentValueKind::Object:
					{
						const auto& Items = Task.Node->GetObjectItems();
						if (!Count(
							NodeLabel + TEXT(".count"),
							Items.Num(),
							OutError))
						{
							return false;
						}
						for (int32 Index = Items.Num() - 1; Index >= 0; --Index)
						{
							Tasks.Add({
								ETaskKind::Visit,
								Items[Index].Value,
								FString()
							});
							Tasks.Add({
								ETaskKind::Key,
								nullptr,
								Items[Index].Key
							});
						}
						break;
					}
					case EUeAgentValueKind::Temporal:
					{
						const auto& Temporal = Task.Node->GetTemporal();
						if (!Temporal.IsValid())
						{
							OutError = FPlanningPreflightError(
								TEXT("planning_fingerprint_temporal_invalid"));
							return false;
						}
						if (!WriteTemporal(
							NodeLabel + TEXT("."),
							*Temporal,
							OutError))
						{
							return false;
						}
						break;
					}
					case EUeAgentValueKind::Spatial:
					{
						const auto& Spatial = Task.Node->GetSpatial();
						if (!Spatial.IsValid())
						{
							OutError = FPlanningPreflightError(
								TEXT("planning_fingerprint_spatial_invalid"));
							return false;
						}
						const auto& Frame = Spatial->GetFrame();
						if (!Text(
								NodeLabel + TEXT(".subject"),
								Spatial->GetSubject(),
								OutError)
							|| !Text(
								NodeLabel + TEXT(".quantity_kind"),
								QuantityKindToString(
									Spatial->GetQuantityKind()),
								OutError)
							|| !Text(
								NodeLabel + TEXT(".frame.frame_id"),
								Frame.GetFrameId(),
								OutError)
							|| !Text(
								NodeLabel + TEXT(".frame.basis_frame_id"),
								Frame.GetBasisFrameId(),
								OutError)
							|| !Count(
								NodeLabel + TEXT(".frame.dimension"),
								Frame.GetDimension(),
								OutError)
							|| !Count(
								NodeLabel + TEXT(".frame.basis.rows"),
								Frame.GetBasis().Num(),
								OutError))
						{
							return false;
						}
						for (const TArray<FString>& Row : Frame.GetBasis())
						{
							if (!Count(
								NodeLabel + TEXT(".frame.basis.row.count"),
								Row.Num(),
								OutError))
							{
								return false;
							}
							for (const FString& Component : Row)
							{
								if (!Text(
									NodeLabel
										+ TEXT(".frame.basis.component"),
									Component,
									OutError))
								{
									return false;
								}
							}
						}
						if (!Count(
							NodeLabel + TEXT(".frame.origin.count"),
							Frame.GetOrigin().Num(),
							OutError))
						{
							return false;
						}
						for (const FString& Component : Frame.GetOrigin())
						{
							if (!Text(
								NodeLabel + TEXT(".frame.origin.component"),
								Component,
								OutError))
							{
								return false;
							}
						}
						if (!Text(
								NodeLabel + TEXT(".frame.unit"),
								Frame.GetUnit(),
								OutError)
							|| !Text(
								NodeLabel + TEXT(".frame.basis_evidence"),
								Frame.GetBasisEvidence(),
								OutError)
							|| !WriteTemporal(
								NodeLabel + TEXT(".observed_at."),
								Spatial->GetObservedAt(),
								OutError)
							|| !Count(
								NodeLabel + TEXT(".components.count"),
								Spatial->GetComponents().Num(),
								OutError))
						{
							return false;
						}
						for (const FString& Component : Spatial->GetComponents())
						{
							if (!Text(
								NodeLabel + TEXT(".components.component"),
								Component,
								OutError))
							{
								return false;
							}
						}
						if (!Count(
							NodeLabel
								+ TEXT(".conversion_evidence.count"),
							Spatial->GetConversionEvidence().Num(),
							OutError))
						{
							return false;
						}
						for (const FString& Evidence :
							Spatial->GetConversionEvidence())
						{
							if (!Text(
								NodeLabel
									+ TEXT(".conversion_evidence.item"),
								Evidence,
								OutError))
							{
								return false;
							}
						}
						break;
					}
					default:
						OutError = FPlanningPreflightError(
							TEXT(
								"planning_fingerprint_value_kind_unknown"));
						return false;
					}
				}
				return true;
			}

			bool Finish(
				FString& OutFingerprint,
				FPlanningPreflightError& OutError)
			{
				uint8 Digest[SHA256_DIGEST_LENGTH] = {};
				SHA256(Bytes.GetData(), Bytes.Num(), Digest);
				FString Hex;
				Hex.Reserve(SHA256_DIGEST_LENGTH * 2);
				for (const uint8 Byte : Digest)
				{
					Hex += FString::Printf(TEXT("%02x"), Byte);
				}
				OutFingerprint = TEXT("sha256:") + Hex;

				FUeAgentResourceBudgetExceeded Error;
				if (!Resources.TryChargeMany(
					{
						{
							EUeAgentResourceDimension::OutputBytes,
							MeasureText(OutFingerprint).Utf8Bytes
						},
						{EUeAgentResourceDimension::WorkUnits, 1},
					},
					TEXT("/planning/fingerprint/result"),
					Error))
				{
					OutError = MakeResourceError(Error);
					OutFingerprint.Reset();
					return false;
				}
				return true;
			}

		private:
			void AppendUInt32(const uint32 Value)
			{
				for (int32 Shift = 24; Shift >= 0; Shift -= 8)
				{
					Bytes.Add(static_cast<uint8>(Value >> Shift));
				}
			}

			void AppendUInt64(const uint64 Value)
			{
				for (int32 Shift = 56; Shift >= 0; Shift -= 8)
				{
					Bytes.Add(static_cast<uint8>(Value >> Shift));
				}
			}

			bool WriteTemporal(
				const FString& Prefix,
				const UE::UAI::StateSemantic::FUeAgentTemporalPoint& Value,
				FPlanningPreflightError& OutError)
			{
				return Text(
						Prefix + TEXT("clock_id"),
						Value.GetClockId(),
						OutError)
					&& Text(
						Prefix + TEXT("tick"),
						Value.GetTick(),
						OutError)
					&& Text(
						Prefix + TEXT("rate_numerator"),
						Value.GetRateNumerator(),
						OutError)
					&& Text(
						Prefix + TEXT("rate_denominator"),
						Value.GetRateDenominator(),
						OutError)
					&& Text(
						Prefix + TEXT("state"),
						Value.GetState(),
						OutError);
			}

			FUeAgentResourceTracker& Resources;
			TArray<uint8> Bytes;
		};

		const FUeAgentStateEntry* FindEntry(
			const FUeAgentCanonicalSnapshotRef& Snapshot,
			const FString& CanonicalKey)
		{
			return Snapshot->GetEntries().FindByPredicate(
				[&CanonicalKey](const FUeAgentStateEntry& Entry)
				{
					return Entry.GetIdentity().GetCanonicalKey()
						== CanonicalKey;
				});
		}
	}

	FPlanningPreflightError::FPlanningPreflightError(
		FString InCode,
		FString InDetail,
		FString InIdentityKey)
		: Code(MoveTemp(InCode))
		, Detail(MoveTemp(InDetail))
		, IdentityKey(MoveTemp(InIdentityKey))
	{
		Code.TrimStartAndEndInline();
		Detail.TrimStartAndEndInline();
		IdentityKey.TrimStartAndEndInline();
	}

	const FString& FPlanningPreflightError::GetCode() const { return Code; }
	const FString& FPlanningPreflightError::GetDetail() const { return Detail; }
	const FString& FPlanningPreflightError::GetIdentityKey() const
	{
		return IdentityKey;
	}
	bool FPlanningPreflightError::IsPresent() const { return !Code.IsEmpty(); }

	FString LexToString(const EChangeIntent Intent)
	{
		switch (Intent)
		{
		case EChangeIntent::Create: return TEXT("create");
		case EChangeIntent::Update: return TEXT("update");
		case EChangeIntent::Replace: return TEXT("replace");
		case EChangeIntent::Delete: return TEXT("delete");
		default: return TEXT("unknown");
		}
	}

	FChangeDraft::FChangeDraft(
		FUeAgentCanonicalIdentity InIdentity,
		const EChangeIntent InIntent,
		FUeAgentCanonicalValuePtr InExpectedBefore,
		FUeAgentCanonicalValuePtr InProposedAfter,
		TArray<FString> InRequiredAuthorityScopes,
		FString InImpactEvidenceId,
		TArray<FUeAgentCanonicalIdentity> InImpactTargets)
		: Identity(MoveTemp(InIdentity))
		, Intent(InIntent)
		, ExpectedBefore(MoveTemp(InExpectedBefore))
		, ProposedAfter(MoveTemp(InProposedAfter))
		, RequiredAuthorityScopes(
			NormalizeStrings(MoveTemp(InRequiredAuthorityScopes)))
		, ImpactEvidenceId(MoveTemp(InImpactEvidenceId))
		, ImpactTargets(MoveTemp(InImpactTargets))
	{
		ImpactEvidenceId.TrimStartAndEndInline();
		ImpactTargets.Sort(
			[](const FUeAgentCanonicalIdentity& Left,
				const FUeAgentCanonicalIdentity& Right)
			{
				return Left.GetCanonicalKey() < Right.GetCanonicalKey();
			});
		const bool bValuesValid =
			(Intent == EChangeIntent::Create
				&& !ExpectedBefore.IsValid()
				&& ProposedAfter.IsValid())
			|| (Intent == EChangeIntent::Delete
				&& ExpectedBefore.IsValid()
				&& !ProposedAfter.IsValid())
			|| ((Intent == EChangeIntent::Update
					|| Intent == EChangeIntent::Replace)
				&& ExpectedBefore.IsValid()
				&& ProposedAfter.IsValid());
		bool bImpactTargetsValid = true;
		for (const FUeAgentCanonicalIdentity& Target : ImpactTargets)
		{
			bImpactTargetsValid = bImpactTargetsValid && Target.IsValid();
		}
		bValid = Identity.IsValid()
			&& bValuesValid
			&& !ImpactEvidenceId.IsEmpty()
			&& bImpactTargetsValid;
	}

	const FUeAgentCanonicalIdentity& FChangeDraft::GetIdentity() const
	{
		return Identity;
	}
	EChangeIntent FChangeDraft::GetIntent() const { return Intent; }
	const FUeAgentCanonicalValuePtr& FChangeDraft::GetExpectedBefore() const
	{
		return ExpectedBefore;
	}
	const FUeAgentCanonicalValuePtr& FChangeDraft::GetProposedAfter() const
	{
		return ProposedAfter;
	}
	const TArray<FString>& FChangeDraft::GetRequiredAuthorityScopes() const
	{
		return RequiredAuthorityScopes;
	}
	const FString& FChangeDraft::GetImpactEvidenceId() const
	{
		return ImpactEvidenceId;
	}
	const TArray<FUeAgentCanonicalIdentity>& FChangeDraft::GetImpactTargets() const
	{
		return ImpactTargets;
	}
	bool FChangeDraft::IsValid() const { return bValid; }

	FChangeSetDraft::FChangeSetDraft(
		FUeAgentDescriptorIdentity InDescriptorIdentity,
		FString InRequestId,
		FString InIdempotencyKey,
		FUeAgentCanonicalSnapshotRef InSnapshot,
		TArray<FChangeDraft> InChanges)
		: DescriptorIdentity(MoveTemp(InDescriptorIdentity))
		, RequestId(MoveTemp(InRequestId))
		, IdempotencyKey(MoveTemp(InIdempotencyKey))
		, Snapshot(MoveTemp(InSnapshot))
		, Changes(MoveTemp(InChanges))
	{
		RequestId.TrimStartAndEndInline();
		IdempotencyKey.TrimStartAndEndInline();
		bool bChangesValid = true;
		for (const FChangeDraft& Change : Changes)
		{
			bChangesValid = bChangesValid && Change.IsValid();
		}
		bValid = DescriptorIdentity.IsValid()
			&& !RequestId.IsEmpty()
			&& !IdempotencyKey.IsEmpty()
			&& !Changes.IsEmpty()
			&& bChangesValid;
	}

	const FUeAgentDescriptorIdentity& FChangeSetDraft::GetDescriptorIdentity() const
	{
		return DescriptorIdentity;
	}
	const FString& FChangeSetDraft::GetRequestId() const { return RequestId; }
	const FString& FChangeSetDraft::GetIdempotencyKey() const
	{
		return IdempotencyKey;
	}
	const FUeAgentCanonicalSnapshotRef& FChangeSetDraft::GetSnapshot() const
	{
		return Snapshot;
	}
	const TArray<FChangeDraft>& FChangeSetDraft::GetChanges() const
	{
		return Changes;
	}
	bool FChangeSetDraft::IsValid() const { return bValid; }

	FPlanningRequest::FPlanningRequest(
		FChangeSetDraft InDraft,
		FUeAgentCallerContext InCallerContext,
		const bool bInDryRun)
		: Draft(MoveTemp(InDraft))
		, CallerContext(MoveTemp(InCallerContext))
		, bDryRun(bInDryRun)
	{
		bValid = Draft.IsValid() && CallerContext.IsValid();
	}

	const FChangeSetDraft& FPlanningRequest::GetDraft() const { return Draft; }
	const FUeAgentCallerContext& FPlanningRequest::GetCallerContext() const
	{
		return CallerContext;
	}
	bool FPlanningRequest::IsDryRun() const { return bDryRun; }
	bool FPlanningRequest::IsValid() const { return bValid; }

	FPlannedChange::FPlannedChange(
		FUeAgentCanonicalIdentity InIdentity,
		const EChangeIntent InIntent,
		FUeAgentCanonicalValuePtr InBefore,
		FUeAgentCanonicalValuePtr InAfter,
		TArray<FString> InRequiredAuthorityScopes,
		FString InImpactEvidenceId)
		: Identity(MoveTemp(InIdentity))
		, Intent(InIntent)
		, Before(MoveTemp(InBefore))
		, After(MoveTemp(InAfter))
		, RequiredAuthorityScopes(
			NormalizeStrings(MoveTemp(InRequiredAuthorityScopes)))
		, ImpactEvidenceId(MoveTemp(InImpactEvidenceId))
	{
		ImpactEvidenceId.TrimStartAndEndInline();
	}

	const FUeAgentCanonicalIdentity& FPlannedChange::GetIdentity() const
	{
		return Identity;
	}
	EChangeIntent FPlannedChange::GetIntent() const { return Intent; }
	const FUeAgentCanonicalValuePtr& FPlannedChange::GetBefore() const
	{
		return Before;
	}
	const FUeAgentCanonicalValuePtr& FPlannedChange::GetAfter() const
	{
		return After;
	}
	const TArray<FString>& FPlannedChange::GetRequiredAuthorityScopes() const
	{
		return RequiredAuthorityScopes;
	}
	const FString& FPlannedChange::GetImpactEvidenceId() const
	{
		return ImpactEvidenceId;
	}

	FChangeSet::FChangeSet(TArray<FPlannedChange> InChanges)
		: Changes(MoveTemp(InChanges))
	{
	}
	const TArray<FPlannedChange>& FChangeSet::GetChanges() const
	{
		return Changes;
	}

	FImpact::FImpact(
		FUeAgentCanonicalIdentity InIdentity,
		FUeAgentCanonicalIdentity InSourceIdentity,
		FString InKind,
		FString InEvidenceId)
		: Identity(MoveTemp(InIdentity))
		, SourceIdentity(MoveTemp(InSourceIdentity))
		, Kind(MoveTemp(InKind))
		, EvidenceId(MoveTemp(InEvidenceId))
	{
		Kind.TrimStartAndEndInline();
		EvidenceId.TrimStartAndEndInline();
	}
	const FUeAgentCanonicalIdentity& FImpact::GetIdentity() const
	{
		return Identity;
	}
	const FUeAgentCanonicalIdentity& FImpact::GetSourceIdentity() const
	{
		return SourceIdentity;
	}
	const FString& FImpact::GetKind() const { return Kind; }
	const FString& FImpact::GetEvidenceId() const { return EvidenceId; }

	FImpactSet::FImpactSet(TArray<FImpact> InImpacts)
		: Impacts(MoveTemp(InImpacts))
	{
	}
	const TArray<FImpact>& FImpactSet::GetImpacts() const { return Impacts; }

	FPrecondition::FPrecondition(
		FUeAgentCanonicalIdentity InIdentity,
		FUeAgentStateRevision InObservedRevision,
		FString InExistence,
		FUeAgentCanonicalValuePtr InExpectedBefore)
		: Identity(MoveTemp(InIdentity))
		, ObservedRevision(MoveTemp(InObservedRevision))
		, Existence(MoveTemp(InExistence))
		, ExpectedBefore(MoveTemp(InExpectedBefore))
	{
		Existence.TrimStartAndEndInline();
	}
	const FUeAgentCanonicalIdentity& FPrecondition::GetIdentity() const
	{
		return Identity;
	}
	const FUeAgentStateRevision& FPrecondition::GetObservedRevision() const
	{
		return ObservedRevision;
	}
	const FString& FPrecondition::GetExistence() const { return Existence; }
	const FUeAgentCanonicalValuePtr& FPrecondition::GetExpectedBefore() const
	{
		return ExpectedBefore;
	}

	FPlanningPreflightPlan::FPlanningPreflightPlan(
		FUeAgentDescriptorIdentity InDescriptorIdentity,
		FString InRequestId,
		FString InIdempotencyKey,
		FString InCallerId,
		TArray<FString> InAuthorityScopes,
		FString InSnapshotId,
		FUeAgentStateRevision InObservedRevision,
		FString InSnapshotEvidenceId,
		FChangeSet InChangeSet,
		FImpactSet InImpactSet,
		TArray<FPrecondition> InPreconditions,
		FString InFingerprint)
		: DescriptorIdentity(MoveTemp(InDescriptorIdentity))
		, RequestId(MoveTemp(InRequestId))
		, IdempotencyKey(MoveTemp(InIdempotencyKey))
		, CallerId(MoveTemp(InCallerId))
		, AuthorityScopes(NormalizeStrings(MoveTemp(InAuthorityScopes)))
		, SnapshotId(MoveTemp(InSnapshotId))
		, ObservedRevision(MoveTemp(InObservedRevision))
		, SnapshotEvidenceId(MoveTemp(InSnapshotEvidenceId))
		, ChangeSet(MoveTemp(InChangeSet))
		, ImpactSet(MoveTemp(InImpactSet))
		, Preconditions(MoveTemp(InPreconditions))
		, Fingerprint(MoveTemp(InFingerprint))
	{
	}

	const FUeAgentDescriptorIdentity&
	FPlanningPreflightPlan::GetDescriptorIdentity() const
	{
		return DescriptorIdentity;
	}
	const FString& FPlanningPreflightPlan::GetRequestId() const
	{
		return RequestId;
	}
	const FString& FPlanningPreflightPlan::GetIdempotencyKey() const
	{
		return IdempotencyKey;
	}
	const FString& FPlanningPreflightPlan::GetCallerId() const
	{
		return CallerId;
	}
	const TArray<FString>& FPlanningPreflightPlan::GetAuthorityScopes() const
	{
		return AuthorityScopes;
	}
	const FString& FPlanningPreflightPlan::GetSnapshotId() const
	{
		return SnapshotId;
	}
	const FUeAgentStateRevision&
	FPlanningPreflightPlan::GetObservedRevision() const
	{
		return ObservedRevision;
	}
	const FString& FPlanningPreflightPlan::GetSnapshotEvidenceId() const
	{
		return SnapshotEvidenceId;
	}
	const FChangeSet& FPlanningPreflightPlan::GetChangeSet() const
	{
		return ChangeSet;
	}
	const FImpactSet& FPlanningPreflightPlan::GetImpactSet() const
	{
		return ImpactSet;
	}
	const TArray<FPrecondition>&
	FPlanningPreflightPlan::GetPreconditions() const
	{
		return Preconditions;
	}
	const FString& FPlanningPreflightPlan::GetFingerprint() const
	{
		return Fingerprint;
	}
	FPlanningPreflightPlanRef FPlanningPreflightPlan::WithFingerprint(
		FString InFingerprint) const
	{
		return MakeShared<FPlanningPreflightPlan>(
			DescriptorIdentity,
			RequestId,
			IdempotencyKey,
			CallerId,
			AuthorityScopes,
			SnapshotId,
			ObservedRevision,
			SnapshotEvidenceId,
			ChangeSet,
			ImpactSet,
			Preconditions,
			MoveTemp(InFingerprint));
	}

	FPlanningResult::FPlanningResult(
		FPlanningPreflightPlanPtr InPlan,
		const bool bInDryRun,
		const bool bInMutationPermitted,
		FString InStatus)
		: Plan(MoveTemp(InPlan))
		, bDryRun(bInDryRun)
		, bMutationPermitted(bInMutationPermitted)
		, Status(MoveTemp(InStatus))
	{
		Status.TrimStartAndEndInline();
	}
	const FPlanningPreflightPlanPtr& FPlanningResult::GetPlan() const
	{
		return Plan;
	}
	bool FPlanningResult::IsDryRun() const { return bDryRun; }
	bool FPlanningResult::IsMutationPermitted() const
	{
		return bMutationPermitted;
	}
	const FString& FPlanningResult::GetStatus() const { return Status; }
	bool FPlanningResult::IsReady() const
	{
		return Plan.IsValid()
			&& (Status == TEXT("ready") || Status == TEXT("no_change"));
	}

	bool TryComputePlanFingerprint(
		const FPlanningPreflightPlanRef& Plan,
		FUeAgentResourceTracker& Resources,
		FString& OutFingerprint,
		FPlanningPreflightError& OutError)
	{
		OutFingerprint.Reset();
		OutError = FPlanningPreflightError();
		FFingerprintWriter Writer(Resources);
		const FUeAgentDescriptorIdentity& Descriptor =
			Plan->GetDescriptorIdentity();
		if (!Writer.Text(
				TEXT("schema"),
				TEXT("uai.c6.planning_preflight.plan.v1"),
				OutError)
			|| !Writer.Text(
				TEXT("descriptor.id"),
				Descriptor.GetDescriptorId(),
				OutError)
			|| !Writer.Count(
				TEXT("descriptor.version"),
				Descriptor.GetVersion(),
				OutError)
			|| !Writer.Text(
				TEXT("descriptor.hash"),
				Descriptor.GetDescriptorHash(),
				OutError)
			|| !Writer.Text(TEXT("request_id"), Plan->GetRequestId(), OutError)
			|| !Writer.Text(
				TEXT("idempotency_key"),
				Plan->GetIdempotencyKey(),
				OutError)
			|| !Writer.Text(TEXT("caller_id"), Plan->GetCallerId(), OutError)
			|| !Writer.Count(
				TEXT("authority_scopes.count"),
				Plan->GetAuthorityScopes().Num(),
				OutError))
		{
			return false;
		}
		for (int32 Index = 0;
			Index < Plan->GetAuthorityScopes().Num();
			++Index)
		{
			if (!Writer.Text(
				FString::Printf(TEXT("authority_scopes[%d]"), Index),
				Plan->GetAuthorityScopes()[Index],
				OutError))
			{
				return false;
			}
		}
		if (!Writer.Text(
				TEXT("snapshot.id"),
				Plan->GetSnapshotId(),
				OutError)
			|| !Writer.Revision(
				TEXT("snapshot.revision"),
				Plan->GetObservedRevision(),
				OutError)
			|| !Writer.Text(
				TEXT("snapshot.evidence"),
				Plan->GetSnapshotEvidenceId(),
				OutError))
		{
			return false;
		}

		const auto& Changes = Plan->GetChangeSet().GetChanges();
		if (!Writer.Count(TEXT("changes.count"), Changes.Num(), OutError))
		{
			return false;
		}
		for (int32 Index = 0; Index < Changes.Num(); ++Index)
		{
			const FPlannedChange& Change = Changes[Index];
			const FString Label =
				FString::Printf(TEXT("changes[%d]"), Index);
			if (!Writer.Identity(
					Label + TEXT(".identity"),
					Change.GetIdentity(),
					OutError)
				|| !Writer.Text(
					Label + TEXT(".intent"),
					LexToString(Change.GetIntent()),
					OutError)
				|| !Writer.Value(
					Label + TEXT(".before"),
					Change.GetBefore(),
					OutError)
				|| !Writer.Value(
					Label + TEXT(".after"),
					Change.GetAfter(),
					OutError)
				|| !Writer.Count(
					Label + TEXT(".required.count"),
					Change.GetRequiredAuthorityScopes().Num(),
					OutError))
			{
				return false;
			}
			for (int32 ScopeIndex = 0;
				ScopeIndex < Change.GetRequiredAuthorityScopes().Num();
				++ScopeIndex)
			{
				if (!Writer.Text(
					FString::Printf(
						TEXT("%s.required[%d]"),
						*Label,
						ScopeIndex),
					Change.GetRequiredAuthorityScopes()[ScopeIndex],
					OutError))
				{
					return false;
				}
			}
			if (!Writer.Text(
				Label + TEXT(".impact_evidence"),
				Change.GetImpactEvidenceId(),
				OutError))
			{
				return false;
			}
		}

		const auto& Impacts = Plan->GetImpactSet().GetImpacts();
		if (!Writer.Count(TEXT("impacts.count"), Impacts.Num(), OutError))
		{
			return false;
		}
		for (int32 Index = 0; Index < Impacts.Num(); ++Index)
		{
			const FImpact& Impact = Impacts[Index];
			const FString Label =
				FString::Printf(TEXT("impacts[%d]"), Index);
			if (!Writer.Identity(
					Label + TEXT(".identity"),
					Impact.GetIdentity(),
					OutError)
				|| !Writer.Identity(
					Label + TEXT(".source_identity"),
					Impact.GetSourceIdentity(),
					OutError)
				|| !Writer.Text(
					Label + TEXT(".kind"),
					Impact.GetKind(),
					OutError)
				|| !Writer.Text(
					Label + TEXT(".evidence"),
					Impact.GetEvidenceId(),
					OutError))
			{
				return false;
			}
		}

		const auto& Preconditions = Plan->GetPreconditions();
		if (!Writer.Count(
			TEXT("preconditions.count"),
			Preconditions.Num(),
			OutError))
		{
			return false;
		}
		for (int32 Index = 0; Index < Preconditions.Num(); ++Index)
		{
			const FPrecondition& Precondition = Preconditions[Index];
			const FString Label =
				FString::Printf(TEXT("preconditions[%d]"), Index);
			if (!Writer.Identity(
					Label + TEXT(".identity"),
					Precondition.GetIdentity(),
					OutError)
				|| !Writer.Revision(
					Label + TEXT(".revision"),
					Precondition.GetObservedRevision(),
					OutError)
				|| !Writer.Text(
					Label + TEXT(".existence"),
					Precondition.GetExistence(),
					OutError)
				|| !Writer.Value(
					Label + TEXT(".expected_before"),
					Precondition.GetExpectedBefore(),
					OutError))
			{
				return false;
			}
		}
		return Writer.Finish(OutFingerprint, OutError);
	}

	bool TryPlanChanges(
		const FPlanningRequest& Request,
		FUeAgentResourceTracker& Resources,
		FPlanningResult& OutResult,
		FPlanningPreflightError& OutError)
	{
		OutResult = FPlanningResult();
		OutError = FPlanningPreflightError();
		if (!Request.IsValid())
		{
			OutError = FPlanningPreflightError(
				TEXT("planning_request_invalid"));
			return false;
		}

		const FChangeSetDraft& Draft = Request.GetDraft();
		const FUeAgentCallerContext& Caller = Request.GetCallerContext();
		const int64 RequestStringBytes =
			MeasureText(Draft.GetRequestId()).Utf8Bytes
			+ MeasureText(Draft.GetIdempotencyKey()).Utf8Bytes
			+ MeasureText(Caller.GetCallerId()).Utf8Bytes;
		if (!TryCharge(
			Resources,
			TEXT("/planning/request"),
			1,
			Draft.GetChanges().Num(),
			RequestStringBytes,
			OutError))
		{
			return false;
		}

		TArray<const FChangeDraft*> OrderedDrafts;
		OrderedDrafts.Reserve(Draft.GetChanges().Num());
		for (const FChangeDraft& Change : Draft.GetChanges())
		{
			OrderedDrafts.Add(&Change);
		}
		OrderedDrafts.Sort(
			[](const FChangeDraft& Left, const FChangeDraft& Right)
			{
				return Left.GetIdentity().GetCanonicalKey()
					< Right.GetIdentity().GetCanonicalKey();
			});

		const TSet<FString> CallerScopes(Caller.GetAuthorityScopes());
		TSet<FString> Seen;
		for (const FChangeDraft* Change : OrderedDrafts)
		{
			const FString& Key =
				Change->GetIdentity().GetCanonicalKey();
			if (Seen.Contains(Key))
			{
				OutError = FPlanningPreflightError(
					TEXT("planning_duplicate_target"),
					TEXT(
						"one plan cannot change the same canonical target twice"),
					Key);
				return false;
			}
			Seen.Add(Key);
			TArray<FString> MissingScopes;
			for (const FString& Scope :
				Change->GetRequiredAuthorityScopes())
			{
				if (!CallerScopes.Contains(Scope))
				{
					MissingScopes.Add(Scope);
				}
			}
			if (!MissingScopes.IsEmpty())
			{
				OutError = FPlanningPreflightError(
					TEXT("planning_authority_denied"),
					TEXT("missing scopes: ") + FString::Join(
						MissingScopes,
						TEXT(",")),
					Key);
				return false;
			}
		}

		TArray<FPlannedChange> PlannedChanges;
		TArray<FPrecondition> Preconditions;
		TMap<FString, FImpact> ImpactsByKey;
		for (const FChangeDraft* Change : OrderedDrafts)
		{
			const FString& Key =
				Change->GetIdentity().GetCanonicalKey();
			const FUeAgentStateEntry* Current =
				FindEntry(Draft.GetSnapshot(), Key);
			FUeAgentCanonicalValuePtr Before;
			FString Existence;
			if (Change->GetIntent() == EChangeIntent::Create)
			{
				if (Current != nullptr)
				{
					OutError = FPlanningPreflightError(
						TEXT("planning_target_exists"),
						TEXT("create requires the target to be absent"),
						Key);
					return false;
				}
				Existence = TEXT("must_be_absent");
			}
			else
			{
				if (Current == nullptr)
				{
					OutError = FPlanningPreflightError(
						TEXT("planning_target_missing"),
						LexToString(Change->GetIntent())
							+ TEXT(" requires the target to exist"),
						Key);
					return false;
				}
				Before = Current->GetValue();
				Existence = TEXT("must_exist");
				bool bExpectedMatches = false;
				if (!TryEquivalent(
						Change->GetExpectedBefore(),
						Before,
						Key,
						Resources,
						bExpectedMatches,
						OutError))
				{
					return false;
				}
				if (!bExpectedMatches)
				{
					OutError = FPlanningPreflightError(
						TEXT("planning_expected_before_mismatch"),
						TEXT(
							"expected_before does not match C4 snapshot"),
						Key);
					return false;
				}
			}

			if (Change->GetIntent() == EChangeIntent::Update
				|| Change->GetIntent() == EChangeIntent::Replace)
			{
				bool bNoChange = false;
				if (!TryEquivalent(
						Before,
						Change->GetProposedAfter(),
						Key,
						Resources,
						bNoChange,
						OutError))
				{
					return false;
				}
				if (bNoChange)
				{
					continue;
				}
			}

			PlannedChanges.Emplace(
				Change->GetIdentity(),
				Change->GetIntent(),
				Before,
				Change->GetProposedAfter(),
				Change->GetRequiredAuthorityScopes(),
				Change->GetImpactEvidenceId());
			Preconditions.Emplace(
				Change->GetIdentity(),
				Draft.GetSnapshot()->GetRevision(),
				Existence,
				Before);
			ImpactsByKey.Add(
				Key,
				FImpact(
					Change->GetIdentity(),
					Change->GetIdentity(),
					TEXT("direct"),
					Change->GetImpactEvidenceId()));
			for (const FUeAgentCanonicalIdentity& Target :
				Change->GetImpactTargets())
			{
				const FString& ImpactKey = Target.GetCanonicalKey();
				const FImpact Candidate(
					Target,
					Change->GetIdentity(),
					TEXT("declared"),
					Change->GetImpactEvidenceId());
				FImpact* Existing = ImpactsByKey.Find(ImpactKey);
				const bool bCandidateWins =
					Existing == nullptr
					|| (Existing->GetKind() != TEXT("direct")
						&& (
							Candidate.GetSourceIdentity().GetCanonicalKey()
								< Existing->GetSourceIdentity().GetCanonicalKey()
							|| (
								Candidate.GetSourceIdentity().GetCanonicalKey()
									== Existing->GetSourceIdentity().GetCanonicalKey()
								&& Candidate.GetEvidenceId()
									< Existing->GetEvidenceId())));
				if (bCandidateWins)
				{
					ImpactsByKey.Add(ImpactKey, Candidate);
				}
			}
		}

		PlannedChanges.Sort(
			[](const FPlannedChange& Left, const FPlannedChange& Right)
			{
				return Left.GetIdentity().GetCanonicalKey()
					< Right.GetIdentity().GetCanonicalKey();
			});
		Preconditions.Sort(
			[](const FPrecondition& Left, const FPrecondition& Right)
			{
				return Left.GetIdentity().GetCanonicalKey()
					< Right.GetIdentity().GetCanonicalKey();
			});
		TArray<FString> ImpactKeys;
		ImpactsByKey.GetKeys(ImpactKeys);
		ImpactKeys.Sort();
		TArray<FImpact> Impacts;
		Impacts.Reserve(ImpactKeys.Num());
		for (const FString& ImpactKey : ImpactKeys)
		{
			Impacts.Add(ImpactsByKey.FindChecked(ImpactKey));
		}
		if (!TryCharge(
			Resources,
			TEXT("/planning/closure"),
			1,
			PlannedChanges.Num() + Preconditions.Num() + Impacts.Num(),
			0,
			OutError))
		{
			return false;
		}

		const TArray<FString> AuthorityScopes =
			NormalizeStrings(Caller.GetAuthorityScopes());
		const auto PlanWithoutFingerprint =
			MakeShared<FPlanningPreflightPlan>(
				Draft.GetDescriptorIdentity(),
				Draft.GetRequestId(),
				Draft.GetIdempotencyKey(),
				Caller.GetCallerId(),
				AuthorityScopes,
				Draft.GetSnapshot()->GetSnapshotId(),
				Draft.GetSnapshot()->GetRevision(),
				Draft.GetSnapshot()->GetProvenance().GetEvidenceId(),
				FChangeSet(MoveTemp(PlannedChanges)),
				FImpactSet(MoveTemp(Impacts)),
				MoveTemp(Preconditions),
				FString());
		FString Fingerprint;
		if (!TryComputePlanFingerprint(
			PlanWithoutFingerprint,
			Resources,
			Fingerprint,
			OutError))
		{
			return false;
		}
		const FPlanningPreflightPlanRef Plan =
			PlanWithoutFingerprint->WithFingerprint(MoveTemp(Fingerprint));
		const FString Status =
			Plan->GetChangeSet().GetChanges().IsEmpty()
				? TEXT("no_change")
				: TEXT("ready");
		OutResult = FPlanningResult(
			Plan,
			Request.IsDryRun(),
			Status == TEXT("ready") && !Request.IsDryRun(),
			Status);
		return true;
	}

	bool TryRevalidatePlan(
		const FPlanningPreflightPlanRef& Plan,
		const FUeAgentCanonicalSnapshotRef& CurrentSnapshot,
		const FUeAgentCallerContext& CallerContext,
		FUeAgentResourceTracker& Resources,
		FPlanningPreflightError& OutError)
	{
		OutError = FPlanningPreflightError();
		if (!TryCharge(
			Resources,
			TEXT("/planning/revalidate"),
			1,
			Plan->GetPreconditions().Num(),
			0,
			OutError))
		{
			return false;
		}
		FString Recomputed;
		if (!TryComputePlanFingerprint(
				Plan,
				Resources,
				Recomputed,
				OutError))
		{
			return false;
		}
		if (Recomputed != Plan->GetFingerprint())
		{
			OutError = FPlanningPreflightError(
				TEXT("planning_fingerprint_mismatch"),
				TEXT("plan facts no longer match fingerprint"));
			return false;
		}
		if (CallerContext.GetCallerId() != Plan->GetCallerId()
			|| NormalizeStrings(CallerContext.GetAuthorityScopes())
				!= Plan->GetAuthorityScopes())
		{
			OutError = FPlanningPreflightError(
				TEXT("planning_authority_context_changed"),
				TEXT("caller identity or authority scopes changed"));
			return false;
		}
		if (CurrentSnapshot->GetRevision() != Plan->GetObservedRevision())
		{
			OutError = FPlanningPreflightError(
				TEXT("planning_revision_stale"),
				TEXT(
					"current snapshot revision differs from planned revision"));
			return false;
		}

		for (const FPrecondition& Precondition :
			Plan->GetPreconditions())
		{
			const FString& Key =
				Precondition.GetIdentity().GetCanonicalKey();
			const FUeAgentStateEntry* Current =
				FindEntry(CurrentSnapshot, Key);
			if (Precondition.GetExistence() == TEXT("must_be_absent"))
			{
				if (Current != nullptr)
				{
					OutError = FPlanningPreflightError(
						TEXT("planning_target_exists"),
						TEXT("target became present after planning"),
						Key);
					return false;
				}
				continue;
			}
			if (Current == nullptr)
			{
				OutError = FPlanningPreflightError(
					TEXT("planning_target_missing"),
					TEXT("target became absent after planning"),
					Key);
				return false;
			}
			bool bMatches = false;
			if (!TryEquivalent(
					Precondition.GetExpectedBefore(),
					Current->GetValue(),
					Key,
					Resources,
					bMatches,
					OutError))
			{
				return false;
			}
			if (!bMatches)
			{
				OutError = FPlanningPreflightError(
					TEXT("planning_expected_before_mismatch"),
					TEXT(
						"current value differs from planned precondition"),
					Key);
				return false;
			}
		}
		return true;
	}
}
