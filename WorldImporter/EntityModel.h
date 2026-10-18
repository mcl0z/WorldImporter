#ifndef ENTITY_MODEL_H
#define ENTITY_MODEL_H

// Minimal EntityAffine (PoseStack Matrix4f) for ItemRender / CampfireItems.
// Full EntityModel lives on feat/entity-equipment; this branch only needs the affine.

#include <cmath>

struct EntityAffine {
    double m[3][4] = { {1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0} };
    EntityAffine operator*(const EntityAffine& o) const {
        EntityAffine r;
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j)
                r.m[i][j] = m[i][0] * o.m[0][j] + m[i][1] * o.m[1][j] + m[i][2] * o.m[2][j];
            r.m[i][3] = m[i][0] * o.m[0][3] + m[i][1] * o.m[1][3] + m[i][2] * o.m[2][3] + m[i][3];
        }
        return r;
    }
    void Apply(const double p[3], double out[3]) const {
        out[0] = m[0][0] * p[0] + m[0][1] * p[1] + m[0][2] * p[2] + m[0][3];
        out[1] = m[1][0] * p[0] + m[1][1] * p[1] + m[1][2] * p[2] + m[1][3];
        out[2] = m[2][0] * p[0] + m[2][1] * p[1] + m[2][2] * p[2] + m[2][3];
    }
    double Det() const {
        return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
             - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
             + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    }
    static EntityAffine T(double x, double y, double z) {
        EntityAffine a; a.m[0][3] = x; a.m[1][3] = y; a.m[2][3] = z; return a;
    }
    static EntityAffine S(double x, double y, double z) {
        EntityAffine a; a.m[0][0] = x; a.m[1][1] = y; a.m[2][2] = z; return a;
    }
    static EntityAffine S(double s) { return S(s, s, s); }
    static EntityAffine RX(double r) {
        EntityAffine a; double c = std::cos(r), s = std::sin(r);
        a.m[1][1] = c; a.m[1][2] = -s; a.m[2][1] = s; a.m[2][2] = c; return a;
    }
    static EntityAffine RY(double r) {
        EntityAffine a; double c = std::cos(r), s = std::sin(r);
        a.m[0][0] = c; a.m[0][2] = s; a.m[2][0] = -s; a.m[2][2] = c; return a;
    }
    static EntityAffine RZ(double r) {
        EntityAffine a; double c = std::cos(r), s = std::sin(r);
        a.m[0][0] = c; a.m[0][1] = -s; a.m[1][0] = s; a.m[1][1] = c; return a;
    }
    static EntityAffine RYDeg(double d) {
        constexpr double kPi = 3.14159265358979323846;
        return RY(d * kPi / 180.0);
    }
    static EntityAffine RXDeg(double d) {
        constexpr double kPi = 3.14159265358979323846;
        return RX(d * kPi / 180.0);
    }
    static EntityAffine RZDeg(double d) {
        constexpr double kPi = 3.14159265358979323846;
        return RZ(d * kPi / 180.0);
    }
};

#endif
