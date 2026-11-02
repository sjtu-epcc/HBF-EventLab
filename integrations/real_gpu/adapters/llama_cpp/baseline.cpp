#include "llama.h"
#include "ggml-backend.h"
#include <cuda_runtime_api.h>
#include <cupti.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <vector>

using json = nlohmann::ordered_json;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
static std::mutex trace_mutex;
static std::ofstream kernel_log, node_log;
static std::atomic<int> current_step{-1};
static std::atomic<uint64_t> kernel_count{0}, dropped{0}, trace_errors{0};

static void check(CUptiResult result) {
    if (result != CUPTI_SUCCESS) {
        const char * text = nullptr;
        cuptiGetResultString(result, &text);
        throw std::runtime_error(std::string("CUPTI: ") + (text ? text : "unknown error"));
    }
}

static void CUPTIAPI request_buffer(uint8_t ** buffer, size_t * size, size_t * max_records) {
    *size = 8 * 1024 * 1024;
    *buffer = static_cast<uint8_t *>(_aligned_malloc(*size, 8));
    *max_records = 0;
    if (!*buffer) { *size = 0; ++trace_errors; }
}

static void CUPTIAPI complete_buffer(CUcontext ctx, uint32_t stream, uint8_t * buffer, size_t, size_t valid) {
    try {
        CUpti_Activity * activity = nullptr;
        CUptiResult result;
        std::lock_guard<std::mutex> guard(trace_mutex);
        while ((result = cuptiActivityGetNextRecord(buffer, valid, &activity)) == CUPTI_SUCCESS) {
            if (activity->kind != CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL) continue;
            const auto * k = reinterpret_cast<const CUpti_ActivityKernel10 *>(activity);
            const int step = current_step.load();
            json row = {{"step", step}, {"phase", step == 0 ? "prefill" : "decode"},
                {"name", k->name}, {"start_ns", k->start}, {"end_ns", k->end},
                {"device", k->deviceId}, {"context", k->contextId}, {"stream", k->streamId},
                {"correlation", k->correlationId}, {"grid", {k->gridX,k->gridY,k->gridZ}},
                {"block", {k->blockX,k->blockY,k->blockZ}}, {"registers", k->registersPerThread},
                {"static_shared", k->staticSharedMemory}, {"dynamic_shared", k->dynamicSharedMemory}};
            kernel_log << row.dump() << '\n';
            ++kernel_count;
        }
        if (result != CUPTI_ERROR_MAX_LIMIT_REACHED) ++trace_errors;
        size_t lost = 0;
        if (cuptiActivityGetNumDroppedRecords(ctx, stream, &lost) != CUPTI_SUCCESS) ++trace_errors;
        dropped += lost;
        if (!kernel_log) ++trace_errors;
    } catch (...) { ++trace_errors; }
    _aligned_free(buffer);
}

static json tensor_info(const ggml_tensor * t) {
    json item = {{"name", t->name}, {"type", ggml_type_name(t->type)},
        {"shape", {t->ne[0],t->ne[1],t->ne[2],t->ne[3]}},
        {"strides", {t->nb[0],t->nb[1],t->nb[2],t->nb[3]}}, {"bytes", ggml_nbytes(t)}};
    const auto * storage = t;
    while (!storage->buffer && storage->view_src) storage = storage->view_src;
    item["buffer"] = storage->buffer ? ggml_backend_buffer_name(storage->buffer) : "none";
    item["address"] = reinterpret_cast<uintptr_t>(t->data);
    return item;
}

static bool observe_node(ggml_tensor * t, bool ask, void *) {
    if (!ask) return true;
    try {
        const int step = current_step.load();
        if (step < 0) return false;
        json row = {{"step", step}, {"phase", step == 0 ? "prefill" : "decode"},
            {"op", ggml_op_name(t->op)}, {"tensor", tensor_info(t)}, {"sources", json::array()}};
        for (auto * src : t->src) if (src) row["sources"].push_back(tensor_info(src));
        node_log << row.dump() << '\n';
        if (!node_log) ++trace_errors;
    } catch (...) { ++trace_errors; }
    // Metadata only. The scheduler may still synchronize at split boundaries.
    return false;
}

