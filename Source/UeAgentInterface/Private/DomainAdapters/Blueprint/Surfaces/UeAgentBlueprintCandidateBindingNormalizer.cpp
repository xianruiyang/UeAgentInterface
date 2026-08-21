#include "DomainAdapters/Blueprint/Surfaces/UeAgentBlueprintCandidateBindingNormalizer.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "DomainAdapters/Blueprint/Component/UeAgentBlueprintComponentMechanics.h"
#include "DomainAdapters/Blueprint/Node/UeAgentBlueprintNodeMechanics.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::DomainAdapters::Blueprint::Surfaces
{
	using namespace HjsonWorkspace;

	namespace
	{
		bool Fail(
			FWorkspaceError& OutError,
			FString Code,
			const FString& TargetIdentity,
			FString Pointer)
		{
			OutError.Set(MoveTemp(Code), TargetIdentity, MoveTemp(Pointer));
			return false;
		}

		bool TryGetRequiredString(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			FString& OutValue,
			const FString& TargetIdentity,
			const FString& Pointer,
			FWorkspaceError& OutError)
		{
			if (!Object.IsValid()
				|| !Object->TryGetStringField(Field, OutValue)
				|| OutValue.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("blueprint_surface_candidate_field_required"),
					TargetIdentity,
					Pointer + TEXT("/") + Field);
			}
			return true;
		}

		bool TrySetExactString(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			const FString& Expected,
			const FString& TargetIdentity,
			const FString& Pointer,
			FWorkspaceError& OutError)
		{
			FString Existing;
			if (Object->HasField(Field)
				&& (!Object->TryGetStringField(Field, Existing)
					|| Existing != Expected))
			{
				return Fail(
					OutError,
					TEXT("blueprint_surface_candidate_normalization_conflict"),
					TargetIdentity,
					Pointer + TEXT("/") + Field);
			}
			Object->SetStringField(Field, Expected);
			return true;
		}

		bool TryParseQueryPayload(
			const FString& PayloadJson,
			TSharedPtr<FJsonObject>& OutPayload,
			const FString& TargetIdentity,
			const FString& Pointer,
			FWorkspaceError& OutError)
		{
			FString JsonError;
			if (!CommandContract::Json::TryParseObject(
					PayloadJson, OutPayload, JsonError)
				|| !OutPayload.IsValid())
			{
				return Fail(
					OutError,
					TEXT("blueprint_surface_candidate_query_payload_invalid"),
					TargetIdentity,
					Pointer);
			}
			return true;
		}

		bool HasExactNodeCandidate(
			const TSharedPtr<FJsonObject>& Payload,
			const FString& Name,
			const FString& BindingRevision,
			const FString& GraphId)
		{
			const TArray<TSharedPtr<FJsonValue>>* Candidates = nullptr;
			if (!Payload->TryGetArrayField(TEXT("candidates"), Candidates)
				|| !Candidates)
			{
				return false;
			}
			return Candidates->ContainsByPredicate(
				[&Name, &BindingRevision, &GraphId](
					const TSharedPtr<FJsonValue>& Value)
				{
					const TSharedPtr<FJsonObject> Candidate = Value.IsValid()
						&& Value->Type == EJson::Object
						? Value->AsObject()
						: nullptr;
					FString ActualName;
					FString ActualBinding;
					FString ActualGraph;
					return Candidate.IsValid()
						&& Candidate->TryGetStringField(TEXT("name"), ActualName)
						&& Candidate->TryGetStringField(
							TEXT("binding_revision"), ActualBinding)
						&& Candidate->TryGetStringField(
							TEXT("graph_id"), ActualGraph)
						&& ActualName == Name
						&& ActualBinding == BindingRevision
						&& ActualGraph == GraphId;
				});
		}

		bool HasExactComponentCandidate(
			const TSharedPtr<FJsonObject>& Payload,
			const FString& Name,
			const FString& ComponentClass,
			const FString& BindingRevision)
		{
			const TArray<TSharedPtr<FJsonValue>>* Candidates = nullptr;
			if (!Payload->TryGetArrayField(TEXT("candidates"), Candidates)
				|| !Candidates)
			{
				return false;
			}
			return Candidates->ContainsByPredicate(
				[&Name, &ComponentClass, &BindingRevision](
					const TSharedPtr<FJsonValue>& Value)
				{
					const TSharedPtr<FJsonObject> Candidate = Value.IsValid()
						&& Value->Type == EJson::Object
						? Value->AsObject()
						: nullptr;
					FString ActualName;
					FString ActualClass;
					FString ActualBinding;
					return Candidate.IsValid()
						&& Candidate->TryGetStringField(TEXT("name"), ActualName)
						&& Candidate->TryGetStringField(
							TEXT("component_class"), ActualClass)
						&& Candidate->TryGetStringField(
							TEXT("binding_revision"), ActualBinding)
						&& ActualName == Name
						&& ActualClass == ComponentClass
						&& ActualBinding == BindingRevision;
				});
		}

		bool TryNormalizeComponentCandidates(
			const FString& TargetIdentity,
			const TSharedPtr<FJsonObject>& ProviderExtension,
			FWorkspaceError& OutError)
		{
			const TSharedPtr<FJsonObject>* Components = nullptr;
			if (!ProviderExtension->HasField(TEXT("components")))
			{
				return true;
			}
			if (!ProviderExtension->TryGetObjectField(
					TEXT("components"), Components)
				|| !Components || !Components->IsValid())
			{
				return Fail(
					OutError,
					TEXT("blueprint_surface_components_invalid"),
					TargetIdentity,
					TEXT("/root/document/provider_extension/components"));
			}

			for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry
				: (*Components)->Values)
			{
				const TSharedPtr<FJsonObject> Spec = Entry.Value.IsValid()
					? Entry.Value->AsObject()
					: nullptr;
				const FString Pointer =
					TEXT("/root/document/provider_extension/components/")
					+ Entry.Key;
				if (!Spec.IsValid())
				{
					return Fail(
						OutError,
						TEXT("blueprint_surface_component_spec_invalid"),
						TargetIdentity,
						Pointer);
				}
				const TSharedPtr<FJsonObject>* CandidateField = nullptr;
				if (!Spec->HasField(TEXT("candidate")))
				{
					continue;
				}
				if (!Spec->TryGetObjectField(
						TEXT("candidate"), CandidateField)
					|| !CandidateField || !CandidateField->IsValid())
				{
					return Fail(
						OutError,
						TEXT("blueprint_surface_component_candidate_invalid"),
						TargetIdentity,
						Pointer + TEXT("/candidate"));
				}
				const TSharedPtr<FJsonObject> Candidate = CandidateField
					? *CandidateField
					: nullptr;
				FString Name;
				FString ComponentClass;
				FString BindingRevision;
				if (!TryGetRequiredString(
						Candidate,
						TEXT("name"),
						Name,
						TargetIdentity,
						Pointer + TEXT("/candidate"),
						OutError)
					|| !TryGetRequiredString(
						Candidate,
						TEXT("component_class"),
						ComponentClass,
						TargetIdentity,
						Pointer + TEXT("/candidate"),
						OutError)
					|| !TryGetRequiredString(
						Candidate,
						TEXT("binding_revision"),
						BindingRevision,
						TargetIdentity,
						Pointer + TEXT("/candidate"),
						OutError))
				{
					return false;
				}

				Component::FUeAgentBlueprintComponentMechanicsError QueryError;
				FString PayloadJson;
				if (!Component::TryQueryBlueprintComponentCandidates(
						TargetIdentity,
						ComponentClass,
						Name,
						PayloadJson,
						QueryError))
				{
					return Fail(
						OutError,
						QueryError.Code.IsEmpty()
							? FString(TEXT(
								"blueprint_surface_component_candidate_query_failed"))
							: QueryError.Code,
						TargetIdentity,
						Pointer + TEXT("/candidate"));
				}
				TSharedPtr<FJsonObject> Payload;
				if (!TryParseQueryPayload(
						PayloadJson,
						Payload,
						TargetIdentity,
						Pointer + TEXT("/candidate"),
						OutError))
				{
					return false;
				}
				if (!HasExactComponentCandidate(
						Payload, Name, ComponentClass, BindingRevision))
				{
					return Fail(
						OutError,
						TEXT("blueprint_surface_component_candidate_binding_stale"),
						TargetIdentity,
						Pointer + TEXT("/candidate/binding_revision"));
				}
				if (!TrySetExactString(
						Spec,
						TEXT("name"),
						Name,
						TargetIdentity,
						Pointer,
						OutError)
					|| !TrySetExactString(
						Spec,
						TEXT("class"),
						ComponentClass,
						TargetIdentity,
						Pointer,
						OutError))
				{
					return false;
				}
			}
			return true;
		}

		bool TryNormalizeGraphIdentity(
			const TSharedPtr<FJsonObject>& GraphSpec,
			const TSharedPtr<FJsonObject>& QueryPayload,
			const FString& GraphId,
			const FString& TargetIdentity,
			const FString& Pointer,
			FWorkspaceError& OutError)
		{
			const TSharedPtr<FJsonObject>* QueryGraphField = nullptr;
			if (!QueryPayload->TryGetObjectField(
					TEXT("graph"), QueryGraphField)
				|| !QueryGraphField || !QueryGraphField->IsValid())
			{
				return Fail(
					OutError,
					TEXT("blueprint_surface_node_candidate_graph_invalid"),
					TargetIdentity,
					Pointer + TEXT("/candidate/graph_id"));
			}
			FString QueryGraphId;
			FString QueryGraphName;
			if (!(*QueryGraphField)->TryGetStringField(
					TEXT("id"), QueryGraphId)
				|| QueryGraphId != GraphId
				|| !(*QueryGraphField)->TryGetStringField(
					TEXT("name"), QueryGraphName))
			{
				return Fail(
					OutError,
					TEXT("blueprint_surface_node_candidate_graph_mismatch"),
					TargetIdentity,
					Pointer + TEXT("/candidate/graph_id"));
			}

			TSharedPtr<FJsonObject> Header;
			const TSharedPtr<FJsonObject>* HeaderField = nullptr;
			if (GraphSpec->TryGetObjectField(TEXT("graph"), HeaderField))
			{
				if (!HeaderField || !HeaderField->IsValid())
				{
					return Fail(
						OutError,
						TEXT("blueprint_surface_graph_identity_invalid"),
						TargetIdentity,
						Pointer + TEXT("/graph"));
				}
				Header = *HeaderField;
			}
			else
			{
				Header = MakeShared<FJsonObject>();
				GraphSpec->SetObjectField(TEXT("graph"), Header);
			}
			FString ExistingName;
			if (Header->TryGetStringField(TEXT("name"), ExistingName)
				&& !ExistingName.Equals(QueryGraphName, ESearchCase::CaseSensitive))
			{
				return Fail(
					OutError,
					TEXT("blueprint_surface_node_candidate_graph_mismatch"),
					TargetIdentity,
					Pointer + TEXT("/graph/name"));
			}
			if (!GraphId.StartsWith(TEXT("graph:")))
			{
				return Fail(
					OutError,
					TEXT("blueprint_surface_node_candidate_graph_identity_unsupported"),
					TargetIdentity,
					Pointer + TEXT("/candidate/graph_id"));
			}
			const FString CandidateGuid = GraphId.Mid(6);
			FGuid ParsedGuid;
			if (!FGuid::Parse(CandidateGuid, ParsedGuid))
			{
				return Fail(
					OutError,
					TEXT("blueprint_surface_node_candidate_graph_identity_unsupported"),
					TargetIdentity,
					Pointer + TEXT("/candidate/graph_id"));
			}
			FString ExistingGuid;
			if (Header->TryGetStringField(TEXT("guid"), ExistingGuid))
			{
				FGuid ParsedExisting;
				if (!FGuid::Parse(ExistingGuid, ParsedExisting)
					|| ParsedExisting != ParsedGuid)
				{
					return Fail(
						OutError,
						TEXT("blueprint_surface_node_candidate_graph_mismatch"),
						TargetIdentity,
						Pointer + TEXT("/graph/guid"));
				}
			}
			Header->SetStringField(
				TEXT("guid"),
				ParsedGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
			Header->SetStringField(TEXT("name"), QueryGraphName);
			return true;
		}

		bool TryNormalizeNodeCandidates(
			const FString& TargetIdentity,
			const TSharedPtr<FJsonObject>& ProviderExtension,
			FWorkspaceError& OutError)
		{
			const TSharedPtr<FJsonObject>* Graphs = nullptr;
			if (!ProviderExtension->HasField(TEXT("graphs")))
			{
				return true;
			}
			if (!ProviderExtension->TryGetObjectField(TEXT("graphs"), Graphs)
				|| !Graphs || !Graphs->IsValid())
			{
				return Fail(
					OutError,
					TEXT("blueprint_surface_graphs_invalid"),
					TargetIdentity,
					TEXT("/root/document/provider_extension/graphs"));
			}

			for (const TPair<FString, TSharedPtr<FJsonValue>>& GraphEntry
				: (*Graphs)->Values)
			{
				const TSharedPtr<FJsonObject> GraphSpec = GraphEntry.Value.IsValid()
					? GraphEntry.Value->AsObject()
					: nullptr;
				const FString GraphPointer =
					TEXT("/root/document/provider_extension/graphs/")
					+ GraphEntry.Key;
				if (!GraphSpec.IsValid())
				{
					return Fail(
						OutError,
						TEXT("blueprint_surface_graph_spec_invalid"),
						TargetIdentity,
						GraphPointer);
				}
				const TSharedPtr<FJsonObject>* Nodes = nullptr;
				if (!GraphSpec->HasField(TEXT("nodes")))
				{
					continue;
				}
				if (!GraphSpec->TryGetObjectField(TEXT("nodes"), Nodes)
					|| !Nodes || !Nodes->IsValid())
				{
					return Fail(
						OutError,
						TEXT("blueprint_surface_nodes_invalid"),
						TargetIdentity,
						GraphPointer + TEXT("/nodes"));
				}
				for (const TPair<FString, TSharedPtr<FJsonValue>>& NodeEntry
					: (*Nodes)->Values)
				{
					const TSharedPtr<FJsonObject> Spec = NodeEntry.Value.IsValid()
						? NodeEntry.Value->AsObject()
						: nullptr;
					const FString Pointer = GraphPointer + TEXT("/nodes/")
						+ NodeEntry.Key;
					if (!Spec.IsValid())
					{
						return Fail(
							OutError,
							TEXT("blueprint_surface_node_spec_invalid"),
							TargetIdentity,
							Pointer);
					}
					const TSharedPtr<FJsonObject>* CandidateField = nullptr;
					if (!Spec->HasField(TEXT("candidate")))
					{
						continue;
					}
					if (!Spec->TryGetObjectField(
							TEXT("candidate"), CandidateField)
						|| !CandidateField || !CandidateField->IsValid())
					{
						return Fail(
							OutError,
							TEXT("blueprint_surface_node_candidate_invalid"),
							TargetIdentity,
							Pointer + TEXT("/candidate"));
					}
					const TSharedPtr<FJsonObject> Candidate = CandidateField
						? *CandidateField
						: nullptr;
					FString Name;
					FString BindingRevision;
					FString GraphId;
					if (!TryGetRequiredString(
							Candidate,
							TEXT("name"),
							Name,
							TargetIdentity,
							Pointer + TEXT("/candidate"),
							OutError)
						|| !TryGetRequiredString(
							Candidate,
							TEXT("binding_revision"),
							BindingRevision,
							TargetIdentity,
							Pointer + TEXT("/candidate"),
							OutError)
						|| !TryGetRequiredString(
							Candidate,
							TEXT("graph_id"),
							GraphId,
							TargetIdentity,
							Pointer + TEXT("/candidate"),
							OutError))
					{
						return false;
					}

					Node::FUeAgentBlueprintNodeMechanicsError QueryError;
					FString PayloadJson;
					if (!Node::TryQueryBlueprintNodeCandidates(
							TargetIdentity,
							GraphId,
							Name,
							PayloadJson,
							QueryError))
					{
						return Fail(
							OutError,
							QueryError.Code.IsEmpty()
								? FString(TEXT(
									"blueprint_surface_node_candidate_query_failed"))
								: QueryError.Code,
							TargetIdentity,
							Pointer + TEXT("/candidate"));
					}
					TSharedPtr<FJsonObject> Payload;
					if (!TryParseQueryPayload(
							PayloadJson,
							Payload,
							TargetIdentity,
							Pointer + TEXT("/candidate"),
							OutError))
					{
						return false;
					}
					if (!HasExactNodeCandidate(
							Payload, Name, BindingRevision, GraphId))
					{
						return Fail(
							OutError,
							TEXT("blueprint_surface_node_candidate_binding_stale"),
							TargetIdentity,
							Pointer + TEXT("/candidate/binding_revision"));
					}
					if (!TryNormalizeGraphIdentity(
							GraphSpec,
							Payload,
							GraphId,
							TargetIdentity,
							GraphPointer,
							OutError)
						|| !TrySetExactString(
							Spec,
							TEXT("node_type"),
							TEXT("node_by_class"),
							TargetIdentity,
							Pointer,
							OutError)
						|| !TrySetExactString(
							Spec,
							TEXT("node_class"),
							Name,
							TargetIdentity,
							Pointer,
							OutError))
					{
						return false;
					}
				}
			}
			return true;
		}
	}

	bool TryNormalizeBlueprintCandidateBindings(
		const FString& TargetIdentity,
		const TSharedPtr<FJsonObject>& ProviderExtension,
		FWorkspaceError& OutError)
	{
		if (!ProviderExtension.IsValid())
		{
			return Fail(
				OutError,
				TEXT("blueprint_surface_provider_extension_invalid"),
				TargetIdentity,
				TEXT("/root/document/provider_extension"));
		}
		return TryNormalizeComponentCandidates(
				TargetIdentity, ProviderExtension, OutError)
			&& TryNormalizeNodeCandidates(
				TargetIdentity, ProviderExtension, OutError);
	}
}
