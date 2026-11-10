// Adapted from GroomLab's SessionStrokeHost (stroke.h/.cpp, github.com/thekesslereffect/groomlab, Apache-2.0,
// see src/groom/groomlab/LICENSE and NOTICE).
#pragma once

#include "groom/GroomEvaluator.h"

#include <groomlab/stroke.h>

#include <map>
#include <set>
#include <string>
#include <vector>

namespace lila::groom
{
/** Applies a stroke's edit (its texts after or before) to an evaluator. */
void applyStrokeEdit(const groomlab::StrokeEdit& edit, GroomEvaluator& evaluator, bool after);

/**
 * GroomLab's brushes on a GroomEvaluator: inputs from it, previews to its preview setters, edits applied to it and kept
 * for undo and redo. The selection, hidden groups, shot sculpting and texture maps' paints are set on the host.
 */
class GroomStrokeHost : public groomlab::StrokeHost
{
public:
    explicit GroomStrokeHost(GroomEvaluator& evaluator);

    groomlab::MeshPtr scalp() override;
    groomlab::MeshPtr paintScalp() override;
    std::shared_ptr<const groomlab::GuideHierarchy> authoredHierarchy() override;
    /** The evaluator's sculpt target's guides while it has one. */
    std::shared_ptr<const groomlab::GuideHierarchy> shownHierarchy() override;
    std::uint64_t shownHierarchyVersion() override { return 0; }
    std::string sculptNode() override;
    groomlab::SculptInput sculptInput(const std::string& node) override;
    void previewSculpt(const std::string& node, groomlab::SculptPtr edits, std::shared_ptr<const groomlab::GuideHierarchy> shown) override;
    std::shared_ptr<const groomlab::GuideHierarchy> posedHierarchy() override;
    std::string storedText(groomlab::StrokeField field) override;
    const groomlab::ScalpMaps& scalpMaps() override;
    std::vector<groomlab::GuideId> selectedGuides() override { return mSelection; }
    float softSelectRadius() override { return mSoftRadius; }
    std::set<std::string> hiddenGroups() override { return mHidden; }
    int guideSmoothing() override { return mSmoothing; }
    bool shotSculpt() override { return mShot; }
    double seconds() override { return mSeconds; }
    long frame() override { return mFrame; }
    bool usesLiveCorrectiveWeights() override { return !mWeights.empty(); }
    float liveCorrectiveWeight(int slot) override
    {
        const auto it = mWeights.find(slot);
        return it == mWeights.end() ? 1.0f : it->second;
    }
    std::shared_ptr<groomlab::TexturePaint> texturePaint(const std::string& name, int channels, int resolution) override;
    void previewHierarchy(std::shared_ptr<groomlab::GuideHierarchy> preview, bool final) override;
    void previewOverlays(std::shared_ptr<const groomlab::PartSet> parts, std::shared_ptr<const groomlab::FlowSet> flow) override;
    void previewVertexMap(const std::string& name, std::shared_ptr<const std::vector<float>> values, int channels) override;
    void previewTextureMap(const std::string& name, std::shared_ptr<const groomlab::TexturePreview> image) override;
    void liveStroke(bool on) override;
    /** Records the stored texts before the edit, applies it and keeps it for undo; clears what could be redone. */
    void commit(groomlab::StrokeEdit edit) override;
    void warn(const std::string& message) override { mWarnings.push_back(message); }

    void setSelection(std::vector<groomlab::GuideId> ids, float softRadius = 0.0f)
    {
        mSelection = std::move(ids);
        mSoftRadius = softRadius;
    }
    void setHiddenGroups(std::set<std::string> groups) { mHidden = std::move(groups); }
    void setGuideSmoothing(int smoothing) { mSmoothing = smoothing; }
    void setShotSculpt(bool on, double seconds = 0.0, long frame = 0, std::string layer = {});
    void setCorrectiveWeights(std::map<int, float> weights) { mWeights = std::move(weights); }
    /** A texture map's paint by name (painted by strokes on that map; the host writes no file). */
    void setTexturePaint(const std::string& name, std::shared_ptr<groomlab::TexturePaint> paint);
    std::shared_ptr<groomlab::TexturePaint> paintOf(const std::string& name) const;

    bool undo();
    bool redo();
    const std::vector<groomlab::StrokeEdit>& edits() const { return mDone; }
    const std::vector<std::string>& warnings() const { return mWarnings; }
    void clearWarnings() { mWarnings.clear(); }
    GroomEvaluator& evaluator() { return mEvaluator; }
    GroomEvaluator& session() { return mEvaluator; }

private:
    void applyEdit(const groomlab::StrokeEdit& edit, bool after);

    GroomEvaluator& mEvaluator;
    std::vector<groomlab::GuideId> mSelection;
    float mSoftRadius = 0.0f;
    std::set<std::string> mHidden;
    int mSmoothing = 0;
    bool mShot = false;
    double mSeconds = 0.0;
    long mFrame = 0;
    std::string mShotLayer;
    std::map<int, float> mWeights;
    std::map<std::string, std::shared_ptr<groomlab::TexturePaint>> mPaints;
    std::vector<groomlab::StrokeEdit> mDone, mUndone;
    std::vector<std::string> mWarnings;
};
} // namespace lila::groom
