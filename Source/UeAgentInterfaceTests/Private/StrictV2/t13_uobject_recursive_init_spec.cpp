#include "Misc/AutomationTest.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "DomainAdapters/UObjectDataAsset/UeAgentRecursiveDataAsset.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "Module/CommandRegistration/UObjectDataAsset/UeAgentUObjectDataAssetRegistration.h"
#include "ObjectTools.h"
#include "OperationOwners/UObjectDataAsset/UeAgentUObjectDataAssetCommandService.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandRegistry;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::DomainAdapters::UObjectDataAsset;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Module::CommandRegistration::UObjectDataAsset;
	using namespace UE::UAI::OperationOwners::UObjectDataAsset;

	const TArray<FString> ExpectedCommandIds = {
		TEXT("content_graph.export_workspace"),
		TEXT("content_graph.import_workspace"),
	};

	class FNeverCancelled final : public IUeAgentCancellationSignal
	{
	public:
		virtual bool IsCancelled() const override { return false; }
	};

	class FFixtureService final : public IUeAgentUObjectDataAssetCommandService
	{
	public:
		virtual bool Execute(
			const FUeAgentUObjectDataAssetCommandRequest& Request,
			FUeAgentUObjectDataAssetCommandResponse& OutResponse,
			FUeAgentUObjectDataAssetCommandError& OutError) override
		{
			++Calls;
			LastRequest = Request;
			OutResponse = {};
			OutError = {};
			if (bFail)
			{
				OutError.Code = TEXT("fixture_t13_failed");
				OutError.SubjectId = Request.CommandId;
				OutError.InternalContext = TEXT("fixture failure remains typed");
				return false;
			}
			OutResponse.PayloadJson = FString::Printf(
				TEXT("{\"command_id\":\"%s\",\"read_only\":%s}"),
				*Request.CommandId,
				Request.CommandId == TEXT("content_graph.import_workspace")
					? TEXT("false")
					: TEXT("true"));
			OutResponse.bReadOnly =
				Request.CommandId != TEXT("content_graph.import_workspace");
			return true;
		}

		int32 Calls = 0;
		FUeAgentUObjectDataAssetCommandRequest LastRequest;
		bool bFail = false;
	};

	struct FFixtureRegistration
	{
		TSharedRef<FFixtureService> Service = MakeShared<FFixtureService>();
		TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
		TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
		FString Error;
	};

	FFixtureRegistration MakeRegistration(FAutomationTestBase* Test)
	{
		FFixtureRegistration Fixture;
		Test->TestTrue(
			TEXT("T13 contribution composes in isolation"),
			TryCreateUObjectDataAssetRegistration(
				Fixture.Service,
				Fixture.Registry,
				Fixture.Dispatch,
				Fixture.Error));
		Test->TestTrue(TEXT("T13 immutable registry exists"), Fixture.Registry.IsValid());
		Test->TestTrue(TEXT("T13 exact dispatch exists"), Fixture.Dispatch.IsValid());
		return Fixture;
	}

	bool DecodeRequest(
		const FString& CommandId,
		const FString& ContractHash,
		const FString& ParamsJson,
		TOptional<FUeAgentCommandRequest>& OutRequest)
	{
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"corr-t13\",")
			TEXT("\"request_id\":\"req-t13\",")
			TEXT("\"command_id\":\"%s\",")
			TEXT("\"command_binding_hash\":\"%s\",\"params\":%s}"),
			*CommandId,
			*ContractHash,
			*ParamsJson);
		FTCHARToUTF8 Encoded(*Envelope);
		TArray<uint8> Bytes;
		Bytes.Append(
			reinterpret_cast<const uint8*>(Encoded.Get()),
			Encoded.Length());
		FUeAgentCommandProtocolDiagnostic Diagnostic;
		return FUeAgentCommandProtocolCodec().DecodeRequest(
			Bytes,
			OutRequest,
			Diagnostic);
	}

	FUeAgentCommandResult Submit(
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		const FString& CommandId,
		const FString& ParamsJson,
		TArray<FString> Authorities = {
			TEXT("uai:query"),
			TEXT("uai:export"),
			TEXT("uai:operate"),
		})
	{
		const auto* Contract = Registry.IsValid()
			? Registry->FindExact(CommandId)
			: nullptr;
		if (!Contract || !Dispatch.IsValid())
		{
			return FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t13"), TEXT("req-t13"), CommandId,
				TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
				TEXT("fixture_contract_missing"),
				TEXT("T13 fixture could not find the exact contract"));
		}
		TOptional<FUeAgentCommandRequest> Request;
		if (!DecodeRequest(
				CommandId,
				Contract->GetContractHash(),
				ParamsJson,
				Request)
			|| !Request.IsSet())
		{
			return FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t13"), TEXT("req-t13"), CommandId,
				Contract->GetContractHash(),
				TEXT("fixture_request_decode_failed"),
				TEXT("T13 fixture could not decode its request"));
		}
		return Dispatch->Submit(
			Request.GetValue(),
			FUeAgentCallerContext(
				TEXT("strict-v2-t13"),
				MoveTemp(Authorities),
				TEXT("corr-t13")),
			MakeShared<FNeverCancelled>());
	}

	FString RecursiveDocument(const bool bOwnershipCycle = false)
	{
		const FString Ownership = bOwnershipCycle
			? TEXT(R"json(
				"ownership": [
					{"owner":"left","child":"right"},
					{"owner":"right","child":"left"}
				],)json")
			: FString();
		return FString::Printf(TEXT(R"json({
			"schema":"uai.uobject_dataasset.recursive/1",
			"root":{
				"id":"root",
				"type":"/Script/UeAgentInterface.UeAgentRecursiveDataAsset",
				"values":{
					"label":"explicit-root",
					"explicit_number":42,
					"numbers":[3,5,8],
					"named_numbers":{"alpha":13,"beta":21}
				},
				"owned":[
					{"id":"shared","name":"Shared","type":"/Script/UeAgentInterface.UeAgentRecursiveOwnedObject","values":{"label":"shared"},"owned":[{"id":"deep","name":"Deep","type":"/Script/UeAgentInterface.UeAgentRecursiveOwnedObject","values":{"label":"deep"}}]},
					{"id":"left","name":"Left","type":"/Script/UeAgentInterface.UeAgentRecursiveOwnedObject","values":{"label":"left"}},
					{"id":"right","name":"Right","type":"/Script/UeAgentInterface.UeAgentRecursiveOwnedObject","values":{"label":"right"}}
				],
				%s
				"references":[
					{"from":"left","field":"peer","to":"right"},
					{"from":"right","field":"peer","to":"left"},
					{"from":"left","field":"shared","to":"shared"},
					{"from":"right","field":"shared","to":"shared"}
				]
			}
		})json"), *Ownership);
	}

	FString AlternateDocument(const bool bOmitValues)
	{
		const FString RootValues = bOmitValues ? FString() :
			TEXT("\"values\":{\"label\":\"violet-root\",\"explicit_number\":73,\"numbers\":[2,7,11],\"named_numbers\":{\"gamma\":34}},");
		const FString BranchValues = bOmitValues ? FString() :
			TEXT("\"values\":{\"label\":\"violet-branch\"},");
		return FString::Printf(TEXT(R"json({
			"schema":"uai.uobject_dataasset.recursive/1",
			"root":{"id":"origin","type":"/Script/UeAgentInterface.UeAgentRecursiveDataAsset",%s
				"owned":[{"id":"branch","name":"BranchNode","type":"/Script/UeAgentInterface.UeAgentRecursiveOwnedObject",%s
					"owned":[{"id":"leaf-node","name":"LeafNode","type":"/Script/UeAgentInterface.UeAgentRecursiveOwnedObject"}]}],
				"references":[
					{"from":"branch","field":"peer","to":"leaf-node"},
					{"from":"leaf-node","field":"peer","to":"branch"},
					{"from":"branch","field":"shared","to":"leaf-node"}]}
		})json"), *RootValues, *BranchValues);
	}

	FString Sha256Text(const FString& Text)
	{
		FTCHARToUTF8 Utf8(*Text);
		uint8 Digest[SHA256_DIGEST_LENGTH] = {};
		SHA256(
			reinterpret_cast<const uint8*>(Utf8.Get()),
			Utf8.Length(),
			Digest);
		return TEXT("sha256:")
			+ BytesToHex(Digest, SHA256_DIGEST_LENGTH).ToLower();
	}

	FString ImportParamsForSource(
		const FString& Target,
		const TSharedRef<FJsonObject>& Source)
	{
		const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), Target);
		Params->SetObjectField(TEXT("source"), Source);
		FString Result;
		FString Error;
		return UE::UAI::CommandContract::Json::TryCanonicalize(Params, Result, Error)
			? Result
			: FString(TEXT("{}"));
	}

	FString ImportParams(
		const FString& Target,
		const FString& Document)
	{
		const TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("kind"), TEXT("inline"));
		Source->SetStringField(TEXT("document"), Document);
		return ImportParamsForSource(Target, Source);
	}

	FString FileImportParams(
		const FString& Target,
		const FString& Kind,
		const FString& Path)
	{
		const TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("kind"), Kind);
		Source->SetStringField(TEXT("path"), Path);
		return ImportParamsForSource(Target, Source);
	}

	bool WriteText(const FString& Path, const FString& Text)
	{
		return IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true)
			&& FFileHelper::SaveStringToFile(
				Text,
				*Path,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	bool WriteDirectoryManifest(
		const FString& Root,
		const FString& Target,
		const FString& EntryFile,
		const FString& ContentHash)
	{
		const TSharedRef<FJsonObject> Manifest = MakeShared<FJsonObject>();
		Manifest->SetStringField(
			TEXT("workspace_schema"),
			TEXT("uai.content_graph.workspace/1"));
		Manifest->SetNumberField(TEXT("format_version"), 1);
		Manifest->SetStringField(TEXT("target_identity"), Target);
		Manifest->SetStringField(TEXT("base_revision"), TEXT("absent"));
		Manifest->SetStringField(TEXT("entry_file"), EntryFile);
		const TSharedRef<FJsonObject> File = MakeShared<FJsonObject>();
		File->SetStringField(TEXT("path"), EntryFile);
		File->SetStringField(TEXT("content_hash"), ContentHash);
		Manifest->SetArrayField(
			TEXT("files"),
			{MakeShared<FJsonValueObject>(File)});
		Manifest->SetArrayField(
			TEXT("references"),
			TArray<TSharedPtr<FJsonValue>>{});
		FString Text;
		FString Error;
		return UE::UAI::CommandContract::Json::TryCanonicalize(Manifest, Text, Error)
			&& WriteText(
				FPaths::Combine(Root, TEXT(".uai/workspace.hjson")),
				Text);
	}

	bool WriteDirectoryWorkspace(
		const FString& Root,
		const FString& Target,
		const FString& Document)
	{
		return WriteText(FPaths::Combine(Root, TEXT("asset.hjson")), Document)
			&& WriteDirectoryManifest(
				Root,
				Target,
				TEXT("asset.hjson"),
				Sha256Text(Document));
	}

	void DeleteAssetIfPresent(const FString& ObjectPath)
	{
		if (UObject* Existing = StaticLoadObject(
			UObject::StaticClass(), nullptr, *ObjectPath))
		{
			ObjectTools::DeleteObjectsUnchecked({Existing});
		}
	}

	TSharedPtr<FJsonObject> Payload(
		FAutomationTestBase* Test,
		const FUeAgentCommandResult& Result)
	{
		TSharedPtr<FJsonObject> Object;
		FString Error;
		Test->TestTrue(
			TEXT("public response is canonical JSON"),
			UE::UAI::CommandContract::Json::TryParseObject(Result.GetPayloadJson(), Object, Error));
		return Object;
	}
}

