#!/bin/sh
# The macOS bundle versions of a release tag: prints "<CFBundleShortVersionString> <CFBundleVersion>".
# Usage: tools/bundle_version.sh <tag> [<test-of tag>]   (tools/bundle_macos.sh; checked by tools/test_update_tags.cpp)
# Short version (Finder, About): the semantic version under the tag - beta_<n> -> 0.<n>.0, beta_<n>.<m> -> 0.<n>.<m>,
# v0.6.0 -> 0.6.0, v1.2.3 -> 1.2.3. Build number: one number that always goes up, major*10000 + minor*100 + patch
# (0.5.3 -> 503, 0.6.0 -> 600, 1.0.0 -> 10000). macOS compares build numbers part by part, so 503 is above Beta 5.2's
# 0.9.5.2. A test build (test-<name>, previewing <test-of>) is 0.0.1 to players and <test-of's build - 1>.1 inside:
# above the release before it, below the one it previews.
set -eu
tag=$1
semver() {
    case "$1" in
        multi-beta*.*) echo "0.${1#multi-beta}" ;;
        enhanced-beta*.*) echo "0.${1#enhanced-beta}" ;;
        beta_*.*) echo "0.${1#beta_}" ;;
        beta_*) echo "0.${1#beta_}.0" ;;
        v*) echo "${1#v}" ;;
        *) echo "bundle_version.sh: not a release tag: $1" >&2; exit 1 ;;
    esac
}
build() {
    echo "$1" | awk -F. 'NF != 3 || $1 !~ /^[0-9]+$/ || $2 !~ /^[0-9]+$/ || $3 !~ /^[0-9]+$/ || $2 > 99 || $3 > 99 {
        print "bundle_version.sh: no build number for " $0 > "/dev/stderr"; exit 1 }
        { print $1 * 10000 + $2 * 100 + $3 }'
}
case "$tag" in
    test-*)
        [ $# -ge 2 ] || { echo "bundle_version.sh: a test build needs the tag it previews" >&2; exit 1; }
        b=$(build "$(semver "$2")")
        echo "0.0.1 $((b - 1)).1" ;;
    *)
        v=$(semver "$tag")
        echo "$v $(build "$v")" ;;
esac
