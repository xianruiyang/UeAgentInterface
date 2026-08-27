#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadNative.h"

#include "DomainAdapters/ContentGraphRead/UeAgentExternalLocalizationContentGraphProvider.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::DomainAdapters::ContentGraphRead
{
	using UE::UAI::ContextResource::
		EUeAgentResourceDimension;
	using UE::UAI::ContextResource::
		FUeAgentCancellationRequested;
	using UE::UAI::ContextResource::
		FUeAgentOperationContext;
	using UE::UAI::ContextResource::
		FUeAgentOperationDeadlineExceeded;
	using UE::UAI::ContextResource::
		FUeAgentResourceBudgetExceeded;
	using UE::UAI::Contract::FUeAgentContractValue;
	using UE::UAI::Discovery::FUeAgentCandidateBinding;
	using namespace UE::UAI::StateSemantic;

	namespace
	{
		bool Fail(
			FUeAgentContentGraphReadError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError = {};
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
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
				return Fail(
					OutError,
					TEXT("content_graph_read_input_invalid"),
					FString(),
					TEXT("canonical input is not a JSON object"));
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
			FUeAgentOperationDeadlineExceeded DeadlineError;
			if (!Context.GetDeadline().TryCheckpoint(
					Stage,
					DeadlineError))
			{
				return Fail(
					OutError,
					DeadlineError.Code,
					Stage,
					TEXT("operation deadline exceeded"));
			}
			FUeAgentCancellationRequested CancellationError;
			if (!Context.GetCancellationToken().TryCheckpoint(
					CancellationError))
			{
				return Fail(
					OutError,
					CancellationError.Code,
					Stage,
					CancellationError.Reason);
			}
			FUeAgentResourceBudgetExceeded BudgetError;
			auto Scope = Context.Scope(Stage);
			if (!Scope.TryCharge(
					EUeAgentResourceDimension::WorkUnits,
					1,
					BudgetError))
			{
				return Fail(
					OutError,
					BudgetError.Code,
					BudgetError.Path,
					TEXT("content graph work budget exceeded"));
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
				return Fail(
					OutError,
					TEXT(
						"content_graph_read_state_type_missing"),
					SubjectId);
			}
			if (!State.Value.IsValid())
			{
				return Fail(
					OutError,
					TEXT(
						"content_graph_read_state_value_missing"),
					SubjectId);
			}
			if (!State.Default.IsSet())
			{
				return Fail(
					OutError,
					TEXT(
						"content_graph_read_state_default_missing"),
					SubjectId);
			}
			const FUeAgentCanonicalType& Type =
				State.Type.GetValue();
			const FUeAgentDefaultValue& Default =
				State.Default.GetValue();
			if (!Type.IsValid())
			{
				return Fail(
					OutError,
					TEXT(
						"content_graph_read_state_type_invalid"),
					SubjectId);
			}
			if (!Default.IsValid())
			{
				return Fail(
					OutError,
					TEXT(
						"content_graph_read_state_default_invalid"),
					SubjectId);
			}
			if (State.OverrideState
					== EUeAgentOverrideState::Overridden
				&& State.AuthoredPresence
					!= EUeAgentAuthoredPresence::Present)
			{
				return Fail(
					OutError,
					TEXT(
						"content_graph_read_state_presence_invalid"),
					SubjectId,
					TEXT(
						"overridden state must be authored"));
			}
			if (State.OverrideState
					== EUeAgentOverrideState::Inherited
				&& State.AuthoredPresence
					!= EUeAgentAuthoredPresence::Absent)
			{
				return Fail(
					OutError,
					TEXT(
						"content_graph_read_state_presence_invalid"),
					SubjectId,
					TEXT(
						"inherited state cannot be authored"));
			}
			if (!MatchesCanonicalType(State.Value, Type)
				|| !MatchesCanonicalType(
					Default.GetValue(),
					Type))
			{
				return Fail(
					OutError,
					TEXT(
						"content_graph_read_state_type_mismatch"),
					SubjectId,
					Type.GetTypeId());
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
			if (OwnerId.IsEmpty()
				|| !ValidateFieldState(
					State,
					SemanticId,
					OutError))
			{
				return OwnerId.IsEmpty()
					? Fail(
						OutError,
						TEXT(
							"content_graph_read_state_owner_missing"),
						SemanticId)
					: false;
			}
			if (!CheckContext(
					Context,
					TEXT("content_graph_read.entry.") + Section,
					OutError))
			{
				return false;
			}
			const FUeAgentStateRevision StateRevision =
				Revision(Capture);
			const FUeAgentStateEntry Entry(
				FUeAgentCanonicalIdentity(
					SemanticId,
					Capture.DomainId,
					OwnerId,
					Selectors,
					ObjectRole,
					StateRevision.GetGeneration()),
				State.Type.GetValue(),
				State.Value.ToSharedRef(),
				State.Default.GetValue(),
				Details(SemanticId, Section),
				StateRevision,
				State.AuthoredPresence,
				State.OverrideState);
			if (!Entry.IsValid())
			{
				return Fail(
					OutError,
					TEXT("content_graph_read_entry_invalid"),
					OwnerId,
					SemanticId);
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
				return Fail(
					OutError,
					TEXT("content_graph_read_semantic_fact_invalid"),
					OwnerId,
					Identity.SemanticId);
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
				return Fail(
					OutError,
					TEXT("content_graph_read_value_invalid"),
					Capture.Owner
						? Capture.Owner->GetPathName()
						: FString(),
					Selector);
			}
			FUeAgentContentGraphFieldState State;
			State.Type = FUeAgentCanonicalType(
				TypeId,
				Kind,
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
				return Fail(
					OutError,
					StateError.Code,
					StateError.Path,
					StateError.Detail);
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
				return Fail(
					OutError,
					StateError.Code,
					StateError.Path,
					StateError.Detail);
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
				return Fail(
					OutError,
					StateError.Code,
					StateError.Path,
					StateError.Detail);
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
				return Fail(
					OutError,
					StateError.Code,
					StateError.Path,
					StateError.Detail);
			}
			return true;
		}

		bool ValidateUniqueId(
			const FString& Id,
			const TCHAR* Kind,
			TSet<FString>& Seen,
			FUeAgentContentGraphReadError& OutError)
		{
			if (Id.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("content_graph_read_fact_id_missing"),
					Kind);
			}
			const FString Key = FString(Kind) + TEXT(":") + Id;
			if (Seen.Contains(Key))
			{
				return Fail(
					OutError,
					TEXT("content_graph_read_fact_id_duplicate"),
					Id,
					Kind);
			}
			Seen.Add(Key);
			return true;
		}

		bool ValidateCapture(
			const FUeAgentContentGraphCapture& Capture,
			FUeAgentContentGraphReadError& OutError)
		{
			if (!IsValid(Capture.Owner))
			{
				return Fail(
					OutError,
					TEXT("content_graph_read_owner_invalid"));
			}
			if (Capture.DomainId.IsEmpty()
				|| Capture.Revision.IsEmpty()
				|| (Capture.CanonicalRevision.IsSet()
					&& !Capture.CanonicalRevision->IsValid()))
			{
				return Fail(
					OutError,
					TEXT("content_graph_read_capture_identity_missing"),
					Capture.Owner->GetPathName());
			}

			TSet<FString> Seen;
			for (const FUeAgentContentGraphContainer& Value
				: Capture.Containers)
			{
				if (!ValidateUniqueId(
						Value.ContainerId,
						TEXT("container"),
						Seen,
						OutError)
					|| Value.ContainerKind.IsEmpty()
					|| Value.OwnerId.IsEmpty())
				{
					return OutError.Code.IsEmpty()
						? Fail(
							OutError,
							TEXT(
								"content_graph_read_container_invalid"),
							Value.ContainerId)
						: false;
				}
			}
			for (const FUeAgentContentGraphItem& Value : Capture.Items)
			{
				if (!ValidateUniqueId(
						Value.ItemId,
						TEXT("item"),
						Seen,
						OutError)
					|| Value.ItemKind.IsEmpty()
					|| Value.OwnerId.IsEmpty()
					|| Value.ClassPath.IsEmpty())
				{
					return OutError.Code.IsEmpty()
						? Fail(
							OutError,
							TEXT("content_graph_read_item_invalid"),
							Value.ItemId)
						: false;
				}
			}
			for (const FUeAgentContentGraphRelation& Value
				: Capture.Relations)
			{
				if (!ValidateUniqueId(
						Value.RelationId,
						TEXT("relation"),
						Seen,
						OutError)
					|| Value.RelationKind.IsEmpty()
					|| Value.SourceItemId.IsEmpty()
					|| Value.TargetItemId.IsEmpty())
				{
					return OutError.Code.IsEmpty()
						? Fail(
							OutError,
							TEXT(
								"content_graph_read_relation_invalid"),
							Value.RelationId)
						: false;
				}
			}
			for (const FUeAgentContentGraphProperty& Value
				: Capture.Properties)
			{
				const FString SubjectId =
					Value.OwnerId + TEXT(":") + Value.FieldId;
				if (Value.OwnerId.IsEmpty()
					|| Value.FieldId.IsEmpty()
					|| !ValidateUniqueId(
						SubjectId,
						TEXT("property"),
						Seen,
						OutError)
					|| !ValidateFieldState(
						Value.State,
						SubjectId,
						OutError))
				{
					return OutError.Code.IsEmpty()
						? Fail(
							OutError,
							TEXT(
								"content_graph_read_property_invalid"),
							Value.OwnerId,
							Value.FieldId)
						: false;
				}
			}
			for (const FUeAgentContentGraphParameter& Value
				: Capture.Parameters)
			{
				if (!ValidateUniqueId(
						Value.ParameterId,
						TEXT("parameter"),
						Seen,
						OutError)
					|| Value.OwnerId.IsEmpty()
					|| !ValidateFieldState(
						Value.State,
						Value.ParameterId,
						OutError))
				{
					return OutError.Code.IsEmpty()
						? Fail(
							OutError,
							TEXT(
								"content_graph_read_parameter_invalid"),
							Value.ParameterId)
						: false;
				}
			}
			for (const FUeAgentContentGraphDiagnostic& Value
				: Capture.Diagnostics)
			{
				if (!ValidateUniqueId(
						Value.DiagnosticId,
						TEXT("diagnostic"),
						Seen,
						OutError)
					|| Value.OwnerId.IsEmpty()
					|| Value.Severity.IsEmpty()
					|| Value.Code.IsEmpty())
				{
					return OutError.Code.IsEmpty()
						? Fail(
							OutError,
							TEXT(
								"content_graph_read_diagnostic_invalid"),
							Value.DiagnosticId)
						: false;
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
			const TArray<FString>& RootIds,
			bool bIncludeClosure,
			const FUeAgentOperationContext& Context,
			FSelection& OutSelection,
			FUeAgentContentGraphReadError& OutError)
		{
			OutSelection = {};
			if (RootIds.IsEmpty())
			{
				OutSelection.bAll = true;
				return true;
			}
			OutSelection.bAll = false;
			for (const FString& RootId : RootIds)
			{
				OutSelection.Ids.Add(RootId);
			}
			if (!bIncludeClosure)
			{
				return true;
			}

			TArray<FString> Worklist = RootIds;
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
			const TSet<FString> Requested =
				AsSet(ReadStringArray(Input, TEXT("container_ids")));
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

		bool ValidateItemFields(
			const TSet<FString>& Fields,
			FUeAgentContentGraphReadError& OutError)
		{
			static const TSet<FString> Allowed = {
				TEXT("item_kind"),
				TEXT("owner_id"),
				TEXT("class_path"),
				TEXT("container_id"),
				TEXT("order"),
			};
			for (const FString& Field : Fields)
			{
				if (!Allowed.Contains(Field))
				{
					return Fail(
						OutError,
						TEXT("content_graph_read_item_field_unknown"),
						Field);
				}
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
			FUeAgentContentGraphReadError& OutError)
		{
			const TSet<FString> ItemIds =
				AsSet(ReadStringArray(Input, TEXT("item_ids")));
			const TSet<FString> ItemKinds =
				AsSet(ReadStringArray(Input, TEXT("item_kinds")));
			const TSet<FString> Fields =
				AsSet(ReadStringArray(Input, TEXT("field_ids")));
			if (!bCompound && !ValidateItemFields(Fields, OutError))
			{
				return false;
			}
			auto Wants =
				[&Fields, bCompound](const TCHAR* Field)
				{
					return bCompound
						|| Fields.IsEmpty()
						|| Fields.Contains(Field);
				};
			for (const FUeAgentContentGraphItem& Value : Capture.Items)
			{
				if (!Selection.Contains(Value.ItemId)
					|| (!ItemIds.IsEmpty()
						&& !ItemIds.Contains(Value.ItemId))
					|| (!ItemKinds.IsEmpty()
						&& !ItemKinds.Contains(Value.ItemKind)))
				{
					continue;
				}
				if (Value.SemanticIdentity.IsExplicit())
				{
					if (!AddSemanticValueEntry(
						Value.SemanticIdentity,
						TEXT("items"),
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

		bool AddRelations(
			const FUeAgentContentGraphCapture& Capture,
			const TSharedPtr<FJsonObject>& Input,
			bool bCompound,
			const FSelection& Selection,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& Entries,
			FUeAgentContentGraphReadError& OutError)
		{
			const TSet<FString> RelationIds =
				AsSet(ReadStringArray(Input, TEXT("relation_ids")));
			const TSet<FString> EndpointIds =
				AsSet(ReadStringArray(Input, TEXT("endpoint_ids")));
			const TSet<FString> RelationKinds =
				AsSet(ReadStringArray(Input, TEXT("relation_kinds")));
			for (const FUeAgentContentGraphRelation& Value
				: Capture.Relations)
			{
				if (!Selection.Contains(Value.RelationId)
					|| (!RelationIds.IsEmpty()
						&& !RelationIds.Contains(Value.RelationId))
					|| (!EndpointIds.IsEmpty()
						&& !EndpointIds.Contains(Value.SourceItemId)
						&& !EndpointIds.Contains(Value.TargetItemId))
					|| (!RelationKinds.IsEmpty()
						&& !RelationKinds.Contains(
							Value.RelationKind)))
				{
					continue;
				}
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
			const TSet<FString> OwnerIds =
				AsSet(ReadStringArray(Input, TEXT("owner_ids")));
			const TSet<FString> FieldIds =
				AsSet(ReadStringArray(Input, TEXT("field_ids")));
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
			const TSet<FString> Severities =
				AsSet(ReadStringArray(Input, TEXT("severity")));
			const bool bResolved = bCompound
				|| ReadBoolean(Input, TEXT("include_resolved"));
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
				return Fail(
					OutError,
					TEXT("localization_content_graph_route_mismatch"),
					ProviderBinding,
					Capability);
			}
			if (!Target.IsSet())
			{
				return Fail(
					OutError,
					TEXT("content_graph_read_target_required"),
					ProviderBinding);
			}
			UObject* Owner = StaticLoadObject(
				UObject::StaticClass(),
				nullptr,
				*Target->GetPublicRef());
			if (!IsValid(Owner))
			{
				return Fail(
					OutError,
					TEXT("content_graph_read_target_unresolved"),
					Target->GetPublicRef());
			}
			TSharedPtr<FJsonObject> Parameters = MakeShared<FJsonObject>();
			const TSharedPtr<FJsonObject>* Supplied = nullptr;
			if (Input->TryGetObjectField(TEXT("parameters"), Supplied)
				&& Supplied && Supplied->IsValid())
			{
				Parameters = *Supplied;
			}
			TSharedPtr<FJsonObject> Observation;
			FString NativeError;
			if (!TryExecuteExternalLocalizationContentGraph(
					Capability,
					Parameters,
					Observation,
					NativeError)
				|| !Observation.IsValid())
			{
				return Fail(
					OutError,
					TEXT("localization_content_graph_mechanics_failed"),
					Capability,
					NativeError);
			}
			FString ObservationJson;
			const TSharedRef<TJsonWriter<TCHAR,
				TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR,
					TCondensedJsonPrintPolicy<TCHAR>>::Create(
						&ObservationJson);
			if (!FJsonSerializer::Serialize(Observation.ToSharedRef(), Writer))
			{
				return Fail(
					OutError,
					TEXT("localization_content_graph_observation_invalid"),
					Capability);
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
					return Fail(
						OutError,
						TEXT("content_graph_read_target_required"),
						ProviderBinding);
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
				if (!ValidateCapture(Capture, OutError))
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
					return Fail(
						OutError,
						TEXT(
							"content_graph_read_sections_missing"),
						ProviderBinding);
				}

				FSelection Selection;
				if (!BuildSelection(
						Capture,
						bCompound
							? ReadStringArray(Input, TEXT("root_ids"))
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
			return Fail(
				OutError,
				TEXT("content_graph_read_provider_unknown"),
				ProviderBinding);
		}
		OutHook = MakeShared<FNativeHook>(
			ProviderBinding,
			MoveTemp(Environment));
		return true;
	}

	bool TryBuildContentGraphCompoundSnapshot(
		const FUeAgentContentGraphCapture& Capture,
		const FUeAgentOperationContext& Context,
		TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
		FUeAgentContentGraphReadError& OutError)
	{
		OutSnapshot.Reset();
		OutError = {};
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
				OutError)
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
}
