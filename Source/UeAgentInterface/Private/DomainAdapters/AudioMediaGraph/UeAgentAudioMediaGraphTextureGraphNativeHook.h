#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/AudioMediaGraph/UeAgentAudioMediaGraphAdapter.h"

class UTextureGraph;
class UTextureRenderTarget2D;

namespace UE::UAI::DomainAdapters::AudioMediaGraph
{
	enum class EAudioMediaGraphTextureGraphValueKind : uint8
	{
		Number,
		Boolean,
		String,
		NumberArray,
	};

	class FAudioMediaGraphTextureGraphValue final
	{
	public:
		static FAudioMediaGraphTextureGraphValue Number(double InValue);
		static FAudioMediaGraphTextureGraphValue Boolean(bool bInValue);
		static FAudioMediaGraphTextureGraphValue String(FString InValue);
		static FAudioMediaGraphTextureGraphValue NumberArray(
			TArray<double> InValues);

		EAudioMediaGraphTextureGraphValueKind GetKind() const { return Kind; }
		double GetNumber() const { return NumberValue; }
		bool GetBoolean() const { return bBooleanValue; }
		const FString& GetString() const { return StringValue; }
		const TArray<double>& GetNumberArray() const { return NumberArrayValue; }

	private:
		EAudioMediaGraphTextureGraphValueKind Kind =
			EAudioMediaGraphTextureGraphValueKind::Number;
		double NumberValue = 0.0;
		bool bBooleanValue = false;
		FString StringValue;
		TArray<double> NumberArrayValue;
	};

	class FAudioMediaGraphTextureGraphNodeMutation final
	{
	public:
		FAudioMediaGraphTextureGraphNodeMutation(
			FString InElementId,
			FString InExpressionClassRef,
			EAudioMediaGraphMutationIntent InIntent)
			: ElementId(MoveTemp(InElementId))
			, ExpressionClassRef(MoveTemp(InExpressionClassRef))
			, Intent(InIntent)
		{
		}

		const FString& GetElementId() const { return ElementId; }
		const FString& GetExpressionClassRef() const { return ExpressionClassRef; }
		EAudioMediaGraphMutationIntent GetIntent() const { return Intent; }

	private:
		FString ElementId;
		FString ExpressionClassRef;
		EAudioMediaGraphMutationIntent Intent;
	};

	class FAudioMediaGraphTextureGraphEndpoint final
	{
	public:
		FAudioMediaGraphTextureGraphEndpoint(
			FString InElementId, FString InPinName)
			: ElementId(MoveTemp(InElementId))
			, PinName(MoveTemp(InPinName))
		{
		}

		const FString& GetElementId() const { return ElementId; }
		const FString& GetPinName() const { return PinName; }

	private:
		FString ElementId;
		FString PinName;
	};

	class FAudioMediaGraphTextureGraphLinkMutation final
	{
	public:
		FAudioMediaGraphTextureGraphLinkMutation(
			FAudioMediaGraphTextureGraphEndpoint InFrom,
			FAudioMediaGraphTextureGraphEndpoint InTo,
			EAudioMediaGraphMutationIntent InIntent)
			: From(MoveTemp(InFrom)), To(MoveTemp(InTo)), Intent(InIntent)
		{
		}

		const FAudioMediaGraphTextureGraphEndpoint& GetFrom() const { return From; }
		const FAudioMediaGraphTextureGraphEndpoint& GetTo() const { return To; }
		EAudioMediaGraphMutationIntent GetIntent() const { return Intent; }

	private:
		FAudioMediaGraphTextureGraphEndpoint From;
		FAudioMediaGraphTextureGraphEndpoint To;
		EAudioMediaGraphMutationIntent Intent;
	};

