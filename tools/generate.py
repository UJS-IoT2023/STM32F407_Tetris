from pathlib import Path

import miniaudio


def main() -> None:
    decoded = miniaudio.decode_file("Korobeiniki.mp3", nchannels=1, sample_rate=8000)
    samples = decoded.samples
    sample_count = len(samples)

    Path("Core/Inc/korobeiniki_pcm.h").write_text(
        f"""#ifndef __KOROBEINIKI_PCM_H
#define __KOROBEINIKI_PCM_H

#include "main.h"

#define KOROBEINIKI_PCM_SAMPLE_RATE {decoded.sample_rate}U
#define KOROBEINIKI_PCM_CHANNELS {decoded.nchannels}U
#define KOROBEINIKI_PCM_SAMPLE_COUNT {sample_count}U

extern const int16_t korobeiniki_pcm[KOROBEINIKI_PCM_SAMPLE_COUNT];

#endif /* __KOROBEINIKI_PCM_H */
""",
        encoding="ascii",
    )

    with Path("Core/Src/korobeiniki_pcm.c").open("w", encoding="ascii", newline="\n") as f:
        f.write('#include "korobeiniki_pcm.h"\n\n')
        f.write("const int16_t korobeiniki_pcm[KOROBEINIKI_PCM_SAMPLE_COUNT] = {\n")
        for i in range(0, sample_count, 12):
            chunk = samples[i:i + 12]
            f.write("    " + ", ".join(str(int(v)) for v in chunk) + ",\n")
        f.write("};\n")

    print(
        f"sample_rate={decoded.sample_rate} channels={decoded.nchannels} "
        f"samples={sample_count} bytes={sample_count * 2}"
    )


if __name__ == "__main__":
    main()
