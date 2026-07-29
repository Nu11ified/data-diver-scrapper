#include "dd/ml/columns.hpp"

#include "dd/core/core.hpp"
#include "dd/core/json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <random>
#include <thread>

namespace dd::columns {
double ClassResult::recall() const {
    return total == 0 ? 0.0 : static_cast<double>(correct) / static_cast<double>(total);
}

double ClassResult::precision() const {
    return predicted == 0 ? 0.0 : static_cast<double>(correct) / static_cast<double>(predicted);
}

double ClassResult::f1() const {
    const double p = precision();
    const double r = recall();
    return p + r == 0.0 ? 0.0 : 2.0 * p * r / (p + r);
}

namespace {
constexpr int kCls = 1;
constexpr int kSep = 2;
constexpr int kUnk = 3;
constexpr int kVocab = 4 + (126 - 32 + 1);
constexpr double kLnEps = 1e-5;
constexpr std::size_t kNameBudget = 24;
constexpr std::size_t kValueBudget = 12;

int char_token(unsigned char c) {
    if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
    if (c >= 32 && c <= 126) return 4 + (c - 32);
    return kUnk;
}

void validate_hyper(const Hyper& h) {
    const bool ok = h.seq_len >= 2 && h.seq_len <= 1024 && h.d_model >= 2 &&
                    h.d_model <= 1024 && h.heads >= 1 && h.heads <= h.d_model &&
                    h.d_model % h.heads == 0 && h.layers >= 1 && h.layers <= 16 &&
                    h.d_ffn >= 1 && h.d_ffn <= 4096;
    if (!ok) throw Error("columns: invalid architecture dimensions");
}

struct Layout {
    int d, ffn, layers, seq, classes;
    std::size_t tok_emb, pos_emb;
    struct Block {
        std::size_t ln1_g, ln1_b, wq, bq, wk, bk, wv, bv, wo, bo;
        std::size_t ln2_g, ln2_b, w1, b1, w2, b2;
    };
    std::vector<Block> blocks;
    std::size_t lnf_g, lnf_b, head_w, head_b, total;
};

Layout make_layout(const Hyper& h, int classes) {
    Layout l;
    l.d = h.d_model;
    l.ffn = h.d_ffn;
    l.layers = h.layers;
    l.seq = h.seq_len;
    l.classes = classes;
    std::size_t at = 0;
    auto take = [&at](std::size_t n) {
        const std::size_t here = at;
        at += n;
        return here;
    };
    const std::size_t d = static_cast<std::size_t>(l.d);
    const std::size_t ffn = static_cast<std::size_t>(l.ffn);
    l.tok_emb = take(kVocab * d);
    l.pos_emb = take(static_cast<std::size_t>(l.seq) * d);
    for (int i = 0; i < l.layers; ++i) {
        Layout::Block b;
        b.ln1_g = take(d);
        b.ln1_b = take(d);
        b.wq = take(d * d);
        b.bq = take(d);
        b.wk = take(d * d);
        b.bk = take(d);
        b.wv = take(d * d);
        b.bv = take(d);
        b.wo = take(d * d);
        b.bo = take(d);
        b.ln2_g = take(d);
        b.ln2_b = take(d);
        b.w1 = take(d * ffn);
        b.b1 = take(ffn);
        b.w2 = take(ffn * d);
        b.b2 = take(d);
        l.blocks.push_back(b);
    }
    l.lnf_g = take(d);
    l.lnf_b = take(d);
    l.head_w = take(d * static_cast<std::size_t>(classes));
    l.head_b = take(static_cast<std::size_t>(classes));
    l.total = at;
    return l;
}

void linear(const double* x, int n, int rows, int cols, const double* w, const double* b,
            double* out) {
    for (int i = 0; i < n; ++i) {
        double* o = out + static_cast<std::size_t>(i) * cols;
        for (int j = 0; j < cols; ++j) o[j] = b[j];
        const double* xi = x + static_cast<std::size_t>(i) * rows;
        for (int k = 0; k < rows; ++k) {
            const double v = xi[k];
            if (v == 0.0) continue;
            const double* wk = w + static_cast<std::size_t>(k) * cols;
            for (int j = 0; j < cols; ++j) o[j] += v * wk[j];
        }
    }
}

void linear_backward(const double* x, const double* dout, int n, int rows, int cols,
                     const double* w, double* dw, double* db, double* dx) {
    for (int i = 0; i < n; ++i) {
        const double* xi = x + static_cast<std::size_t>(i) * rows;
        const double* doi = dout + static_cast<std::size_t>(i) * cols;
        for (int j = 0; j < cols; ++j) db[j] += doi[j];
        for (int k = 0; k < rows; ++k) {
            double* dwk = dw + static_cast<std::size_t>(k) * cols;
            const double v = xi[k];
            double acc = 0.0;
            const double* wk = w + static_cast<std::size_t>(k) * cols;
            for (int j = 0; j < cols; ++j) {
                dwk[j] += v * doi[j];
                acc += wk[j] * doi[j];
            }
            if (dx != nullptr) dx[static_cast<std::size_t>(i) * rows + k] += acc;
        }
    }
}

void layer_norm(const double* x, int n, int d, const double* g, const double* b, double* out,
                double* xhat, double* inv_std) {
    for (int i = 0; i < n; ++i) {
        const double* xi = x + static_cast<std::size_t>(i) * d;
        double mean = 0.0;
        for (int j = 0; j < d; ++j) mean += xi[j];
        mean /= d;
        double var = 0.0;
        for (int j = 0; j < d; ++j) var += (xi[j] - mean) * (xi[j] - mean);
        var /= d;
        const double is = 1.0 / std::sqrt(var + kLnEps);
        inv_std[i] = is;
        double* xh = xhat + static_cast<std::size_t>(i) * d;
        double* oi = out + static_cast<std::size_t>(i) * d;
        for (int j = 0; j < d; ++j) {
            xh[j] = (xi[j] - mean) * is;
            oi[j] = g[j] * xh[j] + b[j];
        }
    }
}

void layer_norm_backward(const double* dout, const double* xhat, const double* inv_std, int n,
                         int d, const double* g, double* dg, double* db, double* dx) {
    for (int i = 0; i < n; ++i) {
        const double* doi = dout + static_cast<std::size_t>(i) * d;
        const double* xh = xhat + static_cast<std::size_t>(i) * d;
        double mean_dxhat = 0.0;
        double mean_dxhat_xhat = 0.0;
        for (int j = 0; j < d; ++j) {
            dg[j] += doi[j] * xh[j];
            db[j] += doi[j];
            const double dxh = doi[j] * g[j];
            mean_dxhat += dxh;
            mean_dxhat_xhat += dxh * xh[j];
        }
        mean_dxhat /= d;
        mean_dxhat_xhat /= d;
        double* dxi = dx + static_cast<std::size_t>(i) * d;
        for (int j = 0; j < d; ++j) {
            const double dxh = doi[j] * g[j];
            dxi[j] += inv_std[i] * (dxh - mean_dxhat - xh[j] * mean_dxhat_xhat);
        }
    }
}

void softmax_row(double* row, int n) {
    double mx = row[0];
    for (int i = 1; i < n; ++i) mx = std::max(mx, row[i]);
    double sum = 0.0;
    for (int i = 0; i < n; ++i) {
        row[i] = std::exp(row[i] - mx);
        sum += row[i];
    }
    for (int i = 0; i < n; ++i) row[i] /= sum;
}

struct Trace {
    std::vector<int> ids;
    int n = 0;
    std::vector<double> x0;
    struct BlockTrace {
        std::vector<double> x_in, a, xhat1, inv_std1;
        std::vector<double> q, k, v, probs, ctx;
        std::vector<double> x_mid, bnorm, xhat2, inv_std2;
        std::vector<double> h_pre, h;
    };
    std::vector<BlockTrace> blocks;
    std::vector<double> x_final, cls_norm, xhatf, inv_stdf;
    std::vector<double> logits, probs;
};

double cross_entropy(const std::vector<double>& logits, int target) {
    double mx = logits[0];
    for (double v : logits) mx = std::max(mx, v);
    double sum = 0.0;
    for (double v : logits) sum += std::exp(v - mx);
    return mx + std::log(sum) - logits[static_cast<std::size_t>(target)];
}

struct Net {
    Layout lay;
    int heads;

