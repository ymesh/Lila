/*
 *  Copyright (c) 2026 Jeremy HU <jeremy-at-dust3d dot org>. All rights reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 */
#include <AutoRemesher/ConstrainedLeastSquares>
#include <AutoRemesher/FrameField>
#include <AutoRemesher/Parameterizer>
#include <AutoRemesher/QuadParameterizer>
#include <AutoRemesher/SingularitySimplifier>
#include <AutoRemesher/SurfaceMesh>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <tbb/blocked_range.h>
#include <tbb/combinable.h>
#include <tbb/parallel_for.h>

namespace AutoRemesher {

namespace {

    std::vector<double> computeConformalScaling(const SurfaceMesh& mesh,
        const std::vector<int>& vertexCharges,
        const std::vector<double>& desiredFaceScaling,
        double fitting)
    {
        const size_t vertexCount = mesh.vertexCount(), faceCount = mesh.faceCount();
        std::vector<double> cotanWeight(mesh.cornerCount(), 0.0);
        std::vector<double> angleSum(vertexCount, 0.0), faceArea(faceCount, 0.0);
        std::vector<char> onBoundary(vertexCount, 0);
        for (size_t f = 0; f < faceCount; ++f) {
            for (size_t l = 0; l < 3; ++l) {
                const size_t c = 3 * f + l;
                const Vector3 first = mesh.edgeVector(c);
                const Vector3 second = -mesh.edgeVector(mesh.previousCorner(c));
                const double cross = Vector3::crossProduct(first, second).length();
                const double dot = Vector3::dotProduct(first, second);
                angleSum[mesh.cornerVertex(c)] += std::atan2(cross, dot);
                if (cross > 1e-20)
                    cotanWeight[mesh.nextCorner(c)] = .5 * dot / cross;
                faceArea[f] += cross / 6.0;
                if (mesh.oppositeCorner(c) == SurfaceMesh::npos) {
                    onBoundary[mesh.cornerVertex(c)] = 1;
                    onBoundary[mesh.cornerVertex(mesh.nextCorner(c))] = 1;
                }
            }
        }

        std::vector<double> desiredVertexScaling(vertexCount, 0.0), vertexWeight(vertexCount, 0.0);
        for (size_t f = 0; f < faceCount; ++f) {
            const double logScaling = std::log(std::max(1e-6, desiredFaceScaling[f]));
            for (size_t l = 0; l < 3; ++l) {
                const size_t v = mesh.cornerVertex(3 * f + l);
                desiredVertexScaling[v] += faceArea[f] * logScaling;
                vertexWeight[v] += faceArea[f];
            }
        }
        for (size_t v = 0; v < vertexCount; ++v)
            desiredVertexScaling[v] = vertexWeight[v] > 0 ? desiredVertexScaling[v] / vertexWeight[v] : 0.0;

        std::vector<double> deficit(vertexCount, 0.0);
        for (size_t v = 0; v < vertexCount; ++v) {
            const double gaussian = (onBoundary[v] ? M_PI : 2.0 * M_PI) - angleSum[v];
            const int charge = v < vertexCharges.size() ? vertexCharges[v] : 0;
            const double cone = .5 * M_PI * double(charge > 2 ? charge - 4 : charge);
            deficit[v] = gaussian - cone;
        }

        ConstrainedLeastSquares system(vertexCount);
        std::vector<std::pair<size_t, double>> row;
        for (size_t v = 0; v < vertexCount; ++v) {
            if (onBoundary[v] || vertexWeight[v] <= 0.0)
                continue;
            row.clear();
            double diagonal = 0.0;
            for (const size_t c : mesh.cornersAroundVertex(v)) {
                const size_t opposite = mesh.oppositeCorner(c);
                if (opposite == SurfaceMesh::npos)
                    continue;
                const double weight = cotanWeight[c] + cotanWeight[opposite];
                if (0.0 == weight)
                    continue;
                row.push_back({ mesh.cornerVertex(mesh.nextCorner(c)), -weight });
                diagonal += weight;
            }
            if (row.empty())
                continue;
            row.push_back({ v, diagonal });
            system.addEnergy(row, deficit[v], 1.0);
        }
        for (size_t v = 0; v < vertexCount; ++v)
            system.addEnergy({ { v, 1.0 } }, desiredVertexScaling[v], fitting);

        std::vector<double> logScaling;
        if (!system.solve(&logScaling) || logScaling.size() != vertexCount)
            return desiredFaceScaling;

        std::vector<double> result(faceCount, 1.0);
        for (size_t f = 0; f < faceCount; ++f) {
            double total = 0.0;
            for (size_t l = 0; l < 3; ++l)
                total += logScaling[mesh.cornerVertex(3 * f + l)];
            result[f] = total / 3.0;
        }
        double solvedDensity = 0.0, desiredDensity = 0.0;
        for (size_t f = 0; f < faceCount; ++f) {
            solvedDensity += faceArea[f] * std::exp(-2.0 * result[f]);
            desiredDensity += faceArea[f] / (desiredFaceScaling[f] * desiredFaceScaling[f]);
        }
        if (solvedDensity > 0.0 && desiredDensity > 0.0) {
            const double shift = .5 * std::log(solvedDensity / desiredDensity);
            for (double& value : result)
                value -= shift;
        }
        for (double& value : result)
            value = std::max(.2, std::min(5.0, std::exp(value)));
        return result;
    }

