"""Mutation audit: break one guard at a time and see whether anything notices.

    python tools/mutate.py rackpool
    python tools/mutate.py sharedfxrack
    python tools/mutate.py all

A guard that SURVIVES has no test behind it. That is the finding -- not a
failure, but a gap, and usually in the subtlest condition rather than the
obvious one. Used the other way round it is just as useful: when a mutant
survives because the code it breaks is genuinely unobservable, that says the
line is defensive rather than load-bearing, and a comment claiming otherwise
should be corrected.

Restores the file afterwards, including on a crash, so a interrupted run does
not leave a mutant in the tree.
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
VS = (r"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE"
      r"\CommonExtensions\Microsoft\CMake\CMake\bin")
CMAKE = os.path.join(VS, "cmake.exe")
CTEST = os.path.join(VS, "ctest.exe")

TARGETS = {
    "rackpool": (
        os.path.join(ROOT, "core", "src", "RackPool.cpp"),
        {
            "no epoch wait": (
                "        if (wasLive_[static_cast<size_t>(c)] &&\n"
                "            freeSince_[static_cast<size_t>(c)] + 2 > epoch) continue;",
                "        // MUTANT"),
            "no wasLive exemption": (
                "        if (wasLive_[static_cast<size_t>(c)] &&\n"
                "            freeSince_[static_cast<size_t>(c)] + 2 > epoch) continue;",
                "        if (freeSince_[static_cast<size_t>(c)] + 2 > epoch) continue;"),
            "no voice-in-use check": (
                "        if (pool.rackInUse(racks_[static_cast<size_t>(c)].get())) continue;",
                "        // MUTANT"),
            "no ringing check": (
                "        if (racks_[static_cast<size_t>(c)]->ringing()) continue;",
                "        // MUTANT"),
            "no retire request": (
                "        if (oldest >= 0) retireRequest_.store(oldest, std::memory_order_release);",
                "        // MUTANT"),
            "retire does not reset": (
                "            r->reset();",
                "            // MUTANT"),
        }),
    "sharedfxrack": (
        os.path.join(ROOT, "core", "src", "SharedFxRack.cpp"),
        {
            "no empty-pool guard": (
                "    if (delays_.empty() || verbs_.empty()) return route;",
                "    // MUTANT"),
            "delay cache never hits": (
                "        if (const Entry* e = find(key)) {\n"
                "            route.delaySlot = e->slot;",
                "        if (const Entry* e = find(key); false) {\n"
                "            route.delaySlot = e->slot;"),
            "verb cache never hits": (
                "        if (const Entry* e = find(key)) {\n"
                "            route.verbSlot = e->slot;",
                "        if (const Entry* e = find(key); false) {\n"
                "            route.verbSlot = e->slot;"),
            "no node eviction": (
                "    if (entries_.size() > kMaxFxNodes) evictOldestHalf();",
                "    // MUTANT"),
            "duplicate edges allowed": (
                "    for (const auto& e : edges_)\n"
                "        if (e.delaySlot == delaySlot && e.verbSlot == verbSlot) return;",
                "    // MUTANT"),
            "no dry leg past the delay": (
                "        if (route.verbSlot >= 0)\n"
                "            verbs_[static_cast<size_t>(route.verbSlot)].addInput(l, r);\n"
                "        else { masterL_ += l; masterR_ += r; }",
                "        // MUTANT"),
            # Expected to survive. Without the clamp the counter overflows int
            # after about 33 days of continuous silence and wraps negative, at
            # which point an idle rack claims to be ringing and is never rebuilt
            # again. Correct, free, and not reachable by any test worth writing.
            "silence counter not saturated": (
                "    silentBlocks_.store(silent ? std::min(prev + 1, kSilentBlocksToIdle) : 0,",
                "    silentBlocks_.store(silent ? prev + 1 : 0,"),
        }),
}


def run(target):
    src, mutants = TARGETS[target]
    backup = src + ".mutbak"
    shutil.copyfile(src, backup)
    original = io.open(src, encoding="utf-8").read()
    results = []
    try:
        for name, (old, new) in mutants.items():
            if old not in original:
                results.append((name, "PATTERN NOT FOUND -- update the script"))
                continue
            io.open(src, "w", encoding="utf-8").write(original.replace(old, new, 1))

            build = subprocess.run(
                [CMAKE, "--build", BUILD, "--config", "Release", "--target", "sl_tests"],
                capture_output=True, text=True)
            if build.returncode != 0:
                results.append((name, "did not compile"))
                continue

            test = subprocess.run([CTEST, "--test-dir", BUILD, "-C", "Release"],
                                  capture_output=True, text=True)
            if test.returncode == 0:
                results.append((name, "SURVIVED -- nothing covers this"))
            else:
                caught = [l for l in test.stdout.splitlines() if "***" in l]
                first = caught[0].split(":", 2)[-1].split("...")[0].strip() if caught else "?"
                results.append((name, f"caught ({len(caught)}) e.g. {first}"))
    finally:
        shutil.copyfile(backup, src)
        os.remove(backup)

    print(f"\n{target}:")
    for name, outcome in results:
        print(f"  {name:<32} {outcome}")
    return results


if __name__ == "__main__":
    which = sys.argv[1] if len(sys.argv) > 1 else "all"
    for t in (TARGETS if which == "all" else [which]):
        run(t)
