#!/bin/zsh
# STUDY (cross-mod aliasing): standalone measurement harness, not part of the CMake build.
#   tests/study_crossmod/build.sh main && tests/study_crossmod/main f2=440 d=1 num=3 den=2 [dump]
#   tests/study_crossmod/build.sh sweep && tests/study_crossmod/sweep map|mapsync|waves|rates|lofreq [opts]
#   tests/study_crossmod/build.sh timbre && tests/study_crossmod/timbre
# Method: periodic by construction (VCO-2 on bin K2 = a k0, effective VCO-1 rate J = b k0 cycles per
# window, k0 odd), so every wanted component lies on a multiple of k0 and every aliased one off it.
# Options: car/mod=sine|tri|saw|sqr|pls, rate, os, f2, num/den (no sync) or sync=1 sr=<f1/f2>, d=<octaves>,
# prod=1 (production float path), fix=1 (study path, float), win=1 (7-term Blackman-Harris, guard 8 bins),
# trap/naive/hyb/eb/ssf/curv/lin/lpo/lpm/lphz (double-precision mechanism switches, see measure.h).
cd "$(dirname "$0")"
clang++ -std=c++20 -O2 -ffp-contract=off -I../../source "$1.cpp" -o "$1"
