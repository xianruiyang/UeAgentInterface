#include "Misc/AutomationTest.h"
#include "Core/Contract/UeAgentDiagnostic.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringApplyProvider.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringRegistration.h"
#include "Domains/Material/Authoring/UeAgentMaterialInstanceAuthoring.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "EdGraph/EdGraphPin.h"
#include "HAL/FileManager.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialEditingLibrary.h"
#include "ShaderCompiler.h"
#include "MaterialGraph/MaterialGraphNode_Root.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "PackageTools.h"
#include "UObject/UnrealType.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Engine/Texture.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Module/CommandRegistration/UObjectDataAsset/UeAgentUObjectDataAssetRegistration.h"
#include "ObjectTools.h"
#include "OperationOwners/UObjectDataAsset/UeAgentUObjectDataAssetCommandService.h"
#include "QueryOwners/Material/UeAgentMaterialExpressionCandidateQuery.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/GarbageCollection.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	using namespace UE::UAI;

	constexpr const TCHAR* MaterialType = TEXT("/Script/Engine.Material");

	class FNeverCancelled final
		: public Contract::IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override { return false; }
	};

	CommandEdge::FUeAgentCommandResult SubmitPublicImport(
		const TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry>&
			Registry,
		const TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>& Dispatch,
		const FString& ParamsJson)
	{
		const FString CommandId = TEXT("content_graph.import_workspace");
		const auto* Contract = Registry.IsValid()
			? Registry->FindExact(CommandId)
			: nullptr;
		if (!Contract || !Dispatch.IsValid())
		{
			return CommandEdge::FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t85-material"), TEXT("req-t85-material"), CommandId,
				TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
				TEXT("material_public_import_contract_missing"),
				TEXT("the exact public import contract is unavailable"));
		}
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"corr-t85-material\",")
			TEXT("\"request_id\":\"req-t85-material\",")
			TEXT("\"command_id\":\"%s\",")
			TEXT("\"command_binding_hash\":\"%s\",\"params\":%s}"),
			*CommandId, *Contract->GetContractHash(), *ParamsJson);
		FTCHARToUTF8 Encoded(*Envelope);
		TArray<uint8> Bytes;
		Bytes.Append(
			reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
		CommandEdge::FUeAgentCommandProtocolDiagnostic Diagnostic;
		TOptional<CommandEdge::FUeAgentCommandRequest> Request;
		if (!CommandEdge::FUeAgentCommandProtocolCodec().DecodeRequest(
				Bytes, Request, Diagnostic)
			|| !Request.IsSet())
		{
			return CommandEdge::FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t85-material"), TEXT("req-t85-material"), CommandId,
				Contract->GetContractHash(),
				TEXT("material_public_import_request_invalid"),
				TEXT("the exact public request could not be decoded"));
		}
		return Dispatch->Submit(
			Request.GetValue(),
			Contract::FUeAgentCallerContext(
				TEXT("t85-material"),
				{TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")},
				TEXT("corr-t85-material")),
			MakeShared<FNeverCancelled>());
	}

	CommandEdge::FUeAgentCommandResult SubmitPublicExport(
		const TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>& Dispatch,
		const FString& ParamsJson)
	{
		const FString CommandId = TEXT("content_graph.export_workspace");
		const auto* Contract = Registry.IsValid() ? Registry->FindExact(CommandId) : nullptr;
		if (!Contract || !Dispatch.IsValid())
			return CommandEdge::FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t85-material"), TEXT("req-t85-material"), CommandId,
				TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
				TEXT("material_public_export_contract_missing"), TEXT("export contract unavailable"));
			const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,\"message_kind\":\"command_request\",\"correlation_id\":\"corr-t85-material\",\"request_id\":\"req-t85-material-export\",\"command_id\":\"%s\",\"command_binding_hash\":\"%s\",\"params\":%s}"),
			*CommandId, *Contract->GetContractHash(), *ParamsJson);
		FTCHARToUTF8 Encoded(*Envelope);
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
		CommandEdge::FUeAgentCommandProtocolDiagnostic Diagnostic;
		TOptional<CommandEdge::FUeAgentCommandRequest> Request;
		if (!CommandEdge::FUeAgentCommandProtocolCodec().DecodeRequest(Bytes, Request, Diagnostic) || !Request.IsSet())
			return CommandEdge::FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t85-material"), TEXT("req-t85-material-export"), CommandId,
				Contract->GetContractHash(), TEXT("material_public_export_request_invalid"), TEXT("request decode failed"));
		return Dispatch->Submit(Request.GetValue(), Contract::FUeAgentCallerContext(
			TEXT("t85-material"), {TEXT("uai:query"), TEXT("uai:export")}, TEXT("corr-t85-material")),
			MakeShared<FNeverCancelled>());
	}

	CommandEdge::FUeAgentCommandResult SubmitPublicMaterialCandidateQuery(
		const TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>& Dispatch,
		const FString& ParamsJson)
	{
		const FString CommandId = TEXT("material.expression.candidates");
		const auto* Contract = Registry.IsValid() ? Registry->FindExact(CommandId) : nullptr;
		if (!Contract || !Dispatch.IsValid())
			return CommandEdge::FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t85-material-candidate"), TEXT("req-t85-material-candidate"), CommandId,
				TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
				TEXT("material_public_candidate_contract_missing"), TEXT("candidate contract unavailable"));
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,\"message_kind\":\"command_request\",\"correlation_id\":\"corr-t85-material-candidate\",\"request_id\":\"req-t85-material-candidate\",\"command_id\":\"%s\",\"command_binding_hash\":\"%s\",\"params\":%s}"),
			*CommandId, *Contract->GetContractHash(), *ParamsJson);
		FTCHARToUTF8 Encoded(*Envelope);
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
		CommandEdge::FUeAgentCommandProtocolDiagnostic Diagnostic;
		TOptional<CommandEdge::FUeAgentCommandRequest> Request;
		if (!CommandEdge::FUeAgentCommandProtocolCodec().DecodeRequest(Bytes, Request, Diagnostic) || !Request.IsSet())
			return CommandEdge::FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t85-material-candidate"), TEXT("req-t85-material-candidate"), CommandId,
				Contract->GetContractHash(), TEXT("material_public_candidate_request_invalid"), TEXT("request decode failed"));
		return Dispatch->Submit(Request.GetValue(), Contract::FUeAgentCallerContext(
			TEXT("t85-material"), {TEXT("uai:query")}, TEXT("corr-t85-material-candidate")),
			MakeShared<FNeverCancelled>());
	}
	void DeleteAssetIfPresent(const FString& ObjectPath)
	{
		if (UObject* Existing = StaticLoadObject(
			UObject::StaticClass(), nullptr, *ObjectPath))
		{
			ObjectTools::DeleteObjectsUnchecked({Existing});
		}
	}

	bool SaveMaterial(
		UPackage& Package,
		UMaterial& Material,
		const FString& Filename)
	{
		Material.PostEditChange();
		Package.MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		const bool bSaved = UPackage::SavePackage(
			&Package, &Material, *Filename, Args);
		if (bSaved)
		{
			Package.SetDirtyFlag(false);
		}
		return bSaved;
	}

	UEdGraphPin* FindMaterialPropertyPin(
		UMaterialGraph& Graph,
		UMaterialGraphNode_Root& Root,
		const EMaterialProperty Property)
	{
		for (UEdGraphPin* Pin : Root.Pins)
		{
			if (Pin && Graph.MaterialInputs.IsValidIndex(Pin->SourceIndex)
				&& Graph.MaterialInputs[Pin->SourceIndex].GetProperty()
					== Property)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	bool TryEditVisibleMaterialGraph(const FString& WorkspacePath, FString& OutError)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *WorkspacePath, TEXT("*.hjson"), true, false);
		for (const FString& File : Files)
		{
			if (File.EndsWith(TEXT(".interfaces.hjson"))) continue;
			FString Text;
			TSharedPtr<FJsonObject> Document;
			if (!FFileHelper::LoadFileToString(Text, *File)
				|| !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, OutError)
				|| !Document.IsValid()) continue;
			const TSharedPtr<FJsonObject>* Nodes = nullptr;
			if (!Document->TryGetObjectField(TEXT("nodes"), Nodes) || !Nodes || !Nodes->IsValid()) continue;
			FString RootAlias;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& NodePair : (*Nodes)->Values)
			{
				if (!NodePair.Value.IsValid() || NodePair.Value->Type != EJson::Object) continue;
				FString NodeName;
				NodePair.Value->AsObject()->TryGetStringField(TEXT("name"), NodeName);
				if (NodeName.Equals(TEXT("MaterialOutput"), ESearchCase::IgnoreCase))
				{
					RootAlias = NodePair.Key;
					break;
				}
			}
			if (RootAlias.IsEmpty())
			{
				OutError = TEXT("material_visible_graph_root_alias_not_found");
				return false;
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Nodes)->Values)
			{
				if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Object) continue;
				const TSharedPtr<FJsonObject> Node = Pair.Value->AsObject();
				FString Name;
				Node->TryGetStringField(TEXT("name"), Name);
				if (!Name.Equals(TEXT("Multiply"), ESearchCase::IgnoreCase)) continue;
				return false;
			}
			TSharedRef<FJsonObject> Multiply = MakeShared<FJsonObject>();
			Multiply->SetStringField(TEXT("name"), TEXT("Multiply"));
			TSharedRef<FJsonObject> Candidate = MakeShared<FJsonObject>();
			Candidate->SetStringField(TEXT("name"), TEXT("Multiply"));
			Multiply->SetObjectField(TEXT("candidate"), Candidate);
			(*Nodes)->SetObjectField(TEXT("Multiply"), Multiply);
			const TArray<TSharedPtr<FJsonValue>>* Connections = nullptr;
			if (!Document->TryGetArrayField(TEXT("connections"), Connections) || !Connections)
			{
				OutError = TEXT("material_visible_graph_connections_missing");
				return false;
			}
			TArray<TSharedPtr<FJsonValue>> UpdatedConnections = *Connections;
			TSharedRef<FJsonObject> Connection = MakeShared<FJsonObject>();
			Connection->SetStringField(TEXT("from"), TEXT("Multiply.Output"));
			Connection->SetStringField(TEXT("to"), RootAlias + TEXT(".BaseColor"));
			UpdatedConnections.Add(MakeShared<FJsonValueObject>(Connection));
			Document->SetArrayField(TEXT("connections"), MoveTemp(UpdatedConnections));
				FString Updated;
				if (!CommandContract::Json::TryCanonicalize(Document, Updated, OutError)
					|| !FFileHelper::SaveStringToFile(Updated, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
				{
					OutError = TEXT("material_visible_graph_write_failed");
					return false;
				}
				return true;
		}
		OutError = TEXT("material_visible_graph_node_not_found");
		return false;
	}

	bool HasVisibleMaterialGraphResult(
		const FString& WorkspacePath,
		FString& OutError)
	{
		OutError = TEXT("material_readback_graph_not_found");
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *WorkspacePath, TEXT("*.hjson"), true, false);
		for (const FString& File : Files)
		{
			if (File.EndsWith(TEXT(".interfaces.hjson"))) continue;
			FString Text;
			TSharedPtr<FJsonObject> Document;
			FString Error;
			if (!FFileHelper::LoadFileToString(Text, *File)
				|| !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)
				|| !Document.IsValid()) continue;
			const TSharedPtr<FJsonObject>* Nodes = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Connections = nullptr;
			if (!Document->TryGetObjectField(TEXT("nodes"), Nodes) || !Nodes
				|| !Document->TryGetArrayField(TEXT("connections"), Connections) || !Connections) continue;
			FString MultiplyAlias;
			FString RootAlias;
			TArray<FString> ReadableNodeNames;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& NodePair : (*Nodes)->Values)
			{
				if (!NodePair.Value.IsValid() || NodePair.Value->Type != EJson::Object) continue;
				FString NodeName;
				NodePair.Value->AsObject()->TryGetStringField(TEXT("name"), NodeName);
				if (!NodeName.IsEmpty() && ReadableNodeNames.Num() < 8)
				{
					ReadableNodeNames.Add(NodeName.Left(48));
				}
				if (NodeName.Equals(TEXT("MaterialOutput"), ESearchCase::IgnoreCase))
				{
					RootAlias = NodePair.Key;
				}
				else if (NodeName.Equals(TEXT("Multiply"), ESearchCase::IgnoreCase))
				{
					MultiplyAlias = NodePair.Key;
				}
			}
			if (RootAlias.IsEmpty() || MultiplyAlias.IsEmpty())
			{
				OutError = FString::Printf(
					TEXT("material_readback_nodes_missing:root=%s multiply=%s names=%s"),
					RootAlias.IsEmpty() ? TEXT("false") : TEXT("true"),
					MultiplyAlias.IsEmpty() ? TEXT("false") : TEXT("true"),
					*FString::Join(ReadableNodeNames, TEXT(",")));
				continue;
			}
			for (const TSharedPtr<FJsonValue>& Value : *Connections)
			{
				const TSharedPtr<FJsonObject> Connection = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
				FString From, To;
				if (Connection.IsValid() && Connection->TryGetStringField(TEXT("from"), From)
					&& Connection->TryGetStringField(TEXT("to"), To)
					&& From == MultiplyAlias + TEXT(".Output") && To == RootAlias + TEXT(".BaseColor")) return true;
			}
			OutError = FString::Printf(
				TEXT("material_readback_connection_missing:connections=%d"),
				Connections->Num());
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85MaterialPropertyRecoveryTest,
 "UAI.Root.T85.Material.PropertyRecovery", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85MaterialPropertyRecoveryTest::RunTest(const FString& Parameters)
{
 using namespace UeAgentAuthoringOperationApply;
 using namespace UeAgentMaterialAuthoring;
 UE::UAI::Contract::FDiagnostic RegistrationError;
 if (!TestTrue(TEXT("Material recovery authoring registration"),RegisterAuthoring(RegistrationError))) { AddError(RegistrationError.ToPublicDetail()); return false; }
 const FString Token=FGuid::NewGuid().ToString(EGuidFormats::Digits);
 const FString PackageName=TEXT("/Game/UAI/Transient/T85MaterialRecovery/M_")+Token;
 const FString ObjectPath=PackageName+TEXT(".")+FPackageName::GetLongPackageAssetName(PackageName);
 const FString Filename=FPackageName::LongPackageNameToFilename(PackageName,FPackageName::GetAssetPackageExtension());
 ON_SCOPE_EXIT {
  if (UObject* Owned=LoadObject<UObject>(nullptr,*ObjectPath)) TestEqual(TEXT("owned recovery cleanup"),ObjectTools::ForceDeleteObjects({Owned},false),1);
  TestFalse(TEXT("owned recovery disk absent"),IFileManager::Get().FileExists(*Filename));
 };
 UPackage* Package=CreatePackage(*PackageName);
 UMaterial* Material=NewObject<UMaterial>(Package,*FPackageName::GetLongPackageAssetName(PackageName),RF_Public|RF_Standalone|RF_Transactional);
 auto* Custom=Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(Material,UMaterialExpressionCustom::StaticClass(),123,234));
 auto* Constant=Cast<UMaterialExpressionConstant>(UMaterialEditingLibrary::CreateMaterialExpression(Material,UMaterialExpressionConstant::StaticClass(),432,543));
 if (!Custom || !Constant) return false;
 Custom->Code=TEXT("return 0.25;"); Custom->Inputs.Reset();
 auto& Input=Custom->Inputs.AddDefaulted_GetRef(); Input.InputName=TEXT("NamedInput"); Input.Input.InputName=TEXT("NativeInputIdentity");
 Constant->R=0.5f;
 if (!TestTrue(TEXT("recovery seed input connection"),UMaterialEditingLibrary::ConnectMaterialExpressions(Constant,FString(),Custom,TEXT("NamedInput")))
     || !TestTrue(TEXT("recovery seed root connection"),UMaterialEditingLibrary::ConnectMaterialProperty(Constant,FString(),MP_Roughness))) return false;
 FAssetRegistryModule::AssetCreated(Material);
 if (!TestTrue(TEXT("save recovery disk baseline"),SaveMaterial(*Package,*Material,Filename))) return false;
 TArray<uint8> DiskBefore; if (!FFileHelper::LoadFileToArray(DiskBefore,*Filename)) return false;
 auto* Removed=Cast<UMaterialExpressionConstant>(UMaterialEditingLibrary::CreateMaterialExpression(Material,UMaterialExpressionConstant::StaticClass(),765,876));
 if (!Removed) return false;
 Removed->R=0.9f; Constant->R=0.625f; Material->TwoSided=true;
 Custom->Description=TEXT("dirty custom description");
 FCustomDefine Define; Define.DefineName=TEXT("DIRTY"); Define.DefineValue=TEXT("17"); Custom->AdditionalDefines={Define};
 Material->MarkPackageDirty();
 const FGuid CustomGuid=Custom->MaterialExpressionGuid, ConstantGuid=Constant->MaterialExpressionGuid, RemovedGuid=Removed->MaterialExpressionGuid;
 auto FullState=[](UObject* Object) {
  TMap<FName,FString> Values;
  for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It) {
   if (It->HasAnyPropertyFlags(CPF_Transient|CPF_Deprecated)) continue;
   FString Value; It->ExportTextItem_Direct(Value,It->ContainerPtrToValuePtr<void>(Object),nullptr,Object,PPF_Delimited);
   Values.Add(It->GetFName(),MoveTemp(Value));
  }
  return Values;
 };
 TMap<FGuid,TMap<FName,FString>> Before;
 for (UMaterialExpression* Expression:Material->GetExpressions()) Before.Add(Expression->MaterialExpressionGuid,FullState(Expression));
 FAssetApplyRequest Request; Request.AssetPath=PackageName; Request.AssetKind=TEXT("material"); Request.bSave=true;
 Request.GraphContext.Policy=EGraphApplyPolicy::PreserveExisting;
 Request.GraphContext.ExpressionsByAlias.Add(TEXT("custom"),{CustomGuid,Custom->GetClass()->GetPathName(),{}});
 Request.GraphContext.ExpressionsByAlias.Add(TEXT("constant"),{ConstantGuid,Constant->GetClass()->GetPathName(),{}});
 auto Property=[](const TCHAR* Path,TSharedPtr<FJsonValue> Value) {
  auto Descriptor=MakeShared<FJsonObject>(); Descriptor->SetStringField(TEXT("property_path"),Path); Descriptor->SetField(TEXT("value"),Value);
  return MakeShared<FJsonValueObject>(Descriptor);
 };
 const auto Nodes=MakeShared<FJsonObject>();
 auto Node=[&](const TCHAR* Alias,const TCHAR* Name,int32 X,int32 Y,TArray<TSharedPtr<FJsonValue>> Properties) {
  auto Value=MakeShared<FJsonObject>(); Value->SetStringField(TEXT("name"),Name);
  auto Pos=MakeShared<FJsonObject>(); Pos->SetNumberField(TEXT("x"),X); Pos->SetNumberField(TEXT("y"),Y); Value->SetObjectField(TEXT("pos"),Pos);
  Value->SetArrayField(TEXT("properties"),MoveTemp(Properties)); Nodes->SetObjectField(Alias,Value); return Value;
 };
 auto CustomNode=Node(TEXT("custom"),TEXT("MaterialExpressionCustom"),123,234,{Property(TEXT("Description"),MakeShared<FJsonValueString>(TEXT("changed description")))});
 CustomNode->SetArrayField(TEXT("custom_inputs"),{MakeShared<FJsonValueString>(TEXT("NamedInput"))});
 Node(TEXT("constant"),TEXT("MaterialExpressionConstant"),432,543,{Property(TEXT("R"),MakeShared<FJsonValueNumber>(0.75))});
 auto RootNode=MakeShared<FJsonObject>(); RootNode->SetStringField(TEXT("name"),TEXT("MaterialOutput")); Nodes->SetObjectField(TEXT("output"),RootNode);
 auto Connection=[](const TCHAR* From,const TCHAR* To) { auto Value=MakeShared<FJsonObject>(); Value->SetStringField(TEXT("from"),From); Value->SetStringField(TEXT("to"),To); return MakeShared<FJsonValueObject>(Value); };
 Request.Graph=MakeShared<FJsonObject>(); Request.Graph->SetObjectField(TEXT("nodes"),Nodes);
 Request.Graph->SetArrayField(TEXT("connections"),{Connection(TEXT("constant.Output"),TEXT("custom.NamedInput")),Connection(TEXT("constant.Output"),TEXT("output.Roughness"))});
 Request.Properties={Property(TEXT("TwoSided"),MakeShared<FJsonValueBoolean>(false))};
 class FRejectMaterialStage final : public IDurablePackageStager {
 public:
  TWeakObjectPtr<UMaterial> Original;
  TWeakObjectPtr<UMaterialExpression> Retained;
  FGuid RemovedGuid, ConstantGuid;
  FString Filename; TArray<uint8> Disk;
  bool bObserved=false;
  TSharedRef<IDurablePackageStager> Inner=CreateDefaultDurablePackageStager();
  bool BuildArtifactInventory(const TArray<FPackageDurablePlan>& Plans,TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>& Artifacts,FOperationFailure& Failure) override { return Inner->BuildArtifactInventory(Plans,Artifacts,Failure); }
  bool StagePackages(const TArray<FPackageDurablePlan>& Plans,TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>& Artifacts,FOperationFailure& Failure) override {
   if (!Inner->StagePackages(Plans,Artifacts,Failure)) return false;
   UMaterial* Actual=Original.Get(); if (!Actual) { Failure.Diagnostic.Code=TEXT("material_early_target_released"); return false; }
   for (UMaterialExpression* Expression:Actual->GetExpressions()) if (Expression->MaterialExpressionGuid==RemovedGuid) return true;
   UMaterialExpressionConstant* Constant=nullptr;
   for (UMaterialExpression* Expression:Actual->GetExpressions()) if (Expression->MaterialExpressionGuid==ConstantGuid) Constant=Cast<UMaterialExpressionConstant>(Expression);
   TArray<uint8> Bytes;
   bObserved=Actual->GetExpressions().Num()==2 && Retained.IsValid() && Actual->GetExpressions().Contains(Retained.Get())
    && Constant && FMath::IsNearlyEqual(Constant->R,0.75f) && !Actual->TwoSided && Actual->GetOutermost()->IsDirty()
    && FFileHelper::LoadFileToArray(Bytes,*Filename) && Bytes==Disk;
   bool Any=false;
   for (const auto& Artifact:Artifacts) { bObserved &= IFileManager::Get().FileExists(*Artifact.StagingPath)==Artifact.bStagedExists; Any |= Artifact.bStagedExists; }
   bObserved &= Any;
   Failure.Diagnostic.Code=TEXT("t85_material_early_stage_rejected"); Failure.Diagnostic.PublicMessage=TEXT("real target staged after retained mutation and node removal, before release"); return false;
  }
 };
 auto Stager=MakeShared<FRejectMaterialStage>(); Stager->Original=Material; Stager->Retained=Constant;
 Stager->RemovedGuid=RemovedGuid; Stager->ConstantGuid=ConstantGuid; Stager->Filename=Filename; Stager->Disk=DiskBefore;
 Request.PackageStager=Stager;
 Material=nullptr; Custom=nullptr; Constant=nullptr; Removed=nullptr; Package=nullptr;
 FAssetApplyResult Result;
 TestFalse(TEXT("early semantic transaction rejects"),ApplyAsset(Request,Result,TEXT("material-semantic-early-")+Token));
 AddInfo(TEXT("Material early rejection:")+Result.Phase+TEXT(":")+Result.Diagnostic.Code);
 if (!TestTrue(TEXT("early target mutation observed before release"),Stager->bObserved)
     || !TestTrue(TEXT("early checkpoint recovery confirmed"),Result.bRollbackAttempted&&Result.bRollbackSucceeded)) {
  AddError(Result.Phase+TEXT(":")+Result.Diagnostic.Code+TEXT(":")+Result.Diagnostic.PublicMessage); return false;
 }
 auto CheckRestored = [&]() {
 Material=LoadObject<UMaterial>(nullptr,*ObjectPath);
 if (!TestNotNull(TEXT("restored Material"),Material)) return false;
 TestEqual(TEXT("all original dirty nodes restored"),Material->GetExpressions().Num(),Before.Num());
 for (UMaterialExpression* Expression:Material->GetExpressions()) {
  const auto* Original=Before.Find(Expression->MaterialExpressionGuid);
  if (!TestNotNull(TEXT("restored expression identity"),Original)) continue;
  const auto Current=FullState(Expression); TestEqual(TEXT("persistent property inventory"),Current.Num(),Original->Num());
  for (const auto& Pair:*Original) { const FString* Value=Current.Find(Pair.Key); if (!Value || !Value->Equals(Pair.Value,ESearchCase::CaseSensitive)) AddError(TEXT("restored native difference:")+Pair.Key.ToString()); }
 }
 TestTrue(TEXT("dirty root value and flag restored"),Material->TwoSided && Material->GetOutermost()->IsDirty());
 const FExpressionInput* RestoredRoot=Material->GetExpressionInputForProperty(MP_Roughness);
 TestTrue(TEXT("root graph connection restored"),RestoredRoot && RestoredRoot->Expression && RestoredRoot->Expression->MaterialExpressionGuid==ConstantGuid);
 TArray<uint8> DiskAfter; TestTrue(TEXT("original disk independently restored"),FFileHelper::LoadFileToArray(DiskAfter,*Filename)&&DiskAfter==DiskBefore);
 return !HasAnyErrors();
 };
 if (!CheckRestored()) return false;
 TWeakObjectPtr<UMaterial> BeforeLate=Material;
 Request.PackageStager.Reset();
 bool bLateObserved=false;
 Request.FinalReadback=[&](FOperationFailure& Failure) {
  UMaterial* Published=LoadObject<UMaterial>(nullptr,*ObjectPath);
  UMaterialExpressionCustom* PublishedCustom=nullptr;
  UMaterialExpressionConstant* PublishedConstant=nullptr;
  if (Published) for (UMaterialExpression* Expression:Published->GetExpressions()) {
   if (Expression->MaterialExpressionGuid==CustomGuid) PublishedCustom=Cast<UMaterialExpressionCustom>(Expression);
   if (Expression->MaterialExpressionGuid==ConstantGuid) PublishedConstant=Cast<UMaterialExpressionConstant>(Expression);
  }
  TArray<uint8> Bytes;
  const FExpressionInput* Root=Published?Published->GetExpressionInputForProperty(MP_Roughness):nullptr;
  bLateObserved=Published && !BeforeLate.IsValid() && Published->GetExpressions().Num()==2 && !Published->GetOutermost()->IsDirty()
   && !Published->TwoSided && PublishedCustom && PublishedConstant && FMath::IsNearlyEqual(PublishedConstant->R,0.75f)
   && PublishedCustom->Description.Equals(TEXT("changed description"),ESearchCase::CaseSensitive)
   && PublishedCustom->Inputs.Num()==1 && PublishedCustom->Inputs[0].Input.Expression==PublishedConstant
   && Root && Root->Expression==PublishedConstant
   && FFileHelper::LoadFileToArray(Bytes,*Filename) && Bytes!=DiskBefore
   && FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid();
  Failure.Diagnostic.Code=TEXT("t85_material_late_readback_rejected"); Failure.Diagnostic.PublicMessage=TEXT("clean published retained graph and deleted member observed before rejection"); return false;
 };
 Material=nullptr;
 FAssetApplyResult LateResult;
 TestFalse(TEXT("late semantic transaction rejects"),ApplyAsset(Request,LateResult,TEXT("material-semantic-late-")+Token));
 AddInfo(TEXT("Material late rejection:")+LateResult.Phase+TEXT(":")+LateResult.Diagnostic.Code);
 TestEqual(TEXT("late callback diagnostic retained"),LateResult.Diagnostic.Code,FString(TEXT("t85_material_late_readback_rejected")));
 if (!TestTrue(TEXT("late clean persisted mutation observed"),bLateObserved)
     || !TestTrue(TEXT("late checkpoint recovery confirmed"),LateResult.bRollbackAttempted&&LateResult.bRollbackSucceeded)) {
  AddError(LateResult.Phase+TEXT(":")+LateResult.Diagnostic.Code+TEXT(":")+LateResult.Diagnostic.PublicMessage); return false;
 }
 return CheckRestored();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85MaterialPropertyWorkspaceTest,
 "UAI.Root.T85.Material.PropertyWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85MaterialPropertyWorkspaceTest::RunTest(const FString& Parameters)
{
 const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
 const FString PackageName = TEXT("/Game/UAI/Transient/T85MaterialProperties/M_") + Token;
 const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageName);
 const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
 const FString Workspace = FPaths::ProjectSavedDir() / TEXT("Automation/UAI/T85/MaterialProperties") / Token;
 const FString Readback = Workspace + TEXT("-readback");
 ON_SCOPE_EXIT {
  if (UObject* Owned = LoadObject<UObject>(nullptr, *ObjectPath))
   TestEqual(TEXT("owned Material UE cleanup"), ObjectTools::ForceDeleteObjects({Owned}, false), 1);
  TestFalse(TEXT("owned Material disk absent"), IFileManager::Get().FileExists(*Filename));
  IFileManager::Get().DeleteDirectory(*Workspace, false, true);
  IFileManager::Get().DeleteDirectory(*Readback, false, true);
 };
 UPackage* Package = CreatePackage(*PackageName);
 UMaterial* Material = NewObject<UMaterial>(Package, *FPackageName::GetLongPackageAssetName(PackageName), RF_Public | RF_Standalone | RF_Transactional);
 auto* Custom = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCustom::StaticClass(), 123, 234));
 auto* Constant = Cast<UMaterialExpressionConstant>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionConstant::StaticClass(), 432, 543));
 if (!TestNotNull(TEXT("native custom expression"), Custom) || !TestNotNull(TEXT("native neighbor"), Constant)) return false;
 Custom->Code = TEXT("return 0.25;"); Custom->Description = TEXT("preserve custom state");
 Custom->Inputs.Reset();
 FCustomInput& PreservedInput = Custom->Inputs.AddDefaulted_GetRef();
 PreservedInput.InputName = TEXT("NamedInput"); PreservedInput.Input.InputName = TEXT("NativeInputIdentity");
 FCustomDefine Original; Original.DefineName = TEXT("ORIGINAL"); Original.DefineValue = TEXT("7");
 Custom->AdditionalDefines = {Original}; Constant->R = 0.625f;
 if (!TestTrue(TEXT("seed custom connection accepted"), UMaterialEditingLibrary::ConnectMaterialExpressions(Constant, FString(), Custom, TEXT("NamedInput")))
     || !TestTrue(TEXT("seed custom input points to neighbor"), Custom->Inputs[0].Input.Expression == Constant)) return false;
 Material->TwoSided = false;
 if (!TestTrue(TEXT("seed root connection accepted"), UMaterialEditingLibrary::ConnectMaterialProperty(Constant, FString(), MP_Roughness))) return false;
 FAssetRegistryModule::AssetCreated(Material);
 if (!TestTrue(TEXT("save native seed"), SaveMaterial(*Package, *Material, Filename))) return false;
 const FGuid CustomGuid = Custom->MaterialExpressionGuid, ConstantGuid = Constant->MaterialExpressionGuid;
 auto FullState = [](UObject* Object) {
  TMap<FName,FString> Values;
  for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It) {
   if (It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated) || It->GetFName() == TEXT("AdditionalDefines")) continue;
   FString Value; It->ExportTextItem_Direct(Value, It->ContainerPtrToValuePtr<void>(Object), nullptr, Object, PPF_Delimited);
   Values.Add(It->GetFName(), MoveTemp(Value));
  }
  return Values;
 };
 const auto OriginalCustom = FullState(Custom), OriginalConstant = FullState(Constant);
 TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
 TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
 FString Error, Params;
 if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
 auto ExportTo = [&](const FString& Path) {
  const auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath);
  Args->SetStringField(TEXT("workspace_path"), Path); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
  if (!CommandContract::Json::TryCanonicalize(Args, Params, Error)) return false;
  const auto Result = SubmitPublicExport(Registry, Dispatch, Params);
  if (!Result.IsOk()) AddError(Result.GetErrorJson());
  return Result.IsOk();
 };
 if (!TestTrue(TEXT("public property export"), ExportTo(Workspace))) return false;
 auto EditOrRead = [&](const FString& Root, bool bEdit) {
  TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Root, TEXT("properties.hjson"), true, false);
  int32 DefineMatches=0, RootMatches=0;
  for (const FString& File : Files) {
   FString Text; TSharedPtr<FJsonObject> Document;
   if (!FFileHelper::LoadFileToString(Text,*File) || !FUeAgentStructuredHjsonParser::ParseObject(Text,File,Document,Error)) return false;
   const TSharedPtr<FJsonObject>* Categories=nullptr;
   if (!Document->TryGetObjectField(TEXT("categories"),Categories)) continue;
   bool Changed=false;
   for (const auto& Category : (*Categories)->Values) {
    const TSharedPtr<FJsonObject>* Property=nullptr;
    if (Category.Value->AsObject()->TryGetObjectField(TEXT("AdditionalDefines"),Property)) {
     ++DefineMatches; const TArray<TSharedPtr<FJsonValue>>* Items=nullptr;
     if (!(*Property)->TryGetArrayField(TEXT("value"),Items) || Items->Num()!=(bEdit?1:2)) { Error=TEXT("ordinary defines shape:")+Text.Left(3000); return false; }
     if (bEdit) {
      FString CopyText; TSharedPtr<FJsonObject> Copy;
      if (!CommandContract::Json::TryCanonicalize((*Items)[0]->AsObject(),CopyText,Error) || !FUeAgentStructuredHjsonParser::ParseObject(CopyText,File,Copy,Error)) return false;
      const auto Fields=Copy->GetObjectField(TEXT("fields")); Fields->SetStringField(TEXT("DefineName"),TEXT("ADDED")); Fields->RemoveField(TEXT("DefineValue"));
      auto Updated=*Items; Updated.Add(MakeShared<FJsonValueObject>(Copy)); (*Property)->SetArrayField(TEXT("value"),Updated); Changed=true;
     } else if ((*Items)[1]->AsObject()->GetObjectField(TEXT("fields"))->GetStringField(TEXT("DefineName"))!=TEXT("ADDED")) return false;
    }
    if (Category.Value->AsObject()->TryGetObjectField(TEXT("TwoSided"),Property)) {
     ++RootMatches;
     if (bEdit) { (*Property)->SetBoolField(TEXT("value"),true); Changed=true; }
     else if (!(*Property)->GetBoolField(TEXT("value"))) return false;
    }
   }
   if (Changed && (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(),Text,Error) || !FFileHelper::SaveStringToFile(Text,*File,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) return false;
  }
  if (DefineMatches!=1 || RootMatches!=1) Error=FString::Printf(TEXT("ordinary property matches defines=%d root=%d"),DefineMatches,RootMatches);
  return DefineMatches==1 && RootMatches==1;
 };
 if (!TestTrue(TEXT("ordinary container append and root edit"), EditOrRead(Workspace,true))) { AddError(Error); return false; }
 const auto Source=MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"),TEXT("directory")); Source->SetStringField(TEXT("path"),Workspace);
 const auto Args=MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"),ObjectPath); Args->SetObjectField(TEXT("source"),Source);
 if (!CommandContract::Json::TryCanonicalize(Args,Params,Error)) return false;
 Custom=nullptr; Constant=nullptr; Material=nullptr; Package=nullptr;
 const auto Import=SubmitPublicImport(Registry,Dispatch,Params);
 if (!TestTrue(TEXT("public property import"),Import.IsOk())) { AddError(Import.GetErrorJson()); return false; }
 if (UPackage* Published=FindPackage(nullptr,*PackageName)) { FText UnloadError; if (!TestTrue(TEXT("independent Material unload"),UPackageTools::UnloadPackages({Published},UnloadError,true))) return false; }
 Material=LoadObject<UMaterial>(nullptr,*ObjectPath);
 if (!TestNotNull(TEXT("independent Material reload"),Material)) return false;
 for (UMaterialExpression* Expression : Material->GetExpressionCollection().Expressions) {
  if (Expression->MaterialExpressionGuid==CustomGuid) Custom=Cast<UMaterialExpressionCustom>(Expression);
  if (Expression->MaterialExpressionGuid==ConstantGuid) Constant=Cast<UMaterialExpressionConstant>(Expression);
 }
 if (!TestNotNull(TEXT("original custom identity"),Custom) || !TestNotNull(TEXT("original neighbor identity"),Constant)) return false;
 TestEqual(TEXT("native appended defines"),Custom->AdditionalDefines.Num(),2);
 if (Custom->AdditionalDefines.Num()!=2) return false;
 TestTrue(TEXT("original define preserved"),FCustomDefine::StaticStruct()->CompareScriptStruct(&Original,&Custom->AdditionalDefines[0],PPF_None));
 FCustomDefine Added; Added.DefineName=TEXT("ADDED");
 TestTrue(TEXT("new define native omitted default"),FCustomDefine::StaticStruct()->CompareScriptStruct(&Added,&Custom->AdditionalDefines[1],PPF_None));
 auto CheckFull=[&](const auto& Before,UObject* Object) {
  const auto After=FullState(Object); bool Match=Before.Num()==After.Num();
  for (const auto& Pair:Before) { const auto* Value=After.Find(Pair.Key); if (!Value || !Value->Equals(Pair.Value,ESearchCase::CaseSensitive)) { AddError(TEXT("native expression state changed:")+Pair.Key.ToString()); Match=false; } }
  return Match;
 };
 TestTrue(TEXT("complete unedited custom native state"),CheckFull(OriginalCustom,Custom));
 TestTrue(TEXT("complete neighboring expression state"),CheckFull(OriginalConstant,Constant));
 TestTrue(TEXT("root property and existing connection persisted"),Material->TwoSided && Material->GetExpressionInputForProperty(MP_Roughness)->Expression==Constant);
 TestFalse(TEXT("persisted Material clean"),Material->GetOutermost()->IsDirty());
 if (!TestTrue(TEXT("public independent reexport"),ExportTo(Readback))) return false;
 if (!TestTrue(TEXT("ordinary persisted properties"),EditOrRead(Readback,false))) AddError(Error);
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT85MaterialNativeActionVerticalTest,
	"UAI.Root.T85.Material.NativeActionVertical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85MaterialNativeActionVerticalTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI;
	using namespace UE::UAI::QueryOwners::Material;

	const FString PackageName = TEXT("/Game/UAI/StrictV2/T85/M_NativeAction_")
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
	UMaterial* const Material = Package
		? NewObject<UMaterial>(
			Package,
			*AssetName,
			RF_Public | RF_Standalone | RF_Transactional)
		: nullptr;
	if (!TestNotNull(TEXT("a persistent Material fixture is created"), Material))
	{
		return false;
	}
	FAssetRegistryModule::AssetCreated(Material);
	if (!TestTrue(
		TEXT("the initial Material fixture is saved"),
		SaveMaterial(*Package, *Material, Filename)))
	{
		return false;
	}

	FMaterialExpressionCandidateQueryResult BootstrapResult;
	FMaterialExpressionCandidateQueryError QueryError;
	if (!TestTrue(
		TEXT("the native Material catalog initializes the target graph"),
		TryQueryMaterialExpressionCandidates(
			*Material, TEXT("Multiply"), BootstrapResult, QueryError)))
	{
		AddError(QueryError.Code + TEXT(":") + QueryError.PublicMessage);
		return false;
	}
	const FMaterialExpressionCandidate* const Multiply =
		BootstrapResult.Candidates.FindByPredicate(
			[](const FMaterialExpressionCandidate& Candidate)
			{
				return Candidate.Name.Equals(
					TEXT("Multiply"), ESearchCase::IgnoreCase)
					&& Candidate.bSupported;
			});
	if (!TestNotNull(
		TEXT("the exact native Multiply action is searchable"), Multiply))
	{
		return false;
	}
	TestEqual(TEXT("the creation selector is the readable node name"),
		Multiply->Name, FString(TEXT("Multiply")));
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> CandidateRegistry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> CandidateDispatch;
	FString CandidateCompositionError;
	if (!TestTrue(TEXT("the public Material candidate command composes"),
		Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(
			CandidateRegistry, CandidateDispatch, CandidateCompositionError)))
	{
		AddError(CandidateCompositionError);
		return false;
	}
	auto CandidateParamsObject = MakeShared<FJsonObject>();
	CandidateParamsObject->SetStringField(TEXT("target"), ObjectPath);
	CandidateParamsObject->SetStringField(TEXT("query"), TEXT("Multiply"));
	FString CandidateParamsJson;
	FString CandidateJsonError;
	if (!TestTrue(TEXT("the public Material candidate request canonicalizes"),
		CommandContract::Json::TryCanonicalize(
			CandidateParamsObject, CandidateParamsJson, CandidateJsonError)))
	{
		AddError(CandidateJsonError);
		return false;
	}
	const CommandEdge::FUeAgentCommandResult PublicCandidateResult =
		SubmitPublicMaterialCandidateQuery(
			CandidateRegistry, CandidateDispatch, CandidateParamsJson);
	if (!TestTrue(TEXT("the public Material query returns original names"),
		PublicCandidateResult.IsOk()))
	{
		AddError(PublicCandidateResult.GetErrorJson());
		return false;
	}
	TSharedPtr<FJsonObject> PublicCandidatePayload;
	if (!TestTrue(TEXT("the public Material candidate payload is canonical JSON"),
		CommandContract::Json::TryParseObject(
			PublicCandidateResult.GetPayloadJson(), PublicCandidatePayload,
			CandidateJsonError)))
	{
		AddError(CandidateJsonError);
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* PublicNames = nullptr;
	bool bContainsOriginalMultiply = false;
	if (PublicCandidatePayload.IsValid()
		&& PublicCandidatePayload->TryGetArrayField(TEXT("names"), PublicNames)
		&& PublicNames)
	{
		for (const TSharedPtr<FJsonValue>& PublicName : *PublicNames)
		{
			bContainsOriginalMultiply |= PublicName.IsValid()
				&& PublicName->Type == EJson::String
				&& PublicName->AsString() == TEXT("Multiply");
		}
	}
	TestTrue(TEXT("the public candidate payload contains names only"),
		PublicCandidatePayload.IsValid()
			&& PublicCandidatePayload->Values.Num() == 1
			&& PublicNames
			&& bContainsOriginalMultiply);

	const FString WorkspacePath = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / (TEXT("Automation/UAI/T85/MaterialExport_")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*WorkspacePath, false, true); };
	FString ParamsError;

	TSharedPtr<OperationOwners::UObjectDataAsset::
		IUeAgentUObjectDataAssetCommandService> Service;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	UE::UAI::Contract::FDiagnostic RegistrationError;
	if (!TestTrue(
		TEXT("the production public import service composes"),
		OperationOwners::UObjectDataAsset::
			TryCreateProductionUObjectDataAssetCommandService(
				Service, RegistrationError))
		|| !Service.IsValid()
		|| !TestTrue(
			TEXT("production registration creates exact public dispatch"),
			Module::CommandRegistration::UObjectDataAsset::
				TryCreateUObjectDataAssetRegistration(
					Service.ToSharedRef(), Registry, Dispatch,
					RegistrationError)))
	{
		AddError(RegistrationError.ToPublicDetail());
		return false;
	}
	TSharedRef<FJsonObject> ExportParamsObject = MakeShared<FJsonObject>();
	ExportParamsObject->SetStringField(TEXT("target"), ObjectPath);
	ExportParamsObject->SetStringField(TEXT("workspace_path"), WorkspacePath);
	ExportParamsObject->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
	FString ExportParams;
	if (!TestTrue(TEXT("the Material export request canonicalizes"),
		CommandContract::Json::TryCanonicalize(ExportParamsObject, ExportParams, ParamsError)))
	{
		AddError(ParamsError);
		return false;
	}
	const CommandEdge::FUeAgentCommandResult ExportResult =
		SubmitPublicExport(Registry, Dispatch, ExportParams);
	if (!TestTrue(TEXT("the public Material export publishes a workspace"), ExportResult.IsOk()))
	{
		AddError(ExportResult.GetErrorJson());
		return false;
	}
	if (!TestTrue(TEXT("the exported visible graph edits by node alias only"),
		TryEditVisibleMaterialGraph(WorkspacePath, ParamsError)))
	{
		AddError(ParamsError);
		return false;
	}
	const TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
	Source->SetStringField(TEXT("kind"), TEXT("directory"));
	Source->SetStringField(TEXT("path"), WorkspacePath);
	const TSharedRef<FJsonObject> ImportParamsObject = MakeShared<FJsonObject>();
	ImportParamsObject->SetStringField(TEXT("target"), ObjectPath);
	ImportParamsObject->SetObjectField(TEXT("source"), Source);
	FString CanonicalParams;
	if (!TestTrue(TEXT("the edited Material workspace import request canonicalizes"),
		CommandContract::Json::TryCanonicalize(ImportParamsObject, CanonicalParams, ParamsError)))
	{
		AddError(ParamsError);
		return false;
	}
	const CommandEdge::FUeAgentCommandResult ImportResult =
		SubmitPublicImport(Registry, Dispatch, CanonicalParams);
	if (!TestTrue(
		TEXT("exact public dispatch invokes the Material HJSON provider"),
		ImportResult.IsOk()))
	{
		AddError(ImportResult.GetErrorJson());
		return false;
	}
	const FString ImportPayloadJson = ImportResult.GetPayloadJson();
	TSharedPtr<FJsonObject> ImportPayload;
	FString PayloadError;
	if (!TestTrue(
		TEXT("the Material import payload is canonical JSON"),
		CommandContract::Json::TryParseObject(
			ImportPayloadJson, ImportPayload, PayloadError)))
	{
		AddError(PayloadError);
		return false;
	}
	CollectGarbage(RF_NoFlags, true);
	TStrongObjectPtr<UMaterial> Reloaded(
		LoadObject<UMaterial>(nullptr, *ObjectPath));
	if (!TestNotNull(
		TEXT("the persisted Material independently reloads"), Reloaded.Get()))
	{
		return false;
	}
	const FString ReadbackWorkspacePath = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / (TEXT("Automation/UAI/T85/MaterialReadback_")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*ReadbackWorkspacePath, false, true); };
	TSharedRef<FJsonObject> ReadbackExportParamsObject = MakeShared<FJsonObject>();
	ReadbackExportParamsObject->SetStringField(TEXT("target"), ObjectPath);
	ReadbackExportParamsObject->SetStringField(TEXT("workspace_path"), ReadbackWorkspacePath);
	ReadbackExportParamsObject->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
	FString ReadbackExportParams;
	if (!TestTrue(TEXT("the Material independently re-exports after import"),
		CommandContract::Json::TryCanonicalize(
			ReadbackExportParamsObject, ReadbackExportParams, ParamsError)))
	{
		AddError(ParamsError);
		return false;
	}
	const CommandEdge::FUeAgentCommandResult ReadbackExportResult =
		SubmitPublicExport(Registry, Dispatch, ReadbackExportParams);
	if (!TestTrue(TEXT("the Material readback export dispatch succeeds"),
		ReadbackExportResult.IsOk()))
	{
		AddError(ReadbackExportResult.GetErrorJson());
		return false;
	}
	FString ReadbackGraphError;
	if (!TestTrue(
		TEXT("the re-exported Material graph retains the readable node and connection"),
		HasVisibleMaterialGraphResult(ReadbackWorkspacePath, ReadbackGraphError)))
	{
		AddError(ReadbackGraphError);
	}
	FString TerminalStatus;
	FString ProviderId;
	bool bSaved = false;
	bool bReloaded = false;
	TestTrue(TEXT("the import exposes only its compact success status"),
		ImportPayload.IsValid()
			&& ImportPayload->Values.Num() == 1
			&& ImportPayload->GetStringField(TEXT("status")) == TEXT("succeeded"));
	UMaterialExpression* Created = nullptr;
	for (UMaterialExpression* Expression : Reloaded->GetExpressions())
	{
		if (Expression
			&& Expression->GetClass()->GetPathName()
				== TEXT("/Script/Engine.MaterialExpressionMultiply"))
		{
			Created = Expression;
			break;
		}
	}
	if (!TestNotNull(
		TEXT("the native action-created expression survives reload"), Created))
	{
		return false;
	}
	TestEqual(TEXT("the searched action creates a Multiply expression"),
		Created->GetClass()->GetPathName(),
		FString(TEXT("/Script/Engine.MaterialExpressionMultiply")));
	FExpressionInput* const BaseColor =
		Reloaded->GetExpressionInputForProperty(MP_BaseColor);
	TestTrue(TEXT("native action semantics preserve FromPin autowiring"),
		BaseColor && BaseColor->Expression == Created);

	// Protect an unsaved owned expression and connection, independently of the
	// older on-disk graph, while the rejected operation replaces every node.
	TArray<uint8> DiskBefore;
	if (!TestTrue(TEXT("the original Material disk image is readable"),
		FFileHelper::LoadFileToArray(DiskBefore, *Filename))) return false;
	UMaterialExpressionMultiply* BeforeMultiply = Cast<UMaterialExpressionMultiply>(Created);
	UMaterialExpressionConstant* BeforeConstant = Cast<UMaterialExpressionConstant>(
		UMaterialEditingLibrary::CreateMaterialExpression(Reloaded.Get(),
			UMaterialExpressionConstant::StaticClass(), -400, 100));
	if (!TestNotNull(TEXT("the dirty graph owns a new Constant expression"), BeforeConstant)
		|| !TestNotNull(TEXT("the before graph owns a Multiply expression"), BeforeMultiply)) return false;
	BeforeConstant->R = 0.25f;
	BeforeMultiply->A.Connect(0, BeforeConstant);
	Reloaded->MarkPackageDirty();
	const FGuid BeforeMultiplyGuid = BeforeMultiply->MaterialExpressionGuid;
	const FGuid BeforeConstantGuid = BeforeConstant->MaterialExpressionGuid;
	UeAgentMaterialAuthoring::FAssetApplyRequest FailedRequest;
	FailedRequest.AssetPath = PackageName;
	FailedRequest.AssetKind = TEXT("material");
	FailedRequest.bSave = true;
	const TSharedRef<FJsonObject> Replacement = MakeShared<FJsonObject>();
	Replacement->SetStringField(TEXT("id"), TEXT("replacement"));
	Replacement->SetStringField(TEXT("expression_class"), TEXT("/Script/Engine.MaterialExpressionConstant"));
	const TSharedRef<FJsonObject> ValueEdit = MakeShared<FJsonObject>();
	ValueEdit->SetStringField(TEXT("property_path"), TEXT("R"));
	ValueEdit->SetNumberField(TEXT("value"), 0.75);
	Replacement->SetArrayField(TEXT("properties"), {MakeShared<FJsonValueObject>(ValueEdit)});
	const TSharedRef<FJsonObject> From = MakeShared<FJsonObject>();
	From->SetStringField(TEXT("node"), TEXT("replacement"));
	const TSharedRef<FJsonObject> RootInput = MakeShared<FJsonObject>();
	RootInput->SetStringField(TEXT("material_property"), TEXT("BaseColor"));
	RootInput->SetObjectField(TEXT("from"), From);
	FailedRequest.Graph = MakeShared<FJsonObject>();
	FailedRequest.Graph->SetArrayField(TEXT("expressions"), {MakeShared<FJsonValueObject>(Replacement)});
	FailedRequest.Graph->SetArrayField(TEXT("edges"), {});
	FailedRequest.Graph->SetArrayField(TEXT("root_inputs"), {MakeShared<FJsonValueObject>(RootInput)});
	bool bReachedFinalReadback = false;
	FailedRequest.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
	{
		UMaterial* Actual = LoadObject<UMaterial>(nullptr, *ObjectPath);
		UMaterialExpressionConstant* ActualConstant = Actual && Actual->GetExpressions().Num() == 1
			? Cast<UMaterialExpressionConstant>(Actual->GetExpressions()[0]) : nullptr;
		FExpressionInput* ActualBaseColor = Actual ? Actual->GetExpressionInputForProperty(MP_BaseColor) : nullptr;
		bReachedFinalReadback = ActualConstant && FMath::IsNearlyEqual(ActualConstant->R, 0.75f)
			&& ActualBaseColor && ActualBaseColor->Expression == ActualConstant;
		Failure.Diagnostic.Code = TEXT("t85_material_final_readback_rejected");
		Failure.Diagnostic.PublicMessage = TEXT("intentional final acceptance failure after graph disk reload");
		return false;
	};
	Reloaded.Reset();
	UeAgentMaterialAuthoring::FAssetApplyResult FailedResult;
	TestFalse(TEXT("the final Material readback rejection fails the same transaction"),
		UeAgentMaterialAuthoring::ApplyAsset(FailedRequest, FailedResult,
			TEXT("t85-material-final-readback-") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	TestTrue(TEXT("the rejection observes the replacement graph after persisted reload"), bReachedFinalReadback);
	TestTrue(TEXT("the Material late failure reports semantically confirmed recovery"),
		FailedResult.bRollbackAttempted && FailedResult.bRollbackSucceeded);
	if (!bReachedFinalReadback || !FailedResult.bRollbackSucceeded)
		AddError(FailedResult.Phase + TEXT(":") + FailedResult.Diagnostic.Code + TEXT(":") + FailedResult.Diagnostic.InternalContext);
	UMaterial* Restored = LoadObject<UMaterial>(nullptr, *ObjectPath);
	UMaterialExpressionMultiply* RestoredMultiply = nullptr;
	UMaterialExpressionConstant* RestoredConstant = nullptr;
	if (Restored)
	{
		for (UMaterialExpression* Expression : Restored->GetExpressions())
		{
			if (Expression && Expression->MaterialExpressionGuid == BeforeMultiplyGuid)
				RestoredMultiply = Cast<UMaterialExpressionMultiply>(Expression);
			if (Expression && Expression->MaterialExpressionGuid == BeforeConstantGuid)
				RestoredConstant = Cast<UMaterialExpressionConstant>(Expression);
		}
	}
	FExpressionInput* RestoredBaseColor = Restored ? Restored->GetExpressionInputForProperty(MP_BaseColor) : nullptr;
	TestTrue(TEXT("checkpoint restores both original owned nodes and their dirty property"),
		Restored && Restored->GetExpressions().Num() == 2 && RestoredMultiply && RestoredConstant
			&& FMath::IsNearlyEqual(RestoredConstant->R, 0.25f));
	TestTrue(TEXT("checkpoint restores the original nested graph connections"),
		RestoredMultiply && RestoredConstant && RestoredMultiply->A.Expression == RestoredConstant
			&& RestoredBaseColor && RestoredBaseColor->Expression == RestoredMultiply);
	TestTrue(TEXT("the Material restores the original dirty flag"), Restored && Restored->GetOutermost()->IsDirty());
	TArray<uint8> DiskAfter;
	TestTrue(TEXT("the Material retains the older disk image independently of dirty graph memory"),
		FFileHelper::LoadFileToArray(DiskAfter, *Filename) && DiskBefore == DiskAfter);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85MaterialPublicSelectionTest,
	"UAI.Root.T85.Material.PublicSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85MaterialPublicSelectionTest::RunTest(const FString& Parameters)
{
	const FString PackageName = TEXT("/Game/UAI/Transient/T85MaterialSelection/M_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageName);
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()
		/ TEXT("Automation/UAI/T85/MaterialSelection") / FPackageName::GetLongPackageAssetName(PackageName));
	const FString Readback = Workspace + TEXT("-readback");
	ON_SCOPE_EXIT {
		DeleteAssetIfPresent(ObjectPath);
		TestFalse(TEXT("selection fixture disk cleaned"), IFileManager::Get().FileExists(*Filename));
		IFileManager::Get().DeleteDirectory(*Workspace, false, true);
		IFileManager::Get().DeleteDirectory(*Readback, false, true);
	};
	UPackage* Package = CreatePackage(*PackageName);
	UMaterial* Material = NewObject<UMaterial>(Package, *FPackageName::GetLongPackageAssetName(PackageName), RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Material);
	UMaterialExpressionConstant* Neighbor = Cast<UMaterialExpressionConstant>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionConstant::StaticClass()));
	if (!TestNotNull(TEXT("selection fixture owns an existing expression"), Neighbor)) return false;
	Neighbor->R = 0.25f;
	Material->GetExpressionInputForProperty(MP_Roughness)->Connect(0, Neighbor);
	if (!TestTrue(TEXT("selection baseline saves"), SaveMaterial(*Package, *Material, Filename))) return false;
	Neighbor->R = 0.625f;
	Material->TwoSided = true;
	Material->MarkPackageDirty();
	const FGuid NeighborGuid = Neighbor->MaterialExpressionGuid;

	UeAgentPaletteSearch::FSearchParams Search;
	Search.CatalogType = TEXT("material_node"); Search.AssetPath = ObjectPath; Search.GraphName = TEXT("MaterialGraph");
	TArray<UeAgentPaletteSearch::FCandidate> Candidates;
	TArray<FString> Warnings;
	FString Error;
	if (!TestTrue(TEXT("current real native catalog enumerates"), UeAgentPaletteSearch::CollectMaterialCandidates(Search, Candidates, Warnings, Error))) { AddError(Error); return false; }
	FString Name, ExpectedClass;
	for (const auto& Expression : Candidates)
	{
		if (Expression.Kind != TEXT("material_expression") || !Expression.bSupportedInContext) continue;
		const auto* Function = Candidates.FindByPredicate([&](const auto& Candidate) {
			return Candidate.Kind == TEXT("material_function_call") && Candidate.Name == Expression.Name && Candidate.bSupportedInContext;
		});
		if (Function) { Name = Expression.Name; ExpectedClass = Expression.AuthoringSelectors.FindRef(TEXT("expression_class")); break; }
	}
	if (!TestTrue(TEXT("real expression/function collision has an exact native class"), !Name.IsEmpty() && !ExpectedClass.IsEmpty())) return false;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> CandidateRegistry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> CandidateDispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(CandidateRegistry, CandidateDispatch, Error)) { AddError(Error); return false; }
	auto QueryArgs = MakeShared<FJsonObject>(); QueryArgs->SetStringField(TEXT("target"), ObjectPath); QueryArgs->SetStringField(TEXT("query"), Name);
	FString QueryParams; if (!CommandContract::Json::TryCanonicalize(QueryArgs, QueryParams, Error)) return false;
	const auto Queried = SubmitPublicMaterialCandidateQuery(CandidateRegistry, CandidateDispatch, QueryParams);
	if (!TestTrue(TEXT("real same-name group is publicly searchable"), Queried.IsOk())) { AddError(Queried.GetErrorJson()); return false; }
	TSharedPtr<FJsonObject> QueryPayload;
	if (!CommandContract::Json::TryParseObject(Queried.GetPayloadJson(), QueryPayload, Error)) return false;
	int32 NameCount = 0;
	for (const auto& Value : QueryPayload->GetArrayField(TEXT("names"))) if (Value->AsString() == Name) ++NameCount;
	if (!TestTrue(TEXT("public same-name query emits one unchanged name"), QueryPayload->Values.Num() == 1 && NameCount == 1)) return false;

	TSharedPtr<OperationOwners::UObjectDataAsset::IUeAgentUObjectDataAssetCommandService> Service;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!OperationOwners::UObjectDataAsset::TryCreateProductionUObjectDataAssetCommandService(Service, Error)
		|| !Module::CommandRegistration::UObjectDataAsset::TryCreateUObjectDataAssetRegistration(Service.ToSharedRef(), Registry, Dispatch, Error)) { AddError(Error); return false; }
	auto Export = [&](const FString& Path) {
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetStringField(TEXT("workspace_path"), Path); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		FString Params; if (!CommandContract::Json::TryCanonicalize(Args, Params, Error)) return false;
		const auto Result = SubmitPublicExport(Registry, Dispatch, Params);
		if (!Result.IsOk()) AddError(Result.GetErrorJson());
		return Result.IsOk();
	};
	if (!TestTrue(TEXT("selection public export"), Export(Workspace))) return false;
	FString GraphFile;
	TSharedPtr<FJsonObject> GraphDocument;
	TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Workspace, TEXT("*.hjson"), true, false);
	for (const FString& File : Files)
	{
		if (File.EndsWith(TEXT(".interfaces.hjson"))) continue;
		FString Text; TSharedPtr<FJsonObject> Document;
		if (FFileHelper::LoadFileToString(Text, *File) && FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)
			&& Document->HasTypedField<EJson::Object>(TEXT("nodes"))) { GraphFile = File; GraphDocument = Document; break; }
	}
	if (!TestTrue(TEXT("ordinary exported graph located"), GraphDocument.IsValid())) return false;
	auto WriteCandidate = [&](const FString& CandidateName, const TSharedPtr<FJsonObject>& Select) {
		auto Candidate = MakeShared<FJsonObject>(); Candidate->SetStringField(TEXT("name"), CandidateName);
		if (Select.IsValid()) Candidate->SetObjectField(TEXT("select"), Select);
		auto Node = MakeShared<FJsonObject>(); Node->SetStringField(TEXT("name"), Name); Node->SetObjectField(TEXT("candidate"), Candidate);
		GraphDocument->GetObjectField(TEXT("nodes"))->SetObjectField(TEXT("Selected"), Node);
		FString Text;
		return CommandContract::Json::TryCanonicalize(GraphDocument, Text, Error)
			&& FFileHelper::SaveStringToFile(Text, *GraphFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	};
	auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Workspace);
	auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetObjectField(TEXT("source"), Source);
	FString ImportParams; if (!CommandContract::Json::TryCanonicalize(Args, ImportParams, Error)) return false;
	auto FullState = [](UObject* Object) {
		TMap<FName, FString> Values;
		for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It) {
			if (It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)) continue;
			FString Value; It->ExportTextItem_Direct(Value, It->ContainerPtrToValuePtr<void>(Object), nullptr, Object, PPF_Delimited); Values.Add(It->GetFName(), MoveTemp(Value));
		}
		return Values;
	};
	const auto BeforeMaterial = FullState(Material), BeforeNeighbor = FullState(Neighbor);
	TWeakObjectPtr<UMaterial> BeforeObject = Material;
	TWeakObjectPtr<UMaterialExpressionConstant> BeforeExpression = Neighbor;
	TWeakObjectPtr<UMaterialGraph> BeforeGraph = Material->MaterialGraph;
	const bool bHadGraph = Material->MaterialGraph != nullptr;
	if (!TestTrue(TEXT("write-before observation starts with the original material and expression"),
		BeforeObject.IsValid() && BeforeExpression.IsValid() && Material->GetExpressions().Num() == 1
			&& Material->GetExpressions()[0] == Neighbor)) return false;
	TArray<uint8> BeforeDisk; if (!FFileHelper::LoadFileToArray(BeforeDisk, *Filename)) return false;
	auto Reject = [&](const FString& CandidateName, const TSharedPtr<FJsonObject>& Select, const FString& ExpectedCode, TSharedPtr<FJsonObject>& Detail) {
		if (!WriteCandidate(CandidateName, Select)) return false;
		const auto Result = SubmitPublicImport(Registry, Dispatch, ImportParams);
		if (!TestFalse(TEXT("public selection import rejects before target writing"), Result.IsOk())) return false;
		AddInfo(TEXT("public selection rejection: ") + Result.GetErrorJson());
		TSharedPtr<FJsonObject> Failure;
		if (!CommandContract::Json::TryParseObject(Result.GetErrorJson(), Failure, Error)) return false;
		TestEqual(TEXT("public failure preserves candidate code"), Failure->GetStringField(TEXT("code")), ExpectedCode);
		FString DetailText;
		if (Failure->TryGetStringField(TEXT("detail"), DetailText) && !DetailText.IsEmpty()) CommandContract::Json::TryParseObject(DetailText, Detail, Error);
		// Observe immediately after the failed public request, before any cleanup or
		// successful retry: an unload/checkpoint rollback would invalidate these objects.
		UMaterial* Actual = FindObject<UMaterial>(nullptr, *ObjectPath);
		TestTrue(TEXT("write-before rejection preserves original material identity"), BeforeObject.IsValid() && Actual == BeforeObject.Get());
		TestTrue(TEXT("write-before rejection preserves original expression identity"), BeforeExpression.IsValid()
			&& Actual && Actual->GetExpressions().Num() == 1 && Actual->GetExpressions()[0] == BeforeExpression.Get());
		TestTrue(TEXT("write-before rejection preserves graph presence and identity"), Actual
			&& (!bHadGraph || BeforeGraph.IsValid()) && Actual->MaterialGraph == BeforeGraph.Get());
		if (!Actual || !BeforeExpression.IsValid()) return false;
		TestTrue(TEXT("rejection preserves complete persistent native content"), FullState(Actual).OrderIndependentCompareEqual(BeforeMaterial) && FullState(BeforeExpression.Get()).OrderIndependentCompareEqual(BeforeNeighbor));
		TestTrue(TEXT("rejection preserves dirty memory distinct from disk"), Actual->GetOutermost()->IsDirty() && BeforeExpression->R == 0.625f && Actual->TwoSided);
		TArray<uint8> AfterDisk;
		TestTrue(TEXT("rejection preserves exact original disk bytes"), FFileHelper::LoadFileToArray(AfterDisk, *Filename) && AfterDisk == BeforeDisk);
		return !HasAnyErrors();
	};
	TSharedPtr<FJsonObject> Ambiguity;
	if (!Reject(Name, {}, TEXT("candidate_ambiguous"), Ambiguity)) return false;
	const TSharedPtr<FJsonObject>* Choose = nullptr;
	if (!TestTrue(TEXT("public ambiguity has consumable current choice object"), Ambiguity.IsValid()
		&& Ambiguity->GetStringField(TEXT("name")) == Name && Ambiguity->TryGetObjectField(TEXT("choose"), Choose) && Choose)) return false;
	auto Selection = MakeShared<FJsonObject>();
	for (const auto& Field : (*Choose)->Values) {
		for (const auto& Value : Field.Value->AsArray()) if (Value->AsString() == TEXT("expression")) Selection->SetField(Field.Key, Value);
	}
	if (!TestEqual(TEXT("actual returned expression choice copied without remapping"), Selection->Values.Num(), 1)) return false;
	TSharedPtr<FJsonObject> RejectedDetail;
	auto Expired = MakeShared<FJsonObject>(); Expired->SetStringField(TEXT("form"), TEXT("removed"));
	if (!Reject(Name, Expired, TEXT("candidate_selector_invalid"), RejectedDetail)) return false;
	TestTrue(TEXT("public expired select exposes current choices and matching code"), RejectedDetail.IsValid() && RejectedDetail->HasField(TEXT("choose")) && RejectedDetail->GetStringField(TEXT("code")) == TEXT("candidate_selector_invalid"));
	if (!Reject(TEXT("NoSuchMaterialAction"), {}, TEXT("candidate_name_not_found"), RejectedDetail)
		|| !Reject(Name + TEXT(" [material_expression]"), {}, TEXT("candidate_name_not_found"), RejectedDetail)) return false;
	if (!WriteCandidate(Name, Selection)) return false;
	Material = nullptr; Package = nullptr; Neighbor = nullptr;
	const auto Imported = SubmitPublicImport(Registry, Dispatch, ImportParams);
	if (!TestTrue(TEXT("returned public selection imports through the same HJSON entry"), Imported.IsOk())) { AddError(Imported.GetErrorJson()); return false; }
	if (UPackage* Published = FindPackage(nullptr, *PackageName)) {
		FText UnloadError; if (!TestTrue(TEXT("selected Material independently unloads"), UPackageTools::UnloadPackages({Published}, UnloadError, true))) return false;
	}
	TestFalse(TEXT("pre-import Material identity is no longer resident"), BeforeObject.IsValid());
	Material = LoadObject<UMaterial>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("selected Material independently reloads from disk"), Material)) return false;
	UMaterialExpression* Created = nullptr;
	for (UMaterialExpression* Expression : Material->GetExpressions()) {
		if (Expression->MaterialExpressionGuid == NeighborGuid) Neighbor = Cast<UMaterialExpressionConstant>(Expression);
		else Created = Expression;
	}
	if (!TestNotNull(TEXT("selected native expression persists"), Created) || !TestNotNull(TEXT("unedited neighbor persists"), Neighbor)) return false;
	TestEqual(TEXT("selection creates the exact expression rather than same-name function"), Created->GetClass()->GetPathName(), ExpectedClass);
	// Existing instance export uses the native class short name, independently
	// of the palette's human-readable action label (e.g. Smooth Step / SmoothStep).
	FString ExportedInstanceName = Created->GetClass()->GetName();
	ExportedInstanceName.RemoveFromStart(TEXT("MaterialExpression"));
	TestEqual(TEXT("only one requested expression added"), Material->GetExpressions().Num(), 2);
	TestTrue(TEXT("unedited neighbor full native state persists"), FullState(Neighbor).OrderIndependentCompareEqual(BeforeNeighbor));
	TestTrue(TEXT("unedited root and connection persist clean"), Material->TwoSided && !Material->GetOutermost()->IsDirty() && Material->GetExpressionInputForProperty(MP_Roughness)->Expression == Neighbor);
	for (TFieldIterator<FProperty> It(Created->GetClass()); It; ++It) {
		if (It->GetOwnerStruct() != Created->GetClass() || It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)) continue;
		TestTrue(*FString::Printf(TEXT("omitted native default %s"), *It->GetName()), It->Identical_InContainer(Created, Created->GetClass()->GetDefaultObject()));
	}
	if (!TestTrue(TEXT("selected Material ordinary reexport"), Export(Readback))) return false;
	Files.Reset(); IFileManager::Get().FindFilesRecursive(Files, *Readback, TEXT("*.hjson"), true, false);
	bool bReexported = false;
	for (const FString& File : Files) {
		if (File.EndsWith(TEXT(".interfaces.hjson"))) continue;
		FString Text; TSharedPtr<FJsonObject> Document;
		if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) continue;
		const TSharedPtr<FJsonObject>* Nodes = nullptr;
		if (!Document->TryGetObjectField(TEXT("nodes"), Nodes)) continue;
		for (const auto& Pair : (*Nodes)->Values) {
			const auto Node = Pair.Value->AsObject(); FString NodeName;
			if (Node->TryGetStringField(TEXT("name"), NodeName) && NodeName == ExportedInstanceName) bReexported = true;
		}
	}
	TestTrue(TEXT("ordinary graph contains selected persisted node"), bReexported);
	if (!HasAnyErrors()) AddInfo(TEXT("public selection persisted: candidate.name=") + Name
		+ TEXT(" candidate.select={form:expression}; ordinary instance.name=") + ExportedInstanceName
		+ TEXT("; independent reload, defaults, neighbor and ordinary reexport verified"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208MaterialInstanceWorkspace,
	"UAI.Root.T208.MaterialInstance.Workspace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208MaterialInstanceWorkspace::RunTest(const FString&)
{
	using namespace UE::UAI;
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ParentPath = TEXT("/Game/UAI/Tests/T208/InstanceParent_") + Suffix;
	const FString InstancePath = TEXT("/Game/UAI/Tests/T208/Instance_") + Suffix;
	const FString ObjectPath = InstancePath + TEXT(".") + FPackageName::GetLongPackageAssetName(InstancePath);
	FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-material-instance-common/automation") / Suffix);
	AddInfo(TEXT("instance workspace: ") + Workspace);
	UPackage* ParentPackage = CreatePackage(*ParentPath);
	UMaterial* Parent = NewObject<UMaterial>(ParentPackage, *FPackageName::GetLongPackageAssetName(ParentPath), RF_Public | RF_Standalone | RF_Transactional);
	for (const FName Name : {FName(TEXT("Amount")), FName(TEXT("Preserve")), FName(TEXT("Inherited"))})
	{
		auto* Expression = CastChecked<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Parent, UMaterialExpressionScalarParameter::StaticClass()));
		Expression->ParameterName = Name;
		Expression->DefaultValue = Name == TEXT("Amount") ? 0.25f : 0.5f;
	}
	UTexture* OriginalTexture = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
	UTexture* ChangedTexture = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
	if (!TestNotNull(TEXT("texture fixture default"), OriginalTexture) || !TestNotNull(TEXT("texture fixture alternate"), ChangedTexture)) return false;
	const FString ChangedTexturePath = ChangedTexture->GetPathName();
	auto* Tint = CastChecked<UMaterialExpressionVectorParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Parent, UMaterialExpressionVectorParameter::StaticClass()));
	Tint->ParameterName = TEXT("Tint"); Tint->DefaultValue = FLinearColor(0.1f, 0.2f, 0.3f, 1.0f);
	auto* Texture = CastChecked<UMaterialExpressionTextureSampleParameter2D>(UMaterialEditingLibrary::CreateMaterialExpression(Parent, UMaterialExpressionTextureSampleParameter2D::StaticClass()));
	Texture->ParameterName = TEXT("Image"); Texture->Texture = OriginalTexture;
	auto* EmptyTexture = CastChecked<UMaterialExpressionTextureSampleParameter2D>(UMaterialEditingLibrary::CreateMaterialExpression(Parent, UMaterialExpressionTextureSampleParameter2D::StaticClass()));
	EmptyTexture->ParameterName = TEXT("EmptyImage"); EmptyTexture->Texture = nullptr;
	auto* Switch = CastChecked<UMaterialExpressionStaticSwitchParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Parent, UMaterialExpressionStaticSwitchParameter::StaticClass()));
	Switch->ParameterName = TEXT("Enabled"); Switch->DefaultValue = false;
	Parent->PostEditChange();
	FString Error;
	auto SaveFixture = [](UObject* Asset)
	{
		const FString Filename = FPackageName::LongPackageNameToFilename(Asset->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
		FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone;
		return UPackage::SavePackage(Asset->GetOutermost(), Asset, *Filename, Args);
	};
	if (!TestTrue(TEXT("parent fixture persists"), SaveFixture(Parent))) return false;
	UPackage* InstancePackage = CreatePackage(*InstancePath);
	UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(InstancePackage, *FPackageName::GetLongPackageAssetName(InstancePath), RF_Public | RF_Standalone | RF_Transactional);
	Instance->SetParentEditorOnly(Parent);
	Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Preserve")), 0.8f);
	const FMaterialParameterInfo OrphanInfo(TEXT("Detached"), EMaterialParameterAssociation::LayerParameter, 2);
	Instance->SetScalarParameterValueEditorOnly(OrphanInfo, 0.125f);
	UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
	FAssetRegistryModule::AssetCreated(Parent);
	FAssetRegistryModule::AssetCreated(Instance);
	if (!TestTrue(TEXT("instance fixture persists"), SaveFixture(Instance))) return false;
	ON_SCOPE_EXIT
	{
		TGuardValue<bool> Guard(GIsRunningUnattendedScript, true);
		TArray<UObject*> Owned;
		if (UObject* Asset = LoadObject<UObject>(nullptr, *ObjectPath)) Owned.Add(Asset);
		if (UObject* Asset = LoadObject<UObject>(nullptr, *(ParentPath + TEXT(".") + FPackageName::GetLongPackageAssetName(ParentPath)))) Owned.Add(Asset);
		TestEqual(TEXT("owned instance and parent cleanup"), ObjectTools::DeleteObjects(Owned, false), Owned.Num());
		TestFalse(TEXT("owned instance removed from disk"), FPackageName::DoesPackageExist(InstancePath));
		TestFalse(TEXT("owned instance parent removed from disk"), FPackageName::DoesPackageExist(ParentPath));
	};
	Parent = nullptr; Instance = nullptr; ParentPackage = nullptr; InstancePackage = nullptr;
	TSharedPtr<OperationOwners::UObjectDataAsset::IUeAgentUObjectDataAssetCommandService> Service;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!OperationOwners::UObjectDataAsset::TryCreateProductionUObjectDataAssetCommandService(Service, Error)
		|| !Module::CommandRegistration::UObjectDataAsset::TryCreateUObjectDataAssetRegistration(Service.ToSharedRef(), Registry, Dispatch, Error))
	{ AddError(Error); return false; }
	auto Export = [&](const FString& Path)
	{
		auto Params = MakeShared<FJsonObject>(); Params->SetStringField(TEXT("target"), ObjectPath);
		Params->SetStringField(TEXT("workspace_path"), Path); Params->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		FString Json; if (!CommandContract::Json::TryCanonicalize(Params, Json, Error)) return false;
		auto Result = SubmitPublicExport(Registry, Dispatch, Json);
		if (!Result.IsOk()) Error = Result.GetErrorJson();
		return Result.IsOk();
	};
	auto Import = [&]()
	{
		auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Workspace);
		auto Params = MakeShared<FJsonObject>(); Params->SetStringField(TEXT("target"), ObjectPath); Params->SetObjectField(TEXT("source"), Source);
		FString Json; CommandContract::Json::TryCanonicalize(Params, Json, Error);
		return SubmitPublicImport(Registry, Dispatch, Json);
	};
	if (!TestTrue(TEXT("ordinary instance export"), Export(Workspace))) { AddError(Error); return false; }
	// Every parameter points to the shared catalog outside its business values.
	FString CatalogText;
	TSharedPtr<FJsonObject> Catalog;
	const FString CatalogFile = Workspace / TEXT("targets/authoring_target/definitions.hjson");
	if (!FFileHelper::LoadFileToString(CatalogText, *CatalogFile)
		|| !FUeAgentStructuredHjsonParser::ParseObject(CatalogText, CatalogFile, Catalog, Error)) return false;
	const auto CatalogRows = Catalog->GetObjectField(TEXT("definitions"));
	auto FindDefinition = [&](const FString& Ref) -> TSharedPtr<FJsonObject>
	{
		for (const auto& Row : CatalogRows->Values)
			if (Row.Value->Type == EJson::Object && Row.Value->AsObject()->GetStringField(TEXT("ref")) == Ref) return Row.Value->AsObject();
		return nullptr;
	};
	TArray<FString> ParameterFiles;
	IFileManager::Get().FindFilesRecursive(ParameterFiles, *Workspace, TEXT("state.hjson"), true, false);
	TSet<FString> ParameterKinds;
	FString DefinitionEditFile, DefinitionEditOriginal;
	TSharedPtr<FJsonObject> DefinitionEditDocument;
	for (const auto& File : ParameterFiles)
	{
		if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
		FString Text; TSharedPtr<FJsonObject> Document;
		if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) continue;
		const TSharedPtr<FJsonObject>* Unit = nullptr;
		if (!Document->TryGetObjectField(TEXT("unit"), Unit)) continue;
		const auto Values = (*Unit)->GetObjectField(TEXT("values"));
		FString Kind;
		if (!Values->TryGetStringField(TEXT("parameter_kind"), Kind)) continue;
		ParameterKinds.Add(Kind);
		FString Ref;
		if (!TestTrue(TEXT("parameter has ordinary definition reference"), (*Unit)->TryGetStringField(TEXT("definition"), Ref))) return false;
		const auto Definition = FindDefinition(Ref);
		if (!TestTrue(TEXT("parameter definition exists in shared catalog"), Definition.IsValid())) return false;
		TestTrue(TEXT("parameter catalog declares the current editable contract"), Definition->GetBoolField(TEXT("editable")));
		const auto Fields = Definition->GetObjectField(TEXT("value_definition"))->GetObjectField(TEXT("fields"));
		const auto ValueDefinition = FindDefinition(Fields->GetStringField(TEXT("value")));
		if (!TestTrue(TEXT("parameter value resolves to native type definition"), ValueDefinition.IsValid() && ValueDefinition->HasField(TEXT("type")))) return false;
		TestFalse(TEXT("definition metadata is not a business value"), Values->HasField(TEXT("definition")));
		if (Kind == TEXT("scalar") && DefinitionEditFile.IsEmpty())
		{ DefinitionEditFile = File; DefinitionEditOriginal = Text; DefinitionEditDocument = Document; }
	}
	TestEqual(TEXT("all native parameter kinds have reachable definitions"), ParameterKinds.Num(), 4);
	if (!DefinitionEditDocument) return false;
	DefinitionEditDocument->GetObjectField(TEXT("unit"))->SetStringField(TEXT("definition"), TEXT("collection:invalid_definition"));
	FString ChangedDefinition; CommandContract::Json::TryCanonicalize(DefinitionEditDocument, ChangedDefinition, Error);
	FFileHelper::SaveStringToFile(ChangedDefinition, *DefinitionEditFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	auto DefinitionRejected = Import();
	TestFalse(TEXT("definition reference cannot be edited as business state"), DefinitionRejected.IsOk());
	TestTrue(TEXT("definition reference rejection is explicit"), DefinitionRejected.GetErrorJson().Contains(TEXT("semantic_workspace_item_definition_read_only")));
	FFileHelper::SaveStringToFile(DefinitionEditOriginal, *DefinitionEditFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	FString EditedName = TEXT("Amount");
	TSharedPtr<FJsonValue> EditedValue = MakeShared<FJsonValueNumber>(0.65);
	bool bEditedOverride = true;
	TFunction<bool(const TSharedPtr<FJsonObject>&)> EditParameter = [&](const auto& Document)
	{
		const TSharedPtr<FJsonObject>* RootProperty = nullptr;
		if (EditedName == TEXT("bOverrideSubsurfaceProfile")
			&& Document->TryGetObjectField(EditedName, RootProperty) && RootProperty && RootProperty->IsValid())
		{
			(*RootProperty)->SetField(TEXT("value"), EditedValue);
			return true;
		}
		FString Name;
		if (Document->TryGetStringField(TEXT("parameter_name"), Name) && Name == EditedName)
		{
			Document->SetField(TEXT("value"), EditedValue);
			Document->SetBoolField(TEXT("authored_override"), bEditedOverride);
			return true;
		}
		for (const auto& Pair : Document->Values)
		{
			if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Object && EditParameter(Pair.Value->AsObject())) return true;
		}
		return false;
	};
	auto WriteVisibleEdit = [&]()
	{
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Workspace, TEXT("*.hjson"), true, false);
		for (const FString& File : Files)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Document;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) continue;
			if (!EditParameter(Document)) continue;
			FString Updated; CommandContract::Json::TryCanonicalize(Document, Updated, Error);
			return FFileHelper::SaveStringToFile(Updated, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}
		return false;
	};
	if (!TestTrue(TEXT("visible inherited parameter is editable"), WriteVisibleEdit())) return false;
	auto Imported = Import();
	if (!TestTrue(TEXT("ordinary instance import applies"), Imported.IsOk())) { AddError(Imported.GetErrorJson()); return false; }
	Instance = LoadObject<UMaterialInstanceConstant>(nullptr, *ObjectPath);
	float Amount = 0, Preserve = 0;
	if (!TestNotNull(TEXT("persistent instance reload"), Instance)) return false;
	Instance->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Amount")), Amount);
	Instance->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Preserve")), Preserve);
	TestTrue(TEXT("effective scalar changed"), FMath::IsNearlyEqual(Amount, 0.65f));
	TestTrue(TEXT("non-target override preserved"), FMath::IsNearlyEqual(Preserve, 0.8f));
	TestEqual(TEXT("parent preserved"), Instance->Parent->GetOutermost()->GetName(), ParentPath);
	TestTrue(TEXT("independent ordinary reexport"), Export(Workspace + TEXT("-readback")));
	TArray<TSharedPtr<FJsonValue>> Parameters;
	if (!UeAgentMaterialAuthoring::CaptureInstanceParameters(*Instance, Parameters, Error)) { AddError(Error); return false; }
	const auto* EmptyValue = Parameters.FindByPredicate([](const auto& P) { return P->AsObject()->GetStringField(TEXT("parameter_name")) == TEXT("EmptyImage"); });
	TestTrue(TEXT("null inherited texture remains visible read state"), EmptyValue && (*EmptyValue)->AsObject()->TryGetField(TEXT("value"))->IsNull());
	auto* AmountValue = Parameters.FindByPredicate([](const auto& P) { return P->AsObject()->GetStringField(TEXT("parameter_name")) == TEXT("Amount"); });
	if (!AmountValue) return false;
	(*AmountValue)->AsObject()->SetNumberField(TEXT("value"), 0.9);
	UeAgentMaterialAuthoring::FAssetApplyRequest Request;
	Request.AssetPath = InstancePath; Request.AssetKind = TEXT("material_instance"); Request.bSave = true;
	Request.InstanceParameters = {*AmountValue};
	auto LateRootProperty = MakeShared<FJsonObject>();
	LateRootProperty->SetStringField(TEXT("property_path"), TEXT("bOverrideSubsurfaceProfile"));
	LateRootProperty->SetBoolField(TEXT("value"), true);
	Request.Properties = {MakeShared<FJsonValueObject>(LateRootProperty)};
	bool bObservedPersistedValue = false;
	Request.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
	{
		auto* Reloaded = LoadObject<UMaterialInstanceConstant>(nullptr, *ObjectPath); float Value = 0;
		bObservedPersistedValue = Reloaded && Reloaded->bOverrideSubsurfaceProfile
			&& Reloaded->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Amount")), Value) && FMath::IsNearlyEqual(Value, 0.9f);
		Failure.Diagnostic.Code = TEXT("t208_instance_final_readback_rejected"); return false;
	};
	Instance = nullptr;
	UeAgentMaterialAuthoring::FAssetApplyResult Failed;
	TestFalse(TEXT("late rejection fails"), UeAgentMaterialAuthoring::ApplyAsset(Request, Failed));
	TestTrue(TEXT("late rejection sees persisted changed value"), bObservedPersistedValue);
	TestTrue(TEXT("late rejection confirms recovery"), Failed.bRollbackAttempted && Failed.bRollbackSucceeded);
	if (!bObservedPersistedValue || !Failed.bRollbackSucceeded) AddError(Failed.Phase + TEXT(":") + Failed.Diagnostic.Code + TEXT(":") + Failed.Diagnostic.InternalContext);
	Instance = LoadObject<UMaterialInstanceConstant>(nullptr, *ObjectPath);
	Amount = 0;
	TestTrue(TEXT("late rejection restores original scalar"), Instance && Instance->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Amount")), Amount) && FMath::IsNearlyEqual(Amount, 0.65f));
	TestTrue(TEXT("late rejection restores root property"), Instance && !Instance->bOverrideSubsurfaceProfile);
	Workspace += TEXT("-native-types");
	if (!TestTrue(TEXT("native type workspace export"), Export(Workspace))) { AddError(Error); return false; }
	EditedName = TEXT("Tint");
	EditedValue = MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
		MakeShared<FJsonValueNumber>(0.4), MakeShared<FJsonValueNumber>(0.5), MakeShared<FJsonValueNumber>(0.6), MakeShared<FJsonValueNumber>(0.7)});
	if (!TestTrue(TEXT("vector edit"), WriteVisibleEdit())) return false;
	EditedName = TEXT("Enabled"); EditedValue = MakeShared<FJsonValueBoolean>(true);
	if (!TestTrue(TEXT("static switch edit"), WriteVisibleEdit())) return false;
	EditedName = TEXT("bOverrideSubsurfaceProfile"); EditedValue = MakeShared<FJsonValueBoolean>(true);
	if (!TestTrue(TEXT("root property shares parameter workspace"), WriteVisibleEdit())) return false;
	EditedName = TEXT("Image"); EditedValue = MakeShared<FJsonValueString>(ObjectPath);
	if (!TestTrue(TEXT("invalid texture edit"), WriteVisibleEdit())) return false;
	const FString InstanceFilename = FPackageName::LongPackageNameToFilename(InstancePath, FPackageName::GetAssetPackageExtension());
	TArray<uint8> BeforeBytes, AfterBytes; FFileHelper::LoadFileToArray(BeforeBytes, *InstanceFilename);
	const bool bBeforeDirty = Instance->GetOutermost()->IsDirty();
	Instance = nullptr;
	auto Rejected = Import();
	TestFalse(TEXT("non-Texture reference rejects before apply"), Rejected.IsOk());
	Instance = LoadObject<UMaterialInstanceConstant>(nullptr, *ObjectPath);
	FLinearColor TintValue;
	Instance->GetVectorParameterValue(FMaterialParameterInfo(TEXT("Tint")), TintValue);
	TestTrue(TEXT("preflight rejection preserves vector"), TintValue.Equals(FLinearColor(0.1f, 0.2f, 0.3f, 1.0f)));
	TestFalse(TEXT("preflight rejection preserves root property"), static_cast<bool>(Instance->bOverrideSubsurfaceProfile));
	TestEqual(TEXT("preflight rejection preserves dirty state"), Instance->GetOutermost()->IsDirty(), bBeforeDirty);
	FFileHelper::LoadFileToArray(AfterBytes, *InstanceFilename); TestTrue(TEXT("preflight rejection preserves disk bytes"), BeforeBytes == AfterBytes);
	EditedValue = MakeShared<FJsonValueString>(ChangedTexturePath);
	if (!TestTrue(TEXT("valid texture edit"), WriteVisibleEdit())) return false;
	Instance = nullptr;
	auto TypesImported = Import();
	if (!TestTrue(TEXT("native parameter kinds import together"), TypesImported.IsOk())) { AddError(TypesImported.GetErrorJson()); return false; }
	Instance = LoadObject<UMaterialInstanceConstant>(nullptr, *ObjectPath);
	Instance->GetVectorParameterValue(FMaterialParameterInfo(TEXT("Tint")), TintValue);
	UTexture* ImageValue = nullptr; bool bSwitchValue = false; FGuid SwitchGuid;
	Instance->GetTextureParameterValue(FMaterialParameterInfo(TEXT("Image")), ImageValue);
	Instance->GetStaticSwitchParameterValue(FMaterialParameterInfo(TEXT("Enabled")), bSwitchValue, SwitchGuid);
	TestTrue(TEXT("vector persists"), TintValue.Equals(FLinearColor(0.4f, 0.5f, 0.6f, 0.7f)));
	TestTrue(TEXT("texture reference persists"), ImageValue && ImageValue->GetPathName() == ChangedTexturePath);
	TestTrue(TEXT("static switch effective value persists"), bSwitchValue);
	const FStaticParameterSet NativeSwitches = Instance->GetStaticParameters();
	if (const auto* CurrentParent = Cast<UMaterial>(Instance->Parent))
		for (UMaterialExpression* Expression : CurrentParent->GetExpressions())
			if (const auto* CurrentSwitch = Cast<UMaterialExpressionStaticSwitchParameter>(Expression))
				TestEqual(TEXT("parent native switch identity preserved"), CurrentSwitch->ParameterName.ToString(), FString(TEXT("Enabled")));
	TestEqual(TEXT("exact native static override count"), NativeSwitches.StaticSwitchParameters.Num(), 1);
	for (const auto& NativeSwitch : NativeSwitches.StaticSwitchParameters)
		TestEqual(TEXT("native static override identity"), NativeSwitch.ParameterInfo.Name.ToString(), FString(TEXT("Enabled")));
	TArray<TSharedPtr<FJsonValue>> CapturedAfterTypes;
	if (!UeAgentMaterialAuthoring::CaptureInstanceParameters(*Instance, CapturedAfterTypes, Error)) { AddError(Error); return false; }
	int32 CapturedSwitchCount = 0;
	for (const auto& Parameter : CapturedAfterTypes)
		if (Parameter->AsObject()->GetStringField(TEXT("parameter_kind")) == TEXT("static_switch"))
		{
			++CapturedSwitchCount;
			TestEqual(TEXT("captured static switch identity"), Parameter->AsObject()->GetStringField(TEXT("parameter_name")), FString(TEXT("Enabled")));
		}
	TestEqual(TEXT("exact captured static switch count"), CapturedSwitchCount, 1);
	TestTrue(TEXT("root property persists with parameters"), static_cast<bool>(Instance->bOverrideSubsurfaceProfile));
	Instance->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Amount")), Amount);
	Instance->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Preserve")), Preserve);
	TestTrue(TEXT("other parameter overrides preserved"), FMath::IsNearlyEqual(Amount, 0.65f) && FMath::IsNearlyEqual(Preserve, 0.8f));
	float Inherited = 0;
	TestTrue(TEXT("non-target inheritance remains effective"), Instance->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Inherited")), Inherited) && FMath::IsNearlyEqual(Inherited, 0.5f));
	TestFalse(TEXT("non-target inheritance is not turned into an override"), Instance->ScalarParameterValues.ContainsByPredicate([](const auto& P) { return P.ParameterInfo.Name == TEXT("Inherited"); }));
	TestTrue(TEXT("detached local parameter identity and value remain intact"), Instance->ScalarParameterValues.ContainsByPredicate([&](const auto& P) { return P.ParameterInfo == OrphanInfo && FMath::IsNearlyEqual(P.ParameterValue, 0.125f); }));
	TestTrue(TEXT("native type independent reexport"), Export(Workspace + TEXT("-readback")));
	Workspace += TEXT("-clear-reject");
	if (!TestTrue(TEXT("clear rejection fresh workspace"), Export(Workspace))) { AddError(Error); return false; }
	EditedName = TEXT("Amount"); EditedValue = MakeShared<FJsonValueNumber>(0.65); bEditedOverride = false;
	if (!TestTrue(TEXT("explicit clear intent"), WriteVisibleEdit())) return false;
	FFileHelper::LoadFileToArray(BeforeBytes, *InstanceFilename);
	Instance = nullptr;
	auto ClearRejected = Import();
	TestFalse(TEXT("unsupported clear fails explicitly"), ClearRejected.IsOk());
	TestTrue(TEXT("clear rejection identifies unsupported native contract"), ClearRejected.GetErrorJson().Contains(TEXT("material_instance_clear_override_unsupported")));
	Instance = LoadObject<UMaterialInstanceConstant>(nullptr, *ObjectPath);
	Amount = 0;
	TestTrue(TEXT("clear rejection preserves effective parameter"), Instance && Instance->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Amount")), Amount) && FMath::IsNearlyEqual(Amount, 0.65f));
	TestTrue(TEXT("clear rejection preserves local override"), Instance && Instance->ScalarParameterValues.ContainsByPredicate([](const auto& P) { return P.ParameterInfo.Name == TEXT("Amount"); }));
	FFileHelper::LoadFileToArray(AfterBytes, *InstanceFilename); TestTrue(TEXT("clear rejection preserves disk"), BeforeBytes == AfterBytes);
	Instance = nullptr;
	return !HasAnyErrors();
}

namespace
{
	struct FT208FunctionFixture
	{
		UMaterialFunction* Function = nullptr;
		UMaterialExpressionConstant* Constant = nullptr;
		UMaterialExpressionConstant* Preserve = nullptr;
		UMaterialExpressionFunctionOutput* Output = nullptr;
	};

	bool PrepareT208FunctionFixture(const FString& PackageName, FT208FunctionFixture& Fixture, FString& Error)
	{
		// Preparation is test-only and never replaces an existing asset/package.
		if (FPackageName::DoesPackageExist(PackageName) || FindPackage(nullptr, *PackageName))
		{ Error = TEXT("function_fixture_already_exists"); return false; }
		UPackage* Package = CreatePackage(*PackageName);
		Fixture.Function = NewObject<UMaterialFunction>(Package, *FPackageName::GetLongPackageAssetName(PackageName), RF_Public | RF_Standalone | RF_Transactional);
		bool bPrepared = false;
		ON_SCOPE_EXIT
		{
			if (!bPrepared)
			{
				TGuardValue<bool> Guard(GIsRunningUnattendedScript, true);
				ObjectTools::DeleteObjects({Fixture.Function}, false);
				Fixture = {};
			}
		};
		Fixture.Constant = Cast<UMaterialExpressionConstant>(UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Fixture.Function, UMaterialExpressionConstant::StaticClass(), 120, 230));
		Fixture.Preserve = Cast<UMaterialExpressionConstant>(UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Fixture.Function, UMaterialExpressionConstant::StaticClass(), 420, 530));
		Fixture.Output = Cast<UMaterialExpressionFunctionOutput>(UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Fixture.Function, UMaterialExpressionFunctionOutput::StaticClass(), 640, 230));
		if (!Fixture.Constant || !Fixture.Preserve || !Fixture.Output)
		{ Error = TEXT("function_fixture_expression_creation_failed"); return false; }
		Fixture.Constant->R = 0.2f;
		Fixture.Preserve->R = 0.875f; Fixture.Preserve->Desc = TEXT("non-target function expression");
		Fixture.Output->OutputName = TEXT("Result"); Fixture.Output->Description = TEXT("stable function output"); Fixture.Output->SortPriority = 3;
		Fixture.Output->ConditionallyGenerateId(false);
		Fixture.Function->Description = TEXT("independent function preserve settings"); Fixture.Function->bExposeToLibrary = true;
		if (!UMaterialEditingLibrary::ConnectMaterialExpressions(Fixture.Constant, FString(), Fixture.Output, FString()))
		{ Error = TEXT("function_fixture_connection_failed"); return false; }
		UMaterialEditingLibrary::UpdateMaterialFunction(Fixture.Function, nullptr);
		FAssetRegistryModule::AssetCreated(Fixture.Function);
		FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone; Save.SaveFlags = SAVE_NoError;
		const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		if (!UPackage::SavePackage(Package, Fixture.Function, *Filename, Save))
		{ Error = TEXT("function_fixture_save_failed"); return false; }
		bPrepared = true;
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208MaterialFunctionWorkspace,
	"UAI.Root.T208.MaterialFunction.Workspace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208MaterialFunctionWorkspace::RunTest(const FString&)
{
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Tests/T208/Function_") + Token;
	const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageName);
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-material-function/automation") / Token);
	AddInfo(TEXT("independent Function workspace: ") + Workspace);
	// Only the fixture uses native creation; all tested edits use the public workspace.
	FT208FunctionFixture Fixture;
	FString FixtureError;
	if (!TestTrue(TEXT("native independent function fixture persists"), PrepareT208FunctionFixture(PackageName, Fixture, FixtureError)))
	{ AddError(FixtureError); return false; }
	auto* Function = Fixture.Function;
	auto* Constant = Fixture.Constant;
	auto* Preserve = Fixture.Preserve;
	auto* Output = Fixture.Output;
	const FGuid ConstantId = Constant->MaterialExpressionGuid, PreserveId = Preserve->MaterialExpressionGuid;
	const FGuid OutputId = Output->MaterialExpressionGuid, OutputBinding = Output->Id, BeforeStateId = Function->StateId;
	ON_SCOPE_EXIT
	{
		TGuardValue<bool> Guard(GIsRunningUnattendedScript, true);
		if (UObject* Owned = LoadObject<UObject>(nullptr, *ObjectPath))
			TestEqual(TEXT("owned Function cleanup completes"), ObjectTools::DeleteObjects({Owned}, false), 1);
		TestFalse(TEXT("owned function fixture cleaned from disk"), IFileManager::Get().FileExists(*Filename));
	};
	Constant = nullptr; Preserve = nullptr; Output = nullptr; Function = nullptr; Fixture = {};
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
	auto Export = [&](const FString& Path)
	{
		auto Params = MakeShared<FJsonObject>(); Params->SetStringField(TEXT("target"), ObjectPath);
		Params->SetStringField(TEXT("workspace_path"), Path); Params->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		FString Json; if (!CommandContract::Json::TryCanonicalize(Params, Json, Error)) return false;
		auto Result = SubmitPublicExport(Registry, Dispatch, Json);
		if (!Result.IsOk()) Error = Result.GetErrorJson();
		return Result.IsOk();
	};
	auto Import = [&]()
	{
		auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Workspace);
		auto Params = MakeShared<FJsonObject>(); Params->SetStringField(TEXT("target"), ObjectPath); Params->SetObjectField(TEXT("source"), Source);
		FString Json; CommandContract::Json::TryCanonicalize(Params, Json, Error);
		return SubmitPublicImport(Registry, Dispatch, Json);
	};
	if (!TestTrue(TEXT("independent function ordinary export"), Export(Workspace))) { AddError(Error); return false; }
	TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Workspace, TEXT("*.hjson"), true, false);
	FString GraphFile; TSharedPtr<FJsonObject> Graph;
	bool bEditedConstant = false;
	TFunction<bool(const TSharedPtr<FJsonObject>&)> EditValue = [&](const auto& Document)
	{
		const TSharedPtr<FJsonObject>* R = nullptr;
		if (Document->TryGetObjectField(TEXT("R"), R))
		{
			double Number;
			if ((*R)->TryGetNumberField(TEXT("value"), Number) && FMath::IsNearlyEqual(Number, 0.2, 1.e-5))
			{ (*R)->SetNumberField(TEXT("value"), 0.4); return true; }
		}
		for (const auto& Field : Document->Values)
			if (Field.Value->Type == EJson::Object && EditValue(Field.Value->AsObject())) return true;
		return false;
	};
	auto Write = [&](const FString& File, const TSharedPtr<FJsonObject>& Document)
	{
		FString Text; return CommandContract::Json::TryCanonicalize(Document, Text, Error)
			&& FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	};
	for (const auto& File : Files)
	{
		if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
		FString Text; TSharedPtr<FJsonObject> Document;
		if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) continue;
		if (Document->HasField(TEXT("nodes")) && Document->HasField(TEXT("connections"))) { Graph = Document; GraphFile = File; }
		if (!bEditedConstant && File.EndsWith(TEXT("properties.hjson")) && EditValue(Document)) bEditedConstant = Write(File, Document);
	}
	if (!TestTrue(TEXT("function exposes native scalar property"), bEditedConstant) || !TestTrue(TEXT("function exposes ordinary graph"), Graph.IsValid())) return false;
	const auto Nodes = Graph->GetObjectField(TEXT("nodes"));
	for (const auto& Node : Nodes->Values)
		TestFalse(TEXT("independent function has no Material root surrogate"), Node.Value->AsObject()->GetStringField(TEXT("name")) == TEXT("MaterialOutput"));
	const auto Connections = Graph->GetArrayField(TEXT("connections"));
	if (!TestEqual(TEXT("fixture has one ordinary function connection"), Connections.Num(), 1)) return false;
	const FString ConstantEndpoint = Connections[0]->AsObject()->GetStringField(TEXT("from"));
	const FString OutputEndpoint = Connections[0]->AsObject()->GetStringField(TEXT("to"));
	auto MultiplyNode = MakeShared<FJsonObject>(); MultiplyNode->SetStringField(TEXT("name"), TEXT("Multiply"));
	auto Candidate = MakeShared<FJsonObject>(); Candidate->SetStringField(TEXT("name"), TEXT("Multiply")); MultiplyNode->SetObjectField(TEXT("candidate"), Candidate);
	Nodes->SetObjectField(TEXT("Scale"), MultiplyNode);
	auto Link = [](const FString& From, const FString& To)
	{
		auto Value = MakeShared<FJsonObject>(); Value->SetStringField(TEXT("from"), From); Value->SetStringField(TEXT("to"), To);
		return MakeShared<FJsonValueObject>(Value);
	};
	// Invalid input is resolved against the actual Function graph, before any edit.
	Graph->SetArrayField(TEXT("connections"), {Link(ConstantEndpoint, TEXT("Scale.NotAnInput")), Link(TEXT("Scale.Output"), OutputEndpoint)});
	if (!Write(GraphFile, Graph)) return false;
	TArray<uint8> BeforeBytes, AfterBytes; FFileHelper::LoadFileToArray(BeforeBytes, *Filename);
	Function = LoadObject<UMaterialFunction>(nullptr, *ObjectPath);
	const bool bBeforeDirty = Function->GetOutermost()->IsDirty();
	Function = nullptr;
	auto Rejected = Import();
	TestFalse(TEXT("invalid function pin rejects"), Rejected.IsOk());
	Function = LoadObject<UMaterialFunction>(nullptr, *ObjectPath);
	TestEqual(TEXT("preflight preserves function node count"), Function->GetExpressions().Num(), 3);
	TestEqual(TEXT("preflight preserves function dirty state"), Function->GetOutermost()->IsDirty(), bBeforeDirty);
	for (UMaterialExpression* Expression : Function->GetExpressions())
		if (Expression->MaterialExpressionGuid == ConstantId) TestEqual(TEXT("rejected combined edit preserves scalar"), CastChecked<UMaterialExpressionConstant>(Expression)->R, 0.2f);
	FFileHelper::LoadFileToArray(AfterBytes, *Filename); TestTrue(TEXT("preflight preserves function disk"), BeforeBytes == AfterBytes);
	Graph->SetArrayField(TEXT("connections"), {Link(ConstantEndpoint, TEXT("Scale.A")), Link(TEXT("Scale.Output"), OutputEndpoint)});
	if (!Write(GraphFile, Graph)) return false;
	Function = nullptr;
	auto Applied = Import();
	if (!TestTrue(TEXT("independent function ordinary import applies"), Applied.IsOk())) { AddError(Applied.GetErrorJson()); return false; }
	if (UPackage* Published = FindPackage(nullptr, *PackageName))
	{
		FText UnloadError;
		if (!TestTrue(TEXT("Function package independently unloads after import"), UPackageTools::UnloadPackages({Published}, UnloadError)))
		{ AddError(UnloadError.ToString()); return false; }
	}
	Function = LoadObject<UMaterialFunction>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("function persists and reloads independently"), Function)) return false;
	UMaterialExpressionMultiply* Multiply = nullptr;
	for (UMaterialExpression* Expression : Function->GetExpressions())
	{
		if (Expression->MaterialExpressionGuid == ConstantId) Constant = Cast<UMaterialExpressionConstant>(Expression);
		if (Expression->MaterialExpressionGuid == PreserveId) Preserve = Cast<UMaterialExpressionConstant>(Expression);
		if (Expression->MaterialExpressionGuid == OutputId) Output = Cast<UMaterialExpressionFunctionOutput>(Expression);
		if (auto* Found = Cast<UMaterialExpressionMultiply>(Expression)) Multiply = Found;
	}
	if (!Constant || !Preserve || !Output || !Multiply) { AddError(TEXT("persisted function expressions missing")); return false; }
	TestEqual(TEXT("function native node addition persists"), Function->GetExpressions().Num(), 4);
	TestEqual(TEXT("function scalar edit persists"), Constant->R, 0.4f);
	TestTrue(TEXT("native function connections persist"), Multiply->A.Expression == Constant && Output->A.Expression == Multiply);
	TestTrue(TEXT("native function update invalidates compilation state"), Function->StateId.IsValid() && Function->StateId != BeforeStateId);
	TestEqual(TEXT("function description preserved"), Function->Description, FString(TEXT("independent function preserve settings")));
	TestTrue(TEXT("function library exposure preserved"), static_cast<bool>(Function->bExposeToLibrary));
	TestEqual(TEXT("unrelated function expression value preserved"), Preserve->R, 0.875f);
	TestEqual(TEXT("unrelated function expression description preserved"), Preserve->Desc, FString(TEXT("non-target function expression")));
	TestTrue(TEXT("unrelated function expression position preserved"), Preserve->MaterialExpressionEditorX == 420 && Preserve->MaterialExpressionEditorY == 530);
	TestTrue(TEXT("function output native identity preserved"), Output->Id == OutputBinding && Output->OutputName == TEXT("Result") && Output->SortPriority == 3);
	TestFalse(TEXT("persisted function is clean"), Function->GetOutermost()->IsDirty());
	if (!TestTrue(TEXT("independent function ordinary reexport"), Export(Workspace + TEXT("-readback")))) { AddError(Error); return false; }
	// The standalone function's native transient preview compiles its real output;
	// no Material asset or FunctionCall authoring path is used as a substitute.
	UMaterialFunctionInterface* NativeFunction = Function;
	auto* Preview = Cast<UMaterial>(NativeFunction->GetPreviewMaterial());
	if (!TestTrue(TEXT("native function preview exists and is transient"), Preview && Preview->HasAnyFlags(RF_Transient))) return false;
	if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
	const FMaterialResource* Resource = Preview->GetMaterialResource(GMaxRHIFeatureLevel);
	if (!TestNotNull(TEXT("native function output compilation resource"), Resource)) return false;
	TestEqual(TEXT("native function output has no compile errors"), Resource->GetCompileErrors().Num(), 0);
	TestTrue(TEXT("native function output shader map completed"), Resource->GetGameThreadShaderMap() != nullptr);
	Constant = nullptr; Preserve = nullptr; Output = nullptr; Multiply = nullptr; Function = nullptr;
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208MaterialFunctionPrepareSdkFixture,
	"UAI.Root.T208.MaterialFunction.PrepareSdkFixture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208MaterialFunctionPrepareSdkFixture::RunTest(const FString&)
{
	const FString PackageName = TEXT("/Game/UAI/Tests/T208/FunctionSdkFixture");
	FT208FunctionFixture Fixture;
	FString Error;
	if (!PrepareT208FunctionFixture(PackageName, Fixture, Error)) { AddError(Error); return false; }
	FT208FunctionFixture Existing;
	TestFalse(TEXT("SDK fixture preparation refuses existing asset"), PrepareT208FunctionFixture(PackageName, Existing, Error));
	TestEqual(TEXT("existing fixture refusal is explicit"), Error, FString(TEXT("function_fixture_already_exists")));
	const FString Record = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-material-function/sdk-fixture.json"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Record), true);
	auto Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), PackageName);
	Data->SetStringField(TEXT("object_path"), Fixture.Function->GetPathName());
	FString Json;
	if (!CommandContract::Json::TryCanonicalize(Data, Json, Error)
		|| !FFileHelper::SaveStringToFile(Json, *Record, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		TGuardValue<bool> Guard(GIsRunningUnattendedScript, true);
		ObjectTools::DeleteObjects({Fixture.Function}, false);
		AddError(TEXT("function_sdk_fixture_record_failed")); return false;
	}
	AddInfo(TEXT("native SDK fixture retained: ") + Fixture.Function->GetPathName());
	return true;
}
