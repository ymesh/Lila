#include "LilaLinkCrowd.h"

#include "LilaLinkMethodRegistry.h"

#include "Modules/ModuleManager.h"

class FLilaLinkCrowdModule : public IModuleInterface
{
public:
	virtual void StartupModule() override { LilaLinkCrowd::RegisterMethods(); }

	virtual void ShutdownModule() override { LilaLink::UnregisterFeature(LilaLinkCrowd::Feature); }
};

IMPLEMENT_MODULE(FLilaLinkCrowdModule, LilaLinkCrowd)
