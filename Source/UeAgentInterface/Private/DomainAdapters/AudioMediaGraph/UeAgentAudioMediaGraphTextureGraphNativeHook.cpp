#include "DomainAdapters/AudioMediaGraph/UeAgentAudioMediaGraphTextureGraphNativeHook.h"

#include "Blueprint/TG_AsyncRenderTask.h"
#include "Expressions/Input/TG_Expression_Scalar.h"
#include "Expressions/Output/TG_Expression_Output.h"
#include "Expressions/TG_Expression.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Job/JobBatch.h"
#include "Model/Mix/MixSettings.h"
#include "Model/Mix/MixUpdateCycle.h"
#include "TG_Graph.h"
#include "TG_GraphEvaluation.h"
#include "TG_Node.h"
#include "TG_Pin.h"
#include "TG_Variant.h"
#include "TextureGraph.h"
#include "RenderingThread.h"

namespace UE::UAI::DomainAdapters::AudioMediaGraph
{
	namespace
	{
		FString MakeTextureGraphEvidenceId(const TCHAR* Stage)
		{
			return FString::Printf(TEXT("texture-graph:%s:%s"), Stage,
				*FGuid::NewGuid().ToString(EGuidFormats::Digits));
		}

		FString DescribeVariantType(const FTG_Variant& Value)
		{
			if (Value.IsScalar())
			{
				return TEXT("scalar");
			}
			if (Value.IsColor())
			{
				return TEXT("color");
			}
			if (Value.IsVector())
			{
				return TEXT("vector");
			}
			if (Value.IsTexture())
			{
				return TEXT("texture");
			}
			return TEXT("variant");
		}

		UTG_Node* FindSemanticNode(UTG_Graph& Graph, const FString& ElementId)
		{
			UTG_Node* Result = nullptr;
			Graph.ForEachNodes(
				[&Result, &ElementId](const UTG_Node* Node, const uint32)
				{
					if (Result != nullptr || Node == nullptr
						|| Node->GetExpression() == nullptr)
					{
						return;
					}
					const bool bBuiltinOutput = ElementId == TEXT("builtin_output")
						&& Cast<UTG_Expression_Output>(Node->GetExpression()) != nullptr;
					if (bBuiltinOutput || Node->GetExpression()->GetName() == ElementId)
					{
						Result = const_cast<UTG_Node*>(Node);
					}
				});
			return Result;
		}

		bool TryApplyPinValue(
			UTG_Pin& Pin,
			const FAudioMediaGraphTextureGraphValue& Value)
		{
			switch (Value.GetKind())
			{
			case EAudioMediaGraphTextureGraphValueKind::Number:
				return Pin.SetValue(Value.GetNumber());
			case EAudioMediaGraphTextureGraphValueKind::Boolean:
				return Pin.SetValue(Value.GetBoolean());
			case EAudioMediaGraphTextureGraphValueKind::String:
				return Pin.SetValue(Value.GetString());
			case EAudioMediaGraphTextureGraphValueKind::NumberArray:
			{
				const TArray<double>& Values = Value.GetNumberArray();
				if (Values.Num() != 4)
				{
					return false;
				}
				const FName CppType = Pin.GetArgumentCPPTypeName();
				if (CppType.ToString().Contains(TEXT("LinearColor")))
				{
					return Pin.SetValue(FLinearColor(
						Values[0], Values[1], Values[2], Values[3]));
				}
				return Pin.SetValue(FVector4f(
					Values[0], Values[1], Values[2], Values[3]));
			}
			default:
				return false;
			}
		}

