#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Audio/Authoring/UeAgentAudioAuthoringDocuments.h"
#include "Domains/Audio/Authoring/UeAgentAudioAuthoringApplyProvider.h"
#include "UObject/UnrealType.h"
#include "HAL/FileManager.h"
#include "MetasoundBuilderBase.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundEditorSubsystem.h"
#include "MetasoundFactory.h"
#include "MetasoundFrontendLiteral.h"
#include "MetasoundSource.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Sound/SoundSubmixSend.h"
#include "Module/CommandRegistration/UObjectDataAsset/UeAgentUObjectDataAssetRegistration.h"
#include "ObjectTools.h"
#include "OperationOwners/UObjectDataAsset/UeAgentUObjectDataAssetCommandService.h"
#include "QueryOwners/AudioGraph/UeAgentAudioGraphCandidateQuery.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

namespace
{
	using namespace UE::UAI;

	class FNeverCancelled final : public Contract::IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override { return false; }
	};

	CommandEdge::FUeAgentCommandResult SubmitPublicCommand(
		const TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>& Dispatch,
		const FString& CommandId, const FString& RequestId, const FString& ParamsJson)
	{
		const auto* Command = Registry.IsValid() ? Registry->FindExact(CommandId) : nullptr;
		if (!Command || !Dispatch.IsValid())
		{
			return CommandEdge::FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t85-metasound"), RequestId, CommandId,
				TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
				TEXT("metasound_public_command_missing"), TEXT("public command unavailable"));
		}
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"corr-t85-metasound\",\"request_id\":\"%s\",")
			TEXT("\"command_id\":\"%s\",\"command_binding_hash\":\"%s\",\"params\":%s}"),
			*RequestId, *CommandId, *Command->GetContractHash(), *ParamsJson);
		FTCHARToUTF8 Encoded(*Envelope);
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
		CommandEdge::FUeAgentCommandProtocolDiagnostic Diagnostic;
		TOptional<CommandEdge::FUeAgentCommandRequest> Request;
		if (!CommandEdge::FUeAgentCommandProtocolCodec().DecodeRequest(Bytes, Request, Diagnostic)
			|| !Request.IsSet())
		{
			return CommandEdge::FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t85-metasound"), RequestId, CommandId, Command->GetContractHash(),
				TEXT("metasound_public_request_invalid"), TEXT("request decode failed"));
		}
		return Dispatch->Submit(Request.GetValue(), Contract::FUeAgentCallerContext(
			TEXT("t85-metasound"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")},
			TEXT("corr-t85-metasound")), MakeShared<FNeverCancelled>());
	}

	void DeleteAssetIfPresent(const FString& ObjectPath)
	{
		if (UObject* Existing = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath))
		{
			ObjectTools::DeleteObjectsUnchecked({Existing});
		}
	}

	bool SaveSource(UPackage& Package, UMetaSoundSource& Source, const FString& Filename)
	{
		Package.MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		const bool bSaved = UPackage::SavePackage(&Package, &Source, *Filename, Args);
		if (bSaved) Package.SetDirtyFlag(false);
		return bSaved;
	}

	bool TryLoadGraphDocument(const FString& Workspace, FString& OutFile,
		FString& OutText, TSharedPtr<FJsonObject>& OutDocument, FString& OutError)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *Workspace, TEXT("*.hjson"), true, false);
		Files.Sort();
		for (const FString& File : Files)
		{
			if (File.EndsWith(TEXT(".interfaces.hjson"))) continue;
			FString Text;
			TSharedPtr<FJsonObject> Document;
			const TSharedPtr<FJsonObject>* Graph = nullptr;
			FString Semantic;
			if (FFileHelper::LoadFileToString(Text, *File)
				&& FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, OutError)
				&& Document.IsValid()
				&& Document->TryGetObjectField(TEXT("graph"), Graph) && Graph && Graph->IsValid()
				&& (*Graph)->TryGetStringField(TEXT("semantic"), Semantic)
				&& Semantic == TEXT("metasound_graph")
				&& Document->HasTypedField<EJson::Object>(TEXT("nodes"))
				&& Document->HasTypedField<EJson::Array>(TEXT("connections")))
			{
				OutFile = File;
				OutText = MoveTemp(Text);
				OutDocument = MoveTemp(Document);
				return true;
			}
		}
		OutError = TEXT("metasound_visible_graph_not_found");
		return false;
	}

	bool IsReadable(const FString& Text)
	{
		return !Text.Contains(TEXT("sha256:"), ESearchCase::IgnoreCase)
			&& !Text.Contains(TEXT("node_guid"), ESearchCase::IgnoreCase)
			&& !Text.Contains(TEXT("vertex_guid"), ESearchCase::IgnoreCase)
			&& !Text.Contains(TEXT("binding_hash"), ESearchCase::IgnoreCase)
			&& !Text.Contains(TEXT("class_name"), ESearchCase::IgnoreCase)
			&& !Text.Contains(TEXT("major_version"), ESearchCase::IgnoreCase)
			&& !Text.Contains(TEXT("minor_version"), ESearchCase::IgnoreCase);
	}

	bool TryFindAudioOutput(const FString& Workspace, const TSharedPtr<FJsonObject>& Document,
		FString& OutAlias, FString& OutPin, FString* OutDiagnostic = nullptr)
	{
		const TSharedPtr<FJsonObject>* Nodes = nullptr;
		if (!Document.IsValid() || !Document->TryGetObjectField(TEXT("nodes"), Nodes) || !Nodes) return false;
		TMap<FString, TSharedPtr<FJsonValue>> PinInterfaces;
		TArray<FString> InterfaceFiles;
		IFileManager::Get().FindFilesRecursive(InterfaceFiles, *Workspace, TEXT("*.interfaces.hjson"), true, false);
		for (const FString& File : InterfaceFiles)
		{
			FString Text, Error;
			TSharedPtr<FJsonObject> InterfaceDocument;
			const TSharedPtr<FJsonObject>* InterfaceNodes = nullptr;
			if (FFileHelper::LoadFileToString(Text, *File)
				&& FUeAgentStructuredHjsonParser::ParseObject(Text, File, InterfaceDocument, Error)
				&& InterfaceDocument->TryGetObjectField(TEXT("nodes"), InterfaceNodes))
				PinInterfaces.Append((*InterfaceNodes)->Values);
		}
		TArray<FString> ReadableNodeSummaries{
			FString::Printf(TEXT("nodes=%d"), (*Nodes)->Values.Num())};
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Nodes)->Values)
		{
			const TSharedPtr<FJsonObject> Node = Pair.Value.IsValid() && Pair.Value->Type == EJson::Object
				? Pair.Value->AsObject() : nullptr;
			FString Kind;
			const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
			if (!Node.IsValid())
			{
				if (ReadableNodeSummaries.Num() < 9) ReadableNodeSummaries.Add(TEXT("invalid_node"));
				continue;
			}
			if (!Node->TryGetStringField(TEXT("kind"), Kind))
			{
				if (ReadableNodeSummaries.Num() < 9) ReadableNodeSummaries.Add(TEXT("node_without_kind"));
				continue;
			}
			if (const auto* PinInterface = PinInterfaces.Find(Pair.Key);
				PinInterface && (*PinInterface)->Type == EJson::Object)
				(*PinInterface)->AsObject()->TryGetArrayField(TEXT("pins"), Inputs);
			if (ReadableNodeSummaries.Num() < 8)
			{
				FString Label;
				if (!Node->TryGetStringField(TEXT("member_name"), Label))
				{
					Node->TryGetStringField(TEXT("name"), Label);
				}
				TArray<FString> InputSummaries;
				if (Inputs)
				{
					for (const TSharedPtr<FJsonValue>& Value : *Inputs)
					{
						if (InputSummaries.Num() >= 8) break;
						const TSharedPtr<FJsonObject> Input = Value.IsValid() && Value->Type == EJson::Object
							? Value->AsObject() : nullptr;
						FString Name;
						FString Type;
						if (Input.IsValid())
						{
							Input->TryGetStringField(TEXT("alias"), Name);
							Input->TryGetStringField(TEXT("type"), Type);
						}
						InputSummaries.Add(Name + TEXT(":") + Type);
					}
				}
				ReadableNodeSummaries.Add(Kind + TEXT("(") + Label + TEXT(")[")
					+ FString::Join(InputSummaries, TEXT(",")) + TEXT("]"));
			}
			if (Kind != TEXT("graph_output") || !Inputs) continue;
			for (const TSharedPtr<FJsonValue>& Value : *Inputs)
			{
				const TSharedPtr<FJsonObject> Input = Value.IsValid() && Value->Type == EJson::Object
					? Value->AsObject() : nullptr;
				FString Name;
				FString Type;
				FString Direction;
				if (Input.IsValid() && Input->TryGetStringField(TEXT("alias"), Name)
					&& Input->TryGetStringField(TEXT("direction"), Direction) && Direction == TEXT("input")
					&& Input->TryGetStringField(TEXT("type"), Type) && Type == TEXT("Audio"))
				{
					OutAlias = Pair.Key;
					OutPin = Name;
					return true;
				}
			}
		}
		if (OutDiagnostic)
		{
			*OutDiagnostic = FString::Join(ReadableNodeSummaries, TEXT(";"));
		}
		return false;
	}

	bool TryEditWorkspace(const FString& Workspace, const FString& CandidateName,
		FString& OutError)
	{
		FString File;
		FString Text;
		TSharedPtr<FJsonObject> Document;
		if (!TryLoadGraphDocument(Workspace, File, Text, Document, OutError) || !IsReadable(Text))
		{
			if (OutError.IsEmpty()) OutError = TEXT("metasound_visible_graph_contains_opaque_identity");
			return false;
		}
		FString OutputAlias;
		FString OutputPin;
		FString OutputDiagnostic;
		if (!TryFindAudioOutput(Workspace, Document, OutputAlias, OutputPin, &OutputDiagnostic))
		{
			OutError = TEXT("metasound_audio_output_not_found:") + OutputDiagnostic;
			return false;
		}
		const TSharedPtr<FJsonObject>* Nodes = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* ExistingConnections = nullptr;
		Document->TryGetObjectField(TEXT("nodes"), Nodes);
		Document->TryGetArrayField(TEXT("connections"), ExistingConnections);
		TSharedRef<FJsonObject> Sine = MakeShared<FJsonObject>();
		Sine->SetStringField(TEXT("kind"), TEXT("external"));
		Sine->SetStringField(TEXT("name"), CandidateName);
		TSharedRef<FJsonObject> Candidate = MakeShared<FJsonObject>();
		Candidate->SetStringField(TEXT("name"), CandidateName);
		Sine->SetObjectField(TEXT("candidate"), Candidate);
		TSharedRef<FJsonObject> Default = MakeShared<FJsonObject>();
		Default->SetStringField(TEXT("type"), TEXT("float"));
		Default->SetNumberField(TEXT("value"), 440.0);
		TSharedRef<FJsonObject> Frequency = MakeShared<FJsonObject>();
		Frequency->SetObjectField(TEXT("default_value"), Default);
		TSharedRef<FJsonObject> PinDefaults = MakeShared<FJsonObject>();
		PinDefaults->SetObjectField(TEXT("Frequency"), Frequency);
		Sine->SetObjectField(TEXT("pin_defaults"), PinDefaults);
		(*Nodes)->SetObjectField(TEXT("Sine1"), Sine);
		TArray<TSharedPtr<FJsonValue>> Connections = ExistingConnections
			? *ExistingConnections : TArray<TSharedPtr<FJsonValue>>{};
		TSharedRef<FJsonObject> Connection = MakeShared<FJsonObject>();
		Connection->SetStringField(TEXT("from"), TEXT("Sine1.Audio"));
		Connection->SetStringField(TEXT("to"), OutputAlias + TEXT(".") + OutputPin);
		Connections.Add(MakeShared<FJsonValueObject>(Connection));
		Document->SetArrayField(TEXT("connections"), MoveTemp(Connections));
		FString Updated;
		if (!CommandContract::Json::TryCanonicalize(Document, Updated, OutError)
			|| !FFileHelper::SaveStringToFile(Updated, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			if (OutError.IsEmpty()) OutError = TEXT("metasound_visible_graph_write_failed");
			return false;
		}
		return true;
	}

	bool TryReadbackWorkspace(const FString& Workspace, const FString& CandidateName,
		FString& OutError)
	{
		FString File;
		FString Text;
		TSharedPtr<FJsonObject> Document;
		if (!TryLoadGraphDocument(Workspace, File, Text, Document, OutError) || !IsReadable(Text)) return false;
		const TSharedPtr<FJsonObject>* Nodes = nullptr;
		Document->TryGetObjectField(TEXT("nodes"), Nodes);
		FString SineAlias;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Nodes)->Values)
		{
			const TSharedPtr<FJsonObject> Node = Pair.Value.IsValid() && Pair.Value->Type == EJson::Object
				? Pair.Value->AsObject() : nullptr;
			FString FoundName;
			FString Kind;
			const TSharedPtr<FJsonObject>* Candidate = nullptr;
			FString CandidateSelector;
			if (Node.IsValid() && Node->TryGetStringField(TEXT("name"), FoundName)
				&& Node->TryGetStringField(TEXT("kind"), Kind)
				&& Node->TryGetObjectField(TEXT("candidate"), Candidate)
				&& Candidate && Candidate->IsValid()
				&& (*Candidate)->Values.Num() == 1
				&& (*Candidate)->TryGetStringField(TEXT("name"), CandidateSelector)
				&& FoundName == CandidateName && Kind == TEXT("external")
				&& CandidateSelector == CandidateName
				&& !Node->HasField(TEXT("class_name"))
				&& !Node->HasField(TEXT("major_version"))
				&& !Node->HasField(TEXT("minor_version")))
			{
				SineAlias = Pair.Key;
				break;
			}
		}
		FString OutputAlias;
		FString OutputPin;
		const TArray<TSharedPtr<FJsonValue>>* Connections = nullptr;
		if (SineAlias.IsEmpty() || !TryFindAudioOutput(Workspace, Document, OutputAlias, OutputPin)
			|| !Document->TryGetArrayField(TEXT("connections"), Connections) || !Connections)
		{
			OutError = TEXT("metasound_readback_graph_incomplete");
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Connections)
		{
			const TSharedPtr<FJsonObject> Connection = Value.IsValid() && Value->Type == EJson::Object
				? Value->AsObject() : nullptr;
			FString From;
			FString To;
			if (Connection.IsValid() && Connection->TryGetStringField(TEXT("from"), From)
				&& Connection->TryGetStringField(TEXT("to"), To)
				&& From == SineAlias + TEXT(".Audio") && To == OutputAlias + TEXT(".") + OutputPin) return true;
		}
		OutError = TEXT("metasound_readback_connection_missing");
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85MetaSoundCompactRootTest,
 "UAI.Root.T85.MetaSound.CompactRoot", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85MetaSoundCompactRootTest::RunTest(const FString& Parameters)
{
 using namespace UE::UAI;
 const FString Token=FGuid::NewGuid().ToString(EGuidFormats::Digits);
 const FString PackageName=TEXT("/Game/UAI/Transient/T85MetaSoundCompact/MS_")+Token;
 const FString ObjectPath=PackageName+TEXT(".")+FPackageName::GetLongPackageAssetName(PackageName);
 const FString Filename=FPackageName::LongPackageNameToFilename(PackageName,FPackageName::GetAssetPackageExtension());
 const FString Workspace=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("Automation/UAI/T85/AudioCompact")/Token);
 const FString Readback=Workspace+TEXT("_readback");
 ON_SCOPE_EXIT { DeleteAssetIfPresent(ObjectPath); T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this,PackageName); IFileManager::Get().DeleteDirectory(*Workspace,false,true); IFileManager::Get().DeleteDirectory(*Readback,false,true); };
 UPackage* Package=CreatePackage(*PackageName);
 UMetaSoundSourceFactory* Factory=NewObject<UMetaSoundSourceFactory>();
 UMetaSoundSource* Source=Cast<UMetaSoundSource>(Factory->FactoryCreateNew(UMetaSoundSource::StaticClass(),Package,*FPackageName::GetLongPackageAssetName(PackageName),RF_Public|RF_Standalone|RF_Transactional,nullptr,GWarn));
 if (!TestNotNull(TEXT("owned MetaSound source"),Source)) return false;
 Source->bEnableSubmixSends=true;
 FSoundSubmixSendInfo Original; Original.SendLevelControlMethod=ESendLevelControlMethod::Manual; Original.SendLevel=0.37f; Original.MinSendDistance=123.0f; Original.MaxSendDistance=789.0f; Original.SendStage=ESubmixSendStage::PreDistanceAttenuation;
 Source->SoundSubmixSends.Add(Original);
 Source->Priority=2.5f;
 FAssetRegistryModule::AssetCreated(Source);
 if (!TestTrue(TEXT("native original saved"),SaveSource(*Package,*Source,Filename))) return false;
 TSharedPtr<OperationOwners::UObjectDataAsset::IUeAgentUObjectDataAssetCommandService> Service;
 TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
 TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
 FString Error,Params;
 if (!OperationOwners::UObjectDataAsset::TryCreateProductionUObjectDataAssetCommandService(Service,Error)
  || !Module::CommandRegistration::UObjectDataAsset::TryCreateUObjectDataAssetRegistration(Service.ToSharedRef(),Registry,Dispatch,Error)) { AddError(Error); return false; }
 auto ExportTo=[&](const FString& Path) {
  auto Args=MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"),ObjectPath); Args->SetStringField(TEXT("workspace_path"),Path); Args->SetStringField(TEXT("profile_id"),TEXT("llm_authoring"));
  if (!CommandContract::Json::TryCanonicalize(Args,Params,Error)) return false;
  auto Result=SubmitPublicCommand(Registry,Dispatch,TEXT("content_graph.export_workspace"),TEXT("compact-export-")+FGuid::NewGuid().ToString(EGuidFormats::Digits),Params);
  if (!Result.IsOk()) AddError(Result.GetErrorJson()); return Result.IsOk();
 };
 auto EditOrRead=[&](const FString& Path,bool bEdit) {
  TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files,*Path,TEXT("properties.hjson"),true,false); int32 Matches=0;
  for (const FString& File:Files) {
   FString Text; TSharedPtr<FJsonObject> Document;
   if (!FFileHelper::LoadFileToString(Text,*File) || !FUeAgentStructuredHjsonParser::ParseObject(Text,File,Document,Error)) return false;
   const TSharedPtr<FJsonObject>* Categories=nullptr; if (!Document->TryGetObjectField(TEXT("categories"),Categories)) continue;
   for (const auto& Category:(*Categories)->Values) {
    const TSharedPtr<FJsonObject>* Property=nullptr;
    if (!Category.Value->AsObject()->TryGetObjectField(TEXT("SoundSubmixSends"),Property)) continue;
    ++Matches; const TArray<TSharedPtr<FJsonValue>>* Items=nullptr;
    if (!(*Property)->TryGetArrayField(TEXT("value"),Items) || Items->Num()!=(bEdit?1:2)) { Error=TEXT("submix ordinary shape:")+Text.Left(3500); return false; }
    if (bEdit) {
     FString CopyText; TSharedPtr<FJsonObject> Added;
     if (!CommandContract::Json::TryCanonicalize((*Items)[0]->AsObject(),CopyText,Error) || !FUeAgentStructuredHjsonParser::ParseObject(CopyText,File,Added,Error)) return false;
     auto Fields=Added->GetObjectField(TEXT("fields")); Fields->Values.Empty(); Fields->SetNumberField(TEXT("SendLevel"),0.62);
     auto Updated=*Items; Updated.Add(MakeShared<FJsonValueObject>(Added)); (*Property)->SetArrayField(TEXT("value"),Updated);
     if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(),Text,Error) || !FFileHelper::SaveStringToFile(Text,*File,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
    } else if (!FMath::IsNearlyEqual((*Items)[1]->AsObject()->GetObjectField(TEXT("fields"))->GetNumberField(TEXT("SendLevel")),0.62,0.0001)) return false;
   }
  }
  if (Matches!=1) Error=FString::Printf(TEXT("SoundSubmixSends ordinary matches=%d"),Matches); return Matches==1;
 };
 if (!TestTrue(TEXT("public export"),ExportTo(Workspace)) || !TestTrue(TEXT("ordinary append with omitted native defaults"),EditOrRead(Workspace,true))) { AddError(Error); return false; }
 auto Directory=MakeShared<FJsonObject>(); Directory->SetStringField(TEXT("kind"),TEXT("directory")); Directory->SetStringField(TEXT("path"),Workspace);
 auto Args=MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"),ObjectPath); Args->SetObjectField(TEXT("source"),Directory);
 if (!CommandContract::Json::TryCanonicalize(Args,Params,Error)) return false;
 Source=nullptr; Package=nullptr;
 auto Import=SubmitPublicCommand(Registry,Dispatch,TEXT("content_graph.import_workspace"),TEXT("compact-import-")+Token,Params);
 if (!TestTrue(TEXT("public compact root import"),Import.IsOk())) { AddError(Import.GetErrorJson()); return false; }
 if (!TestTrue(TEXT("independent package unload"),T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName,Error))) { AddError(Error); return false; }
 Source=LoadObject<UMetaSoundSource>(nullptr,*ObjectPath);
 if (!TestNotNull(TEXT("independent native reload"),Source)) return false;
 if (!TestEqual(TEXT("native sends count"),Source->SoundSubmixSends.Num(),2)) return false;
 TestTrue(TEXT("complete original send preserved"),FSoundSubmixSendInfo::StaticStruct()->CompareScriptStruct(&Original,&Source->SoundSubmixSends[0],PPF_None));
 FSoundSubmixSendInfo Added; Added.SendLevel=0.62f;
 TestTrue(TEXT("new send uses complete native defaults"),FSoundSubmixSendInfo::StaticStruct()->CompareScriptStruct(&Added,&Source->SoundSubmixSends[1],PPF_None));
 TestTrue(TEXT("neighbor and clean state preserved"),Source->bEnableSubmixSends && Source->Priority==2.5f && !Source->GetOutermost()->IsDirty());
 if (!TestTrue(TEXT("independent public reexport"),ExportTo(Readback)) || !TestTrue(TEXT("ordinary appended send reexport"),EditOrRead(Readback,false))) AddError(Error);
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85MetaSoundFrontendVerticalTest,
	"UAI.Root.T85.MetaSound.FrontendVertical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85MetaSoundFrontendVerticalTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI;
	using namespace UE::UAI::QueryOwners::AudioGraph;
	const FString PackageName = TEXT("/Game/UAI/Transient/T85MetaSoundFrontend/MS_Frontend_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	DeleteAssetIfPresent(ObjectPath);
	ON_SCOPE_EXIT
	{
		DeleteAssetIfPresent(ObjectPath);
		UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};

	UPackage* Package = CreatePackage(*PackageName);
	UMetaSoundSourceFactory* const Factory = NewObject<UMetaSoundSourceFactory>();
	UMetaSoundSource* Source = Package && Factory ? Cast<UMetaSoundSource>(Factory->FactoryCreateNew(
		UMetaSoundSource::StaticClass(), Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional,
		nullptr, GWarn)) : nullptr;
	if (!TestNotNull(TEXT("a persistent MetaSound Source fixture is created"), Source)
		|| !Source) return false;
	TScriptInterface<IMetaSoundDocumentInterface> SourceInterface;
	SourceInterface.SetObject(Source);
	SourceInterface.SetInterface(Cast<IMetaSoundDocumentInterface>(Source));
	EMetaSoundBuilderResult BuilderResult = EMetaSoundBuilderResult::Failed;
	UMetaSoundBuilderBase* Builder = UMetaSoundEditorSubsystem::GetChecked().FindOrBeginBuilding(
		SourceInterface, BuilderResult);
	FMetasoundFrontendLiteral OutputDefault;
	const FMetaSoundBuilderNodeInputHandle OutputHandle = Builder
		? Builder->AddGraphOutputNode(TEXT("AudioOut"), TEXT("Audio"), OutputDefault, BuilderResult)
		: FMetaSoundBuilderNodeInputHandle{};
	if (Builder) Builder->InitNodeLocations();
	if (Builder)
	{
		Builder->ConformObjectToDocument();
		UMetaSoundEditorSubsystem::GetChecked().RegisterGraphWithFrontend(*Source, true);
	}
	const bool bSourceContainsOutput = OutputHandle.IsSet()
		&& SourceInterface->GetConstDocument().RootGraph.GetConstDefaultGraph().Nodes.ContainsByPredicate(
			[&OutputHandle](const FMetasoundFrontendNode& Node)
			{
				return Node.GetID() == OutputHandle.NodeID;
			});
	if (!TestTrue(TEXT("the fixture has a real connectable Audio graph output"),
			Builder && BuilderResult == EMetaSoundBuilderResult::Succeeded
			&& OutputHandle.IsSet() && bSourceContainsOutput)
		|| !TestTrue(TEXT("the initial MetaSound Source fixture is saved"),
			SaveSource(*Package, *Source, Filename))) return false;
	TSharedPtr<FJsonObject> DirectDocument;
	FString DirectDocumentError;
	const bool bDirectDocumentBuilt = UeAgentAudioAuthoring::BuildDocument(
		UeAgentAudioAuthoring::EAssetKind::MetaSound, *Source,
		DirectDocument, DirectDocumentError, PackageName);
	const TSharedPtr<FJsonObject>* DirectNodes = nullptr;
	if (!TestTrue(TEXT("the MetaSound domain document contains the fixture graph"),
			bDirectDocumentBuilt && DirectDocument.IsValid()
			&& DirectDocument->TryGetObjectField(TEXT("nodes"), DirectNodes)
			&& DirectNodes && DirectNodes->IsValid() && !(*DirectNodes)->Values.IsEmpty()))
	{
		AddError(DirectDocumentError);
		return false;
	}
	FAssetRegistryModule::AssetCreated(Source);

	FAudioGraphCandidateQueryResult Candidates;
	FAudioGraphCandidateQueryError CandidateError;
	if (!TestTrue(TEXT("the catalog searches the readable Sine label"),
			TryQueryAudioGraphCandidates(*Source, TEXT("Sine"), Candidates, CandidateError)))
	{
		AddError(CandidateError.Code + TEXT(":") + CandidateError.PublicMessage);
		return false;
	}
	const FAudioGraphCandidate* const Sine = Candidates.Candidates.FindByPredicate(
		[](const FAudioGraphCandidate& Candidate)
		{
			return Candidate.bSupported && Candidate.Name.StartsWith(TEXT("Sine"));
		});
	if (!TestNotNull(TEXT("a supported readable Sine candidate is available"), Sine)) return false;

	TSharedPtr<OperationOwners::UObjectDataAsset::IUeAgentUObjectDataAssetCommandService> Service;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(TEXT("the production public service composes"),
			OperationOwners::UObjectDataAsset::TryCreateProductionUObjectDataAssetCommandService(Service, Error))
		|| !Service.IsValid()
		|| !TestTrue(TEXT("production registration creates exact public dispatch"),
			Module::CommandRegistration::UObjectDataAsset::TryCreateUObjectDataAssetRegistration(
				Service.ToSharedRef(), Registry, Dispatch, Error)))
	{
		AddError(Error);
		return false;
	}

	const FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()
		/ (TEXT("Automation/UAI/T85/MetaSoundExport_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	const FString Readback = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()
		/ (TEXT("Automation/UAI/T85/MetaSoundReadback_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*Workspace, false, true);
		IFileManager::Get().DeleteDirectory(*Readback, false, true);
	};
	auto BuildExportParams = [&ObjectPath, &Error](const FString& Path, FString& Out)
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), ObjectPath);
		Params->SetStringField(TEXT("workspace_path"), Path);
		Params->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		return CommandContract::Json::TryCanonicalize(Params, Out, Error);
	};
	FString Params;
	if (!TestTrue(TEXT("the MetaSound export request canonicalizes"), BuildExportParams(Workspace, Params)))
	{
		AddError(Error);
		return false;
	}
	const CommandEdge::FUeAgentCommandResult Export = SubmitPublicCommand(
		Registry, Dispatch, TEXT("content_graph.export_workspace"), TEXT("req-t85-ms-export"), Params);
	if (!TestTrue(TEXT("the public MetaSound export publishes a workspace"), Export.IsOk())
		|| !TestTrue(TEXT("the model edits readable nodes and connections only"),
			TryEditWorkspace(Workspace, Sine->Name, Error)))
	{
		if (!Export.IsOk()) AddError(Export.GetErrorJson());
		AddError(Error);
		return false;
	}

	const TSharedRef<FJsonObject> Directory = MakeShared<FJsonObject>();
	Directory->SetStringField(TEXT("kind"), TEXT("directory"));
	Directory->SetStringField(TEXT("path"), Workspace);
	const TSharedRef<FJsonObject> ImportObject = MakeShared<FJsonObject>();
	ImportObject->SetStringField(TEXT("target"), ObjectPath);
	ImportObject->SetObjectField(TEXT("source"), Directory);
	if (!TestTrue(TEXT("the edited workspace import request canonicalizes"),
			CommandContract::Json::TryCanonicalize(ImportObject, Params, Error)))
	{
		AddError(Error);
		return false;
	}
	const CommandEdge::FUeAgentCommandResult Import = SubmitPublicCommand(
		Registry, Dispatch, TEXT("content_graph.import_workspace"), TEXT("req-t85-ms-import"), Params);
	if (!TestTrue(TEXT("the public import runs the production persistence coordinator"), Import.IsOk()))
	{
		AddError(Import.GetErrorJson());
		return false;
	}
	TSharedPtr<FJsonObject> Payload;
	FString PayloadError;
	TestTrue(TEXT("the coordinated import exposes compact success"),
		CommandContract::Json::TryParseObject(Import.GetPayloadJson(), Payload, PayloadError)
		&& Payload.IsValid() && Payload->Values.Num() == 1
		&& Payload->GetStringField(TEXT("status")) == TEXT("succeeded"));

	SourceInterface.SetObject(nullptr);
	SourceInterface.SetInterface(nullptr);
	Builder = nullptr;
	Source = nullptr;
	Package = nullptr;
	if (!TestTrue(TEXT("the persisted MetaSound package unloads before readback"),
			UE::UAI::T85::FixtureLifecycle::TryUnloadPackageForReadback(
				PackageName, Error)))
	{
		AddError(Error);
		return false;
	}
	if (!TestNull(TEXT("the imported MetaSound package is no longer resident"),
			FindPackage(nullptr, *PackageName)))
	{
		return false;
	}
	UMetaSoundSource* const Reloaded = LoadObject<UMetaSoundSource>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("the persisted MetaSound independently reloads from disk"),
			Reloaded))
	{
		return false;
	}

	if (!TestTrue(TEXT("the independent re-export request canonicalizes"), BuildExportParams(Readback, Params)))
	{
		AddError(Error);
		return false;
	}
	const CommandEdge::FUeAgentCommandResult ReExport = SubmitPublicCommand(
		Registry, Dispatch, TEXT("content_graph.export_workspace"), TEXT("req-t85-ms-readback"), Params);
	if (!TestTrue(TEXT("an independent public re-export reads the persisted graph"), ReExport.IsOk())
		|| !TestTrue(TEXT("re-export preserves readable Sine and connection semantics"),
			TryReadbackWorkspace(Readback, Sine->Name, Error)))
	{
		if (!ReExport.IsOk()) AddError(ReExport.GetErrorJson());
		AddError(Error);
		return false;
	}

	// The same production apply seam must restore dirty memory independently of
	// the older disk image when the final readback rejects the persisted result.
	TArray<uint8> DiskBefore;
	if (!TestTrue(TEXT("the original disk image is readable"),
		FFileHelper::LoadFileToArray(DiskBefore, *Filename))) return false;
	FFloatProperty* VolumeProperty = FindFProperty<FFloatProperty>(Reloaded->GetClass(), TEXT("Volume"));
	if (!TestNotNull(TEXT("the fixture exposes a scalar volume property"), VolumeProperty)) return false;
	VolumeProperty->SetPropertyValue_InContainer(Reloaded, 0.35f);
	Reloaded->MarkPackageDirty();
	UeAgentAudioAuthoring::FAssetApplyRequest FailedRequest;
	FailedRequest.AssetPath = PackageName;
	FailedRequest.bSave = true;
	TSharedRef<FJsonObject> VolumeEdit = MakeShared<FJsonObject>();
	VolumeEdit->SetStringField(TEXT("property_path"), TEXT("Volume"));
	VolumeEdit->SetNumberField(TEXT("value"), 0.75);
	FailedRequest.Properties.Add(MakeShared<FJsonValueObject>(VolumeEdit));
	bool bReachedFinalReadback = false;
	FailedRequest.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
	{
		UMetaSoundSource* Actual = LoadObject<UMetaSoundSource>(nullptr, *ObjectPath);
		bReachedFinalReadback = Actual && FMath::IsNearlyEqual(
			VolumeProperty->GetPropertyValue_InContainer(Actual), 0.75f);
		Failure.Diagnostic.Code = TEXT("t85_final_readback_rejected");
		Failure.Diagnostic.PublicMessage = TEXT("intentional final acceptance failure after disk reload");
		return false;
	};
	UeAgentAudioAuthoring::FAssetApplyResult FailedResult;
	TestFalse(TEXT("a final persisted-readback rejection fails the same transaction"),
		UeAgentAudioAuthoring::ApplyAsset(FailedRequest, FailedResult,
			TEXT("t85-final-readback-") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	TestTrue(TEXT("the injected rejection ran after real persisted reload"), bReachedFinalReadback);
	TestTrue(TEXT("late failure reports semantically confirmed recovery"),
		FailedResult.bRollbackAttempted && FailedResult.bRollbackSucceeded);
	if (!bReachedFinalReadback || !FailedResult.bRollbackSucceeded)
		AddError(FailedResult.Phase + TEXT(":") + FailedResult.Diagnostic.Code + TEXT(":") + FailedResult.Diagnostic.InternalContext);
	UMetaSoundSource* Restored = LoadObject<UMetaSoundSource>(nullptr, *ObjectPath);
	TestTrue(TEXT("the restored memory retains the dirty before value"), Restored &&
		FMath::IsNearlyEqual(VolumeProperty->GetPropertyValue_InContainer(Restored), 0.35f));
	TestTrue(TEXT("the restored memory retains the before dirty flag"),
		Restored && Restored->GetOutermost()->IsDirty());
	TArray<uint8> DiskAfter;
	TestTrue(TEXT("the original disk bytes survive independently of dirty memory"),
		FFileHelper::LoadFileToArray(DiskAfter, *Filename) && DiskBefore == DiskAfter);

	DeleteAssetIfPresent(ObjectPath);
	UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	TestFalse(TEXT("the task-owned MetaSound fixture leaves no asset file"),
		IFileManager::Get().FileExists(*Filename));
	return !HasAnyErrors();
}
