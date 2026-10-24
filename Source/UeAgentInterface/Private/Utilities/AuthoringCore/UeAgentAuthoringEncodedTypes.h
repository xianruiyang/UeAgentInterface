// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;
namespace UeAgentAuthoringTypeDescriptors { struct FDescriptor; }

namespace UeAgentAuthoringEncodedTypes
{
	/** Projection policy only; native values and their presence remain intact. */
	enum class EFieldEmission : uint8
	{
		Always,
		NativeDefault,
		NonEmptyObject
	};
	/** Preserve codec type provenance while an accessor assembles an untyped document.
	 * The JSON payload is unchanged. The scope ends before the snapshot is detached. */
	class UEAGENTINTERFACE_API FScope
	{
	public:
		FScope();
		~FScope();
		FScope(const FScope&) = delete;
		FScope& operator=(const FScope&) = delete;
		TSharedPtr<FJsonObject> BuildDefinition(const TSharedPtr<FJsonValue>& Value) const;
		/** Derive a sparse authoring view, retaining omitted native values for comparison. */
		TSharedPtr<FJsonValue> ProjectAuthoringValue(const TSharedPtr<FJsonValue>& NativeValue) const;

		static bool IsActive();
		static void Record(const TSharedPtr<FJsonValue>& Value,
			const UeAgentAuthoringTypeDescriptors::FDescriptor& Type,
			const TSharedPtr<FJsonObject>& Definition);
		static void Copy(const TSharedPtr<FJsonValue>& Source, const TSharedPtr<FJsonValue>& Destination);
		/** A native FName collection represented by JSON object members. Ordinary
		 * document fields and generated display aliases must not use this declaration. */
		static void RecordNameKeyedObject(const TSharedPtr<FJsonObject>& Object);
		/** Explicit compound native identity; component kinds are name or string.
		 * The producer owns the separator and component boundaries. */
		static void RecordCompositeKeyedObject(const TSharedPtr<FJsonObject>& Object,
			const FString& Separator, const TArray<FString>& ComponentKinds);
		static void SetNameField(const TSharedPtr<FJsonObject>& Object, const FString& Field, FName Name);
		/** Store the actual native field before projection. NativeDefault requires a
		 * proven native default with no independently meaningful override/presence.
		 * NonEmptyObject is only for non-presence-sensitive grouping objects. */
		static void SetNativeField(const TSharedPtr<FJsonObject>& Parent,
			const FString& Field, const TSharedPtr<FJsonValue>& Value,
			EFieldEmission Emission = EFieldEmission::Always);

	private:
		struct FImpl;
		TUniquePtr<FImpl> Impl;
	};
}
