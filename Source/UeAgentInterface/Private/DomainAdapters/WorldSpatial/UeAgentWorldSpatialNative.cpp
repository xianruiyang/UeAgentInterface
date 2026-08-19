#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialNative.h"

#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialNativeInternal.h"
#include "HAL/PlatformTime.h"
#include "Misc/Crc.h"

namespace UE::UAI::DomainAdapters::WorldSpatial
{
	using namespace ContextResource;
	using namespace Discovery;
	using namespace Private;
	using namespace StateSemantic;
	using namespace Verification;

	namespace
	{
		struct FNativeFamilyRegistry
		{
			TMap<FString, TSharedPtr<IWorldSpatialNativeFamily>> Families;

			IWorldSpatialNativeFamily* Find(const FString& FamilyId) const
			{
				const TSharedPtr<IWorldSpatialNativeFamily>* Found =
					Families.Find(FamilyId);
				return Found ? Found->Get() : nullptr;
			}
		};

		class FNativeReadbackPort final : public IUeAgentWorldSpatialReadbackPort
		{
		public:
			explicit FNativeReadbackPort(TSharedRef<FNativeFamilyRegistry> InRegistry)
				: Registry(MoveTemp(InRegistry))
			{
			}

			bool TryRead(
				const FUeAgentWorldSpatialExecutionChainContract& Contract,
				const FUeAgentWorldSpatialTypedRequestRef& Request,
				const TOptional<FUeAgentCandidateBinding>& Target,
				EUeAgentWorldSpatialReadPhase Phase,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				IWorldSpatialNativeFamily* Family = Registry->Find(Contract.GetFamilyId());
				const FUeAgentWorldSpatialRequest* Typed =
					GetProductionRequest(Request, OutError);
				if (!Family)
				{
					return Fail(
						OutError,
						TEXT("world_spatial_native_family_not_bound"),
						Contract.GetFamilyId());
				}
				return Typed && Family->TryRead(
					Contract, *Typed, Target, Phase, Context, OutSnapshot, OutError);
			}

		private:
			TSharedRef<FNativeFamilyRegistry> Registry;
		};

		class FNativeMutationPort final : public IUeAgentWorldSpatialMutationPort
		{
		public:
			explicit FNativeMutationPort(TSharedRef<FNativeFamilyRegistry> InRegistry)
				: Registry(MoveTemp(InRegistry))
			{
			}

			bool TryMutate(
				const FUeAgentWorldSpatialExecutionChainContract& Contract,
				const FUeAgentWorldSpatialTypedRequestRef& Request,
				const TArray<BindingGateway::Operation::FUeAgentOperationTargetBinding>& Targets,
				const FUeAgentOperationContext& Context,
				FUeAgentWorldSpatialMutationResult& OutResult,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				OutResult = {};
				OutError = {};
				IWorldSpatialNativeFamily* Family = Registry->Find(Contract.GetFamilyId());
				const FUeAgentWorldSpatialRequest* Typed =
					GetProductionRequest(Request, OutError);
				if (!Family)
				{
					return Fail(
						OutError,
						TEXT("world_spatial_native_family_not_bound"),
						Contract.GetFamilyId());
				}
				return Typed && Family->TryMutate(
					Contract, *Typed, Targets, Context, OutResult, OutError);
			}

		private:
			TSharedRef<FNativeFamilyRegistry> Registry;
		};

		class FNativeSettlePort final : public IUeAgentWorldSpatialSettlePort
		{
		public:
			explicit FNativeSettlePort(TSharedRef<FNativeFamilyRegistry> InRegistry)
				: Registry(MoveTemp(InRegistry))
			{
			}

			bool TrySettle(
				const FUeAgentWorldSpatialExecutionChainContract& Contract,
				const FUeAgentWorldSpatialMutationResult& Mutation,
				const FUeAgentOperationContext& Context,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				OutError = {};
				IWorldSpatialNativeFamily* Family = Registry->Find(Contract.GetFamilyId());
				return Family
					? Family->TrySettle(Contract, Mutation, Context, OutError)
					: Fail(
						OutError,
						TEXT("world_spatial_native_family_not_bound"),
						Contract.GetFamilyId());
			}

