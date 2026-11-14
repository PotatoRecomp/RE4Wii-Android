# Packs a file with zstd for the APK (libmain.so -> assets/native/libmain.so.zst, unpacked by the app's
# NativeLib). Needs Python 3.14+ (compression.zstd). Level 19, window 2^27: the decoder's default limit.
import sys
from compression import zstd

P = zstd.CompressionParameter
data = open(sys.argv[1], 'rb').read()
packed = zstd.compress(data, options={P.compression_level: 19, P.window_log: 27,
                                      P.enable_long_distance_matching: 1, P.nb_workers: 8})
open(sys.argv[2], 'wb').write(packed)
print(f'packed {len(data) / 1048576:.1f} MB -> {len(packed) / 1048576:.1f} MB')
