// blockposseed.h
// 与原版一致的"加权变体"渲染种子与随机数发生器。
//
// 原版流程（1.21.5 反汇编确认）：
//   BlockModelRenderer:
//       randomSource.setSeed(state.getSeed(pos));   // = Block.getSeed(state,pos) = Mth.getSeed(x,y,z)
//       model.collectParts(randomSource, parts);    // WeightedVariants -> WeightedList.get(random)
//   WeightedList.get(random): int i = random.nextInt(totalWeight); 再按权重累加选中
//
// RandomSource.create(long) 在 1.21.5 里返回 LegacyRandomSource，即
// java.util.Random 的 48 位 LCG（常量池里可见 25214903917 / 281474976710655 / 11）。
//
// 抽成头文件是为了让测试能直接检查这份实现本身，而不是一份抄件。
#ifndef BLOCKPOSSEED_H
#define BLOCKPOSSEED_H

#include <cstdint>

// 原版 Mth.getSeed(x,y,z)。
// 注意 x*3129871 在 Java 里是 32 位乘法（会回绕），之后整段按 64 位运算；
// C++ 有符号溢出是 UB，所以用无符号显式回绕再转回有符号。
inline long long BlockPosSeed(int x, int y, int z) {
    const std::uint32_t xm =
        static_cast<std::uint32_t>(static_cast<std::uint32_t>(x) * 3129871u);
    const std::int64_t l = static_cast<std::int64_t>(static_cast<std::int32_t>(xm))
        ^ (static_cast<std::int64_t>(z) * 116129781LL)
        ^ static_cast<std::int64_t>(y);
    std::uint64_t u = static_cast<std::uint64_t>(l);
    u = u * u * 42317861ULL + u * 11ULL;   // 同样按 64 位回绕
    // 等价于 Java 的 (long) >> 16（算术右移）
    std::uint64_t r = u >> 16;
    if (u & 0x8000000000000000ULL) r |= 0xFFFF000000000000ULL;
    return static_cast<long long>(r);
}

// 原版 LegacyRandomSource == java.util.Random 的 LCG
struct LegacyRandom {
    std::uint64_t seed;

    explicit LegacyRandom(long long s) : seed(0) { setSeed(s); }

    void setSeed(long long s) {
        seed = (static_cast<std::uint64_t>(s) ^ 0x5DEECE66DULL) & 0xFFFFFFFFFFFFULL;
    }

    std::int32_t next(int bits) {
        seed = (seed * 0x5DEECE66DULL + 0xBULL) & 0xFFFFFFFFFFFFULL;
        return static_cast<std::int32_t>(seed >> (48 - bits));
    }

    // java.util.Random.nextInt(bound)，0-based
    int nextInt(int bound) {
        if (bound <= 0) return 0;
        if ((bound & -bound) == bound) {   // bound 是 2 的幂
            return static_cast<int>((static_cast<long long>(bound) * next(31)) >> 31);
        }
        int j, i;
        do { j = next(31); i = j % bound; } while (j - i + (bound - 1) < 0);
        return i;
    }
};

#endif // BLOCKPOSSEED_H