    void attention(Trace::BlockTrace* t, int n) const {
        const int d = lay.d;
        const int dh = d / heads;
        const double scale = 1.0 / std::sqrt(static_cast<double>(dh));
        t->probs.assign(static_cast<std::size_t>(heads) * n * n, 0.0);
        for (int h = 0; h < heads; ++h) {
            for (int i = 0; i < n; ++i) {
                double* row = t->probs.data() + (static_cast<std::size_t>(h) * n + i) * n;
                const double* qi = t->q.data() + static_cast<std::size_t>(i) * d + h * dh;
                for (int j = 0; j < n; ++j) {
                    const double* kj = t->k.data() + static_cast<std::size_t>(j) * d + h * dh;
                    double s = 0.0;
                    for (int e = 0; e < dh; ++e) s += qi[e] * kj[e];
                    row[j] = s * scale;
                }
                softmax_row(row, n);
                double* ctx = t->ctx.data() + static_cast<std::size_t>(i) * d + h * dh;
                for (int j = 0; j < n; ++j) {
                    const double* vj = t->v.data() + static_cast<std::size_t>(j) * d + h * dh;
                    for (int e = 0; e < dh; ++e) ctx[e] += row[j] * vj[e];
                }
            }
        }
    }

    void attention_backward(const Trace::BlockTrace& t, const double* d_ctx, int n, double* dq,
                            double* dk, double* dv) const {
        const int d = lay.d;
        const int dh = d / heads;
        const double scale = 1.0 / std::sqrt(static_cast<double>(dh));
        std::vector<double> dscore(static_cast<std::size_t>(n), 0.0);
        for (int h = 0; h < heads; ++h) {
            for (int i = 0; i < n; ++i) {
                const double* prow = t.probs.data() + (static_cast<std::size_t>(h) * n + i) * n;
                const double* dctx_i = d_ctx + static_cast<std::size_t>(i) * d + h * dh;
                double dot = 0.0;
                for (int j = 0; j < n; ++j) {
                    const double* vj = t.v.data() + static_cast<std::size_t>(j) * d + h * dh;
                    double dp = 0.0;
                    for (int e = 0; e < dh; ++e) dp += dctx_i[e] * vj[e];
                    dscore[static_cast<std::size_t>(j)] = dp;
                    dot += dp * prow[j];
                    double* dvj = dv + static_cast<std::size_t>(j) * d + h * dh;
                    for (int e = 0; e < dh; ++e) dvj[e] += prow[j] * dctx_i[e];
                }
                for (int j = 0; j < n; ++j) {
                    const double ds = prow[j] * (dscore[static_cast<std::size_t>(j)] - dot) * scale;
                    const double* kj = t.k.data() + static_cast<std::size_t>(j) * d + h * dh;
                    const double* qi = t.q.data() + static_cast<std::size_t>(i) * d + h * dh;
                    double* dqi = dq + static_cast<std::size_t>(i) * d + h * dh;
                    double* dkj = dk + static_cast<std::size_t>(j) * d + h * dh;
                    for (int e = 0; e < dh; ++e) {
                        dqi[e] += ds * kj[e];
                        dkj[e] += ds * qi[e];
                    }
                }
            }
        }
    }

