import sys
skip = {'log_file', 'log_level', 'gpu_plugin', 'headless', 'gtav_native_spirv_dir', 'window_presenter',
        'guest_memory_large_pages', 'gtav_native_vsync', 'gtav_native_stats', 'dlc_enable', 'gtav_native',
        'gpu_skip_rendering', 'gpu_skip_rendering_lag_ms'}
out = []
for line in open(sys.argv[1] if len(sys.argv) > 1 else 'gtav/gtav_switch.toml'):
    line = line.strip()
    if not line or line.startswith('#') or '=' not in line:
        continue
    k, v = [x.strip() for x in line.split('=', 1)]
    if k in skip:
        continue
    out.append(f'--{k}={v.strip(chr(34))}')
print(' '.join(out))
