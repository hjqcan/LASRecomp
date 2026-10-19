"""Compile the actual Multi update-channel parser; no network or game data."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as temporary:
    directory = Path(temporary)
    source = directory / 'channel.cpp'
    source.write_text(r'''
#include "ui/release_channel.h"
#include <cassert>
#include <cstdio>
int main() {
    assert(multi_tag_rank("multi-beta5.4") > multi_tag_rank("enhanced-beta5.3"));
    assert(multi_tag_rank("multi-beta5.4") == multi_tag_rank("enhanced-beta5.4"));
    assert(multi_tag_rank("multi-beta6.1") > multi_tag_rank("multi-beta5.65535"));
    assert(multi_tag_label("multi-beta5.4") == "Multi Beta 5.4");
    assert(multi_tag_label("enhanced-beta5.3") == "Multi Beta 5.3");
    assert(multi_tag_label("test-preview") == "Test build preview");
    assert(multi_tag_rank("multi-beta2147483647.65535") > 0);
    const char *invalid[] = {"beta_5", "v1.0.0", "test-preview", "multi-beta", "multi-beta5",
        "multi-beta0.1", "multi-beta5.0", "multi-beta5.65536", "multi-beta2147483648.1",
        "multi-beta9999999999999999999999999.1", "multi-beta5.4-extra", "multi-beta+5.4",
        "multi-beta-5.4", "multi-beta5.-4", "multi-beta 5.4", "multi-beta5.4 ",
        "enhanced-beta5.4.1", "multi-beta5.4/other"};
    for (auto tag : invalid) assert(multi_tag_rank(tag) == -1);
    long long current = multi_tag_rank("multi-beta5.4");
    std::string best;
    for (auto tag : {"enhanced-beta5.3", "beta_99", "multi-beta5.6", "multi-beta5.5"}) {
        auto rank = multi_tag_rank(tag);
        if (rank > current) { current = rank; best = tag; }
    }
    assert(best == "multi-beta5.6");
    puts("PASS: Multi updates, historical ordering, display labels, unrelated channels and malformed/overflowing tags");
}
''', encoding='utf-8')
    executable = directory / ('channel.exe' if os.name == 'nt' else 'channel')
    subprocess.run(shlex.split(os.environ.get('CXX', 'clang++')) +
                   ['-std=c++17', '-Iruntime', str(source), '-o', str(executable)], cwd=root, check=True)
    subprocess.run([str(executable)], check=True)