    void run(const double* p, const std::vector<int>& ids, Trace* trace) const {
        const int d = lay.d;
        const int n = static_cast<int>(ids.size());
        trace->ids = ids;
        trace->n = n;
        trace->x0.assign(static_cast<std::size_t>(n) * d, 0.0);
        for (int i = 0; i < n; ++i) {
            const double* tok = p + lay.tok_emb + static_cast<std::size_t>(ids[i]) * d;
            const double* pos = p + lay.pos_emb + static_cast<std::size_t>(i) * d;
            double* xi = trace->x0.data() + static_cast<std::size_t>(i) * d;
            for (int j = 0; j < d; ++j) xi[j] = tok[j] + pos[j];
        }
        std::vector<double> x = trace->x0;
        trace->blocks.assign(lay.blocks.size(), {});
        for (std::size_t bi = 0; bi < lay.blocks.size(); ++bi) {
            const Layout::Block& blk = lay.blocks[bi];
            Trace::BlockTrace& t = trace->blocks[bi];
            t.x_in = x;
            t.a.assign(x.size(), 0.0);
            t.xhat1.assign(x.size(), 0.0);
            t.inv_std1.assign(static_cast<std::size_t>(n), 0.0);
            layer_norm(x.data(), n, d, p + blk.ln1_g, p + blk.ln1_b, t.a.data(), t.xhat1.data(),
                       t.inv_std1.data());
            t.q.assign(x.size(), 0.0);
            t.k.assign(x.size(), 0.0);
            t.v.assign(x.size(), 0.0);
            linear(t.a.data(), n, d, d, p + blk.wq, p + blk.bq, t.q.data());
            linear(t.a.data(), n, d, d, p + blk.wk, p + blk.bk, t.k.data());
            linear(t.a.data(), n, d, d, p + blk.wv, p + blk.bv, t.v.data());
            t.ctx.assign(x.size(), 0.0);
            attention(&t, n);
            std::vector<double> attn_out(x.size(), 0.0);
            linear(t.ctx.data(), n, d, d, p + blk.wo, p + blk.bo, attn_out.data());
            t.x_mid.assign(x.size(), 0.0);
            for (std::size_t i = 0; i < x.size(); ++i) t.x_mid[i] = x[i] + attn_out[i];
            t.bnorm.assign(x.size(), 0.0);
            t.xhat2.assign(x.size(), 0.0);
            t.inv_std2.assign(static_cast<std::size_t>(n), 0.0);
            layer_norm(t.x_mid.data(), n, d, p + blk.ln2_g, p + blk.ln2_b, t.bnorm.data(),
                       t.xhat2.data(), t.inv_std2.data());
            t.h_pre.assign(static_cast<std::size_t>(n) * lay.ffn, 0.0);
            linear(t.bnorm.data(), n, d, lay.ffn, p + blk.w1, p + blk.b1, t.h_pre.data());
            t.h = t.h_pre;
            for (double& v : t.h) v = v > 0.0 ? v : 0.0;
            std::vector<double> ffn_out(x.size(), 0.0);
            linear(t.h.data(), n, lay.ffn, d, p + blk.w2, p + blk.b2, ffn_out.data());
            x.assign(x.size(), 0.0);
            for (std::size_t i = 0; i < x.size(); ++i) x[i] = t.x_mid[i] + ffn_out[i];
        }
        trace->x_final = x;
        trace->cls_norm.assign(static_cast<std::size_t>(d), 0.0);
        trace->xhatf.assign(static_cast<std::size_t>(d), 0.0);
        trace->inv_stdf.assign(1, 0.0);
        layer_norm(x.data(), 1, d, p + lay.lnf_g, p + lay.lnf_b, trace->cls_norm.data(),
                   trace->xhatf.data(), trace->inv_stdf.data());
        trace->logits.assign(static_cast<std::size_t>(lay.classes), 0.0);
        linear(trace->cls_norm.data(), 1, d, lay.classes, p + lay.head_w, p + lay.head_b,
               trace->logits.data());
        trace->probs = trace->logits;
        softmax_row(trace->probs.data(), lay.classes);
    }