		bool TryEvaluateNativeGraph(UTG_Graph& NativeGraph, UTextureGraph& Graph)
		{
			UMixSettings* Settings = Graph.GetSettings();
			if (Settings == nullptr)
			{
				return false;
			}
			if (Settings->NumTargets() == 0)
			{
				Settings->InitTargets(1);
			}
			FInvalidationDetails Details = FInvalidationDetails().All();
			Details.Mix = &Graph;
			const std::shared_ptr<JobBatch> Batch = JobBatch::Create(Details);
			const MixUpdateCyclePtr Cycle = Batch ? Batch->GetCycle() : nullptr;
			if (!Cycle)
			{
				return false;
			}
			Cycle->AddTarget(std::make_shared<MixTargetUpdate>(&Graph, 0));
			FTG_EvaluationContext Context;
			Context.Cycle = Cycle;
			Context.TargetId = 0;
			Context.GraphDepth = 1;
			Cycle->PushMix(&Graph);
			const bool bValidated = NativeGraph.Validate(Cycle);
			if (bValidated)
			{
				NativeGraph.Evaluate(&Context);
			}
			Cycle->PopMix();
			TArray<FTG_Variant> Values;
			return bValidated && NativeGraph.GetAllOutputParamValues(Values) > 0;
		}
	}

	FAudioMediaGraphTextureGraphValue
	FAudioMediaGraphTextureGraphValue::Number(const double InValue)
	{
		FAudioMediaGraphTextureGraphValue Value;
		Value.Kind = EAudioMediaGraphTextureGraphValueKind::Number;
		Value.NumberValue = InValue;
		return Value;
	}

	FAudioMediaGraphTextureGraphValue
	FAudioMediaGraphTextureGraphValue::Boolean(const bool bInValue)
	{
		FAudioMediaGraphTextureGraphValue Value;
		Value.Kind = EAudioMediaGraphTextureGraphValueKind::Boolean;
		Value.bBooleanValue = bInValue;
		return Value;
	}

	FAudioMediaGraphTextureGraphValue
	FAudioMediaGraphTextureGraphValue::String(FString InValue)
	{
		FAudioMediaGraphTextureGraphValue Value;
		Value.Kind = EAudioMediaGraphTextureGraphValueKind::String;
		Value.StringValue = MoveTemp(InValue);
		return Value;
	}

	FAudioMediaGraphTextureGraphValue
	FAudioMediaGraphTextureGraphValue::NumberArray(TArray<double> InValues)
	{
		FAudioMediaGraphTextureGraphValue Value;
		Value.Kind = EAudioMediaGraphTextureGraphValueKind::NumberArray;
		Value.NumberArrayValue = MoveTemp(InValues);
		return Value;
	}

	bool FAudioMediaGraphTextureGraphNativeHook::TryApplyNodeMutations(
		UTextureGraph& Graph,
		const TArray<FAudioMediaGraphTextureGraphNodeMutation>& Mutations,
		FAudioMediaGraphError& OutError) const
	{
		OutError.Reset();
		UTG_Graph* const NativeGraph = Graph.Graph();
		if (NativeGraph == nullptr)
		{
			OutError.Set(TEXT("audio_media_graph_texture_graph_missing"));
			return false;
		}
		for (const FAudioMediaGraphTextureGraphNodeMutation& Mutation : Mutations)
		{
			UTG_Node* Existing = FindSemanticNode(*NativeGraph, Mutation.GetElementId());
			if (Mutation.GetIntent() == EAudioMediaGraphMutationIntent::Delete)
			{
				if (Existing == nullptr)
				{
					OutError.Set(TEXT("audio_media_graph_texture_graph_node_missing"));
					return false;
				}
				NativeGraph->RemoveNode(Existing);
				continue;
			}
			UClass* const ExpressionClass = LoadObject<UClass>(
				nullptr, *Mutation.GetExpressionClassRef());
			if (ExpressionClass == nullptr
				|| !ExpressionClass->IsChildOf(UTG_Expression::StaticClass()))
			{
				OutError.Set(TEXT("audio_media_graph_texture_graph_class_missing"),
					Mutation.GetExpressionClassRef());
				return false;
			}
			if (Mutation.GetIntent() == EAudioMediaGraphMutationIntent::Create
				&& Existing != nullptr)
			{
				OutError.Set(TEXT("audio_media_graph_texture_graph_node_exists"));
				return false;
			}
			if (Mutation.GetIntent() == EAudioMediaGraphMutationIntent::Update)
			{
				if (Existing == nullptr || Existing->GetExpression() == nullptr
					|| Existing->GetExpression()->GetClass() != ExpressionClass)
				{
					OutError.Set(TEXT("audio_media_graph_texture_graph_node_mismatch"));
					return false;
				}
				continue;
			}
			if (Existing != nullptr)
			{
				NativeGraph->RemoveNode(Existing);
			}
			UTG_Expression* const Expression = NewObject<UTG_Expression>(
				NativeGraph, ExpressionClass, FName(*Mutation.GetElementId()),
				RF_Transactional);
			UTG_Node* const Created = Expression
				? NativeGraph->CreateExpressionNode(Expression) : nullptr;
			if (Created == nullptr || Created->GetExpression() == nullptr
				|| Created->GetExpression()->GetName() != Mutation.GetElementId())
			{
				OutError.Set(TEXT("audio_media_graph_texture_graph_node_apply_failed"));
				return false;
			}
		}
		Graph.PostEditChange();
		return true;
	}

