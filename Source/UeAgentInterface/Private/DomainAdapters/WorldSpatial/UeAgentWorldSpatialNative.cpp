#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialNative.h"

#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialNativeInternal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformTime.h"
#include "Math/Transform.h"
#include "Misc/Crc.h"
#include "Misc/ScopeExit.h"

namespace UE::UAI::DomainAdapters::WorldSpatial
{
	using namespace ContextResource;
	using namespace Discovery;
	using namespace Private;
	using namespace StateSemantic;
	using namespace Verification;

	namespace
	{
		bool NativeValueFailure(FUeAgentWorldSpatialExecutionError& OutError, const TCHAR* Code,
			const TCHAR* Reason, const FString& Path, TSharedPtr<FJsonValue> Actual,
			TSharedPtr<FJsonValue> Expected, bool bPublicValue = true)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("native_execution"));
			OutError.PropertyPath = Path;
			OutError.SourceKind = TEXT("native_result");
			OutError.Source.JsonPath = Path;
			OutError.ActualValuePresent = Actual.IsValid();
			if (bPublicValue) OutError.ActualValue = MoveTemp(Actual);
			else OutError.ActualEvidence = MoveTemp(Actual);
			OutError.ExpectedValue = MoveTemp(Expected);
			return false;
		}

		bool NativeTextRequired(const FString& Value, const TCHAR* Path,
			FUeAgentWorldSpatialExecutionError& OutError, bool bPublicValue = true)
		{
			return !Value.IsEmpty() || NativeValueFailure(OutError, TEXT("world_spatial_compiled_value_invalid"),
				TEXT("The compiled spatial value requires a non-empty field."), Path,
				MakeShared<FJsonValueString>(Value), MakeShared<FJsonValueString>(TEXT("non-empty string")), bPublicValue);
		}

		bool MissingNativeFamily(FUeAgentWorldSpatialExecutionError& OutError, const FString& FamilyId)
		{
			return NativeValueFailure(OutError, TEXT("world_spatial_native_family_not_bound"),
				TEXT("No native implementation is registered for the requested spatial family."), TEXT("/family_id"),
				MakeShared<FJsonValueString>(FamilyId), MakeShared<FJsonValueString>(TEXT("registered native family")));
		}

		bool NativePortRequired(bool bPresent, const TCHAR* Path, const TCHAR* Code,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			return bPresent || NativeValueFailure(OutError, Code,
				TEXT("A required native spatial port is unavailable."), Path, nullptr,
				MakeShared<FJsonValueString>(TEXT("bound port")));
		}

		const TCHAR* CanonicalKindName(EUeAgentValueKind Kind)
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

		struct FNativeFamilyRegistry
		{
			TMap<FString, TSharedPtr<IWorldSpatialNativeFamily>> Families;
			TSharedPtr<IUeAgentWorldSpatialRevisionSource> RevisionSource;

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
				if (!Typed) return false;
				if (!Family)
				{
					return MissingNativeFamily(OutError, Contract.GetFamilyId());
				}
				return Family->TryRead(
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
				const FUeAgentWorldSpatialPreflightFence& Fence,
				const FUeAgentOperationContext& Context,
				FUeAgentWorldSpatialMutationResult& OutResult,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				OutResult = FUeAgentWorldSpatialMutationResult::NoMutation();
				OutError = {};
				IWorldSpatialNativeFamily* Family = Registry->Find(Contract.GetFamilyId());
				const FUeAgentWorldSpatialRequest* Typed =
					GetProductionRequest(Request, OutError);
				if (!Typed) return false;
				if (!Family)
				{
					return MissingNativeFamily(OutError, Contract.GetFamilyId());
				}
				const bool bRequiresInternalFence =
					Contract.GetCapabilityId() == TEXT("graph.view.update")
					|| Contract.GetCapabilityId() == TEXT("mesh.transform.bake")
					|| Contract.GetCapabilityId() == TEXT("mesh.pivot.update")
					|| Contract.GetCapabilityId() == TEXT("world.object_align")
					|| Contract.GetCapabilityId() == TEXT("world.object_snap_to_surface")
					|| Contract.GetCapabilityId() == TEXT("viewport.state.restore");
				if (bRequiresInternalFence && !Fence.TryValidate(OutError))
				{
					if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Contract.GetCapabilityId();
					return false;
				}
				if (bRequiresInternalFence)
				{
					for (const FUeAgentWorldSpatialFenceEntry& Entry : Fence.Entries)
					{
						if (!Entry.TargetRef.IsEmpty()
							&& !Entry.TargetRef.StartsWith(TEXT("surface:"))
							&& !TryCheckRevision(*Registry->RevisionSource, Entry.TargetRef, {
								Entry.Revision.GetSourceId(), Entry.Revision.GetGeneration(), Entry.Revision.GetSequence()}, OutError))
						{
							return false;
						}
					}
				}
				// The family must confirm its own pre-write failures. Unreported
				// native failures remain unknown, including legacy implementations.
				OutResult = {};
				return Family->TryMutate(
					Contract, *Typed, Targets, Fence, Context, OutResult, OutError);
			}

			bool TryCaptureFence(
				const FUeAgentWorldSpatialExecutionChainContract& Contract,
				const FUeAgentWorldSpatialTypedRequestRef& Request,
				const TArray<BindingGateway::Operation::FUeAgentOperationTargetBinding>& Targets,
				const FUeAgentOperationContext& Context,
				FUeAgentWorldSpatialPreflightFence& OutFence,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				OutFence = {};
				OutError = {};
				const FUeAgentWorldSpatialRequest* Typed = GetProductionRequest(Request, OutError);
				if (!Typed)
				{
					return false;
				}
			TArray<FString> Refs;
			const auto& Payload = Typed->GetPayload();
			if (Payload.IsType<FGraphViewUpdateRequestInput>())
				Refs.Add(Payload.Get<FGraphViewUpdateRequestInput>().ViewState.Location.Subject);
			else if (Payload.IsType<FMeshTransformBakeRequestInput>())
				Refs.Add(Payload.Get<FMeshTransformBakeRequestInput>().Transform.Subject);
			else if (Payload.IsType<FMeshPivotUpdateRequestInput>())
				Refs.Add(Payload.Get<FMeshPivotUpdateRequestInput>().Pivot.Subject);
			else if (Payload.IsType<FWorldObjectAlignRequestInput>())
			{
				Refs.Append(Payload.Get<FWorldObjectAlignRequestInput>().SubjectRefs);
				const auto& Anchor = Payload.Get<FWorldObjectAlignRequestInput>().AnchorRef;
				if (Anchor.IsSet()) Refs.AddUnique(Anchor.GetValue());
			}
			else if (Payload.IsType<FWorldObjectSnapToSurfaceRequestInput>())
				Refs.Append(Payload.Get<FWorldObjectSnapToSurfaceRequestInput>().SubjectRefs);
			else if (Payload.IsType<FViewportStateRestoreRequestInput>())
				Refs.Add(Payload.Get<FViewportStateRestoreRequestInput>().Snapshot.ViewportRef);
			else
			{
				return true;
			}
			for (const FString& Ref : Refs)
			{
				FWorldSpatialRevision Revision;
				if (!Registry->RevisionSource->TryReadRevision(Ref, Revision, OutError)
					|| !Revision.TryValidate(OutError))
				{
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Ref;
					return false;
				}
				OutFence.Entries.Add({
					Ref,
					FUeAgentStateRevision(Revision.SourceId, Revision.Generation, Revision.Sequence)});
			}
			return OutFence.TryValidate(OutError);
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
					: MissingNativeFamily(OutError, Contract.GetFamilyId());
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
				if (!Typed) return false;
				if (!Family)
				{
					return MissingNativeFamily(OutError, Contract.GetFamilyId());
				}
				return Family->TryCompare(
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

	bool FUeAgentWorldSpatialCompiledValue::TryValidate(FUeAgentWorldSpatialExecutionError& OutError) const
	{
		OutError = {};
		ON_SCOPE_EXIT { if (!OutError.IsEmpty() && OutError.TargetPath.IsEmpty()) OutError.TargetPath = Subject; };
		const int32 Expected = Quantity == TEXT("transform") ? 10 : Dimension;
		if (!NativeTextRequired(Subject, TEXT("/subject"), OutError)) return false;
		if (Quantity != TEXT("point") && Quantity != TEXT("direction")
			&& Quantity != TEXT("normal") && Quantity != TEXT("transform"))
		{
			return NativeValueFailure(OutError, TEXT("world_spatial_compiled_value_invalid"),
				TEXT("The compiled spatial quantity is unsupported."), TEXT("/quantity"),
				MakeShared<FJsonValueString>(Quantity), MakeShared<FJsonValueString>(TEXT("point, direction, normal, or transform")));
		}
		if (!NativeTextRequired(FrameId, TEXT("/frame_id"), OutError)) return false;
		if (Dimension != 2 && Dimension != 3)
		{
			return NativeValueFailure(OutError, TEXT("world_spatial_compiled_value_invalid"),
				TEXT("The compiled spatial dimension must be 2 or 3."), TEXT("/dimension"),
				MakeShared<FJsonValueNumber>(Dimension), MakeShared<FJsonValueString>(TEXT("2 or 3")));
		}
		if (!NativeTextRequired(Unit, TEXT("/unit"), OutError)) return false;
		if (Components.Num() != Expected)
		{
			return NativeValueFailure(OutError, TEXT("world_spatial_compiled_value_invalid"),
				TEXT("The component count does not match the compiled spatial quantity and dimension."), TEXT("/components"),
				MakeShared<FJsonValueNumber>(Components.Num()), MakeShared<FJsonValueNumber>(Expected));
		}
		if (!NativeTextRequired(EvidenceId, TEXT("/evidence_id"), OutError, false)) return false;
		for (int32 Index = 0; Index < Components.Num(); ++Index)
		{
			const double Component = Components[Index];
			if (!FMath::IsFinite(Component))
			{
				return NativeValueFailure(OutError, TEXT("world_spatial_compiled_value_invalid"),
					TEXT("The compiled spatial component must be finite."), FString::Printf(TEXT("/components/%d"), Index),
					MakeShared<FJsonValueString>(FMath::IsNaN(Component) ? TEXT("NaN") : (Component < 0 ? TEXT("-Infinity") : TEXT("Infinity"))),
					MakeShared<FJsonValueString>(TEXT("finite number")));
			}
		}
		return true;
	}

	bool FUeAgentWorldSpatialCompiledValue::IsValid() const
	{
		FUeAgentWorldSpatialExecutionError Error;
		return TryValidate(Error);
	}

	bool FUeAgentWorldSpatialMeshSelectionTarget::TryValidate(FUeAgentWorldSpatialExecutionError& OutError) const
	{
		OutError = {};
		ON_SCOPE_EXIT
		{
			if (!OutError.IsEmpty())
			{
				OutError.SourceKind = TEXT("world_spatial_selection_target");
				OutError.TargetPath = ExplicitRef;
			}
		};
		if (ExplicitRef.IsEmpty())
			return NativeValueFailure(OutError, TEXT("world_spatial_mesh_selection_target_invalid"),
				TEXT("The mesh selection target requires an explicit component reference."), TEXT("/explicit_ref"),
				MakeShared<FJsonValueString>(ExplicitRef), MakeShared<FJsonValueString>(TEXT("non-empty component reference")));
		if (!Component)
			return NativeValueFailure(OutError, TEXT("world_spatial_mesh_selection_target_invalid"),
				TEXT("The mesh selection target has no component binding."), TEXT("/component"),
				nullptr, MakeShared<FJsonValueString>(TEXT("bound primitive component")));
		return true;
	}

	bool FUeAgentWorldSpatialMeshSelectionTarget::IsValid() const
	{
		FUeAgentWorldSpatialExecutionError Error;
		return TryValidate(Error);
	}

	bool FUeAgentWorldSpatialNativeDependencies::TryValidate(FUeAgentWorldSpatialExecutionError& OutError) const
	{
		OutError = {};
		return NativePortRequired(Environment.IsValid(), TEXT("/environment"), TEXT("world_spatial_native_dependencies_invalid"), OutError)
			&& NativePortRequired(SpaceCompiler.IsValid(), TEXT("/space_compiler"), TEXT("world_spatial_native_dependencies_invalid"), OutError)
			&& NativePortRequired(RevisionSource.IsValid(), TEXT("/revision_source"), TEXT("world_spatial_native_dependencies_invalid"), OutError);
	}

	bool FUeAgentWorldSpatialNativeDependencies::IsValid() const
	{
		FUeAgentWorldSpatialExecutionError Error;
		return TryValidate(Error);
	}

	bool FUeAgentWorldSpatialNativePorts::TryValidate(FUeAgentWorldSpatialExecutionError& OutError) const
	{
		OutError = {};
		return NativePortRequired(Readback.IsValid(), TEXT("/readback"), TEXT("world_spatial_native_ports_invalid"), OutError)
			&& NativePortRequired(Mutation.IsValid(), TEXT("/mutation"), TEXT("world_spatial_native_ports_invalid"), OutError)
			&& NativePortRequired(Settle.IsValid(), TEXT("/settle"), TEXT("world_spatial_native_ports_invalid"), OutError)
			&& NativePortRequired(Comparator.IsValid(), TEXT("/comparator"), TEXT("world_spatial_native_ports_invalid"), OutError);
	}

	bool FUeAgentWorldSpatialNativePorts::IsValid() const
	{
		FUeAgentWorldSpatialExecutionError Error;
		return TryValidate(Error);
	}

	bool TryCreateWorldSpatialNativePorts(
		const FUeAgentWorldSpatialNativeDependencies& Dependencies,
		FUeAgentWorldSpatialNativePorts& OutPorts,
		FUeAgentWorldSpatialExecutionError& OutError)
	{
		OutPorts = {};
		OutError = {};
		if (!Dependencies.TryValidate(OutError)) return false;

		const TSharedRef<FNativeFamilyRegistry> Registry =
			MakeShared<FNativeFamilyRegistry>();
		Registry->RevisionSource = Dependencies.RevisionSource;
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
				return NativeValueFailure(OutError, TEXT("world_spatial_native_family_duplicate"),
					Family->GetFamilyId().IsEmpty() ? TEXT("The native family identifier is empty.") : TEXT("The native family identifier is already registered."),
					TEXT("/family_id"), MakeShared<FJsonValueString>(Family->GetFamilyId()),
					MakeShared<FJsonValueString>(TEXT("unique non-empty family identifier")));
			}
			Registry->Families.Add(Family->GetFamilyId(), Family);
		}

		OutPorts.Readback = MakeShared<FNativeReadbackPort>(Registry);
		OutPorts.Mutation = MakeShared<FNativeMutationPort>(Registry);
		OutPorts.Settle = MakeShared<FNativeSettlePort>(Registry);
		OutPorts.Comparator = MakeShared<FNativeComparator>(Registry);
		return OutPorts.TryValidate(OutError);
	}
}

