#ifndef ENTITY_MODEL_H
#define ENTITY_MODEL_H

// 原版实体模型(ModelPart 树)的运行时实例: 几何数据来自 EntityModelData.inc
// (tools/entity_models 从原版客户端 LayerDefinitions 自动导出), 姿态按原版 setupAnim 手动设置。

#include "model.h"
#include <functional>
#include <string>
#include <vector>

// 3x4 仿射矩阵 (PoseStack 的 Matrix4f 等价物, 双精度)
struct EntityAffine {
    double m[3][4] = { {1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0} };
    EntityAffine operator*(const EntityAffine& o) const;
    void Apply(const double p[3], double out[3]) const;
    double Det() const;
    static EntityAffine T(double x, double y, double z);
    static EntityAffine S(double x, double y, double z);
    static EntityAffine S(double s) { return S(s, s, s); }
    static EntityAffine RX(double rad);
    static EntityAffine RY(double rad);
    static EntityAffine RZ(double rad);
    static EntityAffine RYDeg(double deg);
    static EntityAffine RZDeg(double deg);
};

// 顶层部件名 -> root 之前插入的变换 (旧版 AgeableListModel 幼体)
using TopPre = std::function<EntityAffine(const std::string&)>;

class EntityModel {
public:
    // family: "legacy" (<=1.21.x) / "modern" (26.x); id: ModelLayerLocation, e.g. "minecraft:villager#main"
    EntityModel(const std::string& family, const std::string& id);
    static bool Exists(const std::string& family, const std::string& id);
    bool Valid() const { return layer_ >= 0; }

    int Find(const std::string& name) const;       // -1 = 不存在
    int Parent(int part) const;
    int TopLevel(int part) const;                  // root 的直接子节点(root 自身返回 -1)
    bool InSubtree(int part, int ancestor) const;
    const std::string& Name(int part) const { return names_[part]; }
    int PartCount() const { return static_cast<int>(names_.size()); }

    // 姿态 (x, y, z, xRot, yRot, zRot, xScale, yScale, zScale), 初值 = 原版 bake 后的 initialPose
    float* Pose(int part) { return poses_[part].data(); }
    const float* Pose(int part) const { return poses_[part].data(); }
    void SetPose(const std::string& part, float x, float y, float z);   // 位置 (模型像素)
    void SetRot(const std::string& part, float x, float y, float z);
    void AddRot(const std::string& part, float x, float y, float z);
    void SetVisible(int part, bool v) { visible_[part] = v; }
    void SetVisible(const std::string& part, bool v) { int i = Find(part); if (i >= 0) visible_[i] = v; }
    void HideAllBut(const std::vector<std::string>& keep);             // 只保留这些部件(及其子部件)

    // ModelPart.copyFrom: 按部件名复制姿态 (withPosition=false 只复制旋转)
    void CopyPoseFrom(const EntityModel& src, const std::vector<std::string>& parts, bool withPosition);

    // 部件的完整变换 (base * [pre] * root * ... * part), 即 ModelPart.translateAndRotate 链
    EntityAffine PartTransform(int part, const EntityAffine& base,
                               const TopPre& pre = nullptr) const;

    // 把可见部件写入 out。material(part) 返回材质索引(<0 跳过该部件);
    // pre(topLevel) 可选, 在 root 之前插入的变换 (旧版 AgeableListModel 幼体头/身缩放)。
    void Emit(ModelData& out, const EntityAffine& base,
              const std::function<int(int)>& material,
              const TopPre& pre = nullptr) const;

private:
    int layer_ = -1;
    int partStart_ = 0;
    std::vector<std::string> names_;
    std::vector<int> parents_;
    std::vector<std::array<float, 9>> poses_;
    std::vector<bool> visible_;
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