	bool FAudioMediaGraphTextureGraphNativeHook::TryApplyLinkMutations(
		UTextureGraph& Graph,
		const TArray<FAudioMediaGraphTextureGraphLinkMutation>& Mutations,
		FAudioMediaGraphError& OutError) const
	{
		OutError.Reset();
		UTG_Graph* const NativeGraph = Graph.Graph();
		if (NativeGraph == nullptr)
		{
			OutError.Set(TEXT("audio_media_graph_texture_graph_missing"));
			return false;
		}
		for (const FAudioMediaGraphTextureGraphLinkMutation& Mutation : Mutations)
		{
			UTG_Node* const From = FindSemanticNode(
				*NativeGraph, Mutation.GetFrom().GetElementId());
			UTG_Node* const To = FindSemanticNode(
				*NativeGraph, Mutation.GetTo().GetElementId());
			FTG_Name FromPinName(*Mutation.GetFrom().GetPinName());
			FTG_Name ToPinName(*Mutation.GetTo().GetPinName());
			UTG_Pin* const FromPin = From ? From->GetOutputPin(FromPinName) : nullptr;
			UTG_Pin* const ToPin = To ? To->GetInputPin(ToPinName) : nullptr;
			if (From == nullptr || To == nullptr || FromPin == nullptr || ToPin == nullptr)
			{
				OutError.Set(TEXT("audio_media_graph_texture_graph_link_endpoint_missing"));
				return false;
			}
			const bool bExists = FromPin->GetEdges().Contains(ToPin->GetId());
			if (Mutation.GetIntent() == EAudioMediaGraphMutationIntent::Delete)
			{
				if (!bExists)
				{
					OutError.Set(TEXT("audio_media_graph_texture_graph_link_missing"));
					return false;
				}
				NativeGraph->RemoveEdge(*From, FromPinName, *To, ToPinName);
			}
			else
			{
				if (bExists || !NativeGraph->Connect(
						*From, FromPinName, *To, ToPinName))
				{
					OutError.Set(TEXT("audio_media_graph_texture_graph_link_apply_failed"));
					return false;
				}
			}
		}
		Graph.PostEditChange();
		return true;
	}

	bool FAudioMediaGraphTextureGraphNativeHook::TryApplyParameterMutations(
		UTextureGraph& Graph,
		const TArray<FAudioMediaGraphTextureGraphParameterMutation>& Mutations,
		FAudioMediaGraphError& OutError) const
	{
		OutError.Reset();
		UTG_Graph* const NativeGraph = Graph.Graph();
		if (NativeGraph == nullptr)
		{
			OutError.Set(TEXT("audio_media_graph_texture_graph_missing"));
			return false;
		}
		for (const FAudioMediaGraphTextureGraphParameterMutation& Mutation : Mutations)
		{
			UTG_Node* const Node = FindSemanticNode(*NativeGraph, Mutation.GetElementId());
			FTG_Name PinName(*Mutation.GetPinName());
			UTG_Pin* const Pin = Node ? Node->GetInputPin(PinName) : nullptr;
			if (Pin == nullptr || !TryApplyPinValue(*Pin, Mutation.GetValue()))
			{
				OutError.Set(TEXT("audio_media_graph_texture_graph_parameter_apply_failed"));
				return false;
			}
			Pin->SetAliasName(FName(*Mutation.GetAlias()));
		}
		Graph.PostEditChange();
		return true;
	}