		private:
			TSharedRef<FNativeFamilyRegistry> Registry;
		};

		class FNativeComparator final : public IUeAgentWorldSpatialResultComparator
		{
		public:
			explicit FNativeComparator(TSharedRef<FNativeFamilyRegistry> InRegistry)
				: Registry(MoveTemp(InRegistry))
			{
			}

			bool TryCompare(
				const FUeAgentWorldSpatialExecutionChainContract& Contract,
				const FUeAgentWorldSpatialTypedRequestRef& Request,
				const FUeAgentCanonicalSnapshot& Before,
				const FUeAgentCanonicalSnapshot& After,
				const FUeAgentOperationContext& Context,
				FVerificationOutcome& OutOutcome,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				OutOutcome = {};
				OutError = {};
				IWorldSpatialNativeFamily* Family = Registry->Find(Contract.GetFamilyId());
				const FUeAgentWorldSpatialRequest* Typed =
					GetProductionRequest(Request, OutError);
				if (!Family)
				{
					return Fail(
						OutError,
						TEXT("world_spatial_native_family_not_bound"),
						Contract.GetFamilyId());
				}
				return Typed && Family->TryCompare(
					Contract, *Typed, Before, After, Context, OutOutcome, OutError);
			}

		private:
			TSharedRef<FNativeFamilyRegistry> Registry;
		};

		const FUeAgentCanonicalValueRef* FindObjectField(
			const FUeAgentCanonicalValueRef& Object,
			const FString& Name)
		{
			if (Object->GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
				: Object->GetObjectItems())
			{
				if (Item.Key == Name)
				{
					return &Item.Value;
				}
			}
			return nullptr;
		}
	}

	bool FUeAgentWorldSpatialCompiledValue::IsValid() const
	{
		const int32 Expected = Quantity == TEXT("transform") ? 10 : Dimension;
		return !Subject.IsEmpty()
			&& (Quantity == TEXT("point")
				|| Quantity == TEXT("direction")
				|| Quantity == TEXT("normal")
				|| Quantity == TEXT("transform"))
			&& !FrameId.IsEmpty()
			&& (Dimension == 2 || Dimension == 3)
			&& !Unit.IsEmpty()
			&& Components.Num() == Expected
			&& !EvidenceId.IsEmpty()
			&& !Components.ContainsByPredicate(
				[](double Component) { return !FMath::IsFinite(Component); });
	}

	bool FUeAgentWorldSpatialMeshSelectionTarget::IsValid() const
	{
		return !ExplicitRef.IsEmpty() && Component != nullptr;
	}

	bool FUeAgentWorldSpatialNativeDependencies::IsValid() const
	{
		return Environment.IsValid()
			&& SpaceCompiler.IsValid()
			&& RevisionSource.IsValid();
	}

	bool FUeAgentWorldSpatialNativePorts::IsValid() const
	{
		return Readback.IsValid()
			&& Mutation.IsValid()
			&& Settle.IsValid()
			&& Comparator.IsValid();
	}