    inline double quadraticForm(const double* T, const Vector3& t)
    {
        return T[0] * t.x() * t.x() + T[2] * t.y() * t.y() + T[5] * t.z() * t.z()
            + 2.0 * (T[1] * t.x() * t.y() + T[3] * t.x() * t.z() + T[4] * t.y() * t.z());
    }

    void computeFaceAnisotropyField(const SurfaceMesh& mesh,
        const std::vector<Vector3>& field, double anisotropy, double maxAspectRatio,
        std::vector<double>* scalingU, std::vector<double>* scalingV)
    {
        scalingU->assign(mesh.faceCount(), 1.0);
        scalingV->assign(mesh.faceCount(), 1.0);
        if (anisotropy <= 0.0 || maxAspectRatio <= 1.0 || field.size() != mesh.faceCount())
            return;
        std::vector<double> tensor(mesh.vertexCount() * 6, 0.0);
        std::vector<std::vector<size_t>> neighbors(mesh.vertexCount());
        for (size_t c = 0; c < mesh.cornerCount(); ++c) {
            const size_t mate = mesh.oppositeCorner(c);
            if (mate == SurfaceMesh::npos || mate < c)
                continue;
            // A feature edge's bend is a crease, not curvature to stretch the quads along.
            if (mesh.hasFeatureEdges() && mesh.isFeature(c, 0.0))
                continue;
            const Vector3 edge = mesh.edgeVector(c);
            const double length = edge.length();
            if (length <= 0.0)
                continue;
            const Vector3 d = edge / length;
            const double weight = length * mesh.normalAngle(c);
            const double contribution[6] = { weight * d.x() * d.x(), weight * d.x() * d.y(), weight * d.y() * d.y(), weight * d.x() * d.z(), weight * d.y() * d.z(), weight * d.z() * d.z() };
            const size_t a = mesh.cornerVertex(c), b = mesh.cornerVertex(mesh.nextCorner(c));
            for (size_t i = 0; i < 6; ++i) {
                tensor[6 * a + i] += contribution[i];
                tensor[6 * b + i] += contribution[i];
            }
        }
        for (size_t c = 0; c < mesh.cornerCount(); ++c) {
            const size_t a = mesh.cornerVertex(c), b = mesh.cornerVertex(mesh.nextCorner(c));
            neighbors[a].push_back(b);
            neighbors[b].push_back(a);
        }
        std::vector<double> smoothed(tensor.size());
        for (size_t pass = 0; pass < 12; ++pass) {
            for (size_t v = 0; v < mesh.vertexCount(); ++v) {
                if (neighbors[v].empty()) {
                    for (size_t i = 0; i < 6; ++i)
                        smoothed[6 * v + i] = tensor[6 * v + i];
                    continue;
                }
                for (size_t i = 0; i < 6; ++i) {
                    double average = 0;
                    for (size_t n : neighbors[v])
                        average += tensor[6 * n + i];
                    smoothed[6 * v + i] = .5 * tensor[6 * v + i] + .5 * average / neighbors[v].size();
                }
            }
            tensor.swap(smoothed);
        }
        std::vector<double> alongU(mesh.faceCount()), alongV(mesh.faceCount());
        double total = 0;
        for (size_t f = 0; f < mesh.faceCount(); ++f) {
            double T[6] = {};
            for (size_t l = 0; l < 3; ++l) {
                const size_t v = mesh.cornerVertex(3 * f + l);
                for (size_t i = 0; i < 6; ++i)
                    T[i] += tensor[6 * v + i];
            }
            const Vector3 n = mesh.faceNormal(f), b = field[f].normalized(), bt = Vector3::crossProduct(n, b);
            alongU[f] = std::fabs(quadraticForm(T, bt));
            alongV[f] = std::fabs(quadraticForm(T, b));
            total += alongU[f] + alongV[f];
        }
        if (total <= 0)
            return;
        const double floor = .001 * total / (2.0 * mesh.faceCount()), maxRho = std::sqrt(maxAspectRatio);
        for (size_t f = 0; f < mesh.faceCount(); ++f) {
            double rho = std::pow((alongV[f] + floor) / (alongU[f] + floor), .25);
            rho = std::pow(rho, anisotropy);
            rho = std::max(1.0 / maxRho, std::min(maxRho, rho));
            (*scalingU)[f] = rho;
            (*scalingV)[f] = 1.0 / rho;
        }
    }

}

std::vector<double> Parameterizer::computeFaceScalingField(const std::vector<Vector3>& vertices,
    const std::vector<std::vector<size_t>>& triangles,
    const std::vector<Vector3>& vertexNormals,
    const std::vector<std::vector<size_t>>& faceAroundVertexMap) const
{
    std::vector<double> faceScaling(triangles.size(), 1.0);
    if (m_adaptivity <= 0.0 || vertices.empty())
        return faceScaling;

    std::vector<double> vertexCurvature(vertices.size(), 0.0);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, vertices.size()),
        [&](const tbb::blocked_range<size_t>& range) {
            for (size_t v = range.begin(); v != range.end(); ++v) {
                const auto& facesAroundVertex = faceAroundVertexMap[v];
                if (facesAroundVertex.empty())
                    continue;
                const auto& normalV = vertexNormals[v];
                double maxCurvature = 0.0;
                for (const auto& faceIndex : facesAroundVertex) {
                    for (const auto& u : triangles[faceIndex]) {
                        if (u == v)
                            continue;
                        double distance = (vertices[u] - vertices[v]).length();
                        if (distance <= 0.0)
                            continue;
                        // A vertex with only unsized faces round it has no normal to compare.
                        if (normalV.lengthSquared() <= 0.0 || vertexNormals[u].lengthSquared() <= 0.0)
                            continue;
                        double cosAngle = Vector3::dotProduct(normalV, vertexNormals[u]);
                        if (cosAngle > 1.0)
                            cosAngle = 1.0;
                        else if (cosAngle < -1.0)
                            cosAngle = -1.0;
                        double curvature = std::acos(cosAngle) / distance;
                        if (curvature > maxCurvature)
                            maxCurvature = curvature;
                    }
                }
                vertexCurvature[v] = maxCurvature;
            }
        });

    double sumCurvature = 0.0;
    for (const auto& it : vertexCurvature)
        sumCurvature += it;
    double averageCurvature = sumCurvature / vertexCurvature.size();
    if (averageCurvature <= 0.0)
        return faceScaling;

    const double minRatio = 0.3;
    const double maxRatio = 3.0;
    tbb::parallel_for(tbb::blocked_range<size_t>(0, triangles.size()),
        [&](const tbb::blocked_range<size_t>& range) {
            for (size_t i = range.begin(); i != range.end(); ++i) {
                const auto& triangle = triangles[i];
                double faceCurvature = 0.0;
                for (const auto& v : triangle)
                    faceCurvature += vertexCurvature[v];
                faceCurvature /= triangle.size();
                double normalized = faceCurvature / averageCurvature;
                if (normalized < 1e-3)
                    normalized = 1e-3;
                double multiplier = std::pow(normalized, -m_adaptivity);
                if (multiplier < minRatio)
                    multiplier = minRatio;
                else if (multiplier > maxRatio)
                    multiplier = maxRatio;
                faceScaling[i] = multiplier;
            }
        });

    return faceScaling;
}

