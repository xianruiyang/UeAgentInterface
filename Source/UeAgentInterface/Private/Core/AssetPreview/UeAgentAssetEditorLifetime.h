#pragma once

#include "CoreMinimal.h"

class FJsonObject;
namespace UE::UAI::Contract { struct FDiagnostic; }

namespace UE::UAI::AssetPreview
{
	// All methods below run on the game thread. Native window identity is owned
	// here, not in an operation or an individual preview domain.
	class IAssetEditorOpenScope
	{
	public:
		virtual ~IAssetEditorOpenScope() = default;
		virtual void Complete(UObject* Asset) = 0;
	};

	class IAssetEditorLease
	{
	public:
		virtual ~IAssetEditorLease() = default;
		// A close request is distinct from observing that the resource is gone.
		// Retry is explicit; polling alone must not repeatedly prompt the user.
		virtual bool Close(bool bRequestClose, UE::UAI::Contract::FDiagnostic& OutDiagnostic,
			TSharedPtr<FJsonObject>& OutDetails) = 0;
		virtual TSharedPtr<FJsonObject> Describe() const = 0;
	};

	TUniquePtr<IAssetEditorOpenScope> BeginAssetEditorOpen(UObject* Asset);
	TSharedPtr<IAssetEditorLease, ESPMode::ThreadSafe> AcquireAssetEditorLease(UObject* Asset, bool bAdoptExisting);
	void StartAssetEditorTracking();
	// Called while UObject/editor subsystems are still alive, after operation drain.
	void DetachAssetEditorTracking();
	// Safe after UObject shutdown; does not query the editor or close windows.
	void ShutdownAssetEditorTracking();
}