namespace UE::UAI::DomainAdapters::WorldSpatial::Private
{
	using namespace StateSemantic;
	using namespace Verification;

	TSharedPtr<FJsonValue> CanonicalFieldFact(const FUeAgentCanonicalValueRef& Value)
	{
		const TSharedRef<FJsonObject> Fact = MakeShared<FJsonObject>();
		Fact->SetStringField(TEXT("kind"), CanonicalKindName(Value->GetKind()));
		switch (Value->GetKind())
		{
		case EUeAgentValueKind::Boolean: Fact->SetBoolField(TEXT("value"), Value->GetBoolean()); break;
		case EUeAgentValueKind::String: Fact->SetStringField(TEXT("value"), Value->GetText()); break;
		case EUeAgentValueKind::Integer:
		case EUeAgentValueKind::Decimal: Fact->SetStringField(TEXT("lexeme"), Value->GetLexeme()); break;
		case EUeAgentValueKind::Array: Fact->SetNumberField(TEXT("count"), Value->GetArrayItems().Num()); break;
		case EUeAgentValueKind::Object: Fact->SetNumberField(TEXT("count"), Value->GetObjectItems().Num()); break;
		default: break;
		}
		return MakeShared<FJsonValueObject>(Fact);
	}

	bool Fail(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& SubjectId)
    { OutError = MoveTemp(Diagnostic); if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = SubjectId; return false; }