bool Parameterizer::solveFieldOn(const SurfaceMesh& topology)
{
    m_field.clear();
    if (nullptr != m_triangleFieldVectors) {
        m_field = *m_triangleFieldVectors;
    } else if (!FrameField::create(topology, m_sharpEdgeDegrees, &m_field, m_faceHints, m_fieldStiffness)) {
        AutoRemesher::log() << "Frame field solve failed" << std::endl;
        return false;
    }
    if (m_field.size() != topology.faceCount()) {
        AutoRemesher::log() << "Frame field has the wrong face count" << std::endl;
        return false;
    }
    SingularitySimplifier simplifier(topology, &m_field);
    if (m_singularitySimplification) {
        if (m_progressHandler)
            m_progressHandler(0.17f, "Simplifying singularities");
        simplifier.setSharpEdgeDegrees(m_sharpEdgeDegrees);
        simplifier.setMaximumPairDistance(m_maximumSingularityPairDistance);
        simplifier.simplify();
    }
    if (m_fieldOverrides)
        for (const auto& it : *m_fieldOverrides)
            if (it.first < m_field.size())
                m_field[it.first] = it.second;
    const std::vector<int> charges = simplifier.vertexCharges();
    m_singularVertexIndices.clear();
    m_singularVertexCharges.clear();
    for (size_t v = 0; v < charges.size(); ++v)
        if (charges[v] != 0) {
            m_singularVertexIndices.push_back(v);
            m_singularVertexCharges.push_back(charges[v] > 2 ? charges[v] - 4 : charges[v]);
        }
    return true;
}

