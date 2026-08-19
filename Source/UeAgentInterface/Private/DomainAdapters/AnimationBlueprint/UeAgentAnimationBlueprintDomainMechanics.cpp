#include "DomainAdapters/AnimationBlueprint/UeAgentAnimationBlueprintDomainMechanics.h"

#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimGraphNode_Root.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "AnimationEditorUtils.h"
#include "AnimationGraph.h"
#include "AnimationGraphSchema.h"
#include "AnimationStateMachineGraph.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringApplyProvider.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringDocuments.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringRegistration.h"
#include "Domains/Animation/UeAgentAnimBlueprintDomainBackend.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintMutationModel.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringRegistration.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/PackageName.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "Utilities/AuthoringOperationApply/UeAgentApplyInputContract.h"

namespace UE::UAI::DomainAdapters::AnimationBlueprint
{
	using namespace CommandContract;
	using namespace ContextResource;
	using namespace HjsonWorkspace;

	namespace
	{
		const FString SchemaId = TEXT("uai.animation_blueprint.edit/1");
		const FString RootType = TEXT("/Script/Engine.AnimBlueprint");

		bool Fail(
			FWorkspaceError& OutError,
			FString Code,
			FString Source = FString(),
			FString Pointer = TEXT("/"))
		{
			OutError.Set(MoveTemp(Code), MoveTemp(Source), MoveTemp(Pointer));
			return false;
		}

		FString PackagePath(FString Target)
		{
			Target.TrimStartAndEndInline();
			return Target.Contains(TEXT("."))
				? FPackageName::ObjectPathToPackageName(Target)
				: Target;
		}

		FString ObjectPath(const FString& Target)
		{
			if (Target.Contains(TEXT("."))) return Target;
			const FString Name = FPackageName::GetLongPackageAssetName(Target);
			return Name.IsEmpty() ? Target : Target + TEXT(".") + Name;
		}

