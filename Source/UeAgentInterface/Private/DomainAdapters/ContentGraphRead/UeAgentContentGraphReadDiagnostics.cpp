#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Domains/Niagara/UeAgentNiagaraCompileValidation.h"
#include "Domains/Niagara/UeAgentNiagaraStackValidation.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Editor/IPCGEditorModule.h"
#include "IMaterialEditor.h"
#include "Logging/TokenizedMessage.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialInterface.h"
#include "MaterialShared.h"
#include "NiagaraEmitter.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "PCGComponent.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "Toolkits/ToolkitManager.h"
#include "UObject/UObjectHash.h"
#include "Utils/PCGNodeVisualLogs.h"

namespace UE::UAI::DomainAdapters::ContentGraphRead
{
	namespace
	{
		class FDiagnosticCollector
		{
		public:
			FDiagnosticCollector(FUeAgentContentGraphCapture& InCapture, FUeAgentContentGraphReadError& InError)
				: Capture(InCapture), Error(InError) {}

			void AddSource(const FString& Source, const FString& Status, const FString& Scope,
				const FString& Context, const FString& StateOwner = FString())
			{
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("source"), Source);
				Row->SetStringField(TEXT("status"), Status);
				Row->SetStringField(TEXT("scope"), Scope);
				Row->SetStringField(TEXT("context"), Context);
				Row->SetStringField(TEXT("owner"), StateOwner.IsEmpty() ? Capture.Owner->GetPathName() : StateOwner);
				Sources.Add(MakeShared<FJsonValueObject>(Row));
			}

			bool Add(const FString& Source, const FString& Owner, const FString& Severity,
				const FString& Code, const FString& Message, TSharedPtr<FJsonObject> Details = nullptr)
			{
				FUeAgentContentGraphDiagnostic Row;
				Row.DiagnosticId = FString::Printf(TEXT("%s:%s:%d"), *Source, *Owner, Capture.Diagnostics.Num());
				Row.OwnerId = Owner;
				Row.Severity = Severity;
				Row.Code = Code;
				Row.Message = Message;
				TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
				Value->SetStringField(TEXT("kind"), TEXT("diagnostic"));
				Value->SetStringField(TEXT("source"), Source);
				Value->SetStringField(TEXT("owner_id"), Owner);
				FString PackagePath, Location;
				if (!Owner.Split(TEXT("."), &PackagePath, &Location)) Location = Owner;
				Value->SetStringField(TEXT("location"), Location);
				Value->SetStringField(TEXT("severity"), Severity);
				Value->SetStringField(TEXT("code"), Code);
				Value->SetStringField(TEXT("message"), Message);
				if (Details.IsValid() && !Details->Values.IsEmpty()) Value->SetObjectField(TEXT("details"), Details);
				if (!Canonical(Value, Row.State)) return false;
				Capture.Diagnostics.Add(MoveTemp(Row));
				return true;
			}

			bool Finish()
			{
				if (Sources.IsEmpty()) AddSource(TEXT("native_diagnostics"), TEXT("unavailable"),
					TEXT("No native diagnostic source is exposed for this asset type."), TEXT("unexposed_native_source"));
				TSharedRef<FJsonObject> Coverage = MakeShared<FJsonObject>();
				Coverage->SetStringField(TEXT("kind"), TEXT("coverage"));
				Coverage->SetStringField(TEXT("scope"), TEXT("current_native_state"));
				Coverage->SetBoolField(TEXT("compile_requested"), false);
				Coverage->SetArrayField(TEXT("sources"), Sources);
				return Canonical(Coverage, Capture.DiagnosticCoverage);
			}

		private:
			bool Canonical(const TSharedRef<FJsonObject>& Value, StateSemantic::FUeAgentCanonicalValuePtr& Out)
			{
				UE::UAI::Contract::FDiagnostic Detail;
				if (StateSemantic::CanonicalJson::TryFromJsonValue(
					MakeShared<FJsonValueObject>(Value), Resources, Out, Detail)) return true;
				Error = MoveTemp(Detail);
                if (Error.SubjectId.IsEmpty()) Error.SubjectId = Capture.Owner->GetPathName();
				if (Error.TargetPath.IsEmpty() && Capture.Owner->IsAsset()) Error.TargetPath = Capture.Owner->GetPathName();
				if (Error.SourceKind.IsEmpty()) Error.SourceKind = TEXT("native_result");
				return false;
			}

