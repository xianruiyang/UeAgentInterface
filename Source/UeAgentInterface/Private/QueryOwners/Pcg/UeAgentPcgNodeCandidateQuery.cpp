#include "QueryOwners/Pcg/UeAgentPcgNodeCandidateQuery.h"
#include "Core/ContentDiscovery/UeAgentReadOnlyCandidateBatch.h"

#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
#include "Core/ContentDiscovery/UeAgentContentDiscovery.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphSchema.h"
#include "Internationalization/Text.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGSettings.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/AuthoringCore/UeAgentCandidatePropertyDescription.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utils/PCGPreconfiguration.h"
#include "UObject/UnrealType.h"
#include "UObject/StrongObjectPtr.h"

namespace UE::UAI::QueryOwners::Pcg
{
	using namespace ContentDiscovery;
	using namespace DomainAdapters::ContentGraphRead;

	namespace
	{
		const FString CandidateKind = TEXT("pcg_node");
		const FName SupportedActionType(
			TEXT("FPCGEditorGraphSchemaAction_NewNativeElement"));

		struct FNativeActionEntry final
		{
			TSharedPtr<FEdGraphSchemaAction> Action;
			FPcgNodeCandidate Candidate;
			UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
			FString Signature;
		};

		struct FNativeActionCatalog final
		{
			TStrongObjectPtr<UEdGraph> EditorGraph;
			TArray<FNativeActionEntry> Entries;
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

		UE::UAI::Contract::FDiagnostic CandidateCause(FString Code, const FString& Subject,
			const UE::UAI::Contract::FDiagnostic& Cause, FString FallbackReason)
		{
			auto Diagnostic = Cause.IsEmpty()
				? CandidateDiagnostic(MoveTemp(Code), Subject, MoveTemp(FallbackReason))
				: UE::UAI::Contract::FDiagnostic::WrapFailure(MoveTemp(Code), Cause);
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = Subject;
			return Diagnostic;
		}