		FUeAgentResourceTracker MakeResources()
		{
			return FUeAgentResourceTracker(FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::OutputBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::StringBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::Nodes, 65536},
				{EUeAgentResourceDimension::Items, 262144},
				{EUeAgentResourceDimension::Files, 4096},
				{EUeAgentResourceDimension::ReferenceEdges, 262144},
				{EUeAgentResourceDimension::WorkUnits, 1048576},
				{EUeAgentResourceDimension::EstimatedMemoryBytes,
					512 * 1024 * 1024},
			}));
		}

		TSharedPtr<FJsonValue> JsonValueFromHjson(
			const FHjsonNode& Node,
			FWorkspaceError& OutError)
		{
			switch (Node.GetKind())
			{
			case EHjsonNodeKind::Null:
				return MakeShared<FJsonValueNull>();
			case EHjsonNodeKind::Boolean:
				return MakeShared<FJsonValueBoolean>(Node.GetBoolean());
			case EHjsonNodeKind::String:
				return MakeShared<FJsonValueString>(Node.GetText());
			case EHjsonNodeKind::Integer:
			case EHjsonNodeKind::Decimal:
				return MakeShared<FJsonValueNumber>(FCString::Atod(*Node.GetLexeme()));
			case EHjsonNodeKind::Array:
			{
				TArray<TSharedPtr<FJsonValue>> Items;
				for (const FHjsonNodeRef& Item : Node.GetItems())
				{
					TSharedPtr<FJsonValue> Converted =
						JsonValueFromHjson(*Item, OutError);
					if (!Converted.IsValid()) return nullptr;
					Items.Add(MoveTemp(Converted));
				}
				return MakeShared<FJsonValueArray>(MoveTemp(Items));
			}
			case EHjsonNodeKind::Object:
			{
				const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
				for (const TPair<FString, FHjsonNodeRef>& Entry : Node.GetEntries())
				{
					TSharedPtr<FJsonValue> Converted =
						JsonValueFromHjson(*Entry.Value, OutError);
					if (!Converted.IsValid()) return nullptr;
					Object->SetField(Entry.Key, MoveTemp(Converted));
				}
				return MakeShared<FJsonValueObject>(Object);
			}
			default:
				Fail(OutError, TEXT("animation_blueprint_hjson_node_invalid"));
				return nullptr;
			}
		}

		bool CanonicalObject(
			const TSharedPtr<FJsonObject>& Object,
			FString& Out,
			FWorkspaceError& OutError)
		{
			FString Error;
			if (!Object.IsValid() || !Json::TryCanonicalize(Object, Out, Error))
			{
				return Fail(OutError,
					TEXT("animation_blueprint_json_invalid"), Error);
			}
			return true;
		}

		bool EnsureAuthoringRegistered(FWorkspaceError& OutError)
		{
			static const TPair<bool, FString> Registration = []()
			{
				FString Error;
				const bool bBlueprint =
					UeAgentBlueprintAuthoring::RegisterAuthoring(Error);
				const bool bAnimation = bBlueprint
					&& UeAgentAnimationAuthoring::RegisterAuthoring(Error);
				return TPair<bool, FString>(bAnimation, MoveTemp(Error));
			}();
			if (!Registration.Key)
			{
				return Fail(OutError,
					TEXT("animation_blueprint_authoring_registration_failed"),
					Registration.Value);
			}
			return true;
		}

		UAnimBlueprint* LoadAnimationBlueprint(const FString& Target)
		{
			return Cast<UAnimBlueprint>(StaticLoadObject(
				UAnimBlueprint::StaticClass(), nullptr, *ObjectPath(Target)));
		}

		TSharedPtr<FJsonObject> DetailedStateMachine(
			const UAnimGraphNode_StateMachineBase& Node)
		{
			const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
			UAnimationStateMachineGraph* Machine = Node.EditorStateMachineGraph;
			Value->SetStringField(TEXT("name"),
				Machine ? Machine->GetName()
					: const_cast<UAnimGraphNode_StateMachineBase&>(Node)
						.GetStateMachineName());
			Value->SetStringField(TEXT("node_guid"),
				UeAgentAnimBlueprintOps::NodeGuidToString(&Node));
			Value->SetStringField(TEXT("owner_graph"),
				Node.GetGraph() ? Node.GetGraph()->GetName() : FString());
			TArray<TSharedPtr<FJsonValue>> States;
			TArray<TSharedPtr<FJsonValue>> Transitions;
			if (Machine)
			{
				TArray<UAnimStateNode*> StateNodes;
				Machine->GetNodesOfClass<UAnimStateNode>(StateNodes);
				StateNodes.Sort([](const UAnimStateNode& A,
					const UAnimStateNode& B)
				{
					return A.GetStateName() < B.GetStateName();
				});
				for (const UAnimStateNode* State : StateNodes)
				{
					States.Add(MakeShared<FJsonValueObject>(
						UeAgentAnimBlueprintOps::BuildStateNodeJson(State)));
				}
				TArray<UAnimStateTransitionNode*> TransitionNodes;
				Machine->GetNodesOfClass<UAnimStateTransitionNode>(TransitionNodes);
				TransitionNodes.Sort([](const UAnimStateTransitionNode& A,
					const UAnimStateTransitionNode& B)
				{
					return UeAgentAnimBlueprintOps::NodeGuidToString(&A)
						< UeAgentAnimBlueprintOps::NodeGuidToString(&B);
				});
				for (const UAnimStateTransitionNode* Transition : TransitionNodes)
				{
					Transitions.Add(MakeShared<FJsonValueObject>(
						UeAgentAnimBlueprintOps::BuildTransitionJson(Transition)));
				}
				const UAnimStateNodeBase* Entry =
					UeAgentAnimBlueprintOps::GetEntryConnectedState(Machine);
				Value->SetStringField(TEXT("entry_state"),
					Entry ? Entry->GetStateName() : FString());
			}
			Value->SetArrayField(TEXT("states"), MoveTemp(States));
			Value->SetArrayField(TEXT("transitions"), MoveTemp(Transitions));
			return Value;
		}

		bool TryCapture(
			const FString& Target,
			TSharedPtr<FJsonObject>& OutCapture,
			FString& OutRevision,
			FWorkspaceError& OutError)
		{
			if (!EnsureAuthoringRegistered(OutError)) return false;
			UAnimBlueprint* Blueprint = LoadAnimationBlueprint(Target);
			if (!Blueprint)
			{
				return Fail(OutError,
					TEXT("animation_blueprint_target_unresolved"), Target);
			}
			TSharedPtr<FJsonObject> Document;
			FString Error;
			if (!UeAgentAnimationAuthoring::BuildDocument(
				UeAgentAnimationAuthoring::EAssetKind::AnimBlueprint,
				*Blueprint, Document, Error, PackagePath(Target))
				|| !Document.IsValid())
			{
				return Fail(OutError,
					TEXT("animation_blueprint_readback_failed"), Target, Error);
			}

			TArray<TSharedPtr<FJsonValue>> Graphs;
			for (UEdGraph* Graph : UeAgentAnimBlueprintOps::GatherAnimGraphs(Blueprint))
			{
				if (!Graph) continue;
				const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
				Value->SetStringField(TEXT("name"), Graph->GetName());
				Value->SetStringField(TEXT("guid"),
					Graph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
				Graphs.Add(MakeShared<FJsonValueObject>(Value));
			}
			Document->SetArrayField(TEXT("animation_graphs"), MoveTemp(Graphs));

			TArray<TSharedPtr<FJsonValue>> StateMachines;
			for (UEdGraph* Graph : UeAgentAnimBlueprintOps::GatherAnimGraphs(Blueprint))
			{
				if (!Graph) continue;
				TArray<UAnimGraphNode_StateMachineBase*> Nodes;
				Graph->GetNodesOfClass<UAnimGraphNode_StateMachineBase>(Nodes);
				for (const UAnimGraphNode_StateMachineBase* Node : Nodes)
				{
					if (Node)
					{
						StateMachines.Add(MakeShared<FJsonValueObject>(
							DetailedStateMachine(*Node)));
					}
				}
			}
			Document->SetArrayField(TEXT("state_machines"),
				MoveTemp(StateMachines));
			Document->SetStringField(TEXT("target"), Target);
			Document->SetStringField(TEXT("root_type"), RootType);
			Document->SetStringField(TEXT("compile_status"),
				Blueprint->Status == BS_Error ? TEXT("error") : TEXT("valid"));
			Document->SetStringField(TEXT("generated_class"),
				Blueprint->GeneratedClass
					? Blueprint->GeneratedClass->GetPathName() : FString());
			Document->SetBoolField(TEXT("complete"), true);

			FString Canonical;
			if (!CanonicalObject(Document, Canonical, OutError)) return false;
			OutRevision = TEXT("animation-blueprint:")
				+ FMD5::HashAnsiString(*Canonical);
			Document->SetStringField(TEXT("revision"), OutRevision);
			OutCapture = MoveTemp(Document);
			return true;
		}

		bool TryRead(
			const FString& Target,
			FString& OutPayload,
			FString& OutRevision,
			FWorkspaceError& OutError)
		{
			TSharedPtr<FJsonObject> Capture;
			if (!TryCapture(Target, Capture, OutRevision, OutError)) return false;
			return CanonicalObject(Capture, OutPayload, OutError);
		}

		bool ParseDocument(
			const TSharedPtr<FJsonObject>& Params,
			TSharedPtr<FJsonObject>& OutDocument,
			FWorkspaceError& OutError)
		{
			OutDocument.Reset();
			if (!Params.IsValid()
				|| Params->HasField(TEXT("carrier"))
				|| Params->HasField(TEXT("hjson_document")))
			{
				return Fail(OutError,
					TEXT("animation_blueprint_legacy_source_contract_rejected"));
			}
			FString Target;
			const TSharedPtr<FJsonObject>* Source = nullptr;
			FString Kind;
			FString TargetIdentity;
			FString EntryFile;
			const TArray<TSharedPtr<FJsonValue>>* Documents = nullptr;
			if (!Params->TryGetStringField(TEXT("target"), Target)
				|| Target.IsEmpty()
				|| !Params->TryGetObjectField(TEXT("source"), Source)
				|| !Source || !Source->IsValid()
				|| !(*Source)->TryGetStringField(TEXT("kind"), Kind)
				|| Kind != TEXT("resolved")
				|| !(*Source)->TryGetStringField(
					TEXT("target_identity"), TargetIdentity)
				|| TargetIdentity != Target
				|| !(*Source)->TryGetStringField(TEXT("entry_file"), EntryFile)
				|| EntryFile.IsEmpty()
				|| !(*Source)->TryGetArrayField(TEXT("documents"), Documents)
				|| !Documents || Documents->IsEmpty())
			{
				return Fail(OutError,
					TEXT("animation_blueprint_resolved_source_invalid"));
			}
			FString Text;
			int32 EntryMatches = 0;
			for (const TSharedPtr<FJsonValue>& Value : *Documents)
			{
				const TSharedPtr<FJsonObject> DocumentObject = Value.IsValid()
					? Value->AsObject() : nullptr;
				FString RelativePath;
				FString DocumentText;
				if (!DocumentObject.IsValid()
					|| !DocumentObject->TryGetStringField(
						TEXT("path"), RelativePath)
					|| !DocumentObject->TryGetStringField(
						TEXT("document"), DocumentText))
				{
					return Fail(OutError,
						TEXT("animation_blueprint_resolved_source_invalid"));
				}
				if (RelativePath == EntryFile)
				{
					Text = MoveTemp(DocumentText);
					++EntryMatches;
				}
			}
			if (EntryMatches != 1 || Text.IsEmpty())
			{
				return Fail(OutError,
					TEXT("animation_blueprint_resolved_entry_invalid"));
			}
			FUeAgentResourceTracker Resources = MakeResources();
			FHjsonDocumentPtr Parsed;
			if (!TryParseHjsonDocument(
				Text, TEXT("inline:animation-blueprint-domain"),
				Resources, Parsed, OutError)
				|| !Parsed.IsValid())
			{
				return false;
			}
			TSharedPtr<FJsonValue> Converted =
				JsonValueFromHjson(*Parsed->GetRoot(), OutError);
			const TSharedPtr<FJsonObject> Root =
				Converted.IsValid() ? Converted->AsObject() : nullptr;
			FString Schema;
			const TSharedPtr<FJsonObject>* DomainRoot = nullptr;
			const TSharedPtr<FJsonObject>* Document = nullptr;
			FString Type;
			if (!Root.IsValid()
				|| !Root->TryGetStringField(TEXT("schema"), Schema)
				|| Schema != SchemaId
				|| !Root->TryGetObjectField(TEXT("root"), DomainRoot)
				|| !DomainRoot || !DomainRoot->IsValid()
				|| !(*DomainRoot)->TryGetStringField(TEXT("type"), Type)
				|| Type != RootType
				|| !(*DomainRoot)->TryGetObjectField(TEXT("document"), Document)
				|| !Document || !Document->IsValid())
			{
				return Fail(OutError,
					TEXT("animation_blueprint_schema_or_type_invalid"), Type);
			}
			OutDocument = *Document;
			return true;
		}

		bool ArrayObjects(
			const TSharedPtr<FJsonObject>& Owner,
			const FString& Field,
			TArray<TSharedPtr<FJsonObject>>& Out,
			FWorkspaceError& OutError)
		{
			Out.Reset();
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Owner.IsValid() || !Owner->TryGetArrayField(Field, Values))
			{
				return true;
			}
			if (!Values) return true;
			for (int32 Index = 0; Index < Values->Num(); ++Index)
			{
				const TSharedPtr<FJsonValue>& Value = (*Values)[Index];
				if (!Value.IsValid() || Value->Type != EJson::Object)
				{
					return Fail(OutError,
						TEXT("animation_blueprint_structure_item_invalid"),
						Field, FString::FromInt(Index));
				}
				Out.Add(Value->AsObject());
			}
			return true;
		}

		bool RequiredName(
			const TSharedPtr<FJsonObject>& Object,
			const FString& Field,
			FString& OutName,
			FWorkspaceError& OutError)
		{
			OutName.Reset();
			if (!Object.IsValid()
				|| !Object->TryGetStringField(Field, OutName)
				|| OutName.TrimStartAndEnd().IsEmpty())
			{
				return Fail(OutError,
					TEXT("animation_blueprint_structure_name_required"), Field);
			}
			OutName.TrimStartAndEndInline();
			return true;
		}

		bool ApplyAnimationSurfaces(
			UAnimBlueprint& Blueprint,
			const TSharedPtr<FJsonObject>& Extension,
			FWorkspaceError& OutError,
			int32& OutGraphs,
			int32& OutMachines,
			int32& OutStates,
			int32& OutTransitions)
		{
			OutGraphs = OutMachines = OutStates = OutTransitions = 0;
			TArray<TSharedPtr<FJsonObject>> GraphSpecs;
			if (!ArrayObjects(Extension, TEXT("animation_graphs"),
				GraphSpecs, OutError)) return false;
			for (const TSharedPtr<FJsonObject>& Spec : GraphSpecs)
			{
				FString Name;
				if (!RequiredName(Spec, TEXT("name"), Name, OutError)) return false;
				if (UeAgentAnimBlueprintOps::ResolveAnyAnimBlueprintGraph(
					&Blueprint, Name))
				{
					continue;
				}
				UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(
					&Blueprint, FName(*Name), UAnimationGraph::StaticClass(),
					UAnimationGraphSchema::StaticClass());
				if (!Graph)
				{
					return Fail(OutError,
						TEXT("animation_blueprint_graph_create_failed"), Name);
				}
				FBlueprintEditorUtils::AddDomainSpecificGraph(&Blueprint, Graph);
				TArray<UAnimGraphNode_Root*> Roots;
				Graph->GetNodesOfClass<UAnimGraphNode_Root>(Roots);
				if (Roots.IsEmpty() && Graph->GetSchema())
				{
					Graph->GetSchema()->CreateDefaultNodesForGraph(*Graph);
				}
				++OutGraphs;
			}

			TArray<TSharedPtr<FJsonObject>> MachineSpecs;
			if (!ArrayObjects(Extension, TEXT("state_machines"),
				MachineSpecs, OutError)) return false;
			for (const TSharedPtr<FJsonObject>& MachineSpec : MachineSpecs)
			{
				FString MachineName;
				if (!RequiredName(
					MachineSpec, TEXT("name"), MachineName, OutError)) return false;
				FString GraphName = TEXT("AnimGraph");
				MachineSpec->TryGetStringField(TEXT("animation_graph"), GraphName);
				UEdGraph* AnimGraph =
					UeAgentAnimBlueprintOps::ResolveAnyAnimBlueprintGraph(
						&Blueprint, GraphName);
				if (!AnimGraph || !AnimationEditorUtils::IsAnimGraph(AnimGraph))
				{
					return Fail(OutError,
						TEXT("animation_blueprint_anim_graph_unresolved"),
						GraphName);
				}
				UAnimGraphNode_StateMachineBase* MachineNode =
					UeAgentAnimBlueprintOps::FindStateMachineNode(
						&Blueprint, MachineName);
				if (!MachineNode)
				{
					double X = 0.0;
					double Y = 0.0;
					MachineSpec->TryGetNumberField(TEXT("pos_x"), X);
					MachineSpec->TryGetNumberField(TEXT("pos_y"), Y);
					UAnimGraphNode_StateMachine* Created =
						UeAgentAnimBlueprintOps::AddNodeToGraph<
							UAnimGraphNode_StateMachine>(
							AnimGraph, static_cast<int32>(X), static_cast<int32>(Y));
					if (!Created || !Created->EditorStateMachineGraph)
					{
						return Fail(OutError,
							TEXT("animation_blueprint_state_machine_create_failed"),
							MachineName);
					}
					FBlueprintEditorUtils::RenameGraph(
						Created->EditorStateMachineGraph, MachineName);
					MachineNode = Created;
					++OutMachines;
				}
				UAnimationStateMachineGraph* Machine =
					MachineNode->EditorStateMachineGraph;
				if (!Machine)
				{
					return Fail(OutError,
						TEXT("animation_blueprint_state_machine_graph_missing"),
						MachineName);
				}

				TArray<TSharedPtr<FJsonObject>> StateSpecs;
				if (!ArrayObjects(
					MachineSpec, TEXT("states"), StateSpecs, OutError)) return false;
				for (const TSharedPtr<FJsonObject>& StateSpec : StateSpecs)
				{
					FString StateName;
					if (!RequiredName(
						StateSpec, TEXT("name"), StateName, OutError)) return false;
					if (UeAgentAnimBlueprintOps::FindStateNodeByName(
						Machine, StateName))
					{
						continue;
					}
					double X = 0.0;
					double Y = 0.0;
					StateSpec->TryGetNumberField(TEXT("pos_x"), X);
					StateSpec->TryGetNumberField(TEXT("pos_y"), Y);
					UAnimStateNode* State =
						UeAgentAnimBlueprintOps::AddNodeToGraph<UAnimStateNode>(
							Machine, static_cast<int32>(X), static_cast<int32>(Y));
					if (!State || !State->GetBoundGraph())
					{
						return Fail(OutError,
							TEXT("animation_blueprint_state_create_failed"),
							StateName);
					}
					FBlueprintEditorUtils::RenameGraph(
						State->GetBoundGraph(), StateName);
					StateSpec->TryGetBoolField(
						TEXT("always_reset_on_entry"),
						State->bAlwaysResetOnEntry);
					++OutStates;
				}

				FString EntryState;
				if (MachineSpec->TryGetStringField(
					TEXT("entry_state"), EntryState))
				{
					UAnimStateNodeBase* Entry =
						UeAgentAnimBlueprintOps::FindStateNodeByName(
							Machine, EntryState);
					FString Error;
					if (!UeAgentAnimBlueprintOps::SetEntryState(
						Machine, Entry, Error))
					{
						return Fail(OutError,
							TEXT("animation_blueprint_entry_state_failed"),
							EntryState, Error);
					}
				}

				TArray<TSharedPtr<FJsonObject>> TransitionSpecs;
				if (!ArrayObjects(MachineSpec, TEXT("transitions"),
					TransitionSpecs, OutError)) return false;
				for (const TSharedPtr<FJsonObject>& TransitionSpec : TransitionSpecs)
				{
					FString Source;
					FString Target;
					if (!RequiredName(TransitionSpec,
						TEXT("source"), Source, OutError)
						|| !RequiredName(TransitionSpec,
							TEXT("target"), Target, OutError))
					{
						return false;
					}
					if (UeAgentAnimBlueprintOps::FindTransitionNode(
						Machine, Source, Target))
					{
						continue;
					}
					UAnimStateNodeBase* SourceState =
						UeAgentAnimBlueprintOps::FindStateNodeByName(Machine, Source);
					UAnimStateNodeBase* TargetState =
						UeAgentAnimBlueprintOps::FindStateNodeByName(Machine, Target);
					if (!SourceState || !TargetState)
					{
						return Fail(OutError,
							TEXT("animation_blueprint_transition_endpoint_unresolved"),
							Source + TEXT("->") + Target);
					}
					UAnimStateTransitionNode* Transition =
						UeAgentAnimBlueprintOps::AddNodeToGraph<
							UAnimStateTransitionNode>(Machine, 0, 0);
					if (!Transition)
					{
						return Fail(OutError,
							TEXT("animation_blueprint_transition_create_failed"),
							Source + TEXT("->") + Target);
					}
					Transition->CreateConnections(SourceState, TargetState);
					TransitionSpec->TryGetBoolField(
						TEXT("bidirectional"), Transition->Bidirectional);
					double Crossfade = Transition->CrossfadeDuration;
					if (TransitionSpec->TryGetNumberField(
						TEXT("crossfade_duration"), Crossfade))
					{
						Transition->CrossfadeDuration =
							static_cast<float>(FMath::Max(0.0, Crossfade));
					}
					++OutTransitions;
				}
			}
			return true;
		}

		void RetireScratch(TStrongObjectPtr<UAnimBlueprint>& Scratch)
		{
			if (!Scratch.IsValid()) return;
			Scratch->ClearFlags(RF_Public | RF_Standalone);
			Scratch->MarkAsGarbage();
			Scratch.Reset();
		}

		bool DeleteCreated(UAnimBlueprint* Blueprint)
		{
			if (!Blueprint) return true;
			UPackage* const Package = Blueprint->GetOutermost();
			const FSoftObjectPath OriginalObjectPath(Blueprint);
			FAssetRegistryModule::AssetDeleted(Blueprint);
			TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);
			if (!ObjectTools::DeleteSingleObject(Blueprint, true))
			{
				FAssetRegistryModule::AssetCreated(Blueprint);
				return false;
			}
			if (Package)
			{
				Package->SetDirtyFlag(false);
			}
			const FAssetRegistryModule& AssetRegistry =
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
					TEXT("AssetRegistry"));
			return !AssetRegistry.Get().GetAssetByObjectPath(
				OriginalObjectPath).IsValid();
		}

		bool TryImport(
			const FString& Target,
			const TSharedPtr<FJsonObject>& Params,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			TSharedPtr<FJsonObject> Document;
			if (!ParseDocument(Params, Document, OutError)) return false;
			FString ExpectedRevision;
			if (!Params->TryGetStringField(
				TEXT("expected_revision"), ExpectedRevision)
				|| ExpectedRevision.IsEmpty())
			{
				return Fail(OutError,
					TEXT("animation_blueprint_expected_revision_required"));
			}
			FString Operation;
			const TSharedPtr<FJsonObject>* DocumentTarget = nullptr;
			FString AssetPath;
			if (!Document->TryGetStringField(TEXT("operation"), Operation)
				|| !Document->TryGetObjectField(TEXT("target"), DocumentTarget)
				|| !DocumentTarget || !DocumentTarget->IsValid()
				|| !(*DocumentTarget)->TryGetStringField(
					TEXT("asset_path"), AssetPath))
			{
				return Fail(OutError,
					TEXT("animation_blueprint_document_identity_required"));
			}
			Operation.TrimStartAndEndInline();
			Operation.ToLowerInline();
			if (Operation != TEXT("create"))
			{
				return Fail(OutError,
					TEXT("animation_blueprint_operation_unsupported"), Operation);
			}
			const FString TargetPackage = PackagePath(Target);
			if (PackagePath(AssetPath) != TargetPackage)
			{
				return Fail(OutError,
					TEXT("animation_blueprint_target_identity_mismatch"),
					AssetPath, Target);
			}
			UObject* Existing = StaticLoadObject(
				UObject::StaticClass(), nullptr, *ObjectPath(Target));
			if (Existing)
			{
				return Fail(OutError,
					Cast<UAnimBlueprint>(Existing)
						? TEXT("animation_blueprint_target_already_exists")
						: TEXT("animation_blueprint_target_type_unsupported"),
					Existing->GetClass()->GetPathName());
			}
			if (ExpectedRevision != TEXT("missing"))
			{
				return Fail(OutError,
					TEXT("animation_blueprint_stale_revision"),
					Target, TEXT("missing"));
			}

			const TSharedPtr<FJsonObject>* ExtensionPtr = nullptr;
			if (!Document->TryGetObjectField(
				TEXT("provider_extension"), ExtensionPtr)
				|| !ExtensionPtr || !ExtensionPtr->IsValid())
			{
				return Fail(OutError,
					TEXT("animation_blueprint_provider_extension_required"));
			}
			const TSharedPtr<FJsonObject> Extension = *ExtensionPtr;
			const TSharedPtr<FJsonObject>* SettingsPtr = nullptr;
			TSharedPtr<FJsonObject> Settings = MakeShared<FJsonObject>();
			if (Extension->TryGetObjectField(
				TEXT("class_settings"), SettingsPtr)
				&& SettingsPtr && SettingsPtr->IsValid())
			{
				Settings = *SettingsPtr;
			}
			const TSharedPtr<FJsonObject>* BlueprintSurfacesPtr = nullptr;
			TSharedPtr<FJsonObject> BlueprintSurfaces = MakeShared<FJsonObject>();
			if (Extension->TryGetObjectField(
				TEXT("blueprint_surfaces"), BlueprintSurfacesPtr)
				&& BlueprintSurfacesPtr && BlueprintSurfacesPtr->IsValid())
			{
				BlueprintSurfaces = *BlueprintSurfacesPtr;
			}

			if (!EnsureAuthoringRegistered(OutError)) return false;
			FString NativeError;
			UObject* ScratchObject =
				UeAgentAnimationAuthoring::CreateFactorySeed(
					UeAgentAnimationAuthoring::EAssetKind::AnimBlueprint,
					GetTransientPackage(),
					*FString(TEXT("ABP_T16_Preflight_")
						+ FGuid::NewGuid().ToString(EGuidFormats::Digits)),
					RF_Transient,
					Settings,
					NativeError);
			TStrongObjectPtr<UAnimBlueprint> Scratch(
				Cast<UAnimBlueprint>(ScratchObject));
			if (!Scratch.IsValid())
			{
				return Fail(OutError,
					TEXT("animation_blueprint_preflight_factory_failed"),
					Target, NativeError);
			}
			TUniquePtr<UeAgentBlueprintAuthoring::FPreparedBlueprintMutation>
				ScratchMutation;
			UeAgentBlueprintAuthoring::FMutationCounters ScratchExpected;
			if (!UeAgentBlueprintAuthoring::FPreparedBlueprintMutation::Prepare(
				*Scratch, BlueprintSurfaces, ScratchMutation,
				ScratchExpected, NativeError))
			{
				RetireScratch(Scratch);
				return Fail(OutError,
					TEXT("animation_blueprint_common_surface_preflight_failed"),
					Target, NativeError);
			}
			int32 ScratchGraphs = 0;
			int32 ScratchMachines = 0;
			int32 ScratchStates = 0;
			int32 ScratchTransitions = 0;
			if (!ApplyAnimationSurfaces(
				*Scratch, Extension, OutError,
				ScratchGraphs, ScratchMachines,
				ScratchStates, ScratchTransitions)
				|| !UeAgentAnimBlueprintOps::FinalizeAnimBlueprintStructureMutation(
					Scratch.Get(), true, false, NativeError)
				|| Scratch->Status == BS_Error)
			{
				RetireScratch(Scratch);
				if (OutError.GetCode().IsEmpty())
				{
					Fail(OutError,
						TEXT("animation_blueprint_topology_preflight_failed"),
						Target, NativeError);
				}
				return false;
			}
			RetireScratch(Scratch);

			UeAgentAnimationAuthoring::FAssetApplyRequest ApplyRequest;
			ApplyRequest.Kind =
				UeAgentAnimationAuthoring::EAssetKind::AnimBlueprint;
			ApplyRequest.Operation =
				UeAgentApplyInputContract::EOperation::Create;
			ApplyRequest.AssetPath = TargetPackage;
			ApplyRequest.Document = MakeShared<FJsonObject>();
			ApplyRequest.Document->SetObjectField(TEXT("settings"), Settings);
			ApplyRequest.bCompile = false;
			ApplyRequest.bSave = false;
			UeAgentAnimationAuthoring::FAssetApplyResult ApplyResult;
			const FString RequestId = TEXT("animation-blueprint-")
				+ FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
			if (!UeAgentAnimationAuthoring::ApplyAsset(
				ApplyRequest, ApplyResult, RequestId))
			{
				return Fail(OutError,
					ApplyResult.ErrorCode.IsEmpty()
						? FString(TEXT("animation_blueprint_apply_failed"))
						: ApplyResult.ErrorCode,
					ApplyResult.Phase, ApplyResult.ErrorDetail);
			}

			UAnimBlueprint* Applied = Cast<UAnimBlueprint>(ApplyResult.Asset);
			auto Rollback = [&Applied, &ApplyResult]()
			{
				ApplyResult.Asset = nullptr;
				const bool bDeleted = DeleteCreated(Applied);
				Applied = nullptr;
				return bDeleted;
			};
			if (!Applied)
			{
				Rollback();
				return Fail(OutError,
					TEXT("animation_blueprint_persisted_asset_missing"), Target);
			}

			TUniquePtr<UeAgentBlueprintAuthoring::FPreparedBlueprintMutation>
				BlueprintMutation;
			UeAgentBlueprintAuthoring::FMutationCounters ExpectedCounters;
			if (!UeAgentBlueprintAuthoring::FPreparedBlueprintMutation::Prepare(
				*Applied, BlueprintSurfaces, BlueprintMutation,
				ExpectedCounters, NativeError))
			{
				const bool bRollback = Rollback();
				return Fail(OutError,
					bRollback
						? TEXT("animation_blueprint_common_surface_apply_failed")
						: TEXT("animation_blueprint_rollback_failed"),
					Target, NativeError);
			}
			int32 Graphs = 0;
			int32 Machines = 0;
			int32 States = 0;
			int32 Transitions = 0;
			if (!ApplyAnimationSurfaces(
				*Applied, Extension, OutError,
				Graphs, Machines, States, Transitions))
			{
				const bool bRollback = Rollback();
				if (!bRollback)
				{
					OutError.Set(TEXT("animation_blueprint_rollback_failed"), Target);
				}
				return false;
			}
			if (!UeAgentAnimBlueprintOps::FinalizeAnimBlueprintStructureMutation(
				Applied, true, false, NativeError))
			{
				const bool bRollback = Rollback();
				return Fail(OutError,
					bRollback
						? TEXT("animation_blueprint_compile_failed")
						: TEXT("animation_blueprint_rollback_failed"),
					Target, NativeError);
			}
			UeAgentBlueprintAuthoring::FMutationCounters AppliedCounters;
			if (!BlueprintMutation->FinalizeAfterCompile(
				*Applied, AppliedCounters, NativeError)
				|| !UeAgentAnimationAuthoring::CompileAsset(
					UeAgentAnimationAuthoring::EAssetKind::AnimBlueprint,
					*Applied, NativeError)
				|| !UeAgentAnimationAuthoring::SaveAsset(
					UeAgentAnimationAuthoring::EAssetKind::AnimBlueprint,
					*Applied, NativeError))
			{
				const bool bRollback = Rollback();
				return Fail(OutError,
					bRollback
						? TEXT("animation_blueprint_settle_or_save_failed")
						: TEXT("animation_blueprint_rollback_failed"),
					Target, NativeError);
			}

			UPackage* Package = Applied->GetOutermost();
			ApplyResult.Asset = nullptr;
			Applied = nullptr;
			FText UnloadError;
			if (!Package
				|| !UPackageTools::UnloadPackages({Package}, UnloadError, true))
			{
				return Fail(OutError,
					TEXT("animation_blueprint_unload_failed"),
					Target, UnloadError.ToString());
			}
			UAnimBlueprint* Reloaded = LoadAnimationBlueprint(Target);
			if (!Reloaded || Reloaded->Status == BS_Error)
			{
				return Fail(OutError,
					TEXT("animation_blueprint_reload_failed"), Target);
			}
			FString ReadbackPayload;
			FString Revision;
			if (!TryRead(Target, ReadbackPayload, Revision, OutError)) return false;
			TSharedPtr<FJsonObject> Readback;
			FString JsonError;
			if (!Json::TryParseObject(
				ReadbackPayload, Readback, JsonError)
				|| !Readback.IsValid())
			{
				return Fail(OutError,
					TEXT("animation_blueprint_readback_payload_invalid"),
					Target, JsonError);
			}

			const TSharedPtr<FJsonObject> Mutation = MakeShared<FJsonObject>();
			Mutation->SetStringField(TEXT("operation"), TEXT("create"));
			Mutation->SetNumberField(TEXT("animation_graphs_added"), Graphs);
			Mutation->SetNumberField(TEXT("state_machines_added"), Machines);
			Mutation->SetNumberField(TEXT("states_added"), States);
			Mutation->SetNumberField(TEXT("transitions_added"), Transitions);
			Mutation->SetNumberField(TEXT("blueprint_variables_added"),
				AppliedCounters.VariablesAdded);
			Mutation->SetNumberField(TEXT("blueprint_components_added"),
				AppliedCounters.ComponentsAdded);
			Mutation->SetNumberField(TEXT("blueprint_graphs_added"),
				AppliedCounters.GraphsAdded);
			Mutation->SetNumberField(TEXT("blueprint_nodes_added"),
				AppliedCounters.NodesAdded);

			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("terminal_status"), TEXT("succeeded"));
			Result->SetStringField(TEXT("provider_id"),
				TEXT("animation-blueprint-domain-provider"));
			Result->SetStringField(TEXT("target"), Target);
			Result->SetStringField(TEXT("revision"), Revision);
			Result->SetBoolField(TEXT("saved"), true);
			Result->SetBoolField(TEXT("reloaded"), true);
			Result->SetObjectField(TEXT("independent_readback"), Readback);
			Result->SetObjectField(TEXT("mutation"), Mutation);
			return CanonicalObject(Result, OutPayload, OutError);
		}
	}

	bool TryExecuteContentGraph(
		const FString& CommandId,
		const FString& TargetIdentity,
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
			return Fail(OutError,
				TEXT("animation_blueprint_params_invalid"), JsonError);
		}
		FString Target;
		if (!Params->TryGetStringField(TEXT("target"), Target)
			|| ObjectPath(Target) != ObjectPath(TargetIdentity))
		{
			return Fail(OutError,
				TEXT("animation_blueprint_target_identity_mismatch"),
				TargetIdentity, Target);
		}
		if (CommandId == TEXT("content_graph.import_workspace"))
		{
			return TryImport(Target, Params, OutPayloadJson, OutError);
		}
		if (CommandId == TEXT("content_graph.export_workspace"))
		{
			return Fail(OutError,
				TEXT("animation_blueprint_export_unsupported"), Target);
		}
		if (CommandId.StartsWith(TEXT("content_graph.")))
		{
			FString Revision;
			bOutReadOnly = true;
			return TryRead(Target, OutPayloadJson, Revision, OutError);
		}
		return Fail(OutError,
			TEXT("animation_blueprint_command_not_supported"), CommandId);
	}
}