namespace
{
void VisitRecursiveWorkspaceRows(const TSharedPtr<FJsonObject>& Object,
	TFunctionRef<void(const FString&, const TSharedPtr<FJsonObject>&)> Visit)
{
	if (!Object) return;
	for (const auto& Field : Object->Values)
	{
		if (Field.Value->Type == EJson::Object)
		{
			auto Child = Field.Value->AsObject();
			if (Child->HasField(TEXT("type")) && Child->HasField(TEXT("value"))) Visit(Field.Key, Child);
			VisitRecursiveWorkspaceRows(Child, Visit);
		}
		else if (Field.Value->Type == EJson::Array)
			for (const auto& Value : Field.Value->AsArray())
				if (Value->Type == EJson::Object) VisitRecursiveWorkspaceRows(Value->AsObject(), Visit);
	}
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentStrictV2T13RecursiveWorkspaceTest,
	"UAI.StrictV2.T13.UOBJECT.RECURSIVE.INIT.EditableWorkspace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentStrictV2T13RecursiveWorkspaceTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI;
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Package = TEXT("/Game/UAI/Transient/T13Workspace/DA_") + Token;
	const FString Target = Package + TEXT(".") + FPackageName::GetLongPackageAssetName(Package);
	const FString Directory = FPaths::ProjectSavedDir() / TEXT("Automation/UAI/T13Workspace") / Token;
	const FString Filename = FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension());
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
	auto Command = [&](const FString& Id, const TSharedRef<FJsonObject>& Params)
	{
		return Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, Id, Params,
			TEXT("t13-recursive-workspace"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
	};
	ON_SCOPE_EXIT
	{
		if (auto* Existing = LoadObject<UUeAgentRecursiveDataAsset>(nullptr, *Target))
		{
			FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
			UPackage::SavePackage(Existing->GetPackage(), Existing, *Filename, Args);
		}
		DeleteAssetIfPresent(Target);
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	};
	auto Imported = Submit(Registry, Dispatch, TEXT("content_graph.import_workspace"), ImportParams(Target, AlternateDocument(false)));
	if (!TestTrue(TEXT("recursive workspace fixture uses public absent initialization"), Imported.IsOk())) { AddError(Imported.GetErrorJson()); return false; }
	TestFalse(TEXT("Peer is a non-owning native reference"), FindFProperty<FObjectPropertyBase>(UUeAgentRecursiveOwnedObject::StaticClass(), TEXT("Peer"))->HasAnyPropertyFlags(CPF_InstancedReference | CPF_ExportObject));
	TestFalse(TEXT("Shared is a non-owning native reference"), FindFProperty<FObjectPropertyBase>(UUeAgentRecursiveOwnedObject::StaticClass(), TEXT("Shared"))->HasAnyPropertyFlags(CPF_InstancedReference | CPF_ExportObject));
	auto Export = [&](const FString& Stage)
	{
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), Target);
		Args->SetStringField(TEXT("workspace_path"), Directory / Stage); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		auto Result = Command(TEXT("content_graph.export_workspace"), Args);
		if (!Result.IsOk()) AddError(Result.GetErrorJson());
		return Result.IsOk();
	};
	auto Edit = [&](const FString& Stage, TFunctionRef<void(const FString&, const TSharedPtr<FJsonObject>&)> Mutate)
	{
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *(Directory / Stage), TEXT("*.hjson"), true, false);
		for (const auto& File : Files)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Root;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Root, Error)) { AddError(Error); return false; }
			VisitRecursiveWorkspaceRows(Root, Mutate);
			if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Root.ToSharedRef(), Text, Error) || !WriteText(File, Text)) { AddError(Error); return false; }
		}
		return true;
	};
	auto Import = [&](const FString& Stage)
	{
		auto Result = Submit(Registry, Dispatch, TEXT("content_graph.import_workspace"), FileImportParams(Target, TEXT("directory"), Directory / Stage));
		return Result;
	};
	auto CheckGraph = [&](UUeAgentRecursiveDataAsset* Asset, const FString& Prefix)
	{
		if (!Asset || Asset->OwnedObjects.Num() < 1) return false;
		auto* Branch = Asset->OwnedObjects.FindByPredicate([](const auto& Item) { return Item && Item->NodeId == TEXT("branch"); });
		if (!Branch || (*Branch)->Children.Num() < 1) return false;
		auto* LeafSlot = (*Branch)->Children.FindByPredicate([](const auto& Item) { return Item && Item->NodeId == TEXT("leaf-node"); });
		auto* Leaf = LeafSlot ? LeafSlot->Get() : nullptr;
		if (!Leaf || Leaf->NodeId != TEXT("leaf-node") || (*Branch)->Peer != Leaf || (*Branch)->Shared != Leaf || Leaf->Peer != *Branch) return false;
		if (Asset->Label != Prefix + TEXT("violet-root") || (*Branch)->Label != Prefix + TEXT("violet-branch") || Leaf->Label != Prefix + TEXT("native-child")) return false;
		TArray<UObject*> Objects; GetObjectsWithOuter(Asset, Objects, true); Objects.Add(Asset);
		int32 OwnedCount = 0, DefaultCount = 0;
		for (UObject* Object : Objects)
		{
			UUeAgentRecursiveLeafObject* Default = nullptr;
			if (Object == Asset) Default = Asset->NativeDefaultSubobject;
			else if (auto* Owned = Cast<UUeAgentRecursiveOwnedObject>(Object)) { ++OwnedCount; Default = Owned->NativeDefaultSubobject; if (Owned->NativeCounter != 17) return false; }
			else { if (Cast<UUeAgentRecursiveLeafObject>(Object)) ++DefaultCount; else return false; continue; }
			if (!Default || Default->GetFName() != TEXT("NativeDefaultSubobject") || Default->GetOuter() != Object || Default->GetClass() != UUeAgentRecursiveLeafObject::StaticClass() || Default->NativeDefaultValue != 29) return false;
		}
		const TMap<FString, int32> ExpectedNamedNumbers = {{TEXT("gamma"), 34}};
		return DefaultCount == OwnedCount + 1 && (*Branch)->GetFName() == TEXT("BranchNode") && Leaf->GetFName() == TEXT("LeafNode")
			&& (*Branch)->GetOuter() == Asset && Leaf->GetOuter() == *Branch && Asset->ExplicitNumber == 73 && Asset->Numbers == TArray<int32>{2,7,11}
			&& Asset->NamedNumbers.OrderIndependentCompareEqual(ExpectedNamedNumbers);
	};
	if (!Export(TEXT("values"))) return false;
	int32 LabelRows = 0;
	if (!Edit(TEXT("values"), [&](const FString& Name, const TSharedPtr<FJsonObject>& Row) {
		if (Name == TEXT("Label")) { ++LabelRows; Row->SetStringField(TEXT("value"), TEXT("edited-") + Row->GetStringField(TEXT("value"))); }
	})) return false;
	if (!TestEqual(TEXT("ordinary root and both descendant label rows exported"), LabelRows, 3)) return false;
	Imported = Import(TEXT("values"));
	if (!TestTrue(TEXT("ordinary root and descendant edits import"), Imported.IsOk())) { AddError(Imported.GetErrorJson()); return false; }
	if (!TestTrue(TEXT("root/descendant values, forward/shared cycle and native defaults persist"), CheckGraph(LoadObject<UUeAgentRecursiveDataAsset>(nullptr, *Target), TEXT("edited-")))) return false;
	if (!Export(TEXT("values-readback"))) return false;
	int32 ReadbackLabels = 0;
	if (!Edit(TEXT("values-readback"), [&](const FString& Name, const TSharedPtr<FJsonObject>& Row) {
		if (Name == TEXT("Label") && Row->GetStringField(TEXT("value")).StartsWith(TEXT("edited-"))) ++ReadbackLabels;
	})) return false;
	TestEqual(TEXT("independent re-export contains all edited labels"), ReadbackLabels, 3);
	auto CandidateArgs = MakeShared<FJsonObject>(); CandidateArgs->SetStringField(TEXT("target"), Target);
	auto CandidateInput = MakeShared<FJsonObject>(); CandidateInput->SetStringField(TEXT("property_path"), TEXT("OwnedObjects"));
	CandidateInput->SetStringField(TEXT("query"), UUeAgentRecursiveOwnedObject::StaticClass()->GetDisplayNameText().ToString());
	CandidateArgs->SetObjectField(TEXT("input"), CandidateInput);
	auto Candidates = Command(TEXT("asset.owned_candidates"), CandidateArgs);
	TSharedPtr<FJsonObject> CandidatePayload;
	if (!Candidates.IsOk() || !UE::UAI::CommandContract::Json::TryParseObject(Candidates.GetPayloadJson(), CandidatePayload, Error)) { AddError(Candidates.GetErrorJson()); return false; }
	FString CandidateName;
	for (const auto& Name : CandidatePayload->GetArrayField(TEXT("names")))
		if (Name->AsString() == UUeAgentRecursiveOwnedObject::StaticClass()->GetDisplayNameText().ToString()) CandidateName = Name->AsString();
	if (!TestFalse(TEXT("real native candidate has a public readable name"), CandidateName.IsEmpty())) return false;
	if (!Export(TEXT("append"))) return false;
	int32 Collections = 0;
	if (!Edit(TEXT("append"), [&](const FString& Name, const TSharedPtr<FJsonObject>& Row) {
		if (Name != TEXT("OwnedObjects")) return;
		++Collections;
		auto Label = MakeShared<FJsonObject>(); Label->SetStringField(TEXT("type"), TEXT("string")); Label->SetStringField(TEXT("value"), TEXT("added-owned"));
		auto Fields = MakeShared<FJsonObject>(); Fields->SetObjectField(TEXT("Label"), Label);
		auto Properties = MakeShared<FJsonObject>(); Properties->SetObjectField(TEXT("UAI"), Fields);
		auto Owned = MakeShared<FJsonObject>(); Owned->SetStringField(TEXT("name"), CandidateName); Owned->SetObjectField(TEXT("properties"), Properties);
		auto Item = MakeShared<FJsonObject>(); Item->SetObjectField(TEXT("owned_object"), Owned);
		auto Items = Row->GetArrayField(TEXT("value")); Items.Add(MakeShared<FJsonValueObject>(Item)); Row->SetArrayField(TEXT("value"), Items);
	})) return false;
	if (!TestEqual(TEXT("one editable root owned collection"), Collections, 1)) return false;
	Imported = Import(TEXT("append"));
	if (!TestTrue(TEXT("public native candidate creates owned object"), Imported.IsOk())) { AddError(Imported.GetErrorJson()); return false; }
	auto* Asset = LoadObject<UUeAgentRecursiveDataAsset>(nullptr, *Target);
	if (!TestTrue(TEXT("new object has native Outer, explicit label and complete defaults"), CheckGraph(Asset, TEXT("edited-"))
		&& Asset->OwnedObjects.Num() == 2 && Asset->OwnedObjects[1]->GetOuter() == Asset && Asset->OwnedObjects[1]->Label == TEXT("added-owned"))) return false;
	const FName AddedName = Asset->OwnedObjects[1]->GetFName();
	Asset = nullptr;
	if (!Export(TEXT("reorder")) || !Edit(TEXT("reorder"), [&](const FString& Name, const TSharedPtr<FJsonObject>& Row) {
		if (Name == TEXT("OwnedObjects")) { auto Items = Row->GetArrayField(TEXT("value")); if (Items.Num() == 2) { Items.Swap(0, 1); Row->SetArrayField(TEXT("value"), Items); } }
	})) return false;
	Imported = Import(TEXT("reorder"));
	if (!TestTrue(TEXT("owned collection reorder imports"), Imported.IsOk())) { AddError(Imported.GetErrorJson()); return false; }
	Asset = LoadObject<UUeAgentRecursiveDataAsset>(nullptr, *Target);
	if (!TestTrue(TEXT("reorder preserves identities and shared/cyclic references"), CheckGraph(Asset, TEXT("edited-")) && Asset->OwnedObjects.Num() == 2 && Asset->OwnedObjects[0]->GetFName() == AddedName)) return false;
	Asset = nullptr;
	if (!Export(TEXT("remove")) || !Edit(TEXT("remove"), [&](const FString& Name, const TSharedPtr<FJsonObject>& Row) {
		if (Name == TEXT("OwnedObjects")) { auto Items = Row->GetArrayField(TEXT("value")); if (Items.Num() == 2) { Items.RemoveAt(0); Row->SetArrayField(TEXT("value"), Items); } }
	})) return false;
	Imported = Import(TEXT("remove"));
	if (!TestTrue(TEXT("unreferenced owned occurrence removal imports"), Imported.IsOk())) { AddError(Imported.GetErrorJson()); return false; }
	Asset = LoadObject<UUeAgentRecursiveDataAsset>(nullptr, *Target);
	if (!TestTrue(TEXT("removal retains original hierarchy, values and references"), CheckGraph(Asset, TEXT("edited-")) && Asset->OwnedObjects.Num() == 1)) return false;
	Asset = nullptr;
	if (!Export(TEXT("child-append"))) return false;
	int32 ChildCollections = 0;
	if (!Edit(TEXT("child-append"), [&](const FString& Name, const TSharedPtr<FJsonObject>& Row) {
		if (Name != TEXT("Children") || Row->GetArrayField(TEXT("value")).Num() != 1) return;
		++ChildCollections;
		auto Owned = MakeShared<FJsonObject>(); Owned->SetStringField(TEXT("name"), CandidateName);
		auto Item = MakeShared<FJsonObject>(); Item->SetObjectField(TEXT("owned_object"), Owned);
		auto Items = Row->GetArrayField(TEXT("value")); Items.Add(MakeShared<FJsonValueObject>(Item)); Row->SetArrayField(TEXT("value"), Items);
	})) return false;
	if (!TestEqual(TEXT("one occupied descendant collection selected"), ChildCollections, 1)) return false;
	Imported = Import(TEXT("child-append"));
	if (!TestTrue(TEXT("same native candidate creates inside descendant owned collection"), Imported.IsOk())) { AddError(Imported.GetErrorJson()); return false; }
	Asset = LoadObject<UUeAgentRecursiveDataAsset>(nullptr, *Target);
	if (!TestTrue(TEXT("new descendant uses its real parent and native defaults"), CheckGraph(Asset, TEXT("edited-"))
		&& Asset->OwnedObjects[0]->Children.Num() == 2 && Asset->OwnedObjects[0]->Children[1]->GetOuter() == Asset->OwnedObjects[0]
		&& Asset->OwnedObjects[0]->Children[1]->Label == TEXT("native-child"))) return false;
	const FName AddedChildName = Asset->OwnedObjects[0]->Children[1]->GetFName();
	Asset = nullptr;
	if (!Export(TEXT("child-reorder")) || !Edit(TEXT("child-reorder"), [&](const FString& Name, const TSharedPtr<FJsonObject>& Row) {
		if (Name == TEXT("Children") && Row->GetArrayField(TEXT("value")).Num() == 2) { auto Items = Row->GetArrayField(TEXT("value")); Items.Swap(0,1); Row->SetArrayField(TEXT("value"), Items); }
	})) return false;
	Imported = Import(TEXT("child-reorder"));
	if (!TestTrue(TEXT("descendant collection reorder imports"), Imported.IsOk())) { AddError(Imported.GetErrorJson()); return false; }
	Asset = LoadObject<UUeAgentRecursiveDataAsset>(nullptr, *Target);
	if (!TestTrue(TEXT("descendant reorder preserves identities and cyclic references"), CheckGraph(Asset, TEXT("edited-"))
		&& Asset->OwnedObjects[0]->Children.Num() == 2 && Asset->OwnedObjects[0]->Children[0]->GetFName() == AddedChildName)) return false;
	Asset = nullptr;
	if (!Export(TEXT("child-remove")) || !Edit(TEXT("child-remove"), [&](const FString& Name, const TSharedPtr<FJsonObject>& Row) {
		if (Name == TEXT("Children") && Row->GetArrayField(TEXT("value")).Num() == 2) { auto Items = Row->GetArrayField(TEXT("value")); Items.RemoveAt(0); Row->SetArrayField(TEXT("value"), Items); }
	})) return false;
	Imported = Import(TEXT("child-remove"));
	if (!TestTrue(TEXT("unreferenced descendant occurrence removal imports"), Imported.IsOk())) { AddError(Imported.GetErrorJson()); return false; }
	Asset = LoadObject<UUeAgentRecursiveDataAsset>(nullptr, *Target);
	if (!TestTrue(TEXT("descendant removal retains the original complete graph"), CheckGraph(Asset, TEXT("edited-")) && Asset->OwnedObjects[0]->Children.Num() == 1)) return false;
	TArray<uint8> OriginalDisk, CurrentDisk;
	if (!FFileHelper::LoadFileToArray(OriginalDisk, *Filename)) return false;
	Asset->Modify(); Asset->NativeCounter = 123; Asset->MarkPackageDirty();
	if (!Export(TEXT("late")) || !Edit(TEXT("late"), [&](const FString& Name, const TSharedPtr<FJsonObject>& Row) {
		if (Name == TEXT("Label")) Row->SetStringField(TEXT("value"), TEXT("late-") + Row->GetStringField(TEXT("value")));
	})) return false;
	bool SawPublished = false;
	const auto Loaded = FCoreUObjectDelegates::OnAssetLoaded.AddLambda([&](UObject* Object) {
		auto* Published = Cast<UUeAgentRecursiveDataAsset>(Object);
		if (SawPublished || !Published || Published->GetPathName() != Target || Published->Label != TEXT("late-edited-violet-root")) return;
		TArray<uint8> Bytes;
		SawPublished = CheckGraph(Published, TEXT("late-edited-")) && Published->NativeCounter == 123
			&& !Published->GetPackage()->IsDirty() && FFileHelper::LoadFileToArray(Bytes, *Filename) && Bytes != OriginalDisk;
		if (SawPublished) { AddInfo(TEXT("T13 workspace late rejection directly observed saved/reloaded new root and descendants before rejection")); Published->Label = TEXT("injected-readback-mismatch"); }
	});
	AddExpectedError(TEXT("UAI workspace operation: readback_mismatch; final_readback;"), EAutomationExpectedErrorFlags::Contains, 1);
	Asset = nullptr; Imported = Import(TEXT("late")); FCoreUObjectDelegates::OnAssetLoaded.Remove(Loaded);
	TestFalse(TEXT("late new-value readback mismatch rejects"), Imported.IsOk());
	TestTrue(TEXT("complete new persistent state observed before late rejection"), SawPublished);
	Asset = LoadObject<UUeAgentRecursiveDataAsset>(nullptr, *Target);
	FFileHelper::LoadFileToArray(CurrentDisk, *Filename);
	TestTrue(TEXT("late rollback restores original dirty memory and independent original disk"), CheckGraph(Asset, TEXT("edited-"))
		&& Asset->NativeCounter == 123 && Asset->GetPackage()->IsDirty() && CurrentDisk == OriginalDisk);
	TestTrue(TEXT("final independent editable export after restore"), Export(TEXT("restored-readback")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T13DomainProviderActivationTest,
	"UAI.StrictV2.T13.UOBJECT.RECURSIVE.INIT.DomainProviderActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T13DomainProviderActivationTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::HjsonWorkspace;
	FWorkspaceError Error;
	TSharedPtr<const FImportInitializationDomainProviderRegistry> Registry;
	if (!TestTrue(
		TEXT("the process HJSON domain provider catalog freezes"),
		TryFreezeProcessImportInitializationDomainProviders(
			Registry, Error)))
	{
		AddError(Error.GetCode());
		return false;
	}
#if defined(UAI_HJSON_DOMAIN_PROVIDER_RESOLVES_CONTRACT)
	const FImportInitializationDomainRequest Request(
		TEXT("content_graph.import_workspace"),
		TEXT("uai.uobject_dataasset.recursive/1"),
		TEXT("/Game/UAI/StrictV2/T13/ProviderFixture.ProviderFixture"),
		TEXT("/Script/UeAgentInterface.UeAgentRecursiveDataAsset"),
		EInitializationNodeRole::RootTarget,
		TEXT("{}"));
	FImportInitializationDomainResolution Resolution;
	TestTrue(
		TEXT("the UObject/DataAsset schema and root type resolve one provider"),
		Registry->TryResolve(Request, Resolution, Error));
	TestTrue(TEXT("the resolved provider and contract exist"), Resolution.IsValid());
	if (Resolution.IsValid())
	{
		TestEqual(
			TEXT("the provider identity is exact"),
			Resolution.GetProvider()->GetProviderId(),
			FString(TEXT("uobject-dataasset-domain-provider")));
		TestEqual(
			TEXT("the provider supplies the root initialization contract"),
			Resolution.GetInitializationContractId(),
			FString(TEXT("uai.init.uobject_dataasset.root/1")));
	}
#else
	const FImportInitializationDomainRequest Request(
		TEXT("content_graph.import_workspace"),
		TEXT("uai.uobject_dataasset.recursive/1"),
		TEXT("/Game/UAI/StrictV2/T13/ProviderFixture.ProviderFixture"),
		TEXT("/Script/UeAgentInterface.UeAgentRecursiveDataAsset"),
		EInitializationNodeRole::RootTarget,
		TEXT("uai.init.uobject_dataasset.root/1"),
		TEXT("{}"));
	TSharedPtr<const IImportInitializationDomainProvider> Provider;
	TestTrue(
		TEXT("the UObject/DataAsset schema resolves one provider"),
		Registry->TryResolve(Request, Provider, Error));
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T13InventoryAndActivationTest,
	"UAI.StrictV2.T13.UOBJECT.RECURSIVE.INIT.InventoryAndActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T13InventoryAndActivationTest::RunTest(
	const FString& Parameters)
{
	for (const FUeAgentUObjectDataAssetCommandRoute& Route
		: GetUObjectDataAssetCommandRoutes())
	{
		TestTrue(TEXT("every T13 route is valid"), Route.IsValid());
		TestFalse(
			TEXT("recursive data-asset routes use the shared domain route"),
			Route.bRequiresExplicitDomainSupport);
	}
	TSharedPtr<const FUeAgentImmutableCommandRegistry> ProcessRegistry;
	TSharedPtr<const FUeAgentExactCommandDispatch> ProcessDispatch;
	FString Error;
	TestTrue(
		TEXT("process active composition includes T13"),
		TryComposeProcessActiveCommandComposition(
			ProcessRegistry,
			ProcessDispatch,
			Error));
	for (const FString& CommandId : ExpectedCommandIds)
	{
		const auto* Contract = ProcessRegistry.IsValid()
			? ProcessRegistry->FindExact(CommandId)
			: nullptr;
		TestNotNull(TEXT("every T13 command has one active exact contract"), Contract);
		if (Contract)
		{
			TestEqual(
				TEXT("the active contract publishes the unique T13 owner"),
				Contract->GetPublicOwner(),
			FString(TEXT("content-graph-workspace-owner")));
			if (CommandId == TEXT("content_graph.import_workspace"))
			{
				TSet<FString> ResultFields;
				for (const FUeAgentCommandFieldDeclaration& Field
					: Contract->GetResultFields())
				{
					ResultFields.Add(Field.Name);
				}
				for (const FString& RequiredField : { TEXT("status") })
				{
					TestTrue(
						TEXT("import contract exposes each common result field"),
						ResultFields.Contains(RequiredField));
				}
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T13ExactDispatchTest,
	"UAI.Root.T36.PublicIngress.AuthorityGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T13ExactDispatchTest::RunTest(const FString& Parameters)
{
	FFixtureRegistration Fixture = MakeRegistration(this);
	if (!Fixture.Registry.IsValid() || !Fixture.Dispatch.IsValid())
	{
		return false;
	}
	const FUeAgentCommandResult Export = Submit(
		Fixture.Registry,
		Fixture.Dispatch,
		TEXT("content_graph.export_workspace"),
		TEXT("{\"target\":\"/Game/StrictV2/T13/Fixture.Fixture\",\"workspace_path\":\"D:/Temp/T13\"}"));
	TestTrue(TEXT("exact export route delegates successfully"), Export.IsOk());
	TestEqual(TEXT("one request reaches one service call"), Fixture.Service->Calls, 1);
	TestEqual(
		TEXT("service receives the unchanged exact command identity"),
		Fixture.Service->LastRequest.CommandId,
		FString(TEXT("content_graph.export_workspace")));

	const FUeAgentCommandResult Unauthorized = Submit(
		Fixture.Registry,
		Fixture.Dispatch,
		TEXT("content_graph.import_workspace"),
		TEXT("{}"),
		{});
	TestTrue(TEXT("missing operation authority is a business failure"), !Unauthorized.IsOk() && !Unauthorized.IsProtocolError());
	TestTrue(TEXT("authority failure remains typed"), Unauthorized.GetErrorJson().Contains(TEXT("content_graph_authority_required")));
	TestEqual(TEXT("unauthorized request never reaches the owner"), Fixture.Service->Calls, 1);

	Fixture.Service->bFail = true;
	const FUeAgentCommandResult Failure = Submit(
		Fixture.Registry,
		Fixture.Dispatch,
		TEXT("content_graph.export_workspace"),
		TEXT("{\"target\":\"/Game/StrictV2/T13/Fixture.Fixture\",\"workspace_path\":\"D:/Temp/T13\"}"));
	TestTrue(TEXT("owner failure remains a business failure"), !Failure.IsOk() && !Failure.IsProtocolError());
	TestTrue(TEXT("owner code is preserved"), Failure.GetErrorJson().Contains(TEXT("fixture_t13_failed")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T13ProductionRoundTripTest,
	"UAI.StrictV2.T13.UOBJECT.RECURSIVE.INIT.ProductionPublicPersistReloadExport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T13ProductionRoundTripTest::RunTest(
	const FString& Parameters)
{
	TestTrue(TEXT("production UObject/DataAsset mechanics run on the game thread"), IsInGameThread());
	const FString Target = TEXT("/Game/StrictV2/T13/DA_Recursive.DA_Recursive");
	const FString Workspace = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / TEXT("Automation/StrictV2/T13/export"));
	const FString SourceRoot = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / TEXT("Automation/StrictV2/T13/import_sources"));
	const FString SingleTarget =
		TEXT("/Game/StrictV2/T13/DA_RecursiveSingle.DA_RecursiveSingle");
	const FString DirectoryTarget =
		TEXT("/Game/StrictV2/T13/DA_RecursiveDirectory.DA_RecursiveDirectory");
	const FString AlternateTarget =
		TEXT("/Game/StrictV2/T13/DA_RecursiveAlternate.DA_RecursiveAlternate");
	const FString DefaultsTarget =
		TEXT("/Game/StrictV2/T13/DA_RecursiveDefaults.DA_RecursiveDefaults");
	const FString SingleFile =
		FPaths::Combine(SourceRoot, TEXT("single/asset.hjson"));
	const FString DirectoryRoot = FPaths::Combine(SourceRoot, TEXT("directory"));
	DeleteAssetIfPresent(Target);
	DeleteAssetIfPresent(SingleTarget);
	DeleteAssetIfPresent(DirectoryTarget);
	DeleteAssetIfPresent(AlternateTarget);
	DeleteAssetIfPresent(DefaultsTarget);
	IFileManager::Get().DeleteDirectory(*Workspace, false, true);
	IFileManager::Get().DeleteDirectory(*SourceRoot, false, true);
	TestTrue(
		TEXT("single-file source fixture persists"),
		WriteText(SingleFile, RecursiveDocument()));
	TestTrue(
		TEXT("directory source fixture persists its document and machine manifest"),
		WriteDirectoryWorkspace(
			DirectoryRoot,
			DirectoryTarget,
			RecursiveDocument()));

	TSharedPtr<IUeAgentUObjectDataAssetCommandService> Service;
	FString Error;
	TestTrue(
		TEXT("production T13 service composes"),
		TryCreateProductionUObjectDataAssetCommandService(Service, Error));
	if (!Service.IsValid())
	{
		AddError(Error);
		return false;
	}
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	TestTrue(
		TEXT("production T13 contribution composes"),
		TryCreateUObjectDataAssetRegistration(
			Service.ToSharedRef(), Registry, Dispatch, Error));
	if (!Registry.IsValid() || !Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}

	const FUeAgentCommandResult Imported = Submit(
		Registry,
		Dispatch,
		TEXT("content_graph.import_workspace"),
		ImportParams(Target, RecursiveDocument()));
	TestTrue(TEXT("public HJSON import succeeds"), Imported.IsOk());
	if (!Imported.IsOk())
	{
		AddError(Imported.GetErrorJson());
	}
	if (Imported.IsOk())
	{
		const TSharedPtr<FJsonObject> ImportPayload = Payload(this, Imported);
		TestTrue(TEXT("successful import exposes only its compact status"),
			ImportPayload.IsValid()
				&& ImportPayload->Values.Num() == 1
				&& ImportPayload->GetStringField(TEXT("status")) == TEXT("succeeded"));
	}

	const FUeAgentCommandResult SingleImported = Submit(
		Registry,
		Dispatch,
		TEXT("content_graph.import_workspace"),
		FileImportParams(SingleTarget, TEXT("single_file"), SingleFile));
	TestTrue(
		TEXT("the same public command imports a single-file source"),
		SingleImported.IsOk());

	const FUeAgentCommandResult DirectoryImported = Submit(
		Registry,
		Dispatch,
		TEXT("content_graph.import_workspace"),
		FileImportParams(DirectoryTarget, TEXT("directory"), DirectoryRoot));
	TestTrue(
		TEXT("the same public command imports a directory source"),
		DirectoryImported.IsOk());

	const FUeAgentCommandResult AlternateImported = Submit(
		Registry, Dispatch, TEXT("content_graph.import_workspace"),
		ImportParams(AlternateTarget, AlternateDocument(false)));
	TestTrue(TEXT("different legal values, ids, names, and topology import"), AlternateImported.IsOk());
	if (!AlternateImported.IsOk()) AddError(AlternateImported.GetErrorJson());
	if (UUeAgentRecursiveDataAsset* Alternate = LoadObject<UUeAgentRecursiveDataAsset>(nullptr, *AlternateTarget))
	{
		TestEqual(TEXT("alternate root value survives reload"), Alternate->Label, FString(TEXT("violet-root")));
		TestEqual(TEXT("alternate integer survives reload"), Alternate->ExplicitNumber, 73);
		TestTrue(TEXT("alternate containers survive reload"),
			Alternate->Numbers == TArray<int32>({2, 7, 11})
				&& Alternate->NamedNumbers.FindRef(TEXT("gamma")) == 34);
		TestTrue(TEXT("alternate owned graph keeps requested native names and Outer"),
			Alternate->OwnedObjects.Num() == 1
				&& Alternate->OwnedObjects[0]
				&& Alternate->OwnedObjects[0]->GetName() == TEXT("BranchNode")
				&& Alternate->OwnedObjects[0]->GetOuter() == Alternate
				&& Alternate->OwnedObjects[0]->Children.Num() == 1
				&& Alternate->OwnedObjects[0]->Children[0]->GetName() == TEXT("LeafNode")
				&& Alternate->OwnedObjects[0]->Children[0]->GetOuter() == Alternate->OwnedObjects[0]
				&& Alternate->OwnedObjects[0]->Peer == Alternate->OwnedObjects[0]->Children[0]
				&& Alternate->OwnedObjects[0]->Children[0]->Peer == Alternate->OwnedObjects[0]);
	}
	else AddError(TEXT("alternate imported asset is missing"));

	const FUeAgentCommandResult DefaultsImported = Submit(
		Registry, Dispatch, TEXT("content_graph.import_workspace"),
		ImportParams(DefaultsTarget, AlternateDocument(true)));
	TestTrue(TEXT("omitted fields retain constructor defaults"), DefaultsImported.IsOk());
	if (!DefaultsImported.IsOk()) AddError(DefaultsImported.GetErrorJson());
	if (UUeAgentRecursiveDataAsset* Defaults = LoadObject<UUeAgentRecursiveDataAsset>(nullptr, *DefaultsTarget))
	{
		TestEqual(TEXT("omitted root label keeps native default"), Defaults->Label, FString(TEXT("native-root")));
		TestEqual(TEXT("omitted root number keeps native default"), Defaults->ExplicitNumber, 0);
		TestTrue(TEXT("omitted containers keep native empty defaults"),
			Defaults->Numbers.IsEmpty() && Defaults->NamedNumbers.IsEmpty());
		TestTrue(TEXT("omitted child values and full default subobjects remain native"),
			Defaults->NativeCounter == 11 && Defaults->NativeDefaultSubobject
				&& Defaults->NativeDefaultSubobject->NativeDefaultValue == 29
				&& Defaults->OwnedObjects.Num() == 1 && Defaults->OwnedObjects[0]
				&& Defaults->OwnedObjects[0]->Label == TEXT("native-child")
				&& Defaults->OwnedObjects[0]->NativeCounter == 17
				&& Defaults->OwnedObjects[0]->NativeDefaultSubobject
				&& Defaults->OwnedObjects[0]->NativeDefaultSubobject->NativeDefaultValue == 29);
	}
	else AddError(TEXT("defaults imported asset is missing"));

	if (Imported.IsOk())
	{
		const TSharedPtr<FJsonObject> ExportParamsObject = MakeShared<FJsonObject>();
		ExportParamsObject->SetStringField(TEXT("target"), Target);
		ExportParamsObject->SetStringField(TEXT("workspace_path"), Workspace);
		ExportParamsObject->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		FString ExportParams;
		FString CanonicalError;
		UE::UAI::CommandContract::Json::TryCanonicalize(ExportParamsObject, ExportParams, CanonicalError);
		const FUeAgentCommandResult Exported = Submit(
			Registry,
			Dispatch,
			TEXT("content_graph.export_workspace"),
			ExportParams);
		TestTrue(TEXT("public verified HJSON export succeeds"), Exported.IsOk());
		const TSharedPtr<FJsonObject> ExportPayload = Exported.IsOk()
			? Payload(this, Exported)
			: nullptr;
		if (ExportPayload.IsValid())
		{
			TestEqual(TEXT("verified export reports one public field"),
				ExportPayload->Values.Num(), 1);
			TestEqual(TEXT("verified export reports success"),
				ExportPayload->GetStringField(TEXT("status")),
				FString(TEXT("succeeded")));
			TestFalse(TEXT("export keeps its plan fingerprint internal"),
				ExportPayload->HasField(TEXT("plan_fingerprint")));
			TestFalse(TEXT("export keeps semantic verification internal"),
				ExportPayload->HasField(TEXT("semantic_verification")));
		}
	}

	DeleteAssetIfPresent(Target);
	DeleteAssetIfPresent(SingleTarget);
	DeleteAssetIfPresent(DirectoryTarget);
	DeleteAssetIfPresent(AlternateTarget);
	DeleteAssetIfPresent(DefaultsTarget);
	IFileManager::Get().DeleteDirectory(*Workspace, false, true);
	IFileManager::Get().DeleteDirectory(*SourceRoot, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T13FailureCleanupTest,
	"UAI.StrictV2.T13.UOBJECT.RECURSIVE.INIT.PrewriteAndRollbackCleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T13FailureCleanupTest::RunTest(
	const FString& Parameters)
{
	TSharedPtr<IUeAgentUObjectDataAssetCommandService> Service;
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	TestTrue(TEXT("production service composes"), TryCreateProductionUObjectDataAssetCommandService(Service, Error));
	TestTrue(TEXT("production registration composes"), Service.IsValid() && TryCreateUObjectDataAssetRegistration(Service.ToSharedRef(), Registry, Dispatch, Error));
	if (!Registry.IsValid() || !Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}

	const FString CycleTarget = TEXT("/Game/StrictV2/T13/DA_Cycle.DA_Cycle");
	DeleteAssetIfPresent(CycleTarget);
	const FUeAgentCommandResult Cycle = Submit(
		Registry, Dispatch, TEXT("content_graph.import_workspace"),
		ImportParams(CycleTarget, RecursiveDocument(true)));
	TestTrue(TEXT("ownership cycle is a typed business failure"), !Cycle.IsOk() && Cycle.GetErrorJson().Contains(TEXT("hjson_import_ownership_cycle")));
	TestNull(TEXT("ownership cycle performs no write"), StaticFindObject(UObject::StaticClass(), nullptr, *CycleTarget));

	const FString UnknownTarget = TEXT("/Game/StrictV2/T13/DA_Unknown.DA_Unknown");
	DeleteAssetIfPresent(UnknownTarget);
	const FString UnknownDocument = RecursiveDocument().Replace(
		TEXT("/Script/UeAgentInterface.UeAgentRecursiveOwnedObject"),
		TEXT("/Script/UeAgentInterface.DoesNotExist"),
		ESearchCase::CaseSensitive);
	const FUeAgentCommandResult Unknown = Submit(
		Registry, Dispatch, TEXT("content_graph.import_workspace"),
		ImportParams(UnknownTarget, UnknownDocument));
	TestTrue(TEXT("unknown type is rejected before write"), !Unknown.IsOk() && Unknown.GetErrorJson().Contains(TEXT("uobject_dataasset_type_unknown")));
	TestNull(TEXT("unknown type leaves no asset"), StaticFindObject(UObject::StaticClass(), nullptr, *UnknownTarget));

	const FString InvalidValueTarget = TEXT("/Game/StrictV2/T13/DA_InvalidValue.DA_InvalidValue");
	const FString InvalidValuePackage = FPackageName::ObjectPathToPackageName(InvalidValueTarget);
	const FString InvalidValueFile = FPackageName::LongPackageNameToFilename(
		InvalidValuePackage, FPackageName::GetAssetPackageExtension());
	DeleteAssetIfPresent(InvalidValueTarget);
	IAssetRegistry& ValueRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	int32 InvalidValueRegistrations = 0;
	const FDelegateHandle AddedHandle = ValueRegistry.OnAssetAdded().AddLambda(
		[&](const FAssetData& AssetData)
		{
			if (AssetData.PackageName.ToString() == InvalidValuePackage) ++InvalidValueRegistrations;
		});
	const UUeAgentRecursiveDataAsset* RootDefaults = GetDefault<UUeAgentRecursiveDataAsset>();
	const UUeAgentRecursiveOwnedObject* OwnedDefaults = GetDefault<UUeAgentRecursiveOwnedObject>();
	const FString DefaultLabel = RootDefaults->Label;
	const int32 DefaultNumber = RootDefaults->ExplicitNumber;
	const TArray<int32> DefaultNumbers = RootDefaults->Numbers;
	const TMap<FString, int32> DefaultNamedNumbers = RootDefaults->NamedNumbers;
	const FString DefaultOwnedLabel = OwnedDefaults->Label;
	const TArray<TPair<FString, FString>> InvalidValues = {
		{TEXT("\"explicit_number\":42"), TEXT("\"explicit_number\":\"invalid\"")},
		{TEXT("\"explicit_number\":42"), TEXT("\"explicit_number\":2147483648")},
		{TEXT("\"numbers\":[3,5,8]"), TEXT("\"numbers\":[3,\"invalid\",8]")},
		{TEXT("\"alpha\":13"), TEXT("\"alpha\":\"invalid\"")},
		{TEXT("\"label\":\"deep\""), TEXT("\"label\":42")},
	};
	for (const TPair<FString, FString>& Replacement : InvalidValues)
	{
		const FString Document = RecursiveDocument().Replace(
			*Replacement.Key, *Replacement.Value, ESearchCase::CaseSensitive);
		TestTrue(TEXT("invalid value case changes the valid document"), Document != RecursiveDocument());
		const FUeAgentCommandResult InvalidValue = Submit(
			Registry, Dispatch, TEXT("content_graph.import_workspace"), ImportParams(InvalidValueTarget, Document));
		TestFalse(TEXT("invalid native property value is rejected"), InvalidValue.IsOk());
		TestFalse(TEXT("invalid value does not enter the mutation lifecycle"),
			InvalidValue.GetErrorJson().Contains(TEXT("mutation_started")));
		TestEqual(TEXT("invalid value never registers the target asset"), InvalidValueRegistrations, 0);
		TestNull(TEXT("invalid value leaves no object"), StaticFindObject(UObject::StaticClass(), nullptr, *InvalidValueTarget));
		TestFalse(TEXT("invalid value leaves no registry entry"),
			ValueRegistry.GetAssetByObjectPath(FSoftObjectPath(InvalidValueTarget)).IsValid());
		TestFalse(TEXT("invalid value leaves no disk package"), IFileManager::Get().FileExists(*InvalidValueFile));
		AddInfo(FString::Printf(TEXT("prewrite value rejection: %s"), *InvalidValue.GetErrorJson()));
	}
	ValueRegistry.OnAssetAdded().Remove(AddedHandle);
	TestEqual(TEXT("preflight preserves root CDO label"), RootDefaults->Label, DefaultLabel);
	TestEqual(TEXT("preflight preserves root CDO number"), RootDefaults->ExplicitNumber, DefaultNumber);
	TestTrue(TEXT("preflight preserves root CDO array"), RootDefaults->Numbers == DefaultNumbers);
	TestTrue(TEXT("preflight preserves root CDO map"), RootDefaults->NamedNumbers.OrderIndependentCompareEqual(DefaultNamedNumbers));
	TestEqual(TEXT("preflight preserves owned CDO label"), OwnedDefaults->Label, DefaultOwnedLabel);

	const FString BoundaryRoot = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / TEXT("Automation/StrictV2/T13/source_failures"));
	IFileManager::Get().DeleteDirectory(*BoundaryRoot, false, true);
	const FString MissingTarget = TEXT("/Game/StrictV2/T13/DA_MissingManifest.DA_MissingManifest");
	const FString MissingRoot = FPaths::Combine(BoundaryRoot, TEXT("missing_manifest"));
	DeleteAssetIfPresent(MissingTarget);
	TestTrue(
		TEXT("missing-manifest source root exists"),
		IFileManager::Get().MakeDirectory(*MissingRoot, true));
	const FUeAgentCommandResult Missing = Submit(
		Registry,
		Dispatch,
		TEXT("content_graph.import_workspace"),
		FileImportParams(MissingTarget, TEXT("directory"), MissingRoot));
	TestTrue(
		TEXT("directory without its machine manifest is rejected before write"),
		!Missing.IsOk()
			&& Missing.GetErrorJson().Contains(
				TEXT("content_graph_workspace_manifest_missing")));
	TestNull(
		TEXT("missing-manifest failure leaves no asset"),
		StaticFindObject(UObject::StaticClass(), nullptr, *MissingTarget));

	const FString DigestTarget = TEXT("/Game/StrictV2/T13/DA_DigestMismatch.DA_DigestMismatch");
	const FString DigestRoot = FPaths::Combine(BoundaryRoot, TEXT("digest_mismatch"));
	DeleteAssetIfPresent(DigestTarget);
	TestTrue(
		TEXT("digest-mismatch document persists"),
		WriteText(FPaths::Combine(DigestRoot, TEXT("asset.hjson")), RecursiveDocument()));
	TestTrue(
		TEXT("digest-mismatch manifest persists"),
		WriteDirectoryManifest(
			DigestRoot,
			DigestTarget,
			TEXT("asset.hjson"),
			TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000")));
	const FUeAgentCommandResult Digest = Submit(
		Registry,
		Dispatch,
		TEXT("content_graph.import_workspace"),
		FileImportParams(DigestTarget, TEXT("directory"), DigestRoot));
	TestTrue(
		TEXT("directory digest mismatch is rejected before write"),
		!Digest.IsOk()
			&& Digest.GetErrorJson().Contains(
				TEXT("content_graph_workspace_digest_mismatch")));
	TestNull(
		TEXT("digest mismatch leaves no asset"),
		StaticFindObject(UObject::StaticClass(), nullptr, *DigestTarget));

	const FString EscapeTarget = TEXT("/Game/StrictV2/T13/DA_PathEscape.DA_PathEscape");
	const FString EscapeRoot = FPaths::Combine(BoundaryRoot, TEXT("path_escape"));
	DeleteAssetIfPresent(EscapeTarget);
	TestTrue(
		TEXT("path-escape manifest persists"),
		WriteDirectoryManifest(
			EscapeRoot,
			EscapeTarget,
			TEXT("../outside.hjson"),
			Sha256Text(RecursiveDocument())));
	const FUeAgentCommandResult Escape = Submit(
		Registry,
		Dispatch,
		TEXT("content_graph.import_workspace"),
		FileImportParams(EscapeTarget, TEXT("directory"), EscapeRoot));
	TestTrue(
		TEXT("directory path escape is rejected before write"),
		!Escape.IsOk()
			&& Escape.GetErrorJson().Contains(TEXT("workspace_path_invalid")));
	TestNull(
		TEXT("path escape leaves no asset"),
		StaticFindObject(UObject::StaticClass(), nullptr, *EscapeTarget));

	const FString RollbackTarget = TEXT("/Game/StrictV2/T13/DA_Rollback.DA_Rollback");
	DeleteAssetIfPresent(RollbackTarget);
	{
		UE::UAI::OperationOwners::UObjectDataAsset::TestHooks::
			FScopedFailureAfterNode Failure(TEXT("deep"));
		const FUeAgentCommandResult RolledBack = Submit(
			Registry, Dispatch, TEXT("content_graph.import_workspace"),
			ImportParams(RollbackTarget, RecursiveDocument()));
		AddInfo(TEXT("child rejection result: ") + RolledBack.GetErrorJson());
		TestTrue(TEXT("child creation failure is surfaced"), !RolledBack.IsOk() && RolledBack.GetErrorJson().Contains(TEXT("uobject_dataasset_child_create_failed")));
		TestTrue(TEXT("child failure reaches rolled-back terminal"), RolledBack.GetErrorJson().Contains(TEXT("rolled_back")));
	}
	TestNull(TEXT("confirmed rollback removes the root asset"), StaticFindObject(UObject::StaticClass(), nullptr, *RollbackTarget));

	const FString LateTarget = TEXT("/Game/StrictV2/T13/DA_LateRollback.DA_LateRollback");
	DeleteAssetIfPresent(LateTarget);
	bool bObservedReloaded = false;
	{
		UE::UAI::OperationOwners::UObjectDataAsset::TestHooks::
			FScopedFinalProjectionFailure Failure([&](const UObject& Object)
			{
				const UUeAgentRecursiveDataAsset* Asset = Cast<UUeAgentRecursiveDataAsset>(&Object);
				bObservedReloaded = true;
				TestTrue(TEXT("late rejection directly sees saved complete reloaded request graph"),
					Asset && Asset->GetPathName() == LateTarget
						&& !Asset->GetOutermost()->IsDirty()
						&& FPackageName::DoesPackageExist(TEXT("/Game/StrictV2/T13/DA_LateRollback"))
						&& Asset->Label == TEXT("violet-root") && Asset->ExplicitNumber == 73
						&& Asset->Numbers == TArray<int32>({2, 7, 11})
						&& Asset->NamedNumbers.Num() == 1 && Asset->NamedNumbers.FindRef(TEXT("gamma")) == 34
						&& Asset->NativeDefaultSubobject && Asset->NativeDefaultSubobject->GetOuter() == Asset
						&& Asset->OwnedObjects.Num() == 1 && Asset->OwnedObjects[0]
						&& Asset->OwnedObjects[0]->NodeId == TEXT("branch")
						&& Asset->OwnedObjects[0]->GetName() == TEXT("BranchNode")
						&& Asset->OwnedObjects[0]->Label == TEXT("violet-branch")
						&& Asset->OwnedObjects[0]->GetOuter() == Asset
						&& Asset->OwnedObjects[0]->Children.Num() == 1 && Asset->OwnedObjects[0]->Children[0]
						&& Asset->OwnedObjects[0]->Children[0]->NodeId == TEXT("leaf-node")
						&& Asset->OwnedObjects[0]->Children[0]->GetName() == TEXT("LeafNode")
						&& Asset->OwnedObjects[0]->Children[0]->GetOuter() == Asset->OwnedObjects[0]
						&& Asset->OwnedObjects[0]->Peer == Asset->OwnedObjects[0]->Children[0]
						&& Asset->OwnedObjects[0]->Shared == Asset->OwnedObjects[0]->Children[0]
						&& Asset->OwnedObjects[0]->Children[0]->Peer == Asset->OwnedObjects[0]);
				AddInfo(TEXT("late rejection observed saved reloaded graph before returning failure"));
				if (Asset)
				{
					auto CheckLeaf = [&](const UObject* Owner, const UUeAgentRecursiveLeafObject* Leaf)
					{
						TestTrue(TEXT("late rejection sees complete native default subobject"),
							Leaf && Leaf->GetOuter() == Owner
								&& Leaf->GetName() == TEXT("NativeDefaultSubobject")
								&& Leaf->GetClass() == UUeAgentRecursiveLeafObject::StaticClass()
								&& Leaf->NativeDefaultValue == GetDefault<UUeAgentRecursiveLeafObject>()->NativeDefaultValue);
					};
					CheckLeaf(Asset, Asset->NativeDefaultSubobject);
					TArray<UUeAgentRecursiveOwnedObject*> Pending = Asset->OwnedObjects;
					for (int32 Index = 0; Index < Pending.Num(); ++Index)
					{
						const UUeAgentRecursiveOwnedObject* Owned = Pending[Index];
						if (!Owned) { AddError(TEXT("late graph contains null owned object")); continue; }
						CheckLeaf(Owned, Owned->NativeDefaultSubobject);
						TestEqual(TEXT("late graph preserves native child counter"), Owned->NativeCounter,
							GetDefault<UUeAgentRecursiveOwnedObject>()->NativeCounter);
						Pending.Append(Owned->Children);
					}
				}
			});
		const FUeAgentCommandResult RolledBack = Submit(
			Registry, Dispatch, TEXT("content_graph.import_workspace"),
			ImportParams(LateTarget, AlternateDocument(false)));
		AddInfo(TEXT("late rejection result: ") + RolledBack.GetErrorJson());
		TestTrue(TEXT("final projection failure is surfaced after independent reload"),
			!RolledBack.IsOk()
				&& RolledBack.GetErrorJson().Contains(TEXT("uobject_dataasset_result_projection_failed"))
				&& RolledBack.GetErrorJson().Contains(TEXT("rolled_back")));
	}
	TestTrue(TEXT("late rejection observer ran before rollback"), bObservedReloaded);
	TestNull(TEXT("late rollback removes the reloaded root object"),
		StaticFindObject(UObject::StaticClass(), nullptr, *LateTarget));
	const FString LateFilename = FPackageName::LongPackageNameToFilename(
		TEXT("/Game/StrictV2/T13/DA_LateRollback"),
		FPackageName::GetAssetPackageExtension());
	TestFalse(TEXT("late rollback removes the saved package"),
		IFileManager::Get().FileExists(*LateFilename));
	const FAssetData LateAssetData = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
		TEXT("AssetRegistry")).Get().GetAssetByObjectPath(FSoftObjectPath(LateTarget));
	TestFalse(TEXT("late rollback removes the asset-registry entry"), LateAssetData.IsValid());
	IFileManager::Get().DeleteDirectory(*BoundaryRoot, false, true);
	return true;
}
