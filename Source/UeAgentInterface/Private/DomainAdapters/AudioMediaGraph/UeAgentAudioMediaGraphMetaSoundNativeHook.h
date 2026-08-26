#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/AudioMediaGraph/UeAgentAudioMediaGraphAdapter.h"
#include "MetasoundFrontendLiteral.h"

class UMetaSoundSource;

namespace UE::UAI::DomainAdapters::AudioMediaGraph
{
	class FAudioMediaGraphMetaSoundInterfaceMutation final
	{
	public:
		FAudioMediaGraphMetaSoundInterfaceMutation(
			FString InInterfaceKind,
			FName InName,
			FName InDataType,
			bool bInConstructor,
			FMetasoundFrontendLiteral InDefaultValue,
			EAudioMediaGraphMutationIntent InIntent)
			: InterfaceKind(MoveTemp(InInterfaceKind))
			, Name(InName)
			, DataType(InDataType)
			, bConstructor(bInConstructor)
			, DefaultValue(MoveTemp(InDefaultValue))
			, Intent(InIntent)
		{
		}

		const FString& GetInterfaceKind() const { return InterfaceKind; }
		FName GetName() const { return Name; }
		FName GetDataType() const { return DataType; }
		bool IsConstructor() const { return bConstructor; }
		const FMetasoundFrontendLiteral& GetDefaultValue() const { return DefaultValue; }
		EAudioMediaGraphMutationIntent GetIntent() const { return Intent; }

	private:
		FString InterfaceKind;
		FName Name;
		FName DataType;
		bool bConstructor = false;
		FMetasoundFrontendLiteral DefaultValue;
		EAudioMediaGraphMutationIntent Intent;
	};

	class FAudioMediaGraphMetaSoundNodeMutation final
	{
	public:
		FAudioMediaGraphMetaSoundNodeMutation(
			FGuid InNodeId,
			FString InClassName,
			int32 InMajorVersion,
			int32 InMinorVersion,
			EAudioMediaGraphMutationIntent InIntent,
			TOptional<FVector2D> InPosition = {})
			: NodeId(InNodeId)
			, ClassName(MoveTemp(InClassName))
			, MajorVersion(InMajorVersion)
			, MinorVersion(InMinorVersion)
			, Intent(InIntent)
			, Position(MoveTemp(InPosition))
		{
		}

		const FGuid& GetNodeId() const { return NodeId; }
		const FString& GetClassName() const { return ClassName; }
		int32 GetMajorVersion() const { return MajorVersion; }
		int32 GetMinorVersion() const { return MinorVersion; }
		EAudioMediaGraphMutationIntent GetIntent() const { return Intent; }
		const TOptional<FVector2D>& GetPosition() const { return Position; }

	private:
		FGuid NodeId;
		FString ClassName;
		int32 MajorVersion = 1;
		int32 MinorVersion = 0;
		EAudioMediaGraphMutationIntent Intent;
		TOptional<FVector2D> Position;
	};

	class FAudioMediaGraphMetaSoundLinkMutation final
	{
	public:
		FAudioMediaGraphMetaSoundLinkMutation(
			FGuid InFromNodeId,
			FGuid InFromVertexId,
			FGuid InToNodeId,
			FGuid InToVertexId,
			EAudioMediaGraphMutationIntent InIntent)
			: FromNodeId(InFromNodeId)
			, FromVertexId(InFromVertexId)
			, ToNodeId(InToNodeId)
			, ToVertexId(InToVertexId)
			, Intent(InIntent)
		{
		}

		const FGuid& GetFromNodeId() const { return FromNodeId; }
		const FGuid& GetFromVertexId() const { return FromVertexId; }
		const FGuid& GetToNodeId() const { return ToNodeId; }
		const FGuid& GetToVertexId() const { return ToVertexId; }
		EAudioMediaGraphMutationIntent GetIntent() const { return Intent; }

