#include "QueryOwners/AnimationRigGraph/UeAgentAnimationRigGraphCandidateQuery.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Domains/Rigging/Authoring/UeAgentRigVMNodeAuthoring.h"
#include "Core/ContentDiscovery/UeAgentReadOnlyCandidateBatch.h"
#include "Core/ContentDiscovery/UeAgentNativeNodeAction.h"
#include "Core/ContentDiscovery/UeAgentNativeTypeDiscovery.h"
#include "AssetRegistry/AssetData.h"
#include "Engine/UserDefinedEnum.h"
#include "Templates/UniquePtr.h"

#include "BlueprintActionDatabase.h"
#include "BlueprintActionFilter.h"
#include "BlueprintNodeSignature.h"
#include "BlueprintNodeSpawner.h"
#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "ControlRigBlueprint.h"
#include "Core/ContentDiscovery/UeAgentContentDiscovery.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "EdGraph/NodeSpawners/RigVMEdGraphTemplateNodeSpawner.h"
#include "EdGraph/NodeSpawners/RigVMEdGraphUnitNodeSpawner.h"
#include "EdGraph/NodeSpawners/RigVMEdGraphVariableNodeSpawner.h"
#include "EdGraph/NodeSpawners/RigVMEdGraphEnumNodeSpawner.h"
#include "EdGraph/RigVMEdGraph.h"
#include "EdGraph/RigVMEdGraphNode.h"
#include "Editor/RigVMActionMenuBuilder.h"
#include "Editor/RigVMActionMenuItem.h"
#include "Editor/RigVMActionMenuUtils.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMFunctionLibrary.h"
#include "RigVMModel/Nodes/RigVMLibraryNode.h"
#include "RigVMHost.h"
#include "RigVMBlueprintGeneratedClass.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTargetLocator.h"

#include <openssl/sha.h>
#include "RigVMCore/RigVMGraphFunctionDefinition.h"

namespace UE::UAI::QueryOwners::AnimationRigGraph
{
	using namespace DomainAdapters::ContentGraphRead;

	namespace
	{
		const FString CandidateKind = TEXT("rigvm_action");

		struct FNativeActionEntry final
		{
			TSharedPtr<FEdGraphSchemaAction> Action;
			FString Signature;
			UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
			FAnimationRigGraphCandidate Candidate;
		};

		struct FNativeActionCatalog final
		{
			URigVMEdGraph* EditorGraph = nullptr;
			TArray<FNativeActionEntry> Entries;
			TArray<TStrongObjectPtr<UBlueprintNodeSpawner>> OwnedSpawners;
			TArray<TStrongObjectPtr<UEnum>> LoadedEnums;
		};

		bool ForwardDiagnostic(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& Subject = FString())
		{
			OutError = MoveTemp(Diagnostic);
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject;
			return false;
		}

		UE::UAI::Contract::FDiagnostic CandidateDiagnostic(FString Code, FString Subject, FString PublicReason, FString InternalContext = FString())
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(MoveTemp(Code), MoveTemp(PublicReason), TEXT("candidate_query"), MoveTemp(InternalContext), MoveTemp(Subject));
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			Diagnostic.SourceKind = TEXT("native_result");
			return Diagnostic;
		}

		UE::UAI::Contract::FDiagnostic CandidateCause(FString Code, const FString& Subject, const UE::UAI::Contract::FDiagnostic& Cause, const FString& FallbackReason)
		{
			auto Diagnostic = Cause.IsEmpty() ? CandidateDiagnostic(MoveTemp(Code), Subject, FallbackReason)
				: Contract::FDiagnostic::WrapFailure(MoveTemp(Code), Cause);
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = Subject;
			return Diagnostic;
		}

