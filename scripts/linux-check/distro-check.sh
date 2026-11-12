#!/bin/sh
# Runs INSIDE a distribution container (started by the "linux-distros" job in
# .github/workflows/build.yml, which mounts the working directory at /w).
#
#   distro-check.sh <pkg-manager: apt|dnf|pacman|none> <expect: pass|fail> <bundle dir> "<runtime packages>"
#
# pass: install the runtime packages, print glibc and the `ldd` of the plugin binary (fail on
#       "not found"), run pluginval (strictness 10, no GUI tests) under xvfb.
# fail: the negative test. The distribution is older than the documented glibc floor, so the
#       plugin must fail to load with a "GLIBC_x.y not found" error; the script exits 0 only
#       if it does, and 1 if the plugin unexpectedly loads.
set -u
pm=$1 expect=$2 bundle=$3 pkgs=$4
pluginval=/w/pluginval

echo "::group::distribution"
. /etc/os-release && echo "$PRETTY_NAME"
ldd --version | head -n 1
echo "::endgroup::"

echo "::group::install runtime packages ($pm)"
case $pm in
  apt)    export DEBIAN_FRONTEND=noninteractive
          apt-get update -qq && apt-get install -y -qq --no-install-recommends $pkgs >/dev/null || exit 1 ;;
  dnf)    dnf -y -q --setopt=install_weak_deps=False install $pkgs || exit 1 ;;
  pacman) pacman -Syu --noconfirm --needed $pkgs >/dev/null || exit 1 ;;
  none)   ;;
  *)      echo "unknown package manager $pm"; exit 2 ;;
esac
echo "::endgroup::"

so=$(ls "$bundle"/Contents/x86_64-linux/*.so 2>/dev/null | head -n 1)
[ -n "$so" ] || { echo "no plugin binary under $bundle/Contents/x86_64-linux"; exit 1; }

echo "::group::ldd of the plugin"
ldd "$so" 2>&1 | tee /tmp/ldd.txt
echo "::endgroup::"

if [ "$expect" = fail ]; then
  # Negative test: documented floor is glibc 2.35. The loader must refuse the plugin.
  if grep -Eq "version .GLIBC_[0-9.]+. not found" /tmp/ldd.txt; then
    echo "EXPECTED FAILURE: the plugin needs a newer glibc than this distribution provides."
  else
    echo "UNEXPECTED: the plugin has no glibc requirement above this distribution; the documented floor is wrong."
    exit 1
  fi
  # pluginval itself may not even start here; either way it must not pass.
  if "$pluginval" --strictness-level 10 --validate-in-process --skip-gui-tests "$bundle" >/tmp/pv.txt 2>&1; then
    echo "UNEXPECTED: pluginval passed on a distribution below the floor."
    tail -n 20 /tmp/pv.txt
    exit 1
  fi
  tail -n 5 /tmp/pv.txt
  echo "pluginval failed as expected."
  exit 0
fi

if grep -q "not found" /tmp/ldd.txt; then
  echo "ldd reports unresolved dependencies (see above)."
  exit 1
fi

echo "::group::ldd of pluginval"
ldd "$pluginval" 2>&1 | grep -i "not found" && echo "pluginval itself is missing libraries (add them to the matrix entry)"
echo "::endgroup::"

xvfb-run -a "$pluginval" --strictness-level 10 --validate-in-process --skip-gui-tests "$bundle"
