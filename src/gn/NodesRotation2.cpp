#include "gn/NodesCommon.h"

#include <algorithm>
#include <array>

namespace lila::gn::nodes::rotation2
{
using M3 = glm::dmat3;
using M4 = glm::dmat4;

const std::vector<std::string>& eulerOrders()
{
    static const std::vector<std::string> v = {"XYZ", "XZY", "YXZ", "YZX", "ZXY", "ZYX"};
    return v;
}

M3 axisTurn(int axis, double a)
{
    D3 ax(0.0);
    ax[axis] = 1.0;
    return axisAngle(ax, a);
}

/** Angles about X, Y and Z applied in the order's letters, first letter first. */
M3 orderedEulerToMatrix(const D3& e, const std::string& order)
{
    M3 r(1.0);
    for (char ch : order)
    {
        int a = ch - 'X';
        r = axisTurn(a, e[a]) * r;
    }
    return r;
}

/** Inverse of orderedEulerToMatrix. The middle axis' angle stays within -pi/2 to pi/2. */
D3 matrixToOrderedEuler(const M3& m, const std::string& order)
{
    int i = order[0] - 'X', j = order[1] - 'X', k = order[2] - 'X';
    // R = Rk * Rj * Ri. Parity tells whether (i, j, k) is a cyclic order of (X, Y, Z).
    double sign = ((j - i + 3) % 3 == 1) ? 1.0 : -1.0;
    auto at = [&](int row, int col) { return m[col][row]; };
    D3 e(0.0);
    double sj = std::clamp(-sign * at(k, i), -1.0, 1.0);
    e[j] = std::asin(sj);
    if (std::abs(sj) < 0.999999)
    {
        e[i] = std::atan2(sign * at(k, j), at(k, k));
        e[k] = std::atan2(sign * at(j, i), at(i, i));
    }
    else
    {
        e[i] = std::atan2(-sign * at(j, k), at(j, j));
        e[k] = 0.0;
    }
    return e;
}

/** Eigen decomposition of a symmetric 3x3 by cyclic Jacobi: values and the matching unit vectors as columns. */
void symmetricEigen(M3 a, D3& values, M3& vectors)
{
    vectors = M3(1.0);
    for (int sweep = 0; sweep < 50; ++sweep)
    {
        double off = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
        if (off < 1e-30)
            break;
        for (int p = 0; p < 2; ++p)
            for (int q = p + 1; q < 3; ++q)
            {
                if (std::abs(a[q][p]) < 1e-300)
                    continue;
                double theta = (a[q][q] - a[p][p]) / (2.0 * a[q][p]);
                double t = (theta >= 0.0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                double c = 1.0 / std::sqrt(t * t + 1.0), s = t * c;
                M3 j(1.0);
                j[p][p] = c;
                j[q][q] = c;
                j[q][p] = s;
                j[p][q] = -s;
                a = glm::transpose(j) * a * j;
                vectors = vectors * j;
            }
    }
    values = D3(a[0][0], a[1][1], a[2][2]);
}

struct Svd
{
    M3 u{1.0}, v{1.0};
    D3 s{0.0};
};

/** a = u * diag(s) * transpose(v), singular values descending, u and v orthonormal. */
Svd svd3(const M3& a)
{
    D3 lambda;
    M3 vecs;
    symmetricEigen(glm::transpose(a) * a, lambda, vecs);
    std::array<int, 3> order = {0, 1, 2};
    std::sort(order.begin(), order.end(), [&](int x, int y) { return lambda[x] > lambda[y]; });
    Svd r;
    for (int k = 0; k < 3; ++k)
    {
        r.v[k] = vecs[order[k]];
        r.s[k] = std::sqrt(std::max(0.0, lambda[order[k]]));
    }
    double tol = 1e-12 * std::max(1.0, r.s[0]);
    for (int k = 0; k < 3; ++k)
    {
        if (r.s[k] > tol)
        {
            r.u[k] = glm::normalize(a * r.v[k] / r.s[k]);
            continue;
        }
        r.s[k] = 0.0;
        // Complete u with directions square to the columns found so far.
        D3 cand = k == 0 ? D3(1, 0, 0) : k == 1 ? glm::cross(r.u[0], std::abs(r.u[0].x) < 0.9 ? D3(1, 0, 0) : D3(0, 1, 0)) : glm::cross(r.u[0], r.u[1]);
        for (int prev = 0; prev < k; ++prev)
            cand -= r.u[prev] * glm::dot(cand, r.u[prev]);
        r.u[k] = glm::normalize(cand);
    }
    return r;
}

M4 widen(const M3& m)
{
    M4 r(1.0);
    for (int c = 0; c < 3; ++c)
        r[c] = glm::dvec4(m[c], 0.0);
    return r;
}

void nodes()
{
    registerDecl({
        .type = "EulerToRotation",
        .label = "Euler to Rotation",
        .category = "Rotation",
        .doc = "A rotation from angles about X, Y and Z (radians), applied in the chosen order, first letter first.",
        .props = {prop("order", eulerOrders(), "XYZ")},
        .sockets = {.inputs = {fin("euler", T::Vector, vec(0, 0, 0))}, .outputs = {fout("rotation", T::Rotation)}},
        .exec =
            [](ExecContext& c) {
                std::string order = c.prop("order");
                c.out("rotation", map<D3, D3>(T::Rotation, [order](D3 e) { return matrixToEuler(orderedEulerToMatrix(e, order)); }, {c.field("euler")}));
            },
    });
    registerDecl({
        .type = "RotationToEuler",
        .label = "Rotation to Euler",
        .category = "Rotation",
        .doc = "A rotation as angles about X, Y and Z (radians) applied in the chosen order, first letter first.",
        .props = {prop("order", eulerOrders(), "XYZ")},
        .sockets = {.inputs = {fin("rotation", T::Rotation, vec(0, 0, 0))}, .outputs = {fout("euler", T::Vector)}},
        .exec =
            [](ExecContext& c) {
                std::string order = c.prop("order");
                c.out("euler", map<D3, D3>(T::Vector, [order](D3 r) { return matrixToOrderedEuler(eulerToMatrix(r), order); }, {c.field("rotation")}));
            },
    });
    registerDecl({
        .type = "MatrixSVD",
        .label = "Matrix SVD",
        .category = "Matrix",
        .doc = "Splits the 3x3 part of a matrix into U * diag(S) * transpose(V): U and V orthonormal, S the singular values, largest first.",
        .sockets = {.inputs = {fin("matrix", T::Matrix)}, .outputs = {fout("u", T::Matrix), fout("s", T::Vector), fout("v", T::Matrix)}},
        .exec =
            [](ExecContext& c) {
                FieldPtr m = c.field("matrix");
                c.out("u", map<M4, M4>(T::Matrix, [](const M4& x) { return widen(svd3(M3(x)).u); }, {m}));
                c.out("s", map<D3, M4>(T::Vector, [](const M4& x) { return svd3(M3(x)).s; }, {m}));
                c.out("v", map<M4, M4>(T::Matrix, [](const M4& x) { return widen(svd3(M3(x)).v); }, {m}));
            },
    });
}
} // namespace lila::gn::nodes::rotation2

namespace lila::gn::nodes
{
void registerRotationNodes2() { rotation2::nodes(); }
} // namespace lila::gn::nodes
