#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <vector>

struct Stats
{
    double mean;
    double stddev;
    double min;
    double p50;
    double p95;
    double p99;
    double max;
    size_t n;

    // Sorts the sample vector in-place; caller may discard it afterwards.
    static Stats from(std::vector<double>& s)
    {
        std::sort(s.begin(), s.end());
        size_t n = s.size();
        double sum = std::accumulate(s.begin(), s.end(), 0.0);
        double mean = sum / n;
        double sq = 0.0;
        for (double v : s) sq += (v - mean) * (v - mean);

        auto pct = [&](int p) { return s[n * p / 100]; };
        return { mean, std::sqrt(sq / n), s[0], pct(50), pct(95), pct(99), s[n - 1], n };
    }

    void print_header(const char* row_label = "label") const
    {
        printf("%-20s  %9s  %9s  %9s  %9s  %9s  %9s  %9s  %6s\n",
               row_label, "mean_us", "std_us", "min_us", "p50_us", "p95_us", "p99_us", "max_us", "n");
    }

    void print_row(const char* label) const
    {
        printf("%-20s  %9.2f  %9.2f  %9.2f  %9.2f  %9.2f  %9.2f  %9.2f  %6zu\n",
               label, mean, stddev, min, p50, p95, p99, max, n);
    }

    // CSV row: caller provides the leading fields, this appends the stats.
    void print_csv(const char* prefix) const
    {
        printf("%s,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%zu\n",
               prefix, mean, stddev, min, p50, p95, p99, max, n);
    }
};

// Ordinary least-squares fit of y = alpha + slope * x.
// Returns false if x values are all identical (degenerate case).
struct LinFit
{
    double alpha;   // y-intercept: fixed orchestration cost estimate
    double slope;   // slope: per-unit variable cost
    double r2;      // coefficient of determination

    static bool fit(const std::vector<double>& xs,
                    const std::vector<double>& ys,
                    LinFit& out)
    {
        size_t n = xs.size();
        if (n < 2) return false;
        double xm = 0, ym = 0;
        for (size_t i = 0; i < n; i++) { xm += xs[i]; ym += ys[i]; }
        xm /= n; ym /= n;

        double sxx = 0, sxy = 0, sstot = 0;
        for (size_t i = 0; i < n; i++)
        {
            sxx   += (xs[i] - xm) * (xs[i] - xm);
            sxy   += (xs[i] - xm) * (ys[i] - ym);
            sstot += (ys[i] - ym) * (ys[i] - ym);
        }
        if (sxx == 0.0) return false;

        out.slope = sxy / sxx;
        out.alpha = ym - out.slope * xm;

        double ssres = 0;
        for (size_t i = 0; i < n; i++)
        {
            double res = ys[i] - (out.alpha + out.slope * xs[i]);
            ssres += res * res;
        }
        out.r2 = (sstot == 0.0) ? 1.0 : 1.0 - ssres / sstot;
        return true;
    }
};
