#include "Misc/AutomationTest.h"

#include "Containers/StringConv.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceResolver.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentCompositionPropertyDefinitionTest,
	"UeAgentInterface.Core.HjsonWorkspace.Composition.PropertyDefinitionDedup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentCompositionPropertyDefinitionTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UeAgentAuthoringTypeDescriptors;

	FString RegistrationError;
	TestTrue(
		TEXT("level composition contracts register"),
		RegisterGraphTreeTargetCompositionContracts(RegistrationError)
			&& RegisterOtherTargetCompositionContracts(RegistrationError));

	const TSharedRef<FJsonObject> ValueObject = MakeShared<FJsonObject>();
	ValueObject->SetStringField(TEXT("value"), TEXT("Movable"));
	const TSharedRef<FJsonObject> CandidateHeader = MakeShared<FJsonObject>();
	CandidateHeader->SetStringField(TEXT("kind"), TEXT("enum"));
	auto CandidateRow = [](const TCHAR* Name)
	{
		return MakeShared<FJsonValueArray>(
			TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueString>(Name)});
	};
	const TSharedRef<FJsonValueArray> CandidateMovable = CandidateRow(TEXT("Movable"));
	const TSharedRef<FJsonValueArray> CandidateStatic = CandidateRow(TEXT("Static"));
	const TSharedRef<FJsonValueArray> CandidateStationary = CandidateRow(TEXT("Stationary"));
	const TSharedRef<FJsonValueArray> CandidateOther = CandidateRow(TEXT("Other"));

	const FDescriptor EnumType{
		TEXT("enum"), FString(), TEXT("/Script/Engine.EComponentMobility"),
		FString(), INDEX_NONE, nullptr, nullptr, nullptr, nullptr};
	auto MakeEntry = [&](const TCHAR* Id, bool bOther) {
		UeAgentAuthoringState::FStateEntry Entry;
		Entry.SemanticId = Id;
		Entry.TargetIdentity.CanonicalKey = TEXT("asset|/Game/Test/BP");
		Entry.TargetIdentity.Selectors.Add(TEXT("asset_path"), TEXT("/Game/Test/BP"));
		Entry.TargetIdentity.Selectors.Add(TEXT("property_path"), Id);
		Entry.Type = EnumType;
		Entry.Value = MakeShared<FJsonValueObject>(ValueObject);
		Entry.bAvailable = true;
		Entry.bEditable = true;
		Entry.Constraints.Add(TEXT("enum_path"), EnumType.EnumPath);
		Entry.CandidatesHeader = MakeShared<FJsonValueObject>(CandidateHeader);
		Entry.Candidates = MakeShared<FJsonValueArray>(
			TArray<TSharedPtr<FJsonValue>>{
				CandidateMovable,
				bOther ? TSharedPtr<FJsonValue>(CandidateOther)
					: TSharedPtr<FJsonValue>(CandidateStatic),
				CandidateStationary});
		return Entry;
	};

	UeAgentAuthoringState::FStateEntry First = MakeEntry(TEXT("MobilityA"), false);
	UeAgentAuthoringState::FStateEntry Second = MakeEntry(TEXT("MobilityB"), false);
	UeAgentAuthoringState::FStateEntry Changed = MakeEntry(TEXT("MobilityC"), true);
	First.CategoryPath = {TEXT("Rendering"), TEXT("Advanced")};
	Second.CategoryPath = {TEXT("Rendering"), TEXT("Advanced")};
	Changed.CategoryPath = {TEXT("Physics/Body")};
	UeAgentAuthoringState::FStateEntry Visible;
	Visible.SemanticId = TEXT("Visible");
	Visible.TargetIdentity.CanonicalKey = TEXT("asset|/Game/Test/BP");
	Visible.TargetIdentity.Selectors.Add(TEXT("asset_path"), TEXT("/Game/Test/BP"));
	Visible.TargetIdentity.Selectors.Add(TEXT("property_path"), TEXT("Visible"));
	Visible.Type = FDescriptor{
		TEXT("bool"), FString(), FString(), FString(), INDEX_NONE,
		nullptr, nullptr, nullptr, nullptr};
	const TSharedRef<FJsonObject> VisibleValue = MakeShared<FJsonObject>();
	VisibleValue->SetBoolField(TEXT("literal"), true);
	Visible.Value = MakeShared<FJsonValueObject>(VisibleValue);
	Visible.bAvailable = true;
	Visible.bEditable = true;
	UeAgentAuthoringState::FStateEntry Names;
	Names.SemanticId = TEXT("Names");
	Names.TargetIdentity.CanonicalKey = TEXT("asset|/Game/Test/BP");
	Names.TargetIdentity.Selectors.Add(TEXT("asset_path"), TEXT("/Game/Test/BP"));
	Names.TargetIdentity.Selectors.Add(TEXT("property_path"), TEXT("Names"));
	Names.Type.Kind = TEXT("array");
	Names.Type.ElementType = MakeShared<FDescriptor>();
	Names.Type.ElementType->Kind = TEXT("name");
	Names.Value = MakeShared<FJsonValueArray>(
		TArray<TSharedPtr<FJsonValue>>{
			MakeShared<FJsonValueString>(TEXT("First")),
			MakeShared<FJsonValueString>(TEXT("Second"))});
	Names.bAvailable = true;
	Names.bEditable = true;
	Names.CategoryPath = {TEXT("Rendering")};
	FCompositionRequest Request;
	Request.TargetKind = TEXT("ue.level.asset");
	Request.ProfileId = TEXT("llm_authoring");
	Request.BaseRevision = TEXT("definition-fixture:r1");
	Request.SourceProvenance = TEXT("test");
	Request.Entries = {First, Second, Changed, Visible, Names};

	const TMap<EUeAgentResourceDimension, int64> Limits{
		{EUeAgentResourceDimension::InputBytes, 4 * 1024 * 1024},
		{EUeAgentResourceDimension::OutputBytes, 4 * 1024 * 1024},
		{EUeAgentResourceDimension::StringBytes, 4 * 1024 * 1024},
		{EUeAgentResourceDimension::Nodes, 4096},
		{EUeAgentResourceDimension::Items, 4096},
		{EUeAgentResourceDimension::Files, 4096},
		{EUeAgentResourceDimension::ReferenceEdges, 4096},
		{EUeAgentResourceDimension::WorkUnits, 32768},
		{EUeAgentResourceDimension::EstimatedMemoryBytes, 32 * 1024 * 1024}};
	FUeAgentResourceTracker Resources;
	FAuthoringModelPtr Model;
	FWorkspaceError Error;
	TestTrue(
		TEXT("composition with property definitions compiles"),
		TryComposeAuthoringModel(Request, Resources, Model, Error));
	if (!Model.IsValid()) return false;

	TArray<const FSemanticUnit*> Definitions;
	TMap<FString, const FSemanticUnit*> Properties;
	for (const FSemanticUnit& Unit : Model->GetUnits())
	{
		if (Unit.GetRole() == TEXT("property_definition")) Definitions.Add(&Unit);
		if (Unit.GetUnitKind() == ESemanticUnitKind::PropertySet
			&& Unit.GetFieldSetRef().StartsWith(TEXT("Mobility")))
		{
			Properties.Add(Unit.GetFieldSetRef(), &Unit);
		}
	}
	TestEqual(TEXT("equal metadata is deduplicated"), Definitions.Num(), 4);
	TestEqual(TEXT("three enum properties are represented"), Properties.Num(), 3);
	if (Definitions.Num() == 4 && Properties.Num() == 3)
	{
		const FSemanticUnit* FirstProperty = Properties.FindRef(TEXT("MobilityA"));
		const FSemanticUnit* SecondProperty = Properties.FindRef(TEXT("MobilityB"));
		const FSemanticUnit* ChangedProperty = Properties.FindRef(TEXT("MobilityC"));
		if (FirstProperty == nullptr || SecondProperty == nullptr || ChangedProperty == nullptr)
		{
			AddError(TEXT("named property fixtures are missing"));
			return false;
		}
		TestTrue(TEXT("properties reference definitions"), !FirstProperty->GetDependencyRefs().IsEmpty());
		TestEqual(TEXT("equal properties reference one definition"), FirstProperty->GetDependencyRefs()[0], SecondProperty->GetDependencyRefs()[0]);
		TestNotEqual(TEXT("changed metadata has another definition"), FirstProperty->GetDependencyRefs()[0], ChangedProperty->GetDependencyRefs()[0]);
		const FSemanticUnit* EnumDefinition =
			Model->FindUnit(FirstProperty->GetDependencyRefs()[0]);
		TestNotNull(TEXT("enum dependency resolves to its definition"), EnumDefinition);
		if (EnumDefinition != nullptr)
		{
			TestTrue(TEXT("definition keeps type"),
				EnumDefinition->GetValues().Contains(TEXT("type")));
			TestTrue(TEXT("definition keeps candidates"),
				EnumDefinition->GetValues().Contains(TEXT("candidates")));
			TestTrue(TEXT("definition keeps candidate header"),
				EnumDefinition->GetValues().Contains(TEXT("candidates_header")));
		}
		for (const FSemanticUnit* Definition : Definitions)
		{
			TestEqual(TEXT("definition is a collection unit"), Definition->GetUnitKind(), ESemanticUnitKind::CollectionState);
		}
	}

	FExportWorkspacePlanPtr Plan;
	TestTrue(
		TEXT("definition export plan builds"),
		TryBuildCompositionExportPlan(
			Model.ToSharedRef(),
			FCompositionSnapshotBinding(
				TEXT("snapshot:definitions"),
				UE::UAI::StateSemantic::FUeAgentStateRevision(TEXT("test"), TEXT("r1"), 1),
				TEXT("evidence:definitions")),
			TMap<FString, int64>{}, Resources, Plan, Error));

	UeAgentAuthoringState::FStateEntry Duplicate =
		MakeEntry(TEXT("MobilityDuplicate"), false);
	Duplicate.TargetIdentity.CanonicalKey = TEXT("asset|/Game/Test/BP|duplicate");
	Duplicate.TargetIdentity.Selectors.Add(TEXT("property_path"), TEXT("MobilityA"));
	Duplicate.CategoryPath = {TEXT("DifferentCategory")};
	FCompositionRequest DuplicateRequest = Request;
	DuplicateRequest.BaseRevision = TEXT("definition-fixture:duplicate");
	DuplicateRequest.Entries = {First, Duplicate};
	FUeAgentResourceTracker DuplicateResources;
	FAuthoringModelPtr DuplicateModel;
	FWorkspaceError DuplicateError;
	TestTrue(TEXT("cross-category duplicate fixture composes"),
		TryComposeAuthoringModel(
			DuplicateRequest, DuplicateResources, DuplicateModel, DuplicateError));
	if (DuplicateModel.IsValid())
	{
		FExportWorkspacePlanPtr DuplicatePlan;
		FUeAgentResourceTracker DuplicateExportResources;
		TestFalse(TEXT("same property name in different categories is rejected"),
			TryBuildCompositionExportPlan(
				DuplicateModel.ToSharedRef(),
				FCompositionSnapshotBinding(
					TEXT("snapshot:duplicate-property"),
					UE::UAI::StateSemantic::FUeAgentStateRevision(
						TEXT("test"), TEXT("duplicate"), 1),
					TEXT("evidence:duplicate-property")),
				TMap<FString, int64>{}, DuplicateExportResources,
				DuplicatePlan, DuplicateError));
		TestEqual(TEXT("cross-category duplicate reports the stable error"),
			DuplicateError.GetCode(),
			FString(TEXT("logical_path_collision")));
	}
	if (Plan.IsValid())
	{
		int32 DefinitionContainers = 0;
		const FExportFilePlan* PropertyContainer = nullptr;
		const FExportFilePlan* DefinitionContainer = nullptr;
		const FExportFilePlan* ManifestFile = nullptr;
		TArray<FString> VisibleOrderingDocuments;
		for (const FExportFilePlan& File : Plan->GetFiles())
		{
			if (File.GetRelativePath().EndsWith(TEXT("/definitions.hjson")))
			{
				++DefinitionContainers;
				DefinitionContainer = &File;
			}
			if (File.GetRelativePath().EndsWith(TEXT("/properties.hjson")))
			{
				const FUTF8ToTCHAR PropertyText(
					reinterpret_cast<const ANSICHAR*>(File.GetCanonicalUtf8().GetData()),
					File.GetCanonicalUtf8().Num());
				const FString VisiblePropertyText(
					PropertyText.Length(), PropertyText.Get());
				if (VisiblePropertyText.Contains(TEXT("MobilityA"))
					&& VisiblePropertyText.Contains(TEXT("Visible")))
				{
					PropertyContainer = &File;
				}
			}
			if (File.GetRelativePath() == TEXT(".uai/workspace.hjson"))
			{
				ManifestFile = &File;
			}
			if (File.GetRelativePath().EndsWith(TEXT("/ordering.hjson")))
			{
				const FUTF8ToTCHAR OrderingText(
					reinterpret_cast<const ANSICHAR*>(File.GetCanonicalUtf8().GetData()),
					File.GetCanonicalUtf8().Num());
				VisibleOrderingDocuments.Add(
					FString(OrderingText.Length(), OrderingText.Get()));
			}
		}
		TestEqual(TEXT("definitions use one physical container"), DefinitionContainers, 1);
		for (const FString& VisibleOrdering : VisibleOrderingDocuments)
		{
			TestFalse(
				TEXT("ordinary ordering omits hashed property-definition identities"),
				VisibleOrdering.Contains(TEXT("property_definition:")));
		}
		auto ParseFile = [](const FExportFilePlan* File, TSharedPtr<FJsonObject>& Out)
		{
			if (File == nullptr) return false;
			const FUTF8ToTCHAR Text(
				reinterpret_cast<const ANSICHAR*>(File->GetCanonicalUtf8().GetData()),
				File->GetCanonicalUtf8().Num());
			FString ParseError;
			return FUeAgentStructuredHjsonParser::ParseObject(
				FString(Text.Length(), Text.Get()), File->GetRelativePath(), Out, ParseError);
		};
		TSharedPtr<FJsonObject> PropertiesDocument;
		FString MobilityDefinition, ChangedMobilityDefinition;
		TestTrue(TEXT("keyed properties parse"), ParseFile(PropertyContainer, PropertiesDocument));
		const TSharedPtr<FJsonObject>* Categories = nullptr;
		if (PropertiesDocument.IsValid()
			&& PropertiesDocument->TryGetObjectField(TEXT("categories"), Categories)
			&& Categories != nullptr)
		{
			const TSharedPtr<FJsonObject> Advanced =
				(*Categories)->GetObjectField(TEXT("Rendering|Advanced"));
			const TSharedPtr<FJsonObject> Physics =
				(*Categories)->GetObjectField(TEXT("Physics/Body"));
			const TSharedPtr<FJsonObject> General =
				(*Categories)->GetObjectField(TEXT("General"));
			const TSharedPtr<FJsonObject> Rendering =
				(*Categories)->GetObjectField(TEXT("Rendering"));
			const TSharedPtr<FJsonObject> Mobility = Advanced->GetObjectField(TEXT("MobilityA"));
			const TSharedPtr<FJsonObject> Bool = General->GetObjectField(TEXT("Visible"));
			const TSharedPtr<FJsonObject> NameArray = Rendering->GetObjectField(TEXT("Names"));
			TestTrue(TEXT("same category contains multiple properties"),
				Advanced->HasField(TEXT("MobilityA"))
					&& Advanced->HasField(TEXT("MobilityB")));
			TestTrue(TEXT("separate category contains its property"),
				Physics->HasField(TEXT("MobilityC")));
			MobilityDefinition = Mobility->GetStringField(TEXT("definition"));
			ChangedMobilityDefinition = Physics->GetObjectField(TEXT("MobilityC"))->GetStringField(TEXT("definition"));
			TestEqual(TEXT("enum type retains its complete native identity"),
				Mobility->GetStringField(TEXT("type")), FString(TEXT("/Script/Engine.EComponentMobility")));
			TestEqual(TEXT("candidate contracts do not rename the native type"), Mobility->GetStringField(TEXT("type")),
				Physics->GetObjectField(TEXT("MobilityC"))->GetStringField(TEXT("type")));
			TestEqual(TEXT("equal candidate contracts share a definition"), MobilityDefinition,
				Advanced->GetObjectField(TEXT("MobilityB"))->GetStringField(TEXT("definition")));
			TestNotEqual(TEXT("different candidate contracts retain distinct definitions"), MobilityDefinition, ChangedMobilityDefinition);
			const FString NativeType(TEXT("/Script/Engine.EComponentMobility"));
			TestTrue(TEXT("definition aliases use X and X#2, never X#1"),
				(MobilityDefinition == NativeType && ChangedMobilityDefinition == NativeType + TEXT("#2"))
				|| (ChangedMobilityDefinition == NativeType && MobilityDefinition == NativeType + TEXT("#2")));
			TestEqual(TEXT("enum selection is direct"),
				Mobility->GetStringField(TEXT("value")), FString(TEXT("Movable")));
			TestEqual(TEXT("bool type uses bool"),
				Bool->GetStringField(TEXT("type")), FString(TEXT("bool")));
			TestTrue(TEXT("bool value is direct"), Bool->GetBoolField(TEXT("value")));
			TestEqual(TEXT("array element type is directly readable"),
				NameArray->GetStringField(TEXT("type")), FString(TEXT("name[]")));
			const TArray<TSharedPtr<FJsonValue>>* NameValues = nullptr;
			TestTrue(TEXT("array literal wrappers are recursively hidden"),
				NameArray->TryGetArrayField(TEXT("value"), NameValues)
					&& NameValues != nullptr && NameValues->Num() == 2);
			if (NameValues != nullptr && NameValues->Num() == 2)
			{
				TestEqual(TEXT("first name is direct"),
					(*NameValues)[0]->AsString(), FString(TEXT("First")));
				TestEqual(TEXT("second name is direct"),
					(*NameValues)[1]->AsString(), FString(TEXT("Second")));
			}
			TestFalse(TEXT("property hides unit id"), Mobility->HasField(TEXT("unit_id")));
			TestFalse(TEXT("property hides dependencies"), Mobility->HasField(TEXT("dependency_refs")));
			TestFalse(TEXT("property hides field-set metadata"), Mobility->HasField(TEXT("field_set_ref")));
		}
		else
		{
			AddError(TEXT("categories object is missing"));
		}
		TestFalse(TEXT("flat properties object is omitted"),
			PropertiesDocument.IsValid()
				&& PropertiesDocument->HasField(TEXT("properties")));
		if (ManifestFile != nullptr)
		{
			const FUTF8ToTCHAR ManifestText(
				reinterpret_cast<const ANSICHAR*>(
					ManifestFile->GetCanonicalUtf8().GetData()),
				ManifestFile->GetCanonicalUtf8().Num());
			const FString Manifest(ManifestText.Length(), ManifestText.Get());
			TestTrue(TEXT("manifest points to hierarchical category property"),
				Manifest.Contains(TEXT("/categories/Rendering|Advanced/MobilityA")));
			TestTrue(TEXT("manifest points to General fallback property"),
				Manifest.Contains(TEXT("/categories/General/Visible")));
			TestTrue(TEXT("manifest JSON pointer escapes category separators"),
				Manifest.Contains(TEXT("/categories/Physics~1Body/MobilityC")));
		}
		else
		{
			AddError(TEXT("workspace manifest is missing"));
		}
		TSharedPtr<FJsonObject> DefinitionsDocument;
		TestTrue(TEXT("keyed definitions parse"), ParseFile(DefinitionContainer, DefinitionsDocument));
		if (DefinitionContainer != nullptr)
		{
			const FUTF8ToTCHAR DefinitionText(
				reinterpret_cast<const ANSICHAR*>(
					DefinitionContainer->GetCanonicalUtf8().GetData()),
				DefinitionContainer->GetCanonicalUtf8().Num());
			const FString VisibleDefinitions(DefinitionText.Length(), DefinitionText.Get());
			TestFalse(TEXT("definitions omit machine unit arrays"),
				VisibleDefinitions.Contains(TEXT("units")));
			TestFalse(TEXT("definitions omit hashed definition identity"),
				VisibleDefinitions.Contains(TEXT("property_definition:")));
			TestFalse(TEXT("definitions omit per-property dependency references"),
				VisibleDefinitions.Contains(TEXT("dependency_refs")));
		}
		const TSharedPtr<FJsonObject>* Enums = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* EnumNames = nullptr;
		TestTrue(TEXT("enum candidates resolve through the property's definition reference"),
			DefinitionsDocument.IsValid()
				&& DefinitionsDocument->TryGetObjectField(TEXT("enums"), Enums)
				&& Enums != nullptr
				&& (*Enums)->TryGetArrayField(MobilityDefinition, EnumNames)
				&& EnumNames != nullptr);
		if (EnumNames != nullptr)
		{
			TestEqual(TEXT("equal enum contracts are deduplicated without merging other contracts"), EnumNames->Num(), 3);
			TestTrue(TEXT("first contract retains Static"), EnumNames->ContainsByPredicate([](const auto& V) { return V->AsString() == TEXT("Static"); }));
			TestFalse(TEXT("first contract does not acquire Other"), EnumNames->ContainsByPredicate([](const auto& V) { return V->AsString() == TEXT("Other"); }));
		}
		const TArray<TSharedPtr<FJsonValue>>* ChangedNames = nullptr;
		if (TestTrue(TEXT("changed enum contract has its own catalog"), Enums && (*Enums)->TryGetArrayField(ChangedMobilityDefinition, ChangedNames)))
		{
			TestEqual(TEXT("changed enum contract retains exactly its candidates"), ChangedNames->Num(), 3);
			TestTrue(TEXT("changed contract retains Other"), ChangedNames->ContainsByPredicate([](const auto& V) { return V->AsString() == TEXT("Other"); }));
			TestFalse(TEXT("changed contract does not acquire Static"), ChangedNames->ContainsByPredicate([](const auto& V) { return V->AsString() == TEXT("Static"); }));
		}

		const FString WorkspaceRoot = FPaths::Combine(
			FPaths::ProjectIntermediateDir(), TEXT("UAI/Tests/KeyedPropertyProjection"),
			FGuid::NewGuid().ToString(EGuidFormats::Digits));
		bool bPersisted = true;
		for (const FExportFilePlan& File : Plan->GetFiles())
		{
			const FString FullPath = FPaths::Combine(WorkspaceRoot, File.GetRelativePath());
			bPersisted = bPersisted
				&& IFileManager::Get().MakeDirectory(*FPaths::GetPath(FullPath), true)
				&& FFileHelper::SaveArrayToFile(File.GetCanonicalUtf8(), *FullPath);
		}
		TestTrue(TEXT("keyed property workspace persists"), bPersisted);
		FUeAgentResourceTracker ResolveResources;
		FResolvedSemanticWorkspacePtr Resolved;
		FWorkspaceError ResolveError;
		TestTrue(TEXT("keyed properties restore through the common resolver"),
			bPersisted && TryResolveSemanticWorkspace(
				FWorkspaceSource::Directory(WorkspaceRoot), ESemanticWorkspaceMode::State,
				ResolveResources, Resolved, ResolveError));
		if (Resolved.IsValid())
		{
			TestEqual(TEXT("keyed projection restores the canonical model"),
				Resolved->GetModel()->GetSemanticFingerprint(), Model->GetSemanticFingerprint());
		}
		if (PropertyContainer != nullptr)
		{
			const FString PropertyPath = FPaths::Combine(
				WorkspaceRoot, PropertyContainer->GetRelativePath());
			FString PropertyText;
			TestTrue(TEXT("category property file can be read for value edit"),
				FFileHelper::LoadFileToString(PropertyText, *PropertyPath));
			TSharedPtr<FJsonObject> EditedProperties;
			FString PropertyError;
			const bool bParsedEdit = FUeAgentStructuredHjsonParser::ParseObject(
				PropertyText, PropertyPath, EditedProperties, PropertyError);
			TestTrue(TEXT("nested category property value is selected for edit"), bParsedEdit);
			if (!bParsedEdit) return false;
			const auto EditableMobility = EditedProperties->GetObjectField(TEXT("categories"))
				->GetObjectField(TEXT("Rendering|Advanced"))->GetObjectField(TEXT("MobilityA"));
			EditableMobility->SetStringField(TEXT("value"), TEXT("Static"));
			TestTrue(TEXT("nested category property value edit persists"),
				FUeAgentStructuredHjsonFormatter::TryWriteObject(EditedProperties.ToSharedRef(), PropertyText, PropertyError)
					&& FFileHelper::SaveStringToFile(PropertyText, *PropertyPath));
			FUeAgentResourceTracker EditedResources;
			FResolvedSemanticWorkspacePtr Edited;
			FWorkspaceError EditedError;
			TestTrue(TEXT("nested manifest pointer consumes edited category value"),
				TryResolveEditedSemanticWorkspace(
					FWorkspaceSource::Directory(WorkspaceRoot),
					ESemanticWorkspaceMode::State,
					EditedResources, Edited, EditedError));
			if (Edited.IsValid())
			{
				TestNotEqual(TEXT("edited category value changes semantic fingerprint"),
					Edited->GetModel()->GetSemanticFingerprint(),
					Model->GetSemanticFingerprint());
			}

			EditableMobility->SetStringField(TEXT("type"), TEXT("bool"));
			TestTrue(TEXT("mismatching property type probe persists"),
				FUeAgentStructuredHjsonFormatter::TryWriteObject(EditedProperties.ToSharedRef(), PropertyText, PropertyError)
					&& FFileHelper::SaveStringToFile(PropertyText, *PropertyPath));
			FUeAgentResourceTracker MismatchResources;
			FResolvedSemanticWorkspacePtr Mismatching;
			FWorkspaceError MismatchError;
			TestFalse(TEXT("mismatching model type is rejected by the common resolver"),
				TryResolveEditedSemanticWorkspace(
					FWorkspaceSource::Directory(WorkspaceRoot), ESemanticWorkspaceMode::State,
					MismatchResources, Mismatching, MismatchError));
			TestEqual(TEXT("mismatching model type reports the stable error"),
				MismatchError.GetCode(), FString(TEXT("semantic_workspace_property_type_mismatch")));
		}
		IFileManager::Get().DeleteDirectory(*WorkspaceRoot, false, true);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentCompositionItemValueDefinitionTest,
	"UeAgentInterface.Core.HjsonWorkspace.Composition.ItemValueDefinition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentCompositionItemValueDefinitionTest::RunTest(const FString&)
{
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::ContextResource;
	FString RegistrationError;
	FTargetCompositionContract Contract;
	Contract.SchemaId = GetTargetCompositionContractSchemaId();
	Contract.TargetKind = TEXT("ue.test.item_definition");
	Contract.Surfaces = {
		{TEXT("target_settings"), EAuthoringSurfaceKind::Properties, ESemanticUnitKind::PropertySet},
		{TEXT("items"), EAuthoringSurfaceKind::Collection, ESemanticUnitKind::CollectionState}};
	Contract.SemanticRoutes = {{ECompositionSemanticRouteMatch::Default, {}, TEXT("items"), ESemanticUnitKind::ItemState, ECompositionSemanticRouteRole::Item, 0}};
	if (!TestTrue(TEXT("generic item contract registers"), FTargetCompositionRegistry::Get().RegisterContract(Contract, RegistrationError)))
	{ AddError(RegistrationError); return false; }
	UeAgentAuthoringState::FStateEntry Item;
	Item.SemanticId = TEXT("test.item");
	Item.TargetIdentity.CanonicalKey = TEXT("test-item");
	Item.TargetIdentity.Selectors = {{TEXT("asset_path"), TEXT("/Game/Test/ItemDefinition")}, {TEXT("item_id"), TEXT("item")}};
	Item.Type.Kind = TEXT("object");
	Item.bAvailable = true;
	Item.bEditable = true;
	auto Business = MakeShared<FJsonObject>();
	Business->SetNumberField(TEXT("value"), 0.5);
	Item.Value = MakeShared<FJsonValueObject>(Business);
	FCompositionRequest Request;
	Request.TargetKind = Contract.TargetKind;
	Request.ProfileId = TEXT("llm_authoring");
	Request.BaseRevision = TEXT("item-definition:r1");
	Request.SourceProvenance = TEXT("test");
	const TMap<EUeAgentResourceDimension, int64> Limits{
		{EUeAgentResourceDimension::InputBytes, 4194304}, {EUeAgentResourceDimension::OutputBytes, 4194304},
		{EUeAgentResourceDimension::StringBytes, 4194304}, {EUeAgentResourceDimension::Nodes, 4096},
		{EUeAgentResourceDimension::Items, 4096}, {EUeAgentResourceDimension::Files, 4096},
		{EUeAgentResourceDimension::ReferenceEdges, 4096}, {EUeAgentResourceDimension::WorkUnits, 32768},
		{EUeAgentResourceDimension::EstimatedMemoryBytes, 33554432}};
	FString ItemId;
	for (const bool bWithDefinition : {false, true})
	{
		if (bWithDefinition)
		{
			auto Type = MakeShared<FJsonObject>(); Type->SetStringField(TEXT("kind"), TEXT("float"));
			auto NativeDefinition = MakeShared<FJsonObject>(); NativeDefinition->SetObjectField(TEXT("type"), Type);
			auto Fields = MakeShared<FJsonObject>(); Fields->SetObjectField(TEXT("value"), NativeDefinition);
			auto Definition = MakeShared<FJsonObject>(); Definition->SetObjectField(TEXT("fields"), Fields);
			Item.ValueDefinition = MakeShared<FJsonValueObject>(Definition);
		}
		Request.Entries = {Item};
		FUeAgentResourceTracker Resources;
		FAuthoringModelPtr Model; FWorkspaceError Error;
		if (!TestTrue(TEXT("generic item composes"), TryComposeAuthoringModel(Request, Resources, Model, Error)))
		{ AddError(Error.GetCode()); return false; }
		const FSemanticUnit* Unit = nullptr;
		int32 Definitions = 0;
		for (const auto& Candidate : Model->GetUnits())
		{
			if (Candidate.GetUnitKind() == ESemanticUnitKind::ItemState) Unit = &Candidate;
			if (Candidate.GetRole() == TEXT("property_definition")) ++Definitions;
		}
		if (!TestNotNull(TEXT("item retained as item"), Unit)) return false;
		if (ItemId.IsEmpty()) ItemId = Unit->GetUnitId();
		TestEqual(TEXT("definitions do not alter item identity"), Unit->GetUnitId(), ItemId);
		TestEqual(TEXT("only business value retained"), Unit->GetValues().Num(), 1);
		TestEqual(TEXT("definition opt-in catalog count"), Definitions, bWithDefinition ? 2 : 0);
		TestEqual(TEXT("definition opt-in binding"), !Unit->GetFieldSetRef().IsEmpty(), bWithDefinition);
		TArray<uint8> Bytes;
		FWorkspaceError SerializationError;
		if (!TestTrue(TEXT("ordinary item serializes"), TrySerializeSemanticModelUnitRow(*Unit, Bytes, SerializationError))) return false;
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
		TSharedPtr<FJsonObject> Row; FString ParseError;
		if (!FUeAgentStructuredHjsonParser::ParseObject(FString(Converted.Length(), Converted.Get()), TEXT("item"), Row, ParseError)) return false;
		TestEqual(TEXT("definition metadata only present when captured"), Row->HasField(TEXT("definition")), bWithDefinition);
		TestEqual(TEXT("ordinary business value unchanged"), Row->GetObjectField(TEXT("values"))->GetNumberField(TEXT("value")), 0.5);
	}
	return !HasAnyErrors();
}