	class FAudioMediaGraphTextureGraphParameterMutation final
	{
	public:
		FAudioMediaGraphTextureGraphParameterMutation(
			FString InElementId,
			FString InPinName,
			FString InAlias,
			FAudioMediaGraphTextureGraphValue InValue,
			EAudioMediaGraphMutationIntent InIntent)
			: ElementId(MoveTemp(InElementId))
			, PinName(MoveTemp(InPinName))
			, Alias(MoveTemp(InAlias))
			, Value(MoveTemp(InValue))
			, Intent(InIntent)
		{
		}

		const FString& GetElementId() const { return ElementId; }
		const FString& GetPinName() const { return PinName; }
		const FString& GetAlias() const { return Alias; }
		const FAudioMediaGraphTextureGraphValue& GetValue() const { return Value; }
		EAudioMediaGraphMutationIntent GetIntent() const { return Intent; }

	private:
		FString ElementId;
		FString PinName;
		FString Alias;
		FAudioMediaGraphTextureGraphValue Value;
		EAudioMediaGraphMutationIntent Intent;
	};

	class FAudioMediaGraphTextureGraphArtifactSelector final
	{
	public:
		FAudioMediaGraphTextureGraphArtifactSelector(
			FString InElementId, FString InOutputName)
			: ElementId(MoveTemp(InElementId))
			, OutputName(MoveTemp(InOutputName))
		{
		}

		const FString& GetElementId() const { return ElementId; }
		const FString& GetOutputName() const { return OutputName; }

	private:
		FString ElementId;
		FString OutputName;
	};

	class FAudioMediaGraphTextureGraphCapturedOutput final
	{
	public:
		FAudioMediaGraphTextureGraphCapturedOutput(
			FString InExpressionObjectPath,
			FString InOutputName,
			int32 InNodeIndex,
			int32 InPinIndex,
			FString InEvaluatedValueType,
			FString InSettingsFingerprint)
			: ExpressionObjectPath(MoveTemp(InExpressionObjectPath))
			, OutputName(MoveTemp(InOutputName))
			, NodeIndex(InNodeIndex)
			, PinIndex(InPinIndex)
			, EvaluatedValueType(MoveTemp(InEvaluatedValueType))
			, SettingsFingerprint(MoveTemp(InSettingsFingerprint))
		{
		}

		const FString& GetExpressionObjectPath() const { return ExpressionObjectPath; }
		const FString& GetOutputName() const { return OutputName; }
		int32 GetNodeIndex() const { return NodeIndex; }
		int32 GetPinIndex() const { return PinIndex; }
		const FString& GetEvaluatedValueType() const { return EvaluatedValueType; }
		const FString& GetSettingsFingerprint() const { return SettingsFingerprint; }

	private:
		FString ExpressionObjectPath;
		FString OutputName;
		int32 NodeIndex = INDEX_NONE;
		int32 PinIndex = INDEX_NONE;
		FString EvaluatedValueType;
		FString SettingsFingerprint;
	};

	class FAudioMediaGraphTextureGraphNodeIdentity final
	{
	public:
		FAudioMediaGraphTextureGraphNodeIdentity() = default;
		FAudioMediaGraphTextureGraphNodeIdentity(
			FString InExpressionObjectPath,
			int32 InNodeIndex,
			FString InExpressionClass)
			: ExpressionObjectPath(MoveTemp(InExpressionObjectPath))
			, NodeIndex(InNodeIndex)
			, ExpressionClass(MoveTemp(InExpressionClass))
		{
		}

		const FString& GetExpressionObjectPath() const
		{
			return ExpressionObjectPath;
		}
		int32 GetNodeIndex() const { return NodeIndex; }
		const FString& GetExpressionClass() const { return ExpressionClass; }

	private:
		FString ExpressionObjectPath;
		int32 NodeIndex = INDEX_NONE;
		FString ExpressionClass;
	};

