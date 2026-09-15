#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <format>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <mutex>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace owl::prometheus {
    namespace detail {
        [[nodiscard]] constexpr bool ascii_alpha(const unsigned char c) noexcept {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        }

        [[nodiscard]] constexpr bool ascii_alnum(const unsigned char c) noexcept {
            return ascii_alpha(c) || (c >= '0' && c <= '9');
        }

        // ASCII only — std::isalpha follows locale and would accept/reject
        // names Prometheus does not.
        [[nodiscard]] inline bool valid_metric_name(const std::string_view name) noexcept {
            if (name.empty()) return false;
            const auto first = static_cast<unsigned char>(name.front());
            if (!(ascii_alpha(first) || first == '_' || first == ':')) return false;
            for (const unsigned char c : name) {
                if (!(ascii_alnum(c) || c == '_' || c == ':')) return false;
            }
            return true;
        }

        [[nodiscard]] inline bool valid_label_name(const std::string_view name) noexcept {
            if (name.empty()) return false;
            const auto first = static_cast<unsigned char>(name.front());
            if (!(ascii_alpha(first) || first == '_')) return false;
            for (const unsigned char c : name) {
                if (!(ascii_alnum(c) || c == '_')) return false;
            }
            return true;
        }

        inline void append_escaped(std::string& out, const std::string_view text) {
            for (const char c : text) {
                if (c == '\\' || c == '"') {
                    out.push_back('\\');
                    out.push_back(c);
                } else if (c == '\n') {
                    out.append("\\n");
                } else {
                    out.push_back(c);
                }
            }
        }

        [[nodiscard]] inline std::string series_key(const std::span<const std::string_view> values) {
            std::string key;
            for (const auto value : values) {
                std::format_to(std::back_inserter(key), "{}:", value.size());
                key.append(value);
            }
            return key;
        }

        inline constexpr std::array<double, 11> default_buckets{
            0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 2.5, 5, 10
        };

        [[nodiscard]] inline bool same_bounds(
            const std::vector<double>& have,
            const std::span<const double> want
        ) noexcept {
            if (have.size() != want.size()) return false;
            for (std::size_t i = 0; i < have.size(); ++i) {
                if (have[i] != want[i]) return false;
            }
            return true;
        }

        // Prometheus le= is a lie if a family mixes bounds. Empty / Inf / NaN
        // cannot be a finite strictly-increasing upper bound.
        inline void require_histogram_buckets(const std::span<const double> buckets) {
            if (buckets.empty()) {
                throw std::invalid_argument("histogram buckets must not be empty");
            }
            for (std::size_t i = 0; i < buckets.size(); ++i) {
                if (!std::isfinite(buckets[i])) {
                    throw std::invalid_argument("histogram buckets must be finite");
                }
                if (i > 0 && buckets[i] <= buckets[i - 1]) {
                    throw std::invalid_argument("histogram buckets must be strictly ascending");
                }
            }
        }

        [[nodiscard]] inline std::span<const double> effective_histogram_buckets(
            const std::span<const double> buckets
        ) noexcept {
            if (buckets.empty()) return default_buckets;
            return buckets;
        }

        struct Series final {
            std::vector<std::string> label_values;
            std::atomic<double> value{0};
            std::vector<double> bucket_bounds;
            std::vector<std::uint64_t> buckets;
            std::atomic<double> sum{0};
            std::atomic<std::uint64_t> count{0};
        };

        enum class Kind { Counter, Gauge, Histogram };

        [[nodiscard]] inline std::string_view type_name(const Kind kind) noexcept {
            switch (kind) {
            case Kind::Counter: return "counter";
            case Kind::Gauge: return "gauge";
            case Kind::Histogram: return "histogram";
            }
            std::unreachable();
        }

        struct TransparentStringHash final {
            using is_transparent = void;

            [[nodiscard]] std::size_t operator()(const std::string_view s) const noexcept {
                return std::hash<std::string_view>{}(s);
            }
        };

        struct Family final {
            Kind kind = Kind::Counter;
            std::vector<std::string> label_names;
            std::vector<double> bucket_bounds;
            std::unordered_map<std::string, std::shared_ptr<Series>, TransparentStringHash, std::equal_to<>> series;
        };

        struct Snapshot final {
            std::unordered_map<std::string, Family, TransparentStringHash, std::equal_to<>> families;
            // describe() can run before the first series; dump() reads this
            // when the family later appears. Default HELP is the metric name.
            std::unordered_map<std::string, std::string, TransparentStringHash, std::equal_to<>> help;
        };

        inline void append_labels(
            std::string& out,
            const Family& fam,
            const Series& series,
            const std::span<const std::pair<std::string_view, std::string_view>> extra
        ) {
            if (fam.label_names.empty() && extra.empty()) return;
            out.push_back('{');
            bool first = true;
            for (std::size_t i = 0; i < fam.label_names.size(); ++i) {
                if (!first) out.push_back(',');
                first = false;
                out.append(fam.label_names[i]);
                out.append("=\"");
                append_escaped(out, series.label_values[i]);
                out.push_back('"');
            }
            for (const auto& [key, value] : extra) {
                if (!first) out.push_back(',');
                first = false;
                out.append(key);
                out.append("=\"");
                append_escaped(out, value);
                out.push_back('"');
            }
            out.push_back('}');
        }

        inline void dump_histogram(
            std::string& out,
            const std::string& name,
            const Family& fam,
            const Series& series
        ) {
            for (std::size_t i = 0; i < fam.bucket_bounds.size(); ++i) {
                out.append(name);
                out.append("_bucket");
                const auto le = std::format("{}", fam.bucket_bounds[i]);
                const std::pair<std::string_view, std::string_view> extra[] = {{"le", le}};
                append_labels(out, fam, series, extra);
                std::format_to(
                    std::back_inserter(out),
                    " {}\n",
                    std::atomic_ref<std::uint64_t>(const_cast<std::uint64_t&>(series.buckets[i])).load(std::memory_order_relaxed));
            }
            out.append(name);
            out.append("_bucket");
            const std::pair<std::string_view, std::string_view> inf[] = {{"le", "+Inf"}};
            append_labels(out, fam, series, inf);
            std::format_to(
                std::back_inserter(out),
                " {}\n",
                std::atomic_ref<std::uint64_t>(const_cast<std::uint64_t&>(series.buckets.back())).load(std::memory_order_relaxed));
            out.append(name);
            out.append("_sum");
            append_labels(out, fam, series, {});
            std::format_to(
                std::back_inserter(out),
                " {}\n",
                series.sum.load(std::memory_order_relaxed));
            out.append(name);
            out.append("_count");
            append_labels(out, fam, series, {});
            std::format_to(
                std::back_inserter(out),
                " {}\n",
                series.count.load(std::memory_order_relaxed));
        }

        // Values are atomics. The family/series map is an atomic shared_ptr
        // snapshot: inc/scrape never take a mutex. Exclusive lock only when
        // copying the snapshot to insert a new series (rare after warmup).
        struct Registry final {
            std::mutex insert_mu;
            mutable std::shared_ptr<Snapshot> snapshot_{std::make_shared<Snapshot>()};

            static void check_names(
                const std::string_view name,
                const std::span<const std::string_view> label_names
            ) {
                if (!valid_metric_name(name)) {
                    throw std::invalid_argument(std::string("invalid metric name: ").append(name));
                }
                for (const auto label : label_names) {
                    if (!valid_label_name(label)) {
                        throw std::invalid_argument(std::string("invalid label name: ").append(label));
                    }
                }
            }

            static void check_family(
                const Family& fam,
                const std::string_view name,
                const std::span<const std::string_view> label_names,
                const Kind kind
            ) {
                if (fam.kind != kind) {
                    throw std::invalid_argument(std::string(name).append(" type mismatch"));
                }
                if (fam.label_names.size() != label_names.size()) {
                    throw std::invalid_argument(std::string(name).append(" label cardinality mismatch"));
                }
                for (std::size_t i = 0; i < label_names.size(); ++i) {
                    if (fam.label_names[i] != label_names[i]) {
                        throw std::invalid_argument(std::string(name).append(" label name mismatch"));
                    }
                }
            }

            static void check_histogram_buckets(
                const Family& fam,
                const std::string_view name,
                const Kind kind,
                const std::span<const double> buckets
            ) {
                if (kind != Kind::Histogram) return;
                if (!same_bounds(fam.bucket_bounds, effective_histogram_buckets(buckets))) {
                    throw std::invalid_argument(std::string(name).append(" bucket mismatch"));
                }
            }

            static std::shared_ptr<Series> find(
                const Snapshot& snap,
                const std::string_view name,
                const std::span<const std::string_view> label_names,
                const Kind kind,
                const std::string& key,
                const std::span<const double> buckets
            ) {
                const auto it = snap.families.find(name);
                if (it == snap.families.end()) return nullptr;
                check_family(it->second, name, label_names, kind);
                // Existing unlabeled series would otherwise be reused when a
                // later histogram() passes different bounds.
                check_histogram_buckets(it->second, name, kind, buckets);
                const auto series = it->second.series.find(key);
                if (series == it->second.series.end()) return nullptr;
                return series->second;
            }

            std::shared_ptr<Series> get_or_insert(
                const std::string_view name,
                const std::span<const std::string_view> label_names,
                const std::span<const std::string_view> values,
                const Kind kind,
                const std::span<const double> buckets = {}
            ) {
                check_names(name, label_names);
                if (values.size() != label_names.size()) {
                    throw std::invalid_argument("label value count mismatch");
                }
                const auto key = series_key(values);
                if (const auto existing = find(*load(), name, label_names, kind, key, buckets)) {
                    return existing;
                }
                const std::unique_lock exclusive{insert_mu};
                auto snap = load();
                if (const auto existing = find(*snap, name, label_names, kind, key, buckets)) return existing;
                auto next = std::make_shared<Snapshot>(*snap);
                const auto [it, inserted] = next->families.try_emplace(std::string(name));
                auto& fam = it->second;
                if (inserted) {
                    fam.kind = kind;
                    fam.label_names.assign(label_names.begin(), label_names.end());
                    if (kind == Kind::Histogram) {
                        const auto bounds = effective_histogram_buckets(buckets);
                        fam.bucket_bounds.assign(bounds.begin(), bounds.end());
                    }
                } else {
                    check_family(fam, name, label_names, kind);
                    check_histogram_buckets(fam, name, kind, buckets);
                }
                auto series = std::make_shared<Series>();
                series->label_values.assign(values.begin(), values.end());
                if (kind == Kind::Histogram) {
                    series->bucket_bounds = fam.bucket_bounds;
                    series->buckets.assign(fam.bucket_bounds.size() + 1, 0);
                }
                fam.series.emplace(key, series);
                std::atomic_store_explicit(&snapshot_, std::move(next), std::memory_order_release);
                return series;
            }

            [[nodiscard]] std::shared_ptr<Snapshot> load() const {
                return std::atomic_load_explicit(&snapshot_, std::memory_order_acquire);
            }

            void describe(const std::string_view name, const std::string_view help) {
                if (!valid_metric_name(name)) {
                    throw std::invalid_argument(std::string("invalid metric name: ").append(name));
                }
                const std::unique_lock exclusive{insert_mu};
                auto snap = load();
                auto next = std::make_shared<Snapshot>(*snap);
                next->help.insert_or_assign(std::string(name), std::string(help));
                std::atomic_store_explicit(&snapshot_, std::move(next), std::memory_order_release);
            }

            [[nodiscard]] std::string dump() const {
                const auto snap = load();
                std::string out;
                for (const auto& [name, fam] : snap->families) {
                    out.append("# HELP ");
                    out.append(name);
                    out.push_back(' ');
                    if (const auto help = snap->help.find(name); help != snap->help.end()) {
                        append_escaped(out, help->second);
                    } else {
                        append_escaped(out, name);
                    }
                    out.append("\n# TYPE ");
                    out.append(name);
                    out.push_back(' ');
                    out.append(type_name(fam.kind));
                    out.push_back('\n');
                    for (const auto& series : fam.series | std::views::values) {
                        if (fam.kind == Kind::Histogram) {
                            dump_histogram(out, name, fam, *series);
                            continue;
                        }
                        out.append(name);
                        append_labels(out, fam, *series, {});
                        std::format_to(
                            std::back_inserter(out),
                            " {}\n",
                            series->value.load(std::memory_order_relaxed));
                    }
                }
                return out;
            }

            void clear() {
                const std::unique_lock exclusive{insert_mu};
                std::atomic_store_explicit(&snapshot_, std::make_shared<Snapshot>(), std::memory_order_release);
            }
        };

        inline Registry& registry() {
            static Registry r;
            return r;
        }

        [[nodiscard]] inline std::vector<std::string_view> views_of(const std::vector<std::string>& names) {
            std::vector<std::string_view> views;
            views.reserve(names.size());
            for (const auto& name : names) views.emplace_back(name);
            return views;
        }
    }

    class Counter final {
    public:
        Counter(
            std::string name,
            std::vector<std::string> label_names,
            std::shared_ptr<detail::Series> series
        ) : name_(std::move(name)),
            label_names_(std::move(label_names)),
            series_(std::move(series)) {
        }

        [[nodiscard]] Counter labels(const std::initializer_list<std::string_view> values) const {
            const auto names = detail::views_of(label_names_);
            const std::vector<std::string_view> view{values};
            return Counter{
                name_,
                label_names_,
                detail::registry().get_or_insert(name_, names, view, detail::Kind::Counter)
            };
        }

        void inc(const double n = 1) const {
            if (!series_) throw std::invalid_argument("counter requires labels()");
            if (!std::isfinite(n)) return;
            if (n < 0) throw std::invalid_argument("counter must not decrease");
            series_->value.fetch_add(n, std::memory_order_relaxed);
        }

    private:
        std::string name_;
        std::vector<std::string> label_names_;
        std::shared_ptr<detail::Series> series_;
    };

    [[nodiscard]] inline Counter counter(const std::string_view name) {
        return Counter{
            std::string{name},
            {},
            detail::registry().get_or_insert(name, {}, {}, detail::Kind::Counter)
        };
    }

    // Label values must be bounded (status, route template). User ids in
    // labels will explode the series map.
    [[nodiscard]] inline Counter counter(
        const std::string_view name,
        const std::initializer_list<std::string_view> label_names
    ) {
        std::vector<std::string> names;
        names.reserve(label_names.size());
        for (const auto label : label_names) names.emplace_back(label);
        return Counter{std::string{name}, std::move(names), nullptr};
    }

    class Gauge final {
    public:
        Gauge(
            std::string name,
            std::vector<std::string> label_names,
            std::shared_ptr<detail::Series> series
        ) : name_(std::move(name)),
            label_names_(std::move(label_names)),
            series_(std::move(series)) {
        }

        [[nodiscard]] Gauge labels(const std::initializer_list<std::string_view> values) const {
            const auto names = detail::views_of(label_names_);
            const std::vector<std::string_view> view{values};
            return Gauge{
                name_,
                label_names_,
                detail::registry().get_or_insert(name_, names, view, detail::Kind::Gauge)
            };
        }

        void set(const double n) const {
            if (!series_) throw std::invalid_argument("gauge requires labels()");
            series_->value.store(n, std::memory_order_relaxed);
        }

        void inc(const double n = 1) const {
            if (!series_) throw std::invalid_argument("gauge requires labels()");
            series_->value.fetch_add(n, std::memory_order_relaxed);
        }

        void dec(const double n = 1) const {
            if (!series_) throw std::invalid_argument("gauge requires labels()");
            series_->value.fetch_add(-n, std::memory_order_relaxed);
        }

    private:
        std::string name_;
        std::vector<std::string> label_names_;
        std::shared_ptr<detail::Series> series_;
    };

    [[nodiscard]] inline Gauge gauge(const std::string_view name) {
        return Gauge{
            std::string{name},
            {},
            detail::registry().get_or_insert(name, {}, {}, detail::Kind::Gauge)
        };
    }

    [[nodiscard]] inline Gauge gauge(
        const std::string_view name,
        const std::initializer_list<std::string_view> label_names
    ) {
        std::vector<std::string> names;
        names.reserve(label_names.size());
        for (const auto label : label_names) names.emplace_back(label);
        return Gauge{std::string{name}, std::move(names), nullptr};
    }

    class Histogram final {
    public:
        Histogram(
            std::string name,
            std::vector<std::string> label_names,
            std::shared_ptr<detail::Series> series,
            std::vector<double> bucket_bounds = {}
        ) : name_(std::move(name)),
            label_names_(std::move(label_names)),
            series_(std::move(series)),
            bucket_bounds_(std::move(bucket_bounds)) {
        }

        [[nodiscard]] Histogram labels(const std::initializer_list<std::string_view> values) const {
            const auto names = detail::views_of(label_names_);
            const std::vector<std::string_view> view{values};
            return Histogram{
                name_,
                label_names_,
                detail::registry().get_or_insert(name_, names, view, detail::Kind::Histogram, bucket_bounds_),
                bucket_bounds_
            };
        }

        void observe(const double n) const {
            if (!series_) throw std::invalid_argument("histogram requires labels()");
            if (n < 0 || !std::isfinite(n)) return;
            series_->count.fetch_add(1, std::memory_order_relaxed);
            series_->sum.fetch_add(n, std::memory_order_relaxed);
            // Series copies the family's bounds: observe must not assume the
            // HTTP defaults, and Family lives in a snapshot that can be replaced.
            for (std::size_t i = 0; i < series_->bucket_bounds.size(); ++i) {
                if (n <= series_->bucket_bounds[i]) {
                    std::atomic_ref<std::uint64_t>(series_->buckets[i]).fetch_add(1, std::memory_order_relaxed);
                }
            }
            std::atomic_ref<std::uint64_t>(series_->buckets.back()).fetch_add(1, std::memory_order_relaxed);
        }

    private:
        std::string name_;
        std::vector<std::string> label_names_;
        std::shared_ptr<detail::Series> series_;
        std::vector<double> bucket_bounds_;
    };

    [[nodiscard]] inline Histogram histogram(const std::string_view name) {
        return Histogram{
            std::string{name},
            {},
            detail::registry().get_or_insert(name, {}, {}, detail::Kind::Histogram)
        };
    }

    [[nodiscard]] inline Histogram histogram(
        const std::string_view name,
        const std::initializer_list<std::string_view> label_names
    ) {
        std::vector<std::string> names;
        names.reserve(label_names.size());
        for (const auto label : label_names) names.emplace_back(label);
        return Histogram{std::string{name}, std::move(names), nullptr};
    }

    [[nodiscard]] inline Histogram histogram(
        const std::string_view name,
        const std::initializer_list<double> buckets
    ) {
        const std::span<const double> bounds{buckets};
        detail::require_histogram_buckets(bounds);
        return Histogram{
            std::string{name},
            {},
            detail::registry().get_or_insert(name, {}, {}, detail::Kind::Histogram, bounds),
            std::vector<double>{bounds.begin(), bounds.end()}
        };
    }

    [[nodiscard]] inline Histogram histogram(
        const std::string_view name,
        const std::initializer_list<std::string_view> label_names,
        const std::initializer_list<double> buckets
    ) {
        const std::span<const double> bounds{buckets};
        detail::require_histogram_buckets(bounds);
        std::vector<std::string> names;
        names.reserve(label_names.size());
        for (const auto label : label_names) names.emplace_back(label);
        return Histogram{
            std::string{name},
            std::move(names),
            nullptr,
            std::vector<double>{bounds.begin(), bounds.end()}
        };
    }

    // HELP defaults to the metric name so a scrape is valid without ceremony.
    // A separate call so help can be set before the first series exists.
    inline void describe(const std::string_view name, const std::string_view help) {
        detail::registry().describe(name, help);
    }

    inline void clear() {
        detail::registry().clear();
    }
}