    void backprop(const double* p, const Trace& trace, int target, double* grad) const {
        const int d = lay.d;
        const int n = trace.n;
        std::vector<double> dlogits(trace.probs);
        dlogits[static_cast<std::size_t>(target)] -= 1.0;
        std::vector<double> d_cls(static_cast<std::size_t>(d), 0.0);
        linear_backward(trace.cls_norm.data(), dlogits.data(), 1, d, lay.classes, p + lay.head_w,
                        grad + lay.head_w, grad + lay.head_b, d_cls.data());
        std::vector<double> dx(static_cast<std::size_t>(n) * d, 0.0);
        layer_norm_backward(d_cls.data(), trace.xhatf.data(), trace.inv_stdf.data(), 1, d,
                            p + lay.lnf_g, grad + lay.lnf_g, grad + lay.lnf_b, dx.data());
        for (std::size_t bi = lay.blocks.size(); bi-- > 0;) {
            const Layout::Block& blk = lay.blocks[bi];
            const Trace::BlockTrace& t = trace.blocks[bi];
            std::vector<double> dh(static_cast<std::size_t>(n) * lay.ffn, 0.0);
            linear_backward(t.h.data(), dx.data(), n, lay.ffn, d, p + blk.w2, grad + blk.w2,
                            grad + blk.b2, dh.data());
            for (std::size_t i = 0; i < dh.size(); ++i) {
                if (t.h_pre[i] <= 0.0) dh[i] = 0.0;
            }
            std::vector<double> dbnorm(static_cast<std::size_t>(n) * d, 0.0);
            linear_backward(t.bnorm.data(), dh.data(), n, d, lay.ffn, p + blk.w1, grad + blk.w1,
                            grad + blk.b1, dbnorm.data());
            layer_norm_backward(dbnorm.data(), t.xhat2.data(), t.inv_std2.data(), n, d,
                                p + blk.ln2_g, grad + blk.ln2_g, grad + blk.ln2_b, dx.data());
            std::vector<double> d_ctx(static_cast<std::size_t>(n) * d, 0.0);
            linear_backward(t.ctx.data(), dx.data(), n, d, d, p + blk.wo, grad + blk.wo,
                            grad + blk.bo, d_ctx.data());
            std::vector<double> dq(static_cast<std::size_t>(n) * d, 0.0);
            std::vector<double> dk(static_cast<std::size_t>(n) * d, 0.0);
            std::vector<double> dv(static_cast<std::size_t>(n) * d, 0.0);
            attention_backward(t, d_ctx.data(), n, dq.data(), dk.data(), dv.data());
            std::vector<double> da(static_cast<std::size_t>(n) * d, 0.0);
            linear_backward(t.a.data(), dq.data(), n, d, d, p + blk.wq, grad + blk.wq,
                            grad + blk.bq, da.data());
            linear_backward(t.a.data(), dk.data(), n, d, d, p + blk.wk, grad + blk.wk,
                            grad + blk.bk, da.data());
            linear_backward(t.a.data(), dv.data(), n, d, d, p + blk.wv, grad + blk.wv,
                            grad + blk.bv, da.data());
            layer_norm_backward(da.data(), t.xhat1.data(), t.inv_std1.data(), n, d, p + blk.ln1_g,
                                grad + blk.ln1_g, grad + blk.ln1_b, dx.data());
        }
        for (int i = 0; i < n; ++i) {
            const double* dxi = dx.data() + static_cast<std::size_t>(i) * d;
            double* dtok = grad + lay.tok_emb + static_cast<std::size_t>(trace.ids[i]) * d;
            double* dpos = grad + lay.pos_emb + static_cast<std::size_t>(i) * d;
            for (int j = 0; j < d; ++j) {
                dtok[j] += dxi[j];
                dpos[j] += dxi[j];
            }
        }
    }
};
} // namespace

std::vector<int> tokenize(const std::string& name, const std::vector<std::string>& values,
                          std::size_t max_len) {
    std::vector<int> ids{kCls};
    for (std::size_t i = 0; i < name.size() && i < kNameBudget; ++i) {
        ids.push_back(char_token(static_cast<unsigned char>(name[i])));
    }
    for (const std::string& value : values) {
        ids.push_back(kSep);
        for (std::size_t i = 0; i < value.size() && i < kValueBudget; ++i) {
            ids.push_back(char_token(static_cast<unsigned char>(value[i])));
        }
    }
    if (ids.size() > max_len) ids.resize(max_len);
    return ids;
}

std::vector<Example> load_corpus(const std::string& path) {
    std::vector<Example> out;
    const std::string text = fileio::read_file(path);
    for (const std::string& line : str::split(text, '\n')) {
        const std::string trimmed{str::trim(line)};
        if (trimmed.empty()) continue;
        const json::Value row = json::parse(trimmed);
        Example e;
        if (const json::Value* v = row.find("name"); v != nullptr) e.name = v->as_string();
        if (const json::Value* v = row.find("label"); v != nullptr) e.label = v->as_string();
        if (const json::Value* v = row.find("domain"); v != nullptr) e.domain = v->as_string();
        if (const json::Value* v = row.find("label_source"); v != nullptr)
            e.label_source = v->as_string();
        if (const json::Value* v = row.find("values"); v != nullptr) {
            for (const json::Value& item : v->items()) e.values.push_back(item.as_string());
        }
        if (e.name.empty() || e.label.empty()) continue;
        out.push_back(std::move(e));
    }
    return out;
}

void append_corpus(const std::string& path, const std::vector<Example>& examples) {
    for (const Example& e : examples) {
        json::Writer w;
        w.begin_object();
        w.field("name", e.name);
        w.field("label", e.label);
        w.field("domain", e.domain);
        if (!e.label_source.empty()) w.field("label_source", e.label_source);
        w.key("values");
        w.begin_array();
        for (const std::string& v : e.values) w.string_value(v);
        w.end_array();
        w.end_object();
        fileio::append_line(path, w.take());
    }
}

void split_by_domain(const std::vector<Example>& all, int fold, std::vector<Example>* train,
                     std::vector<Example>* holdout) {
    if (fold <= 1) throw Error("columns: split fold must be at least 2");
    if (train == nullptr || holdout == nullptr) throw Error("columns: null split output");
    for (const Example& e : all) {
        std::uint64_t h = 1469598103934665603ULL;
        for (char c : e.domain) h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
        (h % static_cast<std::uint64_t>(fold) == 0 ? holdout : train)->push_back(e);
    }
}

std::size_t ColumnModel::parameter_count() const noexcept { return params_.size(); }

void ColumnModel::init_params(std::uint32_t seed) {
    const Layout lay = make_layout(hyper_, static_cast<int>(classes_.size()));
    params_.assign(lay.total, 0.0);
    std::mt19937 rng{seed};
    std::normal_distribution<double> dist{0.0, 0.02};
    for (double& v : params_) v = dist(rng);
    auto ones = [&](std::size_t at, int n) {
        for (int i = 0; i < n; ++i) params_[at + static_cast<std::size_t>(i)] = 1.0;
    };
    auto zeros = [&](std::size_t at, int n) {
        for (int i = 0; i < n; ++i) params_[at + static_cast<std::size_t>(i)] = 0.0;
    };
    for (const Layout::Block& b : lay.blocks) {
        ones(b.ln1_g, lay.d);
        zeros(b.ln1_b, lay.d);
        ones(b.ln2_g, lay.d);
        zeros(b.ln2_b, lay.d);
        zeros(b.bq, lay.d);
        zeros(b.bk, lay.d);
        zeros(b.bv, lay.d);
        zeros(b.bo, lay.d);
        zeros(b.b1, lay.ffn);
        zeros(b.b2, lay.d);
    }
    ones(lay.lnf_g, lay.d);
    zeros(lay.lnf_b, lay.d);
    zeros(lay.head_b, lay.classes);
}

TrainReport ColumnModel::train(const std::vector<Example>& train_set,
                               const std::vector<Example>& holdout, const TrainConfig& config) {
    validate_hyper(hyper_);
    if (train_set.empty()) throw Error("columns: no training examples");
    if (config.batch <= 0) throw Error("columns: batch must be positive");
    if (config.epochs < 0 || config.epochs > 1000) throw Error("columns: epochs out of range");
    if (!std::isfinite(config.lr) || config.lr <= 0.0) {
        throw Error("columns: learning rate must be positive and finite");
    }
    classes_.clear();
    for (const Example& e : train_set) {
        if (std::find(classes_.begin(), classes_.end(), e.label) == classes_.end()) {
            classes_.push_back(e.label);
        }
    }
    std::sort(classes_.begin(), classes_.end());
    if (classes_.size() < 2) {
        throw Error("columns: training needs at least two classes");
    }
    init_params(config.seed);
    const Layout lay = make_layout(hyper_, static_cast<int>(classes_.size()));
    const Net net{lay, hyper_.heads};

    std::map<std::string, int> class_index;
    for (std::size_t i = 0; i < classes_.size(); ++i) {
        class_index[classes_[i]] = static_cast<int>(i);
    }

    TrainReport report;
    report.train_examples = train_set.size();
    report.holdout_examples = holdout.size();
    report.parameters = params_.size();

    std::vector<double> m(params_.size(), 0.0);
    std::vector<double> v(params_.size(), 0.0);
    std::int64_t step = 0;
    std::vector<std::size_t> order(train_set.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::mt19937 rng{config.seed + 1};
    const int threads = config.threads > 0
                            ? config.threads
                            : std::max(1u, std::thread::hardware_concurrency());

    for (int epoch = 0; epoch < config.epochs; ++epoch) {
        std::shuffle(order.begin(), order.end(), rng);
        double epoch_loss = 0.0;
        for (std::size_t start = 0; start < order.size(); start += config.batch) {
            const std::size_t end = std::min(order.size(), start + config.batch);
            const std::size_t count = end - start;
            std::vector<std::vector<double>> grads(
                static_cast<std::size_t>(threads), std::vector<double>(params_.size(), 0.0));
            std::vector<double> losses(static_cast<std::size_t>(threads), 0.0);
            const auto worker = [&](int t) {
                Trace trace;
                for (std::size_t i = start + static_cast<std::size_t>(t); i < end;
                     i += static_cast<std::size_t>(threads)) {
                    const Example& e = train_set[order[i]];
                    const std::vector<int> ids =
                        tokenize(e.name, e.values, static_cast<std::size_t>(hyper_.seq_len));
                    net.run(params_.data(), ids, &trace);
                    const int target = class_index.at(e.label);
                    losses[static_cast<std::size_t>(t)] += cross_entropy(trace.logits, target);
                    net.backprop(params_.data(), trace, target,
                                 grads[static_cast<std::size_t>(t)].data());
                }
            };
            if (threads <= 1) {
                worker(0);
            } else {
                std::vector<std::thread> pool;
                for (int t = 0; t < threads; ++t) pool.emplace_back(worker, t);
                for (std::thread& th : pool) th.join();
            }
            for (int t = 1; t < threads; ++t) {
                for (std::size_t i = 0; i < params_.size(); ++i) grads[0][i] += grads[t][i];
            }
            for (int t = 0; t < threads; ++t) epoch_loss += losses[static_cast<std::size_t>(t)];
            ++step;
            const double bc1 = 1.0 - std::pow(0.9, static_cast<double>(step));
            const double bc2 = 1.0 - std::pow(0.999, static_cast<double>(step));
            for (std::size_t i = 0; i < params_.size(); ++i) {
                const double g = grads[0][i] / static_cast<double>(count);
                m[i] = 0.9 * m[i] + 0.1 * g;
                v[i] = 0.999 * v[i] + 0.001 * g * g;
                params_[i] -= config.lr * (m[i] / bc1) / (std::sqrt(v[i] / bc2) + 1e-8);
            }
        }
        report.epoch_loss.push_back(epoch_loss / static_cast<double>(train_set.size()));
    }

    std::map<std::string, ClassResult> per_class;
    std::size_t correct = 0;
    std::size_t positives = 0;
    std::size_t positives_correct = 0;
    for (const Example& e : holdout) {
        const Prediction p = predict(e.name, e.values);
        ClassResult& truth = per_class[e.label];
        truth.label = e.label;
        ++truth.total;
        ClassResult& guess = per_class[p.label];
        guess.label = p.label;
        ++guess.predicted;
        const bool hit = p.label == e.label;
        if (hit) {
            ++truth.correct;
            ++correct;
        }
        if (e.label != "none") {
            ++positives;
            if (hit) ++positives_correct;
        }
    }
    double f1_total = 0.0;
    std::size_t f1_classes = 0;
    for (const auto& [label, r] : per_class) {
        report.per_class.push_back(r);
        if (label == "none" || r.total == 0) continue;
        f1_total += r.f1();
        ++f1_classes;
    }
    report.holdout_accuracy =
        holdout.empty() ? 0.0 : static_cast<double>(correct) / static_cast<double>(holdout.size());
    report.macro_f1 = f1_classes == 0 ? 0.0 : f1_total / static_cast<double>(f1_classes);
    report.positive_examples = positives;
    report.positive_accuracy =
        positives == 0 ? 0.0
                       : static_cast<double>(positives_correct) / static_cast<double>(positives);
    return report;
}

Prediction ColumnModel::predict(const std::string& name,
                                const std::vector<std::string>& values) const {
    if (!trained()) throw Error("columns: model is not trained");
    const Layout lay = make_layout(hyper_, static_cast<int>(classes_.size()));
    const Net net{lay, hyper_.heads};
    Trace trace;
    net.run(params_.data(), tokenize(name, values, static_cast<std::size_t>(hyper_.seq_len)),
            &trace);
    Prediction out;
    for (std::size_t i = 0; i < classes_.size(); ++i) {
        out.distribution[classes_[i]] = trace.probs[i];
        if (trace.probs[i] > out.confidence) {
            out.confidence = trace.probs[i];
            out.label = classes_[i];
        }
    }
    return out;
}

double ColumnModel::loss_for(const Example& example) const {
    if (!trained()) throw Error("columns: model is not trained");
    const Layout lay = make_layout(hyper_, static_cast<int>(classes_.size()));
    const Net net{lay, hyper_.heads};
    Trace trace;
    net.run(params_.data(),
            tokenize(example.name, example.values, static_cast<std::size_t>(hyper_.seq_len)),
            &trace);
    const auto it = std::find(classes_.begin(), classes_.end(), example.label);
    if (it == classes_.end()) throw Error("columns: unknown label " + example.label);
    return cross_entropy(trace.logits,
                         static_cast<int>(std::distance(classes_.begin(), it)));
}

std::vector<double> ColumnModel::gradient_for(const Example& example) const {
    if (!trained()) throw Error("columns: model is not trained");
    const Layout lay = make_layout(hyper_, static_cast<int>(classes_.size()));
    const Net net{lay, hyper_.heads};
    Trace trace;
    net.run(params_.data(),
            tokenize(example.name, example.values, static_cast<std::size_t>(hyper_.seq_len)),
            &trace);
    const auto it = std::find(classes_.begin(), classes_.end(), example.label);
    if (it == classes_.end()) throw Error("columns: unknown label " + example.label);
    std::vector<double> grad(params_.size(), 0.0);
    net.backprop(params_.data(), trace,
                 static_cast<int>(std::distance(classes_.begin(), it)), grad.data());
    return grad;
}

std::string ColumnModel::serialize() const {
    std::string out = "{\"kind\":\"column_transformer\",\"hyper\":{";
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer),
                  "\"seq_len\":%d,\"d_model\":%d,\"heads\":%d,\"layers\":%d,\"d_ffn\":%d}",
                  hyper_.seq_len, hyper_.d_model, hyper_.heads, hyper_.layers, hyper_.d_ffn);
    out += buffer;
    out += ",\"classes\":[";
    for (std::size_t i = 0; i < classes_.size(); ++i) {
        if (i != 0) out += ',';
        out += json::quote(classes_[i]);
    }
    out += "],\"params\":[";
    for (std::size_t i = 0; i < params_.size(); ++i) {
        if (i != 0) out += ',';
        std::snprintf(buffer, sizeof(buffer), "%.17g", params_[i]);
        out += buffer;
    }
    out += "]}";
    return out;
}

