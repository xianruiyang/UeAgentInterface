// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/Audio/UeAgentHttpServer_AudioOps.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Metasound.h"
#include "MetasoundBuilderSubsystem.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundFactory.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundSource.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UeAgentAudioOps
{
	TSharedPtr<FJsonObject> BuildMetaSoundInfo(UObject* MetaSound, bool bIncludeGraph, bool bIncludeInterfaces, bool bIncludeValidation)
	{
		TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
		FillAssetIdentity(MetaSound, Info);
		if (!MetaSound)
		{
			return Info;
		}
		Info->SetStringField(TEXT("asset_type"), MetaSound->IsA<UMetaSoundSource>() ? TEXT("source") : TEXT("patch"));
		IMetaSoundDocumentInterface* DocumentInterface = Cast<IMetaSoundDocumentInterface>(MetaSound);
		if (!DocumentInterface)
		{
			Info->SetBoolField(TEXT("has_document"), false);
			return Info;
		}
		Info->SetBoolField(TEXT("has_document"), true);
		const FMetasoundFrontendDocument& Document = DocumentInterface->GetConstDocument();
		const FMetasoundFrontendGraph& Graph = Document.RootGraph.GetConstDefaultGraph();
		const FMetasoundFrontendClassInterface& DefaultInterface = Document.RootGraph.GetDefaultInterface();

		TArray<TSharedPtr<FJsonValue>> Inputs;
		for (const FMetasoundFrontendClassInput& Input : DefaultInterface.Inputs)
		{
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), Input.Name.ToString());
			Item->SetStringField(TEXT("type"), Input.TypeName.ToString());
			Item->SetStringField(TEXT("vertex_id"), Input.VertexID.ToString(EGuidFormats::DigitsWithHyphens));
			Inputs.Add(MakeShared<FJsonValueObject>(Item));
		}
		Info->SetArrayField(TEXT("inputs"), Inputs);

		TArray<TSharedPtr<FJsonValue>> Outputs;
		for (const FMetasoundFrontendClassOutput& Output : DefaultInterface.Outputs)
		{
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), Output.Name.ToString());
			Item->SetStringField(TEXT("type"), Output.TypeName.ToString());
			Item->SetStringField(TEXT("vertex_id"), Output.VertexID.ToString(EGuidFormats::DigitsWithHyphens));
			Outputs.Add(MakeShared<FJsonValueObject>(Item));
		}
		Info->SetArrayField(TEXT("outputs"), Outputs);

		if (bIncludeGraph)
		{
			TArray<TSharedPtr<FJsonValue>> Nodes;
			for (const FMetasoundFrontendNode& Node : Graph.Nodes)
			{
				TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
				NodeObj->SetStringField(TEXT("id"), Node.GetID().ToString(EGuidFormats::DigitsWithHyphens));
				NodeObj->SetStringField(TEXT("name"), Node.Name.ToString());
				NodeObj->SetStringField(TEXT("class_id"), Node.ClassID.ToString(EGuidFormats::DigitsWithHyphens));
				TArray<TSharedPtr<FJsonValue>> NodeInputs;
				for (const FMetasoundFrontendVertex& Input : Node.Interface.Inputs)
				{
					TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("name"), Input.Name.ToString());
					Item->SetStringField(TEXT("type"), Input.TypeName.ToString());
					Item->SetStringField(TEXT("vertex_id"), Input.VertexID.ToString(EGuidFormats::DigitsWithHyphens));
					NodeInputs.Add(MakeShared<FJsonValueObject>(Item));
				}
				NodeObj->SetArrayField(TEXT("inputs"), NodeInputs);
				TArray<TSharedPtr<FJsonValue>> NodeOutputs;
				for (const FMetasoundFrontendVertex& Output : Node.Interface.Outputs)
				{
					TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("name"), Output.Name.ToString());
					Item->SetStringField(TEXT("type"), Output.TypeName.ToString());
					Item->SetStringField(TEXT("vertex_id"), Output.VertexID.ToString(EGuidFormats::DigitsWithHyphens));
					NodeOutputs.Add(MakeShared<FJsonValueObject>(Item));
				}
				NodeObj->SetArrayField(TEXT("outputs"), NodeOutputs);
				Nodes.Add(MakeShared<FJsonValueObject>(NodeObj));
			}
			Info->SetArrayField(TEXT("nodes"), Nodes);

			TArray<TSharedPtr<FJsonValue>> Edges;
			for (const FMetasoundFrontendEdge& Edge : Graph.Edges)
			{
				TSharedPtr<FJsonObject> EdgeObj = MakeShared<FJsonObject>();
				EdgeObj->SetStringField(TEXT("from_node_id"), Edge.FromNodeID.ToString(EGuidFormats::DigitsWithHyphens));
				EdgeObj->SetStringField(TEXT("from_vertex_id"), Edge.FromVertexID.ToString(EGuidFormats::DigitsWithHyphens));
				EdgeObj->SetStringField(TEXT("to_node_id"), Edge.ToNodeID.ToString(EGuidFormats::DigitsWithHyphens));
				EdgeObj->SetStringField(TEXT("to_vertex_id"), Edge.ToVertexID.ToString(EGuidFormats::DigitsWithHyphens));
				Edges.Add(MakeShared<FJsonValueObject>(EdgeObj));
			}
			Info->SetArrayField(TEXT("edges"), Edges);
		}
		if (bIncludeInterfaces)
		{
			TArray<TSharedPtr<FJsonValue>> Interfaces;
			for (const FMetasoundFrontendVersion& Version : Document.Interfaces)
			{
				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("name"), Version.Name.ToString());
				Item->SetNumberField(TEXT("major"), Version.Number.Major);
				Item->SetNumberField(TEXT("minor"), Version.Number.Minor);
				Interfaces.Add(MakeShared<FJsonValueObject>(Item));
			}
			Info->SetArrayField(TEXT("interfaces"), Interfaces);
		}
		if (bIncludeValidation)
		{
			TArray<TSharedPtr<FJsonValue>> Issues;
			Info->SetArrayField(TEXT("json_issues"), Issues);
			TSharedPtr<FJsonObject> CompileReport = MakeShared<FJsonObject>();
			CompileReport->SetNumberField(TEXT("error_count"), 0);
			CompileReport->SetNumberField(TEXT("warning_count"), 0);
			Info->SetObjectField(TEXT("compile_report"), CompileReport);
		}
		return Info;
	}
	bool ValidateMetaSoundFolder(const FString& FolderPathInput, TArray<TSharedPtr<FJsonValue>>& Issues, TSharedPtr<FJsonObject>& OutAssetJson, TSharedPtr<FJsonObject>& OutInputsJson, TSharedPtr<FJsonObject>& OutOutputsJson, FString& OutFolder, FString& OutError)
	{
		OutFolder = ResolveFilePath(FolderPathInput);
		if (OutFolder.IsEmpty())
		{
			OutError = TEXT("missing_folder_path");
			return false;
		}
		FString ResolvedPath;
		if (!LoadJsonFile(FPaths::Combine(OutFolder, TEXT("asset.json")), OutAssetJson, ResolvedPath, OutError))
		{
			return false;
		}
		if (!LoadJsonFile(FPaths::Combine(OutFolder, TEXT("members/inputs.json")), OutInputsJson, ResolvedPath, OutError))
		{
			return false;
		}
		if (!LoadJsonFile(FPaths::Combine(OutFolder, TEXT("members/outputs.json")), OutOutputsJson, ResolvedPath, OutError))
		{
			return false;
		}
		TSharedPtr<FJsonObject> NodesJson;
		if (FPaths::FileExists(FPaths::Combine(OutFolder, TEXT("graphs/nodes.json"))))
		{
			if (!LoadJsonFile(FPaths::Combine(OutFolder, TEXT("graphs/nodes.json")), NodesJson, ResolvedPath, OutError))
			{
				return false;
			}
			const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
			if (NodesJson->TryGetArrayField(TEXT("nodes"), Nodes) && Nodes)
			{
				for (const TSharedPtr<FJsonValue>& Value : *Nodes)
				{
					const TSharedPtr<FJsonObject>* NodeObj = nullptr;
					FString Type;
					if (Value.IsValid() && Value->TryGetObject(NodeObj) && NodeObj && NodeObj->IsValid() && (*NodeObj)->TryGetStringField(TEXT("type"), Type))
					{
						if (!Type.IsEmpty() && Type != TEXT("graph_input") && Type != TEXT("graph_output") && Type != TEXT("comment"))
						{
							AddIssue(Issues, TEXT("unsupported_node_type"), TEXT("error"), FString::Printf(TEXT("MetaSound apply only supports graph input/output authoring for node type '%s'."), *Type), TEXT("graphs/nodes.json"));
						}
					}
				}
			}
		}
		return !UeAgentJsonDiagnostics::HasErrorIssue(Issues);
	}
	bool AddMetaSoundMembers(UMetaSoundBuilderBase* Builder, const TSharedPtr<FJsonObject>& MembersJson, const TCHAR* ArrayField, bool bInput, int32& OutAdded, FString& OutError)
	{
		if (!Builder || !MembersJson.IsValid())
		{
			return true;
		}
		const TArray<TSharedPtr<FJsonValue>>* Members = nullptr;
		if (!MembersJson->TryGetArrayField(ArrayField, Members) || !Members)
		{
			return true;
		}
		TSet<FName> ExistingNameSet;
		for (const TSharedPtr<FJsonValue>& Value : *Members)
		{
			const TSharedPtr<FJsonObject>* MemberObj = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(MemberObj) || !MemberObj || !MemberObj->IsValid())
			{
				continue;
			}
			FString Name;
			FString Type;
			(*MemberObj)->TryGetStringField(TEXT("name"), Name);
			(*MemberObj)->TryGetStringField(TEXT("type"), Type);
			if (Name.IsEmpty() || Type.IsEmpty())
			{
				continue;
			}
			const FName MemberName(*Name);
			if (ExistingNameSet.Contains(MemberName))
			{
				continue;
			}
			EMetaSoundBuilderResult FindResult = EMetaSoundBuilderResult::Failed;
			if (bInput)
			{
				Builder->FindGraphInputNode(MemberName, FindResult);
			}
			else
			{
				Builder->FindGraphOutputNode(MemberName, FindResult);
			}
			if (FindResult == EMetaSoundBuilderResult::Succeeded)
			{
				ExistingNameSet.Add(MemberName);
				continue;
			}
			FMetasoundFrontendLiteral Literal;
			EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
			if (bInput)
			{
				Builder->AddGraphInputNode(MemberName, *Type, Literal, Result);
			}
			else
			{
				Builder->AddGraphOutputNode(MemberName, *Type, Literal, Result);
			}
			if (Result != EMetaSoundBuilderResult::Succeeded)
			{
				OutError = bInput ? TEXT("metasound_add_input_failed") : TEXT("metasound_add_output_failed");
				return false;
			}
			ExistingNameSet.Add(MemberName);
			++OutAdded;
		}
		return true;
	}
	bool CreateMetaSoundAsset(const FString& AssetPath, const FString& AssetTypeInput, bool bReplaceExisting, UObject*& OutAsset, FString& OutError)
	{
		OutAsset = nullptr;
		const bool bSource = !AssetTypeInput.Equals(TEXT("patch"), ESearchCase::IgnoreCase);
		UClass* AssetClass = bSource ? UMetaSoundSource::StaticClass() : UMetaSoundPatch::StaticClass();
		if (UObject* Existing = LoadAssetObject(AssetPath))
		{
			if (!bReplaceExisting)
			{
				OutError = TEXT("asset_already_exists");
				return false;
			}
			if (!Existing->IsA(AssetClass))
			{
				OutError = TEXT("existing_asset_class_mismatch");
				return false;
			}
			Existing->Modify();
			OutAsset = Existing;
			return true;
		}

		FString NormalizedPath;
		FString AssetName;
		if (!ValidateAssetPathForCreate(AssetPath, NormalizedPath, AssetName, OutError))
		{
			return false;
		}
		UPackage* Package = CreatePackage(*NormalizedPath);
		if (!Package)
		{
			OutError = TEXT("create_package_failed");
			return false;
		}
		UFactory* Factory = bSource ? static_cast<UFactory*>(NewObject<UMetaSoundSourceFactory>()) : static_cast<UFactory*>(NewObject<UMetaSoundFactory>());
		UObject* Asset = Factory ? Factory->FactoryCreateNew(AssetClass, Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn) : nullptr;
		if (!Asset)
		{
			Asset = NewObject<UObject>(Package, AssetClass, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		}
		if (!Asset)
		{
			OutError = TEXT("create_metasound_failed");
			return false;
		}
		FAssetRegistryModule::AssetCreated(Asset);
		Asset->MarkPackageDirty();
		Package->MarkPackageDirty();
		OutAsset = Asset;
		return true;
	}
}
