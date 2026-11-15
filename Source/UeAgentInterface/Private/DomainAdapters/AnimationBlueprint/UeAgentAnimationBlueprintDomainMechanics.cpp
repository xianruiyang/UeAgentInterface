#include "DomainAdapters/AnimationBlueprint/UeAgentAnimationBlueprintDomainMechanics.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

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
#include "AnimationStateMachineSchema.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringApplyProvider.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringDocuments.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringExport.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringRegistration.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringState.h"
#include "Domains/Animation/UeAgentAnimBlueprintDomainBackend.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringState.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringDescriptors.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintMutationModel.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintSemanticProjection.h"
#include "DomainAdapters/Blueprint/Surfaces/UeAgentBlueprintCandidateBindingNormalizer.h"
#include "EdGraphSchema_K2.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringRegistration.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphSchema.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/PackageName.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "QueryOwners/AnimationBlueprint/UeAgentAnimationBlueprintQueryOwner.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "Utilities/AuthoringOperationApply/UeAgentApplyInputContract.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringJsonFields.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"

namespace UE::UAI::DomainAdapters::AnimationBlueprint
{
	using namespace CommandContract;
	using namespace HjsonWorkspace;
	using namespace HjsonWorkspace::DomainAuthoring;

	namespace
	{
		const FString SchemaId = TEXT("uai.animation_blueprint.edit/1");
		const FString RootType = TEXT("/Script/Engine.AnimBlueprint");

		using UE::UAI::Contract::FDiagnostic;

		FDiagnostic NativeFailure(const FString& Code, const FString& Reason,
			const FString& Target, const FString& Subject = FString())
		{
			auto Diagnostic = FDiagnostic::Failure(Code, Reason, TEXT("animation_blueprint"));
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.TargetPath = Target;
			Diagnostic.SubjectId = Subject;
			return Diagnostic;
		}

		FDiagnostic StructureFailure(const FString& Code, const FString& Reason,
			const UAnimBlueprint& Blueprint, const FString& Subject)
		{
			auto Diagnostic = NativeFailure(Code, Reason,
				UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), Subject);
			Diagnostic.InternalContext = Blueprint.GetPathName();
			return Diagnostic;
		}