	class FAudioMediaGraphTextureGraphPinIdentity final
	{
	public:
		FAudioMediaGraphTextureGraphPinIdentity() = default;
		FAudioMediaGraphTextureGraphPinIdentity(
			int32 InNodeIndex,
			int32 InPinIndex,
			FString InArgumentName,
			FString InDirection,
			FString InCppType)
			: NodeIndex(InNodeIndex)
			, PinIndex(InPinIndex)
			, ArgumentName(MoveTemp(InArgumentName))
			, Direction(MoveTemp(InDirection))
			, CppType(MoveTemp(InCppType))
		{
		}

		int32 GetNodeIndex() const { return NodeIndex; }
		int32 GetPinIndex() const { return PinIndex; }
		const FString& GetArgumentName() const { return ArgumentName; }
		const FString& GetDirection() const { return Direction; }
		const FString& GetCppType() const { return CppType; }

	private:
		int32 NodeIndex = INDEX_NONE;
		int32 PinIndex = INDEX_NONE;
		FString ArgumentName;
		FString Direction;
		FString CppType;
	};

	class FAudioMediaGraphTextureGraphScenarioResult final
	{
	public:
		bool HasAppliedMutation() const { return bAppliedMutation; }
		bool HasEvaluateBarrier() const { return bEvaluateBarrier; }
		bool HasSettled() const { return bSettled; }
		bool HasIndependentCapture() const { return bIndependentCapture; }
		const TArray<FAudioMediaGraphTextureGraphNodeIdentity>&
		GetNodeIdentities() const { return NodeIdentities; }
		const TArray<FAudioMediaGraphTextureGraphPinIdentity>&
		GetPinIdentities() const { return PinIdentities; }
		const TArray<FName>& GetParameterNames() const { return ParameterNames; }
		const FString& GetBeforeRevision() const { return BeforeRevision; }
		const FString& GetReadbackRevision() const { return ReadbackRevision; }
		const FString& GetApplyEvidenceId() const { return ApplyEvidenceId; }
		const FString& GetBarrierEvidenceId() const { return BarrierEvidenceId; }
		const FString& GetReadbackEvidenceId() const { return ReadbackEvidenceId; }

	private:
		friend class FAudioMediaGraphTextureGraphNativeHook;
		bool bAppliedMutation = false;
		bool bEvaluateBarrier = false;
		bool bSettled = false;
		bool bIndependentCapture = false;
		TArray<FAudioMediaGraphTextureGraphNodeIdentity> NodeIdentities;
		TArray<FAudioMediaGraphTextureGraphPinIdentity> PinIdentities;
		TArray<FName> ParameterNames;
		FString BeforeRevision;
		FString ReadbackRevision;
		FString ApplyEvidenceId;
		FString BarrierEvidenceId;
		FString ReadbackEvidenceId;
	};

	class FAudioMediaGraphTextureGraphArtifactOutput final
	{
	public:
		FAudioMediaGraphTextureGraphArtifactOutput() = default;
		FAudioMediaGraphTextureGraphArtifactOutput(
			FString InOutputIdentity,
			FString InSettingsFingerprint,
			FString InEvaluatedValueType)
			: OutputIdentity(MoveTemp(InOutputIdentity))
			, SettingsFingerprint(MoveTemp(InSettingsFingerprint))
			, EvaluatedValueType(MoveTemp(InEvaluatedValueType))
		{
		}

		const FString& GetOutputIdentity() const { return OutputIdentity; }
		const FString& GetSettingsFingerprint() const
		{
			return SettingsFingerprint;
		}
		const FString& GetEvaluatedValueType() const
		{
			return EvaluatedValueType;
		}

	private:
		FString OutputIdentity;
		FString SettingsFingerprint;
		FString EvaluatedValueType;
	};

	class FAudioMediaGraphTextureGraphArtifactState final
	{
	public:
		bool HasIndependentCapture() const { return bIndependentCapture; }
		bool WasPublished() const { return bPublished; }
		const TArray<FAudioMediaGraphTextureGraphArtifactOutput>&
		GetOutputs() const { return Outputs; }
		const FString& GetEvidenceId() const { return EvidenceId; }

