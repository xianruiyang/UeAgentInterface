#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::CommandRegistry
{
	class FUeAgentImmutableCommandRegistry;

	class UEAGENTINTERFACE_API FUeAgentActiveCommandManifest final
	{
	public:
		static bool TryBuild(
			TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
			const FString& PluginVersion,
			const FString& BuildId,
			FString& OutManifest,
			FString& OutError);

		static bool TryWrite(
			TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
			const FString& PluginVersion,
			const FString& BuildId,
			const FString& ManifestPath,
			FString& OutError);

		static bool TryVerify(
			TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
			const FString& PluginVersion,
			const FString& BuildId,
			const FString& ManifestPath,
			FString& OutError);
	};
}
