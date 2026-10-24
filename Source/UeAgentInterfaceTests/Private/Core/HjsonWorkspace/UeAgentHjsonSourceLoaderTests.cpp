#include "Misc/AutomationTest.h"

#include "Core/HjsonWorkspace/SourceLoader/UeAgentHjsonSourceLoader.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	using namespace UE::UAI::HjsonWorkspace;

	const FString TargetIdentity =
		TEXT("/Game/UAI/Tests/L_SourceIngress.L_SourceIngress");

	bool WriteUtf8(
		const FString& Root,
		const FString& RelativePath,
		const FString& Text)
	{
		const FString Path = FPaths::Combine(Root, RelativePath);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		return FFileHelper::SaveStringToFile(
			Text,
			*Path,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	TSharedRef<FJsonObject> DirectorySource(const FString& Root)
	{
		const TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("kind"), TEXT("directory"));
		Source->SetStringField(TEXT("path"), Root);
		return Source;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSemanticWorkspaceDirectoryIngressTest,
	"UeAgentInterface.Core.HjsonWorkspace.SourceLoader.SemanticDirectoryIngress",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticWorkspaceDirectoryIngressTest::RunTest(
	const FString& Parameters)
{
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("UeAgentInterfaceTests/HjsonSourceLoader/semantic")));
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	TestTrue(
		TEXT("write authored manifest without legacy fields"),
		WriteUtf8(
			Root,
			TEXT(".uai/workspace.hjson"),
			TEXT("{\"workspace_schema\":\"uai.authoring.workspace/2\"}")));

	auto Resources = MakeSemanticWorkspaceResourceTracker();
	FResolvedContentGraphImportSourcePtr Resolved;
	FWorkspaceError Error;
	const bool bLoaded = TryLoadContentGraphImportSource(
		DirectorySource(Root),
		TargetIdentity,
		Resources,
		Resolved,
		Error);
	TestTrue(TEXT("authored manifest is recognized"), bLoaded);
	TestTrue(
		TEXT("authored directory has explicit internal identity"),
		Resolved.IsValid() && Resolved->IsSemanticWorkspaceDirectory());
	TestTrue(
		TEXT("authored manifest is not rejected for missing format_version"),
		Error.GetCode() != TEXT("content_graph_workspace_manifest_invalid"));
	if (Resolved.IsValid())
	{
		TestTrue(
			TEXT("public semantic recognition does not invent machine context"),
			Resolved->GetBaseRevision().IsEmpty()
				&& Resolved->GetSourceFingerprint().IsEmpty()
				&& Resolved->GetSemanticFingerprint().IsEmpty());
	}

	const TSharedRef<FJsonObject> Forged = MakeShared<FJsonObject>();
	Forged->SetStringField(TEXT("kind"), TEXT("semantic_workspace"));
	Forged->SetStringField(TEXT("workspace_root"), Root);
	Forged->SetStringField(TEXT("target_identity"), TargetIdentity);
	auto ForgedResources = MakeSemanticWorkspaceResourceTracker();
	Resolved.Reset();
	Error.Reset();
	TestFalse(
		TEXT("internal semantic source cannot be injected through public union"),
		TryLoadContentGraphImportSource(
			Forged,
			TargetIdentity,
			ForgedResources,
			Resolved,
			Error));

	IFileManager::Get().DeleteDirectory(*Root, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentLegacyContentGraphDirectoryIngressTest,
	"UeAgentInterface.Core.HjsonWorkspace.SourceLoader.LegacyDirectoryCompatibility",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentLegacyContentGraphDirectoryIngressTest::RunTest(
	const FString& Parameters)
{
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("UeAgentInterfaceTests/HjsonSourceLoader/legacy")));
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	const FString Document =
		TEXT("{\"schema\":\"uai.world.edit/1\",\"root\":")
		TEXT("{\"type\":\"/Script/Engine.World\"}}");
	const FString Manifest = FString::Printf(
		TEXT("{\"workspace_schema\":\"uai.content_graph.workspace/1\",")
		TEXT("\"format_version\":1,\"target_identity\":\"%s\",")
		TEXT("\"base_revision\":\"legacy-revision\",")
		TEXT("\"entry_file\":\"entry.hjson\",\"files\":[{")
		TEXT("\"path\":\"entry.hjson\",\"content_hash\":")
		TEXT("\"sha256:b1721af1462e295fca1d3107f89f066fedd120ad198914e6b93b704e1556d204\"}")
		TEXT("],\"references\":[]}"),
		*TargetIdentity);
	TestTrue(
		TEXT("write legacy entry"),
		WriteUtf8(Root, TEXT("entry.hjson"), Document));
	TestTrue(
		TEXT("write legacy manifest"),
		WriteUtf8(Root, TEXT(".uai/workspace.hjson"), Manifest));

	auto Resources = MakeSemanticWorkspaceResourceTracker();
	FResolvedContentGraphImportSourcePtr Resolved;
	FWorkspaceError Error;
	const bool bLoaded = TryLoadContentGraphImportSource(
		DirectorySource(Root),
		TargetIdentity,
		Resources,
		Resolved,
		Error);
	TestTrue(TEXT("legacy manifest still loads"), bLoaded);
	TestTrue(
		TEXT("legacy manifest remains legacy resolved source"),
		Resolved.IsValid() && !Resolved->IsSemanticWorkspaceDirectory());
	TestTrue(
		TEXT("legacy entry remains resident"),
		Resolved.IsValid() && Resolved->GetEntryDocument() != nullptr);

	auto CustodyResources = MakeSemanticWorkspaceResourceTracker();
	FResolvedContentGraphImportSourcePtr RecoveredSource;
	FString RecoveredRevision;
	FString RecoveredAuthorization;
	Error.Reset();
	TestTrue(
		TEXT("legacy directory machine context is recovered internally"),
		TryRecoverContentGraphImportMachineContext(
			DirectorySource(Root),
			TargetIdentity,
			CustodyResources,
			RecoveredSource,
			RecoveredRevision,
			RecoveredAuthorization,
			Error));
	TestEqual(
		TEXT("legacy manifest owns the recovered revision"),
		RecoveredRevision,
		FString(TEXT("legacy-revision")));
	TestTrue(
		TEXT("machine custody returns the same resolved source"),
		RecoveredSource.IsValid()
			&& RecoveredSource->GetBaseRevision() == RecoveredRevision);
	TestFalse(
		TEXT("internal impact authorization is present"),
		RecoveredAuthorization.IsEmpty());

		auto ForeignTargetResources = MakeSemanticWorkspaceResourceTracker();
	RecoveredSource.Reset();
	RecoveredRevision.Reset();
	RecoveredAuthorization.Reset();
	Error.Reset();
	TestFalse(
		TEXT("directory machine context keeps the exact target oracle"),
		TryRecoverContentGraphImportMachineContext(
			DirectorySource(Root),
			TEXT("/Game/UAI/Tests/Other.Other"),
			ForeignTargetResources,
			RecoveredSource,
			RecoveredRevision,
			RecoveredAuthorization,
			Error));

	IFileManager::Get().DeleteDirectory(*Root, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentNormalizedContentGraphSourceTest,
	"UeAgentInterface.Core.HjsonWorkspace.SourceLoader.NormalizedSourceIntegrity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentNormalizedContentGraphSourceTest::RunTest(
	const FString& Parameters)
{
	const TSharedRef<FJsonObject> Inline = MakeShared<FJsonObject>();
	Inline->SetStringField(TEXT("kind"), TEXT("inline"));
	Inline->SetStringField(
		TEXT("document"),
		TEXT("{schema:'uai.test/1',root:{type:'test'}}"));
	auto PublicResources = MakeSemanticWorkspaceResourceTracker();
	FResolvedContentGraphImportSourcePtr PublicSource;
	FWorkspaceError Error;
	TestTrue(
		TEXT("public inline source resolves"),
		TryLoadContentGraphImportSource(
			Inline,
			TargetIdentity,
			PublicResources,
			PublicSource,
			Error));
	if (!PublicSource.IsValid())
	{
		return false;
	}

	auto InlineCustodyResources = MakeSemanticWorkspaceResourceTracker();
	FResolvedContentGraphImportSourcePtr InlineResolvedSource;
	FString InlineRevision;
	FString InlineAuthorization;
	Error.Reset();
	TestTrue(
		TEXT("inline source receives only the internal missing-target expectation"),
		TryRecoverContentGraphImportMachineContext(
			Inline,
			TargetIdentity,
			InlineCustodyResources,
			InlineResolvedSource,
			InlineRevision,
			InlineAuthorization,
			Error));
	TestEqual(
		TEXT("inline create expectation is semantic and short"),
		InlineRevision,
		FString(TEXT("missing")));
	TestTrue(
		TEXT("inline custody returns the resolved source"),
		InlineResolvedSource.IsValid()
			&& InlineResolvedSource->GetKind() == EWorkspaceSourceKind::Inline);
	TestFalse(
		TEXT("inline impact authorization remains internal and present"),
		InlineAuthorization.IsEmpty());

	const TSharedRef<FJsonObject> Normalized =
		PublicSource->MakeNormalizedSourceObject();
	auto NormalizedResources = MakeSemanticWorkspaceResourceTracker();
	FResolvedContentGraphImportSourcePtr RoundTrip;
	Error.Reset();
	TestTrue(
		TEXT("normalized inline source round-trips through internal ingress"),
		TryLoadNormalizedContentGraphImportSource(
			Normalized,
			TargetIdentity,
			TEXT("expected-revision"),
			NormalizedResources,
			RoundTrip,
			Error));
	TestEqual(
		TEXT("round-trip retains entry document text"),
		RoundTrip.IsValid() && RoundTrip->GetEntryDocument()
			? RoundTrip->GetEntryDocument()->GetDocument()
			: FString(),
		PublicSource->GetEntryDocument()->GetDocument());

	auto PublicRejectResources = MakeSemanticWorkspaceResourceTracker();
	FResolvedContentGraphImportSourcePtr RejectedPublic;
	Error.Reset();
	TestFalse(
		TEXT("normalized source remains outside public source union"),
		TryLoadContentGraphImportSource(
			Normalized,
			TargetIdentity,
			PublicRejectResources,
			RejectedPublic,
			Error));

	const TSharedRef<FJsonObject> Tampered =
		PublicSource->MakeNormalizedSourceObject();
	const TArray<TSharedPtr<FJsonValue>>& TamperedDocuments =
		Tampered->GetArrayField(TEXT("documents"));
	TamperedDocuments[0]->AsObject()->SetStringField(
		TEXT("document"),
		TEXT("{schema:'uai.test/1',root:{type:'tampered'}}"));
	auto TamperedResources = MakeSemanticWorkspaceResourceTracker();
	RoundTrip.Reset();
	Error.Reset();
	TestFalse(
		TEXT("document text tampering is rejected"),
		TryLoadNormalizedContentGraphImportSource(
			Tampered,
			TargetIdentity,
			TEXT("expected-revision"),
			TamperedResources,
			RoundTrip,
			Error));
	TestEqual(
		TEXT("tampering reports document digest mismatch"),
		Error.GetCode(),
		FString(TEXT("content_graph_normalized_document_digest_mismatch")));

	const TSharedRef<FJsonObject> TamperedHash =
		PublicSource->MakeNormalizedSourceObject();
	const TArray<TSharedPtr<FJsonValue>>& HashDocuments =
		TamperedHash->GetArrayField(TEXT("documents"));
	HashDocuments[0]->AsObject()->SetStringField(
		TEXT("content_hash"),
		TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"));
	auto HashResources = MakeSemanticWorkspaceResourceTracker();
	RoundTrip.Reset();
	Error.Reset();
	TestFalse(
		TEXT("content hash tampering is rejected"),
		TryLoadNormalizedContentGraphImportSource(
			TamperedHash,
			TargetIdentity,
			TEXT("expected-revision"),
			HashResources,
			RoundTrip,
			Error));
	TestEqual(
		TEXT("hash tampering reports document digest mismatch"),
		Error.GetCode(),
		FString(TEXT("content_graph_normalized_document_digest_mismatch")));

	const TSharedRef<FJsonObject> TamperedSourceFingerprint =
		PublicSource->MakeNormalizedSourceObject();
	TamperedSourceFingerprint->SetStringField(
		TEXT("source_fingerprint"),
		TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"));
	auto SourceFingerprintResources = MakeSemanticWorkspaceResourceTracker();
	RoundTrip.Reset();
	Error.Reset();
	TestFalse(
		TEXT("source fingerprint tampering is rejected"),
		TryLoadNormalizedContentGraphImportSource(
			TamperedSourceFingerprint,
			TargetIdentity,
			TEXT("expected-revision"),
			SourceFingerprintResources,
			RoundTrip,
			Error));
	TestEqual(
		TEXT("source fingerprint mismatch has stable error"),
		Error.GetCode(),
		FString(TEXT("content_graph_normalized_source_fingerprint_mismatch")));

	const TSharedRef<FJsonObject> TamperedSemanticFingerprint =
		PublicSource->MakeNormalizedSourceObject();
	TamperedSemanticFingerprint->SetStringField(
		TEXT("semantic_fingerprint"),
		TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"));
	auto SemanticFingerprintResources = MakeSemanticWorkspaceResourceTracker();
	RoundTrip.Reset();
	Error.Reset();
	TestFalse(
		TEXT("semantic fingerprint tampering is rejected"),
		TryLoadNormalizedContentGraphImportSource(
			TamperedSemanticFingerprint,
			TargetIdentity,
			TEXT("expected-revision"),
			SemanticFingerprintResources,
			RoundTrip,
			Error));
	TestEqual(
		TEXT("semantic fingerprint mismatch has stable error"),
		Error.GetCode(),
		FString(TEXT("content_graph_normalized_semantic_fingerprint_mismatch")));

	const TSharedRef<FJsonObject> DirectoryMismatch =
		PublicSource->MakeNormalizedSourceObject();
	DirectoryMismatch->SetStringField(TEXT("carrier"), TEXT("directory"));
	DirectoryMismatch->SetStringField(TEXT("base_revision"), TEXT("old-revision"));
	auto RevisionResources = MakeSemanticWorkspaceResourceTracker();
	RoundTrip.Reset();
	Error.Reset();
	TestFalse(
		TEXT("directory base revision mismatch is rejected"),
		TryLoadNormalizedContentGraphImportSource(
			DirectoryMismatch,
			TargetIdentity,
			TEXT("expected-revision"),
			RevisionResources,
			RoundTrip,
			Error));
	TestEqual(
		TEXT("revision mismatch has stable error"),
		Error.GetCode(),
		FString(TEXT("content_graph_normalized_base_revision_mismatch")));

	const TSharedRef<FJsonObject> MissingEntry =
		PublicSource->MakeNormalizedSourceObject();
	MissingEntry->SetStringField(TEXT("entry_file"), TEXT("missing.hjson"));
	auto MissingEntryResources = MakeSemanticWorkspaceResourceTracker();
	RoundTrip.Reset();
	Error.Reset();
	TestFalse(
		TEXT("missing entry document is rejected"),
		TryLoadNormalizedContentGraphImportSource(
			MissingEntry,
			TargetIdentity,
			TEXT("expected-revision"),
			MissingEntryResources,
			RoundTrip,
			Error));
	TestEqual(
		TEXT("missing entry has stable error"),
		Error.GetCode(),
		FString(TEXT("content_graph_normalized_entry_document_missing")));
	return true;
}
