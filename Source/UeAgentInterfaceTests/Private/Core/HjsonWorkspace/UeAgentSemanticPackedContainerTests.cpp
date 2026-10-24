#include "Misc/AutomationTest.h"

#include "Containers/StringConv.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceResolver.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::HjsonWorkspace;

	TSharedPtr<FJsonObject> FindFirstPackedProperty(const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject>* Categories = nullptr;
		if (!Document.IsValid() || !Document->TryGetObjectField(TEXT("categories"), Categories)) return nullptr;
		TSharedPtr<FJsonObject> Found;
		for (const auto& Category : (*Categories)->Values)
		{
			const TSharedPtr<FJsonObject>* Row = nullptr;
			const auto Object = Category.Value->AsObject();
			if (!Object.IsValid() || !Object->TryGetObjectField(TEXT("first"), Row)) continue;
			if (Found.IsValid()) return nullptr;
			Found = *Row;
		}
		return Found;
	}

	bool WritePackedPlan(
		const FExportWorkspacePlan& Plan,
		const FString& Root)
	{
		for (const FExportFilePlan& File : Plan.GetFiles())
		{
			const FString FullPath =
				FPaths::Combine(Root, File.GetRelativePath());
			if (!IFileManager::Get().MakeDirectory(
					*FPaths::GetPath(FullPath), true)
				|| !FFileHelper::SaveArrayToFile(
					File.GetCanonicalUtf8(), *FullPath))
			{
				return false;
			}
		}
		return true;
	}

	class FScopedPackedWorkspace final
	{
	public:
		FScopedPackedWorkspace()
			: Root(FPaths::Combine(
				FPaths::ProjectIntermediateDir(),
				TEXT("UAI/Tests/PackedSemantic"),
				FGuid::NewGuid().ToString(EGuidFormats::Digits)))
		{
		}

		~FScopedPackedWorkspace()
		{
			IFileManager::Get().DeleteDirectory(*Root, false, true);
		}

		FString Root;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSemanticPackedPropertyContainerTest,
	"UeAgentInterface.Core.HjsonWorkspace.SemanticLayout.PackedPropertyContainer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticPackedPropertyContainerTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::StateSemantic;

	const FString TargetId = TEXT("target:packed");
	const TMap<FString, FUeAgentCanonicalValueRef> EmptyValues;
	TArray<FSemanticUnit> Units;
	Units.Emplace(
		TargetId,
		ESemanticUnitKind::TargetState,
		FString(),
		TEXT("ue.test.asset"),
		TEXT("authoring_target"),
		FString(),
		EmptyValues,
		TArray<FString>{}, TArray<FString>{}, FString(), FString(),
		TArray<FString>{}, TArray<FString>{TEXT("state")},
		TEXT("fixture:packed"));
	for (const TCHAR* FieldSet : {TEXT("first"), TEXT("second")})
	{
		Units.Emplace(
			FString(TEXT("property:packed:")) + FieldSet,
			ESemanticUnitKind::PropertySet,
			TargetId,
			TEXT("uai.authoring.property_set/1"),
			TEXT("editable_properties"),
			FieldSet,
			EmptyValues,
			TArray<FString>{}, TArray<FString>{}, FString(), FString(),
			TArray<FString>{}, TArray<FString>{TEXT("state")},
			TEXT("fixture:packed"));
	}

	const TMap<EUeAgentResourceDimension, int64> Limits{
		{EUeAgentResourceDimension::InputBytes, 1024 * 1024},
		{EUeAgentResourceDimension::OutputBytes, 1024 * 1024},
		{EUeAgentResourceDimension::StringBytes, 1024 * 1024},
		{EUeAgentResourceDimension::Nodes, 1024},
		{EUeAgentResourceDimension::Items, 1024},
		{EUeAgentResourceDimension::Files, 1024},
		{EUeAgentResourceDimension::ReferenceEdges, 1024},
		{EUeAgentResourceDimension::WorkUnits, 8192},
		{EUeAgentResourceDimension::EstimatedMemoryBytes, 8 * 1024 * 1024}};
	FUeAgentResourceTracker Resources;
	FAuthoringModelPtr Model;
	FWorkspaceError Error;
	TestTrue(
		TEXT("fixture model compiles"),
		TryCompileAuthoringModel(
			TEXT("uai.authoring.composition/2"), 2, TEXT("llm_authoring"),
			TEXT("packed:r1"), MoveTemp(Units), {TargetId}, Resources, Model,
			Error));
	if (!Model.IsValid())
	{
		return false;
	}
	FExportWorkspacePlanPtr Plan;
	TestTrue(
		TEXT("composition plan materializes"),
		TryBuildCompositionExportPlan(
			Model.ToSharedRef(),
			FCompositionSnapshotBinding(
				TEXT("snapshot:packed"),
				FUeAgentStateRevision(TEXT("fixture"), TEXT("r1"), 1),
				TEXT("evidence:packed")),
			TMap<FString, int64>{}, Resources, Plan, Error));
	if (!Plan.IsValid())
	{
		return false;
	}
	int32 PropertyFiles = 0;
	const FExportFilePlan* PackedFile = nullptr;
	const FExportFilePlan* ManifestFile = nullptr;
	for (const FExportFilePlan& File : Plan->GetFiles())
	{
		if (File.GetRelativePath().EndsWith(TEXT("/properties.hjson")))
		{
			++PropertyFiles;
			PackedFile = &File;
		}
		if (File.GetRelativePath().EndsWith(TEXT(".uai/workspace.hjson")))
		{
			ManifestFile = &File;
		}
	}
	TestEqual(TEXT("same-owner property sets share one container"), PropertyFiles, 1);
	const FString FirstId = TEXT("property:packed:first");
	const FString SecondId = TEXT("property:packed:second");
	if (ManifestFile != nullptr)
	{
		const FUTF8ToTCHAR ManifestText(
			reinterpret_cast<const ANSICHAR*>(ManifestFile->GetCanonicalUtf8().GetData()),
			ManifestFile->GetCanonicalUtf8().Num());
		const FString Manifest(ManifestText.Length(), ManifestText.Get());
		TestTrue(
			TEXT("manifest uses structured multiline HJSON"),
			Manifest.Contains(TEXT("\n  ")));
		TSharedPtr<FJsonObject> ParsedManifest;
		FString ManifestParseError;
		TestTrue(
			TEXT("manifest parses through the common HJSON formatter"),
			FUeAgentStructuredHjsonParser::ParseObject(
				Manifest,
				TEXT("semantic-packed-manifest.hjson"),
				ParsedManifest,
				ManifestParseError));
		TestTrue(TEXT("manifest keeps first property unit"), Manifest.Contains(FirstId));
		TestTrue(TEXT("manifest keeps second property unit"), Manifest.Contains(SecondId));
		TestTrue(
			TEXT("manifest projects shared physical property document"),
			Manifest.Contains(TEXT("properties.hjson")));
		TestTrue(
			TEXT("manifest projects keyed property pointers"),
			Manifest.Contains(TEXT("/properties/first"))
				&& Manifest.Contains(TEXT("/properties/second")));
		TestTrue(TEXT("manifest carries immutable envelope"), Manifest.Contains(TEXT("envelope")));
	}
	else
	{
		AddError(TEXT("machine manifest file is missing"));
	}
	if (PackedFile != nullptr)
	{
		const FUTF8ToTCHAR Text(
			reinterpret_cast<const ANSICHAR*>(PackedFile->GetCanonicalUtf8().GetData()),
			PackedFile->GetCanonicalUtf8().Num());
		const FString PackedText(Text.Length(), Text.Get());
		TestTrue(
			TEXT("packed semantic document uses structured multiline HJSON"),
			PackedText.Contains(TEXT("\n  ")));
		TSharedPtr<FJsonObject> ParsedPackedDocument;
		FString PackedParseError;
		TestTrue(
			TEXT("packed semantic document parses through the common HJSON formatter"),
			FUeAgentStructuredHjsonParser::ParseObject(
				PackedText,
				TEXT("semantic-packed-properties.hjson"),
				ParsedPackedDocument,
				PackedParseError));
		TestTrue(TEXT("container exposes category-grouped keyed properties"), FindFirstPackedProperty(ParsedPackedDocument).IsValid());
		TestFalse(TEXT("container omits packed unit arrays"), PackedText.Contains(TEXT("units")));
		TestTrue(TEXT("property exposes direct type"), PackedText.Contains(TEXT("type")));
		TestTrue(TEXT("property exposes direct value"), PackedText.Contains(TEXT("value")));
		TestFalse(TEXT("compact row omits unit kind"), PackedText.Contains(TEXT("unit_kind")));
		TestFalse(TEXT("compact row omits owner envelope"), PackedText.Contains(TEXT("owner_ref")));
		TestFalse(TEXT("compact row omits target identity"), PackedText.Contains(TEXT("target_identity_key")));
		TestFalse(TEXT("compact row omits profile visibility"), PackedText.Contains(TEXT("profile_visibility")));
	}

	FScopedPackedWorkspace Workspace;
	TestTrue(
		TEXT("packed workspace is persisted"),
		WritePackedPlan(*Plan, Workspace.Root));
	FUeAgentResourceTracker ResolveResources;
	FResolvedSemanticWorkspacePtr Resolved;
	const bool bResolved = TryResolveSemanticWorkspace(
			FWorkspaceSource::Directory(Workspace.Root),
			ESemanticWorkspaceMode::State,
			ResolveResources,
			Resolved,
			Error);
	TestTrue(TEXT("compact rows hydrate from the manifest envelope"), bResolved);
	if (!bResolved)
	{
		AddError(FString::Printf(
			TEXT("compact workspace resolve failed: %s source=%s pointer=%s"),
			*Error.GetCode(),
			*Error.GetSourceUnit(),
			*Error.GetJsonPointer()));
	}
	if (Resolved.IsValid())
	{
		TestEqual(
			TEXT("compact envelope round-trip preserves semantics"),
			Resolved->GetModel()->GetSemanticFingerprint(),
			Model->GetSemanticFingerprint());
	}

	if (PackedFile != nullptr)
	{
		const FString PackedPath = FPaths::Combine(
			Workspace.Root, PackedFile->GetRelativePath());
		FString PackedText;
		TestTrue(
			TEXT("packed document can be read for envelope conflict"),
			FFileHelper::LoadFileToString(PackedText, *PackedPath));
		TSharedPtr<FJsonObject> PackedObject;
		FString PackedParseError;
		TestTrue(
			TEXT("packed document parses before envelope conflict"),
			FUeAgentStructuredHjsonParser::ParseObject(
				PackedText,
				PackedPath,
				PackedObject,
				PackedParseError));
		TSharedPtr<FJsonObject> FirstRow = FindFirstPackedProperty(PackedObject);
		if (!TestTrue(TEXT("first packed row is uniquely located in categories"), FirstRow.IsValid())) return false;
		FirstRow->SetStringField(TEXT("unit_kind"), TEXT("target_state"));
		TArray<uint8> ConflictingBytes;
		TestTrue(
			TEXT("conflicting packed document serializes through common writer"),
			PackedObject.IsValid()
				&& TrySerializeSemanticHjsonObject(
					PackedObject.ToSharedRef(),
					ConflictingBytes));
		const FUTF8ToTCHAR ConflictingUtf8(
			reinterpret_cast<const ANSICHAR*>(ConflictingBytes.GetData()),
			ConflictingBytes.Num());
		const FString ConflictingText(
			ConflictingUtf8.Length(), ConflictingUtf8.Get());
		TSharedPtr<FJsonObject> ReparsedConflict;
		FString ConflictParseError;
		TestTrue(
			TEXT("common parser accepts the conflicting packed document"),
			FUeAgentStructuredHjsonParser::ParseObject(
				ConflictingText,
				PackedPath,
				ReparsedConflict,
				ConflictParseError));
		const auto ReparsedRow = FindFirstPackedProperty(ReparsedConflict);
		if (!TestTrue(TEXT("serialized tamper actually changes the selected visible row"),
			ConflictingText != PackedText && ReparsedRow.IsValid()
				&& ReparsedRow->GetStringField(TEXT("unit_kind")) == TEXT("target_state"))) return false;
		TestTrue(
			TEXT("conflicting compact document is persisted"),
			FFileHelper::SaveArrayToFile(ConflictingBytes, *PackedPath));

		FUeAgentResourceTracker TamperResources;
		FResolvedSemanticWorkspacePtr Rejected;
		FWorkspaceError TamperError;
		TestFalse(
			TEXT("explicit compact envelope conflict is rejected"),
			TryResolveSemanticWorkspace(
				FWorkspaceSource::Directory(Workspace.Root),
				ESemanticWorkspaceMode::State,
				TamperResources,
				Rejected,
				TamperError));
		TestEqual(
			TEXT("compact envelope rejection is explicit"),
			TamperError.GetCode(),
			FString(TEXT("semantic_workspace_envelope_mismatch")));
		TestFalse(TEXT("rejected compact workspace is not exposed"), Rejected.IsValid());
	}
	return !HasAnyErrors();
}