		Contract::FDiagnostic GameThreadFailure(const UObject& Target)
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("pcg_candidate_game_thread_required"), Target.GetPathName(),
				TEXT("PCG candidates must be accessed on the Editor game thread."));
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("game thread"));
			Diagnostic.ActualValue = MakeShared<FJsonValueString>(TEXT("other thread"));
			return Diagnostic;
		}

		Contract::FDiagnostic MachineSelectorFailure(const FString& Id)
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("pcg_candidate_machine_selector_not_supported"), Id,
				TEXT("PCG candidates require a readable name and optional select fields; machine candidate identifiers are not accepted."));
			Diagnostic.SourceKind.Reset();
			Diagnostic.PropertyPath = TEXT("id");
			Diagnostic.Source.JsonPath = TEXT("/id");
			Diagnostic.ActualValuePresent = true;
			Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Id);
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("readable name with optional select fields"));
			return Diagnostic;
		}

		bool IsSupportedTarget(const UObject& Target)
		{
			return Target.GetClass() == UPCGGraph::StaticClass();
		}

		bool ReadTargetRevision(
			UObject& Target,
			FString& OutRevision,
			FPcgNodeCandidateQueryError& OutError)
		{
			if (Target.GetOutermost() == GetTransientPackage())
			{
				OutRevision = TEXT("transient:")
					+ Target.GetClass()->GetPathName();
				return true;
			}
			FUeAgentContentGraphCapture Capture;
			FUeAgentContentGraphReadError ReadError;
			const bool bCaptured = FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(
				Target, Capture, ReadError);
			if (!bCaptured || Capture.Revision.IsEmpty())
			{
				if (!ReadError.IsEmpty()) return ForwardDiagnostic(OutError, MoveTemp(ReadError));
				auto Diagnostic = CandidateDiagnostic(TEXT("pcg_candidate_target_readback_failed"), Target.GetPathName(),
					bCaptured ? TEXT("The PCG target readback did not provide a revision.")
						: TEXT("The PCG target readback failed without a diagnostic."));
				if (bCaptured)
				{
					Diagnostic.PropertyPath = TEXT("revision");
					Diagnostic.ActualValuePresent = false;
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("target revision"));
				}
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
			}
			OutRevision = Capture.Revision;
			return true;
		}

		void AppendSignatureField(
			FString& Signature,
			const FString& Name,
			const FString& Value)
		{
			Signature += FString::Printf(
				TEXT("%d:%s=%d:%s|"),
				Name.Len(), *Name, Value.Len(), *Value);
		}

		FString StableText(const FText& Text)
		{
			FString Result;
			FTextStringHelper::WriteToBuffer(Result, Text, true, false);
			return Result;
		}

		FString RemovePointerAddresses(const FString& Input)
		{
			FString Result;
			Result.Reserve(Input.Len());
			for (int32 Index = 0; Index < Input.Len();)
			{
				if (Index + 2 < Input.Len()
					&& Input[Index] == TEXT('0')
					&& Input[Index + 1] == TEXT('x')
					&& FChar::IsHexDigit(Input[Index + 2]))
				{
					Result += TEXT("<address>");
					Index += 2;
					while (Index < Input.Len()
						&& FChar::IsHexDigit(Input[Index]))
					{
						++Index;
					}
					continue;
				}
				Result.AppendChar(Input[Index]);
				++Index;
			}
			return Result;
		}

		UScriptStruct* ResolveActionScriptStruct(const FName TypeId)
		{
			FString StructName = TypeId.ToString();
			if (StructName.StartsWith(TEXT("F")))
			{
				StructName.RightChopInline(1, EAllowShrinking::No);
			}
			return FindObject<UScriptStruct>(
				nullptr, *(TEXT("/Script/PCGEditor.") + StructName));
		}

		bool BuildActionSignature(
			const FEdGraphSchemaAction& Action,
			FString& OutSignature,
			FPcgNodeCandidateQueryError& OutError)
		{
			OutSignature.Reset();
			UScriptStruct* const ScriptStruct = ResolveActionScriptStruct(Action.GetTypeId());
			if (Action.GetTypeId() == SupportedActionType && ScriptStruct)
			{
				const auto* ClassProperty = FindFProperty<FClassProperty>(ScriptStruct, TEXT("SettingsClass"));
				const auto* InfoProperty = FindFProperty<FStructProperty>(ScriptStruct, TEXT("PreconfiguredInfo"));
				UClass* SettingsClass = ClassProperty
					? Cast<UClass>(ClassProperty->GetObjectPropertyValue_InContainer(&Action)) : nullptr;
				if (SettingsClass && InfoProperty && InfoProperty->Struct == FPCGPreConfiguredSettingsInfo::StaticStruct())
				{
					const auto* Info = InfoProperty->ContainerPtrToValuePtr<FPCGPreConfiguredSettingsInfo>(&Action);
					// Native creation selects a settings class and its actual preset value.
					// Menu text, tooltip and category do not create different nodes.
					AppendSignatureField(OutSignature, TEXT("type_id"), Action.GetTypeId().ToString());
					AppendSignatureField(OutSignature, TEXT("settings_class"), SettingsClass->GetPathName());
					AppendSignatureField(OutSignature, TEXT("configuration"), LexToString(Info->PreconfiguredIndex));
					// Settings may use the preset label instead of its index. This is
					// creation input, unlike the action's menu alias/category/tooltip.
					AppendSignatureField(OutSignature, TEXT("configuration_label"), StableText(Info->Label));
					return true;
				}
			}
			AppendSignatureField(
				OutSignature, TEXT("type_id"),
				Action.GetTypeId().ToString());
			AppendSignatureField(
				OutSignature, TEXT("category"),
				StableText(Action.GetCategory()));
			AppendSignatureField(
				OutSignature, TEXT("menu"),
				StableText(Action.GetMenuDescription()));
			AppendSignatureField(
				OutSignature, TEXT("tooltip"),
				StableText(Action.GetTooltipDescription()));
			AppendSignatureField(
				OutSignature, TEXT("keywords"),
				StableText(Action.GetKeywords()));
			AppendSignatureField(
				OutSignature, TEXT("grouping"), LexToString(Action.Grouping));
			AppendSignatureField(
				OutSignature, TEXT("section_id"), LexToString(Action.SectionID));

			if (!ScriptStruct)
			{
				auto Diagnostic = CandidateDiagnostic(TEXT("pcg_candidate_action_struct_unresolved"), Action.GetTypeId().ToString(),
					TEXT("The native PCG menu action has no resolvable schema struct."));
				Diagnostic.CandidateName = Action.GetMenuDescription().ToString();
				Diagnostic.ActualValuePresent = false;
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("registered native action schema struct"));
				Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Action.GetTypeId().ToString());
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
			}
			TArray<const FProperty*> Properties;
			for (TFieldIterator<FProperty> It(
				ScriptStruct, EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				Properties.Add(*It);
			}
			Properties.Sort([](const FProperty& Left, const FProperty& Right)
			{
				return Left.GetName().Compare(
					Right.GetName(), ESearchCase::CaseSensitive) < 0;
			});
			for (const FProperty* Property : Properties)
			{
				FString Exported;
				const void* const Value =
					Property->ContainerPtrToValuePtr<void>(&Action);
				Property->ExportTextItem_Direct(
					Exported, Value, nullptr, nullptr, PPF_None);
				AppendSignatureField(
					OutSignature,
					TEXT("property.") + Property->GetName(),
					RemovePointerAddresses(Exported));
			}
			return true;
		}

		bool BuildCandidate(
			const FEdGraphSchemaAction& Action,
			const FString& Signature,
			FPcgNodeCandidate& OutCandidate,
			UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity,
			FPcgNodeCandidateQueryError& OutError)
		{
			UeAgentPaletteSearch::FCandidate IdentityCandidate;
			IdentityCandidate.Kind = CandidateKind;
			FString PublicName;
			FString SettingsSemanticName;
			FString SettingsSource;
			FString Configuration;
			bool bHasPreconfiguredLabel = false;
			UScriptStruct* const ActionStruct = ResolveActionScriptStruct(
				Action.GetTypeId());
			if (const FClassProperty* SettingsClassProperty =
					ActionStruct
						? FindFProperty<FClassProperty>(
							ActionStruct, TEXT("SettingsClass"))
						: nullptr)
			{
				if (UClass* const SettingsClass = Cast<UClass>(
						SettingsClassProperty->GetObjectPropertyValue_InContainer(
							&Action)))
				{
					SettingsSemanticName = SettingsClass->GetName();
					SettingsSource = ContentDiscovery::MakeReadableDiscoveryPath(SettingsClass->GetPathName());
					SettingsSemanticName.RemoveFromStart(
						TEXT("PCG"), ESearchCase::IgnoreCase);
					SettingsSemanticName.RemoveFromEnd(
						TEXT("Settings"), ESearchCase::IgnoreCase);
					SettingsSemanticName = FName::NameToDisplayString(
						SettingsSemanticName, false);
					PublicName = SettingsSemanticName;
				}
			}
			if (const FStructProperty* PreconfiguredProperty =
					ActionStruct
						? FindFProperty<FStructProperty>(
							ActionStruct, TEXT("PreconfiguredInfo"))
						: nullptr;
				PreconfiguredProperty
					&& PreconfiguredProperty->Struct
						== FPCGPreConfiguredSettingsInfo::StaticStruct())
			{
				const FPCGPreConfiguredSettingsInfo* const Preconfigured =
					PreconfiguredProperty->ContainerPtrToValuePtr<
						FPCGPreConfiguredSettingsInfo>(&Action);
				const FString* const SourceLabel = Preconfigured
					? FTextInspector::GetSourceString(Preconfigured->Label)
					: nullptr;
				if (Preconfigured)
					Configuration = Preconfigured->PreconfiguredIndex == INDEX_NONE
						? TEXT("default") : FString::Printf(TEXT("preset %d"), Preconfigured->PreconfiguredIndex);
				FString ReadableLabel = SourceLabel
					? SourceLabel->TrimStartAndEnd()
					: (Preconfigured
						? Preconfigured->Label.ToString().TrimStartAndEnd()
						: FString());
				if (ReadableLabel.Contains(FString::Chr(TCHAR(0xfffd))))
				{
					ReadableLabel.Reset();
				}
				if (!ReadableLabel.IsEmpty())
				{
					PublicName = MoveTemp(ReadableLabel);
					bHasPreconfiguredLabel = true;
				}
			}
			if (!bHasPreconfiguredLabel)
			{
				const FText MenuText = Action.GetMenuDescription();
				const FString* const SourceMenu =
					FTextInspector::GetSourceString(MenuText);
				FString ReadableMenu = SourceMenu
					? SourceMenu->TrimStartAndEnd()
					: MenuText.ToString().TrimStartAndEnd();
				if (ReadableMenu.Contains(FString::Chr(TCHAR(0xfffd))))
				{
					ReadableMenu.Reset();
				}
				if (!ReadableMenu.IsEmpty())
				{
					PublicName = MoveTemp(ReadableMenu);
				}
			}
			if (PublicName.IsEmpty())
			{
				PublicName = FName::NameToDisplayString(Action.GetTypeId().ToString(), false);
			}
			IdentityCandidate.Name = PublicName;
			IdentityCandidate.DisplayName =
				Action.GetMenuDescription().ToString();
			IdentityCandidate.CategoryPath = Action.GetCategory().ToString();
			IdentityCandidate.Tooltip =
				Action.GetTooltipDescription().ToString();
			IdentityCandidate.Keywords = Action.GetKeywords().ToString();
			IdentityCandidate.Type = Action.GetTypeId().ToString();
			IdentityCandidate.FullName = IdentityCandidate.Type + TEXT(":")
				+ FMD5::HashAnsiString(*Signature);
			UE::UAI::Contract::FDiagnostic IdentityError;
			if (!UeAgentPaletteSearch::SetCandidateIdentity(
					TEXT("pcg_node"),
					IdentityCandidate.FullName,
					IdentityCandidate.Name,
					IdentityCandidate,
					IdentityError))
			{
				return ForwardDiagnostic(OutError, CandidateCause(TEXT("pcg_candidate_identity_failed"),
					Action.GetTypeId().ToString(), IdentityError, TEXT("The native PCG candidate could not be assigned an identity.")));
			}

			OutCandidate = {};
			OutCandidate.Id = IdentityCandidate.GetPublicId();
			OutCandidate.Name = IdentityCandidate.Name;
			OutCandidate.Label = IdentityCandidate.DisplayName.IsEmpty()
				? IdentityCandidate.Name : IdentityCandidate.DisplayName;
			OutCandidate.Kind = CandidateKind;
			OutCandidate.ActionKind = IdentityCandidate.Type;
			OutCandidate.SelectionFields.Add(TEXT("action_kind"), OutCandidate.ActionKind);
			OutCandidate.Category = IdentityCandidate.CategoryPath;
			OutCandidate.Summary = IdentityCandidate.Tooltip;
			if (!SettingsSemanticName.IsEmpty())
			{
				OutCandidate.SelectionFields.Add(TEXT("settings"), SettingsSemanticName);
			}
			if (!SettingsSource.IsEmpty()) OutCandidate.SelectionFields.Add(TEXT("settings_source"), SettingsSource);
			if (!Configuration.IsEmpty()) OutCandidate.SelectionFields.Add(TEXT("configuration"), Configuration);
			OutCandidate.bSupported = Action.GetTypeId() == SupportedActionType;
			OutCandidate.WriteSupport = OutCandidate.bSupported
				? FString(TEXT("supported"))
				: FString(TEXT("search_only"));
			OutCandidate.SupportReason = OutCandidate.bSupported
				? FString(TEXT("native_schema_action"))
				: FString(TEXT("native_action_kind_not_yet_verified"));
			if (!OutCandidate.bSupported)
			{
				OutCandidate.SupportDiagnostic = CandidateDiagnostic(TEXT("pcg_candidate_action_kind_unsupported"),
					OutCandidate.Id, TEXT("This PCG candidate has no verified creation binding for its native action type."));
				OutCandidate.SupportDiagnostic.CandidateName = OutCandidate.Name;
				OutCandidate.SupportDiagnostic.ActualValue = MakeShared<FJsonValueString>(OutCandidate.ActionKind);
				OutCandidate.SupportDiagnostic.ExpectedValue = MakeShared<FJsonValueString>(SupportedActionType.ToString());
				OutCandidate.SupportDiagnostic.ActualEvidence = MakeShared<FJsonValueString>(Signature);
			}
			OutIdentity = MoveTemp(IdentityCandidate.Identity);
			return true;
		}

		bool TryBuildNativeCatalog(
			UPCGGraph& Target,
			FNativeActionCatalog& OutCatalog,
			FPcgNodeCandidateQueryError& OutError)
		{
			OutCatalog = {};
			if (!FModuleManager::Get().LoadModulePtr<IModuleInterface>(
					TEXT("PCGEditor")))
			{
				auto Diagnostic = CandidateDiagnostic(TEXT("pcg_candidate_editor_module_unavailable"), TEXT("PCGEditor"),
					TEXT("The PCGEditor module could not be loaded to enumerate native graph actions."));
				Diagnostic.ActualValuePresent = false;
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("loaded PCGEditor module"));
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
			}
			UClass* const EditorGraphClass = FindObject<UClass>(
				nullptr, TEXT("/Script/PCGEditor.PCGEditorGraph"));
			UClass* const SchemaClass = FindObject<UClass>(
				nullptr, TEXT("/Script/PCGEditor.PCGEditorGraphSchema"));
			if (!EditorGraphClass
				|| !EditorGraphClass->IsChildOf(UEdGraph::StaticClass())
				|| !SchemaClass
				|| !SchemaClass->IsChildOf(UEdGraphSchema::StaticClass()))
			{
				auto Diagnostic = CandidateDiagnostic(TEXT("pcg_candidate_editor_context_unavailable"), Target.GetPathName(),
					!EditorGraphClass ? TEXT("The PCG Editor graph class is unavailable.")
						: !EditorGraphClass->IsChildOf(UEdGraph::StaticClass()) ? TEXT("The PCG Editor graph class does not derive from EdGraph.")
						: !SchemaClass ? TEXT("The PCG Editor graph schema class is unavailable.")
						: TEXT("The PCG Editor schema class does not derive from EdGraphSchema."));
				Diagnostic.PropertyPath = !EditorGraphClass || !EditorGraphClass->IsChildOf(UEdGraph::StaticClass()) ? TEXT("editor_graph_class") : TEXT("schema_class");
				UClass* ActualClass = Diagnostic.PropertyPath == TEXT("editor_graph_class") ? EditorGraphClass : SchemaClass;
				Diagnostic.ActualValuePresent = ActualClass != nullptr;
				if (ActualClass) Diagnostic.ActualValue = MakeShared<FJsonValueString>(ActualClass->GetName());
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(Diagnostic.PropertyPath == TEXT("editor_graph_class") ? TEXT("EdGraph subclass") : TEXT("EdGraphSchema subclass"));
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
			}
			UEdGraph* const EditorGraph = Cast<UEdGraph>(NewObject<UObject>(
				GetTransientPackage(), EditorGraphClass));
			FObjectPropertyBase* const PcgGraphProperty =
				FindFProperty<FObjectPropertyBase>(
					EditorGraphClass, TEXT("PCGGraph"));
			FClassProperty* const SchemaProperty = FindFProperty<FClassProperty>(
				UEdGraph::StaticClass(), TEXT("Schema"));
			if (!EditorGraph || !PcgGraphProperty || !SchemaProperty)
			{
				auto Diagnostic = CandidateDiagnostic(TEXT("pcg_candidate_editor_context_unavailable"), Target.GetPathName(),
					!EditorGraph ? TEXT("A transient PCG Editor graph could not be created.")
						: !PcgGraphProperty ? TEXT("The PCG Editor graph has no PCGGraph object property.")
						: TEXT("The Editor graph has no Schema class property."));
				Diagnostic.PropertyPath = !EditorGraph ? TEXT("editor_graph") : !PcgGraphProperty ? TEXT("PCGGraph") : TEXT("Schema");
				Diagnostic.ActualValuePresent = false;
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("PCG Editor graph with PCGGraph and Schema bindings"));
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
			}
			PcgGraphProperty->SetObjectPropertyValue_InContainer(
				EditorGraph, &Target);
			SchemaProperty->SetPropertyValue_InContainer(
				EditorGraph, SchemaClass);
			if (PcgGraphProperty->GetObjectPropertyValue_InContainer(EditorGraph)
					!= &Target
				|| EditorGraph->GetSchema() == nullptr)
			{
				auto Diagnostic = CandidateDiagnostic(TEXT("pcg_candidate_editor_context_invalid"), Target.GetPathName(),
					PcgGraphProperty->GetObjectPropertyValue_InContainer(EditorGraph) != &Target
						? TEXT("The transient PCG Editor graph did not retain its target graph binding.")
						: TEXT("The transient PCG Editor graph did not retain a schema."));
				Diagnostic.PropertyPath = PcgGraphProperty->GetObjectPropertyValue_InContainer(EditorGraph) != &Target ? TEXT("PCGGraph") : TEXT("Schema");
				if (Diagnostic.PropertyPath == TEXT("PCGGraph"))
				{
					Diagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(Target.GetPathName());
					UObject* BoundGraph = PcgGraphProperty->GetObjectPropertyValue_InContainer(EditorGraph);
					Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(BoundGraph ? BoundGraph->GetPathName() : TEXT("<null>"));
				}
				else
				{
					Diagnostic.ActualValuePresent = false;
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("bound PCG graph schema"));
				}
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
			}
			OutCatalog.EditorGraph.Reset(EditorGraph);

			FGraphContextMenuBuilder MenuBuilder(EditorGraph);
			EditorGraph->GetSchema()->GetGraphContextActions(MenuBuilder);
			TMap<FString, FString> SignaturesById;
			for (int32 Index = 0; Index < MenuBuilder.GetNumActions(); ++Index)
			{
				const TSharedPtr<FEdGraphSchemaAction>& Action =
					MenuBuilder.GetSchemaAction(Index);
				if (!Action.IsValid())
				{
					auto Diagnostic = CandidateDiagnostic(TEXT("pcg_candidate_native_action_invalid"), FString::FromInt(Index),
						TEXT("The PCG graph menu returned an empty native action entry."));
					Diagnostic.OperationIndex = Index;
					Diagnostic.ActualValuePresent = false;
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("native menu action"));
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
				}
				FNativeActionEntry& Entry =
					OutCatalog.Entries.AddDefaulted_GetRef();
				Entry.Action = Action;
				if (!BuildActionSignature(*Action, Entry.Signature, OutError)
					|| !BuildCandidate(
						*Action,
						Entry.Signature,
						Entry.Candidate,
						Entry.Identity,
						OutError))
				{
					return false;
				}
				const FString* ExistingSignature = SignaturesById.Find(Entry.Candidate.Id);
				if (ExistingSignature && *ExistingSignature != Entry.Signature)
				{
					auto Diagnostic = CandidateDiagnostic(TEXT("pcg_candidate_identity_ambiguous"), Entry.Candidate.Id,
						TEXT("Distinct PCG native creation inputs produced the same candidate identity."));
					Diagnostic.CandidateName = Entry.Candidate.Name;
					Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(2);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueNumber>(1);
					Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Entry.Candidate.Id + TEXT("|") + Entry.Signature);
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
				}
				// Native menus expose alternate titles as separate actions. Keep their
				// search metadata and names; the shared name resolver already collapses
				// equal identities before deciding whether a selection is ambiguous.
				SignaturesById.Add(Entry.Candidate.Id, Entry.Signature);
			}
			return true;
		}

		bool MatchesSearch(
			const FPcgNodeCandidate& Candidate,
			const FString& Query)
		{
			ContentDiscovery::FContentDiscoveryRequest Request;
			Request.Query = Query;
			Request.MatchMode =
				ContentDiscovery::EContentDiscoveryMatchMode::Text;
			int32 MatchScore = 0;
			FString MatchReason;
			return ContentDiscovery::TryScoreContentDiscoveryMatch(
				Request,
				Candidate.Id,
				Candidate.Name,
				{
					Candidate.Label,
					Candidate.Category,
					Candidate.ActionKind,
					Candidate.Summary,
					Candidate.SupportReason,
				},
				MatchScore,
				MatchReason);
		}

		bool TrySelectNativeAction(
			FNativeActionCatalog& Catalog,
			const FString& TargetPath,
			const FString& Name,
			const FString& Id,
			const TMap<FString, FString>& Select,
			FNativeActionEntry*& OutEntry,
			FPcgNodeCandidateQueryError& OutError)
		{
			OutEntry = nullptr;
			const FString CleanName = Name.TrimStartAndEnd();
			const FString CleanId = Id.TrimStartAndEnd();
			if (!CleanId.IsEmpty())
			{
				return ForwardDiagnostic(OutError, MachineSelectorFailure(Id), CleanId);
			}
			if (CleanName.IsEmpty())
			{
				auto Diagnostic = CandidateDiagnostic(TEXT("pcg_candidate_selector_required"), Name, TEXT("A PCG candidate requires a non-empty readable name."));
				Diagnostic.SourceKind.Reset();
				Diagnostic.PropertyPath = TEXT("name");
				Diagnostic.Source.JsonPath = TEXT("/name");
				Diagnostic.ActualValue = MakeShared<FJsonValueString>(Name);
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty readable candidate name"));
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
			}
			TArray<FNameSelectionEntry> Entries;
			Entries.Reserve(Catalog.Entries.Num());
			for (const FNativeActionEntry& Entry : Catalog.Entries)
			{
				Entries.Add({Entry.Candidate.Name, Entry.Candidate.Id, Entry.Candidate.SelectionFields});
			}
			int32 ResolvedIndex = INDEX_NONE;
			UE::UAI::Contract::FDiagnostic ResolveError;
			if (!TryResolveNameSelection(Entries, CleanName, Select, ResolvedIndex, ResolveError))
			{
				return ForwardDiagnostic(OutError, UeAgentAuthoringDiagnostics::CandidateFailure(ResolveError.Code, CleanName, TargetPath, TEXT("PCG graph menu"), TEXT("pcg.node.candidates"), ResolveError), CleanName);
			}
			if (!Catalog.Entries.IsValidIndex(ResolvedIndex))
			{
				auto Diagnostic = CandidateDiagnostic(TEXT("pcg_candidate_resolved_index_invalid"), CleanName,
					TEXT("The resolved PCG candidate index is outside the current native catalog."));
				Diagnostic.CandidateName = CleanName;
				auto Actual = MakeShared<FJsonObject>();
				Actual->SetNumberField(TEXT("resolved_index"), ResolvedIndex);
				Actual->SetNumberField(TEXT("candidate_count"), Catalog.Entries.Num());
				Diagnostic.ActualValue = MakeShared<FJsonValueObject>(Actual);
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("index within the native candidate catalog"));
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
			}
			OutEntry = &Catalog.Entries[ResolvedIndex];
			return true;
		}
	}

	bool TryQueryPcgNodeCandidates(
		UObject& Target,
		const FString& Query,
		FPcgNodeCandidateQueryResult& OutResult,
		FPcgNodeCandidateQueryError& OutError,
		const FString& CandidateId)
	{
		OutResult = {};
		OutError = {};
		if (!IsInGameThread())
		{
			return ForwardDiagnostic(OutError, GameThreadFailure(Target));
		}
		if (!IsSupportedTarget(Target))
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("pcg_candidate_target_type_unsupported"), Target.GetPathName(),
				TEXT("PCG node candidates require a PCGGraph target."));
			Diagnostic.PropertyPath = TEXT("target");
			Diagnostic.ActualValue = MakeShared<FJsonValueString>(Target.GetClass()->GetName());
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("PCGGraph"));
			Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Target.GetClass()->GetPathName());
			return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
		}
		if (!CandidateId.TrimStartAndEnd().IsEmpty())
		{
			return ForwardDiagnostic(OutError, MachineSelectorFailure(CandidateId), CandidateId);
		}

		OutResult.Target = Target.GetPathName();
		if (!ReadTargetRevision(Target, OutResult.TargetRevision, OutError))
		{
			return false;
		}
		FNativeActionCatalog Catalog;
		if (!TryBuildNativeCatalog(
				static_cast<UPCGGraph&>(Target), Catalog, OutError))
		{
			return false;
		}
		TArray<int32> MatchedIndices;
		for (int32 Index = 0; Index < Catalog.Entries.Num(); ++Index)
		{
			const FNativeActionEntry& Entry = Catalog.Entries[Index];
			if (!MatchesSearch(Entry.Candidate, Query)) continue;
			if (IsReadableDiscoveryName(Entry.Candidate.Name))
			{
				MatchedIndices.Add(Index);
			}
		}
		OutResult.TotalMatches = MatchedIndices.Num();
		for (const int32 MatchedIndex : MatchedIndices)
		{
			OutResult.Candidates.Add(Catalog.Entries[MatchedIndex].Candidate);
		}
		return true;
	}

	bool TryDescribePcgNodeCandidate(UPCGGraph& Target, const CandidateProperties::FRequest& Request,
		TSharedPtr<FJsonObject>& OutDescription, FPcgNodeCandidateQueryError& OutError)
	{
		OutError = {};
		if (!IsInGameThread()) return ForwardDiagnostic(OutError, GameThreadFailure(Target));
		FNativeActionCatalog Catalog;
		FNativeActionEntry* Entry = nullptr;
		if (!TryBuildNativeCatalog(Target, Catalog, OutError)
			|| !TrySelectNativeAction(Catalog, Target.GetPathName(), Request.Name, FString(), Request.Select, Entry, OutError)) return false;
		if (Entry->Action->GetTypeId() != SupportedActionType)
		{
			OutDescription = MakeShared<FJsonObject>();
			OutDescription->SetStringField(TEXT("name"), Request.Name);
			OutDescription->SetStringField(TEXT("basis"), TEXT("native_menu_action"));
			CandidateProperties::SetCreationSupport(*OutDescription, Entry->Candidate.bSupported,
				Entry->Candidate.SupportDiagnostic.IsEmpty() ? Entry->Candidate.SupportReason : Entry->Candidate.SupportDiagnostic.PublicMessage);
			return true;
		}
		UScriptStruct* ActionStruct = ResolveActionScriptStruct(Entry->Action->GetTypeId());
		const auto* ClassProperty = ActionStruct ? FindFProperty<FClassProperty>(ActionStruct, TEXT("SettingsClass")) : nullptr;
		UClass* Class = ClassProperty ? Cast<UClass>(ClassProperty->GetObjectPropertyValue_InContainer(Entry->Action.Get())) : nullptr;
		if (!Class || !Class->IsChildOf(UPCGSettings::StaticClass()))
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("pcg_candidate_settings_unavailable"), Request.Name,
				!ActionStruct ? TEXT("The selected PCG action has no schema struct for settings inspection.")
					: !ClassProperty ? TEXT("The selected PCG action has no SettingsClass property.")
					: !Class ? TEXT("The selected PCG action does not provide a settings class.")
					: TEXT("The selected PCG action's settings class does not derive from PCGSettings."));
			Diagnostic.CandidateName = Request.Name;
			Diagnostic.PropertyPath = TEXT("SettingsClass");
			Diagnostic.ActualValuePresent = Class != nullptr;
			if (Class)
			{
				Diagnostic.ActualValue = MakeShared<FJsonValueString>(Class->GetName());
				Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Class->GetPathName());
			}
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("PCGSettings subclass"));
			return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
		}
		// Match native NewNativeElement settings initialization, without creating a
		// node or mutating the target graph. Pins remain an instance-context concern.
		TStrongObjectPtr<UPCGSettings> Prototype(NewObject<UPCGSettings>(GetTransientPackage(), Class, NAME_None, RF_Transient));
		if (const auto* Preconfigured = FindFProperty<FStructProperty>(ActionStruct, TEXT("PreconfiguredInfo"));
			Preconfigured && Preconfigured->Struct == FPCGPreConfiguredSettingsInfo::StaticStruct())
			Prototype->ApplyPreconfiguredSettings(*Preconfigured->ContainerPtrToValuePtr<FPCGPreConfiguredSettingsInfo>(Entry->Action.Get()));
		UE::UAI::Contract::FDiagnostic Error;
		if (!CandidateProperties::DescribeObject(*Prototype, Request, {}, OutDescription, Error))
			return ForwardDiagnostic(OutError, CandidateCause(TEXT("pcg_candidate_description_failed"),
				Request.Name, Error, TEXT("The selected PCG settings prototype could not be described.")), Request.Name);
		OutDescription->SetStringField(TEXT("basis"), TEXT("native_preconfigured_settings"));
		CandidateProperties::SetCreationSupport(*OutDescription, Entry->Candidate.bSupported, Entry->Candidate.SupportReason);
		OutDescription->SetStringField(TEXT("interface_scope"), TEXT("settings_only; pins_resolve_in_created_graph_context"));
		return true;
	}

	bool TryResolvePcgNodeCandidate(
		UPCGGraph& Target,
		const FString& Name,
		const FString& Id,
		FPcgNodeCandidate& OutCandidate,
		FPcgNodeCandidateQueryError& OutError,
		const TMap<FString, FString>& Select,
		ContentDiscovery::FReadOnlyCandidateBatch* Batch)
	{
		OutCandidate = {};
		OutError = {};
		if (!IsInGameThread())
		{
			return ForwardDiagnostic(OutError, GameThreadFailure(Target));
		}
		const auto Prepared = ContentDiscovery::FReadOnlyCandidateBatch::Prepare<FNativeActionCatalog>(
			Batch, &Target, FString(), [&](FNativeActionCatalog& Catalog)
			{ return TryBuildNativeCatalog(Target, Catalog, OutError); });
		FNativeActionEntry* Entry = nullptr;
		if (!Prepared || !TrySelectNativeAction(*Prepared, Target.GetPathName(), Name, Id, Select, Entry, OutError))
		{
			return false;
		}
		OutCandidate = Entry->Candidate;
		return true;
	}

	bool TryPerformPcgNodeCandidate(
		UPCGGraph& Target,
		const FString& Name,
		const FString& Id,
		const FVector2f& Location,
		UPCGNode*& OutNode,
		FPcgNodeCandidate& OutCandidate,
		FPcgNodeCandidateQueryError& OutError,
		const TMap<FString, FString>& Select)
	{
		OutNode = nullptr;
		OutCandidate = {};
		OutError = {};
		if (!IsInGameThread())
		{
			return ForwardDiagnostic(OutError, GameThreadFailure(Target));
		}
		if (!FMath::IsFinite(Location.X) || !FMath::IsFinite(Location.Y))
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("pcg_candidate_position_invalid"), Name,
				TEXT("A PCG candidate position requires finite x and y coordinates."));
			Diagnostic.SourceKind.Reset();
			Diagnostic.PropertyPath = !FMath::IsFinite(Location.X) ? TEXT("x") : TEXT("y");
			Diagnostic.Source.JsonPath = TEXT("/") + Diagnostic.PropertyPath;
			auto Actual = MakeShared<FJsonObject>();
			const auto CoordinateValue = [](float Value) -> TSharedPtr<FJsonValue>
			{
				if (FMath::IsFinite(Value)) return MakeShared<FJsonValueNumber>(Value);
				return MakeShared<FJsonValueString>(LexToString(Value));
			};
			Actual->SetField(TEXT("x"), CoordinateValue(Location.X));
			Actual->SetField(TEXT("y"), CoordinateValue(Location.Y));
			Diagnostic.ActualValue = MakeShared<FJsonValueObject>(Actual);
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("finite x and y"));
			return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
		}
		FNativeActionCatalog Catalog;
		FNativeActionEntry* Entry = nullptr;
		if (!TryBuildNativeCatalog(Target, Catalog, OutError)
			|| !TrySelectNativeAction(Catalog, Target.GetPathName(), Name, Id, Select, Entry, OutError))
		{
			return false;
		}
		if (!Entry->Candidate.bSupported)
		{
			return ForwardDiagnostic(OutError, Entry->Candidate.SupportDiagnostic, Entry->Candidate.ActionKind);
		}

		TSet<UPCGNode*> NodesBefore;
		for (UPCGNode* Node : Target.GetNodes())
		{
			if (Node)
			{
				NodesBefore.Add(Node);
			}
		}
		UEdGraphNode* const ReturnedEditorNode = Entry->Action->PerformAction(
			Catalog.EditorGraph.Get(), nullptr, Location, false);
		TArray<UPCGNode*> CreatedNodes;
		for (UPCGNode* Node : Target.GetNodes())
		{
			if (Node && !NodesBefore.Contains(Node))
			{
				CreatedNodes.Add(Node);
			}
		}
		if (!ReturnedEditorNode || CreatedNodes.Num() != 1)
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("pcg_candidate_native_action_create_failed"), Entry->Candidate.Id,
				TEXT("The native PCG action did not return an editor node and create exactly one PCG node."));
			Diagnostic.CandidateName = Entry->Candidate.Name;
			auto Actual = MakeShared<FJsonObject>();
			Actual->SetBoolField(TEXT("returned_editor_node"), ReturnedEditorNode != nullptr);
			Actual->SetNumberField(TEXT("created_nodes"), CreatedNodes.Num());
			auto Expected = MakeShared<FJsonObject>();
			Expected->SetBoolField(TEXT("returned_editor_node"), true);
			Expected->SetNumberField(TEXT("created_nodes"), 1);
			Diagnostic.ActualValue = MakeShared<FJsonValueObject>(Actual);
			Diagnostic.ExpectedValue = MakeShared<FJsonValueObject>(Expected);
			Diagnostic.MutationMayHaveStarted = true;
			if (!CreatedNodes.IsEmpty()) Diagnostic.MutationStarted = true;
			return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
		}
		OutNode = CreatedNodes[0];
		OutCandidate = Entry->Candidate;
		return true;
	}

}
