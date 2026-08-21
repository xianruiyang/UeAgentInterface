#pragma once

#include "CoreMinimal.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"

namespace UE::UAI::Composition::GraphCompound::AudioMediaIdentity
{
	inline TMap<FString, FString> MetaSoundInterface(
		FString InterfaceKind,
		FString Name)
	{
		return {
			{TEXT("interface_kind"), MoveTemp(InterfaceKind)},
			{TEXT("name"), MoveTemp(Name)},
		};
	}

	inline TMap<FString, FString> MetaSoundNode(FString NodeGuid)
	{
		return {{TEXT("node_guid"), MoveTemp(NodeGuid)}};
	}

	inline TMap<FString, FString> MetaSoundLink(
		FString FromNodeGuid,
		FString FromVertexGuid,
		FString ToNodeGuid,
		FString ToVertexGuid)
	{
		return {
			{TEXT("from_node_guid"), MoveTemp(FromNodeGuid)},
			{TEXT("from_vertex_guid"), MoveTemp(FromVertexGuid)},
			{TEXT("to_node_guid"), MoveTemp(ToNodeGuid)},
			{TEXT("to_vertex_guid"), MoveTemp(ToVertexGuid)},
		};
	}

	inline TMap<FString, FString> MetaSoundDefault(
		FString NodeGuid,
		FString VertexGuid)
	{
		return {
			{TEXT("node_guid"), MoveTemp(NodeGuid)},
			{TEXT("vertex_guid"), MoveTemp(VertexGuid)},
		};
	}

	inline TMap<FString, FString> SoundCueNode(FString ObjectPath)
	{
		return {{TEXT("object_path"), MoveTemp(ObjectPath)}};
	}

	inline TMap<FString, FString> SoundCueRelation(
		FString ParentObjectPath,
		FString ChildObjectPath)
	{
		return {
			{TEXT("parent_object_path"), MoveTemp(ParentObjectPath)},
			{TEXT("child_object_path"), MoveTemp(ChildObjectPath)},
		};
	}

	inline TMap<FString, FString> TextureGraphNode(FString ElementId)
	{
		return {{TEXT("element_id"), MoveTemp(ElementId)}};
	}

	inline TMap<FString, FString> TextureGraphPin(
		FString ElementId,
		FString Direction,
		FString PinName)
	{
		return {
			{TEXT("element_id"), MoveTemp(ElementId)},
			{TEXT("direction"), MoveTemp(Direction)},
			{TEXT("pin_name"), MoveTemp(PinName)},
		};
	}

	inline TMap<FString, FString> TextureGraphLink(
		FString FromElementId,
		FString FromPinName,
		FString ToElementId,
		FString ToPinName)
	{
		return {
			{TEXT("from_element_id"), MoveTemp(FromElementId)},
			{TEXT("from_pin_name"), MoveTemp(FromPinName)},
			{TEXT("to_element_id"), MoveTemp(ToElementId)},
			{TEXT("to_pin_name"), MoveTemp(ToPinName)},
		};
	}

	inline TMap<FString, FString> TextureGraphParameter(
		FString ElementId,
		FString PinName)
	{
		return {
			{TEXT("element_id"), MoveTemp(ElementId)},
			{TEXT("pin_name"), MoveTemp(PinName)},
		};
	}

	inline StateSemantic::FUeAgentCanonicalIdentity MakeIdentity(
		FString SemanticId,
		FString TargetKind,
		FString OwnerId,
		FString ObjectRole,
		FString Generation,
		TMap<FString, FString> Selectors = {})
	{
		if (Selectors.IsEmpty())
		{
			return StateSemantic::FUeAgentCanonicalIdentity(
				SemanticId,
				MoveTemp(TargetKind),
				MoveTemp(OwnerId),
				SemanticId,
				MoveTemp(ObjectRole),
				MoveTemp(Generation));
		}
		return StateSemantic::FUeAgentCanonicalIdentity(
			MoveTemp(SemanticId),
			MoveTemp(TargetKind),
			MoveTemp(OwnerId),
			MoveTemp(Selectors),
			MoveTemp(ObjectRole),
			MoveTemp(Generation));
	}
}
