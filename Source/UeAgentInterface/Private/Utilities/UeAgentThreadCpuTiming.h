#pragma once

#include "CoreTypes.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

// Diagnostic samples only: no ticker, retained state, scheduling or behavior
// changes. CPU excludes other threads; wall minus CPU is not an IO diagnosis.
namespace UE::UAI::Timing
{
	struct FThreadCpuSample
	{
		uint64 Ticks = 0;
		uint32 ThreadId = 0;
		bool bValid = false;
	};

	inline FThreadCpuSample SampleThreadCpu()
	{
		FThreadCpuSample Result;
#if PLATFORM_WINDOWS
		FILETIME Created{}, Exited{}, Kernel{}, User{};
		Result.ThreadId = ::GetCurrentThreadId();
		if (::GetThreadTimes(::GetCurrentThread(), &Created, &Exited, &Kernel, &User))
		{
			Result.Ticks = (static_cast<uint64>(Kernel.dwHighDateTime) << 32) + Kernel.dwLowDateTime
				+ (static_cast<uint64>(User.dwHighDateTime) << 32) + User.dwLowDateTime;
			Result.bValid = true;
		}
#endif
		return Result;
	}

	inline double ThreadCpuSeconds(const FThreadCpuSample& Before, const FThreadCpuSample& After)
	{
		return Before.bValid && After.bValid && Before.ThreadId == After.ThreadId && After.Ticks >= Before.Ticks
			? static_cast<double>(After.Ticks - Before.Ticks) * 1.e-7 : -1.0;
	}
}