	private:
		FGuid FromNodeId;
		FGuid FromVertexId;
		FGuid ToNodeId;
		FGuid ToVertexId;
		EAudioMediaGraphMutationIntent Intent;
	};

	class FAudioMediaGraphMetaSoundDefaultMutation final
	{
	public:
		FAudioMediaGraphMetaSoundDefaultMutation(
			FGuid InNodeId,
			FGuid InVertexId,
			FMetasoundFrontendLiteral InValue)
			: NodeId(InNodeId)
			, VertexId(InVertexId)
			, Value(MoveTemp(InValue))
		{
		}

		const FGuid& GetNodeId() const { return NodeId; }
		const FGuid& GetVertexId() const { return VertexId; }
		const FMetasoundFrontendLiteral& GetValue() const { return Value; }

	private:
		FGuid NodeId;
		FGuid VertexId;
		FMetasoundFrontendLiteral Value;
	};

	class FAudioMediaGraphMetaSoundScenarioResult final
	{
	public:
		bool HasAppliedMutation() const { return bAppliedMutation; }
		bool HasCompileBarrier() const { return bCompileBarrier; }
		bool HasSettled() const { return bSettled; }
		bool HasIndependentCapture() const { return bIndependentCapture; }
		const FGuid& GetNodeGuid() const { return NodeGuid; }
		const FGuid& GetVertexGuid() const { return VertexGuid; }
		const FString& GetPinDirection() const { return PinDirection; }
		const FName& GetPinType() const { return PinType; }
		const TArray<FName>& GetGraphInputNames() const { return GraphInputNames; }
		const FString& GetBeforeRevision() const { return BeforeRevision; }
		const FString& GetReadbackRevision() const { return ReadbackRevision; }
		const FString& GetApplyEvidenceId() const { return ApplyEvidenceId; }
		const FString& GetBarrierEvidenceId() const { return BarrierEvidenceId; }
		const FString& GetReadbackEvidenceId() const { return ReadbackEvidenceId; }

	private:
		friend class FAudioMediaGraphMetaSoundNativeHook;
		bool bAppliedMutation = false;
		bool bCompileBarrier = false;
		bool bSettled = false;
		bool bIndependentCapture = false;
		FGuid NodeGuid;
		FGuid VertexGuid;
		FString PinDirection;
		FName PinType;
		TArray<FName> GraphInputNames;
		FString BeforeRevision;
		FString ReadbackRevision;
		FString ApplyEvidenceId;
		FString BarrierEvidenceId;
		FString ReadbackEvidenceId;
	};

	class UEAGENTINTERFACE_API FAudioMediaGraphMetaSoundNativeHook final
	{
	public:
		bool TryApplyInterfaceMutations(
			UMetaSoundSource& Source,
			const TArray<FAudioMediaGraphMetaSoundInterfaceMutation>& Mutations,
			FAudioMediaGraphError& OutError) const;

		bool TryApplyNodeMutations(
			UMetaSoundSource& Source,
			const TArray<FAudioMediaGraphMetaSoundNodeMutation>& Mutations,
			FAudioMediaGraphError& OutError) const;

		bool TryApplyLinkMutations(
			UMetaSoundSource& Source,
			const TArray<FAudioMediaGraphMetaSoundLinkMutation>& Mutations,
			FAudioMediaGraphError& OutError) const;

		bool TryApplyDefaultMutation(
			UMetaSoundSource& Source,
			const FAudioMediaGraphMetaSoundDefaultMutation& Mutation,
			FAudioMediaGraphError& OutError) const;

		bool TryRegisterCompileSettle(
			UMetaSoundSource& Source,
			FAudioMediaGraphError& OutError) const;

		bool TryMutateCompileAndCaptureOwnedSource(
			UMetaSoundSource& Source,
			FAudioMediaGraphMetaSoundScenarioResult& OutResult,
			FAudioMediaGraphError& OutError) const;
	};
}