	bool FAudioMediaGraphTextureGraphNativeHook::TryValidateEvaluateSettle(
		UTextureGraph& Graph,
		FAudioMediaGraphError& OutError) const
	{
		OutError.Reset();
		if (Graph.Graph() == nullptr || !TryEvaluateNativeGraph(*Graph.Graph(), Graph))
		{
			OutError.Set(TEXT("audio_media_graph_evaluate_incomplete"));
			return false;
		}
		Graph.PostEditChange();
		return true;
	}

	bool FAudioMediaGraphTextureGraphNativeHook::TryCaptureArtifacts(
		UTextureGraph& Graph,
		const TArray<FAudioMediaGraphTextureGraphArtifactSelector>& Selectors,
		TArray<FAudioMediaGraphTextureGraphCapturedOutput>& OutOutputs,
		FAudioMediaGraphError& OutError) const
	{
		OutOutputs.Reset();
		OutError.Reset();
		UTG_Graph* const NativeGraph = Graph.Graph();
		if (NativeGraph == nullptr)
		{
			OutError.Set(TEXT("audio_media_graph_texture_graph_missing"));
			return false;
		}
		for (const FAudioMediaGraphTextureGraphArtifactSelector& Selector : Selectors)
		{
			UTG_Node* const Node = FindSemanticNode(*NativeGraph, Selector.GetElementId());
			FTG_Name PinName(*Selector.GetOutputName());
			UTG_Pin* const Pin = Node ? Node->GetOutputPin(PinName) : nullptr;
			UTG_Expression* const Expression = Node ? Node->GetExpression() : nullptr;
			if (Node == nullptr || Pin == nullptr || Expression == nullptr)
			{
				OutError.Set(TEXT("audio_media_graph_artifact_output_missing"));
				return false;
			}
			FString SettingsFingerprint = TEXT("native-output-settings:none");
			if (const UTG_Expression_Output* const Output =
				Cast<UTG_Expression_Output>(Expression))
			{
				SettingsFingerprint = Output->OutputSettings.ToString();
			}
			OutOutputs.Emplace(
				Expression->GetPathName(), Selector.GetOutputName(),
				Node->GetId().NodeIdx(), Pin->GetId().PinIdx(),
				Pin->GetArgumentCPPTypeName().ToString(),
				MoveTemp(SettingsFingerprint));
		}
		return !OutOutputs.IsEmpty();
	}

