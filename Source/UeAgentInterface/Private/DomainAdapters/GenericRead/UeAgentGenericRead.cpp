#include "DomainAdapters/GenericRead/UeAgentGenericRead.h"

#include "HAL/PlatformTime.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace UE::UAI::DomainAdapters::GenericRead
{
	namespace
	{
		bool Fail(
			FUeAgentGenericReadDiagnostic& OutDiagnostic,
			FString Code,
			FString Message,
			FString SubjectId = FString())
		{
			OutDiagnostic = {
				MoveTemp(Code),
				MoveTemp(Message),
				MoveTemp(SubjectId),
			};
			return false;
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
			return Fail(
				OutDiagnostic,
				TEXT("ue_read_request_invalid"),
				FString::Printf(
					TEXT("%s must be non-empty"),
					FieldName),
				FieldName);
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
				return Fail(
					OutDiagnostic,
					Cancelled.Code,
					TEXT("generic UE read was cancelled"),
					Cancelled.Reason);
			}

			ContextResource::FUeAgentOperationDeadlineExceeded Expired;
			if (!Deadline.TryCheckpoint(Stage, Expired))
			{
				return Fail(
					OutDiagnostic,
					Expired.Code,
					TEXT("generic UE read deadline expired"),
					Stage);
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
				return Fail(
					OutDiagnostic,
					TEXT("ue_read_identity_owner_mismatch"),
					TEXT("identity owner does not match the resolved UObject"),
					Identity.OwnerId);
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
				return Fail(
					OutDiagnostic,
					TEXT("ue_type_binding_mismatch"),
					TEXT("decimal lexeme binding requires FString"),
					Property.GetName());
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
				return Fail(
					OutDiagnostic,
					TEXT("ue_type_binding_mismatch"),
					TEXT("spatial point binding requires FVector"),
					Property.GetName());
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
				return Fail(
					OutDiagnostic,
					TEXT("ue_property_value_unavailable"),
					TEXT("FVector storage could not be resolved"),
					Property.GetName());
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
				return Fail(
					OutDiagnostic,
					TEXT("ue_property_not_found"),
					TEXT("reflected property was not found"),
					Binding.PropertyPath);
			}
			const UObject* DefaultObject =
				RootObject.GetClass()->GetDefaultObject();
			if (!DefaultObject)
			{
				return Fail(
					OutDiagnostic,
					TEXT("ue_native_default_unavailable"),
					TEXT("class default object was not available"),
					RootObject.GetClass()->GetPathName());
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
				return Fail(
					OutDiagnostic,
					TEXT("ue_type_binding_unsupported"),
					TEXT("generic value kind is unsupported"),
					Binding.PropertyPath);
			}
		}

		bool SameRead(
			const FUeAgentGenericReadEntry& Left,
			const FUeAgentGenericReadEntry& Right)
		{
			return Left.ValueLexeme == Right.ValueLexeme
				&& Left.DefaultLexeme == Right.DefaultLexeme
				&& Left.TypeBinding.SourceTypeId
					== Right.TypeBinding.SourceTypeId
				&& Left.Spatial.Components
					== Right.Spatial.Components
				&& Left.Spatial.DefaultComponents
					== Right.Spatial.DefaultComponents;
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
			OutError.Code = Source.Code;
			OutError.SubjectId = Source.Path;
			OutError.Detail = Source.Detail;
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
						StateError)
					|| !StateSemantic::FUeAgentCanonicalValue::TryDecimal(
						Source.DefaultLexeme,
						Resources,
						DefaultValue,
						StateError))
				{
					MapStateError(StateError, OutError);
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
						OutError)
					|| !TryCreateSpatialValue(
						Source.Spatial,
						Source.Spatial.DefaultComponents,
						CapturedAt,
						Resources,
						DefaultValue,
						OutError))
				{
					return false;
				}
				break;
			default:
				OutError = {
					TEXT("ue_type_binding_unsupported"),
					Source.Identity.Selector,
					TEXT("generic value kind is unsupported"),
				};
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
				OutEntries.Pop();
				OutError = {
					TEXT("ue_read_entry_invalid"),
					Source.Identity.Selector,
					TEXT("reflected UE fact did not satisfy the typed state contract"),
				};
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
			return Fail(
				OutDiagnostic,
				TEXT("ue_read_requires_game_thread"),
				TEXT("UObject reflection read must run on GameThread"));
		}
		if (!IsValid(Request.RootObject))
		{
			return Fail(
				OutDiagnostic,
				TEXT("ue_read_root_invalid"),
				TEXT("root UObject is invalid"));
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
			return Fail(
				OutDiagnostic,
				TEXT("ue_read_request_invalid"),
				TEXT("sequence must be non-negative"),
				TEXT("sequence"));
		}
		if (Request.Properties.IsEmpty())
		{
			return Fail(
				OutDiagnostic,
				TEXT("ue_read_request_invalid"),
				TEXT("at least one property binding is required"),
				TEXT("properties"));
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
				OutCapture = {};
				return false;
			}
			if (!SameRead(First, Second))
			{
				OutCapture = {};
				return Fail(
					OutDiagnostic,
					TEXT("ue_read_not_settled"),
					TEXT("consecutive UE reads produced different facts"),
					Binding.PropertyPath);
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
			OutError = {
				TEXT("ue_read_hook_unconfigured"),
				Request.GetBinding().GetBindingId(),
				TEXT("reflection hook requires an injected UE object/property template"),
			};
			return false;
		}
		if (!Request.IsValid())
		{
			OutError = {
				TEXT("ue_read_request_invalid"),
				Request.GetBinding().GetBindingId(),
				TEXT("typed C2 read request is invalid"),
			};
			return false;
		}

		FUeAgentGenericReadRequest LegacyRequest = ReadTemplate.GetValue();
		if (!IsValid(LegacyRequest.RootObject)
			|| Request.GetBinding().GetSummary().GetIdentity().GetOwnerId()
				!= LegacyRequest.RootObject->GetPathName())
		{
			OutError = {
				TEXT("ue_read_binding_owner_mismatch"),
				Request.GetBinding().GetBindingId(),
				TEXT("C2 binding owner does not match the injected UObject"),
			};
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
			OutError = {
				Diagnostic.Code,
				Diagnostic.SubjectId,
				Diagnostic.Message,
			};
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
			OutCapture.Reset();
			OutError = {
				TEXT("ue_read_capture_invalid"),
				Request.GetBinding().GetBindingId(),
				TEXT("reflection capture did not satisfy the typed UE read contract"),
			};
			return false;
		}
		return true;
	}
}