ColumnModel ColumnModel::deserialize(const std::string& text) {
    const json::Value root = json::parse(text);
    const json::Value* kind = root.find("kind");
    if (kind == nullptr || kind->as_string() != "column_transformer") {
        throw Error("columns: not a column_transformer model");
    }
    ColumnModel model;
    const json::Value* hyper = root.find("hyper");
    if (hyper == nullptr) throw Error("columns: missing hyper");
    const auto hyper_int = [&](const char* name) {
        const json::Value* v = hyper->find(name);
        if (v == nullptr) throw Error(std::string{"columns: hyper missing "} + name);
        const double n = v->as_number();
        if (!std::isfinite(n) || n != std::floor(n)) {
            throw Error(std::string{"columns: hyper "} + name + " is not an integer");
        }
        return static_cast<int>(n);
    };
    model.hyper_.seq_len = hyper_int("seq_len");
    model.hyper_.d_model = hyper_int("d_model");
    model.hyper_.heads = hyper_int("heads");
    model.hyper_.layers = hyper_int("layers");
    model.hyper_.d_ffn = hyper_int("d_ffn");
    validate_hyper(model.hyper_);
    const json::Value* classes = root.find("classes");
    const json::Value* params = root.find("params");
    if (classes == nullptr || params == nullptr || classes->items().empty()) {
        throw Error("columns: missing classes or params");
    }
    if (classes->items().size() > 4096) throw Error("columns: too many classes");
    for (const json::Value& c : classes->items()) {
        const std::string label = c.as_string();
        if (label.empty()) throw Error("columns: empty class label");
        if (std::find(model.classes_.begin(), model.classes_.end(), label) !=
            model.classes_.end()) {
            throw Error("columns: duplicate class label " + label);
        }
        model.classes_.push_back(label);
    }
    const Layout lay = make_layout(model.hyper_, static_cast<int>(model.classes_.size()));
    model.params_.reserve(params->items().size());
    for (const json::Value& p : params->items()) {
        const double value = p.as_number();
        if (!std::isfinite(value)) throw Error("columns: non-finite parameter");
        model.params_.push_back(value);
    }
    if (model.params_.size() != lay.total) {
        throw Error("columns: parameter count does not match the architecture");
    }
    return model;
}

void ColumnModel::save(const std::string& path) const {
    fileio::write_file_atomic(path, serialize());
}

ColumnModel ColumnModel::load(const std::string& path) {
    return deserialize(fileio::read_file(path));
}
} // namespace dd::columns
