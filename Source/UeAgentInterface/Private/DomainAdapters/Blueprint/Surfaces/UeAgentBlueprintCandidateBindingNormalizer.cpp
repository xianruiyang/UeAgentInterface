#include "DomainAdapters/Blueprint/Surfaces/UeAgentBlueprintCandidateBindingNormalizer.h"

#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
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

		bool HasExactLegacyComponentCandidate(
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

		bool TryValidateOptionalCandidateField(
			const TSharedPtr<FJsonObject>& Candidate,
			const TCHAR* Field,
			const FString& Expected,
			const FString& TargetIdentity,
			const FString& Pointer,
			FWorkspaceError& OutError)
		{
			FString Actual;
			if (!Candidate->HasField(Field))
			{
				return true;
			}
			if (!Candidate->TryGetStringField(Field, Actual)
				|| !Actual.Equals(Expected, ESearchCase::CaseSensitive))
			{
				return Fail(
					OutError,
					TEXT("blueprint_surface_component_candidate_metadata_mismatch"),
					TargetIdentity,
					Pointer + TEXT("/") + Field);
			}
			return true;
		}

		bool TryValidateOptionalNodeCandidateFields(
			const TSharedPtr<FJsonObject>& Candidate,
			const UeAgentPaletteSearch::FCandidate& ResolvedCandidate,
			const FString& TargetIdentity,
			const FString& Pointer,
			FWorkspaceError& OutError)
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field :
				Candidate->Values)
			{
				if (!UeAgentPaletteSearch::
						IsPublicBlueprintCandidateSemanticField(Field.Key))
				{
					continue;
				}
				FString Actual;
				const FString* Expected =
					ResolvedCandidate.AuthoringSelectors.Find(Field.Key);
				if (!Candidate->TryGetStringField(Field.Key, Actual)
					|| !Expected
					|| !Actual.Equals(*Expected, ESearchCase::CaseSensitive))
				{
					return Fail(
						OutError,
						TEXT("blueprint_surface_node_candidate_metadata_mismatch"),
						TargetIdentity,
						Pointer + TEXT("/") + Field.Key);
				}
			}
			return true;
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
				FString LegacyBinding;
				Candidate->TryGetStringField(
					TEXT("binding_revision"), LegacyBinding);
				if (!LegacyBinding.IsEmpty())
				{
					FString LegacyName;
					FString LegacyClass;
					if (!TryGetRequiredString(
							Candidate, TEXT("name"), LegacyName,
							TargetIdentity, Pointer + TEXT("/candidate"), OutError)
						|| !TryGetRequiredString(
							Candidate, TEXT("component_class"), LegacyClass,
							TargetIdentity, Pointer + TEXT("/candidate"), OutError))
					{
						return false;
					}
					Component::FUeAgentBlueprintComponentMechanicsError QueryError;
					FString PayloadJson;
					if (!Component::TryQueryBlueprintComponentCandidates(
							TargetIdentity, LegacyClass, LegacyName,
							PayloadJson, QueryError))
					{
						return Fail(
							OutError,
							QueryError.Code.IsEmpty()
								? FString(TEXT("blueprint_surface_component_candidate_query_failed"))
								: QueryError.Code,
							TargetIdentity,
							Pointer + TEXT("/candidate"));
					}
					TSharedPtr<FJsonObject> Payload;
					if (!TryParseQueryPayload(
							PayloadJson, Payload, TargetIdentity,
							Pointer + TEXT("/candidate"), OutError))
					{
						return false;
					}
					if (!HasExactLegacyComponentCandidate(
							Payload, LegacyName, LegacyClass, LegacyBinding))
					{
						return Fail(
							OutError,
							TEXT("blueprint_surface_component_candidate_binding_stale"),
							TargetIdentity,
							Pointer + TEXT("/candidate/binding_revision"));
					}
					if (!TrySetExactString(
							Spec, TEXT("name"), LegacyName,
							TargetIdentity, Pointer, OutError)
						|| !TrySetExactString(
							Spec, TEXT("class"), LegacyClass,
							TargetIdentity, Pointer, OutError))
					{
						return false;
					}
					continue;
				}

				DomainAuthoring::FDomainHjsonCandidateSelector Selector;
				if (!DomainAuthoring::TryReadCandidateSelector(
						Candidate,
						Pointer + TEXT("/candidate"),
						Selector,
						OutError))
				{
					return false;
				}
				UeAgentPaletteSearch::FCandidate ResolvedCandidate;
				FString ResolveError;
				if (!UeAgentPaletteSearch::TryResolveActorComponentCandidate(
						Selector.Name,
						Selector.Id,
						ResolvedCandidate,
						ResolveError))
				{
					FString ErrorCode = ResolveError;
					int32 Separator = INDEX_NONE;
					if (ErrorCode.FindChar(TEXT(':'), Separator))
					{
						ErrorCode.LeftInline(Separator);
					}
					return Fail(
						OutError,
						ErrorCode.IsEmpty()
							? FString(TEXT("blueprint_surface_component_candidate_query_failed"))
							: ErrorCode,
						TargetIdentity,
						Pointer + TEXT("/candidate"));
				}
				const FString* ResolvedClass =
					ResolvedCandidate.AuthoringSelectors.Find(TEXT("component_class"));
				if (!ResolvedClass || ResolvedClass->IsEmpty())
				{
					return Fail(
						OutError,
						TEXT("blueprint_surface_component_candidate_class_missing"),
						TargetIdentity,
						Pointer + TEXT("/candidate"));
				}
				if (!TryValidateOptionalCandidateField(
						Candidate, TEXT("class_path"), *ResolvedClass,
						TargetIdentity, Pointer + TEXT("/candidate"), OutError)
					|| !TryValidateOptionalCandidateField(
						Candidate, TEXT("component_class"), *ResolvedClass,
						TargetIdentity, Pointer + TEXT("/candidate"), OutError))
				{
					return false;
				}
				const FString ResolvedAsset =
					ResolvedCandidate.AuthoringSelectors.FindRef(TEXT("asset_override"));
				if ((Candidate->HasField(TEXT("asset_path"))
						&& !TryValidateOptionalCandidateField(
							Candidate, TEXT("asset_path"), ResolvedAsset,
							TargetIdentity, Pointer + TEXT("/candidate"), OutError))
					|| (Candidate->HasField(TEXT("asset_override"))
						&& !TryValidateOptionalCandidateField(
							Candidate, TEXT("asset_override"), ResolvedAsset,
							TargetIdentity, Pointer + TEXT("/candidate"), OutError)))
				{
					return false;
				}
				if (!Candidate->HasField(TEXT("name")))
				{
					Candidate->SetStringField(
						TEXT("name"), ResolvedCandidate.Name);
				}
				Candidate->SetStringField(
					TEXT("id"), ResolvedCandidate.GetPublicId());
				if (!TrySetExactString(
						Spec, TEXT("class"), *ResolvedClass,
						TargetIdentity, Pointer, OutError))
				{
					return false;
				}
			}
			return true;
		}

		bool TrySetResolvedGraphIdentity(
			const TSharedPtr<FJsonObject>& GraphSpec,
			const FString& GraphId,
			const FString& GraphName,
			const FString& TargetIdentity,
			const FString& Pointer,
			FWorkspaceError& OutError)
		{
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
				&& !ExistingName.Equals(GraphName, ESearchCase::CaseSensitive))
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
			Header->SetStringField(TEXT("name"), GraphName);
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
				FString GraphSelector;
				const TSharedPtr<FJsonObject>* GraphHeader = nullptr;
				if (GraphSpec->TryGetObjectField(TEXT("graph"), GraphHeader)
					&& GraphHeader && GraphHeader->IsValid())
				{
					FString Guid;
					if ((*GraphHeader)->TryGetStringField(TEXT("guid"), Guid)
						&& !Guid.TrimStartAndEnd().IsEmpty())
					{
						GraphSelector = TEXT("graph:") + Guid.TrimStartAndEnd();
					}
					if (GraphSelector.IsEmpty())
					{
						(*GraphHeader)->TryGetStringField(
							TEXT("name"), GraphSelector);
					}
				}
				if (GraphSelector.IsEmpty())
				{
					FGuid StableGuid;
					if (FGuid::Parse(GraphEntry.Key, StableGuid))
					{
						GraphSelector = TEXT("graph:")
							+ StableGuid.ToString(
								EGuidFormats::DigitsWithHyphensLower);
					}
					else
					{
						int32 Separator = INDEX_NONE;
						GraphSelector = GraphEntry.Key.FindChar(
							TEXT(':'), Separator)
							? GraphEntry.Key.Mid(Separator + 1)
							: GraphEntry.Key;
					}
				}
				GraphSelector.TrimStartAndEndInline();
				if (GraphSelector.IsEmpty())
				{
					return Fail(
						OutError,
						TEXT("blueprint_surface_node_candidate_graph_required"),
						TargetIdentity,
						GraphPointer);
				}
				FString ResolvedGraphId;
				FString ResolvedGraphName;
				FString GraphResolveError;
				if (!UeAgentPaletteSearch::TryResolveBlueprintGraphIdentity(
						TargetIdentity,
						GraphSelector,
						ResolvedGraphId,
						ResolvedGraphName,
						GraphResolveError))
				{
					return Fail(
						OutError,
						GraphResolveError.IsEmpty()
							? FString(TEXT(
								"blueprint_surface_node_candidate_graph_invalid"))
							: GraphResolveError,
						TargetIdentity,
						GraphPointer);
				}
				if (!TrySetResolvedGraphIdentity(
						GraphSpec,
						ResolvedGraphId,
						ResolvedGraphName,
						TargetIdentity,
						GraphPointer,
						OutError))
				{
					return false;
				}
				GraphSelector = ResolvedGraphId;
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
					DomainAuthoring::FDomainHjsonCandidateSelector Selector;
					if (!DomainAuthoring::TryReadCandidateSelector(
							Candidate,
							Pointer + TEXT("/candidate"),
							Selector,
							OutError))
					{
						return false;
					}

					UeAgentPaletteSearch::FSearchParams Params;
					Params.CatalogType = TEXT("blueprint_node");
					Params.AssetPath = TargetIdentity;
					Params.GraphName = GraphSelector;
					Params.bContextSensitive = true;
					const TSharedPtr<FJsonObject>* CandidateContext = nullptr;
					if (Spec->HasField(TEXT("candidate_context")))
					{
						if (!Spec->TryGetObjectField(
								TEXT("candidate_context"), CandidateContext)
							|| !CandidateContext
							|| !CandidateContext->IsValid())
						{
							return Fail(
								OutError,
								TEXT(
									"blueprint_surface_candidate_context_invalid"),
								TargetIdentity,
								Pointer + TEXT("/candidate_context"));
						}
						(*CandidateContext)->TryGetStringField(
							TEXT("node_guid"), Params.NodeGuid);
						(*CandidateContext)->TryGetStringField(
							TEXT("pin_guid"), Params.PinGuid);
						(*CandidateContext)->TryGetStringField(
							TEXT("pin_name"), Params.PinName);
					}
					UeAgentPaletteSearch::FCandidate ResolvedCandidate;
					FString ResolveError;
					if (!UeAgentPaletteSearch::TryResolveBlueprintActionCandidate(
							Params,
							Selector.Name,
							Selector.Id,
							ResolvedCandidate,
							ResolveError))
					{
						FString ErrorCode = ResolveError;
						int32 Separator = INDEX_NONE;
						if (ErrorCode.FindChar(TEXT(':'), Separator))
						{
							ErrorCode.LeftInline(Separator);
						}
						return Fail(
							OutError,
							ErrorCode.IsEmpty()
								? FString(TEXT(
									"blueprint_surface_node_candidate_query_failed"))
								: ErrorCode,
							TargetIdentity,
							Pointer + TEXT("/candidate"));
					}

					FString LegacyBinding;
					FString LegacyGraphId;
					FString LegacyCandidateId;
					Candidate->TryGetStringField(
						TEXT("binding_revision"), LegacyBinding);
					Candidate->TryGetStringField(
						TEXT("graph_id"), LegacyGraphId);
					Candidate->TryGetStringField(
						TEXT("candidate_id"), LegacyCandidateId);
					if (!LegacyBinding.IsEmpty())
					{
						if (LegacyGraphId.IsEmpty())
						{
							return Fail(
								OutError,
								TEXT("blueprint_surface_candidate_field_required"),
								TargetIdentity,
								Pointer + TEXT("/candidate/graph_id"));
						}
						if (!LegacyGraphId.Equals(
								ResolvedGraphId, ESearchCase::IgnoreCase))
						{
							return Fail(
								OutError,
								TEXT(
									"blueprint_surface_node_candidate_graph_mismatch"),
								TargetIdentity,
								Pointer + TEXT("/candidate/graph_id"));
						}
						Node::FUeAgentBlueprintNodeMechanicsError LegacyError;
						if (!Node::TryValidateLegacyBlueprintNodeCandidateBinding(
								TargetIdentity,
								LegacyGraphId,
								Selector.Name,
								LegacyCandidateId,
								LegacyBinding,
								LegacyError))
						{
							return Fail(
								OutError,
								LegacyError.Code.IsEmpty()
									? FString(TEXT(
										"blueprint_surface_node_candidate_binding_stale"))
									: LegacyError.Code,
								TargetIdentity,
								Pointer + TEXT("/candidate/binding_revision"));
						}
					}

					if (!TryValidateOptionalNodeCandidateFields(
							Candidate,
							ResolvedCandidate,
							TargetIdentity,
							Pointer + TEXT("/candidate"),
							OutError))
					{
						return false;
					}

					if (!Candidate->HasField(TEXT("name")))
					{
						Candidate->SetStringField(
							TEXT("name"), ResolvedCandidate.Name);
					}
					Candidate->SetStringField(
						TEXT("id"), ResolvedCandidate.GetPublicId());
					if (!TrySetExactString(
							Spec,
							TEXT("node_type"),
							TEXT("native_action"),
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