	bool TryCreateWorldSpatialNativePorts(
		const FUeAgentWorldSpatialNativeDependencies& Dependencies,
		FUeAgentWorldSpatialNativePorts& OutPorts,
		FUeAgentWorldSpatialExecutionError& OutError)
	{
		OutPorts = {};
		OutError = {};
		if (!Dependencies.IsValid())
		{
			return Fail(
				OutError,
				TEXT("world_spatial_native_dependencies_invalid"),
				TEXT("world_spatial"));
		}

		const TSharedRef<FNativeFamilyRegistry> Registry =
			MakeShared<FNativeFamilyRegistry>();
		const TArray<FWorldSpatialNativeFamilyRef> Families{
			MakeGraphViewNativeFamily(Dependencies),
			MakeGraphLayoutNativeFamily(Dependencies),
			MakeObjectTransformNativeFamily(Dependencies),
			MakeViewportProjectionNativeFamily(Dependencies),
			MakeViewportSessionStateNativeFamily(Dependencies),
		};
		for (const FWorldSpatialNativeFamilyRef& Family : Families)
		{
			if (Family->GetFamilyId().IsEmpty()
				|| Registry->Families.Contains(Family->GetFamilyId()))
			{
				return Fail(
					OutError,
					TEXT("world_spatial_native_family_duplicate"),
					Family->GetFamilyId());
			}
			Registry->Families.Add(Family->GetFamilyId(), Family);
		}

		OutPorts.Readback = MakeShared<FNativeReadbackPort>(Registry);
		OutPorts.Mutation = MakeShared<FNativeMutationPort>(Registry);
		OutPorts.Settle = MakeShared<FNativeSettlePort>(Registry);
		OutPorts.Comparator = MakeShared<FNativeComparator>(Registry);
		return OutPorts.IsValid();
	}
}

namespace UE::UAI::DomainAdapters::WorldSpatial::Private
{
	using namespace StateSemantic;
	using namespace Verification;

	bool Fail(
		FUeAgentWorldSpatialExecutionError& OutError,
		FString Code,
		FString SubjectId,
		FString Detail)
	{
		OutError.Code = MoveTemp(Code);
		OutError.SubjectId = MoveTemp(SubjectId);
		OutError.Detail = MoveTemp(Detail);
		return false;
	}

	const FUeAgentWorldSpatialRequest* GetProductionRequest(
		const FUeAgentWorldSpatialTypedRequestRef& Request,
		FUeAgentWorldSpatialExecutionError& OutError)
	{
		if (Request->GetTypeId().IsEmpty()
			|| Request->GetFamilyId().IsEmpty()
			|| Request->GetRequestKindId().IsEmpty())
		{
			Fail(
				OutError,
				TEXT("world_spatial_native_typed_request_invalid"),
				Request->GetTypeId());
			return nullptr;
		}
		return static_cast<const FUeAgentWorldSpatialRequest*>(&Request.Get());
	}

	bool TryCheckRevision(
		IUeAgentWorldSpatialRevisionSource& Source,
		const FString& ExplicitRef,
		const FWorldSpatialRevision& Expected,
		FUeAgentWorldSpatialExecutionError& OutError)
	{
		FWorldSpatialRevision Actual;
		if (!Expected.IsValid()
			|| !Source.TryReadRevision(ExplicitRef, Actual, OutError)
			|| !Actual.IsValid())
		{
			return false;
		}
		return Actual.SourceId == Expected.SourceId
			&& Actual.Generation == Expected.Generation
			&& Actual.Sequence == Expected.Sequence
				? true
				: Fail(
					OutError,
					TEXT("world_spatial_expected_revision_mismatch"),
					ExplicitRef,
					FString::Printf(
						TEXT("expected=%s/%s/%lld actual=%s/%s/%lld"),
						*Expected.SourceId,
						*Expected.Generation,
						Expected.Sequence,
						*Actual.SourceId,
						*Actual.Generation,
						Actual.Sequence));
	}

	bool TryMakeDecimalValue(
		double Value,
		const FUeAgentOperationContext& Context,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentWorldSpatialExecutionError& OutError)
	{
		FUeAgentStateSemanticError Error;
		if (FUeAgentCanonicalValue::TryDecimal(
				FString::SanitizeFloat(Value, 9),
				Context.GetResourceTracker(),
				OutValue,
				Error))
		{
			return true;
		}
		return Fail(
			OutError,
			TEXT("world_spatial_readback_decimal_failed"),
			Error.Path,
			Error.Code);
	}

	bool TryMakeIntegerValue(
		int64 Value,
		const FUeAgentOperationContext& Context,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentWorldSpatialExecutionError& OutError)
	{
		FUeAgentStateSemanticError Error;
		if (FUeAgentCanonicalValue::TryInteger(
				FString::Printf(TEXT("%lld"), Value),
				Context.GetResourceTracker(),
				OutValue,
				Error))
		{
			return true;
		}
		return Fail(
			OutError,
			TEXT("world_spatial_readback_integer_failed"),
			Error.Path,
			Error.Code);
	}