	bool FAudioMediaGraphTextureGraphNativeHook::TryRenderAndReadOutputs(
		UObject& WorldContextObject,
		UTextureGraph& Graph,
		TArray<FAudioMediaGraphTextureGraphRenderedOutput>& OutOutputs,
		FAudioMediaGraphError& OutError) const
	{
		OutOutputs.Reset();
		OutError.Reset();
		(void)WorldContextObject;
		if (!FApp::CanEverRender())
		{
			OutError.Set(TEXT("texture_graph_render_unavailable_null_rhi"));
			return false;
		}
		UTG_AsyncRenderTask* RenderTask =
			UTG_AsyncRenderTask::TG_AsyncRenderTask(&Graph);
		if (!RenderTask)
		{
			OutError.Set(TEXT("texture_graph_render_function_unavailable"));
			return false;
		}
		auto ReleaseRenderTask = [RenderTask]()
		{
			FWorldDelegates::OnWorldCleanup.RemoveAll(RenderTask);
			RenderTask->SetReadyToDestroy();
		};
		const TArray<UTextureRenderTarget2D*> RenderTargets =
			RenderTask->ActivateBlocking(nullptr);
		if (RenderTargets.IsEmpty())
		{
			ReleaseRenderTask();
			OutError.Set(TEXT("texture_graph_render_output_missing"));
			return false;
		}
		FlushRenderingCommands();
		for (UTextureRenderTarget2D* RenderTarget : RenderTargets)
		{
			FRenderTarget* Resource = RenderTarget
				? RenderTarget->GameThread_GetRenderTargetResource() : nullptr;
			TArray<FColor> Pixels;
			FReadSurfaceDataFlags ReadFlags(RCM_UNorm);
			ReadFlags.SetLinearToGamma(false);
			if (!RenderTarget || !Resource
				|| !Resource->ReadPixels(Pixels, ReadFlags)
				|| Pixels.IsEmpty())
			{
				OutOutputs.Reset();
				ReleaseRenderTask();
				OutError.Set(TEXT("texture_graph_render_readback_failed"));
				return false;
			}
			FAudioMediaGraphTextureGraphRenderedOutput Output;
			Output.RenderTarget = RenderTarget;
			Output.Stats.Width = RenderTarget->SizeX;
			Output.Stats.Height = RenderTarget->SizeY;
			Output.Stats.SampledPixelCount = Pixels.Num();
			Output.Stats.Minimum = FLinearColor(1.f, 1.f, 1.f, 1.f);
			Output.Stats.Maximum = FLinearColor(0.f, 0.f, 0.f, 0.f);
			FLinearColor Sum(0.f, 0.f, 0.f, 0.f);
			TSet<uint32> Unique;
			for (const FColor& Pixel : Pixels)
			{
				const FLinearColor Color = Pixel.ReinterpretAsLinear();
				Output.Stats.Minimum.R = FMath::Min(
					Output.Stats.Minimum.R, Color.R);
				Output.Stats.Minimum.G = FMath::Min(
					Output.Stats.Minimum.G, Color.G);
				Output.Stats.Minimum.B = FMath::Min(
					Output.Stats.Minimum.B, Color.B);
				Output.Stats.Minimum.A = FMath::Min(
					Output.Stats.Minimum.A, Color.A);
				Output.Stats.Maximum.R = FMath::Max(
					Output.Stats.Maximum.R, Color.R);
				Output.Stats.Maximum.G = FMath::Max(
					Output.Stats.Maximum.G, Color.G);
				Output.Stats.Maximum.B = FMath::Max(
					Output.Stats.Maximum.B, Color.B);
				Output.Stats.Maximum.A = FMath::Max(
					Output.Stats.Maximum.A, Color.A);
				Sum += Color;
				if ((Pixel.R + Pixel.G + Pixel.B) > 12)
				{
					++Output.Stats.NonBlackPixelCount;
				}
				if (Pixel.A < 250)
				{
					++Output.Stats.TransparentPixelCount;
				}
				if (Unique.Num() < 65536)
				{
					Unique.Add(Pixel.ToPackedARGB());
				}
			}
			Output.Stats.UniqueColorEstimate = Unique.Num();
			Output.Stats.Mean = Sum / static_cast<float>(Pixels.Num());
			OutOutputs.Add(MoveTemp(Output));
		}
		ReleaseRenderTask();
		return !OutOutputs.IsEmpty();
	}

