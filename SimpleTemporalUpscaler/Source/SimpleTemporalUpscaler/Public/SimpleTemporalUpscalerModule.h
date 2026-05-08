#pragma once

#include "Modules/ModuleManager.h"

class FSimpleTemporalUpscalerViewExtension;

class FSimpleTemporalUpscalerModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void RegisterViewExtension();

	FDelegateHandle PostEngineInitHandle;
	TSharedPtr<FSimpleTemporalUpscalerViewExtension, ESPMode::ThreadSafe> ViewExtension;
};
