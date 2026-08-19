#pragma once

#include "CoreMinimal.h"

namespace UE::UAI::ArtifactPublisher::TestHooks
{
	enum class EFailurePoint : uint8
	{
		AfterFinalMovedToBackup,
	};

	class UEAGENTINTERFACE_API FScopedPublishFailure final
	{
	public:
		explicit FScopedPublishFailure(EFailurePoint InFailurePoint);
		~FScopedPublishFailure();

		FScopedPublishFailure(const FScopedPublishFailure&) = delete;
		FScopedPublishFailure& operator=(const FScopedPublishFailure&) = delete;

	private:
		EFailurePoint FailurePoint;
	};
}