bool Parameterizer::solveField()
{
    SurfaceMesh topology(*m_vertices, *m_triangles);
    if (topology.faceCount() != m_triangles->size())
        return false;
    if (m_featureEdges)
        topology.setFeatureEdges(m_featureEdges);
    return solveFieldOn(topology);
}

bool Parameterizer::parameterize()
{
#if AUTO_REMESHER_DEV
    {
        FILE* fp = fopen("debug-input-for-parameterization.obj", "wb");
        for (size_t i = 0; i < m_vertices->size(); ++i) {
            const auto& vertex = (*m_vertices)[i];
            fprintf(fp, "v %f %f %f\n", vertex[0], vertex[1], vertex[2]);
        }
        for (size_t i = 0; i < m_triangles->size(); ++i) {
            const auto& indices = (*m_triangles)[i];
            fprintf(fp, "f %zu %zu %zu\n", indices[0] + 1, indices[1] + 1, indices[2] + 1);
        }
        fclose(fp);
    }
#endif

    // The fractions below are the measured share of parameterization each step
    // costs; "Solving quad cover" dominates, so the bar has to keep moving
    // through it rather than sitting still until it finishes.
    const auto report = [this](float fraction, const char* name) {
        if (m_progressHandler)
            m_progressHandler(fraction, name);
    };

    report(0.0f, "Computing vertex normals");
    std::vector<Vector3> vertexNormals(m_vertices->size());
    {
        std::vector<Vector3> faceNormals(m_triangles->size());
        tbb::parallel_for(tbb::blocked_range<size_t>(0, m_triangles->size()),
            [&](const tbb::blocked_range<size_t>& range) {
                for (size_t i = range.begin(); i != range.end(); ++i) {
                    const auto& it = (*m_triangles)[i];
                    faceNormals[i] = Vector3::normal(
                        (*m_vertices)[it[0]], (*m_vertices)[it[1]], (*m_vertices)[it[2]]);
                }
            });
        const bool unsized = m_unsizedFaces && m_unsizedFaces->size() == m_triangles->size();
        for (size_t i = 0; i < m_triangles->size(); ++i) {
            if (unsized && (*m_unsizedFaces)[i])
                continue;
            const auto& it = (*m_triangles)[i];
            vertexNormals[it[0]] += faceNormals[i];
            vertexNormals[it[1]] += faceNormals[i];
            vertexNormals[it[2]] += faceNormals[i];
        }
        tbb::parallel_for(tbb::blocked_range<size_t>(0, vertexNormals.size()),
            [&](const tbb::blocked_range<size_t>& range) {
                for (size_t i = range.begin(); i != range.end(); ++i)
                    vertexNormals[i].normalize();
            });
    }

    report(0.01f, "Computing scaling field");
    std::vector<double> faceScalingField;
    if (m_faceScalingOverride && m_faceScalingOverride->size() == m_triangles->size()) {
        faceScalingField = *m_faceScalingOverride;
    } else {
        std::vector<std::vector<size_t>> faceAroundVertexMap(m_vertices->size());
        for (size_t i = 0; i < m_triangles->size(); ++i) {
            const auto& it = (*m_triangles)[i];
            faceAroundVertexMap[it[0]].push_back(i);
            faceAroundVertexMap[it[1]].push_back(i);
            faceAroundVertexMap[it[2]].push_back(i);
        }
        faceScalingField = computeFaceScalingField(*m_vertices,
            *m_triangles, vertexNormals, faceAroundVertexMap);
        if (m_unsizedFaces && m_unsizedFaces->size() == faceScalingField.size())
            for (size_t i = 0; i < faceScalingField.size(); ++i)
                if ((*m_unsizedFaces)[i])
                    faceScalingField[i] = 1.0;
    }

    report(0.02f, "Building surface topology");
    SurfaceMesh topology(*m_vertices, *m_triangles);
    if (topology.faceCount() != m_triangles->size()) {
        AutoRemesher::log() << "Topology rejected a non-triangle face" << std::endl;
        return false;
    }
    if (m_featureEdges)
        topology.setFeatureEdges(m_featureEdges);
    if (m_scalingSmoothing > 0) {
        // Averaged through the vertices, in log so halving and doubling weigh alike.
        std::vector<double> at(m_vertices->size()), weight(m_vertices->size());
        for (int pass = 0; pass < m_scalingSmoothing; ++pass) {
            std::fill(at.begin(), at.end(), 0.0);
            std::fill(weight.begin(), weight.end(), 0.0);
            for (size_t f = 0; f < m_triangles->size(); ++f)
                for (size_t v : (*m_triangles)[f]) {
                    at[v] += std::log(faceScalingField[f]);
                    weight[v] += 1.0;
                }
            for (size_t f = 0; f < m_triangles->size(); ++f) {
                double sum = 0.0;
                for (size_t v : (*m_triangles)[f])
                    sum += at[v] / std::max(1.0, weight[v]);
                faceScalingField[f] = std::exp(sum / 3.0);
            }
        }
    }
    if (m_faceScalingMultiplier && m_faceScalingMultiplier->size() == faceScalingField.size())
        for (size_t f = 0; f < faceScalingField.size(); ++f)
            faceScalingField[f] *= (*m_faceScalingMultiplier)[f];
    const bool counted = m_uncountedFaces && m_uncountedFaces->size() == m_triangles->size();
    if (m_targetQuadCount > 0.0) {
        // A quad on face f has area (scale * faceScaling[f])^2, so the count is the sum of area over that.
        double quads = 0.0;
        for (size_t f = 0; f < m_triangles->size(); ++f) {
            if (counted && (*m_uncountedFaces)[f])
                continue;
            const auto& it = (*m_triangles)[f];
            quads += Vector3::area((*m_vertices)[it[0]], (*m_vertices)[it[1]], (*m_vertices)[it[2]])
                / (faceScalingField[f] * faceScalingField[f]);
        }
        const double edge = topology.averageEdgeLength();
        if (quads > 0.0 && edge > 0.0)
            m_scaling = std::sqrt(quads / m_targetQuadCount) / edge;
    }

    report(0.03f, "Solving frame field");
    if (!solveFieldOn(topology))
        return false;
    std::vector<Vector3>& field = m_field;

    std::vector<double> faceScalingU(m_triangles->size(), 1.0);
    std::vector<double> faceScalingV(m_triangles->size(), 1.0);
    if (m_anisotropy > 0.0) {
        report(0.26f, "Computing anisotropy field");
        computeFaceAnisotropyField(topology, field,
            m_anisotropy, m_maxAspectRatio, &faceScalingU, &faceScalingV);
    }
    // The cover solve is the longest single step here, so it reports its own
    // sub-steps from 0.28 onwards rather than going quiet until it finishes.
    ProgressHandler coverProgress;
    if (m_progressHandler) {
        coverProgress = [this](float fraction, const char* name) {
            m_progressHandler(0.28f + (0.99f - 0.28f) * fraction, name);
        };
    }
    QuadParameterizer::Result cover;
    CoverConstraints constraints;
    constraints.featureEdges = m_featureEdges;
    constraints.pinnedVertices = m_pinnedVertices;
    constraints.forcedCorners = m_forcedCorners;
    // Each quad is a unit square of the cover, so its area in uv is the count it will give. The curl correction and the
    // rounding move that off the count the scaling asked for, so the cover is solved again at a corrected scaling.
    const bool limited = m_faceSizeLimit && m_faceSizeLimit->size() == faceScalingField.size();
    std::vector<double> limitedScalingField;
    // The count comes in steps (a whole number of quads round every loop), so the scaling is searched: corrected in
    // proportion until the count is bracketed, then halved between the two sides, and the closest cover is kept.
    QuadParameterizer::Result closest;
    double closestOff = -1.0, below = 0.0, above = 0.0;
    const int attempts = m_targetQuadCount > 0.0 ? (m_triangles->size() < 30000 ? 5 : 3) : 1;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        cover = QuadParameterizer::Result();
        // The count asked for is the layout's before the limits; the quads they add come on top, or the scaling would
        // chase a count the limited faces can't give up.
        double wanted = m_targetQuadCount;
        if (limited) {
            const double scale = m_scaling * topology.averageEdgeLength();
            limitedScalingField = faceScalingField;
            double with = 0.0, without = 0.0;
            for (size_t f = 0; f < limitedScalingField.size(); ++f) {
                if ((*m_faceSizeLimit)[f] > 0.0 && scale > 0.0)
                    limitedScalingField[f] = std::min(limitedScalingField[f], (*m_faceSizeLimit)[f] / scale);
                if (counted && (*m_uncountedFaces)[f])
                    continue;
                const auto& it = (*m_triangles)[f];
                const double area = Vector3::area((*m_vertices)[it[0]], (*m_vertices)[it[1]], (*m_vertices)[it[2]]);
                with += area / (limitedScalingField[f] * limitedScalingField[f]);
                without += area / (faceScalingField[f] * faceScalingField[f]);
            }
            if (without > 0.0)
                wanted *= with / without;
        }
        if (!QuadParameterizer::parameterize(*m_vertices, *m_triangles,
                &field, m_scaling, m_sharpEdgeDegrees, &cover,
                limited ? &limitedScalingField : &faceScalingField, &faceScalingU, &faceScalingV,
                coverProgress ? &coverProgress : nullptr, &constraints)) {
            if (closestOff >= 0.0)
                break;
            AutoRemesher::log() << "Quad cover solve failed" << std::endl;
            return false;
        }
        if (m_targetQuadCount <= 0.0) {
            closest = std::move(cover);
            closestOff = 0.0;
            break;
        }
        double quads = 0.0;
        for (size_t f = 0; f < cover.triangleUvs.size(); ++f) {
            if (counted && (*m_uncountedFaces)[f])
                continue;
            const auto& uv = cover.triangleUvs[f];
            quads += .5 * std::fabs((uv[1].x() - uv[0].x()) * (uv[2].y() - uv[0].y()) - (uv[1].y() - uv[0].y()) * (uv[2].x() - uv[0].x()));
        }
        AutoRemesher::log() << "Cover holds " << quads << " quads of " << wanted << " at scaling " << m_scaling << std::endl;
        const double off = quads > 0.0 ? std::fabs(quads / wanted - 1.0) : 1e9;
        if (closestOff < 0.0 || off < closestOff) {
            closest = std::move(cover);
            closestOff = off;
        }
        if (quads <= 0.0 || off < .04)
            break;
        // A larger scaling gives fewer quads.
        (quads > wanted ? below : above) = m_scaling;
        m_scaling = below > 0.0 && above > 0.0 ? std::sqrt(below * above) : m_scaling * std::sqrt(quads / wanted);
    }
    cover = std::move(closest);
    report(0.99f, "Collecting singularities");
    m_originalTriangleUvs = cover.triangleUvs;
    m_triangleUvs = std::make_unique<std::vector<std::vector<Vector2>>>(cover.triangleUvs);
    m_singularVertexPositions.clear();
    m_singularVertexIndices.clear();
    for (const size_t v : cover.singularVertices) {
        if (v >= m_vertices->size())
            continue;
        m_singularVertexPositions.push_back((*m_vertices)[v]);
        m_singularVertexIndices.push_back(v);
    }
    report(1.0f, "");
    return true;
}

}
