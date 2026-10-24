#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadNative.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadDescriptors.h"
#include "DomainAdapters/ContentGraphRead/UeAgentExternalLocalizationContentGraphProvider.h"

#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/HjsonWorkspace/UeAgentGraphAuthoringProjection.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Core/Contract/UeAgentExecutionContextPolicy.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Misc/ScopeExit.h"

namespace UE::UAI::DomainAdapters::ContentGraphRead
{
	using UE::UAI::ContextResource::
		EUeAgentResourceDimension;
	using UE::UAI::ContextResource::
		EUeAgentExecutionWorldType;
	using UE::UAI::ContextResource::
		EUeAgentRequestKind;
	using UE::UAI::ContextResource::
		FUeAgentCancellationSource;
	using UE::UAI::ContextResource::
		FUeAgentCancellationRequested;
	using UE::UAI::ContextResource::
		FUeAgentExecutionContextError;
	using UE::UAI::ContextResource::
		FUeAgentExecutionContextInput;
	using UE::UAI::ContextResource::
		FUeAgentExecutionContextRef;
	using UE::UAI::ContextResource::
		FUeAgentExecutionContextRefPtr;
	using UE::UAI::ContextResource::
		FUeAgentOperationContext;
	using UE::UAI::ContextResource::
		FUeAgentOperationContextError;
	using UE::UAI::ContextResource::
		FUeAgentOperationDeadline;
	using UE::UAI::ContextResource::
		FUeAgentOperationDeadlineExceeded;
	using UE::UAI::ContextResource::
		FUeAgentResourceChargeError;
	using UE::UAI::Contract::FUeAgentContractValue;
	using UE::UAI::Discovery::FUeAgentCandidateBinding;
	using namespace UE::UAI::StateSemantic;