	bool TryMakeCompiledValue(
		const FUeAgentWorldSpatialCompiledValue& Value,
		const FUeAgentOperationContext& Context,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentWorldSpatialExecutionError& OutError)
	{
		OutValue.Reset();
		if (!Value.IsValid())
		{
			return Fail(
				OutError,
				TEXT("world_spatial_compiled_value_invalid"),
				Value.Subject);
		}
		FUeAgentStateSemanticError Error;
		auto MakeString = [&](const FString& Text, FUeAgentCanonicalValuePtr& Out)
		{
			return FUeAgentCanonicalValue::TryString(
				Text, Context.GetResourceTracker(), Out, Error);
		};
		FUeAgentCanonicalValuePtr Subject;
		FUeAgentCanonicalValuePtr Quantity;
		FUeAgentCanonicalValuePtr Frame;
		FUeAgentCanonicalValuePtr Dimension;
		FUeAgentCanonicalValuePtr Unit;
		FUeAgentCanonicalValuePtr Evidence;
		if (!MakeString(Value.Subject, Subject)
			|| !MakeString(Value.Quantity, Quantity)
			|| !MakeString(Value.FrameId, Frame)
			|| !TryMakeIntegerValue(Value.Dimension, Context, Dimension, OutError)
			|| !MakeString(Value.Unit, Unit)
			|| !MakeString(Value.EvidenceId, Evidence))
		{
			return OutError.IsValid()
				? false
				: Fail(
					OutError,
					TEXT("world_spatial_compiled_value_field_failed"),
					Value.Subject,
					Error.Code);
		}
		TArray<FUeAgentCanonicalValueRef> Components;
		for (const double Component : Value.Components)
		{
			FUeAgentCanonicalValuePtr Item;
			if (!TryMakeDecimalValue(Component, Context, Item, OutError))
			{
				return false;
			}
			Components.Add(Item.ToSharedRef());
		}
		FUeAgentCanonicalValuePtr ComponentArray;
		if (!FUeAgentCanonicalValue::TryArray(
				Components,
				Context.GetResourceTracker(),
				ComponentArray,
				Error)
			|| !FUeAgentCanonicalValue::TryObject(
				{
					{TEXT("subject"), Subject.ToSharedRef()},
					{TEXT("quantity"), Quantity.ToSharedRef()},
					{TEXT("frame_id"), Frame.ToSharedRef()},
					{TEXT("dimension"), Dimension.ToSharedRef()},
					{TEXT("unit"), Unit.ToSharedRef()},
					{TEXT("components"), ComponentArray.ToSharedRef()},
					{TEXT("evidence_id"), Evidence.ToSharedRef()},
				},
				Context.GetResourceTracker(),
				OutValue,
				Error))
		{
			return Fail(
				OutError,
				TEXT("world_spatial_compiled_value_object_failed"),
				Value.Subject,
				Error.Code);
		}
		return true;
	}

