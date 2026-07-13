#pragma once
#include <cstddef>
#include <vector>

// ============================================================
// Chunked region scanning math (pure / process-independent)
//
// A memory region of `regionSize` bytes is scanned for a comparison element
// of `elemLen` bytes (a numeric value width, an AOB pattern length, or a
// search-string length). Reading a multi-hundred-MB region into one vector is
// wasteful, so the region is split into chunks of at most `chunkSize` bytes.
//
// Each chunk carries:
//   offset  - byte offset of the chunk start within the region
//   readLen - number of bytes to read for this chunk (may extend past the
//             chunk's emit range by up to elemLen-1 bytes so an element that
//             straddles a chunk boundary is still fully readable in one read)
//   emitLen - number of leading element offsets this chunk is responsible for
//             emitting. Emit ranges tile the region with NO overlap, so every
//             matchable absolute offset is produced exactly once (no dupes).
//
// Emitting offsets [0, emitLen) within a chunk is always safe because
// readLen >= emitLen + elemLen - 1 by construction.
// ============================================================
struct ScanChunk {
    size_t offset = 0;
    size_t readLen = 0;
    size_t emitLen = 0;
};

// Compute the chunk layout for a region.
// Returns an empty vector when elemLen == 0, chunkSize < elemLen, or the region
// is smaller than one element (nothing matchable).
inline std::vector<ScanChunk> ComputeScanChunks(size_t regionSize,
                                                 size_t chunkSize,
                                                 size_t elemLen) {
    std::vector<ScanChunk> chunks;
    if (elemLen == 0 || chunkSize < elemLen || regionSize < elemLen)
        return chunks;

    // Total number of matchable element offsets across the whole region.
    const size_t totalEmit = regionSize - elemLen + 1;
    const size_t overlap = elemLen - 1;         // bytes shared with next chunk
    const size_t stride = chunkSize - overlap;  // emit span of a full chunk

    size_t emitted = 0;      // absolute element offsets emitted so far
    size_t chunkStart = 0;   // byte offset of current chunk
    while (emitted < totalEmit) {
        ScanChunk c;
        c.offset = chunkStart;
        c.readLen = (regionSize - chunkStart < chunkSize)
                        ? (regionSize - chunkStart)
                        : chunkSize;

        // Offsets this chunk can fully read an element at: [0, readable).
        size_t readable = c.readLen - elemLen + 1;
        size_t remaining = totalEmit - emitted;
        size_t emit = stride < remaining ? stride : remaining;
        if (emit > readable) emit = readable;   // safety clamp
        c.emitLen = emit;

        chunks.push_back(c);
        emitted += emit;
        chunkStart += emit;   // next chunk begins where this one stopped emitting
    }
    return chunks;
}
