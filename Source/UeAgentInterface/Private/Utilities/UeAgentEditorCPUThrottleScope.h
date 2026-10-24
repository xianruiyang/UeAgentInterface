#pragma once

#include "CoreMinimal.h"

namespace UE::UAI::EditorRuntime
{
	// Active game-time work needs native Editor ticks even when the application
	// is in the background. This scope does not change performance settings.
	class UEAGENTINTERFACE_API FScopedCPUThrottleExemption final
	{
	public:
		FScopedCPUThrottleExemption();
		~FScopedCPUThrottleExemption();
		FScopedCPUThrottleExemption(const FScopedCPUThrottleExemption&) = delete;
		FScopedCPUThrottleExemption& operator=(const FScopedCPUThrottleExemption&) = delete;

	private:
		struct FImpl;
		TUniquePtr<FImpl> Impl;
	};
}