			FUeAgentContentGraphCapture& Capture;
			FUeAgentContentGraphReadError& Error;
			ContextResource::FUeAgentResourceTracker Resources;
			TArray<TSharedPtr<FJsonValue>> Sources;
		};

		bool CaptureNiagaraDiagnostics(UObject& Owner, FDiagnosticCollector& Collector)
		{
			const UeAgentNiagaraCompileValidation::FResult Compile =
				UeAgentNiagaraCompileValidation::InspectAsset(&Owner, true);
			Collector.AddSource(TEXT("niagara_compile"),
				!Compile.bStoredCompileStateApplicable ? TEXT("not_applicable")
				: !Compile.bCompilationComplete ? TEXT("pending")
				: !Compile.UncompiledTargets.IsEmpty() ? TEXT("stale") : TEXT("observed"),
				Compile.bStoredCompileStateApplicable
					? TEXT("Stored compile state of the asset and enabled emitter scripts.")
					: TEXT("This definition has no independent stored VM freshness; compilation is evaluated in its native System context. Stored messages remain available."),
				Compile.bStoredCompileStateApplicable ? TEXT("stored_vm") : TEXT("source_definition"));
			for (const UeAgentNiagaraCompileValidation::FDiagnostic& Item : Compile.Diagnostics)
			{
				TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
				for (const auto& SourceValue : Item.RelatedSources)
				{
					const auto Source = SourceValue.IsValid() ? SourceValue->AsObject() : nullptr;
					FString Value;
					if (Source && Source->TryGetStringField(TEXT("node_guid"), Value)) Details->SetStringField(TEXT("node_id"), Value);
					if (Source && Source->TryGetStringField(TEXT("pin_guid"), Value)) Details->SetStringField(TEXT("pin_id"), Value);
				}
				const FString Severity = Item.Severity == UE::UAI::Contract::ESeverity::Error ? TEXT("error") : TEXT("warning");
				if (!Collector.Add(TEXT("niagara_compile"), Item.TargetPath, Severity,
					TEXT("niagara_compile_message"), Item.PublicMessage, Details)) return false;

			}

			if (Owner.IsA<UNiagaraScript>())
			{
				Collector.AddSource(TEXT("niagara_stack"), TEXT("not_applicable"),
					TEXT("Standalone scripts have no system/emitter Stack."), TEXT("source_definition"));
				return true;
			}
			const UeAgentNiagaraStackValidation::FResult Stack =
				UeAgentNiagaraStackValidation::InspectAsset(&Owner, true);
			Collector.AddSource(TEXT("niagara_stack"),
				Stack.bInspectionComplete ? TEXT("observed") : TEXT("unavailable"),
				Stack.bInspectionComplete ? TEXT("Current native system/emitter Stack entries.") : Stack.InspectionDiagnostic.PublicMessage, TEXT("native_stack"));
			for (const UeAgentNiagaraStackValidation::FIssue& Issue : Stack.Issues)
			{
				TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
				Details->SetStringField(TEXT("issue_id"), Issue.Id);
				Details->SetStringField(TEXT("entry_key"), Issue.EntryKey);
				Details->SetStringField(TEXT("entry_name"), Issue.EntryName);
				if (!Issue.EmitterName.IsEmpty()) Details->SetStringField(TEXT("emitter"), Issue.EmitterName);
				if (!Issue.Diagnostic.PublicMessage.IsEmpty()) Details->SetStringField(TEXT("description"), Issue.Diagnostic.PublicMessage);
				if (!Issue.EmitterHandleId.IsEmpty())
					Details->SetStringField(TEXT("emitter_handle_id"), Issue.EmitterHandleId);
				TArray<TSharedPtr<FJsonValue>> Fixes;
				for (const UeAgentNiagaraStackValidation::FFix& Fix : Issue.Fixes)
				{
					TSharedRef<FJsonObject> FixRow = MakeShared<FJsonObject>();
					FixRow->SetStringField(TEXT("fix_id"), Fix.Id);
					FixRow->SetStringField(TEXT("description"), Fix.Description);
					Fixes.Add(MakeShared<FJsonValueObject>(FixRow));
				}
				if (!Fixes.IsEmpty()) Details->SetArrayField(TEXT("fixes"), Fixes);
				if (!Collector.Add(TEXT("niagara_stack"), Owner.GetPathName(), Issue.Diagnostic.Severity == UE::UAI::Contract::ESeverity::Error ? TEXT("error") : Issue.Diagnostic.Severity == UE::UAI::Contract::ESeverity::Warning ? TEXT("warning") : TEXT("info"),
					TEXT("niagara_stack_issue"), Issue.Diagnostic.PublicMessage, Details)) return false;
			}
			return true;
		}