	bool FAudioMediaGraphTextureGraphNativeHook::
	TryMutateEvaluateAndCaptureOwnedGraph(
		UTextureGraph& Graph,
		FAudioMediaGraphTextureGraphScenarioResult& OutResult,
		FAudioMediaGraphError& OutError) const
	{
		OutResult = FAudioMediaGraphTextureGraphScenarioResult();
		OutError.Reset();
		UTG_Graph* NativeGraph = Graph.Graph();
		if (NativeGraph == nullptr)
		{
			OutError.Set(TEXT("audio_media_graph_texture_graph_missing"));
			return false;
		}
		int32 BeforeNodeCount = 0;
		NativeGraph->ForEachNodes(
			[&BeforeNodeCount](const UTG_Node*, const uint32)
			{
				++BeforeNodeCount;
			});
		OutResult.BeforeRevision = FString::Printf(
			TEXT("texture-graph-nodes:%d"), BeforeNodeCount);

		UTG_Node* ScalarNode = NativeGraph->CreateExpressionNode(
			UTG_Expression_Scalar::StaticClass());
		UTG_Node* OutputNode = nullptr;
		NativeGraph->ForEachNodes(
			[&OutputNode](const UTG_Node* Node, const uint32)
			{
				if (OutputNode == nullptr
					&& Cast<UTG_Expression_Output>(Node->GetExpression()) != nullptr)
				{
					OutputNode = const_cast<UTG_Node*>(Node);
				}
			});
		UTG_Expression_Scalar* ScalarExpression = ScalarNode
			? Cast<UTG_Expression_Scalar>(ScalarNode->GetExpression())
			: nullptr;
		UTG_Expression_Output* OutputExpression = OutputNode
			? Cast<UTG_Expression_Output>(OutputNode->GetExpression())
			: nullptr;
		if (ScalarNode == nullptr || ScalarExpression == nullptr
			|| OutputNode == nullptr || OutputExpression == nullptr)
		{
			OutError.Set(TEXT("audio_media_graph_texture_graph_apply_failed"));
			return false;
		}
		ScalarExpression->Scalar = 0.75f;
		ScalarExpression->SetTitleName(TEXT("GC010_Gain"));
		OutputExpression->OutputSettings.FolderPath =
			TEXT("/Game/UAI/Owned/AudioMediaGraph");
		OutputExpression->SetShouldExport(false);
		FTG_Name SourcePin = TEXT("ValueOut");
		FTG_Name TargetPin = TEXT("Source");
		if (!NativeGraph->Connect(
			*ScalarNode, SourcePin, *OutputNode, TargetPin))
		{
			OutError.Set(TEXT("audio_media_graph_texture_graph_link_failed"));
			return false;
		}
		OutResult.bAppliedMutation = true;
		OutResult.ApplyEvidenceId = MakeTextureGraphEvidenceId(TEXT("apply"));

		// Evaluate through an owned native cycle. TriggerUpdate delegates to the
		// global editor MixManager, whose queue is intentionally absent in the
		// isolated NullRHI worker used by this contract test.
		UMixSettings* Settings = Graph.GetSettings();
		if (Settings == nullptr)
		{
			OutError.Set(TEXT("audio_media_graph_texture_graph_settings_missing"));
			return false;
		}
		if (Settings->NumTargets() == 0)
		{
			Settings->InitTargets(1);
		}
		FInvalidationDetails Details = FInvalidationDetails().All();
		Details.Mix = &Graph;
		const std::shared_ptr<JobBatch> Batch = JobBatch::Create(Details);
		const MixUpdateCyclePtr Cycle = Batch ? Batch->GetCycle() : nullptr;
		bool bValidated = false;
		if (Cycle)
		{
			Cycle->AddTarget(std::make_shared<MixTargetUpdate>(&Graph, 0));
			FTG_EvaluationContext Context;
			Context.Cycle = Cycle;
			Context.TargetId = 0;
			// A nested evaluation updates graph output values without the root-only
			// viewport LastRender side effect, which belongs to artifact publishing.
			Context.GraphDepth = 1;
			Cycle->PushMix(&Graph);
			bValidated = NativeGraph->Validate(Cycle);
			if (bValidated)
			{
				NativeGraph->Evaluate(&Context);
			}
			Cycle->PopMix();
		}
		TArray<FTG_Variant> BarrierValues;
		TArray<FName> BarrierNames;
		OutResult.bEvaluateBarrier = bValidated &&
			NativeGraph->GetAllOutputParamValues(
				BarrierValues, &BarrierNames) > 0;
		OutResult.bSettled = OutResult.bEvaluateBarrier;
		OutResult.BarrierEvidenceId =
			MakeTextureGraphEvidenceId(TEXT("validate-evaluate-settle"));
		if (!OutResult.bEvaluateBarrier)
		{
			OutError.Set(TEXT("audio_media_graph_evaluate_incomplete"));
			return false;
		}

		NativeGraph->ForEachNodes(
			[&OutResult](const UTG_Node* Node, const uint32)
			{
				const UTG_Expression* Expression = Node->GetExpression();
				if (Expression != nullptr)
				{
					OutResult.NodeIdentities.Emplace(
						Expression->GetPathName(), Node->GetId().NodeIdx(),
						Expression->GetClass()->GetPathName());
				}
			});
		NativeGraph->ForEachPins(
			[&OutResult](const UTG_Pin* Pin, const uint32, const uint32)
			{
				OutResult.PinIdentities.Emplace(
					Pin->GetId().NodeIdx(), Pin->GetId().PinIdx(),
					Pin->GetArgumentName().ToString(),
					Pin->IsInput() ? TEXT("input") : TEXT("output"),
					Pin->GetArgumentCPPTypeName().ToString());
			});
		OutResult.ParameterNames = NativeGraph->GetParamNames();
		if (OutResult.NodeIdentities.IsEmpty()
			|| OutResult.PinIdentities.IsEmpty()
			|| OutResult.ParameterNames.IsEmpty())
		{
			OutError.Set(TEXT("audio_media_graph_capture_failed"));
			return false;
		}
		OutResult.bIndependentCapture = true;
		OutResult.ReadbackRevision = FString::Printf(
			TEXT("texture-graph-nodes:%d-pins:%d-params:%d"),
			OutResult.NodeIdentities.Num(), OutResult.PinIdentities.Num(),
			OutResult.ParameterNames.Num());
		OutResult.ReadbackEvidenceId =
			MakeTextureGraphEvidenceId(TEXT("native-topology-readback"));
		return true;
	}