	const FUeAgentWorldSpatialRequest* GetProductionRequest(
		const FUeAgentWorldSpatialTypedRequestRef& Request,
		FUeAgentWorldSpatialExecutionError& OutError)
	{
		if (Request->GetTypeId().IsEmpty()
			|| Request->GetFamilyId().IsEmpty()
			|| Request->GetRequestKindId().IsEmpty())
		{
			const TCHAR* Field = Request->GetTypeId().IsEmpty() ? TEXT("/type_id")
				: (Request->GetFamilyId().IsEmpty() ? TEXT("/family_id") : TEXT("/request_kind_id"));
			NativeValueFailure(OutError, TEXT("world_spatial_native_typed_request_invalid"),
				TEXT("The native spatial request is missing a required type or dispatch identifier."), Field,
				MakeShared<FJsonValueString>(FString()), MakeShared<FJsonValueString>(TEXT("non-empty identifier")));
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
		if (!Expected.TryValidate(OutError)
			|| !Source.TryReadRevision(ExplicitRef, Actual, OutError)
			|| !Actual.TryValidate(OutError))
		{
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = ExplicitRef;
			return false;
		}
		if (Actual.SourceId == Expected.SourceId
			&& Actual.Generation == Expected.Generation
			&& Actual.Sequence == Expected.Sequence) return true;
		OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("world_spatial_expected_revision_mismatch"),
			TEXT("The target revision changed after the preflight fence was captured."), TEXT("native_execution"));
		OutError.TargetPath = ExplicitRef;
		OutError.PropertyPath = TEXT("/revision");
		const auto RevisionEvidence = [](const FWorldSpatialRevision& Revision)
		{
			const TSharedRef<FJsonObject> Evidence = MakeShared<FJsonObject>();
			Evidence->SetStringField(TEXT("source_id"), Revision.SourceId);
			Evidence->SetStringField(TEXT("generation"), Revision.Generation);
			Evidence->SetStringField(TEXT("sequence"), ::LexToString(Revision.Sequence));
			return MakeShared<FJsonValueObject>(Evidence);
		};
		OutError.ExpectedEvidence = RevisionEvidence(Expected);
		OutError.ActualEvidence = RevisionEvidence(Actual);
		return false;
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
		OutError = MoveTemp(Error);
		return false;
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
		OutError = MoveTemp(Error);
		return false;
	}

