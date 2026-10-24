#include "DomainAdapters/GenericEffect/UeAgentGenericEffectAdapter.h"

#include "Dom/JsonValue.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace UE::UAI::DomainAdapters::GenericEffect
{
	FString GetUePropertySourceTypeId(const FProperty& Property)
	{
		return FString::Printf(
			TEXT("UE.%s:%s"),
			*Property.GetClass()->GetName(),
			*Property.GetCPPType());
	}

	const FString& GetUeStringPropertySourceTypeId()
	{
		static const FString Value =
			TEXT("UE.StrProperty:FString");
		return Value;
	}

	namespace
	{
		using namespace ContextResource;
		using namespace Discovery;
		using namespace ExecutionMutation;
		using namespace PlanningPreflight;
		using namespace StateSemantic;

		FString NormalizeText(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		TArray<FString> NormalizeStringSet(TArray<FString> Values)
		{
			TArray<FString> Normalized;
			Normalized.Reserve(Values.Num());
			for (FString& Value : Values)
			{
				Value = NormalizeText(MoveTemp(Value));
				if (!Value.IsEmpty())
				{
					Normalized.Add(MoveTemp(Value));
				}
			}
			Normalized.Sort();
			for (int32 Index = Normalized.Num() - 1; Index > 0; --Index)
			{
				if (Normalized[Index] == Normalized[Index - 1])
				{
					Normalized.RemoveAt(Index);
				}
			}
			return Normalized;
		}

		bool HasNonEmptyStrings(const TArray<FString>& Values)
		{
			for (const FString& Value : Values)
			{
				if (NormalizeText(Value).IsEmpty())
				{
					return false;
				}
			}
			return true;
		}

		bool ContainsExact(
			const TArray<FString>& Values,
			const FString& Expected)
		{
			return Values.ContainsByPredicate(
				[&Expected](const FString& Value)
				{
					return Value == Expected;
				});
		}

		EUeAgentCandidateOperation CandidateOperation(
			const EChangeIntent Intent)
		{
			switch (Intent)
			{
			case EChangeIntent::Create:
				return EUeAgentCandidateOperation::Create;
			case EChangeIntent::Update:
				return EUeAgentCandidateOperation::Update;
			case EChangeIntent::Replace:
				return EUeAgentCandidateOperation::Replace;
			case EChangeIntent::Delete:
				return EUeAgentCandidateOperation::Delete;
			default:
				return EUeAgentCandidateOperation::Read;
			}
		}

		bool MatchesType(
			const FUeAgentCanonicalValuePtr& Value,
			const FUeAgentCanonicalType& Type)
		{
			if (!Value.IsValid())
			{
				return true;
			}
			if (Value->GetKind() == Type.GetValueKind())
			{
				return true;
			}
			return Type.IsNullable()
				&& Value->GetKind() == EUeAgentValueKind::Null;
		}

		bool Fail(
			FExecutionMutationError& OutError,
			FExecutionMutationError Diagnostic)
		{
			OutError = MoveTemp(Diagnostic);
			return false;
		}

		FExecutionMutationError NativeValueFailure(const FString& Code, const FString& Reason,
			const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const FString& Target = FString(), const FString& PrivateSubject = FString())
		{
			auto Diagnostic = FExecutionMutationError::Failure(Code, Reason, TEXT("execution"));
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.PropertyPath = Field;
			Diagnostic.TargetPath = Target;
			Diagnostic.SubjectId = PrivateSubject;
			Diagnostic.ActualValuePresent = Actual.IsValid();
			Diagnostic.ExpectedValuePresent = Expected.IsValid();
			Diagnostic.ActualValue = MoveTemp(Actual);
			Diagnostic.ExpectedValue = MoveTemp(Expected);
			return Diagnostic;
		}

		FExecutionMutationError NativeConditionFailure(const FString& Code, const FString& Reason,
			const FString& Field, const bool Actual, const bool Expected, const FString& Target = FString(),
			const FString& PrivateSubject = FString())
		{
			return NativeValueFailure(Code, Reason, Field, MakeShared<FJsonValueBoolean>(Actual),
				MakeShared<FJsonValueBoolean>(Expected), Target, PrivateSubject);
		}

		TSharedPtr<FJsonValue> JsonStrings(const TArray<FString>& Strings)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FString& Value : Strings) Values.Add(MakeShared<FJsonValueString>(Value));
			return MakeShared<FJsonValueArray>(MoveTemp(Values));
		}

		const TCHAR* ValueKindName(const EUeAgentValueKind Kind)
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
			default: return TEXT("unsupported");
			}
		}

		FExecutionMutationError BindingValidationFailure(const FUeAgentGenericEffectBinding& Binding)
		{
			const auto& Candidate = Binding.GetCandidateBinding();
			const FString& Target = Candidate.GetPublicRef();
			if (!Candidate.IsValid())
			{
				if (const auto* Diagnostic = Candidate.GetValidationDiagnostic())
					return FExecutionMutationError::WrapFailure(TEXT("generic_effect_binding_invalid"), *Diagnostic);
				return NativeConditionFailure(TEXT("generic_effect_binding_invalid"),
					TEXT("The candidate binding is invalid and supplied no validation diagnostic."), TEXT("candidate_binding_valid"), false, true, Target);
			}
			if (Binding.GetSourceTypeId().IsEmpty())
				return NativeValueFailure(TEXT("generic_effect_binding_invalid"), TEXT("The generic effect binding has no native source type."),
					TEXT("source_type_id"), MakeShared<FJsonValueString>(Binding.GetSourceTypeId()), MakeShared<FJsonValueString>(TEXT("nonempty native source type")), Target);
			if (!Binding.GetCanonicalType().IsValid())
				return FExecutionMutationError::WrapFailure(TEXT("generic_effect_binding_invalid"), Binding.GetCanonicalType().GetValidationDiagnostic());
			return NativeConditionFailure(TEXT("generic_effect_binding_invalid"), TEXT("The generic effect binding has no type-binding evidence."),
				TEXT("type_binding_evidence_present"), false, true, Target);
		}

		TSharedPtr<FJsonValue> SupportedOperations(const FUeAgentCandidateBinding& Binding)
		{
			TArray<FString> Names;
			for (const auto Operation : Binding.GetSupportedOperations())
			{
				switch (Operation)
				{
				case EUeAgentCandidateOperation::Read: Names.Add(TEXT("read")); break;
				case EUeAgentCandidateOperation::Create: Names.Add(TEXT("create")); break;
				case EUeAgentCandidateOperation::Update: Names.Add(TEXT("update")); break;
				case EUeAgentCandidateOperation::Replace: Names.Add(TEXT("replace")); break;
				case EUeAgentCandidateOperation::Delete: Names.Add(TEXT("delete")); break;
				}
			}
			return JsonStrings(Names);
		}

		FExecutionMutationError TypeBindingFailure(const FPlannedChange& Change, const FUeAgentGenericEffectBinding& Binding)
		{
			const bool bBeforeMismatch = !MatchesType(Change.GetBefore(), Binding.GetCanonicalType());
			const auto& Value = bBeforeMismatch ? Change.GetBefore() : Change.GetAfter();
			FString Expected = ValueKindName(Binding.GetCanonicalType().GetValueKind());
			if (Binding.GetCanonicalType().IsNullable()) Expected += TEXT(" or null");
			return NativeValueFailure(TEXT("generic_effect_type_binding_mismatch"),
				bBeforeMismatch ? TEXT("The planned before value has a different kind from its canonical type binding.")
					: TEXT("The planned after value has a different kind from its canonical type binding."),
				Change.GetIdentity().GetSelector() + (bBeforeMismatch ? TEXT("/before_value_kind") : TEXT("/after_value_kind")),
				Value.IsValid() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(ValueKindName(Value->GetKind()))) : nullptr,
				MakeShared<FJsonValueString>(Expected), Change.GetIdentity().GetOwnerId(), Change.GetIdentity().GetCanonicalKey());
		}

		FExecutionMutationError MissingBindingFailure(const FPlannedChange& Change)
		{
			return NativeConditionFailure(TEXT("generic_effect_binding_missing"),
				TEXT("The planned change has no resolved generic effect binding."), Change.GetIdentity().GetSelector(), false, true,
				Change.GetIdentity().GetOwnerId(), Change.GetIdentity().GetCanonicalKey());
		}

		FExecutionMutationError ApplyResultValidationFailure(const FUeAgentGenericEffectApplyResult& Result)
		{
			if (!Result.DidMutationStart())
				return NativeConditionFailure(TEXT("generic_effect_apply_result_invalid"),
					TEXT("A valid native apply result must report that mutation started."), TEXT("mutation_start_reported"), false, true);
			if (Result.GetSubjectGeneration() < 0)
			{
				auto Diagnostic = NativeConditionFailure(TEXT("generic_effect_apply_result_invalid"),
					TEXT("The generic effect apply result has a negative subject generation."), TEXT("subject_generation_valid"), false, true);
				Diagnostic.ActualEvidence = MakeShared<FJsonValueNumber>(Result.GetSubjectGeneration());
				return Diagnostic;
			}
			if (Result.GetMutatedIdentityKeys().IsEmpty())
				return NativeValueFailure(TEXT("generic_effect_apply_result_invalid"),
					TEXT("The generic effect apply result has no mutated identities."), TEXT("mutated_identity_count"),
					MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one mutated identity")));
			if (Result.IsRollbackSupported() && Result.GetRollbackToken().IsEmpty())
				return NativeConditionFailure(TEXT("generic_effect_apply_result_invalid"),
					TEXT("The generic effect apply result advertises rollback support without a rollback token."), TEXT("rollback_token_present"), false, true);
			for (const auto& Target : Result.GetDurableTargets())
				if (!Target.IsValid()) return FExecutionMutationError::WrapFailure(
					TEXT("generic_effect_apply_result_invalid"), Target.GetValidationDiagnostic());
			return NativeConditionFailure(TEXT("generic_effect_apply_result_invalid"),
				TEXT("The UE hook supplied an invalid apply result without retaining a more specific validation cause."),
				TEXT("apply_result_valid"), false, true);
		}

		bool CheckContext(
			const FExecutionMutationRequest& Request,
			const FString& Stage,
			FExecutionMutationError& OutError)
		{
			FUeAgentCancellationRequested Cancelled;
			if (!Request.GetCancellation().TryCheckpoint(Cancelled))
			{
				if (Cancelled.Stage.IsEmpty()) Cancelled.Stage = Stage;
				return Fail(OutError, MoveTemp(Cancelled));
			}
			return true;
		}

		bool ChargeWork(
			FUeAgentResourceTracker& Resources,
			const int64 Items,
			const FString& Path,
			FExecutionMutationError& OutError)
		{
			FUeAgentResourceChargeError ChargeError;
			if (Resources.TryChargeMany(
					{
						{EUeAgentResourceDimension::Items, Items},
						{EUeAgentResourceDimension::WorkUnits, Items},
					},
					Path,
					ChargeError))
			{
				return true;
			}
			return Fail(OutError, MoveTemp(ChargeError));
		}

		const FUeAgentGenericEffectBinding* FindBinding(
			const TArray<FUeAgentGenericEffectBinding>& Bindings,
			const FString& IdentityKey)
		{
			return Bindings.FindByPredicate(
				[&IdentityKey](
					const FUeAgentGenericEffectBinding& Binding)
				{
					return Binding.GetCandidateBinding()
						.GetSummary().GetIdentity()
						.GetCanonicalKey() == IdentityKey;
				});
		}

		bool ValidateExecutionBoundary(
			const FExecutionMutationRequest& Request,
			const FLockFenceLease& Lease,
			const TArray<FUeAgentGenericEffectBinding>& Bindings,
			FUeAgentResourceTracker& Resources,
			FExecutionMutationError& OutError)
		{
			if (!Request.IsValid()) return Fail(OutError, FExecutionMutationError::WrapFailure(
				TEXT("generic_effect_request_invalid"), Request.GetValidationDiagnostic()));
			if (!Lease.IsValid()) return Fail(OutError, FExecutionMutationError::WrapFailure(
				TEXT("generic_effect_request_invalid"), Lease.GetValidationDiagnostic()));
			if (Lease.GetOwnerId() != Request.GetOwnerLeaseId())
			{
				auto Diagnostic = NativeConditionFailure(TEXT("generic_effect_lease_owner_mismatch"),
					TEXT("The fence lease belongs to a different execution owner."), TEXT("lease_owner_matches"), false, true);
				Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Lease.GetOwnerId());
				Diagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(Request.GetOwnerLeaseId());
				return Fail(OutError, MoveTemp(Diagnostic));
			}
			if (!CheckContext(
					Request,
					TEXT("domain_adapter.generic_effect.before_validation"),
					OutError))
			{
				return false;
			}

			const TArray<FPlannedChange>& Changes =
				Request.GetPlan()->GetChangeSet().GetChanges();
			if (Changes.IsEmpty() || Bindings.Num() != Changes.Num())
			{
				return Fail(OutError, NativeValueFailure(TEXT("generic_effect_binding_missing"), Changes.IsEmpty() ? TEXT("The generic effect plan contains no changes.") : TEXT("The number of generic effect bindings does not match the planned changes."), Changes.IsEmpty() ? TEXT("planned_change_count") : TEXT("binding_count"), MakeShared<FJsonValueNumber>(Changes.IsEmpty() ? Changes.Num() : Bindings.Num()), Changes.IsEmpty() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT("at least one planned change"))) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Changes.Num())), FString(), FString()));
			}
			if (!ChargeWork(
					Resources,
					Changes.Num(),
					TEXT("domain_adapter.generic_effect.bindings"),
					OutError))
			{
				return false;
			}

			TSet<FString> SeenBindings;
			for (const FUeAgentGenericEffectBinding& Binding : Bindings)
			{
				if (!Binding.IsValid())
				{
					return Fail(OutError, Binding.GetValidationDiagnostic());
				}
				const FString& Key = Binding.GetCandidateBinding()
					.GetSummary().GetIdentity().GetCanonicalKey();
				if (SeenBindings.Contains(Key))
				{
					return Fail(OutError, NativeValueFailure(TEXT("generic_effect_binding_duplicate"), TEXT("More than one generic effect binding has the same candidate identity."), Binding.GetCandidateBinding().GetSummary().GetIdentity().GetSelector(), MakeShared<FJsonValueNumber>(2), MakeShared<FJsonValueNumber>(1), Binding.GetCandidateBinding().GetSummary().GetIdentity().GetOwnerId(), Key));
				}
				SeenBindings.Add(Key);
			}

			for (const FPlannedChange& Change : Changes)
			{
				const FString& Key =
					Change.GetIdentity().GetCanonicalKey();
				const FUeAgentGenericEffectBinding* Binding =
					FindBinding(Bindings, Key);
				if (!Binding)
				{
					return Fail(OutError, MissingBindingFailure(Change));
				}
				if (!Binding->GetCandidateBinding().Supports(
						CandidateOperation(Change.GetIntent())))
				{
					const FString Intent = LexToString(Change.GetIntent());
					return Fail(OutError, NativeValueFailure(TEXT("generic_effect_operation_unsupported"),
						TEXT("The candidate binding does not support the planned change intent."), Change.GetIdentity().GetSelector(),
						Intent.IsEmpty() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(static_cast<int32>(Change.GetIntent())))
							: TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Intent)), SupportedOperations(Binding->GetCandidateBinding()),
						Change.GetIdentity().GetOwnerId(), Change.GetIdentity().GetCanonicalKey()));
				}
				if (!MatchesType(
						Change.GetBefore(),
						Binding->GetCanonicalType())
					|| !MatchesType(
						Change.GetAfter(),
						Binding->GetCanonicalType()))
				{
					return Fail(OutError, TypeBindingFailure(Change, *Binding));
				}
				if (!ContainsExact(Lease.GetLockKeys(), Key))
				{
					return Fail(OutError, NativeConditionFailure(TEXT("generic_effect_lock_missing"), TEXT("The fence lease does not contain the planned change lock."), Change.GetIdentity().GetSelector(), false, true, Change.GetIdentity().GetOwnerId(), Change.GetIdentity().GetCanonicalKey()));
				}
			}
			return CheckContext(
				Request,
				TEXT("domain_adapter.generic_effect.before_ue_hook"),
				OutError);
		}

		FExecutionMutationError CompareFacts(
			const FMutationFacts& Facts,
			const FUeAgentGenericEffectApplyResult& Applied)
		{
			if (Facts.GetSubjectGeneration() != Applied.GetSubjectGeneration())
			{
				auto Diagnostic = NativeConditionFailure(TEXT("generic_effect_rollback_facts_mismatch"),
					TEXT("The rollback facts refer to a different subject generation."), TEXT("subject_generation_matches"), false, true);
				Diagnostic.ActualEvidence = MakeShared<FJsonValueNumber>(Facts.GetSubjectGeneration());
				Diagnostic.ExpectedEvidence = MakeShared<FJsonValueNumber>(Applied.GetSubjectGeneration());
				return Diagnostic;
			}
			if (Facts.GetMutatedIdentityKeys() != Applied.GetMutatedIdentityKeys())
			{
				auto Diagnostic = NativeConditionFailure(TEXT("generic_effect_rollback_facts_mismatch"),
					TEXT("The rollback facts contain different mutated identities."), TEXT("mutated_identities_match"), false, true);
				Diagnostic.ActualEvidence = JsonStrings(Facts.GetMutatedIdentityKeys());
				Diagnostic.ExpectedEvidence = JsonStrings(Applied.GetMutatedIdentityKeys());
				return Diagnostic;
			}
			if (Facts.GetRequiredBarrierSignals() != Applied.GetRequiredBarrierSignals())
				return NativeValueFailure(TEXT("generic_effect_rollback_facts_mismatch"),
					TEXT("The rollback facts require different barrier signals."), TEXT("required_barrier_signals"),
					JsonStrings(Facts.GetRequiredBarrierSignals()), JsonStrings(Applied.GetRequiredBarrierSignals()));
			if (Facts.IsRollbackSupported() != Applied.IsRollbackSupported())
				return NativeConditionFailure(TEXT("generic_effect_rollback_facts_mismatch"),
					TEXT("The rollback capability in the supplied facts differs from the applied result."), TEXT("rollback_supported"),
					Facts.IsRollbackSupported(), Applied.IsRollbackSupported());
			return {};
		}

		class FStoredPropertyValue final
		{
		public:
			explicit FStoredPropertyValue(FProperty& InProperty)
				: Property(&InProperty)
			{
				Data = FMemory::Malloc(
					Property->GetSize(),
					Property->GetMinAlignment());
				Property->InitializeValue(Data);
			}

			~FStoredPropertyValue()
			{
				if (Data)
				{
					Property->DestroyValue(Data);
					FMemory::Free(Data);
				}
			}

			void Capture(const void* Source)
			{
				Property->CopyCompleteValue(Data, Source);
			}

			void Restore(void* Destination) const
			{
				Property->CopyCompleteValue(Destination, Data);
			}

		private:
			FProperty* Property = nullptr;
			void* Data = nullptr;
		};

		struct FPropertyMutation
		{
			TWeakObjectPtr<UObject> Object;
			FProperty* Property = nullptr;
			TSharedPtr<FStoredPropertyValue> OriginalValue;
			bool bPackageWasDirty = false;
			FString TargetPath;
			FString PropertyPath;
		};

		struct FOperationRecord
		{
			FString OperationId;
			FString RollbackToken;
			int64 Generation = -1;
			TArray<FPropertyMutation> Mutations;
			bool bRollbackStarted = false;
			bool bRolledBack = false;
		};

		struct FSettleRecord
		{
			int64 Generation = -1;
			TArray<TWeakObjectPtr<UObject>> Objects;
			TArray<FString> ObservedSignals;
			FString Revision;
			TArray<FString> ObjectPaths;
		};

		UObject* ResolveBoundObject(
			const FUeAgentGenericEffectBinding& Binding)
		{
			const FString& OwnerId = Binding.GetCandidateBinding()
				.GetSummary().GetIdentity().GetOwnerId();
			return StaticFindObject(
				UObject::StaticClass(),
				nullptr,
				*OwnerId);
		}

		bool TryApplyStringProperty(
			const FPlannedChange& Change,
			const FUeAgentGenericEffectBinding& Binding,
			FOperationRecord& Record,
			FExecutionMutationError& OutError)
		{
			if (Change.GetIntent() != EChangeIntent::Update && Change.GetIntent() != EChangeIntent::Replace)
			{
				const FString Intent = LexToString(Change.GetIntent());
				return Fail(OutError, NativeValueFailure(TEXT("generic_effect_reflection_intent_unsupported"),
					TEXT("The reflection property hook supports only update and replace changes."), Change.GetIdentity().GetSelector(),
					Intent.IsEmpty() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(static_cast<int32>(Change.GetIntent())))
						: TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Intent)),
					JsonStrings({TEXT("update"), TEXT("replace")}), Change.GetIdentity().GetOwnerId(), Change.GetIdentity().GetCanonicalKey()));
			}
			UObject* Object = ResolveBoundObject(Binding);
			if (!::IsValid(Object))
			{
				return Fail(OutError, NativeConditionFailure(TEXT("generic_effect_object_not_found"), TEXT("The native object referenced by the generic effect binding could not be found."), TEXT("native_object_available"), false, true, Change.GetIdentity().GetOwnerId(), Change.GetIdentity().GetCanonicalKey()));
			}
			if (!IsInGameThread())
			{
				return Fail(OutError, NativeConditionFailure(TEXT("generic_effect_requires_game_thread"), TEXT("Native reflection mutation and rollback must run on the game thread."), TEXT("game_thread"), false, true, Object->GetPathName(), FString()));
			}

			FProperty* Property = FindFProperty<FProperty>(
				Object->GetClass(),
				*Change.GetIdentity().GetSelector());
			if (!Property)
			{
				return Fail(OutError, NativeConditionFailure(TEXT("generic_effect_property_not_found"), TEXT("The bound native property does not exist on the object."), Change.GetIdentity().GetSelector(), false, true, Object->GetPathName(), FString()));
			}
			const bool bEditable = Property->HasAnyPropertyFlags(CPF_Edit);
			const bool bEditConst = Property->HasAnyPropertyFlags(CPF_EditConst);
			if (!bEditable || bEditConst)
				return Fail(OutError, NativeConditionFailure(TEXT("generic_effect_property_not_editable"),
					!bEditable ? TEXT("The native property does not have the edit flag.") : TEXT("The native property is edit-const."),
					Change.GetIdentity().GetSelector(), !bEditable ? false : true, !bEditable ? true : false, Object->GetPathName()));

			const FString ActualSourceType = GetUePropertySourceTypeId(*Property);
			if (ActualSourceType != Binding.GetSourceTypeId())
				return Fail(OutError, NativeValueFailure(TEXT("generic_effect_source_type_mismatch"),
					TEXT("The native property source type differs from its binding."), Change.GetIdentity().GetSelector(),
					MakeShared<FJsonValueString>(ActualSourceType), MakeShared<FJsonValueString>(Binding.GetSourceTypeId()), Object->GetPathName()));

			FStrProperty* StringProperty = CastField<FStrProperty>(Property);
			if (!StringProperty)
				return Fail(OutError, NativeValueFailure(TEXT("generic_effect_reflection_type_unsupported"),
					TEXT("The reflection property hook requires a native FString property."), Change.GetIdentity().GetSelector(),
					MakeShared<FJsonValueString>(ActualSourceType), MakeShared<FJsonValueString>(GetUeStringPropertySourceTypeId()), Object->GetPathName()));
			if (Binding.GetCanonicalType().GetValueKind() != EUeAgentValueKind::String)
				return Fail(OutError, NativeValueFailure(TEXT("generic_effect_reflection_type_unsupported"),
					TEXT("The reflection FString hook requires a string canonical type."), Change.GetIdentity().GetSelector() + TEXT("/canonical_kind"),
					MakeShared<FJsonValueString>(ValueKindName(Binding.GetCanonicalType().GetValueKind())),
					MakeShared<FJsonValueString>(TEXT("string")), Object->GetPathName()));
			if (!Change.GetBefore().IsValid() || !Change.GetAfter().IsValid())
				return Fail(OutError, NativeValueFailure(TEXT("generic_effect_reflection_type_unsupported"),
					!Change.GetBefore().IsValid() ? TEXT("The planned string property change has no before value.") : TEXT("The planned string property change has no after value."),
					Change.GetIdentity().GetSelector() + (!Change.GetBefore().IsValid() ? TEXT("/before") : TEXT("/after")),
					nullptr, MakeShared<FJsonValueString>(TEXT("canonical value")), Object->GetPathName()));

			const FString Current =
				StringProperty->GetPropertyValue_InContainer(Object);
			if (Current != Change.GetBefore()->GetText())
			{
				return Fail(OutError, NativeValueFailure(TEXT("generic_effect_before_mismatch"), TEXT("The native property value changed from the planned before value."), Change.GetIdentity().GetSelector(), MakeShared<FJsonValueString>(Current), MakeShared<FJsonValueString>(Change.GetBefore()->GetText()), Object->GetPathName(), Change.GetIdentity().GetCanonicalKey()));
			}

			void* ValuePtr =
				Property->ContainerPtrToValuePtr<void>(Object);
			if (!ValuePtr)
			{
				return Fail(OutError, NativeConditionFailure(TEXT("generic_effect_property_storage_unavailable"), TEXT("The native property storage address is unavailable."), Change.GetIdentity().GetSelector(), false, true, Object->GetPathName(), FString()));
			}
			TSharedPtr<FStoredPropertyValue> Original =
				MakeShared<FStoredPropertyValue>(*Property);
			Original->Capture(ValuePtr);
			UPackage* Package = Object->GetOutermost();
			const bool bPackageWasDirty =
				Package && Package->IsDirty();

			const FString TargetPath = Object->GetPathName();
			const FString PropertyPath = Property->GetName();
			Object->Modify();
			Object->PreEditChange(Property);
			StringProperty->SetPropertyValue_InContainer(
				Object,
				Change.GetAfter()->GetText());
			FPropertyChangedEvent Event(
				Property,
				EPropertyChangeType::ValueSet);
			Object->PostEditChangeProperty(Event);
			if (Package
				&& Package != GetTransientPackage())
			{
				Object->MarkPackageDirty();
			}
			Record.Mutations.Add({
				Object,
				Property,
				MoveTemp(Original),
				bPackageWasDirty,
				TargetPath,
				PropertyPath,
			});
			return true;
		}
	}

	FUeAgentGenericEffectBinding::FUeAgentGenericEffectBinding(
		FUeAgentCandidateBinding InCandidateBinding,
		FString InSourceTypeId,
		FUeAgentCanonicalType InCanonicalType,
		FString InTypeBindingEvidence)
		: CandidateBinding(MoveTemp(InCandidateBinding))
		, SourceTypeId(NormalizeText(MoveTemp(InSourceTypeId)))
		, CanonicalType(MoveTemp(InCanonicalType))
		, TypeBindingEvidence(
			NormalizeText(MoveTemp(InTypeBindingEvidence)))
	{
		bValid = CandidateBinding.IsValid()
			&& !SourceTypeId.IsEmpty()
			&& CanonicalType.IsValid()
			&& !TypeBindingEvidence.IsEmpty();
		if (!bValid) ValidationDiagnostic = MakeShared<const FExecutionMutationError>(BindingValidationFailure(*this));
	}

	const FUeAgentCandidateBinding&
	FUeAgentGenericEffectBinding::GetCandidateBinding() const
	{
		return CandidateBinding;
	}

	const FString& FUeAgentGenericEffectBinding::GetSourceTypeId() const
	{
		return SourceTypeId;
	}

	const FUeAgentCanonicalType&
	FUeAgentGenericEffectBinding::GetCanonicalType() const
	{
		return CanonicalType;
	}

	const FString&
	FUeAgentGenericEffectBinding::GetTypeBindingEvidence() const
	{
		return TypeBindingEvidence;
	}

	bool FUeAgentGenericEffectBinding::IsValid() const
	{
		return bValid;
	}

	const FExecutionMutationError& FUeAgentGenericEffectBinding::GetValidationDiagnostic() const
	{
		static const FExecutionMutationError Empty;
		return ValidationDiagnostic ? *ValidationDiagnostic : Empty;
	}

	FUeAgentGenericEffectApplyResult::FUeAgentGenericEffectApplyResult()
	{
		ValidationDiagnostic = MakeShared<const FExecutionMutationError>(ApplyResultValidationFailure(*this));
	}

	FUeAgentGenericEffectApplyResult FUeAgentGenericEffectApplyResult::NoMutation()
	{
		FUeAgentGenericEffectApplyResult Result;
		Result.bNoMutationConfirmed = true;
		return Result;
	}

	bool FUeAgentGenericEffectApplyResult::IsNoMutationConfirmed() const
	{
		return bNoMutationConfirmed;
	}

	FUeAgentGenericEffectApplyResult::FUeAgentGenericEffectApplyResult(
		const int64 InSubjectGeneration,
		TArray<FString> InMutatedIdentityKeys,
		TArray<FString> InRequiredBarrierSignals,
		const bool bInRollbackSupported,
		FString InRollbackToken,
		const bool bInMutationStarted,
		TArray<AssetDurable::FAssetDurableTarget>
			InDurableTargets)
		: SubjectGeneration(InSubjectGeneration)
		, MutatedIdentityKeys(
			NormalizeStringSet(MoveTemp(InMutatedIdentityKeys)))
		, RequiredBarrierSignals(
			NormalizeStringSet(MoveTemp(InRequiredBarrierSignals)))
		, bRollbackSupported(bInRollbackSupported)
		, RollbackToken(NormalizeText(MoveTemp(InRollbackToken)))
		, DurableTargets(MoveTemp(InDurableTargets))
		, bMutationStarted(bInMutationStarted)
	{
		bValid = bMutationStarted
			&& SubjectGeneration >= 0
			&& !MutatedIdentityKeys.IsEmpty()
			&& HasNonEmptyStrings(MutatedIdentityKeys)
			&& HasNonEmptyStrings(RequiredBarrierSignals)
			&& (!bRollbackSupported || !RollbackToken.IsEmpty());
		for (const AssetDurable::FAssetDurableTarget& Target :
			DurableTargets)
		{
			bValid = bValid && Target.IsValid();
		}
		if (!bValid) ValidationDiagnostic = MakeShared<const FExecutionMutationError>(ApplyResultValidationFailure(*this));
	}

	int64 FUeAgentGenericEffectApplyResult::GetSubjectGeneration() const
	{
		return SubjectGeneration;
	}

	const TArray<FString>&
	FUeAgentGenericEffectApplyResult::GetMutatedIdentityKeys() const
	{
		return MutatedIdentityKeys;
	}

	const TArray<FString>&
	FUeAgentGenericEffectApplyResult::GetRequiredBarrierSignals() const
	{
		return RequiredBarrierSignals;
	}

	bool FUeAgentGenericEffectApplyResult::IsRollbackSupported() const
	{
		return bRollbackSupported;
	}

	const FString&
	FUeAgentGenericEffectApplyResult::GetRollbackToken() const
	{
		return RollbackToken;
	}

	const TArray<AssetDurable::FAssetDurableTarget>&
	FUeAgentGenericEffectApplyResult::GetDurableTargets() const
	{
		return DurableTargets;
	}

	bool FUeAgentGenericEffectApplyResult::DidMutationStart() const
	{
		return bMutationStarted;
	}

	bool FUeAgentGenericEffectApplyResult::IsValid() const
	{
		return bValid;
	}

	const FExecutionMutationError& FUeAgentGenericEffectApplyResult::GetValidationDiagnostic() const
	{
		static const FExecutionMutationError Empty;
		return ValidationDiagnostic ? *ValidationDiagnostic : Empty;
	}

	FUeAgentGenericEffectMutationHook::
	FUeAgentGenericEffectMutationHook(
		TArray<FUeAgentGenericEffectBinding> InBindings,
		TSharedRef<
			IUeAgentGenericEffectUeHook,
			ESPMode::ThreadSafe> InUeHook)
		: Bindings(MoveTemp(InBindings))
		, UeHook(MoveTemp(InUeHook))
	{
	}

	bool FUeAgentGenericEffectMutationHook::Execute(
		const FExecutionMutationRequest& Request,
		const FLockFenceLease& Lease,
		FUeAgentResourceTracker& Resources,
		FMutationFacts& OutFacts,
		FExecutionMutationError& OutError)
	{
		OutFacts = FMutationFacts();
		OutError = FExecutionMutationError();
		{
			FScopeLock Lock(&StateMutex);
			if (bExecutionStarted)
			{
				return Fail(OutError, NativeConditionFailure(TEXT("generic_effect_execution_state_invalid"), TEXT("The generic effect mutation hook has already started an execution."), TEXT("execution_started"), true, false, FString(), Request.GetOperationId()));
			}
			bExecutionStarted = true;
		}
		if (!ValidateExecutionBoundary(
				Request,
				Lease,
				Bindings,
				Resources,
				OutError))
		{
			OutFacts = FMutationFacts::NoMutation();
			return false;
		}

		FUeAgentGenericEffectApplyResult Result;
		const bool bApplied = UeHook->TryApply(
			Request,
			Lease,
			Bindings,
			Resources,
			Result,
			OutError);
		if (Result.DidMutationStart() && Result.IsValid())
		{
			OutFacts = FMutationFacts(
				Result.GetSubjectGeneration(),
				Result.GetMutatedIdentityKeys(),
				Result.GetRequiredBarrierSignals(),
				Result.IsRollbackSupported(),
				Result.GetDurableTargets());
			FScopeLock Lock(&StateMutex);
			AppliedOperationId = Request.GetOperationId();
			AppliedResult = Result;
		}
		if (!bApplied)
		{
			if (Result.IsNoMutationConfirmed())
			{
				OutFacts = FMutationFacts::NoMutation();
			}
			if (OutError.IsEmpty())
			{
				OutError = NativeConditionFailure(TEXT("generic_effect_apply_failed"),
					TEXT("The UE hook failed to apply the generic effect without providing a diagnostic."), TEXT("native_apply_succeeded"), false, true);
			}
			return false;
		}
		if (!Result.IsValid() || !OutFacts.IsValid())
		{
			return Fail(OutError, !Result.IsValid() ? Result.GetValidationDiagnostic() : FExecutionMutationError::WrapFailure(TEXT("generic_effect_apply_result_invalid"), OutFacts.GetValidationDiagnostic()));
		}
		return true;
	}

	bool FUeAgentGenericEffectMutationHook::Rollback(
		const FExecutionMutationRequest& Request,
		const FMutationFacts& Facts,
		FUeAgentResourceTracker& Resources,
		bool& bOutConfirmed,
		FExecutionMutationError& OutError)
	{
		bOutConfirmed = false;
		OutError = FExecutionMutationError();
		FUeAgentGenericEffectApplyResult Applied;
		{
			FScopeLock Lock(&StateMutex);
			if (bRollbackAttempted
				|| !AppliedResult.IsSet()
				|| AppliedOperationId != Request.GetOperationId())
			{
				return Fail(OutError, NativeConditionFailure(TEXT("generic_effect_rollback_state_invalid"), bRollbackAttempted ? TEXT("Rollback has already been attempted for this generic effect hook.") : !AppliedResult.IsSet() ? TEXT("The generic effect hook has no stored apply result to roll back.") : TEXT("The rollback request belongs to a different applied operation."), bRollbackAttempted ? TEXT("rollback_attempted") : !AppliedResult.IsSet() ? TEXT("applied_result_available") : TEXT("applied_operation_matches"), bRollbackAttempted ? true : false, bRollbackAttempted ? false : true, FString(), Request.GetOperationId()));
			}
			FExecutionMutationError FactsError = CompareFacts(Facts, AppliedResult.GetValue());
			if (!FactsError.IsEmpty()) return Fail(OutError, MoveTemp(FactsError));
			if (!AppliedResult->IsRollbackSupported())
			{
				return Fail(OutError, NativeConditionFailure(TEXT("generic_effect_rollback_unsupported"), TEXT("The applied generic effect does not support rollback."), TEXT("rollback_supported"), false, true, FString(), Request.GetOperationId()));
			}
			bRollbackAttempted = true;
			Applied = AppliedResult.GetValue();
		}
		const bool bRolledBack = UeHook->TryRollback(
			Request,
			Applied,
			Resources,
			bOutConfirmed,
			OutError);
		if (!bRolledBack && OutError.IsEmpty())
			OutError = NativeConditionFailure(TEXT("generic_effect_rollback_failed"),
				TEXT("The UE hook failed to roll back the generic effect without providing a diagnostic."), TEXT("native_rollback_succeeded"), false, true);
		return bRolledBack;
	}

	FUeAgentGenericEffectBarrierProvider::
	FUeAgentGenericEffectBarrierProvider(
		TSharedRef<
			IUeAgentGenericEffectUeHook,
			ESPMode::ThreadSafe> InUeHook)
		: UeHook(MoveTemp(InUeHook))
	{
	}

	bool FUeAgentGenericEffectBarrierProvider::Wait(
		const FBarrierRequest& Request,
		FUeAgentResourceTracker& Resources,
		FBarrierResult& OutResult,
		FExecutionMutationError& OutError)
	{
		OutResult = FBarrierResult();
		OutError = FExecutionMutationError();
		if (!Request.IsValid())
		{
			auto Diagnostic = NativeConditionFailure(TEXT("generic_effect_barrier_request_invalid"),
				TEXT("The generic effect barrier request requires a non-negative subject generation."), TEXT("subject_generation_valid"), false, true);
			Diagnostic.ActualEvidence = MakeShared<FJsonValueNumber>(Request.GetSubjectGeneration());
			return Fail(OutError, MoveTemp(Diagnostic));
		}
		if (!ChargeWork(
				Resources,
				FMath::Max(1, Request.GetRequiredSignals().Num()),
				TEXT("domain_adapter.generic_effect.settle"),
				OutError))
		{
			return false;
		}
		if (!UeHook->TryWaitForSettle(
				Request,
				Resources,
				OutResult,
				OutError))
		{
			if (OutError.IsEmpty()) OutError = NativeConditionFailure(TEXT("generic_effect_settle_failed"),
				TEXT("The UE hook failed to observe settle without providing a diagnostic."), TEXT("settle_observation_succeeded"), false, true);
			return false;
		}
		if (!OutResult.IsValid())
		{
			return Fail(OutError, FExecutionMutationError::WrapFailure(
				TEXT("generic_effect_settle_result_invalid"), OutResult.GetValidationDiagnostic()));
		}
		return true;
	}

	class FUeAgentReflectionGenericEffectUeHook::FState final
	{
	public:
		FCriticalSection Mutex;
		int64 NextGeneration = 1;
		TMap<FString, TSharedPtr<FOperationRecord>> Operations;
		TMap<int64, FSettleRecord> SettleRecords;
	};

	FUeAgentReflectionGenericEffectUeHook::
	FUeAgentReflectionGenericEffectUeHook()
		: State(MakeShared<FState, ESPMode::ThreadSafe>())
	{
	}

	FUeAgentReflectionGenericEffectUeHook::
	~FUeAgentReflectionGenericEffectUeHook() = default;

	bool FUeAgentReflectionGenericEffectUeHook::TryApply(
		const FExecutionMutationRequest& Request,
		const FLockFenceLease& Lease,
		const TArray<FUeAgentGenericEffectBinding>& Bindings,
		FUeAgentResourceTracker& Resources,
		FUeAgentGenericEffectApplyResult& OutResult,
		FExecutionMutationError& OutError)
	{
		OutResult = FUeAgentGenericEffectApplyResult();
		OutError = FExecutionMutationError();
		if (!IsInGameThread())
		{
			OutResult = FUeAgentGenericEffectApplyResult::NoMutation();
			return Fail(OutError, NativeConditionFailure(TEXT("generic_effect_requires_game_thread"), TEXT("Native reflection mutation and rollback must run on the game thread."), TEXT("game_thread"), false, true, FString(), FString()));
		}

		const TSharedRef<FOperationRecord> Record =
			MakeShared<FOperationRecord>();
		Record->OperationId = Request.GetOperationId();
		{
			FScopeLock Lock(&State->Mutex);
			Record->Generation = State->NextGeneration++;
		}
		Record->RollbackToken = FString::Printf(
			TEXT("generic-effect:%s:%lld"),
			*Request.GetOperationId(),
			Record->Generation);

		TArray<FString> MutatedKeys;
		TArray<TWeakObjectPtr<UObject>> MutatedObjects;
		TArray<FString> MutatedObjectPaths;
		const TArray<FPlannedChange>& Changes =
			Request.GetPlan()->GetChangeSet().GetChanges();
		for (const FPlannedChange& Change : Changes)
		{
			if (!ChargeWork(
					Resources,
					1,
					TEXT("domain_adapter.generic_effect.reflection"),
					OutError))
			{
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Change.GetIdentity().GetOwnerId();
				break;
			}
			const FString& Key =
				Change.GetIdentity().GetCanonicalKey();
			const FUeAgentGenericEffectBinding* Binding =
				FindBinding(Bindings, Key);
			if (!Binding)
			{
				Fail(OutError, MissingBindingFailure(Change));
				break;
			}
			if (!TryApplyStringProperty(
					Change,
					*Binding,
					*Record,
					OutError))
			{
				break;
			}
			MutatedKeys.Add(Key);
			MutatedObjects.Add(
				Record->Mutations.Last().Object);
			MutatedObjectPaths.Add(Record->Mutations.Last().TargetPath);
		}

		if (!Record->Mutations.IsEmpty())
		{
			const TArray<FString> Signals = {
				TEXT("ue_property_notification_complete"),
				TEXT("ue_object_state_settled"),
			};
			OutResult = FUeAgentGenericEffectApplyResult(
				Record->Generation,
				MutatedKeys,
				Signals,
				true,
				Record->RollbackToken,
				true);
			FScopeLock Lock(&State->Mutex);
			State->Operations.Add(
				Record->RollbackToken,
				Record);
			State->SettleRecords.Add(
				Record->Generation,
				{
					Record->Generation,
					MutatedObjects,
					Signals,
					FString::Printf(
						TEXT("generic-effect:%lld:settled"),
						Record->Generation),
					MutatedObjectPaths,
				});
		}
		else
		{
			// TryApplyStringProperty records every target write before returning.
			OutResult = FUeAgentGenericEffectApplyResult::NoMutation();
		}
		if ((!OutError.IsEmpty()))
		{
			return false;
		}
		if (!OutResult.IsValid())
		{
			return Fail(OutError, NativeValueFailure(TEXT("generic_effect_no_mutation"), TEXT("The reflection hook completed without recording a native property mutation."), TEXT("mutated_property_count"), MakeShared<FJsonValueNumber>(Record->Mutations.Num()), MakeShared<FJsonValueString>(TEXT("at least one native property mutation")), FString(), Request.GetOperationId()));
		}
		return true;
	}

	bool FUeAgentReflectionGenericEffectUeHook::TryRollback(
		const FExecutionMutationRequest& Request,
		const FUeAgentGenericEffectApplyResult& Applied,
		FUeAgentResourceTracker& Resources,
		bool& bOutConfirmed,
		FExecutionMutationError& OutError)
	{
		bOutConfirmed = false;
		OutError = FExecutionMutationError();
		if (!IsInGameThread())
		{
			return Fail(OutError, NativeConditionFailure(TEXT("generic_effect_requires_game_thread"), TEXT("Native reflection mutation and rollback must run on the game thread."), TEXT("game_thread"), false, true, FString(), FString()));
		}

		TSharedPtr<FOperationRecord> Record;
		{
			FScopeLock Lock(&State->Mutex);
			Record = State->Operations.FindRef(
				Applied.GetRollbackToken());
			if (!Record.IsValid()
				|| Record->OperationId != Request.GetOperationId()
				|| Record->Generation
					!= Applied.GetSubjectGeneration()
				|| Record->bRollbackStarted)
			{
				auto Diagnostic = NativeConditionFailure(TEXT("generic_effect_rollback_record_invalid"),
					!Record.IsValid() ? TEXT("The reflection rollback record could not be found.")
						: Record->OperationId != Request.GetOperationId() ? TEXT("The rollback record belongs to a different operation.")
						: Record->Generation != Applied.GetSubjectGeneration() ? TEXT("The rollback record belongs to a different subject generation.")
						: TEXT("The reflection rollback record has already started rollback."),
					!Record.IsValid() ? TEXT("rollback_record_available") : Record->OperationId != Request.GetOperationId()
						? TEXT("rollback_operation_matches") : Record->Generation != Applied.GetSubjectGeneration()
						? TEXT("rollback_generation_matches") : TEXT("rollback_started"),
					Record.IsValid() && Record->OperationId == Request.GetOperationId()
						&& Record->Generation == Applied.GetSubjectGeneration() && Record->bRollbackStarted,
					!(Record.IsValid() && Record->OperationId == Request.GetOperationId()
						&& Record->Generation == Applied.GetSubjectGeneration() && Record->bRollbackStarted),
					FString(), Applied.GetRollbackToken());
				if (Record.IsValid() && Record->OperationId != Request.GetOperationId())
				{
					Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Record->OperationId);
					Diagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(Request.GetOperationId());
				}
				else if (Record.IsValid() && Record->Generation != Applied.GetSubjectGeneration())
				{
					Diagnostic.ActualEvidence = MakeShared<FJsonValueNumber>(Record->Generation);
					Diagnostic.ExpectedEvidence = MakeShared<FJsonValueNumber>(Applied.GetSubjectGeneration());
				}
				return Fail(OutError, MoveTemp(Diagnostic));
			}
			Record->bRollbackStarted = true;
		}

		for (int32 Index = Record->Mutations.Num() - 1;
			Index >= 0;
			--Index)
		{
			if (!ChargeWork(
					Resources,
					1,
					TEXT("domain_adapter.generic_effect.rollback"),
					OutError))
			{
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Record->Mutations[Index].TargetPath;
				return false;
			}
			FPropertyMutation& Mutation =
				Record->Mutations[Index];
			UObject* Object = Mutation.Object.Get();
			if (!::IsValid(Object)
				|| !Mutation.Property
				|| !Mutation.OriginalValue.IsValid())
			{
				return Fail(OutError, NativeConditionFailure(TEXT("generic_effect_rollback_target_invalid"), !::IsValid(Object) ? TEXT("The mutated native object is no longer valid for rollback.") : !Mutation.Property ? TEXT("The mutated native property is unavailable for rollback.") : TEXT("The stored original native property value is unavailable for rollback."), Mutation.PropertyPath, false, true, Mutation.TargetPath, Request.GetOperationId()));
			}
			void* ValuePtr =
				Mutation.Property->ContainerPtrToValuePtr<void>(
					Object);
			if (!ValuePtr)
			{
				return Fail(OutError, NativeConditionFailure(TEXT("generic_effect_property_storage_unavailable"), TEXT("The native property storage address is unavailable."), Mutation.PropertyPath, false, true, Mutation.TargetPath, FString()));
			}
			Object->PreEditChange(Mutation.Property);
			Mutation.OriginalValue->Restore(ValuePtr);
			FPropertyChangedEvent Event(
				Mutation.Property,
				EPropertyChangeType::ValueSet);
			Object->PostEditChangeProperty(Event);
			if (UPackage* Package = Object->GetOutermost())
			{
				Package->SetDirtyFlag(
					Mutation.bPackageWasDirty);
			}
		}

		{
			FScopeLock Lock(&State->Mutex);
			Record->bRolledBack = true;
			State->SettleRecords.Remove(Record->Generation);
			State->Operations.Remove(
				Applied.GetRollbackToken());
		}
		bOutConfirmed = true;
		return true;
	}

	void FUeAgentReflectionGenericEffectUeHook::
		ReleaseVerifiedOperation(const FString& OperationId)
	{
		TArray<TSharedPtr<FOperationRecord>> Released;
		{
			FScopeLock Lock(&State->Mutex);
			for (auto Iterator = State->Operations.CreateIterator();
				Iterator;
				++Iterator)
			{
				const TSharedPtr<FOperationRecord>& Record =
					Iterator.Value();
				if (Record.IsValid()
					&& Record->OperationId == OperationId)
				{
					State->SettleRecords.Remove(
						Record->Generation);
					Released.Add(Record);
					Iterator.RemoveCurrent();
				}
			}
		}
	}

	bool FUeAgentReflectionGenericEffectUeHook::TryWaitForSettle(
		const FBarrierRequest& Request, FUeAgentResourceTracker& Resources,
		FBarrierResult& OutResult, FExecutionMutationError& OutError)
	{
		OutResult = FBarrierResult();
		OutError = FExecutionMutationError();
		if (!IsInGameThread())
		{
			auto Diagnostic = NativeConditionFailure(TEXT("generic_effect_requires_game_thread"),
				TEXT("Reflection settle observation must run on the game thread."), TEXT("game_thread"), false, true);
			Diagnostic.Stage = TEXT("settling");
			return Fail(OutError, MoveTemp(Diagnostic));
		}
		FSettleRecord Record;
		{
			FScopeLock Lock(&State->Mutex);
			const FSettleRecord* Found = State->SettleRecords.Find(Request.GetSubjectGeneration());
			if (!Found)
			{
				auto Diagnostic = NativeConditionFailure(TEXT("generic_effect_settle_generation_unknown"),
					TEXT("No reflection settle record exists for the requested subject generation."), TEXT("subject_generation_known"), false, true);
				Diagnostic.ActualEvidence = MakeShared<FJsonValueNumber>(Request.GetSubjectGeneration());
				Diagnostic.Stage = TEXT("settling");
				return Fail(OutError, MoveTemp(Diagnostic));
			}
			Record = *Found;
		}

		FExecutionMutationError SettleDiagnostic;
		bool bObjectsValid = true;
		for (int32 Index = 0; Index < Record.Objects.Num(); ++Index)
		{
			if (!Record.Objects[Index].IsValid())
			{
				bObjectsValid = false;
				SettleDiagnostic = NativeConditionFailure(TEXT("generic_effect_settle_object_invalid"),
					TEXT("A mutated native object is no longer valid during settle observation."), TEXT("native_object_valid"), false, true,
					Record.ObjectPaths.IsValidIndex(Index) ? Record.ObjectPaths[Index] : FString());
				SettleDiagnostic.Stage = TEXT("settling");
				break;
			}
		}
		bool bSignalsObserved = true;
		for (const FString& Required : Request.GetRequiredSignals())
		{
			if (!Record.ObservedSignals.Contains(Required))
			{
				bSignalsObserved = false;
				TArray<TSharedPtr<FJsonValue>> Observed;
				for (const FString& Signal : Record.ObservedSignals) Observed.Add(MakeShared<FJsonValueString>(Signal));
				auto Diagnostic = NativeValueFailure(TEXT("generic_effect_settle_signal_missing"),
					TEXT("A required reflection settle signal was not observed."), TEXT("required_signal"),
					MakeShared<FJsonValueArray>(MoveTemp(Observed)), MakeShared<FJsonValueString>(Required));
				Diagnostic.Stage = TEXT("settling");
				SettleDiagnostic.Append(Diagnostic);
				break;
			}
		}
		OutResult = FBarrierResult(Record.Generation, Record.ObservedSignals, Record.Revision,
			bObjectsValid && bSignalsObserved, MoveTemp(SettleDiagnostic));
		return true;
	}
}