	bool FAudioMediaGraphTextureGraphNativeHook::
	TryCaptureSettledArtifactState(
		UTextureGraph& Graph,
		const FAudioMediaGraphTextureGraphScenarioResult& SettledResult,
		FAudioMediaGraphTextureGraphArtifactState& OutState,
		FAudioMediaGraphError& OutError) const
	{
		OutState = FAudioMediaGraphTextureGraphArtifactState();
		OutError.Reset();
		if (!SettledResult.HasEvaluateBarrier() || !SettledResult.HasSettled())
		{
			OutError.Set(TEXT("audio_media_graph_artifact_before_settle"));
			return false;
		}
		UTG_Graph* NativeGraph = Graph.Graph();
		if (NativeGraph == nullptr)
		{
			OutError.Set(TEXT("audio_media_graph_texture_graph_missing"));
			return false;
		}

		TArray<FTG_Variant> Values;
		TArray<FName> Names;
		NativeGraph->GetAllOutputParamValues(Values, &Names);
		for (int32 Index = 0; Index < Values.Num(); ++Index)
		{
			const FName OutputName = Names.IsValidIndex(Index)
				? Names[Index] : FName(*FString::Printf(TEXT("Output%d"), Index));
			const UTG_Pin* OutputPin = NativeGraph->FindParamPin(OutputName);
			FString SettingsFingerprint = TEXT("native-output-settings:missing");
			if (OutputPin != nullptr)
			{
				if (UTG_Expression_Output* OutputExpression =
					Cast<UTG_Expression_Output>(
						OutputPin->GetNodePtr()->GetExpression()))
				{
					SettingsFingerprint =
						OutputExpression->OutputSettings.ToString();
				}
			}
			const FString OutputIdentity = FString::Printf(
				TEXT("owner:%s|output_pin:%s|name:%s"),
				*Graph.GetPathName(),
				OutputPin ? *OutputPin->GetId().ToString() : TEXT("unknown"),
				*OutputName.ToString());
			OutState.Outputs.Emplace(
				OutputIdentity, SettingsFingerprint,
				DescribeVariantType(Values[Index]));
		}
		if (OutState.Outputs.IsEmpty())
		{
			OutError.Set(TEXT("audio_media_graph_artifact_output_missing"));
			return false;
		}
		OutState.bIndependentCapture = true;
		OutState.bPublished = false;
		OutState.EvidenceId =
			MakeTextureGraphEvidenceId(TEXT("native-artifact-state"));
		return true;
	}
}
