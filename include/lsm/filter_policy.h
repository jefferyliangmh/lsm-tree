#pragma once
#include <algorithm>
#include <memory>
#include "lsm/metrics.h"
#include "lsm/options.h"

namespace lsm {

    // 过滤器策略只决定“下一张表用多少 bit”以及是否启用学习型前缀模型。
    // 磁盘上的经典 Bloom 布局保持不变；学习型模型是 Bloom 块前的可选包装。
    class FilterPolicy {
    public:
        virtual ~FilterPolicy() = default;
        virtual const char* Name() const noexcept = 0;
        virtual int BitsForLevel(int level, int tuned_bits, const Options& opt) const = 0;
        // 根据一个观测窗口更新 tuned bits。样本不足时原样返回。
        virtual int Tune(int current_bits, const Options& opt, FilterTelemetry& telemetry) const = 0;
    };

    class StandardBloomPolicy final : public FilterPolicy {
    public:
        const char* Name() const noexcept override { return "standard-bloom"; }
        int BitsForLevel(int, int tuned_bits, const Options&) const override {
            return std::max(1, tuned_bits);
        }
        int Tune(int current_bits, const Options&, FilterTelemetry&) const override {
            return current_bits;
        }
    };

    class AdaptiveBloomPolicy final : public FilterPolicy {
    public:
        explicit AdaptiveBloomPolicy(bool learned) : learned_(learned) {}

        const char* Name() const noexcept override {
            return learned_ ? "learned-bloom" : "adaptive-bloom";
        }

        int BitsForLevel(int level, int tuned_bits, const Options& opt) const override {
            int bits = tuned_bits;
            if (level <= 0) bits -= 2;          // L0 短命，少给一点内存
            else bits += std::min(level, 3);    // 深层查询更多，加位
            bits = std::max(opt.bloom_bits_min, bits);
            bits = std::min(opt.bloom_bits_max, bits);
            return std::max(1, bits);
        }

        int Tune(int current_bits, const Options& opt, FilterTelemetry& telemetry) const override {
            const uint64_t fp = telemetry.false_positives.load(std::memory_order_relaxed);
            const uint64_t rej = telemetry.bloom_rejects.load(std::memory_order_relaxed);
            const uint64_t neg = fp + rej;
            if (neg < 64) return current_bits;
            const double fpr = static_cast<double>(fp) / static_cast<double>(neg);
            int bits = current_bits;
            if (fpr > opt.bloom_target_fpr * 1.5)
                bits = std::min(opt.bloom_bits_max, current_bits + 1);
            else if (neg >= 256 && fpr * 4.0 < opt.bloom_target_fpr)
                bits = std::max(opt.bloom_bits_min, current_bits - 1);
            telemetry.ResetWindow();
            return bits;
        }

    private:
        bool learned_ = false;
    };

    inline std::unique_ptr<FilterPolicy> MakeFilterPolicy(FilterKind kind) {
        switch (kind) {
        case FilterKind::kAdaptiveBloom:
            return std::make_unique<AdaptiveBloomPolicy>(false);
        case FilterKind::kLearnedBloom:
            return std::make_unique<AdaptiveBloomPolicy>(true);
        case FilterKind::kStandardBloom:
        default:
            return std::make_unique<StandardBloomPolicy>();
        }
    }

} // namespace lsm
