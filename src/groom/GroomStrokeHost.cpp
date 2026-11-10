// Adapted from GroomLab's SessionStrokeHost (stroke.cpp, github.com/thekesslereffect/groomlab, Apache-2.0,
// see src/groom/groomlab/LICENSE and NOTICE).
#include "groom/GroomStrokeHost.h"

#include <groomlab/texture.h>

#include <stdexcept>

namespace lila::groom
{
using namespace groomlab;

void applyStrokeEdit(const StrokeEdit& edit, GroomEvaluator& evaluator, bool after)
{
    for (std::size_t i = 0; i < kStrokeFields; ++i)
    {
        const StrokeEdit::Text& t = edit.texts[i];
        if (!t.set || (!after && !t.hasBefore))
            continue;
        const std::string& text = after ? t.after : t.before;
        switch (static_cast<StrokeField>(i))
        {
        case StrokeField::Guides: evaluator.setHierarchyJson(text); break;
        case StrokeField::Parts: evaluator.setPartsJson(text); break;
        case StrokeField::Flow: evaluator.setFlowJson(text); break;
        case StrokeField::Correctives: evaluator.setCorrectivesJson(text); break;
        case StrokeField::ScalpMaps: evaluator.setScalpMapsJson(text); break;
        case StrokeField::ShotLayer: break; // the host's
        case StrokeField::Graph: evaluator.setGraphJson(text); break;
        case StrokeField::Sculpts: evaluator.setSculptsJson(text); break;
        }
    }
}

GroomStrokeHost::GroomStrokeHost(GroomEvaluator& evaluator) : mEvaluator(evaluator) {}

MeshPtr GroomStrokeHost::scalp()
{
    return mEvaluator.scalp();
}

MeshPtr GroomStrokeHost::paintScalp()
{
    return mEvaluator.cageScalp();
}

std::shared_ptr<const GuideHierarchy> GroomStrokeHost::authoredHierarchy()
{
    return mEvaluator.authoredHierarchy();
}

std::shared_ptr<const GuideHierarchy> GroomStrokeHost::shownHierarchy()
{
    if (!mEvaluator.sculptTarget().empty())
        if (const HierarchyPtr sculpted = mEvaluator.sculptHierarchy())
            return sculpted;
    return mEvaluator.hierarchy();
}

std::string GroomStrokeHost::sculptNode()
{
    return mEvaluator.sculptTarget();
}

SculptInput GroomStrokeHost::sculptInput(const std::string& node)
{
    return mEvaluator.sculptInput(node);
}

void GroomStrokeHost::previewSculpt(const std::string& node, SculptPtr edits, std::shared_ptr<const GuideHierarchy> shown)
{
    mEvaluator.setPreviewSculpt(node, std::move(edits), std::move(shown));
}

std::shared_ptr<const GuideHierarchy> GroomStrokeHost::posedHierarchy()
{
    mEvaluator.hierarchy(); // the active hierarchy, and the posed one, brought up to date
    return mEvaluator.posedHierarchy();
}

std::string GroomStrokeHost::storedText(StrokeField field)
{
    switch (field)
    {
    case StrokeField::Guides:
    {
        // Guides handed over as a hierarchy (setHierarchy) have no text: theirs.
        const std::shared_ptr<const GuideHierarchy> stored = mEvaluator.storedHierarchy();
        return mEvaluator.data().hierarchyJson.empty() && stored ? stored->toJson() : mEvaluator.data().hierarchyJson;
    }
    case StrokeField::Parts: return mEvaluator.data().partsJson;
    case StrokeField::Flow: return mEvaluator.data().flowJson;
    case StrokeField::Correctives: return mEvaluator.data().correctivesJson;
    case StrokeField::ScalpMaps: return mEvaluator.scalpMapsJson();
    case StrokeField::ShotLayer: return mShotLayer;
    case StrokeField::Graph: return mEvaluator.data().graphJson;
    case StrokeField::Sculpts: return mEvaluator.data().sculptsJson;
    }
    return {};
}

const ScalpMaps& GroomStrokeHost::scalpMaps()
{
    return mEvaluator.scalpMaps();
}

std::shared_ptr<TexturePaint> GroomStrokeHost::texturePaint(const std::string& name, int, int)
{
    const auto it = mPaints.find(name);
    if (it == mPaints.end() || !it->second)
        throw std::runtime_error("no paint for texture map " + name + " (GroomStrokeHost::setTexturePaint)");
    return it->second;
}

void GroomStrokeHost::previewHierarchy(std::shared_ptr<GuideHierarchy> preview, bool final)
{
    mEvaluator.setPreviewHierarchy(std::move(preview), final);
}

void GroomStrokeHost::previewOverlays(std::shared_ptr<const PartSet> parts, std::shared_ptr<const FlowSet> flow)
{
    mEvaluator.setPreviewOverlays(std::move(parts), std::move(flow));
}

void GroomStrokeHost::previewVertexMap(const std::string& name, std::shared_ptr<const std::vector<float>> values, int channels)
{
    mEvaluator.setPreviewVertexMap(name, std::move(values), channels);
}

void GroomStrokeHost::previewTextureMap(const std::string& name, std::shared_ptr<const TexturePreview> image)
{
    mEvaluator.setPreviewTextureMap(name, std::move(image));
}

void GroomStrokeHost::liveStroke(bool on)
{
    mEvaluator.setLiveStroke(on);
}

void GroomStrokeHost::setShotSculpt(bool on, double seconds, long frame, std::string layer)
{
    mShot = on;
    mSeconds = seconds;
    mFrame = frame;
    mShotLayer = std::move(layer);
}

void GroomStrokeHost::setTexturePaint(const std::string& name, std::shared_ptr<TexturePaint> paint)
{
    if (paint)
        mPaints[name] = std::move(paint);
    else
        mPaints.erase(name);
}

std::shared_ptr<TexturePaint> GroomStrokeHost::paintOf(const std::string& name) const
{
    const auto it = mPaints.find(name);
    return it == mPaints.end() ? nullptr : it->second;
}

void GroomStrokeHost::applyEdit(const StrokeEdit& edit, bool after)
{
    applyStrokeEdit(edit, mEvaluator, after);
    const StrokeEdit::Text& layer = edit.text(StrokeField::ShotLayer);
    if (layer.set && (after || layer.hasBefore))
        mShotLayer = after ? layer.after : layer.before;
}

void GroomStrokeHost::commit(StrokeEdit edit)
{
    for (std::size_t i = 0; i < kStrokeFields; ++i)
    {
        StrokeEdit::Text& t = edit.texts[i];
        if (!t.set)
            continue;
        t.before = storedText(static_cast<StrokeField>(i));
        t.hasBefore = true;
    }
    applyEdit(edit, true); // a texture map's paint holds the stroke already
    mDone.push_back(std::move(edit));
    mUndone.clear();
}

bool GroomStrokeHost::undo()
{
    if (mDone.empty())
        return false;
    StrokeEdit edit = std::move(mDone.back());
    mDone.pop_back();
    applyEdit(edit, false);
    if (!edit.texture.empty())
        if (const std::shared_ptr<TexturePaint> paint = paintOf(edit.textureMap))
            paint->applyEdit(edit.texture, false);
    mUndone.push_back(std::move(edit));
    return true;
}

bool GroomStrokeHost::redo()
{
    if (mUndone.empty())
        return false;
    StrokeEdit edit = std::move(mUndone.back());
    mUndone.pop_back();
    applyEdit(edit, true);
    if (!edit.texture.empty())
        if (const std::shared_ptr<TexturePaint> paint = paintOf(edit.textureMap))
            paint->applyEdit(edit.texture, true);
    mDone.push_back(std::move(edit));
    return true;
}
} // namespace lila::groom