	private:
		friend class FAudioMediaGraphTextureGraphNativeHook;
		bool bIndependentCapture = false;
		bool bPublished = false;
		TArray<FAudioMediaGraphTextureGraphArtifactOutput> Outputs;
		FString EvidenceId;
	};

	class FAudioMediaGraphTextureGraphPixelStats final
	{
	public:
		int32 GetWidth() const { return Width; }
		int32 GetHeight() const { return Height; }
		int32 GetSampledPixelCount() const { return SampledPixelCount; }
		int32 GetNonBlackPixelCount() const { return NonBlackPixelCount; }
		int32 GetTransparentPixelCount() const { return TransparentPixelCount; }
		int32 GetUniqueColorEstimate() const { return UniqueColorEstimate; }
		const FLinearColor& GetMinimum() const { return Minimum; }
		const FLinearColor& GetMaximum() const { return Maximum; }
		const FLinearColor& GetMean() const { return Mean; }

	private:
		friend class FAudioMediaGraphTextureGraphNativeHook;
		int32 Width = 0;
		int32 Height = 0;
		int32 SampledPixelCount = 0;
		int32 NonBlackPixelCount = 0;
		int32 TransparentPixelCount = 0;
		int32 UniqueColorEstimate = 0;
		FLinearColor Minimum = FLinearColor::Black;
		FLinearColor Maximum = FLinearColor::Black;
		FLinearColor Mean = FLinearColor::Black;
	};

	class FAudioMediaGraphTextureGraphRenderedOutput final
	{
	public:
		UTextureRenderTarget2D* GetRenderTarget() const { return RenderTarget; }
		const FAudioMediaGraphTextureGraphPixelStats& GetStats() const
		{
			return Stats;
		}

	private:
		friend class FAudioMediaGraphTextureGraphNativeHook;
		UTextureRenderTarget2D* RenderTarget = nullptr;
		FAudioMediaGraphTextureGraphPixelStats Stats;
	};

	class UEAGENTINTERFACE_API FAudioMediaGraphTextureGraphNativeHook final
	{
	public:
		bool TryApplyNodeMutations(
			UTextureGraph& Graph,
			const TArray<FAudioMediaGraphTextureGraphNodeMutation>& Mutations,
			FAudioMediaGraphError& OutError) const;

		bool TryApplyLinkMutations(
			UTextureGraph& Graph,
			const TArray<FAudioMediaGraphTextureGraphLinkMutation>& Mutations,
			FAudioMediaGraphError& OutError) const;

		bool TryApplyParameterMutations(
			UTextureGraph& Graph,
			const TArray<FAudioMediaGraphTextureGraphParameterMutation>& Mutations,
			FAudioMediaGraphError& OutError) const;

		bool TryValidateEvaluateSettle(
			UTextureGraph& Graph,
			FAudioMediaGraphError& OutError) const;

		bool TryCaptureArtifacts(
			UTextureGraph& Graph,
			const TArray<FAudioMediaGraphTextureGraphArtifactSelector>& Selectors,
			TArray<FAudioMediaGraphTextureGraphCapturedOutput>& OutOutputs,
			FAudioMediaGraphError& OutError) const;

		bool TryRenderAndReadOutputs(
			UObject& WorldContextObject,
			UTextureGraph& Graph,
			TArray<FAudioMediaGraphTextureGraphRenderedOutput>& OutOutputs,
			FAudioMediaGraphError& OutError) const;

		bool TryMutateEvaluateAndCaptureOwnedGraph(
			UTextureGraph& Graph,
			FAudioMediaGraphTextureGraphScenarioResult& OutResult,
			FAudioMediaGraphError& OutError) const;

		bool TryCaptureSettledArtifactState(
			UTextureGraph& Graph,
			const FAudioMediaGraphTextureGraphScenarioResult& SettledResult,
			FAudioMediaGraphTextureGraphArtifactState& OutState,
			FAudioMediaGraphError& OutError) const;
	};
}
