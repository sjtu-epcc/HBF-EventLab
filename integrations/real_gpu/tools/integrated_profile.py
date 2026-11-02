"""M6 capability contract generated from the actual GGUF, never from a model label."""
import argparse
import hashlib
import json
import sys
from pathlib import Path
from setup_llama_baseline import ROOT, COMMIT, sha

sys.path.insert(0, str(ROOT / 'third_party/llama.cpp-integrated/gguf-py'))
from gguf import GGUFReader


def require(value, message):
    if not value:
        raise ValueError(message)


def ceildiv(a, b):
    return (a + b - 1) // b


def derive(model, conversion, *, context=512, ubatch=256, block_tokens=None, frames=4, layer=None):
    reader = GGUFReader(str(model))
    fields = {k: v.contents() for k, v in reader.fields.items() if not k.startswith('tokenizer.ggml.')}
    def field(key, default=None):
        f = reader.get_field(key)
        return default if f is None else f.contents()
    arch = field('general.architecture')
    require(arch in ('qwen3', 'llama'), 'unsupported architecture: ' + str(arch))
    prefix = arch + '.'
    def meta(key, default=None):
        return field(prefix + key, default)
    layers, hidden, ffn = [meta(x) for x in ('block_count', 'embedding_length', 'feed_forward_length')]
    q, kv = meta('attention.head_count'), meta('attention.head_count_kv')
    require(all(isinstance(x, int) and x > 0 for x in (layers, hidden, ffn, q, kv)), 'nonuniform/missing layer dimensions')
    hd = meta('attention.key_length', hidden // q)
    require(meta('attention.value_length', hd) == hd and hd in (32, 64, 128, 256), 'unsupported K/V head dimension')
    require(q % kv == 0 and q <= 64 and kv * hd * 2 <= 8192, 'unsupported GQA or row width')
    require(1 < layers <= 256 and 64 <= hidden <= 8192 and hidden % 32 == 0 and ffn % 512 == 0 and ffn <= 32768, 'unsupported norm/down dimensions')
    require(not meta('expert_count', 0) and not meta('attention.sliding_window', 0), 'MoE/sliding attention unsupported')
    require(meta('rope.scaling.type', 'none') == 'none' and not meta('attention.logit_softcap', 0), 'scaled RoPE/softcap unsupported')
    require(meta('rope.dimension_count', hd) == hd, 'partial rotary unsupported')
    require(32 <= context <= min(4096, meta('context_length')) and 1 <= ubatch <= 256, 'context/ubatch budget unsupported')
    tensors = {t.name: {'name': t.name, 'shape': [int(x) for x in t.shape], 'dtype': t.tensor_type.name, 'bytes': int(t.n_bytes)} for t in reader.tensors}
    require(all(t['dtype'] in ('F16', 'F32') for t in tensors.values()), 'quantized weights unsupported')
    for i in range(layers):
        checks = {'ffn_norm': ([hidden], 'F32'), 'ffn_down': ([ffn, hidden], 'F16'),
                  'attn_q': ([hidden, q * hd], 'F16'), 'attn_k': ([hidden, kv * hd], 'F16'),
                  'attn_v': ([hidden, kv * hd], 'F16'), 'attn_output': ([q * hd, hidden], 'F16')}
        for suffix, (shape, dtype) in checks.items():
            t = tensors.get(f'blk.{i}.{suffix}.weight')
            require(t and t['shape'] == shape and t['dtype'] == dtype, 'unsupported tensor: ' + f'blk.{i}.{suffix}')
        require(not any(name.startswith(f'blk.{i}.') and (name.endswith('.bias') or 'rope_freq' in name) for name in tensors), 'bias/rotary factors unsupported')
    row_bytes = kv * hd * 2
    block_tokens = block_tokens or (2 if row_bytes == 2048 else 16)
    require(1 <= block_tokens <= 256, 'invalid block tokens')
    pages_per_alloc = ceildiv(row_bytes * block_tokens, 4096)
    logical = layers * 2 * ceildiv(context, block_tokens) * pages_per_alloc
    # A token row may span up to three pages; both K and V must be pinned together.
    max_row_pages = max(ceildiv((t * row_bytes) % 4096 + row_bytes, 4096) for t in range(block_tokens))
    require(frames >= 2 * max_row_pages and frames <= 64, 'insufficient K/V lease frame working set')
    layer = min(10, layers - 2) if layer is None else layer
    require(0 <= layer < layers - 1, 'selected layer must precede final logits row pruning')
    norm, down = f'blk.{layer}.ffn_norm.weight', f'blk.{layer}.ffn_down.weight'
    weight_pages = ceildiv(hidden * 4, 4096) + ceildiv(ffn * 2, 4096)
    # Keep A geometry identical; derive larger geometry if a future profile needs it.
    banks, pages_per_block = 2, 4
    blocks_per_bank = max(2048, ceildiv(logical + weight_pages, banks * pages_per_block))
    c = json.loads(conversion.read_text(encoding='utf-8-sig'))
    require(c['gguf_sha256'] == sha(model) and c['llama_commit'] == COMMIT, 'conversion identity mismatch')
    original_a = c['source']['repo'] == 'Qwen/Qwen3-0.6B'
    numerical = ({'max_abs': 0.2, 'rms': 0.025, 'tokens_exact': True, 'basis': 'frozen M5 acceptance, same 128D reduction and operator order'} if original_a else
                 {'relative_l2': 0.005, 'max_abs_over_native_rms': 0.04, 'tokens_exact': True,
                  'basis': 'predeclared dimensionless FP16 KV/reduction envelope; independent attention oracle max_abs <= 1e-5; no tuning after runs'})
    return {'schema': 1, 'model_id': c['source']['repo'], 'revision': c['source']['revision'],
            'model_path': str(model.resolve()), 'gguf_sha256': c['gguf_sha256'], 'conversion': str(conversion.resolve()),
            'framework_commit': COMMIT, 'architecture': arch, 'layers': layers, 'hidden': hidden, 'ffn': ffn,
            'q_heads': q, 'kv_heads': kv, 'head_dim': hd, 'per_layer_shapes': [{'layer': i, 'q_heads': q, 'kv_heads': kv, 'k_dim': hd, 'v_dim': hd} for i in range(layers)],
            'rope': {'type': 'normal' if arch == 'llama' else 'neox', 'base': meta('rope.freq_base'), 'dimensions': hd, 'scaling': 'none', 'implementation': 'pinned llama.cpp before page adapter'},
            'attention': 'causal dense GQA/MHA; scale=1/sqrt(head_dim); no sliding, bias or softcap',
            'weight_dtype': 'F16 matrices/F32 vectors', 'kv_dtype': 'F16', 'context': context, 'batch': 256,
            'ubatch': ubatch, 'sequences': 1, 'block_tokens': block_tokens, 'row_bytes': row_bytes,
            'pages_per_allocation': pages_per_alloc, 'minimum_frames': max_row_pages * 2, 'frames': frames,
            'logical_pages': logical, 'pages_per_block': pages_per_block, 'blocks_per_bank': blocks_per_bank,
            'layer': layer, 'norm_name': norm, 'down_name': down, 'norm_bytes': hidden * 4, 'down_bytes': ffn * 2,
            'coverage': [{'tensor': norm, 'offset': 0, 'bytes': hidden * 4, 'op': 'MUL', 'class': 'automatic PTX'},
                         {'tensor': down, 'offset': 0, 'bytes': ffn * 2, 'op': 'MUL_MAT', 'class': 'warp helper'},
                         {'tensor': 'all layer K/V', 'class': 'Capacity page leases'}, {'tensor': 'remaining weights/activations', 'class': 'native unmodeled'}],
            'kernel': {'entry': 'm6_weight_mul', 'grid': 2, 'block': 128, 'width_parameter': hidden, 'native_input_bytes': hidden * 4, 'output_bytes': hidden * 4, 'fusion': 'selected norm/MUL and down unfused', 'alias': 'selected MUL input/output disjoint', 'down_precision': 'upstream per-token F32 MMVF'},
            'budgets': {'trace_records': 256 * ffn // 64, 'audit_records': 256 * (hidden + ffn // 64),
                        'max_events': 2000000000, 'gpu_required_bytes': sum(t['bytes'] for t in tensors.values()) + 768 * 1024**2,
                        'host_required_bytes': logical * 4096 * 8 + 512 * 1024**2, 'producer_slots': 512, 'launch_slots': 2},
            'access_plan': {'ptx_requests_per_row': hidden, 'helper_requests_per_row': ffn // 64,
                            'sectors_per_row': hidden + ffn // 32, 'weight_pages': weight_pages,
                            'kv_logical_base': weight_pages * 4096, 'initial_programs': logical + weight_pages},
            'tokenizer': {'model': field('tokenizer.ggml.model'), 'pre': field('tokenizer.ggml.pre'), 'chat_template': field('tokenizer.chat_template'), 'prompt_policy': 'literal UTF-8, add_special=true, parse_special=true; no implicit chat template'},
            'numerical': numerical, 'tensors': list(tensors.values()), 'gguf_metadata': fields}


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--model', type=Path, required=True); p.add_argument('--conversion', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True); p.add_argument('--ubatch', type=int, default=256)
    p.add_argument('--frames', type=int, default=4); p.add_argument('--block-tokens', type=int)
    a = p.parse_args()
    profile = derive(a.model, a.conversion, ubatch=a.ubatch, frames=a.frames, block_tokens=a.block_tokens)
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_text(json.dumps(profile, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    print(a.output, sha(a.output))


if __name__ == '__main__':
    main()