		bool CaptureMaterialDiagnostics(UObject& Owner, FDiagnosticCollector& Collector)
		{
			UMaterialInterface* Material = Cast<UMaterialInterface>(&Owner);
			if (!Material && Owner.IsA<UMaterialFunctionInterface>())
			{
				const TSharedPtr<IToolkit> Toolkit = FToolkitManager::Get().FindEditorForAsset(&Owner);
				if (Toolkit.IsValid() && Toolkit->GetToolkitFName() == TEXT("MaterialEditor"))
					Material = StaticCastSharedPtr<IMaterialEditor>(Toolkit)->GetMaterialInterface();
			}
			if (!Material)
			{
				Collector.AddSource(TEXT("material_compile"), TEXT("unavailable"),
					TEXT("No existing native material preview resource; this query does not open an editor or compile."), TEXT("preview_material_resource"));
				return true;
			}
			TSet<const FMaterialResource*> Seen;
			for (int32 Feature = 0; Feature < ERHIFeatureLevel::Num; ++Feature)
			{
				for (int32 Quality = 0; Quality < EMaterialQualityLevel::Num; ++Quality)
				{
					const FMaterialResource* Resource = Material->GetMaterialResource(
						static_cast<ERHIFeatureLevel::Type>(Feature), static_cast<EMaterialQualityLevel::Type>(Quality));
					if (!Resource || Seen.Contains(Resource)) continue;
					Seen.Add(Resource);
					const FString Scope = FString::Printf(TEXT("Existing resource: feature=%d, quality=%d; %s"),
						static_cast<int32>(Resource->GetFeatureLevel()), static_cast<int32>(Resource->GetQualityLevel()), *Material->GetPathName());
					const UMaterialInterface* ResourceOwner = Resource->GetMaterialInterface();
					Collector.AddSource(TEXT("material_compile"), Resource->IsCompilationFinished()
						? TEXT("observed") : TEXT("pending"), Scope,
						Owner.IsA<UMaterialFunctionInterface>() ? TEXT("preview_material_resource") : TEXT("material_resource"),
						ResourceOwner ? ResourceOwner->GetPathName() : Material->GetPathName());
					TArray<TSharedPtr<FJsonValue>> Expressions;
					for (const UMaterialExpression* Expression : Resource->GetErrorExpressions())
						if (Expression) Expressions.Add(MakeShared<FJsonValueString>(Expression->GetPathName()));
					for (const FString& Message : Resource->GetCompileErrors())
					{
						TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
						Details->SetNumberField(TEXT("feature_level"), static_cast<int32>(Resource->GetFeatureLevel()));
						Details->SetNumberField(TEXT("quality_level"), static_cast<int32>(Resource->GetQualityLevel()));
						if (!Expressions.IsEmpty()) Details->SetArrayField(TEXT("error_expressions"), Expressions);
						if (!Collector.Add(TEXT("material_compile"), Owner.GetPathName(), TEXT("error"),
							TEXT("material_compile_error"), Message, Details)) return false;
					}
				}
			}
			if (Seen.IsEmpty()) Collector.AddSource(TEXT("material_compile"), TEXT("unavailable"),
				TEXT("No compiled material resource exists for this asset."), TEXT("material_resource"));
			return true;
		}

		bool CapturePcgDiagnostics(UPCGGraph& Graph, FDiagnosticCollector& Collector)
		{
			const IPCGEditorModule* Module = IPCGEditorModule::Get();
			Collector.AddSource(TEXT("pcg_execution"), Module ? TEXT("observed") : TEXT("unavailable"),
				TEXT("Retained native per-node logs from prior component executions; empty logs do not prove an execution occurred."), TEXT("retained_component_execution"));
			if (!Module) return true;
			TArray<const UPCGNode*> Nodes;
			Nodes.AddUnique(Graph.GetInputNode());
			Nodes.AddUnique(Graph.GetOutputNode());
			for (const UPCGNode* Node : Graph.GetNodes()) Nodes.AddUnique(Node);
			for (const UPCGNode* Node : Nodes)
			{
				if (!Node) continue;
				FPCGPerNodeVisualLogs Logs;
				TArray<const UPCGComponent*> Components;
				Module->GetNodeVisualLogs().GetLogs(Node, Logs, Components);
				for (int32 Index = 0; Index < Logs.Num(); ++Index)
				{
					const FPCGNodeLogEntry& Log = Logs[Index];
					TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
					if (Components.IsValidIndex(Index) && Components[Index])
						Details->SetStringField(TEXT("component"), Components[Index]->GetPathName());
					if (!Collector.Add(TEXT("pcg_execution"), Node->GetPathName(),
						Log.Verbosity <= ELogVerbosity::Error ? TEXT("error")
						: Log.Verbosity == ELogVerbosity::Warning ? TEXT("warning") : TEXT("info"),
						TEXT("pcg_node_execution_message"), Log.Message.ToString(), Details)) return false;
				}
			}
			return true;
		}