	bool TryMakeCompiledValue(
		const FUeAgentWorldSpatialCompiledValue& Value,
		const FUeAgentOperationContext& Context,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentWorldSpatialExecutionError& OutError)
	{
		OutValue.Reset();
		if (!Value.TryValidate(OutError)) return false;
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
			if (OutError.IsEmpty()) OutError = MoveTemp(Error);
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Value.Subject;
			return false;
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
			OutError = MoveTemp(Error);
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Value.Subject;
			return false;
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
		FUeAgentWorldSpatialExecutionError& OutError)
	{
		OutSnapshot.Reset();
		OutError = {};
		if (StateFingerprint.IsEmpty())
		{
			return NativeValueFailure(OutError, TEXT("world_spatial_native_observation_empty"),
				TEXT("The native observation did not provide its state fingerprint."), TEXT("/state_fingerprint"),
				MakeShared<FJsonValueString>(StateFingerprint), MakeShared<FJsonValueString>(TEXT("non-empty fingerprint")), false);
		}
		if (Observations.IsEmpty())
		{
			return NativeValueFailure(OutError, TEXT("world_spatial_native_observation_empty"),
				TEXT("The native readback produced no observations."), TEXT("/observations"),
				MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one observation")));
		}
		const uint64 Upper = FCrc::StrCrc32(*StateFingerprint);
		const uint64 Lower = FCrc::StrCrc32(*(TEXT("uai:") + StateFingerprint));
		const int64 Sequence = static_cast<int64>(
			((Upper << 32) | Lower) & MAX_int64);
		const FUeAgentStateRevision Revision(
				Contract.GetReadbackId(),
				TEXT("native-observation-v1"),
				Sequence);
		if (!Revision.IsValid())
		{
			OutError = Revision.GetValidationDiagnostic();
			return false;
		}
		TArray<FUeAgentStateEntry> Entries;
		Entries.Reserve(Observations.Num());
		for (int32 Index = 0; Index < Observations.Num(); ++Index)
		{
			const FObservedEntry& Observation = Observations[Index];
			if (!Observation.Value.IsValid())
			{
				NativeValueFailure(OutError, TEXT("world_spatial_native_observation_value_missing"),
					TEXT("The native observation contains no canonical value."), FString::Printf(TEXT("/observations/%d/value"), Index),
					nullptr, MakeShared<FJsonValueString>(TEXT("canonical observation value")));
				OutError.SubjectId = Observation.SemanticId;
				return false;
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
			OutError = MoveTemp(Error);
			return false;
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
		OutError = {};
		ON_SCOPE_EXIT
		{
			if (!OutError.IsEmpty())
			{
				OutError.SourceKind = TEXT("canonical_snapshot");
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Role;
			}
		};
		const FUeAgentStateEntry* Entry = FindEntryByRole(Snapshot, Role);
		if (!Entry)
		{
			return NativeValueFailure(OutError, TEXT("world_spatial_readback_entry_missing"),
				TEXT("The snapshot does not contain the required compiled spatial observation."), TEXT("/entries"),
				nullptr, MakeShared<FJsonValueString>(Role));
		}
		const FUeAgentCanonicalValueRef& Object = Entry->GetValue();
		const FUeAgentCanonicalValueRef* Subject = FindObjectField(Object, TEXT("subject"));
		const FUeAgentCanonicalValueRef* Quantity = FindObjectField(Object, TEXT("quantity"));
		const FUeAgentCanonicalValueRef* Frame = FindObjectField(Object, TEXT("frame_id"));
		const FUeAgentCanonicalValueRef* Dimension = FindObjectField(Object, TEXT("dimension"));
		const FUeAgentCanonicalValueRef* Unit = FindObjectField(Object, TEXT("unit"));
		const FUeAgentCanonicalValueRef* Components = FindObjectField(Object, TEXT("components"));
		const FUeAgentCanonicalValueRef* Evidence = FindObjectField(Object, TEXT("evidence_id"));
		struct FField
		{
			const TCHAR* Name;
			const FUeAgentCanonicalValueRef* Value;
			EUeAgentValueKind Kind;
			bool bPublicValue;
		};
		const FField Fields[]{
			{TEXT("subject"), Subject, EUeAgentValueKind::String, true},
			{TEXT("quantity"), Quantity, EUeAgentValueKind::String, true},
			{TEXT("frame_id"), Frame, EUeAgentValueKind::String, true},
			{TEXT("dimension"), Dimension, EUeAgentValueKind::Integer, true},
			{TEXT("unit"), Unit, EUeAgentValueKind::String, true},
			{TEXT("components"), Components, EUeAgentValueKind::Array, true},
			{TEXT("evidence_id"), Evidence, EUeAgentValueKind::String, false},
		};
		for (const FField& Field : Fields)
		{
			if (!Field.Value)
			{
				return NativeValueFailure(OutError, TEXT("world_spatial_readback_entry_shape_invalid"),
					TEXT("The compiled spatial observation is missing a required field."), FString(TEXT("/")) + Field.Name,
					nullptr, MakeShared<FJsonValueString>(Field.Kind == EUeAgentValueKind::Integer
						? TEXT("integer or decimal") : CanonicalKindName(Field.Kind)), Field.bPublicValue);
			}
		}
		for (const FField& Field : Fields)
		{
			const EUeAgentValueKind Kind = (*Field.Value)->GetKind();
			if (Kind != Field.Kind && !(Field.Kind == EUeAgentValueKind::Integer && Kind == EUeAgentValueKind::Decimal))
			{
				return NativeValueFailure(OutError, TEXT("world_spatial_readback_entry_shape_invalid"),
					TEXT("The compiled spatial observation field has an incompatible type."), FString(TEXT("/")) + Field.Name,
					CanonicalFieldFact(*Field.Value), MakeShared<FJsonValueString>(Field.Kind == EUeAgentValueKind::Integer
						? TEXT("integer or decimal") : CanonicalKindName(Field.Kind)), Field.bPublicValue);
			}
		}
		OutValue.Subject = (*Subject)->GetText();
		OutValue.Quantity = (*Quantity)->GetText();
		OutValue.FrameId = (*Frame)->GetText();
		OutValue.Unit = (*Unit)->GetText();
		OutValue.EvidenceId = (*Evidence)->GetText();
		int64 ParsedDimension = 0;
		if (!TryParseInt64Number((*Dimension)->GetLexeme(), ParsedDimension)
			|| ParsedDimension < MIN_int32 || ParsedDimension > MAX_int32)
		{
			return NativeValueFailure(OutError, TEXT("world_spatial_readback_dimension_invalid"),
				TEXT("The compiled spatial dimension cannot be represented as a 32-bit integer."), TEXT("/dimension"),
				CanonicalFieldFact(*Dimension), MakeShared<FJsonValueString>(TEXT("32-bit integer")));
		}
		OutValue.Dimension = static_cast<int32>(ParsedDimension);
		const auto& Items = (*Components)->GetArrayItems();
		for (int32 Index = 0; Index < Items.Num(); ++Index)
		{
			const FUeAgentCanonicalValueRef& Component = Items[Index];
			double Parsed = 0.0;
			if ((Component->GetKind() != EUeAgentValueKind::Integer
					&& Component->GetKind() != EUeAgentValueKind::Decimal)
				|| !TryParseFiniteDecimal(Component->GetLexeme(), Parsed))
			{
				return NativeValueFailure(OutError, TEXT("world_spatial_readback_component_invalid"),
					TEXT("The compiled spatial component must be a finite numeric value."), FString::Printf(TEXT("/components/%d"), Index),
					CanonicalFieldFact(Component), MakeShared<FJsonValueString>(TEXT("finite integer or decimal")));
			}
			OutValue.Components.Add(Parsed);
		}
		return OutValue.TryValidate(OutError);
	}

	bool TryReadDecimalValue(
		const FUeAgentCanonicalSnapshot& Snapshot,
		const FString& Role,
		double& OutValue,
		FUeAgentWorldSpatialExecutionError& OutError)
	{
		OutValue = 0.0;
		OutError = {};
		ON_SCOPE_EXIT
		{
			if (!OutError.IsEmpty())
			{
				OutError.SourceKind = TEXT("canonical_snapshot");
				OutError.SubjectId = Role;
			}
		};
		const FUeAgentStateEntry* Entry = FindEntryByRole(Snapshot, Role);
		if (!Entry)
		{
			return NativeValueFailure(OutError, TEXT("world_spatial_readback_decimal_invalid"),
				TEXT("The snapshot does not contain the required numeric observation."), TEXT("/entries"),
				nullptr, MakeShared<FJsonValueString>(Role));
		}
		const auto& Value = Entry->GetValue();
		if (Value->GetKind() != EUeAgentValueKind::Decimal && Value->GetKind() != EUeAgentValueKind::Integer)
		{
			return NativeValueFailure(OutError, TEXT("world_spatial_readback_decimal_invalid"),
				TEXT("The snapshot observation must be numeric."), TEXT("/value"),
				CanonicalFieldFact(Value), MakeShared<FJsonValueString>(TEXT("integer or decimal")));
		}
		if (!TryParseFiniteDecimal(Value->GetLexeme(), OutValue))
		{
			return NativeValueFailure(OutError, TEXT("world_spatial_readback_decimal_invalid"),
				TEXT("The snapshot observation cannot be read as a finite native number."), TEXT("/value"),
				CanonicalFieldFact(Value), MakeShared<FJsonValueString>(TEXT("finite number")));
		}
		return true;
	}

	void SetComparisonOutcome(
		const FUeAgentWorldSpatialExecutionChainContract& Contract,
		TArray<UE::UAI::Contract::FDiagnostic> Differences,
		const FString& EvidenceId,
		FVerificationOutcome& OutOutcome)
	{
		const bool bMatches = Differences.IsEmpty();
		UE::UAI::Contract::FDiagnostic Diagnostic;
		if (Differences.Num() == 1) Diagnostic = MoveTemp(Differences[0]);
		else if (!bMatches)
		{
			Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("world_spatial_native_state_mismatch"),
				TEXT("Native state differs from the requested values."), TEXT("verification"));
			Diagnostic.Diagnostics = MoveTemp(Differences);
		}
		OutOutcome = FVerificationOutcome(TEXT("world-spatial-verify:") + Contract.GetCapabilityId(),
				bMatches ? EVerificationStatus::Verified : EVerificationStatus::Mismatch,
				bMatches ? EEffectOutcome::Complete : EEffectOutcome::Partial,
				MoveTemp(Diagnostic),
				false,
				{},
				{EvidenceId},
				{});
	}

	UE::UAI::Contract::FDiagnostic& AddComparisonDifference(
		TArray<UE::UAI::Contract::FDiagnostic>& Differences,
		const FString& Path, const FString& Reason,
		TSharedPtr<FJsonValue> Expected, TSharedPtr<FJsonValue> Actual, const FString& Target)
	{
		auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("world_spatial_native_state_mismatch"), Reason, TEXT("verification"));
		Diagnostic.PropertyPath = Path;
		Diagnostic.TargetPath = Target;
		Diagnostic.SourceKind = TEXT("native_result");
		Diagnostic.ExpectedValuePresent = Expected.IsValid();
		Diagnostic.ActualValuePresent = Actual.IsValid();
		Diagnostic.ExpectedValue = MoveTemp(Expected);
		Diagnostic.ActualValue = MoveTemp(Actual);
		return Differences.Add_GetRef(MoveTemp(Diagnostic));
	}

	TSharedPtr<FJsonValue> ComparisonNumber(double Value)
	{
		if (FMath::IsFinite(Value)) return MakeShared<FJsonValueNumber>(Value);
		return MakeShared<FJsonValueString>(FMath::IsNaN(Value) ? TEXT("NaN") : (Value < 0 ? TEXT("-Infinity") : TEXT("Infinity")));
	}

	TSharedPtr<FJsonValue> ComparisonComponents(const TArray<double>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		Items.Reserve(Values.Num());
		for (double Value : Values) Items.Add(ComparisonNumber(Value));
		return MakeShared<FJsonValueArray>(MoveTemp(Items));
	}

	bool CompareTransforms(
		const FUeAgentWorldSpatialCompiledValue& LeftValue,
		const FUeAgentWorldSpatialCompiledValue& RightValue,
		TArray<UE::UAI::Contract::FDiagnostic>& Differences,
		const FString& Path, const FString& Target,
		double Tolerance)
	{
		const auto& Left = LeftValue.Components;
		const auto& Right = RightValue.Components;
		if (Left.Num() != 10 || Right.Num() != 10)
		{
			AddComparisonDifference(Differences, Path + TEXT(".component_counts"),
				TEXT("Both compared transforms must contain ten components."), ComparisonComponents({10, 10}),
				ComparisonComponents({static_cast<double>(Left.Num()), static_cast<double>(Right.Num())}), Target);
			return false;
		}
		const auto Transform = [](const TArray<double>& Components)
		{
			return FTransform(
				FQuat(Components[3], Components[4], Components[5], Components[6]),
				FVector(Components[0], Components[1], Components[2]),
				FVector(Components[7], Components[8], Components[9]));
		};
		const FTransform Expected = Transform(Left);
		const FTransform Actual = Transform(Right);
		const auto ComponentFact = [](const FUeAgentWorldSpatialCompiledValue& Value, int32 Start, int32 Count)
		{
			const TSharedRef<FJsonObject> Fact = MakeShared<FJsonObject>();
			Fact->SetStringField(TEXT("frame_id"), Value.FrameId);
			if (Start == 0) Fact->SetStringField(TEXT("unit"), Value.Unit);
			else if (Start == 3) Fact->SetStringField(TEXT("representation"), TEXT("quaternion_xyzw"));
			else Fact->SetStringField(TEXT("unit"), TEXT("unitless"));
			TArray<double> Values;
			for (int32 Index = Start; Index < Start + Count; ++Index) Values.Add(Value.Components[Index]);
			Fact->SetField(TEXT("components"), ComparisonComponents(Values));
			return MakeShared<FJsonValueObject>(Fact);
		};
		// These are the same predicates used by FTransform::Equals, including q/-q equivalence.
		const bool bLocationMatches = Expected.TranslationEquals(Actual, Tolerance);
		const bool bRotationMatches = Expected.RotationEquals(Actual, Tolerance);
		const bool bScaleMatches = Expected.Scale3DEquals(Actual, Tolerance);
		if (!bLocationMatches)
			AddComparisonDifference(Differences, Path + TEXT(".location"), TEXT("The transform location differs from the expected value."),
				ComponentFact(LeftValue, 0, 3), ComponentFact(RightValue, 0, 3), Target);
		if (!bRotationMatches)
			AddComparisonDifference(Differences, Path + TEXT(".rotation_quaternion"), TEXT("The transform rotation is not equivalent to the expected rotation."),
				ComponentFact(LeftValue, 3, 4), ComponentFact(RightValue, 3, 4), Target);
		if (!bScaleMatches)
			AddComparisonDifference(Differences, Path + TEXT(".scale"), TEXT("The transform scale differs from the expected value."),
				ComponentFact(LeftValue, 7, 3), ComponentFact(RightValue, 7, 3), Target);
		return bLocationMatches && bRotationMatches && bScaleMatches;
	}

	bool CompareComponents(
		const TArray<double>& Left,
		const TArray<double>& Right,
		TArray<UE::UAI::Contract::FDiagnostic>& Differences,
		const FString& Path, const FString& Target,
		double Tolerance)
	{
		if (Left.Num() != Right.Num())
		{
			AddComparisonDifference(Differences, Path + TEXT(".count"), TEXT("The observed component count differs from the expected count."),
				ComparisonNumber(Left.Num()), ComparisonNumber(Right.Num()), Target);
			return false;
		}
		for (int32 Index = 0; Index < Left.Num(); ++Index)
		{
			if (!FMath::IsNearlyEqual(Left[Index], Right[Index], Tolerance))
			{
				AddComparisonDifference(Differences, FString::Printf(TEXT("%s[%d]"), *Path, Index),
					TEXT("The observed component differs from the expected value."), ComparisonNumber(Left[Index]), ComparisonNumber(Right[Index]), Target);
				return false;
			}
		}
		return true;
	}
}