	bool TryMakeSnapshot(
		const FUeAgentWorldSpatialExecutionChainContract& Contract,
		EUeAgentWorldSpatialReadPhase Phase,
		const FString& StateFingerprint,
		const TArray<FObservedEntry>& Observations,
		const FUeAgentOperationContext& Context,
		TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
		FUeAgentWorldSpatialExecutionError& OutError,
		const FUeAgentStateRevision* RevisionOverride)
	{
		OutSnapshot.Reset();
		if (StateFingerprint.IsEmpty() || Observations.IsEmpty())
		{
			return Fail(
				OutError,
				TEXT("world_spatial_native_observation_empty"),
				Contract.GetReadbackId());
		}
		const uint64 Upper = FCrc::StrCrc32(*StateFingerprint);
		const uint64 Lower = FCrc::StrCrc32(*(TEXT("uai:") + StateFingerprint));
		const int64 Sequence = static_cast<int64>(
			((Upper << 32) | Lower) & MAX_int64);
		const FUeAgentStateRevision Revision = RevisionOverride
			? *RevisionOverride
			: FUeAgentStateRevision(
				Contract.GetReadbackId(),
				TEXT("native-observation-v1"),
				Sequence);
		if (!Revision.IsValid())
		{
			return Fail(
				OutError,
				TEXT("world_spatial_native_observation_revision_invalid"),
				Contract.GetReadbackId());
		}
		TArray<FUeAgentStateEntry> Entries;
		Entries.Reserve(Observations.Num());
		for (const FObservedEntry& Observation : Observations)
		{
			if (!Observation.Value.IsValid())
			{
				return Fail(
					OutError,
					TEXT("world_spatial_native_observation_value_missing"),
					Observation.SemanticId);
			}
			Entries.Emplace(
				FUeAgentCanonicalIdentity(
					Observation.SemanticId,
					Observation.TargetKind,
					Observation.OwnerId,
					Observation.Selector,
					Observation.ObjectRole,
					TEXT("native-observation-v1")),
				FUeAgentCanonicalType(
					Observation.TypeId,
					Observation.Value->GetKind(),
					false,
					Observation.Value->GetKind() == EUeAgentValueKind::Object
						? TEXT("object")
						: Observation.Value->GetKind()
							== EUeAgentValueKind::Array
								? TEXT("array")
								: TEXT("scalar")),
				Observation.Value.ToSharedRef(),
				FUeAgentDefaultValue(
					EUeAgentDefaultSourceKind::DomainNative,
					Contract.GetReadbackId(),
					TEXT("independent-native-readback"),
					Observation.Value.ToSharedRef()),
				FUeAgentStateDetails(
					Observation.ObjectRole,
					TEXT("independent WorldSpatial native observation"),
					{TEXT("WorldSpatial"), Contract.GetFamilyId()},
					false,
					TEXT("available"),
					{}),
				Revision);
		}

		FUeAgentStateSemanticError Error;
		if (!FUeAgentCanonicalSnapshot::TryCreate(
				FString::Printf(
					TEXT("world-spatial:%s:%d:%s"),
					*Contract.GetCapabilityId(),
					static_cast<int32>(Phase),
					*Context.GetRequestId()),
				Context.GetExecutionContext().GetStatePlane(),
				Revision,
				FUeAgentTemporalPoint(
					TEXT("editor-native"),
					FString::Printf(
						TEXT("%llu"),
						FPlatformTime::Cycles64())),
				FUeAgentStateProvenance(
					TEXT("independent_readback"),
					Contract.GetReadbackId(),
					TEXT("native-state:") + Contract.GetCapabilityId()),
				Entries,
				Context.GetResourceTracker(),
				OutSnapshot,
				Error))
		{
			return Fail(
				OutError,
				TEXT("world_spatial_native_snapshot_failed"),
				Error.Path,
				Error.Code);
		}
		return true;
	}

	const FUeAgentStateEntry* FindEntryByRole(
		const FUeAgentCanonicalSnapshot& Snapshot,
		const FString& Role)
	{
		return Snapshot.GetEntries().FindByPredicate(
			[&Role](const FUeAgentStateEntry& Entry)
			{
				return Entry.GetIdentity().GetObjectRole() == Role;
			});
	}

