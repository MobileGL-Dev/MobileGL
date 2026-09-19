# P5b user-index span extent fix

- Commit: `0b67568e` (`p5b/indexspan-codex`), parent `348d22a4`.
- Tree: `/home/swung/w7/p5b-d1-codex`.
- Finding: `Offset + Size` staying within SEG_STAGE did not prove that the draw's declared index count fit inside that run. A span at the final two bytes with `Size=2`, `IndexSize=2`, `Count=3` could reach a backend read beyond the segment.
- Change: one shared gate in the encoder, decoder and server sink requires exactly one direct indexed range, index width 1/2/4, client-array `Start=0`, and `Uint64(Count) * IndexSize <= Size`. Existing segment bounds/honesty gates remain in place.
- Validation: `PipeWireCodecTest.*Draw*:PipeWireCodecTest.UserIndex*` **9/9 passed**. The two-byte reproducer dies at all three entry points. Additional negative cases cover arrays, invalid width, zero/multiple ranges, nonzero Start and 32-bit multiplication wrap. An exact-fit two-byte span at segment end passes encoding, decoding and sink validation.
- Build: `cmake --build build-split -j 8 --target PipeWireCodecTest`, completed. Logs: `/home/swung/w7/p5b-indexspan-build.log`, `/home/swung/w7/p5b-indexspan-test.log`.
- No additional full gate was run. The concurrent final census froze `348d22a4`; its results are not evidence for this later commit.
