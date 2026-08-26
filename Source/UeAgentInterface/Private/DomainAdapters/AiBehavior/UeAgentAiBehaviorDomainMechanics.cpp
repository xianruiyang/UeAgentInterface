#include "DomainAdapters/AiBehavior/UeAgentAiBehaviorDomainMechanics.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "OperationOwners/AiCommands/UeAgentAiCommandOperationOwner.h"
#include "QueryOwners/AiBehavior/UeAgentAiBehaviorCandidateQuery.h"
#include "SmartObjectDefinition.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::DomainAdapters::AiBehavior
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace HjsonWorkspace;
	using namespace HjsonWorkspace::DomainAuthoring;

	namespace
	{
		const FString SchemaId = TEXT("uai.ai_behavior.edit/1");
		const FString BehaviorTreeType = TEXT("/Script/AIModule.BehaviorTree");
		const FString BlackboardType = TEXT("/Script/AIModule.BlackboardData");
		const FString EqsType = TEXT("/Script/AIModule.EnvQuery");
		const FString StateTreeType = TEXT("/Script/StateTreeModule.StateTree");
		const FString SmartObjectType =
			TEXT("/Script/SmartObjectsModule.SmartObjectDefinition");
		const FString EqsCarrierType =
			TEXT("/Script/EnvironmentQueryEditor.EnvironmentQueryGraphNode_Option");

		struct FRootSpec final
		{
			FString Type;
			FString CreateCommand;
			FString MutationCommand;
		};

		const TArray<FRootSpec>& RootSpecs()
		{
			static const TArray<FRootSpec> Values = {
				{BehaviorTreeType, TEXT("ai.behavior_tree.asset.create"),
					TEXT("ai.behavior_tree.graph.apply")},
				{BlackboardType, TEXT("ai.blackboard.asset.create"),
					TEXT("ai.blackboard.schema.apply")},
				{EqsType, TEXT("ai.eqs.asset.create"),
					TEXT("ai.eqs.graph.apply")},
				{StateTreeType, TEXT("ai.state_tree.asset.create"),
					TEXT("ai.state_tree.hierarchy.apply")},
				{SmartObjectType,
					TEXT("ai.smart_object_definition.asset.create"),
					TEXT("ai.smart_object_definition.apply")},
			};
			return Values;
		}

		const FRootSpec* FindRootSpec(const FString& RootType)
		{
			return RootSpecs().FindByPredicate(
				[&RootType](const FRootSpec& Spec)
				{
					return Spec.Type == RootType;
				});
		}

		class FNeverCancelled final : public Contract::IUeAgentCancellationSignal
		{
		public:
			virtual bool IsCancelled() const override { return false; }
		};

		bool Fail(
			FWorkspaceError& OutError,
			FString Code,
			FString Source = FString(),
			FString Pointer = TEXT("/"))
		{
			OutError.Set(MoveTemp(Code), MoveTemp(Source), MoveTemp(Pointer));
			return false;
		}

		FString PackageIdentity(const FString& Target)
		{
			return Target.Contains(TEXT("."))
				? FPackageName::ObjectPathToPackageName(Target)
				: Target;
		}

		bool DecodeRequest(
			const FString& CommandId,
			const FString& ContractHash,
			const FString& ParamsJson,
			TOptional<FUeAgentCommandRequest>& OutRequest,
			FWorkspaceError& OutError)
		{
			const FString RequestId = TEXT("ai-behavior-domain-")
				+ FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
			const FString Envelope = FString::Printf(
				TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
				TEXT("\"message_kind\":\"command_request\",")
				TEXT("\"correlation_id\":\"%s\",\"request_id\":\"%s\",")
				TEXT("\"command_id\":\"%s\",\"command_binding_hash\":\"%s\",")
				TEXT("\"params\":%s}"),
				*RequestId, *RequestId, *CommandId, *ContractHash, *ParamsJson);
			FTCHARToUTF8 Encoded(*Envelope);
			TArray<uint8> Bytes;
			Bytes.Append(
				reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
			FUeAgentCommandProtocolDiagnostic Diagnostic;
			if (!FUeAgentCommandProtocolCodec().DecodeRequest(
					Bytes, OutRequest, Diagnostic)
				|| !OutRequest.IsSet())
			{
				return Fail(
					OutError,
					TEXT("ai_behavior_domain_internal_request_invalid"),
					Diagnostic.Code,
					Diagnostic.Message);
			}
			return true;
		}

		bool SubmitAiCommand(
			const FString& CommandId,
			const TSharedPtr<FJsonObject>& Params,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry>
				Registry;
			TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
			FString CompositionError;
			if (!Module::CommandRegistration::Active::
					TryComposeProcessActiveCommandComposition(
						Registry, Dispatch, CompositionError)
				|| !Registry.IsValid())
			{
				return Fail(
					OutError, TEXT("ai_behavior_domain_registry_unavailable"),
					CompositionError);
			}
			const FUeAgentFrozenCommandContract* Contract =
				Registry->FindExact(CommandId);
			if (!Contract)
			{
				return Fail(
					OutError, TEXT("ai_behavior_domain_command_missing"),
					CommandId);
			}
			FString ParamsJson;
			if (!TryCanonicalizeObject(Params, ParamsJson, OutError)) return false;
			TOptional<FUeAgentCommandRequest> Request;
			if (!DecodeRequest(
					CommandId, Contract->GetContractHash(), ParamsJson,
					Request, OutError))
			{
				return false;
			}
			TMap<FString, TSharedRef<IUeAgentCommandHandler>> Mechanics;
			FString MechanicsError;
			if (!OperationOwners::AiCommands::TryCreateMechanicsHandlers(
					Mechanics, MechanicsError))
			{
				return Fail(
					OutError, TEXT("ai_behavior_domain_mechanics_unavailable"),
					CommandId, MechanicsError);
			}
			const TSharedRef<IUeAgentCommandHandler>* Handler =
				Mechanics.Find(CommandId);
			if (!Handler)
			{
				return Fail(
					OutError, TEXT("ai_behavior_domain_mechanics_unavailable"),
					CommandId);
			}
			const TArray<FString> AuthorityScopes = {
				TEXT("uai:query"),
				TEXT("uai:operate"),
				TEXT("uai:authoring:hjson:import"),
				TEXT("pcg_ai_node_graph.write"),
			};
			const FUeAgentCommandResult Result =
				OperationOwners::AiCommands::Submit(
					*Handler,
					Request.GetValue(),
					Contract::FUeAgentCallerContext(
						TEXT("ai-behavior-domain-provider"),
						AuthorityScopes,
						Request->GetCorrelationId()),
					MakeShared<FNeverCancelled>());
			if (!Result.IsOk())
			{
				SetWorkspaceErrorFromCommandFailure(
					Result.GetErrorJson(),
					TEXT("ai_behavior_domain_native_command_failed"),
					CommandId,
					OutError);
				return false;
			}
			OutPayload = Result.GetPayloadJson();
			return true;
		}

		bool TryRead(
			const FString& Target,
			const FString& RootType,
			FString& OutPayload,
			FString& OutRevision,
			TSharedPtr<FJsonObject>* OutSnapshot,
			UObject** OutObject,
			FWorkspaceError& OutError)
		{
			UObject* Owner = nullptr;
			DomainAdapters::ContentGraphRead::FUeAgentContentGraphCapture Capture;
			if (!TryCaptureObject(
					Target, RootType, Owner, Capture, OutError))
			{
				return false;
			}
			TSharedPtr<FJsonObject> Snapshot;
			if (!TryBuildCaptureSnapshot(
					Target, RootType, Capture, Snapshot, OutError))
			{
				return false;
			}
			OutRevision = Capture.Revision;
			if (OutSnapshot) *OutSnapshot = Snapshot;
			if (OutObject) *OutObject = Owner;
			return TryCanonicalizeObject(Snapshot, OutPayload, OutError);
		}

		bool ValidateMissingTarget(
			const FString& Target,
			FWorkspaceError& OutError)
		{
			bool bPresent = false;
			if (!TryInspectObjectTargetPresence(Target, bPresent))
			{
				return Fail(
					OutError, TEXT("ai_behavior_domain_target_invalid"), Target);
			}
			if (bPresent)
			{
				return Fail(
					OutError, TEXT("ai_behavior_domain_stale_revision"),
					Target, TEXT("existing"));
			}
			return true;
		}

		bool ReadCurrentCandidate(
			UObject& TargetObject,
			const TSharedPtr<FJsonObject>& Candidate,
			const FString& Pointer,
			FDomainHjsonCandidateBinding& OutBinding,
			FWorkspaceError& OutError)
		{
			if (!TryReadCandidateBinding(
					Candidate, Pointer, OutBinding, OutError))
			{
				return false;
			}
			if (!OutBinding.SupportsOperation(TEXT("create")))
			{
				return Fail(
					OutError, TEXT("ai_behavior_candidate_operation_unsupported"),
					OutBinding.Name,
					Pointer + TEXT("/supported_operations"));
			}
			QueryOwners::AiBehavior::FAiBehaviorCandidateQueryError CandidateError;
			if (!QueryOwners::AiBehavior::IsCurrentAiBehaviorCandidateBinding(
					TargetObject,
					OutBinding.Name,
					OutBinding.Kind,
					OutBinding.SupportedOperations,
					OutBinding.InputContract,
					OutBinding.PlacementContract,
					OutBinding.RequiredContext,
					OutBinding.BindingRevision,
					CandidateError))
			{
				return Fail(
					OutError,
					CandidateError.Code.IsEmpty()
						? FString(TEXT("ai_behavior_candidate_binding_invalid"))
						: CandidateError.Code,
					CandidateError.SubjectId,
					CandidateError.Detail);
			}
			return true;
		}

		bool CandidateObject(
			const TSharedPtr<FJsonObject>& Value,
			const FString& Pointer,
			TSharedPtr<FJsonObject>& OutCandidate,
			bool& bOutPresent,
			FWorkspaceError& OutError)
		{
			OutCandidate.Reset();
			bOutPresent = Value.IsValid() && Value->HasField(TEXT("candidate"));
			if (!bOutPresent) return true;
			const TSharedPtr<FJsonObject>* Candidate = nullptr;
			if (!Value->TryGetObjectField(TEXT("candidate"), Candidate)
				|| !Candidate || !Candidate->IsValid())
			{
				return Fail(
					OutError, TEXT("domain_hjson_candidate_binding_invalid"),
					FString(), Pointer);
			}
			OutCandidate = *Candidate;
			return true;
		}

		bool HasOnlyCandidateSelectorFields(
			const TSharedPtr<FJsonObject>& Candidate)
		{
			if (!Candidate.IsValid())
			{
				return false;
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field
				: Candidate->Values)
			{
				if (Field.Key != TEXT("name") && Field.Key != TEXT("id"))
				{
					return false;
				}
			}
			return true;
		}

		bool RejectConflict(
			const TSharedPtr<FJsonObject>& Value,
			const FString& Field,
			const FString& Expected,
			const FString& Pointer,
			FWorkspaceError& OutError)
		{
			FString Existing;
			if (Value->TryGetStringField(Field, Existing)
				&& Existing != Expected)
			{
				return Fail(
					OutError, TEXT("ai_behavior_candidate_field_conflict"),
					Existing, Pointer + TEXT("/") + Field);
			}
			return true;
		}

		FString BehaviorCarrier(const FString& Kind)
		{
			if (Kind == TEXT("behavior_composite"))
			{
				return TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Composite");
			}
			if (Kind == TEXT("behavior_task"))
			{
				return TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Task");
			}
			if (Kind == TEXT("behavior_service"))
			{
				return TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Service");
			}
			if (Kind == TEXT("behavior_decorator"))
			{
				return TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Decorator");
			}
			return FString();
		}

		bool NormalizeGraphCandidates(
			const FString& RootType,
			UObject& TargetObject,
			const TArray<TSharedPtr<FJsonValue>>& Edits,
			FWorkspaceError& OutError)
		{
			for (int32 Index = 0; Index < Edits.Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Edit =
					Edits[Index].IsValid() ? Edits[Index]->AsObject() : nullptr;
				FString Intent;
				FString ElementKind;
				const TSharedPtr<FJsonObject>* ValuePtr = nullptr;
				if (!Edit.IsValid()
					|| !Edit->TryGetStringField(TEXT("intent"), Intent)
					|| Intent != TEXT("create")
					|| !Edit->TryGetObjectField(TEXT("value"), ValuePtr)
					|| !ValuePtr || !ValuePtr->IsValid())
				{
					continue;
				}
				const TSharedPtr<FJsonObject> Value = *ValuePtr;
				const FString Pointer = FString::Printf(
					TEXT("/root/operation/params/edits/%d/value/candidate"),
					Index);
				TSharedPtr<FJsonObject> Candidate;
				bool bPresent = false;
				if (!CandidateObject(
						Value, Pointer, Candidate, bPresent, OutError))
				{
					return false;
				}
				if (!bPresent) continue;
				if (!Edit->TryGetStringField(TEXT("element_kind"), ElementKind)
					|| ElementKind != TEXT("item"))
				{
					return Fail(
						OutError, TEXT("ai_behavior_candidate_placement_invalid"),
						ElementKind, Pointer);
				}
				if (RootType == BehaviorTreeType
					&& HasOnlyCandidateSelectorFields(Candidate))
				{
					FDomainHjsonCandidateSelector Selector;
					if (!TryReadCandidateSelector(
							Candidate, Pointer, Selector, OutError))
					{
						return false;
					}
					QueryOwners::AiBehavior::FAiBehaviorCandidate Resolved;
					QueryOwners::AiBehavior::FAiBehaviorCandidateQueryError
						CandidateError;
					if (!QueryOwners::AiBehavior::TryResolveAiBehaviorCandidate(
							TargetObject,
							Selector.Name,
							Selector.Id,
							Resolved,
							CandidateError))
					{
						return Fail(
							OutError,
							CandidateError.Code.IsEmpty()
								? FString(TEXT(
									"ai_behavior_candidate_selector_invalid"))
								: CandidateError.Code,
							CandidateError.SubjectId,
							Pointer);
					}
					if (!Resolved.bSupported
						|| !Resolved.SupportedOperations.Contains(TEXT("create")))
					{
						return Fail(
							OutError,
							TEXT("ai_behavior_candidate_action_kind_unsupported"),
							Resolved.ActionKind,
							Pointer);
					}
					const FString Carrier = BehaviorCarrier(Resolved.Kind);
					if (Carrier.IsEmpty() || Resolved.PayloadType.IsEmpty())
					{
						return Fail(
							OutError,
							TEXT("ai_behavior_candidate_kind_invalid"),
							Resolved.Kind,
							Pointer);
					}
					if (!RejectConflict(
							Value,
							TEXT("carrier_type_ref"),
							Carrier,
							Pointer,
							OutError)
						|| !RejectConflict(
							Value,
							TEXT("payload_type_ref"),
							Resolved.PayloadType,
							Pointer,
							OutError))
					{
						return false;
					}
					Value->SetStringField(TEXT("carrier_type_ref"), Carrier);
					Value->SetStringField(
						TEXT("payload_type_ref"), Resolved.PayloadType);
					Candidate->SetStringField(TEXT("name"), Resolved.Name);
					Candidate->SetStringField(TEXT("id"), Resolved.Id);
					continue;
				}

				FDomainHjsonCandidateBinding Binding;
				if (!ReadCurrentCandidate(
						TargetObject, Candidate, Pointer, Binding, OutError))
				{
					return false;
				}

				if (RootType == BehaviorTreeType)
				{
					const FString Carrier = BehaviorCarrier(Binding.Kind);
					if (Carrier.IsEmpty())
					{
						return Fail(
							OutError, TEXT("ai_behavior_candidate_kind_invalid"),
							Binding.Kind, Pointer);
					}
					if (!RejectConflict(
							Value, TEXT("carrier_type_ref"), Carrier,
							Pointer, OutError)
						|| !RejectConflict(
							Value, TEXT("payload_type_ref"), Binding.Name,
							Pointer, OutError))
					{
						return false;
					}
					Value->SetStringField(TEXT("carrier_type_ref"), Carrier);
					Value->SetStringField(TEXT("payload_type_ref"), Binding.Name);
				}
				else if (RootType == EqsType)
				{
					if (Binding.Kind != TEXT("eqs_generator"))
					{
						return Fail(
							OutError, TEXT("ai_behavior_candidate_kind_invalid"),
							Binding.Kind, Pointer);
					}
					if (!RejectConflict(
							Value, TEXT("carrier_type_ref"), EqsCarrierType,
							Pointer, OutError)
						|| !RejectConflict(
							Value, TEXT("payload_type_ref"), Binding.Name,
							Pointer, OutError))
					{
						return false;
					}
					Value->SetStringField(TEXT("carrier_type_ref"), EqsCarrierType);
					Value->SetStringField(TEXT("payload_type_ref"), Binding.Name);
				}
				else if (RootType == StateTreeType)
				{
					if (!Binding.Kind.StartsWith(TEXT("state_tree_")))
					{
						return Fail(
							OutError, TEXT("ai_behavior_candidate_kind_invalid"),
							Binding.Kind, Pointer);
					}
					if (!RejectConflict(
							Value, TEXT("node_class_ref"), Binding.Name,
							Pointer, OutError))
					{
						return false;
					}
					Value->SetStringField(TEXT("node_class_ref"), Binding.Name);
				}
				else
				{
					return Fail(
						OutError, TEXT("ai_behavior_candidate_shape_invalid"),
						RootType, Pointer);
				}
				Value->RemoveField(TEXT("candidate"));
			}
			return true;
		}

		bool NormalizeBlackboardCandidates(
			UObject& TargetObject,
			const TArray<TSharedPtr<FJsonValue>>& Entries,
			FWorkspaceError& OutError)
		{
			for (int32 Index = 0; Index < Entries.Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Entry = Entries[Index].IsValid()
					? Entries[Index]->AsObject() : nullptr;
				if (!Entry.IsValid()) continue;
				const FString Pointer = FString::Printf(
					TEXT("/root/operation/params/entries/%d/candidate"), Index);
				TSharedPtr<FJsonObject> Candidate;
				bool bPresent = false;
				if (!CandidateObject(
						Entry, Pointer, Candidate, bPresent, OutError))
				{
					return false;
				}
				if (!bPresent) continue;
				FDomainHjsonCandidateBinding Binding;
				if (!ReadCurrentCandidate(
						TargetObject, Candidate, Pointer, Binding, OutError))
				{
					return false;
				}
				if (Binding.Kind != TEXT("blackboard_key"))
				{
					return Fail(
						OutError, TEXT("ai_behavior_candidate_kind_invalid"),
						Binding.Kind, Pointer);
				}
				if (!RejectConflict(
						Entry, TEXT("key_type_ref"), Binding.Name,
						Pointer, OutError))
				{
					return false;
				}
				Entry->SetStringField(TEXT("key_type_ref"), Binding.Name);
				Entry->RemoveField(TEXT("candidate"));
			}
			return true;
		}

		bool NormalizeSmartObjectCandidates(
			UObject& TargetObject,
			const TArray<TSharedPtr<FJsonValue>>& Slots,
			FWorkspaceError& OutError)
		{
			const FString SlotDataType =
				FSmartObjectSlotDefinition::StaticStruct()->GetPathName();
			for (int32 Index = 0; Index < Slots.Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Slot = Slots[Index].IsValid()
					? Slots[Index]->AsObject() : nullptr;
				if (!Slot.IsValid()) continue;
				const FString Pointer = FString::Printf(
					TEXT("/root/operation/params/slots/%d/candidate"), Index);
				TSharedPtr<FJsonObject> Candidate;
				bool bPresent = false;
				if (!CandidateObject(
						Slot, Pointer, Candidate, bPresent, OutError))
				{
					return false;
				}
				if (!bPresent) continue;
				FDomainHjsonCandidateBinding Binding;
				if (!ReadCurrentCandidate(
						TargetObject, Candidate, Pointer, Binding, OutError))
				{
					return false;
				}
				if (Binding.Kind != TEXT("smart_object_behavior"))
				{
					return Fail(
						OutError, TEXT("ai_behavior_candidate_kind_invalid"),
						Binding.Kind, Pointer);
				}
				if (!RejectConflict(
						Slot, TEXT("behavior_type_ref"), Binding.Name,
						Pointer, OutError)
					|| !RejectConflict(
						Slot, TEXT("data_type_ref"), SlotDataType,
						Pointer, OutError))
				{
					return false;
				}
				Slot->SetStringField(TEXT("behavior_type_ref"), Binding.Name);
				Slot->SetStringField(TEXT("data_type_ref"), SlotDataType);
				Slot->RemoveField(TEXT("candidate"));
			}
			return true;
		}

		bool NormalizeCandidates(
			const FString& RootType,
			UObject& TargetObject,
			const TSharedPtr<FJsonObject>& Native,
			FWorkspaceError& OutError)
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (RootType == BlackboardType)
			{
				return Native->TryGetArrayField(TEXT("entries"), Values)
					&& Values
					? NormalizeBlackboardCandidates(
						TargetObject, *Values, OutError)
					: Fail(OutError, TEXT("ai_behavior_domain_entries_required"));
			}
			if (RootType == SmartObjectType)
			{
				return Native->TryGetArrayField(TEXT("slots"), Values)
					&& Values
					? NormalizeSmartObjectCandidates(
						TargetObject, *Values, OutError)
					: Fail(OutError, TEXT("ai_behavior_domain_slots_required"));
			}
			return Native->TryGetArrayField(TEXT("edits"), Values)
				&& Values
				? NormalizeGraphCandidates(
					RootType, TargetObject, *Values, OutError)
				: Fail(OutError, TEXT("ai_behavior_domain_edits_required"));
		}

		bool ParseLifecycleMutation(
			const FString& Target,
			const FString& MutationPayload,
			TSharedPtr<FJsonObject>& OutMutation,
			FDomainHjsonImportLifecycleReceipt& OutReceipt,
			FWorkspaceError& OutError)
		{
			FString JsonError;
			FString MutationTarget;
			FString MutationRevision;
			FString ReceiptId;
			if (!Json::TryParseObject(
					MutationPayload, OutMutation, JsonError)
				|| !OutMutation.IsValid()
				|| !OutMutation->TryGetStringField(
					TEXT("target_ref"), MutationTarget)
				|| ObjectPathFromPackageIdentity(MutationTarget)
					!= ObjectPathFromPackageIdentity(Target)
				|| !OutMutation->TryGetStringField(
					TEXT("revision"), MutationRevision)
				|| MutationRevision.IsEmpty()
				|| !OutMutation->TryGetStringField(
					TEXT("persistence_receipt_id"), ReceiptId)
				|| !ReceiptId.StartsWith(TEXT("reload:")))
			{
				return Fail(
					OutError,
					TEXT("ai_behavior_domain_lifecycle_receipt_invalid"),
					MutationTarget,
					JsonError);
			}
			OutReceipt.bMutationApplied = true;
			OutReceipt.bCompileSettled = true;
			OutReceipt.bPackageSaved = true;
			OutReceipt.bUnloadedReloaded = true;
			OutReceipt.PersistenceReceiptId = MoveTemp(ReceiptId);
			return true;
		}

		bool TryImport(
			const FString& Target,
			const FString& RootType,
			const FString& CanonicalParamsJson,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			const FRootSpec* const Spec = FindRootSpec(RootType);
			if (!Spec)
			{
				return Fail(
					OutError, TEXT("ai_behavior_domain_root_type_invalid"),
					RootType);
			}
			FResolvedDomainHjsonDocument Document;
			if (!TryDecodeResolvedDocument(
					Target,
					CanonicalParamsJson,
					FDomainHjsonDocumentContract(
						TEXT("ai_behavior"), SchemaId, RootType),
					Document,
					OutError))
			{
				return false;
			}

			FString MutationPayload;
			if (Document.ExpectedRevision == TEXT("missing"))
			{
				if (!ValidateMissingTarget(Target, OutError)) return false;
				const TSharedPtr<FJsonObject>* Create = nullptr;
				if (!Document.Root->TryGetObjectField(TEXT("create"), Create)
					|| !Create || !Create->IsValid())
				{
					return Fail(
						OutError,
						TEXT("ai_behavior_domain_create_contract_required"));
				}
				if (Document.Root->HasField(TEXT("operation")))
				{
					return Fail(
						OutError,
						TEXT("ai_behavior_domain_create_then_edit_not_atomic"));
				}
				const FString Package = PackageIdentity(Target);
				const FString ExpectedName =
					FPackageName::GetLongPackageAssetName(Package);
				const FString ExpectedPath =
					FPackageName::GetLongPackagePath(Package);
				FString Declared;
				if ((*Create)->TryGetStringField(TEXT("asset_name"), Declared)
					&& Declared != ExpectedName)
				{
					return Fail(
						OutError,
						TEXT("ai_behavior_domain_create_name_mismatch"),
						Declared,
						ExpectedName);
				}
				if ((*Create)->TryGetStringField(TEXT("package_path"), Declared))
				{
					while (Declared.EndsWith(TEXT("/")))
					{
						Declared.LeftChopInline(1);
					}
					if (Declared != ExpectedPath)
					{
						return Fail(
							OutError,
							TEXT("ai_behavior_domain_create_path_mismatch"),
							Declared,
							ExpectedPath);
					}
				}
				const TSharedPtr<FJsonObject> Native =
					MakeShared<FJsonObject>(**Create);
				Native->SetStringField(TEXT("asset_name"), ExpectedName);
				Native->SetStringField(TEXT("package_path"), ExpectedPath);
				if (!SubmitAiCommand(
						Spec->CreateCommand, Native, MutationPayload, OutError))
				{
					return false;
				}
			}
			else
			{
				FString BeforePayload;
				FString CurrentRevision;
				UObject* TargetObject = nullptr;
				if (!TryRead(
						Target, RootType, BeforePayload, CurrentRevision,
						nullptr, &TargetObject, OutError))
				{
					return false;
				}
				if (Document.ExpectedRevision != CurrentRevision)
				{
					return Fail(
						OutError, TEXT("ai_behavior_domain_stale_revision"),
						Target, CurrentRevision);
				}
				if (Document.Root->HasField(TEXT("create")))
				{
					return Fail(
						OutError,
						TEXT("ai_behavior_domain_create_requires_missing"));
				}
				const TSharedPtr<FJsonObject>* Operation = nullptr;
				const TSharedPtr<FJsonObject>* Params = nullptr;
				FString CommandId;
				if (!Document.Root->TryGetObjectField(
						TEXT("operation"), Operation)
					|| !Operation || !Operation->IsValid()
					|| !(*Operation)->TryGetStringField(
						TEXT("command_id"), CommandId)
					|| !(*Operation)->TryGetObjectField(TEXT("params"), Params)
					|| !Params || !Params->IsValid())
				{
					return Fail(
						OutError,
						TEXT("ai_behavior_domain_operation_contract_required"));
				}
				if (CommandId != Spec->MutationCommand)
				{
					return Fail(
						OutError,
						TEXT("ai_behavior_domain_operation_not_supported"),
						CommandId,
						Spec->MutationCommand);
				}
				const TSharedPtr<FJsonObject> Native =
					MakeShared<FJsonObject>(**Params);
				FString NativeTarget;
				if (Native->TryGetStringField(TEXT("target_ref"), NativeTarget)
					&& ObjectPathFromPackageIdentity(NativeTarget)
						!= ObjectPathFromPackageIdentity(Target))
				{
					return Fail(
						OutError,
						TEXT("ai_behavior_domain_operation_target_mismatch"),
						NativeTarget,
						Target);
				}
				Native->SetStringField(
					TEXT("target_ref"), ObjectPathFromPackageIdentity(Target));
				if (!TargetObject
					|| !NormalizeCandidates(
						RootType, *TargetObject, Native, OutError))
				{
					return false;
				}
				if (!SubmitAiCommand(
						CommandId, Native, MutationPayload, OutError))
				{
					return false;
				}
			}

			TSharedPtr<FJsonObject> Mutation;
			FDomainHjsonImportLifecycleReceipt LifecycleReceipt;
			if (!ParseLifecycleMutation(
					Target, MutationPayload, Mutation,
					LifecycleReceipt, OutError))
			{
				return false;
			}
			FString AfterPayload;
			FString AfterRevision;
			TSharedPtr<FJsonObject> AfterSnapshot;
			if (!TryRead(
					Target, RootType, AfterPayload, AfterRevision,
					&AfterSnapshot, nullptr, OutError))
			{
				return false;
			}
			FString CompileStatus;
			if (!AfterSnapshot.IsValid()
				|| !AfterSnapshot->TryGetStringField(
					TEXT("compile_status"), CompileStatus)
				|| CompileStatus != TEXT("valid"))
			{
				return Fail(
					OutError,
					TEXT("ai_behavior_domain_compile_not_settled"),
					Target,
					CompileStatus);
			}
			return TryBuildImportSuccessPayload(
				TEXT("ai-behavior-domain-provider"),
				Target,
				AfterRevision,
				LifecycleReceipt,
				AfterSnapshot,
				Mutation,
				OutPayload,
				OutError);
		}
	}

	bool TryExecuteContentGraph(
		const FString& CommandId,
		const FString& TargetIdentity,
		const FString& RootTypeIdentity,
		const FString& CanonicalParamsJson,
		FString& OutPayloadJson,
		bool& bOutReadOnly,
		FWorkspaceError& OutError)
	{
		OutPayloadJson.Reset();
		bOutReadOnly = false;
		OutError.Reset();
		TSharedPtr<FJsonObject> Params;
		FString JsonError;
		if (!Json::TryParseObject(CanonicalParamsJson, Params, JsonError)
			|| !Params.IsValid())
		{
			return Fail(
				OutError, TEXT("ai_behavior_domain_params_invalid"), JsonError);
		}
		FString Target;
		if (!Params->TryGetStringField(TEXT("target"), Target)
			|| ObjectPathFromPackageIdentity(Target)
				!= ObjectPathFromPackageIdentity(TargetIdentity))
		{
			return Fail(
				OutError,
				TEXT("ai_behavior_domain_target_identity_mismatch"),
				TargetIdentity,
				Target);
		}
		if (!FindRootSpec(RootTypeIdentity))
		{
			return Fail(
				OutError, TEXT("ai_behavior_domain_root_type_invalid"),
				RootTypeIdentity);
		}
		if (CommandId == TEXT("content_graph.import_workspace"))
		{
			return TryImport(
				Target, RootTypeIdentity, CanonicalParamsJson,
				OutPayloadJson, OutError);
		}
		if (CommandId == TEXT("content_graph.export_workspace"))
		{
			return Fail(
				OutError, TEXT("ai_behavior_domain_export_not_registered"),
				Target);
		}
		if (CommandId.StartsWith(TEXT("content_graph.")))
		{
			FString Revision;
			bOutReadOnly = true;
			return TryRead(
				Target, RootTypeIdentity, OutPayloadJson, Revision,
				nullptr, nullptr, OutError);
		}
		return Fail(
			OutError, TEXT("ai_behavior_domain_command_not_supported"),
			CommandId);
	}
}
