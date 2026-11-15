#include "Misc/AutomationTest.h"

#include "Containers/StringConv.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/SourceLoader/UeAgentHjsonSourceLoader.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceResolver.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"

namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::StateSemantic;

	FUeAgentResourceTracker MakeResolverTracker()
	{
		using UE::UAI::ContextResource::EUeAgentResourceDimension;
		const TMap<EUeAgentResourceDimension, int64> Limits{
			{EUeAgentResourceDimension::InputBytes, 32 * 1024 * 1024},
			{EUeAgentResourceDimension::OutputBytes, 32 * 1024 * 1024},
			{EUeAgentResourceDimension::StringBytes, 32 * 1024 * 1024},
			{EUeAgentResourceDimension::Nodes, 262144},
			{EUeAgentResourceDimension::Items, 262144},
			{EUeAgentResourceDimension::Files, 32768},
			{EUeAgentResourceDimension::ReferenceEdges, 262144},
			{EUeAgentResourceDimension::WorkUnits, 2097152},
			{EUeAgentResourceDimension::EstimatedMemoryBytes, 256 * 1024 * 1024},
		};
		return FUeAgentResourceTracker();
	}

	UeAgentAuthoringState::FStateEntry MakeResolverEntry(
		const FString& CanonicalKey,
		const FString& NodeGuid,
		const FString& Title)
	{
		TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
		Value->SetStringField(TEXT("id"), NodeGuid);
		Value->SetStringField(TEXT("title"), Title);
		const auto Position = MakeShared<FJsonObject>();
		// This fixture represents captured Blueprint NodePosX (native int32).
		Position->SetNumberField(TEXT("x"), 18);
		Position->SetNumberField(TEXT("y"), 23.0);
		Value->SetObjectField(TEXT("pos"), Position);
		UeAgentAuthoringState::FStateEntry Entry;
		Entry.SemanticId = TEXT("uai.semantic.blueprint.node");
		Entry.TargetIdentity.TargetKind = TEXT("blueprint_node");
		Entry.TargetIdentity.CanonicalKey = CanonicalKey;
		Entry.TargetIdentity.Selectors = {
			{TEXT("asset_path"), TEXT("/Game/HX013/BP_Resolver")},
			{TEXT("graph_guid"), TEXT("graph-guid")},
			{TEXT("node_guid"), NodeGuid},
		};
		Entry.Value = MakeShared<FJsonValueObject>(Value);
		Entry.bAvailable = true;
		Entry.bEditable = true;
		Entry.Provenance = TEXT("fixture:hx013:native-resolver");
		return Entry;
	}

	UeAgentAuthoringState::FStateEntry MakeResolverRelation(
		const FString& CanonicalKey,
		const FString& FromNode,
		const FString& ToNode)
	{
		TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
		const auto From = MakeShared<FJsonObject>();
		From->SetStringField(TEXT("node"), FromNode);
		From->SetStringField(TEXT("pin"), TEXT("then"));
		const auto To = MakeShared<FJsonObject>();
		To->SetStringField(TEXT("node"), ToNode);
		To->SetStringField(TEXT("pin"), TEXT("execute"));
		Value->SetObjectField(TEXT("from"), From);
		Value->SetObjectField(TEXT("to"), To);
		UeAgentAuthoringState::FStateEntry Entry;
		Entry.SemanticId = TEXT("uai.semantic.blueprint.edge");
		Entry.TargetIdentity.TargetKind = TEXT("blueprint_edge");
		Entry.TargetIdentity.CanonicalKey = CanonicalKey;
		Entry.TargetIdentity.Selectors = {
			{TEXT("asset_path"), TEXT("/Game/HX013/BP_Resolver")},
			{TEXT("graph_guid"), TEXT("graph-guid")},
			{TEXT("edge_key"), CanonicalKey},
		};
		Entry.Value = MakeShared<FJsonValueObject>(Value);
		Entry.bAvailable = true;
		Entry.bEditable = true;
		Entry.Provenance = TEXT("fixture:hx013:native-resolver");
		return Entry;
	}

	UeAgentAuthoringState::FStateEntry MakeResolverProperty(const FString& NodeKey)
	{
		auto Entry = MakeResolverEntry(NodeKey + TEXT("|InputPriority"), TEXT("node-a"), FString());
		Entry.SemanticId = TEXT("uai.semantic.blueprint.node.property");
		Entry.TargetIdentity.Selectors.Add(TEXT("property_path"), TEXT("InputPriority"));
		Entry.Value = MakeShared<FJsonValueNumberString>(TEXT("9007199254740993"));
		Entry.Type.Kind = TEXT("int64");
		return Entry;
	}

	UeAgentAuthoringState::FStateEntry MakeResolverGraph()
	{
		auto Entry = MakeResolverEntry(TEXT("blueprint_graph|graph_guid=graph-guid"), FString(), FString());
		Entry.SemanticId = TEXT("uai.semantic.blueprint.graph");
		Entry.TargetIdentity.TargetKind = TEXT("blueprint_graph");
		Entry.TargetIdentity.Selectors.Remove(TEXT("node_guid"));
		const auto Value = MakeShared<FJsonObject>();
		Value->SetStringField(TEXT("id"), TEXT("graph-guid"));
		Value->SetStringField(TEXT("name"), TEXT("EventGraph"));
		Entry.Value = MakeShared<FJsonValueObject>(Value);
		return Entry;
	}

	bool WritePlan(
		const FExportWorkspacePlan& Plan,
		const FString& Root)
	{
		for (const FExportFilePlan& File : Plan.GetFiles())
		{
			const FString FullPath =
				FPaths::Combine(Root, File.GetRelativePath());
			if (!IFileManager::Get().MakeDirectory(
					*FPaths::GetPath(FullPath),
					true)
				|| !FFileHelper::SaveArrayToFile(
					File.GetCanonicalUtf8(),
					*FullPath))
			{
				return false;
			}
		}
		return true;
	}

	bool ParseJsonObject(
		const FString& Text,
		TSharedPtr<FJsonObject>& OutObject)
	{
		OutObject.Reset();
		FString Error;
		return FUeAgentStructuredHjsonParser::ParseObject(
				Text,
				TEXT("semantic-workspace-resolver-test.hjson"),
				OutObject,
				Error)
			&& OutObject.IsValid();
	}

	bool SerializeJsonObject(
		const TSharedRef<FJsonObject>& Object,
		FString& OutText)
	{
		OutText.Reset();
		return FJsonSerializer::Serialize(
			Object,
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>
				::Create(&OutText));
	}

	TSharedPtr<FJsonObject> FindEditableInteger(const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject>* Categories = nullptr;
		if (!Document.IsValid() || !Document->TryGetObjectField(TEXT("categories"), Categories)) return nullptr;
		for (const auto& Category : (*Categories)->Values)
		{
			const TSharedPtr<FJsonObject>* Property = nullptr;
			if (Category.Value.IsValid() && Category.Value->Type == EJson::Object
				&& Category.Value->AsObject()->TryGetObjectField(TEXT("InputPriority"), Property))
			{
				return *Property;
			}
		}
		return nullptr;
	}

	bool HasExactInteger(const FAuthoringModel& Model, const FString& Lexeme, FString* OutUnitId = nullptr)
	{
		for (const FSemanticUnit& Unit : Model.GetUnits())
		{
			const auto* Value = Unit.GetValues().Find(TEXT("value"));
			if (Unit.GetFieldSetRef() == TEXT("InputPriority") && Value
				&& (*Value)->GetKind() == EUeAgentValueKind::Integer && (*Value)->GetLexeme() == Lexeme)
			{
				if (OutUnitId) *OutUnitId = Unit.GetUnitId();
				return true;
			}
		}
		return false;
	}

	const FUeAgentCanonicalValue* ObjectField(const FUeAgentCanonicalValue* Object, const TCHAR* Name)
	{
		if (Object && Object->GetKind() == EUeAgentValueKind::Object)
			for (const auto& Field : Object->GetObjectItems())
				if (Field.Key == Name) return &Field.Value.Get();
		return nullptr;
	}

	FString StringField(const FUeAgentCanonicalValue* Object, const TCHAR* Name)
	{
		const auto* Value = ObjectField(Object, Name);
		return Value && Value->GetKind() == EUeAgentValueKind::String ? Value->GetText() : FString();
	}

	bool HasCapturedGraphBindings(const FAuthoringModel& Model)
	{
		for (const FSemanticUnit& Unit : Model.GetUnits())
		{
			const auto* Nodes = Unit.GetValues().Find(TEXT("nodes"));
			const auto* Edges = Unit.GetValues().Find(TEXT("edges"));
			if (!Nodes || !Edges || (*Nodes)->GetKind() != EUeAgentValueKind::Object
				|| (*Edges)->GetKind() != EUeAgentValueKind::Object) continue;
			TSet<FString> NodeIds;
			for (const auto& Node : (*Nodes)->GetObjectItems()) NodeIds.Add(StringField(&Node.Value.Get(), TEXT("id")));
			TSet<FString> Connections;
			for (const auto& Edge : (*Edges)->GetObjectItems())
			{
				const auto* From = ObjectField(&Edge.Value.Get(), TEXT("from"));
				const auto* To = ObjectField(&Edge.Value.Get(), TEXT("to"));
				if (StringField(&Edge.Value.Get(), TEXT("operation")) != TEXT("update")) return false;
				Connections.Add(StringField(From, TEXT("node")) + TEXT(":") + StringField(From, TEXT("pin"))
					+ TEXT("->") + StringField(To, TEXT("node")) + TEXT(":") + StringField(To, TEXT("pin")));
			}
			return NodeIds.Num() == 2 && NodeIds.Contains(TEXT("node-a")) && NodeIds.Contains(TEXT("node-b"))
				&& Connections.Num() == 2 && Connections.Contains(TEXT("node-a:then->node-b:execute"))
				&& Connections.Contains(TEXT("node-b:then->node-a:execute"));
		}
		return false;
	}

	class FScopedResolverWorkspace final
	{
	public:
		FScopedResolverWorkspace()
			: Root(FPaths::Combine(
				FPaths::ProjectIntermediateDir(),
				TEXT("UAI/Tests/HX013"),
				FGuid::NewGuid().ToString(EGuidFormats::Digits)))
		{
		}

		~FScopedResolverWorkspace()
		{
			IFileManager::Get().DeleteDirectory(*Root, false, true);
		}

		FString Root;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSemanticWorkspaceResolverRoundTripTest,
	"UeAgentInterface.Core.HjsonWorkspace.SemanticWorkspaceResolver.RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticWorkspaceResolverRoundTripTest::RunTest(
	const FString& Parameters)
{
	FString RegistrationError;
	if (!TestTrue(
		TEXT("composition contracts register"),
		RegisterGraphTreeTargetCompositionContracts(RegistrationError)
			&& RegisterOtherTargetCompositionContracts(RegistrationError)))
	{
		AddError(RegistrationError);
		return false;
	}

	const FString FirstKey =
		TEXT("blueprint_node|blueprint_node|node_guid|node_guid=node-a");
	const FString SecondKey =
		TEXT("blueprint_node|blueprint_node|node_guid|node_guid=node-b");
	FCompositionRequest Request;
	Request.TargetKind = TEXT("ue.blueprint.asset");
	Request.ProfileId = TEXT("llm_authoring");
	Request.BaseRevision = TEXT("fixture-hx013-resolver-r1");
	Request.SourceProvenance = TEXT("fixture:hx013:native-resolver");
	Request.Entries = {
		MakeResolverGraph(),
		MakeResolverEntry(FirstKey, TEXT("node-a"), TEXT("First")),
		MakeResolverEntry(SecondKey, TEXT("node-b"), TEXT("Second")),
		MakeResolverProperty(FirstKey),
		MakeResolverRelation(
			TEXT("edge:node-a:node-b"),
			TEXT("node-a"),
			TEXT("node-b")),
		MakeResolverRelation(
			TEXT("edge:node-b:node-a"),
			TEXT("node-b"),
			TEXT("node-a")),
	};

	FUeAgentResourceTracker CompositionResources = MakeResolverTracker();
	FAuthoringModelPtr SourceModel;
	FWorkspaceError Error;
	TestTrue(
		TEXT("same-semantic-type Blueprint nodes compose"),
		TryComposeAuthoringModel(
			Request,
			CompositionResources,
			SourceModel,
			Error));
	if (!SourceModel.IsValid())
	{
		AddError(Error.GetCode() + TEXT(":") + Error.GetSourceUnit() + TEXT(":") + Error.GetJsonPointer());
		return false;
	}
	if (!TestTrue(TEXT("composition preserves captured nodes and directional endpoints"), HasCapturedGraphBindings(*SourceModel))
		|| !TestTrue(TEXT("composition retains the exact editable integer"), HasExactInteger(*SourceModel, TEXT("9007199254740993")))) return false;

	FUeAgentResourceTracker PlanResources = MakeResolverTracker();
	FExportWorkspacePlanPtr Plan;
	TestTrue(
		TEXT("semantic directory plan builds"),
		TryBuildCompositionExportPlan(
			SourceModel.ToSharedRef(),
			{{TEXT("estimated_tokens"), 1000000}},
			PlanResources,
			Plan,
			Error));
	if (!Plan.IsValid())
	{
		AddError(Error.GetCode() + TEXT(":") + Error.GetSourceUnit() + TEXT(":") + Error.GetJsonPointer());
		return false;
	}

	FScopedResolverWorkspace Workspace;
	if (!TestTrue(
		TEXT("semantic directory plan is persisted"),
		WritePlan(*Plan, Workspace.Root))) return false;
	for (const FExportFilePlan& File : Plan->GetFiles())
	{
		if (File.GetRelativePath() == TEXT(".uai/workspace.hjson"))
		{
			continue;
		}
		const TArray<uint8>& Bytes = File.GetCanonicalUtf8();
		const FUTF8ToTCHAR Text(
			reinterpret_cast<const ANSICHAR*>(Bytes.GetData()),
			Bytes.Num());
		TestFalse(
			TEXT("persisted semantic units omit diagnostic provenance"),
			FString(Text.Length(), Text.Get())
				.Contains(TEXT("source_provenance")));
	}

	FUeAgentResourceTracker ResolveResources = MakeResolverTracker();
	FResolvedSemanticWorkspacePtr Resolved;
	TestTrue(
		TEXT("native resolver rebuilds the persisted semantic workspace"),
		TryResolveSemanticWorkspace(
			FWorkspaceSource::Directory(Workspace.Root),
			ESemanticWorkspaceMode::State,
			ResolveResources,
			Resolved,
			Error));
	TestTrue(TEXT("resolved workspace is available"), Resolved.IsValid());
	if (!Resolved.IsValid())
	{
		AddError(Error.GetCode() + TEXT(":") + Error.GetSourceUnit() + TEXT(":") + Error.GetJsonPointer());
		return false;
	}
	if (Resolved.IsValid())
	{
		FString GraphUnitId;
		for (const auto& Unit : Resolved->GetModel()->GetUnits())
			if (Unit.GetValues().Contains(TEXT("nodes")) && Unit.GetValues().Contains(TEXT("edges")))
				GraphUnitId = Unit.GetUnitId();
		UE::UAI::Contract::FDiagnostic PositionDiagnostic;
		PositionDiagnostic.Source.JsonPath = TEXT("/nodes/node-a/pos/x");
		Resolved->LocateDocumentDiagnostic({}, {}, PositionDiagnostic, GraphUnitId);
		TestEqual(TEXT("flat graph position has an authored source"), PositionDiagnostic.SourceKind, FString(TEXT("authored_value")));
		TestTrue(TEXT("flat graph position resolves the precise leaf"), PositionDiagnostic.Source.JsonPath.EndsWith(TEXT("/pos/x")));
		TestTrue(TEXT("flat graph position has a parser location"), PositionDiagnostic.Source.Line > 0 && PositionDiagnostic.Source.Column > 0);
		UE::UAI::Contract::FDiagnostic InferredPin;
		InferredPin.Source.JsonPath = TEXT("/graphs/graph-guid/nodes/node-a/pins/0/pin_type/pin_subcategory_object");
		Resolved->LocateDocumentDiagnostic({}, {}, InferredPin);
		FString Kind, File;
		TestTrue(TEXT("inferred pin type locates related authored inputs"), InferredPin.SourceKind == TEXT("related_input"));
		TestTrue(TEXT("inferred type source stays in editable HJSON"), !InferredPin.Source.SourceFile.IsEmpty() && !InferredPin.Source.SourceFile.Contains(TEXT(".uai/")));
		const auto* Sources = &InferredPin.RelatedSources;
		TestTrue(TEXT("derived type identifies node and both incident connections"), Sources->Num() == 3);
		if (Sources) for (const auto& Source : *Sources)
		{
			const auto Object = Source->AsObject();
			TestTrue(TEXT("related source has a real parser position"), Object->GetIntegerField(TEXT("line")) > 0 && Object->GetIntegerField(TEXT("column")) > 0);
		}
		TestEqual(
			TEXT("semantic fingerprint survives persisted round-trip"),
			Resolved->GetModel()->GetSemanticFingerprint(),
			SourceModel->GetSemanticFingerprint());
		TSet<FString> IdentityKeys;
		for (const FSemanticUnit& Unit :
			Resolved->GetModel()->GetUnits())
		{
			const FSemanticUnit* Original = SourceModel->FindUnit(Unit.GetUnitId());
			TestTrue(TEXT("resolved unit has its captured source"), Original != nullptr);
			if (Original)
			{
				TestEqual(TEXT("hidden source provenance survives round-trip"),
					Unit.GetSourceProvenance(), Original->GetSourceProvenance());
			}
			if (Unit.GetSemanticTypeRef()
				== TEXT("uai.semantic.blueprint.node"))
			{
				IdentityKeys.Add(Unit.GetTargetIdentityKey());
			}
		}
		TestEqual(
			TEXT("same semantic type retains two exact target bindings"),
			IdentityKeys.Num(),
			2);
		TestTrue(TEXT("first target binding survives"), IdentityKeys.Contains(FirstKey));
		TestTrue(TEXT("second target binding survives"), IdentityKeys.Contains(SecondKey));
		TestTrue(TEXT("normal resolve preserves the captured directed graph"), HasCapturedGraphBindings(*Resolved->GetModel()));
		TestTrue(TEXT("normal resolve preserves the exact editable integer"), HasExactInteger(*Resolved->GetModel(), TEXT("9007199254740993")));
	}

	FUeAgentResourceTracker UnchangedEditResources = MakeResolverTracker();
	FResolvedSemanticWorkspacePtr UnchangedEdited;
	FWorkspaceError UnchangedEditError;
	TestTrue(
		TEXT("unchanged workspace passes through edit serialization"),
		TryResolveEditedSemanticWorkspace(
			FWorkspaceSource::Directory(Workspace.Root),
			ESemanticWorkspaceMode::State,
			UnchangedEditResources,
			UnchangedEdited,
			UnchangedEditError));
	if (UnchangedEdited.IsValid())
	{
		TestTrue(TEXT("unchanged edited resolve preserves the captured directed graph"), HasCapturedGraphBindings(*UnchangedEdited->GetModel()));
		TestTrue(TEXT("unchanged edited resolve preserves the integer itself"), HasExactInteger(*UnchangedEdited->GetModel(), TEXT("9007199254740993")));
		TestTrue(
			TEXT("unchanged workspace has no false edited units"),
			UnchangedEdited->GetEditedUnitIds().IsSet()
				&& UnchangedEdited->GetEditedUnitIds()->IsEmpty());
	}
	else
	{
		AddError(UnchangedEditError.GetCode() + TEXT(":") + UnchangedEditError.GetSourceUnit() + TEXT(":") + UnchangedEditError.GetJsonPointer());
		return false;
	}

	FString FullPath;
	TSharedPtr<FJsonObject> EditedDocument;
	TSharedPtr<FJsonObject> EditableInteger;
	int32 MatchingDocuments = 0;
	for (const FExportFilePlan& File : Plan->GetFiles())
	{
		if (File.GetRelativePath().StartsWith(TEXT(".uai/"))) continue;
		FString CandidateText;
		TSharedPtr<FJsonObject> Candidate;
		const FString CandidatePath = FPaths::Combine(Workspace.Root, File.GetRelativePath());
		if (!FFileHelper::LoadFileToString(CandidateText, *CandidatePath)
			|| !ParseJsonObject(CandidateText, Candidate)) return false;
		const auto Property = FindEditableInteger(Candidate);
		if (Property.IsValid())
		{
			++MatchingDocuments;
			FullPath = CandidatePath;
			EditedDocument = Candidate;
			EditableInteger = Property;
		}
	}
	if (!TestEqual(TEXT("the declared editable field has one readable document"), MatchingDocuments, 1)) return false;
	{
		FString Text;
		EditableInteger->SetField(TEXT("value"), MakeShared<FJsonValueNumberString>(TEXT("9007199254740995")));
		if (!SerializeJsonObject(EditedDocument.ToSharedRef(), Text)) return false;
		TestTrue(
			TEXT("tampered semantic unit is written"),
			FFileHelper::SaveStringToFile(
				Text,
				*FullPath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));

		FUeAgentResourceTracker TamperResources = MakeResolverTracker();
		FResolvedSemanticWorkspacePtr Rejected;
		FWorkspaceError TamperError;
		TestFalse(
			TEXT("stale inventory hash fails before any Apply"),
			TryResolveSemanticWorkspace(
				FWorkspaceSource::Directory(Workspace.Root),
				ESemanticWorkspaceMode::State,
				TamperResources,
				Rejected,
				TamperError));
		TestEqual(
			TEXT("tamper failure is explicit"),
			TamperError.GetCode(),
			FString(TEXT("semantic_workspace_unit_hash_mismatch")));
		TestFalse(
			TEXT("rejected workspace is not exposed"),
			Rejected.IsValid());

		FUeAgentResourceTracker EditedResources = MakeResolverTracker();
		FResolvedSemanticWorkspacePtr Edited;
		FWorkspaceError EditedError;
		TestTrue(
			TEXT("managed value edits resolve through the import resolver"),
			TryLoadHjsonImportSource(
				FWorkspaceSource::Directory(Workspace.Root),
				ESemanticWorkspaceMode::State,
				EditedResources,
				Edited,
				EditedError));
		TestTrue(
			TEXT("edited workspace is exposed"),
			Edited.IsValid());
		if (Edited.IsValid())
		{
			for (const FSemanticUnit& Unit : Edited->GetModel()->GetUnits())
			{
				const FSemanticUnit* Original = SourceModel->FindUnit(Unit.GetUnitId());
				if (Original)
				{
					TestEqual(TEXT("ordinary edits preserve hidden source provenance"),
						Unit.GetSourceProvenance(), Original->GetSourceProvenance());
				}
			}
			TestTrue(TEXT("field edit preserves unrelated node and connection identities"), HasCapturedGraphBindings(*Edited->GetModel()));
			TestTrue(
				TEXT("editable directory exposes explicit edited-unit selection"),
				Edited->GetEditedUnitIds().IsSet());
			FString EditedValueUnitId;
			TestTrue(
				TEXT("edited resolver preserves the exact integer field value"),
				HasExactInteger(*Edited->GetModel(), TEXT("9007199254740995"), &EditedValueUnitId));
			if (Edited->GetEditedUnitIds().IsSet())
			{
				TestTrue(
					TEXT("edited selection identifies the changed semantic unit"),
					Edited->GetEditedUnitIds()->Contains(EditedValueUnitId));
			}
		}
		else
		{
			AddError(EditedError.GetCode() + TEXT(":") + EditedError.GetSourceUnit() + TEXT(":") + EditedError.GetJsonPointer());
			return false;
		}

		EditableInteger->SetStringField(TEXT("field_set_ref"), TEXT("tampered.message"));
		if (!SerializeJsonObject(EditedDocument.ToSharedRef(), Text)) return false;
		TestTrue(
			TEXT("immutable envelope tamper fixture is written"),
			FFileHelper::SaveStringToFile(
				Text,
				*FullPath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));

		FUeAgentResourceTracker EnvelopeResources = MakeResolverTracker();
		FResolvedSemanticWorkspacePtr EnvelopeRejected;
		FWorkspaceError EnvelopeError;
		const bool bEnvelopeResolved = TryLoadHjsonImportSource(
			FWorkspaceSource::Directory(Workspace.Root),
			ESemanticWorkspaceMode::State,
			EnvelopeResources,
			EnvelopeRejected,
			EnvelopeError);
		TestFalse(
			TEXT("edited import still rejects immutable envelope changes"),
			bEnvelopeResolved);
		if (bEnvelopeResolved)
		{
			AddError(TEXT("envelope_tamper_unexpectedly_resolved"));
		}
		TestEqual(
			TEXT("envelope tamper failure is explicit"),
			EnvelopeError.GetCode(),
			FString(TEXT("semantic_workspace_envelope_mismatch")));
		TestFalse(
			TEXT("envelope-tampered workspace is not exposed"),
			EnvelopeRejected.IsValid());
	}
	// Exercise literal keyed values against this workspace's actual public-ref map.
	FCompositionRequest LiteralRequest;
	LiteralRequest.TargetKind = TEXT("ue.string_table.asset");
	LiteralRequest.ProfileId = TEXT("llm_authoring");
	LiteralRequest.BaseRevision = TEXT("fixture-literal-r1");
	UeAgentAuthoringState::FStateEntry LiteralEntry;
	LiteralEntry.SemanticId = TEXT("string_table.entries");
	LiteralEntry.TargetIdentity.TargetKind = TEXT("string_table");
	LiteralEntry.TargetIdentity.CanonicalKey = TEXT("string_table|asset_path=/Game/HX013/ST_Literal");
	LiteralEntry.TargetIdentity.Selectors = {{TEXT("asset_path"), TEXT("/Game/HX013/ST_Literal")}};
	LiteralEntry.Type.Kind = TEXT("object");
	LiteralEntry.bAvailable = true;
	LiteralEntry.bEditable = true;
	const auto Metadata = MakeShared<FJsonObject>();
	Metadata->SetStringField(TEXT("$uai"), TEXT("decimal"));
	Metadata->SetStringField(TEXT("value"), TEXT("7"));
	const auto Record = MakeShared<FJsonObject>();
	Record->SetStringField(TEXT("key"), TEXT("Entry"));
	Record->SetStringField(TEXT("source"), TEXT("original"));
	Record->SetObjectField(TEXT("metadata"), Metadata);
	const auto State = MakeShared<FJsonObject>();
	State->SetStringField(TEXT("namespace"), TEXT("original"));
	State->SetArrayField(TEXT("entries"), {MakeShared<FJsonValueObject>(Record)});
	LiteralEntry.Value = MakeShared<FJsonValueObject>(State);
	LiteralRequest.Entries = {LiteralEntry};
	auto LiteralResources = MakeResolverTracker();
	FAuthoringModelPtr LiteralModel;
	FExportWorkspacePlanPtr LiteralPlan;
	FWorkspaceLayoutPlanPtr LiteralLayout;
	if (!TryComposeAuthoringModel(LiteralRequest, LiteralResources, LiteralModel, Error)
		|| !TryBuildCompositionExportPlan(LiteralModel.ToSharedRef(), {}, LiteralResources, LiteralPlan, Error)
		|| !TryPlanWorkspaceLayout(LiteralModel.ToSharedRef(), 1, EWorkspaceSourceMode::Directory,
			{}, LiteralResources, LiteralLayout, Error))
	{
		AddError(Error.GetCode() + TEXT(":") + Error.GetJsonPointer());
		return false;
	}
	const auto& PublicRefs = LiteralLayout->GetMachineManifest().PublicRefMapping;
	if (!TestFalse(TEXT("literal fixture uses a real public reference mapping"), PublicRefs.IsEmpty())) return false;
	const FString LiteralRef = (*PublicRefs.begin()).Value;
	FScopedResolverWorkspace LiteralWorkspace;
	if (!WritePlan(*LiteralPlan, LiteralWorkspace.Root)) return false;
	const auto HasLiteral = [&](const FAuthoringModel& Model, const FString& Expected) {
		for (const FSemanticUnit& Unit : Model.GetUnits())
		{
			const auto* Entries = Unit.GetValues().Find(TEXT("entries"));
			const auto* Namespace = Unit.GetValues().Find(TEXT("namespace"));
			if (!Entries || !Namespace || (*Entries)->GetKind() != EUeAgentValueKind::Array
				|| (*Entries)->GetArrayItems().Num() != 1) continue;
			const auto& Item = (*Entries)->GetArrayItems()[0];
			const auto* Meta = ObjectField(&Item.Get(), TEXT("metadata"));
			return (*Namespace)->GetKind() == EUeAgentValueKind::String
				&& (*Namespace)->GetText().Equals(Expected, ESearchCase::CaseSensitive)
				&& StringField(&Item.Get(), TEXT("source")).Equals(Expected, ESearchCase::CaseSensitive)
				&& StringField(Meta, TEXT("$uai")).Equals(TEXT("decimal"), ESearchCase::CaseSensitive)
				&& StringField(Meta, TEXT("value")).Equals(TEXT("7"), ESearchCase::CaseSensitive);
		}
		return false;
	};
	FResolvedSemanticWorkspacePtr LiteralResolved;
	if (!TryResolveSemanticWorkspace(FWorkspaceSource::Directory(LiteralWorkspace.Root),
		ESemanticWorkspaceMode::State, LiteralResources, LiteralResolved, Error))
	{
		AddError(Error.GetCode() + TEXT(":") + Error.GetJsonPointer());
		return false;
	}
	TestTrue(TEXT("normal resolver preserves captured literal metadata"), HasLiteral(*LiteralResolved->GetModel(), TEXT("original")));
	bool bEditedLiteral = false;
	for (const auto& File : LiteralPlan->GetFiles())
	{
		if (File.GetRelativePath().StartsWith(TEXT(".uai/"))) continue;
		const FString Path = FPaths::Combine(LiteralWorkspace.Root, File.GetRelativePath());
		FString Text;
		TSharedPtr<FJsonObject> Document;
		if (!FFileHelper::LoadFileToString(Text, *Path) || !ParseJsonObject(Text, Document)) return false;
		if (!Document->HasField(TEXT("entries"))) continue;
		Document->SetStringField(TEXT("namespace"), LiteralRef);
		Document->GetObjectField(TEXT("entries"))->GetObjectField(TEXT("Entry"))->SetStringField(TEXT("source"), LiteralRef);
		if (!SerializeJsonObject(Document.ToSharedRef(), Text)
			|| !FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
		bEditedLiteral = true;
	}
	if (!TestTrue(TEXT("literal fixture edits the ordinary keyed document"), bEditedLiteral)) return false;
	LiteralResolved.Reset();
	if (!TryLoadHjsonImportSource(FWorkspaceSource::Directory(LiteralWorkspace.Root),
		ESemanticWorkspaceMode::State, LiteralResources, LiteralResolved, Error))
	{
		AddError(Error.GetCode() + TEXT(":") + Error.GetJsonPointer());
		return false;
	}
	TestTrue(TEXT("literal source and namespace matching public refs are not rewritten"), HasLiteral(*LiteralResolved->GetModel(), LiteralRef));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSemanticWorkspaceNumberProjectionTest,
	"UeAgentInterface.Core.HjsonWorkspace.SemanticWorkspaceResolver.NumberProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticWorkspaceNumberProjectionTest::RunTest(const FString& Parameters)
{
	FString RegistrationError;
	if (!TestTrue(TEXT("composition contracts register for numeric projection"),
		RegisterGraphTreeTargetCompositionContracts(RegistrationError)
			&& RegisterOtherTargetCompositionContracts(RegistrationError)))
	{
		AddError(RegistrationError);
		return false;
	}
	auto ComposeNumber = [&](TSharedRef<FJsonValue> Number, FAuthoringModelPtr& Model, FWorkspaceError& Error)
	{
		const FString NodeKey = TEXT("blueprint_node|blueprint_node|node_guid|node_guid=node-a");
		auto Property = MakeResolverProperty(NodeKey);
		Property.Value = Number;
		FCompositionRequest Request;
		Request.TargetKind = TEXT("ue.blueprint.asset");
		Request.ProfileId = TEXT("llm_authoring");
		Request.BaseRevision = TEXT("fixture-numeric-projection");
		Request.Entries = {MakeResolverGraph(), MakeResolverEntry(NodeKey, TEXT("node-a"), TEXT("First")), Property};
		FUeAgentResourceTracker Resources = MakeResolverTracker();
		return TryComposeAuthoringModel(Request, Resources, Model, Error);
	};
	auto CheckDecimal = [&](TSharedRef<FJsonValue> Number, const FString& Lexeme)
	{
		FAuthoringModelPtr Model;
		FWorkspaceError Error;
		if (!TestTrue(TEXT("finite numeric projection succeeds"), ComposeNumber(Number, Model, Error)))
		{
			AddError(Error.GetCode());
			return;
		}
		bool bMatched = false;
		for (const FSemanticUnit& Unit : Model->GetUnits())
		{
			const auto* Value = Unit.GetValues().Find(TEXT("value"));
			if (Unit.GetFieldSetRef() == TEXT("InputPriority") && Value)
				bMatched |= (*Value)->GetKind() == EUeAgentValueKind::Decimal && (*Value)->GetLexeme() == Lexeme;
		}
		TestTrue(TEXT("decimal kind and retained lexeme"), bMatched);
	};
	CheckDecimal(MakeShared<FJsonValueNumber>(1.25), TEXT("1.25"));
	CheckDecimal(MakeShared<FJsonValueNumberString>(TEXT("1.2500")), TEXT("1.2500"));
	CheckDecimal(MakeShared<FJsonValueNumberString>(TEXT("-0.0")), TEXT("-0.0"));
	// Composition retains exact input; the eventual native property owns its range.
	CheckDecimal(MakeShared<FJsonValueNumberString>(TEXT("1e99999")), TEXT("1e99999"));
	for (const FString& Lexeme : {FString(TEXT("NaN")),
		FString(TEXT("12oops")), FString(TEXT("12 # comment"))})
	{
		FAuthoringModelPtr Model;
		FWorkspaceError Error;
		TestFalse(TEXT("invalid or nonfinite NumberString is rejected"),
			ComposeNumber(MakeShared<FJsonValueNumberString>(Lexeme), Model, Error));
		TestFalse(TEXT("invalid numeric projection publishes no model"), Model.IsValid());
		TestFalse(TEXT("numeric rejection keeps a concrete code"), Error.GetCode().IsEmpty());
	}
	return true;
}
