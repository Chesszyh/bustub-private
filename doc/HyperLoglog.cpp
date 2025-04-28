#include <vector>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <string>
#include <limits>
#include <functional>
#include <random>
#include <chrono>
#include <sstream>
#include <iomanip>

// 定义 MurmurHash3 哈希函数，通常用于 HyperLogLog
// 这个哈希函数具有良好的分布性质
uint64_t murmurHash3(const void* key, int len, uint32_t seed) {
    const uint8_t* data = (const uint8_t*)key;
    const int nblocks = len / 16;
    uint64_t h1 = seed;
    uint64_t h2 = seed;

    const uint64_t c1 = 0x87c37b91114253d5ULL;
    const uint64_t c2 = 0x4cf5ad432745937fULL;

    // 主体部分
    const uint64_t* blocks = (const uint64_t*)(data);
    for (int i = 0; i < nblocks; i++) {
        uint64_t k1 = blocks[i * 2];
        uint64_t k2 = blocks[i * 2 + 1];

        k1 *= c1; k1 = (k1 << 31) | (k1 >> 33); k1 *= c2; h1 ^= k1;
        h1 = (h1 << 27) | (h1 >> 37); h1 += h2; h1 = h1 * 5 + 0x52dce729;

        k2 *= c2; k2 = (k2 << 33) | (k2 >> 31); k2 *= c1; h2 ^= k2;
        h2 = (h2 << 31) | (h2 >> 33); h2 += h1; h2 = h2 * 5 + 0x38495ab5;
    }

    // 尾部处理
    const uint8_t* tail = (const uint8_t*)(data + nblocks * 16);
    uint64_t k1 = 0;
    uint64_t k2 = 0;

    switch (len & 15) {
    case 15: k2 ^= uint64_t(tail[14]) << 48; [[fallthrough]];
    case 14: k2 ^= uint64_t(tail[13]) << 40; [[fallthrough]];
    case 13: k2 ^= uint64_t(tail[12]) << 32; [[fallthrough]];
    case 12: k2 ^= uint64_t(tail[11]) << 24; [[fallthrough]];
    case 11: k2 ^= uint64_t(tail[10]) << 16; [[fallthrough]];
    case 10: k2 ^= uint64_t(tail[9]) << 8; [[fallthrough]];
    case 9: k2 ^= uint64_t(tail[8]) << 0;
        k2 *= c2; k2 = (k2 << 33) | (k2 >> 31); k2 *= c1; h2 ^= k2; [[fallthrough]];
    case 8: k1 ^= uint64_t(tail[7]) << 56; [[fallthrough]];
    case 7: k1 ^= uint64_t(tail[6]) << 48; [[fallthrough]];
    case 6: k1 ^= uint64_t(tail[5]) << 40; [[fallthrough]];
    case 5: k1 ^= uint64_t(tail[4]) << 32; [[fallthrough]];
    case 4: k1 ^= uint64_t(tail[3]) << 24; [[fallthrough]];
    case 3: k1 ^= uint64_t(tail[2]) << 16; [[fallthrough]];
    case 2: k1 ^= uint64_t(tail[1]) << 8; [[fallthrough]];
    case 1: k1 ^= uint64_t(tail[0]) << 0;
        k1 *= c1; k1 = (k1 << 31) | (k1 >> 33); k1 *= c2; h1 ^= k1;
    }

    h1 ^= len; h2 ^= len;
    h1 += h2; h2 += h1;

    // 收尾混淆
    h1 ^= h1 >> 33;
    h1 *= 0xff51afd7ed558ccdULL;
    h1 ^= h1 >> 33;
    h1 *= 0xc4ceb9fe1a85ec53ULL;
    h1 ^= h1 >> 33;

    h2 ^= h2 >> 33;
    h2 *= 0xff51afd7ed558ccdULL;
    h2 ^= h2 >> 33;
    h2 *= 0xc4ceb9fe1a85ec53ULL;
    h2 ^= h2 >> 33;

    h1 += h2;

    return h1;
}

/**
 * HyperLogLog实现类
 * p是精度参数，决定了使用2^p个寄存器
 * 标准误差约为1.04/sqrt(2^p)
 */
class HyperLogLog {
private:
    // 精度参数
    uint8_t p;
    // 寄存器数量 m = 2^p
    uint32_t m;
    // 存储所有寄存器的值
    std::vector<uint8_t> registers;
    // Alpha常数用于估计校正
    double alpha;
    // 哈希种子，可以根据需要更改
    uint32_t seed;

    // 计算前导零的数量+1
    int countLeadingZerosPlus1(uint64_t x, int bits) {
        if (x == 0) return bits + 1;
        int zeros = 0;
        uint64_t mask = 1ULL << (bits - 1);
        while ((x & mask) == 0 && mask != 0) {
            zeros++;
            mask >>= 1;
        }
        return zeros + 1;
    }

public:
    /**
     * 构造函数
     * @param precision 精度参数，通常为4到16之间的整数
     */
    explicit HyperLogLog(uint8_t precision = 14) {
        // 确保精度参数在合理范围内
        if (precision < 4 || precision > 16) {
            throw std::invalid_argument("Precision must be between 4 and 16");
        }

        p = precision;
        m = 1 << p;  // 2^p
        registers.resize(m, 0);

        // 根据m选择适当的alpha值
        if (m == 16) {
            alpha = 0.673;
        }
        else if (m == 32) {
            alpha = 0.697;
        }
        else if (m == 64) {
            alpha = 0.709;
        }
        else {
            alpha = 0.7213 / (1.0 + 1.079 / m);
        }

        // 初始化随机种子
        seed = std::chrono::system_clock::now().time_since_epoch().count();
    }

