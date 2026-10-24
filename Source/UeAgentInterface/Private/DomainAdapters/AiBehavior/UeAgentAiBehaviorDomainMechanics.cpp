#include "DomainAdapters/AiBehavior/UeAgentAiBehaviorDomainMechanics.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "Domains/BehaviorTree/Authoring/UeAgentBehaviorTreeGraphDocument.h"
#include "Domains/Blackboard/Authoring/UeAgentBlackboardDocument.h"
#include "Domains/Eqs/Authoring/UeAgentEqsGraphDocument.h"
#include "Domains/StateTree/Authoring/UeAgentStateTreeDocument.h"
#include "StateTree.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "UObject/ObjectMacros.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringApplyProvider.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringAssets.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringExport.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringRegistration.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringState.h"
#include "Domains/SmartObject/Authoring/UeAgentSmartObjectSemanticImport.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "OperationOwners/AiCommands/UeAgentAiCommandOperationOwner.h"
#include "QueryOwners/AiBehavior/UeAgentAiBehaviorCandidateQuery.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
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

		bool Fail(
			FWorkspaceError& OutError,
			UE::UAI::Contract::FDiagnostic Diagnostic,
			FString Source = FString(),
			FString Pointer = TEXT("/"));

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

		const UeAgentReflectionAuthoring::FProfile* ReflectionProfileForRootType(
			const FString& RootType)
		{
			const TCHAR* TypeId = RootType == BehaviorTreeType
				? TEXT("behavior_tree")
				: RootType == BlackboardType
					? TEXT("blackboard")
				: RootType == StateTreeType
					? TEXT("state_tree")
				: RootType == EqsType ? TEXT("eqs")
				: RootType == SmartObjectType ? TEXT("smart_object") : nullptr;
			if (!TypeId) return nullptr;
			const UeAgentReflectionAuthoring::FProfile* Profile =
				UeAgentReflectionAuthoring::FindProfileByTypeId(TypeId);
			return Profile && Profile->AssetClassPath == RootType ? Profile : nullptr;
		}

		bool EnsureReflectionAuthoringRegistered(FWorkspaceError& OutError)
		{
			static const TPair<bool, UE::UAI::Contract::FDiagnostic> Registration = []()
			{
				UE::UAI::Contract::FDiagnostic Error;
				const bool bRegistered =
					UeAgentReflectionAuthoring::RegisterAuthoring(Error);
				return TPair<bool, UE::UAI::Contract::FDiagnostic>(bRegistered, MoveTemp(Error));
			}();
			if (!Registration.Key)
			{
				return Fail(OutError, Registration.Value, FString(), TEXT("/"));
			}
			return true;
		}

		bool Fail(
			FWorkspaceError& OutError,
			UE::UAI::Contract::FDiagnostic Diagnostic,
			FString Source,
			FString Pointer)
		{
			OutError.SetFailure(MoveTemp(Diagnostic), Source, Pointer);
			return false;
		}

		UE::UAI::Contract::FDiagnostic ValueFailure(const TCHAR* Code, const TCHAR* Reason,
			const FString& Pointer, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const FString& Target = FString())
		{
			auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Pointer, Expected, Actual, TEXT("input_decode"), Reason);
			Diagnostic.TargetPath = Target;
			return Diagnostic;
		}

		UE::UAI::Contract::FDiagnostic NativeFailure(const TCHAR* Code, const TCHAR* Reason,
			const FString& Pointer, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const FString& Target, const TCHAR* Stage)
		{
			auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Pointer, Expected, Actual, Stage, Reason);
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.bRetrySafe = false;
			Diagnostic.TargetPath = Target;
			return Diagnostic;
		}

		TSharedPtr<FJsonValue> StringValues(const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			for (const FString& Value : Values) Items.Add(MakeShared<FJsonValueString>(Value));
			return MakeShared<FJsonValueArray>(Items);
		}

		FString PackageIdentity(const FString& Target)
		{
			return Target.Contains(TEXT("."))
				? FPackageName::ObjectPathToPackageName(Target)
				: Target;
		}

		bool SubmitAiCommand(
			const FString& CommandId,
			const TSharedPtr<FJsonObject>& Params,
			FString& OutPayload,
			FWorkspaceError& OutError,
			const FResolvedDomainHjsonDocument& Document, const FString& SourcePrefix)
		{
			FScopedDomainHjsonDiagnosticSource NativeSource(Document, OutError, SourcePrefix);
			FString ParamsJson;
			if (!TryCanonicalizeObject(Params, ParamsJson, OutError)) return false;
			const TArray<FString> AuthorityScopes = {
				TEXT("uai:query"),
				TEXT("uai:operate"),
				TEXT("uai:authoring:hjson:import"),
				TEXT("pcg_ai_node_graph.write"),
			};
			UE::UAI::Contract::FDiagnostic CommandDiagnostic;
			if (!OperationOwners::AiCommands::TryExecuteInternal(
					CommandId,
					ParamsJson,
					Contract::FUeAgentCallerContext(
						TEXT("ai-behavior-domain-provider"),
						AuthorityScopes,
						TEXT("ai-behavior-domain-provider")),
					OutPayload,
					CommandDiagnostic))
			{
				SetWorkspaceErrorFromCommandFailure(CommandDiagnostic,
					TEXT("ai_behavior_domain_native_command_failed"), CommandId, OutError);
				return false;
			}
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
				return Fail(OutError, ValueFailure(TEXT("ai_behavior_domain_target_invalid"), TEXT("The target reference cannot be inspected as an asset target."),
					TEXT("/target"), MakeShared<FJsonValueString>(Target), MakeShared<FJsonValueString>(TEXT("valid asset target reference")), Target));
			}
			if (bPresent)
			{
				return Fail(OutError, ValueFailure(TEXT("ai_behavior_domain_stale_revision"), TEXT("Creation expected a missing target, but the asset already exists."),
					TEXT("/expected_revision"), MakeShared<FJsonValueString>(TEXT("existing target")), MakeShared<FJsonValueString>(TEXT("missing target")), Target));
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
				return Fail(OutError, ValueFailure(TEXT("domain_hjson_candidate_binding_invalid"), TEXT("The candidate binding must be an object."),
					Pointer, Value->TryGetField(TEXT("candidate")), MakeShared<FJsonValueString>(TEXT("object"))));
			}
			OutCandidate = *Candidate;
			return true;
		}

		bool RejectConflict(
			const TSharedPtr<FJsonObject>& Value,
			const FString& Field,
			const FString& Expected,
			const FString& Pointer,
			FWorkspaceError& OutError,
			const bool bPublicValue = true)
		{
			FString Existing;
			if (Value->TryGetStringField(Field, Existing)
				&& Existing != Expected)
			{
				auto Diagnostic = ValueFailure(TEXT("ai_behavior_candidate_field_conflict"), TEXT("The supplied field conflicts with the resolved candidate."),
					Pointer + TEXT("/") + Field,
					bPublicValue ? MakeShared<FJsonValueString>(Existing) : TSharedPtr<FJsonValue>(),
					bPublicValue ? MakeShared<FJsonValueString>(Expected) : TSharedPtr<FJsonValue>());
				if (!bPublicValue)
				{
					Diagnostic.ActualValuePresent = true;
					Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Existing);
					Diagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(Expected);
				}
				return Fail(OutError, MoveTemp(Diagnostic));
			}
			return true;
		}

		bool ResolveCandidate(
			UObject& TargetObject,
			const TSharedPtr<FJsonObject>& Candidate,
			const FString& Pointer,
			QueryOwners::AiBehavior::FAiBehaviorCandidate& OutCandidate,
			FWorkspaceError& OutError)
		{
			FDomainHjsonCandidateSelector Selector;
			if (!TryReadCandidateSelector(
					Candidate, Pointer, Selector, OutError))
			{
				return false;
			}
			QueryOwners::AiBehavior::FAiBehaviorCandidateQueryError CandidateError;
			if (!QueryOwners::AiBehavior::TryResolveAiBehaviorCandidate(
					TargetObject,
					Selector.Name,
				FString(),
					OutCandidate,
					CandidateError, Selector.Select))
			{
				OutError.SetFailure(CandidateError, FString(), Pointer);
				return false;
			}
			if (!OutCandidate.bSupported)
			{
				auto Diagnostic = Contract::FDiagnostic::WrapFailure(TEXT("ai_behavior_candidate_operation_unsupported"), OutCandidate.SupportDiagnostic);
				return Fail(OutError, MoveTemp(Diagnostic), FString(), Pointer);
			}
			if (!OutCandidate.SupportedOperations.Contains(TEXT("create")))
			{
				auto Diagnostic = ValueFailure(TEXT("ai_behavior_candidate_operation_unsupported"),
					TEXT("The resolved candidate does not support creation."),
					Pointer, StringValues(OutCandidate.SupportedOperations), MakeShared<FJsonValueString>(TEXT("supported candidate with create operation")));
				Diagnostic.CandidateName = OutCandidate.Name;
				return Fail(OutError, MoveTemp(Diagnostic));
			}
			return RejectConflict(
					Candidate, TEXT("name"), OutCandidate.Name, Pointer, OutError)
				&& RejectConflict(
					Candidate, TEXT("id"), OutCandidate.Id, Pointer, OutError, false)
				&& RejectConflict(
					Candidate, TEXT("kind"), OutCandidate.Kind, Pointer, OutError)
				&& RejectConflict(
					Candidate, TEXT("action_kind"), OutCandidate.ActionKind,
					Pointer, OutError)
				&& RejectConflict(
					Candidate, TEXT("payload_type"), OutCandidate.PayloadType,
					Pointer, OutError)
				&& RejectConflict(
					Candidate, TEXT("input_contract"), OutCandidate.InputContract,
					Pointer, OutError)
				&& RejectConflict(
					Candidate, TEXT("placement_contract"),
					OutCandidate.PlacementContract, Pointer, OutError)
				&& RejectConflict(
					Candidate, TEXT("required_context"),
					OutCandidate.RequiredContext, Pointer, OutError)
				&& RejectConflict(
					Candidate, TEXT("binding_revision"),
					OutCandidate.BindingRevision, Pointer, OutError, false);
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
				const FString ValuePointer = FString::Printf(TEXT("/root/operation/params/edits/%d/value"), Index);
				const FString Pointer = ValuePointer + TEXT("/candidate");
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
					return Fail(OutError, ValueFailure(TEXT("ai_behavior_candidate_placement_invalid"), TEXT("A create candidate can only be placed as an item."),
						FString::Printf(TEXT("/root/operation/params/edits/%d/element_kind"), Index),
						Edit->TryGetField(TEXT("element_kind")), MakeShared<FJsonValueString>(TEXT("item"))));
				}
				QueryOwners::AiBehavior::FAiBehaviorCandidate Resolved;
				if (!ResolveCandidate(
						TargetObject, Candidate, Pointer, Resolved, OutError))
				{
					return false;
				}

				if (RootType == BehaviorTreeType)
				{
					const FString Carrier = BehaviorCarrier(Resolved.Kind);
					if (Carrier.IsEmpty() || Resolved.PayloadType.IsEmpty())
					{
						return Fail(OutError, ValueFailure(TEXT("ai_behavior_candidate_kind_invalid"),
							Carrier.IsEmpty() ? TEXT("The candidate kind has no Behavior Tree carrier.") : TEXT("The Behavior Tree candidate has no payload type."),
							Pointer, MakeShared<FJsonValueString>(Carrier.IsEmpty() ? Resolved.Kind : Resolved.PayloadType),
							MakeShared<FJsonValueString>(Carrier.IsEmpty() ? TEXT("Behavior Tree carrier kind") : TEXT("nonempty payload type"))));
					}
					if (!RejectConflict(
							Value, TEXT("carrier_type_ref"), Carrier,
							ValuePointer, OutError)
						|| !RejectConflict(
							Value, TEXT("payload_type_ref"), Resolved.PayloadType,
							ValuePointer, OutError))
					{
						return false;
					}
					Value->SetStringField(TEXT("carrier_type_ref"), Carrier);
					Value->SetStringField(
						TEXT("payload_type_ref"), Resolved.PayloadType);
				}
				else if (RootType == EqsType)
				{
					if (Resolved.Kind != TEXT("eqs_generator"))
					{
						return Fail(OutError, ValueFailure(TEXT("ai_behavior_candidate_kind_invalid"), TEXT("An EQS graph item requires an EQS generator candidate."),
							Pointer, MakeShared<FJsonValueString>(Resolved.Kind), MakeShared<FJsonValueString>(TEXT("eqs_generator"))));
					}
					if (!RejectConflict(
							Value, TEXT("carrier_type_ref"), EqsCarrierType,
							ValuePointer, OutError)
						|| !RejectConflict(
							Value, TEXT("payload_type_ref"), Resolved.Name,
							ValuePointer, OutError))
					{
						return false;
					}
					Value->SetStringField(TEXT("carrier_type_ref"), EqsCarrierType);
					Value->SetStringField(TEXT("payload_type_ref"), Resolved.Name);
				}
				else if (RootType == StateTreeType)
				{
					if (!Resolved.Kind.StartsWith(TEXT("state_tree_")))
					{
						return Fail(OutError, ValueFailure(TEXT("ai_behavior_candidate_kind_invalid"), TEXT("A StateTree item requires a StateTree candidate kind."),
							Pointer, MakeShared<FJsonValueString>(Resolved.Kind), MakeShared<FJsonValueString>(TEXT("state_tree_*"))));
					}
					if (!RejectConflict(
							Value, TEXT("node_class_ref"), Resolved.Name,
							ValuePointer, OutError))
					{
						return false;
					}
					Value->SetStringField(TEXT("node_class_ref"), Resolved.Name);
				}
				else
				{
					return Fail(OutError, ValueFailure(TEXT("ai_behavior_candidate_shape_invalid"), TEXT("This asset type does not accept graph-item candidate creation."),
						Pointer, MakeShared<FJsonValueString>(RootType), StringValues({BehaviorTreeType, EqsType, StateTreeType})));
				}
				// BehaviorTree replays the native menu action with this candidate and position.
				// The other graph owners consume only their resolved payload type.
				if (RootType != BehaviorTreeType) Value->RemoveField(TEXT("candidate"));
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
				const FString EntryPointer = FString::Printf(TEXT("/root/operation/params/entries/%d"), Index);
				const FString Pointer = EntryPointer + TEXT("/candidate");
				TSharedPtr<FJsonObject> Candidate;
				bool bPresent = false;
				if (!CandidateObject(
						Entry, Pointer, Candidate, bPresent, OutError))
				{
					return false;
				}
				if (!bPresent) continue;
				QueryOwners::AiBehavior::FAiBehaviorCandidate Resolved;
				if (!ResolveCandidate(
						TargetObject, Candidate, Pointer, Resolved, OutError))
				{
					return false;
				}
				if (Resolved.Kind != TEXT("blackboard_key"))
				{
					return Fail(OutError, ValueFailure(TEXT("ai_behavior_candidate_kind_invalid"), TEXT("A Blackboard entry requires a Blackboard key candidate."),
						Pointer, MakeShared<FJsonValueString>(Resolved.Kind), MakeShared<FJsonValueString>(TEXT("blackboard_key"))));
				}
				if (!RejectConflict(
						Entry, TEXT("key_type_ref"), Resolved.Name,
						EntryPointer, OutError))
				{
					return false;
				}
				Entry->SetStringField(TEXT("key_type_ref"), Resolved.Name);
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
				const FString SlotPointer = FString::Printf(TEXT("/root/operation/params/slots/%d"), Index);
				const FString Pointer = SlotPointer + TEXT("/candidate");
				TSharedPtr<FJsonObject> Candidate;
				bool bPresent = false;
				if (!CandidateObject(
						Slot, Pointer, Candidate, bPresent, OutError))
				{
					return false;
				}
				if (!bPresent) continue;
				QueryOwners::AiBehavior::FAiBehaviorCandidate Resolved;
				if (!ResolveCandidate(
						TargetObject, Candidate, Pointer, Resolved, OutError))
				{
					return false;
				}
				if (Resolved.Kind != TEXT("smart_object_behavior"))
				{
					return Fail(OutError, ValueFailure(TEXT("ai_behavior_candidate_kind_invalid"), TEXT("A Smart Object slot requires a behavior candidate."),
						Pointer, MakeShared<FJsonValueString>(Resolved.Kind), MakeShared<FJsonValueString>(TEXT("smart_object_behavior"))));
				}
				if (!RejectConflict(
						Slot, TEXT("behavior_type_ref"), Resolved.Name,
						SlotPointer, OutError)
					|| !RejectConflict(
						Slot, TEXT("data_type_ref"), SlotDataType,
						SlotPointer, OutError))
				{
					return false;
				}
				Slot->SetStringField(TEXT("behavior_type_ref"), Resolved.Name);
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
					: Fail(OutError, ValueFailure(TEXT("ai_behavior_domain_entries_required"), TEXT("Blackboard operations require an entries array."),
						TEXT("/root/operation/params/entries"), Native->TryGetField(TEXT("entries")), MakeShared<FJsonValueString>(TEXT("array"))));
			}
			if (RootType == SmartObjectType)
			{
				return Native->TryGetArrayField(TEXT("slots"), Values)
					&& Values
					? NormalizeSmartObjectCandidates(
						TargetObject, *Values, OutError)
					: Fail(OutError, ValueFailure(TEXT("ai_behavior_domain_slots_required"), TEXT("Smart Object operations require a slots array."),
						TEXT("/root/operation/params/slots"), Native->TryGetField(TEXT("slots")), MakeShared<FJsonValueString>(TEXT("array"))));
			}
			return Native->TryGetArrayField(TEXT("edits"), Values)
				&& Values
				? NormalizeGraphCandidates(
					RootType, TargetObject, *Values, OutError)
				: Fail(OutError, ValueFailure(TEXT("ai_behavior_domain_edits_required"), TEXT("Graph operations require an edits array."),
					TEXT("/root/operation/params/edits"), Native->TryGetField(TEXT("edits")), MakeShared<FJsonValueString>(TEXT("array"))));
		}

		bool ParseLifecycleMutation(
			const FString& Target,
			const FString& MutationPayload,
			TSharedPtr<FJsonObject>& OutMutation,
			FWorkspaceError& OutError)
		{
			return TryReadNativeMutationPayload(Target, MutationPayload, OutMutation, OutError);
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
				return Fail(OutError, NativeFailure(TEXT("ai_behavior_domain_root_type_invalid"), TEXT("The bound root asset type is not supported by AI authoring."),
					TEXT("/root_type"), MakeShared<FJsonValueString>(RootType), StringValues({BehaviorTreeType, BlackboardType, EqsType, StateTreeType, SmartObjectType}), Target, TEXT("target_binding")));
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
			UE::UAI::HjsonWorkspace::DomainAuthoring::FScopedDomainHjsonDiagnosticSource DiagnosticSource(Document, OutError);
			if (Document.bSemanticWorkspaceDirectory)
			{
				using namespace UE::UAI::Composition::HjsonImport;
				if (!Document.SemanticWorkspace.IsValid())
				{
					return Fail(OutError, NativeFailure(TEXT("ai_behavior_domain_semantic_workspace_missing"), TEXT("The decoded semantic workspace directory has no loaded workspace model."),
						TEXT("/workspace_model"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueString>(TEXT("loaded semantic workspace")), Target, TEXT("workspace_decode")));
				}
				UObject* Asset = nullptr;
				DomainAdapters::ContentGraphRead::FUeAgentContentGraphCapture
					TargetCapture;
				if (!TryCaptureObject(
						Target,
						TArray<FString>{BehaviorTreeType, BlackboardType, StateTreeType, EqsType, SmartObjectType},
						Asset,
						TargetCapture,
						OutError))
				{
					return false;
				}
				const FString ActualRootType = Asset
					? Asset->GetClass()->GetPathName() : FString();
				const UeAgentReflectionAuthoring::FProfile* Profile =
					ReflectionProfileForRootType(ActualRootType);
				if (!Profile || ActualRootType != RootType)
				{
					return Fail(OutError, NativeFailure(TEXT("ai_behavior_domain_semantic_target_type_invalid"),
						!Profile ? TEXT("The resolved asset has no supported AI authoring profile.") : TEXT("The resolved asset type differs from the requested root type."),
						TEXT("/target"), MakeShared<FJsonValueString>(ActualRootType), MakeShared<FJsonValueString>(RootType), Target, TEXT("target_binding")));
				}
				if (!EnsureReflectionAuthoringRegistered(OutError)) return false;

				const FString TargetPackage = PackageIdentity(Target);
				TArray<UeAgentAuthoringState::FReadRequest> Requests;
				UE::UAI::Contract::FDiagnostic CaptureError;
				if (!UeAgentReflectionAuthoring::BuildAssetExportSnapshotReadRequests(
						*Profile, *Asset, Requests, CaptureError))
				{
					OutError.SetFailure(CaptureError, Target);
					return false;
				}
				UeAgentAuthoringState::FCaptureResult Capture;
				if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
				{
					OutError.SetFailure(Capture.Diagnostic, Target);
					return false;
				}

				ContextResource::FUeAgentResourceTracker Resources =
					MakeSemanticWorkspaceResourceTracker();
				StateSemantic::FUeAgentCanonicalSnapshotPtr CanonicalSnapshot;
				FAuthoringModelPtr Model;
				if (!TryBuildCanonicalAuthoringCapture(
						ResolveTargetCompositionKind(
							Profile->TargetKind, Profile->TypeId),
						TEXT("llm_authoring"),
						Capture.Snapshot,
						Resources,
						CanonicalSnapshot,
						Model,
						OutError)
					|| !CanonicalSnapshot.IsValid() || !Model.IsValid())
				{
					if (OutError.GetCode().IsEmpty())
					{
						auto Diagnostic = NativeFailure(TEXT("ai_behavior_domain_semantic_capture_failed"),
							!CanonicalSnapshot.IsValid() ? TEXT("Semantic capture did not provide a canonical snapshot.")
								: !Model.IsValid() ? TEXT("Semantic capture did not provide an authoring model.") : TEXT("Semantic capture failed without providing a diagnostic."),
							!CanonicalSnapshot.IsValid() ? TEXT("/canonical_snapshot") : !Model.IsValid() ? TEXT("/authoring_model") : TEXT("/capture"),
							nullptr, MakeShared<FJsonValueString>(TEXT("successful semantic capture with snapshot and model")), Target, TEXT("semantic_capture"));
						return Fail(OutError, MoveTemp(Diagnostic));
					}
					return false;
				}

				Capture.Snapshot.ReleaseNativeValues();
				const FString TargetObjectPath =
					ObjectPathFromPackageIdentity(Target);
				{
					return TryCoordinateSemanticWorkspaceImport(
						Document.SemanticWorkspace.ToSharedRef(), Model.ToSharedRef(), CanonicalSnapshot.ToSharedRef(),
						Contract::FUeAgentDescriptorIdentity(TEXT("ai_behavior.semantic_workspace"), 1, Model->GetSemanticFingerprint()),
						TargetObjectPath, Resources,
						{TEXT("ai-behavior-domain-provider"), Target, TargetObjectPath, TEXT("ai-behavior-semantic"),
							TEXT("ai_behavior_domain_semantic_prepare_failed"), TEXT("ai_behavior_domain_semantic_apply_failed"), TEXT("ai_behavior_domain_semantic_readback_payload_invalid"),
							TEXT("ai_behavior_domain_compile_not_settled"),
							[Profile, TargetObjectPath](TArray<UeAgentAuthoringState::FReadRequest>& Requests, UE::UAI::Contract::FDiagnostic& Error)
							{
								UObject* Native = StaticLoadObject(UObject::StaticClass(), nullptr, *TargetObjectPath);
								if (!Native)
								{
									Error = NativeFailure(TEXT("ai_behavior_domain_semantic_readback_target_unresolved"), TEXT("The AI asset could not be loaded for semantic readback."),
										TEXT("/target"), MakeShared<FJsonValueString>(TargetObjectPath), MakeShared<FJsonValueString>(TEXT("loadable AI asset")), TargetObjectPath, TEXT("semantic_readback"));
									return false;
								}
								return UeAgentReflectionAuthoring::BuildAssetExportSnapshotReadRequests(*Profile, *Native, Requests, Error);
							}},
						[Profile, TargetPackage, &Asset, &Document](const FString& RequestId,
							const FPreparedAuthoringImport& Prepared, FSemanticWorkspaceChangeProjection&& Projection,
							FRootPropertyApplyOutcome& OutOutcome)
						{
							UBehaviorTree* Tree = Cast<UBehaviorTree>(Asset);
							UBlackboardData* Blackboard = Cast<UBlackboardData>(Asset);
							TSharedPtr<FJsonObject> Graph;
							TMap<FString, Contract::FSourceLocation> GraphSources;
							FWorkspaceError Error;
							UEnvQuery* Query = Cast<UEnvQuery>(Asset);
							UStateTree* StateTree = Cast<UStateTree>(Asset);
							USmartObjectDefinition* SmartObject = Cast<USmartObjectDefinition>(Asset);
							const bool bProjected = Tree ? UeAgentBehaviorTreeAuthoring::BuildGraphDocumentFromSemanticModel(*Tree,
								Document.SemanticWorkspace->GetModel(), Prepared, Projection, Graph, Error, &GraphSources, &OutOutcome.GraphReferenceBindings)
								: Blackboard ? UeAgentBlackboardAuthoring::BuildDocumentFromSemanticModel(*Blackboard,
									Document.SemanticWorkspace->GetModel(), Prepared, Projection, Graph, Error)
								: Query ? UeAgentEqsAuthoring::BuildGraphDocumentFromSemanticModel(*Query,
									Document.SemanticWorkspace->GetModel(), Prepared, Projection, Graph, Error, &GraphSources, &OutOutcome.GraphReferenceBindings)
								: StateTree ? UeAgentStateTreeAuthoring::BuildTreeDocumentFromSemanticModel(*StateTree,
									Document.SemanticWorkspace->GetModel(), Prepared, Projection, Graph, Error)
								: SmartObject && UeAgentSmartObjectAuthoring::BuildDefinitionDocumentFromSemanticModel(*SmartObject,
									Document.SemanticWorkspace->GetModel(), Prepared, Projection, Graph, Error);
							if (!bProjected)
							{
								OutOutcome.Phase = TEXT("semantic_projection");
								OutOutcome.Diagnostic = Error.HasErrors() ? Error.GetDiagnostic() : NativeFailure(TEXT("ai_graph_semantic_projection_failed"),
									TEXT("The AI document projection failed without providing a diagnostic."), TEXT("/semantic_projection"),
									MakeShared<FJsonValueString>(Asset ? Asset->GetClass()->GetPathName() : FString()),
									MakeShared<FJsonValueString>(TEXT("successful document projection")), TargetPackage, TEXT("semantic_projection"));
								if (OutOutcome.Diagnostic.TargetPath.IsEmpty()) OutOutcome.Diagnostic.TargetPath = TargetPackage;
								return false;
							}
							// Generated values carry their original parser sources through the
							// existing inline/prepared-copy projection, before native execution.
							UeAgentAuthoringDiagnostics::FScopedSourceLocations GraphSourceScope(GraphSources.IsEmpty() ? nullptr : Graph.Get(), [&GraphSources](const FString& Pointer) {
								return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(GraphSources, Pointer);
							});
							UeAgentReflectionAuthoring::FAssetApplyRequest ApplyRequest;
							ApplyRequest.TypeId = Profile->TypeId;
							ApplyRequest.Operation = UeAgentApplyInputContract::EOperation::Update;
							ApplyRequest.AssetPath = TargetPackage;
							ApplyRequest.Properties = MoveTemp(Projection.RootObjectProperties);
							ApplyRequest.ProviderExtension = MakeShared<FJsonObject>();
							ApplyRequest.ProviderExtension->SetObjectField(Profile->ProfileStateField, Graph);
							ApplyRequest.bCompactValues = true;
							ApplyRequest.bSave = true;
							ApplyRequest.FinalReadback = OutOutcome.FinalReadback;
							Tree = nullptr;
							Blackboard = nullptr;
							Query = nullptr;
							StateTree = nullptr;
							SmartObject = nullptr;
							Asset = nullptr;
							UeAgentReflectionAuthoring::FAssetApplyResult Result;
							const bool bApplied = UeAgentReflectionAuthoring::ApplyAsset(ApplyRequest, Result, RequestId);
							OutOutcome.bApplied = bApplied && Result.Status == TEXT("applied");
							OutOutcome.Phase = MoveTemp(Result.Phase);
							OutOutcome.Diagnostic = MoveTemp(Result.Diagnostic);
							return bApplied;
						},
						[&Target, &ActualRootType](FString& Payload, FString& Revision, FWorkspaceError& Error) {
							return TryRead(Target, ActualRootType, Payload, Revision, nullptr, nullptr, Error);
						}, OutPayload, OutError);
				}

			}

			FString MutationPayload;
			if (Document.ExpectedRevision == TEXT("missing"))
			{
				if (!ValidateMissingTarget(Target, OutError)) return false;
				const TSharedPtr<FJsonObject>* Create = nullptr;
				if (!Document.Root->TryGetObjectField(TEXT("create"), Create)
					|| !Create || !Create->IsValid())
				{
					return Fail(OutError, ValueFailure(TEXT("ai_behavior_domain_create_contract_required"), TEXT("Creating a missing AI asset requires a create object."),
						TEXT("/root/create"), Document.Root->TryGetField(TEXT("create")), MakeShared<FJsonValueString>(TEXT("object")), Target));
				}
				if (Document.Root->HasField(TEXT("operation")))
				{
					return Fail(OutError, ValueFailure(TEXT("ai_behavior_domain_create_then_edit_not_atomic"), TEXT("Asset creation and a subsequent operation cannot be combined in this import."),
						TEXT("/root/operation"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueString>(TEXT("operation field absent during creation")), Target));
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
					return Fail(OutError, ValueFailure(TEXT("ai_behavior_domain_create_name_mismatch"), TEXT("The declared asset name differs from the target asset name."),
						TEXT("/root/create/asset_name"), MakeShared<FJsonValueString>(Declared), MakeShared<FJsonValueString>(ExpectedName), Target));
				}
				if ((*Create)->TryGetStringField(TEXT("package_path"), Declared))
				{
					while (Declared.EndsWith(TEXT("/")))
					{
						Declared.LeftChopInline(1);
					}
					if (Declared != ExpectedPath)
					{
						return Fail(OutError, ValueFailure(TEXT("ai_behavior_domain_create_path_mismatch"), TEXT("The declared package directory differs from the target package directory."),
							TEXT("/root/create/package_path"), MakeShared<FJsonValueString>(Declared), MakeShared<FJsonValueString>(ExpectedPath), Target));
					}
				}
				const TSharedPtr<FJsonObject> Native =
					MakeShared<FJsonObject>(**Create);
				Native->SetStringField(TEXT("asset_name"), ExpectedName);
				Native->SetStringField(TEXT("package_path"), ExpectedPath);
				if (!SubmitAiCommand(
						Spec->CreateCommand, Native, MutationPayload, OutError, Document, TEXT("/root/create")))
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
					auto Diagnostic = ValueFailure(TEXT("ai_behavior_domain_stale_revision"), TEXT("The asset revision changed after the workspace revision was recorded."),
						TEXT("/expected_revision"), nullptr, nullptr, Target);
					Diagnostic.ActualValuePresent = true;
					Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(CurrentRevision);
					Diagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(Document.ExpectedRevision);
					return Fail(OutError, MoveTemp(Diagnostic));
				}
				if (Document.Root->HasField(TEXT("create")))
				{
					return Fail(OutError, ValueFailure(TEXT("ai_behavior_domain_create_requires_missing"), TEXT("A create object is only allowed when the expected target is missing."),
						TEXT("/root/create"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueString>(TEXT("create field absent for an existing asset")), Target));
				}
				const TSharedPtr<FJsonObject>* Operation = nullptr;
				const TSharedPtr<FJsonObject>* Params = nullptr;
				FString CommandId;
				if (!Document.Root->TryGetObjectField(
						TEXT("operation"), Operation)
					|| !Operation || !Operation->IsValid())
				{
					return Fail(OutError, ValueFailure(TEXT("ai_behavior_domain_operation_contract_required"), TEXT("Updating an AI asset requires an operation object."),
						TEXT("/root/operation"), Document.Root->TryGetField(TEXT("operation")), MakeShared<FJsonValueString>(TEXT("object")), Target));
				}
				if (!(*Operation)->TryGetStringField(TEXT("command_id"), CommandId))
					return Fail(OutError, ValueFailure(TEXT("ai_behavior_domain_operation_contract_required"), TEXT("The operation requires a string command_id."),
						TEXT("/root/operation/command_id"), (*Operation)->TryGetField(TEXT("command_id")), MakeShared<FJsonValueString>(TEXT("string")), Target));
				if (!(*Operation)->TryGetObjectField(TEXT("params"), Params) || !Params || !Params->IsValid())
					return Fail(OutError, ValueFailure(TEXT("ai_behavior_domain_operation_contract_required"), TEXT("The operation requires a params object."),
						TEXT("/root/operation/params"), (*Operation)->TryGetField(TEXT("params")), MakeShared<FJsonValueString>(TEXT("object")), Target));
				if (CommandId != Spec->MutationCommand)
				{
					return Fail(OutError, ValueFailure(TEXT("ai_behavior_domain_operation_not_supported"), TEXT("The operation command is not supported for this AI asset type."),
						TEXT("/root/operation/command_id"), MakeShared<FJsonValueString>(CommandId), MakeShared<FJsonValueString>(Spec->MutationCommand), Target));
				}
				const TSharedPtr<FJsonObject> Native =
					MakeShared<FJsonObject>(**Params);
				FString NativeTarget;
				if (Native->TryGetStringField(TEXT("target_ref"), NativeTarget)
					&& ObjectPathFromPackageIdentity(NativeTarget)
						!= ObjectPathFromPackageIdentity(Target))
				{
					return Fail(OutError, ValueFailure(TEXT("ai_behavior_domain_operation_target_mismatch"), TEXT("The operation target differs from the import target."),
						TEXT("/root/operation/params/target_ref"), MakeShared<FJsonValueString>(NativeTarget), MakeShared<FJsonValueString>(Target), Target));
				}
				Native->SetStringField(
					TEXT("target_ref"), ObjectPathFromPackageIdentity(Target));
				if (!TargetObject)
				{
					auto Diagnostic = NativeFailure(TEXT("ai_behavior_domain_operation_target_missing"), TEXT("The asset readback did not provide an object for candidate normalization."),
						TEXT("/target"), MakeShared<FJsonValueString>(Target), MakeShared<FJsonValueString>(TEXT("resolved AI asset")), Target, TEXT("candidate_normalization"));
					return Fail(OutError, MoveTemp(Diagnostic));
				}
				if (!NormalizeCandidates(
						RootType, *TargetObject, Native, OutError))
				{
					return false;
				}
				if (!SubmitAiCommand(
						CommandId, Native, MutationPayload, OutError, Document, TEXT("/root/operation/params")))
				{
					return false;
				}
			}

			TSharedPtr<FJsonObject> Mutation;
			FDomainHjsonImportLifecycleReceipt LifecycleReceipt;
			if (!ParseLifecycleMutation(
					Target, MutationPayload, Mutation, OutError))
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
			if (!TryValidateReadbackDiagnostics(AfterSnapshot, Target,
				TEXT("ai_behavior_domain_compile_not_settled"), OutError))
				return false;
			LifecycleReceipt.bMutationApplied = true;
			LifecycleReceipt.bNativeUpdateSettled = true;
			LifecycleReceipt.bPackageSaved = true;
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

		bool TryExport(
			const FString& Target,
			const TSharedPtr<FJsonObject>& Params,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			FString WorkspacePath;
			if (!Params->TryGetStringField(TEXT("workspace_path"), WorkspacePath)
				|| WorkspacePath.IsEmpty())
			{
				return Fail(OutError, ValueFailure(TEXT("ai_behavior_domain_export_path_required"), TEXT("Export requires a nonempty workspace path."),
					TEXT("/workspace_path"), Params->TryGetField(TEXT("workspace_path")), MakeShared<FJsonValueString>(TEXT("nonempty string")), Target));
			}
			UObject* Asset = nullptr;
			DomainAdapters::ContentGraphRead::FUeAgentContentGraphCapture Capture;
			if (!TryCaptureObject(
					Target,
					TArray<FString>{BehaviorTreeType, BlackboardType, StateTreeType, EqsType, SmartObjectType},
					Asset,
					Capture,
					OutError))
			{
				return false;
			}
			const FString ActualRootType = Asset
				? Asset->GetClass()->GetPathName() : FString();
			const UeAgentReflectionAuthoring::FProfile* Profile =
				ReflectionProfileForRootType(ActualRootType);
			if (!Profile)
			{
				return Fail(OutError, NativeFailure(TEXT("ai_behavior_domain_export_target_type_invalid"), TEXT("The resolved asset has no supported AI workspace export profile."),
					TEXT("/target"), MakeShared<FJsonValueString>(ActualRootType), StringValues({BehaviorTreeType, BlackboardType, EqsType, StateTreeType, SmartObjectType}), Target, TEXT("target_binding")));
			}
			if (!EnsureReflectionAuthoringRegistered(OutError)) return false;
			UeAgentExportWorkspaceWriter::FWorkspaceResult ExportResult;
			FString ExportError;
			if (!UeAgentReflectionAuthoring::ExportAssetWorkspace(*Profile, *Asset, WorkspacePath, TEXT("llm_authoring"), ExportResult))
			{
				OutError.SetFailure(ExportResult.Diagnostic, Target);
				return false;
			}
			const TSharedPtr<FJsonObject> Result = ExportResult.ToPublicJsonObject();
			return TryCanonicalizeObject(Result, OutPayload, OutError);
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
		UE::UAI::Contract::FDiagnostic JsonError;
		if (!Json::TryParseObject(CanonicalParamsJson, Params, JsonError)
			|| !Params.IsValid())
		{
			return Fail(OutError, JsonError, FString(), TEXT("/"));
		}
		FString Target;
		if (!Params->TryGetStringField(TEXT("target"), Target)
			|| ObjectPathFromPackageIdentity(Target)
				!= ObjectPathFromPackageIdentity(TargetIdentity))
		{
			return Fail(OutError, ValueFailure(TEXT("ai_behavior_domain_target_identity_mismatch"), TEXT("The target must be a string reference matching the bound asset."),
				TEXT("/target"), Params->TryGetField(TEXT("target")), MakeShared<FJsonValueString>(TargetIdentity), TargetIdentity));
		}
		if (!FindRootSpec(RootTypeIdentity))
		{
			return Fail(OutError, NativeFailure(TEXT("ai_behavior_domain_root_type_invalid"), TEXT("The bound root type is not supported by AI authoring."),
				TEXT("/root_type"), MakeShared<FJsonValueString>(RootTypeIdentity), StringValues({BehaviorTreeType, BlackboardType, EqsType, StateTreeType, SmartObjectType}), Target, TEXT("target_binding")));
		}
		if (CommandId == TEXT("content_graph.import_workspace"))
		{
			return TryImport(
				Target, RootTypeIdentity, CanonicalParamsJson,
				OutPayloadJson, OutError);
		}
		if (CommandId == TEXT("content_graph.export_workspace"))
		{
			bOutReadOnly = true;
			return TryExport(Target, Params, OutPayloadJson, OutError);
		}
		if (CommandId.StartsWith(TEXT("content_graph.")))
		{
			FString Revision;
			bOutReadOnly = true;
			return TryRead(
				Target, RootTypeIdentity, OutPayloadJson, Revision,
				nullptr, nullptr, OutError);
		}
		return Fail(OutError, ValueFailure(TEXT("ai_behavior_domain_command_not_supported"), TEXT("The AI authoring provider accepts content_graph commands."),
			TEXT("/command_id"), MakeShared<FJsonValueString>(CommandId), MakeShared<FJsonValueString>(TEXT("content_graph.*")), Target));
	}
}
