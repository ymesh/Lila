/*
 *  Copyright (c) 2020 Jeremy HU <jeremy-at-dust3d dot org>. All rights reserved. 
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 */
#ifndef AUTO_REMESHER_PARAMETERIZER_H
#define AUTO_REMESHER_PARAMETERIZER_H
#include <AutoRemesher/FrameField>
#include <AutoRemesher/Progress>
#include <AutoRemesher/QuadParameterizer>
#include <AutoRemesher/Vector2>
#include <AutoRemesher/Vector3>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace AutoRemesher {

class SurfaceMesh;

class Parameterizer {
public:
    Parameterizer(const std::vector<Vector3>* vertices,
        const std::vector<std::vector<size_t>>* triangles,
        const std::vector<Vector3>* triangleFieldVectors)
        : m_vertices(vertices)
        , m_triangles(triangles)
        , m_triangleFieldVectors(triangleFieldVectors)
    {
    }

    std::unique_ptr<std::vector<std::vector<Vector2>>> takeTriangleUvs()
    {
        return std::move(m_triangleUvs);
    }

    const std::vector<std::vector<Vector2>>& originalTriangleUvs() const
    {
        return m_originalTriangleUvs;
    }

    const std::vector<Vector3>& singularVertexPositions() const
    {
        return m_singularVertexPositions;
    }

    const std::vector<size_t>& singularVertexIndices() const
    {
        return m_singularVertexIndices;
    }

    void setScaling(double scaling)
    {
        m_scaling = scaling;
    }

    void setGradientAdaptivity(double adaptivity)
    {
        m_adaptivity = adaptivity;
    }

    void setSharpEdgeDegrees(double degrees)
    {
        m_sharpEdgeDegrees = degrees;
    }

    void setAnisotropy(double anisotropy)
    {
        m_anisotropy = anisotropy;
    }

    void setSingularitySimplification(bool simplify)
    {
        m_singularitySimplification = simplify;
    }

    void setMaximumSingularityPairDistance(size_t faceHops)
    {
        m_maximumSingularityPairDistance = faceHops;
    }

    void setProgressHandler(ProgressHandler progressHandler)
    {
        m_progressHandler = std::move(progressHandler);
    }

    /** Feature edges by SurfaceMesh::edgeKey; when set they replace the sharp edge angle. */
    void setFeatureEdges(const std::unordered_set<uint64_t>* edges) { m_featureEdges = edges; }
    /** Vertices the quads must have a corner on. */
    void setPinnedVertices(const std::vector<size_t>* vertices) { m_pinnedVertices = vertices; }
    /** Per triangle quad size relative to the scaling; replaces the curvature adaptivity when set. */
    void setFaceScaling(const std::vector<double>* scaling) { m_faceScalingOverride = scaling; }
    void setFaceHints(const std::vector<FaceHint>* hints) { m_faceHints = hints; }
    void setFieldStiffness(const std::vector<double>* stiffness) { m_fieldStiffness = stiffness; }
    /** How far the anisotropy may stretch a quad, long side over short. */
    void setMaxAspectRatio(double ratio) { m_maxAspectRatio = ratio; }
    /** Passes of smoothing over the curvature scaling field before it is used; 0 leaves it per triangle. */
    void setScalingSmoothing(int passes) { m_scalingSmoothing = passes; }
    /**
     * Per triangle, nonzero for faces whose shape is not to be read as curvature (a cap closing an open border): they
     * take the neutral size and don't turn the normals of the vertices they share.
     */
    void setUnsizedFaces(const std::vector<uint8_t>* faces) { m_unsizedFaces = faces; }
    /** Per triangle, a factor on the quad size the curvature adaptivity gives. */
    void setFaceScalingMultiplier(const std::vector<double>* multiplier) { m_faceScalingMultiplier = multiplier; }
    /** Per triangle, the longest quad edge allowed there, as a length; anything not above zero is no limit. */
    void setFaceSizeLimit(const std::vector<double>* limit) { m_faceSizeLimit = limit; }
    /** Field directions for single faces, put in place of the solved ones before the cover. */
    void setFieldOverrides(const std::vector<std::pair<size_t, Vector3>>* overrides) { m_fieldOverrides = overrides; }
    /** See CoverConstraints::forcedCorners. */
    void setForcedCorners(const std::unordered_set<size_t>* corners) { m_forcedCorners = corners; }
    /** Sets the scaling from the quad count wanted, once the scaling field is known; 0 keeps the scaling as set. */
    void setTargetQuadCount(double count) { m_targetQuadCount = count; }

    /** Solves and simplifies the cross field only; singularVertexIndices and singularVertexCharges are set after. */
    bool solveField();
    /** Per singular vertex, its charge: 1 for a three-edge pole, -1 for a five-edge pole. */
    const std::vector<int>& singularVertexCharges() const { return m_singularVertexCharges; }
    const std::vector<Vector3>& field() const { return m_field; }

    bool parameterize();

private:
    const std::vector<Vector3>* m_vertices = nullptr;
    const std::vector<std::vector<size_t>>* m_triangles = nullptr;
    const std::vector<Vector3>* m_triangleFieldVectors = nullptr;
    std::unique_ptr<std::vector<std::vector<Vector2>>> m_triangleUvs;
    std::vector<Vector3> m_singularVertexPositions;
    std::vector<size_t> m_singularVertexIndices;
    std::vector<std::vector<Vector2>> m_originalTriangleUvs;
    double m_scaling = 1.0;
    double m_adaptivity = 0.5;
    double m_sharpEdgeDegrees = 90.0;
    double m_anisotropy = 1.0;
    double m_maxAspectRatio = 2.3;
    bool m_singularitySimplification = true;
    size_t m_maximumSingularityPairDistance = 6;
    ProgressHandler m_progressHandler;
    const std::unordered_set<uint64_t>* m_featureEdges = nullptr;
    const std::vector<size_t>* m_pinnedVertices = nullptr;
    const std::vector<double>* m_faceScalingOverride = nullptr;
    const std::vector<FaceHint>* m_faceHints = nullptr;
    const std::vector<double>* m_fieldStiffness = nullptr;
    double m_targetQuadCount = 0.0;
    const std::vector<std::pair<size_t, Vector3>>* m_fieldOverrides = nullptr;
    const std::unordered_set<size_t>* m_forcedCorners = nullptr;
    int m_scalingSmoothing = 0;
    const std::vector<uint8_t>* m_unsizedFaces = nullptr;
    const std::vector<double>* m_faceScalingMultiplier = nullptr;
    const std::vector<double>* m_faceSizeLimit = nullptr;
    std::vector<Vector3> m_field;
    std::vector<int> m_singularVertexCharges;

    bool solveFieldOn(const SurfaceMesh& topology);
    std::vector<double> computeFaceScalingField(const std::vector<Vector3>& vertices,
        const std::vector<std::vector<size_t>>& triangles,
        const std::vector<Vector3>& vertexNormals,
        const std::vector<std::vector<size_t>>& faceAroundVertexMap) const;
};

}

#endif
