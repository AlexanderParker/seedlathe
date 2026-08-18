"""Mutation audit of RackPool: break one guard at a time, see what notices."""
import io, subprocess, sys, shutil

SRC = r"C:\dev\seedlathe\core\src\RackPool.cpp"
BAK = SRC + ".mutbak"
CM = r"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
CT = r"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"

MUTANTS = {
    "no epoch wait": (
        "        if (wasLive_[static_cast<size_t>(c)] &&\n"
        "            freeSince_[static_cast<size_t>(c)] + 2 > epoch) continue;",
        "        // MUTANT: epoch wait removed"),
    "no wasLive exemption": (
        "        if (wasLive_[static_cast<size_t>(c)] &&\n"
        "            freeSince_[static_cast<size_t>(c)] + 2 > epoch) continue;",
        "        if (freeSince_[static_cast<size_t>(c)] + 2 > epoch) continue;"),
    "no voice-in-use check": (
        "        if (pool.rackInUse(racks_[static_cast<size_t>(c)].get())) continue;",
        "        // MUTANT: voice check removed"),
    "no ringing check": (
        "        if (racks_[static_cast<size_t>(c)]->ringing()) continue;",
        "        // MUTANT: ringing check removed"),
    "no retire request": (
        "        if (oldest >= 0) retireRequest_.store(oldest, std::memory_order_release);",
        "        // MUTANT: retirement never requested"),
    "retire does not reset": (
        "            r->reset();",
        "            // MUTANT: retired rack keeps ringing"),
}

shutil.copyfile(SRC, BAK)
original = io.open(SRC, encoding="utf-8").read()
results = []

for name, (old, new) in MUTANTS.items():
    if old not in original:
        results.append((name, "PATTERN NOT FOUND"))
        continue
    io.open(SRC, "w", encoding="utf-8").write(original.replace(old, new, 1))

    build = subprocess.run([CM, "--build", r"C:\dev\seedlathe\build",
                            "--config", "Release", "--target", "sl_tests"],
                           capture_output=True, text=True)
    if build.returncode != 0:
        results.append((name, "BUILD FAILED"))
        continue

    run = subprocess.run([CT, "--test-dir", r"C:\dev\seedlathe\build", "-C", "Release"],
                         capture_output=True, text=True)
    failed = [l.strip() for l in run.stdout.splitlines()
              if l.strip().startswith(tuple("0123456789")) and "Failed" in l]
    if run.returncode == 0:
        results.append((name, "SURVIVED - nothing caught it"))
    else:
        names = "; ".join(f.split(" - ", 1)[-1].replace(" (Failed)", "")
                          .replace(" (SEGFAULT)", " [crash]") for f in failed[:3])
        results.append((name, f"caught by: {names}"))

shutil.copyfile(BAK, SRC)
print()
for name, outcome in results:
    print(f"  {name:<26} {outcome}")
