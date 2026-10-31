# P7 CTS caselists

Cut from the local VK-GL-CTS checkout at `0b04c470ed78569b0b778ece1262d88455ab2b00` (branch `main`, the source of the glcts build deployed to the device), files
`external/openglcts/data/gl_cts/data/mustpass/gl/khronos_mustpass/main/gl46-main.txt` and `gl33-main.txt`.
The blocks are P7 exit gate 5 (`CONTRACT-P7.md` §7.3); the `gl33` variants exist because `KHR-GL46` is unproven on the
Android runner (runbook §9.1(b)) - the `$BASE` and AFTER readings must use the SAME API version. Line counts are the
denominators before NotSupported is removed:

       371 p7-dsa-gl46.txt
      4732 p7-packed-pixels-gl33.txt
      4732 p7-packed-pixels-gl46.txt
       125 p7-shader-image-gl46.txt
       124 p7-ssbo-gl46.txt
       868 p7-texture-gl33.txt
      1055 p7-texture-gl46.txt
        90 p7-ubo-gl33.txt
        90 p7-ubo-gl46.txt
     12187 total

Regenerate: `git -C ~/cts-src show HEAD:<mustpass path> | grep -E '^KHR-GL46\.<block>'`. A reading is citable only together
with the commit above and the file it was cut from.

`KHR-GL46` carries no `uniform_buffer_object` group: the UBO conformance cases are the GTF module's
`GTF-GL46.gtf31.GL3Tests.uniform_buffer_object*` (90; present only if the deployed glcts was built with GTF -
the device probe decides), so `p7-ubo-*.txt` is cut from `gl46-gtf-main.txt` / `gl33-gtf-main.txt`. `p7-ssbo-gl46.txt`
(`KHR-GL46.shader_storage_buffer_object`, 124) is added because cluster A's unaligned/native-range refusals are SSBO
ranges, not only UBO tails. GL 3.3 has no shader images and no DSA, so those two blocks have no gl33 variant.