		bool CaptureGraphNodeDiagnostics(UObject& Owner, FDiagnosticCollector& Collector)
		{
			TArray<UObject*> Objects;
			GetObjectsWithOuter(&Owner, Objects, true);
			Objects.Sort([](const UObject& A, const UObject& B) { return A.GetPathName() < B.GetPathName(); });
			bool bHasNodes = false;
			for (UObject* Object : Objects)
			{
				const UEdGraphNode* Node = Cast<UEdGraphNode>(Object);
				if (!IsValid(Node)) continue;
				bHasNodes = true;
				if (!Node->bHasCompilerMessage || Node->ErrorMsg.IsEmpty()) continue;
				const FString Severity = Node->ErrorType <= EMessageSeverity::Error ? TEXT("error")
					: (Node->ErrorType == EMessageSeverity::Warning || Node->ErrorType == EMessageSeverity::PerformanceWarning)
					? TEXT("warning") : TEXT("info");
				if (!Collector.Add(TEXT("graph_node"), Node->GetPathName(), Severity,
					TEXT("native_graph_node_message"), Node->ErrorMsg)) return false;
			}
			if (bHasNodes) Collector.AddSource(TEXT("graph_node"), TEXT("observed"),
				TEXT("Stored messages on existing native graph nodes; does not execute or compile the graph."), TEXT("stored_node_messages"));
			return true;
		}
	}

	bool FUeAgentContentGraphReadProductionEnvironment::TryCaptureDiagnostics(
		FUeAgentContentGraphCapture& Capture, FUeAgentContentGraphReadError& OutError)
	{
		Capture.Diagnostics.Reset();
		Capture.DiagnosticCoverage.Reset();
		if (!IsValid(Capture.Owner))
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("content_graph_read_owner_invalid"),
				TEXT("Native diagnostic capture requires a live owner object."), TEXT("content_capture"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("/owner");
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			return false;
		}
		FDiagnosticCollector Collector(Capture, OutError);
		UObject& Owner = *Capture.Owner;
		if (Owner.IsA<UNiagaraSystem>() || Owner.IsA<UNiagaraEmitter>() || Owner.IsA<UNiagaraScript>())
		{
			if (!CaptureNiagaraDiagnostics(Owner, Collector)) return false;
		}
		if (!CaptureGraphNodeDiagnostics(Owner, Collector)) return false;
		if ((Owner.IsA<UMaterialInterface>() || Owner.IsA<UMaterialFunctionInterface>())
			&& !CaptureMaterialDiagnostics(Owner, Collector)) return false;
		if (UPCGGraph* PcgGraph = Cast<UPCGGraph>(&Owner))
			if (!CapturePcgDiagnostics(*PcgGraph, Collector)) return false;
		if (const UBlueprint* Blueprint = Cast<UBlueprint>(&Owner))
		{
			Collector.AddSource(TEXT("blueprint_compile"), UeAgentBlueprintAssetLifecycle::GetStoredCompileObservationStatus(*Blueprint),
				TEXT("Stored Blueprint compilation status."), TEXT("stored_blueprint"));
			if (Blueprint->Status == BS_Error || Blueprint->Status == BS_UpToDateWithWarnings)
			{
				if (!Collector.Add(TEXT("blueprint_compile"), Owner.GetPathName(),
					Blueprint->Status == BS_Error ? TEXT("error") : TEXT("warning"),
					TEXT("blueprint_compile_status"), Blueprint->Status == BS_Error
					? TEXT("The last Blueprint compilation failed.") : TEXT("The last Blueprint compilation reported warnings."))) return false;
			}
		}
		return Collector.Finish();
	}
}
