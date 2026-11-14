#pragma once

#include "uv/FinalMesh.h"
#include "uv/OptimizeMath.h"

#include <Eigen/Dense>
#include <Eigen/SparseCholesky>

#include <array>
#include <complex>
#include <span>
#include <utility>
#include <vector>

namespace lila::uv::detail
{
using CSpMat = Eigen::SparseMatrix<std::complex<double>>;

/**
 * A FinalIsland's exported corner uvs as a linear map of its chart's vertex uvs, y = R x + offset, for the solvers. Each
 * Fit Surface fill is applied by its own solve, and kept as a dense block over the few chart vertices its border reads
 * for R^H S R, so that costs a dense product per fill instead of rows over every vertex the island's border reaches.
 * Weights that rotate make R complex (uvs read as u + iv); an island whose weights all scale only stays real.
 */
class FinalOperator
{
public:
    /** Conjugate reads the island in a frame mirrored in u (weights conjugated), as solves of mirrored islands do. */
    FinalOperator(const FinalIsland& isl, size_t vertexCount, bool conjugate = false);

    size_t corners() const { return mCorners; }
    size_t vertices() const { return mVertices; }
    /** True when some weight rotates, so u and v couple and solves need the complex system. */
    bool rotates() const { return mRotates; }
    /** y for x; offsetScale multiplies the offsets per component ((0, 0) leaves them out, (-1, 1) mirrors them). */
    void apply(std::span<const glm::dvec2> x, std::span<glm::dvec2> y, glm::dvec2 offsetScale) const;
    /** R^H g, per chart vertex. */
    void applyTransposed(std::span<const glm::dvec2> g, std::span<glm::dvec2> out) const;
    /** R^H S R for a symmetric matrix S over the corners: its real part, and its imaginary part (empty unless rotates()). */
    std::pair<SpMat, SpMat> gram(const SpMat& s) const;
    /** Per chart vertex, the sum of |weight| over every corner reading it. */
    std::vector<double> columnWeight() const;

private:
    struct Block
    {
        std::vector<Index> cols;           // chart vertices the fill's border reads
        std::vector<Index> imCols;         // those it reads through weights that rotate
        Eigen::MatrixXd d;                 // interior group x cols: real parts
        Eigen::MatrixXd di;                // interior group x imCols: imaginary parts
        Eigen::MatrixXd off;               // interior group x 2: the fill of the border's offsets
        std::vector<Index> start, corners; // interior group -> local corners
    };
    FinalWeight weight(Index k) const { return mConjugate ? std::conj(mIsl.weights[k]) : mIsl.weights[k]; }

    const FinalIsland& mIsl;
    size_t mCorners = 0, mVertices = 0;
    bool mConjugate = false, mRotates = false;
    SpMat mR, mRi; // real and imaginary parts of the rows
    std::vector<Block> mBlocks;
};

/**
 * The LDLT of a gram matrix from a FinalOperator, solving u and v together. A complex Hermitian system (weights that
 * rotate) is solved with the real part's factor, refined against the imaginary part.
 */
class UvFactor
{
public:
    /** Factors re + i im (im empty for a real system). */
    bool compute(const SpMat& re, const SpMat& im);
    bool ok() const { return mOk; }
    bool solve(const Eigen::VectorXd& bu, const Eigen::VectorXd& bv, Eigen::VectorXd& su, Eigen::VectorXd& sv) const;

private:
    static constexpr int kRefineSteps = 12;
    bool mComplex = false, mOk = false;
    SpMat mRe, mIm;
    Eigen::SimplicialLDLT<SpMat> mReal;
    mutable bool mCxReady = false;
    mutable Eigen::SimplicialLDLT<CSpMat> mCx;
};
/** Cage vertices the export reads by less than this (largest weight) are held to their cage shape, fully below kWeakInfluence. */
inline constexpr double kHoldReach = 0.5;
/** How hard the hold pulls, as a share of the export's mean stiffness per vertex. */
inline constexpr double kCageHold = 0.05;

/**
 * Holds a chart's own shape where the export barely reads it: each cage edge keeps the uv vector it started with, weighed
 * by how weakly the export reads its ends. Without it a solve uses barely read vertices as long levers, flinging them far
 * out (over other islands) to open one more sliver. A quadratic term, (x - x0)^T L (x - x0) with L a weighted Laplacian of
 * the cage.
 */
struct CageHold
{
    /** Whole holds every edge alike, however well the export reads it. */
    CageHold(const Chart& ch, const FinalIsland& isl, std::span<const glm::dvec2> x0, bool whole = false);

    /** The factor that makes the hold the given share of the export's mean stiffness per vertex. */
    double scaleFor(double share, double exportDiag) const { return diagMean > 0.0 ? share * exportDiag / diagMean : 0.0; }
    double energy(std::span<const glm::dvec2> x) const;

    std::vector<Eigen::Triplet<double>> laplacian; // V x V
    std::vector<glm::dvec2> rest;                  // L x0
    std::vector<std::array<Index, 2>> edges;
    std::vector<double> weight;
    std::vector<glm::dvec2> start;                 // per edge, x0[b] - x0[a]
    double diagMean = 0.0;
};
} // namespace lila::uv::detail
