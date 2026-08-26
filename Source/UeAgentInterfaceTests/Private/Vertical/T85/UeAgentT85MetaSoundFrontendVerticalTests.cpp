#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "DomainAdapters/AudioGraph/UeAgentAudioGraphDomainMechanics.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundFactory.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundSource.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "QueryOwners/AudioGraph/UeAgentAudioGraphCandidateQuery.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	using namespace UE::UAI;

	constexpr const TCHAR* MetaSoundSourceType =
		TEXT("/Script/MetasoundEngine.MetaSoundSource");
	constexpr const TCHAR* Fingerprint =
		TEXT("sha256:7777777777777777777777777777777777777777777777777777777777777777");
	constexpr double ExpectedX = 384.0;
	constexpr double ExpectedY = 192.0;

	void DeleteAssetIfPresent(const FString& ObjectPath)
	{
		if (UObject* Existing = StaticLoadObject(
				UObject::StaticClass(), nullptr, *ObjectPath))
		{
			ObjectTools::DeleteObjectsUnchecked({Existing});
		}
	}

	bool SaveSource(
		UPackage& Package,
		UMetaSoundSource& Source,
		const FString& Filename)
	{
		Package.MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		const bool bSaved = UPackage::SavePackage(
			&Package, &Source, *Filename, Args);
		if (bSaved)
		{
			Package.SetDirtyFlag(false);
		}
		return bSaved;
	}

	bool TryBuildResolvedMetaSoundImportParams(
		const FString& Target,
		const FString& ExpectedRevision,
		const FString& CandidateId,
		FString& OutParams,
		FString& OutError)
	{
		const TSharedRef<FJsonObject> Candidate = MakeShared<FJsonObject>();
		Candidate->SetStringField(TEXT("id"), CandidateId);
		const TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
		Position->SetNumberField(TEXT("x"), ExpectedX);
		Position->SetNumberField(TEXT("y"), ExpectedY);
		const TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetStringField(TEXT("intent"), TEXT("create"));
		Node->SetObjectField(TEXT("candidate"), Candidate);
		Node->SetObjectField(TEXT("position"), Position);

		const TSharedRef<FJsonObject> NativeParams = MakeShared<FJsonObject>();
		NativeParams->SetArrayField(
			TEXT("nodes"), {MakeShared<FJsonValueObject>(Node)});
		const TSharedRef<FJsonObject> Operation = MakeShared<FJsonObject>();
		Operation->SetStringField(
			TEXT("command_id"), TEXT("metasound.graph.node.mutate"));
		Operation->SetObjectField(TEXT("params"), NativeParams);
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("type"), MetaSoundSourceType);
		Root->SetObjectField(TEXT("operation"), Operation);
		const TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
		Document->SetStringField(TEXT("schema"), TEXT("uai.audio_graph.edit/1"));
		Document->SetObjectField(TEXT("root"), Root);
		FString DocumentText;
		if (!CommandContract::Json::TryCanonicalize(
				Document, DocumentText, OutError))
		{
			return false;
		}

		const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("path"), TEXT("index.hjson"));
		Entry->SetStringField(TEXT("document"), DocumentText);
		Entry->SetStringField(TEXT("content_hash"), Fingerprint);
		const TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("kind"), TEXT("resolved"));
		Source->SetStringField(TEXT("carrier"), TEXT("inline"));
		Source->SetStringField(
			TEXT("source_unit"), TEXT("t85-metasound-frontend-vertical"));
		Source->SetStringField(TEXT("target_identity"), Target);
		Source->SetStringField(TEXT("base_revision"), TEXT(""));
		Source->SetStringField(TEXT("entry_file"), TEXT("index.hjson"));
		Source->SetStringField(TEXT("source_fingerprint"), Fingerprint);
		Source->SetStringField(TEXT("semantic_fingerprint"), Fingerprint);
		Source->SetArrayField(
			TEXT("documents"), {MakeShared<FJsonValueObject>(Entry)});

		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), Target);
		Params->SetStringField(TEXT("expected_revision"), ExpectedRevision);
		Params->SetObjectField(TEXT("source"), Source);
		return CommandContract::Json::TryCanonicalize(
			Params, OutParams, OutError);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT85MetaSoundFrontendVerticalTest,
	"UAI.Root.T85.MetaSound.FrontendVertical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85MetaSoundFrontendVerticalTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI;
	using namespace UE::UAI::QueryOwners::AudioGraph;

	const FString PackageName =
		TEXT("/Game/UAI/Transient/T85MetaSoundFrontend/MS_Frontend_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	DeleteAssetIfPresent(ObjectPath);
	ON_SCOPE_EXIT
	{
		DeleteAssetIfPresent(ObjectPath);
	};

	UPackage* const Package = CreatePackage(*PackageName);
	UMetaSoundSourceFactory* const Factory =
		NewObject<UMetaSoundSourceFactory>();
	UMetaSoundSource* const Source = Package && Factory
		? Cast<UMetaSoundSource>(Factory->FactoryCreateNew(
			UMetaSoundSource::StaticClass(),
			Package,
			*AssetName,
			RF_Public | RF_Standalone | RF_Transactional,
			nullptr,
			GWarn))
		: nullptr;
	if (!TestNotNull(TEXT("a persistent MetaSound Source fixture is created"),
			Source))
	{
		return false;
	}
	FAssetRegistryModule::AssetCreated(Source);
	if (!TestTrue(TEXT("the initial MetaSound Source fixture is saved"),
			SaveSource(*Package, *Source, Filename)))
	{
		return false;
	}

	FAudioGraphCandidateQueryResult CandidateResult;
	FAudioGraphCandidateQueryError CandidateError;
	if (!TestTrue(TEXT("the frontend registry searches UE.Sine.Audio"),
			TryQueryAudioGraphCandidates(
				*Source,
				TEXT("UE.Sine.Audio"),
				CandidateResult,
				CandidateError,
				64,
				0)))
	{
		AddError(CandidateError.Code + TEXT(":") + CandidateError.Detail);
		return false;
	}
	const FAudioGraphCandidate* const Selected =
		CandidateResult.Candidates.FindByPredicate(
			[](const FAudioGraphCandidate& Candidate)
			{
				return Candidate.Name == TEXT("UE.Sine.Audio")
					&& Candidate.bSupported;
			});
	if (!TestNotNull(TEXT("one supported UE.Sine.Audio candidate is available"),
			Selected))
	{
		return false;
	}
	TestTrue(TEXT("the selected candidate exposes a compact id"),
		Selected->Id.StartsWith(TEXT("candidate:")));
	TestFalse(TEXT("the partial frontend registry is not reported as a native menu"),
		CandidateResult.bCoverageComplete);
	TestFalse(TEXT("candidate search leaves the saved package clean"),
		Package->IsDirty());

	FString CanonicalParams;
	FString ParamsError;
	if (!TestTrue(TEXT("id-only MetaSound HJSON canonicalizes"),
			TryBuildResolvedMetaSoundImportParams(
				PackageName,
				CandidateResult.TargetRevision,
				Selected->Id,
				CanonicalParams,
				ParamsError)))
	{
		AddError(ParamsError);
		return false;
	}
	TestFalse(TEXT("the model input contains no class binding"),
		CanonicalParams.Contains(TEXT("class_name")));
	TestFalse(TEXT("the model input contains no generated node guid"),
		CanonicalParams.Contains(TEXT("node_guid")));

	FString ImportPayloadJson;
	bool bReadOnly = true;
	HjsonWorkspace::FWorkspaceError ImportError;
	if (!TestTrue(TEXT("id-only HJSON creates and persists the frontend node"),
			DomainAdapters::AudioGraph::TryExecuteContentGraph(
				TEXT("content_graph.import_workspace"),
				PackageName,
				CanonicalParams,
				ImportPayloadJson,
				bReadOnly,
				ImportError)))
	{
		AddError(ImportError.GetCode() + TEXT(":")
			+ ImportError.GetSourceUnit() + TEXT(":")
			+ ImportError.GetJsonPointer());
		return false;
	}
	TestFalse(TEXT("the MetaSound import is a mutation"), bReadOnly);
	TSharedPtr<FJsonObject> ImportPayload;
	FString PayloadError;
	if (!TestTrue(TEXT("the MetaSound import payload is canonical JSON"),
			CommandContract::Json::TryParseObject(
				ImportPayloadJson, ImportPayload, PayloadError)))
	{
		AddError(PayloadError);
		return false;
	}
	FString TerminalStatus;
	bool bSaved = false;
	bool bReloaded = false;
	TestTrue(TEXT("the import reports the persisted reload lifecycle"),
		ImportPayload.IsValid()
			&& ImportPayload->TryGetStringField(
				TEXT("terminal_status"), TerminalStatus)
			&& TerminalStatus == TEXT("succeeded")
			&& ImportPayload->TryGetBoolField(TEXT("saved"), bSaved)
			&& bSaved
			&& ImportPayload->TryGetBoolField(TEXT("reloaded"), bReloaded)
			&& bReloaded);

	UMetaSoundSource* const Reloaded =
		LoadObject<UMetaSoundSource>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("the persisted MetaSound Source reloads"), Reloaded))
	{
		return false;
	}
	const IMetaSoundDocumentInterface* const DocumentInterface =
		Cast<IMetaSoundDocumentInterface>(Reloaded);
	if (!TestNotNull(TEXT("the reloaded Source owns a frontend document"),
			DocumentInterface))
	{
		return false;
	}
	const FMetasoundFrontendDocument& Document =
		DocumentInterface->GetConstDocument();
	const FMetasoundFrontendGraph& Graph =
		Document.RootGraph.GetConstDefaultGraph();
	TArray<const FMetasoundFrontendNode*> MatchingNodes;
	for (const FMetasoundFrontendNode& Node : Graph.Nodes)
	{
		const FMetasoundFrontendClass* const Class =
			Document.Dependencies.FindByPredicate(
				[&Node](const FMetasoundFrontendClass& Dependency)
				{
					return Dependency.ID == Node.ClassID;
				});
		if (Class
			&& Class->Metadata.GetClassName().ToString() == Selected->Name
			&& Class->Metadata.GetVersion().Major == Selected->MajorVersion
			&& Class->Metadata.GetVersion().Minor == Selected->MinorVersion)
		{
			MatchingNodes.Add(&Node);
		}
	}
	TestEqual(TEXT("one exact frontend class survives reload"),
		MatchingNodes.Num(), 1);
	if (MatchingNodes.Num() == 1)
	{
		const FMetasoundFrontendNode& Node = *MatchingNodes[0];
		TestTrue(TEXT("the server generated a persistent node guid"),
			Node.GetID().IsValid());
#if WITH_EDITORONLY_DATA
		bool bFoundPosition = false;
		for (const TPair<FGuid, FVector2D>& Location
			: Node.Style.Display.Locations)
		{
			bFoundPosition |= Location.Value.Equals(
				FVector2D(ExpectedX, ExpectedY), KINDA_SMALL_NUMBER);
		}
		TestTrue(TEXT("optional placement survives in frontend metadata"),
			bFoundPosition);
#endif
	}

	DeleteAssetIfPresent(ObjectPath);
	TestFalse(TEXT("the task-owned MetaSound fixture leaves no asset file"),
		IFileManager::Get().FileExists(*Filename));
	return !HasAnyErrors();
}
