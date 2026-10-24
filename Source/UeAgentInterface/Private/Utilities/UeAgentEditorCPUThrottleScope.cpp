#include "Utilities/UeAgentEditorCPUThrottleScope.h"

#include "Async/Async.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Templates/Atomic.h"
#include "UObject/WeakObjectPtr.h"

namespace UE::UAI::EditorRuntime
{
	struct FScopedCPUThrottleExemption::FImpl
	{
		TWeakObjectPtr<UEditorEngine> Editor;
		FDelegateHandle Handle;
		TSharedRef<TAtomic<bool>, ESPMode::ThreadSafe> Active = MakeShared<TAtomic<bool>, ESPMode::ThreadSafe>(true);

		FImpl()
		{
			if (!GEditor) return;
			Editor = GEditor;
			const auto Predicate = Active;
			auto Delegate = UEditorEngine::FShouldDisableCPUThrottling::CreateLambda([Predicate]() { return Predicate->Load(); });
			Handle = Delegate.GetHandle();
			GEditor->ShouldDisableCPUThrottlingDelegates.Add(MoveTemp(Delegate));
		}

		~FImpl()
		{
			Active->Store(false);
			if (!Handle.IsValid()) return;
			auto Remove = [Editor = Editor, Handle = Handle]()
			{
				if (UEditorEngine* Engine = Editor.Get())
					Engine->ShouldDisableCPUThrottlingDelegates.RemoveAll(
						[Handle](const UEditorEngine::FShouldDisableCPUThrottling& Delegate) { return Delegate.GetHandle() == Handle; });
			};
			if (IsInGameThread()) Remove();
			else AsyncTask(ENamedThreads::GameThread, MoveTemp(Remove));
		}
	};

	FScopedCPUThrottleExemption::FScopedCPUThrottleExemption() : Impl(MakeUnique<FImpl>()) {}
	FScopedCPUThrottleExemption::~FScopedCPUThrottleExemption() = default;
}
