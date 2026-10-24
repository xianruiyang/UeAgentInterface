#include "DomainAdapters/GenericRead/UeAgentGenericRead.h"

#include "Dom/JsonValue.h"
#include "HAL/PlatformTime.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace UE::UAI::DomainAdapters::GenericRead
{
	namespace
	{
		bool Fail(
			FUeAgentGenericReadDiagnostic& OutDiagnostic,
			FUeAgentGenericReadDiagnostic Diagnostic)
		{
			OutDiagnostic = MoveTemp(Diagnostic);
			return false;
		}

		FUeAgentGenericReadDiagnostic NativeValueFailure(
			FString Code, FString Reason, FString Field,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			auto Error = FUeAgentGenericReadDiagnostic::Failure(MoveTemp(Code), MoveTemp(Reason), TEXT("generic_read"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = MoveTemp(Field);
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			Error.ActualValuePresent = Error.ActualValue.IsValid();
			Error.ExpectedValuePresent = Error.ExpectedValue.IsValid();
			return Error;
		}

		FUeAgentGenericReadDiagnostic NativeConditionFailure(
			FString Code, FString Reason, FString Field, bool bActual, bool bExpected)
		{
			return NativeValueFailure(MoveTemp(Code), MoveTemp(Reason), MoveTemp(Field),
				MakeShared<FJsonValueBoolean>(bActual), MakeShared<FJsonValueBoolean>(bExpected));
		}

		FUeAgentGenericReadDiagnostic AtBinding(FUeAgentGenericReadDiagnostic Error,
			const Discovery::FUeAgentCandidateBinding& Binding)
		{
			if (Error.TargetPath.IsEmpty()) Error.TargetPath = Binding.GetPublicRef();
			if (Error.SubjectId.IsEmpty()) Error.SubjectId = Binding.GetBindingId();
			return Error;
		}

		bool RequireText(
			const FString& Value,
			const TCHAR* FieldName,
			FUeAgentGenericReadDiagnostic& OutDiagnostic)
		{
			if (!Value.TrimStartAndEnd().IsEmpty())
			{
				return true;
			}
			auto Error = NativeConditionFailure(TEXT("ue_read_request_invalid"),
				FString::Printf(TEXT("%s must be non-empty"), FieldName), FieldName, false, true);
			Error.ActualEvidence = MakeShared<FJsonValueString>(Value);
			return Fail(OutDiagnostic, MoveTemp(Error));
		}

		bool Checkpoint(
			const ContextResource::FUeAgentOperationDeadline& Deadline,
			const ContextResource::FUeAgentCancellationToken& Cancellation,
			const FString& Stage,
			FUeAgentGenericReadDiagnostic& OutDiagnostic)
		{
			ContextResource::FUeAgentCancellationRequested Cancelled;
			if (!Cancellation.TryCheckpoint(Cancelled))
			{
				OutDiagnostic = Cancelled;
				if (OutDiagnostic.Stage.IsEmpty()) OutDiagnostic.Stage = Stage;
				return false;
			}

			return true;
		}

		bool ValidateIdentity(
			const FUeAgentGenericIdentity& Identity,
			const UObject& RootObject,
			FUeAgentGenericReadDiagnostic& OutDiagnostic)
		{
			if (!RequireText(
					Identity.SemanticId,
					TEXT("identity.semantic_id"),
					OutDiagnostic)
				|| !RequireText(
					Identity.TargetKind,
					TEXT("identity.target_kind"),
					OutDiagnostic)
				|| !RequireText(
					Identity.OwnerId,
					TEXT("identity.owner_id"),
					OutDiagnostic)
				|| !RequireText(
					Identity.Selector,
					TEXT("identity.selector"),
					OutDiagnostic)
				|| !RequireText(
					Identity.ObjectRole,
					TEXT("identity.object_role"),
					OutDiagnostic)
				|| !RequireText(
					Identity.Generation,
					TEXT("identity.generation"),
					OutDiagnostic))
			{
				return false;
			}
			if (Identity.OwnerId != RootObject.GetPathName())
			{
				auto Error = NativeConditionFailure(TEXT("ue_read_identity_owner_mismatch"),
					TEXT("The property binding owner differs from the resolved native object."),
					TEXT("/identity/owner_matches_object"), false, true);
				Error.ActualEvidence = MakeShared<FJsonValueString>(Identity.OwnerId);
				Error.ExpectedEvidence = MakeShared<FJsonValueString>(RootObject.GetPathName());
				return Fail(OutDiagnostic, MoveTemp(Error));
			}
			return true;
		}

		FString SourceTypeId(const FProperty& Property)
		{
			return FString::Printf(
				TEXT("UE.%s:%s"),
				*Property.GetClass()->GetName(),
				*Property.GetCPPType());
		}

		void CaptureDetails(
			const FProperty& Property,
			FUeAgentGenericReadEntry& OutEntry)
		{
			OutEntry.DisplayName =
				Property.GetDisplayNameText().ToString();
			OutEntry.Tooltip =
				Property.GetToolTipText().ToString();
			const FString Category =
				Property.GetMetaData(TEXT("Category"));
			if (!Category.IsEmpty())
			{
				Category.ParseIntoArray(
					OutEntry.CategoryPath,
					TEXT("|"),
					true);
			}
			OutEntry.bEditable =
				Property.HasAnyPropertyFlags(CPF_Edit);
			OutEntry.Availability = TEXT("available");
		}

		bool CaptureDecimal(
			const FProperty& Property,
			const UObject& RootObject,
			const UObject& DefaultObject,
			FUeAgentGenericReadEntry& OutEntry,
			FUeAgentGenericReadDiagnostic& OutDiagnostic)
		{
			const FStrProperty* StringProperty =
				CastField<FStrProperty>(&Property);
			if (!StringProperty)
			{
				return Fail(OutDiagnostic, NativeValueFailure(TEXT("ue_type_binding_mismatch"),
					TEXT("A decimal lexeme binding requires a native FString property."), Property.GetName() + TEXT("/native_type"),
					MakeShared<FJsonValueString>(SourceTypeId(Property)), MakeShared<FJsonValueString>(TEXT("FString"))));
			}
			OutEntry.ValueLexeme =
				StringProperty->GetPropertyValue_InContainer(
					&RootObject);
			OutEntry.DefaultLexeme =
				StringProperty->GetPropertyValue_InContainer(
					&DefaultObject);
			return true;
		}

		bool CaptureSpatialPoint(
			const FProperty& Property,
			const UObject& RootObject,
			const UObject& DefaultObject,
			const FUeAgentGenericSpatialBinding& Binding,
			FUeAgentGenericReadEntry& OutEntry,
			FUeAgentGenericReadDiagnostic& OutDiagnostic)
		{
			const FStructProperty* StructProperty =
				CastField<FStructProperty>(&Property);
			if (!StructProperty
				|| StructProperty->Struct
					!= TBaseStructure<FVector>::Get())
			{
				return Fail(OutDiagnostic, NativeValueFailure(TEXT("ue_type_binding_mismatch"),
					TEXT("A spatial point binding requires a native FVector property."), Property.GetName() + TEXT("/native_type"),
					MakeShared<FJsonValueString>(SourceTypeId(Property)), MakeShared<FJsonValueString>(TEXT("FVector"))));
			}
			if (!RequireText(
					Binding.Subject,
					TEXT("spatial.subject"),
					OutDiagnostic)
				|| !RequireText(
					Binding.FrameId,
					TEXT("spatial.frame_id"),
					OutDiagnostic)
				|| !RequireText(
					Binding.BasisFrameId,
					TEXT("spatial.basis_frame_id"),
					OutDiagnostic)
				|| !RequireText(
					Binding.Unit,
					TEXT("spatial.unit"),
					OutDiagnostic)
				|| !RequireText(
					Binding.BasisEvidence,
					TEXT("spatial.basis_evidence"),
					OutDiagnostic))
			{
				return false;
			}

			const FVector* Value =
				StructProperty->ContainerPtrToValuePtr<FVector>(
					&RootObject);
			const FVector* Default =
				StructProperty->ContainerPtrToValuePtr<FVector>(
					&DefaultObject);
			if (!Value || !Default)
			{
				return Fail(OutDiagnostic, NativeConditionFailure(TEXT("ue_property_value_unavailable"),
					!Value ? TEXT("The native FVector value storage could not be resolved.")
						: TEXT("The native FVector default storage could not be resolved."),
					Property.GetName() + (!Value ? TEXT("/value_storage_present") : TEXT("/default_storage_present")), false, true));
			}
			const TArray<FString> DefaultComponents = {
				LexToString(Default->X),
				LexToString(Default->Y),
				LexToString(Default->Z),
			};
			OutEntry.Spatial = {
				Binding.Subject,
				Binding.FrameId,
				Binding.BasisFrameId,
				Binding.Unit,
				Binding.BasisEvidence,
				{
					LexToString(Value->X),
					LexToString(Value->Y),
					LexToString(Value->Z),
				},
				DefaultComponents,
			};
			OutEntry.ValueLexeme = FString::Join(
				OutEntry.Spatial.Components,
				TEXT(","));
			OutEntry.DefaultLexeme =
				FString::Join(DefaultComponents, TEXT(","));
			return true;
		}

		bool CaptureProperty(
			const FUeAgentGenericPropertyBinding& Binding,
			UObject& RootObject,
			FUeAgentGenericReadEntry& OutEntry,
			FUeAgentGenericReadDiagnostic& OutDiagnostic)
		{
			if (!RequireText(
					Binding.PropertyPath,
					TEXT("property_path"),
					OutDiagnostic)
				|| !ValidateIdentity(
					Binding.Identity,
					RootObject,
					OutDiagnostic)
				|| !RequireText(
					Binding.CanonicalTypeId,
					TEXT("canonical_type_id"),
					OutDiagnostic)
				|| !RequireText(
					Binding.BindingEvidence,
					TEXT("binding_evidence"),
					OutDiagnostic)
				|| !RequireText(
					Binding.DefaultSourceIdentity,
					TEXT("default_source_identity"),
					OutDiagnostic)
				|| !RequireText(
					Binding.DefaultProvenance,
					TEXT("default_provenance"),
					OutDiagnostic))
			{
				return false;
			}

			FProperty* Property = FindFProperty<FProperty>(
				RootObject.GetClass(),
				*Binding.PropertyPath);
			if (!Property)
			{
				return Fail(OutDiagnostic, NativeConditionFailure(TEXT("ue_property_not_found"),
					TEXT("The requested reflected property was not found on the native object."), Binding.PropertyPath, false, true));
			}
			const UObject* DefaultObject =
				RootObject.GetClass()->GetDefaultObject();
			if (!DefaultObject)
			{
				auto Error = NativeConditionFailure(TEXT("ue_native_default_unavailable"),
					TEXT("The native class default object was not available."), Binding.PropertyPath + TEXT("/class_default_present"), false, true);
				Error.SubjectId = RootObject.GetClass()->GetPathName();
				return Fail(OutDiagnostic, MoveTemp(Error));
			}

			OutEntry = {};
			OutEntry.Identity = Binding.Identity;
			OutEntry.TypeBinding = {
				SourceTypeId(*Property),
				Binding.CanonicalTypeId,
				Binding.ValueKind,
				Binding.BindingEvidence,
			};
			OutEntry.DefaultSourceIdentity =
				Binding.DefaultSourceIdentity;
			OutEntry.DefaultProvenance =
				Binding.DefaultProvenance;
			CaptureDetails(*Property, OutEntry);

			switch (Binding.ValueKind)
			{
			case EUeAgentGenericValueKind::DecimalLexeme:
				return CaptureDecimal(
					*Property,
					RootObject,
					*DefaultObject,
					OutEntry,
					OutDiagnostic);
			case EUeAgentGenericValueKind::SpatialPoint:
				return CaptureSpatialPoint(
					*Property,
					RootObject,
					*DefaultObject,
					Binding.Spatial,
					OutEntry,
					OutDiagnostic);
			default:
				return Fail(OutDiagnostic, NativeValueFailure(TEXT("ue_type_binding_unsupported"),
					TEXT("The native reflection reader does not support this value kind."), Binding.PropertyPath + TEXT("/value_kind"),
					MakeShared<FJsonValueNumber>(static_cast<int32>(Binding.ValueKind)),
					MakeShared<FJsonValueString>(TEXT("DecimalLexeme or SpatialPoint"))));
			}
		}

		FUeAgentGenericReadDiagnostic CompareRead(
			const FUeAgentGenericReadEntry& Left,
			const FUeAgentGenericReadEntry& Right)
		{
			const auto Difference = [](const FString& Field, TSharedPtr<FJsonValue> Expected, TSharedPtr<FJsonValue> Actual)
			{
				return NativeValueFailure(TEXT("ue_read_not_settled"),
					TEXT("Consecutive native property reads produced different facts."), Field, MoveTemp(Actual), MoveTemp(Expected));
			};
			const auto CompareComponents = [&Difference](const FString& Field, const TArray<FString>& Expected, const TArray<FString>& Actual)
			{
				if (Expected.Num() != Actual.Num())
					return Difference(Field + TEXT("/count"), MakeShared<FJsonValueNumber>(Expected.Num()), MakeShared<FJsonValueNumber>(Actual.Num()));
				for (int32 Index = 0; Index < Expected.Num(); ++Index)
					if (Expected[Index] != Actual[Index])
						return Difference(Field + TEXT("/") + FString::FromInt(Index),
							MakeShared<FJsonValueString>(Expected[Index]), MakeShared<FJsonValueString>(Actual[Index]));
				return FUeAgentGenericReadDiagnostic();
			};
			auto Error = CompareComponents(TEXT("/spatial/components"), Left.Spatial.Components, Right.Spatial.Components);
			if (!Error.IsEmpty()) return Error;
			Error = CompareComponents(TEXT("/spatial/default_components"), Left.Spatial.DefaultComponents, Right.Spatial.DefaultComponents);
			if (!Error.IsEmpty()) return Error;
			if (Left.ValueLexeme != Right.ValueLexeme)
				return Difference(TEXT("/value"), MakeShared<FJsonValueString>(Left.ValueLexeme), MakeShared<FJsonValueString>(Right.ValueLexeme));
			if (Left.DefaultLexeme != Right.DefaultLexeme)
				return Difference(TEXT("/default"), MakeShared<FJsonValueString>(Left.DefaultLexeme), MakeShared<FJsonValueString>(Right.DefaultLexeme));
			if (Left.TypeBinding.SourceTypeId != Right.TypeBinding.SourceTypeId)
				return Difference(TEXT("/type_binding/source_type_id"), MakeShared<FJsonValueString>(Left.TypeBinding.SourceTypeId), MakeShared<FJsonValueString>(Right.TypeBinding.SourceTypeId));
			return {};
		}

		FString LexStatePlane(StateSemantic::EUeAgentStatePlane StatePlane)
		{
			return UE::UAI::Contract::LexToString(StatePlane);
		}

		StateSemantic::FUeAgentCanonicalIdentity ConvertIdentity(
			const FUeAgentGenericIdentity& Identity)
		{
			return StateSemantic::FUeAgentCanonicalIdentity(
				Identity.SemanticId,
				Identity.TargetKind,
				Identity.OwnerId,
				Identity.Selector,
				Identity.ObjectRole,
				Identity.Generation);
		}

		void MapStateError(
			const StateSemantic::FUeAgentStateSemanticError& Source,
			FUeAgentGenericReadError& OutError)
		{
			OutError = Source;
			if (OutError.SourceKind.IsEmpty()) OutError.SourceKind = TEXT("native_result");
		}


		bool TryCreateSpatialValue(
			const FUeAgentGenericSpatialValue& Source,
			const TArray<FString>& Components,
			const StateSemantic::FUeAgentTemporalPoint& CapturedAt,
			ContextResource::FUeAgentResourceTracker& Resources,
			StateSemantic::FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentGenericReadError& OutError)
		{
			const StateSemantic::FUeAgentSpatialFrame Frame(
				Source.FrameId,
				Source.BasisFrameId,
				3,
				{
					{TEXT("1"), TEXT("0"), TEXT("0")},
					{TEXT("0"), TEXT("1"), TEXT("0")},
					{TEXT("0"), TEXT("0"), TEXT("1")},
				},
				{TEXT("0"), TEXT("0"), TEXT("0")},
				Source.Unit,
				Source.BasisEvidence,
				CapturedAt);
			const StateSemantic::FUeAgentSpatialValue Spatial(
				Source.Subject,
				StateSemantic::EUeAgentSpatialQuantityKind::Point,
				Frame,
				Components,
				CapturedAt);
			StateSemantic::FUeAgentStateSemanticError StateError;
			if (!StateSemantic::FUeAgentCanonicalValue::TrySpatial(
					Spatial,
					Resources,
					OutValue,
					StateError))
			{
				MapStateError(StateError, OutError);
				return false;
			}
			return true;
		}

		bool AppendTypedEntry(
			const FUeAgentGenericReadEntry& Source,
			const StateSemantic::FUeAgentStateRevision& Revision,
			const StateSemantic::FUeAgentTemporalPoint& CapturedAt,
			ContextResource::FUeAgentResourceTracker& Resources,
			TArray<FUeAgentUeReadEntry>& OutEntries,
			FUeAgentGenericReadError& OutError)
		{
			StateSemantic::FUeAgentCanonicalValuePtr Value;
			StateSemantic::FUeAgentCanonicalValuePtr DefaultValue;
			StateSemantic::FUeAgentStateSemanticError StateError;
			StateSemantic::EUeAgentValueKind CanonicalKind =
				StateSemantic::EUeAgentValueKind::Null;
			switch (Source.TypeBinding.ValueKind)
			{
			case EUeAgentGenericValueKind::DecimalLexeme:
				CanonicalKind = StateSemantic::EUeAgentValueKind::Decimal;
			if (!StateSemantic::FUeAgentCanonicalValue::TryDecimal(
						Source.ValueLexeme,
						Resources,
						Value,
						StateError))
				{
					MapStateError(StateError, OutError);
					OutError.PropertyPath = Source.Identity.Selector + TEXT("/value") + OutError.PropertyPath;
					return false;
				}
				if (!StateSemantic::FUeAgentCanonicalValue::TryDecimal(
						Source.DefaultLexeme,
						Resources,
						DefaultValue,
						StateError))
				{
					MapStateError(StateError, OutError);
					OutError.PropertyPath = Source.Identity.Selector + TEXT("/default") + OutError.PropertyPath;
					return false;
				}
				break;
			case EUeAgentGenericValueKind::SpatialPoint:
				CanonicalKind = StateSemantic::EUeAgentValueKind::Spatial;
				if (!TryCreateSpatialValue(
						Source.Spatial,
						Source.Spatial.Components,
						CapturedAt,
						Resources,
						Value,
						OutError))
				{
					OutError.PropertyPath = Source.Identity.Selector + TEXT("/value") + OutError.PropertyPath;
					return false;
				}
				if (!TryCreateSpatialValue(
						Source.Spatial,
						Source.Spatial.DefaultComponents,
						CapturedAt,
						Resources,
						DefaultValue,
						OutError))
				{
					OutError.PropertyPath = Source.Identity.Selector + TEXT("/default") + OutError.PropertyPath;
					return false;
				}
				break;
			default:
				OutError = NativeValueFailure(TEXT("ue_type_binding_unsupported"),
					TEXT("The native reflection reader does not support this value kind."), Source.Identity.Selector + TEXT("/value_kind"),
					MakeShared<FJsonValueNumber>(static_cast<int32>(Source.TypeBinding.ValueKind)),
					MakeShared<FJsonValueString>(TEXT("DecimalLexeme or SpatialPoint")));
				return false;
			}

			OutEntries.Emplace(
				ConvertIdentity(Source.Identity),
				FUeAgentUeTypeBinding(
					Source.TypeBinding.SourceTypeId,
					StateSemantic::FUeAgentCanonicalType(
						Source.TypeBinding.CanonicalTypeId,
						CanonicalKind,
						false,
						TEXT("scalar")),
					Source.TypeBinding.BindingEvidence),
				Value.ToSharedRef(),
				StateSemantic::FUeAgentDefaultValue(
					StateSemantic::EUeAgentDefaultSourceKind::DomainNative,
					Source.DefaultSourceIdentity,
					Source.DefaultProvenance,
					DefaultValue.ToSharedRef()),
				StateSemantic::FUeAgentStateDetails(
					Source.DisplayName,
					Source.Tooltip,
					Source.CategoryPath,
					Source.bEditable,
					Source.Availability,
					{}),
				Revision);
			if (!OutEntries.Last().IsValid())
			{
				OutError = OutEntries.Last().GetValidationDiagnostic();
				OutError.PropertyPath = Source.Identity.Selector + OutError.PropertyPath;
				OutEntries.Pop();
				return false;
			}
			return true;
		}
	}

	FUeAgentReflectionGenericReadHook::FUeAgentReflectionGenericReadHook(
		FUeAgentGenericReadRequest InReadTemplate)
		: ReadTemplate(MoveTemp(InReadTemplate))
	{
	}

	bool FUeAgentReflectionGenericReadHook::Capture(
		const FUeAgentGenericReadRequest& Request,
		const ContextResource::FUeAgentOperationDeadline& Deadline,
		const ContextResource::FUeAgentCancellationToken& Cancellation,
		FUeAgentGenericReadCapture& OutCapture,
		FUeAgentGenericReadDiagnostic& OutDiagnostic) const
	{
		OutCapture = {};
		OutDiagnostic = {};
		if (!Checkpoint(
				Deadline,
				Cancellation,
				TEXT("domain_adapter.generic_read.before_capture"),
				OutDiagnostic))
		{
			return false;
		}
		if (!IsInGameThread())
		{
			return Fail(OutDiagnostic, NativeConditionFailure(TEXT("ue_read_requires_game_thread"),
				TEXT("UObject reflection read must run on GameThread."), TEXT("/game_thread"), false, true));
		}
		if (!IsValid(Request.RootObject))
		{
			return Fail(OutDiagnostic, NativeConditionFailure(TEXT("ue_read_root_invalid"),
				TEXT("The native root object is missing or no longer valid."), TEXT("/root_object_valid"), false, true));
		}
		if (!RequireText(
				Request.SnapshotId,
				TEXT("snapshot_id"),
				OutDiagnostic)
			|| !RequireText(
				Request.StatePlane,
				TEXT("state_plane"),
				OutDiagnostic)
			|| !RequireText(
				Request.SourceId,
				TEXT("source_id"),
				OutDiagnostic)
			|| !RequireText(
				Request.Generation,
				TEXT("generation"),
				OutDiagnostic))
		{
			return false;
		}
		if (Request.Sequence < 0)
		{
			auto Error = NativeConditionFailure(TEXT("ue_read_request_invalid"),
				TEXT("The native read revision sequence must be non-negative."), TEXT("/sequence_non_negative"), false, true);
			Error.ActualEvidence = MakeShared<FJsonValueString>(LexToString(Request.Sequence));
			Error.ExpectedEvidence = MakeShared<FJsonValueString>(TEXT(">= 0"));
			return Fail(OutDiagnostic, MoveTemp(Error));
		}
		if (Request.Properties.IsEmpty())
		{
			return Fail(OutDiagnostic, NativeValueFailure(TEXT("ue_read_request_invalid"),
				TEXT("At least one native property binding is required."), TEXT("/properties/count"),
				MakeShared<FJsonValueNumber>(Request.Properties.Num()), MakeShared<FJsonValueString>(TEXT(">= 1"))));
		}

		OutCapture.SnapshotId = Request.SnapshotId;
		OutCapture.StatePlane = Request.StatePlane;
		OutCapture.SourceId = Request.SourceId;
		OutCapture.Generation = Request.Generation;
		OutCapture.Sequence = Request.Sequence;
		OutCapture.Entries.Reserve(Request.Properties.Num());

		for (const FUeAgentGenericPropertyBinding& Binding
			: Request.Properties)
		{
			if (!Checkpoint(
					Deadline,
					Cancellation,
					TEXT("domain_adapter.generic_read.property"),
					OutDiagnostic))
			{
				OutCapture = {};
				return false;
			}

			FUeAgentGenericReadEntry First;
			FUeAgentGenericReadEntry Second;
			if (!CaptureProperty(
					Binding,
					*Request.RootObject,
					First,
					OutDiagnostic)
				|| !CaptureProperty(
					Binding,
					*Request.RootObject,
					Second,
					OutDiagnostic))
			{
				if (OutDiagnostic.PropertyPath.IsEmpty()) OutDiagnostic.PropertyPath = Binding.PropertyPath;
				OutCapture = {};
				return false;
			}
			auto Difference = CompareRead(First, Second);
			if (!Difference.IsEmpty())
			{
				OutCapture = {};
				Difference.PropertyPath = Binding.PropertyPath + Difference.PropertyPath;
				return Fail(OutDiagnostic, MoveTemp(Difference));
			}
			OutCapture.Entries.Add(MoveTemp(Second));
		}

		OutCapture.CapturedAtCycles = FPlatformTime::Cycles64();
		OutCapture.EvidenceId = FString::Printf(
			TEXT("ue-reflection:%s:%llu"),
			*Request.RootObject->GetPathName(),
			OutCapture.CapturedAtCycles);
		OutCapture.bSettled = true;
		return true;
	}

	bool FUeAgentReflectionGenericReadHook::TryCapture(
		const Discovery::FUeAgentStateReadRequest& Request,
		ContextResource::FUeAgentResourceTracker& Resources,
		const ContextResource::FUeAgentOperationDeadline& Deadline,
		const ContextResource::FUeAgentCancellationToken& Cancellation,
		TSharedPtr<const FUeAgentUeReadCapture>& OutCapture,
		FUeAgentGenericReadError& OutError)
	{
		OutCapture.Reset();
		OutError = {};
		if (!ReadTemplate.IsSet())
		{
			OutError = AtBinding(NativeConditionFailure(TEXT("ue_read_hook_unconfigured"),
				TEXT("The reflection hook has no native object/property template."), TEXT("/read_template_present"), false, true), Request.GetBinding());
			return false;
		}
		if (!Request.IsValid())
		{
			const auto& Validation = Request.GetBinding().GetValidationDiagnostic();
			OutError = Validation ? *Validation : NativeConditionFailure(TEXT("ue_read_request_invalid"),
				TEXT("The native read binding is invalid but supplied no validation diagnostic."), TEXT("/binding_valid"), false, true);
			OutError = AtBinding(MoveTemp(OutError), Request.GetBinding());
			return false;
		}

		FUeAgentGenericReadRequest LegacyRequest = ReadTemplate.GetValue();
		if (!IsValid(LegacyRequest.RootObject))
		{
			OutError = AtBinding(NativeConditionFailure(TEXT("ue_read_binding_owner_mismatch"),
				TEXT("The injected native read object is missing or no longer valid."), TEXT("/root_object_valid"), false, true), Request.GetBinding());
			return false;
		}
		const FString NativeOwner = LegacyRequest.RootObject->GetPathName();
		const FString& BindingOwner = Request.GetBinding().GetSummary().GetIdentity().GetOwnerId();
		if (BindingOwner != NativeOwner)
		{
			OutError = AtBinding(NativeConditionFailure(TEXT("ue_read_binding_owner_mismatch"),
				TEXT("The binding owner differs from the injected native read object."), TEXT("/binding_owner_matches_object"), false, true), Request.GetBinding());
			OutError.ActualEvidence = MakeShared<FJsonValueString>(NativeOwner);
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(BindingOwner);
			return false;
		}

		const uint64 SnapshotTick = FPlatformTime::Cycles64();
		LegacyRequest.SnapshotId = FString::Printf(
			TEXT("ue-read:%s:%llu"),
			*Request.GetBinding().GetBindingId(),
			SnapshotTick);
		LegacyRequest.StatePlane = LexStatePlane(Request.GetStatePlane());
		LegacyRequest.SourceId =
			Request.GetBinding().GetReadPortId();
		LegacyRequest.Generation =
			Request.GetBinding().GetSummary().GetIdentity().GetGeneration();

		FUeAgentGenericReadCapture LegacyCapture;
		FUeAgentGenericReadDiagnostic Diagnostic;
		if (!Capture(
				LegacyRequest,
				Deadline,
				Cancellation,
				LegacyCapture,
				Diagnostic))
		{
			OutError = AtBinding(MoveTemp(Diagnostic), Request.GetBinding());
			return false;
		}

		const StateSemantic::FUeAgentTemporalPoint CapturedAt(
			TEXT("ue.platform_cycles64"),
			LexToString(LegacyCapture.CapturedAtCycles),
			TEXT("1"),
			TEXT("1"),
			TEXT("settled"));
		const StateSemantic::FUeAgentStateRevision Revision(
			LegacyCapture.SourceId,
			LegacyCapture.Generation,
			LegacyCapture.Sequence);
		TArray<FUeAgentUeReadEntry> Entries;
		Entries.Reserve(LegacyCapture.Entries.Num());
		for (const FUeAgentGenericReadEntry& Entry
			: LegacyCapture.Entries)
		{
			if (!AppendTypedEntry(
					Entry,
					Revision,
					CapturedAt,
					Resources,
					Entries,
					OutError))
			{
				OutError = AtBinding(MoveTemp(OutError), Request.GetBinding());
				return false;
			}
		}

		OutCapture = MakeShared<const FUeAgentUeReadCapture>(
			LegacyCapture.SnapshotId,
			Request.GetStatePlane(),
			Revision,
			CapturedAt,
			LegacyCapture.EvidenceId,
			LegacyCapture.bSettled,
			MoveTemp(Entries));
		if (!OutCapture->IsValid())
		{
			OutError = AtBinding(OutCapture->GetValidationDiagnostic(), Request.GetBinding());
			OutCapture.Reset();
			return false;
		}
		return true;
	}
}
