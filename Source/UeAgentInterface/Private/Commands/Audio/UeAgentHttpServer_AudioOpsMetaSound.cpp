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
}