		FDiagnostic InputFailure(const FString& Code, const FString& Reason,
			const FJsonObject* Object, const FString& Field, const FString& Expected,
			const FString& Pointer = FString())
		{
			auto Diagnostic = FDiagnostic::Failure(Code, Reason, TEXT("animation_blueprint"));
			Diagnostic.SourceKind = TEXT("related_input");
			Diagnostic.PropertyPath = Field;
			Diagnostic.Source.JsonPath = Pointer.IsEmpty()
				? TEXT("/") + EscapeJsonPointerSegment(Field) : Pointer;
			const auto Value = Object ? Object->TryGetField(Field) : nullptr;
			Diagnostic.ActualValuePresent = Value.IsValid();
			if (Value.IsValid()) Diagnostic.ActualValue = Value;
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			if (Object) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Diagnostic, *Object, Field);
			return Diagnostic;
		}

		void SupplementTarget(FDiagnostic& Diagnostic, const FString& Target)
		{
			if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = Target;
			for (auto& Child : Diagnostic.Diagnostics) SupplementTarget(Child, Target);
			if (Diagnostic.PublicCause)
			{
				auto Cause = MakeShared<FDiagnostic>(*Diagnostic.PublicCause);
				SupplementTarget(*Cause, Target);
				Diagnostic.PublicCause = MoveTemp(Cause);
			}
		}

		bool Fail(
			FWorkspaceError& OutError,
			UE::UAI::Contract::FDiagnostic Diagnostic,
			FString Source = FString(),
			FString Pointer = TEXT("/"))
		{
			OutError.SetFailure(MoveTemp(Diagnostic), Source, Pointer);
			return false;
		}

		bool CreationInputFailure(FWorkspaceError& OutError, const UE::UAI::Contract::FDiagnostic& Detail, const FString& Source)
		{
			OutError.SetFailure(Detail, Source);
			return false;
		}

		bool ValidateCreationFields(const FJsonObject& Object,
			std::initializer_list<UeAgentAuthoringJsonFields::FFieldType> Fields,
			const FString& Source, FWorkspaceError& OutError)
		{
			UE::UAI::Contract::FDiagnostic Detail;
			return UeAgentAuthoringJsonFields::ValidateOptionalFields(Object, Fields, Detail, Source)
				|| CreationInputFailure(OutError, Detail, Source);
		}

		bool ReadCreationPosition(const FJsonObject& Object, int32& X, int32& Y, FWorkspaceError& OutError)
		{
			UE::UAI::Contract::FDiagnostic Detail;
			return (UeAgentAuthoringJsonFields::ReadInt32Field(Object, TEXT("pos_x"), X, Detail)
				&& UeAgentAuthoringJsonFields::ReadInt32Field(Object, TEXT("pos_y"), Y, Detail))
				|| CreationInputFailure(OutError, Detail, TEXT("node_position"));
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

		bool CanonicalObject(
			const TSharedPtr<FJsonObject>& Object,
			FString& Out,
			FWorkspaceError& OutError)
		{
			return TryCanonicalizeObject(Object, Out, OutError);
		}

		bool EnsureAuthoringRegistered(FWorkspaceError& OutError)
		{
			static const TPair<bool, UE::UAI::Contract::FDiagnostic> Registration = []()
			{
				UE::UAI::Contract::FDiagnostic Error;
				const bool bBlueprint =
					UeAgentBlueprintAuthoring::RegisterAuthoring(Error);
				const bool bAnimation = bBlueprint
					&& UeAgentAnimationAuthoring::RegisterAuthoring(Error);
				return TPair<bool, UE::UAI::Contract::FDiagnostic>(bAnimation, MoveTemp(Error));
			}();
			if (!Registration.Key)
			{
				return Fail(OutError, Registration.Value, FString(), TEXT("/"));
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
				auto Diagnostic = NativeFailure(TEXT("animation_blueprint_target_unresolved"),
					TEXT("The capture target could not be loaded as an animation Blueprint."), Target);
				Diagnostic.ActualValue = MakeShared<FJsonValueNull>();
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("AnimBlueprint"));
				return Fail(OutError, MoveTemp(Diagnostic));
			}
			TSharedPtr<FJsonObject> Document;
			UE::UAI::Contract::FDiagnostic Error;
			if (!UeAgentAnimationAuthoring::BuildDocument(
				UeAgentAnimationAuthoring::EAssetKind::AnimBlueprint,
				*Blueprint, Document, Error, PackagePath(Target))
				|| !Document.IsValid())
			{
				if (Error.IsEmpty())
					Error = NativeFailure(TEXT("animation_blueprint_capture_document_missing"),
						TEXT("The animation Blueprint capture did not produce a document."), Target);
				return Fail(OutError, Error, Target, TEXT("/"));
			}

			TArray<TSharedPtr<FJsonValue>> Graphs;
			for (UEdGraph* Graph : UeAgentAnimBlueprintOps::GatherAnimGraphs(Blueprint))
			{
				if (!Graph) continue;
				const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
				Value->SetStringField(TEXT("name"), Graph->GetName());
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
			Document->SetBoolField(TEXT("complete"), true);

			FString Canonical;
			if (!CanonicalObject(Document, Canonical, OutError)) return false;
			OutRevision = TEXT("animation-blueprint:")
				+ FMD5::HashAnsiString(*Canonical);
			AddStoredBlueprintDiagnostics(*Blueprint, Document.ToSharedRef());
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

		bool ArrayObjects(
			const TSharedPtr<FJsonObject>& Owner,
			const FString& Field,
			TArray<TSharedPtr<FJsonObject>>& Out,
			FWorkspaceError& OutError)
		{
			Out.Reset();
			if (Owner && !ValidateCreationFields(*Owner, {{*Field, EJson::Array}}, Field, OutError)) return false;
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
					auto Diagnostic = InputFailure(TEXT("animation_blueprint_structure_item_invalid"),
						TEXT("Each animation structure item must be an object."), Owner.Get(), Field,
						TEXT("object"), TEXT("/") + EscapeJsonPointerSegment(Field) + TEXT("/") + FString::FromInt(Index));
					Diagnostic.ActualValuePresent = true;
					Diagnostic.ActualValue = Value.IsValid() ? Value : MakeShared<FJsonValueNull>();
					return Fail(OutError, MoveTemp(Diagnostic));
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
				return Fail(OutError, InputFailure(TEXT("animation_blueprint_structure_name_required"),
					TEXT("The animation structure field requires a non-empty string."), Object.Get(), Field,
					TEXT("non-empty string")));
			}
			OutName.TrimStartAndEndInline();
			return true;
		}

		bool ReadOptionalCandidateSelector(
			const TSharedPtr<FJsonObject>& Object,
			const FString& JsonPointer,
			FDomainHjsonCandidateSelector& OutSelector,
			FWorkspaceError& OutError)
		{
			OutSelector = {};
			const TSharedPtr<FJsonObject>* CandidateField = nullptr;
			if (!Object.IsValid()
				|| !Object->TryGetObjectField(TEXT("candidate"), CandidateField))
			{
				if (Object.IsValid() && Object->HasField(TEXT("candidate")))
				{
					return Fail(OutError, InputFailure(TEXT("animation_blueprint_candidate_invalid"),
						TEXT("The candidate selector must be an object."), Object.Get(), TEXT("candidate"),
						TEXT("candidate selector object"), JsonPointer + TEXT("/candidate")));
				}
				return true;
			}
			if (!CandidateField || !CandidateField->IsValid())
			{
				return Fail(OutError, InputFailure(TEXT("animation_blueprint_candidate_invalid"),
					TEXT("The candidate selector must contain a valid object."), Object.Get(), TEXT("candidate"),
					TEXT("candidate selector object"), JsonPointer + TEXT("/candidate")));
			}
			FWorkspaceError SelectorError;
			if (!TryReadCandidateSelector(
					*CandidateField,
					JsonPointer + TEXT("/candidate"),
					OutSelector,
					SelectorError))
			{
				OutError = MoveTemp(SelectorError);
				return false;
			}
			return true;
		}

		bool ValidateCandidateClassAssertion(
			const TSharedPtr<FJsonObject>& Object,
			const QueryOwners::AnimationBlueprint::
				FAnimationBlueprintCandidate& Candidate,
			const FString& Subject,
			FWorkspaceError& OutError)
		{
			FString ClassAssertion;
			if (!Object.IsValid()
				|| !Object->TryGetStringField(TEXT("class"), ClassAssertion)
				|| ClassAssertion.TrimStartAndEnd().IsEmpty())
			{
				return true;
			}
			ClassAssertion.TrimStartAndEndInline();
			if (ClassAssertion != Candidate.NodeClassPath)
			{
				auto Diagnostic = InputFailure(TEXT("animation_blueprint_candidate_class_conflict"),
					TEXT("The asserted node class differs from the resolved candidate class."), Object.Get(),
					TEXT("class"), Candidate.NodeClassPath);
				Diagnostic.SubjectId = Subject;
				return Fail(OutError, MoveTemp(Diagnostic));
			}
			return true;
		}

		// Values come from the creation request; GUIDs only bind those values to
		// the native objects created by the schema. Never rebuild intent from a
		// post-compile snapshot, which could already have lost a requested field.
		struct FAnimationSurfaceIntent
		{
			struct FGraph
			{
				FGuid Guid;
				FName Name;
				FString Source;
				FString NameField = TEXT("name");
			};
			struct FNode
			{
				FGuid Guid;
				FGuid GraphGuid;
				FString ClassPath;
				FString Source;
				TOptional<FName> BoundName;
				TOptional<int32> X, Y;
				TOptional<bool> ResetOnEntry, Bidirectional;
				TOptional<float> Crossfade;
				FGuid PreviousState, NextState;
				FString PreviousStateName, NextStateName;
			};
			struct FEntry
			{
				FGuid MachineGraph, State;
				FString Source;
				FString StateName;
			};
			TArray<FGraph> Graphs;
			TArray<FNode> Nodes;
			TArray<FEntry> Entries;

			bool Verify(UAnimBlueprint& Blueprint, FWorkspaceError& Error) const
			{
				TArray<UEdGraph*> NativeGraphs;
				Blueprint.GetAllGraphs(NativeGraphs);
				TMap<FGuid, UEdGraph*> GraphByGuid;
				TMap<FGuid, UEdGraphNode*> NodeByGuid;
				for (UEdGraph* Graph : NativeGraphs) if (Graph)
				{
					GraphByGuid.Add(Graph->GraphGuid, Graph);
					for (UEdGraphNode* Node : Graph->Nodes) if (Node) NodeByGuid.Add(Node->NodeGuid, Node);
				}
				auto Mismatch = [&Error, &Blueprint](const FString& Source, const TCHAR* Field,
					TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
					const FString& Reason = TEXT("The resulting animation Blueprint field differs from the requested structure."))
				{
					auto Diagnostic = StructureFailure(TEXT("animation_blueprint_structure_intent_mismatch"),
						Reason, Blueprint, Field);
					Diagnostic.SourceKind = TEXT("related_input");
					Diagnostic.Source.JsonPath = Source + TEXT("/") + Field;
					Diagnostic.PropertyPath = Field;
					Diagnostic.ActualValue = MoveTemp(Actual);
					Diagnostic.ExpectedValue = MoveTemp(Expected);
					return Fail(Error, MoveTemp(Diagnostic));
				};
				for (const FGraph& Intent : Graphs)
				{
					UEdGraph* Graph = GraphByGuid.FindRef(Intent.Guid);
					if (!Graph || !AnimationEditorUtils::IsAnimGraph(Graph))
					{
						TSharedPtr<FJsonValue> Actual = MakeShared<FJsonValueNull>();
						if (Graph) Actual = MakeShared<FJsonValueString>(Graph->GetClass()->GetName());
						return Mismatch(Intent.Source, *Intent.NameField, Actual, MakeShared<FJsonValueString>(TEXT("AnimationGraph")));
					}
					if (Graph->GetFName() != Intent.Name) return Mismatch(Intent.Source, *Intent.NameField,
						MakeShared<FJsonValueString>(Graph->GetName()), MakeShared<FJsonValueString>(Intent.Name.ToString()));
				}
				for (const FNode& Intent : Nodes)
				{
					UEdGraphNode* Node = NodeByGuid.FindRef(Intent.Guid);
					if (!Node || Node->GetClass()->GetPathName() != Intent.ClassPath)
					{
						TSharedPtr<FJsonValue> Actual = MakeShared<FJsonValueNull>();
						if (Node) Actual = MakeShared<FJsonValueString>(Node->GetClass()->GetPathName());
						return Mismatch(Intent.Source, TEXT("class"), Actual, MakeShared<FJsonValueString>(Intent.ClassPath));
					}
					if (!Node->GetGraph() || Node->GetGraph()->GraphGuid != Intent.GraphGuid)
						return Mismatch(Intent.Source, TEXT("owning_graph"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
							TEXT("The node is no longer bound to its requested owning graph."));
					if (Intent.X.IsSet() && Node->NodePosX != Intent.X.GetValue()) return Mismatch(Intent.Source, TEXT("pos_x"),
						MakeShared<FJsonValueNumber>(Node->NodePosX), MakeShared<FJsonValueNumber>(Intent.X.GetValue()));
					if (Intent.Y.IsSet() && Node->NodePosY != Intent.Y.GetValue()) return Mismatch(Intent.Source, TEXT("pos_y"),
						MakeShared<FJsonValueNumber>(Node->NodePosY), MakeShared<FJsonValueNumber>(Intent.Y.GetValue()));
					if (Intent.BoundName.IsSet())
					{
						UEdGraph* Bound = nullptr;
						if (auto* Machine = Cast<UAnimGraphNode_StateMachineBase>(Node)) Bound = Machine->EditorStateMachineGraph;
						else if (auto* State = Cast<UAnimStateNode>(Node)) Bound = State->GetBoundGraph();
						if (!Bound || Bound->GetFName() != Intent.BoundName.GetValue())
						{
							TSharedPtr<FJsonValue> Actual = MakeShared<FJsonValueNull>();
							if (Bound) Actual = MakeShared<FJsonValueString>(Bound->GetName());
							return Mismatch(Intent.Source, TEXT("name"), Actual, MakeShared<FJsonValueString>(Intent.BoundName.GetValue().ToString()));
						}
					}
					if (Intent.ResetOnEntry.IsSet())
					{
						const auto* State = Cast<UAnimStateNode>(Node);
						if (!State || State->bAlwaysResetOnEntry != Intent.ResetOnEntry.GetValue())
						{
							TSharedPtr<FJsonValue> Actual = MakeShared<FJsonValueNull>();
							if (State) Actual = MakeShared<FJsonValueBoolean>(State->bAlwaysResetOnEntry);
							return Mismatch(Intent.Source, TEXT("always_reset_on_entry"), Actual, MakeShared<FJsonValueBoolean>(Intent.ResetOnEntry.GetValue()));
						}
					}
					if (Intent.PreviousState.IsValid())
					{
						const auto* Transition = Cast<UAnimStateTransitionNode>(Node);
						if (!Transition || !Transition->GetPreviousState() || Transition->GetPreviousState()->NodeGuid != Intent.PreviousState)
						{
							TSharedPtr<FJsonValue> Actual = MakeShared<FJsonValueNull>();
							if (Transition && Transition->GetPreviousState()) Actual = MakeShared<FJsonValueString>(Transition->GetPreviousState()->GetStateName());
							return Mismatch(Intent.Source, TEXT("source"), Actual, MakeShared<FJsonValueString>(Intent.PreviousStateName));
						}
						if (!Transition->GetNextState() || Transition->GetNextState()->NodeGuid != Intent.NextState)
						{
							TSharedPtr<FJsonValue> Actual = MakeShared<FJsonValueNull>();
							if (Transition->GetNextState()) Actual = MakeShared<FJsonValueString>(Transition->GetNextState()->GetStateName());
							return Mismatch(Intent.Source, TEXT("target"), Actual, MakeShared<FJsonValueString>(Intent.NextStateName));
						}
						if (Intent.Bidirectional.IsSet() && Transition->Bidirectional != Intent.Bidirectional.GetValue()) return Mismatch(Intent.Source, TEXT("bidirectional"),
							MakeShared<FJsonValueBoolean>(Transition->Bidirectional), MakeShared<FJsonValueBoolean>(Intent.Bidirectional.GetValue()));
						if (Intent.Crossfade.IsSet() && Transition->CrossfadeDuration != Intent.Crossfade.GetValue()) return Mismatch(Intent.Source, TEXT("crossfade_duration"),
							MakeShared<FJsonValueNumber>(Transition->CrossfadeDuration), MakeShared<FJsonValueNumber>(Intent.Crossfade.GetValue()));
					}
				}
				for (const FEntry& Intent : Entries)
				{
					auto* Machine = Cast<UAnimationStateMachineGraph>(GraphByGuid.FindRef(Intent.MachineGraph));
					const auto* State = UeAgentAnimBlueprintOps::GetEntryConnectedState(Machine);
					if (!State || State->NodeGuid != Intent.State)
					{
						TSharedPtr<FJsonValue> Actual = MakeShared<FJsonValueNull>();
						if (State) Actual = MakeShared<FJsonValueString>(State->GetStateName());
						return Mismatch(Intent.Source, TEXT("entry_state"), Actual, MakeShared<FJsonValueString>(Intent.StateName));
					}
				}
				return true;
			}
		};

		bool ApplyAnimationSurfaces(
			UAnimBlueprint& Blueprint,
			const TSharedPtr<FJsonObject>& Extension,
			FWorkspaceError& OutError,
			int32& OutGraphs,
			int32& OutMachines,
			int32& OutStates,
			int32& OutTransitions,
			FAnimationSurfaceIntent& OutIntent)
		{
			OutGraphs = OutMachines = OutStates = OutTransitions = 0;
			OutIntent = {};
			TArray<TSharedPtr<FJsonObject>> GraphSpecs;
			if (!ArrayObjects(Extension, TEXT("animation_graphs"),
				GraphSpecs, OutError)) return false;
			for (const TSharedPtr<FJsonObject>& Spec : GraphSpecs)
			{
				const FString GraphSource = TEXT("/provider_extension/animation_graphs/")
					+ FString::FromInt(GraphSpecs.IndexOfByKey(Spec));
				FString Name;
				if (!RequiredName(Spec, TEXT("name"), Name, OutError)) return false;
				if (UEdGraph* Existing = UeAgentAnimBlueprintOps::ResolveAnyAnimBlueprintGraph(&Blueprint, Name))
				{
					if (!AnimationEditorUtils::IsAnimGraph(Existing))
					{
						auto Diagnostic = InputFailure(TEXT("animation_blueprint_graph_type_conflict"),
							TEXT("The named graph exists but is not an animation graph."), Spec.Get(), TEXT("name"), TEXT("animation graph"));
						Diagnostic.SubjectId = Name;
						Diagnostic.ActualValue = MakeShared<FJsonValueString>(Existing->GetClass()->GetName());
						Diagnostic.SourceKind = TEXT("related_input");
						return Fail(OutError, MoveTemp(Diagnostic));
					}
					OutIntent.Graphs.Add({Existing->GraphGuid, FName(*Name), GraphSource});
					continue;
				}
				UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(
					&Blueprint, FName(*Name), UAnimationGraph::StaticClass(),
					UAnimationGraphSchema::StaticClass());
				if (!Graph)
				{
					auto Diagnostic = StructureFailure(TEXT("animation_blueprint_graph_create_failed"),
						TEXT("The editor returned no graph when creating the animation graph."), Blueprint, Name);
					Diagnostic.ActualValue = MakeShared<FJsonValueNull>();
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("AnimationGraph"));
					return Fail(OutError, MoveTemp(Diagnostic));
				}
				FBlueprintEditorUtils::AddDomainSpecificGraph(&Blueprint, Graph);
				TArray<UAnimGraphNode_Root*> Roots;
				Graph->GetNodesOfClass<UAnimGraphNode_Root>(Roots);
				if (Roots.IsEmpty() && Graph->GetSchema())
				{
					Graph->GetSchema()->CreateDefaultNodesForGraph(*Graph);
				}
				++OutGraphs;
				OutIntent.Graphs.Add({Graph->GraphGuid, FName(*Name), GraphSource});
			}

			TArray<TSharedPtr<FJsonObject>> MachineSpecs;
			if (!ArrayObjects(Extension, TEXT("state_machines"),
				MachineSpecs, OutError)) return false;
			for (const TSharedPtr<FJsonObject>& MachineSpec : MachineSpecs)
			{
				const FString MachineSource = TEXT("/provider_extension/state_machines/")
					+ FString::FromInt(MachineSpecs.IndexOfByKey(MachineSpec));
				if (!ValidateCreationFields(*MachineSpec, {{TEXT("animation_graph"), EJson::String},
					{TEXT("entry_state"), EJson::String}, {TEXT("class"), EJson::String}},
					TEXT("/provider_extension/state_machines"), OutError)) return false;
				int32 X = 0, Y = 0;
				if (!ReadCreationPosition(*MachineSpec, X, Y, OutError)) return false;
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
					auto Diagnostic = InputFailure(TEXT("animation_blueprint_anim_graph_unresolved"),
						AnimGraph ? TEXT("The selected graph is not an animation graph.")
							: TEXT("The selected animation graph does not exist in the Blueprint."),
						MachineSpec.Get(), TEXT("animation_graph"), TEXT("existing animation graph"), MachineSource + TEXT("/animation_graph"));
					Diagnostic.CandidateName = GraphName;
					return Fail(OutError, MoveTemp(Diagnostic));
				}
				OutIntent.Graphs.Add({AnimGraph->GraphGuid, FName(*GraphName), MachineSource, TEXT("animation_graph")});
				// A name is scoped to its owning graph, not the entire Blueprint.
				TArray<UAnimGraphNode_StateMachineBase*> ExistingMachines;
				AnimGraph->GetNodesOfClass(ExistingMachines);
				for (const auto* Existing : ExistingMachines)
					if (Existing && Existing->EditorStateMachineGraph && Existing->EditorStateMachineGraph->GetFName() == FName(*MachineName))
						return Fail(OutError, InputFailure(TEXT("animation_blueprint_structure_identity_conflict"),
							TEXT("A state machine with this name already exists in the selected animation graph."),
							MachineSpec.Get(), TEXT("name"), TEXT("state machine name unique within the animation graph"), MachineSource + TEXT("/name")));
				UAnimGraphNode_StateMachineBase* MachineNode = nullptr;
				{
					FDomainHjsonCandidateSelector Selector;
					if (!ReadOptionalCandidateSelector(
							MachineSpec,
							MachineSource,
							Selector,
							OutError))
					{
						return false;
					}
					UEdGraphNode* CreatedNode = nullptr;
					QueryOwners::AnimationBlueprint::
						FAnimationBlueprintCandidate ResolvedCandidate;
					QueryOwners::AnimationBlueprint::
						FAnimationBlueprintCandidateQueryError CandidateError;
					if (!QueryOwners::AnimationBlueprint::
							TryPerformAnimationGraphStateMachineCandidate(
								Blueprint,
								*AnimGraph,
								Selector.Name,
							FVector2f::ZeroVector,
								CreatedNode,
								ResolvedCandidate,
								CandidateError, Selector.Select))
					{
						if (CandidateError.SourceKind != TEXT("native_result"))
							UeAgentAuthoringDiagnostics::LocateAuthoringSource(CandidateError, *MachineSpec, TEXT("candidate"));
						OutError.SetFailure(CandidateError, FString(), MachineSource + TEXT("/candidate"));
						return false;
					}
					UAnimGraphNode_StateMachine* Created =
						Cast<UAnimGraphNode_StateMachine>(CreatedNode);
					if (!Created || !Created->EditorStateMachineGraph)
					{
						auto Diagnostic = StructureFailure(TEXT("animation_blueprint_state_machine_create_failed"),
							TEXT("The candidate action did not produce an animation state machine with a bound graph."), Blueprint, MachineName);
						if (CreatedNode) Diagnostic.ActualValue = MakeShared<FJsonValueString>(CreatedNode->GetClass()->GetName());
						else Diagnostic.ActualValue = MakeShared<FJsonValueNull>();
						Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("AnimGraphNode_StateMachine with a bound graph"));
						Diagnostic.MutationMayHaveStarted = true;
						if (CreatedNode) Diagnostic.MutationStarted = true;
						return Fail(OutError, MoveTemp(Diagnostic));
					}
					if (!ValidateCandidateClassAssertion(
							MachineSpec,
							ResolvedCandidate,
							MachineName,
							OutError))
					{
						return false;
					}
					FBlueprintEditorUtils::RenameGraph(
						Created->EditorStateMachineGraph, MachineName);
					// The action API uses float positions; native graph coordinates are
					// int32. Assign the validated original coordinates without narrowing.
					Created->NodePosX = X;
					Created->NodePosY = Y;
					MachineNode = Created;
					++OutMachines;
					FAnimationSurfaceIntent::FNode Intent;
					Intent.Guid = Created->NodeGuid; Intent.GraphGuid = AnimGraph->GraphGuid;
					Intent.ClassPath = ResolvedCandidate.NodeClassPath; Intent.Source = MachineSource;
					Intent.BoundName = FName(*MachineName); Intent.X = X; Intent.Y = Y;
					OutIntent.Nodes.Add(MoveTemp(Intent));
				}
				UAnimationStateMachineGraph* Machine =
					MachineNode->EditorStateMachineGraph;
				if (!Machine)
				{
					auto Diagnostic = StructureFailure(TEXT("animation_blueprint_state_machine_graph_missing"),
						TEXT("The created state machine has no editor graph."), Blueprint, MachineName);
					Diagnostic.ActualValue = MakeShared<FJsonValueNull>();
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("AnimationStateMachineGraph"));
					return Fail(OutError, MoveTemp(Diagnostic));
				}

				TArray<TSharedPtr<FJsonObject>> StateSpecs;
				if (!ArrayObjects(
					MachineSpec, TEXT("states"), StateSpecs, OutError)) return false;
				for (const TSharedPtr<FJsonObject>& StateSpec : StateSpecs)
				{
					const FString StateSource = MachineSource + TEXT("/states/")
						+ FString::FromInt(StateSpecs.IndexOfByKey(StateSpec));
					if (!ValidateCreationFields(*StateSpec, {{TEXT("always_reset_on_entry"), EJson::Boolean},
						{TEXT("class"), EJson::String}}, TEXT("/provider_extension/state_machines/states"), OutError)) return false;
					int32 StateX = 0, StateY = 0;
					if (!ReadCreationPosition(*StateSpec, StateX, StateY, OutError)) return false;
					FString StateName;
					if (!RequiredName(
						StateSpec, TEXT("name"), StateName, OutError)) return false;
					if (UeAgentAnimBlueprintOps::FindStateNodeByName(
						Machine, StateName))
					{
						return Fail(OutError, InputFailure(TEXT("animation_blueprint_structure_identity_conflict"),
							TEXT("A state with this name already exists in the selected state machine."),
							StateSpec.Get(), TEXT("name"), TEXT("state name unique within the state machine"), StateSource + TEXT("/name")));
					}
					FDomainHjsonCandidateSelector Selector;
					if (!ReadOptionalCandidateSelector(
						StateSpec,
							StateSource,
							Selector,
							OutError))
					{
						return false;
					}
					UEdGraphNode* CreatedNode = nullptr;
					QueryOwners::AnimationBlueprint::
						FAnimationBlueprintCandidate ResolvedCandidate;
					QueryOwners::AnimationBlueprint::
						FAnimationBlueprintCandidateQueryError CandidateError;
					if (!QueryOwners::AnimationBlueprint::
							TryPerformStateMachineGraphCandidate(
								*Machine,
								Selector.Name,
							FVector2f::ZeroVector,
								CreatedNode,
								ResolvedCandidate,
								CandidateError, Selector.Select))
					{
						if (CandidateError.SourceKind != TEXT("native_result"))
							UeAgentAuthoringDiagnostics::LocateAuthoringSource(CandidateError, *StateSpec, TEXT("candidate"));
						OutError.SetFailure(CandidateError, FString(), StateSource + TEXT("/candidate"));
						return false;
					}
					UAnimStateNode* State = Cast<UAnimStateNode>(CreatedNode);
					if (!State || !State->GetBoundGraph())
					{
						auto Diagnostic = StructureFailure(TEXT("animation_blueprint_state_create_failed"),
							TEXT("The candidate action did not produce an animation state with a bound graph."), Blueprint, StateName);
						if (CreatedNode) Diagnostic.ActualValue = MakeShared<FJsonValueString>(CreatedNode->GetClass()->GetName());
						else Diagnostic.ActualValue = MakeShared<FJsonValueNull>();
						Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("AnimStateNode with a bound graph"));
						Diagnostic.MutationMayHaveStarted = true;
						if (CreatedNode) Diagnostic.MutationStarted = true;
						return Fail(OutError, MoveTemp(Diagnostic));
					}
					if (!ValidateCandidateClassAssertion(
							StateSpec,
							ResolvedCandidate,
							StateName,
							OutError))
					{
						return false;
					}
					FBlueprintEditorUtils::RenameGraph(
						State->GetBoundGraph(), StateName);
					State->NodePosX = StateX;
					State->NodePosY = StateY;
					bool ResetOnEntry = GetDefault<UAnimStateNode>()->bAlwaysResetOnEntry;
					StateSpec->TryGetBoolField(TEXT("always_reset_on_entry"), ResetOnEntry);
					State->bAlwaysResetOnEntry = ResetOnEntry;
					FAnimationSurfaceIntent::FNode Intent;
					Intent.Guid = State->NodeGuid; Intent.GraphGuid = Machine->GraphGuid;
					Intent.ClassPath = ResolvedCandidate.NodeClassPath; Intent.Source = StateSource;
					Intent.BoundName = FName(*StateName); Intent.X = StateX; Intent.Y = StateY; Intent.ResetOnEntry = ResetOnEntry;
					OutIntent.Nodes.Add(MoveTemp(Intent));
					++OutStates;
				}

				FString EntryState;
				if (MachineSpec->TryGetStringField(
					TEXT("entry_state"), EntryState))
				{
					UAnimStateNodeBase* Entry =
						UeAgentAnimBlueprintOps::FindStateNodeByName(
							Machine, EntryState);
					UE::UAI::Contract::FDiagnostic Error;
					if (!UeAgentAnimBlueprintOps::SetEntryState(
						Machine, Entry, Error))
					{
						if (Error.TargetPath.IsEmpty())
							Error.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint);
						if (Error.CandidateName.IsEmpty()) Error.CandidateName = EntryState;
						if (Error.SourceKind.IsEmpty())
						{
							Error.SourceKind = TEXT("related_input");
							Error.Source.JsonPath = MachineSource + TEXT("/entry_state");
						}
						return Fail(OutError, Error);
					}
					OutIntent.Entries.Add({Machine->GraphGuid, Entry->NodeGuid, MachineSource, EntryState});
				}

				TArray<TSharedPtr<FJsonObject>> TransitionSpecs;
				if (!ArrayObjects(MachineSpec, TEXT("transitions"),
					TransitionSpecs, OutError)) return false;
				for (int32 TransitionIndex = 0; TransitionIndex < TransitionSpecs.Num(); ++TransitionIndex)
				{
					const TSharedPtr<FJsonObject>& TransitionSpec = TransitionSpecs[TransitionIndex];
					if (!ValidateCreationFields(*TransitionSpec, {{TEXT("bidirectional"), EJson::Boolean}},
						TEXT("/provider_extension/state_machines/transitions"), OutError)) return false;
					float Crossfade = GetDefault<UAnimStateTransitionNode>()->CrossfadeDuration;
					UE::UAI::Contract::FDiagnostic ValueError;
					if (!UeAgentAuthoringJsonFields::ReadFloat32Field(*TransitionSpec, TEXT("crossfade_duration"), Crossfade, ValueError))
						return CreationInputFailure(OutError, ValueError, TEXT("crossfade_duration"));
					FString Source;
					FString Target;
					if (!RequiredName(TransitionSpec,
						TEXT("source"), Source, OutError)
						|| !RequiredName(TransitionSpec,
							TEXT("target"), Target, OutError))
					{
						return false;
					}
					UAnimStateNodeBase* SourceState =
						UeAgentAnimBlueprintOps::FindStateNodeByName(Machine, Source);
					UAnimStateNodeBase* TargetState =
						UeAgentAnimBlueprintOps::FindStateNodeByName(Machine, Target);
					if (!SourceState || !TargetState)
					{
						const FString Field = !SourceState ? TEXT("source") : TEXT("target");
						auto Diagnostic = InputFailure(TEXT("animation_blueprint_transition_endpoint_unresolved"),
							!SourceState && !TargetState ? TEXT("Neither transition endpoint resolves to a state in the selected state machine.")
								: !SourceState ? TEXT("The transition source does not resolve to a state in the selected state machine.")
								: TEXT("The transition target does not resolve to a state in the selected state machine."),
							TransitionSpec.Get(), Field, TEXT("existing state in the selected state machine"));
						Diagnostic.SubjectId = Source + TEXT("->") + Target;
						return Fail(OutError, MoveTemp(Diagnostic));
					}
					const UAnimationStateMachineSchema* Schema =
						Cast<UAnimationStateMachineSchema>(Machine->GetSchema());
					if (!Schema)
					{
						auto Diagnostic = StructureFailure(TEXT("animation_blueprint_transition_schema_unresolved"),
							TEXT("The state machine graph has no animation state machine schema."), Blueprint, Source + TEXT("->") + Target);
						if (Machine->GetSchema()) Diagnostic.ActualValue = MakeShared<FJsonValueString>(Machine->GetSchema()->GetClass()->GetName());
						else Diagnostic.ActualValue = MakeShared<FJsonValueNull>();
						Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("AnimationStateMachineSchema"));
						return Fail(OutError, MoveTemp(Diagnostic));
					}
					TSet<UEdGraphNode*> BeforeNodes;
					for (UEdGraphNode* Node : Machine->Nodes) BeforeNodes.Add(Node);
					const FPinConnectionResponse ConnectionResponse = Schema->CanCreateConnection(
						SourceState->GetOutputPin(), TargetState->GetInputPin());
					if (!Schema->TryCreateConnection(
							SourceState->GetOutputPin(), TargetState->GetInputPin()))
					{
						auto Diagnostic = StructureFailure(TEXT("animation_blueprint_transition_connection_rejected"),
							ConnectionResponse.Response == CONNECT_RESPONSE_DISALLOW && !ConnectionResponse.Message.IsEmpty()
								? ConnectionResponse.Message.ToString() : TEXT("The graph schema did not create the requested transition connection."),
							Blueprint, Source + TEXT("->") + Target);
						Diagnostic.PropertyPath = TEXT("transition_connection");
						Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
						Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
						Diagnostic.MutationMayHaveStarted = true;
						for (UEdGraphNode* Node : Machine->Nodes)
							if (!BeforeNodes.Contains(Node)) Diagnostic.MutationStarted = true;
						return Fail(OutError, MoveTemp(Diagnostic));
					}
					// Every relation creates its own native transition. Endpoint pairs
					// are not unique identities: UE permits parallel transition rules.
					UAnimStateTransitionNode* Transition = nullptr;
					for (UEdGraphNode* Node : Machine->Nodes)
						if (!BeforeNodes.Contains(Node))
							if (auto* Added = Cast<UAnimStateTransitionNode>(Node))
								if (Added->GetPreviousState() == SourceState && Added->GetNextState() == TargetState) Transition = Added;
					if (!Transition)
					{
						auto Diagnostic = StructureFailure(TEXT("animation_blueprint_transition_create_failed"),
							TEXT("The schema accepted the connection but produced no transition node between the requested states."),
							Blueprint, Source + TEXT("->") + Target);
						TArray<TSharedPtr<FJsonValue>> AddedClasses;
						for (UEdGraphNode* Node : Machine->Nodes)
							if (Node && !BeforeNodes.Contains(Node)) AddedClasses.Add(MakeShared<FJsonValueString>(Node->GetClass()->GetName()));
						Diagnostic.ActualValue = MakeShared<FJsonValueArray>(AddedClasses);
						Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("AnimStateTransitionNode between the requested states"));
						Diagnostic.MutationMayHaveStarted = true;
						if (!AddedClasses.IsEmpty()) Diagnostic.MutationStarted = true;
						return Fail(OutError, MoveTemp(Diagnostic));
					}
					bool Bidirectional = GetDefault<UAnimStateTransitionNode>()->Bidirectional;
					TransitionSpec->TryGetBoolField(TEXT("bidirectional"), Bidirectional);
					Transition->Bidirectional = Bidirectional;
					if (TransitionSpec->HasField(TEXT("crossfade_duration")))
					{
						Transition->CrossfadeDuration =
							FMath::Max(0.0f, Crossfade);
					}
					++OutTransitions;
					FAnimationSurfaceIntent::FNode Intent;
					Intent.Guid = Transition->NodeGuid; Intent.GraphGuid = Machine->GraphGuid;
					Intent.ClassPath = UAnimStateTransitionNode::StaticClass()->GetPathName();
					Intent.Source = MachineSource + TEXT("/transitions/") + FString::FromInt(TransitionIndex);
					Intent.PreviousState = SourceState->NodeGuid; Intent.NextState = TargetState->NodeGuid;
					Intent.PreviousStateName = Source; Intent.NextStateName = Target;
					Intent.Bidirectional = Bidirectional; Intent.Crossfade = FMath::Max(0.0f, Crossfade);
					OutIntent.Nodes.Add(MoveTemp(Intent));
				}
			}
			return OutIntent.Verify(Blueprint, OutError);
		}

		void RetireScratch(TStrongObjectPtr<UAnimBlueprint>& Scratch)
		{
			if (!Scratch.IsValid()) return;
			Scratch->ClearFlags(RF_Public | RF_Standalone);
			Scratch->MarkAsGarbage();
			Scratch.Reset();
		}

		bool DeleteCreated(UAnimBlueprint* Blueprint, const FString& Target, FDiagnostic& OutDiagnostic)
		{
			OutDiagnostic.Reset();
			if (!Blueprint) return true;
			UPackage* const Package = Blueprint->GetOutermost();
			const FSoftObjectPath OriginalObjectPath(Blueprint);
			FAssetRegistryModule::AssetDeleted(Blueprint);
			TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);
			if (!ObjectTools::DeleteSingleObject(Blueprint, true))
			{
				FAssetRegistryModule::AssetCreated(Blueprint);
				OutDiagnostic = NativeFailure(TEXT("animation_blueprint_rollback_failed"),
					TEXT("The editor did not delete the newly created animation Blueprint during rollback."), Target);
				OutDiagnostic.Stage = TEXT("rollback");
				OutDiagnostic.PropertyPath = TEXT("deletion_succeeded");
				OutDiagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutDiagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				return false;
			}
			if (Package)
			{
				Package->SetDirtyFlag(false);
			}
			const FAssetRegistryModule& AssetRegistry =
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
					TEXT("AssetRegistry"));
			if (AssetRegistry.Get().GetAssetByObjectPath(OriginalObjectPath).IsValid())
			{
				OutDiagnostic = NativeFailure(TEXT("animation_blueprint_rollback_failed"),
					TEXT("The deleted animation Blueprint is still present in the asset registry after rollback."), Target);
				OutDiagnostic.Stage = TEXT("rollback");
				OutDiagnostic.PropertyPath = TEXT("asset_registered");
				OutDiagnostic.ActualValue = MakeShared<FJsonValueBoolean>(true);
				OutDiagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(false);
				return false;
			}
			return true;
		}

		bool TryImportSemanticWorkspace(
			const FString& Target,
			const FResolvedDomainHjsonDocument& Document,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			using namespace UE::UAI::Composition::HjsonImport;

			if (!Document.SemanticWorkspace.IsValid())
			{
				auto Diagnostic = NativeFailure(TEXT("animation_blueprint_semantic_workspace_missing"),
					TEXT("The resolved semantic import contains no workspace."), Target);
				Diagnostic.ActualValue = MakeShared<FJsonValueNull>();
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("resolved semantic workspace"));
				return Fail(OutError, MoveTemp(Diagnostic));
			}
			if (!EnsureAuthoringRegistered(OutError)) return false;
			const UeAgentAnimationAuthoring::FProfile* Profile =
				UeAgentAnimationAuthoring::FindProfile(
					UeAgentAnimationAuthoring::EAssetKind::AnimBlueprint);
			if (!Profile)
			{
				return Fail(OutError, NativeFailure(TEXT("animation_blueprint_semantic_profile_missing"),
					TEXT("No animation Blueprint authoring profile is registered."), Target));
			}

			UAnimBlueprint* Blueprint = LoadAnimationBlueprint(Target);
			if (!Blueprint)
			{
				auto Diagnostic = NativeFailure(TEXT("animation_blueprint_target_unresolved"),
					TEXT("The target could not be loaded as an animation Blueprint."), Target);
				Diagnostic.ActualValue = MakeShared<FJsonValueNull>();
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("AnimBlueprint"));
				return Fail(OutError, MoveTemp(Diagnostic));
			}
			TArray<UeAgentAuthoringState::FReadRequest> Requests;
			UE::UAI::Contract::FDiagnostic CaptureError;
			if (!UeAgentAnimationAuthoring::BuildAssetExportSnapshotReadRequests(
					*Profile, *Blueprint, Requests, CaptureError))
			{
				OutError.SetFailure(CaptureError, Target);
				return false;
			}
			if (!UeAgentBlueprintAuthoring::
					AppendReusableBlueprintExportSnapshotReadRequests(
						*Blueprint, Requests, CaptureError,
						Blueprint->GetOutermost()->GetName()))
			{
				OutError.SetFailure(CaptureError, Target);
				return false;
			}
			SelectRootPropertyImportReadRequests(*Document.SemanticWorkspace, Requests);
			UeAgentAuthoringState::FCaptureResult Capture;
			if (!CapturePropertyImportState(*Document.SemanticWorkspace, Requests, Capture))
			{
				OutError.SetFailure(Capture.Diagnostic, Target);
				return false;
			}

			ContextResource::FUeAgentResourceTracker Resources =
				MakeSemanticWorkspaceResourceTracker();
			StateSemantic::FUeAgentCanonicalSnapshotPtr CanonicalSnapshot;
			FAuthoringModelPtr Model;
			if (!TryBuildPropertyImportCurrentCapture(Document.SemanticWorkspace.Get(),
					TEXT("ue.anim_blueprint.asset"), TEXT("llm_authoring"),
					Capture.Snapshot, Resources, CanonicalSnapshot, Model, OutError)
				|| !CanonicalSnapshot.IsValid() || !Model.IsValid())
			{
				if (OutError.GetCode().IsEmpty())
				{
					auto Diagnostic = NativeFailure(TEXT("animation_blueprint_semantic_capture_failed"),
						TEXT("The semantic capture did not produce both a canonical snapshot and an authoring model."), Target);
					const auto Actual = MakeShared<FJsonObject>();
					Actual->SetBoolField(TEXT("canonical_snapshot"), CanonicalSnapshot.IsValid());
					Actual->SetBoolField(TEXT("authoring_model"), Model.IsValid());
					Diagnostic.ActualValue = MakeShared<FJsonValueObject>(Actual);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("canonical snapshot and authoring model"));
					return Fail(OutError, MoveTemp(Diagnostic));
				}
				return false;
			}

			Capture.Snapshot.ReleaseNativeValues();
			const FString TargetPackage = PackagePath(Target);
			const FString TargetObjectPath = ObjectPath(Target);
			const UeAgentAnimationAuthoring::EAssetKind AssetKind = Profile->Kind;
			return TryCoordinateSemanticWorkspaceImport(
				Document.SemanticWorkspace.ToSharedRef(), Model.ToSharedRef(),
				CanonicalSnapshot.ToSharedRef(),
				BuildSemanticWorkspaceImportDescriptor(TEXT("animation_blueprint.semantic_workspace"), *Model),
				Blueprint->GetPathName(),
				Resources,
				{
					TEXT("animation-blueprint-domain-provider"),
					Target,
					TargetObjectPath,
					TEXT("animation-blueprint-semantic"),
					TEXT("animation_blueprint_semantic_prepare_failed"),
					TEXT("animation_blueprint_semantic_apply_failed"),
					TEXT("animation_blueprint_semantic_readback_payload_invalid"),
					TEXT("animation_blueprint_compile_not_settled"),
					[Profile, TargetObjectPath](TArray<UeAgentAuthoringState::FReadRequest>& Requests, UE::UAI::Contract::FDiagnostic& Error)
					{
						UBlueprint* Native = Cast<UBlueprint>(StaticLoadObject(UBlueprint::StaticClass(), nullptr, *TargetObjectPath));
						return Native && UeAgentAnimationAuthoring::BuildAssetExportSnapshotReadRequests(*Profile, *Native, Requests, Error)
							&& UeAgentBlueprintAuthoring::AppendReusableBlueprintExportSnapshotReadRequests(
								*Native, Requests, Error, Native->GetOutermost()->GetName());
					}, {}, &UeAgentBlueprintAuthoring::ProjectObservedNodePinDefaults},
				[AssetKind, TargetPackage, CurrentModel = Model.ToSharedRef(), &Blueprint, &Requests](
					const FString& RequestId,
					const FPreparedAuthoringImport& Prepared,
					Composition::HjsonImport::FSemanticWorkspaceChangeProjection&& Projection,
					FRootPropertyApplyOutcome& OutOutcome)
				{
					const FAuthoringModelRef Desired = Prepared.GetWorkspace()->GetModel();
					TArray<FString> BlueprintUnitIds;
					FWorkspaceError ProjectionError;
					TSharedPtr<FJsonObject> ClassSettingsDocument;
					TSharedPtr<FJsonObject> BlueprintSurfaces = MakeShared<FJsonObject>();
					if (!HasOnlyRootPropertyIntents(*Prepared.GetWorkspace())) {
					for (const auto& Change : Prepared.GetChangeSetDraft().GetChanges())
					{
						if (Change.GetIdentity().GetSemanticId() !=
							UeAgentAnimationAuthoring::AnimBlueprintSettingsSemanticId) continue;
						TSharedPtr<FJsonValue> Json;
						UE::UAI::Contract::FDiagnostic JsonError;
						if (ClassSettingsDocument.IsValid() || !Change.GetProposedAfter().IsValid() ||
							!StateSemantic::CanonicalJson::TryToJsonValue(
								*Change.GetProposedAfter(), Json, JsonError) ||
							!Json.IsValid() || Json->Type != EJson::Object)
						{
							ProjectionError.SetFailure(JsonError.IsEmpty() ? UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_blueprint_class_settings_projection_failed"), TEXT("Class settings must project to one object.")) : JsonError, UeAgentAnimationAuthoring::AnimBlueprintSettingsSemanticId);
							break;
						}
						ClassSettingsDocument = Json->AsObject();
					}
					TMap<FString, int32> ReadsByTarget, ReadsBySnapshot;
					for (int32 Index = 0; Index < Requests.Num(); ++Index)
					{
						const auto& Read = Requests[Index];
						if (!ReadsByTarget.Contains(Read.TargetIdentity.CanonicalKey)) ReadsByTarget.Add(Read.TargetIdentity.CanonicalKey, Index);
						if (Read.SnapshotIdentityOverride.IsSet())
						{
							const FString& Key = Read.SnapshotIdentityOverride.GetValue().CanonicalKey;
							if (!ReadsBySnapshot.Contains(Key)) ReadsBySnapshot.Add(Key, Index);
						}
					}
					TArray<UEdGraph*> Graphs;
					Blueprint->GetAllGraphs(Graphs);
					TMap<const UEdGraph*, TSet<FGuid>> NodeGuidsByGraph;
					for (const UEdGraph* Graph : Graphs)
					{
						if (!Graph) continue;
						auto& Guids = NodeGuidsByGraph.FindOrAdd(Graph);
						for (const UEdGraphNode* Node : Graph->Nodes)
							if (Node && Node->GetGraph() == Graph) Guids.Add(Node->NodeGuid);
					}
					for (const FString& UnitId : Projection.ChangedUnitIds)
					{
						const FSemanticUnit* Unit = Desired->FindUnit(UnitId);
						if (!Unit)
						{
							auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_blueprint_semantic_unit_missing"),
								TEXT("A changed animation Blueprint unit is absent from the resolved semantic model."), TEXT("document_restore"));
							Detail.TargetPath = TargetPackage;
							Detail.SubjectId = UnitId;
							Detail.SourceKind = TEXT("native_result");
							ProjectionError.SetFailure(MoveTemp(Detail));
							break;
						}
						const FString& Semantic = Unit->GetSemanticTypeRef();
						// Document changes also select their captured target/surface anchors.
						// They carry no separate native animation or graph mutation.
						if (Unit->GetUnitKind() == ESemanticUnitKind::TargetState ||
							Semantic == TEXT("uai.authoring.surface.target_settings")) continue;
						if (Semantic.StartsWith(UeAgentAnimationAuthoring::AnimBlueprintSettingsSemanticId)) continue;
						if (Semantic == TEXT("core.property")) continue;
						if (Semantic == UeAgentBlueprintAuthoring::ClassDefaultPropertySemanticId ||
							Semantic == UeAgentBlueprintAuthoring::VariableSemanticId ||
							Semantic == UeAgentBlueprintAuthoring::DelegateSemanticId ||
							Semantic == UeAgentBlueprintAuthoring::InterfaceSemanticId ||
							Semantic == TEXT("uai.authoring.surface.graph_collection") ||
							(Unit->GetUnitKind() == ESemanticUnitKind::StructureState && Unit->GetRole() == TEXT("member_collection")))
						{ BlueprintUnitIds.Add(UnitId); continue; }
						const FSemanticUnit* GraphUnit = Unit;
						TSet<FString> Visited;
						while (GraphUnit && GraphUnit->GetSemanticTypeRef() != UeAgentBlueprintAuthoring::GraphSemanticId &&
							!Visited.Contains(GraphUnit->GetUnitId()))
						{
							Visited.Add(GraphUnit->GetUnitId());
							GraphUnit = Desired->FindUnit(GraphUnit->GetOwnerRef());
						}
						const int32* ReadIndex = GraphUnit ? ReadsByTarget.Find(GraphUnit->GetTargetIdentityKey()) : nullptr;
						const auto* Read = ReadIndex ? &Requests[*ReadIndex] : nullptr;
						const UEdGraph* NativeGraph = Read ? Cast<UEdGraph>(Read->RootObject) : nullptr;
						bool bExistingNodeProperty = false;
						if (Semantic == UeAgentBlueprintAuthoring::NodePropertySemanticId && NativeGraph)
						{
							const auto* NodeUnit = Desired->FindUnit(Unit->GetOwnerRef());
							const int32* NodeReadIndex = NodeUnit && NodeUnit->GetSemanticTypeRef() == UeAgentBlueprintAuthoring::NodeSemanticId
								? ReadsByTarget.Find(NodeUnit->GetTargetIdentityKey()) : nullptr;
							const auto* NodeRead = NodeReadIndex ? &Requests[*NodeReadIndex] : nullptr;
							const int32* PropertyReadIndex = ReadsBySnapshot.Find(Unit->GetTargetIdentityKey());
							const auto* PropertyRead = PropertyReadIndex ? &Requests[*PropertyReadIndex] : nullptr;
							FGuid NodeGuid;
							if (NodeRead && PropertyRead
								&& NodeRead->TargetIdentity.GetSelector(TEXT("graph_guid")) == Read->TargetIdentity.GetSelector(TEXT("graph_guid"))
								&& PropertyRead->SnapshotIdentityOverride.GetValue().GetSelector(TEXT("graph_guid")) == Read->TargetIdentity.GetSelector(TEXT("graph_guid"))
								&& PropertyRead->SnapshotIdentityOverride.GetValue().GetSelector(TEXT("node_guid")) == NodeRead->TargetIdentity.GetSelector(TEXT("node_guid"))
								&& FGuid::Parse(NodeRead->TargetIdentity.GetSelector(TEXT("node_guid")), NodeGuid))
							{
								const auto* Guids = NodeGuidsByGraph.Find(NativeGraph);
								bExistingNodeProperty = Guids && Guids->Contains(NodeGuid);
							}
						}
						// This animation owner delegates ordinary nodes and pose connections in
						// existing animation graphs to the shared native-schema mutation path.
						// State-machine topology remains on its dedicated animation surface.
						if (!GraphUnit || (!bExistingNodeProperty && Read && (!NativeGraph || !NativeGraph->GetSchema() ||
							!NativeGraph->GetSchema()->IsA<UEdGraphSchema_K2>())))
						{
							auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_blueprint_animation_surface_requires_domain_owner"),
								!GraphUnit ? TEXT("The changed animation surface has no owning graph in the semantic model.")
									: TEXT("This animation surface requires its dedicated domain owner; it cannot be applied through the Blueprint K2 graph path."), TEXT("document_restore"));
							Detail.TargetPath = TargetPackage;
							Detail.ActualValue = MakeShared<FJsonValueString>(Semantic);
							ProjectionError.SetFailure(MoveTemp(Detail), UnitId);
							break;
						}
						if (Unit->GetUnitKind() == ESemanticUnitKind::LayoutState &&
							Projection.ChangedUnitIds.Contains(Unit->GetOwnerRef())) continue;
						if (Semantic != UeAgentBlueprintAuthoring::GraphSemanticId && Semantic != UeAgentBlueprintAuthoring::NodeSemanticId &&
							Semantic != UeAgentBlueprintAuthoring::PinDefaultSemanticId && Semantic != UeAgentBlueprintAuthoring::NodePropertySemanticId &&
							Semantic != UeAgentBlueprintAuthoring::EdgeSemanticId)
						{
							auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_blueprint_projection_surface_not_supported"),
								TEXT("The changed surface is not supported by the animation Blueprint graph projection."), TEXT("document_restore"));
							Detail.TargetPath = TargetPackage;
							Detail.ActualValue = MakeShared<FJsonValueString>(Semantic);
							ProjectionError.SetFailure(MoveTemp(Detail), UnitId);
							break;
						}
						BlueprintUnitIds.Add(UnitId);
					}
					if (!ProjectionError.GetCode().IsEmpty() ||
						!UeAgentBlueprintAuthoring::TryProjectSemanticProviderExtension(Desired, BlueprintUnitIds,
							Projection.PropertyChangesByUnitId, Projection.StructuralEdits, BlueprintSurfaces, ProjectionError,
							&Prepared.GetWorkspace().Get(), &CurrentModel.Get()) ||
						!UE::UAI::DomainAdapters::Blueprint::Surfaces::TryNormalizeBlueprintCandidateBindings(
							TargetPackage, BlueprintSurfaces, ProjectionError))
					{
						OutOutcome.Diagnostic = ProjectionError.GetDiagnostic();
						OutOutcome.Phase = TEXT("prepare");
						return false;
					}
					}
					UeAgentAnimationAuthoring::FAssetApplyRequest ApplyRequest;
					ApplyRequest.Kind = AssetKind;
					ApplyRequest.Operation =
						UeAgentApplyInputContract::EOperation::Update;
					ApplyRequest.AssetPath = TargetPackage;
					ApplyRequest.Properties = MoveTemp(Projection.RootObjectProperties);
					ApplyRequest.Document = MoveTemp(ClassSettingsDocument);
					ApplyRequest.BlueprintSurfaces = MoveTemp(BlueprintSurfaces);
					ApplyRequest.bCompile = true;
					ApplyRequest.bSave = true;
					ApplyRequest.FinalReadback = OutOutcome.FinalReadback;
					UeAgentAnimationAuthoring::FAssetApplyResult ApplyResult;
					const bool bApplied = UeAgentAnimationAuthoring::ApplyAsset(
						ApplyRequest, ApplyResult, RequestId);
					OutOutcome.bApplied = bApplied && ApplyResult.IsApplied();
					OutOutcome.Phase = MoveTemp(ApplyResult.Phase);
					OutOutcome.Diagnostic = MoveTemp(ApplyResult.Diagnostic);
					ApplyResult.Asset = nullptr;
					Blueprint = nullptr;
					return bApplied;
				},
				[&Target](FString& OutReadbackPayload, FString& OutRevision,
					FWorkspaceError& OutReadError)
				{
					return TryRead(
						Target, OutReadbackPayload, OutRevision, OutReadError);
				},
				OutPayload, OutError);
		}

		bool TryImport(
			const FString& Target,
			const TSharedPtr<FJsonObject>& Params,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			FString CanonicalParamsJson;
			if (!TryCanonicalizeObject(Params, CanonicalParamsJson, OutError))
			{
				return false;
			}
			FResolvedDomainHjsonDocument Resolved;
			if (!TryDecodeResolvedDocument(
					Target,
					CanonicalParamsJson,
					FDomainHjsonDocumentContract(
						TEXT("animation_blueprint"), SchemaId, RootType),
					Resolved,
					OutError))
			{
				return false;
			}
			if (Resolved.bSemanticWorkspaceDirectory)
			{
				UE::UAI::HjsonWorkspace::DomainAuthoring::FScopedDomainHjsonDiagnosticSource DiagnosticSource(Resolved, OutError);
				return TryImportSemanticWorkspace(
					Target, Resolved, OutPayload, OutError);
			}
			UE::UAI::HjsonWorkspace::DomainAuthoring::FScopedDomainHjsonDiagnosticSource DiagnosticSource(Resolved, OutError, TEXT("/document"));
			UeAgentAuthoringDiagnostics::FScopedSourceLocations InputSources(Resolved.Root.Get(),
				[&Resolved](const FString& Pointer)
				{
					return Resolved.ResolveSource(TEXT("/root") + Pointer);
				});
			const TSharedPtr<FJsonObject>* DocumentField = nullptr;
			if (!Resolved.Root.IsValid()
				|| !Resolved.Root->TryGetObjectField(
					TEXT("document"), DocumentField)
				|| !DocumentField || !DocumentField->IsValid())
			{
				return Fail(OutError, InputFailure(TEXT("animation_blueprint_document_required"),
					TEXT("The animation Blueprint HJSON root requires a document object."), Resolved.Root.Get(),
					TEXT("document"), TEXT("object"), TEXT("/root/document")));
			}
			const TSharedPtr<FJsonObject> Document = *DocumentField;
			const FString& ExpectedRevision = Resolved.ExpectedRevision;
			FString Operation;
			const TSharedPtr<FJsonObject>* DocumentTarget = nullptr;
			FString AssetPath;
			if (!Document->TryGetStringField(TEXT("operation"), Operation))
			{
				return Fail(OutError, InputFailure(TEXT("animation_blueprint_document_identity_required"),
					TEXT("The document requires a string operation."), Document.Get(), TEXT("operation"), TEXT("string")));
			}
			if (!Document->TryGetObjectField(TEXT("target"), DocumentTarget) || !DocumentTarget || !DocumentTarget->IsValid())
			{
				return Fail(OutError, InputFailure(TEXT("animation_blueprint_document_identity_required"),
					TEXT("The document requires a target object."), Document.Get(), TEXT("target"), TEXT("object")));
			}
			if (!(*DocumentTarget)->TryGetStringField(TEXT("asset_path"), AssetPath))
			{
				return Fail(OutError, InputFailure(TEXT("animation_blueprint_document_identity_required"),
					TEXT("The document target requires a string asset_path."), DocumentTarget->Get(), TEXT("asset_path"), TEXT("string")));
			}
			Operation.TrimStartAndEndInline();
			Operation.ToLowerInline();
			if (Operation != TEXT("create"))
			{
				return Fail(OutError, InputFailure(TEXT("animation_blueprint_operation_unsupported"),
					TEXT("The single-document animation Blueprint import supports only create."),
					Document.Get(), TEXT("operation"), TEXT("create")));
			}
			const FString TargetPackage = PackagePath(Target);
			if (PackagePath(AssetPath) != TargetPackage)
			{
				return Fail(OutError, InputFailure(TEXT("animation_blueprint_target_identity_mismatch"),
					TEXT("The document asset path differs from the requested import target."),
					DocumentTarget->Get(), TEXT("asset_path"), TargetPackage));
			}
			UObject* Existing = StaticLoadObject(
				UObject::StaticClass(), nullptr, *ObjectPath(Target));
			if (Existing)
			{
				auto Diagnostic = InputFailure(Cast<UAnimBlueprint>(Existing)
					? TEXT("animation_blueprint_target_already_exists") : TEXT("animation_blueprint_target_type_unsupported"),
					Cast<UAnimBlueprint>(Existing) ? TEXT("The create target already contains an animation Blueprint.")
						: TEXT("The create target is occupied by an asset of another type."),
					DocumentTarget->Get(), TEXT("asset_path"), TEXT("unoccupied asset path"));
				Diagnostic.SubjectId = Existing->GetClass()->GetName();
				return Fail(OutError, MoveTemp(Diagnostic));
			}
			if (ExpectedRevision != TEXT("missing"))
			{
				auto Diagnostic = FDiagnostic::Failure(TEXT("animation_blueprint_stale_revision"),
					TEXT("The supplied revision does not match the missing target required for creation."), TEXT("animation_blueprint"));
				Diagnostic.SourceKind = TEXT("command_input");
				Diagnostic.Source.JsonPath = TEXT("/expected_revision");
				Diagnostic.PropertyPath = TEXT("expected_revision");
				Diagnostic.ActualValue = MakeShared<FJsonValueString>(TEXT("missing"));
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(ExpectedRevision);
				return Fail(OutError, MoveTemp(Diagnostic));
			}

			const TSharedPtr<FJsonObject>* ExtensionPtr = nullptr;
			if (!Document->TryGetObjectField(
				TEXT("provider_extension"), ExtensionPtr)
				|| !ExtensionPtr || !ExtensionPtr->IsValid())
			{
				return Fail(OutError, InputFailure(TEXT("animation_blueprint_provider_extension_required"),
					TEXT("The animation Blueprint document requires a provider_extension object."),
					Document.Get(), TEXT("provider_extension"), TEXT("object")));
			}
			const TSharedPtr<FJsonObject> Extension = *ExtensionPtr;
			if (!ValidateCreationFields(*Extension, {{TEXT("class_settings"), EJson::Object},
				{TEXT("blueprint_surfaces"), EJson::Object}, {TEXT("animation_graphs"), EJson::Array},
				{TEXT("state_machines"), EJson::Array}}, TEXT("/provider_extension"), OutError)) return false;
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
			UE::UAI::Contract::FDiagnostic NativeError;
			const TSharedPtr<FJsonObject> ScratchSeedDocument =
				MakeShared<FJsonObject>();
			ScratchSeedDocument->SetObjectField(TEXT("settings"), Settings);
			UeAgentAnimationAuthoring::FFactorySeedDiagnosticSource SeedSource;
			SeedSource.TargetPath = Target;
			SeedSource.Settings.JsonPath = TEXT("/provider_extension/class_settings");
			UObject* ScratchObject =
				UeAgentAnimationAuthoring::CreateFactorySeed(
					UeAgentAnimationAuthoring::EAssetKind::AnimBlueprint,
					GetTransientPackage(),
					*FString(TEXT("ABP_T16_Preflight_")
						+ FGuid::NewGuid().ToString(EGuidFormats::Digits)),
					RF_Transient,
					ScratchSeedDocument,
					NativeError, SeedSource);
			TStrongObjectPtr<UAnimBlueprint> Scratch(
				Cast<UAnimBlueprint>(ScratchObject));
			if (!Scratch.IsValid())
			{
				if (NativeError.IsEmpty())
					NativeError = NativeFailure(TEXT("animation_blueprint_preflight_seed_failed"),
						TEXT("The factory did not create an animation Blueprint for preflight."), Target);
				return Fail(OutError, NativeError, Target, TEXT("/"));
			}
			UeAgentBlueprintAssetLifecycle::SetAuthoringContext(*Scratch, Target, nullptr);
			TUniquePtr<UeAgentBlueprintAuthoring::FPreparedBlueprintMutation>
				ScratchMutation;
			UeAgentBlueprintAuthoring::FMutationCounters ScratchExpected;
			if (!UeAgentBlueprintAuthoring::FPreparedBlueprintMutation::Prepare(
				*Scratch, BlueprintSurfaces, ScratchMutation,
				ScratchExpected, NativeError))
			{
				if (!NativeError.IsEmpty()) UeAgentAuthoringDiagnostics::PrefixDocumentFailure(NativeError, TEXT("/provider_extension/blueprint_surfaces"));
				RetireScratch(Scratch);
				if (NativeError.IsEmpty())
					NativeError = NativeFailure(TEXT("animation_blueprint_preflight_prepare_failed"),
						TEXT("Blueprint surface preparation failed without returning a diagnostic."), Target);
				return Fail(OutError, NativeError, Target, TEXT("/"));
			}
			int32 ScratchGraphs = 0;
			int32 ScratchMachines = 0;
			int32 ScratchStates = 0;
			int32 ScratchTransitions = 0;
			FAnimationSurfaceIntent ScratchIntent;
			const bool bSurfacesApplied = ApplyAnimationSurfaces(
				*Scratch, Extension, OutError,
				ScratchGraphs, ScratchMachines,
				ScratchStates, ScratchTransitions, ScratchIntent);
			const bool bFinalized = bSurfacesApplied && UeAgentAnimBlueprintOps::FinalizeAnimBlueprintStructureMutation(
				Scratch.Get(), true, false, NativeError);
			const bool bCompiled = bFinalized && Scratch->Status != BS_Error;
			const bool bVerified = bCompiled && ScratchIntent.Verify(*Scratch, OutError);
			if (!bVerified)
			{
				RetireScratch(Scratch);
				if (OutError.GetCode().IsEmpty())
				{
					if (NativeError.IsEmpty())
					{
						NativeError = NativeFailure(TEXT("animation_blueprint_preflight_failed"),
							!bSurfacesApplied ? TEXT("Animation surface preflight failed without returning a diagnostic.")
							: !bFinalized ? TEXT("Animation Blueprint finalization failed without returning a diagnostic.")
							: !bCompiled ? TEXT("The preflight animation Blueprint has compiler errors.")
							: TEXT("Animation surface intent verification failed without returning a diagnostic."), Target);
						NativeError.PropertyPath = !bSurfacesApplied ? TEXT("surface_preflight")
							: !bFinalized ? TEXT("finalization") : !bCompiled ? TEXT("compile_success") : TEXT("intent_verification");
						NativeError.ActualValue = MakeShared<FJsonValueBoolean>(false);
						NativeError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					}
					Fail(OutError, NativeError, Target, TEXT("/"));
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
				if (ApplyResult.Diagnostic.IsEmpty())
				{
					ApplyResult.Diagnostic = NativeFailure(TEXT("animation_blueprint_apply_failed"),
						TEXT("The animation Blueprint asset apply failed without returning a diagnostic."), TargetPackage);
					ApplyResult.Diagnostic.Stage = ApplyResult.Phase;
				}
				return Fail(OutError, MoveTemp(ApplyResult.Diagnostic), TargetPackage);
			}

			UAnimBlueprint* Applied = Cast<UAnimBlueprint>(ApplyResult.Asset);
			FDiagnostic RollbackDiagnostic;
			auto Rollback = [&Applied, &ApplyResult, &Target, &TargetPackage, &RollbackDiagnostic]()
			{
				ApplyResult.Asset = nullptr;
				if (!Applied) Applied = LoadAnimationBlueprint(Target);
				if (!Applied && FPackageName::DoesPackageExist(TargetPackage))
				{
					RollbackDiagnostic = NativeFailure(TEXT("animation_blueprint_rollback_failed"),
						TEXT("The created package still exists, but its animation Blueprint cannot be loaded for rollback."), Target);
					RollbackDiagnostic.Stage = TEXT("rollback");
					RollbackDiagnostic.ActualValue = MakeShared<FJsonValueNull>();
					RollbackDiagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("AnimBlueprint"));
					return false;
				}
				const bool bDeleted = DeleteCreated(Applied, Target, RollbackDiagnostic);
				Applied = nullptr;
				return bDeleted;
			};
			auto RecoverFailure = [&]()
			{
				auto Diagnostic = OutError.GetDiagnostic();
				if (!Diagnostic.IsValid()) Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_blueprint_apply_failed"),
					TEXT("The animation Blueprint import did not complete."), TEXT("authoring_apply"));
				SupplementTarget(Diagnostic, Target);
				const bool bRestored = Rollback();
				if (!bRestored)
				{
					Diagnostic.Append(MoveTemp(RollbackDiagnostic));
				}
				UeAgentAuthoringDiagnostics::SetRollbackFacts(Diagnostic, bRestored);
				Diagnostic.TerminalStatus = bRestored ? TEXT("rolled_back") : TEXT("rollback_unconfirmed");
				Diagnostic.MutationStarted = true;
				OutError.SetFailure(MoveTemp(Diagnostic));
				return false;
			};
			if (!Applied)
			{
				auto Diagnostic = NativeFailure(TEXT("animation_blueprint_persisted_asset_missing"),
					TEXT("The asset apply reported success but returned no animation Blueprint."), Target);
				if (ApplyResult.Asset) Diagnostic.ActualValue = MakeShared<FJsonValueString>(ApplyResult.Asset->GetClass()->GetName());
				else Diagnostic.ActualValue = MakeShared<FJsonValueNull>();
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("AnimBlueprint"));
				Fail(OutError, MoveTemp(Diagnostic));
				return RecoverFailure();
			}

			TUniquePtr<UeAgentBlueprintAuthoring::FPreparedBlueprintMutation>
				BlueprintMutation;
			UeAgentBlueprintAuthoring::FMutationCounters ExpectedCounters;
			if (!UeAgentBlueprintAuthoring::FPreparedBlueprintMutation::Prepare(
				*Applied, BlueprintSurfaces, BlueprintMutation,
				ExpectedCounters, NativeError))
			{
				if (!NativeError.IsEmpty()) UeAgentAuthoringDiagnostics::PrefixDocumentFailure(NativeError, TEXT("/provider_extension/blueprint_surfaces"));
				Fail(OutError, NativeError, Target, TEXT("/"));
				return RecoverFailure();
			}
			int32 Graphs = 0;
			int32 Machines = 0;
			int32 States = 0;
			int32 Transitions = 0;
			FAnimationSurfaceIntent SurfaceIntent;
			if (!ApplyAnimationSurfaces(
				*Applied, Extension, OutError,
				Graphs, Machines, States, Transitions, SurfaceIntent))
			{
				return RecoverFailure();
			}
			if (!UeAgentAnimBlueprintOps::FinalizeAnimBlueprintStructureMutation(
				Applied, true, false, NativeError))
			{
				Fail(OutError, NativeError, Target, TEXT("/"));
				return RecoverFailure();
			}
			UeAgentBlueprintAuthoring::FMutationCounters AppliedCounters;
			const bool bDefaultsFinalized = BlueprintMutation->FinalizeAfterCompile(
				*Applied, AppliedCounters, NativeError);
			if (!bDefaultsFinalized && !NativeError.IsEmpty())
				UeAgentAuthoringDiagnostics::PrefixDocumentFailure(NativeError, TEXT("/provider_extension/blueprint_surfaces"));
			if (!bDefaultsFinalized || !UeAgentAnimationAuthoring::CompileAsset(
					UeAgentAnimationAuthoring::EAssetKind::AnimBlueprint,
					*Applied, NativeError)
				|| !UeAgentAnimationAuthoring::SaveAsset(
					UeAgentAnimationAuthoring::EAssetKind::AnimBlueprint,
					*Applied, NativeError))
			{
				Fail(OutError, NativeError, Target, TEXT("/"));
				return RecoverFailure();
			}

			UPackage* Package = Applied->GetOutermost();
			ApplyResult.Asset = nullptr;
			Applied = nullptr;
			FText UnloadError;
			if (!Package
				|| !UPackageTools::UnloadPackages({Package}, UnloadError, true))
			{
				auto Diagnostic = NativeFailure(TEXT("animation_blueprint_unload_failed"),
					!Package ? TEXT("The applied animation Blueprint has no package to unload.")
						: !UnloadError.IsEmpty() ? UnloadError.ToString() : TEXT("The editor did not unload the animation Blueprint package."), Target);
				Diagnostic.Stage = TEXT("reload");
				Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
				Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				Fail(OutError, MoveTemp(Diagnostic));
				return RecoverFailure();
			}
			UAnimBlueprint* Reloaded = LoadAnimationBlueprint(Target);
			if (!Reloaded || Reloaded->Status == BS_Error)
			{
				auto Diagnostic = NativeFailure(TEXT("animation_blueprint_reload_failed"),
					Reloaded ? TEXT("The reloaded animation Blueprint has compiler errors.")
						: TEXT("The animation Blueprint could not be loaded after persistence."), Target);
				Diagnostic.Stage = TEXT("reload");
				Diagnostic.PropertyPath = Reloaded ? TEXT("compile_success") : TEXT("asset_present");
				Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
				Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				Fail(OutError, MoveTemp(Diagnostic));
				return RecoverFailure();
			}
			if (!BlueprintMutation->VerifyIntent(*Reloaded, NativeError))
			{
				if (!NativeError.IsEmpty()) UeAgentAuthoringDiagnostics::PrefixDocumentFailure(NativeError, TEXT("/provider_extension/blueprint_surfaces"));
				SupplementTarget(NativeError, Target);
				UeAgentAuthoringDiagnostics::ReportConsistencyWarning(FString(), NativeError, TEXT("animation_blueprint_observation"));
				NativeError.Reset();
			}
			if (!SurfaceIntent.Verify(*Reloaded, OutError))
			{
				auto Diagnostic = OutError.GetDiagnostic();
				SupplementTarget(Diagnostic, Target);
				UeAgentAuthoringDiagnostics::ReportConsistencyWarning(FString(), MoveTemp(Diagnostic), TEXT("animation_blueprint_observation"));
				OutError = FWorkspaceError();
			}
			FString ReadbackPayload;
			FString Revision;
			if (!TryRead(Target, ReadbackPayload, Revision, OutError)) return RecoverFailure();
			TSharedPtr<FJsonObject> Readback;
			UE::UAI::Contract::FDiagnostic JsonError;
			if (!Json::TryParseObject(
				ReadbackPayload, Readback, JsonError)
				|| !Readback.IsValid())
			{
				Fail(OutError, JsonError, Target, TEXT("/"));
				return RecoverFailure();
			}
			if (!TryValidateReadbackDiagnostics(Readback, Target,
				TEXT("animation_blueprint_compile_not_settled"), OutError))
				return RecoverFailure();

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

			FDomainHjsonImportLifecycleReceipt LifecycleReceipt;
			LifecycleReceipt.bMutationApplied = true;
			LifecycleReceipt.bNativeUpdateSettled = true;
			LifecycleReceipt.bPackageSaved = true;
			LifecycleReceipt.PersistenceReceiptId = TEXT("reload:")
				+ ObjectPath(Target) + TEXT(":") + Revision;
			return TryBuildImportSuccessPayload(
				TEXT("animation-blueprint-domain-provider"),
				Target,
				Revision,
				LifecycleReceipt,
				Readback,
				Mutation,
				OutPayload,
				OutError);
		}

		bool TryExport(
			const FString& Target,
			const TSharedPtr<FJsonObject>& Params,
			FString& OutPayload,
			FWorkspaceError& OutError,
			FPreparedWorkspaceExport* PreparedExport)
		{
			FString WorkspacePath;
			if (!Params->TryGetStringField(TEXT("workspace_path"), WorkspacePath)
				|| WorkspacePath.IsEmpty())
			{
				auto Diagnostic = InputFailure(TEXT("animation_blueprint_export_path_required"),
					TEXT("Export requires a non-empty workspace_path string."), Params.Get(), TEXT("workspace_path"), TEXT("non-empty string"));
				Diagnostic.SourceKind = TEXT("command_input");
				return Fail(OutError, MoveTemp(Diagnostic), Target);
			}
			if (!EnsureAuthoringRegistered(OutError)) return false;
			const UeAgentAnimationAuthoring::FProfile* Profile =
				UeAgentAnimationAuthoring::FindProfile(
					UeAgentAnimationAuthoring::EAssetKind::AnimBlueprint);
			UAnimBlueprint* Blueprint = LoadAnimationBlueprint(Target);
			if (!Profile || !Blueprint)
			{
				auto Diagnostic = NativeFailure(Profile ? TEXT("animation_blueprint_target_unresolved")
					: TEXT("animation_blueprint_semantic_profile_missing"),
					Profile ? TEXT("The export target could not be loaded as an animation Blueprint.")
						: TEXT("No animation Blueprint authoring profile is registered."), Target);
				Diagnostic.ActualValue = MakeShared<FJsonValueNull>();
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(Profile ? TEXT("AnimBlueprint") : TEXT("animation Blueprint authoring profile"));
				return Fail(OutError, MoveTemp(Diagnostic));
			}
			UeAgentExportWorkspaceWriter::FWorkspaceResult ExportResult;
			FString ExportError;
			if (!UeAgentAnimationAuthoring::ExportAssetWorkspace(*Profile, *Blueprint, WorkspacePath, TEXT("llm_authoring"), ExportResult, PreparedExport))
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
		const FString& CanonicalParamsJson,
		FString& OutPayloadJson,
		bool& bOutReadOnly,
		FWorkspaceError& OutError,
		FPreparedWorkspaceExport* PreparedExport)
	{
		OutPayloadJson.Reset();
		bOutReadOnly = false;
		OutError.Reset();
		auto ReturnResult = [&](const bool bSucceeded)
		{
			if (!bSucceeded)
			{
				auto Diagnostic = OutError.GetDiagnostic();
				SupplementTarget(Diagnostic, TargetIdentity);
				OutError.SetFailure(MoveTemp(Diagnostic));
			}
			return bSucceeded;
		};
		TSharedPtr<FJsonObject> Params;
		UE::UAI::Contract::FDiagnostic JsonError;
		if (!Json::TryParseObject(CanonicalParamsJson, Params, JsonError)
			|| !Params.IsValid())
		{
			return ReturnResult(Fail(OutError, JsonError, FString(), TEXT("/")));
		}
		FString Target;
		if (!Params->TryGetStringField(TEXT("target"), Target)
			|| ObjectPath(Target) != ObjectPath(TargetIdentity))
		{
			auto Diagnostic = InputFailure(TEXT("animation_blueprint_target_identity_mismatch"),
				TEXT("The command target does not match the selected content graph target."), Params.Get(), TEXT("target"), TargetIdentity);
			Diagnostic.SourceKind = TEXT("command_input");
			return ReturnResult(Fail(OutError, MoveTemp(Diagnostic)));
		}
		if (CommandId == TEXT("content_graph.import_workspace"))
		{
			return ReturnResult(TryImport(Target, Params, OutPayloadJson, OutError));
		}
		if (CommandId == TEXT("content_graph.export_workspace"))
		{
			bOutReadOnly = true;
			return ReturnResult(TryExport(Target, Params, OutPayloadJson, OutError, PreparedExport));
		}
		if (CommandId.StartsWith(TEXT("content_graph.")))
		{
			FString Revision;
			bOutReadOnly = true;
			return ReturnResult(TryRead(Target, OutPayloadJson, Revision, OutError));
		}
		auto Diagnostic = FDiagnostic::Failure(TEXT("animation_blueprint_command_not_supported"),
			TEXT("This animation Blueprint adapter handles content_graph commands only."), TEXT("animation_blueprint"));
		Diagnostic.SourceKind = TEXT("command_input");
		Diagnostic.PropertyPath = TEXT("command");
		Diagnostic.ActualValue = MakeShared<FJsonValueString>(CommandId);
		Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("content_graph.*"));
		return ReturnResult(Fail(OutError, MoveTemp(Diagnostic)));
	}
}
