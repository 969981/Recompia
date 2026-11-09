SMAA (Enhanced Subpixel Morphological Antialiasing) by Jimenez, Echevarria, Masia, Navarro, Gutierrez, MIT licence (LICENSE.txt),
source https://github.com/iryoku/smaa commit 71c806a838bdd7d517df19192a20f0c61b3ca29d. SMAA.hlsl, AreaTex.h and SearchTex.h are unmodified;
scripts/gen_smaa_shaders.py compiles a temporary copy of SMAA.hlsl (as GLSL) whose colour edge fetches go through a function that compresses
scene-linear HDR input.