	bool TryReadCompiledValue(
		const FUeAgentCanonicalSnapshot& Snapshot,
		const FString& Role,
		FUeAgentWorldSpatialCompiledValue& OutValue,
		FUeAgentWorldSpatialExecutionError& OutError)
	{
		OutValue = {};
		const FUeAgentStateEntry* Entry = FindEntryByRole(Snapshot, Role);
		if (!Entry)
		{
			return Fail(
				OutError,
				TEXT("world_spatial_readback_entry_missing"),
				Role);
		}
		const FUeAgentCanonicalValueRef& Object = Entry->GetValue();
		const FUeAgentCanonicalValueRef* Subject = FindObjectField(Object, TEXT("subject"));
		const FUeAgentCanonicalValueRef* Quantity = FindObjectField(Object, TEXT("quantity"));
		const FUeAgentCanonicalValueRef* Frame = FindObjectField(Object, TEXT("frame_id"));
		const FUeAgentCanonicalValueRef* Dimension = FindObjectField(Object, TEXT("dimension"));
		const FUeAgentCanonicalValueRef* Unit = FindObjectField(Object, TEXT("unit"));
		const FUeAgentCanonicalValueRef* Components = FindObjectField(Object, TEXT("components"));
		const FUeAgentCanonicalValueRef* Evidence = FindObjectField(Object, TEXT("evidence_id"));
		if (!Subject || !Quantity || !Frame || !Dimension || !Unit
			|| !Components || !Evidence
			|| (*Subject)->GetKind() != EUeAgentValueKind::String
			|| (*Quantity)->GetKind() != EUeAgentValueKind::String
			|| (*Frame)->GetKind() != EUeAgentValueKind::String
			|| (*Dimension)->GetKind() != EUeAgentValueKind::Integer
			|| (*Unit)->GetKind() != EUeAgentValueKind::String
			|| (*Components)->GetKind() != EUeAgentValueKind::Array
			|| (*Evidence)->GetKind() != EUeAgentValueKind::String)
		{
			return Fail(
				OutError,
				TEXT("world_spatial_readback_entry_shape_invalid"),
				Role);
		}
		OutValue.Subject = (*Subject)->GetText();
		OutValue.Quantity = (*Quantity)->GetText();
		OutValue.FrameId = (*Frame)->GetText();
		OutValue.Unit = (*Unit)->GetText();
		OutValue.EvidenceId = (*Evidence)->GetText();
		if (!LexTryParseString(OutValue.Dimension, *(*Dimension)->GetLexeme()))
		{
			return Fail(
				OutError,
				TEXT("world_spatial_readback_dimension_invalid"),
				Role);
		}
		for (const FUeAgentCanonicalValueRef& Component
			: (*Components)->GetArrayItems())
		{
			double Parsed = 0.0;
			if (Component->GetKind() != EUeAgentValueKind::Decimal
				|| !LexTryParseString(Parsed, *Component->GetLexeme()))
			{
				return Fail(
					OutError,
					TEXT("world_spatial_readback_component_invalid"),
					Role);
			}
			OutValue.Components.Add(Parsed);
		}
		return OutValue.IsValid();
	}

	bool TryReadDecimalValue(
		const FUeAgentCanonicalSnapshot& Snapshot,
		const FString& Role,
		double& OutValue,
		FUeAgentWorldSpatialExecutionError& OutError)
	{
		OutValue = 0.0;
		const FUeAgentStateEntry* Entry = FindEntryByRole(Snapshot, Role);
		return Entry
			&& Entry->GetValue()->GetKind() == EUeAgentValueKind::Decimal
			&& LexTryParseString(OutValue, *Entry->GetValue()->GetLexeme())
				? true
				: Fail(
					OutError,
					TEXT("world_spatial_readback_decimal_invalid"),
					Role);
	}

	void SetComparisonOutcome(
		const FUeAgentWorldSpatialExecutionChainContract& Contract,
		bool bMatches,
		const FString& EvidenceId,
		FVerificationOutcome& OutOutcome)
	{
		OutOutcome = FVerificationOutcome(
			TEXT("world-spatial-verify:") + Contract.GetCapabilityId(),
			bMatches ? EVerificationStatus::Verified : EVerificationStatus::Mismatch,
			bMatches ? EEffectOutcome::Complete : EEffectOutcome::Partial,
			bMatches ? FString() : TEXT("world_spatial_native_state_mismatch"),
			false,
			{},
			{EvidenceId},
			{});
	}

	bool NearlyEqualComponents(
		const TArray<double>& Left,
		const TArray<double>& Right,
		double Tolerance)
	{
		if (Left.Num() != Right.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < Left.Num(); ++Index)
		{
			if (!FMath::IsNearlyEqual(Left[Index], Right[Index], Tolerance))
			{
				return false;
			}
		}
		return true;
	}
}
