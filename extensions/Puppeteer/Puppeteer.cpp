// Puppeteer's entry point: one registration call per subsystem, scene first so the UI can find its types and commands.

#include "Puppeteer.h"
#include "motion/PuppeteerMotion.h"

using namespace lila;

LILA_EXTENSION_EXPORT int lilaExtensionInit(ext::Context& ctx)
{
    if (!puppeteer::registerCharacter(ctx) || !puppeteer::registerRigElements(ctx))
        return 1;
    puppeteer::registerHands(ctx);
    puppeteer::registerProps(ctx);
    puppeteer::registerControllers(ctx);
    puppeteer::registerJointTools(ctx);
    puppeteer::registerFullBody(ctx);
    puppeteer::registerStrings(ctx);
    puppeteer::registerSpinnerOps(ctx);
    puppeteer::registerControllerDrawing(ctx);
    puppeteer::registerSpinner(ctx);
    puppeteer::registerStringsUi(ctx);
    puppeteer::registerLineOfActionTool(ctx);
    puppeteer::registerBreakdownPartsTool(ctx);
    puppeteer::registerRulesEditor(ctx);
    puppeteer::registerCurveOverlay(ctx);
    puppeteer::registerModes(ctx);
    puppeteer::registerAnimProxy(ctx);
    puppeteer::registerGrapple(ctx);

    // Animate-mode panels (anim/): add further anim registration calls here.
    puppeteer::registerAnimPanels(ctx);
    puppeteer::registerHandUi(ctx);
    puppeteer::registerPropUi(ctx);
    puppeteer::motionfill::registerCommands(ctx);
    puppeteer::motionui::registerUi(ctx);
    return 0;
}