static std::ofstream output(const fs::path & p, bool binary = false) {
    std::ofstream f(p, std::ios::out | (binary ? std::ios::binary : std::ios::openmode(0)));
    if (!f) throw std::runtime_error("Cannot write " + p.string());
    return f;
}

static int run(int argc, char ** argv) {
    if (argc != 6) throw std::runtime_error("Usage: hbflab_llama_baseline model.gguf prompt.txt output_dir token_count observe_0_or_1");
    const fs::path out = argv[3];
    const int count = std::stoi(argv[4]);
    const bool observe = std::stoi(argv[5]) != 0;
    if (count < 2 || count > 128) throw std::runtime_error("token_count must be 2..128");
    if (fs::exists(out)) throw std::runtime_error("Output directory already exists; refusing to overwrite evidence");
    fs::create_directories(out);
    std::ifstream prompt_file(argv[2], std::ios::binary);
    if (!prompt_file) throw std::runtime_error("Cannot open prompt");
    const std::string prompt((std::istreambuf_iterator<char>(prompt_file)), {});
    if (prompt.empty()) throw std::runtime_error("Empty prompt");
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices != 1) throw std::runtime_error("M0 requires exactly one visible CUDA GPU");
    cudaDeviceProp device{};
    if (cudaGetDeviceProperties(&device, 0) != cudaSuccess) throw std::runtime_error("CUDA device query failed");
    llama_backend_init();
    auto mp = llama_model_default_params();
    mp.n_gpu_layers = 999;
    mp.split_mode = LLAMA_SPLIT_MODE_NONE;
    mp.main_gpu = 0;
    std::unique_ptr<llama_model, decltype(&llama_model_free)> model(llama_model_load_from_file(argv[1], mp), llama_model_free);
    if (!model) throw std::runtime_error("Model load failed");
    const auto * vocab = llama_model_get_vocab(model.get());
    int n = -llama_tokenize(vocab, prompt.data(), static_cast<int32_t>(prompt.size()), nullptr, 0, true, true);
    if (n <= 0 || n > 256 || n + count > 512) throw std::runtime_error("Prompt outside M0 context limits");
    std::vector<llama_token> tokens(n);
    if (llama_tokenize(vocab, prompt.data(), static_cast<int32_t>(prompt.size()), tokens.data(), n, true, true) != n)
        throw std::runtime_error("Tokenization failed");
    auto cp = llama_context_default_params();
    cp.n_ctx = 512; cp.n_batch = 256; cp.n_ubatch = 256; cp.n_seq_max = 1;
    cp.n_threads = 8; cp.n_threads_batch = 8;
    cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_DISABLED;
    cp.type_k = GGML_TYPE_F16; cp.type_v = GGML_TYPE_F16;
    cp.offload_kqv = true; cp.op_offload = true; cp.no_perf = false;
    if (observe) { cp.cb_eval = observe_node; cp.cb_eval_user_data = nullptr; }
    std::unique_ptr<llama_context, decltype(&llama_free)> ctx(llama_init_from_model(model.get(), cp), llama_free);
    if (!ctx) throw std::runtime_error("Context init failed");
    if (observe) {
        kernel_log = output(out / "kernels.jsonl");
        node_log = output(out / "nodes.jsonl");
        check(cuptiActivityRegisterCallbacks(request_buffer, complete_buffer));
        check(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL));
    }
    std::unique_ptr<llama_batch_ext, decltype(&llama_batch_ext_free)> batch(llama_batch_ext_init(ctx.get()), llama_batch_ext_free);
    std::unique_ptr<llama_sampler, decltype(&llama_sampler_free)> sampler(llama_sampler_init_greedy(), llama_sampler_free);
    if (!batch || !sampler) throw std::runtime_error("Batch/sampler init failed");
    auto logits_file = output(out / "logits.f32", true);
    auto generated_file = output(out / "generated.txt", true);
    auto steps_file = output(out / "steps.jsonl");
    const int nvocab = llama_vocab_n_tokens(vocab);
    json generated = json::array(), times = json::array();
    std::vector<llama_token> input = tokens;
    llama_pos position = 0;
    for (int step = 0; step < count; ++step) {
        current_step = step;
        llama_batch_ext_clear(batch.get());
        for (const auto token : input) {
            const int index = llama_batch_ext_add_token(batch.get(), 0, token);
            const llama_pos pos = position++;
            if (index < 0 || !llama_batch_ext_set_pos(batch.get(), index, &pos)) throw std::runtime_error("Batch token failed");
        }
        if (!llama_batch_ext_set_output_logits(batch.get(), static_cast<int32_t>(input.size() - 1), true)) throw std::runtime_error("Batch logits failed");
        const auto start = Clock::now();
        if (llama_process(ctx.get(), LLAMA_PROCESS_TYPE_DECODE, batch.get()) != 0) throw std::runtime_error("llama_process failed");
        llama_synchronize(ctx.get());
        const double ms = std::chrono::duration<double,std::milli>(Clock::now() - start).count();
        // All work is synchronized and flushed before changing step attribution.
        if (observe) check(cuptiActivityFlushAll(0));
        float * logits = llama_get_logits_ith(ctx.get(), -1);
        if (!logits) throw std::runtime_error("Missing logits");
        for (int i = 0; i < nvocab; ++i) if (!std::isfinite(logits[i])) throw std::runtime_error("Non-finite logits");
        logits_file.write(reinterpret_cast<char *>(logits), static_cast<std::streamsize>(nvocab) * sizeof(float));
        const auto token = llama_sampler_sample(sampler.get(), ctx.get(), -1);
        if (llama_vocab_is_eog(vocab, token)) throw std::runtime_error("Early EOG: baseline requires the requested token count");
        char piece[1024];
        const int length = llama_token_to_piece(vocab, token, piece, sizeof(piece), 0, true);
        if (length < 0) throw std::runtime_error("Token piece too large");
        generated_file.write(piece, length);
        std::cout.write(piece, length);
        std::cout.flush();
        generated.push_back(token); times.push_back(ms);
        json row = {{"step",step}, {"phase",step == 0 ? "prefill" : "decode"}, {"input_tokens",input.size()},
                    {"token",token}, {"logit",logits[token]}, {"process_wall_ms",ms}};
        steps_file << row.dump() << '\n';
        input.assign(1, token);
    }
    if (observe) {
        check(cuptiActivityFlushAll(0));
        check(cuptiActivityDisable(CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL));
        kernel_log.flush(); node_log.flush();
        if (!kernel_log || !node_log || dropped || trace_errors || !kernel_count) throw std::runtime_error("Incomplete CUPTI/node trace");
    }
    logits_file.flush(); generated_file.flush(); steps_file.flush();
    if (!logits_file || !generated_file || !steps_file) throw std::runtime_error("Output write failed");
    json summary = {{"status","success"}, {"llama_commit",M0_LLAMA_COMMIT}, {"model",argv[1]},
        {"gpu",device.name}, {"compute_capability",{device.major,device.minor}}, {"observed",observe},
        {"cuda_graphs",false}, {"flash_attention",false}, {"hbf_modeled",false}, {"n_ctx",512},
        {"n_batch",256}, {"n_ubatch",256}, {"n_threads",8}, {"n_gpu_layers_requested",999},
        {"model_layers",llama_model_n_layer(model.get())}, {"prompt",prompt}, {"prompt_tokens",tokens},
        {"generated_tokens",generated}, {"vocab_size",nvocab}, {"logits_shape",{count,nvocab}},
        {"logits_format","little-endian float32; one next-token row per step"},
        {"sampler","greedy"}, {"process_wall_ms",times}, {"kernel_records",kernel_count.load()},
        {"cupti_dropped",dropped.load()}, {"trace_errors",trace_errors.load()}};
    auto summary_file = output(out / "summary.json");
    summary_file << summary.dump(2) << '\n';
    summary_file.close();
    if (!summary_file) throw std::runtime_error("Summary write failed");
    llama_perf_context_print(ctx.get());
    std::cout << '\n';
    return 0;
}

int main(int argc, char ** argv) {
    try { return run(argc, argv); }
    catch (const std::exception & e) { std::cerr << "M0 failure: " << e.what() << '\n'; return 1; }
}