		Contract::FDiagnostic ThreadFailure()
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("animation_rig_graph_candidate_game_thread_required"), FString(),
				TEXT("Native RigVM candidate discovery and creation require the game thread."));
			Diagnostic.ActualValue = MakeShared<FJsonValueString>(TEXT("other_thread"));
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("game_thread"));
			return Diagnostic;
		}

		Contract::FDiagnostic IndexFailure(const FString& Name, int32 Index, int32 Count)
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("animation_rig_graph_candidate_not_found"), Name,
				TEXT("The RigVM candidate resolver returned an index outside its native catalog."));
			Diagnostic.CandidateName = Name;
			auto Actual = MakeShared<FJsonObject>();
			Actual->SetNumberField(TEXT("resolved_index"), Index);
			Actual->SetNumberField(TEXT("candidate_count"), Count);
			Diagnostic.ActualValue = MakeShared<FJsonValueObject>(Actual);
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("index within the native candidate catalog"));
			return Diagnostic;
		}

		FString Sha256(const FString& Text)
		{
			const FTCHARToUTF8 Utf8(*Text);
			uint8 Digest[SHA256_DIGEST_LENGTH];
			SHA256(
				reinterpret_cast<const uint8*>(Utf8.Get()),
				Utf8.Length(),
				Digest);
			FString Hex;
			for (const uint8 Byte : Digest)
			{
				Hex += FString::Printf(TEXT("%02x"), Byte);
			}
			return TEXT("sha256:") + Hex;
		}

		bool ReadTargetRevision(
			UControlRigBlueprint& Target,
			FString& OutRevision,
			FAnimationRigGraphCandidateQueryError& OutError)
		{
			if (Target.GetOutermost() == GetTransientPackage())
			{
				OutRevision = Sha256(
					TEXT("transient|") + Target.GetClass()->GetPathName());
				return true;
			}
			FUeAgentContentGraphCapture Capture;
			FUeAgentContentGraphReadError ReadError;
			const bool bCaptured = FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(
					Target, Capture, ReadError);
			if (bCaptured
				&& !Capture.Revision.IsEmpty())
			{
				OutRevision = Capture.Revision;
				return true;
			}
			const UPackage* const Package = Target.GetOutermost();
			if (Package && Package != GetTransientPackage())
			{
				OutRevision = FString::Printf(
					TEXT("%s:%s:%s"),
					*Package->GetPersistentGuid().ToString(EGuidFormats::Digits),
					Package->IsDirty() ? TEXT("dirty") : TEXT("clean"),
					*Sha256(Target.GetClass()->GetPathName()
						+ TEXT("|") + Target.GetPathName()));
				return true;
			}
			auto Diagnostic = CandidateCause(TEXT("animation_rig_graph_candidate_target_readback_failed"), Target.GetPathName(), ReadError,
				bCaptured ? TEXT("The Control Rig capture returned no revision and no persistent package revision is available.")
					: TEXT("The Control Rig capture failed and no persistent package revision is available."));
			if (ReadError.IsEmpty())
			{
				auto Actual = MakeShared<FJsonObject>();
				Actual->SetBoolField(TEXT("capture_succeeded"), bCaptured);
				Actual->SetBoolField(TEXT("revision_present"), !Capture.Revision.IsEmpty());
				Actual->SetBoolField(TEXT("package_present"), Package != nullptr);
				Diagnostic.ActualValue = MakeShared<FJsonValueObject>(Actual);
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("captured or persistent-package revision"));
			}
			return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
		}

		bool TryPrepareNativeContext(
			UControlRigBlueprint& Target,
			URigVMEdGraph*& OutEditorGraph,
			FAnimationRigGraphCandidateQueryError& OutError)
		{
			OutEditorGraph = nullptr;
			URigVMGraph* const Model = Target.GetDefaultModel();
			URigVMEdGraph* const EditorGraph = Model
				? Cast<URigVMEdGraph>(Target.GetEdGraph(Model))
				: nullptr;
			if (!Model || !EditorGraph)
			{
				auto Diagnostic = CandidateDiagnostic(TEXT("animation_rig_graph_candidate_context_unavailable"), Target.GetPathName(),
					!Model ? TEXT("The Control Rig blueprint has no default RigVM model.") : TEXT("The default RigVM model has no RigVM editor graph."));
				auto Actual = MakeShared<FJsonObject>();
				Actual->SetBoolField(TEXT("default_model_present"), Model != nullptr);
				Actual->SetBoolField(TEXT("editor_graph_present"), EditorGraph != nullptr);
				Diagnostic.ActualValue = MakeShared<FJsonValueObject>(Actual);
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("default RigVM model with a RigVM editor graph"));
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
			}

			// This is the same public delegate initialization used by the editor.
			// It does not open an asset window and is required by native spawners.
			EditorGraph->InitializeFromBlueprint(&Target);

			UClass* const ActionKey = Target.GetClass();
			FBlueprintActionDatabase& ActionDatabase =
				FBlueprintActionDatabase::Get();
			const FBlueprintActionDatabase::FActionRegistry& Registry =
				ActionDatabase.GetAllActions();
			if (!Registry.Contains(ActionKey)
				|| Registry.FindChecked(ActionKey).IsEmpty())
			{
				ActionDatabase.RefreshClassActions(ActionKey);
			}
			OutEditorGraph = EditorGraph;
			return true;
		}

		bool TryBuildSpawnerSemanticSignature(
			const FEdGraphSchemaAction& Action,
			const UBlueprintNodeSpawner& Spawner,
			FString& OutSignature,
			FAnimationRigGraphCandidateQueryError& OutError)
		{
			FString SpawnerIdentity;
			if (Spawner.IsA<URigVMEdGraphTemplateNodeSpawner>())
			{
				// UE 5.6 passes a delimiterless value to FBlueprintNodeSignature
				// for this spawner, so every template reports `(None="")`.
				// TemplateNotation is the native semantic input to Invoke(), but
				// the engine exposes no getter. Read only that reflected property;
				// never fall back to transient object names or menu ordering.
				const FNameProperty* const NotationProperty =
					FindFProperty<FNameProperty>(
						Spawner.GetClass(), TEXT("TemplateNotation"));
				const FName Notation = NotationProperty
					? NotationProperty->GetPropertyValue_InContainer(&Spawner)
					: NAME_None;
				if (Notation.IsNone())
				{
					auto Diagnostic = CandidateDiagnostic(TEXT("animation_rig_graph_candidate_template_identity_unavailable"), Spawner.GetPathName(),
						!NotationProperty ? TEXT("The native RigVM template spawner has no template-notation property.")
							: TEXT("The native RigVM template spawner has an empty template notation."));
					Diagnostic.CandidateName = Action.GetMenuDescription().ToString();
					Diagnostic.PropertyPath = TEXT("template_notation");
					Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
				}
				SpawnerIdentity = TEXT("template_notation=")
					+ Notation.ToString();
			}
			else
			{
				const FBlueprintNodeSignature NativeSignature =
					Spawner.GetSpawnerSignature();
				if (!NativeSignature.IsValid())
				{
					auto Diagnostic = CandidateDiagnostic(TEXT("animation_rig_graph_candidate_spawner_identity_invalid"), Spawner.GetPathName(),
						TEXT("The native RigVM node spawner returned an invalid semantic signature."));
					Diagnostic.CandidateName = Action.GetMenuDescription().ToString();
					Diagnostic.PropertyPath = TEXT("signature_valid");
					Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
				}
				SpawnerIdentity = TEXT("spawner_signature=")
					+ NativeSignature.ToString();
			}

			OutSignature = TEXT("action_type=") + Action.GetTypeId().ToString()
				+ TEXT("|spawner_class=") + Spawner.GetClass()->GetPathName()
				+ TEXT("|") + SpawnerIdentity;
			return true;
		}

		bool BuildCandidate(
			const FEdGraphSchemaAction& Action,
			FAnimationRigGraphCandidate& OutCandidate,
			FString& OutSignature,
			UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity,
			FAnimationRigGraphCandidateQueryError& OutError,
			const FString& KnownSignature = FString(),
			const TMap<FString, FString>& KnownSelection = {})
		{
			if (Action.GetTypeId() != FRigVMActionMenuItem::StaticGetTypeId())
			{
				auto Diagnostic = CandidateDiagnostic(TEXT("animation_rig_graph_candidate_native_action_unsupported"), Action.GetTypeId().ToString(),
					TEXT("The native RigVM creation menu returned an action that does not provide a node spawner."));
				Diagnostic.CandidateName = Action.GetMenuDescription().ToString();
				Diagnostic.ActualValue = MakeShared<FJsonValueString>(Action.GetTypeId().ToString());
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(FRigVMActionMenuItem::StaticGetTypeId().ToString());
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
			}
			const FRigVMActionMenuItem& Item =
				static_cast<const FRigVMActionMenuItem&>(Action);
			const UBlueprintNodeSpawner* const Spawner = Item.GetRawAction();
			if (!Spawner)
			{
				auto Diagnostic = CandidateDiagnostic(TEXT("animation_rig_graph_candidate_spawner_identity_invalid"), Action.GetMenuDescription().ToString(),
					TEXT("The native RigVM menu action has no node spawner."));
				Diagnostic.CandidateName = Action.GetMenuDescription().ToString();
				Diagnostic.ActualValuePresent = false;
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("native RigVM node spawner"));
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
			}
			OutSignature = KnownSignature;
			if (OutSignature.IsEmpty() && !TryBuildSpawnerSemanticSignature(
					Action, *Spawner, OutSignature, OutError))
			{
				return false;
			}
			UeAgentPaletteSearch::FCandidate IdentityCandidate;
			IdentityCandidate.Kind = CandidateKind;
			IdentityCandidate.Name =
				Action.GetMenuDescription().ToString();
			IdentityCandidate.DisplayName = IdentityCandidate.Name;
			IdentityCandidate.CategoryPath = Action.GetCategory().ToString();
			IdentityCandidate.Tooltip =
				Action.GetTooltipDescription().ToString();
			IdentityCandidate.Keywords = Action.GetKeywords().ToString();
			IdentityCandidate.Type = Spawner->GetClass()->GetPathName();
			IdentityCandidate.FullName = OutSignature;
			UE::UAI::Contract::FDiagnostic IdentityError;
			if (IdentityCandidate.Name.IsEmpty())
			{
				auto Diagnostic = CandidateDiagnostic(TEXT("animation_rig_graph_candidate_identity_failed"), OutSignature,
					TEXT("The native RigVM menu action has no readable candidate name."));
				Diagnostic.ActualValue = MakeShared<FJsonValueString>(IdentityCandidate.Name);
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty readable candidate name"));
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
			}
			if (!UeAgentPaletteSearch::SetCandidateIdentity(
					TEXT("rigvm_action"),
					OutSignature,
					IdentityCandidate.Name,
					IdentityCandidate,
					IdentityError))
			{
				return ForwardDiagnostic(OutError, CandidateCause(TEXT("animation_rig_graph_candidate_identity_failed"), Action.GetMenuDescription().ToString(), IdentityError,
					TEXT("The native RigVM candidate could not be assigned an identity.")), Action.GetMenuDescription().ToString());
			}

			OutCandidate = {};
			OutIdentity = IdentityCandidate.Identity;
			OutCandidate.Id = IdentityCandidate.GetPublicId();
			OutCandidate.Name = IdentityCandidate.Name;
			OutCandidate.Label = IdentityCandidate.DisplayName;
			OutCandidate.Kind = CandidateKind;
			OutCandidate.ActionKind = IdentityCandidate.Type;
			OutCandidate.Category = IdentityCandidate.CategoryPath;
			OutCandidate.Summary = IdentityCandidate.Tooltip;
			OutCandidate.bSupported = !UeAgentRigVMNodeAuthoring::GetSupportedNodeType(*Spawner).IsEmpty();
			const FString Form = UeAgentRigVMNodeAuthoring::GetSupportedNodeType(*Spawner);
			if (!Form.IsEmpty()) OutCandidate.SelectionFields.Add(TEXT("form"), Form);
			// Read the native semantic inputs without invoking a spawner or creating a node.
			if (const FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(
				Spawner->GetClass(), TEXT("StructTemplate")))
			{
				if (const UScriptStruct* Struct = Cast<UScriptStruct>(Property->GetObjectPropertyValue_InContainer(Spawner)))
				{
					OutCandidate.SelectionFields.Add(TEXT("unit"), Struct->GetName());
					OutCandidate.SelectionFields.Add(TEXT("unit_source"), ContentDiscovery::MakeReadableDiscoveryPath(Struct->GetPathName()));
				}
			}
			for (const auto& Field : {TPair<FName, FString>(TEXT("MethodName"), TEXT("method")),
				TPair<FName, FString>(TEXT("EntryName"), TEXT("entry")),
				TPair<FName, FString>(TEXT("TemplateNotation"), TEXT("template"))})
			{
				if (const FNameProperty* Property = FindFProperty<FNameProperty>(Spawner->GetClass(), Field.Key))
				{
					const FName Value = Property->GetPropertyValue_InContainer(Spawner);
					if (!Value.IsNone()) OutCandidate.SelectionFields.Add(Field.Value, Value.ToString());
				}
			}
			if (const FStructProperty* Property = FindFProperty<FStructProperty>(
				Spawner->GetClass(), TEXT("ReferencedPublicFunctionHeader"));
				Property && Property->Struct == FRigVMGraphFunctionHeader::StaticStruct())
			{
				const auto* Header = Property->ContainerPtrToValuePtr<FRigVMGraphFunctionHeader>(Spawner);
				if (!Header->Name.IsNone()) OutCandidate.SelectionFields.Add(TEXT("function"), Header->Name.ToString());
				const FString Library = Header->LibraryPointer.HostObject.GetAssetName();
				if (!Library.IsEmpty()) OutCandidate.SelectionFields.Add(TEXT("library"), Library);
				const FString Source = ContentDiscovery::MakeReadableDiscoveryPath(Header->LibraryPointer.HostObject.ToString());
				if (!Source.IsEmpty()) OutCandidate.SelectionFields.Add(TEXT("library_source"), Source);
			}
			OutCandidate.WriteSupport = OutCandidate.bSupported
				? FString(TEXT("supported"))
				: FString(TEXT("search_only"));
			OutCandidate.SelectionFields.Append(KnownSelection);
			OutCandidate.SupportReason = OutCandidate.bSupported
				? FString(TEXT("native_rigvm_node_spawner"))
				: FString(TEXT("native_action_kind_not_yet_persisted"));
			if (!OutCandidate.bSupported)
			{
				OutCandidate.SupportDiagnostic = CandidateDiagnostic(TEXT("animation_rig_graph_candidate_action_kind_unsupported"), Spawner->GetPathName(),
					TEXT("This native RigVM action has no node form supported by workspace persistence."));
				OutCandidate.SupportDiagnostic.CandidateName = OutCandidate.Name;
				OutCandidate.SupportDiagnostic.ActualValue = MakeShared<FJsonValueString>(Spawner->GetClass()->GetName());
				OutCandidate.SupportDiagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("native spawner with a persistable RigVM node form"));
				OutCandidate.SupportDiagnostic.ActualEvidence = MakeShared<FJsonValueString>(OutCandidate.ActionKind);
			}
			if (OutCandidate.bSupported)
			{
				OutCandidate.SupportedOperations = {TEXT("create")};
			}
			return true;
		}

		bool TryBuildNativeCatalog(
			UControlRigBlueprint& Target,
			FNativeActionCatalog& OutCatalog,
			FAnimationRigGraphCandidateQueryError& OutError)
		{
			OutCatalog = {};
			if (!TryPrepareNativeContext(
					Target, OutCatalog.EditorGraph, OutError))
			{
				return false;
			}

			FBlueprintActionContext Context;
			Context.Blueprints.Add(&Target);
			Context.Graphs.Add(OutCatalog.EditorGraph);
			FRigVMActionMenuBuilder MenuBuilder;
			FRigVMActionMenuUtils::MakeContextMenu(
				Context, true, 0, MenuBuilder);

			FBlueprintActionFilter Filter(FBlueprintActionFilter::BPFILTER_NoFlags);
			Filter.Context = Context;
			Filter.Context.SelectedObjects.Empty();
			auto AddKnownAction = [&](URigVMEdGraphNodeSpawner* Spawner, UObject* Owner,
				const FString& Signature, const TMap<FString, FString>& Selection)
			{
				if (!Spawner)
				{
					auto Diagnostic = CandidateDiagnostic(TEXT("animation_rig_graph_candidate_native_action_invalid"), Signature,
						TEXT("Unreal did not create a RigVM spawner for the native declaration."));
					Diagnostic.ActualValuePresent = false;
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("native RigVM node spawner"));
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
				}
				TStrongObjectPtr<UBlueprintNodeSpawner> Owned(Spawner);
				Spawner->SetRelatedBlueprintClass(Target.GetClass());
				FBlueprintActionInfo Info(Owner, Spawner);
				if (Filter.IsFiltered(Info)) return true;
				Spawner->PrimeDefaultUiSpec(OutCatalog.EditorGraph);
				const auto Ui = Spawner->GetUiSpec(Context, IBlueprintNodeBinder::FBindingSet());
				FNativeActionEntry Entry;
				Entry.Action = MakeShared<FRigVMActionMenuItem>(Spawner, Ui, IBlueprintNodeBinder::FBindingSet(), Ui.Category);
				if (!BuildCandidate(*Entry.Action, Entry.Candidate, Entry.Signature, Entry.Identity, OutError, Signature, Selection)) return false;
				OutCatalog.OwnedSpawners.Add(MoveTemp(Owned));
				OutCatalog.Entries.Add(MoveTemp(Entry));
				return true;
			};
			for (int32 Index = 0; Index < MenuBuilder.GetNumActions(); ++Index)
			{
				const TSharedPtr<FEdGraphSchemaAction>& Action =
					MenuBuilder.GetSchemaAction(Index);
				if (!Action.IsValid())
				{
					auto Diagnostic = CandidateDiagnostic(TEXT("animation_rig_graph_candidate_native_action_invalid"), Target.GetPathName(),
						TEXT("The native RigVM menu returned an empty action."));
					Diagnostic.OperationIndex = Index;
					Diagnostic.ActualValuePresent = false;
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("native RigVM menu action"));
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
				}
				if (Action->GetTypeId() == FRigVMActionMenuItem::StaticGetTypeId())
				{
					const auto* Spawner = StaticCastSharedPtr<FRigVMActionMenuItem>(Action)->GetRawAction();
					// These UE signatures omit real creation inputs. Rebuild only their
					// records below from native declarations, using the same native spawners.
					if (Spawner && (Spawner->IsA<URigVMEdGraphVariableNodeSpawner>() || Spawner->IsA<URigVMEdGraphEnumNodeSpawner>())) continue;
				}
				FNativeActionEntry& Entry =
					OutCatalog.Entries.AddDefaulted_GetRef();
				Entry.Action = Action;
				if (!BuildCandidate(
						*Action,
						Entry.Candidate,
						Entry.Signature,
						Entry.Identity,
						OutError))
				{
					return false;
				}
			}
			TArray<FAssetData> EnumAssets;
			if (!ContentDiscovery::TryCollectTypeAssets(*UUserDefinedEnum::StaticClass(), EnumAssets, OutError)) return false;
			for (const FAssetData& Asset : EnumAssets)
			{
				UEnum* Enum = Cast<UEnum>(Asset.GetAsset());
				if (!Enum)
				{
					auto Diagnostic = CandidateDiagnostic(TEXT("animation_rig_graph_candidate_enum_load_failed"), Asset.GetObjectPathString(),
						TEXT("An enum discovered by the asset registry could not be loaded for native RigVM node discovery."));
					Diagnostic.ActualValue = MakeShared<FJsonValueString>(Asset.GetObjectPathString());
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("UserDefinedEnum asset"));
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
				}
				OutCatalog.LoadedEnums.Emplace(Enum);
			}
			for (TObjectIterator<UEnum> It; It; ++It)
			{
				UEnum* Enum = *It;
				if (Enum->HasMetaData(TEXT("Hidden")) || Enum->IsEditorOnly() || Enum->IsNative()) continue;
				const FText Name = FText::FromString(TEXT("Enum ") + Enum->GetName());
				const TMap<FString, FString> Selection{
					{TEXT("enum"), Enum->GetName()},
					{TEXT("enum_source"), ContentDiscovery::MakeReadableDiscoveryPath(Enum->GetPathName())}};
				if (!AddKnownAction(URigVMEdGraphEnumNodeSpawner::CreateForEnum(Enum, Name, FText::FromString(TEXT("Enum")), Name),
					Target.GetClass(), TEXT("enum|") + Enum->GetPathName(), Selection)) return false;
			}
			if (auto* GeneratedClass = Target.GetRigVMBlueprintGeneratedClass())
			{
				auto AddVariable = [&](const FRigVMExternalVariable& Variable, const FRigVMGraphVariableDescription* Local,
					URigVMGraph* Graph, const FString& Scope, bool bGetter)
				{
					const FString Access = bGetter ? TEXT("get") : TEXT("set");
					const FString TypePath = Variable.TypeObject ? Variable.TypeObject->GetPathName() : FString();
					const FString Type = TypePath.IsEmpty() ? Variable.TypeName.ToString() : ContentDiscovery::MakeReadableDiscoveryPath(TypePath);
					const FString ValueType = Variable.bIsArray ? Type + TEXT("[]") : Type;
					const FString Owner = Graph ? Graph->GetPathName() : Target.GetPathName();
					const FString Signature = FString::Printf(TEXT("variable|%s|%s|%s|%s|%s|%d|%s"),
						*Owner, *Scope, *Variable.Name.ToString(), *Variable.TypeName.ToString(), *TypePath, Variable.bIsArray, *Access);
					const TMap<FString, FString> Selection{
						{TEXT("variable"), Variable.Name.ToString()}, {TEXT("access"), Access}, {TEXT("scope"), Scope},
						{TEXT("type"), ValueType}, {TEXT("declaration"), Scope + TEXT(" · ") + Variable.Name.ToString() + TEXT(" : ") + ValueType}};
					const FText Name = FText::FromName(Variable.Name);
					const FText Category = FText::FromString(Scope == TEXT("external") ? TEXT("Variables") : Scope == TEXT("input") ? TEXT("Input Arguments") : TEXT("Local Variables"));
					const FText Tooltip = FText::FromString(FString::Printf(TEXT("%s the value of %s %s"),
						bGetter ? TEXT("Get") : TEXT("Set"), Scope == TEXT("input") ? TEXT("input") : TEXT("variable"), *Variable.Name.ToString()));
					URigVMEdGraphVariableNodeSpawner* Spawner = Local
						? URigVMEdGraphVariableNodeSpawner::CreateFromLocalVariable(&Target, Graph, *Local, bGetter, Name, Category, Tooltip)
						: URigVMEdGraphVariableNodeSpawner::CreateFromExternalVariable(&Target, Variable, bGetter, Name, Category, Tooltip);
					return AddKnownAction(Spawner, GeneratedClass, Signature, Selection);
				};
				if (auto* Host = Cast<URigVMHost>(GeneratedClass->GetDefaultObject()))
				{
					for (const auto& Variable : Host->GetExternalVariables())
						if (!AddVariable(Variable, nullptr, nullptr, TEXT("external"), true)
							|| !AddVariable(Variable, nullptr, nullptr, TEXT("external"), false)) return false;
				}
				URigVMGraph* Model = OutCatalog.EditorGraph->GetModel();
				if (auto* Library = Target.GetLocalFunctionLibrary())
				{
					for (auto* Function : Library->GetFunctions())
						if (Function->GetContainedGraph() == Model)
							for (const auto& Variable : Model->GetLocalVariables())
								if (!AddVariable(Variable.ToExternalVariable(), &Variable, Model, TEXT("local"), true)
									|| !AddVariable(Variable.ToExternalVariable(), &Variable, Model, TEXT("local"), false)) return false;
				}
				if (Model && Model->GetEntryNode())
				{
					for (const auto& Variable : Model->GetInputArguments())
						if (!AddVariable(Variable.ToExternalVariable(), &Variable, Model, TEXT("input"), true)) return false;
				}
			}
			return true;
		}

		bool MatchesSearch(
			const FAnimationRigGraphCandidate& Candidate,
			const FString& Query)
		{
			ContentDiscovery::FContentDiscoveryRequest Request;
			Request.Query = Query;
			Request.MatchMode =
				ContentDiscovery::EContentDiscoveryMatchMode::Text;
			const TArray<FString> ExtraTerms{
				Candidate.Name,
				Candidate.Category,
				Candidate.Summary,
				Candidate.ActionKind,
				Candidate.Kind,
			};
			int32 MatchScore = 0;
			FString MatchReason;
			return ContentDiscovery::TryScoreContentDiscoveryMatch(
				Request,
				Candidate.Id,
				Candidate.Label,
				ExtraTerms,
				MatchScore,
				MatchReason);
		}

	}

	bool TryQueryAnimationRigGraphCandidates(
		UObject& Target,
		const FString& Query,
		FAnimationRigGraphCandidateQueryResult& OutResult,
		FAnimationRigGraphCandidateQueryError& OutError)
	{
		OutResult = {};
		OutError = {};
		if (!IsInGameThread())
		{
			return ForwardDiagnostic(OutError, ThreadFailure());
		}
		UControlRigBlueprint* const Blueprint =
			Cast<UControlRigBlueprint>(&Target);
		if (!Blueprint)
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("animation_rig_graph_candidate_target_type_unsupported"), Target.GetClass()->GetPathName(),
				TEXT("Native RigVM creation candidates require a ControlRigBlueprint asset."));
			Diagnostic.PropertyPath = TEXT("target");
			Diagnostic.ActualValue = MakeShared<FJsonValueString>(Target.GetClass()->GetName());
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(UControlRigBlueprint::StaticClass()->GetName());
			Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Target.GetClass()->GetPathName());
			return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
		}
		OutResult.Target = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(*Blueprint);
		if (!ReadTargetRevision(*Blueprint, OutResult.TargetRevision, OutError))
		{
			return false;
		}
		FNativeActionCatalog Catalog;
		if (!TryBuildNativeCatalog(*Blueprint, Catalog, OutError))
		{
			return false;
		}
		OutResult.GraphName = Catalog.EditorGraph->GetName();
		TArray<int32> MatchedIndices;
		for (int32 Index = 0; Index < Catalog.Entries.Num(); ++Index)
		{
			if (MatchesSearch(Catalog.Entries[Index].Candidate, Query))
			{
				MatchedIndices.Add(Index);
			}
		}
		for (int32 MatchIndex = 0; MatchIndex < MatchedIndices.Num(); ++MatchIndex)
		{
			FAnimationRigGraphCandidate Candidate =
				Catalog.Entries[MatchedIndices[MatchIndex]].Candidate;
			OutResult.Candidates.Add(MoveTemp(Candidate));
		}
		OutResult.TotalMatches = MatchedIndices.Num();
		if (!OutResult.CatalogDiagnostic.IsEmpty() && OutResult.CatalogDiagnostic.TargetPath.IsEmpty())
			OutResult.CatalogDiagnostic.TargetPath = OutResult.Target;
		return true;
	}

	bool TryResolveAnimationRigGraphCandidate(
		UControlRigBlueprint& Target,
		const FString& Name,
		FAnimationRigGraphCandidate& OutCandidate,
		FAnimationRigGraphCandidateQueryError& OutError,
		ContentDiscovery::FReadOnlyCandidateBatch* Batch,
		const TMap<FString, FString>& Select)
	{
		OutCandidate = {};
		OutError = {};
		if (!IsInGameThread())
		{
			return ForwardDiagnostic(OutError, ThreadFailure());
		}
		struct FPreparedCatalog
		{
			FNativeActionCatalog Catalog;
			TArray<ContentDiscovery::FNameSelectionEntry> Names;
		};
		const auto Prepared = ContentDiscovery::FReadOnlyCandidateBatch::Prepare<FPreparedCatalog>(
			Batch, &Target, FString(), [&](FPreparedCatalog& Value)
		{
			auto& Catalog = Value.Catalog;
			if (!TryBuildNativeCatalog(Target, Catalog, OutError)) return false;
			for (const FNativeActionEntry& Item : Catalog.Entries)
			{
				Value.Names.Add({Item.Candidate.Name, Item.Identity.CanonicalKey, Item.Candidate.SelectionFields});
			}
			return true;
		});
		if (!Prepared) return false;
		int32 ResolvedIndex = INDEX_NONE;
		UE::UAI::Contract::FDiagnostic ResolveError;
		if (!ContentDiscovery::TryResolveNameSelection(Prepared->Names, Name, Select, ResolvedIndex, ResolveError, ContentDiscovery::ENameComparison::IgnoreCase))
		{
			return ForwardDiagnostic(OutError, UeAgentAuthoringDiagnostics::CandidateFailure(ResolveError.Code, Name,
				UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Target), TEXT("Control Rig graph menu"), TEXT("animation_rig_graph.candidates"), ResolveError), Name);
		}
		const FNativeActionEntry* Entry = Prepared->Catalog.Entries.IsValidIndex(ResolvedIndex) ? &Prepared->Catalog.Entries[ResolvedIndex] : nullptr;
		if (!Entry) return ForwardDiagnostic(OutError, IndexFailure(Name, ResolvedIndex, Prepared->Catalog.Entries.Num()));
		OutCandidate = Entry->Candidate;
		return true;
	}

	static bool PerformAnimationRigGraphCandidate(
		UControlRigBlueprint& Target,
		const FString& Name,
		const FVector2f& Location,
		URigVMNode*& OutNode,
		FAnimationRigGraphCandidate& OutCandidate,
		FAnimationRigGraphCandidateQueryError& OutError,
		bool bUnitOnly, const TMap<FString, FString>& Select)
	{
		OutNode = nullptr;
		OutCandidate = {};
		OutError = {};
		if (!IsInGameThread())
		{
			return ForwardDiagnostic(OutError, ThreadFailure());
		}
		if (!FMath::IsFinite(Location.X) || !FMath::IsFinite(Location.Y))
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("animation_rig_graph_candidate_position_invalid"), FString(),
				TEXT("RigVM node creation requires finite x and y coordinates."));
			Diagnostic.SourceKind.Reset();
			Diagnostic.PropertyPath = !FMath::IsFinite(Location.X) ? TEXT("x") : TEXT("y");
			Diagnostic.Source.JsonPath = TEXT("/") + Diagnostic.PropertyPath;
			auto Actual = MakeShared<FJsonObject>();
			auto Coordinate = [](float Value) -> TSharedPtr<FJsonValue>
			{
				if (FMath::IsFinite(Value)) return MakeShared<FJsonValueNumber>(Value);
				return MakeShared<FJsonValueString>(LexToString(Value));
			};
			Actual->SetField(TEXT("x"), Coordinate(Location.X));
			Actual->SetField(TEXT("y"), Coordinate(Location.Y));
			Diagnostic.ActualValue = MakeShared<FJsonValueObject>(Actual);
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("finite x and y coordinates"));
			return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
		}

		FNativeActionCatalog Catalog;
		FNativeActionEntry* Entry = nullptr;
		if (!TryBuildNativeCatalog(Target, Catalog, OutError))
		{
			return false;
		}
		{
			TArray<ContentDiscovery::FNameSelectionEntry> Names;
			for (const FNativeActionEntry& Item : Catalog.Entries)
			{
				Names.Add({Item.Candidate.Name, Item.Identity.CanonicalKey, Item.Candidate.SelectionFields});
			}
			int32 ResolvedIndex = INDEX_NONE;
			UE::UAI::Contract::FDiagnostic ResolveError;
			if (!ContentDiscovery::TryResolveNameSelection(Names, Name, Select, ResolvedIndex, ResolveError, ContentDiscovery::ENameComparison::IgnoreCase))
			{
				return ForwardDiagnostic(OutError, UeAgentAuthoringDiagnostics::CandidateFailure(ResolveError.Code, Name,
					UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Target), TEXT("Control Rig graph menu"), TEXT("animation_rig_graph.candidates"), ResolveError), Name);
			}
			Entry = Catalog.Entries.IsValidIndex(ResolvedIndex) ? &Catalog.Entries[ResolvedIndex] : nullptr;
			if (!Entry) return ForwardDiagnostic(OutError, IndexFailure(Name, ResolvedIndex, Catalog.Entries.Num()));
		}
		if (!Entry->Candidate.bSupported)
		{
			return ForwardDiagnostic(OutError, Entry->Candidate.SupportDiagnostic);
		}
		if (bUnitOnly && Entry->Candidate.ActionKind != URigVMEdGraphUnitNodeSpawner::StaticClass()->GetPathName())
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("animation_rig_graph_candidate_action_kind_unsupported"), Entry->Candidate.ActionKind,
				TEXT("This creation path requires a RigVM unit-node spawner; the selected candidate uses another action kind."));
			Diagnostic.CandidateName = Entry->Candidate.Name;
			Diagnostic.ActualValue = MakeShared<FJsonValueString>(FPackageName::ObjectPathToObjectName(Entry->Candidate.ActionKind));
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(URigVMEdGraphUnitNodeSpawner::StaticClass()->GetName());
			return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
		}

		URigVMGraph* const Model = Target.GetDefaultModel();
		TSet<URigVMNode*> NodesBefore;
		if (!Model)
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("animation_rig_graph_candidate_context_unavailable"), Target.GetPathName(),
				TEXT("The Control Rig blueprint no longer has a default RigVM model for node creation."));
			Diagnostic.ActualValuePresent = false;
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("default RigVM model"));
			return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
		}
		for (URigVMNode* Node : Model->GetNodes())
		{
			if (Node)
			{
				NodesBefore.Add(Node);
			}
		}

		// UE's RigVM menu factory constructs these items with empty bindings.
		// Keep its native spawner/controller behavior, without the menu's focus.
		UEdGraphNode* Returned = nullptr;
		if (Entry->Action->GetTypeId() == FRigVMActionMenuItem::StaticGetTypeId())
		{
			const auto Item = StaticCastSharedPtr<FRigVMActionMenuItem>(Entry->Action);
			if (const auto* Spawner = Item->GetRawAction())
				Returned = ContentDiscovery::NativeNodeAction::Invoke(*Spawner,
					*Catalog.EditorGraph, nullptr, Location, IBlueprintNodeBinder::FBindingSet());
		}
		else
			Returned = Entry->Action->PerformAction(Catalog.EditorGraph, nullptr, Location, false);
		URigVMEdGraphNode* const EditorNode =
			Cast<URigVMEdGraphNode>(Returned);
		URigVMNode* const ModelNode = EditorNode
			? EditorNode->GetModelNode()
			: nullptr;
		if (!ModelNode || NodesBefore.Contains(ModelNode)
			|| !Model->GetNodes().Contains(ModelNode))
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("animation_rig_graph_candidate_native_action_create_failed"), Entry->Candidate.Id,
				!Returned ? TEXT("The native RigVM action did not return an editor graph node.")
					: !EditorNode ? TEXT("The native action returned a node that is not a RigVM editor node.")
					: !ModelNode ? TEXT("The returned RigVM editor node has no model node.")
					: NodesBefore.Contains(ModelNode) ? TEXT("The native RigVM action returned a model node that already existed.")
					: TEXT("The returned RigVM model node is not registered in the target model."));
			Diagnostic.CandidateName = Entry->Candidate.Name;
			auto Actual = MakeShared<FJsonObject>();
			Actual->SetBoolField(TEXT("returned_editor_node"), Returned != nullptr);
			Actual->SetBoolField(TEXT("rigvm_editor_node"), EditorNode != nullptr);
			Actual->SetBoolField(TEXT("model_node_present"), ModelNode != nullptr);
			Actual->SetBoolField(TEXT("model_node_preexisting"), ModelNode && NodesBefore.Contains(ModelNode));
			Actual->SetBoolField(TEXT("registered_in_model"), ModelNode && Model->GetNodes().Contains(ModelNode));
			int32 NewNodes = 0;
			for (URigVMNode* Node : Model->GetNodes()) if (Node && !NodesBefore.Contains(Node)) ++NewNodes;
			Actual->SetNumberField(TEXT("new_nodes"), NewNodes);
			Diagnostic.ActualValue = MakeShared<FJsonValueObject>(Actual);
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("new RigVM model node registered in the target model"));
			auto Evidence = MakeShared<FJsonObject>();
			if (Returned) Evidence->SetStringField(TEXT("returned_node"), Returned->GetPathName());
			if (ModelNode) Evidence->SetStringField(TEXT("model_node"), ModelNode->GetPathName());
			Diagnostic.ActualEvidence = MakeShared<FJsonValueObject>(Evidence);
			Diagnostic.MutationMayHaveStarted = true;
			if (NewNodes > 0) Diagnostic.MutationStarted = true;
			return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
		}
		OutNode = ModelNode;
		OutCandidate = Entry->Candidate;
		return true;
	}

	bool TryPerformAnimationRigGraphCandidate(UControlRigBlueprint& Target,
		const FString& Name, const FVector2f& Location, URigVMNode*& OutNode,
		FAnimationRigGraphCandidate& OutCandidate, FAnimationRigGraphCandidateQueryError& OutError,
		const TMap<FString, FString>& Select)
	{
		return PerformAnimationRigGraphCandidate(Target, Name, Location, OutNode, OutCandidate, OutError, false, Select);
	}

	bool TryPerformAnimationRigGraphCandidate(UControlRigBlueprint& Target,
		const FString& Name, const FVector2f& Location, URigVMUnitNode*& OutNode,
		FAnimationRigGraphCandidate& OutCandidate, FAnimationRigGraphCandidateQueryError& OutError,
		const TMap<FString, FString>& Select)
	{
		OutNode = nullptr;
		URigVMNode* Node = nullptr;
		if (!PerformAnimationRigGraphCandidate(Target, Name, Location, Node, OutCandidate, OutError, true, Select)) return false;
		OutNode = Cast<URigVMUnitNode>(Node);
		if (!OutNode)
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("animation_rig_graph_candidate_native_action_create_failed"), OutCandidate.Id,
				TEXT("The unit-node spawner created a RigVM model node that is not a RigVM unit node."));
			Diagnostic.CandidateName = OutCandidate.Name;
			Diagnostic.ActualValue = MakeShared<FJsonValueString>(Node->GetClass()->GetName());
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(URigVMUnitNode::StaticClass()->GetName());
			Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Node->GetPathName());
			Diagnostic.MutationMayHaveStarted = true;
			Diagnostic.MutationStarted = true;
			return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
		}
		return true;
	}
}