	namespace
	{
		FUeAgentContentGraphReadError NativeReadFact(const FString& Code, const FString& Reason,
			const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& Subject = FString())
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("content_capture"), FString(), Subject);
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Path;
			Error.ActualValue = MoveTemp(Actual);
			if (!Error.ActualValue.IsValid()) Error.ActualValue = MakeShared<FJsonValueString>(TEXT("<missing>"));
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}

		FString NativeReadKind(EUeAgentValueKind Kind)
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
			default: return TEXT("unknown(") + FString::FromInt(static_cast<int32>(Kind)) + TEXT(")");
			}
		}

		void SupplementNativeReadRow(FUeAgentContentGraphReadError& Error, const FString& Target, const FString& Name)
		{
			if (Error.IsEmpty()) return;
			if (Error.TargetPath.IsEmpty()) Error.TargetPath = Target;
			if (Error.CandidateName.IsEmpty()) Error.CandidateName = Name;
			if (Error.SourceKind.IsEmpty() && Error.Source.SourceFile.IsEmpty()) Error.SourceKind = TEXT("native_result");
			for (auto& Child : Error.Diagnostics) SupplementNativeReadRow(Child, Target, Name);
			if (Error.PublicCause)
			{
				auto Cause = MakeShared<UE::UAI::Contract::FDiagnostic>(*Error.PublicCause);
				SupplementNativeReadRow(*Cause, Target, Name);
				Error.PublicCause = MoveTemp(Cause);
			}
		}

		bool Fail(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic)
		{
			OutError = MoveTemp(Diagnostic);
			return false;
		}

		bool ParseInput(
			const FUeAgentContractValue& Input,
			TSharedPtr<FJsonObject>& OutObject,
			FUeAgentContentGraphReadError& OutError)
		{
			OutObject.Reset();
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(Input.GetJsonValue());
			if (!FJsonSerializer::Deserialize(Reader, OutObject)
				|| !OutObject.IsValid())
			{
				return Fail(OutError, NativeReadFact(TEXT("content_graph_read_input_invalid"),
					TEXT("The canonical query input is not valid object JSON. ") + Reader->GetErrorMessage(),
					TEXT("/canonical_input"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueString>(TEXT("object JSON"))));
			}
			return true;
		}

		TArray<FString> ReadStringArray(
			const TSharedPtr<FJsonObject>& Input,
			const TCHAR* Field)
		{
			TArray<FString> Result;
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (Input.IsValid()
				&& Input->TryGetArrayField(Field, Values)
				&& Values)
			{
				for (const TSharedPtr<FJsonValue>& Value : *Values)
				{
					if (Value.IsValid()
						&& Value->Type == EJson::String)
					{
						Result.Add(Value->AsString());
					}
				}
			}
			return Result;
		}

		bool ReadBoolean(
			const TSharedPtr<FJsonObject>& Input,
			const TCHAR* Field,
			bool bDefault = false)
		{
			bool bValue = bDefault;
			if (Input.IsValid())
			{
				Input->TryGetBoolField(Field, bValue);
			}
			return bValue;
		}

		TSet<FString> AsSet(const TArray<FString>& Values)
		{
			TSet<FString> Result;
			for (const FString& Value : Values)
			{
				Result.Add(Value);
			}
			return Result;
		}

		bool CheckContext(
			const FUeAgentOperationContext& Context,
			const FString& Stage,
			FUeAgentContentGraphReadError& OutError)
		{
			FUeAgentCancellationRequested CancellationError;
			if (!Context.GetCancellationToken().TryCheckpoint(
					CancellationError))
			{
				return Fail(OutError, CancellationError);
			}
			FUeAgentResourceChargeError ChargeError;
			auto Scope = Context.Scope(Stage);
			if (!Scope.TryCharge(
					EUeAgentResourceDimension::WorkUnits,
					1,
					ChargeError))
			{
				return Fail(OutError, ChargeError);
			}
			return true;
		}

		FUeAgentTemporalPoint ObservedAt(
			const FUeAgentOperationContext& Context)
		{
			return FUeAgentTemporalPoint(
				TEXT("ue.operation_context:")
					+ Context.GetExecutionContext()
						.GetContextRevision(),
				TEXT("1"));
		}

		FUeAgentStateRevision Revision(
			const FUeAgentContentGraphCapture& Capture)
		{
			if (Capture.CanonicalRevision.IsSet())
			{
				return Capture.CanonicalRevision.GetValue();
			}
			return FUeAgentStateRevision(
				TEXT("content-graph-read"),
				Capture.Revision,
				1);
		}

		FUeAgentStateDetails Details(
			const FString& DisplayName,
			const FString& Section)
		{
			return FUeAgentStateDetails(
				DisplayName,
				TEXT(
					"Typed fact captured from the explicit "
					"ContentGraphRead environment"),
				{TEXT("ContentGraphRead"), Section},
				false,
				TEXT("available"),
				{});
		}

		bool MatchesCanonicalType(
			const FUeAgentCanonicalValuePtr& Value,
			const FUeAgentCanonicalType& Type)
		{
			return Value.IsValid()
				&& (Value->GetKind() == Type.GetValueKind()
					|| (Value->GetKind()
							== EUeAgentValueKind::Null
						&& Type.IsNullable()));
		}

		bool MatchesCanonicalType(
			const FUeAgentCanonicalValueRef& Value,
			const FUeAgentCanonicalType& Type)
		{
			return Value->GetKind() == Type.GetValueKind()
				|| (Value->GetKind() == EUeAgentValueKind::Null
					&& Type.IsNullable());
		}

		bool ValidateFieldState(
			const FUeAgentContentGraphFieldState& State,
			const FString& SubjectId,
			FUeAgentContentGraphReadError& OutError)
		{
			if (!State.Type.IsSet())
			{
				return Fail(OutError, NativeReadFact(TEXT("content_graph_read_state_type_missing"), TEXT("The captured field state has no declared type."),
					TEXT("/state/type"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), SubjectId));
			}
			if (!State.Value.IsValid())
			{
				return Fail(OutError, NativeReadFact(TEXT("content_graph_read_state_value_missing"), TEXT("The captured field state has no canonical value."),
					TEXT("/state/value"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), SubjectId));
			}
			if (!State.Default.IsSet())
			{
				return Fail(OutError, NativeReadFact(TEXT("content_graph_read_state_default_missing"), TEXT("The captured field state has no declared default."),
					TEXT("/state/default"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), SubjectId));
			}
			const FUeAgentCanonicalType& Type =
				State.Type.GetValue();
			const FUeAgentDefaultValue& Default =
				State.Default.GetValue();
			if (!Type.IsValid())
			{
				return Fail(OutError, Type.GetValidationDiagnostic());
			}
			if (!Default.IsValid())
			{
				return Fail(OutError, Default.GetValidationDiagnostic());
			}
			if (State.OverrideState
					== EUeAgentOverrideState::Overridden
				&& State.AuthoredPresence
					!= EUeAgentAuthoredPresence::Present)
			{
				return Fail(OutError, NativeReadFact(TEXT("content_graph_read_state_presence_invalid"), TEXT("An overridden field state must be authored."),
					TEXT("/state/authored_presence"), MakeShared<FJsonValueString>(
						State.AuthoredPresence == EUeAgentAuthoredPresence::Present ? TEXT("present")
						: State.AuthoredPresence == EUeAgentAuthoredPresence::Absent ? TEXT("absent") : *FString::FromInt(static_cast<int32>(State.AuthoredPresence))),
					MakeShared<FJsonValueString>(TEXT("present")), SubjectId));
			}
			if (State.OverrideState
					== EUeAgentOverrideState::Inherited
				&& State.AuthoredPresence
					!= EUeAgentAuthoredPresence::Absent)
			{
				return Fail(OutError, NativeReadFact(TEXT("content_graph_read_state_presence_invalid"), TEXT("An inherited field state must not be authored."),
					TEXT("/state/authored_presence"), MakeShared<FJsonValueString>(
						State.AuthoredPresence == EUeAgentAuthoredPresence::Present ? TEXT("present")
						: State.AuthoredPresence == EUeAgentAuthoredPresence::Absent ? TEXT("absent") : *FString::FromInt(static_cast<int32>(State.AuthoredPresence))),
					MakeShared<FJsonValueString>(TEXT("absent")), SubjectId));
			}
			if (!MatchesCanonicalType(State.Value, Type)
				|| !MatchesCanonicalType(
					Default.GetValue(),
					Type))
			{
				auto Actual = MakeShared<FJsonObject>();
				Actual->SetStringField(TEXT("value_kind"), NativeReadKind(State.Value->GetKind()));
				Actual->SetStringField(TEXT("default_kind"), NativeReadKind(Default.GetValue()->GetKind()));
				auto Expected = MakeShared<FJsonObject>();
				Expected->SetStringField(TEXT("kind"), NativeReadKind(Type.GetValueKind()));
				Expected->SetBoolField(TEXT("nullable"), Type.IsNullable());
				auto Diagnostic = NativeReadFact(TEXT("content_graph_read_state_type_mismatch"),
					TEXT("The captured value or default has a different canonical kind from the declared field type."),
					TEXT("/state"), MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueObject>(Expected), SubjectId);
				Diagnostic.InternalContext = Type.GetTypeId();
				return Fail(OutError, MoveTemp(Diagnostic));
			}
			return true;
		}

		bool AddFieldStateEntry(
			const FString& SemanticId,
			const TMap<FString, FString>& Selectors,
			const FString& Section,
			const FString& OwnerId,
			const FString& ObjectRole,
			const FUeAgentContentGraphFieldState& State,
			const FUeAgentContentGraphCapture& Capture,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& OutEntries,
			FUeAgentContentGraphReadError& OutError)
		{
			if (OwnerId.IsEmpty())
				return Fail(OutError, NativeReadFact(TEXT("content_graph_read_state_owner_missing"),
					TEXT("A captured state entry requires a non-empty owner selector."), TEXT("/state/owner"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), SemanticId));
			if (!ValidateFieldState(State, SemanticId, OutError)) return false;
			if (!CheckContext(
					Context,
					TEXT("content_graph_read.entry.") + Section,
					OutError))
			{
				return false;
			}
			TMap<FString, FString> EntrySelectors = Selectors;
			EntrySelectors.Add(TEXT("section"), Section);
			const FUeAgentStateRevision StateRevision =
				Revision(Capture);
			const FUeAgentStateEntry Entry(
				FUeAgentCanonicalIdentity(
					SemanticId,
					Capture.DomainId,
					OwnerId,
					MoveTemp(EntrySelectors),
					ObjectRole,
					StateRevision.GetGeneration()),
				State.Type.GetValue(),
				State.Value.ToSharedRef(),
				State.Default.GetValue(),
				State.AuthoringDetails.IsSet() ? State.AuthoringDetails.GetValue() : Details(SemanticId, Section),
				StateRevision,
				State.AuthoredPresence,
				State.OverrideState,
				State.AuthoringDescriptor);
			if (!Entry.IsValid())
			{
				return Fail(OutError, Entry.GetValidationDiagnostic());
			}
			OutEntries.Add(Entry);
			return true;
		}

		bool AddFieldStateEntry(
			const FString& SemanticId,
			const FString& Selector,
			const FString& Section,
			const FString& OwnerId,
			const FUeAgentContentGraphFieldState& State,
			const FUeAgentContentGraphCapture& Capture,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& OutEntries,
			FUeAgentContentGraphReadError& OutError)
		{
			return AddFieldStateEntry(
				SemanticId,
				{{TEXT("selector"), Selector}},
				Section,
				OwnerId,
				TEXT("state"),
				State,
				Capture,
				Context,
				OutEntries,
				OutError);
		}

		bool AddSemanticValueEntry(
			const FUeAgentContentGraphSemanticIdentity& Identity,
			const FString& Section,
			const FString& OwnerId,
			const FUeAgentCanonicalValuePtr& Value,
			const FUeAgentContentGraphCapture& Capture,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& OutEntries,
			FUeAgentContentGraphReadError& OutError)
		{
			if (!Identity.IsExplicit() || !Value.IsValid())
			{
				auto Facts = MakeShared<FJsonObject>();
				Facts->SetBoolField(TEXT("semantic_type_present"), !Identity.SemanticId.IsEmpty());
				Facts->SetNumberField(TEXT("selector_count"), Identity.Selectors.Num());
				Facts->SetBoolField(TEXT("object_role_present"), !Identity.ObjectRole.IsEmpty());
				Facts->SetBoolField(TEXT("value_present"), Value.IsValid());
				auto Diagnostic = NativeReadFact(TEXT("content_graph_read_semantic_fact_invalid"),
					TEXT("The captured semantic fact requires a semantic type, selectors, object role and canonical value."),
					TEXT("/semantic_fact"), MakeShared<FJsonValueObject>(Facts), MakeShared<FJsonValueString>(TEXT("all required semantic fact fields present")), OwnerId);
				Diagnostic.InternalContext = Identity.SemanticId;
				return Fail(OutError, MoveTemp(Diagnostic));
			}
			FUeAgentContentGraphFieldState State;
			State.Type = FUeAgentCanonicalType(
				TEXT("content_graph.semantic_value"),
				Value->GetKind(),
				false,
				Value->GetKind() == EUeAgentValueKind::Array
					? TEXT("array")
					: Value->GetKind() == EUeAgentValueKind::Object
						? TEXT("object")
						: TEXT("scalar"));
			State.Value = Value;
			State.Default = FUeAgentDefaultValue(
				EUeAgentDefaultSourceKind::DomainNative,
				OwnerId,
				TEXT("content-graph-read"),
				Value.ToSharedRef());
			return AddFieldStateEntry(
				Identity.SemanticId,
				Identity.Selectors,
				Section,
				OwnerId,
				Identity.ObjectRole,
				State,
				Capture,
				Context,
				OutEntries,
				OutError);
		}

		bool AddValueEntry(
			const FString& SemanticId,
			const FString& Selector,
			const FString& Section,
			const FString& TypeId,
			EUeAgentValueKind Kind,
			const FUeAgentCanonicalValuePtr& Value,
			const FUeAgentContentGraphCapture& Capture,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& OutEntries,
			FUeAgentContentGraphReadError& OutError)
		{
			if (!Value.IsValid())
			{
				return Fail(OutError, NativeReadFact(TEXT("content_graph_read_value_invalid"),
					TEXT("The selected captured field has no canonical value."), Selector,
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
					Capture.Owner ? Capture.Owner->GetPathName() : FString()));
			}
			FUeAgentContentGraphFieldState State;
			State.Type = FUeAgentCanonicalType(
				TypeId,
				Kind,
				false,
				Kind == EUeAgentValueKind::Object ? TEXT("object")
					: Kind == EUeAgentValueKind::Array ? TEXT("array") : TEXT("scalar"));
			State.Value = Value;
			State.Default = FUeAgentDefaultValue(
				EUeAgentDefaultSourceKind::DomainNative,
				TEXT("ue-native"),
				TEXT("content-graph-read"),
				Value.ToSharedRef());
			return AddFieldStateEntry(
				SemanticId,
				Selector,
				Section,
				Capture.Owner->GetPathName(),
				State,
				Capture,
				Context,
				OutEntries,
				OutError);
		}

		bool AddString(
			const FString& SemanticId,
			const FString& Selector,
			const FString& Section,
			const FString& Text,
			const FUeAgentContentGraphCapture& Capture,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& OutEntries,
			FUeAgentContentGraphReadError& OutError)
		{
			FUeAgentCanonicalValuePtr Value;
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TryString(
					Text,
					Context.GetResourceTracker(),
					Value,
					StateError))
			{
				return Fail(OutError, StateError);
			}
			return AddValueEntry(
				SemanticId,
				Selector,
				Section,
				TEXT("core.string"),
				EUeAgentValueKind::String,
				Value,
				Capture,
				Context,
				OutEntries,
				OutError);
		}

		bool AddBooleanForOwner(
			const FString& SemanticId,
			const FString& Selector,
			const FString& Section,
			bool bValue,
			const FString& OwnerId,
			const FUeAgentContentGraphCapture& Capture,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& OutEntries,
			FUeAgentContentGraphReadError& OutError)
		{
			FUeAgentCanonicalValuePtr Value;
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TryBoolean(
					bValue,
					Context.GetResourceTracker(),
					Value,
					StateError))
			{
				return Fail(OutError, StateError);
			}
			FUeAgentContentGraphFieldState State;
			State.Type = FUeAgentCanonicalType(
				TEXT("core.boolean"),
				EUeAgentValueKind::Boolean,
				false,
				TEXT("scalar"));
			State.Value = Value;
			State.Default = FUeAgentDefaultValue(
				EUeAgentDefaultSourceKind::DomainNative,
				TEXT("ue-native"),
				TEXT("content-graph-read"),
				Value.ToSharedRef());
			return AddFieldStateEntry(
				SemanticId,
				Selector,
				Section,
				OwnerId,
				State,
				Capture,
				Context,
				OutEntries,
				OutError);
		}

		bool AddBoolean(
			const FString& SemanticId,
			const FString& Selector,
			const FString& Section,
			bool bValue,
			const FUeAgentContentGraphCapture& Capture,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& OutEntries,
			FUeAgentContentGraphReadError& OutError)
		{
			return AddBooleanForOwner(
				SemanticId,
				Selector,
				Section,
				bValue,
				Capture.Owner->GetPathName(),
				Capture,
				Context,
				OutEntries,
				OutError);
		}

		bool AddInteger(
			const FString& SemanticId,
			const FString& Selector,
			const FString& Section,
			int64 Number,
			const FUeAgentContentGraphCapture& Capture,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& OutEntries,
			FUeAgentContentGraphReadError& OutError)
		{
			FUeAgentCanonicalValuePtr Value;
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TryInteger(
					LexToString(Number),
					Context.GetResourceTracker(),
					Value,
					StateError))
			{
				return Fail(OutError, StateError);
			}
			return AddValueEntry(
				SemanticId,
				Selector,
				Section,
				TEXT("core.integer"),
				EUeAgentValueKind::Integer,
				Value,
				Capture,
				Context,
				OutEntries,
				OutError);
		}

		bool BuildSnapshot(
			const FString& ProviderBinding,
			const FUeAgentContentGraphCapture& Capture,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry> Entries,
			TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
			FUeAgentContentGraphReadError& OutError)
		{
			ON_SCOPE_EXIT { SupplementNativeReadRow(OutError, Capture.Owner ? Capture.Owner->GetPathName() : FString(), FString()); };
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalSnapshot::TryCreate(
					FString::Printf(
						TEXT("%s:%s"),
						*ProviderBinding,
						*Context.GetRequestId()),
					Context.GetExecutionContext().GetStatePlane(),
					Revision(Capture),
					ObservedAt(Context),
					FUeAgentStateProvenance(
						TEXT("independent_readback"),
						ProviderBinding,
						Capture.Owner->GetPathName()),
					Entries,
					Context.GetResourceTracker(),
					OutSnapshot,
					StateError))
			{
				return Fail(OutError, StateError);
			}
			return true;
		}

		

		bool ValidateCapture(
			const FUeAgentContentGraphCapture& Capture,
			FUeAgentContentGraphReadError& OutError)
		{
			if (!IsValid(Capture.Owner))
			{
				return Fail(OutError, NativeReadFact(TEXT("content_graph_read_owner_invalid"),
					TEXT("Content-graph capture requires a live owner object."), TEXT("/owner"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
			}
			if (Capture.DomainId.IsEmpty()
				|| Capture.Revision.IsEmpty()
				|| (Capture.CanonicalRevision.IsSet()
					&& !Capture.CanonicalRevision->IsValid()))
			{
				auto Facts = MakeShared<FJsonObject>();
				Facts->SetBoolField(TEXT("domain_present"), !Capture.DomainId.IsEmpty());
				Facts->SetBoolField(TEXT("revision_present"), !Capture.Revision.IsEmpty());
				if (Capture.CanonicalRevision.IsSet()) Facts->SetBoolField(TEXT("canonical_revision_valid"), Capture.CanonicalRevision->IsValid());
				auto Diagnostic = NativeReadFact(TEXT("content_graph_read_capture_identity_missing"),
					TEXT("The capture requires a domain, revision and a valid canonical revision when one is supplied."),
					TEXT("/capture/identity"), MakeShared<FJsonValueObject>(Facts),
					MakeShared<FJsonValueString>(TEXT("domain and revision present; supplied canonical revision valid")), Capture.Owner->GetPathName());
				if (Capture.Owner->IsAsset()) Diagnostic.TargetPath = Capture.Owner->GetPathName();
				return Fail(OutError, MoveTemp(Diagnostic));
			}

			TSet<FString> Seen;
			auto NoField = [](const auto&) { return FString(); };
			auto ValidateIdentity = [&](const auto& Value, const auto& Values,
				const TCHAR* Kind, const TCHAR* Collection, auto ReadId, auto ReadField)
			{
				const FString Id = ReadId(Value);
				const bool bMissing = Id.IsEmpty();
				const FString Key = FString(Kind) + TEXT(":") + Id;
				if (!bMissing && !Seen.Contains(Key))
				{
					Seen.Add(Key);
					return true;
				}
				OutError = UE::UAI::Contract::FDiagnostic::Failure(
					bMissing ? TEXT("content_graph_read_fact_id_missing") : TEXT("content_graph_read_fact_id_duplicate"),
					FString::Printf(TEXT("Content capture has a %s %s identity."), bMissing ? TEXT("missing") : TEXT("duplicate"), Kind),
					TEXT("content_capture"), Kind, bMissing ? FString(Kind) : Id);
				OutError.TargetPath = Capture.Owner->GetPathName();
				OutError.SourceKind = TEXT("native_result");
				// Build public locations where the identity error is detected. No later
				// stage needs to decode private identities or reconstruct this reason.
				for (int32 Index = 0; Index < Values.Num(); ++Index)
				{
					if (ReadId(Values[Index]) != Id) continue;
					auto Location = UE::UAI::Contract::FDiagnostic::Failure(OutError.Code, OutError.PublicMessage, OutError.Stage);
					Location.TargetPath = OutError.TargetPath;
					Location.SourceKind = TEXT("native_result");
					Location.PropertyPath = FString::Printf(TEXT("/capture/%s/%d"), Collection, Index);
					const FString Field = ReadField(Values[Index]);
					if (!Field.IsEmpty()) Location.PropertyPath += TEXT("/") + Field.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
					OutError.Diagnostics.Add(MoveTemp(Location));
				}
				return false;
			};
			for (const FUeAgentContentGraphContainer& Value : Capture.Containers)
			{
				ON_SCOPE_EXIT { SupplementNativeReadRow(OutError, Capture.Owner->GetPathName(), FString()); };
				if (!ValidateIdentity(Value, Capture.Containers, TEXT("container"), TEXT("containers"),
						[](const auto& V) { return V.ContainerId; }, NoField)) return false;
				if (Value.ContainerKind.IsEmpty() || Value.OwnerId.IsEmpty())
				{
					auto Actual = MakeShared<FJsonObject>();
					Actual->SetStringField(TEXT("kind"), Value.ContainerKind);
					Actual->SetBoolField(TEXT("owner_present"), !Value.OwnerId.IsEmpty());
					return Fail(OutError, NativeReadFact(TEXT("content_graph_read_container_invalid"),
						TEXT("Captured container requires kind, owner."), TEXT("/capture/containers"),
						MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("nonempty required fields"))));
				}
			}
			for (const FUeAgentContentGraphItem& Value : Capture.Items)
			{
				ON_SCOPE_EXIT { SupplementNativeReadRow(OutError, Capture.Owner->GetPathName(), FString()); };
				if (!ValidateIdentity(Value, Capture.Items, TEXT("item"), TEXT("items"),
						[](const auto& V) { return V.ItemId; }, NoField)) return false;
				if (Value.ItemKind.IsEmpty() || Value.OwnerId.IsEmpty() || Value.ClassPath.IsEmpty())
				{
					auto Actual = MakeShared<FJsonObject>();
					Actual->SetStringField(TEXT("kind"), Value.ItemKind);
					Actual->SetBoolField(TEXT("owner_present"), !Value.OwnerId.IsEmpty());
					Actual->SetStringField(TEXT("class"), Value.ClassPath);
					return Fail(OutError, NativeReadFact(TEXT("content_graph_read_item_invalid"),
						TEXT("Captured item requires kind, owner, class."), TEXT("/capture/items"),
						MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("nonempty required fields"))));
				}
			}
			for (const FUeAgentContentGraphRelation& Value : Capture.Relations)
			{
				ON_SCOPE_EXIT { SupplementNativeReadRow(OutError, Capture.Owner->GetPathName(), Value.RelationKind); };
				if (!ValidateIdentity(Value, Capture.Relations, TEXT("relation"), TEXT("relations"),
						[](const auto& V) { return V.RelationId; }, NoField)) return false;
				if (Value.RelationKind.IsEmpty() || Value.SourceItemId.IsEmpty() || Value.TargetItemId.IsEmpty())
				{
					auto Actual = MakeShared<FJsonObject>();
					Actual->SetStringField(TEXT("kind"), Value.RelationKind);
					Actual->SetBoolField(TEXT("source_present"), !Value.SourceItemId.IsEmpty());
					Actual->SetBoolField(TEXT("target_present"), !Value.TargetItemId.IsEmpty());
					return Fail(OutError, NativeReadFact(TEXT("content_graph_read_relation_invalid"),
						TEXT("Captured relation requires kind, from, to."), TEXT("/capture/relations"),
						MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("nonempty required fields"))));
				}
			}
			for (const FUeAgentContentGraphProperty& Value : Capture.Properties)
			{
				ON_SCOPE_EXIT { SupplementNativeReadRow(OutError, Capture.Owner->GetPathName(), Value.FieldId); };
				if (Value.OwnerId.IsEmpty() || Value.FieldId.IsEmpty())
				{
					auto Actual = MakeShared<FJsonObject>();
					Actual->SetBoolField(TEXT("owner_present"), !Value.OwnerId.IsEmpty());
					Actual->SetStringField(TEXT("field"), Value.FieldId);
					return Fail(OutError, NativeReadFact(TEXT("content_graph_read_property_invalid"),
						TEXT("Captured property requires an owner and a nonempty field name."), TEXT("/capture/properties"),
						MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("owner and field present"))));
				}
				if (!ValidateIdentity(Value, Capture.Properties, TEXT("property"), TEXT("properties"),
						[](const auto& V) { return V.OwnerId + TEXT(":") + V.FieldId; }, [](const auto& V) { return V.FieldId; })) return false;
				if (!ValidateFieldState(Value.State, Value.OwnerId + TEXT(":") + Value.FieldId, OutError)) return false;
			}
			for (const FUeAgentContentGraphParameter& Value : Capture.Parameters)
			{
				ON_SCOPE_EXIT { SupplementNativeReadRow(OutError, Capture.Owner->GetPathName(), FString()); };
				if (!ValidateIdentity(Value, Capture.Parameters, TEXT("parameter"), TEXT("parameters"),
						[](const auto& V) { return V.ParameterId; }, NoField)) return false;
				if (Value.OwnerId.IsEmpty())
				{
					auto Actual = MakeShared<FJsonObject>();
					Actual->SetBoolField(TEXT("owner_present"), !Value.OwnerId.IsEmpty());
					return Fail(OutError, NativeReadFact(TEXT("content_graph_read_parameter_invalid"),
						TEXT("Captured parameter requires owner."), TEXT("/capture/parameters"),
						MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("nonempty required fields"))));
				}
				if (!ValidateFieldState(Value.State, Value.ParameterId, OutError)) return false;
			}
			for (const FUeAgentContentGraphDiagnostic& Value : Capture.Diagnostics)
			{
				ON_SCOPE_EXIT { SupplementNativeReadRow(OutError, Capture.Owner->GetPathName(), Value.Code); };
				if (!ValidateIdentity(Value, Capture.Diagnostics, TEXT("diagnostic"), TEXT("diagnostics"),
						[](const auto& V) { return V.DiagnosticId; }, NoField)) return false;
				if (Value.OwnerId.IsEmpty() || Value.Severity.IsEmpty() || Value.Code.IsEmpty())
				{
					auto Actual = MakeShared<FJsonObject>();
					Actual->SetBoolField(TEXT("owner_present"), !Value.OwnerId.IsEmpty());
					Actual->SetStringField(TEXT("severity"), Value.Severity);
					Actual->SetStringField(TEXT("code"), Value.Code);
					return Fail(OutError, NativeReadFact(TEXT("content_graph_read_diagnostic_invalid"),
						TEXT("Captured diagnostic requires owner, severity, code."), TEXT("/capture/diagnostics"),
						MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("nonempty required fields"))));
				}
			}
			return true;
		}

		

		FString ReadPublicString(
			const FUeAgentCanonicalValuePtr& State,
			const FString& FieldName)
		{
			if (!State.IsValid()
				|| State->GetKind() != EUeAgentValueKind::Object)
			{
				return FString();
			}
			for (const auto& Field : State->GetObjectItems())
			{
				if (Field.Key == FieldName
					&& Field.Value->GetKind() == EUeAgentValueKind::String)
				{
					return Field.Value->GetText();
				}
			}
			return FString();
		}

		FString ReadPublicName(
			const FUeAgentCanonicalValuePtr& State)
		{
			return ReadPublicString(State, TEXT("name"));
		}

		bool ProjectPublicCaptureNames(FUeAgentContentGraphCapture& Capture,
			const FUeAgentOperationContext& Context, FUeAgentContentGraphReadError& OutError)
		{
			// Capture is also the authoring baseline. Only the query view consumes the
			// aliases already allocated by each domain; never rename native state there.
			auto Project = [&](auto& Record)
			{
				const FString* Name = Record.SemanticIdentity.Selectors.Find(TEXT("public_name"));
				if (!Name || Name->IsEmpty() || ReadPublicName(Record.State) == *Name) return true;
				if (!Record.State.IsValid() || Record.State->GetKind() != EUeAgentValueKind::Object)
					return Fail(OutError, NativeReadFact(TEXT("content_graph_read_named_state_not_object"),
						TEXT("A named captured item or container requires an object state."), TEXT("/name"),
						MakeShared<FJsonValueString>(Record.State ? NativeReadKind(Record.State->GetKind()) : TEXT("<missing>")),
						MakeShared<FJsonValueString>(TEXT("object")), *Name));
				FUeAgentCanonicalValuePtr PublicName, PublicState;
				FUeAgentStateSemanticError Error;
				if (!FUeAgentCanonicalValue::TryString(*Name, Context.GetResourceTracker(), PublicName, Error))
					return Fail(OutError, MoveTemp(Error));
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
				for (const auto& Field : Record.State->GetObjectItems())
					if (Field.Key != TEXT("name")) Fields.Add(Field);
				Fields.Emplace(TEXT("name"), PublicName.ToSharedRef());
				if (!FUeAgentCanonicalValue::TryObject(Fields, Context.GetResourceTracker(), PublicState, Error))
					return Fail(OutError, MoveTemp(Error));
				Record.State = MoveTemp(PublicState);
				return true;
			};
			for (auto& Container : Capture.Containers) if (!Project(Container)) return false;
			for (auto& Item : Capture.Items) if (!Project(Item)) return false;
			return true;
		}

		FString PublicContainerName(const FUeAgentContentGraphCapture& Capture, const FString& Id)
		{
			const auto* Container = Capture.Containers.FindByPredicate(
				[&](const auto& Value) { return Value.ContainerId == Id; });
			return Container ? ReadPublicName(Container->State) : FString();
		}

		bool MatchesPublicItemName(const FUeAgentContentGraphCapture& Capture,
			const FUeAgentContentGraphItem& Item, const FString& Name)
		{
			const FString ItemName = ReadPublicName(Item.State);
			if (ItemName == Name) return true;
			const FString Container = PublicContainerName(Capture, Item.ContainerId);
			return !Container.IsEmpty() && !ItemName.IsEmpty() && Container + TEXT("/") + ItemName == Name;
		}

		template <typename TMatches>
		FUeAgentContentGraphReadError SelectorFailure(const FUeAgentContentGraphCapture& Capture,
			const FString& Name, const FString& Kind, const FString& InputPath, const TMatches& Matches)
		{
			auto Actual = MakeShared<FJsonObject>();
			Actual->SetNumberField(TEXT("match_count"), Matches.Num());
			TArray<TSharedPtr<FJsonValue>> Candidates;
			for (const auto& Container : Capture.Containers)
			{
				if (!Matches.Contains(Container.ContainerId)) continue;
				auto Candidate = MakeShared<FJsonObject>();
				Candidate->SetStringField(TEXT("container"), ReadPublicName(Container.State));
				Candidate->SetStringField(TEXT("kind"), Container.ContainerKind);
				Candidates.Add(MakeShared<FJsonValueObject>(Candidate));
			}
			for (const auto& Item : Capture.Items)
			{
				if (!Matches.Contains(Item.ItemId)) continue;
				auto Candidate = MakeShared<FJsonObject>();
				Candidate->SetStringField(TEXT("item"), ReadPublicName(Item.State));
				Candidate->SetStringField(TEXT("kind"), Item.ItemKind);
				const auto* Container = Capture.Containers.FindByPredicate([&](const auto& Value) { return Value.ContainerId == Item.ContainerId; });
				if (Container) Candidate->SetStringField(TEXT("container"), ReadPublicName(Container->State));
				Candidates.Add(MakeShared<FJsonValueObject>(Candidate));
			}
			if (Kind == TEXT("endpoint"))
			{
				TSet<FString> Seen;
				for (const auto& Relation : Capture.Relations)
				{
					auto AddEndpoint = [&](const FUeAgentContentGraphEndpoint& Endpoint, const FString& OwnerId, const TCHAR* Side)
					{
						if (!Matches.Contains(Endpoint.Identity) || Seen.Contains(Endpoint.Identity)) return;
						Seen.Add(Endpoint.Identity);
						auto Candidate = MakeShared<FJsonObject>();
						Candidate->SetStringField(TEXT("endpoint"), Endpoint.PublicName);
						Candidate->SetStringField(TEXT("side"), Side);
						Candidate->SetStringField(TEXT("relation_kind"), Relation.RelationKind);
						const auto* Item = Capture.Items.FindByPredicate([&](const auto& Value) { return Value.ItemId == OwnerId; });
						const FString ContainerId = Item ? Item->ContainerId : OwnerId;
						const auto* Container = Capture.Containers.FindByPredicate([&](const auto& Value) { return Value.ContainerId == ContainerId; });
						if (Item) Candidate->SetStringField(TEXT("item"), ReadPublicName(Item->State));
						if (Container) Candidate->SetStringField(TEXT("container"), ReadPublicName(Container->State));
						Candidates.Add(MakeShared<FJsonValueObject>(Candidate));
					};
					AddEndpoint(Relation.SourceEndpoint, Relation.SourceItemId, TEXT("source"));
					AddEndpoint(Relation.TargetEndpoint, Relation.TargetItemId, TEXT("target"));
				}
			}
			if (!Candidates.IsEmpty()) Actual->SetArrayField(TEXT("candidates"), MoveTemp(Candidates));
			auto Diagnostic = NativeReadFact(Matches.IsEmpty() ? TEXT("content_graph_read_selector_not_found") : TEXT("content_graph_read_selector_ambiguous"),
				Matches.IsEmpty() ? FString::Printf(TEXT("%s selector '%s' has no match in the selected scope."), *Kind, *Name)
					: FString::Printf(TEXT("%s selector '%s' has multiple matches; qualify it with a container."), *Kind, *Name),
				InputPath, MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueNumber>(1));
			Diagnostic.SourceKind = TEXT("command_input");
			Diagnostic.Source.JsonPath = InputPath;
			Diagnostic.CandidateName = Name;
			if (Capture.Owner) Diagnostic.TargetPath = Capture.Owner->GetPathName();
			return Diagnostic;
		}

		FUeAgentContentGraphReadError ReadInputFact(const FString& Code, const FString& Reason,
			const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			auto Diagnostic = NativeReadFact(Code, Reason, Path, MoveTemp(Actual), MoveTemp(Expected));
			Diagnostic.SourceKind = TEXT("command_input");
			Diagnostic.Source.JsonPath = Path;
			return Diagnostic;
		}

		bool TryResolveContainerNames(
			const FUeAgentContentGraphCapture& Capture,
			const TArray<FString>& Names,
			TSet<FString>& OutIds,
			FUeAgentContentGraphReadError& OutError, const FString& InputPath = FString())
		{
			OutIds.Reset();
			for (const FString& Name : Names)
			{
				TArray<FString> Matches;
				for (const FUeAgentContentGraphContainer& Value
					: Capture.Containers)
				{
					if (ReadPublicName(Value.State) == Name)
					{
						Matches.Add(Value.ContainerId);
					}
				}
				if (Matches.Num() != 1)
				{
					return Fail(OutError, SelectorFailure(Capture, Name, TEXT("container"),
						InputPath.IsEmpty() ? TEXT("/containers/") + FString::FromInt(Names.IndexOfByKey(Name)) : InputPath, Matches));
				}
				OutIds.Add(Matches[0]);
			}
			return true;
		}

		bool TryResolveItemNames(
			const FUeAgentContentGraphCapture& Capture,
			const TSet<FString>& ContainerIds,
			const TArray<FString>& Names,
			TSet<FString>& OutIds,
			FUeAgentContentGraphReadError& OutError, const FString& InputPath = FString())
		{
			OutIds.Reset();
			for (const FString& Name : Names)
			{
				TArray<FString> Matches;
				for (const FUeAgentContentGraphItem& Value : Capture.Items)
				{
					if ((!ContainerIds.IsEmpty()
							&& !ContainerIds.Contains(Value.ContainerId))
						|| !MatchesPublicItemName(Capture, Value, Name))
					{
						continue;
					}
					Matches.Add(Value.ItemId);
				}
				if (Matches.Num() != 1)
				{
					return Fail(OutError, SelectorFailure(Capture, Name, TEXT("item"),
						InputPath.IsEmpty() ? TEXT("/items/") + FString::FromInt(Names.IndexOfByKey(Name)) : InputPath, Matches));
				}
				OutIds.Add(Matches[0]);
			}
			return true;
		}

		bool TryResolveOwnerNames(
			const FUeAgentContentGraphCapture& Capture,
			const TSet<FString>& ContainerIds,
			const TArray<FString>& Names,
			TSet<FString>& OutIds,
			FUeAgentContentGraphReadError& OutError, const FString& InputPath = FString())
		{
			OutIds.Reset();
			for (const FString& Name : Names)
			{
				if (Name == TEXT("asset"))
				{
					OutIds.Add(Capture.Owner->GetPathName());
					continue;
				}
				TSet<FString> Matches;
				for (const FUeAgentContentGraphContainer& Value
					: Capture.Containers)
				{
					if ((ContainerIds.IsEmpty() || ContainerIds.Contains(Value.ContainerId))
						&& ReadPublicName(Value.State) == Name)
					{
						Matches.Add(Value.ContainerId);
					}
				}
				for (const FUeAgentContentGraphItem& Value : Capture.Items)
				{
					if ((!ContainerIds.IsEmpty()
							&& !ContainerIds.Contains(Value.ContainerId))
						|| !MatchesPublicItemName(Capture, Value, Name))
					{
						continue;
					}
					Matches.Add(Value.ItemId);
				}
				if (Matches.Num() != 1)
				{
					return Fail(OutError, SelectorFailure(Capture, Name, TEXT("owner"),
						InputPath.IsEmpty() ? TEXT("/owners/") + FString::FromInt(Names.IndexOfByKey(Name)) : InputPath, Matches));
				}
				for (const FString& Match : Matches)
				{
					OutIds.Add(Match);
				}
			}
			return true;
		}

		struct FSelection
		{
			bool bAll = true;
			TSet<FString> Ids;

			bool Contains(const FString& Id) const
			{
				return bAll || Ids.Contains(Id);
			}
		};

		bool BuildSelection(
			const FUeAgentContentGraphCapture& Capture,
			const TArray<FString>& RootNames,
			bool bIncludeClosure,
			const FUeAgentOperationContext& Context,
			FSelection& OutSelection,
			FUeAgentContentGraphReadError& OutError)
		{
			OutSelection = {};
			if (RootNames.IsEmpty())
			{
				OutSelection.bAll = true;
				return true;
			}
			OutSelection.bAll = false;
			TSet<FString> RootIds;
			for (int32 Index = 0; Index < RootNames.Num(); ++Index)
			{
				TSet<FString> Resolved;
				if (!TryResolveOwnerNames(Capture, {}, {RootNames[Index]}, Resolved, OutError,
					TEXT("/roots/") + FString::FromInt(Index))) return false;
				RootIds.Append(Resolved);
			}
			for (const FString& RootId : RootIds)
			{
				OutSelection.Ids.Add(RootId);
			}
			if (!bIncludeClosure)
			{
				return true;
			}

			TArray<FString> Worklist = RootIds.Array();
			int32 Cursor = 0;
			while (Cursor < Worklist.Num())
			{
				const FString Current = Worklist[Cursor++];
				if (!CheckContext(
						Context,
						TEXT("content_graph_read.selection_closure"),
						OutError))
				{
					return false;
				}
				auto AddId =
					[&OutSelection, &Worklist](
						const FString& Id)
					{
						if (!Id.IsEmpty()
							&& !OutSelection.Ids.Contains(Id))
						{
							OutSelection.Ids.Add(Id);
							Worklist.Add(Id);
						}
					};
				for (const FUeAgentContentGraphContainer& Value
					: Capture.Containers)
				{
					if (Value.ContainerId == Current
						|| Value.OwnerId == Current
						|| Value.ParentContainerId == Current)
					{
						AddId(Value.ContainerId);
					}
				}
				for (const FUeAgentContentGraphItem& Value
					: Capture.Items)
				{
					if (Value.ItemId == Current
						|| Value.OwnerId == Current
						|| Value.ContainerId == Current)
					{
						AddId(Value.ItemId);
						AddId(Value.ContainerId);
					}
				}
				for (const FUeAgentContentGraphRelation& Value
					: Capture.Relations)
				{
					if (Value.RelationId == Current
						|| Value.SourceItemId == Current
						|| Value.TargetItemId == Current)
					{
						AddId(Value.RelationId);
						AddId(Value.SourceItemId);
						AddId(Value.TargetItemId);
					}
				}
			}
			return true;
		}

		bool AddSummary(
			const FUeAgentContentGraphCapture& Capture,
			const TSharedPtr<FJsonObject>& Input,
			bool bCompound,
			const FSelection& Selection,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& Entries,
			FUeAgentContentGraphReadError& OutError)
		{
			const FString OwnerId = Capture.Owner->GetPathName();
			if (!AddString(
					TEXT("content_graph.summary:owner"),
					TEXT("owner_path"),
					TEXT("summary"),
					OwnerId,
					Capture,
					Context,
					Entries,
					OutError)
				|| !AddString(
					TEXT("content_graph.summary:domain"),
					TEXT("domain_id"),
					TEXT("summary"),
					Capture.DomainId,
					Capture,
					Context,
					Entries,
					OutError))
			{
				return false;
			}

			if (bCompound
				|| ReadBoolean(Input, TEXT("include_revision")))
			{
				if (!AddString(
						TEXT("content_graph.summary:revision"),
						TEXT("revision"),
						TEXT("summary"),
						Capture.Revision,
						Capture,
						Context,
						Entries,
						OutError))
				{
					return false;
				}
			}
			if (!bCompound
				&& !ReadBoolean(Input, TEXT("include_counts")))
			{
				return true;
			}

			if (!AddBoolean(TEXT("content_graph.summary:diagnostics_collected"),
				TEXT("diagnostics_collected"), TEXT("summary"),
				Capture.DiagnosticCoverage.IsValid(), Capture, Context, Entries, OutError))
			{
				return false;
			}

			auto CountSelected =
				[&Selection](const FString& Id)
				{
					return Selection.Contains(Id) ? 1LL : 0LL;
				};
			int64 ContainerCount = 0;
			for (const FUeAgentContentGraphContainer& Value
				: Capture.Containers)
			{
				ContainerCount += CountSelected(Value.ContainerId);
			}
			int64 ItemCount = 0;
			for (const FUeAgentContentGraphItem& Value : Capture.Items)
			{
				ItemCount += CountSelected(Value.ItemId);
			}
			int64 RelationCount = 0;
			for (const FUeAgentContentGraphRelation& Value
				: Capture.Relations)
			{
				RelationCount += CountSelected(Value.RelationId);
			}
			int64 PropertyCount = 0;
			for (const FUeAgentContentGraphProperty& Value
				: Capture.Properties)
			{
				PropertyCount += CountSelected(Value.OwnerId);
			}
			int64 ParameterCount = 0;
			for (const FUeAgentContentGraphParameter& Value
				: Capture.Parameters)
			{
				ParameterCount +=
					(Selection.Contains(Value.ParameterId)
						|| Selection.Contains(Value.OwnerId))
					? 1
					: 0;
			}
			int64 DiagnosticCount = 0;
			for (const FUeAgentContentGraphDiagnostic& Value
				: Capture.Diagnostics)
			{
				DiagnosticCount +=
					(Selection.Contains(Value.DiagnosticId)
						|| Selection.Contains(Value.OwnerId))
					? 1
					: 0;
			}

			const TArray<TPair<FString, int64>> Counts = {
				{TEXT("container"), ContainerCount},
				{TEXT("item"), ItemCount},
				{TEXT("relation"), RelationCount},
				{TEXT("property"), PropertyCount},
				{TEXT("parameter"), ParameterCount},
				{TEXT("diagnostic"), DiagnosticCount},
			};
			for (const TPair<FString, int64>& Pair : Counts)
			{
				if (!AddInteger(
						TEXT("content_graph.summary:")
							+ Pair.Key
							+ TEXT("_count"),
						Pair.Key + TEXT("_count"),
						TEXT("summary"),
						Pair.Value,
						Capture,
						Context,
						Entries,
						OutError))
				{
					return false;
				}
			}
			return true;
		}

		bool AddStructure(
			const FUeAgentContentGraphCapture& Capture,
			const TSharedPtr<FJsonObject>& Input,
			bool bCompound,
			const FSelection& Selection,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& Entries,
			FUeAgentContentGraphReadError& OutError)
		{
			TSet<FString> Requested;
			if (!TryResolveContainerNames(
					Capture,
					ReadStringArray(Input, TEXT("containers")),
					Requested,
					OutError))
			{
				return false;
			}
			const bool bHierarchy = bCompound
				|| ReadBoolean(Input, TEXT("include_hierarchy"));
			const bool bOrder = bCompound
				|| ReadBoolean(Input, TEXT("include_order"));
			for (const FUeAgentContentGraphContainer& Value
				: Capture.Containers)
			{
				if (!Selection.Contains(Value.ContainerId)
					|| (!Requested.IsEmpty()
						&& !Requested.Contains(Value.ContainerId)))
				{
					continue;
				}
				ON_SCOPE_EXIT { SupplementNativeReadRow(OutError, Capture.Owner->GetPathName(), ReadPublicName(Value.State)); };
				if (Value.SemanticIdentity.IsExplicit())
				{
					if (!AddSemanticValueEntry(
						Value.SemanticIdentity,
						TEXT("structure"),
						Value.OwnerId,
						Value.State,
						Capture,
						Context,
						Entries,
						OutError))
					{
						return false;
					}
					continue;
				}
				const FString Base =
					TEXT("content_graph.container:")
					+ Value.ContainerId;
				if (!AddString(
						Base + TEXT(":kind"),
						TEXT("container_kind"),
						TEXT("structure"),
						Value.ContainerKind,
						Capture,
						Context,
						Entries,
						OutError)
					|| !AddString(
						Base + TEXT(":owner"),
						TEXT("owner_id"),
						TEXT("structure"),
						Value.OwnerId,
						Capture,
						Context,
						Entries,
						OutError))
				{
					return false;
				}
				if (bHierarchy
					&& !AddString(
						Base + TEXT(":parent"),
						TEXT("parent_container_id"),
						TEXT("structure"),
						Value.ParentContainerId,
						Capture,
						Context,
						Entries,
						OutError))
				{
					return false;
				}
				if (bOrder
					&& !AddInteger(
						Base + TEXT(":order"),
						TEXT("order"),
						TEXT("structure"),
						Value.Order,
						Capture,
						Context,
						Entries,
						OutError))
				{
					return false;
				}
			}
			return true;
		}

		TSet<FString> GetItemFields(const FUeAgentContentGraphItem& Item)
		{
			if (Item.SemanticIdentity.IsExplicit())
			{
				TSet<FString> Fields;
				if (Item.State.IsValid() && Item.State->GetKind() == EUeAgentValueKind::Object)
					for (const auto& Field : Item.State->GetObjectItems()) Fields.Add(Field.Key);
				return Fields;
			}
			return {
				TEXT("item_kind"),
				TEXT("owner_id"),
				TEXT("class_path"),
				TEXT("container_id"),
				TEXT("order"),
			};
		}

		bool ValidateItemProjectionFields(
			const FUeAgentContentGraphCapture& Capture,
			const TArray<const FUeAgentContentGraphItem*>& Selected,
			const TSet<FString>& Fields,
			FUeAgentContentGraphReadError& OutError)
		{
			if (Fields.IsEmpty()) return true;
			TArray<FString> Requested = Fields.Array(); Requested.Sort();
			TMap<FString, int32> NameCounts, ContainerNameCounts;
			TMap<FString, FString> ContainerNames;
			TMap<FString, TMap<FString, int32>> NamesInContainers;
			for (const auto& Container : Capture.Containers)
			{
				const FString Name = ReadPublicName(Container.State);
				if (!Name.IsEmpty())
				{
					ContainerNames.Add(Container.ContainerId, Name);
					++ContainerNameCounts.FindOrAdd(Name);
				}
			}
			struct FFieldGroup
			{
				FString Kind;
				TArray<TSharedPtr<FJsonValue>> Allowed, Sources;
				TArray<FString> Missing;
			};
			TArray<FFieldGroup> Groups;
			TMap<FString, int32> GroupIndices;
			TArray<const FUeAgentContentGraphItem*> CompatibleItems;
			for (const auto* Item : Selected)
			{
				if (Item->SemanticIdentity.IsExplicit()
					&& (!Item->State.IsValid() || Item->State->GetKind() != EUeAgentValueKind::Object))
				{
					auto Diagnostic = NativeReadFact(TEXT("content_graph_read_item_value_not_object"),
						FString::Printf(TEXT("Captured %s item state must be an object."), *Item->ItemKind), TEXT("/items"),
						MakeShared<FJsonValueString>(Item->State.IsValid() ? NativeReadKind(Item->State->GetKind()) : TEXT("missing")),
						MakeShared<FJsonValueString>(TEXT("object")), Item->ItemId);
					SupplementNativeReadRow(Diagnostic, Capture.Owner->GetPathName(), ReadPublicName(Item->State));
					return Fail(OutError, MoveTemp(Diagnostic));
				}
				const TSet<FString> Allowed = GetItemFields(*Item);
				TArray<FString> Missing;
				for (const auto& Field : Requested) if (!Allowed.Contains(Field)) Missing.Add(Field);
				const FString Name = ReadPublicName(Item->State);
				const FString Container = ContainerNames.FindRef(Item->ContainerId);
				if (Missing.IsEmpty())
				{
					CompatibleItems.Add(Item);
					continue;
				}
				TArray<FString> AllowedNames = Allowed.Array(); AllowedNames.Sort();
				TArray<TSharedPtr<FJsonValue>> AllowedValues;
				for (const auto& Field : AllowedNames) AllowedValues.Add(MakeShared<FJsonValueString>(Field));
				TArray<TSharedPtr<FJsonValue>> KeyParts = AllowedValues;
				KeyParts.Insert(MakeShared<FJsonValueString>(Item->ItemKind), 0);
				FString Key;
				FJsonSerializer::Serialize(KeyParts, TJsonWriterFactory<>::Create(&Key));
				int32* GroupIndex = GroupIndices.Find(Key);
				if (!GroupIndex)
				{
					FFieldGroup Group;
					Group.Kind = Item->ItemKind;
					Group.Allowed = MoveTemp(AllowedValues);
					Group.Missing = MoveTemp(Missing);
					const int32 Index = Groups.Add(MoveTemp(Group));
					GroupIndex = &GroupIndices.Add(Key, Index);
				}
				const auto Source = MakeShared<FJsonObject>();
				if (!Name.IsEmpty())
				{
					Source->SetStringField(TEXT("name"), Name);
					if (!Container.IsEmpty()) Source->SetStringField(TEXT("message"), TEXT("container=") + Container);
				}
				else
				{
					// Unnamed metadata has no public item selector. Locate it without
					// inventing a selector or repeating class/order for named items.
					Source->SetStringField(TEXT("source_kind"), Item->ItemKind);
					Source->SetStringField(TEXT("message"), FString::Printf(
						TEXT("class=%s; order=%d"), *Item->ClassPath, Item->Order)
						+ (Container.IsEmpty() ? FString() : TEXT("; container=") + Container));
				}
				Groups[*GroupIndex].Sources.Add(MakeShared<FJsonValueObject>(Source));
			}
			if (Groups.IsEmpty()) return true;
			// Recovery analysis is only needed for a failed query.
			for (const auto& Item : Capture.Items)
			{
				const FString Name = ReadPublicName(Item.State);
				if (!Name.IsEmpty())
				{
					++NameCounts.FindOrAdd(Name);
					++NamesInContainers.FindOrAdd(Item.ContainerId).FindOrAdd(Name);
				}
			}
			TMap<FString, TArray<FString>> CompatibleNames;
			for (const auto* Item : CompatibleItems)
			{
				const FString Name = ReadPublicName(Item->State);
				const FString Container = ContainerNames.FindRef(Item->ContainerId);
				// Suggestions use the same public-name equality and scope as selectors.
				if (!Name.IsEmpty() && NameCounts.FindRef(Name) == 1)
					CompatibleNames.FindOrAdd(FString()).Add(Name);
				else if (!Name.IsEmpty() && !Container.IsEmpty()
					&& ContainerNameCounts.FindRef(Container) == 1
					&& NamesInContainers.FindChecked(Item->ContainerId).FindRef(Name) == 1)
					CompatibleNames.FindOrAdd(Container).Add(Name);
			}
			UeAgentAuthoringDiagnostics::FDiagnostic Errors;
			for (const auto& Group : Groups)
			{
				for (const auto& Field : Group.Missing)
				{
					auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(
						TEXT("content_graph_read_item_field_unknown"), TEXT("/fields"),
						MakeShared<FJsonValueArray>(Group.Allowed), MakeShared<FJsonValueString>(Field), TEXT("query_projection"));
					Error.TargetPath = Capture.Owner ? Capture.Owner->GetPathName() : FString();
					Error.SourceKind = Group.Kind;
					Error.RelatedSources = Group.Sources;
					Error.PublicMessage = FString::Printf(
						TEXT("Field '%s' is unavailable for %d selected '%s' item(s); other item contracts are checked separately."),
						*Field, Group.Sources.Num(), *Group.Kind);
					Errors.Append(Error);
				}
			}
			TArray<FString> Containers; CompatibleNames.GetKeys(Containers); Containers.Sort();
			TArray<TSharedPtr<FJsonValue>> Choices;
			for (const auto& Container : Containers)
			{
				const auto Choice = MakeShared<FJsonObject>();
				TArray<TSharedPtr<FJsonValue>> Items;
				for (const auto& Name : CompatibleNames.FindChecked(Container)) Items.Add(MakeShared<FJsonValueString>(Name));
				Choice->SetArrayField(TEXT("items"), MoveTemp(Items));
				if (!Container.IsEmpty()) Choice->SetArrayField(TEXT("containers"), {MakeShared<FJsonValueString>(Container)});
				Choices.Add(MakeShared<FJsonValueObject>(Choice));
			}
			Errors.CandidateChoices = MakeShared<FJsonValueArray>(Choices);
			Errors.RecoveryAction = Choices.IsEmpty()
				? TEXT("No publicly selectable item in this selection exposes every requested field. Select the intended items and inspect their default summary before refining fields.")
				: TEXT("Keep the requested fields and refine items/containers with a compatible selection from choose. Fields project every selected item; they do not filter out incompatible items.");
			return Fail(OutError, Errors);
		}

		bool ProjectSemanticItemValue(
			const FUeAgentCanonicalValuePtr& Value,
			const TSet<FString>& Fields,
			const FUeAgentOperationContext& Context,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentContentGraphReadError& OutError,
			bool bCompound = false,
			bool bProjectPinAliases = true)
		{
			OutValue.Reset();
			if (!Value.IsValid()
				|| Value->GetKind() != EUeAgentValueKind::Object)
			{
				return Fail(OutError, NativeReadFact(TEXT("content_graph_read_item_value_not_object"),
					TEXT("A captured semantic item must have an object value."), TEXT("/item/value"),
					MakeShared<FJsonValueString>(Value ? NativeReadKind(Value->GetKind()) : TEXT("<missing>")), MakeShared<FJsonValueString>(TEXT("object"))));
			}

			FUeAgentCanonicalValuePtr CapturedInterface, CapturedDefaults, VisibleInterface, VisibleDefaults;
			for (const auto& Field : Value->GetObjectItems())
			{
				if (Field.Key == TEXT("pin_interface")) CapturedInterface = Field.Value;
				else if (Field.Key == TEXT("pin_defaults")) CapturedDefaults = Field.Value;
			}
			if (bProjectPinAliases && CapturedInterface && CapturedInterface->GetKind() == EUeAgentValueKind::Object
				&& (bCompound || Fields.Contains(TEXT("pin_interface"))
					|| (CapturedDefaults && (Fields.IsEmpty() || Fields.Contains(TEXT("pin_defaults"))))))
			{
				TSharedPtr<FJsonValue> Json;
				UE::UAI::Contract::FDiagnostic Error;
				HjsonWorkspace::FWorkspaceError ProjectionError;
				TSharedRef<FJsonObject> Interface = MakeShared<FJsonObject>();
				TMap<FString, FString> AliasBySelector;
				if (!CanonicalJson::TryToJsonValue(*CapturedInterface, Json, Error)) return Fail(OutError, MoveTemp(Error));
				if (!HjsonWorkspace::TryProjectGraphAuthoringPinInterface(Json->AsObject().ToSharedRef(), Interface, ProjectionError, &AliasBySelector))
					return Fail(OutError, ProjectionError.GetDiagnostic());
				if (!CanonicalJson::TryFromJsonValue(MakeShared<FJsonValueObject>(Interface),
					Context.GetResourceTracker(), VisibleInterface, Error)) return Fail(OutError, MoveTemp(Error));
				if (CapturedDefaults && CapturedDefaults->GetKind() == EUeAgentValueKind::Object && !AliasBySelector.IsEmpty())
				{
					if (!CanonicalJson::TryToJsonValue(*CapturedDefaults, Json, Error)) return Fail(OutError, MoveTemp(Error));
					const auto Defaults = HjsonWorkspace::ProjectGraphAuthoringPinDefaults(Json->AsObject().ToSharedRef(), AliasBySelector);
					if (!CanonicalJson::TryFromJsonValue(MakeShared<FJsonValueObject>(Defaults),
						Context.GetResourceTracker(), VisibleDefaults, Error)) return Fail(OutError, MoveTemp(Error));
				}
			}
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Projected;
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item :
				Value->GetObjectItems())
			{
				const bool bExplicitlyRequested = Fields.Contains(Item.Key);
				if ((!Fields.IsEmpty() && !bExplicitlyRequested)
					|| (Fields.IsEmpty() && !bCompound
						&& Item.Key == TEXT("pin_interface")))
				{
					continue;
				}
				if (Item.Key == TEXT("pin_interface") && VisibleInterface)
					Projected.Emplace(Item.Key, VisibleInterface.ToSharedRef());
				else if (Item.Key == TEXT("pin_defaults") && VisibleDefaults)
					Projected.Emplace(Item.Key, VisibleDefaults.ToSharedRef());
				else Projected.Add(Item);
			}

			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryObject(
					Projected,
					Context.GetResourceTracker(),
					OutValue,
					Error))
			{
				return Fail(OutError, MoveTemp(Error));
			}
			return true;
		}

		bool AddItems(
			const FUeAgentContentGraphCapture& Capture,
			const TSharedPtr<FJsonObject>& Input,
			bool bCompound,
			const FSelection& Selection,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& Entries,
			FUeAgentContentGraphReadError& OutError,
			bool bProjectPinAliases = true)
		{
			TSet<FString> ContainerIds;
			if (!TryResolveContainerNames(
					Capture,
					ReadStringArray(Input, TEXT("containers")),
					ContainerIds,
					OutError))
			{
				return false;
			}
			TSet<FString> ItemIds;
			if (!TryResolveItemNames(
					Capture,
					ContainerIds,
					ReadStringArray(Input, TEXT("items")),
					ItemIds,
					OutError))
			{
				return false;
			}
			const TSet<FString> ItemKinds =
				AsSet(ReadStringArray(Input, TEXT("item_kinds")));
			const TSet<FString> Fields =
				AsSet(ReadStringArray(Input, TEXT("fields")));
			auto Wants =
				[&Fields, bCompound](const TCHAR* Field)
				{
					return bCompound
						|| Fields.IsEmpty()
						|| Fields.Contains(Field);
				};
			TArray<const FUeAgentContentGraphItem*> SelectedItems;
			for (const FUeAgentContentGraphItem& Value : Capture.Items)
			{
				if (!Selection.Contains(Value.ItemId)
					|| (!ContainerIds.IsEmpty()
						&& !ContainerIds.Contains(Value.ContainerId))
					|| (!ItemIds.IsEmpty()
						&& !ItemIds.Contains(Value.ItemId))
					|| (!ItemKinds.IsEmpty()
						&& !ItemKinds.Contains(Value.ItemKind)))
				{
					continue;
				}
				SelectedItems.Add(&Value);
			}
			if (!bCompound && !ValidateItemProjectionFields(Capture, SelectedItems, Fields, OutError)) return false;
			for (const auto* Item : SelectedItems)
			{
				const FUeAgentContentGraphItem& Value = *Item;
				ON_SCOPE_EXIT { SupplementNativeReadRow(OutError, Capture.Owner->GetPathName(), ReadPublicName(Value.State)); };
				if (Value.SemanticIdentity.IsExplicit())
				{
					FUeAgentCanonicalValuePtr ProjectedValue = Value.State;
					if (!ProjectSemanticItemValue(
							Value.State,
							bCompound ? TSet<FString>{} : Fields,
							Context,
							ProjectedValue,
							OutError, bCompound, bProjectPinAliases))
					{
						return false;
					}
					if (!AddSemanticValueEntry(
						Value.SemanticIdentity,
						TEXT("items"),
						Value.OwnerId,
						ProjectedValue,
						Capture,
						Context,
						Entries,
						OutError))
					{
						return false;
					}
					continue;
				}
				const FString Base =
					TEXT("content_graph.item:") + Value.ItemId;
				if (Wants(TEXT("item_kind"))
					&& !AddString(
						Base + TEXT(":kind"),
						TEXT("item_kind"),
						TEXT("items"),
						Value.ItemKind,
						Capture,
						Context,
						Entries,
						OutError))
				{
					return false;
				}
				if (Wants(TEXT("owner_id"))
					&& !AddString(
						Base + TEXT(":owner"),
						TEXT("owner_id"),
						TEXT("items"),
						Value.OwnerId,
						Capture,
						Context,
						Entries,
						OutError))
				{
					return false;
				}
				if (Wants(TEXT("class_path"))
					&& !AddString(
						Base + TEXT(":class"),
						TEXT("class_path"),
						TEXT("items"),
						Value.ClassPath,
						Capture,
						Context,
						Entries,
						OutError))
				{
					return false;
				}
				if (Wants(TEXT("container_id"))
					&& !AddString(
						Base + TEXT(":container"),
						TEXT("container_id"),
						TEXT("items"),
						Value.ContainerId,
						Capture,
						Context,
						Entries,
						OutError))
				{
					return false;
				}
				if (Wants(TEXT("order"))
					&& !AddInteger(
						Base + TEXT(":order"),
						TEXT("order"),
						TEXT("items"),
						Value.Order,
						Capture,
						Context,
						Entries,
						OutError))
				{
					return false;
				}
			}
			return true;
		}

		bool TryResolveRelationEndpoints(const FUeAgentContentGraphCapture& Capture,
			const TSet<FString>& Containers, const TArray<FString>& Names,
			TSet<FString>& Owners, TSet<FString>& Endpoints, FUeAgentContentGraphReadError& OutError)
		{
			for (const FString& Name : Names)
			{
				TSet<FString> Matches;
				auto Match = [&](const FUeAgentContentGraphEndpoint& Endpoint, const FString& Owner)
				{
					if (Endpoint.Identity.IsEmpty()) return;
					const auto* Item = Capture.Items.FindByPredicate([&](const auto& Value) { return Value.ItemId == Owner; });
					const FString Container = PublicContainerName(Capture, Item ? Item->ContainerId : Owner);
					if (Endpoint.PublicName != Name && (Container.IsEmpty()
						|| Container + TEXT("/") + Endpoint.PublicName != Name)) return;
					if (!Containers.IsEmpty() && !Containers.Contains(Owner)
						&& !Capture.Items.ContainsByPredicate([&](const auto& Item) {
							return Item.ItemId == Owner && Containers.Contains(Item.ContainerId); })) return;
					Matches.Add(Endpoint.Identity);
				};
				for (const auto& Relation : Capture.Relations)
				{
					Match(Relation.SourceEndpoint, Relation.SourceItemId);
					Match(Relation.TargetEndpoint, Relation.TargetItemId);
				}
				if (Matches.Num() > 1)
					return Fail(OutError, SelectorFailure(Capture, Name, TEXT("endpoint"),
						TEXT("/endpoints/") + FString::FromInt(Names.IndexOfByKey(Name)), Matches));
				if (Matches.Num() == 1) { Endpoints.Append(Matches); continue; }
				TSet<FString> ResolvedOwners;
				if (!TryResolveOwnerNames(Capture, Containers, {Name}, ResolvedOwners, OutError,
					TEXT("/endpoints/") + FString::FromInt(Names.IndexOfByKey(Name)))) return false;
				Owners.Append(ResolvedOwners);
			}
			return true;
		}

		bool AddRelations(
			const FUeAgentContentGraphCapture& Capture,
			const TSharedPtr<FJsonObject>& Input,
			bool bCompound,
			const FSelection& Selection,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& Entries,
			FUeAgentContentGraphReadError& OutError)
		{
			TSet<FString> ContainerIds;
			if (!TryResolveContainerNames(
					Capture,
					ReadStringArray(Input, TEXT("containers")),
					ContainerIds,
					OutError))
			{
				return false;
			}
			TSet<FString> EndpointOwners, Endpoints;
			if (!TryResolveRelationEndpoints(Capture, ContainerIds,
				ReadStringArray(Input, TEXT("endpoints")), EndpointOwners, Endpoints, OutError)) return false;
			const TSet<FString> RelationKinds =
				AsSet(ReadStringArray(Input, TEXT("relation_kinds")));
			for (const FUeAgentContentGraphRelation& Value
				: Capture.Relations)
			{
				bool bInSelectedContainer = ContainerIds.IsEmpty();
				if (!bInSelectedContainer)
				{
					for (const FUeAgentContentGraphItem& Item : Capture.Items)
					{
						if ((Item.ItemId == Value.SourceItemId
								|| Item.ItemId == Value.TargetItemId)
							&& ContainerIds.Contains(Item.ContainerId))
						{
							bInSelectedContainer = true;
							break;
						}
					}
				}
				if (!Selection.Contains(Value.RelationId)
					|| !bInSelectedContainer
					|| ((!Endpoints.IsEmpty() || !EndpointOwners.IsEmpty())
						&& !EndpointOwners.Contains(Value.SourceItemId)
						&& !EndpointOwners.Contains(Value.TargetItemId)
						&& !Endpoints.Contains(Value.SourceEndpoint.Identity)
						&& !Endpoints.Contains(Value.TargetEndpoint.Identity))
					|| (!RelationKinds.IsEmpty()
						&& !RelationKinds.Contains(
							Value.RelationKind)))
				{
					continue;
				}
				ON_SCOPE_EXIT { SupplementNativeReadRow(OutError, Capture.Owner->GetPathName(), Value.RelationKind); };
				if (Value.SemanticIdentity.IsExplicit())
				{
					if (!AddSemanticValueEntry(
						Value.SemanticIdentity,
						TEXT("relations"),
						Capture.Owner->GetPathName(),
						Value.State,
						Capture,
						Context,
						Entries,
						OutError))
					{
						return false;
					}
					continue;
				}
				const FString Base =
					TEXT("content_graph.relation:")
					+ Value.RelationId;
				if (!AddString(
						Base + TEXT(":kind"),
						TEXT("relation_kind"),
						TEXT("relations"),
						Value.RelationKind,
						Capture,
						Context,
						Entries,
						OutError)
					|| !AddString(
						Base + TEXT(":source"),
						TEXT("source_item_id"),
						TEXT("relations"),
						Value.SourceItemId,
						Capture,
						Context,
						Entries,
						OutError)
					|| !AddString(
						Base + TEXT(":target"),
						TEXT("target_item_id"),
						TEXT("relations"),
						Value.TargetItemId,
						Capture,
						Context,
						Entries,
						OutError))
				{
					return false;
				}
			}
			return true;
		}

		bool AddProperties(
			const FUeAgentContentGraphCapture& Capture,
			const TSharedPtr<FJsonObject>& Input,
			bool bCompound,
			const FSelection& Selection,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& Entries,
			FUeAgentContentGraphReadError& OutError)
		{
			TSet<FString> ContainerIds;
			if (!TryResolveContainerNames(
					Capture,
					ReadStringArray(Input, TEXT("containers")),
					ContainerIds,
					OutError))
			{
				return false;
			}
			TSet<FString> OwnerIds;
			if (!bCompound && !TryResolveOwnerNames(
					Capture,
					ContainerIds,
					ReadStringArray(Input, TEXT("owners")),
					OwnerIds,
					OutError))
			{
				return false;
			}
			const TSet<FString> FieldIds =
				AsSet(ReadStringArray(Input, TEXT("fields")));
			const bool bPresence = !bCompound
				&& ReadBoolean(Input, TEXT("include_presence"));
			TSet<FString> Found;
			for (const FUeAgentContentGraphProperty& Value
				: Capture.Properties)
			{
				if (!Selection.Contains(Value.OwnerId)
					|| (!bCompound
						&& !OwnerIds.Contains(Value.OwnerId))
					|| (!FieldIds.IsEmpty()
						&& !FieldIds.Contains(Value.FieldId)))
				{
					continue;
				}
				const FString Key =
					Value.OwnerId + TEXT("\x1f") + Value.FieldId;
				Found.Add(Key);
				ON_SCOPE_EXIT { SupplementNativeReadRow(OutError, Capture.Owner->GetPathName(), Value.FieldId); };
				if (Value.SemanticIdentity.IsExplicit())
				{
					if (!AddFieldStateEntry(
						Value.SemanticIdentity.SemanticId,
						Value.SemanticIdentity.Selectors,
						TEXT("properties"),
						Value.OwnerId,
						Value.SemanticIdentity.ObjectRole,
						Value.State,
						Capture,
						Context,
						Entries,
						OutError))
					{
						return false;
					}
					continue;
				}
				const FString Base =
					TEXT("content_graph.property:")
					+ Value.OwnerId
					+ TEXT(":")
					+ Value.FieldId;
				if (!AddFieldStateEntry(
						Base + TEXT(":value"),
						TEXT("property.") + Value.FieldId,
						TEXT("properties"),
						Value.OwnerId,
						Value.State,
						Capture,
						Context,
						Entries,
						OutError))
				{
					return false;
				}
			}
			if (!bPresence || bCompound || FieldIds.IsEmpty())
			{
				return true;
			}
			for (const FString& OwnerId : OwnerIds)
			{
				for (const FString& FieldId : FieldIds)
				{
					const FString Key =
						OwnerId + TEXT("\x1f") + FieldId;
					if (Found.Contains(Key))
					{
						continue;
					}
					const FString Base =
						TEXT("content_graph.property:")
						+ OwnerId
						+ TEXT(":")
						+ FieldId;
					if (!AddBooleanForOwner(
							Base + TEXT(":presence"),
							TEXT("presence.") + FieldId,
							TEXT("properties"),
							false,
							OwnerId,
							Capture,
							Context,
							Entries,
							OutError))
					{
						return false;
					}
				}
			}
			return true;
		}

		bool AddParameters(
			const FUeAgentContentGraphCapture& Capture,
			const FSelection& Selection,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& Entries,
			FUeAgentContentGraphReadError& OutError)
		{
			for (const FUeAgentContentGraphParameter& Value
				: Capture.Parameters)
			{
				if (!Selection.Contains(Value.ParameterId)
					&& !Selection.Contains(Value.OwnerId))
				{
					continue;
				}
				ON_SCOPE_EXIT { SupplementNativeReadRow(OutError, Capture.Owner->GetPathName(), Value.SemanticIdentity.Selectors.FindRef(TEXT("name"))); };
				if (Value.SemanticIdentity.IsExplicit())
				{
					if (!AddFieldStateEntry(
						Value.SemanticIdentity.SemanticId,
						Value.SemanticIdentity.Selectors,
						TEXT("parameters"),
						Value.OwnerId,
						Value.SemanticIdentity.ObjectRole,
						Value.State,
						Capture,
						Context,
						Entries,
						OutError))
					{
						return false;
					}
					continue;
				}
				const FString Base =
					TEXT("content_graph.parameter:")
					+ Value.ParameterId;
				if (!AddFieldStateEntry(
						Base + TEXT(":value"),
						TEXT("value"),
						TEXT("parameters"),
						Value.OwnerId,
						Value.State,
						Capture,
						Context,
						Entries,
						OutError))
				{
					return false;
				}
			}
			return true;
		}

		bool AddDiagnostics(
			const FUeAgentContentGraphCapture& Capture,
			const TSharedPtr<FJsonObject>& Input,
			bool bCompound,
			const FSelection& Selection,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& Entries,
			FUeAgentContentGraphReadError& OutError)
		{
			ON_SCOPE_EXIT { SupplementNativeReadRow(OutError, Capture.Owner ? Capture.Owner->GetPathName() : FString(), FString()); };
			const TSet<FString> Severities =
				AsSet(ReadStringArray(Input, TEXT("severity")));
			const bool bResolved = bCompound
				|| ReadBoolean(Input, TEXT("include_resolved"));
			if (Capture.DiagnosticCoverage.IsValid()
				&& !AddValueEntry(TEXT("content_graph.diagnostics:coverage"),
					TEXT("coverage"), TEXT("diagnostics"), TEXT("object"),
					EUeAgentValueKind::Object, Capture.DiagnosticCoverage,
					Capture, Context, Entries, OutError))
			{
				return false;
			}
			for (const FUeAgentContentGraphDiagnostic& Value
				: Capture.Diagnostics)
			{
				if ((!Selection.Contains(Value.DiagnosticId)
						&& !Selection.Contains(Value.OwnerId))
					|| (!Severities.IsEmpty()
						&& !Severities.Contains(Value.Severity)))
				{
					continue;
				}
				ON_SCOPE_EXIT { SupplementNativeReadRow(OutError, Capture.Owner->GetPathName(), Value.Code); };
				if (Value.SemanticIdentity.IsExplicit())
				{
					if (!AddSemanticValueEntry(
						Value.SemanticIdentity,
						TEXT("diagnostics"),
						Value.OwnerId,
						Value.State,
						Capture,
						Context,
						Entries,
						OutError))
					{
						return false;
					}
					continue;
				}
				const FString Base =
					TEXT("content_graph.diagnostic:")
					+ Value.DiagnosticId;
				if (Value.State.IsValid())
				{
					if (!AddValueEntry(Base, TEXT("diagnostic"), TEXT("diagnostics"),
						TEXT("object"), EUeAgentValueKind::Object, Value.State,
						Capture, Context, Entries, OutError)) return false;
					continue;
				}
				if (!AddString(
						Base + TEXT(":owner"),
						TEXT("owner_id"),
						TEXT("diagnostics"),
						Value.OwnerId,
						Capture,
						Context,
						Entries,
						OutError)
					|| !AddString(
						Base + TEXT(":severity"),
						TEXT("severity"),
						TEXT("diagnostics"),
						Value.Severity,
						Capture,
						Context,
						Entries,
						OutError)
					|| !AddString(
						Base + TEXT(":code"),
						TEXT("code"),
						TEXT("diagnostics"),
						Value.Code,
						Capture,
						Context,
						Entries,
						OutError)
					|| !AddString(
						Base + TEXT(":message"),
						TEXT("message"),
						TEXT("diagnostics"),
						Value.Message,
						Capture,
						Context,
						Entries,
						OutError))
				{
					return false;
				}
				if (bResolved
					&& !AddBoolean(
						Base + TEXT(":resolved"),
						TEXT("resolved"),
						TEXT("diagnostics"),
						Value.bResolved,
						Capture,
						Context,
						Entries,
						OutError))
				{
					return false;
				}
			}
			return true;
		}

		bool IsKnownProvider(const FString& ProviderBinding)
		{
			static const TSet<FString> Providers = {
				TEXT("content_graph.read.summary"),
				TEXT("content_graph.read.structure"),
				TEXT("content_graph.read.items"),
				TEXT("content_graph.read.relations"),
				TEXT("content_graph.read.properties"),
				TEXT("content_graph.read.diagnostics"),
				TEXT("content_graph.read.compound"),
			};
			return Providers.Contains(ProviderBinding);
		}

		bool TryCaptureExternalLocalizationCapability(
			const TSharedPtr<FJsonObject>& Input,
			const FString& ProviderBinding,
			const TOptional<FUeAgentCandidateBinding>& Target,
			const FUeAgentOperationContext& Context,
			TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
			bool& bOutHandled,
			FUeAgentContentGraphReadError& OutError)
		{
			bOutHandled = false;
			FString Capability;
			if (!Input.IsValid()
				|| !Input->TryGetStringField(
					TEXT("localization_capability"), Capability))
			{
				return true;
			}
			bOutHandled = true;
			if (!IsExternalLocalizationContentGraphRoute(
					ProviderBinding, Capability))
			{
				auto Diagnostic = ReadInputFact(TEXT("localization_content_graph_route_mismatch"),
					TEXT("Localization capability requires the content graph compound query route."), TEXT("/localization_capability"),
					MakeShared<FJsonValueString>(Capability), MakeShared<FJsonValueString>(TEXT("registered localization capability on compound route")));
				Diagnostic.SubjectId = ProviderBinding;
				return Fail(OutError, MoveTemp(Diagnostic));
			}
			if (!Target.IsSet())
			{
				return Fail(OutError, ReadInputFact(TEXT("content_graph_read_target_required"),
					TEXT("Content graph capture requires a target asset."), TEXT("/target"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
			}
			UObject* Owner = StaticLoadObject(
				UObject::StaticClass(),
				nullptr,
				*Target->GetPublicRef());
			if (!IsValid(Owner))
			{
				return Fail(OutError, ReadInputFact(TEXT("content_graph_read_target_unresolved"),
					TEXT("Content graph target could not be loaded as a valid object."), TEXT("/target"),
					MakeShared<FJsonValueString>(Target->GetPublicRef()), MakeShared<FJsonValueString>(TEXT("loadable asset"))));
			}
			TSharedPtr<FJsonObject> Parameters = MakeShared<FJsonObject>();
			const TSharedPtr<FJsonObject>* Supplied = nullptr;
			if (Input->TryGetObjectField(TEXT("parameters"), Supplied)
				&& Supplied && Supplied->IsValid())
			{
				Parameters = *Supplied;
			}
			TSharedPtr<FJsonObject> Observation;
			UE::UAI::Contract::FDiagnostic NativeError;
			if (!TryExecuteExternalLocalizationContentGraph(
					Capability,
					Parameters,
					Observation,
					NativeError))
			{
				return Fail(OutError, NativeError);
			}
			if (!Observation.IsValid())
				return Fail(OutError, NativeReadFact(TEXT("localization_content_graph_observation_missing"),
					TEXT("Localization capability succeeded without an observation."), TEXT("/observation"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
			FString ObservationJson;
			const TSharedRef<TJsonWriter<TCHAR,
				TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR,
					TCondensedJsonPrintPolicy<TCHAR>>::Create(
						&ObservationJson);
			if (!FJsonSerializer::Serialize(Observation.ToSharedRef(), Writer))
			{
				auto Diagnostic = NativeReadFact(TEXT("localization_content_graph_observation_invalid"),
					TEXT("JSON serializer rejected the localization observation."), TEXT("/observation"),
					MakeShared<FJsonValueString>(TEXT("serialization failed")), MakeShared<FJsonValueString>(TEXT("serializable JSON object")));
				Diagnostic.CandidateName = Capability;
				return Fail(OutError, MoveTemp(Diagnostic));
			}
			FUeAgentContentGraphCapture Capture;
			Capture.Owner = Owner;
			Capture.DomainId = TEXT("localization-string-table");
			Capture.Revision = Context.GetExecutionContext().GetContextRevision();
			if (Capture.Revision.IsEmpty()) Capture.Revision = TEXT("observed");
			TArray<FUeAgentStateEntry> Entries;
			if (!AddString(
					TEXT("localization.capability:") + Capability,
					TEXT("observation_json"),
					TEXT("properties"),
					ObservationJson,
					Capture,
					Context,
					Entries,
					OutError))
			{
				return false;
			}
			return BuildSnapshot(
				ProviderBinding,
				Capture,
				Context,
				MoveTemp(Entries),
				OutSnapshot,
				OutError);
		}

		class FNativeHook final : public IUeAgentContentGraphReadHook
		{
		public:
			FNativeHook(
				FString InProviderBinding,
				TSharedRef<IUeAgentContentGraphReadEnvironment>
					InEnvironment)
				: ProviderBinding(MoveTemp(InProviderBinding))
				, Environment(MoveTemp(InEnvironment))
			{
			}

			bool TryCapture(
				const FUeAgentContractValue& CanonicalInput,
				const TOptional<FUeAgentCandidateBinding>& Target,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentContentGraphReadError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				ON_SCOPE_EXIT { SupplementNativeReadRow(OutError, Target.IsSet() ? Target->GetPublicRef() : FString(), FString()); };
				TSharedPtr<FJsonObject> Input;
				if (!ParseInput(CanonicalInput, Input, OutError))
				{
					return false;
				}
				bool bLocalizationHandled = false;
				if (!TryCaptureExternalLocalizationCapability(
						Input,
						ProviderBinding,
						Target,
						Context,
						OutSnapshot,
						bLocalizationHandled,
						OutError))
				{
					return false;
				}
				if (bLocalizationHandled)
				{
					return true;
				}
				if (!Target.IsSet())
				{
					return Fail(OutError, ReadInputFact(TEXT("content_graph_read_target_required"),
						TEXT("Content graph capture requires a target asset."), TEXT("/target"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
				}
				if (!CheckContext(
						Context,
						TEXT("content_graph_read.capture"),
						OutError))
				{
					return false;
				}

				FUeAgentContentGraphCapture Capture;
				if (!Environment->TryCapture(
						Target.GetValue(),
						Context.GetExecutionContext(),
						Capture,
						OutError))
				{
					return false;
				}
				const bool bCompound = ProviderBinding
					== TEXT("content_graph.read.compound");
				TSet<FString> Sections;
				if (bCompound)
				{
					Sections = AsSet(
						ReadStringArray(Input, TEXT("sections")));
				}
				else if (ProviderBinding
					== TEXT("content_graph.read.summary"))
				{
					Sections.Add(TEXT("summary"));
				}
				else if (ProviderBinding
					== TEXT("content_graph.read.structure"))
				{
					Sections.Add(TEXT("structure"));
				}
				else if (ProviderBinding
					== TEXT("content_graph.read.items"))
				{
					Sections.Add(TEXT("items"));
				}
				else if (ProviderBinding
					== TEXT("content_graph.read.relations"))
				{
					Sections.Add(TEXT("relations"));
				}
				else if (ProviderBinding
					== TEXT("content_graph.read.properties"))
				{
					Sections.Add(TEXT("properties"));
				}
				else if (ProviderBinding
					== TEXT("content_graph.read.diagnostics"))
				{
					Sections.Add(TEXT("diagnostics"));
				}
				if (Sections.IsEmpty())
				{
					return Fail(OutError, ReadInputFact(TEXT("content_graph_read_sections_missing"),
						TEXT("Content graph compound query requires at least one section."), TEXT("/sections"),
						Input->TryGetField(TEXT("sections")), MakeShared<FJsonValueString>(TEXT("nonempty sections array"))));
				}

				if ((Sections.Contains(TEXT("diagnostics"))
					|| (Sections.Contains(TEXT("summary"))
						&& (bCompound || ReadBoolean(Input, TEXT("include_counts")))))
					&& !Environment->TryCaptureDiagnostics(Capture, OutError))
				{
					return false;
				}
				if (!ValidateCapture(Capture, OutError)) return false;
				if (!ProjectPublicCaptureNames(Capture, Context, OutError)) return false;

				FSelection Selection;
				if (!BuildSelection(
						Capture,
						bCompound
							? ReadStringArray(Input, TEXT("roots"))
							: TArray<FString>(),
						bCompound
							&& ReadBoolean(
								Input,
								TEXT("include_closure")),
						Context,
						Selection,
						OutError))
				{
					return false;
				}

				TArray<FUeAgentStateEntry> Entries;
				if (Sections.Contains(TEXT("summary"))
					&& !AddSummary(
						Capture,
						Input,
						bCompound,
						Selection,
						Context,
						Entries,
						OutError))
				{
					return false;
				}
				if (Sections.Contains(TEXT("structure"))
					&& !AddStructure(
						Capture,
						Input,
						bCompound,
						Selection,
						Context,
						Entries,
						OutError))
				{
					return false;
				}
				if (Sections.Contains(TEXT("items"))
					&& !AddItems(
						Capture,
						Input,
						bCompound,
						Selection,
						Context,
						Entries,
						OutError))
				{
					return false;
				}
				if (Sections.Contains(TEXT("relations"))
					&& !AddRelations(
						Capture,
						Input,
						bCompound,
						Selection,
						Context,
						Entries,
						OutError))
				{
					return false;
				}
				if (Sections.Contains(TEXT("properties"))
					&& !AddProperties(
						Capture,
						Input,
						bCompound,
						Selection,
						Context,
						Entries,
						OutError))
				{
					return false;
				}
				if (Sections.Contains(TEXT("parameters"))
					&& !AddParameters(
						Capture,
						Selection,
						Context,
						Entries,
						OutError))
				{
					return false;
				}
				if (Sections.Contains(TEXT("diagnostics"))
					&& !AddDiagnostics(
						Capture,
						Input,
						bCompound,
						Selection,
						Context,
						Entries,
						OutError))
				{
					return false;
				}
				return BuildSnapshot(
					ProviderBinding,
					Capture,
					Context,
					MoveTemp(Entries),
					OutSnapshot,
					OutError);
			}

		private:
			const FString ProviderBinding;
			const TSharedRef<IUeAgentContentGraphReadEnvironment>
				Environment;
		};
	}

	bool FUeAgentContentGraphReadNativeHookFactory::TryCreate(
		const FString& ProviderBinding,
		TSharedRef<IUeAgentContentGraphReadEnvironment> Environment,
		TSharedPtr<IUeAgentContentGraphReadHook>& OutHook,
		FUeAgentContentGraphReadError& OutError)
	{
		OutHook.Reset();
		OutError = {};
		if (!IsKnownProvider(ProviderBinding))
		{
			return Fail(OutError, NativeReadFact(TEXT("content_graph_read_provider_unknown"),
				TEXT("No content graph read provider is registered for this binding."), TEXT("/provider_binding"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), ProviderBinding));
		}
		OutHook = MakeShared<FNativeHook>(
			ProviderBinding,
			MoveTemp(Environment));
		return true;
	}

	static bool BuildContentGraphCompoundSnapshot(
		const FUeAgentContentGraphCapture& Capture,
		const FUeAgentOperationContext& Context,
		TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
		FUeAgentContentGraphReadError& OutError,
		bool bProjectPinAliases)
	{
		OutSnapshot.Reset();
		OutError = {};
		ON_SCOPE_EXIT { SupplementNativeReadRow(OutError, Capture.Owner ? Capture.Owner->GetPathName() : FString(), FString()); };
		if (!ValidateCapture(Capture, OutError)
			|| !CheckContext(
				Context,
				TEXT("content_graph_read.compound_snapshot"),
				OutError))
		{
			return false;
		}

		FSelection Selection;
		if (!BuildSelection(
				Capture,
				{},
				true,
				Context,
				Selection,
				OutError))
		{
			return false;
		}

		const TSharedRef<FJsonObject> Input = MakeShared<FJsonObject>();
		TArray<FUeAgentStateEntry> Entries;
		if (!AddStructure(
				Capture,
				Input,
				true,
				Selection,
				Context,
				Entries,
				OutError)
			|| !AddItems(
				Capture,
				Input,
				true,
				Selection,
				Context,
				Entries,
				OutError, bProjectPinAliases)
			|| !AddRelations(
				Capture,
				Input,
				true,
				Selection,
				Context,
				Entries,
				OutError)
			|| !AddProperties(
				Capture,
				Input,
				true,
				Selection,
				Context,
				Entries,
				OutError)
			|| !AddParameters(
				Capture,
				Selection,
				Context,
				Entries,
				OutError)
			|| !AddDiagnostics(
				Capture,
				Input,
				true,
				Selection,
				Context,
				Entries,
				OutError))
		{
			return false;
		}
		return BuildSnapshot(
			TEXT("content_graph.read.compound"),
			Capture,
			Context,
			MoveTemp(Entries),
			OutSnapshot,
			OutError);
	}

	bool TryBuildContentGraphCompoundSnapshot(
		const FUeAgentContentGraphCapture& Capture,
		const FUeAgentOperationContext& Context,
		TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
		FUeAgentContentGraphReadError& OutError)
	{
		FUeAgentContentGraphCapture PublicCapture = Capture;
		OutSnapshot.Reset();
		OutError = {};
		if (!ProjectPublicCaptureNames(PublicCapture, Context, OutError)) return false;
		return BuildContentGraphCompoundSnapshot(PublicCapture, Context, OutSnapshot, OutError, true);
	}

	bool TryBuildContentGraphAuthoringSnapshot(
		const FUeAgentContentGraphCapture& Capture,
		const FUeAgentOperationContext& Context,
		TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
		FUeAgentContentGraphReadError& OutError)
	{
		FUeAgentContentGraphCapture AuthoringCapture = Capture;
		const FString RootOwnerId = Capture.Owner
			? Capture.Owner->GetPathName()
			: FString();
		AuthoringCapture.Properties.RemoveAll(
			[&RootOwnerId](const FUeAgentContentGraphProperty& Property)
			{
				return !Property.SemanticIdentity.IsExplicit()
					&& Property.OwnerId != RootOwnerId;
			});
		AuthoringCapture.Parameters.RemoveAll(
			[](const FUeAgentContentGraphParameter& Parameter)
			{
				return !Parameter.SemanticIdentity.IsExplicit();
			});
		// Authoring needs original pin selectors to restore links and defaults.
		// Readable aliases belong to the workspace/query projection, not its baseline.
		return BuildContentGraphCompoundSnapshot(
			AuthoringCapture,
			Context,
			OutSnapshot,
			OutError, false);
	}

	bool TryBuildContentGraphAuthoringSnapshot(
		const FUeAgentContentGraphCapture& Capture,
		const UE::UAI::ContextResource::FUeAgentResourceTrackerOwner& Resources,
		TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
		FUeAgentContentGraphReadError& OutError)
	{
		OutSnapshot.Reset();
		OutError = {};
		ON_SCOPE_EXIT { SupplementNativeReadRow(OutError, Capture.Owner ? Capture.Owner->GetPathName() : FString(), FString()); };
		if (!Capture.Owner || Capture.Revision.IsEmpty())
		{
			auto Actual = MakeShared<FJsonObject>();
			Actual->SetBoolField(TEXT("owner_present"), Capture.Owner != nullptr);
			Actual->SetBoolField(TEXT("revision_present"), !Capture.Revision.IsEmpty());
			return Fail(OutError, NativeReadFact(TEXT("content_graph_authoring_snapshot_capture_invalid"),
				TEXT("Authoring snapshot requires a capture owner and revision."), TEXT("/capture"),
				MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("owner and revision present"))));
		}

		TSharedPtr<const Contract::FUeAgentDescriptorRegistry> DescriptorRegistry;
		UE::UAI::Contract::FDiagnostic DescriptorError;
		if (!TryCreateContentGraphReadDescriptorRegistry(
				DescriptorRegistry,
				DescriptorError))
		{
			return Fail(OutError, DescriptorError);
		}
		const Contract::FUeAgentOperationDescriptor* Descriptor =
			DescriptorRegistry->FindByDescriptorId(
				TEXT("uai.query.content_graph.compound"));
		if (Descriptor == nullptr)
		{
			return Fail(OutError, NativeReadFact(TEXT("content_graph_authoring_snapshot_descriptor_unavailable"),
				TEXT("Content graph compound query descriptor is unavailable for authoring snapshot capture."), TEXT("/descriptor"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("uai.query.content_graph.compound")));
		}
		const Contract::FUeAgentDescriptorIdentity& DescriptorIdentity =
			Descriptor->GetIdentity();
		FUeAgentExecutionContextInput ExecutionInput;
		ExecutionInput.ProjectInstanceId = TEXT("content-graph-export-project");
		ExecutionInput.EditorInstanceId = TEXT("content-graph-export-editor");
		ExecutionInput.StatePlane = EUeAgentStatePlane::AuthoringMemory;
		ExecutionInput.WorldType = EUeAgentExecutionWorldType::Editor;
		ExecutionInput.ContextRevision = Capture.Revision;
		const Contract::FUeAgentExecutionContextPolicy ContextPolicy(
			{EUeAgentStatePlane::AuthoringMemory},
			{},
			Contract::EUeAgentContextResolutionPolicy::ResolveCurrentThenFreeze,
			Contract::EUeAgentTransientContextPolicy::ReadOnly);
		FUeAgentExecutionContextRefPtr ExecutionContext;
		FUeAgentExecutionContextError ExecutionError;
		if (!FUeAgentExecutionContextRef::TryCreate(
				ExecutionInput,
				DescriptorIdentity,
				EUeAgentRequestKind::Query,
				ContextPolicy,
				ExecutionContext,
				ExecutionError))
		{
			OutError = ExecutionError;
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Capture.Owner->GetPathName();
			return false;
		}

		const Contract::FUeAgentQueryRequest QueryRequest(
			DescriptorIdentity,
			TEXT("content-graph-authoring-snapshot"),
			Contract::FUeAgentContractValue(TEXT("{}")));
		const Contract::FUeAgentCallerContext Caller(
			TEXT("content-graph-authoring-snapshot"),
			{TEXT("uai:query")},
			TEXT("content-graph-authoring-snapshot"));
		const auto Clock = []() { return FPlatformTime::Seconds(); };
		const FUeAgentOperationDeadline Deadline;
		const FUeAgentCancellationSource Cancellation(Clock);
		TSharedPtr<FUeAgentOperationContext> OperationContext;
		FUeAgentOperationContextError OperationError;
		if (!FUeAgentOperationContext::TryCreate(
				QueryRequest,
				Caller,
				ExecutionContext.ToSharedRef(),
				Resources,
				Deadline,
				Cancellation.GetToken(),
				OperationContext,
				OperationError))
		{
			OutError = OperationError;
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Capture.Owner->GetPathName();
			return false;
		}
		return TryBuildContentGraphAuthoringSnapshot(
			Capture,
			*OperationContext,
			OutSnapshot,
			OutError);
	}
}