    /**
     * 添加一个元素到HyperLogLog
     * @param value 要添加的元素
     */
    template <typename T>
    void add(const T& value) {
        // 计算哈希值
        uint64_t hash = murmurHash3(&value, sizeof(value), seed);

        // 使用哈希值的前p位作为寄存器索引
        uint32_t idx = hash & (m - 1);  // 等价于 hash % m，但更高效

        // 计算哈希值剩余位中的前导零数量+1
        int leadingZeros = countLeadingZerosPlus1(hash >> p, 64 - p);

        // 更新寄存器的最大值
        registers[idx] = std::max(registers[idx], static_cast<uint8_t>(leadingZeros));
    }

    /**
     * 添加一个字符串元素
     */
    void addString(const std::string& value) {
        // 计算哈希值
        uint64_t hash = murmurHash3(value.c_str(), value.length(), seed);

        // 使用哈希值的前p位作为寄存器索引
        uint32_t idx = hash & (m - 1);

        // 计算哈希值剩余位中的前导零数量+1
        int leadingZeros = countLeadingZerosPlus1(hash >> p, 64 - p);

        // 更新寄存器的最大值
        registers[idx] = std::max(registers[idx], static_cast<uint8_t>(leadingZeros));
    }

    /**
     * 估计集合的基数（不同元素数量）
     * @return 估计的基数
     */
    double estimate() const {
        // 计算调和平均数
        double sum = 0.0;
        double zeros = 0.0;

        for (uint8_t val : registers) {
            sum += 1.0 / (1 << val);
            if (val == 0) zeros++;
        }

        // 计算原始估计值
        double estimate = alpha * m * m / sum;

        // 应用小基数和大基数校正

        // 小基数校正：如果估计值小于阈值，且存在寄存器为0
        if (estimate <= 2.5 * m && zeros > 0) {
            // 线性计数校正
            estimate = m * std::log(m / zeros);
        }
        // 大基数校正：如果估计值超过了2^32/30的阈值
        else if (estimate > 4294967296.0 / 30.0) {
            // 修正大基数偏差
            estimate = -4294967296.0 * std::log(1.0 - estimate / 4294967296.0);
        }

        return estimate;
    }

    /**
     * 合并另一个HyperLogLog的状态
     * @param other 要合并的HyperLogLog对象
     */
    void merge(const HyperLogLog& other) {
        if (p != other.p) {
            throw std::invalid_argument("Cannot merge HyperLogLog with different precision");
        }

        for (size_t i = 0; i < m; i++) {
            registers[i] = std::max(registers[i], other.registers[i]);
        }
    }

    /**
     * 重置HyperLogLog到初始状态
     */
    void reset() {
        std::fill(registers.begin(), registers.end(), 0);
    }

    /**
     * 获取当前使用的内存大小（字节）
     */
    size_t memoryUsage() const {
        return registers.size() * sizeof(uint8_t) + sizeof(HyperLogLog);
    }

    /**
     * 获取精度参数p
     */
    uint8_t getPrecision() const {
        return p;
    }

    /**
     * 获取寄存器数量
     */
    uint32_t getRegisterCount() const {
        return m;
    }

    /**
     * 获取当前的相对标准误差
     */
    double getRelativeError() const {
        return 1.04 / std::sqrt(m);
    }
};

// 测试示例
int main() {
    // 创建一个精度为14的HyperLogLog（使用2^14个寄存器）
    HyperLogLog hll(14);

    std::cout << "HyperLogLog demonstration:" << std::endl;
    std::cout << "Precision: " << (int)hll.getPrecision() << std::endl;
    std::cout << "Register count: " << hll.getRegisterCount() << std::endl;
    std::cout << "Memory usage: " << hll.memoryUsage() << " bytes" << std::endl;
    std::cout << "Expected relative error: " << std::fixed << std::setprecision(4)
        << hll.getRelativeError() * 100 << "%" << std::endl << std::endl;

    // 生成随机数并添加到HyperLogLog
    std::mt19937_64 rng(42);  // 使用固定种子以便结果可重复
    std::uniform_int_distribution<int> dist(1, 1000000000);

    // 实际不同元素数量
    std::vector<int> actualCounts = { 10000, 100000, 1000000, 10000000 };

    for (int count : actualCounts) {
        hll.reset();

        // 添加不同元素
        for (int i = 0; i < count; i++) {
            int value = dist(rng);
            hll.add(value);
        }

        // 估计基数
        double estimated = hll.estimate();
        double error = std::abs(estimated - count) / count * 100;

        std::cout << "Actual count: " << count << std::endl;
        std::cout << "Estimated: " << std::fixed << std::setprecision(2) << estimated << std::endl;
        std::cout << "Error: " << std::fixed << std::setprecision(2) << error << "%" << std::endl;
        std::cout << std::endl;
    }

    // 演示合并操作
    HyperLogLog hll1(10);
    HyperLogLog hll2(10);

    for (int i = 0; i < 50000; i++) {
        hll1.add(dist(rng));
    }

    for (int i = 0; i < 30000; i++) {
        hll2.add(dist(rng) + 40000);  // 确保有些重叠
    }

    std::cout << "Demonstrating merge operation:" << std::endl;
    std::cout << "HLL1 estimate: " << hll1.estimate() << std::endl;
    std::cout << "HLL2 estimate: " << hll2.estimate() << std::endl;

    hll1.merge(hll2);
    std::cout << "After merge estimate: " << hll1.estimate() << std::endl;

    return 0;
}